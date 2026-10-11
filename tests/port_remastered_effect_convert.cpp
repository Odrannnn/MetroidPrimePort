#include "port_remastered_effect_convert.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

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
  // The end of a converted PART: the port-only PIRN marker, then _END.
  Retail& end() { return f("PIRN").f("CNST").w(1).f("_END"); }
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

void TestRetailId() {
  Check(EffectRetailId(Legacy(0x0034CE07)) == 0x0034CE07u, "carried-over id resolves");
  Check(!EffectRetailId(Fresh(0)).has_value(), "a fresh id has no retail id");
}

void TestConvert() {
  const std::vector<uint8_t> data = Effect();
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }

  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(1) ? 0x5EED0001u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 2, "root and child");
  if (parts.size() != 2) {
    return;
  }

  Retail root;
  root.f("GPSM");
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
  Check(parts[0].root && parts[0].part == root.bytes, "root converts to retail's bytes");
  Check(parts[0].droppedRetail == 1, "PMDL with no retail id is the one retail property dropped");
  Check(parts[0].dropped.size() == 2, "DVVN and PMDL dropped");

  Retail child;
  child.f("GPSM");
  child.f("MAXP").f("CNST").w(5);
  child.f("TEXR").f("CNST").f("CNST").w(0x5EED0001);
  child.end();
  Check(!parts[1].root && parts[1].id == Legacy(0x0BADF00D), "child keeps its id");
  Check(parts[1].part == child.bytes, "MTIN becomes the child's TEXR");
  Check(parts[1].droppedRetail == 0, "child drops nothing");

  // What the converter writes reads as retail reads it.
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    Check(SplitRetailPart(part.part.data(), part.part.size(), properties, error), "converted PART reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(root.bytes.data(), root.bytes.size(), properties, error) && properties.size() == 10 &&
            properties[5].fourcc == EffectFourCC("COLR") && properties[5].value.size() == 4 + 22 + 32,
        "retail PART splits into its properties");
}

// The splitter refuses what retail's reader would not take.
void TestSplitRejects() {
  Retail bad;
  bad.f("GPSM").f("SIZE").f("RADD").f("_END");
  std::vector<RetailPartProperty> properties;
  std::string error;
  Check(!SplitRetailPart(bad.bytes.data(), bad.bytes.size(), properties, error) && !error.empty(),
        "unknown element refused");
  Retail spawn;
  spawn.f("GPSM").f("KSSM").f("CNST").w(0).w(0).w(10).w(0).w(1).w(3).w(1).w(0xAABBCCDD).w(0).w(0).w(0).f("_END");
  Check(SplitRetailPart(spawn.bytes.data(), spawn.bytes.size(), properties, error) && properties.size() == 1,
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
  Check(ParseEffect(out.data(), out.size(), effect, error), "single-node effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 1 && parts[0].root, "the only GPSM is the root");
  Retail want;
  want.f("GPSM").f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(1);
  want.w(Bits(1.0f)).w(Bits(0.5f)).w(Bits(0.0f)).w(Bits(-1.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "half colour keys widen to floats");
  Check(parts.size() == 1 && parts[0].dropped.empty(), "an empty KSSM is nothing left out");
}

// Remastered's new elements that have a retail equivalent: MPCB's angle form
// and an unrotated ANCR become ANGC, MPRD becomes RAND (and a random LTM2
// comes down by one at both ends), DFCP is kept as the port's element; a vector of nested
// constants is not read as an id.
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
  Check(ParseEffect(out.data(), out.size(), effect, error), "mapped-element effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("SEMR").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  want.f("ANGC").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(720.0f)).f("CNST").w(Bits(720.0f)).f("CNST").w(Bits(0.1f));
  want.f("LTME").f("RAND").f("CNST").w(16).f("CNST").w(32);
  want.f("SIZE").f("MULT").f("CNST").w(Bits(2.0f)).f("DFCP").f("CNST").w(Bits(2.0f)).f("CNST").w(0);
  want.f("POFS").f("ANGC").f("CNST").w(0x80000000u).f("CNST").w(0x80000000u);
  want.f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(0.5f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "new elements map onto retail's");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
  }
  Check(parts.size() == 1 && parts[0].approximated.empty(), "DFCP is kept, not approximated");
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
  Check(ParseEffect(out.data(), out.size(), effect, error), "reject effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "RADD in SIZE is dropped");
  Retail root;
  root.f("GPSM").f("LFOT").f("CNST").w(3).f("LTYP").f("CNST").w(2);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == root.bytes, "LFOT byte 3 is retail 3, LTYP byte 1 is retail 2");
}
}  // namespace

void PutReul(std::vector<uint8_t>& out, float x) {
  PutFourCC(out, "REUL");
  PutConstant(out, Bits(x));
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
}

// Remastered's shapes with no retail element of their own: ASPR becomes ASPH
// and a cone turned about X an X bias, RNDV a whole-sphere ANGC, and a GRAD
// colour gradient over the life 101 percent keys.
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
  Check(ParseEffect(out.data(), out.size(), effect, error), "shapes effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("ASPH").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  want.f("CNST").w(Bits(90.0f)).f("CNST").w(0x80000000u);
  want.f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(180.0f)).f("CNST").w(Bits(0.25f)).f("CNST").w(Bits(0.1f));
  want.f("PSIV").f("ANGC").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(360.0f));
  want.f("CNST").w(Bits(0.5f));
  want.f("COLR").f("KEYP").w(1).w(0).b(0).b(0).w(101).w(0).w(101);
  for (int percent = 0; percent <= 100; ++percent) {
    const float t = float(percent) / 100.0f;
    want.w(Bits(1.0f - t)).w(Bits(0.0f)).w(Bits(t)).w(Bits(1.0f - t));
  }
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "ASPR, RNDV and GRAD map onto retail's");
  if (parts.size() == 1) {
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
    std::vector<RetailPartProperty> split;
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), split, error), "shapes PART reads as retail");
    Check(parts[0].approximated.size() == 2, "rotated cone and RNDV listed as approximated");
  }
}

// A gradient over as many frames as the particle lives (LTM2) ends with the
// life: its last key is the last stop.
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
  Check(ParseEffect(out.data(), out.size(), effect, error), "frame gradient parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM").f("LTME").f("CNST").w(19).f("COLR").f("KEYP").w(1).w(0).b(0).b(0).w(101).w(0).w(101);
  for (int percent = 0; percent <= 100; ++percent) {
    const float t = float(percent) / 100.0f;
    for (int c = 0; c < 4; ++c) {
      want.w(Bits(1.0f - t));
    }
  }
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a gradient over the life in frames ends with it");
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
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(2));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "swoosh and electric effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(2) ? 0x5EED0002u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), io);
  Check(parts.size() == 4, "root and three children");
  if (parts.size() != 4) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }

  Retail root;
  root.f("GPSM").f("MAXP").f("CNST").w(3).f("SSWH").f("CNST").w(0x0000AAAA);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts[0].type == EffectFourCC("PART") && parts[0].part == root.bytes, "root references the swoosh by id");

  Retail swoosh;
  swoosh.f("SWSH").f("LENG").f("CNST").w(8).f("AALP").f("CNST").b(1);
  swoosh.f("TEXR").f("CNST").f("CNST").w(0x5EED0002).f("PIRN").f("CNST").w(1).f("_END");
  Check(parts[1].type == EffectFourCC("SWHC") && parts[1].id == Legacy(0x0000AAAA), "swoosh child is an SWHC");
  Check(parts[1].part == swoosh.bytes, "SBDM becomes AALP and MTIN the swoosh's TEXR");
  Check(parts[1].droppedRetail == 0, "swoosh drops nothing");

  Retail electric;
  electric.f("ELSM").f("LWD1").f("CNST").w(Bits(2.0f)).f("SSWH").f("CNST").w(0x0000AAAA).f("GPSM").f("NONE").f("_END");
  Check(parts[2].type == EffectFourCC("ELSC") && parts[2].part == electric.bytes, "ELSM child is an ELSC");

  Retail elc2;
  elc2.f("ELSM").f("SCNT").f("CNST").w(4).f("_END");
  Check(parts[3].type == EffectFourCC("ELSC") && parts[3].part == elc2.bytes, "ELC2 child is an ELSC");
  Check(parts[3].dropped.size() == 1 && parts[3].droppedRetail == 0, "ELC2's MTIN is Remastered only");

  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    Check(SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error),
          "converted child reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  Check(!SplitRetailEffect(EffectFourCC("SWHC"), root.bytes.data(), root.bytes.size(), properties, error),
        "a PART is not an SWHC");
  Retail wrong;
  wrong.f("SWSH").f("MAXP").f("CNST").w(3).f("_END");
  Check(!SplitRetailEffect(EffectFourCC("SWHC"), wrong.bytes.data(), wrong.bytes.size(), properties, error),
        "a PART property in a swoosh refused");
  Check(SplitRetailEffect(EffectFourCC("ELSC"), electric.bytes.data(), electric.bytes.size(), properties, error) &&
            properties.size() == 3,
        "retail ELSC splits into its properties");
}

// Remastered's spawn tables merge into retail's one KSSM: generators by frame
// with the 16-byte entries retail reads, the first swoosh and electric child
// as SSWH/SSSD and SELC/SESD; further swooshes go to the port-only PSWX.
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
  Check(ParseEffect(out.data(), out.size(), effect, error), "spawn table effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 5, "root and four children");
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
  Check(parts[0].part == root.bytes, "tables merge into retail's KSSM, SSWH, SELC and PSWX");
  Check(parts[0].dropped.empty() && parts[0].droppedRetail == 0, "the second swoosh is kept as PSWX");
  bool merged = false;
  bool selected = false;
  bool conditional = false;
  for (const std::string& note : parts[0].approximated) {
    merged = merged || note == "KSSM: 2 tables merged";
    selected = selected || note == "KSSM: table selector ignored";
    conditional = conditional || note.find("conditions ignored") != std::string::npos;
  }
  Check(merged && selected && conditional, "merge, selector and condition noted");
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) && properties.size() == 8,
        "converted spawn table reads as retail");
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
    ++sFailures;
    return {};
  }
  return ConvertEffect(effect, data.data(), io);
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
  Check(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 4, 4, 16, 0, 0).bytes, "TXP2 becomes a random-tile PATL");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "TXP2 drops nothing");
  Check(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
            properties.size() == 2,
        "a PATL reads as retail");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 2, 5); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 4, 2, 6, 0, 0).bytes, "an int range is the last tile");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 4, 99); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a tile range past the atlas is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 0, 4, Bits(1.0f)); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "an atlas of no columns is refused");

  // ATX2's cycle ILPT(100) is the particle's whole life.
  parts = ConvertOne(OneGenerator([](auto& o) { PutAtx2(o, 2, 100); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 2, 4, 8, 1, 0).bytes, "ATX2 becomes a life PATL");
  parts = ConvertOne(OneGenerator([](auto& o) { PutAtx2(o, 2, 50); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "an ATX2 over half the life is refused");

  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, -1.0f, true); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 1).bytes,
        "TXFB with a random mirror is a flipped flipbook");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, 1.0f, false); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 0).bytes,
        "TXFB with an identity TRST is not flipped");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 0).bytes, "TXFB with no TRST");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, 0.5f, false); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a TRST scaling x is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, -2.0f, true); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a mirror to another scale is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false, 0.5f); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a flipbook over half the life is refused");
  EffectConvertIO bare = io;
  bare.flipbook = nullptr;
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false); }), bare);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "no atlas, no flipbook");

  Retail badCount;
  badCount.f("GPSM").f("TEXR").f("PATL").f("CNST").w(1).f("CNST").w(4).f("CNST").w(4).f("_END");
  Check(!SplitRetailPart(badCount.bytes.data(), badCount.bytes.size(), properties, error), "a short PATL is refused");
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
  Check(parts.size() == 1 && parts[0].part == want.bytes, "SLCT of four models is PMDL and PMDV");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "the model choice drops nothing");
  Check(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
            properties.size() == 4,
        "PMDL, PMDV and SIZE read as retail");

  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 2); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a range short of the array is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 0, 0); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "an empty array is refused");
  EffectConvertIO none;
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 3); }), none);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "models with no ids are refused");

  Retail one;
  one.f("GPSM").f("PMDL").f("CNST").w(1).f("PMDV").f("CNST").w(2).f("CNST").w(1).f("_END");
  Check(!SplitRetailPart(one.bytes.data(), one.bytes.size(), properties, error), "a PMDV short of its count is refused");
  Retail alone;
  alone.f("GPSM").f("PMDV").f("CNST").w(1).f("CNST").w(1).f("_END");
  Check(!SplitRetailPart(alone.bytes.data(), alone.bytes.size(), properties, error) ||
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
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a generator with no texture gets SIZE 0");
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "SIZE: the generator draws nothing",
        "its SIZE is dropped as drawing nothing");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "drawing nothing drops no retail property");

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
  Check(parts.size() == 1 && parts[0].part == textured.bytes, "a generator with a TEXR keeps its SIZE");
  Check(!parts.empty() && parts[0].dropped.empty(), "nothing dropped with a TEXR");
}

// PBDM is Remastered's blend mode: 2 is retail's additive AAPH, anything else
// alpha (1 and 3 noted as approximated).
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
  Check(parts.size() == 1 && parts[0].part == want.bytes, "PBDM 2 becomes AAPH");
  Check(!parts.empty() && parts[0].approximated.empty(), "PBDM 2 is exact");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "PBDM 2 drops nothing");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PBDM", 0);
                 o.push_back(1);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail alpha;
  alpha.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == alpha.bytes, "PBDM 1 writes nothing");
  Check(!parts.empty() && parts[0].approximated.size() == 1 &&
            parts[0].approximated[0] == "PBDM 1 taken as alpha blending",
        "PBDM 1 is listed as approximated");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "PBDM 1 drops nothing");
}

// GPUA (how much GPU time is free) is taken as 1.
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
  Check(parts.size() == 1 && parts[0].part == want.bytes, "GPUA becomes CNST 1");
  Check(!parts.empty() && parts[0].approximated.size() == 1 && parts[0].approximated[0] == "GPUA taken as 1",
        "GPUA is listed as approximated");
}

// SPAF(index, default) reads an effect parameter the game passes in; retail
// passes none, so it is its default.
void TestParameterDefault() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SIZE", 3);
                 PutFourCC(o, "SPAF");
                 o.push_back(5);
                 PutConstant(o, Bits(2.5f));
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("SIZE").f("CNST").w(Bits(2.5f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "SPAF becomes its default");
  Check(!parts.empty() && parts[0].approximated.size() == 1 &&
            parts[0].approximated[0] == "SPAF taken as its default",
        "SPAF is listed as approximated");
}

// PMRQ REUL(x, y, z, #00) is retail's PMRT (both Rz * Ry * Rx in degrees);
// an angle with IRND is dropped (retail would give 0 after frame 0).
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
  want.f("GPSM").f("PMRT").f("CNST").f("CNST").w(Bits(-90.0f)).f("CNST").w(Bits(0.0f));
  want.f("SCAL").f("CNST").w(Bits(-0.05f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "PMRQ REUL becomes PMRT");
  Check(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(), "PMRQ REUL is exact");

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
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == plain.bytes, "PMRQ with IRND writes nothing");
  Check(!parts.empty() && parts[0].dropped.size() == 1 && parts[0].dropped[0] == "PMRQ: an angle with IRND",
        "PMRQ with IRND is listed as dropped");
}

// A MATI with one shader and its texture parameters, as ParseMati reads it.
std::vector<uint8_t> Mati(uint32_t shader, const std::vector<std::pair<const char*, EffectGuid>>& textures) {
  std::vector<uint8_t> out(0x6d, 0);
  for (int i = 0; i < 4; ++i) {
    out[0x48 + size_t(i)] = uint8_t(shader >> (24 - 8 * i));
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
std::vector<uint8_t> VmatEffect(bool model = false) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 4);
  PutProperty(out, "PBDM", 1);
  PutConstant(out, 2);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(7));
  if (model) {
    PutProperty(out, "PMDL", 0);
    PutGuid(out, Legacy(0x1234ABCD));
  }
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

// A model particle with a VMAT carries its converted model as VMSH, when the import has the mesh.
void TestVmsh() {
  const std::vector<uint8_t> data = VmatEffect(true);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "vmsh effect parses");
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
  Check(parts.size() == 1, "one part with a model");
  if (parts.size() != 1) {
    return;
  }
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "vmsh part reads as retail");
  const RetailPartProperty* vmsh = nullptr;
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMSH")) {
      vmsh = &property;
    }
  }
  Check(vmsh != nullptr, "a model particle with a VMAT writes VMSH");
  if (vmsh != nullptr) {
    const std::vector<uint8_t>& v = vmsh->value;
    Check(v.size() == 8 + blob.size() && std::memcmp(v.data(), "CNST", 4) == 0 && v[7] == blob.size() &&
              std::equal(blob.begin(), blob.end(), v.begin() + 8),
          "VMSH is CNST, the length and the blob");
  }
  // A model the import has no mesh for (a disc model) gets no VMSH.
  io.modelMesh = [](uint32_t) { return std::vector<uint8_t>(); };
  parts = ConvertEffect(effect, data.data(), io);
  properties.clear();
  if (parts.size() == 1 && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
    for (const RetailPartProperty& property : properties) {
      Check(property.fourcc != EffectFourCC("VMSH"), "no VMSH for a model without a mesh");
    }
  }
}

// A recipe shader's MATI becomes a VMAT, and the port-only properties survive a retail split.
void TestVmat() {
  const std::vector<uint8_t> data = VmatEffect();
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "vmat effect parses");

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
  Check(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  Check(imports == 1, "the ramp texture is imported once");
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "vmat part reads as retail");
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
  Check(vmat != nullptr, "a 461071b8 material writes VMAT");
  Check(vorn, "VORN is always written");
  Check(pirn, "PIRN marks every converted PART");
  if (vmat != nullptr) {
    // CNST + u32 size, then the blob: version 2, features (Ramp = 8), blend 2, one texture.
    const std::vector<uint8_t>& v = vmat->value;
    auto be32 = [&v](size_t at) { return at + 4 <= v.size() ? uint32_t(v[at]) << 24 | v[at + 1] << 16 | v[at + 2] << 8 | v[at + 3] : ~0u; };
    Check(v.size() > 24 && std::memcmp(v.data(), "CNST", 4) == 0, "VMAT is a constant blob");
    Check(be32(8) == 2 && be32(12) == 8 && be32(16) == 2 && be32(20) == 1, "VMAT v2, Ramp, blend 2, one texture");
    Check(be32(24) == 0x5EED0008, "the texture is the imported id");
  }
  Check(parts[0].approximated.empty(), "a recipe material approximates nothing");

  // A shader with no recipe: no VMAT, and a note.
  io.materialData = [](const EffectGuid&) { return Mati(0x12345678, {}); };
  parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "one part without a recipe");
  if (parts.size() == 1) {
    bool note = false;
    for (const std::string& line : parts[0].approximated) {
      note = note || line == "VMAT: shader 12345678 has no recipe";
    }
    Check(note, "an unknown shader is noted as having no recipe");
    properties.clear();
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "fallback part reads");
    for (const RetailPartProperty& property : properties) {
      Check(property.fourcc != EffectFourCC("VMAT"), "no VMAT without a recipe");
    }
  }

  // A texture the import cannot write: no VMAT either.
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(9)}}); };
  parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "one part when the texture fails");
  if (parts.size() == 1) {
    properties.clear();
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "failed-texture part reads");
    for (const RetailPartProperty& property : properties) {
      Check(property.fourcc != EffectFourCC("VMAT"), "no VMAT when a slot texture is not written");
    }
    Check(!parts[0].approximated.empty(), "the missing texture is noted");
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

// Converts VmatEffect with `mati` as its material, returning the parts and the
// VMAT value (empty when no VMAT was written).
std::pair<std::vector<ConvertedPart>, std::vector<uint8_t>> ConvertVmat(const std::vector<uint8_t>& mati) {
  const std::vector<uint8_t> data = VmatEffect();
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: vmat effect does not parse: %s\n", error.c_str());
    ++sFailures;
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

// The new recipes: the FrameBuffer shaders draw as a multiply (blend 4), and
// the ice charge beam reads its colour from BCLR alone.
void TestVmatNewRecipes() {
  // FrameBuffer_Indirect_Unlit with no warp: features 0, blend Multiply, no textures.
  auto [parts, blob] = ConvertVmat(Mati(0x3db95827, {}));
  Check(parts.size() == 1 && !blob.empty(), "a 3db95827 material writes VMAT");
  if (!blob.empty()) {
    Check(VmatWord(blob, 8) == 2 && VmatWord(blob, 12) == 0 && VmatWord(blob, 16) == 4 && VmatWord(blob, 20) == 0,
          "3db95827 VMAT v2, no features, blend 4, no textures");
  }
  Check(parts.size() == 1 && parts[0].approximated.empty(), "3db95827 with no warp approximates nothing");

  // The same shader with a warp: the warp is dropped (drawn as a multiply).
  auto warped = ConvertVmat(MatiCch0(0x3db95827, {}, 0.5f, 0.0f, 0.0f, 0.0f));
  Check(warped.first.size() == 1 && !warped.second.empty(), "a warped 3db95827 material still writes VMAT");
  if (!warped.second.empty()) {
    Check(VmatWord(warped.second, 16) == 4, "a warped 3db95827 VMAT still blends as a multiply");
  }
  Check(warped.first.size() == 1 && warped.first[0].approximated.size() == 1 &&
            warped.first[0].approximated[0] ==
                "VMAT: shader 3db95827's scene warp dropped (drawn as a multiply)",
        "a warped 3db95827 notes its scene warp dropped");

  // FrameBuffer_Opacity_Unlit: the opacity texture, still a multiply.
  auto opacity = ConvertVmat(Mati(0xd955396d, {{"TCH0", Fresh(8)}}));
  Check(opacity.first.size() == 1 && !opacity.second.empty(), "a d955396d material writes VMAT");
  if (!opacity.second.empty()) {
    Check(VmatWord(opacity.second, 8) == 2 && VmatWord(opacity.second, 12) == 2 &&
              VmatWord(opacity.second, 16) == 4 && VmatWord(opacity.second, 20) == 1,
          "d955396d VMAT v2, OpacityTex, blend 4, one texture");
    Check(VmatWord(opacity.second, 24) == 0x5EED0008, "d955396d reads the TCH0 texture");
  }
  Check(opacity.first.size() == 1 && opacity.first[0].approximated.empty(), "d955396d approximates nothing");

  // VFX_IceChargeBeam: BCLR as the colour texture, rgb only.
  auto ice = ConvertVmat(Mati(0x9e0605cd, {{"BCLR", Fresh(8)}}));
  Check(ice.first.size() == 1 && !ice.second.empty(), "a 9e0605cd material writes VMAT");
  if (!ice.second.empty()) {
    Check(VmatWord(ice.second, 8) == 2 && VmatWord(ice.second, 12) == (1 | 4096) &&
              VmatWord(ice.second, 16) == 2 && VmatWord(ice.second, 20) == 1,
          "9e0605cd VMAT v2, ColorTex|ColorRgbOnly, blend 2, one texture");
    Check(VmatWord(ice.second, 24) == 0x5EED0008, "9e0605cd reads the BCLR texture");
  }
  Check(ice.first.size() == 1 && ice.first[0].approximated.empty(), "9e0605cd approximates nothing");
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
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a models-only generator gets SIZE 0");
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "SIZE: the generator draws models only",
        "its SIZE is dropped as models only");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "models only drops no retail property");
}

// PMRQ's camera-facing form: RADD(RAZY, SUB_(CPSS(PLOC, up, REUL(0, 0, 0)),
// REUL(x, y, z))). `angles` writes the second REUL's three angle arguments.
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
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
  PutFourCC(out, "REUL");
  angles(out);
  out.push_back(0);
}

void TestFacingRotation() {
  // One nonzero angle: PMRT is the negated angles, and PFCM faces the camera.
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
  want.f("GPSM").f("PMRT").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(-90.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  want.f("PFCM").f("CNST").w(1).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a facing PMRQ becomes a negated PMRT with PFCM");
  Check(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(),
        "a facing PMRQ is exact");

  // An IRND angle is a random spin: taken as 0, and noted.
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
  Retail still;
  still.f("GPSM").f("PMRT").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  still.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  still.f("PFCM").f("CNST").w(1).end();
  Check(parts.size() == 1 && parts[0].part == still.bytes, "a facing PMRQ with IRND spins nothing");
  Check(!parts.empty() && parts[0].approximated.size() == 1 &&
            parts[0].approximated[0] == "PMRQ: a camera-facing model's random spin left out",
        "a facing PMRQ with IRND is listed as approximated");

  // Two nonzero angles are not a single negation: the PMRQ is dropped as before.
  parts = ConvertOne(OneGenerator([](auto& o) {
              PutFacingPmrq(o, [](auto& o) {
                PutConstant(o, Bits(45.0f));
                PutConstant(o, Bits(30.0f));
                PutConstant(o, 0);
              });
              PutTexr(o, 0x1234ABCD);
            }),
            {});
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == plain.bytes, "a facing PMRQ with two angles writes nothing");
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "PMRQ: a rotation that is not REUL",
        "a facing PMRQ with two angles is dropped as before");
  if (!parts.empty()) {
    std::string error;
    std::vector<RetailPartProperty> properties;
    bool pfcm = false;
    if (SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        pfcm = pfcm || property.fourcc == EffectFourCC("PFCM");
      }
    }
    Check(!pfcm, "no PFCM when the facing rotation is refused");
  }
}

int main() {
  TestVmat();
  TestVmsh();
  TestAtlasTexture();
  TestModelChoice();
  TestRetailId();
  TestSwooshElectric();
  TestConvert();
  TestRejects();
  TestSingleNode();
  TestMappedElements();
  TestSplitRejects();
  TestShapes();
  TestGradientFrames();
  TestSpawnTable();
  TestDrawsNothing();
  TestBlendMode();
  TestGpuAvailability();
  TestParameterDefault();
  TestModelRotation();
  TestVmatNewRecipes();
  TestModelsOnly();
  TestFacingRotation();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_remastered_effect_convert: ok\n");
  return 0;
}
