// Host-side tests for the FreeType-backed FtFont, focused on the
// metrics-only glyphBounds override: box containment vs rasterize()
// across styles (regular, faux bold, faux/synthesized italic) and sizes
// (exercising ensureSize through the const path), plus failure modes
// (unready face, unmapped codepoint). Per-backend invariants only — no
// cross-backend metric equality (stb and FreeType differ by hinting).

#include <FtFont.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checksRun;                                                           \
    if (!(cond)) {                                                         \
      ++checksFailed;                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

// Containment: the glyphBounds box [bx, bx+bw) x [by, by+bh) must cover the
// rasterized bitmap box [rx, rx+rw) x [ry, ry+rh) (y grows downward, offsets
// relative to the pen baseline).
bool contains(int16_t bx, int16_t by, uint16_t bw, uint16_t bh, int16_t rx, int16_t ry, uint16_t rw,
              uint16_t rh) {
  return bx <= rx && by <= ry && (bx + static_cast<int>(bw)) >= (rx + static_cast<int>(rw)) &&
         (by + static_cast<int>(bh)) >= (ry + static_cast<int>(rh));
}

bool loadFile(const char* path, uint8_t** out, uint32_t* len) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
  if (buf == nullptr) {
    std::fclose(f);
    return false;
  }
  const size_t read = std::fread(buf, 1, static_cast<size_t>(size), f);
  std::fclose(f);
  *out = buf;
  *len = static_cast<uint32_t>(read);
  return read == static_cast<size_t>(size);
}

// Core per-glyph invariant sweep: glyphBounds must succeed where rasterize
// does, and its box must contain the rasterized bitmap box.
void sweepContainment(freeink::font::FtFont& font, uint16_t sizePx) {
  static const uint32_t kCodepoints[] = {'A', 'a', 'g', 'y', 'M', 'W', 'j', '@', '0', 0x00E9 /*é*/,
                                         0x2019 /*’*/, 0x20AC /*€*/};
  for (const uint32_t cp : kCodepoints) {
    int16_t bx = 0;
    int16_t by = 0;
    uint16_t bw = 0;
    uint16_t bh = 0;
    const bool haveBounds = font.glyphBounds(cp, sizePx, bx, by, bw, bh);
    const freeink::font::GlyphBitmap* bmp = font.rasterize(cp, sizePx);
    CHECK(haveBounds == (bmp != nullptr));
    if (!haveBounds || bmp == nullptr) continue;
    CHECK(bw > 0);
    CHECK(bh > 0);
    CHECK(contains(bx, by, bw, bh, bmp->xoff, bmp->yoff, bmp->width, bmp->height));
    // Bitmap must fit inside the reported box, never exceed it.
    CHECK(bmp->width <= bw);
    CHECK(bmp->height <= bh);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures-dir>\n", argv[0]);
    return 2;
  }

  char path[512];
  std::snprintf(path, sizeof(path), "%s/fonts/DejaVuSans.ttf", argv[1]);
  uint8_t* data = nullptr;
  uint32_t len = 0;
  if (!loadFile(path, &data, &len)) {
    std::fprintf(stderr, "cannot load fixture %s\n", path);
    return 2;
  }

  using freeink::font::FtFont;

  // --- failure modes ---------------------------------------------------------
  {
    FtFont font;
    int16_t x = 0;
    int16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    CHECK(!font.glyphBounds('A', 16, x, y, w, h));  // unready face

    CHECK(font.init(data, len, 16));
    CHECK(!font.glyphBounds(0x10FFFF /*beyond all planes, never mapped*/, 16, x, y, w, h));
    CHECK(!font.glyphBounds(0, 16, x, y, w, h));  // codepoint 0 is never mapped
  }

  // --- regular style: containment at several sizes (ensureSize paths) --------
  {
    FtFont font;
    CHECK(font.init(data, len, 12, 400, false));
    sweepContainment(font, 12);
    sweepContainment(font, 16);  // size change through the const glyphBounds path
    sweepContainment(font, 24);
    sweepContainment(font, 16);  // and back down
  }

  // --- faux bold (static face, no wght axis): embolden-grown box --------------
  {
    FtFont font;
    CHECK(font.init(data, len, 16, 700, false));
    sweepContainment(font, 16);
    sweepContainment(font, 32);
    // NOTE: FtFont's faux-bold advance() equals the regular advance (outline
    // embolden affects only rasterize) — existing SDK behavior, asserted
    // nowhere here; the glyphBounds pad covers the grown bitmap either way.
  }

  // --- faux italic (no ital/slnt axis): sheared box ---------------------------
  {
    FtFont font;
    CHECK(font.init(data, len, 16, 400, true));
    sweepContainment(font, 16);
    // A sheared upright glyph gains ink to the right of the regular's box.
    FtFont regular;
    CHECK(regular.init(data, len, 16, 400, false));
    int16_t bx = 0;
    int16_t by = 0;
    uint16_t bw = 0;
    uint16_t bh = 0;
    int16_t rx = 0;
    int16_t ry = 0;
    uint16_t rw = 0;
    uint16_t rh = 0;
    CHECK(font.glyphBounds('M', 16, bx, by, bw, bh));
    CHECK(regular.glyphBounds('M', 16, rx, ry, rw, rh));
    CHECK(bw > rw);  // shear widens the box
  }

  // --- two faces over the same borrowed bytes ---------------------------------
  {
    FtFont a;
    FtFont b;
    CHECK(a.init(data, len, 12));
    CHECK(b.init(data, len, 24));
    int16_t x = 0;
    int16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    CHECK(a.glyphBounds('g', 12, x, y, w, h));
    CHECK(h < 24);
    CHECK(b.glyphBounds('g', 24, x, y, w, h));
    CHECK(h > 12);
  }

  // --- rejection of garbage faces ----------------------------------------------
  {
    FtFont font;
    CHECK(!font.init(nullptr, 0, 16));
    CHECK(!font.init(data, 16, 16));  // truncated to a non-face prefix
    int16_t x = 0;
    int16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    CHECK(!font.glyphBounds('A', 16, x, y, w, h));
  }

  std::free(data);
  std::printf("%d checks, %d failures\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
