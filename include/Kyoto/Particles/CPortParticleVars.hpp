#ifndef _CPORTPARTICLEVARS
#define _CPORTPARTICLEVARS

#ifdef TARGET_PC

#include "types.h"

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

// Port-only Remastered particle variables (PVAR). A converted generator or swoosh description
// carries the table (PVRT); each CElementGen/CParticleSwoosh holds the live values, which start at
// the defaults. VARF/VARI/VARV/VARC elements read the slot of the system being updated or rendered.

struct PortGuid {
  std::array< u8, 16 > bytes{};
  bool operator==(const PortGuid& o) const { return bytes == o.bytes; }
  bool operator!=(const PortGuid& o) const { return bytes != o.bytes; }
};

enum class EPortVarType : u32 { Real = 0, Int = 1, Color = 2, Vector = 3, Rotation = 4 };

// A bind handle: the variable's index in the description's table, or kPortNoVar.
constexpr u16 kPortNoVar = 0xffff;

struct SPortVar {
  PortGuid guid;
  EPortVarType type = EPortVarType::Real;
  float def[4] = {};
};

// A value an owner sets on a system's variable by guid, whatever its type
// (SLdrEffectExternalVariableDefaultInfo, 0xaa8860: one record carries a value of each type,
// and the variable's type in the system picks one).
struct SPortExternalVar {
  PortGuid guid;
  float color[4] = {1.f, 1.f, 1.f, 1.f};
  float vec[3] = {0.f, 0.f, 0.f};
  float real = 0.f;
  s32 integer = 0;
};

class CPortVarTable {
public:
  std::vector< SPortVar > vars;

  u16 Find(const PortGuid& guid, EPortVarType type) const {
    for (size_t i = 0; i < vars.size(); ++i) {
      if (vars[i].type == type && vars[i].guid == guid) {
        return static_cast< u16 >(i);
      }
    }
    return kPortNoVar;
  }
};

// A generator's system variables (CAddVarSystem; the port-only PSYS): SVRn/SVIn/SVCn/SVVn and
// the hidden init-only reals their KPIN/IRND become, in evaluation order. Slots below 10 are
// the variables PSAn and a child's SPA* read.
struct SPortSysItem {
  EPortVarType type = EPortVarType::Real;
  u32 slot = 0;
  bool initOnly = false;
  CRealElement* real = nullptr;
  CIntElement* intE = nullptr;
  CColorElement* color = nullptr;
  CVectorElement* vec = nullptr;
};

class CPortSys {
public:
  std::vector< SPortSysItem > items;
  u32 counts[4] = {};   // slots a type, hidden ones included
  u32 defined[4] = {};  // the variables 0-9 the description declares, a bit each

  CPortSys() = default;
  CPortSys(const CPortSys&) = delete;
  CPortSys& operator=(const CPortSys&) = delete;
  ~CPortSys() {
    for (size_t i = 0; i < items.size(); ++i) {
      delete items[i].real;
      delete items[i].intE;
      delete items[i].color;
      delete items[i].vec;
    }
  }
};

// Remastered's particle variables (GPSM ADV1..9 and IAV0..3), in its SortVars order: one is
// evaluated after those it reads. At spawn every variable runs; on later frames only those that are
// not constant after start (CVarContext IsConstantAfterStart), so the one RNG sequence is the same
// as Remastered's. A kind 0 item runs the generator's own ADVn element. Kinds 3 and 4 are VAV0..3
// and CAV0..3 (CreateGPSM/CreateSWSH AddDefinedVar): no shipped element reads them, so their
// values are not kept, but they run in their place, which keeps the RNG sequence (a VAV0 of
// RAND draws).
enum class EPortPartVarKind : u32 { RealAdv = 0, RealOwn = 1, Int = 2, Vector = 3, Color = 4 };

struct SPortPartVarItem {
  EPortPartVarKind kind = EPortPartVarKind::RealAdv;
  u32 slot = 0;
  bool constAfterStart = false;
  CRealElement* real = nullptr;    // kind 1: ADV9
  CIntElement* intE = nullptr;     // kind 2: IAV<slot>
  CVectorElement* vec = nullptr;   // kind 3: VAV<slot>
  CColorElement* color = nullptr;  // kind 4: CAV<slot>
};

class CPortPartVars {
public:
  std::vector< SPortPartVarItem > items;

  CPortPartVars() = default;
  CPortPartVars(const CPortPartVars&) = delete;
  CPortPartVars& operator=(const CPortPartVars&) = delete;
  ~CPortPartVars() {
    for (size_t i = 0; i < items.size(); ++i) {
      delete items[i].real;
      delete items[i].intE;
      delete items[i].vec;
      delete items[i].color;
    }
  }

  // values: ADV1..9 at 0..8, then the bits of IAV0..3 (CAdvancedValues). advs: the generator's
  // ADV1..8 elements.
  void Eval(CRealElement* const advs[8], bool spawn, int frame, float* values) const {
    for (const SPortPartVarItem& item : items) {
      if (!spawn && item.constAfterStart) {
        continue;
      }
      switch (item.kind) {
      case EPortPartVarKind::RealAdv:
        if (advs[item.slot] != nullptr) {
          advs[item.slot]->GetValue(frame, values[item.slot]);
        }
        break;
      case EPortPartVarKind::RealOwn:
        item.real->GetValue(frame, values[item.slot]);
        break;
      case EPortPartVarKind::Int: {
        int v = 0;
        item.intE->GetValue(frame, v);
        std::memcpy(&values[9 + item.slot], &v, sizeof(v));
        break;
      }
      case EPortPartVarKind::Vector: {
        CVector3f v;
        item.vec->GetValue(frame, v);
        break;
      }
      case EPortPartVarKind::Color: {
        CColor c;
        item.color->GetValue(frame, c);
        break;
      }
      }
    }
  }
};

// The live values of one system. Without a table nothing is stored and every bind is a no-op.
class CPortVarMemory {
public:
  // The system variables start at zero.
  void InitSys(const CPortSys* sys) {
    x_sysDesc = sys;
    for (int t = 0; t < 4; ++t) {
      x_sys[t].assign(sys != nullptr ? sys->counts[t] : 0, std::array< float, 4 >{});
    }
  }
  void SetSys(EPortVarType type, u32 slot, const std::array< float, 4 >& v) {
    const u32 t = static_cast< u32 >(type);
    if (t < 4 && slot < x_sys[t].size()) {
      x_sys[t][slot] = v;
    }
  }
  // Variable n as a child's SPA* resolves it against this system's description, or null.
  const std::array< float, 4 >* SysVar(EPortVarType type, u32 n) const {
    const u32 t = static_cast< u32 >(type);
    if (x_sysDesc == nullptr || t >= 4 || n >= 10 || (x_sysDesc->defined[t] >> n & 1) == 0 ||
        n >= x_sys[t].size()) {
      return nullptr;
    }
    return &x_sys[t][n];
  }
  float SysReal(u32 slot) const { return slot < x_sys[0].size() ? x_sys[0][slot][0] : 0.f; }
  int SysInt(u32 slot) const {
    return slot < x_sys[1].size() ? static_cast< int >(x_sys[1][slot][0]) : 0;
  }
  CVector3f SysVector(u32 slot) const {
    return slot < x_sys[3].size() ? CVector3f(x_sys[3][slot][0], x_sys[3][slot][1], x_sys[3][slot][2])
                                  : CVector3f::Zero();
  }
  // The system that built this one (CVarContext 0x24f06c: ctx+0x48 = the parent's), or null.
  void SetParent(const CPortVarMemory* parent) { x_parent = parent != this ? parent : nullptr; }
  const CPortVarMemory* Parent() const { return x_parent; }
  // PVAR value h, or null.
  const std::array< float, 4 >* Value(u16 h, EPortVarType type) const {
    return Valid(h, type) ? &x_values[h] : nullptr;
  }

  void Init(const CPortVarTable* table) {
    x_table = table;
    x_values.clear();
    if (table != nullptr) {
      x_values.resize(table->vars.size());
      for (size_t i = 0; i < table->vars.size(); ++i) {
        std::memcpy(x_values[i].data(), table->vars[i].def, sizeof(float) * 4);
      }
    }
  }
  bool Empty() const { return x_values.empty(); }
  u16 Find(const PortGuid& guid, EPortVarType type) const {
    return x_table != nullptr ? x_table->Find(guid, type) : kPortNoVar;
  }
  bool Valid(u16 h, EPortVarType type) const {
    return h < x_values.size() && x_table->vars[h].type == type;
  }

  void BindReal(u16 h, float v) {
    if (Valid(h, EPortVarType::Real)) {
      x_values[h][0] = v;
    }
  }
  void BindInt(u16 h, int v) {
    if (Valid(h, EPortVarType::Int)) {
      x_values[h][0] = static_cast< float >(v);
    }
  }
  void BindColor(u16 h, const CColor& c) {
    if (Valid(h, EPortVarType::Color)) {
      x_values[h] = {c.GetRed(), c.GetGreen(), c.GetBlue(), c.GetAlpha()};
    }
  }
  void BindVector(u16 h, const CVector3f& v) {
    if (Valid(h, EPortVarType::Vector)) {
      x_values[h] = {v.GetX(), v.GetY(), v.GetZ(), 0.f};
    }
  }
  // Every variable of that guid takes the record's value of its type.
  void BindExternal(const SPortExternalVar& e) {
    if (x_table == nullptr) {
      return;
    }
    for (size_t i = 0; i < x_values.size(); ++i) {
      if (x_table->vars[i].guid != e.guid) {
        continue;
      }
      switch (x_table->vars[i].type) {
      case EPortVarType::Real:
        x_values[i][0] = e.real;
        break;
      case EPortVarType::Int:
        x_values[i][0] = static_cast< float >(e.integer);
        break;
      case EPortVarType::Color:
        x_values[i] = {e.color[0], e.color[1], e.color[2], e.color[3]};
        break;
      case EPortVarType::Vector:
        x_values[i] = {e.vec[0], e.vec[1], e.vec[2], 0.f};
        break;
      default:
        break;
      }
    }
  }

  // Index-based reads for the elements; out of range reads zero.
  float Real(u32 i) const { return i < x_values.size() ? x_values[i][0] : 0.f; }
  int Int(u32 i) const { return i < x_values.size() ? static_cast< int >(x_values[i][0]) : 0; }
  CColor Color(u32 i) const {
    if (i >= x_values.size()) {
      return CColor(0.f, 0.f, 0.f, 0.f);
    }
    const auto& v = x_values[i];
    return CColor(v[0], v[1], v[2], v[3]);
  }
  // The stored floats, unclamped (null out of range).
  const std::array< float, 4 >* ColorHdr(u32 i) const {
    return i < x_values.size() ? &x_values[i] : nullptr;
  }
  CVector3f Vector(u32 i) const {
    if (i >= x_values.size()) {
      return CVector3f::Zero();
    }
    const auto& v = x_values[i];
    return CVector3f(v[0], v[1], v[2]);
  }

private:
  const CPortVarTable* x_table = nullptr;
  std::vector< std::array< float, 4 > > x_values;
  const CPortSys* x_sysDesc = nullptr;
  std::vector< std::array< float, 4 > > x_sys[4];
  const CPortVarMemory* x_parent = nullptr;
};

// The memory of the system being updated/rendered (null outside one), so a child generator reads
// its own variables. Set with CPortVarScope, which restores the previous one.
struct CPortVarGlobals {
  static const CPortVarMemory* sCurrent;
};

class CPortVarScope {
  const CPortVarMemory* x_prev;

public:
  explicit CPortVarScope(const CPortVarMemory* mem) : x_prev(CPortVarGlobals::sCurrent) {
    CPortVarGlobals::sCurrent = mem;
  }
  ~CPortVarScope() { CPortVarGlobals::sCurrent = x_prev; }
  CPortVarScope(const CPortVarScope&) = delete;
  CPortVarScope& operator=(const CPortVarScope&) = delete;
};

// VARF / VARI / VARV / VARC: the current system's variable at a table index.
class CREPortVar : public CRealElement {
  u32 x4_index;

public:
  explicit CREPortVar(u32 index) : x4_index(index) {}
  ~CREPortVar() override {}
  bool GetValue(int, float& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Real(x4_index) : 0.f;
    return false;
  }
};

class CIEPortVar : public CIntElement {
  u32 x4_index;

public:
  explicit CIEPortVar(u32 index) : x4_index(index) {}
  ~CIEPortVar() override {}
  bool GetValue(int, int& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Int(x4_index) : 0;
    return false;
  }
};

class CVEPortVar : public CVectorElement {
  u32 x4_index;

public:
  explicit CVEPortVar(u32 index) : x4_index(index) {}
  ~CVEPortVar() override {}
  bool GetValue(int, CVector3f& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Vector(x4_index)
                                               : CVector3f::Zero();
    return false;
  }
};

class CCEPortVar : public CColorElement {
  u32 x4_index;

public:
  explicit CCEPortVar(u32 index) : x4_index(index) {}
  ~CCEPortVar() override {}
  bool GetValue(int, CColor& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Color(x4_index)
                                               : CColor(0.f, 0.f, 0.f, 0.f);
    return false;
  }
  bool GetValueHdr(int, float out[4]) const override {
    const std::array< float, 4 >* v = CPortVarGlobals::sCurrent != nullptr
                                          ? CPortVarGlobals::sCurrent->ColorHdr(x4_index)
                                          : nullptr;
    for (int i = 0; i < 4; ++i) {
      out[i] = v != nullptr ? (*v)[i] : 0.f;
    }
    return false;
  }
};

// PSYR (port-only; PSAn and the hidden KPIN/IRND reals): the current system's real slot.
class CREPortSysVar : public CRealElement {
  u32 x4_slot;

public:
  explicit CREPortSysVar(u32 slot) : x4_slot(slot) {}
  ~CREPortSysVar() override {}
  bool GetValue(int, float& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->SysReal(x4_slot) : 0.f;
    return false;
  }
};

// PSYV (port-only; a vector PSAn, GetVectorElement 0x251a44): the current system's vector slot.
class CVEPortSysVar : public CVectorElement {
  u32 x4_slot;

public:
  explicit CVEPortSysVar(u32 slot) : x4_slot(slot) {}
  ~CVEPortSysVar() override {}
  bool GetValue(int, CVector3f& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->SysVector(x4_slot)
                                               : CVector3f::Zero();
    return false;
  }
};

// PSDT (port-only; Remastered's SDTR, CREShutdownTimeRemaining 0x2999e0): the current system's
// shutdown countdown remaining over its total (SHTT), 1 when none is running.
class CREPortShutdownRemaining : public CRealElement {
public:
  CREPortShutdownRemaining() {}
  ~CREPortShutdownRemaining() override {}
  bool GetValue(int frame, float& out) const override;
};

// PVSQ (port-only; Remastered's VISQ, CREVisibilityQuery 0x299790): the current system's
// occlusion visibility (samples passing the depth test over samples drawn), 1 with none yet.
class CREPortVisibility : public CRealElement {
public:
  CREPortVisibility() {}
  ~CREPortVisibility() override {}
  bool GetValue(int frame, float& out) const override;
};

// PDML (port-only; one component of Remastered's DOML vector, CVEDominantLightDirectionInWorldSpace
// 0x2a40e8): the dominant light direction LightHints set (PortRoomEnv::DominantLight), world space.
class CREPortDominantLight : public CRealElement {
  u32 x4_component;

public:
  explicit CREPortDominantLight(u32 component) : x4_component(component) {}
  ~CREPortDominantLight() override {}
  bool GetValue(int frame, float& out) const override;
};

// PSYI (port-only; a hidden init-only int, such as a PEOD IRND): the current system's int slot.
class CIEPortSysVar : public CIntElement {
  u32 x4_slot;

public:
  explicit CIEPortSysVar(u32 slot) : x4_slot(slot) {}
  ~CIEPortSysVar() override {}
  bool GetValue(int, int& out) const override {
    out = CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->SysInt(x4_slot) : 0;
    return false;
  }
};

// A Remastered KPIN outside a system variable becomes a hidden per-particle variable
// (CComplexKeepInitial::RebaseComplexOp 0x2d8fac): evaluated once when the particle or swoosh
// point is born (_Init_Particle 0x2784b8, which CreateNewParticles 0x24a4cc runs before EMTR and
// the properties) and read unchanged after. These are the latched values of one particle or point.
struct SPortKeep {
  static constexpr u32 kMax = 8;
  float values[kMax];
  u32 latched; // a bit per slot
};

class CREPortKeep;

struct CPortKeepGlobals {
  // The particle or swoosh point being evaluated, or null (a KPIN then reads its child).
  static SPortKeep* sCurrent;
  // While a description is read: where its PKPR elements are listed.
  static std::vector< const CREPortKeep* >* sCollect;
};

// Sets the latches being evaluated for its lifetime.
struct CPortKeepScope {
  SPortKeep* prev;
  explicit CPortKeepScope(SPortKeep* keep) : prev(CPortKeepGlobals::sCurrent) {
    CPortKeepGlobals::sCurrent = keep;
  }
  ~CPortKeepScope() { CPortKeepGlobals::sCurrent = prev; }
};

// PKPR (port-only): slot, then the KPIN's element. The first read for a particle or point latches
// the element's value; the description latches every slot at birth (Latch), in reading order.
class CREPortKeep : public CRealElement {
  u32 x4_slot;
  CRealElement* x8_value;

public:
  CREPortKeep(u32 slot, CRealElement* value) : x4_slot(slot), x8_value(value) {
    if (CPortKeepGlobals::sCollect != nullptr) {
      CPortKeepGlobals::sCollect->push_back(this);
    }
  }
  ~CREPortKeep() override {
    // A parse that fails after this element was built deletes it while still collecting.
    if (std::vector< const CREPortKeep* >* list = CPortKeepGlobals::sCollect) {
      list->erase(std::remove(list->begin(), list->end(), this), list->end());
    }
    delete x8_value;
  }
  bool GetValue(int frame, float& out) const override {
    SPortKeep* keep = CPortKeepGlobals::sCurrent;
    if (keep == nullptr) {
      return x8_value->GetValue(frame, out);
    }
    const u32 bit = 1u << x4_slot;
    if ((keep->latched & bit) == 0) {
      x8_value->GetValue(frame, keep->values[x4_slot]);
      keep->latched |= bit;
    }
    out = keep->values[x4_slot];
    return false;
  }
  void Latch(int frame) const {
    float value;
    GetValue(frame, value);
  }
};

// ABS_ (Remastered CREAbsoluteValue GetValue 0x295094, CIEAbsoluteValue 0x2a86b0,
// CVEAbsoluteValue 0x29e254): the child evaluated into a zeroed local, then its absolute value.
// The int one negates unsigned, so INT_MIN stays INT_MIN.
class CREPortAbs : public CRealElement {
  CRealElement* x4_value;

public:
  explicit CREPortAbs(CRealElement* value) : x4_value(value) {}
  ~CREPortAbs() override { delete x4_value; }
  bool GetValue(int frame, float& out) const override {
    float local = 0.f;
    x4_value->GetValue(frame, local);
    out = std::fabs(local);
    return false;
  }
};

class CIEPortAbs : public CIntElement {
  CIntElement* x4_value;

public:
  explicit CIEPortAbs(CIntElement* value) : x4_value(value) {}
  ~CIEPortAbs() override { delete x4_value; }
  bool GetValue(int frame, int& out) const override {
    int local = 0;
    x4_value->GetValue(frame, local);
    out = local >= 0 ? local : static_cast< int >(0u - static_cast< u32 >(local));
    return false;
  }
};

class CVEPortAbs : public CVectorElement {
  CVectorElement* x4_value;

public:
  explicit CVEPortAbs(CVectorElement* value) : x4_value(value) {}
  ~CVEPortAbs() override { delete x4_value; }
  bool GetValue(int frame, CVector3f& out) const override {
    CVector3f local(0.f, 0.f, 0.f);
    x4_value->GetValue(frame, local);
    out = CVector3f(std::fabs(local.GetX()), std::fabs(local.GetY()), std::fabs(local.GetZ()));
    return false;
  }
};

// COSE (CRECosineWave::GetValue 0x298788), in file order frequency, amplitude, phase:
// amplitude * FastCosR((phase + frequency * frame) * pi/180). Phase is evaluated first, then
// frequency, then amplitude; each local starts at 0.
class CREPortCosineWave : public CRealElement {
  CRealElement* x4_frequency;
  CRealElement* x8_amplitude;
  CRealElement* xc_phase;

public:
  CREPortCosineWave(CRealElement* frequency, CRealElement* amplitude, CRealElement* phase)
  : x4_frequency(frequency), x8_amplitude(amplitude), xc_phase(phase) {}
  ~CREPortCosineWave() override {
    delete x4_frequency;
    delete x8_amplitude;
    delete xc_phase;
  }
  bool GetValue(int frame, float& out) const override {
    float phase = 0.f, frequency = 0.f, amplitude = 0.f;
    xc_phase->GetValue(frame, phase);
    x4_frequency->GetValue(frame, frequency);
    x8_amplitude->GetValue(frame, amplitude);
    out = amplitude * PortParticleMath::FastCosR(
                          (phase + frequency * static_cast< float >(frame)) *
                          PortParticleMath::BitsFloat(0x3c8efa35));
    return false;
  }
};

// The sign of a random draw in Remastered's generator: the seed bit 0x10000 is bit 0 of
// CRandom16::Next (the draw is the seed's upper half). Set is positive.
inline bool PortRandomSignPositive() {
  return (CRandom16::GetRandomNumber()->Next() & 1) != 0;
}

// RSGN (CRERandomSign::GetValue 0x299318): the child, negated when the draw's bit is clear. A draw
// on every evaluation.
class CREPortRandomSign : public CRealElement {
  CRealElement* x4_value;

public:
  explicit CREPortRandomSign(CRealElement* value) : x4_value(value) {}
  ~CREPortRandomSign() override { delete x4_value; }
  bool GetValue(int frame, float& out) const override {
    x4_value->GetValue(frame, out);
    if (!PortRandomSignPositive()) {
      out = -out;
    }
    return false;
  }
};

// The external variables of the generator being updated (CElementGen::x9c_externalVars), or null
// while none was ever set: Remastered's effects spawned without a character have no external data
// block (gen+0x48 == 0). Defined by CElementGen.cpp.
const float* PortCurrentExternalVars();

// PMXV (port-only MPXV, MPT_CREExternalValue::GetValue 0x29715c): the index, then the fallback.
// With an external block: i = max(index, 0) % 12, out = vars[i], the fallback is not evaluated.
// Without one: the fallback's value.
class CREPortExternalVar : public CRealElement {
  CIntElement* x4_index;
  CRealElement* x8_fallback;

public:
  CREPortExternalVar(CIntElement* index, CRealElement* fallback) : x4_index(index), x8_fallback(fallback) {}
  ~CREPortExternalVar() override {
    delete x4_index;
    delete x8_fallback;
  }
  bool GetValue(int frame, float& out) const override {
    const float* vars = PortCurrentExternalVars();
    if (vars == nullptr) {
      return x8_fallback->GetValue(frame, out);
    }
    int i = 0;
    x4_index->GetValue(frame, i);
    i = i > 0 ? i : 0;
    out = vars[static_cast< u32 >(i) % 12];
    return false;
  }
};

// PIRS (port-only IRSN, CREInitialRandomSign::GetValue 0x299678): slot, then the real. At frame 0
// one draw sets the particle's sign (+1 for a set bit, else -1), kept in its SPortKeep slot (PKPR's
// counter; not listed in sCollect, as it draws only at frame 0); the result is that sign times the
// child. A slot never stored reads 0 (Remastered's variable memory starts zeroed). Outside a
// particle or point, or with slot kNoSlot, the sign is the element's own.
class CREPortInitialRandomSign : public CRealElement {
  u32 x4_slot;
  CRealElement* x8_value;
  mutable float xc_sign = 0.f;

public:
  static constexpr u32 kNoSlot = 0xffffffffu;
  CREPortInitialRandomSign(u32 slot, CRealElement* value) : x4_slot(slot), x8_value(value) {}
  ~CREPortInitialRandomSign() override { delete x8_value; }
  bool GetValue(int frame, float& out) const override {
    SPortKeep* keep = CPortKeepGlobals::sCurrent;
    float* sign = keep != nullptr && x4_slot < SPortKeep::kMax ? &keep->values[x4_slot] : &xc_sign;
    if (frame == 0) {
      *sign = PortRandomSignPositive() ? 1.f : -1.f;
    }
    float local = 0.f;
    x8_value->GetValue(frame, local);
    out = *sign * local;
    return false;
  }
};

// Where a parent read finds its value, or null for the fallback.
// SPA*: CElementParentAddlVar, the parent's system variable n, resolved against the parent's
// description (CREParentAdditionalValue::GetValue 0x296824). DPV*: the direct parent's PVAR
// variable (AddDirectParentExtVarRef 0x25584c). TPV*: the topmost parent's
// (AddTopmostParentExtVarRef 0x255bf4). [I] gen flag 0xa0 bit 7, which also gates the
// fallback, is not modelled; nor is a CParticleElectric parent (its children see the
// generator above it).
enum class EPortParentRead : u8 { System, Direct, Topmost };

struct SPortParentRead {
  EPortParentRead kind = EPortParentRead::System;
  u32 n = 0;
  PortGuid guid;

  const std::array< float, 4 >* Find(EPortVarType type) const {
    const CPortVarMemory* parent =
        CPortVarGlobals::sCurrent != nullptr ? CPortVarGlobals::sCurrent->Parent() : nullptr;
    if (parent == nullptr) {
      return nullptr;
    }
    if (kind == EPortParentRead::System) {
      return parent->SysVar(type, n);
    }
    if (kind == EPortParentRead::Topmost) {
      while (parent->Parent() != nullptr) {
        parent = parent->Parent();
      }
    }
    return parent->Value(parent->Find(guid, type), type);
  }
};

class CREPortParentVar : public CRealElement {
  SPortParentRead x4_read;
  CRealElement* x_fallback;

public:
  CREPortParentVar(const SPortParentRead& read, CRealElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CREPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, float& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Real)) {
      out = (*v)[0];
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
};

class CIEPortParentVar : public CIntElement {
  SPortParentRead x4_read;
  CIntElement* x_fallback;

public:
  CIEPortParentVar(const SPortParentRead& read, CIntElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CIEPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, int& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Int)) {
      out = static_cast< int >((*v)[0]);
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
};

class CVEPortParentVar : public CVectorElement {
  SPortParentRead x4_read;
  CVectorElement* x_fallback;

public:
  CVEPortParentVar(const SPortParentRead& read, CVectorElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CVEPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, CVector3f& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Vector)) {
      out = CVector3f((*v)[0], (*v)[1], (*v)[2]);
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
};

class CCEPortParentVar : public CColorElement {
  SPortParentRead x4_read;
  CColorElement* x_fallback;

public:
  CCEPortParentVar(const SPortParentRead& read, CColorElement* fallback)
  : x4_read(read), x_fallback(fallback) {}
  ~CCEPortParentVar() override { delete x_fallback; }
  bool GetValue(int frame, CColor& out) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Color)) {
      auto clamp = [](float f) { return f < 0.f ? 0.f : f > 1.f ? 1.f : f; };
      out = CColor(clamp((*v)[0]), clamp((*v)[1]), clamp((*v)[2]), clamp((*v)[3]));
      return false;
    }
    return x_fallback->GetValue(frame, out);
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    if (const std::array< float, 4 >* v = x4_read.Find(EPortVarType::Color)) {
      for (int i = 0; i < 4; ++i) {
        out[i] = (*v)[i];
      }
      return false;
    }
    return x_fallback->GetValueHdr(frame, out);
  }
};

// Remastered colour combinators (GetColorElement 0x25ece4). Remastered keeps half floats with no
// clamp; CColor is bytes, so results are clamped to [0, 1].
// MULT: CCEMultiply 0x2c7704, a * b per channel (CColor4f::Modulate).
class CCEPortMultiply : public CColorElement {
  CColorElement* x4_a;
  CColorElement* x8_b;

public:
  CCEPortMultiply(CColorElement* a, CColorElement* b) : x4_a(a), x8_b(b) {}
  ~CCEPortMultiply() override {
    delete x4_a;
    delete x8_b;
  }
  bool GetValue(int frame, CColor& out) const override {
    CColor a, b;
    x4_a->GetValue(frame, a);
    x8_b->GetValue(frame, b);
    out = CColor(a.GetRed() * b.GetRed(), a.GetGreen() * b.GetGreen(), a.GetBlue() * b.GetBlue(),
                 a.GetAlpha() * b.GetAlpha());
    return false;
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    float a[4], b[4];
    x4_a->GetValueHdr(frame, a);
    x8_b->GetValueHdr(frame, b);
    for (int i = 0; i < 4; ++i) {
      out[i] = a[i] * b[i];
    }
    return false;
  }
};

// MDAO: CCEModifyAlphaOnly 0x2c8678, the colour's RGB with alpha max(real, 0).
class CCEPortModifyAlpha : public CColorElement {
  CColorElement* x4_color;
  CRealElement* x8_alpha;

public:
  CCEPortModifyAlpha(CColorElement* color, CRealElement* alpha) : x4_color(color), x8_alpha(alpha) {}
  ~CCEPortModifyAlpha() override {
    delete x4_color;
    delete x8_alpha;
  }
  bool GetValue(int frame, CColor& out) const override {
    x4_color->GetValue(frame, out);
    float alpha = 1.f;
    if (x8_alpha != nullptr) {
      x8_alpha->GetValue(frame, alpha);
    }
    out.SetAlpha(alpha < 0.f ? 0.f : alpha > 1.f ? 1.f : alpha);
    return false;
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    x4_color->GetValueHdr(frame, out);
    float alpha = 1.f;
    if (x8_alpha != nullptr) {
      x8_alpha->GetValue(frame, alpha);
    }
    out[3] = alpha < 0.f ? 0.f : alpha;
    return false;
  }
};

// SLCT: CElementArray<T>::GetValue (float 0x27121c, CVector3f 0x27186c, CColor4f16 0x271ee4),
// the entry at index % count (C remainder), the index evaluated on every read. IRND indices
// are latched per particle (CIEInitialRandom). Remastered does not wrap a negative index; the
// port does rather than read outside the array.
template < class Element >
class TPortSelect : public Element {
  CIntElement* x4_index;
  std::vector< Element* > x8_entries;

protected:
  const Element* Entry(int frame) const {
    int index = 0;
    x4_index->GetValue(frame, index);
    const int count = static_cast< int >(x8_entries.size());
    index %= count;
    return x8_entries[index < 0 ? index + count : index];
  }

public:
  TPortSelect(CIntElement* index, std::vector< Element* > entries)
  : x4_index(index), x8_entries(std::move(entries)) {}
  ~TPortSelect() override {
    delete x4_index;
    for (size_t i = 0; i < x8_entries.size(); ++i) {
      delete x8_entries[i];
    }
  }
};

class CREPortSelect : public TPortSelect< CRealElement > {
public:
  using TPortSelect::TPortSelect;
  bool GetValue(int frame, float& out) const override { return Entry(frame)->GetValue(frame, out); }
};

class CVEPortSelect : public TPortSelect< CVectorElement > {
public:
  using TPortSelect::TPortSelect;
  bool GetValue(int frame, CVector3f& out) const override { return Entry(frame)->GetValue(frame, out); }
};

class CCEPortSelect : public TPortSelect< CColorElement > {
public:
  using TPortSelect::TPortSelect;
  bool GetValue(int frame, CColor& out) const override { return Entry(frame)->GetValue(frame, out); }
  bool GetValueHdr(int frame, float out[4]) const override {
    return Entry(frame)->GetValueHdr(frame, out);
  }
};

class CInputStream;
// Reads a PVRT body (after the CNST class id): u32 count, then per variable 16 guid bytes, u32
// type and 4 float bit patterns. Null on a malformed table.
CPortVarTable* PortReadVarTable(CInputStream& in);
// Reads a PSYS body (after the CNST class id). Null on a malformed one.
CPortSys* PortReadSys(CInputStream& in);
// Reads a PPVR body (after the CNST class id): u32 count, then per item u32 kind, slot and
// constant-after-start flag, and for kinds 1..4 the element. A generator's kind 1 is ADV9 only; a
// swoosh (whose ADV0..8 are all its own, per point) has no kind 0. Null on a malformed one.
CPortPartVars* PortReadPartVars(CInputStream& in, bool swoosh);

#endif // TARGET_PC
#endif // _CPORTPARTICLEVARS
