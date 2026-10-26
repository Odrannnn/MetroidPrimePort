#include "gfx/shadow_cascade.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

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

// Deliberately project all eight corners in double precision instead of repeating the production
// centre/half-extent formula. A conservative mask may contain extra bits, but may never lose one.
uint8_t corner_mask(const Basis& basis, const Fit (&fits)[Count], const float min[3], const float max[3]) {
  const float* axes[3] = {basis.x, basis.y, basis.z};
  double lo[3] = {INFINITY, INFINITY, INFINITY};
  double hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (int corner = 0; corner < 8; ++corner) {
    for (int axis = 0; axis < 3; ++axis) {
      double value = 0;
      for (int i = 0; i < 3; ++i) {
        value += double(axes[axis][i]) * (corner & (1 << i) ? max[i] : min[i]);
      }
      lo[axis] = std::min(lo[axis], value);
      hi[axis] = std::max(hi[axis], value);
    }
  }
  uint8_t mask = 0;
  for (int c = 0; c < Count; ++c) {
    if (hi[0] >= fits[c].lo[0] && lo[0] <= fits[c].hi[0] && hi[1] >= fits[c].lo[1] &&
        lo[1] <= fits[c].hi[1] && lo[2] <= fits[c].hi[2]) {
      mask |= 1u << c;
    }
  }
  return mask;
}

void separated_fits(Fit (&fits)[Count]) {
  for (int c = 0; c < Count; ++c) {
    fits[c] = {};
    fits[c].lo[0] = float(c * 10);
    fits[c].hi[0] = float(c * 10 + 5);
    fits[c].lo[1] = -5.f;
    fits[c].hi[1] = 5.f;
    fits[c].lo[2] = 0.f;
    fits[c].hi[2] = 10.f;
  }
}

// A model's world AABB after rotation, translation, nonuniform/negative scale, and shear. Build it
// independently from transformed corners, rounding outward when narrowing the double results.
void transformed_box(const float m[3][4], const float min[3], const float max[3], float outMin[3], float outMax[3]) {
  for (int row = 0; row < 3; ++row) {
    double lo = INFINITY;
    double hi = -INFINITY;
    for (int corner = 0; corner < 8; ++corner) {
      double value = m[row][3];
      for (int i = 0; i < 3; ++i) {
        value += double(m[row][i]) * (corner & (1 << i) ? max[i] : min[i]);
      }
      lo = std::min(lo, value);
      hi = std::max(hi, value);
    }
    outMin[row] = std::nextafter(float(lo), -INFINITY);
    outMax[row] = std::nextafter(float(hi), INFINITY);
  }
}

void rotated_camera(float yaw, float pitch, const float position[3], float out[3][4]) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const float rotation[3][3] = {{cy, -sy * cp, sy * sp}, {sy, cy * cp, -cy * sp}, {0.f, sp, cp}};
  for (int row = 0; row < 3; ++row) {
    std::copy_n(rotation[row], 3, out[row]);
    out[row][3] = position[row];
  }
}
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

TEST(ShadowCascade, BoxMaskKeepsSunwardCastersAndTouchingBoundaries) {
  const Basis basis{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  Fit fits[Count];
  separated_fits(fits);
  for (int c = 0; c < Count; ++c) {
    SCOPED_TRACE(c);
    const uint8_t bit = 1u << c;
    const float sunwardMin[3] = {float(c * 10 + 1), -1.f, -100000.f};
    const float sunwardMax[3] = {float(c * 10 + 2), 1.f, -99999.f};
    EXPECT_EQ(box_mask(basis, fits, sunwardMin, sunwardMax), bit);
    // Both x/y faces, the far-depth face, and zero-sized boxes must remain included.
    for (float x : {fits[c].lo[0], fits[c].hi[0]}) {
      for (float y : {fits[c].lo[1], fits[c].hi[1]}) {
        const float edge[3] = {x, y, fits[c].hi[2]};
        EXPECT_EQ(box_mask(basis, fits, edge, edge), bit);
      }
    }
    const float beyondFar[3] = {float(c * 10 + 2), 0.f, 11.f};
    EXPECT_EQ(box_mask(basis, fits, beyondFar, beyondFar), 0);
  }
  const float between[3] = {7.f, 0.f, 0.f};
  EXPECT_EQ(box_mask(basis, fits, between, between), 0);
  const float wideMin[3] = {-1.f, -1.f, -1.f}, wideMax[3] = {36.f, 1.f, 1.f};
  EXPECT_EQ(box_mask(basis, fits, wideMin, wideMax), All);
}

TEST(ShadowCascade, BoxMaskFailsOpenForInvalidBoundsBasisAndFits) {
  const Basis basis{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  Fit fits[Count];
  separated_fits(fits);
  // This valid box misses every cascade. Invalid input must turn that rejection into All.
  const float min[3] = {100.f, 100.f, 100.f}, max[3] = {101.f, 101.f, 101.f};
  ASSERT_EQ(box_mask(basis, fits, min, max), 0);
  const float invalids[] = {std::numeric_limits<float>::quiet_NaN(), INFINITY, -INFINITY};
  for (int axis = 0; axis < 3; ++axis) {
    SCOPED_TRACE(axis);
    for (float invalid : invalids) {
      float badMin[3], badMax[3];
      std::copy_n(min, 3, badMin);
      std::copy_n(max, 3, badMax);
      badMin[axis] = invalid;
      EXPECT_EQ(box_mask(basis, fits, badMin, max), All);
      badMax[axis] = invalid;
      EXPECT_EQ(box_mask(basis, fits, min, badMax), All);
      for (int row = 0; row < 3; ++row) {
        Basis badBasis = basis;
        float* rows[3] = {badBasis.x, badBasis.y, badBasis.z};
        rows[row][axis] = invalid;
        EXPECT_EQ(box_mask(badBasis, fits, min, max), All);
      }
      for (int c = 0; c < Count; ++c) {
        Fit badFits[Count];
        std::copy_n(fits, Count, badFits);
        badFits[c].lo[axis] = invalid;
        EXPECT_EQ(box_mask(basis, badFits, min, max), All);
        std::copy_n(fits, Count, badFits);
        badFits[c].hi[axis] = invalid;
        EXPECT_EQ(box_mask(basis, badFits, min, max), All);
      }
    }
    float invertedMin[3];
    std::copy_n(min, 3, invertedMin);
    invertedMin[axis] = max[axis] + 1.f;
    EXPECT_EQ(box_mask(basis, fits, invertedMin, max), All);
    for (int c = 0; c < Count; ++c) {
      Fit badFits[Count];
      std::copy_n(fits, Count, badFits);
      badFits[c].lo[axis] = badFits[c].hi[axis] + 1.f;
      EXPECT_EQ(box_mask(basis, badFits, min, max), All);
      badFits[c].lo[axis] = badFits[c].hi[axis];
      EXPECT_EQ(box_mask(basis, badFits, min, max), All);
    }
  }
}

TEST(ShadowCascade, BoxMaskHandlesLargeFiniteBoundsWithoutIntermediateOverflow) {
  const Basis basis{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
  Fit fits[Count];
  separated_fits(fits);
  const float large = std::numeric_limits<float>::max();
  // min+max overflows although the centre and half-extent are representable.
  const float farMin[3] = {large * .6f, -1.f, -1.f}, farMax[3] = {large * .9f, 1.f, 1.f};
  EXPECT_EQ(box_mask(basis, fits, farMin, farMax), 0);
  // max-min also overflows; this box still covers all four layers.
  const float wideMin[3] = {-large * .9f, -1.f, -1.f}, wideMax[3] = {large * .9f, 1.f, 1.f};
  EXPECT_EQ(box_mask(basis, fits, wideMin, wideMax), All);
}

TEST(ShadowCascade, BoxMaskUsesRealFitsAndPreservesAdjacentCascadeOverlap) {
  const float sun[3] = {0.f, 1.f, 0.f};
  Basis basis;
  ASSERT_TRUE(light_basis(sun, basis));
  float split[Count];
  splits(kFrustum.znear, kFrustum.zfar, DefaultDistance, split);
  Fit fits[Count];
  fit(basis, kViewToWorld, kFrustum, split, DefaultSize, fits);
  for (int c = 0; c < Count - 1; ++c) {
    SCOPED_TRACE(c);
    for (float depth : {split[c] - Padding * .25f, split[c], split[c] + Padding * .25f}) {
      const float point[3] = {10.f, -20.f + depth, 5.f};
      const uint8_t adjacent = (1u << c) | (1u << (c + 1));
      EXPECT_EQ(box_mask(basis, fits, point, point) & adjacent, adjacent);
    }
  }
  for (int c = 0; c < Count; ++c) {
    SCOPED_TRACE(c);
    EXPECT_NE(box_mask(basis, fits, fits[c].center, fits[c].center) & (1u << c), 0);
    float sunward[3];
    for (int axis = 0; axis < 3; ++axis) {
      sunward[axis] = fits[c].center[axis] - basis.z[axis] * 10000.f;
    }
    float clip[3];
    transform(fits[c].clip, sunward, clip);
    ASSERT_LT(clip[2], 0.f); // The vertex shader pancakes this caster onto depth zero.
    EXPECT_NE(box_mask(basis, fits, sunward, sunward) & (1u << c), 0);
  }
  const float far[3] = {10.f, fits[Count - 1].hi[2] + 100.f, 5.f};
  EXPECT_EQ(box_mask(basis, fits, far, far), 0);
}

TEST(ShadowCascade, BoxMaskConservativelyContainsTransformedCornerProjection) {
  std::mt19937 rng(42917);
  std::uniform_real_distribution<float> angle(-3.14f, 3.14f), world(-100000.f, 100000.f);
  std::uniform_real_distribution<float> position(-500.f, 500.f), depth(-100.f, 600.f);
  std::uniform_real_distribution<float> extent(.01f, 20.f), scale(.1f, 4.f), shear(-.5f, .5f);
  const float sunDirections[][3] = {{.3f, -.4f, -.8f}, {0.f, 1.f, 0.f}, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}};
  int included = 0, rejected = 0, selective = 0;
  for (int scene = 0; scene < 32; ++scene) {
    SCOPED_TRACE(scene);
    Basis basis;
    ASSERT_TRUE(light_basis(sunDirections[scene % 4], basis));
    const float cameraPosition[3] = {world(rng), world(rng), world(rng)};
    float camera[3][4];
    rotated_camera(angle(rng), angle(rng), cameraPosition, camera);
    float split[Count];
    splits(kFrustum.znear, kFrustum.zfar, DefaultDistance, split);
    Fit fits[Count];
    fit(basis, camera, kFrustum, split, DefaultSize, fits);
    for (int sample = 0; sample < 256; ++sample) {
      float model[3][4];
      const float origin[3] = {};
      rotated_camera(angle(rng), angle(rng), origin, model);
      const float cameraLocal[3] = {position(rng), position(rng), -depth(rng)};
      for (int col = 0; col < 3; ++col) {
        const float signedScale = scale(rng) * (sample & (1 << col) ? -1.f : 1.f);
        for (int row = 0; row < 3; ++row) {
          model[row][col] *= signedScale;
        }
      }
      model[0][1] += shear(rng);
      model[2][0] += shear(rng);
      for (int row = 0; row < 3; ++row) {
        model[row][3] = camera[row][3];
        for (int col = 0; col < 3; ++col) {
          model[row][3] += camera[row][col] * cameraLocal[col];
        }
        // Ensure positive samples as well as off-screen/behind-far rejections.
        if (sample % 8 == 0) {
          model[row][3] = fits[(sample / 8) % Count].center[row];
        }
      }
      const float localMin[3] = {-extent(rng), -extent(rng), -extent(rng)};
      const float localMax[3] = {extent(rng), extent(rng), extent(rng)};
      float min[3], max[3];
      transformed_box(model, localMin, localMax, min, max);
      const uint8_t reference = corner_mask(basis, fits, min, max);
      const uint8_t actual = box_mask(basis, fits, min, max);
      ASSERT_EQ(actual & reference, reference) << "sample " << sample;
      EXPECT_EQ(actual & ~All, 0);
      included += reference != 0;
      rejected += actual == 0;
      selective += actual != 0 && actual != All;
    }
  }
  // These keep the independent check meaningful if a regression starts returning All universally.
  EXPECT_GT(included, 1000);
  EXPECT_GT(rejected, 1000);
  EXPECT_GT(selective, 500);
}
