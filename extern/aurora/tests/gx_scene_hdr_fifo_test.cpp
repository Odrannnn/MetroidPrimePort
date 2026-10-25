// GXPortSceneHdr's FIFO encoding and its command-processor handler (gfx::set_scene_hdr is stubbed).

#include "gx_test_common.hpp"
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXExtra.h>

#include "gfx/bloom.hpp"
#include "gfx/recording.hpp"
#include "gx/pipeline.hpp"

#include <algorithm>
#include <cstring>
#include <array>

using aurora::gx::g_gxState;

namespace aurora::webgpu {
extern bool g_testSceneHdrSupported;
} // namespace aurora::webgpu
namespace aurora::gfx {
extern bool g_testSceneHdr;
extern uint32_t g_testSceneHdrSwitches;
extern bool g_testSceneHdrRefuse;
extern bool g_testSceneHdrOffscreen;
extern uint32_t g_testDrawCount;
extern aurora::gx::DrawData g_testLastDraw;
} // namespace aurora::gfx
namespace aurora::gx {
extern uint32_t g_testPopulateCount;
extern uint32_t g_testUniformBuildCount;
extern bool g_testUniformCompatSrcAlphaAdditive;
} // namespace aurora::gx
namespace aurora::gfx::bloom::testing {
extern bool task;
extern uint32_t records;
extern Params lastRecord;
extern bool leaveScene;
} // namespace aurora::gfx::bloom::testing
namespace aurora::gfx::testing {
extern bool lastResolveHasHdr;
extern ResolveHdr lastResolveHdr;
extern bool lastResolveMips;
extern bool copyConvertsRgb565;
extern uint32_t lastRenderTextureMips;
extern wgpu::TextureFormat lastRenderTextureFormat;
} // namespace aurora::gfx::testing

namespace {

class GXSceneHdrFifoTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    aurora::webgpu::g_testSceneHdrSupported = true;
    aurora::gfx::g_testSceneHdr = false;
    aurora::gfx::g_testSceneHdrSwitches = 0;
    aurora::gfx::g_testSceneHdrRefuse = false;
  }

  void TearDown() override {
    aurora::webgpu::g_testSceneHdrSupported = true;
    aurora::gfx::g_testSceneHdr = false;
    aurora::gfx::g_testSceneHdrOffscreen = false;
    aurora::gfx::g_testSceneHdrRefuse = false;
    GXFifoTest::TearDown();
  }

  static std::vector<u8> command(u8 value) {
    return {GX_AURORA, static_cast<u8>(GX_AURORA_PORT_SCENE_HDR >> 8),
            static_cast<u8>(GX_AURORA_PORT_SCENE_HDR & 0xFF), value};
  }

  static std::vector<u8> copy_scene(u8 value) {
    return {GX_AURORA, static_cast<u8>(GX_AURORA_PORT_COPY_SCENE >> 8),
            static_cast<u8>(GX_AURORA_PORT_COPY_SCENE & 0xFF), value};
  }

  static std::vector<u8> draw_id(u8 value) {
    return {GX_AURORA, static_cast<u8>(GX_AURORA_PORT_DRAW_ID_MODE >> 8),
            static_cast<u8>(GX_AURORA_PORT_DRAW_ID_MODE & 0xFF), value};
  }
};

TEST_F(GXSceneHdrFifoTest, EncodesOneByteAndSwitchesTheScene) {
  EXPECT_TRUE(GXPortSceneHdr(GX_TRUE));
  auto bytes = capture_fifo();
  EXPECT_EQ(bytes, command(1));

  reset_gx_state();
  g_gxState.dirty = 0;
  decode_fifo(bytes);
  EXPECT_TRUE(aurora::gfx::g_testSceneHdr);
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 1u);
  EXPECT_NE(g_gxState.dirty & aurora::gx::DirtyPipeline, 0u);

  EXPECT_TRUE(GXPortSceneHdr(GX_FALSE));
  bytes = capture_fifo();
  EXPECT_EQ(bytes, command(0));
  decode_fifo(bytes);
  EXPECT_FALSE(aurora::gfx::g_testSceneHdr);
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 2u);
}

TEST_F(GXSceneHdrFifoTest, RepeatedRequestIsDeduplicated) {
  aurora::gfx::g_testSceneHdr = true;
  g_gxState.dirty = 0;
  decode_fifo(command(1));
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 0u);
  EXPECT_EQ(g_gxState.dirty & aurora::gx::DirtyPipeline, 0u);
  decode_fifo(command(0));
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 1u);
  decode_fifo(command(0));
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 1u);
}

TEST_F(GXSceneHdrFifoTest, InvalidValueIsIgnored) {
  g_gxState.dirty = 0;
  decode_fifo(command(3));
  EXPECT_FALSE(aurora::gfx::g_testSceneHdr);
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 0u);
  EXPECT_EQ(g_gxState.dirty & aurora::gx::DirtyPipeline, 0u);
  // The stream stays in sync: the next command still decodes.
  auto bytes = command(3);
  const auto next = command(1);
  bytes.insert(bytes.end(), next.begin(), next.end());
  decode_fifo(bytes);
  EXPECT_TRUE(aurora::gfx::g_testSceneHdr);
}

TEST_F(GXSceneHdrFifoTest, UnsupportedDeviceWritesNothing) {
  aurora::webgpu::g_testSceneHdrSupported = false;
  EXPECT_FALSE(GXPortSceneHdr(GX_TRUE));
  EXPECT_TRUE(capture_fifo().empty());
  // Turning it off is always encoded.
  EXPECT_TRUE(GXPortSceneHdr(GX_FALSE));
  EXPECT_EQ(capture_fifo(), command(0));
}

TEST_F(GXSceneHdrFifoTest, PauseRequestEncodesAndIsNeverDeduplicated) {
  EXPECT_TRUE(GXPortPauseSceneHdr());
  EXPECT_EQ(capture_fifo(), command(2));
  decode_fifo(command(2));
  EXPECT_TRUE(aurora::gfx::g_testSceneHdr);
  const auto switches = aurora::gfx::g_testSceneHdrSwitches;
  decode_fifo(command(2));
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, switches + 1);
}

TEST_F(GXSceneHdrFifoTest, DrawIdModeRefusesIt) {
  GXPortSetDrawIdMode(GX_TRUE);
  capture_fifo();
  EXPECT_FALSE(GXPortSceneHdr(GX_TRUE));
  EXPECT_TRUE(capture_fifo().empty());
  GXPortSetDrawIdMode(GX_FALSE);
  capture_fifo();
  EXPECT_TRUE(GXPortSceneHdr(GX_TRUE));
  EXPECT_EQ(capture_fifo(), command(1));
}

// The FIFO enforces the draw-id policy too, whatever order the encoder saw.
TEST_F(GXSceneHdrFifoTest, DecodedDrawIdModeRefusesHdr) {
  decode_fifo(draw_id(1));
  ASSERT_TRUE(g_gxState.drawIdMode);
  decode_fifo(command(1));
  EXPECT_FALSE(aurora::gfx::g_testSceneHdr);
  EXPECT_EQ(aurora::gfx::g_testSceneHdrSwitches, 0u);
  decode_fifo(draw_id(0));
  decode_fifo(command(1));
  EXPECT_TRUE(aurora::gfx::g_testSceneHdr);
}

TEST_F(GXSceneHdrFifoTest, DrawIdModeLeavesTheHdrScene) {
  decode_fifo(command(1));
  ASSERT_TRUE(aurora::gfx::g_testSceneHdr);
  g_gxState.dirty = 0;
  decode_fifo(draw_id(1));
  EXPECT_FALSE(aurora::gfx::g_testSceneHdr);
  EXPECT_TRUE(g_gxState.drawIdMode);
  EXPECT_NE(g_gxState.dirty & aurora::gx::DirtyPipeline, 0u);
}

TEST_F(GXSceneHdrFifoTest, DrawIdModeStaysOffWhenHdrCannotBeLeft) {
  decode_fifo(command(1));
  ASSERT_TRUE(aurora::gfx::g_testSceneHdr);
  aurora::gfx::g_testSceneHdrRefuse = true;
  decode_fifo(draw_id(1));
  EXPECT_TRUE(aurora::gfx::g_testSceneHdr);
  EXPECT_FALSE(g_gxState.drawIdMode);
  // Turning draw-id off is never blocked.
  decode_fifo(draw_id(0));
  EXPECT_FALSE(g_gxState.drawIdMode);
}

// --- GXPortCopyScene: the linear scene copy request ------------------------------------------------

class GXCopySceneFifoTest : public GXSceneHdrFifoTest {
protected:
  void SetUp() override {
    GXSceneHdrFifoTest::SetUp();
    g_gxState.copyTextureCache.clear();
    g_gxState.copyTextures.clear();
    g_gxState.copySceneLinear = false;
    aurora::gfx::testing::lastResolveHasHdr = false;
    aurora::gfx::testing::lastResolveHdr = {};
  }

  void TearDown() override {
    g_gxState.copyTextureCache.clear();
    g_gxState.copyTextures.clear();
    g_gxState.copySceneLinear = false;
    GXSceneHdrFifoTest::TearDown();
  }

  // The copy registers and GXCopyTex's command, as the encoder writes them.
  std::vector<u8> copy(void* dest, GXTexFmt fmt, bool scene) {
    GXSetTexCopySrc(0, 0, 64, 32);
    GXSetTexCopyDst(64, 32, fmt, GX_FALSE);
    if (scene) {
      GXPortCopyScene(dest, GX_FALSE);
    } else {
      GXCopyTex(dest, GX_FALSE);
    }
    return capture_fifo();
  }

  static size_t entries(const void* dest, bool linear) {
    size_t n = 0;
    for (const auto& [key, ref] : g_gxState.copyTextureCache) {
      n += key.dest == dest && key.linear == linear;
    }
    return n;
  }
};

TEST_F(GXCopySceneFifoTest, EncodesTheCopyBetweenScopedMarkers) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  const auto plain = copy(dest, GX_TF_RGB565, false);
  const auto scene = copy(dest, GX_TF_RGB565, true);
  const auto on = copy_scene(1);
  const auto off = copy_scene(0);
  // Same copy registers and copy command; only the markers around the copy are new.
  ASSERT_GE(scene.size(), plain.size() + on.size() + off.size());
  EXPECT_EQ(std::vector<u8>(scene.end() - off.size(), scene.end()), off);
  const auto copyCmd = std::search(scene.begin(), scene.end(), on.begin(), on.end());
  ASSERT_NE(copyCmd, scene.end());
  std::vector<u8> unmarked(scene.begin(), copyCmd);
  unmarked.insert(unmarked.end(), copyCmd + on.size(), scene.end() - off.size());
  EXPECT_EQ(unmarked, plain);
}

TEST_F(GXCopySceneFifoTest, LinearOnlyInTheHdrScenePassAndOnlyForThatCopy) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  aurora::gfx::g_testSceneHdr = true;
  g_gxState.pbrTone = {aurora::Vec4<float>{1.f, 2.f, 3.f, 4.f}, aurora::Vec4<float>{5.f, 6.f, 7.f, 8.f},
                       aurora::Vec4<float>{9.f, 10.f, 11.f, 12.f}};
  decode_fifo(copy(dest, GX_TF_RGB565, true));
  EXPECT_FALSE(g_gxState.copySceneLinear) << "the closing marker resets it";
  EXPECT_EQ(entries(dest, true), 1u);
  EXPECT_EQ(entries(dest, false), 0u);
  ASSERT_TRUE(aurora::gfx::testing::lastResolveHasHdr);
  EXPECT_TRUE(aurora::gfx::testing::lastResolveHdr.linear);
  EXPECT_EQ(aurora::gfx::testing::lastResolveHdr.tone[2][3], 12.f) << "the tone rows at the copy";

  // A retail copy right after, to the same dest: the display copy, in its own entry.
  g_gxState.pbrTone[0] = aurora::Vec4<float>{0.5f, 0.f, 0.f, 0.f};
  decode_fifo(copy(dest, GX_TF_RGB565, false));
  EXPECT_EQ(entries(dest, true), 1u);
  EXPECT_EQ(entries(dest, false), 1u);
  ASSERT_TRUE(aurora::gfx::testing::lastResolveHasHdr);
  EXPECT_FALSE(aurora::gfx::testing::lastResolveHdr.linear);
  EXPECT_EQ(aurora::gfx::testing::lastResolveHdr.tone[0][0], 0.5f);

  // Alternating hits the two entries, never a third.
  decode_fifo(copy(dest, GX_TF_RGB565, true));
  decode_fifo(copy(dest, GX_TF_RGB565, false));
  EXPECT_EQ(g_gxState.copyTextureCache.size(), 2u);
}

TEST_F(GXCopySceneFifoTest, OutsideTheHdrScenePassItIsTheOldCopy) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  for (const bool offscreen : {false, true}) {
    SCOPED_TRACE(offscreen);
    g_gxState.copyTextureCache.clear();
    aurora::gfx::g_testSceneHdr = offscreen;
    aurora::gfx::g_testSceneHdrOffscreen = offscreen;
    decode_fifo(copy(dest, GX_TF_RGB565, true));
    EXPECT_EQ(entries(dest, true), 0u);
    EXPECT_EQ(entries(dest, false), 1u);
    EXPECT_FALSE(aurora::gfx::testing::lastResolveHdr.linear);
    EXPECT_FALSE(g_gxState.copySceneLinear);
  }
}

TEST_F(GXCopySceneFifoTest, DepthCopiesAreNeverLinear) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  aurora::gfx::g_testSceneHdr = true;
  decode_fifo(copy(dest, GX_TF_Z24X8, true));
  EXPECT_EQ(entries(dest, true), 0u);
  EXPECT_EQ(entries(dest, false), 1u);
  EXPECT_FALSE(aurora::gfx::testing::lastResolveHdr.linear);
}

TEST_F(GXCopySceneFifoTest, MarkerNeverLeaks) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  aurora::gfx::g_testSceneHdr = true;
  // An invalid payload is not a request, and turns one off.
  decode_fifo(copy_scene(2));
  EXPECT_FALSE(g_gxState.copySceneLinear);
  decode_fifo(copy_scene(1));
  decode_fifo(copy_scene(2));
  EXPECT_FALSE(g_gxState.copySceneLinear);
  // A marker left open (a stream cut before its close) ends with the frame.
  decode_fifo(copy_scene(1));
  ASSERT_TRUE(g_gxState.copySceneLinear);
  aurora::gx::fifo::end_frame();
  EXPECT_FALSE(g_gxState.copySceneLinear);
  // The retail copy after the reset is the display copy.
  decode_fifo(copy(dest, GX_TF_RGB565, false));
  EXPECT_EQ(entries(dest, true), 0u);
  EXPECT_FALSE(aurora::gfx::testing::lastResolveHdr.linear);
}

// A draw's pipeline follows the pass it lands in, not only the FIFO's switch: the scene mode
// changing under it (a frame reset to LDR, an offscreen pass) rebuilds its config, with the bit set
// only in the HDR scene.
TEST_F(GXSceneHdrFifoTest, PipelineConfigFollowsThePassMode) {
  using aurora::gfx::g_testSceneHdr;
  using aurora::gfx::g_testSceneHdrOffscreen;
  using aurora::gx::g_testPopulateCount;
  aurora::gx::fifo::init();
  aurora::gx::fifo::begin_frame();
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
  const auto draw = [] {
    aurora::gfx::g_testDrawCount = 0;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3u8(0, 1, 2);
    GXPosition3u8(3, 4, 5);
    GXPosition3u8(6, 7, 8);
    GXEnd();
    aurora::gx::fifo::drain();
    EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  };
  g_testSceneHdr = false;
  g_testSceneHdrOffscreen = false;
  draw();
  const uint32_t base = g_testPopulateCount;
  draw();
  EXPECT_EQ(g_testPopulateCount, base) << "an unchanged mode keeps the pipeline";
  g_testSceneHdr = true; // the HDR scene, without a FIFO dirty bit
  draw();
  EXPECT_EQ(g_testPopulateCount, base + 1);
  draw();
  EXPECT_EQ(g_testPopulateCount, base + 1);
  g_testSceneHdrOffscreen = true; // an offscreen pass inside it draws LDR
  draw();
  EXPECT_EQ(g_testPopulateCount, base + 2);
  g_testSceneHdrOffscreen = false;
  draw();
  EXPECT_EQ(g_testPopulateCount, base + 3);
  g_testSceneHdr = false; // the next frame starts LDR
  draw();
  EXPECT_EQ(g_testPopulateCount, base + 4);
  draw();
  EXPECT_EQ(g_testPopulateCount, base + 4);
  aurora::gx::fifo::end_frame();
  aurora::gx::fifo::shutdown();
}

// Only cmode0 changes between otherwise identical draws. The FIFO target stubs GPU/packing;
// scene_hdr_writer_test independently verifies the real packer's flag and row layout.
TEST_F(GXSceneHdrFifoTest, BlendOnlyTransitionsRefreshTheHdrUniform) {
  using namespace aurora::gx;
  aurora::gx::fifo::init();
  aurora::gx::fifo::begin_frame();
  aurora::gfx::g_testSceneHdr = true;
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
  const auto draw = [] {
    const u32 before = aurora::gfx::g_testDrawCount;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3u8(0, 1, 2);
    GXPosition3u8(3, 4, 5);
    GXPosition3u8(6, 7, 8);
    GXEnd();
    aurora::gx::fifo::drain();
    EXPECT_EQ(aurora::gfx::g_testDrawCount, before + 1);
    EXPECT_EQ(aurora::gfx::g_testLastDraw.uniformRange.offset, g_testUniformBuildCount);
  };
  struct Blend {
    GXBlendMode mode;
    GXBlendFactor src, dst;
    bool compat;
  };
  for (const auto b :
       {Blend{GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, false}, Blend{GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, true},
        Blend{GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, false}, Blend{GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, true},
        Blend{GX_BM_SUBTRACT, GX_BL_SRCALPHA, GX_BL_ONE, false}, Blend{GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, true},
        Blend{GX_BM_LOGIC, GX_BL_SRCALPHA, GX_BL_ONE, false}, Blend{GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, true},
        Blend{GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, true}, Blend{GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, false},
        Blend{GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, true}}) {
    const u32 before = g_testUniformBuildCount;
    GXSetBlendMode(b.mode, b.src, b.dst, GX_LO_COPY);
    draw();
    EXPECT_EQ(g_testUniformBuildCount, before + 1);
    EXPECT_EQ(g_testUniformCompatSrcAlphaAdditive, b.compat);
    draw();
    EXPECT_EQ(g_testUniformBuildCount, before + 1) << "unchanged draws reuse their uniform";
    EXPECT_EQ(g_testUniformCompatSrcAlphaAdditive, b.compat);
  }
  aurora::gx::fifo::end_frame();
  aurora::gx::fifo::shutdown();
}

// --- The game thread's copy serial across a scene switch ---------------------------------------------
// CCubeModel's glass proxy (sPortGlassCopy) reuses its copy while GXPortCopySerial, the frame and the
// viewport are unchanged. A scene switch changes what the next copy holds without a copy being made,
// so every GXPortSceneHdr request must move the serial, on the game thread, before any FIFO decode.

// The reuse test sPortGlassCopy makes (CCubeModel.cpp), less the frame and viewport, which stay equal.
struct GlassProxyCache {
  bool valid = false;
  u32 serial = 0;
  bool reuse() const { return valid && serial == GXPortCopySerial(); }
  void copied() {
    valid = true;
    serial = GXPortCopySerial();
  }
};

TEST_F(GXSceneHdrFifoTest, EverySwitchRequestMovesTheCopySerial) {
  const u32 start = GXPortCopySerial();
  EXPECT_TRUE(GXPortSceneHdr(GX_TRUE));
  const u32 on = GXPortCopySerial();
  EXPECT_NE(on, start);
  EXPECT_TRUE(GXPortSceneHdr(GX_FALSE));
  const u32 off = GXPortCopySerial();
  EXPECT_NE(off, on);
  // Repeated requests (the FIFO deduplicates them) still move it: a spare recopy is safe.
  EXPECT_TRUE(GXPortSceneHdr(GX_FALSE));
  EXPECT_NE(GXPortCopySerial(), off);
  // Refused requests too: the caller can't know what the scene now holds.
  aurora::webgpu::g_testSceneHdrSupported = false;
  u32 before = GXPortCopySerial();
  EXPECT_FALSE(GXPortSceneHdr(GX_TRUE));
  EXPECT_NE(GXPortCopySerial(), before);
  aurora::webgpu::g_testSceneHdrSupported = true;
  GXPortSetDrawIdMode(GX_TRUE);
  before = GXPortCopySerial();
  EXPECT_FALSE(GXPortSceneHdr(GX_TRUE));
  EXPECT_NE(GXPortCopySerial(), before);
  GXPortSetDrawIdMode(GX_FALSE);
  capture_fifo();
  // Decoding the switches on the FIFO thread never touches the game thread's counter.
  GXPortSceneHdr(GX_TRUE);
  GXPortSceneHdr(GX_FALSE);
  const auto bytes = capture_fifo();
  before = GXPortCopySerial();
  decode_fifo(bytes);
  decode_fifo(command(1));
  decode_fifo(command(2));
  decode_fifo(command(0));
  EXPECT_EQ(GXPortCopySerial(), before);
}

TEST_F(GXSceneHdrFifoTest, AGlassProxyCopyIsRemadeAcrossASwitchInTheSameFrame) {
  void* const dest = reinterpret_cast<void*>(0x6000);
  GlassProxyCache glass;
  GXSetTexCopySrc(0, 0, 64, 32);
  GXSetTexCopyDst(64, 32, GX_TF_RGB565, GX_TRUE);
  GXPortCopyScene(dest, GX_FALSE);
  glass.copied();
  EXPECT_TRUE(glass.reuse());
  for (const GXBool mode : {GX_TRUE, GX_FALSE, GX_TRUE, GX_TRUE, GX_FALSE}) {
    SCOPED_TRACE(static_cast<int>(mode));
    GXPortSceneHdr(mode);
    EXPECT_FALSE(glass.reuse()) << "an earlier copy taken across a switch";
    GXPortCopyScene(dest, GX_FALSE);
    glass.copied();
    EXPECT_TRUE(glass.reuse());
  }
  capture_fifo();
}

TEST_F(GXSceneHdrFifoTest, FramesWithoutSwitchesKeepTheOldSerial) {
  void* const dest = reinterpret_cast<void*>(0x6000);
  GXSetTexCopySrc(0, 0, 64, 32);
  GXSetTexCopyDst(64, 32, GX_TF_RGB565, GX_TRUE);
  const u32 start = GXPortCopySerial();
  GlassProxyCache glass;
  glass.copied();
  // Draw state and frame ends alone don't move it; each copy moves it by one, as before.
  GXSetDstAlpha(GX_FALSE, 0);
  capture_fifo();
  aurora::gx::fifo::end_frame();
  EXPECT_TRUE(glass.reuse());
  GXCopyTex(dest, GX_FALSE);
  EXPECT_EQ(GXPortCopySerial(), start + 1);
  GXPortCopyScene(dest, GX_FALSE);
  EXPECT_EQ(GXPortCopySerial(), start + 2);
  capture_fifo();
}

// --- Mip chains of linear copies ------------------------------------------------------------------------
// CCubeModel's glass copies are RGB565 with mips. RGB565's display copy is converted (one level); its
// linear copy is RGBA16F, never converted, so it keeps the chain it asked for.

TEST_F(GXCopySceneFifoTest, ALinearRgb565CopyKeepsItsMips) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  aurora::gfx::testing::copyConvertsRgb565 = true;
  aurora::gfx::g_testSceneHdr = true;
  GXSetTexCopySrc(0, 0, 64, 32);
  GXSetTexCopyDst(64, 32, GX_TF_RGB565, GX_TRUE);
  GXPortCopyScene(dest, GX_FALSE);
  decode_fifo(capture_fifo());
  ASSERT_EQ(entries(dest, true), 1u);
  EXPECT_TRUE(g_gxState.copyTextureCache.begin()->first.mips);
  EXPECT_TRUE(aurora::gfx::testing::lastResolveHdr.linear);
  EXPECT_TRUE(aurora::gfx::testing::lastResolveMips);
  EXPECT_EQ(aurora::gfx::testing::lastRenderTextureMips, 7u); // 64 -> 1
  EXPECT_EQ(aurora::gfx::testing::lastRenderTextureFormat, wgpu::TextureFormat::RGBA16Float);
  aurora::gfx::testing::copyConvertsRgb565 = false;
}

TEST_F(GXCopySceneFifoTest, DisplayCopiesKeepTheirOldMips) {
  void* const dest = reinterpret_cast<void*>(0x5000);
  aurora::gfx::testing::copyConvertsRgb565 = true;
  for (const bool hdr : {false, true}) {
    SCOPED_TRACE(hdr);
    g_gxState.copyTextureCache.clear();
    aurora::gfx::g_testSceneHdr = hdr;
    // RGB565, converted: one level, in or out of the HDR scene.
    GXSetTexCopySrc(0, 0, 64, 32);
    GXSetTexCopyDst(64, 32, GX_TF_RGB565, GX_TRUE);
    GXCopyTex(dest, GX_FALSE);
    decode_fifo(capture_fifo());
    ASSERT_EQ(entries(dest, false), 1u);
    EXPECT_FALSE(g_gxState.copyTextureCache.begin()->first.mips);
    EXPECT_FALSE(aurora::gfx::testing::lastResolveMips);
    // RGBA8, plain: its chain, as before.
    g_gxState.copyTextureCache.clear();
    GXSetTexCopyDst(64, 32, GX_TF_RGBA8, GX_TRUE);
    GXCopyTex(dest, GX_FALSE);
    decode_fifo(capture_fifo());
    ASSERT_EQ(entries(dest, false), 1u);
    EXPECT_TRUE(g_gxState.copyTextureCache.begin()->first.mips);
    EXPECT_TRUE(aurora::gfx::testing::lastResolveMips);
  }
  aurora::gfx::testing::copyConvertsRgb565 = false;
}

// GXPortPostProcessScene is the end of the HDR scene: it always reaches the command processor, even
// with nothing to do (the scene still has to be converted).
class GXPostProcessSceneFifoTest : public GXSceneHdrFifoTest {
protected:
  void SetUp() override {
    GXSceneHdrFifoTest::SetUp();
    aurora::gfx::bloom::testing::task = true;
    aurora::gfx::bloom::testing::records = 0;
  }
  void TearDown() override {
    aurora::gfx::bloom::testing::task = false;
    aurora::gfx::bloom::testing::leaveScene = false;
    GXSetPBRCostTest(0);
    GXSceneHdrFifoTest::TearDown();
  }

  static constexpr f32 Tints[5][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
  static constexpr f32 Tone[3][4] = {{20.f, 3.f, 0.5f, 1.5f}, {0.8f, 0.02f, 0.1f, 1.f}, {0.18f, 1.f, -1.f, 0.82f}};
};

TEST_F(GXPostProcessSceneFifoTest, NothingToDoStillRecordsTheSceneOne) {
  using namespace aurora::gfx::bloom;
  namespace stub = aurora::gfx::bloom::testing;
  const u32 serial = GXPortCopySerial();
  EXPECT_TRUE(GXPortPostProcessScene(GX_FALSE, 1.f, Tints, Tone, 0, 0, 0.f, 0.f));
  EXPECT_NE(GXPortCopySerial(), serial);
  const auto bytes = capture_fifo();
  ASSERT_FALSE(bytes.empty());
  decode_fifo(bytes);
  ASSERT_EQ(stub::records, 1u);
  EXPECT_EQ(stub::lastRecord.bloom, 0u);
  EXPECT_EQ(stub::lastRecord.gradeA, 0u);
  EXPECT_FLOAT_EQ(stub::lastRecord.tone[0][3], 0.f);
  // The tone curve still travels: the HDR composite converts with it.
  EXPECT_FLOAT_EQ(stub::lastRecord.tone[1][0], Tone[1][0]);
  EXPECT_FLOAT_EQ(stub::lastRecord.tone[2][3], Tone[2][3]);
}

TEST_F(GXPostProcessSceneFifoTest, CarriesTheEffects) {
  using namespace aurora::gfx::bloom;
  namespace stub = aurora::gfx::bloom::testing;
  EXPECT_TRUE(GXPortPostProcessScene(GX_TRUE, 0.6f, Tints, Tone, 3, 4, 0.25f, 0.5f));
  decode_fifo(capture_fifo());
  ASSERT_EQ(stub::records, 1u);
  const Params& scene = stub::lastRecord;
  EXPECT_NE(scene.bloom & 1u, 0u);
  EXPECT_FLOAT_EQ(scene.threshold, 0.6f);
  EXPECT_EQ(scene.gradeA, 3u);
  EXPECT_EQ(scene.gradeB, 4u);
  EXPECT_FLOAT_EQ(scene.gradeWeight, 0.25f);
  EXPECT_FLOAT_EQ(scene.tone[1][0], Tone[1][0]);
  EXPECT_FLOAT_EQ(scene.tone[2][3], Tone[2][3]);
}

TEST_F(GXPostProcessSceneFifoTest, NoPostCostTestKeepsTheConversion) {
  using namespace aurora::gfx::bloom;
  namespace stub = aurora::gfx::bloom::testing;
  GXSetPBRCostTest(10);
  EXPECT_TRUE(GXPortPostProcessScene(GX_TRUE, 0.6f, Tints, Tone, 3, 4, 0.25f, 0.5f));
  decode_fifo(capture_fifo());
  ASSERT_EQ(stub::records, 1u);
  EXPECT_EQ(stub::lastRecord.bloom & 1u, 0u);
  EXPECT_EQ(stub::lastRecord.gradeA, 0u);
  EXPECT_EQ(stub::lastRecord.gradeB, 0u);
  EXPECT_FLOAT_EQ(stub::lastRecord.tone[0][3], 0.f);
}

TEST_F(GXPostProcessSceneFifoTest, LeavingTheHdrSceneDirtiesThePipeline) {
  // The handler re-keys the pipeline only when the post changed the scene domain.
  aurora::gfx::g_testSceneHdr = true;
  EXPECT_TRUE(GXPortPostProcessScene(GX_FALSE, 1.f, Tints, Tone, 0, 0, 0.f, 0.f));
  auto bytes = capture_fifo();
  g_gxState.dirty = 0;
  decode_fifo(bytes);
  EXPECT_EQ(g_gxState.dirty & aurora::gx::DirtyPipeline, 0u);

  aurora::gfx::bloom::testing::leaveScene = true;
  EXPECT_TRUE(GXPortPostProcessScene(GX_FALSE, 1.f, Tints, Tone, 0, 0, 0.f, 0.f));
  bytes = capture_fifo();
  g_gxState.dirty = 0;
  decode_fifo(bytes);
  EXPECT_FALSE(aurora::gfx::g_testSceneHdr);
  EXPECT_NE(g_gxState.dirty & aurora::gx::DirtyPipeline, 0u);
}

} // namespace
