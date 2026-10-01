#!/bin/sh
# Builds and runs the Xteink X3/X4 (ESP32-C3) LEGACY SYNC input-path test.
# This is the path the gh_release (C3) binary runs: beginAsync() is gated on
# BOARD_HAS_PSRAM and is therefore never compiled in for that build.
set -eu
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-x4-sync-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -DFREEINK_DEVICE_X4=1 \
  -Ix4_sync_stubs -I../../include -I../../../BoardConfig/include \
  test_x4_sync_path.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_x4_sync_path"
"$BUILD_DIR/test_x4_sync_path"
