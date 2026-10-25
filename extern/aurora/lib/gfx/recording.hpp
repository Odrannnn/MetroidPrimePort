#pragma once

#include "types.hpp"
#include "hdr_tone.hpp"

#include <cstddef>
#include <string>

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx::detail {

struct FramePacket;

struct RecordedFrame {
  FramePacket* packet;
  size_t frameSlot;
};

void begin_recording(FramePacket& packet, size_t frameSlot);
RecordedFrame end_recording();
void shutdown_recording();
void increment_merged_draw_count() noexcept;

namespace testing {
void suppress_render_worker(bool suppress) noexcept;
void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat);
}

} // namespace aurora::gfx::detail

namespace aurora::gfx {
struct ColorPassDescriptor {
  const char* label = nullptr;
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView;
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Extent3D targetSize;
  uint32_t sampleCount = 1;
  wgpu::LoadOp colorLoadOp = wgpu::LoadOp::Clear;
  wgpu::StoreOp colorStoreOp = wgpu::StoreOp::Store;
  wgpu::Color clearColor{0.f, 0.f, 0.f, 0.f};
  bool hasDepth = false;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Undefined;
  float depthClearValue = 0.f;
  bool hasStencil = false;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
};

void finish();
void begin_color_pass(const ColorPassDescriptor& desc);
void end_color_pass();
void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size);
void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
uint32_t get_sample_count() noexcept;
RenderTargetLayout get_render_target_layout() noexcept;
// Switches EFB passes between the 8-bit display EFB and the RGBA16F linear HDR scene
// (webgpu::g_sceneHdr, same size, sample count and depth buffer), sealing the current EFB pass. The
// first enable of a frame clears the HDR scene to 0; every frame starts with it off. False (mode
// unchanged) outside an EFB pass, when the device is unsupported, or when the target cannot be
// allocated (webgpu::ensure_scene_hdr_target).
// Debug/test plumbing only: GX pipelines, EFB copies, pass snapshots, post passes and present still
// assume the display format, so no game draw, copy or snapshot may run while it is on (stage 2).
bool set_scene_hdr(bool on);
bool scene_hdr_active() noexcept;
// Whether the pass being recorded is the HDR scene's own (scene_hdr_active, and not offscreen or a
// begin_color_pass pass on another target): what GX and VFX pipelines take their colour format from.
bool scene_hdr_pass_active() noexcept;
void clear_caches() noexcept;
void clear_offscreen_cache() noexcept;
// Between begin_frame and end_frame on the main thread: a frame is being recorded.
bool recording_active() noexcept;

namespace tex_palette_conv {
struct ConvRequest;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req);

// What the push functions return when the frame's mapped buffer has no room left for the data
// (a mapped buffer cannot grow). The caller drops whatever needed it.
inline constexpr Range OverflowRange{UINT32_MAX, 0};
constexpr bool overflowed(const Range& range) noexcept { return range.offset == OverflowRange.offset; }

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_indices(ArrayRef<T> data, size_t alignment) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_uniform(const uint8_t* data, size_t length);
// Reserves a MaxUniformSize window of uniform data for the caller to fill in place, saving push_uniform's
// copy; unmap_uniform then gives back what wasn't used. out is null (and the range OverflowRange) when the
// frame's uniform buffer is full. Nothing else may push uniforms in between.
Range map_uniform(uint8_t*& out);
void unmap_uniform(Range& range, size_t length);
template <typename T>
Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
template <typename T>
Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_texture_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage);

template <typename DrawData>
void push_draw_command(DrawData data);
// push_encoder_task's recording, for the FIFO processor, which is where the commands before
// it have already been recorded (push_encoder_task drains the FIFO to get there). The task
// type must be registered and the payload checked by the caller.
bool record_encoder_task(uint64_t type, const void* payload, size_t payloadSize); // type: an EncoderTaskId
// record_encoder_task, with the custom draw drawType (registered, no payload) first in the pass
// that follows, which clears its colour instead of loading it: the draw must cover every pixel.
// depth says what that pass does with the sealed pass's depth.
enum class DepthAfter : uint8_t {
  Load,
  Clear, // and drops the sealed pass's depth store
  // Clear if the frame ends in that pass and no draw there may test against the old depth (one
  // with depth compare whose viewport depth range reaches the nearest depth the EFB was given),
  // else Load. The sealed pass and the task wait for the render worker until that is known.
  IfUnread,
};
// drawPayload (at most InlineDrawPayloadSize bytes) goes to the draw. leaveSceneHdr: the pass that
// follows is the display EFB's instead of the HDR scene's, which this frame's later EFB passes draw into
// too (set_scene_hdr(false)), as a post that tones the HDR scene into the display EFB needs. Only while the
// HDR scene's own pass is recorded (scene_hdr_pass_active); refused (nothing recorded) otherwise.
bool record_encoder_task_overwriting(uint64_t type, const void* payload, size_t payloadSize, uint64_t drawType,
                                     DepthAfter depth = DepthAfter::Load, const void* drawPayload = nullptr,
                                     size_t drawPayloadSize = 0, bool leaveSceneHdr = false);
template <typename DrawData>
DrawData* get_last_draw_command();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);
// As above, but the lookup joins the front of the compile queue and waits for the result, so the
// draw that follows cannot be skipped for a pipeline that is not built yet (the GPU self-test).
PipelineRef pipeline_ref(const gx::PipelineConfig& config, bool blocking);

// An EFB copy recorded while the pass draws the HDR scene (scene_hdr_pass_active): the tone curve
// it post-tones the scene through, as it was when the copy was made, or, linear, the scene itself
// into an RGBA16F texture (GXPortCopyScene).
struct ResolveHdr {
  hdr::Tone tone{};
  bool linear = false;
};
// In an HDR scene pass the copy is HdrDisplay (hdr, or no tone data: the plain sRGB encode) or
// Linear (hdr->linear), and clearColorValue, a display colour, clears the scene as hdr::exposed of it.
// opaqueAlpha: the source has no alpha (an RGB8 EFB), so a converted copy reads it as 1.
void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8,
                       int probeFace = -1, bool resolveMips = false, const ResolveHdr* hdr = nullptr,
                       bool opaqueAlpha = false);
// The scene copy point (vfx::capture_scene): one break of the EFB pass that resolves its colour into
// color (the whole target, scaled to color's size, its mips built; hdr as for resolve_pass_into) and,
// where the device can, snapshots its raw depth (R32Float, this frame slot's pool) into depth. The
// EFB continues on a pass that loads both. False, with nothing recorded, outside an on-screen EFB pass.
bool resolve_scene_copy(TextureHandle color, const ResolveHdr* hdr, wgpu::TextureView& depth);
namespace vfx {
// This frame's scene copy (capture_scene) of the target being drawn, when it was taken in the scene
// pass of the same kind (hdr); else null.
wgpu::TextureView scene_copy_view(bool hdr) noexcept;
} // namespace vfx
// The frame slot being recorded: its pool entries stay alive until the render worker has encoded it.
size_t current_frame_slot() noexcept;
uint32_t align_uniform(uint32_t value);
Vec2<uint32_t> get_render_target_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;
void push_debug_group(std::string label);
void insert_debug_marker(std::string label);
} // namespace aurora::gfx
