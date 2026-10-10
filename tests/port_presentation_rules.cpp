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
  double x, y, z;
};
// Rotation about the X axis (a ball rolling along Y).
V RotX(const V& v, double a) {
  return {v.x, v.y * std::cos(a) - v.z * std::sin(a), v.y * std::sin(a) + v.z * std::cos(a)};
}
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

  // Geometry: the ball centre is origin + (0,0,r) in world space. Rotating
  // that offset with the blended roll (the old origin pivot) moves the centre
  // off the straight line; pivoting about the centre lerps it exactly.
  const double r = 0.5, roll = 1.0; // ~57 degrees a tick
  for (int i = 1; i < 10; ++i) {
    const double t = i / 10.0;
    const V p0{0, 0, 0}, p1{0, 0.4, 0};
    const V want{0, p0.y + (p1.y - p0.y) * t, r};
    // Old: blend(p, Rb) * Rc^-1 applied to the lifted centre.
    const V off = RotX({0, 0, r}, roll * t - roll);
    const V oldCentre{0, p0.y + (p1.y - p0.y) * t + off.y, off.z};
    CHECK(std::fabs(oldCentre.z - want.z) > 1e-3);
    // New: the centre itself lerps.
    CHECK(std::fabs((p0.z + r) - want.z) < 1e-12);
  }
  return 0;
}
