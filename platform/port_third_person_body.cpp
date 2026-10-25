// The third-person camera's animated body; see port_third_person_body.h.
#include "port_third_person_body.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "port_dread_samus_mod.h"
#include "port_shell.h"
#include "port_third_person.h"

#include "Kyoto/Animation/CCharLayoutInfo.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
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
#include <utility>
#include <vector>

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
constexpr ClipNames kStop{"B_intorreadyfromwalk_samus", "B_rready_samus"};
// The walk clips (B_walkgun_samus is not one: the aim clips over the legs raise the cannon at
// any speed), each with the one that stands in where a set lacks it. The other sets give the
// ones the body's lacks: a walk back (the Phazon struggle's) and a sprint (Thardus's). The
// snow walks (B_walk_samus_snow, B_walk_insnow_samus) are not: they are the walk's frames.
// Each plays by the ground covered, backward when the body moves the other way; the walk back
// plays forward as the body moves back.
enum LocoSlot { kWalkBack, kSlowWalk, kWalk, kSlowRun, kFastRun, kLocoSlots };
// How fast the legs turn to a new strafe direction (radians per second): a quarter turn in 0.2 s.
constexpr float kLegTurnSpeed = 8.f;
constexpr ClipNames kLoco[kLocoSlots] = {{"Phazon_Struggle_walkback", nullptr},
                                         {"B_slowwalk_samus", "B_walk_samus"},
                                         {"B_walk_samus", "B_slowwalk_samus"},
                                         {"B_slowrunloop_samus", "B_walk_samus"},
                                         {"Thardus_fastrun_samus", nullptr}};
constexpr float kLocoDir[kLocoSlots] = {-1.f, 1.f, 1.f, 1.f, 1.f};
// The walk clips by speed, slowest first (LocoWeights): forward, and back (the walk back alone,
// played as fast as the ground goes: there is no shipped run back; the forward ones reversed
// without it).
constexpr LocoSlot kForwardTiers[] = {kSlowWalk, kWalk, kSlowRun, kFastRun};
constexpr LocoSlot kBackTiers[] = {kWalkBack};
// The clips played once (or held) for a stance or over the upper body, as phases of the clip:
// from, to (held there once reached). The jump is the run-and-jump's push-off into its tuck; the
// fall holds the tuck's airborne pose, legs drawn up and the cannon forward (the crater jump's
// drop is a dive, crouched with an arm flung up, and read as a stumble); the landing the hangar landing's crouch and rise (after it she
// turns to the ship); the flinch the Omega intro's lean back from the blast; the scan the scan
// cutscene's hand at the visor. Their root height is held at the skeleton's (Ground): the
// player's own movement carries the body.
struct OneShot {
  const char* name;
  float from;
  float to;
};
constexpr OneShot kJump{"01_Intro_Hanger_runandjump_samus", 0.66f, 0.7f};
constexpr OneShot kFall{"01_Intro_Hanger_runandjump_samus", 0.7f, 0.72f};
constexpr OneShot kLand{"01_Intro_Hanger_landintoship_samus", 0.06f, 0.3f};
constexpr OneShot kFlinch{"Omega_Intro_Samus_01_flinch", 0.2f, 0.45f};
constexpr OneShot kScan{"Scan_Cine_forward", 0.03f, 0.03f};
// A hit of at least this much flinches the body: what makes the first-person cannon jolt
// (CPlayerGun::TakeDamage), so heat and poison don't.
constexpr float kFlinchDamage = 10.f;
// The flinch eases in and out over these, in seconds.
constexpr float kFlinchIn = 0.1f;
constexpr float kFlinchOut = 0.3f;
// The aim clips by cell of the 3x3 grid (AimGridWeights: row 0 down, 1 level, 2 up; columns as
// named, which Build checks against where each clip points the cannon). Each clip turns the
// cannon from the ready aim to its own and holds it from about 60% on: the body takes the held
// pose (kAimHold). B_ready_samus is the level aim. The ANCS has no level-left clip, so that cell
// is the level aim too (the shoulder turns the rest of the way, AimArm; half of downl and upl
// points the cannon only 30 degrees left, their arms are too far apart to blend), and no
// down-centre clip, so that one is half of each down clip.
constexpr const char* kReady = "B_ready_samus";
constexpr float kAimHold = 0.999f;
constexpr const char* kAimNames[9][2] = {
    {"B_aimr45downl_samus", nullptr}, {"B_aimr45downl_samus", "B_aimr45downr_samus"},
    {"B_aimr45downr_samus", nullptr}, {kReady, nullptr},
    {kReady, nullptr},                {"B_aimrright_samus", nullptr},
    {"B_aimr45upl_samus", nullptr},   {"B_aimr45upc_samus", nullptr},
    {"B_aimr45upr_samus", nullptr}};
// A clip's pose: each segment's rotation and offset as RecalcPoseBuilder puts them in the pose
// builder. Segment ids are small (CAnimData keeps 100 for its pose history).
constexpr int kSegs = 128;
struct Pose {
  CQuaternion rot[kSegs];
  CVector3f off[kSegs];
};
struct AimCell {
  int clip[2];
  float weight[2];
};
// The other cutscene ANCS on the body's skeleton (CINF 19C53902's segments): the jumps, landings,
// turns and walks back the body's own set lacks. Each is sampled by an animation tree of its own
// (a CModelData never drawn) and its pose put on the body's bones.
constexpr uint32_t kSkeletonSets[] = {
    0xC2E1F513, 0x1413466E, 0x7FE82575, 0xBD389167, 0xA7C039CB, 0x863C1B6A, 0xBAD39B59,
    0xF7EF3A25, 0x58673D8D, 0x4082E602, 0x27550808, 0x2170BFFC, 0x3B448D15, 0xFD49BDDE,
    0x0FA35B51, 0xD31EFEFE, 0x0C6B791C, 0xC8943F0F, 0xA64B8017, 0xDF9B8A9B, 0x62F4E61D};
// A clip of any of those sets.
struct Clip {
  CAnimData* data = nullptr;
  int index = -1;
  bool Valid() const { return data != nullptr && index >= 0; }
  bool operator==(const Clip& o) const { return data == o.data && index == o.index; }
};

struct State {
  // Loading (as CScriptPlayerActor::LoadSuit).
  int wantChar = -1;
  int loadedChar = -1;
  uint32_t ancs = 0;
  std::unique_ptr<TCachedToken<CModel>> suitModel;
  std::unique_ptr<TToken<CSkinRules>> suitSkin;
  // Built.
  std::unique_ptr<CModelData> body;
  rstl::vector<CSegId> segs; // the segments RecalcPoseBuilder poses
  bool posed[kSegs] = {};     // which segments segs holds
  bool upper[kSegs] = {};     // Spine_1 and everything under it: what the aim clips pose
  CSegId hips = CSegId::Invalid(); // the top posed bone, which carries a clip's height
  float hipsHeight = 0.f;          // its height in the skeleton
  CSegId spine = CSegId::Invalid(); // Spine_1, the upper body's top bone
  int idle = -1;
  int stop = -1;
  Clip loco[kLocoSlots];
  // Measured by Build: each walk clip's ground speed (world units per second), the ground one
  // loop covers, and where in the loop the left foot is furthest forward (Measure).
  float locoSpeed[kLocoSlots] = {};
  float locoStride[kLocoSlots] = {};
  float locoPeak[kLocoSlots] = {};
  Clip jump, fall, land, flinch, scan;
  AimCell aim[9] = {};
  bool aimLayer = false;
  // The cannon's yaw and pitch in each aim cell (radians, model space, + right and up), and
  // the grid's extents about the ready pose's (aimCentre).
  float aimYaw[9] = {};
  float aimPitch[9] = {};
  float gridLeft = 0.f, gridRight = 0.f, gridDown = 0.f, gridUp = 0.f;
  // Animating.
  Stance stance = Stance::Idle;
  float stanceWeight[int(Stance::Count)] = {1.f};
  float locoWeight[kLocoSlots] = {};
  float stride = 0.f;     // where the feet are, in loops; each walk clip is at stride + its peak
  bool legsBack = false;  // the legs walk back (LegsBackward)
  float legYaw = 0.f;     // the legs' turn from the facing (LegYaw), eased
  float clock = 0.f;      // seconds, for the clips that play by time (ready, aim)
  float stanceTime[int(Stance::Count)] = {}; // seconds since each stance was last entered
  float airTime = 0.f;    // seconds off the ground
  // The last stance changes, oldest first (thirdperson feet): when, to what, at what speeds.
  struct Change {
    float clock;
    Stance to;
    float fwd, side, rise;
  };
  Change changes[8] = {};
  int changeCount = 0;
  float flinchTime = -1.f; // seconds into the flinch; < 0 none
  float scanWeight = 0.f;
  float gunUp = 0.f;      // seconds the cannon stays raised (NoteShot)
  float gridU = 0.f, gridV = 0.f, gridSpeedU = 0.f, gridSpeedV = 0.f;
  // How far the shoulder turned the aim clips' cannon onto the aim, and how far it still misses
  // (radians; thirdperson feet).
  float armTurn = 0.f, armMiss = 0.f;
  float poseYaw = 0.f, posePitch = 0.f, aimYawNow = 0.f, aimPitchNow = 0.f;
  CVector3f pivot = CVector3f::Zero(); // the shoulder, as last posed (model space, unscaled)
  bool animating = false;
  // The other sets (kSkeletonSets), built once the body is.
  std::vector<std::pair<uint32_t, std::unique_ptr<CModelData>>> sets;
  bool allSets = false;
  std::string setNotes; // the sets left out, and why (thirdperson clips)
  // Debug (PinClip): the clip held instead of the movement's, and where in it.
  Clip pinned;
  float pinPhase = 0.f;
  float pinYaw = 0.f; // degrees, to look at a pinned pose from the front
  // Aiming the cannon: the shoulder that turns, the locators that give the cannon's axis.
  CSegId shoulder = CSegId::Invalid();
  CSegId gunSeg = CSegId::Invalid();
  CSegId muzzleSeg = CSegId::Invalid();
  CQuaternion shoulderPose = CQuaternion::NoRotation(); // the clips', before the aim
  bool advanced = false; // the pose was rebuilt since the last AimArm (shoulderPose is stale)
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
  s.pinned = Clip();
  s.sets.clear();
  s.allSets = false;
  s.setNotes.clear();
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
  s.armTurn = angle;
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
  const CVector3f line = position(s.muzzleSeg) - position(s.gunSeg);
  const CVector3f rest = target - position(s.muzzleSeg);
  s.armMiss = line.CanBeNormalized() && rest.CanBeNormalized()
                  ? std::acos(std::clamp(CVector3f::Dot(line.AsNormalized(), rest.AsNormalized()), -1.f, 1.f))
                  : 0.f;
}

float Wrap(float phase) { return phase - std::floor(phase); }

// a to b by t on the short way, as CAnimTreeBlend slerps its two children.
CQuaternion Blend(const CQuaternion& a, const CQuaternion& b, float t) {
  const float dot = CQuaternion::Dot(a, b);
  const CQuaternion to = dot < 0.f ? CQuaternion(-b.GetScalar(), -b.GetVector()) : b;
  if (std::fabs(dot) > 0.9995f) {
    // Too close for Slerp's 1/sin: the straight line, normalized.
    return CQuaternion((1.f - t) * a.GetScalar() + t * to.GetScalar(),
                       (1.f - t) * a.GetVector() + t * to.GetVector())
        .BuildNormalized();
  }
  return CQuaternion::Slerp(a, to, t);
}

// The pose of clip at phase 0..1, sampled by the body's own animation tree into the pose
// builder (offsets first back to the skeleton's, as a clip without an offset track leaves them).
// A clip of another set is sampled by that set's tree, its offsets moved onto the body's bones (a
// suit's bones may be longer or shorter than the set's: each keeps the clip's change to it).
void Sample(const Clip& clip, float phase, Pose& out) {
  CAnimData& data = *clip.data;
  data.SetAnimation(CAnimPlaybackParms(clip.index, -1, 1.f, true), true);
  data.SetPhase(phase);
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  const CCharLayoutInfo& poseLayout = **pb.CharLayoutInfo();
  for (const CSegId& seg : s.segs) {
    pb.Insert(seg, CQuaternion::NoRotation());
    pb.Insert(seg, poseLayout.GetFromParentUnrotated(seg));
  }
  data.PortInvalidatePose();
  data.PreRender();
  const CAnimData& body = *s.body->GetAnimationData();
  const CCharLayoutInfo& bodyLayout = **body.GetPoseBuilder().CharLayoutInfo();
  const bool other = &data != &body && &bodyLayout != &poseLayout;
  for (const CSegId& seg : s.segs) {
    out.rot[seg.val()] = pb.GetSegRotation(seg);
    out.off[seg.val()] = other ? bodyLayout.GetFromParentUnrotated(seg) +
                                     (pb.GetSegOffset(seg) - poseLayout.GetFromParentUnrotated(seg))
                               : pb.GetSegOffset(seg);
  }
}

void Sample(CAnimData& data, int clip, float phase, Pose& out) { Sample(Clip{&data, clip}, phase, out); }

// Whether the body plays a clip of set id (the rest are built only to list and pin): the walk
// back, the sprint, the jump and landing, the fall, the flinch.
bool SetWired(uint32_t id) {
  return id == 0xA7C039CB || id == 0xBD389167 || id == 0x1413466E || id == 0x7FE82575 ||
         id == 0x58673D8D;
}

// Builds the other sets (all of kSkeletonSets with all, else only those the body plays from),
// each in the body's suit where it has it.
void BuildSets(bool all) {
  if (all && s.allSets) {
    return;
  }
  for (uint32_t id : kSkeletonSets) {
    if (id == s.ancs || (!all && !SetWired(id)) ||
        std::any_of(s.sets.begin(), s.sets.end(), [id](const auto& e) { return e.first == id; })) {
      continue;
    }
    // Most are in one world's PAK, which only that world has loaded: those come off the disc.
    if (!gpResourceFactory->GetResLoader().PortLoadable(id)) {
      char line[80];
      std::snprintf(line, sizeof(line), "%08X not on the disc\n", id);
      s.setNotes += line;
      continue;
    }
    const CAnimRes probe(id, 0, CVector3f(kBodyScale, kBodyScale, kBodyScale), 0, true);
    TLockedToken<CCharacterFactory> factory = gpCharacterFactoryBuilder->GetFactory(probe);
    const int charIdx = s.loadedChar < factory->PortNumCharacters() ? s.loadedChar : 0;
    const CAnimRes res(id, charIdx, CVector3f(kBodyScale, kBodyScale, kBodyScale), 0, true);
    std::unique_ptr<CModelData> set(new CModelData(res));
    const CCharLayoutInfo& layout = *set->GetAnimationData()->GetCharLayoutInfo();
    // Only a set with every bone the body poses.
    const auto missing = std::count_if(s.segs.begin(), s.segs.end(),
                                       [&](const CSegId& seg) { return !layout.ContainsSegment(seg); });
    if (missing == 0) {
      s.sets.emplace_back(id, std::move(set));
    } else {
      char line[80];
      std::snprintf(line, sizeof(line), "%08X lacks %d of the body's bones\n", id, int(missing));
      s.setNotes += line;
    }
  }
  s.allSets = s.allSets || all;
}

// The clip called name in the body's set, else the first other set that has it.
Clip FindClip(const char* name) {
  CAnimData* body = s.body->AnimationData();
  int i = body->GetCharacterInfo().GetAnimationIndex(rstl::string_l(name));
  if (i >= 0) {
    return Clip{body, i};
  }
  for (auto& e : s.sets) {
    CAnimData* data = e.second->AnimationData();
    i = data->GetCharacterInfo().GetAnimationIndex(rstl::string_l(name));
    if (i >= 0) {
      return Clip{data, i};
    }
  }
  return Clip();
}

// The clip with the first name, else the second.
Clip FindClip(const ClipNames& n) {
  const Clip clip = FindClip(n.a);
  return clip.Valid() || n.b == nullptr ? clip : FindClip(n.b);
}

// Holds pose p's hips at most at the skeleton's height: a clip's jump or fall carries the body
// up and down by its hips (6 m up in the run-and-jump, 18 m down the crater), the player's own
// movement does here. Crouches stay.
void Ground(Pose& p) {
  if (s.hips != CSegId::Invalid()) {
    CVector3f& off = p.off[s.hips.val()];
    off.SetZ(std::min(off.GetZ(), s.hipsHeight));
  }
}

float Duration(const Clip& clip) { return std::max(clip.data->GetAnimationDuration(clip.index), 0.1f); }

// How long shot plays in clip before it holds, in seconds, and its phase t seconds in.
float ShotSeconds(const Clip& clip, const OneShot& shot) { return (shot.to - shot.from) * Duration(clip); }
float ShotPhase(const Clip& clip, const OneShot& shot, float t) {
  return std::min(shot.from + t / Duration(clip), shot.to);
}

// Blends pose p over acc by w (the upper body's bones only, with upperOnly).
void Overlay(Pose& acc, const Pose& p, float w, bool upperOnly) {
  if (w <= 0.f) {
    return;
  }
  for (const CSegId& seg : s.segs) {
    const int i = seg.val();
    if (upperOnly && !s.upper[i]) {
      continue;
    }
    acc.rot[i] = Blend(acc.rot[i], p.rot[i], w);
    acc.off[i] = acc.off[i] * (1.f - w) + p.off[i] * w;
  }
}

// Puts pose p on the body.
void ApplyPose(CAnimData& data, const Pose& p) {
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  for (const CSegId& seg : s.segs) {
    pb.Insert(seg, p.rot[seg.val()]);
    pb.Insert(seg, p.off[seg.val()]);
  }
  data.SetPoseBuilderValid(false);
}

// Adds pose p at weight w to the running blend acc (of total weight total so far): each
// addition slerps toward it by its share, so two poses blend as CAnimTreeBlend blends them.
void Mix(Pose& acc, float& total, const Pose& p, float w, bool upperOnly) {
  if (w <= 0.f) {
    return;
  }
  total += w;
  const float t = w / total;
  for (const CSegId& seg : s.segs) {
    const int i = seg.val();
    if (upperOnly && !s.upper[i]) {
      continue;
    }
    if (t >= 1.f) {
      acc.rot[i] = p.rot[i];
      acc.off[i] = p.off[i];
    } else {
      acc.rot[i] = Blend(acc.rot[i], p.rot[i], t);
      acc.off[i] = acc.off[i] * (1.f - t) + p.off[i] * t;
    }
  }
}

CVector3f SegPosition(const CAnimData& data, CSegId seg) {
  CTransform4f xf = CTransform4f::Identity();
  data.GetPoseBuilder().BuildTransform(seg, xf);
  return xf.GetTranslation();
}

// Pose (built last) as the cannon's yaw and pitch: the axis from GUN_LCTR to Charge_LCTR.
void CannonAngles(const CAnimData& data, float& yaw, float& pitch) {
  const CVector3f d = SegPosition(data, s.muzzleSeg) - SegPosition(data, s.gunSeg);
  yaw = std::atan2(d.GetX(), d.GetY());
  pitch = std::atan2(d.GetZ(), std::sqrt(d.GetX() * d.GetX() + d.GetY() * d.GetY()));
}

Pose sTmp;
Pose sAcc;
Pose sArms;
Pose sModel;
Pose sBlended;

// Pose p with each rotation turned into the model's frame (its parents' rotations times its
// own); a segment RecalcPoseBuilder doesn't set keeps the pose builder's rotation.
void ModelRotations(const CAnimData& data, const Pose& p, Pose& out) {
  const CHierarchyPoseBuilder& pb = data.GetPoseBuilder();
  const CCharLayoutInfo& layout = **pb.CharLayoutInfo();
  bool done[kSegs] = {};
  const auto resolve = [&](auto& self, CSegId seg) -> const CQuaternion& {
    const int i = seg.val();
    if (!done[i]) {
      const CQuaternion local = s.posed[i] ? p.rot[i] : pb.GetSegRotation(seg);
      const CSegId parent = layout.GetOriginalParent(seg);
      out.rot[i] = parent == CSegId::Character() ? local : self(self, parent) * local;
      out.off[i] = p.off[i];
      done[i] = true;
    }
    return out.rot[i];
  };
  for (const CSegId& seg : s.segs) {
    resolve(resolve, seg);
  }
}

// What the blend needs to know about the clips, measured on them: the walk clips' speeds,
// strides and foot phases, the aim clips' directions.
void Measure(CAnimData& data) {
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  const CSegId lFoot = layout.GetSegIdFromString(rstl::string_l("L_ankle"));
  const CSegId rFoot = layout.GetSegIdFromString(rstl::string_l("R_ankle"));
  for (int i = 0; i < kLocoSlots; ++i) {
    const Clip& clip = s.loco[i];
    s.locoSpeed[i] = s.locoStride[i] = s.locoPeak[i] = 0.f;
    if (!clip.Valid()) {
      continue;
    }
    s.locoSpeed[i] = std::fabs(clip.data->GetAverageVelocity(clip.index)) * kBodyScale;
    s.locoStride[i] = s.locoSpeed[i] * clip.data->GetAnimationDuration(clip.index);
    if (lFoot == CSegId::Invalid() || rFoot == CSegId::Invalid()) {
      continue;
    }
    // The left ankle's lead over the right along the facing, over one loop.
    constexpr int kSamples = 16;
    float lead[kSamples];
    for (int k = 0; k < kSamples; ++k) {
      Sample(clip, float(k) / kSamples, sTmp);
      lead[k] = SegPosition(*clip.data, lFoot).GetY() - SegPosition(*clip.data, rFoot).GetY();
    }
    s.locoPeak[i] = CurvePeak(lead, kSamples);
  }

  // The aim grid: every cell its clips (missing ones the ready pose), then each row's columns
  // put on the side their cannon points to.
  const int ready = data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(kReady));
  const int centre = ready >= 0 ? ready : s.idle;
  for (int c = 0; c < 9; ++c) {
    AimCell& cell = s.aim[c];
    const int parts = kAimNames[c][1] != nullptr ? 2 : 1;
    for (int k = 0; k < 2; ++k) {
      const int i = k < parts ? data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(kAimNames[c][k])) : -1;
      cell.clip[k] = k < parts ? (i >= 0 ? i : centre) : -1;
      cell.weight[k] = k < parts ? 1.f / parts : 0.f;
    }
  }
  s.aimLayer = s.gunSeg != CSegId::Invalid() && s.muzzleSeg != CSegId::Invalid() && centre >= 0;
  if (!s.aimLayer) {
    return;
  }
  for (int c = 0; c < 9; ++c) {
    float total = 0.f;
    for (int k = 0; k < 2; ++k) {
      if (s.aim[c].clip[k] >= 0) {
        Sample(data, s.aim[c].clip[k], kAimHold, sTmp);
        ModelRotations(data, sTmp, sModel);
        Mix(sAcc, total, sModel, s.aim[c].weight[k], false);
      }
    }
    // Blended as PoseBody blends them, in the model's frame.
    const CCharLayoutInfo& poseLayout = **data.PoseBuilder().CharLayoutInfo();
    for (const CSegId& seg : s.segs) {
      const CSegId parent = poseLayout.GetOriginalParent(seg);
      // (The one segment segs lacks, 3, is a root: its rotation is its model one.)
      const CQuaternion parentModel = parent == CSegId::Character() ? CQuaternion::NoRotation()
                                      : s.posed[parent.val()]
                                          ? sAcc.rot[parent.val()]
                                          : data.PoseBuilder().GetSegRotation(parent);
      data.PoseBuilder().Insert(seg, parentModel.BuildInverted() * sAcc.rot[seg.val()]);
      data.PoseBuilder().Insert(seg, sAcc.off[seg.val()]);
    }
    CannonAngles(data, s.aimYaw[c], s.aimPitch[c]);
  }
  for (int row = 0; row < 3; ++row) {
    if (s.aimYaw[row * 3] > s.aimYaw[row * 3 + 2]) {
      std::swap(s.aim[row * 3], s.aim[row * 3 + 2]);
      std::swap(s.aimYaw[row * 3], s.aimYaw[row * 3 + 2]);
      std::swap(s.aimPitch[row * 3], s.aimPitch[row * 3 + 2]);
    }
  }
  // The extents: how far from the ready pose's cannon the outer cells' clips point it (the
  // filled cells don't count), a quarter turn where no clip says.
  const float yaw0 = s.aimYaw[4];
  const float pitch0 = s.aimPitch[4];
  const auto extent = [](float sum, int n) { return n > 0 && sum / n > 0.1f ? sum / n : 0.785f; };
  float left = 0.f, right = 0.f, down = 0.f, up = 0.f;
  int nl = 0, nr = 0, nd = 0, nu = 0;
  for (int c = 0; c < 9; ++c) {
    if (s.aim[c].clip[0] == centre && s.aim[c].clip[1] < 0) {
      continue;
    }
    if (c % 3 == 0) {
      left += yaw0 - s.aimYaw[c], ++nl;
    } else if (c % 3 == 2) {
      right += s.aimYaw[c] - yaw0, ++nr;
    }
    if (c / 3 == 0) {
      down += pitch0 - s.aimPitch[c], ++nd;
    } else if (c / 3 == 2) {
      up += s.aimPitch[c] - pitch0, ++nu;
    }
  }
  s.gridLeft = extent(left, nl);
  s.gridRight = extent(right, nr);
  s.gridDown = extent(down, nd);
  s.gridUp = extent(up, nu);
}

// Poses the body into its pose builder: the legs' stances blended by their weights (the walk
// clips by speed, in step), then, with the cannon raised, the aim clips over the upper body.
void PoseBody(CAnimData& data) {
  float total = 0.f;
  const float idleDuration = std::max(data.GetAnimationDuration(s.idle), 0.1f);
  if (s.stanceWeight[int(Stance::Idle)] > 0.f) {
    Sample(data, s.idle, Wrap(s.clock / idleDuration), sTmp);
    Mix(sAcc, total, sTmp, s.stanceWeight[int(Stance::Idle)], false);
  }
  if (s.stanceWeight[int(Stance::Stop)] > 0.f) {
    // Played once: held on its last frame (phase 1 would wrap to its first).
    const float duration = std::max(data.GetAnimationDuration(s.stop), 0.1f);
    Sample(data, s.stop, std::min(s.stanceTime[int(Stance::Stop)] / duration, 0.999f), sTmp);
    Mix(sAcc, total, sTmp, s.stanceWeight[int(Stance::Stop)], false);
  }
  for (int i = 0; i < kLocoSlots; ++i) {
    const float w = s.stanceWeight[int(Stance::Loco)] * s.locoWeight[i];
    if (w > 0.f) {
      Sample(s.loco[i], Wrap(kLocoDir[i] * s.stride + s.locoPeak[i]), sTmp);
      Mix(sAcc, total, sTmp, w, false);
    }
  }
  const struct {
    Stance stance;
    const Clip& clip;
    const OneShot& shot;
  } shots[] = {{Stance::Jump, s.jump, kJump}, {Stance::Fall, s.fall, kFall}, {Stance::Land, s.land, kLand}};
  for (const auto& e : shots) {
    const float w = s.stanceWeight[int(e.stance)];
    if (w > 0.f && e.clip.Valid()) {
      Sample(e.clip, ShotPhase(e.clip, e.shot, s.stanceTime[int(e.stance)]), sTmp);
      Ground(sTmp);
      Mix(sAcc, total, sTmp, w, false);
    }
  }
  if (total <= 0.f) {
    // Only stances whose clip is missing: the ready pose.
    Sample(data, s.idle, Wrap(s.clock / idleDuration), sAcc);
  }
  // Over the legs' upper body: the hand to the visor while scanning, a flinch from a hit.
  if (s.scanWeight > 0.f && s.scan.Valid()) {
    Sample(s.scan, kScan.from, sTmp);
    Overlay(sAcc, sTmp, s.scanWeight, true);
  }
  if (s.flinchTime >= 0.f && s.flinch.Valid()) {
    const float length = ShotSeconds(s.flinch, kFlinch);
    const float w = std::clamp(std::min(s.flinchTime / kFlinchIn, (length - s.flinchTime) / kFlinchOut), 0.f, 1.f);
    Sample(s.flinch, ShotPhase(s.flinch, kFlinch, s.flinchTime), sTmp);
    Overlay(sAcc, sTmp, w, true);
  }
  if (s.aimLayer && s.aimWeight > 0.f) {
    float cellWeight[9];
    AimGridWeights(s.gridU, s.gridV, cellWeight);
    // Each clip once, at its summed weight.
    int clips[18];
    float weights[18];
    int n = 0;
    for (int c = 0; c < 9; ++c) {
      for (int k = 0; k < 2; ++k) {
        const float w = cellWeight[c] * s.aim[c].weight[k];
        if (w <= 0.f || s.aim[c].clip[k] < 0) {
          continue;
        }
        int j = 0;
        while (j < n && clips[j] != s.aim[c].clip[k]) {
          ++j;
        }
        if (j == n) {
          clips[n] = s.aim[c].clip[k];
          weights[n++] = 0.f;
        }
        weights[j] += w;
      }
    }
    // Blended in the model's frame, each bone's orientation there: a bone blended in its
    // parent's frame swings the cannon off the line between the clips' aims when the clips'
    // arms differ a lot (the right shoulder turns 156 degrees from the ready pose to downl).
    // Then back into the parents' frames, the upper body's parent the legs'.
    float armsTotal = 0.f;
    for (int j = 0; j < n; ++j) {
      Sample(data, clips[j], kAimHold, sTmp);
      ModelRotations(data, sTmp, sModel);
      Mix(sArms, armsTotal, sModel, weights[j], true);
    }
    if (armsTotal > 0.f) {
      ModelRotations(data, sAcc, sModel);
      const CCharLayoutInfo& layout = **data.PoseBuilder().CharLayoutInfo();
      for (const CSegId& seg : s.segs) {
        const int i = seg.val();
        if (!s.upper[i]) {
          continue;
        }
        sBlended.rot[i] = Blend(sModel.rot[i], sArms.rot[i], s.aimWeight);
      }
      for (const CSegId& seg : s.segs) {
        const int i = seg.val();
        if (!s.upper[i]) {
          continue;
        }
        const int parent = layout.GetOriginalParent(seg).val();
        const CQuaternion& parentModel = s.upper[parent] ? sBlended.rot[parent] : sModel.rot[parent];
        sAcc.rot[i] = parentModel.BuildInverted() * sBlended.rot[i];
        sAcc.off[i] = sAcc.off[i] * (1.f - s.aimWeight) + sArms.off[i] * s.aimWeight;
      }
    }
  }
  // Strafing: the hips turn the legs the way they walk (about the model's up), and Spine_1
  // turns back, so the upper body keeps its orientation in the model.
  if (std::fabs(s.legYaw) > 1e-4f && s.hips != CSegId::Invalid() && s.spine != CSegId::Invalid()) {
    ModelRotations(data, sAcc, sModel);
    const CQuaternion turn = CQuaternion::ZRotation(CRelAngle::FromRadians(s.legYaw));
    const int h = s.hips.val();
    const int sp = s.spine.val();
    const CQuaternion hipsModel = sModel.rot[h];
    const CQuaternion spineParent = sModel.rot[sp] * sAcc.rot[sp].BuildInverted();
    sAcc.rot[h] = sAcc.rot[h] * hipsModel.BuildInverted() * turn * hipsModel;
    sAcc.rot[sp] = spineParent.BuildInverted() * turn.BuildInverted() * sModel.rot[sp];
  }
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  for (const CSegId& seg : s.segs) {
    pb.Insert(seg, sAcc.rot[seg.val()]);
    pb.Insert(seg, sAcc.off[seg.val()]);
  }
  data.SetPoseBuilderValid(false);
  if (s.shoulder != CSegId::Invalid()) {
    s.pivot = SegPosition(data, s.shoulder);
  }
  if (s.aimLayer) {
    CannonAngles(data, s.poseYaw, s.posePitch);
  }
  s.advanced = true;
}

void Build(CStateManager& mgr) {
  const CAnimRes res(s.ancs, s.loadedChar, CVector3f(kBodyScale, kBodyScale, kBodyScale), 0, true);
  s.body.reset(new CModelData(res));
  CAnimData* data = s.body->AnimationData();
  s.idle = Find(*data, kIdle);
  s.animating = s.idle >= 0;
  if (!s.animating) {
    return;
  }
  s.stop = Find(*data, kStop);
  if (s.stop < 0) {
    s.stop = s.idle;
  }
  // The segments RecalcPoseBuilder sets (it never sets 3), and which of them the aim clips own.
  const CCharLayoutInfo& layout = *data->GetCharLayoutInfo();
  const CSegId spine = layout.GetSegIdFromString(rstl::string_l("Spine_1"));
  s.segs.clear();
  std::fill(std::begin(s.posed), std::end(s.posed), false);
  std::fill(std::begin(s.upper), std::end(s.upper), false);
  for (const CSegId& seg : layout.GetBodyPartSegIds()) {
    if (seg.val() != 3 && seg.val() < kSegs) {
      s.segs.push_back(seg);
      s.posed[seg.val()] = true;
      s.upper[seg.val()] = spine != CSegId::Invalid() && (seg == spine || IsAncestor(layout, spine, seg));
    }
  }
  s.spine = spine;
  s.hips = CSegId::Invalid();
  for (const CSegId& seg : s.segs) {
    const CSegId parent = layout.GetOriginalParent(seg);
    if (parent == CSegId::Character() || !s.posed[parent.val()]) {
      s.hips = seg;
      s.hipsHeight = layout.GetFromParentUnrotated(seg).GetZ();
      break;
    }
  }
  FindAimBones(*data);
  BuildSets(false);
  // A missing walk clip stands in by its fallback, else the walk (the ready pose without one);
  // a missing walk back or sprint drops out of its tiers (Update).
  const Clip walk = FindClip(kLoco[kWalk]);
  for (int i = 0; i < kLocoSlots; ++i) {
    s.loco[i] = FindClip(kLoco[i]);
    if (!s.loco[i].Valid() && (i == kSlowWalk || i == kWalk || i == kSlowRun)) {
      s.loco[i] = walk.Valid() ? walk : Clip{data, s.idle};
    }
  }
  s.jump = FindClip(kJump.name);
  s.fall = FindClip(kFall.name);
  s.land = FindClip(kLand.name);
  s.flinch = FindClip(kFlinch.name);
  s.scan = FindClip(kScan.name);
  Measure(*data);
  s.stance = Stance::Idle;
  std::fill(std::begin(s.stanceWeight), std::end(s.stanceWeight), 0.f);
  s.stanceWeight[int(Stance::Idle)] = 1.f;
  std::fill(std::begin(s.locoWeight), std::end(s.locoWeight), 0.f);
  s.locoWeight[kWalk] = 1.f;
  s.flinchTime = -1.f;
  s.scanWeight = 0.f;
  s.airTime = 0.f;
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
  if (s.phazonTex) {
    s.phazonTex->TryCache();
    s.phazonAngle = CMath::ClampRadians(s.phazonAngle + 0.03f);
  }
  if (s.pinned.Valid()) {
    s.aimWeight = 0.f;
    return;
  }
  const CTransform4f& xf = player.GetTransform();
  const CVector3f vel = player.GetVelocityWR();
  const float fwd = CVector3f::Dot(vel, xf.GetForward());
  const float side = CVector3f::Dot(vel, xf.GetRight());
  const bool grounded = player.GetPlayerMovementState() == NPlayer::kMS_OnGround;
  s.gunUp = std::max(s.gunUp - dt, 0.f);
  const bool gunUp = s.gunUp > 0.f || player.GetPlayerGun()->IsCharging();

  // The legs. Every stance fades in over kBlendSeconds while the others fade out.
  const float stanceDone = s.stance == Stance::Stop
                               ? s.body->GetAnimationData()->GetAnimationDuration(s.stop)
                           : s.stance == Stance::Land && s.land.Valid() ? ShotSeconds(s.land, kLand)
                                                                         : 0.f;
  const Stance stance = ChooseStance(fwd, side, grounded, vel.GetZ(), s.stance,
                                     s.stanceTime[int(s.stance)] >= stanceDone,
                                     s.airTime >= kLandAirSeconds);
  if (stance != s.stance) {
    s.stanceTime[int(stance)] = 0.f;
    if (s.changeCount == int(std::size(s.changes))) {
      std::move(s.changes + 1, s.changes + s.changeCount--, s.changes);
    }
    s.changes[s.changeCount++] = {s.clock, stance, fwd, side, vel.GetZ()};
  }
  s.stance = stance;
  for (int i = 0; i < int(Stance::Count); ++i) {
    s.stanceWeight[i] = Approach(s.stanceWeight[i], i == int(stance) ? 1.f : 0.f, dt / kBlendSeconds);
    s.stanceTime[i] += dt;
  }
  s.airTime = grounded ? 0.f : s.airTime + dt;
  s.clock += dt;
  // Over the legs: a hard hit flinches (not again until it is over), scanning raises the hand.
  if (s.flinchTime >= 0.f) {
    s.flinchTime += dt;
    if (!s.flinch.Valid() || s.flinchTime >= ShotSeconds(s.flinch, kFlinch)) {
      s.flinchTime = -1.f;
    }
  }
  if (s.flinchTime < 0.f && player.WasDamaged() && player.GetPrevDamageAmount() >= kFlinchDamage) {
    s.flinchTime = 0.f;
  }
  const bool scanning = player.GetPlayerScanState() == CPlayer::kSS_Scanning;
  s.scanWeight = Approach(s.scanWeight, scanning ? 1.f : 0.f, dt / kBlendSeconds);
  // Walking, the legs turn the way the body goes (strafing: LegYaw) and the ground speed picks
  // the walk clips' weights (kept while the walk fades out) from the tiers for the way they
  // walk, and the feet move a loop for each stride of ground covered: the clips play at the
  // speed the body moves, in step with each other.
  const float speed = std::sqrt(fwd * fwd + side * side);
  if (stance == Stance::Loco) {
    s.legsBack = LegsBackward(fwd, side, s.legsBack);
    LocoSlot tier[4];
    int n = 0;
    const auto add = [&](const LocoSlot* slots, int count) {
      for (int i = 0; i < count; ++i) {
        if (s.loco[slots[i]].Valid()) {
          tier[n++] = slots[i];
        }
      }
    };
    if (s.legsBack) {
      add(kBackTiers, int(std::size(kBackTiers)));
    }
    if (n == 0) {
      add(kForwardTiers, int(std::size(kForwardTiers)));
    }
    float speeds[4] = {};
    float w[4] = {};
    for (int i = 0; i < n; ++i) {
      speeds[i] = s.locoSpeed[tier[i]];
    }
    LocoWeights(speed, speeds, n, w);
    std::fill(std::begin(s.locoWeight), std::end(s.locoWeight), 0.f);
    for (int i = 0; i < n; ++i) {
      s.locoWeight[tier[i]] = w[i];
    }
  }
  const float legYaw = stance == Stance::Loco ? LegYaw(fwd, side, s.legsBack) : 0.f;
  s.legYaw = s.legYaw + std::clamp(legYaw - s.legYaw, -kLegTurnSpeed * dt, kLegTurnSpeed * dt);
  float stride = 0.f;
  for (int i = 0; i < kLocoSlots; ++i) {
    stride += s.locoWeight[i] * s.locoStride[i];
  }
  if (stride > 0.1f) {
    s.stride = Wrap(s.stride + dt * (s.legsBack ? -speed : speed) / stride);
  }

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
  if (s.haveAim && s.aimLayer) {
    // Where the aim point is from the shoulder, as a place on the aim grid.
    CTransform4f bodyXf = xf;
    CVector3f origin = bodyXf.GetTranslation();
    origin.SetZ(player.GetBoundingBox().GetMinPoint().GetZ());
    bodyXf.SetTranslation(origin);
    const CVector3f d = bodyXf.TransposeMultiply(s.aimPoint) * (1.f / kBodyScale) - s.pivot;
    s.aimYawNow = std::atan2(d.GetX(), d.GetY());
    s.aimPitchNow = std::atan2(d.GetZ(), std::sqrt(d.GetX() * d.GetX() + d.GetY() * d.GetY()));
    const float yaw = std::atan2(d.GetX(), d.GetY()) - s.aimYaw[4];
    const float pitch =
        std::atan2(d.GetZ(), std::sqrt(d.GetX() * d.GetX() + d.GetY() * d.GetY())) - s.aimPitch[4];
    const float u = std::clamp(yaw >= 0.f ? yaw / s.gridRight : yaw / s.gridLeft, -1.f, 1.f);
    const float v = std::clamp(pitch >= 0.f ? pitch / s.gridUp : pitch / s.gridDown, -1.f, 1.f);
    if (s.aimWeight <= 0.f) {
      // Raised from the side: straight to the aim.
      s.gridU = u;
      s.gridV = v;
      s.gridSpeedU = s.gridSpeedV = 0.f;
    } else {
      SmoothToward(s.gridU, s.gridSpeedU, u, dt);
      SmoothToward(s.gridV, s.gridSpeedV, v, dt);
    }
  }
  s.aimWeight = s.haveAim ? std::min(s.aimWeight + dt / kAimInSeconds, 1.f)
                          : std::max(s.aimWeight - dt / kAimOutSeconds, 0.f);
}

bool Ready() { return s.body && s.animating; }

void NoteShot() { s.gunUp = kGunUpSeconds; }

void PreRender(const CStateManager& mgr) {
  if (Ready()) {
    CAnimData& data = *s.body->AnimationData();
    if (s.pinned.Valid()) {
      Sample(s.pinned, s.pinPhase, sAcc);
      Ground(sAcc);
      ApplyPose(data, sAcc);
      s.advanced = true;
    } else {
      PoseBody(data);
    }
    AimArm(mgr, data);
    if (s.phazonTex) {
      gpRender->AllocatePhazonSuitMaskTexture();
    }
  }
}

bool Render(const CStateManager& mgr, const CTransform4f& feet, const CActorLights* lights) {
  if (!Ready()) {
    return false;
  }
  const bool phazon = s.phazonTex && s.phazonTex->IsLoaded();
  // Remastered's shell, as CScriptPlayerActor::Render (port_shell.h).
  const bool shell =
      phazon && CModelData::GetRenderingModel(mgr) == CModelData::kWM_Normal && PortShell::Enabled();
  if (phazon && !shell) {
    GXSetDstAlpha(GX_TRUE, 255);
  }
  const CTransform4f xf =
      s.pinned.Valid() ? feet * CTransform4f::RotateZ(CRelAngle::FromDegrees(s.pinYaw)) : feet;
  s.body->Touch(mgr, 0);
  s.body->Render(mgr, xf, lights, CModelFlags::Normal());
  // Prime's cannon at the hand locator, unless the dread-samus mod's body carries its own.
  const bool drawGun = s.beamData && !PortDreadSamus::Replaces(PortDreadSamus::CutsceneBody(s.loadedChar));
  const CTransform4f gun = drawGun ? xf * s.body->GetScaledLocatorTransform("GUN_LCTR") : xf;
  if (drawGun) {
    s.beamData->Touch(mgr, 0);
    s.beamData->Render(mgr, gun, lights, CModelFlags::Normal());
  }
  if (shell) {
    // The lights are the player's, alive until the frame's Flush; the body is this file's.
    const CStateManager* const pmgr = &mgr;
    PortShell::Defer(PortShell::kBody, s.body->GetBounds(xf), [pmgr, xf, lights] {
      if (s.body) {
        s.body->Render(*pmgr, xf, lights, CModelFlags::Normal());
      }
    });
    if (drawGun) {
      PortShell::Defer(PortShell::kBody, s.beamData->GetBounds(gun), [pmgr, gun, lights] {
        if (s.beamData) {
          s.beamData->Render(*pmgr, gun, lights, CModelFlags::Normal());
        }
      });
    }
  } else if (phazon) {
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

std::string DescribeClips() {
  if (!Ready()) {
    return "body not ready";
  }
  BuildSets(true);
  std::string out;
  char line[200];
  const auto list = [&](uint32_t id, const CAnimData& data) {
    for (const auto& info : data.GetCharacterInfo().PortAnimInfo()) {
      const int i = info.first;
      std::snprintf(line, sizeof(line), "%08X %2d %-40s %6.3fs %6.3f u/s\n", id, i,
                    info.second.second.data(), data.GetAnimationDuration(i),
                    data.GetAverageVelocity(i));
      out += line;
    }
  };
  list(s.ancs, *s.body->GetAnimationData());
  for (const auto& e : s.sets) {
    list(e.first, *e.second->GetAnimationData());
  }
  return out + s.setNotes;
}

std::string DescribeFeet() {
  if (!Ready()) {
    return "body not ready";
  }
  std::string out;
  char line[200];
  if (s.pinned.Valid()) {
    // The pinned pose's top bones (parent unposed): where the clip carries its root motion.
    Sample(s.pinned, s.pinPhase, sTmp);
    const CCharLayoutInfo& layout = *s.body->GetAnimationData()->GetCharLayoutInfo();
    for (const CSegId& seg : s.segs) {
      const CSegId parent = layout.GetOriginalParent(seg);
      if (parent != CSegId::Character() && s.posed[parent.val()]) {
        continue;
      }
      const CVector3f& o = sTmp.off[seg.val()];
      const CVector3f r = layout.GetFromParentUnrotated(seg);
      std::snprintf(line, sizeof(line), "top %d (parent %d) off %+.3f %+.3f %+.3f rest %+.3f %+.3f %+.3f\n",
                    seg.val(), parent.val(), o.GetX(), o.GetY(), o.GetZ(), r.GetX(), r.GetY(), r.GetZ());
      out += line;
    }
  }
  for (int i = 0; i < kLocoSlots; ++i) {
    std::snprintf(line, sizeof(line), "%-26s %s %5.2f u/s stride %5.2f peak %4.2f w %4.2f\n",
                  kLoco[i].a, s.loco[i].Valid() ? "ok " : "---", s.locoSpeed[i], s.locoStride[i],
                  s.locoPeak[i], s.locoWeight[i]);
    out += line;
  }
  static const char* const kStanceNames[] = {"idle", "stop", "loco", "jump", "fall", "land"};
  std::snprintf(line, sizeof(line), "stance %s air %.2f flinch %.2f scan %.2f; clips jump %d fall %d land %d flinch %d scan %d\n",
                kStanceNames[int(s.stance)], s.airTime, s.flinchTime, s.scanWeight,
                int(s.jump.Valid()), int(s.fall.Valid()), int(s.land.Valid()), int(s.flinch.Valid()),
                int(s.scan.Valid()));
  out += line;
  std::snprintf(line, sizeof(line), "legs %s yaw %.1f deg\n", s.legsBack ? "back" : "forward",
                s.legYaw * 57.29578f);
  out += line;
  for (int i = 0; i < s.changeCount; ++i) {
    const auto& c = s.changes[i];
    std::snprintf(line, sizeof(line), "  %8.2fs -> %s fwd %+.2f side %+.2f rise %+.2f\n", c.clock,
                  kStanceNames[int(c.to)], c.fwd, c.side, c.rise);
    out += line;
  }
  if (!s.aimLayer) {
    return out + "no aim layer\n";
  }
  for (int c = 0; c < 9; ++c) {
    std::snprintf(line, sizeof(line), "aim %d,%d clips %d+%d yaw %+6.1f pitch %+6.1f\n", c / 3, c % 3,
                  s.aim[c].clip[0], s.aim[c].clip[1], s.aimYaw[c] * 57.2958f, s.aimPitch[c] * 57.2958f);
    out += line;
  }
  std::snprintf(line, sizeof(line), "grid left %.1f right %.1f down %.1f up %.1f; now %+.2f,%+.2f w %.2f; shoulder turn %.1f miss %.1f; posed %+.1f/%+.1f aim %+.1f/%+.1f\n",
                s.gridLeft * 57.2958f, s.gridRight * 57.2958f, s.gridDown * 57.2958f,
                s.gridUp * 57.2958f, s.gridU, s.gridV, s.aimWeight, s.armTurn * 57.2958f,
                s.armMiss * 57.2958f, s.poseYaw * 57.2958f, s.posePitch * 57.2958f,
                s.aimYawNow * 57.2958f, s.aimPitchNow * 57.2958f);
  return out + line;
}

bool PinClip(const char* name, float phase, float yaw) {
  if (name == nullptr || !Ready()) {
    s.pinned = Clip();
    return name == nullptr;
  }
  BuildSets(true);
  const Clip clip = FindClip(name);
  if (!clip.Valid()) {
    return false;
  }
  s.pinned = clip;
  s.pinPhase = phase - std::floor(phase);
  s.pinYaw = yaw;
  return true;
}

} // namespace PortThirdPersonBody
