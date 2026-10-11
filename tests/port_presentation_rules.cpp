// Presented-transform rules (issue #70): camera snapshots only count in the
// tick that wrote them, and the rolling ball blends about its centre.

#include "port_presentation_rules.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "presentation rules regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

struct V {
  double x = 0, y = 0, z = 0;
  V operator+(const V& o) const { return {x + o.x, y + o.y, z + o.z}; }
  V operator-(const V& o) const { return {x - o.x, y - o.y, z - o.z}; }
  V operator*(float f) const { return {x * f, y * f, z * f}; }
  float MagSquared() const { return float(x * x + y * y + z * z); }
};
// Orientation = rotation about X (a ball rolling along Y); quaternion = angle.
struct Q {
  double a = 0;
};
struct Xf {
  V pos;
  double rollX = 0; // rotation about X
};
// V is the vector used by the production template; rotate the lifted centre.
V RotX(const V& v, double a) {
  return {v.x, v.y * std::cos(a) - v.z * std::sin(a), v.y * std::sin(a) + v.z * std::cos(a)};
}
struct Ops {
  Xf Rigid(const Xf& xf, Q& q) const {
    q.a = xf.rollX;
    return xf;
  }
  V Translation(const Xf& xf) const { return xf.pos; }
  void SetTranslation(Xf& xf, const V& v) const { xf.pos = v; }
  float AbsDot(const Q& a, const Q& b) const { return float(std::fabs(std::cos((b.a - a.a) / 2))); }
  Q Slerp(const Q& a, const Q& b, float t) const { return {a.a + (b.a - a.a) * t}; }
  Xf Build(const Q& q, const V& c) const { return {c, q.a}; }
};
} // namespace

int main() {
  // Snapshots: fresh only in their own tick; generation 0 is never current.
  CHECK(PortSnapshotFresh(5, 5));
  CHECK(!PortSnapshotFresh(5, 6)); // a tick that skipped the camera Update
  CHECK(!PortSnapshotFresh(5, 9));
  CHECK(!PortSnapshotFresh(0, 0));
  CHECK(!PortSnapshotFresh(3, 0));

  // Rules: teleports always cut; turns cut ordinary actors only.
  CHECK(PortClassifyRigidBlend(0.f, 1.f, false) == PortRigidBlendKind::Full);
  CHECK(PortClassifyRigidBlend(16.1f, 1.f, false) == PortRigidBlendKind::Cut);
  CHECK(PortClassifyRigidBlend(16.1f, 1.f, true) == PortRigidBlendKind::Cut);
  CHECK(PortClassifyRigidBlend(0.01f, 0.5f, false) == PortRigidBlendKind::Cut);
  CHECK(PortClassifyRigidBlend(0.01f, 0.5f, true) == PortRigidBlendKind::TranslateOnly);
  CHECK(PortClassifyRigidBlend(0.01f, 0.95f, true) == PortRigidBlendKind::Full);

  // Production blend (PortBlendRigidGeneric, the body of CActor::PortBlendRigid).
  const Ops ops;
  const float r = 0.5f;
  const V lift{0, 0, r};
  auto blend = [&](const Xf& a, const Xf& b, float t, Xf& bl, Xf& cur, bool pivoted) {
    return PortBlendRigidGeneric<Xf, V, Q>(ops, a, b, t, bl, cur, pivoted ? lift : V{}, pivoted);
  };
  // The ball: origin moves 0.4 along Y, rolls ~34 degrees a tick. The drawn
  // centre is blend^-1-composed: view = C * R^-1; the presented centre of the
  // lifted offset is blendCentre + Rb * Rc^-1 * (curCentre - curCentre) = the
  // blend centre itself, so it must lerp the sim centres exactly.
  const Xf a{{0, 0, 0}, 0.0}, b{{0, 0.4, 0}, 0.6};
  for (int i = 0; i <= 10; ++i) {
    const float t = i / 10.f;
    Xf bl, cur;
    CHECK(blend(a, b, t, bl, cur, true));
    CHECK(std::fabs(bl.pos.y - 0.4 * t) < 1e-6);
    CHECK(std::fabs(bl.pos.z - r) < 1e-6);
    CHECK(std::fabs(bl.rollX - 0.6 * t) < 1e-6);
    // Endpoints: t=0 = previous pose, t=1 = current pose.
    if (i == 0)
      CHECK(std::fabs(bl.pos.y) < 1e-6 && std::fabs(bl.rollX) < 1e-9);
    if (i == 10)
      CHECK(std::fabs(bl.pos.y - 0.4) < 1e-6 && std::fabs(bl.rollX - 0.6) < 1e-9);
    // The old origin pivot: centre = blend origin + R_blend R_cur^-1 lift.
    const V off = RotX(lift, bl.rollX - cur.rollX);
    const double oldZ = off.z, oldY = 0.4 * t + off.y;
    if (i > 0 && i < 10)
      CHECK(std::fabs(oldY - 0.4 * t) > 1e-3 || std::fabs(oldZ - r) > 1e-3);
  }
  // Big roll (>45 degrees, 2 rad): pivoted keeps translation, current rotation.
  {
    const Xf big{{0, 0.4, 0}, 2.0};
    Xf bl, cur;
    CHECK(blend(a, big, 0.5f, bl, cur, true));
    CHECK(std::fabs(bl.pos.y - 0.2) < 1e-6);
    CHECK(std::fabs(bl.rollX - 2.0) < 1e-9); // TranslateOnly: sim rotation
    // The same turn cuts an ordinary actor.
    CHECK(!blend(a, big, 0.5f, bl, cur, false));
  }
  // Teleport cuts even when pivoted.
  {
    const Xf far{{0, 10, 0}, 0.0};
    Xf bl, cur;
    CHECK(!blend(a, far, 0.5f, bl, cur, true));
  }
  return 0;
}
