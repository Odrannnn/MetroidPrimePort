// The Echoes suits import in the game (port_remastered_import.h, StartEchoesSuitsImport): the user's
// Metroid Prime 2: Echoes GameCube image (or a Trilogy image's MP2 folder) and the open disc, through PortEchoesSuits::Import
// (port_echoes_suits.h), into <mods>/echoes-suits. Like the Dread Samus import it shares the
// Remastered import's state, thread and cancel flag; only one of them runs at a time.

#include "port_remastered_import_internal.h"

#include "port_echoes_suits.h"

#include <aurora/dvd.h>

#include <cctype>
#include <cstring>

namespace PortRemastered {
namespace ImportDetail {
namespace {

constexpr const char* kEchoesStaging = ".echoes-suits.staging";
constexpr const char* kEchoesOld = ".echoes-suits.old";
constexpr const char* kEchoesDone = ".done";  // in the staging folder once it is complete

fs::path Utf8Path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

// The Echoes image's PAKs, indexed by resource; entries read and decompressed on demand.
class EchoesDisc {
public:
  ~EchoesDisc() {
    if (m_image != nullptr) {
      aurora_disc_image_close(m_image);
    }
  }

  bool Open(const std::string& path, std::string& error) {
    char gameId[6] = {};
    m_image = aurora_disc_image_open(path.c_str(), gameId, nullptr, nullptr);
    if (m_image == nullptr) {
      error = "Could not open the image (not a GameCube or Wii disc image, or unreadable).";
      return false;
    }
    // Metroid Prime Trilogy (R3ME01, R3MP01) carries Echoes' PAKs, the same files as the GameCube
    // disc's, in its MP2 folder; MP1 and MP3 have PAKs of the same names.
    const bool trilogy = std::memcmp(gameId, "R3M", 3) == 0;
    if (!trilogy && std::memcmp(gameId, "G2M", 3) != 0) {
      error = "This is not Metroid Prime 2: Echoes for GameCube or Metroid Prime Trilogy (game " +
              std::string(gameId, 6) + ").";
      return false;
    }
    struct Pak {
      uint32_t index;
      uint32_t size;
      std::string name;
    };
    std::vector<Pak> paks;
    aurora_disc_image_list(
        m_image,
        [](u32 index, const char* name, u32 size, void* user) {
          const size_t length = std::strlen(name);
          if (length > 4) {
            std::string ext(name + length - 4);
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".pak") {
              static_cast<std::vector<Pak>*>(user)->push_back({index, size, name});
            }
          }
        },
        &paks);
    for (const auto& [index, size, name] : paks) {
      if (trilogy && aurora_disc_image_find(m_image, ("MP2/" + name).c_str(), nullptr) != s32(index)) {
        continue;
      }
      std::vector<uint8_t> header;
      PortMods::PakTable table;
      size_t needed = 0x10000;
      bool parsed = false;
      while (needed <= (64u << 20)) {
        header.resize(std::min<size_t>(needed, size));
        if (!ReadAt(index, 0, header.data(), header.size())) {
          break;
        }
        if (PortMods::ParsePakTable(header.data(), header.size(), table, needed)) {
          parsed = true;
          break;
        }
        if (header.size() >= size || needed <= header.size()) {
          break;
        }
      }
      if (!parsed) {
        continue;
      }
      for (const PortMods::PakResource& resource : table.resources) {
        m_resources.try_emplace(Key(resource.type, resource.id), Entry{index, resource});
      }
    }
    if (m_resources.empty()) {
      error = "The image has no Echoes PAKs.";
      return false;
    }
    return true;
  }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    const auto found = m_resources.find(Key(type, id));
    if (found == m_resources.end()) {
      return false;
    }
    const Entry& entry = found->second;
    std::vector<uint8_t> raw(entry.resource.size);
    if (!ReadAt(entry.file, entry.resource.offset, raw.data(), raw.size())) {
      return false;
    }
    if (entry.resource.compressed == 0) {
      out = std::move(raw);
      return true;
    }
    std::string why;
    if (!PortEchoesSuits::Decompress(raw.data(), raw.size(), out, &why)) {
      char id8[9];
      std::snprintf(id8, sizeof(id8), "%08X", id);
      PortLog::Write("echoes: %s: %s\n", id8, why.c_str());
      return false;
    }
    return true;
  }

private:
  struct Entry {
    uint32_t file;
    PortMods::PakResource resource;
  };
  static uint64_t Key(uint32_t type, uint32_t id) { return (uint64_t(type) << 32) | id; }

  bool ReadAt(uint32_t file, uint64_t offset, uint8_t* out, size_t size) {
    void* handle = aurora_disc_image_file_open(m_image, file);
    if (handle == nullptr) {
      return false;
    }
    size_t done = 0;
    if (aurora_dvd_base_seek(handle, int64_t(offset), 0) == int64_t(offset)) {
      while (done < size) {
        const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
        if (got <= 0) {
          break;
        }
        done += size_t(got);
      }
    }
    aurora_dvd_base_close(handle);
    return done == size;
  }

  AuroraDiscImage* m_image = nullptr;
  std::map<uint64_t, Entry> m_resources;
};

// The finished staging folder in place of <mods>/echoes-suits. False (staging kept, for the next
// start's ApplyPendingImport) when a rename fails, as it can on Windows with a file still open.
bool MoveEchoesIntoPlace(const fs::path& mods) {
  std::error_code ec;
  const fs::path staging = mods / kEchoesStaging;
  if (!fs::exists(staging / kEchoesDone, ec)) {
    return false;
  }
  const fs::path target = mods / PortEchoesSuits::kModName;
  const fs::path old = mods / kEchoesOld;
  fs::remove_all(old, ec);
  ec.clear();
  const bool had = fs::exists(target, ec);
  if (had) {
    fs::rename(target, old, ec);
    if (ec) {
      PortLog::Write("echoes: could not set aside %s: %s\n", Utf8(target).c_str(), ec.message().c_str());
      return false;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    PortLog::Write("echoes: could not move the import to %s: %s\n", Utf8(target).c_str(), ec.message().c_str());
    if (had) {
      std::error_code restore;
      fs::rename(old, target, restore);
    }
    return false;
  }
  fs::remove(target / kEchoesDone, ec);
  fs::remove_all(old, ec);
  return true;
}

void RunEchoes(std::string imagePath, fs::path mods) {
  auto fail = [](const std::string& message) { Finish(false, sCancel ? std::string("Cancelled.") : message); };
  SetMessage("Opening the Echoes image");
  std::string error;
  EchoesDisc echoes;
  if (!echoes.Open(imagePath, error)) {
    fail(error);
    return;
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }
  const fs::path staging = mods / kEchoesStaging;
  std::error_code ec;
  fs::remove_all(staging, ec);
  ec.clear();
  fs::create_directories(staging, ec);
  if (ec) {
    fail("Cannot write " + Utf8(staging) + ": " + ec.message());
    return;
  }
  PortEchoesSuits::Io io;
  io.echoes = [&echoes](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return echoes.Read(type, id, out); };
  io.retail = [&retail](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
  io.retailId = [&retail](uint32_t id) { return retail.HasId(id); };
  io.write = [&staging](const std::string& name, const std::vector<uint8_t>& data) {
    return WriteFile(staging / Utf8Path(name), data);
  };
  io.log = [](const std::string& line) {
    PortLog::Write("%s\n", line.c_str());
    AddLine(line);
  };
  io.cancelled = [] { return sCancel.load(); };
  io.progress = [](size_t done, size_t total) {
    {
      std::lock_guard<std::mutex> lock(sStateMutex);
      sState.done = int(done);
      sState.total = int(total);
    }
    SetMessage("Converting Samus's suits (" + std::to_string(done) + " of " + std::to_string(total) + ")");
  };
  PortEchoesSuits::Result result;
  if (!PortEchoesSuits::Import(io, result, error)) {
    fs::remove_all(staging, ec);
    fail(error);
    return;
  }
  const std::string stamp =
      std::to_string(PortEchoesSuits::kVersion) + "\n" + std::string(MP_BUILD_REVISION) + "\n";
  if (!WriteFile(staging / PortEchoesSuits::kStampName, std::vector<uint8_t>(stamp.begin(), stamp.end())) ||
      !WriteFile(staging / kEchoesDone, {})) {
    fs::remove_all(staging, ec);
    fail("Cannot write " + Utf8(staging));
    return;
  }
  if (sCancel.load()) {
    fs::remove_all(staging, ec);
    fail("cancelled");
    return;
  }
  const bool placed = MoveEchoesIntoPlace(mods);
  std::string message = std::to_string(result.converted + result.arms) + " suit models, " +
                        std::to_string(result.balls) + " Morph Balls and the arm cannon converted";
  message += placed ? "." : "; it is installed at the next start.";
  Finish(true, message);
}

}  // namespace

bool ApplyPendingEchoesImport() {
  const std::string mods = PortMods::Folder();
  return !mods.empty() && MoveEchoesIntoPlace(Utf8Path(mods));
}

}  // namespace ImportDetail

using namespace ImportDetail;

bool StartEchoesSuitsImport(const std::string& imagePath) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const std::string mods = PortMods::Folder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mods.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  if (imagePath.empty()) {
    sState.finished = true;
    sState.message = "Pick the Metroid Prime 2: Echoes disc image.";
    return false;
  }
  PortLog::Write("echoes: importing Samus's suits from %s\n", imagePath.c_str());
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(RunEchoes, imagePath, Utf8Path(mods));
  return true;
}

int RunEchoesSuitsImportFromCommandLine(const std::string& imagePath) {
  if (!StartEchoesSuitsImport(imagePath)) {
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
  return state.ok ? 0 : 1;
}

}  // namespace PortRemastered
