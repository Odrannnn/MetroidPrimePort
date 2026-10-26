#include "port_input_settings.h"

#include "port_env.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <ostream>

namespace PortInput {
namespace {

std::mutex sLayoutMutex;
std::string sLayout;

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

// The mouse button a settings key such as "mouse_left" names, or -1.
int MouseButtonSetting(const std::string& key) {
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    if (key == PortInputMap::MouseButtonKey(i)) {
      return i;
    }
  }
  return -1;
}

float ParseFloat(const std::string& value) { return static_cast< float >(std::atof(value.c_str())); }

// A number, or the value is ignored (atoi would read junk as scancode 0).
bool ParseCode(const std::string& value, int& out) {
  char* end = nullptr;
  const long code = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast< int >(code);
  return true;
}

} // namespace

Settings& MutableSettings() {
  static Settings settings;
  return settings;
}

const Settings& Get() { return MutableSettings(); }

bool ApplySetting(std::string_view keyView, std::string_view valueView) {
  const std::string key(keyView);
  const std::string value(valueView);
  Settings& s = MutableSettings();
  if (key == "mouse_aim") {
    s.mouseAim = ParseBool(value);
  } else if (key == "twin_stick") {
    s.twinStick = ParseBool(value);
  } else if (key == "touch_colors") {
    s.touchColors = ParseBool(value);
  } else if (key == "touch_labels") {
    s.touchLabels = ParseBool(value);
  } else if (key == "touch_turbo") {
    s.touchTurbo = ParseBool(value);
  } else if (key == "touch_floating_stick") {
    s.touchFloatingStick = ParseBool(value);
  } else if (key == "stick_aim_rate") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 50.f && f <= 10000.f) {
      s.stickAimRate = f;
    }
  } else if (key == "stick_aim_split") {
    s.stickAimSplit = ParseBool(value);
  } else if (key == "stick_aim_rate_y") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 50.f && f <= 10000.f) {
      s.stickAimRateY = f;
    }
  } else if (key == "gyro_mode") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 4) {
      s.gyroMode = static_cast< int >(v);
    }
  } else if (key == "gyro_source") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      s.gyroSource = static_cast< int >(v);
    }
  } else if (key == "gyro_space") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 4) {
      s.gyroSpace = static_cast< int >(v);
    }
  } else if (key == "gyro_auto_calibrate") {
    // 0/1 were the old on/off; 2 is only in menus.
    char* end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (end != value.c_str()) {
      if (v >= 0 && v <= 2) {
        s.gyroAutoCalibrate = static_cast< int >(v);
      }
    } else {
      s.gyroAutoCalibrate = ParseBool(value) ? 1 : 0;
    }
  } else if (key == "gyro_smoothing" || key == "gyro_tightening") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 0.f && f <= kGyroThresholdMax) {
      (key == "gyro_smoothing" ? s.gyroSmoothing : s.gyroTightening) = f;
    }
  } else if (key == "gyro_cursor_angle") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= kGyroCursorAngleMin && f <= kGyroCursorAngleMax) {
      s.gyroCursorAngle = f;
    }
  } else if (key == "gyro_cursor_camera") {
    s.gyroCursorCamera = ParseBool(value);
  } else if (key == "reset_aim_both") {
    s.resetAimBoth = ParseBool(value);
  } else if (key == "gyro_sensitivity") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= kGyroSensitivityMin && f <= kGyroSensitivityMax) {
      s.gyroSensitivity = f;
      s.gyroSensitivityRead = true;
    }
  } else if (key == "gyro_split") {
    s.gyroSplit = ParseBool(value);
  } else if (key == "gyro_sensitivity_y") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= kGyroSensitivityMin && f <= kGyroSensitivityMax) {
      s.gyroSensitivityY = f;
    }
  } else if (key == "gyro_rate") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 20.f && f <= 5000.f) {
      s.gyroLegacyRate = f;
    }
  } else if (key == "touch_aim") {
    s.touchAim = ParseBool(value);
  } else if (key == "touch_map_tap") {
    s.touchMapTap = ParseBool(value);
  } else if (key == "touch_classic_gc") {
    s.touchClassic = ParseBool(value);
  } else if (key == "touch_twin_stick") {
    s.touchTwinStick = ParseBool(value);
  } else if (key == "touch_wheels") {
    s.touchWheels = ParseBool(value);
  } else if (key == "touch_visor_tap_scan") {
    s.touchVisorTapScan = ParseBool(value);
  } else if (key == "touch_aim_speed") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 0.25f && f <= 10.f) {
      s.touchAimSpeed = f;
    }
  } else if (key == "touch_aim_split") {
    s.touchAimSplit = ParseBool(value);
  } else if (key == "touch_aim_speed_y") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 0.25f && f <= 10.f) {
      s.touchAimSpeedY = f;
    }
  } else if (key == "touch_side_margin" || key == "touch_stick_inset" ||
             key == "touch_button_inset") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 0.f && f <= kTouchMarginMaxDp) {
      (key == "touch_side_margin"   ? s.touchSideMargin
       : key == "touch_stick_inset" ? s.touchStickInset
                                    : s.touchButtonInset)
          .store(f);
    }
  } else if (key == "touch_controls") {
    s.touchControls.store(ParseBool(value));
  } else if (key == "touch_opacity") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= kTouchOpacityMin && f <= kTouchOpacityMax) {
      s.touchOpacity.store(f);
    }
  } else if (key == "touch_layout") {
    // Printable ASCII only: JNI's NewStringUTF aborts on invalid UTF-8.
    const bool ascii = std::all_of(value.begin(), value.end(),
                                   [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
    if (ascii) {
      SetTouchLayout(value);
    }
  } else if (key == "mouse_invert_x") {
    s.mouseInvertX = ParseBool(value);
  } else if (key == "mouse_invert_y") {
    s.mouseInvertY = ParseBool(value);
  } else if (key == "mouse_buttons") {
    s.mouseButtons = ParseBool(value);
  } else if (key == "mouse_crosshair") {
    s.mouseCrosshair = ParseBool(value);
  } else if (key == "pointer_aim") {
    s.pointerAim = ParseBool(value);
  } else if (key == "pointer_aim_box") {
    const int v = std::atoi(value.c_str());
    if (v >= kPointerAimBoxMin && v <= kPointerAimBoxMax) {
      s.pointerAimBox = v;
    }
  } else if (key == "pointer_aim_turn") {
    const int v = std::atoi(value.c_str());
    if (v >= kPointerAimTurnMin && v <= kPointerAimTurnMax) {
      s.pointerAimTurn = v;
    }
  } else if (key == "lock_on_free_aim") {
    s.lockOnFreeAim = ParseBool(value);
  } else if (key == "crosshair_size") {
    const int v = std::atoi(value.c_str());
    if (v >= kCrosshairSizeMin && v <= kCrosshairSizeMax) {
      s.crosshairSize = v;
    }
  } else if (key == "mouse_sensitivity") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f > 0.f && f <= 1.f) {
      s.mouseSensitivity = f;
    }
  } else if (key == "mouse_sensitivity_split") {
    s.mouseSensitivitySplit = ParseBool(value);
  } else if (key == "mouse_sensitivity_y") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f > 0.f && f <= 1.f) {
      s.mouseSensitivityY = f;
    }
  } else if (key == "mouse_dpi") {
    const int v = std::atoi(value.c_str());
    if (v >= 100 && v <= 64000) {
      s.mouseDpi = v;
    }
  } else if (key == "spring_ball") {
    s.springBall = ParseBool(value);
  } else if (key == "swap_scan_xray") {
    s.swapScanXray = ParseBool(value);
  } else if (key == "radial_menus") {
    s.radialMenus = ParseBool(value);
  } else if (key == "shift_key" || key == "shift_key_alt" || key == "shift_pad") {
    const int slot = key == "shift_key" ? 0 : key == "shift_key_alt" ? 1 : 2;
    int code = 0;
    if (ParseCode(value, code)) {
      s.shiftBindings[slot] = code;
    }
  } else if (key == "turbo_key" || key == "turbo_key_alt" || key == "turbo_pad") {
    const int slot = key == "turbo_key" ? 0 : key == "turbo_key_alt" ? 1 : 2;
    int code = 0;
    if (ParseCode(value, code)) {
      s.turboBindings[slot] = code;
    }
  } else if (key == "pad_alt") {
    // kPadAltCount comma-separated codes; a short or malformed list keeps the
    // rest as they are.
    const char* cursor = value.c_str();
    for (int i = 0; i < kPadAltCount && *cursor != '\0'; ++i) {
      char* end = nullptr;
      const long code = std::strtol(cursor, &end, 10);
      if (end == cursor) {
        break;
      }
      s.padAltButtons[i] = static_cast< int >(code);
      cursor = *end == ',' ? end + 1 : end;
    }
  } else if (MouseButtonSetting(key) >= 0) {
    const int action = PortInputMap::MouseActionFromName(value.c_str());
    if (action >= 0) {
      s.mouseActions[MouseButtonSetting(key)] = action;
    }
  } else if (key == "fast_morph") {
    s.fastMorph = ParseBool(value);
  } else if (key == "lock_on_toggle") {
    s.lockOnToggle = ParseBool(value);
  } else if (key == "sticky_charge") {
    s.stickyCharge = ParseBool(value);
  } else if (key == "rapid_charge") {
    s.rapidCharge = ParseBool(value);
  } else if (key == "remastered_movement") {
    s.remasteredMovement = ParseBool(value);
  } else if (key == "spring_ball_flick") {
    s.springFlick = ParseBool(value);
  } else if (key == "spring_ball_flick_rate") {
    const float f = ParseFloat(value);
    if (std::isfinite(f) && f >= 2.f && f <= 20.f) {
      s.springFlickRate = f;
    }
  } else if (key == "spring_ball_swipe") {
    s.springSwipe = ParseBool(value);
  } else {
    return false;
  }
  return true;
}

void PostLoad() {
  Settings& s = MutableSettings();
  if (s.touchClassic) {
    s.touchTwinStick = false;
  }
  // gyro_rate was aim pixels per rad/s, and a pixel turned mouse_sensitivity rad.
  if (!s.gyroSensitivityRead && s.gyroLegacyRate > 0.f) {
    s.gyroSensitivity =
        std::clamp(s.gyroLegacyRate * s.mouseSensitivity, kGyroSensitivityMin, kGyroSensitivityMax);
  }
  s.gyroLegacyRate = 0.f;
  s.gyroSensitivityRead = false;
}

void WriteControlSettings(std::ostream& out) {
  const Settings& s = Get();
  out << "mouse_aim=" << (s.mouseAim ? 1 : 0) << '\n';
  out << "twin_stick=" << (s.twinStick ? 1 : 0) << '\n';
  out << "touch_colors=" << (s.touchColors ? 1 : 0) << '\n';
  out << "touch_labels=" << (s.touchLabels ? 1 : 0) << '\n';
  out << "touch_turbo=" << (s.touchTurbo ? 1 : 0) << '\n';
  out << "touch_floating_stick=" << (s.touchFloatingStick ? 1 : 0) << '\n';
  out << "spring_ball=" << (s.springBall ? 1 : 0) << '\n';
  out << "swap_scan_xray=" << (s.swapScanXray ? 1 : 0) << '\n';
  out << "radial_menus=" << (s.radialMenus ? 1 : 0) << '\n';
  out << "shift_key=" << s.shiftBindings[0] << '\n';
  out << "shift_key_alt=" << s.shiftBindings[1] << '\n';
  out << "shift_pad=" << s.shiftBindings[2] << '\n';
  out << "turbo_key=" << s.turboBindings[0] << '\n';
  out << "turbo_key_alt=" << s.turboBindings[1] << '\n';
  out << "turbo_pad=" << s.turboBindings[2] << '\n';
  out << "pad_alt=";
  for (int i = 0; i < kPadAltCount; ++i) {
    out << (i != 0 ? "," : "") << s.padAltButtons[i];
  }
  out << '\n';
  out << "fast_morph=" << (s.fastMorph ? 1 : 0) << '\n';
}

void WriteGameplaySettings(std::ostream& out) {
  const Settings& s = Get();
  out << "lock_on_toggle=" << (s.lockOnToggle ? 1 : 0) << '\n';
  out << "sticky_charge=" << (s.stickyCharge ? 1 : 0) << '\n';
  out << "rapid_charge=" << (s.rapidCharge ? 1 : 0) << '\n';
  out << "remastered_movement=" << (s.remasteredMovement ? 1 : 0) << '\n';
}

void WriteAimSettings(std::ostream& out) {
  const Settings& s = Get();
  out << "spring_ball_flick=" << (s.springFlick ? 1 : 0) << '\n';
  out << "spring_ball_flick_rate=" << s.springFlickRate << '\n';
  out << "spring_ball_swipe=" << (s.springSwipe ? 1 : 0) << '\n';
  out << "stick_aim_rate=" << s.stickAimRate << '\n';
  out << "stick_aim_split=" << (s.stickAimSplit ? 1 : 0) << '\n';
  out << "stick_aim_rate_y=" << s.stickAimRateY << '\n';
  out << "gyro_mode=" << s.gyroMode << '\n';
  out << "gyro_source=" << s.gyroSource << '\n';
  out << "gyro_sensitivity=" << s.gyroSensitivity << '\n';
  out << "gyro_split=" << (s.gyroSplit ? 1 : 0) << '\n';
  out << "gyro_sensitivity_y=" << s.gyroSensitivityY << '\n';
  out << "gyro_space=" << s.gyroSpace << '\n';
  out << "gyro_auto_calibrate=" << s.gyroAutoCalibrate << '\n';
  out << "gyro_smoothing=" << s.gyroSmoothing << '\n';
  out << "gyro_tightening=" << s.gyroTightening << '\n';
  out << "gyro_cursor_angle=" << s.gyroCursorAngle << '\n';
  out << "gyro_cursor_camera=" << (s.gyroCursorCamera ? 1 : 0) << '\n';
  out << "reset_aim_both=" << (s.resetAimBoth ? 1 : 0) << '\n';
  out << "touch_aim=" << (s.touchAim ? 1 : 0) << '\n';
  out << "touch_aim_speed=" << s.touchAimSpeed << '\n';
  out << "touch_aim_split=" << (s.touchAimSplit ? 1 : 0) << '\n';
  out << "touch_aim_speed_y=" << s.touchAimSpeedY << '\n';
  out << "touch_side_margin=" << s.touchSideMargin.load() << '\n';
  out << "touch_stick_inset=" << s.touchStickInset.load() << '\n';
  out << "touch_button_inset=" << s.touchButtonInset.load() << '\n';
  out << "touch_controls=" << (s.touchControls.load() ? 1 : 0) << '\n';
  out << "touch_opacity=" << s.touchOpacity.load() << '\n';
  out << "touch_layout=" << TouchLayout() << '\n';
  out << "touch_map_tap=" << (s.touchMapTap ? 1 : 0) << '\n';
  out << "touch_classic_gc=" << (s.touchClassic ? 1 : 0) << '\n';
  out << "touch_twin_stick=" << (s.touchTwinStick ? 1 : 0) << '\n';
  out << "touch_wheels=" << (s.touchWheels ? 1 : 0) << '\n';
  out << "touch_visor_tap_scan=" << (s.touchVisorTapScan ? 1 : 0) << '\n';
  out << "mouse_invert_x=" << (s.mouseInvertX ? 1 : 0) << '\n';
  out << "mouse_invert_y=" << (s.mouseInvertY ? 1 : 0) << '\n';
  out << "mouse_buttons=" << (s.mouseButtons ? 1 : 0) << '\n';
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    out << PortInputMap::MouseButtonKey(i) << '=' << PortInputMap::MouseActionInfo(s.mouseActions[i]).name
        << '\n';
  }
  out << "mouse_crosshair=" << (s.mouseCrosshair ? 1 : 0) << '\n';
  out << "crosshair_size=" << s.crosshairSize << '\n';
  out << "mouse_sensitivity=" << s.mouseSensitivity << '\n';
  out << "mouse_sensitivity_split=" << (s.mouseSensitivitySplit ? 1 : 0) << '\n';
  out << "mouse_sensitivity_y=" << s.mouseSensitivityY << '\n';
  out << "mouse_dpi=" << s.mouseDpi << '\n';
  out << "pointer_aim=" << (s.pointerAim ? 1 : 0) << '\n';
  out << "pointer_aim_box=" << s.pointerAimBox << '\n';
  out << "pointer_aim_turn=" << s.pointerAimTurn << '\n';
  out << "lock_on_free_aim=" << (s.lockOnFreeAim ? 1 : 0) << '\n';
}

void WriteSettings(std::ostream& out) {
  WriteControlSettings(out);
  WriteGameplaySettings(out);
  WriteAimSettings(out);
}

void ApplyEnvOverrides() {
  Settings& s = MutableSettings();
  s.rapidCharge = port::EnvFlag("MP_RAPID_CHARGE", s.rapidCharge);
  s.remasteredMovement = port::EnvFlag("MP_REMASTERED_MOVEMENT", s.remasteredMovement);
  if (port::EnvFlag("MP_MOUSE_AIM")) {
    s.mouseAim = true;
  }
  if (port::EnvFlag("MP_TWIN_STICK")) {
    s.twinStick = true;
  }
  if (port::EnvFlag("MP_MOUSE_INVERT_X")) {
    s.mouseInvertX = true;
  }
  if (port::EnvFlag("MP_MOUSE_INVERT_Y")) {
    s.mouseInvertY = true;
  }
  if (port::EnvFlag("MP_DISABLE_MOUSE_BUTTONS")) {
    s.mouseButtons = false;
  }
  if (port::EnvFlag("MP_DISABLE_MOUSE_CROSSHAIR")) {
    s.mouseCrosshair = false;
  }
  if (port::EnvFlag("MP_POINTER_AIM")) {
    s.pointerAim = true;
  }
  const float sensitivity = port::EnvFloat("MP_MOUSE_SENS", 0.f);
  if (std::isfinite(sensitivity) && sensitivity > 0.f) {
    s.mouseSensitivity = sensitivity;
  }
}

std::string TouchLayout() {
  std::lock_guard< std::mutex > lock(sLayoutMutex);
  return sLayout;
}

bool SetTouchLayout(std::string_view layout) {
  if (layout.size() > kTouchLayoutMaxLen) {
    return false;
  }
  std::lock_guard< std::mutex > lock(sLayoutMutex);
  sLayout = layout;
  return true;
}

} // namespace PortInput
