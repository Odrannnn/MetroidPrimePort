#include "command_processor.hpp"

#include "../gfx/bloom.hpp"
#include "../gfx/volfog.hpp"
#include "../gfx/depth_peek.hpp"
#include "../gfx/probe.hpp"
#include "../gfx/shadow.hpp"
#include "../gfx/hash.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/recording.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "pipeline.hpp"
#include "regs.hpp"
#include "resident.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <memory>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  // Sized once and written directly: this runs for nearly every draw.
  u32 numIndices = 0;
  if (prim == GX_QUADS) {
    numIndices = (u32(vtxCount) + 3) / 4 * 6;
  } else if (prim == GX_TRIANGLES) {
    numIndices = vtxCount;
  } else if (prim == GX_TRIANGLEFAN || prim == GX_TRIANGLESTRIP) {
    numIndices = vtxCount <= 3 ? vtxCount : 3 + (u32(vtxCount) - 3) * 3;
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  u16* out = reinterpret_cast<u16*>(buf.append_uninit(numIndices * sizeof(u16)));
  if (prim == GX_QUADS) {
    for (u32 v = 0; v < vtxCount; v += 4) {
      const u16 idx0 = vtxStart + v;
      *out++ = idx0;
      *out++ = idx0 + 1;
      *out++ = idx0 + 2;
      *out++ = idx0 + 2;
      *out++ = idx0 + 3;
      *out++ = idx0;
    }
  } else if (prim == GX_TRIANGLES) {
    for (u32 v = 0; v < vtxCount; ++v) {
      *out++ = vtxStart + v;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v >= 3) {
        *out++ = vtxStart;
        *out++ = idx - 1;
      }
      *out++ = idx;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v >= 3) {
        *out++ = (v & 1) == 0 ? idx - 2 : idx - 1;
        *out++ = (v & 1) == 0 ? idx - 1 : idx - 2;
      }
      *out++ = idx;
    }
  } else {
    *out++ = 0;
    *out++ = 1;
    *out++ = 3;
    *out++ = 3;
    *out++ = 2;
    *out++ = 0;
  }
  return static_cast<u16>(numIndices);
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct PipelineMemo {
  ShaderInfo shaderInfo{};
  gfx::PipelineRef shadowPipelineRef{};
  bool shadowCaster = false;
};
// Keyed by pipeline ref (the config's hash); entries stay valid until the config's pipeline is
// dropped, which gfx::touch_pipeline reports.
absl::flat_hash_map<gfx::PipelineRef, PipelineMemo> sPipelineMemo;

struct DrawCache {
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  // The sun's shadow-map pipeline, for a draw that casts (GXPortSetShadowCaster)
  gfx::PipelineRef shadowPipelineRef{};
  bool shadowCaster = false;
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
};
DrawCache sDrawCache;

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const f32 renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  return {
      .rangeK = state.rangeK,
      .rangeCenter = ((static_cast<f32>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
      .renderWidth = renderWidth,
      .targetWidth = gfx::get_render_target_size().x,
  };
}

std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::array<f32, 10> rangeK;
  for (u32 i = 0; i < rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<f32>(key.rangeK[source]) / 64.f;
  }

  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 screenX = ((static_cast<f32>(x) + 0.5f) / key.renderWidth) * 2.f - 1.f;
    const f32 offset = screenX - key.rangeCenter;
    const f32 rangeIndex = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);
    const u32 lower = static_cast<u32>(rangeIndex);
    const u32 upper = std::min(lower + 1, 9u);
    const f32 fraction = rangeIndex - static_cast<f32>(lower);
    const f32 k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;

ProcessResult process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  ByteReader reader{{data, size}};

  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      const u32 value = reader.read<u32>();
      handle_bp(value);
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
  return {size, false};
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

// A texgen reading a UV set the vertices don't carry draws with zero UVs (shader.cpp
// vtx_attr). Names the draw from GX_AURORA_SET_DRAW_TAG, once per model, material and
// texcoord, so a bad mod model can be found.
static void warn_missing_uv_sets(const ShaderConfig& config, const ShaderInfo& info) noexcept {
  static std::vector<std::array<u32, 4>> sReported;
  for (u32 i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if ((tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) || tcg.src < GX_TG_TEX0 || tcg.src > GX_TG_TEX7) {
      continue;
    }
    const u32 set = tcg.src - GX_TG_TEX0;
    if (config.attrs[GX_VA_TEX0 + set].attrType != GX_NONE) {
      continue;
    }
    const auto& tag = g_gxState.drawTag;
    const std::array<u32, 4> key{tag[0], tag[1], tag[2], i};
    if (sReported.size() >= 64 || std::find(sReported.begin(), sReported.end(), key) != sReported.end()) {
      continue;
    }
    sReported.push_back(key);
    if (tag[0] == 0 && tag[1] == UINT32_MAX) {
      Log.warn("untagged draw: texcoord {} reads UV set {}, which its vertices lack{}", i, set,
               config.pbr ? " (PBR)" : "");
    } else {
      Log.warn("model {:08X} (index {}) material {}: texcoord {} reads UV set {}, which its vertices lack{}", tag[0],
               static_cast<s32>(tag[1]), tag[2], i, set, config.pbr ? " (PBR)" : "");
    }
  }
}

// A draw whose data the frame's buffers have no room left for (e.g. an oversized replacement
// model) is dropped instead of aborting the game: true when range overflowed. Warns once per
// model. Whatever was pushed before the drop is orphaned, so the next draw must not merge.
static bool drop_overflowed_draw(gfx::Range range, const char* buffer) noexcept {
  if (!gfx::overflowed(range))
    LIKELY { return false; }
  sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
  static std::vector<u32> sReported;
  const auto& tag = g_gxState.drawTag;
  if (sReported.size() < 64 && std::find(sReported.begin(), sReported.end(), tag[0]) == sReported.end()) {
    sReported.push_back(tag[0]);
    if (tag[0] == 0 && tag[1] == UINT32_MAX) {
      Log.warn("untagged draw dropped: the frame's {} buffer is full", buffer);
    } else {
      Log.warn("model {:08X} (index {}) material {}: draw dropped, the frame's {} buffer is full", tag[0],
               static_cast<s32>(tag[1]), tag[2], buffer);
    }
  }
  return true;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;
  if (drop_overflowed_draw(vertRange, "vertex") || drop_overflowed_draw(idxRange, "index")) {
    return;
  }

  DrawImmediateData immediates{
      .vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx, .serial = state.drawSerial};
  const bool bindPos = bind_pos_active();
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16 && !(bindPos && i == GX_VA_TEX7)) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0 && !resident::array_range(array.data, array.size, array.cachedRange)) {
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
      if (drop_overflowed_draw(array.cachedRange, "storage")) {
        array.cachedRange = {};
        return;
      }
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset + array.baseIndex * array.stride;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.config.msaaSamples == gfx::get_sample_count();
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    const bool prevUsesVolFog = cache.shaderInfo.usesVolFog;
    const bool prevShadowReceive = cache.shaderInfo.shadowReceive;
    const bool prevUsesLightmap = cache.shaderInfo.usesLightmap;
    populate_pipeline_config(cache.config, prim, fmt);
    // A frame switches pipelines hundreds of times between a few hundred configs. The config's
    // hash is the pipeline's ref, so a config seen before reuses its shader info and shadow
    // pipeline without the second lookup (shader info depends on the config alone).
    const gfx::PipelineRef ref = xxh3_hash(cache.config, static_cast<HashType>(gfx::ShaderType::GX));
    auto memoIt = sPipelineMemo.find(ref);
    if (memoIt != sPipelineMemo.end() && gfx::touch_pipeline(ref) &&
        (!memoIt->second.shadowCaster || gfx::touch_pipeline(memoIt->second.shadowPipelineRef))) {
      const auto& memo = memoIt->second;
      cache.shaderInfo = memo.shaderInfo;
      cache.pipelineRef = ref;
      cache.shadowCaster = memo.shadowCaster;
      cache.shadowPipelineRef = memo.shadowPipelineRef;
    } else {
      cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
      warn_missing_uv_sets(cache.config.shaderConfig, cache.shaderInfo);
      cache.pipelineRef = gfx::pipeline_ref(cache.config);
      // Only an opaque surface that writes depth casts: the shadow map's vertex-only pass can't
      // alpha-test or blend. The game draws most opaque surfaces as a ONE/ZERO blend.
      const auto& sc = cache.config.shaderConfig;
      const bool opaque = cache.config.blendMode == GX_BM_NONE ||
                          (cache.config.blendMode == GX_BM_BLEND && cache.config.blendFacSrc == GX_BL_ONE &&
                           cache.config.blendFacDst == GX_BL_ZERO);
      cache.shadowCaster = cache.shaderInfo.usesShadow && cache.config.depthCompare && cache.config.depthUpdate &&
                           opaque && !sc.alphaCompare && sc.depthOnly == 0;
      if (cache.shadowCaster) {
        PipelineConfig shadowConfig = cache.config;
        shadowConfig.shadowPass = 1;
        shadowConfig.msaaSamples = 1;
        cache.shadowPipelineRef = gfx::pipeline_ref(shadowConfig);
      }
      sPipelineMemo.insert_or_assign(cache.pipelineRef, PipelineMemo{
                                                            .shaderInfo = cache.shaderInfo,
                                                            .shadowPipelineRef = cache.shadowPipelineRef,
                                                            .shadowCaster = cache.shadowCaster,
                                                        });
    }
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures ||
        prevUsesVolFog != cache.shaderInfo.usesVolFog || prevUsesLightmap != cache.shaderInfo.usesLightmap ||
        prevShadowReceive != cache.shaderInfo.shadowReceive) {
      cache.bindGeneration = 0;
    }
  }

  note_draw_shader(state.drawSerial, cache.config.shaderConfig);

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    resolve_sampled_textures(cache.shaderInfo);
    cache.bindGroups = build_bind_groups(cache.shaderInfo);
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }

  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    cache.uniformRange = build_uniform(cache.shaderInfo);
    if (drop_overflowed_draw(cache.uniformRange, "uniform")) {
      cache.uniformRange = {};
      return;
    }
    state.dirty &= ~DirtyUniform;
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      if (drop_overflowed_draw(cache.fogRange, "storage")) {
        cache.hasFogRange = false;
        return;
      }
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  const DrawData draw{
      .pipeline = cache.pipelineRef,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .bindGroups = cache.bindGroups,
      .dstAlpha = state.dstAlpha,
      .shadowGroup = cache.shaderInfo.shadowReceive,
  };
  if (!state.shadowCasterOnly) {
    gfx::push_draw_command(draw);
  }
  if (cache.shadowCaster) {
    DrawData caster = draw;
    caster.pipeline = cache.shadowPipelineRef;
    caster.bindGroups = {};
    gfx::shadow::add_caster(caster);
  }
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }

  const bool cleanState = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1;

  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.
  const auto vertexData = reader.take(totalVtxBytes);
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);
  if (drop_overflowed_draw(vertRange, "vertex")) {
    return;
  }

  // Try to merge with previous draw call
  if (canMerge) {
    u32 numIndices = 0;
    gfx::Range idxRange;
    static ByteBuffer idxBuf;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    const u32 prevIndexCount = lastDraw->indexCount;
    if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
      // Generate triangle index buffer for previous draw
      lastDraw->indexCount = prepare_idx_buffer(idxBuf, GX_TRIANGLES, 0, lastDraw->vtxCount);
    }
    if (lastDraw->indexCount != 0) {
      numIndices += prepare_idx_buffer(idxBuf, prim, lastDraw->vtxCount, vtxCount);
      idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), hadIndexRange ? 0 : 4);
      idxBuf.clear();
      if (drop_overflowed_draw(idxRange, "index")) {
        lastDraw->indexCount = prevIndexCount;
        return;
      }
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    gfx::detail::increment_merged_draw_count();
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
}

// How many indices prepare_idx_buffer makes of a draw, or 0 for one it cannot make into
// whole triangles.
static u32 triangle_index_count(GXPrimitive prim, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_TRIANGLES:
    return vtxCount >= 3 && vtxCount % 3 == 0 ? vtxCount : 0;
  case GX_QUADS:
    return vtxCount >= 4 && vtxCount % 4 == 0 ? vtxCount / 4 * 6 : 0;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN:
    return vtxCount >= 3 ? (u32(vtxCount) - 2) * 3 : 0;
  default:
    return 0;
  }
}

static u32 current_vtx_size(GXVtxFmt fmt) noexcept {
  return g_gxState.lastVtxFmt == fmt ? g_gxState.lastVtxSize : calc_vtx_size(fmt);
}

// Makes a retained display list into triangle lists in the resident buffers, as the
// processor would draw it now (merged draws, with their index buffers). False when it holds
// anything but triangle draws, or there is no room for it.
static bool build_resident_dl(resident::Entry& entry) noexcept {
  ZoneScoped;
  const std::vector<u8>& bytes = *entry.bytes;
  std::array<u32, GX_MAX_VTXFMT> sizes{};
  std::vector<resident::Chunk> chunks;
  std::vector<u8> verts;
  std::vector<u8> indices;
  ByteBuffer idxBuf;
  size_t pos = 0;
  while (pos < bytes.size()) {
    const u8 cmd = bytes[pos++];
    if (cmd == CP_CMD_NOP) {
      continue;
    }
    if (cmd < 0x80 || bytes.size() - pos < 2) {
      return false;
    }
    const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    const u16 vtxCount = static_cast<u16>((bytes[pos] << 8) | bytes[pos + 1]);
    pos += 2;
    const u32 numIndices = triangle_index_count(prim, vtxCount);
    if (numIndices == 0 || numIndices > 0xFFFF) {
      return false;
    }
    if (sizes[fmt] == 0) {
      sizes[fmt] = current_vtx_size(fmt);
    }
    const size_t vtxBytes = size_t(vtxCount) * sizes[fmt];
    if (sizes[fmt] == 0 || vtxBytes > bytes.size() - pos) {
      return false;
    }
    if (chunks.empty() || chunks.back().fmt != fmt || u32(chunks.back().vtxCount) + vtxCount > 0xFFFF) {
      // A new draw starts where push_verts and push_indices would start one: 4-aligned.
      verts.resize(AURORA_ALIGN(verts.size(), 4));
      indices.resize(AURORA_ALIGN(indices.size(), 4));
      chunks.push_back(resident::Chunk{
          .fmt = fmt,
          .vtxCount = 0,
          .vertOffset = static_cast<u32>(verts.size()),
          .vertSize = 0,
          .idxOffset = static_cast<u32>(indices.size()),
          .idxCount = 0,
      });
    }
    resident::Chunk& chunk = chunks.back();
    verts.insert(verts.end(), bytes.begin() + pos, bytes.begin() + pos + vtxBytes);
    pos += vtxBytes;
    idxBuf.clear();
    const u32 made = prepare_idx_buffer(idxBuf, prim, chunk.vtxCount, vtxCount);
    indices.insert(indices.end(), idxBuf.data(), idxBuf.data() + idxBuf.size());
    chunk.vtxCount = static_cast<u16>(chunk.vtxCount + vtxCount);
    chunk.vertSize += static_cast<u32>(vtxBytes);
    chunk.idxCount += made;
  }
  if (chunks.empty() || !resident::store_dl(entry, verts, indices)) {
    return false;
  }
  entry.vtxSizes = sizes;
  entry.chunks = std::move(chunks);
  return true;
}

static void call_resident_dl(const void* key, u32 size) noexcept {
  ZoneScoped;
  resident::Entry* const entry = resident::find(key);
  if (entry == nullptr || size > entry->bytes->size()) {
    Log.error("GX_AURORA_RESIDENT_CALL_DL: {} is not retained with {} bytes", key, size);
    return;
  }
  if (!entry->chunks.empty()) {
    // Parsed with other vertex sizes: what the vertices are has changed since.
    for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
      const u32 parsed = entry->vtxSizes[fmt];
      if (parsed != 0 && parsed != current_vtx_size(static_cast<GXVtxFmt>(fmt))) {
        resident::free_dl(*entry);
        break;
      }
    }
  }
  if (entry->chunks.empty() && !entry->dlTried) {
    entry->dlTried = true;
    build_resident_dl(*entry);
  }
  if (entry->chunks.empty()) {
    // As GXCallDisplayList would have sent it.
    process(entry->bytes->data(), size);
    return;
  }
  for (const resident::Chunk& chunk : entry->chunks) {
    const gfx::Range vertRange{entry->vert.offset + chunk.vertOffset, chunk.vertSize};
    const gfx::Range idxRange{entry->idx.offset + chunk.idxOffset, chunk.idxCount * u32(sizeof(u16))};
    push_gx_draw(GX_TRIANGLES, chunk.fmt, chunk.vtxCount, vertRange, idxRange, chunk.idxCount);
  }
  // The next draw must not merge into these: its vertices are not after them.
  sDrawCache.lastDrawFmt = GX_MAX_VTXFMT;
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      // Only an indexed attribute's array, or TEX7's under pbr (bind_pos_active), is in the
      // pipeline config (a VCD or pbr change dirties it anyway).
      const auto type = g_gxState.vtxDesc[attrIdx];
      const bool inConfig =
          type == GX_INDEX8 || type == GX_INDEX16 || (attrIdx == GX_VA_TEX7 && g_gxState.pbr != 0);
      if (inConfig &&
          (array.le != le || (attrIdx == GX_VA_TEX7 && (array.data == nullptr) != (newData == nullptr)))) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newUserData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.userData != newUserData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.userData = newUserData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    g_gxState.texCopyMips = reader.read<u8>() != 0;
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_RESIDENT_RETAIN) {
    const auto* key = reinterpret_cast<const void*>(reader.read<u64>());
    std::unique_ptr<std::vector<u8>> bytes{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    resident::retain(key, std::move(bytes));
  } else if (subCmd == GX_AURORA_RESIDENT_RELEASE) {
    const auto* key = reinterpret_cast<const void*>(reader.read<u64>());
    resident::release(key);
    // An array still set to it reads its resident copy no more.
    for (auto& array : g_gxState.arrays) {
      if (array.data == key) {
        array.cachedRange = {};
      }
    }
  } else if (subCmd == GX_AURORA_RESIDENT_CALL_DL) {
    const auto* key = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 size = reader.read<u32>();
    call_resident_dl(key, size);
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    const gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), 4);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  } else if (subCmd == GX_AURORA_SET_DRAW_SYNC) {
    aurora::gx::set_draw_sync_token(reader.read<u16>());
  } else if (subCmd == GX_AURORA_LOAD_ARRAY_BASE_INDEX) {
    const u32 attrIdx = (reader.read<u8>() & 0x0f) + GX_VA_POS;
    const u32 base = reader.read<u32>();
    if (attrIdx < g_gxState.arrays.size() && g_gxState.arrays[attrIdx].baseIndex != base) {
      g_gxState.arrays[attrIdx].baseIndex = base;
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_SET_PBR) {
    const u8 pbr = reader.read<u8>();
    if (g_gxState.pbr != pbr) {
      g_gxState.pbr = pbr;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_SDF) {
    const u8 sdf = reader.read<u8>();
    if (g_gxState.sdf != sdf) {
      g_gxState.sdf = sdf;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_HUD_SAMPLE) {
    const u8 mode = reader.read<u8>();
    const f32 x = reader.read<f32>();
    const f32 y = reader.read<f32>();
    if (g_gxState.hudSample != mode) {
      g_gxState.hudSample = mode;
      g_gxState.dirty |= DirtyPipeline;
    }
    const Vec4<float> dyin{x, y, 0.f, 0.f};
    if (g_gxState.hudDyin != dyin) {
      g_gxState.hudDyin = dyin;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_PORT_DEPTH_PREPASS) {
    const u8 pass = reader.read<u8>();
    if (g_gxState.depthPrepass != pass) {
      g_gxState.depthPrepass = pass;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_DRAW_SERIAL) {
    const u32 serial = reader.read<u32>();
    if (g_gxState.drawSerial != serial) {
      g_gxState.drawSerial = serial;
      // Draws with different serials must not merge.
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_PORT_DRAW_ID_MODE) {
    const bool on = reader.read<u8>() != 0;
    if (g_gxState.drawIdMode != on) {
      g_gxState.drawIdMode = on;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_DRAW_TAG) {
    for (u32& value : g_gxState.drawTag) {
      value = reader.read<u32>();
    }
  } else if (subCmd == GX_AURORA_COPY_PROBE_FACE) {
    copy_probe_face(reader.read<u8>());
  } else if (subCmd == GX_AURORA_SET_PBR_PROBE) {
    Mat3x4<float> mtx;
    for (Vec4<float>* col : {&mtx.m0, &mtx.m1, &mtx.m2}) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      *col = Vec4<float>{x, y, z, w};
    }
    if (g_gxState.pbrProbe != mtx) {
      g_gxState.pbrProbe = mtx;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_CUBE) {
    const u32 id = reader.read<u32>();
    const u32 size = reader.read<u32>();
    const u32 mipCount = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_cube(id, size, mipCount, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_CUBE) {
    gfx::probe::destroy_cube(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_CUBE) {
    u32 id = reader.read<u32>();
    Vec4<float> value;
    for (int i = 0; i < 4; ++i) {
      value[i] = reader.read<f32>();
    }
    if (id == 0 || !gfx::probe::has_cube(id)) {
      // No such cube: the probe, which is not HDR.
      id = 0;
      value = {};
    }
    if (g_gxState.pbrCube != id) {
      g_gxState.pbrCube = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrCubeParams != value) {
      g_gxState.pbrCubeParams = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_AMBIENT) {
    for (Vec4<float>& v : g_gxState.pbrAmbient) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      const Vec4<float> value{x, y, z, w};
      if (v != value) {
        v = value;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_VOLUME) {
    const u32 id = reader.read<u32>();
    const u32 sizeX = reader.read<u32>();
    const u32 sizeY = reader.read<u32>();
    const u32 sizeZ = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_volume(id, sizeX, sizeY, sizeZ, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_VOLUME) {
    gfx::probe::destroy_volume(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_VOLUME) {
    u32 id = reader.read<u32>();
    std::array<Vec4<float>, 6> rows;
    for (Vec4<float>& v : rows) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      v = {x, y, z, w};
    }
    if (id == 0 || !gfx::probe::has_volume(id)) {
      id = 0;
      rows = {};
    }
    if (g_gxState.pbrVolume != id) {
      g_gxState.pbrVolume = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrVolumeRows != rows) {
      g_gxState.pbrVolumeRows = rows;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_LIGHTMAP) {
    const u32 id = reader.read<u32>();
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    const u32 layers = reader.read<u32>();
    const u32 format = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_lightmap(id, width, height, layers, format, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_LIGHTMAP) {
    gfx::probe::destroy_lightmap(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHTMAP) {
    u32 id = reader.read<u32>();
    Vec4<float> rect;
    {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      rect = {x, y, z, w};
    }
    std::array<Vec4<float>, 3> axes;
    for (Vec4<float>& v : axes) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      v = {x, y, z, 0.f};
    }
    if (id == 0 || !gfx::probe::has_lightmap(id)) {
      id = 0;
      rect = {};
      axes = {};
    }
    if (g_gxState.pbrLightmap != id) {
      g_gxState.pbrLightmap = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrLightmapRect != rect || g_gxState.pbrLightmapAxes != axes) {
      g_gxState.pbrLightmapRect = rect;
      g_gxState.pbrLightmapAxes = axes;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHTMAP_ATTR) {
    const u8 attr = reader.read<u8>();
    if (g_gxState.pbrLightmapAttr != attr) {
      g_gxState.pbrLightmapAttr = attr;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_BRDF_LUT) {
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    const bool on = gfx::probe::set_brdf_lut(texels->data(), texels->size());
    if (g_gxState.pbrBrdfLut != on) {
      g_gxState.pbrBrdfLut = on;
      g_gxState.dirty |= DirtyUniform;
    }
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_TONE) {
    std::array<Vec4<float>, 3> rows;
    for (Vec4<float>& v : rows) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      v = {x, y, z, w};
    }
    if (g_gxState.pbrTone != rows) {
      g_gxState.pbrTone = rows;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_PORT_POST_PROCESS) {
    u32 words[32];
    for (u32& word : words) {
      word = reader.read<u32>();
    }
    gfx::bloom::Params params;
    static_assert(sizeof(params) == sizeof(words));
    std::memcpy(&params, words, sizeof(params));
    gfx::bloom::record(params);
  } else if (subCmd == GX_AURORA_PORT_VOLUMETRIC_FOG) {
    gfx::volfog::Params params;
    u32 words[sizeof(params) / sizeof(u32)];
    for (u32& word : words) {
      word = reader.read<u32>();
    }
    std::memcpy(&params, words, sizeof(params));
    if (gfx::volfog::record(params)) {
      // The draws after it fog themselves through the froxels it fills.
      // w: where the world's depth range starts; nearer is the viewmodel (vf_depth).
      const Vec4<float> fogParams{params.depth[0], params.fog[0], params.colorA[3], params.depth[2]};
      std::array<Vec4<float>, 3> tone;
      for (size_t i = 0; i < tone.size(); ++i) {
        tone[i] = {params.tone[i][0], params.tone[i][1], params.tone[i][2], params.tone[i][3]};
      }
      // The viewmodel's depth reading (vf_depth) needs 1 - near / far; tone[0].w is unused.
      tone[0] = {params.tone[0][0], params.tone[0][1], params.tone[0][2], 1.f - params.depth[0] / params.depth[1]};
      if (!g_gxState.volFog) {
        g_gxState.volFog = true;
        g_gxState.dirty |= DirtyPipeline;
      }
      if (g_gxState.volFogParams != fogParams || g_gxState.volFogTone != tone) {
        g_gxState.volFogParams = fogParams;
        g_gxState.volFogTone = tone;
        g_gxState.dirty |= DirtyUniform;
      }
      // A new froxel texture when the frame's size changed.
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_PORT_VOLUMETRIC_FOG_END) {
    if (g_gxState.volFog) {
      g_gxState.volFog = false;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_PARTICLE_FOG) {
    const bool on = reader.read<u8>() != 0;
    if (g_gxState.particleFog != on) {
      g_gxState.particleFog = on;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_SHADOW_CASTER) {
    const u8 mode = reader.read<u8>();
    const bool on = mode != 0;
    g_gxState.shadowCasterOnly = mode == 2;
    if (g_gxState.shadowCaster != on) {
      g_gxState.shadowCaster = on;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_PORT_SHADOW_FRAME) {
    f32 worldToView[3][4];
    for (auto& row : worldToView) {
      for (f32& v : row) {
        v = reader.read<f32>();
      }
    }
    f32 sunDir[3];
    for (f32& v : sunDir) {
      v = reader.read<f32>();
    }
    const f32 radius = reader.read<f32>();
    f32 color[3];
    for (f32& v : color) {
      v = reader.read<f32>();
    }
    gfx::shadow::Uniform uniform{};
    const bool active = gfx::shadow::set_frame(worldToView, sunDir, radius, color, uniform);
    if (g_gxState.shadowActive != active) {
      g_gxState.shadowActive = active;
      g_gxState.dirty |= DirtyPipeline;
    }
    if (active) {
      static_assert(sizeof(uniform) == sizeof(g_gxState.shadowUniform));
      std::array<Vec4<float>, 10> values;
      std::memcpy(values.data(), &uniform, sizeof(uniform));
      if (g_gxState.shadowUniform != values) {
        g_gxState.shadowUniform = values;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  } else if (subCmd == GX_AURORA_PORT_ROOM_LIGHTS) {
    const u32 count = reader.read<u32>();
    std::vector<f32> records(static_cast<size_t>(count) * 16);
    for (f32& v : records) {
      v = reader.read<f32>();
    }
    Vec4<float> value{};
    if (count != 0) {
      const auto range = gfx::push_storage(reinterpret_cast<const uint8_t*>(records.data()), records.size() * sizeof(f32));
      if (!gfx::overflowed(range)) {
        const u32 base = range.offset / sizeof(u32);
        std::memcpy(&value.x(), &base, sizeof(base));
        value.y() = static_cast<f32>(count);
      }
    }
    // x holds a u32 offset as float bits: compare bits, since denormals may compare equal under FTZ/DAZ.
    if (std::memcmp(&g_gxState.pbrRoomLights, &value, sizeof(value)) != 0) {
      g_gxState.pbrRoomLights = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_PORT_SHADOW_RENDER) {
    gfx::shadow::record();
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHT_SKIP) {
    const Vec4<float> value{static_cast<f32>(reader.read<u32>() & 0xFF), 0.f, 0.f, 0.f};
    if (g_gxState.pbrLightSkip != value) {
      g_gxState.pbrLightSkip = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_BAKED_LIGHT_MODULATION) {
    Vec4<float> value{1.f, 1.f, 1.f, 0.f};
    value.x() = reader.read<f32>();
    value.y() = reader.read<f32>();
    value.z() = reader.read<f32>();
    if (g_gxState.pbrBakedLightModulation != value) {
      g_gxState.pbrBakedLightModulation = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_BACKLIGHT) {
    f32 v[11];
    for (f32& f : v) {
      f = reader.read<f32>();
    }
    const std::array<Vec4<float>, 3> value{
        Vec4<float>{v[0], v[1], v[2], v[3]},
        Vec4<float>{v[4], v[5], v[6], v[7]},
        Vec4<float>{v[8], v[9], v[10], 0.f},
    };
    if (g_gxState.pbrBacklightLights != value) {
      if ((g_gxState.pbrBacklightLights[2].z() > 0.f) != (value[2].z() > 0.f)) {
        g_gxState.dirty |= DirtyPipeline;
      }
      g_gxState.pbrBacklightLights = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_SHIELD) {
    std::array<Vec4<float>, 8> value;
    for (auto& row : value) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      row = Vec4<float>{x, y, z, w};
    }
    if (g_gxState.pbrShield != value) {
      g_gxState.pbrShield = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHT_HDR) {
    const u32 bit = reader.read<u32>() & 0xFF;
    f32 v[8];
    for (f32& f : v) {
      f = reader.read<f32>();
    }
    const u32 falloff = std::min(reader.read<u32>(), 3u);
    if (bit != 0) {
      const u32 idx = static_cast<u32>(std::countr_zero(bit));
      const bool on = v[7] > 0.f;
      const std::array<Vec4<float>, 3> rows{
          on ? Vec4<float>{v[0], v[1], v[2], static_cast<f32>(falloff + 1)} : Vec4<float>{},
          on ? Vec4<float>{v[3], v[4], v[5], v[6]} : Vec4<float>{},
          on ? Vec4<float>{v[7], 0.f, 0.f, 0.f} : Vec4<float>{},
      };
      for (u32 row = 0; row < 3; ++row) {
        if (g_gxState.pbrLightHdr[idx * 3 + row] != rows[row]) {
          g_gxState.pbrLightHdr[idx * 3 + row] = rows[row];
          g_gxState.dirty |= DirtyUniform;
        }
      }
    }
  } else if (subCmd == GX_AURORA_SET_PBR_LIGHT_SCALE) {
    const f32 diffuse = reader.read<f32>();
    const f32 f0 = reader.read<f32>();
    const f32 alpha = std::clamp(reader.read<f32>(), 0.f, 1.f);
    const bool alphaReplaces = reader.read<u32>() != 0;
    // w is the fade as the shader reads it: 0 none, 1 + alpha in place of the material's
    // alpha, -(1 + alpha) times it.
    const f32 fade = alphaReplaces ? 1.f + alpha : alpha < 1.f ? -(1.f + alpha) : 0.f;
    const Vec4<float> value{diffuse, f0, 0.f, fade};
    if (g_gxState.pbrLightScale != value) {
      g_gxState.pbrLightScale = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_MATERIAL) {
    for (Vec4<float>* v :
         {&g_gxState.pbrEmissive, &g_gxState.pbrBacklight, &g_gxState.pbrLayer, &g_gxState.pbrLayerHeight,
          &g_gxState.pbrParam, &g_gxState.pbrUp}) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      const Vec4<float> value{x, y, z, w};
      if (*v != value) {
        *v = value;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void clear_draw_cache() noexcept {
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
  // GX_AURORA_PORT_ROOM_LIGHTS points into this frame's storage buffer.
  if (g_gxState.pbrRoomLights.y() != 0.f) {
    g_gxState.pbrRoomLights = {};
    g_gxState.dirty |= DirtyUniform;
  }
}

} // namespace aurora::gx::fifo
