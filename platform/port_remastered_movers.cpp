// The Remastered player movers, defined here rather than inline in
// src/MetroidPrime/CGameCollision.cpp so the decomp file keeps only the dispatch
// hooks in CGameCollision::MovePlayer (see docs/NATIVE_PORT.md, "Where port code
// goes"). Both are CGameCollision static members, declared under TARGET_PC in
// include/MetroidPrime/CGameCollision.hpp.
//
// This translation unit is part of mp_game, so it is compiled with exactly the
// flags, definitions and forced include (platform/compat.h) the decomp sources
// get; the code below was moved verbatim and must stay bit-for-bit equivalent to
// its former inline form. The includes repeat CGameCollision.cpp's for the same
// reason.

#include "MetroidPrime/CGameCollision.hpp"
#include "Collision/CCollidableAABoxSphere.hpp"
#include "Collision/CCollisionInfoList.hpp"
#include "Collision/CInternalRayCastStructure.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CollisionUtil.hpp"
#include "Kyoto/Math/CLine.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "MetroidPrime/CAABoxFilter.hpp"
#include "MetroidPrime/CBallFilter.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CGroundMovement.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Enemies/CBloodFlower.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDock.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/UserNames.hpp"
#include "WorldFormat/CAreaOctTree.hpp"
#include "WorldFormat/CMetroidAreaCollider.hpp"

#include "rstl/math.hpp"

#include <Collision/CCollidableSphere.hpp>
#include <Collision/CCollisionPrimitive.hpp>
#include <Collision/CRayCastResult.hpp>
#include <WorldFormat/CCollidableOBBTreeGroup.hpp>
#include <WorldFormat/COBBTree.hpp>
#include <Collision/COBBox.hpp>
#include <WorldFormat/CCollisionSurface.hpp>
#include <algorithm>

#ifdef TARGET_PC
#include "port_debug.h"
#include "port_air_mover_contact.h"
#include "port_collision_failsafe.h"
#include "port_remastered_box_landing.h"
#include "port_remastered_ground_box.h"
#include "port_remastered_ground_policy.h"
#include "port_remastered_ground_world.h"
#include "port_remastered_ground_rider.h"
#endif

#ifdef TARGET_PC
// Scales the whole velocity down when its rise exceeds the player's maximum (0xf94480).
void CGameCollision::PortCapRise(CStateManager& mgr, CPlayer& player) {
  const float maxUp = player.GetMaximumPlayerPositiveVerticalVelocity(mgr);
  const CVector3f velocity = player.GetVelocityWR();
  if (velocity.GetZ() > maxUp) {
    player.SetVelocityWR(velocity * (maxUp / velocity.GetZ()));
  }
}

// Remastered's airborne player mover: CPhysicsSteppedMoverColliderMP1::MoveObject
// 0xf943b4 / DoMovementStep 0xf948d4, with the impact callback
// CPlayerMP1::ImpactResponseForOffGroundMoverCollider 0xc6d460 and the landing
// check GroundCheckForOffGroundMoverCollider 0xc6d110. Retail MoveAndCollide
// resets a blocked player to its last free state and zeroes its vertical
// speed, so a jump into a lip or wall stalled (issue #117). The mover slides
// along the contact instead.
//
// 0xf948d4's "pre-probe" (0xf94954-0xf94ae8: clear the contact list, the rise cap,
// a dt=0 motion prediction, the full collision test at the actor's current
// position, and - on a hit - (*(obj+0x1280))->vt[0x10] plus
// CPhysicsCollisionFailsafe::DoCollisionFailsafe(world, obj) 0xc1fa68) is deliberately
// not ported: it cannot run in the shipped mover. Its gate is
// `!bool_b && |dt - worldDt| < 1e-5` (0xf94934-0xf94950), and of its only two call
// sites (both in MoveObject 0xf943b4) the first step of a tick passes bool_b = 1
// (0xf94528 `mov w4,#1`, skipped) while the retry loop - entered only when the first
// step consumed more than 1e-5 s (0xf9455c) and subtracting each step's consumption
// (0xf946ac) - never has dt == worldDt again. The base class has the same two flags
// (0xc1cdd0, 0xc1ce7c). See build/s-issue117-20261015/airfix/REPORT.md.

// The rider carry of PredictMotion 0xc23e40 for the tick, while the controller is live (set
// by an earlier tick): the ridden platform's pose delta since its PreThink this tick, or its
// last step once after a deactivation (PortRemasteredGroundRider::RiderCarryTerm), with the
// rotation of a turning platform. Both Remastered player movers read it.
static PortRemasteredGroundRider::Pose PortPose(const CTransform4f& xf) {
  PortRemasteredGroundRider::Pose pose;
  const CVector3f t = xf.GetTranslation();
  pose.origin = {t.GetX(), t.GetY(), t.GetZ()};
  for (int c = 0; c < 3; ++c) {
    const CVector3f col = xf.GetColumn(c);
    pose.basis.c[c] = {col.GetX(), col.GetY(), col.GetZ()};
  }
  return pose;
}
static PortRemasteredGroundRider::CarryTerm PortRiderCarry(CStateManager& mgr, CPlayer& player,
                                                           uint simTick) {
  PortRemasteredGroundRider::RiderController& rider = player.PortRider();
  if (rider.Active()) {
    const ushort raw = PortRemasteredGroundRider::RawFromOwner(rider.owner);
    if (const CScriptPlatform* platform = TCastToConstPtr< CScriptPlatform >(
            mgr.GetObjectById(TUniqueId(raw >> kUniqueIdIndexBits, raw & kUniqueIdIndexMask)))) {
      return PortRemasteredGroundRider::RiderCarryTerm(
          PortPose(platform->PortPreThinkTransform()), PortPose(platform->GetTransform()),
          PortPose(platform->PortPreThinkEndTransform()), platform->PortPoseTick(), simTick,
          platform->GetActive(), true);
    }
  }
  return PortRemasteredGroundRider::CarryTerm();
}

// The rider's yaw folded into the port's additive orientation delta
// (PortRemasteredGroundRider::YawedOrientationDelta).
static CNUQuaternion PortYawedOrientationDelta(const CPhysicsActor& actor,
                                               const CNUQuaternion& delta, float yaw) {
  const CNUQuaternion q = CNUQuaternion::BuildFromQuaternion(actor.GetRotation());
  const CVector3f qv = q.GetVector();
  const CVector3f dv = delta.GetVector();
  const PortRemasteredGround::Quat4 r = PortRemasteredGroundRider::YawedOrientationDelta(
      {q.GetScalar(), qv.GetX(), qv.GetY(), qv.GetZ()},
      {delta.GetScalar(), dv.GetX(), dv.GetY(), dv.GetZ()}, yaw);
  return CNUQuaternion(r.w, r.x, r.y, r.z);
}

static PortRemasteredGround::Vec3 PortVec(const CVector3f& v) {
  return {v.GetX(), v.GetY(), v.GetZ()};
}

// The carry is part of the predicted motion, so the motion volume covers it (unchanged at 0).
static CAABox PortWidenByCarry(const CAABox& volume, const PortRemasteredGround::Vec3& carry) {
  const CVector3f c(carry.x, carry.y, carry.z);
  const CVector3f lo = volume.GetMinPoint();
  const CVector3f hi = volume.GetMaxPoint();
  return CAABox(CVector3f(rstl::min_val(lo.GetX(), lo.GetX() + c.GetX()),
                          rstl::min_val(lo.GetY(), lo.GetY() + c.GetY()),
                          rstl::min_val(lo.GetZ(), lo.GetZ() + c.GetZ())),
                CVector3f(rstl::max_val(hi.GetX(), hi.GetX() + c.GetX()),
                          rstl::max_val(hi.GetY(), hi.GetY() + c.GetY()),
                          rstl::max_val(hi.GetZ(), hi.GetZ() + c.GetZ())));
}

// A platform without a DCLN tree collides as its CCollidableAABox (CScriptPlatform's ctor),
// a box translated with the platform and never turned (GetTransformedAABox 0xbf6064).
static bool PortIsBoxPlatform(const CScriptPlatform& platform) {
  return !platform.HasComplexCollision() && platform.GetCollisionPrimitive() != NULL &&
         platform.GetCollisionPrimitive()->GetPrimType() == 'AABX';
}

// The admission of a near-list platform to the Remastered producer: active, with a DCLN tree
// or a box; the airborne mover also refuses one rotated this tick (a stale stamp is not a
// rotation), the grounded one admits it (PortRemasteredGroundRider::AdmitGrounded).
static PortRemasteredGroundRider::Candidate PortPlatformCandidate(const CScriptPlatform& platform,
                                                                  uint simTick) {
  PortRemasteredGroundRider::Candidate candidate;
  candidate.platform = true;
  candidate.active = platform.GetActive();
  candidate.hasTree = platform.HasComplexCollision();
  candidate.rotated = platform.PortPoseTick() == simTick && platform.PortRotatedSinceStamp();
  candidate.box = PortIsBoxPlatform(platform);
  return candidate;
}
static bool PortAdmitPlatform(const CScriptPlatform& platform, uint simTick) {
  return PortRemasteredGroundRider::Admit(PortPlatformCandidate(platform, simTick));
}
static bool PortAdmitGroundedPlatform(const CScriptPlatform& platform, uint simTick) {
  return PortRemasteredGroundRider::AdmitGrounded(PortPlatformCandidate(platform, simTick));
}

// The port's filter as the source's CIncludeExcludeCollisionFilterMP1: the include/exclude
// lists are MP1 material-list bits (the source's ctor 0xcb6f0c stores exactly these), the
// mode is the filter's own.
static PortRemasteredCollisionMaterial::MaterialFilter PortTranslatedFilter(
    const CMaterialFilter& f) {
  return {f.GetIncludeList().GetValue(), f.GetExcludeList().GetValue(),
          static_cast< uint32_t >(f.GetFilterType())};
}

static PortCollisionFailsafe::Box PortFsBox(const CAABox& b) {
  return {{b.GetMinPoint().GetX(), b.GetMinPoint().GetY(), b.GetMinPoint().GetZ()},
          {b.GetMaxPoint().GetX(), b.GetMaxPoint().GetY(), b.GetMaxPoint().GetZ()}};
}

// An admitted box platform's place in a query for the self box `box` (the actor's material
// `selfBits`, its filter `f`): AABox_AABox_Boolean 0xbfce60 for `blocked`, AABox_AABox
// 0xbf61b4 for the contacts (PortRemasteredGroundBox). `index` is the platform's position in
// the admitted list, which spaces the contact keys; `at` is the triangle count before it.
static PortRemasteredGroundBox::Slot PortBoxPlatformSlot(const CScriptPlatform& platform,
                                                        std::size_t index,
                                                        const CMaterialFilter& f,
                                                        const CAABox& box, std::uint64_t selfBits,
                                                        std::size_t at, bool wantContacts) {
  PortRemasteredGroundBox::Slot slot;
  slot.at = at;
  PortRemasteredBoxLanding::Pair pair;
  pair.selfBox = PortFsBox(box);
  pair.otherBox = PortFsBox(
      platform.GetCollisionPrimitive()->CalculateAABox(platform.GetPrimitiveTransform()));
  pair.selfMaterialBits = selfBits;
  // Read live, as the tree platforms' object half (SetCollisionMaterial 0xc1536c).
  pair.otherMaterialBits = platform.GetMaterialList().GetValue();
  pair.selfFilter = PortTranslatedFilter(f);
  pair.otherFilter = PortTranslatedFilter(platform.GetMaterialFilter());
  slot.blocked = PortRemasteredGroundBox::Blocked(pair);
  if (slot.blocked && wantContacts) {
    static std::vector< PortCollisionFailsafe::PairedImpact > scratch; // game thread
    PortRemasteredGroundBox::AppendContacts(
        pair, PortRemasteredGroundRider::OwnerFromRaw(platform.GetUniqueId().value),
        PortRemasteredGroundBox::kBoxKeyBase + index * PortRemasteredGroundBox::kBoxKeyStride,
        slot.contacts, scratch);
  }
  return slot;
}

// FindOverlap 0xc1a760 orders actors by `id & 0xffff`.
static void PortSortPlatforms(std::vector< const CScriptPlatform* >& platforms) {
  std::stable_sort(platforms.begin(), platforms.end(),
                   [](const CScriptPlatform* a, const CScriptPlatform* b) {
                     return PortRemasteredGroundRider::NearListBefore(
                         PortRemasteredGroundRider::OwnerFromRaw(a->GetUniqueId().value),
                         PortRemasteredGroundRider::OwnerFromRaw(b->GetUniqueId().value));
                   });
}

// The admitted platforms' triangles for `box`, appended to `tris` platform by platform
// (AABox_OBBoxTree 0xc0fc18 / traversal 0xc0fce0: LIFO, root untested, children in index
// order). Each triangle carries the surface material merged with the object's and its
// owner; `filter` stands for the player's side of SurfaceCollisionPasses (vt+0x18), as the
// retail tree collider applies it per triangle. `ends` receives one end index per platform.
// A box platform has no triangles: its slot goes to `boxes` (PortBoxPlatformSlot, the actor's
// material `selfBits`) when given, and its `ends` entry closes an empty range. Boolean probes
// pass `wantContacts` false: only the slot's `blocked` flag is read.
static void PortPlatformTriangles(const std::vector< const CScriptPlatform* >& platforms,
                                  const CMaterialFilter& f, const CAABox& box,
                                  std::vector< PortRemasteredGroundWorld::Triangle >& tris,
                                  std::vector< std::size_t >* ends,
                                  std::vector< PortRemasteredGroundBox::Slot >* boxes = NULL,
                                  std::uint64_t selfBits = 0, bool wantContacts = true) {
  for (std::size_t p = 0; p < platforms.size(); ++p) {
    const CScriptPlatform& platform = *platforms[p];
    if (!platform.HasComplexCollision()) {
      if (boxes != NULL) {
        boxes->push_back(PortBoxPlatformSlot(platform, p, f, box, selfBits, tris.size(), wantContacts));
      }
      if (ends != NULL) ends->push_back(tris.size());
      continue;
    }
    const CCollidableOBBTreeGroup& group =
        static_cast< const CCollidableOBBTreeGroup& >(*platform.GetCollisionPrimitive());
    const CTransform4f xf = platform.GetPrimitiveTransform();
    const COBBox local = COBBox::FromAABox(box, xf.GetQuickInverse());
    // The object half is read live: 0xc0fce0 merges the collidable's material
    // (+0x10..+0x24) at query time, and every MaterialListChanged rewrites it
    // (CPhysicsState::SetCollisionMaterial 0xc1536c), so a material added since the
    // constructor counts. The tree group's own copy is the constructor's list.
    const std::uint64_t objectBits = platform.GetMaterialList().GetValue();
    const std::uint32_t owner =
        PortRemasteredGroundRider::OwnerFromRaw(platform.GetUniqueId().value);
    for (int t = 0; t < group.GetContainer()->NumTrees(); ++t) {
      const COBBTree& tree = *group.GetOBBTreeAABox(t);
      PortRemasteredGroundRider::WalkTreeLifo(
          tree.GetRoot(), [](const COBBTree::CNode& n) { return n.IsLeaf(); },
          [](const COBBTree::CNode& n) -> const COBBTree::CNode& { return n.GetLeftNode(); },
          [](const COBBTree::CNode& n) -> const COBBTree::CNode& { return n.GetRightNode(); },
          [&](const COBBTree::CNode& n) { return local.OBBIntersectsBox(n.GetOBB()); },
          [&](const COBBTree::CNode& n) {
            const rstl::vector< ushort >& surfaces = n.GetLeafData().GetSurfaceVector();
            for (int i = 0; i < int(surfaces.size()); ++i) {
              const CCollisionSurface surf = tree.GetTransformedSurface(surfaces[i], xf);
              const std::uint64_t surfaceBits = surf.GetSurfaceFlags();
              if (!f.Passes(CMaterialList(surfaceBits | objectBits))) {
                continue;
              }
              const CUnitVector3f nrm = surf.GetNormal();
              tris.push_back(PortRemasteredGroundRider::PlatformTriangle(
                  {PortRemasteredGroundWorld::Vec3f{surf.GetVert(0).GetX(),
                                                    surf.GetVert(0).GetY(),
                                                    surf.GetVert(0).GetZ()},
                   PortRemasteredGroundWorld::Vec3f{surf.GetVert(1).GetX(),
                                                    surf.GetVert(1).GetY(),
                                                    surf.GetVert(1).GetZ()},
                   PortRemasteredGroundWorld::Vec3f{surf.GetVert(2).GetX(),
                                                    surf.GetVert(2).GetY(),
                                                    surf.GetVert(2).GetZ()}},
                  PortRemasteredGroundWorld::Vec3f{nrm.GetX(), nrm.GetY(), nrm.GetZ()},
                  surfaceBits, objectBits, owner));
            }
          });
    }
    if (ends != NULL) ends->push_back(tris.size());
  }
}

void CGameCollision::MoveAirborneStepped(CStateManager& mgr, CPlayer& player, float dt,
                                         const TEntityList* colliderList) {
  CPhysicsActor& actor = player;
  const CMaterialFilter& materialFilter = actor.GetMaterialFilter();
  const float mass = actor.GetMass();
  const CVector3f entryPos = actor.GetTranslation();

  // Platform riders (issue #117, slice 2): this mover owns the tick's carry. The retail
  // carry MoveRiders deferred is dropped, and the rider's pose delta goes into the first
  // step's predictions instead (0xf94528 passes the carry mode only to the first
  // DoMovementStep; the retry loop passes 0), unless retail already moved the player.
  const uint simTick = mgr.PortSimTick();
  const PortRemasteredGroundRider::CarryPlan carryPlan =
      PortRemasteredGroundRider::PlanCarry(true, player.PortRetailRiderCarried(simTick));
  player.PortDropDeferredRiderCarry();
  player.PortSetLastTickRemasteredCarry(true);
  PortRemasteredGroundRider::CarryTerm carry;
  if (carryPlan.remasteredCarry) {
    carry = PortRiderCarry(mgr, player, simTick);
  }
  PortRemasteredGroundRider::AirborneRider airRider;

  CAABox motionVolume = PortWidenByCarry(
      actor.GetMotionVolume(dt), carry.pending ? carry.Translation(PortVec(entryPos))
                                               : PortRemasteredGround::Vec3());
  TEntityList nearList;
  if (colliderList != NULL) {
    nearList = *colliderList;
  } else {
    mgr.BuildColliderList(nearList, actor,
                          CAABox(motionVolume.GetMinPoint() - CVector3f(1.f, 1.f, 1.f),
                                 motionVolume.GetMaxPoint() + CVector3f(1.f, 1.f, 1.f)));
  }
  CAreaCollisionCache cache(motionVolume);
  BuildAreaCollisionCache(mgr, cache);

  // The admitted platforms (active, DCLN or box, unrotated) go through the source producer
  // with the terrain, as on the grounded mover; every other actor stays on the native path,
  // a platform rotated this tick too (its controller offset 0xc18f80 is not ported).
  std::vector< const CScriptPlatform* > platforms;
  TEntityList nativeList;
  for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
    const CScriptPlatform* platform = TCastToConstPtr< CScriptPlatform >(mgr.GetObjectById(*id));
    if (platform != NULL && platform != static_cast< const CActor* >(&actor) &&
        platform->GetCollisionPrimitive() != NULL && PortAdmitPlatform(*platform, simTick)) {
      platforms.push_back(platform);
    } else {
      nativeList.push_back(*id);
    }
  }
  PortSortPlatforms(platforms);
  // The rider controller's offset (0xc18f80): its pose delta, read every step whether or not
  // this tick's carry is the Remastered one.
  // A rotated controller keeps the translation part (see the admission above).
  const PortRemasteredGround::Vec3 riderCarry = PortRiderCarry(mgr, player, simTick).carry;
  const PortAirMoverContact::Vec3 riderOffset = {riderCarry.x, riderCarry.y, riderCarry.z};

  // NCollisionUtils is_floor / is_wall (0xc7xxxx). Port contacts carry
  // DefaultCollisionMaterial's kind 0xffffffff; kMT_Floor stands in for kind 0
  // (inferred, not proven). Remastered normals are Y-up: normal.up = port Z.
  struct Classify {
    static bool IsFloor(const CMaterialList& mat, const CVector3f& n) {
      // is_floor: up > 0.85 || kind == 0 || (up > 0.7071 && kind >= 3).
      return mat.HasMaterial(kMT_Floor) || n.GetZ() > 0.70710677f;
    }
    static bool IsWall(const CMaterialList& mat, const CVector3f& n) {
      // is_wall: false above 0.85; kind 2 true; else -0.7071 <= up <= 0.7071 && kind > 1.
      return !mat.HasMaterial(kMT_Floor) && n.GetZ() >= -0.70710677f && n.GetZ() <= 0.70710677f;
    }
  };

  bool hadCollision = false;
  // Some accepted impact of this tick is_wall (0xbecbb0), for CollidedWithNew 0xcf1a2c below.
  bool wallImpact = false;
  bool grounded = false; // Remastered object state == 1
  CCollisionInfoList groundList;
  TUniqueId riderId = kInvalidUniqueId; // object +0x1210

  // The static half of a collision query runs only when the material filter does not exclude
  // static collision - the guard CGameCollision::DetectCollision and
  // DetectCollisionBoolean put in front of their static half, and the one the native half of
  // MoveAirborneStepped already uses. The producer's terrain query is the same static half.
  struct StaticQuery {
    static bool Allowed(const CMaterialFilter& filter) {
      return !filter.GetExcludeList().HasMaterial(kMT_NoStaticCollision);
    }
  };

  // Remastered's static terrain contacts for the box placed `delta` from the actor,
  // as the shipped mover sees them: NCollidableBoxTree::build_collision_info
  // (0xc0a180) run over the same triangles the port's static half walks, through the
  // source-exact producer (platform/port_remastered_ground_world.cpp). One contact per
  // triangle, reduced to the non-self side (0xf928c0:66-70, 0xf93c9c:311-315,
  // 0xf934ec:546-549), because that is the side whose normal, material and optional
  // secondary normal every shipped consumer reads.
  struct TerrainContacts {
    // The triangles the static half walks for the box placed `delta` from the actor.
    // `groupEnds` (optional) receives each area's end index into `tris`: one collided
    // object per area tree.
    static void Collect(CStateManager& mgr, CAreaCollisionCache& cache, CPhysicsActor& actor,
                        const CMaterialFilter& filter, const CVector3f& delta,
                        PortRemasteredGroundWorld::Query& query,
                        std::vector< PortRemasteredGroundWorld::Triangle >& tris,
                        std::vector< std::size_t >* groupEnds = NULL,
                        const std::vector< const CScriptPlatform* >* platforms = NULL,
                        std::vector< PortRemasteredGroundBox::Slot >* boxes = NULL) {
      actor.MoveCollisionPrimitive(delta);
      const CAABox box =
          actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform());
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      if (!box.Inside(cache.GetCacheBounds())) {
        CAABox bounds(box.GetMinPoint() - CVector3f(.2f, .2f, .2f),
                     box.GetMaxPoint() + CVector3f(.2f, .2f, .2f));
        bounds.AccumulateBounds(cache.GetCacheBounds().GetMinPoint());
        bounds.AccumulateBounds(cache.GetCacheBounds().GetMaxPoint());
        cache.SetCacheBounds(bounds);
        BuildAreaCollisionCache(mgr, cache);
      }
      rstl::vector< CMetroidAreaCollider::CTerrainTriangle > src;
      if (!StaticQuery::Allowed(filter)) {
        // No static half; the platforms below are actors.
      } else if (cache.HasCacheOverflowed()) {
        const CWorld* world = mgr.GetWorld();
        for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
             it != CWorld::skGlobalEnd; ++it) {
          CMetroidAreaCollider::AABoxCollisionCheck_CollectTriangles((*it).GetOctTree(), box,
                                                                     filter, src, true);
          if (groupEnds != NULL) groupEnds->push_back(size_t(src.size()));
        }
      } else {
        for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
          CMetroidAreaCollider::AABoxCollisionCheck_CollectTriangles(
              cache.GetOctreeLeafCache(i), box, filter, src, true);
          if (groupEnds != NULL) groupEnds->push_back(size_t(src.size()));
        }
      }
      tris.clear();
      tris.reserve(size_t(src.size()));
      for (int i = 0; i < int(src.size()); ++i) {
        const CMetroidAreaCollider::CTerrainTriangle& t = src[i];
        PortRemasteredGroundWorld::Triangle tri;
        tri.verts[0] = {t.x0_a.GetX(), t.x0_a.GetY(), t.x0_a.GetZ()};
        tri.verts[1] = {t.xc_b.GetX(), t.xc_b.GetY(), t.xc_b.GetZ()};
        tri.verts[2] = {t.x18_c.GetX(), t.x18_c.GetY(), t.x18_c.GetZ()};
        const CUnitVector3f n =
            CCollisionSurface(t.x0_a, t.xc_b, t.x18_c, t.x24_flags).GetNormal();
        tri.normal = {n.GetX(), n.GetY(), n.GetZ()};
        tri.materialBits = t.x24_flags;
        tris.push_back(tri);
      }
      // The admitted platforms' contacts follow the terrain's (FindOverlap 0xc1a760: static
      // tree first, then actors by `id & 0xffff`), as on the grounded mover.
      if (platforms != NULL) {
        PortPlatformTriangles(*platforms, filter, box, tris, NULL, boxes,
                              actor.GetMaterialList().GetValue());
      }
      query.box.min = {box.GetMinPoint().GetX(), box.GetMinPoint().GetY(),
                       box.GetMinPoint().GetZ()};
      query.box.max = {box.GetMaxPoint().GetX(), box.GetMaxPoint().GetY(),
                       box.GetMaxPoint().GetZ()};
      query.actorMaterialBits = actor.GetMaterialList().GetValue();
    }

    static bool Build(CStateManager& mgr, CAreaCollisionCache& cache, CPhysicsActor& actor,
                      const CMaterialFilter& filter, const CVector3f& delta,
                       std::vector< PortRemasteredGroundWorld::Contact >& out,
                       bool produceImpacts = true, const std::vector< const CScriptPlatform* >* platforms = NULL) {
      out.clear();
      if (!StaticQuery::Allowed(filter) && (platforms == NULL || platforms->empty())) return false;
      PortRemasteredGroundWorld::Query query;
      std::vector< PortRemasteredGroundWorld::Triangle > tris;
      std::vector< PortRemasteredGroundBox::Slot > boxes;
      Collect(mgr, cache, actor, filter, delta, query, tris, NULL, platforms, &boxes);
      if (tris.empty() && !PortRemasteredGroundBox::AnyContacts(boxes)) {
        return PortRemasteredGroundBox::AnyBlocked(boxes);
      }
      PortRemasteredGroundWorld::TerrainResult res =
          PortRemasteredGroundWorld::QueryTerrain(query, tris.data(), tris.size(), produceImpacts);
      // The box platforms' pairs (AABox_AABox 0xbf61b4 / 0xbfce60) in near-list order.
      res.booleanHit = res.booleanHit || PortRemasteredGroundBox::AnyBlocked(boxes);
      if (produceImpacts) PortRemasteredGroundBox::Merge(res.impacts, boxes);
      std::vector< const PortRemasteredGroundWorld::Contact* > consumer;
      PortRemasteredGroundWorld::ConsumerContacts(res.impacts, consumer);
      out.reserve(consumer.size());
      for (const PortRemasteredGroundWorld::Contact* c : consumer) {
        out.push_back(*c);
      }
      return res.booleanHit;
    }

    // Failsafe impacts from producer pairs (self contact, then its triangle's): the self
    // half's +0x14 depth (D, 0xc0b830) and the triangle half's normal. `ends` splits
    // the triangles into collided objects (one {first,last} range each).
    static void PairImpacts(const PortRemasteredGroundWorld::Result& res,
                            const std::vector< std::size_t >& ends,
                            const PortCollisionFailsafe::Vec3& otherCenter,
                            std::vector< PortCollisionFailsafe::Contact >& out) {
      std::vector< PortCollisionFailsafe::Contact > groups(ends.size());
      for (std::size_t i = 0; i + 1 < res.contacts.size(); ++i) {
        const PortRemasteredGroundWorld::Contact& self = res.contacts[i];
        const PortRemasteredGroundWorld::Contact& tri = res.contacts[i + 1];
        if (!self.self || tri.self || tri.triangle != self.triangle) continue;
        std::size_t g = 0;
        while (g < ends.size() && self.triangle >= ends[g]) ++g;
        if (g == ends.size()) continue;
        const CMaterialList mat(tri.materialBits);
        PortCollisionFailsafe::Impact impact;
        impact.depth = self.D;
        impact.otherNormal = {tri.normalX, tri.normalY, tri.normalZ};
        impact.otherCharacter = mat.HasMaterial(kMT_Character) ||
                                mat.HasMaterial(kMT_CollisionActor); // filter flags 0x41
        impact.otherCenter = otherCenter;
        groups[g].impacts.push_back(impact);
        ++i;
      }
      for (std::size_t g = 0; g < groups.size(); ++g) {
        if (!groups[g].impacts.empty()) out.push_back(groups[g]);
      }
    }

    // CheckCollision 0xc1ec60's static half for the failsafe core.
    static void Impacts(CStateManager& mgr, CAreaCollisionCache& cache, CPhysicsActor& actor,
                        const CMaterialFilter& filter, const CVector3f& delta,
                        std::vector< PortCollisionFailsafe::Contact >& out) {
      if (!StaticQuery::Allowed(filter)) return;
      PortRemasteredGroundWorld::Query query;
      std::vector< PortRemasteredGroundWorld::Triangle > tris;
      std::vector< std::size_t > ends;
      Collect(mgr, cache, actor, filter, delta, query, tris, &ends);
      if (tris.empty()) return;
      const PortRemasteredGroundWorld::TerrainResult res =
          PortRemasteredGroundWorld::QueryTerrain(query, tris.data(), tris.size(), true);
      PairImpacts(res.impacts, ends, PortCollisionFailsafe::Vec3(), out);
    }
  };

  struct Test {
    CStateManager& mgr;
    CAreaCollisionCache& cache;
    CPhysicsActor& actor;
    const CMaterialFilter& filter;
    const TEntityList& list; // the native (non-producer) actors
    const std::vector< const CScriptPlatform* >& platforms;
    bool Blocked(const CVector3f& offset) const {
      std::vector<PortRemasteredGroundWorld::Contact> unused;
      if (TerrainContacts::Build(mgr, cache, actor, filter, offset, unused, false, &platforms)) {
        return true;
      }
      actor.MoveCollisionPrimitive(offset);
      const bool b = DetectDynamicCollisionBoolean(*actor.GetCollisionPrimitive(),
                                                    actor.GetPrimitiveTransform(), list, mgr);
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      return b;
    }
  };
  const Test test = {mgr, cache, actor, materialFilter, nativeList, platforms};

  // 0xc6d110's dynamic half for a box (AABX) candidate. The source pair producer
  // (0xbf61b4) and its landing walk have only been proven for the one pair shape the
  // audit covers, so the adapter pre-selects it and every other shape (another
  // overlapping candidate, another primitive, a platform, a Door/Dock/BloodFlower,
  // an inactive actor, malformed bounds, a non-AABX self) falls through to the
  // unchanged retail block. Every one of those pre-select rejections is a PORT
  // conservative gate, never a claim about the source's physics-object type.
  struct BoxLanding {
    enum ESelection { kNone, kPair, kUnsupported };

    static PortCollisionFailsafe::Vec3 V(const CVector3f& v) {
      return {v.GetX(), v.GetY(), v.GetZ()};
    }
    static PortCollisionFailsafe::Box B(const CAABox& b) {
      return {V(b.GetMinPoint()), V(b.GetMaxPoint())};
    }
    // The port's filter as the source's CIncludeExcludeCollisionFilterMP1: the
    // include/exclude lists are MP1 material-list bits (the source's ctor 0xcb6f0c
    // stores exactly these), the mode is the filter's own.
    static PortRemasteredCollisionMaterial::MaterialFilter Filter(const CMaterialFilter& f) {
      return {f.GetIncludeList().GetValue(), f.GetExcludeList().GetValue(),
              static_cast< uint32_t >(f.GetFilterType())};
    }

    // The source's physics-object type 2 classes: CPhysicsActorMP1::SetupPhysicsObject
    // 0xc529f0 gives Door, Dock and BloodFlower type 2,
    // and CheckCollision 0xc1ed44-0xc1ed4c reads +0xc and branches straight to the pair
    // collision for type 2, skipping the other-filter visit test PairPasses always
    // applies. Rather than weaken that gate or invent a native type mapping, these
    // classes stay on the unchanged retail fallback. CBloodFlower has no TCastTo
    // registration (the visitor list in TCastTo.hpp never names it, and the port builds
    // without TypesMatch.cpp), so it is the one class cast by RTTI, which the port
    // already uses elsewhere (CStateManager.cpp, platform/port_savestate_room.cpp).
    static bool Type2Class(const CPhysicsActor& other) {
      return TCastToConstPtr< CScriptDoor >(&other) != NULL ||
             TCastToConstPtr< CScriptDock >(&other) != NULL ||
             dynamic_cast< const CBloodFlower* >(&other) != NULL;
    }

    // One near-list candidate. A candidate whose bounds cannot be compared is rejected
    // BEFORE the overlap test, so a malformed box cannot turn the query into a silent
    // "no candidate" by comparison accident.
    static PortRemasteredBoxLanding::Candidate Classify(
        const PortRemasteredBoxLanding::Box& selfBox,
        const PortRemasteredBoxLanding::Box& otherBox, const CPhysicsActor& other) {
      using namespace PortRemasteredBoxLanding;
      if (!FiniteOrdered(selfBox) || !FiniteOrdered(otherBox)) {
        return Candidate::kUnsupportedPair;
      }
      if (!BoxesOverlap(selfBox, otherBox)) return Candidate::kOutside;
      const CCollisionPrimitive* prim = other.GetCollisionPrimitive();
      if (prim == NULL || prim->GetPrimType() != 'AABX') return Candidate::kUnsupportedPair;
      if (TCastToConstPtr< CScriptPlatform >(&other) != NULL) {
        return Candidate::kUnsupportedPair;
      }
      if (Type2Class(other)) return Candidate::kUnsupportedPair;
      if (!other.GetActive()) return Candidate::kUnsupportedPair;
      return Candidate::kPair;
    }

    // Walks the provided near list in its given order (null, self and primitive-less
    // entries skipped as the retail path does) while the caller has the probe offset
    // applied, and reports the pure selection state. `pair` is snapshotted only for the
    // one accepted candidate; `low`/`result` are never touched here.
    static ESelection Select(CStateManager& mgr, CPhysicsActor& actor,
                             const CMaterialFilter& filter, const TEntityList& list, float feet,
                             PortRemasteredBoxLanding::Pair& pair) {
      using namespace PortRemasteredBoxLanding;
      const CCollisionPrimitive* selfPrim = actor.GetCollisionPrimitive();
      if (selfPrim == NULL || selfPrim->GetPrimType() != 'AABX') return kUnsupported;
      const Box selfBox = B(selfPrim->CalculateAABox(actor.GetPrimitiveTransform()));
      if (!FiniteOrdered(selfBox)) return kUnsupported;
      PairSelection selection(FiniteBand(feet));
      for (const TUniqueId* id = list.begin(); id != list.end(); ++id) {
        const CPhysicsActor* other = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id));
        if (other == NULL || other == &actor || other->GetCollisionPrimitive() == NULL) continue;
        const Box otherBox =
            B(other->GetCollisionPrimitive()->CalculateAABox(other->GetPrimitiveTransform()));
        const Candidate candidate = Classify(selfBox, otherBox, *other);
        selection.Candidate(candidate);
        if (candidate == Candidate::kPair &&
            selection.State() == SelectionState::kPair) {
          pair.selfBox = selfBox;
          pair.otherBox = otherBox;
          pair.selfMaterialBits = actor.GetMaterialList().GetValue();
          pair.otherMaterialBits = other->GetMaterialList().GetValue();
          pair.selfFilter = Filter(filter);
          pair.otherFilter = Filter(other->GetMaterialFilter());
          pair.feet = feet;
        }
      }
      switch (selection.State()) {
      case SelectionState::kPair:
        return kPair;
      case SelectionState::kNone:
        return kNone;
      case SelectionState::kUnsupported:
        break;
      }
      return kUnsupported;
    }
  };

  // 0xc6d110: landing checks consume impacts, not the NoPostFilter SAT result.
  // `hiZ` and `targetZ` are the heights of the callback's two motion states (see
  // PortAirMoverContact::GroundCheckBand): the probe reaches down to hiZ, at most
  // GetStepDownHeight() below the target, and the band runs from there to the box bottom
  // at the target (issue #147: with the band 0.002 deep and the probe 0.002 below the
  // accepted lo state, a floor that the bisection left up to a sixteenth of the step
  // below the player was never found, so a moving platform was never landed on).
  struct Ground {
    static bool Check(CStateManager& mgr, CAreaCollisionCache& cache, CPlayer& player,
                      const CMaterialFilter& filter, const TEntityList& list,
                      const std::vector< const CScriptPlatform* >& platforms, float hiZ, float targetZ) {
      CPhysicsActor& actor = player;
      if (player.GetPlayerMovementState() == NPlayer::kMS_OnGround) return true;
      if (player.GetFallingTime() <= .4f) return false;
      const float feet = actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform())
                             .GetMinPoint().GetZ() + targetZ - actor.GetTranslation().GetZ();
      const PortAirMoverContact::GroundBand band =
          PortAirMoverContact::GroundCheckBand(targetZ, player.GetStepDownHeight(), hiZ);
      const CVector3f delta(0.f, 0.f, band.probeUp - actor.GetTranslation().GetZ());
      std::vector<PortRemasteredGroundWorld::Contact> terrain;
      // Terrain, then the admitted platforms through the same producer; `list` holds the
      // remaining (native) actors for the dynamic half below.
      TerrainContacts::Build(mgr, cache, actor, filter, delta, terrain, true, &platforms);
      bool result = false;
      float low = band.low;
      for (const auto& c : terrain) {
        const auto work = PortAirMoverContact::WorkingNormal(
            {c.normalX,c.normalY,c.normalZ}, c.hasSecondary,
            {c.secondaryX,c.secondaryY,c.secondaryZ});
        for (std::size_t p=0; p<c.pointCount; ++p) {
          const float pz = c.pointVertical[p];
          if (pz > feet || pz <= low) continue;
          if (PortRemasteredGroundBox::IsBoxKey(c.triangle)) {
            // A box platform's half, as the box-actor landing walks it (0xc6d110 through
            // PortRemasteredBoxLanding::Apply): the translated material's is_floor.
            const PortRemasteredCollisionMaterial::CollisionMaterial m =
                PortRemasteredCollisionMaterial::TranslateActor(c.materialBits);
            result = PortRemasteredBoxLanding::FloorEligible(m) &&
                     PortRemasteredCollisionMaterial::IsFloor(m.orientation, work.z);
          } else {
            result = Classify::IsFloor(CMaterialList(c.materialBits),
                                       CVector3f(work.x,work.y,work.z));
          }
          low = pz;
        }
      }
      // The dynamic half. The source query needs the player's box AT THE PROBED
      // position and the retail block needs the probe offset back at zero before it
      // installs its own, so the pre-select runs under `delta` and is released here.
      // Exactly one eligible active non-platform AABox candidate makes the dynamic
      // landing source-exact (0xbf61b4 + 0xc6d110 through
      // platform/include/port_remastered_box_landing.h); anything else falls through
      // to the unchanged retail producer below, whose comment explains its shapes.
      actor.MoveCollisionPrimitive(delta);
      PortRemasteredBoxLanding::Pair pair;
      const BoxLanding::ESelection selection =
          BoxLanding::Select(mgr, actor, filter, list, feet, pair);
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      if (selection == BoxLanding::kPair) {
        // A pair either filter rejects, or one whose impacts are all filtered out, is
        // still the source's answer - no contact from this pair - so the retail
        // producer does not run for it either, and Apply's return is not consulted.
        static std::vector< PortCollisionFailsafe::PairedImpact > boxImpacts;  // game thread
        PortRemasteredBoxLanding::LandingState landing = {low, result};
        PortRemasteredBoxLanding::Apply(pair, landing, boxImpacts);
        low = landing.low;
        result = landing.result;
        return result;
      }
      if (selection == BoxLanding::kNone) {
        // No near-list primitive's box overlaps the probed box, and the retail
        // producer can only report a contact for an overlapping one, so the terrain's
        // result stands.
        return result;
      }
      actor.MoveCollisionPrimitive(delta);
      CCollisionInfoList dynamic;
      TUniqueId id = kInvalidUniqueId;
      DetectDynamicCollision(*actor.GetCollisionPrimitive(), actor.GetPrimitiveTransform(),
                             list, id, dynamic, mgr);
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      // A native box contact is the bounds of its clipped polygon
      // (CCollidableOBBTree::AABoxCollideWithLeaf): GetPoint() is the low corner,
      // GetExtreme() the high one. The source walks every contact point; the two
      // corners' heights are the polygon's lowest and highest points. The low corner
      // alone sits on the probed box bottom, the interval's open end, so a dynamic
      // floor (a moving platform) never grounded.
      // A box actor's side contact is different: retail AABoxAABoxIntersection gives
      // every axis contact the overlap box as its bounds, while Remastered's
      // CCollidableAABox::AABox_AABox (0xbf61b4 via 0xbf6650) gives each axis contact
      // one point, the centre of the overlap's face, so a side contact sits at the
      // overlap's mid-height. With the overlap's top as its point, a side contact ties
      // the top contact's height and, walked first, wins the strict `pz <= low`: a
      // player overhanging the edge of a box actor never landed. DetectDynamicCollision
      // stops at the first actor that collides, so every contact is `id`'s; a side
      // contact of a box (AABX) actor has a horizontal axis normal.
      const CPhysicsActor* other = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(id));
      const bool boxActor = other != NULL && other->GetCollisionPrimitive() != NULL &&
                            other->GetCollisionPrimitive()->GetPrimType() == 'AABX';
      for (int i=0; i<dynamic.GetCount(); ++i) {
        const auto& c = dynamic[i];
        const bool boxSide = boxActor && c.GetNormalLeft().GetZ() == 0.f;
        for (int k = 0; k < (boxSide ? 1 : 2); ++k) {
          const float pz = boxSide ? (c.GetPoint().GetZ() + c.GetExtreme().GetZ()) * .5f
                         : k == 0  ? c.GetPoint().GetZ()
                                   : c.GetExtreme().GetZ();
          if (pz > feet || pz <= low) continue;
          result = Classify::IsFloor(c.GetMaterialLeft(),c.GetNormalLeft());
          low = pz;
        }
      }
      return result;
    }
  };

  // 0xf948d4 DoMovementStep: returns the time consumed.
  // One movement step (0xf948d4). `consumedOut` is the time the step reports.
  struct Stepper {
    CStateManager& mgr;
    CAreaCollisionCache& cache;
    CPlayer& player;
    CPhysicsActor& actor;
    const CMaterialFilter& filter;
    const TEntityList& nearList;
    const Test& test;
    float mass;
    float worldDt;
    bool& hadCollision;
    bool& wallImpact;
    bool& grounded;
    CCollisionInfoList& groundList;
    TUniqueId& riderId;
    PortRemasteredGroundRider::CarryTerm& carry;
    PortRemasteredGroundRider::AirborneRider& airRider;
    const std::vector< const CScriptPlatform* >& platforms;
    // get_rider_controller_offset 0xc18f80 for the rider controller (+0x1210) and whether
    // that controller is valid this tick.
    PortAirMoverContact::Vec3 riderOffset;
    std::uint32_t riderOwner;
    bool riderActive;

    // The step's prediction (vt+0x20 -> PredictMotion 0xc23e40): read-only on the linear
    // state. Every DoMovementStep call passes extra = w4 & 1 (0xf9492c, w3 = w26 at each
    // vt+0x20 call), never 2, so the force branch (0xc23e88) is never taken: translation
    // t * (v + moveImpulse / m), zero linear momentum delta. The force is folded once per
    // tick by Post (IntegratePostForces below). The rider carry is added whole, unscaled by
    // t, to every prediction of the step while it is pending: the full step, each bisection
    // sample, hi and lo (lo at t = 0 too). Only the first step has it, with a turning
    // platform's rotation term (arm from the actor's position now) and yaw. The rest of the
    // angular part keeps the port's existing prediction (unverified, as on the ground mover).
    CMotionState Predict(float t) const {
      const CMotionState base = actor.PredictMotion_Internal(t);
      const CVector3f v = actor.GetVelocityWR();
      const CVector3f jm = actor.GetMoveImpulseWR();
      PortRemasteredGround::Vec3 d = PortRemasteredGround::PredictTranslation(
          t, {v.GetX(), v.GetY(), v.GetZ()}, {jm.GetX(), jm.GetY(), jm.GetZ()},
          1.f / actor.GetMass());
      d = carry.Apply(d, PortVec(actor.GetTranslation()));
      const CNUQuaternion orientation =
          carry.pending && carry.rotates
              ? PortYawedOrientationDelta(actor, base.GetOrientation(), carry.yaw)
              : base.GetOrientation();
      return CMotionState(CVector3f(d.x, d.y, d.z), orientation, CVector3f::Zero(),
                          base.GetAngularMomentum());
    }

    // 0xc6d460 ImpactResponseForOffGroundMoverCollider's per-contact response.
    // `primary` classifies (is_floor 0xc6d6b8/0xc6d6c0, is_wall 0xc6d7c4) and `working` is
    // the normal every force/projection uses: 0xc6d644-0xc6d65c stores the secondary over
    // the working normal whenever the presence byte at contact+0x44 is set, and no branch
    // restores the primary. `w25` (0xc6d69c / 0xc6d6b4) gates only the kMS_ApplyJump
    // gravity-and-force block at 0xc6d7f4 (`cbz w25,#0xc6d908`); the wall vertical halving
    // at 0xc6d7cc-0xc6d7f0 runs before that test.
    void Respond(const CMaterialList& material, const CVector3f& primary, const CVector3f& working,
                 bool w25, bool isFloor, float pointZ, CVector3f& mi, bool flag100 = false,
                 bool flatFloorEarlyOut = true) {
      CVector3f vel = actor.GetVelocityWR();
      if (flatFloorEarlyOut && PortAirMoverContact::SkipFlatFloor(primary.GetZ(), vel.GetZ(),
                                            actor.GetTranslation().GetZ(), pointZ, flag100)) {
        return;
      }
      if (!isFloor) {
        mi += working * (PortAirMoverContact::PushDistance(flag100) / worldDt) * mass;
      }
      const float miDot = CVector3f::Dot(working, mi);
      if (miDot < 0.f) {
        mi -= working * miDot; // 0xc186a0
      }
      const float velDot = CVector3f::Dot(working, vel);
      if (velDot <= 1.19e-7f) {
        vel -= working * velDot; // 0xc18700, e = mu = 0
      }
      if (player.GetPlayerMovementState() == NPlayer::kMS_ApplyJump) {
        const bool isWall = Classify::IsWall(material, primary);
        if (isWall && working.GetZ() < -0.1f && vel.GetZ() > 0.f) {
          vel.SetZ(vel.GetZ() * 0.5f);
        }
        if (w25) {
          float t1 = 0.25f;
          float t2 = 0.37f;
          float scale = 1.f;
          float mult = 1.f;
          float floorMag = 0.f;
          if (player.CheckSubmerged() &&
              !mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_GravitySuit)) {
            t1 = 0.01f;
            t2 = 0.05f;
            scale = 0.8f;
            mult = 35.f;
            floorMag = 5.f;
          }
          const float az = fabsf(working.GetZ());
          if (az > t1 && (az > t2 || vel.GetZ() < 0.f)) {
            const float mag = rstl::max_val(mult * az, floorMag);
            actor.SetForceWR(CVector3f(0.f, 0.f, 1.f) * (mag + 1.f) * mass * player.GetGravity());
            vel = vel * scale;
          }
        }
      }
      actor.SetVelocityWR(vel);
    }

    // 511-533: post-step 0.1 down probe while rising. The callback's first motion state is
    // the step's state moved 0.1 down (built at 0xf95348-0xf953b0 from skDownVector * 0.1),
    // the second the state itself (the call at 0xf953f4 passes them as x4 and x5).
    void PostProbe() {
      if (grounded || actor.GetVelocityWR().GetZ() <= 0.f) {
        return;
      }
      const CVector3f pos = actor.GetTranslation();
      if (Ground::Check(mgr, cache, player, filter, nearList, platforms, pos.GetZ() - 0.1f,
                        pos.GetZ())) {
        grounded = true;
      }
    }

    const CScriptPlatform* PlatformOf(std::uint32_t owner) const {
      if (owner == PortRemasteredGroundRider::kTerrainOwner) return NULL;
      for (std::size_t p = 0; p < platforms.size(); ++p) {
        if (PortRemasteredGroundRider::OwnerFromRaw(platforms[p]->GetUniqueId().value) == owner) {
          return platforms[p];
        }
      }
      return NULL;
    }

    float Step(float stepDt) {
      const CVector3f startPos = actor.GetTranslation();
      CMotionState motion = Predict(stepDt);
      // Save the ORIGINAL full-step target before bisection overwrites its
      // candidates (0xf94b30-50, 0xf94f34-0xf94fe4). Prediction is a delta in
      // the port; the source target is relative to previousTranslation.
      const CVector3f fullTarget = startPos + motion.GetTranslation();
      if (!test.Blocked(motion.GetTranslation())) {
        // The whole step is free (0xf948d4 line ~511).
        actor.AddMotionState(motion);
        actor.ImpulseWR() = CVector3f::Zero();
        PostProbe();
        return stepDt;
      }

      // Exactly four bisection tests: dt/2, then +-dt/4, dt/8, dt/16; the
      // reported time is the bracket midpoint, the state the last free one.
      float loT = 0.f;
      float hiT = stepDt;
      float t = stepDt * 0.5f;
      float half = stepDt * 0.5f;
      float reported = t;
      while (stepDt * 0.05f < half) {
        const bool blocked = test.Blocked(Predict(t).GetTranslation());
        half *= 0.5f;
        if (blocked) {
          hiT = t;
          t -= half;
        } else {
          loT = t;
          t += half;
        }
        reported = t;
      }

      // 0xf94d00: full CheckCollision at hi, distinct from NoPostFilter SAT.
      // Terrain impacts alone supply the static half; never query native static
      // contacts or associate contacts produced by different clipping rules.
      CCollisionInfoList list;
      TUniqueId id = kInvalidUniqueId;
      const CVector3f hiDelta = Predict(hiT).GetTranslation();
      actor.MoveCollisionPrimitive(hiDelta);
      TUniqueId dynamicId = kInvalidUniqueId;
      DetectDynamicCollision(*actor.GetCollisionPrimitive(), actor.GetPrimitiveTransform(),
                             nearList, dynamicId, list, mgr);
      if (dynamicId != kInvalidUniqueId) {
        id = dynamicId;
      }
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      std::vector< PortRemasteredGroundWorld::Contact > terrainContacts;
      TerrainContacts::Build(mgr, cache, actor, filter, hiDelta, terrainContacts, true, &platforms);

      if (!PortAirMoverContact::HasImpacts(terrainContacts.size(), list.GetCount())) {
        // No contact list: the state becomes hi (no response).
        actor.AddMotionState(Predict(hiT));
        actor.ImpulseWR() = CVector3f::Zero();
        PostProbe();
        return reported;
      }

      // Otherwise the state is lo; momentumless impulse seeded from 0x118c.
      actor.AddMotionState(Predict(loT));
      actor.ImpulseWR() = CVector3f::Zero();
      CVector3f mi = actor.GetMoveImpulseWR();
      const CVector3f stateVel = actor.GetVelocityWR();
      CPhysicsActor* const other = TCastToPtr< CPhysicsActor >(mgr.ObjectById(id));
      CVector3f riderVel = CVector3f::Zero();
      if (other != NULL && riderId != kInvalidUniqueId && id == riderId) {
        riderVel = other->GetVelocityWR();
      }

      // No "all responses rejected => whole dt" compensation: 0xf948d4
      // retains the bracket-reported time even when every impact separates
      // (0xf952d4-0xf95324). SAT touching and impact production are deliberately
      // different predicates, not grounds for overriding the reported time.
      bool floorContact = false;
      // 0xf948d4 local_1108: this step's rideable floor owner, reset per step (line 331).
      std::uint32_t stepRider = PortRemasteredGroundRider::kTerrainOwner;
      CCollisionInfoList accepted;
      CCollisionInfoList acceptedDynamic;
      for (int i = 0; i < list.GetCount(); ++i) {
        const CCollisionInfo& info = list[i];
        CVector3f n = info.GetNormalLeft();
        // Normal components below 2.38e-7 are zeroed (0xf948d4).
        if (fabsf(n.GetX()) < 2.38e-7f) n.SetX(0.f);
        if (fabsf(n.GetY()) < 2.38e-7f) n.SetY(0.f);
        if (fabsf(n.GetZ()) < 2.38e-7f) n.SetZ(0.f);
        // The paired-ceiling drop stays for the contacts this port still reasons about on
        // its own (dynamic actor contacts): 0xc6d460 has no such branch, so it is a known
        // divergence there, and it is deliberately not applied to the static terrain
        // response, which now runs from the producer's own contact list below.
        if (n.GetZ() < -0.70710677f) {
          bool pairedWithOther = false;
          for (int j = 0; j < list.GetCount(); ++j) {
            const CCollisionInfo& o = list[j];
            const CVector3f d = o.GetPoint() - info.GetPoint();
            if (j != i && o.GetNormalLeft().GetZ() >= -0.70710677f &&
                d.MagSquared() < 1e-8f) {
              pairedWithOther = true;
            }
          }
          if (pairedWithOther) continue;
        }
        // A contact the player is already separating from (relative to its
        // rider) is skipped.
        if (CVector3f::Dot(stateVel - riderVel, n) > 0.f) {
          continue;
        }
        acceptedDynamic.Add(info, false);
        hadCollision = true;
        if (Classify::IsWall(info.GetMaterialLeft(), n)) wallImpact = true;

        const bool isFloor = Classify::IsFloor(info.GetMaterialLeft(), n);
        if (isFloor) {
          floorContact = true;
          groundList.Add(info, false);
          if (other != NULL) {
            riderId = id;
            // Not static and can be ridden (+0x1220): of the port's dynamic actors only a
            // CScriptPlatform is ridden, so the rideable owner is a platform (INFERRED).
            if (TCastToConstPtr< CScriptPlatform >(other) != NULL) {
              stepRider = PortRemasteredGroundRider::OwnerFromRaw(id.value);
            }
          }
        }

        // Defer response, preserving native actor acceptance. Rejected dynamic
        // entries must not leak into the response pass (AUDIT-3 F3).
      }
      // The static terrain response runs from the producer's own contact list: one contact
      // per triangle the shipped builder retains (0xc0a180), each with the secondary normal
      // it computes. Driving the response from that stream is what keeps a contact's
      // secondary attached to the contact it belongs to - the native list's inclusive
      // clipping (CMetroidAreaCollider::ConvexPolyCollision, height >= 0) and the
      // producer's strict clipping can disagree on a triangle, so the two streams must
      // never be zipped by index.
      for (std::size_t t = 0; t < terrainContacts.size(); ++t) {
        const PortRemasteredGroundWorld::Contact& tc = terrainContacts[t];
        CVector3f primary(tc.normalX, tc.normalY, tc.normalZ);
        const auto p = PortAirMoverContact::Vec3{primary.GetX(),primary.GetY(),primary.GetZ()};
        const auto s = PortAirMoverContact::Vec3{tc.secondaryX,tc.secondaryY,tc.secondaryZ};
        const auto work = PortAirMoverContact::WorkingNormal(p,tc.hasSecondary,s);
        const auto gate = PortAirMoverContact::WorkingNormal(
            PortAirMoverContact::GatePrimary(p),tc.hasSecondary,s);
        // Secondary components bypass primary cleanup (0xf94fb0-0xf94fd8).
        const CVector3f working(work.x,work.y,work.z);
        const bool w25 = PortAirMoverContact::W25(tc.hasSecondary,p,s);
        const CMaterialList material(tc.materialBits);
        // (target-previous) - (current-previous) - riderOffset: previous
        // cancels. CURRENT IS THE PRE-STEP STATE: the source does not apply
        // its selected motion until 0xf95420 (decompile line 538). The port
        // has already applied lo to expose its predicted velocity to Respond,
        // so reading actor.GetTranslation() here would silently substitute
        // the accepted bisection sample (AUDIT-3 F1) for the source operand.
        // riderOffset is subtracted for the rider controller's own contacts only (0xf948d4
        // 354-362); static terrain has no owner, so it is zero there.
        const auto disp = PortAirMoverContact::RemainingDisplacement(
            {fullTarget.GetX(),fullTarget.GetY(),fullTarget.GetZ()},
            {startPos.GetX(),startPos.GetY(),startPos.GetZ()});
        if (!PortAirMoverContact::Acts(
                disp,
                PortAirMoverContact::RiderOffsetFor(tc.ownerUid, riderOwner, riderActive,
                                                    riderOffset),
                gate)) {
          continue;
        }
        // A box platform's half carries its translated material's orientation (the source
        // classifies every contact by it); terrain and tree contacts keep the stand-in.
        const bool boxContact = PortRemasteredGroundBox::IsBoxKey(tc.triangle);
        const auto floorFor = [&](const CVector3f& n) {
          return boxContact ? PortRemasteredCollisionMaterial::IsFloor(tc.orientation, n.GetZ())
                            : Classify::IsFloor(material, n);
        };
        const bool isFloor = floorFor(CVector3f(gate.x,gate.y,gate.z)) && !tc.flag100;
        const CScriptPlatform* const owner = PlatformOf(tc.ownerUid);
        if (owner != NULL) {
          // The accepted platform contact is the step's collided actor for the native
          // messaging, as the native contact used to be (first collided actor wins).
          if (id == kInvalidUniqueId) id = owner->GetUniqueId();
          if (isFloor) {
            // Not static and rideable (+0x1220): a CScriptPlatform (INFERRED, as below).
            riderId = owner->GetUniqueId();
            stepRider = tc.ownerUid;
          }
        }
        // Retain native game messaging, but derive its static entries ONLY
        // from accepted source impacts. Use an actual produced point, never
        // a native contact paired by index or a fabricated point.
        if (tc.pointCount != 0) {
          const auto& point = tc.points[0];
          // Retail convention, as the native dynamic infos: the 2nd ctor
          // argument lands in matLeft, which must be the touched surface.
          const CCollisionInfo event(CVector3f(point.x,point.y,point.z), actor.GetMaterialList(),
                                     material, primary);
          accepted.Add(event, false);
          if (isFloor) groundList.Add(event, false);
        }
        if (isFloor) floorContact = true;
        hadCollision = true;
        if (boxContact ? PortRemasteredCollisionMaterial::IsWall(tc.orientation, primary.GetZ())
                       : Classify::IsWall(material, primary)) {
          wallImpact = true;
        }
        // A DCLN platform is a CCollidableOBBoxTree (type 4): no flat-floor early-out. A box
        // platform is a CCollidableAABox (type 0), which has it.
        Respond(material, primary, working, w25,
                floorFor(primary) && !tc.flag100, tc.minVertical, mi, tc.flag100,
                owner == NULL || PortAirMoverContact::FlatFloorEarlyOutApplies(
                                     boxContact ? PortAirMoverContact::kCollidableAABox
                                                : PortAirMoverContact::kCollidableOBBoxTree,
                                     false));
      }
      // Preserve the port's terrain-then-near-list order. The source iterates
      // candidate/impact order (0xf94e40-0xf9521c), but does not establish a
      // universal static-first world order. Actor primitives, ownership,
      // pairing and velocity-relative acceptance stay native; there is no
      // source actor-secondary/rider-displacement producer here.
      for (int i = 0; i < acceptedDynamic.GetCount(); ++i) {
        const CCollisionInfo& info = acceptedDynamic[i];
        accepted.Add(info, false);
        CVector3f n = info.GetNormalLeft();
        if (fabsf(n.GetX()) < 2.38e-7f) n.SetX(0.f);
        if (fabsf(n.GetY()) < 2.38e-7f) n.SetY(0.f);
        if (fabsf(n.GetZ()) < 2.38e-7f) n.SetZ(0.f);
        const bool isFloor = Classify::IsFloor(info.GetMaterialLeft(), n);
        Respond(info.GetMaterialLeft(), n, n, true, isFloor, info.GetPoint().GetZ(), mi);
      }
      // SetMomentumlessImpulse(local_10c0), colliding branch only (line 503).
      actor.SetMoveImpulseWR(mi);

      if (accepted.GetCount() > 0) {
        MakeCollisionCallbacks(mgr, actor, id, accepted);
      }
      if (floorContact && !grounded) {
        // The callback gets the bisection's last blocked (hi) sample and the accepted lo
        // state (0xf9524c: x4 = sp+0x100, x5 = sp+0xc8).
        if (Ground::Check(mgr, cache, player, filter, nearList, platforms,
                          startPos.GetZ() + hiDelta.GetZ(),
                          actor.GetTranslation().GetZ())) {
          grounded = true;
          airRider.Landed(stepRider); // `*param_7 = local_1108` (line 493)
        }
        return reported; // a floor contact skips the post-probe (line 509)
      }
      if (!floorContact) {
        PostProbe();
      }
      return reported;
    }
  };
  Stepper stepper = {mgr,  cache,       player,      actor,    materialFilter,
                     nativeList, test,  mass,        dt,       hadCollision, wallImpact,
                     grounded, groundList, riderId, carry, airRider, platforms,
                     riderOffset, player.PortRider().owner, player.PortRider().Active()};

  // CPhysicsCollisionFailsafe (platform/port_collision_failsafe.cpp) over the mover's
  // near list. Filters (CPhysicsActorMP1::SetupPhysicsObject 0xc529f0): the collision
  // filter 0x1270 is the material filter, the failsafe filter 0x1290 is Include{Solid}
  // Exclude{Character, CollisionActor}, the result filter 0x1280 passes everything.
  struct Failsafe {
    CStateManager& mgr;
    CAreaCollisionCache& cache;
    CPhysicsActor& actor;
    const CMaterialFilter& materialFilter;
    const CMaterialFilter failsafeFilter;
    const TEntityList& nearList;
    float worldDt;
    // Scratch for Collide's tree walk, reused across calls.
    mutable std::vector< PortRemasteredGroundWorld::Triangle > treeTris;
    mutable std::vector< const COBBTree::CNode* > treeStack;

    static PortCollisionFailsafe::Vec3 V(const CVector3f& v) {
      return {v.GetX(), v.GetY(), v.GetZ()};
    }
    static CVector3f C(const PortCollisionFailsafe::Vec3& v) {
      return CVector3f(v.x, v.y, v.z);
    }
    static PortCollisionFailsafe::Box B(const CAABox& b) {
      return {V(b.GetMinPoint()), V(b.GetMaxPoint())};
    }
    const CMaterialFilter& Filter(bool failsafe) const {
      return failsafe ? failsafeFilter : materialFilter;
    }
    CAABox BoxAt(const CVector3f& pos) const {
      actor.MoveCollisionPrimitive(pos - actor.GetTranslation());
      const CAABox b = actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform());
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      return b;
    }
    // CheckCollision 0xc1ec60 visits another object when its own collision filter passes
    // us; the filter given to GetCollision then gates the other's material.
    const CPhysicsActor* Other(TUniqueId id, const CMaterialFilter& filter) const {
      const CPhysicsActor* other = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(id));
      if (other == NULL || other == &actor || other->GetCollisionPrimitive() == NULL) return NULL;
      if (!other->GetMaterialFilter().Passes(actor.GetMaterialList())) return NULL;
      if (!filter.Passes(other->GetMaterialList())) return NULL;
      return other;
    }

    // 0xc1ec60 with `filter`, the box at `pos`: terrain through the producer, box actors
    // through AABox_AABox 0xbf61b4, tree actors through the producer per triangle.
    // SPHR uses AABox_Sphere 0xc02d70. ABSH has no construction site in this port.
    bool Collide(const CMaterialFilter& filter, const CVector3f& pos,
                 std::vector< PortCollisionFailsafe::Contact >& out) const {
      out.clear();
      TerrainContacts::Impacts(mgr, cache, actor, filter, pos - actor.GetTranslation(), out);
      const CAABox self = BoxAt(pos);
      for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
        const CPhysicsActor* other = Other(*id, filter);
        if (other == NULL) continue;
        const CCollisionPrimitive& prim = *other->GetCollisionPrimitive();
        const CTransform4f primXf = other->GetPrimitiveTransform();
        const CAABox bounds = prim.CalculateAABox(primXf);
        const CMaterialList& otherMat = other->GetMaterialList();
        const bool character =
            otherMat.HasMaterial(kMT_Character) || otherMat.HasMaterial(kMT_CollisionActor);
        if (prim.GetPrimType() == 'AABX') {
          PortCollisionFailsafe::Contact c;
          if (!PortCollisionFailsafe::BoxBoxImpacts(B(self), B(bounds), c.impacts)) continue;
          for (std::size_t i = 0; i < c.impacts.size(); ++i) {
            c.impacts[i].otherCharacter = character;
            c.impacts[i].otherCenter = V(bounds.GetCenterPoint());
          }
          out.push_back(c);
        } else if (prim.GetPrimType() == 'SPHR') {
          const CSphere sphere = static_cast< const CCollidableSphere& >(prim).Transform(primXf);
          PortCollisionFailsafe::Contact c;
          if (!PortCollisionFailsafe::BoxSphereImpacts(B(self), V(sphere.GetCenter()),
                                                       sphere.GetRadius(), c.impacts)) continue;
          // Remastered's CPhysicsState ctor 0xc14f7c / SetCollisionMaterial 0xc1536c
          // copy the actor's translated material into its primitive. Native joint
          // spheres retain Solid|NoStaticCollision instead, so use the actor flags.
          for (std::size_t i = 0; i < c.impacts.size(); ++i) {
            c.impacts[i].otherCharacter = character;
            c.impacts[i].otherCenter = V(sphere.GetCenter());
          }
          out.push_back(c);
        } else if (prim.GetPrimType() == 'OBTG') {
          const CCollidableOBBTreeGroup& group = static_cast< const CCollidableOBBTreeGroup& >(prim);
          std::vector< PortRemasteredGroundWorld::Triangle >& tris = treeTris;
          tris.clear();
          // Nodes are culled against the self box in tree space (as the retail
          // AABoxCollision walks do), grown by a margin so a triangle that only touches
          // the box still reaches the producer.
          const CVector3f margin(.01f, .01f, .01f);
          const COBBox selfObb = COBBox::FromAABox(
              CAABox(self.GetMinPoint() - margin, self.GetMaxPoint() + margin),
              primXf.GetQuickInverse());
          for (int t = 0; t < group.GetContainer()->NumTrees(); ++t) {
            const COBBTree& tree = *group.GetOBBTreeAABox(t);
            std::vector< const COBBTree::CNode* >& stack = treeStack;
            stack.clear();
            stack.push_back(&tree.GetRoot());
            while (!stack.empty()) {
              const COBBTree::CNode* node = stack.back();
              stack.pop_back();
              if (!selfObb.OBBIntersectsBox(node->GetOBB())) continue;
              if (!node->IsLeaf()) {
                stack.push_back(&node->GetRightNode());
                stack.push_back(&node->GetLeftNode());
                continue;
              }
              const rstl::vector< ushort >& surfs = node->GetLeafData().GetSurfaceVector();
              for (int s = 0; s < int(surfs.size()); ++s) {
                const CCollisionSurface surf = tree.GetTransformedSurface(surfs[s], primXf);
                const u64 bits = u64(surf.GetSurfaceFlags()) | prim.GetMaterial().GetValue();
                if (!filter.Passes(CMaterialList(bits))) continue;
                PortRemasteredGroundWorld::Triangle tri;
                for (int k = 0; k < 3; ++k) tri.verts[k] = {surf.GetVert(k).GetX(),
                                                           surf.GetVert(k).GetY(),
                                                           surf.GetVert(k).GetZ()};
                const CVector3f n = surf.GetNormal();
                tri.normal = {n.GetX(), n.GetY(), n.GetZ()};
                tri.materialBits = bits;
                tris.push_back(tri);
              }
            }
          }
          if (tris.empty()) continue;
          PortRemasteredGroundWorld::Query query;
          query.box.min = {self.GetMinPoint().GetX(), self.GetMinPoint().GetY(),
                           self.GetMinPoint().GetZ()};
          query.box.max = {self.GetMaxPoint().GetX(), self.GetMaxPoint().GetY(),
                           self.GetMaxPoint().GetZ()};
          query.actorMaterialBits = actor.GetMaterialList().GetValue();
          const PortRemasteredGroundWorld::TerrainResult res =
              PortRemasteredGroundWorld::QueryTerrain(query, tris.data(), tris.size(), true);
          std::vector< std::size_t > ends(1, tris.size());
          const std::size_t before = out.size();
          TerrainContacts::PairImpacts(res.impacts, ends, V(bounds.GetCenterPoint()), out);
          if (character && out.size() > before) {
            for (std::size_t i = 0; i < out.back().impacts.size(); ++i) {
              out.back().impacts[i].otherCharacter = true;
            }
          }
        }
      }
      return !out.empty();
    }

    // CheckGlobalCollision_Boolean_NoPostFilter 0xc1f240: the inclusive SAT path.
    bool Blocked(const CMaterialFilter& filter, const CVector3f& pos) const {
      std::vector< PortRemasteredGroundWorld::Contact > unused;
      const CVector3f delta = pos - actor.GetTranslation();
      // Build (Collect) moves the primitive by `delta` and back itself.
      if (TerrainContacts::Build(mgr, cache, actor, filter, delta, unused, false)) return true;
      actor.MoveCollisionPrimitive(delta);
      bool hit = false;
      for (const TUniqueId* id = nearList.begin(); id != nearList.end() && !hit; ++id) {
        const CPhysicsActor* other = Other(*id, filter);
        if (other == NULL) continue;
        hit = CCollisionPrimitive::CollideBoolean(
            CInternalCollisionStructure::CPrimDesc(*actor.GetCollisionPrimitive(),
                                                   CMaterialFilter::skPassEverything,
                                                   actor.GetPrimitiveTransform()),
            CInternalCollisionStructure::CPrimDesc(*other->GetCollisionPrimitive(),
                                                   CMaterialFilter::skPassEverything,
                                                   other->GetPrimitiveTransform()));
      }
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      return hit;
    }

    // RayIntersection 0xc20810 with the collision filter: the smallest t below the
    // current length, stopping at |t| < 1e-5. Terrain hits are code 2, front faces only
    // (mode 1, TriangleNoBackface 0xbf9360). Character and
    // CollisionActor objects are left out (FindOverlap Exclude{0x41}). The mover's near
    // list stands in for 0xc20214's swept-box broadphase.
    PortCollisionFailsafe::RayHit Ray(const PortCollisionFailsafe::Vec3& start,
                                      const PortCollisionFailsafe::Vec3& dir, float length) const {
      PortCollisionFailsafe::RayHit best;
      float bestT = length;
      if (StaticQuery::Allowed(materialFilter)) {
        const CRayCastResult r =
            RayStaticIntersection(mgr, C(start), C(dir), length, materialFilter, true);
        if (r.IsValid() && r.GetTime() < bestT) {
          bestT = r.GetTime();
          best.code = PortCollisionFailsafe::kRay_Hit;
          best.t = r.GetTime();
          best.normal = V(r.GetPlane().GetNormal());
          best.orientation = PortRemasteredGroundWorld::Orientation(r.GetMaterial().GetValue());
        }
      }
      for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
        if (best.code != PortCollisionFailsafe::kRay_Miss && fabsf(best.t) < 1e-5f) break;
        const CPhysicsActor* other = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id));
        if (other == NULL || other == &actor || other->GetCollisionPrimitive() == NULL) continue;
        const CMaterialList& otherMat = other->GetMaterialList();
        if (otherMat.HasMaterial(kMT_Character) || otherMat.HasMaterial(kMT_CollisionActor) ||
            !materialFilter.Passes(otherMat)) {
          continue;
        }
        const CCollisionPrimitive& prim = *other->GetCollisionPrimitive();
        PortCollisionFailsafe::RayHit hit;
        if (prim.GetPrimType() == 'AABX') {
          hit = PortCollisionFailsafe::RayAABox(B(prim.CalculateAABox(other->GetPrimitiveTransform())),
                                                start, dir, bestT);
        } else if (prim.GetPrimType() == 'SPHR') {
          // CCollidableSphere::RayCast 0xc0289c -> NCollisionRayCast::Sphere 0xbf6ac0:
          // the sphere's translated centre and untouched radius, including the
          // start-inside code the generic cast below does not report.
          const CSphere sphere =
              static_cast< const CCollidableSphere& >(prim).Transform(other->GetPrimitiveTransform());
          hit = PortCollisionFailsafe::RaySphere(V(sphere.GetCenter()), sphere.GetRadius(), start,
                                                 dir, bestT);
        } else {
          // Other primitives through the port's ray cast; a start inside is not reported
          // as code 1 (unverified against the source's per-type casts).
          const CRayCastResult r =
              prim.CastRay(C(start), C(dir), bestT, materialFilter, other->GetPrimitiveTransform());
          if (r.IsValid()) {
            hit.code = PortCollisionFailsafe::kRay_Hit;
            hit.t = r.GetTime();
            hit.normal = V(r.GetPlane().GetNormal());
          }
        }
        if (hit.code == PortCollisionFailsafe::kRay_Miss || !(hit.t < bestT)) continue;
        hit.orientation = PortRemasteredGroundWorld::Orientation(otherMat.GetValue());
        bestT = hit.t;
        best = hit;
      }
      return best;
    }

    // 0xc20214 for the actor's current bounds. Runs every airborne tick: the length test
    // comes first so the usual short step builds no hooks.
    bool Tunnel(const CVector3f& from, const CVector3f& to, CVector3f& out) const {
      const CAABox bounds =
          actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform());
      if (!PortCollisionFailsafe::TunnelingFailsafeApplies(B(bounds), V(from), V(to))) {
        out = to;
        return false;
      }
      const PortCollisionFailsafe::Vec3 pos = V(actor.GetTranslation());
      PortCollisionFailsafe::TunnelHooks hooks;
      hooks.ray = [this](const PortCollisionFailsafe::Vec3& s, const PortCollisionFailsafe::Vec3& d,
                         float len) { return Ray(s, d, len); };
      hooks.safeOffset = [this, &bounds, &pos](const PortCollisionFailsafe::Vec3& at, float maxDist,
                                               PortCollisionFailsafe::Vec3& offset) {
        // 0xc21740 -> 0xc217a0 with the collision filter, the box placed at `at`.
        PortCollisionFailsafe::Box box = B(bounds);
        const PortCollisionFailsafe::Vec3 shift = {at.x - pos.x, at.y - pos.y, at.z - pos.z};
        box.min = {box.min.x + shift.x, box.min.y + shift.y, box.min.z + shift.z};
        box.max = {box.max.x + shift.x, box.max.y + shift.y, box.max.z + shift.z};
        PortCollisionFailsafe::SafeHooks safe;
        safe.collidesAt = [this](const PortCollisionFailsafe::Vec3& p) {
          return Blocked(materialFilter, C(p));
        };
        safe.rayBlocked = [this](const PortCollisionFailsafe::Vec3& s,
                                 const PortCollisionFailsafe::Vec3& d, float len) {
          const PortCollisionFailsafe::RayHit h = Ray(s, d, len);
          return h.code != PortCollisionFailsafe::kRay_Miss && h.t < len;
        };
        return PortCollisionFailsafe::CalculateOffsetToSafeCollisionPoint(box, at, maxDist, safe,
                                                                          offset);
      };
      PortCollisionFailsafe::Vec3 result;
      const bool moved =
          PortCollisionFailsafe::DoTunnelingFailsafe(B(bounds), pos, V(from), V(to), hooks, result);
      out = C(result);
      return moved;
    }

    // 0xc1fa68: sets the translation when it reports resolved.
    bool Collision() const {
      const CVector3f cur = actor.GetTranslation();
      auto core = [this, &cur](bool failsafe, PortCollisionFailsafe::Vec3& out) {
        PortCollisionFailsafe::CoreHooks hooks;
        hooks.collide = [this, failsafe](const PortCollisionFailsafe::Vec3& p,
                                         std::vector< PortCollisionFailsafe::Contact >& list) {
          return Collide(Filter(failsafe), C(p), list);
        };
        hooks.tunnel = [this](const PortCollisionFailsafe::Vec3& f,
                              const PortCollisionFailsafe::Vec3& t,
                              PortCollisionFailsafe::Vec3& o) {
          CVector3f r;
          const bool moved = Tunnel(C(f), C(t), r);
          o = V(r);
          return moved;
        };
        return PortCollisionFailsafe::Core(V(cur), worldDt, hooks, out);
      };
      auto blocked = [this](bool failsafe, const PortCollisionFailsafe::Vec3& p) {
        return Blocked(Filter(failsafe), C(p));
      };
      PortCollisionFailsafe::Vec3 out;
      const bool resolved = PortCollisionFailsafe::DoCollisionFailsafe(V(cur), true, core, blocked, out);
      if (resolved) {
        actor.SetTranslation(C(out));
      }
      return resolved;
    }
  };
  const Failsafe failsafe = {
      mgr, cache, actor, materialFilter,
      CMaterialFilter::MakeIncludeExclude(CMaterialList(kMT_Solid),
                                          CMaterialList(kMT_Character, kMT_CollisionActor)),
      nearList, dt};

  // 0xf943b4 MoveObject: one step, then up to ten more while time remains.
  // The stepped mover's entry writes the persisted object state 0 unconditionally
  // (0xf9450c): this is the tick that restores validity after any invalidating path (morph
  // ball, the ground dynamic fallback, a save-state load).
  player.PortSetRetainedObjectState(PortRemasteredGround::RetainedAirborneEntry(
      player.PortRetainedObjectState()));
  // Pre 0xc59814 (integrator vt+0x10, called at 0xf9447c), once per tick before the first
  // step: momentum += impulse, no force and no dt (kb CPhysicsActorMP1-integrator-0xc59814).
  // The impulse is spent; the steps never fold it again.
  {
    const CVector3f m = actor.GetConstantForceWR();
    const CVector3f j = actor.GetImpulseWR();
    const PortRemasteredGround::Vec3 r = PortRemasteredGround::PreMomentum(
        {m.GetX(), m.GetY(), m.GetZ()}, {j.GetX(), j.GetY(), j.GetZ()});
    actor.SetConstantForceWR(CVector3f(r.x, r.y, r.z));
    actor.ComputeDerivedQuantities();
    actor.ImpulseWR() = CVector3f::Zero();
  }
  // 0xf94480-0xf944b8: the rise cap follows Pre, so it sees the folded impulse.
  if (player.GetPlayerMovementState() != NPlayer::kMS_OnGround) {
    PortCapRise(mgr, player);
  }
  CVector3f prevVel = actor.GetVelocityWR();
  const CVector3f entryVel = prevVel;
  float remaining = dt - stepper.Step(dt);
  // The first step applied its prediction, and the carried yaw with it.
  if (carry.pending && carry.rotates && carry.yaw != 0.f) {
    player.PortNoteCarriedYaw(carry.yaw);
  }
  carry.ConsumeAfterPass();
  bool stalledFlag = false;
  if (remaining > 1e-5f) {
    int iter = 0;
    int stallCount = 0;
    for (;;) {
      if (grounded) {
        break;
      }
      const float consumed = stepper.Step(remaining);
      bool keep;
      if (iter < 5 || fabsf(consumed) >= 1e-5f) {
        ++iter;
        keep = true;
        if (close_enough(prevVel, actor.GetVelocityWR(), 1e-4f)) {
          stalledFlag = true;
          const bool second = stallCount > 0;
          ++stallCount;
          if (second) {
            keep = false;
          }
        }
        if (keep) {
          prevVel = actor.GetVelocityWR();
        }
      } else {
        keep = false;
      }
      remaining -= consumed;
      if (!keep || iter > 9 || remaining <= 1e-5f) {
        break;
      }
    }
  }

  // 0xf94720: SetRiderController with the rider the landing step wrote, when valid.
  if (airRider.Controller() != PortRemasteredGroundRider::kTerrainOwner) {
    player.PortRider().Set(airRider.Controller());
  }

  // 0xc545bc PostPhysicsUpdate calls CPlayerMP1::CollidedWithNew 0xcf1a2c (vptr+0x190, slot
  // 50) with the tick's collision list whenever it is non-empty, before the OnFloor/Falling
  // messages. It clears slidingOnWall (0x1074 bit 0x1000), and on the first is_wall impact of
  // a collision info sets it again when the movement state (0x3f0) is ApplyJump, then calls
  // SetPlayerHitWallDuringMove 0xc75394 (0x2000, acceleration index 1). Retail's
  // CPlayer::CollidedWith does neither (CGroundMovement did), so without this ComputeDash
  // kept steering an orbiting jump into a steep slope and the wall projection lifted her.
  // With no collision the call is skipped, so the flag keeps its value.
  if (hadCollision) {
    player.SetPlayerIsSlidingOnWall(false);
    if (wallImpact) {
      if (player.GetPlayerMovementState() == NPlayer::kMS_ApplyJump) {
        player.SetPlayerIsSlidingOnWall(true);
      }
      player.SetPlayerHitWallDuringMove();
    }
  }

  // 0xc545bc CPhysicsActorMP1::PostPhysicsUpdate lands on the mover's object
  // state alone: state 1 (grounded) sends OnFloor (0x17) with no second floor
  // test. Retail SendScriptMessages re-tests each contact with IsFloor
  // (n.z > 0.85), which refuses the 0.7071..0.85 floors the mover grounded
  // on, so the player never landed there and the loop froze (issue #117).
  // Surface messages come from 0xcf1a2c CollidedWithNew for floor contacts.
  if (grounded) {
    for (int i = 0; i < groundList.GetCount(); ++i) {
      SendMaterialMessage(mgr, groundList[i].GetMaterialLeft(), actor);
    }
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_OnFloor);
    // 1 alongside the kSM_OnFloor just delivered (the mover's grounded step, DoMovementStep
    // 0xf95414), and exactly when it is delivered: the port's grounded condition IS the
    // message's condition here (both read the mover's object state, no second floor test),
    // so it is passed as true rather than re-derived from the landing list.
    player.PortSetRetainedObjectState(PortRemasteredGround::RetainedAirborneLanding(
        player.PortRetainedObjectState(), true));
    // 0xcf1a2c CPlayerMP1::CollidedWithNew: a floor impact whose owner is the
    // player's rider controller sends AddPlatformRider (0x1f) to the platform,
    // from the player, and to the player, from the platform. The rider
    // controller is the dynamic floor the mover took (object +0x1210, 0xf948d4).
    CScriptPlatform* const platform =
        riderId != kInvalidUniqueId ? TCastToPtr< CScriptPlatform >(mgr.ObjectById(riderId)) : NULL;
    if (platform != NULL) {
      mgr.DeliverScriptMsg(platform, actor.GetUniqueId(), kSM_AddPlatformRider);
      mgr.DeliverScriptMsg(&actor, platform->GetUniqueId(), kSM_AddPlatformRider);
    }
  }
  // 0xc545bc sends OnFloor for object state 1 and Falling only for state 0, never both. A
  // landing found by the post-step probe has no collision, and Falling after the OnFloor
  // above would put the player straight back into kMS_Falling for a tick.
  if (!grounded && !hadCollision) {
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_Falling);
  }
  if (!(fabsf(remaining) < 1e-5f && !stalledFlag)) {
    // 0xf94750 CPhysicsCollisionFailsafe::DoCollisionFailsafe 0xc1fa68: push out of
    // what the actor overlaps. The mover keeps no last free state to fall back on.
    const bool resolved = failsafe.Collision();
    // 0xf943b4: a failsafe that did not reposition the actor, with the whole
    // dt still unconsumed, stops it dead.
    if (!resolved && fabsf(remaining - dt) < 1e-5f) {
      actor.SetVelocityWR(CVector3f::Zero());
    }
    // 0xf943b4: falling fast (entry up-velocity < -0.75) and still speeding
    // up although the actor did not move down, vertical velocity is zeroed.
    const CVector3f endVel = actor.GetVelocityWR();
    if (entryVel.GetZ() < -0.75f && endVel.GetZ() < entryVel.GetZ() &&
        -0.001f < actor.GetTranslation().GetZ() - entryPos.GetZ()) {
      actor.SetVelocityWR(CVector3f(endVel.GetX(), endVel.GetY(), 0.f));
    }
  }
  // 0xf947f4-0xf9485c, every tick: DoTunnelingFailsafe 0xc20214 from the entry
  // position to the current one; a hit places the actor and stops it.
  CVector3f tunnelOut;
  if (failsafe.Tunnel(entryPos, actor.GetTranslation(), tunnelOut)) {
    actor.SetTranslation(tunnelOut);
    actor.SetVelocityWR(CVector3f::Zero());
  }
  // Post 0xc5994c (integrator vt+0x18, called at 0xf94884), once per tick after the steps,
  // the collision failsafe and the tunnel check, on every outcome: momentum += dt * (force +
  // persistent), with the whole tick's dt whatever the steps consumed. A stopped actor
  // (failsafe or tunnel zeroed its velocity) still gets this tick's force.
  {
    const CVector3f m = actor.GetConstantForceWR();
    const CVector3f f = actor.GetForceWR();
    const CVector3f pm = actor.GetMomentumWR();
    const PortRemasteredGround::Vec3 r = PortRemasteredGround::PostMomentum(
        {m.GetX(), m.GetY(), m.GetZ()}, {f.GetX(), f.GetY(), f.GetZ()},
        {pm.GetX(), pm.GetY(), pm.GetZ()}, dt);
    actor.SetConstantForceWR(CVector3f(r.x, r.y, r.z));
    actor.ComputeDerivedQuantities();
  }
  actor.ClearForcesAndTorques();
  actor.MoveCollisionPrimitive(CVector3f::Zero());
}
#endif

#ifdef TARGET_PC
// ---------------------------------------------------------------------------
// Remastered grounded player mover (issue #117 / #114)
//
// Source: CPhysicsGroundMoverColliderMP1::MoveObject 0xf923d8 (loop limits, final
// snap, rider), MoveAlongGround 0xf92ccc (two-sample search, per-contact response,
// impulse reset), CanMoveUpAndDownFromPosition 0xf928c0, IsOnWallAtPosition
// 0xf93c9c, SnapToGround 0xf934ec, and the box/triangle contact builder 0xc0a180.
//
// Every decision comes from the pure policy in platform/include/
// port_remastered_ground_policy.h and every contact number from the source producer
// over platform/port_remastered_ground_world.cpp. This file is only the glue: the
// port's world traversal, physics state and script messages.
//
// NOT MODELLED, deliberately (see slopes/implementation/runtime/REPORT.md):
//  - Dynamic/near-list contacts other than moving platforms. A CScriptPlatform with
//    a collision tree (DCLN) whose orientation did not change this tick is ADMITTED (issue
//    #117 slice 1): its leaf triangles go through the same terrain producer 0xc0a180 with
//    the surface material merged with the object's (AABox_OBBoxTree 0xc0fc18, traversal
//    0xc0fce0), appended after the terrain contacts in near-list order (FindOverlap
//    0xc1a760), in every query below except the tunnelling failsafe's ray, which stays
//    terrain-only (whether the source ray sees platforms is UNKNOWN). A platform without
//    a tree (its CCollidableAABox) is admitted on the same terms: its box/box pair
//    (AABox_AABox 0xbf61b4, boolean 0xbfce60) enters the same lists as self/other contact
//    pairs (platform/include/port_remastered_ground_box.h). For anything else
//    (actors, doors, rotating platforms) no Remastered producer is
//    ported; the port's CCollisionInfo (clipped AABB + n/-n only) does not carry
//    Remastered's penetration scalars or the optional secondary normal. Substituting
//    retail contacts or inventing penetration values is refused; a tick whose query
//    region overlaps such a collidable is handed to the port's existing grounded mover
//    instead (the single, named, counted UnsupportedDynamicContactsFallback below), so an
//    actor touch cannot be silently ignored. That fallback also INVALIDATES the retained
//    object state, because a retail mover's outcomes are not source mover outcomes.
//  - The morph ball's swept mover (0xc255fc) and its floor producers (0xc261a8,
//    0xc26234). The ball is the retail standard collider, so MovePlayer's entry gate
//    (the ball is never kMS_Unmorphed) invalidates the retained object state on every
//    ball tick instead of guessing a floor producer. GROUND RULE: a tick outside the
//    gate — classic movement, direct aim/mouse controls off, morph ball — and every
//    path that leaves the source value unproduced (a dynamic contact, a save-state
//    load) leaves the value invalid, and an invalid value makes the dispatch below take
//    the port's retail grounded mover. So after a morph, a dynamic contact or a
//    save-state load, Remastered ground replacement stays off until the next verified
//    airborne tick (which writes 0 unconditionally, 0xf9450c). The value is tracked
//    only for movers the gate vouches for; it is not a claim of parity with every tick
//    (see ground/retained-state-fix/REPORT.md).
//  - The tree object's material contribution to the terrain contact: the source ORs
//    the collision tree object's behavior/filter into the triangle's
//    (0xbeac58-0xbeac7c). The port's static area path keeps no per-tree material,
//    and the only behavior bit any consumer reads is 0x100, whose object-level
//    producers in the shipped image are DefaultCollisionMaterial (behavior 0,
//    GROUND-INPUTS.md 2.3); its absence is therefore recorded, not replaced.
//  - Rotation carry and the rider-velocity term of PredictMotion 0xc23e40. Slice 1 ports
//    the rider scan after Post (0xf92808-0xf92860, SetRiderController 0xc1631c count 2),
//    its decay (PostSimulation) and the translation carry (pose delta, unscaled, consumed
//    by the first MoveAlongGround pass, 0xf9349c); a platform that rotates this tick is not
//    admitted. Retail CScriptPlatform::MoveRiders' player move is deferred and dropped on
//    an admitted tick, applied on every other (PortRemasteredGroundRider::PlanCarry).
//  - DoCollisionFailsafe (0xf923d8:205-216, outer 0xc1fb88, inner 0xc1fc54): PORTED,
//    TERRAIN ONLY, by RunFailsafe below over port_collision_failsafe.{h,cpp}
//    (Core/DoCollisionFailsafe/DoTunnelingFailsafe 0xc20214, the 0xbf61b4 box routine and
//    the 0xbf7e50 ray). This REPLACES the earlier "NOT ported / blocked" note, whose four
//    blockers are now resolved or bounded:
//      * the inner routine's first virtual (takes a `2`, returns bool, fills a
//        CCollisionInfoList) is the world's CheckCollision 0xc1ec60 — the port supplies it
//        as the `collide` hook, and its false (nothing collided) ends the loop. It is NOT
//        the mover integrator's slot +0x10: that vptr (0x20e7340) is a local integrator
//        class whose +0x10 is PreApplyForces 0xc59814 (DISPATCH-AUDIT.md §2), which the
//        split contract below has already applied once this tick, so the failsafe does not
//        call it again.
//      * the per-contact filter behaviour bits (+0x2c & 0x41) are carried by the adapter
//        (`PortCollisionFailsafe::Impact::otherCharacter`, from the terrain material's
//        Character/CollisionActor bits in FsCollide).
//      * DoTunnelingFailsafe 0xc20214 and the rays/offsets it needs have their own port.
//      * the outer wrapper's second half is the core's `failsafe` filter argument
//        (0x1270 then 0x1290) and its unchanged-transform return is the usual refusal.
//    NOT exercised on this path, because every contact, ray and probe here is terrain:
//    the shipped ray/offset Character/CollisionActor exclusion and the ray start-inside
//    cases (the ground FsRay reports hit/miss only). Dynamic contacts cannot reach it: a
//    tick that overlaps a collidable took the fallback above.
//  - The tick's integrator contract is the SPLIT one (DISPATCH-AUDIT.md §2-§4), which
//    supersedes the earlier "which slot applies the force is unknown" reading: Pre
//    0xc59814 folds the impulse at entry only (no force, no dt), PredictMotion 0xc23e40 is
//    read-only on the linear state — translation t * (v + moveImpulse / m), the mover passes
//    extra = 1 so no force branch — and Post 0xc5994c folds dt * (force + persistent) AFTER
//    the loop and the snap, on every snap outcome. The angular halves of both slots
//    (angular impulse +0x98 through inverse inertia +0xec, torque +0xe0/+0xc8) are NOT
//    ported: the port keeps its own angular prediction in GroundTick::Predict, unverified
//    against them. Also unverified: the contact traversal ORDER (the port's octree
//    leaf-cache order, not the source's AABoxTree/bounds-tree order — CONTACT-LISTS.md D1,
//    observable only with two accepted contacts of different priority, and the wall/floor
//    passes are order-sensitive) and anything of the failsafe's loop/exit order beyond
//    "called on every exit, before SnapToGround, with from = the last prediction"
//    (0xf926b8 / 0xf926e8).

namespace {

using PortRemasteredGround::Contact;
using PortRemasteredGround::GroundSearch;
using PortRemasteredGround::Probe;
using PortRemasteredGround::Snap;

// 0xf923d8:126 (FLT_EPSILON).
constexpr float kMoveDtEpsilon = 1.1920929e-07f;

// A tick that overlaps another collidable is handled by the port's grounded mover.
// Counted so the unsupported region is measurable rather than invisible.
uint gUnsupportedDynamicContactTicks = 0;
// A tick whose collision-list query outgrew the adapter's former fixed capacity.
// Informational only: the adapter grows, the shipped list has no failure path.
uint gRemasteredGroundContactGrowthTicks = 0;
// A tick run against an overflowed octree leaf cache, i.e. with the uncached
// traversal below. Informational only: the mover never changes on overflow.
uint gRemasteredGroundCacheOverflowTicks = 0;

// The policy's view of a contact, from the terrain side of a source contact pair.
// All height/normal consumers (0xf928c0, 0xf93c9c, 0xf934ec) pick the contact whose
// +0x30 is NOT the object, and read its primary or secondary normal, its material
// (orientation + behavior bit 0x100) and its clipped point range from there. Port
// z is Remastered's y, so the vertical component is normalZ.
inline Contact TerrainView(const PortRemasteredGroundWorld::Contact& c) {
  Contact out;
  out.orientation = c.orientation;
  out.normalX = c.normalX;
  out.normalUp = c.normalZ;
  // Contact +0x14. Zero for a terrain contact: the builder stores both scalars as
  // zero for the triangle contact (0xc0b7fc-0xc0b888, `stur xzr,[sp,#0xe4]`).
  out.scalar14 = c.D;
  out.flag100 = c.flag100;
  out.hasSecondary = c.hasSecondary;
  out.secondaryUp = c.secondaryZ;
  out.minHeight = c.minVertical;
  out.maxHeight = c.maxVertical;
  return out;
}

// The list shape the three height/normal consumers see: ONE contact per 0x90 pair,
// the side whose +0x30 owner is not the queried object (0xf928c0:66-70,
// 0xf93c9c:311-315, 0xf934ec:546-549 all walk the pairs with stride 0x90 and read
// only that side). `source` keeps the adapter contact so a landing can take its
// material and its normal; the response loop is the only consumer that reads both
// sides of a pair (0xf92ccc:316-320: the other side supplies +0x14).
struct PairView {
  std::vector< Contact > policy;
  std::vector< const PortRemasteredGroundWorld::Contact* > source;

  void Build(const PortRemasteredGroundWorld::Result& contacts) {
    source.clear();
    PortRemasteredGroundWorld::ConsumerContacts(contacts, source);
    policy.clear();
    policy.reserve(source.size());
    for (const PortRemasteredGroundWorld::Contact* c : source) {
      policy.push_back(TerrainView(*c));
    }
  }
};

// One tick's worth of the port side of the source mover.
struct GroundTick {
  CStateManager& mgr;
  CPlayer& player;
  CPhysicsActor& actor;
  CAreaCollisionCache& cache;
  const CMaterialFilter& filter;
  const TEntityList& nearList;
  // Mover +8 / +0xc, snapshotted from the shipped tweak at construction
  // (CPhysicsActorMP1 ctor 0xc5213c <- SetupPhysicsObject 0xc53100-0xc5311c). The
  // state-dependent CPlayerMP1 accessors (0xcf25e4 / 0xcf2614) are NOT what the
  // mover holds, so the base fields are called non-virtually.
  const float stepUp;
  const float stepDown;
  // Set when any query's contact list outgrew the adapter's former capacity; the
  // tick counter reports it, nothing about the movement changes.
  bool grewContacts = false;
  // The floor the snap selected: its material feeds the port's surface-restraint
  // message and its normal the last-floor-plane-normal, as the retail mover does
  // (the source's SnapToGround only sets its object state).
  std::uint64_t floorMaterial = 0;
  CVector3f floorNormal = CVector3f(0.f, 0.f, 1.f);
  // The admitted moving platforms (DCLN or box), in near-list order: every query appends
  // their contacts after the terrain's (FindOverlap 0xc1a760: static tree first, then actors).
  std::vector< const CScriptPlatform* > platforms;
  // PredictMotion's rider carry, pending until the first MoveAlongGround pass.
  PortRemasteredGroundRider::CarryTerm carry;
  // The mover's result list (+0x12a0) the rider scan after Post reads (0xf92808): empty
  // when the tick starts, then every MoveAlongGround pass's list and the snap's appended.
  PortRemasteredGroundWorld::Result riderContacts;
  // PlatformBlocked's triangles, reused by every boolean query of the tick.
  mutable std::vector< PortRemasteredGroundWorld::Triangle > blockedScratch;

  const CVector3f Up() const { return CVector3f(0.f, 0.f, 1.f); }

  static PortRemasteredGroundWorld::Query QueryFor(const CAABox& box, const CPhysicsActor& a) {
    PortRemasteredGroundWorld::Query query;
    query.box.min = {box.GetMinPoint().GetX(), box.GetMinPoint().GetY(), box.GetMinPoint().GetZ()};
    query.box.max = {box.GetMaxPoint().GetX(), box.GetMaxPoint().GetY(), box.GetMaxPoint().GetZ()};
    query.actorMaterialBits = a.GetMaterialList().GetValue();
    return query;
  }

  // The admitted platforms' triangles for `box`, appended to `tris` platform by platform
  // (AABox_OBBoxTree 0xc0fc18 / traversal 0xc0fce0: LIFO, root untested, children in index
  // order). Each triangle carries the surface material merged with the object's and its
  // owner; `filter` stands for the player's side of SurfaceCollisionPasses (vt+0x18), as the
  // retail tree collider applies it per triangle. `ends` receives one end index per platform.
  // Box platforms add their slots to `boxes` (PortPlatformTriangles).
  void PlatformTriangles(const CMaterialFilter& f, const CAABox& box,
                         std::vector< PortRemasteredGroundWorld::Triangle >& tris,
                         std::vector< std::size_t >* ends,
                         std::vector< PortRemasteredGroundBox::Slot >* boxes,
                         bool wantContacts = true) const {
    PortPlatformTriangles(platforms, f, box, tris, ends, boxes, actor.GetMaterialList().GetValue(),
                          wantContacts);
  }

  // The boolean twin (0xbfd2f0 / 0xbfd360 for trees, 0xbfce60 for boxes): the inclusive
  // SAT, first hit wins.
  bool PlatformBlocked(const CMaterialFilter& f, const CAABox& box) const {
    if (platforms.empty()) return false;
    std::vector< PortRemasteredGroundWorld::Triangle >& tris = blockedScratch;
    tris.clear();
    std::vector< PortRemasteredGroundBox::Slot > boxes;
    PlatformTriangles(f, box, tris, NULL, &boxes, false);
    if (PortRemasteredGroundBox::AnyBlocked(boxes)) return true;
    if (tris.empty()) return false;
    return PortRemasteredGroundWorld::QueryTerrain(QueryFor(box, actor), tris.data(), tris.size(),
                                                   false)
        .booleanHit;
  }

  // The actor's collision box with its primitive placed `delta` from the actor.
  CAABox BoxAt(const CVector3f& delta) {
    actor.MoveCollisionPrimitive(delta);
    const CAABox box =
        actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform());
    actor.MoveCollisionPrimitive(CVector3f::Zero());
    return box;
  }

  // 0xf923d8's integrator predict (vtable+0x20) for the next `dt` seconds. The port's
  // convention (MoveAirborneStepped) is the same: the motion state's translation is
  // the delta from the actor's current state.
  // PredictMotion 0xc23e40 as the ground mover calls it (extra 0/1): read-only on the
  // linear state, translation t * (v + moveImpulse / m), zero linear momentum delta.
  // The angular part keeps the port's existing prediction (player angular path
  // unverified). The impulse was folded in once by IntegrateEntryImpulse, the force after the snap.
  CMotionState Predict(float dt) const {
    const CMotionState base = actor.PredictMotion_Internal(dt);
    const CVector3f v = actor.GetVelocityWR();
    const CVector3f jm = actor.GetMoveImpulseWR();
    const PortRemasteredGround::Vec3 d = PortRemasteredGround::PredictTranslation(
        dt, {v.GetX(), v.GetY(), v.GetZ()}, {jm.GetX(), jm.GetY(), jm.GetZ()},
        1.f / actor.GetMass());
    // The rider carry (0xc23e40, mode 1/2), whole and unscaled, until the first pass applied
    // it: a turning platform's rotation term about its previous position, the arm from the
    // actor's position now (S+0x18), and its yaw.
    const PortRemasteredGround::Vec3 c = carry.Apply(d, PortVec(actor.GetTranslation()));
    const CNUQuaternion orientation =
        carry.pending && carry.rotates
            ? PortYawedOrientationDelta(actor, base.GetOrientation(), carry.yaw)
            : base.GetOrientation();
    return CMotionState(CVector3f(c.x, c.y, c.z), orientation, CVector3f::Zero(),
                        base.GetAngularMomentum());
  }

  // Pre 0xc59814, once per tick: only the linear impulse (see the policy header).
  void IntegrateEntryImpulse() {
    const CVector3f m = actor.GetConstantForceWR();
    const CVector3f j = actor.GetImpulseWR();
    const PortRemasteredGround::Vec3 r = PortRemasteredGround::PreMomentum(
        {m.GetX(), m.GetY(), m.GetZ()}, {j.GetX(), j.GetY(), j.GetZ()});
    actor.SetConstantForceWR(CVector3f(r.x, r.y, r.z));
    actor.ComputeDerivedQuantities();
  }

  // Post 0xc5994c, once per tick after the loop and the snap and before the transient clear and
  // the state messages, on every snap outcome: the actor's CURRENT momentum (refused keeps the
  // pass momentum, no-floor and grounded the restored saved state) += dt * (force + persistent).
  void IntegratePostForces(float dt) {
    const CVector3f m = actor.GetConstantForceWR();
    const CVector3f f = actor.GetForceWR();
    const CVector3f p = actor.GetMomentumWR();
    const PortRemasteredGround::Vec3 r = PortRemasteredGround::PostMomentum(
        {m.GetX(), m.GetY(), m.GetZ()}, {f.GetX(), f.GetY(), f.GetZ()}, {p.GetX(), p.GetY(), p.GetZ()},
        dt);
    actor.SetConstantForceWR(CVector3f(r.x, r.y, r.z));
    actor.ComputeDerivedQuantities();
  }

  // The live areas' octrees, the same chain BuildAreaCollisionCache caches and
  // DetectStaticCollisionBoolean walks when no usable cache exists.
  void CollectTrianglesUncached(const CAABox& box,
                                rstl::vector< CMetroidAreaCollider::CTerrainTriangle >& out) {
    const CWorld* world = mgr.GetWorld();
    for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
         it != CWorld::skGlobalEnd; ++it) {
      CMetroidAreaCollider::AABoxCollisionCheck_CollectTriangles((*it).GetOctTree(), box, filter,
                                                                 out);
    }
  }

  // Terrain first, then the admitted platforms (the near-list order).
  bool StaticBlocked(const CVector3f& delta) {
    const CAABox box = BoxAt(delta);
    if (cache.HasCacheOverflowed()) {
      const CWorld* world = mgr.GetWorld();
      for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
           it != CWorld::skGlobalEnd; ++it) {
        if (CMetroidAreaCollider::AABoxCollisionCheckBoolean((*it).GetOctTree(), box, filter)) {
          return true;
        }
      }
      return PlatformBlocked(filter, box);
    }
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      if (CMetroidAreaCollider::AABoxCollisionCheckBoolean_Cached(cache.GetOctreeLeafCache(i), box,
                                                                 filter)) {
        return true;
      }
    }
    return PlatformBlocked(filter, box);
  }

  // CheckCollision_Boolean_NoPostFilter (0xf928c0 / 0xf934ec). Dynamic overlaps other than
  // the admitted platforms are already excluded for the whole tick by the caller.
  bool Blocked(const CVector3f& delta) { return StaticBlocked(delta); }

  // The terrain half of CheckCollision with Remastered's contact contract.
  bool TerrainContacts(const CVector3f& delta, PortRemasteredGroundWorld::Result& out) {
    out = PortRemasteredGroundWorld::Result();
    const CAABox box = BoxAt(delta);
    rstl::vector< CMetroidAreaCollider::CTerrainTriangle > src;
    if (cache.HasCacheOverflowed()) {
      // The cached leaves are incomplete, and the shipped mover queries the world
      // directly with no leaf cache at all (CheckCollision 0xc1e9b0 ->
      // CheckCollision_Boolean_NoPostFilter 0xc1f110 walk the object's own candidate
      // list per call). Dropping leaves would drop terrain contacts, so the same
      // traversal runs over the octree itself — the port's own convention for an
      // overflowed cache (DetectStaticCollisionBoolean_Cached, CGameCollision.cpp:236).
      CollectTrianglesUncached(box, src);
    } else {
      for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
        CMetroidAreaCollider::AABoxCollisionCheck_CollectTriangles(cache.GetOctreeLeafCache(i), box,
                                                                   filter, src);
      }
    }
    if (src.size() == 0 && platforms.empty()) {
      return false;
    }
    // The retail surface normal is the same unit normal the retail collision path
    // uses for this triangle (CCollisionSurface::GetNormal, cross(b-a, c-a)).
    std::vector< PortRemasteredGroundWorld::Triangle > tris;
    tris.reserve(size_t(src.size()));
    for (int i = 0; i < src.size(); ++i) {
      const CMetroidAreaCollider::CTerrainTriangle& t = src[i];
      PortRemasteredGroundWorld::Triangle tri;
      tri.verts[0] = PortRemasteredGroundWorld::Vec3f{t.x0_a.GetX(), t.x0_a.GetY(), t.x0_a.GetZ()};
      tri.verts[1] = PortRemasteredGroundWorld::Vec3f{t.xc_b.GetX(), t.xc_b.GetY(), t.xc_b.GetZ()};
      tri.verts[2] = PortRemasteredGroundWorld::Vec3f{t.x18_c.GetX(), t.x18_c.GetY(),
                                                      t.x18_c.GetZ()};
      const CUnitVector3f n =
          CCollisionSurface(t.x0_a, t.xc_b, t.x18_c, t.x24_flags).GetNormal();
      tri.normal = PortRemasteredGroundWorld::Vec3f{n.GetX(), n.GetY(), n.GetZ()};
      tri.materialBits = t.x24_flags;
      tris.push_back(tri);
    }
    // The admitted platforms' contacts follow the terrain's; box platforms' pairs are merged
    // into the triangle result at their near-list place.
    std::vector< PortRemasteredGroundBox::Slot > boxes;
    PlatformTriangles(filter, box, tris, NULL, &boxes);
    if (tris.empty() && !PortRemasteredGroundBox::AnyContacts(boxes)) {
      return false;
    }
    PortRemasteredGroundWorld::Query query;
    query.box.min = PortRemasteredGroundWorld::Vec3f{box.GetMinPoint().GetX(),
                                                     box.GetMinPoint().GetY(),
                                                     box.GetMinPoint().GetZ()};
    query.box.max = PortRemasteredGroundWorld::Vec3f{box.GetMaxPoint().GetX(),
                                                     box.GetMaxPoint().GetY(),
                                                     box.GetMaxPoint().GetZ()};
    query.actorMaterialBits = actor.GetMaterialList().GetValue();
    out = PortRemasteredGroundWorld::BuildContacts(query, tris.data(), tris.size());
    PortRemasteredGroundBox::Merge(out, boxes);
    if (out.grewBeyondInlineCapacity) {
      // The adapter grew past its former fixed capacity. The shipped list grows on
      // the heap with no failure path, so this is only counted, never acted on.
      grewContacts = true;
    }
    return out.contacts.size() != 0;
  }

  // 0xf928c0 CanMoveUpAndDownFromPosition's stored extrema, as the policy takes them.
  Probe StepProbeAt(const PortRemasteredGroundWorld::Result& contacts, const CVector3f& delta) {
    const CAABox box = BoxAt(delta);
    PairView view;
    view.Build(contacts);
    return PortRemasteredGround::StepProbe(box.GetMinPoint().GetZ(), box.GetMaxPoint().GetZ(),
                                           view.policy.data(), view.policy.size(), stepUp, stepDown);
  }

  // 0xf93c9c IsOnWallAtPosition: probe the candidate translated down by the step-down
  // height, then ask whether its highest contact is a floor.
  bool OnWallAt(const CVector3f& delta) {
    const CVector3f down = delta - Up() * stepDown;
    PortRemasteredGroundWorld::Result contacts;
    const bool collides = TerrainContacts(down, contacts);
    if (!collides) {
      return false;
    }
    PairView view;
    view.Build(contacts);
    return PortRemasteredGround::OnWall(true, view.policy.data(), view.policy.size());
  }

  // Free(t) from 0xf92ccc:190-195. The tested transform's contacts feed both the
  // up/down probe and the wall test.
  bool FreeAt(float t) {
    const CVector3f delta = Predict(t).GetTranslation();
    PortRemasteredGroundWorld::Result contacts;
    if (TerrainContacts(delta, contacts)) {
      const Probe probe = StepProbeAt(contacts, delta);
      if (!probe.allowed) {
        return false;
      }
      if (Blocked(delta + Up() * probe.offset)) {
        return false;
      }
    }
    return !OnWallAt(delta);
  }

  // 0xf92ccc:163-213: the two-sample free search, then the per-contact response at
  // the colliding sample, then the free sample's state and the impulse reset.
  // D1: MoveAlongGround wrote its param_6 (0xf92ffc-0xf9301c) on this pass: the colliding
  // path only, with the position it applied to the actor.
  bool wroteTarget = false;

  float MoveAlongGround(float dt) {
    wroteTarget = false;
    const GroundSearch search = PortRemasteredGround::SearchGroundTime(
        dt, [this](float t) { return FreeAt(t); });

    PortRemasteredGroundWorld::Result contacts;
    const bool blockedCollides =
        TerrainContacts(Predict(search.blockedSample).GetTranslation(), contacts);
    // This pass's collision list, appended to the mover's result list (0xf923d8 ~L187).
    PortRemasteredGroundRider::AppendContacts(riderContacts, contacts);
    const PortRemasteredGround::GroundSample pick =
        PortRemasteredGround::DecideGroundSample(search.anyFree, blockedCollides, search.consumed);

    CVector3f velocityImpulse = CVector3f::Zero();
    CVector3f momentumless = CVector3f::Zero();
    if (pick.applyResponse) {
      // One info per overlapping triangle, each with its own and the terrain contact
      // (0xf92ccc:305-401). The response classifies the side whose +0x30 is not the
      // object and reads the depth priority from the object's own side (+0x14).
      rstl::vector< PortRemasteredGround::ResponseContact > list;
      for (std::size_t i = 0; i < contacts.contacts.size(); ++i) {
        const PortRemasteredGroundWorld::Contact& terrain = contacts.contacts[i];
        if (terrain.self) {
          continue;
        }
        float depth = 0.f;
        for (std::size_t j = 0; j < contacts.contacts.size(); ++j) {
          if (contacts.contacts[j].self && contacts.contacts[j].triangle == terrain.triangle) {
            depth = contacts.contacts[j].D;
            break;
          }
        }
        PortRemasteredGround::ResponseContact c;
        c.nx = terrain.hasSecondary ? terrain.secondaryX : terrain.normalX;
        c.ny = terrain.hasSecondary ? terrain.secondaryY : terrain.normalY;
        c.nz = terrain.hasSecondary ? terrain.secondaryZ : terrain.normalZ;
        c.orientation = terrain.orientation;
        c.depth = depth;
        list.push_back(c);
      }
      const CVector3f velocity = actor.GetVelocityWR();
      const CVector3f acc = actor.GetMoveImpulseWR();
      const PortRemasteredGround::GroundResponse r = PortRemasteredGround::ResolveGroundResponse(
          list.size() ? &list[0] : nullptr, std::size_t(list.size()),
          {velocity.GetX(), velocity.GetY(), velocity.GetZ()}, {acc.GetX(), acc.GetY(), acc.GetZ()},
          actor.GetMass());
      velocityImpulse = CVector3f(r.velocityImpulse.x, r.velocityImpulse.y, r.velocityImpulse.z);
      momentumless = CVector3f(r.momentumless.x, r.momentumless.y, r.momentumless.z);
    }

    // The sample's state, at the current position when no sample was free
    // (0xf92ccc:409-417); then the response on top (0xf92ccc:404-423).
    CMotionState state =
        Predict(pick.useBlockedSample ? search.blockedSample : search.freeSample);
    if (pick.keepCurrentPosition) {
      state.SetTranslation(CVector3f::Zero());
    }
    actor.AddMotionState(state);
    // 0xf9349c: the rider carry is in this applied state; later predicts leave it out.
    carry.ConsumeAfterPass();
    wroteTarget = pick.applyResponse;
    if (pick.applyResponse) {
      actor.SetVelocityWR(actor.GetVelocityWR() + velocityImpulse);
      actor.SetMoveImpulseWR(actor.GetMoveImpulseWR() + momentumless);
    }
    // 0xf93478-0xf93498: cleared on every path.
    actor.ImpulseWR() = CVector3f::Zero();
    actor.SetAngularImpulseWR(CAxisAngle::Identity());
    return pick.returnedTime;
  }

  // ---- G2: the post-loop DoCollisionFailsafe 0xc1fb88 (terrain only) ----
  // The static half of Remastered's CheckCollision / CheckGlobalCollision_Boolean_NoPostFilter
  // over the same producer the airborne mover uses: the triangles are gathered as
  // candidates (the inclusive SAT 0xc04740 decides, `candidatesOnly`), and impacts come
  // from QueryTerrain. Dynamic contacts are NOT produced: a tick whose query region
  // overlaps another collidable never reaches this mover (guarded retail fallback).
  static bool StaticAllowed(const CMaterialFilter& f) {
    return !f.GetExcludeList().HasMaterial(kMT_NoStaticCollision);
  }
  static PortCollisionFailsafe::Vec3 FsV(const CVector3f& v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
  }
  static CVector3f FsC(const PortCollisionFailsafe::Vec3& v) { return CVector3f(v.x, v.y, v.z); }

  // Triangles for the box placed `delta` from the actor. `ends` receives each collided
  // area tree's end index (one collided object per area). A box that leaves the leaf
  // cache, or an overflowed cache, walks the live octrees instead (the cache is never
  // rebuilt mid-tick), the port's convention for an incomplete cache.
  void FsCollect(const CMaterialFilter& f, const CVector3f& delta,
                 PortRemasteredGroundWorld::Query& query,
                 std::vector< PortRemasteredGroundWorld::Triangle >& tris,
                 std::vector< std::size_t >* ends) {
    const CAABox box = BoxAt(delta);
    rstl::vector< CMetroidAreaCollider::CTerrainTriangle > src;
    if (!StaticAllowed(f)) {
      // No terrain half; the admitted platforms below still collide.
    } else if (cache.HasCacheOverflowed() || !box.Inside(cache.GetCacheBounds())) {
      const CWorld* world = mgr.GetWorld();
      for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
           it != CWorld::skGlobalEnd; ++it) {
        CMetroidAreaCollider::AABoxCollisionCheck_CollectTriangles((*it).GetOctTree(), box, f, src,
                                                                   true);
        if (ends != NULL) ends->push_back(size_t(src.size()));
      }
    } else {
      for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
        CMetroidAreaCollider::AABoxCollisionCheck_CollectTriangles(cache.GetOctreeLeafCache(i), box,
                                                                   f, src, true);
        if (ends != NULL) ends->push_back(size_t(src.size()));
      }
    }
    tris.clear();
    tris.reserve(size_t(src.size()));
    for (int i = 0; i < int(src.size()); ++i) {
      const CMetroidAreaCollider::CTerrainTriangle& t = src[i];
      PortRemasteredGroundWorld::Triangle tri;
      tri.verts[0] = {t.x0_a.GetX(), t.x0_a.GetY(), t.x0_a.GetZ()};
      tri.verts[1] = {t.xc_b.GetX(), t.xc_b.GetY(), t.xc_b.GetZ()};
      tri.verts[2] = {t.x18_c.GetX(), t.x18_c.GetY(), t.x18_c.GetZ()};
      const CUnitVector3f n = CCollisionSurface(t.x0_a, t.xc_b, t.x18_c, t.x24_flags).GetNormal();
      tri.normal = {n.GetX(), n.GetY(), n.GetZ()};
      tri.materialBits = t.x24_flags;
      tris.push_back(tri);
    }
    // The admitted platforms, one collided object each, after the area trees. Box platforms
    // have no triangles here (an empty range); FsBoxes collides them.
    PlatformTriangles(f, box, tris, ends, NULL);
    query.box.min = {box.GetMinPoint().GetX(), box.GetMinPoint().GetY(), box.GetMinPoint().GetZ()};
    query.box.max = {box.GetMaxPoint().GetX(), box.GetMaxPoint().GetY(), box.GetMaxPoint().GetZ()};
    query.actorMaterialBits = actor.GetMaterialList().GetValue();
  }

  // The admitted box platforms as the airborne failsafe takes box actors (its Collide and
  // Blocked): CheckCollision 0xc1ec60's visit test (the platform's filter passes us) and
  // `f` on the platform's material, both on the raw lists, then the pair with the box at
  // `delta`. `each(index, selfBox, platformBox, platform)` runs for every platform that
  // passes, in near-list order.
  template < class Each >
  void FsBoxes(const CMaterialFilter& f, const CVector3f& delta, Each each) {
    const CAABox self = BoxAt(delta);
    for (std::size_t p = 0; p < platforms.size(); ++p) {
      const CScriptPlatform& platform = *platforms[p];
      if (platform.HasComplexCollision()) continue;
      if (!platform.GetMaterialFilter().Passes(actor.GetMaterialList())) continue;
      if (!f.Passes(platform.GetMaterialList())) continue;
      each(p, self,
           platform.GetCollisionPrimitive()->CalculateAABox(platform.GetPrimitiveTransform()),
           platform);
    }
  }

  // CheckGlobalCollision_Boolean_NoPostFilter 0xc1f240: the inclusive SAT, terrain and
  // tree platforms through the producer, box platforms by their inclusive overlap.
  bool FsBlocked(const CMaterialFilter& f, const CVector3f& pos) {
    if (!StaticAllowed(f) && platforms.empty()) return false;
    PortRemasteredGroundWorld::Query query;
    std::vector< PortRemasteredGroundWorld::Triangle > tris;
    FsCollect(f, pos - actor.GetTranslation(), query, tris, NULL);
    if (!tris.empty() &&
        PortRemasteredGroundWorld::QueryTerrain(query, tris.data(), tris.size(), false)
            .booleanHit) {
      return true;
    }
    bool hit = false;
    FsBoxes(f, pos - actor.GetTranslation(),
            [&hit](std::size_t, const CAABox& self, const CAABox& other, const CScriptPlatform&) {
              hit = hit || PortRemasteredBoxLanding::BoxesOverlap(PortFsBox(self), PortFsBox(other));
            });
    return hit;
  }

  // CheckCollision 0xc1ec60's static half for the core: one {first,last} contact per
  // collided area tree (the producer's grouping, not flattened), depth from the self
  // half and the normal from the triangle half.
  bool FsCollide(const CMaterialFilter& f, const CVector3f& pos,
                 std::vector< PortCollisionFailsafe::Contact >& out) {
    out.clear();
    if (!StaticAllowed(f) && platforms.empty()) return false;
    PortRemasteredGroundWorld::Query query;
    std::vector< PortRemasteredGroundWorld::Triangle > tris;
    std::vector< std::size_t > ends;
    FsCollect(f, pos - actor.GetTranslation(), query, tris, &ends);
    // Nothing to collide: no triangles and no box platform (the common case, per tick).
    bool anyBox = false;
    for (std::size_t p = 0; p < platforms.size() && !anyBox; ++p) {
      anyBox = !platforms[p]->HasComplexCollision();
    }
    if (tris.empty() && !anyBox) return false;
    const PortRemasteredGroundWorld::TerrainResult res =
        PortRemasteredGroundWorld::QueryTerrain(query, tris.data(), tris.size(), true);
    std::vector< PortCollisionFailsafe::Contact > groups(ends.size());
    // Box platforms: AABox_AABox 0xbf61b4 into the platform's own group (FsCollect closes
    // one per platform after the area trees), as the airborne failsafe's box actors.
    const std::size_t areaGroups = ends.size() - platforms.size();
    FsBoxes(f, pos - actor.GetTranslation(),
            [&](std::size_t p, const CAABox& self, const CAABox& other,
                const CScriptPlatform& platform) {
              std::vector< PortCollisionFailsafe::Impact >& impacts =
                  groups[areaGroups + p].impacts;
              if (!PortCollisionFailsafe::BoxBoxImpacts(PortFsBox(self), PortFsBox(other),
                                                        impacts)) {
                return;
              }
              const CMaterialList& mat = platform.GetMaterialList();
              const bool character =
                  mat.HasMaterial(kMT_Character) || mat.HasMaterial(kMT_CollisionActor);
              const CVector3f center = other.GetCenterPoint();
              for (std::size_t i = 0; i < impacts.size(); ++i) {
                impacts[i].otherCharacter = character;
                impacts[i].otherCenter = FsV(center);
              }
            });
    for (std::size_t i = 0; i + 1 < res.impacts.contacts.size(); ++i) {
      const PortRemasteredGroundWorld::Contact& self = res.impacts.contacts[i];
      const PortRemasteredGroundWorld::Contact& tri = res.impacts.contacts[i + 1];
      if (!self.self || tri.self || tri.triangle != self.triangle) continue;
      std::size_t g = 0;
      while (g < ends.size() && self.triangle >= ends[g]) ++g;
      if (g == ends.size()) continue;
      const CMaterialList mat(tri.materialBits);
      PortCollisionFailsafe::Impact impact;
      impact.depth = self.D;
      impact.otherNormal = {tri.normalX, tri.normalY, tri.normalZ};
      impact.otherCharacter =
          mat.HasMaterial(kMT_Character) || mat.HasMaterial(kMT_CollisionActor); // filter 0x41
      groups[g].impacts.push_back(impact);
      ++i;
    }
    for (std::size_t g = 0; g < groups.size(); ++g) {
      if (!groups[g].impacts.empty()) out.push_back(groups[g]);
    }
    return !out.empty();
  }

  // RayIntersection 0xc20810, terrain only, front faces only (0xbf9360).
  PortCollisionFailsafe::RayHit FsRay(const PortCollisionFailsafe::Vec3& start,
                                      const PortCollisionFailsafe::Vec3& dir, float length) {
    PortCollisionFailsafe::RayHit best;
    if (!StaticAllowed(filter)) return best;
    const CRayCastResult r =
        CGameCollision::RayStaticIntersection(mgr, FsC(start), FsC(dir), length, filter, true);
    if (r.IsValid() && r.GetTime() < length) {
      best.code = PortCollisionFailsafe::kRay_Hit;
      best.t = r.GetTime();
      best.normal = FsV(r.GetPlane().GetNormal());
      best.orientation = PortRemasteredGroundWorld::Orientation(r.GetMaterial().GetValue());
    }
    return best;
  }

  // 0xc20214 DoTunnelingFailsafe for the actor's current bounds.
  bool FsTunnel(const CVector3f& from, const CVector3f& to, CVector3f& out) {
    const CAABox bounds = BoxAt(CVector3f::Zero());
    const PortCollisionFailsafe::Box b = {FsV(bounds.GetMinPoint()), FsV(bounds.GetMaxPoint())};
    if (!PortCollisionFailsafe::TunnelingFailsafeApplies(b, FsV(from), FsV(to))) {
      out = to;
      return false;
    }
    const PortCollisionFailsafe::Vec3 pos = FsV(actor.GetTranslation());
    PortCollisionFailsafe::TunnelHooks hooks;
    hooks.ray = [this](const PortCollisionFailsafe::Vec3& s, const PortCollisionFailsafe::Vec3& d,
                       float len) { return FsRay(s, d, len); };
    hooks.safeOffset = [this, b, pos](const PortCollisionFailsafe::Vec3& at, float maxDist,
                                      PortCollisionFailsafe::Vec3& offset) {
      PortCollisionFailsafe::Box box = b;
      const PortCollisionFailsafe::Vec3 shift = {at.x - pos.x, at.y - pos.y, at.z - pos.z};
      box.min = {box.min.x + shift.x, box.min.y + shift.y, box.min.z + shift.z};
      box.max = {box.max.x + shift.x, box.max.y + shift.y, box.max.z + shift.z};
      PortCollisionFailsafe::SafeHooks safe;
      safe.collidesAt = [this](const PortCollisionFailsafe::Vec3& p) {
        return FsBlocked(filter, FsC(p));
      };
      safe.rayBlocked = [this](const PortCollisionFailsafe::Vec3& st,
                               const PortCollisionFailsafe::Vec3& d, float len) {
        const PortCollisionFailsafe::RayHit h = FsRay(st, d, len);
        return h.code != PortCollisionFailsafe::kRay_Miss && h.t < len;
      };
      return PortCollisionFailsafe::CalculateOffsetToSafeCollisionPoint(box, at, maxDist, safe,
                                                                        offset);
    };
    PortCollisionFailsafe::Vec3 result;
    const bool moved = PortCollisionFailsafe::DoTunnelingFailsafe(b, pos, FsV(from), FsV(to), hooks,
                                                                  result);
    out = FsC(result);
    return moved;
  }

  // 0xc1fb88 DoCollisionFailsafe(world, obj, from, out): the core with the collision
  // filter, then (when it moved and the result still collides) again with the failsafe
  // filter. `from` is the last prediction target, `state` the object's own transform
  // (the tunneling test's origin, 0xc1fc54).
  bool RunFailsafe(float dt, const CVector3f& from, const CVector3f& state, CVector3f& result) {
    const CMaterialFilter failsafeFilter = CMaterialFilter::MakeIncludeExclude(
        CMaterialList(kMT_Solid), CMaterialList(kMT_Character, kMT_CollisionActor));
    auto filterFor = [&](bool failsafe) -> const CMaterialFilter& {
      return failsafe ? failsafeFilter : filter;
    };
    auto core = [&](bool failsafe, PortCollisionFailsafe::Vec3& out) {
      PortCollisionFailsafe::CoreHooks hooks;
      hooks.collide = [&](const PortCollisionFailsafe::Vec3& p,
                          std::vector< PortCollisionFailsafe::Contact >& list) {
        return FsCollide(filterFor(failsafe), FsC(p), list);
      };
      hooks.tunnel = [&](const PortCollisionFailsafe::Vec3& f, const PortCollisionFailsafe::Vec3& t,
                         PortCollisionFailsafe::Vec3& o) {
        CVector3f r;
        const bool moved = FsTunnel(FsC(f), FsC(t), r);
        o = FsV(r);
        return moved;
      };
      return PortCollisionFailsafe::Core(FsV(from), FsV(state), dt, hooks, out);
    };
    auto blocked = [&](bool failsafe, const PortCollisionFailsafe::Vec3& p) {
      return FsBlocked(filterFor(failsafe), FsC(p));
    };
    PortCollisionFailsafe::Vec3 out;
    const bool resolved =
        PortCollisionFailsafe::DoCollisionFailsafe(FsV(from), true, core, blocked, out);
    result = FsC(out);
    return resolved;
  }

  // 0xf934ec SnapToGround(targetDelta, predictDt): applies the target, snapped down
  // onto the highest floor contact when one is found. Returns the source's outcome:
  // kGrounded (object state 1, 0xf93a50-0xf93ae4), kNoFloor (object state 0 plus the
  // caller's motion state, 0xf93aec-0xf93b00) or kRefused (offset >= GetStepUpHeight
  // jumps to the epilogue 0xf93b04 and applies nothing at all).
  // Probes at the frame's target (a delta from the unmoved actor). Applies nothing: the
  // caller moves the actor per the outcome (ApplySnapOutcome); `snapOffset` is the raise.
  PortRemasteredGround::SnapResult SnapToGround(const CVector3f& targetDelta, float& snapOffset) {
    PortRemasteredGround::SnapResult outcome = PortRemasteredGround::SnapResult::kNoFloor;
    snapOffset = 0.f;

    // GetStepDownHeight from the mover's fields: the travel multipliers are zero, so
    // the reach is the constructor snapshot.
    const float reach = stepDown;
    float distance = reach;
    bool blockedDown = StaticBlocked(targetDelta - Up() * reach);
    if (blockedDown) {
      blockedDown = PortRemasteredGround::SnapProbe(
          reach, [&](float d) { return StaticBlocked(targetDelta - Up() * d); }, distance);
    }

    if (blockedDown) {
      PortRemasteredGroundWorld::Result contacts;
      const bool collided = TerrainContacts(targetDelta - Up() * distance, contacts);
      // The snap's single CheckCollision is appended to the mover's result list after the
      // passes' (0xf934ec L156; 0xf923d8 L241-243).
      PortRemasteredGroundRider::AppendContacts(riderContacts, contacts);
      if (collided) {
        const CAABox targetBox = BoxAt(targetDelta);
        PairView view;
        view.Build(contacts);
        const Snap snap = PortRemasteredGround::SelectSnap(targetBox.GetMinPoint().GetZ(), stepUp,
                                                           view.policy.data(), view.policy.size());
        if (snap.result == PortRemasteredGround::SnapResult::kRefused) {
          return snap.result;
        }
        if (snap.result == PortRemasteredGround::SnapResult::kGrounded) {
          outcome = snap.result;
          snapOffset = snap.offset;
          floorMaterial = view.source[snap.contact]->materialBits;
          floorNormal = CVector3f(view.source[snap.contact]->normalX,
                                  view.source[snap.contact]->normalY,
                                  view.source[snap.contact]->normalZ);
        }
      }
    }

    return outcome;
  }
};

} // namespace

void CGameCollision::MoveGroundedRemastered(CStateManager& mgr, CPlayer& player, float dt,
                                            const TEntityList* colliderList) {
  CPhysicsActor& actor = player;
  const CMaterialFilter& materialFilter = actor.GetMaterialFilter();

  // Mover state at entry (G6): the retained Remastered persisted object state (CPhysicsState
  // +0x13c), which only the movers write. NOT the native material: ENTRY-STATE.md §D shows
  // kMT_GroundCollider is identically true on this path (it is the dispatch condition), so it
  // could never resend Falling for an airborne entry. An invalid value never reaches here (the
  // dispatch takes the retail mover first), and is never written.
  const PortRemasteredGround::RetainedResult retainedAtEntry = player.PortRetainedObjectState();

  // The rider carry (PredictMotion 0xc23e40): the ridden platform's pose delta since its
  // PreThink this tick, while the controller is live (set by an earlier grounded tick).
  const uint simTick = mgr.PortSimTick();
  const PortRemasteredGroundRider::CarryTerm riderCarry = PortRiderCarry(mgr, player, simTick);

  TEntityList nearList;
  const CAABox motionVolume = PortWidenByCarry(
      actor.GetMotionVolume(dt), riderCarry.pending
                                     ? riderCarry.Translation(PortVec(actor.GetTranslation()))
                                     : PortRemasteredGround::Vec3());
  if (colliderList != NULL) {
    nearList = *colliderList;
  } else {
    mgr.BuildColliderList(nearList, actor, motionVolume);
  }

  // Every query of this tick stays inside this volume: the sampled transforms lie in
  // the motion volume, and the up/down probes and the snap ladder only translate
  // vertically by the mover's step heights.
  const float verticalReach =
      rstl::max_val(rstl::max_val(actor.GetStepUpHeight(), actor.GetStepDownHeight()), 1.f) + 0.1f;
  CAABox queryVolume(motionVolume.GetMinPoint() - CVector3f(0.2f, 0.2f, verticalReach),
                     motionVolume.GetMaxPoint() + CVector3f(0.2f, 0.2f, verticalReach));
  CAreaCollisionCache cache(queryVolume);
  BuildAreaCollisionCache(mgr, cache);

  // Dynamic contacts: everything in the near list that overlaps the whole query region.
  // Active DCLN and box platforms are admitted, rotated this tick or not (their triangles
  // or box pairs go into the producer lists, after the terrain's); anything else has no Remastered
  // producer, and the tick goes to the port's existing grounded mover: no retail contact is
  // ever presented as a Remastered one.
  std::vector< const CScriptPlatform* > platforms;
  bool admitted = true;
  {
    const CCollidableAABox volumePrim(queryVolume, actor.GetMaterialList());
    for (const TUniqueId* id = nearList.begin(); id != nearList.end() && admitted; ++id) {
      const CPhysicsActor* other = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id));
      if (other == NULL ||
          !CCollisionPrimitive::CollideBoolean(
              CInternalCollisionStructure::CPrimDesc(volumePrim, CMaterialFilter::skPassEverything,
                                                     CTransform4f::Identity()),
              CInternalCollisionStructure::CPrimDesc(*other->GetCollisionPrimitive(),
                                                     CMaterialFilter::skPassEverything,
                                                     other->GetPrimitiveTransform()))) {
        continue;
      }
      if (const CScriptPlatform* platform = TCastToConstPtr< CScriptPlatform >(other)) {
        if (PortAdmitGroundedPlatform(*platform, simTick)) {
          platforms.push_back(platform);
          continue;
        }
      }
      admitted = false;
    }
  }
  const PortRemasteredGroundRider::CarryPlan carryPlan =
      PortRemasteredGroundRider::PlanCarry(admitted, player.PortRetailRiderCarried(simTick));
  if (carryPlan.applyDeferredRetail) {
    ++gUnsupportedDynamicContactTicks;
    // The retail grounded mover takes the retail rider carry MoveRiders deferred.
    player.PortApplyDeferredRiderCarry(simTick);
    // The retail grounded mover below cannot produce the source's retained state (its
    // contacts are not Remastered mover outcomes), so the value is invalidated: the next
    // ground tick falls back to the retail mover instead of entering this one with a
    // stale entry state, until an airborne tick writes 0 and restores validity.
    player.PortInvalidateRetainedObjectState();
    CGroundMovement::MoveGroundCollider_New(mgr, actor, dt, colliderList);
    return;
  }
  // An admitted tick: the mover's own carry replaces the deferred retail one.
  player.PortDropDeferredRiderCarry();
  player.PortSetLastTickRemasteredCarry(true);
  // FindOverlap 0xc1a760 orders actors by `id & 0xffff`.
  std::stable_sort(platforms.begin(), platforms.end(),
                   [](const CScriptPlatform* a, const CScriptPlatform* b) {
                     return PortRemasteredGroundRider::NearListBefore(
                         PortRemasteredGroundRider::OwnerFromRaw(a->GetUniqueId().value),
                         PortRemasteredGroundRider::OwnerFromRaw(b->GetUniqueId().value));
                   });
  if (cache.HasCacheOverflowed()) {
    // Not a mover switch: the shipped mover has no leaf cache at all, so a full one
    // only costs the port's cached traversal (see StaticBlocked/TerrainContacts).
    ++gRemasteredGroundCacheOverflowTicks;
  }

  // Read-only mover-entry counter: this is the ONLY point where the Remastered ground mover
  // actually runs a tick — every path that would silently disable ground replacement has
  // already returned above (the entry gate in MovePlayer, the dynamic-contact fallback here,
  // a save-state load), so a tick reaching here is exactly one real replacement tick.
  // Incremented once, per player, AFTER that guard and immediately BEFORE the entry impulse
  // (Pre 0xc59814): it counts mover entries, not simulation ticks and not a source quantity,
  // and it is read only by `status --json` (`physics.ground_mover_runs`). Nothing reads it
  // back and it never feeds a decision.
  player.PortNoteGroundMoverRun();

  // The retail grounded mover clears the sliding-on-wall flag at the top of every tick
  // (CGroundMovement.cpp MoveGroundCollider_New) and only its step-down sets it. This mover
  // has no such step-down, so it clears it too: otherwise a value left by a retail tick (the
  // dynamic-contact fallback above, a tick with the retained state invalid) outlives it, and
  // ComputeDash then never sets the orbit velocity, so a lock-on keeps the player sliding
  // at whatever velocity it had (issue #141).
  player.SetPlayerIsSlidingOnWall(false);

  GroundTick tick{mgr,
                  player,
                  actor,
                  cache,
                  materialFilter,
                  nearList,
                  actor.CPhysicsActor::GetStepUpHeight(),
                  actor.CPhysicsActor::GetStepDownHeight()};
  tick.platforms = platforms;
  if (carryPlan.remasteredCarry) {
    tick.carry = riderCarry;
  }

  // 0xf923d8: up to five MoveAlongGround passes, stopping after four that consumed
  // no time, while time remains. The loop re-predicts at the top of every iteration
  // (0xf92544-0xf925a8) and both SnapToGround call sites take that transform and
  // motion state whatever exit the loop makes (0xf92738/0xf9273c and
  // 0xf927cc/0xf927d0), so the last prediction — and the dt it used — survive the
  // loop. Zeroing them here would make the stall exit apply Predict(0) instead of
  // the source's whole-tick prediction.
  tick.IntegrateEntryImpulse();
  float remaining = dt;
  int iterations = 0;
  int zeroAdvances = 0;
  auto V3 = [](const CVector3f& v) { return PortRemasteredGround::Vec3{v.GetX(), v.GetY(), v.GetZ()}; };
  auto C3 = [](const PortRemasteredGround::Vec3& v) { return CVector3f(v.x, v.y, v.z); };
  // CMotionState <-> the policy's snapshot, field for field (GetVelocity is the momentum).
  auto Snap = [&](const CMotionState& m) {
    const CNUQuaternion& q = m.GetOrientation();
    const CVector3f qv = q.GetVector();
    return PortRemasteredGround::MotionSnapshot{
        V3(m.GetTranslation()), {q.GetScalar(), qv.GetX(), qv.GetY(), qv.GetZ()},
        V3(m.GetVelocity()), V3(m.GetAngularMomentum().GetVector())};
  };
  auto Motion = [&](const PortRemasteredGround::MotionSnapshot& m) {
    return CMotionState(C3(m.translation),
                        CNUQuaternion(m.orientation.w, m.orientation.x, m.orientation.y,
                                      m.orientation.z),
                        C3(m.momentum), CAxisAngle(C3(m.angularMomentum)));
  };
  // The loop's two absolute locals (GroundFrame): the snap target and the WHOLE saved
  // loop-top motion state, tracked separately (D1/D3). Saved once per loop top, from the
  // actor's state there; the passes below never write it.
  PortRemasteredGround::GroundFrame frame = PortRemasteredGround::FrameAtLoopTop(
      Snap(actor.GetMotionState()), Snap(tick.Predict(remaining)));
  while (iterations <= 4 && zeroAdvances <= 3 && remaining > kMoveDtEpsilon) {
    const CVector3f actorNow = actor.GetTranslation();
    frame = PortRemasteredGround::FrameAtLoopTop(Snap(actor.GetMotionState()),
                                                 Snap(tick.Predict(remaining)));
    const CVector3f targetDelta = C3(PortRemasteredGround::SubVec(frame.target, V3(actorNow)));
    PortRemasteredGroundWorld::Result contacts;
    if (!tick.TerrainContacts(targetDelta, contacts)) {
      // No collision at the target: it stands (0xf923d8:167, the bVar3 = false exit).
      break;
    }
    const Probe probe = tick.StepProbeAt(contacts, targetDelta);
    if (probe.allowed && !tick.Blocked(targetDelta + tick.Up() * probe.offset)) {
      // CanMoveUpAndDownFromPosition resolved: its adjusted transform is the target (the
      // saved motion state is untouched) and the loop stops without a pass (0xf923d8:176).
      PortRemasteredGround::FrameStepResolved(frame, V3(tick.Up() * probe.offset));
      break;
    }
    const float consumed = tick.MoveAlongGround(remaining);
    PortRemasteredGround::FrameAfterPass(frame, tick.wroteTarget, V3(actor.GetTranslation()));
    remaining -= consumed;
    if (fabsf(consumed) < 1.e-5f) {
      ++zeroAdvances;
    }
    ++iterations;
  }
  if (tick.grewContacts) {
    ++gRemasteredGroundContactGrowthTicks;
  }

  // Every loop exit reaches 0xf926b8 and calls DoCollisionFailsafe 0xc1fb88 before
  // SnapToGround (0xf926e8): from = the snap target, object state = the actor's own
  // translation, the whole-tick dt. Resolved, it writes only the LOCAL snap transform and
  // the saved motion state's translation (0xf926ec-0xf92718); the actor is not moved here.
  // Terrain only; the port's CGameCollision::CollisionFailsafe is a different function.
  {
    const PortRemasteredGround::FailsafeApply fs = PortRemasteredGround::ApplyGroundFailsafe(
        V3(actor.GetTranslation()), frame,
        [&](const PortRemasteredGround::Vec3& from, const PortRemasteredGround::Vec3& state,
            PortRemasteredGround::Vec3& out) {
          CVector3f r;
          const bool resolved = tick.RunFailsafe(dt, C3(from), C3(state), r);
          out = V3(r);
          return resolved;
        });
    if (fs.resolved) {
      PortRemasteredGround::FrameFailsafeResolved(frame, fs.out);
    }
  }

  const CVector3f actorBefore = actor.GetTranslation();
  float snapOffset = 0.f;
  const PortRemasteredGround::SnapResult snap = tick.SnapToGround(
      C3(PortRemasteredGround::SubVec(frame.target, V3(actorBefore))), snapOffset);
  // 0xf93a50-0xf93b04: grounded = the saved loop-top state with the snapped target as
  // translation (0xf93a70-0xf93aa4), no floor = the saved state whole (0xf93afc), refused =
  // nothing (also no failsafe push). Set absolutely (ApplyMotionState 0xc165c0): the
  // passes' responses and orientation steps are replaced, never added on top.
  const PortRemasteredGround::SnapApply applied = PortRemasteredGround::ApplySnapOutcome(
      snap, frame, V3(tick.Up() * snapOffset));
  if (applied.moveActor) {
    actor.SetMotionState(Motion(applied.state));
  }
  // The carried yaw reached the orientation if a pass applied it, or if the snap set the saved
  // state: every loop-top prediction had it until a pass consumed it, and after that the
  // actor's state did. Only a refused snap with no pass leaves the player unturned.
  if (carryPlan.remasteredCarry && riderCarry.pending && riderCarry.rotates &&
      riderCarry.yaw != 0.f && (applied.moveActor || !tick.carry.pending)) {
    player.PortNoteCarriedYaw(riderCarry.yaw);
  }
  // SnapToGround's own state write, which happens before any message (0xf93ae4 grounded 1 /
  // 0xf93af4 no floor 0; the refused epilogue 0xf93b04 writes nothing, so a refused tick
  // carries the entry value into the next tick — ENTRY-STATE.md §C).
  player.PortSetRetainedObjectState(
      PortRemasteredGround::RetainedGroundOutcome(retainedAtEntry, snap));

  // Order (source 0xf92804 / PostSimulation / PostPhysicsUpdate): Post 0xc5994c on the
  // actor's current momentum (every snap outcome), then PostSimulation clears the
  // transient force/impulses, THEN the state messages go out.
  tick.IntegratePostForces(dt);
  actor.ImpulseWR() = CVector3f::Zero();
  actor.ClearForcesAndTorques();

  // 0xf92808-0xf92860: the first non-static contact of the result list (every pass, then
  // the snap) becomes the rider, count 2 (SetRiderController 0xc1631c). Every object here
  // is an admitted platform, so all can be ridden.
  const std::uint32_t riderOwner = PortRemasteredGroundRider::PickRider(
      tick.riderContacts, [](std::uint32_t) { return true; });
  if (riderOwner != PortRemasteredGroundRider::kTerrainOwner) {
    player.PortRider().Set(riderOwner);
  }

  // PostPhysicsUpdate 0xc545bc's messages, from the policy seam (G6/G6b).
  const PortRemasteredGround::StateActions act =
      PortRemasteredGround::DecideStateActions(snap, retainedAtEntry);
  if (act.onFloor) {
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_OnFloor);
    // kSM_OnNormalSurface/OnIceSurface/OnMudSlowSurface set the surface restraint
    // (CPlayer.cpp:1766-1774), which the force law and the air drag read.
    SendMaterialMessage(mgr, CMaterialList(tick.floorMaterial), actor);
    actor.SetLastFloorPlaneNormal(tick.floorNormal);
  } else if (act.resendOnFloor) {
    // Refused snap: the source wrote no state, so the previous one is sent again and
    // the actor is not moved.
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_OnFloor);
  }
  if (act.falling) {
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_Falling);
    // 0xc54ad4: the off-ground mover takes over next tick.
    if (act.removeGroundCollider) {
      actor.RemoveMaterial(kMT_GroundCollider, mgr);
    }
    if (act.clearFloorNormal) {
      actor.SetLastFloorPlaneNormal(rstl::optional_object_null());
    }
  }

  // Port glue, not source: the retail rider list (kSM_AddPlatformRider, both ways) for a
  // platform the player stands on, so the platform's retail state (x82e_ridingPlatform,
  // x318_riders: the camera, the retail fallback's carry) matches the retail mover's.
  if (riderOwner != PortRemasteredGroundRider::kTerrainOwner) {
    const PortRemasteredGroundWorld::Result& list = tick.riderContacts;
    bool floor = false;
    for (const PortRemasteredGroundWorld::Contact& c : list.contacts) {
      if (!c.self && c.ownerUid == riderOwner &&
          PortRemasteredGround::IsFloor(c.orientation, c.normalZ)) {
        floor = true;
        break;
      }
    }
    const ushort raw = PortRemasteredGroundRider::RawFromOwner(riderOwner);
    CEntity* const platform = mgr.ObjectById(TUniqueId(raw >> kUniqueIdIndexBits, raw & kUniqueIdIndexMask));
    if (floor && platform != NULL) {
      mgr.DeliverScriptMsg(platform, actor.GetUniqueId(), kSM_AddPlatformRider);
      mgr.DeliverScriptMsg(&actor, platform->GetUniqueId(), kSM_AddPlatformRider);
    }
  }
}
#endif
