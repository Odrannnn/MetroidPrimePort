#include <gtest/gtest.h>

#include <aurora/gfx.hpp>

#include "gfx/bloom.hpp"
#include "gfx/frame.hpp"
#include "frame_staging.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/fx_pipelines.hpp"
#include "gfx/hdr_tone.hpp"
#include "gfx/recording.hpp"
#include "gfx/resources.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/texture.hpp"
#include "gfx/thermal.hpp"
#include "gfx/volfog.hpp"
#include "gfx/xray.hpp"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// The RGBA16F HDR scene target's allocation, state and layout plumbing on Dawn's Null backend: real
// textures, views and error scopes, but no rendering, so nothing here proves what a GPU draws.
namespace aurora::gfx {
namespace {

constexpr auto ColorFormat = wgpu::TextureFormat::RGBA8Unorm;
constexpr auto DepthFormat = wgpu::TextureFormat::Depth24Plus;

// The layout scene_render_target_layout() built before the HDR target existed.
RenderTargetLayout ldr_layout(uint32_t samples) {
  RenderTargetLayout layout{
      .colorAttachmentCount = 1,
      .colorAttachments = {{{
          .semantic = ColorAttachmentSemantic::SceneColor,
          .format = ColorFormat,
          .width = 640,
          .height = 480,
      }}},
      .depthStencilFormat = DepthFormat,
      .sampleCount = samples,
  };
  detail::finalize_render_target_layout(layout);
  return layout;
}

// Errors that escaped every error scope.
std::vector<std::string> g_uncapturedErrors;

class SceneHdrTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    const std::array instanceFeatures{wgpu::InstanceFeatureName::TimedWaitAny};
    const wgpu::InstanceDescriptor instanceDescriptor{
        .requiredFeatureCount = instanceFeatures.size(),
        .requiredFeatures = instanceFeatures.data(),
    };
    webgpu::g_instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!webgpu::g_instance) {
      return;
    }
    const wgpu::RequestAdapterOptions options{
        .featureLevel = wgpu::FeatureLevel::Core,
        .backendType = wgpu::BackendType::Null,
    };
    wgpu::Adapter adapter;
    webgpu::g_instance.WaitAny(
        webgpu::g_instance.RequestAdapter(
            &options, wgpu::CallbackMode::WaitAnyOnly,
            [&](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView) {
              if (status == wgpu::RequestAdapterStatus::Success) {
                adapter = std::move(result);
              }
            }),
        5000000000);
    if (!adapter) {
      return;
    }
    wgpu::DeviceDescriptor deviceDescriptor{};
    deviceDescriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
          g_uncapturedErrors.emplace_back(std::string_view{message});
        });
    webgpu::g_instance.WaitAny(
        adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                              [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView) {
                                if (status == wgpu::RequestDeviceStatus::Success) {
                                  webgpu::g_device = std::move(device);
                                }
                              }),
        5000000000);
    wgpu::Limits limits{};
    if (webgpu::g_device) {
      webgpu::g_device.GetLimits(&limits);
    }
    sMaxDimension = limits.maxTextureDimension2D;
  }

  static void TearDownTestSuite() {
    webgpu::g_device = {};
    webgpu::g_instance = {};
  }

  void SetUp() override {
    if (!webgpu::g_device) {
      GTEST_SKIP() << "no Dawn Null-backend device";
    }
    g_uncapturedErrors.clear();
    webgpu::g_hasCoreFeatures = true;
    webgpu::g_graphicsConfig.surfaceConfiguration.format = ColorFormat;
    webgpu::g_graphicsConfig.depthFormat = DepthFormat;
    make_efb(1);
    webgpu::release_scene_hdr_target();
    detail::testing::suppress_render_worker(true);
    // The Null device reports no offset alignment (all ones), which no uniform push can honour.
    savedUniformAlignment = detail::resources().limits.minUniformBufferOffsetAlignment;
    detail::resources().limits.minUniformBufferOffsetAlignment = 256;
    staging.attach(frame);
    detail::begin_recording(frame, 0);
    started = true;
    ASSERT_TRUE(bloom::ensure_task());
  }

  void TearDown() override {
    if (started && recording) {
      if (is_offscreen()) {
        end_offscreen();
      }
      finish();
      detail::end_recording();
    }
    if (started) {
      detail::shutdown_recording();
      detail::resources().limits.minUniformBufferOffsetAlignment = savedUniformAlignment;
    }
    bloom::shutdown();
    webgpu::release_scene_hdr_target();
    webgpu::g_frameBuffer = {};
    webgpu::g_frameBufferResolved = {};
    webgpu::g_depthBuffer = {};
    webgpu::g_hasCoreFeatures = false;
    webgpu::g_graphicsConfig.msaaSamples = 1;
    EXPECT_TRUE(g_uncapturedErrors.empty()) << g_uncapturedErrors.front();
  }

  // A real display EFB and depth buffer at `samples`, as resize_swapchain_internal makes them.
  static void make_efb(uint32_t samples) {
    webgpu::g_graphicsConfig.msaaSamples = samples;
    webgpu::g_frameBuffer = webgpu::create_render_texture(640, 480, true);
    webgpu::g_frameBufferResolved = webgpu::create_render_texture(640, 480, false);
    const wgpu::TextureDescriptor depthDescriptor{
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {640, 480, 1},
        .format = DepthFormat,
        .sampleCount = samples,
    };
    auto depth = webgpu::g_device.CreateTexture(&depthDescriptor);
    webgpu::g_depthBuffer = {
        .texture = depth,
        .view = depth.CreateView(),
        .size = {640, 480, 1},
        .format = DepthFormat,
    };
  }

  const detail::RenderPass& current_pass() const { return frames.back().renderPasses.back(); }

  // Ends the frame and starts the next on a fresh packet, with the EFB and HDR target released and
  // remade at `samples` as an MSAA change does.
  void next_frame(uint32_t samples) {
    finish();
    detail::end_recording();
    if (samples != webgpu::g_graphicsConfig.msaaSamples) {
      make_efb(samples);
      webgpu::release_scene_hdr_target();
    }
    frames.emplace_back();
    staging.attach(frames.back());
    detail::begin_recording(frames.back(), 0);
  }

  static inline uint32_t sMaxDimension = 0;
  // Declared before the packets, which wrap its memory.
  detail::testing::StagingPool staging;
  std::deque<detail::FramePacket> frames = std::deque<detail::FramePacket>(1);
  detail::FramePacket& frame = frames.front();
  bool started = false;
  bool recording = true;
  uint32_t savedUniformAlignment = 0;
};

TEST_F(SceneHdrTest, OffKeepsTheLdrLayoutAndKey) {
  for (const uint32_t samples : {1u, 4u}) {
    webgpu::g_graphicsConfig.msaaSamples = samples;
    const auto expected = ldr_layout(samples);
    const auto scene = scene_render_target_layout();
    const auto display = display_render_target_layout();
    EXPECT_FALSE(scene_hdr_active());
    EXPECT_EQ(scene.key, expected.key);
    EXPECT_EQ(std::memcmp(&scene, &expected, sizeof(scene)), 0);
    EXPECT_EQ(std::memcmp(&display, &expected, sizeof(display)), 0);
    EXPECT_EQ(color_format(), ColorFormat);
  }
  webgpu::g_graphicsConfig.msaaSamples = 1;
  EXPECT_EQ(frame.renderPasses.front().target_layout().key, ldr_layout(1).key);
}

TEST_F(SceneHdrTest, CompatibilityDeviceStillGetsTheHdrTarget) {
  webgpu::g_hasCoreFeatures = false;
  EXPECT_TRUE(webgpu::scene_hdr_supported());
  EXPECT_TRUE(webgpu::ensure_scene_hdr_target());
  EXPECT_TRUE(webgpu::g_sceneHdr.texture && webgpu::g_sceneHdrResolved.texture);
  EXPECT_EQ(webgpu::scene_hdr_sample_count(), 1u);
}

TEST_F(SceneHdrTest, AllocatesRealTexturesAtTheEfbSampleCount) {
  for (const uint32_t samples : {1u, 4u}) {
    make_efb(samples);
    ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
    ASSERT_TRUE(webgpu::g_sceneHdr.texture && webgpu::g_sceneHdr.view);
    ASSERT_TRUE(webgpu::g_sceneHdrResolved.texture && webgpu::g_sceneHdrResolved.view);
    EXPECT_EQ(webgpu::g_sceneHdr.texture.GetFormat(), webgpu::SceneHdrFormat);
    EXPECT_EQ(webgpu::g_sceneHdr.texture.GetSampleCount(), samples);
    EXPECT_EQ(webgpu::g_sceneHdr.texture.GetWidth(), 640u);
    EXPECT_EQ(webgpu::g_sceneHdr.texture.GetHeight(), 480u);
    EXPECT_EQ(webgpu::g_sceneHdrResolved.texture.GetSampleCount(), 1u);
    EXPECT_EQ(webgpu::g_sceneHdrResolved.texture.GetFormat(), webgpu::SceneHdrFormat);
    EXPECT_EQ(webgpu::scene_hdr_sample_count(), samples);
  }
}

TEST_F(SceneHdrTest, SampleCountChangeReallocates) {
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  const auto* first = webgpu::g_sceneHdr.texture.Get();
  // Same size and format, other sample count (no release in between): never reused.
  make_efb(4);
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  EXPECT_NE(webgpu::g_sceneHdr.texture.Get(), first);
  EXPECT_EQ(webgpu::g_sceneHdr.texture.GetSampleCount(), 4u);
  EXPECT_EQ(webgpu::scene_hdr_sample_count(), 4u);
}

TEST_F(SceneHdrTest, PartialOrStaleTargetIsReplaced) {
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  const auto* first = webgpu::g_sceneHdr.texture.Get();
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  EXPECT_EQ(webgpu::g_sceneHdr.texture.Get(), first);

  webgpu::g_sceneHdrResolved.view = {};
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  EXPECT_NE(webgpu::g_sceneHdr.texture.Get(), first);
  EXPECT_TRUE(webgpu::g_sceneHdrResolved.view);

  const auto* second = webgpu::g_sceneHdr.texture.Get();
  webgpu::g_sceneHdr.size = {320, 240, 1};
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  EXPECT_NE(webgpu::g_sceneHdr.texture.Get(), second);
  EXPECT_EQ(webgpu::g_sceneHdr.texture.GetWidth(), 640u);
}

TEST_F(SceneHdrTest, FailedAllocationIsCaughtAndReleased) {
  ASSERT_GT(sMaxDimension, 0u);
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  // An EFB size past the device limit: CreateTexture fails validation inside the error scopes.
  const auto realSize = webgpu::g_frameBuffer.size;
  webgpu::g_frameBuffer.size = {sMaxDimension + 1, 4, 1};
  EXPECT_FALSE(webgpu::ensure_scene_hdr_target());
  EXPECT_FALSE(webgpu::g_sceneHdr.texture);
  EXPECT_FALSE(webgpu::g_sceneHdr.view);
  EXPECT_FALSE(webgpu::g_sceneHdrResolved.texture);
  EXPECT_EQ(webgpu::scene_hdr_sample_count(), 0u);
  // The failed size is not retried; the switch refuses without touching the pass list.
  EXPECT_FALSE(webgpu::ensure_scene_hdr_target());
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_EQ(frame.renderPasses.size(), 1u);
  // A valid size allocates again.
  webgpu::g_frameBuffer.size = realSize;
  EXPECT_TRUE(webgpu::ensure_scene_hdr_target());
  EXPECT_TRUE(webgpu::g_sceneHdr.texture);
}

TEST_F(SceneHdrTest, HdrPreflightRequiresARegisteredPostBeforeAllocating) {
  bloom::shutdown();
  EXPECT_FALSE(bloom::scene_post_available());
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_FALSE(webgpu::g_sceneHdr.texture);
  EXPECT_EQ(frame.renderPasses.size(), 1u);

  // A pre-existing target does not make an unserviceable HDR scene safe to enter either.
  ASSERT_TRUE(webgpu::ensure_scene_hdr_target());
  const auto* target = webgpu::g_sceneHdr.texture.Get();
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_EQ(webgpu::g_sceneHdr.texture.Get(), target);
  EXPECT_EQ(frame.renderPasses.size(), 1u);
}

TEST_F(SceneHdrTest, RefusedOnAForeignOrNullTarget) {
  auto other = webgpu::create_render_texture(640, 480, true);
  auto& pass = frames.back().renderPasses.back();
  const auto efbView = pass.colorAttachments[SceneColorAttachmentIndex].view;
  pass.colorAttachments[SceneColorAttachmentIndex].view = other.view;
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_EQ(frame.renderPasses.size(), 1u);
  pass.colorAttachments[SceneColorAttachmentIndex].view = efbView;

  const auto depthView = webgpu::g_depthBuffer.view;
  webgpu::g_depthBuffer.view = {};
  EXPECT_FALSE(set_scene_hdr(true));
  webgpu::g_depthBuffer.view = depthView;

  const auto fbView = webgpu::g_frameBuffer.view;
  webgpu::g_frameBuffer.view = {};
  pass.colorAttachments[SceneColorAttachmentIndex].view = {};
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_EQ(frame.renderPasses.size(), 1u);
  webgpu::g_frameBuffer.view = fbView;
  pass.colorAttachments[SceneColorAttachmentIndex].view = efbView;
  EXPECT_TRUE(set_scene_hdr(true));
}

TEST_F(SceneHdrTest, SwitchSealsThePassAndRetargetsIt) {
  const auto ldrKey = current_pass().target_layout().key;
  EXPECT_EQ(current_pass().colorAttachments[SceneColorAttachmentIndex].view.Get(), webgpu::g_frameBuffer.view.Get());

  ASSERT_TRUE(set_scene_hdr(true));
  EXPECT_TRUE(scene_hdr_active());
  ASSERT_EQ(frame.renderPasses.size(), 2u);
  EXPECT_TRUE(frame.renderPasses[0].sealed);
  const auto& hdrPass = current_pass();
  const auto hdrLayout = hdrPass.target_layout();
  EXPECT_EQ(hdrLayout.colorAttachments[SceneColorAttachmentIndex].format, webgpu::SceneHdrFormat);
  EXPECT_EQ(hdrLayout.sampleCount, 1u);
  EXPECT_EQ(hdrLayout.depthStencilFormat, DepthFormat);
  EXPECT_NE(hdrLayout.key, ldrKey);
  EXPECT_EQ(hdrLayout.key, scene_render_target_layout().key);
  EXPECT_EQ(get_render_target_layout().key, hdrLayout.key);
  // The display EFB keeps its format; only the active scene target changed.
  EXPECT_EQ(display_render_target_layout().key, ldrKey);
  EXPECT_EQ(webgpu::g_frameBuffer.format, ColorFormat);
  EXPECT_EQ(color_format(), ColorFormat);
  // The first enable clears the HDR scene; depth carries over.
  EXPECT_TRUE(hdrPass.colorAttachments[SceneColorAttachmentIndex].clear);
  EXPECT_EQ(hdrPass.colorAttachments[SceneColorAttachmentIndex].loadOp, wgpu::LoadOp::Clear);
  EXPECT_FALSE(hdrPass.clearDepth);
  EXPECT_EQ(hdrPass.colorAttachments[SceneColorAttachmentIndex].view.Get(), webgpu::g_sceneHdr.view.Get());
  EXPECT_EQ(hdrPass.depthStencilView.Get(), webgpu::g_depthBuffer.view.Get());
  const auto* allocated = webgpu::g_sceneHdr.texture.Get();

  // Same request again: nothing sealed.
  EXPECT_TRUE(set_scene_hdr(true));
  EXPECT_EQ(frame.renderPasses.size(), 2u);

  ASSERT_TRUE(set_scene_hdr(false));
  ASSERT_EQ(frame.renderPasses.size(), 3u);
  EXPECT_EQ(current_pass().target_layout().key, ldrKey);
  EXPECT_FALSE(current_pass().colorAttachments[SceneColorAttachmentIndex].clear);

  EXPECT_EQ(current_pass().colorAttachments[SceneColorAttachmentIndex].view.Get(), webgpu::g_frameBuffer.view.Get());

  // A second enable in the frame loads what the first one drew, on the same (kept) target.
  ASSERT_TRUE(set_scene_hdr(true));
  EXPECT_EQ(webgpu::g_sceneHdr.texture.Get(), allocated);
  EXPECT_FALSE(current_pass().colorAttachments[SceneColorAttachmentIndex].clear);
  EXPECT_EQ(current_pass().target_layout().key, hdrLayout.key);
}

TEST_F(SceneHdrTest, PauseReusesColourAndClearsOnlyDepth) {
  ASSERT_TRUE(set_scene_hdr(true));
  const auto* texture = webgpu::g_sceneHdr.texture.Get();
  ASSERT_TRUE(set_scene_hdr(false));
  ASSERT_TRUE(set_scene_hdr(true, true));
  EXPECT_EQ(webgpu::g_sceneHdr.texture.Get(), texture);
  EXPECT_FALSE(current_pass().colorAttachments[SceneColorAttachmentIndex].clear);
  EXPECT_EQ(current_pass().colorAttachments[SceneColorAttachmentIndex].loadOp, wgpu::LoadOp::Load);
  EXPECT_TRUE(current_pass().clearDepth);
  EXPECT_EQ(current_pass().depthLoadOp, wgpu::LoadOp::Clear);
  EXPECT_EQ(current_pass().clearDepthValue, gx::UseReversedZ ? 0.f : 1.f);
}

TEST_F(SceneHdrTest, MsaaModesProduceDistinctKeys) {
  next_frame(4);
  const auto ldr4 = current_pass().target_layout();
  ASSERT_TRUE(set_scene_hdr(true));
  const auto hdr4 = current_pass().target_layout();
  EXPECT_EQ(hdr4.sampleCount, 4u);
  EXPECT_EQ(ldr4.key, ldr_layout(4).key);
  EXPECT_NE(hdr4.key, ldr4.key);
  EXPECT_NE(hdr4.key, ldr_layout(1).key);
  ASSERT_TRUE(set_scene_hdr(false));
  next_frame(1);
  ASSERT_TRUE(set_scene_hdr(true));
  EXPECT_EQ(webgpu::g_sceneHdr.texture.GetSampleCount(), 1u);
  EXPECT_NE(current_pass().target_layout().key, hdr4.key);
}

TEST_F(SceneHdrTest, ModeDoesNotLeakIntoTheNextFrame) {
  ASSERT_TRUE(set_scene_hdr(true));
  finish();
  detail::end_recording();
  EXPECT_FALSE(scene_hdr_active());

  frames.emplace_back();
  staging.attach(frames.back());
  detail::begin_recording(frames.back(), 0);
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_EQ(current_pass().target_layout().key, ldr_layout(1).key);

  ASSERT_TRUE(set_scene_hdr(true));
  detail::shutdown_recording();
  EXPECT_FALSE(scene_hdr_active());
  recording = false;
}

TEST_F(SceneHdrTest, RefusedOffscreenAndKeptAcrossIt) {
  detail::testing::seed_offscreen_cache(320, 180, ColorFormat, DepthFormat);
  begin_offscreen(320, 180);
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_EQ(current_pass().target_layout().colorAttachments[SceneColorAttachmentIndex].format, ColorFormat);
  end_offscreen();

  ASSERT_TRUE(set_scene_hdr(true));
  detail::testing::seed_offscreen_cache(320, 180, ColorFormat, DepthFormat);
  begin_offscreen(320, 180);
  // Offscreen targets keep their own (display) format.
  EXPECT_EQ(current_pass().target_layout().colorAttachments[SceneColorAttachmentIndex].format, ColorFormat);
  end_offscreen();
  EXPECT_TRUE(scene_hdr_active());
  EXPECT_EQ(current_pass().target_layout().colorAttachments[SceneColorAttachmentIndex].format,
            webgpu::SceneHdrFormat);
}

TEST(SceneHdrOutsideFrameTest, RefusedWithoutARecording) {
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_TRUE(set_scene_hdr(false));
}


// --- The HDR writers' WGSL and pipelines through Tint and Dawn's validation ------------------------------------

// The validation error a call raises, or empty.
template <typename F>
std::string validation_error(F&& f) {
  webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
  f();
  std::string error;
  webgpu::g_instance.WaitAny(webgpu::g_device.PopErrorScope(
                                 wgpu::CallbackMode::WaitAnyOnly,
                                 [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
                                   if (type != wgpu::ErrorType::NoError) {
                                     error = std::string_view{message};
                                     if (error.empty()) {
                                       error = "error";
                                     }
                                   }
                                 }),
                             5000000000);
  return error;
}

std::string module_error(const std::string& source) {
  return validation_error([&] {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = source.c_str();
    const wgpu::ShaderModuleDescriptor descriptor{.nextInChain = &wgsl, .label = "HDR writer test"};
    (void)webgpu::g_device.CreateShaderModule(&descriptor);
  });
}

gx::ShaderConfig writer_config(int pbr, int kind, bool hdr) {
  gx::ShaderConfig config;
  config.pbr = static_cast<u8>(pbr);
  config.pbrKind = static_cast<u8>(kind);
  config.sceneHdr = hdr;
  const bool screen = kind == 34 || kind == 36 || kind == 37;
  config.tevStageCount = 2;
  for (u32 i = 0; i < 2; ++i) {
    auto& stage = config.tevStages[i];
    stage.colorPass = {GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO};
    stage.alphaPass = {GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO};
    stage.texCoordId = static_cast<GXTexCoordID>(i);
    stage.texMapId = i == 0 || !screen ? GX_TEXMAP0 : GX_TEXMAP7;
    stage.channelId = GX_COLOR0A0;
  }
  for (const auto attr : {GX_VA_POS, GX_VA_NRM, GX_VA_TEX0, GX_VA_TEX1}) {
    auto& a = config.attrs[attr];
    a.attrType = GX_DIRECT;
    a.cnt = attr == GX_VA_TEX0 || attr == GX_VA_TEX1 ? 2 : 3;
    a.compType = GX_F32;
    a.stride = 12;
  }
  config.colorChannels[GX_COLOR0].lightingEnabled = true;
  config.colorChannels[GX_COLOR0].matSrc = GX_SRC_REG;
  config.colorChannels[GX_COLOR0].ambSrc = GX_SRC_REG;
  return config;
}

TEST_F(SceneHdrTest, InvalidWgslIsCaught) {
  EXPECT_FALSE(module_error("fn f() -> f32 { return vec3f(1.0); }").empty());
  EXPECT_TRUE(module_error("fn f() -> f32 { return 1.0; }").empty());
}

TEST_F(SceneHdrTest, GxWritersCompileInBothModes) {
  // TEV (pbr 0) and the PBR kinds the HDR branch rewrites.
  const std::pair<int, int> kinds[] = {{0, 0}, {1, 0}, {1, 1}, {1, 34}, {1, 36}, {1, 37}};
  for (const auto [pbr, kind] : kinds) {
    for (const bool hdr : {false, true}) {
      SCOPED_TRACE(testing::Message() << "pbr " << pbr << " kind " << kind << " hdr " << hdr);
      const std::string source = gx::build_shader_source(writer_config(pbr, kind, hdr));
      ASSERT_FALSE(source.empty());
      EXPECT_EQ(module_error(source), "");
    }
  }
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

// The config features that add their own WGSL around the writers' output, each on TEV and PBR.
TEST_F(SceneHdrTest, GxWriterVariantsCompileInBothModes) {
  struct Variant {
    const char* name;
    bool tev;
    bool pbr;
    void (*apply)(gx::ShaderConfig&);
  };
  const Variant variants[] = {
      {"fog", true, true, [](gx::ShaderConfig& c) { c.fogType = GX_FOG_PERSP_LIN; }},
      {"fog range", true, true,
       [](gx::ShaderConfig& c) {
         c.fogType = GX_FOG_PERSP_EXP2;
         c.fogRangeEnabled = true;
       }},
      // Volumetric fog: in HDR, vf_apply_hdr fogs the converted light (scene_hdr_water_fog_test).
      {"volfog blended", true, true, [](gx::ShaderConfig& c) { c.volFog = gx::VolFogBlended; }},
      {"volfog additive", true, true, [](gx::ShaderConfig& c) { c.volFog = gx::VolFogAdditive; }},
      {"volfog opaque", true, true, [](gx::ShaderConfig& c) { c.volFog = gx::VolFogOpaque; }},
      {"volfog premultiplied", true, true, [](gx::ShaderConfig& c) { c.volFog = gx::VolFogPremultiplied; }},
      {"fog + volfog", true, true,
       [](gx::ShaderConfig& c) {
         c.fogType = GX_FOG_PERSP_LIN;
         c.volFog = gx::VolFogBlended;
       }},
      {"sdf", true, false, [](gx::ShaderConfig& c) { c.sdf = 128; }},
      {"drawid debug", true, true, [](gx::ShaderConfig& c) { c.drawId = true; }},
      {"lightmap", false, true, [](gx::ShaderConfig& c) { c.pbrLightmapAttr = GX_VA_TEX1; }},
      {"sun shadow", true, true, [](gx::ShaderConfig& c) { c.shadow = true; }},
      {"sun + lightmap + fog", false, true,
       [](gx::ShaderConfig& c) {
         c.shadow = true;
         c.pbrLightmapAttr = GX_VA_TEX1;
         c.fogType = GX_FOG_PERSP_LIN;
       }},
  };
  size_t compiled = 0;
  for (const auto& v : variants) {
    for (const int pbr : {0, 1}) {
      if (!(pbr ? v.pbr : v.tev)) {
        continue;
      }
      for (const int kind : pbr ? std::initializer_list<int>{0, 36} : std::initializer_list<int>{0}) {
        for (const bool hdr : {false, true}) {
          SCOPED_TRACE(testing::Message() << v.name << " pbr " << pbr << " kind " << kind << " hdr " << hdr);
          gx::ShaderConfig config = writer_config(pbr, kind, hdr);
          v.apply(config);
          const std::string source = gx::build_shader_source(config);
          ASSERT_FALSE(source.empty());
          EXPECT_EQ(source.find("fn hdr_exposed(") != std::string::npos, hdr);
          EXPECT_EQ(module_error(source), "");
          ++compiled;
        }
      }
    }
  }
  EXPECT_EQ(compiled, 2u * (9 * 3 + 2 * 2 + 1)); // variants on TEV and PBR (2 kinds), PBR only, TEV only
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

// Water and the full-screen fog's apply pass compile on the HDR scene (stage 3B).
TEST_F(SceneHdrTest, WaterAndFogApplyCompile) {
  size_t compiled = 0;
  for (const uint32_t flags : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(testing::Message() << "water flags " << flags);
    water::PipelineConfig config{};
    config.colorAttachmentCount = 1;
    config.colorFormats[0] = webgpu::SceneHdrFormat;
    config.depthStencilFormat = wgpu::TextureFormat::Depth24Plus;
    config.flags = flags | water::WaterSceneHdr;
    const std::string source = water::pipeline_source(config);
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(module_error(source), "");
    ++compiled;
  }
  for (const uint32_t samples : {1u, 4u}) {
    SCOPED_TRACE(testing::Message() << "volfog samples " << samples);
    const std::string source = volfog::apply_source(samples);
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(module_error(source), "");
    ++compiled;
  }
  // The visor posts read and write the HDR scene's exposed linear light as it is (kb
  // topic/visor-hdr-pipeline.md).
  {
    SCOPED_TRACE("thermal");
    const std::string source = thermal::source();
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(module_error(source), "");
    EXPECT_EQ(source.find("untone"), std::string::npos);
    ++compiled;
  }
  for (const uint32_t samples : {1u, 4u}) {
    SCOPED_TRACE(testing::Message() << "xray samples " << samples);
    const std::string source = xray::source_for(samples);
    ASSERT_FALSE(source.empty());
    EXPECT_EQ(module_error(source), "");
    EXPECT_EQ(source.find("untone"), std::string::npos);
    ++compiled;
  }
  EXPECT_EQ(compiled, 4u + 2u + 1u + 2u);
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

// --- Copies and snapshots of the HDR scene (stage 3A) ---------------------------------------------------------------

constexpr hdr::Tone CopyTone{{{20.f, 3.f, 0.5f, 1.5f}, {0.8f, 0.02f, 0.1f, 1.f}, {0.18f, 1.f, -1.f, 0.82f}}};

TEST_F(SceneHdrTest, CopiesCarryTheirDomainAndClearInTheirScene) {
  // The whole target: the clear is the continuation's load op.
  const ClipRect rect{0, 0, 640, 480};
  const Vec4<float> clear{0.5f, 0.25f, 0.f, 0.75f};
  const ResolveHdr display{.tone = CopyTone, .linear = false};
  const ResolveHdr linear{.tone = CopyTone, .linear = true};
  const auto copy = [&](const ResolveHdr* hdr, Vec4<float> value) {
    resolve_pass_into({}, rect, true, true, false, value, 1.f, GX_TF_RGB565, -1, false, hdr);
    return frames.back().renderPasses.size();
  };
  const auto& sealed = [&]() -> const detail::RenderPass& { return frames.back().renderPasses[frames.back().renderPasses.size() - 2]; };
  const auto clearValue = [&] { return current_pass().colorAttachments[SceneColorAttachmentIndex].clearValue; };

  // LDR: the old copy and clear, whatever the request.
  for (const auto* hdr : {static_cast<const ResolveHdr*>(nullptr), &display, &linear}) {
    copy(hdr, clear);
    EXPECT_EQ(sealed().resolveDomain, tex_copy_conv::CopyDomain::Display);
    EXPECT_EQ(sealed().resolveToneRange.size, 0u);
    EXPECT_EQ(clearValue().x(), clear.x());
    EXPECT_EQ(clearValue().y(), clear.y());
    EXPECT_EQ(clearValue().w(), clear.w());
  }

  ASSERT_TRUE(set_scene_hdr(true));
  copy(&display, clear);
  EXPECT_EQ(sealed().resolveDomain, tex_copy_conv::CopyDomain::HdrDisplay);
  EXPECT_EQ(sealed().resolveToneRange.size, sizeof(hdr::Tone));
  // The clear (display-referred) is the light that post-tones back to it; alpha as is.
  EXPECT_FLOAT_EQ(clearValue().x(), hdr::exposed(clear.x(), CopyTone));
  EXPECT_FLOAT_EQ(clearValue().y(), hdr::exposed(clear.y(), CopyTone));
  EXPECT_FLOAT_EQ(clearValue().z(), 0.f);
  EXPECT_EQ(clearValue().w(), clear.w());
  EXPECT_NEAR(hdr::post_tone(clearValue().x(), CopyTone), clear.x(), 1e-4f);

  copy(&linear, clear);
  EXPECT_EQ(sealed().resolveDomain, tex_copy_conv::CopyDomain::Linear);
  EXPECT_FLOAT_EQ(clearValue().x(), hdr::exposed(clear.x(), CopyTone));

  // Black stays black.
  copy(&display, {0.f, 0.f, 0.f, 0.f});
  EXPECT_EQ(clearValue().x(), 0.f);
  EXPECT_EQ(clearValue().y(), 0.f);
  EXPECT_EQ(clearValue().z(), 0.f);
  EXPECT_EQ(clearValue().w(), 0.f);
  EXPECT_TRUE(set_scene_hdr(false));

  // Part of it: the load op keeps the rest, a clear draw clears the rectangle.
  resolve_pass_into({}, {0, 0, 64, 32}, true, true, false, clear, 1.f, GX_TF_RGB565, -1, false, nullptr);
  EXPECT_FALSE(current_pass().colorAttachments[SceneColorAttachmentIndex].clear);
}

TEST_F(SceneHdrTest, SnapshotsTakeTheActiveAttachmentsFormat) {
  for (const uint32_t samples : {1u, 4u}) {
    SCOPED_TRACE(samples);
    if (samples != webgpu::g_graphicsConfig.msaaSamples) {
      next_frame(samples);
    }
    ResolvedTargets out;
    ASSERT_TRUE(resolve_pass({.color = true}, out));
    EXPECT_EQ(out.colorFormat, ColorFormat);
    ASSERT_TRUE(set_scene_hdr(true));
    ASSERT_TRUE(resolve_pass({.color = true}, out));
    EXPECT_EQ(out.colorFormat, webgpu::SceneHdrFormat);
    ASSERT_TRUE(out.color);
    // Back in LDR the same frame: a display snapshot again, not the HDR one.
    ASSERT_TRUE(set_scene_hdr(false));
    ASSERT_TRUE(resolve_pass({.color = true}, out));
    EXPECT_EQ(out.colorFormat, ColorFormat);
  }
}

TEST_F(SceneHdrTest, CopyDomainPipelinesValidate) {
  tex_copy_conv::initialize();
  auto& res = detail::resources();
  const auto savedUniforms = res.uniformBuffer;
  const wgpu::BufferDescriptor uniformDescriptor{
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = 1024,
  };
  res.uniformBuffer = webgpu::g_device.CreateBuffer(&uniformDescriptor);
  const wgpu::TextureDescriptor srcDescriptor{
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .size = {64, 32, 1},
      .format = webgpu::SceneHdrFormat,
  };
  const auto src = webgpu::g_device.CreateTexture(&srcDescriptor);
  const auto srcView = src.CreateView();
  const auto encode = [&](const tex_copy_conv::ConvRequest& req, bool blit) {
    return validation_error([&] {
      const auto cmd = webgpu::g_device.CreateCommandEncoder();
      if (blit) {
        tex_copy_conv::blit(cmd, req);
      } else {
        tex_copy_conv::run(cmd, req);
      }
      (void)cmd.Finish();
    });
  };
  const std::array convFormats{GX_TF_I4,   GX_TF_I8,   GX_TF_IA4,  GX_TF_IA8,  GX_TF_RGB565, GX_CTF_R4,  GX_CTF_RA4,
                               GX_CTF_RA8, GX_CTF_A8,  GX_CTF_R8,  GX_CTF_G8,  GX_CTF_B8,    GX_CTF_RG8, GX_CTF_GB8};
  for (const auto fmt : convFormats) {
    SCOPED_TRACE(static_cast<int>(fmt));
    const tex_copy_conv::ConvRequest req{
        .fmt = fmt,
        .srcView = srcView,
        .uniformRange = {0, sizeof(tex_copy_conv::Uniforms)},
        .dst = new_conv_texture(64, 32, fmt, "HDR copy test"),
        .domain = tex_copy_conv::CopyDomain::HdrDisplay,
        .toneRange = {256, sizeof(hdr::Tone)},
    };
    EXPECT_EQ(encode(req, false), "");
  }
  // HdrDisplay plain copies (blit), and both linear pipelines into RGBA16F.
  const tex_copy_conv::ConvRequest display{
      .fmt = GX_TF_RGBA8,
      .srcView = srcView,
      .uniformRange = {0, sizeof(tex_copy_conv::Uniforms)},
      .dst = new_render_texture(64, 32, GX_TF_RGBA8, "HDR copy test"),
      .domain = tex_copy_conv::CopyDomain::HdrDisplay,
      .toneRange = {256, sizeof(hdr::Tone)},
  };
  EXPECT_EQ(encode(display, true), "");
  for (const auto fmt : {GX_TF_RGB565, GX_TF_RGBA8}) {
    SCOPED_TRACE(static_cast<int>(fmt));
    const tex_copy_conv::ConvRequest req{
        .fmt = fmt,
        .srcView = srcView,
        .uniformRange = {0, sizeof(tex_copy_conv::Uniforms)},
        .dst = new_render_texture(64, 32, fmt, "Linear copy test", 1, webgpu::SceneHdrFormat),
        .domain = tex_copy_conv::CopyDomain::Linear,
    };
    EXPECT_EQ(req.dst->format, webgpu::SceneHdrFormat);
    EXPECT_EQ(encode(req, fmt == GX_TF_RGBA8), "");
  }
  tex_copy_conv::shutdown();
  res.uniformBuffer = savedUniforms;
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

// Every map-7 screen read of a PBR kind: in HDR it reads the linear copy raw, in LDR it decodes the
// display copy as before. Only those reads change; the base and cube maps keep their decodes.
gx::ShaderConfig screen_config(int kind, bool hdr) {
  gx::ShaderConfig config = writer_config(1, kind, hdr);
  config.tevStageCount = 8;
  for (u32 i = 0; i < 8; ++i) {
    auto& stage = config.tevStages[i];
    stage = config.tevStages[0];
    stage.texCoordId = static_cast<GXTexCoordID>(i % 2);
    stage.texMapId = static_cast<GXTexMapID>(i);
  }
  return config;
}

size_t count(std::string_view source, std::string_view what) {
  size_t n = 0;
  for (size_t at = source.find(what); at != std::string_view::npos; at = source.find(what, at + what.size())) {
    // Whole identifiers only: hdr_srgb_dec( is not srgb_dec(.
    n += at == 0 || !(std::isalnum(static_cast<unsigned char>(source[at - 1])) || source[at - 1] == '_');
  }
  return n;
}

constexpr std::string_view ScreenDecodes[] = {"srgb_dec(pbr_gs)", "srgb_dec(pbr_hg)", "srgb_dec(pbr_sfb)",
                                              "srgb_dec(textureSampleLevel(tex7, tex7_samp,"};

// The screen-copy kinds fog in HDR as their Remastered fog perms do (kb topic/hdr-pbr-pass.md): the frame a
// blend keeps (23/30 transmission, 29 dual and 11's o1) is never dimmed, each kind weights the in-scatter its own
// way, and 14 (no fog perm) is not fogged. LDR keeps its single form.
TEST_F(SceneHdrTest, ScreenKindsFogAsTheirFogPermsInHdr) {
  for (const int kind : {8, 11, 14, 23, 29, 30}) {
    SCOPED_TRACE(kind);
    auto ldrConfig = screen_config(kind, false);
    auto hdrConfig = screen_config(kind, true);
    ldrConfig.volFog = hdrConfig.volFog = kind == 23 || kind == 30 ? gx::VolFogOpaque : gx::VolFogBlended;
    const std::string ldr = gx::build_shader_source(ldrConfig);
    const std::string hdr = gx::build_shader_source(hdrConfig);
    EXPECT_EQ(count(ldr, "pbr_keep"), 0u);
    EXPECT_EQ(count(hdr, "select(vec3f(1.0), pbr_vfw, pbr_vfk);"), 1u);
    EXPECT_EQ(count(hdr, "pbr_vf = vec4f(0.0, 0.0, 0.0, 1.0);"), 1u);
    EXPECT_EQ(count(hdr, "prev = vec4f(pbr_lin + pbr_pass + pbr_keep, pbr_alpha);"), 1u);
    if (kind == 14) {
      // Anchored on 14's own pass: the EnergyField branch (kind 44) also turns the fog off.
      EXPECT_EQ(count(hdr, "pbr_pass = pbr_sfb * pbr_c4.x * pbr_vraw.rgb;\n          pbr_vfon = false;"), 1u);
    }
    if (kind == 23 || kind == 30) {
      EXPECT_EQ(count(hdr, "pbr_keep = pbr_xw * pbr_xscene;"), 1u);
      EXPECT_EQ(count(hdr, "pbr_vfw = vec3f(1.0) - pbr_xw;"), 1u);
    }
    if (kind == 11) {
      // MFC0 on (03407341's MTRL tail, as 2f95a061's): the blend is One / InvSrc1Color, o1 = DIFC.a k, so the
      // frame is kept by 1 - o1, never by o1, and DIFC scales the lit and bent terms.
      EXPECT_EQ(count(hdr, "pbr_pass = pbr_hg * ubuf.pbr_emissive.rgb * ubuf.pbr_param.w * pbr_df.rgb * pbr_df.a;"), 1u);
      EXPECT_EQ(count(hdr, "pbr_vfw = pbr_df.a * (vec3f(1.0) - ubuf.pbr_emissive.rgb * (1.0 - pbr_vraw.a));"), 1u);
      // Kinds 11 and 29 share one source (branched on pbr_kind), each with its own keep line.
      EXPECT_EQ(count(hdr, "pbr_keep = pbr_sdst * (vec3f(1.0) - pbr_vfw);"), 2u);
      EXPECT_EQ(count(ldr, "pbr_sdst * (vec3f(1.0) - pbr_df.a * (vec3f(1.0) - ubuf.pbr_emissive.rgb * (1.0 - pbr_vraw.a)));"),
                1u);
      EXPECT_EQ(count(ldr, "pbr_sdst * (pbr_df.a *"), 0u);
    }
    if (kind == 29) {
      // Kinds 11 and 29 share one source (branched on pbr_kind), each with its own keep line.
      EXPECT_EQ(count(hdr, "pbr_keep = pbr_sdst * (vec3f(1.0) - pbr_vfw);"), 2u);
    }
    EXPECT_EQ(module_error(ldr), "");
    EXPECT_EQ(module_error(hdr), "");
  }
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

TEST_F(SceneHdrTest, ScreenReadsAreRawOnlyInHdr) {
  for (const int kind : {8, 11, 14, 23, 29, 30, 34, 36}) {
    SCOPED_TRACE(kind);
    const std::string ldr = gx::build_shader_source(screen_config(kind, false));
    const std::string hdr = gx::build_shader_source(screen_config(kind, true));
    ASSERT_FALSE(ldr.empty());
    size_t screen = 0;
    for (const auto what : ScreenDecodes) {
      screen += count(ldr, what);
      EXPECT_EQ(count(hdr, what), 0u) << what;
    }
    EXPECT_GT(screen, 0u) << "the kind reads map 7";
    // Every other decode stays (base and cube maps).
    EXPECT_EQ(count(hdr, "srgb_dec("), count(ldr, "srgb_dec(") - screen);
    // Kind 11 reads the frame its blend keeps (unbent, at the fragment) in both, as 29's dual does.
    EXPECT_EQ(count(hdr, "textureSampleLevel(tex7, tex7_samp,"), count(ldr, "textureSampleLevel(tex7, tex7_samp,"));
    EXPECT_EQ(module_error(ldr), "");
    EXPECT_EQ(module_error(hdr), "");
  }
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

TEST_F(SceneHdrTest, VfxWritersBuildHdrPipelines) {
  for (const bool hdr : {false, true}) {
    for (const uint32_t blend : {0u, 1u, 2u, 3u, 4u, 5u}) {
      SCOPED_TRACE(testing::Message() << "blend " << blend << " hdr " << hdr);
      vfx::PipelineConfig config;
      config.colorFormats[0] = hdr ? webgpu::SceneHdrFormat : ColorFormat;
      config.depthStencilFormat = DepthFormat;
      config.colorAttachmentCount = 1;
      config.features = 1; // ColorTex
      config.slotKey = 0x1ffff8u; // colour from slot 0, every other read unbound
      config.blend = blend | (hdr ? vfx::BlendSceneHdr : 0u);
      config.compare = static_cast<uint32_t>(wgpu::CompareFunction::LessEqual);
      wgpu::RenderPipeline pipeline;
      EXPECT_EQ(validation_error([&] { pipeline = vfx::create_pipeline(config); }), "");
      EXPECT_TRUE(pipeline);
    }
  }
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

// VFX_IceChargeCollision_Lit's pipeline: the WGSL the clustered gather, the probe and the light loop add
// has to build in both targets (the port draws particles into the HDR scene, and an LDR run of the same
// material into the display EFB).
TEST_F(SceneHdrTest, LitVfxPipelineBuildsInBothTargets) {
  // The slots the Lit recipe carries: BCLR, NMAP, TCH0.
  static constexpr int8_t Slots[] = {0, 7, 7, 7, 7, 7, 7, 1, 2};
  uint32_t slotKey = 0;
  for (uint32_t i = 0; i < std::size(Slots); ++i) {
    slotKey |= uint32_t(Slots[i]) << (3 * i);
  }
  for (const bool hdr : {false, true}) {
    SCOPED_TRACE(testing::Message() << "hdr " << hdr);
    vfx::PipelineConfig config;
    config.colorFormats[0] = hdr ? webgpu::SceneHdrFormat : ColorFormat;
    config.depthStencilFormat = DepthFormat;
    config.colorAttachmentCount = 1;
    config.features = vfx::IceChargeCollision | vfx::IceChargeCollisionLit | vfx::Unfogged;
    config.slotKey = slotKey;
    config.blend = 2 | (hdr ? vfx::BlendSceneHdr : 0u); // PBDM 2: additive
    config.compare = static_cast<uint32_t>(wgpu::CompareFunction::LessEqual);
    EXPECT_EQ(module_error(vfx::pipeline_source(config)), "");
    wgpu::RenderPipeline pipeline;
    EXPECT_EQ(validation_error([&] { pipeline = vfx::create_pipeline(config); }), "");
    EXPECT_TRUE(pipeline);
  }
  EXPECT_TRUE(g_uncapturedErrors.empty());
}

// The HDR scene's post (GXPortPostProcessScene): what it records and where the FIFO goes on after it.
bloom::Params scene_post() {
  return bloom::Params{};
}

// The composite's damage-tint term (Remastered UberPost 0007259, slot A): clamp(frame) +
// clamp(c).rgb * clamp(c).a in linear light. The Null backend runs no shader, so this checks the
// shared WGSL through Tint and the wire layout, and the term itself against a CPU mirror.
TEST_F(SceneHdrTest, CompositeAddTermCompilesAndKeepsItsWireLayout) {
  static_assert(sizeof(bloom::Params) == 49 * sizeof(uint32_t));
  static_assert(offsetof(bloom::Params, add) == 32 * sizeof(uint32_t));
  static_assert(offsetof(bloom::Params, radial) == 36 * sizeof(uint32_t));
  static_assert(offsetof(bloom::Params, radialOn) == 48 * sizeof(uint32_t));
  const std::string source = std::string{bloom::CompositeAddWgsl} +
                             "@compute @workgroup_size(1) fn main() { let v = composite_add(vec3f(0.5), vec4f(1.0, 0.5, 0.0, 0.5)); _ = v; }";
  EXPECT_EQ(module_error(source), "");
  EXPECT_NE(std::string{bloom::CompositeAddWgsl}.find("clamp(frame, vec3f(0.0), vec3f(1.0)) + t.rgb * t.a"),
            std::string::npos);
  // No tint recorded: the default params add nothing (alpha 0), so the shader leaves the frame alone.
  EXPECT_EQ(scene_post().add[3], 0.f);
}

TEST(CompositeAddMath, MatchesTheTracedShader) {
  const auto term = [](const float frame[3], const float c[4], float out[3]) {
    const auto clamp01 = [](float v) { return std::min(std::max(v, 0.f), 1.f); };
    const float a = clamp01(c[3]);
    for (int i = 0; i < 3; ++i) {
      out[i] = clamp01(frame[i]) + clamp01(c[i]) * a;
    }
  };
  float out[3];
  // The frame is clamped before the add, so it can exceed 1 only by the tint.
  const float f1[3] = {1.4f, 0.2f, -0.1f};
  const float c1[4] = {1.f, 0.5f, 0.25f, 0.5f};
  term(f1, c1, out);
  EXPECT_FLOAT_EQ(out[0], 1.5f);
  EXPECT_FLOAT_EQ(out[1], 0.45f);
  EXPECT_FLOAT_EQ(out[2], 0.125f);
  // The colour clamps before it is weighted by its alpha.
  const float f2[3] = {0.f, 0.f, 0.f};
  const float c2[4] = {2.f, -1.f, 0.5f, 3.f};
  term(f2, c2, out);
  EXPECT_FLOAT_EQ(out[0], 1.f);
  EXPECT_FLOAT_EQ(out[1], 0.f);
  EXPECT_FLOAT_EQ(out[2], 0.5f);
}

TEST_F(SceneHdrTest, CompositeRadialTermCompiles) {
  const std::string source =
      std::string{bloom::CompositeRadialWgsl} +
      "@compute @workgroup_size(1) fn main() { let v = composite_radial(vec4f(0.5), vec2f(0.1, -0.2), "
      "vec4f(1.0), vec4f(0.0), vec4f(1.0)); _ = v; }";
  EXPECT_EQ(module_error(source), "");
  // No fan recorded: the default params leave the radial term off.
  EXPECT_EQ(scene_post().radialOn, 0u);
}

// CompositeRadialWgsl line for line on the CPU (module_error only compiles WGSL), checked against
// the trace: apex c0 at the centre, rim c1 at (6.25 sin a, 3.6 cos a), the clamped frame times c2.
TEST(CompositeRadialMath, MatchesTheTracedFan) {
  const std::string wgsl{bloom::CompositeRadialWgsl};
  EXPECT_NE(wgsl.find("let q = ndc / vec2f(6.25, 3.6);"), std::string::npos);
  EXPECT_NE(wgsl.find("clamp(frame.rgb, vec3f(0.0), vec3f(1.0)) * m.rgb + v.rgb * v.a, frame.a * m.a"),
            std::string::npos);
  const float pi = 3.14159265358979f;
  const auto clamp01 = [](float v) { return std::min(std::max(v, 0.f), 1.f); };
  const auto term = [&](const float ndc[2], const float frame[4], const float c0[4], const float c1[4],
                        const float c2[4], float out[4]) {
    const float sector = pi / 8.f;
    const float q[2] = {ndc[0] / 6.25f, ndc[1] / 3.6f};
    float a = std::atan2(q[0], q[1]);
    if (a < 0.f) {
      a += 2.f * pi;
    }
    const float k = std::min(std::floor(a / sector), 15.f);
    const float mid = (k + 0.5f) * sector;
    const float t = (q[0] * std::sin(mid) + q[1] * std::cos(mid)) / std::cos(sector * 0.5f);
    float v[4] = {};
    if (t <= 1.f) {
      for (int i = 0; i < 4; ++i) {
        v[i] = clamp01(c0[i] + (c1[i] - c0[i]) * t);
      }
    }
    for (int i = 0; i < 3; ++i) {
      out[i] = clamp01(frame[i]) * clamp01(c2[i]) + v[i] * v[3];
    }
    out[3] = frame[3] * clamp01(c2[3]);
  };
  struct Case {
    float ndc[2];
    float frame[4];
    float c0[4], c1[4], c2[4];
    float want[4];
  };
  const float apothem = std::cos(pi / 16.f);
  const Case cases[] = {
      // The centre is the apex: c0 by its alpha, the frame clamped.
      {{0.f, 0.f}, {2.f, 0.5f, -1.f, 1.f}, {1.f, 0.5f, 0.f, 0.5f}, {0.f, 0.f, 1.f, 1.f}, {1.f, 1.f, 1.f, 1.f},
       {1.5f, 0.75f, 0.f, 1.f}},
      // A rim vertex (a = 0: (0, 3.6)) is c1; so is a = 5 pi/8 at (6.25 sin a, 3.6 cos a).
      {{0.f, 3.6f}, {0.f, 0.f, 0.f, 1.f}, {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 0.5f}, {1.f, 1.f, 1.f, 1.f},
       {0.f, 0.5f, 0.f, 1.f}},
      {{6.25f * std::sin(5.f * pi / 8.f), 3.6f * std::cos(5.f * pi / 8.f)}, {0.f, 0.f, 0.f, 1.f},
       {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 0.5f}, {1.f, 1.f, 1.f, 1.f}, {0.f, 0.5f, 0.f, 1.f}},
      // Half way to the middle of an edge (a = pi/16): an even mix of c0 and c1.
      {{0.5f * apothem * 6.25f * std::sin(pi / 16.f), 0.5f * apothem * 3.6f * std::cos(pi / 16.f)},
       {0.f, 0.f, 0.f, 1.f}, {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {1.f, 1.f, 1.f, 1.f},
       {0.5f, 0.5f, 0.f, 1.f}},
      // The mixed colour clamps after the interpolation (c0 below 0, as VEMC's -0.1 blue).
      {{0.f, 0.f}, {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, -0.1f, 1.f}, {0.f, 0.f, 0.f, 1.f}, {1.f, 1.f, 1.f, 1.f},
       {0.f, 0.f, 0.f, 1.f}},
      // c2 multiplies the clamped frame (and its alpha) before the fan is added.
      {{0.f, 0.f}, {0.8f, 1.5f, 0.4f, 0.5f}, {0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}, {0.5f, 2.f, -1.f, 0.5f},
       {0.4f, 1.f, 0.f, 0.25f}},
  };
  for (const Case& c : cases) {
    float out[4];
    term(c.ndc, c.frame, c.c0, c.c1, c.c2, out);
    for (int i = 0; i < 4; ++i) {
      EXPECT_NEAR(out[i], c.want[i], 1e-4f) << "case at (" << c.ndc[0] << ", " << c.ndc[1] << ") [" << i << "]";
    }
  }
}

TEST_F(SceneHdrTest, ScenePostHandsTheSceneToTheDisplayEfb) {
  bloom::shutdown();
  ASSERT_TRUE(bloom::ensure_task());
  ASSERT_TRUE(set_scene_hdr(true));
  const size_t passes = frame.renderPasses.size();
  const size_t tasks = frame.encoderTasks.size();

  ASSERT_TRUE(bloom::record(scene_post()));
  EXPECT_FALSE(scene_hdr_active());
  EXPECT_FALSE(scene_hdr_pass_active());
  EXPECT_EQ(frame.encoderTasks.size(), tasks + 1);
  ASSERT_GT(frame.renderPasses.size(), passes);
  EXPECT_TRUE(frame.renderPasses[passes - 1].sealed);
  // The FIFO resumes on the display EFB at its own format, overwritten by the composite.
  const auto& pass = current_pass();
  EXPECT_EQ(pass.colorAttachments[SceneColorAttachmentIndex].view.Get(), webgpu::g_frameBuffer.view.Get());
  EXPECT_EQ(pass.target_layout().colorAttachments[SceneColorAttachmentIndex].format, ColorFormat);
  EXPECT_EQ(pass.target_layout().key, display_render_target_layout().key);
  EXPECT_EQ(pass.depthStencilView.Get(), webgpu::g_depthBuffer.view.Get());
  // The composite is its first draw (only viewport and scissor state before it).
  const auto first = std::find_if(pass.commands.begin(), pass.commands.end(), [](const auto& c) {
    return c.type == detail::CommandType::Draw || c.type == detail::CommandType::CustomDraw;
  });
  ASSERT_NE(first, pass.commands.end());
  EXPECT_EQ(first->type, detail::CommandType::CustomDraw);
  EXPECT_EQ(first->data.customDraw.payloadSize, sizeof(uint32_t));
  // The RGBA16F scene itself is left as it was.
  EXPECT_EQ(webgpu::g_sceneHdr.format, webgpu::SceneHdrFormat);
  bloom::shutdown();
}

TEST_F(SceneHdrTest, ScenePostStartsTheNextWorldSceneWithFreshBlack) {
  ASSERT_TRUE(set_scene_hdr(true));
  EXPECT_TRUE(current_pass().colorAttachments[SceneColorAttachmentIndex].clear);
  ASSERT_TRUE(bloom::record(scene_post()));
  EXPECT_FALSE(scene_hdr_active());
  // A scan-zoom world is posted before the main world. Each is a separate HDR scene, so the
  // second one clears black instead of loading the first scene's still-allocated RGBA16F target.
  ASSERT_TRUE(set_scene_hdr(true));
  const auto& nextScene = current_pass().colorAttachments[SceneColorAttachmentIndex];
  EXPECT_TRUE(nextScene.clear);
  EXPECT_EQ(nextScene.loadOp, wgpu::LoadOp::Clear);
  EXPECT_FLOAT_EQ(nextScene.clearValue.x(), 0.f);
  EXPECT_FLOAT_EQ(nextScene.clearValue.y(), 0.f);
  EXPECT_FLOAT_EQ(nextScene.clearValue.z(), 0.f);
  EXPECT_FLOAT_EQ(nextScene.clearValue.w(), 0.f);
  bloom::shutdown();
}

TEST_F(SceneHdrTest, ScenePostOnTheDisplayEfbRecordsNothing) {
  bloom::shutdown();
  ASSERT_TRUE(bloom::ensure_task());
  const size_t passes = frame.renderPasses.size();
  const size_t tasks = frame.encoderTasks.size();
  EXPECT_FALSE(bloom::record(scene_post()));
  EXPECT_EQ(frame.renderPasses.size(), passes);
  EXPECT_EQ(frame.encoderTasks.size(), tasks);
  bloom::shutdown();
}

TEST_F(SceneHdrTest, ThirdHdrSegmentIsRefused) {
  bloom::shutdown();
  ASSERT_TRUE(bloom::ensure_task());
  EXPECT_TRUE(bloom::scene_post_available());
  EXPECT_TRUE(bloom::scene_post_available()); // preflight does not consume a slot
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(set_scene_hdr(true));
    EXPECT_TRUE(bloom::record(scene_post())) << i;
    EXPECT_FALSE(scene_hdr_active()) << i;
  }
  EXPECT_FALSE(bloom::scene_post_available());
  EXPECT_FALSE(set_scene_hdr(true));
  EXPECT_FALSE(scene_hdr_active());
  const size_t tasks = frame.encoderTasks.size();
  const size_t passes = frame.renderPasses.size();
  EXPECT_FALSE(bloom::record(scene_post())); // No post outside the HDR scene.
  EXPECT_EQ(frame.encoderTasks.size(), tasks);
  EXPECT_EQ(frame.renderPasses.size(), passes);
  EXPECT_FALSE(scene_hdr_active());
  finish();
  EXPECT_FALSE(scene_hdr_active());
  bloom::shutdown();
}

TEST_F(SceneHdrTest, FailedHandOffStaysInTheHdrScene) {
  bloom::shutdown();
  ASSERT_TRUE(bloom::ensure_task());
  ASSERT_TRUE(set_scene_hdr(true));
  const auto view = webgpu::g_frameBuffer.view;
  webgpu::g_frameBuffer.view = {};
  const size_t tasks = frame.encoderTasks.size();
  EXPECT_FALSE(bloom::record(scene_post()));
  EXPECT_EQ(frame.encoderTasks.size(), tasks);
  EXPECT_TRUE(scene_hdr_pass_active());
  webgpu::g_frameBuffer.view = view;
  bloom::shutdown();
}

TEST(HdrPostToneTest, NoCurveIsTheIdentityClampedAtOne) {
  constexpr hdr::Tone NoTone{};
  for (const float x : {0.f, 0.001f, 0.25f, 0.5f, 0.999f}) {
    EXPECT_FLOAT_EQ(hdr::display_linear(x, NoTone), x) << x;
  }
  for (const float x : {1.f, 2.f, 7.f, 34.f, 1e6f}) {
    EXPECT_FLOAT_EQ(hdr::display_linear(x, NoTone), 1.f) << x;
  }
  EXPECT_FLOAT_EQ(hdr::display_linear(-3.f, NoTone), 0.f);
  EXPECT_FLOAT_EQ(hdr::post_tone(0.5f, NoTone), hdr::srgb_enc(0.5f));
  EXPECT_FLOAT_EQ(hdr::post_tone(34.f, NoTone), 1.f);
}

TEST(HdrPostToneTest, CurveIsAppliedOnceAndClamped) {
  for (const float x : {0.f, 0.5f, 2.f, 7.f, 34.f}) {
    const float y = std::clamp(hdr::tone_fwd(x, CopyTone), 0.f, 1.f);
    EXPECT_FLOAT_EQ(hdr::display_linear(x, CopyTone), y) << x;
    EXPECT_FLOAT_EQ(hdr::post_tone(x, CopyTone), hdr::srgb_enc(y)) << x;
  }
  // The curve keeps 2 < 7 < 34 apart where the identity would clip all three.
  EXPECT_LT(hdr::display_linear(2.f, CopyTone), hdr::display_linear(7.f, CopyTone));
  EXPECT_LT(hdr::display_linear(7.f, CopyTone), hdr::display_linear(34.f, CopyTone));
}

} // namespace
} // namespace aurora::gfx
