#pragma once

// Onyx Picco SE0400NQW47 panel — the controller the stock firmware calls
// "SSD2677" (identified by CMD 0x70 -> 07 01 01). Its command set is
// UltraChip-style, not SSD1677: panel setting 0x00, window 0x83, data 0x10, one
// 535-byte LUT via 0x20, power on/off 0x04/0x02, refresh 0x12, deep sleep 0x07.
// BUSY is active-LOW.
//
// Each frame goes out as 2 bits per pixel through a single 0x10 burst: the
// previous frame in the high bit, the new frame in the low bit; the LUT picks
// the transition. Init list, LUTs and bit packing are from the stock firmware
// (build 2912). Grayscale is the stock overlay AA mode (slot-5 LUT): the LSB
// and MSB masks are packed as the low/high bit of each pixel. Stock's image mode
// (slot 4) uses an unrecovered encoding, so absolute grayscale is not advertised.
//
// Selected by FreeInkDisplay when piccoProbePanel() reports PiccoSe0400nqw47.

#include "PanelDriver.h"

namespace freeink {

class Ssd2677Driver : public PanelDriver {
 public:
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveLow; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, false, false, false};
  }
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;

 private:
  void sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo);
  void refreshWith(EpdBus& bus, const unsigned char* lut, bool turnOff);

  bool _needsPowerOn = true;
  bool _needsInitialFull = true;
  uint8_t* _lsb = nullptr;  // retained LSB mask: the renderer reuses its buffer for the MSB pass
};

PanelDriver& ssd2677Driver();

}  // namespace freeink
