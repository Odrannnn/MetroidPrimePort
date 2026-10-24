// F1 overlay: The Android bridge: JNI entry points the Java side calls. State and helpers shared
// with the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_photo.h"
#include "port_pointer.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_input_bindings.h"
#include "port_input_devices.h"
#include "port_prompts.h"
#include "port_savestate.h"
#include "port_textures.h"

#if defined(__ANDROID__)
#include "touch_pad.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <jni.h>
#endif

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
// The touch overlay covers the display and consumes every touch before SDL
// sees it. While the debug overlay is open the game is paused and those touches
// belong to ImGui, so the Java side asks this and stops claiming them.
#if defined(__ANDROID__)
// The pad itself - descriptor, mapping, axis conversion, attach and detach -
// lives in platform/touch_pad.cpp so that it can be built and tested on the
// host. It used to be here, inside this #if, which meant it was never compiled
// anywhere except an Android build and no test could ever have caught a wrong
// button mapping. See tests/touch_pad.cpp.
namespace {
PortTouchPad::Pad g_touchPad;

PortTouchPad::Pad& TouchPad() {
  if (!g_touchPad.ok()) {
    g_touchPad = PortTouchPad::Attach();
    PortLog::Write("touchpad: attached id=%d gamepad=%d open=%d\n",
                   g_touchPad.id, SDL_IsGamepad(g_touchPad.id) ? 1 : 0,
                   g_touchPad.ok() ? 1 : 0);
    if (!g_touchPad.ok())
      PortLog::Write("touchpad: attach failed: %s\n", SDL_GetError());
  }
  return g_touchPad;
}
} // namespace

// KNOWN LIMITATION: a short tap can be missed.
//
// The virtual joystick API is state-sampling, not event-queueing. Setting a
// button stores the latest value and marks it changed (SDL_virtualjoystick.c:401);
// the change is only delivered at the next update, which sends whatever the
// value is *then* (:742). So a press and release that both land between two
// updates leave only the release, and the game never sees the press. A quick tap
// on A or Start can therefore do nothing, most visibly while a game frame is
// stalled. Triggers behave the same way.
//
// This is not a data race - the setters hold SDL's joystick mutex, so nothing
// tears - and it is not specific to this port; it is how SDL's virtual joystick
// works. Sustained presses and ordinary releases are unaffected, which is why it
// has not shown up as "controls don't work".
//
// Fixing it properly means latching a press until the game has sampled it, and
// the latch has to be released on an update the port does not control. That is a
// real design problem, not a two-line patch, so it is recorded rather than
// half-solved. A missed tap is recoverable by tapping again; a control that fires
// when it should not is not.

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualButton(JNIEnv*, jclass, jint button,
                                                                 jboolean down) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualButton(pad, static_cast< int >(button), down == JNI_TRUE);
  }
}

// Every overlay control's press and release, for touch: bindings in controls.toml.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchControl(JNIEnv*, jclass, jint control,
                                                                jboolean down) {
  PortInputDevices::SetTouchControl(static_cast< int >(control), down == JNI_TRUE);
}

// Whether a binding takes over the control, so the overlay skips its own button.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchControlBound(JNIEnv*, jclass, jint control) {
  return PortInputDevices::TouchControlBound(static_cast< int >(control)) ? JNI_TRUE : JNI_FALSE;
}

// What a stock control does in the ball, for its label: its Morph Ball row's
// name, "" for a ball button left without one, null for any other control.
extern "C" JNIEXPORT jstring JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchBallLabel(JNIEnv* env, jclass, jint control) {
  if (const PortInput::BallRow* row = PortInput::FindBallRow(PortInputDevices::TouchBallRow(control))) {
    return env->NewStringUTF(row->touch);
  }
  for (const PortInput::TouchBallControl& c : PortInput::kTouchBallControls) {
    if (c.control == control) return env->NewStringUTF("");
  }
  return nullptr;
}

// The actions a layout editor button can take, as key, label, key, label... in
// the action list's order (pad buttons, beams, visors, morph, the port's own).
extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchButtonActions(JNIEnv* env, jclass) {
  std::vector< std::string > items;
  for (int i = 0; i < PortInput::kActionCount; ++i) {
    const auto a = static_cast< PortInput::Action >(i);
    if (!PortInput::TouchButtonAction(a)) {
      continue;
    }
    const PortInput::ActionInfo& info = PortInput::Info(a);
    items.emplace_back(info.key);
    items.emplace_back(info.label);
  }
  jclass stringClass = env->FindClass("java/lang/String");
  if (stringClass == nullptr) {
    return nullptr;
  }
  jobjectArray out = env->NewObjectArray(static_cast< jsize >(items.size()), stringClass, nullptr);
  for (size_t i = 0; out != nullptr && i < items.size(); ++i) {
    jstring s = env->NewStringUTF(items[i].c_str());
    env->SetObjectArrayElement(out, static_cast< jsize >(i), s);
    env->DeleteLocalRef(s);
  }
  env->DeleteLocalRef(stringClass);
  return out;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualAxis(JNIEnv*, jclass, jint axis,
                                                               jfloat value) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualAxis(pad, static_cast< int >(axis), PortTouchPad::AxisValue(value));
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchAim(JNIEnv*, jclass, jfloat dxDp,
                                                            jfloat dyDp) {
  PortDebug::AddTouchAim(dxDp, dyDp);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchSpringSwipe(JNIEnv*, jclass) {
  PortDebug::TouchSpringSwipe();
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchAimDown(JNIEnv*, jclass, jboolean down) {
  PortDebug::SetTouchAimDown(down == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchAimEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchAim() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchClassic(JNIEnv*, jclass) {
  return PortDebug::TouchClassic() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchTwinStick(JNIEnv*, jclass) {
  return PortDebug::TouchTwinStick() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchWheelsEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchWheels() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchVisorTapScan(JNIEnv*, jclass) {
  return PortDebug::TouchVisorTapScan() ? JNI_TRUE : JNI_FALSE;
}

// Bits 0-3: visors owned (Combat, X-Ray, Scan, Thermal); 4-7: beams owned
// (Power, Ice, Wave, Plasma); 8-9 current visor; 10-11 current beam; 12: valid.
extern "C" JNIEXPORT jint JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeWheelOwned(JNIEnv*, jclass) {
  return static_cast< jint >(PortDebug::WheelState());
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeRequestVisor(JNIEnv*, jclass, jint visor) {
  PortDebug::RequestVisor(visor);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeRequestBeam(JNIEnv*, jclass, jint beam) {
  PortDebug::RequestBeam(beam);
}

// {width, height, ARGB pixels...} of a wheel icon (wheel 0 visor, 1 beam; item in the same order as
// nativeRequestVisor/nativeRequestBeam), or null until the HUD has loaded it.
extern "C" JNIEXPORT jintArray JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeWheelIcon(JNIEnv* env, jclass, jint wheel,
                                                             jint item) {
  if (wheel < 0 || wheel > 1 || item < 0 || item > 3) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(sWheelIconMutex);
  const WheelIcon& icon = sWheelIcons[wheel][item];
  if (icon.argb.empty()) {
    return nullptr;
  }
  const jsize count = static_cast<jsize>(icon.argb.size());
  jintArray out = env->NewIntArray(2 + count);
  if (out == nullptr) {
    return nullptr;
  }
  const jint dims[2] = {icon.w, icon.h};
  env->SetIntArrayRegion(out, 0, 2, dims);
  env->SetIntArrayRegion(out, 2, count, reinterpret_cast<const jint*>(icon.argb.data()));
  return out;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchMapTapEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchMapTap() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMinimapRect(JNIEnv* env, jclass,
                                                              jfloatArray out) {
  // x0, y0, x1, y1, then 1 when the minimap is drawn there (0: draw a map button).
  float rect[5];
  bool drawn = true;
  if (!PortDebug::TouchMapTap() || !PortDebug::MinimapRect(rect, &drawn) || out == nullptr ||
      env->GetArrayLength(out) < 5) {
    return JNI_FALSE;
  }
  rect[4] = drawn ? 1.f : 0.f;
  env->SetFloatArrayRegion(out, 0, 5, rect);
  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapPan(JNIEnv*, jclass, jfloat dxDp,
                                                         jfloat dyDp, jfloat viewHeightDp) {
  PortDebug::AddMapPan(dxDp, dyDp, viewHeightDp, 250);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapRotate(JNIEnv*, jclass, jfloat radians) {
  PortDebug::AddMapRotate(radians);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapZoom(JNIEnv*, jclass, jfloat ratio) {
  PortDebug::AddMapZoom(ratio);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapScreenOpen(JNIEnv*, jclass) {
  return PortDebug::MapScreenOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativePauseScreenOpen(JNIEnv*, jclass) {
  return PortDebug::PauseScreenOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMessageBoxOpen(JNIEnv*, jclass) {
  return PortDebug::PausedMenuOpen() && !PortDebug::PauseScreenOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativePointerMenuActive(JNIEnv*, jclass) {
  return PortPointer::ScreenActive() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapTap(JNIEnv*, jclass) {
  if (PortDebug::TouchMapTap()) {
    PortDebug::RequestMapTap();
  }
}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTapUpdateToast(JNIEnv*, jclass, jfloat x, jfloat y) {
  return PortDebug::TapUpdateToast(x, y) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeDebugOverlayVisible(JNIEnv*, jclass) {
  // Photo mode is touched like the F1 overlay: the controls hide and leave the touches to SDL, where
  // its panel takes the ones on it and the rest fly the camera (PhotoTouchEvent).
  PortSaveState::ResumeInfo promptInfo;
  bool promptYes = true;
  return PortDebug::OverlayVisible() || PortPhoto::Active() || PortSaveState::ResumePromptShown(promptInfo, promptYes)
             ? JNI_TRUE
             : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchBeamShift(JNIEnv*, jclass, jboolean held) {
  sTouchBeamShift.store(held == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchTurboFire(JNIEnv*, jclass, jboolean held) {
  sTouchTurboFire.store(held == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchTurbo(JNIEnv*, jclass) {
  return PortDebug::TouchTurboFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeSetTouchDevice(JNIEnv*, jclass, jboolean xbox) {
  sTouchActive.store(true, std::memory_order_release);
  PortPrompts::NoteTouchInput(xbox == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchColors(JNIEnv*, jclass) {
  return PortDebug::TouchColorsFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchLabels(JNIEnv*, jclass) {
  return PortDebug::TouchLabelsFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchFloatingStick(JNIEnv*, jclass) {
  return PortDebug::TouchFloatingStickFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchSideMarginDp(JNIEnv*, jclass) {
  return PortDebug::TouchSideMarginDp();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchStickInsetDp(JNIEnv*, jclass) {
  return PortDebug::TouchStickInsetDp();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchButtonInsetDp(JNIEnv*, jclass) {
  return PortDebug::TouchButtonInsetDp();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchOpacity(JNIEnv*, jclass) {
  return PortDebug::TouchOpacity();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchControlsEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchControlsEnabled() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchLayout(JNIEnv* env, jclass) {
  return env->NewStringUTF(PortDebug::TouchLayout().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeSetTouchLayout(JNIEnv* env, jclass,
                                                                  jstring layout) {
  if (layout == nullptr) {
    return;
  }
  const char* chars = env->GetStringUTFChars(layout, nullptr);
  if (chars != nullptr) {
    PortDebug::SetTouchLayout(chars);
    env->ReleaseStringUTFChars(layout, chars);
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchEditRequested(JNIEnv*, jclass) {
  return PortDebug::TakeTouchEditRequested() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeToggleDebugOverlay(JNIEnv*, jclass) {
  PortDebug::RequestToggle();
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeTexturePackStatus(JNIEnv* env, jclass,
                                                                       jstring status) {
  const char* chars = env->GetStringUTFChars(status, nullptr);
  if (chars != nullptr) {
    PortDebug::SetTexturePackStatus(chars);
    env->ReleaseStringUTFChars(status, chars);
  }
}

// The copy landed in <user root>.new; the next frame swaps it in.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeTexturePackReady(JNIEnv*, jclass) {
  PortTextures::RequestUserPackReload();
}

// A file picked for the Remastered import, while this process lived. The
// address is also in RemasteredPickFile, for when it did not.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeRemasteredPicked(JNIEnv* env, jclass, jint which,
                                                                      jstring uri) {
  const char* chars = env->GetStringUTFChars(uri, nullptr);
  if (chars != nullptr) {
    std::lock_guard lock(PortDebug::sRemasteredPickMutex);
    if (which == PortDebug::kPalLanguagesPick) {
      PortDebug::sPalLanguagesPick = chars;
    } else if (which == PortDebug::kFlaahgraPick) {
      PortDebug::sFlaahgraPick = chars;
    } else if (which == PortDebug::kDreadPick) {
      PortDebug::sDreadPick = chars;
    } else {
      PortDebug::sRemasteredPicks.emplace_back(int(which), chars);
    }
    env->ReleaseStringUTFChars(uri, chars);
  }
}

// Whether a real pad, keyboard or mouse was used since the last call.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTakePhysicalInput(JNIEnv*, jclass) {
  return sPhysicalInput.exchange(false, std::memory_order_acq_rel) ? JNI_TRUE : JNI_FALSE;
}
#endif
