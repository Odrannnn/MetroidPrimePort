// ROTV (regular) and PNCV in a component vector: ROTV by REUL (axis order 0), EXTR or CMRT is the
// port-only PRTV, PNCV is the port-only PNCV, and anything else is dropped with a reason.
#include "port_remastered_effect_convert.h"
#include "port_retail_check.h"
#include "port_test.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

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

std::vector<uint8_t> Header() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, 0);
  return out;
}

// A retail texture id, so the generator draws something.
void PutTexr(std::vector<uint8_t>& out, uint32_t retail) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  const uint8_t guid[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  out.insert(out.end(), guid, guid + 12);
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(retail >> shift));
  }
  PutFourCC(out, "NONE");
}

std::vector<uint8_t> BE(uint32_t word) { return {uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)}; }

bool Contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) {
      return true;
    }
  }
  return false;
}

void Append(std::vector<uint8_t>& to, const std::vector<uint8_t>& from) { to.insert(to.end(), from.begin(), from.end()); }

std::vector<ConvertedPart> Convert(std::vector<uint8_t>& data) {
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  return PortRemastered::CheckedConvert(effect, data.data());
}

void PutVec(std::vector<uint8_t>& out, float x, float y, float z) {
  PutFourCC(out, "CNST");
  for (float f : {x, y, z}) {
    PutFourCC(out, "CNST");
    Put32(out, Bits(f));
  }
}

std::vector<uint8_t> Be(const char* text) { return std::vector<uint8_t>(text, text + 4); }

void PutReal(std::vector<uint8_t>& out, float f) {
  PutFourCC(out, "CNST");
  Put32(out, Bits(f));
}

// A generator whose POFS is `vector`, and the converted part.
template <typename Fn>
ConvertedPart ConvertPofs(Fn vector) {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "POFS", 0);
  vector(out);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  return parts.size() == 1 ? parts[0] : ConvertedPart{};
}

void PutRotv(std::vector<uint8_t>& out, const char* rotation) {
  PutFourCC(out, "ROTV");
  PutVec(out, 1.f, 0.f, 0.f);
  PutFourCC(out, rotation);
}

void TestRotvReul() {
  const ConvertedPart part = ConvertPofs([](auto& o) {
    PutRotv(o, "REUL");
    PutReal(o, 0.f);
    PutReal(o, 0.f);
    PutReal(o, 90.f);
    o.push_back(0);
  });
  std::vector<uint8_t> want = Be("POFS");
  for (const char* t : {"BSIS", "PRTV", "CNST"}) {
    Append(want, Be(t));
  }
  for (float f : {1.f, 0.f, 0.f}) {
    Append(want, Be("CNST"));
    Append(want, BE(Bits(f)));
  }
  Append(want, Be("REUL"));
  for (float f : {0.f, 0.f, 90.f}) {
    Append(want, Be("CNST"));
    Append(want, BE(Bits(f)));
  }
  CHECK_MSG(Contains(part.part, want), "ROTV by REUL is PRTV, the vector, REUL and three angles");
  CHECK_MSG(!Contains(part.part, Be("ROTV")) && part.dropped.empty(), "ROTV by REUL drops nothing");
}

void TestRotvExternal() {
  for (const char* rotation : {"EXTR", "CMRT"}) {
    const ConvertedPart part = ConvertPofs([&](auto& o) { PutRotv(o, rotation); });
    std::vector<uint8_t> want = Be("PRTV");
    Append(want, Be("CNST"));
    for (float f : {1.f, 0.f, 0.f}) {
      Append(want, Be("CNST"));
      Append(want, BE(Bits(f)));
    }
    Append(want, Be(rotation));
    CHECK_MSG(Contains(part.part, want), "ROTV by EXTR/CMRT is PRTV with that kind");
  }
}

void TestRotvDropped() {
  ConvertedPart part = ConvertPofs([](auto& o) {
    PutRotv(o, "REUL");
    PutReal(o, 0.f);
    PutReal(o, 0.f);
    PutReal(o, 90.f);
    o.push_back(1);
  });
  CHECK_MSG(!Contains(part.part, Be("PRTV")), "an axis order other than 0 is not converted");
  CHECK_MSG(!part.dropped.empty(), "the dropped ROTV is reported");
}

void TestPncv() {
  const ConvertedPart part = ConvertPofs([](auto& o) { PutFourCC(o, "PNCV"); });
  std::vector<uint8_t> want = Be("POFS");
  Append(want, Be("BSIS"));
  Append(want, Be("PNCV"));
  CHECK_MSG(Contains(part.part, want) && part.dropped.empty(), "PNCV maps onto the port-only PNCV");
}

// MPCB(GRAV(v)) runs v in retail's space, so Remastered's PNCV and PRTV get a BSIS there.
void TestSpaceInMpcb() {
  for (bool prtv : {false, true}) {
    std::vector<uint8_t> out = Header();
    PutProperty(out, "VEL1", 3);
    PutFourCC(out, "MPCB");
    PutFourCC(out, "GRAV");
    if (prtv) {
      PutRotv(out, "EXTR");
    } else {
      PutFourCC(out, "PNCV");
    }
    PutProperty(out, "_END", 4);
    const std::vector<ConvertedPart> parts = Convert(out);
    std::vector<uint8_t> want = Be("GRAV");
    Append(want, Be("BSIS"));
    Append(want, Be(prtv ? "PRTV" : "PNCV"));
    CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want),
              "a PNCV/PRTV inside MPCB(mod vector) is turned into retail's space by BSIS");
  }
}

// INTR and RLCL rotations are both the system rotation (0x25d59c, var 0xb): PRTV kind INTR.
void TestRotvSystemRotation() {
  for (const char* kind : {"INTR", "RLCL"}) {
    const ConvertedPart part = ConvertPofs([&](auto& o) { PutRotv(o, kind); });
    CHECK_MSG(Contains(part.part, Be("PRTV")) && Contains(part.part, Be("INTR")) && part.dropped.empty(),
              "ROTV by INTR/RLCL is PRTV kind INTR");
  }
}

}  // namespace

int main() {
  TestRotvReul();
  TestRotvExternal();
  TestRotvDropped();
  TestPncv();
  TestSpaceInMpcb();
  TestRotvSystemRotation();
  return PORT_TEST_RESULT();
}
