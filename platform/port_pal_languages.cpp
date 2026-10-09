#include "port_pal_languages.h"

#include "port_mods.h"

#include <zlib.h>

namespace PortPalLanguages {
namespace {

constexpr uint32_t kSTRG = 0x53545247;  // 'STRG'

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string TypeName(uint32_t type) {
  const char name[4] = {char(type >> 24), char(type >> 16), char(type >> 8), char(type)};
  return std::string(name, 4);
}

}  // namespace

bool ReadPakStrgs(const ReadAt& read, std::map<uint32_t, std::vector<uint8_t>>& out, std::string& error) {
  std::map<ResourceKey, std::vector<uint8_t>> found;
  if (!ReadPakResources(read, [](uint32_t type, uint32_t) { return type == kSTRG; }, found, error)) {
    return false;
  }
  for (auto& [key, data] : found) {
    out.try_emplace(key.second, std::move(data));
  }
  return true;
}

bool FontTexture(const std::vector<uint8_t>& font, uint32_t& texture) {
  // FONT, version, mono width/height, baseline (v1+), line margin (v2+), two
  // bools, two ints, a NUL-ended name, then the TXTR id (CRasterFont).
  if (font.size() < 8 || Be32(font.data()) != 0x464F4E54u) {
    return false;
  }
  const uint32_t version = Be32(font.data() + 4);
  if (version > 4) {
    return false;
  }
  size_t at = 8 + 8 + (version >= 1 ? 4 : 0) + (version >= 2 ? 4 : 0) + 2 + 8;
  while (at < font.size() && font[at] != 0) {
    ++at;
  }
  if (at + 5 > font.size()) {
    return false;
  }
  texture = Be32(font.data() + at + 1);
  return true;
}

bool ReadPakResources(const ReadAt& read, const std::function<bool(uint32_t type, uint32_t id)>& want,
                      std::map<ResourceKey, std::vector<uint8_t>>& out, std::string& error) {
  std::vector<uint8_t> header;
  PortMods::PakTable table;
  size_t needed = 0x10000;
  bool parsed = false;
  while (needed <= (64u << 20)) {
    header.resize(needed);
    const size_t got = read(0, header.data(), header.size());
    if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
      parsed = true;
      break;
    }
    if (got < header.size() || needed <= header.size()) {
      break;
    }
  }
  if (!parsed) {
    error = "not a Metroid Prime PAK";
    return false;
  }
  for (const PortMods::PakResource& res : table.resources) {
    const ResourceKey key(res.type, res.id);
    if (out.count(key) != 0 || !want(res.type, res.id)) {
      continue;
    }
    const std::string what = TypeName(res.type) + " " + std::to_string(res.id);
    std::vector<uint8_t> raw(res.size);
    if (read(res.offset, raw.data(), raw.size()) != raw.size()) {
      error = "could not read " + what;
      return false;
    }
    if (res.compressed != 0) {
      // A big-endian length, then a zlib stream.
      if (raw.size() < 6) {
        error = what + " is cut short";
        return false;
      }
      std::vector<uint8_t> data(Be32(raw.data()));
      uLongf length = uLongf(data.size());
      if (uncompress(data.data(), &length, raw.data() + 4, uLong(raw.size() - 4)) != Z_OK || length != data.size()) {
        error = "could not decompress " + what;
        return false;
      }
      raw = std::move(data);
    }
    out[key] = std::move(raw);
  }
  return true;
}

bool IsStringTable(const std::vector<uint8_t>& data) {
  if (data.size() < 16 || Be32(data.data()) != 0x87654321u || Be32(data.data() + 4) != 0) {
    return false;
  }
  const uint64_t languages = Be32(data.data() + 8);
  const uint64_t tableEnd = 16 + languages * 8;
  if (languages == 0 || tableEnd > data.size()) {
    return false;
  }
  for (uint64_t i = 0; i < languages; ++i) {
    const uint64_t at = tableEnd + Be32(data.data() + 16 + i * 8 + 4);
    if (at + 4 > data.size() || at + 4 + Be32(data.data() + at) > data.size()) {
      return false;
    }
  }
  return true;
}

}  // namespace PortPalLanguages
