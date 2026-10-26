#include "port_gci.h"
#include "port_env.h"
#include "port_strings.h"
#include "port_bytes.h"

#include <aurora/card.h>

#include "port_save_convert.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
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

// Trilogy's (R3ME) saves have PAL's layout.
ExportFormat NativeExport() {
  return PortSaveConvert::NativeFormat(sGameCode[3] == 'P' || std::memcmp(sGameCode, "R3M", 3) == 0);
}

const char* GameCodeOf(ExportFormat format) { return format == ExportFormat::PalGci ? "GM8P" : "GM8E"; }

const char* CardRegionOf(ExportFormat format) { return format == ExportFormat::PalGci ? "EUR" : "USA"; }

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
  out = port::ReadAll(in);
  if (in.bad()) {
    error = "cannot read " + PathString(path);
    return false;
  }
  return true;
}

// Written beside the target and renamed over it, so a failed write leaves no
// half file. The temporary name does not end in .gci: a card never reads it.
fs::path TempOf(const fs::path& path) {
  fs::path temp = path;
  temp += ".tmp";
  return temp;
}

bool WriteTemp(const fs::path& path, const std::vector<uint8_t>& data, std::string& error) {
  const fs::path temp = TempOf(path);
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast< const char* >(data.data()), static_cast< std::streamsize >(data.size()));
  out.close();
  if (!out) {
    std::error_code ignored;
    fs::remove(temp, ignored);
    error = "cannot write " + PathString(path);
    return false;
  }
  return true;
}

bool CommitTemp(const fs::path& path, std::string& error) {
  const fs::path temp = TempOf(path);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    error = "cannot write " + PathString(path) + ": " + ec.message();
    std::error_code ignored;
    fs::remove(temp, ignored);
    return false;
  }
  return true;
}

bool WriteAll(const fs::path& path, const std::vector<uint8_t>& data, std::string& error) {
  return WriteTemp(path, data, error) && CommitTemp(path, error);
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

// A file moved into _replaced, and where it came from.
using Moved = std::vector< std::pair< fs::path, fs::path > >;

// Moves a file into <folder>/_replaced under a name that is not taken yet.
bool MoveAside(const fs::path& file, std::string& error, Moved* moved = nullptr) {
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
  if (moved != nullptr) {
    moved->emplace_back(file, target);
  }
  return true;
}

// Puts files moved aside back where they were, after the write that was to
// replace them failed: a failed import must not leave the card without its save.
void Restore(const Moved& moved) {
  for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
    std::error_code ec;
    fs::rename(it->second, it->first, ec);
  }
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

namespace {
// Metroid Prime's files of one release (game code) in a GCI folder.
std::vector<fs::path> FilesOf(const fs::path& folder, const std::string& code) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    Header header;
    if (entry.is_regular_file(ec) && entry.path().extension() == ".gci" &&
        ReadHeader(entry.path(), header) && header.game == code && header.maker == "01") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}
} // namespace

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

namespace {

// Written to a temporary file before anything is moved aside, so a write that
// fails (full disk, read-only folder) leaves the card as it was. Files moved
// aside are added to *moved, the file written to *written.
bool Install(const fs::path& folder, const std::vector<uint8_t>& gci, std::string& error,
             Moved& moved, fs::path* written) {
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
  if (!WriteTemp(target, gci, error)) {
    return false;
  }
  const size_t movedBefore = moved.size();
  const auto fail = [&] {
    std::error_code ignored;
    fs::remove(TempOf(target), ignored);
    Restore(Moved(moved.begin() + static_cast< std::ptrdiff_t >(movedBefore), moved.end()));
    moved.resize(movedBefore);
    return false;
  };
  // A card refuses two files with one identity, whatever they are called, and
  // the file at the target name has to go even if it is something else.
  // Listed first, then moved: the listing's error is the only one that means
  // the folder could not be read, and nothing is renamed mid-iteration.
  std::vector< fs::path > candidates;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && it->path().extension() == ".gci") {
      candidates.push_back(it->path());
    }
  }
  if (ec) {
    error = "cannot list " + PathString(folder) + ": " + ec.message();
    return fail();
  }
  for (const fs::path& path : candidates) {
    Header existing;
    const bool sameName = path.filename() == target.filename();
    const bool sameIdentity = ReadHeader(path, existing) && existing.game == header.game &&
                              existing.maker == header.maker &&
                              existing.fileName == header.fileName;
    if ((sameName || sameIdentity) && !MoveAside(path, error, &moved)) {
      return fail();
    }
  }
  if (!CommitTemp(target, error)) {
    return fail();
  }
  if (written != nullptr) {
    *written = target;
  }
  return true;
}

} // namespace

bool InstallGci(const fs::path& folder, const std::vector<uint8_t>& gci, std::string& error,
                bool* replacedOut) {
  Moved moved;
  if (!Install(folder, gci, error, moved, nullptr)) {
    return false;
  }
  if (replacedOut != nullptr) {
    *replacedOut = !moved.empty();
  }
  return true;
}

namespace {

// The game keeps one save as "MetroidPrime A" or "B" and loads whichever is
// newer, so an imported A beside a newer B would be ignored. A save therefore
// moves as a set: every game file already there is moved aside first. If any
// file of the set fails, the ones written are removed and the old set is put
// back, so the card keeps a save it can load. The files take code as their
// game code: a Trilogy disc's card holds GameCube PAL files under R3ME/R3MP.
void InstallSet(Report& report, const fs::path& folder, std::vector<std::vector<uint8_t>> set,
                const std::string& code = sGameCode) {
  if (set.empty()) {
    return;
  }
  for (auto& gci : set) {
    if (gci.size() >= kHeaderSize && code.size() == 4) {
      std::memcpy(gci.data(), code.data(), 4);
    }
  }
  Moved moved;
  for (const fs::path& old : FilesOf(folder, code)) {
    std::string error;
    if (!MoveAside(old, error, &moved)) {
      Restore(moved);
      AddError(report, error);
      return;
    }
  }
  std::vector< fs::path > written;
  for (const auto& gci : set) {
    std::string error;
    fs::path target;
    if (!Install(folder, gci, error, moved, &target)) {
      for (const fs::path& path : written) {
        std::error_code ignored;
        fs::remove(path, ignored);
      }
      Restore(moved);
      AddError(report, error + " (nothing was replaced)");
      return;
    }
    written.push_back(target);
  }
  report.copied += static_cast< int >(written.size());
  report.replaced += static_cast< int >(moved.size());
}

void CollectName(const char* fileName, void* userData) {
  static_cast< std::vector<std::string>* >(userData)->push_back(fileName);
}

} // namespace

namespace {

bool OnPalDisc() { return sGameCode[3] == 'P'; }

// Banner and icon of a game file already in the card folder, for converted
// saves that have none of their own.
std::vector<uint8_t> ExistingArt(const fs::path& folder) {
  for (const fs::path& file : GameFiles(folder)) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (ReadAll(file, bytes, error)) {
      std::vector<uint8_t> art = PortSaveConvert::ArtOf(bytes.data(), bytes.size());
      if (!art.empty()) {
        return art;
      }
    }
  }
  return {};
}

struct Converted {
  std::vector<uint8_t> gci;
  std::string note;
  uint32_t modTime = 0;
  uint32_t newest = 0;
};

std::string ConversionNote(const PortSaveConvert::Conversion& conversion) {
  std::string text = std::string("converted from ") + PortSaveConvert::FormatName(conversion.from);
  if (conversion.stats.Any()) {
    text += " (" + conversion.stats.Summary() + ")";
  }
  return text;
}

bool Convert(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& art, Converted& out,
             std::string& error, PortSaveConvert::Format target = NativeExport(),
             const std::vector<uint8_t>* trilogyBase = nullptr) {
  PortSaveConvert::Conversion conversion;
  const bool ok = target == PortSaveConvert::Format::WiiSave
                      ? trilogyBase != nullptr &&
                            PortSaveConvert::ConvertToTrilogy(bytes.data(), bytes.size(), trilogyBase->data(),
                                                              trilogyBase->size(), conversion, error)
                      : PortSaveConvert::ConvertTo(bytes.data(), bytes.size(), target, art, conversion, error);
  if (!ok) {
    if (error.empty()) {
      error = "no Trilogy save.bin to write into";
    }
    return false;
  }
  out.gci = std::move(conversion.gci);
  out.note = ConversionNote(conversion);
  out.newest = conversion.newest;
  if (bytes.size() >= 0x2C) {
    out.modTime = port::GetBE32(bytes, 0x28);
  }
  return true;
}

bool IsNative(const std::vector<uint8_t>& bytes) {
  return PortSaveConvert::Detect(bytes.data(), bytes.size()) == NativeExport();
}

// Installs a set of candidate files: when some already are the disc's format
// only those count; otherwise the newest is converted (the converted names
// would collide, and the game loads one save). They are installed as code's.
void InstallCandidates(Report& report, const fs::path& folder, const std::string& name,
                       const std::vector<std::vector<uint8_t>>& found, const std::string& code = sGameCode) {
  std::vector<std::vector<uint8_t>> native;
  for (const auto& bytes : found) {
    if (IsNative(bytes)) {
      native.push_back(bytes);
    }
  }
  if (!native.empty()) {
    InstallSet(report, folder, native, code);
    return;
  }
  const std::vector<uint8_t> art = ExistingArt(folder);
  Converted best;
  bool have = false;
  for (const auto& bytes : found) {
    Converted c;
    std::string error;
    if (!Convert(bytes, art, c, error)) {
      AddError(report, name + ": " + error);
      continue;
    }
    if (!have || std::make_pair(c.modTime, c.newest) > std::make_pair(best.modTime, best.newest)) {
      best = std::move(c);
      have = true;
    }
  }
  if (!have) {
    return;
  }
  InstallSet(report, folder, {best.gci}, code);
  report.note = report.note.empty() ? best.note : report.note + "; " + best.note;
}

} // namespace

Report ImportBytes(const std::vector<uint8_t>& bytes, const std::string& name, const fs::path& folder,
                   const fs::path& scratch) {
  Report report;
  Header header;
  std::string gciError;
  const PortSaveConvert::Format format = PortSaveConvert::Detect(bytes.data(), bytes.size());
  if (format == PortSaveConvert::Format::WiiSave || format == PortSaveConvert::Format::WiiDataBin) {
    InstallCandidates(report, folder, name, {bytes});
    return report;
  }
  if (ParseHeader(bytes.data(), bytes.size(), header, gciError)) {
    if (format == PortSaveConvert::Format::Unknown || header.maker != "01") {
      AddError(report, name + " is a save of another game (" + header.game + ")");
    } else {
      InstallCandidates(report, folder, name, {bytes});
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
  std::vector<uint8_t> gci(bytes.size());
  std::vector<std::vector<uint8_t>> set;
  bool listed = false;
  bool failed = false;
  // Either region's file can be converted, so look for both game codes.
  for (const char* code : {"GM8E", "GM8P"}) {
    std::vector<std::string> files;
    if (!aurora_card_raw_list(imagePath.c_str(), code, "01", CollectName, &files)) {
      continue;
    }
    listed = true;
    for (const std::string& file : files) {
      const size_t size = aurora_card_raw_extract(imagePath.c_str(), code, "01", file.c_str(), gci.data(),
                                                  gci.size());
      if (size == 0 || size > gci.size()) {
        AddError(report, "cannot extract " + file + " from " + name);
        failed = true;
        break;
      }
      set.emplace_back(gci.begin(), gci.begin() + size);
    }
  }
  fs::remove(image, ec);
  if (!listed) {
    AddError(report, name + " is neither a .gci file nor a readable memory card image");
  } else if (set.empty() && !failed) {
    AddError(report, name + " holds no Metroid Prime save");
  }
  if (failed) {
    return report; // half a save set would be worse than none
  }
  InstallCandidates(report, folder, name, set);
  return report;
}

Report MigrateLegacyPal(const fs::path& folder) {
  Report report;
  if (!OnPalDisc()) {
    return report;
  }
  struct Old {
    fs::path path;
    std::vector<uint8_t> bytes;
  };
  std::vector<Old> legacy;
  for (const fs::path& file : GameFiles(folder)) {
    Header header;
    if (!ReadHeader(file, header)) {
      continue;
    }
    if (header.fileName == "MetroidPrime") {
      return report; // already has a save in the PAL format
    }
    Old old;
    std::string error;
    if (ReadAll(file, old.bytes, error) &&
        PortSaveConvert::Detect(old.bytes.data(), old.bytes.size()) == PortSaveConvert::Format::PalLegacyGci) {
      old.path = file;
      legacy.push_back(std::move(old));
    }
  }
  if (legacy.empty()) {
    return report;
  }
  Converted best;
  bool have = false;
  for (const Old& old : legacy) {
    Converted c;
    std::string error;
    if (!Convert(old.bytes, {}, c, error)) {
      AddError(report, PathString(old.path.filename()) + ": " + error);
      continue;
    }
    if (!have || std::make_pair(c.modTime, c.newest) > std::make_pair(best.modTime, best.newest)) {
      best = std::move(c);
      have = true;
    }
  }
  if (!have) {
    return report;
  }
  std::string error;
  if (!InstallGci(folder, best.gci, error)) {
    AddError(report, error);
    return report;
  }
  ++report.copied;
  for (const Old& old : legacy) {
    if (MoveAside(old.path, error)) {
      ++report.replaced;
    } else {
      AddError(report, error);
    }
  }
  report.note = best.note;
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

std::vector<fs::path> GameFiles(const fs::path& folder) { return FilesOf(folder, sGameCode); }

std::vector<fs::path> ImportableFiles(const fs::path& source) {
  for (const char* code : {static_cast< const char* >(sGameCode), "GM8E", "GM8P", "R3ME", "R3MP"}) {
    auto files = FilesOf(source, code);
    if (!files.empty()) {
      return files;
    }
  }
  return {};
}

namespace {

// Copies a save set into folder as code's, converted if it is another release's.
Report CopyFiles(const fs::path& source, const std::vector<fs::path>& files, const fs::path& folder,
                 const std::string& code) {
  Report report;
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
  if (!set.empty()) {
    InstallCandidates(report, folder, PathString(source.filename()), set, code);
  }
  return report;
}
} // namespace

Report ImportFolder(const fs::path& source, const fs::path& folder) {
  return CopyFiles(source, ImportableFiles(source), folder, sGameCode);
}

bool ExportBytes(const fs::path& folder, ExportFormat format, std::vector<uint8_t>& gci, std::string& note,
                 std::string& error, const std::vector<uint8_t>& trilogyBase) {
  // The card's files hold one save between them (A/B): convert the newest.
  const std::vector<uint8_t> art = ExistingArt(folder);
  Converted best;
  bool have = false;
  error.clear();
  for (const fs::path& file : GameFiles(folder)) {
    std::vector<uint8_t> bytes;
    Converted c;
    std::string fileError;
    if (!ReadAll(file, bytes, fileError) || !Convert(bytes, art, c, fileError, format, &trilogyBase)) {
      if (error.empty()) {
        error = PathString(file.filename()) + ": " + fileError;
      }
      continue;
    }
    if (!have || std::make_pair(c.modTime, c.newest) > std::make_pair(best.modTime, best.newest)) {
      best = std::move(c);
      have = true;
    }
  }
  if (!have) {
    if (error.empty()) {
      error = "the game's card holds no save";
    }
    return false;
  }
  gci = std::move(best.gci);
  note = format == ExportFormat::WiiSave ? std::string("written into the Trilogy save as its Metroid Prime files")
                                         : std::string("converted to ") + PortSaveConvert::FormatName(format);
  const size_t stats = best.note.find(" (");
  if (stats != std::string::npos) {
    note += best.note.substr(stats);
  }
  error.clear();
  return true;
}

Report ExportFolder(const fs::path& folder, const fs::path& dest, ExportFormat format) {
  // Exporting a card into itself would only move its files aside.
  std::error_code ec;
  Report report;
  if (fs::equivalent(folder, dest, ec)) {
    AddError(report, "that is the game's own card folder");
    return report;
  }
  if (format == NativeExport()) {
    return CopyFiles(folder, GameFiles(folder), dest, GameCodeOf(format));
  }
  std::vector<uint8_t> gci;
  std::string error;
  if (!ExportBytes(folder, format, gci, report.note, error)) {
    AddError(report, error);
    return report;
  }
  InstallSet(report, dest, {gci}, GameCodeOf(format));
  return report;
}

Report ExportRaw(const fs::path& folder, const fs::path& image, ExportFormat format) {
  Report report;
  const auto files = GameFiles(folder);
  if (files.empty()) {
    AddError(report, "the game's card holds no save");
    return report;
  }
  std::vector<std::vector<uint8_t>> set;
  std::vector<std::string> names;
  std::string converted;
  const char* code = GameCodeOf(format);
  if (format != NativeExport()) {
    std::vector<uint8_t> gci;
    std::string error;
    Header header;
    if (!ExportBytes(folder, format, gci, converted, error) ||
        !ParseHeader(gci.data(), gci.size(), header, error)) {
      AddError(report, error);
      return report;
    }
    names.push_back(header.fileName);
    set.push_back(std::move(gci));
  } else {
    for (const fs::path& file : files) {
      std::vector<uint8_t> bytes;
      std::string error;
      Header header;
      if (!ReadAll(file, bytes, error) || !ParseHeader(bytes.data(), bytes.size(), header, error)) {
        AddError(report, error);
        return report;
      }
      names.push_back(header.fileName);
      std::memcpy(bytes.data(), code, 4); // a Trilogy disc's files go out as GameCube ones
      set.push_back(std::move(bytes));
    }
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
  if (!converted.empty()) {
    report.note = report.note.empty() ? converted : converted + "; " + report.note;
  }
  const std::string imagePath = PathString(image);
  // As for folders: the other of A/B, if the image has it, would win when newer.
  std::vector<std::string> existing;
  if (fs::exists(image, ec)) {
    aurora_card_raw_list(imagePath.c_str(), code, "01", CollectName, &existing);
  }
  for (const std::string& old : existing) {
    if (std::find(names.begin(), names.end(), old) == names.end() &&
        !aurora_card_raw_delete(imagePath.c_str(), code, "01", old.c_str())) {
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

Report ExportTrilogy(const fs::path& folder, const fs::path& saveBin) {
  Report report;
  std::vector<uint8_t> base;
  std::string error;
  if (!ReadAll(saveBin, base, error)) {
    AddError(report, error + " (start the Trilogy once so that it makes its save)");
    return report;
  }
  std::vector<uint8_t> bin;
  if (!ExportBytes(folder, ExportFormat::WiiSave, bin, report.note, error, base)) {
    AddError(report, error);
    return report;
  }
  fs::path backup = saveBin;
  backup += ".bak";
  std::error_code ec;
  fs::copy_file(saveBin, backup, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    AddError(report, "cannot back up " + PathString(saveBin) + ": " + ec.message());
    return report;
  }
  if (!WriteAll(saveBin, bin, error)) {
    AddError(report, error);
    return report;
  }
  report.note += "; the previous save is kept as " + PathString(backup.filename());
  report.copied = 1;
  return report;
}

namespace {

std::mutex sEmulatorMutex;
std::string sEmulatorFolder;
bool sShareCard = false;

// Where Dolphin (and PrimeHack) keep their user folders, in preference order.
std::vector<fs::path> AutoDolphinRoots() {
  std::vector<fs::path> roots;
#if defined(__ANDROID__)
  // No Dolphin to find.
#elif defined(_WIN32)
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
  const char* home = port::HomeFolder();
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
    roots.push_back(fs::path(homeDir) / ".var/app/io.github.shiiion.primehack/data/dolphin-emu");
  }
#endif
#endif
  return roots;
}

// The chosen emulator folder, else the autodetected ones.
std::vector<fs::path> DolphinRoots() {
  {
    std::lock_guard<std::mutex> lock(sEmulatorMutex);
    if (!sEmulatorFolder.empty()) {
      return {PathFromString(sEmulatorFolder)};
    }
  }
  return AutoDolphinRoots();
}

} // namespace

std::vector<fs::path> DetectedEmulatorFolders() {
  std::vector<fs::path> found;
  std::error_code ec;
  for (const fs::path& root : AutoDolphinRoots()) {
    if (fs::is_directory(root / "GC", ec)) {
      found.push_back(root);
    }
  }
  return found;
}

fs::path NormalizeEmulatorFolder(const fs::path& picked) {
  fs::path path = picked;
  if (!path.has_filename()) {
    path = path.parent_path(); // a trailing slash
  }
  auto up = [&](int levels) {
    for (int i = 0; i < levels; ++i) {
      path = path.parent_path();
    }
  };
  const std::string name = PathString(path.filename());
  if (name == "Card A" && path.parent_path().parent_path().filename() == "GC") {
    up(3);
  } else if ((name == "USA" || name == "EUR" || name == "JAP") &&
             path.parent_path().filename() == "GC") {
    up(2);
  } else if (name == "GC") {
    up(1);
  }
  return path;
}

void SetEmulatorFolder(const std::string& path) {
  const std::string normal = path.empty() ? path : PathString(NormalizeEmulatorFolder(PathFromString(path)));
  std::lock_guard<std::mutex> lock(sEmulatorMutex);
  sEmulatorFolder = normal;
}

std::string EmulatorFolder() {
  std::lock_guard<std::mutex> lock(sEmulatorMutex);
  return sEmulatorFolder;
}

void SetShareEmulatorCard(bool share) {
#if defined(__ANDROID__)
  share = false; // no emulator to share with
#endif
  std::lock_guard<std::mutex> lock(sEmulatorMutex);
  sShareCard = share;
}

bool ShareEmulatorCard() {
  std::lock_guard<std::mutex> lock(sEmulatorMutex);
  return sShareCard;
}

fs::path SharedCardBase(std::string* why) {
  if (!ShareEmulatorCard()) {
    if (why != nullptr) {
      *why = "Sharing is off.";
    }
    return {};
  }
  std::error_code ec;
  const std::vector<fs::path> roots = DolphinRoots();
  for (const fs::path& root : roots) {
    if (fs::is_directory(root / "GC" / CardRegion() / "Card A", ec)) {
      return root / "GC";
    }
  }
  if (why != nullptr) {
    *why = roots.empty()
               ? "No Dolphin or PrimeHack folder found: choose it above."
               : std::string("The emulator folder has no GCI-folder card for this region (") +
                     CardRegion() +
                     "): set its Slot A to \"GCI Folder\" and start the game once there.";
  }
  return {};
}

std::string CardTargetDirectory(const std::string& apDirectory) {
  if (!apDirectory.empty()) {
    return apDirectory;
  }
  return PathString(SharedCardBase());
}

DolphinCard FindDolphinCard(const char* region) {
  DolphinCard card;
  if (region == nullptr) {
    region = CardRegion();
  }
  std::error_code ec;
  for (const fs::path& root : DolphinRoots()) {
    const fs::path folder = root / "GC" / region / "Card A";
    const fs::path raw = root / "GC" / (std::string("MemoryCardA.") + region + ".raw");
    const bool hasFolder = fs::is_directory(folder, ec);
    const bool hasRaw = fs::is_regular_file(raw, ec);
    if (hasFolder || hasRaw) {
      card.gciFolder = hasFolder ? folder : fs::path();
      card.rawImage = hasRaw ? raw : fs::path();
      break;
    }
  }
  return card;
}

fs::path FindDolphinTrilogy() {
  // The Trilogy's title ids: R3ME (USA) and R3MP (PAL).
  std::error_code ec;
  for (const fs::path& root : DolphinRoots()) {
    for (const char* title : {"52334d45", "52334d50"}) {
      const fs::path bin = root / "Wii/title/00010000" / title / "data/save.bin";
      if (fs::is_regular_file(bin, ec)) {
        return bin;
      }
    }
  }
  return {};
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

Report MigrateMountedLegacyPal() {
  const fs::path folder = MountedCardFolder();
  if (folder.empty()) {
    return {};
  }
  Report migrated = MigrateLegacyPal(folder);
  if (migrated.copied > 0) {
    MarkCardChanged();
  }
  return migrated;
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
