// CRLN with octaves: a mod-vector CRLN whose octave slot is set is the port-only PCRM around PCRO
// (P, frequency, octaves, gain, A).
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

void PutCnst(std::vector<uint8_t>& out, float value) {
  PutFourCC(out, "CNST");
  Put32(out, Bits(value));
}

std::vector<uint8_t> Fcc(const char* text) { return {uint8_t(text[0]), uint8_t(text[1]), uint8_t(text[2]), uint8_t(text[3])}; }

std::vector<uint8_t> Crln(uint8_t slice) {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "CRLN");
  PutFourCC(out, "CRLN");
  out.push_back(0);
  out.push_back(slice);
  PutFourCC(out, "VECF");
  out.insert(out.end(), 16, 0);  // no field
  PutFourCC(out, "PLOC");
  PutCnst(out, 1.f);                 // frequency
  PutFourCC(out, "CNST");            // octaves, an int
  Put32(out, 3);
  PutCnst(out, 0.5f);                // gain
  PutFourCC(out, "CNST");            // A
  PutCnst(out, 0.5f);
  PutCnst(out, 0.25f);
  PutCnst(out, 2.f);
  out.push_back(0);                  // mode
  PutCnst(out, 0.f);                 // t
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  return out;
}

void TestOctaves() {
  std::vector<uint8_t> in = Crln(0);
  const std::vector<ConvertedPart> parts = Convert(in);
  std::vector<uint8_t> want = Fcc("PCRM");
  Append(want, BE(0));
  Append(want, Fcc("BSIS"));
  Append(want, Fcc("PCRO"));
  Append(want, Fcc("BSIS"));
  Append(want, Fcc("PLOC"));
  Append(want, Fcc("CNST"));
  Append(want, BE(Bits(1.f)));  // frequency
  Append(want, Fcc("CNST"));
  Append(want, BE(3));          // octaves
  Append(want, Fcc("CNST"));
  Append(want, BE(Bits(0.5f))); // gain
  Append(want, Fcc("CNST"));
  Append(want, Fcc("CNST"));
  Append(want, BE(Bits(0.5f)));
  Append(want, Fcc("CNST"));
  Append(want, BE(Bits(0.25f)));
  Append(want, Fcc("CNST"));
  Append(want, BE(Bits(2.f)));  // A
  Append(want, Fcc("CNST"));
  Append(want, BE(Bits(0.f)));  // t
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "octave CRLN is PCRM around PCRO");
  if (parts.size() == 1) {
    CHECK_MSG(parts[0].dropped.empty(), "nothing is dropped");
    CHECK_MSG(!Contains(parts[0].part, Fcc("CRLN")), "no CRLN left");
  }
}

void TestOtherSlice() {
  std::vector<uint8_t> in = Crln(2);
  const std::vector<ConvertedPart> parts = Convert(in);
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.size() == 1 &&
                parts[0].dropped[0].find("CRLN slice 2 with octaves not ported") != std::string::npos,
            "an octave CRLN of slice 2 stays dropped");
}

}  // namespace

int main() {
  TestOctaves();
  TestOtherSlice();
  return PORT_TEST_RESULT();
}
