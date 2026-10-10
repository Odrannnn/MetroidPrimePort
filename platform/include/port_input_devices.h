#pragma once

// The device layer between SDL/Aurora and the binding engine (port_input_engine.h):
// it reads port 0's controller, keyboard, mouse and touch overlay into a RawState,
// builds the engine profile from the saved controls (Aurora's controller and key
// mappings, the alt buttons, the beam shift, turbo and the mouse buttons), and turns
// the engine's output back into the PADStatus that PADSetPortOverride hands PADRead.

#include "port_input_bindings.h"
#include "port_input_engine.h"

#include <dolphin/pad.h>

#include <memory>
#include <string>
#include <vector>

namespace PortInputDevices {

// Mouse button codes (Device::MouseButton): each run holds SDL's five buttons
// (left, middle, right, X1, X2), read a different way.
constexpr int kMouseGameplay = 0; // under mouse aim, captured (PortDebug::MouseWeaponButtons)
constexpr int kMouseKeySlot = 8;  // a key slot holding a mouse button (SDL_GetMouseState, as Aurora)
constexpr int kMouseMenu = 16;    // the A/B buttons out of aim (PortDebug::MouseMenuButtons)
constexpr int kMouseHeld = 24;    // the beam shift / turbo key slots (PortDebug::MouseHeldButtons)

// Touch overlay codes (Device::Touch). 0..kTouchControlCount-1 are the overlay's
// controls, numbered as its layout editor (TouchControlsView's C_* ids).
constexpr int kTouchControlCount = 29;
constexpr int kTouchBeamShift = 32;
constexpr int kTouchTurbo = 33;
constexpr int kTouchMapTap = 34; // one poll of Z after a minimap tap

struct SPoll {
  PADStatus status{};
};

// One poll of port 0: reads the devices, rebuilds the profile when the settings
// changed, runs the engine and synthesises the status. mouseHeld: the held mouse
// buttons (SDL mask order); focused: the window takes input.
SPoll Poll(unsigned mouseHeld, bool focused);

// The user's bindings from <user>/controls.toml (port_input_bindings.h), loaded on
// first use and reloaded when the file changes (checked at most once a second). A
// parse error is logged and keeps the last good bindings. Thread-safe; the snapshot
// stays valid while held.
std::shared_ptr<const PortInput::UserBindings> UserBindings();
// Changes whenever the bindings do (a load, a reload or SetUserBindings).
uint64_t UserBindingsVersion();
// Writes controls.toml and, if that worked, applies the bindings from the next poll.
bool SetUserBindings(const PortInput::UserBindings& bindings);
// The user profiles that apply now: the base one and port 0's pad (GUID, then type),
// with the snapshot they point into.
struct SActiveProfiles {
  std::shared_ptr<const PortInput::UserBindings> bindings;
  PortInput::Selection sel;
};
SActiveProfiles ActiveUserProfiles();
// The touch overlay's controls (Android): whether one is held, and whether a user
// binding reads it (then the overlay sends only the touch code, not its pad button).
void SetTouchControl(int control, bool down);
bool TouchControlBound(int control);
// Port 0's pad: its SDL GUID (32 hex digits), gamepad type string and name; false
// (all empty) without one.
bool ActivePad(std::string& guid, std::string& type, std::string& name);
// The devices as they are, for an input capture: keys, port 0's pad (axes without
// deadzones) and SDL's mouse buttons (code kMouseKeySlot..).
void ReadRaw(PortInput::RawState& raw);

// A connected controller (virtual touch pads excluded): SDL GUID, gamepad type
// string and name.
struct SPadInfo {
  std::string guid;
  std::string type;
  std::string name;
};
std::vector<SPadInfo> ConnectedPads();
// The profile the saved controls describe without the user's bindings: what each
// action inherits on the Remap page.
void BuildBaseProfile(PortInput::Profile& out);

// The profile the saved controls describe, with the user's bindings on top
// (exposed for tests and the remap UI).
void BuildProfile(PortInput::Profile& out);
// The PAD status an engine output describes, as Aurora's own mapping would.
PADStatus Synthesize(const PortInput::Output& out);

} // namespace PortInputDevices
