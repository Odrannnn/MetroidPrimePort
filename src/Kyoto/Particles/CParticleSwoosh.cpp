#include "Kyoto/Particles/CParticleSwoosh.hpp"

#include "Kyoto/Basics/CStopwatch.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CLight.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Particles/CParticleDepthBias.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CSwooshDescription.hpp"

#include "Kyoto/Math/CAbsAngle.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "dolphin/gx/GXVert.h"
#include "float.h"
#include "rstl/math.hpp"
#include "rstl/reserved_vector.hpp"

#ifdef TARGET_PC
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "port_beam_colours.h"
#include "port_debug.h"
#include <climits>
#include "port_fx_debug.h"
#include "port_room_env.h"
#include "port_swoosh_ring.h"
#include "port_vfx_particles.h"
#endif

uint CParticleSwoosh::mSwooshAliveCount = 0;
#ifdef TARGET_PC
uint CParticleSwoosh::sPortChildSeed = 0;
bool CParticleSwoosh::sPortChildSeedSet = false;
#endif
static float kFrameTime = 1.f / 60.f;

#pragma sym on

CParticleSwoosh::CParticleSwoosh(const TToken< CSwooshDescription > desc, const int leng)
: x1c_desc(desc)
, x28_curFrame(0)
, x2c_PSLT(0)
, x30_curTime(0.0)
, x38_translation(0.f, 0.f, 0.f)
, x44_orientation(CTransform4f::Identity())
, x74_invOrientation(CTransform4f::Identity())
, xa4_globalTranslation(0.f, 0.f, 0.f)
, xb0_globalOrientation(CTransform4f::Identity())
, xe0_globalScale(1.f, 1.f, 1.f)
, xec_scaleXf(CTransform4f::Identity())
, x11c_invScaleXf(CTransform4f::Identity())
, x14c_localScale(1.f, 1.f, 1.f)
, x158_curParticle(0)
, x1ac_particleCount(0)
, x1b0_SPLN(0)
, x1b8_SIDE(2) // Remastered's CSwooshDescription ctor defaults SIDE (desc+0x208) to 2
, x1c0_rand(x1c_desc->x45_26_CRND ? CCast::ToInt16(CStopwatch::GetGlobalMicros())
                                  : CCast::ToInt16(99))
, x1c4_(0.f)
, x1c8_(0.f)
, x1d0_24_emitting(true)
, x1d0_26_forceOneUpdate(false)
, x1d0_27_renderGaps(false)
, x1d0_31_constantTex(false)
, x1d1_24_constantUv(false)
, x1d4_uMin(0.f)
, x1d8_vMin(0.f)
, x1dc_uMax(0.f)
, x1e0_vMax(0.f)
, x1e4_tex(nullptr)
, x1e8_uvSpan(1.f)
, x1ec_TSPN(0)
, x1f0_aabbMin(CVector3f::Zero())
, x1fc_aabbMax(CVector3f::Zero())
, x208_maxRadius(0.f)
, x20c_moduColor(0xffffffff) {
  int i = 0;
#ifdef TARGET_PC
  xPortVars.Init(x1c_desc->xPortVars.get());
  xPortVars.InitSys(x1c_desc->xPortSys.get());
  // The generator that builds this swoosh, for SPA*/DPV*/TPV*.
  xPortVars.SetParent(CPortVarGlobals::sCurrent);
  CPortVarScope portVarScope(&xPortVars);
  const bool portChildSeed = sPortChildSeedSet;
  sPortChildSeedSet = false;
  if (x1c_desc->xPortIrnd) {
    // Remastered reads CRND and drops it (CreateSWSH 0x2dc4cc). The CParticleGen ctor seeds the
    // system from its parms (0x279724: 99 by default, ctor 0x2af2e0; a parent's child seed), and
    // a nonzero SEED (desc+0x204) replaces that (CParticleSwoosh ctor 0x27c6bc).
    uint seed = portChildSeed ? sPortChildSeed : 99;
    if (x1c_desc->xPortSeed != 0) {
      seed = x1c_desc->xPortSeed;
    }
    x1c0_rand.SetSeed(seed);
  }
  // INTR holds from creation, before any setter runs.
  if (x1c_desc->xPortIntRotation) {
    SetOrientation(CTransform4f::Identity());
  }
#endif
  CGlobalRandom _(x1c0_rand);
  mSwooshAliveCount++;

  if (leng > 0) {
    x1b4_LENG = leng;
  } else if (x1c_desc->x10_LENG) {
    x1c_desc->x10_LENG->GetValue(0, x1b4_LENG);
  }
  ++x1b4_LENG;

  if (x1c_desc->x18_SIDE) {
    x1c_desc->x18_SIDE->GetValue(0, x1b8_SIDE);
  }

  x1d0_28_LLRD = x1c_desc->x44_24_LLRD;
  x1d0_29_VLS1 = x1c_desc->x44_26_VLS1;
  x1d0_30_VLS2 = x1c_desc->x44_27_VLS2;

  if (IsLargeEnough()) {
    if (x1c_desc->x0_PSLT) {
      x1c_desc->x0_PSLT->GetValue(0, x2c_PSLT);
    } else {
      x2c_PSLT = 0x7fffff;
    }
#ifdef TARGET_PC
    // CAddVarSystem::Init once LENG and SIDE are known (ctor 0x27c858).
    PortEvalSys(true);
#endif

    x1d0_25_AALP = x1c_desc->x44_31_AALP;

    if (x1c_desc->x38_SPLN) {
      x1c_desc->x38_SPLN->GetValue(0, x1b0_SPLN);
    }
    if (x1b0_SPLN < 0) {
      x1b0_SPLN = 0;
    }

    x15c_swooshes.clear();
    x15c_swooshes.reserve(x1b4_LENG);
    for (i = 0; i < x15c_swooshes.capacity(); i++) {
      x15c_swooshes.push_back(SSwooshData());
    }

    SetOrientation(CTransform4f::Identity());

    x16c_p0.clear();
    x16c_p0.reserve(x1b8_SIDE);

    for (i = 0; i < x16c_p0.capacity(); i++) {
      x16c_p0.push_back(CVector3f::Zero());
    }
    x17c_p1.clear();
    x17c_p1.reserve(x1b8_SIDE);
    for (i = 0; i < x17c_p1.capacity(); i++) {
      x17c_p1.push_back(CVector3f::Zero());
    }

    x18c_p2.clear();
    x18c_p2.reserve(x1b8_SIDE);
    for (i = 0; i < x18c_p2.capacity(); i++) {
      x18c_p2.push_back(CVector3f::Zero());
    }
    x19c_p3.clear();
    x19c_p3.reserve(x1b8_SIDE);
    for (i = 0; i < x19c_p3.capacity(); i++) {
      x19c_p3.push_back(CVector3f::Zero());
    }
  }
}

CParticleSwoosh::~CParticleSwoosh() { --mSwooshAliveCount; }

bool CParticleSwoosh::IsLargeEnough() const { return x1b4_LENG >= 2 && x1b8_SIDE >= 2; }

#ifdef TARGET_PC
// DFCS/DFCP context: Remastered builds T(global) * R(orientation) * S(scale) from the swoosh's
// registers and applies it to the segment position / local translation (0x299064, 0x299134).
static CParticleGlobals::SPortSwooshEval PortSwooshEvalFor(const CTransform4f& world,
                                                           const CVector3f& systemLocal,
                                                           const CVector3f* segmentPos) {
  return {world, systemLocal, segmentPos};
}
#endif

#ifdef TARGET_PC
namespace {
// Binds a point's variables for the PAPn its elements read (Remastered's ctx+0x20); a null one
// keeps whatever is bound.
struct PortAccessScope {
  float* prev = CParticleGlobals::mParticleAccessParameters;
  explicit PortAccessScope(float* values) {
    if (values != nullptr) {
      CParticleGlobals::mParticleAccessParameters = values;
    }
  }
  ~PortAccessScope() { CParticleGlobals::mParticleAccessParameters = prev; }
  PortAccessScope(const PortAccessScope&) = delete;
  PortAccessScope& operator=(const PortAccessScope&) = delete;
};
}  // namespace

float* CParticleSwoosh::PortPartValues(int index) {
  if (!x1c_desc->xPortPartVars) {
    return nullptr;
  }
  const size_t size = size_t(x15c_swooshes.capacity()) * kPortPartValues;
  if (xPortPartValues.size() != size) {
    xPortPartValues.assign(size, 0.f);
  }
  return &xPortPartValues[size_t(index) * kPortPartValues];
}
#endif

const bool CParticleSwoosh::Update(double dt) {
#ifdef TARGET_PC
  PortFx::UpdateScope fxScope(dt);
  if (fxScope.skip) {
    return false;
  }
  dt = fxScope.dt;
  CPortVarScope portVarScope(&xPortVars);
#endif
  if (!IsLargeEnough()) {
    return false;
  }
  CStopwatch timer;
  CParticleGlobals::SetParticleLifetime(x1b4_LENG);
  CParticleGlobals::SetEmitterTime(x28_curFrame);
  CParticleGlobals::UpdateParticleLifetimeTweenValues(0);
  CGlobalRandom random(x1c0_rand);
#ifdef TARGET_PC
  // The first Update runs CAddVarSystem::Init again (0x27dde4, or InitialUpdate 0x27d16c).
  if (!xPortSysInit) {
    xPortSysInit = true;
    PortEvalSys(true);
  }
#endif
  double evalTime = x28_curFrame * kFrameTime;
  float timeScale = 1.f;
  if (x1c_desc->x4_TIME) {
    x1c_desc->x4_TIME->GetValue(x28_curFrame, timeScale);
  }
  double advance = dt * timeScale;
  if (advance < 0.0) {
    advance = 0.0;
  }
  x30_curTime += advance;
  while (x1d0_26_forceOneUpdate || evalTime < x30_curTime) {
    x1d0_26_forceOneUpdate = false;
#ifdef TARGET_PC
    // CAddVarSystem::Update at the start of each tick (InternalUpdate 0x27d308).
    PortEvalSys(false);
    // Remastered advances the ring only in CreateNewParticle 0x27cdc8; an electric swoosh's
    // points are never retired by a step without one.
    const bool portEmit = x1d0_24_emitting && x28_curFrame < x2c_PSLT;
    if (!xPortElectric || portEmit)
#endif
    if (++x158_curParticle >= x15c_swooshes.capacity()) {
      x158_curParticle = 0;
    }
#ifdef TARGET_PC
    // EMRV is a system-level defined var: the translation since the last step, zero on the
    // system's first, its previous value kept every step.
    const CVector3f razyMotion =
        x28_curFrame == 0 ? CVector3f(0.f, 0.f, 0.f) : x38_translation - xPortRazyPrev;
    xPortRazyPrev = x38_translation;
#endif
    if (x1d0_24_emitting && x28_curFrame < x2c_PSLT) {
      UpdateSwooshTranslation(x38_translation);
#ifdef TARGET_PC
      const CParticleGlobals::SPortSwooshEval spawnEval = PortSwooshEvalFor(
          CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation * xec_scaleXf,
          x38_translation, &x15c_swooshes[x158_curParticle].mTranslation);
      CParticleGlobals::PortSwooshEvalScope spawnEvalScope(&spawnEval);
      SPortKeep& spawnKeep = x15c_swooshes[x158_curParticle].mPortKeep;
      spawnKeep.latched = 0;
      CPortKeepScope spawnKeepScope(&spawnKeep);
      for (const CREPortKeep* keep : x1c_desc->xPortKeeps) {
        keep->Latch(0);
      }
      // CreateNewParticle 0x27cdc8: the new point's time context (age 0, life LENG - 1), then
      // _Init_Particle (0x27ced0) runs every per-point variable at time 0 into its slice, which
      // stays bound for the point's create-time elements.
      // An electric point is born at the age it keeps: the live count, itself included, less one.
      const int spawnAge =
          xPortElectric
              ? x1ac_particleCount - (x15c_swooshes[x158_curParticle].mActive ? 1 : 0)
              : 0;
      float* const spawnValues = PortPartValues(x158_curParticle);
      PortAccessScope spawnAccess(spawnValues);
      if (spawnValues != nullptr) {
        CParticleGlobals::SetParticleLifetime(rstl::max_val(x1b4_LENG - 1, 1));
        CParticleGlobals::UpdateParticleLifetimeTweenValues(spawnAge);
        static CRealElement* const kNoAdvs[8] = {};
        x1c_desc->xPortPartVars->Eval(kNoAdvs, true, spawnAge, spawnValues);
      }
#endif
      if (x1c_desc->x1c_IROT) {
        x1c_desc->x1c_IROT->GetValue(x28_curFrame, x15c_swooshes[x158_curParticle].mInitialRot);
      } else {
        x15c_swooshes[x158_curParticle].mInitialRot = 0.f;
      }
      x15c_swooshes[x158_curParticle].mRotm = 0.f;
      x15c_swooshes[x158_curParticle].mStartFrame = x28_curFrame;
      if (!x15c_swooshes[x158_curParticle].mActive) {
        ++x1ac_particleCount;
        x15c_swooshes[x158_curParticle].mActive = true;
      }
#ifdef TARGET_PC
      if (xPortElectric) {
        x15c_swooshes[x158_curParticle].mFrame = spawnAge;
      }
#endif
      x15c_swooshes[x158_curParticle].mOrientation = x44_orientation;
#ifdef TARGET_PC
      if (x1c_desc->xPortRazyAxis) {
        x15c_swooshes[x158_curParticle].mOrientation = PortRazyOrientation(razyMotion);
      }
      if (x1c_desc->xPortRotZ) {
        x15c_swooshes[x158_curParticle].mOrientation = PortRotZOrientation();
      }
#endif
      if (x1c_desc->x28_IVEL) {
        x1c_desc->x28_IVEL->GetValue(x28_curFrame, x15c_swooshes[x158_curParticle].mVelocity);
        x15c_swooshes[x158_curParticle].mVelocity =
            x44_orientation * x15c_swooshes[x158_curParticle].mVelocity;
      }
      if (x1c_desc->x24_POFS) {
        x1c_desc->x24_POFS->GetValue(x28_curFrame, x15c_swooshes[x158_curParticle].mOffset);
#ifdef TARGET_PC
        if (x1c_desc->xPortRotateOffset) {
          x15c_swooshes[x158_curParticle].mOffset = x44_orientation * x15c_swooshes[x158_curParticle].mOffset;
        }
        x15c_swooshes[x158_curParticle].mOffset =
            CVector3f::ByElementMultiply(x15c_swooshes[x158_curParticle].mOffset, xPortSpawnSpread);
#endif
      }
      x15c_swooshes[x158_curParticle].mUseOffset = x15c_swooshes[x158_curParticle].mOffset;
      if (x1c_desc->x14_COLR) {
        x1c_desc->x14_COLR->GetValue(x28_curFrame, x15c_swooshes[x158_curParticle].mColor);
      } else {
        x15c_swooshes[x158_curParticle].mColor = CColor(1.f, 1.f, 1.f, 1.f);
      }
      int tspn = 0;
      if (x1c_desc->x40_TSPN) {
        x1c_desc->x40_TSPN->GetValue(x28_curFrame, tspn);
      }
      x1cc_TSPN = tspn;
#ifdef TARGET_PC
      ++xPortCreated; // CreateNewParticle 0x27d128
#endif
    }
#ifdef TARGET_PC
    else if (xPortElectric) {
    }
#endif
    else if (x15c_swooshes[x158_curParticle].mActive) {
      x1ac_particleCount = rstl::max_val(0, x1ac_particleCount - 1);
      x15c_swooshes[x158_curParticle].mActive = false;
    }
#ifdef TARGET_PC
    // A PROT not kept from birth runs again for every live point each frame (UpdateExisting 0x27d590).
    if (x1c_desc->xPortRotZ && !x1c_desc->xPortRotKeep) {
      for (SSwooshData& swoosh : x15c_swooshes) {
        if (swoosh.mActive) {
          swoosh.mOrientation = PortRotZOrientation();
        }
      }
    }
#endif
    UpdateTranslationAndOrientation();
    evalTime += kFrameTime;
    ++x28_curFrame;
#ifdef TARGET_PC
    // UpdateCountdownTimerForShutdown (0x27b864) after the tick's frame step (InternalUpdate
    // 0x27d29c): counts down while positive, shuts down at zero.
    if (xPortShutdownCountdown > 0 && --xPortShutdownCountdown == 0) {
      PortDoShutdownBehavior(0xff);
    }
#endif
  }
  x1c4_ = timer.GetElapsedTime();
  return false;
}

void CParticleSwoosh::SetOrientation(const CTransform4f& orientation) {
#ifdef TARGET_PC
  // XFMD 3: Remastered's setters all write the one system var (MPT_SetOrientation 0x27f92c ->
  // SetExternalRotation), which the draw uses as the global orientation; the local one stays identity.
  // INTR replaces what they set with the system rotation var (PortIntOrientation).
  xPortExtOrientation = orientation;
  // XFMD 1: the birth orientation (var bc) is the system's own; only EXTR reads what is set.
  if (x1c_desc->xPortXfmdSystem) {
    return;
  }
  const CTransform4f system = x1c_desc->xPortIntRotation ? PortIntOrientation(orientation) : orientation;
  if (x1c_desc->xPortXfmdFollow) {
    xb0_globalOrientation = system.GetRotation();
    return;
  }
  x44_orientation = system;
#else
  x44_orientation = orientation;
#endif
  x74_invOrientation = x44_orientation.GetQuickInverse();
#ifdef TARGET_PC
  // KPIN freezes a PRAZ point's rotation at its birth; PROZ sets its own.
  if (x1c_desc->xPortRazyAxis || x1c_desc->xPortRotZ) {
    return;
  }
  // An undersized swoosh (!IsLargeEnough) never fills the ring; retail wrote
  // through the empty vector into low memory.
  if (x15c_swooshes.empty()) {
    return;
  }
#endif
  x15c_swooshes[x158_curParticle].mOrientation = x44_orientation;
}

#ifdef TARGET_PC
// HACK_InitForElectric 0x27ebfc: one forced step per ring slot creates every point (ages 0 to
// capacity - 1, frozen), then emission stops and a last forced step runs from frame 0.
void CParticleSwoosh::PortInitForElectric() {
  xPortElectric = true;
  for (int i = 0; i < x15c_swooshes.capacity(); ++i) {
    SetWarmUp();
    Update(0.0);
  }
  SetParticleEmission(false);
  x28_curFrame = 0;
  SetWarmUp();
  Update(0.0);
  x28_curFrame = 0;
}

// HACK_ReInitForElectric 0x27ed0c: a forced step from frame 0 for a new bolt. Remastered keeps
// +0x30's time and steps once per frame unit of dt; the port's Update would catch up every frame
// since that time instead, so its time restarts too (the same steps at a fixed frame dt).
void CParticleSwoosh::PortReInitForElectric() {
  x28_curFrame = 0;
  x30_curTime = 0.0;
  SetWarmUp();
  Update(0.0);
  x28_curFrame = 0;
}

// The system variables in PSYS order, as CElementGen::PortEvalSys: all of them (CAddVarSystem::Init
// 0x278438) or those that are not init-only (Update 0x278638), on the swoosh's own time context:
// its frame and +0x140, the PSLT capped at 0x1000000 or that without one (ctor 0x27c6e8).
void CParticleSwoosh::PortEvalSys(bool init) {
  const CPortSys* sys = x1c_desc->xPortSys.get();
  if (sys == nullptr) {
    return;
  }
  const int frame = x28_curFrame;
  const int systemLifetime = x1c_desc->x0_PSLT ? rstl::min_val(x2c_PSLT, 0x1000000) : 0x1000000;
  const int lifetime = CParticleGlobals::mParticleLifetime;
  const float lifetimeReal = CParticleGlobals::mParticleLifetimeReal;
  const int percentage = CParticleGlobals::mParticleLifetimePercentage;
  const float percentageReal = CParticleGlobals::mParticleLifetimePercentageReal;
  const float percentageRemainder = CParticleGlobals::mParticleLifetimePercentageRemainder;
  const float systemTween = CParticleGlobals::xPortSystemTween;
  CParticleGlobals::SetParticleLifetime(systemLifetime);
  CParticleGlobals::UpdateParticleLifetimeTweenValues(frame);
  CParticleGlobals::PortSetSystemTime(frame, systemLifetime);
  for (const SPortSysItem& item : sys->items) {
    if (item.initOnly && !init) {
      continue;
    }
    std::array< float, 4 > v{};
    switch (item.type) {
    case EPortVarType::Real:
      item.real->GetValue(frame, v[0]);
      break;
    case EPortVarType::Int: {
      int i = 0;
      item.intE->GetValue(frame, i);
      v[0] = static_cast< float >(i);
    } break;
    case EPortVarType::Color:
      item.color->GetValueHdr(frame, v.data());
      break;
    default: {
      CVector3f vec;
      item.vec->GetValue(frame, vec);
      v = {vec.GetX(), vec.GetY(), vec.GetZ(), 0.f};
    } break;
    }
    xPortVars.SetSys(item.type, item.slot, v);
  }
  CParticleGlobals::mParticleLifetime = lifetime;
  CParticleGlobals::mParticleLifetimeReal = lifetimeReal;
  CParticleGlobals::mParticleLifetimePercentage = percentage;
  CParticleGlobals::mParticleLifetimePercentageReal = percentageReal;
  CParticleGlobals::mParticleLifetimePercentageRemainder = percentageRemainder;
  CParticleGlobals::xPortSystemTween = systemTween;
}

// Remastered's PROT KPIN(RAZY(EMRV, ROTV(axis, RLCL), fallback)) at a point's birth
// (CreateNewParticle 0x27cdc8, CERotationFromAxis::GetValue 0x29d298): Z = the motion, X = F x Z
// for F the axis turned by the orientation, Y = Z x X, each falling back (orientation or identity)
// under CParticleValues::skMinNormalizableMagSqd. Remastered draws its ring as q * (r cos, r sin, 0)
// (UpdateRenderData_Swoosh 0x2b52e0) and the port as m * (r cos, 0, r sin), so m = Q * Rx(-90).
// q is in Remastered's particle space (X left, Y up, Z forward), so Q = B * q in the port's, for
// B (x, y, z) to (-x, z, y), and its orientation is B R B: Q's identity fallback is B and its
// orientation fallback R * B. The motion and the axis (the converter's BSIS) are the port's, and
// B keeps cross products, so the frame built from them is Q's own.
CTransform4f CParticleSwoosh::PortRazyOrientation(const CVector3f& motion) const {
  static const float kMinMagSqd = 1.4210854715202004e-14f;
  CVector3f x(-1.f, 0.f, 0.f);
  CVector3f y(0.f, 0.f, 1.f);
  CVector3f z(0.f, 1.f, 0.f);
  if (x1c_desc->xPortRazyFallbackLocal) {
    x = -x44_orientation.GetColumn(kDX);
    y = x44_orientation.GetColumn(kDZ);
    z = x44_orientation.GetColumn(kDY);
  }
  CVector3f axis(0.f, 1.f, 0.f);
  x1c_desc->xPortRazyAxis->GetValue(x28_curFrame, axis);
  const float motionMag = motion.MagSquared();
  if (motionMag >= kMinMagSqd) {
    const CVector3f rowZ = motion * (1.f / CMath::SqrtF(motionMag));
    const CVector3f cross = CVector3f::Cross(x44_orientation.Rotate(axis), rowZ);
    const float crossMag = cross.MagSquared();
    if (crossMag >= kMinMagSqd) {
      x = cross * (1.f / CMath::SqrtF(crossMag));
      z = rowZ;
      y = CVector3f::Cross(z, x);
    }
  }
  return CTransform4f::FromColumns(x, -z, y, CVector3f(0.f, 0.f, 0.f));
}

// Remastered's PROT of [KPIN]([RADD(EXTR, ]REUL(0, 0, z, #00))): q = orientation (or identity) * Rz(z
// degrees) (CERotationEuler 0x29c944, RADD = a * b), drawn as m = Q * Rx(-90) like PortRazyOrientation,
// Q = B * q: [R *] B * Rz(z) in the port's space.
CTransform4f CParticleSwoosh::PortRotZOrientation() const {
  float degrees = 0.f;
  x1c_desc->xPortRotZ->GetValue(x28_curFrame, degrees);
  const CTransform4f q = CTransform4f::RotateZ(CRelAngle::FromDegrees(degrees));
  CVector3f columns[3];
  for (int i = 0; i < 3; ++i) {
    const CVector3f c = q.GetColumn(i);
    columns[i] = CVector3f(-c.GetX(), c.GetZ(), c.GetY());
    if (x1c_desc->xPortRotOriented) {
      columns[i] = (x1c_desc->xPortRotIntr ? x44_orientation : xPortExtOrientation).Rotate(columns[i]);
    }
  }
  return CTransform4f::FromColumns(columns[0], -columns[2], columns[1], CVector3f(0.f, 0.f, 0.f));
}

// INTR, the system rotation var 0xb (CParticleDescription::Compile 0x289af0), which the setters do
// not write: r, or the external orientation times r (RADD = a * b). In the port's space r is B r B
// (the converter's PINR Euler vector, turned as CElementGen::PortApplyIntFrame turns it), and the
// external orientation in Remastered's is B Q B, so RADD(EXTR, r) is Q * (B r B).
CTransform4f CParticleSwoosh::PortIntOrientation(const CTransform4f& external) const {
  CVector3f euler = CVector3f::Zero();
  x1c_desc->xPortIntRotation->GetValue(0, euler);
  CTransform4f rotation = CTransform4f::RotateZ(CRelAngle::FromDegrees(euler.GetZ()));
  rotation.RotateLocalY(CRelAngle::FromDegrees(euler.GetY()));
  rotation.RotateLocalX(CRelAngle::FromDegrees(euler.GetX()));
  return x1c_desc->xPortIntExternal ? external.GetRotation() * rotation : rotation;
}
#endif

void CParticleSwoosh::UpdateTranslationAndOrientation() {
  if (x1ac_particleCount <= 0) {
    return;
  }
  x208_maxRadius = 0.f;
  x1f0_aabbMin = CVector3f(FLT_MAX, FLT_MAX, FLT_MAX);
  x1fc_aabbMax = CVector3f(-FLT_MAX, -FLT_MAX, -FLT_MAX);
#ifdef TARGET_PC
  // Remastered reads a point's keyframes at its age over the ring size less one (CreateNewParticle
  // 0x27cdc8: life = this+0x180 - 1, the LENG the constructor 0x27c4f8 sizes the ring with), and
  // its KEWS at the node key (0x24f00c): (age - base) / (count - 1), where InternalUpdate 0x27d1b4
  // counts the live points (the new one included) and takes the youngest age as the base (0 while
  // emitting).
  const bool portRemastered = x1c_desc->xPortIrnd;
  CParticleGlobals::SetParticleLifetime(portRemastered ? rstl::max_val(x1b4_LENG - 1, 1) : x1b4_LENG);
  const int nodeCount = x1ac_particleCount;
  int nodeBase = 0;
  // An electric swoosh's frozen ages run from 0.
  if (portRemastered && !xPortElectric && !(x1d0_24_emitting && x28_curFrame < x2c_PSLT)) {
    nodeBase = INT_MAX;
    for (int k = 0; k < x15c_swooshes.capacity(); ++k) {
      if (x15c_swooshes.data()[k].mActive) {
        nodeBase = rstl::min_val(nodeBase, x28_curFrame - x15c_swooshes.data()[k].mStartFrame);
      }
    }
  }
  const bool portVfx = PortVfxSwoosh();
  if (portVfx && xPortVfxPts.size() != size_t(x15c_swooshes.capacity())) {
    xPortVfxPts.assign(x15c_swooshes.capacity(), SPortVfxPoint());
  }
#else
  CParticleGlobals::SetParticleLifetime(x1b4_LENG);
#endif
  CParticleGlobals::SetEmitterTime(x28_curFrame);
  int i = 0;
  CVector3f offset(0.f, 0.f, 0.f);
  CVector3f localVel(0.f, 0.f, 0.f);
  CVector3f localTrans(0.f, 0.f, 0.f);
  for (i = 0; i < x15c_swooshes.capacity(); ++i) {
    SSwooshData& swoosh = x15c_swooshes.data()[i];
    if (!swoosh.mActive) {
      continue;
    }
#ifdef TARGET_PC
    // UpdateExisting 0x27d590 leaves an electric point's age as it was born (+0x188 bit 5).
    if (!xPortElectric)
#endif
    swoosh.mFrame = x28_curFrame - swoosh.mStartFrame;
    CParticleGlobals::UpdateParticleLifetimeTweenValues(swoosh.mFrame);
#ifdef TARGET_PC
    CParticleGlobals::SPortSwooshEval segEval = PortSwooshEvalFor(
        CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation * xec_scaleXf,
        x38_translation, &swoosh.mTranslation);
    if (portRemastered) {
      segEval.hasNode = true;
      if (nodeCount > 1) {
        const float key = float(swoosh.mFrame - nodeBase) / float(nodeCount - 1) * 100.f;
        segEval.nodePercentage = int(key);
        segEval.nodeRemainder = key - float(segEval.nodePercentage);
        if (segEval.nodePercentage > 99) {
          segEval.nodePercentage = 100;
          segEval.nodeRemainder = 0.f;
        }
      }
    }
    CParticleGlobals::PortSwooshEvalScope segEvalScope(&segEval);
    CPortKeepScope segKeepScope(&swoosh.mPortKeep);
    // UpdateExisting 0x27d590: _Update_Particle (0x27d6d4) runs the variables that are not
    // constant after start at the point's age, before its elements. A point is never updated in
    // the tick that created it (CreateNewParticle comes after UpdateExisting).
    float* const pointValues = PortPartValues(i);
    PortAccessScope pointAccess(pointValues);
    const bool bornThisStep = xPortElectric
                                  ? x1d0_24_emitting && x28_curFrame < x2c_PSLT &&
                                        i == x158_curParticle
                                  : swoosh.mFrame <= 0;
    if (pointValues != nullptr && !bornThisStep) {
      static CRealElement* const kNoAdvs[8] = {};
      x1c_desc->xPortPartVars->Eval(kNoAdvs, false, swoosh.mFrame, pointValues);
    }
#endif
    if (x1c_desc->x44_28_SROT == true) {
      if (x1c_desc->x1c_IROT) {
        x1c_desc->x1c_IROT->GetValue(x28_curFrame, swoosh.mInitialRot);
      }
      swoosh.mRotm = 0.f;
    } else {
      if (x1c_desc->x20_ROTM) {
        x1c_desc->x20_ROTM->GetValue(x28_curFrame, swoosh.mRotm);
      } else {
        swoosh.mRotm = 0.f;
      }
    }
    if (x1c_desc->x30_VELM) {
      if (x1d0_29_VLS1) {
        localVel = x74_invOrientation * swoosh.mVelocity;
        localTrans = x74_invOrientation * (swoosh.mTranslation - x38_translation);
        x1c_desc->x30_VELM->GetValue(swoosh.mFrame, localVel, localTrans);
        swoosh.mVelocity = x44_orientation * localVel;
        swoosh.mTranslation = x44_orientation * localTrans + x38_translation;
      } else {
        x1c_desc->x30_VELM->GetValue(swoosh.mFrame, swoosh.mVelocity, swoosh.mTranslation);
      }
    }
    if (x1c_desc->x34_VLM2) {
      if (x1d0_30_VLS2) {
        localVel = x74_invOrientation * swoosh.mVelocity;
        localTrans = x74_invOrientation * (swoosh.mTranslation - x38_translation);
        x1c_desc->x34_VLM2->GetValue(swoosh.mFrame, localVel, localTrans);
        swoosh.mVelocity = x44_orientation * localVel;
        swoosh.mTranslation = x44_orientation * localTrans + x38_translation;
      } else {
        x1c_desc->x34_VLM2->GetValue(swoosh.mFrame, swoosh.mVelocity, swoosh.mTranslation);
      }
    }
    if (swoosh.mFrame > 0) {
      swoosh.mTranslation += swoosh.mVelocity;
    }
    if (x1c_desc->x2c_NPOS) {
      x1c_desc->x2c_NPOS->GetValue(swoosh.mFrame, offset);
      swoosh.mUseOffset = swoosh.mOffset + offset;
    }
    if (x1c_desc->x14_COLR) {
      x1c_desc->x14_COLR->GetValue(swoosh.mFrame, swoosh.mColor);
    }
#ifdef TARGET_PC
    if (portVfx) {
      SPortVfxPoint& point = xPortVfxPts[i];
      PortVfxEvalPoint(*x1c_desc->xPortVfx, swoosh.mFrame, point.iten, point.vpmt);
      if (x1c_desc->x14_COLR) {
        x1c_desc->x14_COLR->GetValueHdr(swoosh.mFrame, point.color);
      }
    }
    // PSCL runs for every point after PROT, at its birth and each update (UpdateExisting 0x27d700).
    if (x1c_desc->xPortScale) {
      x1c_desc->xPortScale->GetValue(swoosh.mFrame, swoosh.mPortScale);
    }
#endif
    swoosh.mLeftRad = GetLeftRadius(i);
    UpdateMaxRadius(swoosh.mLeftRad);
    if (x1d0_28_LLRD) {
      swoosh.mRightRad = swoosh.mLeftRad;
    } else {
      swoosh.mRightRad = GetRightRadius(i);
      UpdateMaxRadius(swoosh.mRightRad);
    }
    UpdateBounds(swoosh.mTranslation + swoosh.mUseOffset);
  }
}

void CParticleSwoosh::SetTranslation(const CVector3f& translation) {
#ifdef TARGET_PC
  // XFMD 1: the system's own transform births and draws the points; no setter moves them.
  if (x1c_desc->xPortXfmdSystem) {
    xPortExtTranslation = translation;
    return;
  }
  // XFMD 3: the one system translation (MPT_SetTranslation 0x27f9c4 -> SetExternalTranslation
  // 0x27b45c), drawn as the global translation; points are born at zero and the head is not moved.
  if (x1c_desc->xPortXfmdFollow) {
    xa4_globalTranslation = translation;
    return;
  }
#endif
  x38_translation = translation;
  UpdateSwooshTranslation(x38_translation);
}

void CParticleSwoosh::SetGlobalTranslation(const CVector3f& translation) {
#ifdef TARGET_PC
  if (x1c_desc->xPortXfmdSystem) {
    xPortExtTranslation = translation;
    return;
  }
  // XFMD 4: MPT_SetGlobalTranslation 0x27faa4 also writes the external translation, which the
  // EMVR's EXTT gives var b8, the position a point is born at; the global translation stays zero.
  if (x1c_desc->xPortXfmdMover) {
    SetTranslation(translation);
    return;
  }
#endif
  xa4_globalTranslation = translation;
}

void CParticleSwoosh::SetGlobalOrientation(const CTransform4f& orientation) {
#ifdef TARGET_PC
  if (x1c_desc->xPortXfmdSystem) {
    xPortExtOrientation = orientation;
    return;
  }
  // XFMD 4: MPT_SetGlobalOrientation 0x27fa24 writes the external rotation (EXTR, var bc).
  if (x1c_desc->xPortXfmdMover) {
    SetOrientation(orientation);
    return;
  }
  // XFMD 3: it writes the one system var (0x27fa24 -> SetExternalRotation), so INTR replaces it too.
  if (x1c_desc->xPortXfmdFollow && x1c_desc->xPortIntRotation) {
    SetOrientation(orientation);
    return;
  }
#endif
  xb0_globalOrientation = orientation.GetRotation();
}

void CParticleSwoosh::SetLocalScale(const CVector3f& scale) {
#ifdef TARGET_PC
  if (x1c_desc->xPortXfmdSystem) {
    return;
  }
#endif
  x14c_localScale = scale;
}

#ifdef TARGET_PC
// A point is born at the system translation plus POFS, both in the system's axes. Spreading POFS
// and the translation's offset from the parent's (SSPO) moves where the trail starts without
// widening its ribbon.
void CParticleSwoosh::PortSetSpawnSpread(const CVector3f& spread, const CVector3f& origin) {
  xPortSpawnSpread = spread;
  xPortSpawnOrigin = origin;
}
#endif

int CParticleSwoosh::GetParticleCount() const { return x1ac_particleCount; }

void CParticleSwoosh::SetModulationColor(const CColor& col) { x20c_moduColor = col; }

const CColor& CParticleSwoosh::GetModulationColor() const { return x20c_moduColor; }

bool CParticleSwoosh::IsSystemDeletable() const {
  if (x1d0_24_emitting && x28_curFrame < x2c_PSLT) {
    return false;
  }

  if (GetParticleCount() >= 2) {
    return false;
  }

  return true;
}

void CParticleSwoosh::Render() {
#ifdef TARGET_PC
  PortFx::RenderScope fxScope(x1c_desc->xPortUnfogged);
  PortBeamColours::Scope beamScope(xPortBeamTint);
  // CreateSWSH never reads DBSO: a swoosh keeps CParticleDescription's default (0x288314), on.
  CPortParticleDepthBias portDepthBias(*this, x1c_desc->xPortDepthBias, true);
  if (PortFx::gMuteActive && PortFx::IsMuted(*this)) {
    return;
  }
  const CVector3f portGlobalTranslation = xa4_globalTranslation;
  const CTransform4f portGlobalOrientation = xb0_globalOrientation;
  // A trail drawn with its actor (ball tail, pirate slashes) is in world
  // space: the actor's rigid shift would drag its old segments back and forth
  // every tick, so it drops that shift and blends its own segments instead;
  // when it can't blend (snap, no previous tick) it follows the actor as before.
  CPortRenderScopeSuspend portScopeSuspend(PortDebug::ParticleInterpolation() &&
                                           CCameraManager::GetPresentationInterpolation() >= 0.f);
  const bool portPresenting = PortBeginPresent();
  if (!portPresenting)
    portScopeSuspend.Resume();
  CPortVarScope portVarScope(&xPortVars);
#endif
  if (x1b4_LENG >= 2 && x1ac_particleCount > 1) {
    CStopwatch timer;
    CParticleGlobals::SetParticleLifetime(x1b4_LENG);
    CGlobalRandom random(x1c0_rand);
    CGraphics::DisableAllLights();
    CGraphics::SetDepthWriteMode(true, x1c_desc->xPortNoZTest ? kE_Always : kE_LEqual, x1c_desc->x45_24_ZBUF);
    if (x1d0_25_AALP) {
      CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_One, kLO_Clear);
    } else {
      CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
    }
    CGraphics::SetModelMatrix(CTransform4f::Translate(xa4_globalTranslation) *
                              xb0_globalOrientation * xec_scaleXf *
                              CTransform4f::Scale(x14c_localScale));
    CGraphics::SetCullMode(kCM_None);
#ifdef TARGET_PC
    if (PortVfxSwoosh()) {
      PortRenderVfx();
      CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);
    } else
#endif
    {
    if (x1c_desc->x3c_TEXR) {
      {
        TToken< CTexture > tex = x1c_desc->x3c_TEXR->GetValueTexture(x28_curFrame);
        tex->Load(GX_TEXMAP0, CTexture::kCM_Repeat);
        x1e4_tex = tex.operator->();
      }
      SUVElementSet uvs;
      x1c_desc->x3c_TEXR->GetValueUV(x28_curFrame, uvs);
      x1d4_uMin = uvs.xMin;
      x1d8_vMin = uvs.yMin;
      x1dc_uMax = uvs.xMax;
      x1e0_vMax = uvs.yMax;
      x1d0_31_constantTex = x1c_desc->x3c_TEXR->HasConstantTexture();
      x1d1_24_constantUv = x1c_desc->x3c_TEXR->HasConstantUV();
      if (x1c_desc->x40_TSPN) {
        x1c_desc->x40_TSPN->GetValue(x28_curFrame, x1ec_TSPN);
      }
      if (x1ec_TSPN <= 0) {
        x1ec_TSPN = x15c_swooshes.size() - 1;
      }
      x1e8_uvSpan = 1.f;
      if (x1ec_TSPN > 0) {
        x1e8_uvSpan = 1.f / x1ec_TSPN;
      }
      CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
    } else {
      CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvPassthru);
    }
    CGraphics::SetTevOp(kTS_Stage1, CGraphics::kEnvPassthru);
    if (x1b8_SIDE == 2) {
      if (x1b0_SPLN <= 0) {
        if (x1d0_27_renderGaps) {
          Render2SidedNoSplineGaps();
        } else {
          Render2SidedNoSplineNoGaps();
        }
      } else {
        Render2SidedSpline();
      }
    } else if (x1b8_SIDE == 3) {
      if (x1b0_SPLN > 0) {
        Render3SidedSolidSpline();
      } else {
        Render3SidedSolidNoSplineNoGaps();
      }
    } else {
      if (x1b0_SPLN > 0) {
        RenderNSidedSpline();
      } else {
        RenderNSidedNoSpline();
      }
    }
    }
    x1c8_ = timer.GetElapsedTime();
    CGraphics::SetCullMode(kCM_Front);
  }
#ifdef TARGET_PC
  if (portPresenting) {
    PortEndPresent();
    xa4_globalTranslation = portGlobalTranslation;
    xb0_globalOrientation = portGlobalOrientation;
  }
#endif
}

#ifdef TARGET_PC
// Draws the swoosh between its last two tick states when uncapped. Segments
// are matched by age (head - index), so an emitter's trail slides along
// instead of growing a segment per tick; swooshes their owner writes directly
// (grapple beam, Wave Beam) never step, where age and slot are the same.
// Render drops an enclosing actor shift first (CPortRenderScopeSuspend).
bool CParticleSwoosh::PortBeginPresent() {
  const float t = CCameraManager::GetPresentationInterpolation();
  if (t < 0.f || t >= 1.f || !PortDebug::ParticleInterpolation() ||
      CActor::PortRenderScopeActive())
    return false;
  const int count = x15c_swooshes.size();
  if (count == 0 || x158_curParticle < 0 || x158_curParticle >= count)
    return false;
  const uint gen = CActor::PortTickGeneration();
  if (xPortState.curGen != gen) {
    SPortState now;
    now.segs.resize(count);
    for (int i = 0; i < count; ++i) {
      const SSwooshData& swoosh = x15c_swooshes[i];
      SPortSegment& seg = now.segs[i];
      seg.pos = swoosh.mTranslation;
      seg.useOffset = swoosh.mUseOffset;
      seg.rot = swoosh.mInitialRot + swoosh.mRotm;
      seg.active = swoosh.mActive;
    }
    now.head = x158_curParticle;
    now.frame = x28_curFrame;
    now.global = CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation;
    xPortState.Note(now, gen);
  }
  if (!xPortState.Valid(gen))
    return false;
  const SPortState& prev = xPortState.prev;
  const int frames = x28_curFrame - prev.frame;
  if (prev.segs.size() != count || frames < 0 || frames > 2)
    return false;
  // Owner-written swooshes (no steps) move whole segments per tick, and the
  // grapple beam extends fast; stepped trails use the actor snap rule.
  const float maxJumpSq = frames == 0 ? 256.f : 16.f;
  xPortSaved.resize(count);
  for (int i = 0; i < count; ++i) {
    SSwooshData& swoosh = x15c_swooshes[i];
    SPortSegment& saved = xPortSaved[i];
    saved.pos = swoosh.mTranslation;
    saved.useOffset = swoosh.mUseOffset;
    saved.rot = swoosh.mInitialRot;
    saved.active = swoosh.mActive;
    if (!swoosh.mActive)
      continue;
    int age = x158_curParticle - i;
    if (age < 0)
      age += count;
    int from = prev.head - age;
    if (from < 0)
      from += count;
    const SPortSegment& before = prev.segs[from];
    if (!before.active)
      continue;
    const CVector3f delta =
        (swoosh.mTranslation + swoosh.mUseOffset) - (before.pos + before.useOffset);
    if (delta.MagSquared() > maxJumpSq)
      continue;
    swoosh.mTranslation = CVector3f::Lerp(before.pos, swoosh.mTranslation, t);
    swoosh.mUseOffset = CVector3f::Lerp(before.useOffset, swoosh.mUseOffset, t);
    // Spin (degrees) takes the short way round; a half-turn or more cuts.
    float spin = swoosh.mInitialRot + swoosh.mRotm - before.rot;
    spin -= 360.f * floorf(spin / 360.f + 0.5f);
    if (fabsf(spin) <= 90.f)
      swoosh.mInitialRot -= (1.f - t) * spin;
  }
  const CTransform4f global =
      CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation;
  CTransform4f blend = CTransform4f::Identity();
  CTransform4f cur = CTransform4f::Identity();
  if (!(prev.global == global) && CActor::PortBlendRigid(prev.global, global, t, blend, cur)) {
    xa4_globalTranslation = blend.GetTranslation();
    xb0_globalOrientation =
        blend.GetRotation() * cur.GetRotation().GetQuickInverse() * xb0_globalOrientation;
  }
  return true;
}

void CParticleSwoosh::PortEndPresent() {
  const int count = rstl::min_val< int >(x15c_swooshes.size(), xPortSaved.size());
  for (int i = 0; i < count; ++i) {
    SSwooshData& swoosh = x15c_swooshes[i];
    const SPortSegment& saved = xPortSaved[i];
    if (!saved.active)
      continue;
    swoosh.mTranslation = saved.pos;
    swoosh.mUseOffset = saved.useOffset;
    swoosh.mInitialRot = saved.rot;
  }
}

static inline float fast_sine(float x);
static inline float fast_cosine(float x);

bool CParticleSwoosh::PortVfxSwoosh() const {
  const CPortVfxData* vfx = x1c_desc->xPortVfx.get();
  return vfx != nullptr && vfx->mat.Supported() && x1b8_SIDE >= 2 && !x1c_desc->x44_29_WIRE;
}

// TXUP (UpdateRenderState 0x27e6b0): with a live point, the uv rect's u bounds move back by
// (created % n) / n, n = TSPN, else max(ring size - 1, 1), so the texture stays put along the trail
// as points are added.
float CParticleSwoosh::PortTexUScroll() const {
  if (!x1c_desc->xPortTexUScroll || x1ac_particleCount < 1) {
    return 0.f;
  }
  int n = 0;
  if (x1c_desc->x40_TSPN) {
    x1c_desc->x40_TSPN->GetValue(x28_curFrame, n);
  }
  if (n <= 0) {
    n = rstl::max_val(static_cast< int >(x15c_swooshes.size()) - 1, 1);
  }
  return static_cast< float >(xPortCreated % n) / static_cast< float >(n);
}

// RenderNSidedSpline's geometry through the swoosh's VFX material (VMAT). The base uv is
// retail's (u along the trail, TSPN segments per repeat; v 0..1 across), run through VTMT.
// Colour, ITEN and the VPMT rows run from the segment's point to the next older one, as
// Remastered evaluates them per point.
// The four spline control points of the segment curIdx -> curIdx - 1, as UpdateRenderData_Swoosh
// 0x2b52e0 picks them: the outer neighbours only when alive and emitted one tick apart (+0x58),
// else the segment's own end. Retail's RenderNSidedSpline also pulls buffer slot 0 in for the
// second-oldest segment, which bends that segment toward an arbitrary trail point; Remastered
// has no such case.
int CParticleSwoosh::PortSplineRef(int curIdx, int j) {
  const int cur = WrapIndex(curIdx);
  const int prev = WrapIndex(curIdx - 1);
  auto adjacent = [&](int idx, int to) {
    const SSwooshData& s = x15c_swooshes.data()[idx];
    const int gap = s.mStartFrame - x15c_swooshes.data()[to].mStartFrame;
    return s.mActive && (gap == 1 || gap == -1);
  };
  switch (j) {
  case 0: {
    const int next = WrapIndex(curIdx + 1);
    return adjacent(next, cur) ? next : cur;
  }
  case 1:
    return cur;
  case 2:
    return prev;
  default: {
    const int older = WrapIndex(curIdx - 2);
    return adjacent(older, prev) ? older : prev;
  }
  }
}

// The spline steps of the segment curIdx -> prevIdx. SPLN, unless Remastered's SNRD or SNRA is
// set (SetupSplineNodeGenerationMode 0x29bfac); then UpdateRenderData_Swoosh 0x2b5a78 takes the
// larger of round(SNRD * the segment's length) and round(SNRA * (1 - dot(q, q')^2)) of its
// endpoint orientations.
int CParticleSwoosh::PortSplineSteps(int curIdx, int prevIdx) const {
  const CSwooshDescription& d = **x1c_desc;
  if (!(d.xPortSnrd > 0.f) && !(d.xPortSnra > 0.f)) {
    return x1b0_SPLN;
  }
  const SSwooshData& cur = x15c_swooshes.data()[curIdx];
  const SSwooshData& prev = x15c_swooshes.data()[prevIdx];
  const float dist = (prev.mTranslation - cur.mTranslation).Magnitude();
  const float dot = CQuaternion::Dot(CQuaternion::FromMatrix(cur.mOrientation), CQuaternion::FromMatrix(prev.mOrientation));
  const int byDist = static_cast< int >(floorf(d.xPortSnrd * dist + 0.5f));
  const int byTurn = static_cast< int >(floorf(d.xPortSnra * (1.f - dot * dot) + 0.5f));
  return byTurn < byDist ? byDist : byTurn;
}

void CParticleSwoosh::PortRenderVfx() {
  const CPortVfxData& vfx = *x1c_desc->xPortVfx;
  float vsmt[kPortVfxVsmtCount] = {};
  const uint vsmtMask = PortVfxEvalVsmt(vfx, x28_curFrame, vsmt);
  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(vfx, vsmt, vsmtMask, desc)) {
    return; // a texture is not streamed in yet
  }
  if (xPortVfxPts.size() != size_t(x15c_swooshes.capacity())) {
    return; // not updated yet
  }
  // DVVN/IEXP: UpdateRenderData_Swoosh 0x2b6820 scales ITEN by the context's inverse exposure
  // (ctx+0x18, resolve_inverse_exposure 0x20eba0: auto when blended, static when opaque); the
  // tonemap's frame exposure undoes it, not the GX tone's inherited w.
  float exposureGain = 1.f;
  if (x1c_desc->xPortExposurePolicy) {
    desc.alreadyExposed = false;
    desc.exposureOverride = PortVfxFrameExposureOverride();
    if (x1c_desc->xPortInverseExposure) {
      exposureGain =
          PortRoomEnv::ParticleInverseExposure(vfx.mat.blend != uint32_t(aurora::gfx::vfx::Blend::Opaque));
    }
  }
  if (x1c_desc->x40_TSPN) {
    x1c_desc->x40_TSPN->GetValue(x28_curFrame, x1ec_TSPN);
  }
  if (x1ec_TSPN <= 0) {
    x1ec_TSPN = x15c_swooshes.size() - 1;
  }
  const float uvSpan = x1ec_TSPN > 0 ? 1.f / x1ec_TSPN : 1.f;
  const float uScroll = PortTexUScroll();
  CPortVfxUvXf uvXf;
  uvXf.Eval(vfx, x28_curFrame);
  const float modu[4] = {x20c_moduColor.GetRed(), x20c_moduColor.GetGreen(), x20c_moduColor.GetBlue(),
                         x20c_moduColor.GetAlpha()};

  const bool cros = x1c_desc->x44_25_CROS && x1b8_SIDE % 2 == 0;
  // Remastered builds one strip per segment below SIDE 3 (0x2b52e0), not a quad per ring edge.
  const int faces = x1b8_SIDE < 3 ? 1 : (cros ? x1b8_SIDE / 2 : x1b8_SIDE);
  const int strips = x1c_desc->xPortLwqd;
  // ORNT with SIDE 2: the ribbon width axis is rebuilt from the camera and the trail direction
  // every frame (CERotationFromAxis), not taken from the stored orientation.
  const bool orntView = x1c_desc->x45_25_ORNT && x1b8_SIDE == 2;
  CVector3f camSys = CVector3f::Zero();
  if (orntView) {
    const CTransform4f inverse =
        (CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation * xec_scaleXf).GetInverse();
    camSys = (inverse * CGraphics::GetViewMatrix()).GetTranslation();
  }
  auto sysVec = [](const CVector3f& v) { return PortSwooshRing::Vec{v.GetX(), v.GetY(), v.GetZ()}; };
  const float sideDiv = 360.f / x1b8_SIDE;
  std::vector< CVector3f > ring[4];
  for (std::vector< CVector3f >& r : ring) {
    r.resize(x1b8_SIDE);
  }
  std::vector< aurora::gfx::vfx::Vertex > verts;
  auto emit = [&](const CVector3f& pos, float u, float v, const float color[4], const float extra[4][4]) {
    aurora::gfx::vfx::Vertex out{};
    out.pos[0] = pos.GetX();
    out.pos[1] = pos.GetY();
    out.pos[2] = pos.GetZ();
    uvXf.Apply(u, v, out.uv);
    for (int c = 0; c < 4; ++c) {
      out.color[c] = color[c];
      for (int k = 0; k < 4; ++k) {
        out.extra[c][k] = extra[c][k];
      }
    }
    out.vec[2] = 1.f;
    verts.push_back(out);
  };

  int curIdx = x158_curParticle;
  for (int i = 0; i < x15c_swooshes.size() - 1; ++i) {
    const int prevIdx = WrapIndex(curIdx - 1);
    const bool prevActive = x15c_swooshes[prevIdx].mActive;
    const bool active = x15c_swooshes[WrapIndex(curIdx)].mActive;
    if (!active || !prevActive) {
      if (--curIdx < 0) {
        curIdx = x15c_swooshes.size() - 1;
      }
      continue;
    }
    for (int j = 0; j < 4; ++j) {
      const int refIdx = PortSplineRef(curIdx, j);
      const SSwooshData& swoosh = x15c_swooshes.data()[refIdx];
      if (orntView) {
        const int age = WrapIndex(x158_curParticle - refIdx);
        const int olderIdx = WrapIndex(refIdx - 1);
        const int newerIdx = WrapIndex(refIdx + 1);
        const SSwooshData* older =
            age < x1b4_LENG - 1 && x15c_swooshes.data()[olderIdx].mActive ? &x15c_swooshes.data()[olderIdx] : nullptr;
        const SSwooshData* newer =
            age > 0 && x15c_swooshes.data()[newerIdx].mActive ? &x15c_swooshes.data()[newerIdx] : nullptr;
        const PortSwooshRing::Vec pos = sysVec(swoosh.mTranslation);
        const PortSwooshRing::Vec olderPos = older ? sysVec(older->mTranslation) : pos;
        const PortSwooshRing::Vec newerPos = newer ? sysVec(newer->mTranslation) : pos;
        const PortSwooshRing::Vec axis =
            PortSwooshRing::WidthAxis(pos, sysVec(camSys), older ? &olderPos : nullptr, newer ? &newerPos : nullptr);
        // PSCL's X scales the ring's local X (0x2b607c), the width axis here.
        const float scaleX = swoosh.mPortScale.GetX();
        PortSwooshRing::Vec v0, v1;
        PortSwooshRing::Ring(sysVec(swoosh.mTranslation + swoosh.mUseOffset), axis, swoosh.mLeftRad * scaleX,
                             swoosh.mRightRad * scaleX, v0, v1);
        ring[j][0] = CVector3f(v0.x, v0.y, v0.z);
        ring[j][1] = CVector3f(v1.x, v1.y, v1.z);
        continue;
      }
      for (int k = 0; k < x1b8_SIDE; ++k) {
        const float sideAngle = sideDiv * k;
        const float rawAngle = M_PIF * (sideAngle + (swoosh.mInitialRot + swoosh.mRotm)) / 180.f;
        const float angle = fabs(rawAngle) > M_PIF ? CMath::WrapPi(rawAngle) : rawAngle;
        const float radius = sideAngle > 0.f && sideAngle <= 180.f ? swoosh.mLeftRad : swoosh.mRightRad;
        // 0x2b607c scales the local ring (r cos, r sin, 0) by PSCL before q; the port's ring is
        // m * (r cos, 0, r sin) with m = Q * Rx(-90), so Remastered's local Y is the port's Z.
        const CVector3f& scale = swoosh.mPortScale;
        ring[j][k] = swoosh.mOrientation * CVector3f(radius * fast_cosine(angle) * scale.GetX(), 0.f,
                                                     radius * fast_sine(angle) * scale.GetY()) +
                     swoosh.mTranslation + swoosh.mUseOffset;
      }
    }

    const SSwooshData& a = x15c_swooshes.data()[curIdx];
    const SSwooshData& b = x15c_swooshes.data()[prevIdx];
    const SPortVfxPoint& pa = xPortVfxPts[curIdx];
    const SPortVfxPoint& pb = xPortVfxPts[prevIdx];
    auto pointAt = [&](float t, float color[4], float extra[4][4]) {
      const float iten = (pa.iten + (pb.iten - pa.iten) * t) * exposureGain;
      const float* ca = pa.color;
      const float* cb = pb.color;
      for (int c = 0; c < 4; ++c) {
        color[c] = (ca[c] + (cb[c] - ca[c]) * t) * (c < 3 ? iten : 1.f) * modu[c];
        for (int k = 0; k < 4; ++k) {
          extra[c][k] = pa.vpmt[c][k] + (pb.vpmt[c][k] - pa.vpmt[c][k]) * t;
        }
      }
    };

    float uMin = (x1ec_TSPN > 0 ? i % x1ec_TSPN : i) * uvSpan - uScroll;
    const int steps = PortSplineSteps(curIdx, prevIdx) + 1;
    const float segUvSpan = uvSpan / steps;
    for (int splineIdx = 0; splineIdx < steps; ++splineIdx) {
      const float t0 = splineIdx / static_cast< float >(steps);
      const float t1 = (splineIdx + 1) / static_cast< float >(steps);
      const float uMax = uMin + segUvSpan;
      float c0[4], c1[4], e0[4][4], e1[4][4];
      pointAt(t0, c0, e0);
      pointAt(t1, c1, e1);
      for (int face = 0; face < faces; ++face) {
        const int other = cros ? face + x1b8_SIDE / 2 : (face + 1) % x1b8_SIDE;
        const CVector3f a0 = GetSplinePoint(ring[0][face], ring[1][face], ring[2][face], ring[3][face], t0);
        const CVector3f b0 = GetSplinePoint(ring[0][other], ring[1][other], ring[2][other], ring[3][other], t0);
        const CVector3f a1 = GetSplinePoint(ring[0][face], ring[1][face], ring[2][face], ring[3][face], t1);
        const CVector3f b1 = GetSplinePoint(ring[0][other], ring[1][other], ring[2][other], ring[3][other], t1);
        // LWQD: the face's edge points are lerped across in n steps, position and v alike
        // (0x2b6a58 calling 0x2b6c70 at t = 0, then t += 1/n n times).
        const float step = 1.f / static_cast< float >(strips);
        float s1 = 0.f;
        for (int q = 0; q < strips; ++q) {
          const float s0 = s1;
          s1 += step;
          emit(CVector3f::Lerp(a0, b0, s0), uMin, s0, c0, e0);
          emit(CVector3f::Lerp(a0, b0, s1), uMin, s1, c0, e0);
          emit(CVector3f::Lerp(a1, b1, s1), uMax, s1, c1, e1);
          emit(CVector3f::Lerp(a1, b1, s0), uMax, s0, c1, e1);
        }
      }
      uMin = uMax;
    }
    if (--curIdx < 0) {
      curIdx = x15c_swooshes.size() - 1;
    }
  }
  // draw_quads drops a draw past its limit, so a long trail goes in batches.
  constexpr size_t kMaxQuadsPerDraw = 16384;
  const size_t quads = verts.size() / 4;
  for (size_t first = 0; first < quads; first += kMaxQuadsPerDraw) {
    const uint32_t count = static_cast< uint32_t >(std::min(quads - first, kMaxQuadsPerDraw));
    PortFx::gQuads += count;
    ++PortFx::gDraws;
    aurora::gfx::vfx::draw_quads(desc, verts.data() + first * 4, count);
  }
}
#endif

static inline float fast_sine(float x) {
  float c1 = 0.99980587f;
  float c3 = -0.16621658f;
  float c5 = 0.008087108f;
  float c7 = -0.000152977f;
  const float x2 = x * x;
  float power = x;
  float result = power * c1;
  power *= x2;
  result += power * c3;
  power *= x2;
  result += power * c5;
  power *= x2;
  result += power * c7;
  return result;
}

static inline float fast_cosine(float x) {
  float c0 = 1.f;
  float c2 = -0.49998003f;
  float c4 = 0.041620344f;
  float c6 = -0.0013636103f;
  float c8 = 0.000020169435f;
  const float x2 = x * x;
  float power = x2;
  float result = c0;
  result += power * c2;
  power *= x2;
  result += power * c4;
  power *= x2;
  result += power * c6;
  power *= x2;
  result += power * c8;
  return result;
}

void CParticleSwoosh::Render2SidedNoSplineNoGaps() {
  CGraphics::StreamBegin(kP_TriangleStrip);
  int particleCount = x1ac_particleCount;
  int curIdx = x158_curParticle;
  float uvOffset = 0.f;
  if (x1c_desc->x3c_TEXR) {
    if (x1c_desc->x45_25_ORNT) {
      const CTransform4f inverse =
          (CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation * xec_scaleXf)
              .GetInverse();
      const CTransform4f camera = inverse * CGraphics::GetViewMatrix();
      const CVector3f camToParticle = camera.GetTranslation();
      CVector3f dotVec = CVector3f::Zero();
      for (int i = 0; i < x15c_swooshes.size(); ++i) {
        const SSwooshData& swoosh = x15c_swooshes[curIdx];
        --curIdx;
        if (curIdx < 0) {
          curIdx = x15c_swooshes.size() - 1;
        }
        if (swoosh.mActive) {
          --particleCount;
          int otherIdx = curIdx - 1;
          if (otherIdx < 0) {
            otherIdx = x15c_swooshes.size() - 1;
          }
          const SSwooshData& otherSwoosh = x15c_swooshes[otherIdx];
          CVector3f delta = otherSwoosh.mTranslation - swoosh.mTranslation;
          if (otherIdx == x158_curParticle) {
            delta = swoosh.mTranslation -
                    x15c_swooshes[(curIdx + 1) % x15c_swooshes.size()].mTranslation;
          }
          if (delta.CanBeNormalized()) {
            CVector3f deltaCross = CVector3f::Cross(delta, camToParticle - swoosh.mTranslation);
            if (deltaCross.CanBeNormalized()) {
              deltaCross.Normalize();
              dotVec = (CVector3f::Dot(deltaCross, dotVec) < 0.f ? -1.f : 1.f) * deltaCross;
              const CVector3f v0 =
                  (swoosh.mTranslation + swoosh.mUseOffset) + swoosh.mLeftRad * dotVec;
              const CVector3f v1 =
                  (swoosh.mTranslation + swoosh.mUseOffset) + -1.f * (swoosh.mRightRad * dotVec);
              const uint color = CColor::Modulate(swoosh.mColor, x20c_moduColor).GetColor_u32();
              CGraphics::StreamColor(color);
              CGraphics::StreamTexcoord(uvOffset, x1d8_vMin);
              CGraphics::StreamVertex(v0);
              CGraphics::StreamTexcoord(uvOffset, x1e0_vMax);
              CGraphics::StreamVertex(v1);
              if (uvOffset >= 1.f && particleCount) {
                CGraphics::StreamEnd();
                CGraphics::StreamBegin(kP_TriangleStrip);
                uvOffset -= 1.f;
                CGraphics::StreamColor(color);
                CGraphics::StreamTexcoord(uvOffset, x1d8_vMin);
                CGraphics::StreamVertex(v0);
                CGraphics::StreamTexcoord(uvOffset, x1e0_vMax);
                CGraphics::StreamVertex(v1);
              }
            }
          }
          if (x1ec_TSPN > 0) {
            uvOffset += x1e8_uvSpan;
          } else {
            uvOffset = x1e8_uvSpan * CCast::ToReal32(i);
          }
        }
      }
    } else {
      for (int i = 0; i < x15c_swooshes.size(); ++i) {
        const SSwooshData& swoosh = x15c_swooshes[curIdx];
        --curIdx;
        if (curIdx < 0) {
          curIdx = x15c_swooshes.size() - 1;
        }
        if (swoosh.mActive) {
          --particleCount;
          float angle = M_PIF * (swoosh.mInitialRot + swoosh.mRotm) / 180.f;
          angle = fabs(angle) > M_PIF ? CMath::WrapPi(angle) : angle;
          float cosine = fast_cosine(angle);
          float sine = fast_sine(angle);
          float leftRadius = swoosh.mLeftRad;
          float rightRadius = swoosh.mRightRad;
          float leftSin = leftRadius;
          leftSin *= sine;
          float rightSin = rightRadius;
          rightSin *= -sine;
          const CVector3f v0 = swoosh.mOrientation * CVector3f(leftRadius * cosine, 0.f, leftSin) +
                               swoosh.mTranslation + swoosh.mUseOffset;
          const CVector3f v1 =
              swoosh.mOrientation * CVector3f(rightRadius * -cosine, 0.f, rightSin) +
              swoosh.mTranslation + swoosh.mUseOffset;
          const uint color = CColor::Modulate(swoosh.mColor, x20c_moduColor).GetColor_u32();
          CGraphics::StreamColor(color);
          CGraphics::StreamTexcoord(uvOffset, x1d8_vMin);
          CGraphics::StreamVertex(v0);
          CGraphics::StreamTexcoord(uvOffset, x1e0_vMax);
          CGraphics::StreamVertex(v1);
          if (uvOffset >= 1.f && particleCount) {
            CGraphics::StreamEnd();
            CGraphics::StreamBegin(kP_TriangleStrip);
            uvOffset -= 1.f;
            CGraphics::StreamColor(color);
            CGraphics::StreamTexcoord(uvOffset, x1d8_vMin);
            CGraphics::StreamVertex(v0);
            CGraphics::StreamTexcoord(uvOffset, x1e0_vMax);
            CGraphics::StreamVertex(v1);
          }
          if (x1ec_TSPN > 0) {
            uvOffset += x1e8_uvSpan;
          } else {
            uvOffset = x1e8_uvSpan * CCast::ToReal32(i);
          }
        }
      }
    }
  } else {
    for (int i = 0; i < x15c_swooshes.size(); ++i) {
      const SSwooshData& swoosh = x15c_swooshes.data()[curIdx];
      --curIdx;
      if (curIdx < 0) {
        curIdx = x15c_swooshes.size() - 1;
      }
      if (swoosh.mActive) {
        float angle = M_PIF * (swoosh.mInitialRot + swoosh.mRotm) / 180.f;
        angle = fabs(angle) > M_PIF ? CMath::WrapPi(angle) : angle;
        float cosine = fast_cosine(angle);
        float sine = fast_sine(angle);
        float leftRadius = swoosh.mLeftRad;
        float rightRadius = swoosh.mRightRad;
        float leftSin = leftRadius;
        leftSin *= sine;
        float rightSin = rightRadius;
        rightSin *= -sine;
        const CVector3f v0 = swoosh.mOrientation * CVector3f(leftRadius * cosine, 0.f, leftSin) +
                             swoosh.mTranslation + swoosh.mUseOffset;
        const CVector3f v1 = swoosh.mOrientation * CVector3f(rightRadius * -cosine, 0.f, rightSin) +
                             swoosh.mTranslation + swoosh.mUseOffset;
        CGraphics::StreamColor(CColor::Modulate(swoosh.mColor, x20c_moduColor).GetColor_u32());
        CGraphics::StreamVertex(v0);
        CGraphics::StreamVertex(v1);
      }
    }
  }
  CGraphics::StreamEnd();
}

void CParticleSwoosh::Render2SidedNoSplineGaps() {
  int i = 0;
  bool streaming = false;
  int curIdx = x158_curParticle;
  for (i = 0; i < x15c_swooshes.size(); ++i) {
    const SSwooshData& swoosh = x15c_swooshes[curIdx];
    const SSwooshData& next = x15c_swooshes[WrapIndex(curIdx - 1)];
    --curIdx;
    if (curIdx < 0) {
      curIdx = x15c_swooshes.size() - 2;
    }
    if (!swoosh.mActive) {
      if (streaming == true) {
        streaming = false;
        CGraphics::StreamEnd();
      }
      continue;
    }
    if (!streaming) {
      if (next.mActive != true || i >= x15c_swooshes.size() - 2) {
        continue;
      }
      streaming = true;
      CGraphics::StreamBegin(kP_TriangleStrip);
    }
    float angle = (swoosh.mInitialRot + swoosh.mRotm) * (M_PIF / 180.f);
    angle = fabs(angle) > M_PIF ? CMath::WrapPi(angle) : angle;
    float cosine = fast_cosine(angle);
    float sine = fast_sine(angle);
    float leftRadius = swoosh.mLeftRad;
    float rightRadius = swoosh.mRightRad;
    float leftSin = leftRadius;
    leftSin *= sine;
    float leftCos = leftRadius;
    leftCos *= cosine;
    float rightSin = rightRadius;
    rightSin *= -sine;
    const CVector3f v0 = swoosh.mOrientation * CVector3f(leftCos, 0.f, leftSin) +
                         swoosh.mTranslation + swoosh.mUseOffset;
    const CVector3f v1 = swoosh.mOrientation * CVector3f(rightRadius * -cosine, 0.f, rightSin) +
                         swoosh.mTranslation + swoosh.mUseOffset;
    CGraphics::StreamColor(CColor::Modulate(swoosh.mColor, x20c_moduColor).GetColor_u32());
    CGraphics::StreamTexcoord(1.f, x1d8_vMin);
    CGraphics::StreamVertex(v0);
    CGraphics::StreamTexcoord(1.f, x1e0_vMax);
    CGraphics::StreamVertex(v1);
    if (i < x15c_swooshes.size() - 1) {
      CGraphics::StreamTexcoord(0.f, x1d8_vMin);
      CGraphics::StreamVertex(v0);
      CGraphics::StreamTexcoord(0.f, x1e0_vMax);
      CGraphics::StreamVertex(v1);
    }
  }
  if (streaming == true) {
    CGraphics::StreamEnd();
  }
}

void CParticleSwoosh::Render2SidedSpline() { RenderNSidedSpline(); }

void CParticleSwoosh::Render3SidedSolidNoSplineNoGaps() {
  if (x15c_swooshes.size() < 2) {
    return;
  }
  rstl::reserved_vector< CVector3f, 2 > p0;
  p0.push_back(CVector3f::Zero());
  p0.push_back(CVector3f::Zero());
  rstl::reserved_vector< CVector3f, 2 > p1;
  p1.push_back(CVector3f::Zero());
  p1.push_back(CVector3f::Zero());
  rstl::reserved_vector< CVector3f, 2 > p2;
  p2.push_back(CVector3f::Zero());
  p2.push_back(CVector3f::Zero());
  static const GXVtxDescList skDescList[] = {{GX_VA_POS, GX_DIRECT},
                                             {GX_VA_CLR0, GX_DIRECT},
                                             {GX_VA_TEX0, GX_DIRECT},
                                             {GX_VA_NULL, GX_NONE}};
  CGX::SetVtxDescv(skDescList);
  CGX::SetNumChans(1);
  CGX::SetNumTexGens(1);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  CGX::SetChanCtrl(CGX::Channel0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                   GX_AF_NONE);
  CGX::SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, false, GX_PTIDENTITY);
  int cur = 0;
  int prev = 1;
  uint prevColor;
  uint color = 0;
  int curIdx = x158_curParticle;
  bool lastActive = false;
  float uv = -x1e8_uvSpan;
  for (int i = 0; i < x15c_swooshes.size(); ++i) {
    const SSwooshData& swoosh = x15c_swooshes.data()[curIdx];
    --curIdx;
    if (curIdx < 0) {
      curIdx = x15c_swooshes.size() - 1;
    }
    float rawAngle = M_PIF * (swoosh.mInitialRot + swoosh.mRotm) / 180.f;
    float angle = fabs(rawAngle) > M_PIF ? CMath::WrapPi(rawAngle) : rawAngle;
    const float radius = swoosh.mLeftRad;
    const float cosinea = fast_cosine(angle);
    const float sinea = fast_sine(angle);
    const CVector3f a(radius * cosinea, 0.f, radius * sinea);
    angle += 2.0943952f;
    if (angle > M_PIF) {
      angle -= M_2PIF;
    }
    const float cosineb = fast_cosine(angle);
    const float sineb = fast_sine(angle);
    const CVector3f b(radius * cosineb, 0.f, radius * sineb);
    angle += 2.0943952f;
    if (angle > M_PIF) {
      angle -= M_2PIF;
    }
    const float cosinec = fast_cosine(angle);
    const float sinec = fast_sine(angle);
    const CVector3f c(radius * cosinec, 0.f, radius * sinec);
    const CVector3f offset = swoosh.mTranslation + swoosh.mUseOffset;
    p0[cur] = swoosh.mOrientation * a + offset;
    p1[cur] = swoosh.mOrientation * b + offset;
    p2[cur] = swoosh.mOrientation * c + offset;
    prevColor = color;
    if (!swoosh.mActive) {
      lastActive = false;
    } else if (!lastActive) {
      lastActive = true;
    } else {
      lastActive = true;
      color = CColor::Modulate(swoosh.mColor, x20c_moduColor).GetColor_u32();
      const float prevUv = uv;
      uv += x1e8_uvSpan;
      CGX::Begin(GX_QUADS, GX_VTXFMT0, 12);
      GXPosition3f32(p0[cur].GetX(), p0[cur].GetY(), p0[cur].GetZ());
      GXColor1u32(color);
      GXTexCoord2f32(uv, x1d8_vMin);
      GXPosition3f32(p1[cur].GetX(), p1[cur].GetY(), p1[cur].GetZ());
      GXColor1u32(color);
      GXTexCoord2f32(uv, x1e0_vMax);
      GXPosition3f32(p1[prev].GetX(), p1[prev].GetY(), p1[prev].GetZ());
      GXColor1u32(prevColor);
      GXTexCoord2f32(prevUv, x1e0_vMax);
      GXPosition3f32(p0[prev].GetX(), p0[prev].GetY(), p0[prev].GetZ());
      GXColor1u32(prevColor);
      GXTexCoord2f32(prevUv, x1d8_vMin);
      GXPosition3f32(p1[cur].GetX(), p1[cur].GetY(), p1[cur].GetZ());
      GXColor1u32(color);
      GXTexCoord2f32(uv, x1d8_vMin);
      GXPosition3f32(p2[cur].GetX(), p2[cur].GetY(), p2[cur].GetZ());
      GXColor1u32(color);
      GXTexCoord2f32(uv, x1e0_vMax);
      GXPosition3f32(p2[prev].GetX(), p2[prev].GetY(), p2[prev].GetZ());
      GXColor1u32(prevColor);
      GXTexCoord2f32(prevUv, x1e0_vMax);
      GXPosition3f32(p1[prev].GetX(), p1[prev].GetY(), p1[prev].GetZ());
      GXColor1u32(prevColor);
      GXTexCoord2f32(prevUv, x1d8_vMin);
      GXPosition3f32(p2[cur].GetX(), p2[cur].GetY(), p2[cur].GetZ());
      GXColor1u32(color);
      GXTexCoord2f32(uv, x1d8_vMin);
      GXPosition3f32(p0[cur].GetX(), p0[cur].GetY(), p0[cur].GetZ());
      GXColor1u32(color);
      GXTexCoord2f32(uv, x1e0_vMax);
      GXPosition3f32(p0[prev].GetX(), p0[prev].GetY(), p0[prev].GetZ());
      GXColor1u32(prevColor);
      GXTexCoord2f32(prevUv, x1e0_vMax);
      GXPosition3f32(p2[prev].GetX(), p2[prev].GetY(), p2[prev].GetZ());
      GXColor1u32(prevColor);
      GXTexCoord2f32(prevUv, x1d8_vMin);
      CGX::End();
    }
    ++cur;
    ++prev;
    cur &= 1;
    prev &= 1;
  }
}

void CParticleSwoosh::Render3SidedSolidSpline() {
  if (x15c_swooshes.size() < 2) {
    return;
  }
  static const GXVtxDescList skDescList[] = {{GX_VA_POS, GX_DIRECT},
                                             {GX_VA_CLR0, GX_DIRECT},
                                             {GX_VA_TEX0, GX_DIRECT},
                                             {GX_VA_NULL, GX_NONE}};
  CGX::SetVtxDescv(skDescList);
  CGX::SetNumChans(1);
  CGX::SetNumTexGens(1);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  CGX::SetChanCtrl(CGX::Channel0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                   GX_AF_NONE);
  CGX::SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, false, GX_PTIDENTITY);
  float uvDelta;
  uint prevColor = 0;
  uint color = 0;
  int curIdx = x158_curParticle;
  float curUvSpan = -x1e8_uvSpan;
  for (int i = 0; i < x15c_swooshes.size(); ++i) {
    const SSwooshData& swoosh = x15c_swooshes.data()[curIdx];
    --curIdx;
    if (curIdx < 0) {
      curIdx = x15c_swooshes.size() - 1;
    }
    float rawAngle = M_PIF * (swoosh.mInitialRot + swoosh.mRotm) / 180.f;
    float angle = fabs(rawAngle) > M_PIF ? CMath::WrapPi(rawAngle) : rawAngle;
    const float radius = swoosh.mLeftRad;
    const float cosinea = fast_cosine(angle);
    const float sinea = fast_sine(angle);
    const CVector3f a(radius * cosinea, 0.f, radius * sinea);
    angle += 2.0943952f;
    if (angle > M_PIF) {
      angle -= M_2PIF;
    }
    const float cosineb = fast_cosine(angle);
    const float sineb = fast_sine(angle);
    const CVector3f b(radius * cosineb, 0.f, radius * sineb);
    angle += 2.0943952f;
    if (angle > M_PIF) {
      angle -= M_2PIF;
    }
    const float cosinec = fast_cosine(angle);
    const float sinec = fast_sine(angle);
    const CVector3f c(radius * cosinec, 0.f, radius * sinec);
    const CVector3f offset = swoosh.mTranslation + swoosh.mUseOffset;
    if (i == 2) {
      x19c_p3[0] = 2.f * x17c_p1[0] - x16c_p0[0];
      x19c_p3[1] = 2.f * x17c_p1[1] - x16c_p0[1];
      x19c_p3[2] = 2.f * x17c_p1[2] - x16c_p0[2];
    } else {
      x19c_p3[0] = x18c_p2[0];
      x19c_p3[1] = x18c_p2[1];
      x19c_p3[2] = x18c_p2[2];
    }
    x18c_p2[0] = x17c_p1[0];
    x18c_p2[1] = x17c_p1[1];
    x18c_p2[2] = x17c_p1[2];
    x17c_p1[0] = x16c_p0[0];
    x17c_p1[1] = x16c_p0[1];
    x17c_p1[2] = x16c_p0[2];
    x16c_p0[0] = swoosh.mOrientation * a + offset;
    x16c_p0[1] = swoosh.mOrientation * b + offset;
    x16c_p0[2] = swoosh.mOrientation * c + offset;
    const uint priorColor = prevColor;
    prevColor = color;
    if (swoosh.mActive) {
      const float prevUvSpan = curUvSpan;
      color = CColor::Modulate(swoosh.mColor, x20c_moduColor).GetColor_u32();
      curUvSpan += x1e8_uvSpan;
      if (i > 1) {
        float uv1 = 0.f;
        CVector3f v01 = CVector3f::Zero();
        CVector3f v11 = CVector3f::Zero();
        CVector3f v21 = CVector3f::Zero();
        uint c1 = 0;
        CGX::Begin(GX_QUADS, GX_VTXFMT0, (x1b0_SPLN + 1) * 12);
        uvDelta = prevUvSpan - curUvSpan;
        for (int j = 0; j < x1b0_SPLN + 1; ++j) {
          const float t1 = (j + 1) / static_cast< float >(x1b0_SPLN + 1);
          float uv0 = uv1;
          CVector3f v00 = v01;
          CVector3f v10 = v11;
          CVector3f v20 = v21;
          uint c0 = c1;
          if (j == 0) {
            const float t0 = j / static_cast< float >(x1b0_SPLN + 1);
            v00 = GetSplinePoint(x16c_p0[0], x17c_p1[0], x18c_p2[0], x19c_p3[0], t0);
            v10 = GetSplinePoint(x16c_p0[1], x17c_p1[1], x18c_p2[1], x19c_p3[1], t0);
            v20 = GetSplinePoint(x16c_p0[2], x17c_p1[2], x18c_p2[2], x19c_p3[2], t0);
            c0 = CColor::Lerp(prevColor, priorColor, t0);
            uv0 = t0 * uvDelta + curUvSpan;
          }
          v01 = GetSplinePoint(x16c_p0[0], x17c_p1[0], x18c_p2[0], x19c_p3[0], t1);
          v11 = GetSplinePoint(x16c_p0[1], x17c_p1[1], x18c_p2[1], x19c_p3[1], t1);
          v21 = GetSplinePoint(x16c_p0[2], x17c_p1[2], x18c_p2[2], x19c_p3[2], t1);
          c1 = CColor::Lerp(prevColor, priorColor, t1);
          uv1 = t1 * uvDelta + curUvSpan;
          GXPosition3f32(v00.GetX(), v00.GetY(), v00.GetZ());
          GXColor1u32(c0);
          GXTexCoord2f32(uv0, x1d8_vMin);
          GXPosition3f32(v10.GetX(), v10.GetY(), v10.GetZ());
          GXColor1u32(c0);
          GXTexCoord2f32(uv0, x1e0_vMax);
          GXPosition3f32(v11.GetX(), v11.GetY(), v11.GetZ());
          GXColor1u32(c1);
          GXTexCoord2f32(uv1, x1e0_vMax);
          GXPosition3f32(v01.GetX(), v01.GetY(), v01.GetZ());
          GXColor1u32(c1);
          GXTexCoord2f32(uv1, x1d8_vMin);
          GXPosition3f32(v10.GetX(), v10.GetY(), v10.GetZ());
          GXColor1u32(c0);
          GXTexCoord2f32(uv0, x1d8_vMin);
          GXPosition3f32(v20.GetX(), v20.GetY(), v20.GetZ());
          GXColor1u32(c0);
          GXTexCoord2f32(uv0, x1e0_vMax);
          GXPosition3f32(v21.GetX(), v21.GetY(), v21.GetZ());
          GXColor1u32(c1);
          GXTexCoord2f32(uv1, x1e0_vMax);
          GXPosition3f32(v11.GetX(), v11.GetY(), v11.GetZ());
          GXColor1u32(c1);
          GXTexCoord2f32(uv1, x1d8_vMin);
          GXPosition3f32(v20.GetX(), v20.GetY(), v20.GetZ());
          GXColor1u32(c0);
          GXTexCoord2f32(uv0, x1d8_vMin);
          GXPosition3f32(v00.GetX(), v00.GetY(), v00.GetZ());
          GXColor1u32(c0);
          GXTexCoord2f32(uv0, x1e0_vMax);
          GXPosition3f32(v01.GetX(), v01.GetY(), v01.GetZ());
          GXColor1u32(c1);
          GXTexCoord2f32(uv1, x1e0_vMax);
          GXPosition3f32(v21.GetX(), v21.GetY(), v21.GetZ());
          GXColor1u32(c1);
          GXTexCoord2f32(uv1, x1d8_vMin);
        }
        CGX::End();
      }
    }
  }
}

void CParticleSwoosh::RenderNSidedNoSpline() { RenderNSidedSpline(); }

void CParticleSwoosh::RenderNSidedSpline() {
  if (x1c_desc->x44_29_WIRE) {
    x1bc_prim = GX_LINES;
  } else {
    x1bc_prim = GX_QUADS;
  }
  CGraphics::StreamBegin(static_cast< ERglPrimitive >(x1bc_prim));
  int i = 0;
  bool cros = x1c_desc->x44_25_CROS;
  if (x1b8_SIDE < 4 || x1b8_SIDE % 2 != 0) {
    cros = false;
  }
  int faceIdx;
  int splineIdx;
  int curIdx = x158_curParticle;
  for (i = 0; i < x15c_swooshes.size() - 1; ++i) {
    bool prevActive = x15c_swooshes[WrapIndex(curIdx - 1)].mActive;
    bool active = x15c_swooshes[WrapIndex(curIdx)].mActive;
    if (active == false || (active == true && prevActive == false)) {
      --curIdx;
      if (curIdx < 0) {
        curIdx = x15c_swooshes.size() - 1;
      }
      continue;
    }
    const SSwooshData& refSwoosh = x15c_swooshes.data()[curIdx];
    const float sideDiv = 360.f / x1b8_SIDE;
    for (int j = 0; j < 4; ++j) {
      int refIdx = 0;
      if (j == 0) {
        refIdx = WrapIndex(curIdx + 1);
        if (!x15c_swooshes.data()[refIdx].mActive) {
          refIdx = curIdx;
        }
      } else if (j == 1) {
        refIdx = WrapIndex(curIdx);
      } else if (j == 2) {
        refIdx = WrapIndex(curIdx - 1);
      } else if (j == 3) {
        refIdx = WrapIndex(curIdx - 2);
        if (!x15c_swooshes.data()[refIdx].mActive) {
          refIdx = WrapIndex(curIdx - 1);
        }
      }
      if (x1b4_LENG == 2) {
        if (j == 0) {
          refIdx = WrapIndex(curIdx);
        } else if (j == 3) {
          refIdx = WrapIndex(curIdx - 1);
        }
      } else if (curIdx == x158_curParticle && j == 0) {
        refIdx = x158_curParticle;
      } else if (WrapIndex(x158_curParticle + 2) == curIdx && j == 3) {
        refIdx = WrapIndex(x158_curParticle + 1);
      } else if (i == x1ac_particleCount - 2 && j == 3) {
        refIdx = 0;
      }
#ifdef TARGET_PC
      if (PortIsRemastered()) {
        refIdx = PortSplineRef(curIdx, j);
      }
#endif
      int k;
      const SSwooshData& swoosh = x15c_swooshes.data()[refIdx];
      const CTransform4f& orientation = swoosh.mOrientation;
      for (k = 0; k < x1b8_SIDE; ++k) {
        const float sideAngle = sideDiv * k;
        const float rawAngle = M_PIF * (sideAngle + (swoosh.mInitialRot + swoosh.mRotm)) / 180.f;
        const float angle = fabs(rawAngle) > M_PIF ? CMath::WrapPi(rawAngle) : rawAngle;
        const float cosine = fast_cosine(angle);
        const float sine = fast_sine(angle);
        float x;
        float radius;
        if (sideAngle > 0.f && sideAngle <= 180.f) {
          radius = swoosh.mLeftRad;
        } else {
          radius = swoosh.mRightRad;
        }
        x = radius * cosine;
        const float z = radius * sine;
        const CVector3f offset = swoosh.mTranslation + swoosh.mUseOffset;
        if (j == 0) {
          x16c_p0[k] = orientation * CVector3f(x, 0.f, z) + offset;
        } else if (j == 1) {
          x17c_p1[k] = orientation * CVector3f(x, 0.f, z) + offset;
        } else if (j == 2) {
          x18c_p2[k] = orientation * CVector3f(x, 0.f, z) + offset;
        } else if (j == 3) {
          x19c_p3[k] = orientation * CVector3f(x, 0.f, z) + offset;
        }
      }
    }
    if (x1c_desc->x3c_TEXR) {
      if (x1ec_TSPN > 0) {
        x1d4_uMin = (i % x1ec_TSPN) * x1e8_uvSpan;
      } else {
        x1d4_uMin = i * x1e8_uvSpan;
      }
    }
    const float segUvSpan = x1e8_uvSpan / (x1b0_SPLN + 1);
    for (splineIdx = 0; splineIdx < x1b0_SPLN + 1; ++splineIdx) {
      const float t0 = splineIdx / static_cast< float >(x1b0_SPLN + 1);
      const float t1 = (splineIdx + 1) / static_cast< float >(x1b0_SPLN + 1);
      int faces = x1b8_SIDE;
      if (x1b8_SIDE <= 2) {
        faces = 1;
      } else if (cros == true) {
        faces = x1b8_SIDE / 2;
      }
      x1dc_uMax = x1d4_uMin + segUvSpan;
      for (faceIdx = 0; faceIdx < faces; ++faceIdx) {
        int other = faceIdx + 1;
        if (other >= x1b8_SIDE) {
          other = 0;
        }
        const uint color = CColor::Modulate(refSwoosh.mColor, x20c_moduColor).GetColor_u32();
        if (cros) {
          other = faceIdx + x1b8_SIDE / 2;
          const CVector3f& p00 = x16c_p0[faceIdx];
          const CVector3f& p10 = x17c_p1[faceIdx];
          const CVector3f& p20 = x18c_p2[faceIdx];
          const CVector3f& p30 = x19c_p3[faceIdx];
          const CVector3f& p01 = x16c_p0[other];
          const CVector3f& p11 = x17c_p1[other];
          const CVector3f& p21 = x18c_p2[other];
          const CVector3f& p31 = x19c_p3[other];
          const CVector3f v0 = GetSplinePoint(p00, p10, p20, p30, t0);
          const CVector3f v1 = GetSplinePoint(p01, p11, p21, p31, t0);
          const CVector3f v2 = GetSplinePoint(p01, p11, p21, p31, t1);
          const CVector3f v3 = GetSplinePoint(p00, p10, p20, p30, t1);
          CGraphics::StreamColor(color);
          CGraphics::StreamTexcoord(x1d4_uMin, x1d8_vMin);
          CGraphics::StreamVertex(v0);
          CGraphics::StreamTexcoord(x1d4_uMin, x1e0_vMax);
          CGraphics::StreamVertex(v1);
          CGraphics::StreamTexcoord(x1dc_uMax, x1e0_vMax);
          CGraphics::StreamVertex(v2);
          CGraphics::StreamTexcoord(x1dc_uMax, x1d8_vMin);
          CGraphics::StreamVertex(v3);

        } else {
          const CVector3f& p00 = x16c_p0[faceIdx];
          const CVector3f& p10 = x17c_p1[faceIdx];
          const CVector3f& p20 = x18c_p2[faceIdx];
          const CVector3f& p30 = x19c_p3[faceIdx];
          const CVector3f& p01 = x16c_p0[other];
          const CVector3f& p11 = x17c_p1[other];
          const CVector3f& p21 = x18c_p2[other];
          const CVector3f& p31 = x19c_p3[other];
          const CVector3f v0 = GetSplinePoint(p00, p10, p20, p30, t0);
          const CVector3f v1 = GetSplinePoint(p01, p11, p21, p31, t0);
          const CVector3f v2 = GetSplinePoint(p01, p11, p21, p31, t1);
          const CVector3f v3 = GetSplinePoint(p00, p10, p20, p30, t1);
          if (x1bc_prim == GX_LINES) {
            CGraphics::StreamVertex(v0);
            CGraphics::StreamVertex(v1);
            CGraphics::StreamVertex(v1);
            CGraphics::StreamVertex(v2);
            CGraphics::StreamVertex(v2);
            CGraphics::StreamVertex(v0);
            CGraphics::StreamVertex(v0);
            CGraphics::StreamVertex(v2);
            CGraphics::StreamVertex(v2);
            CGraphics::StreamVertex(v3);
            CGraphics::StreamVertex(v3);
            CGraphics::StreamVertex(v0);
          } else if (x1bc_prim == GX_QUADS) {
            CGraphics::StreamColor(color);
            CGraphics::StreamTexcoord(x1d4_uMin, x1d8_vMin);
            CGraphics::StreamVertex(v0);
            CGraphics::StreamTexcoord(x1d4_uMin, x1e0_vMax);
            CGraphics::StreamVertex(v1);
            CGraphics::StreamColor(color);
            CGraphics::StreamTexcoord(x1dc_uMax, x1e0_vMax);
            CGraphics::StreamVertex(v2);
            CGraphics::StreamTexcoord(x1dc_uMax, x1d8_vMin);
            CGraphics::StreamVertex(v3);
          }
        }
      }
      if (x1c_desc->x3c_TEXR && x1b0_SPLN > 0) {
        x1d4_uMin += segUvSpan;
      }
    }
    --curIdx;
    if (curIdx < 0) {
      curIdx = x15c_swooshes.size() - 1;
    }
  }
  CGraphics::StreamEnd();
}

void CParticleSwoosh::SetParticleEmission(const bool emission) { x1d0_24_emitting = emission; }
int CParticleSwoosh::WrapIndex(int index) {
  for (; index < 0; index += x1b4_LENG)
    ;
  for (; index >= x1b4_LENG; index -= x1b4_LENG)
    ;
  return index;
}

float CParticleSwoosh::GetLeftRadius(int index) {
  float ret = 0.f;
  if (x1c_desc->x8_LRAD) {
    CPortKeepScope scope(&x15c_swooshes[index].mPortKeep);
    x1c_desc->x8_LRAD->GetValue(x15c_swooshes[index].mFrame, ret);
  }

  return ret;
}
float CParticleSwoosh::GetRightRadius(int index) {
  float ret = 0.f;
  if (x1c_desc->xc_RRAD) {
    CPortKeepScope scope(&x15c_swooshes[index].mPortKeep);
    x1c_desc->xc_RRAD->GetValue(x15c_swooshes[index].mFrame, ret);
  }

  return ret;
}

CVector3f CParticleSwoosh::GetSplinePoint(const CVector3f& p0, const CVector3f& p1,
                                          const CVector3f& p2, const CVector3f& p3, float t) const {
  CVector3f ret(p1);
  if (!(t <= 0.f)) {
    if (t >= 1.f) {
      ret = p2;
    } else {
      const float t2 = t * t;
      const float t3 = t2 * t;
      const float halfT3 = 0.5f * t3;
      ret = (-(halfT3) + t2 - 0.5f * t) * p0 + (1.5f * t3 + -2.5f * t2 + 1.f) * p1 +
            (-(1.5f * t3) + 2.f * t2 + 0.5f * t) * p2 + (halfT3 - 0.5f * t2) * p3;
    }
  }
  return ret;
}

rstl::optional_object< CAABox > CParticleSwoosh::GetBounds() const {
  if (GetParticleCount() <= 1) {
    return CAABox(x38_translation + xa4_globalTranslation, x38_translation + xa4_globalTranslation);
  }
  const CVector3f radius(x208_maxRadius, x208_maxRadius, x208_maxRadius);
  return CAABox(x1f0_aabbMin - radius, x1fc_aabbMax + radius)
      .GetTransformedAABox(CTransform4f::Translate(xa4_globalTranslation) * xb0_globalOrientation *
                           xec_scaleXf);
}

void CParticleSwoosh::UpdateBounds(const CVector3f& pos) {
  x1fc_aabbMax = CVector3f(rstl::max_val(pos.GetX(), x1fc_aabbMax.GetX()),
                           rstl::max_val(pos.GetY(), x1fc_aabbMax.GetY()),
                           rstl::max_val(pos.GetZ(), x1fc_aabbMax.GetZ()));
  x1f0_aabbMin = CVector3f(rstl::min_val(pos.GetX(), x1f0_aabbMin.GetX()),
                           rstl::min_val(pos.GetY(), x1f0_aabbMin.GetY()),
                           rstl::min_val(pos.GetZ(), x1f0_aabbMin.GetZ()));
}

void CParticleSwoosh::UpdateMaxRadius(float radius) {
  x208_maxRadius = rstl::max_val(x208_maxRadius, radius);
}

CLight CParticleSwoosh::GetLight() const {
  return CLight::BuildLocalAmbient(CVector3f::Zero(), CColor::White());
}

uint CParticleSwoosh::Get4CharId() const { return 'SWHC'; }

void CParticleSwoosh::UpdateSwooshTranslation(const CVector3f& translation) {
#ifdef TARGET_PC
  // SetTranslation reaches here for an undersized swoosh too (see SetOrientation).
  if (x15c_swooshes.empty()) {
    return;
  }
#endif
#ifdef TARGET_PC
  const CVector3f spawn =
      xPortSpawnSpread == CVector3f(1.f, 1.f, 1.f)
          ? translation
          : xPortSpawnOrigin +
                CVector3f::ByElementMultiply(translation - xPortSpawnOrigin, xPortSpawnSpread);
  x15c_swooshes[x158_curParticle].mTranslation = x11c_invScaleXf * spawn;
#else
  x15c_swooshes[x158_curParticle].mTranslation = x11c_invScaleXf * translation;
#endif
}

void CParticleSwoosh::SetGlobalScale(const CVector3f& scale) {
#ifdef TARGET_PC
  // XFMD 1: var b0 is SYSS's, one by default.
  if (x1c_desc->xPortXfmdSystem) {
    return;
  }
  // XFMD 4: the external scale is read by no mover, so the draw keeps unit scale (var b0 = one).
  if (x1c_desc->xPortXfmdMover) {
    return;
  }
#endif
  xe0_globalScale = scale;
  xec_scaleXf = CTransform4f::Scale(scale);
  x11c_invScaleXf = CTransform4f::Scale(1.f / scale.GetX(), 1.f / scale.GetY(), 1.f / scale.GetZ());
}

bool CParticleSwoosh::SystemHasLight() const { return false; }
void CParticleSwoosh::DestroyParticles() {}
bool CParticleSwoosh::GetParticleEmission() const { return x1d0_24_emitting; }
#ifdef TARGET_PC
// CParticleGen::ShutdownEffect (0x27a66c) -> DoShutdownBehavior (0x27b88c) with the description's
// SHTM: 0 stops the emission, 1 also runs Remastered's CParticleSwoosh::DestroyParticles (0x27f744),
// which empties the segment ring (current index, live counts to 0), so the trail vanishes at once.
// The retail DestroyParticles stays empty. With a SHTT the shutdown waits that many ticks instead,
// armed once.
void CParticleSwoosh::ShutdownEffect() {
  if (!x1c_desc->xPortShutdownTime) {
    PortDoShutdownBehavior(0xff);
    return;
  }
  if (xPortShutdownCountdown == -1) {
    CPortVarScope portVarScope(&xPortVars);
    CParticleGlobals::SetEmitterTime(x28_curFrame);
    int ticks = 0;
    x1c_desc->xPortShutdownTime->GetValue(x28_curFrame, ticks);
    xPortShutdownCountdown = ticks;
  }
}

u32 CParticleSwoosh::PortOwnShutdownMode() const { return x1c_desc->xPortShutdownMode; }

void CParticleSwoosh::PortDoShutdownBehavior(u32 mode) {
  const u32 use = mode == 0xff ? PortShutdownMode() : (mode & 0xff);
  if (use >= 2) {
    return;
  }
  SetParticleEmission(false);
  if (use == 1) {
    x158_curParticle = 0;
    x1ac_particleCount = 0;
    xPortCreated = 0;
    for (int i = 0; i < x15c_swooshes.capacity(); ++i) {
      x15c_swooshes.data()[i].mActive = false;
    }
  }
}
// XFMD 3: the getters read the one system var too (MPT_GetTranslation 0x27fb54). MPT_GetOrientation
// 0x27fb28 reads the external rotation (var bc only under XFMD 5), not INTR's; so does XFMD 1.
const CTransform4f& CParticleSwoosh::GetOrientation() const {
  if (x1c_desc->xPortIntRotation || x1c_desc->xPortXfmdSystem) {
    return xPortExtOrientation;
  }
  return x1c_desc->xPortXfmdFollow ? xb0_globalOrientation : x44_orientation;
}
#else
const CTransform4f& CParticleSwoosh::GetOrientation() const { return x44_orientation; }
#endif
const CVector3f& CParticleSwoosh::GetGlobalScale() const { return xe0_globalScale; }
const CTransform4f& CParticleSwoosh::GetGlobalOrientation() const { return xb0_globalOrientation; }
const CVector3f& CParticleSwoosh::GetGlobalTranslation() const { return xa4_globalTranslation; }
#ifdef TARGET_PC
const CVector3f& CParticleSwoosh::GetTranslation() const {
  if (x1c_desc->xPortXfmdSystem) {
    return xPortExtTranslation;
  }
  return x1c_desc->xPortXfmdFollow ? xa4_globalTranslation : x38_translation;
}
#else
const CVector3f& CParticleSwoosh::GetTranslation() const { return x38_translation; }
#endif

#ifdef TARGET_PC
bool CParticleSwoosh::PortIsRemastered() const { return x1c_desc->xPortIrnd; }

uint CParticleSwoosh::PortFxAsset() const { return CToken(x1c_desc).GetTag().GetId(); }

void CParticleSwoosh::PortFxDescribe(PortFxInfo& out) const {
  out.kind = 'SWHC';
  out.asset = PortFxAsset();
  out.particles = x1ac_particleCount;
  out.maxParticles = x1b4_LENG;
  out.frame = x28_curFrame;
  out.lifetime = x2c_PSLT;
  out.emitting = x1d0_24_emitting;
  out.deletable = IsSystemDeletable();
  out.pos[0] = xa4_globalTranslation.GetX();
  out.pos[1] = xa4_globalTranslation.GetY();
  out.pos[2] = xa4_globalTranslation.GetZ();
}
#endif
