#include "port_input_remap_ui.h"

#include "port_input_bindings.h"
#include "port_input_devices.h"

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
int sProfile = 0;       // index into sWork.profiles
int sEditing = -1;      // binding index in the profile, or -2 for a new one
Binding sDraft;
char sInputsText[256] = {};
std::string sError;
int sUnbindAction = 1;
int sUnbindFamily = 1;

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

UserProfile& EnsureProfile() {
  if (sWork.profiles.empty()) {
    UserProfile def;
    def.name = "Default";
    sWork.profiles.push_back(def);
  }
  sProfile = std::clamp(sProfile, 0, int(sWork.profiles.size()) - 1);
  return sWork.profiles[size_t(sProfile)];
}

std::string ProfileLabel(const UserProfile& p) {
  if (p.match.empty()) return p.name.empty() ? "Default (all devices)" : p.name + " (all devices)";
  return (p.name.empty() ? std::string("Controller") : p.name) + " (" + p.match + ")";
}

// The inputs down now, in a fixed order (keys, mouse, pad buttons, axes).
std::vector<Input> DownInputs(const PortInput::RawState& raw) {
  std::vector<Input> down;
  for (int k = 0; k < PortInput::kKeyCount; ++k) {
    if (raw.keys.test(size_t(k)) && k != SDL_SCANCODE_ESCAPE) down.push_back({Device::Key, 0, 50, uint16_t(k)});
  }
  for (int m = 0; m < PortInput::kMouseButtonCount; ++m) {
    if ((raw.mouseButtons >> m) & 1u) down.push_back({Device::MouseButton, 0, 50, uint16_t(m)});
  }
  for (int b = 0; b < PortInput::kPadButtonCount; ++b) {
    if ((raw.padButtons >> b) & 1u) down.push_back({Device::PadButton, 0, 50, uint16_t(b)});
  }
  for (int a = 0; a < PortInput::kPadAxisCount; ++a) {
    if (std::fabs(raw.padAxes[a]) >= kAxisCapture) {
      down.push_back({Device::PadAxis, int8_t(raw.padAxes[a] > 0.f ? 1 : -1), 50, uint16_t(a)});
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
void UpdateCapture() {
  if (!sCapture.active) return;
  PortInput::RawState raw;
  PortInputDevices::ReadRaw(raw);
  if (raw.keys.test(SDL_SCANCODE_ESCAPE) || SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs) {
    EndCapture();
    return;
  }
  std::vector<Input> down = DownInputs(raw);
  const ImGuiIO& io = ImGui::GetIO();
  if (sCapture.armed && (io.MouseWheel != 0.f || io.MouseWheelH != 0.f)) {
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

void DrawEditor(UserProfile& profile) {
  ImGui::SeparatorText(sEditing == -2 ? "New binding" : "Edit binding");
  ActionCombo("Action", sDraft.action);

  if (sCapture.active) {
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), sCapture.armed ? "Press an input or a chord, then release it (Esc cancels)..."
                                                                     : "Release everything...");
  } else {
    if (ImGui::Button("Record")) StartCapture();
    ImGui::SetItemTooltip("Press one input, or hold several in order (the last one is the trigger), then "
                          "release them all. Mouse wheel too. Esc cancels.");
    ImGui::SameLine();
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##inputs", "pad:leftshoulder + pad:a", sInputsText, sizeof(sInputsText));
  ImGui::SetItemTooltip("Inputs, joined with +: key:<name>, mouse:left, wheel:up, pad:<button>, "
                        "axis:<name>+/-, touch:<control> (a, fire, missile...), gyro:yaw+ ... (docs/NATIVE_PORT.md). "
                        "Append @<percent> for an axis threshold.");
  if (!sCapture.active) {
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
    ImGui::SetItemTooltip("The touch controls hide while this menu is open, so Record can't see them: "
                          "pick one here. Picking another makes a chord.");
    ImGui::SameLine();
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

  const bool valid = inputsOk && sDraft.action != Action::None && sDraft.contexts != 0;
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
    if (sEditing >= 0 && sEditing < int(profile.bindings.size())) {
      profile.bindings[size_t(sEditing)] = parsed;
    } else {
      profile.bindings.push_back(parsed);
    }
    sEditing = -1;
    EndCapture();
    Commit();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    sEditing = -1;
    EndCapture();
  }
}

void DrawProfilePicker() {
  std::string guid, type, name;
  const bool hasPad = PortInputDevices::ActivePad(guid, type, name);
  if (hasPad) {
    ImGui::Text("Controller: %s (%s)", name.c_str(), type.c_str());
  } else {
    ImGui::TextDisabled("No controller connected.");
  }

  const std::string current = sWork.profiles.empty() ? std::string("Default (all devices)")
                                                     : ProfileLabel(sWork.profiles[size_t(sProfile)]);
  if (ImGui::BeginCombo("Profile", current.c_str())) {
    for (int i = 0; i < int(sWork.profiles.size()); ++i) {
      if (ImGui::Selectable((ProfileLabel(sWork.profiles[size_t(i)]) + "##" + std::to_string(i)).c_str(), i == sProfile)) {
        sProfile = i;
        sEditing = -1;
      }
    }
    ImGui::EndCombo();
  }
  if (hasPad) {
    const auto add = [&](const std::string& match, const char* label) {
      for (int i = 0; i < int(sWork.profiles.size()); ++i) {
        if (sWork.profiles[size_t(i)].match == match) {
          sProfile = i;
          return;
        }
      }
      EnsureProfile();
      UserProfile p;
      p.name = label;
      p.match = match;
      sWork.profiles.push_back(p);
      sProfile = int(sWork.profiles.size()) - 1;
      sEditing = -1;
      Commit();
    };
    if (!type.empty() && ImGui::Button("Profile for this kind of controller")) add("type:" + type, type.c_str());
    ImGui::SetItemTooltip("Applies to every %s controller, on top of the default profile.", type.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Profile for this controller only")) add("guid:" + guid, name.c_str());
    ImGui::SetItemTooltip("Applies only to this controller model (its SDL GUID), on top of the default "
                          "profile; wins over a profile for its kind.");
  }
  const PortInputDevices::SActiveProfiles active = PortInputDevices::ActiveUserProfiles();
  if (active.sel.pad != nullptr) ImGui::TextDisabled("In use: default + %s", ProfileLabel(*active.sel.pad).c_str());
}

void DrawProfileOptions(UserProfile& profile) {
  char nameBuf[64];
  std::snprintf(nameBuf, sizeof(nameBuf), "%s", profile.name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) profile.name = nameBuf;
  if (ImGui::IsItemDeactivatedAfterEdit()) Commit();
  if (ImGui::Checkbox("Replace the built-in controls", &profile.replace)) Commit();
  ImGui::SetItemTooltip("On: only this page's bindings apply (with the default profile's, for a "
                        "controller profile). Off: they are added on top of the Keyboard & mouse and "
                        "Controller pages, replacing what those bind to the same action on the same "
                        "kind of device.");
  int window = profile.chordWindowMs;
  if (ImGui::SliderInt("Chord window (ms)", &window, 0, 200, window == 0 ? "Default" : "%d")) {
    profile.chordWindowMs = uint16_t(window);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) Commit();
  ImGui::SetItemTooltip("How long an input that starts a chord waits for the rest before it acts alone.");
}

void DrawUnbinds(UserProfile& profile) {
  if (!ImGui::TreeNode("Removed built-in bindings")) return;
  ImGui::TextDisabled("Drops what the other pages bind to an action on one kind of device.");
  for (size_t i = 0; i < profile.unbind.size(); ++i) {
    const PortInput::Unbind& u = profile.unbind[i];
    ImGui::PushID(int(i));
    if (ImGui::SmallButton("x")) {
      profile.unbind.erase(profile.unbind.begin() + std::ptrdiff_t(i));
      Commit();
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    ImGui::Text("%s on %s", std::string(PortInput::Info(u.action).label).c_str(), kFamilies[int(u.family)]);
    ImGui::PopID();
  }
  Action a = Action(sUnbindAction);
  ImGui::SetNextItemWidth(200.f);
  if (ActionCombo("##unbindAction", a)) sUnbindAction = int(a);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.f);
  ImGui::Combo("##unbindFamily", &sUnbindFamily, kFamilies, int(std::size(kFamilies)));
  ImGui::SameLine();
  if (ImGui::Button("Remove")) {
    const PortInput::Unbind u{Action(sUnbindAction), PortInput::Family(sUnbindFamily)};
    if (std::find(profile.unbind.begin(), profile.unbind.end(), u) == profile.unbind.end()) {
      profile.unbind.push_back(u);
      Commit();
    }
  }
  ImGui::TreePop();
}

void DrawBindingRow(const Binding& b) {
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
  ImGui::TextUnformatted(b.contexts == PortInput::kCtxAll ? "everywhere" : PortInput::ContextsToText(b.contexts).c_str());
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
  if (!sLoaded || (version != sSeenVersion && sEditing == -1 && !sCapture.active)) {
    sWork = *PortInputDevices::UserBindings();
    sSeenVersion = version;
    sLoaded = true;
  }
  UpdateCapture();

  ImGui::TextWrapped("Bind any action to a key, mouse button, controller input or touch control, or to a "
                     "chord of up to four of them. These bindings go on top of the other pages and are "
                     "saved in controls.toml in the user folder. The Default profile holds keyboard, "
                     "mouse and touch bindings and applies to every controller; a controller profile "
                     "adds bindings for one kind or model of controller on top.");
  if (!sError.empty()) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", sError.c_str());
  DrawProfilePicker();

  if (sWork.profiles.empty()) {
    if (ImGui::Button("Add binding")) {
      EnsureProfile();
      BeginEdit(-2, Binding{Action::PadA});
    }
    if (sEditing != -1) DrawEditor(EnsureProfile());
    DrawEffective();
    return;
  }

  UserProfile& profile = EnsureProfile();
  ImGui::Separator();
  if (ImGui::BeginTable("bindings", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Action");
    ImGui::TableSetupColumn("Inputs");
    ImGui::TableSetupColumn("Trigger");
    ImGui::TableSetupColumn("Where");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    int remove = -1;
    for (int i = 0; i < int(profile.bindings.size()); ++i) {
      ImGui::PushID(i);
      ImGui::TableNextRow();
      DrawBindingRow(profile.bindings[size_t(i)]);
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Edit")) BeginEdit(i, profile.bindings[size_t(i)]);
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) {
      profile.bindings.erase(profile.bindings.begin() + remove);
      if (sEditing == remove) sEditing = -1;
      else if (sEditing > remove) --sEditing;
      Commit();
    }
  }
  if (profile.bindings.empty()) ImGui::TextDisabled("No bindings in this profile yet.");
  if (sEditing == -1 && ImGui::Button("Add binding")) BeginEdit(-2, Binding{Action::PadA});
  if (sEditing != -1) DrawEditor(profile);

  ImGui::Separator();
  DrawProfileOptions(profile);
  DrawUnbinds(profile);
  if (ImGui::Button("Delete this profile")) ImGui::OpenPopup("deleteProfile");
  if (ImGui::BeginPopup("deleteProfile")) {
    ImGui::Text("Delete %s and its %d bindings?", ProfileLabel(profile).c_str(), int(profile.bindings.size()));
    if (ImGui::Button("Delete")) {
      sWork.profiles.erase(sWork.profiles.begin() + sProfile);
      sProfile = 0;
      sEditing = -1;
      Commit();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  DrawEffective();
}

} // namespace PortInputRemap
