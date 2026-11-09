#!/usr/bin/env python3
"""Builds the stage partition contents of the Havoc Radio Installer (installer/SPEC.md, "Stage header").

The image, written at target offset 0x0, is split into regions the way tools/radio-flash splits it. The output is
the header sector followed by the data regions at their stage offsets, every region present.

The stage partition's offset and size come from installer/partitions.csv; the build reads them from here.

Usage: make_stage.py --image FILE --label TEXT --out FILE
       make_stage.py --offset    (prints the stage partition's flash offset)
       make_stage.py --size      (prints the stage partition's size)
"""

import argparse
import importlib.machinery
import importlib.util
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

SECTOR = 0x1000
PARTITIONS = os.path.join(os.path.dirname(HERE), "partitions.csv")
HEADER_SIZE = SECTOR
MAGIC = b"HVRI"
VERSION = 1
MAX_REGIONS = 64
LABEL_MAX = 32
REGIONS_AT = 48
REGION_SIZE = 48
CRC_AT = REGIONS_AT + MAX_REGIONS * REGION_SIZE   # 3120
FLAG_ERASE = 0x1
FLAG_PRESENT = 0x2


class StageError(Exception):
    pass


def radio_flash():
    """tools/radio-flash as a module: its region split is the one every uploader uses."""
    path = os.path.join(HERE, "radio-flash")
    loader = importlib.machinery.SourceFileLoader("radio_flash", path)
    spec = importlib.util.spec_from_loader("radio_flash", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def stage_partition(path=PARTITIONS):
    """(offset, size) of the stage partition in an ESP-IDF partition CSV."""
    with open(path) as f:
        for line in f:
            fields = [x.strip() for x in line.split("#")[0].split(",")]
            if fields[0] == "stage":
                return int(fields[3], 0), int(fields[4], 0)
    raise StageError("%s: no stage partition" % path)


def cut_label(label):
    """The label as at most LABEL_MAX bytes of UTF-8, cut on a character boundary."""
    raw = label.encode()
    if len(raw) <= LABEL_MAX:
        return raw
    return raw[:LABEL_MAX].decode(errors="ignore").encode()


def encode_header(label, regions):
    """The header sector for regions (dicts with target, length, stage, flags, sha256 as bytes)."""
    if not 0 < len(regions) <= MAX_REGIONS:
        raise StageError("%d regions, at most %d" % (len(regions), MAX_REGIONS))
    out = bytearray(CRC_AT)
    total = max(r["target"] + r["length"] for r in regions)
    struct.pack_into("<4sIII", out, 0, MAGIC, VERSION, len(regions), total)
    out[16:16 + LABEL_MAX] = cut_label(label).ljust(LABEL_MAX, b"\0")
    for i, r in enumerate(regions):
        struct.pack_into("<IIII32s", out, REGIONS_AT + i * REGION_SIZE, r["target"], r["length"], r["stage"],
                         r["flags"], r["sha256"])
    out += struct.pack("<I", zlib.crc32(bytes(out)))
    return bytes(out) + b"\xff" * (HEADER_SIZE - len(out))


def decode_header(sector):
    """(label, total length, regions) of a header sector; raises StageError when it is not valid."""
    if len(sector) < CRC_AT + 4:
        raise StageError("header too short")
    magic, version, count, total = struct.unpack_from("<4sIII", sector, 0)
    if magic != MAGIC or version != VERSION:
        raise StageError("bad magic or version")
    if not 0 < count <= MAX_REGIONS:
        raise StageError("bad region count %d" % count)
    if struct.unpack_from("<I", sector, CRC_AT)[0] != zlib.crc32(sector[:CRC_AT]):
        raise StageError("bad CRC")
    label = sector[16:16 + LABEL_MAX].split(b"\0")[0].decode()
    regions = []
    for i in range(count):
        target, length, stage, flags, sha = struct.unpack_from("<IIII32s", sector, REGIONS_AT + i * REGION_SIZE)
        regions.append({"target": target, "length": length, "stage": stage, "flags": flags, "sha256": sha})
    if total != max(r["target"] + r["length"] for r in regions):
        raise StageError("bad total length")
    return label, total, regions


def flat_image(stage, regions, total):
    """The flat image the update server streams: data regions from the stage, everything else 0xFF."""
    out = bytearray(b"\xff" * total)
    for r in regions:
        if not r["flags"] & FLAG_ERASE:
            out[r["target"]:r["target"] + r["length"]] = stage[r["stage"]:r["stage"] + r["length"]]
    return bytes(out)


def build_stage(image, label):
    """Stage contents for an image at 0x0: the header, then the data regions back to back."""
    regions = []
    data = bytearray()
    stage = HEADER_SIZE
    for r in radio_flash().split_regions(0, image):
        entry = {"target": r["target"], "length": r["length"], "stage": 0, "flags": FLAG_ERASE,
                 "sha256": bytes.fromhex(r["sha256"])}
        if not r["erase"]:
            entry["stage"] = stage
            entry["flags"] = FLAG_PRESENT
            data += r["data"]
            stage += r["length"]
        regions.append(entry)
    size = stage_partition()[1]
    if stage > size:
        raise StageError("data regions take %d bytes, the stage holds %d" % (stage - HEADER_SIZE,
                                                                             size - HEADER_SIZE))
    return encode_header(label, regions) + bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--offset", action="store_true", help="print the stage partition offset and exit")
    ap.add_argument("--size", action="store_true", help="print the stage partition size and exit")
    ap.add_argument("--image")
    ap.add_argument("--label")
    ap.add_argument("--out")
    args = ap.parse_args()
    if args.offset or args.size:
        print("0x%x" % stage_partition()[0 if args.offset else 1])
        return
    if not (args.image and args.label and args.out):
        ap.error("need --image, --label and --out")
    try:
        with open(args.image, "rb") as f:
            stage = build_stage(f.read(), args.label)
    except (StageError, OSError) as e:
        sys.exit("make_stage: %s" % e)
    with open(args.out, "wb") as f:
        f.write(stage)
    _, total, regions = decode_header(stage)
    print("%s: %d bytes, %d regions (%d data), image %d bytes" % (
        args.out, len(stage), len(regions), sum(1 for r in regions if not r["flags"] & FLAG_ERASE), total))


if __name__ == "__main__":
    main()
