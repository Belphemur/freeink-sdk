// Soak finding 2 (2026-11-10, PR #306 follow-up): the LIFT of a suppressed
// touch contact must not leak into the next consumer's frame.
//
// Device symptom: reader long-press → dictionary selector → definition, all
// mid-contact. The reader suppresses the contact after consuming the
// long-press; when the finger lifts, the GT911 driver raises the Released
// one-shot, and on ASYNC builds consumeTouchFrame()'s exchange delivers that
// DURABLE bit to whoever consumes the next frame — the definition — after the
// suppression latch had already cleared. An unsuppressed tap classifier then
// turned the lift into a page turn ("definition opens on page 1, then flips
// to page 2 with no input").
//
// serviceTouch()'s idle-frame clear now drops the pending Pressed/Released
// one-shots of a suppressed contact in the same pass it clears the latch.
// These tests pin that contract on the real GT911 input path:
//   (i)   suppressed contact: long-press consumed, lift within release slop,
//         next consumer sees NO released edge and NO tap;
//   (ii)  unsuppressed contact: the lift still delivers the Released one-shot
//         and classifies as a tap (no regression);
//   (iii) after the suppressed contact is fully over, the latch clears and a
//         NEW contact is delivered normally.
#define private public
#include <InputManager.h>
#undef private
#include <Logging.h>
#include <Wire.h>

#include <cassert>
#include <cstdio>

namespace {
constexpr uint16_t STATUS = WireStub::STATUS;
constexpr uint16_t POINTS = 0x8150;
constexpr int PX = 300;  // X4 Pro panel coords (touch axis mapping is irrelevant
constexpr int PY = 280;  // here — the finger never moves)

// A one-contact frame + its status byte: 0x80 | count (buffer-ready + count;
// 0x80 itself is the zero-contact idle frame the driver treats as the lift).
void contact(InputManager& in, uint8_t count) {
  Wire.mem[STATUS] = static_cast<uint8_t>(0x80 | count);
  if (count > 0) {
    // X4 Pro touch: gt911CoordsAtByte0 = true (coords at byte 0, no track id).
    // Panel-pixel raw range [0..799]/[0..479] maps unchanged.
    const uint8_t rec[8] = {static_cast<uint8_t>(PX & 0xFF), static_cast<uint8_t>(PX >> 8),
                            static_cast<uint8_t>(PY & 0xFF), static_cast<uint8_t>(PY >> 8)};
    for (int i = 0; i < 4; ++i) {
      // 8-byte records; only the primary contact matters here.
      Wire.mem[POINTS + i] = rec[i];
      Wire.mem[POINTS + 8 + i] = rec[i];
    }
  }
  // touch-update poll: fakeNow advances per tick inside tick().
  fakeNow += 20;
  in.update();
}

void tick(InputManager& in) { contact(in, 0); }

InputManager::TouchFrameState consume(InputManager& in) {
  in.consumeTouchFrame();
  return in.poppedTouch_;
}

int failures = 0;
void expect(const char* what, const bool got, const bool want) {
  if (got != want) {
    printf("  FAIL %-46s got %d, want %d\n", what, got ? 1 : 0, want ? 1 : 0);
    ++failures;
  } else {
    printf("  ok   %-46s %d\n", what, want ? 1 : 0);
  }
}

}  // namespace

int main() {
  {  // (i) suppressed contact: the lift is owned by the suppressor and must
     //     never reach the NEXT consumer as a tap/release.
    printf("(i) suppressed long-press lift\n");
    Wire.loadFactory(1);
    gpioLevels[WireStub::RST_PIN] = 1;
    InputManager in;
    Wire.powerCycle();
    in.begin();
    assert(in.hasTouch());
    // The X4 Pro release runs beginAsync() — fake the poll-task latch so the
    // test drives the SAME durability contract (durable one-shots +
    // consumeTouchFrame()) without a FreeRTOS task on the host.
    in._asyncTask = reinterpret_cast<TaskHandle_t>(1);
    fakeCurrentTask = in._asyncTask;  // update() must believe it polls
    assert(in.asyncActive());

    tick(in);  // idle baseline
    contact(in, 1);
    for (int i = 0; i < 30; ++i) contact(in, 1);  // hold 30 ticks > 500 ms
    {
      const auto frame = consume(in);
      expect("long-press raised", frame.touchLongPressEvent, true);
      // The reader consumed it; suppression latches the rest of the contact.
      in.suppressTouchContact();
      assert(in.touchSuppressed);
    }

    tick(in);   // another idle-tail pass while latched (release must not have
                // arrived yet)
    tick(in);   // lift: zero-contact frame raises Released + clears live flag
                // at the async tail
    tick(in);   // first fully-idle pass: the latch clears AND the durable
                // lift one-shots are dropped
    expect("latch cleared after contact", in.touchSuppressed, false);

    const auto frame = consume(in);
    expect("next consumer: no released edge", frame.touchReleasedEvent, false);
    expect("next consumer: no pressed edge", frame.touchPressedEvent, false);
    float nx = 0, ny = 0;
    expect("next consumer: not a tap", in.wasTouchTap(nx, ny), false);
    float sx = 0, sy = 0, ex = 0, ey = 0;
    expect("next consumer: not a swipe", in.wasSwipe(sx, sy, ex, ey), false);

    // (iii) the latch is dead; a NEW contact must be delivered normally.
    printf("(iii) new contact after the suppressed one\n");
    contact(in, 1);
    {
      const auto f = consume(in);
      expect("new contact: press edge", f.touchPressedEvent, true);
      expect("new contact: press position valid", in.wasTouchPressedAt(nx, ny), true);
    }
    tick(in);
    tick(in);  // lift
    {
      const auto f = consume(in);
      expect("new contact: released edge", f.touchReleasedEvent, true);
      expect("new contact: lift is a tap", in.wasTouchTap(nx, ny), true);
    }
  }

  {  // (ii) unsuppressed contact: the lift must still deliver the Released
     //     one-shot and classify as a tap — the fix only buys suppression.
    printf("(ii) unsuppressed lift regression\n");
    Wire.loadFactory(1);
    gpioLevels[WireStub::RST_PIN] = 1;
    InputManager in;
    Wire.powerCycle();
    in.begin();
    assert(in.hasTouch());
    in._asyncTask = reinterpret_cast<TaskHandle_t>(1);
    fakeCurrentTask = in._asyncTask;
    tick(in);
    contact(in, 1);
    tick(in);
    tick(in);  // lift
    {
      const auto frame = consume(in);
      expect("unsuppressed: released edge delivered", frame.touchReleasedEvent, true);
      expect("unsuppressed: suppression never armed", frame.touchSuppressed, false);
      float nx = 0, ny = 0;
      expect("unsuppressed: lift is a tap", in.wasTouchTap(nx, ny), true);
      expect("unsuppressed: wasTouchReleased", in.wasTouchReleased(), true);
    }
  }

  if (failures == 0) {
    printf("InputManager suppressed-lift host tests: OK\n");
    return 0;
  }
  printf("InputManager suppressed-lift host tests: %d failures\n", failures);
  return 1;
}
