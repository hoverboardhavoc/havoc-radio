#!/usr/bin/env python3
"""Checks radio/build/havoc-radio-install.bin (installer/SPEC.md, "Install image and trampoline").

- regions equal the build outputs at the flash_project_args offsets, gaps are 0xFF;
- the repacked bootloader parses in ROM format with a valid checksum (also checked
  with esptool), keeps the original segments, entry and flash settings, and ends
  below 0x8000;
- eboot's load_app_from_flash_raw(0) (core 3.0.2) loads only the reset stub;
- the stub disassembles to the system_restart_core() sequence: Cache_Read_Disable(), the DPORT icache bits
  cleared, INTENABLE and LITBASE 0, rsync, a jump to the ROM reset vector;
- the partition table and the regions follow radio/partitions.csv: the app at ota_0, 0x100000, linked for offset 0
  of its 1 MB flash window; the starting guide, when the build fetched one, at 0x10000; the rest of the first window
  is 0xFF and the installer stages it as erase-only;
- the stock updater (core 3.0.2 Updater) accepts the image and stages it clear of the copy to 0x0;
- the builder rejects an oversized image and an oversized bootloader.
"""

import os
import re
import struct
import subprocess
import sys

RADIO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # radio/
ROOT = os.path.dirname(RADIO)                                        # repository root
sys.path.insert(0, os.path.join(RADIO, "tools"))
import make_install_image as mii  # noqa: E402

BUILD = os.path.join(RADIO, "build")
IMAGE = os.path.join(BUILD, "havoc-radio-install.bin")
PREFIX = os.path.join(RADIO, "third_party", "xtensa-lx106-elf", "bin", "xtensa-lx106-elf-")
ESPTOOL = os.path.join(RADIO, "third_party", "ESP8266_RTOS_SDK", "components", "esptool_py", "esptool",
                       "esptool.py")
WORK = os.path.join(BUILD, "install", "test")

# Stock facts (installer/SPEC.md, "Stock update path").
STOCK_FS_START = 0x3FB000          # _FS_start of eagle.flash.4m.ld, minus 0x40200000
LARGEST_STOCK_APP = 440208         # bytes, vendor image version 108
STOCK_RUNNING_MODE = 2             # DIO: byte 2 of the stock eboot header at 0x0 (e9 02 02 40)
REAL_FLASH_SIZE = 4 << 20

# radio/partitions.csv: (name, type, subtype, offset, size).
PARTITIONS = [("nvs", 1, 0x02, 0x9000, 0x4000), ("otadata", 1, 0x00, 0xD000, 0x2000),
              ("phy_init", 1, 0x01, 0xF000, 0x1000), ("guide", 1, 0x40, 0x10000, 0xF0000),
              ("ota_0", 0, 0x10, 0x100000, 0x100000),
              ("ota_1", 0, 0x11, 0x200000, 0x100000)]
REGIONS = [0x0, 0x8000, 0xD000, 0x100000]  # bootloader, partition table, OTA data, app
GUIDE_OFFSET = 0x10000             # the starting frequency guide, when the build fetched one
APP_OFFSET = 0x100000
FLASH_WINDOW = 0x100000            # the ESP8266 maps one 1 MB window of flash at 0x40200000
FLASH_MAP = 0x40200000
# The installer's stage partition (installer/partitions.csv) and its region split.
sys.path.insert(0, os.path.join(ROOT, "installer", "tools"))
import make_stage  # noqa: E402

checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL: " + what)


def magic_flash_chip_size(nibble):
    """EspClass::magicFlashChipSize, core 3.0.2."""
    return {0x0: 512 << 10, 0x1: 256 << 10, 0x2: 1 << 20, 0x3: 2 << 20, 0x4: 4 << 20,
            0x8: 8 << 20, 0x9: 16 << 20}.get(nibble & 0xF, 0)


def eboot_load_app_from_flash_raw(flash, flash_addr=0):
    """Follows load_app_from_flash_raw() of eboot.c (core 3.0.2).

    Returns (loaded segments as [(address, bytes)], entry). The header's magic byte
    is not checked, as in eboot.
    """
    pos = flash_addr + 0x1000  # APP_START_OFFSET
    _magic, num_segments, _mode, _size, entry = struct.unpack_from("<BBBBI", flash, pos)
    pos += 8
    loaded = []
    for _ in range(num_segments):
        address, size = struct.unpack_from("<II", flash, pos)
        pos += 8
        load = (address < 0x40000000 or 0x40100000 <= address < 0x4010C000 or address >= 0x60000000)
        if load:
            loaded.append((address, bytes(flash[pos:pos + size])))
        pos += size
    return loaded, entry


def disassemble(code, base, start):
    """objdump of raw Xtensa bytes placed at base, from start. Returns [(addr, mnemonic, operands)]."""
    raw = os.path.join(WORK, "stub_loaded.bin")
    obj = os.path.join(WORK, "stub_loaded.o")
    with open(raw, "wb") as f:
        f.write(code)
    subprocess.run([PREFIX + "objcopy", "-I", "binary", "-O", "elf32-xtensa-le", "-B", "xtensa",
                    "--rename-section", ".data=.text,code,alloc,load,contents", raw, obj], check=True)
    out = subprocess.run([PREFIX + "objdump", "-d", "--adjust-vma=0x%x" % base,
                          "--start-address=0x%x" % start, obj],
                         check=True, capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)$", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return insns


def check_stub(code, base, entry):
    """The stub restarts as system_restart_core() does: Cache_Read_Disable(), clear DPORT 0x3ff00024 bits 3 and 4,
    INTENABLE and LITBASE 0, rsync, then a jump to the ROM reset vector."""
    insns = disassemble(code, base, entry)
    names = [i[1] for i in insns]
    want = ["l32r", "callx0", "l32r", "l32r", "memw", "l32i.n", "and", "s32i.n", "memw", "movi.n",
            "wsr.intenable", "wsr.litbase", "rsync", "l32r", "jx"]
    check(names == want, "stub instructions: %s" % names)
    if names != want:
        return

    def literal(op):
        m = re.match(r"(a\d+), ([0-9a-f]+)", op)
        addr = int(m.group(2), 16)
        return m.group(1), struct.unpack_from("<I", code, addr - base)[0]

    def ops(i):
        return [x.strip() for x in insns[i][2].split(",")]

    reg, val = literal(insns[0][2])
    check(val == 0x400047F0 and ops(1) == [reg], "stub calls Cache_Read_Disable at 0x400047f0: %s 0x%x, callx0 %s"
          % (reg, val, insns[1][2]))
    dport_reg, dport = literal(insns[2][2])
    mask_reg, mask = literal(insns[3][2])
    check(dport == 0x3FF00024, "stub reads DPORT 0x3ff00024: 0x%x" % dport)
    check(mask == 0xFFFFFFE7, "stub clears bits 3 and 4 with 0xffffffe7: 0x%x" % mask)
    load = ops(5)
    check(load[1:] == [dport_reg, "0"], "stub loads from 0x3ff00024: " + insns[5][2])
    val_reg = load[0]
    check(sorted(ops(6)[1:]) == sorted([val_reg, mask_reg]) and ops(6)[0] == val_reg,
          "stub ANDs with the mask: " + insns[6][2])
    check(ops(7) == [val_reg, dport_reg, "0"], "stub stores back to 0x3ff00024: " + insns[7][2])
    zero = ops(9)
    check(zero[1] == "0", "stub sets a register to 0: " + insns[9][2])
    check(insns[10][2] == zero[0] and insns[11][2] == zero[0], "INTENABLE and LITBASE set to 0: %s %s"
          % (insns[10][2], insns[11][2]))
    jreg, vector = literal(insns[13][2])
    check(vector == 0x40000080 and insns[14][2] == jreg, "stub jumps to the ROM reset vector 0x40000080: 0x%x, jx %s"
          % (vector, insns[14][2]))
    check(all(base <= i[0] < base + len(code) for i in insns), "stub instructions inside the segment")


def check_builder_limits(stub_src):
    """The builder fails on an image over 3 MB and on a bootloader reaching 0x8000."""
    with open(os.path.join(BUILD, "bootloader", "bootloader.bin"), "rb") as f:
        boot = f.read()
    img, _, _ = mii.parse_rom_image(boot)

    def build(name, bootloader, app_len):
        d = os.path.join(WORK, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "boot.bin"), "wb") as f:
            f.write(bootloader)
        with open(os.path.join(d, "app.bin"), "wb") as f:
            f.write(b"\xe9" + bytes(app_len - 1))
        with open(os.path.join(d, "flash_project_args"), "w") as f:
            f.write("--flash_mode dio --flash_freq 40m --flash_size 4MB\n0x0 boot.bin\n0x%x app.bin\n" % APP_OFFSET)
        try:
            mii.build_install_image(d, PREFIX, stub_src)
            return None
        except mii.BuildError as e:
            return str(e)

    check(build("ok", boot, 0x300000 - APP_OFFSET) is None, "builder accepts an image of exactly 3 MB")
    err = build("big", boot, 0x300000 - APP_OFFSET + 1)
    check(err is not None and "limit" in err, "builder rejects an image over 3 MB: %s" % err)
    big_boot = mii.build_rom_image(mii.RomImage(img.mode, img.size_freq, img.entry,
                                                img.segments + [(0x3FFF0000, bytes(0x6000))]))
    err = build("bigboot", big_boot, 0x1000)
    check(err is not None and "0x8000" in err, "builder rejects a bootloader reaching 0x8000: %s" % err)


def read_partition_table(path):
    """[(name, type, subtype, offset, size)] of an ESP partition table binary (32-byte entries, magic aa 50)."""
    with open(path, "rb") as f:
        data = f.read()
    parts = []
    for pos in range(0, len(data) - 31, 32):
        magic, ptype, subtype, offset, size, label = struct.unpack_from("<HBBII16s", data, pos)
        if magic != 0x50AA:
            break
        parts.append((label.rstrip(b"\0").decode(), ptype, subtype, offset, size))
    return parts


def check_layout(image, files):
    """The partition table, the region offsets, the app's link offset and the free first window."""
    parts = read_partition_table(os.path.join(BUILD, "partition_table", "partition-table.bin"))
    check(parts == PARTITIONS, "partition table as radio/partitions.csv: %s" % parts)
    offsets = [off for off, _ in files]
    check(offsets in (REGIONS, sorted(REGIONS + [GUIDE_OFFSET])),
          "regions at %s" % ", ".join("0x%x" % off for off in offsets))
    app = dict(files).get(APP_OFFSET)
    if app is None:
        return
    with open(app, "rb") as f:
        app = f.read()
    check(len(app) <= min(p[4] for p in PARTITIONS if p[1] == 0), "app %d B fits both slots" % len(app))
    # The first segment is the code mapped from flash: its data at file offset 0x10 runs at 0x40200010 only
    # when the app sits at offset 0 of its window.
    seg_addr = struct.unpack_from("<I", app, 8)[0]
    check(APP_OFFSET % FLASH_WINDOW == 0 and seg_addr == FLASH_MAP + 0x10,
          "app linked for offset 0 of its flash window: first segment at 0x%08x" % seg_addr)
    guide = dict(files).get(GUIDE_OFFSET)
    first_free = mii.round_up(GUIDE_OFFSET + (os.path.getsize(guide) if guide else 0), 4096)
    check(all(b == 0xFF for b in image[first_free:APP_OFFSET]),
          "the guide partition after its starting table, 0x%x..0x%x, is 0xFF" % (first_free, APP_OFFSET))

    # The installer's split: blank sectors are erase-only, the staged data fits the stage partition.
    regions = make_stage.radio_flash().split_regions(0, image)
    gap = [r for r in regions if r["target"] <= first_free < r["target"] + r["length"]]
    check(len(gap) == 1 and gap[0]["erase"] and gap[0]["target"] + gap[0]["length"] == APP_OFFSET,
          "installer: the rest of the first window is in one erase-only region ending at 0x%x" % APP_OFFSET)
    staged = sum(r["length"] for r in regions if not r["erase"])
    stage_size = make_stage.stage_partition()[1] - make_stage.HEADER_SIZE
    check(staged <= stage_size, "installer stages %d B of data regions, the stage holds %d B" % (staged, stage_size))
    check(sum(r["length"] for r in regions) == len(image), "installer regions cover the image")


def main():
    os.makedirs(WORK, exist_ok=True)
    with open(IMAGE, "rb") as f:
        image = f.read()
    mode, size_freq, files = mii.parse_flash_args(os.path.join(BUILD, "flash_project_args"))

    # Regions and gaps.
    covered = bytearray(len(image))
    boot_end = 0
    for offset, path in files:
        with open(path, "rb") as f:
            data = f.read()
        name = os.path.relpath(path, BUILD)
        if offset == 0:
            rom, boot_end, ok = mii.parse_rom_image(image)
            covered[0:boot_end] = b"\x01" * boot_end
            orig, _, orig_ok = mii.parse_rom_image(data)
            check(ok, "repacked bootloader checksum valid")
            check(orig_ok, "original bootloader checksum valid")
            check(rom.segments[1:] == orig.segments, "original segments byte-identical and in order")
            check(rom.segments[0][0] == mii.TRAMPOLINE_ADDR, "trampoline segment first, at 0x40104000")
            check(rom.entry == orig.entry, "bootloader entry unchanged")
            check(image[2] == mode and image[3] == size_freq,
                  "bootloader header bytes 2, 3 = %02x %02x (flash_project_args %02x %02x)"
                  % (image[2], image[3], mode, size_freq))
            check(boot_end < 0x8000, "repacked bootloader ends at 0x%x, below 0x8000" % boot_end)
            check(all(len(s) % 4 == 0 for _, s in rom.segments), "segment lengths are multiples of 4")
            tramp = rom.segments[0][1]
            check(all(b == 0xFF for b in tramp[:0xFF0]), "trampoline data before file offset 0x1000 is 0xFF")
            check(image[0x1000:0x1000 + len(tramp) - 0xFF0] == tramp[0xFF0:],
                  "trampoline data offset 0xFF0 is file offset 0x1000")
            info = subprocess.run([sys.executable, ESPTOOL, "--chip", "esp8266", "image_info",
                                   _write(os.path.join(WORK, "bootloader-repacked.bin"), image[:boot_end])],
                                  capture_output=True, text=True).stdout
            check("(valid)" in info and "4 segments" in info, "esptool image_info: valid checksum, 4 segments")
        else:
            check(image[offset:offset + len(data)] == data, "%s at 0x%x byte-identical" % (name, offset))
            covered[offset:offset + len(data)] = b"\x01" * len(data)
    gaps_ff = all(image[i] == 0xFF for i in range(len(image)) if not covered[i])
    check(gaps_ff, "gaps are 0xFF")
    last = max(off + os.path.getsize(p) for off, p in files)
    check(len(image) % 4096 == 0 and len(image) - last < 4096, "length padded to a multiple of 4096")
    check(len(image) <= 0x300000, "image within 3 MB")
    check_layout(image, files)

    # eboot after the copy: the flash from 0x0 is the image.
    loaded, entry = eboot_load_app_from_flash_raw(image)
    check(len(loaded) == 1, "eboot loads exactly one segment (got %d)" % len(loaded))
    if loaded:
        addr, code = loaded[0]
        check(addr == mii.STUB_ADDR, "stub segment at 0x%08x" % addr)
        check(addr <= entry < addr + len(code), "entry 0x%08x inside the stub segment" % entry)
        check(all(a < 0x60000000 for a, _ in loaded), "no segment in the peripheral range")
        check_stub(code, addr, entry)

    # The same eboot walk over a flat image with the unmodified bootloader does not.
    with open(os.path.join(BUILD, "bootloader", "bootloader.bin"), "rb") as f:
        plain = f.read()
    plain_loaded, _ = eboot_load_app_from_flash_raw(plain + b"\xff" * 0x1000)
    check(any(not (0x3FFE8000 <= a < 0x40000000 or 0x40100000 <= a < 0x4010C000) for a, _ in plain_loaded),
          "unmodified bootloader: eboot would load outside RAM")

    # Stock updater (core 3.0.2): begin(size), write mode rewrite, _verifyEnd, eboot copy.
    sketch = mii.round_up(LARGEST_STOCK_APP, 4096)
    rounded = mii.round_up(len(image), 4096)
    check(STOCK_FS_START - rounded >= sketch,
          "Update.begin: %d B fits beside a %d B stock app (limit %d B)"
          % (len(image), LARGEST_STOCK_APP, STOCK_FS_START - sketch))
    staged_at = STOCK_FS_START - rounded
    check(staged_at >= rounded, "staged at 0x%x, clear of eboot's copy to 0x0..0x%x" % (staged_at, rounded))
    check(image[0] == 0xE9 and image[:2] != b"\x1f\x8b", "byte 0 is 0xE9 (not gzip)")
    size = magic_flash_chip_size(image[3] >> 4)
    check(0 < size <= REAL_FLASH_SIZE, "flash-size nibble %x = %d B, within the chip" % (image[3] >> 4, size))
    check(image[2] == STOCK_RUNNING_MODE, "flash mode DIO, so the updater leaves byte 2 as is")
    check(len(image) % 4096 == 0, "length a multiple of 4096 (eboot copies whole sectors)")

    check_builder_limits(os.path.join(RADIO, "tools", "install_stub.s"))

    print("install image: %d checks, %d failures (%d B, bootloader %d B)" % (checks, failures, len(image), boot_end))
    sys.exit(1 if failures else 0)


def _write(path, data):
    with open(path, "wb") as f:
        f.write(data)
    return path


if __name__ == "__main__":
    main()
