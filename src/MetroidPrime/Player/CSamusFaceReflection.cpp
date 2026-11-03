#include "MetroidPrime/Player/CSamusFaceReflection.hpp"

#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CAnimRes.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Tweaks/CTweakGui.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CRelAngle.hpp"

#include "port_debug.h"

#ifdef TARGET_PC
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleGen.hpp"
#include "MetroidPrime/CExplosion.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "rstl/math.hpp"

#include "port_mods.h"
#include "port_room_env.h"
#endif

static const char* const skFaceAssetIdName = "ACS_SamusFace";
static const CTransform4f skFaceModelViewAdjust =
    CTransform4f::Scale(0.3f) * CTransform4f::Translate(CVector3f(0.f, 0.5f, 0.f));

CSamusFaceReflection::CSamusFaceReflection(const CStateManager& mgr)
: x0_modelData(
      CModelData(CAnimRes(gpResourceFactory->GetResourceIdByName(skFaceAssetIdName)->GetId(),
                          CAnimRes::kDefaultCharIdx, CVector3f(1.f, 1.f, 1.f), 0, true)))
, x4c_lights(rs_new CActorLights(8, CVector3f::Zero(), 4, 4))
, x50_lookRot(CQuaternion::NoRotation())
, x60_lookDir(CVector3f::Forward())
, x6c_(0)
, x70_hidden(true) {
  CAnimPlaybackParms parms(0, -1, 1.f, true);
  x0_modelData.AnimationData()->SetAnimation(parms, false);
}

static inline float FaceLookBlend(float dt, float lookDot, float freeLookSpeed) {
  const float lookAng = acosf(CMath::Limit(lookDot, 1.f));
  const float f = lookAng > 0.f ? freeLookSpeed / lookAng : 0.f;
  return CMath::Clamp(0.f, dt * 18.f * f, 1.f);
}

void CSamusFaceReflection::Update(float dt, const CStateManager& mgr, CRandom16& rand) {
  if (const CFirstPersonCamera* const fpCam =
          TCastToConstPtr< CFirstPersonCamera >(mgr.GetCameraManager()->GetCurrentCamera(mgr))) {
    CVector3f camTrans = fpCam->GetTranslation();
    x0_modelData.AdvanceAnimationIgnoreParticles(dt, rand, true);

    CActorLights& lights = *x4c_lights;
    lights.SetFindShadowLight(false);

    TAreaId areaId = mgr.GetPlayer()->GetCurrentAreaId();
    if (areaId == kInvalidAreaId)
      return;

    const CVector3f offset(0.125f, 0.125f, 0.125f);
    CAABox aabb(camTrans - offset, camTrans + offset);

    const CGameArea& area = mgr.GetWorld()->GetAreaAlways(areaId);
    lights.BuildFaceLightList(mgr, area, aabb);

    const CMatrix3f matrix = fpCam->GetTransform().BuildMatrix3f();
#ifdef TARGET_PC
    if (PortRoomEnv::Enabled() && PortMods::RoomEnvLoaded()) {
      // Remastered (Update 0xe7be44) keeps the face facing the camera and swaps between
      // three eye anims instead of turning the head.
      const CVector3f right = matrix.GetColumn(0);
      const CUnitVector3f up(matrix.GetColumn(2));
      const float lean =
          (xPortFaceUp.GetY() * -right.GetY() - right.GetX() * xPortFaceUp.GetX()) -
          right.GetZ() * xPortFaceUp.GetZ();
      // 0.5f is CTweakGuiMP1+0x154 (no LDTA override).
      const float thresh = sinf(gpTweakPlayer->GetFreeLookSpeed() * 0.5f * dt);
      xPortFaceUp = up;
      static const char* const skEyeAnims[] = {"B_ready_face", "B_eyesleft_face",
                                               "B_eyesright_face"};
      const int idx = fabsf(lean) < thresh ? 0 : (lean >= 0.f ? 2 : 1);
      CAnimData* const anim = x0_modelData.AnimationData();
      const int animIdx =
          anim->GetCharacterInfo().GetAnimationIndex(rstl::string_l(skEyeAnims[idx]));
      if (animIdx != -1 && anim->GetCurrentAnimation() != animIdx) {
        anim->SetAnimation(CAnimPlaybackParms(animIdx, -1, 1.f, true), false);
      }
      x50_lookRot = CQuaternion::NoRotation();
      x60_lookDir = matrix.GetColumn(1);
      return;
    }
    // Back from Remastered: retail only ever plays the ctor's anim 0.
    if (x0_modelData.GetAnimationData()->GetCurrentAnimation() != 0) {
      x0_modelData.AnimationData()->SetAnimation(CAnimPlaybackParms(0, -1, 1.f, true), false);
    }
#endif
    const CUnitVector3f lookDir(matrix.GetColumn(1));

    CQuaternion xfLook1 =
        CQuaternion::LookAt(CUnitVector3f(lookDir), CUnitVector3f(CVector3f::Forward()),
                            CRelAngle::FromRadians(M_2PIF));
    CQuaternion xfLook2 =
        CQuaternion::LookAt(CUnitVector3f(CVector3f::Forward()), xfLook1.Transform(x60_lookDir),
                            CRelAngle::FromRadians(M_2PIF));
    xfLook2 *= xfLook2;

    const CVector3f lookCenter = xfLook2.BuildTransform().GetColumn(1);
    const CVector3f lookRotCenter = x50_lookRot.BuildTransform().GetColumn(1);
    const float blend = FaceLookBlend(dt, CVector3f::Dot(lookRotCenter, lookCenter),
                                     dt * gpTweakPlayer->GetFreeLookSpeed() * 0.5f);
    xfLook2 = CQuaternion::SlerpLocal(x50_lookRot, xfLook2, blend);
    x50_lookRot = xfLook2;
    x60_lookDir = lookDir;
  }
}

#ifdef TARGET_PC
// CVector3f::IsAngleBetweenLessThan 0x6b378: compares signed squares, so no square roots.
static bool PortIsAngleBetweenLessThan(const CVector3f& a, const CVector3f& b, float ang) {
  if (ang > M_PIF) {
    return true;
  }
  const float d = CVector3f::Dot(a, b);
  const float c = CMath::FastCosR(ang);
  const float dSq = d >= 0.f ? d * d : -(d * d);
  const float cSq = c >= 0.f ? c * c : -(c * c);
  return dSq > cSq * a.MagSquared() * b.MagSquared();
}

// CSamusFaceReflectionMP1::PreRender 0xe7c044: each FRMD explosion in front of the camera adds
// its modulation colour, weighted by its mode's curve at the explosion's age.
static bool PortFaceReflectColor(const CStateManager& mgr, const CFirstPersonCamera& fpCam,
                                 CColor& out) {
  const PortFaceReflection::State& state = mgr.PortFaceReflectionManager();
  const float alphaMod = PortFaceReflection::AlphaModifier(state);
  if (alphaMod == 0.f || state.explosions.empty()) {
    return false;
  }
  const CVector3f camPos = fpCam.GetTransform().GetTranslation();
  const CVector3f camFwd = fpCam.GetTransform().GetColumn(kDY);
  bool visible = false;
  float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
  for (const TUniqueId id : state.explosions) {
    const CExplosion* const explosion = TCastToConstPtr< CExplosion >(mgr.GetObjectById(id));
    if (!explosion || !explosion->xf4_25_hasRenderBounds) {
      continue;
    }
    const CGenDescription* const desc = explosion->PortFaceReflectDesc();
    if (!desc ||
        !PortIsAngleBetweenLessThan(
            camFwd, explosion->GetRenderBoundsCached().GetCenterPoint() - camPos, 0.9599f)) {
      continue;
    }
    const float w = PortFaceReflection::Weight(desc->xPortFrmd, explosion->xf8_time);
    if (w > 0.f) {
      const CColor& color = explosion->xe8_particleGen->GetModulationColor();
      visible = true;
      r += color.GetRed() * w;
      g += color.GetGreen() * w;
      b += color.GetBlue() * w;
      a += color.GetAlpha() * w;
    }
  }
  if (visible) {
    out = CColor(rstl::min_val(r, 1.f), rstl::min_val(g, 1.f), rstl::min_val(b, 1.f),
                 alphaMod * mgr.GetPlayerState()->GetVisorTransitionFactor() *
                     rstl::min_val(a, 1.f));
  }
  return visible;
}
#endif

void CSamusFaceReflection::PreDraw(const CStateManager& mgr) {
  const CFirstPersonCamera* const fpCam =
      TCastToConstPtr< CFirstPersonCamera >(mgr.GetCameraManager()->GetCurrentCamera(mgr));
  if ((x6c_ == 2 || (x4c_lights->GetActiveLightCount() < 1 && (x6c_ == 0 || x6c_ == 3))) ||
      !fpCam) {
    x70_hidden = true;
  } else {
    x70_hidden = false;
  }
#ifdef TARGET_PC
  xPortHidden = !fpCam || !PortFaceReflectColor(mgr, *fpCam, xPortColor);
  if (!x70_hidden || !xPortHidden) {
    x0_modelData.AnimationData()->PreRender();
  }
#else
  if (!x70_hidden) {
    x0_modelData.AnimationData()->PreRender();
  }
#endif
}

void CSamusFaceReflection::Draw(const CStateManager& mgr) const {
#ifdef TARGET_PC
  if (x70_hidden && xPortHidden)
    return;
#else
  if (x70_hidden)
    return;
#endif

  if (const CFirstPersonCamera* fpCam =
          TCastToConstPtr< CFirstPersonCamera >(mgr.GetCameraManager()->GetCurrentCamera(mgr))) {

    const CVector3f camTranslation = fpCam->GetTranslation();
    const CVector3f camYcol = fpCam->GetTransform().GetColumn(kDY);
    const CVector3f camZcol = fpCam->GetTransform().GetColumn(kDZ);

    CQuaternion camRot = CQuaternion::FromMatrix(fpCam->GetTransform());

    float dist = CTweakGui::FaceReflectionDistanceDebugValueToActualValue(
        gpTweakGui->GetFaceReflectionDistance());
    float height = CTweakGui::FaceReflectionHeightDebugValueToActualValue(
        gpTweakGui->GetFaceReflectionHeight());
    float aspect = CTweakGui::FaceReflectionAspectDebugValueToActualValue(
        gpTweakGui->GetFaceReflectionAspect());
    float orthoWidth = CTweakGui::FaceReflectionOrthoWidthDebugValueToActualValue(
        gpTweakGui->GetFaceReflectionOrthoWidth());
    float orthoHeight = CTweakGui::FaceReflectionOrthoHeightDebugValueToActualValue(
        gpTweakGui->GetFaceReflectionOrthoHeight());

    CTransform4f modelXf = CTransform4f((camRot * x50_lookRot).BuildTransform(),
                                        camTranslation + (dist * camYcol) + (height * camZcol)) *
                           skFaceModelViewAdjust;

    CGraphics::SetViewPointMatrix(fpCam->GetTransform());
    // Port: the ortho spans the whole viewport; widen it with the viewport so
    // the face keeps its shape in widescreen. 1 at 4:3.
    const CViewport& vp = CGraphics::GetViewport();
    const float wide = static_cast< float >(vp.mWidth) /
                       static_cast< float >(PortDebug::FourThreeWidth(vp.mWidth, vp.mHeight));
    CGraphics::SetOrtho(wide * aspect * -orthoWidth, wide * aspect * orthoWidth, orthoHeight,
                        -orthoHeight, -10.f, 10.f);

#ifdef TARGET_PC
    // Remastered Draw 0xe7c318: combat visor only, one unlit additive pass that writes depth.
    if (!xPortHidden && mgr.GetPlayerState()->GetActiveVisor(mgr) == CPlayerState::kPV_Combat) {
      x0_modelData.Render(mgr, modelXf, nullptr,
                          CModelFlags::Additive(xPortColor).DepthCompareUpdate(true, true));
    }
    if (x70_hidden)
      return;
#endif
    CActorLights* lights = x6c_ == 1 ? nullptr : x4c_lights.get();
    if (x6c_ == 3) {
      x0_modelData.Render(mgr, modelXf, lights, CModelFlags::Normal());
    } else {
      float transFactor = mgr.GetPlayerState()->GetActiveVisor(mgr) == CPlayerState::kPV_Combat
                              ? mgr.GetPlayerState()->GetVisorTransitionFactor()
                              : 0.f;
      if (transFactor > 0.f) {
        x0_modelData.Render(mgr, modelXf, nullptr,
                            CModelFlags::Additive(CColor::Black()).DepthCompareUpdate(true, true));
        x0_modelData.Render(mgr, modelXf, lights,
                            CModelFlags::Additive(transFactor).DepthCompareUpdate(true, false));
      }
    }
  }
}
