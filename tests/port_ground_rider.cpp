#include "port_remastered_ground_rider.h"
#include "port_remastered_ground_box.h"

// These checks must also run when the enclosing build defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <initializer_list>
#include <vector>

using namespace PortRemasteredGroundRider;
namespace W = PortRemasteredGroundWorld;

// A synthetic tree node: a leaf carries an id, an inner node two children.
struct Node {
  int id = -1;
  const Node* l = nullptr;
  const Node* r = nullptr;
  bool overlaps = true;
};

static std::vector< int > Walk(const Node& root) {
  std::vector< int > visited;
  WalkTreeLifo(
      root, [](const Node& n) { return n.l == nullptr; },
      [](const Node& n) -> const Node& { return *n.l; },
      [](const Node& n) -> const Node& { return *n.r; },
      [](const Node& n) { return n.overlaps; }, [&](const Node& n) { visited.push_back(n.id); });
  return visited;
}

static W::Triangle Flat(float z, std::uint32_t owner) {
  W::Triangle tri;
  tri.verts = {{{-.5f, -.5f, z}, {.5f, -.5f, z}, {0.f, .5f, z}}};
  tri.normal = {0.f, 0.f, 1.f};
  tri.ownerUid = owner;
  return tri;
}

static W::Contact Owned(std::uint32_t owner, bool self = false) {
  W::Contact c;
  c.ownerUid = owner;
  c.self = self;
  return c;
}

int main() {
  // 1. Tree walk: LIFO, the root is never tested, children pushed left then right, so the
  // right subtree is visited first.
  {
    Node a{0}, b{1}, c{2}, d{3};
    Node left{-1, &a, &b}, right{-1, &c, &d};
    Node root{-1, &left, &right};
    root.overlaps = false; // never tested
    assert((Walk(root) == std::vector< int >{3, 2, 1, 0}));
    right.overlaps = false;
    assert((Walk(root) == std::vector< int >{1, 0}));
    b.overlaps = false;
    assert((Walk(root) == std::vector< int >{0}));
    // A leaf root is visited without a test.
    Node lone{7};
    lone.overlaps = false;
    assert((Walk(lone) == std::vector< int >{7}));
  }

  // 2. Material merge and owner.
  {
    const W::Triangle tri =
        PlatformTriangle({{{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}}}, {0.f, 0.f, 1.f},
                         0x0000000000100001ull, 0x0000000200000000ull, OwnerFromRaw(0x0405));
    assert(tri.materialBits == 0x0000000200100001ull);
    assert(tri.ownerUid == 0x10405u);
    assert(RawFromOwner(tri.ownerUid) == 0x0405);
    assert(OwnerFromRaw(0) != kTerrainOwner);
  }

  // 3. Contact order: terrain first, then the platforms by `id & 0xffff` (the port's index).
  {
    W::Query q;
    q.box = {{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const std::uint32_t p1 = OwnerFromRaw(std::uint16_t((5u << 10) | 9u));
    const std::uint32_t p2 = OwnerFromRaw(std::uint16_t((1u << 10) | 3u));
    assert(NearListBefore(p2, p1) && !NearListBefore(p1, p2));
    std::vector< std::uint32_t > platforms{p1, p2};
    std::stable_sort(platforms.begin(), platforms.end(), NearListBefore);
    assert(platforms[0] == p2 && platforms[1] == p1);
    std::vector< W::Triangle > tris{Flat(.9f, kTerrainOwner)};
    for (std::uint32_t p : platforms) {
      tris.push_back(Flat(.8f, p));
    }
    const W::TerrainResult r = W::QueryTerrain(q, tris.data(), tris.size(), true);
    std::vector< std::uint32_t > owners;
    for (const W::Contact& c : r.impacts.contacts) {
      if (!c.self) {
        owners.push_back(c.ownerUid);
      }
    }
    assert((owners == std::vector< std::uint32_t >{kTerrainOwner, p2, p1}));
  }

  // 4. Carry: whole (not scaled by t) until the first pass consumes it.
  {
    CarryTerm term{{0.f, 0.f, .02f}, true};
    const Vec3 p = term.Apply({1.f, 2.f, 3.f});
    assert(p.x == 1.f && p.y == 2.f && p.z == 3.f + .02f);
    const Vec3 half = term.Apply({.5f, 1.f, 1.5f});
    assert(half.z == 1.5f + .02f);
    term.ConsumeAfterPass();
    const Vec3 after = term.Apply({1.f, 2.f, 3.f});
    assert(after.x == 1.f && after.y == 2.f && after.z == 3.f);
    CarryTerm none;
    assert(none.Apply({1.f, 2.f, 3.f}).z == 3.f);
  }

  // 5. Rider choice over the one result list: the first non-self owned, rideable contact; no
  // normal test.
  {
    W::Result list;
    list.contacts = {Owned(kTerrainOwner), Owned(0x10011, true), Owned(0x10022), Owned(0x10033)};
    list.contacts[2].normalZ = -1.f; // a ceiling still counts
    auto all = [](std::uint32_t) { return true; };
    assert(PickRider(list, all) == 0x10022u);
    assert(PickRider(list, [](std::uint32_t o) { return o != 0x10022u; }) == 0x10033u);
    W::Result terrainOnly;
    terrainOnly.contacts = {Owned(kTerrainOwner)};
    assert(PickRider(terrainOnly, all) == kTerrainOwner);
    assert(PickRider(W::Result(), all) == kTerrainOwner);
  }

  // 5b. The list is appended, never replaced (copy_collision_info_list_into 0xc18c00): every
  // pass in order, then the snap, as 0xf923d8 builds it.
  {
    auto all = [](std::uint32_t) { return true; };
    auto build = [](std::initializer_list< W::Result > parts) {
      W::Result list;
      for (const W::Result& part : parts) {
        AppendContacts(list, part);
      }
      return list;
    };
    W::Result terrain, platA, platB;
    terrain.contacts = {Owned(kTerrainOwner)};
    platA.contacts = {Owned(kTerrainOwner), Owned(0x100AA)};
    platB.contacts = {Owned(0x100BB)};
    // The snap hit only terrain, but a pass touched platform A: A is the rider.
    assert(PickRider(build({platA, terrain}), all) == 0x100AAu);
    // Pass 1 terrain, pass 2 platform A, snap platform B: the first, A, wins.
    assert(PickRider(build({terrain, platA, platB}), all) == 0x100AAu);
    // Platforms across passes: the earlier pass's wins.
    assert(PickRider(build({platB, platA}), all) == 0x100BBu);
    // Standing on terrain and brushing a platform's wall in a pass: no normal test, it rides.
    W::Result wall;
    wall.contacts = {Owned(kTerrainOwner), Owned(0x100CC)};
    wall.contacts[1].normalZ = 0.f;
    wall.contacts[1].normalX = 1.f;
    assert(PickRider(build({wall, terrain}), all) == 0x100CCu);
    // Appending keeps each part's order and count.
    const W::Result joined = build({terrain, platA, platB});
    assert(joined.contacts.size() == 4 && joined.contacts[2].ownerUid == 0x100AAu &&
           joined.contacts[3].ownerUid == 0x100BBu);
  }

  // 6. Decay 2 -> 1 -> 0 across contactless ticks; re-contact resets to 2.
  {
    RiderController rider;
    assert(!rider.Active());
    rider.Set(0x10022);
    assert(rider.count == 2 && rider.Active());
    rider.Decay();
    assert(rider.count == 1 && rider.Active());
    rider.Set(0x10022);
    assert(rider.count == 2);
    rider.Decay();
    rider.Decay();
    assert(rider.count == 0 && !rider.Active() && rider.owner == kTerrainOwner);
    rider.Decay();
    assert(rider.count == 0);
    // At 300 Hz it lasts the same 1/30 s: the setting tick and nine more.
    rider.Set(0x10022);
    for (int i = 0; i < 9; ++i) {
      rider.Decay(60.f / 300.f);
      assert(rider.Active());
    }
    rider.Decay(60.f / 300.f);
    assert(!rider.Active() && rider.count == 0 && rider.owner == kTerrainOwner);
  }

  // 7. Gate: only an active platform with a tree or a box is admitted, and unrotated when
  // airborne.
  {
    assert(Admit({true, true, true, false}));
    assert(!Admit({false, true, true, false})); // not a platform (an actor, a door)
    assert(!Admit({true, true, false, false})); // neither a tree nor a box
    assert(!Admit({true, true, true, true}));   // rotated this tick
    assert(!Admit({true, false, true, false})); // inactive
    assert(Admit({true, true, false, false, true}));   // AABox platform
    assert(!Admit({true, true, false, true, true}));   // AABox, rotated (airborne refuses)
    // The grounded mover admits a rotated platform (slice 3: the rotation carry is ported).
    assert(AdmitGrounded({true, true, false, true, true}));
    assert(AdmitGrounded({true, true, true, true}));
    assert(!AdmitGrounded({true, false, false, true, true}));
    assert(!AdmitGrounded({false, true, false, true, true}));
    assert(!AdmitGrounded({true, true, false, true, false}));
    assert(!Admit({true, false, false, false, true})); // AABox, inactive
    assert(!Admit({false, true, false, false, true})); // a box actor that is not a platform
  }

  // 8. A stale pose stamp (or no live rider) carries nothing.
  {
    const Vec3 c = PlatformCarry({0.f, 0.f, 1.f}, {0.f, 0.f, 1.25f}, 7, 7, true);
    assert(c.x == 0.f && c.y == 0.f && c.z == .25f);
    const Vec3 stale = PlatformCarry({0.f, 0.f, 1.f}, {0.f, 0.f, 1.25f}, 6, 7, true);
    assert(stale.x == 0.f && stale.y == 0.f && stale.z == 0.f);
    const Vec3 dead = PlatformCarry({0.f, 0.f, 1.f}, {0.f, 0.f, 1.25f}, 7, 7, false);
    assert(dead.z == 0.f);
  }

  // The stale carry after a deactivation: the last active step, start to end of its PreThink,
  // exactly once, never the pose now.
  {
    const Vec3 start = {0.f, 0.f, 1.f};
    const Vec3 end = {0.f, 0.f, 1.25f};
    const Vec3 warped = {5.f, 0.f, 9.f};
    const Vec3 live = RiderCarry(start, end, end, 7, 7, true, true);
    assert(live.z == .25f);
    const Vec3 stale = RiderCarry(start, warped, end, 7, 8, false, true);
    assert(stale.x == 0.f && stale.z == .25f);
    const Vec3 older = RiderCarry(start, warped, end, 7, 9, false, true);
    assert(older.x == 0.f && older.z == 0.f);
    const Vec3 notThought = RiderCarry(start, end, end, 7, 8, true, true);
    assert(notThought.z == 0.f);
    const Vec3 noRider = RiderCarry(start, warped, end, 7, 8, false, false);
    assert(noRider.z == 0.f);
    // Deactivated after its PreThink this tick: it did move this tick.
    const Vec3 sameTick = RiderCarry(start, end, end, 8, 8, false, true);
    assert(sameTick.z == .25f);
    // The rider set on the last active tick T is live at T+1 only: one stale carry.
    RiderController rider;
    rider.Set(OwnerFromRaw(0x0412));
    rider.Decay();
    int carries = 0;
    for (std::uint32_t tick = 8; tick < 12; ++tick) {
      if (RiderCarry(start, warped, end, 7, tick, false, rider.Active()).z != 0.f) {
        ++carries;
      }
      rider.Decay();
    }
    assert(carries == 1);
  }

  // Exactly one carry per tick, whichever mover runs. This models PlanCarry and
  // DeferRetailCarry over the tick order (MoveRiders in PreThink defers or moves, then the
  // tick's mover applies or drops); it re-implements the call sites' logic rather than
  // exercising them, so a glue site in MovePlayer, MoveGroundedRemastered or
  // MoveAirborneStepped that diverges from this model is not caught here (the rig traces
  // cover that). Both Remastered movers (admitted grounded, airborne) own the carry.
  {
    enum Mover { kAdmitted, kFallback, kAirborne, kMorph, kGateOff, kNoMove };
    const Mover script[] = {kGateOff,  kAdmitted, kAdmitted, kFallback, kAdmitted, kAirborne,
                            kAdmitted, kMorph,    kAdmitted, kNoMove,   kAdmitted, kAdmitted,
                            kFallback, kFallback, kAdmitted, kGateOff,  kAdmitted, kAirborne,
                            kAirborne, kFallback, kAirborne, kAdmitted, kAirborne, kMorph,
                            kAirborne, kNoMove,   kAirborne, kGateOff,  kAirborne};
    bool lastTickRemasteredCarry = false;
    for (Mover mover : script) {
      int carries = 0;
      bool deferred = false;
      bool retailCarried = false;
      // MoveRiders: the player is a retail rider every tick here.
      if (DeferRetailCarry(lastTickRemasteredCarry)) {
        deferred = true;
      } else {
        ++carries;
        retailCarried = true;
      }
      lastTickRemasteredCarry = false;
      if (mover == kAdmitted || mover == kAirborne) {
        const CarryPlan plan = PlanCarry(true, retailCarried);
        assert(!plan.applyDeferredRetail);
        deferred = false; // dropped
        if (plan.remasteredCarry) {
          ++carries;
        }
        lastTickRemasteredCarry = true;
      } else if (mover == kFallback) {
        const CarryPlan plan = PlanCarry(false, retailCarried);
        assert(plan.applyDeferredRetail && !plan.remasteredCarry);
      }
      // Every other mover applies the deferred carry; a tick whose mover did not run gets it
      // at PortEndMovementTick.
      if (deferred) {
        ++carries;
      }
      assert(carries == 1);
    }
  }

  // The airborne rider writer (0xf948d4 line 493, 0xf94720): only a landing step writes,
  // and it writes that step's candidate even when it is invalid.
  {
    AirborneRider none;
    assert(none.Controller() == kTerrainOwner);
    AirborneRider landed;
    landed.Landed(OwnerFromRaw(0x0412));
    assert(landed.Controller() == OwnerFromRaw(0x0412));
    AirborneRider terrain;
    terrain.Landed(kTerrainOwner);
    assert(terrain.Controller() == kTerrainOwner);
  }

  // Jumping off a ridden platform: the landing tick sets the rider (count 2), the
  // PostSimulation decay leaves it live for exactly one more tick, so the first airborne tick
  // is carried once and the second is not.
  {
    RiderController rider;
    rider.Set(OwnerFromRaw(0x0412)); // grounded tick T
    rider.Decay();                   // end of T
    assert(rider.Active());          // T+1: the jump tick, carried
    rider.Decay();
    assert(!rider.Active());         // T+2: airborne, not carried
  }

  // The airborne first step's carry: added unscaled to every prediction of that step, t = 0
  // included, and gone once the step is done.
  {
    CarryTerm carry;
    carry.carry = {0.f, 0.f, .5f};
    carry.pending = true;
    const Vec3 lo = carry.Apply({0.f, 0.f, 0.f});
    const Vec3 hi = carry.Apply({1.f, 0.f, -.25f});
    assert(lo.z == .5f && hi.x == 1.f && hi.z == .25f);
    carry.ConsumeAfterPass();
    const Vec3 retry = carry.Apply({1.f, 0.f, -.25f});
    assert(retry.z == -.25f);
  }
  // 9. Box platforms: the AABox_AABox pair as self/other contact pairs (issue #117, AABX).
  {
    namespace G = PortRemasteredGroundBox;
    namespace M = PortRemasteredCollisionMaterial;
    const std::uint64_t player =
        M::Bit(M::List::Solid) | M::Bit(M::List::Player) | M::Bit(M::List::Character);
    const std::uint64_t platform = M::Bit(M::List::Solid) | M::Bit(M::List::Platform);
    const auto pairOf = [&](G::Box self, G::Box other) {
      G::Pair p;
      p.selfBox = self;
      p.otherBox = other;
      p.selfMaterialBits = player;
      p.otherMaterialBits = platform;
      return p; // both filters kFT_Always
    };
    const std::uint32_t owner = OwnerFromRaw(0x0412);
    std::vector< W::Contact > out;
    std::vector< PortCollisionFailsafe::PairedImpact > scratch;

    // Standing 1 deep in the platform's top: one impact, self half first.
    const G::Pair top = pairOf({{0, 0, 0}, {1, 1, 2}}, {{.2f, .2f, -.5f}, {.8f, .8f, 1}});
    assert(G::Blocked(top));
    assert(G::AppendContacts(top, owner, G::kBoxKeyBase + 8, out, scratch) == 1);
    assert(out.size() == 2);
    assert(out[0].self && out[0].ownerUid == kTerrainOwner && out[0].D == 1.f && out[0].H == 1.f);
    assert(!out[1].self && out[1].ownerUid == owner && out[1].D == 1.f);
    assert(out[0].triangle == G::kBoxKeyBase + 8 && out[1].triangle == out[0].triangle);
    assert(G::IsBoxKey(out[1].triangle) && !G::IsBoxKey(G::kBoxKeyBase - 1));
    assert(out[1].normalZ == 1.f && out[1].pointCount == 1 && out[1].points[0].z == 1.f);
    assert(out[1].minVertical == 1.f && out[1].maxVertical == 1.f && !out[1].hasSecondary);
    assert(out[0].normalZ == -1.f);
    // The rider scan takes the platform half.
    W::Result standing;
    standing.contacts = out;
    assert(PickRider(standing, [](std::uint32_t) { return true; }) == owner);

    // Walking into its side: an other half facing the player along -x, with depth.
    out.clear();
    const G::Pair side = pairOf({{0, 0, 0}, {1, 1, 2}}, {{.9f, .2f, .5f}, {3, .8f, 1.5f}});
    assert(G::AppendContacts(side, owner, G::kBoxKeyBase, out, scratch) >= 1);
    bool wall = false;
    for (const W::Contact& c : out) {
      if (!c.self && c.normalX == -1.f && c.normalZ == 0.f && c.D > 0.f) wall = true;
    }
    assert(wall);

    // Touching boxes: the boolean blocks, the pair produces nothing.
    out.clear();
    const G::Pair touch = pairOf({{0, 0, 0}, {1, 1, 1}}, {{0, 0, 1}, {1, 1, 2}});
    assert(G::Blocked(touch));
    assert(G::AppendContacts(touch, owner, G::kBoxKeyBase, out, scratch) == 0 && out.empty());

    // Apart, or the platform's filter excludes the player: neither.
    const G::Pair apart = pairOf({{0, 0, 0}, {1, 1, 1}}, {{0, 0, 1.5f}, {1, 1, 2}});
    assert(!G::Blocked(apart));
    G::Pair excluded = top;
    excluded.otherFilter = M::MaterialFilter{~std::uint64_t{0}, M::Bit(M::List::Player), 2};
    assert(!G::Blocked(excluded));
    assert(G::AppendContacts(excluded, owner, G::kBoxKeyBase, out, scratch) == 0 && out.empty());

    // Near-list order: a slot goes before the first triangle contact at or past its `at`.
    W::Result tris;
    for (std::size_t i = 0; i < 3; ++i) {
      W::Contact self = Owned(kTerrainOwner, true), tri = Owned(kTerrainOwner);
      self.triangle = tri.triangle = i;
      tris.contacts.push_back(self);
      tris.contacts.push_back(tri);
    }
    std::vector< G::Slot > slots(3);
    slots[0].at = 0;
    slots[1].at = 1;
    slots[2].at = 3;
    for (std::size_t s = 0; s < slots.size(); ++s) {
      W::Contact a = Owned(kTerrainOwner, true), b = Owned(OwnerFromRaw(std::uint16_t(s)));
      a.triangle = b.triangle = G::kBoxKeyBase + s * G::kBoxKeyStride;
      slots[s].contacts = {a, b};
    }
    G::Merge(tris, slots);
    const std::size_t want[] = {G::kBoxKeyBase, G::kBoxKeyBase, 0, 0,
                                G::kBoxKeyBase + 8, G::kBoxKeyBase + 8, 1, 1, 2, 2,
                                G::kBoxKeyBase + 16, G::kBoxKeyBase + 16};
    assert(tris.contacts.size() == 12);
    for (std::size_t i = 0; i < 12; ++i) assert(tris.contacts[i].triangle == want[i]);
    assert(!G::AnyBlocked(slots) && G::AnyContacts(slots));
    slots[1].blocked = true;
    assert(G::AnyBlocked(slots));
    // No slots: untouched.
    W::Result same = standing;
    G::Merge(same, {});
    assert(same.contacts.size() == standing.contacts.size());
  }
  // 10. Rotation carry (PredictMotion 0xc23e40; RESEARCH.md §1 vectors A-G). Platform at
  // (10,0,0); the arm is the rider's position before v*dt.
  {
    const float kPi = 3.14159265358979f;
    auto Yaw = [](float deg) {
      const float a = deg * 3.14159265358979f / 180.f;
      Basis b;
      b.c[0] = {std::cos(a), std::sin(a), 0.f};
      b.c[1] = {-std::sin(a), std::cos(a), 0.f};
      return b;
    };
    auto RollX = [](float deg) {
      const float a = deg * 3.14159265358979f / 180.f;
      Basis b;
      b.c[1] = {0.f, std::cos(a), std::sin(a)};
      b.c[2] = {0.f, -std::sin(a), std::cos(a)};
      return b;
    };
    auto Near = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
    auto Pos = [&](const Pose& start, const Pose& now, const Vec3& self, const Vec3& pred) {
      const CarryTerm t = RiderCarryTerm(start, now, now, 7, 7, true, true);
      const Vec3 p = t.Apply(pred, self);
      return Vec3{self.x + p.x, self.y + p.y, self.z + p.z};
    };
    const Vec3 zero{};
    // A: pure yaw +90 degrees.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, Yaw(90.f)};
      const Vec3 p = Pos(s, n, {12.f, 0.f, 1.f}, zero);
      assert(Near(p.x, 10.f, 1e-5f) && Near(p.y, 2.f, 1e-5f) && Near(p.z, 1.f, 1e-6f));
      const CarryTerm t = RiderCarryTerm(s, n, n, 7, 7, true, true);
      assert(t.pending && t.rotates && Near(t.yaw, kPi / 2.f, 1e-5f));
    }
    // B: +2 degrees.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, Yaw(2.f)};
      const Vec3 p = Pos(s, n, {13.f, 0.f, 1.f}, zero);
      assert(Near(p.x, 12.9982f, 1e-4f) && Near(p.y, 0.1047f, 1e-4f) && p.z == 1.f);
      assert(Near(RiderCarryTerm(s, n, n, 7, 7, true, true).yaw, 2.f * kPi / 180.f, 1e-6f));
    }
    // C: translation + yaw, the previous yaw 30 -> 32 degrees: the relative rotation is 2.
    {
      const Pose s{{10.f, 0.f, 0.f}, Yaw(30.f)}, n{{10.1f, 0.f, 0.f}, Yaw(32.f)};
      const Vec3 p = Pos(s, n, {13.f, 0.f, 1.f}, zero);
      assert(Near(p.x, 13.0982f, 1e-4f) && Near(p.y, 0.1047f, 1e-4f) && Near(p.z, 1.f, 1e-6f));
      assert(Near(RiderCarryTerm(s, n, n, 7, 7, true, true).yaw, 2.f * kPi / 180.f, 1e-5f));
    }
    // D: tiny yaw (+0.05 degrees, the source's matrix path).
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, Yaw(.05f)};
      const Vec3 p = Pos(s, n, {13.f, 0.f, 1.f}, zero);
      assert(Near(p.x, 13.f, 1e-5f) && Near(p.y, .0026f, 1e-4f) && Near(p.z, 1.f, 1e-6f));
    }
    // E: a 10 degree tilt about X moves the rider vertically; no yaw.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, RollX(10.f)};
      const Vec3 p = Pos(s, n, {13.f, 0.f, 1.f}, zero);
      assert(Near(p.x, 13.f, 1e-6f) && Near(p.y, -.1736f, 1e-4f) && Near(p.z, .9848f, 1e-4f));
      assert(RiderCarryTerm(s, n, n, 7, 7, true, true).yaw == 0.f);
    }
    // F: v = (6,0,0): the prediction v*dt adds on top, the arm stays the position before it.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, Yaw(2.f)};
      const Vec3 p = Pos(s, n, {13.f, 0.f, 1.f}, {6.f / 60.f, 0.f, 0.f});
      assert(Near(p.x, 13.0982f, 1e-4f) && Near(p.y, 0.1047f, 1e-4f));
    }
    // G: no live rider, or consumed after the first pass: the prediction alone.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, Yaw(2.f)};
      assert(!RiderCarryTerm(s, n, n, 7, 7, true, false).pending);
      CarryTerm t = RiderCarryTerm(s, n, n, 7, 7, true, true);
      t.ConsumeAfterPass();
      const Vec3 p = t.Apply({.1f, 0.f, 0.f}, {13.f, 0.f, 1.f});
      assert(p.x == .1f && p.y == 0.f && p.z == 0.f);
    }
    // Composition: two ticks of +1 degree carry as one of +2 to first order in position, and
    // exactly in yaw.
    {
      Vec3 self{13.f, 0.f, 1.f};
      Basis prev = Basis();
      float yaw = 0.f;
      for (int i = 1; i <= 2; ++i) {
        const Basis cur = Yaw(float(i));
        const CarryTerm t =
            RiderCarryTerm({{10.f, 0.f, 0.f}, prev}, {{10.f, 0.f, 0.f}, cur},
                           {{10.f, 0.f, 0.f}, cur}, 7, 7, true, true);
        const Vec3 d = t.Translation(self);
        self = {self.x + d.x, self.y + d.y, self.z + d.z};
        yaw += t.yaw;
        prev = cur;
      }
      assert(Near(self.x, 12.9982f, 1e-4f) && Near(self.y, 0.1047f, 1e-4f));
      assert(Near(yaw, 2.f * kPi / 180.f, 1e-5f));
    }
    // An unchanged basis: no rotation term, exactly the translation (the 0xc24208 skip).
    {
      const Pose s{{10.f, 0.f, 0.f}, Yaw(30.f)}, n{{10.f, 0.f, .25f}, Yaw(30.f)};
      const CarryTerm t = RiderCarryTerm(s, n, n, 7, 7, true, true);
      assert(t.pending && !t.rotates && t.yaw == 0.f);
      const Vec3 c = t.Translation({13.f, 0.f, 1.f});
      assert(c.x == 0.f && c.y == 0.f && c.z == .25f);
      // Identity relative rotation gives yaw 0.
      assert(CarryYaw(Basis()) == 0.f);
      // Unmoved and unturned: nothing pending.
      assert(!RiderCarryTerm(s, s, s, 7, 7, true, true).pending);
    }
    // A stale stamp carries nothing; a platform deactivated since its last PreThink carries
    // that PreThink's step, rotation included, never the pose now.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, e{{10.f, 0.f, 0.f}, Yaw(2.f)},
          warped{{50.f, 0.f, 0.f}, Yaw(90.f)};
      assert(!RiderCarryTerm(s, e, e, 6, 7, true, true).pending);
      assert(!RiderCarryTerm(s, warped, e, 5, 7, false, true).pending);
      const CarryTerm t = RiderCarryTerm(s, warped, e, 6, 7, false, true);
      assert(t.rotates && Near(t.yaw, 2.f * kPi / 180.f, 1e-6f));
      const Vec3 c = t.Translation({13.f, 0.f, 1.f});
      assert(Near(c.x, -0.0018f, 1e-4f) && Near(c.y, 0.1047f, 1e-4f) && c.z == 0.f);
    }
    // Moved after its step ended (a SetTransform between the platform's PreThink and the
    // player's move): a teleport, nothing carried, rotated or not. The step alone carries.
    {
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, e{{10.1f, 0.f, 0.f}, Yaw(2.f)};
      const Pose warped{{50.f, 0.f, 0.f}, Yaw(90.f)}, shifted{{10.1f, 0.f, .5f}, Yaw(2.f)};
      const Pose turned{{10.1f, 0.f, 0.f}, Yaw(3.f)};
      assert(!RiderCarryTerm(s, warped, e, 7, 7, true, true).pending);
      assert(!RiderCarryTerm(s, shifted, e, 7, 7, true, true).pending);
      assert(!RiderCarryTerm(s, turned, e, 7, 7, true, true).pending);
      const CarryTerm t = RiderCarryTerm(s, e, e, 7, 7, true, true);
      assert(t.pending && t.rotates && Near(t.carry.x, .1f, 1e-6f));
      // Deactivated after its step and moved in the same tick: a teleport too.
      assert(RiderCarryTerm(s, warped, e, 7, 7, false, true).pending == false);
      assert(RiderCarryTerm(s, e, e, 7, 7, false, true).pending);
    }
    // Tilt and yaw together: the yaw is the heading of column 0's horizontal projection
    // (Orthonormalized 0xd5140), not of row 0. R = RollX(15) * Yaw(20): column 0 is
    // (cos 20, sin 20 cos 15, sin 20 sin 15), heading 19.37 degrees; row 0 would give -20.
    {
      const Basis roll = RollX(15.f), yaw = Yaw(20.f);
      Basis r;
      for (int j = 0; j < 3; ++j) r.c[j] = MulBasis(roll, yaw.c[j]);
      const Pose s{{10.f, 0.f, 0.f}, Basis()}, n{{10.f, 0.f, 0.f}, r};
      const CarryTerm t = RiderCarryTerm(s, n, n, 7, 7, true, true);
      const float a = 20.f * kPi / 180.f, b = 15.f * kPi / 180.f;
      assert(t.rotates && Near(t.yaw, std::atan2(std::sin(a) * std::cos(b), std::cos(a)), 1e-6f));
      assert(Near(t.yaw, 19.37f * kPi / 180.f, 1e-4f));
    }
  }
  // 11. The yaw in the additive orientation delta (YawedOrientationDelta): AddMotionState
  // lands on normalize(q + delta'), which must be normalize(q + delta) * yawq.
  {
    using Q = PortRemasteredGround::Quat4;
    const float kPi = 3.14159265358979f;
    auto Mul = [](const Q& a, const Q& b) {
      return Q{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
               a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
               a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
               a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    };
    auto Norm = [](const Q& a) {
      const float l = std::sqrt(a.w * a.w + a.x * a.x + a.y * a.y + a.z * a.z);
      return Q{a.w / l, a.x / l, a.y / l, a.z / l};
    };
    auto Add = [](const Q& a, const Q& b) { return Q{a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z}; };
    auto Close = [](const Q& a, const Q& b, float eps) {
      // q and -q are the same rotation.
      const float sgn = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z < 0.f ? -1.f : 1.f;
      return std::fabs(a.w - sgn * b.w) <= eps && std::fabs(a.x - sgn * b.x) <= eps &&
             std::fabs(a.y - sgn * b.y) <= eps && std::fabs(a.z - sgn * b.z) <= eps;
    };
    auto About = [](float deg, float x, float y, float z) {
      const float h = 0.5f * deg * 3.14159265358979f / 180.f;
      return Q{std::cos(h), x * std::sin(h), y * std::sin(h), z * std::sin(h)};
    };
    auto Heading = [&](const Q& q) {
      // Body forward (+Y) rotated by q, in the horizontal plane.
      const Q f = Mul(Mul(q, Q{0.f, 0.f, 1.f, 0.f}), Q{q.w, -q.x, -q.y, -q.z});
      return std::atan2(-f.x, f.y);
    };
    const float yaw = 2.f * kPi / 180.f;
    const Q yawq = About(2.f, 0.f, 0.f, 1.f);
    // A tilted and yawed q (25 degrees about X, then 40 about Z) and a nonzero angular step.
    const Q q = Norm(Mul(About(40.f, 0.f, 0.f, 1.f), About(25.f, 1.f, 0.f, 0.f)));
    const Q d{-0.004f, 0.006f, -0.003f, 0.009f};
    const Q landed = Norm(Add(q, YawedOrientationDelta(q, d, yaw)));
    assert(Close(landed, Mul(Norm(Add(q, d)), yawq), 2e-6f));
    // Not yawq * q: the yaw is about the body's up axis, which the tilt has moved.
    assert(!Close(landed, Mul(yawq, Norm(Add(q, d))), 1e-3f));
    // A larger turn and a zero step.
    const Q big = About(-75.f, 0.f, 0.f, 1.f);
    assert(Close(Norm(Add(q, YawedOrientationDelta(q, Q{0.f, 0.f, 0.f, 0.f}, -75.f * kPi / 180.f))),
                 Mul(q, big), 2e-6f));
    // Upright: the heading turns by exactly the yaw, after the step's own turn.
    const Q up = About(30.f, 0.f, 0.f, 1.f);
    const Q step = Add(About(31.f, 0.f, 0.f, 1.f), Q{-up.w, -up.x, -up.y, -up.z});
    const float h = Heading(Norm(Add(up, YawedOrientationDelta(up, step, yaw))));
    assert(std::fabs(h - 33.f * kPi / 180.f) <= 1e-5f);
    // A zero yaw is the delta itself, bit for bit (no rounding from q + delta - q).
    const Q same = YawedOrientationDelta(q, d, 0.f);
    assert(same.w == d.w && same.x == d.x && same.y == d.y && same.z == d.z);
  }
  return 0;
}
