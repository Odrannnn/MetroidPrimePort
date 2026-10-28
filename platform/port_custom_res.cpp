#include "port_custom_res.h"
#include "port_bytes.h"

#include "port_log.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace PortCustomRes {
namespace {

#include "port_custom_assets_data.inc"

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kTXTR = 0x54585452;

// Disc sources (randomprime's resource_info names).
constexpr uint32_t kMetroidCmdl = 0x2F976E86;   // Metroid.CMDL
constexpr uint32_t kGravitySuitCmdl = 0x95946E41; // Node1_11.CMDL
constexpr uint32_t kGravitySuitAncs = 0x27A97006; // Node1_11.ANCS
constexpr uint32_t kVisorCmdl = 0x61DAB956;       // Node1_39_1.CMDL
constexpr uint32_t kVisorAncs = 0x9F0C908A;       // Node1_39_1.ANCS
constexpr uint32_t kBlueShieldCmdl = 0x0734977A; // blueShield_v1.CMDL
constexpr uint32_t kBlueShieldVerticalCmdl = 0x18D0AEE6;
constexpr uint32_t kMissileShieldCmdl = 0xEFDFFB8C;

using port::GetBE32;
using port::SetBE32;

template <size_t N>
std::vector<uint8_t> Embedded(const unsigned char (&bytes)[N]) {
  return std::vector<uint8_t>(bytes, bytes + N);
}

constexpr uint32_t kSCAN = 0x5343414E;
constexpr uint32_t kSTRG = 0x53545247;

// Scan text: pair n is SCAN kTextBase + 2n and its STRG right after.
struct TextRegistry {
  std::mutex mutex;
  std::map<uint64_t, uint32_t> ids;
  std::vector<std::u16string> texts;
  // Bumped whenever pair n's text changes, so Find rebuilds it.
  std::vector<uint32_t> revisions;
};

TextRegistry& Texts() {
  static TextRegistry registry;
  return registry;
}

// 0 while the pair is unregistered.
uint32_t TextRevision(uint32_t id) {
  const uint32_t index = (id - kTextBase) / 2;
  TextRegistry& registry = Texts();
  std::lock_guard<std::mutex> lock(registry.mutex);
  return index < registry.revisions.size() ? registry.revisions[index] : 0;
}

bool BuildText(uint32_t id, Resource& out) {
  const uint32_t index = (id - kTextBase) / 2;
  std::u16string text;
  {
    TextRegistry& registry = Texts();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (index >= registry.texts.size())
      return false;
    text = registry.texts[index];
  }
  if ((id - kTextBase) % 2 == 0) {
    out.type = kSCAN;
    out.data = MakeScan(id + 1);
  } else {
    out.type = kSTRG;
    out.data = MakeStrg(text);
  }
  return true;
}

// A 32x32 RGBA8 TXTR (format 9, one mip): 4x4 tiles of 32 AR bytes then 32 GB
// bytes, for the map legend's swatches. 32x32 as the disc's own swatches are: a
// text image is as wide and tall as its texture, so the same size puts the
// " - " after it in the disc's column and gives its row the disc's height.
bool BuildLegend(uint32_t id, Resource& out) {
  constexpr int kSize = 32;
  uint8_t px[kSize][kSize][4] = {};
  auto put = [&](int x, int y, uint32_t rgb) {
    px[y][x][0] = uint8_t(rgb >> 16);
    px[y][x][1] = uint8_t(rgb >> 8);
    px[y][x][2] = uint8_t(rgb);
    px[y][x][3] = 255;
  };
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const float dx = x + 0.5f - kSize / 2.f;
      const float dy = y + 0.5f - kSize / 2.f;
      const float r2 = dx * dx + dy * dy;
      switch (id) {
      case kLegendDotTxtr:
        if (r2 <= 6.f * 6.f)
          put(x, y, 0xFFFFFF);
        break;
      case kLegendRingTxtr:
        if (r2 <= 6.5f * 6.5f && r2 >= 4.2f * 4.2f)
          put(x, y, 0xFFFFFF);
        break;
      case kLegendCrossTxtr: {
        const float a = dx - dy < 0 ? dy - dx : dx - dy;
        const float b = dx + dy < 0 ? -(dx + dy) : dx + dy;
        if (dx > -6.f && dx < 6.f && dy > -6.f && dy < 6.f) {
          if (a < 1.3f || b < 1.3f)
            put(x, y, 0x808080);
          else if (a < 2.4f || b < 2.4f)
            put(x, y, 0x202020);
        }
        break;
      }
      default:
        if (dx > -6.f && dx < 6.f && dy > -6.f && dy < 6.f)
          put(x, y, 0x40E0FF);
        break;
      }
    }
  }
  std::vector<uint8_t> txtr(12);
  SetBE32(txtr, 0, 9);
  txtr[4] = 0;
  txtr[5] = kSize;
  txtr[6] = 0;
  txtr[7] = kSize;
  SetBE32(txtr, 8, 1);
  for (int ty = 0; ty < kSize; ty += 4) {
    for (int tx = 0; tx < kSize; tx += 4) {
      for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < 4; ++y) {
          for (int x = 0; x < 4; ++x) {
            const uint8_t* p = px[ty + y][tx + x];
            txtr.push_back(pass == 0 ? p[3] : p[1]);
            txtr.push_back(pass == 0 ? p[0] : p[2]);
          }
        }
      }
    }
  }
  out.type = kTXTR;
  out.data.swap(txtr);
  return true;
}

bool Build(uint32_t id, const DiscReader& read, Resource& out) {
  if (id >= kTextBase)
    return BuildText(id, out);
  if (id >= kLegendDotTxtr && id <= kLegendMarkTxtr)
    return BuildLegend(id, out);
  // Each model's texture patches (material set 0 index -> texture).
  struct TexturePatch {
    uint32_t index, texture;
  };
  auto model = [&](std::vector<uint8_t> cmdl, std::initializer_list<TexturePatch> patches) {
    for (const TexturePatch& patch : patches)
      if (!SetCmdlTexture(cmdl, patch.index, patch.texture))
        return false;
    out.type = kCMDL;
    out.data.swap(cmdl);
    return true;
  };
  auto discModel = [&](uint32_t source, std::initializer_list<TexturePatch> patches) {
    std::vector<uint8_t> cmdl;
    return read(source, cmdl) && model(std::move(cmdl), patches);
  };
  // A copy of a retail pickup's ANCS whose character 0 draws the new model.
  auto anim = [&](uint32_t source, uint32_t sourceModel, uint32_t newModel) {
    std::vector<uint8_t> ancs;
    if (!read(source, ancs) || !SetAncsModel(ancs, sourceModel, newModel))
      return false;
    out.type = kANCS;
    out.data.swap(ancs);
    return true;
  };

  if (id >= kShieldBase && id < kShieldEnd) {
    // The missile shield with its glow and body recoloured. randomprime
    // ships painted textures for these; the port tints the disc's own.
    struct Look {
      float glow[3]; // what the glow's red becomes
      float body[3]; // the metal's mean colour, of 255
    };
    static const Look kLooks[kShieldKinds] = {
        {{0.25f, 0.8f, 1.f}, {75, 56, 34}},  {{0.f, 1.f, 1.f}, {69, 86, 93}},
        {{1.3f, 0.f, 0.f}, {106, 51, 35}},   {{2.f, 2.f, 2.f}, {158, 167, 162}},
        {{0.5f, 0.f, 1.f}, {47, 23, 63}},    {{1.f, 1.f, 0.f}, {73, 64, 57}},
        {{0.f, 1.f, 0.f}, {53, 46, 34}},
    };
    static const uint32_t kSources[4] = {0x5B97098E, 0x5C7B215C, 0x6E09EA6B, 0xFA0C2AE8};
    static const float kBodyMean[3] = {83, 81, 77};
    const uint32_t base = id & ~7u;
    const uint32_t part = id & 7u;
    const Look& look = kLooks[(id - kShieldBase) / 8];
    if (part == 7)
      return discModel(kMissileShieldCmdl,
                       {{0, base}, {1, base + 1}, {2, base + 2}, {3, base + 3}});
    if (part > 3)
      return false;
    float matrix[3][3] = {};
    for (int c = 0; c < 3; ++c) {
      if (part == 2) {
        matrix[c][c] = look.body[c] / kBodyMean[c];
      } else {
        // The glow is red over a grey base: keep the base, colour the red.
        matrix[c][0] = look.glow[c];
        matrix[c][1] = matrix[c][2] = (1.f - look.glow[c]) / 2.f;
      }
    }
    std::vector<uint8_t> txtr;
    if (!read(kSources[part], txtr) || !TintTxtr(txtr, matrix))
      return false;
    out.type = kTXTR;
    out.data.swap(txtr);
    return true;
  }

  switch (id) {
  case kNothingTxtr:
    out.type = kTXTR;
    out.data = Embedded(kNothingTxtrData);
    return true;
  case kPhazonSuitTxtr1:
    out.type = kTXTR;
    out.data = Embedded(kPhazonSuitTxtr1Data);
    return true;
  case kPhazonSuitTxtr2:
    out.type = kTXTR;
    out.data = Embedded(kPhazonSuitTxtr2Data);
    return true;
  case kDoorPowerHolorimTxtr:
    out.type = kTXTR;
    out.data = Embedded(kDoorPowerHolorimData);
    return true;
  case kDoorBombHolorimTxtr:
    out.type = kTXTR;
    out.data = Embedded(kDoorBombHolorimData);
    return true;
  case kDoorBombPatternTxtr:
    out.type = kTXTR;
    out.data = Embedded(kDoorBombPatternData);
    return true;
  case kDoorBombColorTxtr:
    out.type = kTXTR;
    out.data = Embedded(kDoorBombColorData);
    return true;
  // randomprime's create_custom_door_cmdl: the blue shield with another rim.
  case kDoorPowerCmdl:
    return discModel(kBlueShieldCmdl, {{0, kDoorPowerHolorimTxtr}});
  case kDoorPowerCmdl + 1:
    return discModel(kBlueShieldVerticalCmdl, {{0, kDoorPowerHolorimTxtr}});
  case kDoorBombCmdl:
    return discModel(kBlueShieldCmdl, {{0, kDoorBombHolorimTxtr}});
  case kDoorBombCmdl + 1:
    return discModel(kBlueShieldVerticalCmdl, {{0, kDoorBombHolorimTxtr}});
  case kDoorMissileCmdl:
    return discModel(kBlueShieldCmdl, {{0, 0x459582C1}});
  case kDoorMissileCmdl + 1:
    return discModel(kBlueShieldVerticalCmdl, {{0, 0x459582C1}});
  case kDoorDisabledCmdl:
    return discModel(kBlueShieldCmdl, {{0, 0x717AABCE}});
  case kDoorDisabledCmdl + 1:
    return discModel(kBlueShieldVerticalCmdl, {{0, 0x717AABCE}});
  case kDoorPlasmaVerticalCmdl:
    return discModel(kBlueShieldVerticalCmdl, {{0, 0x61A6945B}});
  case kNothingCmdl:
    return discModel(kMetroidCmdl, {{0, kNothingTxtr}, {1, kNothingTxtr}, {2, kNothingTxtr},
                                    {3, kNothingTxtr}, {4, kNothingTxtr}, {5, kNothingTxtr},
                                    {6, kNothingTxtr}, {7, kNothingTxtr}});
  case kNothingAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kNothingCmdl);
  case kZoomerCmdl:
    return model(Embedded(kZoomerCmdlData), {{0, kNothingTxtr}});
  case kZoomerAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kZoomerCmdl);
  case kCogCmdl:
    return model(Embedded(kCogCmdlData), {});
  case kCogAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kCogCmdl);
  case kPhazonSuitCmdl:
    return discModel(kGravitySuitCmdl, {{0, kPhazonSuitTxtr1}, {3, kPhazonSuitTxtr2}});
  case kPhazonSuitAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kPhazonSuitCmdl);
  case kThermalCmdl:
    return discModel(kVisorCmdl, {{0, 0xFC095F6C}});
  case kThermalAncs:
    return anim(kVisorAncs, kVisorCmdl, kThermalCmdl);
  case kXrayCmdl:
    return discModel(kVisorCmdl, {{0, 0xBE4CD99D}});
  case kXrayAncs:
    return anim(kVisorAncs, kVisorCmdl, kXrayCmdl);
  case kCombatCmdl:
    return discModel(kVisorCmdl, {{0, 0x1D588B22}});
  case kCombatAncs:
    return anim(kVisorAncs, kVisorCmdl, kCombatCmdl);
  default:
    return false;
  }
}

} // namespace

bool TintTxtr(std::vector<uint8_t>& txtr, const float matrix[3][3]) {
  const size_t kHeader = 12;
  if (txtr.size() < kHeader || GetBE32(txtr, 0) != 10)
    return false;
  auto tint = [&](uint16_t c) {
    const float in[3] = {float((c >> 11) & 31) / 31.f, float((c >> 5) & 63) / 63.f,
                         float(c & 31) / 31.f};
    static const float kMax[3] = {31.f, 63.f, 31.f};
    uint16_t result = 0;
    for (int ch = 0; ch < 3; ++ch) {
      float v = matrix[ch][0] * in[0] + matrix[ch][1] * in[1] + matrix[ch][2] * in[2];
      v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
      result = uint16_t(result << (ch == 1 ? 6 : 5) | uint16_t(v * kMax[ch] + 0.5f));
    }
    return result;
  };
  for (size_t at = kHeader; at + 8 <= txtr.size(); at += 8) {
    const uint16_t c0 = uint16_t(txtr[at] << 8 | txtr[at + 1]);
    const uint16_t c1 = uint16_t(txtr[at + 2] << 8 | txtr[at + 3]);
    // c0 > c1 is the four-colour mode; otherwise index 3 is transparent.
    const bool four = c0 > c1;
    uint16_t n0 = tint(c0);
    uint16_t n1 = tint(c1);
    if (four ? n0 < n1 : n0 > n1) {
      std::swap(n0, n1);
      for (int i = 4; i < 8; ++i) {
        uint8_t b = txtr[at + i];
        // 0 and 1 trade places, and with four colours so do 2 and 3.
        for (int shift = 0; shift < 8; shift += 2)
          if (four || ((b >> shift) & 2) == 0)
            b ^= uint8_t(1 << shift);
        txtr[at + i] = b;
      }
    } else if (four && n0 == n1) {
      // Equal colours would mean the other mode: one colour, opaque.
      for (int i = 4; i < 8; ++i)
        txtr[at + i] = 0;
    }
    txtr[at] = uint8_t(n0 >> 8);
    txtr[at + 1] = uint8_t(n0);
    txtr[at + 2] = uint8_t(n1 >> 8);
    txtr[at + 3] = uint8_t(n1);
  }
  return true;
}

bool SetCmdlTexture(std::vector<uint8_t>& cmdl, uint32_t index, uint32_t texture) {
  // Header: magic, version, flags, AABB (6 floats), section count, material
  // set count, section sizes; data starts 32-byte aligned with material set 0,
  // whose first word is its texture count.
  if (cmdl.size() < 44 || GetBE32(cmdl, 0) != 0xDEADBABE || GetBE32(cmdl, 40) == 0)
    return false;
  const uint64_t sections = GetBE32(cmdl, 36);
  const uint64_t dataStart = (44 + 4 * sections + 31) & ~uint64_t(31);
  if (dataStart + 4 > cmdl.size())
    return false;
  const uint32_t count = GetBE32(cmdl, dataStart);
  const uint64_t at = dataStart + 4 + 4 * uint64_t(index);
  if (index >= count || at + 4 > cmdl.size())
    return false;
  SetBE32(cmdl, at, texture);
  return true;
}

bool SetAncsModel(std::vector<uint8_t>& ancs, uint32_t expectedModel, uint32_t model) {
  // u16 version, u16 character set version, u32 character count, then
  // character 0: u32 id, u16 version, name (NUL-terminated), u32 model.
  if (ancs.size() < 14 || GetBE32(ancs, 4) == 0)
    return false;
  size_t at = 14;
  while (at < ancs.size() && ancs[at] != 0)
    ++at;
  ++at;
  if (at + 4 > ancs.size() || GetBE32(ancs, at) != expectedModel)
    return false;
  SetBE32(ancs, at, model);
  return true;
}

const Resource* Find(uint32_t id, const DiscReader& read) {
  if (!IsCustomId(id))
    return nullptr;
  // Scan text keeps its id when its text changes (a check's scan once it is
  // scouted), so a text pair is rebuilt when its revision moves on. The old
  // copy is retired rather than freed: a loader may still hold its pointer.
  struct Built {
    std::unique_ptr<Resource> resource;
    uint32_t revision = 0;
  };
  static std::mutex sMutex;
  static std::map<uint32_t, Built> sBuilt;
  static std::vector<std::unique_ptr<Resource>> sRetired;
  std::lock_guard<std::mutex> lock(sMutex);
  const uint32_t revision = id >= kTextBase ? TextRevision(id) : 0;
  const auto found = sBuilt.find(id);
  if (found != sBuilt.end() && found->second.revision == revision)
    return found->second.resource.get();
  std::unique_ptr<Resource> resource(new Resource);
  if (!Build(id, read, *resource)) {
    // Unknown ids fail quietly (a randomprime disc's other custom assets are
    // found in its PAKs before this is asked); a known one means an odd disc.
    if (id <= kCombatAncs || (id >= kDoorPowerHolorimTxtr && id < kShieldEnd))
      PortLog::Write("custom resource %08X: disc source missing or unexpected\n", id);
    resource.reset();
  }
  Built& built = sBuilt[id];
  if (built.resource)
    sRetired.push_back(std::move(built.resource));
  built.resource = std::move(resource);
  built.revision = revision;
  return built.resource.get();
}

uint32_t TextScan(uint64_t key, const std::string& text) {
  TextRegistry& registry = Texts();
  std::lock_guard<std::mutex> lock(registry.mutex);
  const auto found = registry.ids.find(key);
  if (found != registry.ids.end()) {
    const uint32_t index = (found->second - kTextBase) / 2;
    std::u16string utf16 = Utf16(text);
    if (registry.texts[index] != utf16) {
      registry.texts[index] = std::move(utf16);
      ++registry.revisions[index];
    }
    return found->second;
  }
  const uint32_t index = uint32_t(registry.texts.size());
  if (!IsCustomId(kTextBase + index * 2 + 1))
    return 0;
  registry.texts.push_back(Utf16(text));
  registry.revisions.push_back(1);
  return registry.ids[key] = kTextBase + index * 2;
}

std::vector<uint8_t> MakeStrg(const std::u16string& text) {
  // One language (ENGL, which the game falls back to for any other) holding
  // one string: its offset table, then the UTF-16BE text and a terminator.
  std::vector<uint8_t> strg(28);
  SetBE32(strg, 0, 0x87654321);
  SetBE32(strg, 4, 0);
  SetBE32(strg, 8, 1);
  SetBE32(strg, 12, 1);
  SetBE32(strg, 16, 0x454E474C);
  SetBE32(strg, 20, 0);
  SetBE32(strg, 24, uint32_t(4 + (text.size() + 1) * 2));
  strg.resize(strg.size() + 4);
  SetBE32(strg, 28, 4);
  for (char16_t unit : text) {
    strg.push_back(uint8_t(unit >> 8));
    strg.push_back(uint8_t(unit));
  }
  strg.push_back(0);
  strg.push_back(0);
  return strg;
}

std::vector<uint8_t> MakeScan(uint32_t strg) {
  // randomprime's pickup scans: version 5, the retail scan frame, normal
  // speed, no logbook category, not important, and four empty image slots.
  std::vector<uint8_t> scan(25);
  SetBE32(scan, 0, 5);
  SetBE32(scan, 4, 0x0BADBEEF);
  SetBE32(scan, 8, 0xDCEC3E77);
  SetBE32(scan, 12, strg);
  SetBE32(scan, 16, 0);
  SetBE32(scan, 20, 0);
  scan[24] = 0;
  const float appearance[] = {0.25f, 0.5f, 0.75f, 1.f};
  for (float range : appearance) {
    const size_t at = scan.size();
    scan.resize(at + 28, 0);
    SetBE32(scan, at, 0xFFFFFFFF);
    uint32_t bits;
    std::memcpy(&bits, &range, sizeof(bits));
    SetBE32(scan, at + 4, bits);
    SetBE32(scan, at + 8, 0xFFFFFFFF);
  }
  scan.resize(scan.size() + 23, 0xFF);
  return scan;
}

std::u16string Utf16(const std::string& text) {
  std::u16string out;
  for (size_t i = 0; i < text.size();) {
    const uint8_t lead = uint8_t(text[i]);
    const int extra = lead < 0x80 ? 0 : (lead >> 5) == 6 ? 1 : (lead >> 4) == 14 ? 2 : (lead >> 3) == 30 ? 3 : -1;
    uint32_t code = extra == 0 ? lead : extra == 1 ? lead & 0x1F : extra == 2 ? lead & 0x0F : lead & 0x07;
    bool ok = extra >= 0 && i + size_t(extra) < text.size();
    for (int k = 1; ok && k <= extra; ++k) {
      const uint8_t next = uint8_t(text[i + k]);
      ok = (next & 0xC0) == 0x80;
      code = (code << 6) | (next & 0x3F);
    }
    const uint32_t least = extra == 1 ? 0x80 : extra == 2 ? 0x800 : extra == 3 ? 0x10000 : 0;
    if (!ok || code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      out.push_back(u'\uFFFD');
      ++i;
      continue;
    }
    i += size_t(extra) + 1;
    if (code >= 0x10000) {
      code -= 0x10000;
      out.push_back(char16_t(0xD800 + (code >> 10)));
      out.push_back(char16_t(0xDC00 + (code & 0x3FF)));
    } else {
      out.push_back(char16_t(code));
    }
  }
  return out;
}

} // namespace PortCustomRes
