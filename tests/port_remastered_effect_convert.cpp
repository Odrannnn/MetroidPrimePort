#include "port_remastered_effect_convert.h"
#include "port_vfx_opacity.h"
#include "Kyoto/Particles/CPortVfxConstants.hpp"
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
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

std::vector<ConvertedPart> ConvertOne(const std::vector<uint8_t>& data, const EffectConvertIO& io);

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
  // The end of a converted PART: Remastered's default PSLT, the port-only PIRN marker, then _END.
  Retail& end() { return pslt().f("PIRN").f("CNST").w(1).f("_END"); }
  // An embedded child's end: PSLT, PIRN, then PGID with its GUID's first word read little-endian.
  Retail& end(const EffectGuid& id) {
    return pslt().f("PIRN").f("CNST").w(1).f("PGID").f("CNST")
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

CQuaternion EulerQuaternion(const CVector3f& degrees) {
  constexpr float kDegreesToRadians = 0.01745329251994329577f;
  const float x = std::remainder(degrees.GetX(), 360.f) * kDegreesToRadians * 0.5f;
  const float y = std::remainder(degrees.GetY(), 360.f) * kDegreesToRadians * 0.5f;
  const float z = std::remainder(degrees.GetZ(), 360.f) * kDegreesToRadians * 0.5f;
  return CQuaternion(std::cos(z), 0.f, 0.f, std::sin(z)) * CQuaternion(std::cos(y), 0.f, std::sin(y), 0.f) *
         CQuaternion(std::cos(x), std::sin(x), 0.f, 0.f);
}

CMatrix3f EulerMatrix(const CVector3f& degrees) { return EulerQuaternion(degrees).BuildTransform(); }

uint32_t ReadBE32(const std::vector<uint8_t>& bytes, size_t& at) {
  if (at > bytes.size() || bytes.size() - at < 4) {
    at = bytes.size() + 1;
    return 0;
  }
  const uint32_t value = uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
                         uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  at += 4;
  return value;
}

bool ReadStaticReal(const std::vector<uint8_t>& bytes, size_t& at, float& out) {
  if (ReadBE32(bytes, at) != EffectFourCC("CNST")) {
    return false;
  }
  const uint32_t bits = ReadBE32(bytes, at);
  if (at > bytes.size()) {
    return false;
  }
  std::memcpy(&out, &bits, sizeof(out));
  return true;
}

// Read the converter's serialized, static CNST/PQAD subset into the production
// vector-element evaluators. SplitRetailPart separately validates the complete
// PART with the effect reader's typed grammar.
std::unique_ptr<CVectorElement> ReadStaticRotation(const std::vector<uint8_t>& bytes, size_t& at, int depth = 0) {
  if (depth > 16) {
    return {};
  }
  const uint32_t type = ReadBE32(bytes, at);
  if (type == EffectFourCC("CNST")) {
    float x, y, z;
    if (!ReadStaticReal(bytes, at, x) || !ReadStaticReal(bytes, at, y) || !ReadStaticReal(bytes, at, z)) {
      return {};
    }
    return std::unique_ptr<CVectorElement>(new CVEFastConstant(x, y, z));
  }
  if (type != EffectFourCC("PQAD")) {
    return {};
  }
  std::unique_ptr<CVectorElement> first = ReadStaticRotation(bytes, at, depth + 1);
  std::unique_ptr<CVectorElement> second = ReadStaticRotation(bytes, at, depth + 1);
  if (!first || !second) {
    return {};
  }
  return std::unique_ptr<CVectorElement>(
      new CVEOrderedEulerProduct(first.release(), second.release()));
}

std::unique_ptr<CVectorElement> ReadConvertedRotation(const ConvertedPart& part) {
  std::vector<RetailPartProperty> properties;
  std::string error;
  if (!SplitRetailPart(part.part.data(), part.part.size(), properties, error)) {
    CHECK_MSG(false, "serialized rotation PART passes the effect reader");
    return {};
  }
  const auto found = std::find_if(properties.begin(), properties.end(),
                                  [](const RetailPartProperty& property) {
                                    return property.fourcc == EffectFourCC("PMRT");
                                  });
  if (found == properties.end()) {
    CHECK_MSG(false, "serialized rotation PART contains PMRT");
    return {};
  }
  size_t at = 0;
  std::unique_ptr<CVectorElement> rotation = ReadStaticRotation(found->value, at);
  CHECK_MSG(rotation != nullptr && at == found->value.size(),
            "serialized static PMRT tree reads completely into native evaluators");
  return rotation;
}

bool SameMatrix(const CMatrix3f& a, const CMatrix3f& b, float epsilon = 5.e-5f) {
  const float difference[] = {a.Get00() - b.Get00(), a.Get01() - b.Get01(), a.Get02() - b.Get02(),
                              a.Get10() - b.Get10(), a.Get11() - b.Get11(), a.Get12() - b.Get12(),
                              a.Get20() - b.Get20(), a.Get21() - b.Get21(), a.Get22() - b.Get22()};
  return std::all_of(std::begin(difference), std::end(difference),
                     [epsilon](float value) { return std::fabs(value) <= epsilon; });
}

// A root using most of what the converter undoes, with one embedded child.
std::vector<uint8_t> Effect() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "DVVN", 1);  // Remastered only
  out.push_back(4);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 20);
  PutProperty(out, "LTM2", 1);
  PutConstant(out, 30);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(3.0f));
  PutProperty(out, "ROTA", 3);
  PutConstant(out, Bits(5.0f));
  PutProperty(out, "ZBUF", 0);
  out.push_back(1);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "KEYE");
  Put32(out, 1);   // percent
  Put32(out, 0);   // unknown
  out.push_back(0);  // loop
  out.push_back(0);  // unknown
  Put32(out, 9);   // loop end
  Put32(out, 0);   // loop start
  Put32(out, 2);   // keys
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
    Put32(out, Bits(c));
  }
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "MPCB");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.0f));
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(0.0f));
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  PutGuid(out, Legacy(0x1234ABCD));
  PutFourCC(out, "NONE");
  PutProperty(out, "PMDL", 0);
  PutGuid(out, Fresh(0));  // no retail id: dropped
  PutProperty(out, "ICTS", 0);
  PutGuid(out, Legacy(0x0BADF00D));
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0BADF00D));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 5);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(1));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

constexpr EffectGuid kPlasmaNestedLight = {0x1f, 0x70, 0x11, 0xf9, 0x15, 0xec, 0x8b, 0x45,
                                            0xae, 0x13, 0xca, 0x2b, 0xf6, 0x5d, 0x42, 0xf6};

void PutRealKey(std::vector<uint8_t>& out, float first, float second) {
  PutFourCC(out, "KEYE");
  Put32(out, 1); // percent-keyed
  Put32(out, 0);
  out.push_back(0);
  out.push_back(0);
  Put32(out, 9);
  Put32(out, 0);
  Put32(out, 2);
  Put32(out, Bits(first));
  Put32(out, Bits(second));
}

std::vector<uint8_t> NestedLightEffect(const EffectGuid& childId, uint8_t lightType = 2,
                                       uint8_t falloff = 3, bool includeRadii = true,
                                       int inverseExposed = 1) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, childId);
  PutGenerator(out, false);
  PutProperty(out, "LTYP", 0);
  out.push_back(lightType);
  PutProperty(out, "LFOT", 0);
  out.push_back(falloff);
  PutProperty(out, "LCLR", 0);
  PutFourCC(out, "CNST");
  for (float value : {0.921881f, 0.151383f, 0.0102983f, 1.f}) {
    PutConstant(out, Bits(value));
  }
  PutProperty(out, "LINT", 0);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(20.f));
  PutRealKey(out, 0.f, 1.f);
  PutProperty(out, "EMTR", 0);
  PutFourCC(out, "SEMR");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.1f));
  PutConstant(out, Bits(0.f));
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.f));
  PutProperty(out, "PMOP", 0);
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.2f));
  PutConstant(out, Bits(0.f));
  if (includeRadii) {
    PutProperty(out, "LIRD", 0);
    PutConstant(out, Bits(0.25f));
    PutProperty(out, "LORD", 0);
    PutConstant(out, Bits(3.f));
  }
  if (inverseExposed >= 0) {  // the Plasma child authors LIXP 1
    PutProperty(out, "LIXP", 0);
    out.push_back(uint8_t(inverseExposed));
  }
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void TestPlasmaNestedHdrLight() {
  std::string error;
  std::vector<RetailPartProperty> properties;
  const auto parts = ConvertOne(NestedLightEffect(kPlasmaNestedLight), {});
  CHECK_MSG(parts.size() == 2, "the Plasma nested-light fixture converts root and child");
  if (parts.size() == 2) {
    CHECK_MSG(SplitRetailPart(parts[1].part.data(), parts[1].part.size(), properties, error),
              "the marked child with PHDR parses as a PART");
    auto find = [&properties](const char* name) -> const RetailPartProperty* {
      const uint32_t fourcc = uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 |
                              uint32_t(uint8_t(name[2])) << 8 | uint32_t(uint8_t(name[3]));
      for (const RetailPartProperty& property : properties) {
        if (property.fourcc == fourcc) {
          return &property;
        }
      }
      return nullptr;
    };
    const RetailPartProperty* marker = find("PHDR");
    const RetailPartProperty* inner = find("PIR0");
    const RetailPartProperty* outer = find("POR0");
    CHECK_MSG(marker != nullptr && marker->value.size() == 8 && std::memcmp(marker->value.data(), "CNST", 4) == 0 &&
                  marker->value[7] == 1,
              "PHDR opts only the exact child into nested PBR lighting");
    CHECK_MSG(inner != nullptr && inner->value.size() == 8 && std::memcmp(inner->value.data(), "CNST", 4) == 0 &&
                  ((uint32_t(inner->value[4]) << 24 | uint32_t(inner->value[5]) << 16 |
                    uint32_t(inner->value[6]) << 8 | inner->value[7]) == Bits(0.25f)),
              "PIR0 preserves LIRD as the real inner radius");
    CHECK_MSG(outer != nullptr && outer->value.size() == 8 && std::memcmp(outer->value.data(), "CNST", 4) == 0 &&
                  ((uint32_t(outer->value[4]) << 24 | uint32_t(outer->value[5]) << 16 |
                    uint32_t(outer->value[6]) << 8 | outer->value[7]) == Bits(3.f)),
              "POR0 preserves LORD as the real outer radius");
    const RetailPartProperty* intensity = find("LINT");
    CHECK_MSG(intensity != nullptr && intensity->value.size() >= 16 &&
                  std::memcmp(intensity->value.data(), "MULT", 4) == 0,
              "the source 20-times-keyed LINT element remains dynamic");
    CHECK_MSG(find("EMTR") != nullptr && find("PMOP") != nullptr && find("LOFF") == nullptr,
          "the .1 EMTR and .2 PMOP offsets remain non-light particle data");
    CHECK_MSG(find("LTYP") != nullptr && find("LFOT") != nullptr,
              "the legacy GX fallback still receives the mapped retail light properties");
    CHECK_MSG(!parts[0].part.empty() && std::search(parts[0].part.begin(), parts[0].part.end(), "PHDR", "PHDR" + 4) ==
                                          parts[0].part.end(),
          "the unmarked root does not get a duplicate nested-light proxy");
  }

  auto findIn = [](const ConvertedPart& part, const char* name, std::vector<RetailPartProperty>& props) -> const RetailPartProperty* {
    std::string err;
    if (!SplitRetailPart(part.part.data(), part.part.size(), props, err)) {
      return nullptr;
    }
    const uint32_t fourcc = uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 |
                            uint32_t(uint8_t(name[2])) << 8 | uint32_t(uint8_t(name[3]));
    for (const RetailPartProperty& property : props) {
      if (property.fourcc == fourcc) {
        return &property;
      }
    }
    return nullptr;
  };
  auto droppedHas = [](const ConvertedPart& part, const char* text) {
    return std::any_of(part.dropped.begin(), part.dropped.end(),
                       [text](const std::string& d) { return d.find(text) != std::string::npos; });
  };
  {
    // The Plasma child (smooth) writes no PHFO: an absent PHFO is smooth.
    std::vector<RetailPartProperty> props;
    CHECK_MSG(parts.size() == 2 && findIn(parts[1], "PHFO", props) == nullptr,
              "a smooth (LFOT 3) light writes no PHFO");
    CHECK_MSG(parts.size() == 2 && !droppedHas(parts[1], "LIRD") && !droppedHas(parts[1], "LORD"),
              "LIRD and LORD are consumed, not dropped as Remastered only");
  }
  // Any child with a constant point light is converted, with its authored falloff.
  const auto anyChild = ConvertOne(NestedLightEffect(Fresh(7), 2, 2), {});
  CHECK_MSG(anyChild.size() == 2, "a generic child converts");
  if (anyChild.size() == 2) {
    std::vector<RetailPartProperty> props;
    const RetailPartProperty* marker = findIn(anyChild[1], "PHDR", props);
    CHECK_MSG(marker != nullptr && marker->value.size() == 8 && marker->value[7] == 1,
              "a generic point-light child gets PHDR");
    const RetailPartProperty* falloff = findIn(anyChild[1], "PHFO", props);
    CHECK_MSG(falloff != nullptr && falloff->value.size() == 8 &&
                  std::memcmp(falloff->value.data(), "CNST", 4) == 0 && falloff->value[7] == 2,
              "LFOT 2 is written as PHFO 2");
    CHECK_MSG(findIn(anyChild[1], "PIR0", props) != nullptr && findIn(anyChild[1], "POR0", props) != nullptr,
              "a generic child keeps PIR0/POR0");
    CHECK_MSG(!droppedHas(anyChild[1], "LIRD") && !droppedHas(anyChild[1], "LORD"),
              "LIRD/LORD of a generic child are not in dropped");
    const RetailPartProperty* inverse = findIn(anyChild[1], "PLIX", props);
    CHECK_MSG(inverse != nullptr && inverse->value.size() == 8 &&
                  std::memcmp(inverse->value.data(), "CNST", 4) == 0 && inverse->value[7] == 1,
              "LIXP 1 is written as PLIX 1");
    CHECK_MSG(!droppedHas(anyChild[1], "LIXP"), "LIXP of a lit child is consumed, not dropped");
  }
  // LIXP 0 and no LIXP both write no PLIX (CParticleLightDescription's default is off).
  for (int lixp : {0, -1}) {
    const auto plain = ConvertOne(NestedLightEffect(Fresh(7), 2, 2, true, lixp), {});
    std::vector<RetailPartProperty> props;
    CHECK_MSG(plain.size() == 2 && findIn(plain[1], "PHDR", props) != nullptr &&
                  findIn(plain[1], "PLIX", props) == nullptr,
              "a light without LIXP writes no PLIX");
  }
  // A spot light (LTYP 3) stays on the retail path.
  const auto spot = ConvertOne(NestedLightEffect(Fresh(7), 3, 2), {});
  CHECK_MSG(spot.size() == 2 &&
            std::search(spot[1].part.begin(), spot[1].part.end(), "PHDR", "PHDR" + 4) == spot[1].part.end(),
        "a spot light is not converted to an HDR light");
  const auto wrongType = ConvertOne(NestedLightEffect(kPlasmaNestedLight, 1, 3), {});
  CHECK_MSG(wrongType.size() == 2 &&
            std::search(wrongType[1].part.begin(), wrongType[1].part.end(), "PHDR", "PHDR" + 4) ==
                wrongType[1].part.end(),
        "a non-point LTYP does not enable the feature");
  const auto missingRadius = ConvertOne(NestedLightEffect(kPlasmaNestedLight, 2, 3, false), {});
  CHECK_MSG(missingRadius.size() == 2 &&
            std::search(missingRadius[1].part.begin(), missingRadius[1].part.end(), "PHDR", "PHDR" + 4) ==
                missingRadius[1].part.end(),
        "missing validated LIRD/LORD leaves the child unmarked");
}

void TestRetailId() {
  CHECK_MSG(EffectRetailId(Legacy(0x0034CE07)) == 0x0034CE07u, "carried-over id resolves");
  CHECK_MSG(!EffectRetailId(Fresh(0)).has_value(), "a fresh id has no retail id");
}

void TestConvert() {
  const std::vector<uint8_t> data = Effect();
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }

  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(1) ? 0x5EED0001u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 2, "root and child");
  if (parts.size() != 2) {
    return;
  }

  Retail root;
  root.f("GPSM");
  root.f("DVVN").f("CNST").w(4);
  root.f("MAXP").f("CNST").w(20);
  root.f("LTME").f("CNST").w(29);  // LTM2 is one frame longer
  root.f("SIZE").f("MULT").f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(3.0f));
  root.f("ROTA").f("CNST").w(Bits(-5.0f));
  root.f("ZBUF").f("CNST").b(1);
  root.f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(2);
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
    root.w(Bits(c));
  }
  root.f("VEL1").f("CNST").f("CNST").w(Bits(0.0f)).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(0.0f));
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.f("ICTS").f("CNST").w(0x0BADF00D);
  root.end();
  CHECK_MSG(parts[0].root && parts[0].part == root.bytes, "root converts to retail's bytes");
  CHECK_MSG(parts[0].droppedRetail == 1, "PMDL with no retail id is the one retail property dropped");
  CHECK_MSG(parts[0].dropped.size() == 2, "PMDL and IEXP without a material are diagnosed");

  Retail child;
  child.f("GPSM");
  child.f("MAXP").f("CNST").w(5);
  child.f("TEXR").f("CNST").f("CNST").w(0x5EED0001);
  child.end(Legacy(0x0BADF00D));
  CHECK_MSG(!parts[1].root && parts[1].id == Legacy(0x0BADF00D), "child keeps its id");
  CHECK_MSG(parts[1].part == child.bytes, "MTIN becomes the child's TEXR");
  CHECK_MSG(parts[1].droppedRetail == 0, "child drops nothing");

  // What the converter writes reads as retail reads it.
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    CHECK_MSG(SplitRetailPart(part.part.data(), part.part.size(), properties, error), "converted PART reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(root.bytes.data(), root.bytes.size(), properties, error) && properties.size() == 12 &&
            properties[6].fourcc == EffectFourCC("COLR") && properties[6].value.size() == 4 + 22 + 32,
        "retail PART splits into its properties");
}

// The splitter refuses what retail's reader would not take.
void TestSplitRejects() {
  Retail bad;
  bad.f("GPSM").f("SIZE").f("RADD").f("_END");
  std::vector<RetailPartProperty> properties;
  std::string error;
  CHECK_MSG(!SplitRetailPart(bad.bytes.data(), bad.bytes.size(), properties, error) && !error.empty(),
            "unknown element refused");
  Retail spawn;
  spawn.f("GPSM").f("KSSM").f("CNST").w(0).w(0).w(10).w(0).w(1).w(3).w(1).w(0xAABBCCDD).w(0).w(0).w(0).f("_END");
  CHECK_MSG(SplitRetailPart(spawn.bytes.data(), spawn.bytes.size(), properties, error) && properties.size() == 1,
            "KSSM spawn table reads");
}

// An effect with no children has its root flag clear but is still the
// effect's own PART; a colour's keys stored as halves come out as floats; a
// spawn table counts as a retail property left out.
void TestSingleNode() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "KEYE");
  Put32(out, 1);
  Put32(out, 0);
  out.push_back(0);
  out.push_back(0);
  Put32(out, 9);
  Put32(out, 0);
  Put32(out, 1);
  for (uint16_t half : {0x3c00, 0x3800, 0x0000, 0xbc00}) {  // 1, 0.5, 0, -1
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "NONE");
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "single-node effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  CHECK_MSG(parts.size() == 1 && parts[0].root, "the only GPSM is the root");
  Retail want;
  want.f("GPSM").f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(1);
  want.w(Bits(1.0f)).w(Bits(0.5f)).w(Bits(0.0f)).w(Bits(-1.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "half colour keys widen to floats");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "an empty KSSM is nothing left out");
}

// Remastered's new elements: MPCB(ADD_(v, MPAC)) becomes BSIS with the port-only PMAC, MPRD becomes RAND
// (and a random LTM2 comes down by one at both ends), DFCP and ANCR are kept as
// the port's elements; a vector of nested constants is not read as an id.
void TestMappedElements() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "SEMR");
  PutFourCC(out, "MPCB");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutFourCC(out, "MPCB");
  PutFourCC(out, "ADD_");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutFourCC(out, "MPAC");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, Bits(720.0f));
  PutConstant(out, Bits(720.0f));
  PutConstant(out, Bits(0.1f));
  PutProperty(out, "LTM2", 3);
  PutFourCC(out, "MPRD");
  PutConstant(out, 17);
  PutConstant(out, 33);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(2.0f));
  PutFourCC(out, "DFCP");
  PutConstant(out, Bits(2.0f));
  PutConstant(out, 0);
  PutProperty(out, "POFS", 0);
  PutFourCC(out, "ANCR");
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
  PutConstant(out, Bits(360.0f));
  PutConstant(out, Bits(360.0f));
  PutConstant(out, Bits(0.5f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "mapped-element effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("SEMR").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  // MPCB's BSIS and the SEMR boundary's turn cancel out.
  want.f("ADD_").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  want.f("PMAC").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(720.0f)).f("CNST").w(Bits(720.0f)).f("CNST").w(Bits(0.1f));
  want.f("LTME").f("RAND").f("CNST").w(16).f("CNST").w(32);
  want.f("SIZE").f("MULT").f("CNST").w(Bits(2.0f)).f("DFCP").f("CNST").w(Bits(2.0f)).f("CNST").w(0);
  // The port's ANCR runs in Remastered's space, so the property boundary turns it.
  want.f("POFS").f("BSIS").f("ANCR").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  want.f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(0.5f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "new elements map onto retail's");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
    std::string got, exp;
    for (uint8_t c : parts[0].part) got += (c >= 32 && c < 127) ? char(c) : '.';
    for (uint8_t c : want.bytes) exp += (c >= 32 && c < 127) ? char(c) : '.';
    std::fprintf(stderr, "  got  %s\n  want %s\n", got.c_str(), exp.c_str());
  }
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "DFCP is kept, not approximated");
}

// CREL(LTHN(a, b), A, B) is retail's CLTN(a, b, B, A); MPCN, ANCM (with a REUL) and MPAC become the
// port-only PMCN, PANM and PMAC. A CREL on another condition and an ANCM on another rotation are
// dropped with the reason.
void TestPortVectors() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "CREL");
  PutFourCC(out, "LTHN");
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(3.0f));
  PutConstant(out, Bits(4.0f));
  PutProperty(out, "POFS", 0);
  PutFourCC(out, "MPCN");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.0f));
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(0.0f));
  PutConstant(out, Bits(0.5f));
  PutProperty(out, "PSOV", 0);
  PutFourCC(out, "ANCM");
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, Bits(90.0f));
  PutConstant(out, 0);
  out.push_back(0);
  PutConstant(out, Bits(10.0f));
  PutConstant(out, Bits(20.0f));
  PutConstant(out, Bits(2.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "port-vector effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("SIZE").f("CLTN").f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(4.0f)).f("CNST").w(Bits(3.0f));
  want.f("POFS").f("BSIS").f("PMCN").f("CNST").f("CNST").w(Bits(0.0f)).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(0.0f));
  want.f("CNST").w(Bits(0.5f));
  want.f("PSOV").f("PANM").f("CNST").w(0).f("CNST").w(Bits(90.0f)).f("CNST").w(0);
  want.f("CNST").w(Bits(10.0f)).f("CNST").w(Bits(20.0f)).f("CNST").w(Bits(2.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "CREL, MPCN and ANCM map onto CLTN, PMCN and PANM");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    std::string got;
    for (uint8_t c : parts[0].part) {
      got += (c >= 32 && c < 127) ? char(c) : '.';
    }
    std::string exp;
    for (uint8_t c : want.bytes) exp += (c >= 32 && c < 127) ? char(c) : '.';
    std::fprintf(stderr, "  got  %s\n  want %s\n", got.c_str(), exp.c_str());
  }
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "nothing dropped");

  // Another condition and another rotation.
  std::vector<uint8_t> bad(0x3c, 0);
  std::memcpy(bad.data(), "RFRM", 4);
  std::memcpy(bad.data() + 0x14, "GENP", 4);
  PutGenerator(bad, false);
  PutProperty(bad, "SIZE", 3);
  PutFourCC(bad, "CREL");
  PutFourCC(bad, "EQAL");
  PutConstant(bad, Bits(1.0f));
  PutConstant(bad, Bits(2.0f));
  PutConstant(bad, Bits(3.0f));
  PutConstant(bad, Bits(4.0f));
  PutProperty(bad, "PSOV", 0);
  PutFourCC(bad, "ANCM");
  PutFourCC(bad, "EXTR");
  PutConstant(bad, Bits(10.0f));
  PutConstant(bad, Bits(20.0f));
  PutConstant(bad, Bits(2.0f));
  PutTexr(bad, 0x1234ABCD);
  PutProperty(bad, "_END", 4);
  EffectNode badEffect;
  CHECK_MSG(ParseEffect(bad.data(), bad.size(), badEffect, error), "bad port-vector effect parses");
  const std::vector<ConvertedPart> badParts = ConvertEffect(badEffect, bad.data(), {});
  bool crel = false;
  bool ancm = false;
  if (badParts.size() == 1) {
    for (const std::string& d : badParts[0].dropped) {
      crel = crel || (d.find("CREL") != std::string::npos && d.find("EQAL") != std::string::npos);
      ancm = ancm || (d.find("PSOV") != std::string::npos && d.find("not REUL") != std::string::npos);
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
  }
  CHECK_MSG(crel, "a CREL on EQAL is dropped, naming it");
  CHECK_MSG(ancm, "an ANCM on a non-REUL rotation is dropped, naming it");
}

// PLNE and PLNV become the port's plane emitters, EMRV the port-only PEMV (under BSIS, which the property boundary cancels) under XFMD 2 and 5,
// a constant zero under 3, and is dropped under 4. A PLNE with a euler that is not a constant is dropped.
void TestPlaneElements() {
  auto vec = [](std::vector<uint8_t>& o, float x, float y, float z) {
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(x));
    PutConstant(o, Bits(y));
    PutConstant(o, Bits(z));
  };
  auto build = [&](uint32_t xfmd, bool plnv, bool badEuler, bool constPofs = false) {
    std::vector<uint8_t> out(0x3c, 0);
    std::memcpy(out.data(), "RFRM", 4);
    std::memcpy(out.data() + 0x14, "GENP", 4);
    PutGenerator(out, false);
    PutProperty(out, "XFMD", 0);
    PutConstant(out, xfmd);
    PutProperty(out, "EMTR", 3);
    PutFourCC(out, plnv ? "PLNV" : "PLNE");
    vec(out, 0, 0, 0);
    if (badEuler) {
      PutFourCC(out, "EMRV");
    } else {
      vec(out, 10, 20, 30);
    }
    if (plnv) {
      PutConstant(out, Bits(5.0f));
      PutConstant(out, Bits(2.0f));
      PutConstant(out, Bits(3.0f));
      PutConstant(out, Bits(15.0f));
      PutConstant(out, 4);
      PutConstant(out, 1);
      PutConstant(out, 3);
      PutConstant(out, 0);
      out.push_back(1);
    } else {
      vec(out, 0, 0, 4);
      PutConstant(out, Bits(2.0f));
      PutConstant(out, Bits(3.0f));
      PutConstant(out, Bits(15.0f));
    }
    PutProperty(out, "POFS", 0);
    if (constPofs) {
      vec(out, 1, 2, 3);
      PutProperty(out, "ILOC", 0);
      PutFourCC(out, "EMRV");
    } else {
      PutFourCC(out, "EMRV");
    }
    PutProperty(out, "_END", 4);
    return out;
  };
  auto convert = [&](const std::vector<uint8_t>& out, const std::string& ok) {
    EffectNode effect;
    std::string error;
    CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), ok + " parses");
    return ConvertEffect(effect, out.data(), {});
  };
  auto has = [](const ConvertedPart& part, const char* fourcc) {
    return std::search(part.part.begin(), part.part.end(), fourcc, fourcc + 4) != part.part.end();
  };
  auto dropped = [](const ConvertedPart& part, const char* what) {
    for (const std::string& d : part.dropped) {
      if (d.find(what) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  for (int plnv = 0; plnv < 2; ++plnv) {
    const char* name = plnv ? "PLNV" : "PLNE";
    const std::vector<ConvertedPart> parts = convert(build(2, plnv != 0, false), name);
    CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), std::string(name) + " and EMRV under XFMD 2 drop nothing");
    CHECK_MSG(parts.size() == 1 && has(parts[0], name) && has(parts[0], "PEMV"),
              std::string(name) + " is kept and EMRV becomes PEMV (POFS's turn cancels the BSIS)");
  }
  std::vector<ConvertedPart> parts = convert(build(3, false, false), "XFMD 3");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && !has(parts[0], "PEMV"), "EMRV under XFMD 3 is a constant zero");
  parts = convert(build(4, false, false), "XFMD 4");
  CHECK_MSG(parts.size() == 1 && dropped(parts[0], "EMRV under XFMD 4"), "EMRV under XFMD 4 is dropped, naming it");
  parts = convert(build(5, false, false, true), "XFMD 5 constant POFS");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() && has(parts[0], "PEMV"), "EMRV under XFMD 5 with a constant POFS is PEMV mode 2");
  parts = convert(build(5, false, false, false), "XFMD 5 variable POFS");
  CHECK_MSG(parts.size() == 1 && dropped(parts[0], "EMRV under XFMD 5"), "EMRV under XFMD 5 with a variable POFS is dropped, naming it");
  parts = convert(build(2, true, false), "zero-count PLNV");
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "a flagged PLNV with a zero count is exact, not approximated");
  parts = convert(build(2, false, true), "bad euler");
  CHECK_MSG(parts.size() == 1 && dropped(parts[0], "PLNE with a euler"), "a PLNE with a variable euler is dropped, naming it");
}

// ABS_, COSE, RSGN, IRSN (the port-only PIRS) and an EMTR CODE are converted, not dropped.
void TestScalarElements() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "CODE");
  PutProperty(out, "MAXP", 3);
  PutFourCC(out, "ABS_");
  PutConstant(out, uint32_t(-5));
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "ADD_");
  PutFourCC(out, "ABS_");
  PutFourCC(out, "RSGN");
  PutConstant(out, Bits(-2.0f));
  PutFourCC(out, "COSE");
  PutConstant(out, Bits(10.0f));
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(30.0f));
  PutProperty(out, "LINT", 3);
  PutFourCC(out, "IRSN");
  PutConstant(out, Bits(4.0f));
  PutProperty(out, "POFS", 0);
  PutFourCC(out, "ABS_");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(-1.0f));
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(-3.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "scalar-element effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("CODE");
  want.f("MAXP").f("ABS_").f("CNST").w(uint32_t(-5));
  want.f("SIZE").f("ADD_").f("ABS_").f("RSGN").f("CNST").w(Bits(-2.0f));
  want.f("COSE").f("CNST").w(Bits(10.0f)).f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(30.0f));
  // The slot is the generator's first per-particle one.
  want.f("LINT").f("PIRS").w(0).f("CNST").w(Bits(4.0f));
  want.f("POFS").f("BSIS").f("ABS_").f("CNST").f("CNST").w(Bits(-1.0f)).f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(-3.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "ABS_, COSE, RSGN, IRSN and CODE are converted");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
    std::string got;
    for (uint8_t c : parts[0].part) {
      got += (c >= 32 && c < 127) ? char(c) : '.';
    }
    std::fprintf(stderr, "  got  %s\n", got.c_str());
  }
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "nothing dropped");
}

// MPXV(index, fallback) is converted as the port-only PMXV and kept by the retail reader.
void TestExternalValue() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "ADV2", 3);
  PutFourCC(out, "MPXV");
  PutConstant(out, 1);
  PutConstant(out, Bits(0.1f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "MPXV effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("ADV2").f("PMXV").f("CNST").w(1).f("CNST").w(Bits(0.1f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "MPXV becomes PMXV");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "MPXV is not dropped");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    std::string got;
    for (uint8_t c : parts[0].part) {
      got += (c >= 32 && c < 127) ? char(c) : '.';
    }
    std::fprintf(stderr, "  got  %s\n", got.c_str());
  }
}

// Properties retail reads as something else than Remastered wrote are left out.
void TestRejects() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "RADD");  // Remastered's, not retail's
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "LFOT", 0);
  out.push_back(3);
  PutProperty(out, "LTYP", 0);
  out.push_back(1);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "reject effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "RADD in SIZE is dropped");
  Retail root;
  root.f("GPSM").f("LFOT").f("CNST").w(3).f("LTYP").f("CNST").w(2);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == root.bytes, "LFOT byte 3 is retail 3, LTYP byte 1 is retail 2");
}
}  // namespace

void PutReul(std::vector<uint8_t>& out, float x) {
  PutFourCC(out, "REUL");
  PutConstant(out, Bits(x));
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
}

// DIVD, NORM, CMPS, EXTT and EXTS (GPSM SVR0/SVV0, PMSC, VEL1) keep their Remastered element
// as the port-only one of the same FourCC.
void TestSystemVectors() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "DIVD");
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(4.0f));
  PutProperty(out, "POFS", 3);
  PutFourCC(out, "NORM");
  PutFourCC(out, "CMPS");
  PutProperty(out, "PSIV", 3);
  PutFourCC(out, "SUB_");
  PutFourCC(out, "EXTT");
  PutFourCC(out, "EXTS");
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "system-vector effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("SIZE").f("DIVD").f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(4.0f));
  want.f("POFS").f("BSIS").f("NORM").f("BSIS").f("CMPS");
  want.f("PSIV").f("BSIS").f("SUB_").f("BSIS").f("EXTT").f("BSSC").f("EXTS");
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "DIVD, NORM, CMPS, EXTT and EXTS carry over");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
  }
}

// Remastered's particle space is X left, Y up, Z forward: a mod vector's CNST
// becomes (-x, z, y), MPCB(v) is B(v), MCBS(v) the scale form (x, z, y), and a
// vector property gets B at the boundary, folded into a constant.
void TestParticleSpace() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "PMSC", 3);
  PutFourCC(out, "MCBS");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(3.0f));
  PutProperty(out, "POFS", 3);
  PutFourCC(out, "MPCB");
  PutFourCC(out, "PLOC");
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(3.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "particle-space effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  // MCBS then the boundary BSSC: P(P(v)) is v.
  want.f("PMSC").f("CNST").f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(3.0f));
  // MPCB(BSIS(PLOC)) then the boundary BSIS: all three cancel to one.
  want.f("POFS").f("BSIS").f("PLOC");
  want.f("VEL1").f("CNST").f("CNST").w(Bits(-1.0f)).f("CNST").w(Bits(3.0f)).f("CNST").w(Bits(2.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "Remastered vectors convert to the port's space");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "particle-space conversion drops nothing");
}

// Remastered's shapes with no retail element of their own: ASPR (its REUL as
// three angles) and RNDV become the port-only elements, and a GRAD colour
// gradient over the life a PGRD.
void TestShapes() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "ASPR");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutReul(out, -90.0f);
  PutConstant(out, Bits(360.0f));
  PutConstant(out, Bits(180.0f));
  PutConstant(out, Bits(0.25f));
  PutConstant(out, Bits(0.1f));
  PutProperty(out, "PSIV", 3);
  PutFourCC(out, "RNDV");
  PutConstant(out, Bits(0.5f));
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "GRAD");
  out.push_back(2);
  for (uint16_t half : {0x3c00, 0x0000, 0x0000, 0x3c00}) {  // red, opaque, at 0
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  Put32(out, Bits(0.0f));
  for (uint16_t half : {0x0000, 0x0000, 0x3c00, 0x0000}) {  // blue, clear, at 1
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  Put32(out, Bits(1.0f));
  PutFourCC(out, "ILPT");
  PutConstant(out, 100);
  out.push_back(0);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "shapes effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("ASPR").f("CNST").f("CNST").w(0x80000000u).f("CNST").w(0).f("CNST").w(0);
  want.f("CNST").w(Bits(-90.0f)).f("CNST").w(0).f("CNST").w(0);
  want.f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(180.0f)).f("CNST").w(Bits(0.25f)).f("CNST").w(Bits(0.1f));
  want.f("PSIV").f("BSIS").f("RNDV").f("CNST").w(Bits(0.5f));
  // A PART's COLR (tag 3) reads its particle's LTM2, one above the port's lifetime: PLPT offset 1.
  want.f("COLR").f("PGRD").w(2);
  want.w(Bits(1.0f)).w(Bits(0.0f)).w(Bits(0.0f)).w(Bits(1.0f)).w(Bits(0.0f));
  want.w(Bits(0.0f)).w(Bits(0.0f)).w(Bits(1.0f)).w(Bits(0.0f)).w(Bits(1.0f));
  want.f("PLPT").w(1).f("CNST").w(100).b(0);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "ASPR, RNDV and GRAD map onto retail's");
  if (parts.size() == 1) {
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
    std::vector<RetailPartProperty> split;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), split, error), "shapes PART reads as retail");
    CHECK_MSG(parts[0].approximated.empty(), "a rotated cone is exact");
  }
}

// A gradient over a period in frames keeps it: PGRD runs Remastered's CCEGradient on the age.
void TestGradientFrames() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "LTM2", 3);
  PutConstant(out, 20);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "GRAD");
  out.push_back(2);
  for (int stop = 0; stop < 2; ++stop) {
    for (int c = 0; c < 4; ++c) {
      const uint16_t half = stop == 0 ? 0x3c00 : 0x0000;
      out.push_back(uint8_t(half));
      out.push_back(uint8_t(half >> 8));
    }
    Put32(out, Bits(float(stop)));
  }
  PutConstant(out, 20);
  out.push_back(0);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "frame gradient parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM").f("LTME").f("CNST").w(19).f("COLR").f("PGRD").w(2);
  want.w(Bits(1.0f)).w(Bits(1.0f)).w(Bits(1.0f)).w(Bits(1.0f)).w(Bits(0.0f));
  want.w(Bits(0.0f)).w(Bits(0.0f)).w(Bits(0.0f)).w(Bits(0.0f)).w(Bits(1.0f));
  want.f("CNST").w(20).b(0);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "a gradient over the life in frames ends with it");
}

// Swoosh and electric children come out as retail SWHC and ELSC under their
// own ids: SBDM becomes the swoosh's AALP, MTIN its TEXR, a zero id NONE. An
// ELC2 has no texture, so its MTIN is left out.
void TestSwooshElectric() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 3);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 3);
  PutGuid(out, Legacy(0x0000AAAA));
  PutFourCC(out, "SWSH");
  PutProperty(out, "LENG", 1);
  PutConstant(out, 8);
  PutProperty(out, "SBDM", 1);
  PutConstant(out, 1);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(2));
  PutProperty(out, "_END", 4);
  PutGuid(out, Fresh(3));
  PutFourCC(out, "ELSM");
  PutProperty(out, "LWD1", 3);
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutProperty(out, "GPSM", 0);
  PutGuid(out, EffectGuid{});
  PutProperty(out, "_END", 4);
  PutGuid(out, Fresh(4));
  PutFourCC(out, "ELC2");
  PutProperty(out, "SCNT", 1);
  PutConstant(out, 4);
  PutProperty(out, "SIDE", 1);
  PutConstant(out, 3);
  PutProperty(out, "RADS", 3);
  PutConstant(out, Bits(0.5f));
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(2));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "swoosh and electric effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(2) ? 0x5EED0002u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), io);
  CHECK_MSG(parts.size() == 4, "root and three children");
  if (parts.size() != 4) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }

  Retail root;
  root.f("GPSM").f("MAXP").f("CNST").w(3).f("SSWH").f("CNST").w(0x0000AAAA);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts[0].type == EffectFourCC("PART") && parts[0].part == root.bytes, "root references the swoosh by id");

  Retail swoosh;
  swoosh.f("SWSH").f("LENG").f("CNST").w(8).f("AALP").f("CNST").b(1);
  swoosh.f("TEXR").f("CNST").f("CNST").w(0x5EED0002).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(parts[1].type == EffectFourCC("SWHC") && parts[1].id == Legacy(0x0000AAAA), "swoosh child is an SWHC");
  CHECK_MSG(parts[1].part == swoosh.bytes, "SBDM becomes AALP and MTIN the swoosh's TEXR");
  CHECK_MSG(parts[1].droppedRetail == 0, "swoosh drops nothing");

  Retail electric;
  electric.f("ELSM").f("LWD1").f("CNST").w(Bits(2.0f)).f("SSWH").f("CNST").w(0x0000AAAA).f("GPSM").f("NONE").f("_END");
  CHECK_MSG(parts[2].type == EffectFourCC("ELSC") && parts[2].part == electric.bytes, "ELSM child is an ELSC");

  // An ELC2 child is an ELSC stream led by the port-only PEL2 marker; its fields follow.
  Retail elc2;
  elc2.f("ELSM").f("PEL2").f("CNST").w(1).f("SCNT").f("CNST").w(4);
  elc2.f("SIDE").f("CNST").w(3).f("RADS").f("CNST").w(Bits(0.5f)).f("_END");
  CHECK_MSG(parts[3].type == EffectFourCC("ELSC") && parts[3].part == elc2.bytes, "ELC2 child is an ELSC with PEL2");
  CHECK_MSG(parts[3].droppedRetail == 0, "ELC2 drops no retail property");

  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    CHECK_MSG(SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error),
              "converted child reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(!SplitRetailEffect(EffectFourCC("SWHC"), root.bytes.data(), root.bytes.size(), properties, error),
            "a PART is not an SWHC");
  Retail wrong;
  wrong.f("SWSH").f("MAXP").f("CNST").w(3).f("_END");
  CHECK_MSG(!SplitRetailEffect(EffectFourCC("SWHC"), wrong.bytes.data(), wrong.bytes.size(), properties, error),
            "a PART property in a swoosh refused");
  CHECK_MSG(SplitRetailEffect(EffectFourCC("ELSC"), electric.bytes.data(), electric.bytes.size(), properties, error) &&
                properties.size() == 3,
            "retail ELSC splits into its properties");
}

// A root generator holding one swoosh child, whose properties `body` writes;
// returns the converted swoosh.
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

// A swoosh has no per-particle variables: PAP9, PAP0..3 reads and ADV9/IAVn are dropped.
void TestSwooshParticleVars() {
  const auto dropped = [](const ConvertedPart& part, const char* what) {
    for (const std::string& d : part.dropped) {
      if (d.find(what) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  ConvertedPart part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "LRAD", 3);
    PutFourCC(o, "PAP9");
  });
  CHECK_MSG(dropped(part, "PAP9 in a swoosh"), "PAP9 in a swoosh is dropped");
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "ADV9", 3);
    PutConstant(o, Bits(1.0f));
  });
  CHECK_MSG(dropped(part, "ADV9: a swoosh has no per-particle variables"), "ADV9 in a swoosh is dropped");
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "IAV1", 3);
    PutConstant(o, 1);
  });
  CHECK_MSG(dropped(part, "IAV1: a swoosh has no per-particle variables"), "IAV1 in a swoosh is dropped");
}

// KPIN(RAZY(EMRV, ROTV((0, 1, 0), RLCL), fallback)). `fallback` writes RAZY's third argument.
template <typename Fallback>
void PutRazyProt(std::vector<uint8_t>& out, Fallback fallback) {
  PutProperty(out, "PROT", 3);
  PutFourCC(out, "KPIN");
  PutFourCC(out, "RAZY");
  PutFourCC(out, "EMRV");
  PutFourCC(out, "ROTV");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, Bits(1.0f));
  PutConstant(out, 0);
  PutFourCC(out, "RLCL");
  fallback(out);
}

void PutZeroReul(std::vector<uint8_t>& out) {
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
}

// A swoosh PROT aligning each point to the system's motion becomes the port-only PRAZ
// (the axis) and PRAF (fallback to the orientation, else identity); a view-oriented
// swoosh (ORNT 1, SIDE 2) faces the camera instead and drops PROT silently.
void TestSwooshRazyProt() {
  ConvertedPart part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "LENG", 1);
    PutConstant(o, 8);
    PutRazyProt(o, PutZeroReul);
  });
  Retail want;
  want.f("SWSH").f("LENG").f("CNST").w(8);
  want.f("PRAZ").f("CNST").f("CNST").w(0x80000000u).f("CNST").w(0).f("CNST").w(Bits(1.0f));
  want.f("PRAF").f("CNST").w(0).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == want.bytes, "a RAZY PROT becomes PRAZ with an identity PRAF");
  CHECK_MSG(part.dropped.empty() && part.droppedRetail == 0, "a RAZY PROT drops nothing");

  part = ConvertSwoosh([](auto& o) { PutRazyProt(o, [](auto& o) { PutFourCC(o, "RLCL"); }); });
  Retail local;
  local.f("SWSH").f("PRAZ").f("CNST").f("CNST").w(0x80000000u).f("CNST").w(0).f("CNST").w(Bits(1.0f));
  local.f("PRAF").f("CNST").w(1).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == local.bytes, "an RLCL fallback becomes PRAF 1");

  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "ORNT", 0);
    PutConstant(o, 1);
    PutProperty(o, "SIDE", 0);
    PutConstant(o, 2);
    PutRazyProt(o, PutZeroReul);
  });
  CHECK_MSG(part.dropped.empty(), "a view-oriented swoosh drops PROT silently");
  std::vector<RetailPartProperty> properties;
  std::string error;
  bool praz = false;
  if (SplitRetailEffect(EffectFourCC("SWHC"), part.part.data(), part.part.size(), properties, error)) {
    for (const RetailPartProperty& property : properties) {
      praz = praz || property.fourcc == EffectFourCC("PRAZ");
    }
  }
  CHECK_MSG(!properties.empty() && !praz, "a view-oriented swoosh writes no PRAZ");

  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 3);
    PutRazyProt(o, PutZeroReul);
  });
  bool xfmd = false;
  for (const std::string& reason : part.dropped) {
    xfmd = xfmd || reason == "PROT: RAZY under XFMD 3";
  }
  CHECK_MSG(xfmd, "a RAZY PROT under XFMD 3 is dropped");
}

void PutZReul(std::vector<uint8_t>& out) {
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, 0);
}

// A swoosh PROT turning about Z only becomes the port-only PROZ: flags (bit 0 kept from
// birth, bit 1 from the orientation) and z with KPIN and SUST taken out.
void TestSwooshZProt() {
  ConvertedPart part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "PROT", 3);
    PutFourCC(o, "KPIN");
    PutFourCC(o, "RADD");
    PutFourCC(o, "EXTR");
    PutZReul(o);
    PutConstant(o, Bits(90.0f));
    o.push_back(0);
  });
  Retail want;
  want.f("SWSH").f("PROZ").f("CNST").w(3).f("CNST").w(Bits(90.0f)).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == want.bytes, "KPIN(RADD(EXTR, REUL(0, 0, 90))) becomes PROZ 3");
  CHECK_MSG(part.dropped.empty(), "a Z PROT drops nothing");

  // REUL(0, 0, MULT(ADD_(KPIN(SUST(SINE(30, 100, 0))), 0), -1)): kept, as its only varying part is.
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "PROT", 3);
    PutZReul(o);
    PutFourCC(o, "MULT");
    PutFourCC(o, "ADD_");
    PutFourCC(o, "KPIN");
    PutFourCC(o, "SUST");
    PutFourCC(o, "SINE");
    PutConstant(o, Bits(30.0f));
    PutConstant(o, Bits(100.0f));
    PutConstant(o, 0);
    PutConstant(o, 0);
    PutConstant(o, Bits(-1.0f));
    o.push_back(0);
  });
  Retail sine;
  sine.f("SWSH").f("PROZ").f("CNST").w(1).f("MULT").f("ADD_").f("SINE").f("CNST").w(Bits(30.0f));
  sine.f("CNST").w(Bits(100.0f)).f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(-1.0f));
  sine.f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == sine.bytes, "a z kept under KPIN becomes PROZ 1");

  // REUL(0, 0, MULT(SUST(RAND(0, 360)), -1)): a new angle every frame.
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "PROT", 3);
    PutZReul(o);
    PutFourCC(o, "MULT");
    PutFourCC(o, "SUST");
    PutFourCC(o, "RAND");
    PutConstant(o, 0);
    PutConstant(o, Bits(360.0f));
    PutConstant(o, Bits(-1.0f));
    o.push_back(0);
  });
  Retail rand;
  rand.f("SWSH").f("PROZ").f("CNST").w(0).f("MULT").f("RAND").f("CNST").w(0).f("CNST").w(Bits(360.0f));
  rand.f("CNST").w(Bits(-1.0f)).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == rand.bytes, "a per-frame z becomes PROZ 0");

  // A SINE outside SUST runs on the point's age, which the port does not evaluate on.
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "PROT", 3);
    PutZReul(o);
    PutFourCC(o, "SINE");
    PutConstant(o, Bits(30.0f));
    PutConstant(o, Bits(100.0f));
    PutConstant(o, 0);
    o.push_back(0);
  });
  CHECK_MSG(part.dropped.size() == 1 && part.dropped[0] == "PROT: z SINE on the point's age",
            "a z SINE on the point's age is dropped");

  // XFMD 3 is written for the swoosh, and a plain REUL turns its points there too.
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 3);
    PutProperty(o, "PROT", 3);
    PutZReul(o);
    PutConstant(o, Bits(90.0f));
    o.push_back(0);
  });
  Retail follow;
  follow.f("SWSH").f("XFMD").f("CNST").w(3).f("PROZ").f("CNST").w(0).f("CNST").w(Bits(90.0f));
  follow.f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == follow.bytes, "XFMD 3 with REUL(0, 0, 90) becomes XFMD 3 and PROZ 0");
  CHECK_MSG(part.dropped.empty(), "XFMD 3 with a plain Z PROT drops nothing");

  // XFMD 5 keeps local and global apart as retail: no XFMD, the same PROZ.
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 5);
    PutProperty(o, "PROT", 3);
    PutZReul(o);
    PutConstant(o, Bits(90.0f));
    o.push_back(0);
  });
  Retail split;
  split.f("SWSH").f("PROZ").f("CNST").w(0).f("CNST").w(Bits(90.0f)).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == split.bytes && part.dropped.empty(), "XFMD 5 with REUL(0, 0, 90) becomes PROZ 0");

  // EXTR under XFMD 3 is the emitter's turn, which XFMD 3 draws again globally.
  part = ConvertSwoosh([](auto& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 3);
    PutProperty(o, "PROT", 3);
    PutFourCC(o, "RADD");
    PutFourCC(o, "EXTR");
    PutZReul(o);
    PutConstant(o, Bits(90.0f));
    o.push_back(0);
  });
  CHECK_MSG(part.dropped.size() == 1 && part.dropped[0] == "PROT: EXTR or INTR under XFMD 3",
            "RADD(EXTR, REUL) under XFMD 3 is dropped");

  // XFMD 4 with the identity EMVR (the ice beam's swooshes) is written and consumes the EMVR.
  auto putMover = [](std::vector<uint8_t>& o, float x) {
    PutProperty(o, "EMVR", 1);
    PutFourCC(o, "SMOV");
    PutFourCC(o, "EXTT");
    PutFourCC(o, "CNST");
    for (float each : {x, 0.f, 0.f}) {
      PutConstant(o, Bits(each));
    }
    PutFourCC(o, "EXTR");
    PutFourCC(o, "NONE");
    PutFourCC(o, "NONE");
  };
  part = ConvertSwoosh([&putMover](auto& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 4);
    putMover(o, 0.f);
  });
  Retail mover;
  mover.f("SWSH").f("XFMD").f("CNST").w(4).f("PIRN").f("CNST").w(1).f("_END");
  CHECK_MSG(part.part == mover.bytes && part.dropped.empty(), "XFMD 4 with SMOV(EXTT, 0, EXTR) becomes XFMD 4");

  // An EMVR with an offset is not the emitter's transform.
  part = ConvertSwoosh([&putMover](auto& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 4);
    putMover(o, 1.f);
  });
  bool offsetDropped = false;
  for (const std::string& reason : part.dropped) {
    offsetDropped = offsetDropped || reason == "XFMD 4: EMVR not SMOV(EXTT, 0, EXTR)";
  }
  CHECK_MSG(offsetDropped, "XFMD 4 with an offset EMVR is dropped");
}

// Spawn tables the port cannot run as Remastered does (here spawns placed by a
// component) merge into retail's one KSSM: generators by frame with the 16-byte
// entries retail reads, the first swoosh and electric child as SSWH/SSSD and
// SELC/SESD; further swooshes go to the port-only PSWX.
void TestSpawnTable() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "CNST");
  for (uint32_t word : {1u, 1u, 40u, 0u}) {  // header, end frame, no events
    Put32(out, word);
  }
  Put32(out, 2);  // tables
  const auto spawn = [&](uint32_t retail, const char* form, bool condition) {
    PutGuid(out, Legacy(retail));
    PutFourCC(out, form);
    Put32(out, 0);
    if (condition) {
      PutConstant(out, Bits(0.5f));
    } else {
      PutFourCC(out, "NONE");
    }
  };
  Put32(out, 0);  // table word
  PutConstant(out, 0);  // selector
  Put32(out, 2);  // frames
  Put32(out, 3);
  Put32(out, 2);
  spawn(0x0000B001, "GENP", false);
  spawn(0x0000B002, "SWSH", false);
  Put32(out, 7);
  Put32(out, 1);
  spawn(0x0000B004, "SWSH", false);
  Put32(out, 0);
  PutConstant(out, 1);  // another selector
  Put32(out, 1);
  Put32(out, 3);
  Put32(out, 2);
  spawn(0x0000B001, "GENP", true);
  spawn(0x0000B003, "ELSM", false);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 4);
  PutGuid(out, Legacy(0x0000B001));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 2);
  PutProperty(out, "_END", 4);
  for (uint32_t retail : {0x0000B002u, 0x0000B004u}) {
    PutGuid(out, Legacy(retail));
    PutFourCC(out, "SWSH");
    PutProperty(out, "LENG", 1);
    PutConstant(out, 8);
    PutProperty(out, "_END", 4);
  }
  PutGuid(out, Legacy(0x0000B003));
  PutFourCC(out, "ELSM");
  PutProperty(out, "LWD1", 3);
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "spawn table effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  CHECK_MSG(parts.size() == 5, "root and four children");
  if (parts.size() != 5) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }
  Retail root;
  root.f("GPSM").f("KSSM").f("CNST").w(0).w(1).w(40).w(0).w(1);
  root.w(3).w(2).w(0x0000B001).w(0).w(0).w(0).w(0x0000B001).w(0).w(0).w(0);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.f("SSWH").f("CNST").w(0x0000B002).f("SSSD").f("CNST").w(3);
  root.f("SELC").f("CNST").w(0x0000B003).f("SESD").f("CNST").w(3);
  root.f("PSWX").f("CNST").w(1).w(0x0000B004).w(7);  // the second swoosh, port-only
  root.end();
  CHECK_MSG(parts[0].part == root.bytes, "tables merge into retail's KSSM, SSWH, SELC and PSWX");
  CHECK_MSG(parts[0].dropped.empty() && parts[0].droppedRetail == 0, "the second swoosh is kept as PSWX");
  bool merged = false;
  bool selected = false;
  bool conditional = false;
  for (const std::string& note : parts[0].approximated) {
    merged = merged || note == "KSSM: 2 tables merged";
    selected = selected || note == "KSSM: table selector ignored";
    conditional = conditional || note.find("conditions ignored") != std::string::npos;
  }
  CHECK_MSG(merged && selected && conditional, "merge, selector and condition noted");
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) && properties.size() == 9,
            "converted spawn table reads as retail");
}

// Remastered's spawn tables are kept whole as the port-only PKSM, which
// CElementGen::PortSpawnTables runs as the Switch's CSpawnSystemKeyframeData does:
// each table's start and period, every spawn's form and instance count.
void TestPortSpawnTables() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "CNST");
  for (uint32_t word : {1u, 1u, 40u, 0u}) {  // header, end frame, no events
    Put32(out, word);
  }
  Put32(out, 2);  // tables
  const auto spawn = [&](uint32_t retail, const char* form, uint32_t instances) {
    PutGuid(out, Legacy(retail));
    PutFourCC(out, form);
    Put32(out, ~0u);  // no component
    if (instances != 0) {
      PutConstant(out, instances);
    } else {
      PutFourCC(out, "NONE");
    }
  };
  Put32(out, 0);  // start
  PutConstant(out, 0);  // period
  Put32(out, 1);  // frames
  Put32(out, 3);
  Put32(out, 2);
  spawn(0x0000B001, "GENP", 0);
  spawn(0x0000B002, "SWSH", 2);
  Put32(out, 5);  // start
  PutConstant(out, 10);  // period
  Put32(out, 1);
  Put32(out, 5);
  Put32(out, 2);
  spawn(0x0000B003, "ELSM", 0);
  spawn(0x0000B001, "GENP", 0);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 3);
  PutGuid(out, Legacy(0x0000B001));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 2);
  PutProperty(out, "_END", 4);
  PutGuid(out, Legacy(0x0000B002));
  PutFourCC(out, "SWSH");
  PutProperty(out, "LENG", 1);
  PutConstant(out, 8);
  PutProperty(out, "_END", 4);
  PutGuid(out, Legacy(0x0000B003));
  PutFourCC(out, "ELSM");
  PutProperty(out, "LWD1", 3);
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "PKSM effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  CHECK_MSG(parts.size() == 4, "root and three children");
  if (parts.size() != 4) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }
  Retail root;
  root.f("GPSM").f("PKSM").f("CNST").w(40).w(2);
  root.w(0).w(1).f("CNST").w(0).w(1);
  root.w(3).w(2).f("PART").w(0x0000B001).f("NONE").w(~0u).f("SWHC").w(0x0000B002).f("CNST").w(2).w(~0u);
  root.w(5).w(1).f("CNST").w(10).w(1);
  root.w(5).w(2).f("ELSC").w(0x0000B003).f("NONE").w(~0u).f("PART").w(0x0000B001).f("NONE").w(~0u);
  root.w(0);  // no components
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.end();
  CHECK_MSG(parts[0].part == root.bytes, "spawn tables kept whole as PKSM");
  CHECK_MSG(parts[0].approximated.empty() && parts[0].dropped.empty(), "PKSM leaves nothing out");
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) && properties.size() == 4,
            "PKSM reads back");  // PKSM, TEXR, PIRN
}

// A spawn that places its child with an SEVT(SMOV) component carries the component's trees
// in PKSM, after the tables, for CElementGen to run.
void TestPortSpawnComponents() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "CNST");
  for (uint32_t word : {1u, 1u, 40u, 1u}) {  // header, end frame, one event
    Put32(out, word);
  }
  PutFourCC(out, "SEVT");
  PutFourCC(out, "SMOV");
  PutFourCC(out, "ADD_");
  PutFourCC(out, "EXTT");
  PutFourCC(out, "CNST");
  for (float each : {1.f, 2.f, 3.f}) {
    PutConstant(out, Bits(each));
  }
  PutFourCC(out, "CNST");
  for (float each : {0.f, 0.f, 0.5f}) {
    PutConstant(out, Bits(each));
  }
  PutFourCC(out, "NONE");
  PutFourCC(out, "NONE");
  PutFourCC(out, "NONE");
  Put32(out, 1);  // tables
  Put32(out, 0);  // start
  PutConstant(out, 0);  // period
  Put32(out, 1);  // frames
  Put32(out, 3);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0000B001));
  PutFourCC(out, "GENP");
  Put32(out, 0);  // component 0
  PutFourCC(out, "NONE");
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0000B001));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 2);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "component effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  CHECK_MSG(parts.size() == 2, "root and its child");
  if (parts.size() != 2) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }
  Retail root;
  root.f("GPSM").f("PKSM").f("CNST").w(40).w(1);
  root.w(0).w(1).f("CNST").w(0).w(1);
  root.w(3).w(1).f("PART").w(0x0000B001).f("NONE").w(0);
  root.w(1);  // components
  root.f("ADD_").f("EXTT").f("CNST").f("CNST").w(Bits(1.f)).f("CNST").w(Bits(2.f)).f("CNST").w(Bits(3.f));
  root.w(Bits(0.f)).w(Bits(0.f)).w(Bits(0.5f)).f("NONE").f("NONE");
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.end();
  CHECK_MSG(parts[0].part == root.bytes, "the component kept in PKSM");
  CHECK_MSG(parts[0].approximated.empty() && parts[0].dropped.empty(), "the component leaves nothing out");
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) && properties.size() == 4,
            "the component reads back");
}

// A one-generator effect whose body (properties) `body` writes.
template <typename Body>
std::vector<uint8_t> OneGenerator(Body body) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  body(out);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void PutTxfb(std::vector<uint8_t>& out, bool trst, float scale, bool mirror, float lifeHigh = 1.0f) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "TXFB");
  PutGuid(out, Fresh(5));
  PutFourCC(out, "LFTW");
  PutConstant(out, 0);
  PutConstant(out, Bits(lifeHigh));
  if (!trst) {
    return;
  }
  PutFourCC(out, "TRST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  if (mirror) {
    PutFourCC(out, "KPIN");
    PutFourCC(out, "CREL");
    PutFourCC(out, "LTHN");
    PutFourCC(out, "RAND");
    PutConstant(out, 0);
    PutConstant(out, Bits(1.0f));
    PutConstant(out, Bits(0.5f));
    PutConstant(out, Bits(1.0f));
    PutConstant(out, Bits(scale));
  } else {
    PutConstant(out, Bits(scale));
  }
  PutConstant(out, Bits(1.0f));
  PutConstant(out, 0);
  Put32(out, 0xFFFFFFFFu);
}

void PutTxp2(std::vector<uint8_t>& out, uint32_t cols, uint32_t rows, uint32_t high) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "TXP2");
  PutGuid(out, Fresh(6));
  PutConstant(out, cols);
  PutConstant(out, rows);
  PutFourCC(out, "IRND");
  PutConstant(out, 0);
  PutConstant(out, high);
}

void PutAtx2(std::vector<uint8_t>& out, uint32_t cols, uint32_t percent) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "ATX2");
  PutGuid(out, Fresh(6));
  PutConstant(out, cols);
  PutConstant(out, 4);
  PutFourCC(out, "ILPT");
  PutConstant(out, percent);
  PutConstant(out, 1);
}

void PutModels(std::vector<uint8_t>& out, uint32_t count, uint32_t high) {
  PutProperty(out, "PMDL", 0);
  PutFourCC(out, "SLCT");
  PutFourCC(out, "IRND");
  PutConstant(out, 0);
  PutConstant(out, high);
  PutFourCC(out, "ARRY");
  Put32(out, count);
  for (uint32_t i = 0; i < count; ++i) {
    PutFourCC(out, "CNST");
    PutGuid(out, Fresh(uint8_t(10 + i)));
  }
}

void PutColour(std::vector<uint8_t>& out, float r, float g, float b, float a) {
  PutFourCC(out, "CNST");
  for (const float c : {r, g, b, a}) {
    PutConstant(out, Bits(c));
  }
}

void PutColourChoice(std::vector<uint8_t>& out, uint32_t count) {
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "SLCT");
  PutFourCC(out, "IRND");
  PutConstant(out, 0);
  PutConstant(out, 2);
  PutFourCC(out, "ARRY");
  Put32(out, count);
  for (uint32_t i = 0; i < count; ++i) {
    PutColour(out, 0.25f * float(i), 1.0f, 2.0f, 1.0f);
  }
}

EffectConvertIO AtlasIO() {
  EffectConvertIO io;
  io.assetId = [](const EffectGuid& id, uint32_t type) -> uint32_t {
    for (uint8_t i = 0; i < 16; ++i) {
      if (id == Fresh(i)) {
        return (type == EffectFourCC("CMDL") ? 0xC0DE0000u : 0x7E570000u) + i;
      }
    }
    return 0;
  };
  io.flipbook = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(5)) {
      atlas.id = 0xF11B0001u;
      atlas.cols = 8;
      atlas.rows = 4;
      atlas.frames = 32;
    }
    return atlas;
  };
  return io;
}

std::vector<ConvertedPart> ConvertOne(const std::vector<uint8_t>& data, const EffectConvertIO& io) {
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: effect does not parse: %s\n", error.c_str());
    port_test::CountFailure();
    return {};
  }
  return ConvertEffect(effect, data.data(), io);
}

// XFMD 4's SMVR is the particles' frame (CSimpleMoverElement): kept as the port-only
// PSMV, one mover in the PKSM component format; with no SMVR, PSMV holds none. The
// identity SMOV(EXTT, 0, EXTR) is the emitter's transform and needs no PSMV.
void TestPortMover() {
  const auto putSmvr = [](std::vector<uint8_t>& o, float dz, bool identity) {
    PutProperty(o, "SMVR", 1);
    PutFourCC(o, "SMOV");
    PutFourCC(o, "EXTT");
    PutFourCC(o, "CNST");
    for (float each : {0.f, 0.f, dz}) {
      PutConstant(o, Bits(each));
    }
    PutFourCC(o, identity ? "EXTR" : "NONE");
    PutFourCC(o, "NONE");
    PutFourCC(o, "NONE");
  };
  const auto xfmd4 = [](std::vector<uint8_t>& o) {
    PutProperty(o, "XFMD", 0);
    PutConstant(o, 4);
  };
  std::vector<RetailPartProperty> properties;
  std::string error;

  auto parts = ConvertOne(OneGenerator([&](auto& o) {
    xfmd4(o);
    putSmvr(o, 0.5f, false);
  }), {});
  Retail want;
  want.f("GPSM").f("SIZE").f("CNST").w(0).f("XFMD").f("CNST").w(4).f("PSMV").f("CNST").w(1);
  want.f("EXTT").w(Bits(0.f)).w(Bits(0.f)).w(Bits(0.5f)).f("NONE").f("NONE");
  want.end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "an SMVR under XFMD 4 becomes PSMV");
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty() && parts[0].dropped.empty(),
            "PSMV leaves nothing out");
  CHECK_MSG(parts.size() == 1 && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
                properties.size() == 5,
            "PSMV reads back");  // SIZE, XFMD, PSMV, PIRN

  parts = ConvertOne(OneGenerator(xfmd4), {});
  Retail none;
  none.f("GPSM").f("SIZE").f("CNST").w(0).f("XFMD").f("CNST").w(4).f("PSMV").f("CNST").w(0).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes, "XFMD 4 with no SMVR writes an empty PSMV");

  parts = ConvertOne(OneGenerator([&](auto& o) {
    xfmd4(o);
    putSmvr(o, 0.f, true);
  }), {});
  Retail identity;
  identity.f("GPSM").f("SIZE").f("CNST").w(0).f("XFMD").f("CNST").w(4).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == identity.bytes, "the identity SMVR writes no PSMV");
}

Retail Patl(uint32_t id, int cols, int rows, int count, int mode, int flip) {
  Retail want;
  want.f("GPSM").f("TEXR").f("PATL").f("CNST").w(id);
  for (const int v : {cols, rows, count, mode, flip}) {
    want.f("CNST").w(uint32_t(v));
  }
  want.end();
  return want;
}

// TXP2's atlas of a random tile and TXFB's flipbook become the port's PATL; any
// other form of either is left out of the PART.
void TestAtlasTexture() {
  const EffectConvertIO io = AtlasIO();
  std::string error;
  std::vector<RetailPartProperty> properties;

  // The tile range is a real over the whole atlas (1f), so 4 x 4 is 16 tiles.
  auto parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 4, Bits(1.0f)); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 4, 4, 16, 0, 0).bytes, "TXP2 becomes a random-tile PATL");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "TXP2 drops nothing");
  CHECK_MSG(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
                properties.size() == 3,
            "a PATL reads as retail");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 2, 5); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 4, 2, 6, 0, 0).bytes, "an int range is the last tile");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 4, 99); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "a tile range past the atlas is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 0, 4, Bits(1.0f)); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "an atlas of no columns is refused");

  // ATX2's cycle ILPT(100) is the particle's whole life.
  parts = ConvertOne(OneGenerator([](auto& o) { PutAtx2(o, 2, 100); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 2, 4, 8, 1, 0).bytes, "ATX2 becomes a life PATL");
  parts = ConvertOne(OneGenerator([](auto& o) { PutAtx2(o, 2, 50); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "an ATX2 over half the life is refused");

  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, -1.0f, true); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 1).bytes,
            "TXFB with a random mirror is a flipped flipbook");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, 1.0f, false); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 0).bytes,
            "TXFB with an identity TRST is not flipped");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 0).bytes, "TXFB with no TRST");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, 0.5f, false); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "a TRST scaling x is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, -2.0f, true); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "a mirror to another scale is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false, 0.5f); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "a flipbook over half the life is refused");
  EffectConvertIO bare = io;
  bare.flipbook = nullptr;
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false); }), bare);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "no atlas, no flipbook");

  Retail badCount;
  badCount.f("GPSM").f("TEXR").f("PATL").f("CNST").w(1).f("CNST").w(4).f("CNST").w(4).f("_END");
  CHECK_MSG(!SplitRetailPart(badCount.bytes.data(), badCount.bytes.size(), properties, error), "a short PATL is refused");
}

// COLR's SLCT over an ARRY of colours is kept whole: the index element, the
// entry count and each entry (CElementArray<CColor4f16>::GetValue, 0x271ee4).
void TestColourChoice() {
  std::string error;
  std::vector<RetailPartProperty> properties;
  auto parts = ConvertOne(OneGenerator([](auto& o) { PutColourChoice(o, 3); }), EffectConvertIO{});
  Retail want;
  want.f("GPSM").f("COLR").f("SLCT").f("IRND").f("CNST").w(0).f("CNST").w(2).w(3);
  for (uint32_t i = 0; i < 3; ++i) {
    want.f("CNST");
    for (const float c : {0.25f * float(i), 1.0f, 2.0f, 1.0f}) {
      want.f("CNST").w(Bits(c));
    }
  }
  // An untextured generator draws no quads: SIZE 0 is written for it.
  want.f("SIZE").f("CNST").w(Bits(0.0f));
  want.end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "SLCT of three colours keeps all three");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "the colour choice drops nothing");
  CHECK_MSG(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
                properties.size() == 4,
            "a colour SLCT reads as retail");

  Retail empty;
  empty.f("GPSM").f("COLR").f("SLCT").f("IRND").f("CNST").w(0).f("CNST").w(0).w(0).f("_END");
  CHECK_MSG(!SplitRetailPart(empty.bytes.data(), empty.bytes.size(), properties, error), "a colour SLCT of no entries is refused");
}

// ROTA's SLCT(IRND(0, 3), ARRY(-1, 1, -0.5, 0.5)) (GENP 445c8f18) becomes the port SLCT of reals.
void TestRealChoice() {
  const float entries[] = {-1.0f, 1.0f, -0.5f, 0.5f};
  std::string error;
  std::vector<RetailPartProperty> properties;
  auto parts = ConvertOne(OneGenerator([&](auto& o) {
                            PutProperty(o, "ROTA", 3);
                            PutFourCC(o, "SLCT");
                            PutFourCC(o, "IRND");
                            PutConstant(o, 0);
                            PutConstant(o, 3);
                            PutFourCC(o, "ARRY");
                            Put32(o, 4);
                            for (const float e : entries) {
                              PutConstant(o, Bits(e));
                            }
                          }),
                          EffectConvertIO{});
  Retail want;
  // Remastered turns the other way: the converter negates ROTA.
  want.f("GPSM").f("ROTA").f("MULT").f("SLCT").f("IRND").f("CNST").w(0).f("CNST").w(3).w(4);
  for (const float e : entries) {
    want.f("CNST").w(Bits(e));
  }
  want.f("CNST").w(Bits(-1.0f));
  want.f("SIZE").f("CNST").w(Bits(0.0f));
  want.end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "SLCT of four reals keeps all four");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "the real choice drops nothing");
  CHECK_MSG(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
                properties.size() == 4,
            "a real SLCT reads as retail");
}

// PMDL's random choice of four models becomes the first as PMDL and all as PMDV.
void TestModelChoice() {
  const EffectConvertIO io = AtlasIO();
  std::string error;
  std::vector<RetailPartProperty> properties;
  auto parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 3); }), io);
  Retail want;
  want.f("GPSM").f("PMDL").f("CNST").w(0xC0DE000A).f("PMDV").f("CNST").w(4);
  for (uint32_t i = 0; i < 4; ++i) {
    want.f("CNST").w(0xC0DE000A + i);
  }
  // A models-only generator draws no quads: SIZE 0 is written for it.
  want.f("SIZE").f("CNST").w(Bits(0.0f));
  want.end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "SLCT of four models is PMDL and PMDV");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "the model choice drops nothing");
  CHECK_MSG(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
                properties.size() == 5,
            "PMDL, PMDV and SIZE read as retail");

  // Remastered takes entry index % n: the PMDV lists IRND's h + 1 outcomes.
  const auto variants = [](std::initializer_list<uint32_t> picks) {
    Retail out;
    out.f("GPSM").f("PMDL").f("CNST").w(0xC0DE000A + *picks.begin());
    if (picks.size() > 1) {
      out.f("PMDV").f("CNST").w(uint32_t(picks.size()));
      for (const uint32_t pick : picks) {
        out.f("CNST").w(0xC0DE000A + pick);
      }
    }
    out.f("SIZE").f("CNST").w(Bits(0.0f));
    out.end();
    return out.bytes;
  };
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 2); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 0 && parts[0].part == variants({0, 1, 2}),
            "a range short of the array picks the first three");
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 2, 4); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 0 && parts[0].part == variants({0, 1, 0, 1, 0}),
            "a range past the array wraps as index % n");
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 1, 8); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 0 && parts[0].part == variants({0}),
            "one model under any range is a constant PMDL");
  parts = ConvertOne(OneGenerator([](auto& o) {
                       PutProperty(o, "PMDL", 0);
                       PutFourCC(o, "SLCT");
                       PutConstant(o, 5);
                       PutFourCC(o, "ARRY");
                       Put32(o, 2);
                       for (uint8_t i = 0; i < 2; ++i) {
                         PutFourCC(o, "CNST");
                         PutGuid(o, Fresh(uint8_t(10 + i)));
                       }
                     }),
                     io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 0 && parts[0].part == variants({1}),
            "a constant index picks entry index % n");
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 0, 0); }), io);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "an empty array is refused");
  EffectConvertIO none;
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 3); }), none);
  CHECK_MSG(parts.size() == 1 && parts[0].droppedRetail == 1, "models with no ids are refused");

  Retail one;
  one.f("GPSM").f("PMDL").f("CNST").w(1).f("PMDV").f("CNST").w(2).f("CNST").w(1).f("_END");
  CHECK_MSG(!SplitRetailPart(one.bytes.data(), one.bytes.size(), properties, error), "a PMDV short of its count is refused");
  Retail alone;
  alone.f("GPSM").f("PMDV").f("CNST").w(1).f("CNST").w(1).f("_END");
  CHECK_MSG(!SplitRetailPart(alone.bytes.data(), alone.bytes.size(), properties, error) ||
                properties.size() == 1,
            "a PMDV parses on its own or is refused");
}

// A generator with no texture, material or model draws nothing: its SIZE is
// left out and a SIZE of 0 written before _END instead. One with a TEXR keeps
// its SIZE.
void TestDrawsNothing() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "MAXP", 1);
                 PutConstant(o, 4);
                 PutProperty(o, "SIZE", 3);
                 PutConstant(o, Bits(2.0f));
               }),
               {});
  Retail want;
  want.f("GPSM").f("MAXP").f("CNST").w(4).f("SIZE").f("CNST").w(Bits(0.0f)).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "a generator with no texture gets SIZE 0");
  CHECK_MSG(!parts.empty() && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "SIZE: the generator draws nothing",
            "its SIZE is dropped as drawing nothing");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "drawing nothing drops no retail property");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "MAXP", 1);
                 PutConstant(o, 4);
                 PutProperty(o, "SIZE", 3);
                 PutConstant(o, Bits(2.0f));
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail textured;
  textured.f("GPSM").f("MAXP").f("CNST").w(4).f("SIZE").f("CNST").w(Bits(2.0f));
  textured.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == textured.bytes, "a generator with a TEXR keeps its SIZE");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty(), "nothing dropped with a TEXR");
}

// PBDM is Remastered's blend mode: 2 is retail's additive AAPH, 1 (premultiplied) and
// 3 (opaque) the port-only PBDM, 4 and up alpha (noted as approximated).
void TestBlendMode() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PBDM", 0);
                 o.push_back(2);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("AAPH").f("CNST").b(1);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "PBDM 2 becomes AAPH");
  CHECK_MSG(!parts.empty() && parts[0].approximated.empty(), "PBDM 2 is exact");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "PBDM 2 drops nothing");

  for (uint8_t mode : {uint8_t(1), uint8_t(3)}) {
    parts = ConvertOne(OneGenerator([mode](auto& o) {
                   PutProperty(o, "PBDM", 0);
                   o.push_back(mode);
                   PutTexr(o, 0x1234ABCD);
                 }),
                 {});
    Retail port;
    port.f("GPSM").f("PBDM").f("CNST").w(mode);
    port.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == port.bytes, "PBDM 1 and 3 become the port's PBDM");
    CHECK_MSG(!parts.empty() && parts[0].approximated.empty(), "PBDM 1 and 3 are exact");
    CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "PBDM 1 and 3 drop nothing");
  }

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PBDM", 0);
                 o.push_back(4);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail alpha;
  alpha.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == alpha.bytes, "PBDM 4 writes nothing");
  CHECK_MSG(!parts.empty() && parts[0].approximated.size() == 1 &&
                parts[0].approximated[0] == "PBDM 4 taken as alpha blending",
            "PBDM 4 is listed as approximated");
}

// DFOG 1 (Remastered's no-fog bit, CreateGPSM 0x25a560) becomes the port-only PDFG; DFOG 0 is
// the default and writes nothing. The effects store it as an element, CNST and a byte.
void TestUnfogged() {
  for (uint8_t on : {uint8_t(1), uint8_t(0)}) {
    const auto parts = ConvertOne(OneGenerator([on](auto& o) {
                                    PutProperty(o, "DFOG", 0);
                                    PutFourCC(o, "CNST");
                                    o.push_back(on);
                                    PutTexr(o, 0x1234ABCD);
                                  }),
                                  {});
    Retail want;
    want.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
    if (on != 0) {
      want.f("PDFG").f("CNST").w(1);
    }
    want.end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "DFOG 1 becomes PDFG, DFOG 0 nothing");
    CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(), "DFOG is exact");
  }
}

// ZTES 0 becomes the port-only PZTS 0 (depth compare ALWAYS instead of LEQUAL, ctor 0x28c2fc/0x28c384),
// in a generator and a swoosh; IVSE 0 the port-only PIVS 0 (retail always turns the emitter's velocity);
// ISVF the port-only PISV (EMRV's mode word, then the factor) and an authored ITPE 1 the port-only PITP
// (the mode word). The defaults, an authored ITPE 0 and XFMD 3 (EMRV is zero there) write nothing;
// an XFMD that EMRV is not decoded for drops ISVF, naming it.
void TestInheritedMotion() {
  const auto one = [](uint8_t ztes, uint8_t ivse, bool isvf, int itpe, uint32_t xfmd) {
    return ConvertOne(OneGenerator([=](auto& o) {
                        PutProperty(o, "XFMD", 0);
                        PutConstant(o, xfmd);
                        PutProperty(o, "ZTES", 0);
                        PutFourCC(o, "CNST");
                        o.push_back(ztes);
                        PutProperty(o, "IVSE", 0);
                        PutFourCC(o, "CNST");
                        o.push_back(ivse);
                        if (isvf) {
                          PutProperty(o, "ISVF", 1);
                          PutConstant(o, Bits(0.5f));
                        }
                        if (itpe >= 0) {
                          PutProperty(o, "ITPE", 0);
                          PutFourCC(o, "CNST");
                          o.push_back(uint8_t(itpe));
                        }
                        PutTexr(o, 0x1234ABCD);
                      }),
                      {});
  };
  // XFMD 2 (the default) writes PZTS, PIVS and PITP at the end, PISV where ISVF stood
  auto parts = one(0, 0, true, 1, 2);
  Retail want;
  want.f("GPSM").f("PISV").f("CNST").w(0).f("CNST").w(Bits(0.5f)).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  want.f("PZTS").f("CNST").w(0).f("PIVS").f("CNST").w(0).f("PITP").f("CNST").w(0);
  want.end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "ZTES 0, IVSE 0, ISVF and ITPE 1 become PZTS, PIVS, PISV, PITP");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(), "they are exact");
  parts = one(1, 1, false, 0, 2);
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == plain.bytes && parts[0].dropped.empty(),
            "ZTES 1, IVSE 1 and ITPE 0 write nothing");
  const auto has = [](const ConvertedPart& part, const char* fourcc) {
    return std::search(part.part.begin(), part.part.end(), fourcc, fourcc + 4) != part.part.end();
  };
  // With the import's spreadDefault, an absent ITPE is Remastered's default 1, flagged 0x100, and noted.
  {
    EffectConvertIO io;
    io.spreadDefault = true;
    const auto spread = [&](uint32_t xfmd) {
      return ConvertOne(OneGenerator([=](auto& o) {
                          PutProperty(o, "XFMD", 0);
                          PutConstant(o, xfmd);
                          PutTexr(o, 0x1234ABCD);
                        }),
                        io);
    };
    parts = spread(2);
    Retail flagged;
    flagged.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).f("PITP").f("CNST").w(0x100).end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == flagged.bytes, "no ITPE under XFMD 2 is PITP 0x100");
    CHECK_MSG(parts.size() == 1 &&
                  std::count(parts[0].approximated.begin(), parts[0].approximated.end(),
                             std::string("ITPE: absent, Remastered's default 1")) == 1,
              "the default is noted");
    parts = spread(3);
    CHECK_MSG(parts.size() == 1 && !has(parts[0], "PITP"), "no ITPE under XFMD 3 writes nothing");
  }
  parts = one(1, 1, true, 1, 3);
  CHECK_MSG(parts.size() == 1 && !has(parts[0], "PISV") && !has(parts[0], "PITP") && parts[0].dropped.empty(),
            "ISVF and ITPE under XFMD 3 write nothing (EMRV is zero there)");
  parts = one(1, 1, true, 1, 4);
  CHECK_MSG(parts.size() == 1 && !has(parts[0], "PISV") && !has(parts[0], "PITP") && parts[0].dropped.size() == 2 &&
                parts[0].dropped[0].find("ISVF under XFMD 4") != std::string::npos &&
                parts[0].dropped[1].find("ITPE under XFMD 4") != std::string::npos,
            "ISVF and ITPE under XFMD 4 are dropped, naming it");
  const auto swoosh = [](uint8_t ztes) {
    return ConvertSwoosh([=](auto& o) {
      PutProperty(o, "ZTES", 0);
      PutFourCC(o, "CNST");
      o.push_back(ztes);
    });
  };
  ConvertedPart part = swoosh(0);
  CHECK_MSG(has(part, "PZTS") && part.dropped.empty(), "a swoosh's ZTES 0 becomes PZTS");
  part = swoosh(1);
  CHECK_MSG(!has(part, "PZTS") && part.dropped.empty(), "a swoosh's ZTES 1 writes nothing");
}

// DBIS (Remastered's particle depth bias, desc+0xd4) becomes the port-only PDBS on a generator and
// a swoosh; 0 is the default and writes nothing.
// A drawing sprite with DVVN 1..4 and no SIZE gets Remastered's default SIZE 1.0 (retail's is 0.1);
// DVVN 0 or an authored SIZE writes none.
void TestDvvnDefaultSize() {
  // The source reader mutates IEXP at each DVVN/IEXP occurrence; test both orders and resets.
  for (const bool versionLast : {false, true}) {
    const auto exposureParts = ConvertOne(OneGenerator([=](auto& o) {
      const auto property = [&](const char* name, uint8_t value) {
        PutProperty(o, name, 1);
        o.push_back(value);
      };
      if (versionLast) {
        property("IEXP", 0);
        property("DVVN", 4);
      } else {
        property("DVVN", 4);
        property("IEXP", 0);
      }
      PutTexr(o, 0x1234ABCD);
    }), {});
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(exposureParts.size() == 1 &&
                  SplitRetailPart(exposureParts[0].part.data(), exposureParts[0].part.size(), properties, error),
              "exposure properties survive conversion");
    CHECK_MSG(properties.size() >= 2 &&
                  properties[0].fourcc == EffectFourCC(versionLast ? "IEXP" : "DVVN") &&
                  properties[1].fourcc == EffectFourCC(versionLast ? "DVVN" : "IEXP"),
              "DVVN and IEXP preserve source property order");
    CHECK_MSG(exposureParts.size() == 1 && exposureParts[0].dropped.size() == size_t(versionLast),
              "only enabled inverse exposure needs a material; DVVN itself is handled");
  }
  const auto one = [](uint8_t dvvn, bool size) {
    return ConvertOne(OneGenerator([=](auto& o) {
                        PutProperty(o, "DVVN", 0);
                        PutFourCC(o, "CNST");
                        o.push_back(dvvn);
                        if (size) {
                          PutProperty(o, "SIZE", 1);
                          PutConstant(o, Bits(0.5f));
                        }
                        PutTexr(o, 0x1234ABCD);
                      }),
                      {});
  };
  const auto sizes = [](const ConvertedPart& part) {
    const char tag[4] = {'S', 'I', 'Z', 'E'};
    int n = 0;
    for (auto it = part.part.begin(); (it = std::search(it, part.part.end(), tag, tag + 4)) != part.part.end(); ++it) {
      ++n;
    }
    return n;
  };
  auto parts = one(4, false);
  Retail want;
  want.f("GPSM").f("DVVN").f("CNST").w(4).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).f("SIZE").f("CNST").w(Bits(1.0f)).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "DVVN 4 without SIZE writes SIZE 1.0");
  parts = one(0, false);
  CHECK_MSG(parts.size() == 1 && sizes(parts[0]) == 0, "DVVN 0 writes no SIZE");
  parts = one(1, true);
  CHECK_MSG(parts.size() == 1 && sizes(parts[0]) == 1, "an authored SIZE is kept alone");
  for (uint8_t version : {0, 1, 2, 3, 4, 5, 255}) {
    parts = one(version, true);
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(parts.size() == 1 && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "DVVN default velocity carrier parses independently of authored SIZE");
    const auto found = std::find_if(properties.begin(), properties.end(), [](const auto& p) {
      return p.fourcc == EffectFourCC("DVVN");
    });
    CHECK_MSG(found != properties.end(), "all version bytes survive, including versions that reset velocity to zero");
    if (found != properties.end()) {
      CHECK_MSG(found->value == Retail().f("CNST").w(version).bytes, "DVVN version is preserved exactly");
    }
  }
}

void TestDepthBias() {
  for (float bias : {-0.5f, 0.0f}) {
    const auto parts = ConvertOne(OneGenerator([bias](auto& o) {
                                    PutProperty(o, "DBIS", 3);
                                    PutConstant(o, Bits(bias));
                                    PutTexr(o, 0x1234ABCD);
                                  }),
                                  {});
    Retail want;
    want.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
    if (bias != 0.0f) {
      want.f("PDBS").f("CNST").w(Bits(bias));
    }
    want.end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "DBIS becomes PDBS, 0 nothing");
    CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(), "DBIS is exact");
  }
  const ConvertedPart swoosh = ConvertSwoosh([](auto& o) {
    PutProperty(o, "DBIS", 3);
    PutConstant(o, Bits(-2.0f));
  });
  Retail pdbs;
  pdbs.f("PDBS").f("CNST").w(Bits(-2.0f));
  CHECK_MSG(std::search(swoosh.part.begin(), swoosh.part.end(), pdbs.bytes.begin(), pdbs.bytes.end()) != swoosh.part.end() &&
                swoosh.dropped.empty(),
            "a swoosh's DBIS becomes PDBS");
}

// GPUA (how much GPU time is free) is 1.0 exactly at a GPU frame time of 14 ms or less.
void TestGpuAvailability() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SIZE", 3);
                 PutFourCC(o, "GPUA");
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("SIZE").f("CNST").w(Bits(1.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "GPUA becomes CNST 1");
  CHECK_MSG(!parts.empty() && parts[0].approximated.empty(), "GPUA is exact, not approximated");
}

// A mod-vector CRLN (CMVECurlNoise) is the port-only PCRM (mode, V, t) around its V child's
// curl-noise evaluator, the port-only PCRN (P, F, A, field). V is turned into the port's
// space by BSIS like any mod-vector argument; the field is NONE or CNST and the VECF's id.
void PutCurlNoise(std::vector<uint8_t>& out, uint8_t slice, const EffectGuid& field, bool octaves, uint8_t mode,
                  float t) {
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "CRLN");
  PutFourCC(out, "CRLN");
  out.push_back(0);
  out.push_back(slice);
  PutFourCC(out, "VECF");
  PutGuid(out, field);
  PutFourCC(out, "PLOC");
  PutConstant(out, Bits(1.0f));
  if (octaves) {
    PutFourCC(out, "CNST");
    Put32(out, 3);
  } else {
    PutFourCC(out, "NONE");
  }
  PutFourCC(out, "NONE");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.5f));
  PutConstant(out, Bits(0.25f));
  PutConstant(out, Bits(2.0f));
  out.push_back(mode);
  PutConstant(out, Bits(t));
}

void TestCurlNoise() {
  const EffectGuid field = {0x51, 0, 0x75, 0x16, 0xf3, 0xaa, 0xd4, 0x4e, 0xb4, 0xcb, 0xdb, 0x5b, 0x76, 0xd8, 0xc4, 0x7f};
  EffectConvertIO io;
  io.assetId = [&](const EffectGuid& id, uint32_t type) -> uint32_t {
    if (type != EffectFourCC("VECF")) {
      return EffectRetailId(id).value_or(0);
    }
    return id == field ? 0xCAFE0001u : 0;
  };
  const auto convert = [&](uint8_t slice, const EffectGuid& id, bool octaves, uint8_t mode, float t) {
    return ConvertOne(OneGenerator([&](auto& o) {
                        PutCurlNoise(o, slice, id, octaves, mode, t);
                        PutTexr(o, 0x1234ABCD);
                      }),
                      io);
  };
  const auto noise = [](Retail& r, bool withField) {
    r.f("PCRN").f("BSIS").f("PLOC").f("CNST").w(Bits(1.0f));
    RetailVector(r, 0.5f, 0.25f, 2.0f);
    if (withField) {
      r.f("CNST").w(0xCAFE0001u);
    } else {
      r.f("NONE");
    }
  };
  {
    auto parts = convert(0, field, false, 0, 0.0f);
    Retail want;
    want.f("GPSM").f("VEL1").f("PCRM").w(0).f("BSIS");
    noise(want, true);
    want.f("CNST").w(Bits(0.0f));
    want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "mod-vector CRLN mode 0 is PCRM around PCRN with a field");
    CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "CRLN slice 0 drops nothing");
  }
  {
    auto parts = convert(0, EffectGuid{}, false, 1, 0.3f);
    Retail want;
    want.f("GPSM").f("VEL1").f("PCRM").w(1).f("BSIS");
    noise(want, false);
    want.f("CNST").w(Bits(0.3f));
    want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "mod-vector CRLN mode 1 with a zero field is NONE");
  }
  {
    auto parts = convert(2, field, false, 0, 0.0f);
    CHECK_MSG(parts.size() == 1 && parts[0].dropped.size() == 1 &&
                  parts[0].dropped[0].find("CRLN slice 2 not ported") != std::string::npos,
              "CRLN slice 2 drops with a reason");
    // Octaves convert to PCRO (tests/port_remastered_effect_convert_crlnoct.cpp).
    auto unknown = convert(0, EffectGuid{0x11}, false, 0, 0.0f);
    CHECK_MSG(unknown.size() == 1 && unknown[0].dropped.size() == 1 &&
                  unknown[0].dropped[0].find("not imported") != std::string::npos,
              "CRLN with an unimported field drops with a reason");
  }
}

// SPAF(n, fallback) reads the parent system's variable n (CElementParentAddlVar):
// kept as SPAF, n and the fallback, which only a constant keeps; n above 9 is 0xff.
void TestParentRead() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SIZE", 3);
                 PutFourCC(o, "SPAF");
                 o.push_back(5);
                 PutConstant(o, Bits(2.5f));
                 PutProperty(o, "ROTA", 3);
                 PutFourCC(o, "SPAF");
                 o.push_back(12);
                 PutFourCC(o, "RAND");
                 PutConstant(o, Bits(1.f));
                 PutConstant(o, Bits(2.f));
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("SIZE").f("SPAF").w(5).f("CNST").w(Bits(2.5f));
  want.f("ROTA").f("MULT").f("SPAF").w(0xff).f("CNST").w(0).f("CNST").w(Bits(-1.f));  // ROTA is negated
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "SPAF keeps n and a constant fallback");
  CHECK_MSG(!parts.empty() && parts[0].approximated.empty() && parts[0].dropped.empty(),
            "SPAF is exact");
}

// SVRn is the system's own variable n, which PSAn reads: the port-only PSYS, in
// dependency order, with IRND/KPIN hoisted into hidden init-only reals (slot 10 up).
void TestSystemVars() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SVR4", 3);
                 PutFourCC(o, "MULT");
                 PutFourCC(o, "PSA3");
                 PutConstant(o, Bits(2.f));
                 PutProperty(o, "SVR3", 3);
                 PutFourCC(o, "IRND");
                 PutConstant(o, Bits(1.f));
                 PutConstant(o, Bits(3.f));
                 PutProperty(o, "SIZE", 3);
                 PutFourCC(o, "PSA4");
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("SIZE").f("PSYR").w(4);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  want.f("PSYS").f("CNST").w(3);
  want.w(0).w(10).w(1).f("RAND").f("CNST").w(Bits(1.f)).f("CNST").w(Bits(3.f));
  want.w(0).w(3).w(0).f("PSYR").w(10);
  want.w(0).w(4).w(0).f("MULT").f("PSYR").w(3).f("CNST").w(Bits(2.f));
  want.end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "SVR3/SVR4 become PSYS in dependency order");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty(), "nothing dropped");
}

// An order-0 PMRQ REUL is one PMRT Euler vector. Preserve its angle expressions,
// including IRND, which PIRN evaluates once per particle and expression.
void TestModelRotation() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "REUL");
                 PutConstant(o, Bits(-90.0f));
                 PutConstant(o, Bits(0.0f));
                 PutFourCC(o, "SCAL");
                 PutConstant(o, Bits(-0.05f));
                 o.push_back(0);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("PMRT").f("PQAD");
  RetailVector(want, 90.f, 0.f, 180.f);
  want.f("PQAD");
  want.f("CNST").f("CNST").w(Bits(-90.f)).f("CNST").w(Bits(0.f)).f("SCAL").f("CNST").w(Bits(-0.05f));
  RetailVector(want, 90.f, 0.f, 180.f);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "PMRQ REUL becomes a basis-conjugated PMRT");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(), "PMRQ REUL is exact");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "REUL");
                 PutConstant(o, Bits(90.0f));
                 PutFourCC(o, "IRND");
                 PutConstant(o, Bits(0.0f));
                 PutConstant(o, Bits(360.0f));
                 PutConstant(o, Bits(0.0f));
                 o.push_back(0);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail random;
  random.f("GPSM").f("PMRT").f("PQAD");
  RetailVector(random, 90.f, 0.f, 180.f);
  random.f("PQAD");
  random.f("CNST").f("CNST").w(Bits(90.0f));
  random.f("IRND").f("CNST").w(Bits(0.0f)).f("CNST").w(Bits(360.0f));
  random.f("CNST").w(Bits(0.0f));
  RetailVector(random, 90.f, 0.f, 180.f);
  random.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == random.bytes,
            "basis-conjugated PMRQ REUL preserves (90, IRND(0,360), 0) in PMRT");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(),
            "direct PMRQ REUL with IRND is not dropped or approximated");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "direct PMRQ IRND output passes the retail element reader");
    CHECK_MSG(std::any_of(properties.begin(), properties.end(),
                           [](const RetailPartProperty& property) { return property.fourcc == EffectFourCC("PIRN"); }),
              "direct PMRQ IRND output retains the per-particle sampling marker");
  }

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "REUL");
                 PutConstant(o, Bits(90.0f));
                 PutFourCC(o, "ADD_");
                 PutConstant(o, Bits(15.0f));
                 PutFourCC(o, "IRND");
                 PutConstant(o, Bits(0.0f));
                 PutConstant(o, Bits(360.0f));
                 PutConstant(o, Bits(0.0f));
                 o.push_back(0);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail nested;
  nested.f("GPSM").f("PMRT").f("PQAD");
  RetailVector(nested, 90.f, 0.f, 180.f);
  nested.f("PQAD").f("CNST").f("CNST").w(Bits(90.0f));
  nested.f("ADD_").f("CNST").w(Bits(15.0f));
  nested.f("IRND").f("CNST").w(Bits(0.0f)).f("CNST").w(Bits(360.0f));
  nested.f("CNST").w(Bits(0.0f));
  RetailVector(nested, 90.f, 0.f, 180.f);
  nested.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == nested.bytes,
            "basis-conjugated PMRQ REUL preserves IRND nested in a supported real expression");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty(), "nested PMRQ IRND is not dropped");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "REUL");
                 PutConstant(o, Bits(90.0f));
                 PutFourCC(o, "IRND");
                 PutConstant(o, Bits(0.0f));
                 PutConstant(o, Bits(360.0f));
                 PutConstant(o, Bits(0.0f));
                 o.push_back(1);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == plain.bytes, "direct PMRQ rejects an unsupported Euler order");
  CHECK_MSG(!parts.empty() && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "PMRQ: a rotation order other than 0 (XYZ)",
            "direct PMRQ reports an unsupported Euler order");
}

void TestMalformedModelRotation() {
  const std::vector<uint8_t> malformed = OneGenerator([](auto& o) {
    PutProperty(o, "PMRQ", 3);
    PutFourCC(o, "REUL");
    PutConstant(o, Bits(90.0f));
    PutFourCC(o, "IRND");
    PutConstant(o, Bits(0.0f));
    PutFourCC(o, "NOPE"); // malformed second IRND operand
    PutConstant(o, Bits(0.0f));
    o.push_back(0);
    PutTexr(o, 0x1234ABCD);
  });
  EffectNode effect;
  std::string error;
  CHECK_MSG(!ParseEffect(malformed.data(), malformed.size(), effect, error),
            "malformed nested IRND in PMRQ is rejected by the effect parser");
  CHECK_MSG(!error.empty(), "malformed PMRQ reports the parse failure");
}

void PutLoopingEmitterKey(std::vector<uint8_t>& out) {
  PutFourCC(out, "KEYE");
  Put32(out, 0);    // emitter time, not life percentage
  Put32(out, 0);    // unknown
  out.push_back(1); // loop
  out.push_back(0); // unknown
  Put32(out, 120);  // end
  Put32(out, 0);    // start
  Put32(out, 120);  // all source samples
  for (int i = 0; i < 120; ++i) {
    const float degrees = i == 119 ? 360.00003f : i == 0 ? 0.f : float(i) * 3.0252087f;
    Put32(out, Bits(degrees));
  }
}

void PutStaticEuler(std::vector<uint8_t>& out, float x, float y, float z, uint8_t order = 0) {
  PutFourCC(out, "REUL");
  PutConstant(out, Bits(x));
  PutConstant(out, Bits(y));
  PutConstant(out, Bits(z));
  out.push_back(order);
}

void TestComposedModelRotation() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                  PutProperty(o, "PMRQ", 3);
                  PutFourCC(o, "RADD");
                  PutFourCC(o, "REUL");
                  for (int i = 0; i < 3; ++i) {
                    PutFourCC(o, "IRND");
                    PutConstant(o, Bits(0.f));
                    PutConstant(o, Bits(360.f));
                  }
                  o.push_back(0);
                  PutFourCC(o, "REUL");
                  PutLoopingEmitterKey(o);
                  PutLoopingEmitterKey(o);
                  PutConstant(o, Bits(0.f));
                  o.push_back(0);
                  PutTexr(o, 0x1234ABCD);
                }),
                {});

  Retail want;
  want.f("GPSM").f("PMRT").f("PQAD");
  RetailVector(want, 90.f, 0.f, 180.f);
  want.f("PQAD").f("PQAD");
  want.f("CNST");
  for (int i = 0; i < 3; ++i) {
    want.f("IRND").f("CNST").w(Bits(0.f)).f("CNST").w(Bits(360.f));
  }
  want.f("CNST");
  for (int axis = 0; axis < 2; ++axis) {
    want.f("KEYE").w(0).w(0).b(1).b(0).w(120).w(0).w(120);
    for (int i = 0; i < 120; ++i) {
      const float degrees = i == 119 ? 360.00003f : i == 0 ? 0.f : float(i) * 3.0252087f;
      want.w(Bits(degrees));
    }
  }
  want.f("CNST").w(Bits(0.f));
  RetailVector(want, 90.f, 0.f, 180.f);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes,
            "basis-conjugated ordered PMRQ preserves IRND and all 120 looping KEYE samples in PQAD");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(),
            "ordered PMRQ has no dropped or approximated rotation data");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    const bool read = SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    CHECK_MSG(read, "basis-conjugated PQAD PMRT passes the retail element reader");
    CHECK_MSG(read && std::any_of(properties.begin(), properties.end(), [](const RetailPartProperty& property) {
                return property.fourcc == EffectFourCC("PIRN");
              }),
              "ordered IRND/KEYE rotation keeps the per-particle PIRN marker");
  }

  parts = ConvertOne(OneGenerator([](auto& o) {
                PutProperty(o, "PMRQ", 3);
                PutFourCC(o, "RADD");
                PutStaticEuler(o, 90.f, 0.f, 0.f, 1);
                PutStaticEuler(o, 0.f, 90.f, 0.f);
                PutTexr(o, 0x1234ABCD);
              }),
              {});
  CHECK_MSG(!parts.empty() && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "PMRQ: a rotation order other than 0 (XYZ)",
            "ordered PMRQ rejects an unsupported Euler order");

  parts = ConvertOne(OneGenerator([](auto& o) {
                PutProperty(o, "PMRQ", 3);
                PutFourCC(o, "RADD");
                PutStaticEuler(o, 90.f, 0.f, 0.f);
                PutFourCC(o, "CNST");
                PutConstant(o, Bits(0.f));
                PutConstant(o, Bits(90.f));
                PutConstant(o, Bits(0.f));
                PutTexr(o, 0x1234ABCD);
              }),
              {});
  CHECK_MSG(!parts.empty() && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "PMRQ: a rotation that is not REUL/RADD/RAZY/MPCB/EXTR/CMRT",
            "ordered PMRQ refuses a non-REUL operand rather than adding Euler components");
}

// SHTM 1 (shutdown kills the live particles) is the port-only PSHM, 0 the default; SHTT is the
// shutdown countdown, PSHT; PMOV and PMPV are the model direction and pivot, vectors in the port's
// space (BSIS, folded for a constant). PMOV is deleted by Remastered under a PMRQ base rotation
// (PMRB/PMCF) and is not converted with EMTU.
void TestShutdownAndPivot() {
  const auto convert = [](auto&& body) {
    return ConvertOne(OneGenerator([&](auto& o) {
                        body(o);
                        PutTexr(o, 0x1234ABCD);
                      }),
                      {});
  };
  const auto shtm = [&](uint8_t mode) {
    return convert([&](auto& o) {
      PutProperty(o, "SHTM", 0);
      o.push_back(mode);
    });
  };
  auto parts = shtm(1);
  Retail want;
  want.f("GPSM").f("PSHM").f("CNST").w(1).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes && parts[0].dropped.empty(), "SHTM 1 is PSHM 1");
  parts = shtm(0);
  Retail none;
  none.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.empty(), "SHTM 0 writes nothing");
  parts = shtm(2);
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "SHTM: mode 2",
            "SHTM 2 is dropped with its reason");

  const auto isdt = [&](uint8_t value) {
    return convert([&](auto& o) {
      PutProperty(o, "ISDT", 0);
      o.push_back(value);
    });
  };
  parts = isdt(0);
  Retail pisd;
  pisd.f("GPSM").f("PISD").f("CNST").w(0).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == pisd.bytes && parts[0].dropped.empty(), "ISDT 0 is PISD 0");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "a PART with PISD passes the retail element reader");
  }
  parts = isdt(1);
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.empty(),
            "ISDT 1, the default, writes nothing");

  const auto blit = [&](uint8_t value) {
    return convert([&](auto& o) {
      PutProperty(o, "BLIT", 0);
      o.push_back(value);
    });
  };
  const auto vmpc = [&](uint8_t value) {
    return convert([&](auto& o) {
      PutProperty(o, "VMPC", 0);
      o.push_back(value);
    });
  };
  for (const char* tag : {"VGD1", "VGD2", "VGD3", "VGD4"}) {
    parts = convert([&](auto& o) { PutProperty(o, tag, 0); o.push_back(1); });
    Retail global;
    global.f("GPSM").f(tag).f("CNST").w(1).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == global.bytes && parts[0].dropped.empty(),
              "VGD true preserves the world-space selector");
    if (!parts.empty()) {
      std::vector<RetailPartProperty> properties;
      std::string error;
      CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "VGD carrier reads");
    }
    parts = convert([&](auto& o) { PutProperty(o, tag, 0); o.push_back(0); });
    CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.empty(), "VGD false is a no-op");
  }
  for (const bool globalLast : {false, true}) {
    parts = convert([&](auto& o) {
      PutProperty(o, globalLast ? "VMD2" : "VGD2", 0); o.push_back(1);
      PutProperty(o, globalLast ? "VGD2" : "VMD2", 0); o.push_back(1);
      PutProperty(o, "VMD2", 0); o.push_back(0);
      PutProperty(o, "VGD2", 0); o.push_back(0);
    });
    Retail ordered;
    ordered.f("GPSM");
    if (globalLast) ordered.f("VMD2").f("CNST").b(1);
    ordered.f("VGD2").f("CNST").w(1);
    if (!globalLast) ordered.f("VMD2").f("CNST").b(1);
    ordered.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
    CHECK_MSG(parts.size() == 1 && parts[0].part == ordered.bytes && parts[0].dropped.empty(),
              "VGD/VMD true order preserved; false does not reset either selector");
  }
  parts = vmpc(1);
  Retail birthVelocity;
  birthVelocity.f("GPSM").f("VMPC").f("CNST").w(1).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == birthVelocity.bytes && parts[0].dropped.empty(),
            "VMPC 1 survives conversion independently of material support");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "VMPC passes the PART element reader");
  }
  parts = vmpc(0);
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.empty(),
            "VMPC 0 leaves retail birth behavior unchanged");
  parts = vmpc(2);
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "VMPC: not a constant 0 or 1",
            "invalid VMPC remains diagnosed");
  parts = blit(1);
  Retail pblt;
  pblt.f("GPSM").f("PBLT").f("CNST").w(1).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == pblt.bytes && parts[0].dropped.empty(), "BLIT 1 is PBLT 1");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "a PART with PBLT passes the retail element reader");
  }
  parts = blit(0);
  CHECK_MSG(parts.size() == 1 && parts[0].part == none.bytes && parts[0].dropped.empty(),
            "BLIT 0, the default, writes nothing");

  parts = convert([](auto& o) {
    PutProperty(o, "SHTT", 1);
    PutConstant(o, 30);
  });
  Retail shtt;
  shtt.f("GPSM").f("PSHT").f("CNST").w(30).f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == shtt.bytes && parts[0].dropped.empty(), "SHTT CNST(30) is PSHT");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "a PART with PSHT passes the retail element reader");
  }

  parts = convert([](auto& o) {
    PutProperty(o, "PMOV", 3);
    PutFourCC(o, "PVEL");
    PutProperty(o, "PMPV", 3);
    PutFourCC(o, "CNST");
    PutConstant(o, Bits(0.f));
    PutConstant(o, Bits(0.4f));
    PutConstant(o, Bits(0.f));
  });
  Retail pivot;
  pivot.f("GPSM").f("PMOV").f("PVEL");
  pivot.f("PMPV").f("CNST").f("CNST").w(Bits(-0.f)).f("CNST").w(Bits(0.f)).f("CNST").w(Bits(0.4f));
  pivot.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == pivot.bytes && parts[0].dropped.empty(),
            "PMOV PVEL and PMPV CNST(0, 0.4, 0) are written in the port's space");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "a PART with PMOV and PMPV passes the retail element reader");
  }

  const auto pmov = [&](const char* flag, uint8_t value) {
    return convert([&](auto& o) {
      PutProperty(o, flag, 0);
      PutFourCC(o, "CNST");
      o.push_back(value);
      PutProperty(o, "PMOV", 3);
      PutFourCC(o, "PVEL");
    });
  };
  parts = pmov("PMRB", 1);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "PMOV under PMRB drops nothing");
  parts = pmov("EMTU", 1);
  CHECK_MSG(parts.size() == 1 &&
                std::count(parts[0].dropped.begin(), parts[0].dropped.end(), "PMOV: EMTU makes its up the system's") == 1,
            "PMOV with EMTU is dropped with its reason");
}

// INTS is the system scale in place of the external one (Compile 0x289af0): the port-only PINS,
// turned into the port's numbers by BSSC; EXTS inside it stays the external scale. XFMD 5 frees it.
void TestIntScale() {
  const auto withInts = [](int mode, bool exts) {
    return ConvertOne(OneGenerator([&](auto& o) {
                        PutProperty(o, "INTS", 3);
                        if (exts) {
                          PutFourCC(o, "MULT");
                          PutFourCC(o, "EXTS");
                        }
                        PutFourCC(o, "RTOV");
                        PutConstant(o, Bits(0.6f));
                        if (mode != 0) {
                          PutProperty(o, "XFMD", 0);
                          PutConstant(o, mode);
                        }
                        PutTexr(o, 0x1234ABCD);
                      }),
                      {});
  };
  auto parts = withInts(0, false);
  Retail want;
  want.f("GPSM").f("PINS").f("BSSC").f("RTOV").f("CNST").w(Bits(0.6f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes && parts[0].dropped.empty(), "INTS RTOV(0.6) is PINS");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "a PART with PINS passes the retail element reader");
  }
  parts = withInts(0, true);
  Retail scaled;
  scaled.f("GPSM").f("PINS").f("BSSC").f("MULT").f("BSSC").f("EXTS").f("RTOV").f("CNST").w(Bits(0.6f));
  scaled.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == scaled.bytes && parts[0].dropped.empty(),
            "INTS MULT(EXTS, RTOV(0.6)) reads the external scale");
  parts = withInts(5, false);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty() &&
                std::search(parts[0].part.begin(), parts[0].part.end(), want.bytes.begin() + 4,
                            want.bytes.begin() + 8) == parts[0].part.end(),
            "INTS under XFMD 5 writes no PINS");
}

bool Contains(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& what);

// INTR and INTT are the system orientation and translation in place of the external ones (Compile
// 0x289af0): the port-only PINR (an Euler vector, as PMRQ's) and PINT, read through EXTR/EXTT.
// XFMD 5 frees INTR and makes INTT each particle's spawn offset, retail's POFS.
void TestIntFrame() {
  const auto convert = [](int mode, const std::function<void(std::vector<uint8_t>&)>& body) {
    auto parts = ConvertOne(OneGenerator([&](auto& o) {
                              body(o);
                              if (mode != 0) {
                                PutProperty(o, "XFMD", 0);
                                PutConstant(o, mode);
                              }
                              PutTexr(o, 0x1234ABCD);
                            }),
                            {});
    return parts.size() == 1 ? parts[0] : ConvertedPart{};
  };
  const auto basis = [](Retail& r) { RetailVector(r, 90.f, 0.f, 180.f); };
  ConvertedPart part = convert(0, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutStaticEuler(o, -90.f, 0.f, 0.f);
  });
  Retail reul;
  reul.f("GPSM").f("PINR").f("PQAD");
  basis(reul);
  reul.f("PQAD");
  RetailVector(reul, -90.f, 0.f, 0.f);
  basis(reul);
  reul.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(part.part == reul.bytes && part.dropped.empty(), "INTR REUL(-90, 0, 0) is PINR B * R * B");
  std::vector<RetailPartProperty> properties;
  std::string error;
  CHECK_MSG(SplitRetailPart(part.part.data(), part.part.size(), properties, error),
            "a PART with PINR passes the retail element reader");

  part = convert(3, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutFourCC(o, "RADD");
    PutFourCC(o, "EXTR");
    PutStaticEuler(o, -15.f, 0.f, 0.f);
  });
  Retail radd;
  radd.f("PINR").f("PQAD");
  basis(radd);
  radd.f("PQAD").f("PQAD").f("EXTR");
  RetailVector(radd, -15.f, 0.f, 0.f);
  basis(radd);
  CHECK_MSG(Contains(part.part, radd.bytes) && part.dropped.empty(), "INTR RADD(EXTR, REUL) under XFMD 3 is PINR");

  part = convert(2, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutFourCC(o, "REUL");
    PutFourCC(o, "IRND");
    PutConstant(o, Bits(230.f));
    PutConstant(o, Bits(300.f));
    PutConstant(o, 0);
    PutConstant(o, 0);
    o.push_back(0);
  });
  // REUL(IRND(230, 300), 0, 0): the draw becomes a hidden init-only PSYR slot (one per system).
  CHECK_MSG(part.dropped.empty(), "INTR with IRND is no longer dropped");
  Retail head, slot;
  head.f("CNST").f("PSYR").w(10).f("CNST").w(Bits(0.f)).f("CNST").w(Bits(0.f));
  slot.f("PSYS").f("CNST").w(1).w(0).w(10).w(1).f("RAND").f("CNST").w(Bits(230.f)).f("CNST").w(Bits(300.f));
  CHECK_MSG(Contains(part.part, head.bytes), "INTR REUL(IRND, 0, 0) reads the hidden PSYR slot 10");
  CHECK_MSG(Contains(part.part, slot.bytes), "the hidden slot is an init-only RAND(230, 300) in PSYS");

  // KPIN(REUL(0, 0, 0)) is a constant identity: the KPIN is stripped, no slot.
  part = convert(2, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutFourCC(o, "KPIN");
    PutStaticEuler(o, 0.f, 0.f, 0.f);
  });
  Retail kconst;
  kconst.f("PINR").f("PQAD");
  basis(kconst);
  kconst.f("PQAD");
  RetailVector(kconst, 0.f, 0.f, 0.f);
  basis(kconst);
  CHECK_MSG(Contains(part.part, kconst.bytes) && part.dropped.empty() && !Contains(part.part, {'P', 'S', 'Y', 'S'}),
            "INTR KPIN(REUL(0, 0, 0)) is PINR CNST");

  // KPIN(REUL(SINE, 0, 0)): the non-constant angle goes through a hidden init-only slot; the
  // constant angles stay constants.
  part = convert(2, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutFourCC(o, "KPIN");
    PutFourCC(o, "REUL");
    PutFourCC(o, "SINE");
    PutConstant(o, Bits(1.f));
    PutConstant(o, Bits(2.f));
    PutConstant(o, Bits(3.f));
    PutConstant(o, 0);
    PutConstant(o, 0);
    o.push_back(0);
  });
  CHECK_MSG(part.dropped.empty() && Contains(part.part, {'P', 'S', 'Y', 'R'}) && Contains(part.part, {'P', 'S', 'Y', 'S'}),
            "INTR KPIN(REUL(non-constant, ...)) keeps the angle through a hidden init-only slot");

  // CMRT: the camera's rotation, converted as PINR PQAD(B, PQAD(CMRT, B)); never constant.
  part = convert(3, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutFourCC(o, "CMRT");
  });
  Retail cmrt;
  cmrt.f("PINR").f("PQAD");
  basis(cmrt);
  cmrt.f("PQAD").f("CMRT");
  basis(cmrt);
  CHECK_MSG(Contains(part.part, cmrt.bytes) && part.dropped.empty(), "INTR CMRT is PINR B * CMRT * B");

  part = convert(5, [](auto& o) {
    PutProperty(o, "INTR", 3);
    PutStaticEuler(o, -90.f, 0.f, 0.f);
  });
  CHECK_MSG(part.dropped.empty() && !Contains(part.part, {'P', 'I', 'N', 'R'}), "INTR under XFMD 5 is freed");

  part = convert(3, [](auto& o) {
    PutProperty(o, "INTT", 3);
    PutFourCC(o, "ADD_");
    PutFourCC(o, "EXTT");
    PutFourCC(o, "CNST");
    PutConstant(o, 0);
    PutConstant(o, Bits(0.75f));
    PutConstant(o, 0);
  });
  Retail add;
  add.f("PINT").f("BSIS").f("ADD_").f("BSIS").f("EXTT");
  RetailVector(add, 0.f, 0.75f, 0.f);
  CHECK_MSG(Contains(part.part, add.bytes) && part.dropped.empty(), "INTT ADD_(EXTT, c) under XFMD 3 is PINT");
  CHECK_MSG(SplitRetailPart(part.part.data(), part.part.size(), properties, error),
            "a PART with PINT passes the retail element reader");

  const auto constant = [](auto& o) {
    PutProperty(o, "INTT", 3);
    PutFourCC(o, "CNST");
    PutConstant(o, 0);
    PutConstant(o, Bits(0.4f));
    PutConstant(o, Bits(0.1f));
  };
  Retail pofs;
  pofs.f("POFS").f("CNST").f("CNST").w(0x80000000u).f("CNST").w(Bits(0.1f)).f("CNST").w(Bits(0.4f));
  for (int mode : {2, 3}) {
    part = convert(mode, constant);
    CHECK_MSG(Contains(part.part, pofs.bytes) && part.dropped.empty() && !Contains(part.part, {'P', 'I', 'N', 'T'}),
              "a constant INTT under XFMD 2/3 is the retail POFS");
  }
  part = convert(2, [&](auto& o) {
    constant(o);
    PutProperty(o, "POFS", 3);
    PutFourCC(o, "CNST");
    PutConstant(o, 0);
    PutConstant(o, Bits(1.f));
    PutConstant(o, 0);
  });
  Retail both;
  both.f("POFS").f("BSIS").f("ADD_");
  CHECK_MSG(Contains(part.part, both.bytes) && part.dropped.empty() && !Contains(part.part, {'P', 'I', 'N', 'T'}),
            "a constant INTT and a POFS under XFMD 2 are added");
  part = convert(5, constant);
  CHECK_MSG(Contains(part.part, pofs.bytes) && part.dropped.empty() && !Contains(part.part, {'P', 'I', 'N', 'T'}),
            "INTT under XFMD 5 is the spawn offset POFS");
}

bool HasPmrf(const ConvertedPart& part, uint32_t frame) {
  Retail pmrf;
  pmrf.f("PMRF").f("CNST").w(frame);
  return std::search(part.part.begin(), part.part.end(), pmrf.bytes.begin(), pmrf.bytes.end()) != part.part.end();
}

// MPCB(R) is qB * R * qB (0x29d69c): at the top level it cancels the B conjugation, nested it
// keeps one. EXTR is the system orientation. PMRB and PMCF pick the PMRQ frame (PMRF).
void TestModelRotationFrames() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "MPCB");
                 PutStaticEuler(o, 10.f, 20.f, 30.f);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail top;
  top.f("GPSM").f("PMRT");
  RetailVector(top, 10.f, 20.f, 30.f);
  top.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == top.bytes, "a top-level MPCB PMRQ is its rotation, unconjugated");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty(), "a top-level MPCB PMRQ is exact");

  parts = ConvertOne(OneGenerator([](auto& o) {
            PutProperty(o, "PMRQ", 3);
            PutFourCC(o, "RADD");
            PutFourCC(o, "EXTR");
            PutFourCC(o, "MPCB");
            PutStaticEuler(o, 10.f, 20.f, 30.f);
            PutTexr(o, 0x1234ABCD);
          }),
          {});
  Retail nested;
  nested.f("GPSM").f("PMRT").f("PQAD");
  RetailVector(nested, 90.f, 0.f, 180.f);
  nested.f("PQAD").f("PQAD").f("EXTR").f("PQAD");
  RetailVector(nested, 90.f, 0.f, 180.f);
  nested.f("PQAD");
  RetailVector(nested, 10.f, 20.f, 30.f);
  RetailVector(nested, 90.f, 0.f, 180.f);
  RetailVector(nested, 90.f, 0.f, 180.f);
  nested.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == nested.bytes, "RADD(EXTR, MPCB(R)) keeps the nested MPCB's B conjugation");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty(), "RADD(EXTR, MPCB(R)) is exact");

  const auto framed = [](const char* flag, int mode) {
    return ConvertOne(OneGenerator([&](auto& o) {
                        PutProperty(o, "PMRQ", 3);
                        PutStaticEuler(o, 10.f, 20.f, 30.f);
                        if (flag != nullptr) {
                          PutProperty(o, flag, 0);
                          PutConstant(o, 1);
                        }
                        if (mode != 0) {
                          PutProperty(o, "XFMD", 0);
                          PutConstant(o, mode);
                        }
                        PutTexr(o, 0x1234ABCD);
                      }),
                      {});
  };
  parts = framed(nullptr, 3);
  CHECK_MSG(parts.size() == 1 && !HasPmrf(parts[0], 1) && !HasPmrf(parts[0], 2), "no PMRB/PMCF writes no PMRF");
  parts = framed("PMRB", 3);
  CHECK_MSG(parts.size() == 1 && HasPmrf(parts[0], 1) && parts[0].dropped.empty(), "PMRB is the PMRQ alone in any mode");
  parts = framed("PMCF", 0);
  CHECK_MSG(parts.size() == 1 && HasPmrf(parts[0], 1) && parts[0].dropped.empty(), "PMCF under XFMD 2 is the PMRQ alone");
  parts = framed("PMCF", 4);
  CHECK_MSG(parts.size() == 1 && HasPmrf(parts[0], 2) && parts[0].dropped.empty(),
            "PMCF under XFMD 4 keeps the system orientation");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
              "a PART with PMRF passes the retail element reader");
  }
  parts = framed("PMCF", 5);
  CHECK_MSG(parts.size() == 1 && !HasPmrf(parts[0], 1) && !HasPmrf(parts[0], 2) && !parts[0].dropped.empty() &&
                parts[0].dropped[0] == "PMRQ: PMRB or PMCF under XFMD 5",
            "PMCF under XFMD 5 is dropped");
}

ConvertedPart ConvertStaticModelRotation(const std::vector<CVector3f>& eulers) {
  const std::vector<ConvertedPart> parts = ConvertOne(OneGenerator([&](auto& out) {
    PutProperty(out, "PMRQ", 3);
    if (eulers.size() == 1) {
      PutStaticEuler(out, eulers[0].GetX(), eulers[0].GetY(), eulers[0].GetZ());
    } else {
      PutFourCC(out, "RADD");
      for (const CVector3f& euler : eulers) {
        PutStaticEuler(out, euler.GetX(), euler.GetY(), euler.GetZ());
      }
    }
    PutTexr(out, 0x1234ABCD);
  }), {});
  CHECK_MSG(parts.size() == 1, "static model rotation converts to one PART");
  if (parts.empty()) {
    return {};
  }
  CHECK_MSG(parts[0].dropped.empty() && parts[0].approximated.empty(),
            "supported static model rotation is neither dropped nor approximated");
  return parts[0];
}

CMatrix3f EvaluateRotation(const ConvertedPart& part) {
  std::unique_ptr<CVectorElement> rotation = ReadConvertedRotation(part);
  if (!rotation) {
    return CMatrix3f::Identity();
  }
  CVector3f euler = CVector3f::Zero();
  rotation->GetValue(0, euler);
  return EulerMatrix(euler);
}

void TestModelRotationBasisEvaluation() {
  const CMatrix3f basis(-1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f);
  const CVector3f asymmetric(25.f, 40.f, 70.f);
  const CMatrix3f asymmetricExpected = basis * EulerMatrix(asymmetric) * basis.GetTranspose();
  const CMatrix3f asymmetricActual = EvaluateRotation(ConvertStaticModelRotation({asymmetric}));
  CHECK_MSG(SameMatrix(asymmetricActual, asymmetricExpected),
            "serialized direct REUL evaluates to B * R * B-transpose for an asymmetric triple");

  const CVector3f first(90.f, 0.f, 0.f);
  const CVector3f second(0.f, 90.f, 0.f);
  const CMatrix3f orderedExpected = basis * EulerMatrix(first) * EulerMatrix(second) * basis.GetTranspose();
  const CMatrix3f orderedActual = EvaluateRotation(ConvertStaticModelRotation({first, second}));
  const CMatrix3f reversed = basis * EulerMatrix(second) * EulerMatrix(first) * basis.GetTranspose();
  CHECK_MSG(SameMatrix(orderedActual, orderedExpected),
            "serialized RADD evaluates in source operand order before basis conjugation");
  CHECK_MSG(!SameMatrix(orderedActual, reversed, 0.1f),
            "noncommuting RADD operands still distinguish reversed composition");

  for (const float y : {90.f, -90.f, 89.9999f, -89.9999f}) {
    const CVector3f gimbal(37.f, y, -71.f);
    const CMatrix3f expected = basis * EulerMatrix(gimbal) * basis.GetTranspose();
    const CMatrix3f actual = EvaluateRotation(ConvertStaticModelRotation({gimbal}));
    CHECK_MSG(SameMatrix(actual, expected, 8.e-5f),
              "serialized basis conjugation preserves exact and near-gimbal orientations");
  }

  const CVector3f convertedPlaneNormal = basis * CVector3f(0.f, 0.f, 1.f);
  for (int angle = 0; angle < 360; ++angle) {
    const CVector3f spin(90.f, float(angle), 0.f);
    const CMatrix3f rotation = EvaluateRotation(ConvertStaticModelRotation({spin}));
    const CVector3f normal = rotation * convertedPlaneNormal;
    CHECK_MSG(std::fabs(normal.GetX()) < 8.e-5f && std::fabs(normal.GetY()) < 8.e-5f &&
                  std::fabs(normal.GetZ() + 1.f) < 8.e-5f,
              "BombExplo source spin keeps the converted ring normal at (0,0,-1)");
  }
  const CVector3f oldTilt = EulerMatrix(CVector3f(90.f, 90.f, 0.f)) * convertedPlaneNormal;
  CHECK_MSG(std::fabs(oldTilt.GetX()) > 0.5f || std::fabs(oldTilt.GetZ() + 1.f) > 0.5f,
            "unconverted source Euler tuple fails the BombExplo plane-normal invariant");
}

// A MATI with one shader and its texture parameters, as ParseMati reads it.
std::vector<uint8_t> Mati(uint32_t shader, const std::vector<std::pair<const char*, EffectGuid>>& textures) {
  std::vector<uint8_t> out(0x6d, 0);
  for (int i = 0; i < 4; ++i) {
    out[0x48 + size_t(i)] = uint8_t(shader >> (24 - 8 * i));
    out[0x61 + size_t(i)] = 0xff;  // EBlendMode -1: the generator's own blend row
  }
  const uint32_t count = uint32_t(textures.size());
  for (int i = 0; i < 4; ++i) {
    out[0x69 + size_t(i)] = uint8_t(count >> (8 * i));
  }
  for (const auto& texture : textures) {
    out.push_back(6);
    out.insert(out.end(), texture.first, texture.first + 4);
    PutGuid(out, texture.second);
    Put32(out, 0);                // texCoord
    Put32(out, 1);                // filter
    Put32(out, 1);                // wrapX
    Put32(out, 1);                // wrapY
    Put32(out, 0xffffffffu);      // wrapZ
  }
  return out;
}

// A root generator whose material is `Fresh(7)`, with a blend mode and sprite centre.
std::vector<uint8_t> VmatEffect(bool model = false, const EffectGuid& material = Fresh(7), uint32_t pbdm = 2,
                                float sftd = 0) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 4);
  PutProperty(out, "PBDM", 1);
  PutConstant(out, pbdm);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, material);
  if (model) {
    PutProperty(out, "PMDL", 0);
    PutGuid(out, Legacy(0x1234ABCD));
  }
  if (sftd > 0) {
    PutProperty(out, "SFTD", 3);
    PutConstant(out, Bits(sftd));
  }
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

// SFTD goes into the VMAT (its depthSoften), so it is reported only when no VMAT comes out.
void TestSftdReport() {
  const std::vector<uint8_t> data = VmatEffect(false, Fresh(7), 2, 3.0f);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "sftd effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(8)}}); };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  auto dropped = [](const ConvertedPart& part, const std::string& line) {
    return std::find(part.dropped.begin(), part.dropped.end(), line) != part.dropped.end();
  };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1, "one sftd part");
  if (parts.size() == 1) {
    CHECK_MSG(!dropped(parts[0], "SFTD: Remastered only") && !dropped(parts[0], "SFTD: no VMAT"),
              "an SFTD the VMAT takes is not reported dropped");
  }
  io.materialData = [](const EffectGuid&) { return Mati(0x12345678, {}); };
  parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1 && dropped(parts[0], "SFTD: no VMAT"), "without a VMAT, SFTD is dropped as no VMAT");
}

// A model particle with a VMAT carries its converted model as VMSH, when the import has the mesh.
void TestVmsh() {
  const std::vector<uint8_t> data = VmatEffect(true);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "vmsh effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(8)}}); };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  const std::vector<uint8_t> blob = {0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 1, 2, 3, 4};  // not parsed here
  io.modelMesh = [&blob](uint32_t model) { return model == 0x1234ABCD ? blob : std::vector<uint8_t>(); };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1, "one part with a model");
  if (parts.size() != 1) {
    return;
  }
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "vmsh part reads as retail");
  const RetailPartProperty* vmsh = nullptr;
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMSH")) {
      vmsh = &property;
    }
  }
  CHECK_MSG(vmsh != nullptr, "a model particle with a VMAT writes VMSH");
  if (vmsh != nullptr) {
    const std::vector<uint8_t>& v = vmsh->value;
    CHECK_MSG(v.size() == 8 + blob.size() && std::memcmp(v.data(), "CNST", 4) == 0 && v[7] == blob.size() &&
                  std::equal(blob.begin(), blob.end(), v.begin() + 8),
              "VMSH is CNST, the length and the blob");
  }
  // A model the import has no mesh for (a disc model) gets no VMSH.
  io.modelMesh = [](uint32_t) { return std::vector<uint8_t>(); };
  parts = ConvertEffect(effect, data.data(), io);
  properties.clear();
  if (parts.size() == 1 && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
    for (const RetailPartProperty& property : properties) {
      CHECK_MSG(property.fourcc != EffectFourCC("VMSH"), "no VMSH for a model without a mesh");
    }
  }
}

// A recipe shader's MATI becomes a VMAT, and the port-only properties survive a retail split.
void TestVmat() {
  const std::vector<uint8_t> data = VmatEffect();
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "vmat effect parses");

  int imports = 0;
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [](const EffectGuid& material) {
    return material == Fresh(7) ? Mati(0x461071b8, {{"TCH0", Fresh(8)}}) : std::vector<uint8_t>();
  };
  io.vfxTexture = [&imports](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) {
      ++imports;
      atlas.id = 0x5EED0008;
    }
    return atlas;
  };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  CHECK_MSG(imports == 1, "the ramp texture is imported once");
  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "vmat part reads as retail");
  const RetailPartProperty* vmat = nullptr;
  bool vorn = false;
  bool pirn = false;
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMAT")) {
      vmat = &property;
    }
    vorn = vorn || property.fourcc == EffectFourCC("VORN");
    pirn = pirn || property.fourcc == EffectFourCC("PIRN");
  }
  CHECK_MSG(vmat != nullptr, "a 461071b8 material writes VMAT");
  CHECK_MSG(vorn, "VORN is always written");
  CHECK_MSG(pirn, "PIRN marks every converted PART");
  if (vmat != nullptr) {
    // CNST + u32 size, then the blob: version 2, features (Ramp = 8), blend 2, one texture.
    const std::vector<uint8_t>& v = vmat->value;
    auto be32 = [&v](size_t at) { return at + 4 <= v.size() ? uint32_t(v[at]) << 24 | v[at + 1] << 16 | v[at + 2] << 8 | v[at + 3] : ~0u; };
    CHECK_MSG(v.size() > 24 && std::memcmp(v.data(), "CNST", 4) == 0, "VMAT is a constant blob");
    CHECK_MSG(be32(8) == 2 && be32(12) == 8 && be32(16) == 2 && be32(20) == 1, "VMAT v2, Ramp, blend 2, one texture");
    CHECK_MSG(be32(24) == 0x5EED0008, "the texture is the imported id");
  }
  CHECK_MSG(parts[0].approximated.empty(), "a recipe material approximates nothing");

  // A shader with no recipe: no VMAT, and a note.
  io.materialData = [](const EffectGuid&) { return Mati(0x12345678, {}); };
  parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1, "one part without a recipe");
  if (parts.size() == 1) {
    bool note = false;
    for (const std::string& line : parts[0].approximated) {
      note = note || line == "VMAT: shader 12345678 has no recipe";
    }
    CHECK_MSG(note, "an unknown shader is noted as having no recipe");
    properties.clear();
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "fallback part reads");
    for (const RetailPartProperty& property : properties) {
      CHECK_MSG(property.fourcc != EffectFourCC("VMAT"), "no VMAT without a recipe");
    }
  }

  // A texture the import cannot write: no VMAT either.
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(9)}}); };
  parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1, "one part when the texture fails");
  if (parts.size() == 1) {
    properties.clear();
    CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "failed-texture part reads");
    for (const RetailPartProperty& property : properties) {
      CHECK_MSG(property.fourcc != EffectFourCC("VMAT"), "no VMAT when a slot texture is not written");
    }
    CHECK_MSG(!parts[0].approximated.empty(), "the missing texture is noted");
  }
}

// A MATI with a CCH0 vector (type 3) after its textures.
std::vector<uint8_t> MatiCch0(uint32_t shader, const std::vector<std::pair<const char*, EffectGuid>>& textures,
                               float x, float y, float z, float w) {
  std::vector<uint8_t> out = Mati(shader, textures);
  out[0x69] += 1;  // one more entry: the CCH0 vector (the counts here are small)
  out.push_back(3);
  out.insert(out.end(), {'C', 'C', 'H', '0'});
  Put32(out, Bits(x));
  Put32(out, Bits(y));
  Put32(out, Bits(z));
  Put32(out, Bits(w));
  return out;
}

// A MATI with `cch` as its CCH0..n-1 vec4s (type 4) after its textures.
std::vector<uint8_t> MatiCchN(uint32_t shader, const std::vector<std::pair<const char*, EffectGuid>>& textures,
                              const std::vector<std::array<float, 4>>& cch) {
  std::vector<uint8_t> out = Mati(shader, textures);
  out[0x69] += uint8_t(cch.size());  // the counts here are small: the first byte is the whole count
  for (size_t n = 0; n < cch.size(); ++n) {
    out.push_back(4);
    out.insert(out.end(), {'C', 'C', 'H', uint8_t('0' + n)});
    for (int c = 0; c < 4; ++c) {
      Put32(out, Bits(cch[n][size_t(c)]));
    }
  }
  return out;
}

// Converts VmatEffect with `mati` as its material, returning the parts and the
// VMAT value (empty when no VMAT was written).
std::pair<std::vector<ConvertedPart>, std::vector<uint8_t>> ConvertVmat(const std::vector<uint8_t>& mati) {
  const std::vector<uint8_t> data = VmatEffect();
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: vmat effect does not parse: %s\n", error.c_str());
    port_test::CountFailure();
    return {};
  }
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) {
      atlas.id = 0x5EED0008;
    }
    return atlas;
  };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  std::vector<uint8_t> blob;
  if (parts.size() == 1) {
    std::vector<RetailPartProperty> properties;
    if (SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        if (property.fourcc == EffectFourCC("VMAT")) {
          blob = property.value;
        }
      }
    }
  }
  return {parts, blob};
}

uint32_t VmatWord(const std::vector<uint8_t>& v, size_t at) {
  return at + 4 <= v.size() ? uint32_t(v[at]) << 24 | v[at + 1] << 16 | v[at + 2] << 8 | v[at + 3] : ~0u;
}

struct SmtrTestEntry {
  const char* parameter;
  uint8_t component;
  float value;
};

size_t PutSmtrItem(std::vector<uint8_t>& out, const SmtrTestEntry& entry) {
  PutConstant(out, Bits(entry.value));
  const size_t parameterAt = out.size();
  PutFourCC(out, entry.parameter);
  out.push_back(0);  // reserved
  out.push_back(entry.component);
  return parameterAt;
}

std::vector<uint8_t> SmtrVmatEffect(size_t& cch0BytesAt) {
  std::vector<uint8_t> data = VmatEffect();
  const std::vector<SmtrTestEntry> entries = {
      {"CCH0", 0, 0.25f}, {"CCH0", 1, 0.5f}, {"CCH0", 2, 0.75f}, {"NOPE", 0, 0.875f}};
  std::vector<uint8_t> smtr;
  PutProperty(smtr, "SMTR", 0);
  smtr.push_back(0);  // SMTR version
  smtr.push_back(uint8_t(entries.size()));
  smtr.push_back(uint8_t(entries.size()));  // group 0 item count
  cch0BytesAt = size_t(-1);
  for (const SmtrTestEntry& entry : entries) {
    const size_t parameterAt = PutSmtrItem(smtr, entry);
    if (cch0BytesAt == size_t(-1)) {
      cch0BytesAt = data.size() - 13 + parameterAt;
    }
  }
  for (int group = 1; group < 5; ++group) {
    smtr.push_back(0);
  }
  data.insert(data.end() - 13, smtr.begin(), smtr.end());  // before _END, child count and FOOT
  return data;
}

void TestSmtrCch0Overrides() {
  size_t cch0BytesAt = 0;
  const std::vector<uint8_t> data = SmtrVmatEffect(cch0BytesAt);
  CHECK_MSG(cch0BytesAt + 4 <= data.size() && data[cch0BytesAt] == 0x30 && data[cch0BytesAt + 1] == 0x48 &&
                data[cch0BytesAt + 2] == 0x43 && data[cch0BytesAt + 3] == 0x43,
            "SMTR fixture serializes CCH0 as source bytes 30 48 43 43");

  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "raw SMTR effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  const auto smtr = std::find_if(effect.properties.begin(), effect.properties.end(),
                                 [](const EffectProperty& property) {
                                   return property.fourcc == EffectFourCC("SMTR");
                                 });
  CHECK_MSG(smtr != effect.properties.end() && smtr->value.size() == 4, "parser retains all raw SMTR entries");
  if (smtr != effect.properties.end() && smtr->value.size() == 4) {
    CHECK_MSG(smtr->value[0].word == EffectFourCC("CCH0"), "raw CCH0 bytes parse to the canonical FourCC");
    CHECK_MSG(smtr->value[3].word != EffectFourCC("CCH0"), "unknown SMTR id remains distinct");
  }

  const EffectGuid texture0 = Fresh(8), texture1 = Fresh(9), texture2 = Fresh(10);
  const std::vector<uint8_t> mati = MatiCch0(0x8e6bfa33,
      {{"TCH0", texture0}, {"TCH1", texture1}, {"TCH2", texture2}}, 0.0f, 0.0f, 0.0f, 0.0f);
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
  io.materialData = [&mati](const EffectGuid& material) {
    return material == Fresh(7) ? mati : std::vector<uint8_t>();
  };
  io.vfxTexture = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) atlas.id = 0x5EED0008;
    if (texture == Fresh(9)) atlas.id = 0x5EED0009;
    if (texture == Fresh(10)) atlas.id = 0x5EED000A;
    return atlas;
  };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  CHECK_MSG(parts.size() == 1, "raw SMTR converts through ConvertEffect");
  if (parts.size() != 1) {
    return;
  }

  std::vector<RetailPartProperty> properties;
  CHECK_MSG(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
            "SMTR output reads as retail PART");
  const auto vsmt = std::find_if(properties.begin(), properties.end(),
                                 [](const RetailPartProperty& property) {
                                   return property.fourcc == EffectFourCC("VSMT");
                                 });
  std::vector<uint8_t> expected;
  const auto appendBe32 = [&expected](uint32_t word) {
    expected.push_back(uint8_t(word >> 24));
    expected.push_back(uint8_t(word >> 16));
    expected.push_back(uint8_t(word >> 8));
    expected.push_back(uint8_t(word));
  };
  expected.insert(expected.end(), {'C', 'N', 'S', 'T'});
  appendBe32(3);
  const std::array<std::pair<uint32_t, float>, 3> overrides = {{{3, 0.25f}, {11, 0.5f}, {12, 0.75f}}};
  for (const auto& [target, value] : overrides) {
    appendBe32(target);
    appendBe32(0);  // real element kind
    expected.insert(expected.end(), {'C', 'N', 'S', 'T'});
    appendBe32(Bits(value));
  }
  CHECK_MSG(vsmt != properties.end() && vsmt->value == expected,
            "CCH0 x maps to the Src override and y/z map to TCH0 warp overrides");
  if (vsmt == properties.end() || vsmt->value != expected) {
    std::fprintf(stderr, "  VSMT expected %zu bytes, got %zu\n", expected.size(),
                 vsmt == properties.end() ? 0 : vsmt->value.size());
    if (vsmt != properties.end()) {
      for (uint8_t byte : vsmt->value) std::fprintf(stderr, "%02x", byte);
      std::fprintf(stderr, "\n");
    }
    for (const std::string& dropped : parts[0].dropped) std::fprintf(stderr, "  dropped: %s\n", dropped.c_str());
  }
  CHECK_MSG(std::find(parts[0].dropped.begin(), parts[0].dropped.end(), "SMTR: a parameter VSMT cannot take") !=
                parts[0].dropped.end(),
            "unknown SMTR parameter id is rejected");

  EffectNode withoutSmtr = effect;
  withoutSmtr.properties.erase(std::remove_if(withoutSmtr.properties.begin(), withoutSmtr.properties.end(),
      [](const EffectProperty& property) { return property.fourcc == EffectFourCC("SMTR"); }),
      withoutSmtr.properties.end());
  const std::vector<ConvertedPart> baseline = ConvertEffect(withoutSmtr, data.data(), io);
  std::vector<RetailPartProperty> baselineProperties;
  CHECK_MSG(baseline.size() == 1 &&
                SplitRetailPart(baseline[0].part.data(), baseline[0].part.size(), baselineProperties, error),
            "SMTR-free baseline converts");
  const auto withoutVsmt = [](std::vector<RetailPartProperty> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](const RetailPartProperty& property) {
                   return property.fourcc == EffectFourCC("VSMT");
                 }), values.end());
    return values;
  };
  const auto convertedBase = withoutVsmt(properties);
  const auto expectedBase = withoutVsmt(baselineProperties);
  bool nonOverrideBytesMatch = convertedBase.size() == expectedBase.size();
  for (size_t i = 0; nonOverrideBytesMatch && i < convertedBase.size(); ++i) {
    nonOverrideBytesMatch = convertedBase[i].fourcc == expectedBase[i].fourcc &&
                            convertedBase[i].value == expectedBase[i].value;
  }
  CHECK_MSG(nonOverrideBytesMatch, "CCH0 conversion changes only the VSMT override property");
}

// The wave orb's exact MATI (MTIN 756533fc-e832-48f2-b1ef-ebb41fcea14f, little-endian groups) and its
// header blend (EBlendMode, the LE u32 at 0x61).
const EffectGuid kWaveMati = {0xfc, 0x33, 0x65, 0x75, 0x32, 0xe8, 0xf2, 0x48,
                              0xb1, 0xef, 0xeb, 0xb4, 0x1f, 0xce, 0xa1, 0x4f};
// aurora::gfx::vfx::AlreadyExposed in a VMAT features word.
constexpr uint32_t kAlreadyExposedBit = 1u << 31;

// PowerBall source fixture IDs, stored as little-endian UUIDs by GENP/MATI.
const EffectGuid kPowerBallChild = {0x82, 0x07, 0x87, 0xae, 0xf4, 0x41, 0x26, 0x4c,
                                    0x88, 0x76, 0xc2, 0xf0, 0x70, 0xcf, 0x1e, 0x06};
const EffectGuid kPowerBallMati = {0x46, 0x2a, 0x75, 0x86, 0x33, 0xe7, 0x23, 0x4d,
                                   0xad, 0xc8, 0xc7, 0x4f, 0x28, 0x15, 0x80, 0xec};
const EffectGuid kPowerBallModel = {0x60, 0xae, 0xf6, 0x4b, 0xe5, 0x3b, 0x51, 0x4d,
                                    0x88, 0xbd, 0x50, 0xbb, 0xd1, 0xbd, 0xf6, 0x71};
const EffectGuid kPowerBallRamp = {0x4f, 0xb8, 0xea, 0xe1, 0xa1, 0x69, 0x03, 0x40,
                                   0x98, 0x6a, 0xed, 0xf9, 0xbd, 0x1e, 0x5b, 0xfe};
const EffectGuid kPowerBallIndirect = {0x79, 0x40, 0x9b, 0xb8, 0x4b, 0x2b, 0x39, 0x49,
                                       0xa2, 0x49, 0xe7, 0x36, 0x09, 0x7f, 0x3a, 0xf0};

// Converts a VmatEffect (PBDM `pbdm`, 0 by default) whose material is `material` and returns the VMAT blob's blend word
// (or the word at `word`: 12 is the features), or ~0u when no VMAT came out.
uint32_t ConvertBlend(std::vector<uint8_t> mati, const EffectGuid& material, bool model, uint32_t pbdm = 0,
                      size_t word = 16) {
  const std::vector<uint8_t> data = VmatEffect(model, material, pbdm);
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: wave blend effect does not parse: %s\n", error.c_str());
    port_test::CountFailure();
    return ~0u;
  }
  EffectConvertIO io;
  io.materialTexture = [&material](const EffectGuid& id) { return id == material ? 0x5EED0007u : 0u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  io.modelMesh = [](uint32_t) { return std::vector<uint8_t>(); };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  std::vector<RetailPartProperty> properties;
  if (parts.size() != 1 || !SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
    return ~0u;
  }
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMAT")) {
      return VmatWord(property.value, word);
    }
  }
  return ~0u;
}

// A swoosh on the orb's exact MATI keeps SBDM: the override is for model particles only.
void TestWaveOrbSwoosh(const std::vector<uint8_t>& mati) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 3);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0000AAAA));
  PutFourCC(out, "SWSH");
  PutProperty(out, "LENG", 1);
  PutConstant(out, 8);
  PutProperty(out, "SBDM", 1);
  PutConstant(out, 0);
  PutProperty(out, "PMDL", 0);
  PutGuid(out, Legacy(0x1234ABCD));
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, kWaveMati);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "orb swoosh effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& id) { return id == kWaveMati ? 0x5EED0007u : 0u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), io);
  uint32_t blend = ~0u, features = ~0u;
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    if (part.type == EffectFourCC("SWHC") && SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        if (property.fourcc == EffectFourCC("VMAT")) {
          blend = VmatWord(property.value, 16);
          features = VmatWord(property.value, 12);
        }
      }
    }
  }
  // A swoosh is not a model: BindGraphicsState takes the MATI's EBlendMode 1 (One/One) over SBDM, and the
  // fog stays SBDM's (alpha, fogged).
  CHECK_MSG(blend == 5, "a swoosh on the orb's MATI takes its EBlendMode 1 (AdditiveOne) over SBDM");
  CHECK_MSG(features != ~0u && (features & (1u << 27)) != 0, "the swoosh keeps SBDM's fog under the MATI's factors");
  CHECK_MSG(features != ~0u && (features & kAlreadyExposedBit) == 0, "a swoosh on the orb's MATI keeps the room's exposure");
}

// The exposure policy (aurora::gfx::vfx::AlreadyExposed, the features word's bit 31) is the orb's exact
// gate only, tested on its own: it is not read off the blend.
void TestWaveOrbExposurePolicy() {
  auto orb = [](uint32_t shader, uint32_t headerBlend) {
    std::vector<uint8_t> mati = Mati(shader, {{shader == 0x642dd72b ? "BCLR" : "TCH0", Fresh(8)}});
    for (int i = 0; i < 4; ++i) {
      mati[0x61 + size_t(i)] = uint8_t(headerBlend >> (8 * i));
    }
    return mati;
  };
  const uint32_t flagged = ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true, 0, 12);
  const uint32_t plain = ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true, 0, 12);
  CHECK_MSG(flagged != ~0u && (flagged & kAlreadyExposedBit) != 0, "wave orb MATI + PMDL + 642dd72b + header 1: AlreadyExposed");
  CHECK_MSG(plain != ~0u && (flagged & ~kAlreadyExposedBit) == plain,
            "the policy bit is the only feature difference (the shader features are the recipe's)");
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true, 2, 12) == flagged, "the policy holds whatever the PBDM");
  const auto unflagged = [](uint32_t f) { return f != ~0u && (f & kAlreadyExposedBit) == 0; };
  CHECK_MSG(unflagged(plain), "near miss: another MATI guid has no policy");
  CHECK_MSG(unflagged(ConvertBlend(orb(0x461071b8, 1), kWaveMati, true, 0, 12)), "near miss: another shader has no policy");
  CHECK_MSG(unflagged(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, false, 0, 12)), "near miss: no PMDL has no policy");
  CHECK_MSG(unflagged(ConvertBlend(orb(0x642dd72b, 0), kWaveMati, true, 0, 12)), "near miss: header blend 0 has no policy");
  CHECK_MSG(unflagged(ConvertBlend(orb(0x642dd72b, 2), kWaveMati, true, 0, 12)), "near miss: header blend 2 has no policy");
  for (uint32_t pbdm = 0; pbdm <= 3; ++pbdm) {
    CHECK_MSG(unflagged(ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true, pbdm, 12)), "no policy from any PBDM alone");
  }
}

std::vector<uint8_t> PowerBallMati(uint32_t shader = 0xe899b1df, uint32_t headerBlend = ~0u) {
  std::vector<uint8_t> mati = MatiCch0(shader, {{"TCH0", kPowerBallRamp}, {"TCH1", kPowerBallIndirect}},
                                       0.5f, 0.5f, 1.0f, 0.0f);
  for (int i = 0; i < 4; ++i) {
    mati[0x61 + size_t(i)] = uint8_t(headerBlend >> (8 * i));
  }
  return mati;
}

void TestPowerOpacityGainBias() {
  // Non-unit MATI defaults must survive without any SMTR; no other recipe changes format.
  const auto mati = MatiCch0(0xe899b1df, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}},
                             0.5f, 0.5f, 2.f, 0.25f);
  const auto base = ConvertVmat(mati);
  const auto& blob = base.second;
  CHECK_MSG(!blob.empty() && VmatWord(blob, 8) == 3, "e899b1df emits VMAT v3 with non-unit defaults");
  CHECK_MSG(blob.size() >= 8 && VmatWord(blob, blob.size() - 8) == Bits(2.f) &&
            VmatWord(blob, blob.size() - 4) == Bits(0.25f), "v3 appends BE CCH0.z/w gain/bias");
  const auto neutral = ConvertVmat(MatiCch0(0xe899b1df, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}},
                                           0.5f, 0.5f, 1.f, 0.f));
  CHECK_MSG(blob.size() == neutral.second.size() && blob.size() >= 8 &&
            std::equal(blob.begin(), blob.end() - 8, neutral.second.begin()),
        "changing gain/bias leaves the entire v2 prefix, sources and warp scales alone");

  size_t ignored = 0;
  const auto data = SmtrVmatEffect(ignored);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "opacity SMTR fixture parses");
  for (auto& prop : effect.properties) {
    if (prop.fourcc == EffectFourCC("SMTR")) {
      auto bias = prop.value[2];
      bias.args[1].word = (bias.args[1].word & ~0xff00u) | (3u << 8);
      bias.args[0].args[0].word = Bits(0.2f);
      prop.value.push_back(bias);
    }
  }
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid&) { FlipbookAtlas a; a.id = 0x5EED0008; return a; };
  const auto parts = ConvertEffect(effect, data.data(), io);
  std::vector<RetailPartProperty> props;
  CHECK_MSG(parts.size() == 1 && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), props, error),
        "opacity overrides convert and split");
  bool found = false;
  for (const auto& prop : props) {
    if (prop.fourcc == EffectFourCC("VMAT")) {
      CHECK_MSG(prop.value == blob, "SMTR does not replace MATI defaults");
    } else if (prop.fourcc == EffectFourCC("VSMT")) {
      // Each real constant entry is target, kind, CNST, float (16 bytes).
      found = true;
      CHECK_MSG(VmatWord(prop.value, 4) == 4, "xy warp plus gain/bias produce four override targets");
      const uint32_t targets[] = {11, 12, 19, 20};
      const float values[] = {0.25f, 0.5f, 0.75f, 0.2f};
      for (size_t i = 0; i < 4; ++i) {
        CHECK_MSG(VmatWord(prop.value, 8 + i * 16) == targets[i] &&
                  VmatWord(prop.value, 20 + i * 16) == Bits(values[i]), "VSMT numbering and CCH0 values preserved");
      }
    }
  }
  CHECK_MSG(found, "e899b1df gain/bias emit VSMT");
  // A time-scaled real is not folded into the MATI default or discarded. The runtime
  // must receive the complete element tree at the new target.
  for (auto& prop : effect.properties) {
    if (prop.fourcc == EffectFourCC("SMTR")) {
      EffectValue scaled;
      scaled.fourcc = EffectFourCC("SCAL");
      scaled.args.push_back(prop.value[2].args[0]);
      prop.value[2].args[0] = scaled;
    }
  }
  const auto dynamic = ConvertEffect(effect, data.data(), io);
  props.clear();
  CHECK_MSG(dynamic.size() == 1 && SplitRetailPart(dynamic[0].part.data(), dynamic[0].part.size(), props, error),
        "dynamic opacity overrides convert and split");
  found = false;
  for (const auto& prop : props) {
    if (prop.fourcc == EffectFourCC("VSMT")) {
      found = true;
      CHECK_MSG(VmatWord(prop.value, 40) == 19 && VmatWord(prop.value, 48) == EffectFourCC("SCAL") &&
                VmatWord(prop.value, 52) == EffectFourCC("CNST") && VmatWord(prop.value, 56) == Bits(0.75f) &&
                VmatWord(prop.value, 60) == 20 && VmatWord(prop.value, 72) == Bits(0.2f),
            "dynamic SMTR tree survives on gain target with bias target following it");
    }
  }
  CHECK_MSG(found, "dynamic gain still emits VSMT");
  const auto guarded = ConvertVmat(MatiCch0(0x7d52bd6b, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}},
                                           0.5f, 0.5f, 2.f, 0.25f));
  CHECK_MSG(guarded.second.empty(), "other recipes retain unitCch0 guard");
}

struct PowerBallFields {
  EffectGuid child = kPowerBallChild;
  EffectGuid material = kPowerBallMati;
  uint8_t materialTag = 0;
  uint8_t materialSelector = 0;
  EffectGuid model = kPowerBallModel;
  uint32_t shader = 0xe899b1df;
  uint32_t headerBlend = ~0u;
  uint32_t dvvn = 4;
  uint8_t dvvnTag = 1;
  uint32_t pbdm = 0;
  uint8_t pbdmTag = 1;
  uint8_t modelTag = 3;
  uint32_t iexp = 1;
  bool hasModel = true;
  bool hasDvvn = true;
  bool hasPbdm = true;
  bool hasIexp = false;
  bool unsupportedModel = false;
  bool unsupportedDvvn = false;
  bool unsupportedPbdm = false;
  bool bareModel = false;
  bool barePbdm = false;
  bool duplicateMaterial = false;
  uint32_t duplicateProperty = 0;
  uint32_t extraValueProperty = 0;
  bool wordDvvn = false;
  bool constantDvvn = false;
  bool swoosh = false;
};

std::vector<uint8_t> PowerBallEffect(const PowerBallFields& fields) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "DVVN", 1);
  out.push_back(4);  // WPSM/APSM root's DVVN #04
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, fields.child);
  PutGenerator(out, false);
  if (fields.hasDvvn) {
    PutProperty(out, "DVVN", fields.dvvnTag);
    if (fields.unsupportedDvvn) {
      PutFourCC(out, "RAND");
      PutConstant(out, 0);
      PutConstant(out, fields.dvvn);
    } else {
      out.push_back(uint8_t(fields.dvvn));  // source stores DVVN as the raw flag byte (#04)
    }
  }
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 1);
  PutProperty(out, "ITEN", 3);
  PutConstant(out, Bits(3.0f));
  if (fields.hasPbdm) {
    PutProperty(out, "PBDM", fields.pbdmTag);
    if (fields.unsupportedPbdm) {
      PutFourCC(out, "RAND");
      PutConstant(out, 0);
      PutConstant(out, fields.pbdm);
    } else if (fields.barePbdm) {
      out.push_back(uint8_t(fields.pbdm));
    } else {
      PutConstant(out, fields.pbdm);
    }
  }
  PutProperty(out, "MTIN", fields.materialTag);
  out.push_back(fields.materialSelector);
  PutGuid(out, fields.material);
  if (fields.duplicateMaterial) {
    PutProperty(out, "MTIN", 0);
    out.push_back(0);
    PutGuid(out, fields.material);
  }
  if (fields.hasModel) {
    PutProperty(out, "PMDL", fields.modelTag);
    if (fields.unsupportedModel) {
      PutFourCC(out, "RAND");
      PutConstant(out, 0);
      PutConstant(out, 1);
    } else if (fields.bareModel) {
      PutGuid(out, fields.model);
    } else {
      PutFourCC(out, "CNST");
      PutGuid(out, fields.model);
    }
  }
  if (fields.hasIexp) {
    PutProperty(out, "IEXP", 1);
    out.push_back(uint8_t(fields.iexp));
  }
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

uint32_t ConvertPowerBallFeatures(const PowerBallFields& fields, std::vector<uint8_t>* emitted = nullptr) {
  const std::vector<uint8_t> data = PowerBallEffect(fields);
  const std::vector<uint8_t> mati = PowerBallMati(fields.shader, fields.headerBlend);
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: PowerBall fixture does not parse: %s\n", error.c_str());
    port_test::CountFailure();
    return ~0u;
  }
  // AST mutations cover ambiguous properties and the other effect representation without
  // depending on the source parser accepting duplicate field encodings.
  EffectNode& child = effect.children.at(0);
  for (EffectProperty& property : child.properties) {
    if (property.fourcc == EffectFourCC("DVVN")) {
      if (fields.wordDvvn) {
        property.value[0].kind = EffectValue::Kind::Word;
      }
      if (fields.constantDvvn) {
        const EffectValue value = property.value[0];
        property.value[0].kind = EffectValue::Kind::Element;
        property.value[0].fourcc = EffectFourCC("CNST");
        property.value[0].args = {value};
      }
    }
    if (property.fourcc == fields.extraValueProperty) {
      property.value.push_back(property.value[0]);
    }
  }
  if (fields.duplicateProperty != 0) {
    for (const EffectProperty& property : child.properties) {
      if (property.fourcc == fields.duplicateProperty) {
        const EffectProperty copy = property;
        child.properties.push_back(copy);
        break;
      }
    }
  }
  if (fields.swoosh) {
    child.form = EffectFourCC("SWSH");
  }
  EffectConvertIO io;
  io.assetId = [&fields](const EffectGuid& id, uint32_t type) {
    return type == EffectFourCC("CMDL") && id == fields.model ? 0xC0DE4BF6u : 0u;
  };
  io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid& texture) {
    if (texture == kPowerBallRamp) {
      return FlipbookAtlas{0x5EED0008u, 1, 1, 1};
    }
    if (texture == kPowerBallIndirect) {
      return FlipbookAtlas{0x5EED0009u, 1, 1, 1};
    }
    return FlipbookAtlas{};
  };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  for (const ConvertedPart& part : parts) {
    if (part.id != fields.child) {
      continue;
    }
    std::vector<RetailPartProperty> properties;
    if (!SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error)) {
      return ~0u;
    }
    if (emitted != nullptr) {
      *emitted = part.part;
    }
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == EffectFourCC("VMAT")) {
        return VmatWord(property.value, 12);
      }
    }
  }
  return ~0u;
}

void TestPowerBallExposurePolicy() {
  const uint32_t flagged = ConvertPowerBallFeatures({});
  CHECK_MSG(flagged != ~0u && (flagged & kAlreadyExposedBit) != 0,
            "PowerBall MATI e899b1df/header -1 + PMDL/DVVN 4/PBDM 0/no IEXP: AlreadyExposed");
  const auto unflagged = [](uint32_t features) {
    return features != ~0u && (features & kAlreadyExposedBit) == 0;
  };
  const uint32_t plain = ConvertPowerBallFeatures(PowerBallFields{.dvvn = 3});
  CHECK_MSG(plain != ~0u && (flagged & ~kAlreadyExposedBit) == plain,
            "PowerBall exposure policy changes only the AlreadyExposed bit");
  // The runtime's model-opacity selection (platform/include/port_vfx_opacity.h) keys on a feature
  // word because the VMAT carries no shader id: this material's word is the key, so its mesh draw
  // takes the normal test. This pins *that* recipe's word; that no other row of the converter's
  // table carries the key is not visible from here and is counted by tests/port_vfx_opacity_wiring.py.
  CHECK_MSG(plain == kPortVfxModelOpacityFeatures,
            "e899b1df's recipe word is the model-opacity key (Ramp|Indirect|DepthSoften|OpacityFade)");
  CHECK_MSG(PortVfxUsesModelOpacity(plain) && PortVfxUsesModelOpacity(flagged),
            "the key ignores the exposure policy bit");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.material = Fresh(7)})), "near miss: other MATI GUID");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.materialTag = 1})), "near miss: MTIN tag");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.materialSelector = 1})), "near miss: MTIN selector");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.duplicateMaterial = true})), "duplicate MTIN fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.child = Fresh(8)})), "near miss: other child GUID");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.shader = 0x461071b8})), "near miss: other MATI shader");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.headerBlend = 1})), "near miss: MATI header blend 1");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.headerBlend = 0})), "near miss: MATI header blend 0");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasModel = false})), "near miss: PMDL absent");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.modelTag = 1})), "near miss: PMDL tag");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.model = Fresh(4)})), "near miss: different PMDL GUID");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.unsupportedModel = true})), "near miss: PMDL expression");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.bareModel = true})), "near miss: PMDL without CNST");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.dvvn = 3})), "near miss: DVVN 3");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.dvvnTag = 3})), "near miss: DVVN tag");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasDvvn = false})), "missing DVVN fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.unsupportedDvvn = true})), "DVVN expression fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.pbdm = 1})), "near miss: PBDM 1");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.pbdmTag = 3})), "near miss: PBDM tag");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasPbdm = false})), "missing PBDM fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.unsupportedPbdm = true})), "PBDM expression fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.barePbdm = true})), "PBDM without CNST fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.iexp = 0, .hasIexp = true})), "IEXP false present fails closed");
  CHECK_MSG(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasIexp = true})), "IEXP true present fails closed");
}

void TestPowerChargeExposurePolicy() {
  // Exact audited held-charge GENP child, not the projectile fixture's child/MATI/PBDM.
  const EffectGuid chargeChild = {0xdf, 0x39, 0xe6, 0xa9, 0xfe, 0x9d, 0x41, 0x42,
                                  0x84, 0x2e, 0x0b, 0x8f, 0x8a, 0x65, 0x48, 0xe2};
  const EffectGuid chargeMati = {0x3c, 0x1c, 0x7d, 0x90, 0xc8, 0xcc, 0x9b, 0x4b,
                                 0x87, 0x8f, 0xee, 0x8a, 0x41, 0xfc, 0x1e, 0x7f};
  PowerBallFields exact;
  exact.child = chargeChild;
  exact.material = chargeMati;
  exact.pbdm = 2;
  std::vector<uint8_t> flaggedBytes;
  const uint32_t flagged = ConvertPowerBallFeatures(exact, &flaggedBytes);
  CHECK_MSG(flagged == (kPortVfxModelOpacityFeatures | kAlreadyExposedBit),
        "held charge exact child/MATI/header -1/model/DVVN4/PBDM2: AlreadyExposed");
  auto plain = exact;
  plain.hasIexp = true;
  std::vector<uint8_t> plainBytes;
  const uint32_t plainFeatures = ConvertPowerBallFeatures(plain, &plainBytes);
  CHECK_MSG(plainFeatures == kPortVfxModelOpacityFeatures, "explicit IEXP uses the runtime policy, not the legacy pin");
  const auto iexpBytes = Retail().f("IEXP").f("CNST").w(plain.iexp).bytes;
  const auto iexpAt = std::search(plainBytes.begin(), plainBytes.end(), iexpBytes.begin(), iexpBytes.end());
  CHECK_MSG(iexpAt != plainBytes.end(), "explicit IEXP is serialized for the runtime reader");
  if (iexpAt != plainBytes.end()) {
    plainBytes.erase(iexpAt, iexpAt + iexpBytes.size());
  }
  // Apart from the new explicit property, only the legacy compatibility feature bit differs.
  size_t differences = 0;
  bool onlyPolicyBit = flaggedBytes.size() == plainBytes.size();
  for (size_t i = 0; i < std::min(flaggedBytes.size(), plainBytes.size()); ++i) {
    if (flaggedBytes[i] != plainBytes[i]) {
      ++differences;
      onlyPolicyBit &= (flaggedBytes[i] ^ plainBytes[i]) == 0x80;
    }
  }
  CHECK_MSG(onlyPolicyBit && differences == 1, "held charge emitted bytes change only exposure bit, not blend/recipe/alpha");
  const auto reject = [](const PowerBallFields& fields, const char* message) {
    const uint32_t features = ConvertPowerBallFeatures(fields);
    CHECK_MSG(features != ~0u && (features & kAlreadyExposedBit) == 0, message);
  };
  // Every exact identity, state, representation and uniqueness guard must fail closed.
  auto f = exact; f.child = Fresh(8); reject(f, "held charge: wrong child");
  f = exact; f.child = kPowerBallChild; reject(f, "held charge: projectile child does not match");
  f = exact; f.material = Fresh(7); reject(f, "held charge: wrong MATI");
  f = exact; f.material = kPowerBallMati; reject(f, "held charge: projectile MATI does not match");
  f = exact; f.shader = 0x461071b8; reject(f, "held charge: wrong shader");
  for (uint32_t header : {0u, 1u, 2u}) {
    f = exact; f.headerBlend = header; reject(f, "held charge: wrong MATI header blend");
  }
  f = exact; f.materialTag = 1; reject(f, "held charge: MTIN tag");
  f = exact; f.materialSelector = 1; reject(f, "held charge: MTIN selector");
  f = exact; f.duplicateMaterial = true; reject(f, "held charge: duplicate MTIN");
  f = exact; f.hasModel = false; reject(f, "held charge: missing PMDL");
  f = exact; f.model = Fresh(4); reject(f, "held charge: wrong PMDL");
  f = exact; f.modelTag = 1; reject(f, "held charge: PMDL tag");
  f = exact; f.unsupportedModel = true; reject(f, "held charge: PMDL expression");
  f = exact; f.bareModel = true; reject(f, "held charge: bare PMDL");
  for (uint32_t dvvn : {0u, 3u, 5u}) {
    f = exact; f.dvvn = dvvn; reject(f, "held charge: wrong DVVN");
  }
  f = exact; f.dvvnTag = 3; reject(f, "held charge: DVVN tag");
  f = exact; f.hasDvvn = false; reject(f, "held charge: missing DVVN");
  f = exact; f.unsupportedDvvn = true; reject(f, "held charge: DVVN expression");
  f = exact; f.wordDvvn = true; reject(f, "held charge: DVVN word instead of source byte");
  f = exact; f.constantDvvn = true; reject(f, "held charge: DVVN CNST instead of source byte");
  for (uint32_t pbdm : {0u, 1u, 3u}) {
    f = exact; f.pbdm = pbdm; reject(f, "held charge: wrong PBDM");
  }
  f = exact; f.pbdmTag = 3; reject(f, "held charge: PBDM tag");
  f = exact; f.hasPbdm = false; reject(f, "held charge: missing PBDM");
  f = exact; f.unsupportedPbdm = true; reject(f, "held charge: PBDM expression");
  f = exact; f.barePbdm = true; reject(f, "held charge: bare PBDM");
  for (uint32_t iexp : {0u, 1u}) {
    f = exact; f.hasIexp = true; f.iexp = iexp; reject(f, "held charge: explicit IEXP0/1");
  }
  for (uint32_t property : {EffectFourCC("PMDL"), EffectFourCC("DVVN"), EffectFourCC("PBDM")}) {
    f = exact; f.duplicateProperty = property; reject(f, "held charge: duplicate state/model");
    f = {}; f.duplicateProperty = property; reject(f, "projectile: duplicate state/model remains unexposed");
  }
  for (uint32_t property : {EffectFourCC("MTIN"), EffectFourCC("PMDL"), EffectFourCC("DVVN"), EffectFourCC("PBDM")}) {
    f = exact; f.extraValueProperty = property; reject(f, "held charge: extra property values fail closed");
  }
  f = exact; f.swoosh = true; reject(f, "held charge: swoosh keeps room exposure");
}

// Only the wave orb's MATI, on a model particle, with header blend 1 takes Additive over PBDM.
void TestWaveOrbHeaderBlend() {
  auto orb = [](uint32_t shader, uint32_t headerBlend) {
    std::vector<uint8_t> mati = Mati(shader, {{shader == 0x642dd72b ? "BCLR" : "TCH0", Fresh(8)}});
    for (int i = 0; i < 4; ++i) {
      mati[0x61 + size_t(i)] = uint8_t(headerBlend >> (8 * i));
    }
    return mati;
  };
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true) == 5, "wave orb MATI + PMDL + header blend 1: AdditiveOne (5)");
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true) == 0, "other MATI guid keeps PBDM 0");
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 0), kWaveMati, true) == 0, "header blend 0 keeps PBDM");
  CHECK_MSG(ConvertBlend(orb(0x461071b8, 1), kWaveMati, true) == 0, "another shader keeps PBDM 0");
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 2), kWaveMati, true) == 0, "header blend 2 keeps PBDM 0");
  CHECK_MSG(ConvertBlend(std::vector<uint8_t>(0x50, 0), kWaveMati, true) == ~0u, "a short MATI writes no VMAT, no crash");
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, false) == 5, "no PMDL: the sprite takes EBlendMode 1 over PBDM");
  // The older modes are untouched: PBDM 0..3 convert as themselves, on the orb's MATI too without PMDL
  // and on its PMDL with another guid.
  for (uint32_t pbdm = 0; pbdm <= 3; ++pbdm) {
    CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true, pbdm) == pbdm, "PBDM 0..3 convert unchanged");
    CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, false, pbdm) == 5, "EBlendMode 1 without PMDL, whatever the PBDM");
  }
  // A sprite's MATI EBlendMode (SBlendModeState 0xf0164) wins over PBDM; -1 and the modes the port has no
  // exact state for keep the row.
  const struct {
    uint32_t header, pbdm, blend;
  } sprite[] = {{~0u, 2, 2}, {0, 2, 3}, {1, 2, 5}, {2, 1, 0}, {5, 2, 6}, {6, 0, 7}, {3, 2, 2}, {4, 1, 1},
                {7, 0, 0}, {10, 2, 2}, {14, 3, 3}};
  for (const auto& c : sprite) {
    CHECK_MSG(ConvertBlend(orb(0x461071b8, c.header), Fresh(7), false, c.pbdm) == c.blend,
              "a sprite takes its MATI's EBlendMode where the port has it, else the PBDM row");
  }
  // The fog is the row's (SetupFogFromBlendMode on PBDM), whatever the MATI's factors.
  const auto fogBits = [&orb](uint32_t header, uint32_t pbdm) {
    return ConvertBlend(orb(0x461071b8, header), Fresh(7), false, pbdm, 12) & (1u << 27 | 1u << 22);
  };
  CHECK_MSG(fogBits(6, 0) == 1u << 27, "PBDM alpha under EBlendMode 6: fogged");
  CHECK_MSG(fogBits(1, 3) == 1u << 27, "PBDM opaque under EBlendMode 1: fogged");
  CHECK_MSG(fogBits(2, 1) == 1u << 22, "PBDM additive under EBlendMode 2: unfogged");
  CHECK_MSG(fogBits(0, 2) == 1u << 22, "PBDM 2 under EBlendMode 0: unfogged");
  CHECK_MSG(fogBits(2, 0) == 0 && fogBits(5, 1) == 0 && fogBits(~0u, 1) == 0, "matching fog pins nothing");
  CHECK_MSG(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true, 2) == 5, "the override is the orb's, whatever its PBDM");
  TestWaveOrbSwoosh(orb(0x642dd72b, 1));
}

// The new recipes: 3db95827 draws as a multiply (blend 4), and
// the ice charge beam reads its colour from BCLR alone.
void TestVmatNewRecipes() {
  // FrameBuffer_Indirect_Unlit with no warp: features 0, blend Multiply, no textures.
  auto [parts, blob] = ConvertVmat(Mati(0x3db95827, {}));
  CHECK_MSG(parts.size() == 1 && !blob.empty(), "a 3db95827 material writes VMAT");
  if (!blob.empty()) {
    CHECK_MSG(VmatWord(blob, 8) == 2 && VmatWord(blob, 12) == 0 && VmatWord(blob, 16) == 4 && VmatWord(blob, 20) == 0,
              "3db95827 VMAT v2, no features, blend 4, no textures");
  }
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "3db95827 with no warp approximates nothing");

  // The same shader with a warp: the warp is dropped (drawn as a multiply).
  auto warped = ConvertVmat(MatiCch0(0x3db95827, {}, 0.5f, 0.0f, 0.0f, 0.0f));
  CHECK_MSG(warped.first.size() == 1 && !warped.second.empty(), "a warped 3db95827 material still writes VMAT");
  if (!warped.second.empty()) {
    CHECK_MSG(VmatWord(warped.second, 16) == 4, "a warped 3db95827 VMAT still blends as a multiply");
  }
  CHECK_MSG(warped.first.size() == 1 && warped.first[0].approximated.size() == 1 &&
                warped.first[0].approximated[0] ==
                    "VMAT: shader 3db95827's scene warp dropped (drawn as a multiply)",
            "a warped 3db95827 notes its scene warp dropped");

  // VFX_IceChargeBeam without its NMAP, and so without the bumped facing term, has no recipe.
  auto ice = ConvertVmat(Mati(0x9e0605cd, {{"BCLR", Fresh(8)}}));
  CHECK_MSG(ice.first.size() == 1 && ice.second.empty(), "a 9e0605cd material without NMAP writes no VMAT");
}

// VFX_IceChargeBeam (9e0605cd): the IceChargeBeam feature on BCLR and NMAP, PBDM 3 opaque, CCH0..2
// whole in VPRM (the NMAP slot first), an SMTR CCHn component to its own VSMT target, and only under
// DFOG 1 (perm 000 is unfogged).
void TestVmatIceChargeBeamRecipe() {
  constexpr uint32_t kIceChargeBeam = 1u << 20;
  const auto withVectors = [](std::vector<uint8_t> mati, const std::vector<std::array<float, 4>>& rows) {
    for (size_t n = 0; n < rows.size(); ++n) {
      mati[0x69] += 1;
      mati.push_back(3);
      mati.insert(mati.end(), {'C', 'C', 'H', uint8_t('0' + n)});
      for (const float f : rows[n]) {
        Put32(mati, Bits(f));
      }
    }
    return mati;
  };
  const std::vector<uint8_t> mati = withVectors(Mati(0x9e0605cd, {{"BCLR", Fresh(8)}, {"NMAP", Fresh(8)}}),
                                                {{50.f, 0.5f, 1.5f, 10.f}, {1.f, 0.2f, 0.1f, 2.f}, {0.f, 0.f, 0.f, 0.f}});
  const auto effect = [](bool dfog) {
    std::vector<uint8_t> data = VmatEffect(true, Fresh(7), 3);
    std::vector<uint8_t> extra;
    if (dfog) {
      PutProperty(extra, "DFOG", 1);
      PutConstant(extra, 1);
    }
    const std::vector<SmtrTestEntry> entries = {{"CCH2", 0, 0.75f}, {"CCH1", 3, 2.5f}};
    PutProperty(extra, "SMTR", 0);
    extra.push_back(0);
    extra.push_back(uint8_t(entries.size()));
    extra.push_back(uint8_t(entries.size()));
    for (const SmtrTestEntry& entry : entries) {
      PutSmtrItem(extra, entry);
    }
    for (int group = 1; group < 5; ++group) {
      extra.push_back(0);
    }
    data.insert(data.end() - 13, extra.begin(), extra.end());
    return data;
  };
  const auto convert = [&mati](const std::vector<uint8_t>& data, std::vector<RetailPartProperty>& properties) {
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the ice effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [&mati](const EffectGuid& material) {
      return material == Fresh(7) ? mati : std::vector<uint8_t>();
    };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      atlas.id = texture == Fresh(8) ? 0x5EED0008 : 0;
      return atlas;
    };
    io.modelMesh = [](uint32_t) { return std::vector<uint8_t>{0, 0, 0, 2}; };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    if (parts.size() == 1) {
      SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    }
    return parts;
  };
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  std::vector<RetailPartProperty> properties;
  std::vector<ConvertedPart> parts = convert(effect(true), properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "9e0605cd under DFOG 1 approximates nothing");
  const std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(VmatWord(vmat, 12) == kIceChargeBeam && VmatWord(vmat, 16) == 3 && VmatWord(vmat, 20) == 2,
            "9e0605cd VMAT: IceChargeBeam, blend 3 (PBDM), two textures");
  // Role slots after the two 11-word texture records: BCLR is the colour slot (0).
  CHECK_MSG(VmatWord(vmat, 112) == 0, "9e0605cd's BCLR is the colour slot");
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 12 + 3 * 16 && VmatWord(vprm, 8) == 1 && VmatWord(vprm, 12) == 1 &&
                VmatWord(vprm, 16) == 3,
            "VPRM v1: the NMAP slot 1 and three CCH rows");
  CHECK_MSG(VmatWord(vprm, 20) == Bits(50.f) && VmatWord(vprm, 20 + 12) == Bits(10.f) &&
                VmatWord(vprm, 36 + 4) == Bits(0.2f) && VmatWord(vprm, 52) == Bits(0.f),
            "VPRM carries the MATI's CCH0..2");
  const std::vector<uint8_t> vsmt = value(properties, EffectFourCC("VSMT"));
  // The class, the count, then {target, kind, element (CNST value)}.
  CHECK_MSG(VmatWord(vsmt, 4) == 2 && VmatWord(vsmt, 8) == kPortVfxParamTarget + 8 &&
                VmatWord(vsmt, 8 + 16) == kPortVfxParamTarget + 4 + 3,
            "SMTR CCH2.x and CCH1.w go to their VPRM targets");
  CHECK_MSG(!value(properties, EffectFourCC("VMSH")).empty(), "the ice model keeps its VMSH");

  properties.clear();
  parts = convert(effect(false), properties);
  CHECK_MSG(parts.size() == 1 && value(properties, EffectFourCC("VMAT")).empty() && value(properties, EffectFourCC("VPRM")).empty(),
            "9e0605cd without DFOG 1 writes no VMAT");
  CHECK_MSG(parts.size() == 1 && !parts[0].approximated.empty() &&
                parts[0].approximated[0].find("drawn fogged") != std::string::npos,
            "9e0605cd without DFOG 1 notes the fog it lacks");
}

// VFX_IceChargeCollision_Unlit (1c651f97): the IceChargeCollision feature on BCLR, NMAP and TCH0 (the
// environment), PBDM 0, CCH0..6 whole in VPRM v2 (the NMAP then TCH0 slots first), a colour SMTR entry
// to four VSMT targets, and drawn fogged unless DFOG 1 (perm 000) marks it Unfogged.
void TestVmatIceChargeCollisionRecipe() {
  constexpr uint32_t kIceChargeCollision = 1u << 21, kUnfogged = 1u << 22;
  // As on the disc: NMAP and TCH0 are type-5 textures, CCH0/1/4/6 type-4 colours.
  std::vector<uint8_t> mati =
      Mati(0x1c651f97, {{"BCLR", Fresh(8)}, {"NMAP", Fresh(8)}, {"TCH0", Fresh(8)}});
  mati[0x6d + 41] = 5;
  mati[0x6d + 82] = 5;
  for (int n = 0; n < 7; ++n) {
    mati[0x69] += 1;
    mati.push_back(n == 0 || n == 1 || n == 4 || n == 6 ? 4 : 3);
    mati.insert(mati.end(), {'C', 'C', 'H', uint8_t('0' + n)});
    for (int c = 0; c < 4; ++c) {
      Put32(mati, Bits(float(n) + 0.25f * float(c)));
    }
  }
  const auto effect = [](bool dfog) {
    std::vector<uint8_t> data = VmatEffect(true, Fresh(7), 0);
    std::vector<uint8_t> extra;
    if (dfog) {
      PutProperty(extra, "DFOG", 1);
      PutConstant(extra, 1);
    }
    // One real entry (CCH5.y) in group 0 and one colour entry (CCH6) in group 4.
    PutProperty(extra, "SMTR", 0);
    extra.push_back(0);
    extra.push_back(2);
    extra.push_back(1);
    PutSmtrItem(extra, {"CCH5", 1, 4.f});
    for (int group = 1; group < 4; ++group) {
      extra.push_back(0);
    }
    extra.push_back(1);
    PutColour(extra, 0.6f, 0.8f, 1.f, 1.f);
    PutFourCC(extra, "CCH6");
    extra.push_back(0);
    extra.push_back(0);
    data.insert(data.end() - 13, extra.begin(), extra.end());
    return data;
  };
  const auto convert = [&mati](const std::vector<uint8_t>& data, std::vector<RetailPartProperty>& properties) {
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the ice collision effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [&mati](const EffectGuid& material) {
      return material == Fresh(7) ? mati : std::vector<uint8_t>();
    };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      atlas.id = texture == Fresh(8) ? 0x5EED0008 : 0;
      return atlas;
    };
    io.modelMesh = [](uint32_t) { return std::vector<uint8_t>{0, 0, 0, 2}; };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    if (parts.size() == 1) {
      SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    }
    return parts;
  };
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  std::vector<RetailPartProperty> properties;
  std::vector<ConvertedPart> parts = convert(effect(false), properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "1c651f97 without DFOG approximates nothing");
  std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(VmatWord(vmat, 12) == kIceChargeCollision && VmatWord(vmat, 16) == 0 && VmatWord(vmat, 20) == 3,
            "1c651f97 VMAT: IceChargeCollision (fogged), blend 0 (PBDM), three textures");
  // Role slots after the three 11-word texture records: BCLR is the colour slot (0).
  CHECK_MSG(VmatWord(vmat, 156) == 0, "1c651f97's BCLR is the colour slot");
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 16 + 7 * 16 && VmatWord(vprm, 8) == 2 && VmatWord(vprm, 12) == 1 &&
                VmatWord(vprm, 16) == 2 && VmatWord(vprm, 20) == 7,
            "VPRM v2: the NMAP slot 1, the TCH0 slot 2 and seven CCH rows");
  CHECK_MSG(VmatWord(vprm, 24) == Bits(0.f) && VmatWord(vprm, 24 + 6 * 16 + 12) == Bits(6.75f),
            "VPRM carries the MATI's CCH0..6");
  const std::vector<uint8_t> vsmt = value(properties, EffectFourCC("VSMT"));
  // The class, the count, then {target, kind, element (CNST value)}.
  CHECK_MSG(VmatWord(vsmt, 4) == 5 && VmatWord(vsmt, 8) == kPortVfxParamTarget + 4 * 5 + 1,
            "SMTR CCH5.y goes to its VPRM target");
  bool row = VmatWord(vsmt, 4) == 5;
  for (uint32_t c = 0; row && c < 4; ++c) {
    row = VmatWord(vsmt, 8 + 16 * (1 + c)) == kPortVfxParamTarget + 4 * 6 + c;
  }
  CHECK_MSG(row && VmatWord(vsmt, 8 + 16 + 12) == Bits(0.6f) && VmatWord(vsmt, 8 + 64 + 12) == Bits(1.f),
            "the colour SMTR CCH6 sets the whole row, one target per component");

  properties.clear();
  parts = convert(effect(true), properties);
  vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty() &&
                VmatWord(vmat, 12) == (kIceChargeCollision | kUnfogged),
            "1c651f97 under DFOG 1 is Unfogged");
}

// VFX_IceChargeCollision_Lit (390fc33b, Effect_IcePop): the unlit recipe's slots and CCH0..6 plus the
// IceChargeCollisionLit bit (the runtime's clustered gather and baked probe), and Unfogged only under
// DFOG 1 as 1c651f97 is.
void TestVmatIceChargeCollisionLitRecipe() {
  constexpr uint32_t kIceChargeCollision = 1u << 21, kUnfogged = 1u << 22, kIceChargeCollisionLit = 1u << 25;
  // The disc's MATI: BCLR/NMAP/TCH0 and CCH0..6, as it lists them.
  std::vector<uint8_t> mati =
      Mati(0x390fc33b, {{"BCLR", Fresh(8)}, {"NMAP", Fresh(8)}, {"TCH0", Fresh(8)}});
  mati[0x6d + 41] = 5;
  for (int n = 0; n < 7; ++n) {
    mati[0x69] += 1;
    mati.push_back(n == 0 || n == 1 || n == 4 || n == 6 ? 4 : 3);
    mati.insert(mati.end(), {'C', 'C', 'H', uint8_t('0' + n)});
    for (int c = 0; c < 4; ++c) {
      Put32(mati, Bits(float(n) + 0.25f * float(c)));
    }
  }
  const auto effect = [](bool dfog) {
    // PBDM 2: the additive blend the IcePop generators carry.
    std::vector<uint8_t> data = VmatEffect(true, Fresh(7), 2);
    std::vector<uint8_t> extra;
    if (dfog) {
      PutProperty(extra, "DFOG", 1);
      PutConstant(extra, 1);
    }
    data.insert(data.end() - 13, extra.begin(), extra.end());
    return data;
  };
  const auto convert = [&mati](const std::vector<uint8_t>& data, std::vector<RetailPartProperty>& properties) {
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the ice pop effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [&mati](const EffectGuid& material) {
      return material == Fresh(7) ? mati : std::vector<uint8_t>();
    };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      atlas.id = texture == Fresh(8) ? 0x5EED0008 : 0;
      return atlas;
    };
    io.modelMesh = [](uint32_t) { return std::vector<uint8_t>{0, 0, 0, 2}; };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    if (parts.size() == 1) {
      SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    }
    return parts;
  };
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  std::vector<RetailPartProperty> properties;
  std::vector<ConvertedPart> parts = convert(effect(false), properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "390fc33b without DFOG approximates nothing");
  const std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(VmatWord(vmat, 12) == (kIceChargeCollision | kIceChargeCollisionLit) && VmatWord(vmat, 16) == 2 &&
                VmatWord(vmat, 20) == 3,
            "390fc33b VMAT: IceChargeCollision + IceChargeCollisionLit (fogged), blend 2 (PBDM 2), three textures");
  CHECK_MSG(VmatWord(vmat, 156) == 0, "390fc33b's BCLR is the colour slot");
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 16 + 7 * 16 && VmatWord(vprm, 8) == 2 && VmatWord(vprm, 12) == 1 &&
                VmatWord(vprm, 16) == 2 && VmatWord(vprm, 20) == 7,
            "VPRM v2: the NMAP slot 1, the TCH0 slot 2 and seven CCH rows");
  CHECK_MSG(VmatWord(vprm, 24 + 6 * 16 + 12) == Bits(6.75f), "VPRM carries the MATI's CCH0..6");

  properties.clear();
  parts = convert(effect(true), properties);
  const std::vector<uint8_t> unfogged = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty() &&
                VmatWord(unfogged, 12) == (kIceChargeCollision | kIceChargeCollisionLit | kUnfogged),
            "390fc33b under DFOG 1 is Unfogged");
}

// VFX_Color_Opacity_Lit (20e149b2, the Landing Site mist): LIT_ 1 selects the lit perm, the ParticleLit bit,
// and its inverse exposure (IEXP, or DVVN 3/4's default) makes the draw AlreadyExposed. LIT_ 0 is the unlit
// recipe alone, and so is a shader without a lit tail.
void TestVmatParticleLit() {
  constexpr uint32_t kColorTex = 1, kOpacityTex = 2, kDepthSoften = 32, kParticleLit = 1u << 26;
  const auto features = [](uint32_t shader, int lit, int iexp, int dvvn) {
    std::vector<uint8_t> data = VmatEffect(false, Fresh(7), 0);
    std::vector<uint8_t> extra;
    if (lit >= 0) {
      PutProperty(extra, "LIT_", 0);
      extra.push_back(uint8_t(lit));
    }
    if (iexp >= 0) {
      PutProperty(extra, "IEXP", 1);
      extra.push_back(uint8_t(iexp));
    }
    if (dvvn >= 0) {
      PutProperty(extra, "DVVN", 1);
      extra.push_back(uint8_t(dvvn));
    }
    data.insert(data.end() - 13, extra.begin(), extra.end());
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the lit sprite effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [shader](const EffectGuid& material) {
      return material == Fresh(7) ? Mati(shader, {{"BCLR", Fresh(8)}, {"TCH0", Fresh(9)}}) : std::vector<uint8_t>();
    };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      atlas.id = texture == Fresh(8) ? 0x5EED0008 : texture == Fresh(9) ? 0x5EED0009 : 0;
      return atlas;
    };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    std::vector<RetailPartProperty> properties;
    if (parts.size() != 1 || !SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
      return ~0u;
    }
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == EffectFourCC("VMAT")) {
        return VmatWord(property.value, 12);
      }
    }
    return ~0u;
  };
  const uint32_t unlit = kColorTex | kOpacityTex | kDepthSoften;
  CHECK_MSG(features(0x20e149b2, -1, -1, -1) == unlit, "20e149b2 without LIT_ is the unlit recipe");
  CHECK_MSG(features(0x20e149b2, 0, 1, 4) == unlit, "LIT_ 0 is the unlit recipe, IEXP or not");
  CHECK_MSG(features(0x20e149b2, 1, 0, 1) == (unlit | kParticleLit), "LIT_ 1: ParticleLit, at the room's exposure");
  // The lit vertex perm reads no inverse exposure (080_1: o6 = v3, v2.z unread), so IEXP never makes a lit
  // draw AlreadyExposed.
  CHECK_MSG(features(0x20e149b2, 1, 1, 1) == (unlit | kParticleLit), "LIT_ 1 + IEXP 1 is lit at the room exposure");
  CHECK_MSG(features(0x20e149b2, 1, -1, 4) == (unlit | kParticleLit), "LIT_ 1 under DVVN 4 is lit at the room exposure");
  CHECK_MSG(features(0x20e149b2, 1, 0, 4) == (unlit | kParticleLit), "LIT_ 1 + IEXP 0 is lit at the room exposure");
  const uint32_t other = features(0x461071b8, 1, 1, 4);
  CHECK_MSG(other != ~0u && (other & (kParticleLit | kAlreadyExposedBit)) == 0, "a shader off the lit-tail list stays unlit");
}

// VFX_VisorRunOff (b5e12668 / d85d4169): one TCH0 flow map as the colour slot, CCH0..4 whole in VPRM
// v1 and SMTR overrides of any of them to their VPRM targets, Unfogged without a DFOG 1 (the visor
// composite pass has no fog), and the second shader on its own feature bit.
void TestVmatVisorRunOffRecipes() {
  constexpr uint32_t kVisorRunOff = 1u << 23, kVisorRunOffLit = 1u << 24, kUnfogged = 1u << 22;
  // The visor runoff base values (SPEC-fb-visor.md section 5).
  const std::vector<std::array<float, 4>> cch = {{0.1f, 0.5f, 0.1f, 0.2f},
                                                 {0.f, 8.f, 0.f, 0.f},
                                                 {0.1f, 0.1f, 0.25f, 64.f},
                                                 {0.75f, 0.85f, 0.9f, 4.f},
                                                 {0.4f, 0.5f, 0.75f, 1.f}};
  // One real entry (CCH1.w, the u scroll) and one colour entry (CCH4, the tint), no DFOG.
  const std::vector<uint8_t> data = [] {
    std::vector<uint8_t> out = VmatEffect(true, Fresh(7), 0);
    std::vector<uint8_t> extra;
    PutProperty(extra, "SMTR", 0);
    extra.push_back(0);
    extra.push_back(2);
    extra.push_back(1);
    PutSmtrItem(extra, {"CCH1", 3, 0.25f});
    for (int group = 1; group < 4; ++group) {
      extra.push_back(0);
    }
    extra.push_back(1);
    PutColour(extra, 0.5f, 0.25f, 0.75f, 1.f);
    PutFourCC(extra, "CCH4");
    extra.push_back(0);
    extra.push_back(0);
    out.insert(out.end() - 13, extra.begin(), extra.end());
    return out;
  }();
  const auto convert = [&](uint32_t shader, std::vector<RetailPartProperty>& properties) {
    const std::vector<uint8_t> mati = MatiCchN(shader, {{"TCH0", Fresh(8)}}, cch);
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the visor runoff effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [&mati](const EffectGuid& material) {
      return material == Fresh(7) ? mati : std::vector<uint8_t>();
    };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      atlas.id = texture == Fresh(8) ? 0x5EED0008 : 0;
      return atlas;
    };
    io.modelMesh = [](uint32_t) { return std::vector<uint8_t>{0, 0, 0, 2}; };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    if (parts.size() == 1) {
      SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    }
    return parts;
  };
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  std::vector<RetailPartProperty> properties;
  std::vector<ConvertedPart> parts = convert(0xb5e12668, properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(),
            "b5e12668 without DFOG writes an exact VMAT");
  const std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(VmatWord(vmat, 12) == (kVisorRunOff | kUnfogged) && VmatWord(vmat, 16) == 0 &&
                VmatWord(vmat, 20) == 1,
            "b5e12668 VMAT: VisorRunOff, Unfogged, blend 0 (PBDM), one texture");
  CHECK_MSG(VmatWord(vmat, 24 + 32) == 0, "b5e12668's TCH0 is the colour slot");
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 12 + 5 * 16 && VmatWord(vprm, 8) == 1 && VmatWord(vprm, 16) == 5,
            "VPRM v1: no NMAP slot and five CCH rows");
  CHECK_MSG(VmatWord(vprm, 20) == Bits(0.1f) && VmatWord(vprm, 20 + 16 + 12) == Bits(0.f) &&
                VmatWord(vprm, 20 + 4 * 16 + 12) == Bits(1.f),
            "VPRM carries the MATI's CCH0..4");
  const std::vector<uint8_t> vsmt = value(properties, EffectFourCC("VSMT"));
  CHECK_MSG(VmatWord(vsmt, 4) == 5 && VmatWord(vsmt, 8) == kPortVfxParamTarget + 4 + 3,
            "SMTR CCH1.w goes to its VPRM target");
  bool row = VmatWord(vsmt, 4) == 5;
  for (uint32_t c = 0; row && c < 4; ++c) {
    row = VmatWord(vsmt, 8 + 16 * (1 + c)) == kPortVfxParamTarget + 4 * 4 + c;
  }
  CHECK_MSG(row && VmatWord(vsmt, 8 + 16 + 12) == Bits(0.5f) &&
                VmatWord(vsmt, 8 + 64 + 12) == Bits(1.f),
            "the colour SMTR CCH4 sets the whole row, one target per component");

  properties.clear();
  parts = convert(0xd85d4169, properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(),
            "d85d4169 without DFOG writes an exact VMAT");
  CHECK_MSG(VmatWord(value(properties, EffectFourCC("VMAT")), 12) == (kVisorRunOffLit | kUnfogged),
            "d85d4169 is VisorRunOffLit, Unfogged");
}

// VFX_VisorRain (0ec41df0, the visor rain splash GENP 45ba782d; vfx/0ec41df0.md): TCH0, a 2-channel
// noise map, as the colour slot, CCH0 whole in VPRM v1 (the drop mask's scale and gain, which the
// effect's SMTR animates), Unfogged without a DFOG 1 (the visor composite pass has no fog), and no
// other slot: the fragment maths needs one noise map and the PMTR rows 0..2.
void TestVmatVisorRainRecipe() {
  constexpr uint32_t kVisorRain = 1u << 28, kUnfogged = 1u << 22;
  // The effect's effective CCH0: the MATI's own (1, .01, .1, .1) with the GENP's SMTR over it.
  const std::vector<std::array<float, 4>> cch = {{1.f, 0.12f, 0.2f, 4.f}};
  const std::vector<uint8_t> data = VmatEffect();
  const std::vector<uint8_t> mati = MatiCchN(0x0ec41df0, {{"TCH0", Fresh(8)}}, cch);
  EffectNode node;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the visor rain effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) {
      atlas.id = 0x5EED0008;
    }
    return atlas;
  };
  std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
  std::vector<RetailPartProperty> properties;
  if (parts.size() == 1) {
    SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
  }
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  const std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(parts.size() == 1 && !vmat.empty() && parts[0].approximated.empty(),
            "0ec41df0 writes an exact VMAT");
  CHECK_MSG(VmatWord(vmat, 12) == (kVisorRain | kUnfogged),
            "0ec41df0 is VisorRain, Unfogged (the visor composite pass has no fog)");
  CHECK_MSG(VmatWord(vmat, 20) == 1, "0ec41df0's TCH0 is its only texture");
  // The role words follow the one texture entry (11 words at 24): colour first, then the absent slots.
  CHECK_MSG(VmatWord(vmat, 24 + 44) == 0 && VmatWord(vmat, 24 + 44 + 4) == ~0u,
            "0ec41df0's TCH0 is the colour slot, with no opacity map");
  // VPRM v1: version, the NMAP slot (-1), one row, then CCH0 whole.
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 12 + 16 && VmatWord(vprm, 8) == 1 && VmatWord(vprm, 16) == 1,
            "VPRM v1: no NMAP slot and one CCH row");
  CHECK_MSG(VmatWord(vprm, 20) == Bits(1.f) && VmatWord(vprm, 24) == Bits(0.12f) &&
                VmatWord(vprm, 28) == Bits(0.2f) && VmatWord(vprm, 32) == Bits(4.f),
            "VPRM carries the effect's CCH0 (1, .12, .2, 4)");
}

// A MATI with the header's material-flag list appended after its parameters: a LE u32 count then that
// many four-byte tags (the tags the material enables), as every shipped MATI header carries.
std::vector<uint8_t> MatiFlags(std::vector<uint8_t> mati, const std::vector<const char*>& flags) {
  const uint32_t count = uint32_t(flags.size());
  for (int i = 0; i < 4; ++i) {
    mati.push_back(uint8_t(count >> (8 * i)));
  }
  for (const char* flag : flags) {
    mati.insert(mati.end(), flag, flag + 4);
  }
  return mati;
}

// VFX_BarrierShield (f197ec38, the Parasite Queen ring barrier's MATI 6429c234): its refraction perm
// 002, the one the material flag MFC0 (USE_FRAME_BUFFER_DISTORTION) selects. TCH0 is the colour slot,
// TCH1 the second raw sample slot (the Indirect role, unwarped: this recipe does its own reads), CCH0
// whole in VPRM v1, the generator's blend kept. Perm 002 carries DFOG 1; without it the material draws
// the fogged sibling 003, and without MFC0 the perm-000 variant with its own vector displacement. Both
// are refused (no VMAT) rather than drawn with this perm's maths.
void TestVmatBarrierShieldRecipe() {
  constexpr uint32_t kBarrierShield = 1u << 29;
  // The MATI's own CCH0: (.1, 1, 0, 0).
  const std::vector<std::array<float, 4>> cch = {{0.1f, 1.f, 0.f, 0.f}};
  const std::vector<uint8_t> mati =
      MatiFlags(MatiCchN(0xf197ec38, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}}, cch), {"MFC0"});
  const std::vector<uint8_t> matiNoFlag = MatiCchN(0xf197ec38, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}}, cch);
  const std::vector<uint8_t> matiExterior =
      MatiFlags(MatiCchN(0xf197ec38, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}}, cch), {"MFC1"});
  const std::vector<uint8_t> matiNoNoise = MatiFlags(MatiCchN(0xf197ec38, {{"TCH0", Fresh(8)}}, cch), {"MFC0"});
  const auto effect = [](bool dfog) {
    std::vector<uint8_t> out = VmatEffect(true);
    if (dfog) {
      std::vector<uint8_t> extra;
      PutProperty(extra, "DFOG", 1);
      PutConstant(extra, 1);
      out.insert(out.end() - 13, extra.begin(), extra.end());
    }
    return out;
  };
  const auto convert = [](const std::vector<uint8_t>& data, const std::vector<uint8_t>& material,
                          std::vector<RetailPartProperty>& properties) {
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the barrier effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [&material](const EffectGuid&) { return material; };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      if (texture == Fresh(8)) {
        atlas.id = 0x5EED0008;
      }
      return atlas;
    };
    io.modelMesh = [](uint32_t) { return std::vector<uint8_t>{0, 0, 0, 2}; };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    if (parts.size() == 1) {
      SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    }
    return parts;
  };
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  const auto mentions = [](const std::vector<ConvertedPart>& parts, const char* what) {
    const std::vector<std::string> notes = parts.size() == 1 ? parts[0].approximated : std::vector<std::string>{};
    for (const std::string& line : notes) {
      if (line.find(what) != std::string::npos) {
        return true;
      }
    }
    return false;
  };

  std::vector<RetailPartProperty> properties;
  std::vector<ConvertedPart> parts = convert(effect(true), mati, properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(), "f197ec38 under MFC0 + DFOG 1 writes an exact VMAT");
  const std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  CHECK_MSG(VmatWord(vmat, 12) == kBarrierShield && VmatWord(vmat, 16) == 2 && VmatWord(vmat, 20) == 2,
            "f197ec38 VMAT: BarrierShield, the generator's blend 2 (PBDM), two textures");
  // The role words follow the two 11-word texture records: TCH0 the colour slot, TCH1 the second raw
  // sample slot (the Indirect role), the rest absent.
  CHECK_MSG(VmatWord(vmat, 112) == 0 && VmatWord(vmat, 116) == ~0u && VmatWord(vmat, 132) == 1 &&
                VmatWord(vmat, 136) == ~0u,
            "f197ec38: TCH0 is the colour slot and TCH1 the indirect sample slot");
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 12 + 16 && VmatWord(vprm, 8) == 1 && VmatWord(vprm, 12) == ~0u &&
                VmatWord(vprm, 16) == 1,
            "VPRM v1: no NMAP slot and one CCH row");
  CHECK_MSG(VmatWord(vprm, 20) == Bits(0.1f) && VmatWord(vprm, 24) == Bits(1.f) && VmatWord(vprm, 28) == Bits(0.f) &&
                VmatWord(vprm, 32) == Bits(0.f),
            "VPRM carries the MATI's CCH0 (.1, 1, 0, 0)");
  CHECK_MSG(!value(properties, EffectFourCC("VMSH")).empty(), "the barrier model keeps its VMSH");

  // Without the material flag the material is perm 000 (its own displacement, no refraction).
  properties.clear();
  parts = convert(effect(true), matiNoFlag, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "MFC0"),
            "f197ec38 without MFC0 writes no VMAT and says why");
  // The exterior flag is another perm (and its own shader).
  properties.clear();
  parts = convert(effect(true), matiExterior, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty(), "f197ec38 with MFC1 is refused");
  // The second pattern map is required.
  properties.clear();
  parts = convert(effect(true), matiNoNoise, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "TCH1"),
            "f197ec38 without TCH1 writes no VMAT");
  // Perm 002's sibling 003 is fogged, and is not ported.
  properties.clear();
  parts = convert(effect(false), mati, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "drawn fogged"),
            "f197ec38 without DFOG 1 writes no VMAT");
}

// VFX_BarrierGlow (b1eb2b67, the barrier's one-variable MATI ba0acbd6-d2d9-4a58-8e2d-f4802bcba587; FS
// perm 000, build/s-queenbarrier-20261024/GLOW-IMPLEMENTATION.md): the same barrier's non-refraction
// perm, whose MATI carries neither MFC0 (USE_FRAME_BUFFER_DISTORTION) nor MFC1 (IS_SHIELD_EXTERIOR).
// TCH0 is the only map this perm reads (three times, at its own uv) and TCH1 sits in the MATI unused;
// CCH0 whole in VPRM v1; the generator's PBDM 2 blend kept; the perm carries DFOG 1 and has no fog
// code, so a material without it is refused rather than drawn with this perm's maths. Only one slot:
// no scene copy is read, so no indirect (TCH1) slot either.
void TestVmatBarrierGlowRecipe() {
  constexpr uint32_t kBarrierGlow = 1u << 30;
  // The MATI's own CCH0: (.1, 1, 0, 0).
  const std::vector<std::array<float, 4>> cch = {{0.1f, 1.f, 0.f, 0.f}};
  const std::vector<uint8_t> mati = MatiCchN(0xb1eb2b67, {{"TCH0", Fresh(8)}, {"TCH1", Fresh(8)}}, cch);
  const std::vector<uint8_t> matiRefraction = MatiFlags(mati, {"MFC0"});
  const std::vector<uint8_t> matiExterior = MatiFlags(mati, {"MFC1"});
  const std::vector<uint8_t> matiNoPattern = MatiCchN(0xb1eb2b67, {}, cch);
  const std::vector<uint8_t> matiNoCch = Mati(0xb1eb2b67, {{"TCH0", Fresh(8)}});
  const auto effect = [](bool dfog) {
    std::vector<uint8_t> out = VmatEffect(true);
    if (dfog) {
      std::vector<uint8_t> extra;
      PutProperty(extra, "DFOG", 1);
      PutConstant(extra, 1);
      out.insert(out.end() - 13, extra.begin(), extra.end());
    }
    return out;
  };
  const auto convert = [](const std::vector<uint8_t>& data, const std::vector<uint8_t>& material,
                          std::vector<RetailPartProperty>& properties) {
    EffectNode node;
    std::string error;
    CHECK_MSG(ParseEffect(data.data(), data.size(), node, error), "the glow effect parses");
    EffectConvertIO io;
    io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
    io.materialData = [&material](const EffectGuid&) { return material; };
    io.vfxTexture = [](const EffectGuid& texture) {
      FlipbookAtlas atlas;
      if (texture == Fresh(8)) {
        atlas.id = 0x5EED0008;
      }
      return atlas;
    };
    io.modelMesh = [](uint32_t) { return std::vector<uint8_t>{0, 0, 0, 2}; };
    std::vector<ConvertedPart> parts = ConvertEffect(node, data.data(), io);
    if (parts.size() == 1) {
      SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error);
    }
    return parts;
  };
  const auto value = [](const std::vector<RetailPartProperty>& properties, uint32_t fourcc) {
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == fourcc) {
        return property.value;
      }
    }
    return std::vector<uint8_t>();
  };
  const auto mentions = [](const std::vector<ConvertedPart>& parts, const char* what) {
    const std::vector<std::string> notes = parts.size() == 1 ? parts[0].approximated : std::vector<std::string>{};
    for (const std::string& line : notes) {
      if (line.find(what) != std::string::npos) {
        return true;
      }
    }
    return false;
  };

  std::vector<RetailPartProperty> properties;
  std::vector<ConvertedPart> parts = convert(effect(true), mati, properties);
  CHECK_MSG(parts.size() == 1 && parts[0].approximated.empty(),
            "b1eb2b67 with neither shield flag writes an exact VMAT");
  const std::vector<uint8_t> vmat = value(properties, EffectFourCC("VMAT"));
  // The features carry the bit alone: perm 000 needs no unfogged pin (the shader has no fog code).
  CHECK_MSG(VmatWord(vmat, 12) == kBarrierGlow && VmatWord(vmat, 16) == 2 && VmatWord(vmat, 20) == 1,
            "b1eb2b67 VMAT: BarrierGlow, the generator's blend 2 (PBDM), one texture");
  // The role words: TCH0 is the colour slot, and no other slot is bound (TCH1 is in the MATI, unused).
  CHECK_MSG(VmatWord(vmat, 68) == 0 && VmatWord(vmat, 72) == ~0u && VmatWord(vmat, 88) == ~0u &&
                VmatWord(vmat, 92) == ~0u,
            "b1eb2b67: TCH0 is the colour slot, and the indirect slot is not bound");
  const std::vector<uint8_t> vprm = value(properties, EffectFourCC("VPRM"));
  CHECK_MSG(vprm.size() == 8 + 12 + 16 && VmatWord(vprm, 8) == 1 && VmatWord(vprm, 12) == ~0u &&
                VmatWord(vprm, 16) == 1,
            "VPRM v1: no NMAP slot and one CCH row");
  CHECK_MSG(VmatWord(vprm, 20) == Bits(0.1f) && VmatWord(vprm, 24) == Bits(1.f) && VmatWord(vprm, 28) == Bits(0.f) &&
                VmatWord(vprm, 32) == Bits(0.f),
            "VPRM carries the MATI's CCH0 (.1, 1, 0, 0)");
  CHECK_MSG(!value(properties, EffectFourCC("VMSH")).empty(), "the glow model keeps its VMSH");

  // Either shield flag is another perm: the refraction 002 with MFC0, the exterior's own shader with MFC1.
  properties.clear();
  parts = convert(effect(true), matiRefraction, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "non-refraction perm"),
            "b1eb2b67 with MFC0 writes no VMAT and says why");
  properties.clear();
  parts = convert(effect(true), matiExterior, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "non-refraction perm"),
            "b1eb2b67 with MFC1 is refused");
  // Its perm reads TCH0 and CCH0; without them there is nothing to draw.
  properties.clear();
  parts = convert(effect(true), matiNoPattern, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "TCH0"),
            "b1eb2b67 without TCH0 writes no VMAT");
  properties.clear();
  parts = convert(effect(true), matiNoCch, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "CCH0"),
            "b1eb2b67 without CCH0 writes no VMAT");
  // The perm carries DFOG 1 and has no fog code.
  properties.clear();
  parts = convert(effect(false), mati, properties);
  CHECK_MSG(value(properties, EffectFourCC("VMAT")).empty() && mentions(parts, "drawn fogged"),
            "b1eb2b67 without DFOG 1 writes no VMAT");
}

bool Contains(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& what) {
  return std::search(bytes.begin(), bytes.end(), what.begin(), what.end()) != bytes.end();
}

// Batch 4: the recipes for 20a05041, 3fe57e12, f0e276eb and a0482072.
void TestVmatBatch4Recipes() {
  auto features = [](uint32_t shader, std::vector<std::pair<const char*, EffectGuid>> textures, uint32_t& count) {
    auto result = ConvertVmat(Mati(shader, textures));
    count = VmatWord(result.second, 20);
    CHECK_MSG(result.first.size() == 1 && !result.second.empty() && result.first[0].approximated.empty(),
              "the batch 4 shader writes an exact VMAT");
    return VmatWord(result.second, 12);
  };
  // aurora::gfx::vfx::Feature bits.
  constexpr uint32_t kRamp = 8, kIndirect = 16, kDepthSoften = 32, kDualMod = 128, kColorIndexing = 512;
  uint32_t count = 0;
  const EffectGuid t = Fresh(8);
  CHECK_MSG(features(0x20a05041, {{"TCH0", t}, {"TCH1", t}}, count) == (kRamp | kIndirect | kDepthSoften) && count == 2,
            "20a05041 is fa49ff3c with DepthSoften");
  CHECK_MSG(features(0x3fe57e12, {{"TCH0", t}, {"TCH1", t}}, count) == (kRamp | kIndirect) && count == 2,
            "3fe57e12 is fa49ff3c's recipe");
  CHECK_MSG(features(0xf0e276eb, {{"TCH0", t}, {"TCH1", t}}, count) == (kRamp | kDualMod) && count == 2,
            "f0e276eb is cc9405ea's recipe");
  CHECK_MSG(features(0xa0482072, {{"BCLR", t}, {"TCH0", t}}, count) == kColorIndexing && count == 2,
            "a0482072 is 28bf8533's without the opacity texture");
}

void TestVmatPyroRecipes() {
  auto features = [](uint32_t shader, uint32_t& count) {
    const EffectGuid t = Fresh(8);
    auto result = ConvertVmat(Mati(shader, {{"TCH0", t}, {"TCH1", t}, {"TCH2", t}}));
    count = VmatWord(result.second, 20);
    CHECK_MSG(result.first.size() == 1 && !result.second.empty() && result.first[0].approximated.empty(),
              "the VFX_Pyro shader writes an exact VMAT");
    return VmatWord(result.second, 12);
  };
  // aurora::gfx::vfx::Feature bits.
  constexpr uint32_t kDepthSoften = 32, kPyro = 8192;
  uint32_t count = 0;
  CHECK_MSG(features(0x8278c22d, count) == kPyro && count == 2, "8278c22d is VFX_Pyro on TCH0 and TCH2");
  CHECK_MSG(features(0x7498e54a, count) == (kPyro | kDepthSoften) && count == 2,
            "7498e54a is VFX_Pyro with DepthSoften");
}

void TestVmatDisintegrationRecipe() {
  const EffectGuid t = Fresh(8);
  auto result = ConvertVmat(Mati(0xd749057f, {{"TCH0", t}}));
  CHECK_MSG(result.first.size() == 1 && !result.second.empty() && result.first[0].approximated.empty(),
            "the VFX_Disintegration shader writes an exact VMAT");
  // aurora::gfx::vfx::Feature::Disintegration, on the TCH0 mask only.
  CHECK_MSG(VmatWord(result.second, 12) == 16384 && VmatWord(result.second, 20) == 1,
            "d749057f is VFX_Disintegration on TCH0");
}

void TestVmatVisorSplatRecipe() {
  auto result = ConvertVmat(Mati(0x9e2cbd46, {{"BCLR", Fresh(8)}, {"TCH1", Fresh(8)}}));
  CHECK_MSG(result.first.size() == 1 && !result.second.empty() && result.first[0].approximated.empty(),
            "the VFX_VisorSplat shader writes an exact VMAT");
  // aurora::gfx::vfx::Feature::VisorSplat, on BCLR and the TCH1 warp map, neither warped by CCH0.
  CHECK_MSG(VmatWord(result.second, 12) == 65536 && VmatWord(result.second, 20) == 2,
            "9e2cbd46 is VFX_VisorSplat on BCLR and TCH1");
  if (result.second.size() >= 24 + 2 * 44) {
    CHECK_MSG(VmatWord(result.second, 24 + 32) == 0 && VmatWord(result.second, 68 + 32) == 0,
              "VisorSplat's warp scale is per particle (PMTR row 0), not the slot's");
  }
}

// VFX_FrameBuffer_Indirect_*: the SceneWarp feature, the warp scale CCH0.xy on the warp map's slot, the
// blend the generator's (no multiply) and nothing approximated.
void TestVmatSceneWarpRecipes() {
  constexpr uint32_t kColorTex = 1, kOpacityTex = 2, kSceneWarp = 32768;
  // Texture records are 11 words from 24 (both textures the one the test resolves); the slot of each
  // role follows them.
  const auto check = [](uint32_t shader, const char* first, const char* map, uint32_t features, int role,
                        const char* what) {
    auto [parts, blob] = ConvertVmat(MatiCch0(shader, {{first, Fresh(8)}, {map, Fresh(8)}}, 0.25f, -0.125f, 1.0f, 1.0f));
    CHECK_MSG(parts.size() == 1 && !blob.empty() && parts[0].approximated.empty(), what);
    if (blob.size() < 24 + 2 * 44 + 7 * 4) {
      return;
    }
    CHECK_MSG(VmatWord(blob, 12) == features && VmatWord(blob, 16) != 4 && VmatWord(blob, 20) == 2, what);
    CHECK_MSG(VmatWord(blob, 24) == 0x5EED0008 && VmatWord(blob, 68) == 0x5EED0008, what);
    // The first slot is not warped; the map's carries CCH0.x, CCH0.y.
    CHECK_MSG(VmatWord(blob, 24 + 32) == 0 && VmatWord(blob, 68 + 32) == 1, what);
    float u = 0.f, v = 0.f;
    const uint32_t uw = VmatWord(blob, 68 + 36), vw = VmatWord(blob, 68 + 40);
    std::memcpy(&u, &uw, 4);
    std::memcpy(&v, &vw, 4);
    CHECK_MSG(u == 0.25f && v == -0.125f, what);
    // Role slots: Color, Opacity, Ramp, Ramp2, Threshold, Indirect, Palette.
    CHECK_MSG(VmatWord(blob, 112 + 4 * role) == 0 && VmatWord(blob, 112 + 4 * 5) == 1, what);
  };
  check(0xcc155f99, "BCLR", "TCH0", kColorTex | kSceneWarp, 0, "cc155f99 is SceneWarp on BCLR, warped by TCH0");
  check(0x04276ae1, "BCLR", "TCH0", kColorTex | kSceneWarp, 0, "04276ae1 is SceneWarp on BCLR, warped by TCH0");
  check(0xd955396d, "TCH0", "TCH1", kOpacityTex | kSceneWarp, 1, "d955396d is SceneWarp on TCH0, warped by TCH1");
}

// An ELC2 child of a root, with `body` writing its properties.
template <typename Body>
std::vector<ConvertedPart> ConvertElc2(Body body) {
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
  body(out);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "ELC2 effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(8)}}); };
  io.vfxTexture = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) {
      atlas.id = 0x5EED0008;
    }
    return atlas;
  };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), io);
  return parts.size() == 2 ? std::vector<ConvertedPart>{parts[1]} : std::vector<ConvertedPart>{};
}

// A generator's SVC0 with tag 1 reads slot 0, the system's PSLT, which CElementGen::PortEvalSys
// holds in CParticleGlobals while it evaluates PSYS: its GRAD over ILPT is the port-only PLPT with
// offset 0, not retail's ILPT over the particle's life (cef0b80d).
void TestSystemVarGradLifetime() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SVC0", 1);
                 PutFourCC(o, "GRAD");
                 o.push_back(2);
                 for (uint16_t half : {0x3c00, 0x0000, 0x0000, 0x3c00}) {
                   o.push_back(uint8_t(half));
                   o.push_back(uint8_t(half >> 8));
                 }
                 Put32(o, Bits(0.0f));
                 for (uint16_t half : {0x0000, 0x0000, 0x3c00, 0x0000}) {
                   o.push_back(uint8_t(half));
                   o.push_back(uint8_t(half >> 8));
                 }
                 Put32(o, Bits(1.0f));
                 PutFourCC(o, "ILPT");
                 PutConstant(o, 100);
                 o.push_back(0);
               }),
               {});
  CHECK_MSG(parts.size() == 1, "SVC0 with a GRAD converts");
  if (parts.size() == 1) {
    Retail plpt;
    plpt.f("PLPT").w(0).f("CNST").w(100).b(0);
    CHECK_MSG(Contains(parts[0].part, plpt.bytes), "an SVC0 tag-1 GRAD over ILPT is PLPT offset 0");
    CHECK_MSG(parts[0].approximated.empty(), "the SVC0 tag-1 GRAD lifetime is exact");
    CHECK_MSG(parts[0].dropped.empty(), "nothing dropped");
  }
}

// An ELC2's EBDM is a raw byte (0 alpha, 1 additive, 2 premultiplied), and its COLR GRAD over ILPT
// reads the bolt's life, offset 0.
void TestElc2BlendAndLifetime() {
  const uint32_t want[] = {0, 2, 1};  // aurora::gfx::vfx::Blend of EBDM 0, 1, 2
  for (uint8_t ebdm = 0; ebdm < 3; ++ebdm) {
    auto parts = ConvertElc2([ebdm](auto& o) {
      PutProperty(o, "EBDM", 1);
      o.push_back(ebdm);
      PutProperty(o, "MTIN", 0);
      o.push_back(1);
      PutGuid(o, Fresh(7));
    });
    CHECK_MSG(parts.size() == 1, "ELC2 converts");
    if (parts.size() != 1) {
      continue;
    }
    std::string error;
    std::vector<RetailPartProperty> properties;
    CHECK_MSG(SplitRetailEffect(parts[0].type, parts[0].part.data(), parts[0].part.size(), properties, error),
              "ELC2 reads as retail");
    const RetailPartProperty* vmat = nullptr;
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == EffectFourCC("VMAT")) {
        vmat = &property;
      }
    }
    CHECK_MSG(vmat != nullptr, "an ELC2 with a material writes VMAT");
    if (vmat != nullptr) {
      CHECK_MSG(VmatWord(vmat->value, 16) == want[ebdm], "EBDM maps to the right blend");
    }
    CHECK_MSG(parts[0].approximated.empty(), "a byte EBDM approximates nothing");
  }

  auto parts = ConvertElc2([](auto& o) {
    PutProperty(o, "COLR", 3);
    PutFourCC(o, "GRAD");
    o.push_back(2);
    for (uint16_t half : {0x3c00, 0x0000, 0x0000, 0x3c00}) {
      o.push_back(uint8_t(half));
      o.push_back(uint8_t(half >> 8));
    }
    Put32(o, Bits(0.0f));
    for (uint16_t half : {0x0000, 0x0000, 0x3c00, 0x0000}) {
      o.push_back(uint8_t(half));
      o.push_back(uint8_t(half >> 8));
    }
    Put32(o, Bits(1.0f));
    PutFourCC(o, "ILPT");
    PutConstant(o, 100);
    o.push_back(1);
  });
  CHECK_MSG(parts.size() == 1, "ELC2 with a COLR GRAD converts");
  if (parts.size() == 1) {
    Retail plpt;
    plpt.f("PLPT").w(0).f("CNST").w(100).b(1);
    CHECK_MSG(Contains(parts[0].part, plpt.bytes), "ELC2 COLR GRAD over ILPT is PLPT offset 0");
    CHECK_MSG(parts[0].approximated.empty(), "the ELC2 COLR GRAD is exact");
  }
}

// An ELC2's NCLR GRAD over ILPT reads slot 2 = nodes - 1, which CParticleElectric2Port sets as the
// lifetime around the evaluation: PLPT offset 0. Its XFMD 3 is the port-only follow flag; 2 is not written.
void TestElc2NclrAndXfmd() {
  auto parts = ConvertElc2([](auto& o) {
    PutProperty(o, "NCLR", 5);
    PutFourCC(o, "GRAD");
    o.push_back(2);
    for (uint16_t half : {0x3c00, 0x0000, 0x0000, 0x3c00}) {
      o.push_back(uint8_t(half));
      o.push_back(uint8_t(half >> 8));
    }
    Put32(o, Bits(0.0f));
    for (uint16_t half : {0x0000, 0x0000, 0x3c00, 0x0000}) {
      o.push_back(uint8_t(half));
      o.push_back(uint8_t(half >> 8));
    }
    Put32(o, Bits(1.0f));
    PutFourCC(o, "ILPT");
    PutConstant(o, 100);
    o.push_back(1);
    PutProperty(o, "XFMD", 1);
    PutConstant(o, 3);
  });
  CHECK_MSG(parts.size() == 1, "ELC2 with an NCLR GRAD and XFMD 3 converts");
  if (parts.size() == 1) {
    Retail plpt;
    plpt.f("PLPT").w(0).f("CNST").w(100).b(1);
    CHECK_MSG(Contains(parts[0].part, plpt.bytes), "ELC2 NCLR GRAD over ILPT is PLPT offset 0");
    Retail follow;
    follow.f("XFMD").f("CNST").w(3);
    CHECK_MSG(Contains(parts[0].part, follow.bytes), "ELC2 XFMD 3 is written as the follow flag");
    CHECK_MSG(parts[0].approximated.empty(), "the ELC2 NCLR GRAD is exact");
  }
  parts = ConvertElc2([](auto& o) {
    PutProperty(o, "XFMD", 1);
    PutConstant(o, 2);
  });
  if (parts.size() == 1) {
    Retail follow;
    follow.f("XFMD");
    CHECK_MSG(!Contains(parts[0].part, follow.bytes), "ELC2 XFMD 2 is not written");
  }
}

// A generator with a PMDL but no TEXR or MTIN draws only models: its SIZE is
// left out and a SIZE of 0 written instead, as with one that draws nothing.
void TestModelsOnly() {
  const EffectConvertIO io = AtlasIO();
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutModels(o, 4, 3);
                 PutProperty(o, "SIZE", 3);
                 PutConstant(o, Bits(2.0f));
               }),
               io);
  Retail want;
  want.f("GPSM").f("PMDL").f("CNST").w(0xC0DE000A).f("PMDV").f("CNST").w(4);
  for (uint32_t i = 0; i < 4; ++i) {
    want.f("CNST").w(0xC0DE000A + i);
  }
  want.f("SIZE").f("CNST").w(Bits(0.0f)).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "a models-only generator gets SIZE 0");
  CHECK_MSG(!parts.empty() && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "SIZE: the generator draws models only",
            "its SIZE is dropped as models only");
  CHECK_MSG(!parts.empty() && parts[0].droppedRetail == 0, "models only drops no retail property");
}

// PMRQ's camera-facing form: RADD(RAZY(SUB_(CPSS, PLOC), CNST(0, 1, 0), REUL(0, 0, 0, #00)),
// REUL(x, y, z, #00)). `angles` writes the second REUL's three angle arguments.
template <typename Angles>
void PutFacingPmrq(std::vector<uint8_t>& out, Angles angles) {
  PutProperty(out, "PMRQ", 3);
  PutFourCC(out, "RADD");
  PutFourCC(out, "RAZY");
  PutFourCC(out, "SUB_");
  PutFourCC(out, "CPSS");
  PutFourCC(out, "PLOC");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, Bits(1.0f));
  PutConstant(out, 0);
  PutStaticEuler(out, 0.f, 0.f, 0.f);
  PutFourCC(out, "REUL");
  angles(out);
  out.push_back(0);
}

void TestFacingRotation() {
  // RAZY becomes the port-only PRZY on Remastered-space vectors (port state read through BSIS),
  // inside the same B conjugation as any other PMRQ.
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutFacingPmrq(o, [](auto& o) {
                   PutConstant(o, 0);
                   PutConstant(o, 0);
                   PutConstant(o, Bits(90.0f));
                 });
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("PMRT").f("PQAD");
  RetailVector(want, 90.f, 0.f, 180.f).f("PQAD").f("PQAD");
  want.f("PRZY").f("SUB_").f("BSIS").f("CPSS").f("BSIS").f("PLOC");
  want.f("CNST").f("CNST").w(0).f("CNST").w(Bits(1.0f)).f("CNST").w(0);
  RetailVector(want, 0.f, 0.f, 0.f);
  want.f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(90.0f));
  RetailVector(want, 90.f, 0.f, 180.f);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "a facing PMRQ becomes PMRT with PRZY");
  CHECK_MSG(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(),
            "a facing PMRQ is exact");

  // An IRND spin is kept as a real expression (PIRN samples it once per particle).
  parts = ConvertOne(OneGenerator([](auto& o) {
              PutFacingPmrq(o, [](auto& o) {
                PutFourCC(o, "IRND");
                PutConstant(o, Bits(0.0f));
                PutConstant(o, Bits(360.0f));
                PutConstant(o, 0);
                PutConstant(o, 0);
              });
              PutTexr(o, 0x1234ABCD);
            }),
            {});
  bool pmrt = false;
  if (!parts.empty()) {
    std::string error;
    std::vector<RetailPartProperty> properties;
    if (SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        pmrt = pmrt || property.fourcc == EffectFourCC("PMRT");
      }
    }
  }
  CHECK_MSG(pmrt && parts[0].dropped.empty() && parts[0].approximated.empty(),
            "a facing PMRQ with an IRND spin is converted exactly");

  // A RAZY fallback that is not a rotation element is refused.
  parts = ConvertOne(OneGenerator([](auto& o) {
              PutProperty(o, "PMRQ", 3);
              PutFourCC(o, "RAZY");
              PutFourCC(o, "PLOC");
              PutFourCC(o, "PVEL");
              PutFourCC(o, "CNST");
              PutConstant(o, 0);
              PutConstant(o, 0);
              PutConstant(o, 0);
              PutTexr(o, 0x1234ABCD);
            }),
            {});
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == plain.bytes, "a RAZY with a vector fallback writes nothing");
  CHECK_MSG(!parts.empty() && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0] == "PMRQ: a rotation that is not REUL/RADD/RAZY/MPCB/EXTR/CMRT",
            "a RAZY with a vector fallback is dropped");
}

int main() {
  TestVmat();
  TestVmsh();
  TestSftdReport();
  TestAtlasTexture();
  TestModelChoice();
  TestColourChoice();
  TestRealChoice();
  TestRetailId();
  TestPlasmaNestedHdrLight();
  TestSwooshElectric();
  TestSwooshParticleVars();
  TestSwooshRazyProt();
  TestSwooshZProt();
  TestConvert();
  TestRejects();
  TestSingleNode();
  TestMappedElements();
  TestScalarElements();
  TestPortVectors();
  TestPlaneElements();
  TestExternalValue();
  TestSplitRejects();
  TestShapes();
  TestSystemVectors();
  TestParticleSpace();
  TestGradientFrames();
  TestSpawnTable();
  TestPortSpawnTables();
  TestPortSpawnComponents();
  TestPortMover();
  TestDrawsNothing();
  TestBlendMode();
  TestUnfogged();
  TestInheritedMotion();
  TestDepthBias();
  TestDvvnDefaultSize();
  TestShutdownAndPivot();
  TestGpuAvailability();
  TestCurlNoise();
  TestParentRead();
  TestSystemVars();
  TestSystemVarGradLifetime();
  TestModelRotation();
  TestComposedModelRotation();
  TestModelRotationBasisEvaluation();
  TestVmatNewRecipes();
  TestVmatBatch4Recipes();
  TestVmatPyroRecipes();
  TestVmatDisintegrationRecipe();
  TestVmatVisorRunOffRecipes();
  TestVmatVisorRainRecipe();
  TestVmatBarrierShieldRecipe();
  TestVmatBarrierGlowRecipe();
  TestVmatVisorSplatRecipe();
  TestVmatSceneWarpRecipes();
  TestVmatIceChargeBeamRecipe();
  TestVmatIceChargeCollisionRecipe();
  TestVmatIceChargeCollisionLitRecipe();
  TestVmatParticleLit();
  TestElc2BlendAndLifetime();
  TestElc2NclrAndXfmd();
  TestWaveOrbHeaderBlend();
  TestWaveOrbExposurePolicy();
  TestPowerBallExposurePolicy();
  TestPowerChargeExposurePolicy();
  TestSmtrCch0Overrides();
  TestPowerOpacityGainBias();
  TestModelsOnly();
  TestFacingRotation();
  TestModelRotationFrames();
  TestIntScale();
  TestIntFrame();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_effect_convert: ok\n");
  }
  return PORT_TEST_RESULT();
}

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
