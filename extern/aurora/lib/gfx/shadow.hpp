#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

#include "shadow_cascade.hpp"

namespace aurora::gx {
struct DrawData;
} // namespace aurora::gx

// Port extension: the sun's shadow (GXPortSetShadowFrame), as Remastered's cascaded shadow map: four
// orthographic depth maps over slices of the view (shadow_cascade.hpp), drawn after the frame's
// casters (GXPortRenderShadowMap) and read by the next frame's receivers, so a draw's caster and
// receiver matrices both come from set_frame.
namespace aurora::gfx::shadow {
constexpr wgpu::TextureFormat MapFormat = wgpu::TextureFormat::Depth32Float;

// The receivers' uniform (ShaderInfo::usesShadow), as set_frame fills it: view -> each cascade's
// clip space for the casters (this frame's map) and the receivers (the previous frame's), as rows;
// the view-space direction to the sun; the extra light's colour with one texel (uv) in w; the far
// ends of the receivers' cascades (view depth); their fade (start, end; lit past the end, all lit
// when there is no map).
struct Uniform {
  float caster[cascade::Count][4][4];
  float receiver[cascade::Count][4][4];
  float dir[4];
  float color[4];
  float splits[4];
  float fade[4];
};
static_assert(sizeof(Uniform) == 36 * 16);

// Registers the map's encoder task (game thread); false if it could not be.
bool ensure_task();
// Sets the frame's sun (FIFO processor): worldToView as GX's view matrix (3 rows of 4), frustum the
// camera's, sunDir the way its light travels in world space, distance how far from the camera it
// shadows (<= 0: none). False when there is no sun; out is filled otherwise.
bool set_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               const float color[3], Uniform& out);
// The cascades set_frame would fit for the same arguments (any thread); false when there are none.
bool fit_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               cascade::Fit out[cascade::Count]);
// Whether a world box [min, max] reaches into one of those cascades (any thread).
bool box_casts(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               const float min[3], const float max[3]);
// How many draws the last recorded map had (any thread).
uint32_t last_caster_count();
// Last encoded map: actual submissions per cascade and otherwise eligible layer draws omitted.
void last_layer_counts(uint32_t submitted[4], uint32_t& rejected);
// A draw that casts into this frame's map (FIFO processor).
void add_caster(const gx::DrawData& draw);
// Records the map's pass over the casters added since the last (FIFO processor).
bool record();
// The maps (a 2D array, a layer a cascade) and their comparison sampler, for the receivers' bind group.
const wgpu::TextureView& map_view();
const wgpu::Sampler& sampler();
void shutdown();
} // namespace aurora::gfx::shadow
