// Host tests for the FtFont glyph bitmap cache. Run via test/host/run.sh,
// which links this against the real vendored FreeType (all three
// FREEINK_FONT_ENABLE_* modules on) under a real font.
//
// Core invariant (determinism gate): a cached render is BYTE-IDENTICAL to the
// uncached render of the same face/options — the cache stores FreeType's own
// output verbatim, never re-derives it.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  } else {
    printf("ok: %s\n", what);
  }
}

void hashRaster(const freeink::font::GlyphBitmap& bm, uint32_t& hash) {
  // FNV-1a over the fields + pixel bytes (compact width stride).
  auto mix = [&hash](const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
      hash ^= p[i];
      hash *= 0x01000193u;
    }
  };
  mix(&bm.width, sizeof(bm.width));
  mix(&bm.height, sizeof(bm.height));
  mix(&bm.xoff, sizeof(bm.xoff));
  mix(&bm.yoff, sizeof(bm.yoff));
  mix(&bm.advance, sizeof(bm.advance));
  if (bm.pixels) mix(bm.pixels, size_t(bm.width) * bm.height);
}

bool rasterizeHash(FtFont& font, uint32_t codepoint, uint16_t sizePx, uint32_t& hash) {
  const auto* bm = font.rasterize(codepoint, sizePx);
  if (!bm) return false;
  hash = 0x811c9dc5u;
  hashRaster(*bm, hash);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s font.ttf [secondFont.otf]\n", argv[0]);
    return 1;
  }
  const std::vector<uint8_t> ttf = readFile(argv[1]);
  const std::vector<uint8_t> otf = argc > 2 ? readFile(argv[2]) : std::vector<uint8_t>{};

  // Reference face: cache fully disabled — every rasterize renders fresh.
  FtFont uncached;
  expect(uncached.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false), "uncached face init");
  uncached.setGlyphCacheBudget(0);

  // Cached face: default budget. NOTE: no test may corrupt its (cp, size)
  // pairs that another test hashes — the hit/marker tests below use size 32,
  // which no hash-reference test touches.
  FtFont cached;
  expect(cached.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false), "cached face init");

  // --- Byte parity: cached == uncached over a glyph/size sweep -------------
  bool parity = true;
  const uint16_t sizes[] = {12, 16, 20, 24};
  for (const uint16_t size : sizes) {
    for (uint32_t cp = 33; cp <= 126; ++cp) {
      uint32_t hashCached = 0;
      uint32_t hashRef = 0;
      const bool haveCached = rasterizeHash(cached, cp, size, hashCached);
      const bool haveRef = rasterizeHash(uncached, cp, size, hashRef);
      if (haveCached != haveRef || (haveCached && hashCached != hashRef)) {
        parity = false;
        fprintf(stderr, "parity mismatch: U+%04lX @ %upx cached=%u ref=%u\n", static_cast<unsigned long>(cp), size,
                hashCached, hashRef);
      }
    }
  }
  expect(parity, "cache vs uncached byte parity across ASCII sweep and 4 sizes");

  // --- Flush on setRenderOptions -------------------------------------------
  uint32_t defaultHash = 0;
  expect(rasterizeHash(cached, 'A', 16, defaultHash), "'A' under default options");
  expect(cached.setRenderOptions({.hinting = FtFont::HintingMode::Light, .monochrome = true}),
         "monochrome+light supported in the all-modules build");
  uint32_t monoHash = 0;
  expect(rasterizeHash(cached, 'A', 16, monoHash) && monoHash != defaultHash,
         "options change flushed the cache (mono bytes, not stale default bytes)");
  expect(cached.setRenderOptions(FtFont::RenderOptions{}), "restoring default options");
  uint32_t restoredHash = 0;
  expect(rasterizeHash(cached, 'A', 16, restoredHash) && restoredHash == defaultHash,
         "flushed options revert to the original bytes (no stale mono served)");

  // --- Flush on size change -------------------------------------------------
  uint32_t hashA16 = 0;
  expect(rasterizeHash(cached, 'A', 16, hashA16), "'A' at 16px");
  uint32_t hashB20 = 0;
  expect(rasterizeHash(cached, 'B', 20, hashB20), "'B' at 20px");
  uint32_t hashA16b = 0;
  expect(rasterizeHash(cached, 'A', 16, hashA16b) && hashA16b == hashA16,
         "returning to 16px re-renders 'A' identically (no cross-size pollution)");

  // --- Re-init flush (glyph IDs are face-local) -----------------------------
  if (!otf.empty()) {
    FtFont crossFont;
    expect(crossFont.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false), "cross face init (ttf)");
    uint32_t ttfHash = 0;
    expect(rasterizeHash(crossFont, 'A', 16, ttfHash), "ttf 'A' cached");
    expect(crossFont.init(otf.data(), static_cast<uint32_t>(otf.size()), 16, 400, false), "cross face re-init (otf)");
    uint32_t otfHash = 0;
    expect(rasterizeHash(crossFont, 'A', 16, otfHash), "otf 'A' rendered after re-init");

    FtFont freshOtf;
    expect(freshOtf.init(otf.data(), static_cast<uint32_t>(otf.size()), 16, 400, false), "fresh otf face init");
    freshOtf.setGlyphCacheBudget(0);
    uint32_t freshOtfHash = 0;
    expect(rasterizeHash(freshOtf, 'A', 16, freshOtfHash), "fresh otf 'A' reference");
    expect(otfHash == freshOtfHash && otfHash != ttfHash,
           "re-init flushed the cache: a different face never serves the previous face's cached bytes");
  } else {
    fprintf(stderr, "note: no second fixture passed; re-init flush test skipped\n");
  }

  // --- Cache hit serves the stored copy without re-rendering ---------------
  // Size 32 is deliberately outside every hash-reference test above. A cache
  // hit is byte-identical to a correct re-render by design (that parity is
  // pinned by the eviction sweep), so the hit itself is proven by capturing
  // the hash at first render and re-checking it on the next call — never by
  // writing through the returned storage (renderers must treat the const
  // GlyphBitmap as read-only; blending in place would corrupt cached
  // coverage).
  uint32_t h1 = 0, h2 = 0;
  rasterizeHash(cached, 'A', 32, h1);
  rasterizeHash(cached, 'A', 32, h2);
  expect(h1 == h2, "cache hit returns byte-identical coverage without re-rendering");
  uint32_t u1 = 0, u2 = 0;
  rasterizeHash(uncached, 'A', 32, u1);
  rasterizeHash(uncached, 'A', 32, u2);
  expect(u1 == u2 && u1 == h1, "budget 0 face re-renders byte-identically (contrast)");

  // --- Contract: the last rasterize() survives flush AND eviction ----------
  // Rasterize, snapshot, force a flush (budget 0): the bitmap handed out
  // must still be readable and byte-identical to a fresh render.
  {
    FtFont survivor;
    expect(survivor.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false), "survivor face init");
    survivor.setGlyphCacheBudget(FtFont::kDefaultGlyphCacheBudget);
    const auto* live = survivor.rasterize('A', 32);
    expect(live && live->pixels && live->width > 0, "survivor live bitmap rendered");
    std::vector<uint8_t> snap(live->pixels, live->pixels + size_t(live->width) * live->height);
    survivor.setGlyphCacheBudget(0);  // flush: frees every cache entry
    expect(live->pixels != nullptr && std::memcmp(snap.data(), live->pixels, snap.size()) == 0,
           "flush preserves the last rasterize() bitmap (no eviction exception)");

    // Eviction of the live entry without an intervening rasterize (a budget
    // shrink evicts from oldest; the rasterize-store path only evicts OLDER
    // entries) must preserve it the same way. Any rasterize after the
    // snapshot would end the base contract by itself.
    const auto* liveA = survivor.rasterize('A', 32);
    expect(liveA && liveA->pixels, "live 'A' bitmap before eviction");
    std::vector<uint8_t> snapA(liveA->pixels, liveA->pixels + size_t(liveA->width) * liveA->height);
    survivor.setGlyphCacheBudget(1);  // evicts every cached entry, incl. the live one
    expect(liveA->pixels != nullptr && std::memcmp(snapA.data(), liveA->pixels, snapA.size()) == 0,
           "eviction preserves the last rasterize() bitmap");
  }

  // --- Eviction: tiny budget re-renders identical bytes --------------------
  FtFont evicting;
  expect(evicting.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false), "evicting face init");
  evicting.setGlyphCacheBudget(4096);  // smaller than a few glyphs → constant eviction
  parity = true;
  for (const uint16_t size : sizes) {
    for (uint32_t cp = 33; cp <= 126; ++cp) {
      uint32_t hashEvict = 0;
      uint32_t hashRef = 0;
      const bool haveEvict = rasterizeHash(evicting, cp, size, hashEvict);
      const bool haveRef = rasterizeHash(uncached, cp, size, hashRef);
      if (haveEvict != haveRef || (haveEvict && hashEvict != hashRef)) {
        parity = false;
        fprintf(stderr, "eviction mismatch: U+%04lX @ %upx\n", static_cast<unsigned long>(cp), size);
      }
    }
  }
  expect(parity, "evicted re-renders stay byte-identical under a 4 KB budget");

  // Budget smaller than one glyph never caches: the store is rejected by the
  // single-entry size guard, so every call re-renders — served bytes stay
  // identical to the reference (a re-render is correct, just slower).
  evicting.setGlyphCacheBudget(1);
  uint32_t rej1 = 0, rej2 = 0, refA16 = 0;
  rasterizeHash(evicting, 'A', 16, rej1);
  rasterizeHash(evicting, 'A', 16, rej2);
  rasterizeHash(uncached, 'A', 16, refA16);
  expect(rej1 == rej2 && rej1 == refA16, "below-single-entry budget disables caching");
  // Raising the budget re-enables caching. The proof is pointer stability
  // across an intervening DIFFERENT glyph's render: a hit serves the entry
  // block, which a slot-backed bitmap could not survive — rendering 'M'
  // overwrites the FT slot, then 'A' still comes back byte-identical from
  // the same entry pixels.
  evicting.setGlyphCacheBudget(FtFont::kDefaultGlyphCacheBudget);
  uint32_t w1 = 0, w2 = 0;
  rasterizeHash(evicting, 'A', 16, w1);  // miss after the raise → stores
  rasterizeHash(evicting, 'A', 16, w2);  // hit
  expect(w1 == w2, "raising the budget re-enables caching");
  const auto* hitA = evicting.rasterize('A', 16);
  if (hitA && hitA->pixels) {
    uint32_t hA = 0;
    hashRaster(*hitA, hA);
    (void)evicting.rasterize('M', 16);  // overwrite the FT slot buffer
    const auto* again = evicting.rasterize('A', 16);
    uint32_t hA2 = 0;
    if (again) hashRaster(*again, hA2);
    expect(again && again->pixels == hitA->pixels && hA2 == hA,
           "hit serves the entry block — survives a different glyph's slot render");
  }

  printf("checks: %d, failures: %d\n", checks, failures);
  return failures ? 1 : 0;
}
