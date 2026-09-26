// Host tests for the BLE HID host (src/BleKeyboardHost.cpp) against the fake
// NimBLE stack in this directory. No radio: these check the host's own state
// machine, report decode and teardown order, not on-air behaviour.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fake_ble.h"

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

using fakeble::host;

constexpr const char* kRemote = "AA:BB:CC:DD:EE:01";

// HID 1.11 Appendix B.1 boot keyboard: modifiers, reserved byte, six key bytes.
constexpr uint8_t kKeyboardMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,              // Generic Desktop, Keyboard, Collection
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,              //   Keyboard page, modifier usages
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,  //   8 x 1 bit
    0x81, 0x02,                                      //   Input: modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,              //   Input: reserved byte
    0x19, 0x00, 0x29, 0x65, 0x25, 0x65,              //   key usages 0..0x65
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,              //   Input: six key bytes
    0xC0,                                            // End Collection
};

// The same keyboard (report 1) plus a Consumer Control report (report 2), as
// many page turners and keyboards with media keys describe themselves.
constexpr uint8_t kKeyboardConsumerMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,  // Keyboard collection, Report ID 1
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,              //   Keyboard page, modifier usages
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,  //   8 x 1 bit
    0x81, 0x02,                                      //   Input: modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,              //   Input: reserved byte
    0x19, 0x00, 0x29, 0x65, 0x25, 0x65,              //   key usages 0..0x65
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,              //   Input: six key bytes
    0xC0,                                            // End Collection
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,  // Consumer Control collection, Report ID 2
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x26, 0xFF, 0x03,  //   usages and logical range 0..0x3FF
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,              //   Input: one 16-bit usage
    0xC0,                                            // End Collection
};

int g_inputReport = -1;

void serveRemote(const uint8_t* map, size_t len) {
  const int mapChar = fakeble::addCharacteristic(0x2A4B, true, false, false);
  fakeble::setValue(mapChar, map, len);
  g_inputReport = fakeble::addInputReport();
}

void sendKeys(uint8_t usage) {
  const uint8_t report[8] = {0, 0, usage, 0, 0, 0, 0, 0};
  fakeble::notify(g_inputReport, report, sizeof report);
}

void testKeyboardPressIsOneKeyEvent() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  sendKeys(0x4B);  // Page Up down
  freeink::KeyEvent ev;
  CHECK(host().popKey(ev));
  CHECK(ev.keycode == 0x4B);
  CHECK(ev.special == freeink::SpecialKey::PageUp);
  sendKeys(0);  // released
  CHECK(!host().popKey(ev));
}

}  // namespace

int main() {
  testKeyboardPressIsOneKeyEvent();
  fakeble::resetWorld();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  std::fflush(stdout);
  // Skip static destructors: a connection task the host deleted inside a NimBLE
  // wait is parked on purpose and must not see its fakes torn down.
  std::_Exit(checksFailed == 0 ? 0 : 1);
}
