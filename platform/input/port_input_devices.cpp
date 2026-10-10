#include "port_input_devices.h"

#include "port_debug.h"
#include "port_input_map.h"
#include "port_input_remap_ui.h"
#include "port_log.h"
#include "port_paths.h"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_touch.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace aurora {
void request_screenshot() noexcept;
}

namespace PortInputDevices {
namespace {

using PortInput::Action;
using PortInput::Binding;
using PortInput::Device;
using PortInput::Input;
using PortInput::Profile;

constexpr u32 kPort = 0;

// The user's bindings (controls.toml), loaded on first use and reloaded when the file
// changes (checked once a second), so hand edits apply without a restart. The game
// thread polls them and the F1 page writes them, so they're published as immutable
// snapshots under a lock.
std::mutex sUserMutex;
std::shared_ptr<const PortInput::UserBindings> sUser = std::make_shared<const PortInput::UserBindings>();
uint64_t sUserVersion = 0;
bool sUserLoaded = false;
std::filesystem::file_time_type sUserTime{};
std::chrono::steady_clock::time_point sUserChecked{};

// Touch overlay controls held now (codes 0..kTouchControlCount-1), set from Android's
// UI thread.
std::atomic<uint64_t> sTouchControls{0};
std::atomic<uint64_t> sTouchBound{0};

// Mouse wheel notches since the last poll (x1000, vertical then horizontal), added
// by an SDL event watch on the event thread.
std::atomic<int> sWheelMilli[2]{};

bool SDLCALL WheelWatch(void*, SDL_Event* event) {
  if (event->type != SDL_EVENT_MOUSE_WHEEL || event->wheel.which == SDL_TOUCH_MOUSEID) return true;
  const float flip = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.f : 1.f;
  sWheelMilli[0].fetch_add(static_cast< int >(std::lround(event->wheel.y * flip * 1000.f)), std::memory_order_relaxed);
  sWheelMilli[1].fetch_add(static_cast< int >(std::lround(event->wheel.x * flip * 1000.f)), std::memory_order_relaxed);
  return true;
}

std::string UserBindingsPath() { return PortPaths::UserFolder() + "controls.toml"; }

std::string ScancodeName(int scancode) {
  const char* name = SDL_GetScancodeName(static_cast< SDL_Scancode >(scancode));
  return name != nullptr ? name : "";
}

int ScancodeFromName(std::string_view name) {
  const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
  return code == SDL_SCANCODE_UNKNOWN ? -1 : static_cast< int >(code);
}

void InstallKeyNames() { PortInput::SetKeyNames(ScancodeName, ScancodeFromName); }

Action PadBitAction(unsigned padButton) {
  switch (padButton) {
  case PAD_BUTTON_A: return Action::PadA;
  case PAD_BUTTON_B: return Action::PadB;
  case PAD_BUTTON_X: return Action::PadX;
  case PAD_BUTTON_Y: return Action::PadY;
  case PAD_TRIGGER_Z: return Action::PadZ;
  case PAD_BUTTON_START: return Action::PadStart;
  case PAD_TRIGGER_L: return Action::PadL;
  case PAD_TRIGGER_R: return Action::PadR;
  case PAD_BUTTON_UP: return Action::PadUp;
  case PAD_BUTTON_DOWN: return Action::PadDown;
  case PAD_BUTTON_LEFT: return Action::PadLeft;
  case PAD_BUTTON_RIGHT: return Action::PadRight;
  default: return Action::None;
  }
}

// Aurora's PAD axes, as the port's actions (PAD_AXIS_LEFT_Y_POS is up).
Action PadAxisAction(PADAxis axis) {
  switch (axis) {
  case PAD_AXIS_LEFT_X_POS: return Action::MainRight;
  case PAD_AXIS_LEFT_X_NEG: return Action::MainLeft;
  case PAD_AXIS_LEFT_Y_POS: return Action::MainUp;
  case PAD_AXIS_LEFT_Y_NEG: return Action::MainDown;
  case PAD_AXIS_RIGHT_X_POS: return Action::CRight;
  case PAD_AXIS_RIGHT_X_NEG: return Action::CLeft;
  case PAD_AXIS_RIGHT_Y_POS: return Action::CUp;
  case PAD_AXIS_RIGHT_Y_NEG: return Action::CDown;
  case PAD_AXIS_TRIGGER_L: return Action::LAnalog;
  case PAD_AXIS_TRIGGER_R: return Action::RAnalog;
  default: return Action::None;
  }
}

// A native button code (SDL_GamepadButton or a trigger's half pull), as the raw
// state's pad button bit; -1 for none.
int NativeButtonCode(u32 native) {
  if (native == PAD_NATIVE_BUTTON_TRIGGER_LEFT) return PAD_RAW_BUTTON_TRIGGER_LEFT;
  if (native == PAD_NATIVE_BUTTON_TRIGGER_RIGHT) return PAD_RAW_BUTTON_TRIGGER_RIGHT;
  if (native < SDL_GAMEPAD_BUTTON_COUNT) return static_cast< int >(native);
  return -1;
}

Input Make(Device device, int code, int dir = 0) {
  Input in;
  in.device = device;
  in.code = static_cast< uint16_t >(code);
  in.dir = static_cast< int8_t >(dir);
  return in;
}

void Bind(Profile& p, Action action, const Input& in, uint16_t turboHz = 0) {
  if (action == Action::None || !in.Valid()) return;
  Binding b;
  b.action = action;
  b.inputs[0] = in;
  b.count = 1;
  b.turboHz = turboHz;
  p.bindings.push_back(b);
}

void BindIn(Profile& p, Action action, const Input& in, uint16_t contexts) {
  if (action == Action::None || !in.Valid()) return;
  Binding b;
  b.action = action;
  b.inputs[0] = in;
  b.count = 1;
  b.contexts = contexts;
  p.bindings.push_back(b);
}

// A key slot's scancode: a key, or a mouse button read the way `mouseBase` says.
Input KeySlotInput(s32 scancode, int mouseBase) {
  if (scancode >= 0 && scancode < PortInput::kKeyCount) return Make(Device::Key, scancode);
  if (scancode <= PAD_KEY_MOUSE_LEFT && scancode >= PAD_KEY_MOUSE_X2) {
    return Make(Device::MouseButton, mouseBase + (PAD_KEY_MOUSE_LEFT - scancode));
  }
  return {};
}

Input NativeInput(s32 native) {
  const int code = native < 0 ? -1 : NativeButtonCode(static_cast< u32 >(native));
  return code < 0 ? Input{} : Make(Device::PadButton, code);
}

// trigger_axis_taken in Aurora's pad.cpp: the L/R analog's input also presses
// another PAD button, so it drives only that button.
bool TriggerAxisTaken(const PADAxisMapping* axes, u32 axisCount, const PADButtonMapping* buttons,
                      u32 buttonCount, PADAxis axis) {
  const PADAxisMapping* m = nullptr;
  for (u32 i = 0; i < axisCount; ++i) {
    if (axes[i].padAxis == axis) {
      m = &axes[i];
      break;
    }
  }
  if (m == nullptr) return false;
  u32 native = static_cast< u32 >(m->nativeButton);
  if (m->nativeAxis.nativeAxis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
    native = PAD_NATIVE_BUTTON_TRIGGER_LEFT;
  } else if (m->nativeAxis.nativeAxis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    native = PAD_NATIVE_BUTTON_TRIGGER_RIGHT;
  } else if (m->nativeAxis.nativeAxis != -1) {
    return false;
  }
  if (native == PAD_NATIVE_BUTTON_INVALID) return false;
  bool other = false;
  for (u32 i = 0; i < buttonCount; ++i) {
    if (buttons[i].nativeButton != native) continue;
    if (buttons[i].padButton == PAD_TRIGGER_L || buttons[i].padButton == PAD_TRIGGER_R) return false;
    other = true;
  }
  return other;
}

void BindController(Profile& p) {
  u32 buttonCount = 0;
  u32 axisCount = 0;
  const PADButtonMapping* buttons = PADGetButtonMappings(kPort, &buttonCount);
  const PADAxisMapping* axes = PADGetAxisMappings(kPort, &axisCount);
  const PADDeadZones* dz = PADGetDeadZones(kPort);
  // No controller on the port: bind the standard defaults anyway, so the Remap
  // page shows what a pad would get (nothing reads them until one connects).
  PADDefaultMapping fallback;
  if (buttons == nullptr || axes == nullptr) {
    PADGetDefaultMapping(&fallback, PAD_TYPE_STANDARD);
    buttons = fallback.buttons;
    axes = fallback.axes;
    buttonCount = PAD_BUTTON_COUNT;
    axisCount = PAD_AXIS_COUNT;
  }

  bool lSet = false;
  bool rSet = false;
  for (u32 i = 0; i < buttonCount; ++i) {
    if (buttons[i].nativeButton == PAD_NATIVE_BUTTON_INVALID) continue;
    if (buttons[i].padButton == PAD_TRIGGER_L) lSet = true;
    if (buttons[i].padButton == PAD_TRIGGER_R) rSet = true;
    Bind(p, PadBitAction(buttons[i].padButton), NativeInput(static_cast< s32 >(buttons[i].nativeButton)));
  }

  for (u32 i = 0; i < axisCount; ++i) {
    const PADAxisMapping& m = axes[i];
    const Action action = PadAxisAction(m.padAxis);
    const bool trigger = m.padAxis == PAD_AXIS_TRIGGER_L || m.padAxis == PAD_AXIS_TRIGGER_R;
    if (trigger && TriggerAxisTaken(axes, axisCount, buttons, buttonCount, m.padAxis)) continue;
    if (m.nativeAxis.nativeAxis >= 0 && m.nativeAxis.nativeAxis < PAD_RAW_AXIS_COUNT) {
      Bind(p, action, Make(Device::PadAxis, m.nativeAxis.nativeAxis, m.nativeAxis.sign < 0 ? -1 : 1));
    } else if (m.nativeAxis.nativeAxis == -1) {
      Bind(p, action, NativeInput(m.nativeButton));
    }
    // Aurora's emulated trigger click: a pulled L/R analog presses L/R when no
    // button is mapped to them.
    if (trigger && (dz == nullptr || dz->emulateTriggers)) {
      const bool left = m.padAxis == PAD_AXIS_TRIGGER_L;
      if (!(left ? lSet : rSet)) {
        Bind(p, left ? Action::PadL : Action::PadR,
             Make(Device::PadButton,
                  left ? PAD_RAW_BUTTON_TRIGGER_LEFT_CLICK : PAD_RAW_BUTTON_TRIGGER_RIGHT_CLICK));
      }
    }
  }
}

void BindKeyboard(Profile& p) {
  for (u32 slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    u32 count = 0;
    if (const PADKeyButtonBinding* keys = PADGetKeyButtonBindingsSlot(kPort, slot, &count)) {
      for (u32 i = 0; i < count; ++i) {
        Bind(p, PadBitAction(keys[i].padButton), KeySlotInput(keys[i].scancode, kMouseKeySlot));
      }
    }
    if (const PADKeyAxisBinding* axes = PADGetKeyAxisBindingsSlot(kPort, slot, &count)) {
      for (u32 i = 0; i < count; ++i) {
        Bind(p, PadAxisAction(axes[i].padAxis), KeySlotInput(axes[i].scancode, kMouseKeySlot));
      }
    }
  }
}

// LB (Remastered's layout locks on with L and jumps with LB) is the pad's beam shift
// under twin stick, unless it presses another PAD button (a visor's D-pad row):
// held as the shift, it would turn that D-pad press into a beam.
bool PadButtonBoundBesidesL(s32 native) {
  u32 count = 0;
  const PADButtonMapping* list = PADGetButtonMappings(kPort, &count);
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].nativeButton == static_cast< u32 >(native) && list[i].padButton != PAD_TRIGGER_L) return true;
  }
  for (int bit = 0; bit < PortDebug::kPadAltCount; ++bit) {
    if (bit != std::countr_zero(static_cast< unsigned >(PAD_TRIGGER_L)) && PortDebug::PadAltButton(bit) == native) return true;
  }
  return false;
}

bool IsKeyboardFamily(const Input& in) { return in.device == Device::Key || in.device == Device::MouseButton; }

bool IsPadFamily(const Input& in) { return in.device == Device::PadButton || in.device == Device::PadAxis; }

// The beams' chords: a shift plus a D-pad direction, the direction the retail
// C-stick picks them with (Power up, Ice down, Plasma left, Wave right; the beam
// hint prompts). A shift pairs with the D-pad inputs of its own family: the
// keyboard's shift keys and mouse buttons with the keyboard's D-pad keys, the pad's
// shift and the touch overlay's Beam button with the pad's D-pad (the touch D-pad
// is a virtual pad). The shifts also spring the ball, alone, in morph ball, where
// beams don't change. D-pad inputs left single still pick visors.
void BindBeamChords(Profile& p) {
  std::vector< Input > keyShifts;
  std::vector< Input > padShifts;
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    keyShifts.push_back(KeySlotInput(PortDebug::ShiftBinding(slot), kMouseHeld));
  }
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    if (PortDebug::MouseAction(i) == PortInputMap::kMA_Shift) {
      keyShifts.push_back(Make(Device::MouseButton, kMouseGameplay + i));
    }
  }
  padShifts.push_back(NativeInput(PortDebug::ShiftBinding(PAD_KEY_SLOT_COUNT)));
  padShifts.push_back(Make(Device::Touch, kTouchBeamShift));

  if (PortDebug::TwinStick()) {
    // Twin stick consumes the C-stick, so the shifts that are always there under
    // it: Left Shift unless a key takes it, and LB.
    bool anyKey = false;
    bool lshiftTaken = false;
    for (const Input& in : keyShifts) anyKey = anyKey || in.Valid();
    for (const Binding& b : p.bindings) {
      if (b.count == 1 && b.inputs[0].device == Device::Key && b.inputs[0].code == SDL_SCANCODE_LSHIFT) {
        lshiftTaken = true;
      }
    }
    if (!anyKey && !lshiftTaken) keyShifts.push_back(Make(Device::Key, SDL_SCANCODE_LSHIFT));
    bool anyPad = false;
    for (const Input& in : padShifts) anyPad = anyPad || (in.Valid() && in.device == Device::PadButton);
    if (!anyPad && !PortDebug::TouchActive() && !PadButtonBoundBesidesL(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
      padShifts.push_back(NativeInput(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    }
  }

  // EBeamId order of the direction: Up, Down, Left, Right.
  static constexpr Action kDirs[4] = {Action::PadUp, Action::PadDown, Action::PadLeft, Action::PadRight};
  static constexpr Action kBeams[4] = {Action::BeamPower, Action::BeamIce, Action::BeamPlasma, Action::BeamWave};
  const std::vector< Binding > base = p.bindings;
  const uint16_t noBall = static_cast< uint16_t >(PortInput::kCtxAll & ~PortInput::kCtxMorphBall);
  for (int d = 0; d < 4; ++d) {
    for (const Binding& dir : base) {
      if (dir.action != kDirs[d] || dir.count != 1) continue;
      const bool keyboard = IsKeyboardFamily(dir.inputs[0]);
      if (!keyboard && !IsPadFamily(dir.inputs[0])) continue;
      for (const Input& shift : keyboard ? keyShifts : padShifts) {
        if (!shift.Valid() || shift == dir.inputs[0]) continue;
        Binding b;
        b.action = kBeams[d];
        b.inputs[0] = shift;
        b.inputs[1] = dir.inputs[0];
        b.count = 2;
        b.contexts = noBall;
        p.bindings.push_back(b);
      }
    }
  }
  for (const Input& shift : keyShifts) BindIn(p, Action::SpringBall, shift, PortInput::kCtxMorphBall);
  for (const Input& shift : padShifts) BindIn(p, Action::SpringBall, shift, PortInput::kCtxMorphBall);
}

void BindPortControls(Profile& p) {
  for (int bit = 0; bit < PortDebug::kPadAltCount; ++bit) {
    Bind(p, PadBitAction(1u << bit), NativeInput(PortDebug::PadAltButton(bit)));
  }

  BindBeamChords(p);

  // Turbo: A on one poll and off the next, so the gun sees a press edge every
  // other tick. Retail has no turbo; the touch button has its own setting.
  const float tick = PortDebug::TickPeriod();
  const auto turboHz = static_cast< uint16_t >(std::lround(tick > 0.f ? 0.5f / tick : 30.f));
  if (!PortDebug::OriginalExperience()) {
    for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
      Bind(p, Action::PadA, KeySlotInput(PortDebug::TurboBinding(slot), kMouseHeld), turboHz);
    }
    Bind(p, Action::PadA, NativeInput(PortDebug::TurboBinding(PAD_KEY_SLOT_COUNT)), turboHz);
  }
  Bind(p, Action::PadA, Make(Device::Touch, kTouchTurbo), turboHz);
  Bind(p, Action::PadZ, Make(Device::Touch, kTouchMapTap));

  // The wheel cycles beams, as in PrimeHack (issue #61); retail has no wheel.
  if (!PortDebug::OriginalExperience()) {
    Bind(p, Action::BeamPrev, Make(Device::MouseWheel, 0, 1));
    Bind(p, Action::BeamNext, Make(Device::MouseWheel, 0, -1));
  }

  // Mouse buttons: under mouse aim each does its action (or the beam shift);
  // out of it the ones on A and B still press them (bombs, boosts, text boxes).
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    const int action = PortDebug::MouseAction(i);
    if (action == PortInputMap::kMA_Shift) continue; // a beam chord's shift, bound above
    const unsigned pad = PortInputMap::MouseActionInfo(action).padButton;
    const Action a = PadBitAction(pad);
    Bind(p, a, Make(Device::MouseButton, kMouseGameplay + i));
    if (pad == PAD_BUTTON_A || pad == PAD_BUTTON_B) {
      Bind(p, a, Make(Device::MouseButton, kMouseMenu + i));
    }
  }
}

float StickValue(s16 raw, u16 deadZone, bool useDeadZones) {
  if (useDeadZones && std::abs(static_cast< int >(raw)) <= deadZone) return 0.f;
  return std::clamp(static_cast< float >(raw) / 32767.f, -1.f, 1.f);
}

void FillRawState(PortInput::RawState& raw, unsigned mouseHeld, bool focused) {
  int numKeys = 0;
  if (const bool* keys = SDL_GetKeyboardState(&numKeys)) {
    for (int i = 0; i < numKeys && i < PortInput::kKeyCount; ++i) {
      if (keys[i]) raw.keys.set(static_cast< size_t >(i));
    }
  }

  PADRawState pad{};
  if (PADGetRawState(kPort, &pad)) {
    raw.padButtons = pad.buttons;
    const PADDeadZones* dz = PADGetDeadZones(kPort);
    const bool useDz = dz != nullptr && dz->useDeadzones;
    for (int a = 0; a < PAD_RAW_AXIS_COUNT; ++a) {
      if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        raw.padAxes[a] = std::max(0.f, static_cast< float >(pad.axes[a]) / 32767.f);
      } else {
        const bool left = a == SDL_GAMEPAD_AXIS_LEFTX || a == SDL_GAMEPAD_AXIS_LEFTY;
        raw.padAxes[a] = StickValue(pad.axes[a], dz == nullptr ? 0 : left ? dz->stickDeadZone : dz->substickDeadZone,
                                    useDz);
      }
    }
  }

  uint32_t mouse = 0;
  // Aurora reads a key slot's mouse button with SDL_GetMouseState.
  mouse |= (SDL_GetMouseState(nullptr, nullptr) & PortInputMap::kMouseButtonMask) << kMouseKeySlot;
  mouse |= (PortDebug::MouseHeldButtons() & PortInputMap::kMouseButtonMask) << kMouseHeld;
  int actions[PortInputMap::kMouseButtonCount];
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    actions[i] = PortDebug::MouseAction(i);
  }
  // Its gate also waits for a release, so a held charge carried into morph ball
  // doesn't drop a bomb; only the A/B buttons feed it, so a held side button
  // with nothing to do there doesn't hold it shut.
  const unsigned menuMask = PortInputMap::MouseButtonsFor(actions, PAD_BUTTON_A | PAD_BUTTON_B);
  mouse |= (PortDebug::MouseMenuButtons(mouseHeld & menuMask, focused) & PortInputMap::kMouseButtonMask)
           << kMouseMenu;
  if (PortDebug::MouseGameplayActive() && PortDebug::MouseCaptured() && PortDebug::MouseButtons()) {
    mouse |= (PortDebug::MouseWeaponButtons(mouseHeld) & PortInputMap::kMouseButtonMask) << kMouseGameplay;
  }
  raw.mouseButtons = mouse;

  // A whole notch is a press for one poll; a smooth-scrolling wheel's fractions add
  // up to one, and extra notches in one poll are dropped. Scrolling over the F1
  // overlay or an unfocused window is dropped too.
  static bool sWheelWatch = false;
  if (!sWheelWatch) sWheelWatch = SDL_AddEventWatch(WheelWatch, nullptr);
  const bool wheelLive = focused && !PortDebug::Visible();
  for (int i = 0; i < 2; ++i) {
    const int milli = sWheelMilli[i].exchange(0, std::memory_order_relaxed);
    if (!wheelLive) continue;
    if (std::abs(milli) >= 1000) {
      raw.wheel[i] = milli > 0 ? 1.f : -1.f;
    } else if (milli != 0) {
      sWheelMilli[i].fetch_add(milli, std::memory_order_relaxed);
    }
  }

  raw.touch = sTouchControls.load(std::memory_order_relaxed);
  if (PortDebug::TouchBeamShift()) raw.touch |= uint64_t{1} << kTouchBeamShift;
  if (PortDebug::TouchTurboFire()) raw.touch |= uint64_t{1} << kTouchTurbo;
  if (PortDebug::ConsumeMapTapZ()) raw.touch |= uint64_t{1} << kTouchMapTap;
}

// Aurora's stick conversion: x/256 truncated, y negated (SDL's y points down).
s8 StickX(float right, float left) {
  const long v = std::clamp(std::lround((right - left) * 32767.f), -32768L, 32767L);
  return static_cast< s8 >(std::clamp(static_cast< int >(v / 256), -127, 127));
}

s8 StickY(float down, float up) {
  const auto yl = static_cast< Sint16 >(std::clamp(std::lround((down - up) * 32767.f), -32768L, 32767L));
  const auto y = static_cast< Sint16 >(-(yl + 1u) / 256u);
  return static_cast< s8 >(std::clamp(static_cast< int >(y), -127, 127));
}

// One of menu (front end, pause screen), map, morph ball or gameplay; the scan
// visor adds scan to gameplay.
uint16_t GameContexts() {
  CStateManager* mgr = PortDebug::StateManager();
  if (mgr == nullptr || PortDebug::PauseScreenOpen()) return PortInput::kCtxMenu;
  if (PortDebug::MapScreenOpen()) return PortInput::kCtxMap;
  if (const CPlayer* player = mgr->GetPlayer()) {
    const CPlayer::EPlayerMorphBallState morph = player->GetMorphballTransitionState();
    if (morph == CPlayer::kMS_Morphed || morph == CPlayer::kMS_Morphing) return PortInput::kCtxMorphBall;
  }
  uint16_t contexts = PortInput::kCtxGameplay;
  const CPlayerState* state = mgr->GetPlayerState();
  if (state != nullptr && state->GetCurrentVisor() == CPlayerState::kPV_Scan) contexts |= PortInput::kCtxScan;
  return contexts;
}

// A next/previous pick the game hasn't taken yet.
struct CyclePending {
  int target = -1;
  uint64_t untilNs = 0;
};
CyclePending sCyclePending[2]; // [0] visor, [1] beam

// The beam or visor the game is on or switching to; -1 out of a game.
int CurrentPick(bool beam) {
  CStateManager* mgr = PortDebug::StateManager();
  const CPlayerState* state = mgr != nullptr ? mgr->GetPlayerState() : nullptr;
  const CPlayer* player = mgr != nullptr ? mgr->GetPlayer() : nullptr;
  if (state == nullptr || player == nullptr) return -1;
  return beam ? static_cast< int >(player->GetPlayerGun()->GetPrimaryDestWeaponId())
              : static_cast< int >(state->GetTransitioningVisor());
}

// Next/previous beam or visor: the owned one `dir` steps from the current pick, in
// the pause screen's order (Power, Wave, Ice, Plasma; Combat, Scan, Thermal, X-ray),
// as PrimeHack cycles them. -1 when there's nothing else to pick.
int CycleTarget(bool beam, int dir) {
  // EBeamId / EPlayerVisor values in cycle order, and the item that owns each.
  static constexpr int kBeamOrder[] = {CPlayerState::kBI_Power, CPlayerState::kBI_Wave, CPlayerState::kBI_Ice,
                                       CPlayerState::kBI_Plasma};
  static constexpr CPlayerState::EItemType kBeamItems[] = {CPlayerState::kIT_PowerBeam, CPlayerState::kIT_WaveBeam,
                                                           CPlayerState::kIT_IceBeam, CPlayerState::kIT_PlasmaBeam};
  static constexpr int kVisorOrder[] = {CPlayerState::kPV_Combat, CPlayerState::kPV_Scan, CPlayerState::kPV_Thermal,
                                        CPlayerState::kPV_XRay};
  static constexpr CPlayerState::EItemType kVisorItems[] = {
      CPlayerState::kIT_CombatVisor, CPlayerState::kIT_ScanVisor, CPlayerState::kIT_ThermalVisor,
      CPlayerState::kIT_XRayVisor};
  const CPlayerState* state = PortDebug::StateManager() != nullptr ? PortDebug::StateManager()->GetPlayerState() : nullptr;
  const int dest = CurrentPick(beam);
  if (state == nullptr || dest < 0) return -1;
  const int* order = beam ? kBeamOrder : kVisorOrder;
  const CPlayerState::EItemType* items = beam ? kBeamItems : kVisorItems;
  // Count from a pick the game hasn't taken yet, so quick notches keep stepping.
  const int current = sCyclePending[beam].target >= 0 ? sCyclePending[beam].target : dest;
  int pos = 0;
  while (pos < 4 && order[pos] != current) ++pos;
  if (pos == 4) pos = 0;
  for (int step = 1; step < 4; ++step) {
    const int i = ((pos + dir * step) % 4 + 4) % 4;
    if (state->HasPowerUp(items[i])) return order[i];
  }
  return -1;
}

// Starts a cycle pick, or re-sends the pending one each poll until the game has
// taken it: the gun and visor ignore requests while they're still switching
// (CPlayerGun::HandleWeaponChange skips input in weapon state 0x8), and a request
// only lasts 120 ms. `beam` picks which; `target` -1 just keeps the pending one alive.
void PushCyclePick(bool beam, int target) {
  CyclePending& p = sCyclePending[beam];
  const uint64_t now = SDL_GetTicksNS();
  if (target >= 0) {
    p.target = target;
    p.untilNs = now + 1'500'000'000ull;
  }
  if (p.target < 0) return;
  if (now > p.untilNs || CurrentPick(beam) == p.target) {
    p.target = -1;
    return;
  }
  if (beam) {
    PortDebug::RequestBeam(p.target);
  } else {
    PortDebug::RequestVisor(p.target);
  }
}

// The actions that aren't pad inputs go straight to the code that does them.
void RouteDirectActions(const PortInput::Output& out) {
  // EBeamId and EPlayerVisor order, as RequestBeam/RequestVisor take them.
  static constexpr Action kBeams[] = {Action::BeamPower, Action::BeamIce, Action::BeamWave, Action::BeamPlasma};
  static constexpr Action kVisors[] = {Action::VisorCombat, Action::VisorXray, Action::VisorScan,
                                       Action::VisorThermal};
  for (int i = 0; i < 4; ++i) {
    // A direct pick replaces a pending cycle pick.
    if (out.Pressed(kBeams[i])) {
      sCyclePending[1].target = -1;
      PortDebug::RequestBeam(i);
    }
    if (out.Pressed(kVisors[i])) {
      sCyclePending[0].target = -1;
      PortDebug::RequestVisor(i);
    }
  }
  const int beamDir = out.Pressed(Action::BeamNext) ? 1 : out.Pressed(Action::BeamPrev) ? -1 : 0;
  PushCyclePick(true, beamDir != 0 ? CycleTarget(true, beamDir) : -1);
  const int visorDir = out.Pressed(Action::VisorNext) ? 1 : out.Pressed(Action::VisorPrev) ? -1 : 0;
  PushCyclePick(false, visorDir != 0 ? CycleTarget(false, visorDir) : -1);
  if (out.Pressed(Action::SpringBall)) PortDebug::RequestSpringBall();
  if (out.Pressed(Action::PortMenu)) PortDebug::RequestToggle();
  if (out.Pressed(Action::SaveState)) PortDebug::RequestSaveStateHotkey(1);
  if (out.Pressed(Action::LoadState)) PortDebug::RequestSaveStateHotkey(2);
  if (out.Pressed(Action::Screenshot)) aurora::request_screenshot();
  if (out.Pressed(Action::ToggleOriginal)) PortDebug::SetOriginalExperience(!PortDebug::OriginalExperience());
}

u8 Trigger(float analog, bool click) {
  if (click) return 180;
  return static_cast< u8 >(std::clamp(std::lround(analog * 32767.f) / 128L, 0L, 255L));
}

} // namespace

namespace {

void PublishLocked(PortInput::UserBindings bindings) {
  sUser = std::make_shared<const PortInput::UserBindings>(std::move(bindings));
  ++sUserVersion;
}

// Loads controls.toml on first use and reloads it when it changed. Call with
// sUserMutex held.
void RefreshLocked() {
  const auto now = std::chrono::steady_clock::now();
  if (sUserLoaded && now - sUserChecked < std::chrono::seconds(1)) return;
  sUserChecked = now;
  if (!sUserLoaded) InstallKeyNames();
  const bool first = !sUserLoaded;
  sUserLoaded = true;
  const std::string path = UserBindingsPath();
  std::error_code ec;
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    // No file (or it was deleted): no user bindings.
    if (sUserTime != std::filesystem::file_time_type{}) {
      sUserTime = {};
      PublishLocked({});
    }
    return;
  }
  if (!first && time == sUserTime) return;
  sUserTime = time;
  std::ifstream file(path, std::ios::binary);
  if (!file) return;
  std::ostringstream text;
  text << file.rdbuf();
  PortInput::UserBindings parsed;
  std::string error;
  int dropped = 0;
  if (PortInput::ParseUserBindings(text.str(), parsed, &error, &dropped)) {
    PortLog::Write("port: controls.toml loaded (%zu profiles)\n", parsed.profiles.size());
    if (dropped > 0)
      PortLog::Write("port: controls.toml: skipped %d beam_shift row(s); beams are now default shift+D-pad chords\n", dropped);
    PublishLocked(std::move(parsed));
  } else {
    // A half-finished hand edit keeps the bindings that were loaded before it.
    PortLog::Write("port: controls.toml ignored, keeping the last good bindings: %s\n", error.c_str());
  }
}

} // namespace

std::shared_ptr<const PortInput::UserBindings> UserBindings() {
  std::lock_guard lock(sUserMutex);
  RefreshLocked();
  return sUser;
}

uint64_t UserBindingsVersion() {
  std::lock_guard lock(sUserMutex);
  RefreshLocked();
  return sUserVersion;
}

bool SetUserBindings(const PortInput::UserBindings& bindings) {
  std::lock_guard lock(sUserMutex);
  RefreshLocked();
  const std::string path = UserBindingsPath();
  const std::string tmp = path + ".tmp";
  std::error_code ec;
  bool written = false;
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (file) {
      file << PortInput::SerializeUserBindings(bindings);
      written = static_cast< bool >(file.flush());
    }
  }
  if (written) std::filesystem::rename(tmp, path, ec);
  if (!written || ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  sUserTime = std::filesystem::last_write_time(path, ec);
  PublishLocked(bindings);
  return true;
}

void SetTouchControl(int control, bool down) {
  if (control < 0 || control >= kTouchControlCount) return;
  const uint64_t bit = uint64_t{1} << control;
  if (down) sTouchControls.fetch_or(bit, std::memory_order_relaxed);
  else sTouchControls.fetch_and(~bit, std::memory_order_relaxed);
}

// Read from the Android UI thread: the mask is published by the game thread's
// poll, so no SDL calls or file reads happen there.
bool TouchControlBound(int control) {
  if (control < 0 || control >= kTouchControlCount) return false;
  return (sTouchBound.load(std::memory_order_relaxed) >> control) & 1;
}

static uint64_t TouchBoundMask(const SActiveProfiles& active) {
  uint64_t mask = 0;
  for (const PortInput::UserProfile* p : {active.sel.base, active.sel.pad}) {
    if (p == nullptr) continue;
    for (const Binding& b : p->bindings) {
      for (int i = 0; i < b.count; ++i) {
        const Input& in = b.inputs[size_t(i)];
        if (in.device == Device::Touch && in.code < kTouchControlCount) mask |= uint64_t{1} << in.code;
      }
    }
  }
  return mask;
}

bool ActivePad(std::string& guid, std::string& type, std::string& name) {
  guid.clear();
  type.clear();
  name.clear();
  const s32 index = PADGetIndexForPort(kPort);
  SDL_Gamepad* pad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast< u32 >(index)) : nullptr;
  if (pad == nullptr) return false;
  char buf[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad)), buf, sizeof(buf));
  guid = buf;
  if (const char* t = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad))) type = t;
  if (const char* n = SDL_GetGamepadName(pad)) name = n;
  return true;
}

SActiveProfiles ActiveUserProfiles() {
  std::string guid;
  std::string type;
  std::string name;
  ActivePad(guid, type, name);
  SActiveProfiles active;
  active.bindings = UserBindings();
  active.sel = PortInput::Select(*active.bindings, guid, type);
  return active;
}

void ReadRaw(PortInput::RawState& raw) {
  raw = PortInput::RawState{};
  int numKeys = 0;
  if (const bool* keys = SDL_GetKeyboardState(&numKeys)) {
    for (int i = 0; i < numKeys && i < PortInput::kKeyCount; ++i) {
      if (keys[i]) raw.keys.set(static_cast< size_t >(i));
    }
  }
  PADRawState pad{};
  if (PADGetRawState(kPort, &pad)) {
    raw.padButtons = pad.buttons;
    for (int a = 0; a < PAD_RAW_AXIS_COUNT && a < PortInput::kPadAxisCount; ++a) {
      raw.padAxes[a] = std::clamp(static_cast< float >(pad.axes[a]) / 32767.f, -1.f, 1.f);
    }
  }
  raw.mouseButtons = (SDL_GetMouseState(nullptr, nullptr) & PortInputMap::kMouseButtonMask) << kMouseKeySlot;
  raw.touch = sTouchControls.load(std::memory_order_relaxed);
}

std::vector<SPadInfo> ConnectedPads() {
  std::vector<SPadInfo> pads;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids != nullptr && i < count; ++i) {
    if (SDL_IsJoystickVirtual(ids[i])) continue;
    SPadInfo info;
    char buf[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), buf, sizeof(buf));
    info.guid = buf;
    if (const char* t = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(ids[i]))) info.type = t;
    if (const char* n = SDL_GetGamepadNameForID(ids[i])) info.name = n;
    pads.push_back(std::move(info));
  }
  SDL_free(ids);
  return pads;
}

void BuildBaseProfile(Profile& out) {
  out = Profile{};
  BindController(out);
  BindKeyboard(out);
  BindPortControls(out);
}

void BuildProfile(Profile& out) {
  BuildBaseProfile(out);
  const SActiveProfiles user = ActiveUserProfiles();
  sTouchBound.store(TouchBoundMask(user), std::memory_order_relaxed);
  if (user.sel.base != nullptr) PortInput::Overlay(out, *user.sel.base);
  if (user.sel.pad != nullptr) PortInput::Overlay(out, *user.sel.pad);
}

PADStatus Synthesize(const PortInput::Output& out) {
  PADStatus s{};
  static constexpr unsigned kButtons[] = {PAD_BUTTON_A,    PAD_BUTTON_B,     PAD_BUTTON_X,    PAD_BUTTON_Y,
                                          PAD_TRIGGER_Z,   PAD_BUTTON_START, PAD_TRIGGER_L,   PAD_TRIGGER_R,
                                          PAD_BUTTON_UP,   PAD_BUTTON_DOWN,  PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT};
  for (unsigned b : kButtons) {
    if (out.Held(PadBitAction(b))) s.button |= static_cast< u16 >(b);
  }
  s.stickX = StickX(out.Value(Action::MainRight), out.Value(Action::MainLeft));
  s.stickY = StickY(out.Value(Action::MainDown), out.Value(Action::MainUp));
  s.substickX = StickX(out.Value(Action::CRight), out.Value(Action::CLeft));
  s.substickY = StickY(out.Value(Action::CDown), out.Value(Action::CUp));
  s.triggerLeft = Trigger(out.Value(Action::LAnalog), (s.button & PAD_TRIGGER_L) != 0);
  s.triggerRight = Trigger(out.Value(Action::RAnalog), (s.button & PAD_TRIGGER_R) != 0);
  const bool any = s.button != 0 || s.stickX != 0 || s.stickY != 0 || s.substickX != 0 || s.substickY != 0 ||
                   s.triggerLeft != 0 || s.triggerRight != 0;
  // Idle: Aurora still reports the port present when a pad or the keyboard is.
  s.err = any ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
  return s;
}

SPoll Poll(unsigned mouseHeld, bool focused) {
  static Profile sProfile;
  static PortInput::Runtime sRuntime;
  static bool sInit = false;
  static double sClockMs = 0.0;

  Profile next;
  BuildProfile(next);
  if (!sInit || next != sProfile) {
    sProfile = std::move(next);
    sRuntime.SetProfile(&sProfile);
    sInit = true;
  }

  PortInput::RawState raw;
  FillRawState(raw, mouseHeld, focused);
  sClockMs += static_cast< double >(PortDebug::TickPeriod()) * 1000.0;
  const PortInput::Output& out = sRuntime.Poll(raw, GameContexts(), sClockMs);
  SPoll r;
  if (PortInputRemap::Capturing()) {
    // The F1 Remap page is recording an input: it reaches neither the game nor
    // the direct actions (a bound PortMenu would close the page mid-capture).
    r.status.err = PAD_ERR_NO_CONTROLLER;
    return r;
  }
  RouteDirectActions(out);

  r.status = Synthesize(out);
  return r;
}

} // namespace PortInputDevices
