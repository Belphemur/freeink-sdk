#pragma once
// Host harness for the Xteink X3/X4 (ESP32-C3) INPUT path. The firmware's
// gh_release build runs BoardConfig's XteinkAdcLadder input style with
// beginAsync() compiled OUT (HalGPIO gates it on BOARD_HAS_PSRAM), so the
// legacy SYNC sampling path is what the C3 release binary actually runs.
// This harness drives the real InputManager through that path so the
// consume-on-check/queue/latch machinery added for the async poll task cannot
// silently change the sync edge contract.
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define IRAM_ATTR
constexpr int HIGH = 1, LOW = 0, INPUT = 1, OUTPUT = 3, INPUT_PULLUP = 5, INPUT_PULLDOWN = 6, FALLING = 2, ADC_11db = 3;

// Host-controllable simulation state.
inline unsigned long fakeNow = 1000;
inline int gpioLevels[64] = {};
inline int adcRaw[8] = {4095, 4095, 4095, 4095, 4095, 4095, 4095, 4095};

inline void pinMode(int, int) {}
inline void digitalWrite(int p, int v) { gpioLevels[p] = v; }
inline int digitalRead(int p) { return (p >= 0 && p < 64) ? gpioLevels[p] : HIGH; }
inline void delay(unsigned long n) { fakeNow += n; }
inline void delayMicroseconds(unsigned int) {}
inline unsigned long millis() { return fakeNow; }
inline void analogSetAttenuation(int) {}
inline int analogReadMilliVolts(int) { return 3300; }
inline int analogRead(int p) { return (p >= 0 && p < 8) ? adcRaw[p] : 0; }
inline int digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int, void (*f)(), int) { (void)f; }
struct SerialStub {
  explicit operator bool() const { return false; }
};
inline SerialStub Serial;
