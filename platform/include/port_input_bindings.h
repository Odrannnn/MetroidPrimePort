#pragma once

// The user's own bindings (<user>/controls.toml), layered over the profile the
// saved controls describe (PortInputDevices::BuildProfile). Plain text and data
// without SDL, so tests/port_input_bindings.cpp links it alone; the device layer
// installs SDL's key names with SetKeyNames.
//
// The file is a TOML subset: [[profile]] tables, each with [[profile.bind]]
// tables below it, string / integer / boolean / float values and arrays of
// strings. Comments (#) and blank lines are ignored; anything else the parser
// doesn't know is an error with its line number.
//
//   [[profile]]
//   name = "Default"
//   match = ""                     # "", "guid:<32 hex>" or "type:<SDL gamepad type>"
//   replace = false                # true: start from no bindings at all
//   chord_window_ms = 40           # absent: keep the base profile's
//   unbind = ["pad_z@pad"]         # action@family: drop the base's bindings
//
//   [[profile.bind]]
//   action = "beam_wave"           # an ActionInfo key
//   inputs = ["pad:leftshoulder", "pad:dpright"]
//   any_order = false
//   trigger = "press"              # press, tap, hold, double_tap, toggle
//   tap_ms = 200
//   hold_ms = 300
//   double_ms = 250
//   turbo_hz = 0
//   scale = 1.0
//   invert = false
//   contexts = ["gameplay"]        # absent: everywhere
//
// Only fields that differ from Binding's defaults are written.
//
// Input text (the threshold suffix "@<percent>" is written only when it isn't 50):
//   key:<name or scancode>          key:space, key:42
//   mouse:<button>                  left, middle, right, x1, x2: SDL's button state
//   mouse:aim:<button>              the captured gameplay buttons (code 0..)
//   mouse:menu:<button>             the menu A/B buttons out of aim (code 16..)
//   mouse:held:<button>             the beam shift / turbo slot read (code 24..)
//   wheel:up|down|left|right
//   motion:x|y  (signed)  motion:x+|x-|y+|y-
//   pad:<SDL gamepad button name>   a, b, x, y, back, guide, start, leftstick,
//                                   rightstick, leftshoulder, rightshoulder, dpup,
//                                   dpdown, dpleft, dpright, misc1, paddle1..4,
//                                   touchpad, misc2..misc6; or pad:<number>
//   axis:<name>[+|-]                leftx, lefty, rightx, righty, lefttrigger,
//                                   righttrigger; or axis:<number>
//   touch:<id>
//   touchaxis:<id>[+|-]
//   gyro:pitch|yaw|roll[+|-]
//
// Contexts: gameplay, morphball, map, menu, scan, layer1..layer4, all.
// Families (for unbind and the overlay rule): keyboard (keys, mouse), pad (pad
// buttons and axes, gyro), touch.
//
// Overlay rule: for each action a profile binds, the base bindings of that
// action whose trigger input (the last one) is in a family the profile binds
// it from are dropped, then the profile's bindings are appended. So rebinding
// the pad's Fire leaves the keyboard's Fire alone. unbind drops one action's
// base bindings in one family without adding any.
//
// Profile choice: the profile with match "" (the first one, if several) always
// applies; then the first profile matching the active pad's GUID, else the first
// matching its type, is layered on top of it.

#include "port_input_engine.h"

#include <string>
#include <string_view>
#include <vector>

namespace PortInput {

enum class Family : uint8_t { Keyboard, Pad, Touch };
Family FamilyOf(Device device);

struct Unbind {
  Action action = Action::None;
  Family family = Family::Keyboard;
  bool operator==(const Unbind&) const = default;
};

struct UserProfile {
  std::string name;
  std::string match; // "", "guid:<hex>", "type:<name>"
  bool replace = false;
  uint16_t chordWindowMs = 0; // 0: keep the base's
  std::vector<Unbind> unbind;
  std::vector<Binding> bindings;
  bool operator==(const UserProfile&) const = default;
};

struct UserBindings {
  std::vector<UserProfile> profiles;
  bool operator==(const UserBindings&) const = default;
};

// Key names: name(scancode) returns "" for none; parse(name) returns -1 for an
// unknown name. Without them (tests), keys are written and read as numbers.
using KeyNameFn = std::string (*)(int scancode);
using KeyParseFn = int (*)(std::string_view name);
void SetKeyNames(KeyNameFn name, KeyParseFn parse);

std::string InputToText(const Input& in);
bool InputFromText(std::string_view text, Input& out);
std::string ContextsToText(uint16_t contexts); // e.g. "gameplay,map"; "all" for kCtxAll
bool ContextFromText(std::string_view text, uint16_t& out);

// error (optional) gets "line N: what". On failure out is left unchanged.
bool ParseUserBindings(std::string_view text, UserBindings& out, std::string* error = nullptr);
std::string SerializeUserBindings(const UserBindings& bindings);

// The profiles that apply for a pad (guid: 32 lowercase hex digits, type: SDL's
// gamepad type string; either may be empty for no pad): the default first, then
// the pad's, each null when absent.
struct Selection {
  const UserProfile* base = nullptr;
  const UserProfile* pad = nullptr;
};
Selection Select(const UserBindings& bindings, std::string_view guid, std::string_view type);

// Applies one user profile over a profile (the overlay rule above).
void Overlay(Profile& profile, const UserProfile& user);

} // namespace PortInput
