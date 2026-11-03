#include "port_remastered_effect_convert.h"
#include "port_bytes.h"
#include "Kyoto/Particles/CPortVfxConstants.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace PortRemastered {
namespace {

constexpr uint32_t F(const char (&text)[5]) { return EffectFourCC(text); }

// The per-particle KPIN latches a generator or swoosh can carry (SPortKeep::kMax).
constexpr uint32_t kKeepSlots = 8;
// PITP's mode word with this bit: an ITPE that was absent (Remastered's default 1), which
// MP_ITPE_DEFAULT=0 turns off at load.
constexpr int kSpreadDefault = 0x100;

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
    const uint32_t fourcc = port::FourCC(c);
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

// Port-only: the ELC2 (CParticleElectric2) fields retail's ELSM has no property for. An ELC2 is written
// as an ELSC stream that starts with the PEL2 marker and carries these (ELC2_RE.md).
const PropertyTable& Elc2Properties() {
  static const PropertyTable table = [] {
    PropertyTable t;
    AddProperties(t, "SIDE LWQD NFOM TSPN BDEP BNDI", Type::Int);
    AddProperties(t, "RADS LRAD RRAD SLVF BLNC BCFF BLEN BANG BIDF BLDF AMDF", Type::Real);
    AddProperties(t, "NCLR", Type::Color);
    // IEXP: CreateELC2 0xb716f4 reads a raw byte into desc+0x26a (ctor default 1), which gates the
    // vertex writer's inverse exposure. ELC2's DVVN only sets desc+0x6a, which nothing known reads.
    AddProperties(t, "LLRD FPLB IEXP", Type::Bool);
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
      // Port-only CIEPortParticleVar: Remastered's PAP0..3, the per-particle ints IAV0..3.
      {F("PAP0"), ""},   {F("PAP1"), ""},   {F("PAP2"), ""},   {F("PAP3"), ""},
      // Port-only (CParticleDataFactory under TARGET_PC): FIAT start, count, divisor, loop.
      {F("FIAT"), "iiiB"},
      // Port-only PRAN: Remastered's int RAND (CIEPortRandom), min and max.
      {F("PRAN"), "ii"},
      // Port-only ABS_ (CIEPortAbs): Remastered's CIEAbsoluteValue.
      {F("ABS_"), "i"},
  };
  static const Signatures realElements = {
      {F("CNST"), "#"},   {F("NONE"), ""},    {F("KEYE"), "k"},    {F("KEYP"), "k"},   {F("SCAL"), "r"},
      {F("SINE"), "rrr"}, {F("ADD_"), "rr"},  {F("MULT"), "rr"},   {F("DOTP"), "vv"},  {F("RAND"), "rr"},
      {F("IRND"), "rr"},  {F("CHAN"), "rri"}, {F("CLMP"), "rrr"},  {F("PULS"), "iirr"}, {F("RLPT"), "r"},
      {F("LFTW"), "rr"},  {F("PRLW"), ""},    {F("PSLL"), ""},     {F("PAP1"), ""},    {F("PAP2"), ""},
      {F("PAP3"), ""},    {F("PAP4"), ""},    {F("PAP5"), ""},     {F("PAP6"), ""},    {F("PAP7"), ""},
      {F("PAP8"), ""},    {F("PAP9"), ""},    {F("VXTR"), "v"},   {F("VYTR"), "v"},    {F("VZTR"), "v"},   {F("VMAG"), "v"},
      {F("ISWT"), "rr"},  {F("CLTN"), "rrrr"}, {F("CEQL"), "rrrr"}, {F("CRNG"), "rrrrr"}, {F("CEXT"), "i"},
      {F("ITRL"), "ir"},  {F("PSSZ"), ""},    {F("SUB_"), "rr"},  {F("GTCR"), "c"},    {F("GTCG"), "c"},   {F("GTCB"), "c"},
      {F("GTCA"), "c"},
      {F("DFCS"), "rr"},  {F("DFCP"), "rr"},
      // Port-only: a converted KEWS, its KEYP read at the swoosh node key.
      {F("KEWS"), "r"},
      // Port-only: a converted KESP, its KEYP read at the ELC2 node key (time slot 2).
      {F("KESP"), "r"},
      // Port-only (CParticleDataFactory under TARGET_PC).
      {F("DIVD"), "rr"},
      // Port-only CREPortTicksToSeconds: Remastered's T2S_, an int element (ticks) as seconds.
      {F("T2S_"), "i"},
      // Port-only: Remastered's CREAbsoluteValue, CRECosineWave (frequency, amplitude, phase)
      // and CRERandomSign. Its CREInitialRandomSign is the port-only PIRS (see Element).
      {F("ABS_"), "r"},   {F("COSE"), "rrr"}, {F("RSGN"), "r"},
  };
  static const Signatures vectorElements = {
      {F("NONE"), ""},      {F("CNST"), "rrr"}, {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("ANGC"), "rrrrr"},
      {F("CONE"), "vr"},    {F("CIRC"), "vvrrr"}, {F("CCLU"), "vvir"}, {F("ADD_"), "vv"}, {F("MULT"), "vv"},
      {F("CHAN"), "vvi"},   {F("PULS"), "iivv"}, {F("RTOV"), "r"},  {F("PLOC"), ""},    {F("PLCO"), ""},
      {F("PVEL"), ""},      {F("PSOF"), ""},    {F("PSOU"), ""},   {F("PSOR"), ""},    {F("PSTR"), ""},
      {F("SUB_"), "vv"},    {F("CTVC"), "c"},     {F("PQAD"), "vv"},
      // Port-only (CParticleDataFactory under TARGET_PC): NORM, and the generator's system
      // vectors (camera in world and in system space, external translation, scale and rotation as
      // Euler degrees, the camera rotation as Euler degrees), PRZY and PRYZ.
      {F("NORM"), "v"},     {F("CMPS"), ""},      {F("CPSS"), ""},      {F("EXTT"), ""},
      {F("EXTS"), ""},      {F("EXTR"), ""},      {F("CMRT"), ""},      {F("PRZY"), "vvv"},
      {F("PRYZ"), "vvv"},
      // Port-only CVEPortBasis: Remastered's particle space to retail's and back.
      {F("BSIS"), "v"},     {F("BSSC"), "v"},
      // Port-only CVEPortAngleCone: Remastered's ANCR with its REUL as three angles.
      {F("ANCR"), "rrrrrr"},
      // Port-only: Remastered's ANCM with its REUL as three angles, MPAC (x and y centre, x and
      // y range, magnitude) and MPCN (direction, radius).
      {F("PANM"), "rrrrrr"}, {F("PMAC"), "rrrrr"}, {F("PMCN"), "vr"},
      // Port-only PANV: Remastered's ANCV, its REUL as three angles, then polar count and
      // variance, azimuth count and variance, and the shuffler size.
      {F("PANV"), "rrrrrrirird"},
      // Port-only CVEPortAbs: Remastered's CVEAbsoluteValue, in whatever space the child is in.
      {F("ABS_"), "v"},
      // Port-only PEMV: Remastered's EMRV, the mode word (0 external translation, 1 local).
      {F("PEMV"), "#"},
      // Port-only PLWV: Remastered's LINV (start, end, count, variance, shuffler size).
      {F("PLWV"), "vvird"},
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
      // Port-only CEEPortAngleSphere: Remastered's ASPR with its REUL as three angles.
      {F("ASPR"), "vrrrrrrr"},
      // Port-only PASV: Remastered's ASPV (origin, REUL as three angles, polar range 1 and 2, polar
      // count and variance, azimuth range, count and variance, radius, speed, shuffler size).
      {F("PASV"), "vrrrrrirrirrrd"},
      // PLNE (position, euler, velocity, x and z half-extent, spread) and the port-only PLNV
      // (position, euler, velocity y, x and z half-extent, spread, x count and variance, z count
      // and variance, then the shuffle byte): Remastered's plane emitters, all in its axes.
      {F("PLNE"), "vvvrrr"},
      {F("PLNV"), "vvrrrrirird"},
      // The port-only PSPV (position, radius, speed, polar count and variance, azimuth count and
      // variance, shuffle byte) and PELP (centre, size, speed, surface-only byte).
      {F("PSPV"), "vrrirird"},
      {F("PELP"), "vvrd"},
      // Port-only CODE: a null emitter, the generator spawns only from code.
      {F("CODE"), ""},
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

// The port-only elements only Writer::Element's own cases write (a Remastered element of the same
// fourcc is never passed through as one), in ElementsOf's letters plus d a byte and w a raw word
// (an index, slot or mode). RetailReader walks these and ElementsOf's alike; the few whose payload
// a signature cannot say (a count, a checked slot, a choice of rotation) it reads itself.
const Signatures& PortElementsOf(Type type) {
  static const Signatures intElements = {
      // VARI: a PVRT index. PSYI: a PSYS slot. PLPT: the lifetime offset, then the percent.
      {F("VARI"), "w"}, {F("PSYI"), "w"}, {F("PLPT"), "wi"},
  };
  static const Signatures realElements = {
      // VARF: a PVRT index. PSYR: a PSYS slot. PSDT: the shutdown countdown's remaining over its
      // total. PMXV: the index, then the fallback. PSLW: the two reals the system tween blends.
      // PDML: a component of the dominant light direction (DOML). PVSQ: the occlusion visibility.
      // PGTC: Remastered's total created particles (a real GTCP).
      {F("VARF"), "w"}, {F("PSYR"), "w"}, {F("PSDT"), ""}, {F("PMXV"), "ir"}, {F("PSLW"), "rr"},
      {F("PDML"), "w"}, {F("PVSQ"), ""}, {F("PGTC"), ""},
  };
  static const Signatures vectorElements = {
      // VARV: a PVRT index. PCRC: centre, axis, start degrees, count, variance, radius, then the
      // shuffle byte. RNDV: the magnitude. PCRO: P, the frequency, the octave count, the gain,
      // then the amplitude A. PNCV: the normalised compensated velocity, no payload. PSYV: a PSYS
      // slot. PCRS: the raw slice, P, F, A.
      {F("VARV"), "w"}, {F("PCRC"), "vvrirrd"}, {F("RNDV"), "r"}, {F("PCRO"), "vrirv"}, {F("PNCV"), ""},
      {F("PSYV"), "w"}, {F("PCRS"), "wvrv"},
  };
  static const Signatures modVectorElements = {
      // PCRM: the raw mode, the V vector, then t.
      {F("PCRM"), "wvr"},
  };
  static const Signatures colorElements = {
      // VARC: a PVRT index.
      {F("VARC"), "w"},
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
  default:
    return none;
  }
}

// The arguments of the element `fourcc` of `type` (ElementsOf, then PortElementsOf), or null.
const char* ShapeOf(uint32_t fourcc, Type type) {
  for (const Signatures* elements : {&ElementsOf(type), &PortElementsOf(type)}) {
    const auto found = elements->find(fourcc);
    if (found != elements->end()) {
      return found->second;
    }
  }
  return nullptr;
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

// The type of the element at `i` in the shape (ShapeOf) of the element `fourcc` of `type`.
Type ArgType(uint32_t fourcc, Type type, size_t i) {
  return TypeOfLetter(ShapeOf(fourcc, type)[i]);
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

// Whether an element or any element under it is `fourcc`.
bool ContainsElement(const EffectValue& value, uint32_t fourcc) {
  if (IsElement(value, fourcc)) {
    return true;
  }
  for (const EffectValue& arg : value.args) {
    if (ContainsElement(arg, fourcc)) {
      return true;
    }
  }
  return false;
}

// SPAF/SPAI/SPAC/SPAV(#n, fallback): the parent system's additional variable n
// (GetRealElement 0x260de4: CElementParentAddlVar over SAddVarAddress(ctx 0, n),
// AddParentAddlVarRef). DPV*/TPV*(guid, fallback): the direct or topmost parent's
// PVAR variable (AddDirectParentExtVarRef 0x25584c, AddTopmostParentExtVarRef
// 0x255bf4). Each of the type it is read as.
bool IsParentRead(uint32_t fourcc, Type type) {
  switch (type) {
  case Type::Real:
    return fourcc == F("SPAF") || fourcc == F("DPVF") || fourcc == F("TPVF");
  case Type::Int:
    return fourcc == F("SPAI") || fourcc == F("DPVI") || fourcc == F("TPVI");
  case Type::Color:
    return fourcc == F("SPAC") || fourcc == F("DPVC") || fourcc == F("TPVC");
  case Type::Vector:
    return fourcc == F("SPAV") || fourcc == F("DPVV") || fourcc == F("TPVV");
  default:
    return false;
  }
}

// Whether every leaf of an element is literal data, so Remastered might fold it to a constant
// (over-reports: a RAND of constants counts too). A keyframe never is: CIEKeyframeEmitter and
// CIEKeyframeInput::GetConstOverride (0x2aa748, 0x2aa764) answer false. Nor is an int parent
// read: GetIntElement 0x26a910 makes SPAI/DPVI/TPVI a CElementParentAddlVar, whose fallback is
// read as a constant (GetIntElementAsConst), not as an input, and whose IsNodeConst is
// IElement's (0x250d38, false), so CElementDep::Finish 0x24f9a4 leaves it non-constant.
bool ConstantLeaves(const EffectValue& value) {
  if (value.kind != EffectValue::Kind::Element) {
    return true;
  }
  if (value.args.empty() || (value.fourcc >> 8) == (F("KEYE") >> 8) || IsParentRead(value.fourcc, Type::Int)) {
    return false;
  }
  for (const EffectValue& arg : value.args) {
    if (!ConstantLeaves(arg)) {
      return false;
    }
  }
  return true;
}

// PSA0..PSA9: the system's own additional variable n (real, int or vector by context), or -1.
int SystemRealRead(uint32_t fourcc) {
  return fourcc >= F("PSA0") && fourcc <= F("PSA9") ? int(fourcc - F("PSA0")) : -1;
}

// SVR0..9, SVI0..9, SVC0..9, SVV0..9: the system's additional variables, as
// (EPortVarType, n), or type ~0u.
std::pair<uint32_t, uint32_t> SystemVarWrite(uint32_t fourcc) {
  const uint32_t prefix = fourcc & 0xffffff00u;
  const uint32_t digit = fourcc & 0xffu;
  if (digit < '0' || digit > '9') {
    return {~0u, 0};
  }
  const uint32_t type = prefix == (F("SVR0") & 0xffffff00u)   ? 0
                        : prefix == (F("SVI0") & 0xffffff00u) ? 1
                        : prefix == (F("SVC0") & 0xffffff00u) ? 2
                        : prefix == (F("SVV0") & 0xffffff00u) ? 3
                                                               : ~0u;
  return {type, digit - '0'};
}

// The PSAn the value reads anywhere, as a bit mask.
uint32_t SystemReadsOf(const EffectValue& value) {
  uint32_t mask = 0;
  if (value.kind == EffectValue::Kind::Element) {
    const int n = SystemRealRead(value.fourcc);
    if (n >= 0 && value.args.empty()) {
      mask |= 1u << n;
    }
  }
  for (const EffectValue& arg : value.args) {
    mask |= SystemReadsOf(arg);
  }
  return mask;
}

// Remastered's IsConstantAfterStart (0x250b18) of a variable's tree, `(flags & 3) != 0`, with the
// flags CElementDep::Finish (0x24f9a4) fills from the node's own virtuals (IsNodeConst slot
// +0xe8, IsNodeInitialOnly +0xf0, GetConstOverride +0x148) and its children:
//  - a node with no registered children keeps its own bits: CNST (CIEConstant, CREConstant,
//    CVEConstant ... override IsNodeConst) is bit 0; the "...Constant" specialisations the
//    factory builds for constant arguments (RAND of two CNST is CRERandomConstant 0x29598c, a
//    leaf) have neither;
//  - otherwise bit 0 is the AND of the children's bit 0 (the node's own is dropped), then
//    GetConstOverride (CRERandom 0x299dbc, CRESineWave, CREPulse, CRETimeChain, CREKeyframe*,
//    the SPAF/TPVF/DPVF reads ... all return false) may overwrite it;
//  - bit 1 is the node's own IsNodeInitialOnly (IRND: CIEInitialRandom(Constant) 0x2aa320 /
//    0x2aa360, KPIN: CREKeepInitial 0x299d3c), else a child's bit 1 with every child
//    constant-after-start (ResolveInitialOnlyConflict 0x250994).
// Only the shapes decoded are claimed constant: CNST, IRND and KPIN, and the plain composites
// ADD_, SUB_ and DIVD over reals (GetRealElement 0x260de4 builds CREAdd/CRESubtract/CREDivide
// over two registered children; none overrides GetConstOverride or IsNodeInitialOnly). Anything
// else (RAND, SPAF, TPVF, MPXV, SINE, PULS, CHAN, a variable read ...) is 0, i.e. re-evaluated
// every frame, which is only ever the safe side.
uint32_t ConstantFlags(const EffectValue& value, Type type) {
  if (value.kind != EffectValue::Kind::Element) {
    return 0;
  }
  if (value.fourcc == F("CNST")) {
    return 1;
  }
  if (value.fourcc == F("IRND") && type != Type::Vector && type != Type::Color) {
    return 2;
  }
  // KPIN: CREKeepInitial 0x299d3c, CVEKeepInitial 0x2a4364, CCEKeepInitial 0x2c92bc.
  if (value.fourcc == F("KPIN") && (type == Type::Real || type == Type::Vector || type == Type::Color)) {
    return 2;
  }
  const bool plain = type == Type::Real && value.args.size() == 2 &&
                     (value.fourcc == F("ADD_") || value.fourcc == F("SUB_") || value.fourcc == F("DIVD"));
  if (!plain) {
    return 0;
  }
  uint32_t all = 1, any2 = 0;
  bool every = true;
  for (const EffectValue& arg : value.args) {
    const uint32_t flags = ConstantFlags(arg, type);
    all &= flags;
    any2 |= flags & 2;
    every = every && (flags & 3) != 0;
  }
  return all | (any2 != 0 && every ? 2u : 0u);
}

// Whether IElement::IsConstant holds: a CNST of words or of constants.
bool IsConstantValue(const EffectValue& value) {
  if (value.kind == EffectValue::Kind::Word || value.kind == EffectValue::Kind::Byte) {
    return true;
  }
  if (!IsElement(value, F("CNST")) || value.args.empty()) {
    return false;
  }
  return std::all_of(value.args.begin(), value.args.end(), IsConstantValue);
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

// A swoosh's SYSR (desc+0x78, CreateSWSH 0x2dd224) makes any XFMD but 5 transform mode 1
// (SetupTransformMode 0x2887d8; 5 deletes it). Mode 1 binds the system vars to the system's own
// elements instead of the external transform: a8 translation, ac SYSR, b0 SYSS, b8 the birth
// position and bc the birth orientation, which default to zero, identity, one, zero and identity.
// So the setters move nothing; EXTR and the getters still read what they set. A zero REUL under
// XFMD 2 is the port-only XFMD 1 (CParticleSwoosh xPortXfmdSystem) unless an EXTT or EXTS reads
// the external translation or scale, an INTR other than a zero REUL is a rotation the points no
// longer start from, or a RAZY's EMRV reads the frozen birth position. 1 writes it, 5 is a no-op,
// and 0 is a drop with `why`.
// A property that is one REUL of three constant zeros: the identity rotation.
bool ZeroReul(const EffectProperty& property) {
  if (property.value.size() != 1 || !IsElement(property.value[0], F("REUL")) || property.value[0].args.size() != 4) {
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    uint32_t word = 0;
    if (!ConstWord(property.value[0].args[i], word) || (word & 0x7fffffffu) != 0) {
      return false;
    }
  }
  return true;
}

int SwooshSystemMode(const EffectNode& node, std::string& why) {
  uint32_t mode = 2;
  bool zeroSysr = false;
  bool otherIntr = false;
  bool razy = false;
  bool external = false;
  for (const EffectProperty& property : node.properties) {
    if (property.fourcc == F("XFMD") && (property.value.size() != 1 || !ConstWord(property.value[0], mode))) {
      mode = UINT32_MAX;
    }
    zeroSysr = zeroSysr || (property.fourcc == F("SYSR") && ZeroReul(property));
    otherIntr = otherIntr || (property.fourcc == F("INTR") && !ZeroReul(property));
    for (const EffectValue& value : property.value) {
      razy = razy || ContainsElement(value, F("RAZY"));
      external = external || ContainsElement(value, F("EXTT")) || ContainsElement(value, F("EXTS"));
    }
  }
  if (mode == 5) {
    return 5;
  }
  why = mode != 2       ? "under XFMD " + std::to_string(int32_t(mode))
        : !zeroSysr     ? "not a zero REUL"
        : external      ? "with an EXTT or EXTS"
        : otherIntr     ? "with an INTR other than a zero REUL"
        : razy          ? "with a RAZY"
                        : "";
  return why.empty() ? 1 : 0;
}

// A generator's SYST, SYSR, SYSS, TEMT or TEMR (CreateGPSM) makes any XFMD but 5 transform mode 1
// as a swoosh's SYSR does (SetupTransformMode 0x2887d8, which also deletes SMVR and EMVR; 5
// deletes them instead): the render's a8 is SYST, live, ac SYSR and b0 SYSS, and b8 TEMT and bc
// TEMR are baked into each particle at spawn (CreateNewParticles 0x24a5e4). The port-only XFMD 1
// (CElementGen kPortXfmdSystem) carries SYST as PSYT and TEMT as PTMT, evaluated every tick, with
// the rotations and scale at their defaults. So a SYSR or TEMR other than a zero REUL, a SYSS, an
// IRND or KPIN (evaluated once per system) or a RAZY in SYST or TEMT, and an INTT, INTR, INTS,
// POFS, EMVR, SMVR or ISVF beside them (mode 1 with those is not decoded) are a drop with `why`
// (0). -1 without any of the five, 5 under XFMD 5 and 1 for XFMD 1.
int PartSystemMode(const EffectNode& node, std::string& why) {
  uint32_t mode = 2;
  bool any = false;
  why.clear();
  for (const EffectProperty& property : node.properties) {
    const uint32_t fourcc = property.fourcc;
    if (fourcc == F("XFMD") && (property.value.size() != 1 || !ConstWord(property.value[0], mode))) {
      mode = UINT32_MAX;
    }
    if (fourcc != F("SYST") && fourcc != F("SYSR") && fourcc != F("SYSS") && fourcc != F("TEMT") &&
        fourcc != F("TEMR")) {
      continue;
    }
    any = true;
    if (!why.empty()) {
      continue;
    }
    if ((fourcc == F("SYSR") || fourcc == F("TEMR")) && !ZeroReul(property)) {
      why = EffectFourCCString(fourcc) + " not a zero REUL";
    } else if (fourcc == F("SYSS")) {
      why = "a SYSS";
    } else if ((fourcc == F("SYST") || fourcc == F("TEMT")) && property.value.size() != 1) {
      why = EffectFourCCString(fourcc) + " not one element";
    } else if (fourcc == F("SYST") || fourcc == F("TEMT")) {
      for (const uint32_t init : {F("IRND"), F("KPIN"), F("RAZY")}) {
        if (why.empty() && ContainsElement(property.value[0], init)) {
          why = EffectFourCCString(fourcc) + " with " + EffectFourCCString(init);
        }
      }
    }
  }
  if (!any) {
    return -1;
  }
  if (mode == 5) {
    why.clear();
    return 5;
  }
  if (mode == UINT32_MAX) {
    why = "XFMD not a constant";
    return 0;
  }
  for (const EffectProperty& property : node.properties) {
    for (const uint32_t other : {F("INTT"), F("INTR"), F("INTS"), F("POFS"), F("EMVR"), F("SMVR"), F("ISVF")}) {
      if (why.empty() && property.fourcc == other) {
        why = "with an " + EffectFourCCString(other);
      }
    }
  }
  return why.empty() ? 1 : 0;
}

// A constant vector of three zeros (CNST of three CNST 0).
bool IsZeroVector(const EffectValue& value) {
  return IsElement(value, F("CNST")) && value.args.size() == 3 &&
         std::all_of(value.args.begin(), value.args.end(), [](const EffectValue& a) { return ConstIs(a, 0); });
}

// RAND or IRND of CNST 0 and CNST `high` (a real's bits or an int).
bool RangeFromZero(const EffectValue& value, uint32_t fourcc, uint32_t& high) {
  return IsElement(value, fourcc) && value.args.size() == 2 && ConstIs(value.args[0], 0) &&
         ConstWord(value.args[1], high);
}

// TRST's x scale of a particle that is mirrored half the time: KPIN(CREL(LTHN(RAND(0, 1), 0.5), 1, -1)),
// or SLCT(IRND(0, 1), ARRY(-1, 1)).
bool RandomMirror(const EffectValue& value) {
  uint32_t high;
  if (IsElement(value, F("SLCT")) && value.args.size() == 2 && IsElement(value.args[1], F("ARRY"))) {
    const std::vector<EffectValue>& array = value.args[1].args;
    return RangeFromZero(value.args[0], F("IRND"), high) && high == 1 && array.size() == 3 &&
           ConstIs(array[1], FloatBits(-1.0f)) && ConstIs(array[2], FloatBits(1.0f));
  }
  if (!IsElement(value, F("KPIN")) || value.args.size() != 1 || !IsElement(value.args[0], F("CREL"))) {
    return false;
  }
  const EffectValue& rel = value.args[0];
  return rel.args.size() == 3 && IsElement(rel.args[0], F("LTHN")) && rel.args[0].args.size() == 2 &&
         RangeFromZero(rel.args[0].args[0], F("RAND"), high) && high == FloatBits(1.0f) &&
         ConstIs(rel.args[0].args[1], FloatBits(0.5f)) && ConstIs(rel.args[1], FloatBits(1.0f)) &&
         ConstIs(rel.args[2], FloatBits(-1.0f));
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
                   kOpacityFade = 2048, kColorRgbOnly = 4096, kPyro = 8192, kDisintegration = 16384,
                   kSceneWarp = 32768, kVisorSplat = 65536, kFrameBufferColorErosion = 1u << 18,
                   kVisorSteam = 1u << 19, kIceChargeBeam = 1u << 20,
                   kIceChargeCollision = 1u << 21, kUnfogged = 1u << 22, kVisorRunOff = 1u << 23,
                   kVisorRunOffLit = 1u << 24, kIceChargeCollisionLit = 1u << 25, kParticleLit = 1u << 26,
                   kVisorRain = 1u << 28, kBarrierShield = 1u << 29, kBarrierGlow = 1u << 30;
constexpr uint32_t kBlendAdditiveOne = 5;  // aurora::gfx::vfx::Blend::AdditiveOne (RGB One/One)
// aurora::gfx::vfx::Fogged: fogged whatever the blend (the row's fog under a MATI EBlendMode's factors).
constexpr uint32_t kFogged = 1u << 27;

// A MATI header EBlendMode as the VMAT blend, or kNoHeaderBlend where the port has no exact state for it.
// SBlendModeState(EBlendMode) 0xf0164 reads colour (eq, src, dst) at 0x1d0cc48 and alpha at 0x1d0cd38, 16 bytes
// a mode; eq 0 is Add, factors 0 Zero, 1 One, 6 SrcAlpha, 7 InvSrcAlpha (CGraphicsStateCache_NVN::SetBlendMode
// 0x1addf0, factor table 0x1d11bd0). 0: One/Zero both = Opaque; 1: One/One, alpha Zero/One = AdditiveOne;
// 2: SrcAlpha/InvSrcAlpha, alpha One/InvSrcAlpha = Alpha; 5: One/InvSrcAlpha both = PremultipliedAlpha;
// 6: SrcAlpha/One both = AdditiveAlpha. Unported: 3 (Zero/SrcAlpha), 4 (Zero/Zero), 7-8 (Min/Max), 9
// (DstColor/Zero, alpha One/Zero), 10 (One/InvSrc1Color: dual source), 11-14 (Sub/RevSub).
uint32_t MatiBlend(uint32_t mode) {
  switch (mode) {
  case 0:
    return 3;
  case 1:
    return kBlendAdditiveOne;
  case 2:
    return 0;
  case 5:
    return 6;
  case 6:
    return 7;
  default:
    return 0xffffffffu;
  }
}
// aurora::gfx::vfx::AlreadyExposed: an exposure policy carried in the features word, not a shader feature.
constexpr uint32_t kAlreadyExposed = 1u << 31;
constexpr uint32_t kModelBackCull = 1u << 17; // aurora::gfx::vfx::ModelBackCull; VMAT layout unchanged
constexpr EffectGuid kIceWedgeMati = {0xc6, 0xb1, 0x08, 0xdf, 0x38, 0xe8, 0x7f, 0x4c,
                                     0xbc, 0x9b, 0xfb, 0x67, 0x54, 0x8b, 0xc1, 0xba};

// The VFX_*_Lit sprite shaders whose lit perms (080_0..091_0) are the unlit perm's colour x the light x
// BLCM x PCMD / pi (aurora's ParticleLit). Checked perm by perm against 000_0 (re.sh fold); 7a29ff64,
// whose lit perms carry no BLCM, is left out.
bool IsLitTailShader(uint32_t shader) {
  static constexpr uint32_t kShaders[] = {0x04276ae1, 0x08757dae, 0x20e149b2, 0x2eaed6b8, 0x3fe57e12, 0x4dfde16d,
                                          0x54844806, 0x55584bd3, 0x58d7fb7b, 0x63c470ce, 0x9adb322c, 0xa617da1d,
                                          0xb9ccfc57, 0xccdf1984, 0xd471baa6, 0xf0e276eb, 0xfef02d58};
  return std::find(std::begin(kShaders), std::end(kShaders), shader) != std::end(kShaders);
}

// MTIN 756533fc-e832-48f2-b1ef-ebb41fcea14f, the wave orb's material, as the table's little-endian GUID.
constexpr EffectGuid kWaveOrbMati = {0xfc, 0x33, 0x65, 0x75, 0x32, 0xe8, 0xf2, 0x48,
                                     0xb1, 0xef, 0xeb, 0xb4, 0x1f, 0xce, 0xa1, 0x4f};

// PowerBall projectile/APSM child, MATI and constant PMDL.
// These are little-endian UUID bytes, as read from the Remastered GENP/MATI.
constexpr EffectGuid kPowerBallChild = {0x82, 0x07, 0x87, 0xae, 0xf4, 0x41, 0x26, 0x4c,
                                        0x88, 0x76, 0xc2, 0xf0, 0x70, 0xcf, 0x1e, 0x06};
constexpr EffectGuid kPowerBallMati = {0x46, 0x2a, 0x75, 0x86, 0x33, 0xe7, 0x23, 0x4d,
                                       0xad, 0xc8, 0xc7, 0x4f, 0x28, 0x15, 0x80, 0xec};
constexpr EffectGuid kPowerBallModel = {0x60, 0xae, 0xf6, 0x4b, 0xe5, 0x3b, 0x51, 0x4d,
                                        0x88, 0xbd, 0x50, 0xbb, 0xd1, 0xbd, 0xf6, 0x71};
constexpr uint32_t kPowerBallShader = 0xe899b1df;
// Power held-charge sphere: a different child/MATI, despite sharing the model and shader.
constexpr EffectGuid kPowerChargeChild = {0xdf, 0x39, 0xe6, 0xa9, 0xfe, 0x9d, 0x41, 0x42,
                                          0x84, 0x2e, 0x0b, 0x8f, 0x8a, 0x65, 0x48, 0xe2};
constexpr EffectGuid kPowerChargeMati = {0x3c, 0x1c, 0x7d, 0x90, 0xc8, 0xcc, 0x9b, 0x4b,
                                         0x87, 0x8f, 0xee, 0x8a, 0x41, 0xfc, 0x1e, 0x7f};

// Normal (an NMAP) and Env (IceChargeCollision's TCH0) are not VMAT roles: VPRM carries their slots.
enum class Role { Color, Opacity, Ramp, Ramp2, Threshold, Indirect, Palette, Normal, Env };

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
  bool cch0GainBias = false; // CCH0.z/.w are the fade's alpha gain and bias (sat(S(fade) - c3[0].w) * c3[0].z): v3
  int params = 0;           // CCH0..params-1 go out whole in VPRM (with the NMAP slot), and SMTR overrides them
  bool fogPerm = false;     // its fog perm (001) is decoded too: drawn fogged without DFOG 1, unfogged (kUnfogged) with
  // Always drawn unfogged, and no DFOG 1 needed: the visor composite shaders never see the room's fog,
  // and their effect data carries no DFOG to read.
  bool alwaysUnfogged = false;
  // Only the MATI whose header flag list carries MFC0 (USE_FRAME_BUFFER_DISTORTION) and not MFC1
  // (IS_SHIELD_EXTERIOR) is this shader's refraction perm (the f197ec38 row); every other material of
  // the shader is a different perm's maths and is refused rather than drawn with this one's.
  bool mfc0Refraction = false;
  // Neither MFC0 (USE_FRAME_BUFFER_DISTORTION) nor MFC1 (IS_SHIELD_EXTERIOR) may be set: the b1eb2b67
  // row is a shader's non-refraction perm (000). A material carrying either flag is a different perm's
  // maths (and, for MFC1, another shader), so it is refused rather than drawn with perm 000's.
  bool noRefractionFlags = false;
  // MFC1 (IS_SHIELD_EXTERIOR) set and MFC0 not: the b46bb66d row is the material the exterior shield's
  // MATI (602b16e3) carries; any other flag set is not the material its folds were read from.
  bool mfc1Exterior = false;
  // > 0: the colour map's alpha is read as pow(|a|, alphaGamma) (VMAT v4).
  float alphaGamma = 0.0f;
  // ParticleLit lights the vertices (a header's uber perm 040_1), not the fragments (VMAT v5 flag).
  bool litPerVertex = false;
};

// RECIPES.md "(b) Per shader" and RECIPES-all.md. Derived from each shader's
// own code; a Lit shader uses its perm-0 (000_0) maths, which is the unlit
// maths.
const Recipe* RecipeOf(uint32_t shader) {
  static const std::vector<Recipe> table = {
      // FrameBuffer_Indirect_Unlit (3db95827, perm 000_0): the same scene copy, offset by the warp map
      // TCH0, times vc.rgb x M; alpha sat(vc.a). d955396d without its opacity map.
      {0x3db95827, kSceneWarp, {{"TCH0", Role::Indirect, 0, 1}}, {}},
      // VFX_FrameBuffer_Indirect_* (perms 000_0 and 016_0): the scene copy at the fragment, offset by the
      // warp map's (d x 0.99609375 - 0.5) x CCH0.xy x near / view distance, times the colour and vc.rgb
      // (cc155f99 = 04276ae1: BCLR, warp TCH0; alpha BCLR.a x vc.a), or times vc.rgb with alpha
      // TCH0.x x vc.a (d955396d: warp TCH1). The warp scale rides on the map's slot.
      {0xcc155f99, kColorTex | kSceneWarp,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Indirect, 0, 1}},
       {}},
      {0x04276ae1, kColorTex | kSceneWarp,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Indirect, 0, 1}},
       {}},
      {0xd955396d, kOpacityTex | kSceneWarp,
       {{"TCH0", Role::Opacity, -1, -1}, {"TCH1", Role::Indirect, 0, 1}},
       {}},
      // The same scene copy over the colour (90ce7873, _Color_Opacity: BCLR, opacity TCH0, warp TCH1; alpha
      // BCLR.a x TCH0.x x vc.a) or over the ramp colour (03ac7d53, _ColorRamp_Opacity: warp TCH0, ramp TCH1
      // picking between the two ramp rows by .x cubed, alpha TCH1.y x the rows' mixed alpha x TCH2.x x vc.a).
      {0x90ce7873, kColorTex | kOpacityTex | kSceneWarp,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Opacity, -1, -1}, {"TCH1", Role::Indirect, 0, 1}},
       {}},
      {0x03ac7d53, kOpacityTex | kRamp | kSceneWarp,
       {{"TCH0", Role::Indirect, 0, 1}, {"TCH1", Role::Ramp, -1, -1}, {"TCH2", Role::Opacity, -1, -1}},
       {}},
      // VFX_IceChargeBeam (perms 000_1/000_0, ICECHARGE.md): PMTR C x CCH0.x x COLOR.z x CCH2.x, plus
      // PMTR A/B blended by NMAP.x x BCLR.x, plus the NMAP-bumped facing term x BCLR.rgb, x vc.rgb; alpha
      // vc.a alone. The VS's hash rotation and shake run in CPortVfxMeshBatch::Add. CCH0..2 in VPRM.
      {0x9e0605cd, kIceChargeBeam,
       {{"BCLR", Role::Color, -1, -1}, {"NMAP", Role::Normal, -1, -1}},
       {}, -1, false, 3},
      // VFX_IceChargeCollision_Unlit (perms 000_1/000_0, fog perm 001; ICECHARGE.md): the NMAP-bumped refraction
      // of the scene copy, desaturated, plus the BCLR-driven colour ramp CCH0..1, the TCH0 reflection and the
      // CCH6 rim, x (CCH5.z / exposure + vc.rgb); alpha vc.a. The VS's tangent frame runs in
      // CPortVfxMeshBatch::Add. CCH0..6 in VPRM v2 with the NMAP and TCH0 slots.
      {0x1c651f97, kIceChargeCollision,
       {{"BCLR", Role::Color, -1, -1}, {"NMAP", Role::Normal, -1, -1}, {"TCH0", Role::Env, -1, -1}},
       {}, -1, false, 7, true},
      // VFX_IceChargeCollision_Lit (390fc33b, Effect_IcePop; the lit perm of IceChargeCollision, REPORT.md
      // sections 3-5): the same feed, slots and CCH0..6 as 1c651f97, and the front end it shares. The tail
      // adds Remastered's clustered light gather and baked probe, which the runtime fills: the gather from
      // the port's room lights, the probe from the room's baked ambient at the particle. The perm has no
      // CCH6 rim term, and its BLCM and PCMD are material uniforms the IcePop MATI does not carry (white and
      // 1). Fogged without DFOG 1 as the unlit perm is; the blend is additive, so the port's fog never mixes.
      {0x390fc33b, kIceChargeCollision | kIceChargeCollisionLit,
       {{"BCLR", Role::Color, -1, -1}, {"NMAP", Role::Normal, -1, -1}, {"TCH0", Role::Env, -1, -1}},
       {}, -1, false, 7, true},
      // VFX_VisorRunOff (b5e12668 unlit, d85d4169 lit; FS perm 000; SPEC-fb-visor.md sections 5/6): the
      // TCH0 flow map scrolled by CCH1 drives a mask m and a glow; the scene copy at the fragment,
      // offset by CCH0.x x CCH1.z x the map x 0.01, is tinted between CCH3.rgb and CCH4.rgb by m, x the
      // fade and vc.rgb. CCH0..4 in VPRM, animated by SMTR; alpha vc.a. The visor composite pass has no
      // fog, so both are Unfogged whatever the effect's DFOG says.
      {0xb5e12668, kVisorRunOff, {{"TCH0", Role::Color, -1, -1}}, {}, -1, false, 5, false, true},
      {0xd85d4169, kVisorRunOffLit, {{"TCH0", Role::Color, -1, -1}}, {}, -1, false, 5, false, true},
      // VFX_VisorRain (0ec41df0, the visor rain splash GENP 45ba782d; FS perm 000, perms 001/002 identical,
      // 003..005 the particle-debug overlay; vfx/0ec41df0.md): TCH0 a 2-channel noise map (BC5, repeat); PMTR
      // rows 0..2 (R0 = noise offset xy, PAP9 fade, PAP8 gain; R1 = PAP3, PAP5 (runner pick), PAP7, size; R2 =
      // 16, 0.15) and CCH0 (1, .12, .2, 4 by the GENP's SMTR) drive a sin-sum drop mask H; where 0.5 < H < 1
      // the scene copy at the fragment, offset along the mask's normal by (1 - fade) x CCH0.w x near / depth, x
      // (0.75 .. 1) x vc.rgb, alpha vc.a; elsewhere discarded. The visor composite pass has no fog, so Unfogged
      // whatever the DFOG.
      {0x0ec41df0, kVisorRain, {{"TCH0", Role::Color, -1, -1}}, {}, -1, false, 1, false, true},
      // VFX_VisorSteam (53eceaa4, the SteamMP1 HUD billboard's model particle, MATI f4970293; FS perm 000_0,
      // VS 000_1; vfx/53eceaa4.md): TCH0 Repeat_Texture (wrap repeat) read at uv x CCH0.x, offset by TCH1
      // Macro_Texture (the second raw sample slot, the Indirect role: no CCH warp) and scrolled by the VS's
      // o2.yz, which depends on CCH0/2/3/4 only (the shader computes it). CCH0..4 in VPRM, SMTR-animated by the
      // Ship1 particle variables. No scene copy; the visor composite pass has no fog, so Unfogged.
      {0x53eceaa4, kVisorSteam, {{"TCH0", Role::Color, -1, -1}, {"TCH1", Role::Indirect, -1, -1}}, {}, -1, false,
       5, false, true},
      // VFX_FrameBuffer_Color_Erosion_Unlit (d8ece62c, the frozen-player HUD effect GENP c23f13f7, MATI
      // eaef3c6e; FS perm 000_0, fog perm 001_0, 002/004 identical to 000 and 003/005 to 001): BCLR's
      // x and y offset the scene copy at the fragment by near / HUD depth x CCH0.xy, read at lod
      // max(0, CCH0.z - 1); rgb = (BCLR.rgb + scene) x vc.rgb x PCMD, alpha max(0, BCLR.a - PMTR row
      // 0's x) x vc.a (the VS's o2 is PMTR row 0). CCH0 in VPRM, animated by SMTR.
      {0xd8ece62c, kFrameBufferColorErosion | kErosion, {{"BCLR", Role::Color, -1, -1}}, {{kSrcErosion, 0, 0}},
       -1, false, 1, true},
      // VFX_BarrierShield (f197ec38, the Parasite Queen barrier's MATI 6429c234; its refraction perm 002,
      // the material flag MFC0 USE_FRAME_BUFFER_DISTORTION with shield-exterior MFC1 off; build/
      // s-queenbarrier-20261024/SHADER-IMPLEMENTATION.md): TCH0 read at (-2u, 2v) is the pattern (its .b
      // scrolls four fract() noise reads of TCH1, .a widens the breakup mask, .rg drive the sawtooth motion),
      // the scene copy at the fragment is the base colour, and the rim/fresnel/mesh-colour glow adds over it.
      // TCH1 is the second raw sample slot (the Indirect role: this recipe's own reads, no CCH warp); the
      // mesh's vertex colour needs VMSH v2 (PortVfxBarrierReady). CCH0 whole in VPRM v1, animated by SMTR.
      // The 003 fog sibling and the perm-000 displacement variant are not ported: without DFOG 1 or without
      // MFC0 the material is refused (no VMAT), never drawn with another perm's maths.
      {0xf197ec38, kBarrierShield,
       {{"TCH0", Role::Color, -1, -1}, {"TCH1", Role::Indirect, -1, -1}},
       {}, -1, false, 1, false, false, true},
      // VFX_BarrierGlow (b1eb2b67, the same Parasite Queen barrier's one-variable MATI ba0acbd6; FS perm
      // 000, the non-refraction perm: the material carries neither MFC0 nor MFC1; build/
      // s-queenbarrier-20261024/GLOW-IMPLEMENTATION.md): TCH0 read three times at (-u, v): its .r scrolls
      // the second read's v, its .b the breakup of the third read's .a, which also drives k and the fade.
      // No scene copy (the glow adds over the frame, so no scene gate). The VS's displacement is in the
      // CPU feed (AddBarrierGlow); the mesh's vertex colour needs VMSH v2 (PortVfxBarrierGlowReady).
      // CCH0 whole in VPRM v1, animated by SMTR. TCH1 is in the MATI but this perm never reads it.
      {0xb1eb2b67, kBarrierGlow, {{"TCH0", Role::Color, -1, -1}}, {}, -1, false, 1, false, false,
       false, true},
      // VFX_BarrierShield's exterior (b46bb66d, MATI 602b16e3 with MFC1 IS_SHIELD_EXTERIOR: Intro_Reactor's
      // and the Phendrana caves' shields; vfx/b46bb66d.md): both barrier bits. TCH1 read twice at (-u, v)
      // (its .r scrolls the second read, whose .a is the breakup); no scene copy. The VS's band sweep and
      // normal push are in the CPU feed (AddBarrierExterior); the mesh colour needs VMSH v2. CCH0 whole in
      // VPRM v1. Its fog perm (001) is decoded: fogged without DFOG 1, unfogged with.
      {0xb46bb66d, kBarrierShield | kBarrierGlow, {{"TCH1", Role::Color, -1, -1}}, {}, -1, false, 1,
       true, false, false, false, true},
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
      // 8e6bfa33 with an opacity map (VFX_ColorRamp_Indirect_Opacity_Thresholding_Unlit): the same VS and CCH0
      // layout; ramp TCH0, warp TCH1, opacity TCH2 (x the alpha), threshold TCH3.
      {0xd47ffcc3, kOpacityTex | kRamp | kIndirect | kThresholding,
       {{"TCH0", Role::Ramp, 1, 2}, {"TCH1", Role::Indirect, -1, -1}, {"TCH2", Role::Opacity, -1, -1},
        {"TCH3", Role::Threshold, -1, -1}},
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
      // 20a05041 VFX_ColorRamp_Indirect_Unlit (+ depth soften): fa49ff3c's maths times the soften factor.
      {0x20a05041, kRamp | kIndirect | kDepthSoften,
       {{"TCH0", Role::Ramp, 0, 1}, {"TCH1", Role::Indirect, -1, -1}},
       {}},
      // 3fe57e12 VFX_ColorRamp_Indirect_Lit: perm 000_0 folds identical to fa49ff3c's.
      {0x3fe57e12, kRamp | kIndirect,
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
      // a0482072 VFX_ColorIndexing_Unlit: 28bf8533's lookup and palette without the opacity read.
      {0xa0482072, kColorIndexing,
       {{"BCLR", Role::Color, -1, -1}, {"TCH0", Role::Palette, -1, -1}},
       {{kSrcIndexScale, 0, 0}, {kSrcIndexOffset, 0, 1}, {kSrcIndexRow, 0, 2}}},
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
      // f0e276eb VFX_DualModColorRamp_Lit: perm 000_0 folds identical to cc9405ea's.
      {0xf0e276eb, kRamp | kDualMod,
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
      // VFX_Pyro (perms 038/039 sprite, 040/041 oriented; SPEC-pyro.md): TCH0 the Pyro map, TCH1
      // the Light map, TCH2 the gradient LUT, PMTR rows 0 (lit tint) and 1 (emission); CCH0 is the
      // light direction (xyz) and the Light map's weight (w). 7498e54a is 8278c22d with the
      // scene-depth soften.
      {0x8278c22d, kPyro,
       {{"TCH0", Role::Color, -1, -1}, {"TCH1", Role::Normal, -1, -1}, {"TCH2", Role::Ramp, -1, -1}},
       {}, -1, false, 1, true},
      {0x7498e54a, kPyro | kDepthSoften,
       {{"TCH0", Role::Color, -1, -1}, {"TCH1", Role::Normal, -1, -1}, {"TCH2", Role::Ramp, -1, -1}},
       {}, -1, false, 1, true},
      // VFX_Disintegration (perm 000_0, SPEC-disint-ice.md): TCH0 the mask, PMTR rows 0..3 (the VS
      // passes them as v2..v5). Perm 000 does no depth soften, so SFTD is not read.
      {0xd749057f, kDisintegration, {{"TCH0", Role::Color, -1, -1}}, {}},
      // VFX_VisorSplat (perm 000_0): BCLR at its uv plus the TCH1 warp, which is read at the world position
      // (x, y) x PMTR row 1.xy + row 0.zw, scaled by row 0.xy and taken to the quad's axes; alpha
      // max(0, BCLR.a + row 1.z - 1) x vc.a. The runtime supplies the world position and the axes.
      {0x9e2cbd46, kVisorSplat, {{"BCLR", Role::Color, -1, -1}, {"TCH1", Role::Indirect, -1, -1}}, {}},
  };
  for (const Recipe& recipe : table) {
    if (recipe.shader == shader) {
      return &recipe;
    }
  }
  return nullptr;
}

// A GPSM's header MTRL (node.baseMaterial, drawn where MTIN is nil), as its shader's perm 048: FS 048_0 is
// rgb = vc.rgb x v1.x and a = sat(v1.y x vc.a), textured x TEXR.rgb and x pow(|TEXR.a|, 2.2) (one FS for
// every untextured shader below: md5 77e87faf). VS 048_1 passes colour and v1 through; the quad differs.
enum class HeaderQuad { RotatedSprite, Sprite, Axis, Planar };
struct HeaderShader {
  uint32_t shader;
  bool textured;
  HeaderQuad quad;
  // The TEXR is an ATFB on a texture array, its layer the flipbook entry's (9d1a1432: 048_0 on a
  // sampler2DArray, layer roundEven(v.z)).
  bool flipbook = false;
  // 52d26efe: perm 048's FS with an AERO erosion (o2.x), its TEXR a TXP2, drawn under LIT_ 1 by the
  // uber perm 040, which lights the vertices (kb topic/header-mtrl-perm048.md).
  bool litPalette = false;
  // LIT_ 1 draws uber perm 040: its FS is 048_0 and its VS 040_1 lights the colour per vertex as
  // 52d26efe's does (b42cacf5 = 29f35b3a perm for perm; their 040_1 lighting is 52d26efe's, reordered).
  bool vertexLit = false;
  // 31c6b110: the TEXR a TXFB on a texture array (layer from the index), and an ANTH thresholding
  // map on its own TRST of the quad's corner, cut by the particle's FadeIn/FadeOut/Softness
  // (kb topic/header-mtrl-31c6b110.md).
  bool arrayThreshold = false;
};

const HeaderShader* HeaderShaderOf(uint32_t shader) {
  static const HeaderShader table[] = {
      // Camera-facing quads rotated by the particle's angle (ROTA): the port's VORN 0 sprite.
      {0xb42cacf5, true, HeaderQuad::RotatedSprite, false, false, true},
      {0x29f35b3a, true, HeaderQuad::RotatedSprite, false, false, true},
      {0x4d6746f4, false, HeaderQuad::RotatedSprite},
      {0x52d26efe, true, HeaderQuad::RotatedSprite, false, true, true},
      {0x31c6b110, true, HeaderQuad::RotatedSprite, false, false, false, true},
      // Unrotated camera-facing quads: a VORN 0 sprite without ROTA.
      {0xe2bfbf91, true, HeaderQuad::Sprite},
      {0x9d1a1432, true, HeaderQuad::Sprite, true},
      {0x1e245375, false, HeaderQuad::Sprite},
      {0xd16ade09, false, HeaderQuad::Sprite},
      // B = cross(toCam, axis), C = cross(toCam, B): the port's VORN 1/2 quad.
      {0xe27845ed, true, HeaderQuad::Axis},
      {0xe512fad9, false, HeaderQuad::Axis},
      {0x471bb62f, false, HeaderQuad::Axis},
      // P + (q.x - 0.5) A + (q.y - 0.5) B from the instance matrix: the port's PPRM 2 planar square.
      {0x4ba0aadc, true, HeaderQuad::Planar},
      {0xc38294d2, false, HeaderQuad::Planar},
  };
  for (const HeaderShader& entry : table) {
    if (entry.shader == shader) {
      return &entry;
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

constexpr uint32_t kNoHeaderBlend = 0xffffffffu;

struct Mati {
  uint32_t shader = 0;  // the four bytes at 0x48, big-endian
  uint32_t headerBlend = kNoHeaderBlend;  // the header's EBlendMode, the LE u32 at 0x61
  uint8_t cull = 0xff;  // the header's cull byte at 0x5d; 0xff (unset) resolves to 2 (back)
  std::map<std::string, MatiTexture> textures;
  std::map<std::string, std::array<float, 4>> vectors;
  // The header's material-flag list, after the parameters: a u32 count then that many four-byte tags
  // (the tags the material enables, e.g. MFC0), then the next chunk (RFRM). Every shipped barrier MATI
  // carries 0 or 1 (6429c234 has MFC0, the exterior b46bb66d MFC1). Empty when absent or unparsed.
  std::vector<std::string> flags;
};

// Whether the MATI's header flag list carries `tag` (a material flag such as MFC0).
bool HasMatiFlag(const Mati& mati, const char* tag) {
  return std::find(mati.flags.begin(), mati.flags.end(), tag) != mati.flags.end();
}

using port::ReadLEFloat;

// The header's shader and the parameter entries (u8 type + four tag bytes +
// payload): 6 and 5 texture (guid, texCoord, filter, wrap x/y/z; 36 bytes), 3
// and 4 vec4, 1 float, 0 int. An unknown type ends the list. Types 5 and 4 are
// the 2D textures and the colours of a few shaders (1c651f97's NMAP, TCH0 and
// CCH0/1/4/6); every disc MATI with them parses to its end with these sizes.
// The parameter count follows the variable table at 0x65 (LE u32 count of
// 10-byte CVariableDescs: variable tag, constant tag, two bytes, e.g. EXSC CCH0
// 01 01 = CCH0.y), empty in all but the four dd007440 MATIs RenderLooseDataMP1
// names (kb topic/model-black-silhouette.md).
bool ParseMati(const std::vector<uint8_t>& d, Mati& out) {
  if (d.size() < 0x6d) {
    return false;
  }
  const uint32_t prefix = ReadLE32(d.data() + 0x65);
  if (prefix > (d.size() - 0x6d) / 10) {
    return false;
  }
  out.shader = port::GetBE32(d, 0x48);
  out.headerBlend = ReadLE32(d.data() + 0x61);
  out.cull = d[0x5d];
  size_t at = 0x69 + 10 * size_t(prefix);
  const uint32_t count = ReadLE32(d.data() + at);
  at += 4;
  uint32_t parsed = 0;
  bool parametersDone = count == 0;
  for (; parsed < count; ++parsed) {
    if (at + 5 > d.size()) {
      break;
    }
    const uint8_t type = d[at];
    const std::string tag(reinterpret_cast<const char*>(d.data() + at + 1), 4);
    at += 5;
    const size_t size = type == 6 || type == 5 ? 36 : type == 3 || type == 4 ? 16 : type == 1 || type == 0 ? 4 : 0;
    if (size == 0 || at + size > d.size()) {
      break;
    }
    if (type == 6 || type == 5) {
      MatiTexture texture;
      std::memcpy(texture.guid.data(), d.data() + at, 16);
      texture.texCoord = ReadLE32(d.data() + at + 16);
      texture.filter = int32_t(ReadLE32(d.data() + at + 20));
      texture.wrapX = int32_t(ReadLE32(d.data() + at + 24));
      texture.wrapY = int32_t(ReadLE32(d.data() + at + 28));
      out.textures[tag] = texture;
    } else if (type == 3 || type == 4) {
      std::array<float, 4> value;
      for (int c = 0; c < 4; ++c) {
        value[size_t(c)] = ReadLEFloat(d.data() + at + 4 * size_t(c));
      }
      out.vectors[tag] = value;
    }
    at += size;
    parametersDone = parsed + 1 == count;
  }
  // The material-flag list: a LE u32 count then that many four-byte tags. Data with no room for it (a
  // synthetic MATI) or a parameter list that did not parse to its end carries no flags, which fails a
  // recipe that requires one. A count past the blob's end is not a flag list: stop, leaving them empty.
  if (parametersDone && at + 4 <= d.size()) {
    const uint32_t flagCount = ReadLE32(d.data() + at);
    at += 4;
    if (flagCount <= (d.size() - at) / 4) {
      for (uint32_t i = 0; i < flagCount; ++i, at += 4) {
        out.flags.emplace_back(reinterpret_cast<const char*>(d.data() + at), 4);
      }
    }
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

// One item of the port-only PSYS: the variable's EPortVarType and slot, whether
// only CAddVarSystem::Init evaluates it, and its element.
struct SystemItem {
  uint32_t type;
  uint32_t slot;
  uint32_t initOnly;
  std::vector<uint8_t> element;
};

// A zero of `type` as a retail CNST.
void Zero(Type type, std::vector<uint8_t>& out) {
  AppendBE32(out, F("CNST"));
  const int reals = type == Type::Vector ? 3 : type == Type::Color ? 4 : 0;
  if (reals == 0) {
    AppendBE32(out, 0);  // an int's 0 and a real's bits
  }
  for (int i = 0; i < reals; ++i) {
    AppendBE32(out, F("CNST"));
    AppendBE32(out, 0);
  }
}

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

  // A written vector element taken through the port-only BSIS or BSSC (CVEPortBasis): (x, y, z) to
  // (-x, z, y) or (x, z, y). Each is its own inverse, so BSIS(BSIS(v)) is written as v, and a
  // constant is turned here.
  static void Basis(uint32_t op, const std::vector<uint8_t>& inner, std::vector<uint8_t>& out) {
    const auto word = [&inner](size_t at) { return port::ReadBE32(inner.data() + at); };
    if (inner.size() >= 4 && word(0) == op) {
      out.insert(out.end(), inner.begin() + 4, inner.end());
      return;
    }
    const uint32_t cnst = F("CNST");
    if (inner.size() == 28 && word(0) == cnst && word(4) == cnst && word(12) == cnst && word(20) == cnst) {
      AppendBE32(out, cnst);
      AppendBE32(out, cnst);
      AppendBE32(out, op == F("BSIS") ? word(8) ^ 0x80000000u : word(8));
      AppendBE32(out, cnst);
      AppendBE32(out, word(24));
      AppendBE32(out, cnst);
      AppendBE32(out, word(16));
      return;
    }
    AppendBE32(out, op);
    out.insert(out.end(), inner.begin(), inner.end());
  }

  // A vector element in Remastered's numbers, written as the port's (BSIS around it).
  bool PortVector(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    std::vector<uint8_t> inner;
    if (!Element(value, Type::Vector, inner, why)) {
      return false;
    }
    Basis(F("BSIS"), inner, out);
    return true;
  }

  // -x for a real element: a constant's sign flipped, anything else MULT(x, CNST -1).
  bool NegatedReal(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    std::vector<uint8_t> inner;
    if (!Element(value, Type::Real, inner, why)) {
      return false;
    }
    if (inner.size() == 8 && port::ReadBE32(inner.data()) == F("CNST")) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, port::ReadBE32(inner.data() + 4) ^ 0x80000000u);
      return true;
    }
    AppendBE32(out, F("MULT"));
    out.insert(out.end(), inner.begin(), inner.end());
    AppendBE32(out, F("CNST"));
    AppendBE32(out, FloatBits(-1.0f));
    return true;
  }

  // One element of `type` into `out`, or false with `why` set.
  //
  // Remastered's particle space is X left, Y up, Z forward; retail's is Z up. A vector tree is
  // written to give Remastered's numbers: reads of the port's state are taken into its space
  // (BSIS, BSSC for EXTS), MPCB(v) is BSIS(v), and the port turns the result back where it is
  // used (a vector property, a mod vector's or emitter's vector argument).
  // Writes `fourcc` of `type`'s shape (ShapeOf) from position `from` up to its end or a raw byte or
  // word, the element at each position from args, in order.
  bool ShapedArgs(uint32_t fourcc, Type type, size_t from, const EffectValue* args, std::vector<uint8_t>& out,
                  std::string& why) const {
    const char* shape = ShapeOf(fourcc, type);
    for (const char* c = shape + from; *c != 0 && *c != 'd' && *c != 'w'; ++c, ++args) {
      if (!Element(*args, TypeOfLetter(*c), out, why)) {
        return false;
      }
    }
    return true;
  }

  bool Element(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Element) {
      why = "a bare value where retail reads an element";
      return false;
    }
    if (value.fourcc == F("SEMR") && value.args.size() == 2) {
      m_semrPos = &value.args[0];
    }
    // A swoosh's PAP0..3 and PAP9 reads (int ids 0..3, real id 8) have nothing behind them
    // unless it defines IAV0..3 or ADV8 (its per-point variables, m_swooshVars).
    if (m_swoosh && value.args.empty() && (type == Type::Real || type == Type::Int)) {
      static constexpr uint32_t kRead[] = {F("PAP0"), F("PAP1"), F("PAP2"), F("PAP3"), F("PAP9")};
      for (uint32_t n = 0; n < 5; ++n) {
        const uint32_t id = n == 4 ? 8 : 9 + n;
        if (value.fourcc == kRead[n] && (type == Type::Int || n == 4) && (m_swooshVars & 1u << id) == 0) {
          why = EffectFourCCString(kRead[n]) + " in a swoosh";
          return false;
        }
      }
    }
    // The particle variables a tree reads (m_varReads): real ids 0..8 are PAP1..9, int ids 9..12
    // PAP0..3.
    if (value.args.empty()) {
      for (uint32_t n = 0; n < 10; ++n) {
        const char name[5] = {'P', 'A', 'P', char('0' + n), 0};
        if (value.fourcc != F(name)) {
          continue;
        }
        if (type == Type::Real && n >= 1) {
          m_varReads |= 1u << (n - 1);
        } else if (type == Type::Int && n <= 3) {
          m_varReads |= 1u << (9 + n);
        }
      }
    }
    // In a system variable, KPIN(x) and IRND(a, b) are evaluated once, by
    // CAddVarSystem::Init (CComplexKeepInitial::RebaseComplexOp; IRND is
    // CRERandomConstant): each becomes a hidden init-only real the tree reads.
    if (m_sysItems != nullptr && type == Type::Real &&
        ((value.fourcc == F("KPIN") && value.args.size() == 1) ||
         (value.fourcc == F("IRND") && value.args.size() == 2))) {
      SystemItem hidden{0, m_sysNextHidden++, 1, {}};
      if (value.fourcc == F("IRND")) {
        AppendBE32(hidden.element, F("RAND"));
        if (!Element(value.args[0], type, hidden.element, why) || !Element(value.args[1], type, hidden.element, why)) {
          return false;
        }
      } else if (!Element(value.args[0], type, hidden.element, why)) {
        return false;
      }
      AppendBE32(out, F("PSYR"));
      AppendBE32(out, hidden.slot);
      m_sysItems->push_back(std::move(hidden));
      return true;
    }
    // Elsewhere in a generator or swoosh KPIN(x) is a hidden per-particle variable
    // (CComplexKeepInitial::RebaseComplexOp 0x2d8fac), evaluated once as the particle or point
    // is born (_Init_Particle 0x2784b8): the port-only PKPR latches it per particle.
    if (m_keeps && m_spawnSysItems == nullptr && type == Type::Real && value.fourcc == F("KPIN") &&
        value.args.size() == 1) {
      const uint32_t first = m_keepNext;
      std::vector<uint8_t> kept;
      if (!Element(value.args[0], type, kept, why)) {
        m_keepNext = first;
        return false;
      }
      if (m_keepNext == kKeepSlots) {
        m_keepNext = first;
        why = "more than " + std::to_string(kKeepSlots) + " per-particle KPINs";
        return false;
      }
      AppendBE32(out, F("PKPR"));
      AppendBE32(out, m_keepNext++);
      out.insert(out.end(), kept.begin(), kept.end());
      return true;
    }
    // An int KPIN (CIEKeepInitial 0x2a98f0) is the same per-particle latch. PKPR is real, so it
    // latches ITRL(x, 1) and RTOI(.., 1) reads it back: an int's float is exact below 2^24,
    // and the product of an integral float and 1 has nothing for RTOI's truncation to drop.
    if (m_keeps && m_spawnSysItems == nullptr && type == Type::Int && value.fourcc == F("KPIN") &&
        value.args.size() == 1) {
      const uint32_t first = m_keepNext;
      std::vector<uint8_t> kept;
      if (!Element(value.args[0], type, kept, why)) {
        m_keepNext = first;
        return false;
      }
      if (m_keepNext == kKeepSlots) {
        m_keepNext = first;
        why = "more than " + std::to_string(kKeepSlots) + " per-particle KPINs";
        return false;
      }
      AppendBE32(out, F("RTOI"));
      AppendBE32(out, F("PKPR"));
      AppendBE32(out, m_keepNext++);
      AppendBE32(out, F("ITRL"));
      out.insert(out.end(), kept.begin(), kept.end());
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(1.0f));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(1.0f));
      return true;
    }
    // IRSN(x) is CREInitialRandomSign: a sign drawn once at frame 0 and kept in the particle's
    // variable memory, which the port-only PIRS keeps in a PKPR-counter slot (never latched by
    // PKPR's birth latch: it draws at frame 0). With no per-particle slots (a system's own
    // elements) it takes kNoSlot and a per-element sign.
    if (type == Type::Real && value.fourcc == F("IRSN") && value.args.size() == 1) {
      const uint32_t first = m_keepNext;
      std::vector<uint8_t> kept;
      if (!Element(value.args[0], type, kept, why)) {
        m_keepNext = first;
        return false;
      }
      uint32_t slot = 0xffffffffu;
      if (m_keeps && m_spawnSysItems == nullptr) {
        if (m_keepNext == kKeepSlots) {
          m_keepNext = first;
          why = "more than " + std::to_string(kKeepSlots) + " per-particle KPINs and IRSNs";
          return false;
        }
        slot = m_keepNext++;
      }
      AppendBE32(out, F("PIRS"));
      AppendBE32(out, slot);
      out.insert(out.end(), kept.begin(), kept.end());
      return true;
    }
    // NEGT(x) is CRENegate 0x294f20: -x, which retail has as MULT(x, CNST -1) (x * -1 is -x
    // bit for bit, a zero's sign included; the child is evaluated once, first).
    if (type == Type::Real && value.fourcc == F("NEGT") && value.args.size() == 1) {
      AppendBE32(out, F("MULT"));
      if (!Element(value.args[0], type, out, why)) {
        return false;
      }
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(-1.0f));
      return true;
    }
    // MPXV(i, fallback) is MPT_CREExternalValue 0x29715c: the port-only PMXV, the int then the real.
    if (type == Type::Real && value.fourcc == F("MPXV") && value.args.size() == 2) {
      AppendBE32(out, F("PMXV"));
      return ShapedArgs(F("PMXV"), type, 0, value.args.data(), out, why);
    }
    // PSA0..PSA9: the system's own additional real n (GetRealElement 0x260de4:
    // AddRefVar(ctx 0, real, n)), which its SVRn sets. The port-only PSYR and n.
    if (type == Type::Real && value.args.empty() && SystemRealRead(value.fourcc) >= 0) {
      AppendBE32(out, F("PSYR"));
      AppendBE32(out, uint32_t(SystemRealRead(value.fourcc)));
      return true;
    }
    // A vector PSAn is the system's own additional vector n (GetVectorElement 0x251a44:
    // AddRefVar(ctx 0, vector, n)), which its SVVn sets. The port-only PSYV and n.
    if (type == Type::Vector && value.args.empty() && SystemRealRead(value.fourcc) >= 0) {
      AppendBE32(out, F("PSYV"));
      AppendBE32(out, uint32_t(SystemRealRead(value.fourcc)));
      return true;
    }
    // An int PSAn is the system's own additional int n (GetIntElement 0x26a910:
    // AddRefVar(ctx 0, int, n)), which its SVIn sets. The port-only PSYI and n.
    if (type == Type::Int && value.args.empty() && SystemRealRead(value.fourcc) >= 0) {
      AppendBE32(out, F("PSYI"));
      AppendBE32(out, uint32_t(SystemRealRead(value.fourcc)));
      return true;
    }
    // SELT is CIEElapsedTicks(ctx 0) 0x2a9f04, the system context's tick count: the tick sets it
    // to the generator's frame (+0x2c, TickSystem_Complete 0x24acc0) and steps both together
    // (0x24aee8..0x24aef8). That frame is retail's GEMT (x74_curFrame).
    if (type == Type::Int && value.args.empty() && value.fourcc == F("SELT") && m_gpsm) {
      AppendBE32(out, F("GEMT"));
      return true;
    }
    // Reads of the port's state, in retail's space, taken into Remastered's.
    if (type == Type::Vector && value.args.empty() &&
        (value.fourcc == F("PLOC") || value.fourcc == F("PLCO") || value.fourcc == F("PVEL") ||
         value.fourcc == F("PSOF") || value.fourcc == F("PSOU") || value.fourcc == F("PSOR") ||
         value.fourcc == F("PSTR") || value.fourcc == F("CMPS") || value.fourcc == F("CPSS") ||
         value.fourcc == F("EXTT") ||
         value.fourcc == F("EXTS"))) {
      AppendBE32(out, value.fourcc == F("EXTS") ? F("BSSC") : F("BSIS"));
      AppendBE32(out, value.fourcc);
      return true;
    }
    // PLCD is CVEParticleLocationDelta 0x2a1f0c, the position less the previous one: SUB_ of the
    // two reads.
    if (type == Type::Vector && value.args.empty() && value.fourcc == F("PLCD")) {
      AppendBE32(out, F("SUB_"));
      for (const uint32_t read : {F("PLOC"), F("PLCO")}) {
        AppendBE32(out, F("BSIS"));
        AppendBE32(out, read);
      }
      return true;
    }
    // MPCB(v) is MPT_CVEConvertToXLeftYUpZForward 0x2a3020, (-x, z, y) of a vector in
    // retail's space: BSIS(v). MCBS(v) is its scale form (0x2a30f8, (x, z, y)): BSSC(v).
    // MPAC inside is the port-only PMAC (exact, below).
    if ((value.fourcc == F("MPCB") || value.fourcc == F("MCBS")) && type == Type::Vector) {
      std::vector<uint8_t> inner;
      if (value.args.size() == 1) {
        if (!Element(value.args[0], type, inner, why)) {
          return false;
        }
        Basis(value.fourcc == F("MCBS") ? F("BSSC") : F("BSIS"), inner, out);
        return true;
      }
      why = EffectFourCCString(value.fourcc) + " in a form retail has no element for";
      return false;
    }
    // MPCB(m) on a mod vector runs m in retail's space on the particle's position and velocity,
    // so m's vectors are not turned.
    if (value.fourcc == F("MPCB") && type == Type::ModVector) {
      if (value.args.size() != 1) {
        why = "MPCB in its angle form where retail reads a mod vector";
        return false;
      }
      const bool outer = m_mp1Space;
      m_mp1Space = true;
      const bool ok = Element(value.args[0], type, out, why);
      m_mp1Space = outer;
      return ok;
    }
    // A mod vector's CNST sets the velocity, (-x, z, y) of Remastered's.
    if (value.fourcc == F("CNST") && type == Type::ModVector && value.args.size() == 3 && !m_mp1Space) {
      AppendBE32(out, F("CNST"));
      return NegatedReal(value.args[0], out, why) && Element(value.args[2], Type::Real, out, why) &&
             Element(value.args[1], Type::Real, out, why);
    }
    if (const std::optional<bool> done = DirectionElement(value, type, out, why)) {
      return *done;
    }
    if (const std::optional<bool> done = EmitterShape(value, type, out, why)) {
      return *done;
    }
    // EMRV: the per-tick change of the system's translation (CVEVelocityCalc), in Remastered's
    // axes. The port-only PEMV gives it in the port's, so BSIS turns it back. XFMD 0, 3 and 6 give
    // the translation a zero constant, so EMRV is zero; 1 and 4 are undecoded.
    if (value.fourcc == F("EMRV") && type == Type::Vector && value.args.empty()) {
      if (m_swoosh) {
        why = "EMRV in a swoosh";
        return false;
      }
      if (m_xfmd == 0 || m_xfmd == 3 || m_xfmd == 6) {
        AppendBE32(out, F("CNST"));
        for (int i = 0; i < 3; ++i) {
          AppendBE32(out, F("CNST"));
          AppendBE32(out, 0);
        }
        return true;
      }
      if (m_xfmd != 2 && m_xfmd != 5) {
        why = "EMRV under XFMD " + std::to_string(m_xfmd);
        return false;
      }
      // XFMD 5 adds the generator's INTT (SetupTransformMode 0x289338, a CVEAdd of the +0x58
      // element), written as retail's POFS, to the local translation over the scale. Only a
      // constant one is exact: the port keeps the value its own update evaluated, where Remastered
      // evaluates the element again here, which a random or time-varying one would not repeat.
      if (m_xfmd == 5 && m_pofs == 2) {
        why = "EMRV under XFMD 5 with a POFS that is not a constant";
        return false;
      }
      AppendBE32(out, F("BSIS"));
      AppendBE32(out, F("PEMV"));
      AppendBE32(out, m_xfmd == 5 ? (m_pofs == 1 ? 2 : 1) : 0);
      return true;
    }
    if (const std::optional<bool> done = SectionedVector(value, type, out, why)) {
      return *done;
    }
    // CREL(LTHN(a, b), A, B) is a < b ? B : A, the real that is chosen read alone, as retail's
    // CLTN(a, b, B, A) (CRealElement.cpp CRECompareLessThan) does.
    if (value.fourcc == F("CREL") && type == Type::Real && value.args.size() == 3) {
      const EffectValue& condition = value.args[0];
      if (!IsElement(condition, F("LTHN")) || condition.args.size() != 2) {
        why = "CREL on a condition that is not LTHN (" +
              (condition.kind == EffectValue::Kind::Element ? EffectFourCCString(condition.fourcc)
                                                           : std::string("a raw value")) +
              ")";
        return false;
      }
      AppendBE32(out, F("CLTN"));
      return Element(condition.args[0], Type::Real, out, why) && Element(condition.args[1], Type::Real, out, why) &&
             Element(value.args[2], Type::Real, out, why) && Element(value.args[1], Type::Real, out, why);
    }
    // PNCV: the port-only PNCV, the particle's velocity normalised by CVENormalizedCompensatedVelocity
    // 0x2a2800 (one refined FRSQRTE step, up vector below 2^-46). It yields Remastered's numbers, so
    // inside an MPCB mod vector (retail's space) BSIS turns them; elsewhere the consumer is in
    // Remastered's space or wraps the property in BSIS.
    if (value.fourcc == F("PNCV") && type == Type::Vector && value.args.empty()) {
      if (m_mp1Space) {
        AppendBE32(out, F("BSIS"));
      }
      AppendBE32(out, F("PNCV"));
      return true;
    }
    // ROTV(v, rotation) outside a component: the port-only PRTV (the vector, the rotation kind and
    // REUL's angles), CVERotateVector 0x2a0328 in Remastered's space. The rotations it carries are
    // REUL (order 0), EXTR and CMRT; EXTR and CMRT read the generator, which a swoosh has not.
    if (value.fourcc == F("ROTV") && type == Type::Vector && value.args.size() == 2) {
      const EffectValue& rot = value.args[1];
      std::vector<uint8_t> prtv;
      bool ok = false;
      // PRTV evaluates in Remastered's space, so its vector is not retail's even inside an MPCB.
      const bool outer = m_mp1Space;
      m_mp1Space = false;
      if (IsElement(rot, F("REUL")) && rot.args.size() == 4) {
        const EffectValue& order = rot.args[3];
        if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
          why = "ROTV by a REUL in an axis order other than 0";
        } else {
          AppendBE32(prtv, F("PRTV"));
          ok = Element(value.args[0], Type::Vector, prtv, why);
          if (ok) {
            AppendBE32(prtv, F("REUL"));
            for (size_t i = 0; ok && i < 3; ++i) {
              ok = Element(rot.args[i], Type::Real, prtv, why);
            }
          }
        }
      } else if ((IsElement(rot, F("EXTR")) || IsElement(rot, F("CMRT"))) && rot.args.empty()) {
        if (m_swoosh) {
          why = "ROTV by " + EffectFourCCString(rot.fourcc) + " in a swoosh";
        } else {
          AppendBE32(prtv, F("PRTV"));
          ok = Element(value.args[0], Type::Vector, prtv, why);
          if (ok) {
            AppendBE32(prtv, rot.fourcc);
          }
        }
      } else if ((IsElement(rot, F("INTR")) || IsElement(rot, F("RLCL"))) && rot.args.empty()) {
        // GetRotationElement 0x25d59c: both are AddRefVar(ctx 0, rotation, 0xb), the system
        // rotation (INTR's when authored, else the external one): PRTV kind INTR.
        if (m_swoosh) {
          why = "ROTV by " + EffectFourCCString(rot.fourcc) + " in a swoosh";
        } else {
          AppendBE32(prtv, F("PRTV"));
          ok = Element(value.args[0], Type::Vector, prtv, why);
          if (ok) {
            AppendBE32(prtv, F("INTR"));
          }
        }
      } else {
        why = "ROTV by a rotation that is not REUL/EXTR/CMRT";
      }
      m_mp1Space = outer;
      if (!ok) {
        return false;
      }
      if (outer) {
        AppendBE32(out, F("BSIS"));
      }
      out.insert(out.end(), prtv.begin(), prtv.end());
      return true;
    }
    // SLTW(a, b) (GetLifetimeTweenElement 0x26a640, time slot 0) is the port-only PSLW: the two
    // reals, blended by the system tween.
    if (value.fourcc == F("SLTW") && type == Type::Real && value.args.size() == 2) {
      AppendBE32(out, F("PSLW"));
      return ShapedArgs(F("PSLW"), type, 0, value.args.data(), out, why);
    }
    // KEYF (version 2) is CREKeyframeInput 0x296490: the 30-byte header (version, an unused word,
    // loop and an unused byte, N at +10, loop start A, start B and rate R as floats, then the key
    // count) with N keys, and an input element. The port-only PKYF: the loop byte, N, A, B, R, the
    // N keys, then the input. Another version builds no element in Remastered. A vector KEYF
    // (CVEKeyframeInput 0x2a18fc, GetValue 0x2a19b8) is the same with three floats per key, in
    // Remastered's numbers like any vector tree.
    if (value.fourcc == F("KEYF") && (type == Type::Real || type == Type::Vector) && value.args.size() == 2 &&
        value.args[0].kind == EffectValue::Kind::Keys) {
      const uint8_t* p = m_data + value.args[0].offset;
      const size_t size = value.args[0].size;
      const size_t floats = type == Type::Vector ? 3 : 1;
      if (size < 30 || ReadLE32(p) != 2) {
        why = "KEYF of a version other than 2";
        return false;
      }
      const uint32_t n = ReadLE32(p + 10);
      if (n < 1 || n != ReadLE32(p + 26) || size - 30 != size_t(n) * 4 * floats) {
        why = "KEYF whose key count differs from N";
        return false;
      }
      AppendBE32(out, F("PKYF"));
      out.push_back(p[8] & 1);
      AppendBE32(out, n);
      AppendBE32(out, ReadLE32(p + 14));
      AppendBE32(out, ReadLE32(p + 18));
      AppendBE32(out, ReadLE32(p + 22));
      for (size_t i = 0; i < size_t(n) * floats; ++i) {
        AppendBE32(out, ReadLE32(p + 30 + i * 4));
      }
      return Element(value.args[1], Type::Real, out, why);
    }
    // KESP is KEYP on time slot 2 (CREKeyframeParticle 0x296430, time type 3): the port-only
    // KESP around a percent KEYP of its keys, read at the slot-2 key. Only an ELC2's UpdateBolt
    // 0xb77638 sets that slot (node over node count - 1); elsewhere it is 0, the first key.
    if (value.fourcc == F("KESP") && type == Type::Real && value.args.size() == 1 &&
        value.args[0].kind == EffectValue::Kind::Keys && value.args[0].size >= 26 &&
        ReadLE32(m_data + value.args[0].offset + 18) >= 1 &&
        value.args[0].size - 22 == size_t(ReadLE32(m_data + value.args[0].offset + 18)) * 4) {
      const uint8_t* p = m_data + value.args[0].offset;
      const uint32_t count = ReadLE32(p + 18);
      AppendBE32(out, F("KESP"));
      AppendBE32(out, F("KEYP"));
      AppendBE32(out, 1); // percent: the lifetime-key path
      AppendBE32(out, ReadLE32(p + 4));
      out.push_back(p[8]);
      out.push_back(p[9]);
      AppendBE32(out, ReadLE32(p + 10));
      AppendBE32(out, ReadLE32(p + 14));
      AppendBE32(out, count);
      for (uint32_t i = 0; i < count; ++i) {
        AppendBE32(out, ReadLE32(p + 22 + size_t(i) * 4));
      }
      return true;
    }
    if (value.fourcc == F("GRAD") && type == Type::Color) {
      return Gradient(value, out, why);
    }
    // ILPT(n) rounds n% of the lifetime as Remastered does: the port-only PLPT, where the
    // property's lifetime context is known (LifetimeOffset); elsewhere retail's ILPT.
    if (value.fourcc == F("ILPT") && type == Type::Int && value.args.size() == 1 && m_lifetimeOffset >= 0) {
      AppendBE32(out, F("PLPT"));
      AppendBE32(out, uint32_t(m_lifetimeOffset));
      return Element(value.args[0], Type::Int, out, why);
    }
    // KEWS: an LE u32 key count, then that many LE floats over a swoosh's
    // points (newest 0, oldest 1). Written as the port-only KEWS around percent
    // keyframes: CREPortSwooshNodeTime reads them at the point's node key
    // (Remastered 0x24f00c), while true KEYPs keep the point's age.
    if (value.fourcc == F("KEWS") && type == Type::Real && value.args.size() == 1 &&
        value.args[0].kind == EffectValue::Kind::Raw) {
      const uint8_t* p = m_data + value.args[0].offset;
      const size_t size = value.args[0].size;
      const uint32_t count = size >= 4 ? ReadLE32(p) : 0;
      if (count < 2 || 4 + size_t(count) * 4 > size) {
        why = "KEWS without its keys";
        return false;
      }
      AppendBE32(out, F("KEWS"));
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
    // SLCT(index, ARRY(entries)): Remastered's CElementArray<T>::GetValue (float
    // 0x27121c, CVector3f 0x27186c, CColor4f16 0x271ee4) evaluates the index,
    // takes the entry at index % count (C remainder) and evaluates it, each time
    // the value is read (per particle per frame for COLR). Written as the
    // port-only SLCT: the index element, the count, then the entries.
    if (value.fourcc == F("SLCT") && (type == Type::Real || type == Type::Vector || type == Type::Color) &&
        value.args.size() == 2 && IsElement(value.args[1], F("ARRY"))) {
      const std::vector<EffectValue>& array = value.args[1].args;
      if (array.size() < 2) {
        why = "SLCT over an ARRY whose entries do not parse";
        return false;
      }
      AppendBE32(out, F("SLCT"));
      if (!Element(value.args[0], Type::Int, out, why)) {
        return false;
      }
      AppendBE32(out, uint32_t(array.size() - 1));
      for (size_t i = 1; i < array.size(); ++i) {
        if (!Element(array[i], type, out, why)) {
          return false;
        }
      }
      return true;
    }
    // MPRD(a, b): a random int between two, as RAND.
    if (value.fourcc == F("MPRD") && value.args.size() == 2 && (type == Type::Int || type == Type::Real)) {
      AppendBE32(out, F("RAND"));
      return Element(value.args[0], type, out, why) && Element(value.args[1], type, out, why);
    }
    // Int RAND(min, max) (FUN_002709a4): min + Next() % (max - min + 1) (CIERandom 0x2a88a8,
    // CIERandomConstant 0x2a895c), the port-only PRAN; but with both constant and min < 0, raw
    // Next() (CIERandom2 0x2a89a0), which retail's RAND gives for a constant min <= 0.
    if (value.fourcc == F("RAND") && type == Type::Int && value.args.size() == 2) {
      auto literal = [](const EffectValue& c) {
        return IsElement(c, F("CNST")) && c.args.size() == 1 &&
               (c.args[0].kind == EffectValue::Kind::Word || c.args[0].kind == EffectValue::Kind::Byte);
      };
      const EffectValue& min = value.args[0];
      const EffectValue& max = value.args[1];
      const bool negative =
          literal(min) && min.args[0].kind == EffectValue::Kind::Word && int32_t(min.args[0].word) < 0;
      if ((!literal(min) && ConstantLeaves(min)) || (negative && !literal(max) && ConstantLeaves(max))) {
        why = "int RAND whose arguments may fold to constants";
        return false;
      }
      const bool rawNext = negative && literal(max);
      AppendBE32(out, rawNext ? F("RAND") : F("PRAN"));
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
    // out under load). Remastered's value is exactly 1.0 whenever the GPU frame
    // time is at or below 14 ms, so 1 is exact on a healthy frame and is not
    // reported as approximated; only the load-shedding below 1 is not ported.
    if (value.fourcc == F("GPUA") && value.args.empty() && type == Type::Real) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(1.0f));
      return true;
    }
    // A parent's variable (IsParentRead), as the port-only element of the same
    // fourcc: n (SPA*) or the guid (DPV*/TPV*), then the fallback. The fallback
    // is read only when the parent has no such variable, and only a constant
    // one (CREParentAdditionalValue::GetValue 0x296824); any other reads 0. An
    // n above 9 is never a variable (0xff).
    if (value.args.size() == 2 && IsParentRead(value.fourcc, type)) {
      const bool slot = (value.fourcc & 0xffffff00u) == (F("SPAF") & 0xffffff00u);
      if (slot ? (value.args[0].kind != EffectValue::Kind::Byte && value.args[0].kind != EffectValue::Kind::Word)
               : value.args[0].kind != EffectValue::Kind::Guid) {
        why = EffectFourCCString(value.fourcc) + " without its variable";
        return false;
      }
      AppendBE32(out, value.fourcc);
      if (slot) {
        AppendBE32(out, value.args[0].word > 9 ? 0xffu : value.args[0].word);
      } else {
        out.insert(out.end(), value.args[0].guid.begin(), value.args[0].guid.end());
      }
      if (IsConstantValue(value.args[1])) {
        return Element(value.args[1], type, out, why);
      }
      Zero(type, out);
      return true;
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
    // A mod vector's CRLN: the parse reads the V child's own CRLN header as the first argument
    // and its fields as siblings (header, VECF, P, F, octaves, octave real, A, mode, t).
    if (value.fourcc == F("CRLN") && type == Type::ModVector) {
      return CurlNoise(value, out, why);
    }
    // SCLE(v, r) is CVEScale 0x29e374: v, then r, out = (v.x * r, r * v.y, r * v.z). Retail's
    // MULT(v, RTOV(r)) is the same products in the same order (v then r drawn), and a uniform
    // scale commutes with the particle-space change, so v stays in Remastered's numbers here.
    if (value.fourcc == F("SCLE") && type == Type::Vector && value.args.size() == 2) {
      AppendBE32(out, F("MULT"));
      if (!Element(value.args[0], Type::Vector, out, why)) {
        return false;
      }
      AppendBE32(out, F("RTOV"));
      return Element(value.args[1], Type::Real, out, why);
    }
    // A swoosh's elements run on the swoosh's own frame, which SUST (0x298ef8) selects for the
    // system frame's child anyway, so SUST(x) is x there.
    if (value.fourcc == F("SUST") && type == Type::Real && value.args.size() == 1 && m_swoosh) {
      return Element(value.args[0], type, out, why);
    }
    // SDTR (CREShutdownTimeRemaining 0x2999e0) is the shutdown countdown's remaining over total
    // (1 with none running): the port-only PSDT, read off the generator's SHTT countdown. A
    // swoosh or electric has none in the port.
    if (value.fourcc == F("SDTR") && type == Type::Real) {
      if (!m_gpsm || !value.args.empty()) {
        why = "SDTR outside a generator needs Remastered's shutdown countdown";
        return false;
      }
      AppendBE32(out, F("PSDT"));
      return true;
    }
    // VISQ (CREVisibilityQuery 0x299790) is the system's occlusion-query visibility, 1 before
    // any result; its int (0x2619e8) is read and unused. A generator's port-only PVSQ; a swoosh
    // or electric issues no queries in the port.
    if (value.fourcc == F("VISQ") && type == Type::Real) {
      if (!m_gpsm || value.args.size() != 1) {
        why = "VISQ outside a generator needs Remastered's occlusion queries";
        return false;
      }
      AppendBE32(out, F("PVSQ"));
      return true;
    }
    // A real GTCP (CREGetTotalCreatedParticleCount 0x297434) is the generator's total created
    // particles (vfunc +0x50, GetTotalParticlesCreated 0x24db30) as a float, 0 with none. In a
    // generator's EMTR it is the port-only PGTC (one less than retail's count, which Remastered
    // raises only at the creation loop's tail 0x24c1c8); elsewhere that offset is unverified.
    if (value.fourcc == F("GTCP") && type == Type::Real && value.args.empty()) {
      if (!m_gpsm || !m_emtr) {
        why = "a real GTCP outside a generator's EMTR";
        return false;
      }
      AppendBE32(out, F("PGTC"));
      return true;
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
    // A mod vector's or emitter's vectors are used in the port's space.
    const bool portVectors = (type == Type::ModVector && !m_mp1Space) || type == Type::Emitter;
    for (size_t i = 0; i < sig.size(); ++i) {
      const bool ok = sig[i] == 'B'                   ? Bool(value.args[i], out, why)
                      : sig[i] == 'v' && portVectors ? PortVector(value.args[i], out, why)
                                                      : Element(value.args[i], TypeOfLetter(sig[i]), out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // The grid of a sectioned element: the argument indices of its counts (one count: both the
  // same), the counts, whether they were resolved to constants, and the shuffler's size (0 none).
  struct Sections {
    size_t at[2] = {0, 0};
    uint32_t counts[2] = {1, 1};
    bool resolved = false;
    uint32_t shuffle = 0;
  };

  // A 2d sectioned emitter's (ASPV, PLNV, SPEV) counts at args `at1` and `at2` and shuffle flag at
  // `flagAt`. The last byte it writes is the size of Remastered's index shuffler, 0 for none. The
  // grid is what CSectionedElementData::Compile2d 0x2a6b90 leaves after Init 0x2a69b8, which runs
  // at the generator's _END:
  // - no count a constant 0: Init's counts stand, and a shuffler exists when the flag is set
  //   (a count that is not constant does not stop it), sized by the product of the constant
  //   counts (a count that is not constant is 1) through SetUpShuffledIndexing 0x26ffe4: below
  //   2 it is 1, and the size is kept in a byte;
  // - a constant 0 count: the counts come from the owning generator's MAXP (SectionCompile2d),
  //   written as constants, and the flag sets a shuffler of max(n1 * n2, 1) cells even at 1
  //   (it still draws). A MAXP or a second count that is not a constant stays dropped.
  // Without a shuffler the index is the creation count modulo the grid.
  bool Sectioned2d(const char* name, const EffectValue& value, size_t at1, size_t at2, size_t flagAt, Sections& grid,
                   std::string& why) const {
    const std::string element = name;
    const EffectValue& flag = value.args[flagAt];
    if (flag.kind != EffectValue::Kind::Byte && flag.kind != EffectValue::Kind::Word) {
      why = element + " with a shuffle flag that is not a byte";
      return false;
    }
    grid.at[0] = at1;
    grid.at[1] = at2;
    bool known[2] = {false, false};
    bool anyZero = false;
    for (int i = 0; i < 2; ++i) {
      known[i] = ConstWord(value.args[grid.at[i]], grid.counts[i]);
      if (known[i] && grid.counts[i] > 255) {
        why = element + " with a constant count over 255 (Remastered keeps it in a byte)";
        return false;
      }
      anyZero = anyZero || (known[i] && grid.counts[i] == 0);
    }
    if (anyZero) {
      if (!known[0] || !known[1]) {
        why = element + " with a constant 0 count and a count that is not a constant";
        return false;
      }
      if (m_maxp < 0) {
        why = element + " with a constant 0 count and a MAXP that is not a constant";
        return false;
      }
      const PortParticleMath::SectionCounts counts = PortParticleMath::SectionCompile2d(
          int(grid.counts[0]), int(grid.counts[1]), grid.counts[0] == 0, grid.counts[1] == 0,
          PortParticleMath::SectionTotal(int32_t(uint32_t(m_maxp))));
      grid.counts[0] = uint32_t(counts.n1);
      grid.counts[1] = uint32_t(counts.n2);
      grid.resolved = true;
      if (flag.word == 0) {
        return true;
      }
    } else if (flag.word != 0) {
      // Init 0x2a68dc sizes the shuffler from the count's value, which a count that is not a
      // constant does not give it (the size is not reproduced).
      if (!known[0] || !known[1]) {
        why = element + " with a shuffler and a count that is not a constant";
        return false;
      }
    } else {
      return true;
    }
    grid.shuffle = uint32_t(std::max<uint64_t>(uint64_t(grid.counts[0]) * grid.counts[1], 1) & 0xff);
    if (grid.shuffle == 0) {
      why = element + " with a shuffler of a multiple of 256 cells";
      return false;
    }
    return true;
  }

  // A 1d sectioned vector's (CRCV, LINV) count at arg `at` and shuffle flag at `flagAt`.
  // CSectionedElementData::Init 0x2a68dc builds a shuffler only for a constant nonzero count (a
  // count that is not a constant has none), sized by it (below 2 it is 1, kept in a byte); a
  // constant 0 count is Compile1d 0x2a6ae4's: the count becomes the owning generator's MAXP
  // (clamped 1 to 128) and the flag sets a shuffler of that many cells. A MAXP that is not a
  // constant there stays dropped.
  bool Sectioned1d(const char* name, const EffectValue& value, size_t at, size_t flagAt, Sections& grid,
                   std::string& why) const {
    const std::string element = name;
    const EffectValue& flag = value.args[flagAt];
    if (flag.kind != EffectValue::Kind::Byte && flag.kind != EffectValue::Kind::Word) {
      why = element + " with a shuffle flag that is not a byte";
      return false;
    }
    grid.at[0] = grid.at[1] = at;
    uint32_t& count = grid.counts[0];
    count = 0;
    const bool known = ConstWord(value.args[at], count);
    if (known && count == 0) {
      // Compile1d runs only for the vector SEmitter::CompileSystem 0x28e00c compiles
      // (vt[0x130] on the SEMR's position child; IElement::Compile 0x250830 does not recurse).
      if (&value != m_semrPos) {
        why = element + " with a constant 0 count that is not a SEMR's position";
        return false;
      }
      if (m_maxp < 0) {
        why = element + " with a constant 0 count and a MAXP that is not a constant";
        return false;
      }
      count = uint32_t(PortParticleMath::SectionTotal(int32_t(uint32_t(m_maxp))));
      grid.resolved = true;
      grid.shuffle = flag.word != 0 ? count & 0xff : 0;
    } else if (known) {
      if (count > 255) {
        why = element + " with a constant count over 255 (Remastered keeps it in a byte)";
        return false;
      }
      if (flag.word != 0) {
        grid.shuffle = std::max<uint32_t>(count, 1) & 0xff;
      }
    }
    return true;
  }

  // Writes args [from, to) as the elements `shift` places on in `fourcc`'s shape, a resolved count
  // as its constant, then the shuffle byte.
  bool SectionedArgs(uint32_t fourcc, Type type, const std::vector<EffectValue>& args, size_t from, size_t to,
                     size_t shift, const Sections& grid, std::vector<uint8_t>& out, std::string& why) const {
    for (size_t i = from; i < to; ++i) {
      if (grid.resolved && (i == grid.at[0] || i == grid.at[1])) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, grid.counts[i == grid.at[0] ? 0 : 1]);
        continue;
      }
      if (!Element(args[i], ArgType(fourcc, type, i + shift), out, why)) {
        return false;
      }
    }
    out.push_back(uint8_t(grid.shuffle));
    return true;
  }

  // The port-only direction elements (ANCR, ANCM, ANCV, MPAC, MPCN, RNDV). std::nullopt when `value` is none of them.
  std::optional<bool> DirectionElement(const EffectValue& value, Type type, std::vector<uint8_t>& out,
                                     std::string& why) const {
    // ANCR(REUL(x, y, z, #00), x range, y range, magnitude): the port-only ANCR
    // (x, y, z, x range, y range, magnitude) runs CVEAngleCone 0x29eebc as is, in
    // Remastered's space like the tree around it.
    if (value.fourcc == F("ANCR") && type == Type::Vector && value.args.size() == 4) {
      AppendBE32(out, F("ANCR"));
      return ConeRotation(value.args[0], out, why) && ShapedArgs(F("ANCR"), type, 3, &value.args[1], out, why);
    }
    // ANCM(REUL, A, B, magnitude): the port-only PANM runs CVEAngleConeMinMax 0x29f304 as is,
    // the rotation as its three angles like ANCR's.
    if (value.fourcc == F("ANCM") && type == Type::Vector && value.args.size() == 4) {
      AppendBE32(out, F("PANM"));
      return ConeRotation(value.args[0], out, why) && ShapedArgs(F("PANM"), type, 3, &value.args[1], out, why);
    }
    // ANCV(REUL, A, B, magnitude, polar count, polar variance, azimuth count, azimuth variance,
    // shuffle): the port-only PANV (CVEAngleConeMinMaxWithVariance::GetValue 0x29f5c0), the REUL as
    // three angles like PANM's. Init 0x2a69b8 runs as for ASPV, but its Compile 0x29fa1c
    // (Compile2d) only for a SEMR's position: elsewhere a constant 0 count stays 0 (GetValue makes
    // it 1) and has no shuffler.
    if (value.fourcc == F("ANCV") && type == Type::Vector && value.args.size() == 9) {
      Sections grid;
      uint32_t counts[2] = {1, 1};
      const bool known[2] = {ConstWord(value.args[4], counts[0]), ConstWord(value.args[6], counts[1])};
      if (&value != m_semrPos && ((known[0] && counts[0] == 0) || (known[1] && counts[1] == 0))) {
        const EffectValue& flag = value.args[8];
        if (flag.kind != EffectValue::Kind::Byte && flag.kind != EffectValue::Kind::Word) {
          why = "ANCV with a shuffle flag that is not a byte";
          return false;
        }
        if ((known[0] && counts[0] > 255) || (known[1] && counts[1] > 255)) {
          why = "ANCV with a constant count over 255 (Remastered keeps it in a byte)";
          return false;
        }
      } else if (!Sectioned2d("ANCV", value, 4, 6, 8, grid, why)) {
        return false;
      }
      AppendBE32(out, F("PANV"));
      // The arguments after the REUL sit two places on in PANV's shape (the REUL is three angles).
      return ConeRotation(value.args[0], out, why) && SectionedArgs(F("PANV"), type, value.args, 1, 8, 2, grid, out, why);
    }
    // MPAC(x centre, y centre, x range, y range, magnitude): the port-only PMAC runs
    // MPT_CVEAngleCone-like 0x29fec0 as is, a direction in retail's space (the MPCB around it
    // turns it to Remastered's).
    if (value.fourcc == F("MPAC") && type == Type::Vector && value.args.size() == 5) {
      AppendBE32(out, F("PMAC"));
      return ShapedArgs(F("PMAC"), type, 0, value.args.data(), out, why);
    }
    // MPCN(direction, radius): the port-only PMCN runs MPT_CVECone 0x29eca0 as is.
    if (value.fourcc == F("MPCN") && type == Type::Vector && value.args.size() == 2) {
      AppendBE32(out, F("PMCN"));
      return ShapedArgs(F("PMCN"), type, 0, value.args.data(), out, why);
    }
    // RNDV(magnitude): the port-only RNDV runs CVERandomVector 0x29e954 as is
    // (a cube point normalised by the ARM rsqrt estimate, so not uniform).
    if (value.fourcc == F("RNDV") && type == Type::Vector && value.args.size() == 1) {
      AppendBE32(out, F("RNDV"));
      return ShapedArgs(F("RNDV"), type, 0, value.args.data(), out, why);
    }
    return std::nullopt;
  }

  // The emitter shapes (ASPR, ASPV, PLNE, PLNV, SPEV, ELPS). std::nullopt when `value` is none of them.
  std::optional<bool> EmitterShape(const EffectValue& value, Type type, std::vector<uint8_t>& out,
                                 std::string& why) const {
    // ASPR(origin, REUL, range 1, range 2, radius, speed): the port-only ASPR
    // (origin, x, y, z, range 1, range 2, radius, speed) runs CVEAngleSphere
    // 0x2cb100 as is and hands the port its position and velocity.
    if (value.fourcc == F("ASPR") && type == Type::Emitter && value.args.size() == 6) {
      AppendBE32(out, F("ASPR"));
      return PortVector(value.args[0], out, why) && ConeRotation(value.args[1], out, why) &&
             ShapedArgs(F("ASPR"), type, 4, &value.args[2], out, why);
    }
    // ASPV(origin, REUL, A, B, polar count, polar variance, azimuth range, azimuth count, azimuth
    // variance, radius, speed, shuffle): the port-only PASV (CVEAngleSphereWithVariance::GetValue
    // 0x2cb6b0), the REUL as three angles like ASPR's. The counts and the shuffler are SPEV's
    // (Compile2d 0x2a6b90 after Init 0x2a69b8, as CVEAngleSphereWithVariance::Compile 0x2cbc08).
    if (value.fourcc == F("ASPV") && type == Type::Emitter && value.args.size() == 12) {
      Sections grid;
      if (!Sectioned2d("ASPV", value, 4, 7, 11, grid, why)) {
        return false;
      }
      AppendBE32(out, F("PASV"));
      if (!PortVector(value.args[0], out, why) || !ConeRotation(value.args[1], out, why)) {
        return false;
      }
      // The arguments after the REUL sit two places on in PASV's shape (the REUL is three angles).
      return SectionedArgs(F("PASV"), type, value.args, 2, 11, 2, grid, out, why);
    }
    // PLNE(position, euler, velocity, wx, wz, spread): the plane emitter. The port's PLNE builds
    // its matrix from the euler and the spread when it is made, so those two must be constants;
    // the arguments stay in Remastered's axes and the emitter turns its outputs to the port's.
    if (value.fourcc == F("PLNE") && type == Type::Emitter && value.args.size() == 6) {
      if (!IsConstantValue(value.args[1]) || !IsConstantValue(value.args[5])) {
        why = "PLNE with a euler or spread that is not a constant";
        return false;
      }
      AppendBE32(out, F("PLNE"));
      return ShapedArgs(F("PLNE"), type, 0, value.args.data(), out, why);
    }
    // PLNV(position, euler, velocity y, wx, wz, spread, x count, x variance, z count, z variance,
    // shuffle): the port-only PLNV, the same way, its counts and shuffler as Sectioned2d sets them.
    if (value.fourcc == F("PLNV") && type == Type::Emitter && value.args.size() == 11) {
      if (!IsConstantValue(value.args[1]) || !IsConstantValue(value.args[5])) {
        why = "PLNV with a euler or spread that is not a constant";
        return false;
      }
      Sections grid;
      if (!Sectioned2d("PLNV", value, 6, 8, 10, grid, why)) {
        return false;
      }
      AppendBE32(out, F("PLNV"));
      return SectionedArgs(F("PLNV"), type, value.args, 0, 10, 0, grid, out, why);
    }
    // SPEV(position, radius, speed, n1, variance 1, n2, variance 2, shuffle): the port-only PSPV,
    // counts and shuffler as for PLNV (Compile2d 0x2a6b90 after Init 0x2a69b8), the position
    // given in Remastered's axes.
    if (value.fourcc == F("SPEV") && type == Type::Emitter && value.args.size() == 8) {
      Sections grid;
      if (!Sectioned2d("SPEV", value, 3, 5, 7, grid, why)) {
        return false;
      }
      AppendBE32(out, F("PSPV"));
      return SectionedArgs(F("PSPV"), type, value.args, 0, 7, 0, grid, out, why);
    }
    // ELPS(centre, size, euler, speed, surface only): the port-only PELP, which has no rotation,
    // so the euler must be a constant (0, 0, 0). Only the surface-only byte follows.
    if (value.fourcc == F("ELPS") && type == Type::Emitter && value.args.size() == 5) {
      // The surface-only flag is a GetBool: the files give it as CNST(byte), so unwrap that.
      const EffectValue& flag =
          IsElement(value.args[4], F("CNST")) && value.args[4].args.size() == 1 ? value.args[4].args[0] : value.args[4];
      if (flag.kind != EffectValue::Kind::Byte && flag.kind != EffectValue::Kind::Word) {
        why = "ELPS with a surface-only flag that is not a byte";
        return false;
      }
      if (!IsZeroVector(value.args[2])) {
        why = "ELPS with a euler that is not a constant (0, 0, 0)";
        return false;
      }
      AppendBE32(out, F("PELP"));
      if (!Element(value.args[0], Type::Vector, out, why) || !Element(value.args[1], Type::Vector, out, why) ||
          !Element(value.args[3], Type::Real, out, why)) {
        return false;
      }
      out.push_back(flag.word != 0 ? 1 : 0);
      return true;
    }
    return std::nullopt;
  }

  // The 1d sectioned vectors (CRCV, LINV). std::nullopt when `value` is none of them.
  std::optional<bool> SectionedVector(const EffectValue& value, Type type, std::vector<uint8_t>& out,
                                    std::string& why) const {
    // CRCV(centre, axis, start degrees, count, variance, radius, shuffle): the port-only PCRC, its
    // count and shuffler as Sectioned1d sets them. The axis is read once, so it must be a constant.
    if (value.fourcc == F("CRCV") && type == Type::Vector && value.args.size() == 7) {
      if (!IsConstantValue(value.args[1])) {
        why = "CRCV with an axis that is not a constant";
        return false;
      }
      Sections grid;
      if (!Sectioned1d("CRCV", value, 3, 6, grid, why)) {
        return false;
      }
      AppendBE32(out, F("PCRC"));
      return SectionedArgs(F("PCRC"), type, value.args, 0, 6, 0, grid, out, why);
    }
    // LINV(start, end, count, variance, shuffle): the port-only PLWV (CVELineWithVariance::GetValue
    // 0x2a119c), in Remastered's numbers like PCRC. The shuffler and a constant 0 count are CRCV's
    // (Init 0x2a68dc; Compile1d 0x2a6ae4, which CVELineWithVariance::Compile 0x2a1418 also calls,
    // only for a SEMR's position).
    if (value.fourcc == F("LINV") && type == Type::Vector && value.args.size() == 5) {
      Sections grid;
      if (!Sectioned1d("LINV", value, 2, 4, grid, why)) {
        return false;
      }
      AppendBE32(out, F("PLWV"));
      return SectionedArgs(F("PLWV"), type, value.args, 0, 4, 0, grid, out, why);
    }
    return std::nullopt;
  }

  // CMVECurlNoise(V, mode, t) as the port-only PCRM (mode, V, t) around V's curl-noise evaluator
  // as the port-only PCRN (P, F, A, then the field: NONE, or CNST and the VECF's id), Remastered's
  // CVECurlNoise_Perlin3d (slice 0, no octaves), the 2d slices as the port-only PCRS and slice 0
  // with octaves as PCRO. The octave slices are not ported. The noise's vectors are in
  // Remastered's numbers; V, like any mod-vector argument, is turned into the port's space
  // around it.
  bool CurlNoise(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    const std::vector<EffectValue>& a = value.args;
    const auto isByte = [](const EffectValue& v) {
      return v.kind == EffectValue::Kind::Byte || v.kind == EffectValue::Kind::Word;
    };
    if (a.size() != 9 || !IsElement(a[0], F("CRLN")) || a[0].args.size() != 2 || !isByte(a[0].args[1]) ||
        !IsElement(a[1], F("VECF")) || a[1].args.size() != 1 || a[1].args[0].kind != EffectValue::Kind::Guid ||
        !isByte(a[7])) {
      why = "CRLN in a form not ported";
      return false;
    }
    const uint32_t slice = a[0].args[1].word;
    if (slice > 3 || (slice != 0 && !IsElement(a[4], F("NONE")))) {
      why = "CRLN slice " + std::to_string(slice) + (slice > 3 ? "" : " with octaves") + " not ported";
      return false;
    }
    if (slice != 0) {
      // ReadCurlNoiseVectorParticleEvaluator 0x26ea20 releases the VECF of a slice.
      std::vector<uint8_t> noise;
      AppendBE32(noise, F("PCRS"));
      AppendBE32(noise, slice);
      if (!Element(a[2], Type::Vector, noise, why) || !Element(a[3], Type::Real, noise, why) ||
          !Element(a[6], Type::Vector, noise, why)) {
        return false;
      }
      AppendBE32(out, F("PCRM"));
      AppendBE32(out, a[7].word);
      if (m_mp1Space) {
        out.insert(out.end(), noise.begin(), noise.end());
      } else {
        Basis(F("BSIS"), noise, out);
      }
      return Element(a[8], Type::Real, out, why);
    }
    if (!IsElement(a[4], F("NONE"))) {
      // Octaves: the port-only PCRO (P, freq, octaves, gain, A). The VECF is released in
      // Remastered's octave class, so it is not carried.
      std::vector<uint8_t> octave;
      AppendBE32(octave, F("PCRO"));
      if (!ShapedArgs(F("PCRO"), Type::Vector, 0, &a[2], octave, why)) {
        return false;
      }
      AppendBE32(out, F("PCRM"));
      AppendBE32(out, a[7].word);
      if (m_mp1Space) {
        out.insert(out.end(), octave.begin(), octave.end());
      } else {
        Basis(F("BSIS"), octave, out);
      }
      return Element(a[8], Type::Real, out, why);
    }
    std::vector<uint8_t> noise;
    AppendBE32(noise, F("PCRN"));
    if (!Element(a[2], Type::Vector, noise, why) || !Element(a[3], Type::Real, noise, why) ||
        !Element(a[6], Type::Vector, noise, why)) {
      return false;
    }
    const EffectGuid& field = a[1].args[0].guid;
    if (field == EffectGuid{}) {
      AppendBE32(noise, F("NONE"));
    } else {
      const uint32_t id = AssetId(field, F("VECF"));
      if (id == 0) {
        why = "CRLN vector field " + EffectGuidString(field) + " not imported";
        return false;
      }
      AppendBE32(noise, F("CNST"));
      AppendBE32(noise, id);
    }
    AppendBE32(out, F("PCRM"));
    AppendBE32(out, a[7].word);
    if (m_mp1Space) {
      out.insert(out.end(), noise.begin(), noise.end());
    } else {
      Basis(F("BSIS"), noise, out);
    }
    return Element(a[8], Type::Real, out, why);
  }

  // A cone's REUL(x, y, z, #00) as its three angle reals, which the port's ANCR and ASPR turn
  // into Remastered's order-0 quaternion themselves.
  bool ConeRotation(const EffectValue& reul, std::vector<uint8_t>& out, std::string& why) const {
    if (!IsElement(reul, F("REUL")) || reul.args.size() != 4) {
      why = "a cone rotation that is not REUL";
      return false;
    }
    const EffectValue& order = reul.args[3];
    if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
      why = "REUL in an unseen axis order";
      return false;
    }
    for (size_t i = 0; i < 3; ++i) {
      if (!Element(reul.args[i], Type::Real, out, why)) {
        return false;
      }
    }
    return true;
  }

  // The Remastered particle basis B = (90, 0, 180) in PMRT's Euler convention: (-x, z, y), the
  // model converter's default basis. B is self-inverse.
  static void AppendRotationBasis(std::vector<uint8_t>& out) {
    AppendBE32(out, F("CNST"));
    for (const float angle : {90.f, 0.f, 180.f}) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, FloatBits(angle));
    }
  }

  // PMDL meshes are converted from Remastered coordinates with the model
  // converter's basis B. Conjugate the source rotation by B so it remains in the
  // converted model's frame: PQAD(B, PQAD(R, B)) is B * R * B^-1. R is
  // written as an Euler vector by SourceRotation; Element() preserves each real
  // expression, including nested IRND, and the PART's PIRN marker keeps those
  // samples stable per particle. A top-level MPCB(R) is B * R * B already
  // (0x29d69c), so it conjugates back to R.
  bool ModelRotation(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.size() != 1) {
      why = "a rotation that is not REUL/RADD/RAZY/RAYZ/MPCB/EXTR/CMRT";
      return false;
    }
    if (IsElement(value[0], F("MPCB")) && value[0].args.size() == 1) {
      return SourceRotation(value[0].args[0], out, why);
    }
    AppendBE32(out, F("PQAD"));
    AppendRotationBasis(out);
    AppendBE32(out, F("PQAD"));
    if (!SourceRotation(value[0], out, why)) {
      return false;
    }
    AppendRotationBasis(out);
    return true;
  }

  // A Remastered rotation element in Remastered's particle space, as a port Euler vector:
  // REUL(x, y, z, #00) -> CNST(x, y, z); RADD(a, b) -> PQAD(a, b) (the ordered product);
  // RAZY(first, second, fallback) -> the port-only PRZY (CERotationFromAxis 0x29d1bc as
  // GetRotationElement 0x25e418 builds it: Z = first, X = second x Z, Y = Z x X, or the
  // fallback when either is degenerate); RAYZ the same way as PRYZ (0x25e068: Y = first,
  // X = Y x second, Z = X x Y). Their vectors stay in Remastered's space.
  // MPCB(r) -> PQAD(B, PQAD(r, B)) (qB * r * qB, 0x29d69c); EXTR (AddRefVar(0, 4, 10),
  // GetRotationElement 0x25d59c) -> the port-only EXTR vector, the system orientation.
  // `keep`: the rotation sits under a KPIN (CERotationKeepInitial 0x29c214, the child's value at
  // time 0 and 0 after), so it is pushed onto the three angles of each REUL, which a system
  // variable turns into init-only hidden reals (KPIN(REUL(a, b, c)) = REUL(KPIN a, KPIN b, KPIN c)).
  bool SourceRotation(const EffectValue& rot, std::vector<uint8_t>& out, std::string& why, bool keep = false) const {
    if (IsElement(rot, F("KPIN")) && rot.args.size() == 1 && m_sysItems != nullptr) {
      return SourceRotation(rot.args[0], out, why, true);
    }
    if (IsElement(rot, F("REUL")) && rot.args.size() == 4) {
      const EffectValue& order = rot.args[3];
      if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
        why = "a rotation order other than 0 (XYZ)";
        return false;
      }
      AppendBE32(out, F("CNST"));
      for (size_t i = 0; i < 3; ++i) {
        // A constant angle is its own initial value; any other is kept through a KPIN.
        if (keep && (ConstantFlags(rot.args[i], Type::Real) & 3) == 0) {
          EffectValue kept;
          kept.fourcc = F("KPIN");
          kept.args.push_back(rot.args[i]);
          if (!Element(kept, Type::Real, out, why)) {
            return false;
          }
        } else if (!Element(rot.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    if (IsElement(rot, F("RADD")) && rot.args.size() == 2) {
      AppendBE32(out, F("PQAD"));
      return SourceRotation(rot.args[0], out, why, keep) && SourceRotation(rot.args[1], out, why, keep);
    }
    if ((IsElement(rot, F("RAZY")) || IsElement(rot, F("RAYZ"))) && rot.args.size() == 3) {
      AppendBE32(out, IsElement(rot, F("RAZY")) ? F("PRZY") : F("PRYZ"));
      return Element(rot.args[0], Type::Vector, out, why) && Element(rot.args[1], Type::Vector, out, why) &&
             SourceRotation(rot.args[2], out, why);
    }
    if (IsElement(rot, F("MPCB")) && rot.args.size() == 1) {
      AppendBE32(out, F("PQAD"));
      AppendRotationBasis(out);
      AppendBE32(out, F("PQAD"));
      if (!SourceRotation(rot.args[0], out, why, keep)) {
        return false;
      }
      AppendRotationBasis(out);
      return true;
    }
    if (!keep && IsElement(rot, F("EXTR")) && rot.args.empty()) {
      AppendBE32(out, F("EXTR"));
      return true;
    }
    // CMRT: the game camera's rotation (CERotationFromCamera 0x29d180), never constant, so a
    // system variable holding it is re-evaluated every tick (CAddVarSystem::Update).
    if (!keep && IsElement(rot, F("CMRT")) && rot.args.empty()) {
      AppendBE32(out, F("CMRT"));
      return true;
    }
    why = "a rotation that is not REUL/RADD/RAZY/RAYZ/MPCB/EXTR/CMRT";
    return false;
  }

  // GRAD: u8 stop count, per stop four halves (RGBA) and an f32 position,
  // then what the position runs over: ILPT(CNST(n)) (n% of the particle's
  // life) or CNST(n) (n frames), then a byte (1: the gradient repeats).
  // Written as the port-only PGRD element, which evaluates Remastered's CCEGradient (0x2c8d48)
  // exactly on the age in frames Remastered feeds it: a particle's age (UpdateExistingParticles
  // 0x24a26c) or a swoosh segment's (UpdateExisting 0x27d590). The period stays the source's own
  // int element: CNST n, or ILPT(CNST n) as the port-only PLPT (Remastered's rounding).
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
    // One stop and no period (CCEFastConstant: 13 bytes, no tail) is that stop's colour, whatever
    // the age: a constant colour (CCEConstant, four real CNSTs), the halves decoded exactly.
    if (stops == 1 && size == 13) {
      AppendBE32(out, F("CNST"));
      for (size_t c = 0; c < 4; ++c) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, FloatBits(HalfToFloat(uint16_t(p[1 + 2 * c] | p[2 + 2 * c] << 8))));
      }
      return true;
    }
    AppendBE32(out, F("PGRD"));
    AppendBE32(out, uint32_t(stops));
    for (size_t i = 0; i < stops; ++i) {
      for (size_t c = 0; c < 4; ++c) {
        AppendBE32(out, FloatBits(HalfToFloat(uint16_t(p[1 + 12 * i + 2 * c] | p[2 + 12 * i + 2 * c] << 8))));
      }
      AppendBE32(out, ReadLE32(p + 1 + 12 * i + 8));  // the f32 position, bits unchanged
    }
    if (at + 12 <= size && ReadLE32(p + at) == F("ILPT") && ReadLE32(p + at + 4) == F("CNST")) {
      if (m_lifetimeOffset < 0) {
        m_approximated.push_back("GRAD over the life taken over the port's lifetime (context unverified)");
      }
      AppendBE32(out, F("PLPT"));
      AppendBE32(out, uint32_t(std::max(m_lifetimeOffset, 0)));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, ReadLE32(p + at + 8));
      at += 12;
    } else if (at + 8 <= size && ReadLE32(p + at) == F("CNST")) {
      AppendBE32(out, F("CNST"));
      AppendBE32(out, ReadLE32(p + at + 4));
      at += 8;
    } else {
      why = "GRAD over something other than the life or frames";
      return false;
    }
    out.push_back(at < size && p[at] != 0 ? 1 : 0);
    return true;
  }

  // What the port's particle lifetime needs added to be the one an ILPT in this property reads,
  // or -1 where the port's evaluation context is unverified. Remastered's ILPT (CREParticleLifetime
  // 0x29694c) reads the CVarContext slot the property's tag picks: tag -> EVarContextTime by the
  // table at 0x1d129b4 (1 -> 0, 3 -> 2, 5 -> 4), then GetDefaultVarContextTime 0x2513dc (2 -> slot
  // 1, 4 -> slot 2, else 0). Slot 0 is the system's PSLT (CElementGen ctor 0x244cfc), as the port's
  // light update has it; slot 1 a particle's LTM2 (0x24c0dc), one above the port's LTME (Property),
  // or a swoosh's life, which the port's swoosh lifetime equals.
  // An ELC2's COLR (tag 3) reads slot 1 = the bolt's end - start (UpdateBolt 0xb77638 sets ctx+0x70
  // = param_2[3] - param_2[2] and calls the colour element with the bolt's age), the lifetime the
  // port's bolt sets, so its offset is 0.
  // A generator's or swoosh's own SVCn/SVRn/SVIn/SVVn (CAddVarSystem's variables) is evaluated by
  // CAddVarSystem::Init/Update 0x278438/0x278638 on the system's context, so its tag 1 is slot 0 =
  // the system's PSLT, which CElementGen/CParticleSwoosh::PortEvalSys put in CParticleGlobals: offset 0.
  static int LifetimeOffset(bool part, bool swoosh, bool electric, const EffectProperty& property) {
    if (electric && property.tag == 3 && property.fourcc == F("COLR")) {
      return 0;
    }
    // An ELC2's NCLR (tag 5) reads slot 2 = nodes - 1 (UpdateBolt 0xb77638: ctx+0x84 = N-1,
    // ctx+0x88 = (float)(N-1)); CParticleElectric2Port sets SSEG - 1 as the lifetime around the
    // NCLR evaluation, so its offset is 0.
    if (electric && property.tag == 5 && property.fourcc == F("NCLR")) {
      return 0;
    }
    if (part && property.tag == 1 && SystemVarWrite(property.fourcc).first != ~0u) {
      return 0;
    }
    if (part && property.tag == 3) {
      return 1;
    }
    if (part && property.tag == 1 && property.fourcc == F("LCLR")) {
      return 0;
    }
    if (swoosh && property.tag == 3 && property.fourcc == F("COLR")) {
      return 0;
    }
    // A swoosh's PMTR (tag 3) reads slot 1 = LENG - 1 (CParticleSwoosh::Update 0x27dcf4,
    // CreateNewParticle 0x27cdc8), the lifetime the port's swoosh sets (build/s-hdr14-20261024/SPEC-grad.md).
    if (swoosh && property.tag == 3 && property.fourcc == F("PMTR")) {
      return 0;
    }
    return -1;
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
  // the particle's life); PAFB: ATFB (an array texture over a cycle, mirrored as
  // TXFB). Only those patterns.
  bool Atlas(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue& head = value[0];
    const std::string name = EffectFourCCString(head.fourcc);
    if (head.args.empty() || head.args[0].kind != EffectValue::Kind::Guid) {
      why = name + " without a texture";
      return false;
    }
    uint32_t id = 0;
    int32_t cols = 0, rows = 0, count = 0, mode = 0, flip = 0;
    if (head.fourcc == F("ATFB")) {
      // PAFB: ATFB(id, duration, loop) on an array texture, as the packed flipbook, its tile picked
      // as CUVEAnimFlipBookTexture::GetValue 0x2cde08 picks the layer; TRST as TXFB's.
      bool mirror = false;
      std::vector<uint8_t> cycle;
      if (value.size() > 2 || head.args.size() != 3 || (value.size() == 2 && !FlipbookTransform(value[1], mirror))) {
        why = "ATFB that is not a flipbook with an optional mirror";
        return false;
      }
      const EffectValue& loop = head.args[2];
      if (!IsElement(loop, F("CNST")) || loop.args.size() != 1 ||
          (loop.args[0].kind != EffectValue::Kind::Word && loop.args[0].kind != EffectValue::Kind::Byte)) {
        why = "ATFB whose loop is not constant";
        return false;
      }
      if (!Element(head.args[1], Type::Int, cycle, why)) {
        why = "ATFB duration: " + why;
        return false;
      }
      const FlipbookAtlas atlas = m_io.flipbook ? m_io.flipbook(head.args[0].guid) : FlipbookAtlas{};
      if (atlas.id == 0) {
        why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no flipbook atlas";
        return false;
      }
      AppendBE32(out, F("PAFB"));
      AppendBE32(out, F("CNST"));
      AppendBE32(out, atlas.id);
      for (const int32_t v : {atlas.cols, atlas.rows, atlas.frames, loop.args[0].word != 0 ? 1 : 0, mirror ? 1 : 0}) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, uint32_t(v));
      }
      out.insert(out.end(), cycle.begin(), cycle.end());
      return true;
    }
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

  // The stored ids of every model a PMDL names: its asset, or each entry of its SLCT's ARRY.
  std::vector<EffectGuid> ModelGuids(const std::vector<EffectValue>& value) const {
    std::vector<EffectGuid> out;
    if (value.size() == 1 && IsElement(value[0], F("SLCT")) && value[0].args.size() == 2 &&
        IsElement(value[0].args[1], F("ARRY")) && !value[0].args[1].args.empty() &&
        value[0].args[1].args[0].kind == EffectValue::Kind::Raw) {
      const EffectValue& array = value[0].args[1].args[0];
      const uint8_t* p = m_data + array.offset;
      const uint32_t count = array.size >= 4 ? ReadLE32(p) : 0;
      for (uint32_t i = 0; i < count && 4 + size_t(i + 1) * 20 <= array.size; ++i) {
        EffectGuid guid;
        std::memcpy(guid.data(), p + 4 + size_t(i) * 20 + 4, 16);
        out.push_back(guid);
      }
    } else if (value.size() == 1 && value[0].kind == EffectValue::Kind::Guid) {
      out.push_back(value[0].guid);
    } else if (value.size() == 1 && IsElement(value[0], F("CNST")) && value[0].args.size() == 1 &&
               value[0].args[0].kind == EffectValue::Kind::Guid) {
      out.push_back(value[0].args[0].guid);
    } else if (value.size() == 2 && value[1].kind == EffectValue::Kind::Guid) {
      out.push_back(value[1].guid);
    }
    return out;
  }

  // PMDL's SLCT(index, ARRY of n CNST ids), as the model ids a particle picks from by its seed.
  // Remastered takes entry index % n; IRND(0, h) is latched per particle, so the list is
  // expanded to h + 1 entries (entry i = model i % n) to keep its odds, and CNST(k) is one model.
  bool Variants(const std::vector<EffectValue>& value, std::vector<uint32_t>& ids, std::string& why) const {
    uint32_t high = 0, constant = 0;
    bool fixed = false;
    if (value.size() != 1 || !IsElement(value[0], F("SLCT")) || value[0].args.size() != 2 ||
        !IsElement(value[0].args[1], F("ARRY")) || value[0].args[1].args.empty() ||
        value[0].args[1].args[0].kind != EffectValue::Kind::Raw) {
      why = "a model choice that is not SLCT(index, ARRY)";
      return false;
    }
    if (ConstWord(value[0].args[0], constant)) {
      fixed = true;
    } else if (!RangeFromZero(value[0].args[0], F("IRND"), high) || int32_t(high) < 0 || high >= 64) {
      why = "a model choice indexed by neither CNST nor IRND(0, h < 64)";
      return false;
    }
    const EffectValue& array = value[0].args[1].args[0];
    const uint8_t* p = m_data + array.offset;
    const uint32_t count = array.size >= 4 ? ReadLE32(p) : 0;
    if (count == 0 || count > 256 || array.size != 4 + size_t(count) * 20) {
      why = "SLCT ARRY of " + std::to_string(count) + " models in " + std::to_string(array.size) + " bytes";
      return false;
    }
    std::vector<uint32_t> models;
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
      models.push_back(id);
    }
    if (fixed) {
      // C remainder, as CElementArray::GetValue: a negative index stays negative.
      const int32_t index = int32_t(constant) % int32_t(count);
      if (index < 0) {
        why = "SLCT model index " + std::to_string(int32_t(constant)) + " is negative";
        return false;
      }
      ids.push_back(models[size_t(index)]);
      return true;
    }
    for (uint32_t i = 0; i <= high; ++i) {
      ids.push_back(models[i % count]);
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
    if (head.fourcc == F("TXP2") || head.fourcc == F("TXFB") || head.fourcc == F("ATX2") || head.fourcc == F("ATFB")) {
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
    // NFOM: ELC2's tangent mode, one raw byte (0xb70734 reads it with CInputStream::Get, no
    // element), written as CNST and an s32 of it. The other ELC2 fields are element reads
    // (GetIntElement, GetIntElementAsConst for LWQD) or flags (GetBool).
    if (fourcc == F("NFOM")) {
      const EffectValue* byte = value.size() == 1 ? &value[0] : nullptr;
      if (byte != nullptr && byte->kind == EffectValue::Kind::Byte) {
        AppendBE32(out, F("CNST"));
        AppendBE32(out, byte->word);
        return true;
      }
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
        // Int RAND of constants is PRAN for min >= 0, retail's RAND (raw Next()) below, as in Element.
        const bool pran = v.fourcc == F("RAND") && int32_t(v.args[0].args[0].word) >= 0;
        AppendBE32(out, v.fourcc == F("MPRD") ? F("RAND") : pran ? F("PRAN") : v.fourcc);
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
    // A vector property is Remastered's numbers, used in the port's space: BSIS, or BSSC for
    // PMSC's scale. PMRT is turned as a rotation (ModelRotation), and PSOV is never written.
    if (type.type == Type::Vector && fourcc != F("PMRT") && fourcc != F("PSOV")) {
      std::vector<uint8_t> inner;
      if (!Element(value[0], type.type, inner, why)) {
        return false;
      }
      Basis(fourcc == F("PMSC") ? F("BSSC") : F("BSIS"), inner, out);
      return true;
    }
    m_emtr = fourcc == F("EMTR");
    const bool ok = Element(value[0], type.type, out, why);
    m_emtr = false;
    return ok;
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

  // Port-only PKSM after its FourCC: Remastered's spawn tables as its
  // CSpawnSystemKeyframeData reads them (0x293350), for CElementGen::PortSpawnTables.
  // CNST, the end frame, the table count; per table its start, 1 if the period is a
  // constant, the period (an int element) and the entry count; per entry its frame and
  // spawn count; per spawn its type, asset id, instance count (an int element) and the
  // index of the component that places it (~0 for none); then the component count and
  // each component (Component()). `node` is the generator, for RSYX.
  // False with `why` set when a table holds what the port cannot run as Remastered does.
  bool SpawnTables(const EffectProperty& property, const EffectNode& node, std::vector<uint8_t>& out,
                   std::string& why) const {
    EffectSpawnTable table;
    if (!ParseSpawnTable(m_data, property.offset + property.size, property, table)) {
      why = "spawn table does not parse";
      return false;
    }
    if (table.components.size() != table.events) {
      why = std::to_string(table.events) + " SEVT events, " + std::to_string(table.components.size()) + " parsed";
      return false;
    }
    if (table.tables.empty()) {
      return true;
    }
    std::vector<uint8_t> bytes;
    size_t unresolved = 0;
    AppendBE32(bytes, F("CNST"));
    AppendBE32(bytes, table.header[2]);
    AppendBE32(bytes, uint32_t(table.tables.size()));
    for (const EffectSpawnTable::Table& each : table.tables) {
      AppendBE32(bytes, each.word);
      AppendBE32(bytes, IsElement(each.selector, F("CNST")) ? 1u : 0u);
      if (!Element(each.selector, Type::Int, bytes, why)) {
        why = "period: " + why;
        return false;
      }
      AppendBE32(bytes, uint32_t(each.frames.size()));
      for (const EffectSpawnTable::Frame& frame : each.frames) {
        std::vector<uint8_t> spawns;
        uint32_t count = 0;
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
          if (spawn.component != ~0u && spawn.component >= table.components.size()) {
            why = "a spawn placed by missing component " + std::to_string(spawn.component);
            return false;
          }
          // PEOD: GetNumInstances 0x2940fc evaluates the count per spawn. A tree that needs
          // storage (CElementDep::Finish 0x24f9a4: IRND of constants is initial-only, and so is
          // a node whose inputs all are) is a ctx-0 variable CAddVarSystem::Init sets once: a
          // hidden init-only PSYS int the port-only PSYI reads. Other initial-only trees are not
          // proven.
          // Shipped counts are PEOD(e) or NONE (count 1).
          const EffectValue& instances = IsElement(spawn.instances, F("PEOD")) && spawn.instances.args.size() == 1
                                         ? spawn.instances.args[0]
                                         : spawn.instances;
          const bool latched = IsElement(instances, F("IRND")) && instances.args.size() == 2 &&
                               IsElement(instances.args[0], F("CNST")) &&
                               IsElement(instances.args[1], F("CNST"));
          if (!latched && (ContainsElement(instances, F("IRND")) || ContainsElement(instances, F("KPIN")))) {
            why = "instance instances with an initial-only " + EffectFourCCString(instances.fourcc);
            return false;
          }
          AppendBE32(spawns, type);
          AppendBE32(spawns, id);
          if (latched) {
            if (m_spawnSysItems == nullptr) {
              why = "an IRND instance count outside a generator";
              return false;
            }
            SystemItem hidden{1, m_sysNextHiddenInt++, 1, {}};
            AppendBE32(hidden.element, F("PRAN"));
            if (!Element(instances.args[0], Type::Int, hidden.element, why) ||
                !Element(instances.args[1], Type::Int, hidden.element, why)) {
              return false;
            }
            AppendBE32(spawns, F("PSYI"));
            AppendBE32(spawns, hidden.slot);
            m_spawnSysItems->push_back(std::move(hidden));
          } else if (IsElement(instances, F("NONE"))) {
            AppendBE32(spawns, F("NONE"));
          } else if (!Element(instances, Type::Int, spawns, why)) {
            why = "instance instances: " + why;
            return false;
          }
          AppendBE32(spawns, spawn.component);
          ++count;
        }
        AppendBE32(bytes, frame.frame);
        AppendBE32(bytes, count);
        bytes.insert(bytes.end(), spawns.begin(), spawns.end());
      }
    }
    AppendBE32(bytes, uint32_t(table.components.size()));
    for (size_t i = 0; i < table.components.size(); ++i) {
      if (!Component(table.components[i], node, bytes, why)) {
        why = "component " + std::to_string(i) + ": " + why;
        return false;
      }
    }
    if (unresolved != 0) {
      m_approximated.push_back("KSSM: " + std::to_string(unresolved) + " spawns unresolved");
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
  }

  // A spawn component, SEVT(SMOV(...)), for the evaluator in CElementGen
  // (Remastered's CSpawnComponent, build/mpr/kb): its SMOV as Mover() writes it.
  bool Component(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
                 std::string& why) const {
    if (!IsElement(value, F("SEVT")) || value.args.size() != 1) {
      why = "not SEVT(SMOV)";
      return false;
    }
    return Mover(value.args[0], node, out, why);
  }

  // A mover, SMOV(position, delta, rotation, scale, modifier) (CSimpleMoverElement),
  // written as the position (a vector node), the delta's three floats (it must be a
  // constant: the mover keeps its first value), the rotation (NONE or a rotation node)
  // and the modifier: NONE, or GRAV and its vector. The scale must be NONE.
  bool Mover(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
             std::string& why) const {
    if (!IsElement(value, F("SMOV")) || value.args.size() != 5) {
      why = "not SMOV";
      return false;
    }
    const std::vector<EffectValue>& args = value.args;
    if (!ComponentVector(args[0], node, out, why)) {
      return false;
    }
    const EffectValue& delta = args[1];
    if (!IsElement(delta, F("CNST")) || delta.args.size() != 3) {
      why = "delta " + EffectFourCCString(delta.fourcc) + " is not a constant";
      return false;
    }
    for (const EffectValue& each : delta.args) {
      std::vector<uint8_t> word;
      if (!IsElement(each, F("CNST")) || !Literal(each, Type::Real, word, why)) {
        why = "delta is not a constant";
        return false;
      }
      out.insert(out.end(), word.begin(), word.end());
    }
    if (IsElement(args[2], F("NONE"))) {
      AppendBE32(out, F("NONE"));
    } else if (!ComponentRotation(args[2], node, out, why)) {
      return false;
    }
    if (!IsElement(args[3], F("NONE"))) {
      why = "scale " + EffectFourCCString(args[3].fourcc);
      return false;
    }
    const EffectValue& modifier = args[4];
    if (IsElement(modifier, F("NONE"))) {
      AppendBE32(out, F("NONE"));
      return true;
    }
    // GRAV(acceleration, flag, next): the flag set and a chained modifier are unseen.
    if (!IsElement(modifier, F("GRAV")) || modifier.args.size() != 3 ||
        (modifier.args[1].kind != EffectValue::Kind::Byte && modifier.args[1].kind != EffectValue::Kind::Word) ||
        modifier.args[1].word != 0 || !IsElement(modifier.args[2], F("NONE"))) {
      why = "modifier " + EffectFourCCString(modifier.fourcc);
      return false;
    }
    AppendBE32(out, F("GRAV"));
    return ComponentVector(modifier.args[0], node, out, why);
  }

  // A component's real: CNST, ADD_, MULT, IRND, RAND, VARF, or KEYE on the emitter frame.
  bool ComponentReal(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if (fourcc == F("CNST")) {
      AppendBE32(out, fourcc);
      return Literal(value, Type::Real, out, why);
    }
    if (fourcc == F("VARF") || (value.args.empty() && SystemRealRead(fourcc) >= 0)) {
      return Element(value, Type::Real, out, why);
    }
    if (fourcc == F("KEYE") && value.args.size() == 1) {
      const EffectValue& keys = value.args[0];
      if (keys.kind != EffectValue::Kind::Keys || keys.size < 22 || ReadLE32(m_data + keys.offset) != 0 ||
          ReadLE32(m_data + keys.offset + 18) == 0) {
        why = "KEYE on the particle's lifetime or with no keys";
        return false;
      }
      AppendBE32(out, fourcc);
      return Keyframes(keys, Type::Real, out, why);
    }
    if ((fourcc == F("ADD_") || fourcc == F("MULT") || fourcc == F("IRND") || fourcc == F("RAND")) &&
        value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentReal(value.args[0], out, why) && ComponentReal(value.args[1], out, why);
    }
    why = "real " + EffectFourCCString(value.fourcc);
    return false;
  }

  // A component's vector: CNST(x, y, z), EXTT, EXTS, ADD_, MULT, ROTV(vector, rotation),
  // KPIN, or CIRC(centre, normal, angle, angle per frame, radius).
  bool ComponentVector(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
                       std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if ((fourcc == F("EXTT") || fourcc == F("EXTS")) && value.args.empty()) {
      AppendBE32(out, fourcc);
      return true;
    }
    if (fourcc == F("KPIN") && value.args.size() == 1) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why);
    }
    if ((fourcc == F("ADD_") || fourcc == F("MULT")) && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why) && ComponentVector(value.args[1], node, out, why);
    }
    if (fourcc == F("ROTV") && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why) && ComponentRotation(value.args[1], node, out, why);
    }
    if (fourcc == F("CNST") && value.args.size() == 3) {
      AppendBE32(out, fourcc);
      return ComponentReal(value.args[0], out, why) && ComponentReal(value.args[1], out, why) &&
             ComponentReal(value.args[2], out, why);
    }
    if (fourcc == F("CIRC") && value.args.size() == 5) {
      AppendBE32(out, fourcc);
      return ComponentVector(value.args[0], node, out, why) && ComponentVector(value.args[1], node, out, why) &&
             ComponentReal(value.args[2], out, why) && ComponentReal(value.args[3], out, why) &&
             ComponentReal(value.args[4], out, why);
    }
    why = "vector " + EffectFourCCString(value.fourcc);
    return false;
  }

  // A component's rotation: EXTR, REUL(x, y, z, order 0) in degrees, RADD(a, b) = a * b,
  // or RSYX, the generator's own system rotation: with XFMD 4 and an identity SMVR the
  // emitter's (EXTR), with XFMD 0 or 2 none (written IDNT).
  bool ComponentRotation(const EffectValue& value, const EffectNode& node, std::vector<uint8_t>& out,
                         std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if (fourcc == F("EXTR") && value.args.empty()) {
      AppendBE32(out, fourcc);
      return true;
    }
    if (fourcc == F("RADD") && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return ComponentRotation(value.args[0], node, out, why) && ComponentRotation(value.args[1], node, out, why);
    }
    if (fourcc == F("KPIN") && value.args.size() == 1) {
      AppendBE32(out, fourcc);
      return ComponentRotation(value.args[0], node, out, why);
    }
    if (fourcc == F("REUL") && value.args.size() == 4) {
      const EffectValue& order = value.args[3];
      if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
        why = "REUL in an unseen axis order";
        return false;
      }
      AppendBE32(out, fourcc);
      return ComponentReal(value.args[0], out, why) && ComponentReal(value.args[1], out, why) &&
             ComponentReal(value.args[2], out, why);
    }
    if (fourcc == F("RSYX") && value.args.empty()) {
      const EffectProperty* xfmd = nullptr;
      const EffectProperty* mover = nullptr;
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == F("XFMD")) {
          xfmd = &property;
        } else if (property.fourcc == F("SMVR")) {
          mover = &property;
        }
      }
      uint32_t mode = ~0u;
      if (xfmd != nullptr && SmallConst(*xfmd, mode) && mode == 4 && mover != nullptr && IdentityMover(*mover)) {
        AppendBE32(out, F("EXTR"));
        return true;
      }
      if (xfmd != nullptr && (mode == 0 || mode == 2)) {
        AppendBE32(out, F("IDNT"));
        return true;
      }
      why = "RSYX under XFMD " + (xfmd != nullptr ? std::to_string(mode) : std::string("none"));
      return false;
    }
    why = "rotation " + EffectFourCCString(value.fourcc);
    return false;
  }

  // A swoosh PROT turning about Z only: [KPIN](REUL(0, 0, z, #00)) or [KPIN](RADD(EXTR or INTR,
  // REUL(0, 0, z, #00))), as port-only PROZ flags (bit 0 keep the birth value, bit 1 start from the
  // orientation) and z in degrees. Remastered evaluates PROT at every point's birth and, unless it is
  // initial-only, again every frame for every live point (UpdateExisting 0x27d590). KPIN keeps its
  // child's value at birth (CREKeepInitial 0x297478, CElementDep::Finish 0x24f9a4); a z whose every
  // varying part sits under KPIN is therefore kept whole. SUST evaluates on the system's frame
  // (CRESolveUsingSystemTime 0x298ef8), the frame the port evaluates swoosh elements on, so it is
  // dropped; a timed SINE outside it would run on the point's age and is left out. EXTR and INTR are
  // both the orientation SetOrientation gives under XFMD 2, unless the swoosh has an INTR property
  // (PSIR): bit 2 marks INTR, which reads that system rotation and not the external one.
  bool SwooshZRotation(const EffectValue& prot, uint32_t& flags, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue* value = &prot;
    flags = 0;
    if (IsElement(*value, F("KPIN")) && value->args.size() == 1) {
      flags |= 1;
      value = &value->args[0];
    }
    if (IsElement(*value, F("RADD")) && value->args.size() == 2 &&
        (IsElement(value->args[0], F("EXTR")) || IsElement(value->args[0], F("INTR"))) &&
        value->args[0].args.empty()) {
      flags |= IsElement(value->args[0], F("INTR")) ? 6 : 2;
      value = &value->args[1];
    }
    uint32_t word = 0;
    if (!IsElement(*value, F("REUL")) || value->args.size() != 4 || !ConstWord(value->args[0], word) ||
        (word & 0x7fffffffu) != 0 || !ConstWord(value->args[1], word) || (word & 0x7fffffffu) != 0) {
      why = "not [KPIN]([RADD(EXTR or INTR, ]REUL(0, 0, z))";
      return false;
    }
    const EffectValue& order = value->args[3];
    if ((order.kind != EffectValue::Kind::Byte && order.kind != EffectValue::Kind::Word) || order.word != 0) {
      why = "REUL in an unseen axis order";
      return false;
    }
    bool kept = false;
    bool live = false;
    std::vector<uint8_t> angle;
    if (!SwooshAngle(value->args[2], false, false, kept, live, angle, why)) {
      return false;
    }
    if (kept && live && (flags & 1) == 0) {
      why = "z mixes kept and per-frame parts";
      return false;
    }
    if (kept) {
      flags |= 1;
    }
    out.insert(out.end(), angle.begin(), angle.end());
    return true;
  }

  // A swoosh PROT's z angle as a retail real with KPIN and SUST taken out (see SwooshZRotation).
  // `kept` reports a KPIN, `live` a part varying outside one.
  bool SwooshAngle(const EffectValue& value, bool underKpin, bool underSust, bool& kept, bool& live,
                   std::vector<uint8_t>& out, std::string& why) const {
    const uint32_t fourcc = value.kind == EffectValue::Kind::Element ? value.fourcc : 0;
    if (fourcc == F("KPIN") && value.args.size() == 1) {
      kept = true;
      return SwooshAngle(value.args[0], true, underSust, kept, live, out, why);
    }
    if (fourcc == F("SUST") && value.args.size() == 1) {
      return SwooshAngle(value.args[0], underKpin, true, kept, live, out, why);
    }
    if (fourcc == F("CNST")) {
      return Element(value, Type::Real, out, why);
    }
    if (fourcc == F("KEYE") && value.args.size() == 1) {
      const EffectValue& keys = value.args[0];
      if (keys.kind != EffectValue::Kind::Keys || keys.size < 22 || ReadLE32(m_data + keys.offset) != 0) {
        why = "z KEYE on the particle's lifetime";
        return false;
      }
      live = live || !underKpin;
      return Element(value, Type::Real, out, why);
    }
    if ((fourcc == F("RAND") || fourcc == F("IRND")) && value.args.size() == 2) {
      live = live || !underKpin;
      AppendBE32(out, fourcc);
      return SwooshAngle(value.args[0], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[1], underKpin, underSust, kept, live, out, why);
    }
    if (fourcc == F("SINE") && value.args.size() == 3 && underSust) {
      live = live || !underKpin;
      AppendBE32(out, fourcc);
      return SwooshAngle(value.args[0], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[1], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[2], underKpin, underSust, kept, live, out, why);
    }
    if ((fourcc == F("ADD_") || fourcc == F("SUB_") || fourcc == F("MULT")) && value.args.size() == 2) {
      AppendBE32(out, fourcc);
      return SwooshAngle(value.args[0], underKpin, underSust, kept, live, out, why) &&
             SwooshAngle(value.args[1], underKpin, underSust, kept, live, out, why);
    }
    why = "z " + EffectFourCCString(value.fourcc) + (fourcc == F("SINE") ? " on the point's age" : "");
    return false;
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

  // LTYP and LFOT are usually stored as a byte property in Remastered, though
  // the typed integer form is also accepted by the grammar.
  static bool SmallInteger(const EffectProperty& property, uint32_t& word) {
    if (property.value.size() == 1 && (property.value[0].kind == EffectValue::Kind::Byte ||
                                       property.value[0].kind == EffectValue::Kind::Word)) {
      word = property.value[0].word;
      return true;
    }
    return SmallConst(property, word);
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
  // mover. Written only for those two; 1, 2 and 5 draw as retail does (a GPSM's
  // forced 1 is PartSystemMode's). With 4,
  // PSMV carries the mover (none: the particles sit at the origin, unrotated),
  // except for the identity SMVR, which is the emitter's transform as it stands.
  void Transform(const EffectProperty& xfmd, const EffectProperty* mover, const EffectNode& node,
                 std::vector<uint8_t>& out) const {
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
    if (mode != 4 || (mover != nullptr && IdentityMover(*mover))) {
      return;
    }
    if (mover == nullptr) {
      PutList(out, F("PSMV"), 0, {});
      return;
    }
    std::vector<uint8_t> items;
    std::string why;
    if (mover->value.size() != 1 || !Mover(mover->value[0], node, items, why)) {
      m_approximated.push_back("SMVR taken as the emitter's transform: " + (why.empty() ? "not SMOV" : why));
      return;
    }
    PutList(out, F("PSMV"), 1, items);
  }

  // EMVR, XFMD 4's emitter mover (desc+0xa0, SetupTransformMode 0x289668), as the port-only PEMR:
  // CNST 1 and the mover as Mover() writes it. It is a system-scoped SMOV like SMVR; its position
  // is vec 0x12 and its rotation Q_l, which CreateNewParticles bakes into each particle at spawn
  // (pos = Q_l emtr + vec12, the velocity turned by Q_l). With no SMVR they are also the system
  // translation and rotation (+0x3c 0x289838, +0x38 0x289a60). Only that case is written: every
  // part EMVR has no SMVR beside it, and with one +0x3c composes the two (0x289954).
  void EmitterMover(const EffectProperty& emvr, const EffectProperty* xfmd, bool smvr, const EffectNode& node,
                    std::vector<uint8_t>& out, std::vector<std::string>& dropped) const {
    uint32_t mode = 2;
    if (xfmd == nullptr || !SmallConst(*xfmd, mode) || mode != 4) {
      dropped.push_back("EMVR: not XFMD 4");
      return;
    }
    if (smvr) {
      dropped.push_back("EMVR with an SMVR");
      return;
    }
    std::vector<uint8_t> items;
    std::string why;
    if (emvr.value.size() != 1 || !Mover(emvr.value[0], node, items, why)) {
      dropped.push_back("EMVR: " + (why.empty() ? "not SMOV" : why));
      return;
    }
    PutList(out, F("PEMR"), 1, items);
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
  // An ELC2 (`electric`, with `swoosh` set) takes its blend from EBDM.
  bool Material(const EffectProperty& material, const EffectNode& node, ConvertedPart& result, bool swoosh,
                bool electric = false) const {
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
    const Recipe* recipe = RecipeOf(mati.shader);
    if (recipe == nullptr) {
      char shaderText[16];
      std::snprintf(shaderText, sizeof(shaderText), "%08x", mati.shader);
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " has no recipe");
      return false;
    }
    return MaterialFrom(mati, *recipe, guid->guid, &material, node, result, swoosh, electric);
  }

  // The VMAT (and its VPRM, VTMT, VPMT, VSMT, SSZE, ITEN, VORN) for `mati` drawn with `recipe`.
  // `matiGuid` and `material` are the MTIN's MATI and property; a header-material node has neither.
  // A sprite GPSM with no MTIN draws its header MTRL (node.baseMaterial; see HeaderShaderOf). Its VMAT,
  // where the shader's quad and maths are the port's: anything else is refused with the reason.
  bool HeaderMaterial(const EffectNode& node, ConvertedPart& result) const {
    if (!m_io.vfxTexture || node.baseMaterial == EffectGuid{}) {
      return false;
    }
    // The MTRL's shader id is its guid's first four bytes as stored (a MATI's +0x48 reads the same way).
    const uint32_t shader = uint32_t(node.baseMaterial[0]) << 24 | uint32_t(node.baseMaterial[1]) << 16 |
                            uint32_t(node.baseMaterial[2]) << 8 | uint32_t(node.baseMaterial[3]);
    char shaderText[16];
    std::snprintf(shaderText, sizeof(shaderText), "%08x", shader);
    const auto refuse = [this, &shaderText](const std::string& why) {
      m_approximated.push_back(std::string("VMAT: header shader ") + shaderText + " " + why);
      return false;
    };
    // Anything else draws as retail, as before, named here so the report says which shader it lacks.
    // Primitive 1 draws the sprite quads, primitive 2 the planar square.
    const HeaderShader* header = HeaderShaderOf(shader);
    if (header != nullptr && (header->quad == HeaderQuad::Planar ? node.primitiveType != 2 : node.primitiveType != 1)) {
      header = nullptr;
    }
    if (header == nullptr) {
      return refuse("is not ported (primitive " + std::to_string(node.primitiveType) + ")");
    }
    const EffectProperty* texr = nullptr;
    const EffectProperty* rota = nullptr;
    const EffectProperty* aero = nullptr;
    const EffectProperty* anth = nullptr;
    uint32_t orient = 0;
    uint32_t lit = 0;
    uint32_t blit = 0;
    for (const EffectProperty& property : node.properties) {
      uint32_t word = 0;
      if (property.fourcc == F("PMDL")) {
        return refuse("on a model particle");
      }
      // A lit draw (LIT_ 1 or BLIT 1) is another perm: the vertexLit shaders' uber perm 040 is ported
      // (BLIT gives it the probe, PBLT), and only under LIT_ 1.
      if (property.fourcc == F("LIT_") || property.fourcc == F("BLIT")) {
        if (!SmallInteger(property, word)) {
          return refuse("has a " + EffectFourCCString(property.fourcc) + " that is not a constant");
        }
        (property.fourcc == F("LIT_") ? lit : blit) = word;
        if (word != 0 && !header->vertexLit) {
          return refuse("is lit (" + EffectFourCCString(property.fourcc) + ")");
        }
      }
      if (property.fourcc == F("AERO")) {
        aero = &property;
      }
      if (property.fourcc == F("ANTH")) {
        anth = &property;
      }
      // Perm 048's UV transform (0.5 + R(E) (diag(C, D) q + (A, B) - 0.5)) is not the port's VTMT.
      if (header->textured && property.fourcc == F("TMTR")) {
        return refuse("has a UV transform (TMTR)");
      }
      if (property.fourcc == F("ORNT") && (!SmallConst(property, orient) || orient > 2)) {
        return refuse("has an ORNT that is not a constant 0..2");
      }
      if (property.fourcc == F("ROTA") && (!SmallConst(property, word) || word != 0)) {
        rota = &property;
      }
      if (property.fourcc == F("TEXR")) {
        texr = &property;
      }
    }
    // AERO is 52d26efe's erosion (VPMT); a header perm that takes none would lose it unreported.
    if (aero != nullptr && !header->litPalette) {
      return refuse("has an erosion (AERO)");
    }
    if (anth != nullptr && !header->arrayThreshold) {
      return refuse("has a thresholding map (ANTH)");
    }
    if (header->quad == HeaderQuad::Axis ? orient == 0 : orient != 0) {
      return refuse("draws a different quad from ORNT " + std::to_string(orient));
    }
    // The planar square takes its axes from the particle transform alone; no shipped one has a ROTA.
    if ((header->quad == HeaderQuad::Sprite || header->quad == HeaderQuad::Planar) && rota != nullptr) {
      return refuse("does not rotate, and the particle has a ROTA");
    }
    Mati mati;
    mati.shader = shader;
    Recipe recipe{shader, 0, {}, {}};
    std::vector<uint8_t> vtrs;
    std::vector<uint8_t> vtrt;
    std::vector<uint8_t> vatx;
    std::vector<uint8_t> vpmt;
    uint32_t vpmtCount = 0;
    // TRST(x, y, sx, sy, rot, wrap): CopyPerSpriteDataToSpriteRenderData 0x2477cc evaluates it per sprite
    // and SetupInstancingComponents_SpriteCustom passes rot in radians (x pi/180, 0x2b4a04). The wrap
    // (GetDiffuseTextureWrapMode 0x291434) is the sampler's when -1.
    const auto transformInto = [&](const EffectValue& value, std::string& why, std::vector<uint8_t>& out,
                                   const char* owner) {
      if (IsElement(value, F("NONE"))) {
        return true;
      }
      if (!IsElement(value, F("TRST"))) {
        why = std::string("has no constant ") + owner;
        return false;
      }
      if (value.args.size() != 6 || value.args[5].kind != EffectValue::Kind::Word || value.args[5].word != 0xffffffffu) {
        why = std::string("has a ") + owner + " TRST with a wrap mode";
        return false;
      }
      for (size_t i = 0; i < 5; ++i) {
        if (!Element(value.args[i], Type::Real, out, why)) {
          why = std::string("has a ") + owner + " TRST the port can't evaluate: " + why;
          return false;
        }
      }
      return true;
    };
    const auto transform = [&](const EffectValue& value, std::string& why) {
      return transformInto(value, why, vtrs, "TEXR");
    };
    const bool atfb = texr != nullptr && !texr->value.empty() && IsElement(texr->value[0], F("ATFB"));
    if (header->flipbook != atfb) {
      return refuse(header->flipbook ? "has no ATFB TEXR" : "has an ATFB TEXR on a single-texture shader");
    }
    if (atfb) {
      // ATFB(id, duration, loop) on a texture array: CUVEAnimFlipBookTexture (ctor 0x2cdb78) keeps
      // one entry per layer, GetValue (0x2cde08) picks it from the particle's frame like ATEX's tile.
      const EffectValue& head = texr->value[0];
      if (texr->value.size() != 2 || head.args.size() != 3 || head.args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has an ATFB TEXR the port can't read");
      }
      const EffectValue& loopArg = head.args[2];
      if (!IsElement(loopArg, F("CNST")) || loopArg.args.size() != 1 ||
          (loopArg.args[0].kind != EffectValue::Kind::Word && loopArg.args[0].kind != EffectValue::Kind::Byte)) {
        return refuse("has an ATFB TEXR whose loop is not constant");
      }
      const uint32_t loop = loopArg.args[0].word;
      const FlipbookAtlas layers = m_io.vfxTexture(head.args[0].guid);
      if (layers.id == 0 || layers.frames < 1) {
        return refuse("has an ATFB TEXR whose texture can't be read");
      }
      std::string why;
      std::vector<uint8_t> duration;
      if (!Element(head.args[1], Type::Int, duration, why)) {
        return refuse("has an ATFB duration the port can't evaluate: " + why);
      }
      if (!transform(texr->value[1], why)) {
        return refuse(why);
      }
      AppendBE32(vatx, F("VATF"));
      AppendBE32(vatx, uint32_t(layers.frames));
      AppendBE32(vatx, loop != 0 ? 1 : 0);
      vatx.insert(vatx.end(), duration.begin(), duration.end());
      mati.textures["TEXR"] = MatiTexture{head.args[0].guid, 0, 1, -1, -1};
      recipe.features = kColorTex;
      recipe.textures = {{"TEXR", Role::Color, -1, -1}};
      recipe.alphaGamma = 2.2f;
    } else if (header->arrayThreshold) {
      // TXFB(id, index) on a texture array: CUVEFlipBookTexture's GetValue (0x2ce114) picks layer
      // trunc(clamp(index x layers, 0, layers - 1)), NaN -> 0, the index a real element.
      if (texr == nullptr || texr->value.size() != 2 || !IsElement(texr->value[0], F("TXFB")) ||
          texr->value[0].args.size() != 2 || texr->value[0].args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has no TXFB TEXR");
      }
      const EffectValue& txfb = texr->value[0];
      const FlipbookAtlas layers = m_io.vfxTexture(txfb.args[0].guid);
      if (layers.id == 0 || layers.frames < 1) {
        return refuse("has a TXFB TEXR whose texture can't be read");
      }
      std::string why;
      std::vector<uint8_t> index;
      m_lifetimeOffset = LifetimeOffset(true, false, false, *texr);
      if (!Element(txfb.args[1], Type::Real, index, why)) {
        return refuse("has a TXFB index the port can't evaluate: " + why);
      }
      if (!transform(texr->value[1], why)) {
        return refuse(why);
      }
      AppendBE32(vatx, F("VTFB"));
      AppendBE32(vatx, uint32_t(layers.frames));
      vatx.insert(vatx.end(), index.begin(), index.end());
      // ANTH(map, TRST, FadeIn, FadeOut, Softness) (ReadAnimatedThresholdingParticleInput 0x268894):
      // the map is sampled at the quad corner through its own TRST (no TEXR rect), and the three
      // reals are per-particle (UpdateRenderState_Node 0x248598 keeps their variable slots): VS
      // v6.z/v6.w/softness -> FS v2.x/v2.y/v2.z, the port's thresholding on per-particle row 0.
      if (anth == nullptr || anth->value.size() != 1 || !IsElement(anth->value[0], F("ANTH")) ||
          anth->value[0].args.size() != 5 || !IsElement(anth->value[0].args[0], F("CNST")) ||
          anth->value[0].args[0].args.size() != 1 || anth->value[0].args[0].args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has no ANTH with a constant map");
      }
      const EffectValue& thr = anth->value[0];
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc != F("PMTR")) {
          continue;
        }
        for (const EffectValue& item : property.value) {
          if ((item.word & 0xffff) <= 2) {  // row 0, components 0..2
            return refuse("has an ANTH and a PMTR on the same component");
          }
        }
      }
      m_lifetimeOffset = LifetimeOffset(true, false, false, *anth);
      if (!IsElement(thr.args[1], F("TRST")) || !transformInto(thr.args[1], why, vtrt, "ANTH")) {
        return refuse(IsElement(thr.args[1], F("TRST")) ? why : "has an ANTH without a TRST");
      }
      for (uint32_t comp = 0; comp < 3; ++comp) {
        AppendBE32(vpmt, 0);  // a real, row 0
        AppendBE32(vpmt, 0);
        AppendBE32(vpmt, comp);
        ++vpmtCount;
        if (!Element(thr.args[2 + comp], Type::Real, vpmt, why)) {
          return refuse("has an ANTH the port can't evaluate: " + why);
        }
      }
      mati.textures["TEXR"] = MatiTexture{txfb.args[0].guid, 0, 1, -1, -1};
      mati.textures["ANTH"] = MatiTexture{thr.args[0].args[0].guid, 1, 1, -1, -1};
      recipe.features = kColorTex | kThresholding;
      recipe.textures = {{"TEXR", Role::Color, -1, -1}, {"ANTH", Role::Threshold, -1, -1}};
      recipe.sources = {{kSrcThrX, 0, 0}, {kSrcThrY, 0, 1}, {kSrcThrW, 0, 2}};
      recipe.alphaGamma = 2.2f;
    } else if (header->textured && texr != nullptr && texr->value.size() == 1 && IsElement(texr->value[0], F("ATEX"))) {
      // ATEX(id, tileW, tileH, strideW, strideH, cycle, loop), all constants: CUVEAnimTexture (ctor
      // 0x2cd0f4) cuts the texture's full size into tiles, GetValue (0x2cd750) picks one per frame.
      const EffectValue& atex = texr->value[0];
      if (atex.args.size() != 7 || atex.args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has an ATEX TEXR the port can't read");
      }
      uint32_t words[6];
      for (size_t i = 0; i < 6; ++i) {
        const EffectValue& arg = atex.args[i + 1];
        if (!IsElement(arg, F("CNST")) || arg.args.size() != 1 ||
            (arg.args[0].kind != EffectValue::Kind::Word && arg.args[0].kind != EffectValue::Kind::Byte)) {
          return refuse("has an ATEX TEXR that is not constant");
        }
        words[i] = arg.args[0].word;
      }
      uint32_t width = 0, height = 0;
      if (!m_io.textureSize || !m_io.textureSize(atex.args[0].guid, width, height) || width == 0 || height == 0) {
        return refuse("has an ATEX TEXR whose texture size is unknown");
      }
      // Remastered divides by the strides; a zero one never shipped.
      if (int32_t(words[0]) <= 0 || int32_t(words[1]) <= 0 || int32_t(words[2]) <= 0 || int32_t(words[3]) <= 0) {
        return refuse("has an ATEX TEXR with a tile or stride that is not positive");
      }
      AppendBE32(vatx, F("VATX"));
      AppendBE32(vatx, width);
      AppendBE32(vatx, height);
      for (size_t i = 0; i < 4; ++i) {
        AppendBE32(vatx, words[i]);
      }
      AppendBE32(vatx, words[4]);              // cycle frames
      AppendBE32(vatx, words[5] != 0 ? 1 : 0); // loop
      mati.textures["TEXR"] = MatiTexture{atex.args[0].guid, 0, 1, -1, -1};
      recipe.features = kColorTex;
      recipe.textures = {{"TEXR", Role::Color, -1, -1}};
      recipe.alphaGamma = 2.2f;
    } else if (header->textured && texr != nullptr && texr->value.size() == 1 &&
               IsElement(texr->value[0], F("ATX2"))) {
      // ATX2(id, cols, rows, cycle, loop): CUVEAnimTexture again (ctor 0x2cd3f4), a cols x rows grid
      // over the full size and its cycle an element GetValue evaluates at the particle's frame.
      const EffectValue& atx2 = texr->value[0];
      if (atx2.args.size() != 5 || atx2.args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has an ATX2 TEXR the port can't read");
      }
      uint32_t words[3];
      for (size_t i = 0; i < 3; ++i) {
        const EffectValue& arg = atx2.args[i == 2 ? 4 : i + 1];
        if (!IsElement(arg, F("CNST")) || arg.args.size() != 1 ||
            (arg.args[0].kind != EffectValue::Kind::Word && arg.args[0].kind != EffectValue::Kind::Byte)) {
          return refuse("has an ATX2 TEXR whose grid or loop is not constant");
        }
        words[i] = arg.args[0].word;
      }
      uint32_t width = 0, height = 0;
      if (!m_io.textureSize || !m_io.textureSize(atx2.args[0].guid, width, height) || width == 0 || height == 0) {
        return refuse("has an ATX2 TEXR whose texture size is unknown");
      }
      // A tag-1 TEXR's ILPT reads slot 0, the system's PSLT (see LifetimeOffset), which the port's
      // sprite loop puts in the lifetime globals around the cycle: offset 0.
      if (texr->tag != 1) {
        return refuse("has an ATX2 TEXR of tag " + std::to_string(texr->tag));
      }
      std::string why;
      std::vector<uint8_t> cycle;
      m_lifetimeOffset = 0;
      if (!Element(atx2.args[3], Type::Int, cycle, why)) {
        return refuse("has an ATX2 cycle the port can't evaluate: " + why);
      }
      AppendBE32(vatx, F("VAT2"));
      AppendBE32(vatx, width);
      AppendBE32(vatx, height);
      AppendBE32(vatx, words[0]);               // columns
      AppendBE32(vatx, words[1]);               // rows
      AppendBE32(vatx, words[2] != 0 ? 1 : 0);  // loop
      vatx.insert(vatx.end(), cycle.begin(), cycle.end());
      mati.textures["TEXR"] = MatiTexture{atx2.args[0].guid, 0, 1, -1, -1};
      recipe.features = kColorTex;
      recipe.textures = {{"TEXR", Role::Color, -1, -1}};
      recipe.alphaGamma = 2.2f;
    } else if (header->litPalette) {
      // TXP2(id, cols, rows, index): CUVETexturePalette (ctor 0x2cd940) cuts the texture into a
      // cols x rows grid (ints evaluated at load, at least 1) and GetValue (0x2cda3c) picks tile
      // clamp(index x count, 0, count - 1), the index a real element with no time term.
      if (texr == nullptr || texr->value.size() != 1 || !IsElement(texr->value[0], F("TXP2"))) {
        return refuse("has no TXP2 TEXR");
      }
      const EffectValue& txp2 = texr->value[0];
      if (txp2.args.size() != 4 || txp2.args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has a TXP2 TEXR the port can't read");
      }
      uint32_t grid[2];
      for (size_t i = 0; i < 2; ++i) {
        const EffectValue& arg = txp2.args[i + 1];
        if (!IsElement(arg, F("CNST")) || arg.args.size() != 1 ||
            (arg.args[0].kind != EffectValue::Kind::Word && arg.args[0].kind != EffectValue::Kind::Byte)) {
          return refuse("has a TXP2 TEXR whose grid is not constant");
        }
        grid[i] = std::max<int32_t>(1, int32_t(arg.args[0].word));
      }
      std::string why;
      std::vector<uint8_t> index;
      m_lifetimeOffset = LifetimeOffset(true, false, false, *texr);
      if (!Element(txp2.args[3], Type::Real, index, why)) {
        return refuse("has a TXP2 index the port can't evaluate: " + why);
      }
      AppendBE32(vatx, F("VTP2"));
      AppendBE32(vatx, grid[0]);
      AppendBE32(vatx, grid[1]);
      vatx.insert(vatx.end(), index.begin(), index.end());
      mati.textures["TEXR"] = MatiTexture{txp2.args[0].guid, 0, 1, -1, -1};
      recipe.features = kColorTex;
      recipe.textures = {{"TEXR", Role::Color, -1, -1}};
      recipe.alphaGamma = 2.2f;
      // AERO (CGenDescription+0x4e0, var 0x12; VS v6.z -> o2.x): a = max(0, pow(|t.a|, 2.2) - AERO),
      // the port's erosion on per-particle row 0.
      if (aero != nullptr) {
        if (aero->value.size() != 1) {
          return refuse("has an AERO the port can't read");
        }
        for (const EffectProperty& property : node.properties) {
          if (property.fourcc != F("PMTR")) {
            continue;
          }
          for (const EffectValue& item : property.value) {
            if ((item.word & 0xffff) == 0) {  // row 0, component 0
              return refuse("has an AERO and a PMTR on the same component");
            }
          }
        }
        m_lifetimeOffset = LifetimeOffset(true, false, false, *aero);
        AppendBE32(vpmt, 0);  // a real, row 0, component 0
        AppendBE32(vpmt, 0);
        AppendBE32(vpmt, 0);
        ++vpmtCount;
        if (!Element(aero->value[0], Type::Real, vpmt, why)) {
          return refuse("has an AERO the port can't evaluate: " + why);
        }
        recipe.features |= kErosion;
        recipe.sources = {{kSrcErosion, 0, 0}};
      }
    } else if (header->textured) {
      // Remastered's TEXR `CNST(id), NONE` or `CNST(id), TRST`; an atlas is not this perm's
      // single map.
      if (texr == nullptr || texr->value.size() != 2 || !IsElement(texr->value[0], F("CNST")) ||
          texr->value[0].args.size() != 1 || texr->value[0].args[0].kind != EffectValue::Kind::Guid) {
        return refuse("has no constant TEXR");
      }
      std::string why;
      if (!transform(texr->value[1], why)) {
        return refuse(why);
      }
      mati.textures["TEXR"] = MatiTexture{texr->value[0].args[0].guid, 0, 1, -1, -1};
      recipe.features = kColorTex;
      recipe.textures = {{"TEXR", Role::Color, -1, -1}};
      recipe.alphaGamma = 2.2f;
    }
    // VS 040_1 (the uber perm every room without a directional light selects) lights the colour per
    // vertex: (the probe + AMBS) x BLCM + the group-0 cluster sum, x 1/pi. BLIT without LIT_ is an
    // unread perm.
    if (blit != 0 && lit == 0) {
      return refuse("is BLIT without LIT_");
    }
    recipe.litPerVertex = lit != 0;
    if (!MaterialFrom(mati, recipe, EffectGuid{}, nullptr, node, result, false, false)) {
      return false;
    }
    if (!vtrs.empty()) {
      AppendBE32(result.part, F("VTRS"));
      result.part.insert(result.part.end(), vtrs.begin(), vtrs.end());
    }
    if (!vtrt.empty()) {
      AppendBE32(result.part, F("VTRT"));
      result.part.insert(result.part.end(), vtrt.begin(), vtrt.end());
    }
    result.part.insert(result.part.end(), vatx.begin(), vatx.end());
    // After MaterialFrom's VPMT, if any: a second list appends to it.
    if (!vpmt.empty()) {
      PutList(result.part, F("VPMT"), vpmtCount, vpmt);
    }
    return true;
  }

  bool MaterialFrom(const Mati& mati, const Recipe& recipeRef, const EffectGuid& matiGuid,
                    const EffectProperty* material, const EffectNode& node, ConvertedPart& result, bool swoosh,
                    bool electric) const {
    const Recipe* recipe = &recipeRef;
    char shaderText[16];
    std::snprintf(shaderText, sizeof(shaderText), "%08x", mati.shader);
    m_vmatShader = mati.shader;

    // The node's properties by fourcc.
    // Also takes the found property's lifetime context for the ILPTs written from it.
    auto find = [this, &node, swoosh, electric](uint32_t fourcc) -> const EffectProperty* {
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == fourcc) {
          m_lifetimeOffset = LifetimeOffset(!swoosh, swoosh && !electric, electric, property);
          return &property;
        }
      }
      return nullptr;
    };
    auto findUnique = [&node](uint32_t fourcc) -> const EffectProperty* {
      const EffectProperty* found = nullptr;
      for (const EffectProperty& property : node.properties) {
        if (property.fourcc == fourcc) {
          if (found != nullptr) {
            return nullptr;
          }
          found = &property;
        }
      }
      return found;
    };
    const auto cch0 = mati.vectors.count("CCH0") ? mati.vectors.at("CCH0") : std::array<float, 4>{};
    // A recipe read against one material permutation: only the MATI whose header flags select it is
    // converted. f197ec38's refraction is the MFC0 (USE_FRAME_BUFFER_DISTORTION) perm 002; a material
    // without MFC0 draws perm 000's displacement and one with MFC1 (IS_SHIELD_EXTERIOR, its own shader
    // b46bb66d) another perm still, so both are refused rather than drawn with perm 002's maths.
    if (recipe->mfc0Refraction && (!HasMatiFlag(mati, "MFC0") || HasMatiFlag(mati, "MFC1"))) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " is not its MFC0 refraction perm");
      return false;
    }
    // And the other way round: b1eb2b67's row is the non-refraction perm 000, which a material carrying
    // either shield flag leaves for the refraction (MFC0) or the exterior (MFC1) variant.
    if (recipe->noRefractionFlags && (HasMatiFlag(mati, "MFC0") || HasMatiFlag(mati, "MFC1"))) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText +
                               " carries a shield flag, so it is not its non-refraction perm");
      return false;
    }
    if (recipe->mfc1Exterior && (!HasMatiFlag(mati, "MFC1") || HasMatiFlag(mati, "MFC0"))) {
      m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " is not its MFC1 exterior material");
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
    if (const EffectProperty* ebdm = electric ? find(F("EBDM")) : nullptr) {
      // EBDM is a raw Byte. InitStaticRenderStateImpl (0xb72618) maps it by the table [0,2,1,0,0,0]
      // (0x1d1eebc) to the shared blend rows (0x45767bc): 0 alpha, 1 premultiplied, 2 additive. So
      // 1 is additive, 2 premultiplied, and 0 and 3-5 alpha. Depth is LEQUAL test, write off.
      uint32_t word = 0;
      if (SmallInteger(*ebdm, word) && word <= 5) {
        blend = word == 1 ? 2 : word == 2 ? 1 : 0;
      } else {
        m_approximated.push_back("VMAT: EBDM taken as alpha blend");
      }
    } else if (electric) {
      blend = 0;
    } else if (const EffectProperty* sbdm = swoosh ? find(F("SBDM")) : nullptr) {
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
    // The factors a sprite, swoosh or ELC2 draws with: CParticleSceneNode::BindGraphicsState 0x2b10e4 (from
    // RenderSpriteEntries 0x2b13a8 and RenderSwooshEntries 0x2b18b8, CParticleDrawParams -1) takes the row
    // above only while the node's material instance resource (node+0x148, set by
    // UpdateCustomMaterialReadyToRender 0x2b0ad4 for every generator with a MATI) has EBlendMode -1 (+0x24,
    // the header's u32 at 0x61); otherwise SBlendModeState(EBlendMode) (MatiBlend). The fog stays the row's
    // (static state bit 0x8000, SetupFogFromBlendMode on PBDM), pinned here as the shader keys it on the
    // blend. Model particles (PMDL) take FUN_002bed10's mesh states instead: the wave orb below.
    const bool model = !swoosh && std::any_of(node.properties.begin(), node.properties.end(),
                                              [](const EffectProperty& p) { return p.fourcc == F("PMDL"); });
    uint32_t fogPin = 0;
    if (!model && mati.headerBlend != kNoHeaderBlend) {
      const uint32_t matiBlend = MatiBlend(mati.headerBlend);
      if (matiBlend == 0xffffffffu) {
        m_approximated.push_back("VMAT: MATI EBlendMode " + std::to_string(int32_t(mati.headerBlend)) +
                                 " has no port blend, the row's taken");
      } else {
        const auto fogged = [](uint32_t b) { return b == 0 || b == 3; };
        if (fogged(blend) && !fogged(matiBlend)) {
          fogPin = kFogged;
        } else if (!fogged(blend) && fogged(matiBlend)) {
          fogPin = kUnfogged;
        }
        blend = matiBlend;
      }
    }
    // The wave beam's orb is the one known particle model whose MATI header blend
    // (EBlendMode 1, Remastered's One/One) wins over PBDM, which is 0 there: the
    // model draw passes -1 and resolves it to the MATI resource's mode
    // (CParticleSceneNode::RenderModelEntries, GetMaterialInstanceBlendMode).
    // Pinned to that exact MATI; no other MATI or shader is generalised from it.
    if (!swoosh && find(F("PMDL")) != nullptr && matiGuid == kWaveOrbMati && mati.shader == 0x642dd72b &&
        mati.headerBlend == 1) {
      blend = kBlendAdditiveOne;
    }
    // Its exposure: the source scales the orb by the inverse of the auto exposure and the post
    // multiplies it back, so the light it adds to the (already exposed) HDR scene is T * COLR * ITEN,
    // net exposure 1. Validated for this MATI only; every other effect keeps the room's exposure.
    // Its own test, not read off the blend: a later blend change must not move the policy.
    uint32_t features = recipe->features | fogPin;
    // Exact shipped custom model: MATI cull=-1 resolves to back at 0x2bd7f4..0x2bd810;
    // the sorted model draw binds it at 0x2bda84. No inference for other materials/sprites.
    if (model && matiGuid == kIceWedgeMati && mati.shader == 0x84e0fe6c && mati.headerBlend == 2) {
      features |= kModelBackCull;
    }
    // The Queen barrier's shield (f197ec38) and glow (b1eb2b67) models, and the exterior shield
    // (b46bb66d): MATI cull byte 2 (0xff resolves to 2 at 0x2bd7fc..0x2bd810) takes SetCullMode at
    // 0x2bda88 from 0x2bd904, Back (NRenderUtil::SetBackCullState 0xf32c0). Drawn cull none, their back
    // faces give 1 + n.v ~ 2 and flood the wall cyan. Cull 0/1 take an undecoded two-pass path; not
    // inferred here.
    if (model && (mati.shader == 0xf197ec38 || mati.shader == 0xb1eb2b67 || mati.shader == 0xb46bb66d) &&
        (mati.cull == 0xff || mati.cull >= 2)) {
      features |= kModelBackCull;
    }
    if (!swoosh && find(F("PMDL")) != nullptr && matiGuid == kWaveOrbMati && mati.shader == 0x642dd72b &&
        mati.headerBlend == 1) {
      features |= kAlreadyExposed;
    }
    // PowerBall's projectile/source material writes RGB multiplied by inverse
    // auto-exposure on the CPU; the source post restores that exposure. Require the exact
    // material, shader, header, model and source render-state fields: PBDM or a
    // similar ramp material alone does not prove exposure invariance.
    const EffectProperty* powerPmdl = findUnique(F("PMDL"));
    const EffectProperty* powerDvvn = findUnique(F("DVVN"));
    const EffectProperty* powerPbdm = findUnique(F("PBDM"));
    const EffectProperty* powerMtin = findUnique(F("MTIN"));
    const bool exactMatiProperty = material != nullptr && powerMtin == material && material->tag == 0 &&
                                   material->value.size() == 2 &&
                                   material->value[0].kind == EffectValue::Kind::Byte && material->value[0].word == 0 &&
                                   material->value[1].kind == EffectValue::Kind::Guid &&
                                   material->value[1].guid == kPowerBallMati;
    const bool hasIexp = std::any_of(node.properties.begin(), node.properties.end(),
                                     [](const EffectProperty& property) { return property.fourcc == F("IEXP"); });
    const auto constantIs = [](const EffectProperty* property, uint8_t expectedTag, uint32_t expected,
                               bool requireCnst, bool requireByte) {
      if (property == nullptr || property->tag != expectedTag || property->value.size() != 1) {
        return false;
      }
      const EffectValue* value = &property->value[0];
      if (requireByte) {
        return value->kind == EffectValue::Kind::Byte && value->word == expected;
      }
      if (IsElement(*value, F("CNST")) && value->args.size() == 1) {
        value = &value->args[0];
      } else if (requireCnst) {
        return false;
      }
      return (value->kind == EffectValue::Kind::Byte || value->kind == EffectValue::Kind::Word) &&
             value->word == expected;
    };
    const auto guidIs = [](const EffectProperty* property, const EffectGuid& expected) {
      if (property == nullptr || property->tag != 3 || property->value.size() != 1) {
        return false;
      }
      const EffectValue* value = &property->value[0];
      if (!IsElement(*value, F("CNST")) || value->args.size() != 1) {
        return false;
      }
      value = &value->args[0];
      return value->kind == EffectValue::Kind::Guid && value->guid == expected;
    };
    if (!swoosh && node.id == kPowerBallChild && exactMatiProperty && mati.shader == kPowerBallShader &&
        mati.headerBlend == kNoHeaderBlend && guidIs(powerPmdl, kPowerBallModel) &&
        constantIs(powerDvvn, 1, 4, false, true) && constantIs(powerPbdm, 1, 0, true, false) && !hasIexp) {
      features |= kAlreadyExposed;
    }
    // Separately audited held-charge sphere: DVVN4 defaults inverse exposure on and PBDM2
    // resolves additive usage. Its raw MATI also has header blend -1 (LE32 at 0x61).
    // This fixes only HDR exposure cancellation; LDR appearance and full parity remain unproven.
    // Do not change blend, recipe or the still-unrepresented SMTR alpha gain/bias here.
    const bool exactChargeMatiProperty = material != nullptr && powerMtin == material && material->tag == 0 &&
                                         material->value.size() == 2 &&
                                         material->value[0].kind == EffectValue::Kind::Byte && material->value[0].word == 0 &&
                                         material->value[1].kind == EffectValue::Kind::Guid &&
                                         material->value[1].guid == kPowerChargeMati;
    if (!swoosh && node.id == kPowerChargeChild && exactChargeMatiProperty && mati.shader == kPowerBallShader &&
        mati.headerBlend == kNoHeaderBlend && guidIs(powerPmdl, kPowerBallModel) &&
        constantIs(powerDvvn, 1, 4, false, true) && constantIs(powerPbdm, 1, 2, true, false) && !hasIexp) {
      features |= kAlreadyExposed;
    }
    // LIT_ 1 on a VFX_*_Lit sprite shader draws its lit perm (20e149b2 080_0..091_0): the unlit recipe's
    // colour x (AMBS + the baked probe + the clustered lights) x BLCM x PCMD / pi, alpha unchanged. The
    // runtime fills the light. The lit vertex perm (080_1) passes the particle colour through (o6 = v3) and
    // never reads the ITEN x inverse exposure scalar the vertex-lit perm 000_1 multiplies in (v2.z), so IEXP
    // has no effect: the light is radiance, exposed by the room as the scene is, and the runtime leaves ITEN
    // out of a lit particle's colour.
    uint32_t lit = 0;
    if (!swoosh && !electric && IsLitTailShader(mati.shader)) {
      if (const EffectProperty* litProperty = find(F("LIT_"))) {
        if (!SmallInteger(*litProperty, lit)) {
          m_approximated.push_back("VMAT: LIT_ taken as 0");
          lit = 0;
        }
      }
    }
    if (lit != 0 || recipe->litPerVertex) {
      features = (features | kParticleLit) & ~kAlreadyExposed;
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

    // VPRM: the recipe's CCH rows whole. Perm 000 (unfogged) is Remastered's draw where DFOG is a
    // constant 1; anywhere else it draws the fog perm (001), which only a fogPerm recipe has.
    std::vector<std::array<float, 4>> params;
    if (recipe->params > 0) {
      const EffectProperty* dfog = findUnique(F("DFOG"));
      uint32_t fogOff = 0;
      const bool unfogged = dfog != nullptr && dfog->value.size() == 1 &&
                            ((dfog->value[0].kind == EffectValue::Kind::Byte && dfog->value[0].word == 1) ||
                             (SmallConst(*dfog, fogOff) && fogOff == 1));
      if (recipe->alwaysUnfogged) {
        features |= kUnfogged;
      } else if (unfogged && recipe->fogPerm) {
        features |= kUnfogged;
      } else if (!unfogged && !recipe->fogPerm) {
        m_approximated.push_back(std::string("VMAT: shader ") + shaderText + " is drawn fogged (no DFOG 1), which its recipe lacks");
        return false;
      }
      for (int n = 0; n < recipe->params; ++n) {
        const std::string tag = "CCH" + std::to_string(n);
        const auto found = mati.vectors.find(tag);
        if (found == mati.vectors.end()) {
          m_approximated.push_back(std::string("VMAT: shader ") + shaderText + "'s MATI has no " + tag);
          return false;
        }
        params.push_back(found->second);
      }
    }

    // The blob.
    std::vector<uint8_t> blob;
    const bool opacityGainBias = recipe->cch0GainBias;
    const bool alphaGamma = recipe->alphaGamma > 0.0f || recipe->litPerVertex;
    AppendBE32(blob, recipe->litPerVertex ? 5 : alphaGamma ? 4 : opacityGainBias ? 3 : 2);
    AppendBE32(blob, features);
    AppendBE32(blob, blend);
    AppendBE32(blob, uint32_t(slots.size()));
    int32_t slotOf[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
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
    // depthSoften: SFTD, the softness distance Remastered's soft-particle shaders
    // divide the scene-depth gap by (desc+0x524, through the node's PZSO scalar).
    // Every SFTD in the shipped effects is a CNST; anything else (or none) is 0,
    // which the shader takes as no softening.
    uint32_t softness = 0;
    if (const EffectProperty* sftd = findUnique(F("SFTD")); sftd != nullptr && SmallConst(*sftd, softness)) {
      if (sftd->value[0].args[0].kind == EffectValue::Kind::Byte) {
        softness = FloatBits(float(softness));
      }
      if (!(FloatFromBits(softness) > 0.0f)) {
        softness = 0;
      }
    } else {
      softness = 0;
    }
    AppendBE32(blob, softness);  // depthSoften
    AppendBE32(blob, spriteCenter);
    if (opacityGainBias) {
      AppendBE32(blob, FloatBits(cch0[2]));
      AppendBE32(blob, FloatBits(cch0[3]));
    } else if (alphaGamma) {
      AppendBE32(blob, FloatBits(1.0f));  // v3's identity gain/bias
      AppendBE32(blob, FloatBits(0.0f));
    }
    if (alphaGamma) {
      AppendBE32(blob, FloatBits(recipe->alphaGamma));
    }
    if (recipe->litPerVertex) {
      AppendBE32(blob, 1);  // v5 flags: bit 0 per-vertex light
    }

    AppendBE32(result.part, F("VMAT"));
    AppendBE32(result.part, F("CNST"));
    AppendBE32(result.part, uint32_t(blob.size()));
    result.part.insert(result.part.end(), blob.begin(), blob.end());
    if (recipe->params > 0) {
      // v1: version, the NMAP slot, n, rows; v2 (a recipe with a TCH0 env map) adds the env slot after it.
      const bool env = slotOf[size_t(Role::Env)] >= 0;
      std::vector<uint8_t> prm;
      AppendBE32(prm, env ? 2 : 1);
      AppendBE32(prm, uint32_t(slotOf[size_t(Role::Normal)]));
      if (env) {
        AppendBE32(prm, uint32_t(slotOf[size_t(Role::Env)]));
      }
      AppendBE32(prm, uint32_t(params.size()));
      for (const std::array<float, 4>& row : params) {
        for (const float f : row) {
          AppendBE32(prm, FloatBits(f));
        }
      }
      AppendBE32(result.part, F("VPRM"));
      AppendBE32(result.part, F("CNST"));
      AppendBE32(result.part, uint32_t(prm.size()));
      result.part.insert(result.part.end(), prm.begin(), prm.end());
    }

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
      constexpr uint32_t kCch0 = F("CCH0");
      for (const EffectValue& item : smtr->value) {
        // A VPRM row's component: its own target (kPortVfxParamTarget + 4 n + component).
        const uint32_t cch = item.word - kCch0;
        // A colour (group 4) entry sets the whole row: a CNST of four reals, one target each.
        if (cch < uint32_t(recipe->params) && item.fourcc == 4 && item.args.size() == 2) {
          const EffectValue& vec = item.args[0];
          if (vec.kind != EffectValue::Kind::Element || vec.fourcc != F("CNST") || vec.args.size() != 4) {
            result.dropped.push_back("SMTR: CCH" + std::to_string(cch) + " colour not four constants");
            continue;
          }
          for (uint32_t c = 0; c < 4; ++c) {
            std::vector<uint8_t> element;
            std::string why;
            if (!Element(vec.args[c], Type::Real, element, why)) {
              result.dropped.push_back("SMTR: " + why);
              continue;
            }
            AppendBE32(items, kPortVfxParamTarget + 4 * cch + c);
            AppendBE32(items, 0);
            items.insert(items.end(), element.begin(), element.end());
            ++count;
          }
          continue;
        }
        // A vector (group 2) entry sets three components from its own: DOML (the dominant light
        // direction) as a PDML each, or a CNST of three reals.
        if (cch < uint32_t(recipe->params) && item.fourcc == 2 && item.args.size() == 2) {
          const EffectValue& vec = item.args[0];
          const uint32_t comp = item.args[1].word >> 8 & 0xff;
          const bool doml = vec.kind == EffectValue::Kind::Element && vec.fourcc == F("DOML") && vec.args.empty();
          const bool cnst = vec.kind == EffectValue::Kind::Element && vec.fourcc == F("CNST") && vec.args.size() == 3;
          if (comp > 1 || (!doml && !cnst)) {
            result.dropped.push_back("SMTR: CCH" + std::to_string(cch) + " vector " +
                                     (vec.kind == EffectValue::Kind::Element ? EffectFourCCString(vec.fourcc) : "value") +
                                     " at component " + std::to_string(comp));
            continue;
          }
          for (uint32_t c = 0; c < 3; ++c) {
            std::vector<uint8_t> element;
            if (doml) {
              AppendBE32(element, F("PDML"));
              AppendBE32(element, c);
            } else {
              std::string why;
              if (!Element(vec.args[c], Type::Real, element, why)) {
                result.dropped.push_back("SMTR: " + why);
                continue;
              }
            }
            AppendBE32(items, kPortVfxParamTarget + 4 * cch + comp + c);
            AppendBE32(items, 0);
            items.insert(items.end(), element.begin(), element.end());
            ++count;
          }
          continue;
        }
        if (cch < uint32_t(recipe->params) && item.args.size() == 2) {
          const uint32_t comp = item.args[1].word >> 8 & 0xff;
          std::vector<uint8_t> element;
          std::string why;
          if (comp >= 4) {
            result.dropped.push_back("SMTR: CCH" + std::to_string(cch) + " component " + std::to_string(comp));
          } else if (!Element(item.args[0], Type::Real, element, why)) {
            result.dropped.push_back("SMTR: " + why);
          } else {
            AppendBE32(items, kPortVfxParamTarget + 4 * cch + comp);
            AppendBE32(items, 0);
            items.insert(items.end(), element.begin(), element.end());
            ++count;
          }
          continue;
        }
        if (item.word != kCch0 || item.args.size() != 2) {
          result.dropped.push_back("SMTR: " + std::string(item.word == kCch0 ? "an entry" : "a parameter") +
                                   " VSMT cannot take");
          continue;
        }
        const int comp = int(item.args[1].word >> 8 & 0xff);
        std::vector<uint32_t> targets;
        if (opacityGainBias && comp == 2) {
          targets.push_back(kPortVfxOpacityGainTarget);
        } else if (opacityGainBias && comp == 3) {
          targets.push_back(kPortVfxOpacityBiasTarget);
        }
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
          char shader[9];
          std::snprintf(shader, sizeof(shader), "%08x", recipe->shader);
          result.dropped.push_back("SMTR: CCH0 component " + std::to_string(comp) + " is unused by shader " + shader);
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

    // ITEN is a real element as it is (the generator writes SSZE); VORN is ORNT's byte.
    if (const EffectProperty* property = find(F("ITEN"))) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (property->value.size() == 1 && Element(property->value[0], Type::Real, bytes, why)) {
        AppendBE32(result.part, F("ITEN"));
        result.part.insert(result.part.end(), bytes.begin(), bytes.end());
      } else {
        result.dropped.push_back("ITEN: " + (why.empty() ? std::string("not one element") : why));
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
  // False when the block is malformed (m_vars is then empty).
  bool ReadVariables(const EffectValue& raw) const {
    if (raw.size < 14) {
      return false;
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
      return false;
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
          return false;
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
    return true;
  }

  struct PartVar {
    uint32_t id;           // 0..8 ADV1..9 (a swoosh's ADV0..8), 9..12 IAV0..3, 13..16 VAV0..3, 17..20 CAV0..3
    uint32_t reads = 0;    // the variables its tree reads (m_varReads)
    uint32_t flags = 0;    // ConstantFlags
    bool converted = false;
    size_t begin = 0, end = 0;     // ADV1..8: its retail property in `out` (to take back out)
    std::vector<uint8_t> element;  // ADV9, IAVn, VAVn, CAVn: the converted tree
  };
  static Type PartVarType(uint32_t id) {
    return id >= 17 ? Type::Color : id >= 13 ? Type::Vector : id >= 9 ? Type::Int : Type::Real;
  }
  static std::string PartVarName(uint32_t id, bool swoosh) {
    if (id < 9) {
      return "ADV" + std::to_string(swoosh ? id : id + 1);
    }
    const char* kinds[] = {"IAV", "VAV", "CAV"};
    return kinds[(id - 9) / 4] + std::to_string((id - 9) % 4);
  }
  // ADV1..9 are 0..8, IAV0..3 9..12, VAV0..3 13..16, CAV0..3 17..20 (CreateGPSM AddDefinedVar;
  // no shipped element reads a VAV or CAV, but they run, and a RAND in one draws), anything
  // else -1.
  static int PartVarId(uint32_t fourcc) {
    for (uint32_t n = 0; n < 9; ++n) {
      const char name[5] = {'A', 'D', 'V', char('1' + n), 0};
      if (fourcc == F(name)) {
        return int(n);
      }
    }
    for (uint32_t n = 0; n < 4; ++n) {
      const char name[5] = {'I', 'A', 'V', char('0' + n), 0};
      const char vav[5] = {'V', 'A', 'V', char('0' + n), 0};
      const char cav[5] = {'C', 'A', 'V', char('0' + n), 0};
      if (fourcc == F(name)) {
        return int(9 + n);
      }
      if (fourcc == F(vav)) {
        return int(13 + n);
      }
      if (fourcc == F(cav)) {
        return int(17 + n);
      }
    }
    return -1;
  }
  // A swoosh's per-point variables (CreateSWSH 0x2db8a4 AddDefinedVar: its ADVn is real id n,
  // read by PAP(n + 1), unlike a GPSM's ADVn): ADV0..8 are 0..8, IAV/VAV/CAV as PartVarId,
  // anything else -1.
  static int SwooshVarId(uint32_t fourcc) {
    for (uint32_t n = 0; n < 9; ++n) {
      const char name[5] = {'A', 'D', 'V', char('0' + n), 0};
      if (fourcc == F(name)) {
        return int(n);
      }
    }
    const int id = PartVarId(fourcc);
    return id >= 9 ? id : -1;
  }

  // What Generator writes and the properties it reads set, shared by the functions it is split
  // into.
  struct GeneratorState {
    GeneratorState(const EffectNode& node, uint32_t type)
        : node(node), type(type), retail(PropertiesOf(type)) {}
    bool Has(uint32_t fourcc) const {
      return std::any_of(node.properties.begin(), node.properties.end(),
                         [&](const EffectProperty& property) { return property.fourcc == fourcc; });
    }
    const EffectNode& node;
    const uint32_t type;
    ConvertedPart result;
    std::vector<uint8_t>& out = result.part;
    const PropertyTable& retail;
    const bool part = type == F("PART");
    bool inverseExposed = false;
    const bool swoosh = type == F("SWHC");
    // An ELC2 (CParticleElectric2) is written as an ELSC stream that starts with the port-only PEL2
    // marker and carries the ELC2 fields; it draws through its VMAT like a swoosh.
    const bool electric = type == F("ELSC") && node.form == F("ELC2");
    // Remastered builds a real scene light for every generator with a point or spot LTYP, from
    // LCLR*LINT, LIRD/LORD and LFOT (and a spot's LIAN/LOAN cone). Those become the port-only
    // PHDR/PIR0/POR0/PHFO (PLIA/PLOA) that CElementGen carries as an HDR light.
    std::vector<uint8_t> nestedLightInnerRadius;
    std::vector<uint8_t> nestedLightOuterRadius;
    bool nestedHdrLight = false;
    uint32_t nestedLightFalloff = 3;
    // LIXP: CParticleLight::Create 0x2df010 multiplies LINT by the static inverse exposure.
    bool nestedLightInverseExposure = false;
    // LSPS: the light block's +0x50, CLight+0x74, which widens the light's specular lobe
    // (roughness r + (1 - r) * LSPS in the PBR shaders). A CNST real's bits; 0 = none.
    uint32_t nestedLightWiden = 0;
    // LIAN/LOAN of a spot LTYP (raw 3): the full cone angles in degrees, CNST real bits.
    bool nestedLightSpot = false;
    uint32_t nestedLightInnerAngle = 0;
    uint32_t nestedLightOuterAngle = 0;
    // The light is built once and never updated (port-only PLON); see NestedLight.
    bool lightOnce = false;
    // KILL is a constant true (port-only PKIL); see the KILL consume.
    bool kill = false;
    // FXBR/FXBM/FXBO/FXBI, the system's own bounds (FixedBounds): the constant box in the port's
    // space (min then max), written as PFXB, and whether children's bounds are left out (PFXC).
    std::optional<std::array<float, 6>> fixedBounds;
    bool boundsNoChildren = false;
    bool boundsConsumed = false;
    // Swooshes: SBDM is Remastered's blend mode, 0 or 1, which retail has as
    // AALP (additive). Taken only where there's no AALP of its own.
    bool additive = false;
    // A generator's SVRn/SVIn/SVCn/SVVn: CAddVarSystem's own variables, which
    // PSAn and children's SPA* read. Written as the port-only PSYS, in the order
    // CElementGen evaluates them: an item after the reals it reads (PSAn of
    // another SVRn), else by property order, each after its hidden init-only
    // reals. [I] the order of RAND draws among items is not proven.
    std::vector<SystemItem> sysItems;
    // The particle variables (GPSM ADV1..9, IAV0..3) the node declares, in stream order.
    std::vector<PartVar> partVars;
    bool texture = false;
    uint32_t pmdl = 0;          // the PMDL's id, when it is one constant
    bool pmdlVariants = false;  // a PMDV was written (a mesh would cover only one model)
    std::vector<EffectGuid> pmdlModels;  // every model the PMDL can pick, as stored
    bool pmph = false;                   // a PMPH was read
    std::vector<Started> started;  // by the spawn table
    std::set<uint32_t> written;    // retail properties
    // Remastered's visor effect data (CGenDescription::Compile 0x2a7d0c): any VE* property marks
    // it present; the VisorGoo splat manager reads the size range VESN/VESM, the visor effect
    // override manager the colours VESC/VEMC/VETC, the blend VEBS and the duration VEDT.
    bool visorData = false;
    std::optional<uint32_t> visorMinScale;
    std::optional<uint32_t> visorMaxScale;
    std::vector<uint8_t> visorStartColor;
    std::vector<uint8_t> visorMidColor;
    std::vector<uint8_t> visorEndColor;
    std::vector<uint8_t> visorBlend;
    std::optional<uint32_t> visorTicks;
    const EffectProperty* material = nullptr;
    const EffectProperty* xfmd = nullptr;
    uint32_t spriteShape = 0;
    bool unfogged = false;  // DFOG 1, port-only PDFG
    bool noZTest = false;   // ZTES 0, port-only PZTS
    bool noIvse = false;    // IVSE 0, port-only PIVS
    uint32_t waterSort = 0;  // WSTP 1/2, port-only PWST
    int spreadMode = -1;    // ITPE 1, port-only PITP with EMRV's mode word (-1 none)
    bool itpe = false;      // the node has an ITPE of its own
    uint32_t depthBias = 0;  // DBIS as float bits (nonzero), port-only PDBS
    bool depthBiasUsesDepthRange = true;  // DBSO (desc+0x6b bit 2), port-only PDBO when false
    uint32_t sortBias = 0;   // SBIS as float bits (nonzero), port-only PSBS
    uint32_t sleepDistance = 0;  // a nonzero constant SLDS as float bits, port-only PSLD
    const EffectProperty* mover = nullptr;
    const EffectProperty* emitterMover = nullptr;  // EMVR, written as PEMR after the loop
    std::vector<uint32_t> portOnly;  // material data, written only with a VMAT
    bool drawsSsze = false;          // an SSZE that shapes the quad, which only a VMAT draws
    bool anth = false;               // an ANTH, drawn only by a header material
    const bool textured = retail.count(F("TEXR")) != 0;
    // SetupPrimitiveTypes 0x29069c and FixAndClean 0x2908a4 select geometry,
    // independently of textures/materials. Primitive 2 is a transformed square:
    // table 0x1d0b9b4 maps 1/2/3 to render kinds 1/2/0, and Render 0x2b23e8
    // sends kinds 1 AND 2 to RenderSpriteEntries. Nil BCLR is opaque black.
    const bool inactivePrimitive = part && (node.primitiveType == 0 || node.primitiveType == 4 ||
                                           node.primitiveType == 5 ||
                                           (node.primitiveType == 3 && !Has(F("PMDL"))));
    const bool customMaterial = std::any_of(node.properties.begin(), node.properties.end(), [](const auto& p) {
      return p.fourcc == F("MTIN") && std::any_of(p.value.begin(), p.value.end(), [](const auto& v) {
        return v.kind == EffectValue::Kind::Guid && v.guid != EffectGuid{};
      });
    });
    // RenderSpriteEntries 0x2b13a8 requires node+0x140. The ctor 0x28bfc4
    // loads either nonnil MTIN or the header MTRL; TEXR cannot supply it.
    // This gates drawing only: active SIZE and child/light evaluation survive.
    const bool nullMaterial = part && (node.primitiveType == 1 || node.primitiveType == 2) &&
                              node.baseMaterial == EffectGuid{} && !customMaterial;
    const bool drawsNothing = inactivePrimitive || nullMaterial;
  };

  ConvertedPart Generator(const EffectNode& node, uint32_t type) const {
    GeneratorState s(node, type);
    s.result.id = node.id;
    s.result.type = type;
    s.result.root = node.root;
    AppendBE32(s.out, HeaderOf(type));
    if (s.part) {
      // The loader must know IRND is particle-initial before folding any expressions.
      AppendBE32(s.out, F("PIRN"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    struct KeepScope {
      const Writer& writer;
      KeepScope(const Writer& w, bool on) : writer(w) {
        writer.m_keeps = on;
        writer.m_keepNext = 0;
      }
      ~KeepScope() { writer.m_keeps = false; }
    } keepScope(*this, s.part || s.swoosh);
    if (s.electric) {
      AppendBE32(s.out, F("PEL2"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    if (type == F("ELSC")) {
      // Every converted electric, ELSM-form ones too, which otherwise read like retail streams.
      AppendBE32(s.out, F("PIRN"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    NestedLight(s);
    FixedBounds(s);
    NodeModes(s);
    ReadSystemItems(s);
    s.result.drawsNothing = s.drawsNothing;
    for (const EffectProperty& property : node.properties) {
      GeneratorProperty(s, property);
    }
    DropVariableReaders(s);
    DrawWrites(s);
    SpawnedSwooshes(s);
    StateWrites(s);
    ParticleVariableWrites(s);
    // Without a PSLT, Remastered's system lifetime is 0x1000000 (CElementGen ctor 0x244cfc), which
    // an ILPT in a system property reads; retail's default is 0x7FFFFF.
    if (s.part && s.written.count(F("PSLT")) == 0) {
      AppendBE32(s.out, F("PSLT"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 0x1000000u);
    }
    if (s.swoosh) {
      // Swoosh marker ordering/semantics stay unchanged: colour runs over the live points
      // (KEWS), not over LENG.
      AppendBE32(s.out, F("PIRN"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    VisorWrites(s);
    const uint32_t guidWord = uint32_t(s.node.id[0]) | uint32_t(s.node.id[1]) << 8 |
                              uint32_t(s.node.id[2]) << 16 | uint32_t(s.node.id[3]) << 24;
    if ((s.part || s.swoosh) && guidWord != 0) {
      // Port-only: the first word of the effect's GUID (little-endian, as the Switch reads it). A
      // Remastered child's seed hashes it in (CElementGen::PortChildSeed); absent means 0.
      AppendBE32(s.out, F("PGID"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, guidWord);
    }
    NestedLightWrites(s);
    if (s.kill) {
      AppendBE32(s.out, F("PKIL"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    if (s.fixedBounds) {
      AppendBE32(s.out, F("PFXB"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 6);
      for (const float value : *s.fixedBounds) {
        AppendBE32(s.out, FloatBits(value));
      }
    }
    if (s.boundsNoChildren) {  // absent PFXC takes the children's bounds in
      AppendBE32(s.out, F("PFXC"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    AppendBE32(s.out, F("_END"));
    s.result.approximated = std::move(m_approximated);
    m_approximated.clear();
    return std::move(s.result);
  }

  // The nested HDR light of a point or spot LTYP (the state's nestedLight*).
  void NestedLight(GeneratorState& s) const {
    if (s.part) {
      auto unique = [&s](uint32_t fourcc) -> const EffectProperty* {
        const EffectProperty* found = nullptr;
        for (const EffectProperty& property : s.node.properties) {
          if (property.fourcc == fourcc) {
            if (found != nullptr) {
              return nullptr;
            }
            found = &property;
          }
        }
        return found;
      };
      const EffectProperty* ltyp = unique(F("LTYP"));
      const EffectProperty* lfot = unique(F("LFOT"));
      const EffectProperty* lclr = unique(F("LCLR"));
      const EffectProperty* lint = unique(F("LINT"));
      const EffectProperty* lird = unique(F("LIRD"));
      const EffectProperty* lord = unique(F("LORD"));
      uint32_t lightType = 0;
      uint32_t falloff = 0;
      std::string why;
      std::vector<uint8_t> colorProbe;
      std::vector<uint8_t> intensityProbe;
      // Raw LTYP 2 is a point, 3 a spot (GetLight 0x2df3f0's type byte 1 / anything else).
      const bool typed = ltyp != nullptr && SmallInteger(*ltyp, lightType) && (lightType == 2 || lightType == 3);
      const bool spot = typed && lightType == 3;
      const EffectProperty* lian = spot ? unique(F("LIAN")) : nullptr;
      const EffectProperty* loan = spot ? unique(F("LOAN")) : nullptr;
      const bool cone = !spot || (lian != nullptr && loan != nullptr && lian->value.size() == 1 &&
                                  loan->value.size() == 1 && ConstWord(lian->value[0], s.nestedLightInnerAngle) &&
                                  ConstWord(loan->value[0], s.nestedLightOuterAngle));
      s.nestedHdrLight = typed && cone && lfot != nullptr && lird != nullptr && lord != nullptr &&
                       lclr != nullptr && lint != nullptr && lclr->value.size() == 1 &&
                       lint->value.size() == 1 && SmallInteger(*lfot, falloff) &&
                       falloff <= 3 && lird->value.size() == 1 && lord->value.size() == 1 &&
                       Element(lclr->value[0], Type::Color, colorProbe, why) &&
                       Element(lint->value[0], Type::Real, intensityProbe, why) &&
                       Element(lird->value[0], Type::Real, s.nestedLightInnerRadius, why) &&
                       Element(lord->value[0], Type::Real, s.nestedLightOuterRadius, why);
      if (s.nestedHdrLight) {
        s.nestedLightFalloff = falloff;
        s.nestedLightSpot = spot;
        const EffectProperty* lixp = unique(F("LIXP"));
        uint32_t inverseExposed = 0;
        s.nestedLightInverseExposure = lixp != nullptr && SmallInteger(*lixp, inverseExposed) && inverseExposed != 0;
        // A non-constant LSPS isn't consumed and stays in the dropped report.
        const EffectProperty* lsps = unique(F("LSPS"));
        if (lsps == nullptr || lsps->value.size() != 1 || !ConstWord(lsps->value[0], s.nestedLightWiden)) {
          s.nestedLightWiden = 0;
        }
      } else if (typed) {
        s.nestedLightInnerAngle = 0;
        s.nestedLightOuterAngle = 0;
        s.result.dropped.push_back(spot ? "PHDR: spot light lacks constant LFOT/LIAN/LOAN or supported "
                                          "LCLR/LINT/LIRD/LORD"
                                        : "PHDR: point light lacks constant LFOT or supported LCLR/LINT/LIRD/LORD");
      }
      // CParticleLight::Create 0x2def58 builds every generator's light in its ctor; only
      // TickSystem_Light 0x24af3c calls CParticleLight::Update. Compile 0x290e4c picks that tick
      // for a type-4 generator (desc+0x318) that makes no particles: desc+0x31c bit 2, set by
      // SParticle::CompileSystem 0x290aa8 for no LTME, a constant LTME 0, or no GRTE (desc+0x2fa
      // bit 1, SEmitter::CompileSystem 0x28e00c). Any other generator keeps its first values.
      const EffectProperty* ltme = unique(F("LTME"));
      if (ltme == nullptr) {
        ltme = unique(F("LTM2"));
      }
      const bool noParticles = ltme == nullptr || (ltme->value.size() == 1 && ConstIs(ltme->value[0], 0)) ||
                               !s.Has(F("GRTE"));
      s.lightOnce = ltyp != nullptr && !(s.node.primitiveType == 3 && noParticles);
    }
  }

  // The system's own bounds. SSystem::CompileSystem 0x28d028: FXBM and FXBO make a box (whatever
  // FXBR), else FXBR a sphere; either takes CBoundsFixedEval (CElementGen::InitBounds 0x2455cc),
  // whose SolveFunc 0x27fe9c bounds m..m+o (FXBM, FXBO) or o-r..o+r (FXBO, absent 0; FXBR) and
  // turns it by the system's T*R*S. Children's bounds join only with FXBI 0 (desc+0x1b8 bit 13 set,
  // bit 12 clear); without bounds FXBI 1 leaves them out of the particles' bounds. Constants only,
  // and no FXBT (the emitter transform), which no shipped effect has.
  void FixedBounds(GeneratorState& s) const {
    if (!s.part || s.Has(F("FXBT"))) {
      return;
    }
    const auto unique = [&s](uint32_t fourcc, const EffectProperty*& found) {
      found = nullptr;
      for (const EffectProperty& property : s.node.properties) {
        if (property.fourcc == fourcc) {
          if (found != nullptr) {
            return false;
          }
          found = &property;
        }
      }
      return true;
    };
    const auto real = [](const EffectProperty& property, float& value) {
      uint32_t word = 0;
      if (property.value.size() != 1 || !ConstWord(property.value[0], word)) {
        return false;
      }
      value = FloatFromBits(word);
      return true;
    };
    const auto vector = [&real](const EffectProperty& property, std::array<float, 3>& value) {
      if (property.value.size() != 1 || !IsElement(property.value[0], F("CNST")) ||
          property.value[0].args.size() != 3) {
        return false;
      }
      for (size_t i = 0; i < 3; ++i) {
        uint32_t word = 0;
        if (!ConstWord(property.value[0].args[i], word)) {
          return false;
        }
        value[i] = FloatFromBits(word);
      }
      return true;
    };
    const EffectProperty *fxbr, *fxbm, *fxbo, *fxbi;
    if (!unique(F("FXBR"), fxbr) || !unique(F("FXBM"), fxbm) || !unique(F("FXBO"), fxbo) ||
        !unique(F("FXBI"), fxbi)) {
      return;
    }
    uint32_t ignoreChildren = 0;
    if (fxbi != nullptr && !SmallInteger(*fxbi, ignoreChildren)) {
      return;
    }
    std::array<float, 3> lo{}, hi{};
    if (fxbm != nullptr && fxbo != nullptr) {
      std::array<float, 3> corner, extent;
      if (!vector(*fxbm, corner) || !vector(*fxbo, extent)) {
        return;
      }
      for (size_t i = 0; i < 3; ++i) {
        lo[i] = std::min(corner[i], corner[i] + extent[i]);
        hi[i] = std::max(corner[i], corner[i] + extent[i]);
      }
    } else if (fxbr != nullptr) {
      float radius = 0.f;
      std::array<float, 3> offset{};
      if (!real(*fxbr, radius) || (fxbo != nullptr && !vector(*fxbo, offset))) {
        return;
      }
      for (size_t i = 0; i < 3; ++i) {
        lo[i] = std::min(offset[i] - radius, offset[i] + radius);
        hi[i] = std::max(offset[i] - radius, offset[i] + radius);
      }
    }
    s.boundsConsumed = true;  // an FXBM or FXBO without the other and no FXBR is never read
    if (fxbr == nullptr && (fxbm == nullptr || fxbo == nullptr)) {
      s.boundsNoChildren = ignoreChildren != 0;
      return;
    }
    // Bit 12 is set when FXBI didn't specify it.
    s.boundsNoChildren = fxbi == nullptr || ignoreChildren != 0;
    // (x, y, z) to the port's (-x, z, y), an axis-aligned box to an axis-aligned box.
    s.fixedBounds = std::array<float, 6>{-hi[0], lo[2], lo[1], -lo[0], hi[2], hi[1]};
  }

  // The node-wide modes its properties are read under: AALP, MAXP, XFMD, POFS and the PVAR table.
  void NodeModes(GeneratorState& s) const {
    for (const EffectProperty& property : s.node.properties) {
      s.additive = s.additive || property.fourcc == F("AALP");
    }
    m_swoosh = s.node.form == F("SWSH");
    m_swooshVars = 0;
    for (const EffectProperty& property : s.node.properties) {
      const int id = m_swoosh ? SwooshVarId(property.fourcc) : -1;
      m_swooshVars |= id >= 0 ? 1u << id : 0u;
    }
    m_gpsm = s.node.form == F("GPSM");
    m_xfmd = 2;
    m_pofs = 0;
    m_maxp = -1;
    for (const EffectProperty& property : s.node.properties) {
      uint32_t maxp;
      if (property.fourcc == F("MAXP") && property.value.size() == 1 && ConstWord(property.value[0], maxp)) {
        m_maxp = int64_t(maxp);
      }
    }
    for (const EffectProperty& property : s.node.properties) {
      uint32_t mode;
      if (property.fourcc == F("XFMD") && SmallConst(property, mode)) {
        m_xfmd = mode;
      }
      if (property.fourcc == F("POFS") && !property.value.empty()) {
        m_pofs = std::max(m_pofs, IsConstantValue(property.value[0]) ? 1u : 2u);
      }
    }
    m_partSys = m_gpsm ? PartSystemMode(s.node, m_partSysWhy) : -1;
    if (m_partSys == 1) {
      m_xfmd = 1;
    }
    // An INTT written as POFS (below, IntIsPofs) is the offset EMRV adds.
    for (const EffectProperty& property : s.node.properties) {
      if (property.fourcc == F("INTT") && !property.value.empty() && IntIsPofs(property)) {
        m_pofs = std::max(m_pofs, IsConstantValue(property.value[0]) ? 1u : 2u);
      }
    }
    // Only a generator or swoosh carries the PVRT table its VARF/VARI/VARV/VARC read; an ELC2's
    // own variable reads are refused ("not in the effect's PVAR").
    m_vars.clear();
    m_varsRead = false;
    for (const EffectProperty& property : s.node.properties) {
      if ((s.part || s.swoosh) && property.fourcc == F("PVAR") && property.value.size() == 1 &&
          property.value[0].kind == EffectValue::Kind::Raw) {
        m_varsRead = ReadVariables(property.value[0]);
      }
    }
  }

  // The system variables (the state's sysItems), in the order CElementGen evaluates them.
  void ReadSystemItems(GeneratorState& s) const {
    if (s.part || s.swoosh) {
      struct Pending {
        const EffectProperty* property;
        uint32_t type;
        uint32_t n;
        uint32_t reads;
      };
      std::vector<Pending> pending;
      uint32_t writtenReals = 0;
      for (const EffectProperty& property : s.node.properties) {
        const auto [varType, n] = SystemVarWrite(property.fourcc);
        if (varType == ~0u) {
          continue;
        }
        uint32_t reads = 0;
        for (const EffectValue& value : property.value) {
          reads |= SystemReadsOf(value);
        }
        pending.push_back({&property, varType, n, reads});
        if (varType == 0) {
          writtenReals |= 1u << n;
        }
      }
      m_sysNextHidden = 10;
      m_sysNextHiddenInt = 10;
      uint32_t doneReals = 0;
      std::vector<bool> done(pending.size(), false);
      for (size_t emitted = 0; emitted < pending.size(); ++emitted) {
        size_t pick = pending.size();
        for (size_t i = 0; i < pending.size() && pick == pending.size(); ++i) {
          const uint32_t self = pending[i].type == 0 ? 1u << pending[i].n : 0;
          if (!done[i] && (pending[i].reads & writtenReals & ~doneReals & ~self) == 0) {
            pick = i;
          }
        }
        for (size_t i = 0; i < pending.size() && pick == pending.size(); ++i) {
          if (!done[i]) {
            pick = i;  // a cycle: property order
          }
        }
        done[pick] = true;
        const Pending& item = pending[pick];
        if (item.type == 0) {
          doneReals |= 1u << item.n;
        }
        static constexpr Type kTypes[4] = {Type::Real, Type::Int, Type::Color, Type::Vector};
        const std::string name = EffectFourCCString(item.property->fourcc);
        std::vector<SystemItem> items;
        SystemItem visible{item.type, item.n, 0, {}};
        std::string why;
        const uint32_t hidden = m_sysNextHidden;
        m_sysItems = &items;
        const int lifetimeOffset = m_lifetimeOffset;
        m_lifetimeOffset = LifetimeOffset(true, s.swoosh, s.electric, *item.property);
        const bool ok = item.property->value.size() == 1 &&
                        Element(item.property->value[0], kTypes[item.type], visible.element, why);
        m_lifetimeOffset = lifetimeOffset;
        m_sysItems = nullptr;
        if (!ok) {
          m_sysNextHidden = hidden;
          s.result.dropped.push_back(name + ": " + (why.empty() ? "not one element" : why));
          continue;
        }
        items.push_back(std::move(visible));
        for (SystemItem& sys : items) {
          s.sysItems.push_back(std::move(sys));
        }
      }
    }
  }

  // One of the node's properties, in stream order.
  void GeneratorProperty(GeneratorState& s, const EffectProperty& property) const {
    uint32_t fourcc = property.fourcc;
    m_lifetimeOffset = LifetimeOffset(s.part, s.swoosh, s.electric, property);
    if (RenderStateProperty(s, property, fourcc)) {
      return;
    }
    if (s.part && fourcc == F("LTM2")) {
      fourcc = F("LTME");
    } else if (s.type == F("SWHC") && fourcc == F("SBDM") && !s.additive) {
      fourcc = F("AALP");
    }
    if (StructureProperty(s, property, fourcc)) {
      return;
    }
    const int varId = s.part ? PartVarId(fourcc) : s.swoosh ? SwooshVarId(fourcc) : -1;
    // ADV9 and IAV0..3, and all of a swoosh's, have no retail property: they are written in the
    // port-only PPVR.
    if (varId >= 8 || (s.swoosh && varId >= 0)) {
      // VAVn = PWSL (744cc8aa): CVEParticleWorldSpaceLocation (0x2a1d68, built by GetVectorElement
      // 0x251d94) transforms the particle's position, drawing no RNG and keeping no state, and only
      // PTVA/BSVA/SYVA (0x2520ac) read a vector variable back, which no shipped effect has: it is
      // never observed and converts to nothing.
      if (PartVarType(varId) == Type::Vector && property.value.size() == 1 && IsElement(property.value[0], F("PWSL")) &&
          property.value[0].args.empty()) {
        return;
      }
      ParticleVariable(s, property, varId);
      return;
    }
    if (PortOnlyProperty(s, property, fourcc)) {
      return;
    }
    RetailProperty(s, property, fourcc, varId);
  }

  // The flags and render state a property sets: true where it is taken.
  bool RenderStateProperty(GeneratorState& s, const EffectProperty& property,
                           uint32_t fourcc) const {
    if (s.inactivePrimitive && fourcc == F("SIZE")) {
      // Both sprite Fixup and transformed Fixup (0x28ec98) consume SIZE.
      // Only inactive primitives clear it; do not substitute SIZE=0 to hide draws.
      return true;
    }
    // DFOG 1 sets the description's no-fog bit (CreateGPSM 0x25a560, CreateSWSH 0x2dce88: desc+0x6b
    // bit 3), which the static render state carries as 0x8000 (ctor 0x28bfc4) and which keeps
    // RenderSpriteEntries/RenderSwooshEntries from injecting the fog block, whatever the blend.
    // Retail has no such property: the port-only PDFG.
    if ((s.part || s.swoosh) && fourcc == F("DFOG")) {
      uint32_t on = 0;
      if (!SmallInteger(property, on)) {
        s.result.dropped.push_back("DFOG: not a constant");
      } else {
        s.unfogged = on != 0;
      }
      return true;
    }
    // ZTES 0 (GPSM desc+0x1b8 bit 1, SWSH desc+0x22c bit 9) makes the static render state's depth
    // compare 1 instead of 4 (ctor 0x28c2fc/0x28c384); the GPU's table 0x1d0c3a8 maps 1 to ALWAYS
    // and 4 to LEQUAL, so the test passes everywhere (writes stay with ZBUF). Default on.
    // Retail has no such property: the port-only PZTS.
    if ((s.part || s.swoosh) && fourcc == F("ZTES")) {
      uint32_t on = 0;
      if (!SmallInteger(property, on)) {
        s.result.dropped.push_back("ZTES: not a constant");
      } else {
        s.noZTest = on == 0;
      }
      return true;
    }
    // WSTP (GetWaterSortMode, desc+0x69): ConvertWaterSortMode 0x256764 maps 1 to sort type 5
    // (always above the water plane) and 2 to type 1 (always below); anything else is 0, which
    // BinEntriesWithSortPlane 0x1f3140 resolves from the bounds. CScriptEffect's AddToRenderer
    // (0xd9bc88) takes the root generator's mode for the whole effect. Retail has no such
    // property: the port-only PWST.
    if (s.part && fourcc == F("WSTP")) {
      uint32_t mode = 0;
      if (!SmallInteger(property, mode)) {
        s.result.dropped.push_back("WSTP: not a constant");
      } else {
        s.waterSort = mode == 1 || mode == 2 ? mode : 0;
      }
      return true;
    }
    // IVSE (desc+0x2fa bit 2, default on): the initial velocity goes through
    // CCoords::TransformVelocity (CreateNewParticles 0x24a9fc). Retail always turns it by the
    // system orientation, so only IVSE 0 differs: the port-only PIVS.
    if (s.part && fourcc == F("IVSE")) {
      uint32_t on = 0;
      if (!SmallInteger(property, on)) {
        s.result.dropped.push_back("IVSE: not a constant");
      } else {
        s.noIvse = on == 0;
      }
      return true;
    }
    // ISVF and ITPE read the system's per-tick change of translation (EMRV, var 0x16: the delta
    // CreateNewParticles gets as its third argument). XFMD 0, 3 and 6 give it a constant zero, so
    // neither does anything there; 2 and 5 are PEMV's modes; the others are not decoded. ITPE is
    // on by default in Remastered (desc ctor 0x290c10): one that is absent is written after the
    // loop, flagged kSpreadDefault.
    if (s.part && (fourcc == F("ISVF") || fourcc == F("ITPE"))) {
      const std::string name = EffectFourCCString(property.fourcc);
      s.itpe = s.itpe || fourcc == F("ITPE");
      uint32_t on = 1;
      if (fourcc == F("ITPE") && !SmallInteger(property, on)) {
        s.result.dropped.push_back("ITPE: not a constant");
        return true;
      }
      if (fourcc == F("ITPE") && on == 0) {
        return true;  // retail never spreads
      }
      if (m_xfmd == 0 || m_xfmd == 3 || m_xfmd == 6) {
        return true;
      }
      if (m_xfmd != 2 && m_xfmd != 5) {
        s.result.dropped.push_back(name + " under XFMD " + std::to_string(m_xfmd));
        return true;
      }
      if (m_xfmd == 5 && m_pofs == 2) {
        s.result.dropped.push_back(name + " under XFMD 5 with a POFS that is not a constant");
        return true;
      }
      const uint32_t mode = m_xfmd == 5 ? (m_pofs == 1 ? 2 : 1) : 0;
      if (fourcc == F("ITPE")) {
        s.spreadMode = int(mode);
        return true;
      }
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (property.value.size() != 1 || !Element(property.value[0], Type::Real, inner, why)) {
        s.result.dropped.push_back("ISVF: " + why);
        return true;
      }
      AppendBE32(s.out, F("PISV"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, mode);
      s.out.insert(s.out.end(), inner.begin(), inner.end());
      return true;
    }
    // IEVM caps the length of the delta ISVF scales (CreateNewParticles 0x24a540; ITPE's spread
    // reads it unclamped). CreateGPSM 0x258438 keeps only a constant above zero: anything else is
    // inert. The port-only PIEV carries the float bits.
    if (s.part && fourcc == F("IEVM")) {
      uint32_t bits = 0;
      if (property.value.size() == 1 && ConstWord(property.value[0], bits) && FloatFromBits(bits) > 0.f) {
        AppendBE32(s.out, F("PIEV"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, bits);
      }
      return true;
    }
    // DBIS is the particle depth bias (desc+0xd4 of CreateGPSM, the SWSH one too; constant only),
    // a world-unit shift along the view depth that CParticleSceneNode::ConstructDepthBias 0x2b1004
    // turns into a projection m22 offset (CScopedGraphicsDepthBias::CalculateProjMatrixDepthBias
    // 0x22a604). Retail has no such property: the port-only PDBS.
    if ((s.part || s.swoosh) && fourcc == F("DBIS")) {
      uint32_t bits = 0;
      if (property.value.size() != 1 || !ConstWord(property.value[0], bits)) {
        s.result.dropped.push_back("DBIS: not a constant");
      } else if ((bits & 0x7fffffffu) != 0) {
        s.depthBias = bits;
      }
      return true;
    }
    // DBSO (CreateGPSM 0x258f18, GetBool) is desc+0x6b bit 2, on by default (CParticleDescription
    // ctor 0x288314): ConstructDepthBias scales the DBIS shift by the device depth range's span
    // when it is set, by 1 when it is clear. CreateSWSH does not read it, so a swoosh keeps the
    // default. Port-only PDBO, written only when DBSO turns it off.
    if (s.part && fourcc == F("DBSO")) {
      uint32_t value = 0;
      if (SmallInteger(property, value) && value <= 1u) {
        s.depthBiasUsesDepthRange = value != 0;
      } else {
        s.result.dropped.push_back("DBSO: not a constant");
      }
      return true;
    }
    // OVEC (CreateGPSM 0x25a1c4, var 0x32 at desc+0x388) is a sprite's vector orientation axis:
    // CParticleStaticRenderState 0x28c4f0 makes a sprite vector-oriented when it is authored, ORNT 0
    // too, and the instance writer 0x2b4948 puts its value where the velocity would go. The port-only
    // POVC, in the port's space. A planar square or model has no vector orientation: every other
    // desc+0x388 load (UpdateSpriteRenderState 0x247668, CopyPerSpriteDataToSpriteRenderData
    // 0x2478bc/0x248324, the instance writer 0x2b45b8/0x2b4948) is on the sprite path, and
    // CreateNewParticles 0x24aae0 only writes the normalized velocity into its slot (ORNT 1). The
    // shipped OVEC on a planar square or model is PLCD or a constant (no random draws), so it is
    // consumed with no effect.
    if (s.part && fourcc == F("OVEC")) {
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (s.node.primitiveType != 1) {
        // Nothing to write.
      } else if (property.value.size() != 1 || !Element(property.value[0], Type::Vector, inner, why)) {
        s.result.dropped.push_back("OVEC: " + why);
      } else {
        AppendBE32(s.out, F("POVC"));
        Basis(F("BSIS"), inner, s.out);
      }
      return true;
    }
    // SBIS is the particle sort bias (desc+0xd8 of CreateGPSM, constant only):
    // CParticleSceneNode::PreRenderForContextConcurrent 0x2b22b8 puts GetSortBias 0x288248 in the
    // translucent sort entry (+0x24), which SortFarToNear 0x1f3000 adds to the bounds' view depth.
    // Retail has no such property: the port-only PSBS.
    if (s.part && fourcc == F("SBIS")) {
      uint32_t bits = 0;
      if (property.value.size() != 1 || !ConstWord(property.value[0], bits)) {
        s.result.dropped.push_back("SBIS: not a constant");
      } else if ((bits & 0x7fffffffu) != 0) {
        s.sortBias = bits;
      }
      return true;
    }
    // SLDS ("SleepDistance", CreateGPSM 0x258a08) is evaluated only when it is a constant; any other
    // element stores 0. desc+0x520 holds its square, and the CElementGen ctor 0x244cfc turns sleep on
    // only when that square is positive (CElementGen::Update). Retail has no such property: the
    // port-only PSLD, the raw distance (any sign; the factory squares it).
    if (s.part && fourcc == F("SLDS")) {
      uint32_t bits = 0;
      if (property.value.size() == 1 && ConstWord(property.value[0], bits) && (bits & 0x7fffffffu) != 0) {
        s.sleepDistance = bits;
      }
      return true;
    }
    // PBDM is Remastered's blend mode, from the table CParticleStaticRenderState
    // reads: 0 alpha (src alpha, 1 - src alpha), 1 premultiplied (1, 1 - src
    // alpha), 2 additive (src alpha, 1) and 3 opaque (1, 0). Retail has only
    // alpha and AAPH (additive), so 2 is AAPH and 1 and 3 go to the port-only PBDM,
    // whose factors the generator's alpha blend takes (CElementGen::PortBlendSrc/Dst).
    if (s.part && fourcc == F("PBDM")) {
      const EffectValue* mode = property.value.size() == 1 ? &property.value[0] : nullptr;
      if (mode != nullptr && IsElement(*mode, F("CNST")) && mode->args.size() == 1) {
        mode = &mode->args[0];
      }
      if (mode == nullptr || (mode->kind != EffectValue::Kind::Byte && mode->kind != EffectValue::Kind::Word)) {
        s.result.dropped.push_back("PBDM: not a constant");
      } else if (mode->word == 2) {
        AppendBE32(s.out, F("AAPH"));
        AppendBE32(s.out, F("CNST"));
        s.out.push_back(1);
      } else if (mode->word == 1 || mode->word == 3) {
        AppendBE32(s.out, F("PBDM"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, mode->word);
      } else if (mode->word != 0) {
        m_approximated.push_back("PBDM " + std::to_string(mode->word) + " taken as alpha blending");
      }
      return true;
    }
    return false;
  }

  // Materials, spawns, transforms and shapes: true where the property is taken.
  bool StructureProperty(GeneratorState& s, const EffectProperty& property,
                         uint32_t fourcc) const {
    if (fourcc == F("MTIN") && (s.textured || s.electric)) {
      s.material = &property;
      return true;
    }
    // A generator's or swoosh's PVAR goes out as the port-only PVRT (NodeModes reads it). An
    // ELC2's has nothing to carry: no shipped ELC2 reads its own variables, nor does any child
    // of one (DPV/TPV), and Remastered's only bind of one (CPlasmaProjectileMP1::UpdateBeamState
    // 0x1020a38, the beam's expansion, guid 793f4892..) writes a variable nothing reads.
    if (fourcc == F("PVAR") && (((s.part || s.swoosh) && m_varsRead) || s.electric)) {
      return true;
    }
    // KPAL (SSystem flag 0x40, absent = false; CreateGPSM 0x25af00) picks IsSystemDeletable_MP1
    // 0x24cd68, else _Default 0x24cb58. Default's extra in-lifetime checks run only when the
    // system parms' bit 0 is clear (CElementGen ctor 0x244e14 -> gen+0x208 bit 2), and it is
    // set by every MP1 owner: CParticleSystemParms(ESpecialInit) 0x2af2e0 writes 7, children
    // copy their parent's (CParticleGen 0x27b088, CElementGen 0x24bc64), and no MP1 call site
    // clears it. With it set, the two predicates agree with retail's, so KPAL changes nothing.
    // PMPH (desc+0x434, a real; loader 0x258b6c) is a model particle's animated-UV phase:
    // UpdateModelRenderState 0x246afc evaluates it per particle into SModelParticleRenderData+0xc
    // (default 0, ctor 0x28aeac), WriteModelDataInstancingComponents 0x2c0148 sends it as a
    // CSetAnimUvPhaseCommand ('AUVP') and CModelAnimUvDataSourceInstance::SetAnimUvPhase 0x232a50
    // sets the phase of the model's ANUV program. A model without ANUV has no such instance, so
    // PMPH changes nothing for it; the end of the PART checks every PMDL model has none.
    if (s.part && fourcc == F("PMPH")) {
      s.pmph = true;
      return true;
    }
    if (s.part && fourcc == F("KPAL")) {
      return true;
    }
    // Retail reads KSSM, but Remastered's spawn table is laid out differently.
    if (s.part && fourcc == F("KSSM")) {
      std::vector<uint8_t> bytes;
      std::string why;
      // The hidden PEOD ints follow the system variables in PSYS, in table order
      // (CAddVarBuilder::AddDefinedVar 0x275188 appends as KSSM is read). [I] their draw
      // order against RAND-drawing system variables is not proven; no shipped effect has both.
      const size_t sysCount = s.sysItems.size();
      const uint32_t hiddenInt = m_sysNextHiddenInt;
      m_spawnSysItems = &s.sysItems;
      const bool exact = SpawnTables(property, s.node, bytes, why);
      m_spawnSysItems = nullptr;
      if (!exact) {
        s.sysItems.resize(sysCount);
        m_sysNextHiddenInt = hiddenInt;
      }
      if (exact) {
        if (!bytes.empty()) {
          AppendBE32(s.out, F("PKSM"));
          s.out.insert(s.out.end(), bytes.begin(), bytes.end());
        }
        return true;
      }
      m_approximated.push_back("KSSM: read as retail's table (" + why + ")");
      why.clear();
      if (!SpawnTable(property, bytes, s.started, why)) {
        s.result.dropped.push_back("KSSM: " + why);
        ++s.result.droppedRetail;
      } else if (!bytes.empty()) {
        AppendBE32(s.out, fourcc);
        s.out.insert(s.out.end(), bytes.begin(), bytes.end());
      }
      return true;
    }
    if (s.part && fourcc == F("PMRQ")) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (!ModelRotation(property.value, bytes, why)) {
        s.result.dropped.push_back("PMRQ: " + why);
        return true;
      }
      s.written.insert(F("PMRT"));
      AppendBE32(s.out, F("PMRT"));
      s.out.insert(s.out.end(), bytes.begin(), bytes.end());
      // PMRB and PMCF (flag bits 0 and 5, CreateGPSM 0x256b7c) make the PMRQ the particle's base
      // rotation (Fixup 0x28ec98, SolveParticleTransform 0x246f14): PMOP turns with it, and the
      // system orientation is left out, except that PMCF still applies the global frame, whose
      // rotation is the system's under XFMD 3 and 4 and identity under 2. Port-only PMRF: 1 =
      // the PMRQ alone, 2 = the orientation times the PMRQ, PMOP turned by both.
      uint32_t pmrb = 0, pmcf = 0, mode = 2;
      for (const EffectProperty& other : s.node.properties) {
        if (other.fourcc == F("PMRB") || other.fourcc == F("PMCF") || other.fourcc == F("XFMD")) {
          uint32_t word = 0;
          if (!SmallConst(other, word)) {
            s.result.dropped.push_back(EffectFourCCString(other.fourcc) + ": not a constant (PMRQ frame)");
            continue;
          }
          (other.fourcc == F("PMRB") ? pmrb : other.fourcc == F("PMCF") ? pmcf : mode) = word;
        }
      }
      if (pmrb != 0 || pmcf != 0) {
        if (mode != 2 && mode != 3 && mode != 4) {
          s.result.dropped.push_back("PMRQ: PMRB or PMCF under XFMD " + std::to_string(int32_t(mode)));
        } else {
          AppendBE32(s.out, F("PMRF"));
          AppendBE32(s.out, F("CNST"));
          AppendBE32(s.out, pmcf != 0 && mode != 2 ? 2u : 1u);
        }
      }
      return true;
    }
    // Read with the PMRQ above; without a PMRQ, Fixup clears both.
    if (s.part && (fourcc == F("PMRB") || fourcc == F("PMCF"))) {
      return true;
    }
    // Port-only: XFMD and its mover are written after the loop, and the
    // material data is written by Material() alongside the VMAT.
    if (s.part && fourcc == F("XFMD")) {
      s.xfmd = &property;
      return true;
    }
    // A generator's SYST, SYSR, SYSS, TEMT or TEMR forces transform mode 1 (PartSystemMode): SYST
    // as the port-only PSYT and TEMT as PTMT, in the port's space as PINT is; the zero REULs are
    // the port's defaults. XFMD 1 is written after the loop.
    if (s.part && (fourcc == F("SYST") || fourcc == F("SYSR") || fourcc == F("SYSS") || fourcc == F("TEMT") ||
                   fourcc == F("TEMR"))) {
      if (m_partSys == 0) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": " + m_partSysWhy);
      } else if (m_partSys == 1 && (fourcc == F("SYST") || fourcc == F("TEMT"))) {
        std::vector<uint8_t> inner;
        std::string why = "not one element";
        if (!Element(property.value[0], Type::Vector, inner, why)) {
          s.result.dropped.push_back(EffectFourCCString(fourcc) + ": " + why);
          return true;
        }
        AppendBE32(s.out, fourcc == F("SYST") ? F("PSYT") : F("PTMT"));
        Basis(F("BSIS"), inner, s.out);
      }
      return true;
    }
    // A swoosh's SYSR forces transform mode 1 (SwooshSystemMode).
    if (s.swoosh && fourcc == F("SYSR")) {
      std::string why;
      const int mode = SwooshSystemMode(s.node, why);
      if (mode == 1) {
        AppendBE32(s.out, F("XFMD"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      } else if (mode == 0) {
        s.result.dropped.push_back("SYSR: " + why);
      }
      return true;
    }
    // A swoosh's XFMD 3 draws its points in the emitter's frame (CParticleSwoosh xPortXfmdFollow);
    // 2 and 5 keep local and global apart as retail does (build/mpr/vfx/RESOLVED-xfmd.md). 4 with
    // an identity EMVR and no SMVR births points at the emitter's transform and draws them at
    // identity and unit scale (xPortXfmdMover); the EMVR is consumed by it.
    if (s.swoosh && fourcc == F("XFMD")) {
      uint32_t mode = 2;
      const EffectProperty* emvr = nullptr;
      bool smvr = false;
      for (const EffectProperty& other : s.node.properties) {
        emvr = other.fourcc == F("EMVR") ? &other : emvr;
        smvr = smvr || other.fourcc == F("SMVR");
      }
      if (!SmallConst(property, mode)) {
        s.result.dropped.push_back("XFMD: not a constant");
      } else if (mode == 4 && (smvr || emvr == nullptr || !IdentityMover(*emvr))) {
        s.result.dropped.push_back(smvr              ? "XFMD 4 with an SMVR"
                                 : emvr == nullptr ? "XFMD 4 with no EMVR"
                                                   : "XFMD 4: EMVR not SMOV(EXTT, 0, EXTR)");
      } else if (mode == 3 || mode == 4) {
        AppendBE32(s.out, F("XFMD"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, mode);
      } else if (mode != 2 && mode != 5) {
        s.result.dropped.push_back("XFMD " + std::to_string(mode));
      }
      return true;
    }
    if (s.swoosh && fourcc == F("EMVR") && IdentityMover(property)) {
      uint32_t mode = 2;
      bool smvr = false;
      for (const EffectProperty& other : s.node.properties) {
        if (other.fourcc == F("XFMD") && !SmallConst(other, mode)) {
          mode = UINT32_MAX;
        }
        smvr = smvr || other.fourcc == F("SMVR");
      }
      if (mode == 4 && !smvr) {
        return true;
      }
    }
    if (s.part && fourcc == F("SMVR")) {
      s.mover = &property;
      return true;
    }
    if (s.part && fourcc == F("EMVR")) {
      s.emitterMover = &property;
      return true;
    }
    // A thresholding map only a header material draws (DrawWrites reports it otherwise).
    if (s.part && fourcc == F("ANTH")) {
      s.anth = true;
      return true;
    }
    // SSZE is a per-particle value whatever draws the generator: sprite Fixup keeps it, transformed
    // Fixup (0x28ec98) moves it as it does SIZE, and only an inactive primitive's Clear (0x28e2d8)
    // deletes it. PSSZ reads it (some model generators keep a random angle there for their PMRQ);
    // only a sprite's VMAT draws it as the quad's length. Port-only SSZE (xPortSsze).
    if (s.part && fourcc == F("SSZE")) {
      if (s.inactivePrimitive) {
        return true;
      }
      std::vector<uint8_t> bytes;
      std::string why;
      if (property.value.size() != 1 || !Element(property.value[0], Type::Real, bytes, why)) {
        s.result.dropped.push_back("SSZE: " + (why.empty() ? std::string("not one element") : why));
        return true;
      }
      AppendBE32(s.out, fourcc);
      s.out.insert(s.out.end(), bytes.begin(), bytes.end());
      // A model particle never draws it (SolveParticleTransform 0x246f14 reads the position alone).
      s.drawsSsze = s.node.primitiveType != 3;
      return true;
    }
    if (s.part && (fourcc == F("TMTR") || fourcc == F("PMTR") || fourcc == F("SMTR") ||
                 fourcc == F("ITEN") || fourcc == F("SCTR") || fourcc == F("SFTD") || fourcc == F("AERO"))) {
      s.portOnly.push_back(fourcc);
      return true;
    }
    if ((s.swoosh || s.electric) &&
        (fourcc == F("TMTR") || fourcc == F("PMTR") || fourcc == F("SMTR") || fourcc == F("ITEN") ||
         fourcc == F("SFTD"))) {
      s.portOnly.push_back(fourcc);
      return true;
    }
    // Port-only: an ELC2's XFMD 3 has its bolts follow the emitter (CElectricDescription
    // xPortXfmdFollow; InitBoltNodePosition 0xb76e18 leaves them local and UpdateRenderState
    // 0xb760e0 copies the live transform every frame). Other modes keep the baked bolts.
    if (s.electric && fourcc == F("XFMD")) {
      uint32_t mode = 2;
      if (!SmallConst(property, mode)) {
        s.result.dropped.push_back("XFMD: not a constant");
      } else if (mode == 3) {
        AppendBE32(s.out, F("XFMD"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, mode);
      } else if (mode != 2) {
        s.result.dropped.push_back("XFMD " + std::to_string(mode));
      }
      return true;
    }
    // ELC2 render-state fields (CreateELC2 0xb70734). EBDM is read by the VMAT writer, the only
    // way an ELC2 draws. DDLN is read into a stack byte and discarded. TEXW (desc+0x265) and ZBUF
    // (desc+0x266) are GetBools whose ctor default is 0 (0xb71d3c); InitStaticRenderStateImpl
    // 0xb72618 sets static-state bits 0x1080 and 0x40 for them. Bit 0x1000 has no reader (the
    // state word's consumers test bits 6, 7, 9 and 14-18 only, and desc+0x265 is read nowhere
    // else), so TEXW is consumed whatever its value. ZBUF 1 (depth write in BindGraphicsState
    // 0x2b10e4) is not ported, so only 0 converts.
    if (s.electric && (fourcc == F("EBDM") || fourcc == F("DDLN") || fourcc == F("TEXW"))) {
      return true;
    }
    if (s.electric && fourcc == F("ZBUF")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": not a constant 0 or 1");
      } else if (value != 0) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + " 1");
      }
      return true;
    }
    // Port-only ELC2 fields, written as the retail encoding under their own tags.
    if (s.electric && Elc2Properties().count(fourcc) != 0) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (Property(fourcc, Elc2Properties().at(fourcc), property.value, bytes, why)) {
        AppendBE32(s.out, fourcc);
        s.out.insert(s.out.end(), bytes.begin(), bytes.end());
      } else {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": " + why);
      }
      return true;
    }
    // Port-only PSRO: a swoosh's POFS of ROTV(v, EXTR) is v turned by the swoosh's orientation.
    if (s.swoosh && fourcc == F("POFS") && s.retail.count(fourcc) != 0 && property.value.size() == 1 &&
        IsElement(property.value[0], F("ROTV")) && property.value[0].args.size() == 2 &&
        IsElement(property.value[0].args[1], F("EXTR"))) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (Property(fourcc, s.retail.at(fourcc), {property.value[0].args[0]}, bytes, why)) {
        s.written.insert(fourcc);
        AppendBE32(s.out, fourcc);
        s.out.insert(s.out.end(), bytes.begin(), bytes.end());
        AppendBE32(s.out, F("PSRO"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
        return true;
      }
      s.result.dropped.push_back("POFS: " + why);
      return true;
    }
    if (s.part && fourcc == F("SSHP") && property.value.size() == 1 && property.value[0].kind == EffectValue::Kind::Byte) {
      // Remastered's sprite mesh slot (desc+0x394 in RenderSpriteEntries): 0-2 are quad grids, 3-6 polygons.
      // Port-only PSHP carries the polygon shapes (kb format/SSHP.md).
      const uint32_t shape = property.value[0].word;
      if (shape >= 3 && shape <= 6) {
        s.spriteShape = shape;
      } else if (shape > 6) {
        s.result.dropped.push_back("SSHP: shape " + std::to_string(shape) + " is past Remastered's 7 meshes");
      }
      return true;
    }
    // INTR, the system rotation var 0xb in place of the external one (CParticleDescription::Compile
    // 0x289af0, which CSwooshDescription::Compile 0x29b1a0 runs): under XFMD 2 the points' local
    // orientation (var bc: CreateNewParticle 0x27cdc8's birth orientation and IVEL's turn, RLCL),
    // under 3 the global one (SetupTransformMode 0x2887d8); 5 deletes it and 4's mover overrides
    // it. A constant REUL r, or RADD(EXTR, r) (the external orientation times r, RADD = a * b), is
    // the port-only PSIR: flags (bit 0 times the external orientation), then r as PINR's Euler
    // vector in the port's space (ModelRotation, B r B). EXTR, or RADD(EXTR, REUL(0, 0, 0)), is the
    // default and writes nothing; so is an ELC2's EXTR.
    if ((s.swoosh || s.electric) && fourcc == F("INTR")) {
      const auto isExtr = [](const EffectValue& value) { return IsElement(value, F("EXTR")) && value.args.empty(); };
      const auto zeroReal = [](const EffectValue& value) {
        uint32_t word = 0;
        return ConstWord(value, word) && (word & 0x7fffffffu) == 0;
      };
      const EffectValue* rot = property.value.size() == 1 ? &property.value[0] : nullptr;
      if (rot != nullptr && isExtr(*rot)) {
        return true;
      }
      if (s.electric) {
        s.result.dropped.push_back("INTR: an ELC2's other than EXTR");
        return true;
      }
      uint32_t mode = 2;
      for (const EffectProperty& other : s.node.properties) {
        if (other.fourcc == F("XFMD") && !SmallConst(other, mode)) {
          mode = UINT32_MAX;
        }
      }
      if (mode == 4 || mode == 5) {
        return true;
      }
      uint32_t flags = 0;
      if (rot != nullptr && IsElement(*rot, F("RADD")) && rot->args.size() == 2 && isExtr(rot->args[0])) {
        flags = 1;
        rot = &rot->args[1];
      }
      const bool reul = rot != nullptr && IsElement(*rot, F("REUL")) && rot->args.size() == 4;
      bool constant = reul;
      bool zero = reul;
      for (size_t i = 0; reul && i < 3; ++i) {
        constant = constant && IsElement(rot->args[i], F("CNST"));
        zero = zero && zeroReal(rot->args[i]);
      }
      std::vector<uint8_t> bytes;
      std::string why;
      if (mode != 2 && mode != 3) {
        why = "under XFMD " + std::to_string(int32_t(mode));
      } else if (!constant) {
        why = "not a constant REUL or RADD(EXTR, constant REUL)";
      } else if (flags == 1 && zero) {
        return true;
      } else if (ModelRotation({*rot}, bytes, why)) {
        AppendBE32(s.out, F("PSIR"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, flags);
        s.out.insert(s.out.end(), bytes.begin(), bytes.end());
        return true;
      }
      s.result.dropped.push_back("INTR: " + why);
      return true;
    }
    // A swoosh's INTT, the system translation var in place of the external one (Compile 0x289af0).
    // EXTT, or EXTT plus a constant zero vector (ROTV of one, the shipped ADD_(EXTT, ROTV(0, EXTR))),
    // is the external translation itself and writes nothing; anything else is not ported.
    if (s.swoosh && fourcc == F("INTT")) {
      const auto isExtt = [](const EffectValue& value) { return IsElement(value, F("EXTT")) && value.args.empty(); };
      const auto zeroVector = [](const EffectValue& value) {
        const EffectValue* v = &value;
        if (IsElement(*v, F("ROTV")) && v->args.size() == 2) {
          v = &v->args[0];
        }
        if (!IsElement(*v, F("CNST")) || v->args.size() != 3) {
          return false;
        }
        for (const EffectValue& c : v->args) {
          uint32_t word = 0;
          if (!ConstWord(c, word) || (word & 0x7fffffffu) != 0) {
            return false;
          }
        }
        return true;
      };
      const EffectValue* t = property.value.size() == 1 ? &property.value[0] : nullptr;
      if (t != nullptr && (isExtt(*t) || (IsElement(*t, F("ADD_")) && t->args.size() == 2 &&
                                          isExtt(t->args[0]) && zeroVector(t->args[1])))) {
        return true;
      }
      s.result.dropped.push_back("INTT: a swoosh's other than EXTT");
      return true;
    }
    // Port-only PRAZ/PRAF: a swoosh's PROT of KPIN(RAZY(EMRV, ROTV(v, RLCL), fallback)), the ring
    // frozen at each point's birth facing the emitter's motion (CERotationFromAxis 0x29d298). Only
    // under XFMD 2, where EMRV is the system-space translation delta; ORNT with SIDE 2 replaces
    // PROT (SetupOrientToView 0x29bdbc), so it is left out there. A PROT turning about Z only
    // becomes port-only PROZ (SwooshZRotation).
    if (s.swoosh && fourcc == F("PROT")) {
      auto nodeWord = [&s](uint32_t tag, uint32_t fallback) {
        uint32_t word = fallback;
        for (const EffectProperty& other : s.node.properties) {
          if (other.fourcc == tag && !SmallConst(other, word)) {
            word = UINT32_MAX;
          }
        }
        return word;
      };
      const uint32_t xfmdMode = nodeWord(F("XFMD"), 2);
      const bool orntView = nodeWord(F("ORNT"), 0) == 1 && nodeWord(F("SIDE"), 0) == 2;
      if (orntView) {
        return true;
      }
      auto zeroReal = [](const EffectValue& value) {
        uint32_t word = 0;
        return ConstWord(value, word) && (word & 0x7fffffffu) == 0;
      };
      const EffectValue* razy = property.value.size() == 1 && IsElement(property.value[0], F("KPIN")) &&
                                        property.value[0].args.size() == 1 &&
                                        IsElement(property.value[0].args[0], F("RAZY")) &&
                                        property.value[0].args[0].args.size() == 3
                                    ? &property.value[0].args[0]
                                    : nullptr;
      const EffectValue* rotv = razy != nullptr && IsElement(razy->args[0], F("EMRV")) &&
                                        IsElement(razy->args[1], F("ROTV")) && razy->args[1].args.size() == 2 &&
                                        IsElement(razy->args[1].args[1], F("RLCL"))
                                    ? &razy->args[1]
                                    : nullptr;
      uint32_t fallback = UINT32_MAX;
      if (rotv != nullptr) {
        const EffectValue& fb = razy->args[2];
        if (IsElement(fb, F("RLCL"))) {
          fallback = 1;
        } else if (IsElement(fb, F("REUL")) && fb.args.size() == 4 && zeroReal(fb.args[0]) && zeroReal(fb.args[1]) &&
                   zeroReal(fb.args[2])) {
          fallback = 0;
        }
      }
      std::vector<uint8_t> bytes;
      std::string why;
      // Under XFMD 3 EMRV is a zero constant (b8 = 0), so RAZY always takes its fallback: a zero
      // REUL there is written as that REUL (PROZ, which also stands in for retail's IROT/ROTM).
      uint32_t flags = 0;
      if (fallback == 0 && xfmdMode == 3) {
        if (!SwooshZRotation(razy->args[2], flags, bytes, why)) {
          s.result.dropped.push_back("PROT: " + why);
        } else {
          AppendBE32(s.out, F("PROZ"));
          AppendBE32(s.out, F("CNST"));
          AppendBE32(s.out, flags);
          s.out.insert(s.out.end(), bytes.begin(), bytes.end());
        }
      } else if (fallback == UINT32_MAX && property.value.size() == 1 && !IsElement(property.value[0], F("KPIN")) &&
          !IsElement(property.value[0], F("RADD")) && !IsElement(property.value[0], F("REUL"))) {
        s.result.dropped.push_back("PROT: not KPIN(RAZY(EMRV, ROTV(v, RLCL), RLCL or zero REUL))");
      } else if (fallback == UINT32_MAX) {
        // CreateNewParticle 0x27cdc8 sets the point's orientation from var bc and then PROT's: a
        // plain REUL is the same under XFMD 3 (bc identity, the emitter's turn drawn globally) and 5
        // (bc set by SetOrientation, as retail's), and 4 (bc the EMVR's EXTR, set by both orientation
        // setters). EXTR/INTR there are not the orientation a point starts from (XFMD 3 draws it
        // again globally), so those stay dropped.
        if (xfmdMode != 2 && xfmdMode != 3 && xfmdMode != 4 && xfmdMode != 5) {
          s.result.dropped.push_back("PROT: turned about Z under XFMD " + std::to_string(int32_t(xfmdMode)));
        } else if (!SwooshZRotation(property.value[0], flags, bytes, why)) {
          s.result.dropped.push_back("PROT: " + why);
        } else if ((flags & 2) != 0 && xfmdMode != 2) {
          s.result.dropped.push_back("PROT: EXTR or INTR under XFMD " + std::to_string(int32_t(xfmdMode)));
        } else {
          AppendBE32(s.out, F("PROZ"));
          AppendBE32(s.out, F("CNST"));
          AppendBE32(s.out, flags);
          s.out.insert(s.out.end(), bytes.begin(), bytes.end());
        }
      } else if (xfmdMode != 2) {
        s.result.dropped.push_back("PROT: RAZY under XFMD " + std::to_string(int32_t(xfmdMode)));
      } else if (!PortVector(rotv->args[0], bytes, why)) {
        s.result.dropped.push_back("PROT: " + why);
      } else {
        AppendBE32(s.out, F("PRAZ"));
        s.out.insert(s.out.end(), bytes.begin(), bytes.end());
        AppendBE32(s.out, F("PRAF"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, fallback);
      }
      return true;
    }
    if ((s.part || s.swoosh) && SystemVarWrite(property.fourcc).first != ~0u) {
      return true;  // written as PSYS
    }
    return false;
  }

  // An ADV9, IAVn, VAVn or CAVn, converted for the port-only PPVR.
  void ParticleVariable(GeneratorState& s, const EffectProperty& property, int varId) const {
    PartVar var{uint32_t(varId)};
    std::string why;
    m_varReads = 0;
    if (property.value.size() != 1) {
      why = "not one element";
    } else if (PartVarType(varId) == Type::Int && IsElement(property.value[0], F("IRND")) && property.value[0].args.size() == 2 &&
               ConstantFlags(property.value[0].args[0], Type::Int) == 1 &&
               ConstantFlags(property.value[0].args[1], Type::Int) == 1) {
      // IRND(CNST, CNST) is CIEInitialRandomConstant (0x2a8c40): one draw of the one RNG at
      // spawn, never again (PPVR runs it at spawn only): min + Next() % (max - min + 1).
      AppendBE32(var.element, F("PRAN"));
      if (Element(property.value[0].args[0], Type::Int, var.element, why) &&
          Element(property.value[0].args[1], Type::Int, var.element, why)) {
        var.converted = true;
      }
    } else if ((PartVarType(varId) == Type::Vector || PartVarType(varId) == Type::Color) &&
               IsElement(property.value[0], F("KPIN")) && property.value[0].args.size() == 1) {
      // CVEKeepInitial/CCEKeepInitial (0x29e878, 0x2c7d58) run their child at age 0 only, and
      // the variable is initial-only (PPVR runs it at spawn only): the child alone.
      var.converted = Element(property.value[0].args[0], PartVarType(varId), var.element, why);
    } else {
      var.converted = Element(property.value[0], PartVarType(varId), var.element, why);
    }
    var.reads = m_varReads;
    var.flags = property.value.size() == 1 ? ConstantFlags(property.value[0], PartVarType(varId)) : 0;
    if (!var.converted) {
      s.result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " + why);
    }
    s.partVars.push_back(std::move(var));
  }

  // The visor data and the port-only system and shutdown properties: true where the property is
  // taken.
  bool PortOnlyProperty(GeneratorState& s, const EffectProperty& property,
                        uint32_t fourcc) const {
    // An ELC2 holds the same SVisorEffectDataDescription (CreateELC2 0xb70734, desc +0x218),
    // read the same way for a billboard root (CParticleElectric2::MPT_GetVisorEffectData 0xb78d5c).
    if ((s.part || s.electric) &&
        (fourcc == F("VESC") || fourcc == F("VEMC") || fourcc == F("VETC") || fourcc == F("VEBS") ||
         fourcc == F("VESN") || fourcc == F("VESM") || fourcc == F("VEDT"))) {
      s.visorData = true;
      uint32_t bits = 0;
      if ((fourcc == F("VESN") || fourcc == F("VESM")) && property.value.size() == 1 &&
          ConstWord(property.value[0], bits)) {
        (fourcc == F("VESN") ? s.visorMinScale : s.visorMaxScale) = bits;
      } else if (fourcc == F("VEDT") && property.value.size() == 1 && ConstWord(property.value[0], bits)) {
        s.visorTicks = bits;
      } else if (fourcc == F("VESC") || fourcc == F("VEMC") || fourcc == F("VETC") || fourcc == F("VEBS")) {
        std::vector<uint8_t>& bytes = fourcc == F("VESC")   ? s.visorStartColor
                                      : fourcc == F("VEMC") ? s.visorMidColor
                                      : fourcc == F("VETC") ? s.visorEndColor
                                                            : s.visorBlend;
        std::string why;
        bytes.clear();
        if (property.value.size() != 1 ||
            !Element(property.value[0], fourcc == F("VEBS") ? Type::Real : Type::Color, bytes, why)) {
          bytes.clear();
          s.result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " +
                                   (why.empty() ? "visor effect element the port cannot convert" : why));
        }
      } else {
        s.result.dropped.push_back(EffectFourCCString(property.fourcc) + ": visor effect data the port does not read");
      }
      return true;
    }
    // INTS, the system scale in place of the external one (Compile 0x289af0), as the port-only
    // PINS: Remastered's scale numbers, so BSSC turns them into the port's. XFMD 5 frees it
    // unread (SetupTransformMode 0x289338).
    if (s.part && fourcc == F("INTS")) {
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (m_xfmd == 5) {
        return true;
      }
      if (property.value.size() != 1 || !Element(property.value[0], Type::Vector, inner, why)) {
        s.result.dropped.push_back("INTS: " + why);
        return true;
      }
      AppendBE32(s.out, F("PINS"));
      Basis(F("BSSC"), inner, s.out);
      return true;
    }
    // INTR, the system orientation in place of the external one (Compile 0x289af0), as the
    // port-only PINR: an Euler vector in the port's space, as PMRQ's is (ModelRotation); EXTR
    // inside it reads the external orientation. XFMD 2 bakes it at spawn (Q_l), 3 makes it the
    // live global frame (SetupTransformMode 0x2887d8); 4's mover overrides it and 5 frees it.
    // The system var is evaluated once at creation (IRND, KPIN: init-only) or every tick (CMRT):
    // an IRND or KPIN inside it becomes a hidden init-only PSYR slot (m_sysItems), one draw per
    // system as in Remastered.
    if (s.part && fourcc == F("INTR")) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (m_xfmd == 4 || m_xfmd == 5) {
        return true;
      }
      if (m_xfmd != 2 && m_xfmd != 3) {
        why = "under XFMD " + std::to_string(m_xfmd);
      } else {
        std::vector<SystemItem> items;
        const uint32_t hidden = m_sysNextHidden;
        m_sysItems = &items;
        const bool ok = ModelRotation(property.value, bytes, why);
        m_sysItems = nullptr;
        if (ok) {
          AppendBE32(s.out, F("PINR"));
          s.out.insert(s.out.end(), bytes.begin(), bytes.end());
          for (SystemItem& sys : items) {
            s.sysItems.push_back(std::move(sys));
          }
          return true;
        }
        m_sysNextHidden = hidden;
      }
      s.result.dropped.push_back("INTR: " + why);
      return true;
    }
    // INTT, the system translation in place of the external one (Compile 0x289af0). Under XFMD 2
    // and 3 one reading EXTT is the port-only PINT, the system's world position. One reading no
    // EXTT is Retro's carry-over of the retail POFS (s-intrt-20261020 REPORT §7: 6 of 7 paired
    // cases equal it), written as POFS. XFMD 4's mover overrides it. XFMD 5 adds it to each
    // particle's spawn offset (vec 0x12, SetupTransformMode 0x289338): retail's POFS, added to a
    // POFS.
    if (s.part && fourcc == F("INTT")) {
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (m_xfmd == 4) {
        return true;
      }
      if (m_xfmd != 2 && m_xfmd != 3 && m_xfmd != 5) {
        s.result.dropped.push_back("INTT: under XFMD " + std::to_string(m_xfmd));
        return true;
      }
      const bool asPofs = IntIsPofs(property);
      const EffectProperty* pofs = nullptr;
      for (const EffectProperty& other : s.node.properties) {
        if (asPofs && other.fourcc == F("POFS")) {
          pofs = &other;
        }
      }
      if (pofs != nullptr && pofs->value.size() == 1) {
        inner.clear();
        AppendBE32(inner, F("ADD_"));
        if (!Element(pofs->value[0], Type::Vector, inner, why)) {
          s.result.dropped.push_back("POFS: " + why);
          inner.clear();
        }
      } else if (pofs != nullptr) {
        s.result.dropped.push_back("POFS: not one element");
      }
      if (property.value.size() != 1 || !Element(property.value[0], Type::Vector, inner, why)) {
        s.result.dropped.push_back("INTT: " + why);
        return true;
      }
      if (asPofs) {
        s.written.insert(F("POFS"));
      }
      AppendBE32(s.out, asPofs ? F("POFS") : F("PINT"));
      Basis(F("BSIS"), inner, s.out);
      return true;
    }
    // SHTM (CreateGPSM 0x256b7c, CreateSWSH 0x2db8a4, CreateELSM/CreateELC2, desc+0xcc): what a shutdown
    // does (CParticleGen::DoShutdownBehavior 0x27b88c). 0 stops emission, 1 also destroys the live
    // particles: the port-only PSHM, for a PART and a swoosh (CParticleSwoosh::DestroyParticles
    // 0x27f744 empties the ring). An electric's DestroyParticles (0x286554, 0xb78e8c) is a no-op,
    // so its 0 and 1 both just stop emitting, which is what the port's ShutdownEffect does.
    if ((s.part || s.swoosh || s.type == F("ELSC")) && fourcc == F("SHTM")) {
      uint32_t mode = 0;
      if (!SmallInteger(property, mode) || mode > 1) {
        s.result.dropped.push_back(mode > 1 ? "SHTM: mode " + std::to_string(mode)
                                          : std::string("SHTM: not a constant"));
      } else if (mode == 1 && (s.part || s.swoosh)) {
        AppendBE32(s.out, F("PSHM"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      }
      return true;
    }
    // ISDT (CreateGPSM, desc+0x6c bit 1, on by default from the CParticleDescription ctor 0x288314):
    // a shutdown reaches the children as DoShutdownBehavior with the parent's mode (0x27b88c,
    // 0x2499b8), so each acts at once on its own SHTM and its SHTT is never armed. 0 calls each
    // child's ShutdownEffect instead: the port-only PISD 0. 1 is the port's default.
    if (s.part && fourcc == F("ISDT")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("ISDT: not a constant 0 or 1");
      } else if (value == 0) {
        AppendBE32(s.out, F("PISD"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 0);
      }
      return true;
    }
    // OSDM (desc+0x6c bit 0, on by default from the CParticleDescription ctor 0x288314): a child
    // the system constructs gets the system's live shutdown mode (gen+0x98) in place of its own
    // SHTM (ConstructChildParticleSystem 0x27b1c0, the CParticleGen ctor 0x279784). 0 leaves each
    // child its own: the port-only POSD 0. 1 is the port's default.
    if (s.part && fourcc == F("OSDM")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("OSDM: not a constant 0 or 1");
      } else if (value == 0) {
        AppendBE32(s.out, F("POSD"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 0);
      }
      return true;
    }
    // An electric's own OSDM is never read: only CElementGen calls ConstructChildParticleSystem
    // (vtable +0x1c0); CParticleElectric and CParticleElectric2 have no call site.
    if (s.type == F("ELSC") && fourcc == F("OSDM")) {
      return true;
    }
    // ORTC (CreateGPSM 0x2595dc, desc+0x3f0 bit 4): CreateNewParticles 0x24a8d8 keeps the system
    // orientation (var +0xbc) at each particle's birth (gen+0x190), and SolveParticleTransform
    // 0x247150 uses it as the model's base rotation in place of the current one when there is no
    // PMOV and no PMRB/PMCF. Retail has no such property: the port-only PBOR 1.
    if (s.part && fourcc == F("ORTC")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("ORTC: not a constant 0 or 1");
      } else if (value == 1) {
        AppendBE32(s.out, F("PBOR"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      }
      return true;
    }
    // VMPC is evaluated after the initial particle values, not an alias for a VEL source.
    // CreateNewParticles 0x24abd0 calls ApplyVelocity (including its integration op).
    if (s.part && fourcc == F("VMPC")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("VMPC: not a constant 0 or 1");
      } else if (value == 1) {
        AppendBE32(s.out, F("VMPC"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      }
      return true;
    }
    // BLIT (static-state word0 bit 14, 0x4000): CParticleSceneNode::AppendBakedLightingIfRequested
    // 0x2b12a8 appends the room's baked probe texture (BPTX) and BLCM to a lit sprite's draw only with
    // it set. 1 is the port-only PBLT 1; 0 (the default) writes nothing.
    if (s.part && fourcc == F("BLIT")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("BLIT: not a constant 0 or 1");
      } else if (value == 1) {
        AppendBE32(s.out, F("PBLT"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      }
      return true;
    }
    // SHTT, the delayed shutdown (ShutdownEffect 0x27a66c, UpdateCountdownTimerForShutdown
    // 0x27b864): an int number of ticks, counted down once per tick after the owner's shutdown.
    // CreateSWSH reads it for a swoosh too (desc+0xd0; CParticleSwoosh::InternalUpdate 0x27d29c
    // counts it down).
    if ((s.part || s.swoosh) && fourcc == F("SHTT")) {
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (property.value.size() != 1 || !Element(property.value[0], Type::Int, inner, why)) {
        s.result.dropped.push_back("SHTT: " + why);
        return true;
      }
      AppendBE32(s.out, F("PSHT"));
      s.out.insert(s.out.end(), inner.begin(), inner.end());
      return true;
    }
    // PMOV, the per-particle model direction (SolveParticleTransform 0x246f14), and PMPV, the
    // pivot offset in the model's final frame: vectors in Remastered's numbers, as the port-only
    // properties of the same names, in the port's space (BSIS, as PMOP's). Fixup 0x28ec98 deletes
    // PMOV under a PMRQ base rotation (PMRB or PMCF); EMTU makes the model's up the system's.
    if (s.part && (fourcc == F("PMOV") || fourcc == F("PMPV"))) {
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (fourcc == F("PMOV")) {
        bool pmrqBase = false;
        bool emtu = false;
        bool emtuUnknown = false;
        for (const EffectProperty& other : s.node.properties) {
          uint32_t word = 0;
          if (other.fourcc == F("PMRB") || other.fourcc == F("PMCF")) {
            pmrqBase = pmrqBase || (SmallConst(other, word) && word != 0);
          } else if (other.fourcc == F("EMTU")) {
            emtuUnknown = !SmallConst(other, word);
            emtu = word != 0;
          }
        }
        if (pmrqBase) {
          return true;
        }
        if (emtu || emtuUnknown) {
          s.result.dropped.push_back("PMOV: EMTU makes its up the system's");
          return true;
        }
      }
      if (property.value.size() != 1 || !Element(property.value[0], Type::Vector, inner, why)) {
        s.result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " + why);
        return true;
      }
      AppendBE32(s.out, fourcc);
      Basis(F("BSIS"), inner, s.out);
      return true;
    }
    // EMTU (desc+0x3f0 bit 3, CreateGPSM 0x2590a8) has one reader, SolveParticleTransform 0x247004
    // in the PMOV branch (the up vector becomes the system's). PMOV above is dropped while EMTU is
    // set, so EMTU itself changes nothing the port draws.
    if (s.part && fourcc == F("EMTU")) {
      return true;
    }
    // PVRS (GPSM +0x534 / SWSH +0x201) only picks the shading mode CParticleSceneNode::Render
    // 0x2b247c passes to NVariableRateShading::CScopedShadingMode 0x19a35c, which does nothing
    // while sEnabled is clear. Only init_for_frame 0x19a7f0 sets it, called from
    // init_frame_for_content_option 0xf9bcb0 when NRenderDebugDefaults::skVariableShadingRateContent
    // is 1 or 2; that is bss nothing writes (its one GOT slot 0x2126610 has one reader, 0xf9ceb0).
    // STRY (0x6b bit 6) has no reader. Neither changes a pixel.
    if ((s.part || s.swoosh) && fourcc == F("PVRS")) {
      return true;
    }
    if (s.part && fourcc == F("STRY")) {
      return true;
    }
    // ISTR (desc+0x528, CreateGPSM 0x259c3c) is evaluated by the three render-state updates
    // (0x246eb4, 0x2475b8, 0x247780) only while desc+0x530 bit 4 is set. SRender::Fixup 0x28fc44
    // is the only setter, and it returns at once unless bit 3 is set, which nothing sets (SRender
    // ctor 0x28f4cc starts the flags at 0x3e0). Even when evaluated, its one sink,
    // CParticleDynamicRenderState+0xd8, has no reader.
    if (s.part && fourcc == F("ISTR")) {
      return true;
    }
    // KILL (desc+0x160, a bool element) is evaluated at the current frame after every
    // CElementGen::Update 0x2460e0 tick. True sets +0xa0 bit 0, which makes every IsSystemDeletable
    // (0x24cb6c, 0x24cd78) return true at once, and clears +0x208 bit 3 so the same update also sets
    // +0xa0 bit 1, after which Update returns at its first instruction. Only a constant is carried:
    // false never fires, true is PKIL.
    if (s.part && fourcc == F("KILL") && property.value.size() == 1 && IsElement(property.value[0], F("CNST")) &&
        property.value[0].args.size() == 1 &&
        (property.value[0].args[0].kind == EffectValue::Kind::Byte || property.value[0].args[0].kind == EffectValue::Kind::Word)) {
      s.kill = property.value[0].args[0].word != 0;
      return true;
    }
    // DPRE (0x6b bit 11, GetRenderInDepthPass 0x288250) is read once, by the proxy ctor 0xfbf3b0
    // (+0xac). QueueRenderDefault_Unsorted 0xfbf410 then also queues the particle as a depth
    // pre-pass command 0xfbd694, besides the unsorted command it always queues; the sorted path
    // ignores it. That depth only feeds early-Z and the hardware occlusion queries
    // (InitDepthBuffer 0xb328d4, RenderOcclusionQueries); the cluster light mask is built from
    // light volumes, not depth. The unsorted draw writes the same depth, so no pixel changes.
    if (s.part && fourcc == F("DPRE")) {
      return true;
    }
    // DEFX and UGFP are GPSM bools in 0x6b bits 0 and 1 (CreateGPSM 0x25a29c, 0x258b0c). The
    // CParticleDescription ctor 0x2883d4 starts the halfword at 0x304 and no other property sets
    // either bit, so 0 is the default; SRender::Fixup 0x28fcb0 may set UGFP after the parse,
    // whatever the file says. Only 0 converts (to nothing).
    if (s.part && (fourcc == F("DEFX") || fourcc == F("UGFP"))) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": not a constant 0 or 1");
      } else if (value != 0) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + " 1");
      }
      return true;
    }
    // RSV0..9 (CreateGPSM 0x257074) is a rotation system variable: GetRotationElement, then
    // CAddVarBuilder AddVar(ctx 0, rotation, n). Only a rotation PSAn reads it back
    // (GetRotationElement 0x25d7ac), and evaluating a rotation has no side effect, so with no PSAn
    // anywhere in the system it is never observed and converts to nothing.
    if (s.part && (fourcc & 0xffffff00u) == (F("RSV0") & 0xffffff00u) && (fourcc & 0xffu) >= '0' && (fourcc & 0xffu) <= '9') {
      const uint32_t read = F("PSA0") + (fourcc & 0xffu) - '0';
      for (const EffectProperty& other : s.node.properties) {
        for (const EffectValue& value : other.value) {
          if (ContainsElement(value, read)) {
            s.result.dropped.push_back(EffectFourCCString(fourcc) + ": read by " + EffectFourCCString(read));
            return true;
          }
        }
      }
      return true;
    }
    // FRMD (desc+0x170, CreateGPSM): the face-reflection mode 1..3. An explosion of a generator
    // with it set lights Samus's visor reflection (CVisorEffectManagerMP1::AddExplosionEffect
    // 0xd10144) in place of retail's game lights. The port-only PFRM keeps the mode; 0 writes nothing.
    if (s.part && fourcc == F("FRMD")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 3) {
        s.result.dropped.push_back("FRMD: not a constant 0 to 3");
      } else if (value != 0) {
        AppendBE32(s.out, F("PFRM"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, value);
      }
      return true;
    }
    // A POFS goes out with an INTT written as POFS above, added to it.
    if (s.part && fourcc == F("POFS") &&
        std::any_of(s.node.properties.begin(), s.node.properties.end(),
                    [this](const EffectProperty& other) { return other.fourcc == F("INTT") && IntIsPofs(other); })) {
      return true;
    }
    if (s.nestedHdrLight && (fourcc == F("LIRD") || fourcc == F("LORD") || fourcc == F("LIXP"))) {
      return true;  // consumed as PIR0/POR0/PLIX below
    }
    if (s.nestedHdrLight && s.nestedLightSpot && (fourcc == F("LIAN") || fourcc == F("LOAN"))) {
      return true;  // consumed as PLIA/PLOA below
    }
    if (s.boundsConsumed &&
        (fourcc == F("FXBR") || fourcc == F("FXBM") || fourcc == F("FXBO") || fourcc == F("FXBI"))) {
      return true;  // consumed as PFXB/PFXC below
    }
    uint32_t widen = 0;
    if (s.nestedHdrLight && fourcc == F("LSPS") && property.value.size() == 1 && ConstWord(property.value[0], widen)) {
      return true;  // consumed as PLSP below
    }
    return false;
  }

  // A retail property, converted as retail reads it.
  void RetailProperty(GeneratorState& s, const EffectProperty& property, uint32_t fourcc,
                     int varId) const {
    const auto found = s.retail.find(fourcc);
    // VGD/VMD mutate the same coordinate selector only when true (CreateGPSM 0x256b7c).
    if (s.part && ((fourcc >= F("VGD1") && fourcc <= F("VGD4")) ||
                 (fourcc >= F("VMD1") && fourcc <= F("VMD4")))) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": invalid constant");
        return;
      }
      if (value == 0) return;
      if (fourcc >= F("VGD1") && fourcc <= F("VGD4")) {
        AppendBE32(s.out, fourcc);
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
        return;
      }
    }
    // CreateGPSM 0x25acc4 and CreateSWSH 0x2dceac reset IEXP whenever DVVN is read. Preserve
    // stream order. The swoosh's default differs (GetDefaultInverseExposure 0x29c118: DVVN == 1)
    // and is applied by its factory.
    if ((s.part || s.swoosh) && (fourcc == F("DVVN") || fourcc == F("IEXP"))) {
      uint32_t value = 0;
      if (SmallInteger(property, value) && value <= (fourcc == F("DVVN") ? 255u : 1u)) {
        if (s.part) {
          s.inverseExposed = fourcc == F("DVVN") ? (value == 3 || value == 4) : value != 0;
        }
        AppendBE32(s.out, fourcc);
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, value);
      } else {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": invalid constant");
      }
      return;
    }
    // A swoosh's SEED is GetIntElementAsConst into desc+0x204 (CreateSWSH 0x2dc420: the value
    // when the element is constant, else 0), which replaces the system's seed when nonzero
    // (CParticleSwoosh ctor 0x27c6bc). The port-only PSED.
    if (s.swoosh && fourcc == F("SEED")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value)) {
        s.result.dropped.push_back("SEED: not a CNST");
      } else if (value != 0) {
        AppendBE32(s.out, F("PSED"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, value);
      }
      return;
    }
    // A swoosh's PSWT is GetIntElementAsConst into desc+0x20c (CreateSWSH 0x2dc8e0), the warm-up
    // the first Update 0x27ddec runs (InternalUpdate(pswt / 60)) only when nonzero: 0 changes
    // nothing. A swoosh warm-up is not ported.
    if (s.swoosh && fourcc == F("PSWT")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value)) {
        s.result.dropped.push_back("PSWT: not a CNST");
      } else if (value != 0) {
        s.result.dropped.push_back("PSWT: a swoosh warm-up");
      }
      return;
    }
    // A swoosh's TXUP is a GetBool into desc+0x22c bit 0x80 (CreateSWSH 0x2dc4f8), read by
    // UpdateRenderState 0x27e6b0 (CParticleSwoosh::PortTexUScroll). The port-only PTXU.
    if (s.swoosh && fourcc == F("TXUP")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("TXUP: invalid constant");
      } else if (value != 0) {
        AppendBE32(s.out, F("PTXU"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      }
      return;
    }
    // A swoosh's BLIT is a GetBool into desc+0x22c bit 0x1000 (CreateSWSH 0x2dc968), which the
    // CSwooshDescription ctor 0x29ad40 leaves clear (0xa00): 0 is the default and changes nothing.
    // Baked lighting on a swoosh (1) is not ported; no shipped swoosh sets it.
    if (s.swoosh && fourcc == F("BLIT")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value) || value > 1) {
        s.result.dropped.push_back("BLIT: invalid constant");
      } else if (value != 0) {
        s.result.dropped.push_back("BLIT: swoosh baked lighting not ported");
      }
      return;
    }
    // A swoosh's PSCL is a GetVectorElement into desc+0x138 (CreateSWSH 0x2dbea8), run for each
    // point after PROT (UpdateExisting 0x27d700) into point+0x40, which scales the local ring vertex
    // before the point's rotation (UpdateRenderData_Swoosh 0x2b607c). The port-only PSSC, in
    // Remastered's numbers: a scale along the ring's own axes, not a vector in the port's space.
    if (s.swoosh && fourcc == F("PSCL")) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (property.value.size() != 1 || !Element(property.value[0], Type::Vector, bytes, why)) {
        s.result.dropped.push_back("PSCL: " + (why.empty() ? std::string("not one vector") : why));
        return;
      }
      AppendBE32(s.out, F("PSSC"));
      s.out.insert(s.out.end(), bytes.begin(), bytes.end());
      return;
    }
    // A swoosh's LWQD is a GetIntElementAsConst clamped to 1..10 into desc+0x228 (CreateSWSH
    // 0x2dbae0): UpdateRenderData_Swoosh 0x2b6a58 splits each side face into that many strips
    // across the trail (CParticleSwoosh::PortRenderVfx). The port-only PLWQ.
    if (s.swoosh && fourcc == F("LWQD")) {
      uint32_t value = 0;
      if (!SmallInteger(property, value)) {
        s.result.dropped.push_back("LWQD: invalid constant");
      } else {
        value = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(value), 1, 10));
        if (value != 1) {
          AppendBE32(s.out, F("PLWQ"));
          AppendBE32(s.out, F("CNST"));
          AppendBE32(s.out, value);
        }
      }
      return;
    }
    // A swoosh's SNRD/SNRA are raw floats (GetReal) clamped to 0..10 / 0..60 into desc+0x21c/+0x220
    // (CreateSWSH 0x2dc280/0x2dc5bc). Either above 0 sizes each segment's spline steps by its
    // length and turn (UpdateRenderData_Swoosh 0x2b5a78; CParticleSwoosh::PortSplineSteps). The
    // port-only PSRD/PSRA, the clamped float's bits; 0 writes nothing.
    if (s.swoosh && (fourcc == F("SNRD") || fourcc == F("SNRA"))) {
      if (property.value.size() != 1 || property.value[0].kind != EffectValue::Kind::Word) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": not a raw float");
        return;
      }
      uint32_t bits = property.value[0].word;
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      if (!std::isfinite(value)) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": not finite");
        return;
      }
      value = std::clamp(value, 0.f, fourcc == F("SNRD") ? 10.f : 60.f);
      if (value > 0.f) {
        std::memcpy(&bits, &value, sizeof(bits));
        AppendBE32(s.out, fourcc == F("SNRD") ? F("PSRD") : F("PSRA"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, bits);
      }
      return;
    }
    // A generator's per-particle child system: PSPS names an embedded child and its type word, read
    // into desc+0x200; PSMX is an int element (desc+0x208) capping how many live at once
    // (CreateGPSM; ConstructParticleSpawnedChildSystem 0x24bc00). The child is built through
    // CParticleGen::ConstructNewParticleSystem, so it is any system: a GENP (CNST id, a PART) or
    // a SWSH (written SWHC id, a swoosh per particle). Port-only, same FourCCs.
    if (s.part && (fourcc == F("PSPS") || fourcc == F("PSMX"))) {
      std::vector<uint8_t> bytes;
      std::string why;
      bool ok = false;
      if (fourcc == F("PSMX")) {
        ok = Property(fourcc, PropertyType{Type::Int}, property.value, bytes, why);
      } else if (property.value.size() == 2 && property.value[0].kind == EffectValue::Kind::Guid &&
                 property.value[1].kind == EffectValue::Kind::Word &&
                 (property.value[1].word == F("GENP") || property.value[1].word == F("SWSH"))) {
        const bool swoosh = property.value[1].word == F("SWSH");
        ok = Property(fourcc, PropertyType{Type::Asset, swoosh ? F("SWHC") : F("PART")}, {property.value[0]},
                      bytes, why);
        if (ok && swoosh && bytes.size() == 8) {
          bytes[0] = 'S', bytes[1] = 'W', bytes[2] = 'H', bytes[3] = 'C';
        }
      } else {
        why = "not a GENP or SWSH id";
      }
      if (!ok) {
        s.result.dropped.push_back(EffectFourCCString(fourcc) + ": " + why);
        return;
      }
      s.written.insert(fourcc);
      AppendBE32(s.out, fourcc);
      s.out.insert(s.out.end(), bytes.begin(), bytes.end());
      return;
    }
    // SPWT/SPWR/SPWS/SPCL are the translation, rotation, scale and colour a generator hands its
    // child systems (SChildSystems::SetupSpawn 0x28d8ac, desc+0x298..0x2a4), read only when one is
    // built (ConstructChildSystemCoords 0x24c594) or updated (UpdateChildSystemExternalDataSet
    // 0x246508). A generator with no child system property never reads them.
    if (s.part && (fourcc == F("SPWT") || fourcc == F("SPWR") || fourcc == F("SPWS") || fourcc == F("SPCL")) &&
        !s.Has(F("ICTS")) && !s.Has(F("IDTS")) && !s.Has(F("IITS")) && !s.Has(F("KSSM")) &&
        !s.Has(F("SSWH")) && !s.Has(F("SELC")) && !s.Has(F("PSPS"))) {
      return;
    }
    // SPWS, the scale a generator hands its children in place of its system scale (SetupSpawn
    // +0x2a0 = desc+0x288), evaluated in its own context at spawn and every tick: the port-only
    // PSWS, in Remastered's scale numbers as PINS is. An IRND, KPIN or RAZY in it (drawn once per
    // system) is not decoded, nor is a mode-5 parent's (its children get identity coords).
    if (s.part && fourcc == F("SPWS")) {
      std::vector<uint8_t> inner;
      std::string why = "not one element";
      if (m_xfmd == 5) {
        why = "under XFMD 5";
      } else if (property.value.size() == 1) {
        why.clear();
        for (const uint32_t init : {F("IRND"), F("KPIN"), F("RAZY")}) {
          if (why.empty() && ContainsElement(property.value[0], init)) {
            why = "with " + EffectFourCCString(init);
          }
        }
        if (why.empty() && !Element(property.value[0], Type::Vector, inner, why)) {
          why = why.empty() ? "not a vector" : why;
        }
      }
      if (!why.empty()) {
        s.result.dropped.push_back("SPWS: " + why);
        return;
      }
      AppendBE32(s.out, F("PSWS"));
      Basis(F("BSSC"), inner, s.out);
      return;
    }
    if (found == s.retail.end()) {
      s.result.dropped.push_back(EffectFourCCString(property.fourcc) + ": Remastered only");
      return;
    }
    std::vector<uint8_t> bytes;
    std::string why;
    if (s.part && fourcc == F("PMDL")) {
      s.pmdlModels = ModelGuids(property.value);
    }
    // A model chosen per particle: the first as PMDL, all of them as PMDV.
    if (s.part && fourcc == F("PMDL") && property.value.size() == 1 && IsElement(property.value[0], F("SLCT"))) {
      std::vector<uint32_t> ids;
      if (!Variants(property.value, ids, why)) {
        s.result.dropped.push_back("PMDL: " + why);
        ++s.result.droppedRetail;
        return;
      }
      AppendBE32(s.out, F("PMDL"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, ids[0]);
      if (std::all_of(ids.begin(), ids.end(), [&](uint32_t id) { return id == ids[0]; })) {
        s.pmdl = ids[0];  // one model, as a constant PMDL
        return;
      }
      s.pmdlVariants = true;
      AppendBE32(s.out, F("PMDV"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, uint32_t(ids.size()));
      for (const uint32_t id : ids) {
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, id);
      }
      return;
    }
    m_varReads = 0;
    const bool propertyOk = Property(fourcc, found->second, property.value, bytes, why);
    if (varId >= 0) {
      PartVar var{uint32_t(varId)};
      var.reads = m_varReads;
      var.flags = property.value.size() == 1 ? ConstantFlags(property.value[0], Type::Real) : 0;
      var.converted = propertyOk;
      s.partVars.push_back(std::move(var));
    }
    if (!propertyOk) {
      s.result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " + why);
      ++s.result.droppedRetail;
      return;
    }
    if (fourcc == F("TEXR")) {
      s.texture = true;
    }
    if (fourcc == F("PMDL") && bytes.size() == 8) {
      s.pmdl = port::GetBE32(bytes, 4);
    }
    s.written.insert(fourcc);
    const size_t begin = s.out.size();
    AppendBE32(s.out, fourcc);
    s.out.insert(s.out.end(), bytes.begin(), bytes.end());
    if (varId >= 0) {
      s.partVars.back().begin = begin;
      s.partVars.back().end = s.out.size();
    }
  }

  // A variable that reads a dropped one would evaluate it as zero, which Remastered does not:
  // drop the reader too, transitively (the retail ADV1..8 properties are taken back out).
  void DropVariableReaders(GeneratorState& s) const {
    if (s.part || s.swoosh) {
      uint32_t lost = 0;
      for (const PartVar& v : s.partVars) {
        lost |= v.converted ? 0u : 1u << v.id;
      }
      for (bool again = true; again;) {
        again = false;
        for (PartVar& v : s.partVars) {
          if (v.converted && (v.reads & lost) != 0) {
            v.converted = false;
            lost |= 1u << v.id;
            again = true;
            s.result.dropped.push_back(PartVarName(v.id, s.swoosh) + ": reads a dropped variable");
          }
        }
      }
      std::vector<const PartVar*> taken;
      for (const PartVar& v : s.partVars) {
        if (!v.converted && v.end > v.begin) {
          taken.push_back(&v);
        }
      }
      std::sort(taken.begin(), taken.end(), [](const PartVar* a, const PartVar* b) { return a->begin > b->begin; });
      for (const PartVar* v : taken) {
        s.out.erase(s.out.begin() + v->begin, s.out.begin() + v->end);
      }
    }
  }

  // The primitive, the material's texture and VMAT, the transform and the model mesh.
  void DrawWrites(GeneratorState& s) const {
    if (s.part && s.node.primitiveType >= 0 && (s.node.primitiveType != 1 || s.nullMaterial)) {
      AppendBE32(s.out, F("PPRM"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, s.drawsNothing ? 0u : uint32_t(s.node.primitiveType));
    }
    if (s.part && s.node.primitiveType == 1 && !s.Has(F("SIZE"))) {
      // DVVN 1..4 makes a sprite's missing SIZE 1.0 (SSpritePrim::Fixup 0x28e640,
      // GetDefaultParticleSize 0x28e6c8), where retail's default is 0.1.
      uint32_t dvvn = 0;
      for (const EffectProperty& property : s.node.properties) {
        if (property.fourcc == F("DVVN") && !SmallInteger(property, dvvn)) {
          dvvn = 0;
        }
      }
      if (dvvn >= 1 && dvvn <= 4) {
        AppendBE32(s.out, F("SIZE"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, FloatBits(1.0f));
      }
    }
    // A material instance draws with its texture where there is no TEXR.
    if (s.material != nullptr && !s.texture && !s.electric) {
      const EffectValue* guid = nullptr;
      for (const EffectValue& value : s.material->value) {
        if (value.kind == EffectValue::Kind::Guid) {
          guid = &value;
        }
      }
      const uint32_t id = guid != nullptr && m_io.materialTexture ? m_io.materialTexture(guid->guid) : 0;
      if (id != 0) {
        AppendBE32(s.out, F("TEXR"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, id);
      } else {
        s.result.dropped.push_back("MTIN: no texture for its material");
        ++s.result.droppedRetail;
      }
    }
    const bool customVmat = (s.part || s.swoosh || s.electric) && s.material != nullptr &&
                            Material(*s.material, s.node, s.result, s.swoosh || s.electric, s.electric);
    // With a TEXR the material still draws: MTIN shaders sample only the material's own textures
    // (kb topic/header-mtrl-perm048.md), and the TEXR stays the retail-path texture. Only a material
    // the VMAT can't carry loses it.
    if (s.material != nullptr && s.texture && !s.electric && !customVmat) {
      s.result.dropped.push_back("MTIN: the effect has a TEXR");
    }
    const bool headerVmat =
        !customVmat && s.part && !s.customMaterial && !s.drawsNothing && HeaderMaterial(s.node, s.result);
    const bool vmat = customVmat || headerVmat;
    // ANTH is drawn only by the header shaders that take it (HeaderMaterial refuses it elsewhere).
    if (s.anth && !headerVmat && !s.drawsNothing) {
      s.result.dropped.push_back("ANTH: not drawn by this material");
    }
    // A node that draws nothing (no primitive, or no material at all) loses no shading.
    // A model generator with no MTIN draws its CMDL's own materials (UpdateModelReadiness
    // 0x2bba34: no custom material -> SetupInstancingRegistrations), tinted by the particle's
    // DIFC (RenderNonInstanced 0x2bd27c: COLR.rgb x max(ITEN, 0) x the inverse exposure IEXP
    // selects). PITN carries ITEN (CNST 1 where it has none) and marks the generator as one.
    const bool ownModelMaterials =
        s.part && !vmat && s.material == nullptr && s.node.primitiveType == 3 && s.Has(F("PMDL"));
    if (ownModelMaterials) {
      const auto iten = std::find_if(s.node.properties.begin(), s.node.properties.end(),
                                     [&](const EffectProperty& property) { return property.fourcc == F("ITEN"); });
      std::vector<uint8_t> bytes;
      std::string why;
      if (iten == s.node.properties.end()) {
        AppendBE32(bytes, F("CNST"));
        AppendBE32(bytes, 0x3f800000);
      } else if (iten->value.size() != 1 || !Element(iten->value[0], Type::Real, bytes, why)) {
        s.result.dropped.push_back("ITEN: " + (why.empty() ? std::string("not one element") : why));
        bytes.clear();
        AppendBE32(bytes, F("CNST"));
        AppendBE32(bytes, 0x3f800000);
      }
      AppendBE32(s.out, F("PITN"));
      s.out.insert(s.out.end(), bytes.begin(), bytes.end());
    }
    if (!vmat && !s.drawsNothing) {
      if (s.part && s.inverseExposed && !ownModelMaterials) {
        s.result.dropped.push_back("IEXP: no VMAT");
      }
      if (s.drawsSsze) {
        s.result.dropped.push_back("SSZE: drawn square (no VMAT)");
      }
      // SFTD (dyn+0xdc, UpdateRenderState_Node 0x2486e8) reaches only the node's PZSO scalar
      // (UpdateRenderDataForContext 0x2b0cec), which is looked up in the custom material's
      // instance data (node+0x80, UpdateCustomMaterialReadyToRender 0x2b0bbc): the CMDL's own
      // materials never read it.
      for (const uint32_t fourcc : s.portOnly) {
        if (!(ownModelMaterials && (fourcc == F("ITEN") || fourcc == F("SFTD")))) {
          s.result.dropped.push_back(EffectFourCCString(fourcc) + ": no VMAT");
        }
      }
    }
    if (s.pmph) {
      for (const EffectGuid& model : s.pmdlModels) {
        bool animatesUv = true;
        if (!m_io.modelAnimatesUv || !m_io.modelAnimatesUv(model, animatesUv)) {
          s.result.dropped.push_back("PMPH: CMDL " + EffectGuidString(model) + " unread");
          break;
        }
        if (animatesUv) {
          s.result.dropped.push_back("PMPH: CMDL " + EffectGuidString(model) + " has ANUV");
          break;
        }
      }
    }
    if (s.part && m_partSys == 1) {
      AppendBE32(s.out, F("XFMD"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    } else if (s.xfmd != nullptr) {
      Transform(*s.xfmd, s.mover, s.node, s.out);
    } else if (s.mover != nullptr) {
      s.result.dropped.push_back("SMVR: no XFMD");
    }
    if (s.emitterMover != nullptr) {
      EmitterMover(*s.emitterMover, s.xfmd, s.mover != nullptr, s.node, s.out, s.result.dropped);
    }
    if (vmat && s.pmdl != 0 && !s.pmdlVariants && m_io.modelMesh) {
      // VMSH: the converted PMDL as one mesh, so the model particle draws through the VMAT.
      std::vector<uint8_t> mesh = m_io.modelMesh(s.pmdl);
      // VMSH v2 flags (BE word at 12): bit 1 every vertex has a colour; bit 2 the colour tints the draw,
      // where the material's shader has the instanced-model perm with COLOR (016), as Remastered draws a
      // coloured mesh: vcol = 2 x COLOR^2.2 x the particle colour, alpha COLOR.a x the particle's. The
      // Plasma charge starburst's rays fade out by that alpha (0 on the tips).
      static const std::unordered_set<uint32_t> kColorPerm = {
          0x0cbeb3de, 0x0cc45d52, 0x0fb73e1b, 0x1a9ef0d8, 0x207abf3f, 0x33b751f2, 0x3db95827, 0x3fd46bb7,
          0x440fbd96, 0x49a43a10, 0x4e386476, 0x5f2a0715, 0x71dc2465, 0x7d52bd6b, 0x80a47126, 0x90ce7873,
          0x96e205db, 0x9ea86831, 0xa327502c, 0xb8eed42e, 0xc5cd782b, 0xe4d4c263, 0xfa49ff3c};
      if (mesh.size() >= 16 && mesh[3] == 2 && (mesh[15] & 2) != 0 && kColorPerm.count(m_vmatShader) != 0) {
        mesh[15] |= 4;
      }
      if (!mesh.empty()) {
        AppendBE32(s.out, F("VMSH"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, uint32_t(mesh.size()));
        s.out.insert(s.out.end(), mesh.begin(), mesh.end());
      }
    }
  }

  // The first swoosh and electric child the spawn table starts, where the
  // generator has none of its own.
  void SpawnedSwooshes(GeneratorState& s) const {
    std::vector<std::pair<uint32_t, uint32_t>> extraSwooshes;  // (id, frame), port-only PSWX
    for (const auto& [type, child, frame] : {std::tuple(F("SWHC"), F("SSWH"), F("SSSD")),
                                              std::tuple(F("ELSC"), F("SELC"), F("SESD"))}) {
      size_t count = 0;
      for (const Started& each : s.started) {
        if (each.type != type) {
          continue;
        }
        if (count++ == 0 && s.written.count(child) == 0) {
          AppendBE32(s.out, child);
          AppendBE32(s.out, F("CNST"));
          AppendBE32(s.out, each.id);
          if (s.written.count(frame) == 0) {
            AppendBE32(s.out, frame);
            AppendBE32(s.out, F("CNST"));
            AppendBE32(s.out, each.frame);
          } else if (each.frame != 0) {
            m_approximated.push_back("KSSM: " + EffectFourCCString(child) + " frame kept from " +
                                     EffectFourCCString(frame));
          }
        } else if (type == F("SWHC")) {
          // Port-only PSWX: retail's generator spawns one swoosh, Remastered's every one it starts.
          extraSwooshes.emplace_back(each.id, each.frame);
        } else {
          s.result.dropped.push_back("KSSM: a spawned " + EffectFourCCString(type) + " beyond retail's one");
          ++s.result.droppedRetail;
        }
      }
    }
    if (!extraSwooshes.empty()) {
      AppendBE32(s.out, F("PSWX"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, uint32_t(extraSwooshes.size()));
      for (const auto& [id, frame] : extraSwooshes) {
        AppendBE32(s.out, id);
        AppendBE32(s.out, frame);
      }
    }
  }

  // The port-only flags and tables gathered over the properties.
  void StateWrites(GeneratorState& s) const {
    if (s.spriteShape != 0) {
      AppendBE32(s.out, F("PSHP"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, s.spriteShape);
    }
    if (s.unfogged) {
      AppendBE32(s.out, F("PDFG"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
    if (s.noZTest) {
      AppendBE32(s.out, F("PZTS"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 0);
    }
    if (s.waterSort != 0) {
      AppendBE32(s.out, F("PWST"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, s.waterSort);
    }
    if (s.noIvse) {
      AppendBE32(s.out, F("PIVS"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 0);
    }
    // No ITPE: Remastered's default 1, where the system's translation change is decoded.
    if (s.part && m_io.spreadDefault && !s.itpe && (m_xfmd == 2 || m_xfmd == 5) && !(m_xfmd == 5 && m_pofs == 2)) {
      s.spreadMode = int(m_xfmd == 5 ? (m_pofs == 1 ? 2 : 1) : 0) | kSpreadDefault;
      m_approximated.push_back("ITPE: absent, Remastered's default 1");
    }
    if (s.spreadMode >= 0) {
      AppendBE32(s.out, F("PITP"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, uint32_t(s.spreadMode));
    }
    if (s.depthBias != 0) {
      AppendBE32(s.out, F("PDBS"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, s.depthBias);
    }
    if (s.depthBias != 0 && !s.depthBiasUsesDepthRange) {
      AppendBE32(s.out, F("PDBO"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 0u);
    }
    if (s.sortBias != 0) {
      AppendBE32(s.out, F("PSBS"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, s.sortBias);
    }
    if (s.sleepDistance != 0) {
      AppendBE32(s.out, F("PSLD"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, s.sleepDistance);
    }
    if ((s.part || s.swoosh) && !m_vars.empty()) {
      // Port-only: the effect's PVAR table (names, kinds, defaults) for VARF/VARI/VARV/VARC.
      AppendBE32(s.out, F("PVRT"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, uint32_t(m_vars.size()));
      for (const PvarEntry& var : m_vars) {
        s.out.insert(s.out.end(), var.guid.begin(), var.guid.end());
        AppendBE32(s.out, var.type);
        for (float f : var.def) {
          AppendBE32(s.out, FloatBits(f));
        }
      }
    }
    if (!s.sysItems.empty()) {
      // Port-only: the system variables in evaluation order; each is its
      // EPortVarType, slot, init-only flag and element (CPortSysItem).
      AppendBE32(s.out, F("PSYS"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, uint32_t(s.sysItems.size()));
      for (const SystemItem& item : s.sysItems) {
        AppendBE32(s.out, item.type);
        AppendBE32(s.out, item.slot);
        AppendBE32(s.out, item.initOnly);
        s.out.insert(s.out.end(), item.element.begin(), item.element.end());
      }
    }
  }

  // The variables in the order Remastered evaluates them (SortVars 0x276188 orders its variables
  // so that each comes after those it reads, otherwise by stream order): ADV1 = KPIN(PAP2 + ..)
  // reads ADV2 = 0.5 of the same particle. A node with ADV9 or an IAVn writes them all as the
  // port-only PPVR (with each one's constant-after-start flag); otherwise ADV1..8 are the
  // port-only PADO, in index order, when that is not 1..8. A swoosh's per-point variables are
  // all in its PPVR.
  void ParticleVariableWrites(GeneratorState& s) const {
    if ((s.part || s.swoosh) && !s.partVars.empty()) {
      const bool ppvr = s.swoosh || std::any_of(s.partVars.begin(), s.partVars.end(), [](const PartVar& v) {
        return v.id >= 8 && v.converted;
      });
      std::vector<const PartVar*> pending;
      for (const PartVar& v : s.partVars) {
        if (ppvr || v.id < 8) {  // a dropped ADV9 or IAVn is not evaluated
          pending.push_back(&v);
        }
      }
      uint32_t present = 0;
      for (const PartVar* v : pending) {
        present |= 1u << v->id;
      }
      // PADO lists all of ADV1..8, an absent one waiting on nothing, in index order.
      std::vector<PartVar> absent;
      if (!ppvr) {
        absent.reserve(8);
        for (uint32_t n = 0; n < 8; ++n) {
          if ((present & 1u << n) == 0) {
            absent.push_back(PartVar{n});
            pending.push_back(&absent.back());
          }
        }
        std::stable_sort(pending.begin(), pending.end(), [](const PartVar* a, const PartVar* b) { return a->id < b->id; });
      }
      std::vector<const PartVar*> sorted;
      uint32_t done = 0;
      while (!pending.empty()) {
        auto pick = pending.end();
        for (auto it = pending.begin(); it != pending.end() && pick == pending.end(); ++it) {
          if (((*it)->reads & present & ~done & ~(1u << (*it)->id)) == 0) {
            pick = it;
          }
        }
        if (pick == pending.end()) {
          pick = pending.begin();  // a cycle: stream order
        }
        done |= 1u << (*pick)->id;
        sorted.push_back(*pick);
        pending.erase(pick);
      }
      if (ppvr) {
        std::vector<const PartVar*> written;
        for (const PartVar* v : sorted) {
          if (v->converted) {
            written.push_back(v);
          }
        }
        if (written.empty()) {
          return;
        }
        AppendBE32(s.out, F("PPVR"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, uint32_t(written.size()));
        for (const PartVar* v : written) {
          // Kinds 2, 3 and 4 are IAVn, VAVn and CAVn.
          AppendBE32(s.out, v->id >= 9 ? 2 + (v->id - 9) / 4 : s.swoosh || v->id == 8 ? 1 : 0);
          AppendBE32(s.out, v->id >= 9 ? (v->id - 9) % 4 : v->id);
          AppendBE32(s.out, (v->flags & 3) != 0);
          s.out.insert(s.out.end(), v->element.begin(), v->element.end());
        }
      } else {
        // PADO: all of ADV1..8.
        std::vector<uint32_t> advOrder;
        for (const PartVar* v : sorted) {
          advOrder.push_back(v->id);
        }
        bool identity = true;
        for (uint32_t n = 0; n < 8; ++n) {
          identity = identity && advOrder[n] == n;
        }
        if (!identity) {
          AppendBE32(s.out, F("PADO"));
          AppendBE32(s.out, F("CNST"));
          AppendBE32(s.out, uint32_t(advOrder.size()));
          for (const uint32_t n : advOrder) {
            AppendBE32(s.out, n);
          }
        }
      }
    }
  }

  void VisorWrites(GeneratorState& s) const {
    if ((s.part || s.electric) && s.visorData) {
      // Port-only: the visor effect data is present (PVSD), with its size range as float bits.
      AppendBE32(s.out, F("PVSD"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
      if (s.visorMinScale) {
        AppendBE32(s.out, F("PVSN"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, *s.visorMinScale);
      }
      if (s.visorMaxScale) {
        AppendBE32(s.out, F("PVSM"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, *s.visorMaxScale);
      }
      // The override colours, blend and duration (ticks, int) of CVisorEffectManagerMP1.
      const std::pair<uint32_t, const std::vector<uint8_t>*> visorElements[] = {
          {F("PVSC"), &s.visorStartColor}, {F("PVMC"), &s.visorMidColor},
          {F("PVTC"), &s.visorEndColor},   {F("PVBS"), &s.visorBlend}};
      for (const auto& [tag, bytes] : visorElements) {
        if (!bytes->empty()) {
          AppendBE32(s.out, tag);
          s.out.insert(s.out.end(), bytes->begin(), bytes->end());
        }
      }
      if (s.visorTicks) {
        AppendBE32(s.out, F("PVDT"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, *s.visorTicks);
      }
    }
  }

  void NestedLightWrites(GeneratorState& s) const {
    if (s.nestedHdrLight) {
      AppendBE32(s.out, F("PHDR"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
      AppendBE32(s.out, F("PIR0"));
      s.out.insert(s.out.end(), s.nestedLightInnerRadius.begin(), s.nestedLightInnerRadius.end());
      AppendBE32(s.out, F("POR0"));
      s.out.insert(s.out.end(), s.nestedLightOuterRadius.begin(), s.nestedLightOuterRadius.end());
      if (s.nestedLightFalloff != 3) {  // absent PHFO is the smooth falloff
        AppendBE32(s.out, F("PHFO"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, s.nestedLightFalloff);
      }
      if (s.nestedLightInverseExposure) {  // absent PLIX is LIXP off, CParticleLightDescription's default
        AppendBE32(s.out, F("PLIX"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, 1);
      }
      if (s.nestedLightWiden != 0) {  // absent PLSP is CLight+0x74's 0
        AppendBE32(s.out, F("PLSP"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, s.nestedLightWiden);
      }
      if (s.nestedLightSpot) {
        AppendBE32(s.out, F("PLIA"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, s.nestedLightInnerAngle);
        AppendBE32(s.out, F("PLOA"));
        AppendBE32(s.out, F("CNST"));
        AppendBE32(s.out, s.nestedLightOuterAngle);
      }
    }
    if (s.lightOnce) {
      AppendBE32(s.out, F("PLON"));
      AppendBE32(s.out, F("CNST"));
      AppendBE32(s.out, 1);
    }
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
  mutable bool m_varsRead = false;                  // m_vars came from a well-formed PVAR
  mutable std::vector<SystemItem>* m_sysItems = nullptr;  // set while an SVR/SVI/SVC/SVV is written
  mutable uint32_t m_sysNextHidden = 10;            // the next hidden init-only real
  mutable std::vector<SystemItem>* m_spawnSysItems = nullptr;  // set while a KSSM is written
  mutable uint32_t m_sysNextHiddenInt = 10;                   // the next hidden init-only int
  mutable std::vector<std::string> m_approximated;  // for the generator being written
  mutable uint32_t m_vmatShader = 0;                // the shader of the last VMAT written
  mutable bool m_keeps = false;                     // a KPIN may be a per-particle PKPR
  mutable uint32_t m_keepNext = 0;                  // the next PKPR slot
  mutable uint32_t m_varReads = 0;                  // the particle variables Element() has read
  mutable bool m_swoosh = false;                    // the node being written is a SWSH
  mutable uint32_t m_swooshVars = 0;                // its ADV0..8 and IAV0..3 (SwooshVarId), a bit each
  mutable bool m_gpsm = false;                      // the node being written is a GPSM
  mutable uint32_t m_xfmd = 2;                      // its XFMD mode, 2 without one, 1 forced by its SYST..
  mutable int m_partSys = -1;                       // a GPSM's PartSystemMode
  mutable std::string m_partSysWhy;                 // and its drop reason
  mutable const EffectValue* m_semrPos = nullptr;   // the position child of the SEMR being written
  mutable bool m_emtr = false;                      // an EMTR property is being written
  mutable int64_t m_maxp = -1;                      // its MAXP when a constant, else -1
  // XFMD 5's INTT, and one under XFMD 2/3 that reads no EXTT, is written as retail's POFS.
  bool IntIsPofs(const EffectProperty& intt) const {
    return m_xfmd == 5 || ((m_xfmd == 2 || m_xfmd == 3) && intt.value.size() == 1 &&
                           !ContainsElement(intt.value[0], F("EXTT")));
  }
  mutable uint32_t m_pofs = 0;                      // its POFS (and an INTT written as POFS): 0 none, 1 constant, 2 other
  mutable bool m_mp1Space = false;                  // inside MPCB(mod vector): its vectors are retail's
  mutable int m_lifetimeOffset = -1;                // LifetimeOffset of the property being written
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
    // Port-only PKPR: a per-particle latch slot, then the element latched.
    if (fourcc == F("PKPR") && type == Type::Real) {
      uint32_t slot;
      return Word(slot) && slot < kKeepSlots && Element(Type::Real);
    }
    // Port-only PIRS: a per-particle slot (or all ones with none), then the element.
    if (fourcc == F("PIRS") && type == Type::Real) {
      uint32_t slot;
      return Word(slot) && (slot < kKeepSlots || slot == 0xffffffffu) && Element(Type::Real);
    }
    // SPA*: the parent's system variable n, then the fallback; DPV*/TPV*: a
    // PVAR guid, then the fallback.
    if (IsParentRead(fourcc, type)) {
      const bool guid = (fourcc >> 24) != 'S';
      return Skip(guid ? 16 : 4) && Element(type);
    }
    // Port-only PCRN: P and F, the amplitude A, then NONE or CNST and a VECF id.
    if (fourcc == F("PCRN") && type == Type::Vector) {
      uint32_t field;
      if (!Element(Type::Vector) || !Element(Type::Real) || !Element(Type::Vector) || !Word(field)) {
        return false;
      }
      return field == F("NONE") || (field == F("CNST") && Skip(4));
    }
    // Port-only PKYF: the loop byte, N, the loop start, start and rate, N keys (three floats each
    // for a vector), then the input.
    if (fourcc == F("PKYF") && (type == Type::Real || type == Type::Vector)) {
      uint32_t count;
      return Skip(1) && Word(count) && count >= 1 && count <= 0x100000 && Skip(12) &&
             Skip(size_t(count) * (type == Type::Vector ? 12 : 4)) && Element(Type::Real);
    }
    // Port-only PRTV: the vector, then EXTR, CMRT, INTR or REUL and its three angles.
    if (fourcc == F("PRTV") && type == Type::Vector) {
      uint32_t rotation;
      if (!Element(Type::Vector) || !Word(rotation)) {
        return false;
      }
      if (rotation == F("REUL")) {
        return Element(Type::Real) && Element(Type::Real) && Element(Type::Real);
      }
      return rotation == F("EXTR") || rotation == F("CMRT") || rotation == F("INTR");
    }
    // Port-only PGRD (a swoosh's gradient): stop count, five floats per stop, the period as an
    // int element, then the repeat byte.
    if (fourcc == F("PGRD") && type == Type::Color) {
      uint32_t count;
      return Word(count) && count <= 0x10000 && Skip(size_t(count) * 20) && Element(Type::Int) && Skip(1);
    }
    // Port-only SLCT: the index as an int element, the entry count, then the entries.
    if (fourcc == F("SLCT") && (type == Type::Real || type == Type::Vector || type == Type::Color)) {
      uint32_t count;
      if (!Element(Type::Int) || !Word(count) || count == 0 || count > 256) {
        return false;
      }
      for (uint32_t i = 0; i < count; ++i) {
        if (!Element(type)) {
          return false;
        }
      }
      return true;
    }
    const char* shape = ShapeOf(fourcc, type);
    if (shape == nullptr) {
      m_error = "no " + EffectFourCCString(fourcc) + " element of that type";
      return false;
    }
    for (const char* c = shape; *c != 0; ++c) {
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
      case 'd':
        ok = Skip(1);
        break;
      case 'w':
        ok = Skip(4);
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
    // PATL: CNST id, then cols, rows, count, mode and flipX as CNST ints. PAFB: the same with loop
    // for mode, then the cycle as an int element.
    if (fourcc == F("PATL") || fourcc == F("PAFB")) {
      uint32_t sub;
      if (!Word(sub) || sub != F("CNST") || !Skip(4)) {
        return false;
      }
      for (int i = 0; i < 5; ++i) {
        if (!Word(sub) || sub != F("CNST") || !Skip(4)) {
          return false;
        }
      }
      return fourcc == F("PATL") || Element(Type::Int);
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

  // A port-only `CNST <word>` property (VORN, XFMD, PIRN).
  bool PortWord() {
    uint32_t fourcc;
    return Word(fourcc) && fourcc == F("CNST") && Skip(4);
  }

  // The port-only PADO property: CNST, a count up to 8, then that many words.
  bool PortWords() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 8 && Skip(size_t(count) * 4);
  }

  // The port-only PVRT property: CNST, a count, then 36 bytes a variable.
  bool PortVariables() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 4096 && Skip(size_t(count) * 36);
  }

  // The port-only PSYS property: CNST, a count, then per item its type, slot,
  // init-only flag and element.
  bool PortSystemVars() {
    static constexpr Type kTypes[4] = {Type::Real, Type::Int, Type::Color, Type::Vector};
    uint32_t fourcc, count;
    if (!Word(fourcc) || fourcc != F("CNST") || !Word(count) || count > 256) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t type, slot, initOnly;
      if (!Word(type) || type > 3 || !Word(slot) || slot > 255 || !Word(initOnly) || initOnly > 1 ||
          !Element(kTypes[type])) {
        if (m_error.empty()) {
          m_error = "bad PSYS";
        }
        return false;
      }
    }
    return true;
  }

  // The port-only PPVR property: CNST, a count up to 21, then per variable its kind, slot and
  // constant-after-start flag, and for kinds 1 (a real), 2 (an int), 3 (a vector) and 4 (a
  // colour) the element. A swoosh has no kind 0 (its ADV0..8 are all kind 1).
  bool PortPartVars(bool swoosh) {
    uint32_t fourcc, count;
    if (!Word(fourcc) || fourcc != F("CNST") || !Word(count) || count > 21) {
      return false;
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t kind, slot, flag;
      const Type types[] = {Type::Real, Type::Real, Type::Int, Type::Vector, Type::Color};
      if (!Word(kind) || kind > 4 || !Word(slot) || slot >= (kind >= 2 ? 4u : kind == 1 ? 9u : 8u) ||
          (swoosh ? kind == 0 : kind == 1 && slot != 8) || !Word(flag) || flag > 1 ||
          (seen & (1u << (kind >= 2 ? 9 + (kind - 2) * 4 + slot : slot))) != 0 ||  // the factory rejects a duplicate
          (kind != 0 && !Element(types[kind]))) {
        if (m_error.empty()) {
          m_error = "bad PPVR";
        }
        return false;
      }
      seen |= 1u << (kind >= 2 ? 9 + (kind - 2) * 4 + slot : slot);
    }
    return true;
  }

  // The port-only PSWX property: CNST, a count, then an id and a frame each.
  bool PortExtraSwooshes() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 64 && Skip(size_t(count) * 8);
  }

  // PKSM (Writer::SpawnTables): CNST, end, tables; per table start, const flag, period
  // element, entries; per entry frame, spawns; per spawn type, id, instance element and
  // component index; then the components.
  bool PortSpawnTables() {
    uint32_t fourcc, tables;
    if (!Word(fourcc) || fourcc != F("CNST") || !Skip(4) || !Word(tables) || tables > 64) {
      return false;
    }
    uint32_t used = 0;
    for (uint32_t t = 0; t < tables; ++t) {
      uint32_t entries;
      if (!Skip(8) || !Element(Type::Int) || !Word(entries) || entries > 1000) {
        return false;
      }
      for (uint32_t e = 0; e < entries; ++e) {
        uint32_t spawns;
        if (!Skip(4) || !Word(spawns) || spawns > 100) {
          return false;
        }
        for (uint32_t s = 0; s < spawns; ++s) {
          uint32_t component;
          if (!Skip(8) || !Element(Type::Int) || !Word(component)) {
            return false;
          }
          if (component != ~0u) {
            used = std::max(used, component + 1);
          }
        }
      }
    }
    uint32_t components;
    if (!Word(components) || components > 1000 || used > components) {
      m_error = "spawn component out of range";
      return false;
    }
    for (uint32_t c = 0; c < components; ++c) {
      if (!Mover()) {
        return false;
      }
    }
    return true;
  }

  // PSMV (Writer::Transform) and PEMR (Writer::EmitterMover): CNST, a count of 0 or 1, then the mover.
  bool PortMover() {
    uint32_t fourcc, count;
    return Word(fourcc) && fourcc == F("CNST") && Word(count) && count <= 1 && (count == 0 || Mover());
  }

  // Writer::Mover's SMOV: position, delta, rotation (or NONE), modifier (NONE or GRAV).
  bool Mover() {
    uint32_t word;
    if (!ComponentVector(0) || !Skip(12) || !Word(word) || (word != F("NONE") && !ComponentRotation(word, 0)) ||
        !Word(word) || (word != F("NONE") && (word != F("GRAV") || !ComponentVector(0)))) {
      if (m_error.empty()) {
        m_error = "bad mover";
      }
      return false;
    }
    return true;
  }

  // Writer::ComponentReal/Vector/Rotation's nodes.
  bool ComponentReal(int depth) {
    uint32_t fourcc, count;
    if (depth > 64 || !Word(fourcc)) {
      return false;
    }
    if (fourcc == F("CNST") || fourcc == F("VARF") || fourcc == F("PSYR")) {
      return Skip(4);
    }
    if (fourcc == F("KEYE")) {
      return Skip(18) && Word(count) && count != 0 && count <= 0x100000 && Skip(size_t(count) * 4);
    }
    if (fourcc == F("ADD_") || fourcc == F("MULT") || fourcc == F("IRND") || fourcc == F("RAND")) {
      return ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    m_error = "component real " + EffectFourCCString(fourcc);
    return false;
  }

  bool ComponentVector(int depth) {
    uint32_t fourcc;
    if (depth > 64 || !Word(fourcc)) {
      return false;
    }
    if (fourcc == F("EXTT") || fourcc == F("EXTS")) {
      return true;
    }
    if (fourcc == F("KPIN")) {
      return ComponentVector(depth + 1);
    }
    if (fourcc == F("ADD_") || fourcc == F("MULT")) {
      return ComponentVector(depth + 1) && ComponentVector(depth + 1);
    }
    if (fourcc == F("ROTV")) {
      uint32_t rotation;
      return ComponentVector(depth + 1) && Word(rotation) && ComponentRotation(rotation, depth + 1);
    }
    if (fourcc == F("CNST")) {
      return ComponentReal(depth + 1) && ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    if (fourcc == F("CIRC")) {
      return ComponentVector(depth + 1) && ComponentVector(depth + 1) && ComponentReal(depth + 1) &&
             ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    m_error = "component vector " + EffectFourCCString(fourcc);
    return false;
  }

  // A rotation node whose fourcc has been read.
  bool ComponentRotation(uint32_t fourcc, int depth) {
    if (depth > 64) {
      return false;
    }
    if (fourcc == F("EXTR") || fourcc == F("IDNT")) {
      return true;
    }
    if (fourcc == F("REUL")) {
      return ComponentReal(depth + 1) && ComponentReal(depth + 1) && ComponentReal(depth + 1);
    }
    uint32_t a, b;
    if (fourcc == F("RADD")) {
      return Word(a) && ComponentRotation(a, depth + 1) && Word(b) && ComponentRotation(b, depth + 1);
    }
    if (fourcc == F("KPIN")) {
      return Word(a) && ComponentRotation(a, depth + 1);
    }
    m_error = "component rotation " + EffectFourCCString(fourcc);
    return false;
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

bool ReadMaterialParams(const std::vector<uint8_t>& mati, MaterialParams& out) {
  Mati parsed;
  if (!ParseMati(mati, parsed)) {
    return false;
  }
  out = MaterialParams();
  out.shader = parsed.shader;
  for (const auto& [tag, texture] : parsed.textures) {
    out.textures[tag] = texture.guid;
  }
  out.vectors = parsed.vectors;
  return true;
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
  const bool electric = type == F("ELSC");
  const bool vfx = part || type == F("SWHC") || electric; // swooshes and ELC2s take the VFX material too
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
    } else if ((vfx && (fourcc == F("VMAT") || fourcc == F("VPRM"))) || (part && fourcc == F("VMSH"))) {
      ok = reader.Material();
    } else if (vfx && fourcc == F("VTMT")) {
      ok = reader.TextureTransforms();
    } else if (vfx && (fourcc == F("VPMT") || fourcc == F("VSMT"))) {
      ok = reader.PerParticle(fourcc == F("VSMT"));
    } else if (part && (fourcc == F("VTRS") || fourcc == F("VTRT"))) {
      ok = true;
      for (int i = 0; i < 5 && ok; ++i) {
        ok = reader.Element(Type::Real);
      }
    } else if (part && fourcc == F("VATX")) {
      ok = reader.Skip(8 * 4);  // eight words
    } else if (part && fourcc == F("VAT2")) {
      ok = reader.Skip(5 * 4) && reader.Element(Type::Int);  // five words, the cycle
    } else if (part && fourcc == F("VATF")) {
      ok = reader.Skip(2 * 4) && reader.Element(Type::Int);  // layers, loop, the duration
    } else if (part && fourcc == F("VTP2")) {
      ok = reader.Skip(2 * 4) && reader.Element(Type::Real);  // columns, rows, the index
    } else if (part && fourcc == F("VTFB")) {
      ok = reader.Skip(4) && reader.Element(Type::Real);  // layers, the index
    } else if ((part || electric) && (fourcc == F("PVSC") || fourcc == F("PVMC") || fourcc == F("PVTC"))) {
      ok = reader.Element(Type::Color);
    } else if (electric && fourcc == F("PVBS")) {
      ok = reader.Element(Type::Real);
    } else if (electric && (fourcc == F("PVSD") || fourcc == F("PVSN") || fourcc == F("PVSM") || fourcc == F("PVDT"))) {
      ok = reader.PortWord();
    } else if ((part && (fourcc == F("SSZE") || fourcc == F("PIR0") || fourcc == F("POR0") ||
                         fourcc == F("PVBS") || fourcc == F("PITN") || fourcc == F("PSBS") ||
                         fourcc == F("PSLD"))) ||
               (vfx && (fourcc == F("ITEN") || fourcc == F("PDBS")))) {
      ok = reader.Element(Type::Real);
    } else if (part && (fourcc == F("PINS") || fourcc == F("PSWS") || fourcc == F("PINR") || fourcc == F("PINT") || fourcc == F("PMOV") ||
                        fourcc == F("PMPV") || fourcc == F("POVC") || fourcc == F("PSYT") || fourcc == F("PTMT"))) {
      ok = reader.Element(Type::Vector);
    } else if ((part && (fourcc == F("PSHT") || fourcc == F("PSMX"))) || (type == F("SWHC") && fourcc == F("PSHT"))) {
      ok = reader.Element(Type::Int);
    } else if (part && fourcc == F("PSPS")) {
      ok = reader.Asset();
    } else if ((part && (fourcc == F("VORN") || fourcc == F("XFMD") ||
                         fourcc == F("PPRM") || fourcc == F("PSHP") || fourcc == F("PHDR") || fourcc == F("PHFO") || fourcc == F("PLIX") || fourcc == F("PLSP") || fourcc == F("PLIA") || fourcc == F("PLOA") || fourcc == F("PLON") || fourcc == F("PKIL") || fourcc == F("PFXC") || fourcc == F("PGID") || fourcc == F("PBDM") ||
                         fourcc == F("PDFG") || fourcc == F("PZTS") || fourcc == F("PIVS") || fourcc == F("PWST") ||
                         fourcc == F("PITP") || fourcc == F("PDBO") || fourcc == F("PSHM") || fourcc == F("PISD") || fourcc == F("POSD") || fourcc == F("PBLT") || fourcc == F("PBOR") ||
                         fourcc == F("VMPC") || fourcc == F("DVVN") || fourcc == F("IEXP") ||
                         (fourcc >= F("VGD1") && fourcc <= F("VGD4")) ||
                         fourcc == F("PMRF") || fourcc == F("PFRM") || fourcc == F("PVSD") || fourcc == F("PVSN") ||
                         fourcc == F("PVSM") || fourcc == F("PVDT"))) ||
               (vfx && fourcc == F("PIRN")) ||
               (type == F("SWHC") && (fourcc == F("PGID") || fourcc == F("PSED") || fourcc == F("PTXU") ||
                                        fourcc == F("PLWQ") || fourcc == F("PSRD") || fourcc == F("PSRA")))) {
      ok = reader.PortWord();
    } else if (vfx && fourcc == F("PVRT")) {
      ok = reader.PortVariables();
    } else if ((part || type == F("SWHC")) && fourcc == F("PSYS")) {
      ok = reader.PortSystemVars();
    } else if (part && (fourcc == F("PADO") || fourcc == F("PFXB"))) {
      ok = reader.PortWords();
    } else if ((part || type == F("SWHC")) && fourcc == F("PPVR")) {
      ok = reader.PortPartVars(type == F("SWHC"));
    } else if (part && fourcc == F("PSWX")) {
      ok = reader.PortExtraSwooshes();
    } else if (part && fourcc == F("PKSM")) {
      ok = reader.PortSpawnTables();
    } else if (part && (fourcc == F("PSMV") || fourcc == F("PEMR"))) {
      ok = reader.PortMover();
    } else if (!part && vfx && (fourcc == F("PSRO") || fourcc == F("PRAF") || fourcc == F("XFMD") || fourcc == F("PDFG") ||
                          fourcc == F("PZTS") || fourcc == F("PSHM"))) {
      ok = reader.PortWord();
    } else if (type == F("SWHC") && (fourcc == F("DVVN") || fourcc == F("IEXP"))) {
      ok = reader.PortWord();
    } else if (part && fourcc == F("PIEV")) {
      ok = reader.PortWord();
    } else if (part && fourcc == F("PISV")) {
      ok = reader.PortWord() && reader.Element(Type::Real);
    } else if (!part && vfx && fourcc == F("PRAZ")) {
      ok = reader.Element(Type::Vector);
    } else if (type == F("SWHC") && fourcc == F("PSIR")) {
      ok = reader.PortWord() && reader.Element(Type::Vector);
    } else if (type == F("SWHC") && fourcc == F("PSSC")) {
      ok = reader.Element(Type::Vector);
    } else if (!part && vfx && fourcc == F("PROZ")) {
      ok = reader.PortWord() && reader.Element(Type::Real);
    } else if (electric && fourcc == F("PEL2")) {
      ok = reader.PortWord();
    } else if (electric && Elc2Properties().count(fourcc) != 0) {
      const Type elcType = Elc2Properties().at(fourcc).type;
      ok = elcType == Type::Bool ? reader.Bool() : reader.Element(elcType);
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
  return port::ReadBE32(id.data() + 12);
}

std::vector<ConvertedPart> ConvertEffect(const EffectNode& effect, const uint8_t* data, const EffectConvertIO& io) {
  std::vector<ConvertedPart> out;
  Writer(data, io).Collect(effect, true, out);
  return out;
}

}  // namespace PortRemastered
