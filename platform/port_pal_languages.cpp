#include "port_pal_languages.h"

#include "port_mods.h"

#include <zlib.h>

namespace PortPalLanguages {
namespace {

constexpr uint32_t kSTRG = 0x53545247;  // 'STRG'

bool IsTokenCharacter(wchar_t c) {
  return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
}

bool IsHexDigit(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'); }

wchar_t UpperAscii(wchar_t c) { return c >= L'a' && c <= L'z' ? c - (L'a' - L'A') : c; }

bool MatchesId(const std::wstring& text, size_t at, const wchar_t* id) {
  for (size_t i = 0; i < 8; ++i) {
    if (UpperAscii(text[at + i]) != id[i]) {
      return false;
    }
  }
  return true;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string TypeName(uint32_t type) {
  const char name[4] = {char(type >> 24), char(type >> 16), char(type >> 8), char(type)};
  return std::string(name, 4);
}

}  // namespace

bool RemapImportedImageTextureIds(std::wstring& text) {
  // The PAL map legend names three textures absent from the USA PAK. These
  // same icons are present there under the corresponding USA ids.
  static const wchar_t* const kPalIds[] = {L"FAB0528D", L"DE6901DE", L"E7B56235"};
  static const wchar_t* const kUsaIds[] = {L"8A78A5BF", L"39C0091E", L"C6FA23D1"};
  bool changed = false;
  for (size_t tag = text.find(L"&image="); tag != std::wstring::npos;) {
    const size_t value = tag + 7;
    const size_t end = text.find(L';', value);
    if (end == std::wstring::npos) {
      break;  // A partial tag is not a reference.
    }
    const size_t nestedTag = text.find(L'&', value);
    if (nestedTag != std::wstring::npos && nestedTag < end) {
      // Don't interpret an unterminated tag's contents as another image tag.
      tag = text.find(L"&image=", end + 1);
      continue;
    }

    for (size_t at = value; at + 8 <= end;) {
      bool isHex = true;
      for (size_t i = 0; i < 8; ++i) {
        isHex = isHex && IsHexDigit(text[at + i]);
      }
      const bool leftBoundary = at == value || !IsTokenCharacter(text[at - 1]);
      const bool rightBoundary = at + 8 == end || !IsTokenCharacter(text[at + 8]);
      if (isHex && leftBoundary && rightBoundary) {
        for (size_t i = 0; i < 3; ++i) {
          if (MatchesId(text, at, kPalIds[i])) {
            text.replace(at, 8, kUsaIds[i]);
            changed = true;
            break;
          }
        }
        at += 8;
      } else {
        ++at;
      }
    }
    tag = text.find(L"&image=", end + 1);
  }
  return changed;
}

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
