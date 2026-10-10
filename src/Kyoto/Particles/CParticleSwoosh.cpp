#include "Kyoto/Particles/CParticleSwoosh.hpp"

#include "Kyoto/Basics/CStopwatch.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CLight.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CSwooshDescription.hpp"

#include "Kyoto/Math/CAbsAngle.hpp"
#include "dolphin/gx/GXVert.h"
#include "float.h"
#include "rstl/math.hpp"
#include "rstl/reserved_vector.hpp"

#ifdef TARGET_PC
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "port_debug.h"
#include "port_fx_debug.h"
#include "port_vfx_particles.h"
#endif

uint CParticleSwoosh::mSwooshAliveCount = 0;
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
  CPortVarScope portVarScope(&xPortVars);
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
    if (++x158_curParticle >= x15c_swooshes.capacity()) {
      x158_curParticle = 0;
    }
    if (x1d0_24_emitting && x28_curFrame < x2c_PSLT) {
      UpdateSwooshTranslation(x38_translation);
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
      x15c_swooshes[x158_curParticle].mOrientation = x44_orientation;
      if (x1c_desc->x28_IVEL) {
        x1c_desc->x28_IVEL->GetValue(x28_curFrame, x15c_swooshes[x158_curParticle].mVelocity);
        x15c_swooshes[x158_curParticle].mVelocity =
            x44_orientation * x15c_swooshes[x158_curParticle].mVelocity;
      }
      if (x1c_desc->x24_POFS) {
        x1c_desc->x24_POFS->GetValue(x28_curFrame, x15c_swooshes[x158_curParticle].mOffset);
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
    } else if (x15c_swooshes[x158_curParticle].mActive) {
      x1ac_particleCount = rstl::max_val(0, x1ac_particleCount - 1);
      x15c_swooshes[x158_curParticle].mActive = false;
    }
    UpdateTranslationAndOrientation();
    evalTime += kFrameTime;
    ++x28_curFrame;
  }
  x1c4_ = timer.GetElapsedTime();
  return false;
}

void CParticleSwoosh::SetOrientation(const CTransform4f& orientation) {
  x44_orientation = orientation;
  x74_invOrientation = x44_orientation.GetQuickInverse();
  x15c_swooshes[x158_curParticle].mOrientation = x44_orientation;
}

void CParticleSwoosh::UpdateTranslationAndOrientation() {
  if (x1ac_particleCount <= 0) {
    return;
  }
  x208_maxRadius = 0.f;
  x1f0_aabbMin = CVector3f(FLT_MAX, FLT_MAX, FLT_MAX);
  x1fc_aabbMax = CVector3f(-FLT_MAX, -FLT_MAX, -FLT_MAX);
#ifdef TARGET_PC
  // Remastered runs a swoosh point's KEWS over the live points (newest 0, oldest 1).
  CParticleGlobals::SetParticleLifetime(x1c_desc->xPortIrnd ? rstl::max_val(x1ac_particleCount - 1, 1)
                                                            : x1b4_LENG);
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
    swoosh.mFrame = x28_curFrame - swoosh.mStartFrame;
    CParticleGlobals::UpdateParticleLifetimeTweenValues(swoosh.mFrame);
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
  x38_translation = translation;
  UpdateSwooshTranslation(x38_translation);
}

void CParticleSwoosh::SetGlobalTranslation(const CVector3f& translation) {
  xa4_globalTranslation = translation;
}

void CParticleSwoosh::SetGlobalOrientation(const CTransform4f& orientation) {
  xb0_globalOrientation = orientation.GetRotation();
}

void CParticleSwoosh::SetLocalScale(const CVector3f& scale) { x14c_localScale = scale; }

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
  PortFx::RenderScope fxScope;
  if (PortFx::gMuteActive && PortFx::IsMuted(*this)) {
    return;
  }
  const CVector3f portGlobalTranslation = xa4_globalTranslation;
  const CTransform4f portGlobalOrientation = xb0_globalOrientation;
  const bool portPresenting = PortBeginPresent();
  CPortVarScope portVarScope(&xPortVars);
#endif
  if (x1b4_LENG >= 2 && x1ac_particleCount > 1) {
    CStopwatch timer;
    CParticleGlobals::SetParticleLifetime(x1b4_LENG);
    CGlobalRandom random(x1c0_rand);
    CGraphics::DisableAllLights();
    CGraphics::SetDepthWriteMode(true, kE_LEqual, x1c_desc->x45_24_ZBUF);
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
// Systems drawn inside a shifted actor draw follow the actor instead.
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
  return vfx != nullptr && vfx->mat.version == 2 && x1b8_SIDE >= 4 && !x1c_desc->x44_29_WIRE;
}

// RenderNSidedSpline's geometry through the swoosh's VFX material (VMAT). The base uv is
// retail's (u along the trail, TSPN segments per repeat; v 0..1 across), run through VTMT.
// Colour, ITEN and the VPMT rows run from the segment's point to the next older one, as
// Remastered evaluates them per point.
void CParticleSwoosh::PortRenderVfx() {
  const CPortVfxData& vfx = *x1c_desc->xPortVfx;
  float vsmt[19] = {};
  const uint vsmtMask = PortVfxEvalVsmt(vfx, x28_curFrame, vsmt);
  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(vfx, vsmt, vsmtMask, desc)) {
    return; // a texture is not streamed in yet
  }
  if (xPortVfxPts.size() != size_t(x15c_swooshes.capacity())) {
    return; // not updated yet
  }
  if (x1c_desc->x40_TSPN) {
    x1c_desc->x40_TSPN->GetValue(x28_curFrame, x1ec_TSPN);
  }
  if (x1ec_TSPN <= 0) {
    x1ec_TSPN = x15c_swooshes.size() - 1;
  }
  const float uvSpan = x1ec_TSPN > 0 ? 1.f / x1ec_TSPN : 1.f;
  CPortVfxUvXf uvXf;
  uvXf.Eval(vfx, x28_curFrame);
  const float modu[4] = {x20c_moduColor.GetRed(), x20c_moduColor.GetGreen(), x20c_moduColor.GetBlue(),
                         x20c_moduColor.GetAlpha()};

  const bool cros = x1c_desc->x44_25_CROS && x1b8_SIDE % 2 == 0;
  const int faces = cros ? x1b8_SIDE / 2 : x1b8_SIDE;
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
    // The four ring points, picked as RenderNSidedSpline does.
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
        refIdx = prevIdx;
      } else {
        refIdx = WrapIndex(curIdx - 2);
        if (!x15c_swooshes.data()[refIdx].mActive) {
          refIdx = prevIdx;
        }
      }
      if (x1b4_LENG == 2) {
        if (j == 0) {
          refIdx = WrapIndex(curIdx);
        } else if (j == 3) {
          refIdx = prevIdx;
        }
      } else if (curIdx == x158_curParticle && j == 0) {
        refIdx = x158_curParticle;
      } else if (WrapIndex(x158_curParticle + 2) == curIdx && j == 3) {
        refIdx = WrapIndex(x158_curParticle + 1);
      } else if (i == x1ac_particleCount - 2 && j == 3) {
        refIdx = 0;
      }
      const SSwooshData& swoosh = x15c_swooshes.data()[refIdx];
      for (int k = 0; k < x1b8_SIDE; ++k) {
        const float sideAngle = sideDiv * k;
        const float rawAngle = M_PIF * (sideAngle + (swoosh.mInitialRot + swoosh.mRotm)) / 180.f;
        const float angle = fabs(rawAngle) > M_PIF ? CMath::WrapPi(rawAngle) : rawAngle;
        const float radius = sideAngle > 0.f && sideAngle <= 180.f ? swoosh.mLeftRad : swoosh.mRightRad;
        ring[j][k] = swoosh.mOrientation * CVector3f(radius * fast_cosine(angle), 0.f, radius * fast_sine(angle)) +
                     swoosh.mTranslation + swoosh.mUseOffset;
      }
    }

    const SSwooshData& a = x15c_swooshes.data()[curIdx];
    const SSwooshData& b = x15c_swooshes.data()[prevIdx];
    const SPortVfxPoint& pa = xPortVfxPts[curIdx];
    const SPortVfxPoint& pb = xPortVfxPts[prevIdx];
    auto pointAt = [&](float t, float color[4], float extra[4][4]) {
      const float iten = pa.iten + (pb.iten - pa.iten) * t;
      const float* ca = pa.color;
      const float* cb = pb.color;
      for (int c = 0; c < 4; ++c) {
        color[c] = (ca[c] + (cb[c] - ca[c]) * t) * (c < 3 ? iten : 1.f) * modu[c];
        for (int k = 0; k < 4; ++k) {
          extra[c][k] = pa.vpmt[c][k] + (pb.vpmt[c][k] - pa.vpmt[c][k]) * t;
        }
      }
    };

    float uMin = (x1ec_TSPN > 0 ? i % x1ec_TSPN : i) * uvSpan;
    const float segUvSpan = uvSpan / (x1b0_SPLN + 1);
    for (int splineIdx = 0; splineIdx < x1b0_SPLN + 1; ++splineIdx) {
      const float t0 = splineIdx / static_cast< float >(x1b0_SPLN + 1);
      const float t1 = (splineIdx + 1) / static_cast< float >(x1b0_SPLN + 1);
      const float uMax = uMin + segUvSpan;
      float c0[4], c1[4], e0[4][4], e1[4][4];
      pointAt(t0, c0, e0);
      pointAt(t1, c1, e1);
      for (int face = 0; face < faces; ++face) {
        const int other = cros ? face + x1b8_SIDE / 2 : (face + 1) % x1b8_SIDE;
        emit(GetSplinePoint(ring[0][face], ring[1][face], ring[2][face], ring[3][face], t0), uMin, 0.f, c0, e0);
        emit(GetSplinePoint(ring[0][other], ring[1][other], ring[2][other], ring[3][other], t0), uMin, 1.f, c0, e0);
        emit(GetSplinePoint(ring[0][other], ring[1][other], ring[2][other], ring[3][other], t1), uMax, 1.f, c1, e1);
        emit(GetSplinePoint(ring[0][face], ring[1][face], ring[2][face], ring[3][face], t1), uMax, 0.f, c1, e1);
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
    x1c_desc->x8_LRAD->GetValue(x15c_swooshes[index].mFrame, ret);
  }

  return ret;
}
float CParticleSwoosh::GetRightRadius(int index) {
  float ret = 0.f;
  if (x1c_desc->xc_RRAD) {
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
  x15c_swooshes[x158_curParticle].mTranslation = x11c_invScaleXf * translation;
}

void CParticleSwoosh::SetGlobalScale(const CVector3f& scale) {
  xe0_globalScale = scale;
  xec_scaleXf = CTransform4f::Scale(scale);
  x11c_invScaleXf = CTransform4f::Scale(1.f / scale.GetX(), 1.f / scale.GetY(), 1.f / scale.GetZ());
}

bool CParticleSwoosh::SystemHasLight() const { return false; }
void CParticleSwoosh::DestroyParticles() {}
bool CParticleSwoosh::GetParticleEmission() const { return x1d0_24_emitting; }
const CTransform4f& CParticleSwoosh::GetOrientation() const { return x44_orientation; }
const CVector3f& CParticleSwoosh::GetGlobalScale() const { return xe0_globalScale; }
const CTransform4f& CParticleSwoosh::GetGlobalOrientation() const { return xb0_globalOrientation; }
const CVector3f& CParticleSwoosh::GetGlobalTranslation() const { return xa4_globalTranslation; }
const CVector3f& CParticleSwoosh::GetTranslation() const { return x38_translation; }

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
