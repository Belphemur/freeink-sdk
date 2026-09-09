#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>

#include "driver/Uc8279Driver.h"
#include "lut/Uc8279X3Luts.h"

// Only the driver's nothrow array allocation is intercepted. Test containers
// use the ordinary scalar allocator and are excluded from the scratch count.
static void* scratch;
static bool failAllocation;
static size_t allocations;
static constexpr size_t PANEL_BYTES = 792 / 8 * 528;
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  assert(size == PANEL_BYTES);
  assert(!scratch);
  ++allocations;
  if (failAllocation) return nullptr;
  scratch = std::malloc(size);
  return scratch;
}
void operator delete[](void* ptr) noexcept {
  if (ptr == scratch) scratch = nullptr;
  std::free(ptr);
}

int main() {
  using namespace freeink;
  EpdBus bus;
  std::vector<uint8_t> bw(PANEL_BYTES, 0x88);
  std::vector<uint8_t> lsb(PANEL_BYTES, 0x22);
  std::vector<uint8_t> msb(PANEL_BYTES, 0x66);
  Uc8279Driver driver;
  driver.begin(bus);
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(!scratch && allocations == 0);

  // All four pixel levels: base|LSB = 0xAA, then XOR MSB = 0xCC.
  // Repeat to cover the post-grayscale rebase as well as the initial full path.
  for (int repeat = 0; repeat < 3; ++repeat) {
    driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
    assert(scratch);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    assert(!scratch);
    assert(bus.oldPlane == std::vector<uint8_t>(PANEL_BYTES, 0xAA));
    assert(bus.newPlane == std::vector<uint8_t>(PANEL_BYTES, 0xCC));
    driver.displayGray(bus, bw.data(), false, nullptr, false);
    assert(bus.lastBank == std::vector<uint8_t>(kUc8279X3_Xth4[0], kUc8279X3_Xth4[0] + 49));
    assert(!scratch);
  }

  // Low coverage remains delta AA, with its original waveform and raw planes.
  std::fill(msb.begin(), msb.end(), 0);
  driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  assert(!scratch && bus.oldPlane == lsb && bus.newPlane == msb);
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  assert(bus.lastBank == std::vector<uint8_t>(kUc8279X3_XtfAa[0], kUc8279X3_XtfAa[0] + 49));
  assert(bus.rawRegisters == std::vector<uint8_t>({0x20, 0x23, 0x22, 0x21, 0x24}));

  // Every terminal/abandoned path must relinquish the snapshot, including a
  // base replaced by another base, an empty cleanup, and invalid plane input.
  for (int path = 0; path < 10; ++path) {
    driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
    assert(scratch);
    switch (path) {
      case 0: driver.cleanupGrayscaleBuffers(bus, nullptr); break;
      case 1: driver.grayscaleRevert(bus, bw.data()); break;
      case 2: driver.deepSleep(bus); break;
      case 3: driver.begin(bus); break;
      case 4: driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false); break;
      case 5: driver.copyGrayscaleLsb(bus, nullptr); break;
      case 6: driver.copyGrayscaleMsb(bus, nullptr); break;
      case 7: driver.writeGrayscalePlaneStrip(bus, GrayPlane::Lsb, lsb.data(), 0, 1); break;
      case 8: driver.displayGray(bus, bw.data(), false, nullptr, false); break;
      case 9:
        driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
        driver.cleanupGrayscaleBuffers(bus, bw.data());
        break;
    }
    assert(!scratch);
  }

  // OOM falls back to the existing delta-AA path, then a later pass can retry.
  failAllocation = true;
  driver.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
  assert(!scratch);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.copyGrayscaleMsb(bus, msb.data());
  assert(bus.oldPlane == lsb && bus.newPlane == msb);
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  assert(bus.lastBank == std::vector<uint8_t>(kUc8279X3_XtfAa[0], kUc8279X3_XtfAa[0] + 49));
  assert(bus.rawRegisters == std::vector<uint8_t>({0x20, 0x23, 0x22, 0x21, 0x24}));
  failAllocation = false;
  {
    Uc8279Driver other;
    other.begin(bus);
    other.displayGrayscaleBase(bus, bw.data(), RefreshMode::Fast, false);
    assert(scratch);
  }
  assert(!scratch);
  std::cout << "PASS: UC8279 scratch lifetime, four-tone planes, AA, cancellation and OOM recovery\n";
}
