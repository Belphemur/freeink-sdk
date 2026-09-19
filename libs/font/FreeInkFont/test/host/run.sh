#!/bin/sh
# Builds and runs the FreeInkFont host tests. No device or PlatformIO needed —
# the library is freestanding C++17 and FreeType compiles as-is with the same
# wrapper TUs (src/freetype/*.c) and config headers the firmware build uses.
# FreeType sources/headers come from the third_party/freetype submodule
# checkout; the freetype-config overlay (listed first) shadows its config.
set -e
cd "$(dirname "$0")"

BUILD_DIR="${TMPDIR:-/tmp}/freeinkfont-tests"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

INCLUDES="-I../../include -I../../freetype-config/include -I../../third_party/freetype/include -I../../third_party/stb"
FONT_FIXTURE="../fixtures/fonts/DejaVuSans.ttf"

# The GSUB companion TU to FtFont.cpp (upstream's ligature feature).
GSUB_SOURCE=
if [ -f ../../src/Gsub.cpp ]; then
  GSUB_SOURCE="../../src/Gsub.cpp"
fi

if cc --version 2>&1 | grep -qi clang; then
  FRAME_CHECK='-Wframe-larger-than=2048 -Werror=frame-larger-than'
else
  FRAME_CHECK='-Werror=frame-larger-than=2048'
fi

# compile_ft <defines> <obj-prefix>: build every FreeType wrapper TU through
# the same generated wrappers PlatformIO uses (FT2_BUILD_LIBRARY set inside
# each wrapper), plus FontAlloc. No -Werror on third-party code — only the
# library and test TUs must be warning-clean.
compile_ft() {
  defines="$1"; prefix="$2"
  for wrapper in ../../src/freetype/*.c; do
    obj="$BUILD_DIR/${prefix}$(basename "$wrapper" .c).o"
    # shellcheck disable=SC2086
    cc -O1 -std=c99 $defines $INCLUDES -c "$wrapper" -o "$obj"
  done
  cc -O1 -std=c99 $defines $INCLUDES -c ../../src/FontAlloc.c -o "$BUILD_DIR/${prefix}FontAlloc.o"
}

compile_ft "" "obj_"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES \
  ../../src/FtFont.cpp test_ftfont.cpp $GSUB_SOURCE \
  "$BUILD_DIR"/obj_*.o \
  -o "$BUILD_DIR/test_ftfont"
"$BUILD_DIR/test_ftfont" ../fixtures

# --- RenderOptions test matrix -------------------------------------------
# Flags-ON build: every HintingMode plus monochrome must work.
DEFINES='-DFREEINK_FONT_ENABLE_AUTOHINT=1 -DFREEINK_FONT_ENABLE_NATIVE_HINTING=1 -DFREEINK_FONT_ENABLE_MONOCHROME=1'
# Frame-size caveat for the two opt-in module wrappers (measured at -O2 on the
# submodule's FreeType): the auto-hinter's metrics walkers use ~15 KB frames
# (af_latin_metrics_init_widths 15216, afcjk 14960, afmodule 13408) and the
# B/W rasterizer's Render_Glyph 4112 — both exceed upstream's 2048-byte
# host-gate bound, so no -Werror=frame-larger-than check is run here. Neither
# module is compiled into any default build (glue bodies are #if'd off and
# ftmodule.h registers them only under FREEINK_FONT_ENABLE_AUTOHINT /
# _MONOCHROME); a consumer enabling either needs a render-task stack sized for
# these frames. ft_smooth.c independently trips the same bound (~16.7 KB frame
# in gray_convert_glyph; pre-existing finding).
compile_ft "$DEFINES" "on_"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES $DEFINES \
  ../../src/FtFont.cpp $GSUB_SOURCE \
  FtFontRenderOptionsTest.cpp "$BUILD_DIR"/on_*.o \
  -o "$BUILD_DIR/ftfont-render-options-test"
"$BUILD_DIR/ftfont-render-options-test" "$FONT_FIXTURE"

# A consumer that defines none of the three flags must still build AND RUN
# clean: Default rendering still works, and setRenderOptions() reports
# (without crashing on) a request this build can't honor.
compile_ft "" "minimal-"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES \
  ../../src/FtFont.cpp $GSUB_SOURCE \
  FtFontMinimalTest.cpp "$BUILD_DIR"/minimal-*.o \
  -o "$BUILD_DIR/ftfont-minimal-test"
"$BUILD_DIR/ftfont-minimal-test" "$FONT_FIXTURE"

# The PlatformIO idiom "-D FLAG=0" (explicitly off, distinct from never
# mentioning the flag) must behave identically to the never-mentioned case
# above — #ifdef instead of #if for the compiled-capability constants in
# FtFont.cpp (kAutohintCompiled etc.) once made a "=0" build report monochrome
# as supported while every glyph silently rasterized to nullptr.
EXPLICIT_OFF_DEFINES='-DFREEINK_FONT_ENABLE_AUTOHINT=0 -DFREEINK_FONT_ENABLE_NATIVE_HINTING=0 -DFREEINK_FONT_ENABLE_MONOCHROME=0'
compile_ft "$EXPLICIT_OFF_DEFINES" "explicitOff-"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES $EXPLICIT_OFF_DEFINES \
  ../../src/FtFont.cpp $GSUB_SOURCE \
  FtFontMinimalTest.cpp "$BUILD_DIR"/explicitOff-*.o \
  -o "$BUILD_DIR/ftfont-explicit-off-test"
"$BUILD_DIR/ftfont-explicit-off-test" "$FONT_FIXTURE"

# Regression check for "compiling AUTOHINT in never changes a Default caller's
# output": a per-instance assertion inside ONE binary can't catch a difference
# that only shows up BETWEEN two build configurations, so hash the
# default-options rasterized output across many sizes/codepoints and compare an
# AUTOHINT+MONOCHROME build (no native hinting) against the minimal one. These
# two MUST match (FT_LOAD_NO_AUTOHINT blocks exactly the autofit fallback).
# NATIVE_HINTING is deliberately excluded: once a native hinter is compiled in
# AND registered, FreeType uses it automatically for any hinted load on a font
# with real hint instructions — a build that defines
# FREEINK_FONT_ENABLE_NATIVE_HINTING has chosen real bytecode hinting, and
# different Default output afterward is expected, not a regression.
NO_NATIVE_DEFINES='-DFREEINK_FONT_ENABLE_AUTOHINT=1 -DFREEINK_FONT_ENABLE_MONOCHROME=1'
compile_ft "$NO_NATIVE_DEFINES" "noNative-"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES $NO_NATIVE_DEFINES \
  ../../src/FtFont.cpp $GSUB_SOURCE \
  FtFontDefaultParityHash.cpp "$BUILD_DIR"/noNative-*.o \
  -o "$BUILD_DIR/parity-no-native"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -Wall -Wextra -Werror $INCLUDES \
  ../../src/FtFont.cpp $GSUB_SOURCE \
  FtFontDefaultParityHash.cpp "$BUILD_DIR"/minimal-*.o \
  -o "$BUILD_DIR/parity-minimal"
HASH_NO_NATIVE=$("$BUILD_DIR/parity-no-native" "$FONT_FIXTURE")
HASH_MINIMAL=$("$BUILD_DIR/parity-minimal" "$FONT_FIXTURE")
echo "Default output hash — AUTOHINT+MONOCHROME (no native): $HASH_NO_NATIVE, minimal: $HASH_MINIMAL"
if [ "$HASH_NO_NATIVE" != "$HASH_MINIMAL" ]; then
  echo "FAIL: compiling in AUTOHINT/MONOCHROME (without NATIVE_HINTING) changed the default render output" >&2
  exit 1
fi

echo "FreeInkFont host tests: OK"
