// Converter shapes: a one-stop GRAD (CCEFastConstant) is a constant colour, KESP (KEYP on time
// slot 2, the ELC2 node tween) becomes the port's KESP around a percent KEYP, and NCLR is typed as a
// colour so MDAO takes its two arguments.
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

// GRAD with one stop and no tail: 0x3c00 = 1.0, 0x3800 = 0.5, 0x3400 = 0.25, 0x4000 = 2.0.
void TestOneStopGradient() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "GRAD");
  out.push_back(1);
  for (uint16_t half : {0x3c00, 0x3800, 0x3400, 0x4000}) {
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  Put32(out, Bits(0.0f));
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  std::vector<uint8_t> want = {'C', 'O', 'L', 'R', 'C', 'N', 'S', 'T'};
  for (float f : {1.0f, 0.5f, 0.25f, 2.0f}) {
    Append(want, {'C', 'N', 'S', 'T'});
    Append(want, BE(Bits(f)));
  }
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "a one-stop GRAD is a constant colour");
  if (parts.size() == 1) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailEffect(parts[0].type, parts[0].part.data(), parts[0].part.size(), properties, error),
              "the one-stop GRAD's PART reads as retail");
    CHECK_MSG(!Contains(parts[0].part, {'P', 'G', 'R', 'D'}), "no PGRD for a one-stop GRAD");
  }
}

// KESP over keys 7.5 and 9 is CNST(7.5).
void TestKesp() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "KESP");
  Put32(out, 1);  // percent
  Put32(out, 0);
  out.push_back(0);
  out.push_back(0);
  Put32(out, 0);
  Put32(out, 0);
  Put32(out, 2);  // count
  Put32(out, Bits(7.5f));
  Put32(out, Bits(9.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  std::vector<uint8_t> want = {'S', 'I', 'Z', 'E', 'K', 'E', 'S', 'P', 'K', 'E', 'Y', 'P'};
  Append(want, BE(1));
  Append(want, BE(0));
  want.push_back(0);
  want.push_back(0);
  Append(want, BE(0));
  Append(want, BE(0));
  Append(want, BE(2));
  Append(want, BE(Bits(7.5f)));
  Append(want, BE(Bits(9.0f)));
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want),
            "KESP stays a port KESP around a percent KEYP of its keys");
}

}  // namespace

int main() {
  TestOneStopGradient();
  TestKesp();
  return PORT_TEST_RESULT();
}
