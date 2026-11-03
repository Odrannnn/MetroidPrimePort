#include "port_apclient_internal.h"
#include "port_beam_doors.h"

#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

namespace PortAp {
namespace ClientDetail {

rstl::wstring ToHudWide(const std::string& text) {
  std::wstring wide;
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    uint32_t codepoint = 0xfffd;
    size_t length = 1;
    if (first < 0x80) {
      codepoint = first;
    } else {
      size_t expected = 0;
      uint32_t minimum = 0;
      if (first >= 0xc2 && first <= 0xdf) {
        expected = 2;
        minimum = 0x80;
        codepoint = first & 0x1f;
      } else if (first >= 0xe0 && first <= 0xef) {
        expected = 3;
        minimum = 0x800;
        codepoint = first & 0x0f;
      } else if (first >= 0xf0 && first <= 0xf4) {
        expected = 4;
        minimum = 0x10000;
        codepoint = first & 0x07;
      }
      bool valid = expected != 0 && i + expected <= text.size();
      for (size_t offset = 1; valid && offset < expected; ++offset) {
        const uint8_t continuation = static_cast<uint8_t>(text[i + offset]);
        if ((continuation & 0xc0) != 0x80) {
          valid = false;
          break;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
      valid = valid && codepoint >= minimum && codepoint <= 0x10ffff &&
              !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      if (valid) {
        length = expected;
      } else {
        codepoint = 0xfffd;
      }
    }
    i += length;
    if constexpr (sizeof(wchar_t) >= 4) {
      wide.push_back(static_cast<wchar_t>(codepoint));
    } else if (codepoint <= 0xffff) {
      wide.push_back(static_cast<wchar_t>(codepoint));
    } else {
      codepoint -= 0x10000;
      wide.push_back(static_cast<wchar_t>(0xd800 + (codepoint >> 10)));
      wide.push_back(static_cast<wchar_t>(0xdc00 + (codepoint & 0x3ff)));
    }
  }
  return rstl::wstring(wide.c_str());
}


Runtime& GetRuntime() {
  // Leaked on purpose, so a worker Shutdown() leaves behind never touches
  // freed memory; the stopper runs Shutdown() with the other static
  // destructors.
  static Runtime* runtime = new Runtime;
  static struct Stopper {
    Runtime* runtime;
    ~Stopper() { runtime->Shutdown(); }
  } stopper{runtime};
  return *runtime;
}

// The session's layout, shared. `mutex` is held and the session exists.
std::shared_ptr<const PortApWorld::Layout> SessionWorldLocked(Runtime& runtime) {
  const uint64_t revision = runtime.session->WorldRevision();
  if (runtime.worldCache == nullptr || runtime.worldCacheRevision != revision) {
    runtime.worldCache =
        std::make_shared<const PortApWorld::Layout>(runtime.session->GetState().world);
    runtime.worldCacheRevision = revision;
  }
  return runtime.worldCache;
}

// Names the session a save's received items came from: FNV-1a over the seed
// and slot (and the room suffix of a seed's second room), never 0, which marks
// a game no session has given items to.
uint32_t SessionIdentity(const std::string& seed, const std::string& slot,
                         const std::string& roomSuffix) {
  uint32_t hash = port::kFnv32Basis;
  const auto mix = [&hash](const std::string& text) { hash = port::Fnv1a32(text, hash); };
  mix(seed);
  mix(std::string(1, '\0'));
  mix(slot);
  if (!roomSuffix.empty()) {
    mix(std::string(1, '\0'));
    mix(roomSuffix);
  }
  return hash != 0 ? hash : 1;
}

// A game some session has given items to. Its pickups at the built-in
// locations stay the multiworld's while the client is off.
bool IsApGame(const CGameState::ApProgress& progress) {
  return progress.recorded && progress.identity != 0;
}

// A built-in location's bit in the save's checked record, or -1.
int CheckedBit(const MetroidPrime::Location* location) {
  size_t count = 0;
  const MetroidPrime::Location* first = MetroidPrime::Locations(count);
  if (location == nullptr)
    return -1;
  const ptrdiff_t index = location - first;
  return index >= 0 && static_cast<size_t>(index) < count && index < 128 ? static_cast<int>(index)
                                                                          : -1;
}

void ClearChecked(CGameState::ApProgress& progress) {
  for (uint& word : progress.checked)
    word = 0;
  for (uint& word : progress.shields)
    word = 0;
}

// Hands the session the checks the game records but the session has not:
// ones made while the client was off, which go out on the next connection.
void QueueRecordedChecksLocked(Runtime& runtime, const CGameState::ApProgress& progress) {
  size_t count = 0;
  const MetroidPrime::Location* locations = MetroidPrime::Locations(count);
  int queued = 0;
  for (size_t i = 0; i < count && i < 128; ++i) {
    if (((progress.checked[i / 32] >> (i % 32)) & 1) == 0)
      continue;
    char key[32];
    PortRandomizer::FormatLocationKey(locations[i].world, locations[i].area, locations[i].pickup,
                                      key, sizeof(key));
    int64_t id = 0;
    if (runtime.session->MarkLocationChecked(key, id)) {
      runtime.queuedChecks.push_back(id);
      ++queued;
    }
  }
  if (queued == 0)
    return;
  runtime.MarkStateDirtyLocked();
  PortLog::Write("archipelago: the loaded game holds %d checks the server has not had; "
                 "sending them\n",
                 queued);
}

// Lines the session up with the loaded game. The state file's item index says
// what this client has received, but not what the game holds: quitting without
// saving, or loading an older save, drops items the index has moved past. So
// the save records how many it holds, and on load the session rewinds to that
// many and has the server replay the rest. The other way round, the save
// records the checks it made, some of which the session may not have seen.
// True when the running game is another seed's or room's than the session's
// (the server changed under it): the caller sends it back to the title.
bool ReconcileLocked(Runtime& runtime, CGameState::ApProgress& progress) {
  const Protocol::State& state = runtime.session->GetState();
  if (state.seed.empty())
    return false; // no session yet to compare with; nothing can have been granted
  const uint32_t identity = SessionIdentity(state.seed, runtime.config.slot, runtime.roomSuffix);
  if (progress.reconciled) {
    // The game holds the old seed's or room's items, which the new one's
    // checks and receipts can't be applied on top of.
    if (progress.identity != identity)
      return true;
    if (!runtime.recordSynced)
      QueueRecordedChecksLocked(runtime, progress); // reconnected mid-game
    runtime.recordSynced = true;
    return false;
  }
  progress.reconciled = true;
  runtime.recordSynced = true;
  // The record a loaded game (a save, a resume or a save state) brings, before
  // it is lined up: what the decision below starts from.
  PortLog::Write("archipelago: lining up a loaded game: it records %s, %u item(s) applied; "
                 "the session has received %lld, %zu waiting for the game\n",
                 !progress.recorded              ? "nothing (a save from before the record)"
                 : progress.identity == 0        ? "a new game"
                 : progress.identity == identity ? "this seed and slot"
                                                 : "another seed or slot",
                 progress.appliedIndex, static_cast<long long>(state.nextItemIndex), runtime.grants.size());
  // Checks made in another seed or slot are not this one's. A new game's
  // (identity 0) were made under this session before its seed was known.
  if (progress.identity != 0 && progress.identity != identity) {
    PortLog::Write("archipelago: the loaded game's checks were made in another seed or slot; "
                   "they are not sent\n");
    ClearChecked(progress);
  }
  else
    QueueRecordedChecksLocked(runtime, progress);
  // Where the game's items end on the session's side: grants still queued
  // for the game are about to be applied, so they count as not held yet.
  const int64_t firstPending =
      runtime.grants.empty() ? state.nextItemIndex : runtime.grants.front().index;
  if (!progress.recorded) {
    // A save from before the record existed: take it to hold what the state
    // file says was received, which is what the client assumed until now.
    progress.recorded = true;
    progress.identity = identity;
    progress.appliedIndex = static_cast<uint>(
        std::clamp<int64_t>(firstPending, 0, std::numeric_limits<uint32_t>::max()));
    PortLog::Write("archipelago: taking the loaded game to hold the first %lld item(s)\n",
                   static_cast<long long>(firstPending));
    return false;
  }
  // A new game, or a save from another seed or slot, holds none of these items.
  const int64_t held = progress.identity == identity ? progress.appliedIndex : 0;
  progress.identity = identity;
  progress.appliedIndex = static_cast<uint>(held);
  if (held == firstPending) {
    PortLog::Write("archipelago: the loaded game is in step: it holds the first %lld item(s)\n",
                   static_cast<long long>(held));
    return false;
  }
  PortLog::Write("archipelago: the loaded game holds %lld of %lld received items; "
                 "asking the server for the rest\n",
                 static_cast<long long>(held), static_cast<long long>(state.nextItemIndex));
  runtime.session->RewindTo(held);
  runtime.DrainSessionLogLocked();
  runtime.MarkStateDirtyLocked();
  runtime.grants.clear();
  runtime.itemCount = static_cast<int>(std::min<int64_t>(held, std::numeric_limits<int>::max()));
  runtime.syncWanted = true;
  return false;
}

// With the built-in tables the game is a plain disc, so what the AP ISO patches
// in is done here each tick instead: unlimited ammo, the Artifact Temple totems
// following the artifacts held (the retail pickup scripts light the totem of the
// artifact that used to be at a location, not the one received), and the goal.
void ApplyBuiltinWorld(Runtime& runtime, CStateManager& mgr, CPlayerState& player) {
  namespace Prime = MetroidPrime;
  bool unlimitedMissiles = false;
  bool unlimitedPowerBombs = false;
  // Capacity wanted for missiles and power bombs, or -1 while not known.
  int ammoTarget[2] = {-1, -1};
  // The disc's values outside a seed's game.
  static const PortApWorld::Layout kNoLayout;
  std::shared_ptr<const PortApWorld::Layout> seedLayout;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    const bool builtin = runtime.config.builtin && runtime.session != nullptr;
    if (builtin && runtime.session->GetState().hasWorld)
      seedLayout = SessionWorldLocked(runtime);
    // The world's item limits (its Config.py; not in slot data) hold in a
    // seed's game, the disc's anywhere else.
    const bool seedGame = builtin && runtime.session->GetState().hasWorld;
    CPlayerState::PortSetAmmoLimits(seedGame ? 999 : 250, seedGame ? 99 : 8);
    // The seed's tank capacity; a game outside Archipelago has the disc's.
    const float capacity =
        static_cast<float>((seedLayout != nullptr ? *seedLayout : kNoLayout).etankCapacity);
    if (CPlayerState::GetEnergyTankCapacity() != capacity) {
      // Full health stays full under the new capacity.
      const bool full = player.GetHealthInfo().GetHP() >= player.CalculateHealth();
      CPlayerState::PortSetEnergyTankCapacity(capacity);
      if (full)
        player.HealthInfo()->SetHP(player.CalculateHealth());
    }
    if (!builtin)
      return;
    unlimitedMissiles =
        runtime.session->ReceivedCount(Prime::kItemBase + Prime::kUnlimitedMissiles) > 0;
    unlimitedPowerBombs =
        runtime.session->ReceivedCount(Prime::kItemBase + Prime::kUnlimitedPowerBombs) > 0;
    // The capacity the received items add up to, once the whole inventory is in
    // and nothing is queued for the game. Games that were played against the
    // official AP world before its item ids were translated never got the
    // Missile Launcher, and the save's applied index is already past it.
    if (seedGame && runtime.session->InventoryReceived() && runtime.grants.empty()) {
      const std::map<int64_t, int64_t>& counts = runtime.session->GetState().progressive;
      const Protocol::SlotData& options = runtime.session->GetSlotData();
      ammoTarget[0] = Prime::AmmoCapacity(counts, true, options.requireMissileLauncher);
      ammoTarget[1] = Prime::AmmoCapacity(counts, false, options.requireMainPowerBomb);
    }
    if (!runtime.goalWanted && gpGameState->CurrentWorldAssetId() == Prime::kEndOfGameWorld) {
      runtime.goalWanted = true;
      runtime.wake.notify_all();
    }
  }
  const auto topOff = [&player](CPlayerState::EItemType type) {
    const int missing = player.GetItemCapacity(type) - player.GetItemAmount(type);
    if (missing > 0)
      player.IncrPickUp(type, missing);
  };
  // Top-up only: a capacity above the received items' is never lowered.
  const auto topUpCapacity = [&player](CPlayerState::EItemType type, int target, int limit) {
    target = std::min(target, limit);
    const int capacity = player.GetItemCapacity(type);
    if (target <= capacity)
      return;
    const int amountBefore = player.GetItemAmount(type);
    player.InitializePowerUp(type, target - capacity);
    player.IncrPickUp(type, target - capacity);
    PortLog::Write("archipelago: raised %s capacity %d -> %d (amount %d -> %d) to match the received items\n",
                   PortRandomizer::ItemName(static_cast<int>(type)), capacity, player.GetItemCapacity(type), amountBefore,
                   player.GetItemAmount(type));
  };
  if (ammoTarget[0] >= 0)
    topUpCapacity(CPlayerState::kIT_Missiles, ammoTarget[0], 999);
  if (ammoTarget[1] >= 0)
    topUpCapacity(CPlayerState::kIT_PowerBombs, ammoTarget[1], 99);
  if (unlimitedMissiles)
    topOff(CPlayerState::kIT_Missiles);
  if (unlimitedPowerBombs)
    topOff(CPlayerState::kIT_PowerBombs);

  // Tallon's layer state is shared with the running world while in Tallon, so
  // writing it through the game state covers both cases.
  CScriptLayerManager* layers =
      gpGameState->StateForWorld(Prime::kTallonWorld).GetLayerState().GetPtr();
  TAreaId temple = Prime::kArtifactTempleIndex;
  const CWorld* world = mgr.GetWorld();
  if (world != nullptr && world->IGetWorldAssetId() == Prime::kTallonWorld)
    temple = world->IGetAreaId(Prime::kArtifactTempleArea);
  // The layers the seed keeps on or off.
  const PortApWorld::Layout& layout = seedLayout != nullptr ? *seedLayout : kNoLayout;
  for (const PortApWorld::LayerChange& change : PortApWorld::Layers(layout)) {
    CScriptLayerManager* state = gpGameState->StateForWorld(change.mlvl).GetLayerState().GetPtr();
    TAreaId area(change.area);
    if (world != nullptr && world->IGetWorldAssetId() == change.mlvl)
      area = world->IGetAreaId(change.mrea);
    if (state == nullptr || area.Value() < 0 ||
        static_cast<size_t>(area.Value()) >= static_cast<size_t>(state->GetAreaLayers().size()))
      continue;
    if (change.whileLayer >= 0 && !state->IsLayerActive(area, TLayerId(change.whileLayer)))
      continue;
    if (state->IsLayerActive(area, TLayerId(change.layer)) != change.active)
      state->SetLayerActive(area, TLayerId(change.layer), change.active);
  }
  if (layers == nullptr || temple.Value() < 0 ||
      static_cast<size_t>(temple.Value()) >= static_cast<size_t>(layers->GetAreaLayers().size()))
    return;
  for (int id = CPlayerState::kIT_Truth; id <= CPlayerState::kIT_Newborn; ++id) {
    const bool held = player.GetItemAmount(static_cast<CPlayerState::EItemType>(id)) > 0;
    // Truth's totem is the first thing in the room, so it has its own layer.
    const TLayerId layer(id == CPlayerState::kIT_Truth ? 23 : id - 28);
    if (layers->IsLayerActive(temple, layer) != held)
      layers->SetLayerActive(temple, layer, held);
  }
}

void CountChecks(Runtime& runtime, size_t count) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                       runtime.checkCount));
  runtime.checkCount += static_cast<int>(std::min(count, room));
}


// Before games had directories, one archipelago_state.json sat beside the
// configuration. It moves into its game's directory, if it names one.
void MigrateLegacyState(const std::filesystem::path& parent) {
  const std::filesystem::path legacy = parent / "archipelago_state.json";
  std::error_code error;
  if (!std::filesystem::exists(legacy, error))
    return;
  const Protocol::State state = Protocol::LoadStateFile(legacy.string());
  if (state.slot.empty() || state.seed.empty())
    return;
  const std::filesystem::path target = GameStatePath(state.slot, state.seed);
  if (std::filesystem::exists(target, error))
    return;
  std::filesystem::create_directories(target.parent_path(), error);
  std::filesystem::rename(legacy, target, error);
  if (error)
    PortLog::Write("archipelago: could not move %s to %s: %s\n", legacy.string().c_str(),
                   target.string().c_str(), error.message().c_str());
  else
    PortLog::Write("archipelago: moved %s to %s\n", legacy.string().c_str(),
                   target.string().c_str());
}

// Loads the configuration and starts the worker. `mutex` is held and no worker
// is running. `firstStart` is the launch, as opposed to a Connect from the
// overlay, which must not run the launch-only MP_AP_RESET_STATE again.
void StartLocked(Runtime& runtime, bool firstStart) {
  const std::string configPath = ConfigPath();
  runtime.config = Protocol::LoadConfigFile(configPath);
  if (!runtime.config.valid) {
    runtime.stateLabel = "off";
    runtime.lastError = runtime.config.error;
    PortLog::Write( "archipelago: %s\n", runtime.config.error.c_str());
    return;
  }

  std::filesystem::path parent = std::filesystem::path(configPath).parent_path();
  if (parent.empty())
    parent = ".";
  // tls_ca is written relative to the config file, not the working directory.
  if (!runtime.config.tlsCa.empty() && std::filesystem::path(runtime.config.tlsCa).is_relative())
    runtime.config.tlsCa = (parent / runtime.config.tlsCa).string();
  MigrateLegacyState(parent);
  // The seed this slot had last time picks the game (and its save card) until
  // the server says otherwise.
  const Protocol::Connection savedConnection = Protocol::LoadConnectionFile(configPath);
  const std::string& seed = savedConnection.seed;
  Protocol::State state;
  runtime.statePath.clear();
  runtime.roomSuffix.clear();
  if (!seed.empty()) {
    runtime.roomSuffix = RoomSuffix(runtime.config.slot, seed, savedConnection.room);
    runtime.statePath = GameStatePath(runtime.config.slot, seed, runtime.roomSuffix);
    state = Protocol::LoadStateFile(runtime.statePath);
    if (state.slot != runtime.config.slot || (!state.seed.empty() && state.seed != seed) ||
        (!runtime.roomSuffix.empty() && state.room != savedConnection.room))
      state = Protocol::State();
    state.seed = seed;
    if (!runtime.roomSuffix.empty())
      state.room = savedConnection.room;
  }
  // A rewind the client cannot see - a new game or an older save on the same
  // slot and seed - leaves the recorded checks looking valid, so this is the
  // way out: it drops them before the first connect (or once the seed is known).
  runtime.resetPending = false;
  if (firstStart && port::EnvFlag("MP_AP_RESET_STATE")) {
    if (runtime.statePath.empty()) {
      runtime.resetPending = true;
    } else if (state.nextItemIndex != 0 || !state.checkedLocations.empty() ||
               !state.progressive.empty()) {
      PortLog::Write("archipelago: MP_AP_RESET_STATE=1 discarded saved progress\n");
      state = Protocol::State();
      state.seed = seed;
    }
  }
  state.slot = runtime.config.slot;
  runtime.session = std::make_unique<Session>(runtime.config, state);
  runtime.enabled = true;
  runtime.stateLabel = "connecting";
  runtime.stop.store(false, std::memory_order_release);
  runtime.workerDone = false;
  runtime.worker = std::thread(Worker, std::ref(runtime));
}

void EnsureLoadedImpl(Runtime& runtime) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.attempted)
    return;
  runtime.attempted = true;
  if (port::EnvFlag("MP_AP_DISABLE")) {
    runtime.stateLabel = "off";
    return;
  }
  StartLocked(runtime, true);
}

// Runs on its own thread, because stopping the old worker can wait on a DNS
// lookup that cannot be cut short and the overlay must not freeze meanwhile.
// Stops the session, writes what it had, and starts again from the file.
void Restart(Runtime& runtime) {
  std::lock_guard<std::mutex> restarting(runtime.restartMutex);
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.exiting)
      return;
    old = std::move(runtime.worker);
    // Under the lock, so WaitBackoff cannot miss the wake-up below.
    runtime.stop.store(true, std::memory_order_release);
  }
  runtime.wake.notify_all();
  if (old.joinable())
    old.join();
  runtime.FlushState(); // the old session's progress, before the file is reread

  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.exiting)
    return;
  const std::string oldSlot = runtime.config.slot;
  const std::string oldServer = runtime.config.server;
  runtime.session.reset();
  runtime.enabled = false;
  runtime.connected = false;
  runtime.stateLabel = "off";
  runtime.lastError.clear();
  runtime.lastMessage.clear();
  runtime.lastLogged.clear();
  // Unsent checks are in the state file and go out with the next handshake.
  runtime.queuedChecks.clear();
  runtime.notifications.clear();
  runtime.pendingSay.clear();
  runtime.pendingHintScouts.clear();
  runtime.pendingBounce.clear();
  runtime.syncWanted = false;
  runtime.stateDirty = false;
  runtime.itemCount = 0;
  runtime.checkCount = 0;
  runtime.unmappedCount = 0;
  runtime.deathAnnounced = false;
  runtime.goalWanted = false;
  runtime.goalSent = false;
  runtime.recordSynced = false;
  StartLocked(runtime, false);
  // Items received but not yet handed to the game belong to that slot; the
  // state file already counts them as processed, so a new session on the same
  // slot would not send them again.
  if (runtime.config.slot != oldSlot || runtime.config.server != oldServer || !runtime.enabled)
    runtime.grants.clear();
}

void StartRestart(Runtime& runtime) {
  // Restarts that have all returned (counted down under the lock, so only the
  // lambda's own return is left) are joined here rather than piling up.
  std::vector<std::thread> finished;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.exiting)
      return;
    if (runtime.restartsRunning == 0)
      finished.swap(runtime.restarters);
  }
  for (std::thread& thread : finished)
    thread.join();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.exiting)
    return;
  ++runtime.restartsRunning;
  try {
    runtime.restarters.emplace_back([&runtime] {
      try {
        Restart(runtime);
      } catch (const std::exception& error) {
        PortLog::Write("archipelago: restart failed: %s\n", error.what());
      } catch (...) {
        PortLog::Write("archipelago: restart failed\n");
      }
      std::lock_guard<std::mutex> done(runtime.mutex);
      --runtime.restartsRunning;
      runtime.wake.notify_all();
    });
  } catch (...) {
    --runtime.restartsRunning; // no thread, so nothing to wait for
    throw;
  }
}

}  // namespace ClientDetail

using namespace ClientDetail;

void EnsureLoaded() {
  try {
    EnsureLoadedImpl(GetRuntime());
  } catch (const std::exception& error) {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.attempted)
      runtime.attempted = true;
    runtime.enabled = false;
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: initialization failed: %s\n", error.what());
  } catch (...) {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    runtime.attempted = true;
    runtime.enabled = false;
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: initialization failed\n");
  }
}

ConnectionDetails SavedConnection() {
  try {
    const Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
    ConnectionDetails details;
    details.server = saved.server;
    details.slot = saved.slot;
    details.password = saved.password;
    details.enabled = saved.enabled;
    details.seed = saved.seed;
    details.room = saved.room;
    return details;
  } catch (...) {
    return ConnectionDetails();
  }
}

std::vector<ConnectionDetails> RecentGames() {
  std::vector<ConnectionDetails> games;
  try {
    std::error_code error;
    for (std::filesystem::directory_iterator it(ConfigDirectory() / "archipelago_games", error), end;
         !error && it != end; it.increment(error)) {
      const std::filesystem::path file = it->path() / "game.json";
      if (!std::filesystem::is_regular_file(file, error))
        continue;
      const Protocol::Connection game = Protocol::LoadConnectionFile(file.string());
      if (game.server.empty() || game.slot.empty() || game.seed.empty())
        continue;
      ConnectionDetails details;
      details.server = game.server;
      details.slot = game.slot;
      details.password = game.password;
      details.seed = game.seed;
      details.room = game.room;
      details.lastPlayed = game.lastPlayed;
      games.push_back(std::move(details));
    }
  } catch (...) {
  }
  std::sort(games.begin(), games.end(), [](const ConnectionDetails& a, const ConnectionDetails& b) {
    return a.lastPlayed > b.lastPlayed;
  });
  return games;
}

bool Connect(const ConnectionDetails& details, std::string& error) {
  try {
    Protocol::Connection connection;
    connection.server = details.server;
    connection.slot = details.slot;
    connection.password = details.password;
    connection.enabled = true;
    const auto trim = [](std::string text) {
      const size_t first = text.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
        return std::string();
      return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    connection.server = trim(connection.server);
    connection.slot = trim(connection.slot);
    // The room page shows "/connect host:port"; pasting that whole is fine.
    if (connection.server.rfind("/connect ", 0) == 0)
      connection.server = trim(connection.server.substr(9));
    if (connection.server.empty() || connection.slot.empty()) {
      error = "a server and a slot name are needed";
      return false;
    }
    std::string host;
    std::string path;
    uint16_t port = 0;
    bool secure = false;
    std::string soloSeed;
    const bool solo = PortApSolo::ParseServer(connection.server, soloSeed);
    const bool bare = connection.server.find("://") == std::string::npos;
    if (!solo && ((bare && connection.server.find('/') != std::string::npos) ||
                  !PortWs::ParseUrl(ServerUrls(connection.server).front(), host, port, path, secure))) {
      error = "not a server address: " + connection.server;
      return false;
    }
    if (port::EnvFlag("MP_AP_DISABLE")) {
      error = "MP_AP_DISABLE is set";
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(ConnectionFileMutex());
      // The same slot on the same server is most likely the same game, so its
      // seed (and save card) stays until the server says otherwise.
      // A game resumed from the recent list brings its own seed.
      const Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
      if (!details.seed.empty()) {
        connection.seed = details.seed;
        connection.room = details.room;
      } else if (saved.server == connection.server && saved.slot == connection.slot) {
        connection.seed = saved.seed;
        connection.room = saved.room;
      }
      if (!Protocol::SaveConnectionFile(ConfigPath(), connection, error))
        return false;
    }
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      runtime.stateLabel = "connecting";
    }
    StartRestart(runtime);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "could not connect";
    return false;
  }
}

std::string SoloSeedInPlay() {
  const ConnectionDetails saved = SavedConnection();
  std::string name;
  if (!saved.enabled || !PortApSolo::ParseServer(saved.server, name))
    return std::string();
  return name;
}

bool SameSoloSeed(const std::string& a, const std::string& b) {
  // Names are made safe for a file name the same way, so "a b" is "a_b".
  return PortRandoGen::SeedPath(a) == PortRandoGen::SeedPath(b);
}

bool PlaySolo(const std::string& seedName, std::string& error) {
  // A loaded game keeps its save card (CSaveGameScreen only switches at the
  // front end), so another seed would be played on top of this game's
  // inventory and mix both seeds' progress.
  if (PortDebug::StateManager() != nullptr) {
    const std::string current = SoloSeedInPlay();
    if (current.empty() || !SameSoloSeed(current, seedName)) {
      error = "Quit to the title screen to switch seeds.";
      return false;
    }
  }
  // Read it now, so a seed that is missing or broken is said right away
  // rather than as a connection that keeps failing.
  PortRandoGen::Seed seed;
  if (!PortRandoGen::Load(PortRandoGen::SeedPath(seedName), seed, error))
    return false;
  ConnectionDetails details;
  details.server = std::string(PortApSolo::kServerPrefix) + seedName;
  details.slot = PortApSolo::kSlotName;
  // The save card is known before the server answers (it names the seed the
  // same way), so the first frames already use it.
  details.seed = "solo-" + seed.name;
  return Connect(details, error);
}

bool DeleteSolo(const std::string& seedName, std::string& error) {
  // The client and the in-process server write the seed's progress and save
  // card while it is the connection, so they would bring the files back.
  const std::string current = SoloSeedInPlay();
  if (!current.empty() && SameSoloSeed(current, seedName)) {
    error = "This seed is being played. Play another seed or disconnect first.";
    return false;
  }
  try {
    const std::string seedPath = PortRandoGen::SeedPath(seedName);
    std::error_code ec;
    if (!std::filesystem::exists(seedPath, ec)) {
      error = "no seed named " + seedName;
      return false;
    }
    // The save card's directory is named after the name inside the seed.
    PortRandoGen::Seed seed;
    std::string loadError;
    const std::string name = PortRandoGen::Load(seedPath, seed, loadError) ? seed.name : seedName;
    const std::filesystem::path card = GameDirectory(PortApSolo::kSlotName, "solo-" + name);
    std::filesystem::remove_all(card, ec);
    if (ec) {
      error = "could not delete " + card.string() + ": " + ec.message();
      return false;
    }
    const std::string stem = seedPath.substr(0, seedPath.size() - 5);
    std::filesystem::remove(PortApSolo::StatePath(seedName), ec);
    std::filesystem::remove(stem + ".spoiler.txt", ec);
    // Last, so a seed whose other files could not go stays listed.
    if (!std::filesystem::remove(seedPath, ec)) {
      error = "could not delete " + seedPath + (ec ? ": " + ec.message() : std::string());
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "could not delete the seed";
    return false;
  }
}

bool Disconnect(std::string& error) {
  try {
    {
      std::lock_guard<std::mutex> lock(ConnectionFileMutex());
      Protocol::Connection connection = Protocol::LoadConnectionFile(ConfigPath());
      connection.enabled = false;
      if (!Protocol::SaveConnectionFile(ConfigPath(), connection, error))
        return false;
    }
    EnsureLoaded();
    StartRestart(GetRuntime());
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "could not disconnect";
    return false;
  }
}

std::string SaveCardDirectory() {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.statePath.empty())
      return std::string();
    return std::filesystem::path(runtime.statePath).parent_path().string();
  } catch (...) {
    return std::string();
  }
}

std::string ConfigFilePath() {
  try {
    return ConfigPath();
  } catch (...) {
    return std::string();
  }
}

bool Enabled() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.enabled;
}

bool Connected() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.connected;
}

const char* StatusText() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string text;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled) {
      text = runtime.stateLabel == "connecting" ? "ap: connecting" : "ap: off";
    } else if (runtime.connected) {
      text = "ap: connected, " + std::to_string(runtime.itemCount) + " items, " +
             std::to_string(runtime.checkCount) + " checks";
    } else if (runtime.stateLabel == "error" && !runtime.lastError.empty()) {
      text = "ap: " + runtime.lastError;
    } else {
      text = "ap: connecting";
    }
  }
  constexpr size_t kOverlayLimit = 48;
  if (text.size() > kOverlayLimit)
    text.resize(kOverlayLimit);
  return text.c_str();
}

int ItemCount() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.itemCount;
}

std::vector< TrackedItem > TrackedItems() {
  EnsureLoaded();
  std::vector< TrackedItem > result;
  Runtime& runtime = GetRuntime();
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return result;
    const std::vector< Protocol::TrackedItem >& tracked = runtime.session->Tracked();
    result.reserve(tracked.size());
    for (const Protocol::TrackedItem& item : tracked) {
      TrackedItem entry;
      entry.name = item.name;
      entry.from = item.from;
      entry.step = static_cast< int >(item.step);
      entry.total = static_cast< int >(item.total);
      result.push_back(std::move(entry));
    }
  }
  return result;
}

std::vector< ChatLine > ChatLog(uint64_t* serial) {
  std::vector< ChatLine > result;
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    result.assign(runtime.chat.begin(), runtime.chat.end());
    if (serial != nullptr)
      *serial = runtime.chatSerial;
  } catch (...) {
    result.clear();
  }
  return result;
}

bool SendChat(const std::string& text, std::string& error) {
  try {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      error = "nothing to send";
      return false;
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    constexpr size_t kSayLimit = 1000;
    std::string trimmed = text.substr(first, std::min(last - first + 1, kSayLimit));
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.connected) {
      error = "not connected";
      return false;
    }
    // The socket thread sends these after its receive wait (up to 100 ms).
    constexpr size_t kPendingSay = 64;
    if (runtime.pendingSay.size() >= kPendingSay) {
      error = "still sending the last messages";
      return false;
    }
    runtime.pendingSay.push_back(std::move(trimmed));
    runtime.wake.notify_all();
    error.clear();
    return true;
  } catch (...) {
    error = "could not queue the message";
    return false;
  }
}

int CheckCount() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.checkCount;
}

const char* LastMessage() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string message;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    message = runtime.lastMessage;
  }
  return message.c_str();
}

const char* SwitchNotice() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string notice;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    notice = runtime.switchNotice;
  }
  return notice.c_str();
}

bool TakeNotification(std::string& text) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.notifications.empty()) {
      text = std::move(runtime.notifications.front());
      runtime.notifications.pop_front();
      return true;
    }
    return runtime.session != nullptr && runtime.session->TakeNotification(text);
  } catch (...) {
    text.clear();
    return false;
  }
}

const char* SeedName() {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    thread_local std::string seed;
    std::lock_guard<std::mutex> lock(runtime.mutex);
    seed = runtime.session != nullptr ? runtime.session->SeedName() : std::string();
    return seed.c_str();
  } catch (...) {
    return "";
  }
}

void QueueCheck(const char* locationKey) {
  if (locationKey == nullptr || locationKey[0] == '\0')
    return;
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr)
      return;
    int64_t id = 0;
    if (!runtime.session->MarkLocationChecked(locationKey, id)) {
      if (runtime.session->LocationIdFor(locationKey, id))
        PortLog::Write("archipelago: check %s (location %lld) was recorded already (%s); not sent again\n",
                       locationKey, static_cast<long long>(id),
                       runtime.session->LocationChecked(id) ? "the server or this game has it"
                                                            : "queued earlier");
      // Two things come back false here and only one of them is a problem. A key
      // the table knows but has already recorded is the ordinary case. A key the
      // table has never heard of means this location will never be reported, and
      // that fails silently: the session plays fine, items arrive, and the server
      // just never records a check. For a multiworld that is the worst shape a
      // failure can take, so name it once instead of dropping it quietly. The
      // usual cause is a location table whose keys are not the world:area:entity
      // form the game produces - see the note in docs/ARCHIPELAGO.md.
      if (!runtime.session->KnowsLocation(locationKey)) {
        if (runtime.unmappedCount == 0) {
          PortLog::Write(
              "archipelago: location '%s' has no id in the location table, so it was not "
              "reported. The session still works and the server will simply never record "
              "this check; if none ever arrives, the world's location keys are probably "
              "not world:area:entity (see docs/ARCHIPELAGO.md).\n",
              locationKey);
        }
        ++runtime.unmappedCount;
      }
      return;
    }
    runtime.MarkStateDirtyLocked();
    runtime.queuedChecks.push_back(id);
    PortLog::Write("archipelago: check %s (location %lld) %s\n", locationKey, static_cast<long long>(id),
                   runtime.connected ? "queued to send" : "queued; sent when connected");
  } catch (...) {
    // This is called from game pickup handling; AP must never disrupt gameplay.
  }
}

namespace {

bool BuiltinEnabled() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.enabled && runtime.session != nullptr && runtime.session->GetConfig().builtin;
}

// The built-in locations belong to the multiworld: a session on the built-in
// tables is running, or, with the client off, the running game is an AP game.
bool BuiltinRules() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.enabled && runtime.session != nullptr)
      return runtime.session->GetConfig().builtin;
  }
  return gpGameState != nullptr && IsApGame(gpGameState->PortApProgress());
}

// The layout of the seed in play, once its slot_data has been seen (in this
// session or an earlier one), else null. Shared rather than copied: this is
// asked on every hit the player takes and every room load.
std::shared_ptr<const PortApWorld::Layout> SeedLayout() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (!runtime.enabled || runtime.session == nullptr || !runtime.session->GetConfig().builtin ||
      !runtime.session->GetState().hasWorld)
    return nullptr;
  return SessionWorldLocked(runtime);
}

} // namespace

bool OwnsPickup(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    return MetroidPrime::FindPickup(world, area, entity) != nullptr && BuiltinRules();
  } catch (...) {
    return false;
  }
}

bool OwnsMemo(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    return MetroidPrime::FindMemo(world, area, entity) != nullptr && BuiltinRules();
  } catch (...) {
    return false;
  }
}

bool IsPickupMemo(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    return MetroidPrime::FindMemo(world, area, entity) != nullptr;
  } catch (...) {
    return false;
  }
}

bool PickupJingle(uint32_t world, uint32_t area, uint32_t entity, std::string& out) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindAudio(world, area, entity);
    if (location == nullptr)
      return false;
    if (BuiltinRules()) {
      int64_t item = 0;
      bool sameGame = false;
      {
        Runtime& runtime = GetRuntime();
        std::lock_guard<std::mutex> lock(runtime.mutex);
        if (runtime.enabled && runtime.session != nullptr &&
            runtime.session->ScoutedAt(location->id, item, sameGame, nullptr)) {
          out = MetroidPrime::JingleFile(item, sameGame);
          return true;
        }
      }
    }
    // No AP item known here: an offline seed may have put another item at
    // this pickup (CScriptPickup is rewritten at load).
    int itemType = -1, capacity = 0;
    if (PortRandomizer::Placed(world, area, location->pickup, itemType, capacity)) {
      if (const char* file = MetroidPrime::JingleForItemType(itemType)) {
        out = file;
        return true;
      }
    }
    return false;
  } catch (...) {
    return false;
  }
}

void RecordPickup(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    const int bit = CheckedBit(MetroidPrime::FindPickup(world, area, entity));
    if (bit < 0 || gpGameState == nullptr)
      return;
    gpGameState->PortApProgress().checked[bit / 32] |= 1u << (bit % 32);
  } catch (...) {
  }
}

void AnnouncePickup(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr)
      return;
    std::string text;
    bool offline = true;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr) {
        text = runtime.session->AnnounceLocation(location->id);
        offline = false;
      }
    }
    if (offline)
      text = std::string("Checked ") + location->name + ", sent when connected";
    else if (text.empty())
      text = std::string("Checked ") + location->name;
    PortLog::Write("archipelago: %s\n", text.c_str());
    CSamusHud::DisplayHudMemo(ToHudWide(text), CHUDMemoParms(5.f, true, false, false));
  } catch (...) {
  }
}

bool PickupModel(uint32_t world, uint32_t area, uint32_t entity, PortRandomizer::PickupModel& out) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr || !BuiltinRules())
      return false;
    int64_t item = 0;
    bool sameGame = false;
    int64_t flags = 0;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr ||
          !runtime.session->ScoutedAt(location->id, item, sameGame, &flags))
        return false;
    }
    const int key = sameGame ? MetroidPrime::PickupModelKey(item)
                             : MetroidPrime::OtherGameModelKey(flags);
    return PortSkipCutscenes::PickupModel(key, out);
  } catch (...) {
    return false;
  }
}

int PickupMinor(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr || !BuiltinRules())
      return -1;
    int64_t item = 0;
    bool sameGame = false;
    int64_t flags = 0;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr ||
          !runtime.session->ScoutedAt(location->id, item, sameGame, &flags))
        return -1;
    }
    if (!sameGame)
      return (flags & 1) != 0 ? 0 : 1; // progression
    switch (item - MetroidPrime::kItemBase) {
    case MetroidPrime::kMissileExpansion:
    case MetroidPrime::kPowerBombExpansion:
    case MetroidPrime::kEnergyTank:
      return 1;
    default:
      return 0;
    }
  } catch (...) {
    return -1;
  }
}

bool PickupScanText(uint32_t world, uint32_t area, uint32_t entity, std::string& out) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr || !BuiltinRules())
      return false;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr)
        out = runtime.session->ScanText(location->id);
    }
    // Empty when not scouted yet (or playing offline): the pickup is still a
    // check, not the retail item.
    return true;
  } catch (...) {
    return false;
  }
}

bool ArtifactHint(int itemType, std::string& out) {
  try {
    if (itemType < MetroidPrime::kArtifactTruth || itemType > MetroidPrime::kArtifactNewborn ||
        !BuiltinRules())
      return false;
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr || !runtime.session->HandshakeComplete())
      return false;
    out = runtime.session->ArtifactHint(MetroidPrime::kItemBase + itemType);
    return !out.empty();
  } catch (...) {
    return false;
  }
}

void ArtifactScanned(int itemType) {
  try {
    if (itemType < MetroidPrime::kArtifactTruth || itemType > MetroidPrime::kArtifactNewborn ||
        !BuiltinRules())
      return;
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr || !runtime.session->HandshakeComplete())
      return;
    // The session gives each location once, so a totem scanned again sends nothing.
    int64_t location = 0;
    if (runtime.session->TakeArtifactScanHint(MetroidPrime::kItemBase + itemType, location))
      runtime.pendingHintScouts.push_back(location);
  } catch (...) {
  }
}

bool NewGameStart(uint32_t& world, uint32_t& area) {
  try {
    if (!BuiltinEnabled())
      return false;
    world = MetroidPrime::kTallonWorld;
    area = 0;
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    if (seedLayout != nullptr) {
      PortApWorld::Place place;
      if (PortApWorld::StartRoom(*seedLayout, place)) {
        world = place.mlvl;
        area = place.mrea;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool WarpToStart(uint32_t& world, uint32_t& area) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    if (seedLayout == nullptr) {
      // A legacy seed only moves pickups, so its start is the new game's.
      if (!PortRandomizer::Enabled())
        return false;
      world = MetroidPrime::kTallonWorld;
      area = MetroidPrime::kLandingSiteArea;
      return true;
    }
    const PortApWorld::Layout& layout = *seedLayout;
    // A start room the tables don't have is the Landing Site, as for a new game.
    PortApWorld::Place place;
    place.mlvl = MetroidPrime::kTallonWorld;
    PortApWorld::StartRoom(layout, place);
    world = place.mlvl;
    area = place.mrea;
    return true;
  } catch (...) {
    return false;
  }
}

bool SeedStrings(uint32_t strg, std::vector< std::string >& out) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    return seedLayout != nullptr && PortApWorld::Strings(*seedLayout, strg, out);
  } catch (...) {
    return false;
  }
}

bool SuitDamageReduction(int mode, bool varia, bool gravity, bool phazon, float& out) {
  try {
    if (SeedLayout() == nullptr)
      return false;
    const float reduction = PortApWorld::SuitDamageReduction(mode, varia, gravity, phazon);
    if (reduction < 0.f)
      return false;
    out = reduction;
    return true;
  } catch (...) {
    return false;
  }
}

bool SeedResultsLine(std::string& out) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr || !runtime.session->GetConfig().builtin ||
        !runtime.session->GetState().hasWorld || runtime.session->GetState().seed.empty())
      return false;
    out = "Archipelago | " + runtime.session->GetState().seed + " | " +
          runtime.session->GetConfig().slot;
    return true;
  } catch (...) {
    return false;
  }
}

bool SeedGivesStartItems() {
  try {
    return SeedLayout() != nullptr;
  } catch (...) {
    return false;
  }
}

bool TeleporterDestination(uint32_t world, uint32_t editorId, uint32_t& destWorld,
                           uint32_t& destArea) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    if (seedLayout == nullptr)
      return false;
    const PortApWorld::Layout& layout = *seedLayout;
    PortApWorld::Place retail, place;
    retail.mlvl = destWorld;
    retail.mrea = destArea;
    if (!PortApWorld::TeleporterDestination(layout, world, editorId, retail, place))
      return false;
    destWorld = place.mlvl;
    destArea = place.mrea;
    return true;
  } catch (...) {
    return false;
  }
}

bool TempleOps(std::vector< uint8_t >& ops) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    if (seedLayout == nullptr)
      return false;
    const PortApWorld::Layout& layout = *seedLayout;
    ops = PortApWorld::TempleOps(layout);
    return !ops.empty();
  } catch (...) {
    return false;
  }
}

// The blast shields placed in each loaded room, watched by Poll. Rooms are
// patched on the loading thread.
std::mutex& PlacedShieldsMutex() {
  static std::mutex mutex;
  return mutex;
}
std::map< uint32_t, std::vector< PortApWorld::PlacedShield > >& PlacedShields() {
  static std::map< uint32_t, std::vector< PortApWorld::PlacedShield > > shields;
  return shields;
}

// The shields this game has broken, null outside a game.
const uint32_t* BrokenShields(uint32_t (&copy)[4]) {
  if (gpGameState == nullptr)
    return nullptr;
  for (int i = 0; i < 4; ++i)
    copy[i] = gpGameState->PortApProgress().shields[i];
  return copy;
}

// A shield whose trigger went inactive was broken (or its door was opened
// from behind): remember it, so the room comes back without it.
void WatchShields(CStateManager& mgr, CGameState::ApProgress& progress) {
  std::lock_guard< std::mutex > lock(PlacedShieldsMutex());
  std::vector< int > broken;
  for (auto& room : PlacedShields()) {
    auto& shields = room.second;
    for (size_t i = 0; i < shields.size();) {
      const TUniqueId uid = mgr.GetIdForScript(TEditorId(shields[i].trigger));
      const CEntity* entity = uid == kInvalidUniqueId ? nullptr : mgr.GetObjectById(uid);
      // The id is one DoorOps made up, so whatever answers to it is the trigger.
      if (entity == nullptr || entity->GetActive()) {
        ++i;
        continue;
      }
      const int bit = shields[i].bit;
      if (bit >= 0 && bit < PortApWorld::kShieldBits)
        progress.shields[bit / 32] |= 1u << (bit % 32);
      shields.erase(shields.begin() + i);
      broken.push_back(bit);
    }
  }
  // The same doorway's shield in the room behind it goes too, quietly: its
  // relay does the unlocking, with the sound, music and shake switched off.
  for (auto& room : PlacedShields()) {
    auto& shields = room.second;
    for (size_t i = 0; i < shields.size();) {
      if (std::find(broken.begin(), broken.end(), shields[i].bit) == broken.end()) {
        ++i;
        continue;
      }
      const uint32_t trigger = shields[i].trigger;
      auto send = [&](uint32_t id, EScriptObjectMessage msg) {
        const TUniqueId uid = mgr.GetIdForScript(TEditorId(id));
        if (uid != kInvalidUniqueId)
          mgr.SendScriptMsgAlways(uid, kInvalidUniqueId, msg);
      };
      for (uint32_t id = trigger + 3; id <= trigger + 5; ++id)
        send(id, kSM_Deactivate);
      send(trigger + 2, kSM_SetToZero);
      shields.erase(shields.begin() + i);
    }
  }
}

namespace {

// The pickups of room `mrea` whose locations are checked, by the session or
// in the loaded game's record. A death or an older save brings them back
// while their items stay given.
std::vector< uint32_t > TakenPickups(uint32_t mrea) {
  std::vector< uint32_t > taken;
  if (!BuiltinRules())
    return taken;
  size_t count = 0;
  const MetroidPrime::Location* locations = MetroidPrime::Locations(count);
  std::vector< int64_t > checked;
  {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.enabled && runtime.session != nullptr)
      checked = runtime.session->GetState().checkedLocations;
  }
  const CGameState::ApProgress* progress =
      gpGameState != nullptr && gpGameState->PortApProgress().reconciled ? &gpGameState->PortApProgress()
                                                                         : nullptr;
  for (size_t i = 0; i < count; ++i) {
    if (locations[i].area != mrea)
      continue;
    const bool recorded = progress != nullptr && i < 128 && ((progress->checked[i / 32] >> (i % 32)) & 1) != 0;
    if (recorded || std::find(checked.begin(), checked.end(), locations[i].id) != checked.end())
      taken.push_back(locations[i].pickup);
  }
  return taken;
}

} // namespace

bool RoomOps(uint32_t mrea, const uint8_t* scly, size_t size, std::vector< uint8_t >& ops) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    const std::vector< uint32_t > taken = TakenPickups(mrea);
    if (seedLayout == nullptr && taken.empty())
      return false;
    std::vector< PortSkipCutscenes::ScriptObject > objects;
    if (!PortSkipCutscenes::ScanObjects(scly, size, objects))
      return false;
    ops.clear();
    if (seedLayout != nullptr)
      ops = PortApWorld::RoomOps(*seedLayout, mrea, objects);
    const std::vector< uint8_t > pickups = PortApWorld::TakenPickupOps(objects, taken);
    if (!pickups.empty()) {
      PortLog::Write("archipelago: room %08X leaves out %zu checked pickup(s)\n", mrea, pickups.size() / 5);
      ops.insert(ops.end(), pickups.begin(), pickups.end());
    }
    return !ops.empty();
  } catch (...) {
    return false;
  }
}

bool DoorOps(uint32_t mrea, const uint8_t* scly, size_t size, std::vector< uint8_t >& ops) {
  try {
    {
      std::lock_guard< std::mutex > lock(PlacedShieldsMutex());
      PlacedShields().erase(mrea);
    }
    // Without a seed the layout is the disc's, and only the doorways the player
    // unlocked (the beam doors setting) change.
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    const PortApWorld::Layout disc;
    const PortApWorld::Layout& layout = seedLayout != nullptr ? *seedLayout : disc;
    uint32_t broken[4];
    uint32_t unlocked[PortApWorld::kBeamDoorWords];
    const std::vector< PortApWorld::DoorChange > doors = PortApWorld::Doors(
        layout, mrea, BrokenShields(broken), PortBeamDoors::Unlocked(unlocked));
    std::vector< PortSkipCutscenes::ScriptObject > objects;
    if (doors.empty() || !PortSkipCutscenes::ScanObjects(scly, size, objects))
      return false;
    std::vector< PortApWorld::PlacedShield > placed;
    ops = PortApWorld::DoorOps(
        doors, objects,
        [](const std::string& text) {
          // One scan per text: the key only has to tell the door types apart.
          return PortCustomRes::TextScan(0xD0020000ull << 32 | std::hash< std::string >()(text),
                                         text);
        },
        &placed);
    if (!placed.empty()) {
      std::lock_guard< std::mutex > lock(PlacedShieldsMutex());
      PlacedShields()[mrea] = std::move(placed);
    }
    return !ops.empty();
  } catch (...) {
    return false;
  }
}

bool MapDoors(uint32_t mapa, std::vector< std::pair< uint32_t, int > >& doors) {
  try {
    doors.clear();
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    const PortApWorld::Layout disc;
    const PortApWorld::Layout& layout = seedLayout != nullptr ? *seedLayout : disc;
    uint32_t broken[4];
    uint32_t unlocked[PortApWorld::kBeamDoorWords];
    for (const PortApWorld::MapDoor& door : PortApWorld::MapDoors(
             layout, mapa, BrokenShields(broken), PortBeamDoors::Unlocked(unlocked)))
      doors.emplace_back(door.doorId, door.type);
    return !doors.empty();
  } catch (...) {
    return false;
  }
}

bool BeamDoorUnlockable(uint32_t mrea, int door) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    const PortApWorld::Layout disc;
    uint32_t broken[4];
    return PortApWorld::BeamDoorUnlockable(seedLayout != nullptr ? *seedLayout : disc, mrea, door,
                                           BrokenShields(broken));
  } catch (...) {
    return false;
  }
}

bool LockedDoorStyle(int door, PortApWorld::DoorStyle& style,
                     std::vector< uint8_t >& vulnerability) {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    const PortApWorld::Layout disc;
    uint32_t broken[4];
    return PortApWorld::LockedDoorStyle(seedLayout != nullptr ? *seedLayout : disc, door,
                                        BrokenShields(broken), style, vulnerability);
  } catch (...) {
    return false;
  }
}

int RequiredArtifacts() {
  try {
    const std::shared_ptr<const PortApWorld::Layout> seedLayout = SeedLayout();
    return seedLayout != nullptr ? seedLayout->requiredArtifacts : 12;
  } catch (...) {
    return 12;
  }
}

bool VariaOnlyHeatProtection() {
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    return runtime.enabled && runtime.session != nullptr &&
           runtime.session->GetSlotData().variaOnlyHeat;
  } catch (...) {
    return false;
  }
}

int PreScanElevators() {
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled)
      return 0;
    if (runtime.session == nullptr || !runtime.session->GetSlotData().received)
      return -1;
    return runtime.session->GetSlotData().preScanElevators ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

bool RandomizedGame() {
  try {
    return BuiltinRules();
  } catch (...) {
    return false;
  }
}

bool ChargeBeamRule(int beam) {
  namespace Prime = MetroidPrime;
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    // Not an AP seed with progressive beams (or its slot_data has not arrived):
    // owning the Charge Beam is enough, as in retail.
    if (!runtime.enabled || !runtime.config.builtin || runtime.session == nullptr ||
        !runtime.session->GetSlotData().received ||
        !runtime.session->GetSlotData().progressiveBeamUpgrades)
      return true;
    // Each beam's second upgrade is its Charge Beam.
    int64_t item = 0;
    switch (beam) {
    case CPlayerState::kBI_Power:
      item = Prime::kProgressivePowerBeam;
      break;
    case CPlayerState::kBI_Ice:
      item = Prime::kProgressiveIceBeam;
      break;
    case CPlayerState::kBI_Wave:
      item = Prime::kProgressiveWaveBeam;
      break;
    case CPlayerState::kBI_Plasma:
      item = Prime::kProgressivePlasmaBeam;
      break;
    default:
      return true; // the Phazon Beam
    }
    return runtime.session->ReceivedCount(Prime::kItemBase + item) >= 2;
  } catch (...) {
    return true;
  }
}

int SpringBallRule() {
  namespace Prime = MetroidPrime;
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || !runtime.config.builtin || runtime.session == nullptr ||
        !runtime.session->GetSlotData().received)
      return -1;
    switch (runtime.session->GetSlotData().springBall) {
    case 1:
      return 1;
    case 2:
      return runtime.session->ReceivedCount(Prime::kItemBase + Prime::kSpringBall) > 0 ? 2 : 0;
    case 3:
      return runtime.session->ReceivedCount(Prime::kItemBase + Prime::kProgressiveBomb) > 0 ? 2
                                                                                            : 0;
    default:
      // The seed leaves Spring Ball out of logic: the port's own setting applies.
      return -1;
    }
  } catch (...) {
    return -1;
  }
}

bool Logic(LogicState& out) {
  try {
    Runtime& runtime = GetRuntime();
    size_t count = 0;
    const MetroidPrime::Location* locations = MetroidPrime::Locations(count);
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || !runtime.config.builtin || runtime.session == nullptr)
        return false;
      const Protocol::State& state = runtime.session->GetState();
      if (!state.hasLogic)
        return false;
      if (!runtime.logicValid || runtime.logicOptions != state.logic ||
          runtime.logicHasWorld != state.hasWorld ||
          runtime.logicWorldRevision != runtime.session->WorldRevision() ||
          runtime.logicItems != state.progressive) {
        runtime.logicOptions = state.logic;
        runtime.logicHasWorld = state.hasWorld;
        runtime.logicWorldRevision = runtime.session->WorldRevision();
        runtime.logicItems = state.progressive;
        PortApLogic::Options options = state.logic;
        if (state.hasWorld)
          PortApWorld::FillLogic(state.world, options);
        runtime.logicLevels = PortApLogic::Evaluate(options, state.progressive);
        runtime.logicValid = true;
      }
      out.levels = runtime.logicLevels;
      out.checked.assign(count, false);
      for (size_t i = 0; i < count; ++i) {
        out.checked[i] = std::find(state.checkedLocations.begin(), state.checkedLocations.end(),
                                   locations[i].id) != state.checkedLocations.end();
      }
    }
    // Checks made while disconnected are only in the save so far.
    if (gpGameState != nullptr && gpGameState->PortApProgress().reconciled) {
      const CGameState::ApProgress& progress = gpGameState->PortApProgress();
      for (size_t i = 0; i < count && i < 128; ++i) {
        if (((progress.checked[i / 32] >> (i % 32)) & 1) != 0)
          out.checked[i] = true;
      }
    }
    return out.levels.size() == count;
  } catch (...) {
    return false;
  }
}

std::string LogicText() {
  try {
    LogicState state;
    if (!Logic(state))
      return "logic: no Archipelago seed options yet\n";
    static const char* const kNames[] = {"out of logic", "inspect", "sequence break", "in logic"};
    size_t count = 0;
    const PortApLogic::Check* checks = PortApLogic::Checks(count);
    int totals[4] = {};
    int checked = 0;
    std::string lines;
    for (size_t i = 0; i < count; ++i) {
      if (state.checked[i]) {
        ++checked;
        continue;
      }
      ++totals[static_cast<int>(state.levels[i])];
      if (state.levels[i] == PortApLogic::Level::None)
        continue;
      lines += std::string("check ") + kNames[static_cast<int>(state.levels[i])] + ": " +
               checks[i].area + " / " + checks[i].room +
               (checks[i].section[0] != 0 ? std::string(" / ") + checks[i].section : std::string()) +
               "\n";
    }
    return "logic: " + std::to_string(totals[3]) + " in logic, " + std::to_string(totals[2]) +
           " sequence break, " + std::to_string(totals[1]) + " inspect, " +
           std::to_string(totals[0]) + " out of logic, " + std::to_string(checked) + " checked\n" +
           lines;
  } catch (...) {
    return std::string();
  }
}

void OnInventoryReset() {
  try {
    if (gpGameState == nullptr)
      return;
    CGameState::ApProgress& progress = gpGameState->PortApProgress();
    if (!Enabled() && !IsApGame(progress))
      return;
    progress.appliedIndex = 0;
    progress.reconciled = false; // reconcile again, now holding nothing
  } catch (...) {
  }
}

void Poll(CStateManager& mgr) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    CPlayerState* player = mgr.PlayerState();
    if (player == nullptr || gpGameState == nullptr)
      return;
    CGameState::ApProgress& progress = gpGameState->PortApProgress();

    static unsigned int notificationTicks = 0;
    std::deque<ItemGrant> grants;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      if (ReconcileLocked(runtime, progress)) {
        // Its items stay in the game, and quick resume would take it for the
        // new one's (whose identity it now has), so it is left for the file
        // select. The new game's receipts stay queued for it.
        if (mgr.GetWantsToQuit())
          return;
        PortLog::Write("archipelago: the server is another seed or room than the game in "
                       "play; back to the title\n");
        runtime.switchNotice = "This is another Archipelago seed or room than the game that "
                               "was running. Start its game from the file select.";
        PortSaveState::ResumeDiscard();
        mgr.QuitGame();
        return;
      }
      runtime.switchNotice.clear();
      grants.swap(runtime.grants);
      const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                           runtime.itemCount));
      runtime.itemCount += static_cast<int>(std::min(grants.size(), room));
    }
    for (const ItemGrant& grant : grants) {
      if (grant.index >= 0 && grant.index < std::numeric_limits<uint32_t>::max())
        progress.appliedIndex =
            std::max(progress.appliedIndex, static_cast<uint>(grant.index + 1));
      if (grant.itemType < 0)
        continue;
      const auto type = static_cast<CPlayerState::EItemType>(grant.itemType);
      const int amountBefore = player->GetItemAmount(type);
      const int capacityBefore = player->GetItemCapacity(type);
      player->InitializePowerUp(type, grant.capacity);
      player->IncrPickUp(type, grant.amount);
      // What the inventory holds after each grant, so a report shows whether an
      // item reached the game and what it did there.
      PortLog::Write("archipelago: applied item #%lld %s: %s %d/%d -> %d/%d (amount/capacity)\n",
                     static_cast<long long>(grant.index), grant.display.c_str(),
                     PortRandomizer::ItemName(grant.itemType), amountBefore, capacityBefore,
                     player->GetItemAmount(type), player->GetItemCapacity(type));
      if (type == CPlayerState::kIT_EnergyTanks)
        player->HealthInfo()->SetHP(player->CalculateHealth());
    }
    ApplyBuiltinWorld(runtime, mgr, *player);
    WatchShields(mgr, progress);

    // DeathLink, inbound. A bounce the server sent is applied by clearing the
    // alive flag, which is what the world's own client does and what drives
    // the whole death sequence here. Bounces that arrive while the game is not
    // running are left pending by the session and picked up on a later tick,
    // so one sent during a load is not lost.
    int deathsOwed = 0;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr) {
        deathsOwed = runtime.session->TakeDeathPending();
        // Outbound: a death is announced once. The flag stays cleared for
        // several seconds of the death animation, so this is latched rather
        // than sent on every tick, and cleared when the player is alive again.
        const bool alive = player->IsAlive();
        if (alive) {
          runtime.deathAnnounced = false;
        } else if (!runtime.deathAnnounced && runtime.connected) {
          // Only while connected: a death during an outage would otherwise
          // be announced, stale, on the next connect.
          runtime.deathAnnounced = true;
          runtime.pendingBounce = "death";
        }
        // A death that came in over DeathLink is not announced back: every
        // other client would echo it too, and the deaths would go round the
        // multiworld forever.
        if (deathsOwed > 0)
          runtime.deathAnnounced = true;
      }
    }
    if (deathsOwed > 0)
      PortLog::Write("archipelago: DeathLink: %d death(s) from the multiworld\n", deathsOwed);
    if (deathsOwed > 0)
      PortLog::Write("archipelago: DeathLink: %d death(s) from the multiworld\n", deathsOwed);
    for (int i = 0; i < deathsOwed; ++i)
      player->SetPlayerAlive(false);

    if (mgr.GetGameState() != CStateManager::kGS_Running)
      return;
    if (++notificationTicks < 120)
      return;
    notificationTicks = 0;

    std::string notification;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      std::string next;
      while (runtime.session->TakeNotification(next)) {
        runtime.notifications.push_back(std::move(next));
        next.clear();
      }
      while (runtime.notifications.size() > 8)
        runtime.notifications.pop_front();
      if (!runtime.notifications.empty()) {
        notification = std::move(runtime.notifications.front());
        runtime.notifications.pop_front();
      }
    }
    if (!notification.empty()) {
      CSamusHud::DisplayHudMemo(ToHudWide(notification), CHUDMemoParms(5.f, true, false, false));
    }
  } catch (...) {
    // Avoid leaking exceptions into the simulation loop.
  }
}

} // namespace PortAp
