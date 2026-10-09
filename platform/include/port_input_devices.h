#pragma once

// The device layer between SDL/Aurora and the binding engine (port_input_engine.h):
// it reads port 0's controller, keyboard, mouse and touch overlay into a RawState,
// builds the engine profile from the saved controls (Aurora's controller and key
// mappings, the alt buttons, the beam shift, turbo and the mouse buttons), and turns
// the engine's output back into the PADStatus that PADSetPortOverride hands PADRead.

#include "port_input_engine.h"

#include <dolphin/pad.h>

namespace PortInputDevices {

// Mouse button codes (Device::MouseButton): each run holds SDL's five buttons
// (left, middle, right, X1, X2), read a different way.
constexpr int kMouseGameplay = 0; // under mouse aim, captured (PortDebug::MouseWeaponButtons)
constexpr int kMouseKeySlot = 8;  // a key slot holding a mouse button (SDL_GetMouseState, as Aurora)
constexpr int kMouseMenu = 16;    // the A/B buttons out of aim (PortDebug::MouseMenuButtons)
constexpr int kMouseHeld = 24;    // the beam shift / turbo key slots (PortDebug::MouseHeldButtons)

// Touch overlay codes (Device::Touch).
constexpr int kTouchBeamShift = 32;
constexpr int kTouchTurbo = 33;
constexpr int kTouchMapTap = 34; // one poll of Z after a minimap tap

struct SPoll {
  PADStatus status{};
  bool beamShift = false; // the beam shift binding is held
  bool mouseL = false;    // a gameplay mouse button presses L
};

// One poll of port 0: reads the devices, rebuilds the profile when the settings
// changed, runs the engine and synthesises the status. mouseHeld: the held mouse
// buttons (SDL mask order); focused: the window takes input.
SPoll Poll(unsigned mouseHeld, bool focused);

// The profile the saved controls describe (exposed for tests and the remap UI).
void BuildProfile(PortInput::Profile& out);
// The PAD status an engine output describes, as Aurora's own mapping would.
PADStatus Synthesize(const PortInput::Output& out);

} // namespace PortInputDevices
