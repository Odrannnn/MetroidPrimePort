// Port curl noise (CRLN): Remastered's Perlin noise, CurlNoise3d, the VECF vector field and the
// PCRM mod-vector arithmetic. Expected noise values come from an independent double-precision
// transcription of the reference algorithm.
#include "Kyoto/Particles/CPortCurlNoise.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

bool Near(float a, float b, float epsilon = 1.e-5f) { return std::fabs(a - b) <= epsilon; }

class TestVector final : public CVectorElement {
public:
  explicit TestVector(CVector3f v) : mValue(v) {}
  bool GetValue(int, CVector3f& out) const override {
    out = mValue;
    return false;
  }

private:
  CVector3f mValue;
};

class TestReal final : public CRealElement {
public:
  explicit TestReal(float v) : mValue(v) {}
  bool GetValue(int, float& out) const override {
    out = mValue;
    return false;
  }

private:
  float mValue;
};

void TestNoise() {
  CHECK(Near(PortCurlNoise::PerlinNoise(0.5f, 0.5f, 0.5f, 256), -0.25f));
  CHECK(Near(PortCurlNoise::PerlinNoise(1.25f, 2.5f, -0.75f, 256), 1.882032871f));
  CHECK(Near(PortCurlNoise::PerlinNoise(3.3f, 0.7f, 9.1f, 256), 0.386599429f));
  // Lattice points are zero, and the period wraps the lattice.
  CHECK(Near(PortCurlNoise::PerlinNoise(3.f, 4.f, 5.f, 256), 0.f));
  CHECK(Near(PortCurlNoise::PerlinNoise(0.5f, 0.5f, 0.5f, 256),
             PortCurlNoise::PerlinNoise(256.5f, 0.5f, 512.5f, 256)));
}

void TestCurl() {
  const CVector3f c = PortCurlNoise::CurlNoise3d(0.3f, 0.6f, 0.9f, 1.e-2f, 256);
  CHECK(Near(c.GetX(), 0.575285295f, 2.e-3f));
  CHECK(Near(c.GetY(), -0.778021286f, 2.e-3f));
  CHECK(Near(c.GetZ(), 0.202735991f, 2.e-3f));
}

// CurlNoise2dSliceXY/XZ/YZ: (db, -da) of the noise on the plane, the third coordinate 0.
void TestSlice() {
  const float e = 1.e-2f;
  const float s = 1.f / (e + e);
  const auto n = [](float x, float y, float z) { return PortCurlNoise::PerlinNoise(x, y, z, 256); };
  float u, v;
  PortCurlNoise::CurlNoise2dSlice(2, 0.3f, 0.9f, e, 256, u, v);
  CHECK(Near(u, s * (n(0.3f, 0.f, 0.9f + e) - n(0.3f, 0.f, 0.9f - e))));
  CHECK(Near(v, s * -(n(0.3f + e, 0.f, 0.9f) - n(0.3f - e, 0.f, 0.9f))));
  PortCurlNoise::CurlNoise2dSlice(1, 0.3f, 0.6f, e, 256, u, v);
  CHECK(Near(u, s * (n(0.3f, 0.6f + e, 0.f) - n(0.3f, 0.6f - e, 0.f))));
  PortCurlNoise::CurlNoise2dSlice(3, 0.6f, 0.9f, e, 256, u, v);
  CHECK(Near(v, s * -(n(0.f, 0.6f + e, 0.9f) - n(0.f, 0.6f - e, 0.9f))));
  CHECK(!Near(u, 0.f) || !Near(v, 0.f));

  // PCRS: P*F's slice axes in, A scales the result's, the third axis 0.
  CVEPortCurlNoiseSlice xz(2, new TestVector(CVector3f(0.15f, 0.f, 0.45f)), new TestReal(2.f),
                           new TestVector(CVector3f(3.f, 5.f, 7.f)));
  CVector3f out;
  xz.GetValue(0, out);
  PortCurlNoise::CurlNoise2dSlice(2, 0.3f, 0.9f, 1.e-5f, 256, u, v);
  CHECK(Near(out.GetX(), 3.f * u) && out.GetY() == 0.f && Near(out.GetZ(), 7.f * v));
}

// A 2x2x2 field whose cell value is its index y * (X * Z) + z * X + x in every component.
CVectorField MakeField() {
  std::vector< CVector3f > cells;
  for (int i = 0; i < 8; ++i) {
    cells.emplace_back(float(i), float(10 * i), float(100 * i));
  }
  return CVectorField(2, 2, 2, CVector3f(0.f, 0.f, 0.f), CVector3f(1.f, 1.f, 1.f), cells);
}

void TestField() {
  const CVectorField f = MakeField();
  CVector3f v = f.Compute(CVector3f(0.25f, 0.5f, 0.75f)); // 4 * fy + 2 * fz + fx
  CHECK(Near(v.GetX(), 3.75f));
  CHECK(Near(v.GetY(), 37.5f));
  CHECK(Near(v.GetZ(), 375.f));
  v = f.Compute(CVector3f(1.25f, 0.5f, 0.75f)); // wraps: cell 1 weighs 0.75, cell 0 0.25
  CHECK(Near(v.GetX(), 4.25f));
  v = f.Compute(CVector3f(-0.25f, 0.5f, 0.75f)); // negative fraction is kept
  CHECK(Near(v.GetX(), 3.25f));
  CHECK(CVectorField().Compute(CVector3f(1.f, 2.f, 3.f)).GetX() == 0.f);
}

void PutU32(std::vector< unsigned char >& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast< unsigned char >(v >> s));
  }
}

void PutF32(std::vector< unsigned char >& b, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutU32(b, v);
}

std::vector< unsigned char > MakeBlob(uint32_t magic, uint32_t version, uint32_t count) {
  std::vector< unsigned char > b;
  PutU32(b, magic);
  PutU32(b, version);
  for (int i = 0; i < 3; ++i) {
    PutU32(b, 2);
  }
  for (float f : {0.f, 0.f, 0.f, 1.f, 1.f, 1.f}) {
    PutF32(b, f);
  }
  PutU32(b, count);
  for (uint32_t i = 0; i < count; ++i) {
    PutF32(b, float(i));
    PutF32(b, float(10 * i));
    PutF32(b, float(100 * i));
  }
  return b;
}

void TestParse() {
  const uint32_t kVecf = 'V' << 24 | 'E' << 16 | 'C' << 8 | 'F';
  {
    auto b = MakeBlob(kVecf, 1, 8);
    CMemoryInStream in(b.data(), b.size());
    CVectorField f;
    REQUIRE(f.Parse(in));
    CHECK(Near(f.Compute(CVector3f(0.25f, 0.5f, 0.75f)).GetX(), 3.75f));
  }
  for (auto b : {MakeBlob(kVecf + 1, 1, 8), MakeBlob(kVecf, 2, 8), MakeBlob(kVecf, 1, 7)}) {
    CMemoryInStream in(b.data(), b.size());
    CVectorField f;
    CHECK(!f.Parse(in));
    CHECK(f.IsEmpty());
  }
}

void TestMod() {
  const CVector3f v0(10.f, 20.f, 30.f);
  const CVector3f vel(1.f, 2.f, 3.f);
  {
    CMVEPortCurlNoise m(0, new TestVector(v0), new TestReal(0.25f));
    CVector3f vl = vel, pos(100.f, 200.f, 300.f);
    CHECK(!m.GetValue(0, vl, pos));
    CHECK(Near(pos.GetX(), 110.f) && Near(pos.GetY(), 220.f) && Near(pos.GetZ(), 330.f));
    CHECK(Near(vl.GetX(), 1.f));
    CHECK(!m.PortFinalizesPosition());
  }
  {
    CMVEPortCurlNoise m(1, new TestVector(v0), new TestReal(0.25f));
    CVector3f vl = vel, pos(100.f, 200.f, 300.f);
    m.GetValue(0, vl, pos); // 0.75 * vel + 0.25 * v + pos
    CHECK(Near(pos.GetX(), 0.75f + 2.5f + 100.f));
    CHECK(Near(pos.GetY(), 1.5f + 5.f + 200.f));
    CHECK(Near(pos.GetZ(), 2.25f + 7.5f + 300.f));
    CHECK(m.PortFinalizesPosition());
  }
  {
    CMVEPortCurlNoise m(2, new TestVector(v0), new TestReal(0.25f));
    CVector3f vl = vel, pos(100.f, 200.f, 300.f);
    m.GetValue(0, vl, pos);
    CHECK(Near(pos.GetX(), 100.f) && Near(pos.GetY(), 200.f) && Near(pos.GetZ(), 300.f));
    CHECK(!m.PortFinalizesPosition());
  }
}

} // namespace

// Heap storage for elements; the production field resolution needs the resource pool, which the
// test bypasses by installing the field with SetField.
void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
const CVectorField* CVEPortCurlNoise::ResolveField() const { return mField; }

int main() {
  TestNoise();
  TestCurl();
  TestSlice();
  TestField();
  TestParse();
  TestMod();
  return PORT_TEST_RESULT();
}
