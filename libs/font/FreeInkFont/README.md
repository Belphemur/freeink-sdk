# FreeInkFont

A standalone TTF/OTF font engine for e-paper firmware: real advances, kerning,
ligatures, per-codepoint fallback, and on-demand glyph rasterization to 8-bit
alpha bitmaps — **decoupled from any layout or book engine**. Depend on this
library alone to render fonts; you do not need the EPUB engine.

It is freestanding C++17 with no Arduino/ESP-IDF dependency and a strict no-heap
rule: the font file bytes are *borrowed* and all cache memory comes from a
caller-sized arena.

## API

- **`freeink::font::Font`** — metrics only: `advance()`, `lineHeight()`,
  `ascent()`, `kerning()`, `ligature()`, `covers()`. A layout pass can run
  host-side against this with no font files at all.
- **`freeink::font::RasterFont : Font`** — adds `hasGlyph()` and
  `rasterize(codepoint, sizePx) → const GlyphBitmap*` (8-bit coverage +
  bearings + advance).
- **`freeink::font::TtfFont : RasterFont`** — the stb_truetype backend.
  `init(const uint8_t* data, uint32_t len, Arena& glyphArena)`; the data is
  borrowed (PSRAM / mmap / arena-loaded SD file).
- **`freeink::font::FontChain : Font`** — up to 8 faces with per-codepoint,
  per-style fallback (user → Latin → CJK) so mixed scripts don't render tofu.
- **`freeink::font::Arena`** — the bump allocator backing glyph/advance caches.
- **`freeink::font::GlyphBitmap`** — `{pixels(w*h, 8-bit), width, height, xoff,
  yoff, advance}`.

Cache sizes are tunable via `-DFREEINK_FONT_ADVANCE_SLOTS` /
`-DFREEINK_FONT_GLYPH_SLOTS` (defaults 512 / 128).

## Using it from a third-party renderer (e.g. CrossPoint)

The whole point: adopt runtime TTF **without** replacing your existing text
layout. Add this one library, then bridge your renderer's font call-sites to
`RasterFont`:

```cpp
#include <TtfFont.h>          // FreeInkFont
using namespace freeink::font;

static uint8_t glyphArenaBuf[48 * 1024];   // ~32-64 KB per active size
Arena glyphArena(glyphArenaBuf, sizeof glyphArenaBuf);

TtfFont face;
face.init(ttfBytesInPsram, ttfLen, glyphArena);   // bytes borrowed

// measure:
int w = face.advance(cp, sizePx, StyleNone);
// draw:
if (const GlyphBitmap* g = face.rasterize(cp, sizePx)) {
  blit(g->pixels, g->width, g->height, penX + g->xoff, baseline + g->yoff);
}
```

Your pagination, line-breaking, and page cache stay exactly as they are — you
swap only the font backend (e.g. from a pre-rasterized bitmap format to live
outlines).

## Backends: stb_truetype and FreeType

- **`TtfFont`** — stb_truetype. Small, no extra deps; renders a font's default
  master only (no variable-font axes).
- **`FtFont`** — FreeType (git submodule `third_party/freetype`, official
  `github.com/freetype/freetype` mirror, pinned to a `VER-2-13-3`-style tag).
  The curated build (module list and options) lives in
  `freetype-config/include/`, which shadows FreeType's own
  `include/freetype/config/` — do not edit headers inside the submodule. Reads
  OpenType **variable-font axes** (real bold from the `wght` axis,
  real/oblique italic), streams large CJK faces, and does GPOS/kerning. Use
  this for variable fonts, multi-weight families, or CJK on constrained RAM.
  Both implement the same `RasterFont` interface, so consumers pick a backend
  without other changes.

### FreeType attribution (FTL)

FreeType (https://freetype.org) is used under the **FreeType License (FTL)** —
see `third_party/freetype/docs/FTL.TXT` in the submodule. Per the FTL, products
that include this library must credit FreeType in their documentation:

> Portions of this software are copyright © The FreeType Project
> (www.freetype.org). All rights reserved.

## CJK / large fonts

stb_truetype needs the whole font file in RAM, which a no-PSRAM device can't do
for a multi-megabyte CJK face. A **FreeType** backend (streaming table access) is
the upgrade path and slots in behind the same `RasterFont` interface as a second
implementation alongside `TtfFont` — no API change for consumers. This library
ships the stb_truetype backend first.

## Relationship to FreeInkBook

This engine used to live inside FreeInkBook. It was extracted here unchanged;
FreeInkBook now re-exports the types under their historical names
(`freeink::book::BookFont == Font`, `RenderFont == RasterFont`, `TtfFont`,
`FontChain`, `Arena`) via thin alias headers, so its layout code and its full
host-test suite are unaffected.
