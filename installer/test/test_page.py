#!/usr/bin/env python3
"""The page's JavaScript under Node (installer/SPEC.md, Tests 4).

Usage: test_page.py NODE IMAGE [IMAGE ...]

The page's SHA-256 against known vectors and hashlib (lengths around the block and padding boundaries), its region
split against tools/radio-flash's for each image (the install image and the 4 MB stock backup), and its console
filter that removes ANSI colour sequences, also when a frame ends inside one.
"""

import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import make_stage  # noqa: E402
from hostserver import Checks  # noqa: E402

PAGE = os.path.join(os.path.dirname(HERE), "page", "index.html")
WORK = os.path.join(ROOT, "installer", "build", "host")

E = "\x1b"
# Console frames and the text the page shows for them. Havoc Radio's ESP-IDF log colours its lines.
CSI = [
    (["plain text\r\n"], "plain text\r\n"),
    ([E + "[0;32mI (312) radio: up" + E + "[0m\r\n"], "I (312) radio: up\r\n"),
    ([E + "[0;3", "1mE (5) x" + E + "[", "0m!"], "E (5) x!"),
    (["a" + E, "[1mb"], "ab"),
    (["a" + E + "Xb"], "aXb"),
    ([E + "[2K" + E + "[1;1Hhome"], "home"),
    (["ets Jan  8 2013,rst cause:2, boot mode:(3,6)\n"], "ets Jan  8 2013,rst cause:2, boot mode:(3,6)\n"),
]

# FIPS 180-2 vectors.
KNOWN = [
    (b"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
    (b"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
    (b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
    (b"a" * 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
]


def main():
    node, images = sys.argv[1], sys.argv[2:]
    check = Checks("page")
    vectors = [m for m, _ in KNOWN] + [bytes(range(256)) * 2 + bytes(n) for n in (0, 1, 54, 55, 56, 57, 63, 64, 65)]
    vectors.append(b"\xff" * 4096)
    os.makedirs(WORK, exist_ok=True)
    request = os.path.join(WORK, "page-request.json")
    with open(request, "w") as f:
        json.dump({"sha256": [v.hex() for v in vectors], "split": images, "csi": [c[0] for c in CSI]}, f)
    try:
        out = subprocess.run([node, os.path.join(HERE, "page_harness.js"), PAGE, request], check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print("FAIL: node: %s %s" % (e, getattr(e, "stderr", "")))
        return 1
    got = json.loads(out)
    for (_, want), digest in zip(KNOWN, got["sha256"]):
        check(digest == want, "known vector: %s" % digest)
    for v, digest in zip(vectors, got["sha256"]):
        check(digest == hashlib.sha256(v).hexdigest(), "%d bytes: %s" % (len(v), digest))

    for (frames, want), text in zip(CSI, got["csi"]):
        check(text == want, "console %r: %r" % (frames, text))

    rf = make_stage.radio_flash()
    for path, regions in zip(images, got["split"]):
        with open(path, "rb") as f:
            want = [{k: r[k] for k in ("target", "length", "erase", "sha256")} for r in rf.split_regions(0, f.read())]
        check(regions == want, "%s: page split equals radio-flash's (%d regions)" % (os.path.basename(path), len(want)))
        print("%s: %d regions, %d erase-only" % (os.path.basename(path), len(regions),
                                                 sum(1 for r in regions if r["erase"])))
    return check.report()


if __name__ == "__main__":
    sys.exit(main())
