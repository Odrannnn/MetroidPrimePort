// The MP_GX_CHECK_ARRAYS vertex array overrun check: a draw whose indexed attributes read past the end
// of their arrays is dropped instead of reading garbage (or crashing, as 807013b1e did where nothing
// bounded the read), for draws sent in the FIFO and for those replayed from a retained display list.

#include "gx_test_common.hpp"
#include "__gx.h"

#include "dolphin/gx/GXExtra.h"
#include "dolphin/gx/GXCommandList.h"
#include "gx/resident.hpp"

#include <array>
#include <vector>

using aurora::gx::g_gxState;

namespace aurora::gfx {
extern uint32_t g_testDrawCount;
extern Range g_testResidentRegions[3];
extern uint32_t g_testResidentUploads;
} // namespace aurora::gfx

namespace {

class GXVtxArrayCheckTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    aurora::gx::testing::set_vertex_array_check(true);
    aurora::gx::testing::vertexArrayOverruns.store(0, std::memory_order_relaxed);
    aurora::gx::fifo::init();
    aurora::gx::fifo::begin_frame();
    GXClearVtxDesc();
  }

  void TearDown() override {
    aurora::gx::fifo::end_frame();
    aurora::gx::testing::set_vertex_array_check(false);
    GXFifoTest::TearDown();
  }

  static uint32_t overruns() { return aurora::gx::testing::vertexArrayOverruns.load(std::memory_order_relaxed); }

  // A three-vertex triangle of 1-byte position indices, into an array of `size` bytes whose elements are
  // `stride` bytes apart.
  void draw_pos_indexed(const void* array, u32 size, u8 stride) {
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
    GXSetArray(GX_VA_POS, array, size, stride, false);
    aurora::gfx::g_testDrawCount = 0;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition1x8(0);
    GXPosition1x8(1);
    GXPosition1x8(2);
    GXEnd();
    aurora::gx::fifo::drain();
  }
};

// A retained display list draws from the resident buffers, not from the FIFO's bytes: its vertices are
// read out of the list itself, and the check must see that copy.
class GXVtxArrayCheckResidentTest : public GXVtxArrayCheckTest {
protected:
  void SetUp() override {
    GXVtxArrayCheckTest::SetUp();
    aurora::gfx::g_testResidentRegions[0] = {5 << 20, 1 << 20};
    aurora::gfx::g_testResidentRegions[1] = {2 << 20, 1 << 20};
    aurora::gfx::g_testResidentRegions[2] = {8 << 20, 1 << 20};
    aurora::gfx::g_testResidentUploads = 0;
    aurora::gx::resident::reset();
  }

  void TearDown() override {
    aurora::gx::resident::reset();
    aurora::gfx::g_testResidentRegions[0] = {};
    aurora::gfx::g_testResidentRegions[1] = {};
    aurora::gfx::g_testResidentRegions[2] = {};
    GXVtxArrayCheckTest::TearDown();
  }
};

} // namespace

TEST_F(GXVtxArrayCheckTest, InBoundsIndexedDrawIsDrawn) {
  static const std::array<u8, 9> positions{0, 1, 2, 3, 4, 5, 6, 7, 8};
  draw_pos_indexed(positions.data(), static_cast<u32>(positions.size()), 3);
  // Index 2's element ends at 2 * 3 + 3 = 9, the array's size.
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  EXPECT_EQ(overruns(), 0u);
}

TEST_F(GXVtxArrayCheckTest, IndexPastTheArrayEndDropsTheDraw) {
  static const std::array<u8, 3> positions{0, 1, 2};
  draw_pos_indexed(positions.data(), static_cast<u32>(positions.size()), 3);
  // Index 2's element would end at 9, past the array's 3 bytes.
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 0u);
  EXPECT_EQ(overruns(), 1u);
}

// 807013b1e: a skinned PBR draw read its bind pose through GX_VA_TEX7 while TEX7's array held 8-byte
// UVs, and the bind-pose read is a 12-byte position's y.
TEST_F(GXVtxArrayCheckTest, BindPoseArrayOfShorterElementsIsDropped) {
  static const std::array<f32, 9> positions{}; // 3 positions, 12 bytes each
  static const std::array<f32, 6> uvs{};       // 3 UVs, 8 bytes each
  GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetArray(GX_VA_POS, positions.data(), static_cast<u32>(sizeof(positions)), 12, false);
  GXSetArray(GX_VA_TEX7, uvs.data(), static_cast<u32>(sizeof(uvs)), 8, false);
  aurora::gfx::g_testDrawCount = 0;
  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  GXPosition1x8(0);
  GXPosition1x8(1);
  GXPosition1x8(2);
  GXEnd();
  // The backlight asks for the bind pose (bind_pos_active).
  g_gxState.pbr = 1;
  g_gxState.pbrBacklightLights[2] = aurora::Vec4<float>{0.f, 0.f, 1.f, 0.f};
  aurora::gx::fifo::drain();
  // Index 2 reads bytes 28..32 of a 24-byte array.
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 0u);
  EXPECT_EQ(overruns(), 1u);
}

// The same draw with TEX7 holding what the shader reads (12-byte positions) draws.
TEST_F(GXVtxArrayCheckTest, BindPoseArrayOfPositionsIsDrawn) {
  static const std::array<f32, 9> positions{};
  GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetArray(GX_VA_POS, positions.data(), static_cast<u32>(sizeof(positions)), 12, false);
  GXSetArray(GX_VA_TEX7, positions.data(), static_cast<u32>(sizeof(positions)), 12, false);
  aurora::gfx::g_testDrawCount = 0;
  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  GXPosition1x8(0);
  GXPosition1x8(1);
  GXPosition1x8(2);
  GXEnd();
  g_gxState.pbr = 1;
  g_gxState.pbrBacklightLights[2] = aurora::Vec4<float>{0.f, 0.f, 1.f, 0.f};
  aurora::gx::fifo::drain();
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  EXPECT_EQ(overruns(), 0u);
}

// Retail's packed-lightmap draws keep TEX0 in the vertex description with indices past the bound UVs,
// and no texgen reads it: the shader's load of it is dead, so the draw stays. A texgen that reads it
// makes the same draw an overrun.
TEST_F(GXVtxArrayCheckTest, TexCoordOverrunCountsOnlyWhenATexgenReadsIt) {
  static const std::array<u8, 9> positions{0, 1, 2, 3, 4, 5, 6, 7, 8};
  static const std::array<f32, 2> uvs{}; // 1 UV; the draw indexes 3
  for (const bool sampled : {false, true}) {
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetArray(GX_VA_POS, positions.data(), static_cast<u32>(positions.size()), 3, false);
    GXSetArray(GX_VA_TEX0, uvs.data(), static_cast<u32>(sizeof(uvs)), 8, false);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, sampled ? GX_TG_TEX0 : GX_TG_TEX1, GX_IDENTITY);
    aurora::gfx::g_testDrawCount = 0;
    aurora::gx::testing::vertexArrayOverruns.store(0, std::memory_order_relaxed);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (u8 i = 0; i < 3; ++i) {
      GXPosition1x8(i);
      GXTexCoord1x8(i);
    }
    GXEnd();
    aurora::gx::fifo::drain();
    EXPECT_EQ(aurora::gfx::g_testDrawCount, sampled ? 0u : 1u) << "sampled " << sampled;
    EXPECT_EQ(overruns(), sampled ? 1u : 0u) << "sampled " << sampled;
  }
}

TEST_F(GXVtxArrayCheckResidentTest, ResidentDisplayListDrawIsChecked) {
  static const std::array<u8, 3> positions{0, 1, 2};
  GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_U8, 0);
  GXSetArray(GX_VA_POS, positions.data(), static_cast<u32>(positions.size()), 3, false);

  std::vector<u8> dl{GX_DRAW_TRIANGLES | GX_VTXFMT0, 0, 3, 0, 1, 2};
  dl.resize(32, GX_NOP);
  GXPortRetainResident(dl.data(), static_cast<u32>(dl.size()));

  aurora::gfx::g_testDrawCount = 0;
  GXCallDisplayList(dl.data(), static_cast<u32>(dl.size()));
  aurora::gx::fifo::drain();
  // Made into the resident buffers (vertices and indices), so the draw below comes from there.
  EXPECT_EQ(aurora::gfx::g_testResidentUploads, 2u);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 0u);
  EXPECT_EQ(overruns(), 1u);

  // The same list with an array its indices fit draws from the resident copy.
  static const std::array<u8, 9> longer{0, 1, 2, 3, 4, 5, 6, 7, 8};
  GXSetArray(GX_VA_POS, longer.data(), static_cast<u32>(longer.size()), 3, false);
  aurora::gfx::g_testDrawCount = 0;
  aurora::gx::testing::vertexArrayOverruns.store(0, std::memory_order_relaxed);
  GXCallDisplayList(dl.data(), static_cast<u32>(dl.size()));
  aurora::gx::fifo::drain();
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  EXPECT_EQ(overruns(), 0u);

  GXPortReleaseResident(dl.data());
}
