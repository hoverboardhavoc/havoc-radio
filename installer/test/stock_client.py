"""Stock-client emulator: the K-Radio firmware's update client as a script. The
Arduino core 3.0.2 behaviour it relies on is modelled with it:

- WiFiClient sets Stream::_timeout to 5000 ms (WiFiClient.cpp), so
  readStringUntil() waits 5 s for a character that never comes.
- WiFiClient::read(buf, n) returns min(n, available) without waiting.
- UpdaterClass begin/write/_writeBuffer/end/_verifyEnd (Updater.cpp) on a
  4 MB flash with the eagle.flash.4m.ld layout (_FS_start 0x3FB000).
- eboot's copy_raw() on the restart (eboot.c), so the caller can check the flash.

`waits` is never reset during an update: the 5000 x 2 ms limit counts every
empty poll of the whole transfer, not 10 s of silence.
"""

import socket
import time

FLASH_SIZE = 4 << 20
FS_START = 0x3FB000
SECTOR = 0x1000
WIFICLIENT_TIMEOUT_MS = 5000
STOCK_EBOOT_HEADER = bytes([0xE9, 0x02, 0x02, 0x40])  # flash 0x0 of the stock radio: DIO, 4 MB, 40 MHz


def millis():
    return int(time.monotonic() * 1000)


def delay(ms):
    time.sleep(ms / 1000.0)


def to_int(s):
    """String::toInt() is atol()."""
    s = s.lstrip(" \t\n\r\f\v")
    sign = 1
    if s[:1] in ("+", "-"):
        sign = -1 if s[0] == "-" else 1
        s = s[1:]
    digits = ""
    for ch in s:
        if not ch.isdigit():
            break
        digits += ch
    return sign * int(digits) if digits else 0


def substring(s, left, right):
    """String::substring(unsigned left, unsigned right): -1 becomes UINT_MAX."""
    left &= 0xFFFFFFFF
    right &= 0xFFFFFFFF
    if left > right:
        left, right = right, left
    if left >= len(s):
        return ""
    return s[left:min(right, len(s))]


def magic_flash_chip_size(nibble):
    return {0x0: 512 << 10, 0x1: 256 << 10, 0x2: 1 << 20, 0x3: 2 << 20, 0x4: 4 << 20,
            0x8: 8 << 20, 0x9: 16 << 20}.get(nibble & 0xF, 0)


def magic_flash_chip_mode(byte):
    return byte if byte <= 3 else 0xFF


class WiFiClient:
    def __init__(self):
        self.sock = None
        self.rx = bytearray()
        self._timeout = WIFICLIENT_TIMEOUT_MS

    def connect(self, host, port):
        try:
            self.sock = socket.create_connection((host, port), timeout=self._timeout / 1000.0)
        except OSError:
            return False
        self.sock.setblocking(False)
        return True

    def _pull(self):
        while True:
            try:
                data = self.sock.recv(65536)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return
            if not data:
                return
            self.rx += data

    def available(self):
        self._pull()
        return len(self.rx)

    def read_byte(self):
        if not self.available():
            return -1
        c = self.rx[0]
        del self.rx[0]
        return c

    def read(self, size):
        self._pull()
        n = min(size, len(self.rx))
        data = bytes(self.rx[:n])
        del self.rx[:n]
        return data

    def print(self, s):
        try:
            self.sock.sendall(s.encode())
        except OSError:
            pass

    def stop(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def timed_read(self):
        start = millis()
        while True:
            c = self.read_byte()
            if c >= 0:
                return c
            time.sleep(0)  # yield()
            if not millis() - start < self._timeout:
                return -1

    def read_string_until(self, terminator):
        ret = ""
        c = self.timed_read()
        while c >= 0 and c != ord(terminator):
            ret += chr(c)
            c = self.timed_read()
        return ret


class Updater:
    """UpdaterClass for U_FLASH, core 3.0.2, without signing or MD5."""

    def __init__(self, flash, sketch_size):
        self.flash = flash
        self.sketch_size = sketch_size
        self._size = 0
        self._start = 0
        self._cur = 0
        self._buf = bytearray()
        self._buffer_size = SECTOR
        self.error = 0
        self.eboot_command = None

    def progress(self):
        return self._cur - self._start

    def remaining(self):
        return self._size - self.progress()

    def is_finished(self):
        return self._cur == self._start + self._size

    def begin(self, size):
        if self._size > 0:
            return False
        if size == 0:
            self.error = 1
            return False
        size &= 0xFFFFFFFF  # size_t
        current = (self.sketch_size + SECTOR - 1) & ~(SECTOR - 1)
        rounded = (size + SECTOR - 1) & ~(SECTOR - 1)
        end = FS_START
        start = end - rounded if end > rounded else 0
        if start < current:
            self.error = 2  # UPDATE_ERROR_SPACE
            return False
        self._start = self._cur = start
        self._size = size
        self._buf = bytearray()
        return True

    def _write_buffer(self):
        buf = bytearray(self._buf)
        if self._cur == self._start and buf[0] != 0x1F:
            flash_mode = magic_flash_chip_mode(self.flash[2])
            buffer_mode = magic_flash_chip_mode(buf[2])
            if buffer_mode != flash_mode:
                buf[2] = flash_mode  # restored in _buffer afterwards, written to flash modified
        self.flash[self._cur:self._cur + len(buf)] = buf
        self._cur += len(buf)
        self._buf = bytearray()
        return True

    def write(self, data):
        if self.error or self._size == 0:
            return 0
        if self.progress() + len(self._buf) + len(data) > self._size:
            self.error = 2
            return 0
        left = len(data)
        while len(self._buf) + left > self._buffer_size:
            to_buff = self._buffer_size - len(self._buf)
            self._buf += data[len(data) - left:len(data) - left + to_buff]
            self._write_buffer()
            left -= to_buff
        self._buf += data[len(data) - left:]
        if len(self._buf) == self.remaining():
            self._write_buffer()
        return len(data)

    def end(self, even_if_remaining=False):
        if self._size == 0:
            return False
        if not self.progress():
            self.error = 6  # UPDATE_ERROR_NO_DATA
        if self.error or (not self.is_finished() and not even_if_remaining):
            self._size = 0
            return False
        head = self.flash[self._start:self._start + 4]
        if not (head[0] == 0x1F and head[1] == 0x8B):
            if head[0] != 0xE9:
                self.error = 10  # UPDATE_ERROR_MAGIC_BYTE
                return False
            if magic_flash_chip_size(head[3] >> 4) > FLASH_SIZE:
                self.error = 9  # UPDATE_ERROR_NEW_FLASH_CONFIG
                return False
        self.eboot_command = (self._start, 0x0, self._size)  # ACTION_COPY_RAW
        self._size = 0
        return True


def eboot_copy_raw(flash, src, dst, size):
    """copy_raw(src, dst, size, false) of eboot.c for an uncompressed image. Returns its result."""
    if src & 0xFFF or dst & 0xFFF:
        return 1
    if flash[src] == 0x1F and flash[src + 1] == 0x8B:
        return 5  # gzip is not emulated
    left = (size + SECTOR - 1) & ~(SECTOR - 1)
    while left > 0:
        flash[dst:dst + SECTOR] = flash[src:src + SECTOR]
        src += SECTOR
        dst += SECTOR
        left -= SECTOR
    return 0


class Result:
    def __init__(self):
        self.restarted = False      # "Update successfully completed. Rebooting."
        self.reason = ""
        self.line = None            # header as read by readStringUntil('\r')
        self.fsize = None
        self.written = 0
        self.waits = 0
        self.flash = None
        self.eboot_command = None
        self.header_wait_ms = None  # time spent in readStringUntil
        self.waits_at_last_byte = None  # empty polls used before the last byte arrived


def ota_update(host, port, version="102", mac="5C:CF:7F:01:02:03", sketch_size=440208, on_mac_sent=None):
    """One update as the stock firmware runs it, sending its version and its WiFi MAC address."""
    r = Result()
    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[0:4] = STOCK_EBOOT_HEADER
    update = Updater(flash, sketch_size)
    r.flash = flash

    size = ""
    ver = ""
    line = ""
    client = WiFiClient()
    if not client.connect(host, port):
        r.reason = "connection update failed"
        return r
    client.print("update")
    t = 0
    while not client.available():
        delay(1)
        t += 1
        if t > 2000:
            r.reason = "no reply to update"
            client.stop()
            return r
    if client.available() > 0:
        t0 = millis()
        line = client.read_string_until("\r")
        r.header_wait_ms = millis() - t0
        r.line = line
        idx = line.find(",")
        if idx != -1:
            ver = substring(line, 0, idx)
            line = substring(line, idx + 1, len(line))
            size = substring(line, 0, line.find(","))
        if ver == version:
            client.stop()
            r.reason = "same version"
            return r
        fsize = to_int(size)
        r.fsize = fsize
        if not update.begin(fsize):
            r.reason = "OTAerror!"
            client.stop()
            return r
        client.print(mac)
        if on_mac_sent:
            on_mac_sent()
        t = 0
        while not client.available():
            delay(1)
            t += 1
            if t > 2000:
                r.reason = "no data after MAC"
                client.stop()
                return r
        waits = 0
        written = 0
        while True:
            avail = client.available()
            if avail > 0:
                buf = client.read(1024)
                n = len(buf)
                if n != -1:
                    if update.write(buf) == n:
                        written += n
                        if written == fsize and r.waits_at_last_byte is None:  # instrumentation only
                            r.waits_at_last_byte = waits
            else:
                waits += 1
                delay(2)
                if waits > 5000:
                    update.end(False)
                    r.written = written
                    r.waits = waits
                    r.eboot_command = update.eboot_command
                    if written == fsize:
                        r.restarted = True  # ESP.restart(): the connection is dropped, eboot runs
                        r.reason = "Update successfully completed. Rebooting."
                        client.stop()
                        if update.eboot_command:
                            src, dst, length = update.eboot_command
                            eboot_copy_raw(flash, src, dst, length)
                        return r
                    r.reason = "OTAtimeout!"
                    client.stop()
                    return r
    client.stop()
    return r


def staged(result):
    """The bytes the updater wrote to the staging area."""
    if not result.eboot_command:
        return b""
    src, _, length = result.eboot_command
    return bytes(result.flash[src:src + length])

