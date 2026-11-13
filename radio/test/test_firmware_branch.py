#!/usr/bin/env python3
"""Dry run of the release job's firmware branch step (radio/tools/firmware_branch.py) against local repositories.

usage: test_firmware_branch.py

A bare repository stands in for GitHub, and each run starts from a fresh, empty clone as the release job's checkout
does, with a fake release image. The first release creates the branch with no parent; the next adds a commit on top
and keeps a folder it does not own; the same release again commits nothing; a file that is not an app image is
refused. Everything happens under radio/build/firmware-branch-test.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(ROOT, "radio", "tools", "firmware_branch.py")
WORK = os.path.join(ROOT, "radio", "build", "firmware-branch-test")
ORIGIN = os.path.join(WORK, "origin.git")
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        sys.exit(f"FAIL {what}")


def git(cwd, *args, ok=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True)
    if ok and r.returncode:
        sys.exit(f"git {' '.join(args)}: {r.stderr.decode().strip()}")
    return r.stdout


def checkout(name):
    """A fresh clone with nothing in it but the remote, as the job's checkout of a tag is for this branch."""
    d = os.path.join(WORK, name)
    git(WORK, "init", "--quiet", d)
    git(d, "remote", "add", "origin", ORIGIN)
    return d


def release(name, version, image, released="2026-11-16T09:12:00Z", expect_ok=True):
    d = checkout(name)
    path = os.path.join(WORK, f"{name}.bin")
    open(path, "wb").write(image)
    r = subprocess.run([sys.executable, SCRIPT, "--image", path, "--version", version, "--released", released, "--push"],
                       cwd=d, capture_output=True, text=True)
    check((r.returncode == 0) == expect_ok, f"{name}: exit {r.returncode}: {r.stderr.strip()}")
    return r.stdout


def image(n, seed):
    b = bytearray((i * seed + 7) & 0xFF for i in range(n))
    b[0] = 0xE9
    return bytes(b)


def show(path):
    return git(ORIGIN, "show", f"firmware:{path}")


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    git(WORK, "init", "--quiet", "--bare", ORIGIN)

    # The first release: the branch does not exist yet.
    first = image(988160, 131)
    out = release("r1", "2026.47.0", first)
    check("(new branch)" in out, "first release creates the branch")
    files = git(ORIGIN, "ls-tree", "-r", "--name-only", "firmware").decode().split()
    check(files == ["README.md", "layout1/havoc-radio.bin", "layout1/manifest.json"], f"files {files}")
    check(show("layout1/havoc-radio.bin") == first, "the image as released")
    m = json.loads(show("layout1/manifest.json"))
    check(m == {"version": "2026.47.0", "length": len(first), "sha256": hashlib.sha256(first).hexdigest(),
                "released": "2026-11-16T09:12:00Z"}, f"manifest {m}")
    log = git(ORIGIN, "log", "--format=%an <%ae>|%cn|%s|%P", "firmware").decode().strip().splitlines()
    check(log == ["github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>|github-actions[bot]|"
                  "Havoc Radio 2026.47.0|"], f"first commit {log}")
    check(b"Settings > Firmware" in show("README.md"), "README")
    check(git(ORIGIN, "branch", "--list").decode().split() == ["firmware"], "only the firmware branch")

    # A folder the step does not own, as a later layout's would be.
    d = checkout("other")
    git(d, "fetch", "--quiet", "origin", "firmware")
    git(d, "checkout", "--quiet", "-b", "firmware", "FETCH_HEAD")
    os.makedirs(os.path.join(d, "layout2"))
    open(os.path.join(d, "layout2", "manifest.json"), "w").write("{}\n")
    git(d, "add", "layout2")
    git(d, "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "--quiet", "-m", "layout2")
    git(d, "push", "--quiet", "origin", "firmware")

    # The next release: a commit on top, layout2 kept.
    second = image(990000, 97)
    out = release("r2", "2026.48.0", second, "2026-11-23T09:00:00Z")
    check("(new branch)" not in out, "second release on the existing branch")
    log = git(ORIGIN, "log", "--format=%s", "firmware").decode().split("\n")
    check(log[:3] == ["Havoc Radio 2026.48.0", "layout2", "Havoc Radio 2026.47.0"], f"history {log}")
    check(show("layout1/havoc-radio.bin") == second, "the new image")
    check(json.loads(show("layout1/manifest.json"))["version"] == "2026.48.0", "the new manifest")
    check(show("layout2/manifest.json") == b"{}\n", "the other layout kept")
    check(git(ORIGIN, "show", "firmware~2:layout1/havoc-radio.bin") == first, "the old image in the history")

    # The same release again: nothing to commit.
    tip = git(ORIGIN, "rev-parse", "firmware")
    out = release("r3", "2026.48.0", second, "2026-11-23T09:00:00Z")
    check("already holds" in out and git(ORIGIN, "rev-parse", "firmware") == tip, "a repeated release commits nothing")

    # Not an app image, too long for a slot, or a version that is not one: refused, nothing pushed.
    release("r4", "2026.49.0", b"PK\x03\x04" + bytes(100), expect_ok=False)
    release("r5", "2026.49.0", image(0x100001, 3), expect_ok=False)
    release("r6", "v2026.49.0", second, expect_ok=False)
    check(git(ORIGIN, "rev-parse", "firmware") == tip, "refused releases push nothing")
    print(f"firmware branch: {checks} checks, 0 failures")


if __name__ == "__main__":
    main()
