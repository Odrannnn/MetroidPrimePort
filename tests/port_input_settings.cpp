#include "port_input_settings.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "input settings regression failed: %s\n", what);
    std::abort();
  }
}

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
  Check(!Get().mouseAim && !Get().twinStick && Get().remasteredMovement, "defaults");
  Check(Get().shiftBindings[0] == kScancodeLShift && Get().shiftBindings[1] == -1, "default shift");
  Check(defaults.rfind("mouse_aim=0\ntwin_stick=0\n", 0) == 0, "first lines");
  Check(defaults.find("pad_alt=-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1\n") != std::string::npos, "pad_alt");
  Check(defaults.find("mouse_left=") != std::string::npos && defaults.find("mouse_x2=") != std::string::npos,
        "mouse action keys");

  // Keys that aren't input keys are left to the caller.
  Check(!ApplySetting("fov", "90") && !ApplySetting("game_options", "00") && !ApplySetting("nonsense", "1"),
        "foreign keys");

  // Toggles, ranges and rejects.
  Check(ApplySetting("mouse_aim", "1") && Get().mouseAim, "mouse_aim");
  Check(ApplySetting("stick_aim_rate", "2000") && Get().stickAimRate == 2000.f, "stick_aim_rate");
  ApplySetting("stick_aim_rate", "10");
  Check(Get().stickAimRate == 2000.f, "stick_aim_rate below range");
  ApplySetting("stick_aim_rate", "20000");
  Check(Get().stickAimRate == 2000.f, "stick_aim_rate above range");
  ApplySetting("crosshair_size", "80");
  Check(Get().crosshairSize == 80, "crosshair_size");
  ApplySetting("crosshair_size", "5");
  Check(Get().crosshairSize == 80, "crosshair_size range");
  ApplySetting("touch_side_margin", "40");
  Check(Get().touchSideMargin.load() == 40.f, "touch_side_margin");
  ApplySetting("touch_side_margin", "400");
  Check(Get().touchSideMargin.load() == 40.f, "touch_side_margin range");
  ApplySetting("gyro_mode", "2");
  ApplySetting("gyro_mode", "3");
  Check(Get().gyroMode == 2, "gyro_mode range");
  ApplySetting("shift_key_alt", "44");
  ApplySetting("turbo_pad", "3");
  Check(Get().shiftBindings[1] == 44 && Get().turboBindings[2] == 3, "bindings");
  ApplySetting("mouse_right", "lock_on");
  const std::string before = Text();
  ApplySetting("mouse_right", "not_an_action");
  Check(Text() == before, "unknown mouse action ignored");

  // Touch layout.
  Check(ApplySetting("touch_layout", "1:0.5,0.5,1") && TouchLayout() == "1:0.5,0.5,1", "touch_layout");
  Check(!SetTouchLayout(std::string(kTouchLayoutMaxLen + 1, 'x')), "touch layout length");
  Check(TouchLayout() == "1:0.5,0.5,1", "touch layout kept");

  // Classic GameCube wins over twin stick on load.
  ApplySetting("touch_twin_stick", "1");
  ApplySetting("touch_classic_gc", "1");
  PostLoad();
  Check(Get().touchClassic && !Get().touchTwinStick, "classic wins");

  // Write then read back reproduces the same text.
  const std::string saved = Text();
  ApplySetting("mouse_aim", "0");
  ApplySetting("stick_aim_rate", "300");
  ApplySetting("touch_side_margin", "10");
  ApplySetting("shift_key", "5");
  ApplySetting("touch_classic_gc", "0");
  SetTouchLayout("");
  Check(Text() != saved, "state changed before the read-back");
  std::istringstream in(saved);
  for (std::string line; std::getline(in, line);) {
    const size_t eq = line.find('=');
    Check(eq != std::string::npos, "line has =");
    Check(ApplySetting(line.substr(0, eq), line.substr(eq + 1)) || line.compare(0, 12, "touch_layout") == 0,
          "every written key is read");
  }
  PostLoad();
  Check(Text() == saved, "round trip");
  return 0;
}
