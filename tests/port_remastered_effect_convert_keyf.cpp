// KEYF (version 2) in a component real: the port-only PKYF with the loop byte, N, A, B, R, the keys
// and the input element; another version, or a key count that differs from N, is dropped.
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

// A KEYF block: header (version, loop, N, A, B, R, count) and the keys, then a CNST 3 input, or a
// real GTCP.
void PutKeyf(std::vector<uint8_t>& out, uint32_t version, bool loop, uint32_t n, uint32_t count,
             const std::vector<float>& keys, bool gtcp = false) {
  PutFourCC(out, "KEYF");
  Put32(out, version);
  Put32(out, 0);
  out.push_back(loop ? 1 : 0);
  out.push_back(0);
  Put32(out, n);
  Put32(out, Bits(1.0f));  // A
  Put32(out, Bits(0.5f));  // B
  Put32(out, Bits(2.0f));  // R
  Put32(out, count);
  for (float key : keys) {
    Put32(out, Bits(key));
  }
  if (gtcp) {
    PutFourCC(out, "GTCP");
    return;
  }
  PutFourCC(out, "CNST");
  Put32(out, Bits(3.0f));
}

std::vector<ConvertedPart> ConvertSize(uint32_t version, bool loop, uint32_t n, uint32_t count,
                                       const std::vector<float>& keys, bool gtcp = false) {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "SIZE", 3);
  PutKeyf(out, version, loop, n, count, keys, gtcp);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  return Convert(out);
}

void TestKeyf() {
  const std::vector<ConvertedPart> parts = ConvertSize(2, true, 3, 3, {0.f, 10.f, 20.f});
  std::vector<uint8_t> want = {'S', 'I', 'Z', 'E', 'P', 'K', 'Y', 'F', 1};
  Append(want, BE(3));
  Append(want, BE(Bits(1.0f)));
  Append(want, BE(Bits(0.5f)));
  Append(want, BE(Bits(2.0f)));
  for (float key : {0.f, 10.f, 20.f}) {
    Append(want, BE(Bits(key)));
  }
  Append(want, {'C', 'N', 'S', 'T'});
  Append(want, BE(Bits(3.0f)));
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "KEYF maps onto the port-only PKYF");
  if (parts.size() == 1) {
    CHECK_MSG(parts[0].dropped.empty(), "nothing is dropped");
  }
}

void TestKeyfNoLoop() {
  const std::vector<ConvertedPart> parts = ConvertSize(2, false, 1, 1, {7.f});
  const std::vector<uint8_t> want = {'P', 'K', 'Y', 'F', 0, 0, 0, 0, 1};
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "a non-looping KEYF has loop byte 0");
}

void TestKeyfDropped() {
  std::vector<ConvertedPart> parts = ConvertSize(1, false, 2, 2, {0.f, 1.f});
  CHECK_MSG(parts.size() == 1 && !Contains(parts[0].part, {'P', 'K', 'Y', 'F'}) &&
                !parts[0].dropped.empty(),
            "another version is dropped with a reason");
  parts = ConvertSize(2, false, 2, 3, {0.f, 1.f, 2.f});
  CHECK_MSG(parts.size() == 1 && !Contains(parts[0].part, {'P', 'K', 'Y', 'F'}) &&
                !parts[0].dropped.empty(),
            "a key count that differs from N is dropped with a reason");
}

// A vector KEYF (three floats per key) as an emitter's position, as the two shipped ones.
std::vector<ConvertedPart> ConvertEmitter(uint32_t n, uint32_t count, const std::vector<float>& keys,
                                          bool gtcp = false) {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "SEMR");
  PutKeyf(out, 2, false, n, count, keys, gtcp);
  PutFourCC(out, "CNST");
  for (int i = 0; i < 3; ++i) {
    PutFourCC(out, "CNST");
    Put32(out, Bits(0.f));
  }
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  return Convert(out);
}

void TestVectorKeyf() {
  const std::vector<float> keys = {1.f, 2.f, 3.f, -4.f, 5.f, -6.f};
  std::vector<ConvertedPart> parts = ConvertEmitter(2, 2, keys);
  std::vector<uint8_t> want = {'P', 'K', 'Y', 'F', 0};
  Append(want, BE(2));
  Append(want, BE(Bits(1.0f)));
  Append(want, BE(Bits(0.5f)));
  Append(want, BE(Bits(2.0f)));
  for (float key : keys) {
    Append(want, BE(Bits(key)));
  }
  Append(want, {'C', 'N', 'S', 'T'});
  Append(want, BE(Bits(3.0f)));
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "a vector KEYF maps onto PKYF with 3 floats a key");
  if (parts.size() == 1) {
    CHECK_MSG(parts[0].dropped.empty(), "nothing is dropped");
  }
  // Keys sized for a real KEYF are not a vector KEYF.
  parts = ConvertEmitter(2, 2, {1.f, 2.f});
  CHECK_MSG(parts.size() == 1 && !Contains(parts[0].part, {'P', 'K', 'Y', 'F'}) && !parts[0].dropped.empty(),
            "a vector KEYF with 4-byte keys is dropped");
}

// A real GTCP is the port-only PGTC in an emitter, and dropped elsewhere.
void TestGtcp() {
  const std::vector<float> keys = {1.f, 2.f, 3.f, -4.f, 5.f, -6.f};
  std::vector<ConvertedPart> parts = ConvertEmitter(2, 2, keys, true);
  std::vector<uint8_t> want;
  for (float key : {5.f, -6.f}) {
    Append(want, BE(Bits(key)));
  }
  Append(want, {'P', 'G', 'T', 'C'});
  CHECK_MSG(parts.size() == 1 && Contains(parts[0].part, want), "an emitter's real GTCP is PGTC");
  if (parts.size() == 1) {
    CHECK_MSG(parts[0].dropped.empty(), "nothing is dropped");
  }
  parts = ConvertSize(2, false, 1, 1, {7.f}, true);
  CHECK_MSG(parts.size() == 1 && !Contains(parts[0].part, {'P', 'G', 'T', 'C'}) && !parts[0].dropped.empty(),
            "a real GTCP outside an emitter is dropped");
}

}  // namespace

int main() {
  TestKeyf();
  TestKeyfNoLoop();
  TestKeyfDropped();
  TestVectorKeyf();
  TestGtcp();
  return PORT_TEST_RESULT();
}
