// Port vector elements from Remastered: MPAC (CVEPortMpac), MPCN (CVEPortCone), ANCM
// (CVEPortAngleConeMinMax) and ANCV (CVEPortAngleConeMinMaxVariance), against independent double-precision values for a fixed seed. The
// parabolic sine and FRSQRTE steps differ from std:: by a few 1e-3, so the angle-based ones use
// that tolerance; the MPCN frame is exact for an axis-aligned direction.
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdint>

namespace {

class ConstReal final : public CRealElement {
public:
  explicit ConstReal(float v) : mValue(v) {}
  bool GetValue(int, float& out) const override {
    out = mValue;
    return false;
  }

private:
  float mValue;
};

class ConstVector final : public CVectorElement {
public:
  explicit ConstVector(CVector3f v) : mValue(v) {}
  bool GetValue(int, CVector3f& out) const override {
    out = mValue;
    return false;
  }

private:
  CVector3f mValue;
};

// The LCG behind CRandom16::PortFloat28, kept apart from the code under test.
struct Lcg {
  uint32_t seed;
  double next() {
    seed = seed * 0x41c64e6du + 0x3039u;
    return double(seed & 0xfffffffu) / 268435456.0;
  }
};

bool Near(double a, double b, double epsilon) { return std::fabs(a - b) <= epsilon; }

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

void TestMpac() {
  CRandom16 rnd(4242);
  CGlobalRandom global(rnd);
  const CVEPortMpac element(new ConstReal(0.f), new ConstReal(30.f), new ConstReal(720.f), new ConstReal(90.f),
                            new ConstReal(2.f));
  Lcg lcg{4242};
  for (int i = 0; i < 20; ++i) {
    CVector3f out;
    CHECK(!element.GetValue(i, out));
    const double u = lcg.next();
    const double v = lcg.next();
    const double ax = (360.0 - 720.0 * u) * kDeg;
    const double ay = (30.0 + 45.0 - 90.0 * v) * kDeg;
    CHECK_MSG(Near(out.GetX(), 2.0 * std::cos(ax) * -std::sin(ay), 6.e-3), "MPAC x = mag cos(ax) -sin(ay)");
    CHECK_MSG(Near(out.GetY(), 2.0 * std::sin(ax), 6.e-3), "MPAC y = mag sin(ax)");
    CHECK_MSG(Near(out.GetZ(), 2.0 * std::cos(ay) * std::cos(ax), 6.e-3), "MPAC z = mag cos(ay) cos(ax)");
  }
  CHECK_MSG(rnd.GetSeed() == lcg.seed, "MPAC draws two numbers per evaluation");
}

void TestCone() {
  CRandom16 rnd(99);
  CGlobalRandom global(rnd);
  // Direction +y: n.x <= 0.8, u = (0, 0, -1), v = n x u = (-1, 0, 0). Radius 0.5 never rejects
  // (x^2 + y^2 <= 0.5), so each evaluation draws exactly two numbers: X = a - 0.5, Y = b - 0.5.
  const CVEPortCone element(new ConstVector(CVector3f(0.f, 1.f, 0.f)), new ConstReal(0.5f));
  Lcg lcg{99};
  for (int frame = 0; frame < 12; ++frame) {
    CVector3f out;
    CHECK(!element.GetValue(frame, out));
    const double x = lcg.next() - 0.5;
    const double y = lcg.next() - 0.5;
    CHECK_MSG(Near(out.GetX(), -y, 1.e-6) && Near(out.GetY(), 1.0, 1.e-6) && Near(out.GetZ(), -x, 1.e-6),
              "MPCN is the centre plus a disc point in the frame (u, v)");
  }
  CHECK_MSG(rnd.GetSeed() == lcg.seed, "MPCN draws two numbers per accepted point");

  // A radius above 1 is clamped to 1, and the rejection loop keeps points inside the unit disc.
  CVEPortCone wide(new ConstVector(CVector3f(1.f, 0.f, 0.f)), new ConstReal(5.f));
  for (int frame = 0; frame < 50; ++frame) {
    CVector3f out;
    wide.GetValue(frame, out);
    const double dy = out.GetY();
    const double dz = out.GetZ();
    CHECK_MSG(out.GetX() == 1.f && dy * dy + dz * dz <= 1.0 + 1.e-6, "MPCN stays in the unit disc");
  }

  // The frame is cached by the first call at frame 0: later calls with a changed direction use it.
  CVEPortCone cached(new ConstVector(CVector3f(0.f, 1.f, 0.f)), new ConstReal(0.5f));
  CVector3f first;
  cached.GetValue(0, first);
  CHECK_MSG(Near(first.GetY(), 1.0, 1.e-6), "cached cone centre");
}

void TestAngleCone() {
  CRandom16 rnd(2024);
  CGlobalRandom global(rnd);
  // The rotation is the identity, so the direction is (s cos phi, s sin phi, c) times the magnitude.
  const CVEPortAngleConeMinMax element(new ConstReal(0.f), new ConstReal(0.f), new ConstReal(0.f),
                                       new ConstReal(10.f), new ConstReal(20.f), new ConstReal(2.f));
  Lcg lcg{2024};
  for (int i = 0; i < 20; ++i) {
    CVector3f out;
    CHECK(!element.GetValue(i, out));
    const double u0 = lcg.next();
    const double u1 = lcg.next();
    const double cA = std::cos(10.0 * kDeg);
    const double cB = std::cos(20.0 * kDeg);
    const double c = cA + (cB - cA) * u0;
    const double s = std::sqrt(1.0 - c * c);
    const double phi = u1 * 2.0 * kPi - kPi;
    CHECK_MSG(Near(out.GetX(), 2.0 * s * std::cos(phi), 8.e-3), "ANCM x");
    CHECK_MSG(Near(out.GetY(), 2.0 * s * std::sin(phi), 8.e-3), "ANCM y");
    CHECK_MSG(Near(out.GetZ(), 2.0 * c, 8.e-3), "ANCM z is mag times the cone cosine");
  }
  CHECK_MSG(rnd.GetSeed() == lcg.seed, "ANCM draws two numbers per evaluation");

  // Turned 90 degrees about X (Euler order 0), +z goes to -y.
  const CVEPortAngleConeMinMax turned(new ConstReal(90.f), new ConstReal(0.f), new ConstReal(0.f),
                                      new ConstReal(0.f), new ConstReal(0.f), new ConstReal(1.f));
  CVector3f out;
  turned.GetValue(0, out);
  CHECK_MSG(Near(out.GetX(), 0.0, 8.e-3) && Near(std::fabs(out.GetY()), 1.0, 8.e-3) && Near(out.GetZ(), 0.0, 8.e-3),
            "ANCM turns the cone axis by the rotation");
}

class ConstInt final : public CIntElement {
public:
  explicit ConstInt(int v) : mValue(v) {}
  bool GetValue(int, int& out) const override {
    out = mValue;
    return false;
  }

private:
  int mValue;
};

void TestAngleConeVariance() {
  CRandom16 rnd(77);
  CGlobalRandom global(rnd);
  // Outside a generator the index is 0: polar band 0 of 2 (between cos 10 and the mid cosine) and
  // azimuth sector 0 of 4 (-pi .. -pi/2), each jittered by its variance times half its width.
  const CVEPortAngleConeMinMaxVariance element(new ConstReal(0.f), new ConstReal(0.f), new ConstReal(0.f),
                                               new ConstReal(10.f), new ConstReal(20.f), new ConstReal(2.f),
                                               new ConstInt(2), new ConstReal(0.5f), new ConstInt(4),
                                               new ConstReal(1.f), 0);
  Lcg lcg{77};
  for (int i = 0; i < 20; ++i) {
    CVector3f out;
    CHECK(!element.GetValue(i, out));
    const double u0 = lcg.next();
    const double u1 = lcg.next();
    const double cA = std::cos(10.0 * kDeg);
    const double cB = std::cos(20.0 * kDeg);
    const double d = (cB - cA) / 2.0;
    const double c = cA + d * 0.5 + 0.5 * d * 0.5 * (2.0 * u0 - 1.0);
    const double sector = 2.0 * kPi / 4.0;
    const double phi = sector * 0.5 + sector * 0.5 * (2.0 * u1 - 1.0) - kPi;
    const double s = std::sqrt(1.0 - c * c);
    CHECK_MSG(Near(out.GetX(), 2.0 * s * std::cos(phi), 8.e-3), "ANCV x");
    CHECK_MSG(Near(out.GetY(), 2.0 * s * std::sin(phi), 8.e-3), "ANCV y");
    CHECK_MSG(Near(out.GetZ(), 2.0 * c, 8.e-3), "ANCV z is mag times the band's cosine");
  }
  CHECK_MSG(rnd.GetSeed() == lcg.seed, "ANCV draws two numbers per evaluation");

  // Counts of 0 are 1: the whole cone and turn, as ANCM's (with the variances at 1).
  const CVEPortAngleConeMinMaxVariance whole(new ConstReal(0.f), new ConstReal(0.f), new ConstReal(0.f),
                                             new ConstReal(10.f), new ConstReal(20.f), new ConstReal(1.f),
                                             new ConstInt(0), new ConstReal(1.f), new ConstInt(0),
                                             new ConstReal(1.f), 0);
  const CVEPortAngleConeMinMax ancm(new ConstReal(0.f), new ConstReal(0.f), new ConstReal(0.f), new ConstReal(10.f),
                                    new ConstReal(20.f), new ConstReal(1.f));
  CRandom16 a(5);
  CRandom16 b(5);
  for (int i = 0; i < 10; ++i) {
    CVector3f va;
    CVector3f vb;
    {
      CGlobalRandom ga(a);
      whole.GetValue(i, va);
    }
    {
      CGlobalRandom gb(b);
      ancm.GetValue(i, vb);
    }
    CHECK_MSG(Near(va.GetX(), vb.GetX(), 1.e-5) && Near(va.GetY(), vb.GetY(), 1.e-5) &&
                  Near(va.GetZ(), vb.GetZ(), 1.e-5),
              "ANCV with counts 0 and variances 1 is ANCM");
  }
}

} // namespace

// Heap storage for elements.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }

// PEMV outside a generator evaluation is zero.
void TestEmitterVelocity() {
  CVEPortEmitterVelocity element;
  CVector3f out(1.f, 2.f, 3.f);
  CHECK(!element.GetValue(0, out));
  CHECK(out.GetX() == 0.f && out.GetY() == 0.f && out.GetZ() == 0.f);
}

int main() {
  TestMpac();
  TestCone();
  TestAngleCone();
  TestAngleConeVariance();
  TestEmitterVelocity();
  return PORT_TEST_RESULT();
}
