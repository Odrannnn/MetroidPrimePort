#ifndef METROID_PRIME_PORT_PORT_APCLIENT_H
#define METROID_PRIME_PORT_PORT_APCLIENT_H
#include "port_ap_logic.h"

#include <cstdint>
#include <string>
#include <vector>

class CStateManager;
namespace PortRandomizer {
struct PickupModel;
}
namespace PortApWorld {
struct DoorStyle;
}

// Archipelago client.
//
// A background thread owns a WebSocket to the server and speaks the AP JSON
// protocol; the game thread only touches two queues. Location checks collected
// in game are queued with QueueCheck() and sent on that thread; items the server
// sends are queued and granted to the player state by Poll(), which the game
// calls once per simulation tick. Nothing here runs when no configuration is
// present.
//
// Configuration: $MP_AP_CONFIG, else <user dir>/archipelago.json. Each slot's
// game in each seed has a directory beside it, archipelago_games/<slot>-<seed>,
// holding its save card, game.json (how to reconnect) and archipelago_state.json
// (the last processed item index and the checks already sent, so reconnecting
// does not re-grant items).
//
// Environment:
//   MP_AP_DISABLE=1  ignore the configuration entirely
//   MP_AP_CONFIG=... configuration path
//   MP_AP_SEND_ALL=1 debug: on connect, queue every configured location id, to
//                    validate the map against the server (marks them all found)
namespace PortAp {

// Loads the configuration and starts the client thread once. Idempotent, never
// throws, and a no-op when disabled or unconfigured.
void EnsureLoaded();
// The overlay's Connect screen. The details live in the configuration file
// (other keys there are kept), so they survive a restart.
struct ConnectionDetails {
  std::string server;
  std::string slot;
  std::string password;
  bool enabled = true; // false after Disconnect: kept, but not connected at launch
  // The game's seed, when known. Connect uses it to pick that game's save card
  // before the server answers; empty keeps the saved one for the same slot.
  std::string seed;
  // The server room of that seed (rooms generated from one seed share it).
  std::string room;
  int64_t lastPlayed = 0; // Unix time, RecentGames only
};
ConnectionDetails SavedConnection();
// Every game played before (archipelago_games/*/game.json), most recent first.
std::vector<ConnectionDetails> RecentGames();
// Saves the details and restarts the client with them, on a background thread
// (the status line shows the progress). False, with `error`, when the details
// are incomplete or the file cannot be written; nothing changes then.
bool Connect(const ConnectionDetails& details, std::string& error);
// Plays a built-in randomizer seed (randomizer_seeds/<name>.json) as a
// one-player game through an in-process server, as slot "Samus" on the server
// "solo:<name>". Saves the details and restarts the client like Connect; false,
// with `error`, when the seed cannot be loaded, or while a game is loaded and
// the seed is not the one in play (the game keeps its save card until the
// title screen).
bool PlaySolo(const std::string& seedName, std::string& error);
// The built-in seed the connection plays, or "" (another server, or
// disconnected).
std::string SoloSeedInPlay();
// Two seed names that select the same seed file.
bool SameSoloSeed(const std::string& a, const std::string& b);
// Deletes a built-in seed: its file, progress, spoiler file and save card.
// False, with `error`, for the seed in play or a file that cannot be removed.
bool DeleteSolo(const std::string& seedName, std::string& error);
// Ends the session and keeps it off at the next launch, until Connect.
bool Disconnect(std::string& error);
// The directory whose memory card the running Archipelago game saves to, or ""
// for the default card (no session, or its seed not known yet).
std::string SaveCardDirectory();
// Where the configuration is read from and saved to.
std::string ConfigFilePath();
// A configuration with a server and a slot was loaded.
bool Enabled();
// The handshake with the server finished.
bool Connected();
// Short line for the F1 overlay: state, items received, checks sent.
const char* StatusText();
// Items granted to the player this session.
int ItemCount();
// Location checks sent this session.
int CheckCount();
// Most recent PrintJSON text, or "" when there has not been one.
const char* LastMessage();
// Why the running game was sent back to the title (the server is another
// seed or room than the game in play), or "" until then. Cleared when the
// next game starts.
const char* SwitchNotice();
// Drains the oldest human-readable notification (an item receipt or a
// PrintJSON line) into `text`. False when the queue is empty. Call from the
// game thread.
bool TakeNotification(std::string& text);
// One line of the item tracker: the item's display name, who sent it, and the
// step of a progressive sequence. `step` and `total` describe a flat item as
// 1 of 1. `from` is empty for an item the player sent themselves. The tracker
// list, oldest first; this reads the session's records and is safe from the
// overlay's thread.
struct TrackedItem {
  std::string name;
  std::string from;
  int step = 1;
  int total = 1;
};
std::vector< TrackedItem > TrackedItems();
// One line of the chat log: every PrintJSON message in full (chat, hints,
// item sends, command replies), plus the port's own "port" lines for
// connections. `type` is the packet's type, as the server named it.
struct ChatLine {
  std::string type;
  std::string text;
};
// The chat log, oldest first. It outlives reconnects. `serial`, when given,
// receives a number that changes whenever a line is added.
std::vector< ChatLine > ChatLog(uint64_t* serial = nullptr);
// Queues a Say for the socket thread: chat text, or a server command such as
// "!hint Missile Launcher". False with `error` set when not connected or the
// text is empty.
bool SendChat(const std::string& text, std::string& error);
// Seed name from the server's RoomInfo, or "" before it arrives.
const char* SeedName();

// The tracker's view of the seed: for each location (indexed as
// PortApLogic::Checks and MetroidPrime::Locations) whether it is in logic with
// the items received so far, and whether it has been checked. False when there
// is nothing to say: no session on the built-in tables, or the seed's options
// have never been received. Works offline once they have. Game thread.
struct LogicState {
  std::vector< PortApLogic::Level > levels;
  std::vector< bool > checked;
};
bool Logic(LogicState& out);
// The same as text for the console: the reachable checks, one per line.
std::string LogicText();

// Queues the configured location id for `locationKey` (the randomizer's
// "world:area:entity" key). No-op when unconfigured, unmapped, or already sent.
void QueueCheck(const char* locationKey);

// With the built-in tables, the pickups at this slot's locations hold the
// multiworld's items rather than the retail ones: touching one sends the check
// and grants nothing locally, and its "acquired" memo stays quiet. This holds
// for an AP game (a save some session has given items to) while the client is
// off too.
bool OwnsPickup(uint32_t world, uint32_t area, uint32_t entity);
bool OwnsMemo(uint32_t world, uint32_t area, uint32_t entity);
// Whether `entity` is a pickup's "acquired" memo (any game, AP or not), which
// seamless_pickups shows on the HUD instead of as a message box.
bool IsPickupMemo(uint32_t world, uint32_t area, uint32_t entity);
// The jingle file for the StreamedAudio `entity` when it is an AP location's
// pickup jingle and the item placed there is known (scouted), so the pickup
// plays the placed item's jingle, not the retail item's. Without an AP
// session, the item an offline seed (PortRandomizer) placed at the location's
// pickup counts. False: leave it.
bool PickupJingle(uint32_t world, uint32_t area, uint32_t entity, std::string& out);
// Records an owned pickup in the game's save, so the check reaches the server
// on a later connection if this one cannot send it. Call before QueueCheck.
void RecordPickup(uint32_t world, uint32_t area, uint32_t entity);
// Shows on the HUD what an owned pickup held ("Found X for Bob"), or its
// location name when the server has not said yet. Call after QueueCheck.
void AnnouncePickup(uint32_t world, uint32_t area, uint32_t entity);

// The model for the item the server scouted at an AP pickup: that item's
// retail pickup model, or a stand-in for another game's item. False (keep the
// retail model) when AP is off, the location isn't scouted yet, or the item
// has no pickup on the disc.
bool PickupModel(uint32_t world, uint32_t area, uint32_t entity, PortRandomizer::PickupModel& out);
// Whether the item at an AP location is a minor one (a missile or power bomb
// expansion or an energy tank of this game, or a filler item of another): 1
// minor, 0 major, -1 not an AP game or not scouted yet.
int PickupMinor(uint32_t world, uint32_t area, uint32_t entity);
// The scan text for that item ("Hookshot\nfor Bob (A Link to the Past)"),
// empty before the location is scouted. False when the location isn't one of
// the multiworld's (AP off, or not an AP location).
bool PickupScanText(uint32_t world, uint32_t area, uint32_t entity, std::string& out);
// The Artifact Temple totem text for an artifact item type (29-40): where the
// multiworld put it. False when AP is off or not connected.
bool ArtifactHint(int itemType, std::string& out);
// The player finished scanning an artifact's totem: asks the server to hint
// where the artifact is when it is in this slot's own world. Reading the text
// (ArtifactHint) never does: that runs when the room loads.
void ArtifactScanned(int itemType);

// A spawn point's Reset message replaced the whole inventory. Any received items
// the game held are gone, so the next Poll rewinds the session and the server
// replays them. No-op when AP is off.
void OnInventoryReset();

// Where a new game starts: false for the retail start. With the built-in
// tables it is the seed's starting room, or the Landing Site until the seed's
// layout is known (seeds skip the frigate). `area` is 0 for the world's first.
bool NewGameStart(uint32_t& world, uint32_t& area);

// Where a save station's "Warp to start" choice (or declining it with L and R
// held) leads: the seed's starting room, as every randomprime seed of the
// apworld allows, or the Landing Site for a legacy MP_RANDO seed (`area` is 0
// for the world's first). False without a randomizer seed.
bool WarpToStart(uint32_t& world, uint32_t& area);

// The strings the seed gives the string table `strg` in place of the disc's
// (UTF-8): elevator texts and the temple's objective. False for any other
// table, and without a seed layout.
bool SeedStrings(uint32_t strg, std::vector< std::string >& out);

// The share of damage the suits take off in the seed in play, under the
// staggered suit damage mode the player set (0 default, 1 progressive,
// 2 additive: the server doesn't say which the seed was made with). False
// without a seed layout or in the default mode: the game's own rule.
bool SuitDamageReduction(int mode, bool varia, bool gravity, bool phazon, float& out);

// The line naming the seed and the slot that the completion screen shows above
// "Percentage Complete" (randomprime's resultsString). False without a seed
// layout.
bool SeedResultsLine(std::string& out);

// The seed's layout is known, so the server hands out everything Samus starts
// with (the starting beam, the Scan Visor unless shuffled, the start room's
// loadout) and a spawn point gives only the Combat Visor and Power Suit.
bool SeedGivesStartItems();

// Where the seed leads the world teleporter `editorId` of `world`: rewrites
// the destination and returns true for a shuffled elevator, and for the
// Artifact Temple's portal when the seed has no Metroid Prime fight.
bool TeleporterDestination(uint32_t world, uint32_t editorId, uint32_t& destWorld,
                           uint32_t& destArea);

// The seed's changes to the Artifact Temple's script (fewer artifacts needed,
// no Meta Ridley fight) as an op list for PortSkipCutscenes::ApplyOps. False
// when the temple is the retail one.
bool TempleOps(std::vector< uint8_t >& ops);

// What the seed's smaller options change in a room's script (no Hive Mecha,
// backwards Lower Mines, Flaahgra power bombs), as an op list for
// PortSkipCutscenes::ApplyOps over its script `scly`. False for no change.
bool RoomOps(uint32_t mrea, const uint8_t* scly, size_t size, std::vector< uint8_t >& ops);

// The seed's door types (colours, locked doors) for a room, as an op list for
// PortSkipCutscenes::ApplyOps over its script `scly`. False for no change.
bool DoorOps(uint32_t mrea, const uint8_t* scly, size_t size, std::vector< uint8_t >& ops);

// The doors of a map area whose icon the seed changes: editor id and the
// door colour (0 blue, 1 shield, 2 ice, 3 wave, 4 plasma). False for none.
bool MapDoors(uint32_t mapa, std::vector< std::pair< uint32_t, int > >& doors);
// Whether door table index `door` of area `mrea` is a beam door with no blast
// shield on it, as the seed (or the disc, with none) leaves it.
bool BeamDoorUnlockable(uint32_t mrea, int door);
// PortApWorld::LockedDoorStyle for this seed (the disc with none) and the broken shields.
bool LockedDoorStyle(int door, PortApWorld::DoorStyle& style, std::vector< uint8_t >& vulnerability);

// How many artifacts open the Artifact Temple: the seed's required_artifacts,
// 12 otherwise.
int RequiredArtifacts();

// The connected seed wants heat to hurt through every suit but the Varia Suit
// (the AP world's non_varia_heat_damage, on by default). False when AP is off.
bool VariaOnlyHeatProtection();

// Spring Ball as the connected seed has it, for a seed on the built-in tables
// whose slot_data has arrived: 0 not yet (its item hasn't been received),
// 1 once the Morph Ball Bombs are held, 2 on (its item, or the first
// Progressive Bomb, has been received). -1 when no such seed is connected or
// the seed has Spring Ball off, so the port's own setting applies.
int SpringBallRule();

// Whether an owned Charge Beam charges `beam` (a CPlayerState::EBeamId). On a seed with
// progressive_beam_upgrades only the beams whose second upgrade has arrived
// charge; anything else (AP off, slot_data not yet in, the Phazon Beam) is true.
bool ChargeBeamRule(int beam);

// The connected seed's pre_scan_elevators: 1 on, 0 off (or AP off), -1 while
// the client is on but its slot_data has not arrived yet.
int PreScanElevators();

// The running game is an Archipelago game on the built-in tables (connected,
// or an AP save with the client off), so retail hints would point at items
// that are no longer there.
bool RandomizedGame();

// Called once per simulation tick by CStateManager::Update. Sends queued checks
// and grants queued items to the player state.
void Poll(CStateManager& mgr);

} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_APCLIENT_H
