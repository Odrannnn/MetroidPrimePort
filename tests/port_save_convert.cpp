#include "port_save_convert.h"
#include "port_map_marks.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Argument 1 (optional): the fixtures directory (build/wii-save: saves/ and
// scripts/wii2gci.py). Without it the fixture tests are skipped and pass.

namespace fs = std::filesystem;
using namespace PortSaveConvert;

namespace {

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

constexpr size_t kSaveOffset = 64 + 5188; // the 3004-byte save region of a USA GCI
constexpr size_t kSaveSize = 3004;

size_t FirstDiff(const uint8_t* a, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i])
      return i;
  }
  return n;
}

// ---- no fixtures ------------------------------------------------------------------

void TestPrimitives() {
  static const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  static const uint8_t cipher[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                     0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  static const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  uint8_t iv[16] = {};
  uint8_t out[16];
  AesCbcDecrypt(key, iv, cipher, 16, out);
  CHECK_MSG(std::memcmp(out, plain, 16) == 0, "AES-128 decrypts the FIPS-197 vector");
  // CBC chains: two copies of the block decrypt to plain and plain ^ cipher.
  uint8_t two[32], outTwo[32];
  std::memcpy(two, cipher, 16);
  std::memcpy(two + 16, cipher, 16);
  AesCbcDecrypt(key, iv, two, 32, outTwo);
  bool chained = std::memcmp(outTwo, plain, 16) == 0;
  for (int i = 0; i < 16; ++i)
    chained = chained && outTwo[16 + i] == uint8_t(plain[i] ^ cipher[i]);
  CHECK_MSG(chained, "AES-CBC chains blocks");
  CHECK_MSG(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "CRC32 check value");
}

Save SyntheticSave(Layout layout) {
  Save save;
  save.layout = layout;
  save.system.present = true;
  const bool palContainer = layout != Layout::Usa;
  save.system.lead = palContainer ? Lead::Pal : Lead::Usa;
  save.system.leadBytes.assign(palContainer ? 82 : 162, 0);
  for (size_t i = 0; i < save.system.leadBytes.size(); ++i)
    save.system.leadBytes[i] = uint8_t(i * 7 + 1);
  save.system.fps = 2;
  save.system.ball = 1;
  save.system.powerBombs = 1;
  save.system.logScan = 77;
  save.system.flags = {true, false, true, false, true};
  save.system.autoMapper = 2;
  save.system.palField = palContainer ? 5 : 0;
  Slot& s = save.slots[1];
  s.present = true;
  s.timestamp = 0x12345678;
  s.hard = true;
  s.init = true;
  s.mlvl = 0x83f6ff6f;
  s.playHi = 0x40c00000;
  s.playLo = 0x12345;
  s.enabled = 0xF;
  s.health = 0x42c80000;
  s.beam = 3;
  s.suit = 2;
  for (int i = 0; i < 41; ++i) {
    s.amount[i] = i == 4 ? 123 : (i == 7 ? 5 : 0);
    s.capacity[i] = i == 4 ? 200 : (i == 7 ? 8 : 0);
  }
  s.rates = {300, 17};
  for (size_t i = 0; i < s.opts.size(); ++i)
    s.opts[i] = uint8_t(255 - i);
  s.sound = 2;
  s.bright = 9;
  s.xoff = 33;
  s.yoff = 21;
  s.stretch = 12;
  s.sfx = 100;
  s.music = 50;
  s.hud = 200;
  s.helmet = 150;
  s.flags = {false, true, true, false, true, palContainer};
  for (uint32_t i = 0; i < 32; ++i) {
    s.hintState[i] = i & 3;
    s.hintTime[i] = i * 1000;
  }
  s.hasAp = true;
  s.ap[0] = 0x41505834;
  for (size_t i = 1; i < kApWords; ++i)
    s.ap[i] = i < 11 ? 0x01010101u * uint32_t(i) : uint32_t(i * 99);
  s.hasSeen = true;
  s.seen = {0x80000001u, 0, 0x12345678u, 0x7Fu};
  s.marks = {PortMapMarks::Pack(1, 5), PortMapMarks::Pack(4, 60)};
  return save;
}

// Fills in the worlds a reader makes (present, default area) so a synthetic
// save compares equal to its own re-read.
bool RoundTrip(Save& save, bool pal, const char* what) {
  std::vector<uint8_t> gci, gci2;
  Stats stats;
  std::string error;
  std::vector<uint8_t> art(5120, 0xA5);
  if (!WriteGci(save, pal, art, 1000000000, gci, stats, error)) {
    CHECK_MSG(false, std::string(what) + ": write: " + error);
    return false;
  }
  Format format;
  Save back, back2;
  if (!Read(gci.data(), gci.size(), format, back, nullptr, error)) {
    CHECK_MSG(false, std::string(what) + ": read: " + error);
    return false;
  }
  CHECK_MSG(format == NativeFormat(pal, save.layout == Layout::TriGc), std::string(what) + ": format detected");
  if (!WriteGci(back, pal, art, 1000000000, gci2, stats, error)) {
    CHECK_MSG(false, std::string(what) + ": rewrite: " + error);
    return false;
  }
  CHECK_MSG(gci == gci2, std::string(what) + ": write(read(write)) is byte-identical");
  CHECK_MSG(Read(gci2.data(), gci2.size(), format, back2, nullptr, error) && back == back2,
            std::string(what) + ": re-read is equal");
  const Slot& a = save.slots[1];
  const Slot& b = back.slots[1];
  CHECK_MSG(b.present && !back.slots[0].present && !back.slots[2].present, std::string(what) + ": slot presence");
  CHECK_MSG(a.timestamp == b.timestamp && a.hard == b.hard && a.init == b.init && a.mlvl == b.mlvl &&
                a.playHi == b.playHi && a.playLo == b.playLo && a.enabled == b.enabled && a.health == b.health &&
                a.beam == b.beam && a.suit == b.suit,
            std::string(what) + ": slot header fields");
  CHECK_MSG(a.amount == b.amount && a.capacity == b.capacity && a.rates == b.rates && a.opts == b.opts,
            std::string(what) + ": items, rates, options");
  CHECK_MSG(a.sound == b.sound && a.bright == b.bright && a.xoff == b.xoff && a.yoff == b.yoff &&
                a.stretch == b.stretch && a.sfx == b.sfx && a.music == b.music && a.hud == b.hud &&
                a.helmet == b.helmet && a.flags == b.flags,
            std::string(what) + ": option values");
  CHECK_MSG(a.hintState == b.hintState && a.hintTime == b.hintTime, std::string(what) + ": hints");
  CHECK_MSG(b.hasAp && a.ap == b.ap, std::string(what) + ": Archipelago trailer");
  CHECK_MSG(b.hasSeen && a.seen == b.seen, std::string(what) + ": seen-pickups trailer");
  CHECK_MSG(a.marks == b.marks, std::string(what) + ": room-marks trailer");
  CHECK_MSG(back.system.fps == 2 && back.system.ball == 1 && back.system.powerBombs == 1 &&
                back.system.logScan == 77 && back.system.flags == save.system.flags &&
                back.system.autoMapper == 2 && back.system.palField == save.system.palField,
            std::string(what) + ": system fields");
  CHECK_MSG(back.system.leadBytes == save.system.leadBytes, std::string(what) + ": lead bytes");
  CHECK_MSG(ArtOf(gci.data(), gci.size()) == art, std::string(what) + ": art");
  save = back;
  return true;
}

void TestSynthetic() {
  Save usa = SyntheticSave(Layout::Usa);
  RoundTrip(usa, false, "synthetic USA");
  Save pal = SyntheticSave(Layout::Pal);
  RoundTrip(pal, true, "synthetic PAL");
  Save tri = SyntheticSave(Layout::TriGc);
  RoundTrip(tri, true, "synthetic Trilogy card");

  // A Trilogy disc's card file orders its relays by the Trilogy's tables, not
  // PAL's: its Chozo list has one more relay (0x2C01FB) part way through, so
  // with PAL's order every later relay would read one slot off.
  {
    constexpr uint32_t kChozo = 0x83f6ff6f;
    const std::set<uint32_t> relays = {0x00310063, 0x003402DF, 0x003D0004};
    Save src = SyntheticSave(Layout::Usa);
    World& w = src.slots[1].worlds[kChozo];
    w.present = true;
    w.relays = relays;
    std::vector<uint8_t> gci;
    Stats stats;
    std::string error;
    CHECK_MSG(WriteGci(src, false, {}, 1000000000, gci, stats, error), "USA with Chozo relays: " + error);
    Conversion conv;
    CHECK_MSG(ConvertTo(gci.data(), gci.size(), Format::TriGci, {}, conv, error) && !conv.stats.Any(),
              "USA -> Trilogy card: " + error);
    Format format;
    Save back;
    CHECK_MSG(Read(conv.gci.data(), conv.gci.size(), format, back, nullptr, error) && format == Format::TriGci &&
                  back.layout == Layout::TriGc,
              "Trilogy card reads back as one: " + error);
    CHECK_MSG(back.slots[1].worlds[kChozo].relays == relays, "Trilogy card keeps the Chozo relays");
    // The same stream read with PAL's tables would name other relays.
    std::memcpy(conv.gci.data(), "GM8P", 4);
    CHECK_MSG(Read(conv.gci.data(), conv.gci.size(), format, back, nullptr, error) && format == Format::PalGci &&
                  back.slots[1].worlds[kChozo].relays != relays,
              "Trilogy card's relay order is not PAL's");
  }

  // A PAL save is told from the port's earlier PAL saves by its name, not its size.
  std::vector<uint8_t> head(0x40 + 0x2000, 0);
  std::memcpy(head.data(), "GM8P01", 6);
  std::memcpy(head.data() + 8, "MetroidPrime A", 14);
  CHECK_MSG(Detect(head.data(), head.size()) == Format::PalLegacyGci, "legacy PAL save detected by name");
  std::memcpy(head.data() + 8, "MetroidPrime\0\0", 14);
  CHECK_MSG(Detect(head.data(), head.size()) == Format::Unknown, "cut-short PAL save is not legacy");

  // USA -> PAL -> USA keeps everything the formats share.
  Stats stats;
  Save viaPal = usa;
  Retarget(viaPal, Layout::Pal, stats);
  CHECK_MSG(viaPal.layout == Layout::Pal, "retarget changes the layout");
  Retarget(viaPal, Layout::Usa, stats);
  CHECK_MSG(viaPal.slots[1].amount == usa.slots[1].amount && viaPal.slots[1].hintTime == usa.slots[1].hintTime &&
                viaPal.slots[1].worlds.size() == usa.slots[1].worlds.size(),
            "USA -> PAL -> USA keeps the slot");

  // Garbage never crashes: every prefix of a valid file, and noisy copies.
  std::vector<uint8_t> gci;
  std::string error;
  Save saved = SyntheticSave(Layout::Usa);
  WriteGci(saved, false, {}, 1000000000, gci, stats, error);
  unsigned seed = 1;
  for (size_t n = 0; n <= gci.size(); n += 97) {
    Format format;
    Save out;
    Read(gci.data(), n, format, out, nullptr, error);
  }
  for (int round = 0; round < 40; ++round) {
    std::vector<uint8_t> noisy = gci;
    for (int k = 0; k < 200; ++k) {
      seed = seed * 1664525u + 1013904223u;
      noisy[64 + (seed >> 8) % (noisy.size() - 64)] = uint8_t(seed >> 3);
    }
    Format format;
    Save out;
    Conversion conversion;
    Read(noisy.data(), noisy.size(), format, out, nullptr, error);
    ConvertToDisc(noisy.data(), noisy.size(), round & 1, {}, conversion, error);
  }
  // ConvertTo writes the GC formats it is asked for, and only those.
  for (const Format target : {Format::UsaGci, Format::PalGci}) {
    Conversion conversion;
    Format got;
    Save back;
    CHECK_MSG(ConvertTo(gci.data(), gci.size(), target, {}, conversion, error) &&
                  Read(conversion.gci.data(), conversion.gci.size(), got, back, nullptr, error) && got == target &&
                  back.slots[1].amount == saved.slots[1].amount,
              std::string("USA -> ") + FormatName(target) + ": " + error);
  }
  for (const Format target : {Format::PalLegacyGci, Format::WiiSave, Format::WiiDataBin, Format::Unknown}) {
    Conversion conversion;
    CHECK_MSG(!ConvertTo(gci.data(), gci.size(), target, {}, conversion, error),
              std::string("ConvertTo refuses ") + FormatName(target));
  }
  std::vector<uint8_t> junk(0x1C000, 0xFF);
  Format format;
  Save out;
  CHECK_MSG(!Read(junk.data(), junk.size(), format, out, nullptr, error), "junk of the Trilogy's size is refused");
  std::vector<uint8_t> bk(0xF0C0 + 0x100, 0);
  bk[0xF0C4] = 'B';
  bk[0xF0C5] = 'k';
  std::vector<uint8_t> bin;
  CHECK_MSG(!DecryptDataBin(bk.data(), bk.size(), bin, error), "an empty data.bin is refused");
}

// ---- fixtures -----------------------------------------------------------------------

int CountIds(const Save& save) {
  int n = 0;
  for (const Slot& s : save.slots) {
    n += int(s.scans.size());
    for (const auto& [mlvl, w] : s.worlds)
      n += int(w.relays.size() + w.doors.size() + w.visited.size() + w.mapped.size());
  }
  return n + int(save.system.cine.size());
}

bool Subset(const Save& small, const Save& big) {
  for (int i = 0; i < 3; ++i) {
    const Slot& a = small.slots[i];
    const Slot& b = big.slots[i];
    if (!std::includes(b.scans.begin(), b.scans.end(), a.scans.begin(), a.scans.end()))
      return false;
    for (const auto& [mlvl, w] : a.worlds) {
      const auto it = b.worlds.find(mlvl);
      if (it == b.worlds.end())
        return false;
      const World& o = it->second;
      if (!std::includes(o.relays.begin(), o.relays.end(), w.relays.begin(), w.relays.end()) ||
          !std::includes(o.doors.begin(), o.doors.end(), w.doors.begin(), w.doors.end()))
        return false;
    }
  }
  return std::includes(big.system.cine.begin(), big.system.cine.end(), small.system.cine.begin(),
                       small.system.cine.end());
}

void TestPalFixture(const fs::path& file) {
  std::vector<uint8_t> bytes;
  if (!ReadFile(file, bytes)) {
    CHECK_MSG(false, "read " + file.string());
    return;
  }
  Format format;
  Save save;
  std::string error;
  CHECK_MSG(Read(bytes.data(), bytes.size(), format, save, nullptr, error), "PAL fixture reads: " + error);
  CHECK_MSG(format == Format::PalGci, "PAL fixture is detected as PAL");
  int slots = 0;
  for (const Slot& s : save.slots)
    slots += s.present ? 1 : 0;
  std::fprintf(stderr, "PAL fixture: %d slots\n", slots);
  CHECK_MSG(slots > 0, "PAL fixture has slots");

  Stats stats;
  std::vector<uint8_t> gci;
  CHECK_MSG(WriteGci(save, true, ArtOf(bytes.data(), bytes.size()), 1000000000, gci, stats, error),
            "PAL writes: " + error);
  Format format2;
  Save again;
  CHECK_MSG(Read(gci.data(), gci.size(), format2, again, nullptr, error) && again == save,
            "PAL -> intermediate -> PAL is identical in every field, and re-parses");
  CHECK_MSG(gci.size() == bytes.size(), "PAL file size is kept");

  Conversion usa, back;
  CHECK_MSG(ConvertToDisc(bytes.data(), bytes.size(), false, {}, usa, error), "PAL -> USA: " + error);
  std::fprintf(stderr, "PAL -> USA: %s\n", usa.stats.Summary().c_str());
  CHECK_MSG(ConvertToDisc(usa.gci.data(), usa.gci.size(), true, {}, back, error), "USA -> PAL: " + error);
  std::fprintf(stderr, "USA -> PAL: %s\n", back.stats.Summary().c_str());
  Save viaUsa, middle;
  Format f3;
  CHECK_MSG(Read(back.gci.data(), back.gci.size(), f3, viaUsa, nullptr, error), "PAL re-read: " + error);
  CHECK_MSG(Read(usa.gci.data(), usa.gci.size(), f3, middle, nullptr, error), "USA re-read: " + error);
  CHECK_MSG(Subset(viaUsa, save), "PAL -> USA -> PAL only keeps ids it had");
  CHECK_MSG(CountIds(save) - CountIds(viaUsa) ==
                usa.stats.relays + usa.stats.doors + usa.stats.cinematics + usa.stats.scans + usa.stats.areas +
                    back.stats.relays + back.stats.doors + back.stats.cinematics + back.stats.scans +
                    back.stats.areas,
            "every id lost in PAL -> USA -> PAL is counted as dropped");
  for (int i = 0; i < 3; ++i) {
    if (!save.slots[i].present)
      continue;
    const Slot &a = save.slots[i], &b = viaUsa.slots[i];
    CHECK_MSG(a.timestamp == b.timestamp && a.mlvl == b.mlvl && a.amount == b.amount && a.capacity == b.capacity &&
                  a.hintState == b.hintState && a.hintTime == b.hintTime && a.opts == b.opts && a.flags == b.flags,
              "PAL -> USA -> PAL keeps the slot's scalars");
  }
}

void TestUsaFixture(const fs::path& file) {
  std::vector<uint8_t> bytes;
  if (!ReadFile(file, bytes)) {
    CHECK_MSG(false, "read " + file.string());
    return;
  }
  Format format;
  Save save;
  std::string error;
  CHECK_MSG(Read(bytes.data(), bytes.size(), format, save, nullptr, error), "USA fixture reads: " + error);
  CHECK_MSG(format == Format::UsaGci, "USA fixture is detected as USA");
  std::vector<uint8_t> gci;
  Stats stats;
  CHECK_MSG(WriteGci(save, false, ArtOf(bytes.data(), bytes.size()), 1000000000, gci, stats, error),
            "USA writes: " + error);
  const size_t diff = bytes.size() >= kSaveOffset + kSaveSize && gci.size() == bytes.size()
                          ? FirstDiff(bytes.data() + kSaveOffset, gci.data() + kSaveOffset, kSaveSize)
                          : 0;
  if (diff != kSaveSize)
    std::fprintf(stderr, "USA save region first differs at byte %zu\n", diff);
  CHECK_MSG(diff == kSaveSize, "USA -> intermediate -> USA is byte-identical (save region)");
  Conversion same;
  CHECK_MSG(ConvertToDisc(bytes.data(), bytes.size(), false, {}, same, error), "USA -> USA: " + error);
  CHECK_MSG(!same.stats.Any(), "USA -> USA drops nothing");

  Conversion pal, back;
  CHECK_MSG(ConvertToDisc(bytes.data(), bytes.size(), true, {}, pal, error), "USA -> PAL: " + error);
  std::fprintf(stderr, "USA -> PAL: %s\n", pal.stats.Summary().c_str());
  CHECK_MSG(ConvertToDisc(pal.gci.data(), pal.gci.size(), false, {}, back, error), "PAL -> USA: " + error);
  std::fprintf(stderr, "PAL -> USA: %s\n", back.stats.Summary().c_str());
  Save viaPal;
  CHECK_MSG(Read(back.gci.data(), back.gci.size(), format, viaPal, nullptr, error), "USA re-read: " + error);
  CHECK_MSG(Subset(viaPal, save), "USA -> PAL -> USA only keeps ids it had");
  for (int i = 0; i < 3; ++i) {
    if (save.slots[i].present) {
      CHECK_MSG(viaPal.slots[i].amount == save.slots[i].amount && viaPal.slots[i].hintTime == save.slots[i].hintTime,
                "USA -> PAL -> USA keeps the slot's scalars");
    }
  }
}

void TestWiiFixtures(const fs::path& root) {
  const fs::path script = root / "scripts" / "wii2gci.py";
  if (!fs::exists(script)) {
    std::fprintf(stderr, "no wii2gci.py: Trilogy comparison skipped\n");
    return;
  }
  const fs::path tmp = fs::temp_directory_path() / "port_save_convert_ref";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  int compared = 0;
  for (const auto& entry : fs::directory_iterator(root / "saves", ec)) {
    const fs::path save = entry.path() / "save.bin";
    if (!fs::exists(save))
      continue;
    const std::string name = entry.path().filename().string();
    const fs::path ref = tmp / (name + ".gci");
    const std::string command = "python3 -I '" + script.string() + "' '" + save.string() + "' '" + ref.string() +
                                "' > /dev/null 2>&1";
    std::vector<uint8_t> bytes, expected;
    if (std::system(command.c_str()) != 0 || !ReadFile(ref, expected) || !ReadFile(save, bytes)) {
      std::fprintf(stderr, "%s: no reference output, skipped\n", name.c_str());
      continue;
    }
    Conversion conversion;
    std::string error;
    const bool converted = ConvertToDisc(bytes.data(), bytes.size(), false, {}, conversion, error);
    if (!converted && error == "the save has no used slots") {
      std::fprintf(stderr, "%s: empty Trilogy save, rejected as intended\n", name.c_str());
      continue;
    }
    CHECK_MSG(converted, name + ": convert: " + error);
    CHECK_MSG(conversion.from == Format::WiiSave, name + ": detected as a Trilogy save");
    if (conversion.gci.size() != expected.size() || expected.size() < kSaveOffset + kSaveSize) {
      CHECK_MSG(false, name + ": output size");
      continue;
    }
    const size_t diff = FirstDiff(expected.data() + kSaveOffset, conversion.gci.data() + kSaveOffset, kSaveSize);
    if (diff != kSaveSize) {
      std::fprintf(stderr, "%s: first differs from wii2gci.py at byte %zu of the save region\n", name.c_str(), diff);
      for (int w = 0; w < 2; ++w) {
        std::fprintf(stderr, w ? "  got " : "  ref ");
        const uint8_t* q = (w ? conversion.gci.data() : expected.data()) + kSaveOffset + diff;
        for (int k = -4; k < 12; ++k)
          std::fprintf(stderr, "%02x ", q[k]);
        std::fprintf(stderr, "\n");
      }
    }
    CHECK_MSG(diff == kSaveSize, name + ": matches wii2gci.py (3004-byte save region)");
    std::fprintf(stderr, "%s -> USA: %s\n", name.c_str(), conversion.stats.Summary().c_str());
    ++compared;
    // The same save for a PAL disc must at least be a valid PAL file.
    Conversion pal;
    CHECK_MSG(ConvertToDisc(bytes.data(), bytes.size(), true, {}, pal, error), name + ": to PAL: " + error);
    Format format;
    Save back;
    CHECK_MSG(Read(pal.gci.data(), pal.gci.size(), format, back, nullptr, error) && format == Format::PalGci,
              name + ": the PAL output re-reads");
  }
  fs::remove_all(tmp, ec);
  std::fprintf(stderr, "compared %d Trilogy saves with wii2gci.py\n", compared);
}

// The Trilogy writer: every fixture written back into itself must give the
// same bytes (slots, summaries, header), and a fixture taken through a USA GCI
// and written into an empty Trilogy save (01-firstboot) must read back as the
// same slots.
void TestTrilogyWrite(const fs::path& root) {
  std::vector<uint8_t> empty;
  if (!ReadFile(root / "saves" / "01-firstboot" / "save.bin", empty)) {
    std::fprintf(stderr, "no 01-firstboot/save.bin: Trilogy write tests skipped\n");
    return;
  }
  std::error_code ec;
  int written = 0;
  for (const auto& entry : fs::directory_iterator(root / "saves", ec)) {
    const std::string name = entry.path().filename().string();
    std::vector<uint8_t> bin;
    if (!ReadFile(entry.path() / "save.bin", bin))
      continue;
    Format format;
    Save save;
    std::string error;
    if (!Read(bin.data(), bin.size(), format, save, nullptr, error) || format != Format::WiiSave)
      continue;
    bool any = false;
    for (const Slot& s : save.slots)
      any = any || s.present;
    if (!any)
      continue;
    std::vector<uint8_t> same;
    Stats stats;
    CHECK_MSG(WriteWii(save, bin.data(), bin.size(), same, stats, error), name + ": write into itself: " + error);
    const size_t diff = FirstDiff(bin.data(), same.data(), std::min(bin.size(), same.size()));
    if (diff != bin.size())
      std::fprintf(stderr, "%s: rewritten save.bin first differs at 0x%zx (%02x, got %02x)\n", name.c_str(), diff,
                   bin[diff], same[diff]);
    CHECK_MSG(same == bin, name + ": written into itself gives the same bytes");

    Conversion usa, back;
    CHECK_MSG(ConvertToDisc(bin.data(), bin.size(), false, {}, usa, error), name + ": to USA: " + error);
    CHECK_MSG(!ConvertToTrilogy(usa.gci.data(), usa.gci.size(), empty.data(), empty.size(), back, error),
              name + ": a Trilogy save without an MP1 file is refused");
    CHECK_MSG(ConvertToTrilogy(usa.gci.data(), usa.gci.size(), bin.data(), bin.size(), back, error),
              name + ": USA back into the Trilogy save: " + error);
    Save reread;
    CHECK_MSG(Read(back.gci.data(), back.gci.size(), format, reread, nullptr, error) && format == Format::WiiSave,
              name + ": the written save.bin re-reads: " + error);
    // The newest slot lands in the file a Mii owns (its summary carries the
    // Mii's id, all 0xFF otherwise); the others keep theirs.
    const auto owned = [&](uint32_t i) {
      const uint8_t* id = bin.data() + 0x64 + 0x258 * i + 0x3F;
      return std::any_of(id, id + 8, [](uint8_t b) { return b != 0xFF; });
    };
    int newest = -1, target = -1;
    for (int i = 0; i < 3; ++i) {
      if (save.slots[i].present && (newest < 0 || save.slots[i].timestamp > save.slots[newest].timestamp))
        newest = i;
      if (target < 0 && owned(i))
        target = i;
    }
    const uint32_t current = uint32_t(bin[8]) << 24 | bin[9] << 16 | bin[10] << 8 | bin[11];
    if (current < 3 && owned(current))
      target = int(current);
    // What the trip keeps: the Trilogy has relays, doors, scans and
    // cinematics the GameCube lacks.
    Save expect = save;
    Stats lost;
    Retarget(expect, Layout::Usa, lost);
    Retarget(expect, Layout::Wii, lost);
    Slot want = expect.slots[newest];
    want.timestamp = reread.slots[target].timestamp; // a GCI keeps its own
    want.cine = expect.system.cine;
    // Layers the GameCube has no entry for come back at the Trilogy's default.
    Slot got = reread.slots[target];
    for (auto& [mlvl, world] : got.worlds) {
      const auto it = want.worlds.find(mlvl);
      std::erase_if(world.layers, [&](const auto& l) {
        return it == want.worlds.end() || it->second.layers.count(l.first) == 0;
      });
    }
    CHECK_MSG(got == want, name + ": slot " + std::to_string(newest + 1) + " survives Trilogy -> USA -> Trilogy");
    for (int i = 0; i < 3; ++i) {
      if (i != target)
        CHECK_MSG(reread.slots[i] == save.slots[i], name + ": file " + std::to_string(i + 1) + " is kept");
    }
    ++written;
  }
  std::fprintf(stderr, "wrote %d Trilogy saves back\n", written);
}

// Real SD-export data.bin files: our decryption must give the save.bin wiisave.py extracts.
void TestDataBinFixtures(const fs::path& root) {
  const fs::path script = root / "scripts" / "wiisave.py";
  std::error_code ec;
  if (!fs::exists(script) || !fs::is_directory(root / "dl", ec)) {
    std::fprintf(stderr, "no wiisave.py or dl/: data.bin comparison skipped\n");
    return;
  }
  const fs::path tmp = fs::temp_directory_path() / "port_save_convert_databin";
  int compared = 0;
  for (const auto& entry : fs::recursive_directory_iterator(root / "dl", ec)) {
    if (entry.path().filename() != "data.bin")
      continue;
    const std::string name = fs::relative(entry.path(), root, ec).string();
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string command = "python3 -I '" + script.string() + "' '" + entry.path().string() + "' '" +
                                tmp.string() + "' > /dev/null 2>&1";
    std::vector<uint8_t> bytes, expected, got;
    if (std::system(command.c_str()) != 0 || !ReadFile(tmp / "save.bin", expected) ||
        !ReadFile(entry.path(), bytes)) {
      std::fprintf(stderr, "%s: no reference output, skipped\n", name.c_str());
      continue;
    }
    std::string error;
    CHECK_MSG(DecryptDataBin(bytes.data(), bytes.size(), got, error), name + ": decrypt: " + error);
    CHECK_MSG(got == expected, name + ": save.bin matches wiisave.py");
    Conversion conversion;
    const bool converted = ConvertToDisc(bytes.data(), bytes.size(), false, {}, conversion, error);
    CHECK_MSG(converted || error == "the save has no used slots", name + ": convert: " + error);
    if (converted)
      std::fprintf(stderr, "%s -> USA: %s\n", name.c_str(), conversion.stats.Summary().c_str());
    ++compared;
  }
  fs::remove_all(tmp, ec);
  std::fprintf(stderr, "compared %d data.bin files with wiisave.py\n", compared);
}

} // namespace

int main(int argc, char** argv) {
  TestPrimitives();
  TestSynthetic();
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path();
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root / "saves", ec)) {
    std::fprintf(stderr, "no fixtures directory: fixture tests skipped\n");
  } else {
    for (const auto& entry : fs::directory_iterator(root / "saves", ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("31-", 0) == 0) {
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
          if (f.path().extension() == ".gci")
            TestPalFixture(f.path());
        }
      } else if (name.rfind("41-", 0) == 0) {
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
          if (f.path().extension() == ".gci")
            TestUsaFixture(f.path());
        }
      }
    }
    TestWiiFixtures(root);
    TestTrilogyWrite(root);
    TestDataBinFixtures(root);
  }
  if (port_test::Failures() == 0) {
    std::printf("port_save_convert_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
