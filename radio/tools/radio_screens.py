#!/usr/bin/env python3
"""Drive the radio's controls over the network, capture its screen and compare runs.

usage: radio_screens.py run SCRIPT --host HOST --out DIR
       radio_screens.py compare DIR REF

A script is lines of the input syntax of POST /api/input (turn N, press, long, side, sidehold, wait MS), each sent
as its own request, and:

  shot NAME        capture the screen to DIR/NAME.pbm and DIR/NAME.png
  mask X Y W H     leave a rectangle out of the comparison of the next shot (several may be given)
  tune KHZ         POST /api/tune
  mode M           POST /api/mode
  band B           POST /api/band

`#` starts a comment. The masks of each shot are written to DIR/masks.json. `compare` checks every capture in DIR
against the one of the same name in REF outside the masks of either run, writes a side-by-side PNG of each that
differs to DIR/diff/ (this run, the reference, and the differing pixels in red), and exits 1 when any differs.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

import pbm2png

W, H = 128, 32
INPUT_OPS = {"turn", "press", "long", "side", "sidehold", "wait"}
RED = (0xFF, 0x30, 0x30)
MASKED = (0x30, 0x30, 0x60)


def request(host, method, path, body=None, timeout=30):
    req = urllib.request.Request(f"http://{host}{path}", data=body, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def post_input(host, line):
    status, body = request(host, "POST", "/api/input", line.encode())
    if status != 200:
        raise SystemExit(f"{line}: {status} {body.decode(errors='replace')}")


def post_json(host, path, obj):
    status, body = request(host, "POST", path, json.dumps(obj).encode())
    if status != 200:
        raise SystemExit(f"{path} {obj}: {status} {body.decode(errors='replace')}")


def read_pbm(path):
    """Pixels of a 128x32 PBM, binary (P4, the captures) or plain (P1, the goldens), as rows of bools."""
    w, h, px = pbm2png.read_pbm(path)
    if (w, h) != (W, H):
        raise SystemExit(f"{path}: not 128x32")
    return px


def render(rows, scale=4):
    """Pixel colours scaled up with the OLED grid gap, as pbm2png renders the goldens."""
    out = []
    for y in range(H * scale):
        row = []
        for x in range(len(rows[0]) * scale):
            gap = x % scale == scale - 1 or y % scale == scale - 1
            row.append(pbm2png.GAP if gap else rows[y // scale][x // scale])
        out.append(row)
    return out


def write_png(path, rows):
    pbm2png.png(path, len(rows[0]) * 4, H * 4, render(rows))


def colours(px):
    return [[pbm2png.ON if p else pbm2png.OFF for p in r] for r in px]


def run(args):
    os.makedirs(args.out, exist_ok=True)
    masks, pending = {}, []
    for n, raw in enumerate(open(args.script, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        op, *rest = line.split()
        if op in INPUT_OPS:
            post_input(args.host, line)
        elif op == "shot" and len(rest) == 1:
            status, body = request(args.host, "GET", "/api/screen")
            if status != 200:
                raise SystemExit(f"{args.script}:{n}: /api/screen answered {status}")
            pbm = os.path.join(args.out, rest[0] + ".pbm")
            open(pbm, "wb").write(body)
            write_png(os.path.join(args.out, rest[0] + ".png"), colours(read_pbm(pbm)))
            masks[rest[0]] = pending
            pending = []
            print(pbm)
        elif op == "mask" and len(rest) == 4:
            pending.append([int(v) for v in rest])
        elif op in ("tune", "mode", "band") and len(rest) == 1:
            if op == "tune":
                post_json(args.host, "/api/tune", {"hz": round(float(rest[0]) * 1000)})
            else:
                post_json(args.host, "/api/" + op, {op: rest[0]})
            post_input(args.host, "wait 100")  # answers after a frame drawn with the new state
        else:
            raise SystemExit(f"{args.script}:{n}: {line!r} is not a script line")
    with open(os.path.join(args.out, "masks.json"), "w") as f:
        json.dump(masks, f, indent=1)


def load_masks(d):
    path = os.path.join(d, "masks.json")
    return json.load(open(path)) if os.path.exists(path) else {}


def compare(args):
    masks, ref_masks = load_masks(args.dir), load_masks(args.ref)
    names = sorted(f[:-4] for f in os.listdir(args.dir) if f.endswith(".pbm"))
    diff_dir = os.path.join(args.dir, "diff")
    failed = 0
    for name in names:
        ref = os.path.join(args.ref, name + ".pbm")
        if not os.path.exists(ref):
            print(f"{name}: not in {args.ref}")
            failed += 1
            continue
        a, b = read_pbm(os.path.join(args.dir, name + ".pbm")), read_pbm(ref)
        masked = [[False] * W for _ in range(H)]
        for x0, y0, w, h in masks.get(name, []) + ref_masks.get(name, []):
            for y in range(max(y0, 0), min(y0 + h, H)):
                for x in range(max(x0, 0), min(x0 + w, W)):
                    masked[y][x] = True
        diff = [[a[y][x] != b[y][x] and not masked[y][x] for x in range(W)] for y in range(H)]
        count = sum(map(sum, diff))
        if not count:
            continue
        failed += 1
        os.makedirs(diff_dir, exist_ok=True)
        marks = [[RED if diff[y][x] else MASKED if masked[y][x] else pbm2png.OFF for x in range(W)] for y in range(H)]
        side = [ra + [pbm2png.GAP] * 4 + rb + [pbm2png.GAP] * 4 + rm
                for ra, rb, rm in zip(colours(a), colours(b), marks)]
        out = os.path.join(diff_dir, name + ".png")
        write_png(out, side)
        print(f"{name}: {count} pixels differ outside the masks ({out})")
    print(f"{len(names)} captures, {failed} differ")
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run a script, capturing the screen at each shot")
    r.add_argument("script")
    r.add_argument("--host", required=True)
    r.add_argument("--out", required=True)
    c = sub.add_parser("compare", help="compare the captures in DIR with those in REF")
    c.add_argument("dir")
    c.add_argument("ref")
    args = p.parse_args()
    if args.cmd == "run":
        run(args)
        return 0
    return compare(args)


if __name__ == "__main__":
    sys.exit(main())
