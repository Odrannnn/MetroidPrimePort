#include "port_savestate.h"
#include "port_strings.h"

#include "port_apclient.h"
#include "port_debug.h"
#include "port_beam_doors.h"
#include "port_gci.h"
#include "port_log.h"
#include "port_map_side.h"
#include "port_paths.h"
#include "port_randomizer.h"
#include "port_mods.h"
#include "port_remastered_import.h"
#include "port_tracker.h"
#include "port_world_holders.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CMemoryStreamOut.hpp"
#include "MetroidPrime/CInGameGuiManager.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CWorldTransManager.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"

#include "port_discord.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

// CStateManager.cpp: keep the music through a state load, then start the
// area's once Samus is placed.
void PortKeepMusicForStateLoad(bool sameWorld);
void PortStartStateLoadMusic(CStateManager& mgr);
void PortEndStateLoadMusic();

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

// The room state of that load, applied area by area as the rebuilt areas come
// up. Areas that load later than kRoomApplyFrames ticks after the place keep
// the memory card load's fresh objects (the player may have left and come back).
constexpr int kRoomApplyFrames = 600;
constexpr int kRoomLateFrames = 3600;
bool sRoomPending = false;
// The primary restore is over (the log line is out) but neighbour areas that were
// resident at the save are still streaming in; each takes its state when it finishes.
// Ends when all have, when Samus leaves the saved room, or after kRoomLateFrames.
bool sRoomLate = false;
// The room state names the music that was playing: the room restore plays it, and
// the area-music guess (PortStartStateLoadMusic) only runs if none of it could be.
bool sRoomMusic = false;
int sRoomFrames = 0;
std::vector< int32_t > sRoomDone;
RoomApplyStats sRoomStats;

// Quick resume: a load that came from resume.mpss deletes the file once
// Samus is placed. sResumeLoadRequest is the console's (or a hotkey's) load
// from inside a game.
bool sResumeLoading = false;
bool sResumeLoadRequest = false;

// The run marker ("running" next to resume.mpss): present while the game runs,
// removed on a clean quit and while Android has the app in the background
// (where it may be killed without warning). One left at launch means the last
// run crashed or hung. It holds "resumed <savedAt>" once the run resumed, so a
// crash before any newer snapshot shows as the same state crashing again.
bool sLastRunCrashed = false;
std::string sLastRunMarker;
std::string sMarker;

// The resume snapshot: the bytes resume.mpss will hold, built on the game
// thread at a safe moment and written from wherever the quit or background
// event arrives. Everything but the game-thread-only timers is under the mutex.
using Clock = std::chrono::steady_clock;
struct ResumeSnapshot {
  bool valid = false;
  std::string bytes;
  fs::path path;
  ResumeMeta meta;
  std::string world;
  std::string room;
  uint32_t worldId = 0;
  uint32_t areaId = 0;
  Clock::time_point taken;
  uint64_t gen = 0;
  uint64_t writtenGen = 0;
};
std::mutex sSnapMutex;
ResumeSnapshot sSnap;
std::atomic< bool > sResumeEnabled{true};
std::atomic< bool > sPumpSafe{false};
std::thread::id sGameThread;
// Game thread only: when the current room was first seen, and the snapshot's.
uint64_t sRoomKey = 0;
Clock::time_point sRoomSince;

void SetMessage(const std::string& text) {
  sMessage = text;
  PortLog::Write("[savestate] %s\n", text.c_str());
}

std::string SlotName(int slot) { return slot == kUndoSlot ? "undo" : "slot " + std::to_string(slot); }

fs::path SlotPath(int slot) {
  const std::string folder = Folder();
  if (folder.empty())
    return {};
  return PortGci::PathFromString(folder) / ("slot" + std::to_string(slot) + ".mpss");
}

// A state the running game can build: Capture always writes kBlobSize bytes
// (CGameState reads them without a bound), and its world must be one this disc
// has, or SetCurrentWorldId sends the main flow to an MLVL that isn't there.
bool Loadable(const Header& header, const std::vector< uint8_t >& blob) {
  if (blob.size() != kBlobSize)
    return false;
  if (gpMemoryCard == nullptr)
    return true;
  const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
  for (int i = 0; i < worlds.size(); ++i) {
    if (static_cast< uint32_t >(worlds[i].first) == header.worldId)
      return true;
  }
  return false;
}

bool ReadSlot(int slot, Header& header, std::vector< uint8_t >& blob) {
  const fs::path path = SlotPath(slot);
  if (path.empty())
    return false;
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  const std::string data = port::ReadAllText(file);
  return Decode(data, header, blob) && Loadable(header, blob);
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
    file.flush();
    file.close();
    if (!file) {
      std::error_code ignored;
      fs::remove(temp, ignored);
      return false;
    }
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

// The map record of a captured blob; defined with the load side below.
void StoreMapData(std::vector< uint8_t >& blob, size_t gameBits);
void StoreBeamData(std::vector< uint8_t >& blob, size_t gameBits);

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
  size_t gameBits = 0;
  {
    // During a state load's restore, areas not yet spawned carry the state's spawn bits
    // (#161): write their live bits.
    SetSpawnLayersLive(true);
    CMemoryStreamOut out(blob.data(), blob.size());
    gpGameState->PutTo(out);
    gameBits = out.PortBitsWritten();
    SetSpawnLayersLive(false);
  }
  // After the stream is gone: it flushes its last byte at the destructor, and
  // the record's fingerprint has to cover the bytes as they are left here.
  StoreBeamData(blob, gameBits);
  StoreMapData(blob, gameBits);

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
  if (PortDebug::SaveStateRoomState())
    CaptureRoomState(mgr, header.roomState);
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
  DisarmSpawnLayers();
  sInstallPending = true;
  sPlacePending = false;
  sRoomPending = false;
  sRoomLate = false;
  sRoomMusic = false;

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

// The map data the blob's own trailers cannot carry (port_map_side.h): the
// "RMMK" trailer holds PortMapMarks::kTrailerMarks marks, the save's list holds
// PortMapMarks::kMaxMarks, and a state that came back with only the trailer
// would drop the rest - and the next save would then write that shortened list
// into the card's side file. The record sits at a fixed offset past the whole
// game state (which stays under 1 KiB) as a length and the side file's own
// bytes; its fingerprint covers everything before it, so a state written by an
// older build (zeros there) or a damaged one is ignored and the trailers stand.
constexpr size_t kMapOffset = kBlobSize - 256;
// The open beam doorways (port_beam_doors.h) sit in the 64 bytes before it, the
// same way: a length and the side file's bytes, fingerprinted over everything
// before this offset. It is stored first, so the map record's fingerprint covers
// it too.
constexpr size_t kBeamOffset = kMapOffset - 64;
static_assert(PortBeamDoors::kSideMaxBytes + 4 <= kMapOffset - kBeamOffset,
              "the beam door record fits before the map record");
static_assert(PortMapSide::kMaxBytes + 4 <= kBlobSize - kMapOffset,
              "the map record fits the tail of the blob");

void StoreMapData(std::vector< uint8_t >& blob, size_t gameBits) {
  if (blob.size() < kBlobSize || gpGameState == nullptr) {
    return;
  }
  if (gameBits > kBeamOffset * 8) {
    PortLog::Write("save state: the game state grew past the map record (%u bits); marks stay "
                   "in the trailers only\n",
                   static_cast< unsigned >(gameBits));
    return;
  }
  PortMapSide::Data data;
  for (int i = 0; i < PortMapPickups::kSeenWords; ++i) {
    data.seen[i] = gpGameState->PortSeenPickups()[i];
  }
  data.marks = gpGameState->PortMapMarkList();
  const std::vector< uint8_t > bytes =
      PortMapSide::Encode(PortMapSide::Fingerprint(blob.data(), kMapOffset), data);
  uint8_t* tail = blob.data() + kMapOffset;
  for (int i = 0; i < 4; ++i) {
    tail[i] = static_cast< uint8_t >(bytes.size() >> (8 * i));
  }
  std::memcpy(tail + 4, bytes.data(), bytes.size());
}

void StoreBeamData(std::vector< uint8_t >& blob, size_t gameBits) {
  if (blob.size() < kBlobSize || gpGameState == nullptr || gameBits > kBeamOffset * 8) {
    return;
  }
  const std::vector< uint8_t > bytes =
      PortBeamDoors::Encode(PortMapSide::Fingerprint(blob.data(), kBeamOffset), gpGameState->PortBeamDoorBits());
  uint8_t* tail = blob.data() + kBeamOffset;
  for (int i = 0; i < 4; ++i) {
    tail[i] = static_cast< uint8_t >(bytes.size() >> (8 * i));
  }
  std::memcpy(tail + 4, bytes.data(), bytes.size());
}

// The record back over the rebuilt game state; false (nothing changed) for a
// state without one (written by an older build, zeros there) or a damaged one.
bool LoadBeamData(const std::vector< uint8_t >& blob) {
  if (gpGameState == nullptr || blob.size() < kMapOffset) {
    return false;
  }
  const uint8_t* tail = blob.data() + kBeamOffset;
  uint32_t size = 0;
  for (int i = 0; i < 4; ++i) {
    size |= static_cast< uint32_t >(tail[i]) << (8 * i);
  }
  if (size > PortBeamDoors::kSideMaxBytes || kBeamOffset + 4 + size > kMapOffset) {
    return false;
  }
  uint32_t bits[PortBeamDoors::kWords] = {};
  if (!PortBeamDoors::Decode(tail + 4, size, PortMapSide::Fingerprint(blob.data(), kBeamOffset), bits)) {
    return false;
  }
  for (int i = 0; i < PortBeamDoors::kWords; ++i) {
    gpGameState->PortBeamDoorBits()[i] = bits[i];
  }
  return true;
}

// Puts the record back over the game state the blob was just built into. False
// (nothing changed) when there is none: an older state keeps what its own
// trailers gave, which is up to kTrailerMarks marks and the whole seen set.
bool LoadMapData(const std::vector< uint8_t >& blob) {
  if (gpGameState == nullptr || blob.size() < kMapOffset + 4) {
    return false;
  }
  const uint8_t* tail = blob.data() + kMapOffset;
  uint32_t size = 0;
  for (int i = 0; i < 4; ++i) {
    size |= static_cast< uint32_t >(tail[i]) << (8 * i);
  }
  if (size > PortMapSide::kMaxBytes || kMapOffset + 4 + size > blob.size()) {
    return false;
  }
  PortMapSide::Data data;
  if (!PortMapSide::Decode(tail + 4, size, PortMapSide::Fingerprint(blob.data(), kMapOffset),
                           data)) {
    return false;
  }
  for (int i = 0; i < PortMapPickups::kSeenWords; ++i) {
    gpGameState->PortSeenPickups()[i] = data.seen[i];
  }
  gpGameState->PortMapMarkList() = data.marks;
  return true;
}

void Place(CStateManager& mgr) {
  CPlayer& player = *mgr.Player();
  // The retained Remastered physics object state is port-only and never serialized, and the
  // state being loaded cannot carry it: invalidate it at the one PC point where the player is
  // put back into a rebuilt world (both a slot load and quick resume reach here), so a value
  // from before the load is never carried across it. The next verified Remastered airborne
  // tick restores validity; until then the grounded Remastered mover stands aside (see
  // CGameCollision::MovePlayer).
  player.PortInvalidateRetainedObjectState();
  // Same for the platform rider state (issue #117): no rider and no deferred carry survive.
  player.PortResetRiderState();
  const Header& h = sPendingHeader;
  const CVector3f pos(h.position[0], h.position[1], h.position[2]);
  CVector3f look(h.forward[0], h.forward[1], 0.f);
  if (!look.CanBeNormalized())
    look = CVector3f(0.f, 1.f, 0.f);
  player.Teleport(CTransform4f::LookAt(pos, pos + look, CVector3f(0.f, 0.f, 1.f)), mgr, true);
  player.SetVelocityWR(CVector3f(0.f, 0.f, 0.f));
  // The rebuilt player has no area until its own first Update, which runs after the
  // enemies' (a Metroid whose target is the player loses interest while it reads -1).
  mgr.SetActorAreaId(player, mgr.GetWorld()->GetCurrentAreaId());
  const bool morphed = player.GetMorphballTransitionState() == CPlayer::kMS_Morphed;
  if (h.morphed != morphed) {
    player.SetSpawnedMorphBallState(h.morphed ? CPlayer::kMS_Morphed : CPlayer::kMS_Unmorphed,
                                    mgr);
  }
  mgr.CameraManager()->ResetCameras(mgr);
  sPendingBlob.clear();
  sRoomPending = PortDebug::SaveStateRoomState() && !h.roomState.empty();
  if (!sRoomPending)
    DisarmSpawnLayers();  // no restore will run to the end that disarms them
  sRoomLate = false;
  sRoomFrames = 0;
  sRoomDone.clear();
  sRoomStats = {};
}

void EndRoomState() {
  sRoomPending = false;
  sRoomLate = false;
  DisarmSpawnLayers();
}

// Puts the saved room objects on the rebuilt areas as each one finishes loading.
void TickRoomState(CStateManager& mgr) {
  if (!(sRoomPending || sRoomLate) || mgr.GetWantsToQuit())
    return;
  const int waiting = ApplyRoomState(mgr, sPendingHeader.roomState, sRoomDone, sRoomStats);
  if (sRoomLate) {
    ++sRoomFrames;
    if (sRoomStats.late == 0 || sRoomFrames >= kRoomLateFrames ||
        mgr.GetWorld()->GetCurrentAreaId().Value() != static_cast< int >(sPendingHeader.areaId)) {
      PortLog::Write("[savestate] room state, neighbour areas: %d applied in total, %d areas %s\n",
                     sRoomStats.applied, sRoomStats.late,
                     sRoomStats.late == 0 ? "all loaded" : "never loaded; left as loaded");
      EndRoomState();
    }
    return;
  }
  if (waiting > 0 && ++sRoomFrames < kRoomApplyFrames)
    return;
  const bool late = sRoomStats.late > 0;
  PortLog::Write("[savestate] room state: %d applied, %d generated, %d removed, %d unmatched, "
                 "%d areas skipped, %d script messages replayed, %d enemy fields set%s\n",
                 sRoomStats.applied, sRoomStats.generated, sRoomStats.removed, sRoomStats.unmatched,
                 sRoomStats.skipped, sRoomStats.replayed, sRoomStats.fields,
                 late ? ", neighbour areas still streaming" : "");
  sRoomPending = false;
  if (sRoomMusic) {
    sRoomMusic = false;
    if (sRoomStats.music > 0)
      PortEndStateLoadMusic();
    else
      PortStartStateLoadMusic(mgr);
  }
  if (late) {
    // The armed spawn layers stay: those areas spawn with the bits they had at the save.
    sRoomLate = true;
    sRoomFrames = 0;
  } else {
    EndRoomState();
  }
}


fs::path ResumePath() {
  const std::string folder = Folder();
  if (folder.empty())
    return {};
  return PortGci::PathFromString(folder) / "resume.mpss";
}

// What ties a resume to one game: the randomizer seed, or the Archipelago
// session (its save card directory and built-in seed). Retail is "".
std::string CurrentIdentity() {
  std::string id;
  if (PortAp::Enabled()) {
    id += "ap:" + PortAp::SaveCardDirectory() + "|" + PortAp::SoloSeedInPlay();
  }
  const std::string shared = PortGci::CardTargetDirectory("");
  if (!shared.empty()) {
    id += "card:" + shared;
  }
  if (PortRandomizer::Enabled()) {
    const char* seed = PortRandomizer::SeedName();
    id += std::string("rando:") + (seed != nullptr ? seed : "");
  }
  return id;
}

void RemoveResumeFile() {
  const fs::path path = ResumePath();
  if (path.empty())
    return;
  std::error_code ec;
  fs::remove(path, ec);
}

fs::path MarkerPath() {
  const std::string folder = Folder();
  if (folder.empty())
    return {};
  return PortGci::PathFromString(folder) / "running";
}

void WriteMarker() {
  const fs::path path = MarkerPath();
  if (path.empty())
    return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << sMarker;
  file.flush();
  file.close();
  if (!file) {
    // Without the marker a crash goes undetected, so the user is told once per
    // failure.
    PortLog::Write("[savestate] cannot write the crash marker %s\n", PortGci::PathString(path).c_str());
  }
}

void RemoveMarker() {
  const fs::path path = MarkerPath();
  if (path.empty())
    return;
  std::error_code ec;
  fs::remove(path, ec);
}

std::string ResumedMarker(int64_t savedAt) { return "resumed " + std::to_string(savedAt); }

bool ReadResume(ResumeMeta& meta, Header& header, std::vector< uint8_t >& blob) {
  const fs::path path = ResumePath();
  if (path.empty())
    return false;
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  const std::string data = port::ReadAllText(file);
  std::string state;
  return DecodeResume(data, meta, state) && Decode(state, header, blob) && Loadable(header, blob);
}

ResumeContext GatherResumeContext() {
  ResumeContext ctx;
  const CStateManager* mgr = PortDebug::StateManager();
  if (mgr == nullptr || mgr->GetPlayer() == nullptr || mgr->GetWorld() == nullptr ||
      gpGameState == nullptr) {
    return ctx;
  }
  ctx.inGame = true;
  ctx.initialized = mgr->IsFullyInitialized();
  ctx.quitting = mgr->GetWantsToQuit() || sInstallPending || sPlacePending;
  const CPlayer* player = mgr->GetPlayer();
  ctx.dead = player->IsPlayerDeadEnough() || player->GetDeathTime() > 0.f ||
             (mgr->GetPlayerState() != nullptr && mgr->GetPlayerState()->GetHealthInfo().GetHP() <= 0.f);
  ctx.cinematic = mgr->GetCameraManager() != nullptr && mgr->GetCameraManager()->IsInCinematicCamera();
  ctx.transition = mgr->GetWorldTransManager() != nullptr &&
                   mgr->GetWorldTransManager()->GetTransType() != CWorldTransManager::kTT_Disabled;
  // Not CStateManager::GetInSaveUI: that flag goes true at the first pause or message screen and only
  // a save station clears it (ThinkSaveStation reads it as "the save UI was exited").
  const CInGameGuiManager* gui = CInGameGuiManager::PortCurrent();
  ctx.saveUi = gui != nullptr && gui->IsInOrTransitioningToOrFromState(kIGGS_PauseSaveGame);
  return ctx;
}

// The resume file survives a load that could not start.
void DoResumeLoad(CStateManager& mgr) {
  ResumeMeta meta;
  Header header;
  std::vector< uint8_t > blob;
  if (!ReadResume(meta, header, blob)) {
    SetMessage("No resume state to load");
    return;
  }
  ResumeMeta current;
  current.cardSerial = gpGameState != nullptr ? gpGameState->GetCardSerial() : 0;
  current.identity = CurrentIdentity();
  if (!ResumeMatches(meta, current) || (gpGameState != nullptr && meta.fileIdx != gpGameState->GetFileIdx())) {
    SetMessage("The resume state belongs to another game");
    return;
  }
  std::string why;
  if (!PlayerCanSave(mgr, why)) {
    SetMessage("Can't load the resume state: " + why);
    return;
  }
  sPendingHeader = header;
  sPendingBlob = std::move(blob);
  sResumeLoading = true;
  QuitToPending(mgr);
  SetMessage("Loading the resume state: " + header.world + " - " + header.room);
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

bool RoomRestoreOwnsArea(CStateManager& mgr, int area) {
  if (!(sPlacePending || sRoomPending || sRoomLate) || !PortDebug::SaveStateRoomState() ||
      mgr.GetWorld() == nullptr ||
      static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId()) != sPendingHeader.worldId)
    return false;
  return RoomAreaAwaitsRestore(mgr, sPendingHeader.roomState, sRoomDone, area);
}

bool Tick(CStateManager& mgr) {
  if (sPlacePending && !mgr.GetWantsToQuit() && mgr.GetWorld() != nullptr &&
      mgr.GetGameState() == CStateManager::kGS_Running) {
    sPlacePending = false;
    if (static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId()) == sPendingHeader.worldId) {
      Place(mgr);
    } else {
      DisarmSpawnLayers();
    }
    if (sResumeLoading) {
      // The file stays until the next snapshot replaces it: a crash before
      // then resumes here again, and the marker tells the prompt so.
      sResumeLoading = false;
      sMarker = ResumedMarker(sPendingHeader.savedAt);
      WriteMarker();
      SetMessage("Resumed");
    }
    sRoomMusic = sRoomPending && RoomStateHasMusic(sPendingHeader.roomState);
    if (!sRoomMusic)
      PortStartStateLoadMusic(mgr);
  }
  TickRoomState(mgr);
  if (sInstallPending) {
    // Still in the outgoing game, whose PAKs are retired: run nothing more
    // until CMainFlow replaces it.
    return mgr.GetWantsToQuit();
  }
  ResumeTick(mgr);
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
  if (sResumeLoadRequest) {
    sResumeLoadRequest = false;
    DoResumeLoad(mgr);
    return sInstallPending;
  }
  if (sReloadRequest) {
    sReloadRequest = false;
    return DoModReload(mgr);
  }
  return false;
}

void ReloadModsNow() {
  sReloadMods = false;
  // In game the world that held them is already gone (CStateManager's destructor); a reload made
  // in the front end has no world, so this is the only place its holders can go. Their tokens
  // would otherwise pin the outgoing mods' models in gpSimplePool.
  PortWorldHolders::ReloadMods(&PortWorldHolders::Release,
                               [] { gpResourceFactory->PortReopenPaks(ReloadModFiles); });
  const PortMods::Status& status = PortMods::CurrentStatus();
  SetMessage("Mods reloaded: " + std::to_string(status.overlays) + " disc file(s), " +
             std::to_string(PortMods::NativeTextureCount()) + " native texture(s)");
}

void SetInFrontEnd(bool inFrontEnd) {
  sInFrontEnd = inFrontEnd;
  if (inFrontEnd) {
    // Back at the title: the snapshot, a room restore still running and the layers
    // armed for it belong to a game that was left.
    sRoomPending = false;
    sRoomLate = false;
    if (sRoomMusic) {
      // The restore that was to play the music never finished: the next world
      // destroyed must not keep the track as a state load would.
      sRoomMusic = false;
      PortEndStateLoadMusic();
    }
    DisarmSpawnLayers();
    std::lock_guard< std::mutex > lock(sSnapMutex);
    sSnap.valid = false;
    sSnap.bytes.clear();
    sSnap.gen++;
  }
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
  // A state from before the beam door record keeps the running game's open
  // doorways (the usual case is loading a state of the same save) rather than
  // dropping them; the rebuilt game state would have none.
  uint32_t keptBeam[PortBeamDoors::kWords] = {};
  if (gpGameState != nullptr) {
    for (int i = 0; i < PortBeamDoors::kWords; ++i) {
      keptBeam[i] = gpGameState->PortBeamDoorBits()[i];
    }
  }
  gpMain->PortLoadGameState(in);
  if (!LoadBeamData(sPendingBlob) && gpGameState != nullptr) {
    for (int i = 0; i < PortBeamDoors::kWords; ++i) {
      gpGameState->PortBeamDoorBits()[i] = keptBeam[i];
    }
  }
  if (gpGameState != nullptr) {
    gpGameState->PortBackUpBeamDoors();
  }
  LoadMapData(sPendingBlob);
  // Rebuild the areas with the layers their objects were spawned with (#161); each area puts
  // the saved live bits back as its objects are spawned.
  if (PortDebug::SaveStateRoomState())
    ArmSpawnLayers(sPendingHeader.roomState, sPendingHeader.worldId);
  sPlacePending = true;
}

namespace {

// Logs why quick resume is (not) taking snapshots, once per change of reason.
void ResumeNote(const std::string& reason) {
  static std::string sLast;
  if (reason == sLast)
    return;
  sLast = reason;
  PortLog::Write("[resume] snapshots: %s\n", reason.c_str());
}

// The reason a snapshot can't be taken now, or empty when it can.
std::string UnsafeReason(CStateManager& mgr) {
  const ResumeContext ctx = GatherResumeContext();
  if (const char* skip = ResumeSkipReason(ctx))
    return skip;
  std::string why;
  if (!PlayerCanSave(mgr, why))
    return why;
  return {};
}

bool SafeNow(CStateManager& mgr) { return UnsafeReason(mgr).empty(); }

uint64_t RoomKey(const CStateManager& mgr) {
  return (static_cast< uint64_t >(static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId())) << 32) |
         static_cast< uint32_t >(mgr.GetWorld()->GetCurrentAreaId().Value());
}

ResumeMeta CurrentMeta() {
  ResumeMeta meta;
  meta.fileIdx = gpGameState->GetFileIdx();
  meta.cardSerial = gpGameState->GetCardSerial();
  meta.identity = CurrentIdentity();
  return meta;
}

// Builds the snapshot from the live game (caller checked SafeNow).
bool TakeSnapshot(CStateManager& mgr) {
  const auto start = Clock::now();
  ResumeSnapshot snap;
  snap.meta = CurrentMeta();
  if (snap.meta.fileIdx > 2) {
    ResumeNote("blocked: no card file selected (file " + std::to_string(snap.meta.fileIdx) + ")");
    return false;
  }
  // No card serial: a save state loaded from the title, or "continue without
  // saving". No card file can take such a state at the title, so it isn't kept.
  if (snap.meta.cardSerial == 0) {
    ResumeNote("blocked: the game has no memory card");
    return false;
  }
  Header header;
  std::vector< uint8_t > blob;
  Capture(mgr, header, blob);
  snap.path = ResumePath();
  if (snap.path.empty()) {
    ResumeNote("blocked: no resume path (no user folder)");
    return false;
  }
  snap.bytes = EncodeResume(snap.meta, Encode(header, blob));
  snap.world = header.world;
  snap.room = header.room;
  snap.worldId = header.worldId;
  snap.areaId = static_cast< uint32_t >(mgr.GetWorld()->GetCurrentAreaId().Value());
  snap.taken = Clock::now();
  snap.valid = true;
  const double ms = std::chrono::duration< double, std::milli >(snap.taken - start).count();
  PortLog::Write("[resume] snapshot %s - %s: %zu bytes in %.3f ms\n", snap.world.c_str(),
                 snap.room.c_str(), snap.bytes.size(), ms);
  {
    std::lock_guard< std::mutex > lock(sSnapMutex);
    snap.gen = sSnap.gen + 1;
    snap.writtenGen = sSnap.writtenGen;
    sSnap = std::move(snap);
  }
  return true;
}

void DropSnapshot() {
  std::lock_guard< std::mutex > lock(sSnapMutex);
  sSnap.valid = false;
  sSnap.bytes.clear();
  sSnap.gen++;
}

bool HaveSnapshot() {
  std::lock_guard< std::mutex > lock(sSnapMutex);
  return sSnap.valid;
}

// SDL calls this on whichever thread sends the event. Android's game thread
// pumps the lifecycle in SDL_PollEvent (SDL_androidevents.c Android_OnPause),
// so this is the game thread, but it takes nothing from the game unless the
// main loop marked the pump safe.
bool SDLCALL ResumeEventWatch(void*, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
    if (sResumeEnabled.load() && sPumpSafe.load() && std::this_thread::get_id() == sGameThread) {
      CStateManager* mgr = const_cast< CStateManager* >(PortDebug::StateManager());
      if (mgr != nullptr && mgr->GetPlayer() != nullptr && mgr->GetWorld() != nullptr &&
          gpGameState != nullptr && SafeNow(*mgr)) {
        TakeSnapshot(*mgr);
      }
    }
    [[fallthrough]];
  case SDL_EVENT_DID_ENTER_BACKGROUND:
  case SDL_EVENT_TERMINATING:
    if (sResumeEnabled.load()) {
      ResumeFlush();
    }
    // Android may kill a background app at any time: that is no crash.
    RemoveMarker();
    break;
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    WriteMarker();
    break;
  default:
    break;
  }
  return true;
}

} // namespace

namespace {

bool FlushSnapshot(bool quiet) {
  std::lock_guard< std::mutex > lock(sSnapMutex);
  if (!sSnap.valid || sSnap.writtenGen == sSnap.gen)
    return false;
  fs::path temp = sSnap.path;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file)
      return false;
    file.write(sSnap.bytes.data(), static_cast< std::streamsize >(sSnap.bytes.size()));
    file.flush();
    if (!file)
      return false;
  }
  std::error_code ec;
  fs::rename(temp, sSnap.path, ec);
  if (ec)
    return false;
  sSnap.writtenGen = sSnap.gen;
  if (!quiet) {
    const double age = std::chrono::duration< double >(Clock::now() - sSnap.taken).count();
    PortLog::Write("[resume] written: %s - %s (snapshot %.1f s old)\n", sSnap.world.c_str(),
                   sSnap.room.c_str(), age);
  }
  return true;
}

} // namespace

bool ResumeFlush() { return FlushSnapshot(false); }

void ResumeInstallEventWatch() {
  sGameThread = std::this_thread::get_id();
  SDL_AddEventWatch(ResumeEventWatch, nullptr);
  const fs::path marker = MarkerPath();
  std::error_code ec;
  if (!marker.empty() && fs::exists(marker, ec)) {
    sLastRunCrashed = true;
    std::ifstream file(marker, std::ios::binary);
    std::getline(file, sLastRunMarker);
    PortLog::Write("[resume] the last run did not quit cleanly%s\n",
                   sLastRunMarker.empty() ? "" : (" (" + sLastRunMarker + ")").c_str());
  }
  WriteMarker();
  // Returning from main or exit() is a clean quit too; a crash or a kill is not.
  std::atexit(ResumeQuitCleanly);
}

void ResumeQuitCleanly() { RemoveMarker(); }

void SetEventPumpSafe(bool safe) { sPumpSafe.store(safe); }

void ResumeTick(CStateManager& mgr) {
  const bool enabled = PortDebug::QuickResume();
  sResumeEnabled.store(enabled);
  if (!enabled) {
    if (HaveSnapshot())
      DropSnapshot();
    return;
  }
  if (sInFrontEnd || mgr.GetPlayer() == nullptr || mgr.GetWorld() == nullptr || gpGameState == nullptr) {
    ResumeNote(sInFrontEnd ? "paused: in the front end" : "paused: no player or world");
    return;
  }
  const auto now = Clock::now();
  const uint64_t key = RoomKey(mgr);
  if (key != sRoomKey) {
    sRoomKey = key;
    sRoomSince = now;
  }
  bool haveSnap;
  bool sameRoom = false;
  double sinceSnap = 0.0;
  {
    std::lock_guard< std::mutex > lock(sSnapMutex);
    haveSnap = sSnap.valid;
    if (haveSnap) {
      // Another card file or game since: that snapshot is no longer ours.
      const ResumeMeta meta = CurrentMeta();
      if (!(sSnap.meta.fileIdx == meta.fileIdx && ResumeMatches(sSnap.meta, meta))) {
        sSnap.valid = false;
        sSnap.gen++;
        haveSnap = false;
        PortLog::Write("[resume] snapshot dropped: another card file or game (file %u -> %u)\n",
                       static_cast< unsigned >(sSnap.meta.fileIdx), static_cast< unsigned >(meta.fileIdx));
      } else {
        sameRoom = ((static_cast< uint64_t >(sSnap.worldId) << 32) | sSnap.areaId) == key;
        sinceSnap = std::chrono::duration< double >(now - sSnap.taken).count();
      }
    }
  }
  const std::string unsafe = UnsafeReason(mgr);
  const bool safe = unsafe.empty();
  if (!safe)
    ResumeNote("blocked: " + unsafe);
  else
    ResumeNote("safe");
  if (ResumeRefreshDue(safe, haveSnap, sameRoom, std::chrono::duration< double >(now - sRoomSince).count(),
                       sinceSnap)) {
    // Written at once (a few KB): a crash leaves this snapshot to resume from.
    if (TakeSnapshot(mgr)) {
      if (!FlushSnapshot(true))
        ResumeNote("blocked: writing the snapshot failed");
    }
  }
}

bool ResumeSave(bool force) {
  if (!force && !PortDebug::QuickResume())
    return false;
  CStateManager* mgr = const_cast< CStateManager* >(PortDebug::StateManager());
  const bool inGame = mgr != nullptr && mgr->GetPlayer() != nullptr && mgr->GetWorld() != nullptr &&
                      gpGameState != nullptr && !sInFrontEnd;
  const bool safe = inGame && SafeNow(*mgr);
  switch (ChooseResumeWrite(safe, HaveSnapshot())) {
  case ResumeWriteChoice::kNothing:
    SetMessage("No resume state written: no safe point yet");
    return false;
  case ResumeWriteChoice::kRefreshThenWrite:
    if (!TakeSnapshot(*mgr)) {
      SetMessage("Couldn't write the resume state in " + Folder());
      return false;
    }
    break;
  case ResumeWriteChoice::kWriteSnapshot:
    break;
  }
  if (force) {
    std::lock_guard< std::mutex > lock(sSnapMutex);
    // A forced save writes again even when the snapshot was already written.
    sSnap.writtenGen = 0;
  }
  if (!ResumeFlush()) {
    std::lock_guard< std::mutex > lock(sSnapMutex);
    if (sSnap.valid && sSnap.writtenGen == sSnap.gen) {
      return true; // an event watch already wrote this snapshot
    }
    SetMessage("Couldn't write the resume state in " + Folder());
    return false;
  }
  std::lock_guard< std::mutex > lock(sSnapMutex);
  SetMessage(std::string("Resume state written") + (safe ? "" : " from the last safe point") + ": " +
             sSnap.world + " - " + sSnap.room);
  return true;
}

bool ResumeOffer(ResumeInfo& info) {
  if (!PortDebug::QuickResume())
    return false;
  ResumeMeta meta;
  Header header;
  std::vector< uint8_t > blob;
  // Polled while the title waits: say why once, not every poll.
  static std::string sLastReason;
  const auto note = [](const std::string& reason) {
    if (reason != sLastReason) {
      sLastReason = reason;
      PortLog::Write("[resume] %s\n", reason.c_str());
    }
  };
  if (!ReadResume(meta, header, blob)) {
    std::error_code ec;
    note(fs::exists(ResumePath(), ec) ? "no offer: " + ResumePath().string() + " is unreadable"
                                      : "no offer: no resume state");
    return false;
  }
  ResumeMeta current = meta;
  current.identity = CurrentIdentity();
  if (!ResumeMatches(meta, current)) {
    note("no offer yet: saved for '" + meta.identity + "', running '" + current.identity + "'");
    return false;
  }
  note("offering " + header.world + " - " + header.room);
  info.exists = true;
  info.fileIdx = meta.fileIdx;
  info.cardSerial = meta.cardSerial;
  info.world = header.world;
  info.room = header.room;
  info.playTime = header.playTime;
  info.savedAt = header.savedAt;
  info.afterCrash = sLastRunCrashed;
  info.crashedAgain = sLastRunCrashed && sLastRunMarker == ResumedMarker(header.savedAt);
  return true;
}

bool ResumeAccept(uint64_t cardSerial, uint32_t& fileIdx) {
  ResumeMeta meta;
  Header header;
  std::vector< uint8_t > blob;
  if (!ReadResume(meta, header, blob))
    return false;
  ResumeMeta current;
  current.cardSerial = cardSerial;
  current.identity = CurrentIdentity();
  if (!ResumeMatches(meta, current)) {
    PortLog::Write("[resume] mismatch: card %016llx/%016llx identity '%s'/'%s' file %u\n",
                   static_cast< unsigned long long >(meta.cardSerial), static_cast< unsigned long long >(current.cardSerial),
                   meta.identity.c_str(), current.identity.c_str(), meta.fileIdx);
    SetMessage("The resume state belongs to another game");
    return false;
  }
  sPendingHeader = header;
  sPendingBlob = std::move(blob);
  sInstallPending = true;
  sPlacePending = false;
  sResumeLoading = true;
  fileIdx = meta.fileIdx;
  SetMessage("Resuming: " + header.world + " - " + header.room);
  return true;
}

bool RequestResumeLoad() {
  if (PortDebug::StateManager() == nullptr) {
    SetMessage("Can't load the resume state: no game running");
    return false;
  }
  if (sInstallPending || sPlacePending) {
    SetMessage("A load is already in progress");
    return false;
  }
  sResumeLoadRequest = true;
  return true;
}

void ResumeDiscard() {
  DropSnapshot();
  RemoveResumeFile();
}

void ResumeClear() {
  ResumeDiscard();
  SetMessage("Resume state deleted");
}

std::string ResumeStatus() {
  ResumeMeta meta;
  Header header;
  std::vector< uint8_t > blob;
  if (!ReadResume(meta, header, blob))
    return "no resume state";
  ResumeMeta current = meta;
  current.identity = CurrentIdentity();
  char when[32] = "";
  const std::time_t at = static_cast< std::time_t >(header.savedAt);
  if (const std::tm* tm = std::localtime(&at))
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
  return header.world + " - " + header.room + ", file " + std::to_string(meta.fileIdx + 1) + ", " +
         when + (ResumeMatches(meta, current) ? "" : " (another game's)");
}

namespace {
std::mutex sPromptMutex;
bool sPromptShown = false;
bool sPromptYes = true;
int sPromptAnswer = -1;
ResumeInfo sPromptInfo;
} // namespace

void SetResumePrompt(const ResumeInfo* info, bool yesSelected) {
  std::lock_guard< std::mutex > lock(sPromptMutex);
  sPromptShown = info != nullptr;
  if (info != nullptr) {
    sPromptInfo = *info;
    sPromptYes = yesSelected;
  } else {
    sPromptAnswer = -1;
  }
}

bool ResumePromptShown(ResumeInfo& out, bool& yesSelected) {
  std::lock_guard< std::mutex > lock(sPromptMutex);
  if (!sPromptShown)
    return false;
  out = sPromptInfo;
  yesSelected = sPromptYes;
  return true;
}

void AnswerResumePrompt(bool yes) {
  std::lock_guard< std::mutex > lock(sPromptMutex);
  if (sPromptShown && sPromptAnswer < 0)
    sPromptAnswer = yes ? 1 : 0;
}

int TakeResumePromptAnswer() {
  std::lock_guard< std::mutex > lock(sPromptMutex);
  const int answer = sPromptAnswer;
  sPromptAnswer = -1;
  return answer;
}

} // namespace PortSaveState
