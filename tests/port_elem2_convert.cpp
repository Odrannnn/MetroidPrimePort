#include "port_remastered_effect_convert.h"
#include "port_retail_check.h"
#include "port_vfx_opacity.h"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "port_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include "Kyoto/Particles/PortParticleMath.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

namespace {

// Remastered side: little-endian, FourCCs byte-reversed.
void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

uint32_t Bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

void PutFourCC(std::vector<uint8_t>& out, const char* text) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(text[i]));
  }
}

void PutProperty(std::vector<uint8_t>& out, const char* name, uint8_t tag) {
  PutFourCC(out, name);
  out.push_back(tag);
}

void PutConstant(std::vector<uint8_t>& out, uint32_t word) {
  PutFourCC(out, "CNST");
  Put32(out, word);
}

void PutGenerator(std::vector<uint8_t>& out, bool root) {
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, root ? 1 : 0);
}

void PutGuid(std::vector<uint8_t>& out, const EffectGuid& guid) { out.insert(out.end(), guid.begin(), guid.end()); }

// An id carried over from retail, as an effect stores it.
EffectGuid Legacy(uint32_t retail) {
  EffectGuid guid = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  guid[12] = uint8_t(retail >> 24);
  guid[13] = uint8_t(retail >> 16);
  guid[14] = uint8_t(retail >> 8);
  guid[15] = uint8_t(retail);
  return guid;
}

EffectGuid Fresh(uint8_t seed) {
  EffectGuid guid;
  for (size_t i = 0; i < guid.size(); ++i) {
    guid[i] = uint8_t(0xa0 + seed + i);
  }
  return guid;
}

// A texture carried over from retail, so the generator draws something.
void PutTexr(std::vector<uint8_t>& out, uint32_t retail) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  PutGuid(out, Legacy(retail));
  PutFourCC(out, "NONE");
}

// Retail side: big-endian.
struct Retail {
  std::vector<uint8_t> bytes;
  Retail& f(const char* text) {
    bytes.insert(bytes.end(), text, text + 4);
    return *this;
  }
  // Converted PART: insert early PIRN, then finish with default PSLT and _END.
  Retail& pirn() {
    const uint8_t marker[] = {'P','I','R','N','C','N','S','T',0,0,0,1};
    bytes.insert(bytes.begin() + 4, std::begin(marker), std::end(marker));
    return *this;
  }
  Retail& end() { return pirn().pslt().f("_END"); }
  // An embedded child's end also carries PGID (GUID's first word read little-endian).
  Retail& end(const EffectGuid& id) {
    return pirn().pslt().f("PGID").f("CNST")
        .w(uint32_t(id[0]) | uint32_t(id[1]) << 8 | uint32_t(id[2]) << 16 | uint32_t(id[3]) << 24)
        .f("_END");
  }
  // Remastered's system lifetime without a PSLT (CElementGen ctor 0x244cfc).
  Retail& pslt() { return f("PSLT").f("CNST").w(0x1000000u); }
  Retail& w(uint32_t value) {
    for (int i = 3; i >= 0; --i) {
      bytes.push_back(uint8_t(value >> (i * 8)));
    }
    return *this;
  }
  Retail& b(uint8_t value) {
    bytes.push_back(value);
    return *this;
  }
};

Retail& RetailVector(Retail& out, float x, float y, float z) {
  out.f("CNST").f("CNST").w(Bits(x)).f("CNST").w(Bits(y)).f("CNST").w(Bits(z));
  return out;
}


// Element batch 2 converter output: NEGT, then the PLNV count rule, CRCV, SPEV and ELPS.

// A generator with the given properties written by `props`, one TEXR so it draws something.
std::vector<ConvertedPart> ConvertGen(const std::function<void(std::vector<uint8_t>&)>& props, EffectNode& effect,
                                      std::vector<uint8_t>& out) {
  out.assign(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  props(out);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "effect parses");
  return PortRemastered::CheckedConvert(effect, out.data());
}

bool Contains(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& what) {
  return std::search(bytes.begin(), bytes.end(), what.begin(), what.end()) != bytes.end();
}

void Show(const ConvertedPart& part) {
  for (const std::string& d : part.dropped) {
    std::fprintf(stderr, "  dropped %s\n", d.c_str());
  }
}

// NEGT(x) is MULT(x, CNST -1).
void TestNegate() {
  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "SIZE", 3);
        PutFourCC(o, "RAND");
        PutFourCC(o, "NEGT");
        PutConstant(o, Bits(2.0f));
        PutConstant(o, Bits(2.0f));
      },
      effect, out);
  Retail want;
  want.f("GPSM");
  want.f("SIZE").f("RAND").f("MULT").f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(-1.0f)).f("CNST").w(Bits(2.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "RAND(NEGT(a), a) is RAND(MULT(a, CNST -1), a)");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "NEGT drops nothing");
  if (parts.size() == 1) {
    Show(parts[0]);
  }
}


// PLNV: Remastered's Compile2d grid and shuffler from the owning generator's MAXP.
struct PlnvRow {
  uint32_t maxp, nx, nz;
  int flag;
  uint32_t gx, gz;  // the grid GetValue reads (each count at least 1)
  uint32_t shuffle; // the shuffler's cells, 0 for none
};

// One PLNV generator; `maxpFirst` puts MAXP before the EMTR.
std::vector<ConvertedPart> ConvertPlnv(uint32_t maxp, bool maxpConst, uint32_t nx, uint32_t nz, int flag,
                                       bool maxpFirst, std::vector<uint8_t>& out) {
  EffectNode effect;
  auto vec = [](std::vector<uint8_t>& o, float x, float y, float z) {
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(x));
    PutConstant(o, Bits(y));
    PutConstant(o, Bits(z));
  };
  auto putMaxp = [&](std::vector<uint8_t>& o) {
    PutProperty(o, "MAXP", 3);
    if (maxpConst) {
      PutConstant(o, maxp);
    } else {
      PutFourCC(o, "ABS_");
      PutConstant(o, maxp);
    }
  };
  return ConvertGen(
      [&](std::vector<uint8_t>& o) {
        if (maxpFirst) {
          putMaxp(o);
        }
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "PLNV");
        vec(o, 0, 0, 0);
        vec(o, 0, 0, 0);
        PutConstant(o, Bits(5.0f));
        PutConstant(o, Bits(2.0f));
        PutConstant(o, Bits(3.0f));
        PutConstant(o, Bits(0.0f));
        PutConstant(o, nx);
        PutConstant(o, Bits(1.0f));
        PutConstant(o, nz);
        PutConstant(o, Bits(0.0f));
        o.push_back(uint8_t(flag));
        if (!maxpFirst) {
          putMaxp(o);
        }
      },
      effect, out);
}

// The bytes PLNV ends with: n1, x variance, n2, z variance, shuffler size.
std::vector<uint8_t> PlnvTail(uint32_t n1, uint32_t n2, uint32_t shuffle) {
  Retail t;
  t.f("CNST").w(n1).f("CNST").w(Bits(1.0f)).f("CNST").w(n2).f("CNST").w(Bits(0.0f)).b(uint8_t(shuffle));
  return t.bytes;
}

void TestPlnvCounts() {
  // Every use in the Switch dumps (build/s-elemdrops2-20261019/plnv_cmp.txt, Remastered column).
  static const PlnvRow kRows[] = {
    {2, 0, 0, 1, 2, 1, 2},  // 01efdc65:13
    {32, 4, 0, 1, 4, 8, 32},  // 01efdc65:67
    {10, 0, 0, 1, 3, 3, 9},  // 2170745a:87
    {10, 0, 0, 1, 3, 3, 9},  // 2170745a:109
    {10, 0, 0, 1, 3, 3, 9},  // 2170745a:131
    {10, 0, 0, 1, 3, 3, 9},  // 2170745a:153
    {1, 0, 0, 0, 1, 1, 0},  // 2896ee18:41
    {1, 0, 0, 0, 1, 1, 0},  // 2fab93e3:26
    {60, 0, 0, 1, 8, 7, 56},  // 37e5393c:14
    {100, 0, 0, 1, 10, 9, 90},  // 37e5393c:38
    {4, 0, 0, 1, 2, 1, 2},  // 39597d19:13
    {15, 0, 0, 1, 4, 3, 12},  // 3de2bff3:47
    {9, 0, 0, 0, 3, 2, 0},  // 3f89d502:143
    {15, 0, 0, 1, 4, 3, 12},  // 40b89419:48
    {6, 0, 0, 1, 3, 2, 6},  // 48fd54ed:30
    {1, 0, 0, 1, 1, 1, 1},  // 49214922:13
    {3, 0, 0, 1, 2, 1, 2},  // 4ed39df9:13
    {4, 1, 0, 1, 1, 4, 4},  // 58488d63:36
    {1, 0, 0, 1, 1, 1, 1},  // 58488d63:81
    {1, 0, 0, 0, 1, 1, 0},  // 5de5a938:72
    {4, 0, 0, 1, 2, 1, 2},  // 6bc93d36:13
    {24, 4, 0, 1, 4, 6, 24},  // 6bc93d36:67
    {6, 0, 0, 0, 3, 2, 0},  // 6bd19865:12
    {25, 0, 0, 1, 5, 4, 20},  // 6bd19865:31
    {30, 0, 0, 0, 6, 5, 0},  // 6bd19865:72
    {2, 0, 0, 1, 2, 1, 2},  // 6ffcb3be:13
    {5, 0, 0, 1, 2, 2, 4},  // 73f748b3:12
    {5, 0, 0, 1, 2, 2, 4},  // 73f748b3:42
    {25, 0, 0, 1, 5, 4, 20},  // 73f748b3:70
    {2, 0, 0, 1, 2, 1, 2},  // 7708be79:13
    {20, 4, 0, 1, 4, 5, 20},  // 7708be79:67
    {8, 0, 0, 1, 3, 2, 6},  // 78319d52:56
    {4, 0, 0, 1, 2, 1, 2},  // 7c758b24:87
    {12, 0, 0, 1, 4, 3, 12},  // 80711a86:32
    {2, 0, 0, 1, 2, 1, 2},  // 84fcbc0f:13
    {20, 4, 0, 1, 4, 5, 20},  // 84fcbc0f:67
    {6, 0, 0, 0, 3, 2, 0},  // 896c4450:15
    {6, 0, 0, 1, 3, 2, 6},  // 896c4450:35
    {128, 0, 0, 1, 11, 11, 121},  // 896c4450:58
    {25, 0, 0, 1, 5, 4, 20},  // 92838da2:68
    {6, 0, 0, 0, 3, 2, 0},  // 92838da2:83
    {1, 0, 0, 0, 1, 1, 0},  // 948965e4:72
    {4, 0, 0, 1, 2, 1, 2},  // 97da18f8:13
    {25, 0, 0, 1, 5, 4, 20},  // 97da18f8:50
    {60, 0, 0, 1, 8, 7, 56},  // ace2a1a4:12
    {4, 0, 0, 1, 2, 1, 2},  // d02728d2:13
    {120, 0, 0, 1, 11, 10, 110},  // d02728d2:60
    {2, 0, 0, 1, 2, 1, 2},  // f524d88f:13
  };
  int bad = 0;
  for (const PlnvRow& row : kRows) {
    std::vector<uint8_t> out;
    const auto parts = ConvertPlnv(row.maxp, true, row.nx, row.nz, row.flag, row.maxp & 1, out);
    bool ok = parts.size() == 1 && parts[0].dropped.empty();
    if (ok) {
      // The converter writes n2 = 0 where Remastered's total / n1 is 0 (GetValue reads it as 1), so
      // accept either of the two and require the clamped grid to match.
      ok = false;
      for (uint32_t n1 = row.gx == 1 ? 0u : row.gx; n1 <= row.gx + 1; ++n1) {
        for (uint32_t n2 = row.gz == 1 ? 0u : row.gz; n2 <= row.gz; ++n2) {
          if (std::max(n1, 1u) == row.gx && std::max(n2, 1u) == row.gz &&
              Contains(parts[0].part, PlnvTail(n1, n2, row.shuffle))) {
            ok = true;
          }
        }
      }
    }
    if (!ok) {
      ++bad;
      std::fprintf(stderr, "PLNV row MAXP=%u nx=%u nz=%u flag=%d: want %ux%u shuffler %u\n", row.maxp, row.nx,
                   row.nz, row.flag, row.gx, row.gz, row.shuffle);
      if (parts.size() == 1) {
        Show(parts[0]);
      }
    }
  }
  CHECK_MSG(bad == 0, "every PLNV use in the dumps gets Remastered's grid and shuffler");

  // The exact tails, by hand: MAXP 10 gives 3x3 with 9 cells; MAXP 1 gives 1x0 and a one-cell
  // shuffler (its draw still happens); no flag gives no shuffler; MAXP 4 (a perfect square) 2x1.
  std::vector<uint8_t> out;
  auto parts = ConvertPlnv(10, true, 0, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(3, 3, 9)), "MAXP 10: 3x3, 9 cells");
  parts = ConvertPlnv(1, true, 0, 0, 1, false, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(1, 0, 1)), "MAXP 1: 1x0, one-cell shuffler");
  parts = ConvertPlnv(9, true, 0, 0, 0, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(3, 2, 0)), "MAXP 9, no flag: 3x2, no shuffler");
  parts = ConvertPlnv(4, true, 0, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(2, 1, 2)), "MAXP 4: 2x1, 2 cells (perfect square gives s-1)");
  parts = ConvertPlnv(4, true, 4, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(4, 1, 4)), "nx 4, MAXP 4: 4x1");
  parts = ConvertPlnv(500, true, 0, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(11, 11, 121)),
            "MAXP over 128 is 128: 11x11");
  // Counts that are not zero stand as they are, with Init's shuffler.
  parts = ConvertPlnv(100, true, 4, 3, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(4, 3, 12)), "no zero count: Init's counts and shuffler");
  // A MAXP that is not a constant keeps a zero count dropped, naming it.
  parts = ConvertPlnv(10, false, 0, 0, 1, true, out);
  bool named = false;
  if (parts.size() == 1) {
    for (const std::string& d : parts[0].dropped) {
      named = named || d.find("PLNV with a constant 0 count and a MAXP that is not a constant") != std::string::npos;
    }
  }
  CHECK_MSG(named, "a zero count with a variable MAXP is dropped, naming it");
  // And a nonzero count does not need MAXP.
  parts = ConvertPlnv(10, false, 2, 2, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, PlnvTail(2, 2, 4)), "constant counts ignore MAXP");
}

// CRCV: SEMR(CRCV(centre, axis, 0, n, 1, 2, flag), velocity) in an EMTR; MAXP 3rd.
std::vector<ConvertedPart> ConvertCrcv(uint32_t maxp, bool maxpConst, uint32_t n, int flag, bool axisConst,
                                       std::vector<uint8_t>& out) {
  EffectNode effect;
  auto vec = [](std::vector<uint8_t>& o, float x, float y, float z) {
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(x));
    PutConstant(o, Bits(y));
    PutConstant(o, Bits(z));
  };
  return ConvertGen(
      [&](std::vector<uint8_t>& o) {
        PutProperty(o, "MAXP", 3);
        if (maxpConst) {
          PutConstant(o, maxp);
        } else {
          PutFourCC(o, "ABS_");
          PutConstant(o, maxp);
        }
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "SEMR");
        PutFourCC(o, "CRCV");
        vec(o, 0, 0, 0);
        if (axisConst) {
          vec(o, 0, 1, 0);
        } else {
          PutFourCC(o, "PLOC");
        }
        PutConstant(o, Bits(0.0f));
        PutConstant(o, n);
        PutConstant(o, Bits(1.0f));
        PutConstant(o, Bits(2.0f));
        o.push_back(uint8_t(flag));
        vec(o, 0, 0, 0);
      },
      effect, out);
}

// The bytes PCRC ends with: the count, variance, radius, shuffler size.
std::vector<uint8_t> CrcvTail(uint32_t n, uint32_t shuffle) {
  Retail t;
  t.f("CNST").w(n).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(2.0f)).b(uint8_t(shuffle));
  return t.bytes;
}

bool Dropped(const std::vector<ConvertedPart>& parts, const char* what) {
  if (parts.size() != 1) {
    return false;
  }
  for (const std::string& d : parts[0].dropped) {
    if (d.find(what) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// A PLNV shuffler needs a constant count; CRCV's 0 count means MAXP only as a SEMR's position.
void TestNonConstantCounts() {
  EffectNode effect;
  std::vector<uint8_t> out;
  auto vec = [](std::vector<uint8_t>& o, float x, float y, float z) {
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(x));
    PutConstant(o, Bits(y));
    PutConstant(o, Bits(z));
  };
  auto plnv = [&](int flag) {
    return ConvertGen(
        [&](std::vector<uint8_t>& o) {
          PutProperty(o, "MAXP", 3);
          PutConstant(o, 9);
          PutProperty(o, "EMTR", 3);
          PutFourCC(o, "PLNV");
          vec(o, 0, 0, 0);
          vec(o, 0, 0, 0);
          PutConstant(o, Bits(5.0f));
          PutConstant(o, Bits(2.0f));
          PutConstant(o, Bits(3.0f));
          PutConstant(o, Bits(0.0f));
          PutFourCC(o, "ABS_");
          PutConstant(o, 4);
          PutConstant(o, Bits(1.0f));
          PutConstant(o, 3);
          PutConstant(o, Bits(0.0f));
          o.push_back(uint8_t(flag));
        },
        effect, out);
  };
  CHECK_MSG(Dropped(plnv(1), "PLNV with a shuffler and a count that is not a constant"),
            "PLNV shuffler with a variable count is dropped");
  CHECK_MSG(!Dropped(plnv(0), "PLNV with a shuffler and a count that is not a constant"),
            "without the shuffler flag the variable count stands");
  // CRCV as the velocity (not the position) has no MAXP to resolve a 0 count from.
  auto parts = ConvertGen(
      [&](std::vector<uint8_t>& o) {
        PutProperty(o, "MAXP", 3);
        PutConstant(o, 3);
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "SEMR");
        vec(o, 0, 0, 0);
        PutFourCC(o, "CRCV");
        vec(o, 0, 0, 0);
        vec(o, 0, 1, 0);
        PutConstant(o, Bits(0.0f));
        PutConstant(o, 0);
        PutConstant(o, Bits(1.0f));
        PutConstant(o, Bits(2.0f));
        o.push_back(1);
      },
      effect, out);
  CHECK_MSG(Dropped(parts, "CRCV with a constant 0 count that is not a SEMR's position"),
            "CRCV count 0 outside SEMR's position is dropped");
}

void TestCircleConvert() {
  std::vector<uint8_t> out;
  // A 0 count is the generator's MAXP (clamped 1 to 128), 126ebc84 (3), 900b2baa (4), 3d8d5854 (1024).
  auto parts = ConvertCrcv(3, true, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, CrcvTail(3, 3)),
            "MAXP 3, flag: count 3, 3-cell shuffler");
  parts = ConvertCrcv(60, true, 0, 0, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, CrcvTail(60, 0)), "MAXP 60, no flag: count 60, none");
  parts = ConvertCrcv(1024, true, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, CrcvTail(128, 128)), "MAXP 1024: 128");
  parts = ConvertCrcv(0, true, 0, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, CrcvTail(1, 1)), "MAXP 0: 1");
  // A nonzero count stands, with Init's shuffler; no MAXP needed.
  parts = ConvertCrcv(10, false, 6, 1, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, CrcvTail(6, 6)), "count 6 and the flag: 6 cells");
  parts = ConvertCrcv(10, false, 6, 0, true, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, CrcvTail(6, 0)), "count 6, no flag: no shuffler");
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, Retail().f("PCRC").bytes), "it is the port-only PCRC");
  parts = ConvertCrcv(10, false, 0, 1, true, out);
  CHECK_MSG(Dropped(parts, "CRCV with a constant 0 count and a MAXP that is not a constant"),
            "a zero count with a variable MAXP is dropped, naming it");
  parts = ConvertCrcv(10, true, 4, 1, false, out);
  CHECK_MSG(Dropped(parts, "CRCV with an axis that is not a constant"), "a variable axis is dropped, naming it");
}

// SPEV(0, r, spd, n1, 1, n2, 1, flag) in an EMTR with a MAXP.
std::vector<ConvertedPart> ConvertSpev(uint32_t maxp, bool maxpConst, uint32_t n1, uint32_t n2, int flag,
                                       std::vector<uint8_t>& out) {
  EffectNode effect;
  return ConvertGen(
      [&](std::vector<uint8_t>& o) {
        PutProperty(o, "MAXP", 3);
        if (maxpConst) {
          PutConstant(o, maxp);
        } else {
          PutFourCC(o, "ABS_");
          PutConstant(o, maxp);
        }
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "SPEV");
        PutFourCC(o, "CNST");
        PutConstant(o, Bits(0.0f));
        PutConstant(o, Bits(0.0f));
        PutConstant(o, Bits(0.0f));
        PutConstant(o, Bits(0.1f));
        PutConstant(o, Bits(0.5f));
        PutConstant(o, n1);
        PutConstant(o, Bits(1.0f));
        PutConstant(o, n2);
        PutConstant(o, Bits(1.0f));
        o.push_back(uint8_t(flag));
      },
      effect, out);
}

std::vector<uint8_t> SpevTail(uint32_t n1, uint32_t n2, uint32_t shuffle) {
  Retail t;
  t.f("CNST").w(n1).f("CNST").w(Bits(1.0f)).f("CNST").w(n2).f("CNST").w(Bits(1.0f)).b(uint8_t(shuffle));
  return t.bytes;
}

void TestSphereConvert() {
  std::vector<uint8_t> out;
  // 633c258f and 6e839072: MAXP 10 with both counts 0 and the flag: a 3 x 3 grid, 9 cells.
  auto parts = ConvertSpev(10, true, 0, 0, 1, out);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, SpevTail(3, 3, 9)) &&
                Contains(parts[0].part, Retail().f("PSPV").bytes),
            "SPEV with MAXP 10: PSPV 3 x 3, 9-cell shuffler");
  parts = ConvertSpev(10, true, 0, 0, 0, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, SpevTail(3, 3, 0)), "no flag: no shuffler");
  parts = ConvertSpev(10, true, 4, 2, 1, out);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, SpevTail(4, 2, 8)), "constant counts stand, 8 cells");
  parts = ConvertSpev(10, false, 0, 0, 1, out);
  CHECK_MSG(Dropped(parts, "SPEV with a constant 0 count and a MAXP that is not a constant"),
            "a zero count with a variable MAXP is dropped, naming it");
}

// ASPV(origin, REUL, A, B, nP, vP, range, nA, vA, radius, speed, flag) in an EMTR with a MAXP.
std::vector<ConvertedPart> ConvertAspv(uint32_t maxp, uint32_t nP, uint32_t nA, int flag, std::vector<uint8_t>& out) {
  EffectNode effect;
  return ConvertGen(
      [&](std::vector<uint8_t>& o) {
        PutProperty(o, "MAXP", 3);
        PutConstant(o, maxp);
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "ASPV");
        PutFourCC(o, "CNST");
        PutConstant(o, Bits(0.0f));
        PutConstant(o, Bits(0.0f));
        PutConstant(o, Bits(0.0f));
        PutFourCC(o, "REUL");
        PutConstant(o, 0);
        PutConstant(o, 0);
        PutConstant(o, 0);
        o.push_back(0);
        PutConstant(o, Bits(180.0f));
        PutConstant(o, Bits(0.0f));
        PutConstant(o, nP);
        PutConstant(o, Bits(1.0f));
        PutConstant(o, Bits(360.0f));
        PutConstant(o, nA);
        PutConstant(o, Bits(1.0f));
        PutConstant(o, Bits(2.0f));
        PutConstant(o, Bits(3.0f));
        o.push_back(uint8_t(flag));
      },
      effect, out);
}

void TestAngleSphereVarianceConvert() {
  std::vector<uint8_t> out;
  auto parts = ConvertAspv(10, 4, 2, 1, out);
  Retail tail;
  tail.f("CNST").w(4).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(360.0f)).f("CNST").w(2).f("CNST").w(Bits(1.0f))
      .f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(3.0f)).b(8);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, Retail().f("PASV").bytes) &&
                Contains(parts[0].part, tail.bytes),
            "ASPV converts to PASV, constant counts, 8-cell shuffler, nothing dropped");
  parts = ConvertAspv(10, 0, 0, 0, out);
  Retail grid;
  grid.f("CNST").w(3).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(360.0f)).f("CNST").w(3);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, grid.bytes),
            "ASPV with MAXP 10 and zero counts: a 3 x 3 grid");
}

// LINV(start, end, n, variance, flag) as a SEMR's position.
std::vector<ConvertedPart> ConvertLinv(uint32_t n, int flag, std::vector<uint8_t>& out) {
  EffectNode effect;
  auto vec = [](std::vector<uint8_t>& o, float x, float y, float z) {
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(x));
    PutConstant(o, Bits(y));
    PutConstant(o, Bits(z));
  };
  return ConvertGen(
      [&](std::vector<uint8_t>& o) {
        PutProperty(o, "MAXP", 3);
        PutConstant(o, 5);
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "SEMR");
        PutFourCC(o, "LINV");
        vec(o, 0, 0, 0);
        vec(o, 10, 0, 0);
        PutConstant(o, n);
        PutConstant(o, Bits(1.0f));
        o.push_back(uint8_t(flag));
        vec(o, 0, 0, 0);
      },
      effect, out);
}

void TestLineVarianceConvert() {
  std::vector<uint8_t> out;
  auto parts = ConvertLinv(4, 1, out);
  Retail tail;
  tail.f("CNST").w(4).f("CNST").w(Bits(1.0f)).b(4);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, Retail().f("PLWV").bytes) &&
                Contains(parts[0].part, tail.bytes),
            "LINV converts to PLWV with a 4-cell shuffler, nothing dropped");
  parts = ConvertLinv(0, 1, out);
  Retail zero;
  zero.f("CNST").w(5).f("CNST").w(Bits(1.0f)).b(5);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, zero.bytes),
            "LINV with a zero count in a SEMR's position takes MAXP");
}

// ELPS(centre, size, euler, speed, flag).
std::vector<ConvertedPart> ConvertElps(bool zeroEuler, int flag, std::vector<uint8_t>& out) {
  EffectNode effect;
  auto vec = [](std::vector<uint8_t>& o, float x, float y, float z) {
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(x));
    PutConstant(o, Bits(y));
    PutConstant(o, Bits(z));
  };
  return ConvertGen(
      [&](std::vector<uint8_t>& o) {
        PutProperty(o, "EMTR", 3);
        PutFourCC(o, "ELPS");
        vec(o, 1, 2, 3);
        vec(o, 2, 0.25f, 2);
        vec(o, 0, zeroEuler ? 0.f : 90.f, 0);
        PutConstant(o, Bits(0.025f));
        PutFourCC(o, "CNST");  // the files give the flag as a GetBool: CNST and a byte
        o.push_back(uint8_t(flag));
      },
      effect, out);
}

void TestEllipsoidConvert() {
  std::vector<uint8_t> out;
  auto parts = ConvertElps(true, 1, out);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && Contains(parts[0].part, Retail().f("PELP").bytes),
            "ELPS with a zero euler is the port-only PELP");
  Retail tail;
  tail.f("CNST").w(Bits(0.025f)).b(1);
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, tail.bytes), "PELP ends with the speed and surface-only byte");
  parts = ConvertElps(false, 1, out);
  CHECK_MSG(Dropped(parts, "ELPS with a euler that is not a constant (0, 0, 0)"), "a rotated ELPS is dropped, naming it");
}

}  // namespace

int main() {
  TestNegate();
  TestPlnvCounts();
  TestCircleConvert();
  TestNonConstantCounts();
  TestSphereConvert();
  TestEllipsoidConvert();
  TestAngleSphereVarianceConvert();
  TestLineVarianceConvert();
  if (port_test::Failures() == 0) {
    std::printf("port_elem2_convert: ok\n");
  }
  return PORT_TEST_RESULT();
}

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
