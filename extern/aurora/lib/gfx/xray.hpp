#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

// Port extension: Remastered's X-ray visor post passes (shader 00089f0, render_xray_effect), run on
// the finished EFB (see GXPortXRayPass).
namespace aurora::gfx::xray {
// The task's uniform, word for word.
struct Params {
  float tone[3][4];  // the tone curve the EFB was drawn through (as GXSetPBRTone)
  float p[8][4];     // cb0 p0..p7 of 00089f0; p0.z selects the pass (1: distortion, else ramp)
  float depth[4];    // x, y: the GX z range the world draws in (min, max); z: low-resolution target scale
};
static_assert(sizeof(Params) == 48 * 4);

// Registers the task (game thread); false if it could not be.
bool ensure_task();
// Records a pass from the FIFO processor (GX_AURORA_PORT_XRAY), once ensure_task has returned true.
bool record(const Params& params);
void shutdown();
} // namespace aurora::gfx::xray
