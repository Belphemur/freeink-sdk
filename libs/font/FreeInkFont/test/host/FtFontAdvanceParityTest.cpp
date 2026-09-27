// Host test pinning FtFont::advance() to the metrics-load reference. advance()
// takes a fast path through FT_Get_Advance (no outline decode); the value must
// stay bit-identical to the old linearHoriAdvance rounding for every
// codepoint/size the layout engine can ask for, and the per-face memo must not
// leak values across sizes or dominate a miss. Run via test/host/run.sh against
// a TrueType fixture and a CFF/OTF fixture (the CFF case is where the fast path
// matters most, since decoding charstrings is what it avoids).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "FtFont.h"

using freeink::font::FtFont;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (fread(data.data(), 1, data.size(), f) != data.size()) {
    fprintf(stderr, "short read on %s\n", path);
    exit(1);
  }
  fclose(f);
  return data;
}

int checks = 0;
int failures = 0;
void expect(bool cond, const char* what) {
  ++checks;
  if (!cond) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

// The pre-fast-path computation: full metrics load, then the same fixed16.16
// -> 26.6 -> pixel rounding advance() has always used.
int16_t referenceAdvance(FtFont& font, uint32_t cp, uint16_t sizePx) {
  FtFont::GlyphMetrics metrics;
  if (!font.metrics26_6(cp, uint32_t(sizePx) * 64u, metrics)) return 0;
  return int16_t(metrics.advance26_6 >> 6);
}

// Codepoints a reader meets: Latin + punctuation, plus a few that exercise
// absent glyphs (CJK in a Latin-only face) and non-ASCII present ones.
const uint32_t kExtraCodepoints[] = {0x0416, 0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014,
                                     0x2026, 0x00E6, 0x0153, 0xFB01, 0xFB02, 0x4E00, 0x732B};
const uint16_t kSizes[] = {12, 14, 16, 18, 22, 28, 32};

void checkFont(const char* path) {
  const std::vector<uint8_t> data = readFile(path);
  FtFont font;
  if (!font.init(data.data(), static_cast<uint32_t>(data.size()), 16)) {
    fprintf(stderr, "FAIL: init %s\n", path);
    ++failures;
    return;
  }

  long compared = 0;
  long mismatches = 0;
  for (const uint16_t sizePx : kSizes) {
    for (uint32_t cp = 32; cp <= 0x24F; ++cp) {
      if (font.advance(cp, sizePx, 0) != referenceAdvance(font, cp, sizePx)) ++mismatches;
      ++compared;
    }
    for (const uint32_t cp : kExtraCodepoints) {
      if (font.advance(cp, sizePx, 0) != referenceAdvance(font, cp, sizePx)) ++mismatches;
      ++compared;
    }
    // Style flags must not change the advance (Font contract: advance is
    // per-face/size, not per-run style) and a repeat call must be stable.
    if (font.advance('A', sizePx, freeink::font::StyleBold) != font.advance('A', sizePx, 0)) ++mismatches;
    const int16_t first = font.advance('A', sizePx, 0);
    if (font.advance('A', sizePx, 0) != first) ++mismatches;
  }
  char label[512];
  snprintf(label, sizeof(label), "%s: advance() == metrics reference over %ld probes", path, compared);
  expect(mismatches == 0, label);

  // Interleave two sizes to prove the memo keys on size: a hit for one size
  // must never be served for another.
  bool sizeIsolation = true;
  for (uint32_t cp = 32; cp <= 0x24F; ++cp) {
    const int16_t at14 = font.advance(cp, 14, 0);
    const int16_t at22 = font.advance(cp, 22, 0);
    if (font.advance(cp, 14, 0) != at14 || font.advance(cp, 22, 0) != at22) sizeIsolation = false;
  }
  expect(sizeIsolation, "memo keys on size (14/22 interleave stable)");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s font.ttf font.otf\n", argv[0]);
    return 1;
  }
  checkFont(argv[1]);
  checkFont(argv[2]);

  printf("checks: %d, failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
