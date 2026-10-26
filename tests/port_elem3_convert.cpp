#include "port_remastered_effect_convert.h"
#include "port_retail_check.h"
#include "port_vfx_opacity.h"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
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



// Element batch 3: T2S_, SCLE, NFOM, DIVD and the swoosh SUST strip.

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

bool Contains(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& what) {
  return std::search(bytes.begin(), bytes.end(), what.begin(), what.end()) != bytes.end();
}

// A root generator holding one swoosh child, whose properties `body` writes.
template <typename Body>
ConvertedPart ConvertSwoosh(Body body) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0000AAAA));
  PutFourCC(out, "SWSH");
  body(out);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  if (!ParseEffect(out.data(), out.size(), effect, error)) {
    std::fprintf(stderr, "  %s\n", error.c_str());
    return {};
  }
  std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  return parts.size() == 2 ? parts[1] : ConvertedPart{};
}

// T2S_ is 1/60 per tick, as a double product narrowed once.
void TestTicksToSeconds() {
  CREPortTicksToSeconds seconds(new CIEConstant(90));  // owns its argument
  float value = 0.f;
  CHECK_MSG(!seconds.GetValue(0, value), "T2S_ is not a terminal");
  CHECK_MSG(Bits(value) == Bits(float(0.016666666666666666 * 90.0)), "T2S_(90) is the double product narrowed");
  CHECK_MSG(std::fabs(value - 1.5f) < 1e-6f, "90 ticks is 1.5 seconds");

  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "SIZE", 3);
        PutFourCC(o, "T2S_");
        PutConstant(o, 90);
      },
      effect, out);
  Retail want;
  want.f("GPSM").f("SIZE").f("T2S_").f("CNST").w(90);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "T2S_(CNST 90) converts to the port-only T2S_");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "T2S_ drops nothing");
}

// SCLE(v, r) is MULT(v, RTOV(r)); both evaluate to (v.x * r, r * v.y, r * v.z).
void TestScale() {
  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "ILOC", 3);
        PutFourCC(o, "SCLE");
        PutFourCC(o, "CNST");
        PutConstant(o, Bits(1.0f));
        PutConstant(o, Bits(2.0f));
        PutConstant(o, Bits(3.0f));
        PutConstant(o, Bits(4.0f));
      },
      effect, out);
  Retail want;
  want.f("GPSM").f("ILOC").f("BSIS").f("MULT");
  RetailVector(want, 1.0f, 2.0f, 3.0f);
  want.f("RTOV").f("CNST").w(Bits(4.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "SCLE(v, r) is MULT(v, RTOV(r))");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "SCLE drops nothing");

  CVEMultiply product(new CVEConstant(new CREConstant(1.0f), new CREConstant(2.0f), new CREConstant(3.0f)),
                      new CVERealToVector(new CREConstant(4.0f)));
  CVector3f v;
  product.GetValue(0, v);
  CHECK_MSG(v.GetX() == 4.0f && v.GetY() == 8.0f && v.GetZ() == 12.0f, "the converted product scales by r");
}

// ELC2's NFOM is one raw byte: CNST and an s32 of it.
void TestNfom() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000BBBB));
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0000BBBB));
  PutFourCC(out, "ELC2");
  PutProperty(out, "SCNT", 1);
  PutConstant(out, 4);
  PutProperty(out, "NFOM", 1);
  out.push_back(1);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "ELC2 with NFOM parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  CHECK_MSG(parts.size() == 2, "root and the ELC2 child");
  if (parts.size() != 2) {
    return;
  }
  Retail want;
  want.f("ELSM").f("PEL2").f("CNST").w(1).f("SCNT").f("CNST").w(4).f("NFOM").f("CNST").w(1).f("_END");
  CHECK_MSG(parts[1].part == want.bytes, "NFOM byte 1 is CNST s32 1");
  CHECK_MSG(parts[1].droppedRetail == 0, "NFOM drops nothing");
}

// DIVD is read as two reals, so what follows it parses.
void TestDivide() {
  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "SIZE", 3);
        PutFourCC(o, "DIVD");
        PutConstant(o, Bits(6.0f));
        PutConstant(o, Bits(3.0f));
        PutProperty(o, "ADV1", 3);
        PutConstant(o, Bits(1.0f));
      },
      effect, out);
  Retail want;
  want.f("GPSM").f("SIZE").f("DIVD").f("CNST").w(Bits(6.0f)).f("CNST").w(Bits(3.0f));
  want.f("ADV1").f("CNST").w(Bits(1.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "DIVD(a, b) parses with two arguments");
}

// A swoosh's SUST(x) is x; elsewhere it stays unconverted.
void TestSwooshSust() {
  const ConvertedPart part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "LRAD", 3);
    PutFourCC(o, "SUST");
    PutConstant(o, Bits(1.0f));
  });
  Retail want;
  want.f("SWSH").f("LRAD").f("CNST").w(Bits(1.0f)).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == want.bytes, "SUST(x) in a swoosh is x");
  CHECK_MSG(part.dropped.empty(), "the swoosh SUST strip drops nothing");

  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "SIZE", 3);
        PutFourCC(o, "SUST");
        PutConstant(o, Bits(1.0f));
      },
      effect, out);
  CHECK_MSG(Dropped(parts, "retail has no element SUST there"), "SUST in a generator stays dropped");
}

// SDTR is read as a leaf, then written as the port-only PSDT.
void TestShutdownTime() {
  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "SIZE", 3);
        PutFourCC(o, "SDTR");
        PutProperty(o, "ADV1", 3);
        PutConstant(o, Bits(1.0f));
      },
      effect, out);
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty(), "SDTR drops nothing");
  const char psdt[] = {'P', 'S', 'D', 'T'};
  CHECK_MSG(!parts.empty() &&
                std::search(parts[0].part.begin(), parts[0].part.end(), psdt, psdt + 4) != parts[0].part.end(),
            "SDTR is written as PSDT");
}

}  // namespace

int main() {
  TestTicksToSeconds();
  TestScale();
  TestNfom();
  TestDivide();
  TestSwooshSust();
  TestShutdownTime();
  if (port_test::Failures() == 0) {
    std::printf("port_elem3_convert: ok\n");
  }
  return PORT_TEST_RESULT();
}

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
