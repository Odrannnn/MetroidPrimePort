#include "MetroidPrime/CTargetReticles.hpp"

#include "port_third_person.h"

#include "MetroidPrime/CEulerAngles.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/CCollisionActor.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/ScriptObjects/CScriptGrapplePoint.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Tweaks/CTweakGuiColors.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"
#include "MetroidPrime/Tweaks/CTweakTargeting.hpp"

#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CWorld.hpp"

#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Basics/CCast.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CloseEnough.hpp"

#include "MetaRender/CCubeRenderer.hpp"

#include "rstl/math.hpp"

#include "MetroidPrime/SFX/UI.h"

#ifdef TARGET_PC
#include "port_debug.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "port_mouse.h"
#include "port_disc.h"
#include "Collision/CMaterialFilter.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CRayCastResult.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPointOfInterest.hpp"
#endif

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef TARGET_PC
namespace {
// Reticles are drawn in the view the world is drawn from (third person moves it behind the
// shoulder; PortThirdPerson::View is the game camera otherwise).
CTransform4f PortReticleCameraXf(const CStateManager& mgr) {
  return PortThirdPerson::View(mgr.GetCameraManager()->GetCurrentCameraTransform(mgr));
}
} // namespace
#else
#define PortReticleCameraXf(mgr) (mgr).GetCameraManager()->GetCurrentCameraTransform(mgr)
#endif

static const char skCrosshairsReticleAssetName[] = "CMDL_Crosshairs";
static const char skOrbitZoneReticleAssetName[] = "CMDL_OrbitZone";
static const char skSeekerAssetName[] = "CMDL_Seeker";
static const char skLockConfirmAssetName[] = "CMDL_LockConfirm";
static const char skTargetFlowerAssetName[] = "CMDL_TargetFlower";
static const char skMissileBracketAssetName[] = "CMDL_MissileBracket";
static const char skChargeGaugeAssetName[] = "CMDL_ChargeGauge";
static const char skChargeBeamTickAssetName[] = "CMDL_ChargeTickFirst";
static const char skOuterBeamIconSquareNameBase[] = "CMDL_BeamSquare";
static const char skInnerBeamIconName[] = "CMDL_InnerBeamIcon";
static const char skLockFireAssetName[] = "CMDL_LockFire";
static const char skLockDaggerAssetName[] = "CMDL_LockDagger0";
static const char skGrappleReticleAssetName[] = "CMDL_Grapple";
static const char skXRayRingModelName[] = "CMDL_XRayRetRing";
static const char skThermalReticleAssetName[] = "CMDL_ThermalRet";
static const char skOrbitPointAssetName[] = "CMDL_OrbitPoint";
#ifdef TARGET_PC
static const char skTriScanRingAssetName[] = "CMDL_ScanReticleRing";
static const char skTriScanBracketAssetName[] = "CMDL_ScanReticleBracket";
static const char skTriScanProgressAssetName[] = "CMDL_ScanReticleProgress";
#endif

static const float gkEpsilon = FLT_EPSILON;

static CTargetReticleRenderState skZeroRenderState(kInvalidUniqueId, 1.f, CVector3f::Zero(), 0.f,
                                                   1.f, true);

static bool IsDamageOrbit(CPlayer::EOrbitBrokenType type) {
  switch (type) {
  case CPlayer::kOB_Five:
  case CPlayer::kOB_ActivateOrbitSource:
  case CPlayer::kOB_ProjectileCollide:
  case CPlayer::kOB_Freeze:
  case CPlayer::kOB_DamageOnGrapple:
    return true;
  default:
    return false;
  }
}

static float offshoot_func(float f1, float f2, float f3) {
  return f1 * CMath::FastSinR((f3 - 0.5f) * f2) + 0.5f;
}

static float calculate_premultiplied_overshoot_offset(float f) {
  float x = static_cast< float >(asin(static_cast< double >(1.f / f)));
  return 2.f * (M_PIF - x);
}

CCompoundTargetReticle::SOuterItemInfo::SOuterItemInfo(const char* modelName)
: x0_model(gpSimplePool->GetObj(modelName))
, xc_offshootBaseAngle(0.f)
, x10_rotAng(0.f)
, x14_baseAngle(0.f)
, x18_offshootAngleDelta(0.f) {}

CCompoundTargetReticle::CCompoundTargetReticle(const CStateManager& mgr)
: x0_leadingOrientation(
      CQuaternion::FromMatrix(mgr.GetCameraManager()->GetCurrentCamera(mgr).GetTransform()))
, x10_laggingOrientation(
      CQuaternion::FromMatrix(mgr.GetCameraManager()->GetCurrentCamera(mgr).GetTransform()))
, x20_prevState(kRS_Unspecified)
, x24_nextState(kRS_Unspecified)
, x28_noDrawTicks(0)
, x2c_overshootOffsetHalf(0.5f * gpTweakTargeting->xe8_chargeGaugeOvershootOffset)
, x30_premultOvershootOffset(
      calculate_premultiplied_overshoot_offset(gpTweakTargeting->xe8_chargeGaugeOvershootOffset))
, x34_crosshairs(gpSimplePool->GetObj(skCrosshairsReticleAssetName))
, x40_seeker(gpSimplePool->GetObj(skSeekerAssetName))
, x4c_lockConfirm(gpSimplePool->GetObj(skLockConfirmAssetName))
, x58_targetFlower(gpSimplePool->GetObj(skTargetFlowerAssetName))
, x64_missileBracket(gpSimplePool->GetObj(skMissileBracketAssetName))
, x70_innerBeamIcon(gpSimplePool->GetObj(skInnerBeamIconName))
, x7c_lockFire(gpSimplePool->GetObj(skLockFireAssetName))
, x88_lockDagger(gpSimplePool->GetObj(skLockDaggerAssetName))
, x94_grapple(gpSimplePool->GetObj(skGrappleReticleAssetName))
, xa0_chargeTickFirst(gpSimplePool->GetObj(skChargeBeamTickAssetName))
, xac_xrayRetRing(gpSimplePool->GetObj(skXRayRingModelName))
, xb8_thermalReticle(gpSimplePool->GetObj(skThermalReticleAssetName))
, xc4_chargeGauge(skChargeGaugeAssetName)
, xf0_targetId(kInvalidUniqueId)
, xf2_nextTargetId(kInvalidUniqueId)
, xf4_targetPos(CalculateOrbitZoneReticlePosition(mgr, false))
, x100_laggingTargetPos(CalculateOrbitZoneReticlePosition(mgr, true))
, x10c_currGroupInterp(skZeroRenderState)
, x12c_currGroupA(skZeroRenderState)
, x14c_currGroupB(skZeroRenderState)
, x16c_currGroupDur(0.f)
, x170_currGroupTimer(0.f)
, x174_nextGroupInterp(skZeroRenderState)
, x194_nextGroupA(skZeroRenderState)
, x1b4_nextGroupB(skZeroRenderState)
, x1d4_nextGroupDur(0.f)
, x1d8_nextGroupTimer(0.f)
, x1dc_grapplePoint0(kInvalidUniqueId)
, x1de_grapplePoint1(kInvalidUniqueId)
, x1e0_grapplePoint0T(0.f)
, x1e4_grapplePoint1T(0.f)
, x1e8_crosshairsScale(0.f)
, x1ec_seekerAngle(0.f)
, x1f0_xrayRetAngle(0.f)
, x1f4_missileActive(false)
, x1f8_missileBracketTimer(0.f)
, x1fc_missileBracketScaleTimer(0.f)
, x200_beam(CPlayerState::kBI_Power)
, x204_chargeGaugeOvershootTimer(0.f)
, x208_lockonTimer(gpTweakTargeting->xd0_lockonDuration)
, x20c_unk(0.f)
, x210_lockFireTimer(0.f)
, x214_fullChargeFadeTimer(0.f)
, x218_beamShot(false)
, x219_missileShot(false)
, x21a_fullyCharged(false) {
  xe0_outerBeamIconSquares.reserve(9);
  for (int i = 0; i < 9; ++i) {
    char buf[64];
    sprintf(buf, "%s%d", skOuterBeamIconSquareNameBase, i);
    xe0_outerBeamIconSquares.push_back(SOuterItemInfo(buf));
  }
  x34_crosshairs.Lock();
#ifdef TARGET_PC
  // Trilogy ctor: tokens at this+0x3E0/0x3EC/0x3F8 (aram:RS5).
  if (PortDisc::IsTrilogy(PortDisc::Current())) {
    xPortTri = true;
    x3e0_scanRing = gpSimplePool->GetObj(skTriScanRingAssetName);
    x3ec_scanBracket = gpSimplePool->GetObj(skTriScanBracketAssetName);
    x3f8_scanProgress = gpSimplePool->GetObj(skTriScanProgressAssetName);
  }
#endif
}

bool CCompoundTargetReticle::CheckLoadComplete() { return true; }

EReticleState CCompoundTargetReticle::GetDesiredReticleState(const CStateManager& mgr) const {
  switch (mgr.GetPlayerState()->GetCurrentVisor()) {
  case CPlayerState::kPV_Scan:
    return kRS_Scan;
  case CPlayerState::kPV_XRay:
    return kRS_XRay;
  case CPlayerState::kPV_Combat:
    return kRS_Combat;
  case CPlayerState::kPV_Thermal:
    return kRS_Thermal;
  default:
    return kRS_Combat;
  }
}

void CCompoundTargetReticle::Update(float dt, const CStateManager& mgr) {
  // 1. Orientation slerp
  CRelAngle angle = x10_laggingOrientation.AngleFrom(x0_leadingOrientation);
  float angleDeg = angle.AsDegrees();
  bool extreme = false;
  if (angleDeg < 0.1f || angleDeg > 45.f) {
    extreme = true;
  }
  float t;
  if (extreme) {
    t = 1.f;
  } else {
    float lagSpeed = gpTweakTargeting->x224_angularLagSpeed;
    t = rstl::min_val(1.f, lagSpeed * dt / angleDeg);
  }
  x10_laggingOrientation =
      t == 1.f ? x0_leadingOrientation
               : CQuaternion::Slerp(x10_laggingOrientation, x0_leadingOrientation, t);

  // 2. Target positions
  xf4_targetPos = CalculateOrbitZoneReticlePosition(mgr, false);
  x100_laggingTargetPos = CalculateOrbitZoneReticlePosition(mgr, true);

  // 3. Sub-updates
#ifdef TARGET_PC
  // Trilogy fn_80235DB0 runs fn_8023BB0C before the lock-on groups and fn_8023BF48 after.
  if (xPortTri) {
    PortTriUpdateScanPointer(mgr);
    PortTriUpdateScan(dt, mgr);
  }
#endif
  UpdateCurrLockOnGroup(dt, mgr);
  UpdateNextLockOnGroup(dt, mgr);
#ifdef TARGET_PC
  if (xPortTri) {
    PortTriUpdateScanBounds(mgr);
  }
#endif
  UpdateOrbitZoneGroup(dt, mgr);
#ifdef TARGET_PC
  if (PortDebug::CrosshairStyle() == PortDebug::kCrosshairStyle_Remastered) {
    PortUpdateRemasteredCrosshair(dt, mgr);
  }
#endif

  // 4. Reticle state transitions
  EReticleState desiredState = GetDesiredReticleState(mgr);
  if (desiredState != x20_prevState && x20_prevState == x24_nextState) {
    x24_nextState = desiredState;
    x28_noDrawTicks = 2;
  }

  if (x20_prevState != x24_nextState && x28_noDrawTicks <= 0) {
    x20_prevState = x24_nextState;
    bool combat = false;
    bool scan = false;
    bool xray = false;
    bool thermal = false;
    switch (x24_nextState) {
    case kRS_Combat:
      combat = true;
      break;
    case kRS_Scan:
      scan = true;
      break;
    case kRS_XRay:
      xray = true;
      break;
    case kRS_Thermal:
      thermal = true;
      break;
    default:
      break;
    }

    if (combat) {
      x40_seeker.Lock();
    } else {
      x40_seeker.Unlock();
    }
    if (combat) {
      x4c_lockConfirm.Lock();
    } else {
      x4c_lockConfirm.Unlock();
    }
    if (combat) {
      x58_targetFlower.Lock();
    } else {
      x58_targetFlower.Unlock();
    }
    if (combat) {
      x64_missileBracket.Lock();
    } else {
      x64_missileBracket.Unlock();
    }
    if (combat) {
      x70_innerBeamIcon.Lock();
    } else {
      x70_innerBeamIcon.Unlock();
    }
    if (combat) {
      x7c_lockFire.Lock();
    } else {
      x7c_lockFire.Unlock();
    }
    if (combat) {
      x88_lockDagger.Lock();
    } else {
      x88_lockDagger.Unlock();
    }
    if (combat) {
      xa0_chargeTickFirst.Lock();
    } else {
      xa0_chargeTickFirst.Unlock();
    }
    if (xray) {
      xac_xrayRetRing.Lock();
    } else {
      xac_xrayRetRing.Unlock();
    }
    if (thermal) {
      xb8_thermalReticle.Lock();
    } else {
      xb8_thermalReticle.Unlock();
    }
    if (combat) {
      xc4_chargeGauge.x0_model.Lock();
    } else {
      xc4_chargeGauge.x0_model.Unlock();
    }
    if (scan) {
      x94_grapple.Unlock();
    } else {
      x94_grapple.Lock();
    }
#ifdef TARGET_PC
    if (xPortTri) {
      if (scan) {
        x3e0_scanRing.Lock();
        x3ec_scanBracket.Lock();
        x3f8_scanProgress.Lock();
      } else {
        x3e0_scanRing.Unlock();
        x3ec_scanBracket.Unlock();
        x3f8_scanProgress.Unlock();
      }
    }
#endif
    for (AUTO(it, xe0_outerBeamIconSquares.begin()); it != xe0_outerBeamIconSquares.end(); ++it) {
      if (combat) {
        it->x0_model.Lock();
      } else {
        it->x0_model.Unlock();
      }
    }
  }

  // 5. Charge gauge / fully charged
  bool fullyCharged = mgr.GetPlayer()->GetPlayerGun()->GetChargePercentage() >= 1.f;
  if (fullyCharged != x21a_fullyCharged) {
    x21a_fullyCharged = fullyCharged;
  }
  if (x21a_fullyCharged) {
    x214_fullChargeFadeTimer = rstl::min_val(
        gpTweakTargeting->x1b8_fullChargeFadeDuration,
        x214_fullChargeFadeTimer + dt / gpTweakTargeting->x1b8_fullChargeFadeDuration);
  } else {
    x214_fullChargeFadeTimer = rstl::max_val(
        0.f, x214_fullChargeFadeTimer - dt / gpTweakTargeting->x1b8_fullChargeFadeDuration);
  }

  // 6. Missile active state
  bool missileActive = mgr.GetPlayer()->GetPlayerGun()->GetMissileMode() == CPlayerGun::kMM_Active;
  if (missileActive != x1f4_missileActive) {
    if (x1f8_missileBracketTimer != 0.f) {
      x1f8_missileBracketTimer = FLT_EPSILON - x1f8_missileBracketTimer;
    } else {
      x1f8_missileBracketTimer = FLT_EPSILON;
    }
    x1f4_missileActive = missileActive;
  }

  // 7. Beam change
  CPlayerState::EBeamId beam = mgr.GetPlayer()->GetPlayerGun()->GetPrimaryWeaponId();
  if (beam != x200_beam) {
    x204_chargeGaugeOvershootTimer = gpTweakTargeting->xec_chargeGaugeOvershootDuration;
    for (int i = 0; i < 9; ++i) {
      SOuterItemInfo& icon = xe0_outerBeamIconSquares[i];
      float baseAngle = CMath::ClampRadians(gpTweakTargeting->xf8_outerBeamSquareAngles[beam][i]);
      CRelAngle offshootAngleDelta(baseAngle - icon.x10_rotAng);
      if (i % 2 == 1) {
        offshootAngleDelta = offshootAngleDelta.AsRadians() > 0.f
                                 ? CRelAngle(-1.f * (M_2PIF - offshootAngleDelta.AsRadians()))
                                 : CRelAngle(M_2PIF + offshootAngleDelta.AsRadians());
      }
      icon.xc_offshootBaseAngle = icon.x10_rotAng;
      icon.x18_offshootAngleDelta = offshootAngleDelta.AsRadians();
      icon.x14_baseAngle = baseAngle;
    }

    float chargeBaseAngle = CMath::ClampRadians(gpTweakTargeting->x108_chargeGaugeAngles[beam]);
    bool odd = rand() % 2 == 1;
    CRelAngle chargeOffshootAngleDelta(chargeBaseAngle - xc4_chargeGauge.x10_rotAng);
    if (odd) {
      chargeOffshootAngleDelta =
          chargeOffshootAngleDelta.AsRadians() > 0.f
              ? CRelAngle(-1.f * (M_2PIF - chargeOffshootAngleDelta.AsRadians()))
              : CRelAngle(M_2PIF + chargeOffshootAngleDelta.AsRadians());
    }
    xc4_chargeGauge.xc_offshootBaseAngle = xc4_chargeGauge.x10_rotAng;
    xc4_chargeGauge.x18_offshootAngleDelta = chargeOffshootAngleDelta.AsRadians();
    xc4_chargeGauge.x14_baseAngle = chargeBaseAngle;
    x200_beam = beam;
    x208_lockonTimer = 0.f;
  }

  // 8. Beam shot / lock fire
  const CPlayerGun* gun = mgr.GetPlayer()->GetPlayerGun();
  if (gun->GetFiring() & 0x1) {
    if (!x218_beamShot) {
      x210_lockFireTimer = gpTweakTargeting->x12c_lockFireDuration;
    }
    x218_beamShot = true;
  } else {
    x218_beamShot = false;
  }

  // 9. Missile shot / missile bracket scale
  if (gun->GetFiring() & 0x2) {
    if (!x219_missileShot) {
      x1fc_missileBracketScaleTimer = gpTweakTargeting->xc8_missileBracketScaleDuration;
    }
    x219_missileShot = true;
  } else {
    x219_missileShot = false;
  }

  // 10. Grapple point tracking
  const CScriptGrapplePoint* castResult = TCastToConstPtr< CScriptGrapplePoint >(
      mgr.GetObjectListById(kOL_All).GetObjectById(xf2_nextTargetId));
  const CScriptGrapplePoint* grapplePoint = nullptr;
  if (xf2_nextTargetId != kInvalidUniqueId) {
    grapplePoint = castResult;
  }
  if (grapplePoint != nullptr) {
    TUniqueId gpId = grapplePoint->GetUniqueId();
    if (gpId != x1dc_grapplePoint0) {
      float tmp;
      if (gpId == x1de_grapplePoint1) {
        tmp = rstl::max_val(gkEpsilon, x1e4_grapplePoint1T);
      } else {
        tmp = FLT_EPSILON;
      }
      x1de_grapplePoint1 = x1dc_grapplePoint0;
      x1e4_grapplePoint1T = x1e0_grapplePoint0T;
      x1e0_grapplePoint0T = tmp;
      x1dc_grapplePoint0 = gpId;
    }
  } else {
    if (x1dc_grapplePoint0 != kInvalidUniqueId) {
      x1de_grapplePoint1 = x1dc_grapplePoint0;
      x1e4_grapplePoint1T = x1e0_grapplePoint0T;
      x1e0_grapplePoint0T = 0.f;
      x1dc_grapplePoint0 = kInvalidUniqueId;
    }
  }

  // 11. Grapple point interpolation timers
  if (x1e0_grapplePoint0T > 0.f) {
    x1e0_grapplePoint0T = rstl::min_val(1.f, x1e0_grapplePoint0T + dt / 0.5f);
  }
  if (x1e4_grapplePoint1T > 0.f) {
    x1e4_grapplePoint1T = rstl::max_val(0.f, x1e4_grapplePoint1T - dt / 0.5f);
    if (x1e4_grapplePoint1T == 0.f) {
      x1de_grapplePoint1 = kInvalidUniqueId;
    }
  }

  // 12. Xray/seeker angle updates
  x1f0_xrayRetAngle = CMath::ClampRadians(
      x1f0_xrayRetAngle +
      CRelAngle::FromDegrees(dt * gpTweakTargeting->x2c_xrayRetAngleSpeed).AsRadians());
  x1ec_seekerAngle = CMath::ClampRadians(
      x1ec_seekerAngle +
      CRelAngle::FromDegrees(dt * gpTweakTargeting->x28_seekerAngleSpeed).AsRadians());
}

void CCompoundTargetReticle::UpdateCurrLockOnGroup(float dt, const CStateManager& mgr) {
  const CPlayer* player = mgr.GetPlayer();
  const TUniqueId targetId = player->GetOrbitTargetId();

  if (targetId != xf0_targetId) {
    if (xf0_targetId != targetId && targetId != kInvalidUniqueId) {
      if (TCastToConstPtr< CScriptGrapplePoint >(mgr.GetObjectById(targetId))) {
        CSfxManager::SfxStart(SFXui_x_graplock_00);
      } else {
        CSfxManager::SfxStart(SFXui_x_lockon_00);
      }
    }

    if (kInvalidUniqueId == targetId) {
      CPlayer::EOrbitBrokenType orbitBrokenType = player->GetOrbitBrokenType();
      x12c_currGroupA = x10c_currGroupInterp;
      x12c_currGroupA.SetIsOrbitZoneIdlePosition(false);
      x14c_currGroupB.SetFactor(0.f);
      x16c_currGroupDur =
          IsDamageOrbit(orbitBrokenType) ? 0.65f : gpTweakTargeting->xc_currLockOnEnterDuration;
    } else {
      x12c_currGroupA = x10c_currGroupInterp;
      x12c_currGroupA.SetIsOrbitZoneIdlePosition(false);
      if (xf0_targetId == kInvalidUniqueId) {
        x12c_currGroupA.SetTargetId(targetId);
      }
      float scale =
          IsGrappleTarget(targetId, mgr) ? gpTweakTargeting->x1b0_grappleMinClampScale : 1.f;
      x14c_currGroupB =
          CTargetReticleRenderState(targetId, 1.f, CVector3f::Zero(), 1.f, scale, false);
      x16c_currGroupDur = (kInvalidUniqueId == xf0_targetId)
                              ? gpTweakTargeting->x8_currLockOnExitDuration
                              : gpTweakTargeting->x10_currLockOnSwitchDuration;
    }

    x170_currGroupTimer = x16c_currGroupDur;
    xf0_targetId = targetId;
  }

  if (x170_currGroupTimer > 0.f) {
    UpdateTargetParameters(x12c_currGroupA, mgr);
    UpdateTargetParameters(x14c_currGroupB, mgr);
    x170_currGroupTimer = rstl::max_val(0.f, x170_currGroupTimer - dt);
    CTargetReticleRenderState::InterpolateWithClamp(x12c_currGroupA, x10c_currGroupInterp,
                                                    x14c_currGroupB,
                                                    1.f - x170_currGroupTimer / x16c_currGroupDur);
  } else {
    UpdateTargetParameters(x10c_currGroupInterp, mgr);
  }

  if (x1f8_missileBracketTimer != 0.f &&
      x1f8_missileBracketTimer < gpTweakTargeting->xbc_missileBracketDuration) {
    if (x1f8_missileBracketTimer < 0.f) {
      x1f8_missileBracketTimer = rstl::min_val(x1f8_missileBracketTimer + dt, 0.f);
    } else {
      x1f8_missileBracketTimer = rstl::min_val(x1f8_missileBracketTimer + dt,
                                               gpTweakTargeting->xbc_missileBracketDuration);
    }
  }

  if (x204_chargeGaugeOvershootTimer > 0.f) {
    x204_chargeGaugeOvershootTimer = rstl::max_val(x204_chargeGaugeOvershootTimer - dt, 0.f);
    if (x204_chargeGaugeOvershootTimer == 0.f) {
      for (int i = 0; i < 9; ++i) {
        xe0_outerBeamIconSquares[i].x10_rotAng = xe0_outerBeamIconSquares[i].x14_baseAngle;
      }
      xc4_chargeGauge.x10_rotAng = xc4_chargeGauge.x14_baseAngle;
      x208_lockonTimer = FLT_EPSILON;
    } else {
      float offshoot = offshoot_func(x2c_overshootOffsetHalf, x30_premultOvershootOffset,
                                     1.f - x204_chargeGaugeOvershootTimer /
                                               gpTweakTargeting->xec_chargeGaugeOvershootDuration);
      for (int i = 0; i < 9; ++i) {
        SOuterItemInfo& item = xe0_outerBeamIconSquares[i];
        float angleDelta = offshoot * item.x18_offshootAngleDelta;
        item.x10_rotAng = CMath::ClampRadians(angleDelta + item.xc_offshootBaseAngle);
      }
      xc4_chargeGauge.x10_rotAng = CMath::ClampRadians(
          xc4_chargeGauge.xc_offshootBaseAngle + offshoot * xc4_chargeGauge.x18_offshootAngleDelta);
    }
  }

  if (x208_lockonTimer > 0.f && x208_lockonTimer < gpTweakTargeting->xd0_lockonDuration) {
    x208_lockonTimer = rstl::min_val(x208_lockonTimer + dt, gpTweakTargeting->xd0_lockonDuration);
  }

  if (x210_lockFireTimer > 0.f) {
    x210_lockFireTimer = rstl::max_val(0.f, x210_lockFireTimer - dt);
  }

  if (x1fc_missileBracketScaleTimer > 0.f) {
    x1fc_missileBracketScaleTimer = rstl::max_val(0.f, x1fc_missileBracketScaleTimer - dt);
  }
}

void CCompoundTargetReticle::UpdateNextLockOnGroup(float dt, const CStateManager& mgr) {
  const CPlayer* player = mgr.GetPlayer();
  TUniqueId nextTargetId = player->GetOrbitNextTargetId();
  if (mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_Scan &&
      player->GetOrbitTargetId() != kInvalidUniqueId) {
    nextTargetId = player->GetOrbitTargetId();
  }

  if (nextTargetId != xf2_nextTargetId) {
    if (kInvalidUniqueId == nextTargetId) {
      x194_nextGroupA = x174_nextGroupInterp;
      x194_nextGroupA.SetIsOrbitZoneIdlePosition(false);
      bool lag = (x20_prevState == kRS_XRay || x20_prevState == kRS_Thermal);
      x1b4_nextGroupB = CTargetReticleRenderState(
          kInvalidUniqueId, 1.f, lag ? x100_laggingTargetPos : xf4_targetPos, 0.f, 1.f, true);
      x1d4_nextGroupDur = gpTweakTargeting->x1c_nextLockOnExitDuration;
      x1d8_nextGroupTimer = x1d4_nextGroupDur;
      xf2_nextTargetId = nextTargetId;
    } else {
      x194_nextGroupA = x174_nextGroupInterp;
      x194_nextGroupA.SetIsOrbitZoneIdlePosition(false);
      float scale =
          IsGrappleTarget(nextTargetId, mgr) ? gpTweakTargeting->x1b0_grappleMinClampScale : 1.f;
      x1b4_nextGroupB =
          CTargetReticleRenderState(nextTargetId, 1.f, CVector3f::Zero(), 1.f, scale, true);
      x1d4_nextGroupDur = (kInvalidUniqueId == xf2_nextTargetId)
                              ? gpTweakTargeting->x18_nextLockOnEnterDuration
                              : gpTweakTargeting->x20_nextLockOnSwitchDuration;
      x1d8_nextGroupTimer = x1d4_nextGroupDur;
      xf2_nextTargetId = nextTargetId;
    }
  }

  if (x1d8_nextGroupTimer > 0.f) {
    UpdateTargetParameters(x194_nextGroupA, mgr);
    UpdateTargetParameters(x1b4_nextGroupB, mgr);
    x1d8_nextGroupTimer = rstl::max_val(0.f, x1d8_nextGroupTimer - dt);
    CTargetReticleRenderState::InterpolateWithClamp(x194_nextGroupA, x174_nextGroupInterp,
                                                    x1b4_nextGroupB,
                                                    1.f - x1d8_nextGroupTimer / x1d4_nextGroupDur);
  } else {
    UpdateTargetParameters(x174_nextGroupInterp, mgr);
  }
}

void CCompoundTargetReticle::UpdateOrbitZoneGroup(float dt, const CStateManager& mgr) {
  if (xf0_targetId == kInvalidUniqueId && xf2_nextTargetId != kInvalidUniqueId) {
    x20c_unk = rstl::min_val(2.f * dt + x20c_unk, 1.f);
  } else {
    x20c_unk = rstl::max_val(x20c_unk - 2.f * dt, 0.f);
  }

  if (mgr.GetPlayer()->IsCrosshairsOpen() &&
      mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan) {
    x1e8_crosshairsScale =
        rstl::min_val(x1e8_crosshairsScale + dt / gpTweakTargeting->x1c4_crosshairsScaleDur, 1.f);
  } else {
    x1e8_crosshairsScale =
        rstl::max_val(x1e8_crosshairsScale - dt / gpTweakTargeting->x1c4_crosshairsScaleDur, 0.f);
  }
}

void CCompoundTargetReticle::Draw(const CStateManager& mgr, bool hideLockon) const {
  if (mgr.GetPlayer()->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
      !mgr.GetCameraManager()->IsInCinematicCamera()) {
    CTransform4f camXf = PortReticleCameraXf(mgr);
    CGraphics::SetViewPointMatrix(camXf);
    CMatrix3f rot = camXf.BuildMatrix3f();

    CGraphics::SetCullMode(kCM_None);

#ifdef TARGET_PC
    // The groups were placed at the tick's poses. While smooth frames draw
    // actors and the camera between ticks, carry a lock-on with its target to
    // where it is drawn, and an idle (camera-placed) group with the camera,
    // else they shake against what they mark (issue #76). Restored below.
    CTargetReticleRenderState& curr = const_cast< CTargetReticleRenderState& >(x10c_currGroupInterp);
    CTargetReticleRenderState& next = const_cast< CTargetReticleRenderState& >(x174_nextGroupInterp);
    const CVector3f savedCurr = curr.GetTargetPositionWorld();
    const CVector3f savedNext = next.GetTargetPositionWorld();
    PortPresentGroup(curr, mgr);
    PortPresentGroup(next, mgr);
#endif

    if (!hideLockon) {
      DrawCurrLockOnGroup(rot, mgr);
      DrawNextLockOnGroup(rot, mgr);
#ifdef TARGET_PC
      // Trilogy draws the scan ring between the next-target and orbit-zone groups.
      if (xPortTri) {
        PortTriDrawScan(rot, mgr);
      }
#endif
      DrawOrbitZoneGroup(rot, mgr);
    }

#ifdef TARGET_PC
    curr.SetTargetPositionWorld(savedCurr);
    next.SetTargetPositionWorld(savedNext);
#endif

    DrawGrappleGroup(rot, mgr, hideLockon);

    CGraphics::SetCullMode(kCM_Front);
  }

  if (x28_noDrawTicks > 0) {
    --x28_noDrawTicks;
  }
}

void CCompoundTargetReticle::DrawGrappleGroup(const CMatrix3f& rot, const CStateManager& mgr,
                                              bool hideLockon) const {
  if (x28_noDrawTicks > 0)
    return;

  if (!mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_GrappleBeam))
    return;

  const_cast< TCachedToken< CModel >& >(x94_grapple).TryCache();
  if (x94_grapple.GetObject() == nullptr)
    return;

  if (x20_prevState == kRS_Scan)
    return;

  const CObjectList& list = mgr.GetObjectListById(kOL_All);

  if (hideLockon) {
    for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i)) {
      const CScriptGrapplePoint* gp = TCastToConstPtr< CScriptGrapplePoint >(list[i]);
      if (gp == nullptr)
        continue;
      if (!gp->GetActive())
        continue;
      TAreaId areaId = gp->GetCurrentAreaId();
      if (areaId != kInvalidAreaId) {
        const CGameArea& area = mgr.GetWorld()->GetAreaAlways(areaId);
        if (area.GetOcclusionState() != CGameArea::kOS_Visible)
          continue;
      }
      float t = 0.f;
      TUniqueId uid = gp->GetUniqueId();
      if (uid == x1dc_grapplePoint0)
        t = x1e0_grapplePoint0T;
      else if (uid == x1de_grapplePoint1)
        t = x1e4_grapplePoint1T;
      if (close_enough(t, 0.f, 0.00001f)) {
        DrawGrapplePoint(*gp, t, mgr, rot, true);
      }
    }
  } else {
    const CScriptGrapplePoint* gp0 =
        TCastToConstPtr< CScriptGrapplePoint >(mgr.GetObjectById(x1dc_grapplePoint0));
    const CScriptGrapplePoint* gp1 =
        TCastToConstPtr< CScriptGrapplePoint >(mgr.GetObjectById(x1de_grapplePoint1));
    for (int i = 0; i < 2; ++i) {
      const CScriptGrapplePoint* gp = (i == 0) ? gp0 : gp1;
      float t = (i == 0) ? x1e0_grapplePoint0T : x1e4_grapplePoint1T;
      if (gp != nullptr) {
        DrawGrapplePoint(*gp, t, mgr, rot, false);
      }
    }
  }
}

void CCompoundTargetReticle::DrawGrapplePoint(const CScriptGrapplePoint& point, float t,
                                              const CStateManager& mgr, const CMatrix3f& rot,
                                              bool zEqual) const {
  CVector3f orbitPos = point.GetOrbitPosition(mgr);

  const CColor& selectColor = point.GetGrappleParameters().GetLockSwingTurn()
                                  ? gpTweakTargeting->x1ac_lockedGrapplePointSelectColor
                                  : gpTweakTargeting->x1a4_grapplePointSelectColor;
  CColor color = CColor::Lerp(gpTweakTargeting->x1a8_grapplePointColor, selectColor, t);

  t = (1.f - t) * gpTweakTargeting->x198_grappleScale +
      t * gpTweakTargeting->x194_grappleSelectScale;
  float scale = CalculateClampedScale(orbitPos, 1.f, gpTweakTargeting->x19c_grappleClampMin,
                                      gpTweakTargeting->x1a0_grappleClampMax, mgr);
  scale *= t;

  CMatrix3f scaledRot = rot * CMatrix3f::Scale(scale);
  gpRender->SetModelMatrix(CTransform4f(scaledRot, orbitPos));

  const CModel* model = x94_grapple.GetObject();
  model->Draw(CModelFlags::Additive(color).DepthCompareUpdate(zEqual, false));
}

void CCompoundTargetReticle::DrawCurrLockOnGroup(const CMatrix3f& rot,
                                                 const CStateManager& mgr) const {
  if (x28_noDrawTicks > 0)
    return;

  CVector3f position = x10c_currGroupInterp.GetTargetPositionWorld();
  float radius = x10c_currGroupInterp.GetRadiusWorld();

  if (x1e0_grapplePoint0T + x1e4_grapplePoint1T > 0.f)
    return;

  float factor = x10c_currGroupInterp.GetFactor();
  float lockBreakAlpha = factor;
  if (0.f == factor)
    return;

  float visorFactor = mgr.GetPlayerState()->GetVisorTransitionFactor();
  float minVpClampScale = x10c_currGroupInterp.GetMinViewportClampScale();

  bool lockConfirm = false;
  bool lockReticule = false;

  switch (x20_prevState) {
  case kRS_Combat:
    lockConfirm = true;
    lockReticule = true;
    break;
  case kRS_Scan:
    lockConfirm = true;
    break;
  case kRS_XRay:
  case kRS_Thermal:
  case kRS_Four:
  case kRS_Unspecified:
    break;
  }

  CMatrix3f lockBreakXf(CMatrix3f::Identity());
  CColor lockBreakColor(0);

  if (IsDamageOrbit(mgr.GetPlayer()->GetOrbitBrokenType()) && x14c_currGroupB.GetFactor() == 0.f) {
    CVector3f columns[3] = {CVector3f::Right(), CVector3f::Forward(), CVector3f::Up()};

    for (int i = 0; i < 4; ++i) {
      int r1 = rand();
      int idx = rand() % 9;
      int col = idx % 3;
      int row = idx / 3;
      columns[col][row] += static_cast< float >(r1) / static_cast< float >(RAND_MAX) - 0.5f;
    }

    lockBreakXf = CMatrix3f(columns[0], columns[1], columns[2]);

    if (factor > 0.8f) {
      lockBreakColor = CColor::White().WithAlphaOf(0.3f * (factor - 0.8f) / 0.2f);
    }

    if (factor > 0.75f) {
      lockBreakAlpha = 1.f;
    } else {
      lockBreakAlpha = rstl::max_val((factor - 0.55f) / 0.2f, 0.f);
    }
  }

  if (lockConfirm) {
    const_cast< TCachedToken< CModel >& >(x4c_lockConfirm).TryCache();
    if (CModel* const model = x4c_lockConfirm.GetObject()) {
      CTweakTargeting* tweak = gpTweakTargeting;
      float scale =
          CalculateClampedScale(position, radius, minVpClampScale * tweak->x154_lockConfirmClampMin,
                                tweak->x158_lockConfirmClampMax, mgr);
      scale *= gpTweakTargeting->x14_lockConfirmScale;
      scale /= factor;

      CMatrix3f combined =
          rot * CMatrix3f::RotateY(CRelAngle(x1ec_seekerAngle)) * CMatrix3f::Scale(scale);

      gpRender->SetModelMatrix(
          CTransform4f(lockBreakXf * combined, x10c_currGroupInterp.GetTargetPositionWorld()));

      model->Draw(CModelFlags::Additive(
                      CColor::Add(lockBreakColor, tweak->x14c_lockConfirmColor.WithAlphaModulatedBy(
                                                      lockBreakAlpha)))
                      .DepthCompareUpdate(false, false));
    }
  }

  if (lockReticule) {
    // Target flower
    const_cast< TCachedToken< CModel >& >(x58_targetFlower).TryCache();
    if (CModel* const model = x58_targetFlower.GetObject()) {
      float scale = CalculateClampedScale(
          position, radius, minVpClampScale * gpTweakTargeting->x15c_targetFlowerClampMin,
          gpTweakTargeting->x160_targetFlowerClampMax, mgr);
      CTweakTargeting* tweak = gpTweakTargeting;
      scale *= tweak->xb4_targetFlowerScale;
      scale /= lockBreakAlpha;

      CMatrix3f combined =
          rot * CMatrix3f::RotateY(CRelAngle(x1f0_xrayRetAngle)) * CMatrix3f::Scale(scale);

      gpRender->SetModelMatrix(
          CTransform4f(lockBreakXf * combined, x10c_currGroupInterp.GetTargetPositionWorld()));

      model->Draw(CModelFlags::Additive(
                      CColor::Add(lockBreakColor, tweak->xb8_targetFlowerColor.WithAlphaModulatedBy(
                                                      lockBreakAlpha * visorFactor)))
                      .DepthCompareUpdate(true, false));
    }

    // Missile bracket
    if (x1f8_missileBracketTimer != 0.f) {
      const_cast< TCachedToken< CModel >& >(x64_missileBracket).TryCache();
      if (CModel* const bracketModel = x64_missileBracket.GetObject()) {
        float bracketScale = CalculateClampedScale(
            position, radius, minVpClampScale * gpTweakTargeting->x16c_missileBracketClampMin,
            gpTweakTargeting->x170_missileBracketClampMax, mgr);
        CTweakTargeting* tweak = gpTweakTargeting;
        float halfDur = 0.5f * tweak->xc8_missileBracketScaleDuration;
        float t = CMath::AbsF((x1fc_missileBracketScaleTimer - halfDur) / halfDur);
        float tscale =
            (1.f - t) * tweak->xc4_missileBracketScaleEnd + t * tweak->xc0_missileBracketScaleStart;
        float bracketFactor =
            CMath::AbsF(x1f8_missileBracketTimer) / tweak->xbc_missileBracketDuration;
        float s = bracketFactor * bracketScale * tscale / factor;

        CMatrix3f scaleMtx = CMatrix3f::Scale(s);

        for (int i = 0; i < 4; ++i) {
          float xSign = i < 2 ? 1.f : -1.f;
          float zSign = (i & 1) != 0 ? 1.f : -1.f;
          CMatrix3f combined = lockBreakXf * rot *
                               CMatrix3f(xSign, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, zSign) *
                               scaleMtx;

          gpRender->SetModelMatrix(
              CTransform4f(combined, x10c_currGroupInterp.GetTargetPositionWorld()));

          bracketModel->Draw(
              CModelFlags::Additive(
                  CColor::Add(lockBreakColor, tweak->xcc_missileBracketColor.WithAlphaModulatedBy(
                                                  lockBreakAlpha * visorFactor)))
                  .DepthCompareUpdate(false, false));
        }
      }
    }

    // Outer beam icon squares
    {
      float outerScale = CalculateClampedScale(
          position, radius, minVpClampScale * gpTweakTargeting->x17c_chargeGaugeClampMin,
          gpTweakTargeting->x180_chargeGaugeClampMax, mgr);
      outerScale = gpTweakTargeting->xf0_outerBeamSquaresScale * (1.f / factor * outerScale);

      CMatrix3f outerBeamXf = rot * CMatrix3f::Scale(outerScale);
      int i;
      CTweakTargeting* tweak = gpTweakTargeting;

      for (i = 0; i < 9; ++i) {
        const SOuterItemInfo& info = xe0_outerBeamIconSquares[i];
        const_cast< TCachedToken< CModel >& >(info.x0_model).TryCache();
        CModel* const outerModel = info.x0_model.GetObject();
        if (outerModel != nullptr) {
          CRelAngle outerAngle(info.x10_rotAng);
          CMatrix3f combined = outerBeamXf * CMatrix3f::RotateY(outerAngle);

          gpRender->SetModelMatrix(
              CTransform4f(lockBreakXf * combined, x10c_currGroupInterp.GetTargetPositionWorld()));

          outerModel->Draw(
              CModelFlags::Additive(
                  CColor::Add(lockBreakColor, tweak->xf4_outerBeamSquareColor.WithAlphaModulatedBy(
                                                  lockBreakAlpha * visorFactor)))
                  .DepthCompareUpdate(false, false));
        }
      }
    }

    // Charge gauge
    {
      const_cast< SOuterItemInfo& >(xc4_chargeGauge).x0_model.TryCache();
      if (CModel* const gaugeModel = xc4_chargeGauge.x0_model.GetObject()) {
        float gaugeScale = CalculateClampedScale(
            position, radius, minVpClampScale * gpTweakTargeting->x17c_chargeGaugeClampMin,
            gpTweakTargeting->x180_chargeGaugeClampMax, mgr);
        gaugeScale = gaugeScale * gpTweakTargeting->x118_chargeGaugeScale / factor;

        CMatrix3f gaugeMtx = rot * CMatrix3f::Scale(gaugeScale);
        CRelAngle gaugeAngle(xc4_chargeGauge.x10_rotAng);
        CMatrix3f chargeGaugeXf = gaugeMtx * CMatrix3f::RotateY(gaugeAngle);

        float chargeFadeFactor =
            x214_fullChargeFadeTimer / gpTweakTargeting->x1b8_fullChargeFadeDuration;
        float pulsePeriod = gpTweakTargeting->x1d0_chargeGaugePulsePeriod;
        float secondsMod = CGraphics::GetSecondsMod900();
        float pulseT = CMath::AbsF(static_cast< float >(
            fmod(static_cast< double >(secondsMod), static_cast< double >(pulsePeriod))));
        float halfPeriod = 0.5f * pulsePeriod;
        float pulseRatio;
        if (pulseT < halfPeriod) {
          pulseRatio = pulseT / halfPeriod;
        } else {
          pulseRatio = (pulsePeriod - pulseT) / halfPeriod;
        }

        CColor pulseColor =
            CColor::Lerp(gpTweakTargeting->x1b4_chargeGaugePulseColorHigh,
                         gpTweakTargeting->x1cc_chargeGaugePulseColorLow, pulseRatio);
        CColor gaugeColor = CColor::Lerp(gpTweakTargeting->x11c_chargeGaugeNonFullColor, pulseColor,
                                         chargeFadeFactor);

        CTransform4f modelXf = CTransform4f(lockBreakXf * chargeGaugeXf,
                                            x10c_currGroupInterp.GetTargetPositionWorld());
        gpRender->SetModelMatrix(modelXf);

        gaugeModel->Draw(
            CModelFlags::Additive(CColor::Add(lockBreakColor, gaugeColor.WithAlphaModulatedBy(
                                                                  lockBreakAlpha * visorFactor)))
                .DepthCompareUpdate(false, false));

        // Charge ticks
        const_cast< TCachedToken< CModel >& >(xa0_chargeTickFirst).TryCache();
        CModel* const tickModel = xa0_chargeTickFirst.GetObject();
        if (tickModel != nullptr) {
          const CPlayerGun* gun = mgr.GetPlayer()->GetPlayerGun();
          int numTicks =
              static_cast< int >(static_cast< float >(gpTweakTargeting->x120_chargeTickCount) *
                                 gun->GetChargePercentage());
          for (int i = 0; i < numTicks; ++i) {
            tickModel->Draw(CModelFlags::Additive(
                                CColor::Add(lockBreakColor, gaugeColor.WithAlphaModulatedBy(
                                                                lockBreakAlpha * visorFactor)))
                                .DepthCompareUpdate(false, false));
            modelXf.RotateLocalY(CRelAngle(gpTweakTargeting->x124_chargeTickAnglePitch));
            gpRender->SetModelMatrix(modelXf);
          }
        }
      }
    }

    // Inner beam icon
    if (x208_lockonTimer > 0.f) {
      const_cast< TCachedToken< CModel >& >(x70_innerBeamIcon).TryCache();
      if (CModel* const beamModel = x70_innerBeamIcon.GetObject()) {
        const CColor* iconColor;
        if (x200_beam == CPlayerState::kBI_Power) {
          iconColor = &gpTweakTargeting->xd8_innerBeamColorPower;
        } else if (x200_beam == CPlayerState::kBI_Ice) {
          iconColor = &gpTweakTargeting->xdc_innerBeamColorIce;
        } else if (x200_beam == CPlayerState::kBI_Wave) {
          iconColor = &gpTweakTargeting->xe0_innerBeamColorWave;
        } else {
          iconColor = &gpTweakTargeting->xe4_innerBeamColorPlasma;
        }

        float beamScale = CalculateClampedScale(
            position, radius, minVpClampScale * gpTweakTargeting->x174_innerBeamClampMin,
            gpTweakTargeting->x178_innerBeamClampMax, mgr);
        beamScale = beamScale * gpTweakTargeting->xd4_innerBeamScale *
                    (x208_lockonTimer / gpTweakTargeting->xd0_lockonDuration) / factor;

        CMatrix3f beamMtx = rot * CMatrix3f::Scale(beamScale);

        gpRender->SetModelMatrix(
            CTransform4f(lockBreakXf * beamMtx, x10c_currGroupInterp.GetTargetPositionWorld()));

        beamModel->Draw(
            CModelFlags::Additive(CColor::Add(lockBreakColor, iconColor->WithAlphaModulatedBy(
                                                                  lockBreakAlpha * visorFactor)))
                .DepthCompareUpdate(false, false));
      }
    }

    // Lock fire
    if (x210_lockFireTimer > 0.f) {
      const_cast< TCachedToken< CModel >& >(x7c_lockFire).TryCache();
      if (CModel* const fireModel = x7c_lockFire.GetObject()) {
        CTweakTargeting* tweak = gpTweakTargeting;
        float lockFireFactor = x210_lockFireTimer / tweak->x12c_lockFireDuration;

        float fireScale =
            CalculateClampedScale(position, radius, minVpClampScale * tweak->x184_lockFireClampMin,
                                  tweak->x188_lockFireClampMax, mgr);
        fireScale = fireScale * gpTweakTargeting->x128_lockFireScale / factor;

        CMatrix3f combined =
            rot * CMatrix3f::Scale(fireScale) * CMatrix3f::RotateY(CRelAngle(x1f0_xrayRetAngle));

        gpRender->SetModelMatrix(
            CTransform4f(lockBreakXf * combined, x10c_currGroupInterp.GetTargetPositionWorld()));

        fireModel->Draw(
            CModelFlags::Additive(
                CColor::Add(lockBreakColor, tweak->x130_lockFireColor.WithAlphaModulatedBy(
                                                lockBreakAlpha * lockFireFactor * visorFactor)))
                .DepthCompareUpdate(false, false));
      }
    }

    // Lock dagger
    if (x208_lockonTimer > 0.f) {
      const_cast< TCachedToken< CModel >& >(x88_lockDagger).TryCache();
      if (CModel* const daggerModel = x88_lockDagger.GetObject()) {
        float daggerScale = CalculateClampedScale(
            position, radius, minVpClampScale * gpTweakTargeting->x18c_lockDaggerClampMin,
            gpTweakTargeting->x190_lockDaggerClampMax, mgr);
        CTweakTargeting* tweak = gpTweakTargeting;
        float halfDur = 0.5f * tweak->x12c_lockFireDuration;
        float t = CMath::AbsF((x210_lockFireTimer - halfDur) / halfDur);
        float tscale =
            (1.f - t) * tweak->x138_lockDaggerScaleEnd + t * tweak->x134_lockDaggerScaleStart;
        daggerScale =
            daggerScale * tscale * (x208_lockonTimer / tweak->xd0_lockonDuration) / factor;

        CMatrix3f daggerMtx = rot * CMatrix3f::Scale(daggerScale);

        for (int i = 0; i < 3; ++i) {
          float ang;
          if (i == 0) {
            ang = gpTweakTargeting->x140_lockDaggerAngle0;
          } else if (i == 1) {
            ang = gpTweakTargeting->x144_lockDaggerAngle1;
          } else {
            ang = gpTweakTargeting->x148_lockDaggerAngle2;
          }

          CMatrix3f combined = daggerMtx * CMatrix3f::RotateY(CRelAngle(ang));

          gpRender->SetModelMatrix(
              CTransform4f(lockBreakXf * combined, x10c_currGroupInterp.GetTargetPositionWorld()));

          daggerModel->Draw(
              CModelFlags::Additive(
                  CColor::Add(lockBreakColor, tweak->x13c_lockDaggerColor.WithAlphaModulatedBy(
                                                  lockBreakAlpha * visorFactor)))
                  .DepthCompareUpdate(false, false));
        }
      }
    }
  }
}

void CCompoundTargetReticle::DrawNextLockOnGroup(const CMatrix3f& rot,
                                                 const CStateManager& mgr) const {
  if (x28_noDrawTicks > 0)
    return;

  CVector3f position = x174_nextGroupInterp.GetTargetPositionWorld();
  float radius = x174_nextGroupInterp.GetRadiusWorld();
  float factor = x174_nextGroupInterp.GetFactor();
  float visorFactor = mgr.GetPlayerState()->GetVisorTransitionFactor();
  // Port: F1 "Secondary crosshairs" off hides the seeker and the X-ray/thermal next-target
  // reticles; the scan visor's box below stays (it shows what a scan would pick).
  const bool portSecondary = PortDebug::SecondaryCrosshairs();

  bool scanRet = false;
  bool xrayRet = false;
  bool thermalRet = false;

  switch (x20_prevState) {
  case kRS_Combat:
    break;
  case kRS_Scan:
    scanRet = true;
    break;
  case kRS_XRay:
    xrayRet = true;
    break;
  case kRS_Thermal:
    thermalRet = true;
    break;
  default:
    break;
  }

  float minVpClampScale = x174_nextGroupInterp.GetMinViewportClampScale();

  if (portSecondary && !xrayRet && factor > 0.f) {
    const_cast< TCachedToken< CModel >& >(x40_seeker).TryCache();
    if (CModel* const model = x40_seeker.GetObject()) {
      float scale = CalculateClampedScale(position, radius,
                                          minVpClampScale * gpTweakTargeting->x164_seekerClampMin,
                                          gpTweakTargeting->x168_seekerClampMax, mgr);
      CTweakTargeting* tweak = gpTweakTargeting;
      scale *= tweak->x24_seekerScale;

      CMatrix3f seekerMatrix(rot * CMatrix3f::RotateY(CRelAngle(x1ec_seekerAngle)) *
                             CMatrix3f::Scale(scale));

      gpRender->SetModelMatrix(
          CTransform4f(seekerMatrix, x174_nextGroupInterp.GetTargetPositionWorld()));

      model->Draw(CModelFlags::Additive(tweak->x150_seekerColor.WithAlphaModulatedBy(factor))
                      .DepthCompareUpdate(false, false));
    }
  }

  if (portSecondary && xrayRet) {
    const_cast< TCachedToken< CModel >& >(xac_xrayRetRing).TryCache();
    if (CModel* const model = xac_xrayRetRing.GetObject()) {
      float scale = CalculateClampedScale(position, radius,
                                          minVpClampScale * gpTweakTargeting->x20c_reticuleClampMin,
                                          gpTweakTargeting->x210_reticuleClampMax, mgr);
      CTweakTargeting* tweak = gpTweakTargeting;
      scale *= tweak->x218_reticuleScale;

      CMatrix3f xrayMatrix(rot * CMatrix3f::Scale(scale) *
                           CMatrix3f::RotateY(CRelAngle(x1f0_xrayRetAngle)));

      gpRender->SetModelMatrix(
          CTransform4f(xrayMatrix, x174_nextGroupInterp.GetTargetPositionWorld()));

      model->Draw(
          CModelFlags::Additive(tweak->x214_xrayRetRingColor.WithAlphaModulatedBy(visorFactor))
              .DepthCompareUpdate(false, false));
    }
  }

  if (portSecondary && thermalRet) {
    const_cast< TCachedToken< CModel >& >(xb8_thermalReticle).TryCache();
    if (CModel* const model = xb8_thermalReticle.GetObject()) {
      float scale = CalculateClampedScale(position, radius,
                                          minVpClampScale * gpTweakTargeting->x20c_reticuleClampMin,
                                          gpTweakTargeting->x210_reticuleClampMax, mgr);
      CTweakTargeting* tweak = gpTweakTargeting;
      scale *= tweak->x218_reticuleScale;

      CMatrix3f thermalMatrix(rot * CMatrix3f::Scale(scale));

      gpRender->SetModelMatrix(
          CTransform4f(thermalMatrix, x174_nextGroupInterp.GetTargetPositionWorld()));

      model->Draw(
          CModelFlags::Additive(tweak->xb0_thermalReticuleColor.WithAlphaModulatedBy(visorFactor))
              .DepthCompareUpdate(false, false));
    }
  }

#ifdef TARGET_PC
  // Trilogy's next-target group (fn_80237EC4) draws no scan lines: its reticle code has no line
  // primitives and no 20.5 arm length. Its scan reticle is the ring and brackets
  // (PortTriDrawScan).
  if (xPortTri) {
    scanRet = false;
  }
#endif
  if (scanRet && visorFactor > 0.f) {
    float nextFactor = x174_nextGroupInterp.GetFactor();
    float scale = CalculateClampedScale(position, radius,
                                        minVpClampScale * gpTweakTargeting->x21c_scanTargetClampMin,
                                        gpTweakTargeting->x220_scanTargetClampMax, mgr);
    int i;
    CTweakGuiColors* guiColors = gpTweakGuiColors;
    scale *= 1.f / (visorFactor * nextFactor);

    CMatrix3f scanMatrix(rot * CMatrix3f::Scale(scale));

    gpRender->SetModelMatrix(
        CTransform4f(scanMatrix, x174_nextGroupInterp.GetTargetPositionWorld()));

    CGraphics::SetDepthWriteMode(true, kE_Less, false);

    for (i = 0; i < 2; ++i) {
      float lineWidth = i == 0 ? 1.f : 2.5f;
      CGraphics::SetLineWidth(lineWidth, kTO_Zero);

      CColor color =
          guiColors->GetScanReticuleColor().WithAlphaModulatedBy(0.5f * (visorFactor * nextFactor));

      gpRender->BeginLines(8);
      gpRender->PrimColor(color);
      gpRender->PrimVertex(CVector3f(-0.5f, 0.f, 0.f));
      gpRender->PrimVertex(CVector3f(-20.5f, 0.f, 0.f));
      gpRender->PrimVertex(CVector3f(0.5f, 0.f, 0.f));
      gpRender->PrimVertex(CVector3f(20.5f, 0.f, 0.f));
      gpRender->PrimVertex(CVector3f(0.f, 0.f, -0.5f));
      gpRender->PrimVertex(CVector3f(0.f, 0.f, -20.5f));
      gpRender->PrimVertex(CVector3f(0.f, 0.f, 0.5f));
      gpRender->PrimVertex(CVector3f(0.f, 0.f, 20.5f));
      gpRender->EndPrimitive();

      for (int j = 0; j < 4; ++j) {
        float xSign = j < 2 ? -1.f : 1.f;
        float zSign = (j & 1) != 0 ? -1.f : 1.f;

        gpRender->BeginLineStrip(4);
        gpRender->PrimVertex(CVector3f(0.5f * xSign, 0.f, 0.1f * zSign));
        gpRender->PrimVertex(CVector3f(0.5f * xSign, 0.f, 0.35f * zSign));
        gpRender->PrimVertex(CVector3f(0.35f * xSign, 0.f, 0.5f * zSign));
        gpRender->PrimVertex(CVector3f(0.1f * xSign, 0.f, 0.5f * zSign));
        gpRender->EndPrimitive();
      }
    }

    CGraphics::SetLineWidth(1.f, kTO_Zero);
  }
}

#ifdef TARGET_PC
namespace {
struct PortXhairVertex {
  CVector3f pos;
  CColor color;
};

// Remastered's combat aiming cursor (CReticleCombatAiming), drawn procedurally: a soft hexagonal
// centre dot and three tapered spikes turned 0/120/240 degrees about it, the tip of each
// toward the centre. Units are Remastered's model units on its cursor plane, 1.25 m in front of
// the camera; `k` scales them to the world at the depth the cursor is drawn at. The outlines
// were read off Remastered's arm and centre textures (core half-width and glow per station).
struct PortXhairStation {
  float y, core, glow;
};
const PortXhairStation kPortXhairArm[] = {
    {0.018f, 0.f, 0.f},         {0.014f, 0.004f, 0.040f},  {0.001f, 0.012f, 0.047f},
    {-0.010f, 0.016f, 0.047f},  {-0.022f, 0.016f, 0.048f}, {-0.045f, 0.013f, 0.047f},
    {-0.068f, 0.009f, 0.044f},  {-0.080f, 0.004f, 0.041f}, {-0.100f, 0.003f, 0.032f},
    {-0.125f, 0.0015f, 0.023f}, {-0.135f, 0.f, 0.f},
};
const float kPortXhairGlowAlpha = 0.3f;
const float kPortXhairCentreCore = 0.019f;  // hexagon circumradius, opaque
const float kPortXhairCentreOuter = 0.030f; // where the glow reaches zero
const float kPortXhairCentreAlpha = 0.92f;
const float kPortXhairArmOffset = 0.1f;
const float kPortXhairGlobalScale = 0.45f;

void PortXhairAddQuad(std::vector< PortXhairVertex >& v, const PortXhairVertex& a,
                      const PortXhairVertex& b, const PortXhairVertex& c, const PortXhairVertex& d) {
  v.insert(v.end(), {a, b, c, a, c, d});
}

void PortDrawRemasteredCrosshair(const CMatrix3f& rot, const CVector3f& pos, float k,
                                 float armScale, float centreScale, const CColor& color) {
  std::vector< PortXhairVertex > v;
  const float pi = 3.14159265f;
  auto col = [&](float a) { return CColor(color.GetRed(), color.GetGreen(), color.GetBlue(), color.GetAlpha() * a); };
  auto vert = [&](float x, float y, float a) {
    return PortXhairVertex{CVector3f(x * k, 0.f, y * k), col(a)};
  };

  // Centre: solid hexagon (point up) and a falloff ring out to the glow radius.
  {
    const float su = centreScale * kPortXhairGlobalScale;
    for (int i = 0; i < 6; ++i) {
      const float a0 = (90.f + 60.f * static_cast< float >(i)) * pi / 180.f;
      const float a1 = (90.f + 60.f * static_cast< float >(i + 1)) * pi / 180.f;
      const float ri = kPortXhairCentreCore * su, ro = kPortXhairCentreOuter * su;
      v.push_back(vert(0.f, 0.f, kPortXhairCentreAlpha));
      v.push_back(vert(ri * cosf(a0), ri * sinf(a0), kPortXhairCentreAlpha));
      v.push_back(vert(ri * cosf(a1), ri * sinf(a1), kPortXhairCentreAlpha));
      PortXhairAddQuad(v, vert(ri * cosf(a0), ri * sinf(a0), kPortXhairCentreAlpha),
                       vert(ro * cosf(a0), ro * sinf(a0), 0.f), vert(ro * cosf(a1), ro * sinf(a1), 0.f),
                       vert(ri * cosf(a1), ri * sinf(a1), kPortXhairCentreAlpha));
    }
  }

  // Arms: glow strip (faint along the axis, zero at the edges), then the solid core.
  const float sa = armScale * kPortXhairGlobalScale;
  const int stations = static_cast< int >(sizeof(kPortXhairArm) / sizeof(kPortXhairArm[0]));
  for (int arm = 0; arm < 3; ++arm) {
    const float ang = static_cast< float >(arm) * 120.f * pi / 180.f;
    const float ca = cosf(ang), sn = sinf(ang);
    auto place = [&](float x, float y, float a) {
      const float px = x * sa, py = y * sa + kPortXhairArmOffset;
      return vert(px * ca - py * sn, px * sn + py * ca, a);
    };
    for (int i = 0; i + 1 < stations; ++i) {
      const PortXhairStation& p = kPortXhairArm[i];
      const PortXhairStation& q = kPortXhairArm[i + 1];
      PortXhairAddQuad(v, place(-p.glow, p.y, 0.f), place(0.f, p.y, kPortXhairGlowAlpha),
                       place(0.f, q.y, kPortXhairGlowAlpha), place(-q.glow, q.y, 0.f));
      PortXhairAddQuad(v, place(0.f, p.y, kPortXhairGlowAlpha), place(p.glow, p.y, 0.f),
                       place(q.glow, q.y, 0.f), place(0.f, q.y, kPortXhairGlowAlpha));
      PortXhairAddQuad(v, place(-p.core, p.y, 1.f), place(p.core, p.y, 1.f), place(q.core, q.y, 1.f),
                       place(-q.core, q.y, 1.f));
    }
  }

  gpRender->SetModelMatrix(CTransform4f(rot, pos));
  gpRender->SetBlendMode_AlphaBlended();
  CGraphics::SetCullMode(kCM_None);
  gpRender->BeginTriangles(static_cast< int >(v.size()));
  for (const PortXhairVertex& pv : v) {
    gpRender->PrimColor(pv.color);
    gpRender->PrimVertex(pv.pos);
  }
  gpRender->EndPrimitive();
}

CColor PortRemasteredCrosshairColor(EReticleState state, float targetT) {
  float idle[3] = {0.f, 0.7f, 1.f};
  if (state == kRS_XRay) {
    idle[0] = 1.f, idle[1] = 0.54f, idle[2] = 0.19f;
  } else if (state == kRS_Thermal) {
    idle[0] = 0.58f, idle[1] = 0.91f, idle[2] = 1.f;
  }
  float rgb[3];
  if (PortDebug::CrosshairColor(rgb)) {
    std::copy_n(rgb, 3, idle);
  }
  const float target[3] = {1.f, 0.f, 0.f};
  return CColor(idle[0] + (target[0] - idle[0]) * targetT, idle[1] + (target[1] - idle[1]) * targetT,
                idle[2] + (target[2] - idle[2]) * targetT, 1.f);
}
} // namespace

// Remastered's cursor tracks what the aim ray meets: its arms and dot shrink with the distance
// (1 m or less: full, 50 m or more: 0.45 and 0.33, at most 0.2 per tick), and the colour fades
// to red over a hostile. "Hostile" here is a living, active, targetable CPatterned, or the
// owner of a collision actor the ray hits. Run once per simulation tick.
void CCompoundTargetReticle::PortUpdateRemasteredCrosshair(float dt, const CStateManager& mgr) {
  const CCameraManager* camMgr = mgr.GetCameraManager();
  const CGameCamera& cam = camMgr->GetCurrentCamera(mgr);
  CVector3f eye = camMgr->GetSimulationCameraTransform(mgr).GetTranslation();
  CVector3f dir = xf4_targetPos - eye;
  float pointerX = 0.f;
  float pointerY = 0.f;
  if (PortDebug::PresentedPointerAimPosition(CCameraManager::GetPresentationInterpolation(), pointerX,
                                             pointerY)) {
    float yaw = 0.f;
    float pitch = 0.f;
    PortMouse::PointerAim::RayAngles(pointerX, pointerY, cam.GetRenderFov(), cam.GetAspectRatio(), yaw,
                                     pitch);
    const CTransform4f viewXf = cam.GetTransform();
    dir = viewXf.Rotate(CVector3f(sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), sinf(pitch)));
    eye = viewXf.GetTranslation();
  }
  const float kRange = 100.f;
  float distance = kRange;
  bool overTarget = false;
  if (dir.CanBeNormalized()) {
    dir.Normalize();
    static const CMaterialFilter filter = CMaterialFilter::MakeIncludeExclude(
        CMaterialList(kMT_Solid), CMaterialList(kMT_Player, kMT_ProjectilePassthrough));
    TEntityList nearList;
    mgr.BuildNearList(nearList, eye, dir, kRange, filter, mgr.GetPlayer());
    TUniqueId hitId = kInvalidUniqueId;
    const CRayCastResult hit = mgr.RayWorldIntersection(hitId, eye, dir, kRange, filter, nearList);
    if (hit.IsValid()) {
      distance = (hit.GetPoint() - mgr.GetPlayer()->GetTranslation()).Magnitude();
      const CEntity* ent = mgr.GetObjectById(hitId);
      if (const CCollisionActor* col = TCastToConstPtr< CCollisionActor >(ent)) {
        ent = mgr.GetObjectById(col->GetOwnerId());
      }
      if (const CPatterned* pat = TCastToConstPtr< CPatterned >(ent)) {
          overTarget = pat->IsAlive() && pat->GetActive() && pat->GetTargetable();
      }
    }
  }
  const float f = rstl::min_val(rstl::max_val((distance - 1.f) / 49.f, 0.f), 1.f);
  auto step = [](float cur, float want, float lo) {
    return rstl::min_val(rstl::max_val(cur + rstl::min_val(rstl::max_val(want - cur, -0.2f), 0.2f), lo), 1.f);
  };
  xPortXhairArmScale = step(xPortXhairArmScale, 1.f + (0.45f - 1.f) * f, 0.45f);
  xPortXhairCentreScale = step(xPortXhairCentreScale, 1.f + (0.33f - 1.f) * f, 0.33f);
  xPortXhairTargetT = overTarget ? rstl::min_val(xPortXhairTargetT + dt / 0.25f, 1.f)
                                 : rstl::max_val(xPortXhairTargetT - dt / 0.25f, 0.f);
}
#endif

void CCompoundTargetReticle::DrawOrbitZoneGroup(const CMatrix3f& rot,
                                                const CStateManager& mgr) const {
  if (x28_noDrawTicks <= 0 && x1e8_crosshairsScale > 0.f) {
    const_cast< TCachedToken< CModel >& >(x34_crosshairs).TryCache();
    CTweakTargeting* tweak;
    CModel* const model = x34_crosshairs.GetObject();
    if (model == nullptr)
      return;

    tweak = gpTweakTargeting;

    float scale = x1e8_crosshairsScale;
#ifdef TARGET_PC
    if (PortDebug::DirectAim()) {
      scale *= static_cast< float >(PortDebug::CrosshairSize()) / 100.f;
    }
#endif
    CVector3f targetPos = xf4_targetPos;
#ifdef TARGET_PC
    const CCameraManager* const camMgr = mgr.GetCameraManager();
    const CGameCamera& cam = camMgr->GetCurrentCamera(mgr);
    // The idle crosshair sits on the tick's aim ray, placed against the
    // simulation camera. Keep it where it is relative to the view while
    // smooth frames blend the camera between ticks or per-frame look turns it
    // ahead, else it slides off-centre and snaps back every tick (issue #24).
    targetPos = PortReticleCameraXf(mgr) *
                (camMgr->GetSimulationCameraTransform(mgr).GetInverse() * targetPos);
    // Pointer aim: the crosshair is the pointer, moved every drawn frame. The model's
    // geometry sits well in front of its origin (it turns with the camera), so put its
    // centre on the ray through the pointer at the depth it is drawn at when centred.
    float pointerX = 0.f;
    float pointerY = 0.f;
    if (PortDebug::PresentedPointerAimPosition(CCameraManager::GetPresentationInterpolation(),
                                                    pointerX, pointerY)) {
      float yaw = 0.f;
      float pitch = 0.f;
      PortMouse::PointerAim::RayAngles(pointerX, pointerY, cam.GetRenderFov(),
                                       cam.GetAspectRatio(), yaw, pitch);
      const CTransform4f camXf = PortReticleCameraXf(mgr);
      const CVector3f centre = model->GetBoundingBox().GetCenterPoint() * scale;
      const float depth = (camXf.GetInverse() * targetPos).GetY() + centre.GetY();
      const CVector3f dir(sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), sinf(pitch));
      if (depth > 0.f && dir.GetY() > 0.f) {
        targetPos = camXf * (dir * (depth / dir.GetY()) - centre);
      }
    }
#endif
    gpRender->SetModelMatrix(CTransform4f(rot, targetPos) * CTransform4f::Scale(scale));

    CColor color = tweak->x1c0_crosshairsColor;
#ifdef TARGET_PC
    float rgb[3];
    if (PortDebug::CrosshairColor(rgb)) {
      color = CColor(rgb[0], rgb[1], rgb[2], color.GetAlpha());
    }
    // The tweak's alpha is 0.19, so the additive strokes barely show on a modern screen. Outside
    // Original experience the opacity setting is the alpha itself (100% = full strength).
    const float opacity = static_cast< float >(PortDebug::CrosshairOpacity()) / 100.f;
    color = PortDebug::OriginalExperience()
                ? color.WithAlphaModulatedBy(opacity)
                : CColor(color.GetRed(), color.GetGreen(), color.GetBlue(), opacity);
#endif
#ifdef TARGET_PC
    if (PortDebug::CrosshairStyle() == PortDebug::kCrosshairStyle_Remastered) {
      // Where the classic model's centre is drawn, with Remastered's cursor at its own size
      // (100% = Remastered's; the size slider scales it) facing the camera.
      const CTransform4f camXf = PortReticleCameraXf(mgr);
      const CVector3f cursor = targetPos + rot * (model->GetBoundingBox().GetCenterPoint() * scale);
      const float depth = (camXf.GetInverse() * cursor).GetY();
      if (depth > 0.f) {
        // Remastered places the cursor on a plane 1.25 along the camera's forward axis and draws
        // it in the world camera's projection (CAimingCursorMP1::Update 0xcb6250,
        // GetCursorPlaneDistance 0xcb6da4, CGameRenderJobMP1::RenderHUD 0xfa0fd8): drawn at
        // `depth` instead, it keeps its angular size, and the FOV is the projection's.
        const float units = depth / 1.25f;
        const float size = PortDebug::DirectAim() ? static_cast< float >(PortDebug::CrosshairSize()) / 100.f : 1.f;
        CColor rc = PortRemasteredCrosshairColor(x20_prevState, xPortXhairTargetT);
        rc = CColor(rc.GetRed(), rc.GetGreen(), rc.GetBlue(),
                    x1e8_crosshairsScale * static_cast< float >(PortDebug::CrosshairOpacity()) / 100.f);
        CGraphics::SetDepthWriteMode(false, kE_Always, false);
        PortDrawRemasteredCrosshair(rot, cursor, units * size, xPortXhairArmScale, xPortXhairCentreScale, rc);
        gpRender->SetBlendMode_AlphaBlended();
      }
      return;
    }
#endif
    model->Draw(CModelFlags::Additive(color.WithAlphaModulatedBy(x1e8_crosshairsScale))
                    .DepthCompareUpdate(false, false));
  }
}

void CCompoundTargetReticle::UpdateTargetParameters(CTargetReticleRenderState& state,
                                                    const CStateManager& mgr) {
  if (const CActor* act = TCastToConstPtr< CActor >(
          mgr.GetObjectListById(kOL_All).GetObjectById(state.GetTargetId()))) {
    state.SetRadiusWorld(CalculateRadiusWorld(*act, mgr));
    CVector3f pos = CalculatePositionWorld(*act, mgr);
    state.SetTargetPositionWorld(pos);
  } else if (state.GetIsOrbitZoneIdlePosition()) {
    state.SetRadiusWorld(1.f);
    state.SetTargetPositionWorld((x20_prevState == kRS_XRay || x20_prevState == kRS_Thermal)
                                     ? x100_laggingTargetPos
                                     : xf4_targetPos);
  }
}

#ifdef TARGET_PC
void CCompoundTargetReticle::PortPresentGroup(CTargetReticleRenderState& state,
                                              const CStateManager& mgr) {
  // Same sources as UpdateTargetParameters: an actor's point moves with the
  // actor's drawn pose, the idle point with the drawn camera (see issue #24).
  if (const CActor* act = TCastToConstPtr< CActor >(
          mgr.GetObjectListById(kOL_All).GetObjectById(state.GetTargetId()))) {
    state.SetTargetPositionWorld(act->PortPresentedPoint(state.GetTargetPositionWorld()));
  } else if (state.GetIsOrbitZoneIdlePosition()) {
    const CCameraManager* camMgr = mgr.GetCameraManager();
    state.SetTargetPositionWorld(
        PortReticleCameraXf(mgr) *
        (camMgr->GetSimulationCameraTransform(mgr).GetInverse() * state.GetTargetPositionWorld()));
  }
}
#endif

float CCompoundTargetReticle::CalculateRadiusWorld(const CActor& actor,
                                                   const CStateManager& mgr) const {
  rstl::optional_object< CAABox > touchBounds = actor.GetTouchBounds();
  const CAABox& aabb = touchBounds.valid()
                           ? *touchBounds
                           : CAABox(actor.GetAimPosition(mgr, 0.f), actor.GetAimPosition(mgr, 0.f));

  const CVector3f min = aabb.GetMinPoint();
  const CVector3f max = aabb.GetMaxPoint();

  float radius;
  switch (gpTweakTargeting->x4_targetRadiusMode) {
  case 0: {
    radius = rstl::min_val(max[0] - min[0], rstl::min_val(max[2] - min[2], max[1] - min[1])) * 0.5f;
    break;
  }
  case 1:
    radius = rstl::max_val(max[0] - min[0], rstl::max_val(max[2] - min[2], max[1] - min[1])) * 0.5f;
    break;
  case 2:
  default: {
    float w = max[0] - min[0];
    float h = max[1] - min[1];
    float d = max[2] - min[2];
    radius = (w + d + h) * (1.f / 6.f);
    break;
  }
  }

  return radius > 0.f ? radius : 1.f;
}

CVector3f CCompoundTargetReticle::CalculatePositionWorld(const CActor& actor,
                                                         const CStateManager& mgr) const {
  return x20_prevState == kRS_Scan ? actor.GetOrbitPosition(mgr) : actor.GetAimPosition(mgr, 0.f);
}

CVector3f CCompoundTargetReticle::CalculateOrbitZoneReticlePosition(const CStateManager& mgr,
                                                                    bool lag) const {
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  float halfExtY = CCast::LtoF(gpTweakPlayer->GetOrbitZoneHeight(0));
  float dist = 224.f / halfExtY;
  dist /= CMath::SlowTangentR(cam.GetRenderFov() * 0.5f * (1.f / 360.f) * (2.f * M_PIF));

  // The simulation camera's frame, not the third-person view: Draw and PortPresentGroup map
  // this point to the presented view (View * Sim^-1), which is where the shoulder offset goes.
  CTransform4f camXf = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
  CVector3f fwd = camXf.GetForward();

  if (lag) {
    fwd = x10_laggingOrientation.Transform(fwd);
  }

  return camXf.GetTranslation() + dist * fwd;
}

bool CCompoundTargetReticle::IsGrappleTarget(TUniqueId id, const CStateManager& mgr) {
  return TCastToConstPtr< CScriptGrapplePoint >(mgr.GetObjectListById(kOL_All).GetObjectById(id)) !=
         nullptr;
}

float CCompoundTargetReticle::CalculateClampedScale(CVector3f pos, float scale, float clampMin,
                                                    float clampMax, const CStateManager& mgr) {
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  CTransform4f camXf = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
#ifdef TARGET_PC
  // Draws pass points placed for the presented camera (PortPresentGroup), but
  // the scale is measured against the tick's camera. Between ticks the two are
  // up to a step apart, so walking forward or back pulsed the clamped size of
  // the X-ray ring and thermal reticle at every sim step (issue #97). Measure
  // the point where it sits relative to the drawn camera.
  pos = mgr.GetCameraManager()->GetSimulationCameraTransform(mgr) * camXf.GetInverse() * pos;
#endif
#ifdef TARGET_PC
  CVector3f viewSpace = PortThirdPerson::TickView(cam.GetTransform()).TransposeMultiply(pos);
#else
  CVector3f viewSpace = cam.GetTransform().TransposeMultiply(pos);
#endif
  float projX1 = cam.GetPerspectiveMatrix().MultiplyOneOverW(viewSpace).GetX();
  float pixelScale =
      cam.GetPerspectiveMatrix().MultiplyOneOverW(viewSpace + CVector3f(scale, 0.f, 0.f)).GetX() -
      projX1;
#ifdef TARGET_PC
  // Trilogy (fn_8023A0B4, 0x8023A1CC) measures against a fixed 640 (lbl_805C6CF8), its
  // framebuffer width in 4:3 and anamorphic 16:9 alike, so a clamp is a share of the screen
  // width. The port's viewport widens with the aspect (854 at 16:9), which shrank Trilogy's
  // scan brackets by a quarter.
  pixelScale *= PortDisc::IsTrilogy(PortDisc::Current())
                    ? 640.f
                    : static_cast< float >(CGraphics::GetViewport().mWidth);
#else
  pixelScale *= static_cast< float >(CGraphics::GetViewport().mWidth);
#endif
  return scale * (CMath::Clamp(clampMin, pixelScale, clampMax) / pixelScale);
}

CTargetReticleRenderState::CTargetReticleRenderState(TUniqueId target, float radiusWorld,
                                                     CVector3f positionWorld, float factor,
                                                     float minVpClampScale,
                                                     bool orbitZoneIdlePosition)
: x0_target(target)
, x4_radiusWorld(radiusWorld)
, x8_positionWorld(positionWorld)
, x14_factor(factor)
, x18_minVpClampScale(minVpClampScale)
, x1c_orbitZoneIdlePosition(orbitZoneIdlePosition) {}

void CTargetReticleRenderState::InterpolateWithClamp(const CTargetReticleRenderState& a,
                                                     CTargetReticleRenderState& out,
                                                     const CTargetReticleRenderState& b, float t) {
  float t2 = CMath::Clamp(0.f, t, 1.f);
  float omt = 1.f - t2;
  out.x4_radiusWorld = omt * a.x4_radiusWorld + t2 * b.x4_radiusWorld;
  out.x14_factor = omt * a.x14_factor + t2 * b.x14_factor;
  out.x18_minVpClampScale = omt * a.x18_minVpClampScale + t2 * b.x18_minVpClampScale;
  out.x8_positionWorld = CVector3f::Lerp(a.x8_positionWorld, b.x8_positionWorld, t2);
  if (t2 == 1.f)
    out.SetTargetId(b.GetTargetId());
  else if (t2 == 0.f)
    out.SetTargetId(a.GetTargetId());
  else
    out.SetTargetId(kInvalidUniqueId);
}

CTargetingManager::CTargetingManager(const CStateManager& mgr) : x0_targetReticle(mgr) {}

bool CTargetingManager::CheckLoadComplete() {
  return x0_targetReticle.CheckLoadComplete() && x21c_orbitPointMarker.CheckLoadComplete();
}

void CTargetingManager::Update(float dt, const CStateManager& mgr) {
  x0_targetReticle.Update(dt, mgr);
  x21c_orbitPointMarker.Update(dt, mgr);
}

void CTargetingManager::Draw(const CStateManager& mgr, bool hideLockon) const {
  CGraphics::SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  x21c_orbitPointMarker.Draw(mgr);
  const CGameCamera& curCam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  CTransform4f camXf = PortReticleCameraXf(mgr);
  CGraphics::SetViewPointMatrix(camXf);
  CFrustumPlanes frustum(camXf, curCam.GetRenderFov() * 0.01745329238474369f, curCam.GetAspectRatio(),
                         1.f, false, 100.f);
  gpRender->SetClippingPlanes(frustum);
#ifdef TARGET_PC
  gpRender->SetPerspective(CStateManager::PortZoomedFov(curCam.GetRenderFov()), static_cast< float >(CGraphics::GetViewport().mWidth),
                           static_cast< float >(CGraphics::GetViewport().mHeight),
                           curCam.GetNearClipDistance(), curCam.GetFarClipDistance());
#else
  gpRender->SetPerspective(curCam.GetRenderFov(), static_cast< float >(CGraphics::GetViewport().mWidth),
                           static_cast< float >(CGraphics::GetViewport().mHeight),
                           curCam.GetNearClipDistance(), curCam.GetFarClipDistance());
#endif
  x0_targetReticle.Draw(mgr, hideLockon);
}

#ifdef TARGET_PC
// Trilogy scan ring. Behaviour port of the Wii additions to this class; module note
// build/s-trilogy-spike-20261026/notes/tri-scan-reticle.md. CTweakTargeting 0x360 (0.15) is
// hardcoded in the Trilogy tweak ctor fn_802685B8.
static const float skTriScanFadeTime = 0.15f; // tweak 0x360

// Trilogy reads the Wii-remote pointer (player +0xEB8, fn_80015090). The port's pointer is the
// pointer aim's, or the screen centre without it; either is always valid, so fn_80015D20
// (valid) is true. presented takes the pointer as drawn this frame rather than as of the tick.
static CVector3f PortTriPointerDir(const CTransform4f& camXf, const CStateManager& mgr,
                                   bool presented) {
  float x = 0.f;
  float y = 0.f;
  const bool on = presented ? PortDebug::PresentedPointerAimPosition(
                                  CCameraManager::GetPresentationInterpolation(), x, y)
                            : PortDebug::PointerAimPosition(x, y);
  if (!on) {
    return camXf.GetForward();
  }
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  float yaw = 0.f;
  float pitch = 0.f;
  PortMouse::PointerAim::RayAngles(x, y, cam.GetRenderFov(), cam.GetAspectRatio(), yaw, pitch);
  return camXf.Rotate(CVector3f(sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), sinf(pitch)));
}

// The ray follows fn_80015090 from the camera along the pointer direction.
// @ fn_80015090
void CCompoundTargetReticle::PortTriUpdateScanPointer(const CStateManager& mgr) {
  const CTransform4f camXf = PortReticleCameraXf(mgr);
  const CVector3f start = camXf.GetTranslation();
  const CVector3f dir = PortTriPointerDir(camXf, mgr, false);
  const CMaterialFilter filter = CMaterialFilter::MakeIncludeExclude(
      CMaterialList(kMT_Solid, kMT_Character, kMT_NonSolidDamageable),
      CMaterialList(kMT_ProjectilePassthrough, kMT_Player));
  TEntityList nearList;
  mgr.BuildNearList(nearList, start, dir, 100.f /* lbl_805C3910 */, filter, nullptr);
  TUniqueId hitId = kInvalidUniqueId;
  const CRayCastResult result =
      mgr.RayWorldIntersection(hitId, start, dir, 100.f /* lbl_805C3910 */, filter, nearList);
  float dist = 100.f; // lbl_805C3910
  if (result.IsValid()) {
    dist = result.GetTime();
    if (dist < 0.25f /* lbl_805C3914 */) {
      dist = 0.25f;
    }
  }
  xPortTriPointerPos = start + dir * dist;
}

// @ fn_8023BB0C
void CCompoundTargetReticle::PortTriUpdateScan(float dt, const CStateManager& mgr) {
  const CPlayer* player = mgr.GetPlayer();
  // 0x8023BB40: mgr+0x8B4 (player state) ->x18 current visor == 2 (scan)
  if (mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan) {
    x434_scanSize = CVector3f::Zero();       // lbl_804E78B0
    x41c_scanCentre = xPortTriPointerPos;    // fn_80015CF0
    x408_scanLerp = 0.f;                     // lbl_805C6C90
    x440_scanTargetCount = 0;
    return;
  }

  // 0x8023BBA0: player +0x3A4 scanning object, else this+0x456
  const TUniqueId scanningId = player->GetScanningObjectId();
  const TUniqueId id = scanningId != kInvalidUniqueId ? scanningId : x456_scanTargetId;
  int count = 0;
  TUniqueId list[1];
  if (id != kInvalidUniqueId) {
    list[count++] = id;
  }

  bool same = false;
  if (count == x440_scanTargetCount) {
    same = std::equal(list, list + count, x444_scanTargets); // fn_8023BF0C
  }
  if (!same) {
    // 0x8023BC44
    std::copy(list, list + count, x444_scanTargets);
    x440_scanTargetCount = count;
    const float t = x408_scanLerp;
    x428_scanPrevSize = x434_scanSize * (1.f - t) + x428_scanPrevSize * t;
    x410_scanPrevCentre = x41c_scanCentre * (1.f - t) + x410_scanPrevCentre * t;
    x408_scanLerp = 1.f; // lbl_805C6C84
  } else {
    // 0x8023BD54
    x408_scanLerp = rstl::max_val(0.f, x408_scanLerp - dt / skTriScanFadeTime);
  }

  // 0x8023BD80: any listed id a CActor (fn_802A88FC)
  bool anyActor = false;
  for (int i = 0; i < x440_scanTargetCount; ++i) {
    if (TCastToConstPtr< CActor >(mgr.GetObjectById(x444_scanTargets[i]))) {
      anyActor = true;
      break;
    }
  }
  if (x20_prevState == kRS_Scan && anyActor) {
    x404_scanFade = rstl::min_val(1.f, x404_scanFade + dt / skTriScanFadeTime);
  } else {
    x404_scanFade = rstl::max_val(0.f, x404_scanFade - dt / skTriScanFadeTime);
  }

  // 0x8023BE40
  if (scanningId != kInvalidUniqueId) {
    x40c_scanning = rstl::min_val(1.f, x40c_scanning + dt);
  } else {
    x40c_scanning = rstl::max_val(0.f, x40c_scanning - dt);
  }

  // 0x8023BE94: player +0x338 orbit next target while the pointer is valid (always, here)
  if (mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_Scan) {
    const TUniqueId next = player->GetOrbitNextTargetId();
    if (next != x454_lastNextTargetId) {
      x454_lastNextTargetId = next;
      x456_scanTargetId = PortTriResolveScanTarget(next, mgr);
    }
  }
}

// @ fn_8023C494
TUniqueId CCompoundTargetReticle::PortTriResolveScanTarget(TUniqueId id,
                                                           const CStateManager& mgr) {
  const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(id));
  if (!actor) {
    return kInvalidUniqueId;
  }
  // fn_8000B328 scan info; x28 bit7 active
  if (actor->GetScannableObjectInfo() && actor->GetActive()) {
    return id;
  }
  // fn_8008EC84(actor, mgr, 0x12 kSS_Play, 1 kSM_Activate, fn_8023C58C): first active
  // CScriptPointOfInterest (type 0x2B) the actor activates on Play.
  const rstl::vector< SConnection >& conns = actor->GetConnectionList();
  for (rstl::vector< SConnection >::const_iterator conn = conns.begin(); conn != conns.end();
       ++conn) {
    if (conn->x0_state != kSS_Play || conn->x4_msg != kSM_Activate) {
      continue;
    }
    CStateManager::TIdListResult search = mgr.GetIdListForScript(conn->x8_objId);
    for (CStateManager::TIdList::const_iterator it = search.first; it != search.second; ++it) {
      const CScriptPointOfInterest* poi =
          TCastToConstPtr< CScriptPointOfInterest >(mgr.GetObjectById(it->second));
      if (poi && poi->GetActive()) {
        return it->second;
      }
    }
  }
  return kInvalidUniqueId;
}

// @ fn_8023BF48
void CCompoundTargetReticle::PortTriUpdateScanBounds(const CStateManager& mgr) {
  if (mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan) {
    return;
  }
  CVector3f centre = xPortTriPointerPos;
  CVector3f size = CVector3f::Zero();
  const TUniqueId first = x440_scanTargetCount != 0 ? x444_scanTargets[0] : kInvalidUniqueId;
  if (TCastToConstPtr< CActor >(mgr.GetObjectById(first))) {
    // lbl_804DF2E8 = (FLT_MAX x3, -FLT_MAX x3), set by fn_802C8734 (802C8750)
    CAABox accum = CAABox::MakeMaxInvertedBox();
    bool any = false;
    for (int i = 0; i < x440_scanTargetCount; ++i) {
      const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(x444_scanTargets[i]));
      if (!actor) {
        continue;
      }
      const CModelData* modelData = actor->GetModelData(); // actor +0x5C
      if (!modelData || modelData->IsNull()) {
        continue;
      }
      const CVector3f scale = modelData->GetScale();
      if (const CAnimData* animData = modelData->GetAnimationData()) {
        // fn_80018AD8, fn_80348D94 on both corners
        const CAABox local = animData->GetBoundingBox();
        const CTransform4f& xf = actor->GetTransform();
        const CVector3f p0 = xf * CVector3f::ByElementMultiply(scale, local.GetMinPoint());
        const CVector3f p1 = xf * CVector3f::ByElementMultiply(scale, local.GetMaxPoint());
        const CVector3f lo(rstl::min_val(p0.GetX(), p1.GetX()), rstl::min_val(p0.GetY(), p1.GetY()),
                           rstl::min_val(p0.GetZ(), p1.GetZ()));
        const CVector3f hi(rstl::max_val(p0.GetX(), p1.GetX()), rstl::max_val(p0.GetY(), p1.GetY()),
                           rstl::max_val(p0.GetZ(), p1.GetZ()));
        accum.AccumulateBounds(lo); // fn_802C7CB8
        accum.AccumulateBounds(hi);
      } else {
        // fn_80139524 GetBounds(xf) already scales; Trilogy scales the extent again.
        const CAABox b = modelData->GetBounds(actor->GetTransform());
        const CVector3f c = b.GetCenterPoint(); // fn_802C8250
        const CVector3f half =
            CVector3f::ByElementMultiply((b.GetMaxPoint() - b.GetMinPoint()) * 0.5f /* lbl_805C6C80 */, scale);
        accum.AccumulateBounds(c - half);
        accum.AccumulateBounds(c + half);
      }
      any = true;
    }
    if (any) {
      centre = accum.GetCenterPoint();
      // fn_8023A034
      const CAABox t = accum.GetTransformedAABox(PortReticleCameraXf(mgr));
      size = t.GetMaxPoint() - t.GetMinPoint();
      // fn_800C1230: behind the camera → pointer, unit size (lbl_804E7904)
      const CVector3f proj =
          mgr.GetCameraManager()->GetCurrentCamera(mgr).ConvertToScreenSpace(centre);
      if (proj.GetZ() >= 1.f /* lbl_805C6C84 */) {
        centre = xPortTriPointerPos;
        size = CVector3f(1.f, 1.f, 1.f);
      }
    }
  }
  x41c_scanCentre = centre;
  x434_scanSize = size;
}

// Trilogy CTweakTargeting fields GC lacks, hardcoded in the tweak ctor fn_802685B8.
static const float skTriScanOverrideAlpha = 0.18f;  // 0x35c (0x8026937C/0x8026944C)
static const float skTriScanRingScale = 0.25f;      // 0x364 (0x80269454)
static const CColor skTriScanRingColor(uchar(0x56), uchar(0xC1), uchar(0xFB), uchar(0x9F));    // 0x368 (0x80268EAC..)
static const CColor skTriScanBracketColor(uchar(0x49), uchar(0xC3), uchar(0xF6), uchar(0xA0)); // 0x36c
static const CColor skTriScanLockedColor(uchar(0x49), uchar(0xC3), uchar(0xF6), uchar(0x31));  // 0x370 (..0x80268ED8)
static const CColor skTriScanOverrideColor(uchar(178), uchar(178), uchar(15), uchar(255)); // lbl_805C2758

// psq_st ...,1,qr2: float to u8, saturating, truncating.
static uchar PortTriAlphaU8(float a) {
  return static_cast< uchar >(CMath::Clamp(0.f, a, 255.f));
}

// The camera Trilogy reads (fn_8006298C, cam +0x2C) is the port's reticle view, the frame the
// pointer ray (PortTriUpdateScanPointer) and Draw's view matrix use.
// @ fn_80239C84
static float PortTriScanRingDistance(const CStateManager& mgr) {
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  // Trilogy's GetOrbitZoneHeight (fn_80254158) scales the tweak value by 0.65 (lbl_805C6EE0)
  // and truncates (fctiwz): 180 -> 116 (0.65f * 180 is 116.99999 in single precision), so the
  // ring sits farther out and draws smaller than the raw tweak value would put it.
  const int height = static_cast< int >(0.65f * CCast::LtoF(gpTweakPlayer->GetOrbitZoneHeight(0)));
  float dist = 224.f /* lbl_805C6CF0 */ / CCast::LtoF(height);
  return dist / CMath::SlowTangentR(cam.GetRenderFov() * 0.5f /* lbl_805C6C80 */ *
                                    (1.f / 360.f) /* lbl_805C6CF4 */ * (2.f * M_PIF) /* lbl_805C6C94 */);
}

// @ fn_80239DB0
static CVector3f PortTriProjectScanCorner(const CVector3f& world, const CStateManager& mgr,
                                          float radius) {
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  const CTransform4f camXf = PortReticleCameraXf(mgr);
  const CVector3f view = camXf.TransposeRotate(world - camXf.GetTranslation());
  const CVector3f ndc = cam.GetPerspectiveMatrix().MultiplyOneOverW(view);
  const CVector3f flat(ndc.GetX(), ndc.GetY(), 0.f);
  if (flat.MagSquared() <= radius * radius) {
    return world;
  }
  // Pull the point sideways in the camera's XZ plane until it sits on the NDC disc.
  const CVector3f c(view.GetZ(), 0.f, -view.GetX()); // cross(view, lbl_804E78EC (0,1,0))
  const float tangLen = c.Magnitude();
  const CVector3f nc = c * (1.f / tangLen);
  const CVector3f n(nc.GetZ(), 0.f, -nc.GetX());
  const float flatLen = flat.Magnitude();
  const float k = tangLen * ((flatLen - radius) / flatLen);
  return camXf * (view + k * n);
}

// @ fn_80238DD4
void CCompoundTargetReticle::PortTriDrawScan(const CMatrix3f& rot, const CStateManager& mgr) const {
  if (x28_noDrawTicks > 0) {
    return;
  }
  if (mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan) {
    return;
  }
  if (x20_prevState != kRS_Scan) {
    return;
  }

  // 0x80238EDC..0x80238F7C
  const_cast< TCachedToken< CModel >& >(x3e0_scanRing).TryCache();
  const_cast< TCachedToken< CModel >& >(x3ec_scanBracket).TryCache();
  const_cast< TCachedToken< CModel >& >(x3f8_scanProgress).TryCache();
  const CModel* ring = x3e0_scanRing.GetObject();
  const CModel* bracket = x3ec_scanBracket.GetObject();
  // The progress model must be loaded but is never drawn (0x80238FD0).
  if (!ring || !bracket || !x3f8_scanProgress.GetObject()) {
    return;
  }

  // 0x80238F9C: Wii pointer intensity (fn_80015D2C, pointer +0x94) while not scanning. The
  // port's pointer is always on screen, so it is 1. player +0x11A4 (0x80238FAC) is the orbit
  // lock fade (fn_8019BDFC): the ring and brackets fade out as the lock settles.
  const bool scanning = mgr.GetPlayer()->GetScanningObjectId() != kInvalidUniqueId;
  const float pointerIntensity = 1.f;
  const float intensity =
      (scanning ? 1.f : pointerIntensity) * (1.f - mgr.GetPlayer()->PortTriOrbitFade());

  // fn_80239B84: the ring sits on the pointer ray at the orbit-ring distance. The port takes
  // the pointer as drawn, so the ring stays on the scan window that follows it between ticks.
  const CTransform4f camXf = PortReticleCameraXf(mgr);
  const CVector3f ringPos =
      camXf.GetTranslation() + PortTriScanRingDistance(mgr) * PortTriPointerDir(camXf, mgr, true);

  // 0x80239014..0x802390AC
  CColor ringColor = CColor::Lerp(skTriScanRingColor, skTriScanLockedColor, x404_scanFade);
  ringColor.SetAlpha(PortTriAlphaU8(intensity * ringColor.GetAlphau8()));
  // The brackets lerp the other way (0x80239050: from 0x370 to 0x36c): faint on the pointer,
  // full once they frame a target.
  CColor bracketColor = CColor::Lerp(skTriScanLockedColor, skTriScanBracketColor, x404_scanFade);
  bracketColor.SetAlpha(PortTriAlphaU8(intensity * bracketColor.GetAlphau8()));
  // 0x80239094: Trilogy recolours both while not scanning and its counter x388 runs. x388 only
  // counts while the Wii pointer is off screen (fn_80015DAC), which the port's never is.
  const int overrideTicks = 0; // x388
  if (!scanning && overrideTicks != 0) {
    const uchar a = PortTriAlphaU8(intensity * skTriScanOverrideAlpha *
                                   skTriScanOverrideColor.GetAlphau8());
    ringColor = skTriScanOverrideColor;
    ringColor.SetAlpha(a);
    bracketColor = ringColor;
  }

  // 0x80239138..0x802391E4
  const float t = x408_scanLerp;
  const float it = 1.f - t;
  const CVector3f centre = x41c_scanCentre * it + x410_scanPrevCentre * t;
  const CVector3f size = x434_scanSize * it + x428_scanPrevSize * t;
  const float s = CalculateClampedScale(centre, 1.f, 120.f /* lbl_805C6CC8 */,
                                        120.f /* lbl_805C6CC8 */, mgr);
  const float hx = 0.5f * rstl::max_val(0.f, 0.85f /* lbl_805C6CDC */ * size.GetX());
  const float hz = 0.5f * rstl::max_val(0.f, 0.85f /* lbl_805C6CDC */ * size.GetZ());
  const CVector3f halfExt(hx, 0.f, -hz);

  // 0x80239284..0x802392E0
  gpRender->SetModelMatrix(CTransform4f(rot, ringPos) *
                           CTransform4f::Scale(1.8f /* lbl_805C6CE0 */ * skTriScanRingScale));
  // CModelFlags(7, 0, 0, colour): no depth compare or update, so the HUD draws over the world.
  ring->Draw(CModelFlags::AdditiveDepthCompareUpdate(ringColor, false, false));

  // 0x802392F0..0x80239490: project the box corners, clamped to the NDC unit disc.
  CVector3f corners[4];
  int n = 0;
  if (x404_scanFade > 0.f) {
    for (int i = 0; i < 4; ++i) {
      const float cx = (i & 1) ? -1.f : 1.f;
      const float cz = i < 2 ? -1.f : 1.f;
      const CVector3f world =
          centre + rot * CVector3f(halfExt.GetX() * cx, 0.f, halfExt.GetZ() * cz);
      const CVector3f proj = PortTriProjectScanCorner(world, mgr, 1.f);
      const CVector3f local = rot.GetTranspose() * (proj - centre); // fn_80305644
      corners[n++] = CVector3f(local.GetX() * cx, local.GetY(), local.GetZ() * cz);
    }
  }

  // 0x80239498..0x802395C0
  if (n == 4) {
    const float xA = rstl::max_val(rstl::min_val(corners[0].GetX(), corners[2].GetX()), -100.f);
    const float xB = rstl::max_val(rstl::min_val(corners[1].GetX(), corners[3].GetX()), -100.f);
    const float zA = rstl::min_val(rstl::max_val(corners[1].GetZ(), corners[0].GetZ()), 100.f);
    const float zB = rstl::min_val(rstl::max_val(corners[3].GetZ(), corners[2].GetZ()), 100.f);
    corners[0] = CVector3f(xA, 0.f, zA);
    corners[1] = CVector3f(xB, 0.f, zA);
    corners[2] = CVector3f(xA, 0.f, zB);
    corners[3] = CVector3f(xB, 0.f, zB);
  }

  // 0x802395C4..0x802397C8
  const float invS = 1.f / s;
  CColor drawColor = bracketColor;
  drawColor.SetAlpha(PortTriAlphaU8((1.f - x40c_scanning) * bracketColor.GetAlphau8()));
  for (int i = 0; i < 4; ++i) {
    const float cx = (i & 1) ? -1.f : 1.f;
    const float cz = i < 2 ? -1.f : 1.f;
    // Without projected corners the bracket sits at the unprojected half extent (0x80239644).
    const CVector3f pos = n != 0 ? corners[i] * x404_scanFade : halfExt;
    gpRender->SetModelMatrix(CTransform4f(rot * CMatrix3f::Scale(s), centre) *
                             CTransform4f::Scale(cx, 1.f, cz) *
                             CTransform4f::Translate(pos * invS));
    bracket->Draw(CModelFlags::AdditiveDepthCompareUpdate(drawColor, false, false));
  }
}
#endif

void CCompoundTargetReticle::Touch() const {
  if (x34_crosshairs.GetObject()) {
    x34_crosshairs.GetObject()->Touch(0);
  }
  if (x40_seeker.GetObject()) {
    x40_seeker.GetObject()->Touch(0);
  }
  if (x4c_lockConfirm.GetObject()) {
    x4c_lockConfirm.GetObject()->Touch(0);
  }
  if (x58_targetFlower.GetObject()) {
    x58_targetFlower.GetObject()->Touch(0);
  }
  if (x64_missileBracket.GetObject()) {
    x64_missileBracket.GetObject()->Touch(0);
  }
  if (x70_innerBeamIcon.GetObject()) {
    x70_innerBeamIcon.GetObject()->Touch(0);
  }
  if (x7c_lockFire.GetObject()) {
    x7c_lockFire.GetObject()->Touch(0);
  }
  if (x88_lockDagger.GetObject()) {
    x88_lockDagger.GetObject()->Touch(0);
  }
  if (x94_grapple.GetObject()) {
    x94_grapple.GetObject()->Touch(0);
  }
  if (xa0_chargeTickFirst.GetObject()) {
    xa0_chargeTickFirst.GetObject()->Touch(0);
  }
  if (xac_xrayRetRing.GetObject()) {
    xac_xrayRetRing.GetObject()->Touch(0);
  }
  if (xb8_thermalReticle.GetObject()) {
    xb8_thermalReticle.GetObject()->Touch(0);
  }
  if (xc4_chargeGauge.x0_model.GetObject()) {
    xc4_chargeGauge.x0_model.GetObject()->Touch(0);
  }
  for (AUTO(it, xe0_outerBeamIconSquares.begin()); it != xe0_outerBeamIconSquares.end(); ++it) {
    if (it->x0_model.GetObject()) {
      it->x0_model.GetObject()->Touch(0);
    }
  }
#ifdef TARGET_PC
  if (x3e0_scanRing.GetObject()) {
    x3e0_scanRing.GetObject()->Touch(0);
  }
  if (x3ec_scanBracket.GetObject()) {
    x3ec_scanBracket.GetObject()->Touch(0);
  }
  if (x3f8_scanProgress.GetObject()) {
    x3f8_scanProgress.GetObject()->Touch(0);
  }
#endif
}

void CTargetingManager::Touch() const { x0_targetReticle.Touch(); }

COrbitPointMarker::COrbitPointMarker()
: x0_zOffset(gpTweakTargeting->x50_orbitPointZOffset)
, x4_camRelZPos(true)
, x8_lagAzimuth(0.f)
, xc_azimuth(0.f)
, x10_lagTargetPos(CVector3f::Zero())
, x1c_lastFreeOrbit(false)
, x20_interpTimer(0.f)
, x24_curTime(0.f)
, x28_orbitPointModel(gpSimplePool->GetObj(skOrbitPointAssetName)) {
  x28_orbitPointModel.Lock();
}

bool COrbitPointMarker::CheckLoadComplete() { return x28_orbitPointModel.TryCache(); }

void COrbitPointMarker::Update(float dt, const CStateManager& mgr) {
  x24_curTime += dt;
  const CPlayer* player = mgr.GetPlayer();
  CPlayer::EPlayerOrbitState orbitState = player->GetOrbitState();
  const CGameCamera& curCam = mgr.GetCameraManager()->GetCurrentCamera(mgr);

  bool freeOrbit =
      (orbitState == CPlayer::kOS_OrbitPoint || orbitState == CPlayer::kOS_OrbitCarcass);

  if (x1c_lastFreeOrbit != freeOrbit) {
    if (orbitState == CPlayer::kOS_OrbitPoint || orbitState == CPlayer::kOS_OrbitCarcass) {
      ResetInterpolationTimer(gpTweakTargeting->x54_orbitPointInTime);
      x10_lagTargetPos = !x4_camRelZPos
                             ? player->GetHUDOrbitTargetPosition() + CVector3f(0.f, 0.f, x0_zOffset)
                             : CVector3f(player->GetHUDOrbitTargetPosition().GetX(),
                                         player->GetHUDOrbitTargetPosition().GetY(),
                                         x0_zOffset + curCam.GetTranslation().GetZ());
      CEulerAngles euler =
          CEulerAngles::FromQuaternion(CQuaternion::FromMatrix(curCam.GetTransform()));
      x8_lagAzimuth = CMath::Deg2Rad(45.f) + euler.GetZ();
    } else {
      ResetInterpolationTimer(gpTweakTargeting->x58_orbitPointOutTime);
    }
    x1c_lastFreeOrbit = !x1c_lastFreeOrbit;
  }

  if (x20_interpTimer > 0.f) {
    x20_interpTimer = rstl::max_val(0.f, x20_interpTimer - dt);
  }

  if (!x4_camRelZPos) {
    CVector3f orbitPos = player->GetHUDOrbitTargetPosition();
    float targetZ = x0_zOffset + orbitPos.GetZ();
    float delta = targetZ - x10_lagTargetPos.GetZ();
    if (delta < 0.1f) {
      x10_lagTargetPos = orbitPos + CVector3f(0.f, 0.f, x0_zOffset);
    } else if (delta < 0.f) {
      x10_lagTargetPos =
          CVector3f(orbitPos.GetX(), orbitPos.GetY(), x10_lagTargetPos.GetZ() - 0.1f);
    } else {
      x10_lagTargetPos =
          CVector3f(orbitPos.GetX(), orbitPos.GetY(), x10_lagTargetPos.GetZ() + 0.1f);
    }
  } else {
    x10_lagTargetPos = CVector3f(player->GetHUDOrbitTargetPosition().GetX(),
                                 player->GetHUDOrbitTargetPosition().GetY(),
                                 x0_zOffset + player->GetHUDOrbitTargetPosition().GetZ());
  }

  if (x1c_lastFreeOrbit) {
    CEulerAngles euler =
        CEulerAngles::FromQuaternion(CQuaternion::FromMatrix(curCam.GetTransform()));
    float newAzimuth = CMath::Deg2Rad(45.f) + euler.GetZ();
    float aziDelta = newAzimuth - xc_azimuth;
    if (mgr.GetPlayer()->IsInFreeLook()) {
      x8_lagAzimuth += aziDelta;
    }
    xc_azimuth = newAzimuth;
  }
}

void COrbitPointMarker::Draw(const CStateManager& mgr) const {
  if ((x1c_lastFreeOrbit || x20_interpTimer > 0.f) && gpTweakTargeting->x1c8_drawOrbitPoint) {
    const_cast< TCachedToken< CModel >& >(x28_orbitPointModel).TryCache();
    if (x28_orbitPointModel.GetObject() != NULL) {
      const CGameCamera& curCam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
      CTransform4f camXf = PortReticleCameraXf(mgr);
      CGraphics::SetViewPointMatrix(camXf);
      {
        CFrustumPlanes frustum(camXf, curCam.GetRenderFov() * 0.01745329238474369f,
                               curCam.GetAspectRatio(), 1.f, false, 100.f);
        gpRender->SetClippingPlanes(frustum);
#ifdef TARGET_PC
        gpRender->SetPerspective(CStateManager::PortZoomedFov(curCam.GetRenderFov()),
                                 static_cast< float >(CGraphics::GetViewport().mWidth),
                                 static_cast< float >(CGraphics::GetViewport().mHeight),
                                 curCam.GetNearClipDistance(), curCam.GetFarClipDistance());
#else
        gpRender->SetPerspective(curCam.GetRenderFov(),
                                 static_cast< float >(CGraphics::GetViewport().mWidth),
                                 static_cast< float >(CGraphics::GetViewport().mHeight),
                                 curCam.GetNearClipDistance(), curCam.GetFarClipDistance());
#endif
      }

      float scale;
      if (x1c_lastFreeOrbit) {
        scale = 1.f - x20_interpTimer / gpTweakTargeting->x54_orbitPointInTime;
      } else {
        scale = x20_interpTimer / gpTweakTargeting->x58_orbitPointOutTime;
      }

      const CTweakTargeting* pTweaks = gpTweakTargeting;
      CTransform4f modelXf = CTransform4f::RotateZ(CRelAngle(x8_lagAzimuth));
      modelXf.ScaleBy(scale);
      modelXf.AddTranslation(x10_lagTargetPos);
      gpRender->SetModelMatrix(modelXf);

      CModel* model = x28_orbitPointModel.GetObject();
      CColor color = pTweaks->x1bc_orbitPointColor.WithAlphaModulatedBy(scale);
      CModelFlags flags = CModelFlags::Additive(color).DepthCompareUpdate(false, false);
      model->Draw(flags);
    }
  }
}

void COrbitPointMarker::ResetInterpolationTimer(float time) { x20_interpTimer = time; }
