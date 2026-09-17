#pragma once

// FreeInk SDK — FreeType-backed font engine (FreeInkFont).
//
// FtFont implements RasterFont over FreeType. Unlike the stb_truetype backend
// (TtfFont), it reads OpenType variable-font axes, so a single variable file
// yields real weights (bold = wght axis) and, where present, a real italic/slant
// axis — and it can synthesize oblique italic and emboldening when an axis is
// absent. It also streams large CJK faces (FreeType maps tables on demand), which
// stb_truetype cannot on a no-PSRAM device.
//
// One FtFont instance = one styled face at one pixel size: pick the design
// `weight` (e.g. 400 regular / 700 bold) and `italic` at init. A font FAMILY is
// several FtFonts over the SAME borrowed file bytes at different coordinates,
// surfaced to a renderer as regular/bold/italic/bold-italic.
//
// Memory: the font file bytes are BORROWED (PSRAM / resident buffer) and must
// outlive the FtFont. FreeType owns the glyph slot, so rasterize() returns a
// bitmap valid until the next rasterize() on this face (the RasterFont contract);
// no external glyph arena is needed. FreeType's own allocations are routed to
// PSRAM (when present) via a custom FT_Memory — see ensureLib() in FtFont.cpp
// and FontAlloc.h.

#include <stdint.h>

#include "Font.h"

// Opaque FreeType handles kept out of the public header.
typedef struct FT_LibraryRec_* FtLibraryHandle;
typedef struct FT_FaceRec_* FtFaceHandle;

namespace freeink {
namespace font {

class FtFont : public RasterFont {
 public:
  FtFont() = default;
  ~FtFont();
  FtFont(const FtFont&) = delete;
  FtFont& operator=(const FtFont&) = delete;

  // Borrow `data` (must outlive this face). `weight` is the design weight applied
  // to the font's `wght` axis if it has one (clamped to the axis range); ignored
  // on a static face. `italic`: use a real ital/slnt axis if present, else apply
  // an oblique shear. Returns false if FreeType can't parse the face.
  bool init(const uint8_t* data, uint32_t len, uint16_t sizePx, int weight = 400, bool italic = false);

  // Streamed variant: instead of holding the whole file in RAM, FreeType pulls
  // bytes on demand through `read` (absolute offset). `fileSize` is the total
  // length; `ctx` is passed back to `read` and, along with whatever it wraps
  // (e.g. an open SD file), must outlive this face. This keeps only the tables
  // and glyphs actually used resident — essential for multi-MB variable/CJK
  // fonts. The source is NOT closed on destruction (the caller owns it).
  using ReadFn = unsigned long (*)(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  bool initStream(ReadFn read, void* ctx, unsigned long fileSize, uint16_t sizePx, int weight = 400,
                  bool italic = false);

  bool ready() const { return ready_; }

  // Release the FreeType face (and any streamed source wrappers), returning the
  // object to the pre-init state. The borrowed file bytes / stream source are
  // NOT freed (the caller owns them). Safe to init()/initStream() again after —
  // lets a caller shed an idle face's FreeType memory and rebuild it on demand.
  void deinit();

  bool hasGlyph(uint32_t codepoint) const override;

  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override;
  int16_t lineHeight(uint16_t sizePx) override;
  int16_t ascent(uint16_t sizePx) override;
  int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) override;

  // Rasterizes one glyph to an 8-bit alpha GlyphBitmap. Valid until the next
  // rasterize() on this face (FreeType glyph-slot lifetime).
  const GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) override;

 private:
  void applyVariation(int weight, bool italic);
  void ensureSize(uint16_t sizePx);

  bool finishInit(uint16_t sizePx, int weight, bool italic);  // shared tail of init/initStream

  FtFaceHandle face_ = nullptr;
  void* stream_ = nullptr;     // FT_StreamRec* for the streamed path (owned)
  void* streamCtx_ = nullptr;  // {ReadFn, ctx} for the streamed path (owned)
  bool ready_ = false;
  bool obliqueShear_ = false;  // faux italic (no ital/slnt axis)
  bool emboldenBold_ = false;  // faux bold (static or no wght axis); per-glyph outline embolden
  uint16_t sizePx_ = 0;
  GlyphBitmap glyph_{};  // last rasterized glyph (points into the FT slot buffer)
};

}  // namespace font
}  // namespace freeink
