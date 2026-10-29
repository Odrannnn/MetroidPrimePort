#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gx/GXAurora.h"
#include "../../gfx/bloom.hpp"
#include "../../gfx/pipeline_cache.hpp"
#include "../../gfx/probe.hpp"
#include "../../gfx/shadow.hpp"
#include "../../gfx/actor_shadow.hpp"
#include "../../gfx/volfog.hpp"
#include "../../gfx/thermal.hpp"
#include "../../gfx/xray.hpp"
#include "../../webgpu/gpu.hpp"
#include "../../webgpu/gpu_prof.hpp"

#include <atomic>
#include <bit>
#include <cstring>
#include <vector>

namespace {
// GXPortSetDrawIdMode: the frame must reach the screen as the draws made it, so the post-processing and
// the volumetric fog are left out.
bool sDrawIdMode = false;

// Port: a PBR draw sets all of its probe, cube, ambient, volume, tone and material state
// per surface, and neighbouring surfaces almost always repeat it. The processor already
// ignores a repeat, so a repeat is not written at all; that saves a few hundred FIFO bytes
// per surface. These are only ever written from the game thread, and never recorded into a
// display list (which a skipped write would leave out).
template <typename T>
struct LastPBRWrite {
  T value{};
  bool valid = false;

  // True when `now` is what was written last; otherwise remembers it.
  bool repeats(const T& now) {
    if (valid && std::memcmp(&value, &now, sizeof(T)) == 0) {
      return true;
    }
    std::memcpy(&value, &now, sizeof(T));
    valid = true;
    return false;
  }
};

struct PBRProbeWrite {
  f32 rows[3][3];
  f32 weight;
  f32 occlusionMin;
  f32 occlusionInvMax;
};
struct PBRCubeWrite {
  u32 id;
  f32 params[4];
};
struct PBRAmbientWrite {
  f32 rows[6][3];
  f32 mode;
  u32 present;
};
struct PBRVolumeWrite {
  u32 id;
  f32 rows[6][4];
};
struct PBRLightmapWrite {
  u32 id;
  f32 rect[4];
  f32 axes[3][3];
};
struct PBRToneWrite {
  f32 rows[3][4];
};
struct PBRLightScaleWrite {
  f32 diffuse;
  f32 f0;
  f32 alpha;
  u32 alphaReplaces;
};
struct PBRLightHdrWrite {
  f32 color[3];
  f32 pos[3];
  f32 r0;
  f32 r1;
  u32 falloff;
};
struct PBRMaterialWrite {
  f32 emissive[3];
  f32 backlight[3];
  f32 heightBlend;
  f32 mode;
  f32 layer[5];
  f32 kind[6];
  f32 up[3];
  f32 bareGain;
  u32 debugView;
};
} // namespace

namespace {
// GXPortPostCompositeAdd's colour, for the next HDR scene post only (game thread).
f32 sPostAdd[4] = {};
// GXPortPostCompositeRadial's c0, c1, c2, likewise; sPostRadialOn false: none.
f32 sPostRadial[3][4] = {};
bool sPostRadialOn = false;

aurora::gfx::bloom::Params post_process_params(GXBool bloom, f32 threshold, const f32 tints[5][3],
                                               const f32 tone[3][4], u32 gradeA, u32 gradeB, f32 gradeWeight,
                                               f32 exposure) {
  aurora::gfx::bloom::Params params{};
  // Cost tests 12 and 14 price the bloom and the depth reload after the break; 15 always reloads
  // the depth, to price clearing it when the HUD doesn't need it.
  const u32 costTest = GXGetPBRCostTest();
  params.bloom = bloom && costTest != 12 ? 1 : 0;
  if (costTest == 14) {
    params.bloom |= aurora::gfx::bloom::CostNoDepthReload;
  } else if (costTest == 15) {
    params.bloom |= aurora::gfx::bloom::CostKeepDepth;
  }
  params.threshold = threshold;
  if (tints != nullptr) {
    std::memcpy(params.tints, tints, sizeof(params.tints));
  }
  if (tone != nullptr) {
    std::memcpy(params.tone, tone, sizeof(params.tone));
  }
  params.gradeA = gradeA;
  params.gradeB = gradeB;
  params.gradeWeight = gradeWeight;
  // Row 0's w is unused: the exposure it carried went to GXPortMeasureScene, so `exposure` is ignored.
  params.tone[0][3] = 0.f;
  return params;
}

bool write_post_process(const aurora::gfx::bloom::Params& params) {
  if (!aurora::gfx::bloom::ensure_task()) {
    return false;
  }
  // Through the FIFO: recording it directly would first wait for the FIFO thread to finish
  // everything the frame has drawn so far.
  static_assert(sizeof(params) == 49 * sizeof(u32));
  u32 words[49];
  std::memcpy(words, &params, sizeof(words));
  GX_WRITE_AURORA(GX_AURORA_PORT_POST_PROCESS);
  for (const u32 word : words) {
    GX_WRITE_U32(word);
  }
  return true;
}
} // namespace

extern "C" {
void GXPortPostCompositeAdd(const f32 rgba[4]) {
  if (rgba == nullptr) {
    std::memset(sPostAdd, 0, sizeof(sPostAdd));
  } else {
    std::memcpy(sPostAdd, rgba, sizeof(sPostAdd));
  }
}

void GXPortPostCompositeRadial(const f32 c0[4], const f32 c1[4], const f32 c2[4]) {
  if (c0 == nullptr || c1 == nullptr || c2 == nullptr) {
    std::memset(sPostRadial, 0, sizeof(sPostRadial));
    sPostRadialOn = false;
    return;
  }
  std::memcpy(sPostRadial[0], c0, sizeof(sPostRadial[0]));
  std::memcpy(sPostRadial[1], c1, sizeof(sPostRadial[1]));
  std::memcpy(sPostRadial[2], c2, sizeof(sPostRadial[2]));
  sPostRadialOn = true;
}

GXBool GXPortPostProcessScene(GXBool bloom, f32 threshold, const f32 tints[5][3], const f32 tone[3][4], u32 gradeA,
                              u32 gradeB, f32 gradeWeight, f32 exposure) {
  // The EFB the copies saw is about to change target (the HDR scene ends here).
  __GXPortInvalidateCopySerial();
  f32 add[4];
  std::memcpy(add, sPostAdd, sizeof(add));
  GXPortPostCompositeAdd(nullptr);
  f32 radial[3][4];
  std::memcpy(radial, sPostRadial, sizeof(radial));
  const bool radialOn = sPostRadialOn;
  GXPortPostCompositeRadial(nullptr, nullptr, nullptr);
  if (sDrawIdMode) {
    return true;
  }
  // Cost test 10 skips the post's work, not the scene's conversion: bloom and grades off.
  const bool noPost = GXGetPBRCostTest() == 10;
  auto params = noPost ? post_process_params(false, threshold, tints, tone, 0, 0, 0.f, 0.f)
                       : post_process_params(bloom, threshold, tints, tone, gradeA, gradeB, gradeWeight, exposure);
  std::memcpy(params.add, add, sizeof(add));
  std::memcpy(params.radial, radial, sizeof(radial));
  params.radialOn = radialOn ? 1 : 0;
  return write_post_process(params);
}

GXBool GXPortVolumetricFog(const GXPortFogParams* fog) {
  if (fog == nullptr || GXGetPBRCostTest() == 10 || sDrawIdMode || fog->fog[0] <= 0.f) {
    return true;
  }
  // No density anywhere: the fog or a region must add some.
  bool dense = fog->fog[3] > 0.f;
  for (u32 i = 0; i < fog->regionCount && i < 8; ++i) {
    dense = dense || fog->regions[i][6][0] > 0.f;
  }
  if (!dense) {
    return true;
  }
  if (!aurora::gfx::volfog::ensure_task()) {
    return false;
  }
  aurora::gfx::volfog::Params params{};
  static_assert(offsetof(aurora::gfx::volfog::Params, flags) == sizeof(GXPortFogParams));
  std::memcpy(&params, fog, sizeof(*fog));
  static_assert(sizeof(params) % sizeof(u32) == 0);
  u32 words[sizeof(params) / sizeof(u32)];
  std::memcpy(words, &params, sizeof(words));
  GX_WRITE_AURORA(GX_AURORA_PORT_VOLUMETRIC_FOG);
  for (const u32 word : words) {
    GX_WRITE_U32(word);
  }
  return true;
}

void GXPortVolumetricFogEnd() { GX_WRITE_AURORA(GX_AURORA_PORT_VOLUMETRIC_FOG_END); }

void GXPortSetParticleFog(GXBool on) {
  static_assert(GX_PORT_PARTICLE_UNFOGGED == aurora::gx::GXState::kParticleUnfogged);
  GX_WRITE_AURORA(GX_AURORA_PORT_PARTICLE_FOG);
  GX_WRITE_U8(on > GX_PORT_PARTICLE_UNFOGGED ? 1 : on);
}

GXBool GXPortXRayPass(const f32 p[8][4], const f32 tone[3][4], const f32 depthRange[3]) {
  if (!aurora::gfx::xray::ensure_task()) {
    return false;
  }
  aurora::gfx::xray::Params params{};
  std::memcpy(params.tone, tone, sizeof(params.tone));
  std::memcpy(params.p, p, sizeof(params.p));
  params.depth[0] = depthRange[0];
  params.depth[1] = depthRange[1];
  params.depth[2] = depthRange[2];
  u32 words[sizeof(params) / sizeof(u32)];
  std::memcpy(words, &params, sizeof(words));
  GX_WRITE_AURORA(GX_AURORA_PORT_XRAY);
  for (const u32 word : words) {
    GX_WRITE_U32(word);
  }
  return true;
}

GXBool GXPortSetThermalLut(const u8* rgba, u32 size) { return aurora::gfx::thermal::set_lut(rgba, size); }

GXBool GXPortThermalPass(const f32 tone[3][4], const f32 v[4], const f32 r[4]) {
  if (!aurora::gfx::thermal::ensure_task()) {
    return false;
  }
  aurora::gfx::thermal::Params params{};
  std::memcpy(params.tone, tone, sizeof(params.tone));
  std::memcpy(params.v, v, sizeof(params.v));
  std::memcpy(params.r, r, sizeof(params.r));
  u32 words[sizeof(params) / sizeof(u32)];
  std::memcpy(words, &params, sizeof(words));
  GX_WRITE_AURORA(GX_AURORA_PORT_THERMAL);
  for (const u32 word : words) {
    GX_WRITE_U32(word);
  }
  return true;
}

GXBool GXPortSceneHdr(GXBool on) {
  // A switch changes what an EFB copy holds (the HDR scene's light or the display EFB) without one being
  // made, so the game thread's copy caches must not take an earlier copy for this one. Every request
  // counts, refused or not, and whatever the FIFO later makes of it: a spare recopy is only a copy.
  // (The switch back to the display EFB at the post, GXPortPostProcessScene, invalidates it the same way.)
  __GXPortInvalidateCopySerial();
  if (on && (sDrawIdMode || !aurora::webgpu::scene_hdr_supported())) {
    return false;
  }
  GX_WRITE_AURORA(GX_AURORA_PORT_SCENE_HDR);
  GX_WRITE_U8(on ? 1 : 0);
  return true;
}

// The mode last written, for the game thread (the FIFO state is read on the render thread).
static u8 sShadowCasterMode = 0;

GXBool GXPortPauseSceneHdr(void) {
  __GXPortInvalidateCopySerial();
  if (sDrawIdMode || !aurora::webgpu::scene_hdr_supported()) return false;
  GX_WRITE_AURORA(GX_AURORA_PORT_SCENE_HDR);
  GX_WRITE_U8(2);
  return true;
}

void GXPortSetShadowCaster(GXBool on) {
  sShadowCasterMode = on ? 1 : 0;
  GX_WRITE_AURORA(GX_AURORA_PORT_SHADOW_CASTER);
  GX_WRITE_U8(on ? 1 : 0);
}

void GXPortSetShadowCasterOnly(GXBool on) {
  sShadowCasterMode = on ? 2 : 1;
  GX_WRITE_AURORA(GX_AURORA_PORT_SHADOW_CASTER);
  GX_WRITE_U8(on ? 2 : 1);
}

u8 GXPortGetShadowCasterMode() { return sShadowCasterMode; }

// The last shadow frame, for GXPortShadowBoxCasts and GXPortGetShadowInfo on the game thread.
static struct {
  f32 worldToView[3][4];
  aurora::gfx::shadow::cascade::Frustum frustum{};
  f32 sunDir[3];
  f32 color[3];
  f32 distance = 0.f;
} sShadowFrame;

GXBool GXPortShadowBoxCasts(const f32 min[3], const f32 max[3]) {
  return aurora::gfx::shadow::box_casts(sShadowFrame.worldToView, sShadowFrame.frustum, sShadowFrame.sunDir,
                                        sShadowFrame.distance, min, max);
}

void GXPortGetShadowInfo(f32 sunDir[3], f32 color[3], f32* distance, f32 center[3], u32* casters) {
  std::memcpy(sunDir, sShadowFrame.sunDir, sizeof(sShadowFrame.sunDir));
  std::memcpy(color, sShadowFrame.color, sizeof(sShadowFrame.color));
  aurora::gfx::shadow::cascade::Fit fits[aurora::gfx::shadow::cascade::Count];
  const bool active = aurora::gfx::shadow::fit_frame(sShadowFrame.worldToView, sShadowFrame.frustum,
                                                     sShadowFrame.sunDir, sShadowFrame.distance, fits);
  *distance = active ? sShadowFrame.distance : 0.f;
  for (int i = 0; i < 3; ++i) {
    center[i] = active ? fits[aurora::gfx::shadow::cascade::Count - 1].center[i] : 0.f;
  }
  *casters = aurora::gfx::shadow::last_caster_count();
}

static f32 sRoomLights[64 * 16] = {};
static u32 sRoomLightCount = 0;

u32 GXPortGetRoomLights(f32 records[64 * 16]) {
  std::memcpy(records, sRoomLights, sRoomLightCount * 16 * sizeof(f32));
  return sRoomLightCount;
}

void GXPortSetRoomLights(const f32* records, u32 count) {
  count = records == nullptr ? 0 : std::min(count, u32(64));
  sRoomLightCount = count;
  if (count) std::memcpy(sRoomLights, records, count * 16 * sizeof(f32));
  GX_WRITE_AURORA(GX_AURORA_PORT_ROOM_LIGHTS);
  GX_WRITE_U32(count);
  for (u32 i = 0; i < count * 16; ++i) {
    GX_WRITE_F32(records[i]);
  }
}

void GXPortSetShadowFrame(const f32 worldToView[3][4], const f32 frustum[6], const f32 sunDir[3], f32 distance,
                          const f32 color[3]) {
  std::memcpy(sShadowFrame.worldToView, worldToView, sizeof(sShadowFrame.worldToView));
  sShadowFrame.frustum = {frustum[0], frustum[1], frustum[2], frustum[3], frustum[4], frustum[5]};
  std::memcpy(sShadowFrame.sunDir, sunDir, sizeof(sShadowFrame.sunDir));
  std::memcpy(sShadowFrame.color, color, sizeof(sShadowFrame.color));
  sShadowFrame.distance = distance;
  GX_WRITE_AURORA(GX_AURORA_PORT_SHADOW_FRAME);
  for (u32 i = 0; i < 3; ++i) {
    for (u32 j = 0; j < 4; ++j) {
      GX_WRITE_F32(worldToView[i][j]);
    }
  }
  for (u32 i = 0; i < 6; ++i) {
    GX_WRITE_F32(frustum[i]);
  }
  for (u32 i = 0; i < 3; ++i) {
    GX_WRITE_F32(sunDir[i]);
  }
  GX_WRITE_F32(distance);
  for (u32 i = 0; i < 3; ++i) {
    GX_WRITE_F32(color[i]);
  }
}

GXBool GXPortRenderShadowMap() {
  if (sDrawIdMode || !aurora::gfx::shadow::ensure_task()) {
    return false;
  }
  GX_WRITE_AURORA(GX_AURORA_PORT_SHADOW_RENDER);
  return true;
}

void GXPortActorShadowCasters(u32 layer, const f32 clip[16]) {
  GX_WRITE_AURORA(GX_AURORA_PORT_ACTOR_SHADOW);
  GX_WRITE_U8(1);
  GX_WRITE_U8(static_cast<u8>(layer));
  for (u32 i = 0; i < 64; ++i) {
    GX_WRITE_F32(i < 16 ? clip[i] : 0.f);
  }
}

void GXPortActorShadowDecal(u32 layer, const f32 params[64]) {
  GX_WRITE_AURORA(GX_AURORA_PORT_ACTOR_SHADOW);
  GX_WRITE_U8(2);
  GX_WRITE_U8(static_cast<u8>(layer));
  for (u32 i = 0; i < 64; ++i) {
    GX_WRITE_F32(params[i]);
  }
}

void GXPortActorShadowEnd() {
  GX_WRITE_AURORA(GX_AURORA_PORT_ACTOR_SHADOW);
  GX_WRITE_U8(0);
}

GXBool GXPortActorShadowRender() {
  if (sDrawIdMode || !aurora::gfx::actor_shadow::ensure_task()) {
    return false;
  }
  GX_WRITE_AURORA(GX_AURORA_PORT_ACTOR_SHADOW);
  GX_WRITE_U8(3);
  return true;
}

void GXPortShell(u32 mode, const f32* params) {
  GX_WRITE_AURORA(GX_AURORA_PORT_SHELL);
  GX_WRITE_U8(static_cast<u8>(params != nullptr ? mode : 0));
  if (mode != 0 && params != nullptr) {
    for (u32 i = 0; i < 24; ++i) {
      GX_WRITE_F32(params[i]);
    }
  }
}

void GXPortColorGradeLut(u32 id, const u8* rgba) { aurora::gfx::bloom::set_grade_lut(id, rgba); }

GXBool GXPortMeasureScene(f32 exposure) {
  if (sDrawIdMode) {
    return false;
  }
  return aurora::gfx::bloom::measure_scene(exposure);
}

GXBool GXPortFrameRadiance(f32 out[3], u32* serial) {
  uint32_t count = 0;
  const bool ok = aurora::gfx::bloom::frame_radiance(out, count);
  if (serial != nullptr) {
    *serial = count;
  }
  return ok;
}

void GXDestroyTexObj(GXTexObj* obj_) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  if (obj->texObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TEXOBJ);
    GX_WRITE_U32(obj->texObjId);
  }
  obj->texObjId = 0;
}

void GXDestroyTlutObj(GXTlutObj* obj_) {
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  if (obj->tlutObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TLUT);
    GX_WRITE_U32(obj->tlutObjId);
  }
  obj->tlutObjId = 0;
}

void GXDestroyCopyTex(void* dest) {
  if (dest != nullptr) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_COPY_TEX);
    GX_WRITE_U64(reinterpret_cast<u64>(dest));
  }
}

void GXSetArrayBaseIndex(GXAttr attr, u32 base) {
  if (attr == GX_VA_NBT) {
    attr = GX_VA_NRM;
  }
  const u32 cpIdx = attr - GX_VA_POS;
  assert((cpIdx & ~0xF) == 0);
  GX_WRITE_AURORA(GX_AURORA_LOAD_ARRAY_BASE_INDEX);
  GX_WRITE_U8(static_cast<u8>(cpIdx));
  GX_WRITE_U32(base);
}

static u32 sPBRCostTest = 0;

void GXSetPBRCostTest(u32 test) { sPBRCostTest = test <= 15 ? test : 0; }

u32 GXGetPBRCostTest() { return sPBRCostTest; }

void GXSetPBR(GXBool enable) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR);
  // Tests 10 and 11 are outside the shading.
  GX_WRITE_U8(enable ? static_cast<u8>(1 + (sPBRCostTest <= 9 ? sPBRCostTest : 0)) : 0);
}

void GXSetSDF(u8 edge) {
  GX_WRITE_AURORA(GX_AURORA_SET_SDF);
  GX_WRITE_U8(edge);
}

void GXSetHudSample(u8 mode, f32 dyinX, f32 dyinY) {
  GX_WRITE_AURORA(GX_AURORA_SET_HUD_SAMPLE);
  GX_WRITE_U8(mode);
  GX_WRITE_F32(dyinX);
  GX_WRITE_F32(dyinY);
}

void GXSetClipPlane(GXBool enable, const f32 plane[4]) {
  GX_WRITE_AURORA(GX_AURORA_SET_CLIP_PLANE);
  GX_WRITE_U8(enable ? 1 : 0);
  for (int i = 0; i < 4; ++i) {
    GX_WRITE_F32(enable ? plane[i] : 0.f);
  }
}

void GXPortSetDepthPrepass(u8 pass) {
  GX_WRITE_AURORA(GX_AURORA_PORT_DEPTH_PREPASS);
  GX_WRITE_U8(pass);
}

static std::atomic< u32 > sPerformance{GX_PORT_PERF_ALL};

void GXPortSetPerformance(u32 flags) { sPerformance.store(flags & GX_PORT_PERF_ALL, std::memory_order_relaxed); }

u32 GXPortGetPerformance(void) { return sPerformance.load(std::memory_order_relaxed); }

void GXSetDrawTag(u32 asset, u32 model, u32 material) {
  GX_WRITE_AURORA(GX_AURORA_SET_DRAW_TAG);
  GX_WRITE_U32(asset);
  GX_WRITE_U32(model);
  GX_WRITE_U32(material);
}

void GXPortSetDrawSerial(u32 serial) {
  GX_WRITE_AURORA(GX_AURORA_PORT_DRAW_SERIAL);
  GX_WRITE_U32(serial);
}

void GXPortSetDrawIdMode(GXBool on) {
  sDrawIdMode = on != 0;
  GX_WRITE_AURORA(GX_AURORA_PORT_DRAW_ID_MODE);
  GX_WRITE_U8(on ? 1 : 0);
}

u32 GXPortShaderDump(const char* dir) { return aurora::gx::dump_shaders(dir); }

void GXPortShaderOverrideDir(const char* dir) { aurora::gx::set_shader_override_dir(dir); }

void GXPortShaderReload(void) { aurora::gfx::drop_pipelines(); }

void GXPortDrawLog(GXBool on) { aurora::gx::set_draw_shader_log(on != GX_FALSE); }

u64 GXPortDrawShader(u32 serial) { return aurora::gx::draw_shader_hash(serial); }

GXBool GXPortShaderOverridden(u64 hash) { return aurora::gx::shader_overridden(hash) ? GX_TRUE : GX_FALSE; }

void GXCopyProbeFace(u32 face) {
  GX_WRITE_AURORA(GX_AURORA_COPY_PROBE_FACE);
  GX_WRITE_U8(static_cast<u8>(face));
  aurora::gx::fifo::publish();
}

void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight) { GXSetPBRProbeEx(viewToProbe, weight, 0.f, 0.f); }

void GXSetPBRProbeEx(const f32 viewToProbe[3][3], f32 weight, f32 occlusionMin, f32 occlusionInvMax) {
  static LastPBRWrite<PBRProbeWrite> sLast;
  PBRProbeWrite now{};
  std::memcpy(now.rows, viewToProbe, sizeof(now.rows));
  now.weight = weight;
  now.occlusionMin = occlusionMin;
  now.occlusionInvMax = occlusionInvMax;
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_PROBE);
  const f32 w[3]{weight, occlusionMin, occlusionInvMax};
  for (int col = 0; col < 3; ++col) {
    GX_WRITE_F32(viewToProbe[0][col]);
    GX_WRITE_F32(viewToProbe[1][col]);
    GX_WRITE_F32(viewToProbe[2][col]);
    GX_WRITE_F32(w[col]);
  }
}

GXBool GXPortBlendPBRCube(u32 dst, const u32* src, const f32* weights, u32 count) {
  return aurora::gfx::probe::blend_cubes(dst, src, weights, count);
}

static u32 sPBRDebugView = 0;

void GXSetPBRDebugView(u32 view) { sPBRDebugView = view; }

void GXSetPBRMaterial(const f32 emissive[3], const f32 backlight[3], f32 heightBlend, f32 mode, const f32 layer[5],
                      const f32 kind[6], const f32 up[3], f32 bareGain) {
  static LastPBRWrite<PBRMaterialWrite> sLast;
  PBRMaterialWrite now{};
  std::memcpy(now.emissive, emissive, sizeof(now.emissive));
  std::memcpy(now.backlight, backlight, sizeof(now.backlight));
  now.heightBlend = heightBlend;
  now.mode = mode;
  std::memcpy(now.layer, layer, sizeof(now.layer));
  std::memcpy(now.kind, kind, sizeof(now.kind));
  std::memcpy(now.up, up, sizeof(now.up));
  now.bareGain = bareGain;
  now.debugView = sPBRDebugView;
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_MATERIAL);
  GX_WRITE_F32(emissive[0]);
  GX_WRITE_F32(emissive[1]);
  GX_WRITE_F32(emissive[2]);
  GX_WRITE_F32(heightBlend);
  GX_WRITE_F32(backlight[0]);
  GX_WRITE_F32(backlight[1]);
  GX_WRITE_F32(backlight[2]);
  GX_WRITE_F32(mode);
  GX_WRITE_F32(layer[0]);
  GX_WRITE_F32(kind[0]);
  GX_WRITE_F32(kind[1]);
  GX_WRITE_F32(static_cast<f32>(sPBRDebugView));
  for (int i = 1; i < 5; ++i) {
    GX_WRITE_F32(layer[i]);
  }
  for (int i = 2; i < 6; ++i) {
    GX_WRITE_F32(kind[i]);
  }
  GX_WRITE_F32(up[0]);
  GX_WRITE_F32(up[1]);
  GX_WRITE_F32(up[2]);
  GX_WRITE_F32(bareGain);
}

void GXCreatePBRCube(u32 id, u32 size, u32 mipCount, const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_CUBE);
  GX_WRITE_U32(id);
  GX_WRITE_U32(size);
  GX_WRITE_U32(mipCount);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXDestroyPBRCube(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_CUBE);
  GX_WRITE_U32(id);
}

void GXSetPBRCube(u32 id, const f32 params[4]) {
  static LastPBRWrite<PBRCubeWrite> sLast;
  PBRCubeWrite now{};
  now.id = id;
  std::memcpy(now.params, params, sizeof(now.params));
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_CUBE);
  GX_WRITE_U32(id);
  for (int i = 0; i < 4; ++i) {
    GX_WRITE_F32(params[i]);
  }
}

void GXSetPBRAmbient(const f32 rows[6][3], f32 mode) {
  static LastPBRWrite<PBRAmbientWrite> sLast;
  PBRAmbientWrite now{};
  if (rows != nullptr) {
    std::memcpy(now.rows, rows, sizeof(now.rows));
    now.mode = mode;
    now.present = 1;
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_AMBIENT);
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 3; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
    GX_WRITE_F32(rows != nullptr && row == 0 ? mode : 0.f);
  }
}

void GXCreatePBRVolume(u32 id, u32 sizeX, u32 sizeY, u32 sizeZ, const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_VOLUME);
  GX_WRITE_U32(id);
  GX_WRITE_U32(sizeX);
  GX_WRITE_U32(sizeY);
  GX_WRITE_U32(sizeZ);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

extern "C++" void GXCreatePBRVolumeOwned(u32 id, u32 sizeX, u32 sizeY, u32 sizeZ, void* owned) {
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_VOLUME);
  GX_WRITE_U32(id);
  GX_WRITE_U32(sizeX);
  GX_WRITE_U32(sizeY);
  GX_WRITE_U32(sizeZ);
  GX_WRITE_U64(reinterpret_cast<u64>(owned));
}

void GXDestroyPBRVolume(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_VOLUME);
  GX_WRITE_U32(id);
}

void GXSetPBRVolume(u32 id, const f32 rows[6][4]) {
  static LastPBRWrite<PBRVolumeWrite> sLast;
  PBRVolumeWrite now{};
  if (rows != nullptr) {
    now.id = id;
    std::memcpy(now.rows, rows, sizeof(now.rows));
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_VOLUME);
  GX_WRITE_U32(rows != nullptr ? id : 0);
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 4; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
  }
}

GXBool GXPBRLightmapBC6HSupported(void) { return aurora::gfx::probe::lightmap_bc_supported(); }

u32 GXCreatePBRLightmap(u32 width, u32 height, u32 layers, GXPBRLightmapFormat format, const void* data, u32 size) {
  if (data == nullptr || !aurora::gfx::probe::lightmap_available()) {
    return 0;
  }
  const bool bc = format == GX_PBR_LIGHTMAP_BC6H_SFLOAT || format == GX_PBR_LIGHTMAP_BC6H_UFLOAT;
  if (!bc && format != GX_PBR_LIGHTMAP_RGBA16F) {
    return 0;
  }
  if (bc && (!aurora::gfx::probe::lightmap_bc_supported() || width % 4 != 0 || height % 4 != 0)) {
    return 0;
  }
  const u64 bytes = bc ? u64(width / 4) * (height / 4) * 16 * layers : u64(width) * height * 8 * layers;
  if (width == 0 || height == 0 || layers == 0 || bytes > size) {
    return 0;
  }
  static std::atomic<u32> sNextLightmap{1};
  const u32 id = sNextLightmap.fetch_add(1);
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(data), static_cast<const u8*>(data) + bytes);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_LIGHTMAP);
  GX_WRITE_U32(id);
  GX_WRITE_U32(width);
  GX_WRITE_U32(height);
  GX_WRITE_U32(layers);
  GX_WRITE_U32(format);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
  return id;
}

void GXDestroyPBRLightmap(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_LIGHTMAP);
  GX_WRITE_U32(id);
}

void GXSetPBRLightmap(u32 id, const f32 rect[4], const f32 axes[3][3]) {
  static LastPBRWrite<PBRLightmapWrite> sLast;
  PBRLightmapWrite now{};
  if (id != 0 && rect != nullptr && axes != nullptr) {
    now.id = id;
    std::memcpy(now.rect, rect, sizeof(now.rect));
    std::memcpy(now.axes, axes, sizeof(now.axes));
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHTMAP);
  GX_WRITE_U32(now.id);
  for (int i = 0; i < 4; ++i) {
    GX_WRITE_F32(now.rect[i]);
  }
  for (int row = 0; row < 3; ++row) {
    for (int i = 0; i < 3; ++i) {
      GX_WRITE_F32(now.axes[row][i]);
    }
  }
}

void GXSetPBRLightmapAttr(GXAttr attr) {
  static LastPBRWrite<u8> sLast;
  const u8 now = attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7 ? static_cast<u8>(attr) : static_cast<u8>(GX_VA_NULL);
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHTMAP_ATTR);
  GX_WRITE_U8(now);
}

void GXSetPBRBrdfLut(const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>;
  if (texels != nullptr && length == 256) {
    copy->assign(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_BRDF_LUT);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXSetPBRTone(const f32 rows[3][4]) {
  static LastPBRWrite<PBRToneWrite> sLast;
  PBRToneWrite now{};
  if (rows != nullptr) {
    std::memcpy(now.rows, rows, sizeof(now.rows));
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_TONE);
  for (int row = 0; row < 3; ++row) {
    for (int i = 0; i < 4; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
  }
}

void GXSetPBRLightSkip(u32 mask) {
  static LastPBRWrite<u32> sLast;
  if (sLast.repeats(mask)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHT_SKIP);
  GX_WRITE_U32(mask);
}

void GXSetPBRBakedLightModulation(const f32 rgb[3]) {
  struct Write {
    f32 rgb[3];
  };
  static LastPBRWrite<Write> sLast;
  const Write now = rgb != nullptr ? Write{{rgb[0], rgb[1], rgb[2]}} : Write{{1.f, 1.f, 1.f}};
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_BAKED_LIGHT_MODULATION);
  for (f32 v : now.rgb) {
    GX_WRITE_F32(v);
  }
}

void GXSetPBRModelModulation(const f32 diffuse[4], const f32 incan[4], f32 coverage) {
  struct Write {
    f32 diffuse[4];
    f32 incan[4];
    f32 coverage;
  };
  static LastPBRWrite<Write> sLast;
  Write now{{1.f, 1.f, 1.f, 1.f}, {0.f, 0.f, 0.f, 1.f}, coverage};
  for (u32 i = 0; i < 4; ++i) {
    if (diffuse != nullptr) {
      now.diffuse[i] = diffuse[i];
    }
    if (incan != nullptr) {
      now.incan[i] = incan[i];
    }
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_MODEL_MODULATION);
  for (f32 v : now.diffuse) {
    GX_WRITE_F32(v);
  }
  for (f32 v : now.incan) {
    GX_WRITE_F32(v);
  }
  GX_WRITE_F32(now.coverage);
}

void GXSetPBRThermal(GXBool on, f32 x, f32 y, GXBool blended) {
  struct Write {
    f32 x;
    f32 y;
    f32 blended;
    u32 on;
  };
  static LastPBRWrite<Write> sLast;
  const Write now = on ? Write{x, y, blended ? 1.f : 0.f, 1u} : Write{0.f, 0.f, 0.f, 0u};
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_THERMAL);
  GX_WRITE_F32(now.x);
  GX_WRITE_F32(now.y);
  GX_WRITE_F32(now.blended);
  GX_WRITE_U32(now.on);
}

void GXSetPBRBacklight(const f32 plane[4], const f32 backDir[3], f32 back, f32 top, f32 bindScale, f32 bindOffset) {
  struct Write {
    f32 values[11];
  };
  static LastPBRWrite<Write> sLast;
  Write now{};
  if (plane != nullptr && backDir != nullptr) {
    now = Write{{plane[0], plane[1], plane[2], plane[3], backDir[0], backDir[1], backDir[2], back, top, bindOffset,
                 bindScale}};
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_BACKLIGHT);
  for (f32 v : now.values) {
    GX_WRITE_F32(v);
  }
}

void GXSetPBRShield(const f32 rows[8][4]) {
  struct Write {
    f32 values[32];
  };
  static LastPBRWrite<Write> sLast;
  Write now{};
  if (rows != nullptr) {
    std::memcpy(now.values, rows, sizeof(now.values));
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_SHIELD);
  for (f32 v : now.values) {
    GX_WRITE_F32(v);
  }
}

void GXSetPBRLightHdr(GXLightID light, const f32 color[3], const f32 viewPos[3], f32 r0, f32 r1, u32 falloff) {
  const u32 bit = static_cast<u32>(light) & 0xFF;
  if (bit == 0) {
    return;
  }
  const u32 idx = static_cast<u32>(std::countr_zero(bit));
  PBRLightHdrWrite now{};
  if (color != nullptr && viewPos != nullptr && r1 > 0.f) {
    now = {{color[0], color[1], color[2]}, {viewPos[0], viewPos[1], viewPos[2]}, r0, r1, falloff};
  }
  static LastPBRWrite<PBRLightHdrWrite> sLast[8];
  if (sLast[idx].repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHT_HDR);
  GX_WRITE_U32(bit);
  for (f32 v : now.color) {
    GX_WRITE_F32(v);
  }
  for (f32 v : now.pos) {
    GX_WRITE_F32(v);
  }
  GX_WRITE_F32(now.r0);
  GX_WRITE_F32(now.r1);
  GX_WRITE_U32(now.falloff);
}

void GXSetPBRLightScale(f32 diffuse, f32 f0, f32 alpha, GXBool alphaReplaces) {
  static LastPBRWrite<PBRLightScaleWrite> sLast;
  const PBRLightScaleWrite now{diffuse, f0, alpha, alphaReplaces ? 1u : 0u};
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHT_SCALE);
  GX_WRITE_F32(diffuse);
  GX_WRITE_F32(f0);
  GX_WRITE_F32(alpha);
  GX_WRITE_U32(now.alphaReplaces);
}
}

void GXPortSetGpuTimes(GXBool on) { aurora::webgpu::gpu_prof::set_enabled(on != GX_FALSE); }

GXBool GXPortGpuTimesSupported(void) { return aurora::webgpu::gpu_prof::supported() ? GX_TRUE : GX_FALSE; }

u32 GXPortGetGpuTimes(GXPortGpuTime* out, u32 max, float* totalMs, float* spanMs) {
  const auto result = aurora::webgpu::gpu_prof::results();
  if (totalMs != nullptr) {
    *totalMs = result.totalMs;
  }
  if (spanMs != nullptr) {
    *spanMs = result.spanMs;
  }
  u32 count = 0;
  for (const auto& entry : result.entries) {
    if (out == nullptr || count >= max) {
      break;
    }
    out[count++] = {entry.name, entry.msPerFrame, entry.passesPerFrame};
  }
  return out == nullptr ? u32(result.entries.size()) : count;
}
