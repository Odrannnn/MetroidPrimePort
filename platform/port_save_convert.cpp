#include "port_save_convert.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace PortSaveConvert {

namespace {

using u32 = uint32_t;

// ---- per-release tables (generated; see tools/gen_save_layouts.py) ----------

namespace lay {
using u32 = uint32_t;
struct ScanEntry {
  u32 id;
  u32 category;
};
struct AreaEntry {
  u32 mrea;
  u32 firstLayerName;
  u32 layerCount;
  uint64_t defaultLayers;
};
struct WorldEntry {
  u32 mlvl;
  u32 areaCount;
  const u32* relays;
  u32 nRelays;
  const u32* cine;
  u32 nCine;
  const u32* doors;
  u32 nDoors;
  const ScanEntry* scans;
  u32 nScans;
  const AreaEntry* areas;
  u32 nAreas;
  const char* const* layerNames;
};
namespace usa {
#include "save_layouts/usa.inc"
} // namespace usa
namespace pal {
#include "save_layouts/pal.inc"
} // namespace pal
namespace wii {
#include "save_layouts/wii.inc"
} // namespace wii
} // namespace lay

struct Tab {
  const lay::WorldEntry* w;
  size_t n;
};

Tab TabOf(Layout layout) {
  switch (layout) {
  case Layout::Pal:
    return {lay::pal::kWorlds, std::size(lay::pal::kWorlds)};
  case Layout::Wii:
    return {lay::wii::kWorlds, std::size(lay::wii::kWorlds)};
  default:
    return {lay::usa::kWorlds, std::size(lay::usa::kWorlds)};
  }
}

const lay::WorldEntry* FindWorld(Layout layout, u32 mlvl) {
  const Tab tab = TabOf(layout);
  for (size_t i = 0; i < tab.n; ++i) {
    if (tab.w[i].mlvl == mlvl)
      return &tab.w[i];
  }
  return nullptr;
}

const char* LayerName(const lay::WorldEntry& w, const lay::AreaEntry& a, u32 i) {
  return w.layerNames[a.firstLayerName + i];
}

int LayerOccurrence(const lay::WorldEntry& w, const lay::AreaEntry& a, u32 i) {
  int n = 0;
  for (u32 j = 0; j < i; ++j) {
    if (std::strcmp(LayerName(w, a, j), LayerName(w, a, i)) == 0)
      ++n;
  }
  return n;
}

LayerKey KeyOf(const lay::WorldEntry& w, const lay::AreaEntry& a, u32 i) {
  return LayerKey(a.mrea, LayerName(w, a, i), LayerOccurrence(w, a, i));
}

// The scan bits of a slot, in stream order: the union over the worlds, sorted
// by id (stable), duplicates removed. The Trilogy keys by id only.
const std::vector<std::pair<u32, u32>>& ScanList(Layout layout) {
  static std::vector<std::pair<u32, u32>> lists[3];
  static bool built[3] = {};
  auto& list = lists[int(layout)];
  if (!built[int(layout)]) {
    built[int(layout)] = true;
    const Tab tab = TabOf(layout);
    for (size_t i = 0; i < tab.n; ++i) {
      for (u32 k = 0; k < tab.w[i].nScans; ++k) {
        const std::pair<u32, u32> p(tab.w[i].scans[k].id,
                                    layout == Layout::Wii ? kAnyCategory : tab.w[i].scans[k].category);
        if (std::find(list.begin(), list.end(), p) == list.end())
          list.push_back(p);
      }
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    if (layout == Layout::Wii) {
      list.erase(std::unique(list.begin(), list.end(),
                             [](const auto& a, const auto& b) { return a.first == b.first; }),
                 list.end());
    }
  }
  return list;
}

std::vector<std::pair<u32, u32>> CineList(Layout layout) {
  std::vector<std::pair<u32, u32>> list;
  const Tab tab = TabOf(layout);
  for (size_t i = 0; i < tab.n; ++i) {
    for (u32 k = 0; k < tab.w[i].nCine; ++k)
      list.emplace_back(tab.w[i].mlvl, tab.w[i].cine[k]);
  }
  return list;
}

constexpr u32 kPowerMax[41] = {1, 1, 1, 1, 250, 1, 1, 8, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                               1, 1, 1, 14, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

int BitCount(u32 n) {
  int bits = 0;
  while (n != 0) {
    ++bits;
    n >>= 1;
  }
  return bits;
}

constexpr u32 kApMagic = 0x41505834; // "APX4", CGameState.cpp kPortApMagic
constexpr size_t kApBits = kApWords * 32 - 4 * 16;

// ---- bits --------------------------------------------------------------------

struct BitReader {
  const uint8_t* d = nullptr;
  size_t nbits = 0;
  size_t pos = 0;
  bool eof = false;
  BitReader(const uint8_t* data, size_t size) : d(data), nbits(size * 8) {}
  u32 Read(int n) {
    if (eof || pos + size_t(n) > nbits) {
      eof = true;
      pos = nbits;
      return 0;
    }
    u32 v = 0;
    for (int i = 0; i < n; ++i, ++pos)
      v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
    return v;
  }
  size_t Left() const { return nbits - pos; }
};

struct BitWriter {
  std::vector<uint8_t> bytes;
  size_t pos = 0;
  size_t limit;
  bool overflow = false;
  explicit BitWriter(size_t limitBits) : limit(limitBits) { bytes.assign((limitBits + 7) / 8, 0); }
  void Write(u32 v, int n) {
    for (int i = n - 1; i >= 0; --i, ++pos) {
      if (pos >= limit) {
        overflow = true;
        continue;
      }
      if ((v >> i) & 1)
        bytes[pos >> 3] |= uint8_t(0x80 >> (pos & 7));
    }
  }
  bool Fits(size_t n) const { return pos + n <= limit; }
};

// ---- crc32, aes ---------------------------------------------------------------

struct Aes {
  uint8_t sbox[256], inv[256];
  uint8_t rk[176];
  Aes(const uint8_t key[16]) {
    uint8_t p = 1, q = 1;
    do {
      p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
      q = uint8_t(q ^ (q << 1));
      q = uint8_t(q ^ (q << 2));
      q = uint8_t(q ^ (q << 4));
      if (q & 0x80)
        q ^= 0x09;
      auto rotl = [](uint8_t x, int s) { return uint8_t((x << s) | (x >> (8 - s))); };
      const uint8_t x = uint8_t(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4));
      sbox[p] = uint8_t(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; ++i)
      inv[sbox[i]] = uint8_t(i);
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
        const uint8_t t0 = t[0];
        t[0] = uint8_t(sbox[t[1]] ^ rcon);
        t[1] = sbox[t[2]];
        t[2] = sbox[t[3]];
        t[3] = sbox[t0];
        rcon = uint8_t((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
      }
      for (int k = 0; k < 4; ++k)
        rk[i + k] = uint8_t(rk[i - 16 + k] ^ t[k]);
    }
  }
  static uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b != 0) {
      if (b & 1)
        r ^= a;
      a = uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
      b >>= 1;
    }
    return r;
  }
  void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i)
      s[i] = uint8_t(in[i] ^ rk[160 + i]);
    for (int round = 9; round >= 0; --round) {
      uint8_t t[16];
      // InvShiftRows then InvSubBytes (state is column-major).
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
          t[((c + r) % 4) * 4 + r] = inv[s[c * 4 + r]];
      }
      for (int i = 0; i < 16; ++i)
        t[i] ^= rk[round * 16 + i];
      if (round == 0) {
        std::memcpy(s, t, 16);
        break;
      }
      for (int c = 0; c < 4; ++c) {
        const uint8_t* a = t + c * 4;
        s[c * 4 + 0] = uint8_t(Mul(a[0], 14) ^ Mul(a[1], 11) ^ Mul(a[2], 13) ^ Mul(a[3], 9));
        s[c * 4 + 1] = uint8_t(Mul(a[0], 9) ^ Mul(a[1], 14) ^ Mul(a[2], 11) ^ Mul(a[3], 13));
        s[c * 4 + 2] = uint8_t(Mul(a[0], 13) ^ Mul(a[1], 9) ^ Mul(a[2], 14) ^ Mul(a[3], 11));
        s[c * 4 + 3] = uint8_t(Mul(a[0], 11) ^ Mul(a[1], 13) ^ Mul(a[2], 9) ^ Mul(a[3], 14));
      }
    }
    std::memcpy(out, s, 16);
  }
};

u32 BE32(const uint8_t* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void PutBE32(uint8_t* p, u32 v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// The card's checksum: crc32 of everything after the first four bytes, inverted.
u32 CardCrc(const uint8_t* block, size_t size) { return Crc32(block + 4, size - 4) ^ 0xFFFFFFFFu; }

// ---- sizes --------------------------------------------------------------------

constexpr size_t kBlock = 8192;
constexpr size_t kHeader = 64;
constexpr size_t kArtOffset = 0x44;
constexpr size_t kArtSize = 3584 + 1024 + 512;
constexpr size_t kUsaDataOffset = kArtOffset + kArtSize; // 5188
constexpr size_t kUsaSave = 3004;
constexpr size_t kUsaSystem = 174;
constexpr size_t kUsaLead = 162; // 98 NES bytes and 64 option bytes
constexpr size_t kUsaSlot = 940;
constexpr size_t kPalSystem = 114;
constexpr size_t kPalLead = 82;
constexpr size_t kPalSlot = 0xA7F;
constexpr size_t kPalSlotBase = 0x81;
constexpr size_t kSlotHeader = 128;
constexpr size_t kWiiSave = 0x1C000;
constexpr size_t kWiiSlot[3] = {0x780, 0x1720, 0x26C0};
constexpr size_t kWiiSlotSize = 4000;

// ---- slot streams ---------------------------------------------------------------

struct SlotFmt {
  Layout layout;
  bool wii;  // Trilogy: no init bit, no option values, cinematics in the slot, layer total included
  int flags; // option flags stored: 5 (USA, earlier port PAL), 6 (real PAL)
};

void ReadSlot(const uint8_t* d, size_t size, const SlotFmt& f, Slot& s, std::set<std::pair<u32, u32>>* wiiCine) {
  BitReader r(d, size);
  r.pos = kSlotHeader * 8;
  s.timestamp = r.Read(32);
  s.hard = r.Read(1) != 0;
  if (!f.wii)
    s.init = r.Read(1) != 0;
  s.mlvl = r.Read(32);
  s.playHi = r.Read(32);
  s.playLo = r.Read(32);
  s.enabled = r.Read(32);
  s.health = r.Read(32);
  s.beam = r.Read(3);
  s.suit = r.Read(3);
  for (int i = 0; i < 41; ++i) {
    if (kPowerMax[i] > 0) {
      const int n = BitCount(kPowerMax[i]);
      s.amount[i] = r.Read(n);
      s.capacity[i] = r.Read(n);
    }
  }
  for (const auto& scan : ScanList(f.layout)) {
    if (r.Read(1) && !r.eof)
      s.scans.insert(scan);
  }
  s.rates[0] = r.Read(9);
  s.rates[1] = r.Read(9);
  for (auto& b : s.opts)
    b = uint8_t(r.Read(8));
  if (!f.wii) {
    s.sound = r.Read(2);
    s.bright = r.Read(4);
    s.xoff = r.Read(6);
    s.yoff = r.Read(6);
    s.stretch = r.Read(5);
    s.sfx = r.Read(7);
    s.music = r.Read(7);
    s.hud = r.Read(8);
    s.helmet = r.Read(8);
    for (int i = 0; i < f.flags; ++i)
      s.flags[i] = r.Read(1) != 0;
  }
  for (int i = 0; i < 32; ++i) {
    s.hintState[i] = r.Read(2);
    s.hintTime[i] = r.Read(32);
  }
  if (f.wii) {
    s.cine.emplace();
    for (const auto& c : CineList(Layout::Wii)) {
      if (r.Read(1) && !r.eof) {
        s.cine->insert(c);
        if (wiiCine != nullptr)
          wiiCine->insert(c);
      }
    }
  }
  const Tab tab = TabOf(f.layout);
  for (size_t wi = 0; wi < tab.n; ++wi) {
    const lay::WorldEntry& we = tab.w[wi];
    World w;
    w.areaRaw = r.Read(32);
    w.desired = r.Read(32);
    if (r.eof)
      break; // this world and the rest were cut off
    w.present = true;
    if (w.areaRaw < we.nAreas) {
      w.areaKnown = true;
      w.areaMrea = we.areas[w.areaRaw].mrea;
    }
    for (u32 i = 0; i < we.nRelays; ++i) {
      if (r.Read(1) && !r.eof)
        w.relays.insert(we.relays[i]);
    }
    for (u32 i = 0; i < we.areaCount; ++i) {
      if (r.Read(1) && !r.eof && i < we.nAreas)
        w.visited.insert(we.areas[i].mrea);
    }
    for (u32 i = 0; i < we.areaCount; ++i) {
      if (r.Read(1) && !r.eof && i < we.nAreas)
        w.mapped.insert(we.areas[i].mrea);
    }
    for (u32 i = 0; i < we.nDoors; ++i) {
      if (r.Read(1) && !r.eof)
        w.doors.insert(we.doors[i]);
    }
    w.mapStation = r.Read(1) != 0;
    r.Read(10); // layer total; the area tables give it
    for (u32 a = 0; a < we.nAreas; ++a) {
      const lay::AreaEntry& area = we.areas[a];
      for (u32 i = 1; i < area.layerCount; ++i) {
        const bool bit = r.Read(1) != 0;
        if (!r.eof)
          w.layers[KeyOf(we, area, i)] = bit;
      }
    }
    s.worlds[we.mlvl] = std::move(w);
    if (r.eof)
      break; // the last world was cut mid-way: keep what it had
  }
  if (!f.wii && !r.eof && r.Left() >= kApBits) {
    BitReader peek = r;
    if (peek.Read(32) == kApMagic) {
      s.hasAp = true;
      s.ap[0] = kApMagic;
      for (size_t i = 1; i < kApWords; ++i)
        s.ap[i] = peek.Read(i < 11 ? 32 : 16);
    }
  }
  s.present = !r.eof || s.mlvl != 0 || s.timestamp != 0;
}

// `cine`: the Trilogy's cinematics, which it keeps in each slot (Layout::Wii only).
bool WriteSlot(const Slot& s, Layout layout, int flags, size_t slotSize, std::vector<uint8_t>& out,
               Stats& stats, const std::set<std::pair<u32, u32>>* cine = nullptr, size_t* used = nullptr) {
  const bool wii = layout == Layout::Wii;
  const Tab tab = TabOf(layout);
  BitWriter w(slotSize * 8);
  w.pos = kSlotHeader * 8;
  w.Write(s.timestamp, 32);
  w.Write(s.hard ? 1 : 0, 1);
  if (!wii)
    w.Write(s.init ? 1 : 0, 1);
  w.Write(s.mlvl, 32);
  w.Write(s.playHi, 32);
  w.Write(s.playLo, 32);
  w.Write(s.enabled, 32);
  w.Write(s.health, 32);
  w.Write(s.beam & 7, 3);
  w.Write(s.suit & 7, 3);
  for (int i = 0; i < 41; ++i) {
    if (kPowerMax[i] > 0) {
      const int n = BitCount(kPowerMax[i]);
      w.Write(s.amount[i] & ((1u << n) - 1), n);
      w.Write(s.capacity[i] & ((1u << n) - 1), n);
    }
  }
  for (const auto& scan : ScanList(layout))
    w.Write(s.scans.count(scan) ? 1 : 0, 1);
  w.Write(s.rates[0] & 0x1FF, 9);
  w.Write(s.rates[1] & 0x1FF, 9);
  for (uint8_t b : s.opts)
    w.Write(b, 8);
  if (!wii) {
    w.Write(s.sound & 3, 2);
    w.Write(s.bright & 15, 4);
    w.Write(s.xoff & 63, 6);
    w.Write(s.yoff & 63, 6);
    w.Write(s.stretch & 31, 5);
    w.Write(s.sfx & 127, 7);
    w.Write(s.music & 127, 7);
    w.Write(s.hud & 255, 8);
    w.Write(s.helmet & 255, 8);
    for (int i = 0; i < flags; ++i)
      w.Write(s.flags[i] ? 1 : 0, 1);
  }
  for (int i = 0; i < 32; ++i) {
    w.Write(s.hintState[i] & 3, 2);
    w.Write(s.hintTime[i], 32);
  }
  if (wii) {
    for (const auto& c : CineList(Layout::Wii))
      w.Write(cine != nullptr && cine->count(c) ? 1 : 0, 1);
  }
  for (size_t wi = 0; wi < tab.n; ++wi) {
    const lay::WorldEntry& we = tab.w[wi];
    const auto it = s.worlds.find(we.mlvl);
    const World* world = it != s.worlds.end() && it->second.present ? &it->second : nullptr;
    w.Write(world ? world->areaRaw : 0, 32);
    w.Write(world ? world->desired : (we.nAreas > 0 ? we.areas[0].mrea : 0), 32);
    for (u32 i = 0; i < we.nRelays; ++i)
      w.Write(world && world->relays.count(we.relays[i]) ? 1 : 0, 1);
    for (u32 i = 0; i < we.areaCount; ++i)
      w.Write(world && i < we.nAreas && world->visited.count(we.areas[i].mrea) ? 1 : 0, 1);
    for (u32 i = 0; i < we.areaCount; ++i)
      w.Write(world && i < we.nAreas && world->mapped.count(we.areas[i].mrea) ? 1 : 0, 1);
    for (u32 i = 0; i < we.nDoors; ++i)
      w.Write(world && world->doors.count(we.doors[i]) ? 1 : 0, 1);
    w.Write(world && world->mapStation ? 1 : 0, 1);
    u32 total = 0;
    for (u32 a = 0; a < we.nAreas; ++a)
      total += we.areas[a].layerCount > 0 ? we.areas[a].layerCount - 1 : 0;
    w.Write(total, 10);
    for (u32 a = 0; a < we.nAreas; ++a) {
      const lay::AreaEntry& area = we.areas[a];
      for (u32 i = 1; i < area.layerCount; ++i) {
        bool bit = i < 64 && ((area.defaultLayers >> i) & 1) != 0;
        if (world) {
          const auto found = world->layers.find(KeyOf(we, area, i));
          if (found != world->layers.end())
            bit = found->second;
        }
        w.Write(bit ? 1 : 0, 1);
      }
    }
  }
  if (s.hasAp && !wii && w.Fits(kApBits)) {
    for (size_t i = 0; i < kApWords; ++i)
      w.Write(i == 0 ? kApMagic : s.ap[i], i < 11 ? 32 : 16);
  }
  if (w.overflow)
    ++stats.truncatedSlots;
  if (used != nullptr)
    *used = (w.pos + 7) / 8;
  out = std::move(w.bytes);
  return true;
}

// ---- system block ----------------------------------------------------------------

void ReadSystem(const uint8_t* d, size_t size, size_t leadBytes, Lead lead, bool palField, bool cine,
                Layout cineLayout, System& s) {
  s.present = true;
  s.lead = lead;
  s.leadBytes.assign(d, d + std::min(leadBytes, size));
  BitReader r(d, size);
  r.pos = leadBytes * 8;
  s.fps = r.Read(2);
  s.ball = r.Read(2);
  s.powerBombs = r.Read(1);
  s.logScan = r.Read(7);
  for (auto& f : s.flags)
    f = r.Read(1) != 0;
  s.autoMapper = r.Read(2);
  if (palField)
    s.palField = r.Read(3);
  if (cine) {
    for (const auto& c : CineList(cineLayout)) {
      const u32 bit = r.Read(1);
      if (r.eof)
        break;
      if (bit)
        s.cine.insert(c);
    }
  }
}

std::vector<uint8_t> LeadFor(const System& s, bool pal) {
  const size_t want = pal ? kPalLead : kUsaLead;
  std::vector<uint8_t> out(want, 0);
  const auto& b = s.leadBytes;
  // The system block starts with the NES state (USA 98 bytes, PAL and the
  // Trilogy 18), then 64 option bytes (CSystemState::x68_). Across shapes the
  // first 18 NES bytes and the option bytes carry over; USA's NES bytes 18..97
  // become 0.
  const size_t srcNes = s.lead == Lead::Usa ? kUsaLead - 64 : kPalLead - 64;
  const size_t dstNes = want - 64;
  if (s.lead == Lead::None || b.size() != srcNes + 64)
    return out;
  std::copy(b.begin(), b.begin() + std::min(srcNes, dstNes), out.begin());
  std::copy(b.begin() + srcNes, b.end(), out.begin() + dstNes);
  return out;
}

std::vector<uint8_t> WriteSystem(const System& s, Layout layout) {
  const bool pal = layout == Layout::Pal;
  const size_t total = pal ? kPalSystem : kUsaSystem;
  std::vector<uint8_t> lead = LeadFor(s, pal);
  BitWriter w(total * 8);
  std::vector<uint8_t> out(total, 0);
  std::copy(lead.begin(), lead.end(), out.begin());
  w.pos = lead.size() * 8;
  w.Write(s.fps & 3, 2);
  w.Write(s.ball & 3, 2);
  w.Write(s.powerBombs & 1, 1);
  w.Write(s.logScan & 127, 7);
  for (bool f : s.flags)
    w.Write(f ? 1 : 0, 1);
  w.Write(s.autoMapper & 3, 2);
  if (pal)
    w.Write(s.palField & 7, 3);
  // The port keeps only the cinematic bits that fit after the fixed fields.
  for (const auto& c : CineList(layout)) {
    if (!w.Fits(1))
      break;
    w.Write(s.cine.count(c) ? 1 : 0, 1);
  }
  for (size_t i = lead.size(); i < total; ++i)
    out[i] = w.bytes[i];
  return out;
}

// ---- id mapping --------------------------------------------------------------------

// wii2gci.py's map_ids: src index -> dst index (-1 none). Exact ids first, then
// the same area+instance (low 26 bits, the layer moved) when unique on both sides.
std::vector<int> MapIds(const u32* src, u32 ns, const u32* dst, u32 nd) {
  std::vector<int> m(ns, -1);
  std::vector<bool> used(nd, false);
  for (u32 i = 0; i < ns; ++i) {
    for (u32 j = 0; j < nd; ++j) {
      if (dst[j] == src[i]) {
        m[i] = int(j);
        used[j] = true;
        break;
      }
    }
  }
  // wii2gci.py marks every exact destination as used only after all exact
  // matches, as here.
  for (u32 i = 0; i < ns; ++i) {
    if (m[i] >= 0)
      continue;
    const u32 key = src[i] & 0x03FFFFFF;
    int cand = -1, nc = 0;
    for (u32 j = 0; j < nd; ++j) {
      if (!used[j] && (dst[j] & 0x03FFFFFF) == key) {
        cand = int(j);
        ++nc;
      }
    }
    int ns2 = 0;
    for (u32 k = 0; k < ns; ++k) {
      if (m[k] < 0 && (src[k] & 0x03FFFFFF) == key)
        ++ns2;
    }
    if (nc == 1 && ns2 == 1) {
      m[i] = cand;
      used[cand] = true;
    }
  }
  return m;
}

void Note(Stats& st, const char* kind, u32 id) {
  if (st.ids.size() < 200) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s 0x%08X", kind, id);
    st.ids.emplace_back(buffer);
  }
}

std::set<u32> Remap(const std::set<u32>& ids, const u32* src, u32 ns, const u32* dst, u32 nd, int& dropped,
                    Stats& st, const char* kind) {
  std::set<u32> out;
  const std::vector<int> m = MapIds(src, ns, dst, nd);
  for (u32 id : ids) {
    int to = -1;
    for (u32 i = 0; i < ns; ++i) {
      if (src[i] == id) {
        to = m[i];
        break;
      }
    }
    if (to >= 0) {
      out.insert(dst[to]);
    } else {
      ++dropped;
      Note(st, kind, id);
    }
  }
  return out;
}

// ---- containers ---------------------------------------------------------------------

bool ReadUsaContainer(const uint8_t* save, size_t size, Layout layout, int flags, Save& out,
                      std::string& error) {
  if (size < kUsaSave) {
    error = "the save data is too short";
    return false;
  }
  out.layout = layout;
  ReadSystem(save + 7, kUsaSystem, kUsaLead, Lead::Usa, false, true, layout, out.system);
  size_t off = 7 + kUsaSystem;
  const SlotFmt fmt{layout, false, flags};
  for (int i = 0; i < 3; ++i) {
    if (save[4 + i] == 0)
      continue;
    if (off + kUsaSlot > size)
      break;
    ReadSlot(save + off, kUsaSlot, fmt, out.slots[i], nullptr);
    out.slots[i].present = true;
    off += kUsaSlot;
  }
  return true;
}

bool ReadPalContainer(const uint8_t* gci, size_t size, Save& out, std::string& error) {
  if (size < kHeader + 3 * kBlock) {
    error = "the save is cut short";
    return false;
  }
  const uint8_t* copies[2] = {gci + kHeader + kBlock, gci + kHeader + 2 * kBlock};
  bool valid[2];
  for (int i = 0; i < 2; ++i)
    valid[i] = BE32(copies[i]) == CardCrc(copies[i], kBlock);
  int pick;
  if (!valid[0] && !valid[1]) {
    error = "both copies of the save fail their checksum";
    return false;
  } else if (valid[0] != valid[1]) {
    pick = valid[0] ? 0 : 1;
  } else {
    pick = BE32(copies[0] + 4) == BE32(copies[1] + 4) ? 0 : 1;
  }
  const uint8_t* c = copies[pick];
  out.layout = Layout::Pal;
  ReadSystem(c + 0xF, kPalSystem, kPalLead, Lead::Pal, true, true, Layout::Pal, out.system);
  size_t off = kPalSlotBase;
  const SlotFmt fmt{Layout::Pal, false, 6};
  for (int i = 0; i < 3; ++i) {
    if (c[0xC + i] == 0)
      continue;
    if (off + kPalSlot > kBlock)
      break;
    ReadSlot(c + off, kPalSlot, fmt, out.slots[i], nullptr);
    out.slots[i].present = true;
    off += kPalSlot;
  }
  return true;
}

bool ReadWii(const uint8_t* d, size_t size, Save& out, std::string& error) {
  if (size != kWiiSave) {
    error = "not a Trilogy save.bin";
    return false;
  }
  if (BE32(d) != (Crc32(d + 4, 0x1980C - 4) ^ 0xFFFFFFFFu)) {
    error = "the Trilogy save fails its checksum";
    return false;
  }
  out.layout = Layout::Wii;
  const SlotFmt fmt{Layout::Wii, true, 0};
  for (int i = 0; i < 3; ++i) {
    const uint8_t* slot = d + kWiiSlot[i];
    const uint8_t* b = slot + kWiiSlotSize / 2;
    bool any = false;
    for (size_t k = 0; k < kWiiSlotSize / 2 && !any; ++k)
      any = b[k] != 0;
    if (!any)
      continue;
    ReadSlot(b, kWiiSlotSize / 2, fmt, out.slots[i], &out.system.cine);
    out.slots[i].present = true;
    Slot& s = out.slots[i];
    // The Trilogy keeps no option values for this game: take the GameCube defaults.
    s.flags = {true, true, false, true, false, false};
    if (!out.system.present) {
      // Part A is the system block: the same shape as PAL's, without the PAL
      // field and the cinematics (those are in the slot).
      std::set<std::pair<u32, u32>> keep = std::move(out.system.cine);
      ReadSystem(slot, kWiiSlotSize / 2, kPalLead, Lead::Wii, false, false, Layout::Wii, out.system);
      out.system.cine = std::move(keep);
    }
  }
  return true;
}


void PutName(uint8_t* hdr, const char* name) {
  std::memset(hdr + 8, 0, 32);
  std::memcpy(hdr + 8, name, std::strlen(name));
}

std::vector<uint8_t> MakeHeader(bool pal, size_t blocks, int64_t now) {
  std::vector<uint8_t> h(kHeader, 0);
  std::memcpy(h.data(), pal ? "GM8P" : "GM8E", 4);
  h[4] = '0';
  h[5] = '1';
  h[6] = 0xFF;
  h[7] = 0x01;
  PutName(h.data(), pal ? "MetroidPrime" : "MetroidPrime A");
  PutBE32(&h[0x28], u32(now - 946684800));
  PutBE32(&h[0x2C], u32(kArtOffset));
  h[0x30] = 0x00;
  h[0x31] = 0x01;
  h[0x32] = 0x00;
  h[0x33] = 0x02;
  h[0x34] = 0x04;
  h[0x35] = pal ? 0x66 : 0x01;
  h[0x36] = 0;
  h[0x37] = 0; // first block: the card assigns it
  h[0x38] = uint8_t(blocks >> 8);
  h[0x39] = uint8_t(blocks);
  h[0x3A] = h[0x3B] = 0xFF;
  PutBE32(&h[0x3C], 4);
  return h;
}

void PutComment(uint8_t* block, bool pal, int64_t now) {
  const std::time_t t = std::time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), pal ? "%d.%m.%y  %H:%M" : "%m.%d.%y  %H:%M", &tm);
  std::memset(block + 4, ' ', 32);
  std::memcpy(block + 4, "Metroid Prime", 13);
  std::memset(block + 36, 0, 32);
  std::memcpy(block + 36, stamp, std::min<size_t>(std::strlen(stamp), 32));
}

} // namespace

// ---- public ----------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t size) {
  static u32 table[256];
  static bool ready = false;
  if (!ready) {
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  u32 crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

void AesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size, uint8_t* out) {
  const Aes aes(key);
  uint8_t prev[16];
  std::memcpy(prev, iv, 16);
  for (size_t off = 0; off + 16 <= size; off += 16) {
    uint8_t block[16], next[16];
    std::memcpy(next, in + off, 16);
    aes.DecryptBlock(in + off, block);
    for (int i = 0; i < 16; ++i)
      out[off + i] = uint8_t(block[i] ^ prev[i]);
    std::memcpy(prev, next, 16);
  }
}

const char* FormatName(Format format) {
  switch (format) {
  case Format::UsaGci:
    return "USA";
  case Format::PalGci:
    return "PAL";
  case Format::PalLegacyGci:
    return "PAL (earlier port save)";
  case Format::WiiSave:
  case Format::WiiDataBin:
    return "Wii";
  default:
    return "unknown";
  }
}

Layout LayoutOf(Format format) {
  switch (format) {
  case Format::PalGci:
  case Format::PalLegacyGci:
    return Layout::Pal;
  case Format::WiiSave:
  case Format::WiiDataBin:
    return Layout::Wii;
  default:
    return Layout::Usa;
  }
}

std::string Stats::Summary() const {
  if (!Any())
    return "nothing dropped";
  std::string text = "dropped";
  char buffer[64];
  auto add = [&](int n, const char* what) {
    if (n > 0) {
      std::snprintf(buffer, sizeof(buffer), " %d %s", n, what);
      text += buffer;
    }
  };
  add(relays, "relays");
  add(doors, "doors");
  add(cinematics, "cinematics");
  add(scans, "scans");
  add(worlds, "worlds");
  add(areas, "areas");
  if (truncatedSlots > 0)
    text += " (a slot was cut short)";
  return text;
}

bool DecryptDataBin(const uint8_t* d, size_t size, std::vector<uint8_t>& saveBin, std::string& error) {
  static const uint8_t kKey[16] = {0xab, 0x01, 0xb9, 0xd8, 0xe1, 0x62, 0x2b, 0x08,
                                   0xaf, 0xba, 0xd8, 0x4d, 0xbf, 0xc2, 0xa5, 0x5d};
  constexpr size_t kBk = 0xF0C0;
  constexpr size_t kFileHeader = 0x80;
  saveBin.clear();
  if (size < kBk + 0x70 || d[kBk + 4] != 'B' || d[kBk + 5] != 'k') {
    error = "not a Wii data.bin";
    return false;
  }
  const u32 nfiles = BE32(d + kBk + 0xC);
  if (nfiles == 0 || nfiles > 64) {
    error = "the data.bin has an invalid file count";
    return false;
  }
  size_t off = kBk + 0x70;
  for (u32 n = 0; n < nfiles; ++n) {
    if (off + 4 > size) {
      error = "the data.bin is cut short";
      return false;
    }
    if (BE32(d + off) != 0x03ADF17Eu)
      off += 0x10;
    if (off + kFileHeader > size || BE32(d + off) != 0x03ADF17Eu) {
      error = "the data.bin has a bad file header";
      return false;
    }
    const uint8_t* h = d + off;
    const u32 fsize = BE32(h + 4);
    const uint8_t type = h[10];
    char name[0x46] = {};
    std::memcpy(name, h + 11, 0x45);
    off += kFileHeader;
    if (type != 1)
      continue;
    const size_t padded = (size_t(fsize) + 0x3F) & ~size_t(0x3F);
    if (padded < fsize || off + padded > size) {
      error = "the data.bin is cut short";
      return false;
    }
    const size_t len = std::strlen(name);
    if (len >= 8 && std::strcmp(name + len - 8, "save.bin") == 0) {
      std::vector<uint8_t> plain(padded);
      AesCbcDecrypt(kKey, h + 0x50, d + off, padded, plain.data());
      plain.resize(fsize);
      saveBin = std::move(plain);
      return true;
    }
    off += padded;
  }
  error = "the data.bin has no save.bin";
  return false;
}

Format NativeFormat(bool pal) { return pal ? Format::PalGci : Format::UsaGci; }

Format Detect(const uint8_t* d, size_t size) {
  if (d == nullptr)
    return Format::Unknown;
  if (size >= 0xF0C0 + 0x70 && d[0xF0C4] == 'B' && d[0xF0C5] == 'k')
    return Format::WiiDataBin;
  if (size == kWiiSave)
    return Format::WiiSave;
  if (size >= kHeader + kBlock && d[0] == 'G' && d[1] == 'M' && d[2] == '8') {
    if (d[3] == 'E')
      return Format::UsaGci;
    if (d[3] == 'P') {
      // The port's earlier PAL saves are named like USA's ("MetroidPrime A"/"B").
      const bool legacy = std::memcmp(d + 8, "MetroidPrime ", 13) == 0;
      if (legacy)
        return Format::PalLegacyGci;
      return size >= kHeader + 3 * kBlock ? Format::PalGci : Format::Unknown;
    }
  }
  return Format::Unknown;
}

std::vector<uint8_t> ArtOf(const uint8_t* gci, size_t size) {
  if (gci == nullptr || size < kHeader + kArtOffset + kArtSize)
    return {};
  return std::vector<uint8_t>(gci + kHeader + kArtOffset, gci + kHeader + kArtOffset + kArtSize);
}

bool Read(const uint8_t* data, size_t size, Format& format, Save& out, std::vector<uint8_t>* art,
          std::string& error) {
  out = Save{};
  format = Detect(data, size);
  std::vector<uint8_t> bin;
  bool ok = false;
  switch (format) {
  case Format::UsaGci:
  case Format::PalLegacyGci: {
    const bool legacy = format == Format::PalLegacyGci;
    if (size < kHeader + kBlock) {
      error = "the file is too short";
      return false;
    }
    ok = ReadUsaContainer(data + kHeader + kUsaDataOffset, size - kHeader - kUsaDataOffset,
                          legacy ? Layout::Pal : Layout::Usa, 5, out, error);
    break;
  }
  case Format::PalGci:
    ok = ReadPalContainer(data, size, out, error);
    break;
  case Format::WiiSave:
    ok = ReadWii(data, size, out, error);
    break;
  case Format::WiiDataBin:
    ok = DecryptDataBin(data, size, bin, error) && ReadWii(bin.data(), bin.size(), out, error);
    break;
  default:
    error = "not a Metroid Prime save";
    return false;
  }
  if (!ok)
    return false;
  if (!out.slots[0].present && !out.slots[1].present && !out.slots[2].present) {
    error = "the save has no used slots";
    return false;
  }
  if (art != nullptr)
    *art = ArtOf(data, size);
  return true;
}

void Retarget(Save& save, Layout to, Stats& stats) {
  const Layout from = save.layout;
  if (from == to)
    return;
  // Scans: the target's entry with the same id (and category, unless the
  // Trilogy's category-less one is on either side).
  const auto& list = ScanList(to);
  for (Slot& s : save.slots) {
    s.cine.reset(); // the system set carries them over
    std::set<std::pair<u32, u32>> scans;
    for (const auto& sc : s.scans) {
      bool hit = false;
      for (const auto& t : list) {
        if (t.first == sc.first &&
            (t.second == sc.second || t.second == kAnyCategory || sc.second == kAnyCategory)) {
          scans.insert(t);
          hit = true;
        }
      }
      if (!hit) {
        ++stats.scans;
        Note(stats, "scan", sc.first);
      }
    }
    s.scans = std::move(scans);
    std::map<u32, World> worlds;
    for (auto& [mlvl, w] : s.worlds) {
      const lay::WorldEntry* src = FindWorld(from, mlvl);
      const lay::WorldEntry* dst = FindWorld(to, mlvl);
      if (dst == nullptr || src == nullptr) {
        ++stats.worlds;
        Note(stats, "world", mlvl);
        continue;
      }
      World n = w;
      n.relays = Remap(w.relays, src->relays, src->nRelays, dst->relays, dst->nRelays, stats.relays, stats, "relay");
      n.doors = Remap(w.doors, src->doors, src->nDoors, dst->doors, dst->nDoors, stats.doors, stats, "door");
      auto areaIn = [&](u32 mrea) {
        for (u32 i = 0; i < dst->nAreas; ++i) {
          if (dst->areas[i].mrea == mrea)
            return true;
        }
        return false;
      };
      auto keepAreas = [&](const std::set<u32>& in) {
        std::set<u32> r;
        for (u32 m : in) {
          if (areaIn(m)) {
            r.insert(m);
          } else {
            ++stats.areas;
            Note(stats, "area", m);
          }
        }
        return r;
      };
      n.visited = keepAreas(w.visited);
      n.mapped = keepAreas(w.mapped);
      n.layers.clear();
      for (const auto& [key, bit] : w.layers) {
        for (u32 a = 0; a < dst->nAreas; ++a) {
          const lay::AreaEntry& area = dst->areas[a];
          if (area.mrea != std::get<0>(key))
            continue;
          for (u32 i = 1; i < area.layerCount; ++i) {
            if (KeyOf(*dst, area, i) == key)
              n.layers[key] = bit;
          }
        }
      }
      if (w.areaKnown) {
        for (u32 i = 0; i < dst->nAreas; ++i) {
          if (dst->areas[i].mrea == w.areaMrea) {
            n.areaRaw = i;
            break;
          }
        }
      }
      n.areaKnown = n.areaRaw < dst->nAreas;
      if (n.areaKnown)
        n.areaMrea = dst->areas[n.areaRaw].mrea;
      worlds[mlvl] = std::move(n);
    }
    s.worlds = std::move(worlds);
  }
  // Cinematics, per world.
  std::set<std::pair<u32, u32>> cine;
  const Tab tab = TabOf(from);
  for (size_t i = 0; i < tab.n; ++i) {
    std::set<u32> ids;
    for (const auto& c : save.system.cine) {
      if (c.first == tab.w[i].mlvl)
        ids.insert(c.second);
    }
    const lay::WorldEntry* dst = FindWorld(to, tab.w[i].mlvl);
    if (dst == nullptr) {
      stats.cinematics += int(ids.size());
      continue;
    }
    for (u32 id : Remap(ids, tab.w[i].cine, tab.w[i].nCine, dst->cine, dst->nCine, stats.cinematics, stats, "cinematic"))
      cine.emplace(dst->mlvl, id);
  }
  save.system.cine = std::move(cine);
  save.layout = to;
}

bool WriteGci(const Save& save, bool pal, const std::vector<uint8_t>& art, int64_t now,
              std::vector<uint8_t>& gci, Stats& stats, std::string& error) {
  const Layout want = pal ? Layout::Pal : Layout::Usa;
  if (save.layout != want) {
    error = "the save is not in the target layout";
    return false;
  }
  if (now == 0)
    now = int64_t(std::time(nullptr));
  const size_t blocks = pal ? 3 : 1;
  gci = MakeHeader(pal, blocks, now);
  gci.resize(kHeader + blocks * kBlock, 0);
  uint8_t* sector0 = gci.data() + kHeader;
  PutComment(sector0, pal, now);
  if (art.size() == kArtSize)
    std::memcpy(sector0 + kArtOffset, art.data(), kArtSize);
  const std::vector<uint8_t> sys = WriteSystem(save.system, want);
  const size_t slotSize = pal ? kPalSlot : kUsaSlot;
  std::vector<std::vector<uint8_t>> slots;
  uint8_t present[3] = {};
  for (int i = 0; i < 3; ++i) {
    if (!save.slots[i].present)
      continue;
    present[i] = 1;
    slots.emplace_back();
    WriteSlot(save.slots[i], want, pal ? 6 : 5, slotSize, slots.back(), stats);
  }
  if (!pal) {
    uint8_t* d = sector0 + kUsaDataOffset;
    std::memcpy(d + 4, present, 3);
    std::memcpy(d + 7, sys.data(), sys.size());
    size_t off = 7 + sys.size();
    for (const auto& s : slots) {
      std::memcpy(d + off, s.data(), s.size());
      off += s.size();
    }
    PutBE32(sector0, CardCrc(sector0, kBlock));
    return true;
  }
  for (int c = 0; c < 2; ++c) {
    uint8_t* d = gci.data() + kHeader + (1 + c) * kBlock;
    PutBE32(d + 4, 0);
    PutBE32(d + 8, 0);
    std::memcpy(d + 0xC, present, 3);
    std::memcpy(d + 0xF, sys.data(), sys.size());
    size_t off = kPalSlotBase;
    for (const auto& s : slots) {
      std::memcpy(d + off, s.data(), s.size());
      off += s.size();
    }
    PutBE32(d, CardCrc(d, kBlock));
  }
  PutBE32(sector0, CardCrc(sector0, kBlock));
  return true;
}

bool ConvertTo(const uint8_t* data, size_t size, Format target, const std::vector<uint8_t>& art,
               Conversion& out, std::string& error) {
  out = Conversion{};
  if (target != Format::UsaGci && target != Format::PalGci) {
    error = std::string("cannot write ") + FormatName(target) + " saves";
    return false;
  }
  const bool pal = target == Format::PalGci;
  Save save;
  std::vector<uint8_t> sourceArt;
  if (!Read(data, size, out.from, save, &sourceArt, error))
    return false;
  for (const Slot& s : save.slots) {
    if (s.present)
      out.newest = std::max(out.newest, s.timestamp);
  }
  Retarget(save, pal ? Layout::Pal : Layout::Usa, out.stats);
  return WriteGci(save, pal, sourceArt.empty() ? art : sourceArt, 0, out.gci, out.stats, error);
}

bool ConvertToDisc(const uint8_t* data, size_t size, bool pal, const std::vector<uint8_t>& art,
                   Conversion& out, std::string& error) {
  return ConvertTo(data, size, NativeFormat(pal), art, out, error);
}

namespace {

// The Trilogy's save.bin, beyond the slots (build/wii-save/FINDINGS.md and
// build/s-trilogyexport-20261011/NOTES.md). A header at 4 (region id, ...,
// one 7-byte entry per file at 0x11; the game treats a file as used when its
// bytes 0 and 4 are set), one 600-byte summary per file that the file select
// shows as written (MP1's part at +0x47), and at 0x76C a game-data constant the
// load checks whenever a file is used.
constexpr size_t kWiiEntry = 0x11;
constexpr size_t kWiiSummary[3] = {0x64, 0x2BC, 0x514};
constexpr size_t kWiiSummaryMp1 = 0x47;
constexpr size_t kWiiHash = 0x76C;
constexpr u32 kWiiHashValue = 0x4F54DF87;
constexpr size_t kWiiCrcEnd = 0x1980C;
constexpr u32 kFrigateMlvl = 0x158EFE17;

bool WiiFileUsed(const uint8_t* bin, int i) {
  const uint8_t* e = bin + kWiiEntry + 7 * size_t(i);
  return e[0] != 0 && e[4] != 0;
}

// Whether a file belongs to a Mii: its summary carries the Mii's id (all 0xFF
// otherwise), set by the Trilogy before the file has any MP1 progress.
bool WiiFileOwned(const uint8_t* bin, int i) {
  const uint8_t* id = bin + kWiiSummary[i] + 0x3F;
  return std::any_of(id, id + 8, [](uint8_t b) { return b != 0xFF; });
}

// CPlayerState::CalculateItemCollectionPercentage, as CGameState's file
// summary computes it (0 on the Frigate): the pickups, of 99, as a percentage.
u32 ItemPercent(const Slot& s) {
  if (s.mlvl == kFrigateMlvl)
    return 0;
  const auto& c = s.capacity;
  u32 rate = c[4] / 5 + (c[7] >= 4 ? c[7] - 3 : c[7]);
  for (int i : {1, 2, 3, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 21, 22, 24, 28})
    rate += c[i];
  for (int i = 29; i <= 40; ++i) // the artifacts
    rate += c[i];
  return rate * 100 / 99;
}

} // namespace

bool WriteWii(const Save& save, const uint8_t* base, size_t size, std::vector<uint8_t>& bin, Stats& stats,
              std::string& error) {
  if (save.layout != Layout::Wii) {
    error = "the save is not in the target layout";
    return false;
  }
  Save probe;
  if (!ReadWii(base, size, probe, error)) {
    error = "the Trilogy save to write into: " + error;
    return false;
  }
  bin.assign(base, base + size);
  uint8_t* d = bin.data();
  // A used file of the base is the model for a file it has empty.
  int model = -1;
  for (int i = 0; i < 3 && model < 0; ++i) {
    if (WiiFileUsed(d, i))
      model = i;
  }
  bool any = false;
  for (int i = 0; i < 3; ++i) {
    const Slot& s = save.slots[i];
    if (!s.present)
      continue; // the Trilogy file keeps what it had
    any = true;
    uint8_t* slot = d + kWiiSlot[i];
    uint8_t* sum = d + kWiiSummary[i];
    uint8_t* entry = d + kWiiEntry + 7 * size_t(i);
    const bool used = WiiFileUsed(d, i);
    if (!used) {
      if (model >= 0) {
        std::memcpy(entry, d + kWiiEntry + 7 * size_t(model), 7);
        std::memcpy(sum, d + kWiiSummary[model], kWiiSummaryMp1);
        std::memcpy(slot, d + kWiiSlot[model], kWiiSlotSize / 2);
      } else {
        static constexpr uint8_t kEntry[7] = {1, 1, 0, 0, 1, 0, 0}; // a real PAL save's
        std::memcpy(entry, kEntry, 7);
      }
    }
    // Part A: the options. Its leading 82 bytes stay zero (as in every Trilogy
    // save); the bits after them are the system block's, then a 3-bit field
    // the GameCube has no counterpart of, kept.
    if (save.system.present) {
      BitWriter w(19);
      w.Write(save.system.fps & 3, 2);
      w.Write(save.system.ball & 3, 2);
      w.Write(save.system.powerBombs & 1, 1);
      w.Write(save.system.logScan & 127, 7);
      for (bool f : save.system.flags)
        w.Write(f ? 1 : 0, 1);
      w.Write(save.system.autoMapper & 3, 2);
      uint8_t* a = slot + kPalLead;
      a[0] = w.bytes[0];
      a[1] = w.bytes[1];
      a[2] = uint8_t((a[2] & 0x1F) | (w.bytes[2] & 0xE0));
    }
    // Part B: the slot stream, then bytes the Trilogy adds (26 of them, mostly
    // 1 in finished games, meaning unknown), kept from the file it replaces;
    // a new file has them zero.
    std::vector<uint8_t> stream;
    size_t streamUsed = 0;
    WriteSlot(s, Layout::Wii, 0, kWiiSlotSize / 2, stream, stats, s.cine ? &*s.cine : &save.system.cine,
              &streamUsed);
    uint8_t* b = slot + kWiiSlotSize / 2;
    if (!used)
      std::memset(b, 0, kWiiSlotSize / 2);
    std::memcpy(b, stream.data(), std::min(streamUsed, kWiiSlotSize / 2));
    // MP1's summary: the difficulty (0 Normal, 1 Veteran; 1 when empty), a u32
    // (-1 when empty, else 4 or 3), the item percentage, the play time (a
    // double), the energy tanks, three bytes behind a count, a u32, the world,
    // two bytes (a 1 in the first hides the file). Kept when the file had MP1
    // progress: the second u32, the three bytes and the last two, whose
    // meaning is unknown, and a difficulty above Veteran on a hard slot.
    uint8_t* m = sum + kWiiSummaryMp1;
    const bool hadMp1 = BE32(m + 4) != 0xFFFFFFFFu && BE32(m + 24) == 3;
    const u32 second = hadMp1 ? BE32(m + 4) : 4;
    const u32 difficulty = hadMp1 && s.hard && BE32(m) > 1 ? BE32(m) : (s.hard ? 1 : 0);
    uint8_t keep[5] = {};
    if (hadMp1) {
      std::memcpy(keep, m + 28, 3);
      std::memcpy(keep + 3, m + 39, 2);
    }
    std::memset(m, 0, 41);
    PutBE32(m, difficulty);
    PutBE32(m + 4, second);
    PutBE32(m + 8, ItemPercent(s));
    PutBE32(m + 12, s.playHi);
    PutBE32(m + 16, s.playLo);
    PutBE32(m + 20, s.capacity[24]);
    PutBE32(m + 24, 3);
    std::memcpy(m + 28, keep, 3);
    PutBE32(m + 35, s.mlvl);
    std::memcpy(m + 39, keep + 3, 2);
  }
  if (!any) {
    error = "the save has no slots";
    return false;
  }
  if (BE32(d + kWiiHash) == 0)
    PutBE32(d + kWiiHash, kWiiHashValue);
  PutBE32(d, Crc32(d + 4, kWiiCrcEnd - 4) ^ 0xFFFFFFFFu);
  return true;
}

bool ConvertToTrilogy(const uint8_t* data, size_t size, const uint8_t* base, size_t baseSize, Conversion& out,
                      std::string& error) {
  out = Conversion{};
  Save save;
  if (!Read(data, size, out.from, save, nullptr, error))
    return false;
  // The Trilogy shows one MP1 file per Mii, the first whose summary carries the
  // Mii's id, which only the Trilogy can write: so the newest slot goes into a
  // file a Mii owns (the header's current one when owned, else the first).
  int target = -1;
  if (baseSize == kWiiSave) {
    const u32 current = BE32(base + 8);
    for (int i = 0; i < 3 && target < 0; ++i) {
      if (WiiFileOwned(base, i))
        target = i;
    }
    if (current < 3 && WiiFileOwned(base, int(current)))
      target = int(current);
  }
  if (target < 0) {
    error = "the Trilogy save has no Metroid Prime file yet: start Metroid Prime in the Trilogy once first";
    return false;
  }
  int newest = -1;
  for (int i = 0; i < 3; ++i) {
    if (save.slots[i].present && (newest < 0 || save.slots[i].timestamp > save.slots[newest].timestamp))
      newest = i;
  }
  if (newest < 0) {
    error = "the save has no slots";
    return false;
  }
  out.newest = save.slots[newest].timestamp;
  Slot chosen = std::move(save.slots[newest]);
  for (Slot& s : save.slots)
    s = Slot{};
  save.slots[target] = std::move(chosen);
  Retarget(save, Layout::Wii, out.stats);
  return WriteWii(save, base, baseSize, out.gci, out.stats, error);
}

} // namespace PortSaveConvert
