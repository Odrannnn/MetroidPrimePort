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
#include "MetroidPrime/Player/CGrappleArm.hpp"
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
// Standing: the relaxed ready, the pose the stop ends in (B_ready_samus, the cannon levelled,
// stands in a lunge side-on, and turning into it from the stop's end swung the body a quarter
// turn). Walking back keeps B_ready_samus above the hips (s.ready).
constexpr ClipNames kIdle{"B_rready_samus", "B_ready_samus"};
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
// How finely Measure samples the feet: over a walk loop, and over the Stop clip.
constexpr int kLeadSamples = 16;
constexpr int kStopSamples = 32;
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
  bool freeArm[kSegs] = {};   // the arm without the cannon: it swings with the walk while aiming
  CSegId hips = CSegId::Invalid(); // the top posed bone, which carries a clip's height
  float hipsHeight = 0.f;          // its height in the skeleton
  CSegId spine = CSegId::Invalid(); // Spine_1, the upper body's top bone
  int idle = -1;
  int ready = -1; // B_ready_samus, the cannon levelled: the backpedal's and walk-back's upper body
  int stop = -1;
  Clip loco[kLocoSlots];
  // Measured by Build: each walk clip's ground speed (world units per second), the ground one
  // loop covers, and where in the loop the left foot is furthest forward (Measure).
  float locoSpeed[kLocoSlots] = {};
  float locoStride[kLocoSlots] = {};
  float locoPeak[kLocoSlots] = {};
  // The left ankle's lead over the right along the facing (model units), over each walk clip's
  // loop and over the whole Stop clip (Measure; StopStart).
  float locoLead[kLocoSlots][kLeadSamples] = {};
  float stopLead[kStopSamples] = {};
  float stopStart = 0.f; // the Stop clip's phase this stop began at (StopStart)
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
  // The procedural backpedal (Backpedal): each leg's hip, knee, ankle and ball (left, right), their
  // lengths, and the ready pose's ankles and balls (model space, unscaled), measured by Build.
  CSegId leg[2][4] = {{CSegId::Invalid(), CSegId::Invalid(), CSegId::Invalid(), CSegId::Invalid()},
                      {CSegId::Invalid(), CSegId::Invalid(), CSegId::Invalid(), CSegId::Invalid()}};
  bool legs = false;
  float thigh[2] = {}, shin[2] = {};
  CVector3f restAnkle[2] = {CVector3f::Zero(), CVector3f::Zero()};
  CVector3f restBall[2] = {CVector3f::Zero(), CVector3f::Zero()}; // from the ankle
  float backLoop = 0.f;   // ground one backpedal loop covers (world units)
  float backDuty = kBackDuty; // the share of a loop each foot is planted, last pose
  float backWeight = 0.f; // the backpedal's share of the walk, eased
  float backCrouch = 0.f; // how far it lowered the hips last pose (model units; thirdperson feet)
  float stepAngle = 3.14159265f; // the way the procedural gait goes from the facing (+ right), eased
  float legYaw = 0.f;     // the legs' turn from the facing (LegYaw), eased
  float clock = 0.f;      // seconds, for the clips that play by time (ready, aim)
  float stanceTime[int(Stance::Count)] = {}; // seconds since each stance was last entered
  float airTime = 0.f;    // seconds off the ground
  float slowTime = 0.f;   // seconds under kStandSpeed
  float brakeTime = 0.f;  // seconds braking (kBrakeDecel), the time under kBrakeHold after it too
  float lastSpeed = 0.f;  // ground speed last tick
  // Where the player and the platform it rides stood last tick (GroundVelocity).
  bool haveLast = false;
  CVector3f lastPos = CVector3f::Zero(), lastPlatformPos = CVector3f::Zero();
  CVector3f lastVelocity = CVector3f::Zero();
  TUniqueId lastPlatform = kInvalidUniqueId;
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
  // The grapple: the other arm reaches for the grapple point while the beam is out (no Samus
  // ANCS has a grapple clip). Its shoulder and elbow turn; the hand is the beam's end.
  CSegId reachShoulder = CSegId::Invalid();
  CSegId reachElbow = CSegId::Invalid();
  CSegId reachHand = CSegId::Invalid();
  CQuaternion reachShoulderPose = CQuaternion::NoRotation();
  CQuaternion reachElbowPose = CQuaternion::NoRotation();
  CVector3f reachPoint = CVector3f::Zero();
  float reachWeight = 0.f;
  // The Phazon suit's indirect-texture pass (as CScriptPlayerActor's).
  rstl::optional_object<TCachedToken<CTexture>> phazonTex;
  float phazonAngle = 0.f;
  // The cannon.
  int beam = -1;
  std::unique_ptr<TToken<CModel>> beamModel;
  std::unique_ptr<CModelData> beamData;
  // The muzzle as Render last posed it, for the charge effect (TakeMuzzle).
  CTransform4f muzzle = CTransform4f::Identity();
  bool muzzleDrawn = false;
};
State s;
// The procedural backpedal: the walk-back clip covers 0.88 u/s, so at Samus's ~1.9 u/s backward
// it plays over twice its speed, and its stance stays splayed; the forward walks reversed swing
// the arms and land heel first, which reads as a moonwalk (#216).
BackWalk sBackWalk = BackWalk::Procedural;

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
  s.haveLast = false;
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

// The grapple arm: the left one, the hand's knuckles (L_middle_1) its end.
void FindReachBones(const CAnimData& data) {
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  s.reachShoulder = layout.GetSegIdFromString(rstl::string_l("L_shoulder"));
  s.reachElbow = layout.GetSegIdFromString(rstl::string_l("L_elbow"));
  s.reachHand = layout.GetSegIdFromString(rstl::string_l("L_middle_1"));
  if (s.reachShoulder == CSegId::Invalid() || s.reachElbow == CSegId::Invalid() ||
      s.reachHand == CSegId::Invalid() || s.reachShoulder == s.shoulder ||
      !IsAncestor(layout, s.reachShoulder, s.reachElbow) || !IsAncestor(layout, s.reachElbow, s.reachHand)) {
    s.reachShoulder = CSegId::Invalid();
  }
}

// Turns seg by the model-space turn over its local pose (composed as BuildTransform composes
// the parents' rotations, root first).
void TurnSeg(CHierarchyPoseBuilder& pb, const CCharLayoutInfo& layout, CSegId seg, const CQuaternion& pose,
             const CQuaternion& turn) {
  CQuaternion parentRot = CQuaternion::NoRotation();
  for (CSegId p = layout.GetOriginalParent(seg); p != CSegId::Character(); p = layout.GetOriginalParent(p)) {
    parentRot = pb.GetSegRotation(p) * parentRot;
  }
  pb.Insert(seg, parentRot.BuildInverted() * turn * parentRot * pose);
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

// Points the grapple arm at the grapple point by s.reachWeight: the shoulder puts the hand on the
// line to it, then the elbow straightens the forearm along it. fresh: the clip pose was rebuilt
// since the last call (the saved poses are stale).
void ReachArm(const CStateManager& mgr, CAnimData& data, bool fresh) {
  if (s.reachShoulder == CSegId::Invalid()) {
    return;
  }
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  if (fresh) {
    s.reachShoulderPose = pb.GetSegRotation(s.reachShoulder);
    s.reachElbowPose = pb.GetSegRotation(s.reachElbow);
  }
  pb.Insert(s.reachShoulder, s.reachShoulderPose);
  pb.Insert(s.reachElbow, s.reachElbowPose);
  data.SetPoseBuilderValid(false);
  if (s.reachWeight <= 0.f) {
    return;
  }
  const CPlayer& player = *mgr.GetPlayer();
  CTransform4f bodyXf = player.GetTransform();
  CVector3f origin = bodyXf.GetTranslation();
  origin.SetZ(player.GetBoundingBox().GetMinPoint().GetZ());
  bodyXf.SetTranslation(origin);
  const CVector3f target = bodyXf.TransposeMultiply(s.reachPoint) * (1.f / kBodyScale);
  const auto position = [&pb](CSegId seg) {
    CTransform4f xf = CTransform4f::Identity();
    pb.BuildTransform(seg, xf);
    return xf.GetTranslation();
  };
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  const CVector3f shoulder = position(s.reachShoulder);
  const CVector3f elbow = position(s.reachElbow);
  const CVector3f hand = position(s.reachHand);
  const CVector3f upper = elbow - shoulder;
  const CVector3f fore = hand - elbow;
  const CVector3f line = target - shoulder;
  if (!upper.CanBeNormalized() || !fore.CanBeNormalized() || !line.CanBeNormalized()) {
    return;
  }
  // Two-bone IK: the hand on the line to the point, the arm nearly straight, the elbow bent down
  // and out (toward the hint) whatever the clip had the arm doing. One shortest arc from the clip's
  // arm, often hanging back, twisted the whole arm about and turned it as the clip moved.
  const float l1 = upper.Magnitude();
  const float l2 = fore.Magnitude();
  const CVector3f dir = line.AsNormalized();
  const float reach = std::min(kReachStraight * (l1 + l2), line.Magnitude());
  const float along = (l1 * l1 - l2 * l2 + reach * reach) / (2.f * reach);
  const float out = std::sqrt(std::max(l1 * l1 - along * along, 0.f));
  const CVector3f hint(shoulder.GetX() < 0.f ? -kReachElbowOut : kReachElbowOut, 0.f, -1.f);
  CVector3f pole = hint - dir * CVector3f::Dot(hint, dir);
  if (!pole.CanBeNormalized()) {
    pole = CVector3f(shoulder.GetX() < 0.f ? -1.f : 1.f, 0.f, 0.f);
  }
  pole = pole.AsNormalized();
  const CVector3f wantElbow = shoulder + dir * along + pole * out;
  const CVector3f wantHand = shoulder + dir * reach;
  // The shoulder carries the upper arm onto the new elbow and turns the bend into the new plane.
  const CVector3f wantUpper = (wantElbow - shoulder).AsNormalized();
  CQuaternion turn = CQuaternion::ShortestRotationArc(upper.AsNormalized(), wantUpper);
  const CVector3f bend = turn.Transform(fore);
  const CVector3f bendSide = bend - wantUpper * CVector3f::Dot(bend, wantUpper);
  const CVector3f wantFore = wantHand - wantElbow;
  const CVector3f wantSide = wantFore - wantUpper * CVector3f::Dot(wantFore, wantUpper);
  if (bendSide.Magnitude() > 0.05f * l2 && wantSide.CanBeNormalized()) {
    turn = CQuaternion::ShortestRotationArc(bendSide.AsNormalized(), wantSide.AsNormalized()) * turn;
  }
  TurnSeg(pb, layout, s.reachShoulder, s.reachShoulderPose,
          CQuaternion::Slerp(CQuaternion::NoRotation(), turn, s.reachWeight));
  // The elbow then hinges the hand onto the line.
  const CVector3f pivot = position(s.reachElbow);
  const CVector3f nowFore = position(s.reachHand) - pivot;
  const CVector3f toHand = wantHand - pivot;
  if (nowFore.CanBeNormalized() && toHand.CanBeNormalized()) {
    const CQuaternion hinge = CQuaternion::ShortestRotationArc(nowFore.AsNormalized(), toHand.AsNormalized());
    TurnSeg(pb, layout, s.reachElbow, s.reachElbowPose,
            CQuaternion::Slerp(CQuaternion::NoRotation(), hinge, s.reachWeight));
  }
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
Pose sUpper;
Pose sWalkArm;

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
    std::fill(std::begin(s.locoLead[i]), std::end(s.locoLead[i]), 0.f);
    if (!clip.Valid()) {
      continue;
    }
    s.locoSpeed[i] = std::fabs(clip.data->GetAverageVelocity(clip.index)) * kBodyScale;
    s.locoStride[i] = s.locoSpeed[i] * clip.data->GetAnimationDuration(clip.index);
    if (lFoot == CSegId::Invalid() || rFoot == CSegId::Invalid()) {
      continue;
    }
    // The left ankle's lead over the right along the facing, over one loop.
    float* lead = s.locoLead[i];
    for (int k = 0; k < kLeadSamples; ++k) {
      Sample(clip, float(k) / kLeadSamples, sTmp);
      lead[k] = SegPosition(*clip.data, lFoot).GetY() - SegPosition(*clip.data, rFoot).GetY();
    }
    s.locoPeak[i] = CurvePeak(lead, kLeadSamples);
  }
  std::fill(std::begin(s.stopLead), std::end(s.stopLead), 0.f);
  if (s.stop >= 0 && lFoot != CSegId::Invalid() && rFoot != CSegId::Invalid()) {
    for (int k = 0; k < kStopSamples; ++k) {
      Sample(data, s.stop, float(k) / kStopSamples, sTmp);
      s.stopLead[k] = SegPosition(data, lFoot).GetY() - SegPosition(data, rFoot).GetY();
    }
  }

  s.stopStart = 0.f;

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

// The procedural backpedal's shape, in leg lengths (hip to ankle): how high a swinging foot
// lifts, how far the pelvis bobs, and the least it crouches; the torso's forward lean (radians).
constexpr float kBackLift = 0.1f;
constexpr float kBackBob = 0.02f;
constexpr float kBackCrouch = 0.03f;
constexpr float kBackReach = 0.96f; // the longest a leg stretches
constexpr float kBackLean = 0.1f;
// The furthest a planted foot travels either side of under her, in leg lengths, and the most
// loops a second (faster, the loop lengthens; past what a foot covers planted, she runs).
constexpr float kBackHalf = 0.3f;
constexpr float kBackCadence = 3.f;
// Sidestepping (the gait across the facing; between, mixed by its share across), the planted
// foot's travel, the loop as a share of the forward walk's stride, and the most loops a second.
// The feet stay kStepGap legs apart at least (StepWiden).
constexpr float kSideHalf = 0.32f;
constexpr float kSideLoop = 0.8f;
constexpr float kSideCadence = 2.4f;
constexpr float kStepGap = 0.12f;

// The legs' bones and lengths, and the ready pose's feet, for Backpedal (s.legs if all there).
void FindLegBones(CAnimData& data) {
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  static const char* const kNames[2][4] = {{"L_hip", "L_knee", "L_ankle", "L_ball"},
                                           {"R_hip", "R_knee", "R_ankle", "R_ball"}};
  s.legs = s.ready >= 0 && s.hips != CSegId::Invalid();
  for (int f = 0; f < 2; ++f) {
    for (int b = 0; b < 4; ++b) {
      s.leg[f][b] = layout.GetSegIdFromString(rstl::string_l(kNames[f][b]));
      s.legs = s.legs && s.leg[f][b] != CSegId::Invalid() && s.leg[f][b].val() < kSegs && s.posed[s.leg[f][b].val()];
    }
    s.legs = s.legs && IsAncestor(layout, s.leg[f][0], s.leg[f][1]) && IsAncestor(layout, s.leg[f][1], s.leg[f][2]) &&
             IsAncestor(layout, s.leg[f][2], s.leg[f][3]);
  }
  if (!s.legs) {
    return;
  }
  Sample(data, s.ready, 0.f, sTmp);
  for (int f = 0; f < 2; ++f) {
    const CVector3f hip = SegPosition(data, s.leg[f][0]);
    const CVector3f knee = SegPosition(data, s.leg[f][1]);
    s.restAnkle[f] = SegPosition(data, s.leg[f][2]);
    s.restBall[f] = SegPosition(data, s.leg[f][3]) - s.restAnkle[f];
    s.thigh[f] = (knee - hip).Magnitude();
    s.shin[f] = (s.restAnkle[f] - knee).Magnitude();
    s.legs = s.legs && s.thigh[f] > 1e-3f && s.shin[f] > 1e-3f;
  }
}

// seg's rotation in the model, as the pose builder holds it now.
CQuaternion ModelRotation(const CHierarchyPoseBuilder& pb, const CCharLayoutInfo& layout, CSegId seg) {
  CQuaternion rot = pb.GetSegRotation(seg);
  for (CSegId p = layout.GetOriginalParent(seg); p != CSegId::Character(); p = layout.GetOriginalParent(p)) {
    rot = pb.GetSegRotation(p) * rot;
  }
  return rot;
}

// Walking any way but forward, posed rather than sampled (no clip walks backward well, #216, and
// none sidesteps): the ready pose above the hips, the cannon held, and below them each foot on its
// BackpedalFoot path along the way she goes (s.stepAngle), planted feet sliding the other way under
// her at her ground speed (s.stride runs a loop per s.backLoop of ground). Two-bone IK puts each
// ankle there, its knee bent forward; the hips crouch so the stretched leg reaches, and bob at
// each step. Sidestepping, the feet stand wider so they never cross (StepWiden), and land flat.
void Backpedal(CAnimData& data, Pose& out) {
  const float readyDuration = std::max(data.GetAnimationDuration(s.ready), 0.1f);
  Sample(data, s.ready, Wrap(s.clock / readyDuration), out);
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  const CCharLayoutInfo& layout = *data.GetCharLayoutInfo();
  const float leg = 0.5f * (s.thigh[0] + s.shin[0] + s.thigh[1] + s.shin[1]);
  // The way she goes in the model (x right, y forward).
  const float dirX = std::sin(s.stepAngle);
  const float dirY = std::cos(s.stepAngle);
  const float across = std::fabs(dirX);
  // A foot is planted while it covers 2 * kBackHalf legs of ground (kSideHalf across), at most
  // kBackDuty of a loop.
  const float maxHalf = kBackHalf + (kSideHalf - kBackHalf) * across;
  const float duty = std::clamp(2.f * maxHalf * leg * kBodyScale / std::max(s.backLoop, 0.1f), 0.2f, kBackDuty);
  s.backDuty = duty;
  // Left foot furthest forward at stride 0, as the walk clips are at their peaks.
  const float phaseL = Wrap(duty - s.stride);
  const float phase[2] = {phaseL, Wrap(phaseL + 0.5f)};
  const float half = duty * s.backLoop / (2.f * kBodyScale);
  const float midY = 0.5f * (s.restAnkle[0].GetY() + s.restAnkle[1].GetY());
  const float widen = StepWiden(dirX, half, std::fabs(s.restAnkle[0].GetX() - s.restAnkle[1].GetX()), kStepGap * leg);
  const float outX[2] = {s.restAnkle[0].GetX() < s.restAnkle[1].GetX() ? -widen : widen,
                         s.restAnkle[1].GetX() < s.restAnkle[0].GetX() ? -widen : widen};
  // Running, the feet lift higher.
  const float lift = kBackLift * leg * (1.f + 4.f * (kBackDuty - duty));

  // Crouch so the leg furthest from under its hip still reaches.
  float crouch = kBackCrouch * leg;
  for (int f = 0; f < 2; ++f) {
    const CVector3f hip = SegPosition(data, s.leg[f][0]);
    const float reach = kBackReach * (s.thigh[f] + s.shin[f]);
    for (const float end : {-1.f, 1.f}) {
      const float dx = hip.GetX() - s.restAnkle[f].GetX() - outX[f] + end * dirX * half;
      const float dy = hip.GetY() - midY + end * dirY * half;
      const float dz = std::sqrt(std::max(reach * reach - dx * dx - dy * dy, 0.f));
      crouch = std::max(crouch, hip.GetZ() - s.restAnkle[f].GetZ() - dz);
    }
  }
  s.backCrouch = crouch;
  CVector3f hipsOff = pb.GetSegOffset(s.hips);
  hipsOff.SetZ(hipsOff.GetZ() - crouch - kBackBob * leg * BackpedalBob(phaseL, duty));
  pb.Insert(s.hips, hipsOff);

  for (int f = 0; f < 2; ++f) {
    const CSegId hipSeg = s.leg[f][0];
    const CSegId kneeSeg = s.leg[f][1];
    const CSegId ankleSeg = s.leg[f][2];
    BackFoot foot = BackpedalFoot(phase[f], duty);
    // Stepping back it lands toe first; to the side flat.
    foot.pitch *= std::max(-dirY, 0.f);
    // The ankle's place: the foot turns about its ball landing toe first, about its heel (under
    // the ankle) rolling up onto the toe.
    const CVector3f& ball = s.restBall[f];
    const CVector3f pivot = foot.pitch < 0.f ? ball : CVector3f(0.f, -0.35f * std::fabs(ball.GetY()), -s.restAnkle[f].GetZ());
    const CVector3f roll = pivot - CQuaternion::XRotation(CRelAngle::FromRadians(foot.pitch)).Transform(pivot);
    const CVector3f target(s.restAnkle[f].GetX() + outX[f] - dirX * foot.y * half, midY - dirY * foot.y * half + roll.GetY(),
                           s.restAnkle[f].GetZ() + foot.lift * lift + roll.GetZ());
    const CQuaternion ankleReady = ModelRotation(pb, layout, ankleSeg);

    // The knee bends to the hip-to-target distance.
    {
      const CVector3f hip = SegPosition(data, hipSeg);
      const CVector3f knee = SegPosition(data, kneeSeg);
      const CVector3f ankle = SegPosition(data, ankleSeg);
      const CVector3f up = hip - knee;
      const CVector3f down = ankle - knee;
      const float reach = s.thigh[f] + s.shin[f];
      const float d = std::clamp((target - hip).Magnitude(), 0.05f * reach, 0.999f * reach);
      const float want = KneeAngle(s.thigh[f], s.shin[f], d);
      float now = std::acos(std::clamp(CVector3f::Dot(up.AsNormalized(), down.AsNormalized()), -1.f, 1.f));
      CVector3f axis = CVector3f::Cross(up, down);
      if (!axis.CanBeNormalized() || axis.GetX() <= 0.f) {
        // Straight or bent backward: about the model's right, the way a knee bends.
        now = axis.CanBeNormalized() ? 2.f * 3.14159265f - now : 3.14159265f;
        axis = CVector3f(1.f, 0.f, 0.f);
      }
      TurnSeg(pb, layout, kneeSeg, pb.GetSegRotation(kneeSeg),
              CQuaternion::AxisAngle(CUnitVector3f(axis.AsNormalized()), CRelAngle::FromRadians(want - now)));
    }
    // The hip swings the ankle onto the target, then twists the knee forward (a little out).
    const CVector3f hip = SegPosition(data, hipSeg);
    const CVector3f toAnkle = SegPosition(data, ankleSeg) - hip;
    const CVector3f toTarget = target - hip;
    if (toAnkle.CanBeNormalized() && toTarget.CanBeNormalized()) {
      TurnSeg(pb, layout, hipSeg, pb.GetSegRotation(hipSeg),
              CQuaternion::ShortestRotationArc(toAnkle.AsNormalized(), toTarget.AsNormalized()));
      const CVector3f line = toTarget.AsNormalized();
      const CVector3f kneeDir = SegPosition(data, kneeSeg) - hip;
      const CVector3f pole(s.restAnkle[f].GetX() < 0.f ? -0.2f : 0.2f, 1.f, 0.f);
      const CVector3f a = kneeDir - line * CVector3f::Dot(kneeDir, line);
      const CVector3f b = pole - line * CVector3f::Dot(pole, line);
      if (a.CanBeNormalized() && b.CanBeNormalized()) {
        TurnSeg(pb, layout, hipSeg, pb.GetSegRotation(hipSeg),
                CQuaternion::ShortestRotationArc(a.AsNormalized(), b.AsNormalized()));
      }
    }
    // The foot keeps the ready pose's orientation in the model, pitched.
    const CQuaternion ankleNow = ModelRotation(pb, layout, ankleSeg);
    TurnSeg(pb, layout, ankleSeg, pb.GetSegRotation(ankleSeg),
            CQuaternion::XRotation(CRelAngle::FromRadians(foot.pitch)) * ankleReady * ankleNow.BuildInverted());
    for (int b = 0; b < 3; ++b) {
      out.rot[s.leg[f][b].val()] = pb.GetSegRotation(s.leg[f][b]);
    }
  }
  out.off[s.hips.val()] = hipsOff;
}

// Poses the body into its pose builder: the legs' stances blended by their weights (the walk
// clips by speed, in step), then, with the cannon raised, the aim clips over the upper body.
// The forward walk's left-ankle lead at stride, as a share of its widest (-1..1), its clips
// mixed by their weights; false when no forward clip walks.
bool WalkLead(float stride, float& lead) {
  float sum = 0.f, amp = 0.f;
  for (int i = 0; i < kLocoSlots; ++i) {
    if (i == kWalkBack || s.locoWeight[i] <= 0.f || !s.loco[i].Valid()) {
      continue;
    }
    const float* curve = s.locoLead[i];
    const float x = Wrap(kLocoDir[i] * stride + s.locoPeak[i]) * kLeadSamples;
    const int k = int(x) % kLeadSamples;
    const float t = x - std::floor(x);
    sum += s.locoWeight[i] * (curve[k] + (curve[(k + 1) % kLeadSamples] - curve[k]) * t);
    amp += s.locoWeight[i] * std::fabs(*std::max_element(curve, curve + kLeadSamples,
                                                         [](float a, float b) { return std::fabs(a) < std::fabs(b); }));
  }
  if (amp <= 0.f) {
    return false;
  }
  lead = sum / amp;
  return true;
}

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
    Sample(data, s.stop, std::min(s.stopStart + s.stanceTime[int(Stance::Stop)] / duration, 0.999f), sTmp);
    Mix(sAcc, total, sTmp, s.stanceWeight[int(Stance::Stop)], false);
  }
  const float backWeight = s.stanceWeight[int(Stance::Loco)] * s.backWeight;
  if (backWeight > 0.f) {
    Backpedal(data, sTmp);
    Mix(sAcc, total, sTmp, backWeight, false);
  }
  for (int i = 0; i < kLocoSlots; ++i) {
    const float w = s.stanceWeight[int(Stance::Loco)] * s.locoWeight[i] * (1.f - s.backWeight);
    if (w > 0.f) {
      Sample(s.loco[i], Wrap(kLocoDir[i] * s.stride + s.locoPeak[i]), sTmp);
      if (kLocoDir[i] < 0.f) {
        // The walk-back is Samus struggling in the Phazon: its arms clutch the helmet and
        // fling out. Only its legs; above them the ready pose.
        Sample(data, s.ready, Wrap(s.clock / std::max(data.GetAnimationDuration(s.ready), 0.1f)), sUpper);
        Overlay(sTmp, sUpper, 1.f, true);
      }
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
      // Walking, the free arm keeps the walk's swing under the aimed shoulders.
      const float freeAim = s.aimWeight * (1.f - s.stanceWeight[int(Stance::Loco)] * (1.f - s.backWeight));
      sWalkArm = sAcc;
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
      if (freeAim < s.aimWeight) {
        for (const CSegId& seg : s.segs) {
          const int i = seg.val();
          if (s.freeArm[i]) {
            sAcc.rot[i] = Blend(sWalkArm.rot[i], sAcc.rot[i], s.aimWeight > 0.f ? freeAim / s.aimWeight : 0.f);
            sAcc.off[i] = sWalkArm.off[i] * (1.f - freeAim) + sArms.off[i] * freeAim;
          }
        }
      }
    }
  }
  // Backpedalling, the torso leans forward over the feet (over the aim, which AimArm corrects).
  if (backWeight > 0.f && s.spine != CSegId::Invalid()) {
    ModelRotations(data, sAcc, sModel);
    const int sp = s.spine.val();
    const CQuaternion parent = sModel.rot[sp] * sAcc.rot[sp].BuildInverted();
    const CQuaternion lean =
        CQuaternion::XRotation(CRelAngle::FromRadians(-kBackLean * backWeight * std::max(-std::cos(s.stepAngle), 0.f)));
    sAcc.rot[sp] = parent.BuildInverted() * lean * sModel.rot[sp];
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
  s.ready = data->GetCharacterInfo().GetAnimationIndex(rstl::string_l(kReady));
  if (s.ready < 0) {
    s.ready = s.idle;
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
  FindReachBones(*data);
  std::fill(std::begin(s.freeArm), std::end(s.freeArm), false);
  for (const char* name : {"L_shoulder", "R_shoulder"}) {
    CSegId arm = layout.GetSegIdFromString(rstl::string_l(name));
    if (arm != CSegId::Invalid() && s.shoulder != CSegId::Invalid() && arm != s.shoulder) {
      // From where the arm leaves the spine: the aim clips raise the collarbone too.
      for (int depth = 0; depth < 64; ++depth) {
        const CSegId parent = layout.GetOriginalParent(arm);
        if (!s.upper[parent.val()] || IsAncestor(layout, parent, s.shoulder)) {
          break;
        }
        arm = parent;
      }
      for (const CSegId& seg : s.segs) {
        s.freeArm[seg.val()] = s.upper[seg.val()] && (seg == arm || IsAncestor(layout, arm, seg));
      }
      break;
    }
  }
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
  FindLegBones(*data);
  s.backWeight = 0.f;
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

// Faster than this (world units per second) the player warped rather than moved.
constexpr float kWarpSpeed = 60.f;
// On the ground the run tops out near 13 and dashes leave the ground, so faster over it is a
// short warp too (a debug tp nearby read 32), which would otherwise brake into a Stop.
constexpr float kWarpGroundSpeed = 20.f;

// How fast the player goes over the ground: how far it moved since last tick, less the ride of
// the platform it stands on. Not its velocity: walking into a wall that swings between the
// walk's speed and none every tick, the collision undoing each step, while the player stays put.
CVector3f GroundVelocity(const CStateManager& mgr, const CPlayer& player, float dt) {
  const CVector3f pos = player.GetTranslation();
  const TUniqueId platformId = player.GetRidingPlatformId();
  const CActor* platform =
      platformId != kInvalidUniqueId ? TCastToConstPtr< CActor >(mgr.GetObjectById(platformId)) : nullptr;
  const CVector3f platformPos = platform != nullptr ? platform->GetTranslation() : CVector3f::Zero();
  if (s.haveLast && dt <= 0.f) {
    return s.lastVelocity;
  }
  CVector3f v = CVector3f::Zero();
  if (s.haveLast) {
    CVector3f moved = pos - s.lastPos;
    if (platform != nullptr && platformId == s.lastPlatform) {
      moved = moved - (platformPos - s.lastPlatformPos);
    }
    v = moved * (1.f / dt);
    const bool grounded = player.GetPlayerMovementState() == NPlayer::kMS_OnGround;
    if (v.Magnitude() > kWarpSpeed ||
        (grounded && v.GetX() * v.GetX() + v.GetY() * v.GetY() > kWarpGroundSpeed * kWarpGroundSpeed)) {
      v = CVector3f::Zero();
    }
  }
  s.haveLast = true;
  s.lastPos = pos;
  s.lastPlatform = platform != nullptr ? platformId : kInvalidUniqueId;
  s.lastPlatformPos = platformPos;
  s.lastVelocity = v;
  return v;
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
    s.haveLast = false;
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
    s.reachWeight = 0.f;
    s.haveLast = false;
    return;
  }
  const CTransform4f& xf = player.GetTransform();
  const CVector3f vel = GroundVelocity(mgr, player, dt);
  const float fwd = CVector3f::Dot(vel, xf.GetForward());
  const float side = CVector3f::Dot(vel, xf.GetRight());
  const bool grounded = player.GetPlayerMovementState() == NPlayer::kMS_OnGround;
  s.gunUp = std::max(s.gunUp - dt, 0.f);
  const bool gunUp = s.gunUp > 0.f || player.GetPlayerGun()->IsCharging();

  // The legs. Every stance fades in over kBlendSeconds while the others fade out.
  const float stanceDone = s.stance == Stance::Stop
                               ? s.body->GetAnimationData()->GetAnimationDuration(s.stop) * (1.f - s.stopStart)
                           : s.stance == Stance::Land && s.land.Valid() ? ShotSeconds(s.land, kLand)
                                                                         : 0.f;
  const float speed = std::sqrt(fwd * fwd + side * side);
  s.slowTime = speed < kStandSpeed ? s.slowTime + dt : 0.f;
  if (dt > 0.f) {
    // Only a walk brakes: kept through idle, the hold would stop the next walk as it starts.
    const bool walking = s.stance == Stance::Loco || s.stance == Stance::Stop;
    const bool slowing = speed <= s.lastSpeed - kBrakeDecel * dt;
    s.brakeTime = walking && (slowing || (speed < kBrakeHold && s.brakeTime > 0.f)) ? s.brakeTime + dt : 0.f;
    s.lastSpeed = speed;
  }
  const bool braking = grounded && s.brakeTime >= kBrakeSeconds && speed < kBrakeSpeed;
  const Stance stance = ChooseStance(fwd, side, grounded, vel.GetZ(), s.stance,
                                     s.stanceTime[int(s.stance)] >= stanceDone,
                                     s.airTime >= kLandAirSeconds, s.slowTime >= kSettleSeconds,
                                     braking);
  if (stance == Stance::Loco && s.stance == Stance::Stop) {
    s.brakeTime = 0.f; // walking on after a stop played out: only a new slowdown brakes
  }
  if (stance == Stance::Stop && stance != s.stance) {
    // Stepping out of a forward walk, the Stop clip starts where its feet carry on from the walk's.
    float lead = 0.f, ahead = 0.f;
    s.stopStart = s.stance == Stance::Loco && !s.legsBack && s.backWeight < 0.5f &&
                          WalkLead(s.stride, lead) && WalkLead(s.stride + 0.01f, ahead)
                      ? StopStart(lead, ahead > lead, s.stopLead, kStopSamples)
                      : 0.f;
  }
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
  // Procedural: every way but the front steps (Backpedal), the hips facing forward.
  const bool procedural = sBackWalk == BackWalk::Procedural && s.legs;
  if (stance == Stance::Loco) {
    s.legsBack = procedural ? Stepping(fwd, side, s.legsBack) : LegsBackward(fwd, side, s.legsBack);
    if (s.legsBack && procedural) {
      const float want = std::atan2(side, fwd);
      const float turn = std::remainder(want - s.stepAngle, 2.f * 3.14159265f);
      s.stepAngle = s.backWeight <= 0.f ? want
                                        : std::remainder(s.stepAngle + std::clamp(turn, -kLegTurnSpeed * dt, kLegTurnSpeed * dt),
                                                         2.f * 3.14159265f);
    }
    LocoSlot tier[4];
    int n = 0;
    const auto add = [&](const LocoSlot* slots, int count) {
      for (int i = 0; i < count; ++i) {
        if (s.loco[slots[i]].Valid()) {
          tier[n++] = slots[i];
        }
      }
    };
    if (s.legsBack && sBackWalk == BackWalk::Struggle) {
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
  const float legYaw = stance == Stance::Loco && !(s.legsBack && procedural) ? LegYaw(fwd, side, s.legsBack) : 0.f;
  s.legYaw = s.legYaw + std::clamp(legYaw - s.legYaw, -kLegTurnSpeed * dt, kLegTurnSpeed * dt);
  // Stepping posed (Backpedal): a loop is half a walk's backward (kSideLoop of it sideways), the
  // steps short and quick, and lengthens once that would step more than kBackCadence
  // (kSideCadence) loops a second.
  const float across = std::fabs(std::sin(s.stepAngle));
  s.backLoop = std::max((0.5f + (kSideLoop - 0.5f) * across) * s.locoStride[kWalk],
                        speed / (kBackCadence + (kSideCadence - kBackCadence) * across));
  const bool backpedal = s.legsBack && procedural && s.backLoop > 0.1f;
  s.backWeight = Approach(s.backWeight, backpedal ? 1.f : 0.f, dt / kBlendSeconds);
  float stride = 0.f;
  for (int i = 0; i < kLocoSlots; ++i) {
    stride += s.locoWeight[i] * s.locoStride[i];
  }
  stride += (s.backLoop - stride) * s.backWeight;
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

  // The grapple arm reaches from the first-person arm's wind-up on, so it is up when the beam
  // leaves the hand, and while the beam is out.
  const CGrappleArm& arm = player.GetPlayerGun()->GetGrappleArm();
  const bool reach = arm.GetActive() && (arm.BeamActive() || player.GetGrappleState() != CPlayer::kGS_None);
  if (arm.BeamActive()) {
    s.reachPoint = arm.PortGrapplePointPos();
  } else if (const CActor* point = TCastToConstPtr< CActor >(mgr.GetObjectById(player.GetOrbitTargetId()));
             reach && point != nullptr) {
    s.reachPoint = point->GetTranslation();
  }
  s.reachWeight = reach ? std::min(s.reachWeight + dt / kReachInSeconds, 1.f)
                        : std::max(s.reachWeight - dt / kReachOutSeconds, 0.f);
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
    const bool fresh = s.advanced;
    AimArm(mgr, data);
    ReachArm(mgr, data, fresh);
    s.advanced = false;
    if (s.phazonTex) {
      gpRender->AllocatePhazonSuitMaskTexture();
    }
  }
}

// The cannon's muzzle for the body at `xf`, facing down the barrel: from the wrist (GUN_LCTR)
// toward Charge_LCTR, at the far end of the drawn cannon (Charge_LCTR sits mid-barrel on it), or
// at Charge_LCTR with no cannon drawn.
CTransform4f Muzzle(const CTransform4f& xf) {
  const CTransform4f gun = xf * s.body->GetScaledLocatorTransform("GUN_LCTR");
  const CVector3f charge = (xf * s.body->GetScaledLocatorTransform("Charge_LCTR")).GetTranslation();
  CVector3f dir = charge - gun.GetTranslation();
  if (dir.Magnitude() < 0.01f) {
    return CTransform4f::Translate(charge);
  }
  dir = dir.AsNormalized();
  CVector3f tip = charge;
  if (s.beamData && !PortDreadSamus::Replaces(PortDreadSamus::CutsceneBody(s.loadedChar))) {
    // The farthest the cannon's box reaches along the barrel, in the cannon's own frame.
    const CVector3f d = gun.TransposeRotate(dir);
    const CAABox box = s.beamData->GetBounds();
    float reach = 0.f;
    for (int i = 0; i < 3; ++i) {
      reach += std::max(box.GetMinPoint()[i] * d[i], box.GetMaxPoint()[i] * d[i]);
    }
    tip = gun.GetTranslation() + dir * reach;
  }
  return CTransform4f::LookAt(tip, tip + dir);
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
  // A camera pulled in by a wall behind her ends up in her: the body fades as it nears.
  float cam[3];
  PortThirdPerson::GetCameraPosition(cam);
  const CAABox box = s.body->GetBounds(xf);
  float near2 = 0.f;
  for (int i = 0; i < 3; ++i) {
    const float d = std::max({box.GetMinPoint()[i] - cam[i], cam[i] - box.GetMaxPoint()[i], 0.f});
    near2 += d * d;
  }
  const float fade = std::clamp(std::sqrt(near2) / kFadeDistance, kFadeAlpha, 1.f);
  const CModelFlags flags = fade < 1.f ? CModelFlags(CModelFlags::kT_Blend, fade) : CModelFlags::Normal();
  s.body->Touch(mgr, 0);
  s.body->Render(mgr, xf, lights, flags);
  // Prime's cannon at the hand locator, unless the dread-samus mod's body carries its own.
  const bool drawGun = s.beamData && !PortDreadSamus::Replaces(PortDreadSamus::CutsceneBody(s.loadedChar));
  const CTransform4f gun = drawGun ? xf * s.body->GetScaledLocatorTransform("GUN_LCTR") : xf;
  if (drawGun) {
    s.beamData->Touch(mgr, 0);
    s.beamData->Render(mgr, gun, lights, flags);
  }
  s.muzzle = Muzzle(xf);
  s.muzzleDrawn = true;
  if (shell) {
    // The lights are the player's, alive until the frame's Flush; the body is this file's.
    const CStateManager* const pmgr = &mgr;
    PortShell::Defer(PortShell::kBody, box, [pmgr, xf, lights, flags] {
      if (s.body) {
        s.body->Render(*pmgr, xf, lights, flags);
      }
    });
    if (drawGun) {
      PortShell::Defer(PortShell::kBody, s.beamData->GetBounds(gun), [pmgr, gun, lights, flags] {
        if (s.beamData) {
          s.beamData->Render(*pmgr, gun, lights, flags);
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
  // Where the charge forms (Render, TakeMuzzle).
  out = Muzzle(bodyXf).GetTranslation();
  return true;
}

bool TakeMuzzle(CTransform4f& out) {
  const bool drawn = s.muzzleDrawn && Ready();
  s.muzzleDrawn = false;
  if (drawn) {
    out = s.muzzle;
  }
  return drawn;
}

bool GrappleHand(const CPlayer& player, CVector3f& out) {
  if (!PortThirdPerson::Active() || !Ready() || s.reachShoulder == CSegId::Invalid()) {
    return false;
  }
  CTransform4f bodyXf = player.GetTransform();
  CVector3f origin = bodyXf.GetTranslation();
  origin.SetZ(player.GetBoundingBox().GetMinPoint().GetZ());
  bodyXf.SetTranslation(origin);
  out = (bodyXf * s.body->GetScaledLocatorTransform("L_middle_1")).GetTranslation();
  return true;
}

void Release() { Drop(); }

std::string DescribeClips() {
  if (!Ready()) {
    return "body not ready\n";
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
    return "body not ready\n";
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
  std::snprintf(line, sizeof(line), "stop start %.2f\n", s.stopStart);
  out += line;
  static const char* const kStanceNames[] = {"idle", "stop", "loco", "jump", "fall", "land"};
  std::snprintf(line, sizeof(line), "stance %s air %.2f flinch %.2f scan %.2f; clips jump %d fall %d land %d flinch %d scan %d\n",
                kStanceNames[int(s.stance)], s.airTime, s.flinchTime, s.scanWeight,
                int(s.jump.Valid()), int(s.fall.Valid()), int(s.land.Valid()), int(s.flinch.Valid()),
                int(s.scan.Valid()));
  out += line;
  std::snprintf(line, sizeof(line), "legs %s yaw %.1f deg step %.1f deg\n", s.legsBack ? "back" : "forward",
                s.legYaw * 57.29578f, s.stepAngle * 57.29578f);
  out += line;
  std::snprintf(line, sizeof(line),
                "backpedal %s weight %.2f loop %.2f u duty %.2f crouch %.3f legs %d thigh %.3f/%.3f shin %.3f/%.3f\n",
                sBackWalk == BackWalk::Procedural ? "procedural"
                : sBackWalk == BackWalk::Struggle ? "struggle"
                                                  : "reverse",
                s.backWeight, s.backLoop, s.backDuty, s.backCrouch, int(s.legs), s.thigh[0], s.thigh[1], s.shin[0], s.shin[1]);
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

void SetBackWalk(BackWalk mode) { sBackWalk = mode; }
BackWalk GetBackWalk() { return sBackWalk; }

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
