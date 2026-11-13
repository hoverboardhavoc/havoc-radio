#!/usr/bin/env python3
"""Commit a release's app image and its manifest to the firmware branch, from which radios update themselves.

usage: firmware_branch.py --image <havoc-radio.bin> --version <V> [--released <ISO 8601>] [--remote origin]
                          [--branch firmware] [--layout layout1] [--push]

Run in a clone of hoverboardhavoc/havoc-radio (the release job's checkout). The branch holds README.md and one folder per
flash layout; this replaces the layout's folder with havoc-radio.bin and manifest.json
({"version", "length", "sha256", "released"}) and keeps everything else on the branch. The commit is made from git
objects alone, so the checkout's own files are not touched: its parent is the remote branch's tip, or none when the
branch does not exist yet (the first release creates it). The author is github-actions[bot] and the message
"Havoc Radio <V>". When the tree would not change (the same release again) nothing is committed. The commit is stored
as refs/heads/<branch> and, with --push, pushed to the remote.
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys

BOT = ("github-actions[bot]", "41898282+github-actions[bot]@users.noreply.github.com")
SLOT_SIZE = 0x100000  # an app slot of layout 1 (radio/partitions.csv)

README = """# Havoc Radio firmware

This branch holds the app image of the latest Havoc Radio release, which a radio downloads in Settings > Firmware >
Update. The release workflow writes it; every release, with the install image and the installer, is on the
[releases page](https://github.com/hoverboardhavoc/havoc-radio/releases).

Each flash layout has its own folder. `layout1` is the layout with two 1 MB app slots at `0x100000` and `0x200000`:

- `layout1/havoc-radio.bin`: the app image, the same file as the release's `havoc-radio.bin`.
- `layout1/manifest.json`: its version, its length in bytes, its SHA-256 and the time of the release.
"""


def git(*args, data=None, env=None):
    r = subprocess.run(["git", *args], input=data, capture_output=True, env=env)
    if r.returncode:
        sys.exit(f"git {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout.decode().strip()


def blob(data):
    return git("hash-object", "-w", "--stdin", data=data)


def manifest(image, version, released):
    return {"version": version, "length": len(image), "sha256": hashlib.sha256(image).hexdigest(), "released": released}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--released", default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    ap.add_argument("--remote", default="origin")
    ap.add_argument("--branch", default="firmware")
    ap.add_argument("--layout", default="layout1")
    ap.add_argument("--push", action="store_true")
    a = ap.parse_args()

    image = open(a.image, "rb").read()
    if not image or image[0] != 0xE9 or len(image) > SLOT_SIZE:
        sys.exit(f"{a.image}: not an app image of at most {SLOT_SIZE} bytes")
    if not all(f.isdigit() for f in a.version.split(".")):
        sys.exit(f"{a.version}: not a version of numbers separated by dots")

    # The remote branch's tip, when the branch exists.
    parent = None
    if git("ls-remote", "--heads", a.remote, f"refs/heads/{a.branch}"):
        git("fetch", "--quiet", "--depth=1", a.remote, f"refs/heads/{a.branch}")
        parent = git("rev-parse", "FETCH_HEAD")

    m = json.dumps(manifest(image, a.version, a.released), indent=2) + "\n"
    layout = git("mktree", data=(f"100644 blob {blob(image)}\thavoc-radio.bin\n"
                                 f"100644 blob {blob(m.encode())}\tmanifest.json\n").encode())
    entries = [f"100644 blob {blob(README.encode())}\tREADME.md", f"040000 tree {layout}\t{a.layout}"]
    if parent:
        for line in git("ls-tree", parent).splitlines():
            if line.split("\t", 1)[1] not in ("README.md", a.layout):
                entries.append(line)
    tree = git("mktree", data=("\n".join(entries) + "\n").encode())

    if parent and git("rev-parse", f"{parent}^{{tree}}") == tree:
        print(f"{a.branch}: already holds Havoc Radio {a.version}")
        return
    env = dict(os.environ, GIT_AUTHOR_NAME=BOT[0], GIT_AUTHOR_EMAIL=BOT[1], GIT_COMMITTER_NAME=BOT[0],
               GIT_COMMITTER_EMAIL=BOT[1])
    commit = git("commit-tree", tree, *(["-p", parent] if parent else []), "-m", f"Havoc Radio {a.version}", env=env)
    git("update-ref", f"refs/heads/{a.branch}", commit)
    print(f"{a.branch}: {commit} Havoc Radio {a.version}, {len(image)} bytes" + ("" if parent else " (new branch)"))
    if a.push:
        git("push", "--quiet", a.remote, f"{commit}:refs/heads/{a.branch}")


if __name__ == "__main__":
    main()
