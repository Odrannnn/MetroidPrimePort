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
// What the Controller column edits: "pads" (the Default profile's controller
// bindings) or a controller profile's match ("type:..." / "guid:..."). Keyboard
// and touch always edit the Default profile.
std::string sPadDevice = "pads";
#ifdef __ANDROID__
constexpr bool kShowTouch = true;
#else
constexpr bool kShowTouch = false; // the touch overlay is Android's
#endif
constexpr int kEditNone = -1;
constexpr int kEditNew = -2;
constexpr int kEditInherited = -3; // sEditOrig, a built-in binding, is being changed
int sEditing = kEditNone;          // else a binding index in sEditFamily's profile
PortInput::Family sEditFamily = PortInput::Family::Keyboard; // where the edited binding is
bool sQuickAdd = false; // "+": the next recorded input is saved at once
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

// What one family's column edits.
struct SView {
  PortInput::Family family = PortInput::Family::Keyboard;
  std::string match; // the profile's match: "" for the Default profile
  std::string name;  // for a profile made on first edit
  PortInput::Profile inherited; // what the action falls back to
};

SView MakeView(PortInput::Family family) {
  SView v;
  v.family = family;
  if (family == PortInput::Family::Pad && sPadDevice != "pads") {
    v.match = sPadDevice;
    if (const UserProfile* p = Find(sPadDevice)) v.name = p->name;
  }
  PortInputDevices::BuildBaseProfile(v.inherited);
  // A kind or model sits on top of the Default profile's own overrides.
  if (!v.match.empty()) {
    if (const UserProfile* def = Find("")) PortInput::Overlay(v.inherited, *def);
  }
  return v;
}

UserProfile& EditedProfile(const SView& v) { return Ensure(v.match, v.name); }

// The three columns' views, indexed by Family.
struct SViews {
  SView v[3];
  const SView& operator[](PortInput::Family f) const { return v[int(f)]; }
};

SViews MakeViews() {
  using PortInput::Family;
  return {{MakeView(Family::Keyboard), MakeView(Family::Pad), MakeView(Family::Touch)}};
}

struct SDeviceEntry {
  std::string key;
  std::string label;
};

// What the Controller column can edit.
std::vector<SDeviceEntry> PadEntries() {
  std::vector<SDeviceEntry> out = {{"pads", "All controllers"}};
  const auto listed = [&](const std::string& key) {
    return std::any_of(out.begin(), out.end(), [&](const SDeviceEntry& e) { return e.key == key; });
  };
  for (const PortInputDevices::SPadInfo& pad : PortInputDevices::ConnectedPads()) {
    const std::string type = "type:" + pad.type;
    const std::string guid = "guid:" + pad.guid;
    if (!pad.type.empty() && !listed(type)) {
      out.push_back({type, "Only " + pad.type + " controllers"});
    }
    if (!pad.guid.empty() && !listed(guid)) {
      out.push_back({guid, "Only " + pad.name + " (this model)"});
    }
  }
  for (const UserProfile& p : sWork.profiles) {
    if (p.match.empty() || listed(p.match)) continue;
    out.push_back({p.match, (p.name.empty() ? p.match : p.name) + " (not connected)"});
  }
  return out;
}

// The inputs down now, in a fixed order (keys, mouse, pad buttons, axes).
// Touch can't be read: the overlay hides while F1 is open.
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

// Whether every input of a binding comes from one family (keyboard and mouse are one).
bool OneFamily(const Binding& b) {
  for (int i = 1; i < b.count; ++i) {
    if (PortInput::FamilyOf(b.inputs[size_t(i)].device) != PortInput::FamilyOf(b.inputs[0].device)) return false;
  }
  return true;
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
// chord in press order (the last one is the trigger). The first input picks the
// device: a chord's other inputs come from the same one. Esc cancels.
void UpdateCapture() {
  using PortInput::Family;
  if (!sCapture.active) return;
  PortInput::RawState raw;
  PortInputDevices::ReadRaw(raw);
  if (raw.keys.test(SDL_SCANCODE_ESCAPE) || SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs) {
    EndCapture();
    return;
  }
  std::vector<Input> down = DownInputs(raw);
  const bool started = !sCapture.chord.empty();
  const Family family = started ? PortInput::FamilyOf(sCapture.chord[0].device) : Family::Keyboard;
  const ImGuiIO& io = ImGui::GetIO();
  if (sCapture.armed && (!started || family == Family::Keyboard) && (io.MouseWheel != 0.f || io.MouseWheelH != 0.f)) {
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
    if (!sCapture.chord.empty() && PortInput::FamilyOf(in.device) != PortInput::FamilyOf(sCapture.chord[0].device)) {
      continue;
    }
    if (std::find(sCapture.chord.begin(), sCapture.chord.end(), in) == sCapture.chord.end() &&
        sCapture.chord.size() < size_t(PortInput::kMaxChord)) {
      sCapture.chord.push_back(in);
    }
  }
  if (!sCapture.chord.empty() && down.empty()) FinishCapture();
}

bool sAdvancedSync = false; // set the editor's Advanced node from the draft on the next draw
// The editor is drawn under the row it edits (a new binding: under its action's
// last row). When that row isn't on screen (filtered, section closed), it goes
// above the table instead.
Action sEditAction = Action::None;
bool sEditRowShown = true; // the last table draw reached the edited row

// Whether a binding uses anything the editor keeps under Advanced.
bool UsesAdvanced(const Binding& b) {
  return b.trigger != Trigger::Press || b.turboHz != 0 || b.contexts != PortInput::kCtxAll || (b.IsChord() && b.anyOrder) ||
         b.scale != 1.f || b.invert;
}

void BeginEdit(int index, const Binding& b, PortInput::Family family) {
  sEditing = index;
  sEditFamily = family;
  sQuickAdd = false;
  sEditAction = b.action;
  sEditRowShown = true;
  sDraft = b;
  sAdvancedSync = true;
  std::snprintf(sInputsText, sizeof(sInputsText), "%s", InputsToText(b).c_str());
  EndCapture();
}

// Inputs are shown as a device icon and a name, not as their "kind:" text.
enum class Icon { Keyboard, Mouse, Pad, Touch };

Icon IconOf(Device d) {
  switch (d) {
  case Device::MouseButton:
  case Device::MouseWheel:
  case Device::MouseMotion: return Icon::Mouse;
  case Device::PadButton:
  case Device::PadAxis:
  case Device::Gyro: return Icon::Pad;
  case Device::Touch:
  case Device::TouchAxis: return Icon::Touch;
  default: return Icon::Keyboard;
  }
}

const char* IconName(Icon icon) {
  switch (icon) {
  case Icon::Mouse: return "mouse";
  case Icon::Pad: return "controller";
  case Icon::Touch: return "touch";
  default: return "keyboard";
  }
}

// An icon in the s x s square at p.
void DrawIcon(ImDrawList* dl, ImVec2 p, float s, Icon icon, ImU32 col) {
  const float t = std::max(1.f, s * 0.09f);
  const auto at = [&](float x, float y) { return ImVec2(p.x + s * x, p.y + s * y); };
  switch (icon) {
  case Icon::Keyboard: // a box with two rows of keys and a space bar
    dl->AddRect(at(0.f, 0.2f), at(1.f, 0.82f), col, s * 0.12f, 0, t);
    for (int row = 0; row < 2; ++row) {
      for (int i = 0; i < 4; ++i) {
        const ImVec2 c = at(0.2f + 0.2f * float(i), 0.37f + 0.15f * float(row));
        const float k = s * 0.055f;
        dl->AddRectFilled(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col);
      }
    }
    dl->AddLine(at(0.3f, 0.68f), at(0.7f, 0.68f), col, t);
    break;
  case Icon::Mouse: // a capsule split into two buttons
    dl->AddRect(at(0.22f, 0.04f), at(0.78f, 0.96f), col, s * 0.28f, 0, t);
    dl->AddLine(at(0.22f, 0.42f), at(0.78f, 0.42f), col, t);
    dl->AddLine(at(0.5f, 0.04f), at(0.5f, 0.42f), col, t);
    break;
  case Icon::Pad: // a pad body with a D-pad and two buttons
    dl->AddRect(at(0.f, 0.24f), at(1.f, 0.8f), col, s * 0.24f, 0, t);
    dl->AddLine(at(0.17f, 0.52f), at(0.41f, 0.52f), col, t);
    dl->AddLine(at(0.29f, 0.4f), at(0.29f, 0.64f), col, t);
    dl->AddCircleFilled(at(0.66f, 0.46f), s * 0.07f, col);
    dl->AddCircleFilled(at(0.8f, 0.58f), s * 0.07f, col);
    break;
  case Icon::Touch: // a touch point with a ripple
    dl->AddCircleFilled(at(0.5f, 0.5f), s * 0.14f, col);
    dl->AddCircle(at(0.5f, 0.5f), s * 0.3f, col, 0, t);
    dl->AddCircle(at(0.5f, 0.5f), s * 0.46f, col, 0, t * 0.7f);
    break;
  }
}

// An input's name without its "kind:" (the icon says it).
// Pad buttons by raw bit (SDL_GamepadButton, then the triggers' pulls and clicks).
constexpr const char* kPadButtonLabels[] = {
    "A",       "B",        "X",        "Y",        "Back",     "Guide",    "Start",     "Left stick",
    "Right stick", "LB",   "RB",       "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share",
    "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad", "Misc 2",   "Misc 3",    "Misc 4",
    "Misc 5",  "Misc 6",   "LT",       "RT",       "LT click", "RT click",
};

// An axis or half of one, as on the controller.
std::string PadAxisName(const Input& in) {
  static constexpr const char* kSticks[] = {"Left stick", "Left stick", "Right stick", "Right stick"};
  if (in.code < 4) {
    const bool x = in.code % 2 == 0;
    const char* way = in.dir == 0 ? (x ? " X" : " Y") : x ? (in.dir > 0 ? " right" : " left")
                                                          : (in.dir > 0 ? " down" : " up");
    return std::string(kSticks[in.code]) + way;
  }
  if (in.code == 4) return "LT";
  if (in.code == 5) return "RT";
  return "Axis " + std::to_string(in.code);
}

// Names without the text form's kind prefix, as the device labels them.
std::string InputName(const Input& in) {
  const std::string text = PortInput::InputToText(in);
  const size_t at = text.find('@');
  const std::string threshold = at == std::string::npos ? std::string() : " " + text.substr(at + 1) + "%";
  if (in.device == Device::PadButton && in.code < std::size(kPadButtonLabels)) {
    return kPadButtonLabels[in.code] + threshold;
  }
  if (in.device == Device::PadAxis) return PadAxisName(in) + threshold;
  const size_t colon = text.find(':');
  std::string name = colon == std::string::npos ? text : text.substr(colon + 1);
  switch (in.device) {
  case Device::MouseButton: {
    // aim:/menu:/held: pick when the click counts.
    static constexpr std::pair<const char*, const char*> kWhen[] = {
        {"aim:", " (aiming)"}, {"menu:", " (menus)"}, {"held:", " (held)"}};
    for (const auto& [prefix, suffix] : kWhen) {
      if (name.starts_with(prefix)) name = name.substr(std::strlen(prefix)) + suffix;
    }
    break;
  }
  case Device::MouseWheel: name = "wheel " + name; break;
  case Device::MouseMotion: name = "move " + name; break;
  case Device::Gyro: name = "gyro " + name; break;
  case Device::TouchAxis: name = "stick " + name; break;
  case Device::Touch:
    for (const STouchInfo& t : kTouchControls) {
      if (name == t.name) return t.label;
    }
    break;
  default: break;
  }
  if (!name.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// The parts of a chip: icon + name per input, " + " between them.
struct SChipLayout {
  std::vector<std::string> names;
  float width = 0.f;
};

SChipLayout ChipLayout(const Binding& b) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float icon = ImGui::GetTextLineHeight();
  const float gap = style.ItemInnerSpacing.x * 0.5f;
  SChipLayout out;
  out.width = style.FramePadding.x * 2.f;
  for (int i = 0; i < b.count; ++i) {
    if (i != 0) out.width += ImGui::CalcTextSize(" + ").x;
    out.names.push_back(InputName(b.inputs[size_t(i)]));
    out.width += icon + gap + ImGui::CalcTextSize(out.names.back().c_str()).x;
  }
  return out;
}

// A binding as a button (or just drawn, when !interactive). dim: a default the
// user hasn't made theirs. Returns whether it was clicked.
bool Chip(const char* id, const Binding& b, bool dim, bool selected, bool interactive = true) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const SChipLayout layout = ChipLayout(b);
  const float icon = ImGui::GetTextLineHeight();
  // Frame height, so the text lines up with buttons and text on the same line.
  const ImVec2 size(layout.width, ImGui::GetFrameHeight());
  const ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = false;
  if (interactive) {
    clicked = ImGui::InvisibleButton(id, size);
  } else {
    ImGui::Dummy(size);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (interactive) {
    const ImGuiCol bg = selected || ImGui::IsItemActive() ? ImGuiCol_ButtonActive
                        : ImGui::IsItemHovered()          ? ImGuiCol_ButtonHovered
                                                          : ImGuiCol_Button;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(bg, dim && !selected ? 0.45f : 1.f),
                      style.FrameRounding);
  }
  const ImU32 col = ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text);
  const float gap = style.ItemInnerSpacing.x * 0.5f;
  float x = p.x + (interactive ? style.FramePadding.x : 0.f);
  const float y = p.y + style.FramePadding.y;
  for (int i = 0; i < b.count; ++i) {
    if (i != 0) {
      dl->AddText(ImVec2(x, y), col, " + ");
      x += ImGui::CalcTextSize(" + ").x;
    }
    DrawIcon(dl, ImVec2(x, y), icon, IconOf(b.inputs[size_t(i)].device), col);
    x += icon + gap;
    const std::string& name = layout.names[size_t(i)];
    dl->AddText(ImVec2(x, y), col, name.c_str());
    x += ImGui::CalcTextSize(name.c_str()).x;
  }
  return clicked;
}

// A binding in words for tooltips: "keyboard Left Shift + keyboard 1".
std::string ChipTooltip(const Binding& b) {
  std::string out;
  for (int i = 0; i < b.count; ++i) {
    if (i != 0) out += " + ";
    out += std::string(IconName(IconOf(b.inputs[size_t(i)].device))) + " " + InputName(b.inputs[size_t(i)]);
  }
  return out;
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

void DrawAdvancedEditor(const Binding& parsed, bool inputsOk);
void SaveDraft(const SViews& views, Binding parsed);

// Appends a touch control to the draft's inputs (a second one makes a chord).
void TouchPicker() {
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
}

void DrawEditor(const SViews& views) {
  if (sQuickAdd) {
    // "+" is recording; Draw() saves what it gets.
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "%s: press any key, mouse button or controller input, or hold "
                       "several for a chord (Esc cancels)...", std::string(PortInput::Info(sDraft.action).label).c_str());
    return;
  }
  ImGui::SeparatorText(sEditing == kEditNew ? "New binding" : "Edit binding");
  ActionCombo("Action", sDraft.action);

  Binding parsed = sDraft;
  const bool inputsOk = InputsFromText(sInputsText, parsed);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Input:");
  ImGui::SameLine();
  if (sCapture.active) {
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), sCapture.armed ? "Press an input or a chord, then release it (Esc cancels)..."
                                                                     : "Release everything...");
  } else {
    if (inputsOk && parsed.count > 0) {
      Chip("##draft", parsed, false, false, false);
      ImGui::SetItemTooltip("%s", sInputsText);
    } else {
      ImGui::TextDisabled(sInputsText[0] == '\0' ? "(none)" : "(not understood: see Advanced)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Record")) StartCapture();
    ImGui::SetItemTooltip("Press one key, mouse button or controller input (mouse wheel and sticks too), or "
                          "hold several in order (the last one is the trigger), then release them all. "
                          "Esc cancels.");
    ImGui::SameLine();
    if (kShowTouch) {
      TouchPicker();
      ImGui::SameLine();
    }
    if (ImGui::Button("Clear")) sInputsText[0] = '\0';
  }

  // Everything past the action and its input, open by itself when the binding uses it.
  if (sAdvancedSync) {
    ImGui::SetNextItemOpen(UsesAdvanced(sDraft), ImGuiCond_Always);
    sAdvancedSync = false;
  }
  if (ImGui::TreeNode("Advanced##edit")) {
    DrawAdvancedEditor(parsed, inputsOk);
    ImGui::TreePop();
  }

  const bool familyOk = inputsOk && parsed.count > 0 && OneFamily(parsed);
  if (inputsOk && parsed.count > 0 && !familyOk) {
    ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                       "A chord's inputs must come from one device (keyboard and mouse count as one).");
  }
  const bool valid = familyOk && sDraft.action != Action::None && sDraft.contexts != 0;
  ImGui::BeginDisabled(!valid);
  if (ImGui::Button("Save")) {
    SaveDraft(views, parsed);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    sEditing = kEditNone;
    EndCapture();
  }
}

void DrawAdvancedEditor(const Binding& parsed, bool inputsOk) {
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##inputs", "pad:leftshoulder + pad:a", sInputsText, sizeof(sInputsText));
  ImGui::SetItemTooltip("Inputs, joined with +: key:<name>, mouse:left, wheel:up, pad:<button>, "
                        "axis:<name>+/-, touch:<control> (a, fire, missile...), gyro:yaw+ ... (docs/NATIVE_PORT.md). "
                        "Append @<percent> for an axis threshold.");
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

  bool everywhere = sDraft.contexts == PortInput::kCtxAll;
  if (ImGui::Checkbox("Works everywhere", &everywhere)) {
    sDraft.contexts = everywhere ? PortInput::kCtxAll : PortInput::kCtxGameplay;
  }
  ImGui::SetItemTooltip("Off: the binding works only where you tick below, so the same input can do "
                        "different things on the map, in menus or in Morph Ball.");
  if (!everywhere) {
    ImGui::TextUnformatted("Only in:");
    for (size_t i = 0; i < std::size(kContexts); ++i) {
      bool on = (sDraft.contexts & kContexts[i].bit) != 0;
      if (i % 5 != 0) ImGui::SameLine();
      if (ImGui::Checkbox(kContexts[i].label, &on)) {
        sDraft.contexts = uint16_t(on ? sDraft.contexts | kContexts[i].bit : sDraft.contexts & ~kContexts[i].bit);
      }
    }
    ImGui::TextDisabled("Layer n: only while the input bound to \"Layer n (hold)\" is held, like a shift "
                        "key. There it wins over the input's normal binding.");
  }
}

void SaveDraft(const SViews& views, Binding parsed) {
  using PortInput::Family;
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
  // The binding goes to its own device's column, wherever it was edited from.
  const Family to = PortInput::BindingFamily(parsed);
  const auto finish = [] {
    sEditing = kEditNone;
    EndCapture();
    Commit();
  };
  if (sEditing == kEditNew) {
    const SView& target = views[to];
    const UserProfile* user = Find(target.match);
    const auto& bindings = user != nullptr && PortInput::Overridden(*user, parsed.action, to)
                               ? user->bindings : target.inherited.bindings;
    if (std::find(bindings.begin(), bindings.end(), parsed) != bindings.end()) {
      // An existing default is already bound: don't turn it into an override.
      sEditing = kEditNone;
      EndCapture();
      return;
    }
  }
  if (sEditing == kEditInherited || sEditing >= 0) {
    const SView& from = views[sEditFamily];
    UserProfile& profile = EditedProfile(from);
    if (sEditing == kEditInherited) {
      PortInput::Materialize(profile, sEditOrig.action, sEditFamily, from.inherited);
      const auto it = std::find(profile.bindings.begin(), profile.bindings.end(), sEditOrig);
      if (it != profile.bindings.end()) PortInput::RemoveUserBinding(profile, size_t(it - profile.bindings.begin()));
    } else if (sEditing < int(profile.bindings.size())) {
      const Binding& old = profile.bindings[size_t(sEditing)];
      if (old.action == parsed.action && to == sEditFamily) {
        PortInput::RemoveUserBinding(profile, size_t(sEditing));
        PortInput::AddUserBinding(profile, parsed);
        finish();
        return;
      }
      PortInput::RemoveUserBinding(profile, size_t(sEditing));
    }
  }
  // Fetched again: making another profile can move the one above.
  UserProfile& profile = EditedProfile(views[to]);
  // Keep the other built-in bindings of this action: a user binding drops them all.
  PortInput::Materialize(profile, parsed.action, to, views[to].inherited);
  PortInput::AddUserBinding(profile, parsed);
  finish();
}

void DrawBindingRow(const Binding& b, bool inherited = false) {
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(std::string(PortInput::Info(b.action).label).c_str());
  ImGui::TableNextColumn();
  Chip("##in", b, false, false, false);
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
  PortInput::Family family = PortInput::Family::Keyboard;
  size_t index = 0;
  Binding binding;
  unsigned revertFamilies = 0; // Revert: bit per overridden family
};

void ApplyPending(const SViews& views, const SPending& pending) {
  using PortInput::Family;
  if (pending.op == SPending::Op::None) return;
  if (pending.op == SPending::Op::Revert) {
    for (const Family f : {Family::Keyboard, Family::Pad, Family::Touch}) {
      if ((pending.revertFamilies >> int(f)) & 1u) PortInput::Revert(EditedProfile(views[f]), pending.action, f);
    }
  } else {
    const SView& v = views[pending.family];
    UserProfile& profile = EditedProfile(v);
    if (pending.op == SPending::Op::RemoveUser) {
      PortInput::RemoveUserBinding(profile, pending.index);
    } else {
      PortInput::Materialize(profile, pending.action, v.family, v.inherited);
      const auto it = std::find(profile.bindings.begin(), profile.bindings.end(), pending.binding);
      if (it != profile.bindings.end()) PortInput::RemoveUserBinding(profile, size_t(it - profile.bindings.begin()));
    }
  }
  sEditing = kEditNone;
  EndCapture();
  Commit();
}

// The table's sections. Layers are for shift-key setups, so they start closed.
struct SGroup {
  const char* label;
  std::vector<Action> actions;
  bool open;
};

const std::vector<SGroup>& Groups() {
  static const std::vector<SGroup> groups = [] {
    std::vector<SGroup> g = {
        {"Game buttons",
         {Action::PadA, Action::PadB, Action::PadX, Action::PadY, Action::PadZ, Action::PadStart, Action::PadL,
          Action::PadR, Action::LAnalog, Action::RAnalog, Action::PadUp, Action::PadDown, Action::PadLeft,
          Action::PadRight},
         true},
        // The C-stick is the right stick: twin-stick aim, classic beam picks, map panning.
        {"Move and aim",
         {Action::MainUp, Action::MainDown, Action::MainLeft, Action::MainRight, Action::CUp, Action::CDown,
          Action::CLeft, Action::CRight},
         true},
        // Direct picks work in every scheme; the GameCube inputs behind them are listed above.
        {"Beams",
         {Action::BeamPower, Action::BeamWave, Action::BeamIce, Action::BeamPlasma, Action::BeamNext,
          Action::BeamPrev},
         true},
        {"Visors",
         {Action::VisorCombat, Action::VisorScan, Action::VisorThermal, Action::VisorXray, Action::VisorNext,
          Action::VisorPrev},
         true},
        {"Morph Ball", {Action::SpringBall}, true},
        {"Port", {Action::PortMenu, Action::SaveState, Action::LoadState, Action::Screenshot, Action::ToggleOriginal}, true},
        {"Layers (shift keys, advanced)", {Action::Layer1, Action::Layer2, Action::Layer3, Action::Layer4}, false},
    };
    // An action added to the engine but not sorted here still gets a row.
    SGroup other{"Other", {}, true};
    // Aim*/Look* aren't read by the game yet (aim is the C-stick, mouse look its own path): no rows.
    const Action unused[] = {Action::AimUp, Action::AimDown, Action::AimLeft, Action::AimRight, Action::LookX,
                             Action::LookY};
    for (int i = 1; i < PortInput::kActionCount; ++i) {
      if (std::find(std::begin(unused), std::end(unused), Action(i)) != std::end(unused)) continue;
      const bool listed = std::any_of(g.begin(), g.end(), [&](const SGroup& s) {
        return std::find(s.actions.begin(), s.actions.end(), Action(i)) != s.actions.end();
      });
      if (!listed) other.actions.push_back(Action(i));
    }
    if (!other.actions.empty()) g.push_back(other);
    return g;
  }();
  return groups;
}

// What a GameCube button does in the game, next to its name.
const char* GameHint(Action a) {
  switch (a) {
  case Action::PadA: return "Fire";
  case Action::PadB: return "Jump";
  case Action::PadX: return "Morph Ball";
  case Action::PadY: return "Missile";
  case Action::PadZ: return "Map";
  case Action::PadStart: return "Pause";
  case Action::PadL:
  case Action::LAnalog: return "Lock on, scan";
  case Action::PadR:
  case Action::RAnalog: return "Free aim";
  case Action::MainUp:
  case Action::MainDown:
  case Action::MainLeft:
  case Action::MainRight: return "Move";
  case Action::CUp:
  case Action::CDown:
  case Action::CLeft:
  case Action::CRight: return "Aim, or pick a beam";
  case Action::PadUp:
  case Action::PadDown:
  case Action::PadLeft:
  case Action::PadRight: return "Pick a visor";
  case Action::AimUp:
  case Action::AimDown:
  case Action::AimLeft:
  case Action::AimRight: return "Twin-stick aim";
  default: return nullptr;
  }
}

// A binding's non-default settings, in words ("" when it has none).
std::string Notes(const Binding& b, bool includeContexts = true) {
  std::string out;
  const auto add = [&](const std::string& s) {
    if (!out.empty()) out += ", ";
    out += s;
  };
  if (b.trigger != Trigger::Press) add(TriggerLabel(b.trigger));
  if (b.turboHz != 0) add("turbo " + std::to_string(b.turboHz) + "/s");
  if (b.IsChord() && b.anyOrder) add("any order");
  if (includeContexts && b.contexts != PortInput::kCtxAll) {
    std::string where;
    for (const SContextInfo& c : kContexts) {
      if ((b.contexts & c.bit) == 0) continue;
      if (!where.empty()) where += ", ";
      where += c.label;
    }
    add("only in " + where);
  }
  return out;
}

// One action's inputs in one device's column.
struct SCell {
  std::vector<std::pair<Binding, int>> rows; // (binding, index into the user's bindings or -1 for a default)
  bool overridden = false;
};

SCell CellOf(const SView& v, const UserProfile* user, Action action) {
  SCell cell;
  cell.overridden = user != nullptr && PortInput::Overridden(*user, action, v.family);
  if (cell.overridden) {
    for (int n = 0; n < int(user->bindings.size()); ++n) {
      const Binding& b = user->bindings[size_t(n)];
      if (b.count > 0 && b.action == action && PortInput::BindingFamily(b) == v.family) cell.rows.push_back({b, n});
    }
  } else {
    for (const Binding& b : PortInput::InFamily(v.inherited.bindings, action, v.family)) cell.rows.push_back({b, -1});
  }
  return cell;
}

// editorDrawn: the editor already went above the table this frame.
void DrawTable(const SViews& views, bool editorDrawn) {
  using PortInput::Family;
  ImGui::SetNextItemWidth(220.f);
  ImGui::InputTextWithHint("##filter", "Filter actions", sFilter, sizeof(sFilter));
  ImGui::SameLine();
  ImGui::Checkbox("Only bound", &sOnlyBound);

  std::vector<Family> families = {Family::Keyboard, Family::Pad};
  if (kShowTouch) families.push_back(Family::Touch);
  const UserProfile* users[3] = {};
  const auto findUsers = [&] {
    for (const Family f : families) users[int(f)] = Find(views[f].match);
  };
  findUsers();
  std::string inputsLabel = "Inputs";
  if (!views[Family::Pad].match.empty()) {
    inputsLabel += " (controller: " +
                   (views[Family::Pad].name.empty() ? views[Family::Pad].match : views[Family::Pad].name) + ")";
  }

  const ImGuiStyle& style = ImGui::GetStyle();
  const float xWidth = ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.f;
  const float xGap = style.ItemInnerSpacing.x * 0.5f;
  SPending pending;
  bool editRowShown = false;
  bool saved = false;
  for (const SGroup& group : Groups()) {
    // A filter opens every section, so a match is never hidden in a closed one.
    if (sFilter[0] != '\0') ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (!ImGui::CollapsingHeader(group.label, group.open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
    // Called with the same ID stack each time, so a table resumed under the
    // editor keeps the columns of the one above it.
    const auto beginTable = [&](bool headers) {
      if (!ImGui::BeginTable(group.label, 3,
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return false;
      }
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.f);
      ImGui::TableSetupColumn(inputsLabel.c_str(), ImGuiTableColumnFlags_WidthStretch, 2.4f);
      // Room for "+" and "Revert" always, so the columns don't move when a row gets a Revert.
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize("+Revert").x + style.FramePadding.x * 4.f + style.ItemSpacing.x);
      if (headers) ImGui::TableHeadersRow();
      return true;
    };
    if (!beginTable(true)) continue;
    bool tableOpen = true;
    // Breaks the table under the row just drawn for the editor, then goes on.
    const auto editorHere = [&] {
      editRowShown = true;
      if (editorDrawn) return;
      editorDrawn = true;
      ImGui::EndTable();
      ImGui::Indent();
      DrawEditor(views);
      ImGui::Unindent();
      ImGui::Separator();
      saved = sEditing == kEditNone;
      findUsers(); // a Save can add a profile
      tableOpen = beginTable(false);
    };
    for (const Action action : group.actions) {
      if (!tableOpen) break;
      const std::string label(PortInput::Info(action).label);
      const char* hint = GameHint(action);
      if (!Contains(label, sFilter) && (hint == nullptr || !Contains(hint, sFilter))) continue;
      SCell cells[3];
      bool any = false;
      unsigned overridden = 0;
      for (const Family f : families) {
        cells[int(f)] = CellOf(views[f], users[int(f)], action);
        any = any || !cells[int(f)].rows.empty();
        if (cells[int(f)].overridden) overridden |= 1u << int(f);
      }
      if (!any && sOnlyBound) continue;

      // The IDs are popped before the editor, so its table resumes with the same ID.
      ImGui::PushID(int(action));
      ImGui::TableNextRow();
      const bool editingAction = sEditing != kEditNone && sEditAction == action;
      if (editingAction) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label.c_str());
      if (hint != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint);
      }

      // Every device's inputs in one cell; the icons tell them apart.
      ImGui::TableNextColumn();
      const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
      bool first = true;
      for (const Family f : families) {
        ImGui::PushID(int(f));
        const SCell& cell = cells[int(f)];
        for (size_t r = 0; r < cell.rows.size(); ++r) {
          const Binding& b = cell.rows[r].first;
          const int userIndex = cell.rows[r].second;
          const bool isDefault = userIndex < 0;
          const std::string notes = Notes(b, false);
          // Inputs flow left to right and wrap at the column's edge.
          if (!first) {
            float width = ChipLayout(b).width + xGap + xWidth;
            if (!notes.empty()) width += style.ItemSpacing.x + ImGui::CalcTextSize(("(" + notes + ")").c_str()).x;
            if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right) ImGui::SameLine();
          }
          first = false;
          ImGui::PushID(int(r));
          const bool selected = sEditFamily == f && sEditAction == action &&
                                (isDefault ? sEditing == kEditInherited && b == sEditOrig : sEditing == userIndex);
          if (Chip("##in", b, isDefault, selected)) {
            BeginEdit(isDefault ? kEditInherited : userIndex, b, f);
            sEditOrig = b;
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            std::string tip = ChipTooltip(b);
            const std::string tooltipNotes = Notes(b);
            if (!tooltipNotes.empty()) tip += "\n" + tooltipNotes;
            tip += isDefault ? "\nDefault: changing or removing it makes this device's inputs for the action "
                               "yours; Revert gives the defaults back."
                             : "\nClick to change.";
            ImGui::SetTooltip("%s", tip.c_str());
          }
          if (!notes.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", notes.c_str());
          }
          ImGui::SameLine(0.f, xGap);
          if (ImGui::SmallButton("x")) {
            pending.action = action;
            pending.family = f;
            if (isDefault) {
              pending.op = SPending::Op::RemoveInherited;
              pending.binding = b;
            } else {
              pending.op = SPending::Op::RemoveUser;
              pending.index = size_t(userIndex);
            }
          }
          ImGui::SetItemTooltip("Remove this input.");
          ImGui::PopID();
        }
        // The touch controls hide under this menu, so they're picked, not recorded.
        if (f == Family::Touch) {
          if (!first) ImGui::SameLine();
          first = false;
          if (ImGui::SmallButton("+ Touch")) ImGui::OpenPopup("addTouch");
          ImGui::SetItemTooltip("Add a touch control.");
          if (ImGui::BeginPopup("addTouch")) {
            for (const STouchInfo& t : kTouchControls) {
              if (!ImGui::Selectable(t.label)) continue;
              BeginEdit(kEditNew, Binding{action}, Family::Touch);
              Binding parsed = sDraft;
              std::snprintf(sInputsText, sizeof(sInputsText), "touch:%s", t.name);
              if (InputsFromText(sInputsText, parsed)) {
                SaveDraft(views, parsed);
                saved = true;
                findUsers();
              } else {
                sEditing = kEditNone;
              }
            }
            ImGui::EndPopup();
          }
        }
        ImGui::PopID();
      }
      if (first) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(overridden != 0 ? "(removed)" : "-");
      }

      ImGui::TableNextColumn();
      if (ImGui::SmallButton("+")) {
        BeginEdit(kEditNew, Binding{action}, Family::Keyboard);
        sQuickAdd = true;
        StartCapture();
      }
      ImGui::SetItemTooltip("Add an input: press +, then any key, mouse button or controller input (hold several "
                            "for a chord).");
      if (overridden != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Revert")) {
          pending.op = SPending::Op::Revert;
          pending.action = action;
          pending.revertFamilies = overridden;
        }
        ImGui::SetItemTooltip("Back to the default inputs on every device.");
      }
      ImGui::PopID();
      // Read after the row's buttons: a click this frame opens the editor at once.
      if (sEditing != kEditNone && sEditAction == action) editorHere();
    }
    if (tableOpen) ImGui::EndTable();
  }
  sEditRowShown = editRowShown;
  // A remove index from before a Save in the same frame may be stale.
  if (!saved) ApplyPending(views, pending);
}

struct SPreset {
  const char* name;
  const char* id;
};
constexpr SPreset kKeyPresets[] = {{"Classic", "classic"}, {"Mouse & keyboard", "mouse"}};
constexpr SPreset kPadPresets[] = {
    {"GameCube", "gamecube"}, {"Remastered", "remastered"}, {"Modern", "modern"}, {"Southpaw", "southpaw"}};

// One device's reset: back to a preset (or just the defaults), behind the popup's confirmation.
void ResetSection(const SView& v, const SPreset* presets, int count, int& preset) {
  using PortInput::Family;
  ImGui::PushID(int(v.family));
  ImGui::SeparatorText(v.family == Family::Pad && !v.match.empty() ? (v.name.empty() ? v.match : v.name).c_str()
                                                                   : kFamilies[int(v.family)]);
  const bool pads = v.family == Family::Pad;
  const bool gcAdapter = PADIsGCAdapter(0) != 0;
  const auto needsPad = [&](int i) {
    return pads && (std::string_view(presets[i].id) == "remastered" || std::string_view(presets[i].id) == "modern");
  };
  if (count > 0) {
    for (int i = 0; i < count; ++i) {
      ImGui::BeginDisabled(needsPad(i) && gcAdapter);
      if (i != 0) ImGui::SameLine();
      ImGui::RadioButton(presets[i].name, &preset, i);
      ImGui::EndDisabled();
    }
    if (preset >= count || (needsPad(preset) && gcAdapter)) preset = 0;
    ImGui::TextDisabled("Your own bindings for this device are removed; the others are left alone.");
  } else if (v.family == Family::Touch) {
    ImGui::TextDisabled("Removes every touch binding of yours.");
  } else {
    ImGui::TextDisabled("Removes this controller's own bindings, so it uses Controllers' again.");
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
    if (count > 0 && v.family == Family::Keyboard) PortControls::ApplyKeyPresetNamed(presets[preset].id);
    if (count > 0 && pads) PortControls::ApplyPadPresetNamed(presets[preset].id);
    ImGui::CloseCurrentPopup();
  }
  ImGui::PopID();
}

// The reset buttons, one per device, behind a popup.
void DrawReset(const SViews& views) {
  using PortInput::Family;
  static int sKeyPreset = 0;
  static int sPadPreset = 0;
  if (ImGui::Button("Reset...")) {
    sKeyPreset = 0;
    sPadPreset = 0;
    ImGui::OpenPopup("resetDevice");
  }
  if (!ImGui::BeginPopup("resetDevice")) return;
  ResetSection(views[Family::Keyboard], kKeyPresets, int(std::size(kKeyPresets)), sKeyPreset);
  const bool allPads = views[Family::Pad].match.empty();
  ResetSection(views[Family::Pad], allPads ? kPadPresets : nullptr, allPads ? int(std::size(kPadPresets)) : 0,
               sPadPreset);
  if (kShowTouch) {
    int none = 0;
    ResetSection(views[Family::Touch], nullptr, 0, none);
  }
  ImGui::Separator();
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
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
    ImGui::SetItemTooltip("On: only this device's bindings apply (with Controllers'). Off: they are "
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
        sPadDevice = "pads";
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

  ImGui::TextWrapped("Each action lists its inputs for every device. Press + and then any key, mouse button or "
                     "controller input to add one; click an input to change it, x removes it. Greyed inputs are "
                     "the defaults; Revert brings them back.");
  if (!sError.empty()) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", sError.c_str());

  // The Controller column shows every controller's bindings, or one kind's or model's on top of them.
  const std::vector<SDeviceEntry> pads = PadEntries();
  if (std::none_of(pads.begin(), pads.end(), [](const SDeviceEntry& e) { return e.key == sPadDevice; })) {
    sPadDevice = "pads";
  }
  std::string current;
  for (const SDeviceEntry& e : pads) {
    if (e.key == sPadDevice) current = e.label;
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.f);
  if (ImGui::BeginCombo("Controller bindings for", current.c_str())) {
    for (const SDeviceEntry& e : pads) {
      if (ImGui::Selectable((e.label + "##" + e.key).c_str(), e.key == sPadDevice) && e.key != sPadDevice) {
        sPadDevice = e.key;
        sEditing = kEditNone;
        EndCapture();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("All controllers: bindings every controller uses. A kind or model gets its own bindings "
                        "on top of those; connected controllers are listed.");

  const SViews views = MakeViews();
  UpdateCapture();
  if (sQuickAdd && !sCapture.active) {
    sQuickAdd = false;
    Binding parsed = sDraft;
    if (sInputsText[0] != '\0' && InputsFromText(sInputsText, parsed) && parsed.count > 0 && OneFamily(parsed)) {
      SaveDraft(views, parsed);
    } else {
      sEditing = kEditNone;
    }
  }

  ImGui::SameLine();
  DrawReset(views);
  ImGui::Separator();

  const bool editorOnTop = sEditing != kEditNone && !sEditRowShown;
  if (editorOnTop) DrawEditor(views);
  DrawTable(views, editorOnTop);
  ImGui::Separator();
  if (ImGui::TreeNode("Advanced##page")) {
    DrawProfileOptions(views[PortInput::Family::Pad]);
    DrawEffective();
    ImGui::TreePop();
  }
}

} // namespace PortInputRemap
