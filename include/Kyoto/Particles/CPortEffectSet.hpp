#ifndef _CPORTEFFECTSET
#define _CPORTEFFECTSET

#ifdef TARGET_PC

#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/TToken.hpp"

// Remastered's CEffectModule effect set, for the one-shot muzzle flashes. Start
// (SetupForStart 0x6ab0d4, behaviour 0) stops the active effect's emission and moves it
// to the fading list (SEffectSet::FadeActive 0x6a7f90), drops the oldest fading ones
// until limit - 1 remain (LimitFading 0x6a8500), then creates a new effect. UpdateEffects
// 0x6aa38c removes deletable effects; the module is finished when none remain.
// SetupTransforms 0x6aaf4c (mode 1) gives each effect the orientation, translation and
// scale.
class CPortEffectSet {
public:
  enum { kMaxEffects = 3 };

  CPortEffectSet() : mCount(0), mHasActive(false) {}
  ~CPortEffectSet() { Clear(); }

  void Start(const TToken< CGenDescription >& desc, int limit) {
    if (mHasActive) {
      mEffects[mCount - 1]->SetParticleEmission(false);
      mHasActive = false;
    }
    int keep = limit - 1;
    if (keep < 0) {
      keep = 0;
    }
    if (keep > kMaxEffects - 1) {
      keep = kMaxEffects - 1;
    }
    while (mCount > keep) {
      Remove(0);
    }
    mEffects[mCount++] = rs_new CElementGen(desc);
    mHasActive = true;
  }

  void Update(float dt, const CTransform4f& xf, const CVector3f& scale) {
    for (int i = 0; i < mCount;) {
      CElementGen* gen = mEffects[i];
      gen->SetGlobalOrientAndTrans(xf);
      gen->SetGlobalScale(scale);
      gen->Update(dt);
      if (gen->IsSystemDeletable()) {
        Remove(i);
      } else {
        ++i;
      }
    }
  }

  void Render() const {
    for (int i = 0; i < mCount; ++i) {
      mEffects[i]->Render();
    }
  }

  bool IsFinished() const { return mCount == 0; }

  void Clear() {
    while (mCount > 0) {
      Remove(mCount - 1);
    }
  }

private:
  void Remove(int idx) {
    if (mHasActive && idx == mCount - 1) {
      mHasActive = false;
    }
    delete mEffects[idx];
    for (int i = idx; i + 1 < mCount; ++i) {
      mEffects[i] = mEffects[i + 1];
    }
    --mCount;
  }

  CElementGen* mEffects[kMaxEffects];
  int mCount;
  bool mHasActive;

  CPortEffectSet(const CPortEffectSet&);
  CPortEffectSet& operator=(const CPortEffectSet&);
};

#endif // TARGET_PC

#endif // _CPORTEFFECTSET
