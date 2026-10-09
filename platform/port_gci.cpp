#include "port_gci.h"

#include <aurora/card.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace PortGci {

namespace {
char sGameCode[5] = "GM8E";
} // namespace

void SetGameCode(const char* code4) {
  if (code4 != nullptr && std::strlen(code4) >= 4)
    std::memcpy(sGameCode, code4, 4);
}

const char* GameCode() { return sGameCode; }

const char* CardRegion() { return sGameCode[3] == 'P' ? "EUR" : "USA"; }

std::string PathString(const fs::path& path) {
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

fs::path PathFromString(const std::string& text) {
  return fs::path(std::u8string(text.begin(), text.end()));
}

namespace {

std::atomic_bool sCardChanged{false};

std::string Field(const uint8_t* data, size_t size) {
  size_t length = 0;
  while (length < size && data[length] != 0) {
    ++length;
  }
  return std::string(reinterpret_cast< const char* >(data), length);
}

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + PathString(path);
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (in.bad()) {
    error = "cannot read " + PathString(path);
    return false;
  }
  return true;
}

// Written beside the target and renamed over it, so a failed write leaves no
// half file. The temporary name does not end in .gci: a card never reads it.
bool WriteAll(const fs::path& path, const std::vector<uint8_t>& data, std::string& error) {
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast< const char* >(data.data()),
              static_cast< std::streamsize >(data.size()));
    out.close();
    if (!out) {
      std::error_code ignored;
      fs::remove(temp, ignored);
      error = "cannot write " + PathString(path);
      return false;
    }
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    fs::remove(temp, ec);
    error = "cannot write " + PathString(path);
    return false;
  }
  return true;
}

bool ReadHeader(const fs::path& path, Header& out) {
  std::ifstream in(path, std::ios::binary);
  uint8_t header[kHeaderSize];
  if (!in.read(reinterpret_cast< char* >(header), sizeof(header))) {
    return false;
  }
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) {
    return false;
  }
  // Only the header is read, so check it against the real size by hand.
  std::string error;
  if (!ParseHeader(header, kHeaderSize, out, error) && error != "truncated") {
    return false;
  }
  return size == kHeaderSize + size_t(out.blockCount) * kBlockSize;
}

std::string Timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
  return buffer;
}

// Moves a file into <folder>/_replaced under a name that is not taken yet.
bool MoveAside(const fs::path& file, std::string& error) {
  std::error_code ec;
  const fs::path dir = file.parent_path() / "_replaced";
  fs::create_directories(dir, ec);
  const std::string stem = PathString(file.stem()) + "-" + Timestamp();
  fs::path target = dir / PathFromString(stem + ".gci");
  for (int suffix = 1; fs::exists(target, ec); ++suffix) {
    target = dir / PathFromString(stem + "-" + std::to_string(suffix) + ".gci");
  }
  fs::rename(file, target, ec);
  if (ec) {
    error = "cannot move " + PathString(file) + " aside: " + ec.message();
    return false;
  }
  return true;
}

void AddError(Report& report, std::string error) {
  ++report.skipped;
  report.errors.push_back(std::move(error));
}

} // namespace

bool ParseHeader(const uint8_t* data, size_t size, Header& out, std::string& error) {
  if (data == nullptr || size < kHeaderSize) {
    error = "not a .gci file (too short)";
    return false;
  }
  out.game = Field(data, 4);
  out.maker = Field(data + 4, 2);
  out.fileName = Field(data + 8, 32);
  out.blockCount = uint16_t((data[0x38] << 8) | data[0x39]);
  if (out.game.size() != 4 || out.maker.size() != 2 || out.fileName.empty()) {
    error = "not a .gci file (bad header)";
    return false;
  }
  if (out.blockCount == 0 || out.blockCount > 2043) {
    error = "not a .gci file (bad block count)";
    return false;
  }
  if (size == kHeaderSize) {
    // Callers holding only the header check the size themselves.
    error = "truncated";
    return false;
  }
  if (size != kHeaderSize + size_t(out.blockCount) * kBlockSize) {
    error = "not a .gci file (size does not match its header)";
    return false;
  }
  return true;
}

bool IsGameFile(const Header& header) { return header.game == sGameCode && header.maker == "01"; }

std::string DiskName(const Header& header) {
  std::string name = header.maker + "-" + header.game + "-" + header.fileName;
  for (char& c : name) {
    if (static_cast< unsigned char >(c) < 32 || std::strchr("<>:\"/\\|?*", c) != nullptr) {
      c = '_';
    }
  }
  return name + ".gci";
}

std::string Report::Summary(const char* verb) const {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "%s %d save file%s", verb, copied, copied == 1 ? "" : "s");
  std::string text = buffer;
  if (replaced > 0) {
    std::snprintf(buffer, sizeof(buffer), "; %d earlier file%s kept in _replaced", replaced,
                  replaced == 1 ? "" : "s");
    text += buffer;
  }
  if (skipped > 0) {
    std::snprintf(buffer, sizeof(buffer), "; %d skipped", skipped);
    text += buffer;
  }
  if (!note.empty()) {
    text += "; " + note;
  }
  text += ".";
  for (const std::string& error : errors) {
    text += "\n" + error;
  }
  return text;
}

bool InstallGci(const fs::path& folder, const std::vector<uint8_t>& gci, std::string& error,
                bool* replacedOut) {
  Header header;
  if (!ParseHeader(gci.data(), gci.size(), header, error)) {
    return false;
  }
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (!fs::is_directory(folder, ec)) {
    error = "no folder at " + PathString(folder);
    return false;
  }
  const fs::path target = folder / PathFromString(DiskName(header));
  bool replaced = false;
  // A card refuses two files with one identity, whatever they are called, and
  // the file at the target name has to go even if it is something else.
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    const fs::path& path = entry.path();
    if (!entry.is_regular_file(ec) || path.extension() != ".gci") {
      continue;
    }
    Header existing;
    const bool sameName = path.filename() == target.filename();
    const bool sameIdentity = ReadHeader(path, existing) && existing.game == header.game &&
                              existing.maker == header.maker &&
                              existing.fileName == header.fileName;
    if (sameName || sameIdentity) {
      if (!MoveAside(path, error)) {
        return false;
      }
      replaced = true;
    }
  }
  if (ec) {
    error = "cannot list " + PathString(folder) + ": " + ec.message();
    return false;
  }
  if (!WriteAll(target, gci, error)) {
    return false;
  }
  if (replacedOut != nullptr) {
    *replacedOut = replaced;
  }
  return true;
}

namespace {

void Install(Report& report, const fs::path& folder, const std::vector<uint8_t>& gci) {
  std::string error;
  bool replaced = false;
  if (InstallGci(folder, gci, error, &replaced)) {
    ++report.copied;
    report.replaced += replaced ? 1 : 0;
  } else {
    AddError(report, error);
  }
}

// The game keeps one save as "MetroidPrime A" or "B" and loads whichever is
// newer, so an imported A beside a newer B would be ignored. A save therefore
// moves as a set: every game file already there is moved aside first.
void InstallSet(Report& report, const fs::path& folder,
                const std::vector<std::vector<uint8_t>>& set) {
  if (set.empty()) {
    return;
  }
  for (const fs::path& old : GameFiles(folder)) {
    std::string error;
    if (!MoveAside(old, error)) {
      AddError(report, error);
      return;
    }
    ++report.replaced;
  }
  for (const auto& gci : set) {
    Install(report, folder, gci);
  }
}

void CollectName(const char* fileName, void* userData) {
  static_cast< std::vector<std::string>* >(userData)->push_back(fileName);
}

} // namespace

Report ImportBytes(const std::vector<uint8_t>& bytes, const std::string& name, const fs::path& folder,
                   const fs::path& scratch) {
  Report report;
  Header header;
  std::string gciError;
  if (ParseHeader(bytes.data(), bytes.size(), header, gciError)) {
    if (!IsGameFile(header)) {
      AddError(report, name + " is a save of another game (" + header.game + ")");
    } else {
      InstallSet(report, folder, {bytes});
    }
    return report;
  }
  // Raw images are whole cards: 59 to 2043 blocks plus the 5 system blocks.
  if (bytes.size() < 64 * kBlockSize || bytes.size() % kBlockSize != 0) {
    AddError(report, name + ": " + gciError + ", and not a raw memory card image either");
    return report;
  }
  std::error_code ec;
  fs::create_directories(scratch, ec);
  const fs::path image = scratch / "gci-import.raw";
  std::string error;
  if (!WriteAll(image, bytes, error)) {
    AddError(report, error);
    return report;
  }
  const std::string imagePath = PathString(image);
  std::vector<std::string> files;
  if (!aurora_card_raw_list(imagePath.c_str(), sGameCode, "01", CollectName, &files)) {
    AddError(report, name + " is neither a .gci file nor a readable memory card image");
  } else if (files.empty()) {
    AddError(report, name + " holds no Metroid Prime save");
  }
  std::vector<uint8_t> gci(bytes.size());
  std::vector<std::vector<uint8_t>> set;
  for (const std::string& file : files) {
    const size_t size = aurora_card_raw_extract(imagePath.c_str(), sGameCode, "01", file.c_str(),
                                                gci.data(), gci.size());
    if (size == 0 || size > gci.size()) {
      AddError(report, "cannot extract " + file + " from " + name);
      set.clear(); // half a save set would be worse than none
      break;
    }
    set.emplace_back(gci.begin(), gci.begin() + size);
  }
  fs::remove(image, ec);
  InstallSet(report, folder, set);
  return report;
}

Report ImportFile(const fs::path& source, const fs::path& folder, const fs::path& scratch) {
  std::vector<uint8_t> bytes;
  std::string error;
  if (!ReadAll(source, bytes, error)) {
    Report report;
    AddError(report, error);
    return report;
  }
  return ImportBytes(bytes, PathString(source.filename()), folder, scratch);
}

std::vector<fs::path> GameFiles(const fs::path& folder) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    Header header;
    if (entry.is_regular_file(ec) && entry.path().extension() == ".gci" &&
        ReadHeader(entry.path(), header) && IsGameFile(header)) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

Report ImportFolder(const fs::path& source, const fs::path& folder) {
  Report report;
  const auto files = GameFiles(source);
  if (files.empty()) {
    AddError(report, "no Metroid Prime save in " + PathString(source));
  }
  std::vector<std::vector<uint8_t>> set;
  for (const fs::path& file : files) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!ReadAll(file, bytes, error)) {
      AddError(report, error);
      return report; // half a save set would be worse than none
    }
    set.push_back(std::move(bytes));
  }
  InstallSet(report, folder, set);
  return report;
}

Report ExportFolder(const fs::path& folder, const fs::path& dest) {
  // Exporting a card into itself would only move its files aside.
  std::error_code ec;
  if (fs::equivalent(folder, dest, ec)) {
    Report report;
    AddError(report, "that is the game's own card folder");
    return report;
  }
  return ImportFolder(folder, dest);
}

Report ExportRaw(const fs::path& folder, const fs::path& image) {
  Report report;
  const auto files = GameFiles(folder);
  if (files.empty()) {
    AddError(report, "the game's card holds no save");
    return report;
  }
  std::error_code ec;
  if (fs::exists(image, ec)) {
    fs::path backup = image;
    backup += ".bak";
    fs::copy_file(image, backup, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      AddError(report, "cannot back up " + PathString(image) + ": " + ec.message());
      return report;
    }
    report.note = "the previous card image is kept as " + PathString(backup.filename());
  }
  const std::string imagePath = PathString(image);
  std::vector<std::vector<uint8_t>> set;
  std::vector<std::string> names;
  for (const fs::path& file : files) {
    std::vector<uint8_t> bytes;
    std::string error;
    Header header;
    if (!ReadAll(file, bytes, error) || !ParseHeader(bytes.data(), bytes.size(), header, error)) {
      AddError(report, error);
      return report;
    }
    names.push_back(header.fileName);
    set.push_back(std::move(bytes));
  }
  // As for folders: the other of A/B, if the image has it, would win when newer.
  std::vector<std::string> existing;
  if (fs::exists(image, ec)) {
    aurora_card_raw_list(imagePath.c_str(), sGameCode, "01", CollectName, &existing);
  }
  for (const std::string& old : existing) {
    if (std::find(names.begin(), names.end(), old) == names.end() &&
        !aurora_card_raw_delete(imagePath.c_str(), sGameCode, "01", old.c_str())) {
      AddError(report, "cannot remove the old " + old + " from " + imagePath);
      return report;
    }
  }
  for (size_t i = 0; i < set.size(); ++i) {
    const std::vector<uint8_t>& bytes = set[i];
    if (!aurora_card_raw_insert(imagePath.c_str(), bytes.data(), bytes.size(), true)) {
      AddError(report, "cannot write " + names[i] + " into " + imagePath +
                           " (card full, or not a card image?)");
    } else {
      ++report.copied;
    }
  }
  return report;
}

DolphinCard FindDolphinCard() {
  DolphinCard card;
#if !defined(__ANDROID__)
  std::vector<fs::path> roots;
#if defined(_WIN32)
  // Dolphin's own rule: the registry's UserConfigPath, else Documents.
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Dolphin Emulator", 0, KEY_QUERY_VALUE, &key) ==
      ERROR_SUCCESS) {
    wchar_t value[MAX_PATH] = {};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    if (RegQueryValueExW(key, L"UserConfigPath", nullptr, nullptr,
                         reinterpret_cast< LPBYTE >(value), &size) == ERROR_SUCCESS &&
        value[0] != 0) {
      roots.emplace_back(value);
    }
    RegCloseKey(key);
  }
  PWSTR documents = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
    roots.push_back(fs::path(documents) / L"Dolphin Emulator");
  }
  CoTaskMemFree(documents);
#else
  const char* home = std::getenv("HOME");
  const std::string homeDir = home != nullptr ? home : "";
#if defined(__APPLE__)
  if (!homeDir.empty()) {
    roots.push_back(fs::path(homeDir) / "Library/Application Support/Dolphin");
  }
#else
  // Not SDL_GetPrefPath: it would create the folder.
  const char* dataHome = std::getenv("XDG_DATA_HOME");
  if (dataHome != nullptr && dataHome[0] == '/') {
    roots.push_back(fs::path(dataHome) / "dolphin-emu");
  } else if (!homeDir.empty()) {
    roots.push_back(fs::path(homeDir) / ".local/share/dolphin-emu");
  }
  if (!homeDir.empty()) {
    roots.push_back(fs::path(homeDir) / ".var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu");
    roots.push_back(fs::path(homeDir) / ".dolphin-emu");
  }
#endif
#endif
  std::error_code ec;
  for (const fs::path& root : roots) {
    const fs::path folder = root / "GC" / CardRegion() / "Card A";
    const fs::path raw = root / "GC" / (std::string("MemoryCardA.") + CardRegion() + ".raw");
    const bool hasFolder = fs::is_directory(folder, ec);
    const bool hasRaw = fs::is_regular_file(raw, ec);
    if (hasFolder || hasRaw) {
      card.gciFolder = hasFolder ? folder : fs::path();
      card.rawImage = hasRaw ? raw : fs::path();
      break;
    }
  }
#endif
  return card;
}

fs::path MountedCardFolder() {
  if (aurora_card_get_type(0) != AURORA_CARD_GCI_DIRECTORY) {
    return {};
  }
  char buffer[4096];
  if (aurora_card_get_path(sGameCode, AURORA_CARD_GCI_DIRECTORY, 0, buffer, sizeof(buffer)) == 0 ||
      buffer[0] == '\0') {
    return {};
  }
  return PathFromString(buffer);
}

void MarkCardChanged() { sCardChanged = true; }

bool RemountIfChanged() {
  if (!sCardChanged.exchange(false)) {
    return false;
  }
  aurora_card_remount(0);
  return true;
}

} // namespace PortGci
