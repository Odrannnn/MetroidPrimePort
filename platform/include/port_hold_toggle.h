#pragma once

// Hold-to-toggle helpers for the gameplay input (accessibility options). Both
// are pure state machines over one button so they can be tested without the
// game; CStateManager::ProcessInput feeds them the player's pad each tick and
// writes the result back into the CFinalInput the player reads.

namespace PortHoldToggle {

// What the game should see for one button this tick.
struct Output {
  bool held;
  bool pressed; // the press edge (CFinalInput's P* flags)
};

// Toggle lock-on: a press that locks on (or scans, or grapples) latches L as
// held until the next press, so those work without holding the trigger. A press
// with nothing to lock on is a plain hold (strafe while held), and only latches
// if a lock comes during that hold. The latch also lets go by itself when its
// lock ends (target killed or out of range), since a latched L would otherwise
// keep Samus strafing.
class Toggle {
public:
  // active: gameplay has control and the option applies (unmorphed, input not
  // disabled). locked: the player was locked on an object or grappling at the end
  // of the previous tick (orbiting a carcass or a point is not a lock).
  Output Update(bool active, bool held, bool pressed, bool locked) {
    if (!active) {
      Reset();
      return {held, pressed};
    }
    if (mLatched && mWasLocked && !locked) {
      mLatched = false;
      mSwallow = held; // the finger may still be on the button
    }
    if (mPending && held && !pressed && locked) {
      mPending = false;
      mLatched = true;
    }
    mWasLocked = mLatched && locked;
    if (mSwallow) {
      if (held && !pressed) {
        return {false, false};
      }
      mSwallow = false;
    }
    if (mLatched) {
      if (pressed) {
        mLatched = false;
        mSwallow = true;
        return {false, false};
      }
      return {true, false};
    }
    // Not latched: the button passes through, and a press waits for its lock.
    mPending = held && (pressed || mPending);
    return {held, pressed};
  }
  void Reset() {
    mLatched = false;
    mSwallow = false;
    mWasLocked = false;
    mPending = false;
  }
  bool Latched() const { return mLatched; }

private:
  bool mLatched = false;
  bool mSwallow = false; // a press that ended the latch, hidden until released
  bool mWasLocked = false;
  bool mPending = false; // held since a press, no lock yet
};

// Sticky charge: taps fire as usual, but letting go of a button held for at
// least kStickAfter keeps it held (the beam keeps charging) until the next
// press, which releases it and fires the charged shot.
class Sticky {
public:
  static constexpr float kStickAfter = 0.35f;

  Output Update(bool active, bool held, bool pressed, float dt) {
    if (!active) {
      Reset();
      return {held, pressed};
    }
    if (mSwallow) {
      if (held && !pressed) {
        return {false, false};
      }
      mSwallow = false;
    }
    if (mLatched) {
      if (pressed) {
        mLatched = false;
        mSwallow = true;
        mHeldTime = 0.f;
        return {false, false};
      }
      return {true, false};
    }
    if (held) {
      mHeldTime = pressed ? 0.f : mHeldTime + dt;
      return {true, pressed};
    }
    if (mHeldTime >= kStickAfter) {
      mLatched = true;
      mHeldTime = 0.f;
      return {true, false};
    }
    mHeldTime = 0.f;
    return {false, false};
  }
  void Reset() {
    mLatched = false;
    mSwallow = false;
    mHeldTime = 0.f;
  }
  bool Latched() const { return mLatched; }

private:
  bool mLatched = false;
  bool mSwallow = false;
  float mHeldTime = 0.f;
};

} // namespace PortHoldToggle
