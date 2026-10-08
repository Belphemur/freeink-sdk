// Picco: ADC battery + SGM41562 charger @0x03 (CHRG_STAT REG08[4:3], PG_STAT REG08[2],
// ship mode REG06 bit 5) against a register-file model of the charger.
#include <cstdio>
#include <vector>

#include "../../src/BatteryMonitor.cpp"

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

uint8_t regs[16] = {};
bool present = true;
}  // namespace

bool hostI2cWrite(const uint8_t addr, const std::vector<uint8_t>& bytes) {
  if (addr != 0x03 || !present || bytes.size() != 2 || bytes[0] >= sizeof(regs)) return false;
  regs[bytes[0]] = bytes[1];
  return true;
}
bool hostI2cRead(const uint8_t addr, const uint8_t reg, uint8_t* out, const size_t length) {
  if (addr != 0x03 || !present || length != 1 || reg >= sizeof(regs)) return false;
  *out = regs[reg];
  return true;
}

int main() {
  BatteryMonitor bm;
  bool known = false;

  struct Case {
    uint8_t reg08;
    bool charging;
    bool usb;
  } cases[] = {{0x00, false, false}, {0x0C, true, true}, {0x14, true, true}, {0x1C, false, true}, {0x18, false, false}};
  for (const Case& c : cases) {
    regs[0x08] = c.reg08;
    CHECK(bm.isCharging() == c.charging);
    CHECK(bm.isExternalPowerPresent(&known) == c.usb && known);
    const BatteryMonitor::Status s = bm.readStatus();
    CHECK(s.supported && s.chargingKnown && s.charging == c.charging);
    CHECK(s.externalPowerKnown && s.externalPower == c.usb);
  }

  regs[0x06] = 0x81;
  CHECK(BatteryMonitor::enterShipMode());
  CHECK(regs[0x06] == 0xA1);  // BATFET off set, other bits kept

  present = false;
  CHECK(!bm.isExternalPowerPresent(&known) && !known);
  CHECK(!bm.readStatus().chargingKnown);
  CHECK(!BatteryMonitor::enterShipMode());

  std::printf("%d checks, %d failures\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
