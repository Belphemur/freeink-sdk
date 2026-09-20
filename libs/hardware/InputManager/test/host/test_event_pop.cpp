// Host test for the soak-fix7 consume-on-check input protocol, as a pure
// policy mirror of InputManager's mechanism (the real class is FreeRTOS-
// bound): a nullable-pop event stream (single scratch slot — one operation
// does consume + emptiness check, never returns a zeroed struct), a pending
// UNION cache for edges walked past (nothing dropped), and test-and-clear
// edge checks (checked twice, the second read is false — by contract).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>

enum { kAsyncEventPress = 0, kAsyncEventRelease = 1 };
struct AsyncInputEvent {
  uint8_t button;
  uint8_t kind;
};

// Mirror of InputManager's pop protocol over an injectable source.
struct PopStream {
  struct Source {
    std::deque<AsyncInputEvent> q;
    bool pop(AsyncInputEvent& out) {
      if (q.empty()) return false;
      out = q.front();
      q.pop_front();
      return true;
    }
  } source;
  AsyncInputEvent scratch{};
  // Counts per button (mirror of the SDK's pending cache): two edges of the
  // same button queued before one check deliver two consumable edges.
  uint8_t pendingPressCount[7] = {};
  uint8_t pendingReleaseCount[7] = {};
  uint32_t pops = 0;

  const AsyncInputEvent* popEvent() {
    if (!source.pop(scratch)) return nullptr;
    ++pops;
    return &scratch;
  }
  void drain() {
    while (const AsyncInputEvent* ev = popEvent()) {
      if (ev->kind == kAsyncEventPress) {
        ++pendingPressCount[ev->button];
      } else {
        ++pendingReleaseCount[ev->button];
      }
    }
  }
  bool wasPressed(uint8_t button) {
    drain();
    if (pendingPressCount[button] > 0) {
      --pendingPressCount[button];
      return true;
    }
    return false;
  }
  bool wasReleased(uint8_t button) {
    drain();
    if (pendingReleaseCount[button] > 0) {
      --pendingReleaseCount[button];
      return true;
    }
    return false;
  }
  bool wasAnyPressed() {  // report-only: never consumes
    drain();
    for (const uint8_t count : pendingPressCount) {
      if (count > 0) return true;
    }
    return false;
  }
};

int main() {
  {  // Empty stream: popEvent is nullptr (no zeroed struct handed out), the
     // checks are false and non-blocking, and a later event is still fine.
    PopStream s;
    assert(s.popEvent() == nullptr);
    assert(s.popEvent() == nullptr);
    assert(!s.wasPressed(0));
    assert(!s.wasReleased(0));
    s.source.q.push_back({0, kAsyncEventPress});
    assert(s.wasPressed(0));
  }

  {  // Consume-once: one press delivered exactly once, never replayed.
    PopStream s;
    s.source.q.push_back({0, kAsyncEventPress});
    assert(s.wasPressed(0));
    assert(!s.wasPressed(0));
    assert(!s.wasPressed(0));
  }

  {  // Release edges flow through the same stream, kinds don't cross.
    PopStream s;
    s.source.q.push_back({1, kAsyncEventRelease});
    assert(!s.wasPressed(1));
    assert(s.wasReleased(1));
    assert(!s.wasReleased(1));
  }

  {  // Press during a wait: the edge is delivered exactly once afterwards —
     // never replayed, never dropped (the walk-past press stays pending).
    PopStream s;
    s.source.q.push_back({0, kAsyncEventPress});   // arrives mid-wait
    s.source.q.push_back({2, kAsyncEventRelease}); // the wait loop's own check target
    assert(s.wasReleased(2));  // the wait loop's check: walks past the press
    assert(!s.wasReleased(2)); // consume-once
    assert(s.wasPressed(0));   // the walked-past edge survives, delivered once
    assert(!s.wasPressed(0));
  }

  {  // No loss under volume: 5 mixed events all delivered exactly once.
    PopStream s;
    for (uint8_t b = 0; b < 5; ++b) s.source.q.push_back({b, kAsyncEventPress});
    for (uint8_t b = 0; b < 5; ++b) assert(s.wasPressed(b));
    for (uint8_t b = 0; b < 5; ++b) assert(!s.wasPressed(b));
    assert(s.pops == 5);  // each event popped from the stream exactly once
  }

  {  // Double-read of the same edge loses it on the second read (documented
     // consume-on-check contract) — call sites must read into a local.
    PopStream s;
    s.source.q.push_back({3, kAsyncEventPress});
    assert(s.wasPressed(3));
    assert(!s.wasPressed(3));
  }

  {  // wasAnyPressed reports without consuming: the specific check still fires.
    PopStream s;
    s.source.q.push_back({1, kAsyncEventPress});
    assert(s.wasAnyPressed());
    assert(s.wasAnyPressed());  // still pending
    assert(s.wasPressed(1));
    assert(!s.wasAnyPressed());
  }

  {  // Rapid repeated edges of the SAME button survive the pending cache:
     // two full clicks queued before one check deliver TWO consumable
     // releases (the double-click gesture depends on it).
    PopStream s;
    s.source.q.push_back({0, kAsyncEventPress});
    s.source.q.push_back({0, kAsyncEventRelease});
    s.source.q.push_back({0, kAsyncEventPress});
    s.source.q.push_back({0, kAsyncEventRelease});
    assert(s.wasReleased(0));  // first release
    assert(s.wasPressed(0));   // second press — a bit cache would have lost it
    assert(s.wasReleased(0));  // second release
    assert(s.wasPressed(0));   // second press delivered (4 events, 4 consumptions)
    assert(!s.wasPressed(0) && !s.wasReleased(0));  // stream empty
  }

  std::printf("event-pop policy tests: OK\n");
  return 0;
}
