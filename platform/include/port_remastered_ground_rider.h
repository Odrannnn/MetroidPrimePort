#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "port_remastered_ground_policy.h"
#include "port_remastered_ground_world.h"

// Moving CScriptPlatforms under Remastered's grounded mover (issue #117, slice 1:
// platforms with a DCLN collision tree, translation only). The pure decisions; the
// game glue is CGameCollision::MoveGroundedRemastered and CScriptPlatform::MoveRiders.
//
// Sources (Remastered 1.0.0 offsets):
//  - CheckCollision_Boolean_NoPostFilter 0xc1f110 / CheckCollision 0xc1ec60 walk the near
//    list built by FindOverlap 0xc1a760: static tree first, then actors by ascending
//    `id & 0xffff`, so platform contacts follow terrain contacts.
//  - AABox_OBBoxTree 0xc0fc18, traversal 0xc0fce0: LIFO, 256-entry stack, the root OBB is
//    never tested, children pushed in index order (visited in reverse). Each leaf triangle
//    goes through the terrain producer 0xc0a180 with the surface material merged with the
//    object's (MergeSurfaceWithObjectMaterial). Boolean twin 0xbfd2f0 / 0xbfd360.
//  - MoveObject 0xf923d8: after Post (0xf92804) the scan 0xf92808-0xf92860 takes the first
//    contact whose owner is not static and can be ridden, and calls SetRiderController
//    0xc1631c with count 2. No normal test. DecayRiderFrameCount (PostSimulation) 2 -> 1 -> 0.
//  - PredictMotion 0xc23e40: the rider's pose delta (R+0x1100 - R+0x1128) is added to the
//    predicted translation, unscaled, with the rotation term and yaw of a turning rider
//    (slice 3, RotationTerm/CarryYaw); the mode is cleared after the first MoveAlongGround
//    pass's ApplyMotionState (0xf9349c).
namespace PortRemasteredGroundRider {

using PortRemasteredGround::Vec3;

// Contact owner encoding (PortRemasteredGroundWorld::Contact::ownerUid): 0 is static
// terrain; an object is 0x10000 | its raw 16-bit TUniqueId, so a valid id never encodes as 0.
constexpr std::uint32_t kTerrainOwner = 0;
inline std::uint32_t OwnerFromRaw(std::uint16_t raw) { return 0x10000u | raw; }
inline std::uint16_t RawFromOwner(std::uint32_t owner) { return std::uint16_t(owner & 0xffffu); }
// The near-list order key. Remastered sorts actors by `id & 0xffff`, its object index; the
// port's index is the low 10 bits of TUniqueId (TUniqueId::Value), the version above it.
inline std::uint32_t NearListKey(std::uint32_t owner) { return owner & 0x3ffu; }
inline bool NearListBefore(std::uint32_t a, std::uint32_t b) {
  return NearListKey(a) < NearListKey(b);
}

// 0xc0fce0's traversal. `leaf(node)` is called for each overlapping leaf in visit order;
// returns false when a push was dropped at the 256-entry cap.
constexpr std::size_t kTreeStackDepth = 256;
template < class Node, class IsLeaf, class Left, class Right, class Overlaps, class Leaf >
bool WalkTreeLifo(const Node& root, IsLeaf isLeaf, Left left, Right right, Overlaps overlaps,
                  Leaf leaf) {
  if (isLeaf(root)) {
    leaf(root);
    return true;
  }
  const Node* stack[kTreeStackDepth];
  std::size_t top = 0;
  bool complete = true;
  auto push = [&](const Node* n) {
    if (top < kTreeStackDepth) {
      stack[top++] = n;
    } else {
      complete = false;
    }
  };
  push(&left(root));
  push(&right(root));
  while (top != 0) {
    const Node& n = *stack[--top];
    if (!overlaps(n)) {
      continue;
    }
    if (isLeaf(n)) {
      leaf(n);
    } else {
      push(&left(n));
      push(&right(n));
    }
  }
  return complete;
}

// One platform triangle as the terrain producer takes it: the surface material merged with
// the object's (MergeSurfaceWithObjectMaterial), tagged with its owner.
inline PortRemasteredGroundWorld::Triangle PlatformTriangle(
    const std::array< PortRemasteredGroundWorld::Vec3f, 3 >& verts,
    const PortRemasteredGroundWorld::Vec3f& normal, std::uint64_t surfaceBits,
    std::uint64_t objectBits, std::uint32_t owner) {
  PortRemasteredGroundWorld::Triangle tri;
  tri.verts = verts;
  tri.normal = normal;
  tri.materialBits = surfaceBits | objectBits;
  tri.ownerUid = owner;
  return tri;
}

// Admission of one near-list collidable that overlaps the tick's query region: an active
// CScriptPlatform with a DCLN tree (slice 1) or a box collidable (AABX, its contacts from
// AABox_AABox 0xbf61b4, port_remastered_ground_box.h). Anything else sends the whole tick to
// the retail grounded mover.
//  - The grounded mover admits a platform that rotated this tick (slice 3): its contacts
//    come from its pose now (a box does not turn with its platform, GetTransformedAABox
//    0xbf6064 is translation only), and PredictMotion carries the rider's rotation.
//  - The airborne mover (Admit) still refuses one, so it stays on the native list: its
//    controller offset for a rotated rider (get_rider_controller_offset 0xc18f80, the
//    farthest contact point) is not ported.
struct Candidate {
  bool platform = false;
  bool active = false;
  bool hasTree = false;
  bool rotated = false;
  bool box = false;
};
inline bool AdmitGrounded(const Candidate& c) {
  return c.platform && c.active && (c.hasTree || c.box);
}
inline bool Admit(const Candidate& c) { return AdmitGrounded(c) && !c.rotated; }

// The rider controller (object +0x1220 side, SetRiderController 0xc1631c): set to 2 by a
// grounded tick that picked a rider, decayed once at the end of every movement tick.
// The count is in 60 Hz frames: off 60 Hz a tick spends ticks60 of it (dt * 60), so the
// controller outlives its tick by 1/60 s at any sim rate. Counted in ticks, a 300 Hz jump
// lost the platform's carry and its floor offset after 1/300 s, and a rising lift caught
// up with her and was skipped as a floor she was moving away from (issue #248).
struct RiderController {
  std::uint32_t owner = kTerrainOwner;
  float count = 0.f;

  void Set(std::uint32_t o) {
    owner = o;
    count = 2.f;
  }
  void Decay(float ticks60 = 1.f) {
    if (count > 0.f) {
      count -= ticks60;
      if (count <= 1e-3f) {
        count = 0.f;
        owner = kTerrainOwner;
      }
    }
  }
  bool Active() const { return count > 0 && owner != kTerrainOwner; }
};

// The mover's result list (0xf923d8 +0x12a0): cleared once when the tick starts, then every
// MoveAlongGround pass's list and finally the snap's are appended to it, in that order
// (copy_collision_info_list_into 0xc18c00 appends and never clears its destination).
inline void AppendContacts(PortRemasteredGroundWorld::Result& into,
                           const PortRemasteredGroundWorld::Result& from) {
  into.contacts.insert(into.contacts.end(), from.contacts.begin(), from.contacts.end());
}

// The scan after Post (0xf92808-0xf92860) over that combined list: the first non-static
// contact (the side whose owner is not the mover) that can be ridden wins, whichever pass or
// the snap produced it; no normal or floor test, so a wall contact counts.
template < class Rideable >
std::uint32_t PickRider(const PortRemasteredGroundWorld::Result& list, Rideable rideable) {
  for (const PortRemasteredGroundWorld::Contact& c : list.contacts) {
    if (!c.self && c.ownerUid != kTerrainOwner && rideable(c.ownerUid)) {
      return c.ownerUid;
    }
  }
  return kTerrainOwner;
}

// The rider's pose delta for this tick: the platform's translation now minus its
// translation at the start of its PreThink, only when that snapshot was taken this tick and
// the rider is live. A stale stamp (the platform did not think this tick) carries nothing.
inline Vec3 PlatformCarry(const Vec3& previous, const Vec3& current, std::uint32_t stampTick,
                          std::uint32_t tick, bool riderActive) {
  if (!riderActive || stampTick != tick) {
    return {};
  }
  return {current.x - previous.x, current.y - previous.y, current.z - previous.z};
}

// The rider carry for any platform state. A platform that thought this tick carries its pose
// delta since that PreThink (PlatformCarry). One deactivated since its last PreThink (it was
// active in the previous tick's, not in this one's) carries that last step's delta, start to
// end of that PreThink, once: an inactive actor is not simulated, so PredictMotion's
// R+0x1100 - R+0x1128 still holds its last step, and the rider set in that step decays at the
// end of this one (platform-lifecycle ADDENDUM §1, §3). Never the pose now: a warp while
// inactive is not carried. Any older stamp, or an active platform that did not think this
// tick, carries nothing.
inline Vec3 RiderCarry(const Vec3& start, const Vec3& now, const Vec3& end,
                       std::uint32_t stampTick, std::uint32_t tick, bool platformActive,
                       bool riderActive) {
  if (!riderActive) {
    return {};
  }
  if (stampTick == tick) {
    return PlatformCarry(start, now, stampTick, tick, true);
  }
  if (!platformActive && stampTick + 1u == tick) {
    return {end.x - start.x, end.y - start.y, end.z - start.z};
  }
  return {};
}

// A platform's rotation, by columns (CTransform4f::GetColumn), in the port's Z-up frame.
struct Basis {
  Vec3 c[3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
};
// PredictMotion's skip (0xc24208): an exactly unchanged orientation has no rotation term.
inline bool SameBasis(const Basis& a, const Basis& b) {
  for (int i = 0; i < 3; ++i) {
    if (a.c[i].x != b.c[i].x || a.c[i].y != b.c[i].y || a.c[i].z != b.c[i].z) return false;
  }
  return true;
}
inline Vec3 MulBasis(const Basis& m, const Vec3& v) {
  return {m.c[0].x * v.x + m.c[1].x * v.y + m.c[2].x * v.z,
          m.c[0].y * v.x + m.c[1].y * v.y + m.c[2].y * v.z,
          m.c[0].z * v.x + m.c[1].z * v.y + m.c[2].z * v.z};
}
// dq = qcur * conj(qprev) (0xc24254, Hamilton product) as a matrix: cur * prev^T, whose
// column j is cur applied to row j of prev.
inline Basis RelativeRotation(const Basis& prev, const Basis& cur) {
  auto at = [](const Vec3& v, int j) { return j == 0 ? v.x : j == 1 ? v.y : v.z; };
  Basis r;
  for (int j = 0; j < 3; ++j) {
    r.c[j] = MulBasis(cur, {at(prev.c[0], j), at(prev.c[1], j), at(prev.c[2], j)});
  }
  return r;
}
// The rotation term (0xc24228-0xc2441c): dq * d * dq^-1 - d, with the arm d the rider's
// position at the prediction (S+0x18, before v*dt) minus the platform's previous position.
// A full 3D rotation: a tilt moves the rider vertically. Evaluated in double, as the
// source's small-angle matrix path (its float quaternion path agrees, INFERRED).
inline Vec3 RotationTerm(const Basis& r, const Vec3& pivot, const Vec3& self) {
  const double d[3] = {double(self.x) - pivot.x, double(self.y) - pivot.y,
                       double(self.z) - pivot.z};
  const Vec3* c = r.c;
  const double x = c[0].x * d[0] + double(c[1].x) * d[1] + double(c[2].x) * d[2];
  const double y = c[0].y * d[0] + double(c[1].y) * d[1] + double(c[2].y) * d[2];
  const double z = c[0].z * d[0] + double(c[1].z) * d[1] + double(c[2].z) * d[2];
  return {float(x - d[0]), float(y - d[1]), float(z - d[2])};
}
// The orientation part (0xc24430-0xc24458): BuildTransform(dq) with the up row and column
// forced to e_up, Orthonormalized 0xd5140, FromMatrix. Only the heading of dq's forward axis
// in the horizontal plane survives: the yaw about up, in radians (0 when that axis is
// vertical). The rider's orientation becomes q_integrated * ZRotation(yaw) (0xc2446c).
inline float CarryYaw(const Basis& r) {
  const Vec3& f = r.c[0];
  if (f.x == 0.f && f.y == 0.f) return 0.f;
  return std::atan2(f.y, f.x);
}

// The rider's yaw (0xc2446c, q_integrated * ZRotation(yaw)) folded into the port's additive
// orientation delta. AddMotionState does q <- normalize(q + delta') (CNUQuaternion.cpp), so
// the delta that lands on q_integrated * yawq = normalize(q + delta) * yawq (|yawq| = 1) is
// delta' = (q + delta) * yawq - q; a saved prediction (SetMotionState normalizes too) carries
// it the same way. The product is CNUQuaternion::operator* with yawq = (cos, 0, 0, sin)
// written out, term for term. A zero yaw is the delta itself: going through the sum and
// difference would only add rounding (fl(q + delta) - q != delta).
inline PortRemasteredGround::Quat4 YawedOrientationDelta(const PortRemasteredGround::Quat4& q,
                                                         const PortRemasteredGround::Quat4& delta,
                                                         float yaw) {
  if (yaw == 0.f) return delta;
  const float w = q.w + delta.w, x = q.x + delta.x, y = q.y + delta.y, z = q.z + delta.z;
  const float c = std::cos(0.5f * yaw), s = std::sin(0.5f * yaw);
  const float tw = w * c - z * s;
  const float tx = c * x + y * s;
  const float ty = c * y - x * s;
  const float tz = w * s + c * z;
  return {tw - q.w, tx - q.x, ty - q.y, tz - q.z};
}

// The carry term of PredictMotion: added whole (not scaled by t) while pending, and
// consumed by the first MoveAlongGround pass's ApplyMotionState. `carry` is the platform's
// translation delta; a rotating platform adds the rotation term about its previous position
// `pivot`, for the rider at `self`, and turns the rider by `yaw`.
struct CarryTerm {
  Vec3 carry;
  bool pending = false;
  bool rotates = false;
  Basis rotation;
  Vec3 pivot;
  float yaw = 0.f;

  Vec3 Translation(const Vec3& self) const {
    if (!rotates) return carry;
    const Vec3 r = RotationTerm(rotation, pivot, self);
    return {carry.x + r.x, carry.y + r.y, carry.z + r.z};
  }
  Vec3 Apply(const Vec3& predicted, const Vec3& self = {}) const {
    if (!pending) {
      return predicted;
    }
    const Vec3 c = Translation(self);
    return {predicted.x + c.x, predicted.y + c.y, predicted.z + c.z};
  }
  void ConsumeAfterPass() { pending = false; }
};

// A platform pose: its translation and rotation.
struct Pose {
  Vec3 origin;
  Basis basis;
};

inline bool SamePose(const Pose& a, const Pose& b) {
  return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.origin.z == b.origin.z &&
         SameBasis(a.basis, b.basis);
}

// RiderCarry's choice of the two poses, with the rotation: the platform's pose at its
// PreThink start, and now or at that PreThink's end (a deactivated platform). Pending when
// it moves the rider at all.
inline CarryTerm RiderCarryTerm(const Pose& start, const Pose& now, const Pose& end,
                                std::uint32_t stampTick, std::uint32_t tick,
                                bool platformActive, bool riderActive) {
  CarryTerm term;
  if (!riderActive) return term;
  const Pose* to = nullptr;
  if (stampTick == tick) {
    // Moved since its own step ended (`end`: its PreThink, then its physics move in
    // MovePlatforms), so by a SetTransform: a teleport. Remastered teleports the physics
    // object to the new pose before the step and resets Previous with it (PrePhysicsUpdate
    // 0xc543b8, TeleportObject ERetainPrevious 0), so a teleport carries nothing. As if the
    // stamp were re-taken at the new pose.
    if (!SamePose(now, end)) return term;
    to = &now;
  } else if (!platformActive && stampTick + 1u == tick) {
    to = &end;
  } else {
    return term;
  }
  term.carry = {to->origin.x - start.origin.x, to->origin.y - start.origin.y,
                to->origin.z - start.origin.z};
  term.rotates = !SameBasis(start.basis, to->basis);
  if (term.rotates) {
    term.rotation = RelativeRotation(start.basis, to->basis);
    term.pivot = start.origin;
    term.yaw = CarryYaw(term.rotation);
  }
  term.pending =
      term.rotates || term.carry.x != 0.f || term.carry.y != 0.f || term.carry.z != 0.f;
  return term;
}

// Exactly one carry per tick, whichever mover runs. Order inside a tick:
// CStateManager::Update runs PreThinkObjects (CScriptPlatform::PreThink: pose snapshot,
// MovePlatform, MoveRiders) BEFORE the player's CGameCollision::Move, so retail MoveRiders
// has to act before the player's mover is chosen.
//  - MoveRiders defers the player's retail carry (instead of moving it) when the player's
//    previous movement tick ran a Remastered mover that owns the carry: an admitted grounded
//    tick (MoveObject 0xf923d8) or an airborne one (MoveObject 0xf943b4, slice 2); otherwise
//    it moves the player as retail does and the tick is marked as already carried.
//  - Then, once the tick's mover is known: a carry-owning Remastered tick drops the deferred
//    retail carry and adds its own carry unless a retail carry already moved the player this
//    tick; every other tick (retail grounded fallback, morph ball, gate off, a tick whose
//    mover never runs) applies the deferred retail carry and no Remastered one.
// Both Remastered movers own the carry because both predict through PredictMotion 0xc23e40,
// which adds the rider's pose delta whenever the rider id is valid (state +0x128): nothing
// in it tests the object state, so an airborne tick with a live rider is carried too.
inline bool DeferRetailCarry(bool lastTickRemasteredCarry) { return lastTickRemasteredCarry; }

struct CarryPlan {
  bool applyDeferredRetail = false;
  bool remasteredCarry = false;
};
inline CarryPlan PlanCarry(bool remasteredCarryMover, bool retailCarriedThisTick) {
  CarryPlan plan;
  if (!remasteredCarryMover) {
    plan.applyDeferredRetail = true;
    return plan;
  }
  plan.remasteredCarry = !retailCarriedThisTick;
  return plan;
}

// The airborne mover's rider writer. DoMovementStep 0xf948d4 resets its candidate to the
// invalid id at the top of each step's impact loop (local_1108, line 331), takes the owner of
// every is_floor impact that is not flagged (contact+0x29 bit 0), not static
// (`(type & ~1) != 2`) and can be ridden (+0x1220) (lines 395-412), and writes it out only on
// the landing branch (`*param_7 = local_1108`, line 493), invalid or not. The post-step probe
// landing (lines 511-533) writes nothing. MoveObject 0xf943b4 then calls SetRiderController
// with it only when valid (0xf94720); a tick that does not land leaves the rider to decay.
struct AirborneRider {
  std::uint32_t written = kTerrainOwner;
  bool any = false;

  void Landed(std::uint32_t stepCandidate) {
    written = stepCandidate;
    any = true;
  }
  // The owner to pass to SetRiderController, or kTerrainOwner for none.
  std::uint32_t Controller() const { return any ? written : kTerrainOwner; }
};

} // namespace PortRemasteredGroundRider
