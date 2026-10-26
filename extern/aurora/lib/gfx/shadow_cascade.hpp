#pragma once

#include <cstdint>

// The sun's shadow cascades as Remastered fits them (the hangar's sun, the only real-time sun it
// has: 4 cascades of 1024², 350 units, split lambda 0.8, 2.5 units of overlap). Pure maths, no GPU.
//   splits: NShadowMap's practical split scheme between the camera's near plane and
//     min(distance, far): lin + lambda * (log - lin).
//   fit: FUN_003bd060. The cascade's slice of the view frustum (its near pulled back by the
//     padding), the sphere around its 8 corners (mean centre, radius + 0.01), that sphere's box in
//     light space, x/y snapped outward to whole texels (min truncated toward zero, max ceiled), z
//     as is. The light space is CTransform4f::LookAt(0, dir, skUpVector (0,1,0)).inverse: x = up x
//     dir, y = the up vector made perpendicular to dir, z = dir.
namespace aurora::gfx::shadow::cascade {
constexpr int Count = 4;
constexpr float Lambda = 0.8f;
constexpr float Padding = 2.5f;
constexpr float DefaultDistance = 350.f;
constexpr uint32_t DefaultSize = 1024;
constexpr uint8_t All = (1u << Count) - 1u;

// The camera's perspective: its extents on the near plane (view space, GX looks down -z) and its
// near and far distances.
struct Frustum {
  float left, right, top, bottom, znear, zfar;
};

// Light space's axes in world space (rows of world -> light).
struct Basis {
  float x[3], y[3], z[3];
};

struct Fit {
  // World -> the map's clip space (rows): x/y in [-1, 1] across the snapped box (x mirrored so a
  // triangle keeps the winding the camera sees it with), z in [0, 1] from the box's near to far.
  float clip[4][4];
  // The bounding sphere (world) and the light-space box the map covers.
  float center[3];
  float radius;
  float lo[3], hi[3];
};

// False if dir is zero.
bool light_basis(const float dir[3], Basis& out);
// The far ends of the cascades.
void splits(float znear, float zfar, float distance, float out[Count]);
// Fits every cascade; viewToWorld is GX's view matrix inverted (3 rows of 4).
void fit(const Basis& basis, const float viewToWorld[3][4], const Frustum& frustum, const float split[Count],
         uint32_t size, Fit out[Count]);
// Conservative membership of a world-space box in the fitted maps. vs_shadow pancakes casters
// toward the sun onto the near plane, so only the far z plane rejects them. Uncertain bounds
// retain every cascade. Fit once per frame, then reuse the basis/fits for every box.
uint8_t box_mask(const Basis& basis, const Fit (&fits)[Count], const float min[3], const float max[3]);
} // namespace aurora::gfx::shadow::cascade
