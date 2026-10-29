#include "port_remastered_import_internal.h"
#include "port_remastered_effect_convert.h"
#include "port_queen_barrier_validate.h"
#include "port_envfx_file.h"
#include "port_icy_file.h"
#include "port_shell_file.h"
#include "port_retail_placement.h"
#include "port_dread_reskin.h"
#include "port_remastered_anim.h"

namespace PortRemastered {
namespace ImportDetail {

// The import's own folders, staged in the root port_remastered_storage.h works out.
constexpr const char* kStagingName = PortRemasteredStorage::kStagingName;
constexpr const char* kTrashPrefix = PortRemasteredStorage::kTrashPrefix;
// Written last, so a staging folder without it is an import that was cut short.
constexpr const char* kMarkerName = "import-complete";
constexpr const char* kRoomFolder = "roomenv";
constexpr const char* kGeometryFolder = "roomgeo";
constexpr const char* kFontFolder = "font";
constexpr const char* kFontName = "deface.sdfont";
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

// What the panel leaves out (SetImportParts). Read by the import thread, set
// only while no import runs.
std::mutex sPartsMutex;
ImportParts sParts;

ImportParts Parts() {
  std::lock_guard<std::mutex> lock(sPartsMutex);
  return sParts;
}

// The rooms whose geometry is imported, from MP_REMASTERED_GEOMETRY: "all", or
// room names (any part of one) separated by commas, or "none". Without it, all;
// "none" when the panel leaves the geometry out.
std::string GeometryChoice() {
  if (!Parts().geometry) {
    return "none";
  }
  const char* env = port::EnvString("MP_REMASTERED_GEOMETRY");
  return env == nullptr || env[0] == '\0' ? std::string("all") : std::string(env);
}

bool WantsGeometry(const std::string& room) {
  const std::string list = GeometryChoice();
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

std::string PathString(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

void SetMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.message = message;
}

// The steps of a full import in the order it makes them, for the panel's progress bar: reading
// the image, then the stages (beginStage's names).
const char* const kSteps[] = {"disc", "models", "effects", "rooms", "roommodels",
                              "text", "font",   "hud",     "movies", "gallery"};

void SetStep(const std::string& name) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  for (size_t i = 0; i < std::size(kSteps); ++i) {
    if (name == kSteps[i]) {
      sState.step = int(i + 1);
      sState.steps = int(std::size(kSteps));
      sState.stepDone = 0;
      sState.stepTotal = 0;
      return;
    }
  }
}

void SetStepProgress(size_t done, size_t total) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.stepDone = int(done);
  sState.stepTotal = int(total);
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
  return Parts().text && port::EnvFlag("MP_REMASTERED_TEXT", true);
}

// MP_REMASTERED_HUD=0 leaves the disc's HUD alone.
bool WantsHud() {
  return Parts().hud && port::EnvFlag("MP_REMASTERED_HUD", true);
}

// MP_REMASTERED_GALLERY=0 leaves the Extras gallery out.
bool WantsGallery() {
  return Parts().gallery && port::EnvFlag("MP_REMASTERED_GALLERY", true);
}

// MP_REMASTERED_MOVIES=0 leaves the disc's movies alone; a size and rate
// ("1280x720@30") is what they are written as.
// Set to an off value (port_env.h's: 0, false, off, no); unset or empty keeps them.
bool MoviesTurnedOff() {
  const char* env = port::EnvString("MP_REMASTERED_MOVIES");
  return env != nullptr && env[0] != '\0' && !port::EnvFlag("MP_REMASTERED_MOVIES");
}

bool WantsMovies(MovieFormat& format) {
  if (!Parts().movies || MoviesTurnedOff()) {
    return false;
  }
  const char* env = port::EnvString("MP_REMASTERED_MOVIES");
  if (env == nullptr || env[0] == '\0' || std::strcmp(env, "1") == 0) {
    return true;
  }
  if (!ParseMovieFormat(env, format)) {
    AddLine(std::string("MP_REMASTERED_MOVIES: \"") + env + "\" is not a size and rate like 1280x720@30");
  }
  return true;
}

// --- Reusing the previous import ----------------------------------------------

// Set from the Mods panel for the next import; not saved, so it lasts until the game is closed.
std::atomic<bool> sRemakeAll{false};

// Off with MP_REMASTERED_REUSE=0 or the panel's "Full import (no reuse)". With
// MP_REMASTERED_REUSE=textures only the converted textures are reused (TextureMemory).
bool WantsReuse() { return !sRemakeAll && port::EnvFlag("MP_REMASTERED_REUSE", true); }

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

uint64_t Hash64(const std::string& text) { return port::Fnv1a64(text); }

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

// A GUID string as the bytes of a ModelUuid, in the order written, else the mixed-endian one
// the pak may hold it in.
void GuidUuids(const char* text, ModelUuid& plain, ModelUuid& swapped) {
  size_t n = 0;
  for (const char* c = text; *c != 0 && n < 16; ++c) {
    if (*c == '-') {
      continue;
    }
    const auto hex = [](char h) { return h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10; };
    plain[n++] = uint8_t(hex(c[0]) << 4 | hex(c[1]));
    ++c;
  }
  swapped = plain;
  std::swap(swapped[0], swapped[3]);
  std::swap(swapped[1], swapped[2]);
  std::swap(swapped[4], swapped[5]);
  std::swap(swapped[6], swapped[7]);
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
    std::snprintf(buffer, sizeof(buffer), "%08X %08X %d %d %d %08X|", entry.retail, entry.ancs, int(entry.key),
                  int(entry.pbr), int(entry.options), entry.character);
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

// --- The import ------------------------------------------------------------------

fs::path StagingFolder() {
  const std::string root = ImportRootFolder();
  return root.empty() ? fs::path() : PathFromString(root) / kStagingName;
}

#if defined(__ANDROID__)
// The app's own storage, where Android keeps the built-in import (SDL's answer is the folder the
// APK's private files live in).
std::string InternalStoragePath() {
  const char* storage = SDL_GetAndroidInternalStoragePath();
  return storage != nullptr ? storage : std::string();
}
#endif

// The shared mods folder's own import from before Android moved the import into the app, or from a
// first private import that has none: read-only, and empty when the mods folder is the root itself,
// where the import already is that folder's.
std::string LegacySharedImport() {
  const std::string root = ImportRootFolder();
  const std::string mods = PortMods::Folder();
  if (root.empty() || mods.empty() ||
      PortRemasteredStorage::WithSeparator(mods) == root) {
    return {};
  }
  std::error_code ec;
  const fs::path legacy = PathFromString(mods) / kImportModName;
  return fs::is_directory(legacy, ec) ? PathString(legacy) : std::string();
}

// Where a finished import that is not yet in place waits while the next one reuses it.
fs::path HeldFolder(const fs::path& staging) {
  return staging.parent_path() / PortRemasteredStorage::kHeldName;
}

// Folders on their way out (kTrashPrefix): an old import can be most of a gigabyte,
// too much to delete while the game waits on a mods reload. Renamed to a hidden name
// (the mod loader skips it) at once and deleted on a thread; one a quit interrupted
// is picked up by the next sweep.
std::mutex sTrashMutex;
bool sTrashSweeping = false;
bool sTrashAgain = false;

void SweepTrash(const fs::path& mods) {
  {
    std::lock_guard<std::mutex> lock(sTrashMutex);
    if (sTrashSweeping) {
      sTrashAgain = true;
      return;
    }
    sTrashSweeping = true;
  }
  std::thread([mods] {
    for (;;) {
      std::error_code ec;
      std::vector<fs::path> trash;
      for (fs::directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(kTrashPrefix, 0) == 0) {
          trash.push_back(it->path());
        }
      }
      for (const fs::path& dir : trash) {
        std::error_code removeError;
        fs::remove_all(dir, removeError);
        if (removeError) {
          PortLog::Write("metroid_prime_port: could not delete trash %s: %s\n", dir.string().c_str(),
                         removeError.message().c_str());
        } else {
          PortLog::Write("metroid_prime_port: deleted trash %s\n", dir.filename().string().c_str());
        }
      }
      std::lock_guard<std::mutex> lock(sTrashMutex);
      if (!sTrashAgain) {
        sTrashSweeping = false;
        return;
      }
      sTrashAgain = false;
    }
  }).detach();
}

// Takes `dir` out of the way now and deletes it later. Deletes it here only when
// it cannot be renamed.
void Discard(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return;
  }
  static std::atomic<unsigned> sCount{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path trash =
      dir.parent_path() / (kTrashPrefix + std::to_string(stamp) + "-" + std::to_string(sCount++));
  fs::rename(dir, trash, ec);
  if (ec) {
    PortLog::Write("metroid_prime_port: could not set aside %s for deletion: %s\n",
                   dir.string().c_str(), ec.message().c_str());
    fs::remove_all(dir, ec);
    if (ec) {
      PortLog::Write("metroid_prime_port: could not delete %s: %s\n", dir.string().c_str(),
                     ec.message().c_str());
    }
    return;
  }
  SweepTrash(dir.parent_path());
}

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
    SetStepProgress(i, movies.size());
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

// A plain 2D BC6H texture as the .dds the converter writes for it (ConvertIO::hdrDds): under BC
// its blocks as they are, from the first mip within maxEdge; without BC (ASTC phones) RGBA16F,
// at most 2048 on its longest edge, each level decoded from the source's own mip and any the
// file lacks halved from the last. Linear either way, as BC6H is.
bool EncodeHdrDds(const TxtrMipsBc6h& tex, int maxEdge, TextureFormat format, std::vector<uint8_t>& dds,
                  std::string& error) {
  uint32_t limit = maxEdge > 0 ? uint32_t(maxEdge) : 0xFFFFFFFFu;
  if (format != TextureFormat::BC) {
    limit = std::min(limit, 2048u);
  }
  size_t start = 0;
  while (start + 1 < tex.mips.size() && std::max(tex.width >> start, tex.height >> start) > limit) {
    ++start;
  }
  const uint32_t w = std::max(tex.width >> start, 1u), h = std::max(tex.height >> start, 1u);
  if (w % 4 != 0 || h % 4 != 0) {
    error = std::to_string(w) + "x" + std::to_string(h) + " is not whole blocks";
    return false;
  }
  if (format == TextureFormat::BC) {
    const std::vector<std::vector<uint8_t>> mips(tex.mips.begin() + std::ptrdiff_t(start), tex.mips.end());
    dds = EncodeDdsBc6h(w, h, tex.isSigned, mips);
  } else {
    std::vector<std::vector<uint16_t>> mips;
    for (uint32_t m = 0;; ++m) {
      const uint32_t mw = std::max(w >> m, 1u), mh = std::max(h >> m, 1u);
      if (start + m < tex.mips.size()) {
        mips.emplace_back(size_t(mw) * mh * 4);
        DecodeBc6hImage(tex.mips[start + m].data(), mw, mh, tex.isSigned, mips.back().data());
      } else {
        mips.push_back(HalveRgba16f(mips.back(), std::max(w >> (m - 1), 1u), std::max(h >> (m - 1), 1u)));
      }
      if (mw == 1 && mh == 1) {
        break;
      }
    }
    dds = EncodeDdsRgba16f(w, h, mips);
  }
  if (dds.empty()) {
    error = "the mips do not match the texture's size";
    return false;
  }
  return true;
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
  fs::path folder = target;
  folder += ".new";
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  std::atomic<int> written{0};
  const auto convert = [&](size_t position, const std::vector<uint8_t>& raw) {
    char name[16];
    std::snprintf(name, sizeof(name), "%03d.jpg", int(position));
    std::string error;
    TxtrImage image;
    std::vector<uint8_t> jpeg;
    bool ok = false;
    // On a worker thread: an exception out of here (bad_alloc on a huge
    // picture) would end the program, as the other stages' workers guard.
    try {
      ok = DecodeTxtr(raw.data(), raw.size(), image, error);
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
    } catch (const std::exception& e) {
      ok = false;
      error = e.what();
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
    PortLog::Write("gallery: cannot move the new folder into place: %s\n", ec.message().c_str());
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

using port::Hex8;

// A retail effect id that two projectiles reference gives each its own Remastered effect: the
// first claimant (in WPSM name, then field order) keeps the disc's id, any other with a different
// effect gets a synthetic id and an entry in the table the game reads (effectids.map: owner WPSC,
// field, retail id, synthetic id; PortMods::RemapEffectId).
struct EffectIdRemap {
  uint32_t owner;
  uint32_t field;  // the WPSC field's fourcc
  uint32_t retail;
  uint32_t synthetic;
};

// A swoosh Remastered's WPSM has in a field the disc's WPSC lacks (the plasma shots' ASW1/ASW2): the
// SWSH is imported under `synthetic` (SyntheticEffectId of "<name> <field>", lower case) and the WPSC
// is written into the mod with the field set to it (AddRetailLink), once the SWHC is written.
struct WpscSwoosh {
  uint32_t wpsc;
  char field[5];
  uint32_t synthetic;
  std::string who;
};

std::vector<EffectPairing> ProjectilePairings(const Remastered& remastered, Retail& retail,
                                              std::vector<std::string>& lines,
                                              std::vector<EffectIdRemap>& remaps,
                                              std::vector<WpscSwoosh>& additions) {
  struct Claim {
    uint32_t owner;
    EffectGuid guid;
    std::string who;
  };
  std::map<uint32_t, Claim> claims;  // by disc id
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
          discId = port::GetBE32(weapon, i + 8);
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
      const bool discNone = discId == 0 || discId == 0xFFFFFFFFu;
      if (found && discNone && field.type == kSWSH) {
        std::string who = name + " " + field.tag;
        for (char& c : who) {
          c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        const uint32_t synthetic = SyntheticEffectId(who.c_str());
        if (retail.HasId(synthetic)) {
          lines.push_back("WPSM " + name + " " + field.tag + ": the disc has no such field, and " + Hex8(synthetic) +
                          " is a disc id");
          continue;
        }
        WpscSwoosh addition{wpsc, {}, synthetic, name + " " + field.tag};
        std::memcpy(addition.field, field.tag, 4);
        additions.push_back(std::move(addition));
        pairings.push_back({guid, synthetic, "wpsm " + name + " " + field.tag + " (added)"});
        lines.push_back("WPSM " + name + " " + field.tag + ": the disc has no such field; " +
                        EffectGuidString(ToStored(guid)) + " -> " + Hex8(synthetic) + " (" + who + "), added to the WPSC " +
                        Hex8(wpsc));
        continue;
      }
      if (!found || discNone || !retail.HasId(discId)) {
        if (found != !discNone) {
          lines.push_back("WPSM " + name + " " + field.tag + (found ? ": the disc has no such field" : ": the WPSM has no such field"));
        }
        continue;
      }
      const std::string who = name + " " + field.tag;
      uint32_t target = discId;
      const auto claim = claims.find(discId);
      if (claim == claims.end()) {
        claims.emplace(discId, Claim{wpsc, guid, who});
      } else if (claim->second.guid != guid) {
        // The effect's name, else (most of the projectile effects are unnamed) the claimant's.
        std::string effectName;
        if (!remastered.EffectName(guid, effectName)) {
          effectName = who;
          for (char& c : effectName) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
          }
        }
        const uint32_t synthetic = SyntheticEffectId(effectName.c_str());
        if (retail.HasId(synthetic)) {
          lines.push_back("WPSM " + who + ": " + Hex8(discId) + " is shared with " + claim->second.who +
                          ", no synthetic id for " + EffectGuidString(ToStored(guid)));
          continue;
        }
        lines.push_back("WPSM " + who + ": " + Hex8(discId) + " is shared with " + claim->second.who + ", so " +
                        Hex8(synthetic) + " (" + effectName + ")");
        target = synthetic;
        remaps.push_back({wpsc, port::FourCC(field.tag), discId, synthetic});
      }
      pairings.push_back({guid, target, "wpsm " + who});
      lines.push_back("WPSM " + who + ": " + EffectGuidString(ToStored(guid)) + " -> " + Hex8(target));
    }
  }
  // Effects the disc's PARTs/SWHCs name the same (SamGunFx.pak), that no WPSC reaches.
  struct Named {
    const char* name;
    uint32_t retail;
  };
  static const Named kNamed[] = {{"powerauxmuzzle", 0x2AED975B},
                                 {"bustermuzzle", 0},
                                 {"busterswoosh1", 0x869B8E14},
                                 {"busterswoosh2", 0x804E26D9},
                                 {"slowbluetailswoosh", 0x841ED1D0},
                                 {"slowbluetailswoosh2", 0x699EA052},
                                 {"jaggytrail", 0x5E11D659},
                                 {"spiderelectric", 0x4735D6E1},
                                 {"wave2ndelec2_1", 0x16871871},
                                 {"wave2ndelec2_2", 0x21D217FC}};
  for (const Named& named : kNamed) {
    ModelUuid id;
    if (named.retail != 0 && retail.HasId(named.retail) && remastered.EffectByName(named.name, id)) {
      pairings.push_back({id, named.retail, std::string("name ") + named.name});
      lines.push_back(std::string("name ") + named.name + ": " + EffectGuidString(ToStored(id)) + " -> " + Hex8(named.retail));
    }
  }
  // Effects the disc has no PART for are imported under an id from their name.
  for (const char* name : kSyntheticEffectNames) {
    ModelUuid id;
    const uint32_t synthetic = SyntheticEffectId(name);
    if (!retail.HasId(synthetic) && remastered.EffectByName(name, id)) {
      pairings.push_back({id, synthetic, std::string("synthetic ") + name});
      lines.push_back(std::string("synthetic ") + name + ": " + EffectGuidString(ToStored(id)) + " -> " + Hex8(synthetic));
    }
  }

  // Missile's collision-response default is a separate role from APSM/ASW:
  // WPSM COLR -> CRSM DEFS GENP, matching WPSC COLR -> CRSC DEFS PART. Pin
  // only the verified USA chain; any absent, ambiguous, region-mismatched, or
  // conflicting record leaves the disc's collision response untouched.
  uint32_t missileWpsc = 0;
  std::vector<uint8_t> missileWeapon;
  std::vector<uint8_t> missileProjectile;
  std::vector<uint8_t> missileRetailCollision;
  std::vector<uint8_t> missileRemasteredCollision;
  std::string missileError;
  if (!retail.WpscId("missile", missileWpsc)) {
    lines.push_back("WPSM missile COLR/DEFS: no unique disc WPSC named Missile");
  } else if (!retail.Read(kWPSC, missileWpsc, missileWeapon) ||
             !remastered.ReadProjectile("missile", missileProjectile, missileError)) {
    lines.push_back("WPSM missile COLR/DEFS: could not read the exact weapon assets");
  } else {
    const bool crscPresent = retail.HasUniqueResource(kCRSC, ImportDetail::kMissileCrsc) &&
                             retail.Read(kCRSC, ImportDetail::kMissileCrsc, missileRetailCollision);
    const bool crsmPresent = remastered.HasUniqueCollision(ImportDetail::kMissileCrsm) &&
                             remastered.ReadCollision(ImportDetail::kMissileCrsm, missileRemasteredCollision,
                                                      missileError);
    const bool genpPresent = remastered.HasUniqueEffect(ImportDetail::kMissileDefaultGenp) &&
                             remastered.EffectAssetType(ImportDetail::kMissileDefaultGenp) == kGENP;
    ImportDetail::MissileDefaultRoleInput input;
    input.weaponName = "missile";
    input.retailWpscId = missileWpsc;
    input.retailWpscExists = retail.HasUniqueResource(kWPSC, missileWpsc);
    input.retailCrscExists = crscPresent;
    input.retailPartExists = retail.HasUniqueResource(kPART, ImportDetail::kMissileDefaultPart);
    input.remasteredWpsmExists = true;  // ReadProjectile rejected missing/ambiguous names.
    input.remasteredCrsmExists = crsmPresent;
    input.remasteredGenpExists = genpPresent;
    input.retailWpsc = missileWeapon.data();
    input.retailWpscSize = missileWeapon.size();
    input.retailCrsc = missileRetailCollision.data();
    input.retailCrscSize = missileRetailCollision.size();
    input.remasteredWpsm = missileProjectile.data();
    input.remasteredWpsmSize = missileProjectile.size();
    input.remasteredCrsm = missileRemasteredCollision.data();
    input.remasteredCrsmSize = missileRemasteredCollision.size();
    ImportDetail::MissileDefaultRolePair candidate;
    std::string roleError;
    if (!ImportDetail::ResolveMissileDefaultRole(input, candidate, &roleError)) {
      lines.push_back("WPSM missile COLR/DEFS: not the pinned USA default role (" + roleError + "); no pair");
    } else {
      bool exact = false;
      bool conflict = false;
      for (const EffectPairing& have : pairings) {
        if (have.id == candidate.genp && have.retail == candidate.retailPart) {
          exact = true;
        } else if (have.id == candidate.genp || have.retail == candidate.retailPart) {
          conflict = true;
          lines.push_back("WPSM missile COLR/DEFS: conflict with " + have.method + " (" +
                          EffectGuidString(ToStored(have.id)) + " -> " + Hex8(have.retail) + "); no pair");
        }
      }
      if (!conflict && !exact) {
        pairings.push_back({candidate.genp, candidate.retailPart, "wpsm missile COLR/DEFS default"});
        lines.push_back("WPSM missile COLR/DEFS default: " + EffectGuidString(ToStored(candidate.genp)) + " -> " +
                        Hex8(candidate.retailPart));
      }
    }
  }

  // PowerBall's collision-response default is separately pinned to USA 1.00:
  // WPSC COLR -> CRSC DEFS PART and named WPSM COLR -> CRSM DEFS GENP. The
  // named lookups and unique typed-resource checks make other regions,
  // duplicates, missing assets, and conflicting effect-id claims fail closed.
  uint32_t powerBallWpsc = 0;
  std::vector<uint8_t> powerBallWeapon;
  std::vector<uint8_t> powerBallProjectile;
  std::vector<uint8_t> powerBallRetailCollision;
  std::vector<uint8_t> powerBallRemasteredCollision;
  std::string powerBallError;
  if (!retail.WpscId("powerball", powerBallWpsc)) {
    lines.push_back("WPSM powerball COLR/DEFS: no unique disc WPSC named PowerBall");
  } else if (!retail.Read(kWPSC, powerBallWpsc, powerBallWeapon) ||
             !remastered.ReadProjectile("powerball", powerBallProjectile, powerBallError)) {
    lines.push_back("WPSM powerball COLR/DEFS: could not read the exact named weapon assets");
  } else {
    const bool crscPresent = retail.HasUniqueResource(kCRSC, ImportDetail::kPowerBallCrsc) &&
                             retail.Read(kCRSC, ImportDetail::kPowerBallCrsc, powerBallRetailCollision);
    const bool crsmPresent = remastered.HasUniqueCollision(ImportDetail::kPowerBallCrsm) &&
                             remastered.ReadCollision(ImportDetail::kPowerBallCrsm, powerBallRemasteredCollision,
                                                      powerBallError);
    const bool genpPresent = remastered.HasUniqueEffect(ImportDetail::kPowerBallDefaultGenp) &&
                             remastered.EffectAssetType(ImportDetail::kPowerBallDefaultGenp) == kGENP;
    ImportDetail::PowerBallDefaultRoleInput input;
    input.weaponName = "powerball";
    input.retailWpscId = powerBallWpsc;
    input.retailWpscExists = retail.HasUniqueResource(kWPSC, powerBallWpsc);
    input.retailCrscExists = crscPresent;
    input.retailPartExists = retail.HasUniqueResource(kPART, ImportDetail::kPowerBallDefaultPart);
    input.remasteredWpsmExists = true;  // ReadProjectile rejected missing/ambiguous names.
    input.remasteredCrsmExists = crsmPresent;
    input.remasteredGenpExists = genpPresent;
    input.retailWpsc = powerBallWeapon.data();
    input.retailWpscSize = powerBallWeapon.size();
    input.retailCrsc = powerBallRetailCollision.data();
    input.retailCrscSize = powerBallRetailCollision.size();
    input.remasteredWpsm = powerBallProjectile.data();
    input.remasteredWpsmSize = powerBallProjectile.size();
    input.remasteredCrsm = powerBallRemasteredCollision.data();
    input.remasteredCrsmSize = powerBallRemasteredCollision.size();
    ImportDetail::PowerBallDefaultRolePair candidate;
    std::string roleError;
    if (!ImportDetail::ResolvePowerBallDefaultRole(input, candidate, &roleError)) {
      lines.push_back("WPSM powerball COLR/DEFS: not the pinned USA 1.00 default role (" + roleError + "); no pair");
    } else {
      bool exact = false;
      bool conflict = false;
      for (const EffectPairing& have : pairings) {
        if (have.id == candidate.genp && have.retail == candidate.retailPart) {
          exact = true;
        } else if (ImportDetail::ConflictsWithPowerBallDefaultRolePair(candidate, have.id, have.retail)) {
          conflict = true;
          lines.push_back("WPSM powerball COLR/DEFS: conflict with " + have.method + " (" +
                          EffectGuidString(ToStored(have.id)) + " -> " + Hex8(have.retail) + "); no pair");
        }
      }
      if (!conflict && !exact) {
        pairings.push_back({candidate.genp, candidate.retailPart, "wpsm powerball COLR/DEFS default"});
        lines.push_back("WPSM powerball COLR/DEFS default: " + EffectGuidString(ToStored(candidate.genp)) + " -> " +
                        Hex8(candidate.retailPart));
      }
    }
  }

  // PlasmaBeam's collision-response default is independently pinned to USA
  // 1.00. Its APSM projectile link is a different role and remains untouched.
  // The exact lower-case FrameKey and unique typed-resource checks make other
  // regions, duplicates, missing assets, and conflicting claims fail closed.
  uint32_t plasmaBeamWpsc = 0;
  std::vector<uint8_t> plasmaBeamWeapon;
  std::vector<uint8_t> plasmaBeamProjectile;
  std::vector<uint8_t> plasmaBeamRetailCollision;
  std::vector<uint8_t> plasmaBeamRemasteredCollision;
  std::string plasmaBeamError;
  if (!retail.WpscId("plasmabeam", plasmaBeamWpsc)) {
    lines.push_back("WPSM plasmabeam COLR/DEFS: no unique disc WPSC named PlasmaBeam");
  } else if (!retail.Read(kWPSC, plasmaBeamWpsc, plasmaBeamWeapon) ||
             !remastered.ReadProjectile("plasmabeam", plasmaBeamProjectile, plasmaBeamError)) {
    lines.push_back("WPSM plasmabeam COLR/DEFS: could not read the exact named weapon assets");
  } else {
    const bool crscPresent = retail.HasUniqueResource(kCRSC, ImportDetail::kPlasmaBeamCrsc) &&
                             retail.Read(kCRSC, ImportDetail::kPlasmaBeamCrsc, plasmaBeamRetailCollision);
    const bool crsmPresent = remastered.HasUniqueCollision(ImportDetail::kPlasmaBeamCrsm) &&
                             remastered.ReadCollision(ImportDetail::kPlasmaBeamCrsm, plasmaBeamRemasteredCollision,
                                                      plasmaBeamError);
    const bool genpPresent = remastered.HasUniqueEffect(ImportDetail::kPlasmaBeamDefaultGenp) &&
                             remastered.EffectAssetType(ImportDetail::kPlasmaBeamDefaultGenp) == kGENP;
    ImportDetail::PlasmaBeamDefaultRoleInput input;
    input.weaponName = "plasmabeam";
    input.retailWpscId = plasmaBeamWpsc;
    input.retailWpscExists = retail.HasUniqueResource(kWPSC, plasmaBeamWpsc);
    input.retailCrscExists = crscPresent;
    input.retailPartExists = retail.HasUniqueResource(kPART, ImportDetail::kPlasmaBeamDefaultPart);
    input.remasteredWpsmExists = true;  // ReadProjectile rejected missing/ambiguous names.
    input.remasteredCrsmExists = crsmPresent;
    input.remasteredGenpExists = genpPresent;
    input.retailWpsc = plasmaBeamWeapon.data();
    input.retailWpscSize = plasmaBeamWeapon.size();
    input.retailCrsc = plasmaBeamRetailCollision.data();
    input.retailCrscSize = plasmaBeamRetailCollision.size();
    input.remasteredWpsm = plasmaBeamProjectile.data();
    input.remasteredWpsmSize = plasmaBeamProjectile.size();
    input.remasteredCrsm = plasmaBeamRemasteredCollision.data();
    input.remasteredCrsmSize = plasmaBeamRemasteredCollision.size();
    ImportDetail::PlasmaBeamDefaultRolePair candidate;
    std::string roleError;
    if (!ImportDetail::ResolvePlasmaBeamDefaultRole(input, candidate, &roleError)) {
      lines.push_back("WPSM plasmabeam COLR/DEFS: not the pinned USA 1.00 default role (" + roleError + "); no pair");
    } else {
      bool exact = false;
      bool conflict = false;
      for (const EffectPairing& have : pairings) {
        if (have.id == candidate.genp && have.retail == candidate.retailPart) {
          exact = true;
        } else if (ImportDetail::ConflictsWithPlasmaBeamDefaultRolePair(candidate, have.id, have.retail)) {
          conflict = true;
          lines.push_back("WPSM plasmabeam COLR/DEFS: conflict with " + have.method + " (" +
                          EffectGuidString(ToStored(have.id)) + " -> " + Hex8(have.retail) + "); no pair");
        }
      }
      if (!conflict && !exact) {
        pairings.push_back({candidate.genp, candidate.retailPart, "wpsm plasmabeam COLR/DEFS default"});
        lines.push_back("WPSM plasmabeam COLR/DEFS default: " + EffectGuidString(ToStored(candidate.genp)) + " -> " +
                        Hex8(candidate.retailPart));
      }
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

// One level of a shell texture's chain: each texel the mean of the 2x2 above it. An odd or
// 1-wide edge reads its last column/row twice, so a side never halves below 1. An sRGB level
// averages its colour in linear light (the shader samples colour as sRGB) and its alpha
// plainly; a linear one averages every channel plainly.
void ShellLevel(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh,
                bool srgb) {
  for (uint32_t y = 0; y < dh; ++y) {
    const uint32_t y0 = std::min(y * 2, sh - 1);
    const uint32_t y1 = std::min(y * 2 + 1, sh - 1);
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = std::min(x * 2, sw - 1);
      const uint32_t x1 = std::min(x * 2 + 1, sw - 1);
      const uint8_t* p[4] = {src + (size_t(y0) * sw + x0) * 4, src + (size_t(y0) * sw + x1) * 4,
                             src + (size_t(y1) * sw + x0) * 4, src + (size_t(y1) * sw + x1) * 4};
      uint8_t* out = dst + (size_t(y) * dw + x) * 4;
      for (int c = 0; c < 4; ++c) {
        if (srgb && c < 3) {
          const float sum = SrgbByteToLinear(p[0][c]) + SrgbByteToLinear(p[1][c]) +
                            SrgbByteToLinear(p[2][c]) + SrgbByteToLinear(p[3][c]);
          out[c] = LinearToSrgbByteExact(sum * 0.25f);
        } else {
          out[c] = uint8_t((unsigned(p[0][c]) + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
        }
      }
    }
  }
}

// `tex`'s top mip and every level below it, down to 1x1, as shell.bin holds them (port_shell_file.h):
// the shell shader samples the chain, so the import makes it rather than coding down to 1x1.
void BuildShellImage(const TxtrImage& tex, PortShell::Image& out) {
  out = PortShell::Image();
  out.width = tex.width;
  out.height = tex.height;
  out.srgb = tex.srgb;
  for (uint32_t side = std::max(tex.width, tex.height); side != 0; side >>= 1) {
    ++out.mips;
  }
  out.rgba = tex.rgba;
  for (uint32_t level = 1; level < out.mips; ++level) {
    const uint32_t sw = std::max(out.width >> (level - 1), 1u);
    const uint32_t sh = std::max(out.height >> (level - 1), 1u);
    const uint32_t dw = std::max(out.width >> level, 1u);
    const uint32_t dh = std::max(out.height >> level, 1u);
    const size_t from = out.rgba.size() - size_t(sw) * sh * 4;
    const size_t to = out.rgba.size();
    out.rgba.resize(to + size_t(dw) * dh * 4);
    ShellLevel(out.rgba.data() + from, sw, sh, out.rgba.data() + to, dw, dh, out.srgb);
  }
}

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
  fs::path source = fromHeld ? held : staging.parent_path() / kImportModName;
  if (!fromHeld) {
    // Which import the unchanged stages are reused from (port_remastered_storage.h): the installed
    // one inside the root, else a legacy import in the shared mods folder - which is only read, as
    // LinkFiles hard-links its files, or copies them where the file systems differ (the card and the
    // app's storage do), so that folder is left exactly as it is. A full import into the app never
    // moves or deletes it.
    const std::string legacy = LegacySharedImport();
    std::error_code installedEc;
    std::error_code legacyEc;
    const bool installedUsable = fs::is_regular_file(source / kManifestName, installedEc);
    const bool legacyUsable =
        !legacy.empty() && fs::is_regular_file(PathFromString(legacy) / kManifestName, legacyEc);
    const std::string reused =
        PortRemasteredStorage::ReuseFolder(PathString(source), installedUsable, legacy, legacyUsable);
    if (!reused.empty()) {
      source = PathFromString(reused);
    }
  }
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

  SetStep("disc");
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
    SetStep(name);
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
    // Said only once its files are in: a stage whose files are gone is made again.
    if (reuse) {
      reuse = LinkFiles(source, staging, old->second.files);
      SetMessage(reuse ? "Reusing the previous import's " + name
                       : "Making the " + name + " again: the previous import's files are gone");
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
        PortLog::Write("%s\n", line.c_str());
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
        decoded.hdr = image.format == kTxtrFormatBc6hUfloat || image.format == kTxtrFormatBc6hSfloat;
        return true;
      });
    };
    io.hdrDds = [&](const ModelUuid& id, int maxEdge, std::vector<uint8_t>& dds, std::string& hdrError) {
      std::vector<uint8_t> raw;
      TxtrMipsBc6h tex;
      if (!remastered.ReadTexture(id, raw, hdrError) || !ReadTxtrMipsBc6h(raw.data(), raw.size(), tex, hdrError)) {
        return false;
      }
      return EncodeHdrDds(tex, maxEdge, WantedTextureFormat(), dds, hdrError);
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
    size_t at = SIZE_MAX;  // only there (TableEntry::character), else wherever `from` is
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
  auto hex = [](uint32_t id) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X", id);
    return std::string(name);
  };
  // The CCharacterInfo a pair at `at` of an ANCS belongs to: the offset of its body, the
  // CMDL, CSKR, CINF triple that leads it (0x14..0x1c of the info, after the id, the
  // version and the name), the nearest one at or before `at`. A pair after it is another
  // model of the character, its ice overlay. SIZE_MAX when there is none.
  auto bodyBefore = [&](const std::vector<uint8_t>& data, size_t at) {
    const auto isBody = [&](size_t p) {
      return p + 12 <= data.size() && retail.HasResource(kCMDL, port::ReadBE32(&data[p])) &&
             retail.HasResource(kCSKR, port::ReadBE32(&data[p + 4])) &&
             retail.HasResource(kCINF, port::ReadBE32(&data[p + 8]));
    };
    for (size_t p = at + 1; p-- > 0;) {
      if (isBody(p)) {
        return p;
      }
    }
    return SIZE_MAX;
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
      uint32_t id = port::kFnv32Basis;
      for (int i = 0; i < 4; ++i) {
        id = port::Fnv1a32Step(id, uint8_t(seed >> (i * 8)));
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
      size_t at = SIZE_MAX;
      for (auto it = std::search(data.begin(), data.end(), pair.begin(), pair.end()); it != data.end();
           it = std::search(it + 1, data.end(), pair.begin(), pair.end())) {
        const size_t offset = size_t(it - data.begin());
        const size_t body = entry.character != 0 ? bodyBefore(data, offset) : SIZE_MAX;
        if (entry.character == 0 || (body != SIZE_MAX && port::ReadBE32(&data[body]) == entry.character)) {
          ++found;
          at = offset;
        }
      }
      if (found != 1) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " binds " + hex(entry.retail) + " " + std::to_string(found) +
                     " times" + (entry.character != 0 ? " in the character of " + hex(entry.character) : "");
        continue;
      }
      const uint32_t seed =
          entry.ancs * 0x9E3779B1u ^ entry.retail ^ uint32_t(entry.key + 1) * 0x85EBCA6Bu ^ entry.character;
      look.model = fresh(seed);
      look.skins = {fresh(seed ^ 0x534B494Eu)};
      character.rebinds.push_back({i, pair, bytesOf({look.model, look.skins[0]}), look.skins[0],
                                   entry.character != 0 ? at : SIZE_MAX});
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
              skeletons[{i, s}].insert(port::ReadBE32(&*(it + 8)));
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
  // Each skinned model's skeletons, for the votes (ConvertOptions::jointPivots):
  // the retail ANCS that binds the model to its first skin names the CINF, and
  // the CHPR under that ANCS's id is the Remastered character, whose joints are
  // the model's if its skinned model is the one converted.
  // A pair that is not the character's body (its ice overlay, CCharacterInfo's frozen
  // pair) sits under the character's CINF too: the CINF is the body triple's, the nearest
  // CMDL, CSKR, CINF before the pair, and the body votes (ConvertOptions::weightModel).
  struct Rig {
    std::vector<std::array<double, 3>> pivots;
    std::vector<ConvertOptions::RetailBone> bones;
    uint32_t bodyModel = 0, bodySkin = 0;
  };
  std::vector<Rig> rigs(modelsReused ? 0 : count);
  if (!modelsReused) {
    SetMessage("Reading the skeletons");
    std::unordered_map<uint64_t, std::vector<size_t>> byPair;
    for (size_t i = 0; i < count; ++i) {
      if (table[i].skinCount > 0 && looks[i].ok) {
        byPair[uint64_t(table[i].retail) << 32 | TableSkins(table[i])[0]].push_back(i);
      }
    }
    size_t rigged = 0;
    for (uint32_t ancs : retail.Ids(kANCS)) {
      std::vector<uint8_t> data;
      if (byPair.empty() || !retail.Read(kANCS, ancs, data)) {
        continue;
      }
      struct Bound {
        size_t entry;
        uint32_t cinf, bodyModel, bodySkin;  // body 0: the pair is the body
      };
      std::vector<Bound> bound;
      for (size_t at = 0; at + 8 <= data.size(); ++at) {
        const auto it = byPair.find(uint64_t(port::ReadBE32(&data[at])) << 32 | port::ReadBE32(&data[at + 4]));
        if (it == byPair.end()) {
          continue;
        }
        const size_t p = bodyBefore(data, at);
        if (p == SIZE_MAX) {
          continue;
        }
        for (size_t i : it->second) {
          if (table[i].character != 0 && (table[i].ancs != ancs || port::ReadBE32(&data[p]) != table[i].character)) {
            continue;
          }
          bound.push_back({i, port::ReadBE32(&data[p + 8]), p == at ? 0 : port::ReadBE32(&data[p]),
                           p == at ? 0 : port::ReadBE32(&data[p + 4])});
        }
      }
      if (bound.empty()) {
        continue;
      }
      // The body votes with or without the Remastered character (Magdolite and Metroid
      // Prime have no CHPR).
      for (const Bound& b : bound) {
        if (b.bodyModel != 0 && rigs[b.entry].bodyModel == 0) {
          rigs[b.entry].bodyModel = b.bodyModel;
          rigs[b.entry].bodySkin = b.bodySkin;
        }
      }
      ModelUuid chprId = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x00, 0xf0, 0x00, 0x00, 0x00,
                          uint8_t(ancs >> 24), uint8_t(ancs >> 16), uint8_t(ancs >> 8), uint8_t(ancs)};
      std::vector<uint8_t> chpr;
      std::string why;
      PortRemasteredAnim::Character character;
      if (!remastered.ReadCharacter(chprId, chpr, why) ||
          !PortRemasteredAnim::ReadCharacter(chpr, character, why, false) || character.inverseBind.empty()) {
        continue;
      }
      // The CHPR names its models in property order; a pak id swaps the first three fields.
      // Searched for rather than read from Character::skinnedModel, whose position is wrong
      // in some files (the Queen's AAA2D50D, with 2 stream infos).
      static const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
      const auto names = [&chpr](const uint8_t* rem) {
        uint8_t id[16];
        for (int b = 0; b < 16; ++b) {
          id[kOrder[b]] = rem[b];
        }
        return std::search(chpr.begin(), chpr.end(), id, id + 16) != chpr.end();
      };
      for (const auto& [i, cinf, bodyModel, bodySkin] : bound) {
        (void)bodyModel;
        (void)bodySkin;
        std::vector<uint8_t> cinfData;
        PortDread::PrimeSkeleton skeleton;
        if (!rigs[i].pivots.empty() || !names(table[i].rem) ||
            !retail.Read(kCINF, cinf, cinfData) ||
            !PortDread::ParseCinf(cinfData.data(), cinfData.size(), skeleton, &why)) {
          continue;
        }
        // A joint's pivot: the translation of its inverse bind's inverse, -R^-1 t,
        // R^-1 by the adjugate (the bind is a rotation, maybe scaled).
        for (const auto& m : character.inverseBind) {
          const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
          const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
          const double inv[3][3] = {{e * k - f * h, c * h - b * k, b * f - c * e},
                                    {f * g - d * k, a * k - c * g, c * d - a * f},
                                    {d * h - e * g, b * g - a * h, a * e - b * d}};
          auto& p = rigs[i].pivots.emplace_back();
          for (int r = 0; r < 3; ++r) {
            p[size_t(r)] = std::abs(det) > 1e-12
                               ? -(inv[r][0] * m[3] + inv[r][1] * m[7] + inv[r][2] * m[11]) / det
                               : 0.0;
          }
        }
        for (const PortDread::PrimeJoint& joint : skeleton.joints) {
          auto& bone = rigs[i].bones.emplace_back();
          bone.id = joint.id;
          bone.parent = joint.parent;
          for (int a = 0; a < 3; ++a) {
            bone.head[a] = joint.head[a];
          }
        }
        ++rigged;
      }
    }
    AddLine("skeletons: " + std::to_string(rigged) + " skinned models with their Remastered joints");
  }
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
        if (i < rigs.size()) {
          options.jointPivots = rigs[i].pivots;
          options.retailBones = rigs[i].bones;
          options.weightModel = rigs[i].bodyModel;
          options.weightSkin = rigs[i].bodySkin;
        }
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
      sState.stepDone = sState.done;
      sState.stepTotal = sState.total;
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
        if (modelOk[rebind.entry] != 0 && fs::exists(staging / (hex(rebind.skin) + ".CSKR"), skinError) &&
            rebind.at != SIZE_MAX) {
          if (rebind.at + rebind.from.size() <= data.size() &&
              std::equal(rebind.from.begin(), rebind.from.end(), data.begin() + ptrdiff_t(rebind.at))) {
            std::copy(rebind.to.begin(), rebind.to.end(), data.begin() + ptrdiff_t(rebind.at));
            swapped = true;
          }
        } else if (modelOk[rebind.entry] != 0 && fs::exists(staging / (hex(rebind.skin) + ".CSKR"), skinError)) {
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
  if (Parts().effects && WantsRemasteredEffects() &&
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
    effectIO.retailHas = [&](uint32_t type, uint32_t id) { return retail.HasResource(type, id); };
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
    effectIO.textureRg = [&](const EffectGuid& id) {
      std::vector<uint8_t> raw;
      std::string ignored;
      TxtrImage info;
      return remastered.ReadTexture(id, raw, ignored) && ReadTxtrInfo(raw.data(), raw.size(), info, ignored) &&
             (info.format == kTxtrFormatBc5Unorm || info.format == kTxtrFormatBc5Snorm);
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
      PortLog::Write("remastered import: %s\n", line.c_str());
    };
    effectIO.report = [&](const EffectReportRow& row) {
      const std::string text = FormatEffectRow(row);
      std::lock_guard<std::mutex> lock(reportMutex);
      effectRows.push_back(text);
    };
    std::vector<uint8_t> effectTable;  // effectids.map
    std::vector<WpscSwoosh> wpscSwooshes;
    {
      std::vector<std::string> pairLines;
      std::vector<EffectIdRemap> remaps;
      effectIO.pairings = ProjectilePairings(remastered, retail, pairLines, remaps, wpscSwooshes);
      std::sort(remaps.begin(), remaps.end(), [](const EffectIdRemap& a, const EffectIdRemap& b) {
        return std::tie(a.owner, a.field, a.retail) < std::tie(b.owner, b.field, b.retail);
      });
      std::string table;
      for (const EffectIdRemap& remap : remaps) {
        char text[64];
        std::snprintf(text, sizeof(text), "%08X %08X %08X %08X\n", remap.owner, remap.field, remap.retail,
                      remap.synthetic);
        table += text;
      }
      effectTable.assign(table.begin(), table.end());
      for (const std::string& line : pairLines) {
        PortLog::Write("remastered import: projectile %s\n", line.c_str());
      }
      AddLine("effects: " + std::to_string(effectIO.pairings.size()) + " projectile effects paired");

      // The GENPs of the rooms' own effect actors, under ids no disc resource, other synthetic
      // effect or other such GENP has (the rooms' .roomfx files name them).
      std::unordered_set<uint32_t> synthetic;
      for (const char* name : kSyntheticEffectNames) {
        synthetic.insert(SyntheticEffectId(name));
      }
      for (const EffectIdRemap& remap : remaps) {
        synthetic.insert(remap.synthetic);
      }
      for (const WpscSwoosh& addition : wpscSwooshes) {
        synthetic.insert(addition.synthetic);
      }
      size_t roomGenps = 0, collisions = 0;
      for (const RoomWorld& world : RoomWorlds()) {
        RoomPak master;
        std::vector<RoomPak> rooms;
        remastered.World(world.dir, master, rooms);
        rooms.push_back(master);
        for (const EffectGuid& genp : RoomEffectGenps(rooms)) {
          if (std::find(effectIO.roomOnly.begin(), effectIO.roomOnly.end(), genp) != effectIO.roomOnly.end()) {
            continue;
          }
          ++roomGenps;
          const uint32_t id = RoomEffectId(genp);
          if (retail.HasId(id) || !synthetic.insert(id).second) {
            ++collisions;
            PortLog::Write("remastered import: room effect %s: its id %s is taken, left out\n",
                           EffectGuidString(genp).c_str(), Hex8(id).c_str());
            continue;
          }
          effectIO.roomOnly.push_back(genp);
          std::lock_guard<std::mutex> lock(takenMutex);
          taken.insert(id);
        }
      }
      // The unnamed Remastered-only effects port actors build (kActorEffectGuids), the same way.
      for (const EffectGuid& genp : effectIO.effects) {
        const bool actor = std::any_of(std::begin(kActorEffectGuids), std::end(kActorEffectGuids),
                                       [&](const char* text) { return EffectIdIs(genp, text); });
        if (!actor || std::find(effectIO.roomOnly.begin(), effectIO.roomOnly.end(), genp) != effectIO.roomOnly.end()) {
          continue;
        }
        ++roomGenps;
        const uint32_t id = RoomEffectId(genp);
        if (retail.HasId(id) || !synthetic.insert(id).second) {
          ++collisions;
          PortLog::Write("remastered import: actor effect %s: its id %s is taken, left out\n",
                         EffectGuidString(genp).c_str(), Hex8(id).c_str());
          continue;
        }
        effectIO.roomOnly.push_back(genp);
        std::lock_guard<std::mutex> lock(takenMutex);
        taken.insert(id);
      }
      AddLine("effects: " + std::to_string(roomGenps) + " effects of the rooms' own effect actors and port actors, " +
              std::to_string(collisions) + " left out (id taken)");
    }
    std::set<std::string> effectWritten;  // the files the effects wrote (the converters write on every thread)
    {
      static std::mutex writtenMutex;
      const auto write = effectIO.write;
      effectIO.write = [&effectWritten, write](const std::string& file, const std::vector<uint8_t>& data) {
        const bool ok = write(file, data);
        if (ok) {
          std::lock_guard<std::mutex> lock(writtenMutex);
          effectWritten.insert(file);
        }
        return ok;
      };
    }
    const EffectImportResult effects = ImportEffects(effectIO);
    if (!effectTable.empty()) {
      effectIO.write("effectids.map", effectTable);
    }
    // The WPSCs that name the added swooshes, those whose SWHC was written. Each WPSC gets all its fields.
    std::map<uint32_t, std::vector<uint8_t>> wpscs;
    for (const WpscSwoosh& addition : wpscSwooshes) {
      if (effectWritten.count(Hex8(addition.synthetic) + ".SWHC") == 0) {
        PortLog::Write("remastered import: projectile WPSM %s: the swoosh did not convert, the WPSC keeps no field\n",
                       addition.who.c_str());
        continue;
      }
      auto it = wpscs.find(addition.wpsc);
      if (it == wpscs.end()) {
        std::vector<uint8_t> weapon;
        if (!retail.Read(kWPSC, addition.wpsc, weapon)) {
          continue;
        }
        it = wpscs.emplace(addition.wpsc, std::move(weapon)).first;
      }
      std::string error;
      if (!AddRetailLink(it->second, addition.field, addition.synthetic, it->second, &error)) {
        PortLog::Write("remastered import: projectile WPSM %s: %s\n", addition.who.c_str(), error.c_str());
      } else {
        PortLog::Write("remastered import: projectile WPSM %s: %s -> %s in the WPSC %s\n", addition.who.c_str(),
                       addition.field, Hex8(addition.synthetic).c_str(), Hex8(addition.wpsc).c_str());
      }
    }
    for (const auto& [wpsc, data] : wpscs) {
      effectIO.write(Hex8(wpsc) + ".WPSC", data);
    }
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
  const bool roomsReused = beginStage("rooms",
                                      std::to_string(ImportStage::kRooms) + " geometry " +
                                          GeometryChoice() +  // as the env or the panel says it
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
  // Where the rooms put the retail models Remastered keeps under their retail ids.
  std::mutex retailMutex;
  std::vector<RetailPlacement> retailPlacements;
  auto geometryId = [&](const ModelUuid& uuid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = geometryIds.find(uuid);
    if (known != geometryIds.end()) {
      id = known->second;
      return true;
    }
    id = port::Fnv1a32(uuid.data(), uuid.size());
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
    id = port::Fnv1a32(uuid.data(), uuid.size(), port::kFnv32Basis ^ uint32_t(joint + 1) * 0x85EBCA77u);
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
    id = port::Fnv1a32(key, port::kFnv32Basis ^ 0x9E3779B1u);
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
      uint32_t id = port::Fnv1a32(uuid.data(), uuid.size(), port::kFnv32Basis ^ 0x57A7E5u);
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
      // A room effect actor's PART: the one the effects step wrote for its GENP (or the ELSC for
      // an ELC2 slot), else the disc's it carries the id of.
      io.effect = [&](const std::array<uint8_t, 16>& genp) -> uint32_t {
        std::error_code ec;
        const uint32_t own = RoomEffectId(genp);
        if (fs::exists(staging / PathFromString(Hex8(own) + ".PART"), ec) ||
            fs::exists(staging / PathFromString(Hex8(own) + ".ELSC"), ec)) {
          return own;
        }
        std::array<uint8_t, 16> property = genp;
        std::swap(property[0], property[3]);
        std::swap(property[1], property[2]);
        std::swap(property[4], property[5]);
        std::swap(property[6], property[7]);
        const std::optional<uint32_t> carried = EffectRetailId(property);
        return carried && retail.HasId(*carried) ? *carried : 0;
      };
      // The retail PARTs the effects step wrote this GENP under by its own table (one GENP can
      // stand for two, the lava spit's): only those the mod has a PART for, or the retail object
      // would be pointed at nothing.
      io.barrierEffect = [&](uint32_t part, uint32_t feature,
                             const std::vector<std::array<uint8_t, 16>>& vars) {
        const fs::path path = staging / PathFromString(Hex8(part) + ".PART");
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return false;
        const auto length = in.tellg();
        if (length <= 0 || length > 64 * 1024 * 1024) return false;
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return false;
        std::string reason;
        const auto dependency = [&](uint32_t type, uint32_t id) {
          if (type != 0x54585452) return false;
          std::error_code ec;
          const fs::path texture = staging / PathFromString(Hex8(id) + ".TXTR");
          return fs::is_regular_file(texture, ec) && !ec && fs::file_size(texture, ec) > 0 && !ec;
        };
        if (PortQueenBarrier::ValidatePart(bytes, feature, vars, dependency, reason)) return true;
        PortLog::Write("remastered import: barrier effect %s: %s\n", Hex8(part).c_str(), reason.c_str());
        return false;
      };
      io.matched = [&](const std::array<uint8_t, 16>& genp) {
        std::vector<uint32_t> out;
        for (uint32_t id : MatchedRetailEffects(genp)) {
          std::error_code ec;
          if (fs::exists(staging / PathFromString(Hex8(id) + ".PART"), ec)) {
            out.push_back(id);
          }
        }
        return out;
      };
      io.wantsGeometry = WantsGeometry;
      io.retailModel = [&](const RetailPlacement& placement) {
        std::lock_guard<std::mutex> lock(retailMutex);
        retailPlacements.push_back(placement);
      };
      io.cancelled = [] { return sCancel.load(); };
      // One line a room (what was left out and why): too many for the panel, so the log.
      io.log = [](const std::string& line) {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        PortLog::Write("remastered import: %s\n", line.c_str());
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
  // "g <uuid> <id> <joint> <liquid>" a model, "l <lava values>" a lava pool, in order;
  // "r <uuid> <retail> <mrea> <object> <ok> <entity> <orient, 9> <offset x y z> <room>" a retail
  // model's placement.
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
      } else if (tag == "r") {
        std::string uuid;
        std::string orient[9], offset[3];
        RetailPlacement& p = retailPlacements.emplace_back();
        in >> uuid >> std::hex >> p.retail >> p.mrea >> p.object >> std::dec >> p.ok >> p.entity;
        for (std::string& value : orient) {
          in >> value;
        }
        in >> offset[0] >> offset[1] >> offset[2] >> p.room;
        for (size_t b = 0; b < p.model.size() && b * 2 + 1 < uuid.size(); ++b) {
          p.model[b] = uint8_t(std::strtoul(uuid.substr(b * 2, 2).c_str(), nullptr, 16));
        }
        for (int i = 0; i < 9; ++i) {
          p.orient[i / 3][i % 3] = std::strtod(orient[i].c_str(), nullptr);
        }
        for (int i = 0; i < 3; ++i) {
          p.offset[i] = std::strtod(offset[i].c_str(), nullptr);
        }
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
    for (const RetailPlacement& p : retailPlacements) {
      char line[256];
      std::snprintf(line, sizeof(line), "r %s %08X %08X %08X %d %d", UuidText(p.model).c_str(), p.retail, p.mrea,
                    p.object, int(p.ok), p.entity);
      std::string text = line;
      for (int i = 0; i < 12; ++i) {
        std::snprintf(line, sizeof(line), " %a", i < 9 ? p.orient[i / 3][i % 3] : p.offset[i - 9]);
        text += line;
      }
      stage.extra.push_back(text + " " + p.room);
    }
    endStage();
    if (brdfMissing) {
      made.erase("rooms");
    }
  }

  // The retail models Remastered keeps under their retail ids that the table doesn't pair. The
  // orient and offset are the model's own (where its vertices sit against retail's), so one
  // replaces its retail model when the retail objects paired with their entities (RetailPlacement)
  // all found it at one transform: "paired", or "partial" when other objects naming it are in an
  // area whose count of them differs from its room's entities. One whose placements differ
  // ("per-instance": Remastered moved some of its objects on their own), or that no object paired
  // with ("unpaired"), stays retail. reports/retailmodels.tsv lists each with its placements. Other
  // assets share the id prefix (textures, effects), so only Remastered's models count.
  {
    std::sort(retailPlacements.begin(), retailPlacements.end(), [](const RetailPlacement& a, const RetailPlacement& b) {
      return std::tie(a.retail, a.mrea, a.object, a.room) < std::tie(b.retail, b.mrea, b.object, b.room);
    });
    size_t tableCount = 0;
    const TableEntry* tableEntries = Table(tableCount);
    std::unordered_map<uint32_t, const TableEntry*> inTable;
    for (size_t i = 0; i < tableCount; ++i) {
      inTable.emplace(tableEntries[i].retail, &tableEntries[i]);
    }
    struct RetailModel {
      ModelUuid uuid;
      uint32_t retail;
      double orient[3][3];
      double offset[3];
      bool modelSpace = false;  // per-instance: converted untransformed, placed per object
    };
    std::vector<PortRetailPlacement::Record> placementRecords;
    // Per-instance models: the largest difference between two of a model's placements, by
    // decade, so the report shows whether they differ by float noise or by real moves.
    std::map<int, int> orientSpread, offsetSpread;
    int mirrored = 0;
    std::string spreadReport = "retail\tplacements\torient spread\toffset spread\tmirrored\n";
    const auto decade = [](double v) { return v <= 0 ? -99 : int(std::floor(std::log10(v))); };
    std::vector<RetailModel> retailModels;
    std::string report = "retail\tstatus\torient\toffset\ttable orient\ttable offset\t"
                         "placements (room object entity ok orient offset)\n";
    std::map<std::string, int> statusCount;
    const auto offsetText = [](const double* o) {
      char text[96];
      std::snprintf(text, sizeof(text), "%.3f,%.3f,%.3f", o[0], o[1], o[2]);
      return std::string(text);
    };
    const auto orientText = [](const double (*m)[3]) {
      std::string text;
      for (int i = 0; i < 9; ++i) {
        char value[32];
        std::snprintf(value, sizeof(value), "%s%.3f", i == 0 ? "" : ",", m[i / 3][i % 3] + 0.0);
        text += value;
      }
      return text;
    };
    // Placements agree only when every component is the same float: ROOM and SCLY store their
    // transforms as float, so a difference there is an object Remastered placed on its own.
    const auto same = [](const RetailPlacement& a, const RetailPlacement& b) {
      for (int i = 0; i < 9; ++i) {
        if (float(a.orient[i / 3][i % 3]) != float(b.orient[i / 3][i % 3])) {
          return false;
        }
      }
      return float(a.offset[0]) == float(b.offset[0]) && float(a.offset[1]) == float(b.offset[1]) &&
             float(a.offset[2]) == float(b.offset[2]);
    };
    for (auto first = retailPlacements.begin(); first != retailPlacements.end();) {
      const auto last = std::find_if(first, retailPlacements.end(),
                                     [&](const RetailPlacement& p) { return p.retail != first->retail; });
      if (!remastered.HasModel(first->model)) {
        first = last;
        continue;
      }
      if (std::any_of(first, last, [](const RetailPlacement& p) { return p.animated; })) {
        // Retail draws an animation set's character there, not this model, and that character's
        // own model can be this one: Remastered's copy (a stand-in box in 8_courtyard's gate,
        // whose look is the CHPR's) must not replace it.
        ++statusCount["animated"];
        char head[32];
        std::snprintf(head, sizeof(head), "%08X\tanimated\t-\t-\t-\t-\t", first->retail);
        report += head;
        for (auto p = first; p != last; ++p) {
          char text[48];
          std::snprintf(text, sizeof(text), " %08X #%d", p->object, p->entity);
          report += (p == first ? "" : "; ") + p->room + text;
        }
        report += '\n';
        first = last;
        continue;
      }
      const auto ok = std::find_if(first, last, [](const RetailPlacement& p) { return p.ok; });
      const bool all = std::all_of(first, last, [](const RetailPlacement& p) { return p.ok; });
      const bool agree =
          ok != last && std::all_of(first, last, [&](const RetailPlacement& p) { return !p.ok || same(p, *ok); });
      const auto table = inTable.find(first->retail);
      const bool perInstance = ok != last && !agree;
      const char* status = ok == last                  ? "unpaired"
                           : perInstance && !all       ? "per-instance unplaced"
                           : perInstance && table != inTable.end() ? "per-instance table"
                           : perInstance               ? "per-instance"
                           : table != inTable.end()    ? "table"
                           : all                       ? "paired"
                                                       : "partial";
      ++statusCount[status];
      char head[32];
      std::snprintf(head, sizeof(head), "%08X\t", first->retail);
      report += head + std::string(status) + "\t" + (agree ? orientText(ok->orient) : "-") + "\t" +
                (agree ? offsetText(ok->offset) : "-") + "\t";
      if (table != inTable.end()) {
        double orient[3][3];
        for (int i = 0; i < 9; ++i) {
          orient[i / 3][i % 3] = table->second->orient[i];
        }
        report += orientText(orient) + "\t" + offsetText(table->second->offset) + "\t";
      } else {
        report += "-\t-\t";
      }
      for (auto p = first; p != last; ++p) {
        char text[64];
        std::snprintf(text, sizeof(text), " %08X #%d %d ", p->object, p->entity, int(p->ok));
        report += (p == first ? "" : "; ") + p->room + text + orientText(p->orient) + " " + offsetText(p->offset);
      }
      report += '\n';
      int flips = 0;
      if (perInstance) {
        double maxOrient = 0, maxOffset = 0;
        for (auto a = first; a != last; ++a) {
          const double(*o)[3] = a->orient;
          const double det = o[0][0] * (o[1][1] * o[2][2] - o[1][2] * o[2][1]) -
                             o[0][1] * (o[1][0] * o[2][2] - o[1][2] * o[2][0]) +
                             o[0][2] * (o[1][0] * o[2][1] - o[1][1] * o[2][0]);
          flips += a->ok && det < 0;
          for (auto b = a + 1; b != last; ++b) {
            if (!a->ok || !b->ok) {
              continue;
            }
            for (int i = 0; i < 9; ++i) {
              maxOrient = std::max(maxOrient, std::fabs(a->orient[i / 3][i % 3] - b->orient[i / 3][i % 3]));
            }
            for (int i = 0; i < 3; ++i) {
              maxOffset = std::max(maxOffset, std::fabs(a->offset[i] - b->offset[i]));
            }
          }
        }
        ++orientSpread[decade(maxOrient)];
        ++offsetSpread[decade(maxOffset)];
        mirrored += flips;
        if (all && flips != 0 && table == inTable.end()) {
          // No runtime path for it (below) and no single placement to bake: it stays retail.
          --statusCount[status];
          ++statusCount["per-instance mirrored"];
        }
        char line[160];
        std::snprintf(line, sizeof(line), "%08X\t%d\t%.3g\t%.3g\t%d\n", first->retail, int(last - first),
                      maxOrient, maxOffset, flips);
        spreadReport += line;
      }
      if (perInstance && all && flips == 0 && table == inTable.end()) {
        // Remastered moved some of this model's objects on their own: the model converts once,
        // untransformed, and each object's draw goes through its own placement. A mirrored
        // placement stays out: the baked conversion swaps its winding, a runtime one could not.
        RetailModel& m = retailModels.emplace_back();
        std::copy(first->model.begin(), first->model.end(), m.uuid.begin());
        m.retail = first->retail;
        m.modelSpace = true;
        for (auto p = first; p != last; ++p) {
          PortRetailPlacement::Record& r = placementRecords.emplace_back();
          r.mrea = p->mrea;
          r.editorId = p->object;
          r.model = p->retail;
          for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 3; ++c) {
              r.correction[row * 4 + c] = float(p->orient[row][c]);
            }
            r.correction[row * 4 + 3] = float(p->offset[row]);
          }
        }
      } else if (agree && table == inTable.end()) {
        RetailModel& m = retailModels.emplace_back();
        std::copy(first->model.begin(), first->model.end(), m.uuid.begin());
        m.retail = first->retail;
        std::copy(&ok->orient[0][0], &ok->orient[0][0] + 9, &m.orient[0][0]);
        std::copy(ok->offset, ok->offset + 3, m.offset);
      }
      first = last;
    }
    std::string list;
    for (const RetailModel& m : retailModels) {
      list += UuidText(m.uuid) + " " + Hex8(m.retail) + " " +
              (m.modelSpace ? std::string("model space") : orientText(m.orient) + " " + offsetText(m.offset)) + "\n";
    }
    const std::vector<uint8_t> placementFile = PortRetailPlacement::Encode(placementRecords);
    list += "placements " + Hex64(Hash64(std::string(placementFile.begin(), placementFile.end()))) + "\n";
    {
      std::string counts;
      for (const auto& [status, n] : statusCount) {
        counts += " " + status + " " + std::to_string(n);
      }
      PortLog::Write("remastered import: retail models placed by the rooms:%s\n", counts.c_str());
      std::string spreads;
      for (const auto& [d, n] : orientSpread) {
        spreads += " 1e" + std::to_string(d) + ":" + std::to_string(n);
      }
      spreads += "; offset";
      for (const auto& [d, n] : offsetSpread) {
        spreads += " 1e" + std::to_string(d) + ":" + std::to_string(n);
      }
      PortLog::Write("remastered import: per-instance spread by decade, orient%s; %d mirrored placements; "
                     "%zu placements\n",
                     spreads.c_str(), mirrored, placementRecords.size());
    }
    if (!beginStage("retailmodels", std::to_string(ImportStage::kModels) + " list " + Hex64(Hash64(list)) + keyConverter,
                    false, "reports/parts/retailmodels.materials.tsv")) {
      std::error_code reportError;
      fs::create_directories(staging / "reports", reportError);
      writeFile(staging / "reports" / "retailmodels.tsv", std::vector<uint8_t>(report.begin(), report.end()));
      writeFile(staging / "reports" / "retailmodels.spread.tsv",
                std::vector<uint8_t>(spreadReport.begin(), spreadReport.end()));
      writeFile(staging / PortRetailPlacement::kFileName, placementFile);
      std::atomic<size_t> nextRetail{0};
      std::atomic<int> retailDone{0};
      std::mutex retailClaimMutex;
      std::unordered_set<uint32_t> retailClaimed;
      auto retailWork = [&](int worker) {
        YieldToGame();
        ConvertIO io = makeIO(worker, staging);
        io.claim = [&](uint32_t id) {
          std::lock_guard<std::mutex> lock(retailClaimMutex);
          return retailClaimed.insert(id).second;
        };
        Converter converter(std::move(io));
        for (size_t i = nextRetail++; i < retailModels.size() && !sCancel; i = nextRetail++) {
          const RetailModel& m = retailModels[i];
          ConvertOptions options;
          options.retail = m.retail;
          options.source = UuidText(m.uuid);
          // The placement's orient after kR2G, the default orient; a per-instance model keeps
          // the default, its placement applied at draw time.
          const ConvertOptions defaults;
          for (int r = 0; r < 3 && !m.modelSpace; ++r) {
            for (int c = 0; c < 3; ++c) {
              options.orient[r][c] = m.orient[r][0] * defaults.orient[0][c] + m.orient[r][1] * defaults.orient[1][c] +
                                     m.orient[r][2] * defaults.orient[2][c];
            }
          }
          if (!m.modelSpace) {
            std::copy(m.offset, m.offset + 3, options.offset);
          }
          options.pbr = true;
          std::string modelError;
          bool ok = false;
          // As for the models: an exception here would terminate the game.
          try {
            std::vector<uint8_t> raw;
            Model model;
            ok = remastered.ReadModel(m.uuid, raw, modelError) && ParseModel(raw.data(), raw.size(), model, modelError) &&
                 converter.Convert(model, options, modelError);
          } catch (const std::exception& e) {
            modelError = e.what();
          }
          if (ok) {
            ++retailDone;
          } else {
            AddLine("retail model " + Hex8(m.retail) + ": " + modelError);
          }
        }
      };
      workers.clear();
      for (int i = 1; i < std::max(threads, DefaultThreads()); ++i) {
        workers.emplace_back(retailWork, i);
      }
      retailWork(0);
      for (std::thread& worker : workers) {
        worker.join();
      }
      if (sCancel) {
        fail("Cancelled.");
        return;
      }
      made["retailmodels"].counts["models"] = retailDone.load();
      writeReportPart("retailmodels", false);
      endStage();
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
        uint32_t id = port::Fnv1a32(g.uuid.data(), g.uuid.size(), port::kFnv32Basis ^ uint32_t(level) * 0x9E3779B1u);
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
        const size_t now = ++seen;
        SetMessage("Converting room models (" + std::to_string(now) + "/" + std::to_string(geometry.size()) + ")");
        SetStepProgress(now, geometry.size());
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
          !MergeStringTable(original.data(), original.size(), strings, checkWording, merged, reworded, translated, strg)) {
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
    } else {
      const std::string disc = PortDisc::Name(PortDisc::Current());
      if (!writeFile(textFolder / kTextDiscName, std::vector<uint8_t>(disc.begin(), disc.end()))) {
        AddLine(std::string("text/") + kTextDiscName + ": cannot write");
      }
    }
    made["text"].counts["tables"] = textTables;
    made["text"].counts["strings"] = textStrings;
    made["text"].counts["translated"] = textTranslated;
    endStage();
  }
  // Remastered's typeface, which the port draws the disc's text with: the font
  // with the most characters, so that every language's text is covered.
  auto largestFont = [&](PortHdFont::Font& font, ModelUuid& atlas, std::string& fontError) {
    std::vector<uint8_t> raw;
    fontError = "not in the image";
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
  };
  bool fontWritten = false;
  if (beginStage("font", std::to_string(ImportStage::kText) + keyConverter, false)) {
    fontWritten = made["font"].counts["written"] != 0;
  } else {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> out;
    ModelUuid atlas{};
    PortHdFont::Font font;
    TxtrImage image;
    std::string fontError;
    largestFont(font, atlas, fontError);
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
      // Text panes are sized by the capital the port's text is fitted by.
      PortHdFont::Font font;
      ModelUuid atlas{};
      std::string fontError;
      largestFont(font, atlas, fontError);
      const PortHdFont::Glyph* const capital = font.Find(U'H');
      if (capital != nullptr && capital->height > 2.f * font.padding) {
        converter.SetGlyphInk(capital->height - 2.f * font.padding);
      } else {
        AddLine("hud: no capital in Remastered's font (" + fontError + "); the text keeps the disc's size");
      }
    }
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
    if (hudFrames != 0) {
      const std::string disc = PortDisc::Name(PortDisc::Current());
      if (!writeFile(hudFolder / kHudDiscName, std::vector<uint8_t>(disc.begin(), disc.end()))) {
        AddLine(std::string("hud/") + kHudDiscName + ": cannot write");
      }
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
    // The thermal visor post's heat gradient (port_thermal.h): raw RGBA, 256x4.
    {
      // a30fbad7-6022-44d0-b832-f8b13f04bb58, in the order the bytes are written, else the mixed-endian one.
      static constexpr ModelUuid kPlain = {0xa3, 0x0f, 0xba, 0xd7, 0x60, 0x22, 0x44, 0xd0,
                                           0xb8, 0x32, 0xf8, 0xb1, 0x3f, 0x04, 0xbb, 0x58};
      static constexpr ModelUuid kSwapped = {0xd7, 0xba, 0x0f, 0xa3, 0x22, 0x60, 0xd0, 0x44,
                                             0xb8, 0x32, 0xf8, 0xb1, 0x3f, 0x04, 0xbb, 0x58};
      std::vector<uint8_t> raw;
      TxtrImage lut;
      std::string lutError;
      if ((remastered.ReadTexture(kPlain, raw, lutError) || remastered.ReadTexture(kSwapped, raw, lutError)) &&
          DecodeTxtr(raw.data(), raw.size(), lut, lutError) && lut.width == 256 && lut.height == 4 &&
          lut.rgba.size() == 256 * 4 * 4) {
        // thermal.lut = the gradient (256x4 RGBA8) then the 64x64 R8 noise from the executable.
        std::vector<uint8_t> noise;
        std::string noiseError;
        if (!remastered.ExtractThermalNoiseBytes(noise, noiseError)) {
          AddLine("thermal visor noise: left out (" + noiseError + ")");
        } else {
          std::vector<uint8_t> file = lut.rgba;
          file.insert(file.end(), noise.begin(), noise.end());
          if (!makeIO(0, hudFolder).write("thermal.lut", file)) {
            AddLine("thermal visor gradient: cannot write");
          }
        }
      } else {
        AddLine("thermal visor gradient: " + (lutError.empty() ? std::string("unexpected size") : lutError));
      }
    }
    // The EnvironmentFX textures (port_envfx_file.h) as envfx.bin: the rain, snow and underwater
    // flakes and the fade gradient the native EnvFX draw samples. All four or no file.
    {
      PortEnvFx::Assets envfx;
      std::string envfxError;
      for (int i = 0; i < 4 && envfxError.empty(); ++i) {
        const char* guid = i == 0 ? PortEnvFx::kGradientGuid : PortEnvFx::kFlakeGuid[i - 1];
        PortEnvFx::Image& image = i == 0 ? envfx.gradient : envfx.flake[i - 1];
        ModelUuid plain{};
        ModelUuid swapped{};
        GuidUuids(guid, plain, swapped);
        std::vector<uint8_t> raw;
        std::string error;
        if (!remastered.ReadTexture(plain, raw, error) && !remastered.ReadTexture(swapped, raw, error)) {
          envfxError = std::string(guid) + ": " + error;
          break;
        }
        if (i == 0) {
          TxtrImage gradient;
          if (!DecodeTxtr(raw.data(), raw.size(), gradient, error)) {
            envfxError = std::string(guid) + ": " + error;
            break;
          }
          // DecodeTxtr's R8 is already (v, v, v, 255) or (v, 0, 0, 255); the draw reads .r.
          image.width = gradient.width;
          image.height = gradient.height;
          image.layers = 1;
          image.rgba.assign(gradient.rgba.size(), 0);
          for (size_t p = 0; p + 3 < gradient.rgba.size(); p += 4) {
            image.rgba[p] = gradient.rgba[p];
            image.rgba[p + 3] = 255;
          }
          continue;
        }
        if (!DecodeTxtrLayersRgba8(raw.data(), raw.size(), image.width, image.height, image.layers, image.rgba,
                                   error)) {
          // The underwater flake may be a plain 2D texture: one layer.
          TxtrImage single;
          if (!DecodeTxtr(raw.data(), raw.size(), single, error)) {
            envfxError = std::string(guid) + ": " + error;
            break;
          }
          image.width = single.width;
          image.height = single.height;
          image.layers = 1;
          image.rgba = std::move(single.rgba);
        }
      }
      PortEnvFx::Assets check;
      std::vector<uint8_t> file;
      if (envfxError.empty()) {
        file = PortEnvFx::Serialize(envfx);
        if (!PortEnvFx::Parse(file.data(), file.size(), check, envfxError)) {  // the sizes the draw expects
          envfxError = "unexpected size: " + envfxError;
        }
      }
      if (!envfxError.empty()) {
        AddLine("EnvironmentFX textures: " + envfxError);
      } else if (!makeIO(0, hudFolder).write("envfx.bin", file)) {
        AddLine("EnvironmentFX textures: cannot write");
      }
    }

    // Remastered's silhouette-shell textures (port_shell_file.h) as shell.bin: the strength
    // gradient the shell reads through, and the indirect noise map the player body and the
    // Samus doll each sample (build/mpr/kb topic/model-black-silhouette.md). All three or no file.
    {
      PortShell::Assets shell;
      std::string shellError;
      for (int i = 0; i < PortShell::kCount && shellError.empty(); ++i) {
        const char* guid = PortShell::kGuid[i];
        ModelUuid plain{};
        ModelUuid swapped{};
        GuidUuids(guid, plain, swapped);
        std::vector<uint8_t> raw;
        std::string error;
        TxtrImage tex;
        if (!remastered.ReadTexture(plain, raw, error) && !remastered.ReadTexture(swapped, raw, error)) {
          shellError = std::string(guid) + ": " + error;
          break;
        }
        if (!DecodeTxtr(raw.data(), raw.size(), tex, error)) {
          shellError = std::string(guid) + ": " + error;
          break;
        }
        if (tex.width < 1 || tex.height < 1 || tex.width > 4096 || tex.height > 4096) {
          shellError = std::string(guid) + ": " + std::to_string(tex.width) + "x" + std::to_string(tex.height);
          break;
        }
        // The whole chain, down to 1x1: the shader's scene lookup is a textureLod.
        BuildShellImage(tex, shell.image[i]);
      }
      PortShell::Assets check;
      std::vector<uint8_t> file;
      if (shellError.empty()) {
        file = PortShell::Serialize(shell);
        if (!PortShell::Parse(file.data(), file.size(), check, shellError)) {  // the sizes the draw expects
          shellError = "unexpected size: " + shellError;
        }
      }
      if (!shellError.empty()) {
        AddLine("silhouette shell textures: " + shellError);
      } else if (!makeIO(0, hudFolder).write("shell.bin", file)) {
        AddLine("silhouette shell textures: cannot write");
      }
    }

    // Remastered's frozen-enemy material (port_icy_file.h) as icy.bin: the icy MATI the render
    // loose data names, its three textures and four constants (build/mpr/kb
    // topic/frozen-enemy-icy-render.md). All of it or no file, and frozen enemies draw as retail.
    {
      PortIcy::Assets icy;
      std::string icyError;
      std::vector<uint8_t> raw;
      ModelUuid material{};
      MaterialParams params;
      // A GUID as the file holds it, else with its first three groups swapped.
      const auto swap = [](ModelUuid id) {
        std::swap(id[0], id[3]);
        std::swap(id[1], id[2]);
        std::swap(id[4], id[5]);
        std::swap(id[6], id[7]);
        return id;
      };
      std::string readError;
      if (!remastered.ReadTweak("RenderLooseDataMP1", raw, readError) ||
          !RenderIcyMaterial(raw.data(), raw.size(), material, readError)) {
        icyError = "RenderLooseDataMP1: " + readError;
      } else if (!remastered.ReadEffectAsset(kMATI, material, raw, readError) &&
                 !remastered.ReadEffectAsset(kMATI, swap(material), raw, readError)) {
        icyError = "the icy material: " + readError;
      } else if (!ReadMaterialParams(raw, params)) {
        icyError = "the icy material does not parse";
      }
      for (int i = 0; i < PortIcy::kCount && icyError.empty(); ++i) {
        const auto texture = params.textures.find(PortIcy::kImageTag[i]);
        if (texture == params.textures.end()) {
          icyError = std::string("the icy material has no ") + PortIcy::kImageTag[i];
          break;
        }
        std::string error;
        TxtrImage tex;
        if (!remastered.ReadTexture(texture->second, raw, error) &&
            !remastered.ReadTexture(swap(texture->second), raw, error)) {
          icyError = std::string(PortIcy::kImageTag[i]) + ": " + error;
        } else if (!DecodeTxtr(raw.data(), raw.size(), tex, error)) {
          icyError = std::string(PortIcy::kImageTag[i]) + ": " + error;
        } else if (tex.width < 1 || tex.height < 1 || tex.width > 4096 || tex.height > 4096) {
          icyError = std::string(PortIcy::kImageTag[i]) + ": " + std::to_string(tex.width) + "x" +
                     std::to_string(tex.height);
        } else {
          BuildShellImage(tex, icy.image[i]);
        }
      }
      for (int i = 0; i < PortIcy::kVectors && icyError.empty(); ++i) {
        const auto vector = params.vectors.find(PortIcy::kVectorTag[i]);
        if (vector == params.vectors.end()) {
          icyError = std::string("the icy material has no ") + PortIcy::kVectorTag[i];
        } else {
          icy.vector[i] = vector->second;
        }
      }
      PortIcy::Assets check;
      std::vector<uint8_t> file;
      if (icyError.empty()) {
        file = PortIcy::Serialize(icy);
        if (!PortIcy::Parse(file.data(), file.size(), check, icyError)) {
          icyError = "unexpected size: " + icyError;
        }
      }
      if (!icyError.empty()) {
        AddLine("frozen-enemy material: " + icyError);
      } else if (!makeIO(0, hudFolder).write("icy.bin", file)) {
        AddLine("frozen-enemy material: cannot write");
      }
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
      return port::ReadAllText(in);
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
      PortLog::Write("remastered import: %zu listed file(s) are missing from the staging folder (first: %s)\n",
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
  PortLog::Write("remastered import: %s\n", textures.Summary().c_str());
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

}  // namespace ImportDetail

using namespace ImportDetail;

std::string ImportRootFolder(std::string* error) {
#if defined(__ANDROID__)
  const PortRemasteredStorage::RootChoice choice =
      PortRemasteredStorage::ResolveRoot(true, InternalStoragePath(), std::string());
#else
  const PortRemasteredStorage::RootChoice choice =
      PortRemasteredStorage::ResolveRoot(false, std::string(), PortMods::Folder());
#endif
  if (choice.folder.empty()) {
    if (error != nullptr) {
      *error = choice.error.empty() ? "There is no mods folder to write to." : choice.error;
    }
    return {};
  }
  // The root has to exist before anything is staged in it; SDL's internal storage path does.
  std::error_code ec;
  fs::create_directories(PathFromString(choice.folder), ec);
  ec.clear();
  if (!fs::is_directory(PathFromString(choice.folder), ec)) {
    if (error != nullptr) {
      *error = "Cannot use the import folder " + choice.folder + (ec ? ": " + ec.message() : "");
    }
    return {};
  }
  return choice.folder;
}

bool UsesPrivateImportRoot() {
#if defined(__ANDROID__)
  return PortRemasteredStorage::ResolveRoot(true, InternalStoragePath(), std::string()).kind ==
         PortRemasteredStorage::Root::kPrivateStorage;
#else
  return false;
#endif
}

std::string InstalledModFolder() {
  const std::string root = ImportRootFolder();
  if (root.empty()) {
    return {};
  }
  // On desktop the root is the mods folder, whose every folder the mod scan already reads.
  const std::string mods = PortMods::Folder();
  if (!mods.empty() && PortRemasteredStorage::WithSeparator(mods) == root) {
    return {};
  }
  std::error_code ec;
  const fs::path installed = PathFromString(root) / kImportModName;
  return fs::is_directory(installed, ec) ? PathString(installed) : std::string();
}

std::string DefaultKeysPath() {
  const char* home = port::HomeFolder();
  if (home == nullptr) {
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
  // A finished import that has not been moved into place yet is the newer mod; the movies go into
  // the import's own folder (port_remastered_storage.h), never into a legacy one in the shared mods
  // folder, so on Android a player who still has only that one is asked for a full import.
  std::string rootError;
  const std::string root = ImportRootFolder(&rootError);
  const fs::path staging = root.empty() ? fs::path() : PathFromString(root) / kStagingName;
  std::error_code ec;
  const std::string legacy = LegacySharedImport();
  bool legacyExists = false;
  if (!legacy.empty()) {
    legacyExists = fs::is_directory(PathFromString(legacy), ec);
  }
  const bool privateRoot = UsesPrivateImportRoot();
  PortRemasteredStorage::MovieTarget target;
  if (!staging.empty()) {
    target = PortRemasteredStorage::ChooseMovieTarget(
        privateRoot, PathString(staging), fs::exists(staging / kMarkerName, ec),
        PathString(staging.parent_path() / kImportModName),
        fs::is_directory(staging.parent_path() / kImportModName, ec), legacy, legacyExists);
  }
  const fs::path mod = target.folder.empty() ? fs::path() : PathFromString(target.folder);
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mod.empty()) {
    sState.finished = true;
    sState.message = staging.empty() ? (rootError.empty() ? "There is no mods folder to write to." : rootError)
                                     : target.error;
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
  // The root the import is made in (port_remastered_storage.h). On Android its absence is an error
  // that names itself; the shared mods folder is not a fallback.
  std::string rootError;
  const std::string root = ImportRootFolder(&rootError);
  const fs::path staging = root.empty() ? fs::path() : PathFromString(root) / kStagingName;
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (staging.empty()) {
    sState.finished = true;
    sState.message = rootError.empty() ? "There is no mods folder to write to." : rootError;
    return false;
  }
  // A first import writes everything (docs/MODS.md: about 1 GB, 6.5 GB with every
  // room's geometry, plus 0.6 GB of movies); one with an import to reuse links
  // most of its files instead, so only a first one is checked. Refused up front
  // rather than failing at some write an hour in.
  {
    std::error_code ec;
    const fs::path target = staging.parent_path() / kImportModName;
    const fs::space_info space = fs::space(staging.parent_path(), ec);
    if (!ec && !fs::exists(target, ec) && !fs::exists(HeldFolder(staging), ec)) {
      const std::string geometry = GeometryChoice();
      const bool allGeometry = geometry == "all";
      double needGb = allGeometry ? 6.5 : geometry == "none" ? 1.0 : 2.0;
      // Not WantsMovies: it reports a bad value through the lock held here.
      needGb += !Parts().movies || MoviesTurnedOff() ? 0.0 : 0.6;
      const double freeGb = double(space.available) / (1024.0 * 1024.0 * 1024.0);
      if (freeGb < needGb) {
        char text[256];
        std::snprintf(text, sizeof text,
                      "Not enough free disk space: the import needs about %.1f GB and the import folder's drive "
                      "has %.1f GB free.%s",
                      needGb, freeGb, allGeometry ? " Leaving the room geometry out needs less." : "");
        sState.finished = true;
        sState.message = text;
        return false;
      }
    }
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
  PortLog::Write("remastered: writing %s textures\n", TextureFormatName());
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(Run, nspPath, keysPath, threads, staging);
  return true;
}


void SetImportParts(const ImportParts& parts) {
  std::lock_guard<std::mutex> lock(sPartsMutex);
  sParts = parts;
}

ImportState ImportStatus() {
  std::lock_guard<std::mutex> lock(sStateMutex);
  ImportState state = sState;
  state.cancelling = state.running && sCancel.load();
  return state;
}

void CancelImport() { sCancel = true; }

void SetRemakeAll(bool remake) { sRemakeAll = remake; }
bool RemakeAll() { return sRemakeAll; }

void StopImport() {
  sCancel = true;
  if (sThread.joinable()) {
    sThread.join();
  }
}

namespace {

bool MoveIntoPlace(const fs::path& staging) {
  std::error_code ec;
  // Whatever a quit left half deleted.
  SweepTrash(staging.parent_path());
  const fs::path held = HeldFolder(staging);
  if (fs::exists(staging / kMarkerName, ec)) {
    // An older import the finished one was made from, left if the game quit just after it.
    if (fs::exists(held, ec)) {
      PortLog::Write("metroid_prime_port: discarding held import %s\n", held.string().c_str());
    }
    Discard(held);
  } else {
    Discard(staging);
    // A finished import held aside by one that never finished (the game quit during it).
    if (!fs::exists(held / kMarkerName, ec)) {
      if (fs::exists(held, ec)) {
        PortLog::Write("metroid_prime_port: discarding unfinished held import %s\n",
                       held.string().c_str());
      }
      Discard(held);
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
  const fs::path backup = staging.parent_path() / PortRemasteredStorage::kBackupName;
  const bool hadTarget = fs::exists(target, ec);
  if (hadTarget) {
    if (fs::exists(backup, ec)) {
      PortLog::Write("metroid_prime_port: discarding old backup %s\n", backup.string().c_str());
    }
    Discard(backup);
    fs::rename(target, backup, ec);
    if (ec) {
      PortLog::Write("metroid_prime_port: could not set aside %s: %s\n", target.string().c_str(),
                     ec.message().c_str());
      return false;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    PortLog::Write("metroid_prime_port: could not move the imported mod to %s: %s\n",
                   target.string().c_str(), ec.message().c_str());
    if (hadTarget) {
      std::error_code restoreError;
      fs::rename(backup, target, restoreError);
      if (restoreError) {
        PortLog::Write("metroid_prime_port: could not restore %s: %s\n", target.string().c_str(),
                       restoreError.message().c_str());
      }
    }
    return false;
  }
  if (hadTarget) {
    Discard(backup);
  }
  fs::remove(target / kMarkerName, ec);
  return true;
}

} // namespace

bool StartInstall() {
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
  std::error_code ec;
  if (staging.empty() || !fs::exists(staging / kMarkerName, ec)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(sStateMutex);
  // The import's result is shown again once the mod is in place.
  ImportState result = sState;
  sCancel = false;
  sState.running = true;
  sState.installing = true;
  sState.finished = false;
  sState.done = sState.total = 0;
  sState.step = sState.steps = sState.stepDone = sState.stepTotal = 0;
  sState.message = "Moving the mod into place";
  sThread = std::thread([staging, result]() mutable {
    if (!MoveIntoPlace(staging)) {
      // Left staged: the next mods reload or start tries again.
      result.lines.push_back("The mod could not be moved into place yet; the next mods reload tries again.");
    }
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState = std::move(result);
  });
  return true;
}

bool ApplyPendingImport() {
  const fs::path staging = StagingFolder();
  // A running import is still writing there.
  if (ImportStatus().running) {
    return false;
  }
  const bool dread = ApplyPendingDreadImport();
  if (staging.empty()) {
    return dread;
  }
  return MoveIntoPlace(staging) || dread;
}

void RemoveLegacyImport() {
#if defined(__ANDROID__)
  std::string rootError;
  const std::string root = ImportRootFolder(&rootError);
  const std::string mods = PortMods::Folder();
  if (root.empty() || mods.empty()) {
    return;
  }
  const fs::path modsPath = PathFromString(mods);
  // A deletion a kill interrupted resumes here.
  SweepTrash(modsPath);
  if (ImportStatus().running) {
    return;
  }
  std::error_code ec;
  const fs::path installed = PathFromString(root) / kImportModName;
  const bool installedUsable = fs::is_regular_file(installed / kManifestName, ec) &&
                               !fs::exists(installed / kMarkerName, ec);
  const fs::path legacy = modsPath / kImportModName;
  const fs::file_status legacyStatus = fs::symlink_status(legacy, ec);
  const bool legacyIsRealDir = !ec && fs::is_directory(legacyStatus);
  bool samePath = false;
  if (legacyIsRealDir) {
    std::error_code a;
    std::error_code b;
    const fs::path canonLegacy = fs::canonical(legacy, a);
    const fs::path canonInstalled = fs::canonical(installed, b);
    samePath = a || b || canonLegacy == canonInstalled;
  }
  if (!PortRemasteredStorage::ShouldDeleteLegacy(true, installedUsable, legacyIsRealDir, samePath)) {
    return;
  }
  PortLog::Write("metroid_prime_port: deleting the old Remastered import in the mods folder (%s), replaced by "
                 "the app's own copy\n",
                 legacy.string().c_str());
  Discard(legacy);
#endif
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
  // The root the files actually went into: the mods folder on desktop, the app's own storage on
  // Android (port_remastered_storage.h).
  std::string rootError;
  const std::string root = ImportRootFolder(&rootError);
  if (root.empty()) {
    std::fprintf(stderr, "%s\n", rootError.c_str());
    return 1;
  }
  if (!ApplyPendingImport()) {
    std::fprintf(stderr, "could not move the mod into %s\n", root.c_str());
    return 1;
  }
  std::printf("installed as %s%s\n", root.c_str(), kImportModName);
  return 0;
}

}  // namespace PortRemastered
