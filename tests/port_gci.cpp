#include "port_gci.h"
#include "port_test.h"

#include <aurora/aurora.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Aurora's card library wants these from its host.
namespace aurora {
AuroraConfig g_config{};
char g_gameName[4] = {'G', 'M', '8', 'E'};
void log_internal(AuroraLogLevel, const char*, const char* message, unsigned int length) noexcept {
  std::fprintf(stderr, "%.*s\n", static_cast< int >(length), message);
}
} // namespace aurora

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> MakeGci(const char* game, const char* name, uint16_t blocks, uint8_t fill) {
  std::vector<uint8_t> gci(PortGci::kHeaderSize + blocks * PortGci::kBlockSize, fill);
  std::memset(gci.data(), 0, PortGci::kHeaderSize);
  std::memcpy(gci.data(), game, 4);
  std::memcpy(gci.data() + 4, "01", 2);
  gci[6] = 0xFF;
  std::strncpy(reinterpret_cast< char* >(gci.data() + 8), name, 32);
  gci[0x36] = 0;
  gci[0x37] = 5; // first block
  gci[0x38] = uint8_t(blocks >> 8);
  gci[0x39] = uint8_t(blocks);
  std::memset(gci.data() + 0x2C, 0xFF, 4); // no icon
  std::memset(gci.data() + 0x3C, 0xFF, 4); // no comment
  return gci;
}

std::vector<uint8_t> Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Write(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast< const char* >(data.data()), std::streamsize(data.size()));
}

size_t CountGci(const fs::path& folder) {
  size_t count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    count += entry.path().extension() == ".gci" ? 1 : 0;
  }
  return count;
}
} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "port-gci-tests";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  // Header checks.
  {
    const auto good = MakeGci("GM8E", "MetroidPrime A", 3, 0x11);
    PortGci::Header header;
    std::string error;
    CHECK_MSG(PortGci::ParseHeader(good.data(), good.size(), header, error), "good header parses");
    CHECK_MSG(header.fileName == "MetroidPrime A" && header.blockCount == 3, "header fields");
    CHECK_MSG(PortGci::IsGameFile(header), "GM8E/01 is the game's");
    CHECK_MSG(PortGci::DiskName(header) == "01-GM8E-MetroidPrime A.gci", "Dolphin disk name");
    auto shortData = good;
    shortData.pop_back();
    CHECK_MSG(!PortGci::ParseHeader(shortData.data(), shortData.size(), header, error),
              "size mismatch rejected");
    auto zero = MakeGci("GM8E", "x", 1, 0);
    zero[0x39] = 0;
    CHECK_MSG(!PortGci::ParseHeader(zero.data(), zero.size(), header, error), "zero blocks rejected");
    const auto slashes = MakeGci("GM8E", "a/b:c", 1, 0);
    PortGci::ParseHeader(slashes.data(), slashes.size(), header, error);
    CHECK_MSG(PortGci::DiskName(header) == "01-GM8E-a_b_c.gci", "disk name sanitised");
  }

  // Import into a card folder: new, then replacing the same identity kept
  // under another disk name, which moves the old file aside.
  const fs::path card = root / "card";
  fs::create_directories(card);
  Write(card / "renamed.gci", MakeGci("GM8E", "MetroidPrime A", 3, 0x22));
  Write(card / "01-GZLE-other.gci", MakeGci("GZLE", "other", 1, 0x33));
  const fs::path source = root / "in.gci";
  const auto incoming = MakeGci("GM8E", "MetroidPrime A", 3, 0x44);
  Write(source, incoming);
  {
    const auto report = PortGci::ImportFile(source, card, root / "scratch");
    CHECK_MSG(report.copied == 1 && report.replaced == 1 && report.skipped == 0, "import replaces");
    CHECK_MSG(Read(card / "01-GM8E-MetroidPrime A.gci") == incoming, "imported bytes");
    CHECK_MSG(!fs::exists(card / "renamed.gci"), "old identity moved");
    CHECK_MSG(CountGci(card / "_replaced") == 1, "old file kept in _replaced");
    CHECK_MSG(fs::exists(card / "01-GZLE-other.gci"), "other game untouched");
    CHECK_MSG(PortGci::GameFiles(card).size() == 1, "one game file");
  }
#ifndef _WIN32
  // An entry that cannot be inspected (a symlink loop) is passed over; it is
  // not a failure to list the folder.
  {
    const fs::path odd = root / "odd-card";
    fs::create_directories(odd);
    std::error_code ec;
    fs::create_symlink("loop.gci", odd / "loop.gci", ec);
    CHECK_MSG(!ec, "symlink loop created");
    const auto report = PortGci::ImportFile(source, odd, root / "scratch");
    CHECK_MSG(report.copied == 1 && report.skipped == 0, "import beside an unreadable entry");
  }
#endif
  // Another game's .gci is refused.
  {
    Write(root / "zelda.gci", MakeGci("GZLE", "zelda", 1, 0));
    const auto report = PortGci::ImportFile(root / "zelda.gci", card, root / "scratch");
    CHECK_MSG(report.copied == 0 && report.skipped == 1, "other game's save refused");
    const auto junk = std::vector<uint8_t>(100, 7);
    Write(root / "junk.gci", junk);
    CHECK_MSG(PortGci::ImportFile(root / "junk.gci", card, root / "scratch").copied == 0,
              "junk refused");
  }

  // Export to a folder, then a raw card, and import the raw card back.
  {
    const fs::path dolphin = root / "dolphin" / "Card A";
    auto report = PortGci::ExportFolder(card, dolphin);
    CHECK_MSG(report.copied == 1 && Read(dolphin / "01-GM8E-MetroidPrime A.gci") == incoming,
              "export to folder");
    CHECK_MSG(PortGci::ExportFolder(card, card).copied == 0, "export into itself refused");

    const fs::path raw = root / "MemoryCardA.USA.raw";
    report = PortGci::ExportRaw(card, raw);
    CHECK_MSG(report.copied == 1 && report.skipped == 0, "export to a new raw card");
    report = PortGci::ExportRaw(card, raw);
    CHECK_MSG(report.copied == 1 && fs::exists(root / "MemoryCardA.USA.raw.bak"),
              "export over a raw card keeps a backup");

    const fs::path fresh = root / "fresh";
    report = PortGci::ImportFile(raw, fresh, root / "scratch");
    CHECK_MSG(report.copied == 1 && report.skipped == 0, "import from raw card");
    const auto back = Read(fresh / "01-GM8E-MetroidPrime A.gci");
    // Placement fields (first block) can differ; the name and data must not.
    CHECK_MSG(back.size() == incoming.size() &&
                  std::memcmp(back.data() + 8, incoming.data() + 8, 32) == 0 &&
                  std::memcmp(back.data() + 64, incoming.data() + 64, incoming.size() - 64) == 0,
              "raw round trip keeps the save");
  }

  // The game alternates between the A and B files and loads the newer, so an
  // import has to replace the whole set: importing A moves an existing B aside,
  // and exporting to a raw card drops a stale B from it.
  {
    const fs::path ab = root / "ab";
    fs::create_directories(ab);
    Write(ab / "01-GM8E-MetroidPrime B.gci", MakeGci("GM8E", "MetroidPrime B", 2, 0x55));
    const fs::path onlyA = root / "onlyA.gci";
    Write(onlyA, MakeGci("GM8E", "MetroidPrime A", 2, 0x66));
    const auto report = PortGci::ImportFile(onlyA, ab, root / "scratch");
    CHECK_MSG(report.copied == 1 && report.replaced == 1, "import A replaces B");
    CHECK_MSG(PortGci::GameFiles(ab).size() == 1 && fs::exists(ab / "01-GM8E-MetroidPrime A.gci"),
              "only A left");

    const fs::path raw = root / "ab.raw";
    const fs::path withB = root / "withB";
    fs::create_directories(withB);
    Write(withB / "01-GM8E-MetroidPrime B.gci", MakeGci("GM8E", "MetroidPrime B", 2, 0x77));
    CHECK_MSG(PortGci::ExportRaw(withB, raw).copied == 1, "raw card with B");
    CHECK_MSG(PortGci::ExportRaw(ab, raw).copied == 1, "raw card with A");
    const fs::path check = root / "abcheck";
    CHECK_MSG(PortGci::ImportFile(raw, check, root / "scratch").copied == 1 &&
                  PortGci::GameFiles(check).size() == 1 &&
                  fs::exists(check / "01-GM8E-MetroidPrime A.gci"),
              "stale B dropped from the raw card");
  }

  // A Trilogy disc's card holds GameCube PAL saves under its own code: a PAL
  // save (or a Dolphin folder of them) goes in as R3ME's, beside the GameCube
  // disc's files, and goes out again as GM8P's.
  {
    PortGci::SetGameCode("R3ME");
    const fs::path tri = root / "tri";
    fs::create_directories(tri);
    const auto gc = MakeGci("GM8E", "MetroidPrime A", 2, 0x12);
    Write(tri / "01-GM8E-MetroidPrime A.gci", gc);
    const auto pal = MakeGci("GM8P", "MetroidPrime", 3, 0x34);
    Write(root / "pal.gci", pal);
    auto report = PortGci::ImportFile(root / "pal.gci", tri, root / "scratch");
    CHECK_MSG(report.copied == 1 && report.replaced == 0 && report.skipped == 0, "PAL save into a Trilogy card");
    const auto in = Read(tri / "01-R3ME-MetroidPrime.gci");
    CHECK_MSG(in.size() == pal.size() && std::memcmp(in.data(), "R3ME", 4) == 0 &&
                  std::memcmp(in.data() + 4, pal.data() + 4, pal.size() - 4) == 0,
              "stored under the Trilogy's code");
    CHECK_MSG(Read(tri / "01-GM8E-MetroidPrime A.gci") == gc, "the GameCube disc's save untouched");
    CHECK_MSG(PortGci::GameFiles(tri).size() == 1, "one Trilogy file");

    const fs::path out = root / "tri-out";
    report = PortGci::ExportFolder(tri, out);
    CHECK_MSG(report.copied == 1 && Read(out / "01-GM8P-MetroidPrime.gci") == pal, "exported as GM8P");

    const fs::path emu = root / "tri-emu";
    fs::create_directories(emu);
    Write(emu / "01-GM8P-MetroidPrime.gci", MakeGci("GM8P", "MetroidPrime", 3, 0x56));
    CHECK_MSG(PortGci::ImportableFiles(emu).size() == 1, "an emulator's GameCube files are importable");
    report = PortGci::ImportFolder(emu, tri);
    CHECK_MSG(report.copied == 1 && report.replaced == 1 && Read(tri / "01-R3ME-MetroidPrime.gci")[0x40] == 0x56,
              "folder import replaces the Trilogy save");
    PortGci::SetGameCode("GM8E");
  }

  // Emulator folder normalisation and the shared card base.
  {
    const fs::path emu = root / "emu";
    const fs::path card = emu / "GC" / "USA" / "Card A";
    fs::create_directories(card);
    CHECK_MSG(PortGci::NormalizeEmulatorFolder(emu) == emu, "user folder stays");
    CHECK_MSG(PortGci::NormalizeEmulatorFolder(emu / "GC") == emu, "GC folder -> user folder");
    CHECK_MSG(PortGci::NormalizeEmulatorFolder(emu / "GC" / "USA") == emu, "region folder -> user folder");
    CHECK_MSG(PortGci::NormalizeEmulatorFolder(card) == emu, "Card A folder -> user folder");
    CHECK_MSG(PortGci::NormalizeEmulatorFolder(fs::path(card.string() + "/")) == emu, "trailing slash");

    PortGci::SetGameCode("GM8E");
    std::string why;
    PortGci::SetEmulatorFolder(PortGci::PathString(card));
    CHECK_MSG(PortGci::EmulatorFolder() == PortGci::PathString(emu), "folder stored normalised");
    PortGci::SetShareEmulatorCard(false);
    CHECK_MSG(PortGci::SharedCardBase(&why).empty() && !why.empty(), "sharing off: no base");
    CHECK_MSG(PortGci::CardTargetDirectory("").empty(), "sharing off: default card");
    PortGci::SetShareEmulatorCard(true);
    CHECK_MSG(PortGci::SharedCardBase() == emu / "GC", "sharing on: <emu>/GC");
    CHECK_MSG(PortGci::CardTargetDirectory("") == PortGci::PathString(emu / "GC"), "shared card is the target");
    CHECK_MSG(PortGci::CardTargetDirectory("/ap") == "/ap", "Archipelago card wins");
    fs::remove_all(card, ec);
    CHECK_MSG(PortGci::SharedCardBase(&why).empty() && why.find("GCI Folder") != std::string::npos,
              "no Card A: reason names the GCI folder");
    PortGci::SetEmulatorFolder("");
    PortGci::SetShareEmulatorCard(false);
  }

  fs::remove_all(root, ec);
  if (port_test::Failures() == 0) {
    std::puts("port_gci tests passed");
  }
  return PORT_TEST_RESULT();
}
