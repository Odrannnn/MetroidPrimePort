#include <gtest/gtest.h>

#include "gfx/resources.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/texture.hpp"
#include "webgpu/gpu.hpp"

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Depth tex copies from a multisampled EFB (issue #200) on Dawn's Null backend: real textures, views,
// bind groups, pipelines and error scopes, but no rendering, so nothing here proves what a GPU draws.
// It proves that a 4x EFB depth copy builds a pipeline Dawn validates instead of ending the game,
// which is what encoding.cpp's Log.fatal did before c87d028ba.
namespace aurora::gfx {
namespace {

constexpr auto SurfaceFormat = wgpu::TextureFormat::RGBA8Unorm;
// The EFB's depth buffer: webgpu::g_graphicsConfig.depthFormat, as resize_swapchain_internal makes it.
constexpr auto EfbDepthFormat = wgpu::TextureFormat::Depth32Float;
constexpr uint32_t EfbWidth = 64;
constexpr uint32_t EfbHeight = 32;
// tex_copy_conv's DepthConvPipelines: every format a depth copy can ask to convert.
constexpr std::array DepthFormats{GX_TF_Z8, GX_TF_Z16};

// Errors that escaped every error scope.
std::vector<std::string> g_uncapturedErrors;

class TexCopyConvMsaaTest : public ::testing::Test {
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
    // Validation on: a pipeline or bind group that does not match what the shaders declare must be
    // an error here, as it is on a real device.
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
    webgpu::g_graphicsConfig.surfaceConfiguration.format = SurfaceFormat;
    webgpu::g_graphicsConfig.depthFormat = EfbDepthFormat;
    // A copy's uniform lives in the frame's buffer; a small one stands in for it.
    savedUniformBuffer = detail::resources().uniformBuffer;
    detail::resources().uniformBuffer = webgpu::g_device.CreateBuffer(&uniformDescriptor);
    tex_copy_conv::initialize();
  }

  void TearDown() override {
    if (!webgpu::g_device) {
      return;
    }
    tex_copy_conv::shutdown();
    detail::resources().uniformBuffer = savedUniformBuffer;
    webgpu::g_hasCoreFeatures = false;
    webgpu::g_graphicsConfig.msaaSamples = 1;
    EXPECT_TRUE(g_uncapturedErrors.empty()) << g_uncapturedErrors.front();
  }

  // The EFB's depth target at `samples`, which copySourceDepthView views (lib/gfx/recording.cpp).
  static wgpu::TextureView make_efb_depth(uint32_t samples) {
    const wgpu::TextureDescriptor depthDescriptor{
        .label = "EFB depth test target",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {EfbWidth, EfbHeight, 1},
        .format = EfbDepthFormat,
        .sampleCount = samples,
    };
    return webgpu::g_device.CreateTexture(&depthDescriptor).CreateView();
  }

  // The request encoding.cpp builds for a depth copy (its resolveTarget is a render texture of the
  // copied format, its resolveRect the whole EFB).
  static tex_copy_conv::ConvRequest depth_copy_request(GXTexFmt fmt, const wgpu::TextureView& srcDepth,
                                                       uint32_t srcSamples) {
    return tex_copy_conv::ConvRequest{
        .fmt = fmt,
        .srcView = srcDepth,
        .uniformRange = {0, sizeof(tex_copy_conv::Uniforms)},
        .dst = new_render_texture(EfbWidth, EfbHeight, fmt, "EFB depth copy test"),
        .sampleFilter = tex_copy_conv::SampleFilter::Nearest,
        .domain = tex_copy_conv::CopyDomain::Display,
        .srcSamples = srcSamples,
    };
  }

  // The validation error recording and submitting `f` raises, or empty.
  template <typename F>
  static std::string submit_error(F&& f) {
    webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
    const auto cmd = webgpu::g_device.CreateCommandEncoder();
    f(cmd);
    auto commandBuffer = cmd.Finish();
    webgpu::g_device.GetQueue().Submit(1, &commandBuffer);
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

  static inline const wgpu::BufferDescriptor uniformDescriptor{
      .label = "EFB depth copy test uniforms",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = 1024,
  };

  wgpu::Buffer savedUniformBuffer;
};

// Every depth format tex_copy_conv converts builds a pipeline for a 4x source on a Core device: the
// list the 4x EFB copy looks its pipeline up in.
TEST_F(TexCopyConvMsaaTest, CoreDeviceSupportsDepthConversionAtOneAndFourSamples) {
  EXPECT_TRUE(webgpu::g_hasCoreFeatures);
  for (const auto fmt : DepthFormats) {
    SCOPED_TRACE(static_cast<int>(fmt));
    EXPECT_TRUE(tex_copy_conv::depth_conversion_supported(fmt, 1));
    EXPECT_TRUE(tex_copy_conv::depth_conversion_supported(fmt, 4));
  }
}

// The #200 case: a 4x multisampled EFB depth copy. Before c87d028ba this aborted in encoding.cpp
// ("Depth tex copies from multisampled EFB targets are not supported"); a 4x source with no pipeline
// is now skipped by tex_copy_conv::run, so the query gates it here and the run itself must validate.
TEST_F(TexCopyConvMsaaTest, FourSampleDepthCopyConvertsSampleZero) {
  for (const auto fmt : DepthFormats) {
    SCOPED_TRACE(static_cast<int>(fmt));
    ASSERT_TRUE(tex_copy_conv::depth_conversion_supported(fmt, 4));
    const auto srcDepth = make_efb_depth(4);
    const auto req = depth_copy_request(fmt, srcDepth, 4);
    EXPECT_EQ(submit_error([&](const wgpu::CommandEncoder& cmd) { tex_copy_conv::run(cmd, req); }), "");
    EXPECT_TRUE(g_uncapturedErrors.empty()) << g_uncapturedErrors.front();
  }
}

// The control: the same copy from a 1x EFB, which never had a multisampled pipeline to find.
TEST_F(TexCopyConvMsaaTest, OneSampleDepthCopyConverts) {
  for (const auto fmt : DepthFormats) {
    SCOPED_TRACE(static_cast<int>(fmt));
    ASSERT_TRUE(tex_copy_conv::depth_conversion_supported(fmt, 1));
    const auto srcDepth = make_efb_depth(1);
    const auto req = depth_copy_request(fmt, srcDepth, 1);
    EXPECT_EQ(submit_error([&](const wgpu::CommandEncoder& cmd) { tex_copy_conv::run(cmd, req); }), "");
    EXPECT_TRUE(g_uncapturedErrors.empty()) << g_uncapturedErrors.front();
  }
}

// Compatibility devices cannot bind multisampled depth textures, so a 4x depth copy has no pipeline
// and encoding.cpp skips it with a warning (the run is never reached).
TEST_F(TexCopyConvMsaaTest, CompatibilityDeviceHasNoMultisampledDepthConversion) {
  webgpu::g_hasCoreFeatures = false;
  tex_copy_conv::shutdown();
  tex_copy_conv::initialize();
  for (const auto fmt : DepthFormats) {
    SCOPED_TRACE(static_cast<int>(fmt));
    EXPECT_FALSE(tex_copy_conv::depth_conversion_supported(fmt, 4));
    // A 1x copy converts here as it always did.
    EXPECT_TRUE(tex_copy_conv::depth_conversion_supported(fmt, 1));
  }
  EXPECT_TRUE(g_uncapturedErrors.empty()) << g_uncapturedErrors.front();
}

} // namespace
} // namespace aurora::gfx
