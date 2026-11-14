#!/bin/sh
# Soak finding 2: the lift of a suppressed touch contact must not leak into
# the next consumer's frame (durable one-shots dropped when the latch clears).
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-gt911-lift.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
for flag in 0 1; do
  c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
    -DFREEINK_DEVICE_X4PRO=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DFREEINK_GT911_INT_WAKE=$flag \
    -Igt911_stubs -Ipicco_stubs -I../../include -I../../../BoardConfig/include \
    test_gt911_suppressed_lift.cpp ../../src/InputManager.cpp -o "$BUILD_DIR/test_gt911_suppressed_lift_$flag"
  "$BUILD_DIR/test_gt911_suppressed_lift_$flag"
done
