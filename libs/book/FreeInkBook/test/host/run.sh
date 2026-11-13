#!/bin/sh
# Builds and runs the FreeInkBook host tests. No device or PlatformIO needed —
# the library is freestanding C++17. Fixture EPUBs are assembled here with the
# system `zip` (mimetype stored first, per the EPUB OCF spec).
set -e
cd "$(dirname "$0")"

# The miniz fork is a nested submodule. Fail loudly rather than producing a
# confusing missing-header/compiler error from an empty gitlink directory.
SDK_ROOT="$(cd ../../../../.. && pwd)"
if [ ! -f "$SDK_ROOT/libs/book/FreeInkBook/third_party/miniz/include/full_miniz.h" ]; then
  echo "error: esp_full_miniz submodule is not checked out" >&2
  echo "run: git -C '$SDK_ROOT' submodule update --init --recursive" >&2
  exit 1
fi

BUILD_DIR="${TMPDIR:-/tmp}/freeinkbook-tests"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/fixtures" "$BUILD_DIR/obj"

# --- fixtures ---------------------------------------------------------------
cp -R ../fixtures/minimal "$BUILD_DIR/fixtures/minimal"
cp -R ../fixtures/ncxonly "$BUILD_DIR/fixtures/ncxonly"
python3 ../fixtures/gen_big_chapter.py "$BUILD_DIR/fixtures/minimal/OEBPS/text/ch2.xhtml"
mkdir -p "$BUILD_DIR/fixtures/minimal/OEBPS/images"
python3 ../fixtures/gen_test_image.py "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png"
python3 ../fixtures/gen_gif_bmp.py "$BUILD_DIR/fixtures/minimal/OEBPS/images"
# JPEG variant via macOS sips (or ImageMagick if available).
if command -v sips >/dev/null 2>&1; then
  sips -s format jpeg "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png" \
    --out "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.jpg" >/dev/null
elif command -v convert >/dev/null 2>&1; then
  convert "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png" \
    "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.jpg"
else
  # No JPEG converter — reuse the PNG bytes; the JPEG-specific test will skip.
  cp "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.png" \
     "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern.jpg"
fi

# Progressive JPEG variant of the pattern (PIL; skipped when absent — the
# matching test skips itself if the entry is missing from the fixture).
python3 - "$BUILD_DIR/fixtures/minimal/OEBPS/images" <<'PYEOF2' || true
import sys
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
d = sys.argv[1]
Image.open(d + "/pattern.png").convert("RGB").save(d + "/pattern_prog.jpg",
                                                   progressive=True, quality=92)
PYEOF2

# 512x512 baseline JPEG for the pre-scale arena-retry test (yuvj420p; skipped
# when no converter exists).
IMGDIR="$BUILD_DIR/fixtures/minimal/OEBPS/images"
if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -y -loglevel error -i "$IMGDIR/pattern.png" -vf scale=512:512 \
    -pix_fmt yuvj420p "$IMGDIR/pattern_512.jpg" || true
fi

# make_epub <stage-dir> <out.epub> <compression-flag>
make_epub() {
  (cd "$1" \
    && zip -X -q "$3" "$BUILD_DIR/fixtures/$2" mimetype \
    && zip -X -q -r "$3" "$BUILD_DIR/fixtures/$2" META-INF OEBPS)
}
make_epub "$BUILD_DIR/fixtures/minimal" minimal.epub -9
make_epub "$BUILD_DIR/fixtures/minimal" stored.epub -0
# Stored twin for the JPEG arena-retry test (the zip shim above always
# deflates; the test needs an entry with no 32 KB inflate window).
python3 ../fixtures/gen_stored_epub.py "$BUILD_DIR/fixtures/minimal" \
  "$BUILD_DIR/fixtures/stored512.epub"
ls "$BUILD_DIR/fixtures/minimal/OEBPS/images/pattern_512.jpg" >/dev/null 2>&1 \
  || echo "note: pattern_512.jpg absent — testJpegScaleArenaRetry will skip"

# Real-fixture JPEG pages (committed under ../fixtures/images) for the
# JPEGDEC front-end parity test. Stored entries so the render needs no
# 32 KB inflate window; skipped when the fixtures are absent.
if [ -f ../fixtures/images/00004.jpeg ]; then
  IMG_STAGE="$BUILD_DIR/fixtures/images_stage"
  rm -rf "$IMG_STAGE"
  mkdir -p "$IMG_STAGE/META-INF" "$IMG_STAGE/OEBPS/images" "$IMG_STAGE/OEBPS/text"
  cp ../fixtures/images/00004.jpeg ../fixtures/images/00005.jpeg \
     ../fixtures/images/00006.jpeg ../fixtures/images/wide_prog.jpeg \
     ../fixtures/images/odd_dim.jpeg "$IMG_STAGE/OEBPS/images/"
  # Malformed JPEG header: SOI + a progressive SOF whose declared length is
  # never delivered (padded to the probe's 26-byte minimum). The probe still
  # reports a Jpeg; the renderer must surface ParseError, not OutOfMemory.
  printf '\377\330\377\302\000\017\010\000\020\040\003\001\000\000\000\000\000\000\000\000\000\000\000\000\000\000' \
    > "$IMG_STAGE/OEBPS/images/broken.jpg"
  printf 'application/epub+zip' > "$IMG_STAGE/mimetype"
  cat > "$IMG_STAGE/META-INF/container.xml" <<'EOF2'
<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
EOF2
  cat > "$IMG_STAGE/OEBPS/content.opf" <<'EOF2'
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:freeinkbook-fixture-0002</dc:identifier>
    <dc:title>Fixture Image Pages</dc:title>
    <dc:creator>Ann Author</dc:creator>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="p4" href="text/p4.xhtml" media-type="application/xhtml+xml"/>
    <item id="p5" href="text/p5.xhtml" media-type="application/xhtml+xml"/>
    <item id="p6" href="text/p6.xhtml" media-type="application/xhtml+xml"/>
    <item id="img4" href="images/00004.jpeg" media-type="image/jpeg"/>
    <item id="img5" href="images/00005.jpeg" media-type="image/jpeg"/>
    <item id="img6" href="images/00006.jpeg" media-type="image/jpeg"/>
    <item id="img7" href="images/wide_prog.jpeg" media-type="image/jpeg"/>
    <item id="img8" href="images/odd_dim.jpeg" media-type="image/jpeg"/>
    <item id="img9" href="images/broken.jpg" media-type="image/jpeg"/>
  </manifest>
  <spine><itemref idref="nav"/><itemref idref="p4"/><itemref idref="p5"/><itemref idref="p6"/> \
         <itemref idref="p7"/><itemref idref="p8"/><itemref idref="p9"/></spine>
</package>
EOF2
  cat > "$IMG_STAGE/OEBPS/nav.xhtml" <<'EOF2'
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>n</title></head>
<body><nav epub:type="toc"><ol><li><a href="text/p4.xhtml">p4</a></li></ol></nav></body></html>
EOF2
  for n in 4 5 6; do
    cat > "$IMG_STAGE/OEBPS/text/p$n.xhtml" <<EOF2
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>p$n</title></head>
<body><img src="../images/0000$n.jpeg"/></body></html>
EOF2
  done
  # p7 wide progressive, p8 odd-dim baseline, p9 malformed header.
  cat > "$IMG_STAGE/OEBPS/text/p7.xhtml" <<'EOF2'
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>p7</title></head>
<body><img src="../images/wide_prog.jpeg"/></body></html>
EOF2
  cat > "$IMG_STAGE/OEBPS/text/p8.xhtml" <<'EOF2'
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>p8</title></head>
<body><img src="../images/odd_dim.jpeg"/></body></html>
EOF2
  cat > "$IMG_STAGE/OEBPS/text/p9.xhtml" <<'EOF2'
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>p9</title></head>
<body><img src="../images/broken.jpg"/></body></html>
EOF2
  make_epub "$IMG_STAGE" images.epub -0
  rm -rf "$IMG_STAGE"
else
  echo "note: fixtures/images/0000*.jpeg absent — testJpegFixtureParity will skip"
fi
make_epub "$BUILD_DIR/fixtures/ncxonly" ncxonly.epub -9
head -c 4096 /dev/zero > "$BUILD_DIR/fixtures/garbage.bin"
python3 ../fixtures/gen_omnibus.py "$BUILD_DIR/fixtures/omnibus.epub" 1700 >/dev/null

# --- build ------------------------------------------------------------------
# Default (internal): compile the vendored third_party expat/miniz/etc. With
# FREEINK_BOOK_EXTERNAL_EXPAT=1 the vendored-expat wrapper TUs compile to
# nothing and the XML_* symbols must come from the SYSTEM libexpat — the
# include path for third_party/expat is dropped so <expat.h> resolves to the
# system headers (mirrors the CrossPoint firmware consumption mode; verified
# by checking the linked XML_* symbols match the system expat).
# ProgressiveJpeg is an optional dep of the renderer: when its headers are
# reachable the full-fidelity progressive path compiles in (TJFB_HAVE_
# PROGRESSIVE_FULL); without it that dispatcher arm drops out and pollFn/
# pollUser go unused (-Werror). Always expose it here so the host suite
# covers the full decode path.
INCLUDES="-I../../include -I../../../../assets/ProgressiveJpeg/include -I../../third_party/miniz/include -I../../third_party/libunibreak -I../../third_party/pngle -I../../third_party/jpegdec/src -I../../third_party/stb -I../../../../font/FreeInkFont/include -I../../../../font/FreeInkFont/third_party/stb"
VENDOR_SRCS="miniz_impl miniz_cores_impl expat_xmlparse expat_xmlrole expat_xmltok unibreak_impl pngle_impl"
if [ -n "$FREEINK_BOOK_EXTERNAL_EXPAT" ]; then
  CC_FLAGS="-O1 -std=c99 -DFREEINK_BOOK_EXTERNAL_EXPAT=1 $INCLUDES"
  LD_LIBS="-lexpat"
  VENDOR_SRCS="miniz_impl miniz_cores_impl unibreak_impl pngle_impl"
else
  CC_FLAGS="-O1 -std=c99 $INCLUDES"
  LD_LIBS=""
  VENDOR_SRCS="miniz_impl miniz_cores_impl expat_xmlparse expat_xmlrole expat_xmltok unibreak_impl pngle_impl"
  INCLUDES="$INCLUDES -I../../third_party/expat"
fi
for src in $VENDOR_SRCS; do
  cc $CC_FLAGS -c "../../src/vendor/$src.c" -o "$BUILD_DIR/obj/$src.o"
done
# JPEGDEC is C++ — the vendored wrapper compiles it into the vendor object set
# for every configuration (all JPEG paths in ImageRenderer run through it).
# Vendored upstream sources are compiled warning-free-exempt (-w): upstream's
# own Makefile does not build with -Wextra/-Werror.
c++ -std=c++17 -w $INCLUDES -c "../../src/vendor/jpegdec_impl.cpp" \
  -o "$BUILD_DIR/obj/jpegdec_impl.o"

CORE_SRCS="../../src/FreeInkBook.cpp ../../src/BookCatalog.cpp ../../src/epub/ZipCatalog.cpp ../../src/epub/XmlSax.cpp \
  ../../src/epub/PackageParsers.cpp ../../src/epub/ImageProbe.cpp ../../src/text/EntityFilter.cpp \
  ../../src/text/Hyphenator.cpp ../../src/css/Css.cpp ../../src/layout/ChapterLayout.cpp \
  ../../src/cache/PageCache.cpp ../../src/render/ImageRenderer.cpp ../../../../font/FreeInkFont/src/TtfFont.cpp ../../src/render/PageRenderer.cpp \
  ../../../../assets/ProgressiveJpeg/src/ProgressiveJpeg.cpp"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_freeinkbook.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_freeinkbook"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_layout.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_layout"

# The layout suite also builds and runs under the SMALL and LARGE profiles
# (BookProfile.h) so neither tier bit-rots.
c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_BOOK_SMALL=1 $INCLUDES \
  $CORE_SRCS test_layout.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_layout_small"

c++ -std=c++17 -Wall -Wextra -Werror -DFREEINK_BOOK_LARGE=1 $INCLUDES \
  $CORE_SRCS test_layout.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_layout_large"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_cache.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_cache"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_font.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_font"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_catalog.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_catalog"

c++ -std=c++17 -Wall -Wextra -Werror $INCLUDES \
  $CORE_SRCS test_deflate_robustness.cpp "$BUILD_DIR"/obj/*.o $LD_LIBS \
  -o "$BUILD_DIR/test_deflate_robustness"

python3 ../../tools/hyphc.py ../../third_party/hyphen-patterns/hyph-en-us.pat.txt \
  "$BUILD_DIR/hyph-en-us.fibh"
python3 ../../tools/hyphc.py ../fixtures/hyph-test-ru.pat.txt \
  "$BUILD_DIR/hyph-test-ru.fibh"

mkdir -p "$BUILD_DIR/cache"
"$BUILD_DIR/test_freeinkbook" "$BUILD_DIR/fixtures"
"$BUILD_DIR/test_layout" "$BUILD_DIR/fixtures" "$BUILD_DIR/hyph-en-us.fibh" "$BUILD_DIR/hyph-test-ru.fibh"
"$BUILD_DIR/test_layout_small" "$BUILD_DIR/fixtures" "$BUILD_DIR/hyph-en-us.fibh" "$BUILD_DIR/hyph-test-ru.fibh"
"$BUILD_DIR/test_layout_large" "$BUILD_DIR/fixtures" "$BUILD_DIR/hyph-en-us.fibh" "$BUILD_DIR/hyph-test-ru.fibh"
"$BUILD_DIR/test_cache" "$BUILD_DIR/fixtures" "$BUILD_DIR/cache" "$BUILD_DIR/hyph-en-us.fibh"
"$BUILD_DIR/test_font" "$BUILD_DIR/fixtures" ../fixtures/fonts/DejaVuSans.ttf
"$BUILD_DIR/test_catalog" "$BUILD_DIR/fixtures" "$BUILD_DIR/cache"
"$BUILD_DIR/test_deflate_robustness" "$BUILD_DIR/fixtures"
