// The third-person camera's animated body; see port_third_person_body.h.
#include "port_third_person_body.h"

#include <algorithm>
#include <cmath>

#include "port_dread_samus_mod.h"
#include "port_third_person.h"

#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
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

#include <memory>

namespace PortThirdPersonBody {
namespace {

// The cutscene Samus ANCS that carry the locomotion clips, best first. C2E1F513 has them all
// (it is in the Metroid1-6 paks); the rest only the ready pose, so the body stands in them.
constexpr uint32_t kAncsCandidates[] = {0xC2E1F513, 0x1413466E, 0x27550808,
                                        0x0C6B791C, 0x58673D8D, 0xC2BF3133};
constexpr int kPhazonChars[] = {3, 8};
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

struct State {
  // Loading (as CScriptPlayerActor::LoadSuit).
  int wantChar = -1;
  int loadedChar = -1;
  uint32_t ancs = 0;
  std::unique_ptr<TCachedToken<CModel>> suitModel;
  std::unique_ptr<TToken<CSkinRules>> suitSkin;
  // Built.
  std::unique_ptr<CModelData> body;
  int idx[6] = {-1, -1, -1, -1, -1, -1}; // by Clip
  Clip current = Clip::Hold;
  float phase = 0.f; // Backward: where the walk clip is, 0..1, run in reverse by hand
  bool animating = false;
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
  for (int p : kPhazonChars) {
    if (charIdx == p) {
      return; // Phazon needs the indirect-texture pass of CScriptPlayerActor: frozen body
    }
  }
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
  s.loadedChar = charIdx;
}

int Find(const CAnimData& data, const ClipNames& n) {
  int i = data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(n.a));
  if (i < 0) {
    i = data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(n.b));
  }
  return i;
}

void Build(CStateManager& mgr) {
  const CAnimRes res(s.ancs, s.loadedChar, CVector3f(kBodyScale, kBodyScale, kBodyScale), 0, true);
  s.body.reset(new CModelData(res));
  CAnimData* data = s.body->AnimationData();
  s.idx[int(Clip::Idle)] = s.idx[int(Clip::Hold)] = Find(*data, kIdle);
  s.idx[int(Clip::SlowWalk)] = Find(*data, kSlowWalk);
  s.idx[int(Clip::Walk)] = s.idx[int(Clip::Backward)] = Find(*data, kWalk);
  s.idx[int(Clip::Run)] = Find(*data, kRun);
  for (int& i : s.idx) {
    if (i < 0) {
      i = s.idx[int(Clip::Idle)];
    }
  }
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
  const Choice c = ChooseClip(CVector3f::Dot(vel, fwd), CVector3f::Dot(vel, right), grounded);
  CAnimData* data = s.body->AnimationData();
  if (c.clip != s.current) {
    data->SetAnimation(CAnimPlaybackParms(s.idx[int(c.clip)], -1, 1.f, true), false);
    s.current = c.clip;
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
}

bool Ready() { return s.body && s.animating; }

void PreRender() {
  if (Ready()) {
    s.body->AnimationData()->PreRender();
  }
}

bool Render(const CStateManager& mgr, const CTransform4f& xf, const CActorLights* lights) {
  if (!Ready()) {
    return false;
  }
  s.body->Touch(mgr, 0);
  s.body->Render(mgr, xf, lights, CModelFlags::Normal());
  // Prime's cannon at the hand locator, unless the dread-samus mod's body carries its own.
  if (s.beamData && !PortDreadSamus::Replaces(PortDreadSamus::CutsceneBody(s.loadedChar))) {
    const CTransform4f gun = xf * s.body->GetScaledLocatorTransform("GUN_LCTR");
    s.beamData->Touch(mgr, 0);
    s.beamData->Render(mgr, gun, lights, CModelFlags::Normal());
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
