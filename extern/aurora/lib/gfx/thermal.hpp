#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

// Port extension: Remastered's thermal visor post (NRenderThermalVisor: cold 0xfb1aa0 / hot 0xfb1cf0 /
// upscale 0xfb2120), run on the finished EFB (see GXPortThermalPass).
namespace aurora::gfx::thermal {
// The task's uniform, word for word.
struct Params {
  float tone[3][4]; // the tone curve the EFB was drawn through (as GXSetPBRTone)
  float v[4];       // pass (0 cold, 1 hot), area heat, thermal time (s), ghost (1: blend the previous frame)
  float r[4];       // cold: the two CRandom16 draws of this call
};
static_assert(sizeof(Params) == 20 * 4);

// The heat gradient: 256x4 RGBA8, as the import writes it. Call before the first pass; false if it
// is not that size.
bool set_lut(const uint8_t* rgba, uint32_t size);
bool has_lut();
// Registers the task (game thread); false if it could not be or there is no gradient.
bool ensure_task();
// Records a pass from the FIFO processor (GX_AURORA_PORT_THERMAL), once ensure_task has returned true.
bool record(const Params& params);
void shutdown();
} // namespace aurora::gfx::thermal
