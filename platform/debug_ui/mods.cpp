// F1 overlay: The Remastered import, importers and mods sections. State and helpers shared with the
// other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_pointer.h"
#include "port_mods.h"
#include "port_importers.h"
#include "port_remastered_import.h"
#include "port_dread_samus_mod.h"
#include "port_remastered_text.h"
#include "port_savestate.h"

#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <imgui_internal.h>
#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace PortDebug {

namespace {
// The console's key file. One file serves both imports, so it has one buffer, one row and one
// pick; the value is kept as the Remastered import's setting (sRemasteredKeysPath). A typed path
// on desktop; on Android the descriptor of the picked file ("fd:<n>", OpenRemasteredPick slot 1).
char sKeys[1024] = "";
#if defined(__ANDROID__)
std::string sKeysName;
#endif
} // namespace

// The key file row: both imports use the same prod.keys, so the page draws it once, above both
// sections and inside neither, and it stays visible whichever of them is open. Its buffer is
// filled and its pick taken here too, so the row is right while both sections are closed.
void DrawImportKeys() {
  static bool sFilled = false;
  if (!sFilled) {
    sFilled = true;
#if defined(__ANDROID__)
    // The file picked last time, or one picked just before the process was killed, opens again.
    std::lock_guard lock(sRemasteredPickMutex);
    std::ifstream in(RemasteredPickFile(1));
    std::string uri;
    if (std::getline(in, uri) && !uri.empty() && sRemasteredKeysPath != uri) {
      sRemasteredKeysPath = uri;
      MarkDirty();
    }
    std::remove(RemasteredPickFile(1).c_str());
    if (!sRemasteredKeysPath.empty()) {
      const std::string opened = OpenRemasteredPick(1, sRemasteredKeysPath);
      std::snprintf(sKeys, sizeof(sKeys), "%s", opened.c_str());
      sKeysName = opened.empty() ? "could not be opened" : RemasteredPickName(sRemasteredKeysPath);
    }
#else
    // The default key file when the player never picked one.
    std::snprintf(sKeys, sizeof(sKeys), "%s", PortRemastered::DefaultKeysPath().c_str());
    if (!sRemasteredKeysPath.empty()) {
      std::snprintf(sKeys, sizeof(sKeys), "%s", sRemasteredKeysPath.c_str());
    }
#endif
  }
  {
    // Only the key picks: the image picks stay for the section that shows them.
    std::lock_guard lock(sRemasteredPickMutex);
    for (auto pick = sRemasteredPicks.begin(); pick != sRemasteredPicks.end();) {
      if (pick->first != 1) {
        ++pick;
        continue;
      }
      const std::string path = pick->second;
      pick = sRemasteredPicks.erase(pick);
#if defined(__ANDROID__)
      const std::string opened = OpenRemasteredPick(1, path);
      std::snprintf(sKeys, sizeof(sKeys), "%s", opened.c_str());
      sKeysName = opened.empty() ? "could not be opened" : RemasteredPickName(path);
      if (!opened.empty() && sRemasteredKeysPath != path) {
        sRemasteredKeysPath = path;
        MarkDirty();
      }
      std::remove(RemasteredPickFile(1).c_str());
#else
      std::snprintf(sKeys, sizeof(sKeys), "%s", path.c_str());
      if (sRemasteredKeysPath != path) {
        sRemasteredKeysPath = path;
        MarkDirty();
      }
#endif
    }
  }
  ImGui::TextUnformatted("Your Nintendo Switch's prod.keys");
#if defined(__ANDROID__)
  if (ImGui::Button("Pick the keys...##import-keys")) {
    OpenRemasteredDialog(1);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sKeysName.empty() ? "prod.keys" : sKeysName.c_str());
#else
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##import-keys", "prod.keys", sKeys, sizeof(sKeys));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##import-keys")) {
    OpenRemasteredDialog(1);
  }
#endif
  ItemHelp("One file: both the Remastered import and the Dread import read it.");
}

void DrawRemasteredImport() {
  static char sImage[1024] = "";
#if defined(__ANDROID__)
  static std::string sImageName;
#endif
  static bool sFilled = false;
  const auto remember = [](int which, const std::string& path) {
    std::string& saved = which == 0 ? sRemasteredImagePath : sRemasteredKeysPath;
    if (!path.empty() && saved != path) {
      saved = path;
      MarkDirty();
    }
  };
  if (!sFilled) {
    sFilled = true;
#if defined(__ANDROID__)
    // The files picked last time open again as if picked now. The key file is the shared row's
    // (DrawImportKeys), which owns its own pick.
    std::lock_guard lock(sRemasteredPickMutex);
    if (!sRemasteredImagePath.empty()) {
      sRemasteredPicks.emplace_back(0, sRemasteredImagePath);
    }
    TakeRemasteredPickFiles();
#else
    if (!sRemasteredImagePath.empty()) {
      std::snprintf(sImage, sizeof(sImage), "%s", sRemasteredImagePath.c_str());
    }
#endif
  }
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    for (const auto& [which, path] : sRemasteredPicks) {
      const std::string opened = OpenRemasteredPick(which, path);
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", opened.c_str());
      (which == 0 ? sImageName : sKeysName) = opened.empty() ? "could not be opened" : RemasteredPickName(path);
      if (!opened.empty()) {
        remember(which, path);
      }
      std::remove(RemasteredPickFile(which).c_str());
    }
#else
    for (const auto& [which, path] : sRemasteredPicks) {
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", path.c_str());
      remember(which, path);
    }
#endif
    sRemasteredPicks.clear();
  }
  // Drawn straight into the Prime Remastered section body: no header of its own in between.
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  ImGui::TextWrapped(
      "Converts the models of your own Metroid Prime Remastered copy into a mod. Needs the game's "
      ".nsp or .xci and your console's prod.keys; takes a few minutes.");
#if defined(__ANDROID__)
  ImGui::TextDisabled(
      "It is installed into the app's own storage, not the mods folder. An app update keeps it; an "
      "uninstall or \"clear data\" removes it, so it must be imported again.");
  ImGui::TextDisabled("A remastered-models folder in the mods folder keeps working as a fallback "
                      "until a full import installs the app's own copy.");
#endif
  if (PortMods::StaleRemasteredImport() != nullptr) {
    ImGui::TextColored(ThemeWarnColor(),
                       "Re-import needed: the installed models were made by an older version.");
  }
  bool performance = GXPortGetPerformance() != 0;
  if (ImGui::Checkbox("Performance mode##remastered", &performance)) {
    GXPortSetPerformance(performance ? GX_PORT_PERF_ALL : 0);
    MarkDirty();
  }
  ItemHelp("Faster drawing that is no longer exactly Remastered's (at most 2/255 apart): glass "
           "refraction updated every second frame, exposure measured from a smaller image, "
           "volumetric fog blended onto the scene instead of rewriting it, and the world left "
           "unshaded behind the arm cannon.");
  if (ImGui::Checkbox("Phazon Suit aura##remastered", &sPhazonShell)) {
    MarkDirty();
  }
  ItemHelp("On: the Phazon Suit wears Remastered's glowing aura (its silhouette shell). Off: the "
           "original game's distortion effect around the suit instead.");
  ImGui::BeginDisabled(state.running);
#if defined(__ANDROID__)
  // No path to type here: the files are picked, and shown by name.
  if (ImGui::Button("Pick the .nsp/.xci...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sImageName.empty() ? "Metroid Prime Remastered .nsp or .xci" : sImageName.c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Android may close the game while you pick a file. The pick is kept: open this page again.");
  ImGui::PopStyleColor();
#else
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-image", "Metroid Prime Remastered .nsp or .xci", sImage, sizeof(sImage));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
#endif
  ImGui::EndDisabled();
  if (state.running) {
    // The whole import, step by step, and within a step as far as it can say.
    const float within = state.stepTotal > 0 ? std::min(1.f, float(state.stepDone) / float(state.stepTotal)) : 0.f;
    float fraction = state.total > 0 ? float(state.done) / float(state.total) : 0.f;
    std::string label = state.cancelling ? "Cancelling..." : state.message;
    if (state.steps > 0 && state.step > 0) {
      fraction = (float(state.step - 1) + within) / float(state.steps);
      if (!state.cancelling) {
        label = "Step " + std::to_string(state.step) + " of " + std::to_string(state.steps) + ": " + state.message;
      }
    }
    ImGui::ProgressBar(fraction, ImVec2(-1.f, 0.f), label.c_str());
    if (sImportUnloadedMods) {
      ImGui::TextDisabled("The mods are unloaded until the import ends.");
      DrawModReloadMessage();
    }
    ImGui::BeginDisabled(state.cancelling || state.installing);
    if (ImGui::Button("Cancel##remastered")) {
      PortRemastered::CancelImport();
    }
    ImGui::EndDisabled();
    if (state.cancelling) {
      ImGui::SameLine();
      ImGui::TextDisabled("Stopping at the next file, which can take a moment.");
    }
  } else {
    if (ImGui::TreeNode("remastered-parts", "What to import")) {
      // Kept as the parts left out, so a part added later is imported by default.
      PortRemastered::ImportParts parts = RemasteredImportParts();
      bool changed = false;
      changed |= ImGui::Checkbox("Room geometry", &parts.geometry);
      ItemHelp("Every model the rooms place. Most of the import's size (about 5.5 of 6.5 GB) and "
               "time; restart the game to draw it.");
      changed |= ImGui::Checkbox("Particle effects", &parts.effects);
      changed |= ImGui::Checkbox("HUD", &parts.hud);
      changed |= ImGui::Checkbox("Text", &parts.text);
      changed |= ImGui::Checkbox("Movies", &parts.movies);
      changed |= ImGui::Checkbox("Gallery", &parts.gallery);
      if (changed) {
        SetRemasteredImportParts(parts);
      }
      ImGui::TextDisabled("Applies to the next import.");
      ImGui::TreePop();
    }
    ImGui::BeginDisabled(sImage[0] == '\0' || sKeys[0] == '\0');
#if !defined(__ANDROID__)
    // A typed path is kept once it is used (Android keeps what was picked).
    const auto rememberTyped = [&] {
      remember(0, sImage);
      remember(1, sKeys);
    };
#else
    const auto rememberTyped = [] {};
#endif
    if (ImGui::Button("Import##remastered")) {
      rememberTyped();
      StartRemasteredImport(sImage, sKeys);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The mods are unloaded while it runs, and loaded again with the new import when it ends.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool remakeAll = PortRemastered::RemakeAll();
    if (ImGui::Checkbox("Full import (no reuse)##remastered", &remakeAll)) {
      PortRemastered::SetRemakeAll(remakeAll);
    }
    ItemHelp("Converts everything again instead of reusing the previous import, so it takes as "
             "long as the first. Lasts until the game is closed.");
    if (state.finished && state.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)));
      ImGui::TextWrapped("%s", state.message.c_str());
      ImGui::PopStyleColor();
      if (state.failed > 0) {
        ImGui::TextColored(ThemeWarnColor(), "%d of %d models could not be converted and keep the disc's look; "
                           "the notes below say why.", state.failed, state.total);
      }
      // It loads on its own when the import ends; this is to load it again
      // by hand.
      if (ImGui::Button("Load it again##remastered")) {
        PortSaveState::RequestModReload();
      }
      DrawModReloadMessage();
    } else if (state.finished && state.cancelled) {
      ImGui::TextDisabled("The import was cancelled.");
    } else if (state.finished) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeBadColor());
      ImGui::TextWrapped("The import failed: %s", state.message.c_str());
      ImGui::PopStyleColor();
    }
  }
  if (!state.lines.empty() && ImGui::TreeNode("remastered-log", "%d notes", int(state.lines.size()))) {
    if (ImGui::SmallButton("Copy the notes##remastered")) {
      std::string all;
      for (const std::string& line : state.lines) {
        all += line;
        all += '\n';
      }
      ImGui::SetClipboardText(all.c_str());
    }
    if (state.lines.size() >= PortRemastered::kImportMaxLines) {
      ImGui::SameLine();
      ImGui::TextDisabled("(the newest %d)", int(PortRemastered::kImportMaxLines));
    }
    for (const std::string& line : state.lines) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::TreePop();
  }
}

// The Dread Samus import (port_dread_samus.h): Samus's Power, Varia, Gravity and Phazon bodies from the
// player's own Metroid Dread into the dread-samus mod. Same progress, notes and mods reload as the
// Remastered import, with which it shares one worker (only one runs at a time).
void DrawDreadSamusImport() {
  static char sImage[1024] = "";
  static bool sStarted = false;  // the shared state is this import's
#if defined(__ANDROID__)
  static bool sFilled = false;
  // Picked content URIs, opened as "fd:<n>" (OpenRemasteredPick). The keys are the shared pick
  // (sRemasteredKeysPath), opened again here into a descriptor of their own.
  static char sDreadKeys[1024] = "";
  static std::string sImageName;
  static std::string sKeysOpened;  // the URI sDreadKeys holds a descriptor of
  if (!sFilled) {
    sFilled = true;
    // The pick file is kept, so the image picked last time (or behind a killed process) opens again.
    std::lock_guard lock(sRemasteredPickMutex);
    std::ifstream in(RemasteredPickFile(kDreadPick));
    std::string uri;
    if (sDreadPick.empty() && std::getline(in, uri) && !uri.empty()) {
      sDreadPick = uri;
    }
  }
  {
    std::lock_guard lock(sRemasteredPickMutex);
    if (!sDreadPick.empty()) {
      const std::string opened = OpenRemasteredPick(kDreadPick, sDreadPick);
      std::snprintf(sImage, sizeof(sImage), "%s", opened.c_str());
      sImageName = opened.empty() ? "could not be opened" : RemasteredPickName(sDreadPick);
      sDreadPick.clear();
    }
  }
  if (sKeysOpened != sRemasteredKeysPath) {
    sKeysOpened = sRemasteredKeysPath;
    const std::string opened = OpenRemasteredPick(kDreadKeysSlot, sKeysOpened);
    std::snprintf(sDreadKeys, sizeof(sDreadKeys), "%s", opened.c_str());
  }
#else
  char* const sDreadKeys = sKeys;
  {
    std::lock_guard lock(sRemasteredPickMutex);
    if (!sDreadPick.empty()) {
      std::snprintf(sImage, sizeof(sImage), "%s", sDreadPick.c_str());
      sDreadPick.clear();
    }
  }
#endif
  // Drawn straight into the Metroid Dread section body, like the Remastered import in its own.
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  ImGui::TextWrapped(
      "Puts Samus's Power, Varia, Gravity and Phazon Suit bodies and Morph Balls from your own Metroid Dread in place "
      "of Prime's. Needs the game's .nsp or .xci and your console's prod.keys. "
      "Phazon takes Dread's Metroid Suit. It is the dread-samus mod: switch it off above.");
  ImGui::BeginDisabled(state.running);
#if defined(__ANDROID__)
  if (ImGui::Button("Load Metroid Dread image##dread-image")) {
    OpenRemasteredDialog(kDreadPick);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sImageName.empty() ? "Metroid Dread .nsp or .xci" : sImageName.c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Android may close the game while you pick a file. The pick is kept: open this page again.");
  ImGui::PopStyleColor();
#else
  // The button's label is long, so the path field gives up the width it needs: a narrow window
  // would clip the label otherwise.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float buttonWidth = ImGui::CalcTextSize("Load Metroid Dread image").x + style.FramePadding.x * 2.f;
  ImGui::SetNextItemWidth(std::max(120.f, std::min(avail * 0.6f, avail - buttonWidth - style.ItemSpacing.x)));
  ImGui::InputTextWithHint("##dread-image", "Metroid Dread .nsp or .xci", sImage, sizeof(sImage));
  ImGui::SameLine();
  if (ImGui::Button("Load Metroid Dread image##dread-image")) {
    OpenRemasteredDialog(kDreadPick);
  }
#endif
  ImGui::BeginDisabled(sImage[0] == '\0' || sDreadKeys[0] == '\0');
  if (ImGui::Button("Import##dread")) {
    // A start refused for a missing mods folder or file says why in the shared state; one refused
    // because the Remastered import runs leaves that import's state, which is not this one's.
#if !defined(__ANDROID__)
    // A typed key path is kept once used, as the Remastered import keeps it.
    if (sRemasteredKeysPath != sKeys) {
      sRemasteredKeysPath = sKeys;
      MarkDirty();
    }
#endif
    sStarted = StartDreadSamusImport(sImage, sDreadKeys) || !state.running;
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (!sStarted) {
    return;
  }
  if (state.running) {
    const float fraction = state.total > 0 ? float(state.done) / float(state.total) : 0.f;
    ImGui::ProgressBar(fraction, ImVec2(-1.f, 0.f), state.cancelling ? "Cancelling..." : state.message.c_str());
    ImGui::BeginDisabled(state.cancelling || state.installing);
    if (ImGui::Button("Cancel##dread")) {
      PortRemastered::CancelImport();
    }
    ImGui::EndDisabled();
  } else if (state.finished && state.ok) {
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)));
    ImGui::TextWrapped("%s", state.message.c_str());
    ImGui::PopStyleColor();
    DrawModReloadMessage();
  } else if (state.finished && state.cancelled) {
    ImGui::TextDisabled("The import was cancelled.");
  } else if (state.finished) {
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeBadColor());
    ImGui::TextWrapped("The import failed: %s", state.message.c_str());
    ImGui::PopStyleColor();
  }
  if (!state.lines.empty() && ImGui::TreeNode("dread-log", "%d notes", int(state.lines.size()))) {
    for (const std::string& line : state.lines) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::TreePop();
  }
}

namespace {
#if !defined(__ANDROID__)
// Importers (port_importers.h): the user's own programs that build a mod.
// Nothing is drawn until the importers folder holds one.
void DrawImporters() {
  // The folder is read when the panel opens and after a run, not every frame.
  static std::vector<std::string> sNames;
  static int sListedFrame = -2;
  static char sArgument[512] = "";
  const int frame = ImGui::GetFrameCount();
  const PortImporters::State& state = PortImporters::Poll();
  if (sListedFrame != frame - 1 || ImGui::IsWindowAppearing()) {
    sNames = PortImporters::List();
  }
  sListedFrame = frame;
  if (sNames.empty() && !state.running && !state.finished) {
    return;
  }
  ImGui::SeparatorText("Importers");
  ImGui::BeginDisabled(state.running);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("Argument", "optional, e.g. the file to import", sArgument, sizeof(sArgument));
  ItemHelp("An importer is a program in the importers folder beside the mods folder that builds a "
           "mod from your own files. The argument is passed to it.");
  for (const std::string& name : sNames) {
    if (ImGui::Button(("Run " + name).c_str())) {
      PortImporters::Start(name, sArgument);
    }
  }
  ImGui::EndDisabled();
  if (state.running) {
    ImGui::TextColored(ThemeWarnColor(), "%s is running...", state.name.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      PortImporters::Cancel();
    }
  } else if (state.finished && state.exitCode == 0) {
    ImGui::TextColored(ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)), "%s finished. Reload the mods to load it.",
                       state.name.c_str());
  } else if (state.finished && state.cancelled) {
    ImGui::TextDisabled("%s was cancelled.", state.name.c_str());
  } else if (state.finished) {
    ImGui::TextColored(ThemeBadColor(), "%s failed (exit code %d).", state.name.c_str(), state.exitCode);
  }
  if (!state.lines.empty()) {
    // The tail of the output; a failure shows more of it.
    const size_t shown = std::min<size_t>(state.lines.size(), state.finished && state.exitCode != 0 && !state.cancelled ? 12 : 3);
    for (size_t i = state.lines.size() - shown; i < state.lines.size(); ++i) {
      ImGui::TextWrapped("%s", state.lines[i].c_str());
    }
  }
}
#endif
} // namespace

void DrawMods() {
  const PortMods::Status& status = PortMods::CurrentStatus();
  ImGui::SeparatorText("Mods");
  if (sOriginalExperience) {
    ImGui::TextDisabled("Original experience is on: no mods are loaded.");
  }
  bool enabled = sModsEnabled;
  if (ImGui::Checkbox("Load mods", &enabled)) {
    SetModsEnabled(enabled);
  }
  ItemHelp("Each folder in the mods folder is a mod; later names win. A file at a disc path "
           "(Metroid1.pak, Audio/...) replaces that file. Mods load at startup.");
  // What the settings would load next time, against what this launch loaded: every mod, the
  // built-in ones included, so their switches share this line and button.
  bool changed = ModsEnabled() != status.active;
  bool restartRequired = false;
  {
    const std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
    for (const PortMods::ModInfo& mod : status.mods) {
      const bool on = std::find(disabled.begin(), disabled.end(), mod.name) == disabled.end();
      const bool wanted = ModsEnabled() && on;
      restartRequired = restartRequired || (wanted && mod.pendingRestart);
      changed = changed || (wanted != mod.enabled && !(wanted && mod.pendingRestart));
      changed = changed || (mod.enabled && mod.import && !PortMods::SameImportParts(mod.hidden, RemasteredHidden()));
      // Left out, the room geometry no longer needs the buffers a restart would reserve.
      changed = changed || (wanted && mod.pendingRestart && mod.import &&
                            PortMods::ImportPartHidden(RemasteredHidden(), "geometry"));
    }
  }
  if (PortMods::Suspended()) {
    ImGui::TextColored(ThemeWarnColor(), "Unloaded while an import runs; they load again when it ends.");
  } else {
    if (changed) {
      ImGui::TextColored(ThemeWarnColor(), "Reload the mods to apply.");
    }
    if (restartRequired) {
      ImGui::TextColored(ThemeWarnColor(), "Restart the application to apply pending room geometry mods.");
    }
  }
  if (ImGui::Button("Reload mods")) {
    PortSaveState::RequestModReload();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reads the mods folder again and reloads the room, as a save state does.");
  }
#if !defined(__ANDROID__)
  if (!status.folder.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
      std::string url = status.folder.front() == '/' ? "file://" : "file:///";
      for (const char c : status.folder) {
        url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
      }
      SDL_OpenURL(url.c_str());
    }
  }
#endif
  DrawModReloadMessage();
  for (const std::string& message : status.messages) {
    ImGui::TextColored(ThemeBadColor(), "%s", message.c_str());
  }

  DrawBuiltinMods();

  // The mods folder's other mods: the port's own are the sections above.
  ImGui::SeparatorText("Other mods");
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  bool listed = false;
  for (const PortMods::ModInfo& mod : status.mods) {
    if (mod.name == PortRemastered::kImportModName || mod.name == PortDreadSamus::kModName) {
      continue;
    }
    listed = true;
    const auto found = std::find(disabled.begin(), disabled.end(), mod.name);
    bool on = found == disabled.end();
    ImGui::BeginDisabled(!sModsEnabled);
    if (ImGui::Checkbox(mod.name.c_str(), &on)) {
      if (on) {
        disabled.erase(std::remove(disabled.begin(), disabled.end(), mod.name), disabled.end());
      } else {
        disabled.push_back(mod.name);
      }
      SetModsDisabled(PortMods::JoinDisabled(disabled));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (mod.enabled) {
      if (mod.textures > 0) {
        ImGui::TextDisabled("%d file(s), %d resource(s), %d native texture(s)", mod.files, mod.resources, mod.textures);
      } else {
        ImGui::TextDisabled("%d file(s), %d resource(s)", mod.files, mod.resources);
      }
    } else if (ModsEnabled() && on && mod.pendingRestart) {
      ImGui::TextColored(ThemeWarnColor(), "not loaded: Restart the application to apply.");
    } else {
      ImGui::TextDisabled("not loaded");
    }
    if (mod.importStale) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "re-import needed");
    }
  }
  if (!listed) {
    ImGui::TextDisabled("No other mods.");
  }
  if (PortMods::NativeTextureCount() > 0) {
    ImGui::TextDisabled("Native textures: %zu, %zu in use", PortMods::NativeTextureCount(), PortMods::NativeTexturesBound());
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Folder: %s", status.folder.c_str());
  ImGui::PopStyleColor();
#if !defined(__ANDROID__)
  DrawImporters();
#endif
}
} // namespace PortDebug
