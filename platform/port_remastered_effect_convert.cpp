#include "port_remastered_effect_convert.h"
#include "port_bytes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace PortRemastered {
namespace {

constexpr uint32_t F(const char (&text)[5]) { return EffectFourCC(text); }

// The types CParticleDataFactory reads a value as.
enum class Type : uint8_t { Int, Real, Vector, ModVector, Color, Emitter, Texture, Bool, Asset };

// Retail's properties (CParticleDataFactory::CreateGPSM) and the type each is
// read as. An Asset property also names the type of asset it refers to.
struct PropertyType {
  Type type;
  uint32_t asset = 0;
};

using PropertyTable = std::unordered_map<uint32_t, PropertyType>;

void AddProperties(PropertyTable& t, const char* names, Type type, uint32_t asset = 0) {
  for (const char* c = names; *c != 0;) {
    const uint32_t fourcc = uint32_t(uint8_t(c[0])) << 24 | uint32_t(uint8_t(c[1])) << 16 |
                            uint32_t(uint8_t(c[2])) << 8 | uint32_t(uint8_t(c[3]));
    t[fourcc] = {type, asset};
    c += 4;
    while (*c == ' ') {
      ++c;
    }
  }
}

// Retail's swoosh properties (CParticleSwooshDataFactory::CreateWPSM).
const PropertyTable& SwooshProperties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    AddProperties(t, "PSLT LENG SIDE SPLN TSPN", Type::Int);
    AddProperties(t, "TIME LRAD RRAD IROT ROTM", Type::Real);
    AddProperties(t, "POFS IVEL NPOS", Type::Vector);
    AddProperties(t, "VELM VLM2", Type::ModVector);
    AddProperties(t, "COLR", Type::Color);
    AddProperties(t, "TEXR", Type::Texture);
    AddProperties(t, "LLRD CROS SROT VLS1 VLS2 WIRE AALP ZBUF ORNT TEXW CRND", Type::Bool);
    return t;
  }();
  return table;
}

// Retail's electric properties (CParticleElectricDataFactory::CreateELSM).
// GPSM and EPSM name child PARTs, read as ICTS is.
const PropertyTable& ElectricProperties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    AddProperties(t, "LIFE SLIF SCNT SSEG", Type::Int);
    AddProperties(t, "GRAT AMPL AMPD LWD1 LWD2 LWD3", Type::Real);
    AddProperties(t, "COLR LCL1 LCL2 LCL3", Type::Color);
    AddProperties(t, "IEMT FEMT", Type::Emitter);
    AddProperties(t, "ZERY", Type::Bool);
    AddProperties(t, "GPSM EPSM", Type::Asset, F("PART"));
    AddProperties(t, "SSWH", Type::Asset, F("SWHC"));
    return t;
  }();
  return table;
}

const PropertyTable& RetailProperties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    auto add = [&t](const char* names, Type type, uint32_t asset = 0) { AddProperties(t, names, type, asset); };
    add("PSLT PSWT MBSP MAXP LTME SEED NCSY CSSD NDSY PISY SISY SSSD SESD LTYP LFOT", Type::Int);
    add("PSTS GRTE SIZE ROTA LENG WIDT LINT LFOR LSLA ADV1 ADV2 ADV3 ADV4 ADV5 ADV6 ADV7 ADV8", Type::Real);
    add("PSIV PSOV ILOC IVEC POFS PMOP PMRT PMSC SSPO SEPO LOFF LDIR", Type::Vector);
    add("PSVM VEL1 VEL2 VEL3 VEL4", Type::ModVector);
    add("COLR PMCL LCLR", Type::Color);
    add("EMTR", Type::Emitter);
    add("TEXR TIND", Type::Texture);
    add("LIT_ ORNT RSOP AAPH ZBUF SORT MBLR VMD1 VMD2 VMD3 VMD4 CIND PMAB PMUS PMOO LINE FXLL OPTS", Type::Bool);
    add("PMDL", Type::Asset, F("CMDL"));
    add("ICTS IDTS IITS", Type::Asset, F("PART"));
    add("SSWH", Type::Asset, F("SWHC"));
    add("SELC", Type::Asset, F("ELSC"));
    return t;
  }();
  return table;
}

// The properties of a retail asset type, and the FourCC its file starts with.
const PropertyTable& PropertiesOf(uint32_t type) {
  if (type == F("SWHC")) {
    return SwooshProperties();
  }
  if (type == F("ELSC")) {
    return ElectricProperties();
  }
  return RetailProperties();
}

uint32_t HeaderOf(uint32_t type) {
  if (type == F("SWHC")) {
    return F("SWSH");
  }
  if (type == F("ELSC")) {
    return F("ELSM");
  }
  return F("GPSM");
}

// Each retail element per type, and its arguments: i int, r real, v vector,
// m mod vector, c colour, B bool (CNST and a byte), k a keyframe block, # a
// literal of the type (CNST's int or float). Empty is no arguments.
using Signatures = std::unordered_map<uint32_t, const char*>;

const Signatures& ElementsOf(Type type) {
  static const Signatures intElements = {
      {F("CNST"), "#"},  {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("TSCL"), "r"},  {F("DETH"), "ii"},
      {F("CHAN"), "iii"}, {F("ADD_"), "ii"}, {F("MULT"), "ii"},  {F("MODU"), "ii"}, {F("RAND"), "ii"},
      {F("IMPL"), "i"},  {F("ILPT"), "i"},  {F("SPAH"), "iii"}, {F("IRND"), "ii"}, {F("CLMP"), "iii"},
      {F("PULS"), "iiii"}, {F("NONE"), ""}, {F("RTOI"), "rr"},  {F("SUB_"), "ii"}, {F("GTCP"), ""},
      {F("GAPC"), ""},   {F("GEMT"), ""},
  };
  static const Signatures realElements = {
      {F("CNST"), "#"},   {F("NONE"), ""},    {F("KEYE"), "k"},    {F("KEYP"), "k"},   {F("SCAL"), "r"},
      {F("SINE"), "rrr"}, {F("ADD_"), "rr"},  {F("MULT"), "rr"},   {F("DOTP"), "vv"},  {F("RAND"), "rr"},
      {F("IRND"), "rr"},  {F("CHAN"), "rri"}, {F("CLMP"), "rrr"},  {F("PULS"), "iirr"}, {F("RLPT"), "r"},
      {F("LFTW"), "rr"},  {F("PRLW"), ""},    {F("PSLL"), ""},     {F("PAP1"), ""},    {F("PAP2"), ""},
      {F("PAP3"), ""},    {F("PAP4"), ""},    {F("PAP5"), ""},     {F("PAP6"), ""},    {F("PAP7"), ""},
      {F("PAP8"), ""},    {F("VXTR"), "v"},   {F("VYTR"), "v"},    {F("VZTR"), "v"},   {F("VMAG"), "v"},
      {F("ISWT"), "rr"},  {F("CLTN"), "rrrr"}, {F("CEQL"), "rrrr"}, {F("CRNG"), "rrrrr"}, {F("CEXT"), "i"},
      {F("ITRL"), "ir"},  {F("PSSZ"), ""},    {F("SUB_"), "rr"},  {F("GTCR"), "c"},    {F("GTCG"), "c"},   {F("GTCB"), "c"},
      {F("GTCA"), "c"},
      {F("DFCS"), "rr"},  {F("DFCP"), "rr"},
  };
  static const Signatures vectorElements = {
      {F("NONE"), ""},      {F("CNST"), "rrr"}, {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("ANGC"), "rrrrr"},
      {F("CONE"), "vr"},    {F("CIRC"), "vvrrr"}, {F("CCLU"), "vvir"}, {F("ADD_"), "vv"}, {F("MULT"), "vv"},
      {F("CHAN"), "vvi"},   {F("PULS"), "iivv"}, {F("RTOV"), "r"},  {F("PLOC"), ""},    {F("PLCO"), ""},
      {F("PVEL"), ""},      {F("PSOF"), ""},    {F("PSOU"), ""},   {F("PSOR"), ""},    {F("PSTR"), ""},
      {F("SUB_"), "vv"},    {F("CTVC"), "c"},
  };
  static const Signatures modVectorElements = {
      {F("NONE"), ""},     {F("CNST"), "rrr"},  {F("GRAV"), "v"},    {F("WIND"), "vr"},  {F("EXPL"), "rr"},
      {F("CHAN"), "mmi"},  {F("PULS"), "iimm"}, {F("IMPL"), "vrrrB"}, {F("LMPL"), "vrrrB"}, {F("EMPL"), "vrrrB"},
      {F("SWRL"), "vvrr"}, {F("BNCE"), "vvrrB"}, {F("SPOS"), "v"},
  };
  static const Signatures colorElements = {
      {F("CNST"), "rrrr"}, {F("KEYE"), "k"},   {F("KEYP"), "k"},    {F("FADE"), "ccr"}, {F("CFDE"), "ccrr"},
      {F("CHAN"), "cci"},  {F("PULS"), "iicc"}, {F("PCOL"), ""},    {F("NONE"), ""},
      // Port-only (CParticleDataFactory under TARGET_PC).
      {F("MULT"), "cc"},   {F("MDAO"), "cr"},
  };
  // SETR is left out: Remastered writes SEMR, which retail reads too.
  static const Signatures emitterElements = {
      {F("NONE"), ""}, {F("SEMR"), "vv"}, {F("SPHE"), "vrr"}, {F("ASPH"), "vrrrrrr"},
  };
  static const Signatures none;
  switch (type) {
  case Type::Int:
    return intElements;
  case Type::Real:
    return realElements;
  case Type::Vector:
    return vectorElements;
  case Type::ModVector:
    return modVectorElements;
  case Type::Color:
    return colorElements;
  case Type::Emitter:
    return emitterElements;
  default:
    return none;
  }
}

Type TypeOfLetter(char letter) {
  switch (letter) {
  case 'i':
    return Type::Int;
  case 'r':
    return Type::Real;
  case 'v':
    return Type::Vector;
  case 'm':
    return Type::ModVector;
  case 'c':
    return Type::Color;
  default:
    return Type::Bool;
  }
}

using port::AppendBE32;
using port::ReadLE16;
using port::ReadLE32;

float FloatFromBits(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half >> 15) << 31;
  const int exponent = (half >> 10) & 0x1f;
  const uint32_t mantissa = half & 0x3ff;
  uint32_t bits;
  if (exponent == 0) {
    // Zero or subnormal: value = mantissa * 2^-24.
    float value = float(mantissa) * (1.0f / 16777216.0f);
    std::memcpy(&bits, &value, 4);
    bits |= sign;
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | mantissa << 13;
  } else {
    bits = sign | uint32_t(exponent - 15 + 127) << 23 | mantissa << 13;
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

bool IsElement(const EffectValue& value, uint32_t fourcc) {
  return value.kind == EffectValue::Kind::Element && value.fourcc == fourcc;
}

bool IsParameterRead(uint32_t fourcc) {
  for (uint32_t read : {F("SPAF"), F("SPAC"), F("SPAV"), F("TPVF"), F("TPVC"), F("TPVV"), F("DPVF"), F("DPVC"), F("DPVV")}) {
    if (fourcc == read) {
      return true;
    }
  }
  return false;
}

// CNST holding one word (an int, or a real's bits).
bool ConstWord(const EffectValue& value, uint32_t& word) {
  if (!IsElement(value, F("CNST")) || value.args.size() != 1 || value.args[0].kind != EffectValue::Kind::Word) {
    return false;
  }
  word = value.args[0].word;
  return true;
}

bool ConstIs(const EffectValue& value, uint32_t word) {
  uint32_t have;
  return ConstWord(value, have) && have == word;
}

// RAND or IRND of CNST 0 and CNST `high` (a real's bits or an int).
bool RangeFromZero(const EffectValue& value, uint32_t fourcc, uint32_t& high) {
  return IsElement(value, fourcc) && value.args.size() == 2 && ConstIs(value.args[0], 0) &&
         ConstWord(value.args[1], high);
}

// TRST's x scale of a particle that is mirrored half the time: KPIN(CREL(LTHN(RAND(0, 1)), 0.5, 1, -1)).
bool RandomMirror(const EffectValue& value) {
  uint32_t high;
  if (!IsElement(value, F("KPIN")) || value.args.size() != 1 || !IsElement(value.args[0], F("CREL"))) {
    return false;
  }
  const EffectValue& rel = value.args[0];
  return rel.args.size() == 4 && IsElement(rel.args[0], F("LTHN")) && rel.args[0].args.size() == 1 &&
         RangeFromZero(rel.args[0].args[0], F("RAND"), high) && high == FloatBits(1.0f) &&
         ConstIs(rel.args[1], FloatBits(0.5f)) && ConstIs(rel.args[2], FloatBits(1.0f)) &&
         ConstIs(rel.args[3], FloatBits(-1.0f));
}

// TXFB's transform: TRST(0, 0, x scale, 1, 0, raw) with the scale 1 or a random mirror.
bool FlipbookTransform(const EffectValue& value, bool& flip) {
  if (!IsElement(value, F("TRST")) || value.args.size() != 6 || !ConstIs(value.args[0], 0) ||
      !ConstIs(value.args[1], 0) || !ConstIs(value.args[3], FloatBits(1.0f)) || !ConstIs(value.args[4], 0)) {
    return false;
  }
  flip = RandomMirror(value.args[2]);
  return flip || ConstIs(value.args[2], FloatBits(1.0f));
}

// ---- The port-only material (VMAT), from a MATI and its shader ----

// aurora::gfx::vfx::Feature.
constexpr uint32_t kColorTex = 1, kOpacityTex = 2, kErosion = 4, kRamp = 8, kIndirect = 16, kDepthSoften = 32,
                   kThresholding = 64, kDualMod = 128, kOpacityFresnel = 256, kColorIndexing = 512, kAddColor = 1024,
                   kOpacityFade = 2048, kColorRgbOnly = 4096;
// aurora::gfx::vfx::Blend::Multiply, the port's stand-in for the FrameBuffer_* shaders.
constexpr uint32_t kBlendMultiply = 4;

enum class Role { Color, Opacity, Ramp, Ramp2, Threshold, Indirect, Palette };

// A MATI texture parameter (`tag`) in the role it plays, and which CCH0
// components scale the indirect warp of its u and v (-1: not warped).
struct TexSpec {
  const char* tag;
  Role role;
  int warpU;
  int warpV;
};

// Order of the VMAT blob's Src records.
enum SrcIndex {
  kSrcErosion,
  kSrcThrX,
  kSrcThrY,
  kSrcThrW,
  kSrcFresnelX,
  kSrcFresnelY,
  kSrcFadeX,
  kSrcFadeY,
  kSrcIndexScale,
  kSrcIndexOffset,
  kSrcIndexRow,
  kSrcCount
};

// Where a uniform comes from: row >= 0 is component `comp` of PMTR row `row`;
// row < 0 with comp >= 0 is the MATI's CCH0[comp], a constant unless an SMTR
// overrides it.
struct SrcSpec {
  SrcIndex index;
  int row;
  int comp;
};

struct Recipe {
  uint32_t shader;
  uint32_t features;
  std::vector<TexSpec> textures;
  std::vector<SrcSpec> sources;
  int addRow = -1;         // PMTR row of the AddColor (rgb, scale); -1: none
  bool unitCch0 = false;   // the shader also reads CCH0.z/.w (fade gain/bias): only the identity (1, 0) is expressible
  bool frameBuffer = false; // a FrameBuffer_* shader (scene x vc.rgb, warped by CCH0.xy): drawn as a multiply
};

// RECIPES.md "(b) Per shader" and RECIPES-all.md. Derived from each shader's
// own code; a Lit shader uses its perm-0 (000_0) maths, which is the unlit
// maths.
const Recipe* RecipeOf(uint32_t shader) {
  static const std::vector<Recipe> table = {
      // FrameBuffer_Indirect(_Opacity)_Unlit: the scene behind at the fragment, offset by TCH0's
      // (resp. TCH1's) warp x CCH0.xy, times vc.rgb; alpha vc.a (resp. x TCH0.x). The warp is dropped.
      {0x3db95827, 0, {}, {}, -1, false, true},
      {0xd955396d, kOpacityTex,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}, -1, false, true},
      // VFX_IceChargeBeam (Lit): the PMTR colour pair blended by BCLR.x x NMAP.x, plus BCLR.rgb lit
      // through NMAP, x vc.rgb; alpha is vc.a alone. Approximated as BCLR.rgb x vc.rgb.
      {0x9e0605cd, kColorTex | kColorRgbOnly,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x84e0fe6c, kOpacityTex,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x642dd72b, kColorTex,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x73118d89, kColorTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x5bbdb68a, kColorTex | kOpacityTex,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x461071b8, kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {}},
      {0x75cdfb9a, kColorTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x3c4deeae, kOpacityTex | kErosion,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xe1a93093, kColorTex | kIndirect,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x12c96e89, kColorTex | kOpacityTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x8e6bfa33, kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x6c165efd, kOpacityTex | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x034cb0d3, kOpacityTex | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x2eaed6b8, kColorTex | kOpacityTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x17713aeb, kColorTex | kIndirect | kDepthSoften,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x493fe0ff, kColorTex | kOpacityTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x3fc4b661, kErosion | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0xe27969fe, kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}},
       {}},
      {0xccff18d9, kRamp | kIndirect | kThresholding | kOpacityFresnel,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcFresnelX, 2, 2}, {kSrcFresnelY, 2, 3}, {kSrcThrW, -1, 0}}},
      {0x7a29ff64, kOpacityTex | kAddColor,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}, 0},  // [I] perm 001_0 (no 000_0)
      {0x08757dae, kColorTex | kErosion,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x42d8bf93, kOpacityTex | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x20e149b2, kColorTex | kOpacityTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0xfef02d58, kColorTex,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0x58d7fb7b, kColorTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xfa49ff3c, kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      {0x8177c4e0, kOpacityTex | kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0xb658b55f, kErosion | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x158f4374, kOpacityTex | kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}, {"TCH3", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0xccdf1984, kColorTex | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {}},
      {0xeba92100, kErosion | kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x63c470ce, kColorTex | kIndirect,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x4dfde16d, kOpacityTex | kErosion,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x55584bd3, kColorTex | kOpacityTex,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x8e97872f, kRamp | kIndirect | kDepthSoften | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x28bf8533, kOpacityTex | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}, {"TCH1", Role::Opacity, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
      {0x9adb322c, kOpacityTex | kErosion | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xb8eed42e, kOpacityTex | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x38fbea2d, kOpacityTex | kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x7116d51a, kColorTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x7d52bd6b, kRamp | kIndirect | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}, -1, true},
      {0xb1e9a0e4, kRamp | kIndirect | kDepthSoften | kThresholding | kOpacityFresnel,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcFresnelX, 2, 2}, {kSrcFresnelY, 2, 3}, {kSrcThrW, -1, 0}}},
      {0x72cc98d8, kColorTex | kOpacityTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0x2d3ee3e3, kColorTex | kThresholding,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Threshold, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0xa327502c, kOpacityTex | kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x0cc45d52, kColorTex | kIndirect | kDepthSoften,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0xa617da1d, kColorTex | kOpacityTex | kErosion | kDepthSoften,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xbdf7eb7f, kRamp | kIndirect | kThresholding | kOpacityFresnel,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcFresnelX, 2, 2}, {kSrcFresnelY, 2, 3}, {kSrcThrW, -1, 0}}},
      {0x54844806, kOpacityTex | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}},
      {0x1a9ef0d8, kOpacityTex | kAddColor,
       {{"TCH0", Role::Opacity, -1, -1}},
       {}, 0},
      {0x2da497da, kErosion | kDepthSoften | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}, {kSrcErosion, 0, 3}}},
      {0xe4d4c263, kColorTex | kOpacityTex | kErosion | kDepthSoften | kOpacityFade,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcFadeX, 0, 0}, {kSrcFadeY, 0, 1}, {kSrcErosion, 0, 2}}},
      {0x605484a4, kOpacityTex | kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0xb9ccfc57, kColorTex | kIndirect | kDepthSoften,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0xe899b1df, kRamp | kIndirect | kDepthSoften | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}, -1, true},
      {0x3a867837, kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      {0xf1c52f0d, kColorTex | kErosion | kOpacityFade,
       {{"BCLR", Role::Color, -1, -1}},
       {{kSrcFadeX, 0, 0}, {kSrcFadeY, 0, 1}, {kSrcErosion, 0, 2}}},
      {0xde9a121b, kOpacityTex | kErosion | kDepthSoften,
       {{"TCH0", Role::Opacity, -1, -1}},
       {{kSrcErosion, 0, 0}}},
      {0xdbe138ec, kOpacityTex | kErosion | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}, {"TCH1", Role::Opacity, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}, {kSrcErosion, 0, 3}}},
      {0x49a43a10, kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {}},
      {0x9abe8368, kColorTex | kIndirect | kThresholding,
       {{"BCLR", Role::Color, 1, 2}, {"TCH0", Role::Threshold, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0xfbfffd6f, kRamp | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0xd471baa6, kOpacityTex | kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1}, {"TCH3", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x0fb73e1b, kColorTex | kIndirect,
       {{"BCLR", Role::Color, 0, 1}, {"TCH0", Role::Indirect, -1, -1}},
       {}},
      {0x96e205db, kRamp | kIndirect | kDualMod | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}},
      {0xb995317d, kRamp | kIndirect | kDualMod | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}},
      {0x93df8f48, kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Threshold, -1, -1}, {"TCH2", Role::Indirect, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x641fc76e, kColorTex | kDepthSoften | kThresholding,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Threshold, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0x5f2a0715, kErosion | kRamp | kIndirect,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x0cbeb3de, kOpacityTex | kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x5283e927, kErosion | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x10dbd66e, kOpacityTex | kRamp | kDepthSoften | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Opacity, -1, -1}, {"TCH2", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x71dc2465, kRamp | kIndirect | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {}},
      {0xec6b1bb2, kOpacityTex | kRamp | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Opacity, -1, -1}, {"TCH2", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x83549ed0, kOpacityTex | kDepthSoften | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}, {"TCH1", Role::Opacity, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
      {0xcc9405ea, kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}},
       {}},
      {0x9ea86831, kRamp | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}},
       {}},
      {0x33ba3497, kDepthSoften | kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
      {0xeeb36096, kOpacityTex | kRamp | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x1ad75997, kRamp | kIndirect | kOpacityFade,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}, -1, true},
      {0x574c1724, kColorTex | kIndirect | kDepthSoften | kThresholding,
       {{"BCLR", Role::Color, 1, 2}, {"TCH0", Role::Threshold, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0x4e386476, kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      {0x68bf7bc6, kRamp | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}},
       {}},
      {0x3fd46bb7, kOpacityTex | kRamp | kDualMod | kOpacityFade,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Ramp2, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {{kSrcFadeX, 2, 0}, {kSrcFadeY, 2, 1}}},
      {0x207abf3f, kRamp | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x33b751f2, kRamp | kIndirect | kDualMod,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {}},
      {0x8d9c7aa7, kOpacityTex | kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0x440fbd96, kColorTex | kOpacityTex | kErosion | kOpacityFade,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}},
       {{kSrcFadeX, 0, 0}, {kSrcFadeY, 0, 1}, {kSrcErosion, 0, 2}}},
      {0xc5cd782b, kOpacityTex | kRamp,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      {0xfb91aaad, kErosion | kRamp | kDepthSoften,
       {{"TCH0", Role::Ramp, -1, -1}},
       {{kSrcErosion, 2, 0}}},
      {0x555e40fa, kRamp | kDepthSoften | kThresholding,
       {{"TCH0", Role::Ramp, -1, -1}, {"TCH1", Role::Threshold, -1, -1}},
       {{kSrcThrX, 2, 0}, {kSrcThrY, 2, 1}, {kSrcThrW, -1, 0}}},
      {0x80a47126, kColorTex | kIndirect | kThresholding,
       {{"BCLR", Role::Color, 1, 2}, {"TCH0", Role::Threshold, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, -1, 0}}},
      {0xb32b6148, kRamp | kIndirect | kDepthSoften | kDualMod,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Ramp2, 2, 3}, {"TCH2", Role::Indirect, -1, -1}},
       {}},
  };
  for (const Recipe& recipe : table) {
    if (recipe.shader == shader) {
      return &recipe;
    }
  }
  return nullptr;
}

struct MatiTexture {
  EffectGuid guid{};
  uint32_t texCoord = 0;
  int32_t filter = -1;
  int32_t wrapX = -1;
  int32_t wrapY = -1;
};

struct Mati {
  uint32_t shader = 0;  // the four bytes at 0x48, big-endian
  std::map<std::string, MatiTexture> textures;
  std::map<std::string, std::array<float, 4>> vectors;
};

using port::ReadLEFloat;

// The header's shader and the parameter entries (u8 type + four tag bytes +
// payload): 6 texture (guid, texCoord, filter, wrap x/y/z; 36 bytes), 3 vec4,
// 1 float, 0 int. An unknown type ends the list.
bool ParseMati(const std::vector<uint8_t>& d, Mati& out) {
  if (d.size() < 0x6d) {
    return false;
  }
  out.shader = uint32_t(d[0x48]) << 24 | uint32_t(d[0x49]) << 16 | uint32_t(d[0x4a]) << 8 | d[0x4b];
  const uint32_t count = ReadLE32(d.data() + 0x69);
  size_t at = 0x6d;
  for (uint32_t i = 0; i < count && at + 5 <= d.size(); ++i) {
    const uint8_t type = d[at];
    const std::string tag(reinterpret_cast<const char*>(d.data() + at + 1), 4);
    at += 5;
    const size_t size = type == 6 ? 36 : type == 3 ? 16 : type == 1 || type == 0 ? 4 : 0;
    if (size == 0 || at + size > d.size()) {
      break;
    }
    if (type == 6) {
      MatiTexture texture;
      std::memcpy(texture.guid.data(), d.data() + at, 16);
      texture.texCoord = ReadLE32(d.data() + at + 16);
      texture.filter = int32_t(ReadLE32(d.data() + at + 20));
      texture.wrapX = int32_t(ReadLE32(d.data() + at + 24));
      texture.wrapY = int32_t(ReadLE32(d.data() + at + 28));
      out.textures[tag] = texture;
    } else if (type == 3) {
      std::array<float, 4> value;
      for (int c = 0; c < 4; ++c) {
        value[size_t(c)] = ReadLEFloat(d.data() + at + 4 * size_t(c));
      }
      out.vectors[tag] = value;
    }
    at += size;
  }
  return true;
}

// Remastered's wrap enum (-1 default, 0 clamp, 1 repeat, 2 mirror) as GXTexWrapMode.
// [I] the default taken as repeat.
uint32_t WrapOf(int32_t wrap) { return wrap == 0 ? 0 : wrap == 2 ? 2 : 1; }

// PVAR variable kinds, in the table's own order (reals, ints, colours, vectors, rotations).
// The numbers are what PVRT stores (EPortVarType).
struct PvarEntry {
  EffectGuid guid;
  uint32_t type;
  float def[4];
};

uint32_t VarTypeOf(uint32_t fourcc) {
  return fourcc == F("VARF") ? 0 : fourcc == F("VARI") ? 1 : fourcc == F("VARC") ? 2 : 3;
}

class Writer {
public:
  Writer(const uint8_t* data, const EffectConvertIO& io) : m_data(data), m_io(io) {}

  uint32_t AssetId(const EffectGuid& id, uint32_t type) const {
    if (m_io.assetId) {
      return m_io.assetId(id, type);
    }
    return EffectRetailId(id).value_or(0);
  }

  // One element of `type` into `out`, or false with `why` set.
  bool Element(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Element) {
      why = "a bare value where retail reads an element";
      return false;
    }
    // MPCB wraps Remastered's vectors; its cartesian form is the vector itself,
    // and MPCB(MPAC(x bias, y bias, x range, y range), magnitude) is retail's
    // ANGC with the same five arguments (checked against the disc's pairs).
    if (value.fourcc == F("MPCB") && type == Type::Vector) {
      if (value.args.size() == 1) {
        return Element(value.args[0], type, out, why);
      }
      if (value.args.size() == 2 && IsElement(value.args[0], F("MPAC")) && value.args[0].args.size() == 4) {
        AppendBE32(out, F("ANGC"));
        for (const EffectValue& arg : value.args[0].args) {
          if (!Element(arg, Type::Real, out, why)) {
            return false;
          }
        }
        return Element(value.args[1], Type::Real, out, why);
      }
      why = "MPCB in a form retail has no element for";
      return false;
    }
    if (value.fourcc == F("MPCB") && type == Type::ModVector) {
      if (value.args.size() != 1) {
        why = "MPCB in its angle form where retail reads a mod vector";
        return false;
      }
      return Element(value.args[0], type, out, why);
    }
    // ANCR(REUL(x, y, z, #00), x range, y range, magnitude) is retail's ANGC.
    // Unrotated, the biases are 0 (the disc writes -0). A cone turned about X
    // only is taken as ANGC's X bias (-x: REUL(-90) points the cone's +Z axis
    // at +Y, as ANGC's X bias of 90 does); that is exact along the cone's
    // centre line and approximate across it.
    if (value.fourcc == F("ANCR") && type == Type::Vector && value.args.size() == 4) {
      uint32_t xBias;
      if (!ConeBias(value.args[0], xBias, why)) {
        return false;
      }
      AppendBE32(out, F("ANGC"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, xBias);
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 0x80000000u);
      for (size_t i = 1; i < 4; ++i) {
        if (!Element(value.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    // ASPR(origin, REUL, x range, y range, radius, speed) is retail's ASPH
    // (origin, x bias, y bias, x range, y range, radius, speed), with the
    // rotation taken as ANCR's is.
    if (value.fourcc == F("ASPR") && type == Type::Emitter && value.args.size() == 6) {
      uint32_t xBias;
      if (!ConeBias(value.args[1], xBias, why)) {
        return false;
      }
      AppendBE32(out, F("ASPH"));
      if (!Element(value.args[0], Type::Vector, out, why)) {
        return false;
      }
      AppendBE32(out, F("CNST"));
      AppendBE32(out, xBias);
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 0x80000000u);
      for (size_t i = 2; i < 6; ++i) {
        if (!Element(value.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    // RNDV(magnitude): a random direction of that length, as a whole-sphere
    // ANGC. Its spread is not uniform over the sphere as Remastered's may be.
    if (value.fourcc == F("RNDV") && type == Type::Vector && value.args.size() == 1) {
      m_approximated.push_back("RNDV taken as a whole-sphere ANGC");
      AppendBE32(out, F("ANGC"));
      for (float angle : {0.0f, 0.0f, 360.0f, 360.0f}) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, FloatBits(angle));
      }
      return Element(value.args[0], Type::Real, out, why);
    }
    if (value.fourcc == F("GRAD") && type == Type::Color) {
      return Gradient(value, out, why);
    }
    // KEWS: an LE u32 key count, then that many LE floats over a swoosh's
    // points (newest 0, oldest 1). Retail reads a swoosh's COLR at a point's
    // age over the life, so this is percent keyframes (the swoosh's PIRN path
    // makes the life the point count).
    if (value.fourcc == F("KEWS") && type == Type::Real && value.args.size() == 1 &&
        value.args[0].kind == EffectValue::Kind::Raw) {
      const uint8_t* p = m_data + value.args[0].offset;
      const size_t size = value.args[0].size;
      const uint32_t count = size >= 4 ? ReadLE32(p) : 0;
      if (count < 2 || 4 + size_t(count) * 4 > size) {
        why = "KEWS without its keys";
        return false;
      }
      AppendBE32(out, F("KEYP"));
      AppendBE32(out, 1);
      AppendBE32(out, 0);
      out.push_back(0);
      out.push_back(0);
      AppendBE32(out, 101);
      AppendBE32(out, 0);
      AppendBE32(out, 101);
      for (int percent = 0; percent <= 100; ++percent) {
        const float at = float(percent) / 100.0f * float(count - 1);
        const uint32_t i = std::min(uint32_t(at), count - 2);
        float a, b;
        std::memcpy(&a, p + 4 + 4 * i, 4);
        std::memcpy(&b, p + 8 + 4 * i, 4);
        AppendBE32(out, FloatBits(a + (b - a) * (at - float(i))));
      }
      return true;
    }
    // MPRD(a, b): a random int between two, as RAND.
    if (value.fourcc == F("MPRD") && value.args.size() == 2 && (type == Type::Int || type == Type::Real)) {
      AppendBE32(out, F("RAND"));
      return Element(value.args[0], type, out, why) && Element(value.args[1], type, out, why);
    }
    // DFCS/DFCP(a, b): a ramp of the depth from the camera, a port-only real element.
    if ((value.fourcc == F("DFCP") || value.fourcc == F("DFCS")) && value.args.size() == 2 &&
        type == Type::Real) {
      AppendBE32(out, value.fourcc);
      return Element(value.args[0], Type::Real, out, why) &&
             Element(value.args[1], Type::Real, out, why);
    }
    // GPUA is CREGPUAvailabilty, how much GPU time is free (to thin effects
    // out under load): taken as 1, all of it.
    if (value.fourcc == F("GPUA") && value.args.empty() && type == Type::Real) {
      m_approximated.push_back("GPUA taken as 1");
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(1.0f));
      return true;
    }
    // SPAF/SPAC/SPAV(#n, default) read an effect parameter the game passes in,
    // TPVF/TPVC/TPVV and DPVF/DPVC/DPVV(guid, default) a named one; retail
    // passes none, so they are taken as their default.
    if (value.args.size() == 2 && IsParameterRead(value.fourcc)) {
      m_approximated.push_back(EffectFourCCString(value.fourcc) + " taken as its default");
      return Element(value.args[1], type, out, why);
    }
    // VARF/VARI/VARV/VARC(guid): the generator's own variable. Port-only elements, written as
    // the fourcc and the variable's index in the PVRT table (PVAR guid order).
    if (value.args.size() == 1 && value.args[0].kind == EffectValue::Kind::Guid) {
      const bool ours = (value.fourcc == F("VARF") && type == Type::Real) ||
                        (value.fourcc == F("VARI") && type == Type::Int) ||
                        (value.fourcc == F("VARV") && type == Type::Vector) ||
                        (value.fourcc == F("VARC") && type == Type::Color);
      if (ours) {
        for (size_t i = 0; i < m_vars.size(); ++i) {
          if (m_vars[i].guid == value.args[0].guid && m_vars[i].type == VarTypeOf(value.fourcc)) {
            AppendBE32(out, value.fourcc);
            AppendBE32(out, uint32_t(i));
            return true;
          }
        }
        why = EffectFourCCString(value.fourcc) + " " + EffectGuidString(value.args[0].guid) +
              " is not in the effect's PVAR";
        return false;
      }
    }
    const Signatures& elements = ElementsOf(type);
    const auto found = elements.find(value.fourcc);
    if (found == elements.end()) {
      why = "retail has no element " + EffectFourCCString(value.fourcc) + " there";
      return false;
    }
    const std::string sig = found->second;
    AppendBE32(out, value.fourcc);
    if (sig == "#") {
      return Literal(value, type, out, why);
    }
    if (sig == "k") {
      return value.args.size() == 1 && Keyframes(value.args[0], type, out, why);
    }
    if (value.args.size() != sig.size()) {
      why = EffectFourCCString(value.fourcc) + " with " + std::to_string(value.args.size()) + " arguments, retail reads " +
            std::to_string(sig.size());
      return false;
    }
    for (size_t i = 0; i < sig.size(); ++i) {
      const bool ok = sig[i] == 'B' ? Bool(value.args[i], out, why) : Element(value.args[i], TypeOfLetter(sig[i]), out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // REUL(x, y, z, #00) as ANGC's X bias (-x), for a rotation about X only.
  bool ConeBias(const EffectValue& reul, uint32_t& xBias, std::string& why) const {
    if (!IsElement(reul, F("REUL")) || reul.args.size() != 4) {
      why = "a cone rotation that is not REUL";
      return false;
    }
    uint32_t angles[3];
    for (size_t i = 0; i < 3; ++i) {
      const EffectValue& angle = reul.args[i];
      if (!IsElement(angle, F("CNST")) || angle.args.size() != 1 || angle.args[0].kind != EffectValue::Kind::Word) {
        why = "a cone rotation that is not constant";
        return false;
      }
      angles[i] = angle.args[0].word;
    }
    if ((angles[1] & 0x7fffffffu) != 0 || (angles[2] & 0x7fffffffu) != 0) {
      why = "a cone rotated about Y or Z";
      return false;
    }
    if ((angles[0] & 0x7fffffffu) == 0) {
      xBias = 0x80000000u;
    } else {
      xBias = angles[0] ^ 0x80000000u;
      m_approximated.push_back("rotated cone taken as an X bias");
    }
    return true;
  }

  // Remastered's camera-facing model rotation,
  // RADD(RAZY, SUB_(CPSS(PLOC, CNST(0, 1, 0), REUL(0, 0, 0, #00)), REUL(x, y, z, #00))): the model
  // turned to face the camera, then by the inverse of REUL. Written as PMRT CNST(-x, -y, -z), which
  // is that inverse when at most one angle is nonzero, plus the port-only PFCM flag (CElementGen
  // turns each particle to the camera). An IRND angle (a random spin) is taken as 0.
  bool FacingRotation(const std::vector<EffectValue>& value, std::vector<uint8_t>& out) const {
    if (value.size() != 1 || !IsElement(value[0], F("RADD")) || value[0].args.size() != 2 ||
        !IsElement(value[0].args[0], F("RAZY"))) {
      return false;
    }
    const EffectValue& sub = value[0].args[1];
    if (!IsElement(sub, F("SUB_")) || sub.args.size() != 2) {
      return false;
    }
    const EffectValue& look = sub.args[0];
    if (!IsElement(look, F("CPSS")) || look.args.size() != 3 || !IsElement(look.args[0], F("PLOC"))) {
      return false;
    }
    const EffectValue& up = look.args[1];
    if (!IsElement(up, F("CNST")) || up.args.size() != 3 || !ConstIs(up.args[0], 0) ||
        !ConstIs(up.args[1], 0x3f800000u) || !ConstIs(up.args[2], 0)) {
      return false;
    }
    uint32_t angles[3];
    if (!ReulAngles(look.args[2], angles, nullptr) || ((angles[0] | angles[1] | angles[2]) & 0x7fffffffu) != 0) {
      return false;
    }
    bool spin = false;
    if (!ReulAngles(sub.args[1], angles, &spin)) {
      return false;
    }
    int nonzero = 0;
    for (uint32_t angle : angles) {
      nonzero += (angle & 0x7fffffffu) != 0;
    }
    if (nonzero > 1) {
      return false;
    }
    if (spin) {
      m_approximated.push_back("PMRQ: a camera-facing model's random spin left out");
    }
    AppendBE32(out, F("CNST"));
    for (uint32_t angle : angles) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, (angle & 0x7fffffffu) != 0 ? angle ^ 0x80000000u : 0);
    }
    return true;
  }

  // REUL(CNST x, CNST y, CNST z, #00)'s angle bits. With `irnd`, an angle using IRND reads as 0.
  static bool ReulAngles(const EffectValue& reul, uint32_t (&angles)[3], bool* irnd) {
    if (!IsElement(reul, F("REUL")) || reul.args.size() != 4 ||
        (reul.args[3].kind != EffectValue::Kind::Byte && reul.args[3].kind != EffectValue::Kind::Word) ||
        reul.args[3].word != 0) {
      return false;
    }
    for (size_t i = 0; i < 3; ++i) {
      if (irnd != nullptr && UsesElement(reul.args[i], F("IRND"))) {
        angles[i] = 0;
        *irnd = true;
      } else if (!ConstWord(reul.args[i], angles[i])) {
        return false;
      }
    }
    return true;
  }

  static bool UsesElement(const EffectValue& value, uint32_t fourcc) {
    if (value.kind == EffectValue::Kind::Element && value.fourcc == fourcc) {
      return true;
    }
    return std::any_of(value.args.begin(), value.args.end(),
                       [fourcc](const EffectValue& arg) { return UsesElement(arg, fourcc); });
  }

  // PMRQ (a model particle's rotation) REUL(x, y, z, #00) as retail's PMRT
  // CNST(x, y, z). CERotationEuler::QuatGeneric's order 0 is Rz * Ry * Rx in
  // degrees, as CElementGen builds PMRT. IRND is left out: retail evaluates a
  // varying PMRT afresh each frame, where IRND gives 0 after frame 0.
  bool ModelRotation(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue* reul = value.size() == 1 ? &value[0] : nullptr;
    if (reul == nullptr || !IsElement(*reul, F("REUL")) || reul->args.size() != 4) {
      why = "a rotation that is not REUL";
      return false;
    }
    const EffectValue& order = reul->args[3];
    if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
      why = "a rotation order other than 0 (XYZ)";
      return false;
    }
    AppendBE32(out, F("CNST"));
    for (size_t i = 0; i < 3; ++i) {
      if (UsesElement(reul->args[i], F("IRND"))) {
        why = "an angle with IRND";
        return false;
      }
      if (!Element(reul->args[i], Type::Real, out, why)) {
        return false;
      }
    }
    return true;
  }

  // GRAD: u8 stop count, per stop four halves (RGBA) and an f32 position,
  // then what the position runs over: ILPT(CNST(n)) (n% of the particle's
  // life) or CNST(n) (n frames), then a byte (1: the gradient repeats).
  // Written as retail's percent keyframes (KEYP, 101 keys over the life).
  bool Gradient(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.args.size() != 1 || value.args[0].kind != EffectValue::Kind::Raw) {
      why = "GRAD without its stops";
      return false;
    }
    const uint8_t* p = m_data + value.args[0].offset;
    const size_t size = value.args[0].size;
    const size_t stops = size == 0 ? 0 : p[0];
    size_t at = 1 + 12 * stops;
    if (stops == 0 || at > size) {
      why = "GRAD without its stops";
      return false;
    }
    // How many percent of the life one pass over the gradient takes.
    float span = 0.0f;
    if (at + 12 <= size && ReadLE32(p + at) == F("ILPT") && ReadLE32(p + at + 4) == F("CNST")) {
      span = float(int32_t(ReadLE32(p + at + 8)));
      at += 12;
    } else if (at + 8 <= size && ReadLE32(p + at) == F("CNST")) {
      const float frames = float(int32_t(ReadLE32(p + at + 4)));
      at += 8;
      if (m_lifetime > 0) {
        span = frames * 100.0f / float(m_lifetime);
      } else {
        span = 100.0f;
        m_approximated.push_back("GRAD over frames taken over the life (no constant lifetime)");
      }
    } else {
      why = "GRAD over something other than the life or frames";
      return false;
    }
    if (span <= 0.0f) {
      why = "GRAD over no time";
      return false;
    }
    const bool repeat = at < size && p[at] != 0;
    auto colorAt = [p, stops](float t) {
      std::array<float, 4> color{};
      auto stop = [p](size_t i, size_t c) { return HalfToFloat(uint16_t(p[1 + 12 * i + 2 * c] | p[2 + 12 * i + 2 * c] << 8)); };
      auto position = [p](size_t i) {
        float v;
        std::memcpy(&v, p + 1 + 12 * i + 8, 4);
        return v;
      };
      size_t next = 0;
      while (next < stops && position(next) < t) {
        ++next;
      }
      for (size_t c = 0; c < 4; ++c) {
        if (next == 0) {
          color[c] = stop(0, c);
        } else if (next == stops) {
          color[c] = stop(stops - 1, c);
        } else {
          const float a = position(next - 1);
          const float b = position(next);
          const float f = b > a ? (t - a) / (b - a) : 1.0f;
          color[c] = stop(next - 1, c) + (stop(next, c) - stop(next - 1, c)) * f;
        }
      }
      return color;
    };
    AppendBE32(out, F("KEYP"));
    AppendBE32(out, 1);   // percent of the particle's life
    AppendBE32(out, 0);
    out.push_back(0);  // no loop
    out.push_back(0);
    AppendBE32(out, 101);
    AppendBE32(out, 0);
    AppendBE32(out, 101);
    for (int percent = 0; percent <= 100; ++percent) {
      float t = float(percent) / span;
      t = repeat ? t - std::floor(t) : std::min(t, 1.0f);
      for (float c : colorAt(t)) {
        AppendBE32(out, FloatBits(c));
      }
    }
    return true;
  }

  // CNST's literal: retail reads an s32 for an int and a float for a real.
  bool Literal(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.args.size() != 1) {
      why = "CNST with " + std::to_string(value.args.size()) + " arguments";
      return false;
    }
    const EffectValue& arg = value.args[0];
    if (arg.kind == EffectValue::Kind::Word) {
      AppendBE32(out, arg.word);
      return true;
    }
    if (arg.kind == EffectValue::Kind::Byte) {
      AppendBE32(out, type == Type::Int ? arg.word : FloatBits(float(arg.word)));
      return true;
    }
    why = "CNST holding neither a number nor a byte";
    return false;
  }

  // GetBool: a class id (CNST) and a byte.
  bool Bool(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue* byte = &value;
    if (IsElement(value, F("CNST")) && value.args.size() == 1) {
      byte = &value.args[0];
    }
    if (byte->kind != EffectValue::Kind::Byte && byte->kind != EffectValue::Kind::Word) {
      why = "not a flag";
      return false;
    }
    AppendBE32(out, F("CNST"));
    out.push_back(byte->word != 0 ? 1 : 0);
    return true;
  }

  // A keyframe block: percent, unknown, loop, unknown (u32 u32 u8 u8), loop
  // end and start (u32 u32), a count and that many keys of the type's size:
  // 4 bytes for an int or real, 12 for a vector, 16 for a colour. Remastered
  // may store a colour's key as four halves; those are widened.
  bool Keyframes(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Keys || value.size < 22) {
      why = "not a keyframe block";
      return false;
    }
    const uint8_t* p = m_data + value.offset;
    const uint32_t count = ReadLE32(p + 18);
    const size_t keys = value.size - 22;
    const size_t want = type == Type::Color ? 16 : type == Type::Vector ? 12 : 4;
    const size_t keySize = count == 0 ? want : keys / count;
    const bool halves = type == Type::Color && keySize == 8;
    if ((count == 0 && keys != 0) || (count != 0 && keys % count != 0) || (keySize != want && !halves)) {
      why = "keyframes of " + std::to_string(keySize) + " bytes where retail reads " + std::to_string(want);
      return false;
    }
    AppendBE32(out, ReadLE32(p));
    AppendBE32(out, ReadLE32(p + 4));
    out.push_back(p[8]);
    out.push_back(p[9]);
    AppendBE32(out, ReadLE32(p + 10));
    AppendBE32(out, ReadLE32(p + 14));
    AppendBE32(out, count);
    if (halves) {
      for (size_t at = 22; at < value.size; at += 2) {
        AppendBE32(out, FloatBits(HalfToFloat(uint16_t(p[at] | p[at + 1] << 8))));
      }
      return true;
    }
    for (size_t at = 22; at < value.size; at += 4) {
      AppendBE32(out, ReadLE32(p + at));
    }
    return true;
  }

  // An id, bare or as CNST(id); NONE is no asset.
  bool Asset(const std::vector<EffectValue>& value, uint32_t type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.size() == 1 && IsElement(value[0], F("NONE"))) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    // A zero id names nothing, as NONE does.
    if (value.size() == 1 && value[0].kind == EffectValue::Kind::Guid && value[0].guid == EffectGuid{}) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    const EffectValue* guid = nullptr;
    if (value.size() == 1 && value[0].kind == EffectValue::Kind::Guid) {
      guid = &value[0];
    } else if (value.size() == 1 && IsElement(value[0], F("CNST")) && value[0].args.size() == 1 &&
               value[0].args[0].kind == EffectValue::Kind::Guid) {
      guid = &value[0].args[0];
    } else if (value.size() == 2 && value[0].kind == EffectValue::Kind::Byte && value[1].kind == EffectValue::Kind::Guid) {
      guid = &value[1];
    }
    if (guid == nullptr) {
      why = "not an asset id";
      return false;
    }
    const uint32_t id = AssetId(guid->guid, type);
    if (id == 0) {
      why = EffectFourCCString(type) + " " + EffectGuidString(guid->guid) + " has no retail id";
      return false;
    }
    AppendBE32(out, F("CNST"));
    AppendBE32(out, id);
    return true;
  }

  // PATL: TXP2 (an atlas, a random tile), TXFB (an array texture over the
  // particle's life, with TRST's optional random mirror) or ATX2 (an atlas over
  // the particle's life). Only those patterns.
  bool Atlas(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue& head = value[0];
    const std::string name = EffectFourCCString(head.fourcc);
    if (head.args.empty() || head.args[0].kind != EffectValue::Kind::Guid) {
      why = name + " without a texture";
      return false;
    }
    uint32_t id = 0;
    int32_t cols = 0, rows = 0, count = 0, mode = 0, flip = 0;
    if (head.fourcc == F("TXP2")) {
      uint32_t c, r, high;
      if (value.size() != 1 || head.args.size() != 4 || !ConstWord(head.args[1], c) || !ConstWord(head.args[2], r) ||
          c == 0 || r == 0 || c > 64 || r > 64 || !RangeFromZero(head.args[3], F("IRND"), high)) {
        why = "TXP2 that is not an atlas with a random tile";
        return false;
      }
      cols = int32_t(c);
      rows = int32_t(r);
      // The range is a real over the whole atlas, or an int tile index.
      if (high == FloatBits(1.0f)) {
        count = cols * rows;
      } else if (high < uint32_t(cols * rows)) {
        count = int32_t(high) + 1;
      } else {
        why = "TXP2 tile range " + std::to_string(high) + " is not the atlas";
        return false;
      }
      id = AssetId(head.args[0].guid, F("TXTR"));
      if (id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
        return false;
      }
    } else if (head.fourcc == F("ATX2")) {
      // ATX2(id, cols, rows, cycle frames, loop): with the cycle ILPT(100),
      // retail's whole lifetime in frames, the atlas plays once over the life.
      uint32_t c, r;
      const EffectValue& cycle = head.args.size() == 5 ? head.args[3] : head;
      if (value.size() != 1 || head.args.size() != 5 || !ConstWord(head.args[1], c) ||
          !ConstWord(head.args[2], r) || c == 0 || r == 0 || c > 64 || r > 64 || !IsElement(cycle, F("ILPT")) ||
          cycle.args.size() != 1 || !ConstIs(cycle.args[0], 100)) {
        why = "ATX2 that is not an atlas over the particle's life";
        return false;
      }
      id = AssetId(head.args[0].guid, F("TXTR"));
      if (id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
        return false;
      }
      cols = int32_t(c);
      rows = int32_t(r);
      count = cols * rows;
      mode = 1;
    } else {
      uint32_t high;
      bool mirror = false;
      if (value.size() > 2 || head.args.size() != 2 || !IsElement(head.args[1], F("LFTW")) ||
          !RangeFromZero(head.args[1], F("LFTW"), high) || high != FloatBits(1.0f) ||
          (value.size() == 2 && !FlipbookTransform(value[1], mirror))) {
        why = "TXFB that is not a flipbook over the particle's life";
        return false;
      }
      const FlipbookAtlas atlas = m_io.flipbook ? m_io.flipbook(head.args[0].guid) : FlipbookAtlas{};
      if (atlas.id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no flipbook atlas";
        return false;
      }
      id = atlas.id;
      cols = atlas.cols;
      rows = atlas.rows;
      count = atlas.frames;
      mode = 1;
      flip = mirror ? 1 : 0;
    }
    AppendBE32(out, F("PATL"));
    AppendBE32(out, F("CNST"));
    AppendBE32(out, id);
    for (const int32_t v : {cols, rows, count, mode, flip}) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, uint32_t(v));
    }
    return true;
  }

  // PMDL's SLCT(IRND(0, n - 1), ARRY of n CNST ids), as the model ids.
  bool Variants(const std::vector<EffectValue>& value, std::vector<uint32_t>& ids, std::string& why) const {
    uint32_t high;
    if (value.size() != 1 || !IsElement(value[0], F("SLCT")) || value[0].args.size() != 2 ||
        !RangeFromZero(value[0].args[0], F("IRND"), high) || !IsElement(value[0].args[1], F("ARRY")) ||
        value[0].args[1].args.size() != 1 || value[0].args[1].args[0].kind != EffectValue::Kind::Raw) {
      why = "a model choice that is not SLCT(IRND(0, n - 1), ARRY)";
      return false;
    }
    const EffectValue& array = value[0].args[1].args[0];
    const uint8_t* p = m_data + array.offset;
    const uint32_t count = array.size >= 4 ? ReadLE32(p) : 0;
    if (count == 0 || count != high + 1 || array.size != 4 + size_t(count) * 20) {
      why = "SLCT over " + std::to_string(count) + " models with a range of " + std::to_string(high);
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* entry = p + 4 + size_t(i) * 20;
      EffectGuid guid;
      std::memcpy(guid.data(), entry + 4, 16);
      if (ReadLE32(entry) != F("CNST")) {
        why = "SLCT model that is not a constant";
        return false;
      }
      const uint32_t id = AssetId(guid, F("CMDL"));
      if (id == 0) {
        why = "CMDL " + EffectGuidString(guid) + " has no retail id";
        return false;
      }
      ids.push_back(id);
    }
    return true;
  }

  // TEXR/TIND: Remastered's `CNST(id), NONE` or `ATEX(id, ...)`, retail's
  // `CNST CNST id` and `ATEX CNST id ...`.
  bool Texture(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.empty() || value[0].kind != EffectValue::Kind::Element) {
      why = "not a texture";
      return false;
    }
    const EffectValue& head = value[0];
    if (head.fourcc == F("NONE")) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    if (head.fourcc == F("TXP2") || head.fourcc == F("TXFB") || head.fourcc == F("ATX2")) {
      return Atlas(value, out, why);
    }
    const bool animated = head.fourcc == F("ATEX");
    if ((head.fourcc != F("CNST") && !animated) || head.args.empty() || head.args[0].kind != EffectValue::Kind::Guid) {
      why = "texture element " + EffectFourCCString(head.fourcc) + " retail does not have";
      return false;
    }
    const uint32_t id = AssetId(head.args[0].guid, F("TXTR"));
    if (id == 0) {
      why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
      return false;
    }
    AppendBE32(out, head.fourcc);
    AppendBE32(out, F("CNST"));
    AppendBE32(out, id);
    if (!animated) {
      return true;
    }
    // ATEX: tile width and height, stride width and height, cycle frames, loop.
    const char* sig = "iiiiiB";
    if (head.args.size() != 7) {
      why = "ATEX with " + std::to_string(head.args.size()) + " arguments";
      return false;
    }
    for (size_t i = 0; i < 6; ++i) {
      const bool ok = sig[i] == 'B' ? Bool(head.args[i + 1], out, why) : Element(head.args[i + 1], Type::Int, out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // One property's value, as the type retail reads it.
  bool Property(uint32_t fourcc, const PropertyType& type, const std::vector<EffectValue>& value,
                std::vector<uint8_t>& out, std::string& why) const {
    switch (type.type) {
    case Type::Bool:
      if (value.size() != 1) {
        why = "not a flag";
        return false;
      }
      return Bool(value[0], out, why);
    case Type::Texture:
      return Texture(value, out, why);
    case Type::Asset:
      return Asset(value, type.asset, out, why);
    default:
      break;
    }
    // LFOT and LTYP: Remastered writes the enum as a byte. The exe maps it to retail's
    // int as LFOT 0-3 unchanged and LTYP 0, 2, 1, 3 (its point and spot are swapped).
    if (fourcc == F("LFOT") || fourcc == F("LTYP")) {
      const EffectValue* byte = value.size() == 1 ? &value[0] : nullptr;
      if (byte != nullptr && IsElement(*byte, F("CNST")) && byte->args.size() == 1) {
        byte = &byte->args[0];
      }
      if (byte != nullptr && byte->kind == EffectValue::Kind::Byte) {
        if (byte->word > 3) {
          why = "enum byte " + std::to_string(byte->word);
          return false;
        }
        static constexpr uint32_t kLightType[4] = {0, 2, 1, 3};
        AppendBE32(out, F("CNST"));
        AppendBE32(out, fourcc == F("LTYP") ? kLightType[byte->word] : byte->word);
        return true;
      }
    }
    if (value.size() != 1) {
      why = "a value of " + std::to_string(value.size()) + " parts where retail reads one element";
      return false;
    }
    // ROTA: Remastered turns the other way. MULT(x, -1) is x in retail; anything
    // else is wrapped in MULT(..., CNST -1).
    // LTM2 is one frame longer than retail's LTME: constants, and the bounds
    // of a random lifetime, come down by one.
    if (fourcc == F("LTME")) {
      const EffectValue& v = value[0];
      auto constant = [](const EffectValue& c) {
        return IsElement(c, F("CNST")) && c.args.size() == 1 &&
               (c.args[0].kind == EffectValue::Kind::Word || c.args[0].kind == EffectValue::Kind::Byte);
      };
      auto putLess = [&out](const EffectValue& c) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, c.args[0].word == 0 ? 0 : c.args[0].word - 1);
      };
      if (constant(v)) {
        putLess(v);
        return true;
      }
      if ((IsElement(v, F("IRND")) || IsElement(v, F("RAND")) || IsElement(v, F("MPRD"))) && v.args.size() == 2 &&
          constant(v.args[0]) && constant(v.args[1])) {
        AppendBE32(out, v.fourcc == F("MPRD") ? F("RAND") : v.fourcc);
        putLess(v.args[0]);
        putLess(v.args[1]);
        return true;
      }
      AppendBE32(out, F("SUB_"));
      if (!Element(v, Type::Int, out, why)) {
        return false;
      }
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
      return true;
    }
    if (fourcc == F("ROTA")) {
      const EffectValue& v = value[0];
      if (IsElement(v, F("CNST")) && v.args.size() == 1 && v.args[0].kind == EffectValue::Kind::Word) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, v.args[0].word ^ 0x80000000u);
        return true;
      }
      if (IsElement(v, F("MULT")) && v.args.size() == 2 && IsElement(v.args[1], F("CNST")) && v.args[1].args.size() == 1 &&
          v.args[1].args[0].kind == EffectValue::Kind::Word && v.args[1].args[0].word == FloatBits(-1.0f)) {
        return Element(v.args[0], Type::Real, out, why);
      }
      AppendBE32(out, F("MULT"));
      if (!Element(v, Type::Real, out, why)) {
        return false;
      }
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(-1.0f));
      return true;
    }
    return Element(value[0], type.type, out, why);
  }

  // A swoosh or electric child a spawn table starts: retail has one of each
  // per generator (SSWH at frame SSSD, SELC at frame SESD), not in KSSM.
  struct Started {
    uint32_t type;  // SWHC or ELSC
    uint32_t id;
    uint32_t frame;
  };

  // Retail's KSSM after its FourCC, from Remastered's: CNST, four ints (the
  // third is the end frame) and per frame its PART ids. Empty when nothing
  // but swooshes and electric children are spawned (they go to `started`).
  // Remastered's tables are merged and its SEVT events (moves) dropped; what
  // a table's selector chooses is not known.
  bool SpawnTable(const EffectProperty& property, std::vector<uint8_t>& out, std::vector<Started>& started,
                  std::string& why) const {
    EffectSpawnTable table;
    if (!ParseSpawnTable(m_data, property.offset + property.size, property, table)) {
      why = "spawn table does not parse";
      return false;
    }
    if (table.tables.empty()) {
      return true;
    }
    std::map<uint32_t, std::vector<uint32_t>> frames;
    size_t unresolved = 0;
    bool conditional = false;
    bool selected = false;
    for (const EffectSpawnTable::Table& each : table.tables) {
      selected = selected || !IsElement(each.selector, F("CNST")) || each.selector.args.size() != 1 ||
                 each.selector.args[0].kind != EffectValue::Kind::Word || each.selector.args[0].word != 0;
      for (const EffectSpawnTable::Frame& frame : each.frames) {
        for (const EffectSpawnTable::Spawn& spawn : frame.spawns) {
          const uint32_t type = spawn.form == F("GENP")   ? F("PART")
                                : spawn.form == F("SWSH") ? F("SWHC")
                                : spawn.form == F("ELC2") || spawn.form == F("ELSM") ? F("ELSC")
                                                                                     : 0;
          const uint32_t id = type != 0 ? AssetId(spawn.id, type) : 0;
          if (id == 0) {
            ++unresolved;
            continue;
          }
          conditional = conditional || spawn.conditional;
          if (type == F("PART")) {
            frames[frame.frame].push_back(id);
          } else {
            started.push_back({type, id, frame.frame});
          }
        }
      }
    }
    if (frames.empty() && started.empty()) {
      why = "no spawned child resolves";
      return false;
    }
    if (table.events != 0) {
      m_approximated.push_back("KSSM: " + std::to_string(table.events) + " SEVT events dropped");
    }
    if (table.tables.size() > 1) {
      m_approximated.push_back("KSSM: " + std::to_string(table.tables.size()) + " tables merged");
    }
    if (selected) {
      m_approximated.push_back("KSSM: table selector ignored");
    }
    if (conditional) {
      m_approximated.push_back("KSSM: spawn conditions ignored, every spawn always starts");
    }
    if (unresolved != 0) {
      m_approximated.push_back("KSSM: " + std::to_string(unresolved) + " spawns unresolved");
    }
    if (frames.empty()) {
      return true;
    }
    AppendBE32(out, F("CNST"));
    for (uint32_t word : {0u, 1u, table.header[2], 0u}) {
      AppendBE32(out, word);
    }
    AppendBE32(out, uint32_t(frames.size()));
    for (const auto& [frame, ids] : frames) {
      AppendBE32(out, frame);
      AppendBE32(out, uint32_t(ids.size()));
      for (uint32_t id : ids) {
        AppendBE32(out, id);
        for (int i = 0; i < 3; ++i) {
          AppendBE32(out, 0);
        }
      }
    }
    return true;
  }

  // A one-byte or one-word CNST property (PBDM, ORNT, SCTR).
  static bool SmallConst(const EffectProperty& property, uint32_t& word) {
    if (property.value.size() != 1 || !IsElement(property.value[0], F("CNST")) || property.value[0].args.size() != 1) {
      return false;
    }
    const EffectValue& arg = property.value[0].args[0];
    if (arg.kind != EffectValue::Kind::Word && arg.kind != EffectValue::Kind::Byte) {
      return false;
    }
    word = arg.word;
    return true;
  }

  // SMVR as `SMOV(EXTT, CNST(0, 0, 0), EXTR, NONE, NONE)`: the emitter's own
  // translation and rotation with no offset (1114 of 1233 SMVR forms).
  static bool IdentityMover(const EffectProperty& mover) {
    if (mover.value.size() != 1 || !IsElement(mover.value[0], F("SMOV")) || mover.value[0].args.size() != 5) {
      return false;
    }
    const std::vector<EffectValue>& args = mover.value[0].args;
    const EffectValue& offset = args[1];
    if (!IsElement(args[0], F("EXTT")) || !IsElement(args[2], F("EXTR")) || !IsElement(args[3], F("NONE")) ||
        !IsElement(args[4], F("NONE")) || !IsElement(offset, F("CNST")) || offset.args.size() != 3) {
      return false;
    }
    uint32_t bits;
    return std::all_of(offset.args.begin(), offset.args.end(), [&bits](const EffectValue& each) {
      return ConstWord(each, bits) && (bits & 0x7fffffffu) == 0;
    });
  }

  // XFMD, Remastered's transform mode (build/mpr/vfx/RESOLVED-xfmd.md): 3 keeps
  // the particles in emitter space, 4 does too at unit scale with SMVR as the
  // mover. Written only for those two; 1, 2 and 5 draw as retail does.
  void Transform(const EffectProperty& xfmd, const EffectProperty* mover, std::vector<uint8_t>& out) const {
    uint32_t mode = 2;
    if (!SmallConst(xfmd, mode)) {
      m_approximated.push_back("XFMD: not a constant, drawn as retail");
      return;
    }
    if (mode == 3 || mode == 4) {
      AppendBE32(out, F("XFMD"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, mode);
    } else if (mode != 1 && mode != 2 && mode != 5) {
      m_approximated.push_back("XFMD " + std::to_string(mode) + " drawn as retail");
    }
    if (mode == 4 && (mover == nullptr || !IdentityMover(*mover))) {
      m_approximated.push_back(mover == nullptr ? "XFMD 4 with no SMVR taken as the emitter's transform"
                                                : "SMVR taken as the emitter's transform");
    }
  }

  // The count-prefixed list `CNST n, items` of a port-only property.
  static void PutList(std::vector<uint8_t>& out, uint32_t fourcc, uint32_t count, const std::vector<uint8_t>& items) {
    AppendBE32(out, fourcc);
    AppendBE32(out, F("CNST"));
    AppendBE32(out, count);
    out.insert(out.end(), items.begin(), items.end());
  }

  // The port-only properties of a generator whose material instance has a
  // recipe: VMAT and the per-particle data it reads (build/mpr/vfx/DESIGN.md).
  // Writes nothing where the MATI is unavailable or its shader has no recipe.
  // A swoosh's (`swoosh`) has no SSZE or VORN, and its blend mode is SBDM.
  bool Material(const EffectProperty& material, const EffectNode& node, ConvertedPart& result, bool swoosh) const {
    if (!m_io.materialData || !m_io.vfxTexture) {
      return false;
    }
    const EffectValue* guid = nullptr;
    for (const EffectValue& value : material.value) {
      if (value.kind == EffectValue::Kind::Guid) {
        guid = &value;
      }
    }
    if (guid == nullptr) {
      return false;
    }
    const std::vector<uint8_t> data = m_io.materialData(guid->guid);
    Mati mati;
    if (!ParseMati(data, mati)) {
      result.dropped.push_back("VMAT: no MATI for its material");
      return false;
    }
    char shaderText[16];
    std::snprintf(shaderText, sizeof(shaderText), "%08x", mati.shader);
    const Recipe* recipe = RecipeOf(mati.shader);
    if (recipe == nullptr) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has no recipe");
      return false;
    }

    // The node's properties by fourcc.
    auto find = [&node](uint32_t fourcc) -> const EffectProperty* {
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == fourcc) {
          return &property;
        }
      }
      return nullptr;
    };
    const auto cch0 = mati.vectors.count("CCH0") ? mati.vectors.at("CCH0") : std::array<float, 4>{};
    if (recipe->unitCch0 && (cch0[2] != 1.0f || cch0[3] != 0.0f)) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has a fade gain/bias the renderer lacks");
      return false;
    }

    // Textures, in the recipe's slot order.
    struct Slot {
      FlipbookAtlas atlas;
      MatiTexture texture;
      const TexSpec* spec;
    };
    std::vector<Slot> slots;
    for (const TexSpec& spec : recipe->textures) {
      const auto found = mati.textures.find(spec.tag);
      if (found == mati.textures.end()) {
        m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has no " + spec.tag + " texture");
        return false;
      }
      const FlipbookAtlas atlas = m_io.vfxTexture(found->second.guid);
      if (atlas.id == 0) {
        m_approximated.push_back(std::string("VMAT: ") + spec.tag + " texture did not import");
        return false;
      }
      slots.push_back({atlas, found->second, &spec});
    }

    uint32_t blend = 0;
    if (const EffectProperty* sbdm = swoosh ? find(F("SBDM")) : nullptr) {
      uint32_t word = 0;
      if (SmallConst(*sbdm, word) && word <= 1) {
        blend = word == 1 ? 2 : 0;
      } else {
        m_approximated.push_back("VMAT: SBDM taken as alpha blend");
      }
    } else if (const EffectProperty* pbdm = swoosh ? nullptr : find(F("PBDM"))) {
      uint32_t word = 0;
      if (SmallConst(*pbdm, word) && word <= 3) {
        blend = word;
      } else {
        m_approximated.push_back("VMAT: PBDM taken as alpha blend");
      }
    }
    if (recipe->frameBuffer) {
      if (cch0[0] != 0.0f || cch0[1] != 0.0f) {
        m_approximated.push_back(std::string("VMAT: shader ") + shaderText + "'s scene warp dropped (drawn as a multiply)");
      }
      blend = kBlendMultiply;
    }
    uint32_t spriteCenter = 0;
    if (const EffectProperty* sctr = find(F("SCTR"))) {
      uint32_t word = 0;
      if (SmallConst(*sctr, word) && word <= 8) {
        spriteCenter = word;
      } else {
        m_approximated.push_back("VMAT: SCTR taken as 0");
      }
    }

    // The blob.
    std::vector<uint8_t> blob;
    AppendBE32(blob, 2);
    AppendBE32(blob, recipe->features);
    AppendBE32(blob, blend);
    AppendBE32(blob, uint32_t(slots.size()));
    int32_t slotOf[7] = {-1, -1, -1, -1, -1, -1, -1};
    for (size_t i = 0; i < slots.size(); ++i) {
      const Slot& slot = slots[i];
      slotOf[size_t(slot.spec->role)] = int32_t(i);
      AppendBE32(blob, slot.atlas.id);
      AppendBE32(blob, slot.texture.texCoord);
      AppendBE32(blob, WrapOf(slot.texture.wrapX));
      AppendBE32(blob, WrapOf(slot.texture.wrapY));
      AppendBE32(blob, slot.texture.filter != 0 ? 1 : 0);
      AppendBE32(blob, uint32_t(std::max(slot.atlas.cols, 1)));
      AppendBE32(blob, uint32_t(std::max(slot.atlas.rows, 1)));
      AppendBE32(blob, uint32_t(std::max(slot.atlas.frames, 1)));
      const bool warped = slot.spec->warpU >= 0;
      AppendBE32(blob, warped ? 1 : 0);
      AppendBE32(blob, FloatBits(warped ? cch0[size_t(slot.spec->warpU)] : 0.0f));
      AppendBE32(blob, FloatBits(warped ? cch0[size_t(slot.spec->warpV)] : 0.0f));
    }
    for (const Role role : {Role::Color, Role::Opacity, Role::Ramp, Role::Ramp2, Role::Threshold, Role::Indirect,
                            Role::Palette}) {
      AppendBE32(blob, uint32_t(slotOf[size_t(role)]));
    }
    AppendBE32(blob, 0);             // rampRow[2]
    AppendBE32(blob, 1);
    AppendBE32(blob, uint32_t(recipe->addRow));
    int32_t srcRow[kSrcCount], srcComp[kSrcCount];
    float srcValue[kSrcCount];
    for (int i = 0; i < kSrcCount; ++i) {
      srcRow[i] = -1;
      srcComp[i] = 0;
      srcValue[i] = i == kSrcIndexScale ? 1.0f : 0.0f;
    }
    for (const SrcSpec& src : recipe->sources) {
      srcRow[src.index] = src.row;
      srcComp[src.index] = src.comp < 0 ? 0 : src.comp;
      if (src.row < 0) {
        srcValue[src.index] = cch0[size_t(src.comp)];
      }
    }
    for (int i = 0; i < kSrcCount; ++i) {
      AppendBE32(blob, uint32_t(srcRow[i]));
      AppendBE32(blob, uint32_t(srcComp[i]));
      AppendBE32(blob, FloatBits(srcValue[i]));
    }
    AppendBE32(blob, FloatBits(1.0f));  // modulate
    AppendBE32(blob, FloatBits(0.0f));  // depthSoften
    AppendBE32(blob, spriteCenter);

    AppendBE32(result.part, F("VMAT"));
    AppendBE32(result.part, F("CNST"));
    AppendBE32(result.part, uint32_t(blob.size()));
    result.part.insert(result.part.end(), blob.begin(), blob.end());

    // VTMT: one TRSS per UV set (A-F), up to three.
    if (const EffectProperty* tmtr = find(F("TMTR"))) {
      std::vector<uint8_t> items;
      uint32_t count = 0;
      std::string why;
      bool ok = true;
      for (const EffectValue& transform : tmtr->value) {
        if (!IsElement(transform, F("TRSS")) || transform.args.size() != 6) {
          ok = false;
          why = "not a six-argument TRSS";
          break;
        }
        if (count == 3) {
          ok = false;
          why = "more than three UV sets";
          break;
        }
        for (const EffectValue& arg : transform.args) {
          if (!Element(arg, Type::Real, items, why)) {
            ok = false;
            break;
          }
        }
        if (!ok) {
          break;
        }
        ++count;
      }
      if (ok && count != 0) {
        PutList(result.part, F("VTMT"), count, items);
      } else if (!ok) {
        result.dropped.push_back("TMTR: " + why);
      }
    }

    // VPMT: each PMTR item to a row and component of the per-particle extras.
    if (const EffectProperty* pmtr = find(F("PMTR"))) {
      std::vector<uint8_t> items;
      uint32_t count = 0;
      for (const EffectValue& item : pmtr->value) {
        const uint32_t row = item.word & 0xff, comp = item.word >> 8 & 0xff, width = item.word >> 16 & 0xff;
        // Group 0 an int, 1 a real, 2 a vector, 4 a colour.
        uint32_t kind = 0;
        Type type = Type::Real;
        uint32_t expect = 1;
        switch (item.fourcc) {
        case 0: kind = 2; type = Type::Int; break;
        case 1: break;
        case 2: kind = 1; type = Type::Vector; expect = 3; break;
        case 4: kind = 3; type = Type::Color; expect = 4; break;
        default: expect = 0; break;
        }
        if (item.args.size() != 1 || expect == 0 || width != expect || row >= 4 || comp + expect > 4) {
          result.dropped.push_back("PMTR: an entry of group " + std::to_string(item.fourcc) + " that VPMT cannot hold");
          continue;
        }
        std::vector<uint8_t> element;
        std::string why;
        if (!Element(item.args[0], type, element, why)) {
          result.dropped.push_back("PMTR: " + why);
          continue;
        }
        AppendBE32(items, kind);
        AppendBE32(items, row);
        AppendBE32(items, comp);
        items.insert(items.end(), element.begin(), element.end());
        ++count;
      }
      if (count != 0) {
        PutList(result.part, F("VPMT"), count, items);
      }
    }

    // VSMT: the CCH0 overrides, each to the uniform and warp scales that read the component.
    if (const EffectProperty* smtr = find(F("SMTR"))) {
      std::vector<uint8_t> items;
      uint32_t count = 0;
      for (const EffectValue& item : smtr->value) {
        // The id's four bytes read little-endian: 'CCH0'.
        constexpr uint32_t kCch0 = 0x30484343;
        if (item.word != kCch0 || item.args.size() != 2) {
          result.dropped.push_back("SMTR: " + std::string(item.word == kCch0 ? "an entry" : "a parameter") +
                                   " VSMT cannot take");
          continue;
        }
        const int comp = int(item.args[1].word >> 8 & 0xff);
        std::vector<uint32_t> targets;
        for (const SrcSpec& src : recipe->sources) {
          if (src.row < 0 && src.comp == comp) {
            targets.push_back(uint32_t(src.index));
          }
        }
        for (size_t i = 0; i < slots.size(); ++i) {
          if (slots[i].spec->warpU == comp) {
            targets.push_back(uint32_t(11 + 2 * i));
          }
          if (slots[i].spec->warpV == comp) {
            targets.push_back(uint32_t(11 + 2 * i + 1));
          }
        }
        if (targets.empty()) {
          result.dropped.push_back("SMTR: CCH0 component " + std::to_string(comp) + " is unused by the shader");
          continue;
        }
        std::vector<uint8_t> element;
        std::string why;
        if (!Element(item.args[0], Type::Real, element, why)) {
          result.dropped.push_back("SMTR: " + why);
          continue;
        }
        for (const uint32_t target : targets) {
          AppendBE32(items, target);
          AppendBE32(items, 0);
          items.insert(items.end(), element.begin(), element.end());
          ++count;
        }
      }
      if (count != 0) {
        PutList(result.part, F("VSMT"), count, items);
      }
    }

    // SSZE and ITEN are real elements as they are; VORN is ORNT's byte.
    for (const uint32_t fourcc : {F("SSZE"), F("ITEN")}) {
      const std::string name = EffectFourCCString(fourcc);
      const EffectProperty* property = swoosh && fourcc == F("SSZE") ? nullptr : find(fourcc);
      if (property == nullptr) {
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string why;
      if (property->value.size() == 1 && Element(property->value[0], Type::Real, bytes, why)) {
        AppendBE32(result.part, fourcc);
        result.part.insert(result.part.end(), bytes.begin(), bytes.end());
      } else {
        result.dropped.push_back(name + ": " + (why.empty() ? "not one element" : why));
      }
    }
    if (swoosh) {
      return true;
    }
    uint32_t orient = 0;
    if (const EffectProperty* ornt = find(F("ORNT"))) {
      if (!SmallConst(*ornt, orient) || orient > 2) {
        m_approximated.push_back("VORN: ORNT taken as 0");
        orient = 0;
      }
    }
    AppendBE32(result.part, F("VORN"));
    AppendBE32(result.part, F("CNST"));
    AppendBE32(result.part, orient);
    return true;
  }

  // One generator, swoosh or electric description, as the retail asset `type`.
  // PVAR (value.offset = the u16 id count, then 5 u16 per-kind counts, the u16 data size, the
  // 16-byte ids, then the defaults: real f32, int i32, colour 4 half, vector 3 f32, rotation 4 f32).
  void ReadVariables(const EffectValue& raw) const {
    if (raw.size < 14) {
      return;
    }
    const uint8_t* p = m_data + raw.offset;
    const size_t total = ReadLE16(p);
    size_t counts[5];
    for (int i = 0; i < 5; ++i) {
      counts[i] = ReadLE16(p + 2 + 2 * i);
    }
    const size_t dataSize = ReadLE16(p + 12);
    if (14 + 16 * total + dataSize > raw.size ||
        counts[0] + counts[1] + counts[2] + counts[3] + counts[4] != total) {
      return;
    }
    const uint8_t* id = p + 14;
    const uint8_t* data = id + 16 * total;
    const uint8_t* end = data + dataSize;
    // kind index in table order -> PVRT type (real 0, int 1, colour 2, vector 3, rotation 4)
    static constexpr size_t kSizes[5] = {4, 4, 8, 12, 16};
    for (int kind = 0; kind < 5; ++kind) {
      for (size_t i = 0; i < counts[kind]; ++i) {
        if (data + kSizes[kind] > end) {
          m_vars.clear();
          return;
        }
        PvarEntry var{};
        std::memcpy(var.guid.data(), id, 16);
        id += 16;
        var.type = uint32_t(kind);
        switch (kind) {
        case 0:
          var.def[0] = FloatFromBits(ReadLE32(data));
          break;
        case 1:
          var.def[0] = float(int32_t(ReadLE32(data)));
          break;
        case 2:
          for (int c = 0; c < 4; ++c) {
            var.def[c] = HalfToFloat(ReadLE16(data + 2 * c));
          }
          break;
        default:
          for (int c = 0; c < int(kSizes[kind] / 4); ++c) {
            var.def[c] = FloatFromBits(ReadLE32(data + 4 * c));
          }
          break;
        }
        data += kSizes[kind];
        m_vars.push_back(var);
      }
    }
  }

  ConvertedPart Generator(const EffectNode& node, uint32_t type) const {
    ConvertedPart result;
    result.id = node.id;
    result.type = type;
    result.root = node.root;
    std::vector<uint8_t>& out = result.part;
    AppendBE32(out, HeaderOf(type));
    const auto& retail = PropertiesOf(type);
    const bool part = type == F("PART");
    const bool swoosh = type == F("SWHC");
    // Swooshes: SBDM is Remastered's blend mode, 0 or 1, which retail has as
    // AALP (additive). Taken only where there's no AALP of its own.
    bool additive = false;
    for (const EffectProperty& property : node.properties) {
      additive = additive || property.fourcc == F("AALP");
    }
    // A constant lifetime, for gradients timed in frames. Both count in
    // Remastered's frames, so LTM2 is taken as it is (one above retail's).
    m_lifetime = 0;
    for (const EffectProperty& property : node.properties) {
      if ((property.fourcc == F("LTM2") || property.fourcc == F("LTME")) && property.value.size() == 1 &&
          IsElement(property.value[0], F("CNST")) && property.value[0].args.size() == 1 &&
          property.value[0].args[0].kind == EffectValue::Kind::Word) {
        m_lifetime = int32_t(property.value[0].args[0].word);
      }
    }
    m_vars.clear();
    for (const EffectProperty& property : node.properties) {
      if (property.fourcc == F("PVAR") && property.value.size() == 1 &&
          property.value[0].kind == EffectValue::Kind::Raw) {
        ReadVariables(property.value[0]);
      }
    }
    bool texture = false;
    uint32_t pmdl = 0;          // the PMDL's id, when it is one constant
    bool pmdlVariants = false;  // a PMDV was written (a mesh would cover only one model)
    std::vector<Started> started;  // by the spawn table
    std::set<uint32_t> written;    // retail properties
    const EffectProperty* material = nullptr;
    const EffectProperty* xfmd = nullptr;
    bool faceCamera = false;
    const EffectProperty* mover = nullptr;
    std::vector<uint32_t> portOnly;  // material data, written only with a VMAT
    const bool textured = retail.count(F("TEXR")) != 0;
    // Remastered draws nothing for a generator with no texture, material or
    // model (it only carries spawns and lights); retail would draw its
    // particles as untextured quads (0.1 wide and white without SIZE/COLR).
    // Such a generator is written with a size of 0, and so is one that draws only
    // models (retail drew a white quad round each of the Ice charge's shards).
    const auto has = [&](uint32_t fourcc) {
      return std::any_of(node.properties.begin(), node.properties.end(),
                         [&](const EffectProperty& property) { return property.fourcc == fourcc; });
    };
    // A material whose colour texture (BCLR) is nil binds Remastered's black
    // texture. These are placeholder MATIs on generators that carry spawns and
    // lights (PlasmaCharge's root among them), so their quads are taken as
    // drawing nothing too; model generators keep the black material.
    const auto nilColour = [&](const EffectProperty& property) {
      if (!m_io.materialData) {
        return false;
      }
      for (const EffectValue& value : property.value) {
        if (value.kind != EffectValue::Kind::Guid) {
          continue;
        }
        static constexpr uint8_t kTag[4] = {'B', 'C', 'L', 'R'};
        const std::vector<uint8_t> data = m_io.materialData(value.guid);
        const auto tag = std::search(data.begin(), data.end(), std::begin(kTag), std::end(kTag));
        return tag != data.end() && data.end() - tag >= 20 && std::all_of(tag + 4, tag + 20, [](uint8_t b) { return b == 0; });
      }
      return false;
    };
    const bool placeholder = part && !has(F("TEXR")) && !has(F("PMDL")) &&
                             std::any_of(node.properties.begin(), node.properties.end(), [&](const EffectProperty& property) {
                               return property.fourcc == F("MTIN") && nilColour(property);
                             });
    result.placeholder = placeholder;
    const bool drawsNothing = part && !has(F("TEXR")) && (!has(F("MTIN")) || placeholder) && !has(F("PMDL"));
    result.drawsNothing = drawsNothing;
    const bool modelsOnly = part && !has(F("TEXR")) && !has(F("MTIN")) && has(F("PMDL"));
    for (const EffectProperty& property : node.properties) {
      uint32_t fourcc = property.fourcc;
      if (placeholder && fourcc == F("MTIN")) {
        result.dropped.push_back("MTIN: its colour texture is nil (the generator draws nothing)");
        continue;
      }
      if ((drawsNothing || modelsOnly) && fourcc == F("SIZE")) {
        result.dropped.push_back(drawsNothing ? "SIZE: the generator draws nothing" : "SIZE: the generator draws models only");
        continue;
      }
      // PBDM is Remastered's blend mode, from the table CParticleStaticRenderState
      // reads: 0 alpha (src alpha, 1 - src alpha), 1 premultiplied (1, 1 - src
      // alpha), 2 additive (src alpha, 1) and 3 opaque (1, 0). Retail has only
      // alpha and AAPH (additive), so 2 is AAPH and the rest are alpha.
      if (part && fourcc == F("PBDM")) {
        const EffectValue* mode = property.value.size() == 1 ? &property.value[0] : nullptr;
        if (mode != nullptr && IsElement(*mode, F("CNST")) && mode->args.size() == 1) {
          mode = &mode->args[0];
        }
        if (mode == nullptr || (mode->kind != EffectValue::Kind::Byte && mode->kind != EffectValue::Kind::Word)) {
          result.dropped.push_back("PBDM: not a constant");
        } else if (mode->word == 2) {
          AppendBE32(out, F("AAPH"));
          AppendBE32(out, F("CNST"));
          out.push_back(1);
        } else if (mode->word != 0) {
          m_approximated.push_back("PBDM " + std::to_string(mode->word) + " taken as alpha blending");
        }
        continue;
      }
      if (part && fourcc == F("LTM2")) {
        fourcc = F("LTME");
      } else if (type == F("SWHC") && fourcc == F("SBDM") && !additive) {
        fourcc = F("AALP");
      }
      if (fourcc == F("MTIN") && textured) {
        material = &property;
        continue;
      }
      // Retail reads KSSM, but Remastered's spawn table is laid out differently.
      if (part && fourcc == F("KSSM")) {
        std::vector<uint8_t> bytes;
        std::string why;
        if (!SpawnTable(property, bytes, started, why)) {
          result.dropped.push_back("KSSM: " + why);
          ++result.droppedRetail;
        } else if (!bytes.empty()) {
          AppendBE32(out, fourcc);
          out.insert(out.end(), bytes.begin(), bytes.end());
        }
        continue;
      }
      if (part && fourcc == F("PMRQ")) {
        std::vector<uint8_t> bytes;
        std::string why;
        if (FacingRotation(property.value, bytes)) {
          faceCamera = true;
          written.insert(F("PMRT"));
          AppendBE32(out, F("PMRT"));
          out.insert(out.end(), bytes.begin(), bytes.end());
        } else if (!ModelRotation(property.value, bytes, why)) {
          result.dropped.push_back("PMRQ: " + why);
        } else {
          written.insert(F("PMRT"));
          AppendBE32(out, F("PMRT"));
          out.insert(out.end(), bytes.begin(), bytes.end());
        }
        continue;
      }
      // Port-only: XFMD and its mover are written after the loop, and the
      // material data is written by Material() alongside the VMAT.
      if (part && fourcc == F("XFMD")) {
        xfmd = &property;
        continue;
      }
      if (part && fourcc == F("SMVR")) {
        mover = &property;
        continue;
      }
      if (part && (fourcc == F("TMTR") || fourcc == F("PMTR") || fourcc == F("SMTR") || fourcc == F("SSZE") ||
                   fourcc == F("ITEN") || fourcc == F("SCTR"))) {
        portOnly.push_back(fourcc);
        continue;
      }
      if (swoosh && (fourcc == F("TMTR") || fourcc == F("PMTR") || fourcc == F("SMTR") || fourcc == F("ITEN"))) {
        portOnly.push_back(fourcc);
        continue;
      }
      const auto found = retail.find(fourcc);
      if (found == retail.end()) {
        result.dropped.push_back(EffectFourCCString(property.fourcc) + ": Remastered only");
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string why;
      // A model chosen per particle: the first as PMDL, all of them as PMDV.
      if (part && fourcc == F("PMDL") && property.value.size() == 1 && IsElement(property.value[0], F("SLCT"))) {
        std::vector<uint32_t> ids;
        if (!Variants(property.value, ids, why)) {
          result.dropped.push_back("PMDL: " + why);
          ++result.droppedRetail;
          continue;
        }
        pmdlVariants = true;
        AppendBE32(out, F("PMDL"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, ids[0]);
        AppendBE32(out, F("PMDV"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, uint32_t(ids.size()));
        for (const uint32_t id : ids) {
          AppendBE32(out, F("CNST"));
          AppendBE32(out, id);
        }
        continue;
      }
      if (!Property(fourcc, found->second, property.value, bytes, why)) {
        result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " + why);
        ++result.droppedRetail;
        continue;
      }
      if (fourcc == F("TEXR")) {
        texture = true;
      }
      if (fourcc == F("PMDL") && bytes.size() == 8) {
        pmdl = uint32_t(bytes[4]) << 24 | uint32_t(bytes[5]) << 16 | uint32_t(bytes[6]) << 8 | bytes[7];
      }
      written.insert(fourcc);
      AppendBE32(out, fourcc);
      out.insert(out.end(), bytes.begin(), bytes.end());
    }
    if (drawsNothing || modelsOnly) {
      AppendBE32(out, F("SIZE"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(0.0f));
    }
    // A material instance draws with its texture where there is no TEXR.
    if (material != nullptr && !texture) {
      const EffectValue* guid = nullptr;
      for (const EffectValue& value : material->value) {
        if (value.kind == EffectValue::Kind::Guid) {
          guid = &value;
        }
      }
      const uint32_t id = guid != nullptr && m_io.materialTexture ? m_io.materialTexture(guid->guid) : 0;
      if (id != 0) {
        AppendBE32(out, F("TEXR"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, id);
      } else {
        result.dropped.push_back("MTIN: no texture for its material");
        ++result.droppedRetail;
      }
    } else if (material != nullptr) {
      result.dropped.push_back("MTIN: the effect has a TEXR");
    }
    const bool vmat = (part || swoosh) && material != nullptr && Material(*material, node, result, swoosh);
    if (!vmat) {
      for (const uint32_t fourcc : portOnly) {
        result.dropped.push_back(EffectFourCCString(fourcc) + ": no VMAT");
      }
    }
    if (xfmd != nullptr) {
      Transform(*xfmd, mover, out);
    } else if (mover != nullptr) {
      result.dropped.push_back("SMVR: no XFMD");
    }
    if (vmat && pmdl != 0 && !pmdlVariants && m_io.modelMesh) {
      // VMSH: the converted PMDL as one mesh, so the model particle draws through the VMAT.
      const std::vector<uint8_t> mesh = m_io.modelMesh(pmdl);
      if (!mesh.empty()) {
        AppendBE32(out, F("VMSH"));
        AppendBE32(out, F("CNST"));
        AppendBE32(out, uint32_t(mesh.size()));
        out.insert(out.end(), mesh.begin(), mesh.end());
      }
    }
    // The first swoosh and electric child the spawn table starts, where the
    // generator has none of its own.
    for (const auto& [type, child, frame] : {std::tuple(F("SWHC"), F("SSWH"), F("SSSD")),
                                              std::tuple(F("ELSC"), F("SELC"), F("SESD"))}) {
      size_t count = 0;
      for (const Started& each : started) {
        if (each.type != type) {
          continue;
        }
        if (count++ == 0 && written.count(child) == 0) {
          AppendBE32(out, child);
          AppendBE32(out, F("CNST"));
          AppendBE32(out, each.id);
          if (written.count(frame) == 0) {
            AppendBE32(out, frame);
            AppendBE32(out, F("CNST"));
            AppendBE32(out, each.frame);
          } else if (each.frame != 0) {
            m_approximated.push_back("KSSM: " + EffectFourCCString(child) + " frame kept from " +
                                     EffectFourCCString(frame));
          }
        } else {
          result.dropped.push_back("KSSM: a spawned " + EffectFourCCString(type) + " beyond retail's one");
          ++result.droppedRetail;
        }
      }
    }
    if (faceCamera) {
      // Port-only: the model particles face the camera (xPortFaceCamera; see FacingRotation).
      AppendBE32(out, F("PFCM"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
    }
    if ((part || swoosh) && !m_vars.empty()) {
      // Port-only: the effect's PVAR table (names, kinds, defaults) for VARF/VARI/VARV/VARC.
      AppendBE32(out, F("PVRT"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, uint32_t(m_vars.size()));
      for (const PvarEntry& var : m_vars) {
        out.insert(out.end(), var.guid.begin(), var.guid.end());
        AppendBE32(out, var.type);
        for (float f : var.def) {
          AppendBE32(out, FloatBits(f));
        }
      }
    }
    if (part || swoosh) {
      // Port-only: nested IRND elements are evaluated once per particle and element, not at frame 0
      // only (xPortIrnd). Marks every converted PART; retail's own PARTs do not have it.
      // On a swoosh it marks Remastered's timing: a point's colour runs over the live points
      // (KEWS), not over LENG.
      AppendBE32(out, F("PIRN"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, 1);
    }
    AppendBE32(out, F("_END"));
    result.approximated = std::move(m_approximated);
    m_approximated.clear();
    return result;
  }

  // The top node is the effect's own PART whatever its root flag says
  // (Remastered sets it only on effects with embedded children).
  void Collect(const EffectNode& node, bool top, std::vector<ConvertedPart>& out) const {
    if (const uint32_t type = EffectRetailType(node.form); type != 0) {
      out.push_back(Generator(node, type));
      out.back().root = top;
    }
    for (const EffectNode& child : node.children) {
      Collect(child, false, out);
    }
  }

private:
  const uint8_t* m_data;
  const EffectConvertIO& m_io;
  mutable std::vector<PvarEntry> m_vars;            // the PVAR of the generator being written
  mutable std::vector<std::string> m_approximated;  // for the generator being written
  mutable int32_t m_lifetime = 0;                   // its constant lifetime in frames, or 0
};


using port::ReadBE32;

// Walks a retail PART the way CParticleDataFactory reads it.
class RetailReader {
public:
  RetailReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

  size_t At() const { return m_at; }

  bool Word(uint32_t& out) {
    if (m_at + 4 > m_size) {
      return false;
    }
    out = ReadBE32(m_data + m_at);
    m_at += 4;
    return true;
  }

  bool Skip(size_t bytes) {
    if (m_at + bytes > m_size) {
      return false;
    }
    m_at += bytes;
    return true;
  }

  bool Bool() {
    uint32_t fourcc;
    return Word(fourcc) && Skip(1);
  }

  // A keyframe block of `keySize`-byte keys.
  bool Keyframes(size_t keySize) {
    uint32_t count;
    return Skip(18) && Word(count) && Skip(size_t(count) * keySize);
  }

  bool Element(Type type) {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (type == Type::Emitter && fourcc == F("SETR")) {
      uint32_t name;
      return Word(name) && Element(Type::Vector) && Word(name) && Element(Type::Vector);
    }
    // Port-only VARF/VARI/VARV/VARC: a PVRT index.
    if ((fourcc == F("VARF") && type == Type::Real) || (fourcc == F("VARI") && type == Type::Int) ||
        (fourcc == F("VARV") && type == Type::Vector) || (fourcc == F("VARC") && type == Type::Color)) {
      return Skip(4);
    }
    const Signatures& elements = ElementsOf(type);
    const auto found = elements.find(fourcc);
    if (found == elements.end()) {
      m_error = "no " + EffectFourCCString(fourcc) + " element of that type";
      return false;
    }
    for (const char* c = found->second; *c != 0; ++c) {
      bool ok;
      switch (*c) {
      case '#':
        ok = Skip(4);
        break;
      case 'k': {
        const size_t sizes[] = {4, 4, 12, 12, 16, 0, 0, 0, 0};
        ok = Keyframes(sizes[size_t(type)]);
        break;
      }
      case 'B':
        ok = Bool();
        break;
      default:
        ok = Element(TypeOfLetter(*c));
        break;
      }
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  bool Texture() {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (fourcc == F("NONE")) {
      return true;
    }
    // PATL: CNST id, then cols, rows, count, mode and flipX as CNST ints.
    if (fourcc == F("PATL")) {
      uint32_t sub;
      if (!Word(sub) || sub != F("CNST") || !Skip(4)) {
        return false;
      }
      for (int i = 0; i < 5; ++i) {
        if (!Word(sub) || sub != F("CNST") || !Skip(4)) {
          return false;
        }
      }
      return true;
    }
    uint32_t sub;
    if ((fourcc != F("CNST") && fourcc != F("ATEX")) || !Word(sub) || (sub != F("NONE") && !Skip(4))) {
      return false;
    }
    if (fourcc == F("CNST")) {
      return true;
    }
    for (int i = 0; i < 5; ++i) {
      if (!Element(Type::Int)) {
        return false;
      }
    }
    return Bool();
  }

  bool Asset() {
    uint32_t fourcc;
    return Word(fourcc) && (fourcc == F("NONE") || Skip(4));
  }

  // PMDV: CNST n, then n CNST ids.
  bool ModelVariants() {
    uint32_t fourcc, count;
    if (!Word(fourcc) || fourcc != F("CNST") || !Word(count) || count > 64) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (!Word(fourcc) || fourcc != F("CNST") || !Skip(4)) {
        return false;
      }
    }
    return true;
  }

  // KSSM: NONE, or CNST, four ints and a frame table (frame, count, 16 bytes each).
  bool SpawnTable() {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (fourcc != F("CNST")) {
      return true;
    }
    uint32_t frames;
    if (!Skip(16) || !Word(frames)) {
      return false;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      uint32_t count;
      if (!Skip(4) || !Word(count) || !Skip(size_t(count) * 16)) {
        return false;
      }
    }
    return true;
  }

  // The port-only properties (DESIGN.md): `CNST n` and then n records.
  bool CountedList(uint32_t& count) {
    uint32_t fourcc;
    return Word(fourcc) && fourcc == F("CNST") && Word(count);
  }

  // VMAT: CNST, the blob's length, the blob.
  bool Material() {
    uint32_t bytes;
    return CountedList(bytes) && Skip(bytes);
  }

  bool TextureTransforms() {
    uint32_t count;
    if (!CountedList(count) || count > 3) {
      return false;
    }
    for (uint32_t i = 0; i < count * 6; ++i) {
      if (!Element(Type::Real)) {
        return false;
      }
    }
    return true;
  }

  // VPMT: {kind, row, comp, element}; VSMT: {target, kind, element} (reals).
  bool PerParticle(bool shader) {
    uint32_t count;
    if (!CountedList(count)) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t kind = 0;
      if (shader ? !Skip(4) || !Word(kind) : !Word(kind) || !Skip(8)) {
        return false;
      }
      static constexpr Type kTypes[4] = {Type::Real, Type::Vector, Type::Int, Type::Color};
      if (kind > 3 || !Element(shader ? Type::Real : kTypes[kind])) {
        return false;
      }
    }
    return true;
  }

  // A port-only `CNST <word>` property (VORN, XFMD, PIRN, PFCM).
  bool PortWord() {
    uint32_t fourcc;
    return Word(fourcc) && fourcc == F("CNST") && Skip(4);
  }

  // The port-only PVRT property: CNST, a count, then 36 bytes a variable.
  bool PortVariables() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 4096 && Skip(size_t(count) * 36);
  }

  const std::string& Error() const { return m_error; }

private:
  const uint8_t* m_data;
  size_t m_size;
  size_t m_at = 0;
  std::string m_error;
};

}  // namespace

uint32_t EffectRetailType(uint32_t form) {
  if (form == F("GPSM")) {
    return F("PART");
  }
  if (form == F("SWSH")) {
    return F("SWHC");
  }
  if (form == F("ELC2") || form == F("ELSM")) {
    return F("ELSC");
  }
  return 0;
}

bool SplitRetailPart(const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out, std::string& error) {
  return SplitRetailEffect(F("PART"), data, size, out, error);
}

bool SplitRetailEffect(uint32_t type, const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out,
                       std::string& error) {
  out.clear();
  RetailReader reader(data, size);
  uint32_t fourcc;
  if (!reader.Word(fourcc) || fourcc != HeaderOf(type)) {
    error = "not a " + EffectFourCCString(HeaderOf(type));
    return false;
  }
  const auto& retail = PropertiesOf(type);
  const bool part = type == F("PART");
  const bool vfx = part || type == F("SWHC"); // swooshes take the VFX material too
  for (;;) {
    const size_t start = reader.At();
    if (!reader.Word(fourcc)) {
      error = "no _END";
      return false;
    }
    if (fourcc == F("_END")) {
      return true;
    }
    bool ok;
    if (part && fourcc == F("KSSM")) {
      ok = reader.SpawnTable();
    } else if (part && fourcc == F("PMDV")) {
      ok = reader.ModelVariants();
    } else if ((vfx && fourcc == F("VMAT")) || (part && fourcc == F("VMSH"))) {
      ok = reader.Material();
    } else if (vfx && fourcc == F("VTMT")) {
      ok = reader.TextureTransforms();
    } else if (vfx && (fourcc == F("VPMT") || fourcc == F("VSMT"))) {
      ok = reader.PerParticle(fourcc == F("VSMT"));
    } else if ((part && fourcc == F("SSZE")) || (vfx && fourcc == F("ITEN"))) {
      ok = reader.Element(Type::Real);
    } else if ((part && (fourcc == F("VORN") || fourcc == F("XFMD") || fourcc == F("PFCM"))) ||
               (vfx && fourcc == F("PIRN"))) {
      ok = reader.PortWord();
    } else if (vfx && fourcc == F("PVRT")) {
      ok = reader.PortVariables();
    } else if (const auto found = retail.find(fourcc); found == retail.end()) {
      error = "property " + EffectFourCCString(fourcc) + " retail does not read";
      return false;
    } else {
      switch (found->second.type) {
      case Type::Bool:
        ok = reader.Bool();
        break;
      case Type::Texture:
        ok = reader.Texture();
        break;
      case Type::Asset:
        ok = reader.Asset();
        break;
      default:
        ok = reader.Element(found->second.type);
        break;
      }
    }
    if (!ok) {
      error = EffectFourCCString(fourcc) + " does not read" + (reader.Error().empty() ? "" : ": " + reader.Error());
      return false;
    }
    out.push_back({fourcc, std::vector<uint8_t>(data + start + 4, data + reader.At())});
  }
}

std::optional<uint32_t> EffectRetailId(const EffectGuid& id) {
  // 10000000-0000-f000-f000-0000XXXXXXXX in pak order is stored here with its
  // first three groups byte-swapped.
  static const uint8_t kPrefix[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  if (std::memcmp(id.data(), kPrefix, sizeof(kPrefix)) != 0) {
    return std::nullopt;
  }
  return uint32_t(id[12]) << 24 | uint32_t(id[13]) << 16 | uint32_t(id[14]) << 8 | uint32_t(id[15]);
}

std::vector<ConvertedPart> ConvertEffect(const EffectNode& effect, const uint8_t* data, const EffectConvertIO& io) {
  std::vector<ConvertedPart> out;
  Writer(data, io).Collect(effect, true, out);
  return out;
}

}  // namespace PortRemastered
