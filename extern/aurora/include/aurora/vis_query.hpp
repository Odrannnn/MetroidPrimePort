#pragma once

#include <cstdint>

// Occlusion queries for the game (Remastered's particle visibility, VISQ). A query is a set of
// triangles drawn invisibly into the open EFB pass inside an occlusion query; its result (the
// samples that passed) arrives a few frames later. Inactive (no query set, no pass hooks) until
// the first draw.
namespace aurora::gfx::vis_query {
// Game thread. Draws `vertexCount` world-space vertices (a triangle list, xyz each) under the
// current GX position matrix and projection as query `key`: with depthTest, the samples nearer
// than the depth buffer pass (no depth write); without, every covered sample does. A key drawn
// twice in a frame keeps the later result.
void draw(uint64_t key, const float (*verts)[3], uint32_t vertexCount, bool depthTest);
// Game thread. The samples of `key`'s latest result, taken: false (samples unchanged) when none
// arrived since the last take. Untaken results expire after a few frames.
bool take(uint64_t key, uint64_t& samples);
} // namespace aurora::gfx::vis_query
