#!/usr/bin/env python3
"""Install the pinned ESP8266_RTOS_SDK, its xtensa-lx106 toolchain and a Python venv under radio/third_party/.

Everything goes into the git-ignored radio/third_party/ directory; nothing outside the repository is changed.
Re-running the script skips steps that are already done.
"""
import hashlib
import os
import platform
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repository root
TP = os.path.join(ROOT, "radio", "third_party")

SDK_URL = "https://github.com/espressif/ESP8266_RTOS_SDK.git"
SDK_TAG = "v3.4"
# Submodules the build needs. The SDK's CMake checks every submodule, so the unused ones (mqtt, coap) are
# fetched too, without recursion: libcoap's nested submodule URL no longer resolves.
SUBMODULES = [
    "components/json/cJSON",
    "components/mbedtls/mbedtls",
    "components/lwip/lwip",
    "components/mqtt/esp-mqtt",
    "components/coap/libcoap",
]

TOOLCHAIN = {
    "Darwin": ("https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-macos.tar.gz",
               "7823c5fdf3cea2b588cf0c5bd962774ab60a30b8d9c19ae0056a0ef3697537db"),
    "Linux": ("https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz",
              None),
}


def run(*cmd, cwd=None):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=cwd)


def sdk():
    path = os.path.join(TP, "ESP8266_RTOS_SDK")
    if not os.path.isdir(path):
        run("git", "clone", "--depth", "1", "--branch", SDK_TAG, SDK_URL, path)
    for sm in SUBMODULES:
        run("git", "submodule", "update", "--init", "--depth", "1", sm, cwd=path)


def toolchain():
    dest = os.path.join(TP, "xtensa-lx106-elf")
    if os.path.isdir(dest):
        return
    url, sha = TOOLCHAIN[platform.system()]
    tarball = os.path.join(TP, "toolchain.tar.gz")
    print("+ download", url)
    urllib.request.urlretrieve(url, tarball)
    if sha:
        h = hashlib.sha256(open(tarball, "rb").read()).hexdigest()
        if h != sha:
            os.remove(tarball)
            sys.exit(f"toolchain checksum mismatch: {h}")
    with tarfile.open(tarball) as t:
        t.extractall(TP)
    os.remove(tarball)


def venv():
    path = os.path.join(TP, "venv")
    if not os.path.isdir(path):
        run(sys.executable, "-m", "venv", path)
    pip = os.path.join(path, "bin", "pip")
    # The SDK's dependency check imports pkg_resources, which newer setuptools no longer ships.
    run(pip, "install", "-q", "setuptools<70")
    run(pip, "install", "-q", "-r", os.path.join(TP, "ESP8266_RTOS_SDK", "requirements.txt"))
    # QuickJS runs the UI prototype headless to produce the framebuffer goldens.
    run(pip, "install", "-q", "quickjs")


if __name__ == "__main__":
    os.makedirs(TP, exist_ok=True)
    sdk()
    toolchain()
    venv()
    print("done")
