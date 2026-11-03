// Remastered particle variables ADV9, IAV0..3 and VAV/CAV0..3 (CPortPartVars): spawn evaluates every item in
// order, later frames skip the constant-after-start ones, and an initial-random int draws the one
// RNG exactly once. Per-particle ints live as raw bits after ADV1..9 in CAdvancedValues.
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "port_test.h"

#include <cstdint>
#include <cstring>

// IElement's allocator lives with the effect system; plain heap here.
void* IElement::operator new(size_t sz, const char*, const char*) { return ::operator new(sz); }
void IElement::operator delete(void* ptr, size_t) { ::operator delete(ptr); }

namespace {

int ReadInt(const float* values, int n) {
  int v;
  std::memcpy(&v, &values[9 + n], sizeof(v));
  return v;
}

class CountReal final : public CRealElement {
public:
  CountReal(float v, int* calls) : mValue(v), mCalls(calls) {}
  bool GetValue(int frame, float& out) const override {
    ++*mCalls;
    out = mValue + float(frame);
    return false;
  }

private:
  float mValue;
  int* mCalls;
};

// IRND(min, max) as a CIEInitialRandomConstant would draw: one CRandom16::Range per call.
class DrawInt final : public CIntElement {
public:
  DrawInt(int lo, int hi, int* calls) : mLo(lo), mHi(hi), mCalls(calls) {}
  bool GetValue(int, int& out) const override {
    ++*mCalls;
    out = CRandom16::GetRandomNumber()->Range(mLo, mHi);
    return false;
  }

private:
  int mLo, mHi;
  int* mCalls;
};

void TestIntBits() {
  float values[13] = {};
  CParticleGlobals::SetParticleAccessInt(values, 2, -7);
  float* saved = CParticleGlobals::GetParticleAccessParameters();
  CParticleGlobals::mParticleAccessParameters = values;
  CHECK_MSG(CParticleGlobals::GetParticleAccessInt(2) == -7, "GetParticleAccessInt reads the stored bits");
  CParticleGlobals::mParticleAccessParameters = saved;
  CHECK_MSG(ReadInt(values, 2) == -7, "int round-trips");
}

void TestSpawnThenFrames() {
  CRandom16 rnd(1234);
  CGlobalRandom global(rnd);
  CRandom16 reference(1234);
  const int e0 = reference.Range(0, 100);

  int adv9Calls = 0, ivCalls = 0, ownCalls = 0;
  CPortPartVars vars;
  // ADV9 (not constant), IAV1 (initial-only random), IAV0 (constant).
  vars.items.push_back({EPortPartVarKind::RealOwn, 8, false, new CountReal(10.f, &adv9Calls), nullptr});
  vars.items.push_back({EPortPartVarKind::Int, 1, true, nullptr, new DrawInt(0, 100, &ivCalls)});
  vars.items.push_back({EPortPartVarKind::Int, 0, true, nullptr, new DrawInt(5, 5, &ownCalls)});
  CRealElement* advs[8] = {};

  float values[13] = {};
  vars.Eval(advs, true, 0, values);
  CHECK_MSG(values[8] == 10.f, "ADV9 at spawn");
  CHECK_MSG(ReadInt(values, 1) == e0, "IAV1 is the first RNG draw");
  CHECK_MSG(adv9Calls == 1 && ivCalls == 1 && ownCalls == 1, "every item evaluated at spawn");
  const uint32_t seedAfterSpawn = rnd.GetSeed();

  vars.Eval(advs, false, 3, values);
  CHECK_MSG(values[8] == 13.f, "ADV9 re-evaluated on later frames");
  CHECK_MSG(ReadInt(values, 1) == e0, "IAV1 keeps its spawn value");
  CHECK_MSG(adv9Calls == 2 && ivCalls == 1 && ownCalls == 1, "constant items are skipped after spawn");
  CHECK_MSG(rnd.GetSeed() == seedAfterSpawn, "no RNG draw after spawn");
}

// RAND x3 under KPIN, as a swoosh's VAV0: three draws at spawn, none later.
class DrawVector final : public CVectorElement {
public:
  explicit DrawVector(int* calls) : mCalls(calls) {}
  bool GetValue(int, CVector3f& out) const override {
    ++*mCalls;
    for (int i = 0; i < 3; ++i) {
      CRandom16::GetRandomNumber()->Range(0, 100);
    }
    out = CVector3f::Zero();
    return false;
  }

private:
  int* mCalls;
};

// A VAVn has no reader, but it runs in its place, so the draws after it are Remastered's.
void TestVectorKeepsRng() {
  CRandom16 rnd(99);
  CGlobalRandom global(rnd);
  CRandom16 reference(99);
  for (int i = 0; i < 3; ++i) {
    reference.Range(0, 100);
  }
  const int e3 = reference.Range(0, 100);
  int vecCalls = 0, ivCalls = 0;
  CPortPartVars vars;
  SPortPartVarItem vav{EPortPartVarKind::Vector, 0, true};
  vav.vec = new DrawVector(&vecCalls);
  vars.items.push_back(vav);
  vars.items.push_back({EPortPartVarKind::Int, 0, true, nullptr, new DrawInt(0, 100, &ivCalls)});
  CRealElement* advs[8] = {};
  float values[13] = {};
  vars.Eval(advs, true, 0, values);
  CHECK_MSG(ReadInt(values, 0) == e3 && vecCalls == 1, "VAV0's draws come before IAV0's");
  vars.Eval(advs, false, 1, values);
  CHECK_MSG(vecCalls == 1 && ivCalls == 1, "an initial-only VAV0 runs at spawn only");
}

void TestRealAdvSlot() {
  int calls = 0;
  CountReal adv3(2.f, &calls);
  CRealElement* advs[8] = {};
  advs[2] = &adv3;
  CPortPartVars vars;
  vars.items.push_back({EPortPartVarKind::RealAdv, 2, false, nullptr, nullptr});
  float values[13] = {};
  vars.Eval(advs, false, 1, values);
  CHECK_MSG(values[2] == 3.f && calls == 1, "ADV1..8 reference reads the generator element");
  vars.items.clear();
}

// An owner's value record: each variable of its guid takes the value of its own type.
void TestBindExternal() {
  CPortVarTable table;
  PortGuid g, other;
  g.bytes[0] = 1;
  other.bytes[0] = 2;
  table.vars.resize(4);
  table.vars[0] = {g, EPortVarType::Real, {9.f}};
  table.vars[1] = {g, EPortVarType::Color, {1.f, 1.f, 1.f, 1.f}};
  table.vars[2] = {other, EPortVarType::Real, {9.f}};
  table.vars[3] = {g, EPortVarType::Vector, {}};
  CPortVarMemory mem;
  mem.Init(&table);
  SPortExternalVar e;
  e.guid = g;
  e.real = 0.65f;
  e.color[0] = 0.59f;
  e.vec[2] = 0.5f;
  mem.BindExternal(e);
  CHECK((*mem.Value(0, EPortVarType::Real))[0] == 0.65f);
  CHECK((*mem.Value(1, EPortVarType::Color))[0] == 0.59f && (*mem.Value(1, EPortVarType::Color))[3] == 1.f);
  CHECK((*mem.Value(2, EPortVarType::Real))[0] == 9.f);
  CHECK((*mem.Value(3, EPortVarType::Vector))[2] == 0.5f);
  CPortVarMemory none;
  none.BindExternal(e);  // no table: nothing to set
  CHECK(none.Empty());
}

}  // namespace

int main() {
  TestBindExternal();
  TestIntBits();
  TestSpawnThenFrames();
  TestRealAdvSlot();
  TestVectorKeepsRng();
  return PORT_TEST_RESULT();
}
