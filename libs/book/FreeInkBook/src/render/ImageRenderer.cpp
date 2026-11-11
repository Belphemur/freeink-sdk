// FreeInkBook — streaming PNG/JPEG decode → scaled grayscale rows.

#include "render/ImageRenderer.h"

#include <string.h>

#include <pngle.h>
#include <tjpgd.h>

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

struct JpegState {
  RowScaler scaler;
  ZipEntryReader* reader = nullptr;
  uint8_t* skipBuf = nullptr;
  uint8_t* band = nullptr;      // srcW × kBandRows grayscale
  uint16_t bandW = 0;
  uint32_t bandTop = 0;         // source y of band row 0
  uint32_t bandFilled = 0;      // rows known to be complete (top-based)
  bool ioError = false;
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

size_t jpegInput(JDEC* jd, uint8_t* buf, size_t len) {
  JpegState* state = static_cast<JpegState*>(jd->device);
  size_t total = 0;
  while (total < len) {
    uint8_t* dst = buf != nullptr ? buf + total : state->skipBuf;
    const uint32_t want =
        buf != nullptr ? static_cast<uint32_t>(len - total)
                       : static_cast<uint32_t>((len - total) < 1024 ? (len - total) : 1024);
    const int32_t n = state->reader->read(dst, want);
    if (n < 0) {
      state->ioError = true;
      return 0;
    }
    if (n == 0) break;
    total += static_cast<size_t>(n);
  }
  return total;
}

int jpegOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  JpegState* state = static_cast<JpegState*>(jd->device);
  if (state->scaler.stopped) return 0;
  // Blocks arrive left→right within a band, bands top→bottom. When a new
  // band starts, all rows of the previous band are complete.
  if (static_cast<uint32_t>(rect->top) >= state->bandFilled) {
    state->flushBandRows(state->bandFilled);
    state->bandFilled = rect->bottom + 1;
  }
  const uint8_t* src = static_cast<const uint8_t*>(bitmap);
  const uint16_t rw = static_cast<uint16_t>(rect->right - rect->left + 1);
  for (uint16_t y = rect->top; y <= rect->bottom; ++y) {
    uint8_t* dst = state->band + (y % JpegState::kBandRows) * state->bandW + rect->left;
    memcpy(dst, src, rw);
    src += rw;
  }
  return 1;
}

BookStatus renderJpeg(BookSource& source, const ZipEntry& entry, const PageImage& image,
                      Arena& scratch, ImageRowFn rowFn, void* user, bool passThrough,
                      SourceDimsFn dimsFn, void* dimsUser) {
  JpegState state;
  state.skipBuf = static_cast<uint8_t*>(scratch.alloc(1024, 1));
  void* work = scratch.alloc(8 * 1024, alignof(max_align_t));
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;
  state.reader = &reader;
  if (state.skipBuf == nullptr || work == nullptr) return BookStatus::OutOfMemory;

  JDEC jd;
  if (jd_prepare(&jd, jpegInput, work, 8 * 1024, &state) != JDR_OK) {
    return state.ioError ? BookStatus::IoError : BookStatus::ParseError;
  }

  // Use TJpgDec's power-of-two downscale to get close to the target size
  // cheaply; the RowScaler handles the remainder. A half-scale decode that
  // lands within 1.5x of the target forces the scaler to re-resample a
  // 1.0-1.1x reduction — every 1.5px ink line lands on the blocky power-of-
  // two grid and comes out stair-stepped (the 2026-11-10 fidelity soak's
  // "Lego" blocks). Only step the scale when the halved dims still leave a
  // 1.5x margin on both axes; else decode at full resolution.
  uint8_t scale = 0;
  while (scale < 3 && (jd.width >> (scale + 1)) >= (image.width * 3) / 2 &&
         (jd.height >> (scale + 1)) >= (image.height * 3) / 2) {
    ++scale;
  }
  const uint8_t preferredScale = scale;
  // The fidelity-preferred scale keeps the widest band (srcW * kBandRows);
  // a caller arena that cannot hold it must not lose the page. Step to a
  // smaller decode that still covers the placement (the pre-1.5x pick) and
  // only report OutOfMemory when even that does not fit.
  const size_t bandMark = scratch.mark();
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  for (;; ++scale) {
    if (scale > 3) return BookStatus::OutOfMemory;
    srcW = static_cast<uint16_t>(jd.width >> scale);
    srcH = static_cast<uint16_t>(jd.height >> scale);
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

  const JRESULT jr = jd_decomp(&jd, jpegOutput, scale);
  if (state.ioError) return BookStatus::IoError;
  if (jr != JDR_OK && !state.scaler.stopped) return BookStatus::ParseError;
  if (!state.scaler.stopped) state.flushBandRows(srcH);
  return BookStatus::Ok;
}


// --- progressive JPEG ----------------------------------------------------------
//
// When the ProgressiveJpeg decoder is linked (the app ships it as a libdep;
// optional per install), pass-through (renderSourceRows, the
// app-side-scaler path) decodes full-fidelity luma from ALL scans — the
// coefficient store is ~2 B/pixel, a fit for the 8 MB PSRAM decode budget —
// and hands the unscaled source rows to the app scaler. Anything the full
// decoder cannot start (store too big, parse/feature miss) falls back to the
// old DC-only first-scan rendering below so the placement still paints; the
// PageRenderer path (`render`, passThrough=false) keeps the DC-only contract
// unchanged.
//
// The DC-only decoder below decodes only the FIRST scan of a progressive
// JPEG (block DCT averages = a correct 1/8-scale picture, chroma DC values
// entropy-decoded to keep the bitstream in sync but never used — output is
// luma only) — the fallback and the PageRenderer contract.

struct ProgHuff {
  uint8_t counts[17] = {0};     // codes per length 1..16
  uint16_t firstCode[17] = {0};
  uint16_t firstIndex[17] = {0};
  uint8_t symbols[256] = {0};
  bool ready = false;
};

struct ProgJpeg {
  ZipEntryReader* reader = nullptr;
  uint8_t buf[512];
  uint32_t bufLen = 0;
  uint32_t bufPos = 0;
  bool ioError = false;

  uint32_t bitBuf = 0;
  int bitCount = 0;
  int pendingMarker = 0;  // marker seen inside entropy data (RSTn/EOI)

  int rawByte() {
    if (bufPos >= bufLen) {
      const int32_t n = reader->read(buf, sizeof(buf));
      if (n <= 0) {
        if (n < 0) ioError = true;
        return -1;
      }
      bufLen = static_cast<uint32_t>(n);
      bufPos = 0;
    }
    return buf[bufPos++];
  }

  // One entropy-coded bit; FF00 unstuffed, markers latch into pendingMarker.
  int nextBit() {
    if (bitCount == 0) {
      int b = rawByte();
      if (b < 0) return -1;
      if (b == 0xFF) {
        int m = rawByte();
        while (m == 0xFF) m = rawByte();
        if (m < 0) return -1;
        if (m != 0x00) {
          pendingMarker = m;
          return -1;
        }
        b = 0xFF;
      }
      bitBuf = static_cast<uint32_t>(b);
      bitCount = 8;
    }
    --bitCount;
    return static_cast<int>((bitBuf >> bitCount) & 1u);
  }

  int decodeHuff(const ProgHuff& t) {
    uint32_t code = 0;
    for (int len = 1; len <= 16; ++len) {
      const int b = nextBit();
      if (b < 0) return -1;
      code = (code << 1) | static_cast<uint32_t>(b);
      if (t.counts[len] != 0 && code < static_cast<uint32_t>(t.firstCode[len]) + t.counts[len]) {
        return t.symbols[t.firstIndex[len] + (code - t.firstCode[len])];
      }
    }
    return -1;
  }

  // JPEG RECEIVE+EXTEND for an s-bit DC difference.
  int32_t receiveExtend(int s, bool* ok) {
    int32_t v = 0;
    for (int i = 0; i < s; ++i) {
      const int b = nextBit();
      if (b < 0) {
        *ok = false;
        return 0;
      }
      v = (v << 1) | b;
    }
    if (s > 0 && v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
  }
};

void buildProgHuff(ProgHuff& t) {
  uint16_t code = 0;
  uint16_t index = 0;
  for (int len = 1; len <= 16; ++len) {
    t.firstCode[len] = code;
    t.firstIndex[len] = index;
    code = static_cast<uint16_t>(code + t.counts[len]);
    index = static_cast<uint16_t>(index + t.counts[len]);
    code = static_cast<uint16_t>(code << 1);
  }
  t.ready = true;
}

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

BookStatus renderJpegProgressive(BookSource& source, const ZipEntry& entry,
                                 const PageImage& image, Arena& scratch, ImageRowFn rowFn,
                                 void* user, bool passThrough, SourceDimsFn dimsFn,
                                 void* dimsUser) {
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;

  ProgJpeg pj;
  pj.reader = &reader;

  // Header bytes come through the same buffered reader, pre-bitstream.
  auto rd = [&](uint8_t* dst, uint32_t n) -> bool {
    for (uint32_t i = 0; i < n; ++i) {
      const int b = pj.rawByte();
      if (b < 0) return false;
      dst[i] = static_cast<uint8_t>(b);
    }
    return true;
  };

  uint8_t hdr[2];
  if (!rd(hdr, 2) || hdr[0] != 0xFF || hdr[1] != 0xD8) return BookStatus::ParseError;

  ProgHuff* dcTables = static_cast<ProgHuff*>(scratch.alloc(sizeof(ProgHuff) * 4,
                                                            alignof(ProgHuff)));
  if (dcTables == nullptr) return BookStatus::OutOfMemory;
  for (int i = 0; i < 4; ++i) dcTables[i] = ProgHuff{};

  uint16_t width = 0, height = 0, restartInterval = 0;
  uint16_t dcQuant[4] = {1, 1, 1, 1};  // DC quantizer per DQT table id
  struct Comp {
    uint8_t id = 0, h = 1, v = 1, tq = 0, td = 0;
  } comps[4];
  uint8_t ncomp = 0;
  uint8_t scanComp[4] = {0};  // indices into comps, in scan order
  uint8_t ns = 0;
  uint8_t al = 0;

  for (;;) {  // segment loop up to SOS
    int m = pj.rawByte();
    while (m == 0xFF) m = pj.rawByte();
    if (m < 0) return pj.ioError ? BookStatus::IoError : BookStatus::ParseError;
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
    uint8_t lenB[2];
    if (!rd(lenB, 2)) return BookStatus::ParseError;
    uint32_t segLen = (static_cast<uint32_t>(lenB[0]) << 8 | lenB[1]);
    if (segLen < 2) return BookStatus::ParseError;
    segLen -= 2;
    if (m == 0xDB) {  // DQT — only the DC entry matters
      while (segLen > 0) {
        uint8_t pqtq;
        if (!rd(&pqtq, 1)) return BookStatus::ParseError;
        const uint8_t pq = pqtq >> 4, tq = pqtq & 0x0F;
        const uint32_t bytes = pq ? 128 : 64;
        if (segLen < 1 + bytes || tq > 3) return BookStatus::ParseError;
        uint8_t q0[2];
        if (!rd(q0, pq ? 2 : 1)) return BookStatus::ParseError;
        dcQuant[tq] = pq ? static_cast<uint16_t>(q0[0] << 8 | q0[1]) : q0[0];
        for (uint32_t i = pq ? 2u : 1u; i < bytes; ++i) {
          uint8_t skip;
          if (!rd(&skip, 1)) return BookStatus::ParseError;
        }
        segLen -= 1 + bytes;
      }
    } else if (m == 0xC4) {  // DHT
      while (segLen > 0) {
        uint8_t tcth;
        if (!rd(&tcth, 1)) return BookStatus::ParseError;
        const uint8_t tc = tcth >> 4, th = tcth & 0x0F;
        uint8_t counts[16];
        if (segLen < 17 || !rd(counts, 16)) return BookStatus::ParseError;
        uint32_t total = 0;
        for (int i = 0; i < 16; ++i) total += counts[i];
        if (total > 256 || segLen < 17 + total) return BookStatus::ParseError;
        if (tc == 0 && th < 4) {
          ProgHuff& t = dcTables[th];
          for (int i = 0; i < 16; ++i) t.counts[i + 1] = counts[i];
          if (!rd(t.symbols, total)) return BookStatus::ParseError;
          buildProgHuff(t);
        } else {
          uint8_t skip;
          for (uint32_t i = 0; i < total; ++i) {
            if (!rd(&skip, 1)) return BookStatus::ParseError;
          }
        }
        segLen -= 17 + total;
      }
    } else if (m == 0xC2) {  // SOF2
      uint8_t sof[6];
      if (!rd(sof, 6)) return BookStatus::ParseError;
      height = static_cast<uint16_t>(sof[1] << 8 | sof[2]);
      width = static_cast<uint16_t>(sof[3] << 8 | sof[4]);
      ncomp = sof[5];
      if (ncomp == 0 || ncomp > 4 || segLen != 6u + ncomp * 3u) return BookStatus::ParseError;
      for (uint8_t c = 0; c < ncomp; ++c) {
        uint8_t cc[3];
        if (!rd(cc, 3)) return BookStatus::ParseError;
        comps[c].id = cc[0];
        comps[c].h = cc[1] >> 4;
        comps[c].v = cc[1] & 0x0F;
        comps[c].tq = cc[2] & 0x03;
        if (comps[c].h == 0 || comps[c].v == 0) return BookStatus::ParseError;
      }
    } else if (m == 0xC0 || m == 0xC1) {
      return BookStatus::ParseError;  // baseline mislabeled — not our path
    } else if (m == 0xDD) {  // DRI
      uint8_t d[2];
      if (segLen != 2 || !rd(d, 2)) return BookStatus::ParseError;
      restartInterval = static_cast<uint16_t>(d[0] << 8 | d[1]);
    } else if (m == 0xDA) {  // SOS — first scan
      uint8_t nsB;
      if (!rd(&nsB, 1)) return BookStatus::ParseError;
      ns = nsB;
      if (ns == 0 || ns > 4 || segLen != 1u + ns * 2u + 3u) return BookStatus::ParseError;
      for (uint8_t i = 0; i < ns; ++i) {
        uint8_t sc[2];
        if (!rd(sc, 2)) return BookStatus::ParseError;
        uint8_t idx = 0xFF;
        for (uint8_t c = 0; c < ncomp; ++c) {
          if (comps[c].id == sc[0]) idx = c;
        }
        if (idx == 0xFF) return BookStatus::ParseError;
        comps[idx].td = sc[1] >> 4;
        scanComp[i] = idx;
      }
      uint8_t prog[3];
      if (!rd(prog, 3)) return BookStatus::ParseError;
      // First scan of a valid progressive stream is DC (Ss=Se=0, Ah=0).
      if (prog[0] != 0 || prog[1] != 0 || (prog[2] >> 4) != 0) return BookStatus::Unsupported;
      al = prog[2] & 0x0F;
      break;
    } else if (m == 0xD9) {
      return BookStatus::ParseError;
    } else {
      uint8_t skip;
      for (uint32_t i = 0; i < segLen; ++i) {
        if (!rd(&skip, 1)) return BookStatus::ParseError;
      }
    }
  }
  if (width == 0 || height == 0) return BookStatus::ParseError;

  // Luma must carry the max sampling factors or its DC grid is not 1/8 scale.
  uint8_t hmax = 1, vmax = 1;
  for (uint8_t c = 0; c < ncomp; ++c) {
    if (comps[c].h > hmax) hmax = comps[c].h;
    if (comps[c].v > vmax) vmax = comps[c].v;
  }
  const Comp& y = comps[0];
  if (y.h != hmax || y.v != vmax) return BookStatus::Unsupported;
  const bool interleaved = ns > 1;
  if (!interleaved && scanComp[0] != 0) return BookStatus::Unsupported;  // first scan not luma

  const uint16_t blocksW = static_cast<uint16_t>((width + 7) / 8);
  const uint16_t blocksH = static_cast<uint16_t>((height + 7) / 8);
  const uint16_t mcusX = static_cast<uint16_t>((width + 8 * hmax - 1) / (8 * hmax));
  const uint16_t mcusY = static_cast<uint16_t>((height + 8 * vmax - 1) / (8 * vmax));
  const uint16_t padBlocksW = interleaved ? static_cast<uint16_t>(mcusX * y.h) : blocksW;

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
  uint8_t* rowBuf = static_cast<uint8_t*>(scratch.alloc(static_cast<uint32_t>(padBlocksW) * y.v, 1));
  if (rs.prevRow == nullptr || rs.currRow == nullptr || rs.outRow == nullptr ||
      rowBuf == nullptr) {
    return BookStatus::OutOfMemory;
  }
  // Pass-through: report the streamed dims once (each row is one DC-block
  // row, blocksW wide — the app's scaler sees 8x8-block averages).
  if (passThrough && dimsFn != nullptr && !dimsFn(dimsUser, blocksW, blocksH, /*scaleReduced=*/false)) {
    return BookStatus::Ok;  // consumer stopped before any row
  }

  int32_t pred[4] = {0, 0, 0, 0};
  uint32_t sinceRestart = 0;
  const uint16_t q0 = dcQuant[y.tq];
  uint32_t emittedBlockRows = 0;

  const uint32_t rows = interleaved ? mcusY : blocksH;
  const uint32_t cols = interleaved ? mcusX : blocksW;
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      if (restartInterval != 0 && sinceRestart == restartInterval) {
        pj.bitCount = 0;  // byte-align
        int mk = pj.pendingMarker;
        pj.pendingMarker = 0;
        if (mk == 0) {
          int b = pj.rawByte();
          while (b == 0xFF) b = pj.rawByte();
          mk = b;
        }
        if (mk < 0xD0 || mk > 0xD7) return BookStatus::ParseError;
        pred[0] = pred[1] = pred[2] = pred[3] = 0;
        sinceRestart = 0;
      }
      const uint8_t nsc = interleaved ? ns : 1;
      for (uint8_t sci = 0; sci < nsc; ++sci) {
        const uint8_t ci = scanComp[sci];
        const Comp& comp = comps[ci];
        if (!dcTables[comp.td].ready) return BookStatus::ParseError;
        const uint8_t bh = interleaved ? comp.h : 1;
        const uint8_t bv = interleaved ? comp.v : 1;
        for (uint8_t by = 0; by < bv; ++by) {
          for (uint8_t bx = 0; bx < bh; ++bx) {
            const int t = pj.decodeHuff(dcTables[comp.td]);
            if (t < 0 || t > 15) {
              return pj.ioError ? BookStatus::IoError : BookStatus::ParseError;
            }
            bool ok = true;
            const int32_t diff = pj.receiveExtend(t, &ok);
            if (!ok) return pj.ioError ? BookStatus::IoError : BookStatus::ParseError;
            pred[ci] += diff;
            if (ci == 0) {  // luma: DC * quant is 8x the block average (level-shifted)
              const int32_t dc = (pred[ci] << al) * static_cast<int32_t>(q0);
              int32_t gray = 128 + dc / 8;
              if (gray < 0) gray = 0;
              if (gray > 255) gray = 255;
              const uint32_t px = interleaved ? c * y.h + bx : c;
              if (px < padBlocksW) {
                rowBuf[static_cast<uint32_t>(by) * padBlocksW + px] =
                    static_cast<uint8_t>(gray);
              }
            }
          }
        }
      }
      ++sinceRestart;
    }
    const uint8_t rowsHere = interleaved ? y.v : 1;
    for (uint8_t by = 0; by < rowsHere && emittedBlockRows < blocksH; ++by) {
      if (!rs.feed(rowBuf + static_cast<uint32_t>(by) * padBlocksW, emittedBlockRows)) {
        return BookStatus::Ok;  // consumer stopped
      }
      ++emittedBlockRows;
    }
  }
  rs.finish();
  return BookStatus::Ok;
}

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
