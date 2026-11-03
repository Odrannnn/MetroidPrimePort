// Converter output for the particle variables ADV9 and IAV0..3: the port-only PPVR table (sorted,
// with each one's constant-after-start flag), the PAP0 and PAP9 leaves, and no PPVR for a generator
// that only has ADV1..8.
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


void PutCnst(std::vector<uint8_t>& out, uint32_t word) {
  PutFourCC(out, "CNST");
  Put32(out, word);
}

std::vector<uint8_t> Four(const char* text) {
  return {uint8_t(text[0]), uint8_t(text[1]), uint8_t(text[2]), uint8_t(text[3])};
}

// Appends each word big endian, four-character codes as their text.
void Be(std::vector<uint8_t>& to, std::initializer_list<uint32_t> words) {
  for (uint32_t w : words) {
    Append(to, BE(w));
  }
}

// IAV0 = IRND(1, 63), ADV9 = 0.01, SIZE = ITRL(PAP0, 1.0): a PPVR of the two in stream order, IAV0
// as the one-draw PRAN, both constant after start; SIZE reads PAP0 as an int leaf.
void TestPpvr() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "IAV0", 0);
  PutFourCC(out, "IRND");
  PutCnst(out, 1);
  PutCnst(out, 63);
  PutProperty(out, "ADV9", 0);
  PutCnst(out, Bits(0.01f));
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "ITRL");
  PutFourCC(out, "PAP0");
  PutCnst(out, Bits(1.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  std::vector<uint8_t> ppvr = Four("PPVR");
  Append(ppvr, Four("CNST"));
  Be(ppvr, {2});
  Be(ppvr, {2, 0, 1});  // int IAV0, constant after start
  Append(ppvr, Four("PRAN"));
  Append(ppvr, Four("CNST"));
  Be(ppvr, {1});
  Append(ppvr, Four("CNST"));
  Be(ppvr, {63});
  Be(ppvr, {1, 8, 1});  // ADV9
  Append(ppvr, Four("CNST"));
  Be(ppvr, {Bits(0.01f)});
  CHECK_MSG(Contains(parts[0].part, ppvr), "PPVR holds IAV0 as PRAN and ADV9 as a constant");
  std::vector<uint8_t> size = Four("SIZE");
  Append(size, Four("ITRL"));
  Append(size, Four("PAP0"));
  CHECK_MSG(Contains(parts[0].part, size), "SIZE keeps its PAP0 leaf");
  CHECK_MSG(!Contains(parts[0].part, Four("PADO")), "no PADO next to the PPVR");
}

// PAP9 as a real leaf of another property, with ADV9 = ADD_(PAP1, 1.0) not constant after start
// (it reads ADV1 = 2.0): the PPVR lists ADV1 first (flag 1), then ADV9 with flag 0.
void TestPap9Leaf() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "ADV9", 0);
  PutFourCC(out, "ADD_");
  PutFourCC(out, "PAP1");
  PutCnst(out, Bits(1.0f));
  PutProperty(out, "ADV1", 0);
  PutCnst(out, Bits(2.0f));
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "PAP9");
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  std::vector<uint8_t> item = Four("PPVR");
  Append(item, Four("CNST"));
  Be(item, {2, 0, 0, 1, 1, 8, 0});
  Append(item, Four("ADD_"));
  Append(item, Four("PAP1"));
  CHECK_MSG(Contains(parts[0].part, item), "ADV1 first, then ADV9 re-evaluated each frame (flag 0)");
  std::vector<uint8_t> size = Four("SIZE");
  Append(size, Four("PAP9"));
  CHECK_MSG(Contains(parts[0].part, size), "SIZE keeps its PAP9 leaf");
}

// The PPVR constant-after-start flag follows CElementDep::Finish (0x24f9a4): ADD_(CNST, CNST) is
// constant, RAND(CNST, CNST) is not (CRERandom::GetConstOverride 0x299dbc), a plain IRND is
// initial-only (flag 1, evaluated at spawn).
void TestConstantFlag() {
  struct Case {
    const char* op;
    uint32_t flag;
  };
  for (const Case c : {Case{"ADD_", 1}, Case{"RAND", 0}}) {
    std::vector<uint8_t> out = Header();
    PutProperty(out, "ADV9", 0);
    PutFourCC(out, c.op);
    PutCnst(out, Bits(1.0f));
    PutCnst(out, Bits(2.0f));
    PutTexr(out, 0x1234ABCD);
    PutProperty(out, "_END", 4);
    const std::vector<ConvertedPart> parts = Convert(out);
    CHECK_MSG(parts.size() == 1, "one part");
    if (parts.size() != 1) {
      continue;
    }
    std::vector<uint8_t> item = Four("PPVR");
    Append(item, Four("CNST"));
    Be(item, {1, 1, 8, c.flag});
    CHECK_MSG(Contains(parts[0].part, item), c.op);
  }
}

// ADV9 is dropped (a vector element where a real is read), ADV1 = PAP9 + 1 reads it and ADV2 = PAP1 + 1 reads ADV1: both
// readers are dropped with it, and their retail properties are not written.
void TestDroppedReader() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "ADV9", 0);
  PutFourCC(out, "PLNV");  // a vector element where a real is read
  PutProperty(out, "ADV1", 0);
  PutFourCC(out, "ADD_");
  PutFourCC(out, "PAP9");
  PutCnst(out, Bits(1.0f));
  PutProperty(out, "ADV2", 0);
  PutFourCC(out, "ADD_");
  PutFourCC(out, "PAP1");
  PutCnst(out, Bits(1.0f));
  PutProperty(out, "ADV3", 0);
  PutCnst(out, Bits(3.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  bool adv9 = false, adv1 = false, adv2 = false;
  for (const std::string& d : parts[0].dropped) {
    adv9 = adv9 || d.rfind("ADV9", 0) == 0;
    adv1 = adv1 || d == "ADV1: reads a dropped variable";
    adv2 = adv2 || d == "ADV2: reads a dropped variable";
  }
  CHECK_MSG(adv9 && adv1 && adv2, "ADV9 and its readers ADV1, ADV2 are dropped");
  CHECK_MSG(!Contains(parts[0].part, Four("ADV1")) && !Contains(parts[0].part, Four("ADV2")),
            "the readers' properties are not written");
  CHECK_MSG(Contains(parts[0].part, Four("ADV3")), "an unrelated ADV3 stays");
}

// ADV1..8 only: no PPVR, and the identity order writes no PADO either.
void TestNoPpvr() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "ADV1", 0);
  PutCnst(out, Bits(2.0f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() == 1) {
    CHECK_MSG(!Contains(parts[0].part, Four("PPVR")), "no PPVR without ADV9 or IAV");
    CHECK_MSG(!Contains(parts[0].part, Four("PADO")), "no PADO for the identity order");
  }
}

// LTME = RAND(SPAI(#0, 60), SPAI(#1, 100)): parent reads never fold to constants, so the RAND is
// CIERandom, the port-only PRAN over the two reads, not a drop.
void TestIntRandParentRead() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "LTME", 0);
  PutFourCC(out, "RAND");
  PutFourCC(out, "SPAI");
  out.push_back(0);
  PutCnst(out, 60);
  PutFourCC(out, "SPAI");
  out.push_back(1);
  PutCnst(out, 100);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  for (const std::string& d : parts[0].dropped) {
    CHECK_MSG(d.rfind("LTME", 0) != 0, d.c_str());
  }
  std::vector<uint8_t> ltme = Four("PRAN");
  Append(ltme, Four("SPAI"));
  Be(ltme, {0});
  Append(ltme, Four("CNST"));
  Be(ltme, {60});
  Append(ltme, Four("SPAI"));
  Be(ltme, {1});
  Append(ltme, Four("CNST"));
  Be(ltme, {100});
  CHECK_MSG(Contains(parts[0].part, ltme), "LTME reads a PRAN of the two SPAI reads");
}

// SPWT on a generator with no child system: nothing reads it, so it is neither written nor dropped.
void TestChildlessSpwt() {
  std::vector<uint8_t> out = Header();
  PutProperty(out, "SPWT", 1);
  PutFourCC(out, "CNST");
  PutCnst(out, Bits(0.f));
  PutCnst(out, Bits(0.5f));
  PutCnst(out, Bits(0.f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  const std::vector<ConvertedPart> parts = Convert(out);
  CHECK_MSG(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  for (const std::string& d : parts[0].dropped) {
    CHECK_MSG(d.rfind("SPWT", 0) != 0, d.c_str());
  }
  CHECK_MSG(!Contains(parts[0].part, Four("SPWT")), "no SPWT written");
}

}  // namespace

int main() {
  TestChildlessSpwt();
  TestIntRandParentRead();
  TestPpvr();
  TestPap9Leaf();
  TestNoPpvr();
  TestConstantFlag();
  TestDroppedReader();
  return PORT_TEST_RESULT();
}
