#!/bin/sh
# Builds and runs the FreeInkFont host tests. No device or PlatformIO needed —
# the library is freestanding C++17 and the vendored FreeType compiles as-is
# with the same wrapper TUs (src/freetype/*.c) and config headers the firmware
# build uses.
set -e
cd "$(dirname "$0")"

BUILD_DIR="${TMPDIR:-/tmp}/freeinkfont-tests"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

INCLUDES="-I../../include -I../../freetype-config/include -I../../third_party/freetype/include"

# The vendored FreeType modules, compiled through the same generated wrappers
# PlatformIO uses (FT2_BUILD_LIBRARY set inside each wrapper). No -Werror on
# third-party code — only the library and test TUs must be warning-clean.
FT_OBJS=""
for wrapper in ../../src/freetype/*.c; do
  obj="$BUILD_DIR/obj_$(basename "$wrapper" .c).o"
  cc -O1 -std=c99 $INCLUDES -c "$wrapper" -o "$obj"
  FT_OBJS="$FT_OBJS $obj"
done

cc -O1 -std=c99 $INCLUDES -c ../../src/FontAlloc.c -o "$BUILD_DIR/obj_FontAlloc.o"

c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES \
  ../../src/FtFont.cpp test_ftfont.cpp \
  "$BUILD_DIR"/obj_*.o \
  -o "$BUILD_DIR/test_ftfont"

"$BUILD_DIR/test_ftfont" ../fixtures
