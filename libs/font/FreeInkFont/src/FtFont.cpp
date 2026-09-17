#include "FtFont.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H

#include "FontAlloc.h"

#include <limits>

namespace freeink {
namespace font {

namespace {
// FreeType memory hooks → the PSRAM-preferring font allocator. Routing the
// library's allocations (face tables, glyph rasterization, the streaming frame
// cache, MM_Var, ...) to PSRAM keeps internal SRAM free on boards that have it,
// and falls back to internal RAM where they don't. This replaces FreeType's
// default malloc-backed ftsystem, so ALL of FreeType's memory follows suit.
void* ftAlloc(FT_Memory, long size) { return fiFontMalloc(static_cast<size_t>(size)); }
void ftFree(FT_Memory, void* block) { fiFontFree(block); }
void* ftRealloc(FT_Memory, long, long new_size, void* block) {
  return fiFontRealloc(block, static_cast<size_t>(new_size));
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

struct StreamCtx {
  FtFont::ReadFn read;
  void* ctx;
};

// FT_Stream io hook: FreeType asks for `count` bytes at absolute `offset`.
// count == 0 is a seek FreeType tracks itself, so nothing to do.
unsigned long ftStreamIo(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
  if (count == 0) return 0;
  auto* c = static_cast<StreamCtx*>(stream->descriptor.pointer);
  return c->read(c->ctx, offset, buffer, count);
}
// The source (e.g. an SD file) is owned by the caller, not FreeType.
void ftStreamClose(FT_Stream) {}
}  // namespace

FtFont::~FtFont() { deinit(); }

void FtFont::deinit() {
  if (face_) {
    FT_Done_Face(static_cast<FT_Face>(face_));
    face_ = nullptr;
  }
  delete static_cast<FT_StreamRec*>(stream_);
  stream_ = nullptr;
  delete static_cast<StreamCtx*>(streamCtx_);
  streamCtx_ = nullptr;
  ready_ = false;
  sizePx_ = 0;
  obliqueShear_ = false;
}

bool FtFont::init(const uint8_t* data, const uint32_t len, const uint16_t sizePx, const int weight, const bool italic) {
  ready_ = false;
  if (!ensureLib() || data == nullptr || len == 0) return false;
  FT_Face face = nullptr;
  if (FT_New_Memory_Face(g_lib, data, static_cast<FT_Long>(len), 0, &face) != 0) return false;
  face_ = face;
  return finishInit(sizePx, weight, italic);
}

bool FtFont::initStream(const ReadFn read, void* ctx, const unsigned long fileSize, const uint16_t sizePx,
                        const int weight, const bool italic) {
  ready_ = false;
  if (!ensureLib() || read == nullptr || fileSize == 0) return false;

  auto* sc = new StreamCtx{read, ctx};
  auto* stream = new FT_StreamRec{};  // zero-initialized
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
  if (FT_Open_Face(g_lib, &args, 0, &face) != 0) return false;
  face_ = face;
  return finishInit(sizePx, weight, italic);
}

bool FtFont::finishInit(const uint16_t sizePx, const int weight, const bool italic) {
  auto face = static_cast<FT_Face>(face_);
  applyVariation(weight, italic);
  sizePx_ = sizePx;
  if (FT_Set_Pixel_Sizes(face, 0, sizePx) != 0) {
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
  if (obliqueShear_) {
    // ~12° oblique: x' = x + 0.21*y.
    FT_Matrix m{0x10000, static_cast<FT_Fixed>(0x10000 * 0.21), 0, 0x10000};
    FT_Set_Transform(face, &m, nullptr);
  }
}

void FtFont::ensureSize(const uint16_t sizePx) {
  if (sizePx != sizePx_ && face_) {
    FT_Set_Pixel_Sizes(static_cast<FT_Face>(face_), 0, sizePx);
    sizePx_ = sizePx;
  }
}

bool FtFont::hasGlyph(const uint32_t codepoint) const {
  if (!ready_) return false;
  return FT_Get_Char_Index(static_cast<FT_Face>(face_), codepoint) != 0;
}

bool FtFont::glyphBounds(const uint32_t codepoint, const uint16_t sizePx, int16_t& xoff, int16_t& yoff,
                         uint16_t& width, uint16_t& height) const {
  if (!ready_) return false;
  // ensureSize only mutates the face's pixel size (the pointee of face_) plus
  // sizePx_, so route the size change through a const_cast — the glyph data
  // itself is untouched.
  const_cast<FtFont*>(this)->ensureSize(sizePx);
  auto face = static_cast<FT_Face>(face_);
  if (FT_Get_Char_Index(face, codepoint) == 0) return false;
  // Outline load only: FreeType computes the metrics, no pixels are generated.
  if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) != 0) return false;
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
  // hinting rounding, and faux bold (FT_Outline_Embolden in rasterize) grows
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

int16_t FtFont::advance(const uint32_t codepoint, const uint16_t sizePx, uint8_t) {
  if (!ready_) return 0;
  ensureSize(sizePx);
  auto face = static_cast<FT_Face>(face_);
  if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) != 0) return 0;
  return static_cast<int16_t>(face->glyph->advance.x >> 6);
}

int16_t FtFont::lineHeight(const uint16_t sizePx) {
  if (!ready_) return sizePx;
  ensureSize(sizePx);
  auto face = static_cast<FT_Face>(face_);
  return static_cast<int16_t>(face->size->metrics.height >> 6);
}

int16_t FtFont::ascent(const uint16_t sizePx) {
  if (!ready_) return sizePx;
  ensureSize(sizePx);
  auto face = static_cast<FT_Face>(face_);
  return static_cast<int16_t>(face->size->metrics.ascender >> 6);
}

int16_t FtFont::kerning(const uint32_t left, const uint32_t right, const uint16_t sizePx, uint8_t) {
  if (!ready_) return 0;
  auto face = static_cast<FT_Face>(face_);
  if (!FT_HAS_KERNING(face)) return 0;
  ensureSize(sizePx);
  const FT_UInt l = FT_Get_Char_Index(face, left);
  const FT_UInt r = FT_Get_Char_Index(face, right);
  if (l == 0 || r == 0) return 0;
  FT_Vector k;
  if (FT_Get_Kerning(face, l, r, FT_KERNING_DEFAULT, &k) != 0) return 0;
  return static_cast<int16_t>(k.x >> 6);
}

const GlyphBitmap* FtFont::rasterize(const uint32_t codepoint, const uint16_t sizePx) {
  if (!ready_) return nullptr;
  ensureSize(sizePx);
  auto face = static_cast<FT_Face>(face_);
  // Faux bold defers rendering: load the outline, thicken it, then render.
  const FT_Int32 loadFlags = emboldenBold_ ? FT_LOAD_DEFAULT : FT_LOAD_RENDER;
  if (FT_Load_Char(face, codepoint, loadFlags) != 0) return nullptr;
  FT_GlyphSlot s = face->glyph;
  if (emboldenBold_) {
    if (s->format == FT_GLYPH_FORMAT_OUTLINE) {
      // ~1/26 em of extra stroke: close to a real bold's stem gain without the
      // glyphs merging at reader sizes.
      FT_Outline_Embolden(&s->outline, static_cast<FT_Pos>(sizePx) * 64 / 26);
    }
    if (s->format != FT_GLYPH_FORMAT_BITMAP) FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL);
  }
  glyph_.pixels = s->bitmap.buffer;                          // 8-bit alpha; valid until next load
  glyph_.width = static_cast<uint16_t>(s->bitmap.width);
  glyph_.height = static_cast<uint16_t>(s->bitmap.rows);
  glyph_.xoff = static_cast<int16_t>(s->bitmap_left);
  glyph_.yoff = static_cast<int16_t>(-s->bitmap_top);        // top offset from baseline, negative = above
  glyph_.advance = static_cast<int16_t>(s->advance.x >> 6);
  return &glyph_;
}

}  // namespace font
}  // namespace freeink
