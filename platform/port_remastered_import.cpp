#include "port_env.h"
#include "port_remastered_import.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <aurora/dvd.h>
#include <aurora/gfx.h>

#include "port_build_info.h"
#include "port_disc.h"
#include "port_gallery.h"
#include "port_map_icons.h"
#include "port_model_variant.h"
#include "port_mods.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_effect_import.h"
#include "port_remastered_font.h"
#include "port_remastered_image.h"
#include "port_remastered_hud.h"
#include "port_remastered_map.h"
#include "port_remastered_movie.h"
#include "port_remastered_nsp.h"
#include "port_remastered_pak.h"
#include "port_remastered_report.h"
#include "port_remastered_room.h"
#include "port_remastered_table.h"
#include "port_remastered_text.h"
#include "port_remastered_txtr.h"
#include "port_room_geo.h"
#include "port_ws.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

namespace PortRemastered {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kSMDL = 0x534D444C;
constexpr uint32_t kWMDL = 0x574D444C;  // a liquid's surface
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kMLVL = 0x4D4C564C;
constexpr uint32_t kMREA = 0x4D524541;
constexpr uint32_t kSTRG = 0x53545247;
constexpr uint32_t kMSBT = 0x4D534254;
constexpr uint32_t kFONT = 0x464F4E54;
constexpr uint32_t kGUIF = 0x47554946;
constexpr uint32_t kLDTA = 0x4C445441;
constexpr uint32_t kCMAP = 0x434D4150;
constexpr uint32_t kMAPA = 0x4D415041;
constexpr uint32_t kMAPW = 0x4D415057;
constexpr uint32_t kFRME = 0x46524D45;
constexpr uint32_t kFMV0 = 0x464D5630;
constexpr uint32_t kGENP = 0x47454E50;  // a particle effect
constexpr uint32_t kSWSH = 0x53575348;  // a standalone swoosh effect
constexpr uint32_t kMATI = 0x4D415449;  // a material instance
constexpr uint32_t kWPSM = 0x5750534D;  // a projectile weapon (Remastered's WPSC)
constexpr uint32_t kWPSC = 0x57505343;  // the disc's projectile weapon

constexpr const char* kStagingName = ".remastered-models.importing";
// Written last, so a staging folder without it is an import that was cut short.
constexpr const char* kMarkerName = "import-complete";
constexpr const char* kRoomFolder = "roomenv";
constexpr const char* kGeometryFolder = "roomgeo";
constexpr const char* kTextFolder = "text";
constexpr const char* kFontFolder = "font";
constexpr const char* kFontName = "deface.sdfont";
constexpr const char* kHudFolder = "hud";
constexpr const char* kMapFolder = "map";
// The disc's own folder: a mod's file there is opened in place of the disc's.
constexpr const char* kMovieFolder = "Video";
constexpr const char* kGalleryFolder = "gallery";
// Largest edge of a room geometry texture: there are thousands of them.
constexpr int kGeometryTexture = 1024;
// A room model's coarser level of detail is written only when it has at most this share of
// the triangles of the level before it that was: one barely coarser costs a file and a
// switch for nothing.
constexpr double kLodShare = 0.75;

// The coarser levels of a room model worth converting, with the distance squared each
// starts at (Remastered's own rules).
std::vector<std::pair<int, float>> CoarserLevels(const Model& model) {
  std::vector<std::pair<int, float>> out;
  const size_t levels = std::min(model.lods.size() / 5, size_t(PortRoomGeo::kLodLevels));
  if (levels < 2 || model.lodRules.size() < levels) {
    return out;
  }
  auto triangles = [&](size_t level) {
    std::vector<bool> seen(model.meshes.size(), false);
    size_t count = 0;
    for (size_t r = level * 5; r < level * 5 + 5; ++r) {
      const ModelLod& range = model.lods[r];
      for (uint64_t i = range.indexOffset;
           i < uint64_t(range.indexOffset) + range.indexCount && i < model.lodMeshes.size(); ++i) {
        const uint16_t mesh = model.lodMeshes[i];
        if (mesh < seen.size() && !seen[mesh]) {
          seen[mesh] = true;
          count += model.meshes[mesh].indices.size() / 3;
        }
      }
    }
    return count;
  };
  double kept = double(triangles(0));
  float previous = 0.f;
  for (size_t level = 1; level < levels; ++level) {
    const float rule = model.lodRules[level];
    const size_t count = triangles(level);
    if (!std::isfinite(rule) || rule <= previous || count == 0) {
      break;
    }
    previous = rule;
    if (double(count) <= kLodShare * kept) {
      out.emplace_back(int(level), rule);
      kept = double(count);
    }
  }
  return out;
}

// The rooms whose geometry is imported, from MP_REMASTERED_GEOMETRY: "all", or
// room names (any part of one) separated by commas, or "none". Without it,
// what SetImportGeometry() last said: none, the port's drawing of room
// geometry being unfinished.
std::atomic<bool> sGeometry{false};

bool WantsGeometry(const std::string& room) {
  const char* env = std::getenv("MP_REMASTERED_GEOMETRY");
  if (env == nullptr || env[0] == '\0') {
    return sGeometry.load();
  }
  const std::string list = env;
  if (list == "none") {
    return false;
  }
  if (list == "all") {
    return true;
  }
  for (size_t at = 0; at <= list.size();) {
    const size_t comma = std::min(list.find(',', at), list.size());
    if (comma > at && room.find(list.substr(at, comma - at)) != std::string::npos) {
      return true;
    }
    at = comma + 1;
  }
  return false;
}

// The import runs beside the game on nearly every core: its threads only take
// the time the game leaves, or the game stutters for as long as it runs.
void YieldToGame() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 19);
#endif
}

std::mutex sStateMutex;
ImportState sState;
std::thread sThread;
std::atomic<bool> sCancel{false};

fs::path PathFromString(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

void SetMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.message = message;
}

// The lines of the stage being made, kept in the manifest so that a re-import that reuses the
// stage shows them again.
std::vector<std::string>* sStageLines = nullptr;

void AddLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  if (sStageLines != nullptr) {
    sStageLines->push_back(line);
  }
  sState.lines.push_back(line);
  if (sState.lines.size() > kImportMaxLines) {
    sState.lines.erase(sState.lines.begin(), sState.lines.end() - kImportMaxLines);
  }
}

void Finish(bool ok, const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.running = false;
  sState.finished = true;
  sState.ok = ok;
  sState.cancelled = !ok && sCancel.load();
  sState.message = message;
}

// MP_REMASTERED_TEXT=0 leaves the disc's wording alone.
bool WantsText() {
  return port::EnvFlag("MP_REMASTERED_TEXT", true);
}

// MP_REMASTERED_HUD=0 leaves the disc's HUD alone.
bool WantsHud() {
  return port::EnvFlag("MP_REMASTERED_HUD", true);
}

// MP_REMASTERED_GALLERY=0 leaves the Extras gallery out.
bool WantsGallery() {
  return port::EnvFlag("MP_REMASTERED_GALLERY", true);
}

// MP_REMASTERED_MOVIES=0 leaves the disc's movies alone; a size and rate
// ("1280x720@30") is what they are written as.
bool WantsMovies(MovieFormat& format) {
  const char* env = std::getenv("MP_REMASTERED_MOVIES");
  if (env == nullptr || env[0] == '\0' || std::strcmp(env, "1") == 0) {
    return true;
  }
  if (std::strcmp(env, "0") == 0) {
    return false;
  }
  if (!ParseMovieFormat(env, format)) {
    AddLine(std::string("MP_REMASTERED_MOVIES: \"") + env + "\" is not a size and rate like 1280x720@30");
  }
  return true;
}

// --- Reusing the previous import ----------------------------------------------

// Off with MP_REMASTERED_REUSE=0 or the import panel's "Reconvert everything". With
// MP_REMASTERED_REUSE=textures only the converted textures are reused (TextureMemory).
std::atomic<bool> sReuse{true};

bool WantsReuse() {
  return sReuse.load() && port::EnvFlag("MP_REMASTERED_REUSE", true);
}

// What a stage of an import made, written to kManifestName in the mod: a re-import whose stage
// has the same key (and, for a stage that gives out ids, the same ids taken before it) links
// these files in, takes the ids and shows the lines again instead of making it anew.
struct StageRecord {
  std::string key;
  std::string before;  // TakenHash() before the stage, empty for a stage that gives out no ids
  std::map<std::string, long long> counts;
  std::vector<std::string> lines;
  std::vector<uint32_t> ids;       // the ids it took
  std::vector<std::string> files;  // relative to the mod, '/'-separated
  std::vector<std::string> extra;  // what the stage hands on to the next ones (the rooms' models)
};
using Manifest = std::map<std::string, StageRecord>;
constexpr const char* kManifestName = ".import-manifest";
constexpr const char* kManifestHeader = "remastered import manifest 1";

uint64_t Hash64(const std::string& text) {
  uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a
  for (const char c : text) {
    hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
  }
  return hash;
}

std::string Hex64(uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));
  return text;
}

std::string UuidText(const ModelUuid& uuid) {
  std::string text;
  for (const uint8_t byte : uuid) {
    char pair[4];
    std::snprintf(pair, sizeof(pair), "%02X", byte);
    text += pair;
  }
  return text;
}

// The same for the same set of ids, in any order.
std::string TakenHash(const std::unordered_set<uint32_t>& taken) {
  uint64_t sum = 0;
  uint64_t mixed = 0;
  for (const uint32_t id : taken) {
    uint64_t z = id + 0x9E3779B97F4A7C15ull;  // splitmix64
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    sum += z;
    mixed ^= z * 0x2545F4914F6CDD1Dull;
  }
  return Hex64(sum) + Hex64(mixed) + std::to_string(taken.size());
}

// Everything in the model table, so that an edit to it re-runs the models without a bump.
std::string TableKey() {
  size_t count = 0;
  const TableEntry* table = Table(count);
  std::string text;
  char buffer[160];
  for (size_t i = 0; i < count; ++i) {
    const TableEntry& entry = table[i];
    std::snprintf(buffer, sizeof(buffer), "%08X %08X %d %d %d|", entry.retail, entry.ancs, int(entry.key),
                  int(entry.pbr), int(entry.options));
    text += buffer;
    text.append(reinterpret_cast<const char*>(entry.rem), sizeof(entry.rem));
    text.append(reinterpret_cast<const char*>(entry.orient), sizeof(entry.orient));
    for (const double offset : entry.offset) {
      std::snprintf(buffer, sizeof(buffer), "%a ", offset);
      text += buffer;
    }
    const uint32_t* skins = TableSkins(entry);
    for (int s = 0; s < entry.skinCount; ++s) {
      std::snprintf(buffer, sizeof(buffer), "%08X ", skins[s]);
      text += buffer;
    }
    if (const TableOptions* extra = TableExtra(entry)) {
      std::snprintf(buffer, sizeof(buffer), "%d %d %s %a %a %a %a", extra->material, extra->maxTexture,
                    extra->squeezeRole != nullptr ? extra->squeezeRole : "-", extra->squeeze[0], extra->squeeze[1],
                    extra->squeeze[2], extra->squeeze[3]);
      text += buffer;
    }
    text += '\n';
  }
  return Hex64(Hash64(text));
}

bool ReadManifest(const fs::path& path, Manifest& out) {
  std::ifstream file(path, std::ios::binary);
  std::string line;
  if (!std::getline(file, line) || line != kManifestHeader) {
    return false;
  }
  StageRecord* stage = nullptr;
  while (std::getline(file, line)) {
    const size_t space = line.find(' ');
    const std::string tag = line.substr(0, space);
    const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
    if (tag == "end") {
      return true;  // only a manifest written to the end is trusted
    }
    if (tag == "stage") {
      stage = &out[rest];
      continue;
    }
    if (stage == nullptr) {
      return false;
    }
    if (tag == "key") {
      stage->key = rest;
    } else if (tag == "before") {
      stage->before = rest;
    } else if (tag == "count") {
      const size_t at = rest.find(' ');
      stage->counts[rest.substr(0, at)] = at == std::string::npos ? 0 : std::atoll(rest.c_str() + at + 1);
    } else if (tag == "line") {
      stage->lines.push_back(rest);
    } else if (tag == "ids") {
      for (const char* p = rest.c_str(); *p != '\0';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 16);
        if (end == p) {
          break;
        }
        stage->ids.push_back(uint32_t(id));
        p = end;
      }
    } else if (tag == "file") {
      stage->files.push_back(rest);
    } else if (tag == "extra") {
      stage->extra.push_back(rest);
    }
  }
  return false;
}

bool WriteManifest(const fs::path& path, const Manifest& manifest) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << kManifestHeader << '\n';
  for (const auto& [name, stage] : manifest) {
    file << "stage " << name << "\nkey " << stage.key << '\n';
    if (!stage.before.empty()) {
      file << "before " << stage.before << '\n';
    }
    for (const auto& [counter, value] : stage.counts) {
      file << "count " << counter << ' ' << value << '\n';
    }
    for (const std::string& line : stage.lines) {
      // A line is one line of the log; a newline in one would end it early.
      std::string flat = line;
      std::replace(flat.begin(), flat.end(), '\n', ' ');
      file << "line " << flat << '\n';
    }
    for (size_t i = 0; i < stage.ids.size(); i += 16) {
      file << "ids";
      for (size_t j = i; j < std::min(i + 16, stage.ids.size()); ++j) {
        char id[12];
        std::snprintf(id, sizeof(id), " %08X", stage.ids[j]);
        file << id;
      }
      file << '\n';
    }
    for (const std::string& name : stage.files) {
      file << "file " << name << '\n';
    }
    for (const std::string& line : stage.extra) {
      file << "extra " << line << '\n';
    }
  }
  file << "end\n";
  file.close();
  return bool(file);
}

// Hard-links (or, where the file system has none, copies) a stage's files from `source` into
// `target`, leaving any `target` already has. False, with nothing linked, when one is missing,
// lies outside the folder, or cannot be linked.
bool LinkFiles(const fs::path& source, const fs::path& target, const std::vector<std::string>& files) {
  std::error_code ec;
  for (const std::string& name : files) {
    const fs::path relative = PathFromString(name).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_path() || *relative.begin() == "..") {
      return false;
    }
    if (!fs::is_regular_file(source / relative, ec)) {
      return false;
    }
  }
  std::vector<fs::path> linked;
  for (const std::string& name : files) {
    const fs::path from = source / PathFromString(name);
    const fs::path to = target / PathFromString(name);
    if (fs::exists(to, ec)) {
      continue;
    }
    fs::create_directories(to.parent_path(), ec);
    fs::create_hard_link(from, to, ec);
    if (ec) {
      fs::copy_file(from, to, ec);
    }
    if (ec) {
      AddLine("cannot reuse " + name + ": " + ec.message());
      for (const fs::path& path : linked) {
        fs::remove(path, ec);
      }
      return false;
    }
    linked.push_back(to);
  }
  return true;
}

// What the converters remember across imports (ConvertIO::recall): each converted texture's id
// and files, and facts about Remastered textures. Kept in the manifest as stage kTextureStage,
// one "extra" line an entry: key, value and files, tab-separated. The key starts with the folder
// the converter writes to, relative to the mod; the files are relative to the mod.
constexpr const char* kTextureStage = "textures";

class TextureMemory {
public:
  struct Entry {
    std::string value;
    std::vector<std::string> files;
  };

  void Load(const StageRecord& record) {
    for (const std::string& line : record.extra) {
      std::vector<std::string> fields;
      for (size_t at = 0;;) {
        const size_t tab = line.find('\t', at);
        fields.push_back(line.substr(at, tab - at));
        if (tab == std::string::npos) {
          break;
        }
        at = tab + 1;
      }
      if (fields.size() >= 2) {
        Entry& entry = entries_[fields[0]];
        entry.value = fields[1];
        entry.files.assign(fields.begin() + 2, fields.end());
      }
    }
  }

  bool Find(const std::string& key, Entry& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      return false;
    }
    out = it->second;
    return true;
  }

  void Put(const std::string& key, Entry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(entry);
  }

  // The entries whose files are all in `mod`, for the next import.
  void Save(const fs::path& mod, StageRecord& record) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    for (const auto& [key, entry] : entries_) {
      std::string line = key + '\t' + entry.value;
      bool there = true;
      for (const std::string& file : entry.files) {
        there = there && fs::is_regular_file(mod / PathFromString(file), ec);
        line += '\t' + file;
      }
      if (there) {
        record.extra.push_back(std::move(line));
      }
    }
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
};

// Writes `data` beside `path` and renames it into place: a file may be a hard link into the
// previous import, which must not change under it.
bool WriteReplacing(const fs::path& path, const std::vector<uint8_t>& data) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    file.close();
    if (!file) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
  }
  return !ec;
}

// --- The retail disc ----------------------------------------------------------

// The CMDL, CSKR, TXTR, MLVL, MREA, STRG, FRME, MAPA and MAPW resources of the unmodded disc, and every id on it.
class Retail {
public:
  ~Retail() {
    for (auto& [entry, handle] : m_handles) {
      aurora_dvd_base_close(handle);
    }
  }

  bool Index(std::string& error) {
    const int32_t baseCount = aurora_dvd_base_entry_count();
    for (const auto& [entry, path] : PortMods::DiscPaks()) {
      if (entry < 0 || entry >= baseCount) {
        continue;  // a PAK only a mod brings
      }
      void* handle = aurora_dvd_base_open(entry);
      if (handle == nullptr) {
        continue;
      }
      m_handles[entry] = handle;
      std::vector<uint8_t> header;
      PortMods::PakTable table;
      size_t needed = 0x10000;
      bool parsed = false;
      while (needed <= (64u << 20)) {
        header.resize(needed);
        const size_t got = ReadAt(handle, 0, header.data(), header.size());
        if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
          parsed = true;
          break;
        }
        if (got < header.size() || needed <= header.size()) {
          break;
        }
      }
      if (!parsed) {
        continue;
      }
      // The named resources (a pak's first section): the projectile weapons' names pair them
      // with Remastered's WPSMs. Kept out of m_resources, which the fingerprint covers.
      if (header.size() >= 12) {
        const auto be32 = [&header](size_t at) {
          return uint32_t(header[at]) << 24 | uint32_t(header[at + 1]) << 16 | uint32_t(header[at + 2]) << 8 |
                 uint32_t(header[at + 3]);
        };
        size_t at = 12;
        for (uint32_t n = be32(8); n > 0 && at + 12 <= header.size(); --n) {
          const uint32_t length = be32(at + 8);
          if (at + 12 + length > header.size()) {
            break;
          }
          if (be32(at) == kWPSC) {
            std::string name(reinterpret_cast<const char*>(header.data() + at + 12), length);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            m_wpscNames.emplace(std::move(name), be32(at + 4));
          }
          at += 12 + length;
        }
      }
      for (const PortMods::PakResource& res : table.resources) {
        m_ids.insert(res.id);
        if (res.type == kWPSC) {
          m_wpsc.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
        }
        if (res.type == kCMDL || res.type == kCSKR || res.type == kANCS || res.type == kTXTR || res.type == kMLVL ||
            res.type == kMREA || res.type == kSTRG || res.type == kFRME || res.type == kMAPA || res.type == kMAPW) {
          m_resources.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
        }
      }
    }
    if (m_resources.empty()) {
      error = "no models found on the disc";
      return false;
    }
    return true;
  }

  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }

  // The ids of every resource of `type` it reads, in no particular order.
  std::vector<uint32_t> Ids(uint32_t type) const {
    std::vector<uint32_t> ids;
    for (const auto& [key, where] : m_resources) {
      if (uint32_t(key >> 32) == type) {
        ids.push_back(uint32_t(key));
      }
    }
    return ids;
  }

  // Which disc this is, as far as the import reads it: every resource's place and size, in
  // any order, and the ids of the rest.
  uint64_t Fingerprint() const {
    uint64_t sum = m_resources.size() * 0x9E3779B97F4A7C15ull + m_ids.size();
    for (const auto& [key, where] : m_resources) {
      const std::string text = std::to_string(key) + " " + std::to_string(where.entry) + " " +
                               std::to_string(where.offset) + " " + std::to_string(where.size) +
                               (where.compressed ? " z" : "");
      sum += Hash64(text);
    }
    for (const uint32_t id : m_ids) {
      sum ^= Hash64(std::to_string(id)) * 3;
    }
    return sum;
  }

  // The id of the disc's projectile weapon (WPSC) of this name.
  bool WpscId(const std::string& name, uint32_t& id) const {
    const auto found = m_wpscNames.find(name);
    if (found == m_wpscNames.end()) {
      return false;
    }
    id = found->second;
    return true;
  }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    auto found = m_resources.find(Key(type, id));
    if (found == m_resources.end()) {
      found = m_wpsc.find(Key(type, id));
      if (found == m_wpsc.end()) {
        return false;
      }
    }
    const Where& where = found->second;
    if (!where.compressed) {
      std::vector<uint8_t> raw(where.size);
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
          return false;
        }
      }
      out = std::move(raw);
      return true;
    }
    // Read straight into the string the inflater takes, so the blob is not copied into one.
    std::string raw(where.size, '\0');
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (ReadAt(m_handles[where.entry], where.offset, reinterpret_cast<uint8_t*>(raw.data()), raw.size()) !=
          raw.size()) {
        return false;
      }
    }
    // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
    // data, and a checksum the decoder never reaches.
    if (raw.size() < 6) {
      return false;
    }
    const auto byte = [&raw](size_t i) { return size_t(static_cast<uint8_t>(raw[i])); };
    const size_t length = byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
    raw.erase(0, 6);
    PortWs::Inflater inflater;
    inflater.SetKeepWindow(false);
    std::string inflated;
    if (!inflater.InflateMessage(raw, inflated, length) || inflated.size() != length) {
      return false;
    }
    out.assign(inflated.begin(), inflated.end());
    return true;
  }

private:
  struct Where {
    int32_t entry;
    uint32_t offset;
    uint32_t size;
    bool compressed;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return uint64_t(type) << 32 | id; }

  static size_t ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
    if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
      return 0;
    }
    size_t done = 0;
    while (done < size) {
      const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  }

  std::mutex m_mutex;
  std::map<int32_t, void*> m_handles;
  std::unordered_map<uint64_t, Where> m_resources;
  std::unordered_set<uint32_t> m_ids;
  std::unordered_map<uint64_t, Where> m_wpsc;                 // the projectile weapons, read on demand
  std::unordered_map<std::string, uint32_t> m_wpscNames;      // WPSC id by name
};

// --- The Remastered image ------------------------------------------------------

// Every model and texture in the image's paks, by id.
class Remastered {
public:
  bool Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
    if (!m_nsp.Open(nspPath, keysPath, error)) {
      return false;
    }
    std::vector<const RomfsFile*> files;
    for (const RomfsFile& file : m_nsp.Files()) {
      if (file.path.size() > 4 && file.path.compare(file.path.size() - 4, 4, ".pak") == 0) {
        files.push_back(&file);
      }
    }
    for (size_t i = 0; i < files.size(); ++i) {
      if (sCancel) {
        error = "cancelled";
        return false;
      }
      SetMessage("Reading the paks (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")");
      const RomfsFile* file = files[i];
      auto pak = std::make_unique<Pak>();
      std::string pakError;
      // Nsp::Read is safe from the workers side by side.
      const ReadFn read = [this, file](uint64_t offset, void* out, size_t size) {
        std::string ignored;
        return m_nsp.Read(*file, offset, out, size, ignored);
      };
      if (!pak->Open(read, file->size, pakError)) {
        AddLine(file->path + ": " + pakError);
        continue;
      }
      const std::vector<PakAsset>& assets = pak->Assets();
      for (size_t a = 0; a < assets.size(); ++a) {
        const uint32_t type = assets[a].type;
        if (type == kCMDL || type == kSMDL || type == kWMDL) {
          m_models.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_modelNames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kTXTR) {
          m_textures.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_textureNames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kMSBT) {
          m_texts.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kFONT) {
          m_fonts.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kFMV0) {
          m_movies.emplace(IdToString(assets[a].id), Where{m_paks.size(), a});
        } else if (type == kGENP) {
          m_effects.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_effectNames.emplace(FrameKey(name), assets[a].id);
          }
        } else if (type == kSWSH) {
          m_swooshes.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_effectNames.emplace(FrameKey(name), assets[a].id);
          }
        } else if (type == kWPSM) {
          for (const std::string& name : assets[a].names) {
            m_projectiles.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kMATI) {
          m_materials.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kGUIF) {
          for (const std::string& name : assets[a].names) {
            m_frames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kCMAP) {
          for (const std::string& name : assets[a].names) {
            m_maps.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kLDTA) {
          for (const std::string& name : assets[a].names) {
            m_tweaks.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        }
      }
      m_paks.push_back(std::move(pak));
      m_paths.push_back(file->path);
    }
    if (m_models.empty() && m_movies.empty()) {
      error = "no models in this image; is it Metroid Prime Remastered?";
      return false;
    }
    return true;
  }

  bool ReadModel(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_models, id, out, error);
  }
  bool ReadTexture(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_textures, id, out, error);
  }

  // The FONT assets, in id order. There are several, with different sets of characters.
  std::vector<ModelUuid> Fonts() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_fonts) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  bool ReadFont(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_fonts, id, out, error);
  }

  // A GUI frame by its asset name ("FRME_CombatHud"), whatever folder and case the pak has it under.
  bool ReadFrame(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_frames.find(FrameKey(name));
    if (found == m_frames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A tweak file by its asset name ("TweakGuiColorsMP1"), as ReadFrame finds a frame.
  bool ReadTweak(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_tweaks.find(FrameKey(name));
    if (found == m_tweaks.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A texture by its asset name ("TXTR_IconS"), as ReadFrame finds a frame.
  bool ReadTextureNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_textureNames.find(FrameKey(name));
    if (found == m_textureNames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // The Extras gallery's pictures, in the pak's order: the textures of UI_FrontEnd at least 1000 texels high (the
  // concept art; the menu backdrops are 1600x900). `visit` gets each one's position and its TXTR file, and says
  // whether to go on.
  template <class Visit>
  void ForEachGalleryTexture(Visit&& visit) const {
    size_t position = 0;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      if (path.substr(path.rfind('/') + 1) != "UI_FrontEnd.pak") {
        continue;
      }
      const Pak& pak = *m_paks[i];
      for (const PakAsset& asset : pak.Assets()) {
        std::vector<uint8_t> raw;
        std::string error;
        TxtrImage info;
        if (asset.type != kTXTR || !pak.ReadAsset(asset, raw, error) ||
            !ReadTxtrInfo(raw.data(), raw.size(), info, error) || info.height < 1000) {
          continue;
        }
        if (!visit(position++, raw)) {
          return;
        }
      }
    }
  }

  // A model by its asset name ("CMDL_MapCompass"), as ReadFrame finds a frame.
  bool ReadModelNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_modelNames.find(FrameKey(name));
    if (found == m_modelNames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A world's map by its asset name ("CMAP_IceLevel"), as ReadFrame finds a frame.
  bool ReadMap(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_maps.find(FrameKey(name));
    if (found == m_maps.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A movie by its id as IdToString prints it.
  bool ReadMovie(const std::string& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_movies.find(id);
    if (found == m_movies.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // Every text asset, each once however many paks carry it.
  std::vector<ModelUuid> Texts() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_texts) {
      ids.push_back(id);
    }
    return ids;
  }
  bool ReadText(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_texts, id, out, error);
  }

  // The particle effects (GENP) and what they read: material instances and textures.
  std::vector<ModelUuid> Effects() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_effects) {
      ids.push_back(id);
    }
    for (const auto& [id, where] : m_swooshes) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  // The projectile weapons (WPSM) by lower-case name, and a GENP/SWSH by its name.
  std::vector<std::string> ProjectileNames() const {
    std::vector<std::string> names;
    for (const auto& [name, where] : m_projectiles) {
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
  }
  bool ReadProjectile(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_projectiles.find(name);
    if (found == m_projectiles.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }
  bool EffectByName(const std::string& name, ModelUuid& id) const {
    const auto found = m_effectNames.find(name);
    if (found == m_effectNames.end()) {
      return false;
    }
    id = found->second;
    return true;
  }
  bool ReadEffectAsset(uint32_t type, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(type == kGENP   ? m_effects
                : type == kSWSH ? m_swooshes
                : type == kMATI ? m_materials
                                : m_textures,
                id, out, error);
  }
  uint32_t EffectAssetType(const ModelUuid& id) const {
    return m_textures.count(id) != 0    ? kTXTR
           : m_materials.count(id) != 0 ? kMATI
           : m_effects.count(id) != 0   ? kGENP
           : m_swooshes.count(id) != 0  ? kSWSH
           : m_models.count(id) != 0    ? kCMDL
                                        : 0;
  }

  // The paks of one world directory ("Intro_Master") as the room writer takes
  // them, and every pak of the image for the assets rooms share.
  void World(const std::string& dir, RoomPak& master, std::vector<RoomPak>& rooms) const {
    const std::string folder = "/!" + dir + "/";
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      const size_t at = path.find(folder);
      if (at == std::string::npos) {
        continue;
      }
      const std::string name = path.substr(at + folder.size(), path.size() - at - folder.size() - 4);
      if (name == "!" + dir) {
        master = RoomPak{dir, m_paks[i].get()};
      } else if (name.find('/') == std::string::npos) {
        rooms.push_back(RoomPak{name, m_paks[i].get()});
      }
    }
  }
  // The environment BRDF table out of the executable. Never throws.
  bool ExtractBrdf(std::vector<uint8_t>& out, std::string& error) const {
    try {
      return ExtractBrdfLut(m_nsp, out, error);
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  std::vector<RoomPak> AllPaks() const {
    std::vector<RoomPak> all;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      all.push_back(RoomPak{m_paths[i], m_paks[i].get()});
    }
    return all;
  }

private:
  struct Where {
    size_t pak;
    size_t asset;
  };
  using Index = std::unordered_map<std::array<uint8_t, 16>, Where, PakIdHash>;

  static std::string FrameKey(const std::string& name) {
    const size_t slash = name.find_last_of("/\\");
    std::string key = name.substr(slash == std::string::npos ? 0 : slash + 1);
    key = key.substr(0, key.find('.'));
    for (char& c : key) {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
  }

  bool Read(const Index& index, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = index.find(id);
    if (found == index.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  Nsp m_nsp;
  std::vector<std::unique_ptr<Pak>> m_paks;
  std::vector<std::string> m_paths;  // of m_paks, in the image
  Index m_models;
  Index m_textures;
  Index m_texts;
  Index m_fonts;
  Index m_effects;
  Index m_swooshes;  // standalone SWSH effects
  Index m_materials;
  std::unordered_map<std::string, Where> m_projectiles;  // WPSM, by FrameKey
  std::unordered_map<std::string, ModelUuid> m_effectNames;  // the named GENP and SWSH, by FrameKey
  std::unordered_map<std::string, Where> m_frames;  // GUIF, by FrameKey
  std::unordered_map<std::string, Where> m_tweaks;  // LDTA, by FrameKey
  std::unordered_map<std::string, Where> m_textureNames;  // the named TXTR, by FrameKey
  std::unordered_map<std::string, Where> m_modelNames;    // the named CMDL, by FrameKey
  std::unordered_map<std::string, Where> m_maps;  // CMAP, by FrameKey
  std::unordered_map<std::string, Where> m_movies;  // FMV0, by IdToString
};

// --- The import ------------------------------------------------------------------

fs::path StagingFolder() {
  const std::string mods = PortMods::Folder();
  return mods.empty() ? fs::path() : PathFromString(mods) / kStagingName;
}

// Where a finished import that is not yet in place waits while the next one reuses it.
fs::path HeldFolder(const fs::path& staging) { return staging.parent_path() / (std::string(kStagingName) + ".held"); }

ConvertOptions OptionsFor(const TableEntry& entry) {
  ConvertOptions options;
  options.retail = entry.retail;
  for (int i = 0; i < 9; ++i) {
    options.orient[i / 3][i % 3] = entry.orient[i];
  }
  for (int i = 0; i < 3; ++i) {
    options.offset[i] = entry.offset[i];
  }
  const uint32_t* skins = TableSkins(entry);
  options.skins.assign(skins, skins + entry.skinCount);
  options.pbr = entry.pbr;
  if (const TableOptions* extra = TableExtra(entry)) {
    options.material = extra->material;
    options.maxTexture = extra->maxTexture;
    if (extra->squeezeRole != nullptr) {
      options.squeeze = true;
      options.squeezeRole = extra->squeezeRole;
      options.squeezeFrom[0] = extra->squeeze[0];
      options.squeezeFrom[1] = extra->squeeze[1];
      options.squeezeTo[0] = extra->squeeze[2];
      options.squeezeTo[1] = extra->squeeze[3];
    }
  }
  return options;
}

// Remastered's menu movies, written into `folder` under the disc's names
// (port_remastered_movie.h). Returns how many of the disc's movies were replaced.
int ImportMovies(const Remastered& remastered, const fs::path& folder, const MovieFormat& format, bool& noFfmpeg) {
  SetMessage("Looking for ffmpeg");
  const std::string ffmpeg = FindFfmpeg();
  noFfmpeg = ffmpeg.empty();
  if (noFfmpeg) {
    AddLine("movies skipped: ffmpeg not found. Install ffmpeg (or put it next to the game), then use \"Import "
            "movies\".");
    return 0;
  }
  std::error_code ec;
  fs::create_directories(folder, ec);
  const auto text = [](const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
  };
  // ffmpeg reads the MP4 from a file: it has to seek in it.
  const fs::path source = folder / "import.tmp.mp4";
  const std::vector<Movie>& movies = Movies();
  int written = 0;
  for (size_t i = 0; i < movies.size() && !sCancel; ++i) {
    const Movie& movie = movies[i];
    SetMessage("Converting the movies (" + std::to_string(i + 1) + "/" + std::to_string(movies.size()) + ")");
    std::vector<uint8_t> raw;
    std::string error;
    size_t offset = 0;
    size_t length = 0;
    int frames = 0;
    const fs::path first = folder / (std::string(movie.names[0]) + ".thp");
    const fs::path tmp = folder / "import.tmp.thp";
    bool ok = remastered.ReadMovie(movie.id, raw, error) &&
              (MovieStream(raw.data(), raw.size(), offset, length) || (error = "not a movie", false));
    if (ok) {
      std::ofstream file(source, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(raw.data() + offset), std::streamsize(length));
      file.close();
      ok = bool(file) || (error = "cannot write to the mod folder", false);
    }
    raw = {};
    ok = ok && ConvertMovie(ffmpeg, text(source), text(tmp), format, [] { return sCancel.load(); }, frames, error);
    if (ok) {
      // Written beside it and renamed, so a movie cut short never has the name.
      fs::rename(tmp, first, ec);
      ok = !ec || (error = "cannot replace the movie: " + ec.message(), false);
    }
    if (!ok) {
      fs::remove(tmp, ec);
      if (!sCancel) {
        AddLine(std::string(movie.names[0]) + ".thp: " + error);
      }
      continue;
    }
    ++written;
    // The disc's other takes of a transition are the same file again.
    for (size_t n = 1; n < movie.names.size(); ++n) {
      const fs::path other = folder / (std::string(movie.names[n]) + ".thp");
      fs::remove(other, ec);
      fs::create_hard_link(first, other, ec);
      if (ec) {
        fs::copy_file(first, other, fs::copy_options::overwrite_existing, ec);
      }
      if (ec) {
        AddLine(std::string(movie.names[n]) + ".thp: " + ec.message());
      } else {
        ++written;
      }
    }
  }
  fs::remove(source, ec);
  if (written == 0) {
    fs::remove(folder, ec); // only if nothing is in it
  }
  return written;
}

// Each worker holds a whole model, its decoded buffers and a few RGBA
// textures at once, so the count is bounded by memory as much as by cores.
int DefaultThreads() {
#if defined(__ANDROID__)
  return std::clamp(int(std::thread::hardware_concurrency()) - 2, 1, 3);
#else
  return std::clamp(int(std::thread::hardware_concurrency()) - 2, 1, 8);
#endif
}

// The Extras gallery's concept art as gallery/NNN.jpg in `folder` (port_gallery.h). Returns how many were written.
int ImportGallery(const Remastered& remastered, const fs::path& target, int threads) {
  std::error_code ec;
  // Written beside the live folder and swapped in only if a picture came out, so a failed or
  // cancelled re-run keeps the old gallery.
  const fs::path folder = target.string() + ".new";
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  std::atomic<int> written{0};
  const auto convert = [&](size_t position, const std::vector<uint8_t>& raw) {
    char name[16];
    std::snprintf(name, sizeof(name), "%03d.jpg", int(position));
    std::string error;
    TxtrImage image;
    std::vector<uint8_t> jpeg;
    bool ok = DecodeTxtr(raw.data(), raw.size(), image, error);
    ok = ok && PortGallery::EncodeGalleryJpeg(image.rgba.data(), int(image.width), int(image.height), jpeg);
    if (ok) {
      std::error_code fileError;
      const fs::path tmp = folder / (std::string(name) + ".tmp");
      {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(jpeg.data()), std::streamsize(jpeg.size()));
        file.close();
        ok = bool(file);
      }
      // Renamed, so a picture cut short never has the name.
      if (ok) {
        fs::rename(tmp, folder / name, fileError);
        ok = !fileError;
      }
      if (!ok) {
        fs::remove(tmp, fileError);
        error = "cannot write to the mod folder";
      }
    } else if (error.empty()) {
      error = "cannot encode";
    }
    if (ok) {
      ++written;
    } else if (!sCancel) {
      AddLine(std::string("gallery/") + name + ": " + error);
    }
  };
  // The pak is read here and the pictures decoded and encoded on `threads` workers; the
  // queue holds a few TXTRs at most, so memory stays bounded by the worker count.
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::pair<size_t, std::vector<uint8_t>>> queue;
  bool done = false;
  std::vector<std::thread> workers;
  for (int i = 0; i < std::max(threads, 1); ++i) {
    workers.emplace_back([&] {
      for (;;) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return done || !queue.empty(); });
        if (queue.empty()) {
          return;
        }
        auto [position, raw] = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        changed.notify_all();
        if (!sCancel) {
          convert(position, raw);
        }
      }
    });
  }
  remastered.ForEachGalleryTexture([&](size_t position, const std::vector<uint8_t>& raw) {
    SetMessage("Gallery picture " + std::to_string(position + 1));
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return queue.size() < workers.size(); });
    queue.emplace_back(position, raw);
    lock.unlock();
    changed.notify_all();
    return !sCancel;
  });
  {
    std::lock_guard<std::mutex> lock(mutex);
    done = true;
  }
  changed.notify_all();
  for (std::thread& worker : workers) {
    worker.join();
  }
  if (written == 0) {
    fs::remove_all(folder, ec);
    return 0;
  }
  fs::remove_all(target, ec);
  fs::rename(folder, target, ec);
  if (ec) {
    std::fprintf(stderr, "gallery: cannot move the new folder into place: %s\n", ec.message().c_str());
    fs::remove_all(folder, ec);
    return 0;
  }
  return written;
}

// Only the movies and the gallery, into the mod an earlier import made: for a player who had
// no ffmpeg at the time, or imported before the gallery.
void RunMovies(std::string nspPath, std::string keysPath, fs::path mod) {
  YieldToGame();
  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    Finish(false, sCancel ? std::string("Cancelled.") : error);
    return;
  }
  // The next import must not reuse a record of movies or pictures this run replaces.
  if (Manifest manifest; ReadManifest(mod / kManifestName, manifest) &&
                         (manifest.erase("movies") + manifest.erase("gallery")) != 0) {
    fs::path tmp = mod / kManifestName;
    tmp += ".tmp";
    std::error_code ec;
    if (!WriteManifest(tmp, manifest) || (fs::rename(tmp, mod / kManifestName, ec), ec)) {
      fs::remove(tmp, ec);
      fs::remove(mod / kManifestName, ec);
    }
  }
  MovieFormat format;
  WantsMovies(format);
  bool noFfmpeg = false;
  const int movies = ImportMovies(remastered, mod / kMovieFolder, format, noFfmpeg);
  const int gallery = WantsGallery() && !sCancel ? ImportGallery(remastered, mod / kGalleryFolder, DefaultThreads()) : 0;
  if (sCancel) {
    Finish(false, "Cancelled.");
  } else if (noFfmpeg && gallery == 0) {
    Finish(false, "ffmpeg not found. Install it, or put it next to the game.");
  } else if (movies == 0 && gallery == 0) {
    Finish(false, "No movies converted.");
  } else {
    std::string message = std::to_string(movies) + " movies converted";
    if (gallery != 0) {
      message += ", " + std::to_string(gallery) + " gallery pictures";
    }
    Finish(true, message + (noFfmpeg ? ". Movies skipped: ffmpeg not found." : "."));
  }
}

// Decoded Remastered textures, shared by the workers. The room models share a
// few thousand textures, and each model's converter used to decode its own
// copy. The least recently used go past the byte budget; one decode per id runs
// at a time, and the other workers asking for it wait.
// The PARTs and SWHCs the projectile weapons name, as the retail ones they replace. A Remastered WPSM
// is the disc's WPSC of the same name (both have the weapons' names: PowerBeam, WaveBall, Missile...);
// the GENP in its APSM/APS2 and the SWSH in its ASW1-3 take the PART/SWHC the WPSC's field of that
// name holds. Remastered's field is stored as <reversed fourcc> 00 <guid>, the disc's as the fourcc,
// CNST and a big-endian id (CProjectileWeaponDataFactory::CreateWPSM); a guid that is zero or no
// asset of the image is not a reference.
EffectGuid ToStored(EffectGuid id) {
  std::swap(id[0], id[3]);
  std::swap(id[1], id[2]);
  std::swap(id[4], id[5]);
  std::swap(id[6], id[7]);
  return id;
}

std::string HexId(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", id);
  return text;
}

std::vector<EffectPairing> ProjectilePairings(const Remastered& remastered, Retail& retail,
                                              std::vector<std::string>& lines) {
  struct Field {
    const char* tag;
    uint32_t type;  // the Remastered asset a reference must be
  };
  static const Field kFields[] = {{"APSM", kGENP}, {"APS2", kGENP}, {"ASW1", kSWSH}, {"ASW2", kSWSH}, {"ASW3", kSWSH}};
  std::vector<EffectPairing> pairings;
  for (const std::string& name : remastered.ProjectileNames()) {
    uint32_t wpsc = 0;
    std::vector<uint8_t> weapon;
    std::vector<uint8_t> projectile;
    std::string ignored;
    if (!retail.WpscId(name, wpsc)) {
      lines.push_back("WPSM " + name + ": no disc WPSC of that name");
      continue;
    }
    if (!retail.Read(kWPSC, wpsc, weapon) || !remastered.ReadProjectile(name, projectile, ignored)) {
      lines.push_back("WPSM " + name + ": could not read it");
      continue;
    }
    for (const Field& field : kFields) {
      // The disc's id: tag, "CNST", id.
      uint32_t discId = 0;
      for (size_t i = 0; i + 12 <= weapon.size(); ++i) {
        if (std::memcmp(weapon.data() + i, field.tag, 4) == 0 && std::memcmp(weapon.data() + i + 4, "CNST", 4) == 0) {
          discId = uint32_t(weapon[i + 8]) << 24 | uint32_t(weapon[i + 9]) << 16 | uint32_t(weapon[i + 10]) << 8 |
                   uint32_t(weapon[i + 11]);
          break;
        }
      }
      // Remastered's: the reversed tag, a zero byte, 16 bytes in an effect's order.
      EffectGuid guid{};
      bool found = false;
      for (size_t i = 0; i + 21 <= projectile.size() && !found; ++i) {
        if (projectile[i] != uint8_t(field.tag[3]) || projectile[i + 1] != uint8_t(field.tag[2]) ||
            projectile[i + 2] != uint8_t(field.tag[1]) || projectile[i + 3] != uint8_t(field.tag[0]) ||
            projectile[i + 4] != 0) {
          continue;
        }
        std::copy_n(projectile.begin() + std::ptrdiff_t(i + 5), 16, guid.begin());
        std::swap(guid[0], guid[3]);  // to a pak's order
        std::swap(guid[1], guid[2]);
        std::swap(guid[4], guid[5]);
        std::swap(guid[6], guid[7]);
        found = remastered.EffectAssetType(guid) == field.type;
      }
      if (!found || discId == 0 || discId == 0xFFFFFFFFu || !retail.HasId(discId)) {
        if (found != (discId != 0 && discId != 0xFFFFFFFFu)) {
          lines.push_back("WPSM " + name + " " + field.tag + (found ? ": the disc has no such field" : ": the WPSM has no such field"));
        }
        continue;
      }
      pairings.push_back({guid, discId, "wpsm " + name + " " + field.tag});
      lines.push_back("WPSM " + name + " " + field.tag + ": " + EffectGuidString(ToStored(guid)) + " -> " + HexId(discId));
    }
  }
  // Effects the disc's PARTs/SWHCs name the same (SamGunFx.pak), that no WPSC reaches.
  struct Named {
    const char* name;
    uint32_t retail;
  };
  static const Named kNamed[] = {{"powerauxmuzzle", 0x2AED975B}, {"bustermuzzle", 0}, {"busterswoosh1", 0x869B8E14},
                                 {"busterswoosh2", 0x804E26D9}};
  for (const Named& named : kNamed) {
    ModelUuid id;
    if (named.retail != 0 && retail.HasId(named.retail) && remastered.EffectByName(named.name, id)) {
      pairings.push_back({id, named.retail, std::string("name ") + named.name});
      lines.push_back(std::string("name ") + named.name + ": " + EffectGuidString(ToStored(id)) + " -> " + HexId(named.retail));
    }
  }
  return pairings;
}

class TextureCache {
public:
  explicit TextureCache(size_t budget) : m_budget(budget) {}

  bool Get(const ModelUuid& id, Image& out, std::string& error,
           const std::function<bool(Image&, std::string&)>& decode) {
    std::promise<Decoded> made;
    std::shared_future<Decoded> pending;
    bool mine = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto it = m_entries.find(id);
      if (it != m_entries.end()) {
        m_order.splice(m_order.end(), m_order, it->second.place);
        pending = it->second.image;
        ++m_hits;
      } else {
        mine = true;
        pending = made.get_future().share();
        m_order.push_back(id);
        m_entries.emplace(id, Entry{pending, std::prev(m_order.end()), 0});
        ++m_misses;
      }
    }
    if (!mine) {
      if (const Decoded image = pending.get()) {
        out = *image;
        return true;
      }
      return decode(out, error);  // failed for the first asker: fail with a message of its own
    }
    Image image;
    Decoded decoded;
    try {
      if (decode(image, error)) {
        decoded = std::make_shared<const Image>(std::move(image));
      }
    } catch (...) {
      // The waiters decode on their own; the entry goes, so the next asker retries.
      made.set_value(nullptr);
      std::lock_guard<std::mutex> lock(m_mutex);
      Settle(id, nullptr);
      throw;
    }
    made.set_value(decoded);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      Settle(id, decoded);
    }
    if (!decoded) {
      return false;
    }
    out = *decoded;  // outside the lock: a big map is a 16 MB copy
    return true;
  }

  std::string Summary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::to_string(m_misses) + " texture decodes, " + std::to_string(m_hits) + " reused";
  }

private:
  using Decoded = std::shared_ptr<const Image>;

  // Records the finished decode of `id` (dropping it if it failed) and evicts past the budget.
  void Settle(const ModelUuid& id, const Decoded& decoded) {
    const auto it = m_entries.find(id);
    if (!decoded) {
      m_order.erase(it->second.place);
      m_entries.erase(it);
      return;
    }
    it->second.bytes = decoded->rgba.size();
    m_bytes += it->second.bytes;
    // Entries still being decoded (0 bytes) stay, as does the one just made.
    for (auto old = m_order.begin(); m_bytes > m_budget && old != m_order.end();) {
      const auto entry = m_entries.find(*old);
      if (entry->second.bytes == 0 || *old == id) {
        ++old;
        continue;
      }
      m_bytes -= entry->second.bytes;
      m_entries.erase(entry);
      old = m_order.erase(old);
    }
  }

  struct Entry {
    std::shared_future<Decoded> image;
    std::list<ModelUuid>::iterator place;
    size_t bytes;
  };
  mutable std::mutex m_mutex;
  std::map<ModelUuid, Entry> m_entries;
  std::list<ModelUuid> m_order;  // least recently used first
  size_t m_budget, m_bytes = 0;
  uint64_t m_hits = 0, m_misses = 0;
};

void Run(std::string nspPath, std::string keysPath, int threads, fs::path staging) {
  YieldToGame();
  std::error_code ec;
  // The import whose unchanged stages are reused: one finished but not yet moved into place
  // (held aside while this one is made, and put back if this one fails), else the installed mod.
  const fs::path held = HeldFolder(staging);
  if (fs::exists(staging / kMarkerName, ec)) {
    fs::remove_all(held, ec);
    fs::rename(staging, held, ec);
  } else if (!fs::exists(held / kMarkerName, ec)) {
    fs::remove_all(held, ec);
  }
  const bool fromHeld = fs::exists(held / kMarkerName, ec);
  const fs::path source = fromHeld ? held : staging.parent_path() / kImportModName;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec) {
    const std::string why = ec.message();
    if (fromHeld) {
      fs::remove_all(staging, ec);
      fs::rename(held, staging, ec);
    }
    Finish(false, "Cannot create the mod folder: " + why);
    return;
  }
  auto fail = [&](const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(sStateMutex);
      sStageLines = nullptr;
    }
    fs::remove_all(staging, ec);
    if (fromHeld) {
      fs::rename(held, staging, ec);
    }
    Finish(false, sCancel ? std::string("Cancelled.") : message);
  };
  Manifest previous;
  if (WantsReuse() && !ReadManifest(source / kManifestName, previous)) {
    previous.clear();
  }
  // MP_REMASTERED_REUSE=textures: every stage made again, from the textures already converted.
  const char* reuseEnv = port::EnvString("MP_REMASTERED_REUSE");
  const bool texturesOnly = reuseEnv != nullptr && std::strcmp(reuseEnv, "textures") == 0;

  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    fail(error);
    return;
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }

  size_t count = 0;
  const TableEntry* table = Table(count);
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState.total = int(count);
  }
  std::atomic<size_t> next{0};
  std::atomic<int> converted{0};
  // Ids the import has given out: no two resources of it share one, whatever their types.
  std::mutex takenMutex;
  std::unordered_set<uint32_t> taken;
  // The PBR maps a model worker has taken on, so that a map several models
  // share is converted once rather than once per worker that meets it.
  std::mutex modelClaimMutex;
  std::unordered_set<uint32_t> modelClaimed;

  // The stages, each either made and recorded in `made`, or reused from `previous`.
  Manifest made;
  std::vector<std::string> reused;
  std::string keyCommon;  // what every stage depends on: the image and the disc
  {
    const fs::path nsp = PathFromString(nspPath);
    const uintmax_t size = fs::file_size(nsp, ec);
    keyCommon = "|nsp " + std::to_string(ec ? 0 : size);
    const auto time = fs::last_write_time(nsp, ec);
    keyCommon += " " + std::to_string(ec ? 0 : int64_t(time.time_since_epoch().count()));
    keyCommon += "|disc " + Hex64(retail.Fingerprint());
  }
  const std::string keyTextures = "|textures " + std::to_string(ImportStage::kTextures) + " " + TextureFormatName();
  const std::string keyConverter = "|converter " + std::to_string(ImportStage::kConverter) + keyTextures;
  // The converted textures of the previous import, while the textures are written the same way.
  TextureMemory textureMemory;
  const std::string keptKey = Hex64(Hash64(kTextureStage + keyTextures + keyCommon));
  if (const auto old = previous.find(kTextureStage); old != previous.end() && old->second.key == keptKey) {
    textureMemory.Load(old->second);
  }
  previous.erase(kTextureStage);
  if (texturesOnly) {
    previous.clear();
  }
  std::mutex recordMutex;
  StageRecord* active = nullptr;  // the stage being made
  std::unordered_set<uint32_t> takenBefore;
  auto record = [&](const fs::path& path) {
    std::lock_guard<std::mutex> lock(recordMutex);
    if (active != nullptr) {
      const std::u8string name = path.lexically_relative(staging).generic_u8string();
      active->files.emplace_back(name.begin(), name.end());
    }
  };
  auto recordFolder = [&](const fs::path& folder) {
    std::error_code walkError;
    for (fs::recursive_directory_iterator it(folder, walkError), end; !walkError && it != end; it.increment(walkError)) {
      if (it->is_regular_file(walkError)) {
        record(it->path());
      }
    }
  };
  // Starts stage `name`. True when the previous import's is reused: its files are linked in, its
  // ids taken and its lines shown, and the caller restores what it hands on from the record.
  // A stage that gives out ids is reused only if none of them has been taken since (`strict`:
  // only if the same ids were taken before it), so that ids stay unique.
  auto beginStage = [&](const std::string& name, const std::string& key, bool strict,
                        const std::string& requiredFile = std::string()) {
    StageRecord fresh;
    fresh.key = Hex64(Hash64(name + "|" + key + keyCommon));
    fresh.before = TakenHash(taken);
    const auto old = previous.find(name);
    bool reuse = old != previous.end() && old->second.key == fresh.key && (!strict || old->second.before == fresh.before);
    for (size_t i = 0; reuse && i < old->second.ids.size(); ++i) {
      reuse = taken.count(old->second.ids[i]) == 0;
    }
    // A stage the previous import made before it wrote its report part is made again.
    if (reuse && !requiredFile.empty()) {
      reuse = std::find(old->second.files.begin(), old->second.files.end(), requiredFile) != old->second.files.end();
    }
    if (reuse) {
      SetMessage("Reusing the previous import's " + name);
      reuse = LinkFiles(source, staging, old->second.files);
    }
    if (reuse) {
      StageRecord& kept = made[name] = old->second;
      kept.before = fresh.before;
      taken.insert(kept.ids.begin(), kept.ids.end());
      // A file another stage wrote too has its id there; it is this one's as well.
      for (const std::string& file : kept.files) {
        const std::string base = file.substr(file.rfind('/') + 1);
        if (base.size() > 9 && base[8] == '.' &&
            std::all_of(base.begin(), base.begin() + 8, [](char c) { return std::isxdigit(uint8_t(c)) != 0; })) {
          taken.insert(uint32_t(std::strtoul(base.substr(0, 8).c_str(), nullptr, 16)));
        }
      }
      for (const std::string& line : kept.lines) {
        AddLine(line);
      }
      reused.push_back(name);
      return true;
    }
    StageRecord& stage = made[name] = std::move(fresh);
    takenBefore = taken;
    {
      std::lock_guard<std::mutex> lock(recordMutex);
      active = &stage;
    }
    std::lock_guard<std::mutex> lock(sStateMutex);
    sStageLines = &stage.lines;
    return false;
  };
  auto endStage = [&] {
    std::lock_guard<std::mutex> lock(recordMutex);
    if (active == nullptr) {
      return;
    }
    for (const uint32_t id : taken) {
      if (takenBefore.count(id) == 0) {
        active->ids.push_back(id);
      }
    }
    std::sort(active->ids.begin(), active->ids.end());
    std::sort(active->files.begin(), active->files.end());
    active->files.erase(std::unique(active->files.begin(), active->files.end()), active->files.end());
    active = nullptr;
    takenBefore.clear();
    std::lock_guard<std::mutex> stateLock(sStateMutex);
    sStageLines = nullptr;
  };
  auto writeFile = [&](const fs::path& path, const std::vector<uint8_t>& data) {
    if (!WriteReplacing(path, data)) {
      return false;
    }
    record(path);
    return true;
  };

  // 32 MB a worker (a 2048x2048 map is 16 MB), 512 MB at most: 16 workers reuse 57% of
  // the decodes at this size (1 GB: 64%, for ~0.9 GB more peak RSS).
  TextureCache textures(std::min(size_t(std::max(threads, 1)) * (size_t(32) << 20), size_t(512) << 20));

  // The converters' decisions, collected while a stage runs and written at its end as part
  // files (reports/parts/); a reused stage's parts are linked in, and the final reports are
  // merged from all of them.
  std::mutex reportMutex;
  std::vector<std::string> materialRows;
  std::vector<std::string> effectRows;
  auto writeReportPart = [&](const std::string& stage, bool withEffects) {
    std::vector<std::string> materials;
    std::vector<std::string> effects;
    {
      std::lock_guard<std::mutex> lock(reportMutex);
      materials.swap(materialRows);
      effects.swap(effectRows);
    }
    const fs::path folder = staging / "reports" / "parts";
    std::error_code reportError;
    fs::create_directories(folder, reportError);
    const std::string m = JoinReport(MaterialReportHeader(), std::move(materials));
    writeFile(folder / (stage + ".materials.tsv"), std::vector<uint8_t>(m.begin(), m.end()));
    if (withEffects) {
      const std::string e = JoinReport(EffectReportHeader(), std::move(effects));
      writeFile(folder / (stage + ".effects.tsv"), std::vector<uint8_t>(e.begin(), e.end()));
    }
  };

  auto makeIO = [&](int worker, const fs::path& folder) {
    ConvertIO io;
    io.decision = [&](const MaterialDecision& decision) {
      const std::string row = FormatMaterialRow(decision);
      std::lock_guard<std::mutex> lock(reportMutex);
      materialRows.push_back(row);
    };
    io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
    io.retailId = [&](uint32_t id) { return retail.HasId(id); };
    // MP_REMASTERED_JOINTS=1: the models' skin log (joint to bone) on stderr.
    if (port::EnvFlag("MP_REMASTERED_JOINTS")) {
      io.log = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(reportMutex);
        std::fprintf(stderr, "%s\n", line.c_str());
      };
    }
    io.texture = [&](const ModelUuid& id, Image& out, std::string& textureError) {
      return textures.Get(id, out, textureError, [&](Image& decoded, std::string& decodeError) {
        std::vector<uint8_t> raw;
        TxtrImage image;
        if (!remastered.ReadTexture(id, raw, decodeError) ||
            !DecodeTxtr(raw.data(), raw.size(), image, decodeError)) {
          return false;
        }
        decoded.width = int(image.width);
        decoded.height = int(image.height);
        decoded.rgba = std::move(image.rgba);
        decoded.srgb = image.srgb;
        return true;
      });
    };
    io.volume = [&](const ModelUuid& id, Image& out, std::string& volumeError) {
      std::vector<uint8_t> raw, slices;
      uint32_t w = 0, h = 0, d = 0;
      bool srgb = false;
      if (!remastered.ReadTexture(id, raw, volumeError) ||
          !DecodeTxtrVolume(raw.data(), raw.size(), w, h, d, srgb, slices, volumeError)) {
        return false;
      }
      if (w != 64 || h != 64 || d != 64) {
        volumeError = "a volume of " + std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(d) +
                      " is not the 64^3 the atlas holds";
        return false;
      }
      out.width = out.height = 512;
      out.srgb = srgb;
      out.rgba.assign(size_t(512) * 512 * 4, 0);
      for (uint32_t z = 0; z < 64; ++z) {
        for (uint32_t y = 0; y < 64; ++y) {
          std::memcpy(&out.rgba[((size_t(z / 8) * 64 + y) * 512 + size_t(z % 8) * 64) * 4],
                      &slices[(size_t(z) * 64 + y) * 64 * 4], 64 * 4);
        }
      }
      return true;
    };
    io.cube = [&](const ModelUuid& id, uint32_t& edge, std::vector<float>& rgba, std::string& cubeError) {
      std::vector<uint8_t> raw;
      return remastered.ReadTexture(id, raw, cubeError) &&
             DecodeTxtrCubeLinear(raw.data(), raw.size(), edge, rgba, cubeError);
    };
    // Workers can still meet the same texture at once (a TEV slot's, a solid
    // colour); each writes its own temporary file and the rename decides, the
    // content being the same either way.
    io.write = [&, worker, folder](const std::string& name, const std::vector<uint8_t>& data) {
      {
        std::lock_guard<std::mutex> lock(takenMutex);
        taken.insert(uint32_t(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16)));
      }
      const fs::path path = folder / PathFromString(name);
      const fs::path tmp = folder / PathFromString(name + ".tmp" + std::to_string(worker));
      {
        std::ofstream file(tmp, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        file.close();
        if (!file) {
          return false;
        }
      }
      std::error_code renameError;
      fs::rename(tmp, path, renameError);
      if (renameError) {
        return false;
      }
      record(path);
      return true;
    };
    const std::u8string scope = folder.lexically_relative(staging).generic_u8string();
    const std::string prefix = std::string(scope.begin(), scope.end()) + "|";
    io.recall = [&, prefix](const std::string& key, std::string& value) {
      TextureMemory::Entry entry;
      if (!textureMemory.Find(prefix + key, entry)) {
        return false;
      }
      value = std::move(entry.value);
      return true;
    };
    // A file is linked from the previous import, unless a converter of this one has put it
    // there already; under a temporary name first, as io.write does.
    io.relink = [&, worker, prefix](const std::string& key) {
      TextureMemory::Entry entry;
      if (!textureMemory.Find(prefix + key, entry)) {
        return false;
      }
      std::error_code linkError;
      for (const std::string& file : entry.files) {
        const fs::path relative = PathFromString(file).lexically_normal();
        if (relative.empty() || relative.has_root_path() || *relative.begin() == ".." ||
            (!fs::is_regular_file(staging / relative, linkError) && !fs::is_regular_file(source / relative, linkError))) {
          return false;
        }
      }
      for (const std::string& file : entry.files) {
        const fs::path path = staging / PathFromString(file);
        if (!fs::is_regular_file(path, linkError)) {
          const fs::path from = source / PathFromString(file);
          fs::path tmp = path;
          tmp += ".tmp" + std::to_string(worker);
          fs::remove(tmp, linkError);
          fs::create_hard_link(from, tmp, linkError);
          if (linkError) {
            fs::copy_file(from, tmp, linkError);
          }
          if (!linkError) {
            fs::rename(tmp, path, linkError);
          }
          if (linkError) {
            fs::remove(tmp, linkError);
            return false;
          }
        }
        const std::string base = path.filename().string();
        {
          std::lock_guard<std::mutex> lock(takenMutex);
          taken.insert(uint32_t(std::strtoul(base.substr(0, 8).c_str(), nullptr, 16)));
        }
        record(path);
      }
      return true;
    };
    io.remember = [&, folder, prefix](const std::string& key, const std::string& value,
                                      const std::vector<std::string>& files) {
      TextureMemory::Entry entry{value, {}};
      for (const std::string& name : files) {
        const std::u8string relative = (folder / PathFromString(name)).lexically_relative(staging).generic_u8string();
        entry.files.emplace_back(relative.begin(), relative.end());
      }
      textureMemory.Put(prefix + key, std::move(entry));
    };
    return io;
  };
  // Second looks of a retail model (TableEntry::ancs, key) are written under
  // ids of their own, given out here before anything is written so that they
  // come out the same in every import.
  struct Look {
    bool ok = true;
    std::string error;
    uint32_t model = 0;  // the CMDL's id, 0 for the retail one
    std::vector<uint32_t> skins;  // the CSKRs' ids, empty for the retail ones
    uint32_t ancs = 0;            // the ANCS copy's id, 0 for none
  };
  std::vector<Look> looks(count);
  // The characters written once the models are in: retail ANCS with some of
  // their (model, skin[, skeleton]) ids, big-endian and side by side, swapped
  // for new ones. A swap is made only if its model and skin were written.
  struct Rebind {
    size_t entry;
    std::vector<uint8_t> from, to;
    uint32_t skin;  // the CSKR `to` names
  };
  struct Character {
    uint32_t source = 0;  // the retail ANCS it is a copy of
    std::vector<uint8_t> data;
    std::vector<Rebind> rebinds;  // its own looks'
  };
  std::map<uint32_t, Character> characters;
  std::vector<Rebind> everywhere;  // swapped in every character that binds them
  auto bytesOf = [](std::initializer_list<uint32_t> ids) {
    std::vector<uint8_t> out;
    for (uint32_t id : ids) {
      for (int b = 0; b < 4; ++b) {
        out.push_back(uint8_t(id >> (24 - b * 8)));
      }
    }
    return out;
  };
  auto readU32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
  auto hex = [](uint32_t id) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X", id);
    return std::string(name);
  };
  {
    auto variant = [&](Look& look, uint32_t id, int key) {
      const uint32_t out = PortModelVariant::Id(id, key);
      if (retail.HasId(out) || taken.count(out) != 0) {
        look.ok = false;
        look.error = "the id for look " + std::to_string(key) + " of " + hex(id) + " is taken";
      }
      taken.insert(out);
      return out;
    };
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs == kEveryCharacter) {
        if (entry.key >= 0) {
          look.ok = false;
          look.error = "a model bound in every character has no second look";
        }
      } else if (entry.ancs == 0 && entry.key >= 0) {
        look.model = variant(look, entry.retail, entry.key);
      } else if (entry.ancs != 0) {
        look.ancs = entry.key >= 0 ? variant(look, entry.ancs, entry.key) : entry.ancs;
        if (entry.skinCount != 1) {
          look.ok = false;
          look.error = "a character's look needs exactly one skin";
        }
      }
    }
    // The new models and skins of the looks: hashed, then moved past every id
    // the disc or this import has.
    auto fresh = [&](uint32_t seed) {
      uint32_t id = 0x811C9DC5u;  // FNV-1a
      for (int i = 0; i < 4; ++i) {
        id = (id ^ ((seed >> (i * 8)) & 0xFFu)) * 0x01000193u;
      }
      while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
        ++id;
      }
      taken.insert(id);
      return id;
    };
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs != 0 || entry.key < 0 || !look.ok) {
        continue;
      }
      // A static look of a skinned model (the low-poly and glass balls) is
      // drawn without its skin, but the converter still writes one; it must
      // not land on the retail model's.
      const uint32_t* skins = TableSkins(entry);
      for (int s = 0; s < entry.skinCount; ++s) {
        look.skins.push_back(fresh(look.model ^ skins[s] * 0x9E3779B1u));
      }
    }
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (look.ancs == 0 || !look.ok) {
        continue;
      }
      // The copy binds the new pair where the retail one binds the old.
      Character& character = characters[look.ancs];
      character.source = entry.ancs;
      if (character.data.empty() && !retail.Read(kANCS, entry.ancs, character.data)) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " is not on the disc";
        continue;
      }
      const std::vector<uint8_t> pair = bytesOf({entry.retail, TableSkins(entry)[0]});
      const std::vector<uint8_t>& data = character.data;
      int found = 0;
      for (auto it = std::search(data.begin(), data.end(), pair.begin(), pair.end()); it != data.end();
           it = std::search(it + 1, data.end(), pair.begin(), pair.end())) {
        ++found;
      }
      if (found != 1) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " binds " + hex(entry.retail) + " " + std::to_string(found) +
                     " times";
        continue;
      }
      const uint32_t seed = entry.ancs * 0x9E3779B1u ^ entry.retail ^ uint32_t(entry.key + 1) * 0x85EBCA6Bu;
      look.model = fresh(seed);
      look.skins = {fresh(seed ^ 0x534B494Eu)};
      character.rebinds.push_back({i, pair, bytesOf({look.model, look.skins[0]}), look.skins[0]});
    }
    // A body whose skin another model shares keeps its id but takes skins of
    // its own, and every character that binds it is rebound to them. Its
    // first skin's CSKR serves every skin it is bound with on the same
    // skeleton: the weights name the skeleton's bones.
    std::vector<size_t> bodies;
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs != kEveryCharacter || !look.ok) {
        continue;
      }
      bodies.push_back(i);
      const uint32_t* skins = TableSkins(entry);
      for (int s = 0; s < entry.skinCount; ++s) {
        look.skins.push_back(fresh(entry.retail * 0x85EBCA6Bu ^ skins[s]));
      }
    }
    if (!bodies.empty()) {
      // The skeletons each (body, skin) is bound with.
      std::map<std::pair<size_t, int>, std::set<uint32_t>> skeletons;
      for (uint32_t id : retail.Ids(kANCS)) {
        const auto known = characters.find(id);
        std::vector<uint8_t> read;
        if (known == characters.end() && !retail.Read(kANCS, id, read)) {
          continue;
        }
        const std::vector<uint8_t>& data = known != characters.end() ? known->second.data : read;
        bool binds = false;
        for (size_t i : bodies) {
          const TableEntry& entry = table[i];
          for (int s = 0; s < entry.skinCount; ++s) {
            const std::vector<uint8_t> pair = bytesOf({entry.retail, TableSkins(entry)[s]});
            for (auto it = std::search(data.begin(), data.end(), pair.begin(), pair.end()); data.end() - it >= 12;
                 it = std::search(it + 1, data.end(), pair.begin(), pair.end())) {
              skeletons[{i, s}].insert(readU32(&*(it + 8)));
              binds = true;
            }
          }
        }
        if (binds && known == characters.end()) {
          characters[id] = Character{id, std::move(read), {}};
        }
      }
      for (const auto& [key, bound] : skeletons) {
        const auto [i, s] = key;
        const TableEntry& entry = table[i];
        const Look& look = looks[i];
        const auto first = skeletons.find({i, 0});
        for (uint32_t skeleton : bound) {
          const bool shared = first != skeletons.end() && first->second.count(skeleton) != 0;
          const uint32_t skin = shared ? look.skins[0] : look.skins[s];
          everywhere.push_back({i, bytesOf({entry.retail, TableSkins(entry)[s], skeleton}),
                                bytesOf({entry.retail, skin, skeleton}), skin});
        }
      }
    }
  }
  std::vector<uint8_t> modelOk(count, 0);
  const bool modelsReused =
      beginStage("models", std::to_string(ImportStage::kModels) + " table " + TableKey() + keyConverter, true,
                  "reports/parts/models.materials.tsv");
  auto work = [&](int worker) {
    YieldToGame();
    ConvertIO io = makeIO(worker, staging);
    io.claim = [&](uint32_t id) {
      std::lock_guard<std::mutex> lock(modelClaimMutex);
      return modelClaimed.insert(id).second;
    };
    Converter converter(std::move(io));
    for (size_t i = next++; i < count && !sCancel; i = next++) {
      const TableEntry& entry = table[i];
      const Look& look = looks[i];
      ModelUuid id;
      std::memcpy(id.data(), entry.rem, 16);
      std::string modelError = look.error;
      bool ok = false;
      // A worker thread that lets an exception out (bad_alloc on a phone)
      // terminates the game, so a model that throws only fails itself.
      try {
        std::vector<uint8_t> raw;
        Model model;
        ConvertOptions options = OptionsFor(entry);
        options.outputModel = look.model;
        options.source = UuidText(id);
        options.outputSkins = look.skins;
        ok = look.ok && remastered.ReadModel(id, raw, modelError) &&
             ParseModel(raw.data(), raw.size(), model, modelError) && converter.Convert(model, options, modelError);
      } catch (const std::exception& e) {
        ok = false;
        modelError = e.what();
      }
      if (ok) {
        modelOk[i] = 1;
        ++converted;
      } else {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X", entry.retail);
        AddLine(std::string(name) + ": " + modelError);
      }
      std::lock_guard<std::mutex> lock(sStateMutex);
      ++sState.done;
      if (!ok) {
        ++sState.failed;
      }
      sState.message = "Converting models (" + std::to_string(sState.done) + "/" + std::to_string(sState.total) + ")";
    }
  };
  std::vector<std::thread> workers;
  if (modelsReused) {
    converted = int(made["models"].counts["converted"]);
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState.done = int(count);
    sState.failed = int(count) - converted;
  } else {
    SetMessage("Converting models");
    for (int i = 1; i < threads; ++i) {
      workers.emplace_back(work, i);
    }
    work(0);
    for (std::thread& worker : workers) {
      worker.join();
    }
  }

  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  if (converted == 0) {
    fail("No model could be converted.");
    return;
  }

  // The characters last, so that none names a model that failed. A copy is
  // written only if one of its own looks made it.
  if (!modelsReused) {
    const auto write = makeIO(0, staging).write;
    for (auto& [id, character] : characters) {
      std::vector<uint8_t>& data = character.data;
      bool own = false;
      bool changed = false;
      auto apply = [&](const Rebind& rebind) {
        bool swapped = false;
        std::error_code skinError;
        if (modelOk[rebind.entry] != 0 && fs::exists(staging / (hex(rebind.skin) + ".CSKR"), skinError)) {
          for (auto it = std::search(data.begin(), data.end(), rebind.from.begin(), rebind.from.end());
                it != data.end();
               it = std::search(it + rebind.from.size(), data.end(), rebind.from.begin(), rebind.from.end())) {
            std::copy(rebind.to.begin(), rebind.to.end(), it);
            swapped = true;
          }
        }
        return swapped;
      };
      for (const Rebind& rebind : character.rebinds) {
        own = apply(rebind) || own;
      }
      for (const Rebind& rebind : everywhere) {
        changed = apply(rebind) || changed;
      }
      if (!(own || (changed && id == character.source))) {
        continue;
      }
      if (!write(hex(id) + ".ANCS", data)) {
        AddLine("could not write " + hex(id) + ".ANCS");
      }
    }
    made["models"].counts["converted"] = converted;
    writeReportPart("models", false);
    endStage();
  }

  // Remastered's particle effects in place of the disc's, when asked for.
  if (WantsRemasteredEffects() &&
      !beginStage("effects", std::to_string(ImportStage::kEffects) + keyConverter, false, "reports/parts/effects.effects.tsv")) {
    SetMessage("Converting effects");
    EffectImportIO effectIO;
    effectIO.effects = remastered.Effects();
    effectIO.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& effectError) {
      return remastered.ReadEffectAsset(type, id, out, effectError);
    };
    effectIO.typeOf = [&](const EffectGuid& id) { return remastered.EffectAssetType(id); };
    effectIO.retailId = [&](uint32_t id) { return retail.HasId(id); };
    effectIO.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
    effectIO.freshId = [&](uint32_t seed) {
      std::lock_guard<std::mutex> lock(takenMutex);
      uint32_t id = seed;
      while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
        ++id;
      }
      taken.insert(id);
      return id;
    };
    effectIO.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba,
                           std::string& effectError) {
      Image image;
      if (!makeIO(0, staging).texture(id, image, effectError)) {
        return false;
      }
      width = image.width;
      height = image.height;
      rgba = std::move(image.rgba);
      return true;
    };
    effectIO.textureSrgb = [&](const EffectGuid& id) {
      std::vector<uint8_t> raw;
      std::string ignored;
      TxtrImage info;
      return remastered.ReadTexture(id, raw, ignored) && ReadTxtrInfo(raw.data(), raw.size(), info, ignored) &&
             info.srgb;
    };
    effectIO.layers = [&](const EffectGuid& id, int& width, int& height, int& layers, std::vector<uint8_t>& rgba,
                          std::string& effectError) {
      std::vector<uint8_t> raw;
      uint32_t w = 0;
      uint32_t h = 0;
      uint32_t n = 0;
      if (!remastered.ReadTexture(id, raw, effectError) ||
          !DecodeTxtrLayersRgba8(raw.data(), raw.size(), w, h, n, rgba, effectError)) {
        return false;
      }
      width = int(w);
      height = int(h);
      layers = int(n);
      return true;
    };
    // A model of Remastered's own, as a standalone CMDL under the id the effect
    // was given. One converter serves them all: the effects run on one thread.
    std::unique_ptr<Converter> effectModels;
    std::unordered_set<uint32_t> effectClaimed;
    std::unordered_map<uint32_t, std::vector<uint8_t>> effectMeshes;  // VMSH blob by converted CMDL id
    effectIO.modelMesh = [&](uint32_t model) {
      const auto found = effectMeshes.find(model);
      return found != effectMeshes.end() ? found->second : std::vector<uint8_t>();
    };
    effectIO.model = [&](const EffectGuid& id, uint32_t retailId, std::string& effectError) {
      if (!effectModels) {
        ConvertIO io = makeIO(0, staging);
        io.retailId = [&](uint32_t other) { return retail.HasId(other); };
        io.claim = [&](uint32_t other) { return effectClaimed.insert(other).second; };
        effectModels = std::make_unique<Converter>(std::move(io));
      }
      ConvertOptions options;
      options.retail = retailId;
      options.source = EffectGuidString(id);
      options.standalone = true;
      options.skip.clear();
      options.nativeMax = kGeometryTexture;
      options.vmsh = &effectMeshes[retailId];
      try {
        std::vector<uint8_t> raw;
        Model model;
        return remastered.ReadModel(id, raw, effectError) && ParseModel(raw.data(), raw.size(), model, effectError) &&
               effectModels->Convert(model, options, effectError);
      } catch (const std::exception& e) {
        effectError = e.what();
        return false;
      }
    };
    effectIO.write = makeIO(0, staging).write;
    // One line an effect the disc keeps (and why), plus the models: too many for the panel, so
    // the log. The panel gets the count, the reasons are also in the effects report.
    effectIO.log = [](const std::string& line) {
      static std::mutex logMutex;
      std::lock_guard<std::mutex> lock(logMutex);
      std::printf("remastered import: %s\n", line.c_str());
    };
    effectIO.report = [&](const EffectReportRow& row) {
      const std::string text = FormatEffectRow(row);
      std::lock_guard<std::mutex> lock(reportMutex);
      effectRows.push_back(text);
    };
    {
      std::vector<std::string> pairLines;
      effectIO.pairings = ProjectilePairings(remastered, retail, pairLines);
      for (const std::string& line : pairLines) {
        std::printf("remastered import: projectile %s\n", line.c_str());
      }
      AddLine("effects: " + std::to_string(effectIO.pairings.size()) + " projectile effects paired");
    }
    const EffectImportResult effects = ImportEffects(effectIO);
    AddLine("effects: " + std::to_string(effects.written) + " of " + std::to_string(effects.candidates) + " written (" +
            std::to_string(effects.parts) + " PARTs, " + std::to_string(effects.textures) + " textures, " +
            std::to_string(effects.flipbooks) + " flipbooks, " + std::to_string(effects.models) + " models, " +
            std::to_string(effects.dropped) + " properties left out)");
    if (effects.failed > 0) {
      AddLine("effects: " + std::to_string(effects.failed) +
              " keep the disc's version (why: the log, reports/parts/effects.effects.tsv)");
    }
    writeReportPart("effects", true);
    endStage();
  }

  // The rooms' reflection cubes and baked ambient light, a file per area. A
  // world that cannot be read costs its rooms their environment, not the import.
  SetMessage("Writing the room environments");
  const fs::path roomFolder = staging / kRoomFolder;
  fs::create_directories(roomFolder, ec);
  const char* geometryEnv = std::getenv("MP_REMASTERED_GEOMETRY");
  const bool roomsReused = beginStage("rooms",
                                      std::to_string(ImportStage::kRooms) + " geometry " +
                                          (geometryEnv != nullptr && geometryEnv[0] != '\0'
                                               ? std::string(geometryEnv)
                                               : std::string(sGeometry ? "all" : "none")) +  // as the env says it
                                          keyConverter,
                                      false);
  // Remastered's environment BRDF table, from the user's own executable. Nothing
  // here may fail the import: without it the port keeps its own fit.
  bool brdfMissing = false;  // then the stage is not kept: the next import tries again
  if (!roomsReused) {
    std::vector<uint8_t> brdf;
    std::string brdfError;
    if (!remastered.ExtractBrdf(brdf, brdfError)) {
      AddLine("brdf.lut: left out (" + brdfError + ")");
      brdfMissing = true;
    } else if (!writeFile(roomFolder / "brdf.lut", brdf)) {
      AddLine("brdf.lut: cannot be written");
      brdfMissing = true;
    }
  }
  fs::create_directories(staging / kGeometryFolder, ec);
  const std::vector<RoomPak> allPaks = remastered.AllPaks();
  const std::vector<RoomWorld>& worlds = RoomWorlds();
  std::atomic<size_t> nextWorld{0};
  std::atomic<int> roomFiles{0};
  // A room's geometry names its models; they are given ids here and converted
  // afterwards, on every thread. A model that then fails leaves its id naming
  // nothing, which the port skips.
  const fs::path geometryFolder = staging / kGeometryFolder;
  struct GeometryModel {
    ModelUuid uuid;
    uint32_t id;
    int liquid = -1;  // index into `liquids` when it is a lava pool
    int joint = -1;   // the joint whose rigid piece of a skinned model it is (ConvertOptions::joint)
    // Ids set aside for its coarser levels of detail (index 0 unused); a level it
    // turns out not to have, or that is not worth its file, leaves its id unused.
    std::array<uint32_t, PortRoomGeo::kLodLevels> lods{};
  };
  std::vector<RoomLiquid> liquids;
  std::vector<GeometryModel> geometry;
  std::unordered_map<ModelUuid, uint32_t, PakIdHash> geometryIds;
  auto geometryId = [&](const ModelUuid& uuid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = geometryIds.find(uuid);
    if (known != geometryIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u;  // FNV-1a
    for (const uint8_t byte : uuid) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    geometryIds.emplace(uuid, id);
    geometry.push_back({uuid, id});
    return true;
  };
  std::map<std::pair<ModelUuid, int>, uint32_t> pieceIds;
  auto pieceId = [&](const ModelUuid& uuid, int joint, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = pieceIds.find({uuid, joint});
    if (known != pieceIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u ^ uint32_t(joint + 1) * 0x85EBCA77u;
    for (const uint8_t byte : uuid) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    pieceIds.emplace(std::make_pair(uuid, joint), id);
    GeometryModel& g = geometry.emplace_back();
    g.uuid = uuid;
    g.id = id;
    g.joint = joint;
    return true;
  };
  // A lava pool is converted as a model of its own, with its pool's values baked in: one per
  // model and values, whatever room asks first (the rooms run in parallel, so the id must not
  // depend on the order they come in).
  std::map<std::string, uint32_t> liquidIds;
  auto liquidId = [&](const RoomLiquid& liquid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    std::string key(reinterpret_cast<const char*>(liquid.model.data()), liquid.model.size());
    key.append(reinterpret_cast<const char*>(liquid.lava), sizeof(liquid.lava));
    if (const auto known = liquidIds.find(key); known != liquidIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u ^ 0x9E3779B1u;
    for (const char byte : key) {
      id = (id ^ uint8_t(byte)) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    liquidIds.emplace(std::move(key), id);
    geometry.push_back({liquid.model, id, int(liquids.size())});
    liquids.push_back(liquid);
    return true;
  };
  // A water or poison surface is Remastered's own mesh and maps (the room's fields go in the
  // .roomliquid itself). Its maps are written as linear data under ids given out here, once
  // per texture across rooms.
  struct WaterTexture {
    uint32_t id = 0;
    int width = 0, height = 0;
  };
  std::mutex waterMutex;
  std::map<ModelUuid, WaterTexture> waterTextures;
  auto waterTexture = [&](const ModelUuid& uuid, WaterTexture& out) {
    out = WaterTexture{};
    if (uuid == ModelUuid{}) {
      return;
    }
    std::lock_guard<std::mutex> lock(waterMutex);
    const auto known = waterTextures.find(uuid);
    if (known != waterTextures.end()) {
      out = known->second;
      return;
    }
    WaterTexture made;
    Image image;
    std::string textureError;
    if (!makeIO(0, geometryFolder).texture(uuid, image, textureError)) {
      AddLine("water texture: " + textureError);
    } else {
      uint32_t id = 0x811C9DC5u ^ 0x57A7E5u;
      for (const uint8_t byte : uuid) {
        id = (id ^ byte) * 0x01000193u;
      }
      {
        std::lock_guard<std::mutex> takenLock(takenMutex);
        while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
          ++id;
        }
        taken.insert(id);
      }
      // As the effect import: small ones are the TXTR alone; over the stub's side the .dds is
      // the texture and a small TXTR stands in for it on the game's heap.
      constexpr int kStub = 64;
      const auto roundUp4 = [](int v) { return std::max(8, (v + 3) / 4 * 4); };
      const int edge = std::max(image.width, image.height);
      const auto write = makeIO(9999, geometryFolder).write;
      const std::string name = hex(id);
      bool wrote;
      if (edge <= kStub) {
        wrote = write(name + ".TXTR", EncodeTxtrRgba8(image));
      } else {
        const Image stub = Resize(image, roundUp4(image.width * kStub / edge), roundUp4(image.height * kStub / edge),
                                  MapKind::Data);
        wrote = write(name + ".dds", EncodeDds(image, ColourDdsFormat(), false, MapKind::Data)) &&
                write(name + ".TXTR", EncodeTxtrRgba8(stub));
      }
      if (wrote) {
        made = {id, image.width, image.height};
      } else {
        AddLine("water texture " + name + ": cannot be written");
      }
    }
    waterTextures.emplace(uuid, made);
    out = made;
  };
  auto waterSurface = [&](const RoomLiquid& liquid, RoomWaterAssets& assets) {
    std::string meshError;
    try {
      std::vector<uint8_t> raw;
      Model model;
      if (!remastered.ReadModel(liquid.model, raw, meshError) || !ParseModel(raw.data(), raw.size(), model, meshError)) {
        AddLine("water mesh: " + meshError);
        return false;
      }
      // The finest level's meshes only, as the converter keeps them.
      std::vector<bool> finest(model.meshes.size(), false);
      bool anyFinest = false;
      for (size_t r = 0; r < 5 && r < model.lods.size(); ++r) {
        const ModelLod& range = model.lods[r];
        for (uint64_t i = range.indexOffset; i < uint64_t(range.indexOffset) + range.indexCount; ++i) {
          if (i < model.lodMeshes.size() && model.lodMeshes[i] < finest.size()) {
            finest[model.lodMeshes[i]] = true;
            anyFinest = true;
          }
        }
      }
      std::map<uint32_t, uint32_t> base;  // vertex buffer -> its first vertex in the merge
      bool any = false;
      for (size_t m = 0; m < model.meshes.size(); ++m) {
        const ModelMesh& mesh = model.meshes[m];
        if ((anyFinest && !finest[m]) || mesh.vertexBuffer >= model.vertexBuffers.size()) {
          continue;
        }
        const ModelVertexBuffer& vb = model.vertexBuffers[mesh.vertexBuffer];
        const size_t n = vb.vertexCount;
        if (vb.positions.size() != n * 3) {
          meshError = "a vertex buffer has no positions";
          break;
        }
        auto at = base.find(mesh.vertexBuffer);
        if (at == base.end()) {
          at = base.emplace(mesh.vertexBuffer, uint32_t(assets.vertices.size())).first;
          const std::vector<float>* uv = !vb.uvs.empty() && vb.uvs[0].size() == n * 2 ? &vb.uvs[0] : nullptr;
          const std::vector<float>* zw = !vb.uvsZw.empty() && vb.uvsZw[0].size() == n * 2 ? &vb.uvsZw[0] : nullptr;
          const bool colours = vb.colors.size() == n * 4;
          for (size_t v = 0; v < n; ++v) {
            RoomWaterAssets::Vertex out{};
            for (int c = 0; c < 3; ++c) {
              out.pos[c] = vb.positions[v * 3 + c];
              if (!any) {
                assets.boundsMin[c] = assets.boundsMax[c] = out.pos[c];
              }
              assets.boundsMin[c] = std::min(assets.boundsMin[c], out.pos[c]);
              assets.boundsMax[c] = std::max(assets.boundsMax[c], out.pos[c]);
            }
            any = true;
            for (int c = 0; c < 2; ++c) {
              out.uv[c] = uv ? (*uv)[v * 2 + c] : 0.0f;
              out.uv[2 + c] = zw ? (*zw)[v * 2 + c] : 0.0f;
            }
            for (int c = 0; c < 4; ++c) {
              out.color[c] = colours ? uint8_t(std::clamp(vb.colors[v * 4 + c], 0.0f, 1.0f) * 255.0f + 0.5f) : 255;
            }
            assets.vertices.push_back(out);
          }
        }
        const size_t count = mesh.indices.size() / 3 * 3;
        for (size_t i = 0; i < count; ++i) {
          if (mesh.indices[i] >= n) {
            meshError = "a mesh indexes past its vertex buffer";
            break;
          }
          assets.indices.push_back(at->second + mesh.indices[i]);
        }
        if (!meshError.empty()) {
          break;
        }
      }
      if (!meshError.empty() || assets.indices.empty()) {
        AddLine("water mesh: " + (meshError.empty() ? std::string("no triangles") : meshError));
        return false;
      }
    } catch (const std::exception& e) {
      AddLine(std::string("water mesh: ") + e.what());
      return false;
    }
    WaterTexture normal, flow, noise;
    waterTexture(liquid.normalMap, normal);
    waterTexture(liquid.flowMap, flow);
    waterTexture(liquid.rainNoise, noise);
    assets.normalMap = normal.id;
    assets.flowMap = flow.id;
    assets.rainNoise = noise.id;
    assets.rainNoiseWidth = uint32_t(noise.width);
    assets.rainNoiseHeight = uint32_t(noise.height);
    return true;
  };
  auto roomWork = [&] {
    YieldToGame();
    for (size_t i = nextWorld++; i < worlds.size() && !sCancel; i = nextWorld++) {
      RoomPak master;
      std::vector<RoomPak> rooms;
      remastered.World(worlds[i].dir, master, rooms);
      RoomIO io;
      io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
      io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
        const bool isGeometry = (name.size() > 8 && name.compare(name.size() - 8, 8, ".roomgeo") == 0) ||
                                (name.size() > 11 && name.compare(name.size() - 11, 11, ".roomliquid") == 0);
        return writeFile((isGeometry ? geometryFolder : roomFolder) / PathFromString(name), data);
      };
      io.model = geometryId;
      io.piece = pieceId;
      io.liquid = liquidId;
      io.water = waterSurface;
      io.wantsGeometry = WantsGeometry;
      io.cancelled = [] { return sCancel.load(); };
      // One line a room (what was left out and why): too many for the panel, so the log.
      io.log = [](const std::string& line) {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        std::printf("remastered import: %s\n", line.c_str());
      };
      int written = 0;
      std::string worldError;
      bool ok = false;
      // As for the models: an exception here would terminate the game.
      try {
        ok = WriteWorldRoomEnvs(worlds[i].mlvl, master, rooms, allPaks, io, written, worldError);
      } catch (const std::exception& e) {
        worldError = e.what();
      }
      if (!ok && !sCancel) {
        AddLine(std::string(worlds[i].dir) + ": " + worldError);
      }
      roomFiles += written;
    }
  };
  // What the room models stage needs from this one, as the record's `extra` lines:
  // "g <uuid> <id> <joint> <liquid>" a model, "l <lava values>" a lava pool, in order.
  if (roomsReused) {
    StageRecord& kept = made["rooms"];
    roomFiles = int(kept.counts["files"]);
    for (const std::string& line : kept.extra) {
      std::istringstream in(line);
      std::string tag;
      in >> tag;
      if (tag == "g") {
        std::string uuid;
        GeometryModel g;
        in >> uuid >> std::hex >> g.id >> std::dec >> g.joint >> g.liquid;
        for (size_t b = 0; b < g.uuid.size() && b * 2 + 1 < uuid.size(); ++b) {
          g.uuid[b] = uint8_t(std::strtoul(uuid.substr(b * 2, 2).c_str(), nullptr, 16));
        }
        geometry.push_back(g);
      } else if (tag == "l") {
        RoomLiquid& liquid = liquids.emplace_back();
        for (float& value : liquid.lava) {
          std::string text;
          in >> text;
          value = std::strtof(text.c_str(), nullptr);
        }
      }
    }
  } else {
    workers.clear();
    for (int i = 1; i < threads; ++i) {
      workers.emplace_back(roomWork);
    }
    roomWork();
    for (std::thread& worker : workers) {
      worker.join();
    }
    if (sCancel) {
      fail("Cancelled.");
      return;
    }
    StageRecord& stage = made["rooms"];
    stage.counts["files"] = roomFiles.load();
    for (const GeometryModel& g : geometry) {
      char line[96];
      std::snprintf(line, sizeof(line), "g %s %08X %d %d", UuidText(g.uuid).c_str(), g.id, g.joint, g.liquid);
      stage.extra.push_back(line);
    }
    for (const RoomLiquid& liquid : liquids) {
      std::string line = "l";
      for (const float value : liquid.lava) {
        char text[32];
        std::snprintf(text, sizeof(text), " %a", double(value));
        line += text;
      }
      stage.extra.push_back(line);
    }
    endStage();
    if (brdfMissing) {
      made.erase("rooms");
    }
  }

  std::atomic<int> geometryDone{0};
  std::atomic<int> lodsDone{0};
  bool roomModelsReused = false;
  if (!geometry.empty()) {
    // The models in any order (the room workers add them as they come), each with its lava.
    std::vector<std::string> lines;
    for (const GeometryModel& g : geometry) {
      std::string line = UuidText(g.uuid);
      char text[48];
      std::snprintf(text, sizeof(text), " %08X %d", g.id, g.joint);
      line += text;
      if (g.liquid >= 0 && size_t(g.liquid) < liquids.size()) {
        for (const float value : liquids[size_t(g.liquid)].lava) {
          std::snprintf(text, sizeof(text), " %a", double(value));
          line += text;
        }
      }
      lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    std::string list;
    for (const std::string& line : lines) {
      list += line + '\n';
    }
    roomModelsReused = beginStage("roommodels",
                                  std::to_string(ImportStage::kRoomModels) + " list " + Hex64(Hash64(list)) + keyConverter,
                                  false, "reports/parts/roommodels.materials.tsv");
    if (roomModelsReused) {
      geometryDone = int(made["roommodels"].counts["models"]);
      lodsDone = int(made["roommodels"].counts["levels"]);
    }
  }
  if (!geometry.empty() && !roomModelsReused) {
    // Before any texture takes an id, so none takes a level's.
    for (GeometryModel& g : geometry) {
      if (g.liquid >= 0 || g.joint >= 0) {
        continue;
      }
      for (int level = 1; level < PortRoomGeo::kLodLevels; ++level) {
        uint32_t id = 0x811C9DC5u ^ uint32_t(level) * 0x9E3779B1u;
        for (const uint8_t byte : g.uuid) {
          id = (id ^ byte) * 0x01000193u;
        }
        while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
          ++id;
        }
        taken.insert(id);
        g.lods[size_t(level)] = id;
      }
    }
    std::unordered_set<uint32_t> modelIds;
    for (const GeometryModel& g : geometry) {
      modelIds.insert(g.id);
      for (int level = 1; level < PortRoomGeo::kLodLevels; ++level) {
        if (g.lods[size_t(level)] != 0) {
          modelIds.insert(g.lods[size_t(level)]);
        }
      }
    }
    std::mutex lodMutex;
    std::vector<PortRoomGeo::Lods> lodTable;
    std::atomic<size_t> nextModel{0};
    std::atomic<int> seen{0};
    std::mutex claimMutex;
    std::unordered_set<uint32_t> claimed;
    auto geometryWork = [&](int worker) {
      YieldToGame();
      ConvertIO io = makeIO(worker, geometryFolder);
      // A texture never takes a geometry model's id either.
      io.retailId = [&](uint32_t id) { return retail.HasId(id) || modelIds.count(id) != 0; };
      io.claim = [&](uint32_t id) {
        std::lock_guard<std::mutex> lock(claimMutex);
        return claimed.insert(id).second;
      };
      Converter converter(std::move(io));
      for (size_t i = nextModel++; i < geometry.size() && !sCancel; i = nextModel++) {
        ConvertOptions options;
        options.retail = geometry[i].id;
        options.source = UuidText(geometry[i].uuid);
        options.standalone = true;
        options.lightmapUv = true;  // (the LOD levels below reuse these options)
        // The list drops a character's simplified meshes by name; a room has none, and its
        // stone is named "simple".
        options.skip.clear();
        options.nativeMax = kGeometryTexture;
        options.joint = geometry[i].joint;
        options.hasLava = geometry[i].liquid >= 0;
        if (options.hasLava) {
          std::copy(std::begin(liquids[size_t(geometry[i].liquid)].lava),
                    std::end(liquids[size_t(geometry[i].liquid)].lava), options.lava);
        }
        std::string modelError;
        bool ok = false;
        // As for the models: an exception here would terminate the game.
        try {
          std::vector<uint8_t> raw;
          Model model;
          ok = remastered.ReadModel(geometry[i].uuid, raw, modelError) &&
               ParseModel(raw.data(), raw.size(), model, modelError) && converter.Convert(model, options, modelError);
          // Its coarser levels, for the distance; a level that fails ends the list there.
          PortRoomGeo::Lods lods;
          lods.model = geometry[i].id;
          for (const auto& [level, distanceSq] : ok && geometry[i].liquid < 0 && geometry[i].joint < 0
                                                     ? CoarserLevels(model)
                                                                              : std::vector<std::pair<int, float>>()) {
            options.lod = level;
            options.retail = geometry[i].lods[size_t(level)];
            std::string levelError;
            if (!converter.Convert(model, options, levelError)) {
              char name[16];
              std::snprintf(name, sizeof(name), "%08X", geometry[i].id);
              AddLine(std::string("room model ") + name + " level " + std::to_string(level) + ": " + levelError);
              break;
            }
            lods.levels.push_back({distanceSq, options.retail});
          }
          if (!lods.levels.empty()) {
            lodsDone += int(lods.levels.size());
            std::lock_guard<std::mutex> lock(lodMutex);
            lodTable.push_back(std::move(lods));
          }
        } catch (const std::exception& e) {
          modelError = e.what();
        }
        if (ok) {
          ++geometryDone;
        } else {
          char name[16];
          std::snprintf(name, sizeof(name), "%08X", geometry[i].id);
          AddLine(std::string("room model ") + name + ": " + modelError);
        }
        SetMessage("Converting room models (" + std::to_string(++seen) + "/" + std::to_string(geometry.size()) + ")");
      }
    };
    workers.clear();
    // A worker here holds one model, not a world's, so the phone's two-worker cap (debug_ui's
    // StartRemasteredImport) doesn't apply: 16 workers add ~1.3 GB on desktop, the most of it the
    // shared texture cache.
    const int geometryThreads = std::max(threads, DefaultThreads());
    for (int i = 1; i < geometryThreads; ++i) {
      workers.emplace_back(geometryWork, i);
    }
    geometryWork(0);
    for (std::thread& worker : workers) {
      worker.join();
    }
    if (sCancel) {
      fail("Cancelled.");
      return;
    }
    if (!lodTable.empty()) {
      std::sort(lodTable.begin(), lodTable.end(),
                [](const PortRoomGeo::Lods& a, const PortRoomGeo::Lods& b) { return a.model < b.model; });
      if (!writeFile(geometryFolder / PortRoomGeo::kLodFileName, PortRoomGeo::WriteLods(lodTable))) {
        AddLine(std::string(PortRoomGeo::kLodFileName) + ": cannot be written");
      }
    }
    made["roommodels"].counts["models"] = geometryDone.load();
    made["roommodels"].counts["levels"] = lodsDone.load();
    writeReportPart("roommodels", false);
    endStage();
  }
  if (geometry.empty()) {
    fs::remove(geometryFolder, ec);
  }

  // The strings Remastered reworded, and its European translations, as the
  // disc's tables with those strings changed and the languages added.
  int textTables = 0;
  int textStrings = 0;
  int textTranslated = 0;
  if (WantsText() && beginStage("text", std::to_string(ImportStage::kText), false)) {
    textTables = int(made["text"].counts["tables"]);
    textStrings = int(made["text"].counts["strings"]);
    textTranslated = int(made["text"].counts["translated"]);
  } else if (WantsText()) {
    SetMessage("Writing the text");
    std::vector<const char*> languages{kRemasteredEnglish};
    for (size_t k = 0; k < kTextLanguageCount; ++k) {
      languages.push_back(kTextLanguages[k].code);
    }
    std::map<uint32_t, TableText> tables;
    for (const ModelUuid& id : remastered.Texts()) {
      std::vector<uint8_t> raw;
      std::string textError;
      if (!remastered.ReadText(id, raw, textError)) {
        AddLine("text " + IdToString(id) + ": " + textError);
        continue;
      }
      for (const char* language : languages) {
        std::vector<TextEntry> entries;
        if (!ParseMsbt(raw.data(), raw.size(), language, entries, textError)) {
          if (language == kRemasteredEnglish) {
            AddLine("text " + IdToString(id) + ": " + textError);
            break;
          }
          continue;
        }
        for (TextEntry& entry : entries) {
          uint32_t strg = 0;
          uint32_t index = 0;
          std::string name;
          if (SplitTextLabel(entry.label, strg, index)) {
            tables[strg].byIndex[index][language] = std::move(entry.text);
          } else if (SplitNamedLabel(entry.label, strg, name)) {
            tables[strg].byName[name][language] = std::move(entry.text);
          }
        }
      }
    }
    const fs::path textFolder = staging / kTextFolder;
    fs::create_directories(textFolder, ec);
    // Remastered's indices are 1.00's; another version's tables moved or rewrote some strings.
    const bool checkWording = PortDisc::Current() != PortDisc::Version::Usa100;
    for (const auto& [strg, strings] : tables) {
      std::vector<uint8_t> original;
      std::vector<uint8_t> merged;
      int reworded = 0;
      int translated = 0;
      if (!retail.Read(kSTRG, strg, original) ||
          !MergeStringTable(original.data(), original.size(), strings, checkWording, merged, reworded, translated)) {
        continue;  // not on this disc, or worded as it was
      }
      char name[16];
      std::snprintf(name, sizeof(name), "%08X.STRG", strg);
      if (!writeFile(textFolder / name, merged)) {
        AddLine(std::string(name) + ": cannot write");
        continue;
      }
      ++textTables;
      textStrings += reworded;
      textTranslated += translated;
    }
    if (textTables == 0) {
      fs::remove(textFolder, ec);
    }
    made["text"].counts["tables"] = textTables;
    made["text"].counts["strings"] = textStrings;
    made["text"].counts["translated"] = textTranslated;
    endStage();
  }
  // Remastered's typeface, which the port draws the disc's text with.
  bool fontWritten = false;
  if (beginStage("font", std::to_string(ImportStage::kText) + keyConverter, false)) {
    fontWritten = made["font"].counts["written"] != 0;
  } else {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> out;
    ModelUuid atlas{};
    PortHdFont::Font font;
    TxtrImage image;
    std::string fontError = "not in the image";
    // The one with the most characters, so that every language's text is covered.
    for (const ModelUuid& id : remastered.Fonts()) {
      ModelUuid candidateAtlas{};
      PortHdFont::Font candidate;
      std::string candidateError;
      if (!remastered.ReadFont(id, raw, candidateError) ||
          !ParseFont(raw.data(), raw.size(), candidateAtlas, candidate, candidateError)) {
        fontError = candidateError;
      } else if (candidate.glyphs.size() > font.glyphs.size()) {
        font = std::move(candidate);
        atlas = candidateAtlas;
      }
    }
    if (font.glyphs.empty() || !remastered.ReadTexture(atlas, raw, fontError) ||
        !DecodeTxtr(raw.data(), raw.size(), image, fontError)) {
      AddLine("font: " + fontError);
    } else if (!SetFontAtlas(font, image.width, image.height, image.rgba.data(), image.rgba.size()) ||
               !PortHdFont::WriteFont(font, out)) {
      AddLine("font: its texture is not usable");
    } else {
      const fs::path fontFolder = staging / kFontFolder;
      fs::create_directories(fontFolder, ec);
      fontWritten = writeFile(fontFolder / kFontName, out);
      if (!fontWritten) {
        AddLine("font: cannot write");
      }
    }
    made["font"].counts["written"] = fontWritten ? 1 : 0;
    endStage();
    if (!fontWritten) {
      made.erase("font");
    }
  }
  // Remastered's HUD: the disc's frames laid out and drawn as its own.
  int hudFrames = 0;
  const bool hudReused = WantsHud() && !sCancel && beginStage("hud", std::to_string(ImportStage::kHud) + keyConverter, false);
  if (hudReused) {
    hudFrames = int(made["hud"].counts["frames"]);
  }
  if (WantsHud() && !sCancel && !hudReused) {
    SetMessage("Converting the HUD");
    const fs::path hudFolder = staging / kHudFolder;
    fs::create_directories(hudFolder, ec);
    HudConverter converter(makeIO(0, hudFolder));
    HudCounts counts;
    {
      // Remastered colours each beam's icon in the beam menu.
      std::vector<uint8_t> tweak;
      std::map<std::string, HudConverter::Tint> tints;
      std::string tweakError;
      if (remastered.ReadTweak("TweakGuiColorsMP1", tweak, tweakError) &&
          HudBeamIconTints(tweak.data(), tweak.size(), tints, tweakError)) {
        converter.SetTints(std::move(tints));
      } else {
        AddLine("TweakGuiColorsMP1: " + tweakError + "; the beam icons are left white");
      }
    }
    for (const HudFrame& frame : HudFrames()) {
      std::vector<uint8_t> raw;
      std::vector<uint8_t> rawModel;
      ModelUuid modelId{};
      Model model;
      std::string hudError;
      if (!remastered.ReadFrame(frame.name, raw, hudError) ||
          !(HudFrameModel(raw.data(), raw.size(), modelId) || (hudError = "not a frame", false)) ||
          !remastered.ReadModel(modelId, rawModel, hudError) ||
          !ParseModel(rawModel.data(), rawModel.size(), model, hudError) ||
          !converter.Convert(frame.retail, raw.data(), raw.size(), model, counts, hudError)) {
        AddLine(std::string(frame.name) + ": " + hudError);
        continue;
      }
      ++hudFrames;
    }
    // The map screen's compass, which the game draws itself (port_map_icons.h).
    int compassModels = 0;
    for (const auto& [name, id] : {std::pair<const char*, uint32_t>{"CMDL_MapCompassShell", PortMapIcons::kCompassShell},
                                   std::pair<const char*, uint32_t>{"CMDL_MapCompass", PortMapIcons::kCompassNeedle}}) {
      std::vector<uint8_t> raw;
      Model model;
      std::string compassError;
      if (!remastered.ReadModelNamed(name, raw, compassError) ||
          !ParseModel(raw.data(), raw.size(), model, compassError) ||
          !converter.ConvertModel(model, id, counts, compassError)) {
        AddLine(std::string(name) + ": " + compassError);
        continue;
      }
      ++compassModels;
    }
    if (hudFrames == 0 && compassModels == 0) {
      fs::remove_all(hudFolder, ec);
    }
  }
  // Remastered's map icons, where the game looks for the disc's, and the rooms
  // whose map it reshaped.
  if (WantsHud() && !sCancel && !hudReused) {
    const fs::path mapFolder = staging / kMapFolder;
    fs::create_directories(mapFolder, ec);
    ConvertIO io = makeIO(0, mapFolder);
    int icons = 0;
    for (const MapIcon& icon : MapIcons()) {
      std::vector<uint8_t> raw;
      TxtrImage decoded;
      std::string iconError;
      if (!remastered.ReadTextureNamed(icon.name, raw, iconError) ||
          !DecodeTxtr(raw.data(), raw.size(), decoded, iconError)) {
        AddLine(std::string(icon.name) + ": " + iconError);
        continue;
      }
      Image image;
      image.width = int(decoded.width);
      image.height = int(decoded.height);
      image.rgba = std::move(decoded.rgba);
      char name[32];
      std::snprintf(name, sizeof(name), "%08X.TXTR", icon.id);
      const std::vector<uint8_t> txtr = EncodeMapIcon(image);
      if (txtr.empty() || !io.write(name, txtr)) {
        AddLine(std::string(icon.name) + ": cannot write");
        continue;
      }
      ++icons;
    }
    MapIO mapIO;
    mapIO.retail = io.retail;
    mapIO.write = io.write;
    mapIO.log = [](const std::string& line) { AddLine(line); };
    for (const MapWorld& world : MapWorlds()) {
      if (sCancel) {
        break;
      }
      std::vector<uint8_t> raw;
      std::string mapError;
      if (!remastered.ReadMap(world.name, raw, mapError) ||
          !WriteWorldMapAreas(world.mlvl, raw.data(), raw.size(), mapIO, icons, mapError)) {
        AddLine(std::string(world.name) + ": " + mapError);
      }
    }
    if (icons == 0) {
      fs::remove_all(mapFolder, ec);
    }
    made["hud"].counts["frames"] = hudFrames;
    endStage();
  }
  // Remastered's menu movies.
  int movies = 0;
  bool noFfmpeg = false;
  if (MovieFormat format; WantsMovies(format) && !sCancel) {
    char key[64];
    std::snprintf(key, sizeof(key), "%d %dx%d %d", ImportStage::kMovies, format.width, format.height, format.fps);
    if (beginStage("movies", key, false)) {
      movies = int(made["movies"].counts["movies"]);
    } else {
      movies = ImportMovies(remastered, staging / kMovieFolder, format, noFfmpeg);
      recordFolder(staging / kMovieFolder);
      made["movies"].counts["movies"] = movies;
      endStage();
      // Without ffmpeg, or with a movie missing, there is nothing to keep: the next import tries again.
      size_t wanted = 0;
      for (const Movie& movie : Movies()) {
        wanted += movie.names.size();
      }
      if (noFfmpeg || size_t(movies) < wanted || sCancel) {
        made.erase("movies");
      }
    }
  }
  // The Extras gallery's concept art.
  int gallery = 0;
  if (WantsGallery() && !sCancel) {
    if (beginStage("gallery", std::to_string(ImportStage::kGallery) + keyConverter, false)) {
      gallery = int(made["gallery"].counts["images"]);
    } else {
      gallery = ImportGallery(remastered, staging / kGalleryFolder, threads);
      recordFolder(staging / kGalleryFolder);
      made["gallery"].counts["images"] = gallery;
      endStage();
      if (gallery == 0) {
        made.erase("gallery");
      }
    }
  }
  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  // The converter reports, merged from the stages' parts (reused stages' are linked in).
  {
    const fs::path partFolder = staging / "reports" / "parts";
    auto slurp = [](const fs::path& path) {
      std::ifstream in(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    std::vector<std::string> materialRows;
    std::vector<std::string> effectRows;
    std::error_code reportError;
    for (fs::directory_iterator it(partFolder, reportError), end; !reportError && it != end; it.increment(reportError)) {
      const std::string name = it->path().filename().string();
      const auto rows = ReportRows(slurp(it->path()));
      auto& into = name.find(".materials.") != std::string::npos ? materialRows : effectRows;
      into.insert(into.end(), rows.begin(), rows.end());
    }
    if (!materialRows.empty() || !effectRows.empty()) {
      const std::string materials = JoinReport(MaterialReportHeader(), std::move(materialRows));
      const std::string effects = JoinReport(EffectReportHeader(), std::move(effectRows));
      const std::string summary = SummarizeReports(materials, effects);
      const fs::path folder = staging / "reports";
      for (const auto& [file, text] : {std::pair<const char*, const std::string&>{"materials.tsv", materials},
                                       {"effects.tsv", effects},
                                       {"summary.txt", summary}}) {
        if (!WriteReplacing(folder / file, std::vector<uint8_t>(text.begin(), text.end()))) {
          AddLine(std::string("reports/") + file + ": cannot be written");
        }
      }
    }
  }
  // Every file a stage lists must be there before the import is published: an install whose
  // manifest names files that are gone would load as a mod with missing resources.
  {
    size_t missing = 0;
    std::string first;
    std::error_code checkError;
    for (const auto& [stageName, stage] : made) {
      for (const std::string& file : stage.files) {
        if (!fs::is_regular_file(staging / PathFromString(file), checkError)) {
          if (missing++ == 0) {
            first = stageName + ": " + file;
          }
        }
      }
    }
    if (missing != 0) {
      std::fprintf(stderr, "remastered import: %zu listed file(s) are missing from the staging folder (first: %s)\n",
                   missing, first.c_str());
      fail("The import is incomplete (" + std::to_string(missing) + " files missing, first " + first +
           "); the installed models are kept.");
      return;
    }
  }
  // What each stage made, for the next import to reuse. Without it the next one is a full import.
  {
    const fs::path manifest = staging / kManifestName;
    fs::path tmp = manifest;
    tmp += ".tmp";
    std::error_code manifestError;
    StageRecord& textures = made[kTextureStage];
    textures.key = keptKey;
    textureMemory.Save(staging, textures);
    if (!WriteManifest(tmp, made) || (fs::rename(tmp, manifest, manifestError), manifestError)) {
      fs::remove(tmp, manifestError);
      AddLine("import manifest: cannot be written, so the next import reconverts everything");
    }
  }
  {
    // Only a full import stamps: the movies-only run leaves an older mod's stamp as it was.
    std::ofstream stamp(staging / kImportStampName);
    stamp << kImportVersion << '\n' << MP_BUILD_REVISION << '\n';
    stamp.close();
    if (!stamp) {
      fail("Cannot write to the mod folder.");
      return;
    }
  }
  {
    std::ofstream marker(staging / kMarkerName);
    marker.close();
    if (!marker) {
      fail("Cannot write to the mod folder.");
      return;
    }
  }
  if (fromHeld) {
    fs::remove_all(held, ec);
  }
  const int failed = int(count) - converted.load();
  std::printf("remastered import: %s\n", textures.Summary().c_str());
  std::string message = std::to_string(converted.load()) + " models converted";
  if (failed != 0) {
    message += ", " + std::to_string(failed) + " failed";
  }
  message += ", " + std::to_string(roomFiles.load()) + " room environments";
  if (!geometry.empty()) {
    message += ", " + std::to_string(geometryDone.load()) + " of " + std::to_string(geometry.size()) + " room models (" +
               std::to_string(lodsDone.load()) + " coarser levels)";
  }
  if (textTables != 0) {
    message += ", " + std::to_string(textStrings) + " strings in " + std::to_string(textTables) + " text tables";
    if (textTranslated != 0) {
      message += " (" + std::to_string(textTranslated) + " translated)";
    }
  }
  if (fontWritten) {
    message += ", the font";
  }
  if (hudFrames != 0) {
    message += ", " + std::to_string(hudFrames) + " HUD frames";
  }
  if (movies != 0) {
    message += ", " + std::to_string(movies) + " movies";
  }
  if (gallery != 0) {
    message += ", " + std::to_string(gallery) + " gallery pictures";
  }
  if (!reused.empty()) {
    message += ". Unchanged since the last import, so reused:";
    for (size_t i = 0; i < reused.size(); ++i) {
      message += (i == 0 ? " " : ", ") + reused[i];
    }
  }
  Finish(true, message + (noFfmpeg ? ". Movies skipped: ffmpeg not found." : "."));
}

}  // namespace

std::string DefaultKeysPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    home = std::getenv("USERPROFILE");
  }
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  const fs::path path = PathFromString(home) / ".switch" / "prod.keys";
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return {};
  }
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool StartMovieImport(const std::string& nspPath, const std::string& keysPath) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  // A finished import that has not been moved into place yet is the newer mod.
  const fs::path staging = StagingFolder();
  std::error_code ec;
  fs::path mod;
  if (!staging.empty()) {
    mod = fs::exists(staging / kMarkerName, ec) ? staging : staging.parent_path() / kImportModName;
  }
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mod.empty() || !fs::is_directory(mod, ec)) {
    sState.finished = true;
    sState.message = "Import the models first: the movies go into that mod.";
    return false;
  }
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(RunMovies, nspPath, keysPath, mod);
  return true;
}

bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const fs::path staging = StagingFolder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (staging.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  if (threads <= 0) {
    threads = DefaultThreads();
  }
#if defined(__ANDROID__)
  threads = std::min(threads, 3);
#endif
  // The device exists in the game and not on the command line, where this
  // stays at its default of BC.
  bool bc = false, astc = false;
  aurora_get_texture_support(&bc, &astc);
  SetGpuTextureSupport(bc, astc);
  std::fprintf(stderr, "remastered: writing %s textures\n", TextureFormatName());
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(Run, nspPath, keysPath, threads, staging);
  return true;
}

void SetImportGeometry(bool on) { sGeometry = on; }

void SetImportReuse(bool on) { sReuse = on; }

ImportState ImportStatus() {
  std::lock_guard<std::mutex> lock(sStateMutex);
  return sState;
}

void CancelImport() { sCancel = true; }

void StopImport() {
  sCancel = true;
  if (sThread.joinable()) {
    sThread.join();
  }
}

bool ApplyPendingImport() {
  const fs::path staging = StagingFolder();
  std::error_code ec;
  // A running import is still writing there.
  if (staging.empty() || ImportStatus().running) {
    return false;
  }
  const fs::path held = HeldFolder(staging);
  if (fs::exists(staging / kMarkerName, ec)) {
    // An older import the finished one was made from, left if the game quit just after it.
    fs::remove_all(held, ec);
  } else {
    fs::remove_all(staging, ec);
    // A finished import held aside by one that never finished (the game quit during it).
    if (!fs::exists(held / kMarkerName, ec)) {
      fs::remove_all(held, ec);
      return false;
    }
    fs::rename(held, staging, ec);
    if (ec) {
      return false;
    }
  }
  const fs::path target = staging.parent_path() / kImportModName;
  // The working mod is kept as a backup until the new one is in place. Hidden, so the mod loader
  // never picks it up as a second mod if a crash leaves it behind.
  const fs::path backup = staging.parent_path() / (std::string(".") + kImportModName + ".old");
  const bool hadTarget = fs::exists(target, ec);
  if (hadTarget) {
    fs::remove_all(backup, ec);
    fs::rename(target, backup, ec);
    if (ec) {
      std::fprintf(stderr, "metroid_prime_port: could not set aside %s: %s\n", target.string().c_str(),
                   ec.message().c_str());
      return false;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    std::fprintf(stderr, "metroid_prime_port: could not move the imported mod to %s: %s\n",
                 target.string().c_str(), ec.message().c_str());
    if (hadTarget) {
      std::error_code restoreError;
      fs::rename(backup, target, restoreError);
      if (restoreError) {
        std::fprintf(stderr, "metroid_prime_port: could not restore %s: %s\n", target.string().c_str(),
                     restoreError.message().c_str());
      }
    }
    return false;
  }
  if (hadTarget) {
    fs::remove_all(backup, ec);
  }
  fs::remove(target / kMarkerName, ec);
  return true;
}

int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly) {
  std::string keys = keysPath.empty() ? DefaultKeysPath() : keysPath;
  if (keys.empty()) {
    std::fprintf(stderr, "no key file given and no ~/.switch/prod.keys\n");
    return 2;
  }
  if (moviesOnly ? !StartMovieImport(nspPath, keys)
                 : !StartImport(nspPath, keys, int(std::max(1u, std::thread::hardware_concurrency())))) {
    std::fprintf(stderr, "%s\n", ImportStatus().message.c_str());
    return 1;
  }
  std::string shown;
  for (;;) {
    const ImportState state = ImportStatus();
    if (state.message != shown) {
      shown = state.message;
      std::printf("%s\n", shown.c_str());
      std::fflush(stdout);
    }
    if (!state.running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StopImport();
  const ImportState state = ImportStatus();
  for (const std::string& line : state.lines) {
    std::printf("  %s\n", line.c_str());
  }
  if (!state.ok) {
    return 1;
  }
  if (moviesOnly) {
    return 0;
  }
  if (!ApplyPendingImport()) {
    std::fprintf(stderr, "could not move the mod into %s\n", PortMods::Folder().c_str());
    return 1;
  }
  std::printf("installed as %s/%s\n", PortMods::Folder().c_str(), kImportModName);
  return 0;
}

}  // namespace PortRemastered
