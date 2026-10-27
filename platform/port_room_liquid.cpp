// Liquid surfaces at run time: which areas have a file, their meshes and models, and the
// draw. See port_room_liquid.h.
#include "port_env.h"
#include "port_room_liquid.h"
#include "port_strings.h"
#include "port_bytes.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_room_env.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/TToken.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetaRender/CCubeRenderer.hpp"

#include "aurora/water.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>

namespace PortRoomLiquid {
namespace {

constexpr uint32_t kMagic = 0x4C52504D; // 'MPRL'
constexpr uint32_t kVersion = 5;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kSurfaceBytes = 12 + 12 * 4;
// The Water block before its vertices: 2 counts, the bounds, the feature bytes, then
// 49 floats and 5 ids.
constexpr size_t kWaterBytes = 8 + 24 + 12 + 49 * 4 + 5 * 4;
constexpr size_t kVertexBytes = 32;
constexpr size_t kFilterBytes = 7 * 4;
// How far a surface may be from the water object it is drawn for.
constexpr float kReach = 2.f;
constexpr float kPi = 3.14159265358979f;
// CStateManagerGameData's water clock wraps at this many seconds.
constexpr float kClockWrap = 5000.f;

struct Placed {
  uint32_t type = 0;
  uint32_t id = 0;
  uint32_t owner = 0; // Surface::owner
  CTransform4f xf = CTransform4f::Identity(); // model -> world
  bool taken = false;                         // a water object draws it
  // Lava: a CMDL.
  std::unique_ptr< CModelData > data;
  std::unique_ptr< CActorLights > lights;
  // Water and poison: Remastered's mesh and material.
  Water water;
  std::vector< aurora::gfx::water::Vertex > verts;
  // Null for none (an empty CToken cannot be assigned to).
  std::unique_ptr< TCachedToken< CTexture > > normalMap, flowMap, rainNoise;
};

struct Area {
  bool read = false;
  bool placed = false; // the surfaces have their world transforms
  std::vector< Placed > items;
  std::unordered_map< uint32_t, int > owners; // water object -> its surface, -1 for none
  std::vector< Filter > filters;                     // positions in world space once placed
  std::unordered_map< uint32_t, int > filterOwners; // water object -> its filter, -1 for none
};

// Never destroyed, as port_room_geo.cpp's: the tokens must not outlive the pool.
std::unordered_map< uint32_t, Area >& Areas() {
  static auto* const areas = new std::unordered_map< uint32_t, Area >();
  return *areas;
}

int sEnabled = -1;
int sDrawn = 0;
int sDrawnLast = 0;
float sClock = 0.f; // Advance

using port::HexDigit;

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
using port::ReadAll;

using port::ReadLE32;

// Reads `count` floats at `p`, moving it on; false when one is not finite.
bool ReadFloats(const uint8_t*& p, float* out, size_t count) {
  for (size_t i = 0; i < count; ++i, p += 4) {
    const uint32_t bits = ReadLE32(p);
    std::memcpy(&out[i], &bits, 4);
    if (!std::isfinite(out[i])) {
      return false;
    }
  }
  return true;
}

std::unique_ptr< TCachedToken< CTexture > > TextureToken(uint32_t id) {
  if (id == 0 || gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(id)) != 'TXTR') {
    return nullptr;
  }
  std::unique_ptr< TCachedToken< CTexture > > token(
      new TCachedToken< CTexture >(gpSimplePool->GetObj(SObjectTag('TXTR', static_cast< CAssetId >(id)))));
  token->Lock();
  return token;
}

void Load(uint32_t mrea, Area& area) {
  area.read = true;
  const std::string path = PortMods::RoomLiquidPath(mrea);
  if (path.empty() || gpResourceFactory == nullptr) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector< uint8_t > data = ReadAll(in);
  std::vector< Surface > surfaces;
  std::string error;
  if (!in || !Parse(data, surfaces, area.filters, error)) {
    PortLog::Write("room liquid: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    return;
  }
  size_t missing = 0;
  for (Surface& surface : surfaces) {
    if (surface.type == 2 &&
        gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(surface.model)) != 'CMDL') {
      ++missing;
      continue;
    }
    Placed& item = area.items.emplace_back();
    item.type = surface.type;
    item.id = surface.model;
    item.owner = surface.owner;
    const float* const m = surface.transform;
    item.xf = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    if (surface.type == 2) {
      item.data.reset(
          new CModelData(CStaticRes(static_cast< CAssetId >(surface.model), CVector3f(1.f, 1.f, 1.f))));
      continue;
    }
    item.water = std::move(surface.water);
    const Water& w = item.water;
    item.verts.resize(w.vertices.size());
    for (size_t i = 0; i < w.vertices.size(); ++i) {
      aurora::gfx::water::Vertex& v = item.verts[i];
      std::memcpy(v.pos, w.vertices[i].pos, sizeof(v.pos));
      std::memcpy(v.uv, w.vertices[i].uv, sizeof(v.uv));
      std::memcpy(v.color, w.vertices[i].color, sizeof(v.color));
    }
    item.normalMap = TextureToken(w.normalMap);
    item.flowMap = TextureToken(w.flowMap);
    item.rainNoise = TextureToken(w.rainNoise);
  }
  PortLog::Write("room liquid: %08X: %zu surface(s), %zu without a model, %zu water filter(s)\n", mrea,
                 area.items.size(), missing, area.filters.size());
}

// Where the surface lies in the world as Remastered placed it: a lava model's origin, a water
// mesh's top (its bounds' top, y up).
float SurfaceTop(const Placed& item) {
  return item.type == 2 ? item.xf.Get23() : (item.xf * CVector3f(0.f, item.water.boundsMax[1], 0.f)).GetZ();
}

// The area's file, read and placed in the world; null when it is off.
Area* Prepare(const CGameArea& gameArea) {
  if (!Enabled()) {
    return nullptr;
  }
  const uint32_t mrea = gameArea.GetAreaAssetId();
  Area& area = Areas()[mrea];
  if (!area.read) {
    Load(mrea, area);
  }
  if (!area.placed) {
    for (Placed& item : area.items) {
      item.xf = gameArea.GetTM() * item.xf;
    }
    for (Filter& filter : area.filters) {
      const CVector3f world = gameArea.GetTM() * CVector3f(filter.position[0], filter.position[1], filter.position[2]);
      filter.position[0] = world.GetX();
      filter.position[1] = world.GetY();
      filter.position[2] = world.GetZ();
    }
    area.placed = true;
  }
  return &area;
}

// The texture once it is in memory, loaded to `map`; null for none. `pending` is set while
// it is still streaming in.
const GXTexObj* Texture(const std::unique_ptr< TCachedToken< CTexture > >& token, GXTexMapID map, bool& pending) {
  if (token == nullptr) {
    return nullptr;
  }
  if (!token->TryCache()) {
    pending = true;
    return nullptr;
  }
  CTexture* const tex = token->GetObject();
  tex->PortLoad(map, CTexture::kCM_Repeat, CTexture::kCM_Repeat);
  return tex->PortTexObj();
}

// A WaterRenderVolume (CWaterSceneNode): its uniforms as water::SetupSurfaceParamsData and
// SetupWaveSim fill them, its cull and pixel shader as PreRender picks them, and the room's
// light as CCubeModel gives it to a PBR model (build/mpr/water/B-cpu.md, G-rainflags.md).
bool DrawWater(Placed& item, const CStateManager& mgr, uint32_t mrea, const CTransform4f& xf) {
  const Water& w = item.water;
  const uint8_t* const b = w.features;
  // CWaterSceneNode's flags from the loader's feature bytes; the node is made only with a
  // top (b1) or a bottom (b2).
  if (b[1] == 0 && b[2] == 0) {
    return true;
  }
  bool pending = false;
  aurora::gfx::water::DrawDesc d;
  d.normalMap = Texture(item.normalMap, GX_TEXMAP0, pending);
  d.sourceFlow = Texture(item.flowMap, GX_TEXMAP1, pending);
  d.rainNoise = Texture(item.rainNoise, GX_TEXMAP2, pending);
  if (pending) {
    // Skip the frame rather than flash retail's plane in while the maps stream.
    return true;
  }

  // The cull (0x399ff4): both sides drawn with a top and a bottom, else the back faces go
  // for a top and the front faces for a bottom alone.
  d.cull = b[1] != 0 && b[2] != 0 ? aurora::gfx::water::Cull::None
           : b[1] != 0            ? aurora::gfx::water::Cull::Back
                                  : aurora::gfx::water::Cull::Front;

  // PreRender: the camera's signed distance to the plane through the top of the mesh's bounds
  // (the mesh is y up), along the plane's normal through the transform.
  const CTransform4f& view = CGraphics::GetViewMatrix();
  const CVector3f eye = view.GetTranslation();
  const CVector3f planePoint = xf * CVector3f(0.f, w.boundsMax[1], 0.f);
  const CVector3f normal = xf.GetInverse().TransposeRotate(CVector3f(0.f, 1.f, 0.f)).AsNormalized();
  d.bottom = CVector3f::Dot(normal, eye) - CVector3f::Dot(normal, planePoint) < 0.f;

  // uc_surface.
  const float* const mat = w.material;
  const float* const flow = w.flow;
  float(&S)[8][4] = d.surface;
  S[0][0] = sClock;
  S[0][1] = 1.f - mat[1];
  S[0][2] = 1.f / std::max(mat[2], 0.001f);
  S[0][3] = w.normalScale * std::max(w.boundsMax[0] - w.boundsMin[0], w.boundsMax[2] - w.boundsMin[2]);
  S[1][0] = w.normalDir[0];
  S[1][1] = w.normalDir[1];
  S[1][2] = w.normalSpeed;
  S[1][3] = 1.f - mat[1];
  std::memcpy(S[2], w.tint, sizeof(S[2]));
  // CScriptWaterMP1::Render (0xd19404) sets the opacity multiplier in the X-Ray visor and resets
  // it to 1 otherwise; PushTransferData (0x3998b8) multiplies the tint's alpha by it.
  if (mgr.GetPlayerState()->GetActiveVisor(mgr) == CPlayerState::kPV_XRay) {
    S[2][3] *= w.xrayOpacity;
  }
  S[3][0] = mat[0];
  S[3][1] = mat[3];
  S[3][2] = 1.f; // the dynamics simulation's, off
  S[3][3] = 1.f;
  S[4][0] = flow[0];
  S[4][1] = -flow[1];
  S[4][2] = 1.f / std::max(flow[3], 0.001f);
  // The flow simulation's displacement scale. b8 turns Remastered's CFluidSurfaceDynamicsSim on
  // (node+0x608), and the vertex shader lifts the mesh by (height + source flow w - 1) * this. The
  // sim rests at height 1 - w (1 in the water, where w is 0; 0 on land), so a still surface is
  // flat. The port runs no sim and binds black for its height, which would sink every surface
  // whose flow map marks the water with w = 0 (Phendrana Shorelines' by 2.2, under its shelf).
  // So it is 0, as SetupSurfaceParamsData (0x39c09c) leaves it with the sim off.
  S[4][3] = 0.f;
  S[6][0] = S[6][1] = S[6][2] = 1.f;
  S[6][3] = mat[4];
  S[7][0] = w.fogColor[0];
  S[7][1] = w.fogColor[1];
  S[7][2] = w.fogColor[2];
  S[7][3] = 1.f / std::max(w.fogDistance, 0.001f);

  // uc_waveParams: both waves' directions, then per wave its two amplitudes, phase and wave
  // number. No waves when every amplitude is about 0.
  const float(&wave)[2][5] = w.waves;
  if (std::fabs(wave[0][1]) + std::fabs(wave[1][1]) >= 1e-5f ||
      std::fabs(wave[0][2]) + std::fabs(wave[1][2]) >= 1e-5f) {
    constexpr float kDegrees = 0.017453292f;
    d.waves[0][0] = std::cos(wave[1][0] * kDegrees);
    d.waves[0][1] = std::sin(wave[1][0] * kDegrees);
    d.waves[0][2] = std::cos(wave[0][0] * kDegrees);
    d.waves[0][3] = std::sin(wave[0][0] * kDegrees);
    for (int i = 0; i < 2; ++i) {
      const float number = 2.f * kPi / wave[i][3];
      d.waves[1 + i][0] = wave[i][1];
      d.waves[1 + i][1] = wave[i][2];
      d.waves[1 + i][2] = wave[i][4] * number * sClock;
      d.waves[1 + i][3] = number;
    }
  }

  // The rain ripples, with a noise texture and the rain flag (b5).
  if (d.rainNoise != nullptr && b[5] != 0 && w.rainNoiseWidth != 0 && w.rainNoiseHeight != 0) {
    d.rain = true;
    d.rainParams[0][0] = 1.f / float(w.rainNoiseWidth);
    d.rainParams[0][1] = 1.f / float(w.rainNoiseHeight);
    d.rainParams[0][2] = w.rain[0];
    d.rainParams[0][3] = w.rain[1];
    std::memcpy(d.rainParams[1], w.rain + 2, 4 * sizeof(float));
    std::memcpy(d.rainParams[2], w.rain + 6, 4 * sizeof(float));
  }

  // The room's light, as CCubeModel sets it for a PBR model: the probe's cube and the baked
  // volume, both in world space with the shader's directions in view space.
  const f32 viewToWorld[3][3] = {
      {view.Get00(), view.Get02(), -view.Get01()},
      {view.Get10(), view.Get12(), -view.Get11()},
      {view.Get20(), view.Get22(), -view.Get21()},
  };
  const CVector3f centre = CAABox(CVector3f(w.boundsMin[0], w.boundsMin[1], w.boundsMin[2]),
                                  CVector3f(w.boundsMax[0], w.boundsMax[1], w.boundsMax[2]))
                               .GetTransformedAABox(xf)
                               .GetCenterPoint();
  const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
  PortRoomEnv::SetVolumeHint(mrea, at);
  PortRoomEnv::Selection env;
  const bool found = PortRoomEnv::Select(at, env);
  PortRoomEnv::ClearVolumeHint();
  if (found && env.cube != 0) {
    d.cube = env.cube;
    // The TOP shader takes the cube without the probe's intensity.
    d.cubeScale = env.params[0] / (d.bottom ? 1.f : std::max(env.cubeIntensity, 1e-4f));
    d.cubeMips = env.params[1];
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        d.viewToCube[row][col] = env.worldToCube[row * 3] * viewToWorld[0][col] +
                                 env.worldToCube[row * 3 + 1] * viewToWorld[1][col] +
                                 env.worldToCube[row * 3 + 2] * viewToWorld[2][col];
      }
    }
  }
  float modulation[3];
  PortRoomEnv::BakedLightModulation(modulation);
  if (found && env.volume != 0) {
    d.volume = env.volume;
    for (int row = 0; row < 3; ++row) {
      const f32* const v = env.worldToVolume + row * 4;
      for (int col = 0; col < 3; ++col) {
        d.viewToVolume[row][col] = v[0] * viewToWorld[0][col] + v[1] * viewToWorld[1][col] + v[2] * viewToWorld[2][col];
      }
      d.viewToVolume[row][3] = v[0] * eye.GetX() + v[1] * eye.GetY() + v[2] * eye.GetZ() + v[3];
      d.volumeScale[row] = modulation[row] * env.volumeLevel * kPi;
    }
  } else {
    for (int i = 0; i < 3; ++i) {
      d.fallbackMean[i] = found && env.hasAmbient ? env.ambient[0][i] : 1.f;
      d.volumeScale[i] = modulation[i];
    }
  }
  d.volumeScale[3] = 1.f;
  if (PortRoomEnv::Tone(d.tone)) {
    d.tone[0][3] = 1.f; // drawn in the sorted pass
  }
  const CGraphics::CProjectionState& proj = CGraphics::GetProjectionState();
  d.zNear = proj.GetNear();
  d.zFar = proj.GetFar();
  // SetupViewForDraw's depth range.
  d.zMin = 0.125f;
  d.zMax = 1.f;

  gpRender->SetModelMatrix(xf);
  aurora::gfx::water::draw(d, item.verts.data(), uint32_t(item.verts.size()), w.indices.data(),
                           uint32_t(w.indices.size()));
  return true;
}

bool DrawLava(Placed& item, const CStateManager& mgr, const CGameArea& gameArea, uint32_t mrea,
              const CTransform4f& xf) {
  if (!item.data->IsLoaded(0)) {
    item.data->Touch(CModelData::kWM_Normal, 0);
    return false;
  }
  const CAABox bounds = item.data->GetBounds().GetTransformedAABox(xf);
  const bool baked = PortRoomEnv::HasVolume(mrea);
  if (item.lights == nullptr) {
    // As room geometry: the baked ambient already holds the area's lights.
    item.lights.reset(new CActorLights(8, CVector3f(0.f, 0.f, 0.f), 4, baked ? 0 : 4));
    if (baked) {
      item.lights->SetAmbientColor(CColor::White());
    }
  }
  if (baked) {
    const CVector3f centre = bounds.GetCenterPoint();
    const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
    PortRoomEnv::SetVolumeHint(mrea, at);
  } else {
    item.lights->BuildAreaLightList(mgr, gameArea, bounds);
  }
  item.lights->BuildDynamicLightList(mgr, bounds);
  // The object is drawn in the sorted pass, so both halves go here. The thermal visor's
  // passes take the fluid plane's whole shader too (the hot one adds it), so the surface
  // keeps its own; the X-Ray visor draws it as it is.
  const EThermalDrawFlag thermal = mgr.GetThermalDrawFlag();
  const CCubeMaterial::EPortPBRThermal outerThermal = CCubeMaterial::sPortPBRThermal;
  CCubeMaterial::sPortPBRThermal = thermal == kTD_Hot    ? CCubeMaterial::kPT_Additive
                                   : thermal == kTD_Cold ? CCubeMaterial::kPT_Cold
                                                         : CCubeMaterial::kPT_None;
  const CModel& model = **item.data->PickStaticModel(CModelData::kWM_Normal);
  gpRender->SetModelMatrix(xf);
  item.lights->ActivateLights();
  model.DrawUnsortedParts(CModelFlags::Normal());
  model.DrawSortedParts(CModelFlags::Normal());
  if (CCubeMaterial::sPortPBRThermal == CCubeMaterial::kPT_Additive) {
    // The blend was set past the material cache.
    CCubeMaterial::ResetCachedMaterials();
  }
  CCubeMaterial::sPortPBRThermal = outerThermal;
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  if (baked) {
    PortRoomEnv::ClearVolumeHint();
  }
  return true;
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  return port::ParseHexFileName(fileName, ".roomliquid", id);
}

bool Parse(const std::vector< uint8_t >& data, std::vector< Surface >& out, std::vector< Filter >& filters,
           std::string& error) {
  out.clear();
  filters.clear();
  if (data.size() < kHeaderBytes || ReadLE32(data.data()) != kMagic) {
    error = "not a room liquid file";
    return false;
  }
  if (ReadLE32(data.data() + 4) != kVersion) {
    error = "unknown version";
    return false;
  }
  const size_t count = ReadLE32(data.data() + 8);
  const uint8_t* p = data.data() + kHeaderBytes;
  const uint8_t* const end = data.data() + data.size();
  auto fail = [&](const char* why) {
    error = why;
    out.clear();
    filters.clear();
    return false;
  };
  if (count > size_t(end - p) / kSurfaceBytes) {
    return fail("truncated");
  }
  out.resize(count);
  for (Surface& s : out) {
    if (size_t(end - p) < kSurfaceBytes) {
      return fail("truncated");
    }
    s.type = ReadLE32(p);
    s.model = ReadLE32(p + 4);
    s.owner = ReadLE32(p + 8);
    p += 12;
    if (s.type > 2) {
      return fail("unknown surface type");
    }
    if (!ReadFloats(p, s.transform, 12)) {
      return fail("a transform is not finite");
    }
    if (s.type == 2) {
      continue;
    }
    if (size_t(end - p) < kWaterBytes) {
      return fail("truncated");
    }
    Water& w = s.water;
    const uint32_t vertices = ReadLE32(p);
    const uint32_t indices = ReadLE32(p + 4);
    p += 8;
    bool finite = ReadFloats(p, w.boundsMin, 3) && ReadFloats(p, w.boundsMax, 3);
    std::memcpy(w.features, p, sizeof(w.features));
    p += 12;
    finite = finite && ReadFloats(p, &w.waves[0][0], 10) && ReadFloats(p, w.tint, 4) &&
             ReadFloats(p, w.normalDir, 2) && ReadFloats(p, &w.normalSpeed, 1) && ReadFloats(p, &w.normalScale, 1) &&
             ReadFloats(p, w.fogColor, 4) && ReadFloats(p, &w.fogDistance, 1) && ReadFloats(p, w.material, 5) &&
             ReadFloats(p, w.rain, 10) && ReadFloats(p, w.flow, 10) && ReadFloats(p, &w.xrayOpacity, 1);
    if (!finite) {
      return fail("a water value is not finite");
    }
    w.normalMap = ReadLE32(p);
    w.flowMap = ReadLE32(p + 4);
    w.rainNoise = ReadLE32(p + 8);
    w.rainNoiseWidth = ReadLE32(p + 12);
    w.rainNoiseHeight = ReadLE32(p + 16);
    p += 20;
    if (indices % 3 != 0) {
      return fail("indices are not triangles");
    }
    if (vertices > size_t(end - p) / kVertexBytes || indices > (size_t(end - p) - size_t(vertices) * kVertexBytes) / 4) {
      return fail("truncated");
    }
    w.vertices.resize(vertices);
    for (Water::Vertex& v : w.vertices) {
      if (!ReadFloats(p, v.pos, 3) || !ReadFloats(p, v.uv, 4)) {
        return fail("a vertex is not finite");
      }
      std::memcpy(v.color, p, 4);
      p += 4;
    }
    w.indices.resize(indices);
    for (uint32_t& index : w.indices) {
      index = ReadLE32(p);
      p += 4;
      if (index >= vertices) {
        return fail("an index is out of range");
      }
    }
  }
  if (size_t(end - p) < 4) {
    return fail("truncated");
  }
  const size_t filterCount = ReadLE32(p);
  p += 4;
  if (filterCount > size_t(end - p) / kFilterBytes) {
    return fail("truncated");
  }
  filters.resize(filterCount);
  for (Filter& filter : filters) {
    if (!ReadFloats(p, filter.position, 3) || !ReadFloats(p, filter.color, 4)) {
      return fail("a filter is not finite");
    }
  }
  if (p != end) {
    return fail("trailing bytes");
  }
  return true;
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  auto& areas = Areas();
  for (auto it = areas.begin(); it != areas.end();) {
    bool here = false;
    for (size_t i = 0; i < count; ++i) {
      here |= mreas[i] == it->first;
    }
    it = here ? std::next(it) : areas.erase(it);
  }
  sDrawnLast = sDrawn;
  sDrawn = 0;
}

void Advance(float dt) {
  sClock += dt;
  if (sClock >= kClockWrap) {
    sClock -= kClockWrap;
  }
}

bool Draw(const CStateManager& mgr, const CGameArea& gameArea, uint32_t uid, uint32_t editorId,
          const CVector3f& position, int fluidType, float surfaceZ) {
  Area* const prepared = Prepare(gameArea);
  if (prepared == nullptr || prepared->items.empty()) {
    return false;
  }
  Area& area = *prepared;
  const uint32_t mrea = gameArea.GetAreaAssetId();
  auto owner = area.owners.find(uid);
  if (owner == area.owners.end()) {
    // The surface the importer matched to this object; else the nearest unmatched surface of
    // what the object holds (thick lava is lava here).
    const uint32_t type = fluidType == 1 ? 1 : fluidType == 2 || fluidType == 5 ? 2 : fluidType == 0 ? 0 : ~0u;
    const uint32_t eid = editorId & 0x3FFFFFFu;
    int best = -1;
    float bestDistance = 1e30f;
    bool matched = false;
    for (size_t i = 0; i < area.items.size(); ++i) {
      const Placed& item = area.items[i];
      if (eid != 0 && item.owner == eid && !item.taken) {
        best = int(i);
        bestDistance = (CVector3f(item.xf.Get03(), item.xf.Get13(), item.xf.Get23()) - position).Magnitude();
        matched = true;
        break;
      }
    }
    for (size_t i = 0; !matched && i < area.items.size(); ++i) {
      const Placed& item = area.items[i];
      const float distance = (CVector3f(item.xf.Get03(), item.xf.Get13(), item.xf.Get23()) - position).Magnitude();
      if (item.owner == 0 && item.type == type && !item.taken && distance < bestDistance) {
        best = int(i);
        bestDistance = distance;
      }
    }
    PortLog::Write("room liquid: %08X: object %u (editor %08X, fluid %d) at (%.1f, %.1f, %.1f), surface at %.1f: %s"
                   ", %.2f away, its own top at %.2f\n",
                   mrea, uid, editorId, fluidType, position.GetX(), position.GetY(), position.GetZ(), surfaceZ,
                   matched              ? "matched surface"
                   : best < 0           ? "no surface"
                   : bestDistance > kReach ? "nearest surface too far"
                                           : "surface found",
                   best < 0 ? 0.f : bestDistance, best < 0 ? 0.f : SurfaceTop(area.items[size_t(best)]));
    if (!matched && bestDistance > kReach) {
      best = -1;
    }
    if (best >= 0) {
      area.items[size_t(best)].taken = true;
    }
    owner = area.owners.emplace(uid, best).first;
  }
  if (owner->second < 0) {
    return false;
  }
  Placed& item = area.items[size_t(owner->second)];
  // The surface lies where the game has its own, which a script may raise or drain: a lava
  // model's origin, a water mesh's top (its bounds' top, y up).
  CTransform4f xf(item.xf);
  xf.AddTranslationZ(surfaceZ - SurfaceTop(item));
  if (item.type == 2 ? !DrawLava(item, mgr, gameArea, mrea, xf) : !DrawWater(item, mgr, mrea, xf)) {
    return false;
  }
  ++sDrawn;
  return true;
}

bool CameraFilter(const CGameArea& gameArea, uint32_t uid, const CVector3f& position, float color[4]) {
  Area* const area = Prepare(gameArea);
  if (area == nullptr || area->filters.empty()) {
    return false;
  }
  auto owner = area->filterOwners.find(uid);
  if (owner == area->filterOwners.end()) {
    int best = -1;
    float bestDistance = 1e30f;
    for (size_t i = 0; i < area->filters.size(); ++i) {
      const float* const at = area->filters[i].position;
      const float distance = (CVector3f(at[0], at[1], at[2]) - position).Magnitude();
      if (distance < bestDistance) {
        best = int(i);
        bestDistance = distance;
      }
    }
    PortLog::Write("room liquid: %08X: object %u at (%.1f, %.1f, %.1f): %s, %.2f away\n", gameArea.GetAreaAssetId(),
                   uid, position.GetX(), position.GetY(), position.GetZ(),
                   bestDistance > kReach ? "no water filter" : "water filter found", best < 0 ? 0.f : bestDistance);
    owner = area->filterOwners.emplace(uid, bestDistance > kReach ? -1 : best).first;
  }
  if (owner->second < 0) {
    return false;
  }
  std::memcpy(color, area->filters[size_t(owner->second)].color, 4 * sizeof(float));
  return true;
}

void Reset() { Areas().clear(); }

void SetEnabled(bool enabled) {
  sEnabled = enabled ? 1 : 0;
  // The objects are matched again, and the models let go when it is off.
  Areas().clear();
}

bool Enabled() {
  if (sEnabled < 0) {
    sEnabled = port::EnvFlag("MP_ROOM_LIQUID", true) ? 1 : 0;
  }
  return sEnabled != 0;
}

void Stats(int& areas, int& surfaces, int& drawn) {
  areas = surfaces = 0;
  for (const auto& [mrea, area] : Areas()) {
    if (!area.items.empty()) {
      ++areas;
      surfaces += int(area.items.size());
    }
  }
  drawn = sDrawnLast;
}

} // namespace PortRoomLiquid
