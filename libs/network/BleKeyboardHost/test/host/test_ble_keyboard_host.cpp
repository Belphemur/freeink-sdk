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

int drainKeys(uint8_t* lastKeycode = nullptr) {
  int count = 0;
  freeink::KeyEvent ev;
  while (host().popKey(ev)) {
    ++count;
    if (lastKeycode != nullptr) *lastKeycode = ev.keycode;
  }
  return count;
}

bool waitConnected(uint32_t timeoutMs = 2000) {
  for (uint32_t i = 0; i < timeoutMs; ++i) {
    if (host().isConnected()) return fakeble::waitForWorkerIdle();
    vTaskDelay(0);
  }
  return false;
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

// A peer that is pairing or being discovered sits in a NimBLE wait that only a
// disconnect ends (cancelConnect() covers the GAP connect alone). end() must not
// delete the connection task inside that wait: NimBLE later completes the wait
// on a task handle that no longer exists.
void testEndDuringPairingDoesNotDeleteTaskInsideNimble(fakeble::Stage stage) {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  fakeble::holdAt(stage);
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitUntilHeld(stage));

  const unsigned long start = fakeble::clockMs();
  host().end();
  const unsigned long elapsed = fakeble::clockMs() - start;

  CHECK(!fakeble::taskDeletedWhileHeld());
  CHECK(elapsed < 2000);  // the 8.5 s connect wait is for a GAP cancel, not for this
  CHECK(!host().isRunning());
}

// Scanning from a settings screen cancels a pending reconnect. A reconnect that
// is already pairing must end too, instead of holding the caller for the whole
// connect wait and leaving the attempt running beside the scan.
void testScanCancelsReconnectThatIsPairing() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  fakeble::holdAt(fakeble::Stage::Security);
  CHECK(host().connect(kRemote));
  CHECK(fakeble::waitUntilHeld(fakeble::Stage::Security));

  const unsigned long start = fakeble::clockMs();
  host().startScan(5000);
  const unsigned long elapsed = fakeble::clockMs() - start;

  CHECK(elapsed < 2000);
  CHECK(host().isScanning());
  CHECK(fakeble::waitForWorkerIdle(200));  // the attempt is over, not still pairing
}

// disconnect() is what a settings "Disconnect" row calls. Auto-reconnect must
// not bring the remote straight back 4 s later; an explicit connect(), a new
// begin(), or a link the peer dropped turn it back on.
void testDisconnectIsNotUndoneByAutoReconnect() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));
  CHECK(host().pairedCount() == 1);

  host().disconnect();
  CHECK(!host().isConnected());
  fakeble::advanceMillis(5000);
  host().poll();
  CHECK(!host().isConnecting());
  CHECK(fakeble::connectCalls() == 1);

  // An explicit connect works and re-arms auto-reconnect.
  CHECK(fakeble::connectTo(kRemote));
  fakeble::peerDisconnect();
  fakeble::advanceMillis(5000);
  host().poll();
  CHECK(host().isConnecting());
  CHECK(waitConnected());

  // Bluetooth off and on again also re-arms it.
  host().disconnect();
  host().end();
  CHECK(fakeble::beginHost());
  fakeble::advanceMillis(5000);
  host().poll();
  CHECK(host().isConnecting());
  CHECK(waitConnected());
}

// Remotes that stream a held key send the same keyboard report again while the
// button is down. On a device whose report map also has a Consumer page, the
// generic fallback read that repeat as a new press: two page turns per press.
void testStreamedHeldKeyIsOnePress() {
  fakeble::resetWorld();
  serveRemote(kKeyboardConsumerMap, sizeof kKeyboardConsumerMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  sendKeys(0x51);  // Down arrow pressed
  sendKeys(0x51);  // still held
  sendKeys(0x51);  // still held
  sendKeys(0);     // released
  CHECK(drainKeys() == 1);

  // A keyboard-shaped frame with no key in its key slots still reaches the
  // generic path, as before (here a vendor code in the reserved byte).
  const uint8_t vendor[8] = {0, 0x05, 0, 0, 0, 0, 0, 0};
  const uint8_t idle[8] = {0};
  fakeble::notify(g_inputReport, vendor, sizeof vendor);
  fakeble::notify(g_inputReport, idle, sizeof idle);
  uint8_t code = 0;
  CHECK(drainKeys(&code) == 1);
  CHECK(code == 0x05);
}

// A link that drops while a key is down (remote asleep, out of range) never
// sends that key's release. The next session's first press of the same key
// must still be a press.
void testKeyHeldAcrossLinkDropIsPressedAgain() {
  fakeble::resetWorld();
  serveRemote(kKeyboardMap, sizeof kKeyboardMap);
  CHECK(fakeble::beginHost());
  CHECK(fakeble::connectTo(kRemote));

  sendKeys(0x4E);  // Page Down, link drops before the release frame
  CHECK(drainKeys() == 1);
  fakeble::peerDisconnect();
  for (int i = 0; i < 10; ++i) {
    fakeble::advanceMillis(1000);
    host().poll();
  }
  CHECK(waitConnected());

  sendKeys(0x4E);
  uint8_t code = 0;
  CHECK(drainKeys(&code) == 1);
  CHECK(code == 0x4E);
  sendKeys(0);
}

// The host keeps its own 24-entry scan table. NimBLE's result list is not read
// anywhere, so every advertiser it keeps there is heap the reader cannot use.
void testScanKeepsNoAdvertiserInNimble() {
  fakeble::resetWorld();
  CHECK(fakeble::beginHost());
  host().startScan(5000);
  char addr[18];
  for (int i = 0; i < 10; ++i) {
    std::snprintf(addr, sizeof addr, "AA:BB:CC:00:00:%02X", i);
    fakeble::advertise(addr, "Remote");
  }
  CHECK(host().deviceCount() == 10);
  CHECK(fakeble::retainedScanResults() == 0);
}

}  // namespace

int main() {
  testKeyboardPressIsOneKeyEvent();
  testEndDuringPairingDoesNotDeleteTaskInsideNimble(fakeble::Stage::Security);
  testEndDuringPairingDoesNotDeleteTaskInsideNimble(fakeble::Stage::Discovery);
  testScanCancelsReconnectThatIsPairing();
  testDisconnectIsNotUndoneByAutoReconnect();
  testStreamedHeldKeyIsOnePress();
  testScanKeepsNoAdvertiserInNimble();
  testKeyHeldAcrossLinkDropIsPressedAgain();
  fakeble::resetWorld();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  std::fflush(stdout);
  // Skip static destructors: a connection task the host deleted inside a NimBLE
  // wait is parked on purpose and must not see its fakes torn down.
  std::_Exit(checksFailed == 0 ? 0 : 1);
}
