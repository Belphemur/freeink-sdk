// Host test for TrueType-collection (.ttc) support: inspect* scan/exact-index
// semantics and init()/initStream() face-index selection. The container is
// synthesized from a real TTF fixture — a TTC header whose offsets point at a
// copy of the face with its table directory patched to container-absolute
// offsets (both faces share the same patched body).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FtFont.h"

using freeink::font::FtFont;

namespace {

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

uint32_t rd32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void wr32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

// Wraps one sfnt into a 2-face TTC: header + two offsets pointing at the
// same embedded face whose table offsets are patched to be container-
// absolute (what the TTC format requires).
std::vector<uint8_t> makeTwoFaceTtc(const std::vector<uint8_t>& ttf) {
  // 12-byte TTC header + 2 offsets, face body aligned to 4.
  const uint32_t faceBase = 20;
  std::vector<uint8_t> out(faceBase + ttf.size(), 0);
  std::memcpy(out.data(), "ttcf", 4);
  out[4] = 0;
  out[5] = 1;  // version 1.0
  out[6] = 0;
  out[7] = 0;
  out[8] = 0;
  out[9] = 0;
  out[10] = 0;
  out[11] = 2;  // numFonts = 2
  wr32(out.data() + 12, faceBase);
  wr32(out.data() + 16, faceBase);

  // Patch the face's table directory: every table offset += faceBase.
  const uint16_t numTables = static_cast<uint16_t>((ttf[4] << 8) | ttf[5]);
  std::vector<uint8_t> face = ttf;
  for (size_t i = 0; i < numTables && 12 + 16 * (i + 1) <= face.size(); ++i) {
    const size_t offField = 12 + 16 * i + 8;
    wr32(face.data() + offField, rd32(face.data() + offField) + faceBase);
  }
  std::memcpy(out.data() + faceBase, face.data(), face.size());
  return out;
}

// Absolute-offset ReadFn over an in-memory vector (count 0 = seek probe).
unsigned long readVec(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count) {
  auto* bytes = static_cast<std::vector<uint8_t>*>(ctx);
  if (offset > bytes->size()) return 0;
  const unsigned long n = count > bytes->size() - offset ? static_cast<unsigned long>(bytes->size()) - offset : count;
  if (count > 0) std::memcpy(buffer, bytes->data() + offset, n);
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <font.ttf>\n", argv[0]);
    return 2;
  }
  const std::vector<uint8_t> ttf = readFile(argv[1]);
  const std::vector<uint8_t> ttc = makeTwoFaceTtc(ttf);
  void* ttcCtx = const_cast<std::vector<uint8_t>*>(&ttc);  // readVec treats it read-only

  // Scan mode (-1): first face with a Unicode cmap; collection metadata.
  FtFont::FaceInfo info;
  expect(FtFont::inspectMemory(ttc.data(), static_cast<uint32_t>(ttc.size()), info, nullptr, 0, -1) ==
             FtFont::InspectResult::Ok,
         "inspectMemory scans the collection");
  expect(info.numFaces == 2, "inspectMemory reports numFaces=2");
  expect(info.faceIndex == 0, "scan picks face 0");
  expect(info.weight >= 100 && info.weight <= 1000, "scan reads OS/2 weight");

  // Exact-index mode.
  FtFont::FaceInfo exact;
  expect(FtFont::inspectMemory(ttc.data(), static_cast<uint32_t>(ttc.size()), exact) == FtFont::InspectResult::Ok,
         "inspectMemory face 0 default");
  expect(exact.numFaces == 2 && exact.faceIndex == 0, "default inspect reports collection metadata");
  expect(FtFont::inspectMemory(ttc.data(), static_cast<uint32_t>(ttc.size()), exact, nullptr, 0, 1) ==
             FtFont::InspectResult::Ok,
         "inspectMemory exact face 1");
  expect(exact.faceIndex == 1, "exact inspect reports faceIndex=1");
  expect(FtFont::inspectMemory(ttc.data(), static_cast<uint32_t>(ttc.size()), exact, nullptr, 0, 2) ==
             FtFont::InspectResult::Unsupported,
         "out-of-range face rejected");

  // Streamed inspect: faceIndex -1 scans, exact 1 pins.
  FtFont::FaceInfo streamInfo;
  expect(FtFont::inspectStream(&readVec, ttcCtx, static_cast<unsigned long>(ttc.size()), streamInfo, nullptr, 0, -1) ==
             FtFont::InspectResult::Ok,
         "inspectStream scans the collection");
  expect(streamInfo.numFaces == 2 && streamInfo.faceIndex == 0, "streamed scan reports collection metadata");
  expect(FtFont::inspectStream(&readVec, ttcCtx, static_cast<unsigned long>(ttc.size()), streamInfo, nullptr, 0, 1) ==
             FtFont::InspectResult::Ok,
         "inspectStream exact face 1");
  expect(streamInfo.faceIndex == 1, "streamed exact inspect reports faceIndex=1");

  // init() at each face index; out-of-range init fails cleanly.
  FtFont face0;
  expect(face0.init(ttc.data(), static_cast<uint32_t>(ttc.size()), 16, 400, false, 0) && face0.ready(),
         "init face 0");
  expect(face0.ascent(16) != 0, "face 0 metrics valid");
  FtFont face1;
  expect(face1.init(ttc.data(), static_cast<uint32_t>(ttc.size()), 16, 400, false, 1) && face1.ready(),
         "init face 1");
  expect(face1.ascent(16) != 0, "face 1 metrics");
  FtFont bad;
  expect(!bad.init(ttc.data(), static_cast<uint32_t>(ttc.size()), 16, 400, false, 2), "init face 2 fails");
  expect(!bad.init(ttc.data(), static_cast<uint32_t>(ttc.size()), 16, 400, false, -1), "negative face index fails");

  // Streamed init on the collection.
  FtFont streamed;
  expect(streamed.initStream(&readVec, ttcCtx, static_cast<unsigned long>(ttc.size()), 16, 400, false, 1) &&
             streamed.ready(),
         "initStream face 1");
  expect(streamed.ascent(16) == face1.ascent(16), "streamed face 1 matches memory face 1 ascent");

  // Plain TTF unaffected: numFaces=1, init faceIndex 0 works, 1 fails.
  FtFont::FaceInfo plain;
  expect(FtFont::inspectMemory(ttf.data(), static_cast<uint32_t>(ttf.size()), plain) == FtFont::InspectResult::Ok,
         "plain ttf inspect");
  expect(plain.numFaces == 1 && plain.faceIndex == 0, "plain ttf reports one face");
  FtFont plainFace;
  expect(plainFace.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false, 0), "plain ttf init");
  expect(!plainFace.init(ttf.data(), static_cast<uint32_t>(ttf.size()), 16, 400, false, 1),
         "plain ttf face 1 rejected");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}