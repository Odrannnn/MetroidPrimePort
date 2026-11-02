// Remastered's particle depth bias (DBIS): the replaced projection m22 makes a vertex at view depth
// d land at the NDC depth of d + bias (CalculateProjMatrixDepthBias 0x22a604). Hand-computed with
// a GX frustum, near 1 / far 101: m22 = -n/(f-n) = -0.01, m23 = -fn/(f-n) = -1.01, m32 = -1, m33 = 0.
#include "Kyoto/Particles/PortParticleMath.hpp"
#include "port_test.h"

#include <cmath>

namespace {

bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

double Ndc(double m22, double m23, double d) { return (m23 - m22 * d) / d; }

void TestBias() {
  const float m22 = -0.01f, m23 = -1.01f;
  // d 10, bias -5: ndc(5) - ndc(10) = -0.192 + 0.091 = -0.101, so m22 = -0.01 + 0.101 = 0.091.
  const float biased = PortParticleMath::DepthBiasedProjM22(m22, m23, -1.f, 0.f, 10.f, -5.f);
  CHECK_MSG(Near(biased, 0.091), "m22 offset by the NDC shift");
  CHECK_MSG(Near(Ndc(biased, m23, 10.0), Ndc(m22, m23, 5.0)), "d lands at the depth of d + bias");
  // The shift is the same NDC amount at any depth the vertex has (clip z / w is m22-linear).
  CHECK_MSG(Near(Ndc(biased, m23, 40.0) - Ndc(m22, m23, 40.0), -0.101), "constant shift");
  CHECK_MSG(Near(PortParticleMath::DepthBiasedProjM22(m22, m23, -1.f, 0.f, 10.f, 0.f), m22), "bias 0 changes nothing");
  // A bias away from the camera moves the other way.
  CHECK_MSG(PortParticleMath::DepthBiasedProjM22(m22, m23, -1.f, 0.f, 10.f, 5.f) < m22, "positive bias is farther");
  // Depth below the epsilon clamps instead of dividing by zero.
  CHECK_MSG(std::isfinite(PortParticleMath::DepthBiasedProjM22(m22, m23, -1.f, 0.f, -3.f, -1.f)), "negative depth is finite");
  // Orthographic (m32 0, m33 1): ndc = m23 - m22 * d, shift -0.1, so m22 = -0.02 + 0.1.
  const float ortho = PortParticleMath::DepthBiasedProjM22(-0.02f, -1.02f, 0.f, 1.f, 10.f, -5.f);
  CHECK_MSG(Near(ortho, 0.08), "orthographic");
  // DBSO: the shift times the depth range's span, -0.101 * 0.5.
  CHECK_MSG(Near(PortParticleMath::DepthBiasedProjM22(m22, m23, -1.f, 0.f, 10.f, -5.f, 0.5f), -0.01 + 0.0505),
            "range span scales the shift");
  // Retail's world and gun ranges take Remastered's (0.0390625, 1) and (0.0097656, 0.0386719).
  CHECK_MSG(Near(PortParticleMath::RemasteredDepthRangeSpan(0.125f, 1.f), 0.9609375), "world span");
  CHECK_MSG(Near(PortParticleMath::RemasteredDepthRangeSpan(1.f / 64.f, 1.f / 32.f), 0.0289063, 1e-6), "gun span");
  CHECK_MSG(Near(PortParticleMath::RemasteredDepthRangeSpan(0.f, 1.f / 512.f), 1.0 / 512.0), "other ranges keep theirs");
}

}  // namespace

int main() {
  TestBias();
  return PORT_TEST_RESULT();
}
