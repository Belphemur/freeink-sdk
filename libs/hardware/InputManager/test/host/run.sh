#!/bin/sh
# Builds and runs the allocation-free multi-touch gesture math tests.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-input-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror test_multitouch_gesture_math.cpp -o "$BUILD_DIR/test_multitouch_gesture_math"
"$BUILD_DIR/test_multitouch_gesture_math"

c++ -std=c++17 -Wall -Wextra -Werror -I../../include test_latch_policy.cpp -o "$BUILD_DIR/test_latch_policy"
"$BUILD_DIR/test_latch_policy"

echo "InputManager host tests: OK"
