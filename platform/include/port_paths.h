#pragma once

// Where the port keeps what it writes: settings, mods, save states, importers,
// the shader caches. One answer for every caller, ending in a separator.
//
// The folder is user/ beside the executable, so a copied build carries its data
// with it (an AppImage keeps it loose beside the .AppImage file instead), and
// files an older build left loose beside the executable are moved there
// (MigrateLooseData). In order:
//   1. MP_USER_PATH, when set.
//   2. Android: the folder in shared storage the player moved the data to
//      (F1 > System > Data folder, port_data_folder.h) when it can be written
//      to, else the app's private storage (the executable is inside the APK).
//   3. user/ in the executable's folder - for an AppImage, the folder the
//      .AppImage file is in, with no user/ - when the folder can be written to.
//      One exception: an install from before this, whose settings are still in
//      the per-user folder and which has none in user/, stays on the per-user
//      folder, so an update does not appear to lose the settings, mods and save
//      states. Moving that folder's contents into user/ switches it over.
//      Never inside a macOS app bundle.
//   4. The per-user folder (SDL_GetPrefPath), for a read-only install such as
//      a Flatpak or a system package.
//
// Header-only so the tests that build a single platform source need no more.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

namespace PortPaths {

namespace detail {

inline std::filesystem::path FromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

inline std::string WithSeparator(std::string dir) {
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
    dir += '/';
  }
  return dir;
}

inline std::string PrefFolder() {
  std::string dir;
  if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
    dir = pref;
    SDL_free(pref);
  }
  return dir;
}

inline bool Writable(const std::string& dir) {
  const std::filesystem::path probe = FromUtf8(dir) / ".port_write_test";
  {
    std::ofstream file(probe, std::ios::binary);
    if (!file) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::remove(probe, ec);
  return true;
}

#if defined(__ANDROID__)
// The app's private storage (getFilesDir). The built-in textures and the
// initial pipeline cache are always here, whatever folder the data is in.
inline std::string PrivateFolder() {
  return WithSeparator(PrefFolder());
}

// Names the folder the data was moved to (port_data_folder.h); absent while
// the data is in private storage.
inline std::string MarkerPath() {
  const std::string priv = PrivateFolder();
  return priv.empty() ? std::string() : priv + "data_folder.txt";
}

inline std::string ReadMarker() {
  const std::string marker = MarkerPath();
  if (marker.empty()) {
    return {};
  }
  std::ifstream file(FromUtf8(marker), std::ios::binary);
  std::string dir;
  std::getline(file, dir);
  while (!dir.empty() && (dir.back() == '\r' || dir.back() == '\n' || dir.back() == ' ')) {
    dir.pop_back();
  }
  // Only an absolute path: anything else would land in the working directory.
  return dir.empty() || dir[0] != '/' ? std::string() : WithSeparator(dir);
}
#endif

#if !defined(__ANDROID__)
// The per-user folder SDL_GetPrefPath would answer with, worked out by hand
// because asking SDL creates it, and a portable copy should leave no trace.
inline std::string LegacyFolder() {
#if defined(_WIN32)
  // SDL's environment is UTF-8 on Windows, unlike std::getenv.
  const char* root = SDL_getenv("APPDATA");
  return root != nullptr && root[0] != '\0' ? std::string(root) + "\\Metroid Prime\\" : std::string();
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  return home != nullptr && home[0] != '\0'
             ? std::string(home) + "/Library/Application Support/Metroid Prime/"
             : std::string();
#else
  if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] == '/') {
    return WithSeparator(data) + "Metroid Prime/";
  }
  const char* home = std::getenv("HOME");
  return home != nullptr && home[0] != '\0' ? std::string(home) + "/.local/share/Metroid Prime/"
                                            : std::string();
#endif
}

inline std::string ExecutableFolder() {
  // Inside an AppImage the executable sits in a read-only mount that is gone
  // when the game exits; the file the user copies around is $APPIMAGE.
  if (const char* image = std::getenv("APPIMAGE"); image != nullptr && image[0] != '\0') {
    const std::filesystem::path parent = FromUtf8(image).parent_path();
    if (!parent.empty()) {
      return WithSeparator(ToUtf8(parent));
    }
  }
  const char* base = SDL_GetBasePath();
  return base != nullptr ? WithSeparator(base) : std::string();
}
#endif

#if !defined(__ANDROID__)
inline bool InAppImage() {
  const char* image = std::getenv("APPIMAGE");
  return image != nullptr && image[0] != '\0';
}

// A macOS app bundle is signed as a whole: anything written inside it breaks
// the signature (Gatekeeper then calls the app damaged), and a quarantined
// download runs from a read-only translocated copy anyway.
inline bool InAppBundle(const std::string& exe) {
  return exe.find(".app/Contents/") != std::string::npos;
}

// The portable data folder: <executable folder>/user/. An AppImage keeps its
// data loose next to the .AppImage file, as it always has.
inline std::string PortableFolder() {
  const std::string exe = ExecutableFolder();
  if (exe.empty()) {
    return {};
  }
  return InAppImage() ? exe : exe + "user/";
}

inline std::vector<std::string>& MigrationLines() {
  static std::vector<std::string> sLines;
  return sLines;
}

// An older portable install keeps its data loose next to the executable. Moves
// each known item into user/ (a rename, so never a copy and never a deletion),
// once per process, leaving anything it does not know about alone. An item
// whose target already exists stays where it is.
inline void MigrateLooseData(const std::string& exe) {
  static bool sDone = false;
  if (sDone) {
    return;
  }
  sDone = true;
  if (exe.empty() || InAppImage()) {
    return;
  }
  namespace fs = std::filesystem;
  const fs::path from = FromUtf8(exe);
  const fs::path to = from / "user";
  std::error_code ec;

  // Files and folders moved one by one; "-wal", "-shm" and "-journal" siblings
  // of the caches travel with them as a group.
  static const char* const kItems[] = {
      "port_settings.ini", "metroid_prime_port.log", "metroid_prime_port.dmp", "imgui.ini",
      "imgui.log", "controller_ports.dat", "keyboard_bindings.dat", "randomizer_seed.json",
      "randomizer_locations.log", "randomizer_placements.log", "randomizer_checks.log",
      "archipelago.json", "archipelago_games", "mods", "savestates", "importers", "user_textures",
      "texture_dumps", "USA", "EUR", "JAP", "MemoryCardA.USA.raw", "MemoryCardB.USA.raw",
      "MemoryCardA.EUR.raw", "MemoryCardB.EUR.raw", "MemoryCardA.JAP.raw", "MemoryCardB.JAP.raw"};
  static const char* const kCaches[] = {"pipeline_cache.db", "dawn_cache.db"};
  static const char* const kSuffixes[] = {"", "-wal", "-shm", "-journal"};

  std::vector<std::string> names;
  for (const char* item : kItems) {
    names.emplace_back(item);
  }
  // Mapping files are named after the pad: <name>_<vid>_<pid>.controller.
  for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    const fs::path name = it->path().filename();
    if (name.extension() == ".controller" && it->is_regular_file(entryEc)) {
      names.push_back(ToUtf8(name));
    }
  }

  const auto move = [&](const std::string& name) {
    std::error_code e;
    const fs::path source = from / FromUtf8(name);
    const fs::path target = to / FromUtf8(name);
    if (!fs::exists(fs::symlink_status(source, e)) || fs::exists(fs::symlink_status(target, e))) {
      return;
    }
    fs::create_directories(to, e);
    e.clear();
    fs::rename(source, target, e);
    MigrationLines().push_back(e ? "could not move " + name + " into user/: " + e.message()
                                 : "moved " + name + " into user/");
  };
  for (const std::string& name : names) {
    move(name);
  }
  for (const char* cache : kCaches) {
    std::error_code e;
    if (fs::exists(fs::symlink_status(to / cache, e))) {
      continue;
    }
    for (const char* suffix : kSuffixes) {
      move(std::string(cache) + suffix);
    }
  }
}
#endif

inline std::string Resolve() {
  if (const char* env = std::getenv("MP_USER_PATH"); env != nullptr && env[0] != '\0') {
    return WithSeparator(env);
  }
#if defined(__ANDROID__)
  // A folder in shared storage the player moved the data to, as long as it
  // can still be written: without the storage permission (revoked, or a
  // reinstall) the private folder is used, and UnavailableFolder says so.
  if (const std::string chosen = ReadMarker(); !chosen.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(FromUtf8(chosen), ec);
    if (Writable(chosen)) {
      return chosen;
    }
  }
  return PrivateFolder();
#else
  const std::string exe = ExecutableFolder();
  if (!exe.empty() && !InAppBundle(exe) && Writable(exe)) {
    MigrateLooseData(exe);
    const std::string portable = PortableFolder();
    std::error_code ec;
    const std::string legacy = LegacyFolder();
    const bool hereHasSettings = std::filesystem::exists(FromUtf8(portable + "port_settings.ini"), ec);
    const bool legacyHasSettings =
        !legacy.empty() && std::filesystem::exists(FromUtf8(legacy + "port_settings.ini"), ec);
    if (hereHasSettings || !legacyHasSettings) {
      std::filesystem::create_directories(FromUtf8(portable), ec);
      return portable;
    }
    return legacy;
  }
  return WithSeparator(PrefFolder());
#endif
}

} // namespace detail

// Empty only when no folder could be found at all.
inline const std::string& UserFolder() {
  static const std::string sFolder = detail::Resolve();
  return sFolder;
}

// Messages about what the move of an older install's loose files did, empty
// when there was nothing to move. Paths resolve before the log is open, so
// main.cpp logs these once it is.
inline const std::vector<std::string>& MigrationLog() {
#if !defined(__ANDROID__)
  UserFolder();
  return detail::MigrationLines();
#else
  static const std::vector<std::string> sNone;
  return sNone;
#endif
}

// The memory card's folder. It has always been in the executable's folder, also
// for an install whose other data is still in the per-user folder and whatever
// MP_USER_PATH says; now that is its user/ subfolder. It follows UserFolder
// where the executable's folder is read-only (an AppImage, a Flatpak) or inside
// a macOS app bundle, where SDL_GetBasePath is Contents/Resources/: a card there
// breaks the bundle's signature and is lost with the app when it is replaced.
inline std::string CardFolder() {
#if !defined(__ANDROID__)
  if (const char* base = SDL_GetBasePath();
      base != nullptr && !detail::InAppBundle(base) && detail::Writable(base)) {
    detail::MigrateLooseData(detail::WithSeparator(base));
    const std::string folder = detail::WithSeparator(base) + "user/";
    std::error_code ec;
    std::filesystem::create_directories(detail::FromUtf8(folder), ec);
    return folder;
  }
#endif
  return UserFolder();
}

// True when the data is kept in the executable's portable folder or, on
// Android, outside the app's private storage.
inline bool IsPortable() {
  const std::string& folder = UserFolder();
#if defined(__ANDROID__)
  return !folder.empty() && folder != detail::PrivateFolder();
#else
  return !folder.empty() && folder == detail::PortableFolder();
#endif
}

#if defined(__ANDROID__)
// The data folder the player chose but this run could not use (no storage
// permission), or empty.
inline std::string UnavailableFolder() {
  const std::string chosen = detail::ReadMarker();
  if (chosen.empty() || chosen == UserFolder() || std::getenv("MP_USER_PATH") != nullptr) {
    return {};
  }
  return chosen;
}
#endif

} // namespace PortPaths
