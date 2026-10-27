#include "port_electric2_math.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace PortElectric2;

namespace {

bool Same(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
  BoltParams prm;
  prm.nodes = 8;
  const V3 start{0.f, 0.f, 0.f};
  const V3 end{4.f, 0.f, 0.f};

  // Same seed, same bolt; bit for bit.
  std::vector<V3> a, b;
  Rng r1, r2;
  CalculatePointsForBolt(r1, prm, start, end, {}, {}, a, 0.5f, 0.1f);
  CalculatePointsForBolt(r2, prm, start, end, {}, {}, b, 0.5f, 0.1f);
  CHECK_MSG(a.size() == 8 && b.size() == 8, "node count is SSEG");
  bool same = a.size() == b.size();
  for (size_t i = 0; same && i < a.size(); ++i) {
    same = Same(a[i], b[i]);
  }
  CHECK_MSG(same, "fixed seed is deterministic");
  CHECK_MSG(Same(a.front(), start) && Same(a.back(), end), "endpoints stay on start and end");
  CHECK_MSG(r1.seed == r2.seed && r1.seed != 99u, "rng advanced identically");

  // A fractal amplitude moves the inner nodes; zero amplitude leaves them near the line.
  std::vector<V3> flat;
  Rng r3;
  CalculatePointsForBolt(r3, prm, start, end, {}, {}, flat, 0.f, 0.f);
  float maxOff = 0.f, maxFlat = 0.f;
  for (size_t i = 1; i + 1 < a.size(); ++i) {
    maxOff = std::fmax(maxOff, std::sqrt(a[i].y * a[i].y + a[i].z * a[i].z));
    maxFlat = std::fmax(maxFlat, std::sqrt(flat[i].y * flat[i].y + flat[i].z * flat[i].z));
  }
  CHECK_MSG(maxFlat == 0.f, "no amplitude gives a straight bolt");
  CHECK_MSG(maxOff > 0.f, "amplitude bends the bolt");

  // A start direction takes the Bezier path and still pins both ends.
  std::vector<V3> bez;
  Rng r4;
  CalculatePointsForBolt(r4, prm, start, end, V3{0.f, 1.f, 0.f}, {}, bez, 0.f, 0.f);
  CHECK_MSG(bez.size() == 8 && Same(bez.front(), start) && Same(bez.back(), end), "Bezier ends pinned");
  CHECK_MSG(bez[3].y > 0.f, "Bezier bends toward the start direction");

  // Both shipped Wave ELC2 assets author IEMT/FEMT direction=(0, 0.01, 0). The raw
  // CalculatePointsForBolt branch at 0xb73cc0 sends that pair directly to the cubic path;
  // it bypasses the later count comparison/linear loop at 0xb73d84/0xb73e48.
  std::vector<V3> waveCurve;
  Rng waveRng;
  const V3 waveDirection{0.f, 0.01f, 0.f};
  CalculatePointsForBolt(waveRng, prm, start, end, waveDirection, waveDirection, waveCurve, 0.f, 0.f);
  CHECK_MSG(waveCurve.size() == 8 && Same(waveCurve.front(), start) && Same(waveCurve.back(), end) &&
                waveCurve[3].y > 0.f,
            "Wave's paired nonzero emitter directions take the four-control cubic path");

  // One node count and one point per node for the minimum.
  BoltParams one;
  one.nodes = 2;
  std::vector<V3> two;
  Rng r5;
  CalculatePointsForBolt(r5, one, start, end, {}, {}, two, 0.5f, 0.1f);
  CHECK_MSG(two.size() == 2 && Same(two[1], end), "two-node bolt is its endpoints");

  // SIDE=2, NFOM=0 uses camera-forward x the endpoint tangent for every node, rather than
  // the swoosh renderer's local perspective frame. The chosen axes also pin the source handedness.
  const std::vector<V3> bent{{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 1.f, 0.f}, {1.f, 1.f, 1.f}};
  const V3 cameraForward{0.f, 0.f, 1.f};
  V3 sharedAxis{};
  CHECK_MSG(Electric2RibbonWidthAxis(bent, 0, 0, 2, cameraForward, sharedAxis),
            "SIDE=2 shared ribbon basis is valid");
  bool shared = true;
  for (size_t i = 1; i < bent.size(); ++i) {
    V3 axis{};
    shared = shared && Electric2RibbonWidthAxis(bent, i, 0, 2, cameraForward, axis) && Same(axis, sharedAxis);
  }
  CHECK_MSG(shared, "NFOM=0 shares the first-to-last tangent basis across nodes");
  V3 otherNfomAxis{};
  CHECK_MSG(Electric2RibbonWidthAxis(bent, 0, 2, 2, cameraForward, otherNfomAxis) &&
                Same(otherNfomAxis, sharedAxis),
            "NFOM values other than 1 follow the source's shared-tangent branch");

  V3 screenUp{};
  CHECK_MSG(Electric2RibbonWidthAxis(std::vector<V3>{{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}}, 0, 0, 2,
                                    cameraForward, screenUp) &&
                Same(screenUp, V3{0.f, 1.f, 0.f}),
            "camera-forward x forward beam gives positive screen-up width");
  V3 perNode0{}, perNode1{};
  CHECK_MSG(Electric2RibbonWidthAxis(bent, 0, 1, 2, cameraForward, perNode0) &&
                Electric2RibbonWidthAxis(bent, 1, 1, 2, cameraForward, perNode1) &&
                !Same(perNode0, perNode1),
            "NFOM=1 uses per-node next-segment tangents");

  const std::vector<V3> degenerate{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
  V3 fallbackAxis{};
  CHECK_MSG(Electric2RibbonWidthAxis(degenerate, 0, 0, 2, cameraForward, fallbackAxis) &&
                Same(fallbackAxis, V3{1.f, 0.f, 0.f}),
            "degenerate tangent and camera cross use source forward/up fallbacks");
  const std::vector<V3> parallel{{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
  CHECK_MSG(!Electric2RibbonWidthAxis(parallel, 0, 0, 2, V3{0.f, 1.f, 0.f}, fallbackAxis),
            "two degenerate source cross attempts do not invent a third basis");

  V3 theta0{}, theta180{};
  Electric2RibbonSidePoints(V3{10.f, 20.f, 30.f}, V3{0.f, 1.f, 0.f}, 0.25f, 0.1f, theta0, theta180);
  CHECK_MSG(Near(theta0.y, 20.1f) && Near(theta180.y, 19.75f),
            "SIDE=2 theta 0 uses RRAD and theta 180 uses LRAD");

  // One node's half-widths (UpdateBolt 0xb77638): RADS scales both sides, LLRD copies LRAD to
  // RRAD, and a present RRAD overwrites that copy.
  const float l = 0.2f, r = 0.1f;
  const BoltRadii radii = ComputeBoltRadii(0.5f, &l, &r, false);
  CHECK_MSG(Near(radii.left, 0.1f) && Near(radii.right, 0.05f), "RADS scales LRAD and RRAD");
  const BoltRadii copied = ComputeBoltRadii(0.5f, &l, nullptr, true);
  CHECK_MSG(Near(copied.left, 0.1f) && Near(copied.right, 0.1f), "LLRD copies LRAD to a missing RRAD");
  const BoltRadii overwritten = ComputeBoltRadii(0.5f, &l, &r, true);
  CHECK_MSG(Near(overwritten.left, 0.1f) && Near(overwritten.right, 0.05f), "a present RRAD overwrites the LLRD copy");
  const BoltRadii defaults = ComputeBoltRadii(1.f, nullptr, nullptr, true);
  CHECK_MSG(Near(defaults.left, 0.1f) && Near(defaults.right, 0.1f),
            "a node with no RADS/LRAD/RRAD is RADS 1 x 0.1 on both sides");
  const BoltRadii zero = ComputeBoltRadii(0.f, &l, &r, true);
  CHECK_MSG(Near(zero.left, 0.f) && Near(zero.right, 0.f), "a RADS taper's 0 key gives a zero-width node");

  constexpr float rawIntensity = 0.37f;
  bool cancels = true;
  for (float exposure : {0.25f, 0.5f, 1.f, 2.f, 4.f}) {
    const float compensated = ExposureCompensatedIntensity(rawIntensity, exposure);
    cancels = cancels && Near(compensated * exposure, rawIntensity);
  }
  CHECK_MSG(cancels, "ELC2 inverse exposure cancels the VFX exposure at varied frame exposures");
  CHECK_MSG(ExposureCompensatedIntensity(rawIntensity, 0.f) == rawIntensity &&
                ExposureCompensatedIntensity(rawIntensity, std::numeric_limits<float>::infinity()) == rawIntensity,
            "missing or invalid exposure leaves ELC2 intensity unchanged");

  if (port_test::Failures() == 0) {
    std::puts("port_electric2_bolt: ok");
  }
  return PORT_TEST_RESULT();
}
