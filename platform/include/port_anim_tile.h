#pragma once

// Remastered's animated-texture tile pick, shared by CUVEAnimTexture::GetValue 0x2cd750 (ATEX,
// ATX2) and CUVEAnimFlipBookTexture::GetValue 0x2cde08 (ATFB): frame x count over the cycle, the
// reciprocal FRECPE with two fused FRECPS steps; looping wraps, else it holds the last tile.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// AArch64 FRECPE on a float (FPRecipEstimate): 8 bits of the reciprocal from the top 8 of the
// mantissa. Outside normal inputs whose reciprocal is normal it falls back to the true one.
inline float PortRecipEstimate(float x) {
  uint32_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  const uint32_t exp = bits >> 23 & 0xff;
  if (exp == 0 || exp >= 253) {
    return 1.f / x;
  }
  const uint32_t a = (0x100 | (bits >> 15 & 0xff)) * 2 + 1;
  const uint32_t r = ((1u << 19) / a + 1) / 2;
  const uint32_t out = (bits & 0x80000000u) | (253 - exp) << 23 | (r & 0xff) << 15;
  float result;
  std::memcpy(&result, &out, sizeof(result));
  return result;
}

// A zero cycle is tile 0. Remastered reads before its table for a negative tile; nothing shipped
// reaches it, so it clamps to 0.
inline int PortAnimTile(int count, bool loop, float cycle, int frame) {
  if (cycle == 0.f || count <= 0) {
    return 0;
  }
  float e = PortRecipEstimate(cycle);
  e *= std::fma(-e, cycle, 2.f);
  e *= std::fma(-e, cycle, 2.f);
  int tile = int(float(count) * float(frame) * e);
  if (loop) {
    tile = std::max(tile, 0);
    if (tile >= count) {
      tile %= count;
    }
  } else if (tile >= count) {
    tile = count - 1;
  }
  return std::max(tile, 0);
}
