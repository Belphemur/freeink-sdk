// X4 Pro GT911 INT wake (FREEINK_GT911_INT_WAKE) against a model controller:
// config read, checksum patch, Config_Fresh consumption, read-compare-skip, the
// status clear, and the I2C-failure fallback (retry, then withdraw the INT as a
// wake source until the status register is reachable again). Built once with
// the flag and once without; without it the driver must not touch the config.
#define private public
#include <InputManager.h>
#undef private
#include <Logging.h>
#include <Wire.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
constexpr int8_t INT_PIN = WireStub::INT_PIN;

// A device boot. begin()'s reset dance raises RST, which makes the model
// controller reload its stored table (gt911_stubs/Wire.h).
void boot(InputManager& in) {
  Wire.resetCounters();
  gpioModes[INT_PIN] = 0;
  loggedErrors = loggedInfos = 0;
  in.begin();
  assert(in.hasTouch());
}

void poll(InputManager& in) {
  fakeNow += 20;
  in.update();
}

// --- park entry (2026-11-09-gt911-park-recovery.md §3.1) -------------------
// A SENT sleep command parks the controller whatever the post-command probe
// says; only a NACKed (unsent) command stays awake.
void testSentSleepCommandParksRegardlessOfProbe() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);

  // The probe ACKs — the exact racing outcome that left touch dead until a
  // chip reset before the fix.
  Wire.forceProbeStatus = 0;
  in.enterGt911Sleep();
  assert(in.isTouchAsleep());
  assert(std::strstr(lastLog, "sleep entered") != nullptr);
  Wire.forceProbeStatus = -1;
  // The wake handshake clears the parked state again.
  assert(in.exitGt911Sleep() && !in.isTouchAsleep());
#if FREEINK_GT911_INT_WAKE
  assert(gpioModes[INT_PIN] == INPUT_PULLUP);  // level-wake pin re-armed
#else
  assert(gpioModes[INT_PIN] == INPUT);
#endif

  // Probe anomaly: parked too, with the code logged.
  Wire.forceProbeStatus = 4;
  in.enterGt911Sleep();
  assert(in.isTouchAsleep() && std::strstr(lastLog, "probe code 4") != nullptr);
  Wire.forceProbeStatus = -1;
  assert(in.exitGt911Sleep());

  // A NACKed command was never sent: uncontroversially stay awake.
  Wire.busDead = true;
  in.enterGt911Sleep();
  assert(!in.isTouchAsleep() && std::strstr(lastLog, "NACKed") != nullptr);
  Wire.busDead = false;
}

// --- poll self-heal ladder (§3.2) ------------------------------------------
// Failed 0x814E reads fire the INT wake handshake after 5, a failed handshake
// schedules the rate-limited beginGt911() re-probe, and a read that works
// resets every step.
void testPollSelfHealLadder() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  poll(in);  // healthy baseline

  // Data phase dead (write phase ACKs): 4 failed polls stay silent.
  Wire.failReads = true;
  const unsigned infos0 = loggedInfos;
  for (int i = 0; i < 4; ++i) poll(in);
  assert(loggedInfos == infos0 && in.gt911ReadFails == 4);

  // The 5th fires the handshake; the chip never ACKs, so it times out (the
  // timeout error is the last line logged).
  const unsigned errs0 = loggedErrors;
  poll(in);
  assert(loggedInfos == infos0 + 1);
  assert(loggedErrors == errs0 + 1 && std::strstr(lastLog, "wake timeout") != nullptr);
  assert(in.gt911WakeBlocked && !in.isTouchAsleep() && in.gt911ReadFails == 0);

  // Rate limit: further failures inside the window do nothing.
  const unsigned infos1 = loggedInfos;
  const unsigned errs1 = loggedErrors;
  for (int i = 0; i < 6; ++i) poll(in);
  assert(loggedInfos == infos1 && loggedErrors == errs1 && in.gt911WakeBlocked);

  // After the deadline the next step is the re-probe; the address probe ACKs
  // again, so the controller comes back.
  fakeNow += InputManager::GT911_REPROBE_INTERVAL_MS;
  const unsigned infos2 = loggedInfos;
  const unsigned errs2 = loggedErrors;
  // Gate-blocked ladder steps reset the counter, so counts here run 2..4:
  // three silent polls, then the fire.
  for (int i = 0; i < 3; ++i) poll(in);
  assert(loggedInfos == infos2 && loggedErrors == errs2);  // still gated
  poll(in);
  assert(loggedErrors == errs2 + 1);  // the re-probe's LOG_ERR
#if FREEINK_GT911_INT_WAKE
  // Its re-configure rewrote the log line on the INT-wake build.
  assert(std::strstr(lastLog, "patched this boot") != nullptr);
#else
  assert(std::strstr(lastLog, "re-probing") != nullptr);
#endif

  // A good read after the re-probe leaves the ladder fully idle.
  Wire.failReads = false;
  Wire.report(0);
  poll(in);
  assert(!in.gt911WakeBlocked && in.gt911ReprobeAt == 0 && in.gt911ReadFails == 0);
  assert(!in.isTouchAsleep() && in.gt911Addr != 0);

  // And the ladder starts over cleanly from the reset state.
  Wire.failReads = true;
  const unsigned infos3 = loggedInfos;
  for (int i = 0; i < 5; ++i) poll(in);
  assert(loggedInfos == infos3 + 1);
  Wire.failReads = false;  // knob owns the poll loop: restore for later tests
}

// A failed re-probe strands gt911Addr at 0, where no read can count anymore;
// the missing-controller tail must re-run the re-probe at the rate limit and
// recover for a chip that comes back.
void testFailedReprobeRetriesUntilTheChipReturns() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  poll(in);  // healthy baseline

  // Handshake fails: ladder moves to the re-probe.
  Wire.failReads = true;
  for (int i = 0; i < 4; ++i) poll(in);
  poll(in);
  assert(in.gt911WakeBlocked && !in.isTouchAsleep());

  // At the rate window the re-probe runs — with the chip gone it leaves the
  // address unresolved and the ladder must keep retrying, not strand touch.
  fakeNow += InputManager::GT911_REPROBE_INTERVAL_MS;
  Wire.busDead = true;  // chip truly unreachable at that attempt
  const unsigned errs0 = loggedErrors;
  for (int i = 0; i < 4; ++i) poll(in);  // counter 2..4, silent
  poll(in);
  assert(loggedErrors == errs0 + 1 && in.gt911Addr == 0 && in.gt911WakeBlocked);

  // Inside the window: one missing-controller pass, nothing.
  const unsigned errs1 = loggedErrors;
  poll(in);
  assert(loggedErrors == errs1);

  // Chip comes back: the next rate window re-probes; the re-probe poll
  // returns before reading, and the following healthy read clears the ladder.
  Wire.busDead = false;
  Wire.failReads = false;
  fakeNow += InputManager::GT911_REPROBE_INTERVAL_MS;
  poll(in);
  assert(in.gt911Addr != 0 && !in.isTouchAsleep());
  Wire.report(0);
  poll(in);
  assert(!in.gt911WakeBlocked && in.gt911ReprobeAt == 0 && in.gt911ReadFails == 0);
}

#if FREEINK_GT911_INT_WAKE
// A handshake that cannot clear the controller's latched report must not
// re-arm the level wake; the fault machinery clears it on a later poll, and
// a clean clear restores the configured INT mode with its pull-up.
void testHandshakeLiftsFaultOnlyOnCleanClear() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);

  Wire.report(0x81);  // pending report
  Wire.busDead = true;
  poll(in);           // latches the status fault, INT withdrawn
  Wire.busDead = false;
  assert(in.touchWakeIrqPin() == -1 && in.gt911StatusFault);

  Wire.failStatusClears = 1;
  assert(in.gt911WakeHandshake());  // read works, clear NACKs
  assert(in.gt911StatusFault && in.touchWakeIrqPin() == -1);

  Wire.failStatusClears = 0;
  assert(in.gt911WakeHandshake());
  assert(!in.gt911StatusFault && in.touchWakeIrqPin() == INT_PIN);
  assert(gpioModes[INT_PIN] == INPUT_PULLUP);
}
#endif
#if FREEINK_GT911_INT_WAKE
void testPatchAppliedOncePerBootWhenDroppedOnReset() {
  Wire.loadFactory(1);  // falling edge, the X4 Pro factory mode
  Wire.persistent = false;
  for (int bootNo = 0; bootNo < 3; ++bootNo) {
    InputManager in;
    boot(in);
    // Whole block read in three bounded chunks plus the Module_Switch1
    // readback, and exactly one short apply: Config_Version, Module_Switch1,
    // checksum + Config_Fresh.
    assert(Wire.configReads == 4);
    assert(Wire.configWriteTxns == 3 && Wire.applies == 1);
    assert(Wire.mem[WireStub::CFG] == 0x00);
    assert(Wire.activeIntMode() == 2 && (Wire.mem[WireStub::SWITCH1] & ~0x03) == 0x30);
    assert(WireStub::checksum(&Wire.mem[WireStub::CFG]) == Wire.mem[WireStub::CHK]);
    assert(Wire.mem[WireStub::FRESH] == 0);
    // The report latched by the re-read is cleared, so INT idles released.
    assert(Wire.mem[WireStub::STATUS] == 0 && gpioLevels[INT_PIN] == HIGH);
    assert(gpioModes[INT_PIN] == INPUT_PULLUP);
    assert(in.touchWakeIrqPin() == INT_PIN && in.touchWakeIrqActiveLow());
    assert(loggedErrors == 0 && std::strstr(lastLog, "patched this boot"));

    // Polling never writes the config again.
    for (int i = 0; i < 5; ++i) poll(in);
    assert(Wire.configWriteTxns == 3 && Wire.applies == 1);
    // A second begin() resets the controller, which drops the patch again:
    // exactly one more apply, no more.
    Wire.resetCounters();
    in.begin();
    assert(Wire.configWriteTxns == 3 && Wire.applies == 1 && in.touchWakeIrqPin() == INT_PIN);
  }
}

void testRebeginRefusedWithdrawsPin() {
  // A first begin() arms the pin; a later begin() resets the controller back
  // to its edge mode and this time the table is refused. The earlier result
  // must not survive: the line only pulses now.
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.touchWakeIrqPin() == INT_PIN);
  Wire.acceptConfig = false;
  Wire.resetCounters();
  in.begin();
  assert(Wire.applies == 1 && Wire.activeIntMode() == 1);
  assert(in.touchWakeIrqPin() == -1);
  Wire.acceptConfig = true;
}

void testNoWriteWhenModeAlreadyLowLevel() {
  // Panel that keeps an applied table: the second boot reads low level back
  // from its own table and is never written.
  Wire.loadFactory(1);
  Wire.persistent = true;
  {
    InputManager in;
    boot(in);
    assert(Wire.applies == 1 && in.touchWakeIrqPin() == INT_PIN);
  }
  {
    InputManager in;
    boot(in);
    assert(Wire.configReads == 4 && Wire.configWriteTxns == 0 && Wire.applies == 0);
    assert(in.touchWakeIrqPin() == INT_PIN && std::strstr(lastLog, "already set"));
  }
  Wire.persistent = false;

  // Factory table already in low-level mode: never written.
  Wire.loadFactory(2);
  InputManager in;
  boot(in);
  assert(Wire.configWriteTxns == 0 && Wire.applies == 0);
  assert(Wire.mem[WireStub::CFG] == 0x41);  // version untouched
  assert(in.touchWakeIrqPin() == INT_PIN && loggedErrors == 0);
}

void testRefusals() {
  // Checksum mismatch: the table is not understood, so nothing is written.
  Wire.loadFactory(1);
  Wire.factory[5] ^= 0xFF;
  {
    InputManager in;
    boot(in);
    assert(Wire.configWriteTxns == 0 && in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1 && std::strstr(lastLog, "checksum"));
    assert(gpioModes[INT_PIN] != INPUT_PULLUP);
  }

  // Controller never clears Config_Fresh: one apply, bounded wait, no retry.
  Wire.loadFactory(1);
  Wire.acceptConfig = false;
  {
    InputManager in;
    const unsigned long before = fakeNow;
    boot(in);
    assert(Wire.applies == 1 && Wire.configWriteTxns == 3);
    assert(fakeNow - before < 1000);
    assert(in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1 && std::strstr(lastLog, "Config_Fresh"));
    for (int i = 0; i < 5; ++i) poll(in);
    assert(Wire.applies == 1 && in.touchWakeIrqPin() == -1);
    // The register holds the patch, but the controller still runs edge mode.
    assert(Wire.activeIntMode() == 1 && (Wire.mem[WireStub::SWITCH1] & 0x03) == 2);
    // Never offered as a wake source: a bus fault costs one attempt per poll
    // and logs nothing, as without the flag.
    Wire.busDead = true;
    Wire.resetCounters();
    poll(in);
    poll(in);
    Wire.busDead = false;
    assert(Wire.statusReadAttempts == 2 && loggedErrors == 1);
  }
  Wire.acceptConfig = true;

  // Table consumed but the INT mode did not change: the readback guard holds.
  Wire.loadFactory(1);
  Wire.honorIntMode = false;
  {
    InputManager in;
    boot(in);
    assert(Wire.applies == 1 && in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1 && std::strstr(lastLog, "not low level"));
  }
  Wire.honorIntMode = true;

  // Profile that claims an active-high INT: not even read.
  Wire.loadFactory(1);
  BoardConfig::ACTIVE.touch.irqActiveLow = false;
  {
    InputManager in;
    boot(in);
    assert(Wire.configReads == 0 && Wire.configWriteTxns == 0 && in.touchWakeIrqPin() == -1);
    assert(loggedErrors == 1);
  }
  BoardConfig::ACTIVE.touch.irqActiveLow = true;
}

void testStatusReadFailureWithdrawsWake() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.touchWakeIrqPin() == INT_PIN);

  // Dead bus with a report pending: INT is held LOW and cannot be cleared.
  Wire.report(0x81);
  Wire.busDead = true;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusReadAttempts == 3);  // bounded retry
  assert(in.touchWakeIrqPin() == -1);    // not armed while INT may be stuck
  assert(loggedErrors == 1);
  // Further polls: a single attempt each and no further log.
  poll(in);
  poll(in);
  assert(Wire.statusReadAttempts == 5 && loggedErrors == 1 && in.touchWakeIrqPin() == -1);
  assert(gpioLevels[INT_PIN] == LOW);

  // Bus back: the pending report is read and cleared, and the pin returns.
  Wire.busDead = false;
  Wire.report(0x80);
  const unsigned infos = loggedInfos;
  poll(in);
  assert(Wire.mem[WireStub::STATUS] == 0 && gpioLevels[INT_PIN] == HIGH);
  assert(in.touchWakeIrqPin() == INT_PIN && loggedInfos == infos + 1);

  // A transient glitch inside the retry budget never withdraws the pin.
  Wire.report(0x80);
  Wire.failStatusClears = 2;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClearAttempts == 3 && Wire.statusClears == 1);
  assert(in.touchWakeIrqPin() == INT_PIN && loggedErrors == 1);

  // Recovery with nothing pending: a clean all-zero status read is enough,
  // and it costs no write.
  Wire.busDead = true;
  poll(in);
  assert(in.touchWakeIrqPin() == -1 && loggedErrors == 2);
  Wire.busDead = false;
  Wire.report(0);
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClearAttempts == 0 && in.touchWakeIrqPin() == INT_PIN);
}

void testStatusClearFailureWithdrawsWake() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);

  // Status reads fine, but the clear keeps NACKing: INT stays asserted.
  Wire.report(0x80);
  Wire.failStatusClears = 100;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClearAttempts == 3 && in.touchWakeIrqPin() == -1 && loggedErrors == 1);
  poll(in);
  assert(Wire.statusClearAttempts == 4 && loggedErrors == 1);

  Wire.failStatusClears = 0;
  poll(in);
  assert(Wire.mem[WireStub::STATUS] == 0 && in.touchWakeIrqPin() == INT_PIN);
}

void testInitialClearFailure() {
  // The table is consumed, but the clear of the report it latched fails: the
  // line is configured yet withheld until a poll clears the register.
  Wire.loadFactory(1);
  Wire.failStatusClears = 3;
  InputManager in;
  boot(in);
  assert(Wire.applies == 1 && Wire.activeIntMode() == 2);
  assert(gpioLevels[INT_PIN] == LOW && in.touchWakeIrqPin() == -1 && loggedErrors == 1);
  poll(in);
  assert(gpioLevels[INT_PIN] == HIGH && in.touchWakeIrqPin() == INT_PIN);
}

void testNotReadyLatchedStatusIsCleared() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  // Nonzero status without the buffer-ready bit still holds INT LOW.
  Wire.report(0x01);
  assert(gpioLevels[INT_PIN] == LOW);
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusClears == 1 && gpioLevels[INT_PIN] == HIGH);
  // The all-zero idle pass stays a single read, no write.
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusReadAttempts == 1 && Wire.statusClearAttempts == 0);
}

#else
void testFlagOffLeavesConfigAlone() {
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(Wire.configReads == 0 && Wire.configWriteTxns == 0 && Wire.applies == 0);
  assert(Wire.activeIntMode() == 1 && gpioModes[INT_PIN] != INPUT_PULLUP);
  assert(in.touchWakeIrqPin() == -1);
  // Polling is unchanged: a failed status read is a single attempt.
  Wire.busDead = true;
  Wire.resetCounters();
  poll(in);
  assert(Wire.statusReadAttempts == 1);
  Wire.busDead = false;
}
#endif

// --- gesture-mode park (design 2026-11-16-gt911-gesture-touch-wake.md) ------

// The public park API posts through the sleep/wake funnel, which executes
// inline only on the poll owner: stamp begin()'s owner with a non-null cookie.
int gt911PollOwner;
void parkTestSetup() { fakeCurrentTask = &gt911PollOwner; }

// Entry writes 8 to the 0x8046 gesture slot BEFORE the 0x8040 commit; a sent
// commit parks unconditionally (fail-safe park rule); the wake-edge ISR arms
// on the profile's active-low polarity and the pin stops being driven.
void testGestureParkEntry() {
  parkTestSetup();
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  Wire.writeLog.clear();

  assert(in.setTouchSleepGesture());
  assert(in.isTouchAsleep());
  assert(in.touchParkMode() == InputManager::Gt911ParkMode::Gesture);
  assert(Wire.writeLog.size() >= 2);
  const uint16_t slot = Wire.writeLog[Wire.writeLog.size() - 2];
  const uint16_t commit = Wire.writeLog[Wire.writeLog.size() - 1];
  assert(slot == 0x8046 && commit == 0x8040);
  assert(Wire.mem[0x8046] == 8 && Wire.mem[0x8040] == 8);
  // ISR armed on the polarity edge; the pin is an input with a pull-up so the
  // controller's pulse is visible.
  assert(gpioIsrHandlers[INT_PIN] != nullptr);
  assert(gpioIsrModes[INT_PIN] == FALLING);
  assert(gpioModes[INT_PIN] == INPUT_PULLUP);
  // A second gesture request while parked is a no-op (no re-send).
  const size_t writesBefore = Wire.writeLog.size();
  assert(in.setTouchSleepGesture());
  assert(Wire.writeLog.size() == writesBefore);
}

// Parked state without a probe proof: an anomaly probe still parks; a NACKed
// commit (nothing was sent) stays awake and never arms the ISR.
void testGestureParkFailSafe() {
  parkTestSetup();
  Wire.loadFactory(1);
  InputManager in;
  boot(in);

  Wire.forceProbeStatus = 4;  // anomaly: diagnostic only
  assert(in.setTouchSleepGesture());
  assert(in.isTouchAsleep() && in.touchParkMode() == InputManager::Gt911ParkMode::Gesture);
  Wire.forceProbeStatus = -1;

  // Wake up again for the NACK case.
  assert(in.exitGt911Sleep());
  assert(!in.isTouchAsleep() && gpioIsrHandlers[INT_PIN] == nullptr);

  Wire.busDead = true;
  assert(!in.setTouchSleepGesture());
  assert(!in.isTouchAsleep() && gpioIsrHandlers[INT_PIN] == nullptr);
  assert(gpioModes[INT_PIN] == INPUT);  // awake: stop contending on INT
  Wire.busDead = false;
}

// The ISR posts the latch only; the app drains it and runs the unchanged wake
// handshake, which disarms the ISR and clears the park mode on success.
void testGestureWakeLatchAndWake() {
  parkTestSetup();
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.setTouchSleepGesture());
  assert(!in.wasTouchWakeEdgePending());

  fireGpioIsr(INT_PIN);  // the controller's >250 µs wake pulse
  assert(in.wasTouchWakeEdgePending());
  // pollGt911 must NOT drain the latch: it stays until the app clears it.
  poll(in);
  assert(in.wasTouchWakeEdgePending());

  in.clearTouchWakeEdge();
  assert(!in.wasTouchWakeEdgePending());
  assert(in.exitGt911Sleep());
  assert(!in.isTouchAsleep() && in.touchParkMode() == InputManager::Gt911ParkMode::Sleep);
  assert(gpioIsrHandlers[INT_PIN] == nullptr);
}

// Fallback (design §2.3): a stuck INT line fires the ISR but the wake
// handshake never finds an ACK. N=3 consecutive failures latch "gesture
// unsupported" for this boot, disarm the ISR, and re-park in Sleep mode.
void testGestureUnsupportedFallback() {
  parkTestSetup();
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.setTouchSleepGesture());
  Wire.failReads = true;  // the handshake's status reads never complete

  for (int i = 0; i < InputManager::GT911_GESTURE_WAKE_FAILS - 1; ++i) {
    assert(!in.exitGt911Sleep());
    assert(!in.gt911GestureUnsupported);
    assert(gpioIsrHandlers[INT_PIN] != nullptr);  // still trying gesture
  }
  // The 3rd failure logs the wake timeout AND the one fallback line (the
  // re-park's own sleep-entry line overwrites lastLog, so pin the count).
  const unsigned errsBefore = loggedErrors;
  assert(!in.exitGt911Sleep());
  assert(loggedErrors == errsBefore + 2);
  assert(in.gt911GestureUnsupported);
  assert(in.touchParkMode() == InputManager::Gt911ParkMode::Sleep);
  assert(gpioIsrHandlers[INT_PIN] == nullptr);  // edge capture withdrawn
  // Writes still ACK, so the re-park's sleep command landed: the controller
  // is parked in Sleep mode, not left "awake" on a dead bus.
  assert(in.isTouchAsleep());
  assert(Wire.writeLog.back() == 0x8040 && Wire.mem[0x8040] == 0x05);

  // A later gesture request parks Sleep mode directly (no gesture attempt).
  Wire.failReads = false;
  assert(in.exitGt911Sleep());
  Wire.writeLog.clear();
  assert(in.setTouchSleepGesture());
  assert(in.isTouchAsleep() && in.touchParkMode() == InputManager::Gt911ParkMode::Sleep);
  assert(gpioIsrHandlers[INT_PIN] == nullptr);
  assert(Wire.mem[0x8040] == 0x05);  // sleep command, not the gesture commit
  for (const uint16_t reg : Wire.writeLog) assert(reg != 0x8046);
}

// A begin() re-probe (chip reset) and the deep-sleep boundary both drop the
// gesture edge capture; the unsupported latch survives the re-probe.
void testGestureDisarmOnReprobeAndDeepSleep() {
  parkTestSetup();
  Wire.loadFactory(1);
  InputManager in;
  boot(in);
  assert(in.setTouchSleepGesture());
  assert(gpioIsrHandlers[INT_PIN] != nullptr);

  // Re-probe: the reset dance reloads the controller, park state is gone.
  boot(in);
  assert(gpioIsrHandlers[INT_PIN] == nullptr);
  assert(in.touchParkMode() == InputManager::Gt911ParkMode::Sleep);

  // Deep-sleep boundary: never leave the ISR armed across the rail cut.
  assert(in.setTouchSleepGesture());
  assert(gpioIsrHandlers[INT_PIN] != nullptr);
  assert(in.prepareForDeepSleep());
  assert(gpioIsrHandlers[INT_PIN] == nullptr);
}

}  // namespace

int main() {
  testSentSleepCommandParksRegardlessOfProbe();
  testPollSelfHealLadder();
  testFailedReprobeRetriesUntilTheChipReturns();
  testGestureParkEntry();
  testGestureParkFailSafe();
  testGestureWakeLatchAndWake();
  testGestureUnsupportedFallback();
  testGestureDisarmOnReprobeAndDeepSleep();
#if FREEINK_GT911_INT_WAKE
  testHandshakeLiftsFaultOnlyOnCleanClear();
  testPatchAppliedOncePerBootWhenDroppedOnReset();
  testRebeginRefusedWithdrawsPin();
  testNoWriteWhenModeAlreadyLowLevel();
  testRefusals();
  testStatusReadFailureWithdrawsWake();
  testStatusClearFailureWithdrawsWake();
  testInitialClearFailure();
  testNotReadyLatchedStatusIsCleared();
  std::puts("GT911 INT wake (FREEINK_GT911_INT_WAKE=1) passed");
#else
  testFlagOffLeavesConfigAlone();
  std::puts("GT911 INT wake (FREEINK_GT911_INT_WAKE=0) passed");
#endif
}
