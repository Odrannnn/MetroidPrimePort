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

// --settings-selftest: checks that every settings key reads back as written,
// without starting the game. Returns the process exit code. Never saves.
int SettingsSelfTest();

// Live game state for the debug menu (set every simulation tick by
// CStateManager::Update). Null before gameplay starts.
void SetStateManager(CStateManager* mgr);
CStateManager* StateManager();
// The "quick_resume" setting: write a resume state on quit or background.
bool QuickResume();
// The "savestate_room_state" setting: save states keep enemies and room objects.
bool SaveStateRoomState();
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
// Frames a second the main loop is paced to: 60 under the 60 FPS cap, else the
// FPS cap setting (fps_cap=, MP_FPS_CAP); 0 = unlimited.
int FrameRateCap();
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
// Photo mode captures: the configured scale (auto: the scale the window's own size
// is, as a multiple of what 1x draws), and a temporary override
// (0 = none) that also holds dynamic resolution still.
float BaseRenderScale();
void SetCaptureRenderScale(float scale);
// The lowest scale dynamic resolution may be set to drop to.
constexpr float kDynamicResMinLowest = 0.25f;
// Dynamic resolution: draws the EFB between DynamicResMin and RenderScale to
// hold DynamicResTarget fps (0 = the frame cap). False when off. With auto render
// scale the top is the window's own scale.
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
// The morph ball looks retail lacks, made from Varia's at boot (port_ball_looks.h): a Power
// Suit spider ball instead of the Gravity Suit's, and Gravity and Phazon Suit balls instead
// of the Power Suit's. On by default, off under Original experience. Setting it reloads the
// mods, which serve the looks.
bool GeneratedBallLooks();
void SetGeneratedBallLooks(bool enabled);
// The glyph set the on-screen prompts are forced to ("xbox", "playstation", "switch", "gamecube",
// "keyboard"), or nullptr to follow the input in use. Thread-safe.
const char* PromptGlyphs();
// HUD scale in percent (50-100). Compact HUD elements shrink toward the nearest
// screen edge or corner; screen-spanning decoration keeps its size. Only the
// combat/scan/ball HUD frames and the minimap, not the helmet or menus.
const int kHudScaleMin = 50;
const int kHudScaleMax = 100;
int HudScale();
void SetHudScale(int percent);
// How far the widescreen HUD spread moves the scan visor's side panels and their images, in
// percent of the full spread (0 keeps them at their 4:3 place near the centre, 100 takes them
// out to the screen edges with the rest of the HUD). Touch controls cover the edges on phones.
const int kScanSpreadMin = 0;
const int kScanSpreadMax = 100;
int ScanSpread();
void SetScanSpread(int percent);
// HUD opacity per visor (issue #144), in percent, or kHudOpacityVisor for the game's Visor
// Opacity. The X-Ray, Thermal and Scan HUDs default to Visor Opacity. The energy readout
// defaults to 100, as in the game, where Visor Opacity never reaches it; set to
// kHudOpacityVisor it follows the opacity of the HUD it is on. Stock under Original experience.
enum HudOpacityPart { kHudOpacityXRay, kHudOpacityThermal, kHudOpacityScan, kHudOpacityEnergy, kHudOpacityPartCount };
const int kHudOpacityVisor = -1;
const int kHudOpacityMin = 0;
const int kHudOpacityMax = 100;
int HudOpacity(HudOpacityPart part);
void SetHudOpacity(HudOpacityPart part, int percent);
// The centre crosshair (free aim, mouse aim, twin stick): opacity in percent,
// and an optional colour that replaces the game's (false = the game's own).
const int kCrosshairOpacityMin = 0;
const int kCrosshairOpacityMax = 100;
int CrosshairOpacity();
// The centre crosshair's look: the game's model, or a procedural take on Remastered's design
// (crosshair_style=, MP_CROSSHAIR_STYLE). Classic under Original experience.
enum ECrosshairStyle { kCrosshairStyle_Classic, kCrosshairStyle_Remastered };
ECrosshairStyle CrosshairStyle();
void SetCrosshairStyle(ECrosshairStyle style);
bool CrosshairColor(float rgb[3]);
// Auto-aim: without Lock-On the gun picks the target nearest the screen centre
// (CPlayer::UpdateAimTarget). Off = aim only at a locked target. On under Original experience.
bool AutoAim();
// Scan visor speed in percent (scan_speed=, MP_SCAN_SPEED); ScanSpeed() is the factor on the
// scan timer in CPlayer::UpdateScanningState. 1 under Original experience.
const int kScanSpeedMin = 100;
const int kScanSpeedMax = 500;
float ScanSpeed();
// Hint delay in minutes (hint_delay=, MP_HINT_DELAY); 0 = the game's own timing. HintDelaySeconds()
// caps the wait before a hint first shows (CHintOptions::Update); 0 when off or under Original
// experience. RequestShowHint() (F1 button, `hint show`) asks the next CHintOptions::Update to show
// the hint the game would show next; ConsumeShowHintRequest() takes the request.
const int kHintDelayMin = 0;
const int kHintDelayMax = 30;
float HintDelaySeconds();
void RequestShowHint();
bool ConsumeShowHintRequest();
// The next-target reticles (combat seeker, X-ray ring, thermal crosshair). Off hides them;
// lock-on and scan reticles stay. On under Original experience.
bool SecondaryCrosshairs();
// Menus and the map read a remapped pad's standard buttons (F1 Controls > Remap).
bool MenuStandardButtons();
void SetMenuStandardButtons(bool enabled);
void SetMapSwapFingers(bool swap); // the map_swap_fingers setting; read with PortPointer::MapSwapFingers()
void SetPointerMenus(bool enabled); // the pointer_menus setting; read with PortPointer::Enabled()
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
// Pickup marking on the map (PortMapPickups): Off; Seen draws a dot for each
// uncollected pickup the player has laid eyes on; Rooms tints the rooms that
// still hold an uncollected pickup; All draws a dot for every uncollected
// pickup. The saved value 1 (the old on/off setting) is All.
// The values are stored in the config: don't reorder or renumber.
enum EMapPickups { kMapPickups_Off, kMapPickups_All, kMapPickups_Seen, kMapPickups_Rooms };
EMapPickups MapPickups();
void SetMapPickups(EMapPickups mode);
// In an Archipelago game, colour the dots by the seed's logic like a tracker
// (green in logic, yellow sequence break, blue visible only, red out of reach,
// grey checked). On by default; off gives the plain white dots.
bool MapLogicColors();
void SetMapLogicColors(bool enabled);
// The dot's shape tells a major item from a minor one, and a collected pickup
// stays as a grey cross (PortMapPickups::Kinds). On by default.
bool MapPickupKinds();
void SetMapPickupKinds(bool enabled);
// In Seen, rooms a map station (or Reveal map) maps show all their pickups.
// On by default.
bool MapStationPickups();
void SetMapStationPickups(bool enabled);
// The port's own lines on the world map screen: the legend entries for the
// pickup shapes in use and the marked-room colour, and the "X Mark room" prompt
// (with a pointer click on it marking the shown room). Off by default, so the
// disc's own legend and prompts are what a first playthrough shows.
bool MapExtraUi();
void SetMapExtraUi(bool enabled);
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
// beam_doors_stay_unlocked (PortBeamDoors): a Wave, Ice, Plasma or Power Beam
// door whose force field was broken is a plain blue door from then on, on both
// sides, for the rest of the save. Off by default, and off in the Original
// experience. Rooms load patched, so turning it on or off applies to rooms
// loaded afterwards.
bool BeamDoorsStayUnlocked();
void SetBeamDoorsStayUnlocked(bool enabled);
// stack_beams (CPlayerGun::UpdateNormalShotCycle): every normal or charged arm-cannon
// shot also fires the projectiles of every other beam owned, from the same transform.
// Projectiles only (no animation, sound or shake of the extra beams); combos, missiles
// and the Phazon Beam are unchanged. Off by default, and off in the Original experience.
bool StackBeams();
void SetStackBeams(bool enabled);
// walk_speed (CPlayerDynamics WalkScale), in percent: Samus's on-foot top speed,
// acceleration and friction (and the orbit strafe speeds) are scaled by it, so she
// moves that much faster or slower and gets there in the same time. Jumps, dashes,
// turning, grapple and the morph ball are unchanged. 100 (retail) by default, and
// 100 in the Original experience.
const int kWalkSpeedMin = 50;
const int kWalkSpeedMax = 200;
const int kWalkSpeedDefault = 100;
int WalkSpeed();
void SetWalkSpeed(int percent);
// The stored 1.00 sequence-break compatibility choice (raw, not the effective
// policy). Randomizer and Archipelago games force it on at use.
bool Revision100Compatibility();
void SetRevision100Compatibility(bool enabled);
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
// The Nintendo, Retro and Dolby logos at launch (CSplashScreen). Skippable ends
// the current one with a short fade on A or Start; Off goes straight to the
// front end. Original experience shows them in full.
enum EBootLogos { kBootLogos_Show, kBootLogos_Skippable, kBootLogos_Off };
EBootLogos BootLogos();
void SetBootLogos(EBootLogos mode);
// First-person vertical field of view in degrees (retail 55). The arm cannon is
// drawn at the retail FOV whatever this is, like a view-model FOV.
const float kFovRetail = 55.f;
const float kFovMin = 45.f;
const float kFovMax = 120.f;
float FirstPersonFov();
void SetFirstPersonFov(float degrees);
// Arm cannon offset from its retail place, in metres along the camera's right,
// forward and up axes (issue #72, like PrimeHack's). It moves the real gun, so
// shots leave from the new muzzle. Zero in the original experience.
const float kGunOffsetMax = 1.f;
void GunOffset(float out[3]);
void SetGunOffset(const float offset[3]);
// Multisample anti-aliasing (1 = off, or 4) and the max texture anisotropy
// (1-16, retail-style mipmapped textures ask for the max). Applied next frame.
int Msaa();
void SetMsaa(int samples);
int Anisotropy();
void SetAnisotropy(int level);
// Setting `per_pixel_lighting` (default off, off in the original experience): GX
// lights summed per pixel instead of per vertex. Lit pipelines rebuild on next use.
bool PerPixelLighting();
void SetPerPixelLighting(bool enabled);
// Setting `sun_shadows` (default on): the 4-cascade sun shadow in outdoor rooms. Remastered
// draws none; off gives PortSetupShadow distance 0, as in a room with no sun.
bool SunShadows();
void SetSunShadows(bool enabled);
// Setting `projected_shadows` (default on): Remastered's projected actor shadows (CSimpleShadow decal
// mode); only active with Remastered world geometry, else the retail blob quad is drawn.
bool ProjectedShadows();
void SetProjectedShadows(bool enabled);
// Setting `opengles`: start on Dawn's OpenGL ES backend instead of Vulkan, for
// drivers that draw wrong on Vulkan (Adreno 7xx, issue #7). Read at window
// creation, so it takes a restart; aurora falls back to Vulkan if it fails.
bool OpenGles();
void SetOpenGles(bool enabled);
// Setting `graphics_backend` (Windows and desktop Linux): the graphics API to ask
// Aurora for at the next start. Auto asks for nothing in particular, so Aurora
// takes its own order (Direct3D 12, then Vulkan, then Direct3D 11 on Windows;
// Vulkan, then OpenGL ES on Linux); an explicit API is tried first (issue #135).
// Either way a failed start falls back to that order for the run and leaves the
// setting alone. Read at window creation, so it takes a restart. OpenGL is offered
// on desktop Linux only for now, through Dawn's desktop GL: Windows has no WGL
// backend yet. Other platforms ignore the setting: Android's OpenGL ES choice is
// `opengles`. These are the config file's values: don't reorder or renumber.
enum EGraphicsBackend {
  kGraphicsBackend_Auto = 0,
  kGraphicsBackend_D3D12,
  kGraphicsBackend_Vulkan,
  kGraphicsBackend_D3D11,
  kGraphicsBackend_OpenGL,
};
// The number of choices. The Video page lists a platform's subset of them
// (port_graphics_backend.h's Choices()), so this is not a combo's item count.
constexpr int kGraphicsBackendCount = 5;
EGraphicsBackend GraphicsBackend();
void SetGraphicsBackend(EGraphicsBackend backend);
// The config file's name for a choice; these, and only these, are what
// ParseGraphicsBackend and MP_GRAPHICS_BACKEND accept.
inline const char* GraphicsBackendConfigName(EGraphicsBackend backend) {
  switch (backend) {
  case kGraphicsBackend_D3D12:
    return "d3d12";
  case kGraphicsBackend_Vulkan:
    return "vulkan";
  case kGraphicsBackend_D3D11:
    return "d3d11";
  case kGraphicsBackend_OpenGL:
    return "opengl";
  default:
    return "auto";
  }
}
// Reads a stored name or an MP_GRAPHICS_BACKEND value. False for anything else,
// with `out` left as it was, so an unknown value keeps the saved selection.
inline bool ParseGraphicsBackend(const std::string& value, EGraphicsBackend& out) {
  if (value == "auto") {
    out = kGraphicsBackend_Auto;
  } else if (value == "d3d12") {
    out = kGraphicsBackend_D3D12;
  } else if (value == "vulkan") {
    out = kGraphicsBackend_Vulkan;
  } else if (value == "d3d11") {
    out = kGraphicsBackend_D3D11;
  } else if (value == "opengl") {
    out = kGraphicsBackend_OpenGL;
  } else {
    return false;
  }
  return true;
}
// Setting `gpu_driver`: the custom Vulkan driver (PortGpuDriver id) to load at the
// next start, "" = the system's. Android only.
const std::string& GpuDriver();
void SetGpuDriver(const std::string& id);
// Setting `storage_clamp` (Auto/Off/On): sets MP_STORAGE_CLAMP for aurora's shader
// generator unless it's already in the environment. Call before aurora_initialize.
void ApplyStorageClamp();
// Setting `driver_workarounds`: the Dawn toggles the ticked Video > Compatibility > Driver
// workarounds turn on and off, comma-separated, for AuroraConfig. Call once, before
// aurora_initialize. MP_DRIVER_WORKAROUNDS (names, or 0 for none) replaces the boxes for one
// run; a start that never called DriverWorkaroundsStarted turns them off at the next one; and
// `validation` (driver error checks) is unticked as it is applied, so it lasts one start.
void BeginDriverWorkarounds(const std::string& userFolder, std::string& enable, std::string& disable);
// The game got through its first second of frames: the workarounds didn't break the start.
void DriverWorkaroundsStarted();
// Extras normally earned by finishing the game (or, for the Fusion Suit, by a
// GBA link to Metroid Fusion). They only change what the title screen offers;
// nothing is written into the save's persistent flags.
bool UnlockHardMode();
void SetUnlockHardMode(bool enabled);
bool UnlockFusionSuit();
void SetUnlockFusionSuit(bool enabled);
// Puts the Fusion Suit on or off in game, as the Fusion Bonus menu's choice
// would: the suit, arm and ball load again as after a suit pickup.
void WearFusionSuit(CStateManager& mgr, bool wearing);
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
// The touch overlay's Beam button is held: the input engine's touch:beam_shift.
bool TouchBeamShift();
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
// Trilogy discs only: holding Beam menu / Visor menu opens the Wii's radial menus
// (CHudRadialSelect); the aim travel or C-stick picks. Off by default.
bool RadialMenus();
void SetRadialMenus(bool enabled);
// The beam shift's bindings (read by PortInputDevices, which pairs each with the D-pad
// into the beam chords): slots 0 and 1 are keys
// or mouse buttons (scancode or PAD_KEY_MOUSE_*), slot 2 a controller button
// (SDL gamepad button or PAD_NATIVE_BUTTON_TRIGGER_*); -1 for none.
int ShiftBinding(int slot);
void SetShiftBinding(int slot, int code);
// The turbo fire's bindings (read by PortInputDevices), laid out as ShiftBinding's
// (slot 2 reads -1 while touch is in use); all -1 (none) by default.
int TurboBinding(int slot);
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
// The Remastered import on or off (mods_disabled, saved as on the Mods page), then the mods
// reload; a note at the top of the screen says which.
void ToggleRemastered();
// A note at the top of the screen for a few seconds. Thread-safe.
void ShowNote(const std::string& text);
// Starts a Remastered import (port_remastered_import.h) with the mods unloaded
// while it runs; they load again, the new import included, when it ends.
// False when one is already running or there is no mods folder.
bool StartRemasteredImport(const std::string& image, const std::string& keys);
// The Dread Samus import (PortRemastered::StartDreadSamusImport), with the same mods unload/reload
// around it.
bool StartDreadSamusImport(const std::string& image, const std::string& keys);
// Memory card transfer to and from Dolphin (port_gci.h), for the overlay and
// the console. Each returns a message for the user. Imports are refused in
// game; `path` may be a .gci, a raw card image, a folder of .gci files or an
// Android content:// URI, and an export to a path ending in .raw writes a card
// image instead of .gci files. `format` picks what an export writes: "" (the
// card's files as they are), "usa" or "pal" (the save converted to that
// GameCube release; Dolphin's card of that region) or "trilogy" (the newest
// save written into the Wii Trilogy's save.bin at `dest`, or Dolphin's).
std::string CardList();
std::string CardImport(const std::string& path);
std::string CardExport(const std::string& dest, const std::string& format = "");
std::string CardImportDolphin();
std::string CardExportDolphin(const std::string& format = "");
// Cheat: the player takes no damage (F1 > Debug > Cheats, MP_GODMODE, console `god`).
bool Invulnerable();
void SetInvulnerable(bool enabled);
// Cheat: the Phazon Beam takes the Power Beam's place, anywhere (F1 > Debug > Cheats).
bool PhazonBeamCheat();
void SetPhazonBeamCheat(bool enabled);
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
void SetSpringBallFlick(bool enabled);
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
// Gyro aiming. Mode: 0 off, 1 aim while the Gyro aim button is held, 2 always
// aim, 3 aim except while it is held, 4 press to toggle aim (initially off).
// Source: 0 auto (a pad's gyro if it has
// one, else the phone's), 1 controller only, 2 the device's own gyro (Android
// phones). Sensitivity is camera angle per pad angle, apart from the mouse's.
void SetGyroMode(int mode);
void SetGyroSource(int source);
void SetGyroSensitivity(float scale);
// Vertical (pitch) sensitivity, used instead of the above for pitch while the split is on.
void SetGyroSensitivityY(float scale);
// Gyro space: 0 local (the pad's own yaw axis), 1 player, 2 world (the turn
// about gravity). Auto calibration learns the gyro's drift while the pad is
// still; the confidence is 0..1, or negative when the source has no
// accelerometer (no calibration or gravity then).
void SetGyroSpace(int space);
void SetGyroAutoCalibrate(int mode); // 0 off, 1 always, 2 only in menus
// Manual recalibration: a 5 s countdown (put the pad down), then a reset. Cancel
// drops a pending one. The state is for the F1 button's label.
void StartGyroRecalibration();
void CancelGyroRecalibration();
float GyroRecalibrationCountdown(); // seconds left, or negative when none pending
float GyroManualCalibrationProgress(); // 0..1 while Recalibrate samples, or negative
bool GyroCalibrating();             // auto calibration is learning right now
void ResetGyroCalibration();
float GyroCalibrationConfidence();
// Recentres the pointer-aim cursor and levels the camera pitch (the "Reset aim"
// action; thread-safe, applied on the next aim tick).
void RequestResetAim();
// The input engine's "Gyro aim" action, set each frame before PollGyro.
void SetGyroButton(bool held);
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
// The free-aim crosshair under direct aim: the setting, or always with pointer aim.
bool AimCrosshair();
// Pointer aim (Trilogy style, any direct aim): the mouse, right stick, gyro or a
// touch drag moves a pointer and the view turns once it passes the dead box
// (percent of the half view) at up to PointerAimTurn degrees/second. Shots and
// the gun follow the pointer.
bool PointerAim();
void SetPointerAim(bool on);
int PointerAimBox();
int PointerAimTurn();
// Lock-on free aim: under a combat lock any direct aim moves a pointer, with or
// without pointer aim (no edge turn: the lock owns the view). CPlayer::PortLockFreeAim
// reads it.
bool LockOnFreeAim();
// The pointer (-1..1 of the half view, right/down positive) as of the last tick;
// false (and centre) unless it steered that tick's free look.
bool PointerAimPosition(float& x, float& y);
// The same, plus the travel since the tick, for drawing; fraction is the part of
// the tick shown (CCameraManager::GetPresentationInterpolation).
bool PresentedPointerAimPosition(float fraction, float& x, float& y);
// Places the pointer (console `pointeraim <x> <y>`); the next tick's look may re-centre it.
void SetPointerAimPosition(float x, float y);
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
// pointerHold: under pointer aim, a lock that keeps the pointer where it is instead of
// re-centring it (the Scan Visor's point orbit, CPlayer::UpdateMouseAim).
bool UpdateMouseAim(bool active, bool locked, float x, float y, float z, bool pointerLock = false,
                    bool pointerHold = false);
void SynchronizeMouseAim(float x, float y, float z);
// Turns the mouse-look yaw by dyaw radians (the game turned the player: a rotating
// platform's rider carry). No effect until the aim is initialized.
void TurnMouseAim(float dyaw);
void ResetMouseAim();
float MouseSensitivity();
void SetMouseSensitivity(float radiansPerPixel);
// Vertical mouse sensitivity, used for up and down while the split is on.
void SetMouseSensitivityY(float radiansPerPixel);
// Called from the input event loop as relative motion arrives.
void AddMouseDelta(float dx, float dy);
// Called once per simulated frame to latch the deltas for that frame.
void BeginFrameMouse();
// Touch aim (Android drag-to-turn): finger travel in dp, right/down positive.
// Thread-safe; drained by BeginFrameMouse.
bool TouchAim();
void SetTouchAim(bool on);
void SetTouchAimSpeed(float pixelsPerDp);
void SetTouchAimSpeedY(float pixelsPerDp);
void AddTouchAim(float dxDp, float dyDp);
// A quick swipe up on the touch aim area (the overlay detects it): requests the
// Spring Ball when "Spring Ball on swipe up" is on. Thread-safe.
void SetSpringBallSwipe(bool enabled);
void TouchSpringSwipe();
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
// CInGameGuiManager publishes each frame whether the pause menu is up (stale = closed),
// and whether a paused dialog (save station, upgrade message) is.
void SetPauseScreenOpen(bool open, bool dialog);
bool PauseScreenOpen();
// The pause menu or a paused dialog: input reads the Menu rows there (issue #93).
bool PausedMenuOpen();
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
void SetMusyxAudioEnabled(bool enabled);

// Session
void RequestReset();
bool ConsumeResetRequest();

bool Visible();
// Writes the settings file now instead of waiting for the next overlay frame,
// so a disc chosen during startup is remembered even if no frame is drawn yet.
// It always writes, changed or not.
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
// The overlay's opacity in percent (10-100). Same rules.
float TouchOpacity();
// False when F1 turned the on-screen controls off: only MENU and START stay. Same rules.
bool TouchControlsEnabled();
// Per-control offset and size, an opaque `<id>:<dx>,<dy>,<scale>;...` string the
// Android touch view owns. Setting it saves the config on the next frame.
std::string TouchLayout();
void SetTouchLayout(const std::string& layout);
// True once after F1's "Edit layout" was pressed.
bool TakeTouchEditRequested();
// Asks for the layout editor as "Edit layout" does (the console's `touchedit`).
void RequestTouchEdit();
// True while the in-game touch layout editor is open (never without PORT_TOUCH_OVERLAY).
bool TouchEditing();
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
