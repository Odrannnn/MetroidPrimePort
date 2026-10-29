// Exercise the production Initialize scan with a real mod directory. Only the
// renderer/disc/settings and generated resources are stubbed; no game data needed.
#include "port_mods.h"
#include "port_ball_looks.h"
#include "port_suit_colours.h"
#include "port_debug.h"
#include "port_disc.h"
#include "port_remastered_import.h"
#include "port_room_geo.h"
#include "port_room_env.h"
#include "port_room_liquid.h"
#include "port_room_effects.h"
#include "port_hd_font.h"
#include "port_hud_bars.h"
#include <aurora/dvd.h>
#include <aurora/texture.hpp>
#include <dolphin/dvd.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
bool ready = false;
bool modsOn = true;
std::string disabled;
std::string hidden;
int overlays = 0;
int discRegistrations = 0;
int frameBufferReserves = 0;
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
}
namespace PortDebug {
bool ModsEnabled() { return modsOn; }
std::string ModsDisabled() { return disabled; }
std::string RemasteredHidden() { return hidden; }
bool GeneratedBallLooks() { return false; }
bool OriginalExperience() { return true; }
}
namespace PortRemastered { std::string InstalledModFolder() { return {}; } }
namespace PortDisc {
Version Current() { return Version::Usa100; }
const char* Name(Version) { return "USA 1.00"; }
}
namespace PortRoomGeo { bool BuffersReady() { return ready; } void Reset() {} }
namespace PortRoomEnv { bool ParseFileName(const std::string&, uint32_t&) { return false; } void Reset() {} }
namespace PortRoomLiquid { bool ParseFileName(const std::string&, uint32_t&) { return false; } void Reset() {} }
namespace PortRoomEffects { bool ParseFileName(const std::string&, uint32_t&) { return false; } }
namespace PortHdFont { bool ParseFileName(const std::string&) { return false; } void Reset() {} }
namespace PortHudBars { bool ParseFileName(const std::string&, uint32_t&) { return false; } void Reset() {} }
namespace PortBallLooks {
const char* Name(Look) { return "unused"; }
std::vector<std::pair<uint32_t, uint32_t>> ModelKeys(Look) { return {}; }
std::vector<PortMods::LooseResource> Collect(Look) { return {}; }
void SetState(bool, unsigned) {}
}
namespace PortSuitColours {
Picks SuitPicks(int) { return {}; }
std::string PicksKey(int, const Picks&) { return {}; }
void SetServed(const std::array<std::string, kSuitCount>&) {}
std::vector<PortMods::LooseResource> Collect(int, const Picks&,
    const std::map<std::pair<uint32_t, uint32_t>, PortMods::LooseResource>&, bool (*)(uint32_t)) { return {}; }
}
namespace aurora::texture {
ReplacementRegistration register_file_replacement(ReplacementKey, const std::filesystem::path&, ReplacementOptions) { return {}; }
void unregister_replacement(const ReplacementRegistration&) {}
}
extern "C" {
void aurora_reserve_frame_buffers() { ++frameBufferReserves; }
void* aurora_dvd_base_open(s32) { return nullptr; }
void aurora_dvd_base_close(void*) {}
int64_t aurora_dvd_base_seek(void*, int64_t, int32_t) { return -1; }
int64_t aurora_dvd_base_read(void*, uint8_t*, size_t) { return -1; }
s32 aurora_dvd_base_entry_count() { return 1; }
void aurora_dvd_overlay_callbacks(const AuroraOverlayCallbacks*) {}
void aurora_dvd_overlay_files(const AuroraOverlayFile*, size_t count, s32* entries) {
  overlays = int(count);
  discRegistrations += int(count);
  for (size_t i = 0; i < count; ++i) entries[i] = 0;
}
s32 DVDConvertPathToEntrynum(const char* path) { return std::string(path) == "/Metroid1.pak" ? 0 : -1; }
BOOL DVDConvertEntrynumToPath(s32, char* path, u32) { std::snprintf(path, 512, "/Metroid1.pak"); return true; }
BOOL DVDFastOpenDir(s32, DVDDir*) { return false; }
BOOL DVDOpenDir(const char*, DVDDir*) { return false; }
int DVDReadDir(DVDDir*, DVDDirEntry*) { return false; }
int DVDCloseDir(DVDDir*) { return true; }
}

int main() {
  namespace fs = std::filesystem;
  // A test-specific fixture within the build tree; no external game files.
  const fs::path root = fs::current_path() / "restart-fixture";
  const fs::path mod = root / "remastered-models";
  fs::create_directories(mod / "nested");
  // PortMods reads MP_MODS with std::getenv, which on Windows sees only the CRT's
  // copy of the environment, not one SDL_setenv_unsafe sets.
#ifdef _WIN32
  _putenv_s("MP_MODS", root.string().c_str());
#else
  setenv("MP_MODS", root.string().c_str(), 1);
#endif
  const auto put = [&](const char* name) { std::ofstream(mod / name) << "fixture"; };
  put("nested/12345678.RoOmGeO"); // actual production parser: any case, any subdirectory
  put("00000001.dds"); // sorts before geometry: must not leak out of deferred mod
  put("00000002.CMDL");
  put("Metroid1.pak");
  put("envfx.bin");
  put("thermal.lut");
  put("shell.bin");
  put("effectids.map");
  const auto scan = [&] { PortMods::Initialize(); Check(disabled.empty(), "scan changed saved enabled intent"); };
  scan();
  const auto& deferred = PortMods::CurrentStatus();
  Check(deferred.mods.size() == 1, "fixture not discovered");
  Check(!deferred.mods[0].enabled && deferred.mods[0].pendingRestart, "geometry not deferred");
  Check(deferred.mods[0].files == 0 && deferred.mods[0].resources == 0 && deferred.mods[0].textures == 0,
        "deferred mod reported active resources");
  Check(!PortMods::RoomGeometryLoaded() && PortMods::NativeTextureCount() == 0 &&
        PortMods::ResourceProvider(0x434d444c, 2).empty() && PortMods::EnvFxPath().empty() &&
        PortMods::ThermalLutPath().empty() && PortMods::ShellPath().empty() &&
        overlays == 0 && discRegistrations == 0, "deferred mod registered resources");
  Check(std::any_of(deferred.messages.begin(), deferred.messages.end(), [](const std::string& message) {
    return message.find("Restart the application to apply") != std::string::npos;
  }), "missing restart diagnostic");
  scan(); // a world/mod reload cannot turn this into a partially active mod
  Check(PortMods::CurrentStatus().mods[0].pendingRestart && discRegistrations == 0, "reload bypassed deferral");
  Check(frameBufferReserves == 0, "deferred mod asked for the frame buffers");
  hidden = "hud, geometry"; // left out, the geometry needs no restart; the rest loads
  scan();
  Check(PortMods::CurrentStatus().mods[0].enabled && !PortMods::CurrentStatus().mods[0].pendingRestart &&
        !PortMods::RoomGeometryLoaded() && frameBufferReserves == 0 &&
        !PortMods::ResourceProvider(0x434d444c, 2).empty() && PortMods::NativeTextureCount() == 1,
        "hidden geometry still deferred the mod");
  hidden = "models,effects,hud";
  ready = true;
  scan();
  Check(PortMods::RoomGeometryLoaded() && PortMods::ResourceProvider(0x434d444c, 2).empty() &&
        PortMods::NativeTextureCount() == 1 && !PortMods::EnvFxPath().empty(),
        "hidden models loaded, or textures or root HUD files left out");
  hidden.clear();
  frameBufferReserves = 0;
  ready = true;
  scan();
  Check(PortMods::CurrentStatus().mods[0].enabled && !PortMods::CurrentStatus().mods[0].pendingRestart,
        "ready geometry mod did not load");
  Check(frameBufferReserves == 1, "geometry mod did not ask for the frame buffers");
  Check(PortMods::RoomGeometryLoaded() && PortMods::NativeTextureCount() == 1 && overlays == 1 &&
        !PortMods::ResourceProvider(0x434d444c, 2).empty() && !PortMods::EnvFxPath().empty(),
        "ready geometry mod missing resources");
  disabled = "remastered-models";
  ready = false;
  Check(PortMods::HasRoomGeometry(), "startup must still find disabled geometry");
  PortMods::Initialize();
  Check(disabled == "remastered-models" && !PortMods::CurrentStatus().mods[0].enabled &&
        !PortMods::CurrentStatus().mods[0].pendingRestart, "disabled intent not preserved");
  disabled.clear();
  fs::remove(mod / "nested/12345678.RoOmGeO");
  fs::create_directories(mod / ".hidden");
  fs::create_directories(mod / "reports");
  put(".hidden/12345678.roomgeo");
  put("reports/12345678.roomgeo");
  put("not-an-id.roomgeo"); // a matching extension alone is not room geometry
  scan();
  Check(PortMods::CurrentStatus().mods[0].enabled && !PortMods::CurrentStatus().mods[0].pendingRestart &&
        !PortMods::RoomGeometryLoaded() && PortMods::NativeTextureCount() == 1 && overlays == 1,
        "nongeometry mod requires restart");
  fs::rename(mod, root / "dread-samus");
  scan();
  Check(PortMods::CurrentStatus().mods[0].enabled && !PortMods::CurrentStatus().mods[0].pendingRestart &&
        PortMods::NativeTextureCount() == 1 && overlays == 1, "Dread mod requires restart");
  modsOn = false;
  scan();
  Check(!PortMods::CurrentStatus().mods[0].enabled && !PortMods::CurrentStatus().mods[0].pendingRestart &&
        PortMods::NativeTextureCount() == 0 && overlays == 0, "global mods-off not preserved");
  fs::remove_all(root); // only this test's fixture
  return 0;
}
