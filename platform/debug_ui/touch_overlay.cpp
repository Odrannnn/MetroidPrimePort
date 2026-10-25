// iOS touch controls: the Android overlay's default layout (TouchControlsView.java), drawn with
// ImGui over the game and fed by an SDL event filter. Same geometry, the same virtual pad
// (touch_pad.h) and the same native calls the Java view makes through android_jni.cpp, so the
// game, its bindings and the F1 settings treat both alike. Not here yet: the layout editor, the
// classic and twin-stick layouts (C-stick, D-pad), map pinch and rotate, and wheel icons.
#include "debug_ui_internal.h"

#if defined(PORT_TOUCH_OVERLAY)

#include "port_input_devices.h"
#include "port_log.h"
#include "port_photo.h"
#include "port_pointer.h"
#include "port_prompts.h"
#include "port_savestate.h"
#include "touch_pad.h"

#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace {

constexpr float kPi = 3.14159265358979f;

// Gamepad buttons and axes, as the Java view sends them.
constexpr int kBtnSouth = 0, kBtnEast = 1, kBtnWest = 2, kBtnNorth = 3, kBtnStart = 6,
              kBtnRightShoulder = 10;
constexpr int kAxisLeftX = 0, kAxisLeftY = 1, kAxisTriggerL = 4, kAxisTriggerR = 5;

// Every control the overlay can hold. Ball* replace A, B, X, Y on the Morph Ball page.
enum Ctl {
  kA, kB, kX, kY, kBallBomb, kBallBoost, kBallPowerBomb, kBallSpider, kBallUnmorph,
  kTurbo, kL, kR, kZ, kStart, kMenu, kEye, kMap, kBack, kVisorWheel, kBeamWheel, kCtlCount
};

struct CtlDef {
  int code;   // touch binding code (TouchControlsView's C_*), -1 for none
  int button; // gamepad button, -1 for none
  int axis;   // trigger axis, -1 for none
};
constexpr CtlDef kDefs[kCtlCount] = {
    {3, kBtnSouth, -1},   {4, kBtnEast, -1},  {5, kBtnWest, -1}, {6, kBtnNorth, -1},
    {PortInputDevices::kTouchBallBase + 0, -1, -1}, {PortInputDevices::kTouchBallBase + 1, -1, -1},
    {PortInputDevices::kTouchBallBase + 2, -1, -1}, {PortInputDevices::kTouchBallBase + 3, -1, -1},
    {PortInputDevices::kTouchBallBase + 4, -1, -1},
    {-1, -1, -1},         {7, -1, kAxisTriggerL}, {8, -1, kAxisTriggerR}, {9, kBtnRightShoulder, -1},
    {12, kBtnStart, -1},  {13, -1, -1},       {15, -1, -1},      {14, -1, -1},
    {-1, -1, -1},         {10, -1, -1},       {11, -1, -1},
};

// Face buttons, anchored to A (GC_A_FROM_RIGHT, GC_A_Y): offsets and sizes in layout units.
struct FaceDef {
  Ctl ctl;
  bool kidney;
  float x, y, r;                 // round: centre offset and radius
  float ring, half, start, sweep; // kidney: around A, degrees
  uint32_t rgb;
  const char* letter;
  const char* label;
};
constexpr uint32_t kGreen = 0x2FA864, kRed = 0xC8343A, kGrey = 0x8A8A94, kPurple = 0x6A4FB0;
constexpr FaceDef kFace[] = {
    {kA, false, 0.f, 0.f, 0.085f, 0, 0, 0, 0, kGreen, "A", "Fire"},
    {kB, false, -0.123f, 0.103f, 0.050f, 0, 0, 0, 0, kRed, "B", "Jump"},
    {kX, true, 0, 0, 0, 0.158f, 0.040f, -37.f, 55.f, kGrey, "X", "Morph"},
    {kY, true, 0, 0, 0, 0.158f, 0.040f, -150.f, 55.f, kGrey, "Y", "Missile"},
};
constexpr FaceDef kBallFace[] = {
    {kBallBomb, false, 0.f, 0.f, 0.085f, 0, 0, 0, 0, kGreen, "A", "Bomb"},
    {kBallBoost, false, -0.123f, 0.103f, 0.050f, 0, 0, 0, 0, kRed, "B", "Boost"},
    {kBallPowerBomb, false, -0.085f, -0.133f, 0.055f, 0, 0, 0, 0, kGrey, "Y", "P.Bomb"},
    {kBallSpider, false, 0.05f, -0.27f, 0.050f, 0, 0, 0, 0, kGrey, "R", "Spider"},
    {kBallUnmorph, false, 0.156f, -0.026f, 0.055f, 0, 0, 0, 0, kGrey, "X", "Unmorph"},
};
constexpr FaceDef kTurboDef = {kTurbo, false, 0.05f, -0.27f, 0.045f, 0, 0, 0, 0, kGrey, "T", "Turbo"};

// Wheel sector (up, right, down, left) -> visor or beam index, and the sector labels.
constexpr int kWheelItems[2][4] = {{0, 1, 3, 2}, {0, 2, 1, 3}};
constexpr const char* kWheelLabels[2][4] = {{"Combat", "X-Ray", "Thermal", "Scan"},
                                            {"Power", "Wave", "Ice", "Plasma"}};

enum class Mode { PassThrough, Pointer, Minimal, Full };

struct Rect {
  float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
  bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
  float Cx() const { return (x0 + x1) * 0.5f; }
  float Cy() const { return (y0 + y1) * 0.5f; }
};

struct Finger {
  enum Type { kControl, kStick, kAim, kWheel, kMapPan, kMinimap, kSwallow } type = kSwallow;
  Ctl ctl = kCtlCount;
  float x = 0.f, y = 0.f;   // latest position
  float x0 = 0.f, y0 = 0.f; // where it went down (or started aiming)
  Uint64 t0 = 0;
  bool aiming = false; // a held A, B or Turbo slid into aiming
  bool swiped = false;
};

// Everything below is guarded by sMutex: the filter runs inside SDL's event pump, the draw in
// the frame, both on the main thread today, but nothing here relies on it.
std::recursive_mutex sMutex;
bool sInstalled = false;
PortTouchPad::Pad sPad;
int sHeld[kCtlCount] = {};
std::map< SDL_FingerID, Finger > sFingers;
bool sAimTaken = false;
Mode sMode = Mode::PassThrough;
bool sHidden = false; // physical input or the eye button; the next touch brings it back

// Layout, rebuilt every frame (pixels of the ImGui main viewport).
struct Layout {
  bool valid = false;
  float w = 0.f, h = 0.f, dp = 1.f, u = 1.f;
  float side = 0.f, bottomInset = 0.f, buttonInset = 0.f, stickInset = 0.f;
  float ax = 0.f, ay = 0.f; // A's centre: the face cluster's anchor
  Rect face;                // union of the face shapes
  float stickX = 0.f, stickY = 0.f, stickR = 0.f;
  Rect pillL, pillR, pillZ;
  bool showR = false, showZ = false;
  Rect start, menu, eye, map;
  bool showMap = false;
  bool minimapValid = false;
  Rect minimap;
  float wheelR = 0.f, wheelY = 0.f, visorX = 0.f, beamX = 0.f;
  bool showWheels = false;
  uint32_t wheelState = 0;
  bool morphed = false, mapOpen = false;
  bool turbo = false, labels = true, colours = false, floating = true;
};
Layout sL;

// The floating stick's centre while a finger holds it.
float sStickCx = 0.f, sStickCy = 0.f;
float sStickVx = 0.f, sStickVy = 0.f;
SDL_FingerID sStickFinger = 0;
bool sStickHeld = false;

// An open wheel.
struct Wheel {
  bool open = false;
  int which = 0; // 0 visor, 1 beam
  float cx = 0.f, cy = 0.f;
  SDL_FingerID finger = 0;
};
Wheel sWheel;

PortTouchPad::Pad& Pad() {
  if (!sPad.ok()) {
    sPad = PortTouchPad::Attach();
    PortLog::Write("touchpad: attached id=%d open=%d\n", sPad.id, sPad.ok() ? 1 : 0);
    if (sPad.ok()) {
      // A trigger rests at the axis minimum; 0 would read as half pressed.
      SDL_SetJoystickVirtualAxis(sPad.handle, kAxisTriggerL, PortTouchPad::AxisValue(-1.f));
      SDL_SetJoystickVirtualAxis(sPad.handle, kAxisTriggerR, PortTouchPad::AxisValue(-1.f));
    } else {
      PortLog::Write("touchpad: attach failed: %s\n", SDL_GetError());
    }
  }
  return sPad;
}

void SetButton(int button, bool down) {
  if (SDL_Joystick* pad = Pad().handle) {
    SDL_SetJoystickVirtualButton(pad, button, down);
  }
}

void SetAxis(int axis, float value) {
  if (SDL_Joystick* pad = Pad().handle) {
    SDL_SetJoystickVirtualAxis(pad, axis, PortTouchPad::AxisValue(value));
  }
}

// Refcounted, as the Java view's held map: two fingers on one button release it once.
void Press(Ctl ctl, bool down) {
  int& count = sHeld[ctl];
  if (down ? count++ > 0 : (count == 0 || --count > 0)) {
    return;
  }
  const CtlDef& def = kDefs[ctl];
  if (def.code >= 0) {
    PortInputDevices::SetTouchControl(def.code, down);
  }
  switch (ctl) {
  case kMenu:
    if (down) PortDebug::RequestToggle();
    return;
  case kTurbo:
    sTouchTurboFire.store(down, std::memory_order_release);
    return;
  case kEye:
    if (down) sHidden = true;
    return;
  case kMap:
    if (down && PortDebug::TouchMapTap()) PortDebug::RequestMapTap();
    return;
  case kBack:
    if (down) PortPointer::ScriptBack();
    return;
  case kVisorWheel:
  case kBeamWheel:
    return;
  default:
    break;
  }
  if (def.code >= 0 && PortInputDevices::TouchControlBound(def.code)) {
    return; // a touch: binding owns it
  }
  if (def.button >= 0) {
    SetButton(def.button, down);
  } else if (def.axis >= 0) {
    SetAxis(def.axis, down ? 1.f : -1.f);
  }
}

void SetStick(float vx, float vy) {
  sStickVx = vx;
  sStickVy = vy;
  SetAxis(kAxisLeftX, vx);
  SetAxis(kAxisLeftY, vy);
}

// Lets a finger go. `act` false drops a wheel or minimap tap instead of acting on it.
void EndFinger(SDL_FingerID id, Finger& f, bool act) {
  if (f.aiming) {
    sAimTaken = false;
    PortDebug::SetTouchAimDown(false);
  }
  switch (f.type) {
  case Finger::kControl:
    Press(f.ctl, false);
    break;
  case Finger::kStick:
    if (sStickHeld && sStickFinger == id) {
      sStickHeld = false;
      SetStick(0.f, 0.f);
    }
    break;
  case Finger::kWheel:
    if (sWheel.open && sWheel.finger == id) {
      sWheel.open = false;
      Press(f.ctl, false);
      if (!act) break;
      const float dx = f.x - sWheel.cx, dy = f.y - sWheel.cy;
      const float moved = std::hypot(f.x - f.x0, f.y - f.y0);
      const uint32_t state = sL.wheelState;
      if (sWheel.which == 0 && SDL_GetTicks() - f.t0 < 250 && moved < 12.f * sL.dp &&
          PortDebug::TouchVisorTapScan() && (state & (1u << 2)) != 0) {
        PortDebug::RequestVisor(2); // a quick tap on Visor scans
      } else if (std::hypot(dx, dy) > 30.f * sL.dp) {
        const int sector = std::abs(dx) > std::abs(dy) ? (dx > 0.f ? 1 : 3) : (dy < 0.f ? 0 : 2);
        const int item = kWheelItems[sWheel.which][sector];
        if ((state & (1u << (sWheel.which * 4 + item))) != 0) {
          if (sWheel.which == 0) {
            PortDebug::RequestVisor(item);
          } else {
            PortDebug::RequestBeam(item);
          }
        }
      }
    }
    break;
  case Finger::kMinimap:
    if (act && std::hypot(f.x - f.x0, f.y - f.y0) < 12.f * sL.dp && PortDebug::TouchMapTap()) {
      PortDebug::RequestMapTap();
    }
    break;
  default:
    break;
  }
}

void ReleaseAll() {
  for (auto& [id, f] : sFingers) {
    EndFinger(id, f, false);
  }
  sFingers.clear();
  for (int i = 0; i < kCtlCount; ++i) {
    if (sHeld[i] > 0) {
      sHeld[i] = 1;
      Press(static_cast< Ctl >(i), false);
    }
  }
  sWheel.open = false;
  sAimTaken = false;
}

Mode CurrentMode() {
  PortSaveState::ResumeInfo info;
  bool yes = true;
  if (!sL.valid || PortDebug::OverlayVisible() || PortPhoto::Active() ||
      PortSaveState::ResumePromptShown(info, yes)) {
    return Mode::PassThrough;
  }
  if (PortPointer::ScreenActive()) {
    return Mode::Pointer;
  }
  return PortDebug::TouchControlsEnabled() ? Mode::Full : Mode::Minimal;
}

void UpdateMode() {
  const Mode mode = CurrentMode();
  if (mode != sMode) {
    ReleaseAll();
    sMode = mode;
  }
}

// Kidney geometry: the band's angles in radians and the ring centre (A's centre).
void KidneyAngles(const FaceDef& d, float& a0, float& a1) {
  a0 = d.start * kPi / 180.f;
  a1 = (d.start + d.sweep) * kPi / 180.f;
}

bool InFace(const FaceDef& d, float x, float y) {
  const float u = sL.u;
  if (!d.kidney) {
    const float cx = sL.ax + d.x * u, cy = sL.ay + d.y * u;
    return std::hypot(x - cx, y - cy) <= d.r * u;
  }
  float a0, a1;
  KidneyAngles(d, a0, a1);
  const float dx = x - sL.ax, dy = y - sL.ay;
  const float dist = std::hypot(dx, dy);
  const float ring = d.ring * u, half = d.half * u;
  float a = std::atan2(dy, dx);
  while (a < a0) a += 2.f * kPi;
  if (a <= a1 && std::abs(dist - ring) <= half) {
    return true;
  }
  for (const float end : {a0, a1}) {
    if (std::hypot(x - (sL.ax + ring * std::cos(end)), y - (sL.ay + ring * std::sin(end))) <= half) {
      return true;
    }
  }
  return false;
}

void GrowFace(Rect& r, bool& any, float x0, float y0, float x1, float y1) {
  if (!any) {
    r = {x0, y0, x1, y1};
    any = true;
    return;
  }
  r.x0 = std::min(r.x0, x0);
  r.y0 = std::min(r.y0, y0);
  r.x1 = std::max(r.x1, x1);
  r.y1 = std::max(r.y1, y1);
}

Rect FaceBounds() {
  Rect r;
  bool any = false;
  const float u = sL.u;
  for (const FaceDef& d : kFace) {
    if (!d.kidney) {
      const float cx = sL.ax + d.x * u, cy = sL.ay + d.y * u, rr = d.r * u;
      GrowFace(r, any, cx - rr, cy - rr, cx + rr, cy + rr);
      continue;
    }
    float a0, a1;
    KidneyAngles(d, a0, a1);
    const float ring = d.ring * u, half = d.half * u;
    for (int i = 0; i <= 8; ++i) {
      const float a = a0 + (a1 - a0) * static_cast< float >(i) / 8.f;
      const float px = sL.ax + ring * std::cos(a), py = sL.ay + ring * std::sin(a);
      GrowFace(r, any, px - half, py - half, px + half, py + half);
    }
  }
  return r;
}

void BuildLayout() {
  Layout& L = sL;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  L.w = vp->Size.x;
  L.h = vp->Size.y;
  SDL_Window* window = PortDebug::MainWindow();
  int pointsW = 0, pointsH = 0;
  if (window == nullptr || !SDL_GetWindowSize(window, &pointsW, &pointsH) || pointsW <= 0 ||
      L.w <= 0.f || L.h <= 0.f) {
    L.valid = false;
    return;
  }
  L.valid = true;
  L.dp = L.w / static_cast< float >(pointsW); // one dp is one iOS point
  const float dp = L.dp;
  // The notch, rounded corners and the home indicator.
  SDL_Rect safe{0, 0, pointsW, pointsH};
  SDL_GetWindowSafeArea(window, &safe);
  const float insetSide = static_cast< float >(std::max(safe.x, pointsW - safe.x - safe.w)) * dp;
  L.bottomInset = static_cast< float >(std::max(0, pointsH - safe.y - safe.h)) * dp;
  L.u = std::min(L.h, dp * 520.f);
  const float u = L.u, w = L.w, h = L.h, s = u / h;
  L.side = PortDebug::TouchSideMarginDp() * dp + insetSide;
  L.buttonInset = PortDebug::TouchButtonInsetDp() * dp;
  L.stickInset = PortDebug::TouchStickInsetDp() * dp;
  auto layoutX = [&](float f) {
    return f < 0.5f ? L.side + f * w * s : f * w + (1.f - f) * w * (1.f - s) - L.side;
  };
  auto layoutY = [&](float f) { return f < 0.5f ? (h - u) + f * u : f * h + (1.f - f) * (h - u); };

  L.ax = w - L.side - L.buttonInset - 0.235f * u;
  L.ay = layoutY(0.700f);
  L.face = FaceBounds();
  L.stickR = 0.16f * u;
  L.stickX = (w - L.face.x1 - L.buttonInset) + L.stickInset + L.stickR;
  L.stickY = L.face.Cy();

  L.wheelState = PortDebug::WheelState();
  L.morphed = (L.wheelState & (1u << 13)) != 0;
  L.mapOpen = PortDebug::MapScreenOpen();
  L.showWheels = (L.wheelState & (1u << 12)) != 0 && !L.mapOpen && !L.morphed;
  L.turbo = PortDebug::TouchTurboFlag() && !L.morphed;
  L.labels = PortDebug::TouchLabelsFlag();
  L.colours = PortDebug::TouchColorsFlag();
  L.floating = PortDebug::TouchFloatingStickFlag();

  // Pills: L Lock, R Look (morph ball, pause and map only), Z Map (without minimap taps).
  L.showR = L.morphed || PortDebug::PauseScreenOpen() || L.mapOpen;
  L.showZ = !PortDebug::TouchMapTap();
  L.pillL = {layoutX(0.020f), layoutY(0.030f), layoutX(0.150f), layoutY(0.190f)};
  L.pillR = {layoutX(0.850f), layoutY(0.030f), layoutX(0.980f), layoutY(0.190f)};
  L.pillZ = {layoutX(0.870f), layoutY(0.205f), layoutX(0.960f), layoutY(0.265f)};
  if (u < h && L.showWheels) {
    // A tall screen: bring the pills down to the thumbs, dp(12) above the cluster.
    const float lowest = L.showZ ? L.pillZ.y1 : L.pillL.y1;
    const float shift =
        std::max(0.f, std::min(L.stickY - L.stickR, L.face.y0) - 12.f * dp - lowest);
    for (Rect* r : {&L.pillL, &L.pillR, &L.pillZ}) {
      r->y0 += shift;
      r->y1 += shift;
    }
  }

  // Bottom corners: START, MENU on the left; eye, map on the right.
  const float d = 2.f * 22.f * dp;
  const float bottom = h - 12.f * dp - L.bottomInset;
  auto corner = [&](bool right, int slot) {
    const float fromSide = L.side + 28.f * dp + static_cast< float >(slot) * (d + 8.f * dp);
    return right ? Rect{w - fromSide - d, bottom - d, w - fromSide, bottom}
                 : Rect{fromSide, bottom - d, fromSide + d, bottom};
  };
  L.start = corner(false, 0);
  L.menu = corner(false, 1);
  L.eye = corner(true, 0);
  L.map = corner(true, 1);
  float mini[4] = {};
  bool drawn = true;
  L.minimapValid = PortDebug::TouchMapTap() && PortDebug::MinimapRect(mini, &drawn);
  L.showMap = PortDebug::TouchMapTap() && ((L.minimapValid && !drawn) || L.mapOpen);
  L.minimapValid = L.minimapValid && drawn;
  L.minimap = {mini[0] * w, mini[1] * h, mini[2] * w, mini[3] * h};

  // Wheel buttons: Visor right of the stick, Beam left of the face cluster.
  L.wheelR = 0.072f * u;
  const float edge = 16.f * dp + L.wheelR;
  const float lo = 112.f * dp, hi = w - 112.f * dp;
  L.visorX = std::clamp(L.stickX + L.stickR + edge, lo, std::max(lo, hi));
  L.beamX = std::clamp(L.face.x0 - edge, lo, std::max(lo, hi));
  L.wheelY = h - 112.f * dp - 8.f * dp - L.bottomInset;
}

bool InCircle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) <= r; }

const FaceDef* FaceAt(float x, float y) {
  if (sL.morphed) {
    for (const FaceDef& d : kBallFace) {
      if (InFace(d, x, y)) return &d;
    }
    return nullptr;
  }
  for (const FaceDef& d : kFace) {
    if (InFace(d, x, y)) return &d;
  }
  if (sL.turbo && InFace(kTurboDef, x, y)) {
    return &kTurboDef;
  }
  return nullptr;
}

void StartAim(Finger& f) {
  f.aiming = true;
  f.x0 = f.x;
  f.y0 = f.y;
  f.t0 = SDL_GetTicks();
  sAimTaken = true;
  PortDebug::SetTouchAimDown(true);
}

// A finger lands: claim it for a control, the stick, a wheel or aiming. Returns false when the
// finger is left to the game (pointer menus, or a free area with the controls off).
bool FingerDown(SDL_FingerID id, float x, float y) {
  const Layout& L = sL;
  Finger f;
  f.x = f.x0 = x;
  f.y = f.y0 = y;
  f.t0 = SDL_GetTicks();
  auto hold = [&](Ctl ctl) {
    f.type = Finger::kControl;
    f.ctl = ctl;
    Press(ctl, true);
    sFingers[id] = f;
    return true;
  };
  if (sMode == Mode::Pointer) {
    if (L.start.Contains(x, y)) return hold(kBack);
    if (L.menu.Contains(x, y)) return hold(kMenu);
    return false;
  }
  sTouchActive.store(true, std::memory_order_release);
  PortPrompts::NoteTouchInput(false);
  if (sMode == Mode::Minimal) {
    if (L.start.Contains(x, y)) return hold(kStart);
    if (L.menu.Contains(x, y)) return hold(kMenu);
    return false;
  }
  if (sHidden) {
    sHidden = false; // this touch only brings the controls back
    sFingers[id] = f;
    return true;
  }
  if (L.pillL.Contains(x, y)) return hold(kL);
  if (L.showR && L.pillR.Contains(x, y)) return hold(kR);
  if (L.showZ && L.pillZ.Contains(x, y)) return hold(kZ);
  if (const FaceDef* d = FaceAt(x, y)) return hold(d->ctl);
  if (L.start.Contains(x, y)) return hold(kStart);
  if (L.menu.Contains(x, y)) return hold(kMenu);
  if (L.eye.Contains(x, y)) return hold(kEye);
  if (L.showMap && L.map.Contains(x, y)) return hold(kMap);
  if (L.showWheels) {
    for (int which = 0; which < 2; ++which) {
      const float cx = which == 0 ? L.visorX : L.beamX;
      if (InCircle(x, y, cx, L.wheelY, L.wheelR * 1.1f)) {
        // A hit swallows the finger even when the wheel is already open for another.
        f.type = Finger::kWheel;
        f.ctl = which == 0 ? kVisorWheel : kBeamWheel;
        if (!sWheel.open) {
          Press(f.ctl, true);
          const float outer = 112.f * L.dp;
          sWheel = {true, which, std::clamp(cx, outer, std::max(outer, L.w - outer)), L.wheelY, id};
        } else {
          f.type = Finger::kSwallow;
        }
        sFingers[id] = f;
        return true;
      }
    }
  }
  if (L.minimapValid && L.minimap.Contains(x, y)) {
    f.type = Finger::kMinimap;
    sFingers[id] = f;
    return true;
  }
  if (!sStickHeld) {
    const bool floating = L.floating && !L.mapOpen && x < L.w * 0.5f;
    if (floating || InCircle(x, y, L.stickX, L.stickY, L.stickR * 1.5f)) {
      sStickHeld = true;
      sStickFinger = id;
      sStickCx = floating ? std::clamp(x, L.stickR, std::max(L.stickR, L.w - L.stickR)) : L.stickX;
      sStickCy = floating ? y : L.stickY;
      f.type = Finger::kStick;
      sFingers[id] = f;
      return true;
    }
  }
  if (L.mapOpen) {
    f.type = Finger::kMapPan;
  } else if (!sAimTaken && PortDebug::TouchAim()) {
    f.type = Finger::kAim;
    StartAim(f);
  }
  sFingers[id] = f;
  return true;
}

void FingerMove(Finger& f, SDL_FingerID id, float x, float y) {
  const float dx = x - f.x, dy = y - f.y;
  f.x = x;
  f.y = y;
  const float dp = sL.dp;
  if (f.type == Finger::kStick && sStickHeld && sStickFinger == id) {
    float vx = (x - sStickCx) / sL.stickR, vy = (y - sStickCy) / sL.stickR;
    const float len = std::hypot(vx, vy);
    if (len > 1.f) {
      vx /= len;
      vy /= len;
    }
    if (len < 0.12f) {
      vx = vy = 0.f;
    }
    SetStick(vx, vy);
    return;
  }
  if (f.type == Finger::kMapPan) {
    PortDebug::AddMapPan(dx / dp, dy / dp, sL.h / dp);
    return;
  }
  if (f.type == Finger::kControl && !f.aiming && !sAimTaken && PortDebug::TouchAim() &&
      (f.ctl == kA || f.ctl == kB || f.ctl == kTurbo) &&
      std::hypot(x - f.x0, y - f.y0) > 12.f * dp) {
    StartAim(f); // slide off Fire or Jump to aim while it stays held
    return;
  }
  if (f.aiming) {
    PortDebug::AddTouchAim(dx / dp, dy / dp);
    const float up = f.y0 - y;
    if (!f.swiped && SDL_GetTicks() - f.t0 <= 150 && up >= 40.f * dp && up > std::abs(x - f.x0)) {
      f.swiped = true;
      PortDebug::TouchSpringSwipe();
    }
  }
}

bool SDLCALL TouchFilter(void*, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    // Touches also arrive as a mouse; only the F1 overlay and the photo panel want those.
    const SDL_MouseID which =
        event->type == SDL_EVENT_MOUSE_MOTION ? event->motion.which : event->button.which;
    if (which != SDL_TOUCH_MOUSEID) {
      return true;
    }
    std::lock_guard lock(sMutex);
    return sMode == Mode::PassThrough;
  }
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    break;
  default:
    return true;
  }
  std::lock_guard lock(sMutex);
  UpdateMode();
  const SDL_FingerID id = event->tfinger.fingerID;
  const float x = event->tfinger.x * sL.w, y = event->tfinger.y * sL.h;
  const auto it = sFingers.find(id);
  if (event->type == SDL_EVENT_FINGER_DOWN) {
    if (sMode == Mode::PassThrough) {
      return true;
    }
    if (it != sFingers.end()) {
      EndFinger(id, it->second, false);
      sFingers.erase(it);
    }
    return !FingerDown(id, x, y);
  }
  if (it == sFingers.end()) {
    // Not ours: a pointer-menu or pass-through finger, or one released by a mode change.
    return sMode != Mode::Full;
  }
  if (event->type == SDL_EVENT_FINGER_MOTION) {
    FingerMove(it->second, id, x, y);
  } else {
    it->second.x = x;
    it->second.y = y;
    EndFinger(id, it->second, event->type == SDL_EVENT_FINGER_UP);
    sFingers.erase(it);
  }
  return false;
}

// Drawing. Colours are the Java view's ARGB literals.
ImU32 Argb(uint32_t argb, float alpha) {
  const float a = static_cast< float >(argb >> 24) * alpha;
  return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, static_cast< int >(a + 0.5f));
}

ImU32 PadFill(uint32_t rgb, bool pressed, float alpha) {
  if (!pressed) {
    return Argb(0x99000000u | rgb, alpha);
  }
  auto lift = [](uint32_t c) { return c + (255 - c) * 2 / 5; };
  const uint32_t r = lift((rgb >> 16) & 0xFF), g = lift((rgb >> 8) & 0xFF), b = lift(rgb & 0xFF);
  return Argb(0xDD000000u | (r << 16) | (g << 8) | b, alpha);
}

constexpr uint32_t kRestFill = 0x77081218, kPressedFill = 0xCC48C8E8, kRestStroke = 0xBBFFFFFF,
                   kPressedStroke = 0xFFE1F8FF;

bool Pressed(Ctl ctl) {
  for (const auto& [id, f] : sFingers) {
    if (f.ctl == ctl && (f.type == Finger::kControl || f.type == Finger::kWheel)) return true;
  }
  return false;
}

void Text(ImDrawList* dl, float cx, float cy, float size, ImU32 col, const char* text) {
  ImFont* font = ImGui::GetFont();
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
  dl->AddText(font, size, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), col, text);
}

void KidneyPath(ImDrawList* dl, const FaceDef& d) {
  float a0, a1;
  KidneyAngles(d, a0, a1);
  const float ring = sL.u * d.ring, half = sL.u * d.half;
  const ImVec2 c(sL.ax, sL.ay);
  dl->PathArcTo(c, ring + half, a0, a1, 24);
  dl->PathArcTo(ImVec2(c.x + ring * std::cos(a1), c.y + ring * std::sin(a1)), half, a1, a1 + kPi, 12);
  dl->PathArcTo(c, ring - half, a1, a0, 24);
  dl->PathArcTo(ImVec2(c.x + ring * std::cos(a0), c.y + ring * std::sin(a0)), half, a0 + kPi,
                a0 + 2.f * kPi, 12);
}

void DrawFace(ImDrawList* dl, const FaceDef& d, float alpha) {
  const float u = sL.u, dp = sL.dp;
  const bool pressed = Pressed(d.ctl);
  const ImU32 fill = sL.colours ? PadFill(d.rgb, pressed, alpha)
                                : Argb(pressed ? kPressedFill : kRestFill, alpha);
  const ImU32 stroke = Argb(pressed ? kPressedStroke : kRestStroke, alpha);
  const ImU32 text = Argb(0xFFFFFFFF, alpha);
  float tx, ty, size;
  if (d.kidney) {
    KidneyPath(dl, d);
    dl->PathFillConcave(fill);
    KidneyPath(dl, d);
    dl->PathStroke(stroke, ImDrawFlags_Closed, 2.f * dp);
    float a0, a1;
    KidneyAngles(d, a0, a1);
    const float mid = (a0 + a1) * 0.5f;
    tx = sL.ax + d.ring * u * std::cos(mid);
    ty = sL.ay + d.ring * u * std::sin(mid);
    size = d.half * u * 1.1f;
  } else {
    tx = sL.ax + d.x * u;
    ty = sL.ay + d.y * u;
    dl->AddCircleFilled(ImVec2(tx, ty), d.r * u, fill, 40);
    dl->AddCircle(ImVec2(tx, ty), d.r * u, stroke, 40, 2.f * dp);
    size = d.r * u * 0.8f;
  }
  if (sL.labels) {
    Text(dl, tx, ty - size * 0.3f, size, text, d.letter);
    Text(dl, tx, ty + size * 0.45f, std::max(10.f * dp, size * 0.42f), text, d.label);
  } else {
    Text(dl, tx, ty, size, text, d.letter);
  }
}

void DrawPill(ImDrawList* dl, const Rect& r, Ctl ctl, const char* letter, const char* label,
              uint32_t rgb, float alpha) {
  const bool pressed = Pressed(ctl);
  const ImU32 fill = rgb != 0 && sL.colours ? PadFill(rgb, pressed, alpha)
                                            : Argb(pressed ? kPressedFill : kRestFill, alpha);
  const float radius = std::min(r.x1 - r.x0, r.y1 - r.y0) * 0.28f;
  dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), fill, radius);
  dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), Argb(pressed ? kPressedStroke : kRestStroke, alpha),
              radius, 0, 2.f * sL.dp);
  char text[32];
  std::snprintf(text, sizeof(text), sL.labels ? "%s %s" : "%s", letter, label);
  Text(dl, r.Cx(), r.Cy(), std::min(16.f * sL.dp, (r.y1 - r.y0) * 0.45f), Argb(0xFFFFFFFF, alpha), text);
}

enum class Glyph { Start, Menu, Eye, Map, Back };

void DrawCorner(ImDrawList* dl, const Rect& r, Ctl ctl, Glyph glyph, float alpha) {
  const bool pressed = Pressed(ctl);
  const float dp = sL.dp, cx = r.Cx(), cy = r.Cy(), rad = (r.x1 - r.x0) * 0.5f;
  dl->AddCircleFilled(ImVec2(cx, cy), rad, Argb(pressed ? kPressedFill : kRestFill, alpha), 32);
  dl->AddCircle(ImVec2(cx, cy), rad, Argb(pressed ? kPressedStroke : kRestStroke, alpha), 32, 2.f * dp);
  const ImU32 ink = Argb(0xFFFFFFFF, alpha);
  const float g = rad * 0.45f, t = 2.f * dp;
  switch (glyph) {
  case Glyph::Start: // pause bars
    dl->AddRectFilled(ImVec2(cx - g * 0.6f, cy - g), ImVec2(cx - g * 0.15f, cy + g), ink);
    dl->AddRectFilled(ImVec2(cx + g * 0.15f, cy - g), ImVec2(cx + g * 0.6f, cy + g), ink);
    break;
  case Glyph::Menu: // cog
    dl->AddCircle(ImVec2(cx, cy), g * 0.62f, ink, 20, t * 1.4f);
    for (int i = 0; i < 8; ++i) {
      const float a = static_cast< float >(i) * kPi / 4.f;
      dl->AddLine(ImVec2(cx + std::cos(a) * g * 0.75f, cy + std::sin(a) * g * 0.75f),
                  ImVec2(cx + std::cos(a) * g * 1.1f, cy + std::sin(a) * g * 1.1f), ink, t * 1.6f);
    }
    break;
  case Glyph::Eye:
    dl->PathArcTo(ImVec2(cx, cy + g * 0.9f), g * 1.35f, -kPi * 0.82f, -kPi * 0.18f, 16);
    dl->PathArcTo(ImVec2(cx, cy - g * 0.9f), g * 1.35f, kPi * 0.18f, kPi * 0.82f, 16);
    dl->PathStroke(ink, ImDrawFlags_Closed, t);
    dl->AddCircleFilled(ImVec2(cx, cy), g * 0.32f, ink, 16);
    break;
  case Glyph::Map: {
    const ImVec2 pts[] = {{cx - g, cy - g * 0.7f}, {cx - g * 0.33f, cy - g}, {cx + g * 0.33f, cy - g * 0.7f},
                          {cx + g, cy - g},        {cx + g, cy + g * 0.7f},  {cx + g * 0.33f, cy + g},
                          {cx - g * 0.33f, cy + g * 0.7f}, {cx - g, cy + g}};
    dl->AddPolyline(pts, 8, ink, ImDrawFlags_Closed, t);
    dl->AddLine(pts[1], pts[6], ink, t);
    dl->AddLine(pts[2], pts[5], ink, t);
    break;
  }
  case Glyph::Back: {
    const ImVec2 pts[] = {{cx + g * 0.3f, cy - g}, {cx - g * 0.6f, cy}, {cx + g * 0.3f, cy + g}};
    dl->AddPolyline(pts, 3, ink, ImDrawFlags_None, t * 1.4f);
    break;
  }
  }
}

void DrawWheels(ImDrawList* dl, float alpha) {
  const Layout& L = sL;
  const uint32_t state = L.wheelState;
  const ImU32 ink = Argb(0xFFFFFFFF, alpha);
  for (int which = 0; which < 2; ++which) {
    const float cx = which == 0 ? L.visorX : L.beamX;
    const Ctl ctl = which == 0 ? kVisorWheel : kBeamWheel;
    const bool pressed = Pressed(ctl);
    dl->AddCircleFilled(ImVec2(cx, L.wheelY), L.wheelR, Argb(pressed ? kPressedFill : kRestFill, alpha), 32);
    dl->AddCircle(ImVec2(cx, L.wheelY), L.wheelR, Argb(pressed ? kPressedStroke : kRestStroke, alpha), 32,
                  2.f * L.dp);
    const int current = static_cast< int >((state >> (which == 0 ? 8 : 10)) & 3);
    const char* name = kWheelLabels[which][0];
    for (int sector = 0; sector < 4; ++sector) {
      if (kWheelItems[which][sector] == current) name = kWheelLabels[which][sector];
    }
    Text(dl, cx, L.wheelY - L.wheelR * 0.22f, L.wheelR * 0.42f, ink, which == 0 ? "Visor" : "Beam");
    Text(dl, cx, L.wheelY + L.wheelR * 0.3f, L.wheelR * 0.34f, ink, name);
  }
  if (!sWheel.open) {
    return;
  }
  // The open wheel: four sectors around the button, the hovered one lit.
  const auto it = sFingers.find(sWheel.finger);
  const float fx = it != sFingers.end() ? it->second.x : sWheel.cx;
  const float fy = it != sFingers.end() ? it->second.y : sWheel.cy;
  const float dx = fx - sWheel.cx, dy = fy - sWheel.cy;
  const float outer = 112.f * L.dp, dead = 30.f * L.dp;
  int hovered = -1;
  if (std::hypot(dx, dy) > dead) {
    hovered = std::abs(dx) > std::abs(dy) ? (dx > 0.f ? 1 : 3) : (dy < 0.f ? 0 : 2);
  }
  const int current = static_cast< int >((state >> (sWheel.which == 0 ? 8 : 10)) & 3);
  const ImVec2 c(sWheel.cx, sWheel.cy);
  for (int sector = 0; sector < 4; ++sector) {
    const int item = kWheelItems[sWheel.which][sector];
    const bool owned = (state & (1u << (sWheel.which * 4 + item))) != 0;
    const float mid = (sector == 0 ? -0.5f : sector == 1 ? 0.f : sector == 2 ? 0.5f : 1.f) * kPi;
    const float a0 = mid - kPi * 0.25f + 0.04f, a1 = mid + kPi * 0.25f - 0.04f;
    auto path = [&] {
      dl->PathArcTo(c, outer, a0, a1, 16);
      dl->PathArcTo(c, dead, a1, a0, 8);
    };
    path();
    dl->PathFillConcave(Argb(sector == hovered && owned ? 0xDD48C8E8 : owned ? 0xAA081218 : 0x66081218, 1.f));
    path();
    dl->PathStroke(item == current ? Argb(0xFFE0C020, 1.f) : Argb(kRestStroke, 1.f), ImDrawFlags_Closed,
                   (item == current ? 3.f : 1.5f) * L.dp);
    if (owned) {
      const float r = (outer + dead) * 0.5f;
      Text(dl, c.x + r * std::cos(mid), c.y + r * std::sin(mid), 14.f * L.dp, Argb(0xFFFFFFFF, 1.f),
           kWheelLabels[sWheel.which][sector]);
    }
  }
}

void DrawStick(ImDrawList* dl, float alpha) {
  const Layout& L = sL;
  const bool held = sStickHeld;
  if (!held && L.floating && !L.mapOpen) {
    return; // the floating stick shows only under a thumb
  }
  const float cx = held ? sStickCx : L.stickX, cy = held ? sStickCy : L.stickY;
  dl->AddCircleFilled(ImVec2(cx, cy), L.stickR, Argb(held ? 0x8848C8E8 : 0x66081218, alpha), 48);
  dl->AddCircle(ImVec2(cx, cy), L.stickR, Argb(held ? kPressedStroke : 0xAAFFFFFF, alpha), 48, 2.f * L.dp);
  const float kx = cx + sStickVx * L.stickR * 0.58f, ky = cy + sStickVy * L.stickR * 0.58f;
  dl->AddCircleFilled(ImVec2(kx, ky), L.stickR * 0.42f, Argb(held ? 0xCC48C8E8 : 0x77081218, alpha), 32);
  dl->AddCircle(ImVec2(kx, ky), L.stickR * 0.42f, Argb(kRestStroke, alpha), 32, 2.f * L.dp);
}

} // namespace

namespace PortDebug {

void InstallTouchOverlay() {
  std::lock_guard lock(sMutex);
  if (!sInstalled) {
    sInstalled = true;
    SDL_SetEventFilter(TouchFilter, nullptr);
  }
}

void DrawTouchOverlay() {
  std::lock_guard lock(sMutex);
  BuildLayout();
  UpdateMode();
  if (sPhysicalInput.exchange(false, std::memory_order_acq_rel) && sMode == Mode::Full) {
    ReleaseAll();
    sHidden = true; // a real pad or keyboard: get out of the way until the next touch
  }
  if (sMode == Mode::PassThrough) {
    return;
  }
  for (const auto& [id, f] : sFingers) {
    if (f.type == Finger::kMapPan) {
      PortDebug::AddMapPan(0.f, 0.f, sL.h / sL.dp); // keeps a held pan from timing out
      break;
    }
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
  if (sMode == Mode::Pointer) {
    DrawCorner(dl, sL.start, kBack, Glyph::Back, 1.f);
    DrawCorner(dl, sL.menu, kMenu, Glyph::Menu, 1.f);
    return;
  }
  if (sMode == Mode::Minimal) {
    DrawCorner(dl, sL.start, kStart, Glyph::Start, 1.f);
    DrawCorner(dl, sL.menu, kMenu, Glyph::Menu, 1.f);
    return;
  }
  if (sHidden) {
    return;
  }
  const float alpha = std::clamp(PortDebug::TouchOpacity() / 100.f, 0.f, 1.f);
  DrawPill(dl, sL.pillL, kL, "L", "Lock", 0, alpha);
  if (sL.showR) DrawPill(dl, sL.pillR, kR, "R", "Look", 0, alpha);
  if (sL.showZ) DrawPill(dl, sL.pillZ, kZ, "Z", "Map", kPurple, alpha);
  DrawStick(dl, alpha);
  if (sL.morphed) {
    for (const FaceDef& d : kBallFace) DrawFace(dl, d, alpha);
  } else {
    for (const FaceDef& d : kFace) DrawFace(dl, d, alpha);
    if (sL.turbo) DrawFace(dl, kTurboDef, alpha);
  }
  DrawCorner(dl, sL.start, kStart, Glyph::Start, alpha);
  DrawCorner(dl, sL.menu, kMenu, Glyph::Menu, std::max(alpha, 0.25f));
  DrawCorner(dl, sL.eye, kEye, Glyph::Eye, alpha);
  if (sL.showMap) DrawCorner(dl, sL.map, kMap, Glyph::Map, alpha);
  if (sL.showWheels) DrawWheels(dl, alpha);
}

} // namespace PortDebug

#endif // PORT_TOUCH_OVERLAY
