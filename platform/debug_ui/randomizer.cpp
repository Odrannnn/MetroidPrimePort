// F1 overlay: The Randomizer tab. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_paths.h"
#include "port_apclient.h"
#include "port_discord.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace PortDebug {

// Trick names are comma-separated in the settings; the box shows them so.
std::string JoinNames(const std::vector< std::string >& names) {
  std::string text;
  for (const std::string& name : names) {
    text += (text.empty() ? "" : ", ") + name;
  }
  return text;
}

std::vector< std::string > SplitNames(const char* text) {
  std::vector< std::string > names;
  std::string item;
  for (const char* c = text;; ++c) {
    if (*c == ',' || *c == '\0') {
      item = Trim(item);
      if (!item.empty()) {
        names.push_back(item);
      }
      item.clear();
      if (*c == '\0') {
        break;
      }
    } else {
      item += *c;
    }
  }
  return names;
}

struct RandoSeedRow {
  std::string name;
  std::string summary;
  long long modified = 0;
};

std::string RandoSummary(const PortRandoGen::Settings& s) {
  static const char* const kBosses[] = {"both bosses", "Ridley", "Prime", "no boss"};
  std::string text = std::to_string(s.requiredArtifacts) + " artifacts, " +
                     kBosses[std::clamp(s.finalBosses, 0, 3)];
  if (s.elevatorRandomization) {
    text += ", elevators";
  }
  if (s.doorColorRandomization != 0) {
    text += s.doorColorRandomization == 1 ? ", doors global" : ", doors regional";
  }
  if (s.startingRoom != 0) {
    text += s.startingRoom == 1 ? ", safe start" : ", buckle-up start";
  }
  if (s.blastShieldRandomization != 0) {
    text += s.blastShieldRandomization == 1 ? ", shields replaced" : ", shields mixed";
  }
  if (s.lockedDoorCount > 0) {
    text += ", " + std::to_string(s.lockedDoorCount) + " locked";
  }
  if (s.trickDifficulty >= 0) {
    static const char* const kTricks[] = {"easy", "medium", "hard"};
    text += std::string(", tricks ") + kTricks[std::min(s.trickDifficulty, 2)];
  }
  return text;
}

std::vector< RandoSeedRow > ScanRandoSeeds() {
  std::vector< RandoSeedRow > rows;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(PortRandoGen::SeedDirectory(), ec)) {
    const std::string file = entry.path().filename().string();
    if (file.size() <= 5 || file.compare(file.size() - 5, 5, ".json") != 0 ||
        (file.size() > 11 && file.compare(file.size() - 11, 11, ".state.json") == 0)) {
      continue;
    }
    RandoSeedRow row;
    row.name = file.substr(0, file.size() - 5);
    PortRandoGen::Seed seed;
    std::string error;
    row.summary = PortRandoGen::Load(entry.path().string(), seed, error) ? RandoSummary(seed.settings)
                                                                          : "unreadable: " + error;
    const auto time = std::filesystem::last_write_time(entry.path(), ec);
    row.modified = ec ? 0 : static_cast< long long >(time.time_since_epoch().count());
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(),
            [](const RandoSeedRow& a, const RandoSeedRow& b) { return a.modified > b.modified; });
  return rows;
}

// The built-in randomizer: options, seed text, and the seeds made so far.
// Playing goes through the Archipelago client, so it replaces any session.
void DrawRandomizerTab() {
  static bool sScanned = false;
  static std::vector< RandoSeedRow > sRows;
  static char sSeedText[64];
  static char sAllow[256];
  static char sDeny[256];
  static bool sTextLoaded = false;
  static std::string sMessage;
  static bool sMessageError = false;
  static std::string sSpoilerName;
  static std::string sSpoilerText;
  static std::string sSpoilerNote;
  // The list is read again each time the page is opened (a frame passed
  // without it), so seeds made with the console's `rando gen` show up.
  static int sLastFrame = -2;
  if (ImGui::GetFrameCount() != sLastFrame + 1)
    sScanned = false;
  sLastFrame = ImGui::GetFrameCount();
  if (!sScanned) {
    sRows = ScanRandoSeeds();
    sScanned = true;
  }

  PortRandoGen::Settings s;
  {
    std::lock_guard< std::mutex > lock(sRandoMutex);
    s = sRandoSettings;
  }
  if (!sTextLoaded) {
    sTextLoaded = true;
    SDL_strlcpy(sAllow, JoinNames(s.trickAllow).c_str(), sizeof(sAllow));
    SDL_strlcpy(sDeny, JoinNames(s.trickDeny).c_str(), sizeof(sDeny));
  }
  bool changed = false;
  const auto check = [&](const char* label, bool& value, const char* help) {
    changed |= ImGui::Checkbox(label, &value);
    ItemHelp(help);
  };

  ImGui::TextWrapped(
      "Makes a seed from these options and plays it as a one-player Archipelago game with its own "
      "save card. The same options and seed text give the same seed everywhere.");

  ImGui::SeparatorText("Goal");
  changed |= ImGui::SliderInt("Required artifacts", &s.requiredArtifacts, 1, 12);
  ItemHelp("How many of the 12 Chozo artifacts open the way to the end.");
  changed |= ImGui::Combo("Final bosses", &s.finalBosses, "Ridley and Prime\0Ridley\0Prime\0None\0");
  ItemHelp("Which bosses must be beaten to finish. None finishes at the artifact temple.");
  check("Artifact hints", s.artifactHints, "The Artifact Temple totems say where each artifact is.");

  ImGui::SeparatorText("Start");
  changed |= ImGui::Combo("Starting room", &s.startingRoom, "Normal\0Safe\0Buckle up\0");
  ItemHelp("Normal starts at the Landing Site. Safe and Buckle up start in a random room with that "
           "room's items; Buckle up's rooms are harder to leave.");
  check("Random starting beam", s.randomizeStartingBeam,
        "Start with the Wave, Ice or Plasma Beam instead of the Power Beam.");

  ImGui::SeparatorText("Items");
  check("Missile Launcher", s.missileLauncher, "Missiles are useless until the Missile Launcher is found.");
  check("Main Power Bomb", s.mainPowerBomb, "Power bombs need the main Power Bomb item.");
  check("Shuffle Scan Visor", s.shuffleScanVisor, "The Scan Visor is an item to find, not a start item.");
  check("Progressive beams", s.progressiveBeams, "Each beam has upgrades received in order, not as named items.");
  check("Spring Ball", s.springBall, "Morph Ball Bombs also give the Spring Ball (jump in Morph Ball form).");
  check("Shuffle Unlimited Missiles", s.shuffleUnlimitedMissiles,
        "Adds an Unlimited Missiles item to the pool, in place of a Missile Expansion.");
  check("Shuffle Unlimited Power Bombs", s.shuffleUnlimitedPowerBombs,
        "Adds an Unlimited Power Bombs item to the pool, in place of a Missile Expansion.");
  changed |= ImGui::Combo("Remove X-Ray requirement", &s.removeXray,
                          "None\0Most\0All but the Omega Pirate\0");
  ItemHelp("Takes the X-Ray Visor out of the logic where it is needed to see hidden things.");
  changed |= ImGui::Combo("Remove Thermal requirement", &s.removeThermal, "None\0Most\0All\0");
  ItemHelp("Takes the Thermal Visor out of the logic where it is needed.");
  check("Remove Hive Mecha", s.removeHiveMecha, "Skips the Hive Mecha fight in Hive Totem.");

  ImGui::SeparatorText("World");
  check("Pre-scanned elevators", s.preScanElevators, "Elevator destinations are known without scanning them.");
  check("Elevator randomization", s.elevatorRandomization, "Elevators lead to other areas.");
  changed |= ImGui::Combo("Door colours", &s.doorColorRandomization, "None\0Global\0Regional\0");
  ItemHelp("Shuffles the coloured door locks, everywhere or within each area.");
  check("Power Beam doors", s.includePowerBeamDoors,
        "Door colours may turn a colour into Power Beam doors (with a random starting beam, the start "
        "beam's doors open to the Power Beam instead).");
  check("Morph Ball Bomb doors", s.includeMorphBallBombDoors,
        "With door colours, one colour of one area other than the start's opens to Morph Ball Bombs.");
  changed |= ImGui::Combo("Blast shields", &s.blastShieldRandomization, "None\0Replace existing\0Mix it up\0");
  ItemHelp("Replace existing gives the disc's missile shields random types. Mix it up puts random shields on "
           "doors all over each area instead.");
  ImGui::BeginDisabled(s.blastShieldRandomization != 2);
  int frequency = s.blastShieldFrequency >= 6 ? 2 : s.blastShieldFrequency >= 4 ? 1 : 0;
  if (ImGui::Combo("Blast shield frequency", &frequency, "Low (10%)\0Medium (40%)\0High (60%)\0")) {
    s.blastShieldFrequency = frequency == 0 ? 1 : frequency == 1 ? 4 : 6;
    changed = true;
  }
  ItemHelp("How many of each area's doors take a shield under Mix it up.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(s.blastShieldRandomization == 0);
  changed |= ImGui::Combo("Blast shield types", &s.blastShieldAvailableTypes, "No beam combos\0All\0");
  ItemHelp("All adds Flamethrower, Ice Spreader and Wavebuster shields, at most one per area.");
  ImGui::EndDisabled();
  changed |= ImGui::SliderInt("Locked doors", &s.lockedDoorCount, 0, 2);
  ItemHelp("How many areas (not Magmoor) get one door locked for good.");
  check("Backwards Lower Mines", s.backwardsLowerMines, "Phazon Mines' lower levels can be entered from the other end.");
  check("Flaahgra power bombs", s.flaahgraPowerBombs, "Flaahgra can be beaten with power bombs.");
  changed |= ImGui::SliderInt("Energy tank capacity", &s.etankCapacity, 10, 200);
  ItemHelp("Energy per tank (the suit alone holds 99). Combat logic still counts a tank as 100.");

  ImGui::SeparatorText("Logic");
  check("Heat damage without the Varia Suit", s.nonVariaHeatDamage, "Hot rooms hurt without Varia, so other suits or tanks can carry you through.");
  changed |= ImGui::Combo("Staggered suit damage", &s.staggeredSuitDamage, "Default\0Progressive\0Additive\0");
  ItemHelp("How the suits split damage between them.");
  int combat = s.combatLogic + 1;
  if (ImGui::Combo("Combat logic", &combat, "None\0Normal\0Minimal\0")) {
    s.combatLogic = combat - 1;
    changed = true;
  }
  ItemHelp("How strictly the logic expects you to have the gear to win fights.");
  int tricks = s.trickDifficulty + 1;
  if (ImGui::Combo("Trick difficulty", &tricks, "None\0Easy\0Medium\0Hard\0")) {
    s.trickDifficulty = tricks - 1;
    changed = true;
  }
  ItemHelp("Sequence breaks the logic may expect, up to this difficulty.");
  if (ImGui::InputText("Allow tricks", sAllow, sizeof(sAllow))) {
    s.trickAllow = SplitNames(sAllow);
    changed = true;
  }
  ItemHelp("Trick names, separated by commas, always expected whatever the difficulty.");
  if (ImGui::InputText("Deny tricks", sDeny, sizeof(sDeny))) {
    s.trickDeny = SplitNames(sDeny);
    changed = true;
  }
  ItemHelp("Trick names, separated by commas, never expected whatever the difficulty.");

  if (changed) {
    {
      std::lock_guard< std::mutex > lock(sRandoMutex);
      sRandoSettings = s;
    }
    MarkDirty();
  }
  if (ImGui::Button("Reset options")) {
    {
      std::lock_guard< std::mutex > lock(sRandoMutex);
      sRandoSettings = PortRandoGen::Settings();
    }
    sTextLoaded = false;
    MarkDirty();
  }

  ImGui::SeparatorText("New seed");
  ImGui::InputTextWithHint("Seed", "empty picks a random one", sSeedText, sizeof(sSeedText));
  ItemHelp("Any text. The same text with the same options makes the same seed.");
  ImGui::SameLine();
  if (ImGui::Button("Random")) {
    sSeedText[0] = '\0';
  }
  // A loaded game keeps its save card until the title screen, so seeds are
  // only switched there (PortAp::PlaySolo refuses too).
  const bool inGame = PortDebug::StateManager() != nullptr;
  const std::string inPlay = PortAp::SoloSeedInPlay();
  if (ImGui::Button(inGame ? "Generate" : "Generate & Play")) {
    PortRandoGen::Seed seed;
    std::string error;
    sMessageError = true;
    if (!PortRandoGen::Generate(s, sSeedText, seed, error) || !PortRandoGen::Save(seed, error)) {
      sMessage = error;
    } else if (inGame) {
      sMessageError = false;
      sMessage = "Saved seed " + seed.name + ". Quit to the title screen to play it.";
    } else if (!PortAp::PlaySolo(seed.name, error)) {
      sMessage = error;
    } else {
      sMessageError = false;
      sMessage = "Playing seed " + seed.name;
    }
    sScanned = false;
  }
  ItemHelp(inGame ? "Makes the seed and saves it. Quit to the title screen to play it."
                  : "Makes the seed, saves it and starts playing it. If an Archipelago session is running, "
                    "it is replaced.");
  if (!sMessage.empty()) {
    ImGui::TextColored(sMessageError ? ImVec4(1.f, 0.4f, 0.4f, 1.f) : ImVec4(0.5f, 1.f, 0.5f, 1.f), "%s",
                       sMessage.c_str());
  }

  static constexpr const char* kPlayHelp = "Plays this seed with its own save card. A running Archipelago session "
                                           "is replaced. Seeds are switched at the title screen.";
  static constexpr const char* kDeleteHelp = kInlineHelp
      ? "Delete removes the seed, its progress and its save card (not the seed being played)."
      : "Deletes the seed, its progress and its save card.";
  ImGui::SeparatorText("Seeds");
  if (!inPlay.empty()) {
    // The way back is the Archipelago page's Disconnect: the saved connection is
    // switched off, so the game plays its own save card again.
    ImGui::Text("Playing seed %s.", inPlay.c_str());
    ImGui::BeginDisabled(inGame);
    if (ImGui::Button("Play the normal game")) {
      std::string error;
      sMessageError = !PortAp::Disconnect(error);
      sMessage = sMessageError ? error : "Playing the normal game. The seed and its progress are kept.";
    }
    ImGui::EndDisabled();
    ItemHelp(inGame ? "Quit to the title screen to go back to the normal game."
                    : "Stops playing the seed and goes back to the normal game and its save card. "
                      "The seed keeps its progress: Play resumes it.");
  }
  if (sRows.empty()) {
    ImGui::TextDisabled("No seeds yet.");
  } else if (inGame) {
    ImGui::TextDisabled("Quit to the title screen to switch seeds.");
  }
  if (kInlineHelp && !sRows.empty()) {
    // Inline help under each button would push the rest of the row off the right edge.
    ItemHelp(kPlayHelp);
    ItemHelp(kDeleteHelp);
  }
  // Keeps the row's next button on this line only if it fits, else wraps it.
  const auto sameLineIfFits = [](const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.f;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right) {
      ImGui::SameLine();
    }
  };
  bool deleted = false;
  for (const RandoSeedRow& row : sRows) {
    const bool current = !inPlay.empty() && PortAp::SameSoloSeed(inPlay, row.name);
    ImGui::PushID(row.name.c_str());
    if (current) {
      ImGui::Text("%s (playing)", row.name.c_str());
    } else {
      ImGui::TextUnformatted(row.name.c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", row.summary.c_str());
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(inGame && !current);
    if (ImGui::Button("Play")) {
      std::string error;
      sMessageError = !PortAp::PlaySolo(row.name, error);
      sMessage = sMessageError ? error : "Playing seed " + row.name;
    }
    ImGui::EndDisabled();
    if (!kInlineHelp) {
      ItemHelp(kPlayHelp);
    }
    sameLineIfFits("Spoiler");
    if (ImGui::Button("Spoiler")) {
      PortRandoGen::Seed seed;
      std::string error;
      sSpoilerName = row.name;
      sSpoilerNote.clear();
      sSpoilerText = PortRandoGen::Load(PortRandoGen::SeedPath(row.name), seed, error) ? seed.spoiler
                                                                                       : "Could not read: " + error;
    }
    sameLineIfFits("Write spoiler file");
    if (ImGui::Button("Write spoiler file")) {
      PortRandoGen::Seed seed;
      std::string error;
      sSpoilerName = row.name;
      if (!PortRandoGen::Load(PortRandoGen::SeedPath(row.name), seed, error)) {
        sSpoilerNote = "Could not read: " + error;
      } else {
        const std::string path = PortRandoGen::SeedDirectory() + "/" + row.name + ".spoiler.txt";
        std::ofstream out(path, std::ios::trunc);
        out << seed.spoiler;
        sSpoilerNote = out.good() ? "Wrote " + path : "Could not write " + path;
      }
    }
    sameLineIfFits("Delete");
    ImGui::BeginDisabled(current);
    if (ImGui::Button("Delete")) {
      ImGui::OpenPopup("Delete seed?");
    }
    ImGui::EndDisabled();
    if (!kInlineHelp) {
      ItemHelp(current ? "This seed is being played. Play another seed or disconnect first." : kDeleteHelp);
    }
    if (ImGui::BeginPopupModal("Delete seed?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Delete seed %s, its progress and its save card?", row.name.c_str());
      ImGui::TextUnformatted("This can't be undone.");
      if (ImGui::Button("Delete")) {
        std::string error;
        sMessageError = !PortAp::DeleteSolo(row.name, error);
        sMessage = sMessageError ? error : "Deleted seed " + row.name;
        if (sSpoilerName == row.name) {
          sSpoilerText.clear();
          sSpoilerNote.clear();
        }
        sScanned = false;
        deleted = true;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Separator();
    if (deleted) {
      break; // sRows is read again next frame
    }
  }
  if (!sSpoilerNote.empty()) {
    ImGui::TextWrapped("%s", sSpoilerNote.c_str());
  }
  if (!sSpoilerText.empty()) {
    ImGui::SeparatorText("Spoiler");
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "This shows where everything is in seed %s.",
                       sSpoilerName.c_str());
    if (ImGui::Button("Hide")) {
      sSpoilerText.clear();
    } else if (ImGui::BeginChild("randoSpoiler", ImVec2(0, ImGui::GetFontSize() * 24.f), ImGuiChildFlags_Borders)) {
      ImGui::TextUnformatted(sSpoilerText.c_str());
    }
    if (!sSpoilerText.empty()) {
      ImGui::EndChild();
    }
  }
}

} // namespace PortDebug
