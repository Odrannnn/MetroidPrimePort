#include "port_savestate.h"

#include "port_debug.h"
#include "port_gci.h"
#include "port_paths.h"
#include "port_mods.h"
#include "port_remastered_import.h"
#include "port_tracker.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CMemoryStreamOut.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"

#include "port_discord.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

// CStateManager.cpp: keep the music through a state load, then start the
// area's once Samus is placed.
void PortKeepMusicForStateLoad(bool sameWorld);
void PortStartStateLoadMusic(CStateManager& mgr);

namespace PortSaveState {
namespace {

// CGameState::PutTo writes under 1 KiB (940 bytes retail, plus the
// Archipelago trailer); the tail stays zero, as on a memory card.
constexpr size_t kBlobSize = 4096;

int sSelected = 1;
int sSaveRequest = -1;
int sLoadRequest = -1;
bool sReloadRequest = false;
// The mods are read again where the next game starts (InstallPending).
bool sReloadMods = false;
// In the front end nothing of a game is loaded, so the mods are read again
// at CFrontEndUI's next tick (TakeFrontEndReload), instead of at the next
// game: a Remastered import started at the title screen unloads them then.
bool sInFrontEnd = false;
bool sFrontEndReload = false;
int sModReloads = 0;
std::string sMessage;
// SlotInfo is drawn every overlay frame; files change only through WriteSlot.
Info sInfoCache[kSlotCount + 1];
bool sInfoCached[kSlotCount + 1] = {};

// A load in flight: the blob waits for CMainFlow to build the next game from
// it, then the header places Samus once that game runs.
bool sInstallPending = false;
bool sPlacePending = false;
Header sPendingHeader;
std::vector< uint8_t > sPendingBlob;

void SetMessage(const std::string& text) {
  sMessage = text;
  std::fprintf(stderr, "[savestate] %s\n", text.c_str());
}

std::string SlotName(int slot) { return slot == kUndoSlot ? "undo" : "slot " + std::to_string(slot); }

fs::path SlotPath(int slot) {
  const std::string folder = Folder();
  if (folder.empty())
    return {};
  return PortGci::PathFromString(folder) / ("slot" + std::to_string(slot) + ".mpss");
}

bool ReadSlot(int slot, Header& header, std::vector< uint8_t >& blob) {
  const fs::path path = SlotPath(slot);
  if (path.empty())
    return false;
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  const std::string data((std::istreambuf_iterator< char >(file)), std::istreambuf_iterator< char >());
  return Decode(data, header, blob);
}

bool WriteSlot(int slot, const Header& header, const std::vector< uint8_t >& blob) {
  const fs::path path = SlotPath(slot);
  if (path.empty())
    return false;
  const std::string data = Encode(header, blob);
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file)
      return false;
    file.write(data.data(), static_cast< std::streamsize >(data.size()));
    if (!file)
      return false;
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  sInfoCached[slot] = false;
  return !ec;
}

std::string WorldName(CAssetId world) {
  if (gpMemoryCard == nullptr)
    return {};
  const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
  for (int i = 0; i < worlds.size(); ++i) {
    if (worlds[i].first == world)
      return PortDiscord::GameTextToUtf8(worlds[i].second.GetFrontEndName());
  }
  return {};
}

std::string RoomName(const CStateManager& mgr, int area) {
  const PortTracker::Summary summary = PortTracker::Collect(mgr);
  for (const PortTracker::Room& room : summary.rooms) {
    if (room.index == area)
      return room.name;
  }
  return "Room " + std::to_string(area);
}

bool PlayerCanSave(CStateManager& mgr, std::string& why) {
  const CPlayer* player = mgr.GetPlayer();
  const CWorld* world = mgr.GetWorld();
  if (player == nullptr || world == nullptr || gpGameState == nullptr) {
    why = "no game running";
    return false;
  }
  if (mgr.GetWantsToQuit()) {
    why = "the game is quitting";
    return false;
  }
  if (player->IsPlayerDeadEnough() || player->GetDeathTime() > 0.f) {
    why = "Samus is dead";
    return false;
  }
  return true;
}

bool Capture(CStateManager& mgr, Header& header, std::vector< uint8_t >& blob) {
  const CPlayer& player = *mgr.GetPlayer();
  const CWorld& world = *mgr.GetWorld();
  const TAreaId area = world.GetCurrentAreaId();
  // The save puts the next game in this room (normally already so; set it in
  // case a transition is half done).
  CWorldState& worldState = gpGameState->CurrentWorldState();
  worldState.SetAreaId(area);
  worldState.SetDesiredAreaAssetId(kInvalidAssetId);

  blob.assign(kBlobSize, 0);
  {
    CMemoryStreamOut out(blob.data(), blob.size());
    gpGameState->PutTo(out);
  }

  const CTransform4f& xf = player.GetTransform();
  const CVector3f pos = xf.GetTranslation();
  const CVector3f fwd = xf.GetForward();
  header = {};
  header.worldId = static_cast< uint32_t >(world.IGetWorldAssetId());
  header.areaId = area.Value();
  header.position[0] = pos.GetX();
  header.position[1] = pos.GetY();
  header.position[2] = pos.GetZ();
  header.forward[0] = fwd.GetX();
  header.forward[1] = fwd.GetY();
  header.forward[2] = fwd.GetZ();
  const CPlayer::EPlayerMorphBallState morph = player.GetMorphballTransitionState();
  header.morphed = morph == CPlayer::kMS_Morphed || morph == CPlayer::kMS_Morphing;
  header.playTime = gpGameState->GetTotalPlayTime();
  header.savedAt = static_cast< int64_t >(std::time(nullptr));
  header.world = WorldName(world.IGetWorldAssetId());
  header.room = RoomName(mgr, area.Value());
  return true;
}

void DoSave(CStateManager& mgr, int slot) {
  std::string why;
  if (!PlayerCanSave(mgr, why)) {
    SetMessage("Can't save to " + SlotName(slot) + ": " + why);
    return;
  }
  Header header;
  std::vector< uint8_t > blob;
  Capture(mgr, header, blob);
  if (!WriteSlot(slot, header, blob)) {
    SetMessage("Couldn't write " + SlotName(slot) + " in " + Folder());
    return;
  }
  SetMessage("Saved " + SlotName(slot) + ": " + header.world + " - " + header.room);
}

void QuitToPending(CStateManager& mgr) {
  sInstallPending = true;
  sPlacePending = false;

  // As the debug world warp: stop the outgoing world's loads, retire its PAKs
  // and quit to CMainFlow, which starts the next game (InstallPending).
  const_cast< CWorld* >(mgr.GetWorld())->SetLoadPauseState(true);
  PortKeepMusicForStateLoad(static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId()) ==
                            sPendingHeader.worldId);
  gpGameState->SetCurrentWorldId(static_cast< CAssetId >(sPendingHeader.worldId));
  gpMain->SetRestartMode(CMain::kRM_None);
  mgr.QuitGame();
}

// The game is rebuilt where it stands, as a state saved and loaded at once,
// so that everything it draws is loaded again, from the new files.
bool DoModReload(CStateManager& mgr) {
  std::string why;
  if (!PlayerCanSave(mgr, why)) {
    SetMessage("Can't reload the mods: " + why);
    return false;
  }
  Capture(mgr, sPendingHeader, sPendingBlob);
  sReloadMods = true;
  QuitToPending(mgr);
  SetMessage("Reloading the mods");
  return true;
}

void ReloadModFiles() {
  PortMods::BeginReload();
  PortRemastered::ApplyPendingImport();
  PortMods::FinishReload();
  ++sModReloads;
}

bool DoLoad(CStateManager& mgr, int slot) {
  Header header;
  std::vector< uint8_t > blob;
  if (!ReadSlot(slot, header, blob)) {
    SetMessage(SlotName(slot) + " is empty or unreadable");
    return false;
  }
  std::string why;
  if (!PlayerCanSave(mgr, why)) {
    SetMessage("Can't load " + SlotName(slot) + ": " + why);
    return false;
  }
  if (slot != kUndoSlot) {
    Header undo;
    std::vector< uint8_t > undoBlob;
    Capture(mgr, undo, undoBlob);
    WriteSlot(kUndoSlot, undo, undoBlob);
  }

  sPendingHeader = header;
  sPendingBlob = std::move(blob);
  QuitToPending(mgr);
  SetMessage("Loading " + SlotName(slot) + ": " + header.world + " - " + header.room);
  return true;
}

void Place(CStateManager& mgr) {
  CPlayer& player = *mgr.Player();
  const Header& h = sPendingHeader;
  const CVector3f pos(h.position[0], h.position[1], h.position[2]);
  CVector3f look(h.forward[0], h.forward[1], 0.f);
  if (!look.CanBeNormalized())
    look = CVector3f(0.f, 1.f, 0.f);
  player.Teleport(CTransform4f::LookAt(pos, pos + look, CVector3f(0.f, 0.f, 1.f)), mgr, true);
  player.SetVelocityWR(CVector3f(0.f, 0.f, 0.f));
  const bool morphed = player.GetMorphballTransitionState() == CPlayer::kMS_Morphed;
  if (h.morphed != morphed) {
    player.SetSpawnedMorphBallState(h.morphed ? CPlayer::kMS_Morphed : CPlayer::kMS_Unmorphed,
                                    mgr);
  }
  mgr.CameraManager()->ResetCameras(mgr);
  sPendingBlob.clear();
}

} // namespace

std::string Folder() {
  std::string dir = PortPaths::UserFolder();
  if (dir.empty()) {
    return {};
  }
  dir += "savestates";
  std::error_code ec;
  fs::create_directories(PortGci::PathFromString(dir), ec);
  return dir;
}

Info SlotInfo(int slot) {
  if (slot < kUndoSlot || slot > kSlotCount)
    return {};
  if (sInfoCached[slot])
    return sInfoCache[slot];
  Info info;
  sInfoCached[slot] = true;
  sInfoCache[slot] = info;
  Header header;
  std::vector< uint8_t > blob;
  if (!ReadSlot(slot, header, blob))
    return info;
  info.exists = true;
  info.world = header.world;
  info.room = header.room;
  info.playTime = header.playTime;
  info.savedAt = header.savedAt;
  info.morphed = header.morphed;
  sInfoCache[slot] = info;
  return info;
}

int SelectedSlot() { return sSelected; }

void SetSelectedSlot(int slot) {
  if (slot >= 1 && slot <= kSlotCount)
    sSelected = slot;
}

bool RequestSave(int slot) {
  if (slot < 1 || slot > kSlotCount) {
    SetMessage("No such slot");
    return false;
  }
  if (PortDebug::StateManager() == nullptr) {
    SetMessage("Can't save: no game running");
    return false;
  }
  sSaveRequest = slot;
  return true;
}

bool RequestLoad(int slot) {
  if (slot < kUndoSlot || slot > kSlotCount) {
    SetMessage("No such slot");
    return false;
  }
  if (PortDebug::StateManager() == nullptr) {
    SetMessage("Can't load: no game running");
    return false;
  }
  if (sInstallPending || sPlacePending) {
    SetMessage("A load is already in progress");
    return false;
  }
  if (!SlotInfo(slot).exists) {
    SetMessage(slot == kUndoSlot ? "Nothing to undo" : "Slot is empty");
    return false;
  }
  sLoadRequest = slot;
  return true;
}

int ModReloads() { return sModReloads; }

bool RequestModReload() {
  if (PortDebug::StateManager() == nullptr) {
    if (sInFrontEnd) {
      sFrontEndReload = true;
      return true;
    }
    // Nothing of a game is loaded: the next one starts from the new files.
    sReloadMods = true;
    SetMessage("The mods reload when the game starts");
    return true;
  }
  sReloadRequest = true;
  return true;
}

std::string LastMessage() { return sMessage; }

bool Tick(CStateManager& mgr) {
  if (sPlacePending && !mgr.GetWantsToQuit() && mgr.GetWorld() != nullptr &&
      mgr.GetGameState() == CStateManager::kGS_Running) {
    sPlacePending = false;
    if (static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId()) == sPendingHeader.worldId) {
      Place(mgr);
    }
    PortStartStateLoadMusic(mgr);
  }
  if (sInstallPending) {
    // Still in the outgoing game, whose PAKs are retired: run nothing more
    // until CMainFlow replaces it.
    return mgr.GetWantsToQuit();
  }
  if (sSaveRequest >= 0) {
    const int slot = sSaveRequest;
    sSaveRequest = -1;
    DoSave(mgr, slot);
  }
  if (sLoadRequest >= 0) {
    const int slot = sLoadRequest;
    sLoadRequest = -1;
    return DoLoad(mgr, slot);
  }
  if (sReloadRequest) {
    sReloadRequest = false;
    return DoModReload(mgr);
  }
  return false;
}

void ReloadModsNow() {
  sReloadMods = false;
  gpResourceFactory->PortReopenPaks(ReloadModFiles);
  const PortMods::Status& status = PortMods::CurrentStatus();
  SetMessage("Mods reloaded: " + std::to_string(status.overlays) + " disc file(s), " +
             std::to_string(PortMods::NativeTextureCount()) + " native texture(s)");
}

void SetInFrontEnd(bool inFrontEnd) {
  sInFrontEnd = inFrontEnd;
  if (!inFrontEnd && sFrontEndReload) {
    // Left before CFrontEndUI got to it: the next game reads the new files.
    sFrontEndReload = false;
    sReloadMods = true;
  }
}

bool TakeFrontEndReload() {
  if (!sFrontEndReload || !sInFrontEnd || PortDebug::StateManager() != nullptr) {
    return false;
  }
  sFrontEndReload = false;
  return true;
}

void InstallPending() {
  if (sReloadMods) {
    ReloadModsNow();
  }
  if (!sInstallPending)
    return;
  sInstallPending = false;
  CMemoryInStream in(sPendingBlob.data(), sPendingBlob.size(), CMemoryInStream::kOS_NotOwned);
  gpMain->PortLoadGameState(in);
  sPlacePending = true;
}

} // namespace PortSaveState
