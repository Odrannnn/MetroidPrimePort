#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Remastered CParticleElectric2 bolt generation (ELC2), pure math so it can be unit tested.
// Addresses are the Remastered (Switch) binary: CalculatePointsForBolt 0xb73b18,
// CalculateFractal 0xb74e3c, CalculatePointsForBoltBranches 0xb745b0.
namespace PortElectric2 {
struct V3 {
  float x = 0.f, y = 0.f, z = 0.f;
};
inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool IsNormalizable(const V3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         Dot(v, v) >= 0x1p-46f;
}
inline V3 Normalized(const V3& v) { return v * (1.f / std::sqrt(Dot(v, v))); }

// The source's CVector3f basis vectors (main.raw): skUp=(0,1,0), skForward=(0,0,1),
// skBack=(0,0,-1). SIDE 2 uses camera forward as the reference for the ribbon frame.
inline constexpr V3 kElectric2Up{0.f, 1.f, 0.f};
inline constexpr V3 kElectric2Forward{0.f, 0.f, 1.f};
inline constexpr V3 kElectric2Back{0.f, 0.f, -1.f};

// InitBoltNodeOrientation 0xb76f98. NFOM=0 uses one endpoint tangent for every node;
// NFOM=1 uses the next segment's tangent, with the final node inheriting the previous one.
// SIDE=2's first basis row is camera-forward x tangent. If that is degenerate, the source
// retries with skUpVector; other SIDE values use skUpVector and retry with skBackVector.
// CrossIntoMatrixRow 0x286d70 rejects squared magnitudes below 2^-46.
inline bool Electric2RibbonWidthAxis(const std::vector<V3>& line, size_t node, int nfom, int side,
                                     const V3& cameraForward, V3& width) {
  if (line.size() < 2 || node >= line.size()) {
    return false;
  }

  V3 tangent;
  if (nfom == 1 && node + 1 < line.size()) {
    tangent = line[node + 1] - line[node];
  } else if (nfom == 1) {
    tangent = line[node] - line[node - 1];
  } else {
    tangent = line.back() - line.front();
  }
  if (!IsNormalizable(tangent)) {
    tangent = kElectric2Forward;
  } else {
    tangent = Normalized(tangent);
  }

  const bool cameraFacing = side == 2;
  const V3 reference = cameraFacing ? cameraForward : kElectric2Up;
  width = Cross(reference, tangent);
  if (!IsNormalizable(width)) {
    width = Cross(cameraFacing ? kElectric2Up : kElectric2Back, tangent);
    if (!IsNormalizable(width)) {
      return false;
    }
  }
  width = Normalized(width);
  return true;
}

// SIDE=2 samples theta 0 (RRAD, positive local X) and theta 180 (LRAD, negative local X).
inline void Electric2RibbonSidePoints(const V3& center, const V3& width, float leftRad, float rightRad,
                                     V3& theta0, V3& theta180) {
  theta0 = center + width * rightRad;
  theta180 = center - width * leftRad;
}

// The half-widths one node is drawn with (UpdateBolt 0xb77638, once per node): node+0x28 = LRAD;
// if LRAD is present and LLRD is set it is copied to node+0x2c; then RRAD, if present, overwrites
// node+0x2c; both are scaled by node+0x1c = RADS. A missing element keeps the node's default
// (RADS 1, LRAD 0.1, RRAD 0.1). Null pointers here mean the element is absent.
struct BoltRadii {
  float left = 0.f;
  float right = 0.f;
};

inline BoltRadii ComputeBoltRadii(float rads, const float* lrad, const float* rrad, bool llrd) {
  float left = 0.1f;
  float right = 0.1f;
  if (lrad != nullptr) {
    left = *lrad;
    if (llrd) {
      right = left;
    }
  }
  if (rrad != nullptr) {
    right = *rrad;
  }
  return BoltRadii{rads * left, rads * right};
}

// ELC2's vertex AddVar is multiplied by inverse exposure before the VFX shader applies
// the room exposure. A missing/nonpositive exposure means no exposed-room factor is active.
inline float ExposureCompensatedIntensity(float intensity, float exposure) {
  return std::isfinite(exposure) && exposure > 0.f ? intensity / exposure : intensity;
}

// CRandom16 as the ELC2 object uses it: seed = seed * 0x41c64e6d + 0x3039, Float() reads the
// low 28 bits (* 2^-28), integers the high half. (Advance-then-read order and the initial seed
// are not confirmed in the decompile.)
struct Rng {
  uint32_t seed = 99;
  uint32_t Next() {
    seed = seed * 0x41c64e6du + 0x3039u;
    return seed;
  }
  float Float() { return float(Next() & 0xfffffffu) * 0x1p-28f; }
  float RangeF(float a, float b) { return (b - a) * Float() + a; }
  int RangeI(int lo, int hi) { return int((Next() >> 16) % uint32_t(hi - lo + 1)) + lo; }
};

// 0xb74e3c. `nodes` is the ELC2 node count (this+0x670); r1 is drawn before r2.
inline void CalculateFractal(Rng& rng, std::vector<float>& a, int lo, int hi, int nodes,
                             float amp, float ampd) {
  for (;;) {
    const int mid = (lo + hi) >> 1;
    const float scale = (float(hi - lo) / float(nodes)) * amp;
    const float r1 = rng.Float();
    const float r2 = rng.Float();
    a[mid] = (r2 * ampd + ((a[lo] + a[hi]) * 0.5f + r1 * scale - scale * 0.5f)) - ampd * 0.5f;
    if ((lo + hi) & 1) {
      a[hi - 1] = a[hi];
    }
    if (mid - lo > 1) {
      CalculateFractal(rng, a, lo, mid, nodes, amp, ampd);
    }
    lo = mid;
    if (hi - mid <= 1) {
      break;
    }
  }
}

inline V3 Bezier(const V3& p0, const V3& p1, const V3& p2, const V3& p3, float t) {
  const float u = 1.f - t;
  return p0 * (u * u * u) + p1 * (3.f * u * u * t) + p2 * (3.f * u * t * t) + p3 * (t * t * t);
}

// Rotate v about unit axis by angle (radians).
inline V3 Rotate(const V3& v, const V3& axis, float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.f - c));
}

struct BoltParams {
  int nodes = 8;
  float slvf = 1.f;   // SLVF
  float blnc = 1.f;   // BLNC (Bezier balance)
  float bcff = 1.f;   // BCFF
  bool fplb = false;  // FPLB
  bool zery = false;  // ZERY
  V3 camAxis{0.f, 0.f, 1.f};  // third column of the camera transform in system space
};

// 0xb73b18. Fills `out` with `nodes` points from start to end.
inline void CalculatePointsForBolt(Rng& rng, const BoltParams& prm, const V3& start,
                                   const V3& end, const V3& startDir, const V3& endDir,
                                   std::vector<V3>& out, float amp, float ampd) {
  const int n = prm.nodes;
  out.assign(size_t(n), V3{});
  const float slv = prm.slvf < 0.f ? 0.f : (prm.slvf > 1.f ? 1.f : prm.slvf);
  const float jit = slv * 0.45f;
  const bool d0 = startDir.x != 0.f || startDir.y != 0.f || startDir.z != 0.f;
  const bool d1 = endDir.x != 0.f || endDir.y != 0.f || endDir.z != 0.f;
  if (!d0 && !d1) {
    out[0] = start;
    if (n > 1) {
      const float inv = 1.f / float(n - 1);
      const V3 step = (end - start) * inv;
      V3 cur = start;
      for (int i = 1; i < n - 1; ++i) {
        cur = cur + step;
        out[size_t(i)] = cur + step * rng.RangeF(-jit, jit);
      }
      out[size_t(n - 1)] = end;
    }
  } else {
    V3 c[4];
    if (d0 && d1) {
      c[0] = start;
      c[1] = start + startDir;
      c[2] = end + endDir;
      c[3] = end;
    } else if (d0) {
      c[0] = start;
      c[1] = start + startDir;
      c[2] = start + startDir * 2.f;
      c[3] = end;
    } else {
      c[0] = start;
      c[1] = end + endDir * 2.f;
      c[2] = end + endDir;
      c[3] = end;
    }
    float bl = prm.blnc < 0.f ? 0.f : (prm.blnc > 1.f ? 1.f : prm.blnc);
    const float bj = 0.45f * bl;
    out[0] = start;
    for (int i = 1; i < n - 1; ++i) {
      const float inv = 1.f / float(n - 1);
      const float t = float(i) * inv + inv * rng.RangeF(-bj, bj);
      out[size_t(i)] = Bezier(c[0], c[1], c[2], c[3], t);
    }
    if (n > 1) {
      out[size_t(n - 1)] = end;
    }
  }
  if (amp != 0.f) {
    std::vector<float> a(size_t(n), 0.f);
    if (n > 1) {
      CalculateFractal(rng, a, 0, n - 1, n, amp, ampd);
    }
    // v1 = p0 - p1, v2 = pLast - p1 (0xb73b18): their cross is the bend-plane normal.
    V3 perp{0.f, 0.f, 1.f};
    float base = 0.f;
    if (n > 2) {
      V3 v1 = out[0] - out[1];
      V3 v2 = out[size_t(n - 1)] - out[1];
      if (IsNormalizable(v1) && IsNormalizable(v2)) {
        v1 = Normalized(v1);
        v2 = Normalized(v2);
        const float d = std::fabs(Dot(v1, v2));
        if (std::fabs(d - 1.f) >= 1e-5f) {
          perp = Normalized(Cross(v1, v2));
          base = std::fabs(Dot(prm.camAxis, perp)) * 90.f;
        } else if (std::fabs(Dot(prm.camAxis, v1)) > 0.9999f) {
          perp = Normalized(Cross(V3{1.f, 0.f, 0.f}, v1));
        } else {
          perp = Normalized(Cross(prm.camAxis, v1));
        }
      }
    }
    const float c = prm.bcff < 0.f ? 0.f : (prm.bcff > 1.f ? 1.f : prm.bcff);
    float extra = rng.RangeF(0.f, (1.f - c) * 90.f);
    if (rng.RangeI(0, 1) != 0) {
      extra = 180.f - extra;
    }
    if (n > 2) {
      std::vector<V3> off(static_cast<size_t>(n));
      for (int i = 1; i < n - 1; ++i) {
        const V3 dir = out[size_t(i + 1)] - out[size_t(i - 1)];
        if (dir.x != 0.f || dir.y != 0.f || dir.z != 0.f) {
          float jit2 = 0.f;
          if (!prm.fplb) {
            jit2 = rng.RangeF(-1.f, 1.f) * (a[size_t(i)] / amp) * 16.f;
          }
          const float ang = (base + extra + jit2) * 0.017453292f;
          off[size_t(i)] = Rotate(perp * a[size_t(i)], Normalized(dir), ang);
        }
      }
      for (int i = 1; i < n - 1; ++i) {
        out[size_t(i)] = out[size_t(i)] + off[size_t(i)];
      }
    }
  }
  if (prm.zery) {
    for (V3& p : out) {
      p.z = 0.f;
    }
  }
}
} // namespace PortElectric2
