#pragma once

// Input frame-latch policy (soak replay regression 2026-09-20).
//
// The async input sampling split delivers edges through a two-register latch:
//   * beginFrame(): whatever drained MID-frame (the carry) becomes this
//     frame's delivery; the previous frame's latch dies with it — an edge is
//     delivered to EXACTLY ONE frame (the replay direction).
//   * drain(edges): the FIRST drain after beginFrame publishes into the
//     latch (the frame that will read it); mid-frame drains accumulate into
//     the carry so the next beginFrame delivers them (the lost-input
//     direction — a wait-loop drain must not be erased unread).
//
// Header-only and dependency-free: State needs a default constructor and a
// `void mergeFrom(const State&)` (events OR, context scalars last-wins).
// Host tests exercise this exact code; the SDK's InputManager instantiates
// it for the button edge pair and the touch frame snapshot.
template <typename State>
class InputFrameLatch {
 public:
  void beginFrame() {
    latch_ = carry_;
    carry_ = State{};
    frameBegan_ = true;
  }

  void drain(const State& edges) {
    if (frameBegan_) {
      latch_.mergeFrom(edges);
      frameBegan_ = false;
    } else {
      carry_.mergeFrom(edges);
    }
  }

  const State& latch() const { return latch_; }

 private:
  State latch_{};
  State carry_{};
  bool frameBegan_ = false;
};
