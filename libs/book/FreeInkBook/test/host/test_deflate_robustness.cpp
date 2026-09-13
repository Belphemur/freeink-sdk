// Degenerate/corrupt deflate-stream regression tests for ZipEntryReader.
//
// The vendored inflate core is the v1.15 lineage the ESP32 mask ROM was built
// from (see include/epub/MinizConfig.h); its pre-2.1 defect class on
// attacker-controlled streams is mitigated, not removed:
//   - TINFL_GET_BYTE zero-pads exhausted input (miniz_cores.c:109-123), so a
//     stream whose trailing zero bits keep decoding must be bounded by the
//     reader, not by the core.
//   - Oversized distances are masked into the 32 KB window
//     (miniz_cores.c:359,363: "& out_buf_size_mask") — in-bounds garbage, no
//     OOB, on the engine's wrapping-window configuration.
// These tests feed hand-crafted hostile ZIP entries through
// ZipEntryReader::read() and assert bounded behavior: the loop terminates,
// produced bytes stay within the central-directory uncompressedSize, and the
// outcome is a clean error or clean end — never a hang or a crash.

#include <epub/ZipCatalog.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace freeink::book;

int checksRun = 0;
int checksFailed = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    ++checksRun;                                                  \
    if (!(cond)) {                                                \
      ++checksFailed;                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
    }                                                             \
  } while (0)

class HostFileSource : public BookSource {
 public:
  ~HostFileSource() override {
    if (file_ != nullptr) std::fclose(file_);
  }
  bool open(const char* path) {
    if (file_ != nullptr) std::fclose(file_);
    file_ = std::fopen(path, "rb");
    if (file_ == nullptr) return false;
    std::fseek(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(std::ftell(file_));
    return true;
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return static_cast<int32_t>(std::fread(dst, 1, len, file_));
  }
  uint64_t size() const override { return size_; }

 private:
  FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

const char* fixturesDir = nullptr;

// --- deflate bit-stream builder (LSB-first bits; Huffman codes MSB-first) ---
struct BitWriter {
  std::vector<uint8_t> bytes;
  uint32_t acc = 0;
  int nbits = 0;

  void put(uint32_t value, int count) {
    acc |= value << nbits;
    nbits += count;
    while (nbits >= 8) {
      bytes.push_back(static_cast<uint8_t>(acc & 0xFF));
      acc >>= 8;
      nbits -= 8;
    }
  }
  void huff(uint32_t code, int len) {
    for (int i = len - 1; i >= 0; --i) put((code >> i) & 1, 1);
  }
  void flush() {
    if (nbits > 0) bytes.push_back(static_cast<uint8_t>(acc & 0xFF));
    acc = 0;
    nbits = 0;
  }
};

// --- single-entry ZIP container with a method-8 entry -----------------------
bool writeDeflateZip(const char* path, const std::vector<uint8_t>& deflateData, uint32_t compSize,
                     uint32_t uncompSize) {
  std::vector<uint8_t> z;
  const char* name = "target.bin";
  const uint16_t nameLen = static_cast<uint16_t>(std::strlen(name));
  const auto put16 = [&z](uint16_t v) {
    z.push_back(v & 0xFF);
    z.push_back(v >> 8);
  };
  const auto put32 = [&z](uint32_t v) {
    for (int i = 0; i < 4; ++i) z.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  };

  put32(0x04034b50);  // local file header
  put16(20);          // version needed
  put16(0);           // flags
  put16(8);           // method: deflate
  put16(0);           // mod time
  put16(0);           // mod date
  put32(0);           // crc32 (not validated on the read path)
  put32(compSize);
  put32(uncompSize);
  put16(nameLen);
  put16(0);  // extra len
  z.insert(z.end(), name, name + nameLen);
  z.insert(z.end(), deflateData.begin(), deflateData.begin() + (compSize < deflateData.size() ? compSize : deflateData.size()));

  const uint32_t cdOffset = static_cast<uint32_t>(z.size());
  put32(0x02014b50);  // central directory header
  put16(20);          // version made by
  put16(20);          // version needed
  put16(0);           // flags
  put16(8);           // method
  put16(0);           // mod time
  put16(0);           // mod date
  put32(0);           // crc32
  put32(compSize);
  put32(uncompSize);
  put16(nameLen);
  put16(0);  // extra len
  put16(0);  // comment len
  put16(0);  // disk start
  put16(0);  // internal attrs
  put32(0);  // external attrs
  put32(0);  // local header offset
  z.insert(z.end(), name, name + nameLen);

  const uint32_t cdSize = static_cast<uint32_t>(z.size()) - cdOffset;
  put32(0x06054b50);  // end of central directory
  put16(0);           // disk
  put16(0);           // disk with cd
  put16(1);           // entries this disk
  put16(1);           // total entries
  put32(cdSize);
  put32(cdOffset);
  put16(0);  // comment len

  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) return false;
  const size_t wrote = std::fwrite(z.data(), 1, z.size(), f);
  std::fclose(f);
  return wrote == z.size();
}

// Reads the whole entry through ZipEntryReader::read(); returns false if the
// reader produced more than `uncompSize` bytes or needed more than `maxCalls`
// read() calls (either would mean an unbounded / hung stream).
struct ReadResult {
  bool bounded = true;
  bool errored = false;
  uint32_t produced = 0;
};
ReadResult readBounded(ZipCatalog& catalog, Arena& scratch, uint32_t uncompSize, int maxCalls) {
  ReadResult r;
  const ZipEntry* entry = catalog.find("target.bin");
  if (entry == nullptr) {
    r.bounded = false;
    return r;
  }
  ZipEntryReader reader;
  if (static_cast<int>(reader.open(*catalog.source(), *entry, scratch)) !=
      static_cast<int>(BookStatus::Ok)) {
    r.bounded = false;
    return r;
  }
  uint8_t chunk[512];
  for (int calls = 0; calls < maxCalls; ++calls) {
    const int32_t n = reader.read(chunk, sizeof(chunk));
    if (n < 0) {
      r.errored = true;
      break;
    }
    if (n == 0) break;
    r.produced += static_cast<uint32_t>(n);
    if (r.produced > uncompSize) {
      r.bounded = false;
      break;
    }
  }
  // Loop exhausted without a terminal 0/negative: unbounded.
  if (!r.errored && r.produced < uncompSize && reader.read(chunk, sizeof(chunk)) > 0) {
    r.bounded = false;
  }
  if (reader.totalProduced() > uncompSize) r.bounded = false;
  return r;
}

// --- hostile streams ---------------------------------------------------------

// Stored deflate block cut off mid-payload: the decoder must end with a clean
// error (needs input that the container does not declare), not loop.
std::vector<uint8_t> truncatedStoredBlock() {
  BitWriter w;
  w.put(1, 1);  // BFINAL
  w.put(0, 2);  // BTYPE 00: stored
  w.flush();    // byte-align
  const uint16_t len = 64;
  w.bytes.push_back(len & 0xFF);
  w.bytes.push_back(len >> 8);
  w.bytes.push_back(~len & 0xFF);
  w.bytes.push_back((~len >> 8) & 0xFF);
  for (int i = 0; i < len; ++i) w.bytes.push_back(0x41);
  w.bytes.resize(1 + 4 + 20);  // keep only 20 of the 64 payload bytes
  return w.bytes;
}

// Empty deflate payload on a method-8 entry.
std::vector<uint8_t> emptyDeflate() {
  return {};
}

// Fixed-Huffman block with a length/distance pair whose distance (32768)
// exceeds everything decoded so far. The 32 KB wrapping window must keep the
// copy in bounds (masked index), producing garbage — never an OOB access.
std::vector<uint8_t> oversizedDistance() {
  BitWriter w;
  w.put(1, 1);  // BFINAL
  w.put(1, 2);  // BTYPE 01: fixed Huffman
  w.huff(0x30 + 65, 8);  // literal 'A' (1 byte produced)
  w.huff(1, 7);          // length symbol 257 -> length 3, no extra bits
  w.huff(29, 5);         // distance symbol 29
  w.put(0x1FFF, 13);     // distance extra bits -> dist = 24577 + 8191 = 32768
  w.huff(0x30 + 65, 8);  // literal 'A'
  w.huff(0, 7);          // end-of-block (symbol 256)
  w.flush();
  return w.bytes;
}

// Dynamic-Huffman block whose single-symbol literal tree turns the v1.15
// zero-padding escape (TINFL_GET_BYTE pads 0's once input is exhausted) into
// an unbounded stream of literals: literal 65 has the all-zeros code "0" and
// the stream never emits an end-of-block symbol, so zero bits decode as 'A'
// forever. The reader's uncompressedSize cap must bound the output.
std::vector<uint8_t> zeroPaddingEscape() {
  BitWriter w;
  // Block header: BFINAL, BTYPE 10 (dynamic).
  w.put(1, 1);
  w.put(2, 2);
  w.put(0, 5);   // HLIT-257 = 0  (literals 0..256)
  w.put(0, 5);   // HDIST-1  = 0  (distance symbol 0)
  w.put(14, 4);  // HCLEN-4  = 14 (18 code-length symbols)

  // Code-length code lengths, in the s_length_dezigzag order
  // (16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15).
  // Tree: sym1 -> "0" (1 bit), sym0 -> "10" (2), sym17 -> "110", sym18 -> "111".
  static const int clLens[] = {0, 3, 3, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
  for (int i = 0; i < 18; ++i) w.put(static_cast<uint32_t>(clLens[i]), 3);  // HCLEN = 18

  // Literal code lengths (257 symbols): 65 zeros, sym65 = 1, 190 zeros,
  // sym256 = 1. Zeros are run-coded with symbol 18 (11..138) + 7 extra bits.
  w.huff(7, 3);
  w.put(65 - 11, 7);
  w.huff(0, 1);  // CL sym 1: length 1
  w.huff(7, 3);
  w.put(138 - 11, 7);
  w.huff(7, 3);
  w.put(52 - 11, 7);
  w.huff(0, 1);  // sym256: length 1
  // Distance code lengths (1 symbol): sym0 = 1.
  w.huff(0, 1);

  // LZ data: literal 'A' (code "0") repeated; the stream then just ends, so
  // the zero-padding path keeps decoding 'A' forever.
  for (int i = 0; i < 400; ++i) w.put(0, 1);
  w.flush();
  return w.bytes;
}

// Pathological code-length table: all 19 code-length code sizes are zero, so
// the code-length tree has no codes at all. v1.15 must not crash or hang: the
// empty tree decodes zero-length runs (lengths all zero) and the stream ends
// without a usable literal tree.
std::vector<uint8_t> emptyCodeLengthTable() {
  BitWriter w;
  w.put(1, 1);   // BFINAL
  w.put(2, 2);   // BTYPE 10: dynamic
  w.put(0, 5);   // HLIT-257
  w.put(0, 5);   // HDIST-1
  w.put(0, 4);   // HCLEN-4 (4 code-length symbols)
  for (int i = 0; i < 4; ++i) w.put(0, 3);  // sizes for syms 16,17,18,0
  w.flush();
  return w.bytes;
}

// Oversubscribed Huffman table (three symbols with 1-bit codes): v1.15
// validates Kraft equality (miniz_cores.c:258) and must fail cleanly.
std::vector<uint8_t> oversubscribedTable() {
  BitWriter w;
  w.put(1, 1);   // BFINAL
  w.put(2, 2);   // BTYPE 10: dynamic
  w.put(0, 5);   // HLIT-257
  w.put(0, 5);   // HDIST-1
  w.put(0, 4);   // HCLEN-4 (4 code-length symbols)
  // Code-length tree with 3 symbols of length 1: lengths for 16,17,18 = 1.
  w.put(1, 3);   // sym16
  w.put(1, 3);   // sym17
  w.put(1, 3);   // sym18
  w.put(0, 3);   // sym0
  w.flush();
  return w.bytes;
}

struct Case {
  const char* name;
  std::vector<uint8_t> (*stream)();
  uint32_t uncompSize;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <fixtures-build-dir>\n", argv[0]);
    return 2;
  }
  fixturesDir = argv[1];

  const Case cases[] = {
      {"truncated-stored", truncatedStoredBlock, 64},
      {"empty-deflate", emptyDeflate, 10},
      {"oversized-distance", oversizedDistance, 8},
      {"zero-padding-escape", zeroPaddingEscape, 8},
      {"empty-cl-table", emptyCodeLengthTable, 8},
      {"oversubscribed-table", oversubscribedTable, 8},
  };

  static uint8_t scratchBuf[256 * 1024];
  for (const Case& c : cases) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/deflate_%s.zip", fixturesDir, c.name);
    const std::vector<uint8_t> stream = c.stream();
    if (!writeDeflateZip(path, stream, static_cast<uint32_t>(stream.size()), c.uncompSize)) {
      ++checksRun;
      ++checksFailed;
      std::printf("FAIL cannot write fixture %s\n", path);
      continue;
    }

    HostFileSource source;
    ++checksRun;
    if (!source.open(path)) {
      ++checksFailed;
      std::printf("FAIL %s: cannot open fixture\n", c.name);
      continue;
    }

    Arena scratch(scratchBuf, sizeof(scratchBuf));
    ZipCatalog catalog;
    const BookStatus st = catalog.open(source, scratch);
    if (st != BookStatus::Ok) {
      ++checksRun;
      ++checksFailed;
      std::printf("FAIL %s: catalog open -> %s\n", c.name, bookStatusName(st));
      continue;
    }

    // 4096 read() calls is a generous ceiling for any bounded decode of a
    // handful of bytes; a hostile stream must never need more.
    const ReadResult r = readBounded(catalog, scratch, c.uncompSize, 4096);
    ++checksRun;
    if (!r.bounded) {
      ++checksFailed;
      std::printf("FAIL %s: unbounded read loop or produced > declared\n", c.name);
      continue;
    }
    ++checksRun;
    if (r.produced > c.uncompSize) {
      ++checksFailed;
      std::printf("FAIL %s: produced %u > declared %u\n", c.name, r.produced, c.uncompSize);
      continue;
    }
    std::printf("  %-22s produced %u/%u B, clean %s\n", c.name, r.produced, c.uncompSize,
                r.errored ? "error" : "end");
  }

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
