#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

#include "Kyoto/Graphics/CCubeSurface.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGX_Impl.hpp" // IWYU pragma: keep
#include "Kyoto/Graphics/CGraphics.hpp"
#include "dolphin/gx/GXVert.h"
#ifdef TARGET_PC
#include <dolphin/gx/GXExtra.h>
#include "port_pbr_record.h"
#include "port_room_env.h"
#include <algorithm>
#include <cmath>
#include <vector>
#endif

static bool sDrawingOccluders = false;
static bool sDrawingWireframe = false;
bool CCubeModel::sUsingPackedLightmaps = false;

inline uint GetMaterialOffset(const uchar* materialData, const int idx) {
  materialData += (idx * 4);
  return CBasics::SwapBytes(*reinterpret_cast< const uint* >(materialData - 4));
}

CCubeModel::CCubeModel(rstl::vector< void* >* surfaces,
                       rstl::vector< TCachedToken< CTexture > >* textures, const void* materialData,
                       const void* positions, const void* normals, const void* colors,
                       const void* uvs, const void* compressedUvs, const CAABox& bounds,
                       const uchar visorFlags, const bool texturesLoaded, const uint idx,
                       const uint positionsSize, const uint normalsSize, const uint colorsSize,
                       const uint texCoordsSize, const uint packedTexCoordsSize)
: x0_instance(*surfaces, materialData, positions, normals, colors, uvs, compressedUvs,
              positionsSize, normalsSize, colorsSize, texCoordsSize, packedTexCoordsSize)
, x1c_textures(textures)
, x20_bounds(bounds)
, x38_firstUnsorted(nullptr)
, x3c_firstSorted(nullptr)
, x40_24_loadTextures(static_cast< uchar >(!texturesLoaded))
, x40_25_visible(false)
, x41_visorFlags(visorFlags)
, x44_idx(idx) {
  rstl::vector< void* >& surf = x0_instance.Surfaces();
  for (AUTO(it, surf.begin()); it != surf.end(); ++it) {
    CCubeSurface::SSurfaceData* data = static_cast< CCubeSurface::SSurfaceData* >(*it);
    data->mParent = this;
  }

  for (int i = surf.size(); i > 0; i--) {
    void*& data = surf[i - 1];
    uint materialIndex = static_cast< CCubeSurface::SSurfaceData* >(data)->mMaterialIndex;
    if (GetMaterialByIndex(materialIndex).IsFlagSet(kStateFlag_DepthSorting)) {
      static_cast< CCubeSurface::SSurfaceData* >(data)->mNextSurface = x3c_firstSorted.x0_rawdata;
      x3c_firstSorted.x0_rawdata = static_cast< uchar* >(data);
    } else {
      static_cast< CCubeSurface::SSurfaceData* >(data)->mNextSurface = x38_firstUnsorted.x0_rawdata;
      x38_firstUnsorted.x0_rawdata = static_cast< uchar* >(data);
    }
  }
}

void CCubeModel::MakeTexturesFromMats(const void* data,
                                      rstl::vector< TCachedToken< CTexture > >& textures,
                                      IObjectStore& store, const bool cache) {
  const uint* textureIds = static_cast< const uint* >(data);
  const uint textureCount = CBasics::SwapBytes(*static_cast< const int* >(data));
  textureIds++;
  textures.reserve(textureCount);

  for (int i = 0; i < textureCount; i++) {
    textures.push_back(store.GetObj(SObjectTag('TXTR', CBasics::SwapBytes(*textureIds))));
    if (!cache) {
      textures.back().ForceCache();
    }
    ++textureIds;
  }
}

void CCubeModel::SetStaticArraysCurrent() const {
  CGX::SetArray(GX_VA_CLR0, x0_instance.GetColorPointer(), x0_instance.GetColorSize(),
                sizeof(CColor));
  const void* packed = x0_instance.GetPackedTCPointer();
  const void* unpacked = x0_instance.GetTCPointer();
  if (!packed) {
    sUsingPackedLightmaps = false;
  }

  if (sUsingPackedLightmaps) {
    CGX::SetArray(GX_VA_TEX0, packed, x0_instance.GetPackedTCSize(), sizeof(ushort) * 2);
  } else {
    CGX::SetArray(GX_VA_TEX0, unpacked, x0_instance.GetTCSize(), sizeof(CVector2f));
  }

  if (unpacked) {
    for (int i = 1; i <= GX_VA_TEX7 - GX_VA_TEX0; ++i) {
      CGX::SetArray(static_cast< GXAttr >(i + GX_VA_TEX0), unpacked, x0_instance.GetTCSize(),
                    sizeof(CVector2f));
    }
  }

  CCubeMaterial::KillCachedViewDepState();
}

void CCubeModel::SetArraysCurrent() const {
  CGX::SetArray(GX_VA_POS, x0_instance.GetVertexPointer(), x0_instance.GetVertexSize(),
                sizeof(CVector3f));
  // Not a skinned draw: no bind pose (see SetSkinningArraysCurrent).
  CGX::ClearArray(GX_VA_TEX7);
  const int stride = HasNbtNormals()          ? sizeof(float) * 3 * NormalVecs()
                     : (x41_visorFlags & 1) ? sizeof(short) * 3
                                            : sizeof(CVector3f);
  CGX::SetArray(GX_VA_NRM, x0_instance.GetNormalPointer(), x0_instance.GetNormalSize(), stride);
  SetStaticArraysCurrent();
}

void CCubeModel::SetSkinningArraysCurrent(const float* positions, const float* normals) const {
  // Port: CSkinnedModel's callback draws hand a single-bone (or unskinned) model
  // its own file arrays, which are big-endian (and may hold short normals).
  // Uploading them as native floats exploded the vertices (thermal pickups).
  if (positions == x0_instance.GetVertexPointer()) {
    SetArraysCurrent();
    return;
  }
  // The skinned workspaces reuse the same pointer each frame, so force the
  // backend to drop its cached copy or the new vertex data is never uploaded.
  CGX::ClearArray(GX_VA_POS);
  CGX::ClearArray(GX_VA_NRM);
  // The bind pose (the file's positions) goes in as GX_VA_TEX7's array for the PBR backlight,
  // which fades by the bind-pose height, as Remastered's CharacterBacklight does.
  CGX::SetArray(GX_VA_TEX7, x0_instance.GetVertexPointer(), x0_instance.GetVertexSize(), sizeof(CVector3f));
  CGraphics::sRenderState.SetVtxState(positions, normals,
                                      static_cast< const uint* >(x0_instance.GetColorPointer()));
  SetStaticArraysCurrent();
}

void CCubeModel::SetUsingPackedLightmaps(const bool use) const {
  sUsingPackedLightmaps = use;
  if (sUsingPackedLightmaps) {
    CGX::SetArray(GX_VA_TEX0, x0_instance.GetPackedTCPointer(), x0_instance.GetPackedTCSize(),
                  sizeof(ushort) * 2);
  } else {
    CGX::SetArray(GX_VA_TEX0, x0_instance.GetTCPointer(), x0_instance.GetTCSize(),
                  sizeof(CVector2f));
  }
}

CCubeMaterial CCubeModel::GetMaterialByIndex(const int idx) const {
  uint materialCount = 0;
  uint materialOffset = 0;
  const uchar* materialData = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                              (x1c_textures->size() + 1) * 4;
  materialCount = *reinterpret_cast< const uint* >(materialData++);
  materialCount = CBasics::SwapBytes(materialCount);
  if (idx < 0 || static_cast<uint>(idx) >= materialCount) {
    throw std::runtime_error("Surface material index " + std::to_string(idx) +
                             " exceeds material count " + std::to_string(materialCount));
  }
  materialData++;
  materialData++;
  materialData++;
  if (idx != 0) {
    materialOffset = GetMaterialOffset(materialData, idx);
  }

  materialData += (materialCount * 4);
  materialData += materialOffset;
  return CCubeMaterial(materialData);
}

#ifdef TARGET_PC
// Port: the material record a converter may append to a kStateFlag_PortPBR material, read
// back from the material's end so that nothing retail parses moves: six big-endian floats
// (emissive multiplier rgb, backlight weight rgb) and the tag 'PBRM', or eight (the same,
// then the height blend threshold and the shading mode) and 'PBR2', or thirteen (the same,
// then a second layer's edge width and the scale and offset of each layer's height) and
// 'PBR3', or nineteen (the same, then the kind of a special surface, its strength and four
// parameters; see GXSetPBRMaterial) and 'PBR4', or those nineteen, then one big-endian word
// of the maps' wrap modes (see the declaration) and 'PBR5', or those and two more floats (the
// diffuse and F0 factors of a back-facing copy, see GXSetPBRLightScale) and 'PBR6', or those
// and one more big-endian word (the file id of the material's own reflection cube) and
// 'PBR7'. A material without one gets the neutral values. A converted TEV material may end
// in the wrap word alone and 'WRAP' (no floats). A kind 14 record carries a trailer after
// any of these: 32 floats (the boundary shield's CCH0..CCH6 and DIFC) and 'PBR8'. A room
// geometry material may end in one more trailer after everything: a big-endian word (the
// vertex texcoord slot of the model's lightmap UV) and 'LMUV'; PortPbrRecord::Read skips it.
int CCubeModel::PortReadPBRMaterial(const int idx, f32 values[19], uint* wrap,
                                    f32 lightScale[2], uint* cube, f32* shield) const {
  const uchar* table = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                       (x1c_textures->size() + 1) * 4;
  const uint count = CBasics::SwapBytes(*reinterpret_cast< const uint* >(table));
  table += 4;
  const uint begin = idx != 0 ? GetMaterialOffset(table, idx) : 0;
  const uint end = GetMaterialOffset(table, idx + 1);
  uint32_t wrapWord;
  uint32_t cubeId;
  const int floats = PortPbrRecord::Read(table + count * 4 + end, end - begin, values, &wrapWord,
                                         lightScale, &cubeId, shield);
  if (wrap != nullptr) {
    *wrap = wrapWord;
  }
  if (cube != nullptr) {
    *cube = cubeId;
  }
  return floats;
}

int CCubeModel::PortLightmapSlot(const int idx) const {
  const uchar* table = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                       (x1c_textures->size() + 1) * 4;
  const uint count = CBasics::SwapBytes(*reinterpret_cast< const uint* >(table));
  table += 4;
  const uint begin = idx != 0 ? GetMaterialOffset(table, idx) : 0;
  const uint end = GetMaterialOffset(table, idx + 1);
  return PortPbrRecord::LightmapSlot(table + count * 4 + end, end - begin);
}

uint CCubeModel::PortMaterialCount() const {
  const uchar* table = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                       (x1c_textures->size() + 1) * 4;
  return CBasics::SwapBytes(*reinterpret_cast< const uint* >(table));
}

// Values a debugging session puts in place of a record's (the console's `roomgeo mat`).
namespace {
struct SPortPBROverride {
  const CCubeModel* model;
  int material;
  int field;
  f32 value;
};
std::vector< SPortPBROverride > sPortPBROverrides;
bool sPortGlows = false;
f32 sPortGlow[3];
bool sPortSky = false;
f32 sPortSkyGain[3];
f32 sPortChargeShell = 0.f;
f32 sPortDisintegration = 0.f;
} // namespace

void CCubeModel::PortSetChargeShell(const f32 amount) { sPortChargeShell = amount; }

void CCubeModel::PortSetDisintegration(const f32 amount) { sPortDisintegration = amount; }

void CCubeModel::PortSetSky(const f32* rgb) {
  sPortSky = rgb != nullptr;
  if (rgb != nullptr) {
    std::copy(rgb, rgb + 3, sPortSkyGain);
  }
}

void CCubeModel::PortSetGlow(const f32* rgb) {
  sPortGlows = rgb != nullptr;
  if (rgb == nullptr) {
    return;
  }
  // Linear, as the converter stores a strength; mode bit 32 exposes it like the record's.
  for (int i = 0; i < 3; ++i) {
    sPortGlow[i] = rgb[i];
  }
}

void CCubeModel::PortOverridePBR(const CCubeModel* model, const int material, const int field,
                                 const f32 value) {
  if (model == nullptr || field < 0 || field >= 19) {
    return;
  }
  for (SPortPBROverride& entry : sPortPBROverrides) {
    if (entry.model == model && entry.material == material && entry.field == field) {
      entry.value = value;
      return;
    }
  }
  const SPortPBROverride entry = {model, material, field, value};
  sPortPBROverrides.push_back(entry);
}

void CCubeModel::PortClearPBROverrides() { sPortPBROverrides.clear(); }

// Draw identification. Serials count up over the whole run (24 bits, 0 is none), so an entry
// is found by its serial in the frame being drawn or the last few before it (a screenshot
// lags the frames by a few).
namespace {
constexpr size_t kPortDrawHistory = 8;
bool sPortDrawLog = false;
bool sPortDrawIds = false;
bool sPortDrawNumbering = false; // either of the two: the one flag DrawSurface tests
uint sPortDrawSerial = 0;
uint sPortDrawFrame = 0;
std::vector< CCubeModel::PortDraw > sPortDraws;                      // the frame being drawn
std::vector< std::vector< CCubeModel::PortDraw > > sPortHistory;     // completed frames, oldest first
} // namespace

void CCubeModel::PortSetDrawLog(const bool on) {
  sPortDrawLog = on;
  sPortDrawNumbering = sPortDrawLog || sPortDrawIds;
  GXPortDrawLog(on ? GX_TRUE : GX_FALSE);
  if (!on) {
    sPortDraws.clear();
    sPortHistory.clear();
  }
}

void CCubeModel::PortSetDrawIds(const bool on) {
  sPortDrawIds = on;
  sPortDrawNumbering = sPortDrawLog || sPortDrawIds;
  // The shader hashes are noted for the serials, so they can be told at the pick.
  GXPortDrawLog(sPortDrawNumbering ? GX_TRUE : GX_FALSE);
}

bool CCubeModel::PortDrawLogOn() { return sPortDrawNumbering; }

bool CCubeModel::PortFindDraw(const uint serial, PortDraw& out) {
  const auto find = [serial, &out](const std::vector< PortDraw >& frame) {
    const auto it = std::lower_bound(frame.begin(), frame.end(), serial,
                                     [](const PortDraw& draw, const uint s) { return draw.serial < s; });
    if (it != frame.end() && it->serial == serial) {
      out = *it;
      return true;
    }
    return false;
  };
  if (find(sPortDraws)) {
    return true;
  }
  for (auto frame = sPortHistory.rbegin(); frame != sPortHistory.rend(); ++frame) {
    if (find(*frame)) {
      return true;
    }
  }
  return false;
}

void CCubeModel::PortLastFrameDraws(std::vector< PortDraw >& out) {
  out = sPortHistory.empty() ? std::vector< PortDraw >() : sPortHistory.back();
}

const CCubeModel* CCubeModel::PortFindModel(const uint asset) {
  // Only the last two frames' draws are trusted: a model that stopped drawing may be gone.
  for (const PortDraw& draw : sPortDraws) {
    if (draw.asset == asset) {
      return draw.model;
    }
  }
  if (!sPortHistory.empty()) {
    for (const PortDraw& draw : sPortHistory.back()) {
      if (draw.asset == asset) {
        return draw.model;
      }
    }
  }
  return nullptr;
}

const char* CCubeModel::PortRecordTag(const int floats, const uint wrap, const bool scaled, const uint cube) {
  const bool wraps = wrap != 0x55555555;
  return cube != 0                ? "PBR7"
         : scaled                 ? "PBR6"
         : wraps && floats == 0   ? "WRAP"
         : wraps                  ? "PBR5"
         : floats == 19           ? "PBR4"
         : floats == 13           ? "PBR3"
         : floats == 8            ? "PBR2"
         : floats == 6            ? "PBRM"
                                  : "none";
}

uint CCubeModel::PortBeginDraw(const CCubeSurface& surface, const bool pbr) const {
  const uint frame = CGraphics::GetFrameCounter();
  if (frame != sPortDrawFrame) {
    sPortDrawFrame = frame;
    sPortHistory.push_back(std::move(sPortDraws));
    sPortDraws.clear();
    if (sPortHistory.size() > kPortDrawHistory) {
      sPortHistory.erase(sPortHistory.begin());
    }
  }
  sPortDrawSerial = sPortDrawSerial >= 0xFFFFFF ? 1 : sPortDrawSerial + 1;
  PortDraw draw{};
  draw.serial = sPortDrawSerial;
  draw.model = this;
  draw.asset = PortAssetId();
  draw.modelIndex = static_cast< uint >(x44_idx);
  draw.material = surface.GetMaterialIndex();
  uint place = 0;
  for (const CCubeSurface* list : {&x38_firstUnsorted, &x3c_firstSorted}) {
    for (CCubeSurface at = *list; at.IsValid(); at = at.GetNextSurface(), ++place) {
      if (at.x0_rawdata == surface.x0_rawdata) {
        draw.surface = place;
        goto found;
      }
    }
  }
found:
  draw.flags = GetMaterialByIndex(draw.material).GetFlags();
  uint cube = 0;
  float lightScale[2];
  draw.floats = PortReadPBRMaterial(static_cast< int >(draw.material), draw.values, &draw.wrap, lightScale, &cube);
  draw.scaled = lightScale[0] != 1.f || lightScale[1] != 1.f;
  draw.cube = cube;
  draw.mode = static_cast< uint >(draw.values[7] + 0.5f);
  draw.kind = draw.values[13];
  draw.pbr = pbr;
  sPortDraws.push_back(draw);
  GXPortSetDrawSerial(draw.serial);
  return draw.serial;
}

void CCubeModel::PortSetFallbackGlow(const CCubeMaterial& material, const int idx,
                                     const bool frameExposed) const {
  f32 values[19];
  PortReadPBRMaterial(idx, values);
  const int mode = int(values[7] + 0.5f);
  // Only an exposed glow (bit 32) follows the room; the embedded TEV's emissive konst is K0,
  // or K1 when ColorUnlit (bit 8) takes K0 (see PbrMaterial in the converter).
  const uint want = (mode & 8) != 0 ? 2 : 1;
  if ((mode & 32) == 0 || material.PortKonstCount() != want) {
    return;
  }
  // The konst holds the emissive in gamma 2.2 and cannot exceed 1.0, so a glow brighter
  // than that (s * gain > 1) is clamped to it.
  const f32 gain = PortRoomEnv::GlowGain(frameExposed);
  GXColor color{0, 0, 0, 255};
  u8* channels[3] = {&color.r, &color.g, &color.b};
  for (int i = 0; i < 3; ++i) {
    const f32 linear = std::clamp(values[i] * gain, 0.f, 1.f);
    *channels[i] = static_cast< u8 >(std::pow(linear, 1.f / 2.2f) * 255.f + 0.5f);
  }
  CGX::SetTevKColor(static_cast< GXTevKColorID >(want - 1), color);
}

f32 CCubeModel::PortSetPBRMaterial(const int idx, const f32 fade, const bool fadeReplaces,
                                   const bool frameExposed, uint* cube) const {
  f32 values[19];
  f32 lightScale[2];
  f32 shield[32];
  PortReadPBRMaterial(idx, values, nullptr, lightScale, cube, shield);
  const f32 kind = values[13];
  if (sPortGlows) {
    values[0] = sPortGlow[0];
    values[1] = sPortGlow[1];
    values[2] = sPortGlow[2];
  }
  // Mode bit 32: the glow is Remastered's bare product, exposed at run time instead of at
  // the 0.10 the older imports baked into the map. Bit 64: the same for a kind's glow
  // strength (values[14]). The shader knows neither bit.
  const int mode = int(values[7] + 0.5f);
  if ((mode & (32 | 64)) != 0) {
    const f32 gain = PortRoomEnv::GlowGain(frameExposed);
    if ((mode & 32) != 0) {
      for (int i = 0; i < 3; ++i) {
        values[i] *= gain;
      }
    }
    if ((mode & 64) != 0) {
      values[14] *= gain;
    }
    values[7] = f32(mode & ~(32 | 64));
  }
  if ((mode & 131072) != 0) {
    // A bare unlit surface: the backlight's place holds the exposure GlowScale leaves.
    const f32 gain = PortRoomEnv::UnlitGain(frameExposed);
    values[3] = values[4] = values[5] = gain;
  }
  if (sPortSky) {
    // Unlit (1) and a sky (16), keeping the material's other flags: the backlight's place
    // holds the gain (see GXSetPBRMaterial).
    values[3] = sPortSkyGain[0];
    values[4] = sPortSkyGain[1];
    values[5] = sPortSkyGain[2];
    values[7] = f32((int(values[7] + 0.5f) & 15) | 17);
  }
  if (kind > 11.5f && kind < 12.5f) {
    values[15] = sPortChargeShell;
    // The glow's IINT pulses 3..10 and back once a second (CIceBeamMP1::PreRenderGunFx:
    // 3 + 14 v, v 0..0.5..0); the record holds the peak, 10.
    const f32 phase = CGraphics::GetSecondsMod900();
    const f32 v = 0.5f - std::fabs(phase - std::floor(phase) - 0.5f);
    const f32 pulse = (3.f + 14.f * v) / 10.f;
    values[0] *= pulse;
    values[1] *= pulse;
    values[2] *= pulse;
  }
  for (const SPortPBROverride& entry : sPortPBROverrides) {
    if (entry.model == this && entry.material == idx) {
      values[entry.field] = entry.value;
    }
  }
  // A liquid's surface (kinds 5 and 6) moves: its first parameter is a rate, and the
  // shader gets the phase. So do falling water (kind 7), the beam glow (9) and the
  // Waste Disposal tank's distortion (11), the Frigate's force fields (14) and the Phazon3 stone (21); glass (8) does not
  // move.
  if ((values[13] > 4.5f && values[13] < 7.5f) || (values[13] > 8.5f && values[13] < 9.5f) ||
      (values[13] > 10.5f && values[13] < 11.5f) || (values[13] > 13.5f && values[13] < 15.5f) || (values[13] > 17.5f && values[13] < 18.5f) ||
      (values[13] > 20.5f && values[13] < 22.5f) || (values[13] > 28.5f && values[13] < 29.5f) ||
      (values[13] > 31.5f && values[13] < 32.5f)) {
    // The Phazon3 stone and the PhazonPool blister read Remastered's scene clock (stops while paused, wraps at 15120 s).
    values[15] *= ((values[13] > 20.5f && values[13] < 22.5f) || (values[13] > 31.5f && values[13] < 32.5f))
                      ? CGraphics::GetSimTime()
                      : CGraphics::GetSecondsMod900();
  }
  const CTransform4f& view = CGraphics::GetViewMatrix();
  // The pickup (kind 15) reads its gradient at the world position: rows 4 and 5 of its constants
  // are world x and y as the dot of the view-space position (right, up, -forward) with xyz, plus w.
  // The hologram (kind 18) reads its texture at the world position the same way.
  if ((kind > 14.5f && kind < 15.5f) || (kind > 17.5f && kind < 18.5f)) {
    shield[16] = view.Get00();
    shield[17] = view.Get02();
    shield[18] = -view.Get01();
    shield[19] = view.Get03();
    shield[20] = view.Get10();
    shield[21] = view.Get12();
    shield[22] = -view.Get11();
    shield[23] = view.Get13();
  }
  // The Phazon Beam's veins (kind 35) take DIFC.w, the "DisintegrationAmount" variable, from the
  // beam's fade (PortSetDisintegration); the record holds only its initial value.
  if (kind > 34.5f && kind < 35.5f) {
    shield[31] = sPortDisintegration;
  }
  // The HoloGlass (kind 29) offsets its layers by a per-object phase: the sum of the model matrix's
  // translation, times 0.33 (row 4 x of its constants).
  if (kind > 28.5f && kind < 29.5f) {
    const CTransform4f& model = CGraphics::GetModelMatrix();
    shield[16] = (model.Get03() + model.Get13() + model.Get23()) * 0.33f;
  }
  // Mode bit 32768: Remastered's procedural wind. The record holds the model's WIND set (v1, v2,
  // rate, b, c); the shader gets CWindModelDataSourceManager::SimulateSingle's constants for
  // the default wind (direction (0, 0, -1), strength 0.3, no impulses) in rows 0-3, and the
  // inverse of the model->world 3x3 with the world translation in rows 4-6.
  bool wind = false;
  if ((int(values[7] + 0.5f) & 32768) != 0) {
    wind = true;
    const CTransform4f& m = CGraphics::GetModelMatrix();
    const f32 a00 = m.Get00(), a01 = m.Get01(), a02 = m.Get02();
    const f32 a10 = m.Get10(), a11 = m.Get11(), a12 = m.Get12();
    const f32 a20 = m.Get20(), a21 = m.Get21(), a22 = m.Get22();
    const f32 c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    const f32 id = std::fabs(det) > 1e-12f ? 1.f / det : 0.f;
    const f32 v1[3] = {shield[0], shield[1], shield[2]};
    const f32 v2[3] = {shield[3], shield[4], shield[5]};
    const f32 rate = shield[6], b = shield[7], c = shield[8];
    constexpr f32 kStrength = 0.3f;
    constexpr f32 kDir[3] = {0.f, 0.f, -1.f};
    f32 rows[32] = {};
    for (int i = 0; i < 3; ++i) {
      rows[i] = kDir[i] * ((1.f - b) * kStrength);
      rows[4 + i] = v1[i];
      rows[8 + i] = v2[i];
      rows[12 + i] = b + (1.f - b) * (std::fabs(kDir[i]) * 0.75f + 0.25f);
    }
    rows[3] = 0.5f * kStrength + b * (c - 0.5f * kStrength);
    rows[7] = rate * CGraphics::GetSimTime();
    // Inverse rows (adjugate / det) with the world translation in w.
    rows[16] = c00 * id;
    rows[17] = (a02 * a21 - a01 * a22) * id;
    rows[18] = (a01 * a12 - a02 * a11) * id;
    rows[19] = m.Get03();
    rows[20] = c01 * id;
    rows[21] = (a00 * a22 - a02 * a20) * id;
    rows[22] = (a02 * a10 - a00 * a12) * id;
    rows[23] = m.Get13();
    rows[24] = c02 * id;
    rows[25] = (a01 * a20 - a00 * a21) * id;
    rows[26] = (a00 * a11 - a01 * a10) * id;
    rows[27] = m.Get23();
    std::memcpy(shield, rows, sizeof(shield));
  }
  // Only the boundary shield, the pickup, the holograms (16-18) and the Phazon3 stone (21) have constants;
  // every other material clears the last one's.
  GXSetPBRShield(wind || (kind > 13.5f && kind < 19.5f) || (kind > 20.5f && kind < 22.5f) || (kind > 24.5f && kind < 25.5f) || (kind > 27.5f && kind < 29.5f) || (kind > 30.5f && kind < 33.5f) || (kind > 34.5f && kind < 35.5f) ? reinterpret_cast< const f32(*)[4] >(shield) : nullptr);
  // World up as the shader sees it: view space is right, up, -forward.
  const f32 up[3] = {view.Get20(), view.Get22(), -view.Get21()};
  GXSetPBRMaterial(values, values + 3, values[6], values[7], values + 8, values + 13, up);
  // Every material sets its own, so a back copy's factors don't reach the next one.
  GXSetPBRLightScale(lightScale[0], lightScale[1], fade, fadeReplaces ? GX_TRUE : GX_FALSE);
  return kind;
}

namespace {
// The screen copy glass refracts (see DrawSurface). One copy serves the glass drawn after it
// until something changes what it would show: any other EFB copy (they share the spare
// buffer, and the probe capture copies too), an opaque model surface, a new frame or another
// viewport. Glass behind glass is therefore seen through, not refracted twice, as a grab of
// the opaque scene works in Remastered and most engines. It used to copy the whole viewport
// before every glass surface, a pass break and a full-screen conversion each time.
struct SPortGlassCopy {
  bool valid;
  u32 serial;
  int frame;
  int left, top, width, height;
  bool mips; // copied with a mip chain (kind 23 samples blurred levels; the others read level 0)
};
SPortGlassCopy sPortGlassCopy = {false, 0, 0, 0, 0, 0, 0, false};
} // namespace
#endif

void CCubeModel::DrawSurface(const CCubeSurface& surface, const CModelFlags& modelFlags) const {
  const CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());
  if (material.IsFlagSet(kStateFlag_ShadowOccluderMesh) && !sDrawingOccluders) {
    return;
  }

#ifdef TARGET_PC
  // Port: an untinted alpha blend (the arm cannon is always drawn alpha blended for its fade,
  // and Samus fades in and out of the morph ball) stays on a PBR material. At full alpha it
  // draws as opaque, since the PBR shader's alpha is the base map's, which a blend would show
  // through. Below it, it keeps the retail blend and the shader takes the fade, in place of
  // an opaque material's alpha; dropping to the TEV fallback for the fade swapped the look of
  // the whole model. TEV materials keep the retail path.
  const CModelFlags opaqueFlags(CModelFlags::kT_Opaque, static_cast< uchar >(modelFlags.GetShaderSet()),
                                static_cast< CModelFlags::EFlags >(modelFlags.GetOtherFlags()),
                                modelFlags.GetColorRef());
  const CColor& tint = modelFlags.GetColorRef();
  const bool pbrBlend = modelFlags.GetTrans() == CModelFlags::kT_Blend && tint.GetRedu8() == 0xFF &&
                        tint.GetGreenu8() == 0xFF && tint.GetBlueu8() == 0xFF &&
                        material.IsFlagSet(kStateFlag_PortPBR) &&
                        CCubeMaterial::PortPBRAllowed(opaqueFlags);
  const bool solidBlend = pbrBlend && tint.GetAlphau8() == 0xFF;
  const bool fadeBlend = pbrBlend && !solidBlend;
  const CModelFlags& drawFlags = solidBlend ? opaqueFlags : modelFlags;
  // The Ice Beam cannon's frost shell (kind 12) shows only while the beam charges
  // (PortSetChargeShell, or a console override of its first parameter), and only as PBR:
  // the TEV fallback has no dissolve and would freeze the gun for good.
  if (material.IsFlagSet(kStateFlag_PortPBR)) {
    const int idx = static_cast< int >(surface.GetMaterialIndex());
    f32 values[19];
    f32 lightScale[2];
    PortReadPBRMaterial(idx, values, nullptr, lightScale, nullptr);
    if (values[13] > 11.5f && values[13] < 12.5f) {
      f32 amount = sPortChargeShell;
      for (const SPortPBROverride& entry : sPortPBROverrides) {
        if (entry.model == this && entry.material == idx && entry.field == 15) {
          amount = entry.value;
        }
      }
      if (amount <= 0.f || !(fadeBlend || CCubeMaterial::PortPBRAllowed(drawFlags))) {
        return;
      }
    }
  }
  material.SetCurrent(drawFlags, surface, *this);
  if (material.IsFlagSet(kStateFlag_PortHudGlow) && !material.IsFlagSet(kStateFlag_PortPBR)) {
    // Remastered's 3853595c on FRME_Helmet: rgb = T*v1 + T*ICNC (ICNC white), alpha T.a^2*v1.a,
    // drawn with the widget's own blend (additive: src alpha, one) into the UNORM target after
    // tone mapping, so the sum clamps at 1 before the blend. The material's stage 0 is
    // texture * vertex colour; T + T*c is that sum and the TEV clamps it the same way.
    CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_TEXC);
  }
  // Port: PBR mod materials. The fallback TEV set above stays valid for the
  // paths PortPBRAllowed rejects.
  const bool pbr = fadeBlend || (material.IsFlagSet(kStateFlag_PortPBR) &&
                                 CCubeMaterial::PortPBRAllowed(drawFlags));
  // A fading model is shaded as it is at full alpha.
  const CModelFlags& lookFlags = fadeBlend ? opaqueFlags : drawFlags;
  const bool sortedDraw =
      lookFlags.GetTrans() >= CModelFlags::kT_Blend || material.IsFlagSet(kStateFlag_DepthSorting);
  if (!pbr && material.IsFlagSet(kStateFlag_PortPBR)) {
    PortSetFallbackGlow(material, static_cast< int >(surface.GetMaterialIndex()),
                        sortedDraw && !sPortSky);
  }
  if (!sortedDraw) {
    sPortGlassCopy.valid = false;
  }
#else
  material.SetCurrent(modelFlags, surface, *this);
#endif
#ifdef TARGET_PC
  if (pbr) {
    // The probe is in world space with Y and Z swapped (world Z-up to cube Y-up), and the
    // shader's reflection vector is in view space: right, up, -forward.
    const CTransform4f& view = CGraphics::GetViewMatrix();
    const f32 viewToWorld[3][3] = {
        {view.Get00(), view.Get02(), -view.Get01()},
        {view.Get10(), view.Get12(), -view.Get11()},
        {view.Get20(), view.Get22(), -view.Get21()},
    };
    // A room environment from a mod has a cube for where the model stands, which replaces
    // the live probe; it needs no captures, so only `probe off` turns it off.
    // Remastered's RequestLightProbeIfApplicable samples at the world centre of the
    // model's local box (the node translation when the box is invalid); the first-person
    // gun overrides it with the player's position.
    const CTransform4f& modelXf = CGraphics::GetModelMatrix();
    const CVector3f origin = x20_bounds.Invalid() ? modelXf.GetTranslation()
                                                  : modelXf * x20_bounds.GetCenterPoint();
    float pos[3] = {origin.GetX(), origin.GetY(), origin.GetZ()};
    PortRoomEnv::ProbeOverride(pos);
    PortRoomEnv::Selection env;
    const int mode = CCubeMaterial::sPortPBRProbeMode;
    const bool found = PortRoomEnv::Select(pos, env);
    if (mode != 0 && found && env.cube != 0) {
      f32 viewToCube[3][3];
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          viewToCube[row][col] = env.worldToCube[row * 3] * viewToWorld[0][col] +
                                 env.worldToCube[row * 3 + 1] * viewToWorld[1][col] +
                                 env.worldToCube[row * 3 + 2] * viewToWorld[2][col];
        }
      }
      GXSetPBRProbeEx(viewToCube, mode > 1 ? static_cast< float >(mode) : 1.f, env.occlusionMin,
                      env.occlusionInvMax);
      GXSetPBRCube(env.cube, env.params);
    } else {
      const f32 viewToProbe[3][3] = {
          {viewToWorld[0][0], viewToWorld[0][1], viewToWorld[0][2]},
          {viewToWorld[2][0], viewToWorld[2][1], viewToWorld[2][2]},
          {viewToWorld[1][0], viewToWorld[1][1], viewToWorld[1][2]},
      };
      static const f32 kNoCube[4] = {0.f, 0.f, 0.f, 0.f};
      GXSetPBRProbe(viewToProbe, CCubeMaterial::sPortPBRProbeWeight);
      GXSetPBRCube(0, kNoCube);
      if (!CCubeMaterial::sPortCapturingProbe) {
        ++CCubeMaterial::sPortPBRProbeDraws;
      }
    }
    if (found && env.hasAmbient) {
      // The baked ambient's directions are in world space, the shader's normal in view space.
      f32 rows[6][3];
      memcpy(rows, env.ambient, sizeof(rows));
      for (int row = 3; row < 6; ++row) {
        for (int col = 0; col < 3; ++col) {
          rows[row][col] = env.ambient[row][0] * viewToWorld[0][col] + env.ambient[row][1] * viewToWorld[1][col] +
                           env.ambient[row][2] * viewToWorld[2][col];
        }
      }
      GXSetPBRAmbient(rows, env.ambientAbsolute ? 2.f : 1.f);
    } else {
      GXSetPBRAmbient(nullptr, 0.f);
    }
    if (found && env.volume != 0) {
      // The volume is in world space, the shader's position and normal in view space.
      const CVector3f eye = view.GetTranslation();
      const f32 at[3] = {eye.GetX(), eye.GetY(), eye.GetZ()};
      f32 rows[6][4];
      for (int row = 0; row < 3; ++row) {
        const f32* w = env.worldToVolume + row * 4;
        const f32* a = env.worldToAxes + row * 3;
        for (int col = 0; col < 3; ++col) {
          rows[row][col] = w[0] * viewToWorld[0][col] + w[1] * viewToWorld[1][col] + w[2] * viewToWorld[2][col];
          rows[3 + row][col] = a[0] * viewToWorld[0][col] + a[1] * viewToWorld[1][col] + a[2] * viewToWorld[2][col];
        }
        rows[row][3] = w[0] * at[0] + w[1] * at[1] + w[2] * at[2] + w[3];
      }
      rows[3][3] = env.volumeLevel;
      rows[4][3] = env.volumeBias;
      rows[5][3] = env.volumeDiagnostic;
      GXSetPBRVolume(env.volume, rows);
    } else {
      GXSetPBRVolume(0, nullptr);
    }
    // Room geometry with a lookup into its room's lightmap is lit by that, per texel, in
    // place of the volume; the material says which texcoord holds the lightmap UV.
    const int lightmapSlot =
        found && env.lightmap != 0 ? PortLightmapSlot(static_cast< int >(surface.GetMaterialIndex())) : -1;
    if (lightmapSlot >= 0) {
      f32 axes[3][3];
      for (int row = 0; row < 3; ++row) {
        const f32* a = env.worldToLightmap + row * 3;
        for (int col = 0; col < 3; ++col) {
          axes[row][col] = a[0] * viewToWorld[0][col] + a[1] * viewToWorld[1][col] + a[2] * viewToWorld[2][col];
        }
      }
      GXSetPBRLightmapAttr(static_cast< GXAttr >(GX_VA_TEX0 + lightmapSlot));
      GXSetPBRLightmap(env.lightmap, env.lightmapRect, axes);
    } else {
      GXSetPBRLightmapAttr(GX_VA_NULL);
      GXSetPBRLightmap(0, nullptr, nullptr);
    }
    // Lit by the bake, which holds the area's light, a model keeps only the runtime lights.
    const bool baked =
        found && ((env.hasAmbient && env.ambientAbsolute) || env.volume != 0 || lightmapSlot >= 0);
    GXSetPBRLightSkip(baked && !PortRoomEnv::AreaLights() ? CCubeMaterial::sPortAreaLights : 0u);
    // The frame's tone curve, when rooms are exposed as Remastered exposes them. Remastered
    // draws the opaque pass's emitted light at the room's static exposure and the sorted
    // pass's at the frame's (CGameRenderJob::RenderPrimaryPass).
    f32 tone[3][4];
    const bool hasTone = PortRoomEnv::Tone(tone);
    if (hasTone) {
      // A sky's gain (PortSetSky) assumes GlowScale, blended or not.
      tone[0][3] = sortedDraw && !sPortSky ? 1.f : PortRoomEnv::GlowScale();
    }
    GXSetPBRTone(hasTone ? tone : nullptr);
    // Remastered's CharacterBacklight fades over the model's bounds along its own y (not
    // z, its height), 0 at the low end and 1 at the high end (CGraphicsModelLoadUtil::LoadMaterialCache), here taken from the
    // view position through the world. The back light is coloured like the ambient along world
    // (1, 1, -1) / sqrt(3). Its strengths are NRenderDebugDefaults' 4 (back) and 2 (top), times
    // the area's backlight hints (PortRoomEnv::Backlight, which gives these as the default).
    // MP_REMASTERED_BACKLIGHT=0 turns it off.
    static const bool sBacklightOff = [] {
      const char* env = getenv("MP_REMASTERED_BACKLIGHT");
      return env != nullptr && env[0] == '0';
    }();
    if (sBacklightOff) {
      GXSetPBRBacklight(nullptr, nullptr, 0.f, 0.f, 0.f, 0.f);
    } else {
      const CAABox& box = GetBoundingBox();
      const f32 bottom = box.GetMinPoint().GetY();
      const f32 extent = box.GetMaxPoint().GetY() - bottom;
      const f32 scale = extent > 1.1920929e-7f ? 1.f / extent : 1.f;
      const CTransform4f toModel = CGraphics::GetModelMatrix().GetInverse();
      const CVector3f eye = view.GetTranslation();
      const f32 k = 0.57735f;
      f32 plane[4];
      f32 backDir[3];
      for (int col = 0; col < 3; ++col) {
        plane[col] = (toModel.Get10() * viewToWorld[0][col] + toModel.Get11() * viewToWorld[1][col] +
                      toModel.Get12() * viewToWorld[2][col]) *
                     scale;
        backDir[col] = k * (viewToWorld[0][col] + viewToWorld[1][col] - viewToWorld[2][col]);
      }
      plane[3] = (toModel.Get10() * eye.GetX() + toModel.Get11() * eye.GetY() + toModel.Get12() * eye.GetZ() +
                  toModel.Get13() - bottom) *
                 scale;
      float top = 2.f;
      float back = 4.f;
      PortRoomEnv::Backlight(top, back);
      GXSetPBRBacklight(plane, backDir, back, top, scale, -bottom * scale);
    }
    // An opaque material's own alpha (dst factor zero) means nothing to a blend.
    uint materialCube = 0;
    const f32 kind = PortSetPBRMaterial(surface.GetMaterialIndex(), fadeBlend ? tint.GetAlpha() : 1.f,
                                        fadeBlend && (material.GetCompressedBlend() >> 16) == GX_BL_ZERO,
                                        sortedDraw && !sPortSky, &materialCube);
    // A material with its own reflection cube (the Remastered arm cannon's) reflects that in
    // place of the room's, as Remastered draws it; the room still lights it. The cube is in
    // Remastered's world, which maps from ours as the room probes' cubes do (-x, z, y).
    f32 materialCubeParams[4];
    const uint materialCubeId =
        materialCube != 0 && mode != 0 && !CCubeMaterial::sPortCapturingProbe ? PortRoomEnv::MaterialCube(materialCube, materialCubeParams) : 0u;
    if (materialCubeId != 0) {
      f32 viewToCube[3][3] = {
          {-viewToWorld[0][0], -viewToWorld[0][1], -viewToWorld[0][2]},
          {viewToWorld[2][0], viewToWorld[2][1], viewToWorld[2][2]},
          {viewToWorld[1][0], viewToWorld[1][1], viewToWorld[1][2]},
      };
      // The ice (kind 28, 088e025e) darkens its reflection by the room's occlusion as the room cubes do.
      const bool occluded = found && kind > 27.5f && kind < 28.5f;
      GXSetPBRProbeEx(viewToCube, 1.f, occluded ? env.occlusionMin : 1.f, occluded ? env.occlusionInvMax : 1.f);
      GXSetPBRCube(materialCubeId, materialCubeParams);
    }
    // Glass (kinds 8 and 11) and the force fields (14) see what is behind them: the screen so far,
    // copied into map 7 as the refracting particles copy it (CElementGen).
    if (((kind > 7.5f && kind < 8.5f) || (kind > 10.5f && kind < 11.5f) || (kind > 13.5f && kind < 14.5f) ||
         (kind > 22.5f && kind < 23.5f) || (kind > 28.5f && kind < 30.5f) || (kind > 33.5f && kind < 34.5f)) &&
        CCubeMaterial::PortScreenCopyUsed()) {
      int portLeft, portTop, portWidth, portHeight;
      CGraphics::GetViewport(portLeft, portTop, portWidth, portHeight);
      SPortGlassCopy& copy = sPortGlassCopy;
      const bool wantMips = (kind > 22.5f && kind < 23.5f) || (kind > 28.5f && kind < 30.5f);
      const bool current = copy.valid && copy.serial == GXPortCopySerial() &&
                           copy.frame == CGraphics::GetFrameCounter() && copy.left == portLeft &&
                           copy.top == portTop && copy.width == portWidth && copy.height == portHeight &&
                           (copy.mips || !wantMips);
      if (!current) {
        GXSetTexCopySrc(static_cast< u16 >(portLeft), static_cast< u16 >(portTop), static_cast< u16 >(portWidth),
                        static_cast< u16 >(portHeight));
        GXSetTexCopyDst(static_cast< u16 >(portWidth), static_cast< u16 >(portHeight), GX_TF_RGB565,
                         wantMips ? GX_TRUE : GX_FALSE);
        const bool useVideoFilter = CGraphics::GetUseVideoFilter();
        CGraphics::SetUseVideoFilter(false);
        GXCopyTex(CGraphics::GetDolphinSpareBuffer(), GX_FALSE);
        CGraphics::SetUseVideoFilter(useVideoFilter);
        GXPixModeSync();
        copy.valid = true;
        copy.serial = GXPortCopySerial();
        copy.frame = CGraphics::GetFrameCounter();
        copy.left = portLeft;
        copy.top = portTop;
        copy.width = portWidth;
        copy.height = portHeight;
        copy.mips = wantMips;
      }
      // Map 7 is shared, so it is bound again either way.
      CGraphics::LoadDolphinSpareTexture(portWidth, portHeight, GX_TF_RGB565, nullptr,
                                         CGraphics::kSpareBufferTexMapID);
      if (wantMips) {
        // The shader picks the level (textureSampleLevel); the sampler must allow them all.
        GXTexObj mipObj;
        GXInitTexObj(&mipObj, CGraphics::GetDolphinSpareBuffer(), static_cast< u16 >(portWidth),
                     static_cast< u16 >(portHeight), GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_TRUE);
        GXInitTexObjLOD(&mipObj, GX_LIN_MIP_LIN, GX_LINEAR, 0.f, 16.f, 0.f, GX_DISABLE, GX_DISABLE,
                        GX_ANISO_1);
        GXLoadTexObj(&mipObj, CGraphics::kSpareBufferTexMapID);
        CTexture::InvalidateTexmap(CGraphics::kSpareBufferTexMapID);
      }
    }
    GXSetPBR(GX_TRUE);
    ++CCubeMaterial::sPortPBRDraws;
  }
  if (CCubeMaterial::sPortPBRThermal == CCubeMaterial::kPT_Additive) {
    // As CFluidPlaneCPU::RenderSetup in the thermal visor's hot pass (a TEV fallback too).
    CGX::SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    CGX::SetZMode(true, GX_LEQUAL, false);
  }
  // Names the draw in Aurora's warnings (a mod model whose texgen reads a missing UV set).
  GXSetDrawTag(PortAssetId(), static_cast< u32 >(x44_idx), surface.GetMaterialIndex());
  if (sPortDrawNumbering) {
    PortBeginDraw(surface, pbr);
  }
#endif
#ifdef TARGET_PC
  // A converted Remastered HUD picture: its alpha squared after the filter, or its
  // UI_Interference static (aurora's GX_AURORA_SET_HUD_SAMPLE).
  u8 hudSample = 0;
  if (material.IsFlagSet(kStateFlag_PortHudInterference)) {
    hudSample = static_cast< u8 >(
        2 + ((material.GetFlags() >> kStateFlag_PortHudInterferenceShift) & 7));
    GXSetHudSample(hudSample, CCubeMaterial::sPortHudDyin[0], CCubeMaterial::sPortHudDyin[1]);
  } else if (material.IsFlagSet(kStateFlag_PortHudSquare)) {
    hudSample = 1;
    GXSetHudSample(hudSample, 0.f, 0.f);
  }
#endif
  surface.CallDisplayList();
#ifdef TARGET_PC
  if (hudSample != 0) {
    GXSetHudSample(0, 0.f, 0.f);
  }
  GXSetDrawTag(0, 0xFFFFFFFF, 0);
  if (sPortDrawNumbering) {
    GXPortSetDrawSerial(0);
  }
  if (pbr) {
    GXSetPBR(GX_FALSE);
  }
#endif
}

static inline const ushort ReadWireframeIndex(const uchar* data) {
  uchar bytes[2];
  bytes[0] = data[0];
  bytes[1] = data[1];
#ifdef __MWERKS__
  return CBasics::SwapBytes(*reinterpret_cast< const ushort* >(bytes));
#else
  ushort value;
  memcpy(&value, bytes, sizeof(value));
  return CBasics::SwapBytes(value);
#endif
}

void CCubeModel::DrawSurfaceWireframe(const CCubeSurface& surface) const {
  const CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());

  static uint sLastDesc = 0;
  static uint sAttrCount = 0;
  uint vertexAttributes = material.GetVertexDesc();

  if (vertexAttributes != sLastDesc) {
    sAttrCount = 0;
    for (int i = 0; i < 16; ++i, sLastDesc = vertexAttributes) {
      if ((vertexAttributes >> (i * 2)) & 3) {
        sAttrCount++;
      }
    }
  }

  const int attrCountTimes2 = sAttrCount * 2;
  static const GXVtxDescList sDesc[] = {
      {GX_VA_POS, GX_INDEX16},
      {GX_VA_NULL, GX_NONE},
  };
  CGX::SetVtxDescv(sDesc);
  CGX::SetTevDirect(GX_TEVSTAGE0);
  CGX::SetNumIndStages(0);
  CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE);
  CGX::SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  CGX::SetNumChans(0);
  CGX::SetNumTexGens(1);
  CGX::SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

  const int displayListSize = surface.GetDisplayListSize();
  const uchar* dispList = static_cast< const uchar* >(surface.GetDisplayList());
  for (int bytesRead = 0; bytesRead < displayListSize;) {
    const uchar pType = *dispList & 0xf8;
    if (!pType) {
      break;
    }
    bytesRead += 3;
    ushort elementCount = ReadWireframeIndex(dispList + 1);
    dispList += 3;
    if (elementCount < 3U) {
      break;
    }

    CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
    GXPosition1x16(ReadWireframeIndex(dispList));
    GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2));
    GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2 * 2));
    GXPosition1x16(ReadWireframeIndex(dispList));
    bytesRead += elementCount * attrCountTimes2;
    dispList += attrCountTimes2 * 3;
    CGX::End();
    if (pType == GX_TRIANGLES) {
      elementCount -= 3;
      for (int j = 0; j < elementCount; j += 3) {
        CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
        GXPosition1x16(ReadWireframeIndex(dispList));
        GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2));
        GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2 * 2));
        GXPosition1x16(ReadWireframeIndex(dispList));
        dispList += attrCountTimes2 * 3;
        CGX::End();
      }
    } else if (pType == GX_TRIANGLESTRIP) {
      elementCount -= 3;
      uint winding = 1;
      for (int j = 0; j < elementCount; ++j) {
        CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 3);
        const uchar* last = dispList - attrCountTimes2 * ((winding ^ 1) + 1);
        const uchar* first = dispList - attrCountTimes2 * (winding + 1);
        winding ^= 1;
        GXPosition1x16(ReadWireframeIndex(first));
        GXPosition1x16(ReadWireframeIndex(dispList));
        dispList += attrCountTimes2;
        GXPosition1x16(ReadWireframeIndex(last));
        CGX::End();
      }
    } else {
      if (pType != GX_TRIANGLEFAN) {
        return;
      }
      elementCount -= 3;
      const uchar* indices = dispList - attrCountTimes2 * 3;

      for (int j = 0; j < elementCount; ++j) {
        const uchar* previous = dispList - attrCountTimes2;
        CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 3);
        GXPosition1x16(ReadWireframeIndex(previous));
        GXPosition1x16(ReadWireframeIndex(dispList));
        dispList += attrCountTimes2;
        GXPosition1x16(ReadWireframeIndex(indices));
        CGX::End();
      }
    }
  }
}

bool CCubeModel::TryLockTextures() const {
  if (!x40_24_loadTextures) {
    bool texturesLoading = false;
    for (int i = 0; i < GetTextures().size(); ++i) {
      GetTextures()[i].Lock();
      if (!GetTextures()[i].TryCache()) {
        texturesLoading = true;
      } else if (!GetTextures()[i].GetObject()->LoadToMRAM()) {
        texturesLoading = true;
      }
    }

    if (!texturesLoading) {
      x40_24_loadTextures = true;
    }
  }

  return !!x40_24_loadTextures;
}

void CCubeModel::DrawSurfaces(const CModelFlags& flags) const {
  if (sDrawingWireframe) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
  } else if ((flags.GetOtherFlags() & CModelFlags::kF_NoTextureLock) || TryLockTextures()) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
  }
}

void CCubeModel::DrawNormalSurfaces(const CModelFlags& flags) const {
  if (sDrawingWireframe) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
  } else if (TryLockTextures()) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
  }
}

void CCubeModel::DrawAlphaSurfaces(const CModelFlags& flags) const {
  if (sDrawingWireframe) {
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
  } else if (TryLockTextures()) {
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
  }
}

void CCubeModel::DrawFlat(const float* positions, const float* normals,
                          ESurfaceSelection which) const {
  if (positions != nullptr) {
    SetSkinningArraysCurrent(positions, normals);
  } else {
    SetArraysCurrent();
  }

  if (which != kSS_Sorted) {
    for (CCubeSurface surface = x38_firstUnsorted; surface.IsValid();
         surface = surface.GetNextSurface()) {
      CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());
      CGX::SetVtxDescv_Compressed(material.GetVertexDescLwzx());
      surface.CallDisplayList();
    }
  }

  if (which != kSS_Unsorted) {
    for (CCubeSurface surface = x3c_firstSorted; surface.IsValid();
         surface = surface.GetNextSurface()) {
      CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());
      CGX::SetVtxDescv_Compressed(material.GetVertexDescLwzx());
      surface.CallDisplayList();
    }
  }
}

void CCubeModel::Draw(const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetArraysCurrent();
  DrawSurfaces(flags);
}

void CCubeModel::Draw(const float* positions, const float* normals,
                      const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetSkinningArraysCurrent(positions, normals);
  DrawSurfaces(flags);
}

void CCubeModel::DrawNormal(const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetArraysCurrent();
  DrawNormalSurfaces(flags);
}

void CCubeModel::DrawAlpha(const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetArraysCurrent();
  DrawAlphaSurfaces(flags);
}

void CCubeModel::SetDrawingOccluders(const bool drawOccluders) {
  sDrawingOccluders = drawOccluders;
}

void CCubeModel::SetModelWireframe(const bool drawWireframe) { sDrawingWireframe = drawWireframe; }

void CCubeModel::UnlockTextures() const {
  for (AUTO(texture, x1c_textures->begin()); texture != x1c_textures->end(); ++texture) {
    texture->Unlock();
  }

  x40_24_loadTextures = false;
}

void CCubeModel::RemapMaterialData(const void* data,
                                   rstl::vector< TCachedToken< CTexture > >* texture) {

  x0_instance.SetMaterialPointer(data);
  x1c_textures = texture;
  x40_24_loadTextures = false;
}

void CCubeModel::DrawNormal(const float* positions, const float* normals,
                            ESurfaceSelection which) const {
  CGX::SetNumIndStages(0);
  CGX::SetNumTevStages(1);
  CGX::SetNumTexGens(1);
  CGX::SetZMode(true, GX_LEQUAL, true);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
  CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
  CGX::SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  CGX::SetStandardTevColorAlphaOp(GX_TEVSTAGE0);
  CGX::SetBlendMode(GX_BM_BLEND, GX_BL_ZERO, GX_BL_ONE, GX_LO_CLEAR);
  DrawFlat(positions, normals, which);
}
