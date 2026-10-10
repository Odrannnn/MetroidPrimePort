#include "port_input_remap_ui.h"

#include "port_controls.h"
#include "port_input_bindings.h"
#include "port_input_devices.h"

#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace PortInputRemap {
namespace {

using PortInput::Action;
using PortInput::Binding;
using PortInput::Device;
using PortInput::Input;
using PortInput::Trigger;
using PortInput::UserBindings;
using PortInput::UserProfile;

constexpr uint64_t kCaptureTimeoutMs = 8000;
constexpr uint64_t kCaptureStaleMs = 250; // the page wasn't drawn for this long
constexpr float kAxisCapture = 0.6f;

struct STriggerInfo {
  Trigger trigger;
  const char* label;
};
constexpr STriggerInfo kTriggers[] = {
    {Trigger::Press, "Press (while held)"},
    {Trigger::Tap, "Tap"},
    {Trigger::Hold, "Hold"},
    {Trigger::DoubleTap, "Double tap"},
    {Trigger::Toggle, "Toggle"},
};

struct SContextInfo {
  uint16_t bit;
  const char* label;
};
constexpr SContextInfo kContexts[] = {
    {PortInput::kCtxGameplay, "Gameplay"}, {PortInput::kCtxMorphBall, "Morph Ball"},
    {PortInput::kCtxMap, "Map"},           {PortInput::kCtxMenu, "Menus"},
    {PortInput::kCtxScan, "Scan Visor"},   {PortInput::kCtxLayer1, "Layer 1"},
    {PortInput::kCtxLayer2, "Layer 2"},    {PortInput::kCtxLayer3, "Layer 3"},
    {PortInput::kCtxLayer4, "Layer 4"},
};

constexpr const char* kFamilies[] = {"Keyboard & mouse", "Controller", "Touch"};

// The touch controls Record can't reach (the overlay hides while F1 is open), as
// "touch:" names and the editor's labels. The sticks, D-pad and menu button
// aren't remappable, so they're left out.
struct STouchInfo {
  const char* name;
  const char* label;
};
constexpr STouchInfo kTouchControls[] = {
    {"a", "A"},
    {"b", "B"},
    {"x", "X"},
    {"y", "Y"},
    {"l", "L"},
    {"r", "R"},
    {"z", "Z"},
    {"visor", "Visor"},
    {"beam", "Beam"},
    {"start", "Start"},
    {"map", "Map"},
    {"eye", "Hide controls"},
    {"turbo", "Turbo"},
    {"jump", "Twin stick: Jump"},
    {"fire", "Twin stick: Fire"},
    {"morph", "Twin stick: Morph"},
    {"missile", "Twin stick: Missile"},
    {"lt", "Twin stick: LT Lock"},
    {"lb", "Twin stick: LB Jump"},
    {"rt", "Twin stick: RT Fire"},
    {"rb", "Twin stick: RB Missile"},
    {"tz", "Twin stick: Map (Z)"},
    {"tturbo", "Twin stick: Turbo"},
    {"beam_shift", "Twin stick: Beam (held)"},
    {"map_tap", "Minimap tap"},
};

UserBindings sWork;     // what the page edits; written through on every change
bool sLoaded = false;
uint64_t sSeenVersion = 0; // the bindings version sWork matches
// The device being edited: "kbd", "touch", "pads", or a controller profile's match
// ("type:..." / "guid:..."). The first three are the Default profile (match "")
// restricted to one family; the others are that match's profile.
std::string sDevice = "kbd";
constexpr int kEditNone = -1;
constexpr int kEditNew = -2;
constexpr int kEditInherited = -3; // sEditOrig, a built-in binding, is being changed
int sEditing = kEditNone;          // else a binding index in the edited profile
Binding sEditOrig;
Binding sDraft;
char sInputsText[256] = {};
std::string sError;
char sFilter[64] = {};
bool sOnlyBound = false;

struct SCapture {
  bool active = false;
  bool armed = false; // everything was released once since the start
  uint64_t startMs = 0;
  std::vector<Input> chord;
} sCapture;
// sCapture.active and the last frame the page was drawn, for Capturing() on the
// game thread.
std::atomic<bool> sCaptureActive{false};
std::atomic<uint64_t> sLastDrawMs{0};

void EndCapture() {
  sCapture.active = false;
  sCaptureActive.store(false, std::memory_order_relaxed);
}

const char* TriggerLabel(Trigger t) {
  for (const STriggerInfo& info : kTriggers) {
    if (info.trigger == t) return info.label;
  }
  return "?";
}

std::string InputsToText(const Binding& b) {
  std::string out;
  for (int i = 0; i < b.count; ++i) {
    if (i != 0) out += " + ";
    out += PortInput::InputToText(b.inputs[size_t(i)]);
  }
  return out;
}

// Whether an input ("kind:") starts at s[i]: a known kind at the start or after a
// space, ',' or '+'. Key names hold spaces, ',' and '+' ("Keypad +"), so inputs are
// split where the next one starts rather than at a separator character.
bool InputStartsAt(const std::string& s, size_t i) {
  static constexpr const char* kKinds[] = {"key:",  "mouse:", "wheel:",     "motion:", "pad:",
                                           "axis:", "touch:", "touchaxis:", "gyro:"};
  if (i > 0 && s[i - 1] != ' ' && s[i - 1] != ',' && s[i - 1] != '+') return false;
  for (const char* kind : kKinds) {
    if (s.compare(i, std::strlen(kind), kind) == 0) return true;
  }
  return false;
}

std::string TrimSpaces(std::string s) {
  s.erase(0, s.find_first_not_of(" \t"));
  s.erase(s.find_last_not_of(" \t") + 1);
  return s;
}

// "a + b + c" (or comma separated) into a binding's inputs.
bool InputsFromText(const char* text, Binding& b) {
  Binding parsed = b;
  parsed.count = 0;
  const std::string s(text);
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size(); ++i) {
    if (InputStartsAt(s, i)) starts.push_back(i);
  }
  if (starts.empty() || !TrimSpaces(s.substr(0, starts[0])).empty()) return false;
  for (size_t n = 0; n < starts.size(); ++n) {
    const bool last = n + 1 == starts.size();
    std::string part = TrimSpaces(s.substr(starts[n], last ? std::string::npos : starts[n + 1] - starts[n]));
    // Before another input, drop one separator: a ',' or a '+' after a space (a
    // '+' right after the name is a directed axis's sign, as in axis:leftx+).
    if (!last && !part.empty() &&
        (part.back() == ',' || (part.back() == '+' && part.size() > 1 && part[part.size() - 2] == ' '))) {
      part = TrimSpaces(part.substr(0, part.size() - 1));
    }
    Input in;
    if (parsed.count >= PortInput::kMaxChord || !PortInput::InputFromText(part, in)) return false;
    parsed.inputs[parsed.count++] = in;
  }
  b = parsed;
  return true;
}

void Commit() {
  if (!PortInputDevices::SetUserBindings(sWork)) {
    sError = "Couldn't write controls.toml.";
  } else {
    sError.clear();
    sSeenVersion = PortInputDevices::UserBindingsVersion();
  }
}

// A user profile by match, or null (reading never creates one).
UserProfile* Find(const std::string& match) {
  for (UserProfile& p : sWork.profiles) {
    if (p.match == match) return &p;
  }
  return nullptr;
}

// The profile for a match, made on first edit. The Default one comes first.
UserProfile& Ensure(const std::string& match, const std::string& name) {
  if (UserProfile* p = Find(match)) return *p;
  UserProfile p;
  p.name = match.empty() ? std::string("Default") : name;
  p.match = match;
  sWork.profiles.push_back(p);
  return sWork.profiles.back();
}

// What the selected device list entry edits.
struct SView {
  PortInput::Family family = PortInput::Family::Keyboard;
  std::string match; // the profile's match: "" for the three general entries
  std::string name;  // for a profile made on first edit
  PortInput::Profile inherited; // what the action falls back to
};

SView MakeView() {
  SView v;
  if (sDevice == "touch") v.family = PortInput::Family::Touch;
  else if (sDevice == "kbd") v.family = PortInput::Family::Keyboard;
  else v.family = PortInput::Family::Pad;
  if (sDevice != "kbd" && sDevice != "touch" && sDevice != "pads") {
    v.match = sDevice;
    if (const UserProfile* p = Find(sDevice)) v.name = p->name;
  }
  PortInputDevices::BuildBaseProfile(v.inherited);
  // A kind or model sits on top of the Default profile's own overrides.
  if (!v.match.empty()) {
    if (const UserProfile* def = Find("")) PortInput::Overlay(v.inherited, *def);
  }
  return v;
}

UserProfile& EditedProfile(const SView& v) { return Ensure(v.match, v.name); }

struct SDeviceEntry {
  std::string key;
  std::string label;
};

std::vector<SDeviceEntry> DeviceEntries() {
  std::vector<SDeviceEntry> out = {
      {"kbd", "Keyboard & mouse"}, {"touch", "Touch"}, {"pads", "All controllers"}};
  const auto listed = [&](const std::string& key) {
    return std::any_of(out.begin(), out.end(), [&](const SDeviceEntry& e) { return e.key == key; });
  };
  for (const PortInputDevices::SPadInfo& pad : PortInputDevices::ConnectedPads()) {
    if (!pad.type.empty() && !listed("type:" + pad.type)) out.push_back({"type:" + pad.type, "All " + pad.type + " controllers"});
    if (!pad.guid.empty() && !listed("guid:" + pad.guid)) out.push_back({"guid:" + pad.guid, pad.name + " (this model)"});
  }
  for (const UserProfile& p : sWork.profiles) {
    if (p.match.empty() || listed(p.match)) continue;
    out.push_back({p.match, (p.name.empty() ? p.match : p.name) + " (not connected)"});
  }
  return out;
}

// The inputs down now, in a fixed order (keys, mouse, pad buttons, axes).
// Only the viewed device's family is read: keys and mouse for the keyboard view,
// pad buttons and axes for a controller view, nothing for touch.
std::vector<Input> DownInputs(const PortInput::RawState& raw, PortInput::Family family) {
  std::vector<Input> down;
  if (family == PortInput::Family::Keyboard) {
    for (int k = 0; k < PortInput::kKeyCount; ++k) {
      if (raw.keys.test(size_t(k)) && k != SDL_SCANCODE_ESCAPE) down.push_back({Device::Key, 0, 50, uint16_t(k)});
    }
    for (int m = 0; m < PortInput::kMouseButtonCount; ++m) {
      if ((raw.mouseButtons >> m) & 1u) down.push_back({Device::MouseButton, 0, 50, uint16_t(m)});
    }
  } else if (family == PortInput::Family::Pad) {
    for (int b = 0; b < PortInput::kPadButtonCount; ++b) {
      if ((raw.padButtons >> b) & 1u) down.push_back({Device::PadButton, 0, 50, uint16_t(b)});
    }
    for (int a = 0; a < PortInput::kPadAxisCount; ++a) {
      if (std::fabs(raw.padAxes[a]) >= kAxisCapture) {
        down.push_back({Device::PadAxis, int8_t(raw.padAxes[a] > 0.f ? 1 : -1), 50, uint16_t(a)});
      }
    }
  }
  return down;
}

void StartCapture() {
  sCapture = SCapture{};
  sCapture.active = true;
  sCapture.startMs = SDL_GetTicks();
  sCaptureActive.store(true, std::memory_order_relaxed);
}

void FinishCapture() {
  sDraft.count = 0;
  for (const Input& in : sCapture.chord) {
    if (sDraft.count < PortInput::kMaxChord) sDraft.inputs[sDraft.count++] = in;
  }
  std::snprintf(sInputsText, sizeof(sInputsText), "%s", InputsToText(sDraft).c_str());
  EndCapture();
}

// Records every input pressed until all are released again: one input, or a
// chord in press order (the last one is the trigger). Esc cancels.
void UpdateCapture(PortInput::Family family) {
  if (!sCapture.active) return;
  PortInput::RawState raw;
  PortInputDevices::ReadRaw(raw);
  if (raw.keys.test(SDL_SCANCODE_ESCAPE) || SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs) {
    EndCapture();
    return;
  }
  std::vector<Input> down = DownInputs(raw, family);
  const ImGuiIO& io = ImGui::GetIO();
  if (sCapture.armed && family == PortInput::Family::Keyboard && (io.MouseWheel != 0.f || io.MouseWheelH != 0.f)) {
    const bool vertical = io.MouseWheel != 0.f;
    const float v = vertical ? io.MouseWheel : io.MouseWheelH;
    sCapture.chord.push_back({Device::MouseWheel, int8_t(v > 0.f ? 1 : -1), 50, uint16_t(vertical ? 0 : 1)});
    FinishCapture();
    return;
  }
  if (!sCapture.armed) {
    sCapture.armed = down.empty();
    return;
  }
  for (const Input& in : down) {
    if (std::find(sCapture.chord.begin(), sCapture.chord.end(), in) == sCapture.chord.end() &&
        sCapture.chord.size() < size_t(PortInput::kMaxChord)) {
      sCapture.chord.push_back(in);
    }
  }
  if (!sCapture.chord.empty() && down.empty()) FinishCapture();
}

void BeginEdit(int index, const Binding& b) {
  sEditing = index;
  sDraft = b;
  std::snprintf(sInputsText, sizeof(sInputsText), "%s", InputsToText(b).c_str());
  EndCapture();
}

bool ActionCombo(const char* label, Action& action) {
  bool changed = false;
  if (ImGui::BeginCombo(label, std::string(PortInput::Info(action).label).c_str())) {
    for (int i = 1; i < PortInput::kActionCount; ++i) {
      const Action a = Action(i);
      const bool selected = a == action;
      if (ImGui::Selectable(std::string(PortInput::Info(a).label).c_str(), selected)) {
        action = a;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void DrawEditor(const SView& v) {
  using PortInput::Family;
  ImGui::SeparatorText(sEditing == kEditNew ? "New binding" : "Edit binding");
  ActionCombo("Action", sDraft.action);

  if (sCapture.active) {
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), sCapture.armed ? "Press an input or a chord, then release it (Esc cancels)..."
                                                                     : "Release everything...");
  } else if (v.family != Family::Touch) {
    if (ImGui::Button("Record")) StartCapture();
    ImGui::SetItemTooltip(v.family == Family::Keyboard
                              ? "Press one key or mouse button, or hold several in order (the last one is the "
                                "trigger), then release them all. Mouse wheel too. Esc cancels."
                              : "Press one controller button or move a stick, or hold several in order (the "
                                "last one is the trigger), then release them all. Esc cancels.");
    ImGui::SameLine();
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##inputs", "pad:leftshoulder + pad:a", sInputsText, sizeof(sInputsText));
  ImGui::SetItemTooltip("Inputs, joined with +: key:<name>, mouse:left, wheel:up, pad:<button>, "
                        "axis:<name>+/-, touch:<control> (a, fire, missile...), gyro:yaw+ ... (docs/NATIVE_PORT.md). "
                        "Append @<percent> for an axis threshold.");
  if (!sCapture.active) {
    if (v.family == Family::Touch) {
      ImGui::SetNextItemWidth(260.f);
      if (ImGui::BeginCombo("##touch", "Add a touch control...")) {
        for (const STouchInfo& t : kTouchControls) {
          if (ImGui::Selectable(t.label)) {
            std::string text = TrimSpaces(sInputsText);
            if (!text.empty()) text += " + ";
            text += std::string("touch:") + t.name;
            std::snprintf(sInputsText, sizeof(sInputsText), "%s", text.c_str());
          }
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("The touch controls hide while this menu is open, so they can't be recorded: "
                            "pick one here. Picking another makes a chord.");
      ImGui::SameLine();
    }
    if (ImGui::Button("Clear")) sInputsText[0] = '\0';
  }

  Binding parsed = sDraft;
  const bool inputsOk = InputsFromText(sInputsText, parsed);
  if (!inputsOk && sInputsText[0] != '\0') ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Unknown input text.");
  if (parsed.count > 1) {
    ImGui::Checkbox("Any order", &sDraft.anyOrder);
    ImGui::SetItemTooltip("Off: hold the first inputs, then press the last one. On: press them all at "
                          "once, in any order.");
  }

  int trigger = 0;
  for (int i = 0; i < int(std::size(kTriggers)); ++i) {
    if (kTriggers[i].trigger == sDraft.trigger) trigger = i;
  }
  if (ImGui::BeginCombo("Trigger", kTriggers[trigger].label)) {
    for (int i = 0; i < int(std::size(kTriggers)); ++i) {
      if (ImGui::Selectable(kTriggers[i].label, i == trigger)) sDraft.trigger = kTriggers[i].trigger;
    }
    ImGui::EndCombo();
  }
  int ms = 0;
  switch (sDraft.trigger) {
  case Trigger::Tap:
    ms = sDraft.tapMs;
    if (ImGui::SliderInt("Tap shorter than (ms)", &ms, 50, 1000)) sDraft.tapMs = uint16_t(ms);
    break;
  case Trigger::Hold:
    ms = sDraft.holdMs;
    if (ImGui::SliderInt("Hold for (ms)", &ms, 50, 2000)) sDraft.holdMs = uint16_t(ms);
    break;
  case Trigger::DoubleTap:
    ms = sDraft.doubleMs;
    if (ImGui::SliderInt("Second press within (ms)", &ms, 50, 1000)) sDraft.doubleMs = uint16_t(ms);
    break;
  default: break;
  }
  int turbo = sDraft.turboHz;
  if (ImGui::SliderInt("Turbo (presses/s)", &turbo, 0, 30, turbo == 0 ? "Off" : "%d")) sDraft.turboHz = uint16_t(turbo);

  const PortInput::ActionKind kind = PortInput::Info(sDraft.action).kind;
  if (kind != PortInput::ActionKind::Digital) {
    ImGui::SliderFloat("Scale", &sDraft.scale, 0.1f, 4.f, "%.2f");
    ImGui::Checkbox("Invert", &sDraft.invert);
  }

  ImGui::TextUnformatted("Active in");
  bool everywhere = sDraft.contexts == PortInput::kCtxAll;
  ImGui::SameLine();
  if (ImGui::Checkbox("Everywhere", &everywhere)) {
    sDraft.contexts = everywhere ? PortInput::kCtxAll : PortInput::kCtxGameplay;
  }
  if (!everywhere) {
    for (size_t i = 0; i < std::size(kContexts); ++i) {
      bool on = (sDraft.contexts & kContexts[i].bit) != 0;
      if (i % 5 != 0) ImGui::SameLine();
      if (ImGui::Checkbox(kContexts[i].label, &on)) {
        sDraft.contexts = uint16_t(on ? sDraft.contexts | kContexts[i].bit : sDraft.contexts & ~kContexts[i].bit);
      }
    }
    ImGui::TextDisabled("A layer applies while its \"Layer n (hold)\" binding is held.");
  }

  const bool familyOk = inputsOk && PortInput::BindingFamily(parsed) == v.family;
  if (inputsOk && !familyOk) {
    ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "This page binds %s inputs only.", kFamilies[int(v.family)]);
  }
  const bool valid = familyOk && sDraft.action != Action::None && sDraft.contexts != 0;
  ImGui::BeginDisabled(!valid);
  if (ImGui::Button("Save")) {
    parsed.action = sDraft.action;
    parsed.anyOrder = parsed.count > 1 && sDraft.anyOrder;
    parsed.trigger = sDraft.trigger;
    parsed.tapMs = sDraft.tapMs;
    parsed.holdMs = sDraft.holdMs;
    parsed.doubleMs = sDraft.doubleMs;
    parsed.turboHz = sDraft.turboHz;
    parsed.scale = sDraft.scale;
    parsed.invert = sDraft.invert;
    parsed.contexts = sDraft.contexts;
    UserProfile& profile = EditedProfile(v);
    const Family family = v.family;
    if (sEditing == kEditNew) {
      // Keep the other built-in bindings of this action: a user binding drops them all.
      PortInput::Materialize(profile, parsed.action, family, v.inherited);
      PortInput::AddUserBinding(profile, parsed);
    } else if (sEditing == kEditInherited) {
      PortInput::Materialize(profile, sEditOrig.action, family, v.inherited);
      PortInput::Materialize(profile, parsed.action, family, v.inherited);
      const auto it = std::find(profile.bindings.begin(), profile.bindings.end(), sEditOrig);
      if (it != profile.bindings.end()) {
        const size_t index = size_t(it - profile.bindings.begin());
        PortInput::RemoveUserBinding(profile, index);
      }
      PortInput::AddUserBinding(profile, parsed);
    } else if (sEditing >= 0 && sEditing < int(profile.bindings.size())) {
      const Binding& old = profile.bindings[size_t(sEditing)];
      if (old.action == parsed.action && PortInput::BindingFamily(old) == family) {
        profile.bindings[size_t(sEditing)] = parsed;
      } else {
        PortInput::RemoveUserBinding(profile, size_t(sEditing));
        PortInput::Materialize(profile, parsed.action, family, v.inherited);
        PortInput::AddUserBinding(profile, parsed);
      }
    }
    sEditing = kEditNone;
    EndCapture();
    Commit();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    sEditing = kEditNone;
    EndCapture();
  }
}

void DrawBindingRow(const Binding& b, bool inherited = false) {
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(std::string(PortInput::Info(b.action).label).c_str());
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(InputsToText(b).c_str());
  if (b.IsChord() && b.anyOrder) {
    ImGui::SameLine();
    ImGui::TextDisabled("(any order)");
  }
  ImGui::TableNextColumn();
  if (b.turboHz != 0) {
    ImGui::Text("%s, turbo %d/s", TriggerLabel(b.trigger), int(b.turboHz));
  } else {
    ImGui::TextUnformatted(TriggerLabel(b.trigger));
  }
  ImGui::TableNextColumn();
  std::string where = b.contexts == PortInput::kCtxAll ? "everywhere" : PortInput::ContextsToText(b.contexts);
  if (inherited) where += " (inherited)";
  ImGui::TextUnformatted(where.c_str());
}

void DrawEffective() {
  if (!ImGui::TreeNode("Everything bound now")) return;
  ImGui::TextDisabled("The built-in controls with every profile in use on top.");
  PortInput::Profile p;
  PortInputDevices::BuildProfile(p);
  if (ImGui::BeginTable("effective", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    for (const Binding& b : p.bindings) {
      ImGui::TableNextRow();
      DrawBindingRow(b);
    }
    ImGui::EndTable();
  }
  ImGui::TreePop();
}

bool Contains(const std::string& text, const char* filter) {
  if (filter[0] == '\0') return true;
  const auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  return lower(text).find(lower(filter)) != std::string::npos;
}

// A change the table asks for; applied after it so the rows aren't edited mid-draw.
struct SPending {
  enum class Op { None, RemoveUser, RemoveInherited, Revert } op = Op::None;
  Action action = Action::None;
  size_t index = 0;
  Binding binding;
};

void ApplyPending(const SView& v, const SPending& pending) {
  if (pending.op == SPending::Op::None) return;
  UserProfile& profile = EditedProfile(v);
  switch (pending.op) {
  case SPending::Op::RemoveUser: PortInput::RemoveUserBinding(profile, pending.index); break;
  case SPending::Op::RemoveInherited: {
    PortInput::Materialize(profile, pending.action, v.family, v.inherited);
    const auto it = std::find(profile.bindings.begin(), profile.bindings.end(), pending.binding);
    if (it != profile.bindings.end()) PortInput::RemoveUserBinding(profile, size_t(it - profile.bindings.begin()));
    break;
  }
  case SPending::Op::Revert: PortInput::Revert(profile, pending.action, v.family); break;
  case SPending::Op::None: break;
  }
  sEditing = kEditNone;
  Commit();
}

void DrawTable(const SView& v) {
  ImGui::SetNextItemWidth(220.f);
  ImGui::InputTextWithHint("##filter", "Filter actions", sFilter, sizeof(sFilter));
  ImGui::SameLine();
  ImGui::Checkbox("Only bound", &sOnlyBound);

  const UserProfile* user = Find(v.match);
  SPending pending;
  if (ImGui::BeginTable("bindings", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Action");
    ImGui::TableSetupColumn("Inputs");
    ImGui::TableSetupColumn("Trigger");
    ImGui::TableSetupColumn("Where");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int i = 1; i < PortInput::kActionCount; ++i) {
      const Action action = Action(i);
      const std::string label(PortInput::Info(action).label);
      if (!Contains(label, sFilter)) continue;
      const bool overridden = user != nullptr && PortInput::Overridden(*user, action, v.family);
      // (binding, index into the user's bindings or -1 for an inherited one)
      std::vector<std::pair<Binding, int>> rows;
      if (overridden) {
        for (int n = 0; n < int(user->bindings.size()); ++n) {
          const Binding& b = user->bindings[size_t(n)];
          if (b.count > 0 && b.action == action && PortInput::BindingFamily(b) == v.family) rows.push_back({b, n});
        }
      } else {
        for (const Binding& b : PortInput::InFamily(v.inherited.bindings, action, v.family)) rows.push_back({b, -1});
      }
      if (rows.empty() && sOnlyBound) continue;

      ImGui::PushID(i);
      const auto buttons = [&](bool last) {
        if (last) {
          if (ImGui::SmallButton("+")) {
            BeginEdit(kEditNew, Binding{action});
          }
          ImGui::SetItemTooltip("Add a binding for this action.");
          if (overridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Revert")) {
              pending.op = SPending::Op::Revert;
              pending.action = action;
            }
            ImGui::SetItemTooltip("Back to what this device inherits.");
          }
        }
      };
      if (rows.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled(overridden ? "(unbound)" : "-");
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        buttons(true);
      }
      for (size_t r = 0; r < rows.size(); ++r) {
        const Binding& b = rows[r].first;
        const int userIndex = rows[r].second;
        ImGui::PushID(int(r));
        ImGui::TableNextRow();
        if (userIndex < 0) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        DrawBindingRow(b, userIndex < 0);
        if (userIndex < 0) ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Edit")) {
          BeginEdit(userIndex < 0 ? kEditInherited : userIndex, b);
          sEditOrig = b;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          if (userIndex < 0) {
            pending.op = SPending::Op::RemoveInherited;
            pending.action = action;
            pending.binding = b;
          } else {
            pending.op = SPending::Op::RemoveUser;
            pending.index = size_t(userIndex);
          }
        }
        ImGui::SetItemTooltip("Remove this binding.");
        ImGui::SameLine();
        buttons(r + 1 == rows.size());
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ApplyPending(v, pending);
}

struct SPreset {
  const char* name;
  const char* id;
};
constexpr SPreset kKeyPresets[] = {{"Classic", "classic"}, {"Mouse & keyboard", "mouse"}};
constexpr SPreset kPadPresets[] = {
    {"GameCube", "gamecube"}, {"Remastered", "remastered"}, {"Modern", "modern"}, {"Southpaw", "southpaw"}};

// The reset buttons, each behind a confirmation.
void DrawReset(const SView& v) {
  using PortInput::Family;
  static int sPreset = 0;
  if (ImGui::Button("Reset...")) {
    sPreset = 0;
    ImGui::OpenPopup("resetDevice");
  }
  if (!ImGui::BeginPopup("resetDevice")) return;
  const SPreset* presets = nullptr;
  int count = 0;
  if (sDevice == "kbd") {
    presets = kKeyPresets;
    count = int(std::size(kKeyPresets));
  } else if (sDevice == "pads") {
    presets = kPadPresets;
    count = int(std::size(kPadPresets));
  }
  const bool gcAdapter = PADIsGCAdapter(0) != 0;
  if (count > 0) {
    ImGui::TextUnformatted("Reset this page's bindings to a preset:");
    for (int i = 0; i < count; ++i) {
      const bool needsPad = sDevice == "pads" && (std::string_view(presets[i].id) == "remastered" ||
                                                  std::string_view(presets[i].id) == "modern");
      ImGui::BeginDisabled(needsPad && gcAdapter);
      ImGui::RadioButton(presets[i].name, &sPreset, i);
      ImGui::EndDisabled();
    }
    if (sPreset >= count || (sDevice == "pads" && gcAdapter && (sPreset == 1 || sPreset == 2))) sPreset = 0;
    ImGui::TextDisabled("Your own bindings here are removed. Other devices are left alone.");
  } else if (v.family == Family::Touch) {
    ImGui::TextUnformatted("Remove every touch binding of yours?");
  } else {
    ImGui::TextUnformatted("Remove this device's own bindings, so it uses All controllers again?");
  }
  if (ImGui::Button("Reset")) {
    sEditing = kEditNone;
    EndCapture();
    if (UserProfile* p = Find(v.match)) {
      if (v.match.empty()) {
        PortInput::ClearFamily(*p, v.family);
      } else {
        p->bindings.clear();
        p->unbind.clear();
      }
      Commit();
    }
    if (count > 0 && sDevice == "kbd") PortControls::ApplyKeyPresetNamed(presets[sPreset].id);
    if (count > 0 && sDevice == "pads") PortControls::ApplyPadPresetNamed(presets[sPreset].id);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void DrawProfileOptions(const SView& v) {
  const UserProfile* user = Find(v.match);
  const bool isProfile = !v.match.empty();
  if (isProfile && user != nullptr) {
    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", user->name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) EditedProfile(v).name = nameBuf;
    if (ImGui::IsItemDeactivatedAfterEdit()) Commit();
    bool replace = user->replace;
    if (ImGui::Checkbox("Replace the built-in controls", &replace)) {
      EditedProfile(v).replace = replace;
      Commit();
    }
    ImGui::SetItemTooltip("On: only this device's bindings apply (with All controllers'). Off: they are "
                          "added on top, replacing what the other bindings give the same action.");
  }
  int window = user != nullptr ? user->chordWindowMs : 0;
  if (ImGui::SliderInt("Chord window (ms)", &window, 0, 200, window == 0 ? "Default" : "%d")) {
    EditedProfile(v).chordWindowMs = uint16_t(window);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) Commit();
  ImGui::SetItemTooltip("How long an input that starts a chord waits for the rest before it acts alone.");
  if (isProfile && user != nullptr) {
    if (ImGui::Button("Delete this profile")) ImGui::OpenPopup("deleteProfile");
    if (ImGui::BeginPopup("deleteProfile")) {
      ImGui::Text("Delete %s and its %d bindings?", user->name.c_str(), int(user->bindings.size()));
      if (ImGui::Button("Delete")) {
        std::erase_if(sWork.profiles, [&](const UserProfile& p) { return p.match == v.match; });
        sDevice = "pads";
        sEditing = kEditNone;
        Commit();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
}

} // namespace

bool Capturing() {
  // The game thread asks too; a capture left behind when the page closed or the
  // tab changed stops counting once the page isn't drawn.
  if (!sCaptureActive.load(std::memory_order_relaxed)) return false;
  return SDL_GetTicks() - sLastDrawMs.load(std::memory_order_relaxed) < kCaptureStaleMs;
}

void Draw() {
  const uint64_t now = SDL_GetTicks();
  if (sCapture.active && now - sLastDrawMs.load(std::memory_order_relaxed) >= kCaptureStaleMs) EndCapture();
  sLastDrawMs.store(now, std::memory_order_relaxed);
  // Picks up a hand edit or a deleted file, unless the page is mid-edit.
  const uint64_t version = PortInputDevices::UserBindingsVersion();
  if (!sLoaded || (version != sSeenVersion && sEditing == kEditNone && !sCapture.active)) {
    sWork = *PortInputDevices::UserBindings();
    sSeenVersion = version;
    sLoaded = true;
  }

  ImGui::TextWrapped("Bind any action to a key, mouse button, controller input or touch control, or to a "
                     "chord of up to four of them, for each device. Greyed rows are what the device gets "
                     "without you; changing one makes it yours, and Revert gives it back. Saved in "
                     "controls.toml in the user folder.");
  if (!sError.empty()) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", sError.c_str());

  const std::vector<SDeviceEntry> entries = DeviceEntries();
  if (std::none_of(entries.begin(), entries.end(), [](const SDeviceEntry& e) { return e.key == sDevice; })) {
    sDevice = "kbd";
  }
  std::string current;
  for (const SDeviceEntry& e : entries) {
    if (e.key == sDevice) current = e.label;
  }
  if (ImGui::BeginCombo("Device", current.c_str())) {
    for (const SDeviceEntry& e : entries) {
      if (ImGui::Selectable((e.label + "##" + e.key).c_str(), e.key == sDevice) && e.key != sDevice) {
        sDevice = e.key;
        sEditing = kEditNone;
        EndCapture();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Keyboard & mouse, touch and All controllers apply to every device of that kind. "
                        "A controller kind or model is added on top of All controllers.");

  const SView view = MakeView();
  UpdateCapture(view.family);

  ImGui::SameLine();
  DrawReset(view);
  ImGui::Separator();

  if (sEditing != kEditNone) DrawEditor(view);
  DrawTable(view);
  ImGui::Separator();
  DrawProfileOptions(view);
  DrawEffective();
}

} // namespace PortInputRemap
