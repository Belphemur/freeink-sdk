#pragma once

// UC8279C panel driver for EEGO A4. The panel controller is 768x600; the host
// framebuffer is 768x552 and every transfer appends 48 white padding rows.
// Transfers scan framebuffer rows bottom-to-top and BUSY is active-low.

#include "PanelDriver.h"

namespace freeink {

class Uc8279cA4Driver : public PanelDriver {
 public:
  Uc8279cA4Driver();

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  // The stock gray waveform drives absolute levels from any prior state, so
  // image planes need no B/W base. Overlay masks are folded onto a snapshot of
  // their B/W base into the same absolute planes.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode == GrayscaleMode::Overlay) return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, false, false, false};
    return {GrayscaleEncoding::AbsolutePlanes, GrayscaleBase::Combined, false, false, false};
  }
  void beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback, bool turnOff) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

 private:
  void hardwareReset(EpdBus& bus);
  void initController(EpdBus& bus, bool grayMode = false);
  void ensurePowerOn(EpdBus& bus);
  void loadFullLut(EpdBus& bus);
  void loadFastLut(EpdBus& bus);
  void loadGrayLut(EpdBus& bus);
  void writeFrame(EpdBus& bus, uint8_t command, const uint8_t* fb, bool invert = false);
  void fillControllerRam(EpdBus& bus, uint8_t command, uint8_t fill);
  void refresh(EpdBus& bus, bool turnOff, const uint8_t* bwBaseline = nullptr);
  uint8_t* allocateGrayBuffer();
  void foldOverlayMasks();

  uint16_t _width;
  uint16_t _height;
  uint16_t _widthBytes;
  bool _screenOn = false;
  bool _grayControllerMode = false;
  bool _redriveAfterGray = false;  // re-drive all target pixels after AA, using the requested waveform
  // begin() seeds DTM1 white, but after a reflash/reset the glass holds the
  // previous UI, so a white-assumed diff leaves it ghosting through the first
  // paint. The first frame is driven to its inverse, then to the target with
  // DTM1 = inverse, so every pixel swings both ways under the full LUT.
  bool _firstRefreshPending = false;
  uint8_t* _grayLsb = nullptr;
  uint8_t* _grayMsb = nullptr;
  uint8_t* _grayBase = nullptr;  // B/W base of an overlay pass (bit 1 = white)
  bool _overlayPass = false;
};

PanelDriver& uc8279cA4Driver();

}  // namespace freeink
