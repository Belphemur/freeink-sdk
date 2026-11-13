// FreeInkBook — streaming PNG/JPEG decode → scaled grayscale rows.

#include "render/ImageRenderer.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <pngle.h>

// JPEG front end: JPEGDEC (third_party/jpegdec — a git submodule pinned to
// upstream tag 1.8.5; provenance in the SDK .gitmodules). TJpgDec was
// removed — every JPEG path (baseline full decode, progressive DC-only
// fallback) runs through JPEGDEC; the full-fidelity progressive decode stays
// in libs/assets/ProgressiveJpeg. A consumer that strips the vendored
// library loses JPEG rendering but the rest of the renderer still compiles.
#if __has_include("JPEGDEC.h")
// JPEGDEC's host builds select the stdlib branch with -D__LINUX__ (its own
// Makefile does the same); without it the header wants Arduino.h.
#if !defined(ARDUINO) && !defined(__MACH__) && !defined(ESP_PLATFORM) && !defined(__LINUX__)
#define __LINUX__ 1
#endif
#include "JPEGDEC.h"
#define TJFB_HAVE_JPEGDEC 1
#endif

#include "epub/ImageProbe.h"
#if __has_include("ProgressiveJpeg.h")
#include "ProgressiveJpeg.h"  // full-fidelity progressive decode (optional dep)
#define TJFB_HAVE_PROGRESSIVE_FULL 1
#endif

// Logging: the firmware's Logging.h facility when the consumer provides it;
// silent fallback otherwise (same convention as InputManager).
#if __has_include(<Logging.h>)
#include <Logging.h>
#define PJ_LOG_WRN(...) LOG_WRN("PJPG", __VA_ARGS__)
#else
#define PJ_LOG_WRN(...) ((void)0)
#endif


namespace freeink {
namespace book {

namespace {

// Maps completed source rows to output rows with box-filter averaging: every
// source pixel contributes to exactly one output pixel, so downscales keep
// tonal detail instead of sampling every Nth pixel (nearest-neighbor's
// aliasing reads as blur/jaggies on photos).
struct RowScaler {
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  uint16_t dstW = 0;
  uint16_t dstH = 0;
  uint8_t* dstRow = nullptr;
  uint32_t* sums = nullptr;    // dstW accumulators
  uint16_t* counts = nullptr;  // dstW sample counts
  ImageRowFn rowFn = nullptr;
  void* user = nullptr;
  uint32_t nextDst = 0;
  bool stopped = false;
  bool failed = false;
  // renderSourceRows mode: no averaging, rows pass straight through.
  bool passThrough = false;

  void resetAccum() {
    memset(sums, 0, static_cast<size_t>(dstW) * sizeof(uint32_t));
    memset(counts, 0, static_cast<size_t>(dstW) * sizeof(uint16_t));
  }

  bool emitAveraged() {
    for (uint32_t x = 0; x < dstW; ++x) {
      dstRow[x] = counts[x] != 0 ? static_cast<uint8_t>(sums[x] / counts[x]) : 0xFF;
    }
    if (!rowFn(user, static_cast<uint16_t>(nextDst), dstRow, dstW)) {
      stopped = true;
      return false;
    }
    ++nextDst;
    resetAccum();
    return true;
  }

  // Feeds one completed source row; emits output rows as their source bands
  // complete.
  bool feedSourceRow(uint32_t srcY, const uint8_t* gray) {
    if (passThrough) return rowFn(user, static_cast<uint16_t>(srcY), gray, srcW);
    // Emit any output rows whose source band ended before this row.
    while (nextDst < dstH &&
           srcY >= static_cast<uint64_t>(nextDst + 1) * srcH / dstH) {
      if (!emitAveraged()) return false;
    }
    if (nextDst >= dstH) return true;
    for (uint32_t x = 0; x < srcW; ++x) {
      const uint32_t dx = static_cast<uint64_t>(x) * dstW / srcW;
      sums[dx] += gray[x];
      ++counts[dx];
    }
    // Last source row of the image: flush the final band.
    if (srcY + 1 == srcH) {
      while (nextDst < dstH) {
        if (!emitAveraged()) return false;
      }
    }
    return true;
  }
};

bool initScaler(RowScaler& scaler, Arena& scratch) {
  scaler.dstRow = static_cast<uint8_t*>(scratch.alloc(scaler.dstW, 1));
  scaler.sums = scratch.allocArray<uint32_t>(scaler.dstW);
  scaler.counts = scratch.allocArray<uint16_t>(scaler.dstW);
  if (scaler.dstRow == nullptr || scaler.sums == nullptr || scaler.counts == nullptr) {
    return false;
  }
  scaler.resetAccum();
  return true;
}

uint8_t rgbaToGray(const uint8_t rgba[4]) {
  const int32_t gray =
      (rgba[0] * 299 + rgba[1] * 587 + rgba[2] * 114) / 1000;
  // Blend over white (e-paper background).
  return static_cast<uint8_t>(255 - ((255 - gray) * rgba[3]) / 255);
}

// --- PNG ---------------------------------------------------------------------

struct PngState {
  RowScaler scaler;
  uint8_t* srcRow = nullptr;   // srcW grayscale accumulation
  uint32_t currentY = 0;
  bool unsupported = false;    // interlaced
  bool dimsSent = false;
  SourceDimsFn dimsFn = nullptr;
  void* dimsUser = nullptr;
};

void pngInitCallback(pngle_t* pngle, uint32_t w, uint32_t h) {
  PngState* state = static_cast<PngState*>(pngle_get_user_data(pngle));
  (void)w;
  (void)h;
  if (pngle_get_ihdr(pngle)->interlace != 0) state->unsupported = true;
  // Pass-through: report the streamed dims once, before the first row.
  if (state->scaler.passThrough && !state->dimsSent && !state->scaler.stopped) {
    state->dimsSent = true;
    if (state->dimsFn != nullptr &&
        !state->dimsFn(state->dimsUser, state->scaler.srcW, state->scaler.srcH, /*scaleReduced=*/false)) {
      state->scaler.stopped = true;
    }
  }
}

void pngDrawCallback(pngle_t* pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const uint8_t rgba[4]) {
  PngState* state = static_cast<PngState*>(pngle_get_user_data(pngle));
  if (state->unsupported || state->scaler.stopped) return;
  (void)w;
  (void)h;
  if (y != state->currentY) {
    // Source row finished (pngle emits strictly in raster order when
    // non-interlaced).
    if (!state->scaler.feedSourceRow(state->currentY, state->srcRow)) return;
    state->currentY = y;
  }
  if (x < state->scaler.srcW) state->srcRow[x] = rgbaToGray(rgba);
}

BookStatus renderPng(BookSource& source, const ZipEntry& entry, const PageImage& image,
                     uint16_t srcW, uint16_t srcH, Arena& scratch, ImageRowFn rowFn,
                     void* user, bool passThrough, SourceDimsFn dimsFn, void* dimsUser) {
  PngState state;
  state.scaler.srcW = srcW;
  state.scaler.srcH = srcH;
  state.scaler.dstW = image.width;
  state.scaler.dstH = image.height;
  state.scaler.rowFn = rowFn;
  state.scaler.user = user;
  state.scaler.passThrough = passThrough;
  state.dimsFn = dimsFn;
  state.dimsUser = dimsUser;
  state.srcRow = static_cast<uint8_t*>(scratch.alloc(srcW, 1));
  uint8_t* readBuf = static_cast<uint8_t*>(scratch.alloc(4096, 1));
  if (!initScaler(state.scaler, scratch) || state.srcRow == nullptr || readBuf == nullptr) {
    return BookStatus::OutOfMemory;
  }
  memset(state.srcRow, 0xFF, srcW);

  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;

  pngle_t* pngle = pngle_new();
  if (pngle == nullptr) return BookStatus::OutOfMemory;
  pngle_set_user_data(pngle, &state);
  pngle_set_init_callback(pngle, pngInitCallback);
  pngle_set_draw_callback(pngle, pngDrawCallback);

  status = BookStatus::Ok;
  for (;;) {
    const int32_t n = reader.read(readBuf, 4096);
    if (n < 0) {
      status = BookStatus::IoError;
      break;
    }
    if (n == 0) break;
    if (pngle_feed(pngle, readBuf, static_cast<size_t>(n)) < 0) {
      status = BookStatus::ParseError;
      break;
    }
    if (state.unsupported) {
      status = BookStatus::Unsupported;
      break;
    }
    if (state.scaler.stopped) break;
  }
  if (status == BookStatus::Ok && !state.scaler.stopped) {
    state.scaler.feedSourceRow(state.currentY, state.srcRow);  // flush last row
  }
  pngle_destroy(pngle);
  return status;
}

// --- JPEG --------------------------------------------------------------------

#if TJFB_HAVE_JPEGDEC

// JPEGDEC keeps its decode state (huffman/quant tables, MCU buffers, 2 KiB
// stream buffer) inside the JPEGDEC object — ~17 KB — and allocates nothing.
// That is far larger than anything the arena held for TJpgDec, so it lives on
// the heap (PSRAM-preferred on ESP32, same allocator pattern as
// ProgressiveJpeg's coefficient store), never on the caller arena: a caller
// arena too small for a decode band must still afford the decoder itself.
void* allocJpegDecMem(size_t bytes) {
#if __has_include(<esp_heap_caps.h>)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p != nullptr) return p;
  return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

void freeJpegDecMem(void* p) {
#if __has_include(<esp_heap_caps.h>)
  heap_caps_free(p);
#else
  free(p);
#endif
}

// RAII guard: the heap JPEGDEC object must be freed on every exit path.
struct JpegDecHandle {
  JPEGDEC* jpeg = nullptr;
  ~JpegDecHandle() {
    if (jpeg != nullptr) freeJpegDecMem(jpeg);
  }
};

struct JpegDecSource {
  ZipEntryReader* reader = nullptr;
  bool ioError = false;
};

int32_t jpegDecRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  auto* st = static_cast<JpegDecSource*>(pFile->fHandle);
  if (iLen <= 0) return 0;
  const int32_t n = st->reader->read(pBuf, static_cast<uint32_t>(iLen));
  if (n < 0) {
    st->ioError = true;
    return 0;
  }
  pFile->iPos += n;
  return n;
}

// JPEGDEC's header scan overflows its 2 KiB buffer past large marker
// segments (EXIF/ICC) and calls seek forward; stored entries are random
// access, deflated entries rewind the inflate state and re-walk.
int32_t jpegDecSeek(JPEGFILE* pFile, int32_t iPosition) {
  auto* st = static_cast<JpegDecSource*>(pFile->fHandle);
  if (iPosition < 0) iPosition = 0;
  if (iPosition >= pFile->iSize) iPosition = pFile->iSize - 1;
  if (!st->reader->seek(static_cast<uint32_t>(iPosition))) {
    st->ioError = true;
    return pFile->iPos;
  }
  pFile->iPos = iPosition;
  return iPosition;
}

void jpegDecClose(void*) {}

struct JpegState {
  RowScaler scaler;
  uint8_t* band = nullptr;      // srcW × kBandRows grayscale
  uint16_t bandW = 0;
  uint32_t bandTop = 0;         // source y of band row 0
  uint32_t bandFilled = 0;      // rows known to be complete (top-based)
  bool dimsSent = false;
  bool scaleReduced = false;  // arena forced a step below the preferred scale
  SourceDimsFn dimsFn = nullptr;
  void* dimsUser = nullptr;

  static constexpr uint32_t kBandRows = 16;

  // Pass-through: report the streamed dims once (baseline JPEG arrives here
  // with the post-pow-2 dims already known — the probe cannot predict them).
  void reportDims() {
    if (dimsSent || dimsFn == nullptr) return;
    dimsSent = true;
    if (!dimsFn(dimsUser, scaler.srcW, scaler.srcH, scaleReduced)) scaler.stopped = true;
  }

  void flushBandRows(uint32_t upToSrcY) {  // exclusive
    while (bandTop < upToSrcY && !scaler.stopped) {
      scaler.feedSourceRow(bandTop, band + (bandTop % kBandRows) * bandW);
      ++bandTop;
    }
  }
};

// Grayscale output blocks arrive left→right within a band, bands top→bottom.
// The pixel buffer holds dense 8-bit rows: stride = pDraw->iWidth (the MCU
// group pitch), valid columns = pDraw->iWidthUsed (the right edge is
// trimmed). When a new band starts, all rows of the previous band are
// complete. Returning 0 stops the decode at this band boundary.
int jpegDecDrawToBand(JPEGDRAW* pDraw) {
  auto* state = static_cast<JpegState*>(pDraw->pUser);
  if (state->scaler.stopped) return 0;
  if (pDraw->iHeight <= 0 || pDraw->iWidthUsed <= 0) return 1;
  const uint32_t top = static_cast<uint32_t>(pDraw->y);
  const uint32_t bottom = top + static_cast<uint32_t>(pDraw->iHeight) - 1;
  const uint16_t left = static_cast<uint16_t>(pDraw->x);
  const uint16_t rw = static_cast<uint16_t>(pDraw->iWidthUsed);
  const uint16_t stride = static_cast<uint16_t>(pDraw->iWidth);
  if (top >= state->bandFilled) {
    state->flushBandRows(state->bandFilled);
    state->bandFilled = bottom + 1;
  }
  const uint8_t* src = reinterpret_cast<const uint8_t*>(pDraw->pPixels);
  for (uint32_t y = top; y <= bottom; ++y) {
    uint8_t* dst = state->band + (y % JpegState::kBandRows) * state->bandW + left;
    memcpy(dst, src, rw);
    src += stride;
  }
  return 1;
}

// Opens the heap JPEGDEC object over `reader`. Returns nullptr with
// *outSource.ioError set on a stream failure; the handle's destructor frees.
JPEGDEC* jpegDecOpen(const ZipEntry& entry, ZipEntryReader& reader, JpegDecSource* outSource,
                     JPEG_DRAW_CALLBACK* drawFn) {
  outSource->reader = &reader;
  void* mem = allocJpegDecMem(sizeof(JPEGDEC));
  if (mem == nullptr) return nullptr;
  JPEGDEC* jpeg = new (mem) JPEGDEC();
  if (jpeg->open(outSource, static_cast<int>(entry.uncompressedSize), jpegDecClose, jpegDecRead, jpegDecSeek,
                 drawFn) != 1) {
    freeJpegDecMem(jpeg);
    return nullptr;
  }
  // 8-bit luma output, Y-only for color sources (BT.601-consistent luma, no
  // chroma conversion — same as the removed TJpgDec grayscale format).
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  return jpeg;
}

BookStatus renderJpeg(BookSource& source, const ZipEntry& entry, const PageImage& image,
                      Arena& scratch, ImageRowFn rowFn, void* user, bool passThrough,
                      SourceDimsFn dimsFn, void* dimsUser) {
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;

  JpegDecSource src;
  JpegDecHandle handle;
  handle.jpeg = jpegDecOpen(entry, reader, &src, jpegDecDrawToBand);
  if (handle.jpeg == nullptr) {
    return src.ioError ? BookStatus::IoError : BookStatus::OutOfMemory;
  }
  JPEGDEC& jpeg = *handle.jpeg;

  // Use JPEGDEC's power-of-two downscale (JPEG_SCALE_HALF/QUARTER/EIGHTH) to
  // get close to the target size cheaply; the RowScaler handles the
  // remainder. A half-scale decode that lands within 1.5x of the target
  // forces the scaler to re-resample a 1.0-1.1x reduction — every 1.5px ink
  // line lands on the blocky power-of-two grid and comes out stair-stepped
  // (the 2026-11-10 fidelity soak's "Lego" blocks). Only step the scale when
  // the halved dims still leave a 1.5x margin on both axes; else decode at
  // full resolution.
  const int fullW = jpeg.getWidth();
  const int fullH = jpeg.getHeight();
  uint8_t scale = 0;
  while (scale < 3 && (fullW >> (scale + 1)) >= (image.width * 3) / 2 &&
         (fullH >> (scale + 1)) >= (image.height * 3) / 2) {
    ++scale;
  }
  const uint8_t preferredScale = scale;
  // The fidelity-preferred scale keeps the widest band (srcW * kBandRows);
  // a caller arena that cannot hold it must not lose the page. Step to a
  // smaller decode that still covers the placement (the pre-1.5x pick) and
  // only report OutOfMemory when even that does not fit. JPEGDEC's scaled
  // dims are ceil((full >> scale) — size the band for those.
  static const int kScaleOption[4] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
  const size_t bandMark = scratch.mark();
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  JpegState state;
  for (;; ++scale) {
    if (scale > 3) return BookStatus::OutOfMemory;
    const int adjust = (1 << scale) - 1;
    srcW = static_cast<uint16_t>((fullW + adjust) >> scale);
    srcH = static_cast<uint16_t>((fullH + adjust) >> scale);
    if (srcW == 0 || srcH == 0) return BookStatus::Unsupported;
    // A retried scale may only shrink while it still fills the placement.
    if (scale > 0 && (srcW < image.width || srcH < image.height)) {
      return BookStatus::OutOfMemory;
    }
    state.scaler.srcW = srcW;
    state.scaler.srcH = srcH;
    state.scaler.dstW = image.width < srcW ? image.width : srcW;
    state.scaler.dstH = image.height < srcH ? image.height : srcH;
    state.scaler.rowFn = rowFn;
    state.scaler.user = user;
    state.scaler.passThrough = passThrough;
    state.dimsFn = dimsFn;
    state.dimsUser = dimsUser;
    state.bandW = srcW;
    state.band = static_cast<uint8_t*>(scratch.alloc(srcW * JpegState::kBandRows, 1));
    if (state.band != nullptr && initScaler(state.scaler, scratch)) break;
    scratch.release(bandMark);
  }
  // The arena-retry loop may have stepped past the preferred scale — report
  // it so the consumer can refuse to cache a reduced-detail decode.
  state.scaleReduced = scale > preferredScale;
  if (passThrough) state.reportDims();  // consumer may stop the stream right here
  jpeg.setUserPointer(&state);

  const int rc = jpeg.decode(0, 0, kScaleOption[scale]);
  if (src.ioError) return BookStatus::IoError;
  // DecodeJPEG returns 1 on a completed (or draw-aborted) decode, 0 on a
  // decode error. An aborted draw set scaler.stopped via the row callback.
  if (rc == 0 && !state.scaler.stopped) return BookStatus::ParseError;
  if (!state.scaler.stopped) state.flushBandRows(srcH);
  return BookStatus::Ok;
}

#else  // !TJFB_HAVE_JPEGDEC

BookStatus renderJpeg(BookSource&, const ZipEntry&, const PageImage&, Arena&, ImageRowFn, void*, bool,
                      SourceDimsFn, void*) {
  return BookStatus::Unsupported;
}

#endif  // TJFB_HAVE_JPEGDEC

// --- progressive JPEG ----------------------------------------------------------
//
// The full-fidelity decoder (libs/assets/ProgressiveJpeg, optional per
// install) pass-through path decodes luma from ALL scans — the coefficient
// store is ~2 B/pixel, a fit for the 8 MB PSRAM decode budget — and hands
// the unscaled source rows to the app scaler. Anything the full decoder
// cannot start (store too big, parse/feature miss) falls back to the
// DC-only first-scan rendering below so the placement still paints; the
// PageRenderer path (`render`, passThrough=false) keeps the DC-only contract
// unchanged.
//
// The DC-only fallback decodes the first scan through JPEGDEC: a
// progressive stream (0xc2) makes JPEGDEC decode the DC values only and
// force its JPEG_SCALE_EIGHTH output — one pixel per 8×8 block, exactly the
// old hand-written DC decoder's block-average picture (chroma is decoded to
// keep the bitstream in sync and discarded — output is luma only), without
// keeping a hand-rolled entropy decoder in the tree. Scaled dims are
// ceil(w/8) × ceil(h/8), matching the block grid the old path reported.

// Streaming bilinear resampler (both directions) for the small DC image.
struct ProgResample {
  uint16_t srcW = 0, srcH = 0, dstW = 0, dstH = 0;
  uint8_t* prevRow = nullptr;  // dstW-wide, already horizontally resampled
  uint8_t* currRow = nullptr;
  uint8_t* outRow = nullptr;
  int32_t prevY = -1;
  uint32_t nextOut = 0;
  ImageRowFn rowFn = nullptr;
  void* user = nullptr;
  bool stopped = false;
  bool passThrough = false;

  static uint32_t stepFp(uint16_t src, uint16_t dst) {
    return (src <= 1 || dst <= 1) ? 0
                                  : ((static_cast<uint32_t>(src - 1) << 16) / (dst - 1));
  }

  void scaleRow(const uint8_t* src, uint8_t* dst) const {
    const uint32_t step = stepFp(srcW, dstW);
    for (uint32_t x = 0; x < dstW; ++x) {
      const uint32_t fp = x + 1 == dstW ? static_cast<uint32_t>(srcW - 1) << 16 : x * step;
      const uint32_t x0 = fp >> 16;
      const uint32_t x1 = x0 + 1 < srcW ? x0 + 1 : x0;
      const uint32_t fx = fp & 0xFFFF;
      dst[x] = static_cast<uint8_t>((src[x0] * (0x10000 - fx) + src[x1] * fx) >> 16);
    }
  }

  bool feed(const uint8_t* srcRow, uint32_t srcY) {
    if (passThrough) return rowFn(user, static_cast<uint16_t>(srcY), srcRow, srcW);
    scaleRow(srcRow, currRow);
    const uint32_t step = stepFp(srcH, dstH);
    while (nextOut < dstH) {
      uint32_t fp = nextOut + 1 == dstH ? static_cast<uint32_t>(srcH - 1) << 16
                                        : nextOut * step;
      const uint32_t y0 = fp >> 16;
      const uint32_t y1 = y0 + 1 < srcH ? y0 + 1 : y0;
      if (y1 > srcY) break;
      const uint8_t* r0 = y0 == srcY ? currRow : prevRow;
      const uint8_t* r1 = y1 == srcY ? currRow : prevRow;
      const uint32_t fy = fp & 0xFFFF;
      for (uint32_t x = 0; x < dstW; ++x) {
        outRow[x] = static_cast<uint8_t>((r0[x] * (0x10000 - fy) + r1[x] * fy) >> 16);
      }
      if (!rowFn(user, static_cast<uint16_t>(nextOut), outRow, dstW)) {
        stopped = true;
        return false;
      }
      ++nextOut;
    }
    uint8_t* t = prevRow;
    prevRow = currRow;
    currRow = t;
    prevY = static_cast<int32_t>(srcY);
    return true;
  }

  bool finish() {
    if (passThrough) return true;  // every source row was fed directly
    while (nextOut < dstH && prevY >= 0) {
      if (!rowFn(user, static_cast<uint16_t>(nextOut), prevRow, dstW)) {
        stopped = true;
        return false;
      }
      ++nextOut;
    }
    return true;
  }
};

// Pass-through state for the full-fidelity ProgressiveJpeg decode.
#if TJFB_HAVE_PROGRESSIVE_FULL
struct PjState {
  ZipEntryReader* reader = nullptr;
  bool ioError = false;
};

int32_t pjReadZip(void* ctx, uint8_t* dst, uint32_t n) {
  auto* st = static_cast<PjState*>(ctx);
  const int32_t got = st->reader->read(dst, n);
  if (got < 0) st->ioError = true;
  return got;
}

// Full-fidelity luma from all scans (libs/assets/ProgressiveJpeg). The
// coefficient store (~2 B/pixel) survives the whole stream, so rows emit
// only after the last scan. Every begin()/decode() memory or stream failure
// falls back to the DC-only path below — the OOM store is freed inside
// ProgressiveJpeg's destructor, so the retry costs no held memory.
BookStatus renderJpegProgressiveFull(BookSource& source, const ZipEntry& entry,
                                     Arena& scratch, ImageRowFn rowFn,
                                     void* user, SourceDimsFn dimsFn,
                                     void* dimsUser, RenderPollFn pollFn,
                                     void* pollUser, bool& usedFull) {
  usedFull = false;
  const size_t marked = scratch.mark();
  PjState st;
  freeink::ProgressiveJpeg pj;
  {
    ZipEntryReader header;
    st.reader = &header;
    const BookStatus hs = header.open(source, entry, scratch);
    if (hs != BookStatus::Ok) {
      scratch.release(marked);
      return hs;
    }
    const freeink::ProgressiveJpeg::Status bs = pj.begin(&pjReadZip, &st);
    scratch.release(marked);
    if (bs != freeink::ProgressiveJpeg::Status::Ok) {
      PJ_LOG_WRN("full decode begin failed (status %d, ncomp %u) - DC fallback", static_cast<int>(bs),
                 static_cast<unsigned>(pj.ncomp()));
      return BookStatus::OutOfMemory;  // DC fallback
    }
  }
  // The dims report is deferred to the FIRST EMITTED ROW (the PNG path's
  // dimsSent shape): a decode that dies mid-stream (store OOM, parse miss)
  // must not have already reported the full frame dims — the DC-only
  // fallback reports its own 1/8-scale dims, and a consumer that sized its
  // state on the first report would see a width change mid-decode (the
  // app's row sink refuses widths other than the attached scaler's).
  struct DimsOnce {
    SourceDimsFn fn;
    void* user;
    uint16_t dimW = 0, dimH = 0;  // the frame header's dims (from begin())
    bool sent = false;
    ImageRowFn rowFn = nullptr;
    void* rowUser = nullptr;
    static bool onRow(void* u, const uint16_t y, const uint8_t* gray, const uint16_t width) {
      auto* self = static_cast<DimsOnce*>(u);
      if (!self->sent) {
        self->sent = true;
        // The full-fidelity path never steps the scale — only the DC
        // fallback's 1/8 dims differ, and the consumer detects those from
        // the reported dims themselves.
        if (self->fn != nullptr && !self->fn(self->user, self->dimW, self->dimH, /*scaleReduced=*/false)) return false;
      }
      return self->rowFn(self->rowUser, y, gray, width);
    }
  } dimsOnce;
  dimsOnce.fn = dimsFn;
  dimsOnce.user = dimsUser;
  dimsOnce.dimW = pj.width();
  dimsOnce.dimH = pj.height();
  dimsOnce.rowFn = rowFn;
  dimsOnce.rowUser = user;
  const size_t marked2 = scratch.mark();
  ZipEntryReader body;
  st.reader = &body;
  const BookStatus os = body.open(source, entry, scratch);
  if (os != BookStatus::Ok) {
    scratch.release(marked2);
    return os;
  }
  const freeink::ProgressiveJpeg::Status status =
      pj.decode(&pjReadZip, &st, DimsOnce::onRow, &dimsOnce, pollFn, pollUser);
  scratch.release(marked2);
  switch (status) {
    case freeink::ProgressiveJpeg::Status::Ok:
    case freeink::ProgressiveJpeg::Status::Stopped:
      usedFull = true;  // rows streamed (or the consumer stopped inside)
      return BookStatus::Ok;
    case freeink::ProgressiveJpeg::Status::OutOfMemory:
    case freeink::ProgressiveJpeg::Status::Unsupported:
    case freeink::ProgressiveJpeg::Status::ParseError:
      PJ_LOG_WRN("full decode failed (status %d, ncomp %u) - DC fallback", static_cast<int>(status),
                 static_cast<unsigned>(pj.ncomp()));
      return BookStatus::OutOfMemory;  // DC fallback covers every retry case
    case freeink::ProgressiveJpeg::Status::IoError:
      return st.ioError ? BookStatus::IoError : BookStatus::ParseError;
  }
  return BookStatus::ParseError;
}
#endif  // TJFB_HAVE_PROGRESSIVE_FULL

#if TJFB_HAVE_JPEGDEC

// Streaming DC-row state for the fallback: JPEGDEC emits the 1/8 image in
// MCU groups (a group can span several block rows); each group's rows are
// dense 8-bit rows of iWidthUsed valid columns, iWidth-strided.
struct ProgDrawState {
  ProgResample* rs = nullptr;
  bool stopped = false;
};

int progDecDrawRow(JPEGDRAW* pDraw) {
  auto* st = static_cast<ProgDrawState*>(pDraw->pUser);
  if (st->stopped) return 0;
  if (pDraw->iHeight <= 0 || pDraw->iWidthUsed <= 0) return 1;
  const uint16_t stride = static_cast<uint16_t>(pDraw->iWidth);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(pDraw->pPixels);
  for (int r = 0; r < pDraw->iHeight; ++r) {
    if (!st->rs->feed(src, static_cast<uint32_t>(pDraw->y) + static_cast<uint32_t>(r))) {
      st->stopped = true;  // consumer stopped (pass-through stop or row stop)
      return 0;
    }
    src += stride;
  }
  return 1;
}

BookStatus renderJpegProgressive(BookSource& source, const ZipEntry& entry,
                                 const PageImage& image, Arena& scratch, ImageRowFn rowFn,
                                 void* user, bool passThrough, SourceDimsFn dimsFn,
                                 void* dimsUser) {
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;

  JpegDecSource src;
  JpegDecHandle handle;
  handle.jpeg = jpegDecOpen(entry, reader, &src, progDecDrawRow);
  if (handle.jpeg == nullptr) {
    return src.ioError ? BookStatus::IoError : BookStatus::OutOfMemory;
  }
  JPEGDEC& jpeg = *handle.jpeg;

  // A progressive stream (0xc2) forces JPEGDEC's EIGHTH scale — the decoded
  // image is the DC-only block grid, ceil(w/8) × ceil(h/8).
  const uint16_t blocksW = static_cast<uint16_t>((jpeg.getWidth() + 7) >> 3);
  const uint16_t blocksH = static_cast<uint16_t>((jpeg.getHeight() + 7) >> 3);
  if (blocksW == 0 || blocksH == 0) return BookStatus::Unsupported;

  ProgResample rs;
  rs.srcW = blocksW;
  rs.srcH = blocksH;
  rs.dstW = image.width;
  rs.dstH = image.height;
  rs.rowFn = rowFn;
  rs.user = user;
  rs.passThrough = passThrough;
  rs.prevRow = static_cast<uint8_t*>(scratch.alloc(image.width, 1));
  rs.currRow = static_cast<uint8_t*>(scratch.alloc(image.width, 1));
  rs.outRow = static_cast<uint8_t*>(scratch.alloc(image.width, 1));
  if (rs.prevRow == nullptr || rs.currRow == nullptr || rs.outRow == nullptr) {
    return BookStatus::OutOfMemory;
  }
  // Pass-through: report the streamed dims once (each row is one DC-block
  // row, blocksW wide — the app's scaler sees 8x8-block averages).
  if (passThrough && dimsFn != nullptr && !dimsFn(dimsUser, blocksW, blocksH, /*scaleReduced=*/false)) {
    return BookStatus::Ok;  // consumer stopped before any row
  }

  ProgDrawState drawState;
  drawState.rs = &rs;
  jpeg.setUserPointer(&drawState);
  const int rc = jpeg.decode(0, 0, JPEG_SCALE_EIGHTH);
  if (src.ioError) return BookStatus::IoError;
  if (rc == 0 && !drawState.stopped) return BookStatus::ParseError;
  if (!drawState.stopped) rs.finish();
  return BookStatus::Ok;
}

#else  // !TJFB_HAVE_JPEGDEC

BookStatus renderJpegProgressive(BookSource&, const ZipEntry&, const PageImage&, Arena&, ImageRowFn, void*,
                                 bool, SourceDimsFn, void*) {
  return BookStatus::Unsupported;
}

#endif  // TJFB_HAVE_JPEGDEC

}  // namespace

void ditherRowOrdered(const uint8_t* gray, uint16_t width, uint16_t y, uint8_t* outBits) {
  static const uint8_t kBayer4[4][4] = {
      {15, 135, 45, 165}, {195, 75, 225, 105}, {60, 180, 30, 150}, {240, 120, 210, 90}};
  memset(outBits, 0, (width + 7u) / 8u);
  for (uint16_t x = 0; x < width; ++x) {
    if (gray[x] < kBayer4[y & 3][x & 3]) {
      outBits[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
    }
  }
}

BookStatus ImageRenderer::render(BookSource& source, const ZipCatalog& zip,
                                 const PageImage& image, Arena& scratch, ImageRowFn rowFn,
                                 void* user) {
  if (image.width == 0 || image.height == 0) return BookStatus::Unsupported;
  const ZipEntry* entry = zip.find(image.href);
  if (entry == nullptr) return BookStatus::NotFound;

  const size_t marked = scratch.mark();
  ImageInfo info;
  BookStatus status = probeImage(source, *entry, scratch, &info);
  if (status == BookStatus::Ok) {
    switch (info.kind) {
      case ImageInfo::Kind::Png:
        status = renderPng(source, *entry, image, info.width, info.height, scratch, rowFn,
                           user, false, nullptr, nullptr);
        break;
      case ImageInfo::Kind::Jpeg:
        status = info.progressive
                     ? renderJpegProgressive(source, *entry, image, scratch, rowFn, user,
                                             false, nullptr, nullptr)
                     : renderJpeg(source, *entry, image, scratch, rowFn, user, false, nullptr,
                                  nullptr);
        break;
      case ImageInfo::Kind::Gif:
      case ImageInfo::Kind::Bmp:
      case ImageInfo::Kind::Unknown:
        // Probed but not decoded by the SDK — the host app renders these
        // with its own converters.
        status = BookStatus::Unsupported;
        break;
    }
  }
  scratch.release(marked);
  return status;
}

BookStatus ImageRenderer::renderSourceRows(BookSource& source, const ZipCatalog& zip, const char* href,
                                           const uint16_t dstW, const uint16_t dstH, Arena& scratch,
                                           SourceDimsFn dimsFn, void* dimsUser, ImageRowFn rowFn,
                                           void* user) {
  return renderSourceRows(source, zip, href, dstW, dstH, scratch, dimsFn, dimsUser, rowFn, user, nullptr,
                          nullptr);
}

BookStatus ImageRenderer::renderSourceRows(BookSource& source, const ZipCatalog& zip, const char* href,
                                           const uint16_t dstW, const uint16_t dstH, Arena& scratch,
                                           SourceDimsFn dimsFn, void* dimsUser, ImageRowFn rowFn,
                                           void* user, RenderPollFn pollFn, void* pollUser) {
  if (rowFn == nullptr) return BookStatus::Unsupported;
  const ZipEntry* entry = zip.find(href);
  if (entry == nullptr) return BookStatus::NotFound;

  // Same decode stack as render(), with the scalers bypassed. dstW/dstH only
  // steer the baseline JPEG power-of-two pre-scale (decode memory), exactly
  // like render()'s placement dims do.
  PageImage image{};
  image.href = href != nullptr ? href : "";
  image.x = 0;
  image.y = 0;
  image.width = dstW;
  image.height = dstH;

  const size_t marked = scratch.mark();
  ImageInfo info;
  BookStatus status = probeImage(source, *entry, scratch, &info);
  if (status == BookStatus::Ok) {
    switch (info.kind) {
      case ImageInfo::Kind::Png:
        status = renderPng(source, *entry, image, info.width, info.height, scratch, rowFn,
                           user, true, dimsFn, dimsUser);
        break;
      case ImageInfo::Kind::Jpeg:
#if TJFB_HAVE_PROGRESSIVE_FULL
        if (info.progressive) {
          // Full-fidelity first: the store (~2 B/pixel) carries all scans.
          // OutOfMemory (or a parse/feature miss) falls back to the DC-only
          // first-scan path so the placement still paints.
          bool usedFull = false;
          const BookStatus full = renderJpegProgressiveFull(source, *entry, scratch, rowFn, user, dimsFn,
                                                            dimsUser, pollFn, pollUser, usedFull);
          if (full == BookStatus::Ok) {
            status = full;
            break;
          }
          if (full != BookStatus::OutOfMemory) {
            status = full;  // IO errors must not hide behind the fallback
            break;
          }
        }
#endif
        status = info.progressive
                     ? renderJpegProgressive(source, *entry, image, scratch, rowFn, user,
                                             true, dimsFn, dimsUser)
                     : renderJpeg(source, *entry, image, scratch, rowFn, user, true, dimsFn,
                                  dimsUser);
        break;
      default:
        // Gif/Bmp/Unknown are decoded by the host app's converters, not here.
        status = BookStatus::Unsupported;
        break;
    }
  }
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
