#include "gfx/shadow_cascade.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace aurora::gfx::shadow::cascade;

namespace {
float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void transform(const float m[4][4], const float p[3], float out[3]) {
  for (int r = 0; r < 3; ++r) {
    out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
  }
}

// A camera at (10, -20, 5) looking down world +y (GX's view looks down -z, Z-up world), 60° fov.
constexpr float kViewToWorld[3][4] = {
    {1.f, 0.f, 0.f, 10.f},
    {0.f, 0.f, -1.f, -20.f},
    {0.f, 1.f, 0.f, 5.f},
};
constexpr Frustum kFrustum{-0.1539f, 0.1539f, 0.1155f, -0.1155f, 0.2f, 4096.f};
} // namespace

TEST(ShadowCascade, SplitsArePractical) {
  float split[Count];
  splits(0.2f, 4096.f, DefaultDistance, split);
  EXPECT_NEAR(split[0], 18.565f, 0.01f);
  EXPECT_NEAR(split[1], 41.713f, 0.01f);
  EXPECT_NEAR(split[2], 95.801f, 0.01f);
  EXPECT_FLOAT_EQ(split[3], 350.f);
  // The camera's far plane caps the distance.
  splits(0.2f, 100.f, DefaultDistance, split);
  EXPECT_FLOAT_EQ(split[3], 100.f);
}

TEST(ShadowCascade, BasisIsLookAt) {
  const float dir[3] = {0.3f, -0.4f, -0.8f};
  Basis basis;
  ASSERT_TRUE(light_basis(dir, basis));
  const float len = std::sqrt(dot(dir, dir));
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(basis.z[i], dir[i] / len, 1e-6f);
  }
  EXPECT_NEAR(dot(basis.x, basis.x), 1.f, 1e-5f);
  EXPECT_NEAR(dot(basis.y, basis.y), 1.f, 1e-5f);
  EXPECT_NEAR(dot(basis.x, basis.y), 0.f, 1e-5f);
  EXPECT_NEAR(dot(basis.x, basis.z), 0.f, 1e-5f);
  EXPECT_NEAR(dot(basis.y, basis.z), 0.f, 1e-5f);
  // y is world up (0, 1, 0) made perpendicular to dir, and x = y x dir.
  EXPECT_GT(basis.y[1], 0.f);
  const float cross[3] = {basis.y[1] * basis.z[2] - basis.y[2] * basis.z[1],
                          basis.y[2] * basis.z[0] - basis.y[0] * basis.z[2],
                          basis.y[0] * basis.z[1] - basis.y[1] * basis.z[0]};
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(basis.x[i], cross[i], 1e-5f);
  }
  const float zero[3] = {};
  EXPECT_FALSE(light_basis(zero, basis));
  // Straight along world up falls back to skForwardVector.
  const float up[3] = {0.f, 1.f, 0.f};
  ASSERT_TRUE(light_basis(up, basis));
  EXPECT_NEAR(dot(basis.x, basis.x), 1.f, 1e-5f);
  EXPECT_NEAR(dot(basis.y, basis.z), 0.f, 1e-5f);
}

TEST(ShadowCascade, FitSnapsAndCovers) {
  const float dir[3] = {0.3f, -0.4f, -0.8f};
  Basis basis;
  ASSERT_TRUE(light_basis(dir, basis));
  float split[Count];
  splits(kFrustum.znear, kFrustum.zfar, DefaultDistance, split);
  Fit fits[Count];
  fit(basis, kViewToWorld, kFrustum, split, DefaultSize, fits);
  for (int c = 0; c < Count; ++c) {
    const Fit& f = fits[c];
    SCOPED_TRACE(c);
    // x/y snapped to whole texels of the box's own size.
    for (int a = 0; a < 2; ++a) {
      const float texel = (f.hi[a] - f.lo[a]) / DefaultSize;
      ASSERT_GT(texel, 0.f);
      EXPECT_LE(f.hi[a] - f.lo[a], 2.f * f.radius + 2.f * texel + 1e-3f);
    }
    // The sphere maps inside the map: its centre and the ends of its light-space extent.
    float centerClip[3];
    transform(f.clip, f.center, centerClip);
    for (int a = 0; a < 2; ++a) {
      EXPECT_LE(std::fabs(centerClip[a]), 1.f);
    }
    EXPECT_GE(centerClip[2], 0.f);
    EXPECT_LE(centerClip[2], 1.f);
    const float* axes[3] = {basis.x, basis.y, basis.z};
    for (int a = 0; a < 3; ++a) {
      for (float s : {-1.f, 1.f}) {
        float p[3];
        for (int i = 0; i < 3; ++i) {
          p[i] = f.center[i] + s * f.radius * axes[a][i];
        }
        float q[3];
        transform(f.clip, p, q);
        // Up to a texel out: the snap truncates the box's min toward zero, as Remastered's does, so
        // a negative min moves in.
        EXPECT_LE(std::fabs(q[0]), 1.f + 2.f / DefaultSize);
        EXPECT_LE(std::fabs(q[1]), 1.f + 2.f / DefaultSize);
        EXPECT_GE(q[2], -1e-4f);
        EXPECT_LE(q[2], 1.f + 1e-4f);
      }
    }
    // x is mirrored (light-space +x goes to clip -x), y is not.
    EXPECT_LT(f.clip[0][0] * basis.x[0] + f.clip[0][1] * basis.x[1] + f.clip[0][2] * basis.x[2], 0.f);
    EXPECT_GT(f.clip[1][0] * basis.y[0] + f.clip[1][1] * basis.y[1] + f.clip[1][2] * basis.y[2], 0.f);
    EXPECT_FLOAT_EQ(f.clip[3][3], 1.f);
  }
  // Each cascade covers more than the one before.
  for (int c = 1; c < Count; ++c) {
    EXPECT_GT(fits[c].radius, fits[c - 1].radius);
  }
}
