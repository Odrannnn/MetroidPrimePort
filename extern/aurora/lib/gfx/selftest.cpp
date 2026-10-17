// GPU self-test: renders known patterns offscreen through the same GX -> generated WGSL -> pipeline ->
// bind group path the game uses, reads the pixels back and compares them with analytic expectations.
// Written for driver bugs that only show on some GPUs (issues #7, #8): each case exercises one feature,
// so a FAIL line names the broken part. Results are logged and kept for the debug UI.
// Every case's pipeline is waited for (blocking lookups, see BlockingPipelines): without that, a case
// whose pipeline happens to still be compiling reads back as "drew nothing" and the test reports a
// device bug that is really the test's own race (issue #124).
// Every case goes draw -> EFB copy -> readback, and a broken stage anywhere reads the same, so two
// checks isolate the later stages: `readback` copies a CPU-written texture straight back (no GX at all)
// and `efb-clear-copy` clears the EFB and copies it (no draw). The readback buffer starts out filled
// with a marker, so a copy that never landed shows as the marker rather than as black (issue #143).
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>

#include "../gx/gx.hpp"
#include "../gx/fifo.hpp"
#include "../gfx/texture.hpp"
#include "../webgpu/gpu.hpp"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <magic_enum.hpp>

namespace aurora::gfx::selftest {
namespace {
Module Log("aurora::gfx::selftest");

using Clock = std::chrono::steady_clock;

constexpr u32 Size = 32;
constexpr u32 RowBytes = 256; // 32 px * 4, already a multiple of the 256-byte copy alignment
constexpr u32 BlockBytes = RowBytes * Size;
constexpr int Tolerance = 2;
constexpr auto StaleAfter = std::chrono::seconds{10};

struct Image {
  std::array<u32, Size * Size> px{}; // 0xRRGGBB, alpha is not compared
};

struct Case {
  const char* name;
  void (*draw)();
  void (*expect)(Image&);
};

struct CaseResult {
  std::string name;
  bool pass = false;
  std::string detail;
  double recordMs = 0.0;
};

enum class Phase { Idle, Pending };

// While this is alive, every draw's pipeline lookup waits for its pipeline (gx::set_blocking_pipelines),
// so a case's draw is never skipped for a pipeline that has not finished compiling. A skipped draw
// reads back as "the case drew nothing", which is what a device bug looks like too: the test is only
// meaningful with this set (see aurora_gpu_selftest_run).
struct BlockingPipelines {
  BlockingPipelines() { gx::set_blocking_pipelines(true); }
  ~BlockingPipelines() { gx::set_blocking_pipelines(false); }
  BlockingPipelines(const BlockingPipelines&) = delete;
  BlockingPipelines& operator=(const BlockingPipelines&) = delete;
};

struct State {
  std::mutex mutex;
  Phase phase = Phase::Idle;
  Clock::time_point started;
  std::vector<CaseResult> results;
  std::vector<Image> expected;
  CaseResult readbackResult; // the `readback` check: its block follows the cases' in the readback buffer
  bool evaluated = false;    // the readback came back and was compared, so a diagnosis means something
  std::vector<TextureHandle> handles;
  wgpu::Buffer readback;
  std::string summary;
  std::string headerLine;
  EncoderTaskId task = InvalidEncoderTask;
  bool warmup = false; // pipelines compile asynchronously, so a first pass only primes them: nothing is logged
};
State g;

// ---- expected image helpers -------------------------------------------------------------------------
constexpr u32 rgb(u32 r, u32 g, u32 b) { return (r << 16) | (g << 8) | b; }

void fill(Image& img, int x0, int y0, int x1, int y1, u32 color) {
  for (int y = std::max(y0, 0); y < std::min<int>(y1, Size); ++y) {
    for (int x = std::max(x0, 0); x < std::min<int>(x1, Size); ++x) {
      img.px[y * Size + x] = color;
    }
  }
}

// ---- draw helpers -----------------------------------------------------------------------------------
void ortho() {
  Mtx44 m;
  MTXOrtho(m, 0.f, static_cast<f32>(Size), 0.f, static_cast<f32>(Size), 1.f, 100.f);
  GXSetProjection(m, GX_ORTHOGRAPHIC);
  Mtx pos;
  MTXIdentity(pos);
  GXLoadPosMtxImm(pos, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
}

void passthrough_tev() {
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTexGens(0);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

void base_state() {
  GXSetViewport(0.f, 0.f, Size, Size, 0.f, 1.f);
  GXSetScissor(0, 0, Size, Size);
  ortho();
  GXSetCullMode(GX_CULL_NONE);
  GXSetNumIndStages(0);
  GXSetFog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0.f, GXColor{0, 0, 0, 0});
  GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  passthrough_tev();
}

void desc(GXAttrType pos, GXAttrType clr, GXAttrType tex = GX_NONE) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, pos);
  if (clr != GX_NONE) {
    GXSetVtxDesc(GX_VA_CLR0, clr);
  }
  if (tex != GX_NONE) {
    GXSetVtxDesc(GX_VA_TEX0, tex);
  }
}

struct Rgba {
  u8 r, g, b, a;
};

void quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 z, Rgba c) {
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  const f32 xs[4] = {x0, x1, x1, x0};
  const f32 ys[4] = {y0, y0, y1, y1};
  for (int i = 0; i < 4; ++i) {
    GXPosition3f32(xs[i], ys[i], z);
    GXColor4u8(c.r, c.g, c.b, c.a);
  }
  GXEnd();
}

// Textured quad; s0/s1 are the S coordinates at the left/right edge.
void tex_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 s0, f32 s1) {
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  const f32 xs[4] = {x0, x1, x1, x0};
  const f32 ys[4] = {y0, y0, y1, y1};
  const f32 ss[4] = {s0, s1, s1, s0};
  const f32 ts[4] = {0.f, 0.f, 1.f, 1.f};
  for (int i = 0; i < 4; ++i) {
    GXPosition3f32(xs[i], ys[i], -5.f);
    GXColor4u8(255, 255, 255, 255);
    GXTexCoord2f32(ss[i], ts[i]);
  }
  GXEnd();
}

void background() {
  desc(GX_DIRECT, GX_DIRECT);
  quad(0.f, 0.f, Size, Size, -50.f, {0, 0, 0, 255});
}

void texture_tev() {
  GXSetNumTexGens(1);
  GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

void load_nearest(GXTexObj* obj, const void* data, u16 w, u16 h, GXTexFmt fmt) {
  GXInitTexObj(obj, data, w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXInitTexObjLOD(obj, GX_NEAR, GX_NEAR, 0.f, 0.f, 0.f, GX_FALSE, GX_FALSE, GX_ANISO_1);
  GXLoadTexObj(obj, GX_TEXMAP0);
}

// ---- cases ------------------------------------------------------------------------------------------
// 1. direct vertices
void draw_direct() {
  background();
  desc(GX_DIRECT, GX_DIRECT);
  quad(4, 4, 16, 16, -5, {255, 0, 0, 255});
  quad(16, 16, 28, 28, -5, {0, 255, 0, 255});
}
void expect_direct(Image& e) {
  fill(e, 4, 4, 16, 16, rgb(255, 0, 0));
  fill(e, 16, 16, 28, 28, rgb(0, 255, 0));
}

// 2. 8-bit indexed positions and colours (junk before the used entries)
alignas(16) const f32 kPos8[] = {
    900, 900, -5, 900, -900, -5, // 0, 1: junk
    2,   2,   -5, 14,  2,    -5, 14, 30, -5, 2,  30, -5, // 2..5
    18,  2,   -5, 30,  2,    -5, 30, 30, -5, 18, 30, -5, // 6..9
};
alignas(4) const u8 kClr8[] = {1, 2, 3, 4, 255, 0, 0, 255, 0, 0, 255, 255};
void draw_indexed8() {
  background();
  desc(GX_INDEX8, GX_INDEX8);
  GXSetArray(GX_VA_POS, kPos8, sizeof(kPos8), 12, true);
  GXSetArray(GX_VA_CLR0, kClr8, sizeof(kClr8), 4, true);
  GXBegin(GX_QUADS, GX_VTXFMT0, 8);
  for (u8 i = 2; i < 6; ++i) {
    GXPosition1x8(i);
    GXColor1x8(1);
  }
  for (u8 i = 6; i < 10; ++i) {
    GXPosition1x8(i);
    GXColor1x8(2);
  }
  GXEnd();
}
void expect_indexed8(Image& e) {
  fill(e, 2, 2, 14, 30, rgb(255, 0, 0));
  fill(e, 18, 2, 30, 30, rgb(0, 0, 255));
}

// 3. 16-bit indexed positions, direct colour, indices above 255
std::array<f32, 304 * 3> g_pos16;
void init_pos16() {
  for (size_t i = 0; i < 304; ++i) {
    g_pos16[i * 3 + 0] = 900.f;
    g_pos16[i * 3 + 1] = 900.f;
    g_pos16[i * 3 + 2] = -5.f;
  }
  const f32 a[4][2] = {{4, 4}, {16, 4}, {16, 16}, {4, 16}};
  const f32 b[4][2] = {{16, 16}, {28, 16}, {28, 28}, {16, 28}};
  for (size_t i = 0; i < 4; ++i) {
    g_pos16[(256 + i) * 3 + 0] = a[i][0];
    g_pos16[(256 + i) * 3 + 1] = a[i][1];
    g_pos16[(1 + i) * 3 + 0] = b[i][0];
    g_pos16[(1 + i) * 3 + 1] = b[i][1];
  }
}
void draw_indexed16() {
  init_pos16();
  background();
  desc(GX_INDEX16, GX_DIRECT);
  GXSetArray(GX_VA_POS, g_pos16.data(), static_cast<u32>(g_pos16.size() * sizeof(f32)), 12, true);
  GXBegin(GX_QUADS, GX_VTXFMT0, 8);
  for (u16 i = 256; i < 260; ++i) {
    GXPosition1x16(i);
    GXColor4u8(0, 255, 0, 255);
  }
  for (u16 i = 1; i < 5; ++i) {
    GXPosition1x16(i);
    GXColor4u8(255, 0, 255, 255);
  }
  GXEnd();
}
void expect_indexed16(Image& e) {
  fill(e, 4, 4, 16, 16, rgb(0, 255, 0));
  fill(e, 16, 16, 28, 28, rgb(255, 0, 255));
}

// 4. konst colour (left) and TEV register colour (right)
void draw_konst_reg() {
  background();
  desc(GX_DIRECT, GX_DIRECT);
  GXSetTevKColor(GX_KCOLOR0, GXColor{10, 200, 90, 255});
  GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
  GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
  GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  quad(0, 0, 16, 32, -5, {255, 255, 255, 255});
  GXSetTevColor(GX_TEVREG0, GXColor{250, 30, 130, 255});
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
  quad(16, 0, 32, 32, -5, {255, 255, 255, 255});
}
void expect_konst_reg(Image& e) {
  fill(e, 0, 0, 16, 32, rgb(10, 200, 90));
  fill(e, 16, 0, 32, 32, rgb(250, 30, 130));
}

// 5. three-stage combine: konst -> * vertex colour -> lerp with a register
void draw_tev3() {
  background();
  desc(GX_DIRECT, GX_DIRECT);
  GXSetNumTevStages(3);
  GXSetTevKColor(GX_KCOLOR1, GXColor{200, 100, 50, 255});
  GXSetTevColor(GX_TEVREG0, GXColor{0, 100, 200, 255});
  for (int s = 0; s < 3; ++s) {
    const auto stage = static_cast<GXTevStageID>(GX_TEVSTAGE0 + s);
    GXSetTevOrder(stage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  }
  GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K1);
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
  GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
  GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_CPREV, GX_CC_C0, GX_CC_HALF, GX_CC_ZERO);
  quad(4, 4, 28, 28, -5, {255, 128, 0, 255});
}
void expect_tev3(Image& e) { fill(e, 4, 4, 28, 28, rgb(100, 75, 100)); }

// 6. RGBA8 texture, nearest, 4x4 texels of 8x8 pixels
constexpr u32 texel_rgba(u32 i, u32 j) { return rgb(60 * i + 15, 60 * j + 15, (i * 4 + j) * 16); }
std::array<u8, 4 * 4 * 4> g_rgba8;
GXTexObj g_texObj;
void draw_rgba8() {
  for (u32 y = 0; y < 4; ++y) {
    for (u32 x = 0; x < 4; ++x) {
      const u32 c = texel_rgba(x, y);
      u8* p = &g_rgba8[(y * 4 + x) * 4];
      p[0] = c >> 16;
      p[1] = c >> 8;
      p[2] = c;
      p[3] = 255;
    }
  }
  background();
  desc(GX_DIRECT, GX_DIRECT, GX_DIRECT);
  load_nearest(&g_texObj, g_rgba8.data(), 4, 4, GX_TF_RGBA8_PC);
  texture_tev();
  tex_quad(0, 0, 32, 32, 0.f, 1.f);
}
void expect_rgba8(Image& e) {
  for (u32 y = 0; y < Size; ++y) {
    for (u32 x = 0; x < Size; ++x) {
      e.px[y * Size + x] = texel_rgba(x / 8, y / 8);
    }
  }
}

// 7. converted formats: IA8, I8 (each 8x8 texels at 2 px) and CMPR
constexpr u32 intensity(u32 x, u32 y) { return x * 30 + y * 4; }
std::array<u8, 8 * 8 * 2> g_ia8;
std::array<u8, 8 * 8> g_i8;
std::array<u8, 32> g_cmpr;
GXTexObj g_texObj2;
GXTexObj g_texObj3;
void init_converted() {
  // IA8: 4x4 tiles, A then I per texel.
  size_t o = 0;
  for (u32 ty = 0; ty < 2; ++ty) {
    for (u32 tx = 0; tx < 2; ++tx) {
      for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
          g_ia8[o++] = 255;
          g_ia8[o++] = static_cast<u8>(intensity(tx * 4 + x, ty * 4 + y));
        }
      }
    }
  }
  // I8: 8x4 tiles; at width 8 this is plain row-major.
  for (u32 y = 0; y < 8; ++y) {
    for (u32 x = 0; x < 8; ++x) {
      g_i8[y * 8 + x] = static_cast<u8>(intensity(x, y));
    }
  }
  // CMPR: four 4x4 DXT1 sub-blocks (TL, TR, BL, BR), big-endian RGB565 endpoints red/blue (c0 > c1).
  const u8 rows[4] = {0x00, 0x55, 0x1B, 0xFF};
  for (int s = 0; s < 4; ++s) {
    u8* b = &g_cmpr[s * 8];
    b[0] = 0xF8;
    b[1] = 0x00;
    b[2] = 0x00;
    b[3] = 0x1F;
    std::memset(b + 4, rows[s], 4);
  }
}
void draw_converted() {
  init_converted();
  background();
  desc(GX_DIRECT, GX_DIRECT, GX_DIRECT);
  texture_tev();
  load_nearest(&g_texObj, g_ia8.data(), 8, 8, GX_TF_IA8);
  tex_quad(0, 0, 16, 16, 0.f, 1.f);
  load_nearest(&g_texObj2, g_i8.data(), 8, 8, GX_TF_I8);
  tex_quad(16, 0, 32, 16, 0.f, 1.f);
  load_nearest(&g_texObj3, g_cmpr.data(), 8, 8, GX_TF_CMPR);
  tex_quad(0, 16, 16, 32, 0.f, 1.f);
}
void expect_converted(Image& e) {
  for (u32 y = 0; y < 16; ++y) {
    for (u32 x = 0; x < 16; ++x) {
      const u32 v = intensity(x / 2, y / 2);
      e.px[y * Size + x] = rgb(v, v, v);
      e.px[y * Size + 16 + x] = rgb(v, v, v);
    }
  }
  const u32 red = rgb(255, 0, 0), blue = rgb(0, 0, 255);
  const u32 mid0 = rgb(159, 0, 95), mid1 = rgb(95, 0, 159);  // GX blends DXT1 at 5/8 + 3/8, not 2/3 + 1/3
  const u32 row[4] = {red, blue, mid0, mid1};
  for (u32 ty = 0; ty < 8; ++ty) {
    for (u32 tx = 0; tx < 8; ++tx) {
      u32 c;
      if (ty < 4) {
        c = tx < 4 ? red : blue;
      } else {
        c = tx < 4 ? row[tx] : mid1;
      }
      fill(e, tx * 2, 16 + ty * 2, tx * 2 + 2, 16 + ty * 2 + 2, c);
    }
  }
}

// 8. alpha blend (left) and alpha-compare discard (right)
void draw_blend_alpha() {
  desc(GX_DIRECT, GX_DIRECT);
  quad(0, 0, 32, 32, -5, {0, 0, 255, 255});
  GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  quad(0, 0, 16, 32, -5, {255, 0, 0, 128});
  GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  GXSetAlphaCompare(GX_GREATER, 100, GX_AOP_AND, GX_ALWAYS, 0);
  quad(16, 0, 32, 16, -5, {0, 255, 0, 50});
  quad(16, 16, 32, 32, -5, {0, 255, 0, 200});
}
void expect_blend_alpha(Image& e) {
  fill(e, 0, 0, 32, 32, rgb(0, 0, 255));
  fill(e, 0, 0, 16, 32, rgb(128, 0, 127));
  fill(e, 16, 16, 32, 32, rgb(0, 255, 0));
}

// 9. depth test, both draw orders (left: near first, right: far first)
void draw_depth() {
  background();
  desc(GX_DIRECT, GX_DIRECT);
  GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
  quad(2, 2, 12, 30, -2, {0, 255, 0, 255});
  quad(4, 4, 14, 28, -8, {255, 0, 0, 255});
  quad(16 + 4, 4, 16 + 14, 28, -8, {255, 0, 0, 255});
  quad(16 + 2, 2, 16 + 12, 30, -2, {0, 255, 0, 255});
}
void expect_depth(Image& e) {
  for (int ox : {0, 16}) {
    fill(e, ox + 4, 4, ox + 14, 28, rgb(255, 0, 0));
    fill(e, ox + 2, 2, ox + 12, 30, rgb(0, 255, 0));
  }
}

// 10. EFB copy to a texture, then sampled mirrored into the target
char g_copyKey[2];
void draw_efb_copy() {
  background();
  desc(GX_DIRECT, GX_DIRECT);
  quad(0, 0, 16, 16, -5, {255, 0, 0, 255});
  quad(16, 0, 32, 16, -5, {0, 255, 0, 255});
  quad(0, 16, 16, 32, -5, {0, 0, 255, 255});
  quad(16, 16, 32, 32, -5, {255, 255, 0, 255});
  GXSetTexCopySrc(0, 0, Size, Size);
  GXSetTexCopyDst(Size, Size, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(&g_copyKey[0], GX_FALSE);
  desc(GX_DIRECT, GX_DIRECT, GX_DIRECT);
  load_nearest(&g_texObj, &g_copyKey[0], Size, Size, GX_TF_RGBA8);
  texture_tev();
  tex_quad(0, 0, 32, 32, 1.f, 0.f);
}
void expect_efb_copy(Image& e) {
  fill(e, 0, 0, 16, 16, rgb(0, 255, 0));
  fill(e, 16, 0, 32, 16, rgb(255, 0, 0));
  fill(e, 0, 16, 16, 32, rgb(255, 255, 0));
  fill(e, 16, 16, 32, 32, rgb(0, 0, 255));
}

// 11. EFB cleared by a copy, then copied: the copy and readback path with no draw in it.
// Changes the copy clear colour; the caller restores the game's.
constexpr Rgba kClearCopyColor{40, 160, 220, 255};
char g_clearKey;
void draw_clear_copy() {
  GXSetCopyClear(GXColor{kClearCopyColor.r, kClearCopyColor.g, kClearCopyColor.b, kClearCopyColor.a}, 0xFFFFFF);
  GXSetTexCopySrc(0, 0, Size, Size);
  GXSetTexCopyDst(Size, Size, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(&g_clearKey, GX_TRUE);
}
void expect_clear_copy(Image& e) {
  fill(e, 0, 0, Size, Size, rgb(kClearCopyColor.r, kClearCopyColor.g, kClearCopyColor.b));
}

constexpr Case Cases[] = {
    {"direct-vertices", draw_direct, expect_direct},
    {"indexed8-pos-color", draw_indexed8, expect_indexed8},
    {"indexed16-pos", draw_indexed16, expect_indexed16},
    {"tev-konst-register", draw_konst_reg, expect_konst_reg},
    {"tev-3-stage", draw_tev3, expect_tev3},
    {"texture-rgba8", draw_rgba8, expect_rgba8},
    {"texture-ia8-i8-cmpr", draw_converted, expect_converted},
    {"blend-alpha-compare", draw_blend_alpha, expect_blend_alpha},
    {"depth-both-orders", draw_depth, expect_depth},
    {"efb-copy-sampled", draw_efb_copy, expect_efb_copy},
    {"efb-clear-copy", draw_clear_copy, expect_clear_copy},
};
constexpr size_t ClearCopyCase = 10;
static_assert(std::string_view{Cases[ClearCopyCase].name} == "efb-clear-copy");
constexpr size_t CaseCount = std::size(Cases);
static char g_keys[CaseCount];

std::string hex(u32 rgb24) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%06X", rgb24);
  return buf;
}

// ---- readback and comparison -------------------------------------------------------------------------
// Every byte of the readback buffer before any copy lands in it: a pixel still holding it was never written.
constexpr u8 FillMarker = 0xA5;

// The `readback` check's texture: four quadrants, written from the CPU and copied straight back.
constexpr u32 kReadbackQuadrant[4] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), rgb(255, 255, 255)};
u32 readback_pattern(u32 x, u32 y) { return kReadbackQuadrant[(y >= Size / 2 ? 2 : 0) + (x >= Size / 2 ? 1 : 0)]; }

constexpr const char* ReadbackBroken =
    "reading textures back fails on this device, so the case results say nothing about drawing";
// The cases draw offscreen and never present, so a clean pass rules out less than it seems to.
constexpr const char* AllPassed =
    "every check passed: offscreen drawing, EFB copies and readback work, so a fault still on screen is "
    "in presenting the frame (swapchain) or in something these cases don't draw";

// The stage the failures point at, from the two checks that leave stages out; empty when nothing stands out.
const char* diagnosis() {
  if (!g.evaluated) {
    return "";
  }
  if (!g.readbackResult.pass) {
    return ReadbackBroken;
  }
  if (!g.results[ClearCopyCase].pass) {
    return "EFB copies fail even with no draw in them (readback works)";
  }
  if (std::all_of(g.results.begin(), g.results.end(), [](const CaseResult& r) { return r.pass; })) {
    return AllPassed;
  }
  const bool anyDrawPassed = std::any_of(g.results.begin(), g.results.end(), [](const CaseResult& r) {
    return r.pass && r.name != Cases[ClearCopyCase].name;
  });
  return anyDrawPassed ? "" : "draws do not reach the framebuffer (readback and EFB copies work)";
}

void finish_locked(const char* note) {
  if (g.warmup) {
    g.readback = {};
    g.handles.clear();
    g.phase = Phase::Idle;
    return;
  }
  size_t passed = 0;
  const auto log_result = [&passed](const CaseResult& r) {
    passed += r.pass ? 1 : 0;
    if (r.pass) {
      Log.info("gpu selftest: {}: PASS ({:.1f} ms)", r.name, r.recordMs);
    } else {
      Log.warn("gpu selftest: {}: FAIL ({}) ({:.1f} ms)", r.name, r.detail, r.recordMs);
    }
  };
  log_result(g.readbackResult);
  for (const auto& r : g.results) {
    log_result(r);
  }
  const char* const cause = diagnosis();
  if (cause == AllPassed) {
    Log.info("gpu selftest: diagnosis: {}", cause);
  } else if (cause[0] != '\0') {
    Log.warn("gpu selftest: diagnosis: {}", cause);
  }
  const auto total =
      std::chrono::duration<double, std::milli>(Clock::now() - g.started).count();
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%zu/%zu passed%s%s (%.0f ms)", passed, g.results.size() + 1, note,
                cause == ReadbackBroken ? ", readback broken" : "", total);
  g.summary = buf;
  Log.info("gpu selftest: {}", g.summary);
  g.readback = {};
  g.handles.clear();
  g.phase = Phase::Idle;
}

// Compares one 32x32 block of the readback with the expected image. Alpha is not compared, but it is shown.
void compare_block(const u8* block, bool bgra, const Image& want, CaseResult& r) {
  size_t wrong = 0;
  size_t unwritten = 0;
  int firstX = -1, firstY = -1;
  u32 firstGot = 0, firstAlpha = 0;
  for (u32 y = 0; y < Size; ++y) {
    for (u32 x = 0; x < Size; ++x) {
      const u8* p = block + y * RowBytes + x * 4;
      const u32 rr = bgra ? p[2] : p[0];
      const u32 gg = p[1];
      const u32 bb = bgra ? p[0] : p[2];
      const u32 w = want.px[y * Size + x];
      const bool ok = std::abs(static_cast<int>(rr) - static_cast<int>(w >> 16 & 0xFF)) <= Tolerance &&
                      std::abs(static_cast<int>(gg) - static_cast<int>(w >> 8 & 0xFF)) <= Tolerance &&
                      std::abs(static_cast<int>(bb) - static_cast<int>(w & 0xFF)) <= Tolerance;
      if (!ok) {
        if (wrong == 0) {
          firstX = static_cast<int>(x);
          firstY = static_cast<int>(y);
          firstGot = rgb(rr, gg, bb);
          firstAlpha = p[3];
        }
        ++wrong;
        unwritten += p[0] == FillMarker && p[1] == FillMarker && p[2] == FillMarker && p[3] == FillMarker;
      }
    }
  }
  r.pass = wrong == 0;
  if (r.pass) {
    return;
  }
  if (unwritten == Size * Size) {
    r.detail = "nothing was copied back: every pixel still holds the readback buffer's fill";
    return;
  }
  r.detail = fmt::format("pixel {},{} got {} alpha {:02X} want {}, {}/{} pixels wrong", firstX, firstY, hex(firstGot),
                         firstAlpha, hex(want.px[firstY * Size + firstX]), wrong, Size * Size);
  if (unwritten != 0) {
    r.detail += fmt::format(", {} never written", unwritten);
  }
}

void evaluate(const u8* data) {
  g.evaluated = true;
  Image readbackWant;
  for (u32 y = 0; y < Size; ++y) {
    for (u32 x = 0; x < Size; ++x) {
      readbackWant.px[y * Size + x] = readback_pattern(x, y);
    }
  }
  compare_block(data + g.results.size() * BlockBytes, false, readbackWant, g.readbackResult);
  for (size_t i = 0; i < g.results.size(); ++i) {
    auto& r = g.results[i];
    if (!g.handles[i]) {
      r.pass = false;
      r.detail = "no copy texture";
      continue;
    }
    const auto format = g.handles[i]->format;
    const bool bgra = format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::BGRA8UnormSrgb;
    const bool rgba = format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::RGBA8UnormSrgb;
    if (!bgra && !rgba) {
      r.pass = false;
      r.detail = fmt::format("unsupported copy format {}", magic_enum::enum_name(format));
      continue;
    }
    compare_block(data + i * BlockBytes, bgra, g.expected[i], r);
  }
}

void encode(const EncoderTaskContext&, const wgpu::CommandEncoder& cmd, const void*, size_t, void*) {
  std::lock_guard lock{g.mutex};
  if (g.phase != Phase::Pending || g.handles.empty()) {
    return;
  }
  // One block per case, then the `readback` check's.
  const wgpu::BufferDescriptor desc{
      .label = "GPU self-test readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = static_cast<uint64_t>(BlockBytes) * (g.handles.size() + 1),
      .mappedAtCreation = true,
  };
  g.readback = webgpu::g_device.CreateBuffer(&desc);
  if (void* fill = g.readback.GetMappedRange(0, desc.size); fill != nullptr) {
    std::memset(fill, FillMarker, desc.size);
  }
  g.readback.Unmap();

  // The `readback` check: a texture written from a CPU buffer, copied back with no GX in between.
  const wgpu::BufferDescriptor patternDesc{
      .label = "GPU self-test readback pattern",
      .usage = wgpu::BufferUsage::CopySrc,
      .size = BlockBytes,
      .mappedAtCreation = true,
  };
  const wgpu::Buffer pattern = webgpu::g_device.CreateBuffer(&patternDesc);
  if (auto* px = static_cast<u8*>(pattern.GetMappedRange(0, BlockBytes)); px != nullptr) {
    for (u32 y = 0; y < Size; ++y) {
      for (u32 x = 0; x < Size; ++x) {
        const u32 c = readback_pattern(x, y);
        u8* p = px + y * RowBytes + x * 4;
        p[0] = c >> 16 & 0xFF;
        p[1] = c >> 8 & 0xFF;
        p[2] = c & 0xFF;
        p[3] = 0xFF;
      }
    }
  }
  pattern.Unmap();
  const wgpu::TextureDescriptor textureDesc{
      .label = "GPU self-test readback texture",
      .usage = wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
      .size = {Size, Size, 1},
      .format = wgpu::TextureFormat::RGBA8Unorm,
  };
  const wgpu::Texture texture = webgpu::g_device.CreateTexture(&textureDesc);
  const wgpu::Extent3D extent{Size, Size, 1};
  {
    const wgpu::TexelCopyBufferInfo from{.layout = {.bytesPerRow = RowBytes, .rowsPerImage = Size}, .buffer = pattern};
    const wgpu::TexelCopyTextureInfo to{.texture = texture};
    cmd.CopyBufferToTexture(&from, &to, &extent);
    const wgpu::TexelCopyTextureInfo back{.texture = texture};
    const wgpu::TexelCopyBufferInfo target{
        .layout = {.offset = static_cast<uint64_t>(BlockBytes) * g.handles.size(),
                   .bytesPerRow = RowBytes,
                   .rowsPerImage = Size},
        .buffer = g.readback,
    };
    cmd.CopyTextureToBuffer(&back, &target, &extent);
  }

  for (size_t i = 0; i < g.handles.size(); ++i) {
    const auto& h = g.handles[i];
    if (!h || h->size.width < Size || h->size.height < Size) {
      continue;
    }
    const wgpu::TexelCopyTextureInfo source{.texture = h->texture};
    const wgpu::TexelCopyBufferInfo target{
        .layout = {.offset = static_cast<uint64_t>(BlockBytes) * i, .bytesPerRow = RowBytes, .rowsPerImage = Size},
        .buffer = g.readback,
    };
    cmd.CopyTextureToBuffer(&source, &target, &extent);
  }
}

void after_submit(const EncoderTaskCompletionContext&, const void*, size_t, void*) {
  wgpu::Buffer buffer;
  uint64_t size = 0;
  {
    std::lock_guard lock{g.mutex};
    if (g.phase != Phase::Pending || !g.readback) {
      return;
    }
    buffer = g.readback;
    size = buffer.GetSize();
  }
  buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowSpontaneous,
                  [buffer, size](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                    std::lock_guard lock{g.mutex};
                    if (g.phase != Phase::Pending || g.readback.Get() != buffer.Get()) {
                      return;
                    }
                    if (status != wgpu::MapAsyncStatus::Success) {
                      Log.warn("gpu selftest: readback mapping failed {}: {}", magic_enum::enum_name(status),
                               std::string_view{message});
                      g.readbackResult.pass = false;
                      g.readbackResult.detail = "readback failed";
                      for (auto& r : g.results) {
                        r.pass = false;
                        r.detail = "readback failed";
                      }
                    } else {
                      const auto* mapped = static_cast<const u8*>(buffer.GetConstMappedRange(0, size));
                      if (mapped != nullptr) {
                        evaluate(mapped);
                      }
                      buffer.Unmap();
                    }
                    finish_locked("");
                  });
}

void log_header() {
  Log.info("gpu selftest: backend {}, adapter \"{}\", clamped storage loads {}",
           magic_enum::enum_name(webgpu::g_backendType), std::string_view{webgpu::g_adapterInfo.device},
           gx::storage_load_clamp_active() ? "active" : "off");
}
} // namespace
} // namespace aurora::gfx::selftest

using namespace aurora;
using namespace aurora::gfx::selftest;

bool aurora_gpu_selftest_pending() {
  std::lock_guard lock{g.mutex};
  return g.phase == Phase::Pending;
}

size_t aurora_gpu_selftest_summary(char* buf, size_t size) {
  std::lock_guard lock{g.mutex};
  if (buf != nullptr && size > 0) {
    std::snprintf(buf, size, "%s", g.summary.c_str());
  }
  return g.summary.size();
}

bool aurora_gpu_selftest_run(bool warmup) {
  {
    std::lock_guard lock{g.mutex};
    if (g.phase == Phase::Pending) {
      if (Clock::now() - g.started < StaleAfter) {
        return false;
      }
      Log.warn("gpu selftest: previous run never returned its readback; starting over");
      g.readback = {};
      g.handles.clear();
      g.phase = Phase::Idle;
    }
  }
  if (!webgpu::g_device) {
    return false;
  }
  if (g.task == gfx::InvalidEncoderTask) {
    g.task = gfx::register_encoder_task_type({
        .label = "GPU self-test readback",
        .callback = encode,
        .afterSubmit = after_submit,
    });
  }

  const auto started = Clock::now();
  g.warmup = warmup;
  if (!warmup) {
    log_header();
  }
  f32 proj[7];
  f32 vp[6];
  u32 sc[4];
  GXCullMode cull;
  GXGetProjectionv(proj);
  GXGetViewportv(vp);
  GXGetScissor(&sc[0], &sc[1], &sc[2], &sc[3]);
  GXGetCullMode(&cull);

  std::vector<CaseResult> results(CaseCount);
  std::vector<Image> expected(CaseCount);
  const BlockingPipelines blockingPipelines;
  for (size_t i = 0; i < CaseCount; ++i) {
    const auto t0 = Clock::now();
    GXCreateFrameBuffer(Size, Size);
    base_state();
    Cases[i].draw();
    GXSetTexCopySrc(0, 0, Size, Size);
    GXSetTexCopyDst(Size, Size, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(&g_keys[i], GX_FALSE);
    GXRestoreFrameBuffer();
    results[i].name = Cases[i].name;
    results[i].recordMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    Cases[i].expect(expected[i]);
  }
  gx::fifo::drain();

  std::vector<gfx::TextureHandle> handles(CaseCount);
  for (size_t i = 0; i < CaseCount; ++i) {
    const auto it = gx::g_gxState.copyTextures.find(&g_keys[i]);
    if (it != gx::g_gxState.copyTextures.end()) {
      handles[i] = it->second.handle;
    }
  }

  GXSetProjectionv(proj);
  GXSetViewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
  GXSetScissor(sc[0], sc[1], sc[2], sc[3]);
  GXSetCullMode(cull);

  {
    std::lock_guard lock{g.mutex};
    g.results = std::move(results);
    g.readbackResult = {.name = "readback"};
    g.evaluated = false;
    g.expected = std::move(expected);
    g.handles = std::move(handles);
    g.started = started;
    g.phase = Phase::Pending;
  }
  if (!gfx::push_encoder_task(g.task, "", 1)) {
    std::lock_guard lock{g.mutex};
    g.readbackResult.detail = "no active render pass";
    for (auto& r : g.results) {
      r.pass = false;
      r.detail = "no active render pass";
    }
    finish_locked("");
    return false;
  }
  return true;
}
