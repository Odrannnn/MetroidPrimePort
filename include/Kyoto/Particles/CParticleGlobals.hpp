#ifndef _CPARTICLEGLOBALS
#define _CPARTICLEGLOBALS

#include <cstring>

#include "types.h"

#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Particles/CElementGen.hpp"

class CParticleGlobals {
public:
  struct SParticleSystem {
    FourCC x0_type;
    CElementGen* x4_system;
    SParticleSystem* x8_prev;
#ifdef TARGET_PC
    // Remastered keeps the lifetime tween (UpdateLifetimeTweenValues 0x24ef38) in the system's own
    // context; the port's global is saved here so a child generator that updates or renders
    // inside this system leaves it as it was.
    float xPortSavedTween;
#endif

    SParticleSystem(FourCC type, CElementGen* system)
    : x0_type(type)
    , x4_system(system)
    , x8_prev(mCurrentParticleSystem)
#ifdef TARGET_PC
    , xPortSavedTween(xPortSystemTween)
#endif
    {
      mCurrentParticleSystem = this;
    }
    ~SParticleSystem() {
      mCurrentParticleSystem = x8_prev;
#ifdef TARGET_PC
      xPortSystemTween = xPortSavedTween;
#endif
    }
  };

  static void SetEmitterTime(int time);
  static void SetParticleLifetime(int lifetime);
  static void UpdateParticleLifetimeTweenValues(int time);

  static int GetParticleLifetime() { return mParticleLifetime; }
  static float GetParticleLifetimeReal() { return mParticleLifetimeReal; }
  static int GetEmitterTime() { return mEmitterTime; }
  static float GetEmitterTimeReal() { return mEmitterTimeReal; }
  static int GetParticleLifetimePercentage() { return mParticleLifetimePercentage; }
  static float GetParticleLifetimePercentageReal() { return mParticleLifetimePercentageReal; }
  static float GetParticleLifetimePercentageRemainder() {
    return mParticleLifetimePercentageRemainder;
  }
  static CElementGen::CParticle* GetCurrentParticle() { return mCurrentParticle; }
#ifdef TARGET_PC
  // GC reads address 0 harmlessly when no ADV values are bound; read zeros instead.
  static float* GetParticleAccessParameters() {
    static float sZeros[13] = {};
    return mParticleAccessParameters != nullptr ? mParticleAccessParameters : sZeros;
  }
  // port-only: Remastered's per-particle int variables IAV0..3 (PAP0..3), stored as their bits in
  // the four floats after ADV1..9 (CElementGen::CAdvancedValues).
  static int GetParticleAccessInt(int n) {
    int v;
    std::memcpy(&v, &GetParticleAccessParameters()[9 + n], sizeof(v));
    return v;
  }
  static void SetParticleAccessInt(float* params, int n, int v) {
    std::memcpy(&params[9 + n], &v, sizeof(v));
  }
#else
  static float* GetParticleAccessParameters() { return mParticleAccessParameters; }
#endif
  static SParticleSystem* GetCurrentParticleSystem() { return mCurrentParticleSystem; }
  static void SetCurrentParticleSystem(SParticleSystem* system) { mCurrentParticleSystem = system; }

public:
  static int mParticleLifetime;
  static float mParticleLifetimeReal;
  static int mEmitterTime;
  static float mEmitterTimeReal;
  static int mParticleLifetimePercentage;
  static float mParticleLifetimePercentageReal;
  static float mParticleLifetimePercentageRemainder;
  static CElementGen::CParticle* mCurrentParticle;
  static float* mParticleAccessParameters;
  static SParticleSystem* mCurrentParticleSystem;
#ifdef TARGET_PC
  // port-only: the particle a per-particle GetValueUV call is for (null = none)
  static CElementGen::CParticle* xPortUVParticle;
  // port-only: the particle the elements are being evaluated for, set only while a generator with
  // PIRN is evaluating per-particle elements (null otherwise). IRND elements then give a value
  // fixed per (particle, element) instead of relying on being read at frame 0.
  static const CElementGen::CParticle* xPortIrndParticle;
  // port-only: what Remastered's DFCS/DFCP (0x299134 / 0x299064) read from a swoosh's context: the
  // swoosh's local->world transform (T(global) * R(global orientation) * S(global scale), the
  // BuildTransform4f of its registers), the swoosh's local translation (DFCS) and the position of
  // the segment being evaluated (DFCP, segment +0xc). Null outside a swoosh evaluation.
  struct SPortSwooshEval {
    CTransform4f worldFromLocal;
    CVector3f systemLocal;
    const CVector3f* segmentPos;
    // The point's swoosh node key (KEWS, Remastered 0x24f00c), when the point has one.
    bool hasNode = false;
    int nodePercentage = 0;
    float nodeRemainder = 0.f;
  };
  static const SPortSwooshEval* xPortSwooshEval;
  // port-only: the system tween Remastered keeps at ctx+0x60 (UpdateLifetimeTweenValues 0x24ef38 on
  // time slot 0, written by every system tick): clamp(system frame / system lifetime, 0, 1). A
  // lifetime of 0 reads as 1, and the infinite one (the port's PSLT default 0x7FFFFF, retail's
  // 0x1000000) gives 0. The reciprocal is a plain 1 / lifetime; the original's estimate is within
  // an ulp of it.
  static float xPortSystemTween;
  static void PortSetSystemTime(int frame, int lifetime);
  // port-only: Remastered's time slot 2 (ctx+0x80), which only CParticleElectric2::UpdateBolt
  // 0xb77638 writes (node i over node count - 1, the same tween as above); the context ctor
  // 0x24f06c zeroes it, so it reads 0 everywhere else. KESP keyframes read it.
  static int xPortSlot2Percentage;
  static float xPortSlot2Remainder;
  static void PortSetSlot2Time(int frame, int lifetime);
  static void PortClearSlot2Time() {
    xPortSlot2Percentage = 0;
    xPortSlot2Remainder = 0.f;
  }
  struct PortSwooshEvalScope {
    const SPortSwooshEval* prev;
    explicit PortSwooshEvalScope(const SPortSwooshEval* eval) : prev(xPortSwooshEval) {
      xPortSwooshEval = eval;
    }
    ~PortSwooshEvalScope() { xPortSwooshEval = prev; }
  };
  // A deterministic uniform in [0, 1) from the particle's seed and the element's address, and the
  // raw 32-bit hash it comes from.
  static u32 PortIrndBits(const CElementGen::CParticle* particle, const void* element);
  static float PortIrndUnit(const CElementGen::CParticle* particle, const void* element);
#endif
};

#endif // _CPARTICLEGLOBALS
