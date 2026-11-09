#!/usr/bin/env python3
"""Update protocol against the host server (installer/SPEC.md, Tests 1 and 2).

Usage: test_protocol.py SERVER_BINARY STAGE_FILE INSTALL_IMAGE

STAGE_FILE is tools/make_stage.py's output for INSTALL_IMAGE.

1. A client that sends nothing is closed after the 1 s command limit.
2. A client that disconnects mid-transfer ends its session; the server takes the next one.
3. The stock-client emulator (stock_client.py) receives the image
   byte for byte and reaches "Update successfully completed"; eboot's copy then
   leaves the image at flash 0x0. A connection made during that session is closed.
4. Refusals: a stage with a region missing, an image over the stock size limit, one
   that does not start with 0xE9, and a plain build (the RTOS bootloader at 0x0 with
   no trampoline, which eboot could not boot) get the command read and the
   connection closed with no header.
"""

import os
import re
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import make_stage  # noqa: E402
import stock_client  # noqa: E402
from hostserver import Checks, HostServer  # noqa: E402

HOST = "127.0.0.1"
WIFI_MAX = 3731456  # US_IMAGE_MAX: 0x3fb000 minus the largest stock app rounded up to 4 KB


def recv_until_idle(sock, idle_s):
    sock.settimeout(idle_s)
    data = b""
    while True:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return data
        if not chunk:
            return data
        data += chunk


def closed_by_peer(sock, timeout_s):
    sock.settimeout(timeout_s)
    try:
        return sock.recv(1) == b""
    except ConnectionResetError:
        return True
    except socket.timeout:
        return False


def refused(check, binary, stage, what):
    """A session on a server holding stage gets no header and is closed."""
    path = os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), "refused-stage.bin")
    with open(path, "wb") as f:
        f.write(stage)
    srv = HostServer(binary, path)
    try:
        s = socket.create_connection((HOST, srv.update_port), timeout=5)
        s.sendall(b"update")
        got = recv_until_idle(s, 2)
        s.close()
        check(got == b"", "%s: no header (%r)" % (what, got[:20]))
        line = srv.wait_for("session end", 2)
        check(line is not None and "refused: no image" in line, "%s: log %s" % (what, line))
    finally:
        srv.stop()
        os.remove(path)


def plain_build(image_path, image):
    """The install image with the build's own bootloader at 0x0 in place of the repacked one."""
    with open(os.path.join(os.path.dirname(os.path.abspath(image_path)), "bootloader", "bootloader.bin"), "rb") as f:
        boot = f.read()
    plain = boot + b"\xff" * (0x8000 - len(boot)) + image[0x8000:]
    assert plain[0] == 0xE9 and len(plain) == len(image)
    return plain


def missing_region(stage):
    """The stage with the present flag of its last data region cleared, CRC fixed."""
    label, _, regions = make_stage.decode_header(stage[:make_stage.HEADER_SIZE])
    last = max(i for i, r in enumerate(regions) if not r["flags"] & make_stage.FLAG_ERASE)
    regions[last]["flags"] = 0
    return make_stage.encode_header(label, regions) + stage[make_stage.HEADER_SIZE:]


def main():
    binary, stage_path, image_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(image_path, "rb") as f:
        image = f.read()
    with open(stage_path, "rb") as f:
        stage = f.read()
    check = Checks("protocol")
    srv = HostServer(binary, stage_path)
    try:
        # 1. No command.
        mark = srv.mark()
        s = socket.create_connection((HOST, srv.update_port), timeout=5)
        t0 = time.monotonic()
        closed = closed_by_peer(s, 3)
        elapsed = time.monotonic() - t0
        s.close()
        check(closed and 0.9 <= elapsed < 2.0, "silent client closed after the 1 s limit (%.2f s)" % elapsed)
        check(srv.wait_for("no update command", 2, mark) is not None, "log: no update command")

        # 2. Disconnect mid-transfer.
        mark = srv.mark()
        s = socket.create_connection((HOST, srv.update_port), timeout=5)
        s.sendall(b"update")
        header = recv_until_idle(s, 0.5)
        check(header == b"999,%d" % len(image), "header %r" % header)
        check(re.fullmatch(rb"[^,\r\n]+,[0-9]+", header) is not None,
              "header in the vendor's <ver>,<size> format with no terminator")
        s.sendall(b"5C:CF:7F:AA:BB:CC")
        got = 0
        s.settimeout(5)
        while got < 64 * 1024:
            chunk = s.recv(4096)
            if not chunk:
                break
            got += len(chunk)
        s.close()
        check(got >= 64 * 1024, "aborting client received %d bytes before closing" % got)
        line = srv.wait_for("session end", 5, mark)
        check(line is not None and "peer closed early" in line and "5C:CF:7F:AA:BB:CC" in line,
              "log: aborted session with its MAC: %s" % line)

        # 3. Stock client, with a second connection during its session.
        mark = srv.mark()
        result = {}
        mac = "5C:CF:7F:01:02:03"
        th = threading.Thread(target=lambda: result.update(r=stock_client.ota_update(HOST, srv.update_port, mac=mac)))
        th.start()
        check(srv.wait_for("update: session from", 5, mark) is not None, "stock client session started")
        s2 = socket.create_connection((HOST, srv.update_port), timeout=5)
        check(closed_by_peer(s2, 2), "connection during a session is closed")
        s2.close()
        check(srv.wait_for("during a session, closed", 2, mark) is not None, "log: second connection closed")
        th.join(90)
        r = result.get("r")
        check(r is not None, "stock client finished")
        if r is not None:
            check(r.restarted, "stock client: %s" % r.reason)
            check(r.line == "999,%d" % len(image), "stock client read header %r" % r.line)
            check(r.fsize == len(image) and r.written == len(image),
                  "stock client wrote %s of %s bytes" % (r.written, r.fsize))
            check(stock_client.staged(r) == image, "staged image byte-identical")
            check(bytes(r.flash[:len(image)]) == image, "flash from 0x0 after the eboot copy is the image")
            line = srv.wait_for("session end", 25, mark)
            check(line is not None and "sent every byte" in line and mac in line
                  and ("%d of %d" % (len(image), len(image))) in line, "log: %s" % line)
            extra = " (header wait %d ms, %d empty polls before the last byte)" % (r.header_wait_ms,
                                                                                  r.waits_at_last_byte or 0)
        else:
            extra = ""
    finally:
        srv.stop()

    # 4. Refusals.
    refused(check, binary, missing_region(stage), "a region missing")
    # One byte over the limit; 0xFF sectors are erase-only regions, so it stages small.
    big = b"\xe9" + os.urandom(4095) + b"\xff" * (WIFI_MAX + 1 - 4096)
    refused(check, binary, make_stage.build_stage(big, "big"), "over the size limit")
    refused(check, binary, make_stage.build_stage(b"\x00" + image[1:], "no magic"), "first byte not 0xE9")
    refused(check, binary, make_stage.build_stage(plain_build(image_path, image), "plain"), "a plain build")
    return check.report(extra)


if __name__ == "__main__":
    sys.exit(main())
