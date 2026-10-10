#ifndef METROID_PRIME_PORT_PORT_DEBUG_H
#define METROID_PRIME_PORT_PORT_DEBUG_H

#include "port_input_settings.h"
#include "port_rando_gen.h"

#include <cstdint>
#include <string>

// Runtime debug settings shared between the platform layer and the game.
// Defaults come from environment variables so existing workflows keep working,
// and the in-game debug window can toggle them live.

class CStateManager;
class CGuiModel;

namespace PortDebug {

// Live game state for the debug menu (set every simulation tick by
// CStateManager::Update). Null before gameplay starts.
void SetStateManager(CStateManager* mgr);
CStateManager* StateManager();
// Where the world is drawn from and which way it looks (the free camera's while it is
// on); false outside a world.
bool ViewRay(float origin[3], float forward[3]);
// What PBR surfaces show in place of their shaded result (GXSetPBRDebugView); 0 is off.
int PbrViewCount();
const char* PbrViewName(int view);
int PbrView();
void SetPbrView(int view);
// "drawid" is the last view: every draw is its serial in a flat colour (see PortDrawLog). Called by SetPbrView
// so the model draws know to number themselves.
void NoteDrawIdMode(bool on);
// Requests an area change; consumed and executed by the game update so it does
// not run from the render/UI path.
void RequestTeleport(int areaId);
bool ConsumeTeleportRequest(int& areaId);
// Requests a jump to a different world (MLVL). areaAssetId is a MREA asset id,
// or 0 to land in the world's default area. The game update runs the same
// restart the in-game world teleporters use, so the world is fully reloaded.
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId);
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId);
// Opt-in pickup-dump tour (MP_RANDO_SWEEP=1, with MP_RANDO_DUMP=1). Advance
// once per gameplay simulation tick; returns true when a restart was queued.
// Duplicate sweep requests and requests conflicting with a teleport are ignored.
void RequestWorldSweep();
bool ConsumeWorldSweepRequest(CStateManager& mgr);

// Disc image chosen on a previous launch, from the settings file. SetDiscPath
// marks the settings dirty so the choice is written back out on exit.
void LoadDiscPath();
const char* DiscPath();
void SetDiscPath(const char* path);

// Fast iteration
bool FastBoot();
// MP_BOOT_WORLD=<MLVL hex>[:<MREA hex>]: no splash screens or front end; a new game
// starts straight in that world (its default area without an MREA). Tests only.
bool BootWorld(uint32_t& worldId, uint32_t& areaAssetId);
// MP_SKIP_CUTSCENES / MP_CUTSCENE_SPEED (tests only; not a saved setting).
bool SkipCutscenes();
float CutsceneSpeed();
// Simulation tick rate. 60 is console-accurate; higher values run the tick at
// the display rate instead of interpolating presentation. Experimental.
unsigned SimRate();
void SetSimRate(unsigned hz);
// The current simulation step in seconds (1 / SimRate()).
float SimPeriod();
// When enabled the simulation step follows the measured frame time (clamped to
// a sane range) instead of SimRate(), so a variable frame rate is matched
// tick-for-tick. Experimental.
bool SimAdaptive();
void SetSimAdaptive(bool enabled);
// Smooth uncapped frames (port_settings.ini smooth_frames, on by default): sets
// the four parts below together. Setting a part on its own (console `interp`)
// is for testing and isn't saved.
bool SmoothFrames();
void SetSmoothFrames(bool enabled);
// Per-frame look (docs/FRAME_INTERPOLATION.md, phase 1): with the frame limiter
// off, frames between ticks show the look input the next tick will apply.
bool FrameInterpolation();
void SetFrameInterpolation(bool enabled);
// Actor transform smoothing (phase 2): with the frame limiter off, actors draw
// at a blend of their previous and current tick transforms.
bool ActorInterpolation();
void SetActorInterpolation(bool enabled);
// Pose smoothing (phase 3): with the frame limiter off, skinned models draw a
// blend of their previous and current tick poses.
bool PoseInterpolation();
void SetPoseInterpolation(bool enabled);
// Keeps a room geometry mod's models on the GPU instead of sending them every frame
// (port_settings.ini room_geo_gpu; an old room_geo_resident=1 still counts). On by default.
// Aurora sizes its buffers for it at
// startup, so a change applies from the next start: RoomGeoResidentAtStartup reads the
// settings file before the rest of them are loaded.
bool RoomGeoResident();
void SetRoomGeoResident(bool enabled);
bool RoomGeoResidentAtStartup();
// Particle smoothing (phase 4): with the frame limiter off, particle systems
// draw between their previous and current tick frames.
bool ParticleInterpolation();
void SetParticleInterpolation(bool enabled);
// Frame interpolation tests (phase 5). MP_PRESENT_T=<0..1> or "cycle" (console
// `present`) forces the presentation factor, even under MP_TURBO or with the
// frame limiter on; cycle steps through 0, 0.25, 0.5 and 0.75 per drawn frame;
// "tick" draws the plain tick state (-1) as the frame limiter does.
// Replaces t and returns true when forced. Not saved.
bool PresentOverride(float& t);
// value in [0,1] = fixed, kPresentCycle = cycle, kPresentTick = tick state,
// anything negative = off.
constexpr float kPresentCycle = 2.f;
constexpr float kPresentTick = 3.f;
void SetPresentOverride(float value);
float PresentOverrideValue();
// Console `hold`/`step`: while held, main loops run no ticks except the ones
// queued by StepTicks, so one tick state can be drawn at several factors.
bool TickHold();
void SetTickHold(bool held);
void StepTicks(unsigned count);
unsigned PendingHeldTicks();
unsigned TakeHeldTicks();
// MP_TURBO[=<ticks>]: lockstep for tests. Every loop runs exactly <ticks> fixed
// ticks (default 1, at most 16) and nothing waits for the wall clock, so a run
// goes as fast as the machine can render it; more ticks per frame skip
// presents, which is what limits a run under Xvfb. Game time stays exact per
// tick; audio and streams do not keep up. Not saved to the settings file.
bool Turbo();
unsigned TurboTicks();
// The dt of the simulation tick currently being processed, set by
// CGameArchitectureSupport::UpdateTicks. Game constants authored per 60 Hz tick
// (friction, damping) scale by this so they stay real-time at any rate.
float TickPeriod();
void SetTickPeriod(float dt);
// The same step expressed in 60 Hz frames (1.0 at 60 Hz). Per-tick counters and
// cadences add this instead of 1 so their real-time timing is unchanged.
float TickFrames();

// Presentation
bool FrameLimitEnabled();
void SetFrameLimitEnabled(bool enabled);
void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented);
bool VsyncEnabled();
void SetVsyncEnabled(bool enabled);
// Setting `fullscreen`: a borderless fullscreen window on desktop (F11 toggles
// it), the status and navigation bars hidden on Android. Applied at window
// creation; Set changes the live window too.
bool Fullscreen();
void SetFullscreen(bool enabled);
// 0 = auto (native, driven by the display scale), otherwise a fixed multiplier.
float RenderScale();
void SetRenderScale(float scale);
// Dynamic resolution: draws the EFB between DynamicResMin and RenderScale to
// hold DynamicResTarget fps (0 = the frame cap). False when off.
bool DynamicRes();
int DynamicResTarget();
float DynamicResMin();
// Rendering aspect ratio. kAspect_4_3 is the game's original 640x480.
// kAspect_16_9 widens to 16:9; kAspect_Window follows the window and updates
// live as it is resized (the default).
enum EAspectMode {
  kAspect_4_3 = 0,
  kAspect_16_9,
  kAspect_Window,
};
EAspectMode AspectMode();
void SetAspectMode(EAspectMode mode);
// Current size of the game window in window coordinates; false before it exists.
bool WindowSize(int& width, int& height);
// Framebuffer width that is shown at 4:3 for the given height: the width itself
// in 4:3 mode (640x448 is displayed at 4:3), height * 4 / 3 otherwise, since the
// widened framebuffers have square pixels. Fits 4:3 art into a wider viewport.
inline int FourThreeWidth(int width, int height) {
  return AspectMode() == kAspect_4_3 ? width : height * 4 / 3;
}
// Widescreen HUD: keep each HUD element's shape but spread its position about
// the screen centre so edge elements reach the true wide corners. Only affects
// the aspect-matched in-game HUD frames.
bool HudWide();
void SetHudWide(bool enabled);
// Scripted 16:9 cutscene bars (CCameraFilterPass kFS_CinemaBars). Off by default:
// below 16:9 the cinematic camera already renders the full shot, so it fills the
// screen instead.
bool CinemaBars();
// The scan visor's magnifier draws the world a second time through a camera zoomed by the
// window's magnification (CMFGame::Draw), instead of stretching a copy of the screen. Off by
// default and under Original experience.
bool SharpScanWindow();
// HUD scale in percent (50-100). Compact HUD elements shrink toward the nearest
// screen edge or corner; screen-spanning decoration keeps its size. Only the
// combat/scan/ball HUD frames and the minimap, not the helmet or menus.
const int kHudScaleMin = 50;
const int kHudScaleMax = 100;
int HudScale();
void SetHudScale(int percent);
// The centre crosshair (free aim, mouse aim, twin stick): opacity in percent,
// and an optional colour that replaces the game's (false = the game's own).
const int kCrosshairOpacityMin = 0;
const int kCrosshairOpacityMax = 100;
int CrosshairOpacity();
bool CrosshairColor(float rgb[3]);
// Hide the helmet frame (the visor rim and its glow and lights).
bool HideHelmet();
void SetHideHelmet(bool enabled);
// Hide visor effects: steam, Samus's face reflection, and rain, water, goo and
// hit splashes on the visor.
bool HideVisorEffects();
void SetHideVisorEffects(bool enabled);
// Every world's map shows as if its map station had been used; the save is
// not changed.
bool RevealMap();
void SetRevealMap(bool enabled);
// A white dot on the map for each item pickup not yet collected. Randomizer
// and Archipelago games force it on (PortMapPickups::Active).
bool MapPickups();
void SetMapPickups(bool enabled);
// In an Archipelago game, colour the dots by the seed's logic like a tracker
// (green in logic, yellow sequence break, blue visible only, red out of reach,
// grey checked). On by default; off gives the plain white dots.
bool MapLogicColors();
void SetMapLogicColors(bool enabled);
// The apworld's staggered_suit_damage, which the server doesn't send: 0 the
// game's rule, 1 progressive (the apworld's default, and the port's), 2
// additive. Only an Archipelago seed uses it.
int ApSuitDamage();
void SetApSuitDamage(int mode);
// randomprime's skippable cutscenes: every cinematic can be skipped with the
// usual button. Rooms load patched, so a change applies to rooms loaded
// afterwards. Randomizer and Archipelago games force it on
// (PortSkipCutscenes::Active).
bool SkippableCutscenes();
void SetSkippableCutscenes(bool enabled);
// The language of the game's text: "" for the disc's English, else one of the
// codes in PortRemastered::kTextLanguages, which a Remastered import adds to the
// string tables. A table without it shows English. Read at every
// CStringTable::GetString, so a change applies to text fetched afterwards.
// MP_LANGUAGE sets it for one run.
const char* TextLanguage();
void SetTextLanguage(const char* code);
// The elevator ride between worlds (CWorldTransManager). Retail holds it at
// least 5 s whatever the load takes; the port loads in well under that.
enum EElevatorRide { kElevatorRide_Original, kElevatorRide_Fast, kElevatorRide_Skip };
EElevatorRide ElevatorRide();
void SetElevatorRide(EElevatorRide mode);
// First-person vertical field of view in degrees (retail 55). The arm cannon is
// drawn at the retail FOV whatever this is, like a view-model FOV.
const float kFovRetail = 55.f;
const float kFovMin = 45.f;
const float kFovMax = 90.f;
float FirstPersonFov();
void SetFirstPersonFov(float degrees);
// Multisample anti-aliasing (1 = off, or 4) and the max texture anisotropy
// (1-16, retail-style mipmapped textures ask for the max). Applied next frame.
int Msaa();
void SetMsaa(int samples);
int Anisotropy();
void SetAnisotropy(int level);
// Setting `opengles`: start on Dawn's OpenGL ES backend instead of Vulkan, for
// drivers that draw wrong on Vulkan (Adreno 7xx, issue #7). Read at window
// creation, so it takes a restart; aurora falls back to Vulkan if it fails.
bool OpenGles();
void SetOpenGles(bool enabled);
// Setting `gpu_driver`: the custom Vulkan driver (PortGpuDriver id) to load at the
// next start, "" = the system's. Android only.
const std::string& GpuDriver();
void SetGpuDriver(const std::string& id);
// Setting `storage_clamp` (Auto/Off/On): sets MP_STORAGE_CLAMP for aurora's shader
// generator unless it's already in the environment. Call before aurora_initialize.
void ApplyStorageClamp();
// Extras normally earned by finishing the game (or, for the Fusion Suit, by a
// GBA link to Metroid Fusion). They only change what the title screen offers;
// nothing is written into the save's persistent flags.
bool UnlockHardMode();
void SetUnlockHardMode(bool enabled);
bool UnlockFusionSuit();
void SetUnlockFusionSuit(bool enabled);
bool UnlockGalleries();
void SetUnlockGalleries(bool enabled);

// Mouse FPS mode owns aim only in playable first person; target locks retain
// their native camera and synchronize the mouse angles for a clean handoff.
// Motion/buttons are accepted only while SDL owns relative capture.
bool MouseAim();
void SetMouseAim(bool enabled);
// Twin-stick: the right stick aims the first-person camera directly (through the
// same aim state as the mouse) and is consumed, so it no longer drives the
// game's free-look. Works with or without mouse aim.
// Reads false while touch is in use (TouchActive), as do SwapScanXray, ShiftBinding(2) (-1)
// and PadAltButton (-1): the touch overlay always does its GameCube-labelled actions.
bool TwinStick();
// The stored pad Twin Stick setting, whatever the touch layout: what the pause option edits.
bool PadTwinStick();
// Android: touch was the last input and the F1 menu is closed. False on desktop.
bool TouchActive();
// True on Android when the modern (non-classic) touch layout is the active device: drag aims like a mouse.
bool TouchDirectAim();
// The direct aim path is active: mouse aim, twin stick or the modern touch layout.
bool DirectAim();
bool TouchClassic();
void SetTouchClassic(bool on);
bool TouchTwinStick();
void SetTouchTwinStick(bool on);
void SetTwinStick(bool enabled);
// Right stick Y (-1..1) before twin-stick consumed it, for the Spring Ball;
// 0 when twin-stick is off (the game input still carries it then).
float TwinStickRightY();
void SetTwinStickRightY(float y);
// The bound beam shift is held in game this poll (no overlay, window focused),
// which springs the Spring Ball in morph ball, as X does in Remastered.
bool BeamShiftHeld();
// The touch twin layout's Beam button is held: the D-pad picks beams (false off Android / without touch).
bool TouchBeamShift();
void SetBeamShiftHeld(bool held);
// The touch overlay's Turbo button is held (false off Android / without touch).
bool TouchTurboFire();
// Spring Ball (C-stick up in morph ball, as in Metroid Prime Trilogy) once the
// Morph Ball Bombs are held. A connected Archipelago seed overrides it.
bool SpringBall();
void SetSpringBall(bool enabled);
// The Scan and X-Ray visors trade D-pad directions (Remastered's Dual Sticks
// layout), off by default.
bool SwapScanXray();
void SetSwapScanXray(bool enabled);
// The beam shift's bindings (read by PortInputDevices): slots 0 and 1 are keys
// or mouse buttons (scancode or PAD_KEY_MOUSE_*), slot 2 a controller button
// (SDL gamepad button or PAD_NATIVE_BUTTON_TRIGGER_*); -1 for none.
int ShiftBinding(int slot);
void SetShiftBinding(int slot, int code);
// The turbo fire's bindings (read by PortInputDevices), laid out as ShiftBinding's
// (slot 2 reads -1 while touch is in use); all -1 (none) by default.
int TurboBinding(int slot);
void SetTurboBinding(int slot, int code);
// A second controller button for a GameCube button (Aurora maps one each),
// ORed in by CDolphinController: `bit` is the PAD_BUTTON_* / PAD_TRIGGER_* bit's
// position, the code as ShiftBinding's slot 2; -1 for none.
constexpr int kPadAltCount = PortInput::kPadAltCount;
int PadAltButton(int bit);
void SetPadAltButton(int bit, int code);
// What mouse button `button` (0 left, 1 middle, 2 right, 3 X1, 4 X2) does under
// mouse aim: a PortInputMap::EMouseAction.
int MouseAction(int button);
void SetMouseAction(int button, int action);
// Speedrun support: an on-screen in-game time (the play time the save shows)
// and the LiveSplit Server client (port_livesplit.h), which connects to
// "host:port" while enabled.
bool SpeedrunTimer();
void SetSpeedrunTimer(bool enabled);
bool LiveSplit();
void SetLiveSplit(bool enabled);
std::string LiveSplitAddress();
void SetLiveSplitAddress(const std::string& address);
// Discord Rich Presence (port_discord.h), connecting while enabled and an
// application id (digits only) is set.
bool DiscordPresence();
void SetDiscordPresence(bool enabled);
// Mods folder (port_mods.h): all mods on or off, and the folder names turned
// off, '/'-separated. Both take effect on the next launch.
bool ModsEnabled();
void SetModsEnabled(bool enabled);
// Original experience: the game as it shipped. While on, the getters for the
// port's additions (render scale, MSAA, aspect, wide HUD, FOV, interpolation,
// sim rate, mods, unlocks, the gameplay assists, turbo, ...) return retail's
// values; the saved settings are not changed, so turning it off restores them.
// Input aids, cheats, save states, the randomizer/Archipelago, timers and
// Discord follow their own settings.
bool OriginalExperience();
void SetOriginalExperience(bool enabled);
std::string ModsDisabled();
void SetModsDisabled(const std::string& list);
// Starts a Remastered import (port_remastered_import.h) with the mods unloaded
// while it runs; they load again, the new import included, when it ends.
// False when one is already running or there is no mods folder.
bool StartRemasteredImport(const std::string& image, const std::string& keys);
// Memory card transfer to and from Dolphin (port_gci.h), for the overlay and
// the console. Each returns a message for the user. Imports are refused in
// game; `path` may be a .gci, a raw card image, a folder of .gci files or an
// Android content:// URI, and an export to a path ending in .raw writes a card
// image instead of .gci files.
std::string CardList();
std::string CardImport(const std::string& path);
std::string CardExport(const std::string& dest);
std::string CardImportDolphin();
std::string CardExportDolphin();
// Cheat: the player takes no damage (F1 > Debug > Cheats, MP_GODMODE, console `god`).
bool Invulnerable();
void SetInvulnerable(bool enabled);
// Write the log to <user folder>/metroid_prime_port.log (port_log_file.h). MP_LOG_FILE=1
// turns it on for one run without changing the setting.
bool LogFile();
void SetLogFile(bool enabled);
// Fast Morph, as in Metroid Prime 4: short morph/unmorph transitions that keep
// momentum (capped at walking speed when unmorphing on the ground).
bool FastMorph();
void SetFastMorph(bool enabled);
// Toggle Lock-On: an L press that locks on (target, scan, grapple) latches
// until pressed again, a press without a lock is a plain hold, and a lock that
// ends lets go by itself. Sticky Charge: letting go
// of a long A hold keeps the beam charging until the next press fires it. Both
// apply only unmorphed, in gameplay (port_hold_toggle.h).
bool LockOnToggle();
void SetLockOnToggle(bool enabled);
bool StickyCharge();
void SetStickyCharge(bool enabled);
// Remastered charge: holding fire shoots a few quick shots after the press shot
// (Power 2, Wave 1, Plasma 1, Ice none), then charges faster (without the Charge
// Beam: stops until release), with Remastered's per-beam timings
// (CPlayerGun::PortRapidCharge*).
bool RapidCharge();
// The Randomizer page's saved options (rando_settings= in the settings file).
PortRandoGen::Settings RandoSettings();
void SetRapidCharge(bool enabled);
// Remastered movement under twin stick/mouse aim: one force along the stick and
// Remastered's planar friction and air drag (CPlayer::ComputeMovement, issue #46).
bool RemasteredMovement();
void SetRemasteredMovement(bool enabled);
// Spring Ball on a gyro flick (pad or phone tilted up sharply, like Trilogy's
// nunchuk flick), on top of C-stick up. Rate is the pitch speed in rad/s a flick
// must pass. The gyro source is the aim's.
bool SpringBallFlick();
void SetSpringBallFlick(bool enabled);
float SpringBallFlickRate();
void SetSpringBallFlickRate(float radiansPerSecond);
// True for a short while after a flick, so one just before landing still counts.
bool SpringBallFlickPending();
void ClearSpringBallFlick();
// Debug console: stand-in gyro rates (rad/s) instead of the sensors.
void SetGyroOverride(bool active, float pitch, float yaw);
// Aim travel in pixels per second at full stick deflection (scaled by the mouse
// sensitivity, so both share the same feel).
float StickAimRate();
void SetStickAimRate(float pixelsPerSecond);
// Gyro aiming. Mode: 0 off, 1 aim while the hold input is down, 2 always aim.
// Source: 0 auto (a pad's gyro if it has one, else the phone's), 1 controller
// only, 2 the device's own gyro (Android phones). Rate is aim pixels per second
// per radian per second of rotation, so it reads like the stick aim speed.
int GyroMode();
void SetGyroMode(int mode);
int GyroSource();
void SetGyroSource(int source);
float GyroRate();
void SetGyroRate(float pixelsPerSecondPerRad);
// Reads the gyro, feeds the aim and spots Spring Ball flicks. Call once per
// tick, before the frame.
void PollGyro();
// Short description of what the gyro is doing, for the overlay.
const char* GyroStatus();
// Feeds a normalised right-stick vector (-1..1, x right, y up) for this tick.
void AddStickAim(float x, float y, float dt);void SetMouseCaptured(bool captured);
bool MouseCaptured();
bool MouseGameplayActive();
void SetMouseGameplayActive(bool active);
bool MouseInvertX();
bool MouseInvertY();
bool MouseButtons();
bool MouseCrosshair();
// Crosshair size in percent while mouse aim or twin stick is on (the retail
// free-aim crosshair is sized for a held R, and it stays up the whole time
// under those modes). Retail R free-aim keeps 100.
const int kCrosshairSizeMin = PortInput::kCrosshairSizeMin;
const int kCrosshairSizeMax = PortInput::kCrosshairSizeMax;
const int kCrosshairSizeDefault = PortInput::kCrosshairSizeDefault;
int CrosshairSize();
void SetCrosshairSize(int percent);
unsigned MouseWeaponButtons(unsigned held);
// Outside mouse gameplay (morph ball, text boxes, menus) the buttons bound to A
// or B still press them; returns the held buttons (SDL masks) that may count.
unsigned MouseMenuButtons(unsigned held, bool focused);
// Real-mouse button state (SDL_BUTTON_*MASK), fed from the event loop. Touch-
// and pen-synthesised mouse buttons are left out; see PortMouse::HeldButtons.
void NoteMouseButton(bool synthetic, unsigned mask, bool down);
void ClearMouseButtons();
unsigned MouseHeldButtons();
// The "newer release" toast: while it shows, the cursor stays visible outside
// mouse capture, and a tap inside it (window-relative 0..1 coordinates, from a
// finger event: in relative mouse mode SDL drops a touch's mouse position)
// opens the release page (on the next frame; true if it hit). Thread-safe.
bool UpdateToastShowing();
bool TapUpdateToast(float x, float y);
// Called during simulation, using the effective unbobbed camera direction.
bool UpdateMouseAim(bool active, bool locked, float x, float y, float z);
void SynchronizeMouseAim(float x, float y, float z);
void ResetMouseAim();
float MouseSensitivity();
void SetMouseSensitivity(float radiansPerPixel);
// Called from the input event loop as relative motion arrives.
void AddMouseDelta(float dx, float dy);
// Called once per simulated frame to latch the deltas for that frame.
void BeginFrameMouse();
// Touch aim (Android drag-to-turn): finger travel in dp, right/down positive.
// Thread-safe; drained by BeginFrameMouse.
bool TouchAim();
void SetTouchAim(bool on);
float TouchAimSpeed();
void SetTouchAimSpeed(float pixelsPerDp);
void AddTouchAim(float dxDp, float dyDp);
// GameCube scheme (neither mouse aim nor twin stick): CPlayer turns by the touch
// travel and holds a free-look pitch while a touch-aim finger is down.
// TakeTouchLook returns this tick's world yaw/pitch change in radians (once per
// tick) and whether touch aim is usable. The finger state comes from the Android
// overlay, or HoldTouchAim (console) for that many seconds.
void SetTouchAimDown(bool down);
void HoldTouchAim(float seconds);
bool TouchAimDown();
bool TakeTouchLook(float& dyaw, float& dpitch);
// Tap the minimap to open the map (Android touch overlay). The HUD publishes the
// minimap's screen rect (0..1 of the window, origin top-left) each frame it is
// drawn; MinimapRect fills x0,y0,x1,y1 and returns false when it isn't shown.
// `drawn` is false where the map opens but the minimap isn't drawn (the visors
// other than Combat): the overlay shows a map button in the rect instead.
// Hold-and-slide beam and visor wheels on the Android overlay. The player
// publishes WheelState each frame (bits 0-3 visors owned in EPlayerVisor order
// Combat/X-Ray/Scan/Thermal, 4-7 beams owned in EBeamId order Power/Ice/Wave/
// Plasma, 8-9 current visor, 10-11 current beam, 12 valid, 13 morphed or
// morphing; 0 when stale). A
// request is read by ControlMapper for ~120 ms as a press of that command.
bool TouchWheels();
void SetTouchWheels(bool on);
bool TouchVisorTapScan();
void SetTouchVisorTapScan(bool on);
void SetWheelState(uint32_t mask);
uint32_t WheelState();
// CInGameGuiManager publishes each frame whether the pause menu is up (stale = closed).
void SetPauseScreenOpen(bool open);
bool PauseScreenOpen();
void RequestVisor(int visor);
void RequestBeam(int beam);
// The HUD's beam/visor menu icons for the touch wheels (Android), decoded to RGBA8 when the HUD
// frame is up. wheel 0 = visor, 1 = beam; icons[i] is the menu item i's icon widget (EPlayerVisor
// / EBeamId order). Game thread, each frame until all four are taken.
void CaptureWheelIcons(int wheel, CGuiModel* const* icons);
bool VisorRequested(int visor);
bool BeamRequested(int beam);
// The binding engine's direct actions (PortInputDevices): one Spring Ball, and the
// save-state hotkeys (1 save, 2 load; F5/F9 send the same).
void RequestSpringBall();
bool SpringBallRequested();
void RequestSaveStateHotkey(int which);
bool TouchMapTap();
void SetTouchMapTap(bool on);
void SetMinimapRect(bool valid, bool drawn, float x0, float y0, float x1, float y1);
bool MinimapRect(float* out4, bool* drawn = nullptr);
void RequestMapTap();
// Pad poll hook: true for the one poll where a requested tap reads Z held.
bool ConsumeMapTapZ();
// Drag to pan the map screen: CAutoMapper publishes SetMapScreenOpen each frame,
// the overlay (or the console's mappan) adds dp deltas with the view height in
// dp, and CAutoMapper drains them with TakeMapPan (true while a finger is on it).
void SetMapScreenOpen(bool open);
bool MapScreenOpen();
void AddMapPan(float dxDp, float dyDp, float viewHeightDp, int holdMs = 250);
void AddMapRotate(float radians);
float TakeMapRotate();
void AddMapZoom(float ratio);
float TakeMapZoom();
bool TakeMapPan(float* dxDp, float* dyDp, float* viewHeightDp);
void GetFrameMouseDelta(float& dx, float& dy);
// The yaw/pitch change (radians) the next tick's look input will apply, as seen
// a fraction of a tick after the last one. False when there is none to show.
bool PresentedAimDelta(float fraction, float& dyaw, float& dpitch);
// Mouse-look camera angles (radians): world yaw and pitch. The game owns the
// update and applies them to the first-person camera.
float AimYaw();
float AimPitch();
bool AimInitialized();

// Audio paths
bool AiAudioEnabled();
void SetAiAudioEnabled(bool enabled);
bool MusyxAudioEnabled();
void SetMusyxAudioEnabled(bool enabled);

// Session
void RequestReset();
bool ConsumeResetRequest();

bool Visible();
// Writes the settings file now instead of waiting for the next overlay frame,
// so a disc chosen during startup is remembered even if no frame is drawn yet.
void SaveSettingsNow();
// Puts the game's own options (Reverse Y Axis, rumble, volumes...) saved in the
// settings file back into the game state. Until it runs, they aren't recorded.
void RestoreGameOptions();
// Copies every setting and any MP_* variable into the log; settings changed
// later in the session are logged as they're saved.
void LogSettings();
// The last session ended because a read of the disc image failed: the overlay
// shows a red alert for a while once the game runs.
void NoteDiscReadFailedLastSession();
// Thread-safe snapshot of the overlay's visibility, for the Android touch
// controls. Unlike Visible() it performs no lazy initialization, so it is safe
// to call from the UI thread.
bool OverlayVisible();
// Same, for whether the Android touch overlay draws the GameCube pad's colours
// rather than plain translucent buttons. Like OverlayVisible(), performs no lazy
// initialization, so it is safe to call from the UI thread.
bool TouchColorsFlag();
// Whether it writes each button's function under its letter. Same rules.
bool TouchLabelsFlag();
// Whether it shows the Turbo fire button. Same rules.
bool TouchTurboFlag();
// Whether the left stick floats (hidden, centred where the left half is touched). Same rules.
bool TouchFloatingStickFlag();
// The touch overlay's side margin, the left stick's extra inset and the face
// buttons' extra inset, in dp. Also safe to call from the UI thread.
float TouchSideMarginDp();
float TouchStickInsetDp();
float TouchButtonInsetDp();
// Per-control offset and size, an opaque `<id>:<dx>,<dy>,<scale>;...` string the
// Android touch view owns. Setting it saves the config on the next frame.
std::string TouchLayout();
void SetTouchLayout(const std::string& layout);
// True once after F1's "Edit layout" was pressed.
bool TakeTouchEditRequested();
void Toggle();
// Asks for the overlay to be toggled on the next frame. Safe to call from any
// thread, unlike Toggle(), which touches ImGui state.
void RequestToggle();
// Feeds the pad into ImGui's gamepad navigation, applies any requested toggle,
// and handles F1. Call once per frame before the frame is built.
void UpdateControllerNav();

// GPU self-test (F1 > Video > Compatibility, console `gpuselftest`, or MP_GPU_SELFTEST=1 once after the first
// frames): renders known patterns offscreen, reads them back and logs "gpu selftest: <case>: PASS|FAIL".
void RequestGpuSelfTest();
// Runs a requested self-test. Call right after a frame begins, before the game draws; it resets the
// game's cached GX state when it ran.
void RunGpuSelfTestIfRequested();

// Builds the debug windows for the current ImGui frame. Call once per presented
// frame, after Aurora begins the frame and before it ends it.
void DrawUI();

} // namespace PortDebug

#endif // METROID_PRIME_PORT_PORT_DEBUG_H
