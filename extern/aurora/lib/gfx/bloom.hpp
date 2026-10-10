#pragma once

#include <cstdint>

// Port extension: Remastered's frame bloom (CRenderPass_Bloom) and colour grade, run on the
// finished EFB between two passes (see GXPortPostProcess).
namespace aurora::gfx::bloom {
constexpr uint32_t GradeLutSize = 33;

struct Params {
  float threshold;
  float tints[5][3];
  float tone[3][4];   // [0][3]: the exposure to measure the frame at (0: don't)
  uint32_t bloom;     // bit 0: the bloom (else grade only); the Cost bits below
  uint32_t gradeA;    // LUT ids from set_grade_lut; 0 is the identity
  uint32_t gradeB;
  float gradeWeight;  // 0 draws A, 1 draws B
};
static_assert(sizeof(Params) <= 128);
// Params::bloom bits for speed tests (GXSetPBRCostTest), which leave out one part of the work and
// look wrong: no copy of the frame (the composite reads a stale one), and the pass after the bloom
// clearing depth instead of loading it. CostKeepDepth looks right: it always loads depth after the
// bloom, as before that pass learned to clear it when the HUD doesn't test against it.
constexpr uint32_t CostNoFrameCopy = 4;
constexpr uint32_t CostNoDepthReload = 8;
constexpr uint32_t CostKeepDepth = 16;
// The world was drawn into the HDR scene target (GXPortSceneHdr): the composite is the HDR -> LDR step.
constexpr uint32_t HdrInput = 32;

static_assert(sizeof(Params) % 4 == 0);

// Records the bloom at this point of the frame. False when it could not be recorded.
// Waits for the FIFO to be processed first; GXPortPostProcess queues it instead.
bool push(const Params& params);
// Registers the bloom's encoder task (game thread); false if it could not be.
bool ensure_task();
// Records the bloom from the FIFO processor (GX_AURORA_PORT_POST_PROCESS), once ensure_task
// has returned true.
void record(const Params& params);
// Stores a 33^3 RGBA8 LUT (red fastest) under id (non-zero), uploaded before its next use.
void set_grade_lut(uint32_t id, const uint8_t* rgba);
// Maps the readbacks of the frame average that the last submit carried.
void after_submit() noexcept;
// The radiance (the average undone by its exposure) of the latest frame read back, and a
// count that goes up with every new one; false before the first.
bool frame_radiance(float out[3], uint32_t& serial);
void shutdown();
} // namespace aurora::gfx::bloom
