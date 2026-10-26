// Room geometry at run time: which areas have a file, their models, and the draw. See
// port_room_geo.h.
#include "port_env.h"
#include "port_strings.h"
#include "port_bytes.h"
#include "port_room_geo.h"
#include "port_maya_spline.h"
#include "port_room_sky.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_room_env.h"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CResLoader.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptActor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDamageableTrigger.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetaRender/CCubeRenderer.hpp"

#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>

namespace PortRoomGeo {
namespace {

// What a model keeps on the GPU (GXPortRetainResident): released when the model goes.
class ResidentKeep {
public:
  ResidentKeep() = default;
  ResidentKeep(const ResidentKeep&) = delete;
  ResidentKeep& operator=(const ResidentKeep&) = delete;
  ResidentKeep(ResidentKeep&& other) noexcept : mKept(std::move(other.mKept)) { other.mKept.clear(); }
  ResidentKeep& operator=(ResidentKeep&& other) noexcept {
    if (this != &other) {
      Release();
      mKept = std::move(other.mKept);
      other.mKept.clear();
    }
    return *this;
  }
  ~ResidentKeep() { Release(); }

  void Keep(const void* data, uint size) {
    if (data != nullptr && size != 0) {
      GXPortRetainResident(data, size);
      mKept.push_back(data);
    }
  }
  void Release() {
    for (const void* data : mKept) {
      GXPortReleaseResident(data);
    }
    mKept.clear();
  }

private:
  std::vector< const void* > mKept;
};

// A coarser level of detail of a model (kLodFileName), drawn from `distanceSq` on.
struct Level {
  uint32_t id = 0;
  float distanceSq = 0.f;
  std::unique_ptr< CModelData > data;
  ResidentKeep resident; // after `data`, as in Model
  bool loaded = false;
};

struct Model {
  uint32_t id = 0;
  std::unique_ptr< CModelData > data;
  // After `data`, so it is released before the model it points into is freed.
  ResidentKeep resident;
  bool loaded = false;
  bool hidden = false; // by the console, to find which model a surface belongs to
  int cutout = -1;     // some material alpha-tests (1) or none does (0); -1 until Draw looks
  CAABox bounds = CAABox::MakeMaxInvertedBox();
  std::vector< Level > levels; // nearest first
};

struct Area;

struct Placed {
  size_t model;
  Area* owner = nullptr; // the area it belongs to; areas stay put in Areas() until erased
  CTransform4f xf = CTransform4f::Identity(); // model -> world
  CAABox bounds = CAABox::MakeMaxInvertedBox();
  bool bounded = false;
  std::unique_ptr< CActorLights > lights;
  bool areaLit = false; // `lights` holds the area's lights
  uint32_t volume = 0;  // the area whose baked ambient lights it, 0 for none
  uint8_t layer = kEveryLayer; // drawn only while this script layer is on
  bool shown = true;           // by the area's scripts
  bool active = true;          // what the file starts it as
  // A platform's slave (0: none): the platform's editor id, where it stood when its area
  // was made, and how far from there it was when `xf` was last moved with it.
  uint32_t platform = 0;
  bool actorTransform = false;
  bool actorVisible = true;
  CTransform4f actorLocal = CTransform4f::Identity();
  CVector3f platformStart = CVector3f::Zero();
  CVector3f dragged = CVector3f::Zero();
  // A DamageableTrigger's linked actor (kFollow; 0: none): the trigger's editor id, whether
  // it was last seen active, and the alpha it draws this instance at.
  uint32_t follow = 0;
  bool following = false;
  float alpha = 1.f;
  bool modulated = false;
  CModelFlags modulation = CModelFlags::Normal();
  size_t cluster = SIZE_MAX; // the Cluster it is drawn in, if any
  float scaleSq = 1.f; // the largest of its transform's axes, squared: model distances to world ones
  int level = -1;      // this frame's level of detail (Model::levels), -1 for the model itself
  bool glows = false;  // Instance::glow
  float glow[3] = {};
  // Instance::anim (empty: it stands still), the clip it is on and the area's animTime it
  // began at, Instance::animOnShow, whether it was shown at the last Think, and where it
  // stands before its pose, which `xf` adds each frame.
  std::vector< Instance::AnimClip > anim;
  size_t clip = 0;
  double clipStart = 0.;
  bool animOnShow = false;
  bool wasShown = false;
  CTransform4f base = CTransform4f::Identity();
  bool sky = false; // Instance::sky: drawn by Sky, not with the room
  bool worldSky = false; // Instance::worldSky
  float skyRadiance[3] = {};
  float lightmap[3] = {}; // Instance::lightmap
  // Instance::drivers[0]: a float driver of one kind parameter. `value` is the spline at the
  // area's clock, from `startAt` (infinite until it starts).
  bool driven = false;
  int driverSlot = 0;
  double startAt = 1e300;
  float driverValue = 0.f;
  PortMayaSpline driverSpline;
};

// A driven instance's driver starting: the delay after `sender` sends `state`.
struct DriverStart {
  uint32_t sender;
  uint8_t state;
  size_t item;
  float delay;
};

// Copies of one small model that never move or change, built into one model at their world
// places: a phone pays for each draw on every screen tile, and a room has hundreds of them.
struct Cluster {
  size_t model;
  std::vector< size_t > members; // items
  std::unique_ptr< CModel > merged;
  // After `merged`, so it is released before the model it points into is freed.
  ResidentKeep resident;
  CAABox bounds = CAABox::MakeMaxInvertedBox();
  std::unique_ptr< CActorLights > lights;
  bool areaLit = false;
  bool split = false; // this frame: some copy is hidden, so the copies are drawn one by one
  // The model's coarser levels merged the same way, once they are all in (BuildClusterLevels).
  struct MergedLevel {
    std::unique_ptr< CModel > merged;
    ResidentKeep resident; // after `merged`, as above
  };
  std::vector< MergedLevel > levels;
  float scaleSq = 1.f; // the largest of its members' (Placed::scaleSq)
};

// A script node now (see ScriptNode).
struct NodeState {
  struct TargetFlags {
    size_t item;
    bool modulated;
    float alpha;
    CModelFlags flags;
  };
  std::vector<TargetFlags> targetFlags;
  AlphaModulateState modulation;
  bool active = true;
  bool inside = false; // a camera volume: the camera was in it when last looked at
  uint32_t count = 0;  // a counter's value
};

// An instance shown or hidden when a script object sends a state.
struct Trigger {
  uint32_t sender; // editor id without the layer bits (TEditorId::Value)
  uint8_t state;
  uint8_t action;
  size_t item;
  float delay; // Link::delay
};

// A delayed trigger's action, due at the area's `clock`.
struct Pending {
  double due;
  size_t trigger;
};

struct Area {
  bool hasFile = false;
  bool skyOnly = false;  // a sky and nothing else: retail still draws the room
  std::vector< size_t > skyModels; // loaded with the area, not when first drawn (Sky)
  bool placed = false; // the instances have their world transforms
  std::vector< Instance > instances;
  std::vector< Model > models;
  std::vector< Placed > items;
  std::vector< const Placed* > sorted; // this frame's, with blended surfaces still to draw
  // The items grouped by model, the order Draw goes in: a model's instances one after another
  // keep its pipelines and textures bound, which is most of the cost of a draw on a phone.
  std::vector< size_t > drawOrder;
  std::vector< Cluster > clusters;
  bool clustered = false; // BuildClusters has run
  size_t levels = 0;       // of all its models
  size_t levelsLoaded = 0;
  bool clusterLevels = false; // BuildClusterLevels has run
  std::vector< Trigger > triggers;
  std::vector< DriverStart > driverStarts;
  bool gated = false; // some instance has a layer
  size_t loaded = 0;
  // Remastered's own script objects (Script), what each is now, the items of each group,
  // and each node's edges out.
  Script script;
  std::vector< NodeState > nodes;
  std::vector< std::vector< size_t > > groups;
  // What the script last made of each group (1 shown, 0 hidden; -1 or past the end: nothing
  // yet), which also covers a group with no instances, such as a sun's (GroupShown).
  std::vector< int8_t > groupShown;
  std::vector< std::vector< size_t > > edgesFrom;
  // Script::hidden, with the item of each instance (one whose model did not load is left out).
  std::vector< std::pair< uint32_t, size_t > > hides;
  bool retailEdges = false; // some edge starts at a retail object
  float camera[3] = {};      // where Think last saw the camera, in area space
  double animTime = 0.;      // seconds its animated instances have played (Think); double, so
                             // hours in still step evenly
  bool animated = false;     // some instance is
  double clock = 0.;         // seconds the area has been thought about (Think), for `pending`
  std::vector< Pending > pending;
};

// Areas in memory; one without a file has no instances. Never destroyed: the models' tokens
// must not outlive the game's resource pool, which static destruction would not respect.
std::unordered_map< uint32_t, Area >& Areas() {
  static auto* const areas = new std::unordered_map< uint32_t, Area >();
  return *areas;
}

int sMode = -1;
int sAreaLights = -1;
float sMinPixels = -1.f; // < 0 until MinPixels reads MP_ROOM_GEO_MIN_PX
bool sMergedDraws = true;
bool sFrontToBack = true;
bool sDepthPrepass = false;
bool sOffscreenCasters = true;
// Out-of-view models drawn caster-only, summed over the frame's areas (the last whole frame's in
// sLastCasterCount), for the console.
int sCasterFrame = -1;
uint32_t sCasterCount = 0;
uint32_t sLastCasterCount = 0;
float sLodDistance = -1.f; // < 0 until LodDistance reads MP_ROOM_GEO_LOD
bool sResident = false;
// The mods' level of detail tables, by model id; read when the first area loads after a Reset.
std::unordered_map< uint32_t, std::vector< LodLevel > > sLods;
bool sLodsRead = false;
int sDrawnCoarse = 0;
int sDrawnCoarseLast = 0;
// Counts the times areas left Areas(). The sorted pass hands DrawSorted only the item, so
// it draws while the areas are the ones they were when AddSorted queued it.
uint32_t sGeneration = 0;
uint32_t sQueuedGeneration = 0;
bool sQueued = false; // AddSorted has queued items since SetLoadedAreas
bool sQueueStale = false; // areas went between two AddSorted calls
bool sBuffersReady = false;
bool sWarned = false;
int sDrawn = 0;
int sDrawnLast = 0;
// Whether any area in memory has triggers, so the script hook costs nothing otherwise.
bool sTriggers = false;

// The console's material values, by model id; CCubeModel holds them by model, which is
// only good while the model is in memory, so they are handed over again every frame.
struct MaterialValue {
  uint32_t id;
  int material;
  int field;
  float value;
};
std::vector< MaterialValue > sMaterialValues;

const CCubeModel* CubeModel(const Model& model) {
  return model.loaded ? (**model.data->PickStaticModel(CModelData::kWM_Normal)).GetCubeModel() : nullptr;
}

// A loaded model's arrays and its surfaces' display lists, as SetArraysCurrent and
// CCubeSurface::CallDisplayList hand them to GX.
void KeepResident(ResidentKeep& resident, const CCubeModel* cube) {
  if (!sResident || cube == nullptr) {
    return;
  }
  const CCubeModel::ModelInstance& instance = cube->GetModelInstance();
  resident.Keep(instance.GetVertexPointer(), instance.GetVertexSize());
  resident.Keep(instance.GetNormalPointer(), instance.GetNormalSize());
  resident.Keep(instance.GetColorPointer(), instance.GetColorSize());
  resident.Keep(instance.GetTCPointer(), instance.GetTCSize());
  resident.Keep(instance.GetPackedTCPointer(), instance.GetPackedTCSize());
  for (const CCubeSurface* first : {&cube->GetNormalSurfaces(), &cube->GetAlphaSurfaces()}) {
    for (CCubeSurface surface = *first; surface.IsValid(); surface = surface.GetNextSurface()) {
      resident.Keep(surface.GetDisplayList(), surface.GetDisplayListSize());
    }
  }
}

void KeepResident(Model& model) { KeepResident(model.resident, CubeModel(model)); }

// Clusters (see Cluster). A model is merged once an area places it this many times, while
// a copy has at most kMergeVertices positions; copies share a cluster within one cell of a
// kMergeCell grid, and with it one set of lights.
const size_t kMergeCopies = 4;
const size_t kMergeVertices = 4096;
const float kMergeCell = 64.f;

using port::ReadBEFloat;
using port::ReadBE32;
using port::WriteBE32;
using port::WriteBEFloat;

// A CMDL's sections, as CModel::CModel reads them.
struct CmdlSections {
  uint32_t version = 0;
  uint32_t flags = 0;
  size_t matSets = 1;
  std::vector< std::pair< size_t, size_t > > sections; // offset, size
  size_t positions = 0; // the index of each array section
  size_t surfaces = 0;  // of the first surface
  size_t surfaceCount = 0;
};

bool ReadCmdl(const std::vector< uint8_t >& data, CmdlSections& out) {
  if (data.size() < 0x2c || ReadBE32(data.data()) != 0xdeadbabe) {
    return false;
  }
  out.version = ReadBE32(&data[4]);
  out.flags = ReadBE32(&data[8]);
  const size_t count = ReadBE32(&data[0x24]);
  const size_t sizesAt = out.version == 1 ? 0x28 : 0x2c;
  out.matSets = out.version >= 2 ? ReadBE32(&data[0x28]) : 1;
  if (sizesAt + count * 4 > data.size()) {
    return false;
  }
  size_t at = (sizesAt + count * 4 + 31) & ~size_t(31);
  for (size_t i = 0; i < count; ++i) {
    const size_t size = ReadBE32(&data[sizesAt + i * 4]);
    if (at + size > data.size()) {
      return false;
    }
    out.sections.emplace_back(at, size);
    at += size;
  }
  out.positions = out.matSets;
  const size_t info = out.positions + 4 + ((out.flags >> 2) & 1);
  if (info >= count || out.sections[info].second < 4) {
    return false;
  }
  out.surfaces = info + 1;
  out.surfaceCount = ReadBE32(&data[out.sections[info].first]);
  return out.surfaces + out.surfaceCount <= count;
}

// The copies of `cube`'s CMDL `source` at `places`, as one CMDL in world space: the
// positions and normals once per copy, the other arrays shared, and each surface's display
// list once per copy with the copy's position and normal indices. Empty when the model has
// what this does not handle (a list with more than primitives in it, direct attributes).
std::vector< uint8_t > MergeCmdl(const std::vector< uint8_t >& source, const CCubeModel& cube,
                                 const std::vector< const CTransform4f* >& places, const CAABox& bounds) {
  CmdlSections in;
  if (!ReadCmdl(source, in)) {
    return {};
  }
  const size_t copies = places.size();
  const auto [positionsAt, positionsSize] = in.sections[in.positions];
  const auto [normalsAt, normalsSize] = in.sections[in.positions + 1];
  const size_t positionCount = positionsSize / 12;

  // Each surface's list, read once: the vertex layout from its material, the primitives'
  // vertex format (which tells the normals' size: VTXFMT0 floats, 1 and 2 shorts, 3 the NBT
  // triples of floats).
  struct Surface {
    size_t stride = 0;
    size_t normalAt = 0; // in a vertex; SIZE_MAX without normals
    uint32_t bases[2] = {}; // PBIX position and normal bases
  };
  std::vector< Surface > surfaces(in.surfaceCount);
  int format = -1;
  for (size_t s = 0; s < in.surfaceCount; ++s) {
    const auto [at, size] = in.sections[in.surfaces + s];
    if (size < 0x40) {
      return {};
    }
    const uint8_t* const header = &source[at];
    const uint32_t extra = ReadBE32(header + 0x1c);
    if (extra > size) {
      return {};
    }
    const size_t headerSize = (0x4b + size_t(extra)) & ~size_t(31);
    const size_t listSize = ReadBE32(header + 0x10) & 0x7fffffff;
    if (headerSize + listSize > size) {
      return {};
    }
    const CCubeMaterial material = cube.GetMaterialByIndex(int(ReadBE32(header + 0xc)));
    if (material.PortNeedsModelMatrix()) {
      return {};
    }
    const uint desc = material.GetVertexDesc();
    Surface& surface = surfaces[s];
    surface.normalAt = SIZE_MAX;
    for (int a = 0; a < 11; ++a) {
      const uint type = (desc >> (a * 2)) & 3;
      if (type == GX_DIRECT || (a < 2 && type == GX_INDEX8)) {
        return {};
      }
      if (a == 1 && type != GX_NONE) {
        surface.normalAt = surface.stride;
      }
      surface.stride += type == GX_INDEX16 ? 2 : type == GX_INDEX8 ? 1 : 0;
    }
    if ((desc & 3) != GX_INDEX16) {
      return {};
    }
    if (extra >= 0x40 && ReadBE32(header + 0x44) == 0x50424958) { // 'PBIX'
      surface.bases[0] = ReadBE32(header + 0x48);
      surface.bases[1] = ReadBE32(header + 0x4c);
    }
    const uint8_t* const list = header + headerSize;
    for (size_t i = 0; i < listSize;) {
      const uint8_t op = list[i];
      if (op == 0) {
        ++i;
        continue;
      }
      if ((op & 0x80) == 0 || i + 3 > listSize) {
        return {};
      }
      // VTXFMT4 (normals of 15 floats) shares its first nine with VTXFMT3: a model's surfaces
      // may mix them, the entry size being the model's.
      const int fmt = (op & 7) == 4 ? 3 : (op & 7);
      if (format >= 0 && format != fmt) {
        return {};
      }
      format = fmt;
      i += 3 + size_t(list[i + 1] << 8 | list[i + 2]) * surface.stride;
      if (i > listSize) {
        return {};
      }
    }
  }
  const bool nbt = format == 3;
  const size_t frames = nbt && (in.flags & 0x10) != 0 ? 2 : 1; // tangent frames per entry
  const size_t normalSize = nbt ? 12 + 24 * frames : format > 0 ? 6 : 12;
  const size_t normalCount = normalsSize / normalSize;
  if (std::max(positionCount, normalCount) * copies > 0x10000) {
    return {};
  }

  // The new sections' sizes, 32-byte aligned as the converter writes them.
  const auto aligned = [](size_t n) { return (n + 31) & ~size_t(31); };
  std::vector< size_t > sizes(in.sections.size());
  for (size_t i = 0; i < sizes.size(); ++i) {
    sizes[i] = in.sections[i].second;
  }
  sizes[in.positions] = aligned(positionCount * 12 * copies);
  sizes[in.positions + 1] = aligned(normalCount * normalSize * copies);
  std::vector< std::vector< uint8_t > > lists(in.surfaceCount);
  for (size_t s = 0; s < in.surfaceCount; ++s) {
    const uint8_t* const header = &source[in.sections[in.surfaces + s].first];
    const size_t headerSize = (0x4b + ReadBE32(header + 0x1c)) & ~31u;
    const size_t listSize = ReadBE32(header + 0x10) & 0x7fffffff;
    const uint8_t* const list = header + headerSize;
    const Surface& surface = surfaces[s];
    std::vector< uint8_t >& out = lists[s];
    for (size_t copy = 0; copy < copies; ++copy) {
      for (size_t i = 0; i < listSize;) {
        if (list[i] == 0) {
          ++i;
          continue;
        }
        const size_t count = size_t(list[i + 1] << 8 | list[i + 2]);
        out.insert(out.end(), list + i, list + i + 3);
        i += 3;
        for (size_t v = 0; v < count; ++v, i += surface.stride) {
          const size_t start = out.size();
          out.insert(out.end(), list + i, list + i + surface.stride);
          const auto move = [&](size_t at, uint32_t base, size_t perCopy) {
            const uint32_t index = base + (uint32_t(out[start + at]) << 8 | out[start + at + 1]) + uint32_t(copy * perCopy);
            out[start + at] = uint8_t(index >> 8);
            out[start + at + 1] = uint8_t(index);
            return index < 0x10000;
          };
          if (!move(0, surface.bases[0], positionCount) ||
              (surface.normalAt != SIZE_MAX && !move(surface.normalAt, surface.bases[1], normalCount))) {
            return {};
          }
        }
      }
    }
    out.resize(aligned(out.size()), 0);
    sizes[in.surfaces + s] = headerSize + out.size();
  }

  const size_t sizesAt = in.version == 1 ? 0x28 : 0x2c;
  size_t total = aligned(sizesAt + sizes.size() * 4);
  for (const size_t size : sizes) {
    total += size;
  }
  std::vector< uint8_t > out(total, 0);
  std::memcpy(out.data(), source.data(), sizesAt);
  const CVector3f lo = bounds.GetMinPoint();
  const CVector3f hi = bounds.GetMaxPoint();
  const float box[6] = {lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ()};
  for (int i = 0; i < 6; ++i) {
    WriteBEFloat(&out[0xc + i * 4], box[i]);
  }
  size_t at = aligned(sizesAt + sizes.size() * 4);
  for (size_t i = 0; i < sizes.size(); ++i) {
    WriteBE32(&out[sizesAt + i * 4], uint32_t(sizes[i]));
    uint8_t* const dst = &out[at];
    const uint8_t* const src = &source[in.sections[i].first];
    if (i == in.positions) {
      for (size_t copy = 0; copy < copies; ++copy) {
        const CTransform4f& xf = *places[copy];
        for (size_t v = 0; v < positionCount; ++v) {
          const uint8_t* const p = src + v * 12;
          const CVector3f world = xf * CVector3f(ReadBEFloat(p), ReadBEFloat(p + 4), ReadBEFloat(p + 8));
          uint8_t* const q = dst + (copy * positionCount + v) * 12;
          WriteBEFloat(q, world.GetX());
          WriteBEFloat(q + 4, world.GetY());
          WriteBEFloat(q + 8, world.GetZ());
        }
      }
    } else if (i == in.positions + 1) {
      for (size_t copy = 0; copy < copies; ++copy) {
        // Normals go by the inverse transpose, which the cofactors are up to a scale.
        const CTransform4f& m = *places[copy];
        const float c[9] = {
            m.Get11() * m.Get22() - m.Get12() * m.Get21(), m.Get12() * m.Get20() - m.Get10() * m.Get22(),
            m.Get10() * m.Get21() - m.Get11() * m.Get20(), m.Get02() * m.Get21() - m.Get01() * m.Get22(),
            m.Get00() * m.Get22() - m.Get02() * m.Get20(), m.Get01() * m.Get20() - m.Get00() * m.Get21(),
            m.Get01() * m.Get12() - m.Get02() * m.Get11(), m.Get02() * m.Get10() - m.Get00() * m.Get12(),
            m.Get00() * m.Get11() - m.Get01() * m.Get10()};
        // The cofactors are the inverse transpose times the determinant: a mirrored copy's
        // would point its normals inward.
        const float sign = m.Get00() * c[0] + m.Get01() * c[1] + m.Get02() * c[2] < 0.f ? -1.f : 1.f;
        for (size_t v = 0; v < normalCount && nbt; ++v) {
          // N, B, T (then B1, T1): the normal as below, each tangent by the matrix itself and
          // each bitangent rebuilt from the handedness (B = w * cross(N, T)), which a mirror flips.
          const uint8_t* const p = src + v * normalSize;
          uint8_t* const q = dst + (copy * normalCount + v) * normalSize;
          float n[3], wn[3];
          for (int k = 0; k < 3; ++k) {
            n[k] = ReadBEFloat(p + k * 4);
          }
          for (int r = 0; r < 3; ++r) {
            wn[r] = c[r * 3] * n[0] + c[r * 3 + 1] * n[1] + c[r * 3 + 2] * n[2];
          }
          const float nLength = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
          for (int k = 0; k < 3; ++k) {
            wn[k] = nLength > 0.f ? sign * wn[k] / nLength : 0.f;
            WriteBEFloat(q + k * 4, wn[k]);
          }
          for (size_t f = 0; f < frames; ++f) {
            const size_t bAt = 12 + 24 * f, tAt = 24 + 24 * f;
            float b[3], t[3];
            for (int k = 0; k < 3; ++k) {
              b[k] = ReadBEFloat(p + bAt + k * 4);
              t[k] = ReadBEFloat(p + tAt + k * 4);
            }
            const float handed = (n[1] * t[2] - n[2] * t[1]) * b[0] + (n[2] * t[0] - n[0] * t[2]) * b[1] +
                                         (n[0] * t[1] - n[1] * t[0]) * b[2] < 0.f ? -sign : sign;
            float wt[3];
            const CVector3f moved = *places[copy] * CVector3f(t[0], t[1], t[2]) -
                                    *places[copy] * CVector3f(0.f, 0.f, 0.f);
            wt[0] = moved.GetX();
            wt[1] = moved.GetY();
            wt[2] = moved.GetZ();
            const float tLength = std::sqrt(wt[0] * wt[0] + wt[1] * wt[1] + wt[2] * wt[2]);
            for (int k = 0; k < 3; ++k) {
              wt[k] = tLength > 0.f ? wt[k] / tLength : 0.f;
            }
            const float wb[3] = {(wn[1] * wt[2] - wn[2] * wt[1]) * handed, (wn[2] * wt[0] - wn[0] * wt[2]) * handed,
                                 (wn[0] * wt[1] - wn[1] * wt[0]) * handed};
            for (int k = 0; k < 3; ++k) {
              WriteBEFloat(q + bAt + k * 4, wb[k]);
              WriteBEFloat(q + tAt + k * 4, wt[k]);
            }
          }
        }
        for (size_t v = 0; v < normalCount && !nbt; ++v) {
          const uint8_t* const p = src + v * normalSize;
          float n[3];
          for (int k = 0; k < 3; ++k) {
            n[k] = format > 0 ? float(int16_t(p[k * 2] << 8 | p[k * 2 + 1])) / 16384.f : ReadBEFloat(p + k * 4);
          }
          // Row r of the world matrix's cofactors dotted with the normal.
          float w[3];
          for (int r = 0; r < 3; ++r) {
            w[r] = c[r * 3] * n[0] + c[r * 3 + 1] * n[1] + c[r * 3 + 2] * n[2];
          }
          const float length = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
          uint8_t* const q = dst + (copy * normalCount + v) * normalSize;
          for (int k = 0; k < 3; ++k) {
            const float value = length > 0.f ? sign * w[k] / length : 0.f;
            if (format > 0) {
              const int16_t s = int16_t(std::clamp(std::lround(value * 16384.f), -32768l, 32767l));
              q[k * 2] = uint8_t(uint16_t(s) >> 8);
              q[k * 2 + 1] = uint8_t(s);
            } else {
              WriteBEFloat(q + k * 4, value);
            }
          }
        }
      }
    } else if (i >= in.surfaces && i < in.surfaces + in.surfaceCount) {
      const size_t s = i - in.surfaces;
      const uint32_t extra = ReadBE32(src + 0x1c);
      const size_t headerSize = (0x4b + size_t(extra)) & ~size_t(31);
      std::memcpy(dst, src, headerSize);
      const CVector3f centre = bounds.GetCenterPoint();
      WriteBEFloat(dst, centre.GetX());
      WriteBEFloat(dst + 4, centre.GetY());
      WriteBEFloat(dst + 8, centre.GetZ());
      WriteBE32(dst + 0x10, (ReadBE32(src + 0x10) & 0x80000000) | uint32_t(lists[s].size()));
      if (extra != 0) {
        for (int k = 0; k < 6; ++k) {
          WriteBEFloat(dst + 0x2c + k * 4, box[k]);
        }
      }
      if (surfaces[s].bases[0] != 0 || surfaces[s].bases[1] != 0) {
        WriteBE32(dst + 0x48, 0); // folded into the indices
        WriteBE32(dst + 0x4c, 0);
      }
      std::memcpy(dst + headerSize, lists[s].data(), lists[s].size());
    } else {
      std::memcpy(dst, src, in.sections[i].second);
    }
    at += sizes[i];
  }
  return out;
}

// A CMDL's bytes as stored, empty unless it is there uncompressed (as MergeCmdl reads it).
std::vector< uint8_t > ReadCmdlFile(uint32_t id) {
  std::vector< uint8_t > bytes;
  const SObjectTag tag('CMDL', static_cast< CAssetId >(id));
  CResLoader& loader = gpResourceFactory->GetResLoader();
  if (loader.ResourceExists(tag) &&
      loader.GetResourceCompression(tag) == CResLoader::kCompressionType_Uncompressed) {
    char* data = nullptr;
    int length = 0;
    loader.LoadMemResourceSync(tag, &data, &length);
    if (data != nullptr && length > 0) {
      bytes.assign(reinterpret_cast< uint8_t* >(data), reinterpret_cast< uint8_t* >(data) + length);
    }
    delete[] data;
  }
  return bytes;
}

// The area's clusters, once all its models are in. Only what nothing moves, hides or fades
// goes in one (see Draw for what can still leave a cluster's copies drawn one by one).
void BuildClusters(Area& area) {
  area.clustered = true;
  if (!port::EnvFlag("MP_ROOM_GEO_MERGE", true)) {
    return; // to compare against
  }
  std::vector< size_t > copies(area.models.size(), 0);
  const auto mergeable = [&](const Placed& item) {
    return item.layer == kEveryLayer && item.platform == 0 && item.follow == 0 && item.active && !item.glows &&
           item.anim.empty() && !item.sky && !item.driven;
  };
  std::vector< bool > linked(area.items.size(), false);
  for (const Trigger& trigger : area.triggers) {
    linked[trigger.item] = true;
  }
  for (const std::vector< size_t >& group : area.groups) {
    for (const size_t i : group) {
      linked[i] = true;
    }
  }
  for (size_t i = 0; i < area.items.size(); ++i) {
    if (!linked[i] && mergeable(area.items[i])) {
      ++copies[area.items[i].model];
    }
  }
  std::unordered_map< uint64_t, std::vector< size_t > > cells; // by model and cell
  std::vector< uint64_t > order;
  for (size_t i = 0; i < area.items.size(); ++i) {
    const Placed& item = area.items[i];
    const Model& model = area.models[item.model];
    if (linked[i] || !mergeable(item) || copies[item.model] < kMergeCopies || !model.loaded) {
      continue;
    }
    const CVector3f at = item.xf.GetTranslation();
    const auto cell = [](float v) {
      return uint64_t(uint16_t(int16_t(std::clamp(std::floor(v / kMergeCell), -32768.f, 32767.f))));
    };
    const uint64_t key = uint64_t(item.model) << 48 | cell(at.GetX()) << 32 | cell(at.GetY()) << 16 | cell(at.GetZ());
    std::vector< size_t >& members = cells[key];
    if (members.empty()) {
      order.push_back(key);
    }
    members.push_back(i);
  }
  std::unordered_map< size_t, std::vector< uint8_t > > sources;
  std::unordered_map< size_t, size_t > perCluster; // copies a cluster of the model holds
  size_t built = 0;
  size_t merged = 0;
  for (const uint64_t key : order) {
    const std::vector< size_t >& members = cells[key];
    if (members.size() < 2) {
      continue;
    }
    const size_t index = area.items[members.front()].model;
    const Model& model = area.models[index];
    const CModel& cmodel = **model.data->PickStaticModel(CModelData::kWM_Normal);
    const CCubeModel* const cube = cmodel.GetCubeModel();
    if (!perCluster.count(index)) {
      bool ok = cmodel.IsDefinitelyOpaque();
      if (ok) {
        sources[index] = ReadCmdlFile(model.id);
      }
      CmdlSections layout;
      ok = ok && ReadCmdl(sources[index], layout);
      // As many copies as 16-bit indices reach; normals counted at their smallest, shorts.
      const size_t vertices = ok ? std::max(layout.sections[layout.positions].second / 12,
                                            layout.sections[layout.positions + 1].second / 6) : 0;
      ok = ok && vertices <= kMergeVertices;
      perCluster[index] = ok ? 0x10000 / std::max< size_t >(vertices, 1) : 0;
    }
    if (perCluster[index] < 2) {
      continue;
    }
    for (size_t first = 0; first < members.size(); first += perCluster[index]) {
      const size_t last = std::min(members.size(), first + perCluster[index]);
      if (last - first < 2) {
        break;
      }
      Cluster cluster;
      cluster.model = index;
      std::vector< const CTransform4f* > places;
      for (size_t m = first; m < last; ++m) {
        Placed& item = area.items[members[m]];
        cluster.members.push_back(members[m]);
        places.push_back(&item.xf);
        item.bounds = model.bounds.GetTransformedAABox(item.xf); // for roomgeo at and pick
        item.bounded = true;
        cluster.bounds.Include(item.bounds);
        cluster.scaleSq = std::max(cluster.scaleSq, item.scaleSq);
      }
      std::vector< uint8_t > bytes = MergeCmdl(sources[index], *cube, places, cluster.bounds);
      if (bytes.empty()) {
        perCluster[index] = 0;
        break;
      }
      rstl::auto_ptr< uchar[] > data(rs_new uchar[bytes.size()]);
      std::memcpy(data.get(), bytes.data(), bytes.size());
      cluster.merged.reset(rs_new CModel(data, int(bytes.size()), *gpSimplePool));
      const_cast< CCubeModel* >(cluster.merged->GetCubeModel())->PortSetAssetId(model.id);
      cluster.merged->Touch(0);
      KeepResident(cluster.resident, cluster.merged->GetCubeModel());
      for (const size_t i : cluster.members) {
        area.items[i].cluster = area.clusters.size();
      }
      merged += cluster.members.size();
      ++built;
      area.clusters.push_back(std::move(cluster));
    }
  }
  if (built != 0) {
    PortLog::Write("room geo: %zu copies merged into %zu models\n", merged, built);
  }
}

// Each cluster's coarser levels, once the area's are all in: a cluster spans a whole cell,
// and without them a far one is drawn at full detail. A level that cannot be merged ends
// the cluster's list there, as the levels must run without gaps.
void BuildClusterLevels(Area& area) {
  area.clusterLevels = true;
  std::unordered_map< uint32_t, std::vector< uint8_t > > sources;
  size_t built = 0;
  for (Cluster& cluster : area.clusters) {
    const Model& model = area.models[cluster.model];
    std::vector< const CTransform4f* > places;
    for (const size_t i : cluster.members) {
      places.push_back(&area.items[i].xf);
    }
    for (const Level& level : model.levels) {
      if (!level.loaded) {
        break;
      }
      const CModel& cmodel = **level.data->PickStaticModel(CModelData::kWM_Normal);
      if (!cmodel.IsDefinitelyOpaque()) {
        break;
      }
      if (!sources.count(level.id)) {
        sources[level.id] = ReadCmdlFile(level.id);
      }
      std::vector< uint8_t > bytes = MergeCmdl(sources[level.id], *cmodel.GetCubeModel(), places, cluster.bounds);
      if (bytes.empty()) {
        break;
      }
      rstl::auto_ptr< uchar[] > data(rs_new uchar[bytes.size()]);
      std::memcpy(data.get(), bytes.data(), bytes.size());
      Cluster::MergedLevel merged;
      merged.merged.reset(rs_new CModel(data, int(bytes.size()), *gpSimplePool));
      const_cast< CCubeModel* >(merged.merged->GetCubeModel())->PortSetAssetId(level.id);
      merged.merged->Touch(0);
      KeepResident(merged.resident, merged.merged->GetCubeModel());
      cluster.levels.push_back(std::move(merged));
      ++built;
    }
  }
  if (built != 0) {
    PortLog::Write("room geo: %zu coarser merged models\n", built);
  }
}

void BindMaterialValues() {
  CCubeModel::PortClearPBROverrides();
  for (const MaterialValue& value : sMaterialValues) {
    bool area = false;
    for (const auto& [mrea, a] : Areas()) {
      for (const Model& model : a.models) {
        if (model.id == value.id) {
          CCubeModel::PortOverridePBR(CubeModel(model), value.material, value.field, value.value);
          area = true;
        }
      }
    }
    if (!area) {
      // Not room geometry: whichever model of that id has drawn lately (needs the draw log).
      CCubeModel::PortOverridePBR(CCubeModel::PortFindModel(value.id), value.material, value.field, value.value);
    }
  }
}

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
using port::ReadAll;

// The seconds a clip takes to play once, first frame to last.
float ClipLength(const Instance::AnimClip& clip) {
  return float(clip.keys.size() / 7 - 1) / clip.fps;
}

// A clip's pose `seconds` in: the frames either side blended, the turn the shorter way
// round. A looping clip comes back from the last frame to the first; any other holds the
// last.
CTransform4f Pose(const Instance::AnimClip& clip, double seconds) {
  const size_t frames = clip.keys.size() / 7;
  const float length = ClipLength(clip);
  float t;
  if (clip.loop) {
    t = float(std::fmod(seconds, double(length)));
    t = (t < 0.f ? t + length : t) * clip.fps;
  } else {
    t = float(std::clamp(seconds, 0., double(length))) * clip.fps;
  }
  const size_t i = std::min(size_t(t), frames - 2);
  const float w = std::clamp(t - float(i), 0.f, 1.f);
  const float* const a = clip.keys.data() + 7 * i;
  const float* const b = a + 7;
  const float sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.f ? -1.f : 1.f;
  float q[4];
  float n = 0.f;
  for (int k = 0; k < 4; ++k) {
    q[k] = a[k] + (sign * b[k] - a[k]) * w;
    n += q[k] * q[k];
  }
  n = n > 0.f ? 1.f / std::sqrt(n) : 0.f;
  const float x = q[0] * n, y = q[1] * n, z = q[2] * n, s = q[3] * n;
  float r[9] = {1.f - 2.f * (y * y + z * z), 2.f * (x * y - s * z), 2.f * (x * z + s * y),
                2.f * (x * y + s * z), 1.f - 2.f * (x * x + z * z), 2.f * (y * z - s * x),
                2.f * (x * z - s * y), 2.f * (y * z + s * x), 1.f - 2.f * (x * x + y * y)};
  if (clip.stretch.size() == frames * 9) {
    // A scaling joint: the pose is rotation * stretch, the stretch blended frame to frame.
    const float* const ua = clip.stretch.data() + 9 * i;
    float u[9], m[9];
    for (int k = 0; k < 9; ++k) {
      u[k] = ua[k] + (ua[k + 9] - ua[k]) * w;
    }
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        m[3 * row + col] = r[3 * row] * u[col] + r[3 * row + 1] * u[3 + col] + r[3 * row + 2] * u[6 + col];
      }
    }
    std::copy(m, m + 9, r);
  }
  return CTransform4f(r[0], r[1], r[2], a[4] + (b[4] - a[4]) * w, r[3], r[4], r[5], a[5] + (b[5] - a[5]) * w, r[6],
                      r[7], r[8], a[6] + (b[6] - a[6]) * w);
}

void RegisterNodes(Area& area);

// The script objects as the file has them: a counter at 0, the camera in no volume.
void ResetNodes(Area& area) {
  area.nodes.assign(area.script.nodes.size(), NodeState());
  area.groupShown.clear();
  for (size_t i = 0; i < area.nodes.size(); ++i) {
    area.nodes[i].active = area.script.nodes[i].active;
  }
  RegisterNodes(area);
  // CacheTargetFlags (0xd5f1bc), at InitializedInArea: each modulator owns
  // a copy of the actual target state, not an inferred Normal endpoint.
  for (const ScriptEdge& edge : area.script.edges) {
    if (edge.retail || edge.action != kGroupModulate || edge.from >= area.nodes.size() ||
        edge.to >= area.groups.size()) continue;
    for (size_t i : area.groups[edge.to]) {
      const Placed& item = area.items[i];
      area.nodes[edge.from].targetFlags.push_back({i, item.modulated, item.alpha, item.modulation});
    }
  }
}

void ReadLods() {
  sLodsRead = true;
  sLods.clear();
  for (const std::string& path : PortMods::RoomLodPaths()) {
    std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
    const std::vector< uint8_t > data = ReadAll(in);
    std::vector< Lods > table;
    std::string error;
    if (!in || !ParseLods(data, table, error)) {
      PortLog::Write("room geo: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
      continue;
    }
    for (Lods& lods : table) {
      sLods[lods.model] = std::move(lods.levels);
    }
  }
}

// An area's file, read and parsed: all of the load that needs nothing of the game's, so
// it can run on a worker while the game streams the area in (Prefetch).
struct Parsed {
  bool ok = false;
  std::string error;
  std::vector< Instance > instances;
  Script script;
};

Parsed ReadAndParse(const std::string& path) {
  Parsed parsed;
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector< uint8_t > data = ReadAll(in);
  parsed.ok = in && Parse(data, parsed.instances, parsed.error, &parsed.script);
  return parsed;
}

// Prefetches by MREA, with the path each was read from: the main thread only.
struct Prefetched {
  std::string path;
  std::future< Parsed > parsed;
};
std::unordered_map< uint32_t, Prefetched > sPrefetched;
constexpr size_t kMaxPrefetched = 16;

void Load(uint32_t mrea, Area& area) {
  const std::string path = PortMods::RoomGeoPath(mrea);
  if (path.empty() || gpResourceFactory == nullptr) {
    return;
  }
  if (!sLodsRead) {
    ReadLods();
  }
  Parsed parsed;
  const auto prefetched = sPrefetched.find(mrea);
  if (prefetched != sPrefetched.end() && prefetched->second.path == path && prefetched->second.parsed.valid()) {
    parsed = prefetched->second.parsed.get(); // waits only if the worker is not done yet
  } else {
    parsed = ReadAndParse(path);
  }
  if (prefetched != sPrefetched.end()) {
    sPrefetched.erase(prefetched);
  }
  if (!parsed.ok) {
    PortLog::Write("room geo: %s: %s\n", path.c_str(), parsed.error.empty() ? "cannot read" : parsed.error.c_str());
    area.instances.clear();
    area.script = Script();
    return;
  }
  area.instances = std::move(parsed.instances);
  area.script = std::move(parsed.script);
  // A model the import could not convert leaves its instances behind.
  std::unordered_map< uint32_t, size_t > index;
  size_t missing = 0;
  std::vector< size_t > itemOf(area.instances.size(), size_t(-1));
  for (const Instance& instance : area.instances) {
    auto found = index.find(instance.model);
    if (found == index.end()) {
      size_t slot = size_t(-1);
      if (gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(instance.model)) == 'CMDL') {
        slot = area.models.size();
        Model& model = area.models.emplace_back();
        model.id = instance.model;
        model.data.reset(new CModelData(
            CStaticRes(static_cast< CAssetId >(instance.model), CVector3f(1.f, 1.f, 1.f))));
        const auto lods = sLods.find(instance.model);
        if (lods != sLods.end()) {
          for (const LodLevel& lod : lods->second) {
            if (gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(lod.model)) != 'CMDL') {
              break; // a coarser one would be drawn where this one should
            }
            Level& level = model.levels.emplace_back();
            level.id = lod.model;
            level.distanceSq = lod.distanceSq;
            level.data.reset(
                new CModelData(CStaticRes(static_cast< CAssetId >(lod.model), CVector3f(1.f, 1.f, 1.f))));
          }
          area.levels += model.levels.size();
        }
      }
      found = index.emplace(instance.model, slot).first;
    }
    if (found->second == size_t(-1)) {
      ++missing;
      continue;
    }
    itemOf[size_t(&instance - area.instances.data())] = area.items.size();
    Placed& item = area.items.emplace_back();
    item.model = found->second;
    const float* const m = instance.transform;
    item.xf = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    for (int axis = 0; axis < 3; ++axis) {
      const float sq = m[axis] * m[axis] + m[4 + axis] * m[4 + axis] + m[8 + axis] * m[8 + axis];
      item.scaleSq = axis == 0 ? sq : std::max(item.scaleSq, sq);
    }
    item.scaleSq = std::max(item.scaleSq, 1e-6f);
    item.layer = instance.layer;
    item.shown = item.active = instance.active;
    item.glows = instance.glows;
    item.sky = instance.sky;
    item.worldSky = instance.sky && instance.worldSky;
    std::copy(instance.skyRadiance, instance.skyRadiance + 3, item.skyRadiance);
    std::copy(instance.glow, instance.glow + 3, item.glow);
    std::copy(instance.lightmap, instance.lightmap + 3, item.lightmap);
    item.wasShown = item.shown;
    if (!instance.anim.empty()) {
      item.anim = instance.anim;
      item.animOnShow = instance.animOnShow;
      area.animated = true;
    }
    item.platform = instance.platform;
    item.platformStart =
        CVector3f(instance.platformStart[0], instance.platformStart[1], instance.platformStart[2]);
    area.gated = area.gated || item.layer != kEveryLayer;
    if (instance.group != kNoGroup) {
      if (area.groups.size() <= instance.group) {
        area.groups.resize(size_t(instance.group) + 1);
      }
      area.groups[instance.group].push_back(area.items.size() - 1);
    }
    if (!instance.drivers.empty()) {
      const Instance::Driver& driver = instance.drivers[0];
      if (item.driverSpline.Load(driver.spline.data(), driver.spline.size())) {
        item.driven = true;
        item.driverSlot = driver.slot;
        item.driverValue = item.driverSpline.Eval(item.driverSpline.FirstTime());
        area.driverStarts.push_back({driver.sender & 0x3ffffff, driver.state, area.items.size() - 1, driver.delay});
      }
    }
    for (const Link& link : instance.links) {
      if (link.action == kActorTransform) {
        item.platform = link.sender;
        item.actorTransform = true;
        item.actorLocal = item.xf;
        continue;
      }
      if (link.action == kFollow) {
        // GetIdForScript wants the layer bits too.
        item.follow = link.sender;
        continue;
      }
      area.triggers.push_back({link.sender & 0x3ffffff, link.state, link.action, area.items.size() - 1, link.delay});
    }
  }
  for (Placed& item : area.items) {
    item.owner = &area;
  }
  area.hides.clear();
  for (const Script::Hidden& hidden : area.script.hidden) {
    if (itemOf[hidden.instance] != size_t(-1)) {
      area.hides.emplace_back(hidden.editorId, itemOf[hidden.instance]);
    }
  }
  area.edgesFrom.assign(area.script.nodes.size(), {});
  for (size_t i = 0; i < area.script.edges.size(); ++i) {
    const ScriptEdge& edge = area.script.edges[i];
    if (edge.retail) {
      area.retailEdges = true;
    } else {
      area.edgesFrom[edge.from].push_back(i);
    }
  }
  ResetNodes(area);
  sTriggers = sTriggers || !area.triggers.empty() || !area.driverStarts.empty() || area.retailEdges;
  area.instances.clear();
  area.instances.shrink_to_fit();
  area.hasFile = !area.items.empty();
  area.skyOnly = std::all_of(area.items.begin(), area.items.end(), [](const Placed& item) { return item.sky; });
  for (const Placed& item : area.items) {
    if (item.sky && std::find(area.skyModels.begin(), area.skyModels.end(), item.model) == area.skyModels.end()) {
      area.skyModels.push_back(item.model);
    }
  }
  PortLog::Write("room geo: %08X: %zu instance(s) of %zu model(s), %zu without a model, %zu trigger(s), %zu script "
                 "node(s), %zu edge(s), %zu group(s)\n",
                 mrea, area.items.size(), area.models.size(), missing, area.triggers.size(),
                 area.script.nodes.size(), area.script.edges.size(), area.groups.size());
}

void Apply(Area& area, const ScriptEdge& edge, int depth);

void Act(uint32_t mrea, Area& area, const Trigger& trigger) {
  Placed& item = area.items[trigger.item];
  const bool shown = trigger.action == kShow ? true : trigger.action == kHide ? false : !item.shown;
  if (shown != item.shown) {
    char line[96];
    std::snprintf(line, sizeof(line), "room geo: %08X: instance %u %s by %08X\n", mrea, unsigned(trigger.item),
                  shown ? "shown" : "hidden", trigger.sender);
    PortLog::Write(line);
  }
  item.shown = shown;
}

// Edges one outside event may set off. A loop that fans out would otherwise take
// exponential time before the depth limit stops it.
constexpr int kApplyBudget = 4096;
int sApplyBudget = kApplyBudget;
bool sBudgetLogged = false;

// A node sends an event: every edge out of it with that event acts, in the file's order,
// each before the next as retail's SendScriptMsgs does.
void Send(Area& area, uint32_t node, uint8_t event, int depth) {
  // Remastered's objects can be wired in a loop; retail's would recurse until the stack
  // ran out, which no room depends on.
  if (depth > 32) {
    return;
  }
  if (sApplyBudget <= 0) {
    if (!sBudgetLogged) {
      PortLog::Write("room geo: script loop; stopped after %d edges\n", kApplyBudget);
      sBudgetLogged = true;
    }
    return;
  }
  for (const size_t i : area.edgesFrom[node]) {
    const ScriptEdge& edge = area.script.edges[i];
    if (edge.event == event) {
      Apply(area, edge, depth + 1);
    }
  }
}

void RegisterNodes(Area& area) {
  // Registered precedes InitializedInArea's target snapshot. Use the same
  // Decrement state transition and target writes as AcceptScriptMsg0xd5fdb8.
  for (size_t i = 0; i < area.nodes.size(); ++i) {
    const ScriptNode& node = area.script.nodes[i];
    NodeState& state = area.nodes[i];
    if (node.kind == kAlphaModulate && state.modulation.Registered(node.AlphaConfig(), state.active)) {
      sApplyBudget = kApplyBudget;
      Send(area, uint32_t(i), 3, 0);
    }
  }
}

void Apply(Area& area, const ScriptEdge& edge, int depth) {
  --sApplyBudget;
  if (edge.action == kGroupModulate) {
    if (edge.to >= area.groups.size()) {
      return;
    }
    const auto& state = area.nodes[edge.from].modulation;
    const auto config = area.script.nodes[edge.from].AlphaConfig();
    for (size_t i : area.groups[edge.to]) {
      Placed& item = area.items[i];
      if (state.restore) {
        for (const auto& cached : area.nodes[edge.from].targetFlags) {
          if (cached.item != i) continue;
          item.modulated = cached.modulated;
          item.alpha = cached.alpha;
          item.modulation = cached.flags;
          break;
        }
        continue;
      }
      item.modulated = true;
      item.alpha = state.alpha;
      CModelFlags flags = item.alpha == 1.f ? CModelFlags::Normal() : CModelFlags::AlphaBlended(item.alpha);
      flags = flags.DepthCompareUpdate((config.flags & 4) != 0, (config.flags & 8) != 0);
      unsigned extra = flags.GetOtherFlags();
      if (config.flags & 16) extra |= CModelFlags::kF_DepthGreater;
      if ((config.flags & 512) && item.alpha != 1.f) extra |= CModelFlags::kF_DrawNormal;
      item.modulation = CModelFlags(flags, extra);
    }
    return;
  }
  if (edge.action == kGroupNextClip) {
    if (edge.to >= area.groups.size()) {
      return;
    }
    size_t moved = 0;
    for (const size_t i : area.groups[edge.to]) {
      Placed& item = area.items[i];
      if (!item.anim.empty()) {
        item.clip = std::min(item.clip + 1, item.anim.size() - 1);
        item.clipStart = area.animTime;
        ++moved;
      }
    }
    if (moved != 0) {
      PortLog::Write("room geo: group %u: %zu instance(s) on their next clip\n", unsigned(edge.to), moved);
    }
    return;
  }
  if (edge.action == kGroupShow || edge.action == kGroupHide || edge.action == kGroupToggle) {
    if (edge.to >= area.groupShown.size()) {
      area.groupShown.resize(size_t(edge.to) + 1, -1);
    }
    int8_t& group = area.groupShown[edge.to];
    group = edge.action == kGroupShow ? 1 : edge.action == kGroupHide ? 0 : group == 1 ? 0 : 1;
    if (edge.to >= area.groups.size()) {
      return;
    }
    size_t changed = 0;
    for (const size_t i : area.groups[edge.to]) {
      Placed& item = area.items[i];
      const bool shown = edge.action == kGroupShow ? true : edge.action == kGroupHide ? false : !item.shown;
      changed += shown != item.shown;
      item.shown = shown;
    }
    if (changed != 0) {
      PortLog::Write("room geo: group %u: %zu of %zu instance(s) %s\n", unsigned(edge.to), changed,
                     area.groups[edge.to].size(), edge.action == kGroupShow ? "shown" : edge.action == kGroupHide ? "hidden" : "toggled");
    }
    return;
  }
  NodeState& node = area.nodes[edge.to];
  const ScriptNode& kind = area.script.nodes[edge.to];
  switch (edge.action) {
  case kNodeActivate:
    node.active = true;
    return;
  case kNodeDeactivate:
    // A trigger told to stop forgets what is inside it (CScriptTrigger::AcceptScriptMsg),
    // so it sends Entered again once it is back on and the camera is still in it.
    node.active = false;
    node.inside = false;
    return;
  default:
    break;
  }
  if (!node.active) {
    return;
  }
  if (kind.kind == kAlphaModulate && (edge.action == kIncrement || edge.action == kDecrement)) {
    if (node.modulation.Message(kind.AlphaConfig(), edge.action == kIncrement)) {
      Send(area, edge.to, 3, depth);
    }
    return;
  }
  if (edge.action == kFire && kind.kind == kRelay) {
    Send(area, edge.to, 0, depth);
  } else if (edge.action == kIncrement && kind.kind == kCounter) {
    if (kind.max != 0 && node.count >= kind.max) {
      return;
    }
    ++node.count;
    if (node.count == kind.max) {
      Send(area, edge.to, 2, depth);
    }
    Send(area, edge.to, 0, depth);
  } else if (edge.action == kDecrement && kind.kind == kCounter) {
    if (node.count == 0) {
      return;
    }
    --node.count;
    Send(area, edge.to, node.count == 0 ? 1 : 0, depth);
  }
}

} // namespace

void Prefetch(uint32_t mrea) {
  if (GetMode() == Mode::Off || Areas().count(mrea) != 0 || sPrefetched.count(mrea) != 0) {
    return;
  }
  std::string path = PortMods::RoomGeoPath(mrea);
  if (path.empty()) {
    return;
  }
  // Areas streamed in and dropped again before they loaded: the finished ones go first.
  for (auto it = sPrefetched.begin(); sPrefetched.size() >= kMaxPrefetched && it != sPrefetched.end();) {
    if (it->second.parsed.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      it = sPrefetched.erase(it);
    } else {
      ++it;
    }
  }
  if (sPrefetched.size() >= kMaxPrefetched) {
    return;
  }
  try {
    std::future< Parsed > parsed = std::async(std::launch::async, ReadAndParse, path);
    sPrefetched.emplace(mrea, Prefetched{std::move(path), std::move(parsed)});
  } catch (...) {
    // No thread to spare: Load reads the file itself.
  }
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  auto& areas = Areas();
  if (!sBuffersReady && !sWarned) {
    for (size_t i = 0; i < count; ++i) {
      if (!PortMods::RoomGeoPath(mreas[i]).empty()) {
        PortLog::Write("room geo: not drawn; the game started without room geometry installed. Restart it.\n");
        sWarned = true;
        break;
      }
    }
  }
  if (GetMode() == Mode::Off) {
    CCubeModel::PortClearPBROverrides();
    areas.clear();
    ++sGeneration;
    return;
  }
  sTriggers = false;
  for (auto it = areas.begin(); it != areas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      it = areas.erase(it);
      ++sGeneration;
    } else {
      sTriggers = sTriggers || !it->second.triggers.empty() || it->second.retailEdges;
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (areas.find(mreas[i]) == areas.end()) {
      Load(mreas[i], areas[mreas[i]]);
    }
  }
  // A sky waits for the area to be the one drawing it otherwise, and retail's world sky
  // shows meanwhile: the Frigate's planet changed under the player a moment after entering.
  for (auto& [mrea, area] : areas) {
    for (const size_t index : area.skyModels) {
      const Model& model = area.models[index];
      if (!model.hidden && !model.data->IsLoaded(0)) {
        model.data->Touch(CModelData::kWM_Normal, 0);
      }
    }
  }
  // A frame Draw sits out (the thermal and X-ray visors) must not queue the last one's.
  for (auto& [mrea, area] : areas) {
    area.sorted.clear();
  }
  sQueued = false;
  sQueueStale = false;
  sDrawnLast = sDrawn;
  sDrawn = 0;
  sDrawnCoarseLast = sDrawnCoarse;
  sDrawnCoarse = 0;
  if (!sMaterialValues.empty()) {
    BindMaterialValues();
  }
}

namespace {
struct ShadowReceiver {
  const CModel* model;
  const CTransform4f* xf;
  CAABox bounds;
};
std::vector< ShadowReceiver > sShadowReceivers;
} // namespace

void ClearShadowReceivers() { sShadowReceivers.clear(); }

void DrawShadowReceivers(const float lo[3], const float hi[3]) {
  CGraphics::DisableAllLights();
  for (const ShadowReceiver& r : sShadowReceivers) {
    const CVector3f& a = r.bounds.GetMinPoint();
    const CVector3f& b = r.bounds.GetMaxPoint();
    if (b.GetX() < lo[0] || a.GetX() > hi[0] || b.GetY() < lo[1] || a.GetY() > hi[1] ||
        b.GetZ() < lo[2] || a.GetZ() > hi[2]) {
      continue;
    }
    gpRender->SetModelMatrix(*r.xf);
    r.model->DrawUnsortedParts(CModelFlags::Normal());
  }
}

bool Draw(const CStateManager& mgr, const CGameArea& gameArea, const CFrustumPlanes& frustum) {
  if (GetMode() == Mode::Off) {
    return false;
  }
  auto& areas = Areas();
  const auto found = areas.find(gameArea.GetAreaAssetId());
  if (found == areas.end() || !found->second.hasFile || found->second.skyOnly) {
    return false;
  }
  Area& area = found->second;
  area.sorted.clear();
  if (!area.placed) {
    for (Placed& item : area.items) {
      item.xf = gameArea.GetTM() * item.xf;
      item.base = item.xf;
    }
    area.placed = true;
  }
  // Slaves go where the platform has taken them (CScriptPlatform::DragSlaves adds each
  // translation the platform makes, never its turns). One whose platform is gone stays
  // where it was last taken.
  for (Placed& item : area.items) {
    if (item.platform == 0) {
      continue;
    }
    const CActor* const platform =
        TCastToConstPtr< CActor >(mgr.GetObjectById(mgr.GetIdForScript(TEditorId(item.platform))));
    if (item.actorTransform) {
      // A shared transform does not replace this entity's script activation.
      item.actorVisible = platform != nullptr && platform->GetActive();
      if (platform != nullptr) {
        item.xf = item.base = platform->GetTransform() * item.actorLocal;
        item.bounded = false;
      }
      continue;
    }
    if (platform == nullptr) {
      continue;
    }
    const CVector3f dragged = platform->GetTranslation() - item.platformStart;
    if (dragged != item.dragged) {
      item.xf.AddTranslation(dragged - item.dragged);
      item.base.AddTranslation(dragged - item.dragged);
      item.dragged = dragged;
      item.bounded = false;
    }
  }
  if (area.animated) {
    for (Placed& item : area.items) {
      if (item.anim.empty()) {
        continue;
      }
      if (item.shown && !item.wasShown && item.animOnShow) {
        item.clip = 0;
        item.clipStart = area.animTime;
      }
      item.wasShown = item.shown;
      if (!item.shown) {
        continue;
      }
      // A clip that has run out hands over to the next, from where it ended.
      while (item.clip + 1 < item.anim.size() && !item.anim[item.clip].loop &&
             area.animTime - item.clipStart >= double(ClipLength(item.anim[item.clip]))) {
        item.clipStart += double(ClipLength(item.anim[item.clip]));
        ++item.clip;
      }
      item.xf = item.base * Pose(item.anim[item.clip], area.animTime - item.clipStart);
      item.bounded = false;
    }
  }
  if (area.loaded != area.models.size()) {
    area.loaded = 0;
    for (Model& model : area.models) {
      if (!model.loaded) {
        if (!model.data->IsLoaded(0)) {
          model.data->Touch(CModelData::kWM_Normal, 0);
          continue;
        }
        model.bounds = model.data->GetBounds();
        model.loaded = true;
        KeepResident(model);
      }
      ++area.loaded;
    }
    // The area's own geometry stays until the last model is in, so no frame has holes.
    if (area.loaded != area.models.size() && GetMode() == Mode::Replace) {
      return false;
    }
  }
  if (!area.clustered && area.loaded == area.models.size()) {
    BuildClusters(area);
  }
  // The coarser levels come in behind the full models and hold nothing up: until one is
  // in, the level before it is drawn in its place.
  if (area.loaded == area.models.size() && area.levelsLoaded != area.levels && LodDistance() > 0.f) {
    area.levelsLoaded = 0;
    for (Model& model : area.models) {
      for (Level& level : model.levels) {
        if (!level.loaded) {
          if (!level.data->IsLoaded(0)) {
            level.data->Touch(CModelData::kWM_Normal, 0);
            continue;
          }
          level.loaded = true;
          KeepResident(level.resident, (**level.data->PickStaticModel(CModelData::kWM_Normal)).GetCubeModel());
        }
        ++area.levelsLoaded;
      }
    }
  }
  if (area.clustered && !area.clusterLevels && area.levelsLoaded == area.levels && LodDistance() > 0.f) {
    BuildClusterLevels(area);
  }
  // A trigger's linked actors are made active and given its alpha every frame it is active
  // (CScriptDamageableTrigger::Think); when it goes inactive, after its death fade or by a
  // Deactivate at alpha 0, they are not seen again until it is.
  for (Placed& item : area.items) {
    if (item.follow == 0) {
      continue;
    }
    const CScriptDamageableTrigger* const trigger = dynamic_cast< const CScriptDamageableTrigger* >(
        mgr.GetObjectById(mgr.GetIdForScript(TEditorId(item.follow))));
    if (trigger != nullptr && trigger->GetActive()) {
      item.shown = true;
      item.following = true;
      item.alpha = trigger->PortLinkedAlpha();
    } else if (item.following) {
      item.shown = false;
      item.following = false;
    }
  }
  const bool volumes = !AreaLights() && PortRoomEnv::HasVolume(gameArea.GetAreaAssetId());
  // A room with a lightmap and no grid: what has a lookup into the lightmap is lit by it, as
  // Remastered's world models are; the rest (a merged cluster, an instance without a lookup)
  // keeps the area's lights, there being no grid to light it.
  const bool lightmapOnly = !AreaLights() && !volumes && PortRoomEnv::HasLightmapOnly(gameArea.GetAreaAssetId());
  const auto bakes = [&](const float* lookup) {
    return volumes || (lightmapOnly && lookup != nullptr && HasLightmapLookup(lookup));
  };
  CScriptLayerManager* const layers =
      area.gated ? const_cast< CStateManager& >(mgr).WorldLayerState().GetPtr() : nullptr;
  if (area.drawOrder.size() != area.items.size()) {
    area.drawOrder.resize(area.items.size());
    for (size_t i = 0; i < area.drawOrder.size(); ++i) {
      area.drawOrder[i] = i;
    }
    std::stable_sort(area.drawOrder.begin(), area.drawOrder.end(),
                     [&](size_t a, size_t b) { return area.items[a].model < area.items[b].model; });
  }
  // MinPixels: an item is left out while its bounds' diagonal over its distance, roughly
  // the angle it spans, is under that many pixels' worth of the projection's height.
  float minSpan = 0.f;
  const CGraphics::CProjectionState& projection = CGraphics::GetProjectionState();
  if (MinPixels() > 0.f && projection.IsPerspective() && projection.GetNear() > 0.f &&
      CGraphics::GetViewportHeight() > 0) {
    minSpan = MinPixels() * std::fabs(projection.GetTop() - projection.GetBottom()) /
              (projection.GetNear() * float(CGraphics::GetViewportHeight()));
  }
  const CVector3f eye = CGraphics::GetViewPoint();
  // From the eye to the nearest point of the box, squared: 0 inside it.
  const auto distanceSq = [&](const CAABox& bounds) {
    const CVector3f& lo = bounds.GetMinPoint();
    const CVector3f& hi = bounds.GetMaxPoint();
    const float dx = std::max(std::max(lo.GetX() - eye.GetX(), eye.GetX() - hi.GetX()), 0.f);
    const float dy = std::max(std::max(lo.GetY() - eye.GetY(), eye.GetY() - hi.GetY()), 0.f);
    const float dz = std::max(std::max(lo.GetZ() - eye.GetZ(), eye.GetZ() - hi.GetZ()), 0.f);
    return dx * dx + dy * dy + dz * dz;
  };
  const auto tooSmall = [&](const CAABox& bounds) {
    if (minSpan <= 0.f) {
      return false;
    }
    return (bounds.GetMaxPoint() - bounds.GetMinPoint()).Magnitude() < minSpan * std::sqrt(distanceSq(bounds));
  };
  // Remastered's switch distances are the model's own, so a scaled item's are scaled with it.
  const float lodScaleSq = LodDistance() * LodDistance();
  // The last level loaded whose distance the bounds are past, -1 for the full model. `usable`
  // caps it, for a cluster's merged levels.
  const auto pickLevel = [&](const Model& model, size_t usable, const CAABox& bounds, float scaleSq) {
    int picked = -1;
    if (usable == 0 || lodScaleSq <= 0.f) {
      return picked;
    }
    const float d = distanceSq(bounds) / scaleSq;
    for (size_t i = 0; i < usable && model.levels[i].distanceSq * lodScaleSq <= d; ++i) {
      if (model.levels[i].loaded) {
        picked = int(i);
      }
    }
    return picked;
  };
  const auto light = [&](std::unique_ptr< CActorLights >& lights, bool& areaLit, const CAABox& bounds, bool baked) {
    if (lights == nullptr || areaLit == baked) {
      // The baked ambient already holds the area's lights, so that set has room for none:
      // one that expects area lights and has none drops its dynamic lights too. Its
      // ambient is what a material outside PBR is drawn at.
      lights.reset(new CActorLights(8, CVector3f(0.f, 0.f, 0.f), 4, baked ? 0 : 4));
      if (baked) {
        lights->SetAmbientColor(CColor::White());
      }
      areaLit = !baked;
    }
    if (!baked) {
      lights->BuildAreaLightList(mgr, gameArea, bounds);
    }
    lights->BuildDynamicLightList(mgr, bounds);
  };
  const auto cutout = [](Model& model) {
    const CCubeModel* const cube = model.cutout < 0 ? CubeModel(model) : nullptr;
    if (cube != nullptr) {
      model.cutout = 0;
      for (int i = 0, count = int(cube->PortMaterialCount()); i < count; ++i) {
        if ((cube->GetMaterialByIndex(i).GetFlags() & kStateFlag_AlphaTest) != 0) {
          model.cutout = 1;
          break;
        }
      }
    }
    return model.cutout == 1;
  };
  // What is drawn this frame, gathered first so it can go nearest first.
  struct Visible {
    bool cutout; // after every opaque one
    float distanceSq;
    const CModel* model;
    const CTransform4f* xf;
    CActorLights* lights;
    const CAABox* bounds;
    const float* glow; // null for none
    // Placed::lightmap; null for a cluster, whose members each have their own (it takes the
    // volume instead)
    const float* lightmap;
    const Placed* driven; // its float driver's instance, or null
    const CModelFlags* flags;
    bool rigidBounds;
  };
  static std::vector< Visible > visible;
  visible.clear();
  // Out of view but in the sun's shadow map: these only cast (a roof over the floor in view).
  static std::vector< Visible > casters;
  casters.clear();
  static const CTransform4f kIdentity = CTransform4f::Identity();
  const auto add = [&](Model& model, const CModel& drawn, const CTransform4f& xf, CActorLights* lights,
                       const CAABox& bounds, const float* glow = nullptr, const float* lightmap = nullptr,
                       bool inView = true, const Placed* driven = nullptr, const CModelFlags* flags = nullptr,
                       bool rigidBounds = true) {
    (inView ? visible : casters)
        .push_back({cutout(model), distanceSq(bounds), &drawn, &xf, lights, &bounds, glow, lightmap, driven, flags, rigidBounds});
    if (inView && !visible.back().cutout) {
      sShadowReceivers.push_back({&drawn, &xf, bounds});
    }
  };
  // In view, or else whether it can cast into view: an opaque model in the shadow map's box.
  const auto inView = [&](Model& model, const CAABox& bounds, bool& casts) {
    if (frustum.BoxInFrustumPlanes(bounds)) {
      return true;
    }
    const float lo[3] = {bounds.GetMinPoint().GetX(), bounds.GetMinPoint().GetY(), bounds.GetMinPoint().GetZ()};
    const float hi[3] = {bounds.GetMaxPoint().GetX(), bounds.GetMaxPoint().GetY(), bounds.GetMaxPoint().GetZ()};
    // A cutout casts too: aurora gives a draw a caster only when it can discard like the colour pass does
    // (a Remastered cut bit); a retail alpha-tested draw (alphaCompare) still gets none.
    casts = sOffscreenCasters && GXPortShadowBoxCasts(lo, hi);
    return false;
  };
  for (Cluster& cluster : area.clusters) {
    cluster.split = !sMergedDraws;
    for (const size_t i : cluster.members) {
      cluster.split = cluster.split || !area.items[i].shown;
    }
    bool casts = false;
    if (cluster.split || area.models[cluster.model].hidden || tooSmall(cluster.bounds) ||
        (!inView(area.models[cluster.model], cluster.bounds, casts) && !casts)) {
      continue;
    }
    light(cluster.lights, cluster.areaLit, cluster.bounds, bakes(nullptr));
    const int level = pickLevel(area.models[cluster.model], cluster.levels.size(), cluster.bounds, cluster.scaleSq);
    add(area.models[cluster.model], level < 0 ? *cluster.merged : *cluster.levels[level].merged, kIdentity,
        cluster.lights.get(), cluster.bounds, nullptr, nullptr, !casts);
    if (casts) {
      continue;
    }
    sDrawn += int(cluster.members.size());
    if (level >= 0) {
      sDrawnCoarse += int(cluster.members.size());
    }
  }
  for (const size_t index : area.drawOrder) {
    Placed& item = area.items[index];
    Model& model = area.models[item.model];
    if (item.sky || !model.loaded || model.hidden || !item.shown || !item.actorVisible || item.alpha <= 0.f ||
        (item.cluster != SIZE_MAX && !area.clusters[item.cluster].split)) {
      continue;
    }
    if (item.layer != kEveryLayer && layers != nullptr &&
        !layers->IsLayerActive(gameArea.GetAreaId(), item.layer)) {
      continue;
    }
    if (!item.bounded) {
      item.bounds = model.bounds.GetTransformedAABox(item.xf);
      item.bounded = true;
    }
    bool casts = false;
    if (tooSmall(item.bounds) || (!inView(model, item.bounds, casts) && (!casts || item.alpha < 1.f))) {
      continue;
    }
    const bool baked = bakes(item.lightmap);
    light(item.lights, item.areaLit, item.bounds, baked);
    item.volume = baked ? gameArea.GetAreaAssetId() : 0;
    // Blended surfaces (glass, decals) wait for the sorted pass, where they are drawn
    // back to front among the actors.
    // A faded one is drawn whole among them, as retail's CActor with blend flags is.
    item.level = pickLevel(model, model.levels.size(), item.bounds, item.scaleSq);
    const CModelData& data = item.level < 0 ? *model.data : *model.levels[item.level].data;
    const CModel& cmodel = **data.PickStaticModel(CModelData::kWM_Normal);
    const bool rigidBounds = item.anim.empty() && item.platform == 0 && item.follow == 0 && !item.driven && !item.modulated;
    if (casts) {
      add(model, cmodel, item.xf, item.lights.get(), item.bounds, nullptr, nullptr, false,
          item.driven ? &item : nullptr, nullptr, rigidBounds);
      continue;
    }
    if (item.level >= 0) {
      ++sDrawnCoarse;
    }
    if (item.modulated) {
      const auto plan = PlanAlphaRender(item.alpha, (item.modulation.GetOtherFlags() & CModelFlags::kF_DrawNormal) != 0);
      if (plan.unsorted) {
        add(model, cmodel, item.xf, item.lights.get(), item.bounds, item.glows ? item.glow : nullptr,
            item.lightmap, true, item.driven ? &item : nullptr, &item.modulation);
      }
      if (plan.sortedOpaque || !cmodel.IsDefinitelyOpaque()) area.sorted.push_back(&item);
      ++sDrawn;
      continue;
    }
    if (item.alpha < 1.f) {
      area.sorted.push_back(&item);
      ++sDrawn;
      continue;
    }
    add(model, cmodel, item.xf, item.lights.get(), item.bounds, item.glows ? item.glow : nullptr, item.lightmap, true,
        item.driven ? &item : nullptr, nullptr, rigidBounds);
    if (!cmodel.IsDefinitelyOpaque()) {
      area.sorted.push_back(&item);
    }
    ++sDrawn;
  }
  if (sFrontToBack) {
    // Nearest first, so the GPU's depth test rejects the pixels behind before they are shaded;
    // cut-outs last, since a shader that can discard keeps the hardware from testing early.
    std::stable_sort(visible.begin(), visible.end(), [](const Visible& a, const Visible& b) {
      return a.cutout != b.cutout ? b.cutout : a.distanceSq < b.distanceSq;
    });
  }
  const bool filterShadows = GXPortShadowFrameActive() && GXPortShadowCascadeCulling();
  const auto drawOne = [&](const Visible& draw) {
    struct MaskScope {
      bool enabled;
      u8 prior;
      explicit MaskScope(bool on) : enabled(on), prior(on ? GXPortGetShadowCascadeMask() : 0xf) {}
      ~MaskScope() {
        if (enabled) GXPortSetShadowCascadeMask(prior);
      }
    } maskScope(filterShadows);
    // No material reads, bound transforms or mask commands when no sun map is active.
    if (filterShadows) {
      u8 mask = 0xf;
      bool rigid = draw.rigidBounds && draw.flags == nullptr;
      const CCubeModel* cube = draw.model->GetCubeModel();
      rigid = rigid && cube != nullptr && cube->PortMaterialCount() != 0;
      for (uint i = 0; rigid && i < cube->PortMaterialCount(); ++i) {
        float values[19];
        // Only ordinary rigid PBR surfaces have verified bounds. Procedural wind (32768),
        // special materials and unknown records retain the reference all-layer path.
        rigid = cube->PortReadPBRMaterial(i, values) != 0 && std::isfinite(values[13]) &&
                std::fabs(values[13]) < 0.5f && std::isfinite(values[7]) && values[7] >= 0.f &&
                values[7] < 16777216.f && (static_cast<uint32_t>(values[7] + 0.5f) & 32768u) == 0;
      }
      if (rigid) {
        // Use the actual chosen LoD or merged model's box, transformed by its draw matrix.
        // Merged clusters already contain world-space vertices and use the identity.
        const CAABox bounds = draw.model->GetBoundingBox().GetTransformedAABox(*draw.xf);
        const auto& lo = bounds.GetMinPoint();
        const auto& hi = bounds.GetMaxPoint();
        const float min[3] = {lo.GetX(), lo.GetY(), lo.GetZ()};
        const float max[3] = {hi.GetX(), hi.GetY(), hi.GetZ()};
        mask = GXPortShadowBoxMask(min, max);
      }
      GXPortSetShadowCascadeMask(mask);
      if (mask == 0 && GXPortGetShadowCasterMode() == 2) {
        return;
      }
    }
    if (bakes(draw.lightmap)) {
      const CVector3f centre = draw.bounds->GetCenterPoint();
      const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
      PortRoomEnv::SetVolumeHint(gameArea.GetAreaAssetId(), at);
      static const float kNone[3] = {};
      PortRoomEnv::SetLightmapHint(draw.lightmap != nullptr ? draw.lightmap : kNone);
    }
    gpRender->SetModelMatrix(*draw.xf);
    draw.lights->ActivateLights();
    if (draw.glow != nullptr) {
      CCubeModel::PortSetGlow(draw.glow);
    }
    if (draw.driven != nullptr) {
      CCubeModel::PortSetKindParam(draw.driven->driverSlot, draw.driven->driverValue);
    }
    draw.model->DrawUnsortedParts(draw.flags != nullptr ? *draw.flags : CModelFlags::Normal());
    if (draw.driven != nullptr) {
      CCubeModel::PortSetKindParam(-1, 0.f);
    }
    if (draw.glow != nullptr) {
      CCubeModel::PortSetGlow(nullptr);
    }
  };
  // With the sort, the cut-outs come last, after every opaque model: they go twice, depth only
  // and then shaded where the depth is equal, so the grass hidden behind grass isn't shaded.
  const auto firstCutout = sFrontToBack && sDepthPrepass
                               ? std::find_if(visible.begin(), visible.end(), [](const Visible& v) { return v.cutout; })
                               : visible.end();
  std::for_each(visible.begin(), firstCutout, drawOne);
  if (firstCutout != visible.end()) {
    GXPortSetDepthPrepass(1);
    std::for_each(firstCutout, visible.end(), drawOne);
    GXPortSetDepthPrepass(2);
    std::for_each(firstCutout, visible.end(), drawOne);
    GXPortSetDepthPrepass(0);
  }
  if (sCasterFrame != CGraphics::GetFrameCounter()) {
    sCasterFrame = CGraphics::GetFrameCounter();
    sLastCasterCount = sCasterCount;
    sCasterCount = 0;
  }
  sCasterCount += static_cast< uint32_t >(casters.size());
  if (!casters.empty()) {
    GXPortSetShadowCasterOnly(GX_TRUE);
    std::for_each(casters.begin(), casters.end(), drawOne);
    GXPortSetShadowCasterOnly(GX_FALSE);
  }
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  if (volumes || lightmapOnly) {
    PortRoomEnv::ClearVolumeHint();
  }
  return GetMode() == Mode::Replace;
}

void AddSorted(const CGameArea& gameArea) {
  const auto found = Areas().find(gameArea.GetAreaAssetId());
  if (found == Areas().end()) {
    return;
  }
  if (sQueued && sQueuedGeneration != sGeneration) {
    sQueueStale = true;
  }
  sQueued = true;
  sQueuedGeneration = sGeneration;
  const CVector3f forward = CGraphics::GetViewMatrix().GetForward();
  for (const Placed* item : found->second.sorted) {
    gpRender->AddDrawable(item, item->bounds.ClosestPointAlongVector(forward), item->bounds, kDrawableType,
                          IRenderer::kDS_SortedCallback);
  }
}

void DrawSorted(const void* drawable) {
  // The models of the area it was added for are still there: the list is rebuilt by Draw
  // every frame, and an area's models only go between frames. Should one go mid-frame
  // all the same (the console turning room geometry off), the item is not touched.
  if (sQueueStale || sQueuedGeneration != sGeneration) {
    return;
  }
  const Placed& item = *static_cast< const Placed* >(drawable);
  if (item.owner == nullptr) {
    return;
  }
  const Model& model = item.owner->models[item.model];
  if (!model.loaded) {
    return;
  }
  if (item.volume != 0) {
    const CVector3f centre = item.bounds.GetCenterPoint();
    const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
    PortRoomEnv::SetVolumeHint(item.volume, at);
    PortRoomEnv::SetLightmapHint(item.lightmap);
  }
  gpRender->SetModelMatrix(item.xf);
  item.lights->ActivateLights();
  const CModelData& data =
      item.level >= 0 && size_t(item.level) < model.levels.size() ? *model.levels[item.level].data : *model.data;
  const CModel& cmodel = **data.PickStaticModel(CModelData::kWM_Normal);
  if (item.glows) {
    CCubeModel::PortSetGlow(item.glow);
  }
  if (item.driven) {
    CCubeModel::PortSetKindParam(item.driverSlot, item.driverValue);
  }
  if (item.modulated) {
    const auto plan = PlanAlphaRender(item.alpha, (item.modulation.GetOtherFlags() & CModelFlags::kF_DrawNormal) != 0);
    // The retail wrappers interpret DrawNormal as an extra colour draw. The
    // Remastered state (0xb42074) instead requests a depth-only opaque group.
    const CModelFlags flags(item.modulation, item.modulation.GetOtherFlags() & ~CModelFlags::kF_DrawNormal);
    if (plan.depthOpaque) {
      GXPortSetDepthPrepass(1);
      cmodel.DrawUnsortedParts(flags.DepthCompareUpdate((flags.GetOtherFlags() & CModelFlags::kF_DepthCompare) != 0, true));
      GXPortSetDepthPrepass(0);
    }
    if (plan.sortedOpaque) cmodel.DrawUnsortedParts(flags);
    if (plan.visible) cmodel.DrawSortedParts(flags);
  } else if (item.alpha < 1.f) {
    cmodel.Draw(CModelFlags(CModelFlags::kT_Blend, item.alpha));
  } else {
    cmodel.DrawSortedParts(CModelFlags::Normal());
  }
  if (item.driven) {
    CCubeModel::PortSetKindParam(-1, 0.f);
  }
  if (item.glows) {
    CCubeModel::PortSetGlow(nullptr);
  }
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  if (item.volume != 0) {
    PortRoomEnv::ClearVolumeHint();
  }
}

bool sReplacingArea = false;

bool Hides(const CGameArea& gameArea, uint32_t editorId) {
  if (GetMode() == Mode::Off) {
    return false;
  }
  auto& areas = Areas();
  const auto found = areas.find(gameArea.GetAreaAssetId());
  if (found == areas.end() || !found->second.hasFile || found->second.hides.empty()) {
    return false;
  }
  const CScriptLayerManager* const layers =
      gpGameState != nullptr ? gpGameState->CurrentWorldState().GetLayerState().GetPtr() : nullptr;
  for (const auto& [id, at] : found->second.hides) {
    const Placed& item = found->second.items[at];
    if (id == editorId && item.shown && item.actorVisible &&
        (item.layer == kEveryLayer || layers == nullptr ||
         layers->IsLayerActive(gameArea.GetAreaId(), item.layer))) {
      return true;
    }
  }
  return false;
}

// The room's sky instances that are shown and on a layer that is on: its own, or (`world`)
// the world's (Instance::worldSky).
template < typename Fn >
void ForEachSky(const CGameArea& gameArea, bool world, Fn&& fn) {
  if (GetMode() == Mode::Off || gpGameState == nullptr) {
    return;
  }
  auto& areas = Areas();
  const auto found = areas.find(gameArea.GetAreaAssetId());
  if (found == areas.end() || !found->second.hasFile) {
    return;
  }
  const Area& area = found->second;
  const CScriptLayerManager* const layers = gpGameState->CurrentWorldState().GetLayerState().GetPtr();
  for (const Placed& item : area.items) {
    if (!item.sky || item.worldSky != world || !item.shown || area.models[item.model].hidden ||
        (item.layer != kEveryLayer && layers != nullptr && !layers->IsLayerActive(gameArea.GetAreaId(), item.layer))) {
      continue;
    }
    if (!fn(area, item)) {
      return;
    }
  }
}

int Skies(const CGameArea& gameArea, SkyLayer (&out)[kMaxSkyLayers], bool world) {
  int count = 0;
  bool loading = false;
  float corners[kMaxSkyLayers];
  float largest = 0.f;
  ForEachSky(gameArea, world, [&](const Area& area, const Placed& item) {
    const Model& model = area.models[item.model];
    if (!model.data->IsLoaded(0)) {
      // Draw loads the area's models, but only while the area is drawn.
      model.data->Touch(CModelData::kWM_Normal, 0);
      loading = true;
      return true;
    }
    if (count == kMaxSkyLayers) {
      return false;
    }
    SkyLayer& layer = out[count];
    layer.model = &**model.data->PickStaticModel(CModelData::kWM_Normal);
    // Until Draw places the area, `xf` is still in area space.
    layer.orient = area.placed ? item.xf : gameArea.GetTM() * item.xf;
    layer.orient.SetTranslation(CVector3f::Zero());
    std::copy(item.skyRadiance, item.skyRadiance + 3, layer.radiance);
    const CAABox& box = layer.model->GetBoundingBox();
    corners[count] = std::max(box.GetMinPoint().Magnitude(), box.GetMaxPoint().Magnitude()) *
                     std::max({layer.orient.GetColumn(kDX).Magnitude(), layer.orient.GetColumn(kDY).Magnitude(),
                               layer.orient.GetColumn(kDZ).Magnitude()});
    largest = std::max(largest, corners[count]);
    ++count;
    return true;
  });
  if (loading) {
    return 0;
  }
  // Remastered's skies are thousands of units across, past the far plane. Centred on the
  // camera, a sky looks the same at any size, so all of them are scaled alike to fit inside it,
  // and drawn outermost first: Landing Site's cloud layers sit one inside the other.
  for (int i = 0; i < count; ++i) {
    for (int j = i; j > 0 && corners[j] > corners[j - 1]; --j) {
      std::swap(corners[j], corners[j - 1]);
      std::swap(out[j], out[j - 1]);
    }
  }
  if (largest > 0.f) {
    const CTransform4f fit = CTransform4f::Scale(0.5f * CGraphics::GetProjectionState().GetFar() / largest);
    for (int i = 0; i < count; ++i) {
      out[i].orient = out[i].orient * fit;
    }
  }
  return count;
}

bool HasSky(const CGameArea& gameArea) {
  bool has = false;
  ForEachSky(gameArea, false, [&](const Area&, const Placed&) {
    has = true;
    return false;
  });
  return has;
}

uint32_t sSkyDrawnFor = 0;

bool HidesSky(const CGameArea& gameArea, const CActor& actor) {
  // Retail's sky domes are 314 to 1867 units across (scaled); the next largest actor in a room
  // with a sky of its own is 139 (Artifact Temple's bird eyes). A dome is that big across two
  // axes at least; a box long in one only is no dome (Sun Tower's spiked door cover F6EAE8D8
  // reaches 5000 units down and is 4 across).
  constexpr float kDomeSize = 250.f;
  if (sSkyDrawnFor == 0 || TCastToConstPtr< CScriptActor >(&actor) == nullptr || !actor.HasModelData()) {
    return false;
  }
  const CAABox box = actor.GetModelData()->GetBounds();
  const CVector3f size = box.GetMaxPoint() - box.GetMinPoint();
  float extents[3] = {std::fabs(size.GetX()), std::fabs(size.GetY()), std::fabs(size.GetZ())};
  std::sort(extents, extents + 3);
  return extents[1] > kDomeSize && HasSky(gameArea);
}

void OnScriptState(CStateManager& mgr, uint32_t editorId, int state) {
  const TAreaId areaId(int((editorId >> 16) & 0x3ff));
  const CWorld* const world = mgr.GetWorld();
  // An area's objects are made after it counts as loaded (CGameArea::PostConstructArea), so
  // a state from elsewhere (editor id 0 reads as area 0) loads nothing.
  if (world == nullptr || areaId.Value() >= world->GetNumAreas() || !world->GetArea(areaId)->IsLoaded()) {
    return;
  }
  const uint32_t mrea = world->GetArea(areaId)->GetAreaAssetId();
  // The room's colour grade hints follow the scripts too, room geometry or not.
  PortRoomEnv::OnScriptState(mrea, editorId & 0x3ffffff, state);
  if (GetMode() == Mode::Off) {
    return;
  }
  auto& areas = Areas();
  auto found = areas.find(mrea);
  if (found == areas.end()) {
    // Script objects are made, and some send states, before the next SetLoadedAreas, which
    // keeps the area (an area without a file stays in as an empty one).
    found = areas.emplace(mrea, Area()).first;
    Load(mrea, found->second);
  }
  if (!sTriggers) {
    return;
  }
  Area& area = found->second;
  const uint32_t sender = editorId & 0x3ffffff;
  for (size_t i = 0; i < area.triggers.size(); ++i) {
    const Trigger& trigger = area.triggers[i];
    if (trigger.sender != sender || trigger.state != state) {
      continue;
    }
    if (trigger.delay > 0.f) {
      // As the timer it stands for, sent again before it is up: started over.
      std::erase_if(area.pending, [&](const Pending& p) { return p.trigger == i; });
      area.pending.push_back({area.clock + trigger.delay, i});
      continue;
    }
    Act(mrea, area, trigger);
  }
  // A driver's ResetAndStart: started over, the delay after the state.
  for (const DriverStart& start : area.driverStarts) {
    if (start.sender == sender && start.state == state) {
      area.items[start.item].startAt = area.clock + start.delay;
    }
  }
  if (area.retailEdges) {
    for (const ScriptEdge& edge : area.script.edges) {
      if (edge.retail && (edge.from & 0x3ffffff) == sender && edge.event == state) {
        sApplyBudget = kApplyBudget;
        Apply(area, edge, 0);
      }
    }
  }
}

void Think(CStateManager& mgr, float dt) {
  const CWorld* const world = mgr.GetWorld();
  if (GetMode() == Mode::Off || world == nullptr) {
    return;
  }
  auto& areas = Areas();
  bool any = false;
  for (auto& [mrea, area] : areas) {
    any = any || !area.script.nodes.empty();
    if (area.animated) {
      area.animTime += dt;
    }
    area.clock += dt;
    // In the order they fall due, each one's action before the next.
    while (!area.pending.empty()) {
      const auto next = std::min_element(area.pending.begin(), area.pending.end(),
                                         [](const Pending& a, const Pending& b) { return a.due < b.due; });
      if (next->due > area.clock) {
        break;
      }
      const size_t trigger = next->trigger;
      area.pending.erase(next);
      Act(mrea, area, area.triggers[trigger]);
    }
    for (const DriverStart& start : area.driverStarts) {
      Placed& item = area.items[start.item];
      item.startAt = DriverStartAt(item.shown, item.startAt, area.clock);
      // Before it starts the parameter is the first key; after the last it holds that key.
      item.driverValue = item.driverSpline.Eval(
          item.driverSpline.FirstTime() + float(std::max(0., area.clock - item.startAt)));
    }
  }
  if (!any) {
    return;
  }
  const CVector3f camera = mgr.CameraManager()->CurrentCamera(mgr).GetTranslation();
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    const CGameArea* const gameArea = world->GetArea(TAreaId(i));
    if (!gameArea->IsLoaded()) {
      continue;
    }
    const auto found = areas.find(gameArea->GetAreaAssetId());
    if (found == areas.end() || found->second.script.nodes.empty()) {
      continue;
    }
    Area& area = found->second;
    // Volumes are in the area's own space, as its script objects are.
    const CVector3f local = gameArea->GetInverseTransform() * camera;
    const float at[3] = {local.GetX(), local.GetY(), local.GetZ()};
    std::memcpy(area.camera, at, sizeof(at));
    for (size_t n = 0; n < area.nodes.size(); ++n) {
      const ScriptNode& node = area.script.nodes[n];
      NodeState& state = area.nodes[n];
      if (node.kind == kAlphaModulate && state.active && state.modulation.enabled) {
        const bool ended = state.modulation.Tick(node.AlphaConfig(), dt);
        sApplyBudget = kApplyBudget;
        Send(area, uint32_t(n), 3, 0);
        if (ended) Send(area, uint32_t(n), 2, 0);
      }
      // An inactive trigger does not think (CScriptTrigger::Think), so it sees nothing.
      if (node.kind != kCameraVolume || !state.active) {
        continue;
      }
      const float d[3] = {at[0] - node.centre[0], at[1] - node.centre[1], at[2] - node.centre[2]};
      bool inside = true;
      for (int axis = 0; axis < 3 && inside; ++axis) {
        const float* const a = node.axes + 3 * axis;
        inside = std::fabs(d[0] * a[0] + d[1] * a[1] + d[2] * a[2]) <= node.half[axis];
      }
      if (inside != state.inside) {
        state.inside = inside;
        sApplyBudget = kApplyBudget;
        Send(area, uint32_t(n), inside ? 0 : 1, 0);
      }
    }
  }
}

std::string ScriptInfo() {
  std::string out;
  char line[200];
  for (const auto& [mrea, area] : Areas()) {
    if (area.script.nodes.empty()) {
      continue;
    }
    std::snprintf(line, sizeof(line), "%08X: camera %.1f %.1f %.1f, %zu node(s), %zu group(s)\n", mrea,
                  area.camera[0], area.camera[1], area.camera[2], area.nodes.size(), area.groups.size());
    out += line;
    for (size_t n = 0; n < area.nodes.size(); ++n) {
      const ScriptNode& node = area.script.nodes[n];
      const NodeState& state = area.nodes[n];
      if (node.kind == kCameraVolume) {
        std::snprintf(line, sizeof(line), "  %zu volume%s%s at %.1f %.1f %.1f, half %.1f %.1f %.1f\n", n,
                      state.active ? "" : " (off)", state.inside ? " CAMERA IN" : "", node.centre[0],
                      node.centre[1], node.centre[2], node.half[0], node.half[1], node.half[2]);
      } else {
        std::snprintf(line, sizeof(line), "  %zu %s%s %u/%u\n", n, node.kind == kCounter ? "counter" : "relay",
                      state.active ? "" : " (off)", state.count, node.max);
      }
      out += line;
    }
    for (const ScriptEdge& edge : area.script.edges) {
      std::snprintf(line, sizeof(line), "  edge %s%u event %u -> %s %u action %u\n", edge.retail ? "retail " : "",
                    edge.from, edge.event, (edge.action >= kGroupShow && edge.action <= kGroupToggle) || edge.action == kGroupNextClip ? "group" : "node", edge.to, edge.action);
      out += line;
    }
    for (size_t g = 0; g < std::max(area.groups.size(), area.groupShown.size()); ++g) {
      size_t shown = 0, size = 0;
      if (g < area.groups.size()) {
        for (const size_t i : area.groups[g]) {
          shown += area.items[i].shown;
        }
        size = area.groups[g].size();
      }
      const int set = g < area.groupShown.size() ? area.groupShown[g] : -1;
      std::snprintf(line, sizeof(line), "  group %zu: %zu of %zu shown%s\n", g, shown, size,
                    set < 0 ? "" : set ? " (script: show)" : " (script: hide)");
      out += line;
    }
  }
  return out;
}

int SetGroupShown(uint32_t group, bool shown) {
  int count = 0;
  for (auto& [mrea, area] : Areas()) {
    if (group < area.groupShown.size()) {
      area.groupShown[group] = shown ? 1 : 0;
    }
    if (group < area.groups.size()) {
      for (const size_t i : area.groups[group]) {
        area.items[i].shown = shown;
        ++count;
      }
    }
  }
  return count;
}

int GroupShown(uint32_t mrea, uint32_t group) {
  const auto area = Areas().find(mrea);
  return area == Areas().end() || group >= area->second.groupShown.size() ? -1 : area->second.groupShown[group];
}

void ResetScriptState() {
  for (auto& [mrea, area] : Areas()) {
    area.animTime = 0.;
    area.clock = 0.;
    area.pending.clear();
    for (Placed& item : area.items) {
      item.shown = item.active;
      item.wasShown = item.shown;
      item.clip = 0;
      item.clipStart = 0.;
      item.following = false;
      item.alpha = 1.f;
      item.modulated = false;
      item.modulation = CModelFlags::Normal();
      if (item.driven) {
        item.startAt = 1e300;
        item.driverValue = item.driverSpline.Eval(item.driverSpline.FirstTime());
      }
    }
    ResetNodes(area);
  }
}

std::string At(const CVector3f& point, float margin) {
  std::string out;
  char line[160];
  for (const auto& [mrea, area] : Areas()) {
    for (const Placed& item : area.items) {
      const Model& model = area.models[item.model];
      // Bounds are worked out as items are drawn: one that is not (hidden, off its layer, or
      // posed again since) gets them here, once its area is placed and its model loaded.
      if (!item.bounded && (!area.placed || !model.loaded)) {
        continue;
      }
      const CAABox bounds = item.bounded ? item.bounds : model.bounds.GetTransformedAABox(item.xf);
      const CVector3f lo = bounds.GetMinPoint();
      const CVector3f hi = bounds.GetMaxPoint();
      if (point.GetX() < lo.GetX() - margin || point.GetX() > hi.GetX() + margin ||
          point.GetY() < lo.GetY() - margin || point.GetY() > hi.GetY() + margin ||
          point.GetZ() < lo.GetZ() - margin || point.GetZ() > hi.GetZ() + margin) {
        continue;
      }
      std::snprintf(line, sizeof(line), "%08X in %08X: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)%s\n", model.id,
                    mrea, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ(),
                    model.hidden ? " hidden" : "");
      out += line;
    }
  }
  return out;
}

int SetHidden(uint32_t id, bool hidden) {
  int count = 0;
  for (auto& [mrea, area] : Areas()) {
    for (Model& model : area.models) {
      if (id == 0 || model.id == id) {
        model.hidden = hidden;
        ++count;
      }
    }
  }
  return count;
}

uint32_t Pick(const CVector3f& origin, const CVector3f& direction, std::string& out) {
  struct Hit {
    float key; // distance to the box, or its volume when the origin is inside
    bool inside;
    uint32_t mrea;
    const Placed* item;
  };
  std::vector< Hit > hits;
  const float o[3] = {origin.GetX(), origin.GetY(), origin.GetZ()};
  const float d[3] = {direction.GetX(), direction.GetY(), direction.GetZ()};
  for (const auto& [mrea, area] : Areas()) {
    for (const Placed& item : area.items) {
      if (!item.bounded || area.models[item.model].hidden || !item.shown) {
        continue;
      }
      const CVector3f lo = item.bounds.GetMinPoint();
      const CVector3f hi = item.bounds.GetMaxPoint();
      const float l[3] = {lo.GetX(), lo.GetY(), lo.GetZ()};
      const float h[3] = {hi.GetX(), hi.GetY(), hi.GetZ()};
      float enter = -3.4e38f;
      float leave = 3.4e38f;
      bool miss = false;
      for (int axis = 0; axis < 3 && !miss; ++axis) {
        if (std::fabs(d[axis]) < 1e-8f) {
          miss = o[axis] < l[axis] || o[axis] > h[axis];
          continue;
        }
        float t0 = (l[axis] - o[axis]) / d[axis];
        float t1 = (h[axis] - o[axis]) / d[axis];
        if (t0 > t1) {
          std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
      }
      if (miss || enter > leave || leave < 0.f) {
        continue;
      }
      const bool inside = enter <= 0.f;
      hits.push_back({inside ? (h[0] - l[0]) * (h[1] - l[1]) * (h[2] - l[2]) : enter, inside, mrea, &item});
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    return a.inside != b.inside ? !a.inside : a.key < b.key;
  });
  constexpr size_t kLines = 12;
  char line[200];
  for (size_t i = 0; i < hits.size() && i < kLines; ++i) {
    const Hit& hit = hits[i];
    const CVector3f lo = hit.item->bounds.GetMinPoint();
    const CVector3f hi = hit.item->bounds.GetMaxPoint();
    const uint32_t id = Areas()[hit.mrea].models[hit.item->model].id;
    if (hit.inside) {
      std::snprintf(line, sizeof(line), "%08X in %08X: around the camera, (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)\n",
                    id, hit.mrea, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ());
    } else {
      std::snprintf(line, sizeof(line), "%08X in %08X: %.1f m, (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)\n", id,
                    hit.mrea, hit.key, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ());
    }
    out += line;
    if (hit.item->glows) {
      out.pop_back();
      std::snprintf(line, sizeof(line), ", glow %g %g %g\n", hit.item->glow[0], hit.item->glow[1], hit.item->glow[2]);
      out += line;
    }
  }
  if (hits.size() > kLines) {
    std::snprintf(line, sizeof(line), "and %zu more\n", hits.size() - kLines);
    out += line;
  }
  return hits.empty() ? 0 : Areas()[hits[0].mrea].models[hits[0].item->model].id;
}

std::string MaterialLine(const CCubeModel* cube, uint32_t id, int i) {
  char line[320];
  const uint flags = cube->GetMaterialByIndex(i).GetFlags();
  float v[19];
  uint wrap = 0;
  float lightScale[2];
  uint cubeMap = 0;
  const int floats = cube->PortReadPBRMaterial(i, v, &wrap, lightScale, &cubeMap);
  const bool wraps = wrap != 0x55555555;
  const bool scaled = lightScale[0] != 1.f || lightScale[1] != 1.f;
  const char* const tag = CCubeModel::PortRecordTag(floats, wrap, scaled, cubeMap);
  // What the console's `roomgeo mat` put in place is what gets drawn, so show that.
  int shown = floats;
  bool overridden = false;
  for (const MaterialValue& value : sMaterialValues) {
    if (value.id == id && value.material == i) {
      v[value.field] = value.value;
      overridden = true;
      shown = std::max(shown, value.field < 6 ? 6 : value.field < 8 ? 8 : 19);
    }
  }
  int used = std::snprintf(line, sizeof(line), "%d: flags %08X %s%s%s, record %s", i, flags,
                           (flags & kStateFlag_PortPBR) != 0 ? "PBR" : "TEV",
                           (flags & kStateFlag_DepthSorting) != 0 ? " blended" : "",
                           (flags & kStateFlag_AlphaTest) != 0 ? " cutout" : "", tag);
  if (shown > 0 && used < int(sizeof(line))) {
    used += std::snprintf(line + used, sizeof(line) - used, ", emissive %g %g %g, backlight %g %g %g", v[0], v[1],
                          v[2], v[3], v[4], v[5]);
  }
  if (shown >= 8 && used < int(sizeof(line))) {
    used += std::snprintf(line + used, sizeof(line) - used, ", height %g, mode %g", v[6], v[7]);
  }
  if (shown >= 19 && used < int(sizeof(line))) {
    used += std::snprintf(line + used, sizeof(line) - used, ", kind %g, strength %g, params %g %g %g %g", v[13],
                          v[14], v[15], v[16], v[17], v[18]);
  }
  if (wraps && used < int(sizeof(line))) {
    // Map i's S and T modes: 0 clamp, 1 repeat, 2 mirror.
    used += std::snprintf(line + used, sizeof(line) - used, ", wrap");
    for (int m = 0; m < 8 && used < int(sizeof(line)); ++m) {
      used += std::snprintf(line + used, sizeof(line) - used, " %u%u", (wrap >> (m * 4)) & 3,
                            (wrap >> (m * 4 + 2)) & 3);
    }
  }
  if (cubeMap != 0 && used < int(sizeof(line))) {
    used += std::snprintf(line + used, sizeof(line) - used, ", cube %08X", cubeMap);
  }
  if (overridden && used < int(sizeof(line))) {
    std::snprintf(line + used, sizeof(line) - used, " (overridden)");
  }
  return line;
}

namespace {

// The loaded model with this file id: an area's (room geometry), else one that has drawn
// since the draw log went on (an actor's, a character's, the viewmodel's).
const CCubeModel* FindCube(uint32_t id) {
  for (const auto& [mrea, area] : Areas()) {
    for (const Model& model : area.models) {
      const CCubeModel* const cube = model.id == id ? CubeModel(model) : nullptr;
      if (cube != nullptr) {
        return cube;
      }
    }
  }
  return CCubeModel::PortFindModel(id);
}

} // namespace

std::string Owner(const CCubeModel* cube) {
  for (const auto& [mrea, area] : Areas()) {
    for (const Model& model : area.models) {
      if (cube != nullptr && CubeModel(model) == cube) {
        char text[40];
        std::snprintf(text, sizeof(text), "roomgeo %08X", mrea);
        return text;
      }
    }
  }
  return {};
}

std::string Materials(uint32_t id) {
  const CCubeModel* const cube = FindCube(id);
  if (cube == nullptr) {
    return {};
  }
  std::string out;
  const int count = int(cube->PortMaterialCount());
  for (int i = 0; i < count; ++i) {
    out += MaterialLine(cube, id, i);
    out += '\n';
  }
  return out;
}

bool SetMaterialValue(uint32_t id, int material, int field, float value) {
  if (material < 0 || field < 0 || field >= 19) {
    return false;
  }
  const CCubeModel* const found = FindCube(id);
  if (found == nullptr || uint(material) >= found->PortMaterialCount()) {
    return false;
  }
  for (MaterialValue& entry : sMaterialValues) {
    if (entry.id == id && entry.material == material && entry.field == field) {
      entry.value = value;
      BindMaterialValues();
      return true;
    }
  }
  sMaterialValues.push_back({id, material, field, value});
  BindMaterialValues();
  return true;
}

int ClearMaterialValues() {
  const int count = int(sMaterialValues.size());
  sMaterialValues.clear();
  CCubeModel::PortClearPBROverrides();
  return count;
}

void Reset() {
  CCubeModel::PortClearPBROverrides();
  sPrefetched.clear(); // waits for any still reading: the files are about to change
  Areas().clear();
  sLods.clear();
  sLodsRead = false;
  ++sGeneration;
}

bool AreaLights() {
  if (sAreaLights < 0) {
    sAreaLights = port::EnvFlag("MP_ROOM_GEO_AREA_LIGHTS") ? 1 : 0;
  }
  return sAreaLights != 0;
}

void SetAreaLights(bool on) { sAreaLights = on ? 1 : 0; }

float MinPixels() {
  if (sMinPixels < 0.f) {
    sMinPixels = std::max(0.f, port::EnvFloat("MP_ROOM_GEO_MIN_PX", 0.f));
  }
  return sMinPixels;
}

void SetMinPixels(float pixels) { sMinPixels = std::max(0.f, pixels); }

float LodDistance() {
  if (sLodDistance < 0.f) {
    sLodDistance = std::max(0.f, port::EnvFloat("MP_ROOM_GEO_LOD", 1.f));
  }
  return sLodDistance;
}

void SetLodDistance(float scale) { sLodDistance = std::max(0.f, scale); }

void LodStats(int& levels, int& loaded, int& drawnCoarse) {
  levels = loaded = 0;
  for (const auto& [mrea, area] : Areas()) {
    levels += int(area.levels);
    loaded += int(area.levelsLoaded);
  }
  drawnCoarse = sDrawnCoarseLast;
}

void SetMergedDraws(bool on) { sMergedDraws = on; }

bool MergedDraws() { return sMergedDraws; }

void SetFrontToBack(bool on) { sFrontToBack = on; }
void SetOffscreenCasters(bool on) { sOffscreenCasters = on; }
bool OffscreenCasters() { return sOffscreenCasters; }
uint32_t OffscreenCasterCount() { return sLastCasterCount; }

bool FrontToBack() { return sFrontToBack; }

void SetDepthPrepass(bool on) { sDepthPrepass = on; }

bool DepthPrepass() { return sDepthPrepass; }

void SetMode(Mode mode) {
  sMode = int(mode);
  if (mode == Mode::Off) {
    Reset();
  }
}

void SetBuffersReady(bool ready) { sBuffersReady = ready; }
bool BuffersReady() { return sBuffersReady; }

void SetResident(bool resident) { sResident = resident; }

bool Resident() { return sResident; }

Mode GetMode() {
  if (!sBuffersReady) {
    return Mode::Off;
  }
  if (sMode < 0) {
    const char* const env = port::EnvString("MP_ROOM_GEO");
    sMode = int(env == nullptr || env[0] == '\0' ? Mode::Replace
                : env[0] == '0'                  ? Mode::Off
                : env[0] == 'o'                  ? Mode::Overlay
                                                 : Mode::Replace);
  }
  return Mode(sMode);
}

void Stats(int& areaCount, int& instances, int& models, int& loaded, int& drawn) {
  areaCount = instances = models = loaded = 0;
  for (const auto& [mrea, area] : Areas()) {
    if (!area.hasFile) {
      continue;
    }
    ++areaCount;
    instances += int(area.items.size());
    models += int(area.models.size());
    for (const Model& model : area.models) {
      loaded += model.loaded ? 1 : 0;
    }
  }
  drawn = sDrawnLast;
}

} // namespace PortRoomGeo
