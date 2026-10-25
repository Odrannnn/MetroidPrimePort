// F1 overlay: Frame interpolation switches, audio backends, reset, debug views, teleports and the
// world sweep, overlay visibility, game options and the settings log. State and helpers shared with
// the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_freecam.h"
#include "port_hd_font.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_input_devices.h"
#include "port_discord.h"
#include "port_tracker.h"
#include "port_mouse.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CMemoryStreamOut.hpp"

#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

// Implemented by the AI and MusyX audio backends.
extern "C" void AIPortSetOutputEnabled(int enabled);
extern "C" int AIPortOutputEnabled(void);
extern "C" void salSetMuted(int muted);

namespace PortDebug {

// State only this part uses.
namespace {
bool sGameOptionsRestored = false;
unsigned sPresentCycleFrame = 0;
bool sTickHold = false;
unsigned sHeldTicks = 0;
bool sResetRequested = false;
int sPendingTeleport = -1;
bool sHasWorldTeleport = false;
uint32_t sWorldTeleportWorld = 0;
uint32_t sWorldTeleportArea = 0;
bool sWorldSweepRequested = false;
struct WorldSweep {
  std::vector< uint32_t > worlds;
  std::vector< uint32_t > areas;
  size_t world = 0;
  size_t area = 0;
  unsigned settledTicks = 0;
  unsigned stalledTicks = 0;
  unsigned completedAreas = 0;
  bool active = false;
  bool waiting = false;
  // An area can hold several layers, and only the active ones are built, so a
  // pickup behind a layer the save has not unlocked is not in the dump at all.
  // The tour revisits each area once per layer with a different one active,
  // which is what makes the dump cover the whole area rather than the state the
  // save happens to be in.
  int layer = 0;
  int layerCount = 1;
  unsigned layerPasses = 0;
  // Set between the hop away from an area and the hop back to it: the area has
  // to be gone before it is rebuilt with the next layer.
  bool revisiting = false;
} sWorldSweep;
} // namespace

bool FrameInterpolation() {
  EnsureInitialized();
  return sFrameInterpolation && !sOriginalExperience;
}

bool SmoothFrames() {
  EnsureInitialized();
  return sSmoothFrames && !sOriginalExperience;
}

void SetSmoothFrames(bool enabled) {
  EnsureInitialized();
  sSmoothFrames = enabled;
  sFrameInterpolation = sActorInterpolation = sPoseInterpolation = sParticleInterpolation = enabled;
  MarkDirty();
}

bool ActorInterpolation() {
  EnsureInitialized();
  return sActorInterpolation && !sOriginalExperience;
}

void SetActorInterpolation(bool enabled) {
  EnsureInitialized();
  sActorInterpolation = enabled;
}

bool PoseInterpolation() {
  EnsureInitialized();
  return sPoseInterpolation && !sOriginalExperience;
}

void SetPoseInterpolation(bool enabled) {
  EnsureInitialized();
  sPoseInterpolation = enabled;
}

void SetRoomGeoResident(bool enabled) { StoreSetting(sRoomGeoResident, enabled); }

bool RoomGeoResidentAtStartup() {
  std::ifstream file(SettingsFilePath());
  std::string line;
  bool on = kRoomGeoResidentDefault;
  while (std::getline(file, line)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    std::string value = line.substr(separator + 1);
    value.erase(std::min(value.find('#'), value.size()));
    if (key == "room_geo_gpu") {
      on = ParseBool(Trim(value));
    } else if (key == "room_geo_resident") {
      on = on || ParseBool(Trim(value));
    }
  }
  return on;
}

bool ParticleInterpolation() {
  EnsureInitialized();
  return sParticleInterpolation && !sOriginalExperience;
}

void SetParticleInterpolation(bool enabled) {
  EnsureInitialized();
  sParticleInterpolation = enabled;
}

bool PresentOverride(float& t) {
  EnsureInitialized();
  if (sPresentOverride < 0.f) {
    return false;
  }
  if (sPresentOverride == kPresentTick) {
    t = -1.f;
  } else if (sPresentOverride > 1.f) {
    t = 0.25f * static_cast< float >(sPresentCycleFrame++ & 3);
  } else {
    t = sPresentOverride;
  }
  return true;
}

void SetPresentOverride(float value) {
  EnsureInitialized();
  sPresentOverride = value < 0.f             ? -1.f
                     : value == kPresentTick ? kPresentTick
                     : value > 1.f           ? kPresentCycle
                                             : value;
  sPresentCycleFrame = 0;
}

float PresentOverrideValue() { return Loaded(sPresentOverride); }

bool TickHold() { return sTickHold; }

void SetTickHold(bool held) {
  sTickHold = held;
  sHeldTicks = 0;
}

void StepTicks(unsigned count) { sHeldTicks += count; }

unsigned PendingHeldTicks() { return sHeldTicks; }

unsigned TakeHeldTicks() {
  // One tick per loop, like MP_TURBO=1, so each step is one drawn frame.
  if (sHeldTicks == 0) {
    return 0;
  }
  --sHeldTicks;
  return 1;
}

void GetFrameMouseDelta(float& dx, float& dy) {
  dx = sMouseFrameX;
  dy = sMouseFrameY;
}

float AimYaw() { return sMouseAimState.yaw; }
float AimPitch() { return sMouseAimState.pitch; }
bool AimInitialized() { return sMouseAimState.initialized; }

bool AiAudioEnabled() {
  EnsureInitialized();
  return AIPortOutputEnabled() != 0;
}

void SetAiAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sAiAudioEnabled == enabled && AiAudioEnabled() == enabled) {
    return;
  }
  sAiAudioEnabled = enabled;
  AIPortSetOutputEnabled(enabled ? 1 : 0);
}

void SetMusyxAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sMusyxAudioEnabled == enabled) {
    return;
  }
  sMusyxAudioEnabled = enabled;
  salSetMuted(enabled ? 0 : 1);
}

void RequestReset() { sResetRequested = true; }

bool ConsumeResetRequest() {
  const bool requested = sResetRequested;
  sResetRequested = false;
  return requested;
}

void SetStateManager(CStateManager* mgr) {
  if (mgr != sStateManager) {
    // The tracker's room names come from the old world's PAKs.
    PortTracker::Reset();
  }
  sStateManager = mgr;
}
bool QuickResume() { return sQuickResume; }
bool SaveStateRoomState() { return sSaveStateRoomState; }
CStateManager* StateManager() { return sStateManager; }
bool ViewRay(float origin[3], float forward[3]) {
  if (sStateManager == nullptr || sStateManager->GetCameraManager() == nullptr) {
    return false;
  }
  const CTransform4f view =
      PortFreeCam::View(sStateManager->GetCameraManager()->GetCurrentCameraTransform(*sStateManager));
  const CVector3f at = view.GetTranslation();
  const CVector3f to = view.GetForward();
  origin[0] = at.GetX();
  origin[1] = at.GetY();
  origin[2] = at.GetZ();
  forward[0] = to.GetX();
  forward[1] = to.GetY();
  forward[2] = to.GetZ();
  return true;
}

namespace {
const char* const kPbrViews[] = {"off",     "albedo",     "normal", "rough",    "metal", "ao",
                                 "ambient", "reflection", "glow",   "exposure", "kind",
                                 "sun",     "drawid"};
// The last entry is not a shader view but Aurora's draw id mode (GXPortSetDrawIdMode).
constexpr int kDrawIdView = 12;
int sPbrView = 0;
} // namespace
void NoteDrawIdMode(bool on) { CCubeModel::PortSetDrawIds(on); }
int PbrViewCount() { return int(sizeof(kPbrViews) / sizeof(kPbrViews[0])); }
const char* PbrViewName(int view) { return view >= 0 && view < PbrViewCount() ? kPbrViews[view] : "?"; }
int PbrView() { return sPbrView; }
void SetPbrView(int view) {
  sPbrView = view >= 0 && view < PbrViewCount() ? view : 0;
  GXSetPBRDebugView(sPbrView == kDrawIdView ? 0u : u32(sPbrView));
  GXPortSetDrawIdMode(sPbrView == kDrawIdView ? GX_TRUE : GX_FALSE);
  PortDebug::NoteDrawIdMode(sPbrView == kDrawIdView);
}
void RequestTeleport(int areaId) { sPendingTeleport = areaId; }
bool ConsumeTeleportRequest(int& areaId) {
  if (sPendingTeleport < 0) {
    return false;
  }
  areaId = sPendingTeleport;
  sPendingTeleport = -1;
  return true;
}
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId) {
  sWorldTeleportWorld = worldId;
  sWorldTeleportArea = areaAssetId;
  sHasWorldTeleport = true;
}
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId) {
  if (!sHasWorldTeleport) {
    return false;
  }
  worldId = sWorldTeleportWorld;
  areaAssetId = sWorldTeleportArea;
  sHasWorldTeleport = false;
  return true;
}

void RequestWorldSweep() {
  if (sWorldSweepRequested || sWorldSweep.active || sHasWorldTeleport || sPendingTeleport >= 0)
    return;
  sWorldSweepRequested = true;
}

// Activates one layer of the current area and deactivates the rest, so the
// area's objects are built for that layer when it is next reconstructed.
// Layer 0 is the one a fresh save has, which is why a plain tour never sees
// anything behind another layer.
void SetSweepLayer(CStateManager& mgr, CWorld* world, TAreaId area, int layer) {
  rstl::rc_ptr< CScriptLayerManager >& layers = mgr.WorldLayerState();
  if (layers.IsNull())
    return;
  const int count = layers->GetAreaLayerCount(area);
  for (int i = 0; i < count; ++i)
    layers->SetLayerActive(area, TLayerId(i), i == layer);
  (void)world;
}

bool SweepLayersEnabled() {
  return port::EnvFlag("MP_RANDO_SWEEP_LAYERS");
}

bool ConsumeWorldSweepRequest(CStateManager& mgr) {
  // This entry point is called only by gameplay, never the frontend or UI.
  static const bool envChecked = [] {
    if (port::EnvFlag("MP_RANDO_SWEEP"))
      RequestWorldSweep();
    return true;
  }();
  (void)envChecked;
  WorldSweep& sweep = sWorldSweep;
  if ((!sWorldSweepRequested && !sweep.active) || mgr.GetWantsToQuit()) {
    return false;
  }
  if (sHasWorldTeleport || sPendingTeleport >= 0 || sResetRequested) {
    // A manual debug request wins; do not resume the tour behind the user's back.
    if (sweep.active) PortLog::Write("[sweep] cancelled: another debug teleport/reset\n");
    sweep = {};
    sWorldSweepRequested = false;
    return false;
  }
  CWorld* world = mgr.World();
  if (mgr.GetGameState() != CStateManager::kGS_Running || world == nullptr ||
      gpGameState == nullptr || gpMemoryCard == nullptr) return false;

  if (sWorldSweepRequested) {
    const auto& worlds = gpMemoryCard->GetMemoryWorlds();
    if (worlds.empty()) return false;
    sweep = {};
    // MP_RANDO_SWEEP_WORLDS=<hex id>[,<hex id>...] limits the tour to those
    // worlds, e.g. to redo one world without walking the seven before it.
    std::vector< uint32_t > only;
    if (const char* list = port::EnvString("MP_RANDO_SWEEP_WORLDS")) {
      for (const char* p = list; *p != '\0';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 16);
        if (end == p) {
          ++p;
          continue;
        }
        only.push_back(static_cast< uint32_t >(id));
        p = end;
      }
    }
    const auto wanted = [&only](uint32_t id) {
      if (only.empty()) return true;
      for (uint32_t w : only)
        if (w == id) return true;
      return false;
    };
    // Visit the live world first so its MLVL can supply the first area list;
    // later worlds are entered at area zero, which also visits their first area.
    if (wanted(world->IGetWorldAssetId())) sweep.worlds.push_back(world->IGetWorldAssetId());
    for (const auto& entry : worlds) {
      if (entry.first != world->IGetWorldAssetId() && wanted(entry.first))
        sweep.worlds.push_back(entry.first);
    }
    sWorldSweepRequested = false;
    if (sweep.worlds.empty()) {
      PortLog::Write("[sweep] stopped: MP_RANDO_SWEEP_WORLDS matches no world\n");
      sweep = {};
      return false;
    }
    sweep.active = true;
    PortLog::Write("[sweep] begin: %zu worlds (use MP_RANDO_DUMP=1 for pickups)\n",
                   sweep.worlds.size());
    if (world->IGetWorldAssetId() != sweep.worlds[0]) {
      // The live world was filtered out: enter the first wanted world the same
      // way a finished world hands over to the next one.
      sweep.waiting = true;
      RequestWorldTeleport(sweep.worlds[0], 0u);
      return true;
    }
  }
  if (world->IGetWorldAssetId() != sweep.worlds[sweep.world]) {
    PortLog::Write("[sweep] cancelled: gameplay changed worlds\n");
    sweep = {};
    return false;
  }
  if (sweep.areas.empty()) {
    for (int i = 0; i < world->IGetAreaCount(); ++i)
      sweep.areas.push_back(world->IGetAreaAlways(TAreaId(i))->IGetAreaAssetId());
    if (sweep.areas.empty()) {
      PortLog::Write("[sweep] stopped: world has no areas\n");
      sweep = {};
      return false;
    }
    PortLog::Write("[sweep] world %zu/%zu: %08X, %zu areas\n", sweep.world + 1,
                   sweep.worlds.size(), sweep.worlds[sweep.world], sweep.areas.size());
    if (SweepLayersEnabled()) {
      // How many layers the widest area has, so every area gets a pass per
      // layer even where its own count is lower. An area with fewer layers
      // simply rebuilds the same thing, which costs a restart and nothing else.
      int widest = 1;
      for (int i = 0; i < world->IGetAreaCount(); ++i) {
        const int count = mgr.WorldLayerState()
                              ? mgr.WorldLayerState()->GetAreaLayerCount(TAreaId(i))
                              : 1;
        if (count > widest)
          widest = count;
      }
      sweep.layerCount = widest;
      PortLog::Write("[sweep] layers: up to %d per area\n", widest);
    }
  }

  // Count consecutive quiet simulation ticks, not rendered frames or a wall
  // clock timeout. Current-area construction alone does not imply that adjacent
  // areas, map tiles and factory requests have finished streaming.
  const TAreaId current = world->GetCurrentAreaId();
  const bool areaExists = world->DoesAreaExist(current);
  const bool areaValid = areaExists && world->GetArea(current)->IsValidated();
  const bool loadingIdle = world->GetChainHead(CWorld::kC_Loading) == CWorld::GetAliveAreasEnd();
  const bool freeingIdle = world->GetChainHead(CWorld::kC_ToDeallocate) == CWorld::GetAliveAreasEnd();
  const bool mapIdle = !world->GetMapWorld()->IsMapAreasStreaming();
  const bool loadsIdle = !gpResourceFactory->HasPendingLoads();
  const bool ready = areaExists && areaValid && loadingIdle && freeingIdle && mapIdle && loadsIdle;
  if (!ready) {
    // A tour that never settles is otherwise invisible: no output, no
    // progress, just frames. Say what is still outstanding, and how long it has
    // been that way, so the stall names itself instead of looking like work.
    if (sweep.settledTicks == 0) {
      PortLog::Write("[sweep] waiting on area %d: %s%s%s%s%s (target %08X, pass %zu/%zu)\n",
                     static_cast<int>(current.Value()), areaExists ? "" : "no-area ",
                     areaValid ? "" : "unvalidated ", loadingIdle ? "" : "loading ",
                     freeingIdle ? "" : "freeing ", mapIdle ? "" : "map-streaming ",
                     sweep.areas.empty() ? 0u : sweep.areas[sweep.area], sweep.area + 1,
                     sweep.areas.size());
    }
    // 10 seconds of simulation. A pass that has not settled by then is not
    // going to: the loading chain stays put while the area is alive and
    // nothing else moves it. The window is short because the wait is measured
    // in ticks and a throttled run reaches a tick slowly.
    if (++sweep.stalledTicks == 600) {
      // One area that will not settle must not end a whole tour: the rest of
      // the world is still worth dumping, and this area's objects were built on
      // the way in even if the settle check never agreed. Report it, skip the
      // pass, and carry on from the next one.
      PortLog::Write("[sweep] giving up on %08X after 10s: %s%s%s%s%s\n",
                     sweep.areas.empty() ? 0u : sweep.areas[sweep.area],
                     areaExists ? "" : "no-area ", areaValid ? "" : "unvalidated ",
                     loadingIdle ? "" : "loading ", freeingIdle ? "" : "freeing ",
                     mapIdle ? "" : "map-streaming ");
      sweep.stalledTicks = 0;
      sweep.settledTicks = 0;
      sweep.revisiting = false;
      if (sweep.waiting) {
        ++sweep.completedAreas;
        ++sweep.area;
        sweep.layer = 0;
        if (sweep.area >= sweep.areas.size()) {
          sweep.areas.clear();
          sweep.area = 0;
          ++sweep.world;
        }
        sweep.waiting = false;
      }
      if (sweep.area >= sweep.areas.size() || sweep.world >= sweep.worlds.size()) {
        PortLog::Write("[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                       sweep.completedAreas);
        sweep = {};
        return false;
      }
      return true;
    }
    sweep.settledTicks = 0;
    return false;
  }
  sweep.stalledTicks = 0;
  if (++sweep.settledTicks < 30) return false;
  sweep.settledTicks = 0;
  if (sweep.waiting) {
    const uint32_t settled = world->IGetAreaAlways(current)->IGetAreaAssetId();
    if (sweep.revisiting) {
      // This is the hop away from the area being re-layered: its objects have
      // been dumped, so all that matters is that it is gone by the time we go
      // back. Wait here, then return to it with the new layer active.
      sweep.revisiting = false;
      PortLog::Write("[sweep] left %08X for layer %d; returning\n", settled,
                     sweep.layer + 1);
      RequestWorldTeleport(sweep.worlds[sweep.world], sweep.areas[sweep.area]);
      return true;
    }
    if (settled != sweep.areas[sweep.area]) {
      // The destination's own scripts moved the player on (Impact Crater's
      // spawn points do). Its objects, and so its dump, were already built.
      PortLog::Write("[sweep] note: %08X moved the player to %08X; continuing\n",
                     sweep.areas[sweep.area], settled);
    }
    // One area, one layer: the count reports passes, not distinct areas, so a
    // multi-layered area is visibly a few.
    ++sweep.completedAreas;
    PortLog::Write("[sweep] area %zu/%zu: %08X layer %d/%d (total %u)\n", sweep.area + 1,
                   sweep.areas.size(), sweep.areas[sweep.area], sweep.layer + 1, sweep.layerCount,
                   sweep.completedAreas);
    if (++sweep.layer < sweep.layerCount) {
      // Another layer of the area just built. The layer state has to be set
      // before the area is reconstructed, because CGameArea builds the objects
      // of the layers that are active at construction time - which is why a
      // pickup behind an inactive layer is not in the dump at all.
      ++sweep.layerPasses;
      SetSweepLayer(mgr, world, current, sweep.layer);
      PortLog::Write("[sweep] re-entering %08X with layer %d active\n",
                     sweep.areas[sweep.area], sweep.layer);
      // The area has to be unloaded before it is rebuilt: SetLayerActive is a
      // bit flip, and travelling straight back onto a live area re-enters it
      // without ever scheduling its load again, so the pass never settles. Two
      // hops: out to the world's first area, which unloads this one, and the
      // return below lands on it with the new layer active.
      sweep.revisiting = true;
      RequestWorldTeleport(sweep.worlds[sweep.world], 0u);
      return true;
    }
    sweep.layer = 0;
    if (++sweep.area == sweep.areas.size()) {
      if (++sweep.world == sweep.worlds.size()) {
        PortLog::Write("[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                       sweep.completedAreas);
        sweep = {};
        return false;
      }
      sweep.areas.clear();
      sweep.area = 0;
    }
  }
  // QuitGame is consumed in this same tick. The next call with !GetWantsToQuit
  // belongs to the new manager even if the allocator reused its old address.
  sweep.waiting = true;
  RequestWorldTeleport(sweep.worlds[sweep.world],
                       sweep.areas.empty() ? 0u : sweep.areas[sweep.area]);
  return true;
}

bool Visible() { return Loaded(sVisible); }

bool OverlayVisible() { return sOverlayVisible.load(std::memory_order_acquire); }

bool TouchColorsFlag() { return sTouchColorsFlag.load(std::memory_order_acquire); }
bool TouchLabelsFlag() { return sTouchLabelsFlag.load(std::memory_order_acquire); }
bool TouchTurboFlag() { return sTouchTurboFlag.load(std::memory_order_acquire); }
bool TouchFloatingStickFlag() { return sTouchFloatingStickFlag.load(std::memory_order_acquire); }
float TouchSideMarginDp() { return sInput.touchSideMargin.load(); }
float TouchStickInsetDp() { return sInput.touchStickInset.load(); }
float TouchButtonInsetDp() { return sInput.touchButtonInset.load(); }
float TouchOpacity() { return sInput.touchOpacity.load(); }
bool TouchControlsEnabled() { return sInput.touchControls.load(); }

std::string TouchLayout() {
  return PortInput::TouchLayout();
}

void SetTouchLayout(const std::string& layout) {
  if (!PortInput::SetTouchLayout(layout)) {
    return;
  }
  sTouchLayoutSavePending.store(true, std::memory_order_release);
}

bool TakeTouchEditRequested() { return sTouchEditRequested.exchange(false, std::memory_order_acq_rel); }

void RequestTouchEdit() { sTouchEditRequested.store(true, std::memory_order_release); }

void SaveSettingsNow() {
  EnsureInitialized();
  // Callers change settings the overlay doesn't track (the console's gci share).
  MarkDirty();
  SaveSettings();
}

namespace {
// CGameOptions::PutTo: x0_'s 64 bytes, then 58 bits of options.
constexpr size_t kGameOptionsBytes = 72;

std::string GameOptionsHex(CGameOptions& options) {
  uchar bytes[128] = {};
  {
    CMemoryStreamOut out(bytes, sizeof(bytes));
    options.PutTo(out);
    out.Flush();
  }
  static const char kDigits[] = "0123456789abcdef";
  std::string hex;
  for (size_t i = 0; i < kGameOptionsBytes; ++i) {
    hex += kDigits[bytes[i] >> 4];
    hex += kDigits[bytes[i] & 0xF];
  }
  return hex;
}
} // namespace

// Records the game's options whenever they change, wherever from (pause menu,
// front-end options, F1, a loaded save state).
void SyncGameOptions() {
  if (!sGameOptionsRestored || gpGameState == nullptr) {
    return;
  }
  std::string hex = GameOptionsHex(gpGameState->GameOptions());
  if (hex != sGameOptions) {
    sGameOptions = std::move(hex);
    MarkDirty();
    // A dragged F1 slider is saved when it's let go.
    if (!sVisible || !ImGui::IsAnyItemActive()) {
      SaveSettings();
    }
  }
}

void RestoreGameOptions() {
  EnsureInitialized();
  sGameOptionsRestored = true;
  if (gpGameState == nullptr || sGameOptions.size() != kGameOptionsBytes * 2) {
    return;
  }
  uchar bytes[128] = {};
  for (size_t i = 0; i < kGameOptionsBytes; ++i) {
    const unsigned long value = std::strtoul(sGameOptions.substr(i * 2, 2).c_str(), nullptr, 16);
    bytes[i] = static_cast< uchar >(value);
  }
  CMemoryInStream in(bytes, sizeof(bytes));
  gpGameState->GameOptions() = CGameOptions(in);
  gpGameState->GameOptions().EnsureOptions();
}

void LogSettings() {
  EnsureInitialized();
  LogSettingChanges(SettingsText());
  sSettingsLogged = true;
  // Wrapped into lines of a readable length; keys in alphabetical order.
  std::string line;
  const auto flush = [&line] {
    if (!line.empty()) {
      PortLog::Write("port: settings:%s\n", line.c_str());
      line.clear();
    }
  };
  for (const auto& [key, value] : sLoggedSettings) {
    const std::string entry = " " + key + "=" + LoggedSettingValue(key, value);
    if (line.size() + entry.size() > 160) {
      flush();
    }
    line += entry;
  }
  flush();
  // MP_* variables override the file for one run (already in the values above).
#if defined(_WIN32)
  char** env = _environ;
#else
  char** env = environ;
#endif
  for (; env != nullptr && *env != nullptr; ++env) {
    if (std::strncmp(*env, "MP_", 3) == 0) {
      // ApplyStorageClamp sets MP_STORAGE_CLAMP itself from `storage_clamp`: say so, or it reads as user-set.
      const bool fromSetting = sStorageClampEnvFromSetting && std::strncmp(*env, "MP_STORAGE_CLAMP=", 17) == 0;
      PortLog::Write("port: env: %s%s\n", *env, fromSetting ? " (from storage_clamp)" : "");
    }
  }
  if (sOriginalExperience) {
    PortLog::Write("port: original experience on: the getters return retail values, not the ones above\n");
  }
}

// Set while the disc is mounted, before the first frame.
bool sDiscReadFailedLastSession = false;

void NoteDiscReadFailedLastSession() { sDiscReadFailedLastSession = true; }

} // namespace PortDebug
