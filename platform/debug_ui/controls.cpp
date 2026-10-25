// F1 overlay: The Controls page. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_apclient.h"
#include "port_controls.h"
#include "port_input_devices.h"
#include "port_mouse.h"

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_gamepad.h>

#if defined(PORT_TOUCH_CONTROLS)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>

namespace PortDebug {

void DrawControlsOptions() {
  const bool locked = BeginOriginalLocked();
  ImGui::SeparatorText("On-screen prompts");
  int glyphs = sPromptGlyphs.load(std::memory_order_relaxed);
  if (ImGui::Combo("Button glyphs", &glyphs,
                   "Follow the input in use\0" "Xbox\0" "PlayStation\0" "Nintendo Switch\0" "GameCube\0"
                   "Keyboard & mouse\0")) {
    sPromptGlyphs.store(glyphs, std::memory_order_relaxed);
    MarkDirty();
  }
  ItemHelp("Which buttons the game's prompts show. Follow uses the device you last used; GameCube "
           "shows the game's own art.");

  ImGui::SeparatorText("Buttons");
  bool lockOnToggle = sInput.lockOnToggle;
  if (ImGui::Checkbox("Toggle Lock-On", &lockOnToggle)) {
    SetLockOnToggle(lockOnToggle);
  }
  ItemHelp("Press L once to lock on, scan or grapple, and again to let go. With no target, L works "
           "as usual.");
  bool stickyCharge = sInput.stickyCharge;
  if (ImGui::Checkbox("Sticky Charge", &stickyCharge)) {
    SetStickyCharge(stickyCharge);
  }
  ItemHelp("Tap to fire as usual. Hold fire, let go, and the beam keeps charging; press fire again "
           "to shoot. Needs the Charge Beam.");
  bool rapidCharge = sInput.rapidCharge;
  if (ImGui::Checkbox("Remastered charge (rapid fire)", &rapidCharge)) {
    SetRapidCharge(rapidCharge);
  }
  ItemHelp("As in Metroid Prime Remastered: holding fire shoots a few shots (depending on the "
           "beam), then charges faster.");
  bool swapScanXray = sInput.swapScanXray;
  if (ImGui::Checkbox("Swap the Scan and X-Ray visor buttons", &swapScanXray)) {
    SetSwapScanXray(swapScanXray);
  }
  ImGui::SetItemTooltip("Swaps the two buttons' D-pad directions, as in Remastered's Dual Sticks "
                        "layout.\nThe controller presets (Remap > Presets...) set it.");

  ImGui::SeparatorText("Aiming");
  if (ImGui::Checkbox("Auto-aim", &sAutoAim)) {
    MarkDirty();
  }
  ItemHelp("On, as the original: without Lock-On, shots home in on the enemy nearest the screen "
           "centre. Off: shots go where you aim.");
  bool pointerAim = sInput.pointerAim;
  if (ImGui::Checkbox("Pointer aim (Trilogy style)", &pointerAim)) {
    SetPointerAim(pointerAim);
  }
  ItemHelp(
      "Mouse, right stick, gyro or a touch drag moves a crosshair and shots follow it; the view "
      "turns when it nears the edge. Needs Mouse aim, Twin stick or the modern touch layout.");
  ImGui::BeginDisabled(!sInput.pointerAim);
  if (ImGui::SliderInt("Dead box", &sInput.pointerAimBox, PortInput::kPointerAimBoxMin, PortInput::kPointerAimBoxMax,
                       "%d%%")) {
    sInput.pointerAimBox = std::clamp(sInput.pointerAimBox, PortInput::kPointerAimBoxMin, PortInput::kPointerAimBoxMax);
    MarkDirty();
  }
  ImGui::SetItemTooltip("How far the crosshair moves from the centre (percent of the half screen) "
                        "before the view turns.");
  if (ImGui::SliderInt("Edge turn speed", &sInput.pointerAimTurn, PortInput::kPointerAimTurnMin,
                       PortInput::kPointerAimTurnMax, "%d deg/s")) {
    sInput.pointerAimTurn =
        std::clamp(sInput.pointerAimTurn, PortInput::kPointerAimTurnMin, PortInput::kPointerAimTurnMax);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Turn speed with the crosshair at the screen edge.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!DirectAim());
  if (ImGui::Checkbox("Lock-on free aim", &sInput.lockOnFreeAim)) {
    MarkDirty();
  }
  ImGui::EndDisabled();
  ItemHelp("As in Remastered: while locked on, the mouse, right stick, gyro or a touch drag moves "
           "a crosshair and shots follow it. Centred, shots and missiles go for the locked target "
           "as before. Needs Mouse aim, Twin stick or the modern touch layout.");
  bool resetAimBoth = sInput.resetAimBoth;
  if (ImGui::Checkbox("Reset aim resets pointer and camera", &resetAimBoth)) {
    sInput.resetAimBoth = resetAimBoth;
    MarkDirty();
  }
  ItemHelp("Reset aim (unbound by default) recentres the crosshair with pointer aim on, or levels "
           "the camera with it off. On: it does both.");

  ImGui::SeparatorText("Morph ball");
  bool fastMorph = sInput.fastMorph;
  if (ImGui::Checkbox("Fast Morph", &fastMorph)) {
    SetFastMorph(fastMorph);
  }
  ItemHelp("Morphing is quick and keeps your momentum, as in Metroid Prime 4.");
  const int springRule = PortAp::SpringBallRule();
  ImGui::BeginDisabled(springRule >= 0);
  bool springBall = sInput.springBall;
  if (ImGui::Checkbox("Spring Ball (C-stick up)", &springBall)) {
    SetSpringBall(springBall);
  }
  ImGui::EndDisabled();
  if (springRule >= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(set by the Archipelago seed: %s)", springRule == 0   ? "not received yet"
                                                             : springRule == 1 ? "with the Bombs"
                                                                               : "unlocked");
  } else {
    ItemHelp(
        "A small jump in morph ball, as in Metroid Prime Trilogy. Needs the Morph Ball Bombs.");
  }
  EndOriginalLocked(locked);

  // The game's own option, so it is the same switch as the pause menu's and is
  // saved with the other game options; not locked by Original experience.
  ImGui::SeparatorText("Rumble");
  ImGui::BeginDisabled(gpGameState == nullptr);
  bool rumble = gpGameState != nullptr && gpGameState->GameOptions().GetIsRumbleEnabled();
  if (ImGui::Checkbox("Rumble", &rumble) && gpGameState != nullptr) {
    gpGameState->GameOptions().SetIsRumbleEnabled(rumble);
  }
  ImGui::EndDisabled();
  ItemHelp("Same switch as Rumble in the pause menu. A controller doesn't rumble while you last "
           "used the keyboard or mouse.");
}

void DrawControlsKeyboardMouse() {
  ImGui::SeparatorText("Mouse aim");
  bool mouseAim = sInput.mouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
    // Mouse aim replaces the R-button free look, so without twin-stick a pad
    // would have no aim at all. Both add into the same aim; twin-stick can
    // still be switched off on its own.
    if (mouseAim) SetTwinStick(true);
    MarkDirty();
  }
  ImGui::TextDisabled("Also switches on Twin stick, so a controller's right stick aims too.");
  // Shown on the Quake/Source/Apex scale (0.022 degrees a count per unit), stored in radians.
  float sourceSensitivity = PortMouse::SourceFromRadians(sInput.mouseSensitivity);
  if (ImGui::SliderFloat("Sensitivity", &sourceSensitivity, 0.05f, 30.f, "%.3f",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat)) {
    SetMouseSensitivity(PortMouse::RadiansFromSource(sourceSensitivity));
    MarkDirty();
  }
  ImGui::SetItemTooltip("Same scale as Quake, Source and Apex games: the same number turns the "
                        "same.\nCtrl+click to type a value.");
  if (ImGui::Checkbox("Separate vertical speed##mouse", &sInput.mouseSensitivitySplit)) {
    if (sInput.mouseSensitivitySplit) sInput.mouseSensitivityY = sInput.mouseSensitivity;
    MarkDirty();
  }
  ItemHelp("Own sensitivity for looking up and down; Sensitivity then sets left and right only.");
  if (sInput.mouseSensitivitySplit) {
    float sourceSensitivityY = PortMouse::SourceFromRadians(sInput.mouseSensitivityY);
    if (ImGui::SliderFloat("Vertical sensitivity", &sourceSensitivityY, 0.05f, 30.f, "%.3f",
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoRoundToFormat)) {
      SetMouseSensitivityY(PortMouse::RadiansFromSource(sourceSensitivityY));
      MarkDirty();
    }
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
  if (ImGui::InputInt("Mouse DPI", &sInput.mouseDpi, 0, 0)) {
    sInput.mouseDpi = std::clamp(sInput.mouseDpi, 100, 64000);
    MarkDirty();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%.1f cm (%.1f in) per 360 degrees",
                      PortMouse::CentimetresPer360(sInput.mouseSensitivity, float(sInput.mouseDpi)),
                      PortMouse::CentimetresPer360(sInput.mouseSensitivity, float(sInput.mouseDpi)) / 2.54f);
  ImGui::SetItemTooltip("Only used for this readout.");
  if (ImGui::Checkbox("Invert X", &sInput.mouseInvertX)) {
    MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Invert Y", &sInput.mouseInvertY)) {
    MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Weapon buttons", &sInput.mouseButtons)) {
    sMouseButtonGate.Reset();
    MarkDirty();
  }
  ImGui::SetItemTooltip("Mouse buttons are set in Controls > Remap; keyboard and controller weapon "
                        "bindings also work.");
  if (ImGui::Checkbox("Crosshair", &sInput.mouseCrosshair)) {
    MarkDirty();
  }
  ImGui::SameLine();
  int crosshairSize = sInput.crosshairSize;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
  if (ImGui::SliderInt("Size", &crosshairSize, kCrosshairSizeMin, kCrosshairSizeMax, "%d%%")) {
    SetCrosshairSize(crosshairSize);
  }
  ImGui::SetItemTooltip("Applies under mouse aim and twin stick.");

  ImGui::SeparatorText("Mouse buttons");
  ImGui::TextDisabled("Mouse buttons are bound in Controls > Remap (Keyboard & mouse).");
}

void DrawControlsController() {
  ImGui::SeparatorText("Stick aim");
  bool twinStick = sInput.twinStick;
  if (ImGui::Checkbox("Twin stick (right stick aims)", &twinStick)) {
    SetTwinStick(twinStick);
    MarkDirty();
  }
  ItemHelp("Aims with the right stick like the mouse, so it no longer free-looks. Fire stays on A; "
           "change it in Controls > Remap.");
  bool remasteredMovement = sInput.remasteredMovement;
  if (ImGui::Checkbox("Remastered movement (twin stick and mouse aim)", &remasteredMovement)) {
    SetRemasteredMovement(remasteredMovement);
  }
  ItemHelp("Moves as in Remastered and PrimeHack: even diagonal speed and a smooth jump arc. Off: "
           "GameCube movement. Classic controls always use GameCube movement.");
  // Shown only with twin stick on, as the pause menu's Stick Aim Speed row.
  if (sInput.twinStick) {
    float stickRate = sInput.stickAimRate;
    if (ImGui::SliderFloat("Stick aim speed", &stickRate, 100.f, 8100.f, "%.0f px/s",
                           ImGuiSliderFlags_Logarithmic)) {
      SetStickAimRate(stickRate);
    }
    ItemHelp(sInput.stickAimSplit ? "How fast the right stick turns left and right at full tilt."
                                  : "How fast the right stick turns the camera at full tilt.");
    bool split = sInput.stickAimSplit;
    if (ImGui::Checkbox("Separate vertical speed", &split)) {
      sInput.stickAimSplit = split;
      // Start vertical where the shared speed was, so turning the split on changes nothing yet.
      if (split) sInput.stickAimRateY = sInput.stickAimRate;
      MarkDirty();
    }
    ItemHelp("Own speed for looking up and down; Stick aim speed then sets left and right only.");
    if (sInput.stickAimSplit) {
      float stickRateY = sInput.stickAimRateY;
      if (ImGui::SliderFloat("Vertical stick aim speed", &stickRateY, 100.f, 8100.f, "%.0f px/s",
                             ImGuiSliderFlags_Logarithmic) &&
          std::isfinite(stickRateY) && stickRateY >= 50.f && stickRateY <= 10000.f) {
        sInput.stickAimRateY = stickRateY;
        MarkDirty();
      }
    }
    // The game's own option, saved with its settings; free look uses it too.
    ImGui::BeginDisabled(gpGameState == nullptr);
    bool invertY = gpGameState != nullptr && gpGameState->GameOptions().GetInvertYAxis();
    if (ImGui::Checkbox("Invert stick aim Y (the game's Reverse Y Axis)", &invertY)) {
      gpGameState->GameOptions().SetInvertYAxis(invertY);
    }
    ImGui::EndDisabled();
  }
  const s32 controllerIndex = PADGetIndexForPort(PAD_CHAN0);
  if (SDL_Gamepad* pad = controllerIndex >= 0 ? PADGetSDLGamepadForIndex(static_cast< u32 >(controllerIndex)) : nullptr;
      pad != nullptr && SDL_IsJoystickVirtual(SDL_GetGamepadID(pad))) {
    // Aurora ignores mappings on the virtual touch pad.
    ImGui::TextWrapped("The touch controls always use the GameCube layout; presets and remaps apply to "
                       "real controllers only.");
  } else {
    PortControls::DrawDeadZones();
  }
}

void DrawControlsTouchGyro() {
#if defined(PORT_TOUCH_CONTROLS)
  ImGui::SeparatorText("Touch controls");
  // First on the page: the layout is what touch players come here to change.
  if (ImGui::Button("Edit layout")) {
    // The touch overlay takes the request and opens its editor once this closes.
    sTouchEditRequested.store(true, std::memory_order_release);
    RequestToggle();
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset margins")) {
    sInput.touchSideMargin.store(kTouchSideMarginDefault);
    sInput.touchStickInset.store(kTouchStickInsetDefault);
    sInput.touchButtonInset.store(kTouchButtonInsetDefault);
    MarkDirty();
  }
  // One help line for the pair: inline help under the first would break the row.
  ItemHelp("Edit layout: move, resize, fade, hide, delete or add on-screen buttons. "
           "Reset margins: sets the three margins below back to 0.");
  bool touchControls = sInput.touchControls.load();
  if (ImGui::Checkbox("On-screen controls", &touchControls)) {
    sInput.touchControls.store(touchControls);
    MarkDirty();
  }
  ItemHelp("Off: only MENU and START stay, so stray touches don't bring the controls back while "
           "you play on a pad.");
  ImGui::BeginDisabled(!touchControls);
  if (ImGui::Checkbox("Coloured buttons", &sInput.touchColors)) {
    MarkDirty();
  }
  ItemHelp("GameCube colours: green A, red B, yellow C-stick, purple Z.");
  if (ImGui::Checkbox("Button descriptions", &sInput.touchLabels)) {
    MarkDirty();
  }
  ItemHelp("Shows what each button does (Fire, Jump, Lock...) next to its letter.");
  ImGui::BeginDisabled(sOriginalExperience);
  if (ImGui::Checkbox("Turbo fire button", &sInput.touchTurbo)) {
    if (!sInput.touchTurbo) {
      sTouchTurboFire.store(false, std::memory_order_release);
    }
    MarkDirty();
  }
  ImGui::EndDisabled();
  ItemHelp("Adds a Turbo button next to Fire: hold it to fire as fast as the game allows.");
  if (ImGui::Checkbox("Floating left stick", &sInput.touchFloatingStick)) {
    MarkDirty();
  }
  ItemHelp("The left stick appears wherever your thumb lands on the left half of the screen. "
           "The map keeps the fixed stick.");
#if defined(PORT_TOUCH_CONTROLS) // both views draw the classic and twin-stick layouts
  int touchLayout = sInput.touchClassic ? 1 : sInput.touchTwinStick ? 2 : 0;
  static const char* const kTouchLayouts[] = {"Default", "Classic GameCube", "Twin stick (Remastered)"};
  if (ImGui::Combo("Layout", &touchLayout, kTouchLayouts, 3)) {
    SetTouchClassic(touchLayout == 1);
    SetTouchTwinStick(touchLayout == 2);
    if (touchLayout == 2) {
      // Remastered picks beams with Y + the D-pad, so twin starts with the D-pad, not the wheels.
      SetTouchWheels(false);
    }
  }
  ItemHelp("Default: GameCube buttons, drag the screen to aim, Visor and Beam wheels.\n"
           "Classic GameCube: adds the C-stick and D-pad.\n"
           "Twin stick (Remastered): a right stick aims; Jump, Fire, Morph, Missile, LT Lock; "
           "D-pad for visors, Y + D-pad for beams.");
#endif
  ImGui::BeginDisabled(!sInput.touchClassic);
  bool touchAim = sInput.touchAim;
  if (ImGui::Checkbox("Touch aim", &touchAim)) {
    SetTouchAim(touchAim);
  }
  ItemHelp("Drag the free screen area to turn and look around (classic layout). Looking up or "
           "down levels out when you let go. Not while locked on or in the ball.");
  ImGui::EndDisabled();
  bool touchMapTap = sInput.touchMapTap;
  if (ImGui::Checkbox("Tap minimap for map", &touchMapTap)) {
    SetTouchMapTap(touchMapTap);
  }
  ItemHelp("Tap the minimap to open the map. Replaces the Z button.");
  ImGui::BeginDisabled(!sInput.touchClassic && !sInput.touchTwinStick);
  bool touchWheels = sInput.touchWheels;
  if (ImGui::Checkbox("Beam and visor wheels", &touchWheels)) {
    SetTouchWheels(touchWheels);
  }
  ItemHelp("Replaces the D-pad with Visor and Beam buttons: hold, slide to one, let go. "
           "Let go in the middle to cancel.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!((!sInput.touchClassic && !sInput.touchTwinStick) || sInput.touchWheels));
  bool touchVisorTapScan = sInput.touchVisorTapScan;
  if (ImGui::Checkbox("Tap Visor for Scan Visor", &touchVisorTapScan)) {
    SetTouchVisorTapScan(touchVisorTapScan);
  }
  ItemHelp("A quick tap on Visor switches to the Scan Visor.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!(!sInput.touchClassic || sInput.touchAim));
  float touchAimSpeed = sInput.touchAimSpeed;
  if (ImGui::SliderFloat("Touch aim speed", &touchAimSpeed, 0.5f, 6.f, "%.2f px/dp",
                         ImGuiSliderFlags_Logarithmic)) {
    SetTouchAimSpeed(touchAimSpeed);
  }
  if (ImGui::Checkbox("Separate vertical speed##touch", &sInput.touchAimSplit)) {
    if (sInput.touchAimSplit) sInput.touchAimSpeedY = sInput.touchAimSpeed;
    MarkDirty();
  }
  if (sInput.touchAimSplit) {
    float touchAimSpeedY = sInput.touchAimSpeedY;
    if (ImGui::SliderFloat("Vertical touch aim speed", &touchAimSpeedY, 0.5f, 6.f, "%.2f px/dp",
                           ImGuiSliderFlags_Logarithmic)) {
      SetTouchAimSpeedY(touchAimSpeedY);
    }
  }
  ImGui::EndDisabled();
  bool springSwipe = sInput.springSwipe;
  if (ImGui::Checkbox("Spring Ball on swipe up", &springSwipe)) {
    SetSpringBallSwipe(springSwipe);
  }
  ItemHelp("In morph ball, swipe up to jump. Needs the Spring Ball option.");
  float touchOpacity = sInput.touchOpacity.load();
  if (ImGui::SliderFloat("Opacity", &touchOpacity, kTouchOpacityMin, kTouchOpacityMax, "%.0f%%")) {
    sInput.touchOpacity.store(touchOpacity);
    MarkDirty();
  }
  ItemHelp("How visible all on-screen controls are. Edit layout sets single controls.");
  float sideMargin = sInput.touchSideMargin.load();
  if (ImGui::SliderFloat("Side margin", &sideMargin, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sInput.touchSideMargin.store(sideMargin);
    MarkDirty();
  }
  ItemHelp("Moves every control in from the left and right edges (curved screens, cases).");
  float stickInset = sInput.touchStickInset.load();
  if (ImGui::SliderFloat("Stick inset", &stickInset, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sInput.touchStickInset.store(stickInset);
    MarkDirty();
  }
  float buttonInset = sInput.touchButtonInset.load();
  if (ImGui::SliderFloat("Button inset", &buttonInset, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sInput.touchButtonInset.store(buttonInset);
    MarkDirty();
  }
  ItemHelp("Moves the face buttons (and the classic C-stick) further in from the edge.");
  ImGui::EndDisabled();
#endif

  ImGui::SeparatorText("Gyro aim");
  const char* gyroModes[] = {"Off", "Hold to aim", "Always aim", "Hold to pause", "Toggle to aim"};
  int gyroMode = sInput.gyroMode;
  if (ImGui::Combo("Mode", &gyroMode, gyroModes, IM_ARRAYSIZE(gyroModes))) {
    SetGyroMode(gyroMode);
  }
  ItemHelp("Tilt the pad or phone to aim. Hold to aim: only while the Gyro aim button is held. "
           "Hold to pause: always, except while it is held (to recentre). Toggle: press to start "
           "and stop. The button is right stick click or left Ctrl (Remap page). Needs mouse aim, "
           "twin stick or the touch "
           "controls (not the classic layout).");
  ImGui::BeginDisabled(sInput.gyroMode == 0 && !sInput.springFlick);
  const char* gyroSources[] = {"Auto", "Controller", "Phone"};
  int gyroSource = sInput.gyroSource;
  if (ImGui::Combo("Source", &gyroSource, gyroSources, 3)) {
    SetGyroSource(gyroSource);
  }
  ImGui::Text("Gyro: %s", GyroStatus());
  ImGui::EndDisabled();
  ImGui::BeginDisabled(sInput.gyroMode == 0);
  // With pointer aim the gyro moves the pointer at its own speed; the camera
  // sensitivity then only matters if the gyro also turns the camera.
  // Lock-on free aim's pointer moves at Pointer speed too, with or without pointer aim.
  const bool gyroPointer = PointerAim();
  if (gyroPointer || LockOnFreeAim()) {
    float cursorAngle = sInput.gyroCursorAngle;
    if (ImGui::SliderFloat("Pointer speed##gyro", &cursorAngle, PortInput::kGyroCursorAngleMin,
                           PortInput::kGyroCursorAngleMax, "%.0f deg across the screen")) {
      sInput.gyroCursorAngle =
          std::clamp(cursorAngle, PortInput::kGyroCursorAngleMin, PortInput::kGyroCursorAngleMax);
      MarkDirty();
    }
    ItemHelp("How far the pad turns to sweep the pointer across the screen. Smaller is faster.");
  }
  // A lock owns the view, so this only matters under pointer aim.
  if (gyroPointer) {
    bool cursorCamera = sInput.gyroCursorCamera;
    if (ImGui::Checkbox("Gyro also turns the camera", &cursorCamera)) {
      sInput.gyroCursorCamera = cursorCamera;
      MarkDirty();
    }
    ItemHelp("Also turns the camera with the gyro, at the Sensitivity below, while it moves the "
             "pointer.");
  }
  ImGui::BeginDisabled(gyroPointer && !sInput.gyroCursorCamera);
  float gyroSensitivity = sInput.gyroSensitivity;
  // The ## suffix keeps its ImGui id apart from the mouse Sensitivity slider.
  if (ImGui::SliderFloat("Sensitivity##gyro", &gyroSensitivity, PortInput::kGyroSensitivityMin,
                         PortInput::kGyroSensitivityMax, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
    SetGyroSensitivity(gyroSensitivity);
  }
  ItemHelp("How far the camera turns per turn of the pad: at 1.00x a 90 degree turn gives 90. With "
           "pointer aim it applies only if the gyro also turns the camera.");
  if (ImGui::Checkbox("Separate vertical speed##gyro", &sInput.gyroSplit)) {
    if (sInput.gyroSplit) sInput.gyroSensitivityY = sInput.gyroSensitivity;
    MarkDirty();
  }
  ItemHelp("Own sensitivity for tilting up and down; Sensitivity then sets left and right only. "
           "With pointer aim it scales the pointer's vertical travel.");
  if (sInput.gyroSplit) {
    float gyroSensitivityY = sInput.gyroSensitivityY;
    if (ImGui::SliderFloat("Vertical sensitivity##gyro", &gyroSensitivityY, PortInput::kGyroSensitivityMin,
                           PortInput::kGyroSensitivityMax, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
      SetGyroSensitivityY(gyroSensitivityY);
    }
  }
  ImGui::EndDisabled();
  // Listed by how common they are; the setting keeps PortGyro::Space's numbers.
  static constexpr int kSpaceOrder[] = {PortGyro::kPlayer, PortGyro::kWorld, PortGyro::kLocal, PortGyro::kLocalRoll,
                                        PortGyro::kLocalBoth};
  static constexpr const char* kSpaceNames[] = {"Player (turn)", "World (turn)", "Local yaw", "Local roll",
                                                "Local yaw and roll"};
  int spaceRow = 0;
  for (int i = 0; i < PortGyro::kSpaceCount; ++i) {
    if (kSpaceOrder[i] == sInput.gyroSpace) {
      spaceRow = i;
    }
  }
  if (ImGui::Combo("Space", &spaceRow, kSpaceNames, PortGyro::kSpaceCount)) {
    SetGyroSpace(kSpaceOrder[spaceRow]);
  }
  ItemHelp("Which pad turn moves the camera left and right. Player: left/right turns, flat or "
           "upright (the usual choice). World: turns about the real vertical. Local yaw: flat "
           "turns. Local roll: upright turns (handheld). Player and World need an accelerometer; "
           "without one they act as Local yaw.");
  static constexpr const char* kCalibrateNames[] = {"Off", "Always", "Only in menus"};
  int autoCalibrate = std::clamp(sInput.gyroAutoCalibrate, 0, 2);
  if (ImGui::Combo("Auto calibrate", &autoCalibrate, kCalibrateNames, 3)) {
    SetGyroAutoCalibrate(autoCalibrate);
  }
  ItemHelp("Removes gyro drift while the pad is still, so the camera doesn't creep; put it down "
           "for a couple of seconds. Needs an accelerometer. Only in menus learns only while "
           "paused or in menus, and keeps it while you play.");
  const float countdown = GyroRecalibrationCountdown();
  const float manual = GyroManualCalibrationProgress();
  if (manual >= 0.f) {
    ImGui::BeginDisabled();
    ImGui::Button(("Calibrating, keep the pad still... " + std::to_string(int(manual * 100.f)) + "%").c_str());
    ImGui::EndDisabled();
  } else if (countdown >= 0.f) {
    ImGui::BeginDisabled();
    ImGui::Button(("Calibrating in " + std::to_string(int(std::ceil(countdown))) + "...").c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      CancelGyroRecalibration();
    }
  } else if (ImGui::Button("Recalibrate")) {
    StartGyroRecalibration();
  }
  ItemHelp("Waits 5 seconds so you can put the pad down, then measures drift for 3. Needs the gyro "
           "in use.");
  const float confidence = GyroCalibrationConfidence();
  if (confidence >= 0.f) {
    ImGui::Text("Calibration: %d%%%s", int(confidence * 100.f + 0.5f),
                GyroCalibrating() ? " (calibrating)" : confidence >= 0.99f ? " (done)" : "");
  } else {
    ImGui::TextDisabled("Calibration: no accelerometer");
  }
  float gyroSmoothing = sInput.gyroSmoothing;
  if (ImGui::SliderFloat("Smoothing", &gyroSmoothing, 0.f, PortInput::kGyroThresholdMax,
                         gyroSmoothing > 0.f ? "below %.0f deg/s" : "off")) {
    sInput.gyroSmoothing = std::clamp(gyroSmoothing, 0.f, PortInput::kGyroThresholdMax);
    MarkDirty();
  }
  ItemHelp("Averages slow turns to hide sensor jitter; faster turns stay direct. Adds a little lag "
           "to fine aim.");
  float gyroTightening = sInput.gyroTightening;
  if (ImGui::SliderFloat("Tightening", &gyroTightening, 0.f, PortInput::kGyroThresholdMax,
                         gyroTightening > 0.f ? "below %.0f deg/s" : "off")) {
    sInput.gyroTightening = std::clamp(gyroTightening, 0.f, PortInput::kGyroThresholdMax);
    MarkDirty();
  }
  ItemHelp("Scales down turns slower than this so a pad held still doesn't wobble the aim. High "
           "values make fine aim sluggish.");
  ImGui::EndDisabled();

  bool springFlick = sInput.springFlick;
  if (ImGui::Checkbox("Spring Ball on gyro flick", &springFlick)) {
    SetSpringBallFlick(springFlick);
  }
  ItemHelp("Tilt the pad or phone up sharply to spring, like Trilogy's nunchuk flick. Works with "
           "gyro aim off. Raise the strength if it triggers by accident.");
  ImGui::BeginDisabled(!sInput.springFlick);
  float flickRate = sInput.springFlickRate;
  if (ImGui::SliderFloat("Flick strength", &flickRate, 2.f, 20.f, "%.1f rad/s")) {
    SetSpringBallFlickRate(flickRate);
  }
  ImGui::EndDisabled();
}

} // namespace PortDebug
