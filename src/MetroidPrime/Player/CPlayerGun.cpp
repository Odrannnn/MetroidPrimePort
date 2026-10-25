#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "port_debug.h"
#include "port_power_charge_release.h"
#include "port_room_env.h"
#include "port_shot_aim.h"
#include "port_synthetic_effect.h"
#include "port_third_person.h"
#include "port_third_person_body.h"
#ifdef MP_ENABLE_SMOKE_DRIVER
#include "port_smoke.h"
#endif

#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CExplosion.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimRes.hpp"
#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/CFluidPlaneCPU.hpp"
#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CRainSplashGenerator.hpp"
#include "MetroidPrime/CRumbleManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CWorldShadow.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Enemies/CMetroid.hpp"
#include "MetroidPrime/Enemies/CMetroidBeta.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"
#include "MetroidPrime/Player/CGrappleArm.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/Player/CMorphBall.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/SFX/PhazonGun.h"
#include "MetroidPrime/SFX/Weapons.h"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/ScriptObjects/CScriptWater.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/TGameTypes.hpp"
#include "MetroidPrime/Tweaks/CTweakGunRes.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayerGun.hpp"
#include "MetroidPrime/Weapons/CAuxWeapon.hpp"
#include "MetroidPrime/Weapons/CBomb.hpp"
#include "MetroidPrime/Weapons/CEnergyProjectile.hpp"
#include "MetroidPrime/Weapons/CIceBeam.hpp"
#include "MetroidPrime/Weapons/CPhazonBeam.hpp"
#include "MetroidPrime/Weapons/CPlasmaBeam.hpp"
#include "MetroidPrime/Weapons/CPowerBeam.hpp"
#include "MetroidPrime/Weapons/CPowerBomb.hpp"
#include "MetroidPrime/Weapons/CWaveBeam.hpp"
#include "MetroidPrime/Weapons/GunController/CGunMotion.hpp"
#include "MetroidPrime/Weapons/WeaponTypes.hpp"

#include "Kyoto/Animation/CPrimitive.hpp"
#include "Kyoto/Audio/CAudioSys.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Basics/CCast.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CAbsAngle.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Text/CStringTable.hpp"

#include "Collision/CMaterialFilter.hpp"
#include "Collision/CMaterialList.hpp"

#include "MetaRender/CCubeRenderer.hpp"

#include "Weapons/IWeaponRenderer.hpp"

#include "rstl/set.hpp"

#include "dolphin/gx/GXFrameBuffer.h"
#include "dolphin/gx/GXManage.h"

#include "math.h"

static const char* const kGunLocator = "GBSE_SDK";
const float CPlayerGun::kGunScale = 2.f;

static float kVerticalAngleTable[3] = {-30.f, 0.f, 30.f};
static float kHorizontalAngleTable[3] = {30.f, 30.f, 30.f};
static float kVerticalVarianceTable[3] = {30.f, 30.f, 30.f};

static const float chargeShakeTbl[3] = {
    -0.001f,
    0.f,
    0.001f,
};

const uint CPlayerGun::mHandAnimId[4] = {
    0,
    1,
    2,
    1,
};

static const CPlayerState::EItemType mBeamArr[4] = {
    CPlayerState::kIT_PowerBeam,
    CPlayerState::kIT_IceBeam,
    CPlayerState::kIT_WaveBeam,
    CPlayerState::kIT_PlasmaBeam,
};

static const CPlayerState::EItemType mBeamComboArr[4] = {
    CPlayerState::kIT_SuperMissile,
    CPlayerState::kIT_IceSpreader,
    CPlayerState::kIT_Wavebuster,
    CPlayerState::kIT_Flamethrower,
};

static const ControlMapper::ECommands mBeamCtrlCmd[4] = {
    ControlMapper::kC_PowerBeam,
    ControlMapper::kC_IceBeam,
    ControlMapper::kC_WaveBeam,
    ControlMapper::kC_PlasmaBeam,
};

static const ushort mToMissileSound[4] = {
    SFXsam_b_misswitch_00,
    SFXsam_b_misswitch_10,
    SFXsam_b_misswitch_20,
    SFXsam_b_misswitch_30,
};

static const ushort mFromMissileSound[4] = {
    SFXsam_b_misswitch_01,
    SFXsam_b_misswitch_11,
    SFXsam_b_misswitch_21,
    SFXsam_b_misswitch_31,
};

float CPlayerGun::kTractorBeamFactor = 0.25f / CPlayerState::GetMissileComboChargeFactor();
CVector3f CPlayerGun::kScaleVector(2.f, 2.f, 2.f);
float CPlayerGun::CMotionState::gGunExtendDistance = 0.125f;

static CColor kArmColor = CColor(0.75f, 0.5f, 0.f, 1.f);
static CMaterialFilter sAimFilter = CMaterialFilter::MakeIncludeExclude(
    CMaterialList(kMT_Solid), CMaterialList(kMT_ProjectilePassthrough));
static const float kChargeSpeed = 1.f / CPlayerState::GetMissileComboChargeFactor();
static const float kChargeStart = 0.025f / CPlayerState::GetMissileComboChargeFactor();
static const float kChargeFxStart = 1.f / CPlayerState::GetMissileComboChargeFactor();
#ifdef TARGET_PC
// Port: Metroid Prime Remastered's per-beam charge tunables (CPlayerGunMP1, in
// frames at 60 fps). With PortDebug::RapidCharge, holding fire after the press
// shot fires `shots` more, each `shotFrames[i]` after the last, and only then
// charges: anim and sound at `triggered`, charge FX at `warmUp`, full at `full`
// (retail: 15/60/108). Ice has no shots and keeps retail's timing, as there.
struct SPortRapidCharge {
  int shots;
  int shotFrames[3];
  int triggered;
  int warmUp;
  int full;
};
static const SPortRapidCharge skPortRapidCharge[4] = {
    {2, {10, 10, 0}, 22, 22, 82},  // Power
    {0, {0, 0, 0}, 15, 60, 108},  // Ice
    {1, {30, 0, 0}, 32, 32, 82},  // Wave
    {1, {22, 0, 0}, 38, 38, 82},  // Plasma
};
static const SPortRapidCharge* PortRapidChargeFor(CPlayerState::EBeamId beam) {
  return beam >= CPlayerState::kBI_Power && beam <= CPlayerState::kBI_Plasma
             ? &skPortRapidCharge[int(beam)]
             : nullptr;
}

// Port: set each frame when the gun runs Remastered's effects (its holo
// transition PART is the Remastered one), so the gun moves as there too.
static bool sPortRemasteredGun = false;
#endif

static const CPlayerState::EItemType skItemArr[2] = {
    CPlayerState::kIT_Invalid,
    CPlayerState::kIT_Missiles,
};

static const CPlayerState::EBeamId mCurrentBeamId[5] = {
    CPlayerState::kBI_Power,  CPlayerState::kBI_Ice,   CPlayerState::kBI_Wave,
    CPlayerState::kBI_Plasma, CPlayerState::kBI_Power,
};

static const CModelFlags kThermalFlags[4] = {
    CModelFlags::Normal(),
    CModelFlags::AlphaBlended(CColor(u8(0), u8(0), u8(0), u8(128))),
    CModelFlags::Normal(),
    CModelFlags::Normal(),
};

static const CModelFlags kHandThermalFlag = CModelFlags::Additive(CColor::White());
static const CModelFlags kHandHoloFlag = CModelFlags::ColorModulate(kArmColor);

static const ushort mItemEmptySound[2] = {
    CSfxManager::kInternalInvalidSfxId,
    SFXsam_a_mislemp_00,
};

static const ushort mIntoBeamSound[4] = {
    CSfxManager::kInternalInvalidSfxId,
    SFXsam_b_iceswitch_00,
    SFXsam_b_wavswitch_00,
    SFXsam_b_plaswitch_00,
};

static const ushort mFromBeamSound[4] = {
    CSfxManager::kInternalInvalidSfxId,
    SFXsam_b_iceswitch_01,
    SFXsam_b_wavswitch_01,
    SFXsam_b_plaswitch_01,
};

static ushort sBeamChargeUpSound[4] = {
    SFXsam_a_cbmcharge_lp_00,
    SFXsam_a_icecharge_lp_00,
    SFXsam_a_wavcharge_lp_00,
    SFXsam_a_placharge_lp_00,
};

static const float kDepthFar = 1.f;
static const float kDepthWorld = 1.f / 8.f;
static const float kDepthGun = 1.f / 32.f;

extern void DrawClipCube(const CAABox& aabb);

CPlayerGun::CPlayerGun(TUniqueId playerId)
: x0_lights(8, CVector3f::Zero(), 4, 4, CActorLights::kDefaultPositionUpdateThreshold, false, false,
            false)
, x2ec_lastFireButtonStates(0)
, x2f0_pressedFireButtonStates(0)
, x2f4_fireButtonStates(0)
, x2f8_stateFlags(0x1)
, x2fc_fidgetAnimBits(0)
, x300_remainingMissiles(0)
, x304_(0)
, x308_bombCount(3)
, x30c_rapidFireShots(0)
, x310_currentBeam(CPlayerState::kBI_Power)
, x314_nextBeam(CPlayerState::kBI_Power)
, x318_comboAmmoIdx(0)
, x31c_missileMode(EMissileMode(x318_comboAmmoIdx))
, x320_currentAuxBeam(x310_currentBeam)
, x324_idleState(kIS_Four)
, x328_animSfxPitch(0x2000)
, x32c_chargePhase(kCP_NotCharging)
, x330_chargeState(kCS_Normal)
, x334_(0)
, x338_nextState(kNS_StatusQuo)
, x33c_phazonBeamState(kPBS_Inactive)
, x340_chargeBeamFactor(0.f)
, x344_comboXferTimer(0.f)
, x348_chargeCooldownTimer(0.f)
, x34c_shakeX(0.f)
, x350_shakeZ(0.f)
, x354_bombFuseTime(gpTweakPlayerGun->GetBombFuseTime())
, x358_bombDropDelayTime(gpTweakPlayerGun->GetBombDropDelayTime())
, x35c_bombTime(0.f)
, x360_(0.f)
, x364_gunStrikeCoolTimer(0.f)
, x368_idleWanderDelayTimer(0.f)
, x36c_(1.f)
, x370_gunMotionSpeedMult(1.f)
, x374_(0.f)
, x378_shotSmokeStartTimer(0.f)
, x37c_rapidFireShotsDecayTimer(0.f)
, x380_shotSmokeTimer(0.f)
, x384_gunStrikeDelayTimer(0.f)
, x388_enterFreeLookDelayTimer(0.f)
, x38c_muzzleEffectVisTimer(0.f)
, x390_cooldown(0.f)
, x394_damageTimer(0.f)
, x398_damageAmt(0.f)
, x39c_phazonMorphT(0.f)
, x3a0_missileExitTimer(0.f)
, x3dc_damageLocation(CVector3f::Zero())
, x3e8_xf(CTransform4f::Identity())
, x418_beamLocalXf(CTransform4f::Identity())
, x448_elbowWorldXf(CTransform4f::Identity())
, x478_assistAimXf(CTransform4f::Identity())
, x4a8_gunWorldXf(CTransform4f::Identity())
, x4d8_gunLocalXf(CTransform4f::Identity())
, x508_elbowLocalXf(CTransform4f::Identity())
, x538_playerId(playerId)
, x53a_powerBomb(kInvalidUniqueId)
, x53c_lightId(kInvalidUniqueId)
, x550_camBob(CPlayerCameraBob::kCBT_One,
              CVector2f(CPlayerCameraBob::kCameraBobExtentX, CPlayerCameraBob::kCameraBobExtentY),
              CPlayerCameraBob::kCameraBobPeriod)
, x658_(1)
, x65c_(0.f)
, x660_(0.f)
, x664_(0.f)
, x668_aimVerticalSpeed(gpTweakPlayerGun->GetAimVerticalSpeed())
, x66c_aimHorizontalSpeed(gpTweakPlayerGun->GetAimHorizontalSpeed())
, x670_animSfx(static_cast< TSfxId >(0xffff), CSfxHandle())
, x678_morph(gpTweakPlayerGun->GetGunTransformTime(), gpTweakPlayerGun->GetHoloHoldTime())
, x6a0_motionState()
, x6c8_hologramClipCube(CVector3f(-0.29329199f, 0.f, -0.2481945f),
                        CVector3f(0.29329199f, 1.292392f, 0.2481945f))
, x6e0_rightHandModel(CAnimRes(gpTweakGunRes->xc_rightHand, CAnimRes::kDefaultCharIdx,
                               CVector3f(3.f, 3.f, 3.f), 0, true))
, x72c_currentBeam(nullptr)
, x730_outgoingBeam(nullptr)
, x734_loadingBeam(nullptr)
, x738_nextBeam(nullptr)
, x73c_gunMotion(rs_new CGunMotion(gpTweakGunRes->x4_gunMotion, kScaleVector))
, x740_grappleArm(rs_new CGrappleArm(kScaleVector))
, x744_auxWeapon(rs_new CAuxWeapon(playerId))
, x748_rainSplashGenerator(rs_new CRainSplashGenerator(kScaleVector, 20, 2, 0.f, 0.125f))
, x74c_powerBeam(rs_new CPowerBeam(gpTweakGunRes->x10_powerBeam, kWT_Power, playerId, kMT_Player,
                                   kScaleVector))
, x750_iceBeam(
      rs_new CIceBeam(gpTweakGunRes->x14_iceBeam, kWT_Ice, playerId, kMT_Player, kScaleVector))
, x754_waveBeam(
      rs_new CWaveBeam(gpTweakGunRes->x18_waveBeam, kWT_Wave, playerId, kMT_Player, kScaleVector))
, x758_plasmaBeam(rs_new CPlasmaBeam(gpTweakGunRes->x1c_plasmaBeam, kWT_Plasma, playerId,
                                     kMT_Player, kScaleVector))
, x75c_phazonBeam(rs_new CPhazonBeam(gpTweakGunRes->x20_phazonBeam, kWT_Phazon, playerId,
                                     kMT_Player, kScaleVector))
, x760_selectableBeams(nullptr)
, x774_holoTransitionGen(rs_new CElementGen(
      gpSimplePool->GetObj(SObjectTag('PART', gpTweakGunRes->x24_holoTransition))))
, x82c_shadow(rs_new CWorldShadow(32, 32, true))
, x830_chargeRumbleHandle(-1)
, x832_24_coolingCharge(false)
, x832_25_chargeEffectVisible(false)
, x832_26_comboFiring(false)
, x832_27_chargeAnimStarted(false)
, x832_28_readyForShot(false)
, x832_29_lockedOn(false)
, x832_30_requestReturnToDefault(false)
, x832_31_inRestPose(true)
, x833_24_notFidgeting(true)
, x833_25_(false)
, x833_26_(false)
, x833_27_(false)
, x833_28_phazonBeamActive(false)
, x833_29_pointBlankWorldSurface(false)
, x833_30_canShowAuxMuzzleEffect(true)
, x833_31_inFreeLook(false)
, x834_24_charging(false)
, x834_25_gunMotionFidgeting(false)
, x834_26_animPlaying(false)
, x834_27_underwater(false)
, x834_28_requestImmediateRecharge(false)
, x834_29_frozen(false)
, x834_30_inBigStrike(false)
, x834_31_gunMotionInFidgetBasePosition(false)
, x835_24_canFirePhazon(false)
, x835_25_inPhazonBeam(false)
, x835_26_phazonBeamMorphing(false)
, x835_27_intoPhazonBeam(false)
, x835_28_bombReady(false)
, x835_29_powerBombReady(false)
, x835_30_inPhazonPool(false)
, x835_31_actorAttached(false)
#ifdef TARGET_PC
, mPortRapidCharge(false)
, mPortRapidShots(0)
, mPortRapidTimer(0.f)
, mPortPointerAimValid(false)
, mPortPointerAimPoint(CVector3f::Zero())
, mPortBubbleXf(CTransform4f::Identity())
, mPortBubbleTimer(0.f)
#endif
{

  x6e0_rightHandModel.SetSortThermal(true);
  kVerticalAngleTable[2] = gpTweakPlayerGun->GetUpLookAngle();
  kVerticalAngleTable[0] = gpTweakPlayerGun->GetDownLookAngle();
  kHorizontalAngleTable[1] = gpTweakPlayerGun->GetHorizontalSpread();
  kHorizontalAngleTable[2] = gpTweakPlayerGun->GetHighHorizontalSpread();
  kHorizontalAngleTable[0] = gpTweakPlayerGun->GetLowHorizontalSpread();
  kVerticalVarianceTable[1] = gpTweakPlayerGun->GetVerticalSpread();
  kVerticalVarianceTable[2] = gpTweakPlayerGun->GetHighVerticalSpread();
  kVerticalVarianceTable[0] = gpTweakPlayerGun->GetLowVerticalSpread();
  CMotionState::SetExtendDistance(gpTweakPlayerGun->GetGunExtendDistance());

  InitBeamData();
  InitBombData();
  InitMuzzleData();
  InitCTData();
  LoadHandAnimTokens();
  x550_camBob.SetPlayerVelocity(CVector3f::Zero());
  x550_camBob.SetBobMagnitude(0.f);
  x550_camBob.SetBobTimeScale(0.f);
}

CPlayerGun::~CPlayerGun() {}

void CPlayerGun::AddToRenderer(const CFrustumPlanes& frustum, const CStateManager&) const {
  rstl::optional_object< CModelData >& model = x72c_currentBeam->SolidModelData();
  if (model) {
    model->RenderParticles(frustum);
  }
}

void CPlayerGun::PreRender(CStateManager& mgr, const CFrustumPlanes& frustum,
                           const CVector3f& camPos) {
  const CPlayerState& playerState = *mgr.GetPlayerState();
  if (playerState.GetCurrentVisor() == CPlayerState::kPV_Scan) {
    return;
  }

  CPlayerState::EPlayerVisor activeVisor = playerState.GetActiveVisor(mgr);
  switch (activeVisor) {
  case CPlayerState::kPV_Thermal: {
    float rgb =
        CMath::Clamp(0.6f, 0.5f * x380_shotSmokeTimer + 0.6f - x378_shotSmokeStartTimer, 1.f);
    x0_lights.BuildConstantAmbientLighting(CColor(rgb, rgb, rgb, 1.f));
    break;
  }
  case CPlayerState::kPV_Combat: {
    CTransform4f offsetXf = CTransform4f::Translate(camPos) * GetGunMotionTransform();
    CAABox aabb = x72c_currentBeam->GetBounds(offsetXf);
    if (mgr.GetNextAreaId() != kInvalidAreaId) {
      x0_lights.SetFindShadowLight(true);
      x0_lights.SetShadowDynamicRangeThreshold(0.25f);
      x0_lights.BuildAreaLightList(mgr, mgr.GetWorld()->GetAreaAlways(mgr.GetNextAreaId()), aabb);
    }
    x0_lights.BuildDynamicLightList(mgr, aabb);
    if (x0_lights.HasShadowLight()) {
      if (x72c_currentBeam->IsLoaded()) {
        x82c_shadow->BuildLightShadowTexture(mgr, mgr.GetNextAreaId(),
                                             x0_lights.GetShadowLightIndex(), aabb, true, false);
      }
    } else {
      x82c_shadow->ResetBlur();
    }
    break;
  }
  default:
    break;
  }

  if (x740_grappleArm->GetActive()) {
    x740_grappleArm->PreRender(mgr, frustum, camPos);
  }

#ifdef TARGET_PC
  // Remastered (PreRender 0xc925e8) draws the right hand only in the X-ray visor, wipes too.
  if (sPortRemasteredGun) {
    if (activeVisor == CPlayerState::kPV_XRay) {
      x6e0_rightHandModel.AnimationData()->PreRender();
    }
  } else
#endif
  if (x678_morph.GetGunState() != CGunMorph::kGS_OutWipeDone ||
      activeVisor == CPlayerState::kPV_XRay) {
    x6e0_rightHandModel.AnimationData()->PreRender();
  }

  if (x833_28_phazonBeamActive) {
    gpRender->AllocatePhazonSuitMaskTexture();
  }
}

static void CopyScreenTex() {
  // Port: copy the lower-right quarter of the actual viewport rather than of a
  // 640x448 one, so the beam-change wipe matches the widescreen framebuffer.
  // Identical to the original at 4:3.
  int left, top, width, height;
  CGraphics::GetViewport(left, top, width, height);
  GXSetTexCopySrc(left + width / 2, top + height / 2, width / 2, height / 2);
  GXSetTexCopyDst(width / 2, height / 2, GX_TF_RGBA8, false);
  GXCopyTex(CGraphics::GetDolphinSpareBuffer(), false);
  GXPixModeSync();
}

void DrawScreenTex(float z) {
  const CTransform4f backupViewMtx(CGraphics::GetViewMatrix());
  const CGraphics::CProjectionState backupProjectionState(CGraphics::GetProjectionState());
  int left, top, width, height;
  CGraphics::GetViewport(left, top, width, height);
  CGraphics::SetOrtho(CCast::ToReal32(left), left + width, top + height, top, -1.f, 1.f);
  CGraphics::SetViewPointMatrix(CTransform4f::Identity());
  gpRender->SetModelMatrix(CTransform4f::Identity());
  gpRender->SetBlendMode_AlphaBlended();
  CGraphics::SetDepthWriteMode(true, kE_GEqual, true);

  CGraphics::LoadDolphinSpareTexture(width / 2, height / 2, GX_TF_RGBA8, NULL,
                                     CGraphics::kSpareBufferTexMapID);

  const GXVtxDescList vtxDesc[3] = {
      {GX_VA_POS, GX_DIRECT},
      {GX_VA_TEX0, GX_DIRECT},
      {GX_VA_NULL, GX_NONE},
  };
  CGX::SetVtxDescv(vtxDesc);
  CGX::SetNumChans(0);
  CGX::SetNumTexGens(1);
  CGX::SetNumTevStages(1);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, CGraphics::kSpareBufferTexMapID, GX_COLOR_NULL);
  CGX::SetChanCtrl(CGX::Channel0, false, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE,
                   GX_AF_NONE);

  // Port: place the quad from the viewport (was 320..640 x 0..224).
  const float quadLeft = static_cast< float >(left + width / 2);
  const float quadRight = static_cast< float >(left + width);
  const float quadBottom = static_cast< float >(top);
  const float quadTop = static_cast< float >(top + height / 2);
  CGX::Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
  RSPosition3f32(quadLeft, z, quadBottom);
  GXTexCoord2f32(0.f, 1.f);
  RSPosition3f32(quadRight, z, quadBottom);
  GXTexCoord2f32(1.f, 1.f);
  RSPosition3f32(quadLeft, z, quadTop);
  GXTexCoord2f32(0.f, 0.f);
  RSPosition3f32(quadRight, z, quadTop);
  GXTexCoord2f32(1.f, 0.f);
  CGX::End();

  CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
  CGraphics::SetViewPointMatrix(backupViewMtx);
  CGraphics::SetProjectionState(backupProjectionState);
}

void CPlayerGun::TouchModel(const CStateManager& mgr) const {
  if (mgr.GetPlayer()->GetMorphballTransitionState() != CPlayer::kMS_Morphed) {
    x73c_gunMotion->GetModelData().Touch(mgr, 0);

    switch (x33c_phazonBeamState) {
    case kPBS_Entering:
      if (x75c_phazonBeam.get()) {
        x75c_phazonBeam->Touch(mgr);
      }
      break;
    case kPBS_Exiting:
      if (x738_nextBeam != nullptr) {
        x738_nextBeam->Touch(mgr);
      }
      break;
    default:
      if (!x833_28_phazonBeamActive) {
        x72c_currentBeam->Touch(mgr);
      } else {
        x75c_phazonBeam->Touch(mgr);
      }
      break;
    }

    x72c_currentBeam->TouchHolo(mgr);
    x740_grappleArm->TouchModel(mgr);
    x6e0_rightHandModel.Touch(mgr, 0);
  }

  if (x734_loadingBeam != nullptr) {
    x734_loadingBeam->Touch(mgr);
    x734_loadingBeam->TouchHolo(mgr);
  }
}

CVector3f CPlayerGun::ConvertToScreenSpace(const CVector3f& pos, const CGameCamera& cam) const {
  CVector3f viewPos = cam.GetTransform().TransposeRotate(
      CVector3f(pos.GetX() - cam.GetTransform().Get03(), pos.GetY() - cam.GetTransform().Get13(),
                pos.GetZ() - cam.GetTransform().Get23()));
  CVector3f screenPos(viewPos);

  if (screenPos.IsNonZero()) {
    return CGraphics::GetPerspectiveProjectionMatrix().MultiplyOneOverW(screenPos);
  }

  return CVector3f(-1.f, -1.f, 1.f);
}

inline void CPlayerGun::DrawArm(const CStateManager& mgr, const CVector3f& pos,
                                const CModelFlags& flags) const {
  if (!x740_grappleArm->GetActive()) {
    return;
  }
  if (x740_grappleArm->GetAnimState() == CGrappleArm::kAS_Done) {
    return;
  }

  const float dot = CVector3f::Dot(x740_grappleArm->GetTransform().GetForward(),
                                   mgr.GetPlayer()->GetTransform().GetForward());
  if (mgr.GetPlayer()->GetGrappleState() != CPlayer::kGS_None ||
      (mgr.GetPlayer()->GetGrappleState() == CPlayer::kGS_None && dot > 0.1f)) {
    x740_grappleArm->Render(
        mgr, pos, x740_grappleArm->IsArmMoving() ? flags : CModelFlags::Normal(), &x0_lights);
  }
}

#ifdef TARGET_PC
namespace {
// The gun's camera-relative pose before the current tick (rigid, gun scale
// dropped) and the tick generation it belongs to.
CTransform4f sPortGunPrevRel = CTransform4f::Identity();
uint sPortGunPrevGeneration = 0;

CTransform4f PortGunRelativePose(const CStateManager& mgr, const CTransform4f& gunWorldXf) {
  const CCameraManager* cameraManager = mgr.GetCameraManager();
  return cameraManager->GetSimulationCameraTransform(mgr).GetQuickInverse() *
         CTransform4f::Translate(cameraManager->GetGlobalCameraTranslation(mgr)) * gunWorldXf;
}
} // namespace

void CPlayerGun::PortSnapshotPresentedPose(const CStateManager& mgr) const {
  sPortGunPrevRel = PortGunRelativePose(mgr, GetGunMotionTransform());
  sPortGunPrevGeneration = CActor::PortTickGeneration();
}

void CPlayerGun::PortShotFromDrawnMuzzle(const CStateManager& mgr, CTransform4f& xf, bool aim) const {
  // CStateManager's RenderGun pass draws the gun at the retail FOV when the view's differs,
  // so on screen the muzzle sits where a world point at the same depth, scaled out from the
  // view axis by the ratio of the two projections, would. Shots start there, so they leave
  // the cannon as drawn; with pointer aim they still head for the pointed point.
  if (PortThirdPerson::Active()) {
    PortShotFromThirdPersonCannon(mgr, xf, aim);
    return;
  }
  const CCameraManager* camMgr = mgr.GetCameraManager();
  if (!camMgr->IsInFPCamera()) {
    return;
  }
  const CGameCamera& cam = camMgr->GetCurrentCamera(mgr);
  if (cam.GetFov() == PortDebug::kFovRetail) {
    return;
  }
  const float gunTan = tanf(CGameCamera::VertPlusFov(PortDebug::kFovRetail, cam.GetAspectRatio()) *
                            0.5f * (M_PIF / 180.f));
  const float viewTan = tanf(cam.GetRenderFov() * 0.5f * (M_PIF / 180.f));
  if (gunTan <= 0.f || viewTan <= 0.f) {
    return;
  }
  const float scale = viewTan / gunTan;
  const CTransform4f viewXf = cam.GetTransform();
  const CVector3f eye = viewXf.GetTranslation();
  const CVector3f p = viewXf.TransposeRotate(xf.GetTranslation() - eye);
  const CVector3f pos =
      eye + viewXf.Rotate(CVector3f(p.GetX() * scale, p.GetY(), p.GetZ() * scale));
  // A shot that only moved would keep retail's direction, aimed from the real muzzle, and pass
  // beside what it was aimed at by as much as it moved (#134: weak points missed with the FOV
  // slider off retail). It is aimed from its new start at where retail's would have arrived:
  // the pointed point, the aim target at its depth along the shot, or what the shot meets.
  if (!aim) {
    xf.SetTranslation(pos);
    return;
  }
  CVector3f aimPoint;
  if (mPortPointerAimValid) {
    aimPoint = mPortPointerAimPoint;
  } else {
    aimPoint = PortShotAimPoint(mgr, xf.GetTranslation(), xf.GetForward());
  }
  const CVector3f toAim = aimPoint - pos;
  if (toAim.CanBeNormalized()) {
    xf = CTransform4f::LookAt(CVector3f::Zero(), toAim);
  }
  xf.SetTranslation(pos);
}

void CPlayerGun::PortShotFromThirdPersonCannon(const CStateManager& mgr, CTransform4f& xf,
                                               bool aim) const {
  // Third person: the shot leaves the body's cannon (the first-person muzzle if the body is not
  // there) and heads for the point under the camera's centre ray, or the assisted aim point of
  // the locked target, as retail's does under a lock.
  const CPlayer& player = *mgr.GetPlayer();
  CVector3f origin = xf.GetTranslation();
  PortThirdPersonBody::CannonPosition(player, origin);
  if (!aim) {
    xf.SetTranslation(origin);
    return;
  }
  PortThirdPersonBody::NoteShot(); // a fired shot (the aux beams' per-frame call passes !aim)
  CVector3f aimPoint;
  const CVector3f camFwd = mgr.GetCameraManager()->GetFirstPersonCamera()->GetTransform().GetForward();
  bool have = false;
  if (player.GetOrbitTargetId() != kInvalidUniqueId &&
      TCastToConstPtr< CActor >(mgr.GetObjectById(player.GetOrbitTargetId())) != nullptr) {
    aimPoint = player.GetAssistedTargetAim();
    have = true;
  } else {
    have = PortThirdPerson::AimPoint(aimPoint);
  }
  if (have && camFwd.CanBeNormalized()) {
    const PortThirdPerson::Vec3 d = PortThirdPerson::AimDirection(
        {origin.GetX(), origin.GetY(), origin.GetZ()},
        {aimPoint.GetX(), aimPoint.GetY(), aimPoint.GetZ()},
        {camFwd.AsNormalized().GetX(), camFwd.AsNormalized().GetY(), camFwd.AsNormalized().GetZ()});
    xf = CTransform4f::LookAt(CVector3f::Zero(), CVector3f(d.x, d.y, d.z));
  }
  xf.SetTranslation(origin);
}

CVector3f CPlayerGun::PortShotAimPoint(const CStateManager& mgr, const CVector3f& from,
                                       const CVector3f& dir) const {
  // Nearer than this the new start's offset would bend the shot more than it is worth.
  const float kMinDepth = 2.f;
  const float kRange = 100.f;
  if (!dir.CanBeNormalized()) {
    return from;
  }
  const CVector3f forward = dir.AsNormalized();
  // Assist-aimed at a target: retail's shot heads for the assisted aim point (the target with
  // its lead), so meet the line at its depth, whether or not the shot touches the target. Only
  // when this shot was aimed at it (port_shot_aim.h): it took the assist direction, and that
  // direction, from the gun, points at the aim point. An aim target kept after it left the box
  // or the line of sight, or one assist was clamped or off for, is not where the shot goes.
  const CPlayer& player = *mgr.GetPlayer();
  if (TCastToConstPtr< CActor >(mgr.GetObjectById(player.GetAimTargetId())) != nullptr &&
      x478_assistAimXf.GetForward().CanBeNormalized()) {
    const CVector3f assistDir = x478_assistAimXf.GetForward().AsNormalized();
    const CVector3f gun = GetTransform().GetTranslation();
    const CVector3f& aim = player.GetAssistedTargetAim();
    const float dirXyz[3] = {forward.GetX(), forward.GetY(), forward.GetZ()};
    const float assistXyz[3] = {assistDir.GetX(), assistDir.GetY(), assistDir.GetZ()};
    const float gunXyz[3] = {gun.GetX(), gun.GetY(), gun.GetZ()};
    const float aimXyz[3] = {aim.GetX(), aim.GetY(), aim.GetZ()};
    const float fromXyz[3] = {from.GetX(), from.GetY(), from.GetZ()};
    if (PortShotAim::SameDirection(dirXyz, assistXyz) &&
        PortShotAim::PointsAt(gunXyz, assistXyz, aimXyz, kMinDepth)) {
      const float depth = PortShotAim::DepthAlong(fromXyz, dirXyz, aimXyz);
      if (depth >= kMinDepth) {
        return from + forward * depth;
      }
    }
  }
  // Else what retail's shot would meet first: the world, or an actor.
  static const CMaterialFilter filter = CMaterialFilter::MakeIncludeExclude(
      CMaterialList(kMT_Solid), CMaterialList(kMT_Player, kMT_ProjectilePassthrough));
  TEntityList nearList;
  mgr.BuildNearList(nearList, from, forward, kRange, filter, mgr.GetPlayer());
  TUniqueId hitId = kInvalidUniqueId;
  const CRayCastResult hit = mgr.RayWorldIntersection(hitId, from, forward, kRange, filter, nearList);
  float depth = kRange;
  if (hit.IsValid()) {
    depth = CMath::Max(CVector3f::Dot(hit.GetPoint() - from, forward), kMinDepth);
  }
  return from + forward * depth;
}

CTransform4f CPlayerGun::PortGunView(const CStateManager& mgr,
                                     const CTransform4f& worldView) const {
  if (!mgr.GetCameraManager()->IsInFPCamera() ||
      mgr.GetPlayer()->GetCameraState() != CPlayer::kCS_FirstPerson ||
      mgr.GetPlayer()->GetMorphballTransitionState() != CPlayer::kMS_Unmorphed) {
    return worldView;
  }
  // The held weapon and its muzzle effects are cached at simulation time.
  // Render their relative pose against that same camera, not an interpolated
  // world camera from a different tick. Projectile simulation is unchanged.
  const CTransform4f simView = mgr.GetCameraManager()->GetSimulationCameraTransform(mgr);
  // Between ticks, blend the gun's pose relative to the camera (bob, sway,
  // recoil) from the last tick's. The view moves instead of the gun, so the
  // arm, beam and muzzle effects all follow.
  const float t = CCameraManager::GetPresentationInterpolation();
  if (t >= 0.f && t < 1.f && PortDebug::ActorInterpolation() &&
      sPortGunPrevGeneration == CActor::PortTickGeneration()) {
    CTransform4f blend = CTransform4f::Identity();
    CTransform4f cur = CTransform4f::Identity();
    if (CActor::PortBlendRigid(sPortGunPrevRel, PortGunRelativePose(mgr, GetGunMotionTransform()),
                               t, blend, cur)) {
      return simView * cur * blend.GetQuickInverse();
    }
  }
  return simView;
}
#endif

void CPlayerGun::Render(const CStateManager& mgr, const CVector3f& pos,
                        const CModelFlags& flags) const {
#ifdef TARGET_PC
  const CVector3f probePos = mgr.GetPlayer()->GetTranslation();
  const float probeXyz[3] = {probePos.GetX(), probePos.GetY(), probePos.GetZ()};
  const PortRoomEnv::ProbeOverrideScope probeScope(probeXyz);
  // CPlayerGunMP1::Render draws the underwater bubbles first, in world space.
  gpRender->SetModelMatrix(CTransform4f::Identity());
  mPortBubbleGens.Render();
#endif
  const CTransform4f worldView = CGraphics::GetViewMatrix();
#ifdef TARGET_PC
  CGraphics::SetViewPointMatrix(PortGunView(mgr, worldView));
#else
  if (mgr.GetCameraManager()->IsInFPCamera() &&
      mgr.GetPlayer()->GetCameraState() == CPlayer::kCS_FirstPerson &&
      mgr.GetPlayer()->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed) {
    // The held weapon and its muzzle effects are cached at simulation time.
    // Render their relative pose against that same camera, not an interpolated
    // world camera from a different tick. Projectile simulation is unchanged.
    CGraphics::SetViewPointMatrix(mgr.GetCameraManager()->GetSimulationCameraTransform(mgr));
  }
#endif
  const CGraphics::CProjectionState projState = CGraphics::GetProjectionState();
#ifdef MP_ENABLE_SMOKE_DRIVER
  PortSmokeMouseGunView(mgr, *this, worldView);
#endif
  const CPlayerState::EPlayerVisor visor = mgr.GetPlayerState()->GetCurrentVisor();
  const bool thermalVisor = visor == CPlayerState::kPV_Thermal;
  const CModelFlags& beamFlags =
      thermalVisor ? kThermalFlags[x310_currentBeam]
      : x835_26_phazonBeamMorphing
          ? static_cast< const CModelFlags& >(CModelFlags::ColorModulate(
                CColor(CColor::Lerp(0xffffffff, 0x000000ff, x39c_phazonMorphT))))
          : flags;

  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  CGraphics::SetDepthRange(kDepthGun, kDepthWorld);
  CTransform4f offsetWorldXf(CTransform4f::Translate(pos) * GetGunMotionTransform());
  CTransform4f elbowOffsetXf(offsetWorldXf * x508_elbowLocalXf);
  if (x32c_chargePhase != kCP_NotCharging && !IsWeaponStateSet(0x10)) {
    offsetWorldXf.AddTranslation(CVector3f(x34c_shakeX, 0.f, x350_shakeZ));
  }

  const CGunMorph::EGunState gunState = x678_morph.GetGunState();
  CTransform4f oldViewMtx(CGraphics::GetViewMatrix());
  CGraphics::SetViewPointMatrix(offsetWorldXf.GetInverse() * oldViewMtx);
  gpRender->SetModelMatrix(CTransform4f::Identity());
  if (x32c_chargePhase >= kCP_FxGrown && x32c_chargePhase < kCP_ComboXfer) {
    x800_auxMuzzleGenerators[x320_currentAuxBeam]->Render();
  }

  if (x832_25_chargeEffectVisible &&
      (x38c_muzzleEffectVisTimer > 0.f || x32c_chargePhase > kCP_AnimAndSfx)) {
    x72c_currentBeam->DrawMuzzleFx(mgr);
  }

#ifdef TARGET_PC
  PortRenderFlashes(0, kPortFirstChargeFlash);
#endif

  if (gunState == CGunMorph::kGS_InWipe || gunState == CGunMorph::kGS_OutWipe) {
    x774_holoTransitionGen->Render();
  }

  CGraphics::SetViewPointMatrix(oldViewMtx);
  if (IsWeaponStateSet(0x10)) {
    x744_auxWeapon->RenderMuzzleFx();
  }
  x72c_currentBeam->PreRenderGunFx(mgr, offsetWorldXf);
  const bool drawSuitArm = !x740_grappleArm->IsGrappling() &&
                           mgr.GetPlayer()->GetGunHolsterState() == CPlayer::kGH_Drawn;
  x73c_gunMotion->Draw(mgr, offsetWorldXf);

  switch (gunState) {
  case CGunMorph::kGS_OutWipeDone:
    if (x0_lights.HasShadowLight()) {
      x82c_shadow->EnableModelProjectedShadow(offsetWorldXf, x0_lights.GetShadowLightArrIndex(),
                                              2.15f);
    }
    if (visor == CPlayerState::kPV_XRay) {
      CTransform4f handXf(elbowOffsetXf * CTransform4f::Translate(0.f, -0.2f, 0.02f));
      x6e0_rightHandModel.Render(mgr, handXf, &x0_lights,
                                 thermalVisor ? kHandThermalFlag : kHandHoloFlag);
    }
    DrawArm(mgr, pos, flags);
    x72c_currentBeam->Draw(drawSuitArm, mgr, offsetWorldXf, beamFlags, &x0_lights);
    x82c_shadow->DisableModelProjectedShadow();
    break;
  case CGunMorph::kGS_InWipeDone:
  case CGunMorph::kGS_InWipe:
  case CGunMorph::kGS_OutWipe: {
    CTransform4f handXf(elbowOffsetXf * CTransform4f::Translate(0.f, -0.2f, 0.02f));
    bool drawHoloHand = true;
#ifdef TARGET_PC
    // Remastered (Render 0xc926d4) draws the hand in the hologram states only in X-ray.
    drawHoloHand = !sPortRemasteredGun || visor == CPlayerState::kPV_XRay;
#endif
    if (gunState != CGunMorph::kGS_InWipeDone) {
      CTransform4f morphXf(elbowOffsetXf *
                           CTransform4f::Translate(0.f, x678_morph.GetYLerp(), 0.f));
      CopyScreenTex();
      if (drawHoloHand) {
        x6e0_rightHandModel.Render(mgr, handXf, &x0_lights,
                                   thermalVisor ? kHandThermalFlag : kHandHoloFlag);
      }
      x72c_currentBeam->DrawHologram(mgr, offsetWorldXf, CModelFlags::Normal());
      DrawScreenTex(ConvertToScreenSpace(morphXf.GetTranslation(), cam).GetZ());
      if (x0_lights.HasShadowLight()) {
        x82c_shadow->EnableModelProjectedShadow(offsetWorldXf, x0_lights.GetShadowLightArrIndex(),
                                                2.15f);
      }
      gpRender->SetModelMatrix(morphXf);
      DrawClipCube(x6c8_hologramClipCube);
      x72c_currentBeam->Draw(drawSuitArm, mgr, offsetWorldXf, beamFlags, &x0_lights);
      DrawArm(mgr, pos, flags);
      x82c_shadow->DisableModelProjectedShadow();
    } else {
      if (drawHoloHand) {
        x6e0_rightHandModel.Render(mgr, handXf, &x0_lights,
                                   thermalVisor ? kHandThermalFlag : kHandHoloFlag);
      }
      x72c_currentBeam->DrawHologram(mgr, offsetWorldXf, CModelFlags::Normal());
      if (x0_lights.HasShadowLight()) {
        x82c_shadow->EnableModelProjectedShadow(offsetWorldXf, x0_lights.GetShadowLightArrIndex(),
                                                2.15f);
      }
      DrawArm(mgr, pos, flags);
      x82c_shadow->DisableModelProjectedShadow();
    }
    break;
  }
  }

  CTransform4f oldViewMtx2(CGraphics::GetViewMatrix());
  CGraphics::SetViewPointMatrix(offsetWorldXf.GetInverse() * oldViewMtx2);
  gpRender->SetModelMatrix(CTransform4f::Identity());
  x72c_currentBeam->PostRenderGunFx(mgr, offsetWorldXf);
#ifdef TARGET_PC
  PortRenderFlashes(kPortFirstChargeFlash, kPortFlashCount);
#endif
  if (x832_26_comboFiring && x77c_comboXferGen.get()) {
    x77c_comboXferGen->Render();
  }
  CGraphics::SetViewPointMatrix(oldViewMtx2);

  CGraphics::SetViewPointMatrix(worldView);
  RenderEnergyDrainEffects(mgr);

  CGraphics::SetDepthRange(kDepthWorld, kDepthFar);
  CGraphics::SetProjectionState(projState);
}

void CPlayerGun::GetLctrWithShake(CTransform4f& xfOut, const CModelData& modelData,
                                  const rstl::string& locatorName, bool shake, bool dynamic) {
  xfOut = dynamic ? modelData.GetScaledLocatorTransformDynamic(locatorName, NULL)
                  : modelData.GetScaledLocatorTransform(locatorName);

  if (x834_24_charging && shake) {
    xfOut.AddTranslation(CVector3f(x34c_shakeX, 0.f, x350_shakeZ));
  }
}

void CPlayerGun::PlayAnim(NWeaponTypes::EGunAnimType type, bool loop) {
  if (x338_nextState != kNS_ChangeWeapon)
    x72c_currentBeam->PlayAnim(type, loop);

  ushort sfx = CSfxManager::kInternalInvalidSfxId;
  switch (type) {
  case NWeaponTypes::kGAT_FromMissile:
    DisableWeaponState(0x4);
    sfx = mFromMissileSound[x310_currentBeam];
    break;
  case NWeaponTypes::kGAT_MissileReload:
    sfx = SFXsam_a_mislload_00;
    break;
  case NWeaponTypes::kGAT_FromBeam:
    sfx = mFromBeamSound[x310_currentBeam];
    break;
  case NWeaponTypes::kGAT_ToMissile:
    DisableWeaponState(0x1);
    sfx = mToMissileSound[x310_currentBeam];
    break;
  default:
    break;
  }

  if (sfx != CSfxManager::kInternalInvalidSfxId)
    NWeaponTypes::play_sfx(sfx, x834_27_underwater, false, 0x4a);
}

inline bool just_thawed(bool frozen, bool playerFrozen) {
  return (frozen ^ playerFrozen) & frozen;
}

inline bool just_froze(bool frozen, bool playerFrozen) {
  return (frozen ^ playerFrozen) & playerFrozen;
}

void CPlayerGun::Update(float grappleSwingT, float cameraBobT, float dt, CStateManager& mgr) {
#ifdef TARGET_PC
  if (!PortDebug::StackBeams()) {
    PortReleaseStackedBeams();
  }
#endif
  CPlayer& player = *mgr.Player();
  const CGunMorph::EGunState gunState = x678_morph.GetGunState();
  CPlayerState& playerState = *mgr.PlayerState();
  const bool isUnmorphed = player.GetMorphballTransitionState() == CPlayer::kMS_Unmorphed;

  const bool justFroze = isUnmorphed ? just_froze(x834_29_frozen, player.GetFrozenState()) : false;
  const bool justThawed =
      isUnmorphed ? just_thawed(x834_29_frozen, player.GetFrozenState()) : false;
  x834_29_frozen = isUnmorphed ? player.GetFrozenState() : false;
  const float advDt = x834_29_frozen ? 0.f : dt;

  const bool inMorph = gunState != CGunMorph::kGS_OutWipeDone;
  if (mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_XRay || inMorph) {
    x6e0_rightHandModel.AdvanceAnimation(advDt, mgr, kInvalidAreaId, true);
  }
  if (inMorph && x734_loadingBeam != NULL && x734_loadingBeam != x72c_currentBeam) {
    x744_auxWeapon->LoadIdle();
    x734_loadingBeam->Update(advDt, mgr);
  }
  if (!x744_auxWeapon->IsLoaded()) {
    x744_auxWeapon->LoadIdle();
  }

  if (justFroze) {
    x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_None);
    x72c_currentBeam->EnableFrozenEffect(kFFT_Frozen);
  } else if (justThawed) {
    x72c_currentBeam->EnableFrozenEffect(kFFT_Thawed);
  }

  if (justFroze || justThawed) {
    x2f4_fireButtonStates = 0;
    x2ec_lastFireButtonStates = 0;
    CancelFiring(mgr);
  }

  x72c_currentBeam->Update(advDt, mgr);
  x73c_gunMotion->Update(advDt * x370_gunMotionSpeedMult, mgr);
  x740_grappleArm->Update(grappleSwingT, advDt, mgr);

  if (x338_nextState != kNS_StatusQuo) {
    const CAnimData& animData = *x72c_currentBeam->GetSolidModelData().GetAnimationData();
    if (gunState == CGunMorph::kGS_InWipeDone) {
      if (x338_nextState == kNS_ChangeWeapon) {
        ChangeWeapon(playerState, mgr);
        x338_nextState = kNS_StatusQuo;
      }
    } else if (!animData.IsAnimTimeRemaining(0.001f, rstl::string_l("Whole Body")) ||
               x832_30_requestReturnToDefault) {
      bool statusQuo = true;
      switch (x338_nextState) {
      case kNS_EnterMissile:
        ResetToMissile();
        break;
      case kNS_ExitMissile:
        ResetToBeam();
        x390_cooldown = x72c_currentBeam->GetWeaponInfo().x0_coolDown;
        break;
      case kNS_MissileReload:
        PlayAnim(NWeaponTypes::kGAT_MissileReload, false);
        x338_nextState = kNS_MissileShotDone;
        statusQuo = false;
        break;
      case kNS_MissileShotDone:
        EnableWeaponState(0x4);
        break;
      case kNS_ChangeWeapon:
        ChangeWeapon(playerState, mgr);
        break;
      case kNS_SetupBeam:
        x390_cooldown = x72c_currentBeam->GetWeaponInfo().x0_coolDown;
        DisableWeaponState(0x8);
        ResetToBeam();
        break;
      case kNS_EnterPhazonBeam:
        if (x75c_phazonBeam->IsLoaded()) {
          break;
        }
        x72c_currentBeam->x218_29_drawHologram = true;
        x75c_phazonBeam->Load(mgr, false);
        x33c_phazonBeamState = kPBS_Entering;
        break;
      case kNS_ExitPhazonBeam:
        if (x738_nextBeam->IsLoaded()) {
          break;
        }
        x72c_currentBeam->x218_29_drawHologram = true;
        x738_nextBeam->Load(mgr, false);
        x33c_phazonBeamState = kPBS_Exiting;
        break;
      default:
        break;
      }

      if (statusQuo) {
        x338_nextState = kNS_StatusQuo;
      }
    }
  }

  if (x37c_rapidFireShotsDecayTimer < 0.2f) {
    x37c_rapidFireShotsDecayTimer += advDt;
  } else {
    x37c_rapidFireShotsDecayTimer = 0.f;
    if (x30c_rapidFireShots > 0) {
      x30c_rapidFireShots -= 1;
    }
  }

  if (x32c_chargePhase != kCP_NotCharging && !player.GetFrozenState()) {
    // Remastered (0xc93950): the factor is 0 unless the charging flag is set.
    const float shakeFactor = (sPortRemasteredGun && !x834_24_charging) ? 0.f : x340_chargeBeamFactor;
    x34c_shakeX = chargeShakeTbl[static_cast< int >(mgr.Random()->Next()) % 3] * shakeFactor;
    x350_shakeZ = chargeShakeTbl[static_cast< int >(mgr.Random()->Next()) % 3] * shakeFactor;
  }

#ifdef TARGET_PC
  // Port: a save state loaded in the ball morphs the player before the beam has loaded, and
  // nothing touches the gun in the ball, so it never would and every bomb is skipped below.
  if (!x72c_currentBeam->IsLoaded()
      && player.GetMorphballTransitionState() == CPlayer::kMS_Morphed) {
    x72c_currentBeam->Touch(mgr);
  }
#endif
  if (!x72c_currentBeam->IsLoaded()) {
    return;
  }

  CModelData& beamModel = *x72c_currentBeam->SolidModelData();
  const CModelData& motionModel = x73c_gunMotion->GetModelData();
  GetLctrWithShake(x4d8_gunLocalXf, motionModel, rstl::string_l(kGunLocator), true, true);
  GetLctrWithShake(x418_beamLocalXf, beamModel, rstl::string_l(CGunWeapon::skMuzzleLocator), false,
                   true);
  GetLctrWithShake(x508_elbowLocalXf, beamModel, rstl::string_l("elbow"), false, false);
  x4a8_gunWorldXf = x3e8_xf * x4d8_gunLocalXf * x550_camBob.GetCameraBobTransformation();

  if (x740_grappleArm->GetActive() && !x740_grappleArm->IsGrappling()) {
    UpdateLeftArmTransform(beamModel, mgr);
  }

#ifdef TARGET_PC
  sPortRemasteredGun = x774_holoTransitionGen->PortIsRemastered();
#endif
  x6a0_motionState.Update((x2f0_pressedFireButtonStates & 1) != 0 && x832_28_readyForShot &&
                              x32c_chargePhase < kCP_AnimAndSfx && !player.IsInFreeLook(),
                          advDt, x4a8_gunWorldXf, mgr);

  beamModel.AdvanceParticles(GetGunMotionTransform(), advDt, mgr);
  x72c_currentBeam->UpdateGunFx(x380_shotSmokeTimer > 2.f && x378_shotSmokeStartTimer > 0.15f, dt,
                                mgr, x508_elbowLocalXf);

  CTransform4f beamWorldXf = GetGunMotionTransform() * x418_beamLocalXf;

  if (player.GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
      !mgr.GetCameraManager()->IsInCinematicCamera()) {
    TEntityList nearList;
    mgr.BuildNearList(nearList,
                      x72c_currentBeam->GetBounds().GetTransformedAABox(GetGunMotionTransform()),
                      sAimFilter, &player);
    TUniqueId bestId = kInvalidUniqueId;
    const CVector3f dir = x4a8_gunWorldXf.GetForward().AsNormalized();
    const CVector3f offset = -(dir * 0.5f);
    const CVector3f pos = x4a8_gunWorldXf.GetTranslation() + offset;
    const CRayCastResult result = mgr.RayWorldIntersection(
        bestId, pos, dir, 3.5f,
        CMaterialFilter::MakeIncludeExclude(CMaterialList(kMT_Solid),
                                            CMaterialList(kMT_ProjectilePassthrough)),
        nearList);
    x833_29_pointBlankWorldSurface = result.IsValid();
    if (result.IsValid()) {
      x448_elbowWorldXf = GetGunMotionTransform() * x508_elbowLocalXf;
      x448_elbowWorldXf.AddTranslation(offset);
      beamWorldXf.SetTranslation(result.GetPoint());
    }
  } else {
    x833_29_pointBlankWorldSurface = false;
  }

  CTransform4f beamTargetXf = x833_29_pointBlankWorldSurface ? x448_elbowWorldXf : beamWorldXf;

  const CVector3f camTrans = mgr.GetCameraManager()->GetGlobalCameraTranslation(mgr);
  beamWorldXf.AddTranslation(camTrans);
  beamTargetXf.AddTranslation(camTrans);
#ifdef TARGET_PC
  // Remastered (Update 0xc93950, UpdateAuxWeapons 0xc977f4) hands the aux weapons the gun x
  // beam locator moved by the camera translation: no elbow swap or hit point at point blank.
  if (sPortRemasteredGun) {
    beamTargetXf = GetGunMotionTransform() * x418_beamLocalXf;
    beamTargetXf.AddTranslation(camTrans);
  }
#endif

  const bool emitting = x833_30_canShowAuxMuzzleEffect ? x344_comboXferTimer < 1.f : false;
  const float scaleFactor =
      (emitting && x832_26_comboFiring) ? (1.f - x344_comboXferTimer) * 2.f : 2.f;
  const CVector3f scale(scaleFactor, scaleFactor, scaleFactor);
  if (x832_25_chargeEffectVisible) {
#ifdef TARGET_PC
    // The muzzle system inherits the gun/world pose from the render frame; pass only
    // the sampled gun-local LBEAM locator so converted XFMD3 children inherit its rotation.
    x72c_currentBeam->UpdateMuzzleFx(advDt, scale, x418_beamLocalXf, emitting);
#else
    x72c_currentBeam->UpdateMuzzleFx(advDt, scale, x418_beamLocalXf.GetTranslation(), emitting);
#endif
    x800_auxMuzzleGenerators[x320_currentAuxBeam]->SetGlobalOrientAndTrans(x418_beamLocalXf);
    x800_auxMuzzleGenerators[x320_currentAuxBeam]->SetGlobalScale(scale);
    x800_auxMuzzleGenerators[x320_currentAuxBeam]->SetParticleEmission(emitting);
    x800_auxMuzzleGenerators[x320_currentAuxBeam]->Update(advDt);
  }

#ifdef TARGET_PC
  // CPlayerGunMP1::Update updates the muzzle modules every frame with the muzzle
  // generators' scale, not only while the charge effect is visible.
  x72c_currentBeam->PortUpdateMuzzleFlash(advDt, scale, x418_beamLocalXf);
  PortUpdateFlashes(advDt, scale);
#endif

  if (x748_rainSplashGenerator.get()) {
    x748_rainSplashGenerator->Update(advDt, mgr);
  }

  UpdateGunLight(beamWorldXf, mgr);
  ProcessGunMorph(advDt, mgr);
  if (x835_26_phazonBeamMorphing) {
    ProcessPhazonGunMorph(advDt, mgr);
  }

  if (x832_26_comboFiring && !x77c_comboXferGen.null()) {
    x77c_comboXferGen->SetGlobalTranslation(x418_beamLocalXf.GetTranslation());
    x77c_comboXferGen->SetGlobalOrientation(x418_beamLocalXf.GetRotation());
    x77c_comboXferGen->Update(advDt);
    x344_comboXferTimer += advDt * 4.f;
  }

  if (x35c_bombTime > 0.f) {
    x35c_bombTime -= advDt;
    if (x35c_bombTime <= 0.f) {
      x308_bombCount = 3;
    }
  }

  if (playerState.ItemEnabled(CPlayerState::kIT_ChargeBeam) &&
      x32c_chargePhase != kCP_NotCharging) {
    UpdateChargeState(advDt, mgr);
  } else {
    x340_chargeBeamFactor -= advDt;
    if (x340_chargeBeamFactor < 0.f) {
      x340_chargeBeamFactor = 0.f;
    }
  }
#ifdef TARGET_PC
  // Port: the Remastered Ice Beam cannon's frost shell dissolves in with the charge, as
  // CGunWeaponMP1::UpdateChargeEffects drives it. Its drive isn't the charge factor but
  // CPlayerGunMP1::Update's own accumulator: past the charge FX start (warm-up) it rises
  // at 1/1.45 per second, otherwise it falls at 1 per second. Ice keeps retail's timing,
  // so the warm-up is kChargeFxStart. It grows through the combo phases too (+0x4c <= 9,
  // i.e. up to kCP_ComboFireDone).
  {
    static float sShellFactor = 0.f;
    const bool charging =
        x32c_chargePhase >= kCP_ChargeRequested && x32c_chargePhase <= kCP_ComboFireDone;
    if (charging && x340_chargeBeamFactor >= kChargeFxStart) {
      sShellFactor += advDt / 1.45f;
      if (sShellFactor > 1.f) {
        sShellFactor = 1.f;
      }
    } else {
      sShellFactor -= advDt;
      if (sShellFactor < 0.f) {
        sShellFactor = 0.f;
      }
    }
    CCubeModel::PortSetChargeShell(sShellFactor);
  }
#endif

  UpdateAuxWeapons(advDt, beamTargetXf, mgr);
  DoUserAnimEvents(advDt, mgr);

  if (player.GetOrbitState() == CPlayer::kOS_OrbitObject && GetTargetId(mgr) != kInvalidUniqueId) {
    if (!x832_29_lockedOn && !x832_26_comboFiring && !IsWeaponStateSet(0x10)) {
      x832_29_lockedOn = true;
      x6a0_motionState.SetState(CMotionState::kMS_LockOn);
      ReturnArmAndGunToDefault(mgr, true);
    }
  } else {
    CancelLockOn();
  }

  UpdateWeaponFire(advDt, playerState, mgr);
  UpdateGunIdle(x364_gunStrikeCoolTimer > 0.f, cameraBobT, advDt, mgr);
#ifdef TARGET_PC
  PortUpdateUnderWaterBubbles(advDt, mgr);
#endif

  if ((x2ec_lastFireButtonStates & 0x1) != 0) {
    x378_shotSmokeStartTimer = 0.f;
  } else if (x378_shotSmokeStartTimer < 2.f) {
    x378_shotSmokeStartTimer += advDt;
    if (x378_shotSmokeStartTimer > 1.f) {
      x30c_rapidFireShots = 0;
      x380_shotSmokeTimer = 0.f;
    }
  }

  if (x38c_muzzleEffectVisTimer > 0.f) {
    x38c_muzzleEffectVisTimer -= advDt;
  }

  if (x30c_rapidFireShots > 5 && x380_shotSmokeTimer < 2.f) {
    x380_shotSmokeTimer += advDt;
  }

  if (x384_gunStrikeDelayTimer > 0.f) {
    x384_gunStrikeDelayTimer -= advDt;
  }

  if (x364_gunStrikeCoolTimer > 0.f) {
    x2f4_fireButtonStates = 0;
    x364_gunStrikeCoolTimer -= advDt;
  }

  if (isUnmorphed && IsWeaponStateSet(0x4)) {
    x3a0_missileExitTimer -= advDt;
    if (x3a0_missileExitTimer < 0.f) {
      x3a0_missileExitTimer = 0.f;
      ExitMissile();
    }
  }
}

void CPlayerGun::CancelMouseInput(CStateManager& mgr) {
  if (x2ec_lastFireButtonStates != 0 || x2f4_fireButtonStates != 0 ||
      x32c_chargePhase != kCP_NotCharging) {
    CancelFiring(mgr);
  }
  x2ec_lastFireButtonStates = x2f4_fireButtonStates = x2f0_pressedFireButtonStates = 0;
}

void CPlayerGun::ProcessInput(const CFinalInput& input, CStateManager& mgr) {
  if (PortDebug::MouseAim() && PortDebug::MouseGameplayActive() &&
      (!PortDebug::MouseCaptured() || PortDebug::Visible() || !mgr.GetPlayer()->MouseControlsAllowed(mgr))) {
    // Losing capture must cancel a held charge, not interpret the cleared PAD
    // state as a deliberate charge-release shot into the UI/background.
    CancelMouseInput(mgr);
    return;
  }
  const CPlayer* player = mgr.GetPlayer();
  const CPlayerState* cPlayerState = mgr.GetPlayerState();
  CPlayerState* playerState = const_cast< CPlayerState* >(cPlayerState);
  bool damageNotMorphed = false;
  if (x834_30_inBigStrike && player->GetMorphballTransitionState() != CPlayer::kMS_Morphed) {
    damageNotMorphed = true;
  }

  if (x832_24_coolingCharge || damageNotMorphed || IsWeaponStateSet(0x8)) {
#if VERSION >= VERSION_GM8P_00
    if (IsWeaponStateSet(0x8)) {
      x2f4_fireButtonStates = 0;
    }
#endif
    return;
  }

  if (cPlayerState->HasPowerUp(CPlayerState::kIT_ChargeBeam)) {
    if (!cPlayerState->ItemEnabled(CPlayerState::kIT_ChargeBeam)) {
      playerState->EnableItem(CPlayerState::kIT_ChargeBeam);
    }
  } else if (cPlayerState->ItemEnabled(CPlayerState::kIT_ChargeBeam)) {
    playerState->DisableItem(CPlayerState::kIT_ChargeBeam);
    ResetCharge(mgr, false);
  }

  switch (player->GetMorphballTransitionState()) {
  case CPlayer::kMS_Morphing:
  case CPlayer::kMS_Unmorphing:
    x2f4_fireButtonStates = 0;
    break;
  case CPlayer::kMS_Unmorphed:
    if (!IsWeaponStateSet(0x10)) {
      HandleWeaponChange(input, mgr);
    }
#ifdef TARGET_PC
    // Port: a randomized start may not own the beam in hand (or any beam).
    if (!x835_25_inPhazonBeam && !cPlayerState->HasPowerUp(mBeamArr[x310_currentBeam])) {
      if (!IsWeaponStateSet(0x8) && !IsWeaponStateSet(0x10)) {
        PortSwitchToOwnedBeam(mgr);
      }
      x2f4_fireButtonStates =
          ControlMapper::GetDigitalInput(ControlMapper::kC_MissileOrPowerBomb, input) ? 2 : 0;
      break;
    }
#endif
    // fallthrough
  case CPlayer::kMS_Morphed:
    x2f4_fireButtonStates =
        ControlMapper::GetDigitalInput(ControlMapper::kC_FireOrBomb, input) ? 1 : 0;
    x2f4_fireButtonStates |=
        ControlMapper::GetDigitalInput(ControlMapper::kC_MissileOrPowerBomb, input) ? 2 : 0;
    break;
  }
}

#ifdef TARGET_PC
// Port: the beam change HandleBeamChange starts, without the input.
void CPlayerGun::PortSwitchToOwnedBeam(CStateManager& mgr) {
  const CPlayerState& playerState = *mgr.GetPlayerState();
  int beam = -1;
  for (int i = 0; i < 4 && beam < 0; ++i) {
    if (playerState.HasPowerUp(mBeamArr[i])) {
      beam = i;
    }
  }
  if (beam < 0 || beam == x310_currentBeam) {
    return;
  }
  x314_nextBeam = static_cast< CPlayerState::EBeamId >(beam);
  SetStateFlags(0);
  EnableWeaponState(0x8);
  PlayAnim(NWeaponTypes::kGAT_FromBeam, false);
  if (x833_31_inFreeLook || x744_auxWeapon->IsComboFxActive(mgr) || x832_26_comboFiring) {
    x832_30_requestReturnToDefault = true;
    x740_grappleArm->EnterIdle(mgr);
  }
  x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_None);
  x338_nextState = kNS_ChangeWeapon;
  x2e4_invalidSfx.Clear();
}
#endif

void CPlayerGun::ProcessChargeState(int releasedStates, int pressedStates, CStateManager& mgr,
                                    float dt) {
  if ((releasedStates & 0x1) != 0) {
    ResetCharged(dt, mgr);
    return;
  }
  if ((pressedStates & 0x1) != 0) {
    if (x32c_chargePhase == kCP_NotCharging && (pressedStates & 0x1) != 0 &&
        x348_chargeCooldownTimer == 0.f && x832_28_readyForShot == 1) {
      UpdateNormalShotCycle(dt, mgr);
      x32c_chargePhase = kCP_ChargeRequested;
#ifdef TARGET_PC
      PortStartRapidCharge();
#endif
    }
  } else {
    const CPlayerState* state = mgr.GetPlayerState();
    if (state->HasPowerUp(CPlayerState::kIT_Missiles) && (pressedStates & 0x2) != 0) {
      if (x32c_chargePhase >= kCP_FxGrown) {
        if (state->HasPowerUp(mBeamComboArr[size_t(x310_currentBeam)]))
          ActivateCombo(mgr);
      } else if (x32c_chargePhase == kCP_NotCharging) {
        FireSecondary(dt, mgr);
      }
    }
  }
}

void CPlayerGun::ResetNormal(CStateManager& mgr) {
  Reset(mgr, false);
  x832_28_readyForShot = false;
}

void CPlayerGun::ResetCharged(float dt, CStateManager& mgr) {
  if (x832_26_comboFiring == true) {
    return;
  }

#ifdef TARGET_PC
  const bool powerRapidCharge = NPortPowerChargeRelease::IsScopedRapidCharge(
      mPortRapidCharge, x310_currentBeam == CPlayerState::kBI_Power);
#else
  const bool powerRapidCharge = false;
#endif
  if (NPortPowerChargeRelease::IsChargedRelease(
          x32c_chargePhase, kCP_FxGrowing, kCP_AnimAndSfx, x832_27_chargeAnimStarted,
          powerRapidCharge)) {
    x833_30_canShowAuxMuzzleEffect = false;
    UpdateNormalShotCycle(dt, mgr);
    x832_24_coolingCharge = true;
    CancelCharge(mgr, true);
  } else if (x32c_chargePhase != kCP_NotCharging) {
    x320_currentAuxBeam = x310_currentBeam;
    x833_30_canShowAuxMuzzleEffect = true;
    x32c_chargePhase = kCP_ChargeDone;
  }
  StopChargeSound(mgr);
}

void CPlayerGun::ProcessNormalState(int releasedStates, int pressedStates, CStateManager& mgr,
                                    float dt) {
  if ((releasedStates & 0x1) != 0) {
#ifdef TARGET_PC
    mPortRapidCharge = false;
#endif
    ResetNormal(mgr);
    return;
  }

  if ((pressedStates & 0x1) != 0 && x348_chargeCooldownTimer == 0.f &&
      x832_28_readyForShot == true) {
    UpdateNormalShotCycle(dt, mgr);
#ifdef TARGET_PC
    PortStartRapidCharge();
#endif
    return;
  }
#ifdef TARGET_PC
  // Remastered without the Charge Beam: the same quick shots while held, then
  // nothing until release (CPlayerGunMP1 charge state 0xb).
  if ((x2ec_lastFireButtonStates & 0x1) != 0 && mPortRapidCharge &&
      !PortRapidChargeShot(dt, mgr))
    mPortRapidCharge = false;
#endif
  if ((pressedStates & 0x2) != 0) {
    FireSecondary(dt, mgr);
  }
}

bool CPlayerGun::ExitMissile() {
  if (!IsWeaponStateSet(0x1)) {
    if (!IsWeaponStateSet(0x10) && x338_nextState != kNS_ExitMissile) {
      x338_nextState = kNS_ExitMissile;
      PlayAnim(NWeaponTypes::kGAT_FromMissile, false);
    }
    return false;
  }
  return true;
}

#ifdef TARGET_PC
// Port (stack_beams): the projectiles of every other owned beam, from the shot just fired.
void CPlayerGun::PortFireStackedBeams(float dt, CStateManager& mgr, const CTransform4f& xf,
                                      uint targetHoming) {
  const bool enabled = PortDebug::StackBeams() && !x833_28_phazonBeamActive;
  if (!enabled) {
    PortReleaseStackedBeams();
    return;
  }
  const CPlayerState& playerState = *mgr.GetPlayerState();
  for (int i = 0; i < 4; ++i) {
    CGunWeapon* beam = x760_selectableBeams[i];
    if (i == x310_currentBeam || beam == nullptr || beam == x72c_currentBeam ||
        !playerState.HasPowerUp(mBeamArr[i])) {
      continue;
    }
    beam->PortLockStackTokens();
    beam->PortFireStackedProjectile(
        x834_27_underwater, dt, CPlayerState::EChargeStage(x330_chargeState), xf, mgr,
        static_cast< const TUniqueId& >(targetHoming ? PortHomingTargetId(mgr) : kInvalidUniqueId),
        x340_chargeBeamFactor, x340_chargeBeamFactor);
  }
}

// Releases the stacked beams' projectile tokens once the setting is off.
void CPlayerGun::PortReleaseStackedBeams() {
  for (int i = 0; i < 4; ++i) {
    if (x760_selectableBeams[i] != nullptr) {
      x760_selectableBeams[i]->PortUnlockStackTokens();
    }
  }
}
#endif

void CPlayerGun::UpdateNormalShotCycle(float dt, CStateManager& mgr) {
  if (!ExitMissile() || mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }
  bool showChargeFx = x833_28_phazonBeamActive;
  if (!x833_28_phazonBeamActive && x310_currentBeam == CPlayerState::kBI_Plasma) {
    showChargeFx = true;
  }
  const uchar hideChargeEffect = showChargeFx && x32c_chargePhase == kCP_NotCharging;
  x832_25_chargeEffectVisible = !hideChargeEffect;
  x30c_rapidFireShots += 1;

  const uint targetHoming =
      x72c_currentBeam->GetVelocityInfo().GetTargetHoming(int(x330_chargeState));

  CTransform4f xf(x833_29_pointBlankWorldSurface ? x448_elbowWorldXf
                                                 : GetGunMotionTransform() * x418_beamLocalXf);
  if (!x833_29_pointBlankWorldSurface && x364_gunStrikeCoolTimer <= 0.f) {
    const CVector3f pos = xf.GetTranslation();
    xf = x478_assistAimXf;
    xf.SetTranslation(pos);
  }
#ifdef TARGET_PC
  if (!x833_29_pointBlankWorldSurface || PortThirdPerson::Active()) {
    PortShotFromDrawnMuzzle(mgr, xf);
  }
#endif

  xf.AddTranslation(mgr.GetCameraManager()->GetGlobalCameraTranslation(mgr));
  x38c_muzzleEffectVisTimer = 0.0625f;

#ifdef MP_ENABLE_SMOKE_DRIVER
  PortSmokeMouseShot(x330_chargeState == kCS_Charged, false);
#endif
#ifdef TARGET_PC
  if (x330_chargeState != kCS_Normal) {
    PortStartFlash(3 + int(x310_currentBeam));
  }
#endif
  x72c_currentBeam->Fire(
      x834_27_underwater, dt, CPlayerState::EChargeStage(x330_chargeState), xf, mgr,
#ifdef TARGET_PC
      static_cast< const TUniqueId& >(targetHoming ? PortHomingTargetId(mgr) : kInvalidUniqueId),
#else
      static_cast< const TUniqueId& >(targetHoming ? GetTargetId(mgr) : kInvalidUniqueId),
#endif
      x340_chargeBeamFactor, x340_chargeBeamFactor);
#ifdef TARGET_PC
  PortFireStackedBeams(dt, mgr, xf, targetHoming);
#endif

  mgr.InformListeners(GetGunMotionTransform().GetTranslation(), kLNT_PlayerFire);
}

void CPlayerGun::FireSecondary(float dt, CStateManager& mgr) {
  if (mgr.GetCameraManager()->IsInCinematicCamera()) {
    return;
  }

  CPlayerState* playerState = mgr.PlayerState();
#ifdef TARGET_PC
  // Port: the Phazon Beam cheat fires missiles (not combos) from the Phazon Beam.
  const bool phazonBlocks = x835_25_inPhazonBeam && !(mPortPhazonCheat && int(x318_comboAmmoIdx) == 1 &&
                                                      !x832_26_comboFiring);
#else
  const bool phazonBlocks = x835_25_inPhazonBeam;
#endif
  if (phazonBlocks || int(x318_comboAmmoIdx) == 0 ||
      playerState->HasPowerUp(skItemArr[x318_comboAmmoIdx]) != true || !IsWeaponStateSet(0x4)) {
    NWeaponTypes::play_sfx(SFXsam_b_malfxn_00, x834_27_underwater, false, 0x4a);
    return;
  }

  bool fired = false;
  switch (x318_comboAmmoIdx) {
  case 1: {
    x300_remainingMissiles = playerState->GetItemAmount(CPlayerState::kIT_Missiles);
    if (mgr.GetWeaponIdCount(x538_playerId, kWT_Missile) < 3 && int(x300_remainingMissiles) != 0) {
      playerState->DecrPickUp(CPlayerState::kIT_Missiles,
                              x832_26_comboFiring ? playerState->GetMissileCostForAltAttack() : 1);
      fired = true;
    }

    if (int(x300_remainingMissiles) > 5) {
      x300_remainingMissiles = 5;
    } else {
      x300_remainingMissiles -= 1;
    }
    break;
  }
  default:
    break;
  }

  if (fired) {
#ifdef TARGET_PC
    TUniqueId targetId = PortHomingTargetId(mgr);
#else
    TUniqueId targetId = GetTargetId(mgr);
#endif
    // Under a lock the aim target is the locked one, so a Wavebuster sent off it takes none.
    if (x832_26_comboFiring && targetId == kInvalidUniqueId &&
#ifdef TARGET_PC
        !PortShotOffLock(mgr) &&
#endif
        x310_currentBeam == CPlayerState::kBI_Wave) {
      targetId = mgr.GetPlayer()->GetAimTargetId();
    }

    CTransform4f xf(x833_29_pointBlankWorldSurface ? x448_elbowWorldXf
                                                   : GetGunMotionTransform() * x418_beamLocalXf);
    if (!x833_29_pointBlankWorldSurface && x364_gunStrikeCoolTimer <= 0.f) {
      const CVector3f pos = xf.GetTranslation();
      xf = x478_assistAimXf;
      xf.SetTranslation(pos);
    }
#ifdef TARGET_PC
    if (!x833_29_pointBlankWorldSurface || PortThirdPerson::Active()) {
      PortShotFromDrawnMuzzle(mgr, xf);
    }
#endif

    xf.AddTranslation(mgr.GetCameraManager()->GetGlobalCameraTranslation(mgr));
#ifdef MP_ENABLE_SMOKE_DRIVER
    PortSmokeMouseShot(x330_chargeState == kCS_Charged, true);
#endif
#ifdef TARGET_PC
    // Missile flash, or the combo's (only Power's super missile and Ice's combo have one).
    if (x330_chargeState != kCS_Charged) {
      PortStartFlash(0);
    } else if (x310_currentBeam == CPlayerState::kBI_Power) {
      PortStartFlash(1);
    } else if (x310_currentBeam == CPlayerState::kBI_Ice) {
      PortStartFlash(2);
    }
#endif
    x744_auxWeapon->Fire(dt, x834_27_underwater, CPlayerState::EBeamId(x310_currentBeam),
                         CPlayerState::EChargeStage(x330_chargeState), xf, mgr,
                         x72c_currentBeam->GetType(), targetId);

    mgr.InformListeners(GetGunMotionTransform().GetTranslation(), kLNT_PlayerFire);

    x3a0_missileExitTimer = 7.f;
    if (x832_26_comboFiring != true) {
      PlayAnim(NWeaponTypes::kGAT_MissileShoot, false);
      x338_nextState = int(x300_remainingMissiles) > 0 ? kNS_MissileReload : kNS_MissileShotDone;
      DisableWeaponState(0x4);
    }
  } else {
    NWeaponTypes::play_sfx(mItemEmptySound[x318_comboAmmoIdx], x834_27_underwater, false, 0x4a);
  }
}

void CPlayerGun::DropBomb(CPlayerGun::EBWeapon weapon, CStateManager& mgr) {
  const float ballHalfExtent = gpTweakPlayer->GetPlayerBallHalfExtent();

  switch (weapon) {
  case kBW_Bomb: {
    if (x32c_chargePhase != kCP_NotCharging) {
      x32c_chargePhase = kCP_ChargeDone;
      break;
    }

    if (x308_bombCount <= 0) {
      break;
    }

    CBomb* const bomb = rs_new CBomb(x784_bombEffects[weapon][0], x784_bombEffects[weapon][1],
                                     mgr.AllocateUniqueId(), mgr.GetPlayer()->GetCurrentAreaId(),
                                     x538_playerId, x354_bombFuseTime,
                                     CTransform4f::Translate(mgr.GetPlayer()->GetTranslation() +
                                                             CVector3f(0.f, 0.f, ballHalfExtent)),
                                     gpTweakPlayerGun->GetBombInfo());
    if (x784_bombEffects[weapon].size() > 2) {
      bomb->SetAttractEffect(x784_bombEffects[weapon][2]);
    }
    mgr.AddObject(*bomb);

    if (x308_bombCount == 3) {
      x35c_bombTime = x358_bombDropDelayTime;
    }

    --x308_bombCount;

    const TUniqueId platformId = mgr.GetPlayer()->GetRidingPlatformId();
    if (CEntity* ent = mgr.ObjectById(platformId)) {
      if (CScriptPlatform* plat = TCastToPtr< CScriptPlatform >(ent)) {
        plat->AddSlave(bomb->GetUniqueId(), mgr);
      }
    }
    break;
  }
  case kBW_PowerBomb:
    mgr.PlayerState()->DecrPickUp(CPlayerState::kIT_PowerBombs, 1);
    x53a_powerBomb = DropPowerBomb(mgr);
    break;
  default:
    break;
  }
}

void CPlayerGun::ActivateCombo(CStateManager& mgr) {
  if (x832_26_comboFiring == true) {
    return;
  }

  CPlayerState& playerState = *mgr.PlayerState();
  const int altCost = playerState.GetMissileCostForAltAttack();
  if (playerState.GetItemAmount(skItemArr[x318_comboAmmoIdx]) >= altCost) {
    bool canFire = true;
    if (x310_currentBeam == CPlayerState::kBI_Plasma) {
      canFire = !x834_27_underwater;
    }

    if (canFire) {
      x832_26_comboFiring = true;

      TCachedToken< CGenDescription >& cachedXferEffect = x72c_currentBeam->x160_xferEffect;
      if (cachedXferEffect.TryCache()) {
        x77c_comboXferGen = rs_new CElementGen(cachedXferEffect);
        x77c_comboXferGen->SetGlobalScale(kScaleVector);
      }

      x72c_currentBeam->x218_25_enableCharge = true;
      StopChargeSound(mgr);
      NWeaponTypes::play_sfx(SFXsam_a_combochg_00, x834_27_underwater, false, 0x4a);
      x32c_chargePhase = kCP_ComboXfer;
    }
  } else {
    NWeaponTypes::play_sfx(SFXsam_b_malfxn_00, x834_27_underwater, false, 0x4a);
  }
}

void CPlayerGun::EnableChargeFx(CPlayerState::EChargeStage, CStateManager& mgr) {
  x72c_currentBeam->ActivateCharge(true, false);
  SetGunLightActive(true, mgr);
  x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_Charge);
  StopContinuousBeam(mgr, false);

  switch (x310_currentBeam) {
  case CPlayerState::kBI_Power:
  case CPlayerState::kBI_Plasma:
    x832_25_chargeEffectVisible = true;
    break;
  default:
    break;
  }

  EnableWeaponState(0x7);
  x318_comboAmmoIdx = 1;
  x338_nextState = kNS_StatusQuo;
  x833_30_canShowAuxMuzzleEffect = true;

  x800_auxMuzzleGenerators[x320_currentAuxBeam] =
      rs_new CElementGen(x7c0_auxMuzzleEffects[x320_currentAuxBeam]);
  x800_auxMuzzleGenerators[x320_currentAuxBeam]->SetParticleEmission(true);
}

#ifdef TARGET_PC
void CPlayerGun::PortStartRapidCharge() {
  mPortRapidCharge = PortDebug::RapidCharge();
  mPortRapidShots = 0;
  mPortRapidTimer = 0.f;
}

// Fires the beam's next quick shot when it is due. True while shots remain, so
// the charge waits for them, as CPlayerGunMP1::UpdateChargeState's rapid-fire
// state does.
bool CPlayerGun::PortRapidChargeShot(float dt, CStateManager& mgr) {
  const SPortRapidCharge* beam = mPortRapidCharge ? PortRapidChargeFor(x310_currentBeam) : nullptr;
  if (beam == nullptr || mPortRapidShots >= beam->shots) {
    return false;
  }
  // A held charge is requested again before the new beam's ToBeam animation ends.
  // Wait for the switch to finish: firing here calls ExitMissile, which can replace
  // ChangeWeapon/SetupBeam and leave the switching flag set, blocking every weapon.
  if (IsWeaponStateSet(0x8)) {
    return true;
  }
  mPortRapidTimer += dt;
  if (mPortRapidTimer < float(beam->shotFrames[mPortRapidShots]) / 60.f) {
    return true;
  }
  mPortRapidTimer = 0.f;
  ++mPortRapidShots;
  UpdateNormalShotCycle(dt, mgr);
  return mPortRapidShots < beam->shots;
}
#endif

void CPlayerGun::UpdateChargeState(float dt, CStateManager& mgr) {
  float animStart = kTractorBeamFactor;
  float fxStart = kChargeFxStart;
  float chargeSpeed = kChargeSpeed;
#ifdef TARGET_PC
  const bool powerRapidCharge = NPortPowerChargeRelease::IsScopedRapidCharge(
      mPortRapidCharge, x310_currentBeam == CPlayerState::kBI_Power);
  if (const SPortRapidCharge* beam = mPortRapidCharge ? PortRapidChargeFor(x310_currentBeam) : nullptr) {
    animStart = float(beam->triggered) / float(beam->full);
    fxStart = float(beam->warmUp) / float(beam->full);
    chargeSpeed = 60.f / float(beam->full);
  }
#else
  const bool powerRapidCharge = false;
#endif
#ifdef TARGET_PC
  // Remastered (UpdateChargeRumbleTriggered 0xc9b41c): one rumble once the charge time passes warmUp.
  if (sPortRemasteredGun && x834_24_charging && x830_chargeRumbleHandle == -1 &&
      x340_chargeBeamFactor > fxStart) {
    x830_chargeRumbleHandle = mgr.GetRumbleManager()->Rumble(mgr, kRFX_PlayerGunCharge, 1.f, kRP_Three);
  }
#endif
  switch (x32c_chargePhase) {
  case kCP_ChargeRequested:
#ifdef TARGET_PC
    if (PortRapidChargeShot(dt, mgr)) {
      x340_chargeBeamFactor = 0.f;
      // Remastered sets the charging flag before its quick-shot phase, so it bubbles.
      PortGenerateUnderWaterBubbles(mgr);
      return;
    }
#endif
    x340_chargeBeamFactor = 0.f;
    x330_chargeState = kCS_Normal;
    x832_27_chargeAnimStarted = false;
    x834_24_charging = true;
    x32c_chargePhase = kCP_AnimAndSfx;
    break;
  case kCP_AnimAndSfx:
    if (x832_27_chargeAnimStarted != true) {
      if (x340_chargeBeamFactor > kChargeStart && x832_25_chargeEffectVisible) {
        x832_25_chargeEffectVisible = false;
      }
      if (x340_chargeBeamFactor > animStart) {
        PlayAnim(NWeaponTypes::kGAT_ChargeUp, false);
        if (!x2e0_chargeSfx) {
          x2e0_chargeSfx = NWeaponTypes::play_sfx(sBeamChargeUpSound[x310_currentBeam],
                                                  x834_27_underwater, true, 0x4a);
        }
        if (x830_chargeRumbleHandle == -1 && !sPortRemasteredGun) {
          x830_chargeRumbleHandle =
              mgr.GetRumbleManager()->Rumble(mgr, kRFX_PlayerGunCharge, 1.f, kRP_Three);
        }
        x832_27_chargeAnimStarted = true;
        x330_chargeState = NPortPowerChargeRelease::ChargeStageAtAnimationStart(
            x330_chargeState, kCS_Charged, powerRapidCharge, x832_27_chargeAnimStarted);
      }
    } else if (x340_chargeBeamFactor >= fxStart && !IsWeaponStateSet(0x8)) {
      x832_25_chargeEffectVisible = true;
      x832_27_chargeAnimStarted = false;
      x32c_chargePhase = kCP_FxGrowing;
      x330_chargeState = kCS_Charged;
      EnableChargeFx(CPlayerState::kCS_Charged, mgr);
      PlayAnim(NWeaponTypes::kGAT_ChargeLoop, true);
    }
    break;
  case kCP_FxGrowing:
    if (x340_chargeBeamFactor >= 1.f) {
      x32c_chargePhase = kCP_FxGrown;
    }
    break;
  case kCP_ComboXfer:
    if (x344_comboXferTimer >= 1.f) {
      x32c_chargePhase = kCP_ComboXferDone;
      x832_25_chargeEffectVisible = false;
    }
    break;
  case kCP_ComboXferDone:
    x32c_chargePhase = kCP_ComboFire;
    x348_chargeCooldownTimer = 0.f;
    break;
  case kCP_ComboFire:
    x740_grappleArm->EnterComboFire(int(x310_currentBeam), mgr);
    x73c_gunMotion->PlayPasAnim(SamusGun::kAS_ComboFire, mgr, 0.f, false);
    x72c_currentBeam->PlayPasAnim(SamusGun::kAS_ComboFire, mgr, 0.f);
    x833_31_inFreeLook = false;
    x32c_chargePhase = kCP_ComboFireDone;
    break;
  case kCP_ChargeCooldown:
    if (!IsWeaponStateSet(0x10)) {
      x348_chargeCooldownTimer += dt;
      if (x348_chargeCooldownTimer >= 0.3f && x72c_currentBeam->IsChargeAnimOver()) {
        x32c_chargePhase = kCP_ChargeDone;
      }
    } else {
      x832_24_coolingCharge = false;
    }
    break;
  case kCP_ChargeDone:
    ResetCharge(mgr, false);
    Reset(mgr, false);
    break;
  default:
    break;
  }

  if (x2e0_chargeSfx) {
    CSfxManager::PitchBend(x2e0_chargeSfx, x834_27_underwater ? 0 : 0x2000);
  }

  if (x32c_chargePhase > kCP_NotCharging && x32c_chargePhase < kCP_FxGrown) {
    x340_chargeBeamFactor += chargeSpeed * dt;
    if (x340_chargeBeamFactor > 1.f) {
      x340_chargeBeamFactor = 1.f;
    }
  }
#ifdef TARGET_PC
  if (x834_24_charging) {
    PortGenerateUnderWaterBubbles(mgr);
  }
#endif
}

void CPlayerGun::Reset(CStateManager& mgr, bool b1) {
  x72c_currentBeam->Reset(mgr);
  x832_25_chargeEffectVisible = false;
  x832_24_coolingCharge = false;
  x833_26_ = false;
  x348_chargeCooldownTimer = 0.f;
  SetGunLightActive(false, mgr);
  if (!IsWeaponStateSet(0x10)) {
    if (!b1 && !IsWeaponStateSet(0x2)) {
      ResetToBeam();
    }
  } else {
    DisableWeaponState(0x7);
  }
}

void CPlayerGun::ResetCharge(CStateManager& mgr, bool resetBeam) {
  if (x32c_chargePhase != kCP_NotCharging) {
#if VERSION >= VERSION_GM8P_00
    x72c_currentBeam->ActivateCharge(false, false);
    SetGunLightActive(false, mgr);
#endif
    StopChargeSound(mgr);
  }

  if (!IsWeaponStateSet(0x8) && !IsWeaponStateSet(0x10)) {
    bool doResetBeam =
        mgr.GetPlayer()->GetMorphballTransitionState() == CPlayer::kMS_Morphed || resetBeam;
    if (x832_27_chargeAnimStarted || doResetBeam)
      PlayAnim(NWeaponTypes::kGAT_BasePosition, false);
    if (doResetBeam)
      x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_None);
    if (!IsWeaponStateSet(0x2) || x330_chargeState != kCS_Normal) {
      ResetToBeam();
    }
  }

  x32c_chargePhase = kCP_NotCharging;
  x330_chargeState = kCS_Normal;
  x320_currentAuxBeam = x310_currentBeam;
  x833_30_canShowAuxMuzzleEffect = true;
  x832_27_chargeAnimStarted = false;
  x832_26_comboFiring = false;
  x344_comboXferTimer = 0.f;
}

#define SFXwpn_morph_out_wipe 1774
#define SFXwpn_morph_in_wipe_done 1775

void CPlayerGun::ResetBeamParams(CStateManager& mgr, const CPlayerState& playerState,
                                 bool playSelectionSfx) {
  StopContinuousBeam(mgr, true);
  if (playerState.ItemEnabled(CPlayerState::kIT_ChargeBeam)) {
    ResetCharge(mgr, false);
  }
  const CAnimPlaybackParms parms(mHandAnimId[size_t(x314_nextBeam)], -1, 1.f, true);
  x6e0_rightHandModel.AnimationData()->SetAnimation(parms, false);
  Reset(mgr, false);
  if (playSelectionSfx) {
    CSfxManager::SfxStart(SFXwpn_morph_out_wipe, 0x7f, 0x40, true, CSfxManager::kMaxPriority, false,
                          CSfxManager::kAllAreas);
  }
  x2ec_lastFireButtonStates &= ~0x1;
  x320_currentAuxBeam = x310_currentBeam;
  x833_30_canShowAuxMuzzleEffect = true;
}

void CPlayerGun::ChangeWeapon(const CPlayerState& playerState, CStateManager& mgr) {
  if (x730_outgoingBeam != nullptr && x730_outgoingBeam != x72c_currentBeam)
    x730_outgoingBeam->Unload(mgr);

  x734_loadingBeam = x760_selectableBeams[x314_nextBeam];
  if (x734_loadingBeam != nullptr && x734_loadingBeam != x72c_currentBeam) {
    x734_loadingBeam->Load(mgr, false);
    x744_auxWeapon->Load(x314_nextBeam, mgr);
  }

  x72c_currentBeam->EnableFx(false);
  x834_28_requestImmediateRecharge = x32c_chargePhase != kCP_NotCharging;
  ResetBeamParams(mgr, playerState, true);
  x678_morph.StartWipe(CGunMorph::kD_In);
}

void CPlayerGun::PortForcePhazonBeam(const bool on) {
  x835_24_canFirePhazon = on;
  if (on) {
    x835_25_inPhazonBeam = true;
  }
}

#ifdef TARGET_PC
// Port: the Phazon Beam cheat. While on, the Phazon Beam takes the Power Beam's place: it is the
// selected Power Beam, in any room. The flags are the ones standing in phazon with the suit sets
// (the retail code drops canFirePhazon on leaving a pool, so they are set again every frame).
// Another beam is reached by leaving the Phazon Beam first (the game's own exit morph), then
// changing to the beam picked; picking Power again enters Phazon again. In a real phazon pool the
// retail code runs untouched.
void CPlayerGun::PortApplyPhazonCheat(const CFinalInput& input, CStateManager& mgr) {
  if (!PortDebug::PhazonBeamCheat()) {
    if (mPortPhazonCheat) {
      // Turned off: let go of the beam through the exit morph (a pool sets it again).
      mPortPhazonCheat = false;
      mPortPhazonPendingBeam = -1;
      if (!x835_30_inPhazonPool) {
        x835_24_canFirePhazon = false;
      }
    }
    return;
  }
  mPortPhazonCheat = true;
  if (IsWeaponStateSet(0x8) || x835_30_inPhazonPool) {
    return;
  }

  if (x835_25_inPhazonBeam && x33c_phazonBeamState == kPBS_Active && mPortPhazonPendingBeam < 0) {
    const CPlayerState& playerState = *mgr.GetPlayerState();
    float maxInput = 0.f;
    for (int i = 1; i < 4; ++i) {
      if (playerState.HasPowerUp(mBeamArr[i])) {
        const float inputVal = ControlMapper::GetAnalogInput(mBeamCtrlCmd[i], input);
        if (inputVal > 0.65f && inputVal > maxInput) {
          maxInput = inputVal;
          mPortPhazonPendingBeam = i;
        }
      }
    }
  }

  const bool phazon = mPortPhazonPendingBeam < 0 && x310_currentBeam == CPlayerState::kBI_Power;
  x835_24_canFirePhazon = phazon;
  if (phazon) {
    x835_25_inPhazonBeam = true;
  }
}
#endif

void CPlayerGun::StartPhazonBeamTransition(bool active, CStateManager& mgr,
                                           CPlayerState& playerState) {
  if (x833_28_phazonBeamActive == active) {
    return;
  }

  x760_selectableBeams[x310_currentBeam]->Unload(mgr);
  x760_selectableBeams[x310_currentBeam] = active ? x75c_phazonBeam.get() : x738_nextBeam;
  ResetBeamParams(mgr, playerState, false);
  x72c_currentBeam = x760_selectableBeams[x310_currentBeam];
  x833_28_phazonBeamActive = active;
  SetPhazonBeamFeedback(active);
  x72c_currentBeam->x1bc_rainSplashGenerator = x748_rainSplashGenerator.get();
  x72c_currentBeam->EnableFx(true);
  x72c_currentBeam->x218_29_drawHologram = false;
  PlayAnim(NWeaponTypes::kGAT_ToBeam, false);
  if (x833_31_inFreeLook) {
    EnterFreeLook(mgr);
  } else if (x832_30_requestReturnToDefault) {
    ReturnArmAndGunToDefault(mgr, false);
  }
  x832_30_requestReturnToDefault = false;
}

void CPlayerGun::HandleWeaponChange(const CFinalInput& input, CStateManager& mgr) {
  x833_25_ = false;
  if (ControlMapper::GetPressInput(ControlMapper::kC_Morph, input)) {
    StopContinuousBeam(mgr, true);
  }
#ifdef TARGET_PC
  PortApplyPhazonCheat(input, mgr);
#endif
  if (!IsWeaponStateSet(0x8)) {
    if (!x835_25_inPhazonBeam) {
      HandleBeamChange(input, mgr);
    } else {
      HandlePhazonBeamChange(mgr);
    }
  }
}

void CPlayerGun::HandleBeamChange(const CFinalInput& input, CStateManager& mgr) {
  const CPlayerState& playerState = *mgr.GetPlayerState();
  float maxInput = 0.f;
  int beam = -1;

  for (int i = 0; i < 4; ++i) {
    if (playerState.HasPowerUp(mBeamArr[i])) {
      const float inputVal = ControlMapper::GetAnalogInput(mBeamCtrlCmd[i], input);
      if (inputVal > 0.65f && inputVal > maxInput) {
        maxInput = inputVal;
        beam = i;
      }
    }
  }

#ifdef TARGET_PC
  // Port: the Phazon Beam cheat's pending pick, once the Phazon Beam has been left.
  if (mPortPhazonPendingBeam >= 0) {
    beam = mPortPhazonPendingBeam;
    mPortPhazonPendingBeam = -1;
  }
#endif
  if (beam > -1) {
    x833_25_ = true;
    if (x310_currentBeam != beam && playerState.HasPowerUp(mBeamArr[beam])) {
      x314_nextBeam = static_cast< CPlayerState::EBeamId >(beam);

      uint stateFlags = 0;
      if (IsWeaponStateSet(0x10)) {
        stateFlags = 0x10;
      }
      SetStateFlags(0);
      EnableWeaponState(stateFlags | 0x8);
      PlayAnim(NWeaponTypes::kGAT_FromBeam, false);

      if (x833_31_inFreeLook || x744_auxWeapon->IsComboFxActive(mgr) || x832_26_comboFiring) {
        x832_30_requestReturnToDefault = true;
        x740_grappleArm->EnterIdle(mgr);
      }

      x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_None);
      x338_nextState = kNS_ChangeWeapon;
      x2e4_invalidSfx.Clear();
    } else if (playerState.HasPowerUp(mBeamArr[beam])) {
      if (ExitMissile()) {
        if (!CSfxManager::IsPlaying(x2e4_invalidSfx)) {
          x2e4_invalidSfx =
              NWeaponTypes::play_sfx(SFXsam_a_mislemp_00, x834_27_underwater, false, 0x4a);
        }
      } else {
        x2e4_invalidSfx.Clear();
      }
    }
  }
}

void CPlayerGun::SetPhazonBeamMorph(bool intoPhazonBeam) {
  x39c_phazonMorphT = intoPhazonBeam ? 0.f : 1.f;
  const bool into = intoPhazonBeam;
  const int morphing = true;
  x835_27_intoPhazonBeam = into;
  x835_26_phazonBeamMorphing = morphing;
}

void CPlayerGun::HandlePhazonBeamChange(CStateManager& mgr) {
  bool inMorph = false;
  switch (x33c_phazonBeamState) {
  case kPBS_Inactive:
    SetPhazonBeamMorph(true);
    x338_nextState = kNS_EnterPhazonBeam;
    inMorph = true;
    break;
  case kPBS_Active:
    if (!x835_24_canFirePhazon) {
      SetPhazonBeamMorph(true);
      x338_nextState = kNS_ExitPhazonBeam;
      inMorph = true;
      CPhazonBeam* beam = x75c_phazonBeam.get();
      if (beam) {
        beam->mClipWipeActive = false;
        beam->mVeinsAlphaActive = true;
      }
    }
    break;
  default:
    break;
  }

  if (inMorph) {
    ResetBeamParams(mgr, *mgr.GetPlayerState(), true);
    SetStateFlags(0);
    EnableWeaponState(0x8);
    PlayAnim(NWeaponTypes::kGAT_FromBeam, false);
    if (x833_31_inFreeLook) {
      x832_30_requestReturnToDefault = true;
      x740_grappleArm->EnterIdle(mgr);
    }
    CancelCharge(mgr, false);
  }
}

void CPlayerGun::InitBeamData() {
  x760_selectableBeams[0] = x74c_powerBeam.get();
  x760_selectableBeams[1] = x750_iceBeam.get();
  x760_selectableBeams[2] = x754_waveBeam.get();
  x760_selectableBeams[3] = x758_plasmaBeam.get();
  x72c_currentBeam = x760_selectableBeams[0];
  x738_nextBeam = x72c_currentBeam;
  x774_holoTransitionGen->SetParticleEmission(true);
}

void CPlayerGun::InitBombData() {
  for (int i = 0; i < 2; ++i)
    x784_bombEffects.push_back(rstl::reserved_vector< TLockedToken< CGenDescription >, 3 >());

  TToken< CGenDescription > obj1 =
      gpSimplePool->GetObj(SObjectTag('PART', gpTweakGunRes->x28_bombSet));
  TToken< CGenDescription > obj2 =
      gpSimplePool->GetObj(SObjectTag('PART', gpTweakGunRes->x2c_bombExplode));
  TToken< CGenDescription > obj3 =
      gpSimplePool->GetObj(SObjectTag('PART', gpTweakGunRes->x30_powerBombExplode));

  x784_bombEffects[0].push_back(obj1);
  x784_bombEffects[0].push_back(obj2);
  x784_bombEffects[1].push_back(obj3);
  x784_bombEffects[1].push_back(obj3);

  // Remastered's extra bomb effects have no disc id; the import writes them under a name-derived
  // one. Only when present do bombs use Remastered's three generators.
  const SObjectTag attractTag('PART', PortRemastered::SyntheticEffectId("bombattract"));
  const SObjectTag secondaryTag('PART', PortRemastered::SyntheticEffectId("powerbombsecondary"));
  if (gpResourceFactory->CanBuild(attractTag) && gpResourceFactory->CanBuild(secondaryTag)) {
    x784_bombEffects[0].push_back(gpSimplePool->GetObj(attractTag));
    x784_bombEffects[1].push_back(gpSimplePool->GetObj(secondaryTag));
  }
}

#ifdef TARGET_PC
void CPlayerGun::PortInitFlashes() {
  static const uint kIds[kPortFlashCount] = {
      PortRemastered::SyntheticEffectId("missilemuzzleflash"),
      PortRemastered::SyntheticEffectId("supermissilemuzzleflash"),
      PortRemastered::SyntheticEffectId("icecombomuzzleflash"),
      PortRemastered::SyntheticEffectId("powerchargemuzzleflash"),
      PortRemastered::SyntheticEffectId("icechargemuzzleflash"),
      PortRemastered::SyntheticEffectId("wavechargemuzzleflash"),
      PortRemastered::SyntheticEffectId("plasmachargemuzzleflash"),
      PortRemastered::SyntheticEffectId("phazonchargemuzzleflash"),
  };
  mPortBubbleGens.Clear();
  mPortBubbleToken = rstl::optional_object_null();
  const SObjectTag bubbleTag('PART', PortRemastered::SyntheticEffectId("underwaterbubblesweaponfire"));
  if (gpResourceFactory->CanBuild(bubbleTag)) {
    mPortBubbleToken = TLockedToken< CGenDescription >(gpSimplePool->GetObj(bubbleTag));
  }
  mPortFlashTokens = rstl::reserved_vector< TLockedToken< CGenDescription >, kPortFlashCount >();
  for (int i = 0; i < kPortFlashCount; ++i) {
    mPortFlashToken[i] = -1;
    const SObjectTag tag('PART', kIds[i]);
    if (gpResourceFactory->CanBuild(tag)) {
      mPortFlashToken[i] = int(mPortFlashTokens.size());
      mPortFlashTokens.push_back(gpSimplePool->GetObj(tag));
    }
  }
}

void CPlayerGun::PortStartFlash(int slot) {
  if (slot < 0 || slot >= kPortFlashCount || mPortFlashToken[slot] < 0) {
    return;
  }
  mPortFlashGens[slot].Start(mPortFlashTokens[mPortFlashToken[slot]], 2);
}

void CPlayerGun::PortUpdateFlashes(float dt, const CVector3f& scale) {
  for (int i = 0; i < kPortFlashCount; ++i) {
    mPortFlashGens[i].Update(dt, x418_beamLocalXf, scale);
  }
}

void CPlayerGun::PortRenderFlashes(int first, int end) const {
  for (int i = first; i < end; ++i) {
    mPortFlashGens[i].Render();
  }
}

// CPlayerMP1::ShouldGenerateUnderWaterBubbles 0xcf14bc: in normal water with the ball's top
// (morphed) or the camera at least 0.9 below the surface.
static bool PortShouldGenerateUnderWaterBubbles(const CPlayer& player, const CStateManager& mgr) {
  if (!player.IsInFluid()) {
    return false;
  }
  const CScriptWater* water = TCastToConstPtr< CScriptWater >(mgr.GetObjectById(player.InFluidId()));
  if (water == nullptr || water->GetFluidPlane().GetFluidType() != CFluidPlane::kFT_NormalWater) {
    return false;
  }
  const float surfaceZ = water->GetTriggerBoundsWR().GetMaxPoint().GetZ();
  float depth;
  if (player.GetMorphballTransitionState() == CPlayer::kMS_Morphed) {
    depth = surfaceZ - (player.GetBallPosition().GetZ() + player.GetMorphBall()->GetBallRadius());
  } else {
    depth = surfaceZ - mgr.GetCameraManager()->GetCurrentCameraTransform(mgr).GetTranslation().GetZ();
  }
  return depth >= 0.9f;
}

// CPlayerGunMP1::GenerateUnderWaterBubbles 0xc9a780, every frame while charging: start (or
// resume) the module 0.55 along the muzzle's forward axis, placed by translation only
// (transform mode 2), and keep it alive for 0.25 s.
void CPlayerGun::PortGenerateUnderWaterBubbles(CStateManager& mgr) {
  if (!mPortBubbleToken || !PortShouldGenerateUnderWaterBubbles(*mgr.GetPlayer(), mgr)) {
    return;
  }
  mPortBubbleGens.StartOrResume(*mPortBubbleToken);
  const CTransform4f muzzleXf = x4a8_gunWorldXf * x418_beamLocalXf;
  mPortBubbleXf =
      CTransform4f::Translate(muzzleXf.GetTranslation() + muzzleXf.GetForward() * 0.55f);
  mPortBubbleTimer = 0.25f;
}

// UpdateUnderWaterBubbles (inlined in CPlayerGunMP1::Update after UpdateGunIdle).
void CPlayerGun::PortUpdateUnderWaterBubbles(float dt, CStateManager& mgr) {
  mPortBubbleTimer = rstl::max_val(mPortBubbleTimer - dt, 0.f);
  if (!PortShouldGenerateUnderWaterBubbles(*mgr.GetPlayer(), mgr) || mPortBubbleTimer <= 0.f) {
    mPortBubbleGens.FadeActive();
  }
  mPortBubbleGens.Update(dt, mPortBubbleXf, CVector3f(1.f, 1.f, 1.f));
}
#endif

void CPlayerGun::InitMuzzleData() {
#ifdef TARGET_PC
  PortInitFlashes();
#endif
  for (int i = 0; i < 5; ++i) {
    x7c0_auxMuzzleEffects.push_back(gpSimplePool->GetObj(SObjectTag(
        'PART', gpTweakGunRes->GetAuxMuzzleResId(static_cast< CPlayerState::EBeamId >(i)))));
    CElementGen* gen = rs_new CElementGen(x7c0_auxMuzzleEffects[i]);
    gen->SetParticleEmission(false);
    x800_auxMuzzleGenerators.push_back(gen);
  }
}

void CPlayerGun::InitCTData() { x77c_comboXferGen = rstl::auto_ptr< CElementGen >(); }

float CPlayerGun::GetBeamVelocity() const {
  if (x72c_currentBeam->IsLoaded())
    return x72c_currentBeam->GetVelocityInfo().GetVelocity(int(x330_chargeState)).GetY();
  return 10.f;
}

TUniqueId CPlayerGun::GetTargetId(CStateManager& mgr) {
  TUniqueId ret = mgr.GetPlayer()->GetOrbitTargetId();
  if (x832_26_comboFiring && ret == kInvalidUniqueId && x310_currentBeam == CPlayerState::kBI_Wave)
    ret = mgr.GetPlayer()->GetOrbitNextTargetId();

  if (ret != kInvalidUniqueId) {
    CActor* act = TCastToPtr< CActor >(const_cast< CEntity* >(mgr.GetObjectById(ret)));
    if (act != nullptr) {
      const uchar hasTarget = act->GetMaterialList().HasMaterial(kMT_Target) ? 1 : 0;
      if (hasTarget != 1) {
        ret = kInvalidUniqueId;
      }
    }
  }

  return ret;
}

bool CPlayerGun::PortShotOffLock(CStateManager& mgr) const {
  // A centred pointer reports no aim point, and plain pointer aim has no orbit target.
  return mPortPointerAimValid && mgr.GetPlayer()->GetOrbitTargetId() != kInvalidUniqueId;
}

TUniqueId CPlayerGun::PortHomingTargetId(CStateManager& mgr) {
  return PortShotOffLock(mgr) ? kInvalidUniqueId : GetTargetId(mgr);
}

CPlayerGun::CGunMorph::CGunMorph(float gunTransformTime, float holoHoldTime)
: x0_yLerp(0.f)
, x4_gunTransformTime(CMath::FastFSel(-gunTransformTime, 1.f, gunTransformTime))
, x8_remTime(0.f)
, xc_speed(0.1f)
, x10_holoHoldTime(fabs(holoHoldTime))
, x14_remHoldTime(2.f)
, x18_transitionFactor(1.f)
, x1c_dir(kD_Done)
, x20_gunState(kGS_OutWipeDone)
, x24_24_morphing(false)
, x24_25_weaponChanged(false) {}

void CPlayerGun::CGunMorph::StartWipe(CPlayerGun::CGunMorph::EDir dir) {
  x14_remHoldTime = x10_holoHoldTime;
  if (dir == kD_In && x20_gunState == kGS_InWipeDone)
    return;

  if (x1c_dir != dir && x20_gunState != kGS_OutWipe) {
    x8_remTime = x4_gunTransformTime;
    xc_speed = 1.f / x4_gunTransformTime;
  } else if (x20_gunState != kGS_InWipe) {
    x8_remTime = x4_gunTransformTime - x8_remTime;
  }

  x1c_dir = dir;
  x20_gunState = x1c_dir == kD_In ? kGS_InWipe : kGS_OutWipe;
  x24_24_morphing = true;
}

CPlayerGun::CGunMorph::EMorphEvent CPlayerGun::CGunMorph::Update(float inY, float outY, float dt) {
  EMorphEvent ret = kME_None;

  switch (x20_gunState) {
  case kGS_InWipeDone:
    x14_remHoldTime -= dt;
    if (x14_remHoldTime <= 0.f && x24_25_weaponChanged) {
      StartWipe(kD_Out);
      x24_25_weaponChanged = false;
      x14_remHoldTime = 0.f;
      ret = kME_InWipeDone;
    }
    // explicitly no break

  case kGS_OutWipeDone:
  case kGS_InWipe:
  case kGS_OutWipe:
  default:
    if (x24_24_morphing) {
      float omt = x8_remTime * xc_speed;
      float t = 1.f - omt;
      if (x1c_dir == kD_In) {
        x0_yLerp = inY * t + outY * omt;
        x18_transitionFactor = omt;
      } else {
        x0_yLerp = outY * t + inY * omt;
        x18_transitionFactor = t;
      }

      if (x8_remTime <= 0.f) {
        x24_24_morphing = false;
        x8_remTime = 0.f;
        if (x1c_dir == kD_In) {
          x20_gunState = kGS_InWipeDone;
          x18_transitionFactor = 0.f;
        } else {
          x18_transitionFactor = 1.f;
          x20_gunState = kGS_OutWipeDone;
          x1c_dir = kD_Done;
          ret = kME_OutWipeDone;
        }
      } else {
        x8_remTime -= dt;
      }
    }
  }

  return ret;
}

void CPlayerGun::UpdateWeaponFire(float dt, CPlayerState& playerState, CStateManager& mgr) {
  uint oldFiring = x2ec_lastFireButtonStates;
  uint fireButtonStates = x2f4_fireButtonStates;
  x2ec_lastFireButtonStates = fireButtonStates;
  uint releasedStates = oldFiring & (oldFiring ^ fireButtonStates);
  uint pressedStates = fireButtonStates & (oldFiring ^ fireButtonStates);
  x2f0_pressedFireButtonStates = pressedStates;
  const CPlayer::EPlayerMorphBallState morphState = mgr.GetPlayer()->GetMorphballTransitionState();
  bool chargeRequested = x32c_chargePhase != kCP_NotCharging;

  x832_28_readyForShot = false;
  CPlayer& player = *mgr.Player();

  if (!x832_24_coolingCharge && !x834_30_inBigStrike) {
    float coolDown = x72c_currentBeam->GetWeaponInfo().x0_coolDown;
    if ((pressedStates & 0x1) == 0) {
      if (x390_cooldown >= coolDown) {
        x390_cooldown = coolDown;
        if (morphState == CPlayer::kMS_Unmorphed &&
            playerState.ItemEnabled(CPlayerState::kIT_ChargeBeam) &&
            player.GetGunHolsterState() == CPlayer::kGH_Drawn &&
            player.GetGrappleState() == CPlayer::kGS_None &&
            mgr.GetPlayerState()->GetTransitioningVisor() != CPlayerState::kPV_Scan &&
            mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan &&
            (x2ec_lastFireButtonStates & 0x1) != 0 && !chargeRequested) {
          x832_28_readyForShot = true;
          pressedStates |= 0x1;
          x390_cooldown = 0.f;
        }
      }
    } else if (x390_cooldown >= coolDown) {
      x832_28_readyForShot = true;
      x390_cooldown = 0.f;
    }
    x390_cooldown += dt;
  }

  if (x834_28_requestImmediateRecharge)
    x834_28_requestImmediateRecharge = (x2ec_lastFireButtonStates & 0x1) != 0;

  if (morphState == CPlayer::kMS_Morphed) {
    x835_28_bombReady = false;
    x835_29_powerBombReady = false;
    if (!x835_31_actorAttached) {
      x835_28_bombReady = true;
      if (x53a_powerBomb != kInvalidUniqueId &&
          !mgr.CanCreateProjectile(x538_playerId, kWT_PowerBomb, 1)) {
        const CPowerBomb* pb = static_cast< const CPowerBomb* >(mgr.GetObjectById(x53a_powerBomb));
        if (pb != NULL && !(pb->GetCurTime() > CPowerBomb::EndingTime())) {
          x835_28_bombReady = false;
        } else {
          x53a_powerBomb = kInvalidUniqueId;
        }
      }
      if (((pressedStates & 0x1) != 0 || chargeRequested) &&
          playerState.HasPowerUp(CPlayerState::kIT_MorphBallBombs)) {
        if (x835_28_bombReady)
          DropBomb(kBW_Bomb, mgr);
      } else {
        x835_29_powerBombReady = playerState.HasPowerUp(CPlayerState::kIT_PowerBombs) &&
                                 playerState.GetItemAmount(CPlayerState::kIT_PowerBombs) > 0 &&
                                 mgr.CanCreateProjectile(x538_playerId, kWT_PowerBomb, 1) &&
                                 mgr.CanCreateProjectile(x538_playerId, kWT_Bomb, 1);
        if ((pressedStates & 0x2) != 0 && x835_29_powerBombReady)
          DropBomb(kBW_PowerBomb, mgr);
      }
    }
  } else {
    if (IsWeaponStateSet(0x8) || morphState != CPlayer::kMS_Unmorphed) {
      return;
    }
    if ((pressedStates & 0x2) != 0 && static_cast< int >(x318_comboAmmoIdx) == 0 &&
        !IsWeaponStateSet(0x2) && x32c_chargePhase == kCP_NotCharging) {
      int missileCount = playerState.GetItemAmount(CPlayerState::kIT_Missiles);
      if (x338_nextState != kNS_EnterMissile && x338_nextState != kNS_ExitMissile) {
        if (playerState.HasPowerUp(CPlayerState::kIT_Missiles) && missileCount > 0) {
          x300_remainingMissiles = missileCount;
          if (static_cast< int >(x300_remainingMissiles) > 5)
            x300_remainingMissiles = 5;
          if (!x835_25_inPhazonBeam
#ifdef TARGET_PC
              || mPortPhazonCheat // Port: Phazon Beam cheat, missiles work in it
#endif
          ) {
            ResetToMissile();
          }
          FireSecondary(dt, mgr);
        } else {
          if (!CSfxManager::IsPlaying(x2e4_invalidSfx)) {
            x2e4_invalidSfx =
                NWeaponTypes::play_sfx(SFXsam_b_malfxn_00, x834_27_underwater, false, 0x4a);
          } else {
            x2e4_invalidSfx.Clear();
          }
        }
      }
    } else {
      if (x3a4_fidget.GetState() == CFidget::kS_NoFidget) {
        if (IsWeaponStateSet(0x10) && x744_auxWeapon->IsComboFxActive(mgr)) {
          bool stopCombo = false;
          if (x310_currentBeam == CPlayerState::kBI_Wave && x833_29_pointBlankWorldSurface) {
            stopCombo = true;
          }
          if (GetFiring() == 0 || stopCombo) {
            StopContinuousBeam(mgr, IsWeaponStateSet(0x8));
          }
        } else {
          if (playerState.ItemEnabled(CPlayerState::kIT_ChargeBeam) &&
              x33c_phazonBeamState == kPBS_Inactive)
            ProcessChargeState(releasedStates, pressedStates, mgr, dt);
          else
            ProcessNormalState(releasedStates, pressedStates, mgr, dt);
        }
      }
    }
  }
}

void CPlayerGun::ResetIdle(CStateManager& mgr) {
  CFidget::EState fidgetState = x3a4_fidget.GetState();
  x370_gunMotionSpeedMult = 1.f;
  x550_camBob.SetState(CPlayerCameraBob::kCBS_GunFireNoBob, mgr);
  if (fidgetState != CFidget::kS_NoFidget) {
    if (fidgetState == CFidget::kS_Loading) {
      UnLoadFidget();
    }
    ReturnArmAndGunToDefault(mgr, true);
  }
  x3a4_fidget.ResetAll();
  ReturnToRestPose();
  if (x324_idleState != kIS_NotIdle)
    x324_idleState = kIS_NotIdle;
  if (!x740_grappleArm->GetActive())
    x834_26_animPlaying = false;
}

void CPlayerGun::UpdateGunIdle(bool inStrikeCooldown, float camBobT, float dt, CStateManager& mgr) {
  CPlayer& player = *mgr.Player();

  if (player.IsInFreeLook() && !x832_29_lockedOn && !x740_grappleArm->IsGrappling() &&
      x3a4_fidget.GetState() != CFidget::kS_HolsterBeam &&
      player.GetGunHolsterState() == CPlayer::kGH_Drawn && !x834_30_inBigStrike) {
    if (!IsWeaponStateSet(0x8)) {
      if (x833_31_inFreeLook != true && !x834_26_animPlaying) {
        if (x388_enterFreeLookDelayTimer < 0.25f)
          x388_enterFreeLookDelayTimer += dt;
        if (x388_enterFreeLookDelayTimer >= 0.25f && !x740_grappleArm->IsSuitLoading()) {
          EnterFreeLook(mgr);
          x833_31_inFreeLook = true;
        }
      } else {
        x388_enterFreeLookDelayTimer = 0.f;
        if (x834_26_animPlaying)
          ResetIdle(mgr);
      }
    }
  } else {
    if (x833_31_inFreeLook) {
      if (!IsWeaponStateSet(0x10)) {
        x73c_gunMotion->ReturnToDefault(mgr, x834_30_inBigStrike);
        x740_grappleArm->ReturnToDefault(mgr, 0.f, false);
      }
      x833_31_inFreeLook = false;
    }
    x388_enterFreeLookDelayTimer = 0.f;
    const CPlayerState& playerState = *mgr.GetPlayerState();
    const bool moving = camBobT > 0.01f;
    const bool firing = (x2f4_fireButtonStates & 0x3) ? true : false;
    x833_24_notFidgeting = player.GetMorphballTransitionState() == CPlayer::kMS_Morphed ||
                           player.GetSurfaceRestraint() == CPlayer::kSR_Water ||
                           playerState.GetCurrentVisor() == CPlayerState::kPV_Scan || firing ||
                           x32c_chargePhase != kCP_NotCharging || x832_29_lockedOn ||
                           IsWeaponStateSet(0x8) || x364_gunStrikeCoolTimer > 0.f ||
                           player.GetPlayerMovementState() != NPlayer::kMS_OnGround ||
                           player.IsInFreeLook() || player.GetFreeLookStickState() ||
                           player.GetOrbitState() != CPlayer::kOS_NoOrbit ||
                           CMath::AbsF(player.GetAngularVelocityOR().GetAngle()) > 0.1f || moving ||
                           mgr.GetCameraManager()->IsInCinematicCamera() ||
                           player.GetGunHolsterState() != CPlayer::kGH_Drawn ||
                           player.GetGrappleState() != CPlayer::kGS_None || x834_30_inBigStrike ||
                           x835_25_inPhazonBeam;
    if (x833_24_notFidgeting) {
      if (!x834_30_inBigStrike) {
        bool doWander = moving && !firing;
        if (doWander) {
          x370_gunMotionSpeedMult = 1.f;
          x374_ = 0.f;
          if (x364_gunStrikeCoolTimer <= 0.f && x368_idleWanderDelayTimer <= 0.f) {
            x368_idleWanderDelayTimer = 8.f;
            x73c_gunMotion->PlayPasAnim(SamusGun::kAS_Wander, mgr, 0.f, false);
            x324_idleState = kIS_Wander;
            x550_camBob.SetState(CPlayerCameraBob::kCBS_Walk, mgr);
          }
          x368_idleWanderDelayTimer -= dt;
          x360_ += dt;
        }
        if (!doWander || x834_26_animPlaying)
          ResetIdle(mgr);
      } else if (x394_damageTimer > 0.f) {
        x394_damageTimer -= dt;
      } else if (x834_31_gunMotionInFidgetBasePosition != true) {
        x394_damageTimer = 0.f;
        x834_31_gunMotionInFidgetBasePosition = true;
        x73c_gunMotion->BasePosition(true);
      } else if (!x73c_gunMotion->GetModelData().GetAnimationData()->IsAnimTimeRemaining(
                     0.001f, rstl::string_l("Whole Body"))) {
        x834_30_inBigStrike = false;
        x834_31_gunMotionInFidgetBasePosition = false;
      }
    } else {
      switch (x3a4_fidget.Update(x2ec_lastFireButtonStates, camBobT > 0.01f, inStrikeCooldown, dt,
                                 mgr)) {
      case CFidget::kS_NoFidget:
        if (x324_idleState != kIS_Idle) {
          x73c_gunMotion->PlayPasAnim(SamusGun::kAS_Idle, mgr, 0.f, false);
          x324_idleState = kIS_Idle;
        }
        x550_camBob.SetState(CPlayerCameraBob::kCBS_WalkNoBob, mgr);
        break;
      case CFidget::kS_MinorFidget:
      case CFidget::kS_MajorFidget:
      case CFidget::kS_HolsterBeam:
        if (x324_idleState != kIS_NotIdle) {
          x73c_gunMotion->BasePosition(false);
          x324_idleState = kIS_NotIdle;
        }
        AsyncLoadFidget(mgr);
        break;
      case CFidget::kS_Loading:
        if (IsFidgetLoaded())
          EnterFidget(mgr);
        break;
      case CFidget::kS_StillMinorFidget:
      case CFidget::kS_StillMajorFidget: {
        x550_camBob.SetState(CPlayerCameraBob::kCBS_Walk, mgr);
        x833_24_notFidgeting = false;
        x834_26_animPlaying =
            x834_25_gunMotionFidgeting
                ? x73c_gunMotion->IsAnimPlaying()
                : x72c_currentBeam->GetSolidModelData().GetAnimationData()->IsAnimTimeRemaining(
                      0.001f, rstl::string_l("Whole Body"));
        if (!x834_26_animPlaying) {
          x3a4_fidget.ResetMinor();
          ReturnToRestPose();
        }
        break;
      }
      default:
        break;
      }
    }
    x550_camBob.Update(dt, mgr);
  }
}

void CPlayerGun::CMotionState::Update(bool firing, float dt, CTransform4f& xf, CStateManager& mgr) {
  if (firing) {
    x24_fireState = kFS_StartFire;
    x8_fireTime = 0.f;
  } else if (x24_fireState != kFS_NotFiring) {
    if (x8_fireTime > dt)
      x24_fireState = kFS_Firing;
    x8_fireTime += dt;
  }

  if (x0_24_extendParabola && x20_state == kMS_LockOn) {
    float extendT = xc_curExtendDist * (1.0f / gGunExtendDistance);
    CTransform4f other =
        CTransform4f::RotateZ(CRelAngle::FromDegrees(extendT * -4.f * (extendT - 1.f) * 15.f));
#ifdef TARGET_PC
    // Remastered pitches the gun 10 degrees on the lock-on extend instead of
    // yawing it 15: RotateX(+a) in its swizzled gun space ((x,y,z) -> (-x,z,y))
    // is RotateX(-a) here (kb func/gunmotion-state-sway-extend).
    if (sPortRemasteredGun) {
      other = CTransform4f::RotateX(CRelAngle::FromDegrees(extendT * 4.f * (extendT - 1.f) * 10.f));
    }
#endif
    other.SetTranslation(CVector3f(0.f, xc_curExtendDist, 0.f));
    xf = xf * other;
  } else if (x24_fireState == kFS_StartFire || x24_fireState == kFS_Firing) {
    if (fabs(x14_rotationT - 1.f) < 0.1f) {
      x18_startRotation = x1c_endRotation;
      x14_rotationT = 0.f;
      if (x24_fireState == kFS_StartFire) {
        x1c_endRotation = CCast::StoF(mgr.Random()->Next() % 15);
        x1c_endRotation *= (mgr.Random()->Next() % 100) > 45 ? 1.f : -1.f;
      } else {
        x1c_endRotation = 0.f;
        if (x18_startRotation == x1c_endRotation) {
          x10_curRotation = x1c_endRotation;
          x24_fireState = kFS_NotFiring;
        }
      }
    } else {
      x10_curRotation = (x1c_endRotation - x18_startRotation) * x14_rotationT + x18_startRotation;
    }

    x14_rotationT += (10.f * dt) * (0.8f * (1.f - x14_rotationT));

    const CRelAngle angle = CRelAngle::FromDegrees(x10_curRotation);
    CQuaternion quat = CQuaternion::AxisAngle(CUnitVector3f(xf.GetForward()), angle);

    CTransform4f tmpXf = quat.BuildTransform4f() * xf.GetRotation();
    tmpXf.SetTranslation(xf.GetTranslation());
    xf = tmpXf * CTransform4f::Translate(0.f, xc_curExtendDist, 0.f);
  } else {
    xf *= CTransform4f::Translate(0.f, xc_curExtendDist, 0.f);
  }

  switch (x20_state) {
  case kMS_LockOn:
    xc_curExtendDist += 3.f * dt;
    if (xc_curExtendDist > gGunExtendDistance) {
      xc_curExtendDist = gGunExtendDistance;
      x20_state = kMS_One;
      x0_24_extendParabola = false;
    }
    break;
  case kMS_CancelLockOn:
    xc_curExtendDist -= 3.f * dt;
    if (xc_curExtendDist < 0.f) {
      xc_curExtendDist = 0.f;
      x20_state = kMS_Zero;
    }
    break;
  default:
    break;
  }

  if (x0_24_extendParabola != true) {
    if (x4_extendParabolaDelayTimer < 30.f) {
      x4_extendParabolaDelayTimer += dt;
    } else {
      x0_24_extendParabola = true;
      x4_extendParabolaDelayTimer = 0.f;
    }
  }
}

void CPlayerGun::DamageRumble(const CVector3f& location, float damage, const CStateManager&) {
  x398_damageAmt = damage;
  x3dc_damageLocation = location;
}

void CPlayerGun::TakeDamage(bool bigStrike, bool notFromMetroid, CStateManager& mgr) {
  const CPlayer& player = *mgr.GetPlayer();
  bool hasStrikeAngle = false;
  float angle = 0.f;
  if (x398_damageAmt >= 10.f && !bigStrike && !IsWeaponStateSet(0x10) && !x832_26_comboFiring &&
      x384_gunStrikeDelayTimer <= 0.f) {
    x384_gunStrikeDelayTimer = 20.f;
    x364_gunStrikeCoolTimer = 0.75f;
    if (x678_morph.GetGunState() == CGunMorph::kGS_OutWipeDone) {
      CVector3f localDamageLoc = player.GetTransform().TransposeRotate(x3dc_damageLocation);
      angle =
          CAbsAngle::FromRadians(atan2(localDamageLoc.GetY(), localDamageLoc.GetX())).AsDegrees();
      hasStrikeAngle = true;
    }
  }

  if (hasStrikeAngle || bigStrike) {
    if (mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan) {
      x73c_gunMotion->PlayPasAnim(SamusGun::kAS_Struck, mgr, angle, bigStrike);
      if ((bigStrike && notFromMetroid) || x833_31_inFreeLook)
        x740_grappleArm->EnterStruck(mgr, angle, bigStrike, !x833_31_inFreeLook);
    }
  }

  x398_damageAmt = 0.f;
  x3dc_damageLocation = CVector3f::Zero();
}

void CPlayerGun::StopChargeSound(CStateManager& mgr) {
  if (x2e0_chargeSfx) {
    CSfxManager::SfxStop(x2e0_chargeSfx);
    x2e0_chargeSfx.Clear();
  }
  if (x830_chargeRumbleHandle != -1) {
    mgr.GetRumbleManager()->StopRumble(x830_chargeRumbleHandle);
    x830_chargeRumbleHandle = -1;
  }
}

void CPlayerGun::CancelFiring(CStateManager& mgr) {
  if (x32c_chargePhase == kCP_ComboFireDone)
    ReturnArmAndGunToDefault(mgr, true);

  if (IsWeaponStateSet(0x10)) {
    StopContinuousBeam(mgr, true);
    ResetToBeam();
  }

  if (x32c_chargePhase != kCP_NotCharging) {
    x72c_currentBeam->ActivateCharge(false, false);
    SetGunLightActive(false, mgr);
    ResetCharge(mgr, true);
  }

  Reset(mgr, IsWeaponStateSet(0x2));
}

void CPlayerGun::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId sender, CStateManager& mgr) {
  const CPlayer& player = *mgr.GetPlayer();
  const CPlayerState& playerState = *mgr.GetPlayerState();
  const bool isUnmorphed = player.GetMorphballTransitionState() == CPlayer::kMS_Unmorphed;

  switch (msg) {
  case kSM_Registered: {
    CreateGunLight(mgr);
    const CPlayerState::EBeamId currentBeam = playerState.GetCurrentBeam();
#if VERSION >= VERSION_GM8P_00
    LoadBeam(currentBeam, mgr);
#else
    const CPlayerState::EBeamId beam = mCurrentBeamId[currentBeam];
    x320_currentAuxBeam = beam;
    x314_nextBeam = beam;
    x310_currentBeam = beam;
    x72c_currentBeam = x760_selectableBeams[x310_currentBeam];
    x738_nextBeam = x72c_currentBeam;
    x72c_currentBeam->Load(mgr, true);
    x72c_currentBeam->x1bc_rainSplashGenerator = x748_rainSplashGenerator.get();
    x744_auxWeapon->Load(x310_currentBeam, mgr);
#endif
    x6e0_rightHandModel.AnimationData()->SetAnimation(
        CAnimPlaybackParms(mHandAnimId[currentBeam], -1, 1.f, true), false);
    break;
  }
  case kSM_Deleted:
    DeleteGunLight(mgr);
    break;
  case kSM_UpdateSplashInhabitant:
    if (playerState.HasPowerUp(CPlayerState::kIT_PhazonSuit) && isUnmorphed) {
      if (const CScriptWater* water = TCastToConstPtr< CScriptWater >(mgr.GetObjectById(sender))) {
        if (water->GetFluidPlane().GetFluidType() == CFluidPlane::kFT_PhazonFluid) {
          x835_24_canFirePhazon = true;
          x835_25_inPhazonBeam = true;
        }
      }
    }

    if (player.GetDistanceUnderWater() > player.GetEyeHeight()) {
      x834_27_underwater = true;
      if (x744_auxWeapon->IsComboFxActive(mgr) && x310_currentBeam != CPlayerState::kBI_Wave) {
        StopContinuousBeam(mgr, false);
      }
    } else {
      x834_27_underwater = false;
    }
    break;
  case kSM_RemoveSplashInhabitant:
    x834_27_underwater = false;
    x835_24_canFirePhazon = false;
    break;
  case kSM_AddPhazonPoolInhabitant:
    x835_30_inPhazonPool = true;
    if (playerState.HasPowerUp(CPlayerState::kIT_PhazonSuit) && isUnmorphed) {
      x835_24_canFirePhazon = true;
    }
    break;
  case kSM_UpdatePhazonPoolInhabitant:
    x835_30_inPhazonPool = true;
    if (playerState.HasPowerUp(CPlayerState::kIT_PhazonSuit) && isUnmorphed) {
      x835_24_canFirePhazon = true;
      x835_25_inPhazonBeam = true;
      CPhazonBeam* phazon;
      if (x833_28_phazonBeamActive) {
        phazon = static_cast< CPhazonBeam* >(x72c_currentBeam);
      } else {
        break;
      }
      if (phazon->IsFiring(mgr)) {
        if (CEntity* ent = TCastToPtr< CEntity >(mgr.ObjectById(sender))) {
          mgr.DeliverScriptMsg(ent, x538_playerId, kSM_Decrement);
        }
      }
    }
    break;
  case kSM_RemovePhazonPoolInhabitant:
    x835_30_inPhazonPool = false;
    x835_24_canFirePhazon = false;
    break;
  case kSM_Damage: {
    bool bigStrike = false;
    bool metroidAttached = false;

    const CEnergyProjectile* proj = TCastToConstPtr< CEnergyProjectile >(mgr.GetObjectById(sender));
    if (proj && (proj->GetAttribField() & CWeapon::kPA_BigStrike) == CWeapon::kPA_BigStrike) {
      x394_damageTimer = proj->GetDamageDuration();
      bigStrike = true;
    } else if (const CPatterned* ai = TCastToConstPtr< CPatterned >(mgr.GetObjectById(sender))) {
      if (ai->IsMakingBigStrike()) {
        x394_damageTimer = ai->GetDamageDuration();
        bigStrike = true;
        const TUniqueId attachedActor = player.GetAttachedActor();
        if (attachedActor != kInvalidUniqueId) {
          metroidAttached = PATTERNED_CAST_TO(CMetroid, const_cast< CEntity* >(mgr.GetObjectById(attachedActor))) != nullptr;
        }
      }
    }

    if (!x834_30_inBigStrike) {
      if (bigStrike) {
        x834_31_gunMotionInFidgetBasePosition = false;
        CancelFiring(mgr);
      }
      TakeDamage(static_cast< const bool& >(bigStrike), !metroidAttached, mgr);
      x834_30_inBigStrike = bigStrike;
    }
    break;
  }
  case kSM_OnFloor:
    if (player.GetControlsFrozen() && !x834_30_inBigStrike) {
      x2f4_fireButtonStates = 0;
      x2ec_lastFireButtonStates = 0;
      CancelFiring(mgr);
      TakeDamage(true, false, mgr);
      x394_damageTimer = 0.75f;
      x834_30_inBigStrike = true;
    }
    break;
  default:
    break;
  }

  x740_grappleArm->AcceptScriptMsg(msg, sender, mgr);
  CPlasmaBeam* plasmaBeam = x758_plasmaBeam.get();
  plasmaBeam->AcceptScriptMsg(msg, sender, mgr);
  CPhazonBeam* phazonBeam = x75c_phazonBeam.get();
  phazonBeam->AcceptScriptMsg(msg, sender, mgr);
  x744_auxWeapon->AcceptScriptMsg(msg, sender, mgr);
}

void CPlayerGun::StopContinuousBeam(CStateManager& mgr, bool stopSfx) {
  if (IsWeaponStateSet(0x10)) {
    ReturnArmAndGunToDefault(mgr, false);
    x744_auxWeapon->StopComboFx(mgr, stopSfx);

    bool doFx = true;
    if (x310_currentBeam != CPlayerState::kBI_Ice) {
      if (x310_currentBeam < CPlayerState::kBI_Ice) {
        if (x310_currentBeam < CPlayerState::kBI_Power) {
          return;
        }
      } else if (x310_currentBeam >= CPlayerState::kBI_Phazon) {
        return;
      }

      if (x310_currentBeam == CPlayerState::kBI_Power && !x833_28_phazonBeamActive) {
        doFx = false;
      }
      if (doFx) {
        x72c_currentBeam->EnableSecondaryFx(stopSfx ? CGunWeapon::kSFT_None
                                                    : CGunWeapon::kSFT_CancelCharge);
      }
    }
  } else if (x833_28_phazonBeamActive) {
    CPhazonBeam* beam = static_cast< CPhazonBeam* >(x72c_currentBeam);
    if (beam->IsFiring(mgr)) {
      beam->StopBeam(mgr, stopSfx);
    }
  } else if (x310_currentBeam == CPlayerState::kBI_Plasma) {
    CPlasmaBeam* beam = static_cast< CPlasmaBeam* >(x72c_currentBeam);
    if (beam->IsFiring(mgr)) {
      beam->StopBeam(mgr, static_cast< const bool >(stopSfx));
    }
  }
}

void CPlayerGun::RenderEnergyDrainEffects(const CStateManager& mgr) const {
  const CEnergyDrainSource* it;
  const CPlayer* const player = TCastToConstPtr< CPlayer >(mgr.GetObjectById(x538_playerId));
  if (player != nullptr) {
    it = player->GetPlayerEnergyDrain().GetEnergyDrainSources().data();
    while (it != player->GetPlayerEnergyDrain().GetEnergyDrainSources().data() +
                     player->GetPlayerEnergyDrain().GetEnergyDrainSources().size()) {
      CMetroidBeta* metroid = PATTERNED_CAST_TO(CMetroidBeta, const_cast< CEntity* >(mgr.GetObjectById(it->GetEnergyDrainSourceId())));
      if (metroid != nullptr) {
        metroid->RenderHitGunEffect();
        return;
      }
      ++it;
    }
  }
}

void CPlayerGun::DoUserAnimEvents(float dt, CStateManager& mgr) {
  const int aid = mgr.GetPlayer()->GetCurrentAreaId().Value();
  const CAnimData& animData = *x72c_currentBeam->GetSolidModelData().GetAnimationData();
  const CGameCamera& camera = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  const CVector3f origin = x3e8_xf.GetTranslation();
  const CVector3f posToCam = camera.GetTranslation() - origin;

  int soundNodeCount = 0;
  const CSoundPOINode* soundNodes = animData.GetSoundPOIList(soundNodeCount);
  if (soundNodeCount > 0) {
    for (int i = 0; i < soundNodeCount; ++i) {
      const CSoundPOINode* soundNode = &soundNodes[i];
      const int charIdx = soundNode->GetCharacterIndex();
      if (soundNode->GetPoiType() != kPT_Sound)
        continue;
      if (charIdx != -1 && charIdx != animData.GetCharacterIndex())
        continue;
      NWeaponTypes::do_sound_event(x670_animSfx, x328_animSfxPitch, false, soundNode->GetSoundId(),
                                   soundNode->GetWeight(), soundNode->GetFlags(),
                                   soundNode->GetFallOff(), soundNode->GetMaxDistance(), 0x14,
                                   CAudioSys::kMaxVolume, posToCam, origin, aid, mgr);
    }
  }

  int intNodeCount = 0;
  const CInt32POINode* intNodes = animData.GetInt32POIList(intNodeCount);
  if (intNodeCount > 0) {
    for (int i = 0; i < intNodeCount; ++i) {
      const CInt32POINode* intNode = &intNodes[i];
      switch (intNode->GetPoiType()) {
      case kPT_UserEvent:
        DoUserAnimEvent(dt, mgr, *intNode, static_cast< EUserEventType >(intNode->GetValue()));
        break;
      case kPT_SoundInt32: {
        const int charIdx = intNode->GetCharacterIndex();
        if (charIdx != -1 && charIdx != animData.GetCharacterIndex())
          break;
        NWeaponTypes::do_sound_event(x670_animSfx, x328_animSfxPitch, false, intNode->GetValue(),
                                     intNode->GetWeight(), intNode->GetFlags(), 0.1f, 150.f, 0x14,
                                     CAudioSys::kMaxVolume, posToCam, origin, aid, mgr);
        break;
      }
      default:
        break;
      }
    }
  }
}

void CPlayerGun::DoUserAnimEvent(float dt, CStateManager& mgr, const CInt32POINode&,
                                 EUserEventType type) {
  switch (type) {
  case kUE_Projectile: {
    if (x32c_chargePhase != kCP_ComboFireDone) {
      return;
    }

    uchar fireSecondary = 0;
    if (x310_currentBeam != CPlayerState::kBI_Wave &&
        x310_currentBeam != CPlayerState::kBI_Plasma) {
      fireSecondary = 1;
    }

    bool doFireSecondary = fireSecondary ? true : (x2ec_lastFireButtonStates & 0x1);
    if (doFireSecondary) {
      FireSecondary(dt, mgr);
    }
    if (!IsWeaponStateSet(0x10)) {
      EnableWeaponState(0x10);
    }
    CancelCharge(mgr, true);
    if (doFireSecondary) {
      x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_ToCombo);
    }
    break;
  }
  case kUE_Delete:
  case kUE_DamageOn:
  default:
    break;
  }
}

void CPlayerGun::CancelCharge(CStateManager& mgr, bool withEffect) {
  if (withEffect) {
    x32c_chargePhase = kCP_ChargeCooldown;
    x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_CancelCharge);
  } else {
    x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_None);
  }

  x834_24_charging = false;
  x348_chargeCooldownTimer = 0.f;
  x72c_currentBeam->ActivateCharge(false, false);
  SetGunLightActive(false, mgr);
}

void CPlayerGun::EnterFreeLook(CStateManager& mgr) {
  if (!x832_30_requestReturnToDefault)
    x73c_gunMotion->PlayPasAnim(SamusGun::kAS_FreeLook, mgr, 0.f, false);

  const int setId = x73c_gunMotion->GetFreeLookSetId();
  x740_grappleArm->EnterFreeLook(x835_25_inPhazonBeam ? 1 : x310_currentBeam, setId, mgr);
}

void CPlayerGun::EnterFidget(CStateManager& mgr) {
  SamusGun::EFidgetType type = x3a4_fidget.GetType();
  int animSet = x3a4_fidget.GetAnimSet();

  if ((x2fc_fidgetAnimBits & 0x1) == 0x1) {
    x73c_gunMotion->EnterFidget(mgr, type, animSet);
    x834_25_gunMotionFidgeting = true;
  } else {
    x834_25_gunMotionFidgeting = false;
  }

  if ((x2fc_fidgetAnimBits & 0x2) == 0x2) {
    x72c_currentBeam->EnterFidget(mgr, type, animSet);
  }

  if ((x2fc_fidgetAnimBits & 0x4) == 0x4) {
    x740_grappleArm->EnterFidget(mgr, type, type != SamusGun::kFT_Minor ? x310_currentBeam : 0,
                                 animSet);
  }

  UnLoadFidget();
  x3a4_fidget.DoneLoading();
}

void CPlayerGun::UpdateLeftArmTransform(const CModelData& modelData, const CStateManager&) {
  CVector3f elbowOffset(-0.9f, -0.4f, 0.4f);
  CTransform4f& auxXf = x740_grappleArm->AuxTransform();

  if (x834_26_animPlaying) {
    auxXf = CTransform4f::Identity();
  } else {
    GetLctrWithShake(auxXf, modelData, rstl::string_l("elbow"), true, false);
  }

  const CVector3f elbowPos = auxXf * elbowOffset;
  auxXf.SetTranslation(elbowPos);
  x740_grappleArm->SetTransform(x3e8_xf);
}

void CPlayerGun::ReturnArmAndGunToDefault(CStateManager& mgr, bool returnToDefault) {
  if (returnToDefault || !x833_31_inFreeLook) {
    x73c_gunMotion->ReturnToDefault(mgr, false);
    x740_grappleArm->ReturnToDefault(mgr, 0.f, false);
  }
  if (!x834_25_gunMotionFidgeting)
    x72c_currentBeam->ReturnToDefault(mgr);
  x834_25_gunMotionFidgeting = false;
}

void CPlayerGun::UpdateAuxWeapons(float dt, const CTransform4f& targetXf, CStateManager& mgr) {
#ifdef TARGET_PC
  CTransform4f fireXf = GetGunMotionTransform() * x418_beamLocalXf;
  // The combo beams fire along targetXf; this only places their muzzle effect.
  PortShotFromDrawnMuzzle(mgr, fireXf, false);
  const CVector3f firePoint = fireXf.GetTranslation();
#else
  const CVector3f firePoint = GetGunMotionTransform() * x418_beamLocalXf.GetTranslation();
#endif
  const CVector3f camPos = mgr.GetCameraManager()->GetGlobalCameraTranslation(mgr);
  bool done = x744_auxWeapon->UpdateComboFx(dt, kScaleVector, firePoint + camPos, targetXf, mgr);
  if (IsWeaponStateSet(0x10)) {
    if (x310_currentBeam == CPlayerState::kBI_Wave &&
        x744_auxWeapon->HasTarget(mgr) == kInvalidUniqueId) {
#ifdef TARGET_PC
      // The Wavebuster re-picks its target while it fires: not the locked one while the
      // pointer is off it.
      TUniqueId targetId = PortHomingTargetId(mgr);
      if (targetId == kInvalidUniqueId && !PortShotOffLock(mgr)) {
#else
      TUniqueId targetId = GetTargetId(mgr);
      if (targetId == kInvalidUniqueId) {
#endif
        targetId = mgr.GetPlayer()->GetAimTargetId();
      }
      x744_auxWeapon->SetNewTarget(targetId, mgr);
    }
    if (done == true) {
      return;
    }

    bool comboDone =
        x310_currentBeam == CPlayerState::kBI_Wave || x310_currentBeam == CPlayerState::kBI_Plasma;
    if (comboDone != true) {
      if (x72c_currentBeam->ComboFireOver()) {
        comboDone = true;
      }
    }

    x72c_currentBeam->EnableSecondaryFx(CGunWeapon::kSFT_CancelCharge);
    if (comboDone) {
      x32c_chargePhase = kCP_ChargeDone;
      ReturnArmAndGunToDefault(mgr, false);
      ResetToBeam();
    }
  } else {
    if (x833_28_phazonBeamActive) {
      CPhazonBeam* phazonBeam = static_cast< CPhazonBeam* >(x72c_currentBeam);
      phazonBeam->UpdateBeam(dt, targetXf, x418_beamLocalXf.GetTranslation(), mgr);
    } else if (x310_currentBeam == CPlayerState::kBI_Plasma) {
      CPlasmaBeam* plasmaBeam = static_cast< CPlasmaBeam* >(x72c_currentBeam);
      plasmaBeam->UpdateBeam(dt, targetXf, x418_beamLocalXf.GetTranslation(), mgr);
    }
  }
}

void CPlayerGun::CancelLockOn() {
  if (x832_29_lockedOn) {
    x832_29_lockedOn = false;
    x6a0_motionState.SetState(CMotionState::kMS_CancelLockOn);
    if (x32c_chargePhase == kCP_NotCharging && int(x318_comboAmmoIdx) != 1) {
      PlayAnim(NWeaponTypes::kGAT_BasePosition, false);
    }
  }
}

void CPlayerGun::CreateGunLight(CStateManager& mgr) {
  if (x53c_lightId != kInvalidUniqueId) {
    return;
  }

  x53c_lightId = mgr.AllocateUniqueId();
  const uint lightId = x53c_lightId.Value();
  mgr.AddObject(rs_new CGameLight(
      x53c_lightId, kInvalidAreaId, false, rstl::string_l("GunLite"), x3e8_xf, x538_playerId,
      CLight::BuildDirectional(CVector3f::Forward(), CColor::Black()), lightId, 0, 0.f));
}

void CPlayerGun::DeleteGunLight(CStateManager& mgr) {
  if (x53c_lightId == kInvalidUniqueId) {
    return;
  }

  mgr.DeleteObjectRequest(x53c_lightId);
  x53c_lightId = kInvalidUniqueId;
}

void CPlayerGun::UpdateGunLight(const CTransform4f& xf, CStateManager& mgr) {
  if (x53c_lightId == kInvalidUniqueId) {
    return;
  }
  if (x32c_chargePhase == kCP_NotCharging) {
    return;
  }

  if (CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(x53c_lightId))) {
    if (light->GetActive()) {
      CElementGen* chargeFx = x72c_currentBeam->GetChargeMuzzleFx();
      light->SetTransform(xf);
      light->SetTranslation(xf.GetTranslation());
      if (chargeFx != NULL && chargeFx->SystemHasLight()) {
        CLight genLight = chargeFx->GetLight();
        genLight.SetColor(
            CColor::Lerp(0, genLight.GetColor().GetColor_u32(), x340_chargeBeamFactor));
#ifdef TARGET_PC
        // Remastered has no charge-beam light: its charge effects light nothing.
        if (chargeFx->PortIsRemastered()) {
          genLight.SetColor(CColor::Black());
        }
#endif
        light->SetLight(genLight);
      }
    }
  }
}

void CPlayerGun::SetGunLightActive(bool active, CStateManager& mgr) {
  if (x53c_lightId == kInvalidUniqueId) {
    return;
  }

  if (CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(x53c_lightId))) {
    light->SetActive(active);
    if (active) {
      if (CElementGen* chargeFx = x72c_currentBeam->GetChargeMuzzleFx()) {
        if (chargeFx->SystemHasLight()) {
          CLight genLight = chargeFx->GetLight();
          genLight.SetColor(CColor::Black());
          light->SetLight(genLight);
        }
      }
    }
  }
}

void CPlayerGun::LoadHandAnimTokens() {
  CAnimData& animData = *x6e0_rightHandModel.AnimationData();
  rstl::set< CPrimitive > prims;
  for (int i = 0; i < 3; ++i) {
    CAnimPlaybackParms parms(i, -1, 1.f, true);
    animData.GetAnimationPrimitives(parms, prims);
  }
  CAnimData::PrimitiveSetToTokenVector(prims, x540_handAnimTokens, true);
}

void CPlayerGun::ProcessPhazonGunMorph(float dt, CStateManager& mgr) {
  if (x835_26_phazonBeamMorphing) {
    if (x835_27_intoPhazonBeam) {
      x39c_phazonMorphT += 15.f * dt;
      if (x39c_phazonMorphT > 1.f) {
        x39c_phazonMorphT = 1.f;
      }
    } else {
      x39c_phazonMorphT -= 2.f * dt;
      if (x39c_phazonMorphT < 0.f) {
        x835_26_phazonBeamMorphing = false;
        x39c_phazonMorphT = 0.f;
      }
    }
  }

  switch (x33c_phazonBeamState) {
  case kPBS_Entering:
    if (x75c_phazonBeam.get() != NULL) {
      x75c_phazonBeam->Update(dt, mgr);
      if (x75c_phazonBeam->IsLoaded()) {
        StartPhazonBeamTransition(true, mgr, *mgr.PlayerState());
        SetPhazonBeamMorph(false);
        x33c_phazonBeamState = kPBS_Active;
        x338_nextState = kNS_SetupBeam;
      }
    }
    break;
  case kPBS_Exiting:
    if (x738_nextBeam != NULL) {
      x738_nextBeam->Update(dt, mgr);
      if (x738_nextBeam->IsLoaded()) {
        x835_25_inPhazonBeam = false;
        StartPhazonBeamTransition(false, mgr, *mgr.PlayerState());
        SetPhazonBeamMorph(false);
        x33c_phazonBeamState = kPBS_Inactive;
        x338_nextState = kNS_SetupBeam;
      }
    }
    break;
  default:
    break;
  }
}

void CPlayerGun::ProcessGunMorph(float dt, CStateManager& mgr) {
  const CGunMorph::EGunState gunState = x678_morph.GetGunState();
  const bool isUnmorphed = mgr.GetPlayer()->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed;
  CPlayerState* playerState = mgr.PlayerState();

  switch (gunState) {
  case CGunMorph::kGS_InWipeDone:
    if (x310_currentBeam != x314_nextBeam && x734_loadingBeam != NULL) {
      if (!isUnmorphed) {
        x734_loadingBeam->Touch(mgr);
      }
      if (x734_loadingBeam->IsLoaded() && x744_auxWeapon->IsLoaded()) {
        x730_outgoingBeam = x734_loadingBeam != x72c_currentBeam ? x72c_currentBeam : NULL;
        x734_loadingBeam = NULL;
        x310_currentBeam = x314_nextBeam;
        x320_currentAuxBeam = x314_nextBeam;
        x833_30_canShowAuxMuzzleEffect = true;
        x72c_currentBeam = x760_selectableBeams[x314_nextBeam];
        x738_nextBeam = x72c_currentBeam;
        x678_morph.SetWeaponChanged();
        playerState->SetCurrentBeam(x314_nextBeam);
      }
    }
    break;
  case CGunMorph::kGS_InWipe:
  case CGunMorph::kGS_OutWipe:
#ifdef TARGET_PC
    // Remastered grows its holo to 3 while wiping in and starts it 0.4 lower
    // (morph range -0.2..1.292392, retail 0.2..; kb func/gunmorph-constants).
    if (x774_holoTransitionGen->PortIsRemastered()) {
      x774_holoTransitionGen->SetGlobalScale(gunState == CGunMorph::kGS_InWipe
                                                 ? CVector3f(3.f, 3.f, 3.f)
                                                 : kScaleVector);
      x774_holoTransitionGen->SetGlobalTranslation(CVector3f(
          0.f, x678_morph.GetYLerp() - 0.4f * (1.f - x678_morph.GetTransitionFactor()), 0.f));
    } else
#endif
    {
      x774_holoTransitionGen->SetGlobalScale(kScaleVector);
      x774_holoTransitionGen->SetGlobalTranslation(CVector3f(0.f, x678_morph.GetYLerp(), 0.f));
    }
    x774_holoTransitionGen->Update(dt);
    break;
  default:
    break;
  }

  switch (x678_morph.Update(0.2f, 1.292392f, dt)) {
  case CGunMorph::kME_InWipeDone:
    CSfxManager::SfxStart(SFXwpn_morph_in_wipe_done, 0x7f, 0x40, true, CSfxManager::kMaxPriority,
                          false, CSfxManager::kAllAreas);
    break;
  case CGunMorph::kME_OutWipeDone:
    if (x730_outgoingBeam != NULL) {
      if (x730_outgoingBeam != x72c_currentBeam) {
        x730_outgoingBeam->Unload(mgr);
        x730_outgoingBeam = NULL;
      }
    }
    if (isUnmorphed) {
      const ushort* const intoBeamSounds = mIntoBeamSound;
      const short pan = 0x4a;
      NWeaponTypes::play_sfx(intoBeamSounds[x310_currentBeam], x834_27_underwater, false, pan);
    }
    x72c_currentBeam->x1bc_rainSplashGenerator = x748_rainSplashGenerator.get();
    x72c_currentBeam->EnableFx(true);
    PlayAnim(NWeaponTypes::kGAT_ToBeam, false);
    if (x833_31_inFreeLook) {
      EnterFreeLook(mgr);
    } else if (x832_30_requestReturnToDefault) {
      ReturnArmAndGunToDefault(mgr, false);
    }
    if (x834_28_requestImmediateRecharge || (x2ec_lastFireButtonStates & 0x1) != 0) {
      if (playerState->GetCurrentVisor() != CPlayerState::kPV_Scan) {
        x32c_chargePhase = kCP_ChargeRequested;
#ifdef TARGET_PC
        PortStartRapidCharge();
#endif
      }
      x834_28_requestImmediateRecharge = false;
    }
    x832_30_requestReturnToDefault = false;
    x338_nextState = kNS_SetupBeam;
    break;
  default:
    break;
  }
}

void CPlayerGun::AsyncLoadFidget(CStateManager& mgr) {
  SamusGun::EFidgetType type = x3a4_fidget.GetType();
  int animSet = x3a4_fidget.GetAnimSet();
  SamusGun::EFidgetType beamType = type;
  bool beamOnly = x3a4_fidget.GetState() == CFidget::kS_HolsterBeam;

  SetFidgetAnimBits(animSet, beamOnly);

  if ((x2fc_fidgetAnimBits & 0x1) == 0x1) {
    x73c_gunMotion->GunController().LoadFidgetAnimAsync(mgr, type, x310_currentBeam, animSet);
  }

  if ((x2fc_fidgetAnimBits & 0x2) == 0x2) {
    x72c_currentBeam->AsyncLoadFidget(mgr, beamOnly ? SamusGun::kFT_Minor : beamType, animSet);
    x832_31_inRestPose = false;
  }

  if ((x2fc_fidgetAnimBits & 0x4) == 0x4) {
    if (CGunController* gc = x740_grappleArm->GunController()) {
      gc->LoadFidgetAnimAsync(mgr, type, type != SamusGun::kFT_Minor ? x310_currentBeam : 0,
                              animSet);
    }
  }
}

void CPlayerGun::UnLoadFidget() {
  if ((x2fc_fidgetAnimBits & 0x1) == 0x1)
    x73c_gunMotion->GunController().UnLoadFidget();
  if ((x2fc_fidgetAnimBits & 0x2) == 0x2)
    x72c_currentBeam->UnLoadFidget();
  if ((x2fc_fidgetAnimBits & 0x4) == 0x4)
    if (CGunController* gc = x740_grappleArm->GunController())
      gc->UnLoadFidget();
  x2fc_fidgetAnimBits = 0;
}

bool CPlayerGun::IsFidgetLoaded() {
  uint loadFlags = 0;
  if ((x2fc_fidgetAnimBits & 0x1) == 0x1 && x73c_gunMotion->GunController().IsFidgetLoaded()) {
    loadFlags |= 0x1;
  }
  if ((x2fc_fidgetAnimBits & 0x2) == 0x2 && x72c_currentBeam->IsFidgetLoaded()) {
    loadFlags |= 0x2;
  }
  if ((x2fc_fidgetAnimBits & 0x4) == 0x4) {
    if (CGunController* gc = x740_grappleArm->GunController()) {
      if (gc->IsFidgetLoaded()) {
        loadFlags |= 0x4;
      }
    }
  }
  return loadFlags == x2fc_fidgetAnimBits;
}

void CPlayerGun::SetFidgetAnimBits(int animSet, bool beamOnly) {
  x2fc_fidgetAnimBits = 0;
  if (beamOnly) {
    x2fc_fidgetAnimBits = 2;
    return;
  }

  switch (x3a4_fidget.GetType()) {
  case SamusGun::kFT_Minor:
    x2fc_fidgetAnimBits = 1;
    if (animSet <= 0) {
      return;
    }
    if (animSet >= 2) {
      return;
    }
    x2fc_fidgetAnimBits |= 4;
    return;
  case SamusGun::kFT_Major:
    if (animSet >= 6 || animSet < 4) {
      x2fc_fidgetAnimBits = 2;
    } else {
      x2fc_fidgetAnimBits = 1;
    }
    x2fc_fidgetAnimBits |= 4;
    return;
  default:
    return;
  }
}

void CPlayerGun::AsyncLoadSuit(CStateManager& mgr) {
  x72c_currentBeam->AsyncLoadSuitArm(mgr);
  x740_grappleArm->AsyncLoadSuit(mgr);
}

void CPlayerGun::ReturnToRestPose() {
  if (x832_31_inRestPose == true) {
    return;
  }

  if (IsWeaponStateSet(0x1)) {
    PlayAnim(NWeaponTypes::kGAT_BasePosition, false);
  } else if (IsWeaponStateSet(0x4)) {
    PlayAnim(NWeaponTypes::kGAT_ToMissile, false);
  }

  x832_31_inRestPose = true;
}

TUniqueId CPlayerGun::DropPowerBomb(CStateManager& mgr) const {
  const CDamageInfo dInfo = mgr.GetPlayer()->GetDeathTime() <= 0.f
                                ? gpTweakPlayerGun->x8c_powerBomb
                                : CDamageInfo(CWeaponMode::PowerBomb(), 0.f, 0.f, 0.f);
  const float ballHalfExtent = gpTweakPlayer->GetPlayerBallHalfExtent();

  TUniqueId uid = mgr.AllocateUniqueId();
  float zero = 0.f;
  const bool remastered = x784_bombEffects[1].size() > 2;
  if (remastered) {
    // Remastered spawns PBSecondaryVFX at the bomb before the bomb itself.
    const CTransform4f bombXf = CTransform4f::Translate(
        mgr.GetPlayer()->GetTranslation() + CVector3f(zero, zero, ballHalfExtent));
    CExplosion* secondary = rs_new CExplosion(
        x784_bombEffects[1][2], mgr.AllocateUniqueId(), true,
        CEntityInfo(kInvalidAreaId, CEntity::NullConnectionList), rstl::string_l("PBSecondaryVFX"), bombXf,
        0, CVector3f(1.f, 1.f, 1.f), CColor::White());
    mgr.AddObject(secondary);
  }
  CPowerBomb* pBomb =
      rs_new CPowerBomb(x784_bombEffects[1][0], uid, kInvalidAreaId, x538_playerId,
                        CTransform4f::Translate(mgr.GetPlayer()->GetTranslation() +
                                                CVector3f(zero, zero, ballHalfExtent)),
                        dInfo);
  if (remastered) {
    pBomb->SetRemasteredFilter();
  }
  mgr.AddObject(*pBomb);
  return uid;
}

void CPlayerGun::SetPhazonBeamFeedback(bool active) {
  const bool fadeOut = !active;
  CSamusHud::DisplayHudMemo(rstl::wstring_l(gpStringTable->GetString(21)),
                            CHUDMemoParms(5.f, true, fadeOut, false));

  if (CSfxManager::IsPlaying(x2e8_phazonBeamSfx)) {
    CSfxManager::SfxStop(x2e8_phazonBeamSfx);
  }

  x2e8_phazonBeamSfx.Clear();
  if (active) {
    x2e8_phazonBeamSfx =
        NWeaponTypes::play_sfx(SFXsam_a_phazup_lp_00, x834_27_underwater, false, 0x4a);
  }
}

#if VERSION >= VERSION_GM8P_00
void CPlayerGun::SetBeam(CPlayerState::EItemType item, CStateManager& mgr) {
  CPlayerState::EBeamId beam = CPlayerState::kBI_Power;
  switch (item) {
  case CPlayerState::kIT_IceBeam:
    beam = CPlayerState::kBI_Ice;
    break;
  case CPlayerState::kIT_WaveBeam:
    beam = CPlayerState::kBI_Wave;
    break;
  case CPlayerState::kIT_PlasmaBeam:
    beam = CPlayerState::kBI_Plasma;
    break;
  }

  if (mgr.GetPlayerState()->HasPowerUp(item) && beam != x310_currentBeam) {
    x72c_currentBeam->Unload(mgr);
    mgr.PlayerState()->SetCurrentBeam(beam);
    x678_morph =
        CGunMorph(gpTweakPlayerGun->GetGunTransformTime(), gpTweakPlayerGun->GetHoloHoldTime());
    LoadBeam(beam, mgr);
  }
}

void CPlayerGun::LoadBeam(CPlayerState::EBeamId beamId, CStateManager& mgr) {
  const CPlayerState::EBeamId& beam = beamId;
  x310_currentBeam = x314_nextBeam = x320_currentAuxBeam = mCurrentBeamId[beam];
  x72c_currentBeam = x760_selectableBeams[beam];
  x738_nextBeam = x72c_currentBeam;
  x338_nextState = kNS_StatusQuo;
  x72c_currentBeam->Load(mgr, true);
  x72c_currentBeam->x1bc_rainSplashGenerator = x748_rainSplashGenerator.get();
  x744_auxWeapon->Load(beam, mgr);
  DisableWeaponState(0x8);
  ResetToBeam();
}
#endif
