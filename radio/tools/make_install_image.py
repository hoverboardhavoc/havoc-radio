#!/usr/bin/env python3
"""Builds the stock-firmware install image (installer/SPEC.md, "Install image and trampoline").

The image is the flash content from 0x0 that a serial flash of the build outputs
writes, as one flat file the stock updater can stage and eboot can copy to 0x0.
Offsets and flash settings come from radio/build/flash_project_args. The bootloader
at 0x0 is repacked with a trampoline segment so that eboot, which boots the
image header it finds at 0x1000, loads a reset stub instead of bootloader text.

Usage: make_install_image.py --build-dir build --toolchain PREFIX --stub radio/tools/install_stub.s
                             --out radio/build/havoc-radio-install.bin
"""

import argparse
import os
import struct
import subprocess
import sys

SECTOR = 0x1000
MAX_IMAGE = 0x300000          # 3 MB
BOOTLOADER_LIMIT = 0x8000     # the partition table starts here
ESP_MAGIC = 0xE9
CHECKSUM_SEED = 0xEF

# The trampoline segment: loaded by the ROM to unused IRAM above the bootloader's
# text (0x40100000..0x40101630 in the current build).
TRAMPOLINE_ADDR = 0x40104000
# Where eboot's load_app_from_flash_raw(0) reads its image header.
EBOOT_APP_OFFSET = 0x1000
# Where the mini app's segment (the stub) is loaded by eboot.
STUB_ADDR = 0x40100000

# esptool's ESP8266 header encodings (byte 2: mode; byte 3: size << 4 | freq).
FLASH_MODES = {"qio": 0, "qout": 1, "dio": 2, "dout": 3}
FLASH_SIZES = {"512KB": 0x00, "256KB": 0x10, "1MB": 0x20, "2MB": 0x30, "4MB": 0x40,
               "2MB-c1": 0x50, "4MB-c1": 0x60, "8MB": 0x80, "16MB": 0x90}
FLASH_FREQS = {"40m": 0x0, "26m": 0x1, "20m": 0x2, "80m": 0xF}


class BuildError(Exception):
    pass


def round_up(n, align):
    return (n + align - 1) // align * align


# ---- ESP8266 ROM image format (esptool "image version 1") ----

class RomImage:
    def __init__(self, mode, size_freq, entry, segments):
        self.mode = mode
        self.size_freq = size_freq
        self.entry = entry
        self.segments = segments  # list of (load address, bytes)


def parse_rom_image(data):
    """Parses an ESP8266 ROM image. Returns (RomImage, end offset, checksum ok)."""
    if len(data) < 8 or data[0] != ESP_MAGIC:
        raise BuildError("not an ESP8266 ROM image (magic 0x%02x)" % (data[0] if data else 0))
    count, mode, size_freq, entry = data[1], data[2], data[3], struct.unpack_from("<I", data, 4)[0]
    pos = 8
    segments = []
    checksum = CHECKSUM_SEED
    for _ in range(count):
        if pos + 8 > len(data):
            raise BuildError("segment header past the end of the image")
        addr, size = struct.unpack_from("<II", data, pos)
        pos += 8
        if pos + size > len(data):
            raise BuildError("segment at 0x%08x runs past the end of the image" % addr)
        seg = bytes(data[pos:pos + size])
        for b in seg:
            checksum ^= b
        segments.append((addr, seg))
        pos += size
    # Zero padding up to the last byte of a 16-byte block, which holds the checksum.
    cks_pos = round_up(pos + 1, 16) - 1
    if cks_pos >= len(data):
        raise BuildError("checksum byte past the end of the image")
    return RomImage(mode, size_freq, entry, segments), cks_pos + 1, data[cks_pos] == checksum


def build_rom_image(img):
    out = bytearray(struct.pack("<BBBBI", ESP_MAGIC, len(img.segments), img.mode, img.size_freq, img.entry))
    checksum = CHECKSUM_SEED
    for addr, seg in img.segments:
        if len(seg) % 4:
            raise BuildError("segment at 0x%08x is not a multiple of 4 bytes" % addr)
        out += struct.pack("<II", addr, len(seg)) + seg
        for b in seg:
            checksum ^= b
    out += bytes(round_up(len(out) + 1, 16) - 1 - len(out))
    out.append(checksum)
    return bytes(out)


# ---- inputs ----

def parse_flash_args(path):
    """Returns (mode byte, size/freq byte, [(offset, file path)]) from flash_project_args."""
    base = os.path.dirname(path)
    flags = {}
    files = []
    with open(path) as f:
        tokens = f.read().split()
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok.startswith("--"):
            flags[tok[2:]] = tokens[i + 1]
            i += 2
        else:
            files.append((int(tok, 0), os.path.join(base, tokens[i + 1])))
            i += 2
    try:
        mode = FLASH_MODES[flags["flash_mode"]]
        size_freq = FLASH_SIZES[flags["flash_size"]] | FLASH_FREQS[flags["flash_freq"]]
    except KeyError as e:
        raise BuildError("flash_project_args: missing or unknown flash setting %s" % e)
    return mode, size_freq, sorted(files)


def assemble_stub(prefix, src, workdir):
    """Assembles the reset stub. Returns (code bytes padded to 4, entry address)."""
    obj = os.path.join(workdir, "install_stub.o")
    elf = os.path.join(workdir, "install_stub.elf")
    raw = os.path.join(workdir, "install_stub.bin")
    subprocess.run([prefix + "as", "-o", obj, src], check=True)
    subprocess.run([prefix + "ld", "-Ttext=0x%08x" % STUB_ADDR, "-e", "stub_entry", "-o", elf, obj],
                   check=True)
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", elf, raw], check=True)
    nm = subprocess.run([prefix + "nm", elf], check=True, capture_output=True, text=True).stdout
    entry = None
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "stub_entry":
            entry = int(parts[0], 16)
    if entry is None:
        raise BuildError("stub_entry not found in the stub")
    with open(raw, "rb") as f:
        code = f.read()
    code += bytes(round_up(len(code), 4) - len(code))
    return code, entry


# ---- the image ----

def trampoline_segment(stub, stub_entry, mode, size_freq):
    """Segment data that puts a one-segment app image at file offset 0x1000.

    The trampoline is the first segment, so its data starts at file offset 0x10:
    file offset 0x1000 is data offset 0xFF0.
    """
    lead = EBOOT_APP_OFFSET - 0x10
    mini = struct.pack("<BBBBI", ESP_MAGIC, 1, mode, size_freq, stub_entry)
    mini += struct.pack("<II", STUB_ADDR, len(stub)) + stub
    return b"\xff" * lead + mini


def repack_bootloader(original, stub, stub_entry, mode, size_freq):
    img, _, ok = parse_rom_image(original)
    if not ok:
        raise BuildError("bootloader checksum is invalid")
    tramp = trampoline_segment(stub, stub_entry, mode, size_freq)
    t_end = TRAMPOLINE_ADDR + len(tramp)
    if any(addr < t_end and TRAMPOLINE_ADDR < addr + len(seg) for addr, seg in img.segments):
        raise BuildError("trampoline at 0x%08x overlaps a bootloader segment" % TRAMPOLINE_ADDR)
    return build_rom_image(RomImage(mode, size_freq, img.entry, [(TRAMPOLINE_ADDR, tramp)] + img.segments))


def build_install_image(build_dir, prefix, stub_src):
    mode, size_freq, files = parse_flash_args(os.path.join(build_dir, "flash_project_args"))
    if not files or files[0][0] != 0:
        raise BuildError("flash_project_args has no image at 0x0")
    workdir = os.path.join(build_dir, "install")
    os.makedirs(workdir, exist_ok=True)
    stub, entry = assemble_stub(prefix, stub_src, workdir)

    regions = []
    for offset, path in files:
        with open(path, "rb") as f:
            data = f.read()
        if offset == 0:
            data = repack_bootloader(data, stub, entry, mode, size_freq)
            if len(data) >= BOOTLOADER_LIMIT:
                raise BuildError("repacked bootloader is %d bytes, reaches 0x%x" % (len(data), BOOTLOADER_LIMIT))
        regions.append((offset, data, path))

    end = max(off + len(data) for off, data, _ in regions)
    image = bytearray(b"\xff" * round_up(end, SECTOR))
    for i, (off, data, path) in enumerate(regions):
        if i + 1 < len(regions) and off + len(data) > regions[i + 1][0]:
            raise BuildError("%s overlaps the next region" % path)
        image[off:off + len(data)] = data
    if len(image) > MAX_IMAGE:
        raise BuildError("install image is %d bytes, over the 0x%x limit" % (len(image), MAX_IMAGE))
    return bytes(image)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default="radio/build")
    ap.add_argument("--toolchain", required=True, help="tool prefix, e.g. .../bin/xtensa-lx106-elf-")
    ap.add_argument("--stub", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    try:
        image = build_install_image(args.build_dir, args.toolchain, args.stub)
    except (BuildError, OSError, subprocess.CalledProcessError) as e:
        sys.exit("make_install_image: %s" % e)
    with open(args.out, "wb") as f:
        f.write(image)
    print("%s: %d bytes" % (args.out, len(image)))


if __name__ == "__main__":
    main()
