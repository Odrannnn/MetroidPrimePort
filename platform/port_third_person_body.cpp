// The third-person camera's animated body; see port_third_person_body.h.
#include "port_third_person_body.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "port_dread_samus_mod.h"
#include "port_third_person.h"

#include "Kyoto/Animation/CCharLayoutInfo.hpp"
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
// The walk clips, slowest first (LocoWeights). B_walkgun_samus is not one: the aim clips over
// the legs raise the cannon at any speed.
constexpr ClipNames kLoco[kLocoClips] = {{"B_slowwalk_samus", "B_walk_samus"},
                                         {"B_walk_samus", "B_slowwalk_samus"},
                                         {"B_slowrunloop_samus", "B_walk_samus"}};
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
  int idle = -1;
  int stop = -1;
  int loco[kLocoClips] = {-1, -1, -1};
  // Measured by Build: each walk clip's ground speed (world units per second), the ground one
  // loop covers, and where in the loop the left foot is furthest forward (Measure).
  float locoSpeed[kLocoClips] = {};
  float locoStride[kLocoClips] = {};
  float locoPeak[kLocoClips] = {};
  AimCell aim[9] = {};
  bool aimLayer = false;
  // The cannon's yaw and pitch in each aim cell (radians, model space, + right and up), and
  // the grid's extents about the ready pose's (aimCentre).
  float aimYaw[9] = {};
  float aimPitch[9] = {};
  float gridLeft = 0.f, gridRight = 0.f, gridDown = 0.f, gridUp = 0.f;
  // Animating.
  Stance stance = Stance::Idle;
  float stanceWeight[int(Stance::Count)] = {1.f, 0.f, 0.f};
  float locoWeight[kLocoClips] = {1.f, 0.f, 0.f};
  float stride = 0.f;   // where the feet are, in loops; each walk clip is at stride + its peak
  float clock = 0.f;    // seconds, for the clips that play by time (ready, aim)
  float stopTime = 0.f; // seconds into the stop clip
  float gunUp = 0.f;    // seconds the cannon stays raised (NoteShot)
  float gridU = 0.f, gridV = 0.f, gridSpeedU = 0.f, gridSpeedV = 0.f;
  // How far the shoulder turned the aim clips' cannon onto the aim, and how far it still misses
  // (radians; thirdperson feet).
  float armTurn = 0.f, armMiss = 0.f;
  float poseYaw = 0.f, posePitch = 0.f, aimYawNow = 0.f, aimPitchNow = 0.f;
  CVector3f pivot = CVector3f::Zero(); // the shoulder, as last posed (model space, unscaled)
  bool animating = false;
  // Debug (PinClip): the clip held instead of the movement's, and where in it.
  int pinned = -1;
  bool pinSet = false;
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
void Sample(CAnimData& data, int clip, float phase, Pose& out) {
  data.SetAnimation(CAnimPlaybackParms(clip, -1, 1.f, true), true);
  data.SetPhase(phase);
  CHierarchyPoseBuilder& pb = data.PoseBuilder();
  const CCharLayoutInfo& poseLayout = **pb.CharLayoutInfo();
  for (const CSegId& seg : s.segs) {
    pb.Insert(seg, CQuaternion::NoRotation());
    pb.Insert(seg, poseLayout.GetFromParentUnrotated(seg));
  }
  data.PortInvalidatePose();
  data.PreRender();
  for (const CSegId& seg : s.segs) {
    out.rot[seg.val()] = pb.GetSegRotation(seg);
    out.off[seg.val()] = pb.GetSegOffset(seg);
  }
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
  for (int i = 0; i < kLocoClips; ++i) {
    const int clip = s.loco[i];
    s.locoSpeed[i] = data.GetAverageVelocity(clip) * kBodyScale;
    s.locoStride[i] = s.locoSpeed[i] * data.GetAnimationDuration(clip);
    s.locoPeak[i] = 0.f;
    if (lFoot == CSegId::Invalid() || rFoot == CSegId::Invalid()) {
      continue;
    }
    // The left ankle's lead over the right along the facing, over one loop.
    constexpr int kSamples = 16;
    float lead[kSamples];
    for (int k = 0; k < kSamples; ++k) {
      Sample(data, clip, float(k) / kSamples, sTmp);
      lead[k] = SegPosition(data, lFoot).GetY() - SegPosition(data, rFoot).GetY();
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
    Sample(data, s.stop, std::min(s.stopTime / duration, 0.999f), sTmp);
    Mix(sAcc, total, sTmp, s.stanceWeight[int(Stance::Stop)], false);
  }
  for (int i = 0; i < kLocoClips; ++i) {
    const float w = s.stanceWeight[int(Stance::Loco)] * s.locoWeight[i];
    if (w > 0.f) {
      Sample(data, s.loco[i], Wrap(s.stride + s.locoPeak[i]), sTmp);
      Mix(sAcc, total, sTmp, w, false);
    }
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
  for (int i = 0; i < kLocoClips; ++i) {
    s.loco[i] = Find(*data, kLoco[i]);
    if (s.loco[i] < 0) {
      s.loco[i] = s.idle;
    }
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
  FindAimBones(*data);
  Measure(*data);
  s.stance = Stance::Idle;
  s.stanceWeight[0] = 1.f;
  s.stanceWeight[1] = s.stanceWeight[2] = 0.f;
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
  if (s.pinned >= 0) {
    CAnimData* data = s.body->AnimationData();
    if (!s.pinSet) {
      data->SetAnimation(CAnimPlaybackParms(s.pinned, -1, 1.f, true), true);
      s.pinSet = true;
    }
    data->SetPhase(s.pinPhase);
    data->SetPlaybackRate(0.f);
    s.body->AdvanceAnimation(dt, mgr, player.GetCurrentAreaId(), true);
    s.advanced = true;
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
  const float stopDuration = s.body->GetAnimationData()->GetAnimationDuration(s.stop);
  const Stance stance = ChooseStance(fwd, side, grounded, s.stance, s.stopTime >= stopDuration);
  if (stance == Stance::Stop && s.stance != Stance::Stop) {
    s.stopTime = 0.f;
  }
  s.stance = stance;
  for (int i = 0; i < int(Stance::Count); ++i) {
    s.stanceWeight[i] = Approach(s.stanceWeight[i], i == int(stance) ? 1.f : 0.f, dt / kBlendSeconds);
  }
  s.stopTime += dt;
  s.clock += dt;
  // Walking, the speed picks the walk clips' weights (kept while the walk fades out) and the
  // feet move a loop for each stride of ground covered, backward when walking backward: the
  // clips play at the speed the body moves, in step with each other.
  if (stance == Stance::Loco) {
    LocoWeights(std::fabs(fwd), s.locoSpeed, s.locoWeight);
  }
  float stride = 0.f;
  for (int i = 0; i < kLocoClips; ++i) {
    stride += s.locoWeight[i] * s.locoStride[i];
  }
  if (stride > 0.1f) {
    s.stride = Wrap(s.stride + dt * fwd / stride);
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
    if (s.pinned >= 0) {
      data.PreRender();
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
  if (phazon) {
    GXSetDstAlpha(GX_TRUE, 255);
  }
  const CTransform4f xf =
      s.pinned >= 0 ? feet * CTransform4f::RotateZ(CRelAngle::FromDegrees(s.pinYaw)) : feet;
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

namespace {
// The Samus clips the console lists (C2E1F513's; the other candidates carry a few of them).
constexpr const char* kClipNames[] = {
    "B_intorreadyfromwalk_samus", "B_rready_samus", "B_walk_samus", "B_aimr45downl_samus",
    "B_aimr45downr_samus", "B_aimr45upc_samus", "B_aimr45upl_samus", "B_aimr45upr_samus",
    "B_aimrright_samus", "B_ready_samus", "B_slowrunloop_samus", "B_slowwalk_samus",
    "B_walkgun_samus", "samus_turnaround", "B_walk_insnow_samus", "B_180turn_samus",
    "B_walk_samus_snow", "ready_to_up", "land_to_lookup"};
} // namespace

std::string DescribeClips() {
  if (!Ready()) {
    return "body not ready";
  }
  const CAnimData& data = *s.body->GetAnimationData();
  std::string out;
  char line[160];
  for (const char* name : kClipNames) {
    const int i = data.GetCharacterInfo().GetAnimationIndex(rstl::string_l(name));
    if (i >= 0) {
      std::snprintf(line, sizeof(line), "%2d %-28s %6.3fs %6.3f u/s\n", i, name,
                    data.GetAnimationDuration(i), data.GetAverageVelocity(i));
      out += line;
    }
  }
  return out;
}

std::string DescribeFeet() {
  if (!Ready()) {
    return "body not ready";
  }
  std::string out;
  char line[200];
  for (int i = 0; i < kLocoClips; ++i) {
    std::snprintf(line, sizeof(line), "%-22s %5.2f u/s stride %5.2f peak %4.2f\n", kLoco[i].a,
                  s.locoSpeed[i], s.locoStride[i], s.locoPeak[i]);
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
    s.pinned = -1;
    return name == nullptr;
  }
  const int i = s.body->GetAnimationData()->GetCharacterInfo().GetAnimationIndex(rstl::string_l(name));
  if (i < 0) {
    return false;
  }
  if (i != s.pinned) {
    s.pinSet = false;
  }
  s.pinned = i;
  s.pinPhase = phase - std::floor(phase);
  s.pinYaw = yaw;
  return true;
}

} // namespace PortThirdPersonBody
