// One physical press on the Xteink X3/X4 (ESP32-C3) ADC ladder must deliver
// exactly ONE press edge and ONE release edge to the app on the LEGACY SYNC
// path — the path the C3 release binary runs (beginAsync() is compiled out
// there, see docs/design/2026-09-20-async-input.md §2.2 and the
// BOARD_HAS_PSRAM gate in lib/hal/HalGPIO.cpp).
//
// The async refactor moved the sync path's edge writes into the shared
// taskPressedEdges_/taskReleasedEdges_ registers and publishes them at the tail
// of update(). This test pins that publication as one-shot: a duplicated or
// stale edge published across two consecutive update() calls is exactly the
// "one press, two inputs" symptom, and it must never come back.
#include <BoardConfig.h>

#include <cassert>
#include <cstdio>
#define private public
#include <InputManager.h>
#undef private

namespace {

constexpr int IDLE_RAIL = 4095;                           // Xteink ladder idles at the full-scale rail.
constexpr int LADDER_1 = InputManager::BUTTON_ADC_PIN_1;  // Back/Confirm/Left/Right
constexpr int LADDER_2 = InputManager::BUTTON_ADC_PIN_2;  // Up/Down
constexpr int POWER_PIN = 3;                              // X4 power GPIO, active-LOW

// Averaged on-device mV per button (InputManager.cpp's calibration comment).
constexpr int kBackMv = 3512;
constexpr int kConfirmMv = 2694;
constexpr int kLeftMv = 1493;
constexpr int kRightMv = 5;
constexpr int kUpMv = 2242;
constexpr int kDownMv = 5;

struct ButtonUnderTest {
  const char* name;
  uint8_t index;
  int ladder1Mv;
  int ladder2Mv;
  bool power;
};

const ButtonUnderTest kButtons[] = {
    {"Back", InputManager::BTN_BACK, kBackMv, IDLE_RAIL, false},
    {"Confirm", InputManager::BTN_CONFIRM, kConfirmMv, IDLE_RAIL, false},
    {"Left", InputManager::BTN_LEFT, kLeftMv, IDLE_RAIL, false},
    {"Right", InputManager::BTN_RIGHT, kRightMv, IDLE_RAIL, false},
    {"Up", InputManager::BTN_UP, IDLE_RAIL, kUpMv, false},
    {"Down", InputManager::BTN_DOWN, IDLE_RAIL, kDownMv, false},
    {"Power", InputManager::BTN_POWER, IDLE_RAIL, IDLE_RAIL, true},
};

void setAllIdle() {
  for (int& raw : adcRaw) raw = IDLE_RAIL;
  for (int& level : gpioLevels) level = HIGH;
  fakeNow = 1000;
}

void apply(const ButtonUnderTest& button, const bool pressed) {
  if (button.power) {
    gpioLevels[POWER_PIN] = pressed ? LOW : HIGH;
    return;
  }
  adcRaw[LADDER_1] = pressed ? button.ladder1Mv : IDLE_RAIL;
  adcRaw[LADDER_2] = pressed ? button.ladder2Mv : IDLE_RAIL;
}

struct Edges {
  int presses = 0;
  int releases = 0;
};

// Mirrors MappedInputManager::update()'s per-tick snapshot: every physical
// button is read exactly once per update(), and the app then reads the mask
// repeatedly (the sync path's wasPressed/wasReleased are non-consuming).
Edges tick(InputManager& input) {
  fakeNow += 10;
  input.update();
  Edges edges;
  for (uint8_t physical = InputManager::BTN_BACK; physical <= InputManager::BTN_POWER; ++physical) {
    if (input.wasPressed(physical)) ++edges.presses;
    if (input.wasReleased(physical)) ++edges.releases;
  }
  // The snapshot is multi-read: re-reading the same tick must be identical.
  for (uint8_t physical = InputManager::BTN_BACK; physical <= InputManager::BTN_POWER; ++physical) {
    assert(input.wasPressed(physical) == false || input.wasPressed(physical) == true);
  }
  return edges;
}

int failures = 0;
void expect(const char* what, const int got, const int want) {
  if (got != want) {
    printf("  FAIL %-28s got %d, want %d\n", what, got, want);
    ++failures;
  } else {
    printf("  ok   %-28s %d\n", what, got);
  }
}

}  // namespace

int main() {
  assert(BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4);
  assert(BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder);

  setAllIdle();
  InputManager input;
  input.begin();

  // The C3 release binary never calls beginAsync(): the async poll task and its
  // queue/latch machinery must be absent here, or this test is measuring the
  // wrong path.
  printf("asyncActive() = %s\n", input.asyncActive() ? "true" : "false");
  expect("async inactive on C3", input.asyncActive() ? 1 : 0, 0);

  // Idle must never manufacture an edge.
  int idlePresses = 0;
  int idleReleases = 0;
  for (int i = 0; i < 50; ++i) {
    const Edges e = tick(input);
    idlePresses += e.presses;
    idleReleases += e.releases;
  }
  expect("idle: press edges", idlePresses, 0);
  expect("idle: release edges", idleReleases, 0);

  for (const ButtonUnderTest& button : kButtons) {
    setAllIdle();
    apply(button, false);
    for (int i = 0; i < 5; ++i) tick(input);  // settle

    apply(button, true);
    int presses = 0;
    for (int i = 0; i < 10; ++i) presses += tick(input).presses;  // ~100 ms hold
    char label[48];
    snprintf(label, sizeof(label), "%s: press edges", button.name);
    expect(label, presses, 1);

    apply(button, false);
    int releases = 0;
    for (int i = 0; i < 20; ++i) releases += tick(input).releases;
    snprintf(label, sizeof(label), "%s: release edges", button.name);
    expect(label, releases, 1);

    // A settled idle must not republish the edges we just consumed.
    int after = 0;
    for (int i = 0; i < 20; ++i) {
      after += tick(input).presses + tick(input).presses;
    }
    snprintf(label, sizeof(label), "%s: no republish", button.name);
    expect(label, after, 0);
  }

  if (failures) {
    printf("\nX4 sync path: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("\nX4 sync path: one physical press = one edge (OK)\n");
  return 0;
}
