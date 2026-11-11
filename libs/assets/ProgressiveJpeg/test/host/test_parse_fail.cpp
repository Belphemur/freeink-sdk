// test_parse_fail <build-dir>: PIL-free parse-failure pins for
// ProgressiveJpeg (run.py needs Pillow + djpeg; this suite does not).
//
// Builds a minimal progressive JPEG in code (same marker layout the CrossPoint
// fixture generator emits: DQT q=8, SOF2, DHT, DC-only SOS, entropy filler,
// EOI) and drives begin()/decode() through ReadFn implementations that fail
// mid-stream. Pins:
//   1. a read-callback FAILURE inside a scan propagates Status::IoError
//      (parse used to fall through to Ok once a frame was seen);
//   2. a plain TRUNCATION (eof, no error) still decodes what arrived -> Ok.
//
// Build & run (host only):
//   c++ -std=c++17 -Wall -Wextra -Werror -I ../../include
//       ../../src/ProgressiveJpeg.cpp test_parse_fail.cpp -o test_parse_fail
//   ./test_parse_fail

#include <cstdio>
#include <cstring>
#include <vector>

#include "ProgressiveJpeg.h"

using freeink::ProgressiveJpeg;

namespace {

std::vector<uint8_t> makeProgressiveJpeg() {
  std::vector<uint8_t> out;
  auto marker = [&out](uint8_t m) { out.push_back(0xFF); out.push_back(m); };
  auto u16 = [&out](uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
  };
  marker(0xD8);  // SOI
  marker(0xDB);  // DQT, 8-bit table 0, q=8
  u16(3 + 64);
  out.push_back(0x00);
  for (int i = 0; i < 64; ++i) out.push_back(8);
  marker(0xC2);  // SOF2: progressive, 8-bit, 16x16, 1 component 1x1
  u16(8 + 3);
  out.push_back(8);
  u16(16);
  u16(16);
  out.push_back(1);
  out.push_back(1);
  out.push_back(0x11);
  out.push_back(0);
  marker(0xC4);  // DHT DC table 0: 12 symbols at length 5
  u16(2 + 1 + 16 + 12);
  out.push_back(0x00);
  std::vector<uint8_t> counts(16, 0);
  counts[4] = 12;
  out.insert(out.end(), counts.begin(), counts.end());
  for (int i = 0; i < 12; ++i) out.push_back(static_cast<uint8_t>(i));
  marker(0xDA);  // SOS: first scan, DC only
  u16(6 + 2);
  out.push_back(1);
  out.push_back(0x01);
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x00);
  // Entropy filler (all-zero bits = valid category-0 DC coefficients).
  // Larger than the reader's 512-byte buffer so the decode must REFILL
  // mid-walk — that is where the failing callback's error is reported.
  for (int i = 0; i < 600; ++i) out.push_back(0x00);
  marker(0xD9);  // EOI
  return out;
}

struct FailingRead {
  const uint8_t* data;
  uint32_t len;
  uint32_t failAt;  // reads report an ERROR once pos >= failAt
  uint32_t pos = 0;
  static int32_t read(void* ctx, uint8_t* dst, uint32_t n) {
    auto* self = static_cast<FailingRead*>(ctx);
    if (self->pos >= self->failAt) return -1;  // hard failure, not eof
    const uint32_t take = self->pos + n < self->len ? n : self->len - self->pos;
    std::memcpy(dst, self->data + self->pos, take);
    self->pos += take;
    return static_cast<int32_t>(take);
  }
};

bool writeRowAccumulate(void* user, uint16_t, const uint8_t*, uint16_t) {
  ++*static_cast<int*>(user);
  return true;
}

}  // namespace

int main() {
  const std::vector<uint8_t> jpeg = makeProgressiveJpeg();

  // The header (SOI..SOS) is 127 bytes and the first refill lands at 512:
  // failing at 200 lets begin() finish its marker walk and kills the
  // stream on the decode's second refill, inside the entropy walk.
  const uint32_t failAt = 200;
  if (jpeg.size() <= failAt) {
    std::printf("fixture too small\n");
    return 1;
  }

  // 1. Read failure inside a scan -> IoError, never Ok.
  {
    ProgressiveJpeg pj;
    FailingRead src{jpeg.data(), static_cast<uint32_t>(jpeg.size()), failAt};
    const ProgressiveJpeg::Status bs = pj.begin(&FailingRead::read, &src);
    if (bs != ProgressiveJpeg::Status::Ok) {
      std::printf("begin should succeed before the failure point (status %d)\n", static_cast<int>(bs));
      return 1;
    }
    src.pos = 0;  // decode re-reads the stream from the start
    int rows = 0;
    const ProgressiveJpeg::Status ds = pj.decode(&FailingRead::read, &src, &writeRowAccumulate, &rows);
    if (ds != ProgressiveJpeg::Status::IoError) {
      std::printf("decode must report IoError on a read failure, got %d\n", static_cast<int>(ds));
      return 1;
    }
  }

  // 2. Plain truncation (eof at the same offset, no error) stays Ok.
  {
    ProgressiveJpeg pj;
    // eof semantics: 0 past the cut, never negative.
    struct EofRead {
      const uint8_t* data;
      uint32_t len;
      uint32_t cut;
      uint32_t pos = 0;
      static int32_t read(void* ctx, uint8_t* dst, uint32_t n) {
        auto* self = static_cast<EofRead*>(ctx);
        if (self->pos >= self->cut) return 0;
        const uint32_t take = self->pos + n < self->cut ? n : self->cut - self->pos;
        std::memcpy(dst, self->data + self->pos, take);
        self->pos += take;
        return static_cast<int32_t>(take);
      }
    } eofSrc{jpeg.data(), static_cast<uint32_t>(jpeg.size()), failAt};
    const ProgressiveJpeg::Status bs = pj.begin(&EofRead::read, &eofSrc);
    if (bs != ProgressiveJpeg::Status::Ok) {
      std::printf("truncated begin should succeed (status %d)\n", static_cast<int>(bs));
      return 1;
    }
    eofSrc.pos = 0;
    int rows = 0;
    const ProgressiveJpeg::Status ds = pj.decode(&EofRead::read, &eofSrc, &writeRowAccumulate, &rows);
    if (ds != ProgressiveJpeg::Status::Ok) {
      std::printf("truncated decode must stay Ok (leniency), got %d\n", static_cast<int>(ds));
      return 1;
    }
  }

  std::printf("ProgressiveJpeg parse-failure pins: OK\n");
  return 0;
}
