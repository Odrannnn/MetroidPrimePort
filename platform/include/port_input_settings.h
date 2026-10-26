#pragma once

// The input and controls settings (mouse aim, twin stick, touch layout, bindings,
// gyro, gameplay toggles): their values, the port_settings.ini lines that carry
// them, and the MP_* overrides. Plain data without SDL, so tests/port_input_settings.cpp
// links it alone. PortDebug:: keeps the getters and setters (Original experience and
// touch gating live there) and forwards to Get() / MutableSettings().

#include "port_input_map.h"

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

namespace PortInput {

// A second controller button per PAD button, indexed by the PAD bit's position.
constexpr int kPadAltCount = 16;
// Mouse crosshair size, percent.
constexpr int kCrosshairSizeMin = 25;
constexpr int kCrosshairSizeMax = 100;
constexpr int kCrosshairSizeDefault = 50;
// Pointer aim: dead box in percent of the half screen, edge turn speed in degrees/second.
constexpr int kPointerAimBoxMin = 20;
constexpr int kPointerAimBoxMax = 95;
constexpr int kPointerAimBoxDefault = 70;
constexpr int kPointerAimTurnMin = 30;
constexpr int kPointerAimTurnMax = 720;
constexpr int kPointerAimTurnDefault = 180;
// Gyro aim: camera turn per turn of the pad.
constexpr float kGyroSensitivityMin = 0.1f;
constexpr float kGyroSensitivityMax = 10.f;
// Gyro smoothing/tightening thresholds, deg/s.
constexpr float kGyroThresholdMax = 50.f;
// Gyro pointer aim: degrees of pad turn that sweep the pointer across the screen.
constexpr float kGyroCursorAngleMin = 5.f;
constexpr float kGyroCursorAngleMax = 180.f;
constexpr float kGyroCursorAngleDefault = 40.f;
// The Android touch overlay's margins, in dp.
constexpr float kTouchMarginMaxDp = 300.f;
constexpr float kTouchSideMarginDefault = 16.f;
constexpr float kTouchStickInsetDefault = 32.f;
constexpr float kTouchButtonInsetDefault = 0.f;
// The overlay's opacity in percent, and the range its slider takes.
constexpr float kTouchOpacityDefault = 100.f;
constexpr float kTouchOpacityMin = 10.f;
constexpr float kTouchOpacityMax = 100.f;
constexpr std::size_t kTouchLayoutMaxLen = 16384; // 64 added buttons fit (issue #92)
// SDL_SCANCODE_LSHIFT (SDL3), the default beam-shift key; debug_ui.cpp checks it.
constexpr int kScancodeLShift = 225;

struct Settings {
  bool mouseAim = false;
  bool twinStick = false;
  bool springBall = false;
  bool swapScanXray = false;
  bool fastMorph = false;
  bool lockOnToggle = false;
  bool stickyCharge = false;
  bool rapidCharge = false;
  bool remasteredMovement = true;
  bool springFlick = false;
  float springFlickRate = 6.f;
  bool springSwipe = true;  // Android: a quick swipe up on the touch aim area springs
  float stickAimRate = 900.f;
  // With stickAimSplit on, stickAimRateY drives vertical stick aim and
  // stickAimRate only the horizontal; off, stickAimRate drives both.
  bool stickAimSplit = false;
  float stickAimRateY = 900.f;
  // Gyro aiming: off / hold to aim / always / hold to pause / toggle to aim, auto / controller /
  // phone, and how far the camera turns per turn of the pad (1 = the same angle).
  int gyroMode = 0;
  int gyroSource = 0;
  float gyroSensitivity = 2.f;
  // With gyroSplit on, gyroSensitivityY drives the pad's pitch and gyroSensitivity only the yaw.
  bool gyroSplit = false;
  float gyroSensitivityY = 2.f;
  // Which turn of the pad steers yaw (PortGyro::Space: 0 local yaw, 1 player,
  // 2 world, 3 local roll, 4 local yaw and roll); and whether the gyro's drift
  // is measured while the pad is still.
  int gyroSpace = 1;
  // Auto calibration: 0 off, 1 always, 2 only in menus (paused, a frontend screen or F1 open).
  int gyroAutoCalibrate = 1;
  // Gyro smoothing and tightening thresholds in deg/s (0 = off; see PortGyro::Filter).
  float gyroSmoothing = 0.f;
  float gyroTightening = 0.f;
  // With pointer aim: how far the pad turns to sweep the pointer across the
  // screen (degrees), and whether the gyro also turns the camera as in direct aim.
  float gyroCursorAngle = kGyroCursorAngleDefault;
  bool gyroCursorCamera = false;
  // The "Reset aim" action recentres the pointer under pointer aim and levels
  // the camera without it; on, it does both either way.
  bool resetAimBoth = false;
  // An old gyro_rate line (aim pixels per rad/s, scaled by the mouse sensitivity)
  // and whether gyro_sensitivity was read since the last PostLoad, which turns the
  // first into the second when only it was there.
  float gyroLegacyRate = 0.f;
  bool gyroSensitivityRead = false;
  // Android touch overlay.
  bool touchColors = false;        // the GameCube pad's colours
  bool touchLabels = true;         // each button's function under its letter
  bool touchTurbo = false;         // a Turbo button beside Fire
  bool touchFloatingStick = true;  // the left stick appears where the left half is touched
  bool touchAim = true;
  float touchAimSpeed = 2.25f;
  // With touchAimSplit on, touchAimSpeedY drives vertical touch aim and touchAimSpeed only the horizontal.
  bool touchAimSplit = false;
  float touchAimSpeedY = 2.25f;
  bool touchMapTap = true;
  bool touchWheels = true;
  bool touchClassic = false;       // classic GameCube layout
  bool touchTwinStick = false;     // exclusive with touchClassic; classic wins on load
  bool touchVisorTapScan = false;
  // Read from the Android UI thread.
  std::atomic< float > touchSideMargin{kTouchSideMarginDefault};
  std::atomic< float > touchStickInset{kTouchStickInsetDefault};
  std::atomic< float > touchButtonInset{kTouchButtonInsetDefault};
  // How visible the gameplay controls are, in percent; the Android view reads it
  // each draw, so it is atomic like the margins.
  std::atomic< float > touchOpacity{kTouchOpacityDefault};
  // Off, the Android view draws and handles only its MENU and START buttons.
  std::atomic< bool > touchControls{true};
  bool mouseInvertX = false;
  bool mouseInvertY = false;
  bool mouseButtons = true;
  bool mouseCrosshair = true;
  int crosshairSize = kCrosshairSizeDefault;
  bool pointerAim = false; // the mouse moves a pointer; the view turns past the dead box
  int pointerAimBox = kPointerAimBoxDefault;
  int pointerAimTurn = kPointerAimTurnDefault;
  // Lock-on free aim (Remastered): under a combat lock a pointer moves over the view and
  // shots follow it while the lock steers the view (any direct aim, with or without
  // pointer aim).
  bool lockOnFreeAim = false;
  // Radians per count; shown on the Source scale. Defaults to 3.0 there
  // (PortMouse::kDefaultSensitivity).
  float mouseSensitivity = 0.00115192f;
  // With mouseSensitivitySplit on, mouseSensitivityY (radians per count) drives vertical mouse
  // aim and mouseSensitivity only the horizontal.
  bool mouseSensitivitySplit = false;
  float mouseSensitivityY = 0.00115192f;
  int mouseDpi = 800;               // only for the cm/360 readout
  // What each mouse button does under mouse aim (PortInputMap::EMouseAction).
  int mouseActions[PortInputMap::kMouseButtonCount] = {
      PortInputMap::DefaultMouseAction(0), PortInputMap::DefaultMouseAction(1),
      PortInputMap::DefaultMouseAction(2), PortInputMap::DefaultMouseAction(3),
      PortInputMap::DefaultMouseAction(4)};
  // The beam shift: two keys or mouse buttons (scancode or PAD_KEY_MOUSE_*) and a
  // controller button (an SDL gamepad button or PAD_NATIVE_BUTTON_TRIGGER_*), -1 for none.
  int shiftBindings[3] = {kScancodeLShift, -1, -1};
  int turboBindings[3] = {-1, -1, -1}; // turbo fire: two keys, then a controller button
  int padAltButtons[kPadAltCount] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                     -1, -1, -1, -1, -1, -1, -1, -1};
};

Settings& MutableSettings();
const Settings& Get();

// Applies one settings-file line; false if the key isn't an input key.
bool ApplySetting(std::string_view key, std::string_view value);
// After a whole file is read: classic and twin-stick touch layouts are exclusive,
// and an old gyro_rate becomes gyro_sensitivity.
void PostLoad();
// The input keys' lines, in three runs that sit at their old places in the file:
// bindings and toggles, gameplay options, then aim/touch/mouse. WriteSettings is all three.
void WriteControlSettings(std::ostream& out);
void WriteGameplaySettings(std::ostream& out);
void WriteAimSettings(std::ostream& out);
void WriteSettings(std::ostream& out);
// MP_* variables for a single run; they win over the file.
void ApplyEnvOverrides();

// The touch overlay's per-control offsets (an opaque string the Java view parses),
// shared between the UI thread and the game thread. Set refuses an over-long layout;
// the settings file also drops one that isn't printable ASCII.
std::string TouchLayout();
bool SetTouchLayout(std::string_view layout);

} // namespace PortInput
