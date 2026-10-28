#include <FrontlightManager.h>
#include <Logging.h>
#include <Wire.h>

#include <cassert>
#include <cstdio>

int main() {
  FrontlightManager light;
  light.begin();
  assert(light.present());
  light.setColorTemperature(100);
  light.setBrightness(0);
  light.setBrightness(55);  // same nonzero saved level as the hardware report
  assert(Wire.visible());   // first on must work without moving a slider
  assert(Wire.rejectedWrites == 0);
  assert(Wire.regs[4] > 0 && (Wire.regs[0] & 2));
  const auto awakeTime = fakeNow;
  light.setBrightness(56);
  light.setColorTemperature(0);
  assert(Wire.visible() && fakeNow == awakeTime);  // no wake delay during slider updates
  assert(Wire.regs[3] > 0 && (Wire.regs[0] & 4));
  const auto cool = Wire.regs[3];
  light.off();
  assert(!Wire.visible());
  light.on();
  assert(Wire.visible() && Wire.regs[3] == cool && fakeNow >= awakeTime + 2);
  light.off();
  Wire.failRegister = 0;  // cannot leave standby: log and leave configuration retryable
  light.on();
  assert(!Wire.visible() && loggedErrors > 0);
  light.setBrightness(56);
  assert(Wire.visible());
  std::puts("Picco frontlight: first on, wake timing, saved level, warm/cool, and retry passed");
}
