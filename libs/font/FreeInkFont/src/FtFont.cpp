#include "FtFont.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

#include "FontAlloc.h"
#include "Gpos.h"
#include "Gsub.h"

namespace freeink {
namespace font {

namespace {
FtFont::MemoryCallbacks g_memoryCallbacks{};

void* fontAlloc(const size_t size) {
  return g_memoryCallbacks.allocate ? g_memoryCallbacks.allocate(g_memoryCallbacks.context, size) : fiFontMalloc(size);
}
void fontFree(void* block) {
  if (g_memoryCallbacks.deallocate)
    g_memoryCallbacks.deallocate(g_memoryCallbacks.context, block);
  else
    fiFontFree(block);
}
void* fontRealloc(void* block, const size_t oldSize, const size_t newSize) {
  return g_memoryCallbacks.reallocate ? g_memoryCallbacks.reallocate(g_memoryCallbacks.context, block, oldSize, newSize)
                                      : fiFontRealloc(block, newSize);
}

// FreeType memory hooks use either the caller's bounded allocator or the
// platform's PSRAM-preferring default. The shared library is initialized only
// once, so configureMemory() deliberately freezes this choice before first use.
void* ftAlloc(FT_Memory, long size) { return size > 0 ? fontAlloc(static_cast<size_t>(size)) : nullptr; }
void ftFree(FT_Memory, void* block) { fontFree(block); }
void* ftRealloc(FT_Memory, long currentSize, long newSize, void* block) {
  if (currentSize < 0 || newSize < 0) return nullptr;
  return fontRealloc(block, static_cast<size_t>(currentSize), static_cast<size_t>(newSize));
}

// One shared FreeType library for all faces; the library object itself is tiny.
FT_Library g_lib = nullptr;
FT_MemoryRec_ g_ftMemory{};
bool ensureLib() {
  if (g_lib) return true;
  g_ftMemory.user = nullptr;
  g_ftMemory.alloc = &ftAlloc;
  g_ftMemory.free = &ftFree;
  g_ftMemory.realloc = &ftRealloc;
  if (FT_New_Library(&g_ftMemory, &g_lib) != 0) return false;
  FT_Add_Default_Modules(g_lib);  // register the sfnt/truetype/smooth/... modules
  return true;
}
constexpr uint32_t kTagWght = FT_MAKE_TAG('w', 'g', 'h', 't');
constexpr uint32_t kTagItal = FT_MAKE_TAG('i', 't', 'a', 'l');
constexpr uint32_t kTagSlnt = FT_MAKE_TAG('s', 'l', 'n', 't');
constexpr uint32_t kTagGsub = FT_MAKE_TAG('G', 'S', 'U', 'B');
constexpr uint32_t kTagGpos = FT_MAKE_TAG('G', 'P', 'O', 'S');
constexpr uint32_t kTagTtcf = FT_MAKE_TAG('t', 't', 'c', 'f');

uint16_t readBe16(const uint8_t* data) { return uint16_t(data[0]) * 256u + data[1]; }
uint32_t readBe32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}

// Number of faces the container header declares; anything that is not a ttcf
// (including a too-short header) counts as one face. Bounds scan-mode retries
// so non-font bytes cannot trigger a driver probe per face index.
long ttcHeaderFaceCount(const uint8_t* header, const size_t size) {
  if (size >= 12 && readBe32(header) == kTagTtcf) {
    const long count = static_cast<long>(readBe32(header + 8));
    return count > 0 ? count : 1;
  }
  return 1;
}

// For a .ttc, ttcFaceIndex selects which member directory to walk; for a
// plain SFNT file it is ignored. The caller passes the FT face index it
// actually opened — each collection face carries its own shaping tables.
bool findSfntTable(const uint8_t* data, size_t size, uint32_t tag, const uint8_t** table, size_t* tableSize,
                   const long ttcFaceIndex) {
  if (data == nullptr || table == nullptr || tableSize == nullptr || size < 12) return false;
  size_t directory = 0;
  if (readBe32(data) == kTagTtcf) {
    if (size < 16) return false;
    const uint32_t faceCount = readBe32(data + 8);
    if (faceCount == 0 || faceCount > (size - 12) / 4) return false;
    // Face 0 lives at offsets[0]; face N at offsets[N]. A face index beyond
    // the directory count cannot have a table here (and would read outside
    // the offsets array), so treat it as table-missing.
    if (ttcFaceIndex < 0 || uint32_t(ttcFaceIndex) >= faceCount) return false;
    directory = readBe32(data + 12 + size_t(ttcFaceIndex) * 4);
  }
  if (directory > size || size - directory < 12) return false;
  const size_t records = directory + 12;
  const unsigned count = readBe16(data + directory + 4);
  if (count > (size - records) / 16) return false;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t* record = data + records + size_t(i) * 16;
    if (readBe32(record) != tag) continue;
    const size_t offset = readBe32(record + 8);
    const size_t length = readBe32(record + 12);
    if (length == 0 || offset > size || length > size - offset) return false;
    *table = data + offset;
    *tableSize = length;
    return true;
  }
  return false;
}

// #if, not #ifdef: ftmodule.h/ftoption.h gate the actual modules on the
// VALUE of these macros (so `-DFREEINK_FONT_ENABLE_MONOCHROME=0`, a common
// PlatformIO idiom for "explicitly off", compiles nothing in), and these
// capability flags have to agree exactly or setRenderOptions() reports a
// mode as supported when the module that would actually serve it isn't
// there — confirmed: with #ifdef, that exact build config still returned
// true for monochrome while every glyph rasterized to nullptr.
#if FREEINK_FONT_ENABLE_AUTOHINT
constexpr bool kAutohintCompiled = true;
#else
constexpr bool kAutohintCompiled = false;
#endif
#if FREEINK_FONT_ENABLE_NATIVE_HINTING
constexpr bool kNativeHintingCompiled = true;
#else
constexpr bool kNativeHintingCompiled = false;
#endif
#if FREEINK_FONT_ENABLE_MONOCHROME
constexpr bool kMonochromeCompiled = true;
#else
constexpr bool kMonochromeCompiled = false;
#endif

// Two independent axes: the antialiasing TARGET (Normal/Light/Mono — these
// three occupy the same FT_LOAD_TARGET_ bitfield and are mutually exclusive
// with EACH OTHER, so only one is picked), and hint SUPPRESSION/forcing
// (NO_HINTING, NO_AUTOHINT, FORCE_AUTOHINT — independent bits, composed on
// top).
//
// FT_LOAD_NO_HINTING disables ALL hinting, including a harmless, unconditional
// part of it that has nothing to do with any optional module: FreeType's
// TrueType loader always pixel-ROUNDS phantom-point advances when hinting is
// on (ttgload.c's IS_HINTED, gated purely by this one bit), even with the
// bytecode interpreter uncompiled — that's the sub-pixel-accurate advance
// rounding every existing build has always shipped with. Adding NO_HINTING to
// "Default" (an earlier version of this fix did exactly that) silently
// truncates every advance instead of rounding it, changing pagination for
// every caller, in every build — worse than the bug it was meant to close.
//
// FT_LOAD_NO_AUTOHINT is the actually-correct "opt-in has no side effect"
// bit: it blocks FreeType's documented fallback (use the auto-hinter when the
// driver has no native hinter of its own) without touching phantom-point
// rounding. Default sets ONLY this — confirmed byte-identical (0/1615 advance
// and bitmap diffs on a real font) to this library's original, pre-RenderOptions
// FT_LOAD_DEFAULT behavior, in both a build with the autofit/native modules
// compiled and one without. None is a distinct, EXPLICIT opt-in for "truly no
// hinting, not even phantom-point rounding" — safe to be more aggressive than
// Default because a caller has to ask for it by name.
FT_Int32 loadFlagsFor(const FtFont::RenderOptions& options) {
  FT_Int32 flags = FT_LOAD_NO_BITMAP;
  if (options.monochrome) {
    flags |= FT_LOAD_TARGET_MONO;
  } else if (options.hinting == FtFont::HintingMode::Light) {
    flags |= FT_LOAD_TARGET_LIGHT;  // FreeType always auto-hints under LIGHT
  } else {
    flags |= FT_LOAD_TARGET_NORMAL;
  }
  switch (options.hinting) {
    case FtFont::HintingMode::Auto:
      flags |= FT_LOAD_FORCE_AUTOHINT;
      break;
    case FtFont::HintingMode::None:
      flags |= FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
      break;
    case FtFont::HintingMode::Light:
      // TARGET_LIGHT forces auto-hint on its own — except monochrome output
      // above already claimed the target slot LIGHT would have used (in
      // which case this reduces to Auto+mono; there is no separate "light"
      // render mode to preserve), so ask for it explicitly under mono too.
      if (options.monochrome) flags |= FT_LOAD_FORCE_AUTOHINT;
      break;
    case FtFont::HintingMode::Native:
    case FtFont::HintingMode::Default:
    default:
      // Identical flags by necessity, not by accident: once a native hinter
      // is compiled in and registered, FreeType uses it automatically for
      // any hinted (non-NO_HINTING) load unless autohint is force-requested
      // — there is no third way to say "use native but only if I explicitly
      // asked", so Native and Default only diverge through which interpreter
      // version gets applied (see applyGlobalProperties()), not through
      // these flags. Both explicitly block the autohint fallback so that
      // compiling FREEINK_FONT_ENABLE_AUTOHINT in never changes a Default
      // caller's output.
      flags |= FT_LOAD_NO_AUTOHINT;
      break;
  }
  return flags;
}

FT_Render_Mode renderModeFor(const FtFont::RenderOptions& options) {
  return options.monochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
}

struct StreamCtx {
  FtFont::ReadFn read;
  void* ctx;
  bool failed;
};

// FT_Stream io hook: FreeType asks for `count` bytes at absolute `offset`.
// count == 0 is a seek FreeType tracks itself, so nothing to do.
unsigned long ftStreamIo(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
  if (count == 0) return 0;
  auto* c = static_cast<StreamCtx*>(stream->descriptor.pointer);
  // FreeType probes past end-of-file and treats a short read there as
  // end-of-data (FT_Stream_TryRead), so clamp the expected length to the
  // container size: only a shortfall inside the file is an I/O error.
  if (offset >= stream->size) return 0;
  const unsigned long expected =
      count > stream->size - offset ? static_cast<unsigned long>(stream->size - offset) : count;
  const unsigned long actual = c->read(c->ctx, offset, buffer, expected);
  if (actual != expected) c->failed = true;
  return actual;
}
// The source (e.g. an SD file) is owned by the caller, not FreeType.
void ftStreamClose(FT_Stream) {}

bool supportedFace(FT_Face face) {
  return face && FT_IS_SCALABLE(face) && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0;
}

void describeFace(FT_Face face, FtFont::FaceInfo& info, char* family, const size_t familyCapacity) {
  info = FtFont::FaceInfo{};
  const char* name = face->family_name ? face->family_name : "";
  info.familyLength = std::strlen(name);
  if (familyCapacity) {
    const size_t copied = std::min(info.familyLength, familyCapacity - 1);
    if (copied) std::memcpy(family, name, copied);
    family[copied] = '\0';
  }
  const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
  info.weight = os2 ? os2->usWeightClass : ((face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400);
  info.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
}

int32_t fixed16_16To26_6(const long value) {
  const int64_t wide = value;
  const int64_t rounded = wide >= 0 ? (wide + 512) >> 10 : -((-wide + 512) >> 10);
  return int32_t(std::clamp<int64_t>(rounded, INT32_MIN, INT32_MAX));
}

// Opens faces 0..num_faces-1 until one passes supportedFace (Unicode cmap;
// this is what "first face with a Unicode cmap" means for a .ttc),
// describes it, and fills info.numFaces / info.faceIndex. faceIndex >= 0
// pins the exact face; faceIndex < 0 scans. containerFaces is the face count
// declared by the input's container header (ttcHeaderFaceCount). Returns
// false when the requested face cannot be opened or no face is supported.
// *openErr (optional) carries the terminal FT error — FT_Err_Out_Of_Memory
// and FT_Err_Invalid_Stream_Read in particular, so callers can map resource
// failures to InspectResult::Unavailable instead of bad font data.
template <typename OpenFn>
static bool inspectFaceAt(const OpenFn& openAt, const int faceIndex, const long containerFaces, FtFont::FaceInfo& info,
                          char* family, const size_t familyCapacity, FT_Error* openErr = nullptr) {
  FT_Face face = nullptr;
  const long pin = faceIndex < 0 ? 0 : faceIndex;
  const FT_Error pinErr = openAt(pin, face);
  if (openErr) *openErr = pinErr;
  // Exact mode: a failed open is the answer. Scan mode: an unparseable face
  // 0 is retried only for a real ttcf container (later members may be
  // valid) — OOM and unreadable-stream errors always stop the scan, and an
  // out-of-range index means the container's member list is exhausted (the
  // loop starts past the already-failed pin).
  if (pinErr != 0 &&
      (faceIndex >= 0 || pinErr == FT_Err_Out_Of_Memory || pinErr == FT_Err_Invalid_Stream_Read || containerFaces <= 1))
    return false;
  // Never scan past what the container header declares, and keep the 16-bit
  // face-index cap: a crafted .ttc claiming huge counts would otherwise make
  // this loop O(N²) in streamed open cost for nothing but a wrapped index.
  long last = std::min<long>(containerFaces - 1, UINT16_MAX);
  if (face) {
    const long numFaces = face->num_faces > 0 ? face->num_faces : 1;
    last = faceIndex >= 0 ? faceIndex : std::min(last, numFaces - 1);
  }
  for (long i = pinErr == 0 ? pin : pin + 1; i <= last; ++i) {
    if (i != pin) {
      if (face) FT_Done_Face(face);
      face = nullptr;
      // An unparseable member face is not terminal (later members may be
      // valid); stop early only when FreeType ran out of memory or the
      // index left the container's member range.
      const FT_Error nextErr = openAt(i, face);
      if (openErr) *openErr = nextErr;
      if (nextErr != 0) {
        if (nextErr == FT_Err_Out_Of_Memory || nextErr == FT_Err_Invalid_Argument ||
            nextErr == FT_Err_Invalid_Stream_Read)
          return false;
        continue;
      }
      if (last == UINT16_MAX) {
        // First successful open on the retry path: num_faces is finally
        // known, so the scan can be bounded by this container's real count.
        const long numFaces = face->num_faces > 0 ? face->num_faces : 1;
        last = std::min<long>(numFaces - 1, UINT16_MAX);
        if (i > last) {
          FT_Done_Face(face);
          return false;
        }
      }
    }
    const long numFaces = face->num_faces > 0 ? face->num_faces : 1;
    if (!supportedFace(face)) {
      FT_Done_Face(face);
      face = nullptr;
      if (faceIndex >= 0) return false;
      continue;
    }
    // 16-bit FaceInfo fields: values that cannot be represented must fail
    // closed rather than wrap (face 65536 would report as face 0).
    if (i > UINT16_MAX || numFaces - 1 > UINT16_MAX) {
      FT_Done_Face(face);
      return false;
    }
    describeFace(face, info, family, familyCapacity);
    info.faceIndex = static_cast<uint16_t>(i);
    info.numFaces = static_cast<uint16_t>(numFaces);
    FT_Done_Face(face);
    return true;
  }
  if (face) FT_Done_Face(face);
  return false;
}
}  // namespace

bool FtFont::configureMemory(const MemoryCallbacks* callbacks) {
  if (g_lib) return false;
  if (!callbacks) {
    g_memoryCallbacks = MemoryCallbacks{};
    return true;
  }
  if (!callbacks->allocate || !callbacks->deallocate || !callbacks->reallocate) return false;
  g_memoryCallbacks = *callbacks;
  return true;
}

FtFont::InspectResult FtFont::inspectMemory(const uint8_t* data, const uint32_t length, FaceInfo& info, char* family,
                                            const size_t familyCapacity, const int faceIndex) {
  info = FaceInfo{};
  if (familyCapacity && !family) return InspectResult::Unsupported;
  if (familyCapacity) family[0] = '\0';
  if (!data || !length) return InspectResult::Unsupported;
  if (!ensureLib()) return InspectResult::Unavailable;

  const auto openAtIndex = [&](const long index, FT_Face& face) {
    return FT_New_Memory_Face(g_lib, data, static_cast<FT_Long>(length), index, &face);
  };
  FT_Error openErr = FT_Err_Ok;
  if (!inspectFaceAt(openAtIndex, faceIndex, ttcHeaderFaceCount(data, length), info, family, familyCapacity,
                     &openErr)) {
    return openErr == FT_Err_Out_Of_Memory ? InspectResult::Unavailable : InspectResult::Unsupported;
  }
  return InspectResult::Ok;
}

FtFont::InspectResult FtFont::inspectStream(const ReadFn read, void* ctx, const unsigned long fileSize, FaceInfo& info,
                                            char* family, const size_t familyCapacity, const int faceIndex) {
  info = FaceInfo{};
  if (familyCapacity && !family) return InspectResult::Unsupported;
  if (familyCapacity) family[0] = '\0';
  if (!read || !fileSize) return InspectResult::Unsupported;
  if (!ensureLib()) return InspectResult::Unavailable;

  StreamCtx source{read, ctx, false};
  FT_StreamRec stream{};
  stream.size = fileSize;
  stream.descriptor.pointer = &source;
  stream.read = &ftStreamIo;
  stream.close = &ftStreamClose;
  FT_Open_Args args{};
  args.flags = FT_OPEN_STREAM;
  args.stream = &stream;
  // Identify the container from the header so scan-mode retries stay bounded
  // (a non-ttcf stream counts as one face and never scans past 0).
  uint8_t header[12];
  long containerFaces = 1;
  if (read(ctx, 0, header, sizeof(header)) == sizeof(header))
    containerFaces = ttcHeaderFaceCount(header, sizeof(header));

  const auto openAtIndex = [&](const long index, FT_Face& face) {
    source.failed = false;
    const FT_Error err = FT_Open_Face(g_lib, &args, index, &face);
    // A short read while opening this face is terminal for the attempt:
    // scanning onward would silently skip a face backed by unreadable data.
    return err != 0 && source.failed ? FT_Err_Invalid_Stream_Read : err;
  };
  FT_Error openErr = FT_Err_Ok;
  if (!inspectFaceAt(openAtIndex, faceIndex, containerFaces, info, family, familyCapacity, &openErr)) {
    return source.failed || openErr == FT_Err_Out_Of_Memory ? InspectResult::Unavailable : InspectResult::Unsupported;
  }
  return source.failed ? InspectResult::Unavailable : InspectResult::Ok;
}

FtFont::~FtFont() { deinit(); }

void FtFont::deinit() {
  if (face_) {
    FT_Done_Face(static_cast<FT_Face>(face_));
    face_ = nullptr;
  }
  fontFree(stream_);
  stream_ = nullptr;
  fontFree(streamCtx_);
  streamCtx_ = nullptr;
  ready_ = false;
  size26_6_ = 0;
  obliqueShear_ = false;
  options_ = RenderOptions{};
  // Glyph IDs are face-local (same reasoning as the GSUB table below): a
  // cached bitmap from the PREVIOUS face would be served for the new face's
  // completely different glyph IDs.
  flushGlyphCache();
  freeMonoBuffer();
  fontFree(bitmapBacking_);
  bitmapBacking_ = nullptr;
  bitmapBackingCap_ = 0;
  glyph_ = {};
  // Glyph IDs are face-local: a stale GSUB table cached from the PREVIOUS
  // face would resolve the new face's glyph IDs against the wrong font,
  // silently either finding nothing or (worse) matching a coincidentally
  // reused ID for a wrong-but-real ligature. Must be cleared on every
  // deinit(), since init()/initStream() can rebuild this same instance with
  // completely different bytes.
  freeGsubTable();
  gsubLoadAttempted_ = false;
  freeGposTable();
  gposLoadAttempted_ = false;
  fontData_ = nullptr;
  fontDataSize_ = 0;
}

void FtFont::freeGsubTable() {
  if (gsubTableOwned_) fontFree(const_cast<uint8_t*>(gsubTable_));
  gsubTable_ = nullptr;
  gsubTableSize_ = 0;
  gsubTableOwned_ = false;
}

void FtFont::setGlyphCacheBudget(const size_t maxBytes) {
  cacheBudget_ = maxBytes > kMaxGlyphCacheBudget ? kMaxGlyphCacheBudget : maxBytes;
  if (!cacheBudget_) flushGlyphCache();
  while (cacheBytes_ > cacheBudget_ && cacheOldest_) evictOldestCachedGlyph();
}

// Moves the entry the last rasterize() handed out into bitmapBacking_ before
// its cache block is freed, so the RasterFont "valid until the next
// rasterize()" contract holds across eviction and flushes too (no eviction
// exception). Mirrors preserveGlyphBitmap()'s growth-then-copy; allocation
// failure degrades to an invalid bitmap rather than a dangling one.
void FtFont::retainLiveGlyphCoverage(const FtFont::GlyphCacheEntry* entry) {
  const size_t bytes = entry->pixelBytes;
  if (bytes == 0 || entry->pixels == nullptr) return;
  if (bytes > bitmapBackingCap_) {
    void* grown = fontRealloc(bitmapBacking_, bitmapBackingCap_, bytes);
    if (grown == nullptr) {
      glyphEntry_ = nullptr;
      glyph_ = {};  // degrade to invalid; never dangle
      return;
    }
    bitmapBacking_ = static_cast<uint8_t*>(grown);
    bitmapBackingCap_ = bytes;
  }
  memcpy(bitmapBacking_, entry->pixels, bytes);
  glyph_.pixels = bitmapBacking_;
  glyphEntry_ = nullptr;
}

void FtFont::flushGlyphCache() {
  if (glyphEntry_) retainLiveGlyphCoverage(glyphEntry_);
  GlyphCacheEntry* entry = cacheNewest_;
  while (entry) {
    GlyphCacheEntry* older = entry->older;
    fontFree(entry);
    entry = older;
  }
  cacheNewest_ = nullptr;
  cacheOldest_ = nullptr;
  cacheBytes_ = 0;
}

const FtFont::GlyphCacheEntry* FtFont::findCachedGlyph(const GlyphId glyph, const uint32_t pixelSize26_6) {
  for (GlyphCacheEntry* entry = cacheNewest_; entry; entry = entry->older) {
    if (entry->glyph == glyph && entry->pixelSize26_6 == pixelSize26_6) {
      if (entry != cacheNewest_) {
        // Unlink (newer points toward newest, older toward oldest) ...
        if (entry->older)
          entry->older->newer = entry->newer;
        else
          cacheOldest_ = entry->newer;
        if (entry->newer)
          entry->newer->older = entry->older;
        else
          cacheNewest_ = entry->older;
        // ... and prepend at the MRU end.
        entry->older = cacheNewest_;
        entry->newer = nullptr;
        cacheNewest_->newer = entry;
        cacheNewest_ = entry;
      }
      return entry;
    }
  }
  return nullptr;
}

void FtFont::evictOldestCachedGlyph() {
  GlyphCacheEntry* victim = cacheOldest_;
  if (!victim) return;
  cacheOldest_ = victim->newer;
  if (cacheOldest_)
    cacheOldest_->older = nullptr;
  else
    cacheNewest_ = nullptr;
  victim->newer = nullptr;
  cacheBytes_ -= victim->pixelBytes + sizeof(GlyphCacheEntry);
  // The last rasterize() handed out a pointer into the victim: move the
  // coverage into bitmapBacking_ so the "valid until the next rasterize()"
  // contract survives the eviction (no eviction exception in the docs).
  if (glyphEntry_ == victim) retainLiveGlyphCoverage(victim);
  fontFree(victim);
}

void FtFont::setGsubByteBudget(const size_t maxBytes) {
  gsubByteBudget_ = maxBytes;
  if (gsubTableOwned_ && gsubTableSize_ > gsubByteBudget_) releaseLigatureTable();
}

void FtFont::releaseLigatureTable() {
  freeGsubTable();
  gsubLoadAttempted_ = true;
}

void FtFont::freeGposTable() {
  if (gposTableOwned_) fontFree(const_cast<uint8_t*>(gposTable_));
  gposTable_ = nullptr;
  gposTableSize_ = 0;
  gposTableOwned_ = false;
}

void FtFont::setGposByteBudget(const size_t maxBytes) {
  gposByteBudget_ = maxBytes;
  if (gposTableOwned_ && gposTableSize_ > gposByteBudget_) releaseKerningTable();
}

void FtFont::releaseKerningTable() {
  freeGposTable();
  gposLoadAttempted_ = true;
}

bool FtFont::init(const uint8_t* data, const uint32_t len, const uint16_t sizePx, const int weight, const bool italic,
                  const int faceIndex) {
  // Rebuild from a clean slate every time: this same instance can be
  // init()'d again with completely different bytes without an intervening
  // deinit() call, and glyph IDs (so the cached GSUB table) are face-local —
  // a stale table from the PREVIOUS face would resolve the new face's glyph
  // IDs against the wrong font. This also closes a pre-existing leak: a
  // second init() without deinit() previously dropped the old FT_Face
  // without ever calling FT_Done_Face on it.
  deinit();
  if (!ensureLib() || data == nullptr || len == 0) return false;
  if (faceIndex < 0) return false;  // negative indexes have special FT meaning; callers pass concrete indexes
  activeFaceIndex_ = faceIndex;
  fontData_ = data;
  fontDataSize_ = len;
  FT_Face face = nullptr;
  if (FT_New_Memory_Face(g_lib, data, static_cast<FT_Long>(len), faceIndex, &face) != 0) {
    fontData_ = nullptr;
    fontDataSize_ = 0;
    return false;
  }
  face_ = face;
  return finishInit(sizePx, weight, italic);
}

bool FtFont::initStream(const ReadFn read, void* ctx, const unsigned long fileSize, const uint16_t sizePx,
                        const int weight, const bool italic, const int faceIndex) {
  deinit();  // see the comment in init() — same reasoning applies here
  if (!ensureLib() || read == nullptr || fileSize == 0) return false;
  if (faceIndex < 0) return false;
  activeFaceIndex_ = faceIndex;

  // These wrappers must outlive the face, so they use the configured font
  // allocator rather than a small task stack frame. Both allocations are
  // bounded and failure is reported to the caller.
  auto* sc = static_cast<StreamCtx*>(fontAlloc(sizeof(StreamCtx)));
  auto* stream = static_cast<FT_StreamRec*>(fontAlloc(sizeof(FT_StreamRec)));
  if (!sc || !stream) {
    fontFree(stream);
    fontFree(sc);
    return false;
  }
  *sc = StreamCtx{read, ctx, false};
  std::memset(stream, 0, sizeof(*stream));
  stream->size = fileSize;
  stream->pos = 0;
  stream->descriptor.pointer = sc;
  stream->read = &ftStreamIo;
  stream->close = &ftStreamClose;
  streamCtx_ = sc;
  stream_ = stream;

  FT_Open_Args args{};
  args.flags = FT_OPEN_STREAM;
  args.stream = stream;
  FT_Face face = nullptr;
  if (FT_Open_Face(g_lib, &args, faceIndex, &face) != 0) {
    fontFree(stream);
    fontFree(sc);
    stream_ = nullptr;
    streamCtx_ = nullptr;
    return false;
  }
  face_ = face;
  return finishInit(sizePx, weight, italic);
}

bool FtFont::finishInit(const uint16_t sizePx, const int weight, const bool italic) {
  auto face = static_cast<FT_Face>(face_);
  applyVariation(weight, italic);
  size26_6_ = 0;
  if (!ensureSize26_6(uint32_t(sizePx) * 64u)) {
    FT_Done_Face(face);
    face_ = nullptr;
    return false;
  }
  ready_ = true;
  return true;
}

void FtFont::applyVariation(const int weight, const bool italic) {
  auto face = static_cast<FT_Face>(face_);
  obliqueShear_ = false;
  emboldenBold_ = false;
  const bool wantBold = weight >= 600;
  FT_MM_Var* mm = nullptr;
  if (FT_Get_MM_Var(face, &mm) != 0 || mm == nullptr) {
    // Static face: no axes. Italic → oblique shear; bold → per-glyph outline
    // embolden (see rasterize). A separate Bold/Italic file, when the caller
    // supplies one, is loaded at weight 400 upright so neither synthesis fires.
    obliqueShear_ = italic;
    emboldenBold_ = wantBold;
  } else {
    FT_Fixed coords[16];
    const FT_UInt n = mm->num_axis < 16 ? mm->num_axis : 16;
    bool haveItalAxis = false;
    bool haveWghtAxis = false;
    for (FT_UInt i = 0; i < n; ++i) {
      coords[i] = mm->axis[i].def;
      const FT_ULong tag = mm->axis[i].tag;
      if (tag == kTagWght) {
        FT_Fixed w = static_cast<FT_Fixed>(weight) * 65536;
        if (w < mm->axis[i].minimum) w = mm->axis[i].minimum;
        if (w > mm->axis[i].maximum) w = mm->axis[i].maximum;
        coords[i] = w;
        haveWghtAxis = true;
      } else if (italic && tag == kTagItal) {
        coords[i] = mm->axis[i].maximum;  // ital 0..1 → 1
        haveItalAxis = true;
      } else if (italic && tag == kTagSlnt) {
        coords[i] = mm->axis[i].minimum;  // slnt negative = slanted
        haveItalAxis = true;
      }
    }
    FT_Set_Var_Design_Coordinates(face, n, coords);
    FT_Done_MM_Var(g_lib, mm);
    obliqueShear_ = italic && !haveItalAxis;
    emboldenBold_ = wantBold && !haveWghtAxis;  // variable but no wght axis
  }
}

bool FtFont::setRenderOptions(const RenderOptions& options) {
  options_ = options;
  // Cached coverage was rendered under the previous options (hinting, mono
  // mode, embolden/slant all change the bytes).
  flushGlyphCache();
  // Report (without refusing) a request this build can't honor, so a caller
  // with real logging can warn instead of silently getting degraded output —
  // e.g. monochrome with FREEINK_FONT_ENABLE_MONOCHROME off fails every
  // FT_Render_Glyph call and rasterize() returns nullptr for every glyph.
  // Also validated here rather than left to FreeType: an out-of-range
  // interpreterVersion would otherwise fail FT_Property_Set silently deep
  // inside a hot per-glyph path with no way for the caller to find out.
  bool supported = true;
  if (options.monochrome && !kMonochromeCompiled) supported = false;
  if ((options.hinting == HintingMode::Auto || options.hinting == HintingMode::Light) && !kAutohintCompiled)
    supported = false;
  if (options.hinting == HintingMode::Native) {
    if (!kNativeHintingCompiled) supported = false;
    if (options.interpreterVersion != 35 && options.interpreterVersion != 40) supported = false;
  }
  return supported;
}

// Both FT_Property_Set targets live on the shared library, not this face —
// FT_Property_Set has no per-face scope. Applying them here, immediately
// before THIS face's own FT_Load_Char (not once back in setRenderOptions()),
// is what actually prevents cross-face leakage: setting them eagerly at
// setRenderOptions() time meant "whichever face configured itself most
// recently" won for every OTHER face's subsequent renders too, since
// FT_Load_Char merely consumes whatever the properties currently say rather
// than re-deriving them from this instance's own options_. Re-pinning them
// right before the one call that consumes them means only the face actually
// rendering right now can be the one that matters, for that one load.
void FtFont::applyGlobalProperties() const {
  if (!ensureLib()) return;
  FT_Bool noStemDarkening = options_.stemDarkening ? 0 : 1;
  FT_Property_Set(g_lib, "autofitter", "no-stem-darkening", &noStemDarkening);
#if FREEINK_FONT_ENABLE_NATIVE_HINTING
  // The TrueType interpreter version is library-global. Restore it before
  // every load so a face rendered with Native v35 cannot affect a later
  // Default/Auto/None face (Default uses FreeType's normal v40 behavior).
  const FT_UInt version = options_.hinting == HintingMode::Native ? options_.interpreterVersion : 40;
  FT_Property_Set(g_lib, "truetype", "interpreter-version", &version);
#endif
}

void FtFont::freeMonoBuffer() {
  fontFree(monoBuf_);
  monoBuf_ = nullptr;
  monoBufCap_ = 0;
}

void FtFont::preserveGlyphBitmap() {
  if (!face_ || glyph_.pixels == nullptr || glyph_.pixels == bitmapBacking_) return;
  if (glyphEntry_ != nullptr) return;  // cache-owned coverage is stable; nothing to preserve
  auto face = static_cast<FT_Face>(face_);
  if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP) return;
  const FT_Bitmap& bm = face->glyph->bitmap;
  const size_t bytes = static_cast<size_t>(bm.pitch > 0 ? bm.pitch : -bm.pitch) * bm.rows;
  if (bytes == 0) return;
  if (bytes > bitmapBackingCap_) {
    void* grown = fontRealloc(bitmapBacking_, bitmapBackingCap_, bytes);
    if (grown == nullptr) return;  // keep the slot pointer; contract degraded, not corrupted
    bitmapBacking_ = static_cast<uint8_t*>(grown);
    bitmapBackingCap_ = bytes;
  }
  memcpy(bitmapBacking_, bm.buffer, bytes);
  glyph_.pixels = bitmapBacking_;
}

const uint8_t* FtFont::expandMonoCoverage(const void* ftBitmapPtr) {
  const auto& bitmap = *static_cast<const FT_Bitmap*>(ftBitmapPtr);
  const size_t pixels = size_t(bitmap.width) * bitmap.rows;
  if (pixels == 0) return monoBuf_;  // empty glyph (e.g. space): nothing to expand
  if (pixels > monoBufCap_) {
    fontFree(monoBuf_);
    monoBuf_ = static_cast<uint8_t*>(fontAlloc(pixels));
    monoBufCap_ = monoBuf_ ? pixels : 0;
    if (!monoBuf_) return nullptr;
  }
  const int pitch = bitmap.pitch;
  for (unsigned y = 0; y < bitmap.rows; ++y) {
    const uint8_t* row =
        pitch >= 0 ? bitmap.buffer + size_t(y) * pitch : bitmap.buffer + size_t(bitmap.rows - 1 - y) * size_t(-pitch);
    uint8_t* dst = monoBuf_ + size_t(y) * bitmap.width;
    for (unsigned x = 0; x < bitmap.width; ++x) {
      dst[x] = (row[x / 8] & (0x80u >> (x % 8))) ? 0xFF : 0x00;
    }
  }
  return monoBuf_;
}

bool FtFont::ensureSize26_6(const uint32_t pixelSize26_6) {
  if (!face_ || pixelSize26_6 < 64 || pixelSize26_6 > 0xFFFFFFu) return false;
  if (pixelSize26_6 == size26_6_) return true;
  if (FT_Set_Char_Size(static_cast<FT_Face>(face_), pixelSize26_6, pixelSize26_6, 72, 72) != 0) return false;
  size26_6_ = pixelSize26_6;
  // No cache flush here: (glyphId, pixelSize26_6) is the cache key, so
  // entries from other sizes can never be served for this size, and the
  // RasterFont contract keeps the last rasterize() bitmap valid across a
  // size-changing glyphBounds()/advance() — a flush would free it.
  return true;
}

bool FtFont::prepareLoad() {
  if (!ready_) return false;
  applyGlobalProperties();
  const int64_t syntheticItalic = obliqueShear_ ? 13763 : 0;  // tan(about 12 degrees) in 16.16
  const FT_Fixed shear =
      static_cast<FT_Fixed>(std::clamp<int64_t>(syntheticItalic + options_.slant16_16, INT32_MIN, INT32_MAX));
  FT_Matrix matrix{0x10000L, shear, 0, 0x10000L};
  FT_Set_Transform(static_cast<FT_Face>(face_), shear ? &matrix : nullptr, nullptr);
  return true;
}

bool FtFont::loadGlyph(const GlyphId glyph, const uint32_t pixelSize26_6) {
  if (!glyph || !ensureSize26_6(pixelSize26_6) || !prepareLoad()) return false;
  auto face = static_cast<FT_Face>(face_);
  if (FT_Load_Glyph(face, glyph, loadFlagsFor(options_)) != 0) return false;
  FT_GlyphSlot slot = face->glyph;
  const int64_t syntheticBold = emboldenBold_ ? int64_t(pixelSize26_6) / 26 : 0;
  const FT_Pos strength =
      static_cast<FT_Pos>(std::clamp<int64_t>(syntheticBold + options_.embolden26_6, INT32_MIN, INT32_MAX));
  return !strength || slot->format != FT_GLYPH_FORMAT_OUTLINE || FT_Outline_Embolden(&slot->outline, strength) == 0;
}

bool FtFont::hasGlyph(const uint32_t codepoint) const { return glyphId(codepoint) != 0; }

FtFont::GlyphId FtFont::glyphId(const uint32_t codepoint) const {
  return ready_ ? FT_Get_Char_Index(static_cast<FT_Face>(face_), codepoint) : 0;
}

bool FtFont::glyphBounds(const uint32_t codepoint, const uint16_t sizePx, int16_t& xoff, int16_t& yoff,
                         uint16_t& width, uint16_t& height) const {
  if (!ready_) return false;
  // Both ensureSize26_6() (size26_6_) and preserveGlyphBitmap() (glyph_,
  // bitmapBacking_) mutate state the const surface of glyphBounds promises not
  // to touch — the glyph data itself is untouched, so route through a
  // const_cast.
  auto* self = const_cast<FtFont*>(this);
  self->preserveGlyphBitmap();
  if (!self->ensureSize26_6(uint32_t(sizePx) * 64u)) return false;
  auto face = static_cast<FT_Face>(face_);
  if (FT_Get_Char_Index(face, codepoint) == 0) return false;
  // Outline load only: FreeType computes the metrics, no pixels are generated.
  // NO_HINTING: hinting is unnecessary for antialiased e-ink at reading sizes
  // (the stb backend is unhinted too), and the CFF Adobe hinting engine's
  // interpreter has a stack-resident footprint far beyond embedded task
  // budgets — unhinted loads never enter it.
  if (FT_Load_Char(face, codepoint, FT_LOAD_NO_HINTING) != 0) return false;
  if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;  // bitmap strike
  // Control box of the loaded outline in 26.6. Use the outline cbox rather
  // than glyph->metrics: the cbox reflects the FT_Set_Transform shear applied
  // for faux italic, which the metrics fields do not. Control points bound
  // the on-curve ink from outside, so the cbox ⊇ the rasterized bitmap.
  FT_BBox cbox;
  FT_Outline_Get_CBox(&face->glyph->outline, &cbox);
  FT_Pos x0 = cbox.xMin;
  FT_Pos x1 = cbox.xMax;
  FT_Pos y1 = cbox.yMax;             // top edge above baseline
  FT_Pos y0 = cbox.yMin;             // bottom edge (negative = below baseline)
  // The rendered bitmap can differ from the outline by up to a pixel of
  // hinting rounding, and faux bold (FT_Outline_Embolden in loadGlyph) grows
  // the outline by the stroke strength, so pad by 1 px per side plus the
  // embolden allowance to keep the documented invariant that the glyphBounds
  // box contains the rasterize box.
  FT_Pos pad = 64;  // hinting-rounding allowance
  if (emboldenBold_) pad += static_cast<FT_Pos>(sizePx) * 32 / 26;  // strength/2
  x0 -= pad;
  x1 += pad;
  y0 -= pad;
  y1 += pad;
  // Convert to whole pixels rounding OUTWARD (26.6 → px): floors on the
  // left/bottom edges, ceils on the right/top. Arithmetic shift = floor, so
  // (v + 63) >> 6 is ceil for negative v too.
  const FT_Pos x0px = x0 >> 6;
  const FT_Pos x1px = (x1 + 63) >> 6;
  const FT_Pos topPx = (y1 + 63) >> 6;
  const FT_Pos botPx = y0 >> 6;
  const FT_Pos w = x1px - x0px;
  const FT_Pos h = topPx - botPx;
  const FT_Pos yoffPos = -topPx;
  // The public contract narrows to int16 offsets and uint16 extents; reject
  // boxes that cannot be represented instead of letting the casts wrap
  // (reachable via a huge sizePx).
  constexpr FT_Pos kOffMin = std::numeric_limits<int16_t>::min();
  constexpr FT_Pos kOffMax = std::numeric_limits<int16_t>::max();
  constexpr FT_Pos kExtentMax = std::numeric_limits<uint16_t>::max();
  if (x0px < kOffMin || x0px > kOffMax || yoffPos < kOffMin || yoffPos > kOffMax || w > kExtentMax ||
      h > kExtentMax) {
    return false;
  }
  xoff = static_cast<int16_t>(x0px);
  yoff = static_cast<int16_t>(yoffPos);
  width = static_cast<uint16_t>(w);
  height = static_cast<uint16_t>(h);
  return true;
}

bool FtFont::metrics26_6(const uint32_t codepoint, const uint32_t pixelSize26_6, GlyphMetrics& out) {
  return metricsGlyph26_6(glyphId(codepoint), pixelSize26_6, out);
}

bool FtFont::metricsGlyph26_6(const GlyphId glyph, const uint32_t pixelSize26_6, GlyphMetrics& out) {
  out = GlyphMetrics{};
  if (!loadGlyph(glyph, pixelSize26_6)) return false;
  auto slot = static_cast<FT_Face>(face_)->glyph;
  long left = 0;
  long right = 0;
  long bottom = 0;
  long top = 0;
  if (options_.monochrome || slot->format != FT_GLYPH_FORMAT_OUTLINE) {
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, renderModeFor(options_)) != 0) return false;
    left = slot->bitmap_left;
    right = left + slot->bitmap.width;
    top = slot->bitmap_top;
    bottom = top - slot->bitmap.rows;
  } else {
    FT_BBox box;
    FT_Outline_Get_CBox(&slot->outline, &box);
    left = box.xMin >> 6;
    right = (box.xMax + 63) >> 6;
    bottom = box.yMin >> 6;
    top = (box.yMax + 63) >> 6;
  }
  if (right < left || top < bottom || right - left > UINT16_MAX || top - bottom > UINT16_MAX || left < INT16_MIN ||
      left > INT16_MAX || top < INT16_MIN || top > INT16_MAX)
    return false;
  out = {fixed16_16To26_6(slot->linearHoriAdvance), int16_t(left), int16_t(top), uint16_t(right - left),
         uint16_t(top - bottom)};
  return true;
}

int32_t FtFont::kerning26_6(const uint32_t left, const uint32_t right, const uint32_t pixelSize26_6) {
  return kerningGlyphs26_6(glyphId(left), glyphId(right), pixelSize26_6);
}

int32_t FtFont::kerningGlyphs26_6(const GlyphId left, const GlyphId right, const uint32_t pixelSize26_6) {
  if (!left || !right || !ensureSize26_6(pixelSize26_6)) return 0;
  auto face = static_cast<FT_Face>(face_);
  if (FT_HAS_KERNING(face)) {
    FT_Vector value{};
    if (FT_Get_Kerning(face, left, right, FT_KERNING_UNFITTED, &value) == 0 && value.x != 0) {
      return int32_t(std::clamp<int64_t>(value.x, INT32_MIN, INT32_MAX));
    }
  }
  // GPOS fallback: modern fonts carry pair kerning only in the GPOS 'kern'
  // feature, which FT_Get_Kerning (legacy 'kern' table only) cannot see.
  // The parser returns font units; x_scale converts to 26.6 at this size.
  ensureGposLoaded();
  if (!gposTable_) return 0;
  const int32_t funits = gpos::PairKernAdjustment(gposTable_, gposTableSize_, left, right);
  if (funits == 0) return 0;
  return int32_t(std::clamp<FT_Long>(FT_MulFix(funits, face->size->metrics.x_scale), INT32_MIN, INT32_MAX));
}

bool FtFont::lineMetrics26_6(const uint32_t pixelSize26_6, LineMetrics& out) {
  out = LineMetrics{};
  if (!ensureSize26_6(pixelSize26_6)) return false;
  const auto& metrics = static_cast<FT_Face>(face_)->size->metrics;
  out = {int32_t(std::clamp<int64_t>(metrics.ascender, INT32_MIN, INT32_MAX)),
         int32_t(std::clamp<int64_t>(metrics.descender, INT32_MIN, INT32_MAX)),
         int32_t(std::clamp<int64_t>(metrics.height, INT32_MIN, INT32_MAX))};
  return true;
}

int16_t FtFont::advance(const uint32_t codepoint, const uint16_t sizePx, uint8_t) {
  // The metrics load below clobbers the glyph-slot bitmap; hand the last
  // rasterize() bitmap to the caller before it goes (RasterFont contract).
  preserveGlyphBitmap();
  GlyphMetrics metrics;
  if (!metrics26_6(codepoint, uint32_t(sizePx) * 64u, metrics)) return 0;
  return int16_t(std::clamp<int32_t>(metrics.advance26_6 >> 6, INT16_MIN, INT16_MAX));
}

int16_t FtFont::lineHeight(const uint16_t sizePx) {
  LineMetrics metrics;
  return lineMetrics26_6(uint32_t(sizePx) * 64u, metrics)
             ? int16_t(std::clamp<int32_t>(metrics.height26_6 >> 6, INT16_MIN, INT16_MAX))
             : int16_t(sizePx);
}

int16_t FtFont::ascent(const uint16_t sizePx) {
  LineMetrics metrics;
  return lineMetrics26_6(uint32_t(sizePx) * 64u, metrics)
             ? int16_t(std::clamp<int32_t>(metrics.ascender26_6 >> 6, INT16_MIN, INT16_MAX))
             : int16_t(sizePx);
}

int16_t FtFont::kerning(const uint32_t left, const uint32_t right, const uint16_t sizePx, uint8_t) {
  if (!ready_) return 0;
  // Route through the glyph-ID form so the integer-pixel Font API sees the
  // GPOS fallback too; round the 26.6 result to whole pixels.
  const int32_t value = kerningGlyphs26_6(glyphId(left), glyphId(right), uint32_t(sizePx) * 64u);
  return int16_t(std::clamp<int32_t>((value + 32) >> 6, INT16_MIN, INT16_MAX));
}

void FtFont::ensureGsubLoaded() {
  // Only remember a real attempt: setting the flag before `ready_` is
  // confirmed would permanently skip loading if this got called too early
  // (e.g. a caller probing ligature() before init() succeeded), with no way
  // to retry once the face actually becomes ready.
  if (!ready_ || gsubLoadAttempted_) return;
  gsubLoadAttempted_ = true;

  // Memory-backed faces can inspect the caller-owned sfnt directory directly.
  // This keeps the GSUB table shared across all styled faces over one resident
  // font instead of allocating a duplicate copy.
  if (fontData_ != nullptr) {
    const uint8_t* table = nullptr;
    size_t tableSize = 0;
    // The face actually opened is collection member `activeFaceIndex_`;
    // the GSUB directory to read is that member's, not offsets[0].
    if (findSfntTable(fontData_, fontDataSize_, kTagGsub, &table, &tableSize, activeFaceIndex_) &&
        tableSize <= kMaxGsubBytes) {
      gsubTable_ = table;
      gsubTableSize_ = tableSize;
    }
    return;
  }

  auto face = static_cast<FT_Face>(face_);
  FT_ULong length = 0;
  // First call with a null buffer just reports the table's length.
  if (FT_Load_Sfnt_Table(face, kTagGsub, 0, nullptr, &length) != 0 || length == 0 || length > kMaxGsubBytes ||
      length > gsubByteBudget_) {
    return;
  }
  // Fallible allocation, deliberately not PsramVector (see the header
  // comment): a missing GSUB table just means no ligatures, not a crash.
  auto* buffer = static_cast<uint8_t*>(fontAlloc(length));
  if (!buffer) return;
  if (FT_Load_Sfnt_Table(face, kTagGsub, 0, buffer, &length) != 0) {
    fontFree(buffer);
    return;
  }
  gsubTable_ = buffer;
  gsubTableSize_ = length;
  gsubTableOwned_ = true;
}

void FtFont::ensureGposLoaded() {
  // Mirrors ensureGsubLoaded(), including the ready_-before-flag ordering:
  // a probe before init() must stay retryable once the face is live.
  if (!ready_ || gposLoadAttempted_) return;
  gposLoadAttempted_ = true;

  if (fontData_ != nullptr) {
    const uint8_t* table = nullptr;
    size_t tableSize = 0;
    if (findSfntTable(fontData_, fontDataSize_, kTagGpos, &table, &tableSize, activeFaceIndex_) &&
        tableSize <= kMaxGsubBytes) {
      gposTable_ = table;
      gposTableSize_ = tableSize;
    }
    return;
  }

  auto face = static_cast<FT_Face>(face_);
  FT_ULong length = 0;
  if (FT_Load_Sfnt_Table(face, kTagGpos, 0, nullptr, &length) != 0 || length == 0 || length > kMaxGsubBytes ||
      length > gposByteBudget_) {
    return;
  }
  // Fallible allocation, same policy as the GSUB copy: a missing GPOS table
  // just means no pair kerning, not a crash.
  auto* buffer = static_cast<uint8_t*>(fontAlloc(length));
  if (!buffer) return;
  if (FT_Load_Sfnt_Table(face, kTagGpos, 0, buffer, &length) != 0) {
    fontFree(buffer);
    return;
  }
  gposTable_ = buffer;
  gposTableSize_ = length;
  gposTableOwned_ = true;
}

uint32_t FtFont::ligatureGlyphId(const uint32_t* codepoints, const unsigned length) {
  if (!ready_ || codepoints == nullptr || length < 2 || length > 3) return 0;
  ensureGsubLoaded();
  if (!gsubTable_) return 0;
  auto face = static_cast<FT_Face>(face_);
  uint32_t glyphs[3];
  for (unsigned i = 0; i < length; ++i) {
    const FT_UInt g = FT_Get_Char_Index(face, codepoints[i]);
    if (g == 0) return 0;
    glyphs[i] = g;
  }
  return gsub::LigatureGlyphId(gsubTable_, gsubTableSize_, glyphs, length);
}

uint32_t FtFont::ligature(const uint32_t left, const uint32_t right, uint8_t) {
  if (!ready_) return 0;
  auto face = static_cast<FT_Face>(face_);

  // Font::ligature() only ever sees a pair, chaining through a previous
  // result for a 3-glyph ligature ("ff"+"i" using the U+FB00 "ff" result as
  // the new left) — see Font.h. A real GSUB table keys "ffi" on the original
  // 3 letters (f, f, i), not on the already-substituted "ff" glyph, so a
  // chained call is rewritten back to its 3 original codepoints before
  // asking GSUB, instead of querying GSUB for a rule no font actually has.
  uint32_t sequence[3];
  unsigned length;
  uint32_t candidates[2];
  unsigned candidateCount;
  if (left == 0xFB00 && (right == 'i' || right == 'l')) {  // previous step already found "ff"
    sequence[0] = 'f';
    sequence[1] = 'f';
    sequence[2] = right;
    length = 3;
    candidates[0] = right == 'i' ? 0xFB03 : 0xFB04;  // ffi / ffl
    candidateCount = 2;
  } else if (left == 'f' && (right == 'f' || right == 'i' || right == 'l')) {
    sequence[0] = left;
    sequence[1] = right;
    length = 2;
    candidates[1] = 0;
    candidateCount = 1;
    candidates[0] = right == 'f' ? 0xFB00 : right == 'i' ? 0xFB01 : 0xFB02;  // ff / fi / fl
  } else {
    // Not one of the five pairs this bridges to a codepoint at all — return
    // early instead of asking GSUB a question whose answer (if any) could
    // never be expressed through this contract anyway, and instead of
    // defaulting `candidates[0]` to 0xFB00, which would falsely credit an
    // unrelated pair's real GSUB match to the "ff" ligature codepoint.
    return 0;
  }

  const uint32_t ligatureGlyph = ligatureGlyphId(sequence, length);
  if (ligatureGlyph == 0) return 0;

  // Font::ligature() must return a real Unicode codepoint — the layout pass
  // bakes it into cached page text — but GSUB substitution glyphs commonly
  // have no cmap entry at all. Confirm one of the standard Latin ligature
  // codepoints actually maps to the glyph GSUB just named; if none does,
  // there is nothing this contract can hand back even though GSUB matched.
  // A glyph-indexed consumer that doesn't need a codepoint can call
  // ligatureGlyphId() directly instead and use `ligatureGlyph` as-is.
  for (unsigned i = 0; i < candidateCount; ++i) {
    if (FT_Get_Char_Index(face, candidates[i]) == ligatureGlyph) return candidates[i];
  }
  return 0;
}

const GlyphBitmap* FtFont::rasterize(const uint32_t codepoint, const uint16_t sizePx) {
  return rasterize26_6(codepoint, uint32_t(sizePx) * 64u);
}

const GlyphBitmap* FtFont::rasterize26_6(const uint32_t codepoint, const uint32_t pixelSize26_6) {
  return rasterizeGlyph26_6(glyphId(codepoint), pixelSize26_6);
}

const GlyphBitmap* FtFont::rasterizeGlyph26_6(const GlyphId glyph, const uint32_t pixelSize26_6) {
  if (const GlyphCacheEntry* hit = findCachedGlyph(glyph, pixelSize26_6)) {
    glyph_ = GlyphBitmap{hit->pixels, hit->width, hit->height, hit->xoff, hit->yoff, hit->advance};
    glyphEntry_ = hit;
    return &glyph_;
  }
  if (!loadGlyph(glyph, pixelSize26_6)) return nullptr;
  FT_GlyphSlot s = static_cast<FT_Face>(face_)->glyph;
  if (s->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(s, renderModeFor(options_)) != 0) return nullptr;
  if (s->bitmap.width > UINT16_MAX || s->bitmap.rows > UINT16_MAX || s->bitmap_left < INT16_MIN ||
      s->bitmap_left > INT16_MAX || s->bitmap_top < INT16_MIN || s->bitmap_top > INT16_MAX ||
      (s->advance.x >> 6) < INT16_MIN || (s->advance.x >> 6) > INT16_MAX)
    return nullptr;
  // GlyphBitmap's contract is 8-bit coverage (see Font.h); FT_PIXEL_MODE_MONO
  // is 1-bpp packed and must be expanded, not published as-is.
  if (s->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
    const uint8_t* expanded = expandMonoCoverage(&s->bitmap);
    if (!expanded && s->bitmap.width && s->bitmap.rows) return nullptr;  // allocation failure
    glyph_.pixels = expanded;
  } else {
    glyph_.pixels = s->bitmap.buffer;  // 8-bit alpha; valid until next load
  }
  glyph_.width = static_cast<uint16_t>(s->bitmap.width);
  glyph_.height = static_cast<uint16_t>(s->bitmap.rows);
  glyph_.xoff = static_cast<int16_t>(s->bitmap_left);
  glyph_.yoff = static_cast<int16_t>(-s->bitmap_top);  // top offset from baseline, negative = above
  glyph_.advance = static_cast<int16_t>(s->advance.x >> 6);
  glyphEntry_ = nullptr;

  // Copy the rendered coverage out of the (about-to-be-reused) FT slot into
  // one cache block: entry struct + compact width-stride pixels. On
  // allocation failure or budget rejection the glyph stays served from the
  // slot / monoBuf_ exactly as before — never-cached is always safe.
  const size_t pixelBytes = size_t(glyph_.width) * glyph_.height;
  // Single-glyph size guard (CWE-400): one entry can never exceed the whole
  // budget. Allocate, then evict least-recent entries until it fits.
  if (cacheBudget_ && pixelBytes && uint32_t(pixelBytes) + sizeof(GlyphCacheEntry) <= cacheBudget_) {
    auto* entry = static_cast<GlyphCacheEntry*>(fontAlloc(sizeof(GlyphCacheEntry) + pixelBytes));
    if (entry) {
      entry->pixels = reinterpret_cast<uint8_t*>(entry + 1);
      const size_t copyPitch = s->bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                                   ? size_t(glyph_.width)  // monoBuf_: compact stride already
                                   : size_t(s->bitmap.pitch > 0 ? s->bitmap.pitch : -s->bitmap.pitch);
      for (unsigned y = 0; y < glyph_.height; ++y) {
        // FT_Bitmap::pitch is a SIGNED row step: a negative pitch walks the
        // rows upward from the end of the buffer (mirror expandMonoCoverage).
        const uint8_t* row = s->bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                                 ? glyph_.pixels + size_t(y) * copyPitch
                                 : (s->bitmap.pitch >= 0 ? glyph_.pixels + size_t(y) * copyPitch
                                                         : glyph_.pixels + size_t(glyph_.height - 1 - y) * copyPitch);
        memcpy(entry->pixels + size_t(y) * glyph_.width, row, glyph_.width);
      }
      entry->older = cacheNewest_;
      entry->newer = nullptr;
      entry->glyph = glyph;
      entry->pixelSize26_6 = pixelSize26_6;
      entry->width = glyph_.width;
      entry->height = glyph_.height;
      entry->xoff = glyph_.xoff;
      entry->yoff = glyph_.yoff;
      entry->advance = glyph_.advance;
      entry->pixelBytes = static_cast<uint32_t>(pixelBytes);
      if (cacheNewest_)
        cacheNewest_->newer = entry;
      else
        cacheOldest_ = entry;
      cacheNewest_ = entry;
      cacheBytes_ += pixelBytes + sizeof(GlyphCacheEntry);
      glyph_.pixels = entry->pixels;
      glyphEntry_ = entry;
      while (cacheBytes_ > cacheBudget_ && cacheOldest_ != entry) evictOldestCachedGlyph();
    }
  }
  return &glyph_;
}

}  // namespace font
}  // namespace freeink
