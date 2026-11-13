#!/usr/bin/env python3
"""Render the framebuffer goldens by running the UI prototype headless.

usage: gen_goldens.py [--check] <prototype.html> <scenarios.txt> <golden dir>

The prototype's own script runs in QuickJS with the browser objects stubbed out. Each scenario of
test/scenarios.txt is replayed from power-on through the prototype's input handlers and its frame()
function, and every `render` writes the 128x32 framebuffer as a plain PBM file. The host tests replay the
same scenarios through the firmware UI and compare against these files.

With --check nothing is written; the script exits non-zero when a golden differs from the prototype.
"""
import json
import os
import re
import sys

import quickjs

STUBS = r"""
var NOW = 1000;
function stub() {
  const f = function () {};
  return new Proxy(f, {
    get(t, p) { if (p === Symbol.toPrimitive) return () => 0; return stub(); },
    set() { return true; },
    apply() { return stub(); },
  });
}
var document = stub();
var performance = { now: () => NOW };
var addEventListener = () => {};
var requestAnimationFrame = () => {};
var getComputedStyle = () => stub();
var setTimeout = () => 0;
var clearTimeout = () => {};
"""

# Runs after the prototype script: deterministic signal bars, no canvas output, no simulated seek motion.
HARNESS = r"""
var BARS = 3;
rssi = () => BARS;
var RDS = { sync: false, ps: '', rt: '', pty: 0, seq: 0 };
rds = () => RDS;
present = () => {};
lastSeekStep = 1e15;
function fbText() { return Array.from(fb).join(''); }
"""

BOOT = 1000  # power-on time in the prototype's clock; scenario times are relative to it
DAY = 1794268800000  # 2026-11-10 00:00 UTC in ms: the date of an `ntp HH:MM` sync
# `dl result NAME`: the string each download result shows on home.
DL_RESULTS = {"updated": "guide_updated", "uptodate": "up_to_date", "nonet": "no_network", "failed": "download_failed",
              "checkfailed": "guide_check_failed"}


def parse_scenarios(path):
    scenarios, cur, t = [], None, 0
    for raw in open(path, encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        if words[0] == "scenario":
            cur = (words[1], [])
            scenarios.append(cur)
            t = None
            continue
        if words[0].startswith("@"):
            t = int(words[0][1:])
            words = words[1:]
        else:
            t = 3000 if t is None else t + 100
        cur[1].append((t, words))
    return scenarios


def script_of(html):
    blocks = re.findall(r"<script>(.*?)</script>", html, re.S)
    return blocks[-1]


def run(script, scenario):
    name, steps = scenario
    ctx = quickjs.Context()
    ctx.eval(STUBS)
    ctx.eval(script)
    ctx.eval(HARNESS)
    ctx.eval(f"NOW = {BOOT}; frame(NOW);")
    frames = []
    for t, words in steps:
        ctx.eval(f"NOW = {BOOT + t}; frame(NOW);")
        op, args = words[0], words[1:]
        js = {
            "turn": lambda: f"onTurn({int(args[0])})",
            "hold": lambda: f"onHoldTurn({int(args[0])})",
            "press": lambda: "onClick()",
            "long": lambda: "onLong()",
            "side": lambda: "onSide()",
            "sidehold": lambda: "onSideHold()",
            "ota": lambda: "startConfirm()",
            "empty": lambda: "showMessage(T('battery_empty'))",
        }
        if op == "render":
            ctx.eval("frame(NOW)")
            label = name + ("_" + args[0] if args else "")
            frames.append((label, ctx.eval("fbText()")))
        elif op == "set":
            key, val = args
            if key == "bars":
                ctx.eval(f"BARS = {int(val)}")
            elif key == "ssb":
                ctx.eval(f"S.ssb = {'true' if val == '1' else 'false'}")
            elif key == "lang":
                ctx.eval(f"S.lang = {val!r}")
            elif key == "wifi" and val == "off":
                ctx.eval("S.wifi.joined = null")
            elif key == "wifi" and val == "none":
                ctx.eval("S.wifi.joined = null; S.wifi.saved = null")
            elif key == "guide" and val == "off":
                ctx.eval("S.guide = false")
            elif key == "server":
                ctx.eval(f"S.server = {json.dumps(val)}")
            else:
                raise ValueError(f"{name}: unknown set {key}")
        elif op == "rds":
            key, text = args[0], " ".join(args[1:])
            if key == "sync":
                ctx.eval(f"RDS.sync = {'true' if text == '1' else 'false'}")
            elif key == "pty":
                ctx.eval(f"RDS.pty = {int(text)}")
            elif key in ("ps", "rt"):
                ctx.eval(f"RDS.{key} = {json.dumps(text)}")
            elif key == "ab":
                ctx.eval("RDS.seq++; RDS.rt = ''")
            else:
                raise ValueError(f"{name}: unknown rds {key}")
        elif op == "dl":
            # Download mode at one step, or its result on home (3 s).
            if args[0] == "result":
                ctx.eval(f"goHome(); toast(T({json.dumps(DL_RESULTS[args[1]])}), 3000)")
            else:
                done, total = (int(args[1]), int(args[2])) if len(args) > 2 else (0, 0)
                ctx.eval(f"S.view = 'download'; S.stack = []; S.dl = {{ step: {json.dumps(args[0])}, done: {done}, "
                         f"total: {total}, frozen: true }}")
        elif op == "ntp":
            h, m = (int(x) for x in args[0].split(":"))
            ctx.eval(f"ntpSync({DAY + (h * 60 + m) * 60000})")
        elif op in js:
            ctx.eval(js[op]())
        else:
            raise ValueError(f"{name}: unknown op {op}")
    return frames


def pbm(bits):
    rows = [bits[y * 128:(y + 1) * 128] for y in range(32)]
    return "P1\n128 32\n" + "\n".join(rows) + "\n"


def main():
    args = sys.argv[1:]
    check = args and args[0] == "--check"
    if check:
        args = args[1:]
    html, scen, outdir = args
    script = script_of(open(html, encoding="utf-8").read())
    names, bad = set(), 0
    for sc in parse_scenarios(scen):
        for label, bits in run(script, sc):
            if label in names:
                sys.exit(f"duplicate golden name {label}")
            names.add(label)
            path = os.path.join(outdir, label + ".pbm")
            text = pbm(bits)
            if check:
                try:
                    same = open(path, encoding="ascii").read() == text
                except OSError:
                    same = False
                if not same:
                    print("differs:", label)
                    bad += 1
            else:
                with open(path, "w", encoding="ascii") as f:
                    f.write(text)
    if not check:
        for fn in os.listdir(outdir):
            if fn.endswith(".pbm") and fn[:-4] not in names:
                os.remove(os.path.join(outdir, fn))
    print(f"{len(names)} goldens" + (f", {bad} differ" if check else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
