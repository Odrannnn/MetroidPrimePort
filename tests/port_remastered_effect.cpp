#include "port_remastered_effect.h"
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

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Put32(out, bits);
}

// FourCCs are stored byte-reversed: 'CNST' is written "TSNC".
void PutFourCC(std::vector<uint8_t>& out, const char* text) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(text[i]));
  }
}

void PutProperty(std::vector<uint8_t>& out, const char* name, uint8_t tag) {
  PutFourCC(out, name);
  out.push_back(tag);
}

void PutGenerator(std::vector<uint8_t>& out, bool root) {
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, root ? 1 : 0);
}

EffectGuid Guid(uint8_t seed) {
  EffectGuid guid;
  for (size_t i = 0; i < guid.size(); ++i) {
    guid[i] = uint8_t(seed + i);
  }
  return guid;
}

// A GENP whose root emits 20 particles of size 2 * 3 textured with
// Guid(0x10), plus one embedded child generator listed under Guid(0x40).
std::vector<uint8_t> Effect() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "DVVN", 1);
  out.push_back(4);
  PutProperty(out, "MAXP", 1);
  PutFourCC(out, "CNST");
  Put32(out, 20);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "MULT");
  PutFourCC(out, "CNST");
  PutFloat(out, 2.0f);
  PutFourCC(out, "CNST");
  PutFloat(out, 3.0f);
  PutProperty(out, "TEXR", 1);
  PutFourCC(out, "CNST");
  const EffectGuid texture = Guid(0x10);
  out.insert(out.end(), texture.begin(), texture.end());
  PutFourCC(out, "NONE");
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  const EffectGuid child = Guid(0x40);
  out.insert(out.end(), child.begin(), child.end());
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutFourCC(out, "CNST");
  Put32(out, 5);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void TestParse() {
  const std::vector<uint8_t> data = Effect();
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  CHECK_MSG(effect.root && effect.form == EffectFourCC("GPSM"), "root is a GPSM");
  CHECK_MSG(effect.properties.size() == 4, "root has four properties");
  if (effect.properties.size() == 4) {
    const EffectProperty& dvvn = effect.properties[0];
    CHECK_MSG(dvvn.fourcc == EffectFourCC("DVVN") && dvvn.tag == 1, "DVVN per emitter");
    CHECK_MSG(dvvn.value.size() == 1 && dvvn.value[0].kind == EffectValue::Kind::Byte && dvvn.value[0].word == 4,
              "DVVN is a byte");
    const EffectProperty& maxp = effect.properties[1];
    CHECK_MSG(maxp.value.size() == 1 && maxp.value[0].fourcc == EffectFourCC("CNST") &&
                  maxp.value[0].args.size() == 1 && maxp.value[0].args[0].word == 20,
              "MAXP is CNST(20)");
    const EffectProperty& size = effect.properties[2];
    CHECK_MSG(size.tag == 3 && size.value.size() == 1 && size.value[0].fourcc == EffectFourCC("MULT") &&
                  size.value[0].args.size() == 2,
              "SIZE is MULT of two elements");
    const EffectProperty& texr = effect.properties[3];
    CHECK_MSG(texr.value.size() == 2 && texr.value[0].args.size() == 1 &&
                  texr.value[0].args[0].kind == EffectValue::Kind::Guid && texr.value[0].args[0].guid == Guid(0x10) &&
                  texr.value[1].fourcc == EffectFourCC("NONE"),
              "TEXR is CNST(guid), NONE");
  }
  CHECK_MSG(effect.children.size() == 1, "one child");
  if (effect.children.size() == 1) {
    const EffectNode& child = effect.children[0];
    CHECK_MSG(child.id == Guid(0x40) && !child.root && child.form == EffectFourCC("GPSM"), "child id and form");
    CHECK_MSG(child.properties.size() == 1 && child.properties[0].fourcc == EffectFourCC("MAXP"), "child MAXP");
  }

  const std::vector<EffectGuid> references = EffectReferences(effect);
  // Embedded children are not references; the texture is.
  CHECK_MSG(references.size() == 1 && references[0] == Guid(0x10), "texture is referenced");
  CHECK_MSG(EffectGuidString(Guid(0x00)) == "03020100-0504-0706-0809-0a0b0c0d0e0f", "ids print as little-endian UUIDs");

  const std::string dump = DumpEffect(effect, data.data());
  CHECK_MSG(dump.find("MAXP 01 CNST(20)") != std::string::npos, "dump shows MAXP");
  CHECK_MSG(dump.find("SIZE 03 MULT(CNST(2f), CNST(3f))") != std::string::npos, "dump shows SIZE");
}

void TestFailure() {
  // Root header, DVVN (FourCC, tag, byte), MAXP (FourCC, tag, CNST, word).
  constexpr size_t kSizeAt = 0x3c + 25 + 6 + 13;
  std::vector<uint8_t> data = Effect();
  // Break the MULT's second argument: no reading of SIZE parses any more.
  const size_t at = kSizeAt + 5 + 4 + 8;
  std::memcpy(data.data() + at, "????", 4);
  EffectNode effect;
  std::string error;
  size_t failOffset = 0;
  CHECK_MSG(!ParseEffect(data.data(), data.size(), effect, error, &failOffset), "broken effect fails");
  CHECK_MSG(!error.empty(), "failure says why");
  CHECK_MSG(failOffset == kSizeAt, "failure points at SIZE");

  std::vector<uint8_t> notEffect = Effect();
  std::memcpy(notEffect.data() + 0x14, "PART", 4);
  CHECK_MSG(!ParseEffect(notEffect.data(), notEffect.size(), effect, error), "non-GENP rejected");
}

// SIZE nested `depth` MULTs deep, then `properties` more MAXPs: a file built to
// run the parser out of stack is refused instead.
std::vector<uint8_t> Nested(size_t depth, size_t properties) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SIZE", 3);
  for (size_t i = 0; i < depth; ++i) {
    PutFourCC(out, "MULT");
  }
  PutFourCC(out, "CNST");
  PutFloat(out, 2.0f);
  for (size_t i = 0; i < depth; ++i) {
    PutFourCC(out, "CNST");
    PutFloat(out, 1.0f);
  }
  for (size_t i = 0; i < properties; ++i) {
    PutProperty(out, "MAXP", 1);
    PutFourCC(out, "CNST");
    Put32(out, 5);
  }
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void TestDeepNesting() {
  EffectNode effect;
  std::string error;
  std::vector<uint8_t> data = Nested(20, 50);
  CHECK_MSG(ParseEffect(data.data(), data.size(), effect, error), "nested effect parses");
  data = Nested(200000, 0);
  CHECK_MSG(!ParseEffect(data.data(), data.size(), effect, error), "too deeply nested effect is refused");
  data = Nested(1, 200000);
  CHECK_MSG(!ParseEffect(data.data(), data.size(), effect, error), "too long a property list is refused");
}
} // namespace

void PutConstant(std::vector<uint8_t>& out, float value) {
  PutFourCC(out, "CNST");
  PutFloat(out, value);
}

void PutVector(std::vector<uint8_t>& out, float x, float y, float z) {
  PutFourCC(out, "CNST");
  PutConstant(out, x);
  PutConstant(out, y);
  PutConstant(out, z);
}

void PutColor(std::vector<uint8_t>& out, float r, float g, float b, float a) {
  PutFourCC(out, "CNST");
  PutConstant(out, r);
  PutConstant(out, g);
  PutConstant(out, b);
  PutConstant(out, a);
}

// Values are read as the type their property holds, so a vector's nested
// constants are three reals (not an id, and not four constants swallowing the
// next argument), and Remastered's wrappers keep their arguments.
void TestTypedNesting() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "IMPL");
  PutFourCC(out, "MPCB");
  PutVector(out, 0.0f, 0.0f, 10000.0f);
  PutConstant(out, 0.0f);
  PutConstant(out, 0.005f);
  PutConstant(out, 1.0f);
  out.push_back(0);
  PutProperty(out, "VEL2", 3);
  PutFourCC(out, "WIND");
  PutVector(out, 0.0f, 0.0f, 0.0f);
  PutConstant(out, 0.1f);
  PutProperty(out, "GRTE", 1);
  PutFourCC(out, "MULT");
  PutFourCC(out, "DFCS");
  PutConstant(out, 6.0f);
  PutConstant(out, 4.0f);
  PutConstant(out, 0.25f);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "CNST");
  PutConstant(out, 1.0f);
  PutConstant(out, 0.5f);
  PutConstant(out, 0.25f);
  PutConstant(out, 1.0f);
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "SEMR");
  PutVector(out, 0.0f, 0.0f, 0.0f);
  PutFourCC(out, "RNDV");
  PutConstant(out, 0.1f);
  // A colour fade over two fades, ending in a real that only parses typed:
  // read untyped, the last colour took four reals and RLPT went into an id.
  PutProperty(out, "LCLR", 3);
  PutFourCC(out, "FADE");
  for (int i = 0; i < 2; ++i) {
    PutFourCC(out, "FADE");
    PutColor(out, 1.0f, 1.0f, 0.0f, 0.2f);
    PutColor(out, 1.0f, 0.0f, 0.0f, 0.0f);
    PutFourCC(out, "RLPT");
    PutConstant(out, 100.0f);
  }
  PutFourCC(out, "RLPT");
  PutConstant(out, 100.0f);
  // SMOV's external transform reads are leaves.
  PutProperty(out, "SMVR", 1);
  PutFourCC(out, "SMOV");
  PutFourCC(out, "EXTT");
  PutVector(out, 0.0f, 0.0f, 0.0f);
  PutFourCC(out, "EXTR");
  PutFourCC(out, "NONE");
  PutFourCC(out, "NONE");
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "typed effect parses");
  const std::string dump = DumpEffect(effect, out.data());
  const char* want =
      "GPSM\n"
      "  VEL1 03 IMPL(MPCB(CNST(CNST(0), CNST(0), CNST(10000f))), CNST(0), CNST(0.005f), CNST(1f), #00)\n"
      "  VEL2 03 WIND(CNST(CNST(0), CNST(0), CNST(0)), CNST(0.1f))\n"
      "  GRTE 01 MULT(DFCS(CNST(6f), CNST(4f)), CNST(0.25f))\n"
      "  COLR 03 CNST(CNST(1f), CNST(0.5f), CNST(0.25f), CNST(1f))\n"
      "  EMTR 03 SEMR(CNST(CNST(0), CNST(0), CNST(0)), RNDV(CNST(0.1f)))\n"
      "  LCLR 03 FADE(FADE(CNST(CNST(1f), CNST(1f), CNST(0), CNST(0.2f)), CNST(CNST(1f), CNST(0), CNST(0), CNST(0)), "
      "RLPT(CNST(100f))), FADE(CNST(CNST(1f), CNST(1f), CNST(0), CNST(0.2f)), CNST(CNST(1f), CNST(0), CNST(0), "
      "CNST(0)), RLPT(CNST(100f))), RLPT(CNST(100f)))\n"
      "  SMVR 01 SMOV(EXTT, CNST(CNST(0), CNST(0), CNST(0)), EXTR, NONE, NONE)\n";
  CHECK_MSG(dump == want, "nested values read as their types");
  if (dump != want) {
    std::fprintf(stderr, "%s", dump.c_str());
  }
}

// An embedded generator can carry the root flag and still end at its _END
// (no children after it), and a property tag can be 05.
void TestFlaggedChild() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "_END", 4);
  Put32(out, 2);
  for (int i = 0; i < 2; ++i) {
    const EffectGuid id = Guid(uint8_t(0x40 + i * 0x20));
    out.insert(out.end(), id.begin(), id.end());
    PutGenerator(out, i == 0);
    PutProperty(out, "LRAD", 5);
    PutConstant(out, 0.5f);
    PutProperty(out, "_END", 4);
  }
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "flagged child parses");
  CHECK_MSG(effect.children.size() == 2 && effect.children[0].root && effect.children[0].children.empty() &&
                effect.children[1].properties.size() == 1 && effect.children[1].properties[0].tag == 5,
            "flagged child ends at its _END");
}

// TEXR can hold a TXP2 (an id and three elements) or a TXFB (an id and an
// element), TIND takes the CNST(id), NONE pair, and ANTH is a property with an
// element of its own name that takes five arguments.
void TestTextureElements() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  const EffectGuid id = Guid(0x10);
  PutProperty(out, "TEXR", 1);
  PutFourCC(out, "TXP2");
  out.insert(out.end(), id.begin(), id.end());
  for (int i = 0; i < 3; ++i) {
    PutConstant(out, 4.0f);
  }
  PutProperty(out, "TIND", 1);
  PutFourCC(out, "CNST");
  out.insert(out.end(), id.begin(), id.end());
  PutFourCC(out, "NONE");
  PutProperty(out, "TEXR", 1);
  PutFourCC(out, "TXFB");
  out.insert(out.end(), id.begin(), id.end());
  PutFourCC(out, "LFTW");
  PutConstant(out, 0.0f);
  PutConstant(out, 1.0f);
  PutProperty(out, "ANTH", 3);
  PutFourCC(out, "ANTH");
  PutFourCC(out, "CNST");
  out.insert(out.end(), id.begin(), id.end());
  for (int i = 0; i < 4; ++i) {
    PutConstant(out, float(i));
  }
  PutProperty(out, "ITEN", 3);
  PutConstant(out, 1.0f);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "texture elements parse");
  CHECK_MSG(effect.properties.size() == 5 && effect.properties[3].fourcc == EffectFourCC("ANTH") &&
                effect.properties[3].value.size() == 1 && effect.properties[3].value[0].args.size() == 5,
            "ANTH takes five arguments");
}

// The GPSM flag word is not a boolean: a root with flag 2 (and a child with
// flag 4) has children after its _END. TEXR can hold an ATX2 (an id and four
// elements).
void TestGeneratorFlags() {
  const auto generator = [](std::vector<uint8_t>& out, uint32_t flag) {
    PutFourCC(out, "GPSM");
    out.resize(out.size() + 17);
    Put32(out, flag);
  };
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  generator(out, 2);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  EffectGuid id = Guid(0x40);
  out.insert(out.end(), id.begin(), id.end());
  generator(out, 4);
  const EffectGuid texture = Guid(0x10);
  PutProperty(out, "TEXR", 1);
  PutFourCC(out, "ATX2");
  out.insert(out.end(), texture.begin(), texture.end());
  for (int i = 0; i < 4; ++i) {
    PutConstant(out, float(i));
  }
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  id = Guid(0x60);
  out.insert(out.end(), id.begin(), id.end());
  generator(out, 0);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "flag 2 root parses");
  CHECK_MSG(effect.root && effect.children.size() == 1 && effect.children[0].children.size() == 1,
            "flags 2 and 4 have children");
  CHECK_MSG(effect.children.size() == 1 && effect.children[0].properties.size() == 1 &&
                effect.children[0].properties[0].value.size() == 1 &&
                effect.children[0].properties[0].value[0].args.size() == 5,
            "ATX2 takes an id and four elements");
}

// ASPV (CVEAngleSphereWithVariance, an emitter: V Q R R I R R I R R R b) and LINV (CVELineWithVariance,
// a vector: V V I R b) are read to their real arity. Both only appear in two shipped GENPs, in an EMTR.
void PutIntConstant(std::vector<uint8_t>& out, uint32_t value) {
  PutFourCC(out, "CNST");
  Put32(out, value);
}

std::vector<uint8_t> EmitterEffect() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "EMTR", 3);
  return out;
}

void FinishEffect(std::vector<uint8_t>& out) {
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
}

void TestVarianceEmitters() {
  std::vector<uint8_t> out = EmitterEffect();
  PutFourCC(out, "ASPV");
  PutVector(out, 0.0f, 0.0f, 0.0f);
  PutFourCC(out, "CNST");  // the rotation: a raw quaternion
  for (int i = 0; i < 4; ++i) {
    Put32(out, 0);
  }
  PutConstant(out, 1.0f);
  PutConstant(out, 2.0f);
  PutIntConstant(out, 1);
  PutConstant(out, 3.0f);
  PutConstant(out, 4.0f);
  PutIntConstant(out, 2);
  PutConstant(out, 5.0f);
  PutConstant(out, 6.0f);
  PutConstant(out, 7.0f);
  out.push_back(1);
  FinishEffect(out);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "ASPV parses");
  CHECK_MSG(effect.properties.size() == 1 && effect.properties[0].value.size() == 1 &&
                effect.properties[0].value[0].fourcc == EffectFourCC("ASPV") &&
                effect.properties[0].value[0].args.size() == 12,
            "ASPV has twelve arguments");

  out = EmitterEffect();
  PutFourCC(out, "SEMR");
  PutFourCC(out, "LINV");
  PutVector(out, 0.0f, 0.0f, -1.0f);
  PutVector(out, 0.0f, 0.0f, 1.0f);
  PutIntConstant(out, 4);
  PutConstant(out, 0.5f);
  out.push_back(1);
  PutVector(out, 0.0f, 0.0f, 0.0f);
  FinishEffect(out);
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "LINV parses");
  CHECK_MSG(effect.properties.size() == 1 && effect.properties[0].value.size() == 1, "one EMTR");
  if (effect.properties.size() == 1 && effect.properties[0].value.size() == 1) {
    const EffectValue& semr = effect.properties[0].value[0];
    CHECK_MSG(semr.fourcc == EffectFourCC("SEMR") && semr.args.size() == 2 &&
                  semr.args[0].fourcc == EffectFourCC("LINV") && semr.args[0].args.size() == 5,
              "SEMR holds a LINV of five arguments");
  }
}

// DFCP and DFCS read two reals (GetRealElement 0x260de4). With a third allowed, an ARRY of two
// colours CNST(r, g, b, MULT(DFCP(a, b), c)) also read as CNST(r, g, b) and MULT(DFCP(a, b, c), CNST(...)).
void TestDistanceBlendArity() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "SLCT");
  PutFourCC(out, "IRND");
  PutIntConstant(out, 0);
  PutIntConstant(out, 1);
  PutFourCC(out, "ARRY");
  Put32(out, 2);
  for (int i = 0; i < 2; ++i) {
    PutFourCC(out, "CNST");
    PutConstant(out, 1.0f);
    PutConstant(out, 0.5f);
    PutConstant(out, 0.25f);
    PutFourCC(out, "MULT");
    PutFourCC(out, "DFCP");
    PutConstant(out, 4.0f);
    PutConstant(out, 2.0f);
    PutConstant(out, 1.0f);
  }
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "ARRY of DFCP colours parses");
  const std::string dump = DumpEffect(effect, out.data());
  const char* want =
      "GPSM\n"
      "  COLR 03 SLCT(IRND(CNST(0), CNST(1)), ARRY(CNST(CNST(1f), CNST(0.5f), CNST(0.25f), MULT(DFCP(CNST(4f), "
      "CNST(2f)), CNST(1f))), CNST(CNST(1f), CNST(0.5f), CNST(0.25f), MULT(DFCP(CNST(4f), CNST(2f)), CNST(1f)))))\n";
  CHECK_MSG(dump == want, "DFCP takes two reals");
  if (dump != want) {
    std::fprintf(stderr, "%s", dump.c_str());
  }
}

// BNPS(SLCT(RAND, ARRY(BNID(name, flag)...))) as a SEMR's position: the bone name is a string
// constant, so the ARRY's entries are not read across it. ATFB: an id, an int as a real, a flag.
void PutBone(std::vector<uint8_t>& out, const char* name) {
  PutFourCC(out, "BNID");
  PutFourCC(out, "CNST");
  Put32(out, uint32_t(std::strlen(name)));
  out.insert(out.end(), name, name + std::strlen(name));
  PutFourCC(out, "CNST");
  out.push_back(1);
}

void TestBoneAndFlipbook() {
  std::vector<uint8_t> out = EmitterEffect();
  PutFourCC(out, "SEMR");
  PutFourCC(out, "BNPS");
  PutFourCC(out, "SLCT");
  PutFourCC(out, "RAND");
  PutIntConstant(out, 0);
  PutIntConstant(out, 2);
  PutFourCC(out, "ARRY");
  Put32(out, 2);
  PutBone(out, "L_ankle");
  PutBone(out, "Head_1");
  PutVector(out, 0.0f, 0.0f, 0.0f);
  FinishEffect(out);
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "BNPS over BNID parses");
  if (effect.properties.size() == 1 && effect.properties[0].value.size() == 1) {
    const EffectValue& semr = effect.properties[0].value[0];
    CHECK_MSG(semr.fourcc == EffectFourCC("SEMR") && semr.args.size() == 2 &&
                  semr.args[0].fourcc == EffectFourCC("BNPS") && semr.args[0].args.size() == 1 &&
                  semr.args[0].args[0].fourcc == EffectFourCC("SLCT"),
              "SEMR holds a BNPS over a SLCT");
  } else {
    CHECK_MSG(false, "one EMTR");
  }

  out.assign(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  const EffectGuid id = Guid(0x20);
  PutProperty(out, "TEXR", 1);
  PutFourCC(out, "ATFB");
  out.insert(out.end(), id.begin(), id.end());
  PutIntConstant(out, 120);
  PutFourCC(out, "CNST");
  out.push_back(1);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "ATFB parses");
  CHECK_MSG(effect.properties.size() == 1 && effect.properties[0].value.size() == 1 &&
                effect.properties[0].value[0].fourcc == EffectFourCC("ATFB") &&
                effect.properties[0].value[0].args.size() == 3,
            "ATFB has an id and two arguments");
}

// A TMTR (u8 0, u8 count, the TRSS) whose TRSS hold SLCT over an ARRY, as 2fab93e3's: TRSS is
// six reals, so a SLCT keeps its ARRY (untyped, the first SLCT took one argument and the list
// read TRSS, ITRL, TRSS).
void TestTrssSelect() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "TMTR", 3);
  out.push_back(0);
  out.push_back(3);
  auto trss = [&out](bool select, bool random, float offset) {
    PutFourCC(out, "TRSS");
    PutConstant(out, 0.0f);
    PutConstant(out, 0.0f);
    if (select) {
      PutFourCC(out, "SLCT");
      if (random) {
        PutFourCC(out, "IRND");
        PutIntConstant(out, 0);
        PutIntConstant(out, 1);
      } else {
        PutIntConstant(out, 0);
      }
      PutFourCC(out, "ARRY");
      Put32(out, 2);
      PutConstant(out, -offset);
      PutConstant(out, offset);
    } else {
      PutConstant(out, 1.0f);
    }
    PutConstant(out, select ? -offset : 1.0f);
    PutConstant(out, 0.0f);
    PutFourCC(out, "ITRL");
    PutIntConstant(out, 0);
    PutConstant(out, 1.0f);
  };
  trss(true, false, 1.0f);
  trss(true, true, 2.0f);
  trss(false, false, 0.0f);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "TMTR with SLCT parses");
  bool ok = effect.properties.size() == 1 && effect.properties[0].value.size() == 3;
  for (size_t i = 0; ok && i < 3; ++i) {
    const EffectValue& trss = effect.properties[0].value[i];
    ok = trss.fourcc == EffectFourCC("TRSS") && trss.args.size() == 6 &&
         trss.args[5].fourcc == EffectFourCC("ITRL") &&
         (i == 2 || (trss.args[2].fourcc == EffectFourCC("SLCT") && trss.args[2].args.size() == 2 &&
                     trss.args[2].args[1].fourcc == EffectFourCC("ARRY")));
  }
  CHECK_MSG(ok, "TMTR is three TRSS, each SLCT over its ARRY");
  if (!ok) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
  }
}

int main() {
  TestParse();
  TestFailure();
  TestTypedNesting();
  TestDistanceBlendArity();
  TestFlaggedChild();
  TestTextureElements();
  TestGeneratorFlags();
  TestDeepNesting();
  TestVarianceEmitters();
  TestBoneAndFlipbook();
  TestTrssSelect();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_effect: ok\n");
  }
  return PORT_TEST_RESULT();
}
