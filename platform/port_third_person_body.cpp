// The third-person camera's animated body; see port_third_person_body.h.
#include "port_third_person_body.h"

#include <algorithm>
#include <cmath>

#include "port_dread_samus_mod.h"
#include "port_third_person.h"

#include "Kyoto/Animation/CCharLayoutInfo.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CAnimRes.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Factories/CCharacterFactory.hpp"
#include "MetroidPrime/Factories/CCharacterFactoryBuilder.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayerRes.hpp"

#include <dolphin/gx.h>

#include <memory>

namespace PortThirdPersonBody {
namespace {

// The cutscene Samus ANCS that carry the locomotion clips, best first. C2E1F513 has them all
// (it is in the Metroid1-6 paks); the rest only the ready pose, so the body stands in them.
constexpr uint32_t kAncsCandidates[] = {0xC2E1F513, 0x1413466E, 0x27550808,
                                        0x0C6B791C, 0x58673D8D, 0xC2BF3133};
// The suit CScriptPlayerActor draws with the Phazon indirect-texture pass.
constexpr int kPhazonChar = 3;
// The scale the game's own CScriptPlayerActor cutscene Samus uses in its SCLY (1.799, measured; the
// clip is 1.756 m tall at scale 1). The suit and cannon are scaled alike, so the locators follow.
constexpr float kBodyScale = 1.799f;

struct ClipNames {
  const char* a;
  const char* b;
};
constexpr ClipNames kIdle{"B_ready_samus", "B_rready_samus"};
constexpr ClipNames kSlowWalk{"B_slowwalk_samus", "B_walk_samus"};
constexpr ClipNames kWalk{"B_walk_samus", "B_slowwalk_samus"};
constexpr ClipNames kRun{"B_slowrunloop_samus", "B_walk_samus"};
constexpr ClipNames kWalkGun{"B_walkgun_samus", "B_walk_samus"};
constexpr ClipNames kStop{"B_intorreadyfromwalk_samus", "B_rready_samus"};

struct State {
  // Loading (as CScriptPlayerActor::LoadSuit).
  int wantChar = -1;
  int loadedChar = -1;
  uint32_t ancs = 0;
  std::unique_ptr<TCachedToken<CModel>> suitModel;
  std::unique_ptr<TToken<CSkinRules>> suitSkin;
  // Built.
  std::unique_ptr<CModelData> body;
  int idx[int(Clip::Count)] = {}; // by Clip
  Clip current = Clip::Hold;
  float phase = 0.f; // Backward: where the walk clip is, 0..1, run in reverse by hand
  float stopLeft = 0.f; // Stop: seconds of the stop clip still to play
  float gunUp = 0.f;    // seconds the cannon stays raised (NoteShot)
  bool animating = false;
  // Aiming the cannon: the shoulder that turns, the locators that give the cannon's axis.
  CSegId shoulder = CSegId::Invalid();
  CSegId gunSeg = CSegId::Invalid();
  CSegId muzzleSeg = CSegId::Invalid();
  CQuaternion shoulderPose = CQuaternion::NoRotation(); // the clip's, before the aim
  bool advanced = false; // the clip advanced since the last PreRender (shoulderPose is stale)
  bool haveAim = false;
  CVector3f aimPoint = CVector3f::Zero();
  float aimWeight = 0.f;
  // The Phazon suit's indirect-texture pass (as CScriptPlayerActor's).
  rstl::optional_object<TCachedToken<CTexture>> phazonTex;
  float phazonAngle = 0.f;
  // The cannon.
  int beam = -1;
  std::unique_ptr<TToken<CModel>> beamModel;
  std::unique_ptr<CModelData> beamData;
};
State s;

int CharIdx(const CStateManager& mgr) {
  const CPlayerState::EPlayerSuit suit = mgr.GetPlayerState()->GetCurrentSuitRaw();
  int charIdx = suit;
  if (mgr.GetPlayerState()->GetIsFusionEnabled()) {
    switch (suit) {
    case CPlayerState::kPS_Power:
      charIdx = 4;
      break;
    case CPlayerState::kPS_Varia:
      charIdx = 7;
      break;
    case CPlayerState::kPS_Gravity:
      charIdx = 6;
      break;
    case CPlayerState::kPS_Phazon:
      charIdx = 8;
      break;
    default:
      break;
    }
  }
  return charIdx;
}

void Drop() {
  s.suitModel.reset();
  s.suitSkin.reset();
  s.body.reset();
  s.beamData.reset();
  s.beamModel.reset();
  s.phazonTex = rstl::optional_object_null();
  s.wantChar = s.loadedChar = s.beam = -1;
  s.ancs = 0;
  s.animating = false;
}

uint32_t PickAncs() {
  for (uint32_t id : kAncsCandidates) {
    if (gpSimplePool->HasObject(SObjectTag('ANCS', id))) {
      return id;
    }
  }
  return 0;
}

void StartLoad(int charIdx) {
  Drop();
  s.wantChar = charIdx;
  s.ancs = PickAncs();
  if (s.ancs == 0) {
    return;
  }
  const CAnimRes res(s.ancs, charIdx, CVector3f(kBodyScale, kBodyScale, kBodyScale), 0, true);
  TLockedToken<CCharacterFactory> factory = gpCharacterFactoryBuilder->GetFactory(res);
  const CCharacterInfo& info = factory->GetCharInfo(charIdx);
  s.suitModel.reset(
      new TCachedToken<CModel>(gpSimplePool->GetObj(SObjectTag('CMDL', info.GetModelId()))));
  s.suitModel->Lock();
  s.suitSkin.reset(
      new TToken<CSkinRules>(gpSimplePool->GetObj(SObjectTag('CSKR', info.GetSkinRulesId()))));
  s.suitSkin->Lock();
  if (charIdx == kPhazonChar) {
    s.phazonTex = TCachedToken<CTexture>(gpSimplePool->GetObj("PhazonIndirectTexture"));
    s.phazonTex->Lock();
  }
  s.loadedChar = charIdx;
}

int Find(const CAnimData& data, const ClipNames& n) {
  int i = data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(n.a));
  if (i < 0) {
    i = data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(n.b));
  }
  return i;
}

bool IsAncestor(const CCharLayoutInfo& layout, CSegId ancestor, CSegId seg) {
  for (int depth = 0; depth < 64 && layout.ContainsSegment(seg); ++depth) {
    seg = layout.GetOriginalParent(seg);
    if (seg == ancestor) {
      return true;
    }
  }
  return false;
}

// The cannon arm: the shoulder above GUN_LCTR (the cannon's mount), Charge_LCTR its muzzle.
void FindAimBones(const CAnimData& data) {
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  s.shoulder = CSegId::Invalid();
  s.gunSeg = layout.GetSegIdFromString(rstl::string_l("GUN_LCTR"));
  s.muzzleSeg = layout.GetSegIdFromString(rstl::string_l("Charge_LCTR"));
  if (s.gunSeg == CSegId::Invalid() || s.muzzleSeg == CSegId::Invalid()) {
    return;
  }
  for (const char* name : {"R_shoulder", "L_shoulder"}) {
    const CSegId seg = layout.GetSegIdFromString(rstl::string_l(name));
    if (seg != CSegId::Invalid() && IsAncestor(layout, seg, s.gunSeg) &&
        IsAncestor(layout, seg, s.muzzleSeg)) {
      s.shoulder = seg;
      return;
    }
  }
}

// Turns the shoulder so the cannon's axis (GUN_LCTR to Charge_LCTR) runs through the aim point,
// by s.aimWeight, over the clip's pose. All in the body's unscaled model space.
void AimArm(const CStateManager& mgr, CAnimData& data) {
  if (s.shoulder == CSegId::Invalid()) {
    return;
  }
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  if (s.advanced) {
    s.shoulderPose = pb.GetSegRotation(s.shoulder);
    s.advanced = false;
  }
  pb.Insert(s.shoulder, s.shoulderPose);
  data.SetPoseBuilderValid(false);
  if (s.aimWeight <= 0.f) {
    return;
  }
  // The body's transform, as CPlayer::Render places it.
  const CPlayer& player = *mgr.GetPlayer();
  CTransform4f bodyXf = player.GetTransform();
  CVector3f origin = bodyXf.GetTranslation();
  origin.SetZ(player.GetBoundingBox().GetMinPoint().GetZ());
  bodyXf.SetTranslation(origin);
  const CVector3f target = bodyXf.TransposeMultiply(s.aimPoint) * (1.f / kBodyScale);

  const auto position = [&pb](CSegId seg) {
    CTransform4f xf = CTransform4f::Identity();
    pb.BuildTransform(seg, xf);
    return xf.GetTranslation();
  };
  const CVector3f pivot = position(s.shoulder);
  const CVector3f gun = position(s.gunSeg) - pivot;
  const CVector3f muzzle = position(s.muzzleSeg) - pivot;
  if (!(muzzle - gun).CanBeNormalized() || (target - pivot).Magnitude() < 1.f) {
    return;
  }
  // Turning about the shoulder moves the muzzle too: a few passes settle on the line.
  CQuaternion turn = CQuaternion::NoRotation();
  for (int i = 0; i < 3; ++i) {
    const CVector3f axis = turn.Transform(muzzle - gun).AsNormalized();
    const CVector3f want = target - (pivot + turn.Transform(muzzle));
    if (!want.CanBeNormalized()) {
      return;
    }
    turn = CQuaternion::ShortestRotationArc(axis, want.AsNormalized()) * turn;
  }
  const float angle = 2.f * std::acos(std::min(std::fabs(turn.GetScalar()), 1.f));
  float weight = s.aimWeight;
  if (angle > kAimMaxRadians) {
    weight *= kAimMaxRadians / angle;
  }
  turn = CQuaternion::Slerp(CQuaternion::NoRotation(), turn, weight);
  // The turn is in model space; the shoulder's rotation is local to its parent's.
  // (Composed as BuildTransform composes them, root first.)
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  CQuaternion parentRot = CQuaternion::NoRotation();
  for (CSegId seg = layout.GetOriginalParent(s.shoulder); seg != CSegId::Character();
       seg = layout.GetOriginalParent(seg)) {
    parentRot = pb.GetSegRotation(seg) * parentRot;
  }
  pb.Insert(s.shoulder, parentRot.BuildInverted() * turn * parentRot * s.shoulderPose);
}

void Build(CStateManager& mgr) {
  const CAnimRes res(s.ancs, s.loadedChar, CVector3f(kBodyScale, kBodyScale, kBodyScale), 0, true);
  s.body.reset(new CModelData(res));
  CAnimData* data = s.body->AnimationData();
  s.idx[int(Clip::Idle)] = s.idx[int(Clip::Hold)] = Find(*data, kIdle);
  s.idx[int(Clip::SlowWalk)] = Find(*data, kSlowWalk);
  s.idx[int(Clip::Walk)] = s.idx[int(Clip::Backward)] = Find(*data, kWalk);
  s.idx[int(Clip::Run)] = Find(*data, kRun);
  s.idx[int(Clip::WalkGun)] = Find(*data, kWalkGun);
  s.idx[int(Clip::Stop)] = Find(*data, kStop);
  for (int& i : s.idx) {
    if (i < 0) {
      i = s.idx[int(Clip::Idle)];
    }
  }
  FindAimBones(*data);
  s.animating = s.idx[int(Clip::Idle)] >= 0;
  if (s.animating) {
    data->SetAnimation(CAnimPlaybackParms(s.idx[int(Clip::Idle)], -1, 1.f, true), false);
    s.current = Clip::Idle;
  }
  s.suitModel.reset();
  s.suitSkin.reset();
}

void PumpBeam(const CStateManager& mgr) {
  const int beam = mgr.GetPlayerState()->GetCurrentBeam();
  if (beam != s.beam) {
    s.beam = beam;
    s.beamData.reset();
    s.beamModel.reset();
    const CAssetId id = gpTweakPlayerRes->GetCinematicBeamResId(CPlayerState::EBeamId(beam));
    if (gpSimplePool->HasObject(SObjectTag('CMDL', id))) {
      s.beamModel.reset(new TToken<CModel>(gpSimplePool->GetObj(SObjectTag('CMDL', id))));
      s.beamModel->Lock();
      s.beamData.reset();
    }
  }
  if (s.beamModel && !s.beamData && s.beamModel->IsLoaded()) {
    // The id is the one the token was made from.
    const CAssetId id = gpTweakPlayerRes->GetCinematicBeamResId(CPlayerState::EBeamId(s.beam));
    s.beamData.reset(new CModelData(CStaticRes(id, CVector3f(kBodyScale, kBodyScale, kBodyScale))));
  }
}

} // namespace

void Update(CStateManager& mgr, float dt) {
  const int charIdx = CharIdx(mgr);
  if (charIdx != s.wantChar) {
    StartLoad(charIdx);
  }
  if (s.suitModel && s.suitModel->TryCache() && s.suitSkin->IsLoaded() &&
      s.suitModel->IsLoaded()) {
    s.suitModel->GetObject()->Touch(0);
    Build(mgr);
  }
  if (!s.body || !s.animating) {
    return;
  }
  PumpBeam(mgr);

  const CPlayer& player = *mgr.GetPlayer();
  const CTransform4f& xf = player.GetTransform();
  const CVector3f vel = player.GetVelocityWR();
  const CVector3f fwd = xf.GetForward();
  const CVector3f right = xf.GetRight();
  const bool grounded = player.GetPlayerMovementState() == NPlayer::kMS_OnGround;
  s.gunUp = std::max(s.gunUp - dt, 0.f);
  const bool gunUp = s.gunUp > 0.f || player.GetPlayerGun()->IsCharging();
  const Choice c =
      Settle(ChooseClip(CVector3f::Dot(vel, fwd), CVector3f::Dot(vel, right), grounded, gunUp),
             s.current, s.stopLeft <= 0.f);
  CAnimData* data = s.body->AnimationData();
  if (c.clip != s.current) {
    // Backward is posed by phase at rate 0, which would never advance a cross-fade: it snaps.
    const bool blend = c.clip != Clip::Backward && s.current != Clip::Backward;
    data->PortSetAnimationBlended(CAnimPlaybackParms(s.idx[int(c.clip)], -1, 1.f, true),
                                  blend ? kBlendSeconds : 0.f);
    if (c.clip == Clip::Stop) {
      s.stopLeft = data->GetAnimationDuration(s.idx[int(Clip::Stop)]);
    }
    s.current = c.clip;
  }
  s.stopLeft -= dt;
  if (s.phazonTex) {
    s.phazonTex->TryCache();
    s.phazonAngle = CMath::ClampRadians(s.phazonAngle + 0.03f);
  }
  if (c.clip == Clip::Backward) {
    // The animation tree only advances (a negative rate would stall it), so step the clip's phase
    // backward and let a zero-length advance rebuild the pose.
    const float duration = std::max(data->GetAnimationDuration(s.idx[int(c.clip)]), 0.1f);
    s.phase -= dt * c.rate / duration;
    s.phase -= std::floor(s.phase);
    data->SetPhase(s.phase);
    data->SetPlaybackRate(0.f);
  } else {
    data->SetPlaybackRate(c.rate);
  }
  s.body->AdvanceAnimation(dt, mgr, player.GetCurrentAreaId(), true);
  s.advanced = true;

  // The aim point is the one the shots head for (CPlayerGun::PortShotFromThirdPersonCannon).
  s.haveAim = false;
  if (gunUp) {
    if (player.GetOrbitTargetId() != kInvalidUniqueId &&
        TCastToConstPtr< CActor >(mgr.GetObjectById(player.GetOrbitTargetId())) != nullptr) {
      s.aimPoint = player.GetAssistedTargetAim();
      s.haveAim = true;
    } else {
      s.haveAim = PortThirdPerson::AimPoint(s.aimPoint);
    }
  }
  s.aimWeight = s.haveAim ? std::min(s.aimWeight + dt / kAimInSeconds, 1.f)
                          : std::max(s.aimWeight - dt / kAimOutSeconds, 0.f);
}

bool Ready() { return s.body && s.animating; }

void NoteShot() { s.gunUp = kGunUpSeconds; }

void PreRender(const CStateManager& mgr) {
  if (Ready()) {
    s.body->AnimationData()->PreRender();
    AimArm(mgr, *s.body->AnimationData());
    if (s.phazonTex) {
      gpRender->AllocatePhazonSuitMaskTexture();
    }
  }
}

bool Render(const CStateManager& mgr, const CTransform4f& xf, const CActorLights* lights) {
  if (!Ready()) {
    return false;
  }
  const bool phazon = s.phazonTex && s.phazonTex->IsLoaded();
  if (phazon) {
    GXSetDstAlpha(GX_TRUE, 255);
  }
  s.body->Touch(mgr, 0);
  s.body->Render(mgr, xf, lights, CModelFlags::Normal());
  // Prime's cannon at the hand locator, unless the dread-samus mod's body carries its own.
  if (s.beamData && !PortDreadSamus::Replaces(PortDreadSamus::CutsceneBody(s.loadedChar))) {
    const CTransform4f gun = xf * s.body->GetScaledLocatorTransform("GUN_LCTR");
    s.beamData->Touch(mgr, 0);
    s.beamData->Render(mgr, gun, lights, CModelFlags::Normal());
  }
  if (phazon) {
    // As CScriptPlayerActor::Render.
    gpRender->CopyTex(1, true, CGraphics::GetDolphinSpareBuffer(), GX_TF_RGB565, false);
    const CVector3f delta = s.body->GetBounds(xf).GetCenterPoint() -
                            mgr.GetCameraManager()->GetCurrentCamera(mgr).GetTranslation();
    const float radius = CMath::Clamp(0.25f, (6.f - delta.Magnitude()) / 6.f, 2.f);
    gpRender->DrawPhazonSuitIndirectEffect(CColor(0.1f, 0.1f, 0.1f, 1.f), s.phazonTex, radius, 0.05f,
                                           CMath::FastSinR(s.phazonAngle),
                                           0.5f * CMath::FastSinR(s.phazonAngle));
  }
  return true;
}

bool CannonPosition(const CPlayer& player, CVector3f& out) {
  if (!Ready()) {
    return false;
  }
  // The body stands where CPlayer::Render puts it: on the floor of the player's box.
  CTransform4f bodyXf = player.GetTransform();
  CVector3f origin = bodyXf.GetTranslation();
  origin.SetZ(player.GetBoundingBox().GetMinPoint().GetZ());
  bodyXf.SetTranslation(origin);
  // GUN_LCTR is where the cannon is held, at the wrist; Charge_LCTR, where the charge forms, is
  // its muzzle (0.56 m past the forearm at this scale, where the Dread cannon's tip is too).
  out = (bodyXf * s.body->GetScaledLocatorTransform("Charge_LCTR")).GetTranslation();
  return true;
}

void Release() { Drop(); }

} // namespace PortThirdPersonBody
