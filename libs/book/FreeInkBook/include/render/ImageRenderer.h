#pragma once

// FreeInk SDK — streaming image rendering for FreeInkBook (Phase 4b).
//
// Decodes a container image (PNG via pngle, JPEG via TJpgDec) and delivers
// it as 8-bit grayscale rows already scaled (nearest-neighbor, no upscaling
// beyond the requested placement) to the caller, top to bottom. The full
// image never exists in memory: PNG decodes scanline-by-scanline, JPEG in
// MCU bands, and each finished source row maps to zero or more output rows.
//
// E-paper targets then quantize each row: ditherRowOrdered() gives 1-bit
// output via a Bayer 4×4 matrix; grayscale panels can consume the rows
// directly.
//
// Interlaced PNGs are rejected (BookStatus::Unsupported) — their pixels
// arrive out of order and would require a full-image buffer.

#include <stdint.h>

#include "BookArena.h"
#include "BookStorage.h"
#include "BookTypes.h"
#include "epub/ZipCatalog.h"
#include "layout/ChapterLayout.h"

namespace freeink {
namespace book {

// Return false to stop rendering early. `gray` holds `width` pixels
// (0 = black, 255 = white) and is valid only during the call.
using ImageRowFn = bool (*)(void* user, uint16_t y, const uint8_t* gray, uint16_t width);

// Reports the streamed source-row dimensions (one call, before the first
// row). Return false to stop rendering early.
using SourceDimsFn = bool (*)(void* user, uint16_t srcW, uint16_t srcH);

// Cooperative abort for a decode that cannot surface progress through the
// row callback (the full-fidelity progressive path holds its whole
// coefficient store until the last scan; the app's worker polls between
// scans and per emitted block row). Return true = stop the decode.
using RenderPollFn = bool (*)(void* user);

class ImageRenderer {
 public:
  // Renders `image` (a placement produced by layout or read from the page
  // cache) at its recorded size. Decode state comes from `scratch` and is
  // released before returning; pngle additionally uses bounded transient
  // heap for its scanline buffer.
  static BookStatus render(BookSource& source, const ZipCatalog& zip, const PageImage& image,
                           Arena& scratch, ImageRowFn rowFn, void* user);

  // Pass-through mode for callers that do their own rescaling (TTF reader's
  // app-side DPID scaler, design docs/design/2026-11-09-ttf-image-dpid-clahe.md):
  // streams the DECODED, UNSCALED grayscale source rows of the PNG/JPEG at
  // `href` to `rowFn` (rows are `srcW` wide). Progressive JPEGs decode at
  // FULL resolution through libs/assets/ProgressiveJpeg (all scans; the
  // coefficient store is ~2 B/pixel on the app's PSRAM budget) with the
  // DC-only block-average renderer as the out-of-memory fallback so an
  // oversized store cannot stall the placement. The streamed dims are
  // reported once via `dimsFn` before the first row — baseline JPEG's
  // power-of-two pre-scale is picked from `dstW`/`dstH` exactly as `render`
  // would pick it. No averaging or interpolation runs here and `render`'s
  // default behavior is unchanged.
  static BookStatus renderSourceRows(BookSource& source, const ZipCatalog& zip, const char* href, uint16_t dstW,
                                     uint16_t dstH, Arena& scratch, SourceDimsFn dimsFn, void* dimsUser,
                                     ImageRowFn rowFn, void* user);
  // Same, plus a cooperative abort hook (RenderPollFn) that reaches decodes
  // whose progress cannot surface through rowFn — today the full-fidelity
  // progressive path (no rows until the last scan). Other decoders stream
  // per-row and ignore it.
  static BookStatus renderSourceRows(BookSource& source, const ZipCatalog& zip, const char* href, uint16_t dstW,
                                     uint16_t dstH, Arena& scratch, SourceDimsFn dimsFn, void* dimsUser,
                                     ImageRowFn rowFn, void* user, RenderPollFn pollFn, void* pollUser);
};

// Quantizes one grayscale row to 1 bit per pixel (MSB first, 1 = black)
// with ordered Bayer 4×4 dithering. `outBits` needs (width+7)/8 bytes.
void ditherRowOrdered(const uint8_t* gray, uint16_t width, uint16_t y, uint8_t* outBits);

}  // namespace book
}  // namespace freeink
