#pragma once

#include <string>

class CFinalInput;
class CStateManager;

// Photo mode: the free camera with the simulation frozen, the HUD, arm cannon, helmet and
// visor effects out of the picture, a field of view and a roll of its own, and a capture
// that saves a PNG (optionally supersampled). Entered from the pause menu, the Photo mode
// binding (F8), F1 > Game or the console's `photo`; the panel is drawn by debug_ui.cpp.
namespace PortPhoto {

bool Active();

// Asks for photo mode to start or stop at the next game tick; a start that gameplay does
// not allow (Samus dead, a cutscene, no game) is refused with a toast.
void RequestEnter();
void RequestExit();
void RequestToggle();
// From the pause menu: the pause screen closes, then photo mode starts once play resumes.
void RequestEnterAfterPause();
bool EnterPending();
// Immediately; for the console, which runs inside the game tick. False and `why` if refused.
bool Enter(CStateManager& mgr, std::string& why);
void Exit();

// Once per game tick while the game flow is in play (CMFGame): resolves requests and
// leaves photo mode if the game it was entered in goes away.
void Tick(CStateManager& mgr, bool inPlay);
// Once per rendered frame (debug_ui DrawUI): the capture sequence and request timeouts.
void Frame();
// The game's pad input: A captures, B or Start leaves, Y hides the panel, X with the d-pad
// left and right zooms, the d-pad alone rolls; the rest is the free camera's. Returns true
// when the input was taken and the game must not see it.
bool Input(const CFinalInput& input);

// The settings of the shot. Field of view is the vertical degrees at 4:3; roll is degrees.
float Fov();
void SetFov(float degrees);
float Roll();
void SetRoll(float degrees);
bool ShowSamus();
void SetShowSamus(bool show);
// Camera speed (metres per second) is remembered between sessions.
float Speed();
void SetSpeed(float speed);
// Capture size multiplier: 1, 2 or 4 (saved).
int Supersample();
void SetSupersample(int factor);

bool PanelShown();
void SetPanelShown(bool shown);
// The panel is up and wants the cursor (not while the right mouse button is held).
bool PanelWantsCursor();
// Desktop: the mouse turns the camera, whatever the mouse aim setting: with the panel hidden, or
// while the right button is held.
bool MouseLook();
// The pad drives the panel (Y toggles it): the d-pad and A work its widgets and the camera is
// still. Off, the pad flies the camera and rolls it.
bool PanelFocused();

// Saves a PNG of the picture as the game draws it, without the panel, overlays or toasts.
// `factor` 0 uses Supersample().
bool RequestCapture(int factor = 0);
bool Capturing();
// What the last capture did, for the toast and the console.
struct CaptureResult {
  int state = 0; // 0 none yet, 1 saved, -1 failed
  std::string path;
  unsigned width = 0;
  unsigned height = 0;
  float scale = 1.f; // render scale the shot was drawn at
  bool clamped = false;
};
CaptureResult LastCapture();
// A message for the toast once, then empty.
std::string TakeMessage();

} // namespace PortPhoto
