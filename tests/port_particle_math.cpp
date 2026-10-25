#include "Kyoto/Particles/PortParticleMath.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/CRandom16.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdio>

namespace {

bool Near(float a, float b, float epsilon = 0.0001f) {
  return a > b - epsilon && a < b + epsilon;
}

bool NearVector(const CVector3f& a, const CVector3f& b, float epsilon = 0.0001f) {
  return Near(a.GetX(), b.GetX(), epsilon) && Near(a.GetY(), b.GetY(), epsilon) &&
         Near(a.GetZ(), b.GetZ(), epsilon);
}

bool Color(const float* out, float r, float g, float b, float a) {
  return Near(out[0], r) && Near(out[1], g) && Near(out[2], b) && Near(out[3], a);
}
} // namespace

int main() {
  using namespace PortParticleMath;

  CHECK_MSG(NearVector(DefaultParticleVelocity(0), CVector3f::Zero()), "DVVN 0 has no default motion");
  CHECK_MSG(NearVector(DefaultParticleVelocity(1), CVector3f(0.f, 1.1f, 0.f)), "DVVN 1 default forward velocity");
  CHECK_MSG(NearVector(DefaultParticleVelocity(2), CVector3f(0.f, 0.f, 1.1f)), "DVVN 2 default up velocity");
  CHECK_MSG(NearVector(DefaultParticleVelocity(3), CVector3f(0.f, 0.f, 1.1f)), "DVVN 3 default up velocity");
  CHECK_MSG(NearVector(DefaultParticleVelocity(4), CVector3f(0.f, 0.f, 0.25f)), "DVVN 4 default up velocity");
  CHECK_MSG(NearVector(DefaultParticleVelocity(5), CVector3f::Zero()) &&
                NearVector(DefaultParticleVelocity(255), CVector3f::Zero()), "unknown DVVN versions have no motion");

  // DFCS/DFCP, CalculateBlendValue 0x298f44: (d - B) / (A - B), clamped; 1 at A, 0 at B.
  CHECK_MSG(Near(DistanceBlend(10.f, 10.f, 2.f), 1.f), "blend 1 at A");
  CHECK_MSG(Near(DistanceBlend(2.f, 10.f, 2.f), 0.f), "blend 0 at B");
  CHECK_MSG(Near(DistanceBlend(6.f, 10.f, 2.f), 0.5f), "blend midpoint");
  CHECK_MSG(Near(DistanceBlend(20.f, 10.f, 2.f), 1.f), "blend clamps high");
  CHECK_MSG(Near(DistanceBlend(-5.f, 10.f, 2.f), 0.f), "blend clamps low");
  CHECK_MSG(Near(DistanceBlend(6.f, 2.f, 10.f), 0.5f), "blend reversed midpoint");
  CHECK_MSG(Near(DistanceBlend(2.f, 2.f, 10.f), 1.f), "blend reversed 1 at A");
  CHECK_MSG(Near(DistanceBlend(7.f, 5.f, 5.000001f), 0.f), "blend equal children is 0");

  // The Wave trail gradient (f597d7d3): HDR halves, period 60, no repeat.
  const GradientStop wave[] = {
      {{2.121f, 1.066f, 3.619f, 1.f}, 0.f},          {{1.260f, 0.797f, 2.736f, 1.f}, 0.0648f},
      {{0.0463f, 0.4167f, 1.492f, 0.765f}, 0.1561f}, {{0.0589f, 0.4099f, 1.523f, 0.75f}, 0.1620f},
      {{0.638f, 0.0961f, 2.969f, 0.f}, 0.4329f},     {{0.645f, 0.0927f, 2.984f, 0.f}, 0.4358f},
  };
  float out[4];
  CHECK_MSG(EvalGradient(wave, 6, 60, false, 0, out) && Color(out, 2.121f, 1.066f, 3.619f, 1.f), "frame 0 is the first stop, HDR kept");
  // t = 30/60 = 0.5 is past the last stop: held.
  CHECK_MSG(EvalGradient(wave, 6, 60, false, 30, out) && Color(out, 0.645f, 0.0927f, 2.984f, 0.f), "holds the last stop");
  CHECK_MSG(EvalGradient(wave, 6, 60, false, 500, out) && Color(out, 0.645f, 0.0927f, 2.984f, 0.f), "clamps past the period");
  // Frame 6 -> t = 0.1, between stops 1 and 2: f = (0.1 - 0.0648) / (0.1561 - 0.0648).
  {
    const float f = (0.1f - 0.0648f) / (0.1561f - 0.0648f);
    CHECK_MSG(EvalGradient(wave, 6, 60, false, 6, out) &&
                  Color(out, 1.260f + (0.0463f - 1.260f) * f, 0.797f + (0.4167f - 0.797f) * f,
                        2.736f + (1.492f - 2.736f) * f, 1.f + (0.765f - 1.f) * f),
              "lerps all four channels, alpha included");
  }

  // Two stops: ramp over the period, then repeat.
  const GradientStop ramp[] = {{{0.f, 0.f, 0.f, 0.f}, 0.f}, {{4.f, 2.f, 1.f, 1.f}, 1.f}};
  CHECK_MSG(EvalGradient(ramp, 2, 10, false, 5, out) && Color(out, 2.f, 1.f, 0.5f, 0.5f), "linear ramp, HDR above 1");
  CHECK_MSG(EvalGradient(ramp, 2, 10, false, 10, out) && Color(out, 4.f, 2.f, 1.f, 1.f), "end of the period is the last stop");
  CHECK_MSG(EvalGradient(ramp, 2, 10, true, 13, out) && Color(out, 1.2f, 0.6f, 0.3f, 0.3f), "repeat is frame mod period");
  CHECK_MSG(EvalGradient(ramp, 2, 10, true, 10, out) && Color(out, 0.f, 0.f, 0.f, 0.f), "repeat wraps to the first stop");
  CHECK_MSG(EvalGradient(ramp, 2, 10, true, -3, out) && Color(out, 2.8f, 1.4f, 0.7f, 0.7f), "negative frame wraps");

  // A period below 2 is 1: the position is 0 at frame 0, then 1.
  CHECK_MSG(EvalGradient(ramp, 2, 0, false, 0, out) && Color(out, 0.f, 0.f, 0.f, 0.f), "period 0 acts as 1, frame 0");
  CHECK_MSG(EvalGradient(ramp, 2, 1, false, 1, out) && Color(out, 4.f, 2.f, 1.f, 1.f), "period 1, frame 1");
  CHECK_MSG(EvalGradient(ramp, 2, -7, false, 3, out) && Color(out, 4.f, 2.f, 1.f, 1.f), "negative period acts as 1");

  // Stops that do not start at 0 or end at 1 hold their colour outside.
  const GradientStop inner[] = {{{1.f, 0.f, 0.f, 1.f}, 0.25f}, {{0.f, 1.f, 0.f, 1.f}, 0.75f}};
  CHECK_MSG(EvalGradient(inner, 2, 4, false, 0, out) && Color(out, 1.f, 0.f, 0.f, 1.f), "holds before the first stop");
  CHECK_MSG(EvalGradient(inner, 2, 4, false, 4, out) && Color(out, 0.f, 1.f, 0.f, 1.f), "holds after the last stop");
  CHECK_MSG(EvalGradient(inner, 2, 4, false, 2, out) && Color(out, 0.5f, 0.5f, 0.f, 1.f), "interpolates between inner stops");

  // Converted Remastered IRND values are keyed by the particle's spawn seed
  // and the individual expression node, so repeated frame reads do not reroll.
  CElementGen::CParticle particleA;
  CElementGen::CParticle particleB;
  particleA.xPortSeed = 0x12345678u;
  particleB.xPortSeed = 0x12345679u;
  int angleExpression = 0;
  int nestedExpression = 0;
  const u32 sample = CParticleGlobals::PortIrndBits(&particleA, &angleExpression);
  CHECK_MSG(sample == CParticleGlobals::PortIrndBits(&particleA, &angleExpression),
            "IRND sampling is stable for the same particle and expression");
  CHECK_MSG(sample != CParticleGlobals::PortIrndBits(&particleB, &angleExpression),
            "IRND sampling varies with the particle seed");
  CHECK_MSG(sample != CParticleGlobals::PortIrndBits(&particleA, &nestedExpression),
            "separate IRND expressions sample independently");
  const float irndUnit = CParticleGlobals::PortIrndUnit(&particleA, &angleExpression);
  CHECK_MSG(irndUnit >= 0.f && irndUnit < 1.f &&
                irndUnit == static_cast<float>(sample >> 8) * (1.f / 16777216.f),
            "IRND real sample is the stable unit hash in [0, 1)");

  // Center/sort positions use the camera rotation inverse only for XFMD3 (which follows the
  // emitter); the legacy system-camera transform remains the direction transform. These raw
  // inputs are the held-frame records for 6E63E6B8 / 5ECDEEA2 in
  // build/s-powercharge-native-20261017/upper-component/live-centers/probe-records.jsonl.
  {
    const CTransform4f cameraRotation(
        1.f, 0.f, 2.82982082e-15f, 0.f,
        9.31322575e-10f, 0.999898493f, -0.0142413424f, 0.f,
        1.45490859e-11f, 0.0142413424f, 0.999898493f, 0.f);
    const CTransform4f cameraRotationInverse(cameraRotation.GetQuickInverse());
    const CTransform4f globalOrientation(
        -0.999991f, -0.000940264261f, 0.00414450979f, 0.f,
        0.00414583506f, -0.00141192845f, 0.999990404f, 0.f,
        -0.000934403448f, 0.999998569f, 0.00141581392f, 0.f);
    const CTransform4f systemCameraCopy(cameraRotationInverse * globalOrientation);
    const CVector3f particle(0.000621676736f, 0.149998531f, 0.000212372004f);
    const CVector3f previous(0.000621676736f, 0.149998531f, 0.000212372004f);
    const CVector3f translation(0.00792796165f, 1.28630543f, 0.00187567004f);
    const CTransform4f model = CTransform4f::Translate(translation) *
                               CTransform4f::Scale(2.f, 2.f, 2.f) * cameraRotation *
                               CTransform4f::Identity();

    const CVector3f followFrame = ParticlePositionForSort(
        cameraRotationInverse, systemCameraCopy, true, particle, previous, 1.f);
    const CVector3f followWorld = model * followFrame;
    CHECK_MSG(NearVector(followWorld, CVector3f(0.00917131547f, 1.5863024f, 0.00230041379f),
                         0.00002f),
              "XFMD3 center removes the extra Q and matches the captured mesh anchor");

    const CVector3f xfmd2Frame = ParticlePositionForSort(
        cameraRotationInverse, systemCameraCopy, false, particle, previous, 1.f);
    const CVector3f xfmd2World = model * xfmd2Frame;
    CHECK_MSG(NearVector(xfmd2World, CVector3f(0.00640430301f, 1.28631175f, 0.301871687f),
                         0.00002f),
              "XFMD2 keeps the original system-camera center path");
    const CVector3f defaultFrame = ParticlePositionForSort(
        cameraRotationInverse, systemCameraCopy, false, particle, previous, 1.f);
    CHECK_MSG(NearVector(defaultFrame, systemCameraCopy * particle),
              "default XFMD retains the original position transform");

    // The actual position helper interpolates before transforming, and its returned camera-space
    // Y is the same farthest-first sort key consumed by PortRenderParticlesVfx.
    const CVector3f interpolated = ParticlePositionForSort(
        cameraRotationInverse, systemCameraCopy, true, CVector3f(0.f, 0.8f, 0.f),
        CVector3f(0.f, 0.4f, 0.f), 0.25f);
    const CVector3f farther = ParticlePositionForSort(
        cameraRotationInverse, systemCameraCopy, true, CVector3f(0.f, 0.7f, 0.f),
        CVector3f(0.f, 0.7f, 0.f), 0.25f);
    CHECK_MSG(Near(interpolated.GetY(), 0.5f, 0.0002f),
              "XFMD3 sort position uses the current/previous interpolation");
    CHECK_MSG(farther.GetY() > interpolated.GetY(),
              "sort ordering uses transformed interpolated positions");

    const CVector3f direction(0.25f, 0.5f, -0.75f);
    const CVector3f directionFrame = ParticleDirectionInCamera(systemCameraCopy, direction);
    CHECK_MSG(NearVector(directionFrame, systemCameraCopy.Rotate(direction)),
              "oriented directions retain systemCameraCopy");
    CHECK_MSG(!NearVector(directionFrame, cameraRotationInverse.Rotate(direction)),
              "oriented directions still include global orientation Q");
  }

  // No stops: Remastered returns 0.
  CHECK_MSG(!EvalGradient(inner, 0, 4, false, 1, out) && Color(out, 0.f, 0.f, 0.f, 0.f), "no stops yields zero");

  // RNDV: ARM FRSQRTE estimates (8 bits), one FRSQRTS step, so a unit-length
  // cube point comes out just short of the magnitude.
  CHECK_MSG(ReciprocalSqrtEstimate(1.f) == 0.998046875f, "FRSQRTE(1)");
  CHECK_MSG(ReciprocalSqrtEstimate(4.f) == 0.4990234375f, "FRSQRTE(4)");
  CHECK_MSG(ReciprocalSqrtEstimate(0.25f) == 1.99609375f, "FRSQRTE(0.25)");
  {
    const CVector3f axis = RandomVector(1.f, 0.5f, 0.5f, 2.f);
    CHECK_MSG(Near(axis.GetX(), 1.99998856f, 1e-6f) && axis.GetX() != 2.f && axis.GetY() == 0.f &&
                  axis.GetZ() == 0.f,
              "RNDV axis point carries the estimate's error");
    const CVector3f corner = RandomVector(0.f, 0.f, 0.f, 1.f);
    CHECK_MSG(NearVector(corner, CVector3f(-0.57735f, -0.57735f, -0.57735f)), "RNDV corner normalised");
  }
  // FRECPE/FRECPS (UpdateInverseScale 0x2a6390), SqrtF 0x65f70, NORM and CPSS.
  CHECK_MSG(ReciprocalEstimate(1.f) == 0.998046875f, "FRECPE(1)");
  CHECK_MSG(ReciprocalEstimate(-2.f) == -0.4990234375f, "FRECPE(-2)");
  CHECK_MSG(std::isinf(ReciprocalEstimate(0.f)) && ReciprocalEstimate(INFINITY) == 0.f, "FRECPE 0 and inf");
  CHECK_MSG(ReciprocalStep(0.f, INFINITY) == 2.f, "FRECPS 0*inf");
  CHECK_MSG(Near(InverseScale(1.f), 1.f, 1e-7f) && Near(InverseScale(4.f), 0.25f, 1e-7f), "inverse scale");
  CHECK_MSG(SqrtF(0.f) == 0.f && SqrtF(-1.f) == 0.f && SqrtF(NAN) == 0.f, "SqrtF of v <= 0 is 0");
  CHECK_MSG(Near(SqrtF(9.f), 3.f, 1e-6f), "SqrtF(9)");
  {
    const CVector3f unit = NormalizeVector(CVector3f(3.f, 0.f, 4.f));
    CHECK_MSG(NearVector(unit, CVector3f(0.6f, 0.f, 0.8f), 1e-6f), "NORM scales to unit length");
    const CVector3f tiny(1e-8f, 0.f, 0.f);
    CHECK_MSG(NormalizeVector(tiny).GetX() == 1e-8f, "NORM keeps |v| <= 2^-23");
    CHECK_MSG(NormalizeVector(CVector3f(0.f, 0.f, 0.f)).GetX() == 0.f, "NORM keeps zero");
  }
  {
    const CVector3f camera(5.f, 2.f, 1.f);
    const CVector3f one(1.f, 1.f, 1.f);
    CHECK_MSG(NearVector(CameraPositionInSystemSpace(camera, CVector3f(1.f, 1.f, 1.f), 1.f, 0.f, 0.f, 0.f, one),
                         CVector3f(4.f, 1.f, 0.f), 1e-6f),
              "CPSS identity frame is camera - t");
    // 90 degrees about Z: world +X is the system's -Y.
    const float h = 0.70710678f;
    CHECK_MSG(NearVector(CameraPositionInSystemSpace(CVector3f(1.f, 0.f, 0.f), CVector3f(0.f, 0.f, 0.f), h, 0.f,
                                                     0.f, h, one),
                         CVector3f(0.f, -1.f, 0.f), 1e-6f),
              "CPSS turns into the system's frame");
    CHECK_MSG(NearVector(CameraPositionInSystemSpace(camera, CVector3f(0.f, 0.f, 0.f), 1.f, 0.f, 0.f, 0.f,
                                                     CVector3f(2.f, 4.f, 0.5f)),
                         CVector3f(2.5f, 0.5f, 2.f), 1e-6f),
              "CPSS divides by the system scale");
  }
  // RAZY (CERotationFromAxis 0x29d1bc): Z = first, X = second x Z, Y = Z x X.
  {
    CVector3f x, y, z;
    CHECK_MSG(RotationFromAxisZY(CVector3f(0.f, 0.f, 2.f), CVector3f(0.f, 3.f, 0.f), x, y, z) &&
                  NearVector(x, CVector3f(1.f, 0.f, 0.f), 1e-6f) && NearVector(y, CVector3f(0.f, 1.f, 0.f), 1e-6f) &&
                  NearVector(z, CVector3f(0.f, 0.f, 1.f), 1e-6f),
              "RAZY Z up, Y second is the identity");
    CHECK_MSG(RotationFromAxisZY(CVector3f(1.f, 0.f, 0.f), CVector3f(0.f, 0.f, 1.f), x, y, z) &&
                  NearVector(x, CVector3f(0.f, 1.f, 0.f), 1e-6f) && NearVector(y, CVector3f(0.f, 0.f, 1.f), 1e-6f) &&
                  NearVector(z, CVector3f(1.f, 0.f, 0.f), 1e-6f),
              "RAZY rows follow first, then second x first");
    CHECK_MSG(!RotationFromAxisZY(CVector3f(0.f, 0.f, 0.f), CVector3f(0.f, 1.f, 0.f), x, y, z),
              "RAZY refuses a zero first vector");
    CHECK_MSG(!RotationFromAxisZY(CVector3f(0.f, 2.f, 0.f), CVector3f(0.f, 1.f, 0.f), x, y, z),
              "RAZY refuses parallel vectors");
  }
  {
    CRandom16 random(0);
    CHECK_MSG(random.PortFloat28() == 12345.f / 268435456.f, "Remastered draw is the low 28 bits");
    CHECK_MSG(random.GetSeed() == 0x3039u, "one LCG step per draw");
  }
  // ANCR/ASPR (CVEAngleCone 0x29eebc, CVEAngleSphere 0x2cb100) and their REUL order 0.
  {
    CHECK_MSG(WrapAngle(1.f) == 1.f, "an angle within pi is not wrapped");
    CHECK_MSG(std::fabs(WrapAngle(4.f) - (4.f - 6.2831853f)) < 1e-5f, "an angle past pi comes back a turn");
    CHECK_MSG(FastSinR(0.f) == 0.f && FastCosR(0.f) == 1.f, "FastSinR/FastCosR at 0");
    const Quat identity = EulerQuat(0.f, 0.f, 0.f);
    CHECK_MSG(std::fabs(identity.w - 1.f) < 1e-4f && identity.x == 0.f && identity.y == 0.f && identity.z == 0.f,
              "REUL(0, 0, 0) is the identity");
    const Quat yaw = EulerQuat(0.f, 90.f, 0.f);
    CHECK_MSG(std::fabs(yaw.w - 0.70710677f) < 1e-3f && std::fabs(yaw.y - 0.70710677f) < 1e-3f &&
                  std::fabs(yaw.x) < 1e-6f && std::fabs(yaw.z) < 1e-6f,
              "REUL(0, 90, 0) turns about Y");
    CHECK_MSG(NearVector(AngleCone(identity, 90.f, 90.f, .5f, .5f, 2.f), CVector3f(0.f, 0.f, 2.f), 1e-4f),
              "a centred ANCR draw points down +Z at the magnitude");
    CHECK_MSG(NearVector(AngleCone(yaw, 90.f, 90.f, .5f, .5f, 1.f), CVector3f(1.f, 0.f, 0.f), 1e-3f),
              "a REUL(0, 90, 0) cone points down +X");
    // u = 0 is the +range/2 edge: a = 45 degrees gives (0, -sin a, cos a).
    CHECK_MSG(NearVector(AngleCone(identity, 90.f, 0.f, 0.f, .5f, 1.f), CVector3f(0.f, -0.70710677f, 0.70710677f), 1e-2f),
              "ANCR's x range tilts toward -Y");
    // RADD (CQuaternion::Multiply 0xd43d8): two quarter turns about Y make a half turn, and
    // i * j = k.
    const Quat half = QuatMultiply(yaw, yaw);
    CHECK_MSG(std::fabs(half.w) < 1e-3f && std::fabs(half.y - 1.f) < 1e-3f, "RADD composes two yaws");
    const Quat k = QuatMultiply(Quat{0.f, 1.f, 0.f, 0.f}, Quat{0.f, 0.f, 1.f, 0.f});
    CHECK_MSG(k.w == 0.f && k.x == 0.f && k.y == 0.f && k.z == 1.f, "RADD is the Hamilton product a * b");
    const CVector3f sphere = AngleSphereDirection(identity, 90.f, 0.f, 0.f, .5f);
    const float length = std::sqrt(sphere.GetX() * sphere.GetX() + sphere.GetY() * sphere.GetY() +
                                   sphere.GetZ() * sphere.GetZ());
    CHECK_MSG(std::fabs(length - 1.f) < 1e-5f, "ASPR's direction is normalised");
    const CVector3f zero = NormalizeInPlace(CVector3f(0.f, 0.f, 0.f));
    CHECK_MSG(zero.GetX() == 2.f && zero.GetY() == 2.f && zero.GetZ() == 2.f,
              "Normalize 0x6b0b0 takes zero to (2, 2, 2)");
  }

  if (port_test::Failures() == 0) {
    std::puts("port_particle_math: OK");
  }
  return PORT_TEST_RESULT();
}
