#!/bin/sh
# Runs idf.py on the radio project with the SDK, toolchain and venv from radio/third_party/
# (see radio/tools/setup_sdk.py). The build directory is radio/build/.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export IDF_PATH="$ROOT/radio/third_party/ESP8266_RTOS_SDK"
export PATH="$ROOT/radio/third_party/xtensa-lx106-elf/bin:$ROOT/radio/third_party/venv/bin:$PATH"
# The SDK's CMake files predate CMake 4.
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd "$ROOT"
exec python "$IDF_PATH/tools/idf.py" -C "$ROOT/radio" -B "$ROOT/radio/build" "$@"
