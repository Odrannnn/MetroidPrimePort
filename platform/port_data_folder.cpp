#include "port_data_folder.h"
#include "port_strings.h"

#include "port_paths.h"
#include "port_remastered_storage.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(__ANDROID__)
#include "port_debug.h"
#include "port_log.h"

#include <imgui.h>
#include <jni.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_timer.h>

#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <thread>
#endif

namespace PortDataFolder {
namespace {

namespace fs = std::filesystem;
using PortPaths::detail::FromUtf8;
using PortPaths::detail::ToUtf8;

const char* const kTempSuffix = ".mpcopy";
const char* const kIncomplete = ".mpcopy_incomplete";

std::string Relative(const fs::path& path, const fs::path& base) {
  const std::u8string text = path.lexically_relative(base).generic_u8string();
  return std::string(text.begin(), text.end());
}

using port::EndsWith;

// Every file a move copies, relative to `root` with '/' separators.
// `failed`, when given, is set if the walk stopped on an error: the list is then
// incomplete, which a copy must not take for the whole folder.
std::vector< std::string > ListFiles(const std::string& root, bool* failed = nullptr) {
  std::vector< std::string > files;
  std::error_code ec;
  const fs::path base = FromUtf8(root);
  if (failed != nullptr) {
    *failed = false;
  }
  if (!fs::is_directory(base, ec)) {
    return files;
  }
  fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const std::string relative = Relative(it->path(), base);
    if (IsSkipped(relative)) {
      if (it->is_directory(ec)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (it->is_regular_file(ec)) {
      files.push_back(relative);
    }
  }
  if (ec && failed != nullptr) {
    *failed = true;
  }
  return files;
}

} // namespace

std::string SharedFolder() {
#if defined(__ANDROID__)
  // EXTERNAL_STORAGE is usually /sdcard, a link to the primary volume of the
  // current user; the resolved path is what a file manager shows.
  const char* env = std::getenv("EXTERNAL_STORAGE");
  std::string root = env != nullptr && env[0] == '/' ? env : "/storage/emulated/0";
  std::error_code ec;
  const fs::path resolved = fs::canonical(FromUtf8(root), ec);
  if (!ec) {
    root = ToUtf8(resolved);
  }
  return PortPaths::detail::WithSeparator(root) + "MetroidPrime/";
#else
  return {};
#endif
}

bool IsSkipped(const std::string& relative) {
  // The built-in textures and initial pipeline cache are re-copied from the APK
  // on every launch, and the shader caches always stay in private storage.
  static const char* const kTopLevel[] = {"textures", "initial_pipeline_cache.db", "data_folder.txt",
                                          "texture_dumps", kIncomplete};
  static const char* const kCaches[] = {"pipeline_cache.db", "dawn_cache.db"};
  const size_t slash = relative.find('/');
  const std::string first = relative.substr(0, slash);
  // The built-in Remastered import belongs to the app (port_remastered_storage.h): moving the data
  // folder neither carries it along nor counts it, also when the data folder is the app's own
  // storage, which is where the import sits.
  if (first == PortRemasteredStorage::kRootFolderName) {
    return true;
  }
  if (slash == std::string::npos) {
    for (const char* name : kTopLevel) {
      if (first == name) {
        return true;
      }
    }
    for (const char* name : kCaches) {
      if (first.rfind(name, 0) == 0) {
        return true;
      }
    }
  } else if (first == "textures" || first == "texture_dumps") {
    return true;
  }
  const std::string name = relative.substr(relative.rfind('/') + 1);
  return name == ".port_write_test" || EndsWith(name, ".partial") || EndsWith(name, ".part") ||
         EndsWith(name, kTempSuffix);
}

Totals Measure(const std::string& from) {
  Totals totals;
  const fs::path base = FromUtf8(from);
  for (const std::string& relative : ListFiles(from)) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(base / FromUtf8(relative), ec);
    if (!ec) {
      totals.bytes += size;
    }
    ++totals.files;
  }
  return totals;
}

bool HasData(const std::string& dir) {
  return !ListFiles(dir).empty();
}

bool IsComplete(const std::string& dir) {
  std::error_code ec;
  return !fs::exists(FromUtf8(dir) / kIncomplete, ec);
}

bool CopyTree(const std::string& from, const std::string& to, Progress& progress, std::string& error,
              bool changedOnly) {
  const fs::path source = FromUtf8(from);
  const fs::path target = FromUtf8(to);
  const fs::path incomplete = target / kIncomplete;
  {
    std::error_code ec;
    fs::create_directories(target, ec);
    std::ofstream flag(incomplete, std::ios::binary | std::ios::trunc);
    if (!flag) {
      error = "Could not write to " + to;
      return false;
    }
  }
  std::vector< char > buffer(1 << 20);
  bool listFailed = false;
  const std::vector< std::string > files = ListFiles(from, &listFailed);
  if (listFailed) {
    // The old data is left alone, and the incomplete flag stays in the target.
    error = "Could not read all of " + from + " (a folder could not be listed).";
    return false;
  }
  for (const std::string& relative : files) {
    const fs::path in = source / FromUtf8(relative);
    const fs::path out = target / FromUtf8(relative);
    fs::path temp = out;
    temp += kTempSuffix;
    std::error_code ec;
    if (changedOnly) {
      // The copy is written after the original, so a newer original changed since.
      std::error_code inEc;
      const uintmax_t inSize = fs::file_size(in, inEc);
      const uintmax_t outSize = fs::file_size(out, ec);
      const bool sameSize = !inEc && !ec && inSize == outSize;
      const auto inTime = fs::last_write_time(in, inEc);
      const auto outTime = fs::last_write_time(out, ec);
      if (sameSize && !inEc && !ec && outTime >= inTime) {
        progress.filesDone.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
    }
    fs::create_directories(out.parent_path(), ec);
    std::ifstream input(in, std::ios::binary);
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
      error = "Could not " + std::string(!input ? "read " : "write ") + ToUtf8(!input ? in : out);
      fs::remove(temp, ec);
      return false;
    }
    while (input) {
      if (progress.cancel.load(std::memory_order_relaxed)) {
        output.close();
        fs::remove(temp, ec);
        error = "Cancelled.";
        return false;
      }
      input.read(buffer.data(), static_cast< std::streamsize >(buffer.size()));
      const std::streamsize got = input.gcount();
      if (got > 0 && !output.write(buffer.data(), got)) {
        break;
      }
      progress.bytesDone.fetch_add(static_cast< uint64_t >(got), std::memory_order_relaxed);
    }
    output.close();
    if (input.bad() || !output) {
      fs::remove(temp, ec);
      error = "Writing " + ToUtf8(out) + " failed (is the storage full?)";
      return false;
    }
    fs::rename(temp, out, ec);
    if (ec) {
      fs::remove(temp, ec);
      error = "Could not replace " + ToUtf8(out);
      return false;
    }
    progress.filesDone.fetch_add(1, std::memory_order_relaxed);
  }
  std::error_code ec;
  fs::remove(incomplete, ec);
  if (ec) {
    error = "Could not finish the copy in " + to;
    return false;
  }
  return true;
}

void DeleteData(const std::string& dir, const std::function< bool(const std::string&) >& keep) {
  const fs::path base = FromUtf8(dir);
  std::error_code ec;
  for (const std::string& relative : ListFiles(dir)) {
    if (!keep || !keep(relative)) {
      fs::remove(base / FromUtf8(relative), ec);
    }
  }
  // Folders left empty by that, deepest first; a folder that still holds a
  // skipped file stays.
  std::vector< fs::path > dirs;
  fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const std::string relative = Relative(it->path(), base);
    if (IsSkipped(relative)) {
      if (it->is_directory(ec)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (it->is_directory(ec)) {
      dirs.push_back(it->path());
    }
  }
  for (auto dirIt = dirs.rbegin(); dirIt != dirs.rend(); ++dirIt) {
    if (fs::is_empty(*dirIt, ec)) {
      fs::remove(*dirIt, ec);
    }
  }
}

#if defined(__ANDROID__)
namespace {

// Calls a no-argument method of MetroidPrimeActivity; `boolResult` for "()Z".
bool CallActivity(const char* method, bool boolResult) {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool result = false;
  jclass cls = env->GetObjectClass(activity);
  if (jmethodID id = env->GetMethodID(cls, method, boolResult ? "()Z" : "()V"); id != nullptr) {
    if (boolResult) {
      result = env->CallBooleanMethod(activity, id) == JNI_TRUE;
    } else {
      env->CallVoidMethod(activity, id);
      result = true;
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    result = false;
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return result;
}

enum class EState { Idle, ConfirmOverwrite, Copying, Ready, Failed };

struct Move {
  std::mutex mutex;
  EState state = EState::Idle;
  std::string target;
  std::string message;
  bool copied = false;
  bool targetComplete = true;
  // Tells a finished copy thread whether its result is still the one shown.
  uint64_t generation = 0;
  // Set by the copy thread once it has measured the data.
  std::atomic< uint64_t > totalBytes{0};
  std::atomic< uint64_t > totalFiles{0};
  Progress progress;
};

Move& State() {
  static Move sMove;
  return sMove;
}

std::string Size(uint64_t bytes) {
  char text[32];
  if (bytes >= (1ull << 30)) {
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast< double >(bytes) / (1ull << 30));
  } else {
    std::snprintf(text, sizeof(text), "%.0f MB", static_cast< double >(bytes) / (1ull << 20));
  }
  return text;
}

void StartCopy(const std::string& target) {
  Move& move = State();
  const std::string from = PortPaths::UserFolder();
  uint64_t generation = 0;
  {
    std::lock_guard lock(move.mutex);
    move.target = target;
    move.state = EState::Copying;
    move.copied = true;
    move.message.clear();
    move.totalBytes = 0;
    move.totalFiles = 0;
    move.progress.bytesDone = 0;
    move.progress.filesDone = 0;
    move.progress.cancel = false;
    generation = ++move.generation;
  }
  // Measuring walks the whole folder (mods can be thousands of files), so it
  // runs here and not on the render thread.
  std::thread([from, target, generation] {
    Move& move = State();
    const Totals totals = Measure(from);
    move.totalBytes = totals.bytes;
    move.totalFiles = totals.files;
    std::error_code ec;
    fs::create_directories(FromUtf8(target), ec);
    const fs::space_info space = fs::space(FromUtf8(target), ec);
    std::string error;
    bool ok = false;
    if (!ec && space.available < totals.bytes + (64ull << 20)) {
      error = "Not enough free space: the data needs " + Size(totals.bytes) + ", " + Size(space.available) +
              " is free.";
    } else {
      PortLog::Write("data folder: copying %" PRIu64 " files (%" PRIu64 " bytes) from %s to %s\n", totals.files,
                     totals.bytes, from.c_str(), target.c_str());
      ok = CopyTree(from, target, move.progress, error);
      PortLog::Write("data folder: copy %s%s\n", ok ? "done" : "failed: ", ok ? "" : error.c_str());
    }
    std::lock_guard lock(move.mutex);
    if (move.generation != generation) {
      return;
    }
    move.state = ok ? EState::Ready : EState::Failed;
    move.message = ok ? std::string() : error;
  }).detach();
}

// Points the next launch at the target and restarts into it.
void Switch() {
  Move& move = State();
  std::string target;
  bool copied = false;
  {
    std::lock_guard lock(move.mutex);
    target = move.target;
    copied = move.copied;
  }
  // A second pass for what changed since the copy: the settings, a save made
  // while it ran or after "Not now".
  PortDebug::SaveSettingsNow();
  if (!copied && !IsComplete(target)) {
    std::lock_guard lock(move.mutex);
    move.state = EState::Failed;
    move.message = target + " holds a copy that did not finish; copy the data again.";
    return;
  }
  if (copied) {
    Progress sync;
    std::string error;
    if (!CopyTree(PortPaths::UserFolder(), target, sync, error, true)) {
      std::lock_guard lock(move.mutex);
      move.state = EState::Failed;
      move.message = error;
      return;
    }
  }
  std::error_code ec;
  const std::string marker = PortPaths::detail::MarkerPath();
  bool written = false;
  if (target == PortPaths::detail::PrivateFolder()) {
    fs::remove(FromUtf8(marker), ec);
    written = !fs::exists(FromUtf8(marker), ec);
  } else {
    const std::string temp = marker + ".new";
    std::ofstream file(FromUtf8(temp), std::ios::binary | std::ios::trunc);
    file << target << '\n';
    file.close();
    written = !file.fail();
    if (written) {
      fs::rename(FromUtf8(temp), FromUtf8(marker), ec);
      written = !ec;
    }
  }
  std::lock_guard lock(move.mutex);
  if (!written) {
    move.state = EState::Failed;
    move.message = "Could not record the new folder in " + marker;
    return;
  }
  PortLog::Write("data folder: next launch uses %s\n", target.c_str());
  if (!CallActivity("restartApp", false)) {
    move.message = "Close the game and open it again to finish the move.";
  }
}

// Asked for once a second at most while the panel is open: the grant happens
// in the system settings, and coming back from there is not reported.
bool StorageAccess() {
  static uint64_t sLastCheck = 0;
  static bool sAccess = false;
  const uint64_t now = SDL_GetTicks();
  if (sLastCheck == 0 || now - sLastCheck > 1000) {
    sLastCheck = now;
    sAccess = CallActivity("hasStorageAccess", true);
  }
  return sAccess;
}

void RequestMove(const std::string& target, bool needsAccess) {
  Move& move = State();
  if (needsAccess && !StorageAccess()) {
    CallActivity("requestStorageAccess", false);
    std::lock_guard lock(move.mutex);
    move.state = EState::Idle;
    move.message = "Allow \"All files access\" for Metroid Prime, then come back and press the button again.";
    return;
  }
  if (HasData(target)) {
    const bool complete = IsComplete(target);
    std::lock_guard lock(move.mutex);
    move.target = target;
    move.targetComplete = complete;
    move.message.clear();
    move.state = EState::ConfirmOverwrite;
    return;
  }
  StartCopy(target);
}

} // namespace

void DrawPanel() {
  ImGui::SeparatorText("Data folder");
  Move& move = State();
  const std::string& current = PortPaths::UserFolder();
  const std::string priv = PortPaths::detail::PrivateFolder();
  const std::string shared = SharedFolder();
  const bool inPrivate = current == priv;
  ImGui::TextWrapped("Settings, memory card, mods, save states and the texture pack are in %s%s.",
                     current.c_str(), inPrivate ? " (app storage, hidden from file managers)" : "");
  if (const std::string lost = PortPaths::UnavailableFolder(); !lost.empty()) {
    ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%s could not be opened, so app storage is used this time.",
                       lost.c_str());
    ImGui::TextWrapped("Allow \"All files access\" for Metroid Prime and restart, or stay in app storage.");
    if (ImGui::Button("Allow access")) {
      CallActivity("requestStorageAccess", false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stay in app storage")) {
      std::error_code ec;
      fs::remove(FromUtf8(PortPaths::detail::MarkerPath()), ec);
    }
  }

  std::unique_lock lock(move.mutex);
  const EState state = move.state;
  const std::string target = move.target;
  const std::string message = move.message;
  lock.unlock();
  const std::string targetName = target == priv ? "app storage" : target;

  switch (state) {
  case EState::Idle:
  case EState::Failed:
    if (!message.empty()) {
      ImGui::TextWrapped("%s", message.c_str());
    }
    if (inPrivate) {
      const std::string label = "Move to " + shared;
      if (ImGui::Button(label.c_str())) {
        RequestMove(shared, true);
      }
      ImGui::TextWrapped(
          "Copies the data there and restarts. It stays there after an uninstall, and needs the "
          "\"All files access\" permission.");
    } else {
      if (ImGui::Button("Move back to app storage")) {
        RequestMove(priv, false);
      }
      // Measured once: walking a big mods folder every frame would stutter,
      // and nothing else writes to app storage while the data is elsewhere.
      static bool sConfirmDelete = false;
      static bool sOldMeasured = false;
      static uint64_t sOldBytes = 0;
      if (!sOldMeasured) {
        sOldMeasured = true;
        sOldBytes = Measure(priv).bytes;
      }
      if (sOldBytes > 0) {
        ImGui::TextWrapped("App storage still holds the copy from before the move (%s).", Size(sOldBytes).c_str());
        if (!sConfirmDelete) {
          if (ImGui::Button("Delete the old copy")) {
            sConfirmDelete = true;
          }
        } else {
          if (ImGui::Button("Delete it for good")) {
            // The disc copy is kept when it is the only one (a move made
            // before the disc was copied, or "Use the data already there"),
            // and the file the settings point at, whatever its name.
            const bool discElsewhere = fs::exists(FromUtf8(current + "disc.iso"));
            const char* configured = PortDebug::DiscPath();
            const fs::path privBase = FromUtf8(priv);
            DeleteData(priv, [discElsewhere, configured, privBase](const std::string& rel) {
              if (rel == "disc.iso" && !discElsewhere) {
                return true;
              }
              if (configured == nullptr) {
                return false;
              }
              std::error_code same;
              return fs::equivalent(privBase / FromUtf8(rel), FromUtf8(configured), same) && !same;
            });
            sOldMeasured = false;
            sConfirmDelete = false;
          }
          ImGui::SameLine();
          if (ImGui::Button("Keep it")) {
            sConfirmDelete = false;
          }
        }
      }
    }
    break;
  case EState::ConfirmOverwrite:
    if (move.targetComplete) {
      ImGui::TextWrapped("%s already holds game data (from an earlier install or move).", targetName.c_str());
      if (ImGui::Button("Use the data already there")) {
        lock.lock();
        move.copied = false;
        move.state = EState::Ready;
        lock.unlock();
      }
      ImGui::SameLine();
    } else {
      ImGui::TextWrapped("%s holds an earlier copy that did not finish.", targetName.c_str());
    }
    if (ImGui::Button("Copy this data over it")) {
      StartCopy(target);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      lock.lock();
      move.state = EState::Idle;
      lock.unlock();
    }
    break;
  case EState::Copying: {
    const uint64_t totalBytes = move.totalBytes.load();
    const uint64_t totalFiles = move.totalFiles.load();
    if (totalFiles == 0) {
      ImGui::TextWrapped("Counting files...");
    } else {
      const uint64_t done = move.progress.bytesDone.load(std::memory_order_relaxed);
      const float fraction = totalBytes > 0 ? static_cast< float >(done) / totalBytes : 0.f;
      const std::string overlay = Size(done) + " / " + Size(totalBytes);
      ImGui::TextWrapped("Copying to %s... (%" PRIu64 " of %" PRIu64 " files)", targetName.c_str(),
                         move.progress.filesDone.load(std::memory_order_relaxed), totalFiles);
      ImGui::ProgressBar(fraction, ImVec2(-1.f, 0.f), overlay.c_str());
    }
    if (ImGui::Button("Cancel##datafolder")) {
      move.progress.cancel = true;
    }
    break;
  }
  case EState::Ready:
    ImGui::TextWrapped("The data is in %s. The game restarts to switch to it; the original stays until deleted.",
                       targetName.c_str());
    if (!message.empty()) {
      ImGui::TextWrapped("%s", message.c_str());
    }
    if (ImGui::Button("Restart now")) {
      Switch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now")) {
      lock.lock();
      move.state = EState::Idle;
      lock.unlock();
    }
    break;
  }
}
#endif

} // namespace PortDataFolder
