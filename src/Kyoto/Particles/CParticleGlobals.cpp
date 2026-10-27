#include "Kyoto/Particles/CParticleGlobals.hpp"

#ifdef TARGET_PC
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/Streams/CInputStream.hpp"

const CPortVarMemory* CPortVarGlobals::sCurrent = nullptr;
SPortKeep* CPortKeepGlobals::sCurrent = nullptr;
std::vector< const CREPortKeep* >* CPortKeepGlobals::sCollect = nullptr;

CPortVarTable* PortReadVarTable(CInputStream& in) {
  const u32 count = in.ReadLong();
  if (count > 4096) {
    return nullptr;
  }
  auto* table = new CPortVarTable();
  table->vars.resize(count);
  for (SPortVar& v : table->vars) {
    in.ReadBytes(v.guid.bytes.data(), 16);
    const u32 type = in.ReadLong();
    if (type > static_cast< u32 >(EPortVarType::Rotation)) {
      delete table;
      return nullptr;
    }
    v.type = static_cast< EPortVarType >(type);
    for (float& f : v.def) {
      f = in.ReadFloat();
    }
  }
  return table;
}
#endif

int CParticleGlobals::mParticleLifetime = 0.f;
float CParticleGlobals::mParticleLifetimeReal = 0.f;
int CParticleGlobals::mEmitterTime = 0;
float CParticleGlobals::mEmitterTimeReal = 0.f;
int CParticleGlobals::mParticleLifetimePercentage = 0;
float CParticleGlobals::mParticleLifetimePercentageReal = 0.f;
float CParticleGlobals::mParticleLifetimePercentageRemainder = 0.f;
CElementGen::CParticle* CParticleGlobals::mCurrentParticle = nullptr;
float* CParticleGlobals::mParticleAccessParameters = nullptr;
CParticleGlobals::SParticleSystem* CParticleGlobals::mCurrentParticleSystem = nullptr;
#ifdef TARGET_PC
CElementGen::CParticle* CParticleGlobals::xPortUVParticle = nullptr;
const CElementGen::CParticle* CParticleGlobals::xPortIrndParticle = nullptr;
const CParticleGlobals::SPortSwooshEval* CParticleGlobals::xPortSwooshEval = nullptr;
float CParticleGlobals::xPortSystemTween = 0.f;
int CParticleGlobals::xPortSlot2Percentage = 0;
float CParticleGlobals::xPortSlot2Remainder = 0.f;

namespace {
// UpdateLifetimeTweenValues 0x24ef38 (+0x10): only the 0x1000000 cap (PSLT is min(0x1000000, ..),
// the no-PSLT default) gives 0; every other lifetime, 0x7FFFFF included, is divided through. The
// reciprocal of the lifetime is FRECPE refined by two FRECPS steps (InverseScale), frame * that,
// then the two fcsel clamps (0 - t >= 0 gives 0, t - 1 >= 0 gives 1; a NaN fails both and stays).
float PortLifetimeTween(int frame, int lifetime) {
  if (lifetime >= 0x1000000) {
    return 0.f;
  }
  const float lifeF = lifetime != 0 ? static_cast< float >(lifetime) : 1.f;
  float tween = static_cast< float >(frame) * PortParticleMath::InverseScale(lifeF);
  if (0.f - tween >= 0.f) {
    tween = 0.f;
  }
  if (tween + -1.f >= 0.f) {
    tween = 1.f;
  }
  return tween;
}
} // namespace

void CParticleGlobals::PortSetSystemTime(int frame, int lifetime) {
  xPortSystemTween = PortLifetimeTween(frame, lifetime);
}

// 0x24ef38 +0xc/+0x14: the key index is (int)(tween * 100), the remainder what the cast drops.
void CParticleGlobals::PortSetSlot2Time(int frame, int lifetime) {
  const float real = PortLifetimeTween(frame, lifetime) * 100.f;
  xPortSlot2Percentage = static_cast< int >(real);
  xPortSlot2Remainder = real - static_cast< float >(xPortSlot2Percentage);
}

// splitmix64 of (seed, element address): the same pair always gives the same bits.
u32 CParticleGlobals::PortIrndBits(const CElementGen::CParticle* particle, const void* element) {
  uint64_t z = (static_cast< uint64_t >(particle->xPortSeed) << 32) ^
               static_cast< uint64_t >(reinterpret_cast< uintptr_t >(element));
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast< u32 >(z >> 32);
}

float CParticleGlobals::PortIrndUnit(const CElementGen::CParticle* particle, const void* element) {
  return static_cast< float >(PortIrndBits(particle, element) >> 8) * (1.f / 16777216.f);
}
#endif

void CParticleGlobals::SetParticleLifetime(int lifetime) {
  mParticleLifetime = lifetime;
  mParticleLifetimeReal = static_cast< float >(lifetime);
}

void CParticleGlobals::SetEmitterTime(int time) {
  mEmitterTime = time;
  mEmitterTimeReal = static_cast< float >(time);
}

void CParticleGlobals::UpdateParticleLifetimeTweenValues(int time) {
  float d = mParticleLifetime != 0.f ? mParticleLifetime : 1.f;
  mParticleLifetimePercentageReal = time * 100.f / d;
  mParticleLifetimePercentage = mParticleLifetimePercentageReal;
  mParticleLifetimePercentageRemainder =
      mParticleLifetimePercentageReal - mParticleLifetimePercentage;
  if (mParticleLifetimePercentage < 0) {
    mParticleLifetimePercentage = 0;
  } else if (mParticleLifetimePercentage > 100) {
    mParticleLifetimePercentage = 100;
  }
}
