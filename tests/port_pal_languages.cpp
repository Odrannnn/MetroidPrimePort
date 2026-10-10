#include "port_pal_languages.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kSTRG = 0x53545247;

// A table with `languages` sections of one string each.
std::vector<uint8_t> MakeStrg(int languages, uint8_t fill) {
  std::vector<uint8_t> out;
  Put32(out, 0x87654321);
  Put32(out, 0);
  Put32(out, uint32_t(languages));
  Put32(out, 1);
  const char* codes[] = {"ENGL", "FREN", "GERM"};
  for (int i = 0; i < languages; ++i) {
    Put32(out, uint32_t(codes[i][0]) << 24 | uint32_t(codes[i][1]) << 16 | uint32_t(codes[i][2]) << 8 | codes[i][3]);
    Put32(out, uint32_t(i * 16));
  }
  for (int i = 0; i < languages; ++i) {
    Put32(out, 12);
    Put32(out, 4);  // the string's offset
    out.insert(out.end(), 8, uint8_t(fill + i));
  }
  return out;
}

struct Entry {
  uint32_t type, id;
  bool compressed;
  std::vector<uint8_t> data;  // as stored
};

// Entries are stored in order, 32-aligned, after the table.
std::vector<uint8_t> MakePak(const std::vector<Entry>& entries) {
  std::vector<uint8_t> pak;
  Put32(pak, 0x00030005);
  Put32(pak, 0);
  Put32(pak, 0);  // no named resources
  Put32(pak, uint32_t(entries.size()));
  uint32_t offset = uint32_t((pak.size() + entries.size() * 20 + 31) & ~size_t(31));
  std::vector<uint32_t> offsets;
  for (const Entry& e : entries) {
    offsets.push_back(offset);
    Put32(pak, e.compressed ? 1 : 0);
    Put32(pak, e.type);
    Put32(pak, e.id);
    const uint32_t size = uint32_t((e.data.size() + 31) & ~size_t(31));
    Put32(pak, size);
    Put32(pak, offset);
    offset += size;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    pak.resize(offsets[i], 0);
    pak.insert(pak.end(), entries[i].data.begin(), entries[i].data.end());
  }
  pak.resize(offset, 0);
  return pak;
}

PortPalLanguages::ReadAt Reader(const std::vector<uint8_t>& file) {
  return [&file](uint64_t offset, uint8_t* out, size_t size) -> size_t {
    if (offset >= file.size()) {
      return 0;
    }
    const size_t got = std::min<size_t>(size, file.size() - offset);
    std::memcpy(out, file.data() + offset, got);
    return got;
  };
}

void TestReadPak() {
  const std::vector<uint8_t> plain = MakeStrg(2, 'a');
  const std::vector<uint8_t> packed = MakeStrg(3, 'x');
  std::vector<uint8_t> stored(4 + compressBound(uLong(packed.size())));
  uLongf length = uLongf(stored.size() - 4);
  Check(compress(stored.data() + 4, &length, packed.data(), uLong(packed.size())) == Z_OK, "compress");
  stored.resize(4 + length);
  stored[0] = 0;
  stored[1] = 0;
  stored[2] = uint8_t(packed.size() >> 8);
  stored[3] = uint8_t(packed.size());

  const std::vector<uint8_t> pak = MakePak({
      {kTXTR, 0x10, false, std::vector<uint8_t>(40, 7)},
      {kSTRG, 0x20, false, plain},
      {kSTRG, 0x30, true, stored},
      {kSTRG, 0x20, false, plain},  // listed twice, as retail PAKs do
  });
  std::map<uint32_t, std::vector<uint8_t>> tables;
  std::string error;
  Check(PortPalLanguages::ReadPakStrgs(Reader(pak), tables, error), "read pak");
  Check(tables.size() == 2, "two tables, no texture");
  // A stored entry is padded to 32 bytes; the padding stays, as on disc.
  Check(tables[0x20].size() >= plain.size() && std::equal(plain.begin(), plain.end(), tables[0x20].begin()),
        "plain table");
  Check(tables[0x30] == packed, "compressed table inflated");
  Check(PortPalLanguages::IsStringTable(tables[0x20]) && PortPalLanguages::IsStringTable(tables[0x30]), "valid tables");

  std::vector<uint8_t> broken = stored;
  broken[3] = uint8_t(broken[3] + 1);  // claims one byte more than it inflates to
  const std::vector<uint8_t> badPak = MakePak({{kSTRG, 0x30, true, broken}});
  tables.clear();
  Check(!PortPalLanguages::ReadPakStrgs(Reader(badPak), tables, error) && !error.empty(), "bad stream refused");

  const std::vector<uint8_t> notPak(64, 0xFF);
  Check(!PortPalLanguages::ReadPakStrgs(Reader(notPak), tables, error), "not a pak");
}

void TestIsStringTable() {
  std::vector<uint8_t> table = MakeStrg(2, 'a');
  Check(PortPalLanguages::IsStringTable(table), "valid");
  std::vector<uint8_t> cut(table.begin(), table.end() - 1);
  Check(!PortPalLanguages::IsStringTable(cut), "cut short");
  std::vector<uint8_t> magic = table;
  magic[0] = 0;
  Check(!PortPalLanguages::IsStringTable(magic), "wrong magic");
  std::vector<uint8_t> version = table;
  version[7] = 1;
  Check(!PortPalLanguages::IsStringTable(version), "wrong version");
  std::vector<uint8_t> languages = table;
  languages[11] = 9;
  Check(!PortPalLanguages::IsStringTable(languages), "too many languages");
  Check(!PortPalLanguages::IsStringTable({}), "empty");
}

// A FONT of `version` naming TXTR 0xCAFEF00D.
std::vector<uint8_t> MakeFont(uint32_t version, const char* name) {
  std::vector<uint8_t> out;
  Put32(out, 0x464F4E54);
  Put32(out, version);
  Put32(out, 16);
  Put32(out, 16);
  if (version >= 1) {
    Put32(out, 14);
  }
  if (version >= 2) {
    Put32(out, 0);
  }
  out.push_back(1);
  out.push_back(0);
  Put32(out, 0);
  Put32(out, 12);
  out.insert(out.end(), name, name + std::strlen(name) + 1);
  Put32(out, 0xCAFEF00D);
  return out;
}

void TestFontTexture() {
  for (uint32_t version : {0u, 1u, 2u, 4u}) {
    uint32_t texture = 0;
    Check(PortPalLanguages::FontTexture(MakeFont(version, "Deface"), texture) && texture == 0xCAFEF00D,
          "font texture by version");
  }
  uint32_t texture = 0;
  Check(!PortPalLanguages::FontTexture(MakeFont(5, "Deface"), texture), "unknown version");
  std::vector<uint8_t> cut = MakeFont(2, "Deface");
  cut.resize(cut.size() - 2);
  Check(!PortPalLanguages::FontTexture(cut, texture), "font cut short");
  Check(!PortPalLanguages::FontTexture(MakeStrg(1, 1), texture), "not a font");
}

void TestImportedImageTextureIds() {
  std::wstring legend =
      L"&image=SI,0.6875,0.6875,323A83B3; &image=SI,0.6875,0.6875,A9ABC1A5; "
      L"&image=SI,0.6875,0.6875,FAB0528D; &image=SI,0.6875,0.6875,2CE9B190; "
      L"&image=SI,0.6875,0.6875,DE6901DE; &image=SI,0.6875,0.6875,7FF222B8; "
      L"&image=SI,0.6875,0.6875,E7B56235;";
  Check(PortPalLanguages::RemapImportedImageTextureIds(legend), "map legend ids remapped");
  Check(legend == L"&image=SI,0.6875,0.6875,8A78A5BF; &image=SI,0.6875,0.6875,8A78A5BF; "
                  L"&image=SI,0.6875,0.6875,8A78A5BF; &image=SI,0.6875,0.6875,39C0091E; "
                  L"&image=SI,0.6875,0.6875,39C0091E; &image=SI,0.6875,0.6875,C6FA23D1; "
                  L"&image=SI,0.6875,0.6875,C6FA23D1;",
        "all seven PAL variants map to the matching USA icon slots");

  std::wstring lower = L"&image=SI,0.6875,0.6875,fab0528d;";
  Check(PortPalLanguages::RemapImportedImageTextureIds(lower) &&
            lower == L"&image=SI,0.6875,0.6875,8A78A5BF;",
        "lowercase asset id");

  std::wstring otherIds =
      L"&image=SI,0.6875,0.6875,FE2235F5; &image=SI,0.6875,0.6875,41031B9B; "
      L"&image=SI,0.6875,0.6875,40074106; &image=SI,0.6875,0.6875,A9D17D44; "
      L"&image=SI,0.6875,0.6875,95401DA9; &image=SI,0.6875,0.6875,81DFB050; "
      L"&image=SI,0.6875,0.6875,CB91BD62;";
  const std::wstring unchangedOtherIds = otherIds;
  Check(!PortPalLanguages::RemapImportedImageTextureIds(otherIds) && otherIds == unchangedOtherIds,
        "other seven map legend ids unchanged");

  std::wstring notImage =
      L"FAB0528D &font=FAB0528D; text DE6901DE; &image=SI,0.68,0.68,E7B56235";
  const std::wstring unchangedNotImage = notImage;
  Check(!PortPalLanguages::RemapImportedImageTextureIds(notImage) && notImage == unchangedNotImage,
        "prose, font tag, and unterminated image tag unchanged");

  std::wstring partialIds =
      L"&image=SI,0,0,0FAB0528D; &image=SI,0,0,FAB0528DA; &image=SI,0,0,xFAB0528D; "
      L"&image=SI,0,0,FAB0528D_;";
  const std::wstring unchangedPartialIds = partialIds;
  Check(!PortPalLanguages::RemapImportedImageTextureIds(partialIds) && partialIds == unchangedPartialIds,
        "partial or embedded ids unchanged");
}

}  // namespace

int main() {
  TestReadPak();
  TestIsStringTable();
  TestFontTexture();
  TestImportedImageTextureIds();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_pal_languages: ok\n");
  return 0;
}
