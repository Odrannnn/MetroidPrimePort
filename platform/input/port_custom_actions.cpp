#include "port_custom_actions.h"

#include "port_apclient.h"
#include "port_console.h"
#include "port_debug.h"
#include "port_discord.h"
#include "port_freecam.h"
#include "port_remastered_text.h"
#include "port_savestate.h"

#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/TToken.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "rstl/optional_object.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <vector>

namespace PortCustomActions {

namespace {

// The presets, in PresetId's order.
enum class PresetId : int {
  Setting,
  Invulnerable,
  PhazonBeam,
  FreeCamera,
  FreezeGame,
  ShowSamus,
  RefillEnergy,
  RefillAmmo,
  GiveItem,
  TakeItem,
  KillTarget,
  Warp,
  SaveState,
  LoadState,
  HudMessage,
  Language,
  Command,
  Count,
};

constexpr Preset kPresets[] = {
    {"setting", "Settings", "Change a setting", "Turn one of the port's settings on, off or the other way",
     false, ArgKind::Setting, true},
    {"invulnerable", "Cheats", "Invincibility", "Samus takes no damage, until turned off again", true,
     ArgKind::None, true},
    {"phazon_beam", "Cheats", "Phazon Beam",
     "Fire the Phazon Beam with the Power Beam, in any room, without the Phazon Suit", true, ArgKind::None,
     true},
    {"free_camera", "Cheats", "Free camera", "Fly the view away from Samus to look around", true, ArgKind::None,
     true},
    {"freeze_game", "Cheats", "Freeze the game", "Hold the game still while the free camera flies", true,
     ArgKind::None, true},
    {"show_samus", "Cheats", "Show Samus in free camera", "Draw Samus where the player stands", true,
     ArgKind::None, true},
    {"refill_energy", "Cheats", "Refill energy", "Fill the energy tanks again", true, ArgKind::None, false},
    {"refill_ammo", "Cheats", "Refill missiles and power bombs", "Fill the missiles and power bombs again",
     true, ArgKind::None, false},
    {"give_item", "Cheats", "Give an item", "Add an item, as the console's give command does", true,
     ArgKind::Item, false},
    {"take_item", "Cheats", "Take an item", "Remove an item, as the console's take command does", true,
     ArgKind::Item, false},
    {"kill_target", "Cheats", "Kill the locked-on enemy", "Defeat the enemy the player is locked on to",
     true, ArgKind::None, false},
    {"warp", "Travel", "Warp to a room",
     "Go straight to a room; another world, or a room not loaded yet, reloads the world", true, ArgKind::Room,
     false},
    {"save_state", "Save states", "Save state", "Save the game into a slot", false, ArgKind::Slot, false},
    {"load_state", "Save states", "Load state", "Load a saved state from a slot", false, ArgKind::Slot, false},
    {"hud_message", "Display", "Show a message", "Show text as a HUD message", false, ArgKind::Text, false},
    {"language", "Display", "Text language", "Change the language of the game's text", false,
     ArgKind::Language, false},
    {"command", "Advanced", "Console command", "Run a debug console command", true, ArgKind::Text, false},
};
static_assert(std::size(kPresets) == size_t(PresetId::Count));

// The settings a `setting` action flips, with the F1 page's own labels and
// sections so the names match what the player reads there.
constexpr BoolSetting kBoolSettings[] = {
    {"hud_wide", "Widescreen HUD (spread to edges)", "Video > HUD and view", &PortDebug::HudWide,
     &PortDebug::SetHudWide},
    {"hide_helmet", "Hide helmet", "Video > HUD and view", &PortDebug::HideHelmet, &PortDebug::SetHideHelmet},
    {"hide_visor_effects", "Hide visor effects", "Video > HUD and view", &PortDebug::HideVisorEffects,
     &PortDebug::SetHideVisorEffects},
    {"power_spider_ball", "Suit-coloured morph balls", "Video > Display", &PortDebug::GeneratedBallLooks,
     &PortDebug::SetGeneratedBallLooks},
    {"vsync", "Vsync", "Video > Display", &PortDebug::VsyncEnabled, &PortDebug::SetVsyncEnabled},
#if defined(__ANDROID__)
    {"fullscreen", "Fullscreen (hide the status and navigation bars)", "Video > Display",
     &PortDebug::Fullscreen, &PortDebug::SetFullscreen},
#else
    {"fullscreen", "Fullscreen (F11)", "Video > Display", &PortDebug::Fullscreen, &PortDebug::SetFullscreen},
#endif
    {"per_pixel_lighting", "Per-pixel lighting", "Video > Quality", &PortDebug::PerPixelLighting,
     &PortDebug::SetPerPixelLighting},
    {"sun_shadows", "Sun shadows", "Video > Quality", &PortDebug::SunShadows, &PortDebug::SetSunShadows},
    {"projected_shadows", "Projected actor shadows", "Video > Quality", &PortDebug::ProjectedShadows,
     &PortDebug::SetProjectedShadows},
    {"frame_limit", "60 FPS cap (target)", "Video > Frame rate", &PortDebug::FrameLimitEnabled,
     &PortDebug::SetFrameLimitEnabled},
    {"skippable_cutscenes", "Skippable cutscenes", "Game > Cutscenes", &PortDebug::SkippableCutscenes,
     &PortDebug::SetSkippableCutscenes},
    {"beam_doors_stay_unlocked", "Unlock beam doors once opened", "Game > Cutscenes",
     &PortDebug::BeamDoorsStayUnlocked, &PortDebug::SetBeamDoorsStayUnlocked},
    {"stack_beams", "Stack beams", "Game > Cutscenes", &PortDebug::StackBeams, &PortDebug::SetStackBeams},
    {"speedrun_timer", "On-screen in-game time", "Game > Speedrun", &PortDebug::SpeedrunTimer,
     &PortDebug::SetSpeedrunTimer},
    {"reveal_map", "Reveal map", "Tracker", &PortDebug::RevealMap, &PortDebug::SetRevealMap},
    {"map_pickup_kinds", "Show major and minor items", "Tracker", &PortDebug::MapPickupKinds,
     &PortDebug::SetMapPickupKinds},
    {"map_station_pickups", "Map stations show pickups", "Tracker", &PortDebug::MapStationPickups,
     &PortDebug::SetMapStationPickups},
    {"map_extra_ui", "Extra map UI", "Tracker", &PortDebug::MapExtraUi, &PortDebug::SetMapExtraUi},
    {"map_logic_colors", "Colour the map's dots by logic", "Tracker > Archipelago checks",
     &PortDebug::MapLogicColors, &PortDebug::SetMapLogicColors},
    {"mouse_aim", "Mouse aim", "Controls > Mouse settings", &PortDebug::MouseAim, &PortDebug::SetMouseAim},
    {"pointer_aim", "Pointer aim (Trilogy style)", "Controls > Options > Aiming", &PortDebug::PointerAim,
     &PortDebug::SetPointerAim},
    {"twin_stick", "Twin stick (right stick aims)", "Controls > Controller settings > Stick aim",
     &PortDebug::PadTwinStick, &PortDebug::SetTwinStick},
    {"remastered_movement", "Remastered movement (twin stick and mouse aim)",
     "Controls > Controller settings > Stick aim", &PortDebug::RemasteredMovement,
     &PortDebug::SetRemasteredMovement},
    {"swap_scan_xray", "Swap the Scan and X-Ray visor buttons", "Controls > Options > Buttons",
     &PortDebug::SwapScanXray, &PortDebug::SetSwapScanXray},
    {"radial_menus", "Radial beam and visor menus (Trilogy)", "Controls > Options > Buttons",
     &PortDebug::RadialMenus, &PortDebug::SetRadialMenus},
    {"lock_on_toggle", "Toggle Lock-On", "Controls > Options > Buttons", &PortDebug::LockOnToggle,
     &PortDebug::SetLockOnToggle},
    {"sticky_charge", "Sticky Charge", "Controls > Options > Buttons", &PortDebug::StickyCharge,
     &PortDebug::SetStickyCharge},
    {"rapid_charge", "Remastered charge (rapid fire)", "Controls > Options > Buttons", &PortDebug::RapidCharge,
     &PortDebug::SetRapidCharge},
    {"fast_morph", "Fast Morph", "Controls > Options > Morph ball", &PortDebug::FastMorph,
     &PortDebug::SetFastMorph},
    {"spring_ball", "Spring Ball (C-stick up)", "Controls > Options > Morph ball", &PortDebug::SpringBall,
     &PortDebug::SetSpringBall},
};

// "on", "off", or the other way when the def says neither.
enum class Mode { Toggle, On, Off };

Mode ModeOf(std::string_view mode) {
  if (mode == "on") {
    return Mode::On;
  }
  if (mode == "off") {
    return Mode::Off;
  }
  return Mode::Toggle;
}

bool Next(Mode mode, bool now) { return mode == Mode::On ? true : mode == Mode::Off ? false : !now; }

PresetId IdOf(const Preset* preset) { return PresetId(preset - kPresets); }

CStateManager* Game() { return PortDebug::StateManager(); }

std::string OnOff(bool on) { return on ? "on" : "off"; }

// At most max bytes of text, cut on a character boundary, with an ellipsis.
std::string Shorten(std::string_view text, size_t max) {
  if (text.size() <= max) {
    return std::string(text);
  }
  size_t cut = max;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return std::string(text.substr(0, cut)) + "\xE2\x80\xA6";
}

const PortConsoleItem* FindItem(std::string_view key) {
  const std::span<const PortConsoleItem> items = PortConsoleItems();
  for (const PortConsoleItem& item : items) {
    if (key == item.key) {
      return &item;
    }
  }
  return nullptr;
}

// The table row of a language code, or nullptr (English is the disc's own and
// has no row).
const PortRemastered::TextLanguage* FindLanguage(std::string_view key) {
  for (size_t i = 0; i < PortRemastered::kTextLanguageCount; ++i) {
    if (key == PortRemastered::kTextLanguages[i].code) {
      return &PortRemastered::kTextLanguages[i];
    }
  }
  return nullptr;
}

bool IsLanguage(std::string_view key) { return key == "en" || FindLanguage(key) != nullptr; }

// The picker's list: English (the disc's own) and the languages a Remastered
// import's tables carry. Built once, from the table the console's command uses.
const std::vector<Language>& LanguageList() {
  static const std::vector<Language> list = [] {
    std::vector<Language> out;
    out.push_back({"en", "English"});
    for (size_t i = 0; i < PortRemastered::kTextLanguageCount; ++i) {
      out.push_back({PortRemastered::kTextLanguages[i].code, PortRemastered::kTextLanguages[i].name});
    }
    return out;
  }();
  return list;
}

std::string LanguageName(std::string_view key) {
  if (key == "en") {
    return "English";
  }
  const PortRemastered::TextLanguage* language = FindLanguage(key);
  return language != nullptr ? std::string(language->name) : std::string(key);
}

int ParseSlot(std::string_view text) {
  if (text.empty()) {
    return -1;
  }
  const int slot = std::atoi(std::string(text).c_str());
  return slot >= 1 && slot <= PortSaveState::kSlotCount ? slot : -1;
}

// A cheat is refused with the Original experience on, and in an Archipelago game,
// where the seed's own rules are what the player is playing.
std::string CheatRefusal() {
  if (PortDebug::OriginalExperience()) {
    return "Cheats are off in the Original experience";
  }
  if (PortAp::Connected()) {
    return "Cheats are off with Archipelago";
  }
  return {};
}

std::string RunSetting(const PortInput::CustomActionDef& def) {
  const BoolSetting* setting = FindBoolSetting(def.arg);
  if (setting == nullptr) {
    return {}; // a stale key: nothing to change, as the name says
  }
  const bool on = Next(ModeOf(def.mode), setting->get());
  setting->set(on);
  return std::string(setting->label) + " " + OnOff(on);
}

std::string RunPhazonBeam(const PortInput::CustomActionDef& def) {
  // The Cheats page's Phazon Beam: CPlayerGun::PortApplyPhazonCheat puts it in the
  // Power Beam's place each frame, and lets go of it through the exit morph.
  const bool on = Next(ModeOf(def.mode), PortDebug::PhazonBeamCheat());
  PortDebug::SetPhazonBeamCheat(on);
  return "Phazon Beam " + OnOff(on);
}

std::string RunFreeCamSwitch(const PortInput::CustomActionDef& def, PresetId id) {
  if (id != PresetId::FreeCamera && !PortFreeCam::Active()) {
    return "Turn the free camera on first";
  }
  CStateManager* mgr = Game();
  if (id == PresetId::FreeCamera) {
    const bool on = Next(ModeOf(def.mode), PortFreeCam::Active());
    if (on && mgr == nullptr) {
      return "Not in a game";
    }
    PortFreeCam::SetActive(on, mgr);
    return "Free camera " + OnOff(on);
  }
  if (id == PresetId::FreezeGame) {
    const bool on = Next(ModeOf(def.mode), PortFreeCam::Frozen());
    PortFreeCam::SetFrozen(on);
    return on ? "Game frozen" : "Game running";
  }
  const bool on = Next(ModeOf(def.mode), PortFreeCam::ShowPlayer());
  PortFreeCam::SetShowPlayer(on);
  return on ? "Samus shown" : "Samus hidden";
}

std::string RunItem(const PortInput::CustomActionDef& def, bool give) {
  const PortConsoleItem* item = FindItem(def.arg);
  if (item == nullptr) {
    return "That item no longer exists";
  }
  if (!PortConsoleRun((give ? "give " : "take ") + std::string(item->key))) {
    return {};
  }
  return (give ? "Gave " : "Took ") + std::string(item->label);
}

std::string RunKillTarget() {
  CStateManager* mgr = Game();
  const CPlayer* player = mgr != nullptr ? mgr->GetPlayer() : nullptr;
  if (player == nullptr) {
    return "Not in a game";
  }
  if (mgr->GetObjectById(player->GetOrbitTargetId()) == nullptr) {
    return "Nothing is locked on";
  }
  if (!PortConsoleRun("kill target")) {
    return {};
  }
  return "Killed the locked-on enemy";
}

std::string RunSlot(const PortInput::CustomActionDef& def, bool save) {
  const int slot = ParseSlot(def.arg);
  if (slot < 0) {
    return "That save state slot does not exist";
  }
  if (!(save ? PortSaveState::RequestSave(slot) : PortSaveState::RequestLoad(slot))) {
    return PortSaveState::LastMessage();
  }
  return (save ? "Saved to slot " : "Loaded slot ") + std::to_string(slot);
}

typedef rstl::optional_object<TCachedToken<CStringTable>> NameToken;

// One world's room names on their way in: its MLVL, then each room's name table.
struct WorldLoad {
  std::unique_ptr<CDummyWorld> level;
  std::vector<NameToken> names; // per rooms entry
};

std::vector<World> sWorlds;
std::vector<WorldLoad> sLoads; // per sWorlds entry

std::string WorldNameOf(int index) {
  return PortDiscord::GameTextToUtf8(gpMemoryCard->GetMemoryWorlds()[index].second.GetFrontEndName());
}

// Moves one world's load along; true once its rooms are all named.
bool LoadWorld(World& world, WorldLoad& load) {
  if (load.level) {
    if (!load.level->ICheckWorldComplete()) {
      return false;
    }
    const int count = load.level->IGetAreaCount();
    world.rooms.resize(count);
    load.names.resize(count);
    for (int i = 0; i < count; ++i) {
      const IGameArea* area = load.level->IGetAreaAlways(TAreaId(i));
      world.rooms[i].area = static_cast<uint32_t>(area->IGetAreaAssetId());
      const CAssetId strg = area->IGetStringTableAssetId();
      if (strg != kInvalidAssetId) {
        load.names[i] = TCachedToken<CStringTable>(gpSimplePool->GetObj(SObjectTag('STRG', strg)));
        load.names[i]->Lock();
      }
    }
    load.level.reset();
  }
  bool done = true;
  for (size_t i = 0; i < load.names.size(); ++i) {
    NameToken& token = load.names[i];
    if (!token) {
      continue;
    }
    if (!token->TryCache()) {
      done = false;
      continue;
    }
    if (token->GetObject()->GetStringCount() > 0) {
      world.rooms[i].name = PortDiscord::GameTextToUtf8(token->GetObject()->GetString(0));
    }
    token = NameToken(); // read: the table is not needed any more
  }
  if (!done) {
    return false;
  }
  for (size_t i = 0; i < world.rooms.size(); ++i) {
    if (world.rooms[i].name.empty()) {
      world.rooms[i].name = "Room " + std::to_string(i);
    }
  }
  std::stable_sort(world.rooms.begin(), world.rooms.end(),
                   [](const Room& a, const Room& b) { return a.name < b.name; });
  // A few rooms share a name (two "Connection Elevator to Deck Beta"): number them.
  for (size_t i = 0; i < world.rooms.size();) {
    size_t end = i + 1;
    while (end < world.rooms.size() && world.rooms[end].name == world.rooms[i].name) {
      ++end;
    }
    if (end - i > 1) {
      for (size_t j = i; j < end; ++j) {
        world.rooms[j].name += " " + std::to_string(j - i + 1);
      }
    }
    i = end;
  }
  load.names.clear();
  return true;
}

const World* FindWorld(uint32_t id) {
  for (const World& world : sWorlds) {
    if (world.id == id) {
      return &world;
    }
  }
  return nullptr;
}

// "Phendrana Drifts", "Ice Ruins West (Phendrana Drifts)", or "" when the names
// aren't read yet (or the world isn't on this disc).
std::string RoomLabel(uint32_t worldId, uint32_t area) {
  const World* world = FindWorld(worldId);
  if (world == nullptr || world->name.empty()) {
    return {};
  }
  if (area == 0) {
    return world->name;
  }
  for (const Room& room : world->rooms) {
    if (room.area == area) {
      return room.name + " (" + world->name + ")";
    }
  }
  return {};
}

std::string RunWarp(const PortInput::CustomActionDef& def) {
  uint32_t worldId = 0;
  uint32_t area = 0;
  if (!ParseRoomArg(def.arg, worldId, area)) {
    return "Pick a room to warp to";
  }
  CStateManager* mgr = Game();
  if (mgr == nullptr || gpGameState == nullptr) {
    return "Not in a game";
  }
  LoadRooms(); // names for the next press, if the Remap page never read them
  // A hand-edited controls.toml can name a world or room this disc lacks; the
  // world reload would then never finish.
  if (!sWorlds.empty()) {
    const World* target = FindWorld(worldId);
    if (target == nullptr) {
      return "That room is not on this disc";
    }
    if (target->loaded && area != 0 &&
        std::none_of(target->rooms.begin(), target->rooms.end(), [&](const Room& room) { return room.area == area; })) {
      return "That room is not on this disc";
    }
  }
  const std::string label = RoomLabel(worldId, area);
  const std::string note = label.empty() ? "Warping" : "Warping to " + label;
  // A loaded room of this world is entered in place, as the Cheats page does;
  // anything else reloads the world straight into it.
  CWorld* world = mgr->World();
  if (world != nullptr && area != 0 && static_cast<uint32_t>(gpGameState->CurrentWorldAssetId()) == worldId) {
    const TAreaId id = world->IGetAreaId(CAssetId(area));
    if (id != kInvalidAreaId && world->GetArea(id)->IsPostConstructed()) {
      PortDebug::RequestTeleport(id.Value());
      return note;
    }
  }
  PortDebug::RequestWorldTeleport(worldId, area);
  return note;
}

// The name a mode-bearing preset shows: "Free camera (on/off)", "Free camera: on".
std::string ModalName(std::string name, std::string_view mode) {
  if (mode == "on") {
    return name + ": on";
  }
  if (mode == "off") {
    return name + ": off";
  }
  return name + " (on/off)";
}

} // namespace

std::span<const Preset> Presets() { return kPresets; }

const Preset* FindPreset(std::string_view key) {
  for (const Preset& preset : kPresets) {
    if (key == preset.key) {
      return &preset;
    }
  }
  return nullptr;
}

std::span<const BoolSetting> BoolSettings() { return kBoolSettings; }

std::span<const Language> Languages() { return LanguageList(); }

void LoadRooms() {
  if (gpMemoryCard == nullptr || gpSimplePool == nullptr) {
    return;
  }
  const rstl::vector<CMemoryCard::MemoryWorld>& worlds = gpMemoryCard->GetMemoryWorlds();
  if (sWorlds.empty()) {
    for (int i = 0; i < worlds.size(); ++i) {
      World world;
      world.id = static_cast<uint32_t>(worlds[i].first);
      sWorlds.push_back(std::move(world));
      WorldLoad load;
      load.level.reset(new CDummyWorld(worlds[i].first, false));
      sLoads.push_back(std::move(load));
    }
  }
  for (size_t i = 0; i < sWorlds.size(); ++i) {
    World& world = sWorlds[i];
    if (world.name.empty()) {
      // The front-end names load with the memory card's own tables.
      world.name = WorldNameOf(static_cast<int>(i));
    }
    if (!world.loaded) {
      world.loaded = LoadWorld(world, sLoads[i]);
    }
  }
}

std::span<const World> Worlds() { return sWorlds; }

std::string RoomArg(uint32_t world, uint32_t area) {
  char text[24];
  std::snprintf(text, sizeof(text), "%08X:%08X", static_cast<unsigned>(world), static_cast<unsigned>(area));
  return text;
}

bool ParseRoomArg(std::string_view arg, uint32_t& world, uint32_t& area) {
  const size_t colon = arg.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == arg.size()) {
    return false;
  }
  const std::string worldText(arg.substr(0, colon));
  const std::string areaText(arg.substr(colon + 1));
  // Hex digits only: strtoul would also take a sign, spaces or "0x".
  const auto hex = [](const std::string& text) { return std::all_of(text.begin(), text.end(), ::isxdigit); };
  if (!hex(worldText) || !hex(areaText)) {
    return false;
  }
  char* end = nullptr;
  const unsigned long w = std::strtoul(worldText.c_str(), &end, 16);
  if (*end != '\0' || worldText.size() > 8) {
    return false;
  }
  const unsigned long a = std::strtoul(areaText.c_str(), &end, 16);
  if (*end != '\0' || areaText.size() > 8 || w == 0) {
    return false;
  }
  world = static_cast<uint32_t>(w);
  area = static_cast<uint32_t>(a);
  return true;
}

const BoolSetting* FindBoolSetting(std::string_view key) {
  if (key.empty()) {
    return nullptr;
  }
  for (const BoolSetting& setting : kBoolSettings) {
    if (key == setting.key) {
      return &setting;
    }
  }
  return nullptr;
}

std::string AutoName(const PortInput::CustomActionDef& def) {
  const Preset* preset = FindPreset(def.preset);
  if (preset == nullptr) {
    return "Custom action";
  }
  switch (IdOf(preset)) {
  case PresetId::Setting: {
    const BoolSetting* setting = FindBoolSetting(def.arg);
    if (setting != nullptr) return ModalName(setting->label, def.mode);
    // None picked yet, or one this build doesn't have (a newer build's file).
    return def.arg.empty() ? "Change a setting" : "Unknown setting \"" + Shorten(def.arg, 24) + "\"";
  }
  case PresetId::Invulnerable:
    return ModalName("Invincibility", def.mode);
  case PresetId::PhazonBeam:
    return ModalName("Phazon Beam", def.mode);
  case PresetId::FreeCamera:
    return ModalName("Free camera", def.mode);
  case PresetId::FreezeGame:
    return ModalName("Freeze the game", def.mode);
  case PresetId::ShowSamus:
    return ModalName("Show Samus in free camera", def.mode);
  case PresetId::RefillEnergy:
    return "Refill energy";
  case PresetId::RefillAmmo:
    return "Refill missiles and power bombs";
  case PresetId::GiveItem:
  case PresetId::TakeItem: {
    const PortConsoleItem* item = FindItem(def.arg);
    const char* verb = IdOf(preset) == PresetId::GiveItem ? "Give " : "Take ";
    return std::string(verb) + (item != nullptr ? item->label : "item");
  }
  case PresetId::KillTarget:
    return "Kill the locked-on enemy";
  case PresetId::Warp: {
    uint32_t world = 0;
    uint32_t area = 0;
    if (!ParseRoomArg(def.arg, world, area)) {
      return "Warp to a room";
    }
    const std::string label = RoomLabel(world, area);
    if (label.empty()) {
      return "Warp to a room"; // names not read yet, or a world this disc lacks
    }
    return area == 0 ? "Warp to the start of " + label : "Warp to " + label;
  }
  case PresetId::SaveState:
  case PresetId::LoadState: {
    const int slot = ParseSlot(def.arg);
    const std::string name = IdOf(preset) == PresetId::SaveState ? "Save state" : "Load state";
    return slot < 0 ? name : name + (IdOf(preset) == PresetId::SaveState ? " to slot " : " from slot ") +
                                 std::to_string(slot);
  }
  case PresetId::HudMessage:
    return def.arg.empty() ? "Show a message" : "Show message \"" + Shorten(def.arg, 24) + "\"";
  case PresetId::Language:
    return def.arg.empty() ? "Text language" : "Text language: " + LanguageName(def.arg);
  case PresetId::Command:
    return "Console command";
  case PresetId::Count:
    break;
  }
  return "Custom action";
}

std::string DisplayName(const PortInput::CustomActionDef& def) {
  if (def.renamed && !def.name.empty()) {
    return def.name;
  }
  return AutoName(def);
}

std::string Run(const PortInput::CustomActionDef& def) {
  const Preset* preset = FindPreset(def.preset);
  if (preset == nullptr) {
    return {};
  }
  if (preset->cheat) {
    std::string refused = CheatRefusal();
    if (!refused.empty()) {
      return refused;
    }
  }
  switch (IdOf(preset)) {
  case PresetId::Setting:
    return RunSetting(def);
  case PresetId::Invulnerable: {
    const bool on = Next(ModeOf(def.mode), PortDebug::Invulnerable());
    PortDebug::SetInvulnerable(on);
    return "Invincibility " + OnOff(on);
  }
  case PresetId::PhazonBeam:
    return RunPhazonBeam(def);
  case PresetId::FreeCamera:
  case PresetId::FreezeGame:
  case PresetId::ShowSamus:
    return RunFreeCamSwitch(def, IdOf(preset));
  case PresetId::RefillEnergy:
    if (!PortConsoleRun("refill energy")) {
      return {};
    }
    return "Energy refilled";
  case PresetId::RefillAmmo:
    if (!PortConsoleRun("refill ammo")) {
      return {};
    }
    return "Missiles and power bombs refilled";
  case PresetId::GiveItem:
    return RunItem(def, true);
  case PresetId::TakeItem:
    return RunItem(def, false);
  case PresetId::KillTarget:
    return RunKillTarget();
  case PresetId::Warp:
    return RunWarp(def);
  case PresetId::SaveState:
    return RunSlot(def, true);
  case PresetId::LoadState:
    return RunSlot(def, false);
  case PresetId::HudMessage:
    if (def.arg.empty()) {
      return "The message is empty";
    }
    if (!PortConsoleRun("memo " + def.arg)) {
      return {};
    }
    return "Message shown";
  case PresetId::Language:
    if (!IsLanguage(def.arg)) {
      return "That language is not available";
    }
    if (!PortConsoleRun("language " + def.arg)) {
      return {};
    }
    return "Text language: " + LanguageName(def.arg);
  case PresetId::Command:
    if (def.command.empty()) {
      return "The command is empty";
    }
    // Its reply goes to the log; a note would only repeat part of it.
    PortConsoleRun(def.command);
    return {};
  case PresetId::Count:
    break;
  }
  return {};
}

} // namespace PortCustomActions
