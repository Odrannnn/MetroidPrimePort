#include "shadow_cascade.hpp"

#include <algorithm>
#include <cmath>

namespace aurora::gfx::shadow::cascade {
namespace {
float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float length(const float v[3]) { return std::sqrt(dot(v, v)); }

// up - dir * dot(up, dir), clamped as LookAt clamps it.
void perpendicular(const float up[3], const float dir[3], float out[3]) {
  const float d = std::clamp(dot(up, dir), -1.f, 1.f);
  for (int i = 0; i < 3; ++i) {
    out[i] = up[i] - dir[i] * d;
  }
}
} // namespace

bool light_basis(const float dir[3], Basis& out) {
  const float len = length(dir);
  if (!(len > 0.f)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    out.z[i] = dir[i] / len;
  }
  // LookAt(eye 0, target dir, up skUpVector, 0.001): up made perpendicular to the look direction;
  // if that leaves (almost) nothing, skUpVector's, then skForwardVector's.
  constexpr float Up[3] = {0.f, 1.f, 0.f};
  constexpr float Forward[3] = {0.f, 0.f, 1.f};
  perpendicular(Up, out.z, out.y);
  float m = length(out.y);
  if (m <= 0.001f) {
    perpendicular(Forward, out.z, out.y);
    m = length(out.y);
  }
  for (float& v : out.y) {
    v /= m;
  }
  // x = y cross z.
  out.x[0] = out.y[1] * out.z[2] - out.y[2] * out.z[1];
  out.x[1] = out.y[2] * out.z[0] - out.y[0] * out.z[2];
  out.x[2] = out.y[0] * out.z[1] - out.y[1] * out.z[0];
  return true;
}

void splits(float znear, float zfar, float distance, float out[Count]) {
  const float n = znear;
  const float f = std::min(distance, zfar);
  for (int i = 1; i <= Count; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(Count);
    const float lin = n + (f - n) * t;
    const float log = n * std::pow(f / n, t);
    out[i - 1] = lin + Lambda * (log - lin);
  }
}

void fit(const Basis& basis, const float viewToWorld[3][4], const Frustum& frustum, const float split[Count],
         uint32_t size, Fit out[Count]) {
  const float n = frustum.znear;
  for (int c = 0; c < Count; ++c) {
    // The slice, as FUN_003bd060 clamps it to the camera's planes.
    const float start = c == 0 ? n : split[c - 1] - Padding;
    const float sliceNear = std::max(n, start);
    const float sliceFar = std::min(std::max(sliceNear, split[c]), frustum.zfar);
    float corners[8][3];
    int k = 0;
    for (const float z : {sliceNear, sliceFar}) {
      const float s = z / n;
      for (const float y : {frustum.bottom * s, frustum.top * s}) {
        for (const float x : {frustum.left * s, frustum.right * s}) {
          for (int i = 0; i < 3; ++i) {
            corners[k][i] = viewToWorld[i][0] * x + viewToWorld[i][1] * y - viewToWorld[i][2] * z + viewToWorld[i][3];
          }
          ++k;
        }
      }
    }
    Fit& cas = out[c];
    float center[3] = {0.f, 0.f, 0.f};
    for (const auto& p : corners) {
      for (int i = 0; i < 3; ++i) {
        center[i] += p[i];
      }
    }
    for (float& v : center) {
      v *= 1.f / 8.f;
    }
    float r2 = 0.f;
    for (const auto& p : corners) {
      const float d[3] = {p[0] - center[0], p[1] - center[1], p[2] - center[2]};
      r2 = std::max(r2, dot(d, d));
    }
    const float r = std::sqrt(r2) + 0.01f;
    std::copy_n(center, 3, cas.center);
    cas.radius = r;

    const float* axes[3] = {basis.x, basis.y, basis.z};
    for (int a = 0; a < 3; ++a) {
      const float mid = dot(axes[a], center);
      cas.lo[a] = mid - r;
      cas.hi[a] = mid + r;
    }
    for (int a = 0; a < 2; ++a) {
      const float texel = (cas.hi[a] - cas.lo[a]) / static_cast<float>(size);
      const float inv = 1.f / texel;
      cas.lo[a] = texel * static_cast<float>(static_cast<int>(cas.lo[a] * inv));
      cas.hi[a] = texel * std::ceil(cas.hi[a] * inv);
    }

    // x mirrored: light space is x = y cross z looking down +z, so the map's right is -x.
    const float scale[3] = {-2.f / (cas.hi[0] - cas.lo[0]), 2.f / (cas.hi[1] - cas.lo[1]), 1.f / (cas.hi[2] - cas.lo[2])};
    const float offset[3] = {(cas.hi[0] + cas.lo[0]) / (cas.hi[0] - cas.lo[0]),
                       -(cas.hi[1] + cas.lo[1]) / (cas.hi[1] - cas.lo[1]), -cas.lo[2] / (cas.hi[2] - cas.lo[2])};
    for (int row = 0; row < 3; ++row) {
      for (int i = 0; i < 3; ++i) {
        cas.clip[row][i] = axes[row][i] * scale[row];
      }
      cas.clip[row][3] = offset[row];
    }
    cas.clip[3][0] = cas.clip[3][1] = cas.clip[3][2] = 0.f;
    cas.clip[3][3] = 1.f;
  }
}
uint8_t box_mask(const Basis& basis, const Fit (&fits)[Count], const float min[3], const float max[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(min[i]) || !std::isfinite(max[i]) || min[i] > max[i]) {
      return All;
    }
    for (const auto& f : fits) {
      if (!std::isfinite(f.lo[i]) || !std::isfinite(f.hi[i]) || f.lo[i] >= f.hi[i]) {
        return All;
      }
    }
  }
  const float* axes[3] = {basis.x, basis.y, basis.z};
  float lo[3], hi[3];
  for (int a = 0; a < 3; ++a) {
    float mid = 0.f, half = 0.f;
    for (int i = 0; i < 3; ++i) {
      if (!std::isfinite(axes[a][i])) {
        return All;
      }
      mid += axes[a][i] * (min[i] * 0.5f + max[i] * 0.5f);
      half += std::fabs(axes[a][i]) * (max[i] * 0.5f - min[i] * 0.5f);
    }
    // Allow for rounding in transformed model bounds and the shader's matrix arithmetic.
    const float margin = 1e-4f + 1e-5f * (std::fabs(mid) + half);
    lo[a] = mid - half - margin;
    hi[a] = mid + half + margin;
    if (!std::isfinite(lo[a]) || !std::isfinite(hi[a])) {
      return All;
    }
  }
  uint8_t mask = 0;
  for (int c = 0; c < Count; ++c) {
    const auto& f = fits[c];
    if (hi[0] >= f.lo[0] && lo[0] <= f.hi[0] && hi[1] >= f.lo[1] && lo[1] <= f.hi[1] && lo[2] <= f.hi[2]) {
      mask |= 1u << c;
    }
  }
  return mask;
}
} // namespace aurora::gfx::shadow::cascade
