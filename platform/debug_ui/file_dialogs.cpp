// F1 overlay: Memory card import and export (port_gci.h), picking the Remastered image and keys,
// and the file dialogs both answer through. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_gci.h"
#include "port_mods.h"
#include "port_remastered_import.h"
#include "port_dread_samus_mod.h"
#include "port_remastered_text.h"
#include "port_discord.h"
#include "port_savestate.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_misc.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#include <unistd.h>
#include <jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace PortDebug {

// Memory card transfer (port_gci.h). The work runs on the main thread; the
// file dialogs answer on another one, so their picks wait in sCardPicks.
namespace {
// kCardPick_Trilogy: the Trilogy save.bin to write the save into.
// kCardPick_Emulator: the Dolphin/PrimeHack user folder.
enum CardPick {
  kCardPick_Import,
  kCardPick_ExportFolder,
  kCardPick_ExportFile,
  kCardPick_Trilogy,
  kCardPick_Emulator
};
std::mutex sCardPickMutex;
std::vector<std::pair<CardPick, std::string>> sCardPicks;
std::atomic<bool> sCardDialogOpen{false};
std::string sCardStatus;
// Android has no folder dialog: each file gets its own save dialog, in turn.
struct CardExportFile {
  std::string name; // the suggested file name
  std::vector<uint8_t> bytes;
  std::string note; // what a conversion could not carry over
};
std::vector<CardExportFile> sCardExportQueue;
// The overlay's "Export as" choice: an index into kCardExportWords.
int sCardExportChoice = 0;
constexpr const char* kCardExportWords[] = {"", "usa", "pal", "trilogy"};
constexpr const char* kCardExportUsage = " (usa, pal or trilogy).";

// "" means the card's files as they are; Unknown for a word that is no format.
// "trilogy" is the Wii Trilogy's save.bin (WiiSave).
PortGci::ExportFormat CardExportFormat(const std::string& word) {
  if (word.empty()) {
    return PortGci::NativeExport();
  }
  if (word == "usa") {
    return PortGci::ExportFormat::UsaGci;
  }
  if (word == "pal") {
    return PortGci::ExportFormat::PalGci;
  }
  if (word == "trilogy") {
    return PortGci::ExportFormat::WiiSave;
  }
  return PortGci::ExportFormat::Unknown;
}

// The last path segment, for messages; content:// URIs keep theirs escaped.
std::string CardDisplayName(const std::string& path) {
  std::string name = path.substr(path.find_last_of("/\\") + 1);
  const size_t escaped = name.rfind("%2F");
  return escaped == std::string::npos ? name : name.substr(escaped + 3);
}

std::string CardImportTarget(std::filesystem::path& folder) {
  if (sStateManager != nullptr) {
    return "Return to the title screen to import: saving the game in progress would "
           "overwrite the imported save.";
  }
  folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder, so there is nowhere to import to.";
  }
  return {};
}

std::string CardExportSource(std::filesystem::path& folder) {
  folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder.";
  }
  if (PortGci::GameFiles(folder).empty()) {
    return "There are no saves on the memory card yet.";
  }
  return {};
}

std::string FinishCardImport(const PortGci::Report& report) {
  if (report.copied > 0) {
    // The save screen remounts the card and reads it again once it is idle.
    PortGci::MarkCardChanged();
  }
  return report.Summary("Imported");
}
} // namespace

std::string CardList() {
  const std::filesystem::path folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder.";
  }
  std::string text = "card: " + PortGci::PathString(folder);
  for (const std::filesystem::path& file : PortGci::GameFiles(folder)) {
    std::error_code ec;
    text += "\n  " + PortGci::PathString(file.filename()) + " (" +
            std::to_string(std::filesystem::file_size(file, ec)) + " bytes)";
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.gciFolder.empty()) {
    text += "\ndolphin folder: " + PortGci::PathString(dolphin.gciFolder);
  }
  if (!dolphin.rawImage.empty()) {
    text += "\ndolphin raw: " + PortGci::PathString(dolphin.rawImage);
  }
  return text;
}

std::string CardImport(const std::string& path) {
  std::filesystem::path folder;
  const std::string refusal = CardImportTarget(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(PortGci::PathFromString(path), ec)) {
    return FinishCardImport(PortGci::ImportFolder(PortGci::PathFromString(path), folder));
  }
  // SDL reads content:// URIs from the Android picker as well as paths.
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (data == nullptr) {
    return "Could not read " + CardDisplayName(path) + ": " + SDL_GetError();
  }
  const uint8_t* bytes = static_cast< const uint8_t* >(data);
  const std::vector<uint8_t> contents(bytes, bytes + size);
  SDL_free(data);
  return FinishCardImport(
      PortGci::ImportBytes(contents, CardDisplayName(path), folder, folder.parent_path()));
}

std::string CardExport(const std::string& dest, const std::string& format) {
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::ExportFormat to = CardExportFormat(format);
  if (to == PortGci::ExportFormat::Unknown) {
    return "Unknown export format " + format + kCardExportUsage;
  }
  const std::filesystem::path target = PortGci::PathFromString(dest);
  if (to == PortGci::ExportFormat::WiiSave) {
    return PortGci::ExportTrilogy(folder, target).Summary("Exported");
  }
  if (target.extension() == ".raw") {
    return PortGci::ExportRaw(folder, target, to).Summary("Exported");
  }
  return PortGci::ExportFolder(folder, target, to).Summary("Exported");
}

std::string CardImportDolphin() {
  std::filesystem::path folder;
  const std::string refusal = CardImportTarget(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  std::error_code same;
  if (!dolphin.gciFolder.empty() && std::filesystem::equivalent(dolphin.gciFolder, folder, same)) {
    return "The port already uses the emulator's card (sharing is on); nothing to import.";
  }
  if (!dolphin.Found()) {
    return std::string("No Dolphin or PrimeHack memory card found (GC/") + PortGci::CardRegion() +
           "/Card A or GC/MemoryCardA." + PortGci::CardRegion() + ".raw in the emulator's user folder).";
  }
  // Dolphin uses one or the other, per its settings: try the one written last.
  std::error_code ec;
  std::filesystem::file_time_type folderTime = std::filesystem::file_time_type::min();
  for (const std::filesystem::path& file : PortGci::ImportableFiles(dolphin.gciFolder)) {
    folderTime = std::max(folderTime, std::filesystem::last_write_time(file, ec));
  }
  const auto rawTime = dolphin.rawImage.empty() ? std::filesystem::file_time_type::min()
                                                : std::filesystem::last_write_time(dolphin.rawImage, ec);
  std::vector<std::filesystem::path> sources;
  if (!dolphin.gciFolder.empty() && folderTime != std::filesystem::file_time_type::min()) {
    sources.push_back(dolphin.gciFolder);
  }
  if (!dolphin.rawImage.empty()) {
    sources.insert(rawTime > folderTime ? sources.begin() : sources.end(), dolphin.rawImage);
  }
  for (const std::filesystem::path& source : sources) {
    const PortGci::Report report = source == dolphin.gciFolder
                                       ? PortGci::ImportFolder(source, folder)
                                       : PortGci::ImportFile(source, folder, folder.parent_path());
    if (report.copied > 0 || !report.errors.empty()) {
      return "From " + PortGci::PathString(source) + ": " + FinishCardImport(report);
    }
  }
  return "The emulator's memory card holds no Metroid Prime saves.";
}

std::string CardExportDolphin(const std::string& format) {
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::ExportFormat to = CardExportFormat(format);
  if (to == PortGci::ExportFormat::Unknown) {
    return "Unknown export format " + format + kCardExportUsage;
  }
  if (to == PortGci::ExportFormat::WiiSave) {
    const std::filesystem::path bin = PortGci::FindDolphinTrilogy();
    if (bin.empty()) {
      return "No Trilogy save in Dolphin or PrimeHack (Wii/title/00010000/52334d45 or 52334d50/data/save.bin in "
             "its user folder); start Metroid Prime Trilogy there once to create it.";
    }
    return "To " + PortGci::PathString(bin) + ": " + PortGci::ExportTrilogy(folder, bin).Summary("Exported");
  }
  const char* region = PortGci::CardRegionOf(to);
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard(region);
  if (!dolphin.Found()) {
    return std::string("No Dolphin or PrimeHack memory card found (GC/") + region + "/Card A or GC/MemoryCardA." +
           region + ".raw in the emulator's user folder); start a GameCube game of that region there once to "
           "create it.";
  }
  std::string text;
  std::error_code same;
  if (!dolphin.gciFolder.empty() && std::filesystem::equivalent(dolphin.gciFolder, folder, same)) {
    return "The port already uses the emulator's card (sharing is on); nothing to export.";
  }
  if (!dolphin.gciFolder.empty()) {
    text = "To " + PortGci::PathString(dolphin.gciFolder) + ": " +
           PortGci::ExportFolder(folder, dolphin.gciFolder, to).Summary("Exported");
  }
  if (!dolphin.rawImage.empty()) {
    text += std::string(text.empty() ? "" : "\n") + "To " +
            PortGci::PathString(dolphin.rawImage) + ": " +
            PortGci::ExportRaw(folder, dolphin.rawImage, to).Summary("Exported");
  }
  return text;
}

namespace {
#if defined(__ANDROID__)
void SetNextAndroidSaveDialogFilename(const char* filename) {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return;
  }
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (activity == nullptr) {
    return;
  }

  jclass cls = env->GetObjectClass(activity);
  jmethodID method = nullptr;
  jstring name = nullptr;
  if (cls != nullptr && !env->ExceptionCheck()) {
    method = env->GetStaticMethodID(cls, "setNextSaveDialogSuggestedFilename", "(Ljava/lang/String;)V");
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    method = nullptr;
  }
  if (method != nullptr) {
    name = env->NewStringUTF(filename != nullptr ? filename : "");
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      name = nullptr;
    }
    if (name != nullptr) {
      env->CallStaticVoidMethod(cls, method, name);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
    }
  }
  if (name != nullptr) {
    env->DeleteLocalRef(name);
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
}
#endif

void OpenCardDialog(CardPick pick) {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void* userdata, const char* const* files, int) {
    std::lock_guard lock(sCardPickMutex);
    const CardPick kind = static_cast< CardPick >(reinterpret_cast< intptr_t >(userdata));
    // An empty path means cancelled (or a failed dialog, with the error set).
    sCardPicks.emplace_back(kind, files != nullptr && files[0] != nullptr ? files[0] : "");
    if (files == nullptr) {
      sCardPicks.back().second = std::string("\x01") + SDL_GetError();
    }
    sCardDialogOpen = false;
  };
  void* userdata = reinterpret_cast< void* >(static_cast< intptr_t >(pick));
  sCardDialogOpen = true;
  switch (pick) {
  case kCardPick_Import: {
#if defined(__ANDROID__)
    // Android turns filters into MIME types, and .gci has none.
    SDL_ShowOpenFileDialog(done, userdata, window, nullptr, 0, nullptr, false);
#else
    static const SDL_DialogFileFilter filters[] = {
        {"Saves (.gci, card images, Wii data.bin/save.bin)", "gci;raw;mcp;sav;bin"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(done, userdata, window, filters, 2, nullptr, false);
#endif
    break;
  }
  case kCardPick_ExportFolder:
  case kCardPick_Emulator:
    SDL_ShowOpenFolderDialog(done, userdata, window, nullptr, false);
    break;
  case kCardPick_Trilogy: {
#if defined(__ANDROID__)
    SDL_ShowOpenFileDialog(done, userdata, window, nullptr, 0, nullptr, false);
#else
    static const SDL_DialogFileFilter filters[] = {
        {"Trilogy save (save.bin)", "bin"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(done, userdata, window, filters, 2, nullptr, false);
#endif
    break;
  }
  case kCardPick_ExportFile: {
    static std::string location;
    location = sCardExportQueue.empty()
                   ? std::string()
                   : sCardExportQueue.front().name;
#if defined(__ANDROID__)
    // SDL's Android save picker ignores its initial-location argument. Hand the
    // current queue member's basename to its Java dialog instead.
    SetNextAndroidSaveDialogFilename(location.c_str());
#endif
    SDL_ShowSaveFileDialog(done, userdata, window, nullptr, 0,
                           location.empty() ? nullptr : location.c_str());
    break;
  }
  }
}

#if defined(__ANDROID__)
// Fills sCardExportQueue with the card's files, read as they are or converted
// to `format` (one file then). Returns an error, or "" with the queue filled.
std::string CardQueueExport(const std::filesystem::path& folder, const std::string& format) {
  sCardExportQueue.clear();
  const PortGci::ExportFormat to = CardExportFormat(format);
  if (to == PortGci::ExportFormat::Unknown || to == PortGci::ExportFormat::WiiSave) {
    return "Unknown export format " + format + " (usa or pal).";
  }
  if (to == PortGci::NativeExport()) {
    for (const std::filesystem::path& file : PortGci::GameFiles(folder)) {
      std::ifstream in(file, std::ios::binary);
      std::vector<uint8_t> bytes = port::ReadAll(in);
      if (!in && !in.eof()) {
        sCardExportQueue.clear();
        return "Could not read " + PortGci::PathString(file.filename()) + ".";
      }
      // A Trilogy disc's files go out as the GameCube release's.
      PortGci::Header header;
      std::string error;
      if (!PortGci::ParseHeader(bytes.data(), bytes.size(), header, error)) {
        sCardExportQueue.clear();
        return "Could not read " + PortGci::PathString(file.filename()) + ": " + error;
      }
      std::memcpy(bytes.data(), PortGci::GameCodeOf(to), 4);
      header.game = PortGci::GameCodeOf(to);
      sCardExportQueue.push_back({PortGci::DiskName(header), std::move(bytes), {}});
    }
    return {};
  }
  CardExportFile entry;
  std::string error;
  PortGci::Header header;
  if (!PortGci::ExportBytes(folder, to, entry.bytes, entry.note, error) ||
      !PortGci::ParseHeader(entry.bytes.data(), entry.bytes.size(), header, error)) {
    return "Could not convert the save: " + error;
  }
  entry.name = PortGci::DiskName(header);
  sCardExportQueue.push_back(std::move(entry));
  return {};
}

// Queues the Trilogy save.bin at `basePath` (a content:// URI from the picker)
// with the card's newest save written into it.
std::string CardQueueTrilogy(const std::string& basePath) {
  sCardExportQueue.clear();
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  size_t size = 0;
  void* data = SDL_LoadFile(basePath.c_str(), &size);
  if (data == nullptr) {
    return "Could not read " + CardDisplayName(basePath) + ": " + SDL_GetError();
  }
  const uint8_t* bytes = static_cast< const uint8_t* >(data);
  const std::vector<uint8_t> base(bytes, bytes + size);
  SDL_free(data);
  CardExportFile entry;
  std::string error;
  if (!PortGci::ExportBytes(folder, PortGci::ExportFormat::WiiSave, entry.bytes, entry.note, error, base)) {
    return "Could not convert the save: " + error;
  }
  entry.name = "save.bin";
  sCardExportQueue.push_back(std::move(entry));
  return {};
}
#endif

std::string CardSaveTo(const CardExportFile& source, const std::string& target) {
  SDL_IOStream* out = SDL_IOFromFile(target.c_str(), "wb");
  bool ok = out != nullptr && SDL_WriteIO(out, source.bytes.data(), source.bytes.size()) == source.bytes.size();
  if (out != nullptr) {
    ok = SDL_CloseIO(out) && ok;
  }
  return ok ? "Saved " + source.name + " as " + CardDisplayName(target) +
                  (source.note.empty() ? "" : ", " + source.note) + "."
            : "Could not save " + source.name + ": " + SDL_GetError();
}
} // namespace

// Runs every frame, overlay open or not, so a dialog's answer is acted on.
void ProcessCardPicks() {
  std::vector<std::pair<CardPick, std::string>> picks;
  {
    std::lock_guard lock(sCardPickMutex);
    picks.swap(sCardPicks);
  }
  for (const auto& [kind, path] : picks) {
    if (path.empty() || path[0] == '\x01') {
      if (path.size() > 1) {
        sCardStatus = "The file dialog failed: " + path.substr(1);
      }
      sCardExportQueue.clear();
      continue;
    }
    switch (kind) {
    case kCardPick_Import:
      sCardStatus = CardImport(path);
      break;
    case kCardPick_Emulator:
      PortGci::SetEmulatorFolder(path);
      sCardStatus = "Emulator folder: " + PortGci::EmulatorFolder();
      MarkDirty();
      SaveSettings();
      break;
    case kCardPick_ExportFolder:
      sCardStatus = CardExport(path, kCardExportWords[sCardExportChoice]);
      break;
    case kCardPick_Trilogy:
#if defined(__ANDROID__)
      sCardStatus = CardQueueTrilogy(path);
      if (!sCardExportQueue.empty()) {
        OpenCardDialog(kCardPick_ExportFile);
      }
#else
      sCardStatus = CardExport(path, "trilogy");
#endif
      break;
    case kCardPick_ExportFile:
      if (!sCardExportQueue.empty()) {
        sCardStatus = CardSaveTo(sCardExportQueue.front(), path);
        sCardExportQueue.erase(sCardExportQueue.begin());
        if (!sCardExportQueue.empty()) {
          OpenCardDialog(kCardPick_ExportFile);
        }
      }
      break;
    }
  }
}

namespace {
#if !defined(__ANDROID__)
// Emulator folder and card sharing (port_gci.h SharedCardBase).
void DrawEmulatorCard() {
  const bool inGame = sStateManager != nullptr;
  const std::string chosen = PortGci::EmulatorFolder();
  std::string found = "none found";
  if (chosen.empty()) {
    const PortGci::DolphinCard card = PortGci::FindDolphinCard();
    if (card.Found()) {
      // <root>/GC/<region>/Card A or <root>/GC/MemoryCardA.<region>.raw.
      found = PortGci::PathString(card.gciFolder.empty()
                                      ? card.rawImage.parent_path().parent_path()
                                      : card.gciFolder.parent_path().parent_path().parent_path());
    }
  }
  ImGui::TextUnformatted("Emulator folder");
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::TextWrapped(": %s", chosen.empty() ? ("Auto (" + found + ")").c_str() : chosen.c_str());
  ImGui::BeginDisabled(sCardDialogOpen);
  if (ImGui::Button("Choose...")) {
    OpenCardDialog(kCardPick_Emulator);
  }
  ImGui::EndDisabled();
  ItemHelp("The Dolphin or PrimeHack user folder (the one containing GC/). Only needed for "
           "portable installs.");
  ImGui::SameLine();
  ImGui::BeginDisabled(chosen.empty());
  if (ImGui::Button("Auto")) {
    PortGci::SetEmulatorFolder("");
    MarkDirty();
    SaveSettings();
  }
  ImGui::EndDisabled();
  // Issue #115: with Dolphin and PrimeHack both installed, Auto takes Dolphin,
  // and the folder dialog may hide PrimeHack's ~/.var folder.
  const std::vector<std::filesystem::path> detected = PortGci::DetectedEmulatorFolders();
  if (!detected.empty()) {
    auto label = [](const std::filesystem::path& root) {
      const std::string path = PortGci::PathString(root);
      return (path.find("primehack") != std::string::npos ? "PrimeHack: " : "Dolphin: ") + path;
    };
    const std::string preview = chosen.empty() ? "Auto" : chosen;
    if (ImGui::BeginCombo("Installed emulators", preview.c_str())) {
      for (const std::filesystem::path& root : detected) {
        const std::string path = PortGci::PathString(root);
        if (ImGui::Selectable(label(root).c_str(), path == chosen)) {
          PortGci::SetEmulatorFolder(path);
          MarkDirty();
          SaveSettings();
        }
      }
      ImGui::EndCombo();
    }
    ItemHelp("Installs found on this computer. Pick one instead of the automatic choice.");
  }
  bool share = PortGci::ShareEmulatorCard();
  ImGui::BeginDisabled(inGame);
  if (ImGui::Checkbox("Share memory card with emulator", &share)) {
    PortGci::SetShareEmulatorCard(share);
    MarkDirty();
    SaveSettings();
  }
  ImGui::EndDisabled();
  ItemHelp("The port and the emulator share the same saves: never run both at once. GameCube "
           "Metroid Prime only; Randomizer and Archipelago keep their own cards. Takes effect at "
           "the title screen.");
  if (!share) {
    return;
  }
  std::string why;
  const std::filesystem::path base = PortGci::SharedCardBase(&why);
  if (base.empty()) {
    ImGui::TextColored(ThemeWarnColor(), "%s", why.c_str());
    return;
  }
  ImGui::BeginDisabled(inGame || sCardDialogOpen);
  if (ImGui::Button("Copy port saves into shared card")) {
    const std::filesystem::path own =
        PortGci::PathFromString(PortPaths::CardFolder()) / PortGci::CardRegion() / "Card A";
    std::error_code same;
    if (std::filesystem::equivalent(own, base / PortGci::CardRegion() / "Card A", same)) {
      sCardStatus = "The port's own card is the shared one.";
    } else {
      const PortGci::Report report = PortGci::ExportFolder(own, base / PortGci::CardRegion() / "Card A");
      if (report.copied > 0) {
        PortGci::MarkCardChanged();
      }
      sCardStatus = report.Summary("Copied");
    }
  }
  ImGui::EndDisabled();
  ItemHelp("Copies the port's own card saves into the shared card; the shared card's own saves "
           "move to _replaced first.");
}
#endif
} // namespace

void DrawMemoryCard() {
  ImGui::SeparatorText("Memory card");
#if !defined(__ANDROID__)
  DrawEmulatorCard();
#endif
  const std::filesystem::path folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    ImGui::TextDisabled("The card is not a GCI folder; nothing to import to or export.");
    return;
  }
  const size_t saves = PortGci::GameFiles(folder).size();
  ImGui::TextWrapped("Card: %s (%zu save file%s)", PortGci::PathString(folder).c_str(), saves,
                     saves == 1 ? "" : "s");
  const bool inGame = sStateManager != nullptr;
  const bool busy = sCardDialogOpen;
  ImGui::BeginDisabled(inGame || busy);
  if (ImGui::Button("Import file...")) {
    OpenCardDialog(kCardPick_Import);
  }
  ItemHelp(
      "Dolphin .gci saves, a card image (.raw) or a Wii Trilogy data.bin/save.bin, converted from "
      "the other release if needed. Replaces the card's saves; the old ones move to _replaced.");
#if !defined(__ANDROID__)
  ImGui::SameLine();
  if (ImGui::Button("Import from Dolphin/PrimeHack")) {
    sCardStatus = CardImportDolphin();
  }
  ItemHelp((std::string("The emulator's card is looked for in its user folder (GC/") + PortGci::CardRegion() +
            "/Card A, GC/MemoryCardA." + PortGci::CardRegion() + ".raw).")
               .c_str());
#endif
  ImGui::EndDisabled();
  ImGui::BeginDisabled(busy || saves == 0);
  const std::string asIs = std::string("As on the card (") +
                           PortSaveConvert::FormatName(PortGci::NativeExport()) + ")";
  const char* choices[] = {asIs.c_str(), "USA GameCube (GM8E)", "PAL GameCube (GM8P)",
                           "Wii Trilogy (save.bin)"};
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  ImGui::Combo("Export as", &sCardExportChoice, choices, IM_ARRAYSIZE(choices));
  ItemHelp("The release to export for; saves of another release are converted. For the Trilogy, "
           "start a Metroid Prime game in an existing save.bin and save once first: that game is "
           "replaced. A Wii's signed data.bin can't be made here.");
  const char* format = kCardExportWords[sCardExportChoice];
  const bool trilogy = CardExportFormat(format) == PortGci::ExportFormat::WiiSave;
#if defined(__ANDROID__)
  if (trilogy) {
    if (ImGui::Button("Export into save.bin...")) {
      OpenCardDialog(kCardPick_Trilogy);
    }
    ItemHelp("Pick the Trilogy save.bin to start from, then where to save the new one.");
  } else {
    if (ImGui::Button("Export...")) {
      sCardStatus = CardQueueExport(folder, format);
      if (!sCardExportQueue.empty()) {
        OpenCardDialog(kCardPick_ExportFile);
      }
    }
    ItemHelp((std::string("Saves each file in turn; keep Dolphin's names (01-") +
              PortGci::GameCodeOf(CardExportFormat(format)) + "-MetroidPrime A.gci) for its GCI folder.")
                 .c_str());
  }
#else
  if (ImGui::Button(trilogy ? "Export into save.bin..." : "Export to folder...")) {
    OpenCardDialog(trilogy ? kCardPick_Trilogy : kCardPick_ExportFolder);
  }
  ImGui::SameLine();
  if (ImGui::Button("Export to Dolphin/PrimeHack")) {
    sCardStatus = CardExportDolphin(format);
  }
  ItemHelp(trilogy ? "Close the emulator first. Goes to its Trilogy save (Wii/title/00010000/52334d45/data/"
                     "save.bin); the old one is kept as save.bin.bak."
                   : (std::string("Close the emulator first. Goes to its ") +
                      PortGci::CardRegionOf(CardExportFormat(format)) +
                      " card; a raw card is backed up to .raw.bak before it is written.")
                         .c_str());
#endif
  ImGui::EndDisabled();
#if !defined(__ANDROID__)
  ImGui::SameLine();
  if (ImGui::Button("Open card folder")) {
    // "Card A" has a space, which a URL can't carry as is.
    const std::string path = PortGci::PathString(folder);
    std::string url = path.front() == '/' ? "file://" : "file:///"; // C:\ on Windows
    for (const char c : path) {
      url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
    }
    SDL_OpenURL(url.c_str());
  }
#endif
  if (inGame) {
    ImGui::TextColored(ThemeWarnColor(), "Return to the title screen to import.");
  }
  if (!sCardStatus.empty()) {
    ImGui::TextWrapped("%s", sCardStatus.c_str());
  }
}

// The Remastered import (port_remastered_import.h): the user's own image and
// key file, converted here into the remastered-models mod.
std::mutex sRemasteredPickMutex;
std::vector<std::pair<int, std::string>> sRemasteredPicks;
// Which 2 of the same picker: a PAL image for its languages (DrawLanguageSection).
std::string sPalLanguagesPick;
std::string sFlaahgraPick;
std::string sDreadPick;

#if defined(__ANDROID__)
// Not SDL_ShowOpenFileDialog: Android often kills the game behind the picker
// for its memory (seen on a tablet: the pick came back to a new process), and
// SDL's callback dies with it. MetroidPrimeActivity.pickRemasteredFile writes
// the picked address to this file, which the panel reads, in this process or
// the next one.
std::string RemasteredPickFile(int which) {
  const char* root = SDL_GetAndroidInternalStoragePath();
  return std::string(root != nullptr ? root : ".") + "/remastered_pick_" + std::to_string(which) + ".txt";
}

void TakeRemasteredPickFiles() {
  for (int which = 0; which < 2; ++which) {
    std::ifstream in(RemasteredPickFile(which));
    std::string uri;
    if (std::getline(in, uri) && !uri.empty()) {
      sRemasteredPicks.emplace_back(which, uri);
    }
  }
}

void OpenRemasteredDialog(int which) {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickRemasteredFile", "(I)V");
  if (method != nullptr) {
    env->CallVoidMethod(activity, method, jint(which));
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#else
void OpenRemasteredDialog(int which) {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void* userdata, const char* const* files, int) {
    if (files != nullptr && files[0] != nullptr) {
      std::lock_guard lock(sRemasteredPickMutex);
      const int which = int(reinterpret_cast< intptr_t >(userdata));
      if (which == kPalLanguagesPick) {
        sPalLanguagesPick = files[0];
      } else if (which == kFlaahgraPick) {
        sFlaahgraPick = files[0];
      } else if (which == kDreadPick) {
        sDreadPick = files[0];
      } else {
        sRemasteredPicks.emplace_back(which, files[0]);
      }
    }
  };
  static const SDL_DialogFileFilter imageFilters[] = {{"Switch images (.nsp, .xci)", "nsp;xci"}, {"All files", "*"}};
  static const SDL_DialogFileFilter keyFilters[] = {{"Key files (.keys)", "keys"}, {"All files", "*"}};
  static const SDL_DialogFileFilter discFilters[] = {
      {"GameCube images (.iso, .gcm, .rvz, .wia, .gcz, .ciso, .wbfs, .tgc)", "iso;gcm;rvz;wia;gcz;ciso;wbfs;tgc"},
      {"All files", "*"}};
  SDL_ShowOpenFileDialog(done, reinterpret_cast< void* >(static_cast< intptr_t >(which)), window,
                         which == 0 || which == kDreadPick ? imageFilters : which == 1 ? keyFilters : discFilters, 2, nullptr, false);
}
#endif

#if defined(__ANDROID__)
// Android's picker gives a content:// address, which only the system can open.
// The image is several GB, so it is not copied as the disc is: the file is
// opened here and the import reads it through the descriptor ("fd:<n>", see
// SourceFile). One descriptor per field, closed when another file is picked.
std::string OpenRemasteredPick(int which, const std::string& uri) {
  static int sHeld[kDreadKeysSlot + 1] = {-1, -1, -1, -1, -1, -1};
  if (which < 0 || which > kDreadKeysSlot) {
    return {};
  }
  if (sHeld[which] >= 0) {
    close(sHeld[which]);
    sHeld[which] = -1;
  }
  SDL_IOStream* io = SDL_IOFromFile(uri.c_str(), "rb");
  if (io == nullptr) {
    PortLog::Write("metroid_prime_port: could not open the picked file: %s: %s\n", uri.c_str(), SDL_GetError());
    return {};
  }
  const int fd = int(SDL_GetNumberProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1));
  sHeld[which] = fd >= 0 ? dup(fd) : -1;
  SDL_CloseIO(io);
  return sHeld[which] >= 0 ? "fd:" + std::to_string(sHeld[which]) : std::string();
}

// "content://.../document/primary%3ADownload%2Fgame.nsp" -> "game.nsp".
std::string RemasteredPickName(const std::string& uri) {
  std::string text;
  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast< unsigned char >(uri[i + 1])) &&
        std::isxdigit(static_cast< unsigned char >(uri[i + 2]))) {
      text += char(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      text += uri[i];
    }
  }
  const size_t cut = text.find_last_of("/:");
  return cut == std::string::npos ? text : text.substr(cut + 1);
}
#endif

// What the last "Reload mods" came to: it waits for a game to start when none
// is loaded, and is refused where the game can't be saved.
void DrawModReloadMessage() {
  const std::string message = PortSaveState::LastMessage();
  if (message.find("mods") != std::string::npos || message.find("Mods") != std::string::npos) {
    ImGui::TextWrapped("%s", message.c_str());
  }
}

// An import from the panel unloads the mods while it runs (on a tablet, the
// game holding a room-geometry mod plus the import ran out of memory) and
// loads them again when it ends, the new import in place of the old.
static bool sImportWatched = false;
bool sImportUnloadedMods = false;
static bool sImportInstallStarted = false;
// The mod the running import makes, switched on when it ends.
static std::string sImportModName = PortRemastered::kImportModName;
// A reload is refused where the game can't save (a cutscene, say), so these
// are asked again every two seconds until one runs.
static int sReloadAwaited = -1;
static double sReloadRetry = 0.0;

static void ReloadUntilDone() {
  sReloadAwaited = PortSaveState::ModReloads();
  sReloadRetry = ImGui::GetTime() + 2.0;
  PortSaveState::RequestModReload();
}

bool StartRemasteredImport(const std::string& image, const std::string& keys) {
  PortRemastered::SetImportParts(RemasteredImportParts());
#if defined(__ANDROID__)
  // Each worker holds a world's models while it converts them; a phone has
  // the memory for two of those, not for one per core.
  const bool started = PortRemastered::StartImport(image, keys, 2);
#else
  const bool started = PortRemastered::StartImport(image, keys);
#endif
  if (!started) {
    return false;
  }
  sImportModName = PortRemastered::kImportModName;
  sImportWatched = true;
  const PortMods::Status& status = PortMods::CurrentStatus();
  if (std::any_of(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& mod) { return mod.enabled; })) {
    PortMods::SetSuspended(true);
    ReloadUntilDone();
    sImportUnloadedMods = true;
  }
  return true;
}

bool StartDreadSamusImport(const std::string& image, const std::string& keys) {
  if (!PortRemastered::StartDreadSamusImport(image, keys)) {
    return false;
  }
  sImportModName = PortDreadSamus::kModName;
  sImportWatched = true;
  // Its old files are replaced at the end: nothing of the mods may be open meanwhile.
  const PortMods::Status& status = PortMods::CurrentStatus();
  if (std::any_of(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& mod) { return mod.enabled; })) {
    PortMods::SetSuspended(true);
    ReloadUntilDone();
    sImportUnloadedMods = true;
  }
  return true;
}

// Every frame, so the mods come back with the panel closed too.
void FinishRemasteredImport() {
  if (sReloadAwaited >= 0) {
    if (PortSaveState::ModReloads() != sReloadAwaited) {
      sReloadAwaited = -1;
    } else if (ImGui::GetTime() >= sReloadRetry) {
      sReloadRetry = ImGui::GetTime() + 2.0;
      PortSaveState::RequestModReload();
    }
  }
  if (!sImportWatched) {
    return;
  }
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  if (state.running) {
    return;
  }
  // The finished import is moved into place on the import's thread, not by the mods reload on this
  // one: in shared storage (what the mods folder used to be on Android) that move took minutes with
  // the game frozen, and a big folder keeps the game from waiting either way. Nothing of the
  // old mod may be open meanwhile, so the mods are unloaded first if they came back on.
  if (state.ok && !sImportInstallStarted) {
    const PortMods::Status& status = PortMods::CurrentStatus();
    if (!sImportUnloadedMods &&
        std::any_of(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& mod) { return mod.enabled; })) {
      PortMods::SetSuspended(true);
      ReloadUntilDone();
      sImportUnloadedMods = true;
    }
    if (sReloadAwaited >= 0) {
      return;
    }
    sImportInstallStarted = true;
    if (PortRemastered::StartInstall()) {
      return;
    }
  }
  sImportWatched = false;
  sImportInstallStarted = false;
  if (state.ok) {
    // A mod just imported is wanted, even if it was switched off to import.
    std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
    const auto kept = std::remove(disabled.begin(), disabled.end(), sImportModName);
    if (kept != disabled.end()) {
      disabled.erase(kept, disabled.end());
      SetModsDisabled(PortMods::JoinDisabled(disabled));
    }
  }
  if (state.ok || sImportUnloadedMods) {
    PortMods::SetSuspended(false);
    ReloadUntilDone();
  }
  sImportUnloadedMods = false;
}

} // namespace PortDebug
