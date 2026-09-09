#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>
#include <type_traits>
// Inspect private state without adding test-only methods to the SDK API.
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Uc8179Driver.h"
#include "src/driver/Uc8279X4Driver.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static Bytes frame(unsigned seed) {
  Bytes b(48000);
  for (size_t i=0; i<b.size(); ++i) b[i]=uint8_t((i*37 + i/100*11 + seed) ^ (i>>8));
  return b;
}

template<class Driver>
static void testStream(bool reverse, unsigned gateOffset) {
  Driver d;
  EpdBus bus;
  const auto a=frame(3), b=frame(89);
  for (bool invert : {false,true}) {
    bus.clear();
    d.streamPlane(bus,0x13,a.data(),invert);
    assert(bus.writes.size()==1);
    const auto& w=bus.writes.front();
    assert(w.transactions==1 && w.bytes.size()==60000);
    for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
      uint8_t expected=0xff;
      if (y>=gateOffset && y<gateOffset+480) {
        unsigned row=y-gateOffset;
        if (reverse) row=479-row;
        expected=a[row*100+x];
        if (invert) expected=uint8_t(~expected);
      }
      assert(w.bytes[y*100+x]==expected);
    }
  }
  bus.clear();
  d.streamPlaneXor(bus,0x13,a.data(),b.data());
  assert(bus.writes.size()==1 && bus.writes[0].transactions==1);
  assert(bus.writes[0].bytes.size()==60000);
  for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
    uint8_t expected=0xff;
    if (y>=gateOffset && y<gateOffset+480) {
      unsigned row=y-gateOffset;
      if (reverse) row=479-row;
      expected=a[row*100+x]^b[row*100+x];
    }
    assert(bus.writes[0].bytes[y*100+x]==expected);
  }
  d._grayRefreshedOnce=true;
  d._oldPlaneValid=true;
  d._needFullClear=false;
  bus.clear();
  d.displayGrayscaleBase(bus,a.data(),RefreshMode::Full,false);
  assert(lastRegister(bus,0xe5)==0x1e); // honor Full even after an AA page
  for (const auto& w : bus.writes)
    if (w.command==0x10 || w.command==0x13) assert(w.transactions==1);
}

static void testSsd() {
  const auto b=frame(33);
  for (auto mode : {RefreshMode::Full,RefreshMode::Half,RefreshMode::Fast}) {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    bus.clear();
    d.display(bus,b.data(),nullptr,mode,false);
    assert(lastRegister(bus,0x22)==(mode==RefreshMode::Full ? 0xf7 : 0xd7));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,false);
    assert(lastRegister(bus,0x22)==0xfc);
  }
  EpdBus bus;
  Ssd1677Driver d;
  d.begin(bus);
  bus.clear();
  d.displayGray(bus,b.data(),false,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcc && d._isScreenOn);
  d.deepSleep(bus);
  assert(lastRegister(bus,0x22)==3 && !d._isScreenOn);
  bus.clear();
  d.displayGray(bus,b.data(),true,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcf && !d._isScreenOn);
}

template<class Driver>
static void testAsyncFrame() {
  Driver driver;
  FreeInkDisplay display(12,11,13,18,14,6);
  display._driver=&driver;
  display.begin();
  const auto submitted=frame(27), redrawn=frame(94);
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.completeDisplay();
  // Finish must sync the frame actually submitted, even after the caller draws.
  const auto& old=display._bus.writes.back();
  assert(old.command==0x10 && old.bytes.size()==60000);
  for (unsigned y=0; y<480; ++y) {
    unsigned dst;
    if constexpr (std::is_same<Driver,Uc8179Driver>::value) dst=479-y;
    else dst=120+y;
    assert(std::equal(submitted.begin()+y*100,submitted.begin()+(y+1)*100,old.bytes.begin()+dst*100));
  }
  assert(!display.isRefreshPending());
  // Shadow-free entry points require the live frame to survive until finish.
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH,false);
  display.completeDisplay();
  const auto& next=display._bus.writes.back();
  unsigned firstRow=std::is_same<Driver,Uc8179Driver>::value ? 479 : 0;
  unsigned offset=std::is_same<Driver,Uc8179Driver>::value ? 0 : 12000;
  assert(std::equal(redrawn.begin()+firstRow*100,redrawn.begin()+(firstRow+1)*100,next.bytes.begin()+offset));
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayAsyncImpl(FreeInkDisplay::FAST_REFRESH,false,true);
  display.completeDisplay();
  const auto& noShadow=display._bus.writes.back();
  assert(std::equal(submitted.begin()+firstRow*100,submitted.begin()+(firstRow+1)*100,
                    noShadow.bytes.begin()+offset));
#endif
  display.releaseBuffers();
  free(driver._grayBase);
}

// A driver with no grayscale implementation must never advertise support.
class BwOnlyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 10000000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override { return {800, 480, 100, 48000}; }
  void begin(EpdBus&) override {}
  void deepSleep(EpdBus&) override {}
  void display(EpdBus&, const uint8_t*, const uint8_t*, RefreshMode, bool) override {}
};

class CombinedGrayDriver : public BwOnlyDriver {
 public:
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, true};
  }
};

static void testCapabilities() {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  assert(!display.grayscaleCapabilities().supported());
  BwOnlyDriver bw;
  display._driver = &bw;
  assert(!display.grayscaleCapabilities().supported());
  assert(!display.supportsStripGrayscale());

  Ssd1677Driver ssd;
  display._driver = &ssd;
  auto caps = display.grayscaleCapabilities();
  assert(caps.supported() && caps.encoding == GrayscaleEncoding::OverlayMasks);
  assert(caps.stripUploads && caps.asyncBase && !caps.stagingWhileBusy);
  assert(caps.base == GrayscaleBase::Separate);
  assert(display.supportsStripGrayscale() == caps.stripUploads);
  assert(display.supportsAsyncGrayscaleBase() == caps.asyncBase);
  assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!display.grayscaleCapabilities(static_cast<GrayscaleMode>(255)).supported());
  display._inversionDirty = true;
  assert(!display.grayscaleCapabilities().asyncBase);
  assert(display.grayscaleCapabilities().stripUploads);
  assert(!display.supportsAsyncGrayscaleBase());
  display._inversionDirty = false;

  CombinedGrayDriver combined;
  display._driver = &combined;
  assert(display.combinesGrayscaleBase());
  assert(display.supportsBusyGrayscaleStaging());
  assert(!display.supportsAsyncGrayscaleBase());
  display._inverted = true;
  caps = display.grayscaleCapabilities();
  assert(!caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
  assert(!display.combinesGrayscaleBase() && !display.supportsBusyGrayscaleStaging());
  assert(!display.supportsStripGrayscale());
  display._inverted = false;

  Uc8179Driver uc8179;
  Uc8279X4Driver uc8279;
  for (PanelDriver* driver : {static_cast<PanelDriver*>(&uc8179), static_cast<PanelDriver*>(&uc8279)}) {
    display._driver = driver;
    caps = display.grayscaleCapabilities();
    assert(caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
    assert(!display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  }
  // Queries must neither start a refresh nor write the panel bus.
  assert(display._bus.writes.empty() && !display.isRefreshPending());
}

static void testAbsolutePipeline() {
  Ssd1677Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto absolute = GrayscaleMode::Absolute;
  for (bool strips : {false, true}) {
    assert(display.displayGrayscaleBase(absolute));
    display._bus.clear();
    if (strips) {
      for (unsigned y = 0; y < 480; y += 80) {
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data() + y * 100, y, 80);
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data() + y * 100, y, 80);
      }
    } else display.copyGrayscaleBuffers(lsb.data(), msb.data());
    Bytes plane0, plane1;
    for (const auto& w : display._bus.writes) {
      if (w.command == 0x24) plane0.insert(plane0.end(), w.bytes.begin(), w.bytes.end());
      if (w.command == 0x26) plane1.insert(plane1.end(), w.bytes.begin(), w.bytes.end());
    }
    assert(plane0.size() == lsb.size() && plane1.size() == msb.size());
    for (size_t i = 0; i < lsb.size(); ++i) {
      assert(plane0[i] == uint8_t(~lsb[i]) && plane1[i] == uint8_t(~msb[i]));
    }
    display.displayGrayBuffer(false);
    assert(lastRegister(display._bus, 0x22) == 0xC7);
    assert(!driver._isScreenOn && driver._needsGrayClear);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needsGrayClear);
    display._bus.clear();
    display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
    display.waitRefreshComplete();
    assert(!driver._needsGrayClear);
  }
  for (unsigned failure = 0; failure < 6; ++failure) {
    assert(display.displayGrayscaleBase(absolute));
    display._bus.clear();
    if (failure == 0) display.copyGrayscaleLsbBuffers(lsb.data()); // missing MSB
    if (failure == 1) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 80, 80);
    if (failure == 2) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 481);
    if (failure == 3) display.copyGrayscaleLsbBuffers(nullptr);
    if (failure == 4) {
      display.copyGrayscaleBuffers(lsb.data(), msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data()); // duplicate plane
    }
    if (failure == 5) {
      display.copyGrayscaleMsbBuffers(msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data());
    }
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.cleanupGrayscaleBuffers(nullptr);
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, true);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
  }
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.setInverted(true);
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  assert(!display.displayGrayscaleBase(absolute));
  display.setInverted(false);
  assert(display.displayGrayscaleBase(absolute));
  display.deepSleep();
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  display.releaseBuffers();

  Ssd1677Config unsupported = ssd1677DefaultConfig();
  unsupported.absoluteGrayscale = false;
  Ssd1677Driver other(unsupported);
  FreeInkDisplay noAbsolute(1, 2, 3, 4, 5, 6);
  noAbsolute._driver = &other;
  assert(!noAbsolute.displayGrayscaleBase(absolute));
  assert(noAbsolute._bus.writes.empty());
}

static void testStickyAbsolute() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  assert(driver._cfg.grayPowerUpFirst);
  assert(driver._cfg.fullSeqOverride == 0xF7);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer(false);
  assert(lastRegister(display._bus, 0x22) == 0xC7);
  assert(!driver._isScreenOn);
  display.cleanupGrayscaleBuffers(bw.data());
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(lastRegister(display._bus, 0x22) == 0xF7);
  display.releaseBuffers();
}

int main(int argc, char**) {
  if (argc > 1) {
    testStickyAbsolute();
    std::puts("Sticky absolute capability, activation, power-down and B/W recovery passed");
    return 0;
  }
  testAbsolutePipeline();
  testCapabilities();
  testStream<Uc8179Driver>(true,0);
  testStream<Uc8279X4Driver>(false,120);
  testSsd();
  testAsyncFrame<Uc8179Driver>();
  testAsyncFrame<Uc8279X4Driver>();
  std::puts("Pro plane bytes, transaction counts, clean refreshes, power state and async frame ownership passed");
}
