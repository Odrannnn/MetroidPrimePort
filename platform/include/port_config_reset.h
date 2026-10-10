#pragma once

// F1 > System > Settings > "Reset configuration...": moves the port's own
// configuration files out of the user folder into a fresh backup folder inside
// it, so the next launch starts from defaults. Nothing is deleted, and only the
// allowlisted names below are touched (never saves, save states, mods, caches,
// the disc or the data folder marker). A symlink among the candidates makes the
// whole reset refuse, and a failure part-way moves everything back.
//
// port_settings.ini is replaced by a fresh file that keeps only the keys that
// say where the player's files are (the disc and Remastered image paths), so the
// reset does not make them pick the disc again; the backup holds the full file.
//
// Header-only so the test needs no more than this file.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace PortConfigReset {

namespace fs = std::filesystem;

struct Result {
  bool ok = false;
  bool moved = false;      // something was moved (false: no configuration existed)
  std::string backupDir;   // the backup folder; kept on a failed reset only if the rollback left files in it
  std::string error;       // set when !ok
  std::vector<std::string> files; // names moved into the backup
};

inline const char* const* FixedNames(size_t& count) {
  static const char* const kNames[] = {"port_settings.ini", "controls.toml", "imgui.ini", "controller_ports.dat",
                                       "keyboard_bindings.dat"};
  count = sizeof(kNames) / sizeof(kNames[0]);
  return kNames;
}

// Whether a name in the user folder is one the reset may move: the fixed files
// and aurora's <pad name>_<vid>_<pid>.controller mappings. A bare name only.
inline bool IsResettable(std::string_view name) {
  if (name.empty() || name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos ||
      name.front() == '.') {
    return false;
  }
  size_t count = 0;
  const char* const* fixed = FixedNames(count);
  for (size_t i = 0; i < count; ++i) {
    if (name == fixed[i]) {
      return true;
    }
  }
  constexpr std::string_view kSuffix = ".controller";
  return name.size() > kSuffix.size() && name.substr(name.size() - kSuffix.size()) == kSuffix;
}

// Keys of port_settings.ini that survive the reset.
inline bool IsPreservedSetting(std::string_view key) {
  return key == "disc_path" || key == "remastered_nsp" || key == "remastered_keys";
}

namespace detail {

inline std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

inline fs::path FromUtf8Path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

inline std::string Stamp() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm parts{};
#ifdef _WIN32
  localtime_s(&parts, &now);
#else
  localtime_r(&now, &parts);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &parts);
  return text;
}

// A new folder "config-backup-<stamp>[-n]" inside dir; create_directory fails
// on an existing one, so an older backup is never reused.
inline fs::path MakeBackupDir(const fs::path& dir, std::string& error) {
  const std::string stamp = Stamp();
  for (int n = 0; n < 1000; ++n) {
    const std::string name = "config-backup-" + stamp + (n == 0 ? "" : "-" + std::to_string(n));
    const fs::path candidate = dir / name;
    std::error_code ec;
    if (fs::create_directory(candidate, ec)) {
      return candidate;
    }
    if (ec) {
      error = "could not create " + Utf8(candidate) + ": " + ec.message();
      return {};
    }
  }
  error = "could not find an unused backup folder name";
  return {};
}

inline bool PreservedSettings(const fs::path& backup, std::string& out) {
  std::ifstream in(backup, std::ios::binary);
  if (!in) {
    return false;
  }
  out.clear();
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t eq = line.find('=');
    if (eq != std::string::npos && IsPreservedSetting(std::string_view(line).substr(0, eq))) {
      out += line + '\n';
    }
  }
  return in.eof() && !in.bad();
}

} // namespace detail

// Moves the configuration in `dir` into a new backup folder. `dir` may itself be
// a symlink; entries inside it may not.
// `beforeMove`, when set, is asked before each file is moved; returning false makes
// that move fail; `beforeRestore` does the same for the rollback's moves back (the
// tests use both).
inline Result Reset(const fs::path& dir, const std::function< bool(const std::string&) >& beforeMove = {},
                    const std::function< bool(const std::string&) >& beforeRestore = {}) {
  Result result;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    result.error = "user folder not found: " + detail::Utf8(dir);
    return result;
  }

  // Collect and vet the candidates before touching anything.
  std::vector<std::string> names;
  size_t fixedCount = 0;
  const char* const* fixed = FixedNames(fixedCount);
  for (size_t i = 0; i < fixedCount; ++i) {
    names.emplace_back(fixed[i]);
  }
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = detail::Utf8(it->path().filename());
    if (IsResettable(name) && std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(name);
    }
  }
  if (ec) {
    result.error = "could not list " + detail::Utf8(dir) + ": " + ec.message();
    return result;
  }
  std::vector<std::string> present;
  for (const std::string& name : names) {
    const fs::path path = dir / fs::path(std::u8string(name.begin(), name.end()));
    std::error_code e;
    const fs::file_status status = fs::symlink_status(path, e);
    if (status.type() == fs::file_type::not_found) {
      continue;
    }
    if (e || status.type() == fs::file_type::none) {
      result.error = "could not inspect " + name + (e ? ": " + e.message() : "") + "; nothing was changed";
      return result;
    }
    if (fs::is_symlink(status)) {
      result.error = name + " is a symbolic link; nothing was changed";
      return result;
    }
    if (!fs::is_regular_file(status)) {
      result.error = name + " is not a regular file; nothing was changed";
      return result;
    }
    present.push_back(name);
  }
  if (present.empty()) {
    result.ok = true;
    return result;
  }

  const fs::path backup = detail::MakeBackupDir(dir, result.error);
  if (backup.empty()) {
    return result;
  }

  std::vector<std::string> moved;
  fs::path freshSettings;
  const auto rollback = [&](const std::string& why) {
    std::error_code e;
    if (!freshSettings.empty()) {
      fs::remove(freshSettings, e);
    }
    std::vector<std::string> stuck;
    std::string detail;
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
      const fs::path name(std::u8string(it->begin(), it->end()));
      std::error_code back;
      if (beforeRestore && !beforeRestore(*it)) {
        back = std::make_error_code(std::errc::io_error);
      } else {
        fs::rename(backup / name, dir / name, back);
      }
      if (back) {
        stuck.push_back(*it);
        detail += " (could not restore " + *it + ": " + back.message() + ")";
      }
    }
    result.ok = false;
    result.moved = false;
    result.files = stuck;
    result.error = why + detail;
    // The backup folder goes only when empty; with files stuck in it, its path is
    // kept so the caller can say where they are.
    std::error_code rm;
    if (stuck.empty() && fs::remove(backup, rm)) {
      result.backupDir.clear();
    } else {
      result.backupDir = detail::Utf8(backup);
    }
    return result;
  };

  for (const std::string& name : present) {
    const fs::path file(std::u8string(name.begin(), name.end()));
    std::error_code e;
    if (beforeMove && !beforeMove(name)) {
      return rollback("could not move " + name + ": simulated failure");
    }
    fs::rename(dir / file, backup / file, e);
    if (e) {
      return rollback("could not move " + name + ": " + e.message());
    }
    moved.push_back(name);
  }

  std::string keep;
  if (std::find(moved.begin(), moved.end(), "port_settings.ini") != moved.end() &&
      !detail::PreservedSettings(backup / "port_settings.ini", keep)) {
    return rollback("could not read the settings to keep the disc paths");
  }
  if (!keep.empty()) {
    freshSettings = dir / "port_settings.ini";
    std::ofstream out(freshSettings, std::ios::binary | std::ios::trunc);
    out << keep;
    out.flush();
    if (!out) {
      out.close();
      return rollback("could not write the kept settings");
    }
  }

  result.ok = true;
  result.moved = true;
  result.backupDir = detail::Utf8(backup);
  result.files = moved;
  return result;
}

} // namespace PortConfigReset
