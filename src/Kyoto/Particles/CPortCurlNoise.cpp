#ifdef TARGET_PC
#include "Kyoto/Particles/CPortCurlNoise.hpp"

#include <cmath>
#include <cstdio>

#include "Kyoto/Streams/CInputStream.hpp"

namespace {
// Ken Perlin's reference permutation (Remastered DAT_01d0c000, doubled by the lookup below).
constexpr unsigned char kPerm[256] = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,
    103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,
    0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149,
    56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166,
    77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,
    245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187,
    208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186,
    3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212,
    207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248,
    152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
    19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,
    242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107,
    49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,
    150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,
    215, 61,  156, 180};

inline int Perm(int i) { return kPerm[i & 255]; }

inline float Fade(float t) { return t * t * t * (t * (t * 6.0f + -15.0f) + 10.0f); }

inline float Lerp(float t, float a, float b) { return a + t * (b - a); }

inline float Grad(int h, float x, float y, float z) {
  h &= 15;
  const float u = h < 8 ? x : y;
  const float v = h < 4 ? y : ((h | 2) == 14 ? x : z);
  return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

inline int Truncate(float s) {
  if (!(std::fabs(s) < 2.0e9f)) {
    return s > 0.f ? 2000000000 : (s < 0.f ? -2000000000 : 0);
  }
  return int(s);
}

// ComputeFieldVector's per-axis step: the cell, its wrapped neighbour and the fraction.
struct Axis {
  int c0;
  int c1;
  float f;
};

inline Axis FieldAxis(float p, float scale, int dim) {
  const float s = p * scale;
  const int i = Truncate(s);
  int c0 = i % dim;
  if (c0 < 0) {
    c0 += dim;
  }
  return {c0, (c0 + 1) % dim, s - float(i)};
}
} // namespace

namespace PortCurlNoise {
float PerlinNoise(float x, float y, float z, int period) {
  const int m = period - 1;
  const int ix = Truncate(x);
  const int iy = Truncate(y);
  const int iz = Truncate(z);
  const float fx = x - float(ix);
  const float fy = y - float(iy);
  const float fz = z - float(iz);
  const int x0 = ix & m;
  const int x1 = (ix + 1) & m;
  const int y0 = iy & m;
  const int y1 = (iy + 1) & m;
  const int z0 = iz & m;
  const int z1 = (iz + 1) & m;
  auto hash = [](int a, int b, int c) { return Perm(Perm(Perm(a) + b) + c); };
  const float u = Fade(fx);
  const float v = Fade(fy);
  const float w = Fade(fz);
  const float a0 = Lerp(v,
                        Lerp(u, Grad(hash(x0, y0, z0), fx, fy, fz),
                             Grad(hash(x1, y0, z0), fx - 1, fy, fz)),
                        Lerp(u, Grad(hash(x0, y1, z0), fx, fy - 1, fz),
                             Grad(hash(x1, y1, z0), fx - 1, fy - 1, fz)));
  const float a1 = Lerp(v,
                        Lerp(u, Grad(hash(x0, y0, z1), fx, fy, fz - 1),
                             Grad(hash(x1, y0, z1), fx - 1, fy, fz - 1)),
                        Lerp(u, Grad(hash(x0, y1, z1), fx, fy - 1, fz - 1),
                             Grad(hash(x1, y1, z1), fx - 1, fy - 1, fz - 1)));
  return Lerp(w, a0, a1);
}

CVector3f CurlNoise3d(float x, float y, float z, float e, int period) {
  const float s = 1.0f / (e + e);
  const float dx = s * (PerlinNoise(x + e, y, z, period) - PerlinNoise(x - e, y, z, period));
  const float dy = s * (PerlinNoise(x, y + e, z, period) - PerlinNoise(x, y - e, z, period));
  const float dz = s * (PerlinNoise(x, y, z + e, period) - PerlinNoise(x, y, z - e, period));
  return CVector3f(dy - dz, dz - dx, dx - dy);
}

void CurlNoise2dSlice(int slice, float a, float b, float e, int period, float& outA, float& outB) {
  const auto noise = [&](float u, float v) {
    switch (slice) {
    case 1:
      return PerlinNoise(u, v, 0.f, period);
    case 2:
      return PerlinNoise(u, 0.f, v, period);
    default:
      return PerlinNoise(0.f, u, v, period);
    }
  };
  const float s = 1.0f / (e + e);
  const float da = noise(a + e, b) - noise(a - e, b);
  const float db = noise(a, b + e) - noise(a, b - e);
  outA = s * db;
  outB = s * -da;
}

namespace {
// One probe of the octave sum: amp0 = 1, f0 = freq, f(k+1) = fk + fk, amp(k+1) = amp(k) * gain,
// summed in order. The offset is already in the coordinates, ahead of the frequency.
float OctaveSum(int octaves, float x, float y, float z, float freq, float gain) {
  float sum = 0.f;
  float f = freq;
  float amp = 1.f;
  for (int k = 0; k < octaves; ++k) {
    const float fx = x * f;
    const float fy = f * y;
    const float fz = f * z;
    const float noise = PerlinNoise(fx, fy, fz, 256);
    const float term = amp * noise;
    sum = sum + term;
    f = f + f;
    amp = amp * gain;
  }
  return sum;
}
} // namespace

CVector3f CurlNoiseOctave3d(int octaves, float x, float y, float z, float freq, float gain,
                            float e) {
  const float s = 1.0f / (e + e);
  const float dx = s * (OctaveSum(octaves, x + e, y, z, freq, gain) -
                        OctaveSum(octaves, x - e, y, z, freq, gain));
  const float dy = s * (OctaveSum(octaves, x, y + e, z, freq, gain) -
                        OctaveSum(octaves, x, y - e, z, freq, gain));
  const float dz = s * (OctaveSum(octaves, x, y, z + e, freq, gain) -
                        OctaveSum(octaves, x, y, z - e, freq, gain));
  return CVector3f(dy - dz, dz - dx, dx - dy);
}
} // namespace PortCurlNoise

CVectorField::CVectorField(int dimX, int dimY, int dimZ, const CVector3f& min,
                           const CVector3f& max, std::vector< CVector3f > cells)
: xc_min(min), x18_max(max), x1c_cells(std::move(cells)) {
  x0_dim[0] = dimX;
  x0_dim[1] = dimY;
  x0_dim[2] = dimZ;
}

bool CVectorField::Parse(CInputStream& in) {
  *this = CVectorField();
  if (in.ReadLong() != 'VECF' || in.ReadLong() != 1) {
    return false;
  }
  int dim[3];
  for (int& d : dim) {
    d = int(in.ReadLong());
  }
  const float minx = in.ReadFloat(), miny = in.ReadFloat(), minz = in.ReadFloat();
  const float maxx = in.ReadFloat(), maxy = in.ReadFloat(), maxz = in.ReadFloat();
  const uint count = in.ReadLong();
  if (dim[0] <= 0 || dim[1] <= 0 || dim[2] <= 0 || dim[0] > 4096 || dim[1] > 4096 ||
      dim[2] > 4096 ||
      uint64_t(dim[0]) * uint64_t(dim[1]) * uint64_t(dim[2]) != uint64_t(count) ||
      count > (1u << 24)) {
    return false;
  }
  std::vector< CVector3f > cells;
  cells.reserve(count);
  for (uint i = 0; i < count; ++i) {
    const float x = in.ReadFloat(), y = in.ReadFloat(), z = in.ReadFloat();
    cells.emplace_back(x, y, z);
  }
  *this = CVectorField(dim[0], dim[1], dim[2], CVector3f(minx, miny, minz),
                       CVector3f(maxx, maxy, maxz), std::move(cells));
  return true;
}

CVector3f CVectorField::Compute(const CVector3f& p) const {
  if (x1c_cells.empty()) {
    return CVector3f::Zero();
  }
  const float pc[3] = {p.GetX(), p.GetY(), p.GetZ()};
  const float mn[3] = {xc_min.GetX(), xc_min.GetY(), xc_min.GetZ()};
  const float mx[3] = {x18_max.GetX(), x18_max.GetY(), x18_max.GetZ()};
  Axis ax[3];
  for (int c = 0; c < 3; ++c) {
    ax[c] = FieldAxis(pc[c], float(x0_dim[c] - 1) / (mx[c] - mn[c]), x0_dim[c]);
  }
  const int dx = x0_dim[0];
  const int dz = x0_dim[2];
  auto cell = [&](int x, int y, int z) -> const CVector3f& {
    return x1c_cells[size_t(y) * size_t(dx * dz) + size_t(z) * size_t(dx) + size_t(x)];
  };
  const float fx = ax[0].f, fy = ax[1].f, fz = ax[2].f;
  float out[3];
  for (int c = 0; c < 3; ++c) {
    auto comp = [&](int x, int y, int z) {
      const CVector3f& v = cell(x, y, z);
      return c == 0 ? v.GetX() : (c == 1 ? v.GetY() : v.GetZ());
    };
    auto yb = [&](int x, int z) {
      return comp(x, ax[1].c0, z) * (1.0f - fy) + comp(x, ax[1].c1, z) * fy;
    };
    const float a00 = yb(ax[0].c0, ax[2].c0);
    const float a10 = yb(ax[0].c1, ax[2].c0);
    const float a01 = yb(ax[0].c0, ax[2].c1);
    const float a11 = yb(ax[0].c1, ax[2].c1);
    const float b0 = a00 * (1.0f - fz) + a01 * fz;
    const float b1 = a10 * (1.0f - fz) + a11 * fz;
    out[c] = b0 * (1.0f - fx) + b1 * fx;
  }
  return CVector3f(out[0], out[1], out[2]);
}

CVEPortCurlNoise::CVEPortCurlNoise(CVectorElement* p, CRealElement* f, CVectorElement* a,
                                   FieldHandle* handle)
: mP(p), mF(f), mA(a), mHandle(handle) {}

bool CVEPortCurlNoise::GetValue(int frame, CVector3f& valOut) const {
  CVector3f p = CVector3f::Zero();
  if (mP) {
    mP->GetValue(frame, p);
  }
  CVector3f a = CVector3f::Zero();
  if (mA) {
    mA->GetValue(frame, a);
  }
  CVector3f n = CVector3f::Zero();
  if (const CVectorField* field = ResolveField()) {
    n = field->Compute(p);
  } else if (mHandle == nullptr) {
    float f = 1.0f;
    if (mF) {
      mF->GetValue(frame, f);
    }
    n = PortCurlNoise::CurlNoise3d(p.GetX() * f, p.GetY() * f, p.GetZ() * f, 1.0e-5f, 256);
  }
  valOut = CVector3f(n.GetX() * a.GetX(), n.GetY() * a.GetY(), n.GetZ() * a.GetZ());
  return false;
}

CVEPortCurlNoiseOctave::CVEPortCurlNoiseOctave(CVectorElement* p, CRealElement* freq,
                                               CIntElement* octaves, CRealElement* gain,
                                               CVectorElement* a)
: mP(p), mFreq(freq), mOctaves(octaves), mGain(gain), mA(a) {}

CVEPortCurlNoiseOctave::~CVEPortCurlNoiseOctave() {
  delete mP;
  delete mFreq;
  delete mOctaves;
  delete mGain;
  delete mA;
}

bool CVEPortCurlNoiseOctave::GetValue(int frame, CVector3f& valOut) const {
  // The original's order: P, frequency, octaves, gain, amplitude.
  CVector3f p = CVector3f::Zero();
  if (mP) {
    mP->GetValue(frame, p);
  }
  float freq = 1.0f;
  if (mFreq) {
    mFreq->GetValue(frame, freq);
  }
  int octaves = 1;
  if (mOctaves) {
    mOctaves->GetValue(frame, octaves);
  }
  float gain = 1.0f;
  if (mGain) {
    mGain->GetValue(frame, gain);
  }
  CVector3f a = CVector3f::Zero();
  if (mA) {
    mA->GetValue(frame, a);
  }
  const CVector3f n = PortCurlNoise::CurlNoiseOctave3d(octaves, p.GetX(), p.GetY(), p.GetZ(),
                                                       freq, gain, 1.0e-5f);
  valOut = CVector3f(a.GetX() * n.GetX(), a.GetY() * n.GetY(), a.GetZ() * n.GetZ());
  return false;
}

CVEPortCurlNoiseSlice::CVEPortCurlNoiseSlice(uint slice, CVectorElement* p, CRealElement* f,
                                             CVectorElement* a)
: mSlice(slice), mP(p), mF(f), mA(a) {}

CVEPortCurlNoiseSlice::~CVEPortCurlNoiseSlice() {
  delete mP;
  delete mF;
  delete mA;
}

bool CVEPortCurlNoiseSlice::GetValue(int frame, CVector3f& valOut) const {
  // The original's order: P, F, A.
  CVector3f p = CVector3f::Zero();
  if (mP) {
    mP->GetValue(frame, p);
  }
  float f = 1.0f;
  if (mF) {
    mF->GetValue(frame, f);
  }
  CVector3f a = CVector3f::Zero();
  if (mA) {
    mA->GetValue(frame, a);
  }
  float u = 0.f;
  float v = 0.f;
  switch (mSlice) {
  case 1:
    PortCurlNoise::CurlNoise2dSlice(1, p.GetX() * f, f * p.GetY(), 1.0e-5f, 256, u, v);
    valOut = CVector3f(u * a.GetX(), v * a.GetY(), 0.f);
    break;
  case 2:
    PortCurlNoise::CurlNoise2dSlice(2, p.GetX() * f, f * p.GetZ(), 1.0e-5f, 256, u, v);
    valOut = CVector3f(u * a.GetX(), 0.f, v * a.GetZ());
    break;
  default:
    PortCurlNoise::CurlNoise2dSlice(3, p.GetY() * f, f * p.GetZ(), 1.0e-5f, 256, u, v);
    valOut = CVector3f(0.f, u * a.GetY(), v * a.GetZ());
    break;
  }
  return false;
}

CMVEPortCurlNoise::CMVEPortCurlNoise(uint mode, CVectorElement* v, CRealElement* t)
: mMode(mode), mV(v), mT(t) {}

CMVEPortCurlNoise::~CMVEPortCurlNoise() {
  delete mV;
  delete mT;
}

bool CMVEPortCurlNoise::GetValue(int frame, CVector3f& pVel, CVector3f& pPos) const {
  CVector3f v = CVector3f::Zero();
  if (mV) {
    mV->GetValue(frame, v);
  }
  if (mMode == 1) {
    float t = 1.0f;
    if (mT) {
      mT->GetValue(frame, t);
    }
    pPos = CVector3f((1.0f - t) * pVel.GetX() + t * v.GetX() + pPos.GetX(),
                     (1.0f - t) * pVel.GetY() + t * v.GetY() + pPos.GetY(),
                     (1.0f - t) * pVel.GetZ() + t * v.GetZ() + pPos.GetZ());
  } else if (mMode == 0) {
    pPos = CVector3f(pPos.GetX() + v.GetX(), pPos.GetY() + v.GetY(), pPos.GetZ() + v.GetZ());
  }
  return false;
}
#endif // TARGET_PC
