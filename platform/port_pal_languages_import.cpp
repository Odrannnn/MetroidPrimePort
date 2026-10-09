// The PAL language import: the image, the background run and the files it writes.

#include "port_pal_languages.h"

#include "port_disc.h"
#include "port_gci.h"
#include "port_paths.h"

#include <aurora/dvd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace PortPalLanguages {
namespace {

std::mutex sMutex;
State sState;
// Imported tables, or -1 before the folder was counted.
int sInstalled = -1;

constexpr uint32_t kSTRG = 0x53545247;  // 'STRG'
constexpr uint32_t kFONT = 0x464F4E54;  // 'FONT'
constexpr uint32_t kTXTR = 0x54585452;  // 'TXTR'

fs::path Folder() { return PortGci::PathFromString(PortPaths::UserFolder()) / "languages"; }

void SetMessage(const std::string& message) {
  std::lock_guard lock(sMutex);
  sState.message = message;
}

int CountTables() {
  int count = 0;
  std::error_code ec;
  for (fs::directory_iterator it(Folder(), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = PortGci::PathString(it->path().filename());
    count += name.size() == 13 && name.compare(8, 5, ".lang") == 0;
  }
  return count;
}

bool EndsWithPak(const char* name) {
  const size_t length = std::strlen(name);
  if (length < 4) {
    return false;
  }
  const char* ext = name + length - 4;
  return ext[0] == '.' && (ext[1] | 0x20) == 'p' && (ext[2] | 0x20) == 'a' && (ext[3] | 0x20) == 'k';
}

// Adds what `want` takes of the image's PAK `pak` (index, name) to `found`.
bool ReadPak(AuroraDiscImage* image, const std::pair<u32, std::string>& pak,
             const std::function<bool(uint32_t, uint32_t)>& want, std::map<ResourceKey, std::vector<uint8_t>>& found,
             std::string& error) {
  void* file = aurora_disc_image_file_open(image, pak.first);
  if (file == nullptr) {
    error = "Could not open " + pak.second + " on the image.";
    return false;
  }
  const ReadAt read = [file](uint64_t offset, uint8_t* out, size_t size) -> size_t {
    if (aurora_dvd_base_seek(file, int64_t(offset), SEEK_SET) != int64_t(offset)) {
      return 0;
    }
    size_t done = 0;
    while (done < size) {
      const int64_t got = aurora_dvd_base_read(file, out + done, size - done);
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  };
  std::string pakError;
  const bool ok = ReadPakResources(read, want, found, pakError);
  aurora_dvd_base_close(file);
  if (!ok) {
    error = pak.second + ": " + pakError + ".";
  }
  return ok;
}

// The number of tables written into Folder(), or -1 with `error` set.
int Import(const std::string& path, std::string& error) {
  char id[6] = {};
  u8 discNumber = 0;
  u8 version = 0;
  AuroraDiscImage* image = aurora_disc_image_open(path.c_str(), id, &discNumber, &version);
  if (image == nullptr) {
    error = "Could not open the image (not a GameCube disc image, or unreadable).";
    return -1;
  }
  struct Closer {
    AuroraDiscImage* image;
    ~Closer() { aurora_disc_image_close(image); }
  } closer{image};
  const PortDisc::Version found = PortDisc::Identify(id, discNumber, version);
  if (found != PortDisc::Version::Pal) {
    error = "This is not the PAL Metroid Prime (GM8P01): it is " +
            (found == PortDisc::Version::Unknown ? std::string(id, 6) : std::string(PortDisc::Name(found))) + ".";
    return -1;
  }

  std::vector<std::pair<u32, std::string>> paks;
  aurora_disc_image_list(
      image,
      [](u32 index, const char* name, u32, void* user) {
        if (EndsWithPak(name)) {
          static_cast<std::vector<std::pair<u32, std::string>>*>(user)->emplace_back(index, name);
        }
      },
      &paks);
  std::map<ResourceKey, std::vector<uint8_t>> resources;
  const auto readPaks = [&](const char* doing, const std::function<bool(uint32_t, uint32_t)>& want) {
    for (size_t i = 0; i < paks.size(); ++i) {
      SetMessage(doing + paks[i].second + " (" + std::to_string(i + 1) + "/" + std::to_string(paks.size()) + ")");
      if (!ReadPak(image, paks[i], want, resources, error)) {
        return false;
      }
    }
    return true;
  };
  // The text and fonts, then only the font textures (TXTRs are most of a PAK).
  if (!readPaks("Reading ", [](uint32_t type, uint32_t) { return type == kSTRG || type == kFONT; })) {
    return -1;
  }
  std::set<uint32_t> textures;
  for (const auto& [key, data] : resources) {
    uint32_t texture = 0;
    if (key.first == kFONT && FontTexture(data, texture)) {
      textures.insert(texture);
    }
  }
  if (!textures.empty() &&
      !readPaks("Reading the fonts of ",
                [&](uint32_t type, uint32_t id) { return type == kTXTR && textures.count(id) != 0; })) {
    return -1;
  }
  std::map<uint32_t, std::vector<uint8_t>> tables;
  for (auto& [key, data] : resources) {
    if (key.first == kSTRG) {
      tables[key.second] = std::move(data);
    }
  }
  if (tables.empty()) {
    error = "No text resources on the image.";
    return -1;
  }

  // Written beside the folder and renamed into place, so a run cut short
  // leaves the previous import whole.
  SetMessage("Writing " + std::to_string(tables.size()) + " tables");
  const fs::path folder = Folder();
  const fs::path staging = folder.parent_path() / ".languages.importing";
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec) {
    error = "Could not create " + PortGci::PathString(staging) + ".";
    return -1;
  }
  const auto write = [&](const char* name, const std::vector<uint8_t>& data) {
    std::ofstream out(staging / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) {
      error = "Could not write " + PortGci::PathString(staging / name) + ".";
      fs::remove_all(staging, ec);
      return false;
    }
    return true;
  };
  int written = 0;
  for (const auto& [tableId, data] : tables) {
    if (!IsStringTable(data)) {
      continue;
    }
    char name[16];
    std::snprintf(name, sizeof(name), "%08X.lang", tableId);
    if (!write(name, data)) {
      return -1;
    }
    ++written;
  }
  for (const auto& [key, data] : resources) {
    if (key.first == kFONT || key.first == kTXTR) {
      char name[16];
      std::snprintf(name, sizeof(name), key.first == kFONT ? "%08X.font" : "%08X.txtr", key.second);
      if (!write(name, data)) {
        return -1;
      }
    }
  }
  fs::remove_all(folder, ec);
  fs::rename(staging, folder, ec);
  if (ec) {
    error = "Could not move the tables into " + PortGci::PathString(folder) + ".";
    return -1;
  }
  return written;
}

void Finish(bool ok, int tables, const std::string& message) {
  std::lock_guard lock(sMutex);
  sState.running = false;
  sState.finished = true;
  sState.ok = ok;
  sState.tables = tables;
  sState.message = message;
  sInstalled = -1;
}

std::string DoneMessage(int tables) {
  return "Added the PAL disc's languages (" + std::to_string(tables) +
         " text tables). Restart the game to see them in every menu.";
}

}  // namespace

bool Start(const std::string& path) {
  std::lock_guard lock(sMutex);
  if (sState.running) {
    return false;
  }
  sState = {};
  sState.running = true;
  sState.message = "Opening the image";
  // Detached: a static joinable thread would call std::terminate at exit.
  std::thread([path] {
    std::string error;
    const int tables = Import(path, error);
    Finish(tables > 0, tables, tables > 0 ? DoneMessage(tables) : error);
  }).detach();
  return true;
}

State Status() {
  std::lock_guard lock(sMutex);
  return sState;
}

int Installed() {
  std::lock_guard lock(sMutex);
  if (sInstalled < 0) {
    sInstalled = CountTables();
  }
  return sInstalled;
}

namespace {

bool ReadFile(const char* format, uint32_t id, std::vector<uint8_t>& out) {
  char name[16];
  std::snprintf(name, sizeof(name), format, id);
  std::ifstream in(Folder() / name, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

}  // namespace

bool ReadTable(uint32_t id, std::vector<uint8_t>& out) { return ReadFile("%08X.lang", id, out); }

bool ReadFont(uint32_t id, std::vector<uint8_t>& font, std::vector<uint8_t>& texture) {
  uint32_t textureId = 0;
  return ReadFile("%08X.font", id, font) && FontTexture(font, textureId) && ReadFile("%08X.txtr", textureId, texture);
}

bool Remove() {
  std::lock_guard lock(sMutex);
  if (sState.running) {
    return false;
  }
  std::error_code ec;
  fs::remove_all(Folder(), ec);
  sInstalled = -1;
  return !ec;
}

int RunFromCommandLine(const std::string& path) {
  std::string error;
  const int tables = Import(path, error);
  if (tables <= 0) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("Added the PAL disc's languages: %d text tables in %s\n", tables, PortGci::PathString(Folder()).c_str());
  return 0;
}

}  // namespace PortPalLanguages
