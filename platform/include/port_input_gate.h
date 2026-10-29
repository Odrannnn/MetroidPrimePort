#pragma once

// Gate for the game's pad 0 while the F1 overlay owns the keyboard and the pad. Header-only and
// generic over the status type (PADStatus: button, stickX/Y, substickX/Y, triggerLeft/Right,
// analogA/B) so tests/port_input_gate.cpp can run it without Dolphin.
//
// Open: the game sees a neutral pad, so anything held releases cleanly. Closed: whatever is still
// held (a key used in the menu, or a stick/trigger the keyboard binds) stays masked until it is
// released, so closing the menu never fires a press or a walk the user did not make. A stick
// counts as released when it returns to rest (within a few counts of zero) or flips direction.
namespace PortInputGate {

template <class S>
class Gate {
public:
  // `blocked` is the overlay's visibility; `s` is this iteration's pad status, edited in place.
  void Apply(bool blocked, S& s) {
    if (blocked) {
      Neutralize(s);
      mWasBlocked = true;
      mButtons = 0;
      for (int& d : mAxis) d = 0;
      return;
    }
    if (mWasBlocked) {
      mWasBlocked = false;
      mButtons = static_cast<unsigned>(s.button);
      int v[kAxes];
      Read(s, v);
      for (int i = 0; i < kAxes; ++i) mAxis[i] = Sign(v[i]);
    }
    mButtons &= static_cast<unsigned>(s.button);
    s.button = static_cast<decltype(s.button)>(static_cast<unsigned>(s.button) & ~mButtons);
    int v[kAxes];
    Read(s, v);
    for (int i = 0; i < kAxes; ++i) {
      if (mAxis[i] != 0 && Sign(v[i]) != mAxis[i]) mAxis[i] = 0; // released or reversed
      if (mAxis[i] != 0) v[i] = 0;
    }
    Write(s, v);
  }

private:
  static constexpr int kAxes = 8;
  // An idle stick can read -1, so a few counts of play count as released.
  static constexpr int kIdle = 4;
  static int Sign(int x) { return x >= kIdle ? 1 : x <= -kIdle ? -1 : 0; }
  static void Read(const S& s, int (&v)[kAxes]) {
    v[0] = s.stickX; v[1] = s.stickY; v[2] = s.substickX; v[3] = s.substickY;
    v[4] = s.triggerLeft; v[5] = s.triggerRight; v[6] = s.analogA; v[7] = s.analogB;
  }
  static void Write(S& s, const int (&v)[kAxes]) {
    using Stick = decltype(s.stickX);
    using Trig = decltype(s.triggerLeft);
    s.stickX = static_cast<Stick>(v[0]); s.stickY = static_cast<Stick>(v[1]);
    s.substickX = static_cast<Stick>(v[2]); s.substickY = static_cast<Stick>(v[3]);
    s.triggerLeft = static_cast<Trig>(v[4]); s.triggerRight = static_cast<Trig>(v[5]);
    s.analogA = static_cast<Trig>(v[6]); s.analogB = static_cast<Trig>(v[7]);
  }
  static void Neutralize(S& s) {
    const int zero[kAxes] = {};
    Write(s, zero);
    s.button = 0;
  }

  bool mWasBlocked = false;
  unsigned mButtons = 0;
  int mAxis[kAxes] = {};
};

} // namespace PortInputGate
