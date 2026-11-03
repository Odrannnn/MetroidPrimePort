// Writes one Remastered world's .roomenv files through PortRemastered::WriteWorldRoomEnvs,
// for comparing against the reference writer (build/mpr/roomtools/envwrite.py). Everything
// it reads is the developer's own data: the romfs, and the retail MLVL/MREA files dumped
// as <TYPE>_<ID8>.bin into one directory.
//
//   port_remastered_room_tool <romfs dir> <retail dump dir> <world dir name> <out dir>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

#include "port_remastered_room.h"

using namespace PortRemastered;
namespace fs = std::filesystem;

namespace {

class FileReader {
public:
  bool Open(const std::string& path, std::string& error) {
    m_stream.open(path, std::ios::binary);
    if (!m_stream.is_open()) {
      error = "could not open '" + path + "'";
      return false;
    }
    m_stream.seekg(0, std::ios::end);
    m_size = uint64_t(m_stream.tellg());
    m_stream.seekg(0, std::ios::beg);
    return true;
  }
  uint64_t Size() const { return m_size; }
  bool Read(uint64_t offset, void* out, size_t size) {
    m_stream.clear();
    m_stream.seekg(std::streamoff(offset));
    if (!m_stream) {
      return false;
    }
    m_stream.read(static_cast<char*>(out), std::streamsize(size));
    return m_stream.gcount() == std::streamsize(size);
  }

private:
  std::ifstream m_stream;
  uint64_t m_size = 0;
};

struct OpenPak {
  std::string name;
  FileReader reader;
  Pak pak;
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: %s <romfs dir> <retail dump dir> <world dir name> <out dir>\n", argv[0]);
    return 2;
  }
  const std::string romfs = argv[1], dump = argv[2], world = argv[3], outDir = argv[4];
  uint32_t mlvl = 0;
  for (const RoomWorld& w : RoomWorlds()) {
    if (world == w.dir) {
      mlvl = w.mlvl;
    }
  }
  if (mlvl == 0) {
    std::fprintf(stderr, "unknown world '%s'\n", world.c_str());
    return 2;
  }

  const fs::path dir = fs::path(romfs) / "Worlds" / "MP1" / ("!" + world);
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string file = entry.path().filename().string();
    if (file.size() > 4 && file.compare(file.size() - 4, 4, ".pak") == 0) {
      std::string n = file.substr(0, file.size() - 4);
      n.erase(0, n.find_first_not_of('!'));
      names.push_back(n);
    }
  }
  // The reference extracted with `find | sort` under a locale that ignores punctuation and case, and the
  // world shift is picked from the first rooms in that order, so match it (it moves the shift by ulps only).
  auto key = [](const std::string& n) {
    std::string k;
    for (char c : n + ".pak") {
      if (std::isalnum(static_cast<unsigned char>(c))) {
        k += char(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    return k;
  };
  std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
    const std::string ka = key(a), kb = key(b);
    return ka != kb ? ka < kb : a < b;
  });

  std::vector<std::unique_ptr<OpenPak>> paks;
  std::vector<RoomPak> rooms;
  RoomPak master;
  for (const std::string& n : names) {
    auto p = std::make_unique<OpenPak>();
    p->name = n;
    std::string error;
    const std::string path = (dir / (n == world ? "!" + n + ".pak" : n + ".pak")).string();
    if (!p->reader.Open(path, error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    FileReader* r = &p->reader;
    if (!p->pak.Open([r](uint64_t o, void* out, size_t s) { return r->Read(o, out, s); }, p->reader.Size(), error)) {
      std::fprintf(stderr, "%s: %s\n", n.c_str(), error.c_str());
      continue;
    }
    (n == world ? master : rooms.emplace_back()) = RoomPak{n, &p->pak};
    paks.push_back(std::move(p));
  }
  rooms.erase(std::remove_if(rooms.begin(), rooms.end(), [&](const RoomPak& r) { return r.name == world; }),
              rooms.end());

  // Every pak outside this world (the other worlds, universeRoomMP1, ...), for the assets a room shares.
  std::vector<RoomPak> others;
  std::vector<std::string> otherFiles;
  for (const auto& entry : fs::recursive_directory_iterator(fs::path(romfs) / "Worlds" / "MP1")) {
    if (entry.path().extension() == ".pak" && entry.path().parent_path() != dir) {
      otherFiles.push_back(entry.path().string());
    }
  }
  std::sort(otherFiles.begin(), otherFiles.end());
  for (const std::string& file : otherFiles) {
    auto p = std::make_unique<OpenPak>();
    std::string error;
    FileReader* r = &p->reader;
    if (!p->reader.Open(file, error) ||
        !p->pak.Open([r](uint64_t o, void* out, size_t s) { return r->Read(o, out, s); }, p->reader.Size(), error)) {
      continue;
    }
    others.push_back(RoomPak{fs::path(file).stem().string(), &p->pak});
    paks.push_back(std::move(p));
  }

  fs::create_directories(outDir);
  RoomIO io;
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    char file[64];
    const char tag[5] = {char(type >> 24), char(type >> 16), char(type >> 8), char(type), 0};
    std::snprintf(file, sizeof file, "/%s_%08X.bin", tag, id);
    return ReadFile(dump + file, out);
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    std::ofstream f(fs::path(outDir) / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
  };
  io.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
  std::vector<RoomPak> all = others;
  all.push_back(master);
  all.insert(all.end(), rooms.begin(), rooms.end());
  const std::set<std::array<uint8_t, 16>> neverLoaded = NeverLoadedUnits(all);
  std::printf("%zu layer units never load\n", neverLoaded.size());
  io.neverLoads = [&](const std::array<uint8_t, 16>& unit) { return neverLoaded.count(unit) != 0; };

  int written = 0;
  std::string error;
  if (!WriteWorldRoomEnvs(mlvl, master, rooms, others, io, written, error)) {
    std::fprintf(stderr, "%s: %s\n", world.c_str(), error.c_str());
    return 1;
  }
  std::printf("%d files\n", written);
  return 0;
}
