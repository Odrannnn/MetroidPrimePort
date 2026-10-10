#ifndef _CPORTPARTICLEVARS
#define _CPORTPARTICLEVARS

#ifdef TARGET_PC

#include "types.h"

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/IElement.hpp"

#include <array>
#include <cstring>
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

// The live values of one system. Without a table nothing is stored and every bind is a no-op.
class CPortVarMemory {
public:
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
};

class CInputStream;
// Reads a PVRT body (after the CNST class id): u32 count, then per variable 16 guid bytes, u32
// type and 4 float bit patterns. Null on a malformed table.
CPortVarTable* PortReadVarTable(CInputStream& in);

#endif // TARGET_PC
#endif // _CPORTPARTICLEVARS
