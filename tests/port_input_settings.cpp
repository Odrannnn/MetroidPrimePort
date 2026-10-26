#include "port_input_settings.h"
#include "port_mouse.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

std::string Text() {
  std::ostringstream out;
  PortInput::WriteSettings(out);
  return out.str();
}
} // namespace

int main() {
  using namespace PortInput;

  // Defaults, and the file they write.
  const std::string defaults = Text();
  REQUIRE_MSG(!Get().mouseAim && !Get().twinStick && Get().remasteredMovement, "defaults");
  REQUIRE_MSG(Get().shiftBindings[0] == kScancodeLShift && Get().shiftBindings[1] == -1, "default shift");
  REQUIRE_MSG(defaults.rfind("mouse_aim=0\ntwin_stick=0\n", 0) == 0, "first lines");
  REQUIRE_MSG(defaults.find("pad_alt=-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1\n") != std::string::npos, "pad_alt");
  REQUIRE_MSG(defaults.find("mouse_left=") != std::string::npos && defaults.find("mouse_x2=") != std::string::npos,
              "mouse action keys");

  // Keys that aren't input keys are left to the caller.
  REQUIRE_MSG(!ApplySetting("fov", "90") && !ApplySetting("game_options", "00") && !ApplySetting("nonsense", "1"),
              "foreign keys");

  // Toggles, ranges and rejects.
  REQUIRE_MSG(ApplySetting("mouse_aim", "1") && Get().mouseAim, "mouse_aim");
  REQUIRE_MSG(ApplySetting("stick_aim_rate", "2000") && Get().stickAimRate == 2000.f, "stick_aim_rate");
  ApplySetting("stick_aim_rate", "10");
  REQUIRE_MSG(Get().stickAimRate == 2000.f, "stick_aim_rate below range");
  ApplySetting("stick_aim_rate", "20000");
  REQUIRE_MSG(Get().stickAimRate == 2000.f, "stick_aim_rate above range");
  REQUIRE_MSG(ApplySetting("stick_aim_split", "1") && Get().stickAimSplit, "stick_aim_split");
  REQUIRE_MSG(ApplySetting("stick_aim_rate_y", "450") && Get().stickAimRateY == 450.f, "stick_aim_rate_y");
  ApplySetting("stick_aim_rate_y", "20000");
  REQUIRE_MSG(Get().stickAimRateY == 450.f, "stick_aim_rate_y above range");
  ApplySetting("stick_aim_split", "0");
  REQUIRE_MSG(!Get().mouseSensitivitySplit && !Get().touchAimSplit && !Get().gyroSplit, "vertical splits default off");
  // 3.0 on the Source scale (issue #210), both axes.
  REQUIRE_MSG(std::fabs(PortInput::Settings{}.mouseSensitivity - PortMouse::kDefaultSensitivity) < 1e-8f &&
                  std::fabs(PortInput::Settings{}.mouseSensitivityY - PortMouse::kDefaultSensitivity) < 1e-8f,
              "mouse sensitivity defaults to 3.0 Source");
  REQUIRE_MSG(ApplySetting("mouse_sensitivity_split", "1") && Get().mouseSensitivitySplit, "mouse_sensitivity_split");
  REQUIRE_MSG(ApplySetting("mouse_sensitivity_y", "0.002") && Get().mouseSensitivityY == 0.002f, "mouse_sensitivity_y");
  ApplySetting("mouse_sensitivity_y", "5");
  ApplySetting("mouse_sensitivity_y", "0");
  REQUIRE_MSG(Get().mouseSensitivityY == 0.002f, "mouse_sensitivity_y range");
  REQUIRE_MSG(ApplySetting("touch_aim_split", "1") && Get().touchAimSplit, "touch_aim_split");
  REQUIRE_MSG(ApplySetting("touch_aim_speed_y", "1.5") && Get().touchAimSpeedY == 1.5f, "touch_aim_speed_y");
  ApplySetting("touch_aim_speed_y", "50");
  ApplySetting("touch_aim_speed_y", "0.1");
  REQUIRE_MSG(Get().touchAimSpeedY == 1.5f, "touch_aim_speed_y range");
  REQUIRE_MSG(ApplySetting("gyro_split", "1") && Get().gyroSplit, "gyro_split");
  REQUIRE_MSG(ApplySetting("gyro_sensitivity_y", "0.5") && Get().gyroSensitivityY == 0.5f, "gyro_sensitivity_y");
  ApplySetting("gyro_sensitivity_y", "50");
  REQUIRE_MSG(Get().gyroSensitivityY == 0.5f, "gyro_sensitivity_y range");
  {
    const std::string text = Text();
    REQUIRE_MSG(text.find("mouse_sensitivity_split=1") != std::string::npos &&
                    text.find("mouse_sensitivity_y=0.002") != std::string::npos &&
                    text.find("touch_aim_split=1") != std::string::npos &&
                    text.find("touch_aim_speed_y=1.5") != std::string::npos &&
                    text.find("gyro_split=1") != std::string::npos &&
                    text.find("gyro_sensitivity_y=0.5") != std::string::npos,
                "vertical split keys written");
  }
  ApplySetting("mouse_sensitivity_split", "0");
  ApplySetting("touch_aim_split", "0");
  ApplySetting("gyro_split", "0");
  ApplySetting("crosshair_size", "80");
  REQUIRE_MSG(Get().crosshairSize == 80, "crosshair_size");
  ApplySetting("crosshair_size", "5");
  REQUIRE_MSG(Get().crosshairSize == 80, "crosshair_size range");
  ApplySetting("touch_side_margin", "40");
  REQUIRE_MSG(Get().touchSideMargin.load() == 40.f, "touch_side_margin");
  ApplySetting("touch_side_margin", "400");
  REQUIRE_MSG(Get().touchSideMargin.load() == 40.f, "touch_side_margin range");
  REQUIRE_MSG(Get().touchControls.load(), "touch_controls defaults on");
  ApplySetting("touch_controls", "0");
  REQUIRE_MSG(!Get().touchControls.load(), "touch_controls off");
  ApplySetting("touch_controls", "1");
  REQUIRE_MSG(Get().touchControls.load(), "touch_controls on");
  ApplySetting("touch_opacity", "50");
  REQUIRE_MSG(Get().touchOpacity.load() == 50.f, "touch_opacity");
  ApplySetting("touch_opacity", "400");
  REQUIRE_MSG(Get().touchOpacity.load() == 50.f, "touch_opacity above range");
  ApplySetting("touch_opacity", "0");
  REQUIRE_MSG(Get().touchOpacity.load() == 50.f, "touch_opacity below range");
  ApplySetting("gyro_mode", "2");
  ApplySetting("gyro_mode", "3");
  ApplySetting("gyro_mode", "4");
  REQUIRE_MSG(Get().gyroMode == 4, "gyro toggle mode");
  ApplySetting("gyro_mode", "5");
  REQUIRE_MSG(Get().gyroMode == 4, "gyro_mode range");
  REQUIRE_MSG(Get().gyroSensitivity == 2.f, "gyro_sensitivity default");
  ApplySetting("gyro_sensitivity", "1.5");
  ApplySetting("gyro_sensitivity", "50");
  REQUIRE_MSG(Get().gyroSensitivity == 1.5f, "gyro_sensitivity range");
  PostLoad();
  // An old gyro_rate (pixels per rad/s at mouse_sensitivity rad per pixel) becomes
  // the same turn on the natural scale, unless gyro_sensitivity is also there.
  ApplySetting("mouse_sensitivity", "0.0035");
  ApplySetting("gyro_rate", "600");
  PostLoad();
  REQUIRE_MSG(std::fabs(Get().gyroSensitivity - 2.1f) < 1e-4f, "gyro_rate migrates");
  ApplySetting("gyro_rate", "600");
  ApplySetting("gyro_sensitivity", "1.5");
  PostLoad();
  REQUIRE_MSG(Get().gyroSensitivity == 1.5f, "gyro_sensitivity wins over gyro_rate");
  REQUIRE_MSG(Text().find("gyro_rate") == std::string::npos, "gyro_rate not written");
  REQUIRE_MSG(Get().gyroSpace == 1 && Get().gyroAutoCalibrate == 1, "gyro space/calibration defaults");
  ApplySetting("gyro_space", "2");
  ApplySetting("gyro_space", "5");
  ApplySetting("gyro_auto_calibrate", "0");
  REQUIRE_MSG(Get().gyroSpace == 2 && Get().gyroAutoCalibrate == 0, "gyro space/calibration");
  REQUIRE_MSG(Text().find("gyro_space=2\n") != std::string::npos &&
                  Text().find("gyro_auto_calibrate=0\n") != std::string::npos,
              "gyro space/calibration written");
  ApplySetting("gyro_auto_calibrate", "1");
  REQUIRE_MSG(Get().gyroAutoCalibrate == 1, "gyro_auto_calibrate 1 = always (old on)");
  ApplySetting("gyro_auto_calibrate", "2");
  REQUIRE_MSG(Get().gyroAutoCalibrate == 2 &&
                  Text().find("gyro_auto_calibrate=2\n") != std::string::npos,
              "gyro_auto_calibrate 2 = only in menus, written");
  ApplySetting("gyro_auto_calibrate", "true");
  REQUIRE_MSG(Get().gyroAutoCalibrate == 1, "gyro_auto_calibrate true = always");
  ApplySetting("gyro_auto_calibrate", "false");
  REQUIRE_MSG(Get().gyroAutoCalibrate == 0, "gyro_auto_calibrate false = off");
  REQUIRE_MSG(Get().gyroSmoothing == 0.f && Get().gyroTightening == 0.f, "gyro filter defaults off");
  ApplySetting("gyro_smoothing", "5");
  ApplySetting("gyro_tightening", "3");
  ApplySetting("gyro_tightening", "51");
  REQUIRE_MSG(Get().gyroSmoothing == 5.f && Get().gyroTightening == 3.f, "gyro filter");
  REQUIRE_MSG(Text().find("gyro_smoothing=5\n") != std::string::npos &&
                  Text().find("gyro_tightening=3\n") != std::string::npos,
              "gyro filter written");
  REQUIRE_MSG(Get().gyroCursorAngle == PortInput::kGyroCursorAngleDefault && !Get().gyroCursorCamera,
              "gyro cursor defaults");
  ApplySetting("gyro_cursor_angle", "60");
  ApplySetting("gyro_cursor_angle", "200");
  ApplySetting("gyro_cursor_camera", "1");
  REQUIRE_MSG(Get().gyroCursorAngle == 60.f && Get().gyroCursorCamera, "gyro cursor");
  REQUIRE_MSG(Text().find("gyro_cursor_angle=60\n") != std::string::npos &&
                  Text().find("gyro_cursor_camera=1\n") != std::string::npos,
              "gyro cursor written");
  REQUIRE_MSG(!Get().lockOnFreeAim, "lock_on_free_aim default off");
  ApplySetting("lock_on_free_aim", "1");
  REQUIRE_MSG(Get().lockOnFreeAim && Text().find("lock_on_free_aim=1\n") != std::string::npos,
              "lock_on_free_aim");
  REQUIRE_MSG(!Get().resetAimBoth, "reset_aim_both default off");
  ApplySetting("reset_aim_both", "1");
  REQUIRE_MSG(Get().resetAimBoth && Text().find("reset_aim_both=1\n") != std::string::npos,
              "reset_aim_both");
  ApplySetting("shift_key_alt", "44");
  ApplySetting("turbo_pad", "3");
  REQUIRE_MSG(Get().shiftBindings[1] == 44 && Get().turboBindings[2] == 3, "bindings");
  ApplySetting("mouse_right", "lock_on");
  const std::string before = Text();
  ApplySetting("mouse_right", "not_an_action");
  REQUIRE_MSG(Text() == before, "unknown mouse action ignored");

  // Touch layout.
  REQUIRE_MSG(ApplySetting("touch_layout", "1:0.5,0.5,1") && TouchLayout() == "1:0.5,0.5,1", "touch_layout");
  REQUIRE_MSG(!SetTouchLayout(std::string(kTouchLayoutMaxLen + 1, 'x')), "touch layout length");
  REQUIRE_MSG(TouchLayout() == "1:0.5,0.5,1", "touch layout kept");

  // Classic GameCube wins over twin stick on load.
  ApplySetting("touch_twin_stick", "1");
  ApplySetting("touch_classic_gc", "1");
  PostLoad();
  REQUIRE_MSG(Get().touchClassic && !Get().touchTwinStick, "classic wins");

  // Write then read back reproduces the same text.
  const std::string saved = Text();
  ApplySetting("mouse_aim", "0");
  ApplySetting("stick_aim_rate", "300");
  ApplySetting("touch_side_margin", "10");
  ApplySetting("shift_key", "5");
  ApplySetting("touch_classic_gc", "0");
  SetTouchLayout("");
  REQUIRE_MSG(Text() != saved, "state changed before the read-back");
  std::istringstream in(saved);
  for (std::string line; std::getline(in, line);) {
    const size_t eq = line.find('=');
    REQUIRE_MSG(eq != std::string::npos, "line has =");
    REQUIRE_MSG(ApplySetting(line.substr(0, eq), line.substr(eq + 1)) || line.compare(0, 12, "touch_layout") == 0,
                "every written key is read");
  }
  PostLoad();
  REQUIRE_MSG(Text() == saved, "round trip");
  return 0;
}
