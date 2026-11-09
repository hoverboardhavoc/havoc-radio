#!/usr/bin/env python3
"""Stage files from tools/make_stage.py (installer/SPEC.md, Tests 1).

Usage: test_make_stage.py STAGE_FILE IMAGE_FILE [STAGE_FILE IMAGE_FILE ...]

For each pair: the header's regions are radio-flash's split of the image (target, length, erase-only, SHA-256), every
data region is present and stored at its stage offset back to back after the header, and the flat image is the
image padded to 4 KB. The C decoder reads the same files in test_stage_layout.
"""

import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import make_stage  # noqa: E402
from hostserver import Checks  # noqa: E402


def main():
    check = Checks("make_stage")
    rf = make_stage.radio_flash()
    args = sys.argv[1:]
    for stage_path, image_path in zip(args[0::2], args[1::2]):
        with open(stage_path, "rb") as f:
            stage = f.read()
        with open(image_path, "rb") as f:
            image = f.read()
        name = os.path.basename(stage_path)
        label, total, regions = make_stage.decode_header(stage[:make_stage.HEADER_SIZE])
        split = rf.split_regions(0, image)
        check([(r["target"], r["length"], bool(r["flags"] & make_stage.FLAG_ERASE), r["sha256"].hex())
               for r in regions] ==
              [(r["target"], r["length"], r["erase"], r["sha256"]) for r in split],
              "%s: header regions are radio-flash's split" % name)
        at = make_stage.HEADER_SIZE
        for r, s in zip(regions, split):
            if s["erase"]:
                check(r["flags"] == make_stage.FLAG_ERASE and r["stage"] == 0, "%s: erase-only 0x%x" % (name, r["target"]))
                continue
            data = stage[r["stage"]:r["stage"] + r["length"]]
            check(r["flags"] == make_stage.FLAG_PRESENT and r["stage"] == at, "%s: 0x%x present at its stage offset"
                  % (name, r["target"]))
            check(data == s["data"] and hashlib.sha256(data).digest() == r["sha256"], "%s: 0x%x data" % (name, r["target"]))
            at += r["length"]
        check(len(stage) == at, "%s: no bytes after the last region" % name)
        padded = image + b"\xff" * (-len(image) % make_stage.SECTOR)
        check(make_stage.flat_image(stage, regions, total) == padded, "%s: flat image is the image" % name)
        print("%s: \"%s\", %d regions, %d B staged, image %d B" % (name, label, len(regions), len(stage), total))

    # Labels are cut on a character boundary; over-full stages are refused.
    check(make_stage.cut_label("é" * 20) == ("é" * 16).encode(), "label cut at 32 bytes")
    check(make_stage.cut_label("a" + "é" * 20) == ("a" + "é" * 15).encode(), "label cut before a split character")
    try:
        make_stage.build_stage(os.urandom(make_stage.stage_partition()[1]), "x")
        check(False, "a stage-sized image is refused (the header takes a sector)")
    except make_stage.StageError:
        pass
    return check.report()


if __name__ == "__main__":
    sys.exit(main())
