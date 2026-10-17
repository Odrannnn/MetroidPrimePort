#include "port_ap_metroidprime.h"
#include "port_ap_protocol.h"

#include "port_randomizer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

bool sPassed = true;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "[ap-tests] FAILED: %s\n", message);
    sPassed = false;
  }
}

bool Contains(const std::string& text, const std::string& value) {
  return text.find(value) != std::string::npos;
}

PortJson::Value Packet(const std::string& json) {
  PortJson::Value value;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(json, value, offset, &reason)) {
    Check(false, "test packet JSON parses");
  }
  return value;
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A per-process temporary directory, so parallel test runs cannot collide.
unsigned long TestProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

} // namespace

int main() {
  using namespace PortAp::Protocol;
  const std::filesystem::path testDir = std::filesystem::temp_directory_path() /
      ("mp-ap-test-" + std::to_string(static_cast<long long>(TestProcessId())));
  std::filesystem::remove_all(testDir);
  std::filesystem::create_directories(testDir);

  const std::string fullConfig = R"({
    "server":"ws://127.0.0.1:38281", "slot":"Player1", "password":"secret",
    "game":"Metroid Prime", "items_handling":7,
    "version":{"major":0,"minor":6,"build":0}, "tags":["AP"],
    "locations":{
      "39F2DE28:B2701146:0000007E":123456,
      "39F2DE28:B2701146:0000007F":123456,
      "39F2DE28:B2701146:00000080":123457
    },
    "items":{
      "100":{"item":"EnergyTanks","display":"Energy Tank","amount":2,"capacity":3},
      "101":{"item":"Missiles"}
    },
    "future_setting":{"ignored":true}
  })";
  Config config = ParseConfig(fullConfig);
  Check(config.valid && config.error.empty(), "complete configuration parses");
  Check(config.server == "ws://127.0.0.1:38281" && config.slot == "Player1" &&
            config.password == "secret" && config.game == "Metroid Prime" &&
            config.itemsHandling == 7,
        "configuration fields parse");
  Check(config.versionMajor == 0 && config.versionMinor == 6 && config.versionBuild == 0 &&
            config.tags.size() == 1 && config.tags[0] == "AP",
        "version and tags parse");
  Check(config.locations.size() == 3 && config.items.size() == 2,
        "location and item maps parse");
  Check(config.items.at(100).itemType == PortRandomizer::ItemFromName("EnergyTanks") &&
            config.items.at(100).amount == 2 && config.items.at(100).capacity == 3 &&
            config.items.at(100).display == "Energy Tank" &&
            config.items.at(101).itemType == PortRandomizer::ItemFromName("Missiles") &&
            config.items.at(101).amount == 1 && config.items.at(101).capacity == 1 &&
            config.items.at(101).display == "Missiles",
        "item types, display names and optional defaults parse");

  Config missingServer = ParseConfig(R"({"slot":"Player1"})");
  Config missingSlot = ParseConfig(R"({"server":"ws://localhost"})");
  Check(!missingServer.valid && Contains(missingServer.error, "server"),
        "missing server is a clear configuration error");
  Check(!missingSlot.valid && Contains(missingSlot.error, "slot"),
        "missing slot is a clear configuration error");
  Config badLocation = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","locations":{"not-a-key":1}})");
  Check(!badLocation.valid && Contains(badLocation.error, "location key"),
        "bad location key is rejected");
  Config badLocationId = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","locations":{"00000001:00000002:00000003":1.5}})");
  Check(!badLocationId.valid && Contains(badLocationId.error, "integer"),
        "non-integer location id is rejected");
  Config unknownItem = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"12":{"item":"Unobtainium"}}})");
  Check(!unknownItem.valid && Contains(unknownItem.error, "Unobtainium"),
        "unknown item name is rejected and named");
  Config defaults = ParseConfig(R"({"server":"ws://localhost","slot":"Default"})");
  Check(defaults.valid && defaults.game == "Metroid Prime" && defaults.password.empty() &&
            defaults.itemsHandling == 7 && defaults.versionMajor == 0 &&
            defaults.versionMinor == 6 && defaults.versionBuild == 8 && defaults.tags.empty() &&
            defaults.tlsCa.empty(),
        "optional configuration defaults apply");
  Config tlsCa = ParseConfig(
      R"({"server":"wss://archipelago.gg:38281","slot":"P","tls_ca":"certs/ca.pem"})");
  Check(tlsCa.valid && tlsCa.tlsCa == "certs/ca.pem", "tls_ca path parses as written");
  Config badTlsCa = ParseConfig(R"({"server":"wss://localhost","slot":"P","tls_ca":true})");
  Check(!badTlsCa.valid && Contains(badTlsCa.error, "tls_ca"),
        "non-string tls_ca is rejected and named");
  Config unknownTop = ParseConfig(R"({"server":"ws://localhost","slot":"P","extra":42})");
  Check(unknownTop.valid, "unknown top-level configuration key is ignored");

  const std::filesystem::path configPath = testDir / "full.json";
  {
    std::ofstream file(configPath);
    file << fullConfig;
  }
  const Config loadedConfig = LoadConfigFile(configPath.string());
  Check(loadedConfig.valid, "configuration loads from a file");
  const Config absentConfig = LoadConfigFile((testDir / "missing.json").string());
  Check(!absentConfig.valid && Contains(absentConfig.error, "missing.json"),
        "missing configuration file reports its path");

  Session session(config, State{});
  int64_t checkedId = -1;
  Check(!session.MarkLocationChecked("unknown", checkedId), "unknown location cannot be checked");
  Check(session.MarkLocationChecked("39F2DE28:B2701146:0000007E", checkedId) &&
            checkedId == 123456,
        "mapped location returns its ID and records it");
  Check(!session.MarkLocationChecked("39F2DE28:B2701146:0000007E", checkedId),
        "a recorded location cannot be checked twice");

  // MarkLocationChecked answers false for two different reasons, and only one is
  // a fault. KnowsLocation is what tells them apart, and the client uses it to
  // report a location the table has never heard of instead of dropping it in
  // silence - a session that collects everything and reports nothing looks, from
  // the player's side, exactly like a session that is working.
  Check(!session.KnowsLocation("unknown"),
        "a location absent from the table is not known");
  Check(session.KnowsLocation("39F2DE28:B2701146:0000007E"),
        "a location in the table stays known after being recorded");
  Check(!session.KnowsLocation(""),
        "an empty key is not known");
  const std::vector<int64_t> allIds = session.AllLocationIds();
  Check(allIds == std::vector<int64_t>({123456, 123457}),
        "all location IDs are deduplicated and sorted");

  const std::string connect = session.BuildConnect();
  Check(Contains(connect, "\"name\":\"Player1\"") &&
            Contains(connect, "\"game\":\"Metroid Prime\"") &&
            Contains(connect, "\"items_handling\":7") &&
            Contains(connect, "\"class\":\"Version\"") &&
            Contains(connect, "\"major\":0") && Contains(connect, "\"minor\":6"),
        "Connect packet contains slot, game, handling and version");
  Check(connect == session.BuildConnect(), "Connect UUID stays stable for the process");
  Check(Session::BuildLocationChecks({7, 8}) ==
            R"({"cmd":"LocationChecks","locations":[7,8]})",
        "LocationChecks packet formatting");
  Check(Session::BuildSync() == R"({"cmd":"Sync"})", "Sync packet formatting");

  State initial;
  initial.checkedLocations.push_back(123456);
  Session packets(config, initial);
  std::vector<std::string> outgoing;
  std::vector<ItemGrant> grants;
  packets.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Alpha"})"),
                       outgoing, grants);
  Check(packets.SeedName() == "MP Seed Alpha", "RoomInfo captures the seed name");
  packets.HandlePacket(Packet(
      R"({"cmd":"Connected","team":0,"slot":3,"players":[{"team":0,"slot":1,"alias":"Bob","name":"Bob's name"},{"team":0,"slot":3,"alias":"","name":"Player1"}],"checked_locations":[123457,123456,999]})"),
      outgoing, grants);
  Check(packets.HandshakeComplete() && packets.SlotDescription() == "slot 3, team 0",
        "Connected completes handshake and records slot description");
  Check(packets.PlayerName(1) == "Bob" && packets.PlayerName(3) == "Player1" &&
            packets.PlayerName(44) == "player 44",
        "Connected captures player aliases, falls back to name, and names unknown slots");
  Check(packets.GetState().checkedLocations == std::vector<int64_t>({123456, 123457, 999}),
        "Connected checked locations merge without duplicates");

  packets.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[{"item":100,"location":1,"player":1,"flags":0}]})"),
      outgoing, grants);
  Check(grants.size() == 1 && grants.back().itemId == 100 &&
            grants.back().itemType == PortRandomizer::ItemFromName("EnergyTanks") &&
            grants.back().amount == 2 && grants.back().capacity == 3 &&
            grants.back().display == "Energy Tank",
        "ReceivedItems object grants configured item fields");
  packets.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":1,"items":[[101,2,3,0]]})"),
      outgoing, grants);
  Check(grants.size() == 2 && grants.back().itemId == 101 &&
            grants.back().itemType == PortRandomizer::ItemFromName("Missiles") &&
            grants.back().amount == 1 && grants.back().capacity == 1 &&
            grants.back().display == "Missiles",
        "ReceivedItems four-element array grants configured item");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[100,1,1,0]]})"),
                      outgoing, grants);
  Check(grants.size() == 2 && packets.GetState().nextItemIndex == 2,
        "already processed item index is not granted twice");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":2,"items":[[999,1,3,0]]})"),
                      outgoing, grants);
  Check(grants.size() == 3 && grants.back().itemType == -1 &&
            packets.LastError() == "unknown item id 999",
        "unknown received item is counted and reported");
  std::string notification;
  Check(packets.TakeNotification(notification) && notification == "Energy Tank from Bob",
        "received item notification includes the sender alias");
  Check(packets.TakeNotification(notification) && notification == "Missiles",
        "received item from our own slot has no sender suffix");
  Check(packets.TakeNotification(notification) && notification == "unknown item 999",
        "unknown item notification names its ID");
  Check(!packets.TakeNotification(notification), "notification queue reports empty after draining");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":5,"items":[[101,2,3,0]]})"),
                      outgoing, grants);
  Check(packets.Desynced() && packets.GetState().nextItemIndex == 3 && grants.size() == 3 &&
            !outgoing.empty() && outgoing.back() == Session::BuildSync(),
        "item index jump marks desync, queues Sync, and holds the items past the gap");
  packets.HandlePacket(
      Packet(R"({"cmd":"ReceivedItems","index":0,"items":)"
             R"([[100,1,1,0],[101,2,3,0],[999,1,3,0],[100,4,5,0],[101,6,7,0],[100,8,9,0]]})"),
      outgoing, grants);
  Check(!packets.Desynced() && packets.GetState().nextItemIndex == 6 && grants.size() == 6 &&
            grants[3].itemId == 100 && grants[4].itemId == 101 && grants[5].itemId == 100,
        "the Sync reply grants the gap and the held items once each");
  while (packets.TakeNotification(notification)) {
  }

  std::string printParts = R"({"cmd":"PrintJSON","data":[{"text":"Hello"},{"text":"\nworld"}]})";
  packets.HandlePacket(Packet(printParts), outgoing, grants);
  Check(packets.LastMessage() == "Hello world", "PrintJSON joins text and flattens newlines");
  Check(packets.TakeNotification(notification) && notification == "Hello world",
        "PrintJSON text is queued as a notification");
  {
    ChatLine line;
    while (packets.TakeChatLine(line)) {
    }
    packets.HandlePacket(
        Packet(R"({"cmd":"PrintJSON","type":"CommandResult","data":[{"text":"line one\nline two\n"}]})"),
        outgoing, grants);
    Check(packets.TakeChatLine(line) && line.type == "CommandResult" &&
              line.text == "line one\nline two" && !packets.TakeChatLine(line),
          "the chat log keeps a reply's line breaks, without the trailing one");
    while (packets.TakeNotification(notification)) {
    }
  }
  Check(Session::BuildSay("hi \"all\"\n") == R"({"cmd":"Say","text":"hi \"all\"\n"})",
        "Say packet formatting escapes the text");
  std::string longText(250, 'x');
  const std::string printLong = "{\"cmd\":\"PrintJSON\",\"data\":[{\"text\":\"" + longText + "\"}]}";
  packets.HandlePacket(Packet(printLong), outgoing, grants);
  Check(packets.LastMessage().size() == 203 && packets.LastMessage().substr(200) == "...",
        "PrintJSON truncates long output and marks the cut");
  Check(packets.TakeNotification(notification) && notification == packets.LastMessage(),
        "truncated PrintJSON message is queued as a notification");

  Session cappedNotifications(config, State{});
  for (int i = 0; i < 40; ++i) {
    const std::string message = "message " + std::to_string(i);
    cappedNotifications.HandlePacket(
        Packet("{\"cmd\":\"PrintJSON\",\"data\":[{\"text\":\"" + message + "\"}]}"),
        outgoing, grants);
  }
  bool newestThirtyTwo = true;
  for (int i = 8; i < 40; ++i) {
    newestThirtyTwo = cappedNotifications.TakeNotification(notification) &&
                      notification == "message " + std::to_string(i) && newestThirtyTwo;
  }
  Check(newestThirtyTwo && !cappedNotifications.TakeNotification(notification),
        "notification queue keeps only the newest 32 entries");
  packets.HandlePacket(Packet(R"({"cmd":"InvalidPacket","text":"bad command"})"), outgoing, grants);
  Check(packets.LastError() == "bad command", "InvalidPacket records text as error");
  packets.HandlePacket(Packet(R"({"cmd":"ConnectionRefused","errors":["bad password","unknown slot"]})"),
                      outgoing, grants);
  Check(!packets.HandshakeComplete() && packets.LastError() == "bad password, unknown slot",
        "ConnectionRefused joins error messages and clears handshake");

  State saved;
  saved.slot = "Player1";
  saved.seed = "MP Seed Alpha";
  saved.nextItemIndex = 23;
  saved.checkedLocations = {8, 10, 12};
  const std::filesystem::path statePath = testDir / "nested" / "archipelago_state.json";
  Check(SaveStateFile(statePath.string(), saved), "state save creates parent directory");
  const State reloaded = LoadStateFile(statePath.string());
  Check(reloaded.slot == saved.slot && reloaded.nextItemIndex == saved.nextItemIndex &&
            reloaded.checkedLocations == saved.checkedLocations && reloaded.seed == saved.seed,
        "saved state round-trips");
  Check(!reloaded.hasLogic, "a state without logic options has none");
  {
    State withLogic = saved;
    withLogic.hasLogic = true;
    withLogic.logic.trickDifficulty = 1;
    withLogic.logic.combatLogic = -1;
    withLogic.logic.progressiveBeams = true;
    withLogic.logic.mainMissile = true;
    withLogic.logic.trickAllow = {"Alcove Escape", "A \"quoted\" trick"};
    withLogic.logic.trickDeny = {"Landing Site Scan Dash"};
    const std::filesystem::path logicPath = testDir / "logic-state.json";
    Check(SaveStateFile(logicPath.string(), withLogic), "state with logic options saves");
    const State logicReloaded = LoadStateFile(logicPath.string());
    Check(logicReloaded.hasLogic && logicReloaded.logic == withLogic.logic &&
              logicReloaded.checkedLocations == saved.checkedLocations,
          "the seed's logic options round-trip through the state file");
  }
  const std::filesystem::path emptyStatePath = testDir / "empty-state.json";
  const State absentState = LoadStateFile(emptyStatePath.string());
  Check(absentState.slot.empty() && absentState.nextItemIndex == 0 &&
            absentState.checkedLocations.empty(),
        "absent state file is an empty state");
  Check(SaveStateFile(emptyStatePath.string(), State{}), "empty state saves");
  const State emptyReloaded = LoadStateFile(emptyStatePath.string());
  Check(emptyReloaded.nextItemIndex == 0 && emptyReloaded.checkedLocations.empty(),
        "empty state round-trips");
  Check(emptyReloaded.progressive.empty() && Contains(Read(emptyStatePath), "\"progressive\":{}"),
        "empty state writes an empty progressive map");

  // The overlay's Connect screen rewrites archipelago.json; the rest of the
  // file is the player's and must survive it.
  {
    const std::filesystem::path connectionPath = testDir / "connect" / "archipelago.json";
    Connection connection;
    connection.server = "archipelago.gg:38281";
    connection.slot = "Samus";
    std::string error;
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "connection saves to a new file");
    const Config fresh = LoadConfigFile(connectionPath.string());
    Check(fresh.valid && fresh.builtin && fresh.server == "archipelago.gg:38281" &&
              fresh.slot == "Samus" && fresh.password.empty(),
          "a saved connection alone is a valid built-in configuration");
    Check(!Contains(Read(connectionPath), "password") && !Contains(Read(connectionPath), "enabled"),
          "an empty password and an enabled connection are not written");

    {
      std::ofstream file(connectionPath, std::ios::binary | std::ios::trunc);
      file << R"({"slot":"Old","death_link":true,"tls_ca":"ca.pem","extra":[1,2.5,null,"x\"y"],)"
              R"("locations":{"39F2DE28:B2701146:0000007E":5031158},"server":"ws://a:1"})";
    }
    connection.password = "pw";
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "connection saves over an existing file");
    const std::string rewritten = Read(connectionPath);
    Check(Contains(rewritten, R"("slot":"Samus")") && Contains(rewritten, R"("death_link":true)") &&
              Contains(rewritten, R"("tls_ca":"ca.pem")") &&
              Contains(rewritten, R"("extra":[1,2.5,null,"x\"y"])") &&
              Contains(rewritten, R"("39F2DE28:B2701146:0000007E":5031158)") &&
              Contains(rewritten, R"("password":"pw")") &&
              rewritten.find("\"slot\"") < rewritten.find("\"death_link\""),
          "other keys and their order survive a connection save");
    const Config merged = LoadConfigFile(connectionPath.string());
    Check(merged.valid && merged.deathLink && merged.password == "pw" &&
              merged.locations.size() == 1,
          "the merged file still parses as the same configuration");

    connection.enabled = false;
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "a disconnect saves");
    const Config disabled = LoadConfigFile(connectionPath.string());
    const Connection kept = LoadConnectionFile(connectionPath.string());
    Check(!disabled.valid && Contains(disabled.error, "enabled"),
          "a disconnected configuration does not start the client");
    Check(!kept.enabled && kept.server == "archipelago.gg:38281" && kept.slot == "Samus" &&
              kept.password == "pw",
          "a disconnected configuration keeps its details for the Connect screen");
    connection.enabled = true;
    Check(SaveConnectionFile(connectionPath.string(), connection, error) &&
              LoadConfigFile(connectionPath.string()).valid,
          "connecting again re-enables it");

    connection.seed = "S1";
    connection.lastPlayed = 1790000000;
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "a connection with a seed saves");
    const Connection seeded = LoadConnectionFile(connectionPath.string());
    Check(seeded.seed == "S1" && seeded.lastPlayed == 1790000000 &&
              Contains(Read(connectionPath), R"("last_played":1790000000)"),
          "the seed and the last played time round-trip");
    connection.seed.clear();
    connection.lastPlayed = 0;
    Check(SaveConnectionFile(connectionPath.string(), connection, error) &&
              !Contains(Read(connectionPath), "seed") &&
              !Contains(Read(connectionPath), "last_played"),
          "an unknown seed and time are removed, not written empty");

    {
      std::ofstream file(connectionPath, std::ios::binary | std::ios::trunc);
      file << "{ not json";
    }
    Check(!SaveConnectionFile(connectionPath.string(), connection, error) && !error.empty() &&
              Read(connectionPath) == "{ not json",
          "a broken file is reported, not overwritten");
    const Connection absent = LoadConnectionFile((testDir / "none.json").string());
    Check(absent.server.empty() && absent.slot.empty() && absent.enabled,
          "an absent file has no connection");
  }

  // Each slot's game in each seed has a directory: plain names stay readable,
  // anything else is made safe and told apart by a hash.
  {
    Check(GameDirectoryName("Samus", "12345678901234567890") == "Samus-12345678901234567890",
          "a plain slot and seed name the directory as they are");
    const std::string odd = GameDirectoryName("Sa/mus", "..");
    Check(odd.find('/') == std::string::npos && odd.rfind("Sa_mus-_..-", 0) == 0 &&
              odd.size() == std::string("Sa_mus-_..-").size() + 8,
          "separators and dot names are replaced, with a hash appended");
    Check(GameDirectoryName("a/b", "s") != GameDirectoryName("a?b", "s"),
          "names that clean to the same text stay apart");
    Check(GameDirectoryName(std::string(60, 'x'), "s").size() == 40 + 3 + 8,
          "a long slot is cut to 40 bytes");
  }

  // Progress belongs to the session that granted it. A different seed must not
  // inherit it, or the client claims locations it never collected and skips the
  // items the server still owes it.
  {
    const std::string configText = R"json({
      "server":"ws://localhost:38281", "slot":"P",
      "items":{"5031004":{"item":"Missiles","amount":5,"capacity":5}},
      "locations":{"39F2DE28:B2701146:0000007E":5031101}
    })json";
    const Config config = ParseConfig(configText);
    Check(config.valid, "seed-mismatch config parses");

    State carried;
    carried.slot = "P";
    carried.seed = "MP Seed Alpha";
    carried.nextItemIndex = 7;
    carried.checkedLocations = {5031101};
    carried.progressive[5031043] = 2;
    Session carriedSession(config, carried);
    std::vector<std::string> seedOutgoing;
    std::vector<ItemGrant> seedGrants;
    carriedSession.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Beta"})"),
                                seedOutgoing, seedGrants);
    const State& afterSwitch = carriedSession.GetState();
    Check(afterSwitch.nextItemIndex == 0 && afterSwitch.checkedLocations.empty() &&
              afterSwitch.progressive.empty(),
          "a different seed discards the recorded checks, item index and progressive counts");
    Check(afterSwitch.seed == "MP Seed Beta", "the new seed is recorded");
    Check(carriedSession.SeedName() == "MP Seed Beta" && carriedSession.SeedName() != "",
          "SeedName still reports the server's seed");
    Check(carriedSession.ResetReason().find("MP Seed Alpha") != std::string::npos &&
              carriedSession.ResetReason().find("MP Seed Beta") != std::string::npos,
          "the reset says which seed the progress belonged to");
    const std::filesystem::path switchedPath = testDir / "switched-state.json";
    Check(SaveStateFile(switchedPath.string(), afterSwitch) &&
              LoadStateFile(switchedPath.string()).nextItemIndex == 0,
          "the reset progress is what gets written back");

    Session sameSession(config, carried);
    std::vector<std::string> sameOutgoing;
    std::vector<ItemGrant> sameGrants;
    sameSession.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Alpha"})"), sameOutgoing,
                             sameGrants);
    const State& afterSame = sameSession.GetState();
    Check(afterSame.nextItemIndex == 7 && afterSame.checkedLocations ==
                std::vector<int64_t>({5031101}) && afterSame.progressive.count(5031043) == 1 &&
              afterSame.progressive.at(5031043) == 2,
          "the same seed keeps the recorded progress");
    Check(sameSession.ResetReason().empty(), "no reset is reported when the seed matches");

    State legacy;
    legacy.slot = "P";
    legacy.nextItemIndex = 4;
    legacy.checkedLocations = {5031101};
    Session legacySession(config, legacy);
    std::vector<std::string> legacyOutgoing;
    std::vector<ItemGrant> legacyGrants;
    legacySession.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Alpha"})"),
                               legacyOutgoing, legacyGrants);
    Check(legacySession.GetState().nextItemIndex == 4 &&
              legacySession.GetState().seed == "MP Seed Alpha" && legacySession.ResetReason().empty(),
          "a state file with no recorded seed adopts the server's without discarding progress");

    // A file written before the seed existed still loads, with an unknown seed.
    const std::filesystem::path legacyPath = testDir / "legacy-state.json";
    {
      std::ofstream legacyFile(legacyPath, std::ios::binary | std::ios::trunc);
      legacyFile << R"({"slot":"P","next_item_index":5,"checked_locations":[5031101]})";
    }
    const State legacyLoaded = LoadStateFile(legacyPath.string());
    Check(legacyLoaded.seed.empty() && legacyLoaded.nextItemIndex == 5,
          "a pre-seed state file loads with an unknown seed and keeps its progress");
  }

  const std::string progressiveConfigText = R"json({
    "server":"ws://localhost", "slot":"P",
    "items":{
      "5031004":{"item":"Missiles","amount":5,"capacity":5,"display":"Missile Expansion"},
      "5031043":{"progressive":[
        {"item":"PowerBeam","amount":1,"capacity":1,"display":"Power Beam"},
        {"item":"ChargeBeam","amount":1,"capacity":1,"display":"Charge Beam"},
        {"item":"SuperMissile","amount":1,"capacity":1,"display":"Super Missile"}]},
      "5031047":{"item":"ChargeBeam","display":"Charge Beam (Power)"}
    }
  })json";
  const Config progressiveConfig = ParseConfig(progressiveConfigText);
  Check(progressiveConfig.valid && progressiveConfig.items.size() == 3,
        "progressive configuration parses");
  const ItemEntry& powerEntry = progressiveConfig.items.at(5031043);
  Check(powerEntry.IsProgressive() && powerEntry.progressive.size() == 3 &&
            powerEntry.progressive[0].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
            powerEntry.progressive[1].itemType == PortRandomizer::ItemFromName("ChargeBeam") &&
            powerEntry.progressive[2].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            powerEntry.progressive[2].display == "Super Missile" &&
            powerEntry.progressive[2].itemId == 5031043,
        "progressive entry keeps its steps in order");
  Check(powerEntry.itemType == powerEntry.progressive[0].itemType &&
            powerEntry.display == "Power Beam",
        "progressive entry's flat fields mirror step 0");
  Check(!progressiveConfig.items.at(5031004).IsProgressive() &&
            progressiveConfig.items.at(5031004).amount == 5 &&
            progressiveConfig.items.at(5031047).itemType ==
                PortRandomizer::ItemFromName("ChargeBeam"),
        "flat entries still parse alongside progressive ones");

  Session progressive(progressiveConfig, State{});
  std::vector<ItemGrant> progressiveGrants;
  progressive.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],[5031004,2,1,0],[5031043,3,1,0]]})"),
      outgoing, progressiveGrants);
  progressive.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":3,"items":[[999,4,1,0],[5031043,5,1,0]]})"),
      outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 5 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
            progressiveGrants[0].display == "Power Beam" &&
            progressiveGrants[1].itemType == PortRandomizer::ItemFromName("Missiles") &&
            progressiveGrants[1].amount == 5 &&
            progressiveGrants[2].itemType == PortRandomizer::ItemFromName("ChargeBeam") &&
            progressiveGrants[2].display == "Charge Beam" &&
            progressiveGrants[3].itemType == -1 &&
            progressiveGrants[4].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            progressiveGrants[4].display == "Super Missile" &&
            progressiveGrants[4].itemId == 5031043,
        "progressive copies grant steps 1, 2 and 3 in order");
  Check(progressive.GetState().progressive.size() == 1 &&
            progressive.GetState().progressive.at(5031043) == 3,
        "progressive count tracks copies and ignores flat and unknown ids");
  const char* expectedNotifications[] = {"Power Beam", "Missile Expansion", "Charge Beam",
                                         "unknown item 999", "Super Missile"};
  bool notificationsMatch = true;
  for (const char* expected : expectedNotifications)
    notificationsMatch = progressive.TakeNotification(notification) &&
                         notification == expected && notificationsMatch;
  Check(notificationsMatch, "notifications name the progressive step actually granted");

  // The tracker keeps the session's receipts so one that arrived while the
  // player was not watching the HUD is still readable. It is the same five
  // grants, with the step of a progressive sequence and who sent it.
  const std::vector< TrackedItem >& tracked = progressive.Tracked();
  Check(tracked.size() == 5, "the tracker records every grant, including unknown items");
  Check(tracked[0].name == "Power Beam" && tracked[0].step == 1 && tracked[0].total == 3 &&
            tracked[1].name == "Missile Expansion" && tracked[1].total == 1 &&
            tracked[2].name == "Charge Beam" && tracked[2].step == 2 && tracked[2].total == 3 &&
            tracked[3].name == "item 999" && tracked[4].name == "Super Missile" &&
            tracked[4].step == 3 && tracked[4].total == 3,
        "the tracker names each grant and its progressive step");
  bool noSender = true;
  for (const TrackedItem& item : tracked)
    noSender = item.from.empty() && noSender;
  Check(noSender, "items the player sent themselves have no sender");
  // A copy past the last step repeats it rather than counting past the end.
  // A fresh session, because the state carries the count of 3 across.
  {
    Session repeated(progressiveConfig, State{});
    std::vector<ItemGrant> repeatedGrants;
    repeated.HandlePacket(Packet(
        R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],[5031043,2,1,0],)"
        R"([5031043,3,1,0],[5031043,4,1,0],[5031043,5,1,0]]})"),
        outgoing, repeatedGrants);
    const std::vector< TrackedItem >& more = repeated.Tracked();
    Check(more.size() == 5 && more[0].step == 1 && more[2].step == 3 && more[3].step == 3 &&
              more[4].step == 3 && more[4].name == "Super Missile",
          "copies past the last step stay on it");
  }
  {
    // A loaded save holding the first two of five received items: the session
    // rewinds, and the full replay grants only the three the save is missing,
    // with the progressive steps counted from the items it already holds.
    const std::string allFive = R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],)"
                                R"([5031004,2,1,0],[5031043,3,1,0],[999,4,1,0],[5031043,5,1,0]]})";
    Session rewound(progressiveConfig, State{});
    std::vector<ItemGrant> firstRun;
    rewound.HandlePacket(Packet(allFive), outgoing, firstRun);
    Check(firstRun.size() == 5 && firstRun[0].index == 0 && firstRun[4].index == 4,
          "grants carry their received-item index");
    while (rewound.TakeNotification(notification)) {
    }
    rewound.RewindTo(2);
    Check(rewound.GetState().nextItemIndex == 0 && rewound.GetState().progressive.empty() &&
              rewound.Tracked().empty(),
          "a rewind clears the item index, progressive counts and tracker");
    std::vector<ItemGrant> replayed;
    outgoing.clear();
    rewound.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":5,"items":[[5031004,6,1,0]]})"),
                         outgoing, replayed);
    Check(replayed.empty() && !outgoing.empty() && outgoing.back() == Session::BuildSync(),
          "a live item before the replay waits for the Sync reply");
    rewound.HandlePacket(Packet(allFive), outgoing, replayed);
    Check(replayed.size() == 3 && replayed[0].index == 2 &&
              replayed[0].itemType == PortRandomizer::ItemFromName("ChargeBeam") &&
              replayed[1].index == 3 && replayed[1].itemType == -1 && replayed[2].index == 4 &&
              replayed[2].itemType == PortRandomizer::ItemFromName("SuperMissile"),
          "the replay grants only the items the save is missing, at the right steps");
    Check(rewound.GetState().nextItemIndex == 5 &&
              rewound.GetState().progressive.at(5031043) == 3 && rewound.Tracked().size() == 5,
          "the replay restores the index, progressive count and tracker");
    int replayNotes = 0;
    while (rewound.TakeNotification(notification))
      ++replayNotes;
    Check(replayNotes == 3, "only the regranted items are announced");
  }
  {
    // Another player's item records who sent it, using the alias from Connected.
    Session shared(progressiveConfig, State{});
    std::vector<ItemGrant> sharedGrants;
    shared.HandlePacket(Packet(
                            R"({"cmd":"Connected","slot":1,"team":0,"players":)"
                            R"([{"slot":1,"alias":"Me"},{"slot":2,"alias":"Bob"}]})"),
                        outgoing, sharedGrants);
    shared.HandlePacket(
        Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[5031004,1,2,0],[5031004,2,1,0]]})"),
        outgoing, sharedGrants);
    const std::vector< TrackedItem >& fromOthers = shared.Tracked();
    Check(fromOthers.size() == 2 && fromOthers[0].from == "Bob" && fromOthers[1].from.empty(),
          "an item from another player names them, one's own does not");
  }

  // DeathLink. A bounce from the server is owed to the game rather than applied
  // here, so one that arrives while nothing is running is not lost, and it is
  // cleared once taken so it is not applied twice.
  {
    const std::string deathConfigText = R"json({
      "server":"ws://localhost", "slot":"P", "death_link":true,
      "items":{"5031004":{"item":"Missiles","amount":5,"capacity":5}}
    })json";
    const Config deathConfig = ParseConfig(deathConfigText);
    Check(deathConfig.valid && deathConfig.deathLink, "death_link parses and is on");
    const Config noDeath = ParseConfig(R"json({"server":"ws://localhost","slot":"P"})json");
    Check(noDeath.valid && !noDeath.deathLink, "death_link is off unless asked for");
    const Config badDeath = ParseConfig(R"json({"server":"w","slot":"P","death_link":"yes"})json");
    Check(!badDeath.valid && badDeath.error.find("death_link") != std::string::npos,
          "a non-boolean death_link is rejected with a reason");

    Session deaths(deathConfig, State{});
    std::vector<ItemGrant> deathGrants;
    deaths.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":)"
                               R"([{"slot":1,"alias":"Me"},{"slot":2,"alias":"Bob"}]})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 0, "no death is owed before a bounce arrives");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],)"
                               R"("data":{"time":1.5,"source":"Bob"}})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 1, "a bounce from another player is owed to the game");
    Check(deaths.LastDeathSource() == "Bob", "the bounce names who died");
    Check(deaths.TakeDeathPending() == 1 && deaths.DeathsPending() == 0,
          "taking a death clears it so it is not applied twice");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],)"
                               R"("data":{"time":2.5,"source":"P"}})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 0, "a bounce from this client is not a death of its own");
    Check(deaths.TakeNotification(notification) && notification.find("Bob") != std::string::npos,
          "a bounce raises a notification naming who died");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["Tracker"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","data":{"source":"Bob"}})"), outgoing,
                        deathGrants);
    deaths.HandlePacket(Packet(R"({"cmd":"Bounce","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 0,
          "only a Bounced carrying the DeathLink tag is a death");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],)"
                               R"("data":{"time":3,"source":"Bob","cause":"Bob fell"}})"),
                        outgoing, deathGrants);
    Check(deaths.TakeNotification(notification) && notification == "Bob fell",
          "a bounce with a cause shows the cause");
    deaths.TakeDeathPending();

    // Two bounces before the game runs are two deaths, not one.
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    Check(deaths.TakeDeathPending() == 2, "bounces that arrive together are all owed");

    Session deathsOff(noDeath, State{});
    deathsOff.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                           outgoing, deathGrants);
    Check(deathsOff.DeathsPending() == 0, "a client without death_link ignores bounces");

    const std::string bounce = deaths.BuildBounce();
    Check(bounce.find("\"cmd\":\"Bounce\"") != std::string::npos &&
              bounce.find("\"tags\":[\"DeathLink\"]") != std::string::npos &&
              bounce.find("\"time\":") != std::string::npos &&
              bounce.find("\"source\":\"P\"") != std::string::npos &&
              bounce.find("\"cause\":\"P died\"") != std::string::npos,
          "an enabled client builds a DeathLink Bounce with time, source and cause");
    PortJson::Value bounceJson;
    size_t bounceOffset = 0;
    const char* bounceReason = nullptr;
    Check(PortJson::Parse(bounce, bounceJson, bounceOffset, &bounceReason),
          "the built Bounce is valid JSON");
    Check(deaths.BuildConnect().find("\"tags\":[\"DeathLink\"]") != std::string::npos,
          "a DeathLink client connects with the DeathLink tag");
    Check(Session(noDeath, State{}).BuildConnect().find("DeathLink") == std::string::npos,
          "a client without death_link does not carry the tag");
    Check(Session(noDeath, State{}).BuildBounce().empty(),
          "a client without death_link sends no Bounce");
  }

  const std::filesystem::path progressiveStatePath = testDir / "progressive-state.json";
  State progressiveState = progressive.GetState();
  progressiveState.slot = "P";
  Check(SaveStateFile(progressiveStatePath.string(), progressiveState) &&
            Contains(Read(progressiveStatePath), "\"progressive\":{\"5031043\":3}"),
        "progressive counts are written to the state file");
  const State progressiveReloaded = LoadStateFile(progressiveStatePath.string());
  Check(progressiveReloaded.nextItemIndex == 5 &&
            progressiveReloaded.progressive == progressiveState.progressive,
        "progressive counts round-trip through the state file");

  Session resumed(progressiveConfig, progressiveReloaded);
  progressiveGrants.clear();
  resumed.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],[5031004,2,1,0],[5031043,3,1,0],[999,4,1,0],[5031043,5,1,0],[5031043,6,1,0]]})"),
      outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 1 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            progressiveGrants[0].display == "Super Missile" &&
            resumed.GetState().progressive.at(5031043) == 4,
        "after reload a 4th copy stays at the last step and old copies are skipped");

  State stale;
  stale.progressive[5031043] = 2;
  Session fresh(progressiveConfig, stale);
  progressiveGrants.clear();
  fresh.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0]]})"),
                     outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 1 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
            fresh.GetState().progressive.at(5031043) == 1,
        "a fresh inventory restarts progressive counts");

  State saturated;
  saturated.progressive[5031043] = std::numeric_limits<int64_t>::max();
  saturated.nextItemIndex = 1;
  Session capped(progressiveConfig, saturated);
  progressiveGrants.clear();
  capped.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":1,"items":[[5031043,1,1,0]]})"),
                      outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 1 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            capped.GetState().progressive.at(5031043) == std::numeric_limits<int64_t>::max(),
        "progressive count saturates instead of overflowing");

  const std::filesystem::path oldStatePath = testDir / "old-state.json";
  {
    std::ofstream file(oldStatePath);
    file << R"({"slot":"P","next_item_index":4,"checked_locations":[1]})";
  }
  const State oldState = LoadStateFile(oldStatePath.string());
  Check(oldState.nextItemIndex == 4 && oldState.progressive.empty(),
        "a state file without progressive counts loads with zero counts");

  const Config emptyProgressive = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031043":{"progressive":[]}}})");
  Check(!emptyProgressive.valid && Contains(emptyProgressive.error, "5031043") &&
            Contains(emptyProgressive.error, "progressive"),
        "empty progressive list is rejected and names the id");
  const Config badStep = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031044":{"progressive":[{"item":"IceBeam"},{"amount":1}]}}})");
  Check(!badStep.valid && Contains(badStep.error, "5031044") &&
            Contains(badStep.error, "step 2"),
        "progressive step without an item name is rejected and named");
  const Config unknownStep = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031045":{"progressive":[{"item":"Unobtainium"}]}}})");
  Check(!unknownStep.valid && Contains(unknownStep.error, "Unobtainium"),
        "unknown item name in a progressive step is rejected");
  const Config neither = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031046":{"display":"Nothing"}}})");
  Check(!neither.valid && Contains(neither.error, "5031046"),
        "entry with neither item nor progressive is rejected and names the id");
  const Config both = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031046":{"item":"PlasmaBeam","progressive":[{"item":"PlasmaBeam"}]}}})");
  Check(!both.valid && Contains(both.error, "5031046"),
        "entry with both item and progressive is rejected");

  // Built-in Metroid Prime tables: a config with only the connection details.
  const Config builtin = ParseConfig(R"({"server":"ws://localhost","slot":"P"})");
  Check(builtin.valid && builtin.builtin, "a bare Metroid Prime config uses the built-in tables");
  Check(builtin.locations.size() == 100 &&
            builtin.locations.count("39F2DE28:B2701146:0000007E") == 1 &&
            builtin.locations.at("39F2DE28:B2701146:0000007E") == 5031158,
        "built-in locations are keyed world:area:pickup");
  Check(builtin.items.count(5031004) == 1 && builtin.items.count(5031049) == 1 &&
            builtin.items.at(5031049).IsProgressive(),
        "built-in items include expansions and progressive beams");
  Check(!ParseConfig(R"({"server":"ws://localhost","slot":"P","game":"Other"})").builtin &&
            !config.builtin,
        "other games and hand-written tables do not get the built-in tables");
  const Config ownLocations = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","locations":{"39F2DE28:B2701146:0000007E":5031101}})");
  Check(ownLocations.valid && ownLocations.builtin && ownLocations.locations.size() == 1 &&
            ownLocations.locations.at("39F2DE28:B2701146:0000007E") == 5031101 &&
            builtin.items.size() == ownLocations.items.size(),
        "a config with only locations keeps them and takes the built-in items");
  {
    Session session(builtin, State());
    Check(Contains(session.BuildConnect(), "\"slot_data\":true"),
          "the built-in tables ask for slot_data");
    Check(Contains(Session(config, State()).BuildConnect(), "\"slot_data\":false"),
          "a hand-written config does not ask for slot_data");
    Check(Contains(Session::BuildGoal(), "\"status\":30"), "goal is StatusUpdate 30");
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"missile_launcher":1,"main_power_bomb":0,
        "death_link":1,"elevator_randomization":true,"starting_room_name":"Arboretum",
        "remove_hive_mecha":1,
        "spring_ball":1,"non_varia_heat_damage":1,"required_artifacts":12,
        "etank_capacity":100,"shuffle_unlimited_missiles":0,"pre_scan_elevators":1}})"),
                         outgoing, grants);
    const SlotData& slot = session.GetSlotData();
    Check(slot.received && slot.requireMissileLauncher && !slot.requireMainPowerBomb,
          "slot_data main-item requirements parse");
    Check(slot.variaOnlyHeat, "non_varia_heat_damage parses");
    Check(slot.springBall == 1, "spring_ball parses");
    Check(slot.preScanElevators, "pre_scan_elevators parses");
    Check(session.GetState().hasLogic && session.GetState().logic.mainMissile &&
              !session.GetState().logic.mainPowerBomb && session.GetState().logic.variaOnlyHeat &&
              session.GetState().logic.preScanElevators &&
              session.GetState().logic.trickDifficulty == -1,
          "slot_data fills the logic options");
    Check(slot.warnings.empty() && session.GetState().hasWorld &&
              session.GetState().world.startRoom == "Arboretum" &&
              session.GetState().world.removeHiveMecha &&
              !session.GetState().world.backwardsLowerMines &&
              session.GetState().world.etankCapacity == 100,
          "the seed's options are taken without a warning");
    Check(session.GetConfig().deathLink && outgoing.size() == 4 &&
              Contains(outgoing[0], "ConnectUpdate") && Contains(outgoing[0], "DeathLink"),
          "the seed's DeathLink option adds the tag");
    Check(outgoing.size() == 4 && Contains(outgoing[1], "\"cmd\":\"LocationScouts\"") &&
              Contains(outgoing[1], "5031100") && Contains(outgoing[1], "5031199") &&
              Contains(outgoing[1], "\"create_as_hint\":0"),
          "the built-in tables scout every location without hinting");
    Check(outgoing.size() == 4 && outgoing[2] == R"({"cmd":"Get","keys":["_read_hints_0_1"]})" &&
              outgoing[3] == R"({"cmd":"SetNotify","keys":["_read_hints_0_1"]})",
          "and read and follow this slot's hints");
    outgoing.clear();
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031004,"location":1,"player":1,"flags":0},
        {"item":5031043,"location":2,"player":1,"flags":0},
        {"item":5031004,"location":3,"player":1,"flags":0},
        {"item":5031049,"location":4,"player":1,"flags":0},
        {"item":5031049,"location":5,"player":1,"flags":0},
        {"item":5031041,"location":6,"player":1,"flags":0}]})"),
                         outgoing, grants);
    const int missiles = PortRandomizer::ItemFromName("Missiles");
    Check(grants.size() == 6 && grants[0].itemType == missiles && grants[0].capacity == 0 &&
              grants[1].itemType == missiles && grants[1].capacity == 10 &&
              grants[1].amount == 10 && grants[2].capacity == 5,
          "missile capacity waits for the launcher when the seed requires it");
    Check(grants.size() == 6 && grants[3].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
              grants[4].itemType == PortRandomizer::ItemFromName("ChargeBeam"),
          "progressive power beam steps to the charge beam");
    Check(grants.size() == 6 && grants[5].itemType < 0 && session.ReceivedCount(5031041) == 1,
          "unlimited missiles is counted, not granted");
  }
  {
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"missile_launcher":0,"main_power_bomb":0}})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031007,"location":1,"player":1,"flags":0},
        {"item":5031007,"location":2,"player":1,"flags":0},
        {"item":5031044,"location":3,"player":1,"flags":0}]})"),
                         outgoing, grants);
    Check(grants.size() == 3 && grants[0].capacity == 4 && grants[1].capacity == 1 &&
              grants[2].capacity == 1,
          "without the main requirement the first expansion carries the main amount");
    Check(outgoing.size() == 3 && !Contains(outgoing[0], "ConnectUpdate"),
          "no DeathLink update when the seed has it off");
    Check(session.GetSlotData().warnings.empty() && session.GetSlotData().springBall == 0,
          "a seed without spring_ball has none and gets no warning");
    Check(!session.GetSlotData().preScanElevators, "a seed without pre_scan_elevators scans nothing");
  }
  {
    // The log a bug report needs (#136): the handshake and seed options, each
    // received item with what it grants, the rewind a loaded game asks for, and
    // which items the replay only counts.
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    std::vector<std::string> log;
    const auto drain = [&] {
      for (std::string line; session.TakeLogLine(line);)
        log.push_back(line);
    };
    const auto logged = [&](const std::string& part) {
      return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return Contains(line, part); });
    };
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[5031100],"slot_data":{"missile_launcher":1,"main_power_bomb":0}})"),
                         outgoing, grants);
    drain();
    Check(logged("connected as slot 1, team 0; the server has 1 location(s) checked") &&
              logged("missile_launcher=1 main_power_bomb=0"),
          "the log names the slot, the checks the server has and the seed's options");
    const std::string both = R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031000,"location":-1,"player":0,"flags":0},
        {"item":5031021,"location":5,"player":1,"flags":0}]})";
    session.HandlePacket(Packet(both), outgoing, grants);
    session.RewindTo(1);
    log.clear();
    drain();
    Check(logged("rewinding: the loaded game holds the first 1 item(s)"), "a rewind is logged");
    session.HandlePacket(Packet(both), outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":3,"items":[
        {"item":5031043,"location":7,"player":1,"flags":1}]})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":2,"items":[
        {"item":5031043,"location":7,"player":1,"flags":1},
        {"item":12345,"location":8,"player":1,"flags":0}]})"),
                         outgoing, grants);
    drain();
    Check(logged("received 2 item(s) from #0 (next expected #0, the loaded game holds the first 1)"),
          "each ReceivedItems is logged with the index it starts at and the one expected");
    Check(logged("item #0 5031000 Power Beam") && logged("(the loaded game holds it already: counted, not granted)"),
          "an item the loaded game holds is logged as counted, not granted");
    Check(logged("item #3 is not the next one (#2): asking the server to resend everything; these wait"),
          "a gap is logged with what happens to the items past it");
    Check(logged("item #2 5031043 Missile Launcher: game item ") && logged("capacity +5, amount +5"),
          "a received Missile Launcher is logged with what it grants");
    Check(logged("item #3 12345: not an item of this game's table, nothing granted"),
          "an unknown item is logged as granting nothing");
  }
  {
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"spring_ball":3}})"),
                         outgoing, grants);
    const SlotData& slot = session.GetSlotData();
    Check(slot.springBall == 3 && slot.warnings.empty(),
          "Spring Ball as a progressive item is supported");
  }
  {
    const Config optedOut =
        ParseConfig(R"({"server":"ws://localhost","slot":"P","death_link":false})");
    Session session(optedOut, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"death_link":true}})"),
                         outgoing, grants);
    Check(outgoing.size() == 3 && !Contains(outgoing[0], "ConnectUpdate") &&
              !session.GetConfig().deathLink,
          "archipelago.json's death_link overrides the seed's");
  }
  {
    // Names from DataPackage, what sits at each location from LocationInfo,
    // and no second HUD line for a find the pickup already announced.
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,
        "players":[{"team":0,"slot":1,"alias":"Samus","name":"Samus"},
                   {"team":0,"slot":2,"alias":"Link","name":"Link"}],
        "slot_info":{"1":{"name":"Samus","game":"Metroid Prime"},
                     "2":{"name":"Link","game":"A Link to the Past"}},
        "checked_locations":[],"slot_data":{}})"),
                         outgoing, grants);
    Check(!outgoing.empty() && Contains(outgoing[0], "\"cmd\":\"GetDataPackage\"") &&
              Contains(outgoing[0], "A Link to the Past") && Contains(outgoing[0], "Metroid Prime"),
          "Connected asks for every game's names");
    session.HandlePacket(Packet(R"({"cmd":"DataPackage","data":{"games":{
        "A Link to the Past":{"item_name_to_id":{"Hookshot":10},
                              "location_name_to_id":{"Link's House":20}}}}})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"LocationInfo","locations":[
        {"item":10,"location":5031158,"player":2,"flags":1},
        {"item":5031024,"location":5031100,"player":1,"flags":1}]})"),
                         outgoing, grants);
    Check(session.LocationText(5031158) == "Found Hookshot for Link" &&
              session.LocationText(5031100) == "Found Energy Tank" &&
              session.LocationText(5031101).empty(),
          "scouted locations read as what they hold and for whom");
    Check(session.ItemName(10, 2) == "Hookshot" && session.ItemName(99, 2) == "item 99" &&
              session.LocationName(5031158, 1) == "Tallon Overworld: Landing Site",
          "item and location names follow the receiving slot's game");
    int64_t scoutedItem = 0;
    bool sameGame = true;
    int64_t scoutedFlags = 0;
    Check(session.ScoutedAt(5031158, scoutedItem, sameGame, &scoutedFlags) && scoutedItem == 10 &&
              !sameGame && scoutedFlags == 1,
          "another game's item is scouted as such, with its classification");
    Check(session.ScoutedAt(5031100, scoutedItem, sameGame) && scoutedItem == 5031024 &&
              sameGame && !session.ScoutedAt(5031101, scoutedItem, sameGame),
          "an own item is scouted as this game's; an unscouted location isn't");
    Check(session.ScanText(5031158) == "Hookshot\nfor Link (A Link to the Past)" &&
              session.ScanText(5031100) == "Energy Tank\nfor you" &&
              session.ScanText(5031101).empty(),
          "a pickup scans as what it holds, and for whom when that's another player");
    session.HandlePacket(Packet(R"({"cmd":"DataPackage","data":{"games":{
        "A Link to the Past":{"item_name_to_id":{"Bow & Arrows":11}}}}})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"LocationInfo","locations":[
        {"item":11,"location":5031103,"player":2,"flags":1}]})"),
                         outgoing, grants);
    Check(session.ScanText(5031103) == "Bow && Arrows\nfor Link (A Link to the Past)",
          "an '&' in a name is escaped for the game's text markup");
    bool askedHints = false;
    for (const std::string& packet : outgoing)
      askedHints = askedHints || (Contains(packet, "\"cmd\":\"Get\"") && Contains(packet, "_read_hints_0_1"));
    Check(askedHints, "Connected asks for this slot's hints");
    {
      using namespace PortAp::MetroidPrime;
      const int64_t truth = kItemBase + kArtifactTruth;
      const int64_t strength = kItemBase + kArtifactTruth + 1;
      const int64_t elder = kItemBase + kArtifactTruth + 2;
      session.HandlePacket(Packet(R"({"cmd":"Retrieved","keys":{"_read_hints_0_1":[
          {"receiving_player":1,"finding_player":2,"location":20,"item":)" +
                                  std::to_string(truth) + R"(,"found":false},
          {"receiving_player":2,"finding_player":1,"location":5031100,"item":10,"found":false}]}})"),
                           outgoing, grants);
      session.HandlePacket(Packet(R"({"cmd":"LocationInfo","locations":[
          {"item":)" + std::to_string(strength) + R"(,"location":5031102,"player":1,"flags":1}]})"),
                           outgoing, grants);
      const std::string truthHint = session.ArtifactHint(truth);
      Check(Contains(truthHint, "#d4cc33;Link's&pop;") && Contains(truthHint, "#89a1ff;Link's House&pop;") &&
                Contains(truthHint, ItemName(truth)),
            "a hinted artifact names the player and location it's at");
      Check(Contains(session.ArtifactHint(strength), "#d4cc33;your&pop;"),
            "an artifact in this world is known from the scouts");
      Check(Contains(session.ArtifactHint(elder), "has not been collected."),
            "an unknown artifact keeps the AP world's fallback text");
      Check(session.ArtifactHint(10).empty(), "only this game's items get a totem hint");
    }
    {
      using namespace PortAp::MetroidPrime;
      Check(PickupModelKey(kItemBase + 16) == 16 && PickupModelKey(kItemBase + 42) == 7 &&
                PickupModelKey(kItemBase + 44) == kModelMainPowerBomb &&
                PickupModelKey(kItemBase + 54) == 6 && PickupModelKey(kItemBase + 57) == 10 &&
                PickupModelKey(kItemBase + 99) == kModelOtherGame &&
                PickupModelKey(10) == kModelOtherGame &&
                PickupModelKey(kItemBase + kSpringBall) == kModelOtherProgression &&
                PickupModelKey(kItemBase + 49) == 11,
            "AP items map to the retail pickup that shows them");
      Check(OtherGameModelKey(1) == kModelOtherProgression &&
                OtherGameModelKey(3) == kModelOtherProgression &&
                OtherGameModelKey(2) == kModelOtherUseful && OtherGameModelKey(0) == kModelOtherGame &&
                OtherGameModelKey(4) == kModelOtherGame,
            "other games' items look like Cog, Zoomer or Nothing by classification");
    }
    std::string notification;
    while (session.TakeNotification(notification)) {
    }
    session.HandlePacket(Packet(R"j({"cmd":"PrintJSON","type":"ItemSend","receiving":2,
        "item":{"item":10,"location":20,"player":2,"flags":1},"data":[
        {"type":"player_id","text":"2"},{"text":" sent "},
        {"type":"item_id","text":"10","player":2,"flags":1},{"text":" to "},
        {"type":"player_id","text":"1"},{"text":" ("},
        {"type":"location_id","text":"20","player":2},{"text":")"}]})j"),
                         outgoing, grants);
    Check(session.TakeNotification(notification) &&
              notification == "Link sent Hookshot to Samus (Link's House)",
          "PrintJSON ids read as player, item and location names");
    Check(session.AnnounceLocation(5031158) == "Found Hookshot for Link",
          "the pickup announces its scouted item");
    session.HandlePacket(Packet(R"({"cmd":"PrintJSON","type":"ItemSend","receiving":2,
        "item":{"item":10,"location":5031158,"player":1,"flags":1},"data":[
        {"type":"player_id","text":"1"},{"text":" sent "},
        {"type":"item_id","text":"10","player":2,"flags":1}]})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031024,"location":5031100,"player":1,"flags":1}]})"),
                         outgoing, grants);
    Check(session.TakeNotification(notification) && notification == "Energy Tank" &&
              !session.TakeNotification(notification),
          "a find the pickup did not announce is still shown, an announced one is not");
    Check(session.LastMessage() == "Samus sent Hookshot",
          "an announced find still reaches the overlay's last message");
    ChatLine line;
    Check(session.TakeChatLine(line) && line.type == "ItemSend" &&
              line.text == "Link sent Hookshot to Samus (Link's House)" &&
              session.TakeChatLine(line) && line.text == "Samus sent Hookshot" &&
              !session.TakeChatLine(line),
          "the chat log keeps every PrintJSON, announced finds included");
  }

  {
    // The seed's layout: slot_data -> Layout -> the saved copy and back.
    PortJson::Value slotData;
    size_t offset = 0;
    const char* reason = nullptr;
    Check(PortJson::Parse(R"({"starting_room_name":"Arboretum","final_bosses":1,
        "elevator_mapping":{"Tallon Overworld":{
          "Transport to Chozo Ruins West":"Transport to Magmoor Caverns East"},
          "Chozo Ruins":{"Transport to Tallon Overworld North":"Transport to Tallon Overworld North"}}})",
                          slotData, offset, &reason),
          "the layout fixture parses");
    PortApWorld::Layout layout;
    PortApWorld::Parse(slotData, layout);
    Check(layout.startRoom == "Arboretum" && layout.finalBosses == 1 &&
              layout.elevators.size() == 2,
          "slot_data fills the layout");

    PortJson::Value saved;
    PortApWorld::Layout reread;
    Check(PortJson::Parse(PortApWorld::Text(layout), saved, offset, &reason),
          "the saved layout is JSON");
    PortApWorld::Parse(saved, reread);
    Check(reread == layout, "the saved layout reads back the same");

    PortApWorld::Place place;
    Check(PortApWorld::StartRoom(layout, place) && place.mlvl == 0x83F6FF6Fu &&
              place.mrea == 0x18AB6106u,
          "the start room resolves to its world and area");
    PortApWorld::Layout unknown;
    Check(!PortApWorld::StartRoom(unknown, place), "no start room means the retail start");
    unknown.startRoom = "No Such Room";
    Check(!PortApWorld::StartRoom(unknown, place), "an unknown start room is refused");

    const PortApWorld::Place retailChozo{0x83F6FF6Fu, 0x3E6B2BB7u};
    PortApWorld::Place dest;
    // The area bits of the editor id are the loader's, the table has the rest.
    Check(PortApWorld::TeleporterDestination(layout, 0x39F2DE28u, 0x000E0005u, retailChozo,
                                             dest) &&
              dest.mlvl == 0x39F2DE28u && dest.mrea == 0x15D6FF8Bu,
          "a remapped elevator leads to its new room");
    Check(!PortApWorld::TeleporterDestination(layout, 0x39F2DE28u, 0x000E0006u, retailChozo, dest),
          "a teleporter that is no elevator is left alone");
    const PortApWorld::Place crater{0xC13B09D1u, 0x93668996u};
    Check(PortApWorld::TeleporterDestination(layout, 0x39F2DE28u, 0x00100001u, crater, dest) &&
              dest.mlvl == 0x13D79165u && dest.mrea == 0xB4B41C48u,
          "without Metroid Prime the temple portal leads to the credits");
    layout.finalBosses = 0;
    Check(!PortApWorld::TeleporterDestination(layout, 0x39F2DE28u, 0x00100001u, crater, dest),
          "with both bosses the temple portal is the disc's");

    // The texts: "Transport to Chozo Ruins West" leads to the Root Cave elevator.
    std::vector< std::string > strings;
    Check(PortApWorld::Strings(layout, 0x9EE2172Au, strings) && strings.size() == 1 &&
              strings[0] == "Transport to Tallon Overworld West\n(Root Cave)",
          "an elevator room's scan names where it leads");
    Check(PortApWorld::Strings(layout, 0x04685AE9u, strings) && strings.size() == 1 &&
              strings[0] == "Access to &main-color=#FF3333;Tallon Overworld West (Root Cave) "
                            "&main-color=#89D6FF;granted. Please step into the hologram.",
          "its hologram message too, on one line");
    Check(PortApWorld::Strings(layout, 0x73A833EBu, strings) && strings.size() == 1 &&
              strings[0] == "Transport to &main-color=#FF3333;Tallon Overworld West (Root Cave)"
                            "&main-color=#89D6FF; active.",
          "and its control message");
    Check(!PortApWorld::Strings(layout, 0x12345678u, strings), "other tables are the disc's");
    Check(PortApWorld::Strings(layout, 0xB389B6D6u, strings) && strings.size() == 3 &&
              strings[2] == "Current Mission: Retrieve " +
                                std::to_string(layout.requiredArtifacts) + " Chozo Artifact" +
                                (layout.requiredArtifacts != 1 ? "s" : "") +
                                "\nDefeat Meta Ridley\nDefeat Metroid Prime",
          "the temple's objective says what the seed asks for");

    Check(PortApWorld::SuitDamageReduction(0, true, true, true) < 0.f,
          "the default suit damage is the game's own");
    Check(PortApWorld::SuitDamageReduction(1, false, false, false) == 0.f &&
              PortApWorld::SuitDamageReduction(1, false, false, true) == 0.1f &&
              PortApWorld::SuitDamageReduction(1, true, false, true) == 0.2f &&
              PortApWorld::SuitDamageReduction(1, true, true, true) == 0.5f,
          "progressive suit damage counts the suits");
    Check(PortApWorld::SuitDamageReduction(2, true, false, false) == 0.1f &&
              PortApWorld::SuitDamageReduction(2, false, true, false) == 0.1f &&
              PortApWorld::SuitDamageReduction(2, false, false, true) == 0.3f &&
              PortApWorld::SuitDamageReduction(2, true, false, true) == 0.4f &&
              PortApWorld::SuitDamageReduction(2, true, true, true) == 0.5f,
          "additive suit damage adds each suit's part");

    Check(PortApWorld::TempleOps(layout).empty() && !PortApWorld::SkipsRidley(layout),
          "a retail temple needs no patch");
    layout.requiredArtifacts = 5;
    const std::vector< uint8_t > fewer = PortApWorld::TempleOps(layout);
    // Two edits (counter value, auto-reset) and twelve connections.
    Check(fewer.size() == 17 + 14 + 12 * 17 && fewer[0] == 2 && fewer[16] == 5,
          "fewer artifacts lower the temple's counter");
    layout.finalBosses = 2;
    Check(PortApWorld::SkipsRidley(layout) && PortApWorld::TempleOps(layout).size() > fewer.size(),
          "without Meta Ridley the temple skips the fight");
    PortJson::Value fewerData;
    Check(PortJson::Parse(R"({"required_artifacts":5,"final_bosses":2})", fewerData, offset,
                          &reason),
          "the temple fixture parses");
    PortApWorld::Parse(fewerData, reread);
    Check(reread.requiredArtifacts == 5 && reread.finalBosses == 2,
          "slot_data carries the temple's rules");

    // Doors: nothing changes without a mapping.
    const uint32_t kArboretum = 0x18AB6106u;
    Check(PortApWorld::Doors(PortApWorld::Layout(), kArboretum).empty(),
          "a seed without door mappings changes no door");
    PortJson::Value doorData;
    Check(PortJson::Parse(R"({"door_color_randomization":1,"door_color_mapping":{
        "Chozo Ruins":{"area":"Chozo Ruins","type_mapping":{"Blue":"Ice Beam","Wave Beam":"Bomb"}},
        "Tallon Overworld":{"area":"Tallon Overworld","type_mapping":{"Blue":"Power Beam Only"}}}})",
                          doorData, offset, &reason),
          "the door colour fixture parses");
    PortApWorld::Layout colours;
    PortApWorld::Parse(doorData, colours);
    Check(colours.hasDoorColors && colours.doorColorRandomization && !colours.hasShields &&
              colours.doorColors["Chozo Ruins"]["Blue"] == "Ice Beam",
          "slot_data carries the door colours");
    Check(PortJson::Parse(PortApWorld::Text(colours), saved, offset, &reason),
          "the saved door colours are JSON");
    PortApWorld::Parse(saved, reread);
    Check(reread == colours, "the saved door colours read back the same");
    // Main Plaza: five blue doors take the area's colour, and the missile door
    // stays a plain door under its shield.
    std::vector< PortApWorld::DoorChange > doors = PortApWorld::Doors(colours, 0xD5CDB809u);
    Check(doors.size() == 6 && doors[0].doorId == 0x0002001Bu && doors[0].type == "Ice Beam" &&
              doors[0].shield.empty() && doors[0].pair >= 0 && doors[0].forces[0] != 0,
          "a blue door takes its area's colour");
    Check(doors.size() == 6 && doors[2].dock == 2 && doors[2].type == "Blue" &&
              doors[2].shield == "Missile",
          "a door under a blast shield stays blue");
    doors = PortApWorld::Doors(colours, 0xB2701146u);
    Check(doors.size() == 5 && doors[0].type == "Power Beam Only",
          "each area has its own colours");

    Check(PortJson::Parse(R"({"blast_shield_mapping":{"Chozo Ruins":{"area":"Chozo Ruins",
        "type_mapping":{"Arboretum":{"0":"Power Bomb","1":"Disabled"}}}}})",
                          doorData, offset, &reason),
          "the blast shield fixture parses");
    PortApWorld::Layout shielded;
    PortApWorld::Parse(doorData, shielded);
    Check(shielded.hasShields && !shielded.hasDoorColors && !shielded.doorColorRandomization &&
              shielded.shields["Chozo Ruins"]["Arboretum"][1] == "Disabled",
          "slot_data carries the blast shields");
    Check(PortJson::Parse(PortApWorld::Text(shielded), saved, offset, &reason),
          "the saved blast shields are JSON");
    PortApWorld::Parse(saved, reread);
    Check(reread == shielded, "the saved blast shields read back the same");
    doors = PortApWorld::Doors(shielded, kArboretum);
    Check(doors.size() == 3 && doors[0].type == "Blue" && doors[0].shield == "Power Bomb",
          "a seed's blast shield sits on a blue door");
    Check(doors.size() == 3 && doors[1].type == "Disabled" && doors[1].shield.empty(),
          "a locked door is a disabled door without a shield");
    Check(doors.size() == 3 && doors[2].type == "Blue" && doors[2].shield == "None",
          "the disc's blast shields go when the seed has its own");
    // The other side of the Power Bomb door gets the shield too.
    bool mirrored = false;
    for (const PortApWorld::DoorChange& other : PortApWorld::Doors(shielded, 0x3D238FCDu))
      mirrored = mirrored || (other.pair == doors[0].index && other.shield == "Power Bomb");
    Check(mirrored, "a blast shield is on both sides of its door");

    // The script patch for a recoloured door: both of its forces and shields
    // are rewritten, and nothing is when the room isn't the disc's.
    doors = PortApWorld::Doors(colours, 0xD5CDB809u);
    std::vector< PortSkipCutscenes::ScriptObject > objects;
    auto object = [&](uint32_t id, uint8_t type, size_t size) {
      PortSkipCutscenes::ScriptObject o;
      o.type = type;
      o.id = id;
      o.props.assign(4 + 2 + size, 0);
      o.props[4] = 'x';
      objects.push_back(o);
    };
    int forceCount = 0, actorCount = 0;
    if (!doors.empty()) {
      for (int k = 0; k < 2; ++k) {
        if (doors[0].forces[k] != 0)
          object(doors[0].forces[k], 0x1A, 180), ++forceCount;
        if (doors[0].shieldActors[k] != 0)
          object(doors[0].shieldActors[k], 0x00, 300), ++actorCount;
      }
      doors.resize(1);
    }
    std::vector< uint8_t > doorOps = PortApWorld::DoorOps(doors, objects, nullptr);
    // An edit is 9 bytes of header and 4 a run: 116 + 12 for a force, 4 for a shield.
    Check(forceCount > 0 && actorCount > 0 &&
              doorOps.size() == size_t(forceCount) * (9 + 4 + 116 + 4 + 12) +
                                    size_t(actorCount) * (9 + 4 + 4),
          "a recoloured door rewrites its forces and shields");
    // Ice Beam: the second weapon opens it, the other beams bounce off.
    Check(doorOps.size() > 32 && doorOps[0] == 2 && doorOps[9 + 4 + 4 + 3] == 2 &&
              doorOps[9 + 4 + 8 + 3] == 1 && doorOps[9 + 4 + 12 + 3] == 2,
          "an ice door takes only the Ice Beam");
    if (!objects.empty())
      objects[0].props.resize(40);
    Check(PortApWorld::DoorOps(doors, objects, nullptr).empty(),
          "a door whose objects aren't the disc's is left alone");

    // A blast shield: its objects are pushed, its trigger reported, and once
    // broken it is gone from both sides.
    doors = PortApWorld::Doors(shielded, kArboretum);
    objects.clear();
    std::vector< PortApWorld::PlacedShield > placed;
    if (doors.size() == 3) {
      doors.resize(1);
      for (int k = 0; k < 2; ++k) {
        if (doors[0].forces[k] != 0)
          object(doors[0].forces[k], 0x1A, 180);
        if (doors[0].shieldActors[k] != 0)
          object(doors[0].shieldActors[k], 0x00, 300);
      }
      object(doors[0].doorId, 0x03, 127 + 4 + 44);
    }
    int scans = 0;
    doorOps = PortApWorld::DoorOps(
        doors, objects,
        [&](const std::string& text) {
          scans += text.find("Bendezium") != std::string::npos;
          return 0xD00D0002u;
        },
        &placed);
    Check(doors.size() == 1 && doors[0].shieldBit == 0 && placed.size() == 1 &&
              placed[0].bit == 0 && (placed[0].trigger & 0xFFFF) == 0x7001 &&
              (placed[0].trigger >> 16) == (doors[0].doorId >> 16) && scans == 1,
          "a blast shield is placed over its door and its trigger reported");
    // The disc's own missile shield on that door makes way for the seed's,
    // and serves when the seed asks for a missile shield there.
    std::vector< PortSkipCutscenes::ScriptObject > disc = objects;
    {
      PortSkipCutscenes::ScriptObject o;
      o.type = 0x00;
      o.id = 0x00027F00;
      o.props.assign(6 + 300, 0);
      o.props[4] = 'x';
      o.props[6 + 196] = 0xEF, o.props[6 + 197] = 0xDF, o.props[6 + 198] = 0xFB,
      o.props[6 + 199] = 0x8C;
      disc.push_back(o);
    }
    placed.clear();
    const std::vector< uint8_t > replaced = PortApWorld::DoorOps(doors, disc, nullptr, &placed);
    const uint8_t removal[5] = {6, 0x00, 0x02, 0x7F, 0x00};
    Check(placed.size() == 1 && doors.size() == 1 && doors[0].replacesShield &&
              std::search(replaced.begin(), replaced.end(), removal, removal + 5) !=
                  replaced.end(),
          "the disc's missile shield makes way for the seed's");
    if (!doors.empty()) {
      std::vector< PortApWorld::DoorChange > missile = doors;
      missile[0].shield = "Missile";
      missile[0].replacesShield = false;
      placed.clear();
      const std::vector< uint8_t > kept = PortApWorld::DoorOps(missile, disc, nullptr, &placed);
      Check(placed.empty() &&
                std::search(kept.begin(), kept.end(), removal, removal + 5) == kept.end(),
            "the disc's missile shield serves where the seed wants one");
    }
    {
      // The smaller options, over stand-ins for the disc's objects.
      const auto object = [](uint8_t type, uint32_t id, size_t rest) {
        PortSkipCutscenes::ScriptObject o;
        o.type = type;
        o.id = id;
        o.props.assign(6 + rest, 0);
        o.props[4] = 'x';
        return o;
      };
      PortApWorld::Layout options;
      const std::vector< PortSkipCutscenes::ScriptObject > totem = {object(0x15, 0x0024008C, 1)};
      Check(PortApWorld::RoomOps(options, 0xC8309DF6, totem).empty() &&
                PortApWorld::Layers(options).size() == 2,
            "a seed without the options leaves the rooms alone");
      options.removeHiveMecha = true;
      options.backwardsLowerMines = true;
      options.flaahgraPowerBombs = true;
      PortApWorld::Layout copy;
      PortJson::Value text;
      size_t textOffset = 0;
      const char* textReason = nullptr;
      if (PortJson::Parse(PortApWorld::Text(options), text, textOffset, &textReason))
        PortApWorld::Parse(text, copy);
      Check(copy == options, "the options survive the layout's text");
      const std::vector< uint8_t > mecha = PortApWorld::RoomOps(options, 0xC8309DF6, totem);
      const std::vector< PortApWorld::LayerChange > layers = PortApWorld::Layers(options);
      Check(mecha.size() > 8 && mecha[0] == 5 && mecha[2] == 0x05 && layers.size() == 3 &&
                layers[0].mrea == 0xC8309DF6 && layers[0].layer == 1 && !layers[0].active,
            "no Hive Mecha: a timer ends the fight and its layer is off");
      Check(PortApWorld::RoomOps(options, 0xC8309DF6, {object(0x15, 0x0024008C, 0)}).empty(),
            "a Hive Totem that isn't the disc's is left alone");
      const std::vector< uint8_t > stone =
          PortApWorld::RoomOps(options, 0x18AB6106, {object(0x1A, 0x001300D7, 180)});
      const uint8_t wanted[] = {2, 0x00, 0x13, 0x00, 0xD7, 0, 0, 0, 1, 0, 56, 0, 4, 0, 0, 0, 1};
      Check(stone == std::vector< uint8_t >(wanted, wanted + sizeof(wanted)),
            "Flaahgra power bombs: the sandstone takes power bomb damage");
      const std::vector< uint8_t > pins = PortApWorld::RoomOps(
          options, 0xED6DE73B,
          {object(0x08, 0x00160075, 352), object(0x00, 0x00160001, 354),
           object(0x08, 0x00160076, 352)});
      const uint8_t gone[] = {6, 0x00, 0x16, 0x00, 0x75, 6, 0x00, 0x16, 0x00, 0x76};
      Check(pins == std::vector< uint8_t >(gone, gone + sizeof(gone)),
            "backwards Lower Mines: the access hall's platforms are removed");
      const std::vector< uint8_t > field =
          PortApWorld::RoomOps(options, 0xC50AF17A, {object(0x00, 0x04100086, 354)});
      Check(field.size() == 14 && field[0] == 2 && field[9] == 0x01 && field[10] == 0x45 &&
                field[13] == 1,
            "backwards Lower Mines: Elite Control's force field lets shots through");

      // What every seed changes.
      const PortApWorld::Layout plain;
      const std::vector< PortApWorld::LayerChange > elite = PortApWorld::Layers(plain);
      Check(elite.size() == 2 && elite[0].mrea == 0x8A97BB54 && elite[0].layer == 1 &&
                elite[0].active && elite[0].whileLayer == 5 && elite[1].layer == 5 &&
                !elite[1].active && elite[1].whileLayer == -1,
            "the Phazon Elite is there once, without Central Dynamo");
      const std::vector< uint8_t > dynamo = PortApWorld::RoomOps(
          plain, 0xFEA372E2, {object(0x3A, 0x001B0522, 70), object(0x3A, 0x001B0525, 70)});
      const uint8_t switches[] = {6, 0x00, 0x1B, 0x05, 0x25, 6, 0x00, 0x1B, 0x05, 0x22};
      Check(dynamo == std::vector< uint8_t >(switches, switches + sizeof(switches)),
            "Central Dynamo no longer switches the Phazon Elite's layers");
      const std::vector< uint8_t > hydra =
          PortApWorld::RoomOps(plain, 0x43E4CC25, {object(0x00, 0x0C190332, 354)});
      Check(hydra.size() == 14 && hydra[0] == 2 && hydra[10] == 0x45 && hydra[13] == 1,
            "Research Lab Hydra's force field lets a scan through");
      const std::vector< uint8_t > vent =
          PortApWorld::RoomOps(plain, 0xAFD4E038, {object(0x15, 0x0015006F, 1)});
      Check(vent.size() > 20 && vent[0] == 5 && vent[1] == 0 && vent[2] == 0x04 && vent[8] == 1,
            "the frigate's unpowered door gets a trigger behind it");
      Check(PortApWorld::RoomOps(plain, 0xAFD4E038, {object(0x15, 0x0015006F, 1),
                                                      object(0x04, 0x00156FF0, 63)})
                .empty(),
            "a room that has the trigger already is left alone");
      const std::vector< uint8_t > quarry = PortApWorld::RoomOps(
          plain, 0x643D038F, {object(0x00, 0x100201DA, 354), object(0x3A, 0x000202B5, 70)});
      Check(quarry.size() > 30 && quarry[0] == 5 && quarry[1] == 4 && quarry[2] == 0x04 &&
                quarry[8] == 2,
            "Main Quarry's barrier gets a trigger behind it, in the barrier's layer");
      const std::vector< PortSkipCutscenes::ScriptObject > plaza = {
          object(0x03, 0x00020060, 217), object(0x1A, 0x00020016, 180),
          object(0x04, 0x00020017, 63),  object(0x00, 0x00020018, 354),
          object(0x05, 0x00020019, 11),  object(0x42, 0x000202F4, 37),
          object(0x04, 0x000202B8, 63),  object(0x15, 0x000202FD, 1)};
      const std::vector< uint8_t > twoWay = PortApWorld::RoomOps(plain, 0xD5CDB809, plaza);
      const std::vector< uint8_t > shieldId = {0x00, 0x00, 0x02, 0x00, 0x04};
      Check(twoWay.size() > 700 && twoWay[0] == 5 && twoWay[2] == 0x1A &&
                std::search(twoWay.begin(), twoWay.end(), shieldId.begin(), shieldId.end()) !=
                    twoWay.end(),
            "Main Plaza's one-way door gets a shield and its triggers");
      std::vector< PortSkipCutscenes::ScriptObject > taken = plaza;
      taken.push_back(object(0x15, 0x00020004, 1));
      Check(PortApWorld::RoomOps(plain, 0xD5CDB809, taken).empty(),
            "a Main Plaza that isn't the disc's is left alone");
      // A spawn point: 35 properties, the default flag after the 29 item counts.
      const auto spawn = [&object](uint32_t id, bool firstSpawn) {
        PortSkipCutscenes::ScriptObject o = object(0x0F, id, 24 + 29 * 4 + 3);
        o.props[3] = 35;
        o.props[6 + 24 + 29 * 4] = firstSpawn ? 1 : 0;
        return o;
      };
      const std::vector< uint8_t > fountain = PortApWorld::RoomOps(
          plain, 0x165A4DE9, {spawn(0x00050010, false), spawn(0x00050011, true), spawn(0x00050012, true)});
      const uint8_t floor[] = {2,    0x00, 0x05, 0x00, 0x11, 0,    0,    0,    1,    0,    0,
                               0,    12,   0x43, 0x52, 0x83, 0x12, 0xC2, 0xA4, 0xD9, 0x17, 0x41,
                               0x99, 0xBD, 0x3C};
      Check(fountain == std::vector< uint8_t >(floor, floor + sizeof(floor)),
            "Ruined Fountain's default spawn point moves out of the spider track alcove");
      Check(PortApWorld::RoomOps(plain, 0x165A4DE9, {spawn(0x00050010, false)}).empty(),
            "a Ruined Fountain without a default spawn point is left alone");

      // The softlock fixes.
      std::vector< PortSkipCutscenes::ScriptObject > tower = {object(0x04, 0x001D015B, 63),
                                                               object(0x02, 0x041D006E, 61)};
      tower[1].layer = 1;
      const uint8_t moved[] = {6, 0x04, 0x1D, 0x00, 0x6E, 7, 1, 0x00, 0x1D, 0x01, 0x5B};
      Check(PortApWorld::RoomOps(plain, 0xDE161372, tower) ==
                std::vector< uint8_t >(moved, moved + sizeof(moved)),
            "Sun Tower's layer trigger moves to a layer of its own");
      tower[0].layer = 1;
      Check(PortApWorld::RoomOps(plain, 0xDE161372, tower).empty(),
            "a Sun Tower already changed is left alone");
      const std::vector< uint8_t > flaahgra =
          PortApWorld::RoomOps(plain, 0x9A0A03EB, {object(0x15, 0x042500D4, 1)});
      const std::vector< uint8_t > towerArea = {0xCF, 0x4C, 0x7A, 0xA5, 0, 0, 0, 1};
      Check(flaahgra.size() > 80 && flaahgra[0] == 5 && flaahgra[1] == 1 && flaahgra[2] == 0x3A &&
                flaahgra[flaahgra.size() - 17] == 4 &&
                std::search(flaahgra.begin(), flaahgra.end(), towerArea.begin(),
                            towerArea.end()) != flaahgra.end(),
            "Flaahgra's death switches the Sun Tower trigger on");
      const std::vector< uint8_t > station =
          PortApWorld::RoomOps(plain, 0x956F1552, {object(0x04, 0x0407033F, 63)});
      Check(station.size() == 3 * 17 && station[0] == 2 && station[10] == 12 &&
                station[13] == 0x42 && station[14] == 0x48 && station[17 + 10] == 16 &&
                station[34 + 10] == 20,
            "the Security Station's alert trigger fills the room");
      std::vector< PortSkipCutscenes::ScriptObject > quarters = {
          object(0x11, 0x041A04C5, 234), object(0x86, 0x141A0126, 793),
          object(0x15, 0x141A0328, 1), object(0x08, 0x001A03D9, 300)};
      quarters[1].connections.push_back({14, 13, 0x141A0328});
      const std::vector< uint8_t > omega = PortApWorld::RoomOps(plain, 0x3953C353, quarters);
      Check(omega.size() == 3 * 17 && omega[0] == 4 && omega[17] == 4 && omega[34] == 3 &&
                omega[35] == 0x14,
            "the Elite Quarters item unlocks the room, the Omega Pirate no longer does");
      Check(PortApWorld::RoomOps(plain, 0x49175472, {object(0x3A, 0x0035013A, 70)}).size() == 5,
            "Gravity Chamber keeps its stalactite");
    }
    std::vector< PortSkipCutscenes::ScriptObject > hatch = objects;
    if (!hatch.empty()) {
      std::vector< uint8_t >& props = hatch.back().props;
      props[6 + 36] = 0xF5, props[6 + 37] = 0x7D, props[6 + 38] = 0xD4, props[6 + 39] = 0x84;
    }
    placed.clear();
    PortApWorld::DoorOps(doors, hatch, nullptr, &placed);
    Check(placed.empty(), "a morph ball door gets no blast shield");
    const uint32_t brokenShields[4] = {1, 0, 0, 0};
    doors = PortApWorld::Doors(shielded, kArboretum, brokenShields);
    bool gone = doors.size() == 3 && doors[0].shield.empty() && doors[0].type == "Blue" &&
                doors[0].shieldBit == 0 && doors[1].shieldBit < 0 && doors[2].shieldBit < 0;
    for (const PortApWorld::DoorChange& other :
         PortApWorld::Doors(shielded, 0x3D238FCDu, brokenShields))
      gone = gone && other.shield != "Power Bomb";
    Check(gone, "a broken blast shield is gone from both sides of its door");

    // A locked door says so when scanned.
    doors = PortApWorld::Doors(shielded, kArboretum);
    objects.clear();
    std::string scanned;
    if (doors.size() == 3) {
      doors.erase(doors.begin());
      doors.resize(1);
      doors[0].forces[0] = doors[0].forces[1] = 0;
      doors[0].shieldActors[0] = doors[0].shieldActors[1] = 0;
      object(doors[0].doorId, 0x03, 127 + 4 + 44);
      PortSkipCutscenes::ScriptObject& door = objects[0];
      door.props[3] = 14;
      door.props[6 + 52 + 3] = 14;
      door.props[6 + 123 + 3] = 1;
    }
    doorOps = PortApWorld::DoorOps(doors, objects, [&](const std::string& text) {
      scanned = text;
      return 0xD00D0001u;
    });
    Check(scanned == "This door cannot be opened." && doorOps.size() == 9 + 4 + 4 &&
              doorOps[9 + 1] == 127 && doorOps[9 + 4] == 0xD0 && doorOps[9 + 7] == 0x01,
          "a locked door gets its scan");

    // The map shows a door's new colour, and a shield as a shield.
    std::vector< PortApWorld::MapDoor > mapDoors = PortApWorld::MapDoors(colours, 0xFC184334u);
    Check(mapDoors.size() == 6 && mapDoors[0].doorId == 0x0002001Bu && mapDoors[0].type == 2 &&
              mapDoors[2].type == 1,
          "the map follows the door colours");
    Check(PortApWorld::MapDoors(PortApWorld::Layout(), 0xFC184334u).empty() &&
              PortApWorld::MapDoors(colours, 0x12345678u).empty(),
          "a map without changed doors is left alone");
    Check(PortApWorld::IsDoorDependency(0x59649E9Du) && !PortApWorld::IsDoorDependency(0x12345678u),
          "the door shields can be loaded from another world");
  }

  // The tracker's logic through a retail layout is the pack's own: the door
  // table and the pack agree on every lock and missile shield.
  {
    PortApLogic::Options plain;
    plain.trickDifficulty = 2;
    PortApLogic::Options filled = plain;
    PortApWorld::FillLogic(PortApWorld::Layout(), filled);
    Check(!filled.doors.empty(), "the logic is given the doors");
    bool same = true;
    PortApLogic::Items items;
    for (int item = 0; item <= 45; ++item) {
      items[PortAp::MetroidPrime::kItemBase + item] = 20;
      same = same && PortApLogic::Evaluate(filled, items) == PortApLogic::Evaluate(plain, items);
    }
    for (int item = 0; item <= 45; ++item) {
      PortApLogic::Items without = items;
      without.erase(PortAp::MetroidPrime::kItemBase + item);
      same = same && PortApLogic::Evaluate(filled, without) == PortApLogic::Evaluate(plain, without);
    }
    Check(same, "a retail layout leaves the tracker's logic as the pack has it");

    PortApWorld::Layout layout;
    layout.startRoom = "Arboretum";
    layout.removeHiveMecha = true;
    layout.hasShields = true;
    layout.shields["Tallon Overworld"]["Landing Site"][3] = "Flamethrower";
    layout.hasDoorColors = true;
    layout.doorColors["Chozo Ruins"]["Wave Beam"] = "Ice Beam";
    PortApWorld::FillLogic(layout, filled);
    const PortApLogic::Options::Door there = filled.doors["Tallon Overworld|Landing Site|Alcove"];
    const PortApLogic::Options::Door back = filled.doors["Tallon Overworld|Alcove|Landing Site"];
    Check(there.shield == "Flamethrower" && there.lock == "Plasma Beam" && back == there,
          "a blast shield reaches the logic from both sides of its door");
    Check(filled.startRoom == "Arboretum" && filled.removeHiveMecha && !filled.backwardsLowerMines &&
              filled.doorColors["Chozo Ruins"]["Wave Beam"] == "Ice Beam",
          "the layout's options reach the logic");
    bool recoloured = false, shielded = false;
    for (const auto& door : filled.doors) {
      if (door.first.compare(0, 12, "Chozo Ruins|") == 0)
        recoloured = recoloured || door.second.lock == "Wave Beam";
      else if (door.first.compare(0, 17, "Tallon Overworld|") == 0 && door.first.find("Alcove") == std::string::npos)
        shielded = shielded || !door.second.shield.empty();
    }
    Check(!recoloured, "no Chozo Ruins door keeps a recoloured lock");
    Check(!shielded, "a seed with blast shields has only its own");
  }

  std::filesystem::remove_all(testDir);
  if (!sPassed)
    return 1;
  std::puts("[ap-tests] passed");
  return 0;
}
