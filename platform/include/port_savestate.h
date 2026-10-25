#pragma once

#include <cstdint>
#include <string>
#include <vector>

class CEntity;
class CStateManager;
class CInputStream;
struct PortSchemaClass;
struct PortSchemaField;

// Save states: save anywhere, load back to the same spot. A state is the
// retail save data (the same CGameState stream a memory card save holds:
// items, health, ammo, map, scans, script layers and relays, in-game time)
// plus where Samus stood, how she faced and whether she was in morph ball.
// Loading rebuilds the world from that save, as the debug world warp does,
// then puts Samus back.
//
// The state also carries an optional "room state" section (setting
// savestate_room_state): the live script objects of the areas that were
// loaded when it was saved, keyed by editor id (enemies with their health,
// position, velocity; platforms, doors, timers, counters, active flags;
// which objects existed; script generator copies; the script commands each enemy
// was sent; not AI state). After the rebuild it is applied on top of the
// freshly built objects, so killed enemies stay dead and the rest resume
// where they were. States without the section, or with it switched off, come
// back the way a memory card load leaves the room.
namespace PortSaveState {

constexpr int kSlotCount = 8;  // slots 1..8
constexpr int kUndoSlot = 0;   // written before every load

struct Info {
  bool exists = false;
  std::string world;       // front-end name
  std::string room;        // room name, or "Room N"
  double playTime = 0.0;   // in-game seconds
  int64_t savedAt = 0;     // Unix seconds
  bool morphed = false;
};

// Room state: the script objects of the loaded areas. Pure data, for the unit
// test; filled and applied by port_savestate_room.cpp. An object carries
// length-prefixed blocks keyed by a tag, so a reader skips tags it does not
// know and a newer writer can add more.
struct RoomBlock {
  uint32_t tag = 0;
  std::string data;
};
struct RoomObject {
  uint32_t editorId = 0;
  bool active = false;
  std::vector< RoomBlock > blocks;
};
// One captured area's room script fingerprint: a hash of the scripting data the
// area actually loaded (a skippable-cutscene patch may have swapped a patched
// copy in). A restore uses it to tell whether a rebuilt area still runs the room
// script the saved objects came from (issue #131).
struct AreaFingerprint {
  int32_t area = 0;
  bool known = false;  // false: nothing was hashed (no readable data)
  uint64_t hash = 0;
};
// One captured area's script layer bits as they were when its script objects were
// spawned (issue #161). Scripts switch layers at runtime without spawning or
// removing objects, so the live bits the memory card stream holds can differ from
// the bits the objects in the room came from; a restore spawns with these.
struct AreaSpawnLayers {
  int32_t area = 0;
  bool known = false;  // false: the area's spawn was not observed
  uint64_t mask = 0;   // bit n: layer n was spawned
};
struct RoomState {
  std::vector< int32_t > areas;     // areas captured (loaded, scripts built)
  std::vector< uint32_t > present;  // editor ids that existed in those areas
  std::vector< RoomObject > objects;
  // False for a state written before fingerprints existed (RMST version 1),
  // which restores as it always did. When true there is one entry per captured
  // area, in the same order, and an unknown entry means that area is never
  // applied.
  bool fingerprintsRecorded = false;
  std::vector< AreaFingerprint > fingerprints;
  // False for a state written before spawn layers were recorded (RMST version 2
  // and older), which rebuilds its areas from the live layer bits as it always did.
  // When true there is one entry per captured area, in the same order.
  bool spawnLayersRecorded = false;
  std::vector< AreaSpawnLayers > spawnLayers;
  bool empty() const { return areas.empty(); }
};
// Little-endian field writer and reader for the block payloads.
struct BlockWriter {
  std::string bytes;
  void U8(uint8_t v) { bytes.push_back(static_cast< char >(v)); }
  void U32(uint32_t v);
  void F32(float v);
  void Str(const std::string& s);
};
struct BlockReader {
  const std::string& bytes;
  size_t pos = 0;
  bool ok = true;
  explicit BlockReader(const std::string& b) : bytes(b) {}
  uint8_t U8();
  uint32_t U32();
  float F32();
  std::string Str();
};
// The block with `tag`, or nullptr.
const RoomBlock* FindBlock(const RoomObject& object, uint32_t tag);

// The kTagDoor block payload. Byte 0 is the door's mIsOpen and is what the old
// format held alone, so a one-byte payload decodes exactly as it used to. Byte 1,
// written by this build only, is a flag byte for the close paths the door can be
// captured in the middle of: each one reports closed while Think still owes the
// Closed message a live close would have sent, and the door's own branch sends it.
// An open door owes neither close, so its pending bits are clear; doClose is
// independent of them.
struct DoorSaveState {
  bool open = false;
  // bit 0: !mIsOpen && mWasOpen - the close animation is running and Think's
  // animation-end branch owes Closed with all of its connections (the dock
  // decrement included).
  bool pendingClose = false;
  // bit 1: mClosing - a refused opening (the room behind was not ready) still owes
  // Closed, but its branch skips the decrement connection.
  bool refusedClosePending = false;
  // bit 2: mDoClose - the close was asked for by another door, which also blocks
  // this door from opening until the close finishes.
  bool doClose = false;
  // bits 3-4, an open door only: which side of its door pair it is (issue #201).
  // OpenDoor makes the door that was opened the leader (mPartner2: it sends Open,
  // which starts its close timer, and closes its partner) and the door it opens
  // the follower (mPartner1: it sends only MaxReached and accepts Close from its
  // leader alone). A restore that linked them the other way round leaves the
  // follower's idle timer as the only thing that could close the pair and the
  // leader's timer refused: both doors stay open and hold the room's other doors
  // shut. Unknown is a save written before these bits, linked the old way.
  enum Partner : uint8_t {
    kPartnerUnknown = 0,
    kPartnerNone = 1,
    kPartnerLeads = 2,
    kPartnerFollows = 3,
  };
  Partner partner = kPartnerUnknown;
};
// The flag bits of byte 1: anything else is a payload this build never wrote and
// is refused.
constexpr uint8_t kDoorSaveFlagsKnown = 0x1F;
constexpr int kDoorSavePartnerShift = 3;
// The two wire bytes, in the order above (for the save writer and the unit test).
std::string EncodeDoorSaveState(const DoorSaveState& state);
// False for a payload this build never writes - empty, longer than two bytes, an
// open flag above 1, an unknown flag bit, open with a close in flight, both
// close paths at once, or a pair side on a closed door, none of which the door's
// state machine can produce. `out` is modified only for a payload that passes
// every check: a caller that keeps its previous value keeps it exactly.
bool DecodeDoorSaveState(const std::string& data, DoorSaveState& out);

// Room script fingerprints (pure, port_savestate_format.cpp, so the unit test
// runs the same code the runtime decides with). The 64-bit FNV-1a is a
// compatibility token - "is this still the same room script?" - not a security
// hash. 0 for a null or empty buffer: nothing is hashed that is not there.
uint64_t FingerprintScriptData(const uint8_t* data, size_t size);
AreaFingerprint MakeAreaFingerprint(int32_t area, const uint8_t* data, size_t size);

// What to do with one saved area while a state is applied to the room loaded now.
enum class RoomAreaRestore {
  kRestore,        // the same room script, or a state with no fingerprints
  kSkipChanged,    // another room script is loaded than the state was saved with
  kSkipUnreadable, // no save-time fingerprint, or the loaded data cannot be read
};
// The decision the runtime makes per area, before it deletes or overwrites
// anything: `data`/`size` is the scripting data the area runs now.
RoomAreaRestore DecideAreaRestore(const RoomState& room, int32_t area, const uint8_t* data, size_t size);
// One line for the log, e.g. "the room script changed".
const char* RoomAreaRestoreReason(RoomAreaRestore decision);

// The order the saved platforms of a room are put back in. A platform carries its
// slaves (effects, props, other platforms) by the distance it moves, so each one is
// restored after every platform that carries it: the parent drags the child, then
// the child's own saved pose wins. parents[i] lists the indices that carry platform
// i. False, with `order` empty, for a cycle, which no room script builds.
bool PlatformRestoreOrder(const std::vector< std::vector< size_t > >& parents,
                          std::vector< size_t >& order);

// The fixed header ahead of the game-state blob. Pure, for the unit test.
struct Header {
  uint32_t worldId = 0;
  int32_t areaId = 0;
  float position[3] = {0.f, 0.f, 0.f};
  float forward[3] = {0.f, 1.f, 0.f};
  bool morphed = false;
  double playTime = 0.0;
  int64_t savedAt = 0;
  std::string world;
  std::string room;
  RoomState roomState;  // optional section after the blob; empty when absent
};
std::string Encode(const Header& header, const std::vector< uint8_t >& blob);
bool Decode(const std::string& data, Header& header, std::vector< uint8_t >& blob);

// Room state glue (port_savestate_room.cpp), game thread only. Capture reads
// the loaded areas' script objects; Apply works through the saved areas that
// are loaded and not in `done` yet, adds them to `done`, and returns how many
// of the saved areas are still waiting. An area whose loaded room script data
// does not match the one the snapshot was taken from (its fingerprint) is added
// to `done` without applying or deleting anything: its objects, editor ids and
// script connections came from other data. `applied`, `removed`, `unmatched`
// and `skipped` accumulate over the calls.
struct RoomApplyStats {
  int applied = 0;
  int removed = 0;
  int unmatched = 0;
  int skipped = 0;  // areas left alone: their loaded room script data differs
  int replayed = 0;   // journalled script messages sent again
  int generated = 0;  // script generator copies made again
  int fields = 0;     // enemy fields set from kTagFields blocks
  int music = 0;      // music objects playing again from kTagMusic blocks
  // Saved areas not loaded yet that the last ApplyRoomState call asked the world to
  // stream in (or found streaming): they get their state when they finish, no longer
  // held against the restore's end. Set by each call, not summed.
  int late = 0;
};
// Script message journal (boss fights). The room script wakes an enemy with a
// message (a trigger sends Thardus Action) and then never sends it again, so an
// enemy rebuilt from disc by a state load would sit in its dormant start state
// behind doors the save keeps locked. Each scripted enemy keeps the messages the
// room script's connections delivered to it, in order, and a load replays them.
// Pure parts here, for the unit test.
struct ScriptMsgEntry {
  uint8_t msg = 0;                // EScriptObjectMessage
  uint32_t sender = 0xFFFFFFFFu;  // the sender's editor id, or none
};
// The most entries one enemy keeps (the first ones: what woke it comes first).
constexpr size_t kMaxScriptMsgs = 32;
// Whether a message is a command worth replaying. Activation is restored by the
// active flag, and engine notifications (registration, damage, touches, floor and
// splash contacts) and patrol traffic (Arrived, Next) are not commands.
bool JournalsScriptMsg(int msg);
// Appends `entry` unless it repeats the last entry or the journal is full.
bool AppendScriptMsg(std::vector< ScriptMsgEntry >& journal, const ScriptMsgEntry& entry);
// The kTagMessages payload: a count byte, then 5 bytes per entry.
std::string EncodeScriptMsgs(const std::vector< ScriptMsgEntry >& journal);
// False, with `out` untouched, for a payload this build never writes.
bool DecodeScriptMsgs(const std::string& data, std::vector< ScriptMsgEntry >& out);

// Game hooks (CStateManager), game thread only. NoteScriptMsg sees every message
// a script connection delivers; NoteGenerated marks an object a script generator
// copied from its template (it shares the template's editor id); NoteRemoved
// drops what was noted for an object that leaves the world. Ids are raw
// TUniqueId values (with the version bits).
void NoteScriptMsg(CStateManager& mgr, uint16_t dest, uint16_t sender, int msg);
void NoteGenerated(CStateManager& mgr, uint16_t uid);
void NoteRemoved(CStateManager& mgr, uint16_t uid);

void CaptureRoomState(CStateManager& mgr, RoomState& out);
// Debug views (port_statedump.cpp): a script generator's copy of an enemy; a timer's or counter's value.
bool IsScriptGeneratedCopy(const CStateManager& mgr, CEntity& ent);
// Whether a generated copy of this class is saved and made again on load: enemies,
// pickups and debris. Others (maze walls, sounds) are left to their generators.
bool SavesGeneratedCopy(CEntity& ent);
// Whether the room state says which music was playing (saves before kTagMusic do not).
bool RoomStateHasMusic(const RoomState& room);
bool ScriptTimerState(CEntity& ent, float& time, bool& timing);
bool ScriptCounterState(CEntity& ent, int& current);
int ApplyRoomState(CStateManager& mgr, const RoomState& room, std::vector< int32_t >& done,
                   RoomApplyStats& stats);
// Whether the saved room state will be applied to `area` (a saved area, not applied yet,
// whose room script is the one saved against). CStateManager::AreaLoaded then skips the
// mailbox: the area's messages were already delivered and played out before the save,
// and the saved objects carry the result - sending them again fires relays after the
// restore (a timer started, a point of interest woken that the save had finished with).
bool RoomRestoreOwnsArea(CStateManager& mgr, int area);
// The test behind it, for a given room state and list of areas done.
bool RoomAreaAwaitsRestore(CStateManager& mgr, const RoomState& room,
                           const std::vector< int32_t >& done, int area);
// The kTagFields block (port_savestate_fields.cpp): an enemy's own fields through the
// generated entity schema. Capture returns false for an entity it does not cover; Apply
// returns how many fields it set (0 for a block of another policy). MP_SAVESTATE_FIELDS=0
// turns both off.
bool CaptureEntityFields(const CStateManager& mgr, CEntity& ent, std::string& out);
int ApplyEntityFields(CStateManager& mgr, CEntity& ent, const std::string& data);
// Whether the block carries every schema class of `ent` (this build's fingerprints): then
// the room restore leaves the enemy's state to it and skips the message replay and the
// per-boss restore, which only older saves and changed classes still need.
bool EntityFieldsCover(CEntity& ent, const std::string& data);
// For statedump: whether `ent` gets a kTagFields block, and which schema fields it holds.
bool EntityFieldsSaved(const CStateManager& mgr, CEntity& ent);
bool SchemaFieldSaved(const PortSchemaClass& cls, const PortSchemaField& f);

// Spawn layers (issue #161), game thread only. CGameArea::LoadScriptObjects calls
// NoteAreaSpawn{Begin,End} around spawning an area's script objects: Begin records
// the layer bits about to be spawned (what CaptureRoomState writes), End puts the
// live bits back if ArmSpawnLayers overrode them. ArmSpawnLayers runs right after a
// state's game state is installed: it makes each recorded area of world `worldId`
// spawn with its recorded bits, remembering the live bits it replaced. Disarm
// restores any live bits still replaced (an area that never finished loading) and
// forgets the override. SetSpawnLayersLive(true) puts the live bits back for the
// areas still armed without forgetting them, so a save taken meanwhile writes the
// live bits; SetSpawnLayersLive(false) arms them again.
void NoteAreaSpawnBegin(CStateManager& mgr, int area);
void NoteAreaSpawnEnd(CStateManager& mgr, int area);
void ArmSpawnLayers(const RoomState& room, uint32_t worldId);
void DisarmSpawnLayers();
void SetSpawnLayersLive(bool live);

// Quick resume: one extra state (resume.mpss, outside the slots) written when
// the app goes to the background or quits, with which memory card file it
// came from and which game it belongs to. The next launch offers it at the
// title screen. Pure parts here, for the unit test; glue in port_resume.cpp.
struct ResumeMeta {
  uint32_t fileIdx = 0;     // memory card file (0-2)
  uint64_t cardSerial = 0;  // the card it was read from
  std::string identity;     // randomizer seed / Archipelago game, "" for retail
};
std::string EncodeResume(const ResumeMeta& meta, const std::string& stateFile);
bool DecodeResume(const std::string& data, ResumeMeta& meta, std::string& stateFile);
// Whether a resume made in `stored` may be offered for the game in `current`.
bool ResumeMatches(const ResumeMeta& stored, const ResumeMeta& current);

// What the writer knows about the running game.
struct ResumeContext {
  bool inGame = false;       // a world is loaded with Samus in it
  bool initialized = false;  // the state manager finished starting up
  bool quitting = false;
  bool dead = false;
  bool cinematic = false;    // cutscene or cinematic camera
  bool transition = false;   // elevator ride / world transition
  bool saveUi = false;       // memory card save screen open
};
// Why a resume must not be written now, or nullptr when it may.
const char* ResumeSkipReason(const ResumeContext& ctx);

// The resume file is built from an in-memory snapshot kept on the game thread
// (a "last safe point", refreshed on room entry and every kResumeRefreshSeconds
// while nothing below applies), so the write at background or quit needs no
// game state and works from an SDL event watch. Each new snapshot is also
// written at once, so a crash leaves the last one behind. What to write then:
enum class ResumeWriteChoice {
  kNothing,          // no snapshot yet (title screen): keep the old file
  kRefreshThenWrite, // safe right now: take a fresh snapshot, write it
  kWriteSnapshot,    // dead, cutscene, elevator...: write the last safe one
};
ResumeWriteChoice ChooseResumeWrite(bool safeNow, bool haveSnapshot);
constexpr double kResumeRefreshSeconds = 30.0;
constexpr double kResumeStableSeconds = 2.0;
// Whether the game thread should refresh the snapshot this frame. A new room
// is taken once it has been stable for kResumeStableSeconds.
bool ResumeRefreshDue(bool safeNow, bool haveSnapshot, bool sameRoomAsSnapshot, double secondsInRoom,
                      double secondsSinceSnapshot);

// Port glue (port_savestate.cpp). Game thread unless noted.

// The resume file, as the title screen prompt shows it.
struct ResumeInfo {
  bool exists = false;
  uint32_t fileIdx = 0;
  uint64_t cardSerial = 0;
  std::string world;
  std::string room;
  double playTime = 0.0;
  int64_t savedAt = 0;
  bool afterCrash = false;   // the last run did not quit cleanly
  bool crashedAgain = false; // ...and had resumed from this same state
};
// Quit or background, between ticks: refreshes the snapshot when it is safe,
// then writes resume.mpss (synchronously, atomically) from the last snapshot.
// Needs the quick_resume setting (or `force`). True when written; with no
// snapshot the previous file is kept.
bool ResumeSave(bool force = false);
// Per frame, from CStateManager's tick: keeps the snapshot current.
void ResumeTick(CStateManager& mgr);
// Registers the SDL event watch that writes on WILL/DID_ENTER_BACKGROUND (SDL
// never queues those events, it only calls watchers, on the thread that pumps).
// Also notes whether the last run crashed, and marks this one as running.
void ResumeInstallEventWatch();
// A clean quit: the next launch must not take this run for a crash.
void ResumeQuitCleanly();
// True around the main loop's event pump: a background event arriving there
// may take a fresh snapshot first (the game is between ticks).
void SetEventPumpSafe(bool safe);
// Writes the last snapshot. Any thread. False when there is none, nothing new
// to write, or the write failed.
bool ResumeFlush();
// The resume file, when it is readable and belongs to this game (same
// randomizer or Archipelago identity).
bool ResumeOffer(ResumeInfo& info);
// Yes at the title screen, once the card serial is known: queues the state
// for the game CFrontEndUI is about to start, and gives the file to start it
// with. The file is kept until the next snapshot replaces it, so a crash right
// after the resume can resume again. False if the file is gone or from
// another game or card.
bool ResumeAccept(uint64_t cardSerial, uint32_t& fileIdx);

// The prompt's window (platform/debug_ui.cpp draws it, ImGui buttons answer).
// Any thread. CFrontEndUI keeps the prompt current every tick (nullptr: none)
// and takes the answer a button gave; pad and keyboard answers it reads itself.
void SetResumePrompt(const ResumeInfo* info, bool yesSelected);
bool ResumePromptShown(ResumeInfo& out, bool& yesSelected);
void AnswerResumePrompt(bool yes);
// -1 none, 0 No, 1 Yes. Clears it.
int TakeResumePromptAnswer();
// Console: load the resume state from inside a game; delete it; one line.
bool RequestResumeLoad();
void ResumeClear();
// Drops the snapshot and the resume file, silently: the game in play is no
// longer the one its identity names (an Archipelago room switch).
void ResumeDiscard();
std::string ResumeStatus();

// The folder states live in (created on first use).
std::string Folder();
Info SlotInfo(int slot);
// The slot F5/F9 use.
int SelectedSlot();
void SetSelectedSlot(int slot);

// Save or load from the overlay, a hotkey or the console. They run at the
// next game tick; the pause and map screens hold them until you unpause.
// A request with no game running is refused at once.
bool RequestSave(int slot); // false (and LastMessage says why) if refused now
bool RequestLoad(int slot);
// Reads the mods folder again (and installs a finished Remastered import):
// the game is rebuilt where Samus stands, as by a save and a load, with every
// PAK reopened. In the front end CFrontEndUI does it at its next tick; with
// no game running otherwise, the next one to start does it.
bool RequestModReload();
// How many times the mod files were read again, to tell when a request ran.
int ModReloads();
// What the last request did, for the overlay and the console.
std::string LastMessage();

// CStateManager::Update: does a queued save, places Samus after a load, and
// returns true when a load has started (the caller must return at once: the
// world is going away).
bool Tick(CStateManager& mgr);
// CMainFlow, before the next game loads: swaps in the loaded game state.
void InstallPending();
// CMainFlow: whether the front end (title screen, file select) is the flow.
void SetInFrontEnd(bool inFrontEnd);
// CFrontEndUI, each tick: true (once) when a reload was asked for in the
// front end. It closes its movies, calls ReloadModsNow and opens them again,
// since rebuilding the disc-file overlays breaks the reads of open files.
bool TakeFrontEndReload();
void ReloadModsNow();

} // namespace PortSaveState

// The prompt starts with the Nintendo splash. Splashes and the front end both
// drive it from their tick and input (src/MetroidPrime/CFrontEndUI.cpp).
// Tick: poll for an offer, take a button's answer, refresh the window.
// Input: pad/keyboard (controller 0); true if consumed. Sound: true once after
// a choice moved or answered. Accepted: Yes was chosen and the start is under way.
class CFinalInput;
void PortResumePromptTick();
bool PortResumePromptInput(const CFinalInput& input);
bool PortResumePromptTakeSound();
bool PortResumePromptAccepted();
