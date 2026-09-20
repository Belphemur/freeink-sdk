// Frame-latch policy tests for the async input split (soak replay regression
// 2026-09-20). These compile the REAL policy header (InputFrameLatch.h) with
// no FreeRTOS and pin the two soak regressions:
//   * replay  — an edge delivered in a frame must NOT re-deliver in later
//     frames (exactly-once);
//   * lost-input — an edge drained MID-frame (a wait-loop update()) must be
//     delivered by the NEXT beginFrame, not erased unread.
// ButtonEdges/TouchFrame stand in for the SDK's InputManager.h types (which
// pull FreeRTOS); they carry the same merge contracts the real
// TouchFrameState::mergeFrom documents (events OR, context scalars last-wins).

#include <cassert>
#include <cstdint>

#include "InputFrameLatch.h"

namespace {

struct ButtonEdges {
  uint8_t pressed = 0;
  uint8_t released = 0;
  void mergeFrom(const ButtonEdges& other) {
    pressed |= other.pressed;
    released |= other.released;
  }
};

// Mirrors the TouchFrameState merge contract with a minimal field set.
struct TouchFrame {
  bool event = false;
  uint16_t geometry = 0;
  void mergeFrom(const TouchFrame& other) {
    event |= other.event;
    geometry = other.geometry;  // context scalars: newest wins
  }
};

constexpr uint8_t kBit = 1u << 3;  // any button

}  // namespace

// Exactly-once: a drain published after beginFrame is readable that frame,
// then dies at the next beginFrame (the replay regression).
static void testExactlyOnce() {
  InputFrameLatch<ButtonEdges> latch;
  latch.beginFrame();
  latch.drain(ButtonEdges{0, kBit});
  assert(latch.latch().released == kBit);
  latch.beginFrame();
  latch.drain(ButtonEdges{});  // empty drain must not resurrect the edge
  assert(latch.latch().released == 0);
}

// Mid-frame carry: a drain between beginFrames (a wait-loop update()) is
// delivered by the NEXT beginFrame, not erased (the lost-input regression).
static void testMidFrameCarry() {
  InputFrameLatch<ButtonEdges> latch;
  latch.beginFrame();
  latch.drain(ButtonEdges{});
  // Mid-frame: this edge was drained by a wait loop after the frame's input
  // phase already ran.
  latch.drain(ButtonEdges{kBit, 0});
  assert(latch.latch().pressed == 0);  // not visible in the current latch
  latch.beginFrame();                  // next frame: the carry delivers
  assert(latch.latch().pressed == kBit);
  latch.beginFrame();
  assert(latch.latch().pressed == 0);  // and exactly once
}

// First drain after beginFrame publishes into THIS frame (the app reads it
// without an extra beginFrame); a second same-frame drain defers to the next.
static void testFirstDrainPublishes() {
  InputFrameLatch<ButtonEdges> latch;
  latch.beginFrame();
  latch.drain(ButtonEdges{0, kBit});
  assert(latch.latch().released == kBit);
  latch.drain(ButtonEdges{0, kBit});
  latch.beginFrame();
  assert(latch.latch().released == kBit);
  latch.beginFrame();
  assert(latch.latch().released == 0);
}

// Touch contract: events OR within a sink; context scalars take the newest
// value (last-wins), matching TouchFrameState::mergeFrom. The frame boundary
// is exactly-once: the carry carries only ITS OWN drains — the previous
// frame's latch dies at beginFrame (a mid-frame event is delivered the next
// frame; a frame-1 event does not leak into frame 2).
static void testTouchMergeContract() {
  InputFrameLatch<TouchFrame> latch;
  latch.beginFrame();
  latch.drain(TouchFrame{true, 10});
  assert(latch.latch().event && latch.latch().geometry == 10);
  // Mid-frame second event with newer geometry: event ORs within the carry,
  // geometry last-wins.
  latch.drain(TouchFrame{true, 20});
  latch.beginFrame();
  assert(latch.latch().event && latch.latch().geometry == 20);
  // Exactly-once across the boundary: frame 1's event does not leak here.
  latch.beginFrame();
  assert(!latch.latch().event);
}

int main() {
  testExactlyOnce();
  testMidFrameCarry();
  testFirstDrainPublishes();
  testTouchMergeContract();
  return 0;
}
