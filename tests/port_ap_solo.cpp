#include "port_ap_metroidprime.h"
#include "port_ap_protocol.h"
#include "port_ap_solo.h"
#include "port_rando_gen.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

unsigned long TestProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

namespace Mp = PortAp::MetroidPrime;
using PortAp::Protocol::ItemGrant;
using PortAp::Protocol::Session;

PortJson::Value Packet(const std::string& json) {
  PortJson::Value value;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(json, value, offset, &reason))
    CHECK_MSG(false, "server packet is JSON");
  return value;
}

// What the client does with the server's messages: feeds them to the session
// and answers with whatever the session sends back, until the server is quiet.
std::vector<ItemGrant> Pump(PortApSolo::Server& server, Session& session) {
  std::vector<ItemGrant> grants;
  std::string message;
  while (server.ReceiveText(message, 20)) {
    const PortJson::Value root = Packet(message);
    CHECK_MSG(root.IsArray(), "the server sends arrays of commands");
    for (const PortJson::Value& command : root.AsArray()) {
      std::vector<std::string> outgoing;
      session.HandlePacket(command, outgoing, grants);
      if (command.StringOr("cmd") == "RoomInfo")
        outgoing.push_back(session.BuildConnect());
      for (const std::string& packet : outgoing)
        CHECK_MSG(server.SendText("[" + packet + "]"), "the server takes a packet");
    }
  }
  return grants;
}

Session MakeSession(const PortAp::Protocol::State& state = PortAp::Protocol::State()) {
  const PortAp::Protocol::Config config =
      PortAp::Protocol::ParseConfig(R"({"server":"solo:t","slot":"Samus"})");
  CHECK_MSG(config.valid && config.builtin, "a solo config uses the built-in tables");
  return Session(config, state);
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Replaces the value text after `"key":` up to the next ',' or '}' with "X",
// for the fields that change from run to run (the client's uuid, a clock).
std::string Mask(std::string text, const std::string& key) {
  const std::string marker = "\"" + key + "\":";
  const size_t start = text.find(marker);
  if (start == std::string::npos)
    return text;
  const size_t value = start + marker.size();
  const size_t end = text.find_first_of(",}", value);
  return text.replace(value, end - value, "X");
}

// Text as a C++ string literal, so a golden mismatch prints what to compare
// against.
std::string Literal(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (byte < 0x20 || byte >= 0x7f) {
      char escape[8];
      std::snprintf(escape, sizeof(escape), "\\x%02x\"\"", byte);
      out += escape;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

#define CHECK_GOLDEN(actual, expected) CheckGolden((actual), (expected), __LINE__)

void CheckGolden(const std::string& actual, const std::string& expected, int line) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "port_ap_solo.cpp:%d: golden output differs\n  expected %s\n  actual   %s\n", line,
               Literal(expected).c_str(), Literal(actual).c_str());
  port_test::CountFailure();
}

// Golden output: the seed and state files and the packets byte for byte, as
// the hand-assembled writers made them before port::JsonWriter replaced them.
// Saved files are compared with the ones players already have (a seed saved
// again must keep its name), so their text must not drift; the packets are
// what an Archipelago server and the client tests expect.
void Golden(const std::filesystem::path& testDir, const PortRandoGen::Seed& base) {
  PortRandoGen::Settings settings;
  settings.trickDifficulty = 1;
  settings.trickAllow = {"Wallcrawl", "Say \"hi\"\n\\"};
  settings.trickDeny = {"Bomb Jump"};
  settings.doorColorRandomization = 2;
  settings.combatLogic = -1;
  settings.etankCapacity = 37;
  CHECK_GOLDEN(PortRandoGen::SettingsText(settings),
      "{\"required_artifacts\":12,\"final_bosses\":0,\"artifact_hints\":1,\"missile_launcher\":0,"
      "\"main_power_bomb\":0,\"shuffle_scan_visor\":0,\"pre_scan_elevators\":1,"
      "\"elevator_randomization\":0,\"door_color_randomization\":2,\"progressive_beam_upgrades\":0,"
      "\"non_varia_heat_damage\":1,\"staggered_suit_damage\":1,\"combat_logic_difficulty\":-1,"
      "\"trick_difficulty\":1,\"trick_allow_list\":[\"Wallcrawl\",\"Say \\\"hi\\\"\\n\\\\\"],"
      "\"trick_deny_list\":[\"Bomb Jump\"],\"flaahgra_power_bombs\":0,\"etank_capacity\":37,"
      "\"backwards_lower_mines\":0,\"remove_xray_requirements\":0,\"remove_thermal_requirements\":0,"
      "\"remove_hive_mecha\":0,\"spring_ball\":1,\"shuffle_unlimited_missiles\":0,"
      "\"shuffle_unlimited_power_bombs\":0,\"starting_room\":0,\"randomize_starting_beam\":0,"
      "\"blast_shield_randomization\":0,\"blast_shield_frequency\":4,\"blast_shield_available_types\":0,"
      "\"locked_door_count\":0,\"include_power_beam_doors\":0,\"include_morph_ball_bomb_doors\":0}");

  // A seed file, with slot data that exercises every number and string rule.
  {
    PortRandoGen::Seed odd;
    odd.name = "golden \"odd\"";
    odd.settings = settings;
    odd.slotData = R"( { "a" : 1.5, "b":-0.0, "c":1e300, "d":12345678901234567, "e":"x\u0001\"\u00e9\/",)"
                   R"( "f":[true,false,null,[],{}], "g":-42, "h":0.1, "i":8999999999999999,)"
                   R"( "j":9000000000000000, "k":-1e-7 } )";
    odd.startItems = {5031045, 5};
    odd.placements = {{-3, 9000000000000000001}, {1, 2}};
    odd.spoiler = "tab\there \x01 \x7f \xc3\xa9 \"q\" \\ /\r\n";
    std::string error;
    CHECK_MSG(PortRandoGen::Save(odd, error), "the golden seed saves");
    CHECK_GOLDEN(ReadText(PortRandoGen::SeedPath(odd.name)),
        "{\"version\":1,\"name\":\"golden \\\"odd\\\"\",\"settings\":{\"required_artifacts\":12,"
        "\"final_bosses\":0,\"artifact_hints\":1,\"missile_launcher\":0,\"main_power_bomb\":0,"
        "\"shuffle_scan_visor\":0,\"pre_scan_elevators\":1,\"elevator_randomization\":0,"
        "\"door_color_randomization\":2,\"progressive_beam_upgrades\":0,\"non_varia_heat_damage\":1,"
        "\"staggered_suit_damage\":1,\"combat_logic_difficulty\":-1,\"trick_difficulty\":1,"
        "\"trick_allow_list\":[\"Wallcrawl\",\"Say \\\"hi\\\"\\n\\\\\"],\"trick_deny_list\":[\"Bomb Jump\"],"
        "\"flaahgra_power_bombs\":0,\"etank_capacity\":37,\"backwards_lower_mines\":0,"
        "\"remove_xray_requirements\":0,\"remove_thermal_requirements\":0,\"remove_hive_mecha\":0,"
        "\"spring_ball\":1,\"shuffle_unlimited_missiles\":0,\"shuffle_unlimited_power_bombs\":0,"
        "\"starting_room\":0,\"randomize_starting_beam\":0,\"blast_shield_randomization\":0,"
        "\"blast_shield_frequency\":4,\"blast_shield_available_types\":0,\"locked_door_count\":0,"
        "\"include_power_beam_doors\":0,\"include_morph_ball_bomb_doors\":0},\"slot_data\":{\"a\":1.5,"
        "\"b\":0,\"c\":1.0000000000000001e+300,\"d\":12345678901234568,\"e\":\"x\\u0001\\\"\xc3""\xa9""/\","
        "\"f\":[true,false,null,[],{}],\"g\":-42,\"h\":0.10000000000000001,\"i\":8999999999999999,"
        "\"j\":9000000000000000,\"k\":-9.9999999999999995e-08},\"start_items\":[5031045,"
        "5],\"placements\":{\"-3\":9000000000000000001,\"1\":2},\"spoiler\":\"tab\\there \\u0001 \x7f"" \xc3""\xa9"" "
        "\\\"q\\\" \\\\ /\\r\\n\"}\n");
    std::filesystem::remove(PortRandoGen::SeedPath(odd.name));
  }

  // The solo server's packets.
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.OpenSeed(base, (testDir / "golden.state.json").string(), error), "the golden server opens");
    const auto drain = [&server] {
      std::string all;
      std::string message;
      while (server.ReceiveText(message, 20))
        all += message + "\n";
      return all;
    };
    CHECK_GOLDEN(drain(),
        "[{\"cmd\":\"RoomInfo\",\"version\":{\"major\":0,\"minor\":6,\"build\":0,"
        "\"class\":\"Version\"},\"generator_version\":{\"major\":0,\"minor\":6,\"build\":0,"
        "\"class\":\"Version\"},\"tags\":[],\"password\":false,\"permissions\":{\"release\":0,"
        "\"collect\":0,\"remaining\":0},\"hint_cost\":0,\"location_check_points\":1,"
        "\"games\":[\"Metroid Prime\"],\"datapackage_checksums\":{},\"seed_name\":\"solo-t1 /weird\","
        "\"time\":0}]\n");
    CHECK_MSG(server.SendText(R"([{"cmd":"Connect","name":"Samus","game":"Metroid Prime"}])"), "connect sent");
    CHECK_GOLDEN(drain(),
        "[{\"cmd\":\"Connected\",\"team\":0,\"slot\":1,\"players\":[{\"team\":0,\"slot\":1,"
        "\"alias\":\"Samus\",\"name\":\"Samus\"}],\"missing_locations\":[5031100,"
        "5031101,5031102,5031103],\"checked_locations\":[],\"slot_data\":{\"required_artifacts\":5,"
        "\"missile_launcher\":0,\"note\":\"a\\\"b\",\"n\":1.5}}]\n[{\"cmd\":\"ReceivedItems\","
        "\"index\":0,\"items\":[{\"item\":5031045,\"location\":-2,\"player\":1,\"flags\":1}]}]\n");
    size_t count = 0;
    const Mp::Location* locations = Mp::Locations(count);
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[1].id, 42}) + "," +
                              Session::BuildLocationScouts({locations[0].id, 42, locations[3].id}) + "]"),
              "checks and scouts sent");
    CHECK_MSG(server.SendText("[" + Session::BuildSay("hello \"there\"\n") + "," + Session::BuildSay("!players") + "," +
                              Session::BuildGoal() + "]"),
              "chat and goal sent");
    CHECK_MSG(server.SendText(R"([{"cmd":"Get","keys":["a\"b",3,"c"]},{"cmd":"SetNotify","keys":[]},)"
                              R"({"cmd":"Set","key":"k\n","want_reply":true},{"cmd":"GetDataPackage"}])"),
              "data storage sent");
    CHECK_MSG(server.SendText("not json"), "garbage sent");
    CHECK_GOLDEN(drain(),
        "[{\"cmd\":\"ReceivedItems\",\"index\":1,\"items\":[{\"item\":5031043,\"location\":5031101,"
        "\"player\":1,\"flags\":1}]}]\n[{\"cmd\":\"LocationInfo\",\"locations\":[{\"item\":5031024,"
        "\"location\":5031100,\"player\":1,\"flags\":0},{\"item\":5031029,\"location\":5031103,"
        "\"player\":1,\"flags\":1}]}]\n[{\"cmd\":\"PrintJSON\",\"type\":\"Chat\","
        "\"team\":0,\"slot\":1,\"message\":\"hello \\\"there\\\"\\n\",\"data\":[{\"type\":\"text\","
        "\"text\":\"Samus: hello \\\"there\\\"\\n\"}]}]\n[{\"cmd\":\"PrintJSON\","
        "\"type\":\"CommandResult\",\"data\":[{\"type\":\"text\",\"text\":\"Samus (Metroid Prime) is the "
        "only player.\"}]}]\n[{\"cmd\":\"PrintJSON\",\"type\":\"Goal\",\"team\":0,\"slot\":1,"
        "\"data\":[{\"type\":\"text\",\"text\":\"Samus has completed their goal.\"}]}]\n"
        "[{\"cmd\":\"Retrieved\",\"keys\":{\"a\\\"b\":null,\"c\":null}}]\n[{\"cmd\":\"Retrieved\","
        "\"keys\":{}}]\n[{\"cmd\":\"SetReply\",\"key\":\"k\\n\",\"value\":\"solo\","
        "\"original_value\":\"solo\",\"slot\":1}]\n[{\"cmd\":\"DataPackage\",\"data\":{\"games\":{}}}]\n"
        "[{\"cmd\":\"InvalidPacket\",\"type\":\"cmd\",\"original_cmd\":null,\"text\":\"not JSON\"}]\n");
    CHECK_GOLDEN(ReadText(testDir / "golden.state.json"), "{\"checked\":[5031101]}\n");
  }

  // The client's state file, with its logic options and layout.
  {
    PortAp::Protocol::State state;
    state.slot = "Sa\"mus";
    state.seed = "se\\ed";
    state.room = std::string("room") + '\x02';
    state.nextItemIndex = 77;
    state.checkedLocations = {3, -4, 9000000000000000001};
    state.progressive = {{5, 2}, {-1, 7}};
    state.hasLogic = true;
    state.logic.trickDifficulty = 2;
    state.logic.removeThermal = 1;
    state.logic.mainMissile = true;
    state.logic.trickAllow = {"a\"b", "c"};
    state.hasWorld = true;
    state.world.startRoom = "Landing \"Site\"";
    state.world.finalBosses = 3;
    state.world.elevators["Chozo Ruins"]["Transport A"] = "Transport B";
    state.world.elevators["Tallon Overworld"]["Transport C"] = "Transport D";
    state.world.hasDoorColors = true;
    state.world.doorColors["Chozo Ruins"]["Wave Beam"] = "Ice Beam";
    state.world.doorColors["Chozo Ruins"]["Ice Beam"] = "Plasma Beam";
    state.world.hasShields = true;
    state.world.shields["Phazon Mines"]["Elite Research"][0] = "Missile";
    state.world.shields["Phazon Mines"]["Elite Research"][3] = "Power Bomb";
    state.world.shields["Phazon Mines"]["Quarantine Access"][1] = "Super Missile";
    const std::string path = (testDir / "golden_state.json").string();
    CHECK_MSG(PortAp::Protocol::SaveStateFile(path, state), "the golden state saves");
    CHECK_GOLDEN(ReadText(path),
        "{\"slot\":\"Sa\\\"mus\",\"seed\":\"se\\\\ed\",\"room\":\"room\\u0002\",\"next_item_index\":77,"
        "\"checked_locations\":[3,-4,9000000000000000001],\"progressive\":{\"-1\":7,"
        "\"5\":2},\"logic\":{\"trick_difficulty\":2,\"combat_logic_difficulty\":0,"
        "\"remove_xray_requirements\":0,\"remove_thermal_requirements\":1,\"flaahgra_power_bombs\":0,"
        "\"progressive_beam_upgrades\":0,\"missile_launcher\":1,\"main_power_bomb\":0,"
        "\"non_varia_heat_damage\":0,\"pre_scan_elevators\":0,\"trick_allow_list\":[\"a\\\"b\","
        "\"c\"],\"trick_deny_list\":[]},\"world\":{\"starting_room_name\":\"Landing \\\"Site\\\"\","
        "\"final_bosses\":3,\"required_artifacts\":12,\"elevator_mapping\":{\"Chozo Ruins\":{\"Transport A\":\"Tr"
        "ansport B\"},\"Tallon Overworld\":{\"Transport C\":\"Transport D\"}},\"door_color_randomization\":0,"
        "\"remove_hive_mecha\":0,\"backwards_lower_mines\":0,\"flaahgra_power_bombs\":0,"
        "\"etank_capacity\":100,\"door_color_mapping\":{\"Chozo Ruins\":{\"type_mapping\":{\"Ice Beam\":\"Plasma "
        "Beam\",\"Wave Beam\":\"Ice Beam\"}}},\"blast_shield_mapping\":{\"Phazon Mines\":{\"type_mapping\":{\"Eli"
        "te Research\":{\"0\":\"Missile\",\"3\":\"Power Bomb\"},\"Quarantine Access\":{\"1\":\"Super Missile\"}}}"
        "}}}");
    PortAp::Protocol::State minimal;
    minimal.slot = "x";
    CHECK_MSG(PortAp::Protocol::SaveStateFile(path, minimal), "the minimal state saves");
    CHECK_GOLDEN(ReadText(path),
        "{\"slot\":\"x\",\"seed\":\"\",\"next_item_index\":0,\"checked_locations\":[],"
        "\"progressive\":{}}");
  }

  // archipelago.json rewritten around the player's own keys.
  {
    const std::filesystem::path path = testDir / "golden_archipelago.json";
    {
      std::ofstream file(path, std::ios::binary);
      file << R"( {"slot":"Old","death_link":true,"extra":[1,2.5,null,"x\"y",-0.0,1e300,0.1],"o":{"a":{}},)"
              R"("locations":{"39F2DE28:B2701146:0000007E":5031158},"server":"ws://a:1","password":"zz"} )";
    }
    PortAp::Protocol::Connection connection;
    connection.server = "wss://h\"ost:1";
    connection.slot = "Sam\tus";
    connection.enabled = false;
    connection.seed = "s";
    connection.lastPlayed = 1790000000;
    std::string error;
    CHECK_MSG(PortAp::Protocol::SaveConnectionFile(path.string(), connection, error), "the connection saves");
    CHECK_GOLDEN(ReadText(path),
        "{\"slot\":\"Sam\\tus\",\"death_link\":true,\"extra\":[1,2.5,null,\"x\\\"y\","
        "0,1.0000000000000001e+300,0.10000000000000001],\"o\":{\"a\":{}},\"locations\":{\"39F2DE28:B2701146:00000"
        "07E\":5031158},\"server\":\"wss://h\\\"ost:1\",\"enabled\":false,\"seed\":\"s\","
        "\"last_played\":1790000000}\n");
    std::filesystem::remove(path);
    connection.password = "pw";
    connection.enabled = true;
    connection.room = "r";
    connection.lastPlayed = 0;
    CHECK_MSG(PortAp::Protocol::SaveConnectionFile(path.string(), connection, error), "a new connection saves");
    CHECK_GOLDEN(ReadText(path),
        "{\"server\":\"wss://h\\\"ost:1\",\"slot\":\"Sam\\tus\",\"password\":\"pw\","
        "\"seed\":\"s\",\"room\":\"r\"}\n");
  }

  // The client's packets.
  {
    const PortAp::Protocol::Config tagged = PortAp::Protocol::ParseConfig(
        R"({"server":"ws://h","slot":"P\"1","password":"p\\w","tags":["AP","Tracker"],"death_link":true})");
    const Session session(tagged, PortAp::Protocol::State());
    CHECK_GOLDEN(Mask(session.BuildConnect(), "uuid"),
        "{\"cmd\":\"Connect\",\"password\":\"p\\\\w\",\"game\":\"Metroid Prime\","
        "\"name\":\"P\\\"1\",\"uuid\":X,\"version\":{\"class\":\"Version\",\"major\":0,"
        "\"minor\":6,\"build\":8},\"items_handling\":7,\"tags\":[\"AP\",\"Tracker\","
        "\"DeathLink\"],\"slot_data\":true}");
    CHECK_GOLDEN(Mask(session.BuildBounce(), "time"),
        "{\"cmd\":\"Bounce\",\"tags\":[\"DeathLink\"],\"data\":{\"time\":X,\"source\":\"P\\\"1\","
        "\"cause\":\"P\\\"1 died\"}}");
    CHECK_GOLDEN(Mask(session.BuildBounce("fell \"down\""), "time"),
        "{\"cmd\":\"Bounce\",\"tags\":[\"DeathLink\"],\"data\":{\"time\":X,\"source\":\"P\\\"1\","
        "\"cause\":\"fell \\\"down\\\"\"}}");
    CHECK_GOLDEN(Session::BuildLocationChecks({1, -2, 9000000000000000001}),
        "{\"cmd\":\"LocationChecks\",\"locations\":[1,-2,9000000000000000001]}");
    CHECK_GOLDEN(Session::BuildLocationChecks({}), "{\"cmd\":\"LocationChecks\",\"locations\":[]}");
    CHECK_GOLDEN(Session::BuildLocationScouts({5, 6}),
        "{\"cmd\":\"LocationScouts\",\"locations\":[5,6],\"create_as_hint\":0}");
    CHECK_GOLDEN(Session::BuildSay(std::string("a") + '\x01' + "\xc3\xa9/"),
        "{\"cmd\":\"Say\",\"text\":\"a\\u0001\xc3""\xa9""/\"}");

    // A built-in slot whose seed turns DeathLink on, among other games.
    Session solo = MakeSession();
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    solo.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"S"})"), outgoing, grants);
    CHECK_GOLDEN(Mask(solo.BuildConnect(), "uuid"),
        "{\"cmd\":\"Connect\",\"password\":\"\",\"game\":\"Metroid Prime\",\"name\":\"Samus\","
        "\"uuid\":X,\"version\":{\"class\":\"Version\",\"major\":0,\"minor\":6,\"build\":8},"
        "\"items_handling\":7,\"tags\":[],\"slot_data\":true}");
    solo.HandlePacket(
        Packet(R"({"cmd":"Connected","team":2,"slot":3,"players":[{"team":2,"slot":3,"alias":"P","name":"P"}],)"
               R"("slot_info":{"1":{"game":"Other \"Game\""},"3":{"game":"Metroid Prime"},"4":{"game":"Zelda"}},)"
               R"("checked_locations":[],"slot_data":{"death_link":1,"artifact_locations":{}}})"),
        outgoing, grants);
    std::string all;
    for (const std::string& packet : outgoing)
      all += (packet.rfind("{\"cmd\":\"LocationScouts\"", 0) == 0 ? std::string("scouts") : packet) + "\n";
    CHECK_GOLDEN(all,
        "{\"cmd\":\"GetDataPackage\",\"games\":[\"Metroid Prime\"]}\n"
        "{\"cmd\":\"GetDataPackage\",\"games\":[\"Other \\\"Game\\\"\",\"Zelda\"]}\n"
        "{\"cmd\":\"ConnectUpdate\",\"tags\":[\"DeathLink\"]}\nscouts\n"
        "{\"cmd\":\"Get\",\"keys\":[\"_read_hints_2_3\"]}\n{\"cmd\":\"SetNotify\","
        "\"keys\":[\"_read_hints_2_3\"]}\n");
  }
}

} // namespace

int main() {
  const std::filesystem::path testDir =
      std::filesystem::temp_directory_path() / ("mp-solo-test-" + std::to_string(TestProcessId()));
  std::filesystem::remove_all(testDir);
  std::filesystem::create_directories(testDir);
  // Before anything asks for the user folder, which is resolved only once.
  const std::string userPath = testDir.string();
#ifdef _WIN32
  _putenv_s("MP_USER_PATH", userPath.c_str());
#else
  setenv("MP_USER_PATH", userPath.c_str(), 1);
#endif

  // Settings text.
  {
    PortRandoGen::Settings settings;
    PortRandoGen::Settings parsed;
    CHECK_MSG(PortRandoGen::ParseSettings(PortRandoGen::SettingsText(settings), parsed) && parsed == settings,
              "the default settings round-trip");
    settings.requiredArtifacts = 7;
    settings.finalBosses = 2;
    settings.artifactHints = false;
    settings.missileLauncher = true;
    settings.mainPowerBomb = true;
    settings.shuffleScanVisor = true;
    settings.preScanElevators = false;
    settings.elevatorRandomization = true;
    settings.doorColorRandomization = 2;
    settings.progressiveBeams = true;
    settings.nonVariaHeatDamage = false;
    settings.staggeredSuitDamage = 2;
    settings.combatLogic = -1;
    settings.trickDifficulty = 2;
    settings.trickAllow = {"Wallcrawl", "Say \"hi\""};
    settings.trickDeny = {"Bomb Jump"};
    settings.flaahgraPowerBombs = true;
    settings.backwardsLowerMines = true;
    settings.removeXray = 1;
    settings.removeThermal = 2;
    settings.removeHiveMecha = true;
    settings.springBall = false;
    settings.startingRoom = 2;
    settings.randomizeStartingBeam = true;
    settings.blastShieldRandomization = 2;
    settings.blastShieldFrequency = 6;
    settings.blastShieldAvailableTypes = 1;
    settings.lockedDoorCount = 2;
    settings.includePowerBeamDoors = true;
    settings.includeMorphBallBombDoors = true;
    CHECK_MSG(PortRandoGen::ParseSettings(PortRandoGen::SettingsText(settings), parsed) && parsed == settings,
              "changed settings round-trip");
    PortRandoGen::Settings other = settings;
    other.trickDeny.push_back("x");
    CHECK_MSG(other != settings, "a different list is a different setting");

    CHECK_MSG(PortRandoGen::ParseSettings("{}", parsed) && parsed == PortRandoGen::Settings(),
              "missing options are the defaults");
    CHECK_MSG(PortRandoGen::ParseSettings(
              R"({"required_artifacts":99,"final_bosses":-4,"trick_difficulty":50,"spring_ball":false})", parsed) &&
              parsed.requiredArtifacts == 12 && parsed.finalBosses == 0 && parsed.trickDifficulty == 2 &&
              !parsed.springBall,
          "out of range options are clamped");
    CHECK_MSG(!PortRandoGen::ParseSettings("[1,2]", parsed) && !PortRandoGen::ParseSettings("{", parsed),
              "malformed settings are refused");
  }

  // A small seed with known placements.
  size_t locationCount = 0;
  const Mp::Location* locations = Mp::Locations(locationCount);
  CHECK_MSG(locationCount >= 4, "the location table has locations");
  PortRandoGen::Seed seed;
  seed.name = "t1 /weird";
  seed.settings.requiredArtifacts = 5;
  seed.slotData = R"({"required_artifacts":5,"missile_launcher":0,"note":"a\"b","n":1.5})";
  seed.startItems = {Mp::kItemBase + Mp::kSpringBall};
  const int64_t items[4] = {Mp::kItemBase + Mp::kEnergyTank, Mp::kItemBase + Mp::kMissileLauncher,
                            Mp::kItemBase + Mp::kMissileExpansion, Mp::kItemBase + Mp::kArtifactTruth};
  for (int i = 0; i < 4; ++i)
    seed.placements[locations[i].id] = items[i];
  seed.spoiler = "line one\nline \"two\"\n";

  // Save and Load.
  {
    std::string error;
    CHECK_MSG(PortRandoGen::Save(seed, error), "a seed saves");
    const std::string path = PortRandoGen::SeedPath(seed.name);
    CHECK_MSG(path.find("weird") != std::string::npos && path.find(' ') == std::string::npos &&
                  path.find("t1 ") == std::string::npos &&
                  std::filesystem::path(path).parent_path() == std::filesystem::path(PortRandoGen::SeedDirectory()),
              "the seed's file name is made safe");
    CHECK_MSG(std::filesystem::exists(path) && !std::filesystem::exists(path + ".tmp"),
              "the seed file exists and no temporary is left");
    PortRandoGen::Seed loaded;
    CHECK_MSG(PortRandoGen::Load(path, loaded, error), "a seed loads");
    CHECK_MSG(loaded.name == seed.name && loaded.settings == seed.settings &&
                  loaded.startItems == seed.startItems && loaded.placements == seed.placements &&
                  loaded.spoiler == seed.spoiler,
              "a loaded seed matches the saved one");
    const PortJson::Value slot = Packet(loaded.slotData);
    CHECK_MSG(slot.Find("required_artifacts") != nullptr && slot.Find("required_artifacts")->AsInt() == 5 &&
                  slot.StringOr("note") == "a\"b" && slot.Find("n") != nullptr && slot.Find("n")->AsNumber() == 1.5,
              "slot data survives the round trip");
    CHECK_MSG(!PortRandoGen::Load((testDir / "nothing.json").string(), loaded, error) && !error.empty(),
              "a missing seed file is an error");
    {
      std::ofstream(testDir / "bad.json") << "{\"version\":2}";
    }
    CHECK_MSG(!PortRandoGen::Load((testDir / "bad.json").string(), loaded, error), "an unknown version is refused");
    PortRandoGen::Seed broken = seed;
    broken.slotData = "[1]";
    CHECK_MSG(!PortRandoGen::Save(broken, error), "slot data that is not an object is not saved");
    PortRandoGen::Seed again = seed;
    CHECK_MSG(PortRandoGen::Save(again, error) && again.name == seed.name, "the same seed saved again keeps its name");
    PortRandoGen::Seed other = seed;
    other.settings.requiredArtifacts = 6;
    CHECK_MSG(PortRandoGen::Save(other, error) && other.name == seed.name + "-2" &&
                  PortRandoGen::Load(PortRandoGen::SeedPath(seed.name), loaded, error) &&
                  loaded.settings == seed.settings,
              "a different seed with the same name gets a new one and leaves the old file");
    std::filesystem::remove(PortRandoGen::SeedPath(other.name));
    CHECK_MSG(PortRandoGen::SeedPath("x.state") != PortApSolo::StatePath("x"),
              "a seed named x.state doesn't take seed x's state file");
    PortRandoGen::Seed longSeed = seed;
    longSeed.name = std::string(80, 'L');
    PortRandoGen::Seed longOther = longSeed;
    longOther.settings.requiredArtifacts = 7;
    CHECK_MSG(PortRandoGen::Save(longSeed, error) && PortRandoGen::Save(longOther, error) &&
                  PortRandoGen::SeedPath(longOther.name) != PortRandoGen::SeedPath(longSeed.name),
              "a long name that is taken still gets a free one");
    std::filesystem::remove(PortRandoGen::SeedPath(longSeed.name));
    std::filesystem::remove(PortRandoGen::SeedPath(longOther.name));
  }

  // The handshake through the real session.
  const std::string statePath = PortApSolo::StatePath(seed.name);
  CHECK_MSG(statePath.find(".state.json") != std::string::npos, "the state file is beside the seed");
  std::filesystem::remove(statePath);
  PortAp::Protocol::State saved;
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.OpenSeed(seed, statePath, error), "the solo server opens");
    Session session = MakeSession();
    const std::vector<ItemGrant> grants = Pump(server, session);
    CHECK_MSG(session.HandshakeComplete(), "Connected is parsed");
    CHECK_MSG(session.GetSlotData().received && session.GetSlotData().requiredArtifacts == 5,
              "the seed's slot data reaches the session");
    CHECK_MSG(grants.size() == 1 && grants[0].itemId == seed.startItems[0] && grants[0].index == 0,
              "the start item is granted at index 0");
    int64_t item = 0;
    bool sameGame = false;
    int64_t flags = -1;
    CHECK_MSG(session.ScoutedAt(locations[1].id, item, sameGame, &flags) && item == items[1] && sameGame &&
                  flags == 1,
              "scouting a main item says progression");
    CHECK_MSG(session.ScoutedAt(locations[0].id, item, sameGame, &flags) && item == items[0] && flags == 0,
              "scouting an energy tank says filler");

    // A check grants its item at the next index.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[1].id}) + "]"), "a check is sent");
    std::vector<ItemGrant> next = Pump(server, session);
    CHECK_MSG(next.size() == 1 && next[0].itemId == items[1] && next[0].index == 1,
              "a check grants its item at index 1");
    // The same check again, and an unknown location, grant nothing.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[1].id, 42}) + "]"), "a repeat is sent");
    CHECK_MSG(Pump(server, session).empty(), "a repeated check grants nothing");
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[3].id}) + "]"), "a second check is sent");
    next = Pump(server, session);
    CHECK_MSG(next.size() == 1 && next[0].itemId == items[3] && next[0].index == 2,
              "the next check grants at index 2");
    CHECK_MSG(server.CheckedLocations() == std::vector<int64_t>({locations[1].id, locations[3].id}),
              "the server keeps the checks in order");

    // Sync replays everything; the session has it already.
    CHECK_MSG(server.SendText("[" + Session::BuildSync() + "]"), "a sync is sent");
    CHECK_MSG(Pump(server, session).empty(), "a sync grants nothing twice");

    // Chat, commands, the goal and the data storage keys.
    CHECK_MSG(server.SendText(R"([{"cmd":"Say","text":"hello"}])"), "chat is sent");
    CHECK_MSG(server.SendText(R"([{"cmd":"Say","text":"!hint"}])"), "a command is sent");
    CHECK_MSG(server.SendText("[" + Session::BuildGoal() + "]"), "the goal is sent");
    Pump(server, session);
    PortAp::Protocol::ChatLine line;
    std::vector<std::string> chat;
    while (session.TakeChatLine(line))
      chat.push_back(line.type + "|" + line.text);
    const auto has = [&chat](const std::string& wanted) {
      return std::any_of(chat.begin(), chat.end(),
                         [&wanted](const std::string& entry) { return entry.find(wanted) != std::string::npos; });
    };
    CHECK_MSG(has("Chat|Samus: hello"), "chat is echoed");
    CHECK_MSG(has("CommandResult|Usage: !hint <item>"), "a command answers");
    CHECK_MSG(has("Goal|Samus has completed their goal."), "the goal is announced");

    saved = session.GetState();
    CHECK_MSG(saved.nextItemIndex == 3 && saved.seed == "solo-t1 /weird", "the session recorded the seed and index");
    server.Close();
    std::string message;
    CHECK_MSG(!server.IsOpen() && !server.SendText("[]") && !server.ReceiveText(message, 1),
              "a closed server takes and gives nothing");
  }

  // A relaunch: the state file brings the checks back in order.
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.Open(seed.name, error), "the seed opens by name") ;
    CHECK_MSG(server.CheckedLocations() == std::vector<int64_t>({locations[1].id, locations[3].id}),
              "the checks come back from the state file");

    // The same client session reconnecting: the indices repeat, nothing is granted again.
    Session session = MakeSession(saved);
    CHECK_MSG(Pump(server, session).empty(), "a reconnect does not grant items twice");
    CHECK_MSG(session.GetState().nextItemIndex == 3, "the item index is unchanged by the reconnect");
    // The client repeats its checks after Connected; the server has them.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks(saved.checkedLocations) + "]"), "checks resent");
    CHECK_MSG(Pump(server, session).empty(), "resent checks grant nothing");
    // A new location after the reload carries on at the right index.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[0].id}) + "]"), "a new check is sent");
    const std::vector<ItemGrant> next = Pump(server, session);
    CHECK_MSG(next.size() == 1 && next[0].itemId == items[0] && next[0].index == 3,
              "a check after the reload grants at index 3");
  }
  {
    // A fresh client (a new save card) gets the whole inventory, in order.
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.Open(seed.name, error), "the seed opens again");
    Session session = MakeSession();
    const std::vector<ItemGrant> grants = Pump(server, session);
    CHECK_MSG(grants.size() == 4 && grants[0].itemId == seed.startItems[0] && grants[1].itemId == items[1] &&
                  grants[2].itemId == items[3] && grants[3].itemId == items[0],
              "a fresh client gets the start items, then every check's item in order");
    for (size_t i = 0; i < grants.size(); ++i)
      CHECK_MSG(grants[i].index == static_cast<int64_t>(i), "the indices count up from 0");
  }

  // Chat commands: the player is the solo room's admin.
  {
    const std::string commandState = (testDir / "commands.state.json").string();
    std::filesystem::remove(commandState);
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.OpenSeed(seed, commandState, error), "the seed opens for commands");
    Session session = MakeSession();
    Pump(server, session);
    std::vector<ItemGrant> grants;
    const auto say = [&](const std::string& text) {
      CHECK_MSG(server.SendText("[" + Session::BuildSay(text) + "]"), "a command is sent");
      grants = Pump(server, session);
      std::string reply;
      PortAp::Protocol::ChatLine line;
      while (session.TakeChatLine(line))
        reply += line.type + "|" + line.text + "\n";
      return reply;
    };
    const auto has = [](const std::string& reply, const char* wanted) {
      return reply.find(wanted) != std::string::npos;
    };
    std::string reply = say("!getitem energy TANK");
    CHECK_MSG(has(reply, "CommandResult|Cheat console: sending \"Energy Tank\" to Samus.") && grants.size() == 1 &&
                  grants[0].itemId == items[0] && grants[0].index == 1,
              "!getitem grants the item at the next index");
    reply = say(std::string("!admin /send_location Samus ") + locations[1].name);
    CHECK_MSG(has(reply, "Checked ") && grants.size() == 1 && grants[0].itemId == items[1] && grants[0].index == 2,
              "!admin /send_location checks the location and grants its item");
    reply = say(std::string("!send_location ") + locations[1].name);
    CHECK_MSG(has(reply, "was checked already") && grants.empty(), "a location is checked once");
    CHECK_MSG(has(say("!getitem no such thing"), "No item is called \"no such thing\".") && grants.empty(),
              "an unknown item is named back");
    CHECK_MSG(has(say(std::string("!hint_location ") + locations[3].name), "holds Artifact of Truth (not found)."),
              "!hint_location names the item there");
    CHECK_MSG(has(say("!hint missile launcher"), "Missile Launcher is at ") &&
                  has(say("!status"), "Samus (Metroid Prime): 1 of 4 locations checked.") &&
                  has(say("!admin login secret"), "no admin password") && has(say("!frob"), "Unknown command \"frob\"."),
              "hints, status, admin login and unknown commands answer");
    CHECK_GOLDEN(ReadText(commandState), "{\"checked\":[" + std::to_string(locations[1].id) +
                                             "],\"given\":[[0," + std::to_string(items[0]) + "]]}\n");
    server.Close();

    // The grant keeps its index across a relaunch, and !release sends the rest.
    PortApSolo::Server again;
    CHECK_MSG(again.OpenSeed(seed, commandState, error), "the seed reopens with its commands");
    Session fresh = MakeSession();
    grants = Pump(again, fresh);
    CHECK_MSG(grants.size() == 3 && grants[1].itemId == items[0] && grants[2].itemId == items[1],
              "a granted item comes back in its place");
    CHECK_MSG(again.SendText("[" + Session::BuildSay("/release") + "]"), "release sent");
    grants = Pump(again, fresh);
    CHECK_MSG(grants.size() == 3 && grants[0].index == 3 && again.CheckedLocations().size() == 4,
              "!release checks every location left");
  }

  // Wrong slot and server strings.
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.OpenSeed(seed, "", error), "a seed opens without a state file");
    std::string message;
    CHECK_MSG(server.ReceiveText(message, 20) && message.find("solo-t1 /weird") != std::string::npos &&
                  message.find("\"password\":false") != std::string::npos,
              "RoomInfo comes first");
    CHECK_MSG(server.SendText(R"([{"cmd":"Connect","name":"Someone","game":"Metroid Prime"}])"), "connect sent");
    CHECK_MSG(server.ReceiveText(message, 20) && message.find("ConnectionRefused") != std::string::npos,
              "another slot name is refused");
    CHECK_MSG(!server.ReceiveText(message, 5) && std::string(server.Error()) == "receive timed out",
              "an idle server times out");
    std::string seedName;
    CHECK_MSG(PortApSolo::ParseServer("solo:abc", seedName) && seedName == "abc" &&
                  !PortApSolo::ParseServer("solo:", seedName) && !PortApSolo::ParseServer("ws://solo:x", seedName),
              "solo server strings parse");
    PortApSolo::Server missing;
    CHECK_MSG(!missing.Open("no-such-seed", error) && !error.empty(), "a missing seed does not open");
  }

  Golden(testDir, seed);

  std::filesystem::remove_all(testDir);
  if (port_test::Failures() == 0)
    std::printf("[solo-tests] OK\n");
  return PORT_TEST_RESULT();
}
