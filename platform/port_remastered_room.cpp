// Room environment writer. See port_remastered_room.h.
//
// A port of build/mpr/roomtools/envwrite.py (with roomlib.py, gcres.py and
// ltpb.cpp). Everything read here is untrusted file content: every read is
// bounds checked and a malformed file fails its room, never the process.
//
// This file is the writer itself (envwrite.py); the parts it reads with are in
// port_remastered_room_file.cpp, port_remastered_room_retail.cpp and
// port_remastered_room_scripts.cpp, shared through port_remastered_room_internal.h.

#include "port_remastered_room_internal.h"

namespace PortRemastered {
namespace RoomDetail {
namespace {

// ---------------------------------------------------------------------------
// The writer (envwrite.py)
// ---------------------------------------------------------------------------

struct RoomData {
  std::string name;
  const Pak* pak = nullptr;
  std::vector<uint8_t> bytes;  // the ROOM file
  Room room;
  Id16 id{};                   // the ROOM asset's id, as property bytes (Python's bytes_le)
};

// A room's BloomEffect; SLdrBloomEffect's threshold when the component leaves it out.
struct BloomData {
  bool present = false;
  float threshold = 0.9f;
  std::vector<float> tints; // RGBA
};

// A ColorGrade: the retail layer it is on (-1 = always), its hint's fades, whether it is
// requested from the start, its priority, what turns it on and off, the LUT (33^3 RGBA8,
// red fastest).
struct GradeData {
  int32_t layer = -1;
  float fadeIn = 0, fadeOut = 0;
  bool on = true;
  bool master = false;  // the world's master area's (see PortRoomEnv::Grade::master)
  int32_t priority = 50;
  std::vector<PortRoomGeo::Link> links;
  std::vector<uint8_t> lut;
};

// A Backlight: as a GradeData, with the coefficients in place of the LUT.
struct BacklightData {
  int32_t layer = -1;
  float fadeIn = 1, fadeOut = 1;
  bool on = false;
  bool master = false;
  int32_t priority = 50;
  float top = 1, back = 1;
  std::vector<PortRoomGeo::Link> links;
};

// A VolumetricFogHint with its fog, in retail axes (see PortRoomEnv::FogHint).
struct FogData {
  int32_t layer = -1;
  // The raw fade splines; empty: no interpolation, the fog changes at once.
  std::vector<uint8_t> fadeIn, fadeOut;
  bool on = false;
  bool master = false;
  int32_t priority = 50;
  float s[10] = {};  // range, scatter, absorb, m1z, decay, attenSlope, attenBias, noiseFreq, noiseStrength, lightCap
  float wind[3] = {};
  bool useScriptWind = false, noProbe = false;
  float colorB[4] = {1, 1, 1, 1}, colorA[4] = {0, 0, 0, 1};
  float lut[64] = {};
  std::vector<PortRoomGeo::Link> links;
};

// A VolumetricFogRegion as CVolumetricFogRegionGOC::AddRegion and RebuildPositionalData make
// its state, over retail world space (see PortRoomEnv::FogRegion).
struct FogRegionData {
  int32_t layer = -1;
  bool on = false;
  uint8_t fluid = 0; // 0 always, 1 while the camera is in a fluid, 2 while it is not
  bool hasColor = false, hasCap = false;
  float m[3][4] = {};
  float edgeScale[3] = {}, mult = 1, edgeBias[3] = {}, cap = 0;
  float color[4] = {};
  float density = 0;
  float box[6] = {}; // min, max
  std::vector<PortRoomGeo::Link> links;
  // What a transition starts from (CVolumetricFogRegionGOC::GetTransitionState).
  float distance = 250, transmittance = 0.01f;
  bool subtract = false;
};

// A VolumetricFogRegionTransition (see PortRoomEnv::FogTransition).
struct FogTransitionData {
  uint32_t region = 0; // index into the room's FogRegionData
  int32_t layer = -1;
  bool on = false, autoStart = false, loop = false;
  uint8_t select = 0;
  float distance = 250, transmittance = 0.01f, color[4] = {1, 1, 1, 1}, cap = 0;
  std::vector<uint8_t> phase; // CMayaSpline bytes
  std::vector<PortRoomGeo::Link> links;
};

// A directional LightDynamic (see PortRoomEnv::Sun).
struct SunData {
  int32_t layer = -1;
  bool on = false;
  float toSun[3] = {}; // retail world, unit length
  float color[3] = {}; // linear colour times intensity
  uint32_t group = PortRoomGeo::kNoGroup; // the script group that shows and hides it
  bool casts = false; // LightFlags a103d675 (SLdrLightFlags byte 0): the light builds a shadow map
};

// A point or spot LightDynamic (see PortRoomEnv::RoomLights), as SLdrLightDynamic::Load and
// NLightLoaders::build_light read it (kb func/light-dynamic.md).
struct PointLightData {
  int32_t layer = -1;
  bool on = false;
  bool spot = false;
  uint8_t falloff = 3;         // 1 linear, 2 squared, 3 smooth cubic
  float pos[3] = {};           // retail world
  float toLight[3] = {};       // spots: unit, against the beam
  float color[3] = {};         // linear colour times intensity
  float nearFar[2] = {0.25f, 1.f};  // times the entity's scale x and y
  float cone[2] = {0.f, 45.f}; // inner, outer full angles in degrees
  uint32_t group = PortRoomGeo::kNoGroup;
  std::vector<PortRoomGeo::Link> links;  // show/hide and the timeline's actions (LightAct)
  // The animated path: timeline flags, its falloff, whether its end deactivates it (its
  // finished event -> ICMP/ICTV on itself), the entity's scale x and y (for near and far),
  // and the splines' bytes (PortRoomEnv::kLightSplines; empty: absent).
  bool animated = false, playing = false, loop = false, offAtStart = false;
  uint8_t animFalloff = 3;
  float length = 0.f;
  float scale[2] = {1.f, 1.f};
  float intensity = 0.f; // the static colour and intensity, for a timeline that lacks a spline
  float rgb[3] = {};
  std::vector<uint8_t> splines[PortRoomEnv::kLightSplines];
};

struct Placement {
  Vec3 pos{};  // GameCube world coordinates, once the world shift is added
};

// A world master room's Skybox (see PortRoomGeo::Instance::worldSky): its model, turn and
// scale (Remastered axes; the master and its rooms share them) and its radiance.
struct WorldSky {
  Id16 model{};
  Vec3 rot{}, scale{};
  float radiance[3] = {};
};

// A Skybox's colour times its intensity (PortRoomGeo::Instance::skyRadiance).
void SkyRadiance(const Room& room, const Component& c, float out[3]) {
  const auto f = room.Flat(c);
  const auto intensity = f.find(kPropSkyboxIntensity);
  const float level =
      intensity != f.end() && intensity->second.size == 4 ? ReadLEFloat(room.Bytes(intensity->second)) : 1.f;
  const auto colour = f.find(kPropSkyboxColor);
  for (int i = 0; i < 3; ++i) {
    Span s;
    const float channel = colour != f.end() && colour->second.size >= 12 ? ReadLEFloat(room.Bytes(colour->second) + 4 * i)
                          : room.Nested(c, {kPropSkyboxColor, kPropColorChannel[i]}, s) && s.size == 4
                              ? ReadLEFloat(room.Bytes(s))
                              : 1.f;
    out[i] = std::isfinite(channel * level) ? std::max(channel * level, 0.f) : 0.f;
  }
}

class Writer {
public:
  Writer(const RoomPak& master, const std::vector<RoomPak>& rooms, const std::vector<RoomPak>& others,
         const RoomIO& io)
      : m_master(master), m_rooms(rooms), m_others(others), m_io(io) {}

  bool Run(uint32_t mlvl, int& written, std::string& error);

private:
  void Log(const std::string& line) const {
    if (m_io.log) {
      m_io.log(line);
    }
  }

  static const PakAsset* FirstOfType(const Pak& pak, uint32_t type) {
    for (const PakAsset& a : pak.Assets()) {
      if (a.type == type) {
        return &a;
      }
    }
    return nullptr;
  }
  bool ReadRoomFile(const RoomPak& rp, RoomData& out, std::string& error) const;
  bool LoadAreas(uint32_t mlvl, std::string& error);
  // A REFL or TXTR by its uuid in property byte order: the room's own pak, the master's, then the others.
  bool FindResource(const uint8_t* propertyId, uint32_t type, const RoomPak& home, std::vector<uint8_t>& out,
                    const Pak** foundIn, Id16* foundId) const;

  // The room's baked lightmap (BLIT's TXTR) as BC6H layers, read once per room: the
  // geometry's lookups index it and the roomenv carries it. `lit` is how many of the
  // geometry's instances have a lookup (set by WriteGeometry).
  struct LightmapData {
    std::string room;
    bool ok = false;
    PortRemastered::TxtrLayersBc6h tex;
    size_t lit = 0;
  };
  LightmapData& Lightmap(const RoomData& r) const;
  mutable LightmapData m_lightmap;

  struct Match {
    uint32_t mrea = 0;
    Mat34 a{};
    double err = 0;
    const Area* area = nullptr;
  };
  bool MatchRoom(const RoomData& r, const std::map<std::string, Placement>& placed, Match& out) const;
  void Tonemap(const RoomData& r, float out[5]) const;
  void Exposure(const RoomData& r, float out[5], std::vector<PortRoomEnv::FluidHint>* fluid = nullptr) const;
  void ReadBloom(const RoomData& r, BloomData& out) const;
  // The room's global ColorGrades in component order; `area` gives their retail layers.
  void ReadGrades(const RoomData& r, const Area* area, std::vector<GradeData>& out,
                  const SceneryScripts* given = nullptr) const;
  // The room's Backlight hints, likewise.
  void ReadBacklights(const RoomData& r, const Area* area, std::vector<BacklightData>& out,
                      const SceneryScripts* given = nullptr) const;
  // The room's fog hints, heights in Remastered axes (the writer applies the room's shift).
  void ReadFogs(const RoomData& r, const Area* area, std::vector<FogData>& out,
                const SceneryScripts* given = nullptr) const;
  // The room's fog regions, placed by the area's transform `xf`.
  void ReadFogRegions(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                      std::vector<FogRegionData>& out, std::vector<FogTransitionData>& transitions) const;
  void ReadSuns(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf, std::vector<SunData>& out) const;
  void ReadPointLights(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                       std::vector<PointLightData>& out) const;
  bool Grid(const RoomPak& rp, const Vec3& shift, const std::vector<Vec3>& check, std::vector<uint8_t>& out,
            std::string& note) const;
  // Tells RoomIO::retailModel where the room's retail models stand (RetailPlacement), and
  // keeps the entities that placed one in m_retailEntities, for WriteGeometry to leave out.
  void PairRetailModels(const RoomData& r, const Area& area);
  std::set<int> m_retailEntities;
  // Entities carrying a model under a retail-style id (10000000-...-XXXXXXXX) that no retail
  // object of the area names: Remastered's own models standing in for retail objects (Central
  // Dynamo's floor grates). WriteGeometry draws them, though they lack kPropActorAdded.
  std::set<int> m_syntheticEntities;
  // The retail ids the room's entities carry (models it keeps), and where its entities stand
  // (GC world space): what WriteGeometry checks before hiding a retail object.
  std::set<uint32_t> m_roomRetailIds;
  std::vector<Vec3> m_entityPositions;
  // The room's static geometry (its ModCon components), as "<MREA id>.roomgeo".
  void WriteGeometry(const RoomData& r, uint32_t mrea, const Area& area);
  // The room's liquid surfaces (its water and lava render volumes), as "<MREA id>.roomliquid".
  void WriteLiquids(const RoomData& r, uint32_t mrea);
  std::string WriteRoom(const RoomData& r, const std::map<std::string, Placement>& placed, const Vec3& shift,
                        const float tonemap[5], const BloomData& bloom, const std::vector<GradeData>& worldGrades,
                        const std::vector<BacklightData>& worldBacklights, const std::vector<FogData>& worldFogs,
                        const std::vector<PortRoomEnv::WorldVolume>& worldVolumes, const Match& m, int& written,
                        std::string& matched);

  const RoomPak& m_master;
  const std::vector<RoomPak>& m_rooms;
  const std::vector<RoomPak>& m_others;
  const RoomIO& m_io;
  std::vector<Area> m_areas;
  std::vector<WorldSky> m_worldSkies;  // the master's (Run), for rooms with no Skybox of their own
};

double Spread(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
  double sum = 0;
  for (const Vec3& p : a) {
    double best = 1e300;
    for (const Vec3& q : b) {
      best = std::min(best, Distance(q, p));
    }
    sum += best;
  }
  return sum / double(a.size());
}

bool Writer::ReadRoomFile(const RoomPak& rp, RoomData& out, std::string& error) const {
  if (rp.pak == nullptr) {
    error = "no pak";
    return false;
  }
  const PakAsset* asset = FirstOfType(*rp.pak, FourCC("ROOM"));
  if (asset == nullptr) {
    error = "no ROOM";
    return false;
  }
  if (!rp.pak->ReadAsset(*asset, out.bytes, error)) {
    return false;
  }
  out.name = rp.name;
  out.pak = rp.pak;
  out.id = SwapUuid(asset->id.data());
  return out.room.Parse(out.bytes, error);
}

bool Writer::LoadAreas(uint32_t mlvl, std::string& error) {
  std::vector<uint8_t> mlvlData;
  if (!m_io.retail || !m_io.retail(FourCC("MLVL"), mlvl, mlvlData)) {
    error = "no retail MLVL";
    return false;
  }
  std::vector<std::pair<uint32_t, Mat34>> list;
  if (!ReadMlvl(mlvlData, list) || list.empty()) {
    error = "the retail MLVL is unreadable";
    return false;
  }
  for (const auto& [mrea, xf] : list) {
    Area a;
    a.mrea = mrea;
    a.xf = xf;
    std::vector<uint8_t> data;
    if (!m_io.retail(FourCC("MREA"), mrea, data) || !ReadScly(data, a.objects)) {
      Log("  retail area " + std::to_string(mrea) + ": doors unreadable");
      a.objects.clear();
    }
    for (const ScriptObject& o : a.objects) {
      if (o.type == 3) {
        a.doors.push_back(o.pos);
      }
    }
    m_areas.push_back(std::move(a));
  }
  return true;
}

Writer::LightmapData& Writer::Lightmap(const RoomData& r) const {
  LightmapData& lm = m_lightmap;
  if (lm.room == r.name) {
    return lm;
  }
  lm = LightmapData{};
  lm.room = r.name;
  const Blit& blit = r.room.Lighting();
  if (!blit.HasLightmap()) {
    return lm;
  }
  std::vector<uint8_t> txtr;
  std::string error;
  if (!FindResource(blit.lightmap.data(), FourCC("TXTR"), RoomPak{r.name, r.pak}, txtr, nullptr, nullptr)) {
    Log("  " + r.name + ": lightmap texture not found");
  } else if (!PortRemastered::ReadTxtrLayersBc6h(txtr.data(), txtr.size(), lm.tex, error)) {
    Log("  " + r.name + ": lightmap not used: " + error);
  } else if (lm.tex.layers.size() < 4) {
    Log("  " + r.name + ": lightmap not used: " + std::to_string(lm.tex.layers.size()) + " layers");
  } else {
    lm.ok = true;
  }
  return lm;
}

bool Writer::FindResource(const uint8_t* propertyId, uint32_t type, const RoomPak& home,
                          std::vector<uint8_t>& out, const Pak** foundIn, Id16* foundId) const {
  const Id16 id = SwapUuid(propertyId);
  std::vector<const Pak*> order{home.pak, m_master.pak};
  for (const RoomPak& r : m_rooms) {
    order.push_back(r.pak);
  }
  for (const RoomPak& r : m_others) {
    order.push_back(r.pak);
  }
  for (const Pak* pak : order) {
    if (pak == nullptr) {
      continue;
    }
    const PakAsset* asset = pak->Find(id);
    std::string error;
    if (asset != nullptr && asset->type == type && pak->ReadAsset(*asset, out, error)) {
      if (foundIn != nullptr) {
        *foundIn = pak;
      }
      if (foundId != nullptr) {
        *foundId = id;
      }
      return true;
    }
  }
  return false;
}

bool Writer::MatchRoom(const RoomData& r, const std::map<std::string, Placement>& placed, Match& out) const {
  std::vector<Vec3> doors;
  for (const Component* c : r.room.Of(kDoorMP1)) {
    Vec3 pos, rot, scale;
    if (r.room.Xform(*c, pos, rot, scale)) {
      doors.push_back(MulR2G(pos));
    }
  }
  auto transformed = [&](const Mat34& a) {
    std::vector<Vec3> t;
    for (const Vec3& d : doors) {
      t.push_back(Apply(a, d));
    }
    return t;
  };
  auto err = [&](const Mat34& a, const std::vector<Vec3>& g) {
    return !doors.empty() && !g.empty() ? Spread(transformed(a), g) : 0.0;
  };
  // Both ways, or a one-door room fits a neighbour that has that door among its own.
  auto both = [&](const Mat34& a, const std::vector<Vec3>& g) { return err(a, g) + Spread(g, transformed(a)); };
  const auto it = placed.find(r.name);
  if (it != placed.end()) {
    // Areas can share an origin (the Frigate's do), so the doors pick among those.
    const Area* best = nullptr;
    double bestKey = 0;
    for (const Area& t : m_areas) {
      if (Distance({t.xf[0][3], t.xf[1][3], t.xf[2][3]}, it->second.pos) >= 0.5) {
        continue;
      }
      const double key = !doors.empty() && !t.doors.empty() ? both(t.xf, t.doors)
                         : doors.size() == t.doors.size()   ? 0.0
                                                            : 99.0;
      if (best == nullptr || key < bestKey) {
        best = &t;
        bestKey = key;
      }
    }
    if (best != nullptr) {
      out = {best->mrea, best->xf, err(best->xf, best->doors), best};
      return true;
    }
  }
  // Not placed: fall back on the doors alone.
  if (doors.empty()) {
    return false;
  }
  const Area* best = nullptr;
  double bestErr = 0;
  for (const Area& t : m_areas) {
    if (t.doors.empty()) {
      continue;
    }
    const double e = err(t.xf, t.doors);
    if (best == nullptr || e < bestErr) {
      best = &t;
      bestErr = e;
    }
  }
  if (best == nullptr) {
    return false;
  }
  Log("  " + r.name + ": matched by its doors");
  out = {best->mrea, best->xf, bestErr, best};
  return true;
}

// The first Tonemap component's values, where the room has one; `out` keeps the rest.
void Writer::Tonemap(const RoomData& r, float out[5]) const {
  for (const Component* c : r.room.Of(kTonemap)) {
    const auto f = r.room.Flat(*c);
    for (int i = 0; i < 5; ++i) {
      const auto it = f.find(kPropTonemap[i]);
      if (it != f.end() && it->second.size >= 4) {
        out[i] = ReadLEFloat(r.room.Bytes(it->second));
      }
    }
    break;
  }
}

// The range of exposure values the room's auto exposure is held to, the bias it adds, the
// sigma it eases by and where in the range the static exposure sits, or 0, 0, 0, 32, 0.5
// for a room without auto exposure. A room can have several hints: the plain
// one for the whole room is wanted, not those with a mode (they are for a state of the
// room, such as a cutscene) or a volume of their own. Hints with mode 1 or 2 (active while
// the camera is inside a fluid / once it has left one) go to `fluid` with their mode. A value the hint leaves out is
// SLdrAutoExposureHint's default.
//
// The two hints a CAutoExposureHintMP1 swaps between (role 1 for the one its 0x9ea1739d link
// reaches, 2 for its 0x4ff221ac link; which is the normal view and which the thermal one is
// inferred from the data (unverified)) go to `fluid` whatever their mode, with the
// instance's number in `pair`, and out of the plain-hint pick.
void Writer::Exposure(const RoomData& r, float out[5], std::vector<PortRoomEnv::FluidHint>* fluid) const {
  out[0] = out[1] = out[2] = 0.f;
  out[3] = 32.f;
  out[4] = 0.5f;
  int best = 0;
  std::map<const Component*, std::pair<uint8_t, uint8_t>> paired;  // hint -> role, pair
  {
    const auto& comps = r.room.Components();
    int instances = 0, unresolved = 0;
    for (const Component* m : r.room.Of(kAutoExposureHintMP1)) {
      bool ok = true;
      for (const auto& l : ReadEntityLinkIds(r.room, size_t(m - comps.data()))) {
        if (l.first != kLinkExposureHintA && l.first != kLinkExposureHintB) {
          continue;
        }
        if (l.second < 0 || comps[size_t(l.second)].type != kAutoExposureHint || instances >= 255) {
          ok = false;
          continue;
        }
        paired[&comps[size_t(l.second)]] = {uint8_t(l.first == kLinkExposureHintA ? 1 : 2), uint8_t(instances)};
      }
      ++instances;
      unresolved += ok ? 0 : 1;
    }
    std::string modes;  // "A modes / B modes", for the import log
    for (int role = 1; role <= 2; ++role) {
      modes += role == 1 ? " (A modes" : "; B modes";
      for (const auto& [hint, rp] : paired) {
        if (rp.first == role) {
          const auto hf = r.room.Flat(*hint);
          const auto mi = hf.find(kPropHintMode);
          modes += " " + std::to_string(mi != hf.end() && mi->second.size >= 4
                                            ? int(ReadLE32(r.room.Bytes(mi->second)))
                                            : 0);
        }
      }
    }
    modes += ")";
    if (instances > 0) {
      Log("  " + r.name + ": exposure hint swap: " + std::to_string(instances) + " instance(s), " +
          std::to_string(paired.size()) + " hints" + modes +
          (unresolved > 0 ? ", WARNING " + std::to_string(unresolved) + " with an unresolved link" : ""));
    }
  }
  for (const Component* c : r.room.Of(kAutoExposureHint)) {
    const auto f = r.room.Flat(*c);
    const auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      return it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
    };
    const float range[2] = {value(kPropHintMin, -24.f), value(kPropHintMax, 24.f)};
    const float bias = value(kPropHintBias, 0.f);
    const float sigma = value(kPropHintSigma, 32.f);
    const float lerp = value(kPropHintStaticLerp, 0.5f);
    if (!std::isfinite(range[0]) || !std::isfinite(range[1]) || range[1] < range[0]) {
      continue;
    }
    Vec3 pos, rot, scale;
    const bool whole = !r.room.Xform(*c, pos, rot, scale) ||
                       (std::fabs(scale[0] - 1.0) < 1e-3 && std::fabs(scale[1] - 1.0) < 1e-3 &&
                        std::fabs(scale[2] - 1.0) < 1e-3);
    const auto modeIt = f.find(kPropHintMode);
    const int mode = modeIt != f.end() && modeIt->second.size >= 4 ? int(ReadLE32(r.room.Bytes(modeIt->second))) : 0;
    const auto pairIt = paired.find(c);
    if (mode == 1 || mode == 2 || pairIt != paired.end()) {
      if (fluid != nullptr) {
        PortRoomEnv::FluidHint h;
        h.mode = uint8_t(std::clamp(mode, 0, 255));
        if (pairIt != paired.end()) {
          h.role = pairIt->second.first;
          h.pair = pairIt->second.second;
        }
        h.exposure[0] = range[0];
        h.exposure[1] = range[1];
        h.bias = std::isfinite(bias) ? bias : 0.f;
        h.sigma = sigma >= 0.f && sigma < 10000.f ? sigma : 32.f;
        h.staticLerp = lerp >= 0.f && lerp <= 1.f ? lerp : 0.5f;
        fluid->push_back(h);
      }
      continue;
    }
    const int rank = modeIt != f.end() ? 1 : whole ? 3 : 2;
    if (rank > best) {
      best = rank;
      out[0] = range[0];
      out[1] = range[1];
      out[2] = std::isfinite(bias) ? bias : 0.f;
      out[3] = sigma >= 0.f && sigma < 10000.f ? sigma : 32.f;
      out[4] = lerp >= 0.f && lerp <= 1.f ? lerp : 0.5f;
    }
  }
}

// The first BloomEffect component's values, where the room has one; `out` keeps the rest.
void Writer::ReadBloom(const RoomData& r, BloomData& out) const {
  for (const Component* c : r.room.Of(kBloomEffect)) {
    const auto f = r.room.Flat(*c);
    auto it = f.find(kPropBloomThreshold);
    if (it != f.end() && it->second.size >= 4) {
      out.threshold = ReadLEFloat(r.room.Bytes(it->second));
    }
    it = f.find(kPropBloomTints);
    if (it != f.end() && it->second.size >= 4) {
      const uint8_t* const p = r.room.Bytes(it->second);
      const uint32_t count = ReadLE32(p);
      if (count <= 16 && it->second.size >= 4 + count * 16) {
        out.tints.assign(count * 4, 0.f);
        for (uint32_t i = 0; i < count * 4; ++i) {
          out.tints[i] = ReadLEFloat(p + 4 + i * 4);
        }
      }
    }
    // UberPost accumulates five tints from the list with no length check
    // (FUN_006b5310; kb topic/bloom-no-bloomeffect.md): missing ones add black (inferred).
    if (out.tints.size() < 20) {
      out.tints.resize(20, 0.f);
    }
    out.present = true;
    break;
  }
}

void Writer::ReadGrades(const RoomData& r, const Area* area, std::vector<GradeData>& out,
                        const SceneryScripts* given) const {
  const std::vector<const Component*> grades = r.room.Of(kColorGrade);
  if (grades.empty()) {
    return;
  }
  std::map<int, const Component*> hints;  // by entity
  for (const Component* c : r.room.Of(kColorGradeHint)) {
    if (c->entity >= 0) {
      hints.emplace(c->entity, c);
    }
  }
  // The master's are driven by its own script objects (MasterScripts), a room's by retail's.
  SceneryScripts scripts;
  if (given != nullptr) {
    scripts = *given;
  } else if (area != nullptr) {
    scripts = MatchScripts(r.room, *area);
  }
  const RoomPak home{r.name, r.pak};
  for (const Component* c : grades) {
    const auto h = hints.find(c->entity);
    if (c->entity < 0 || h == hints.end()) {
      continue;
    }
    const auto hf = r.room.Flat(*h->second);
    const auto global = hf.find(kPropHintGlobal);
    const bool isGlobal = global != hf.end() && global->second.size >= 1 && r.room.Bytes(global->second)[0] != 0;
    // A local hint is requested only by its connections (OnRequest/OffRequest); one with
    // none that the port can follow never shows.
    std::vector<PortRoomGeo::Link> links;
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (!PortRoomEnv::PseudoSender(link.sender)) {
          link.sender &= 0x3ffffff;  // as CEntity sends it: no layer bits
        }
        links.push_back(link);
      }
    }
    const auto unresolved = scripts.unresolved.find(c->entity);
    if (unresolved != scripts.unresolved.end()) {
      Log("  " + r.name + ": " + (isGlobal ? "global" : "local") + " colour grade hint with " +
          std::to_string(unresolved->second) +
          " connection(s) not followed (sender type/event/action:" +
          (scripts.unresolvedHow.count(c->entity) != 0 ? scripts.unresolvedHow.at(c->entity) : std::string()) +
          "), " + std::to_string(links.size()) + " followed");
    }
    if (!isGlobal && links.empty()) {
      continue;
    }
    const auto f = r.room.Flat(*c);
    const auto lut = f.find(kPropGradeLut);
    std::vector<uint8_t> txtr;
    if (lut == f.end() || lut->second.size < 16 ||
        !FindResource(r.room.Bytes(lut->second), FourCC("TXTR"), home, txtr, nullptr, nullptr)) {
      Log("  " + r.name + ": colour grade LUT not found");
      continue;
    }
    GradeData g;
    uint32_t w = 0, hgt = 0, d = 0;
    std::string error;
    if (!DecodeTxtrVolumeRgba8(txtr.data(), txtr.size(), w, hgt, d, g.lut, error) || w != kGradeLutSize ||
        hgt != kGradeLutSize || d != kGradeLutSize) {
      Log("  " + r.name + ": colour grade LUT unreadable (" + error + ")");
      continue;
    }
    // Defaults as CGameHintBase's.
    auto fade = [&](uint32_t prop) {
      const auto it = hf.find(prop);
      return it != hf.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : 2.f;
    };
    g.fadeIn = fade(kPropHintFadeIn);
    g.fadeOut = fade(kPropHintFadeOut);
    Span priority;
    if (r.room.Nested(*h->second, {kPropHintPriority[0], kPropHintPriority[1]}, priority) && priority.size >= 4) {
      g.priority = int32_t(ReadLE32(r.room.Bytes(priority)));
    }
    // A global hint is requested while its entity is active; a local one once told to.
    g.on = isGlobal && r.room.Active(*h->second);
    g.links = std::move(links);
    const auto layer = scripts.layer.find(c->entity);
    g.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    out.push_back(std::move(g));
  }
}

void Writer::ReadBacklights(const RoomData& r, const Area* area, std::vector<BacklightData>& out,
                            const SceneryScripts* given) const {
  const std::vector<const Component*> lights = r.room.Of(kBacklight);
  if (lights.empty()) {
    return;
  }
  // The master's are driven by its own script objects (MasterScripts), a room's by retail's.
  SceneryScripts scripts;
  if (given != nullptr) {
    scripts = *given;
  } else if (area != nullptr) {
    scripts = MatchScripts(r.room, *area);
  }
  for (const Component* c : lights) {
    if (c->entity < 0) {
      continue;
    }
    const auto f = r.room.Flat(*c);
    const auto global = f.find(kPropBacklightGlobal);
    const bool isGlobal = global != f.end() && global->second.size >= 1 && r.room.Bytes(global->second)[0] != 0;
    std::vector<PortRoomGeo::Link> links;
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (!PortRoomEnv::PseudoSender(link.sender)) {
          link.sender &= 0x3ffffff;  // as CEntity sends it: no layer bits
        }
        links.push_back(link);
      }
    }
    const auto unresolved = scripts.unresolved.find(c->entity);
    if (unresolved != scripts.unresolved.end()) {
      Log("  " + r.name + ": " + (isGlobal ? "global" : "local") + " backlight hint with " +
          std::to_string(unresolved->second) + " connection(s) not followed (sender type/event/action:" +
          (scripts.unresolvedHow.count(c->entity) != 0 ? scripts.unresolvedHow.at(c->entity) : std::string()) + "), " +
          std::to_string(links.size()) + " followed");
    }
    if (!isGlobal && links.empty()) {
      continue;
    }
    BacklightData b;
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      const float v = it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    b.top = value(kPropBacklightTop, 1.f);
    b.back = value(kPropBacklightBack, 1.f);
    b.fadeIn = value(kPropBacklightFadeIn, 1.f);
    b.fadeOut = value(kPropBacklightFadeOut, 1.f);
    Span priority;
    if (r.room.Nested(*c, {kPropBacklightPriority[0], kPropBacklightPriority[1]}, priority) && priority.size >= 4) {
      b.priority = int32_t(ReadLE32(r.room.Bytes(priority)));
    }
    b.on = isGlobal && r.room.Active(*c);
    b.links = std::move(links);
    const auto layer = scripts.layer.find(c->entity);
    b.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    out.push_back(std::move(b));
  }
}

void Writer::ReadFogs(const RoomData& r, const Area* area, std::vector<FogData>& out,
                      const SceneryScripts* given) const {
  const std::vector<const Component*> hints = r.room.Of(kVolumetricFogHint);
  if (hints.empty()) {
    return;
  }
  std::map<int32_t, const Component*> fogOf;
  for (const Component* c : r.room.Of(kVolumetricFog)) {
    fogOf[c->entity] = c;
  }
  // The master's are driven by its own script objects (MasterScripts), a room's by retail's.
  SceneryScripts scripts;
  if (given != nullptr) {
    scripts = *given;
  } else if (area != nullptr) {
    scripts = MatchScripts(r.room, *area);
  }
  for (const Component* h : hints) {
    const auto fogIt = fogOf.find(h->entity);
    if (h->entity < 0 || fogIt == fogOf.end()) {
      continue;
    }
    const Component& fog = *fogIt->second;
    const auto hf = r.room.Flat(*h);
    const auto autoReq = hf.find(kPropFogAuto);
    const bool isGlobal = autoReq == hf.end() || autoReq->second.size < 1 || r.room.Bytes(autoReq->second)[0] != 0;
    std::vector<PortRoomGeo::Link> links;
    const auto linked = scripts.links.find(h->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (!PortRoomEnv::PseudoSender(link.sender)) {
          link.sender &= 0x3ffffff;
        }
        links.push_back(link);
      }
    }
    const auto unresolved = scripts.unresolved.find(h->entity);
    if (unresolved != scripts.unresolved.end()) {
      Log("  " + r.name + ": " + (isGlobal ? "global" : "local") + " fog hint with " +
          std::to_string(unresolved->second) + " connection(s) not followed (sender type/event/action:" +
          (scripts.unresolvedHow.count(h->entity) != 0 ? scripts.unresolvedHow.at(h->entity) : std::string()) + "), " +
          std::to_string(links.size()) + " followed");
    }
    if (!isGlobal && links.empty()) {
      continue;
    }
    FogData g;
    const auto ff = r.room.Flat(fog);
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = ff.find(prop);
      const float v = it != ff.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    auto vec4 = [&](uint32_t prop, float* dst) {
      const auto it = ff.find(prop);
      if (it != ff.end() && it->second.size >= 16) {
        for (int i = 0; i < 4; ++i) {
          const float v = ReadLEFloat(r.room.Bytes(it->second) + 4 * i);
          dst[i] = std::isfinite(v) ? v : dst[i];
        }
      }
    };
    auto flag = [&](uint32_t prop, bool fallback) {
      const auto it = ff.find(prop);
      return it != ff.end() && it->second.size >= 1 ? r.room.Bytes(it->second)[0] != 0 : fallback;
    };
    const float range = value(kPropFogRange, 250.f);
    g.s[0] = range > 0 ? range : 250.f;
    g.s[1] = value(kPropFogScatter, 1.f);
    g.s[2] = value(kPropFogAbsorb, 1.f);
    g.s[3] = value(kPropFogM1z, 0.95f);
    const float residual = value(kPropFogResidual, 0.01f);
    g.s[4] = residual > 0 && residual < 1 ? -std::log(residual) / g.s[0] : 0.f;
    // Attenuation by height: factor = slope * y + bias, between heights a and b (Remastered y;
    // retail's z is that plus the room's shift, which the writer folds into the bias).
    Span at;
    float a = 0, b = 0, slope = 0, bias = 1;
    auto atten = [&](uint32_t id, float& dst) {
      Span sp;
      if (r.room.Nested(fog, {kPropFogAtten, id}, sp) && sp.size >= 4) {
        const float v = ReadLEFloat(r.room.Bytes(sp));
        dst = std::isfinite(v) ? v : dst;
      }
    };
    atten(kPropFogAttenA, a);
    atten(kPropFogAttenB, b);
    if (r.room.Nested(fog, {kPropFogAtten, kPropFogAttenOn}, at) && at.size >= 1 && r.room.Bytes(at)[0] != 0) {
      float diff = b - a;
      if (std::fabs(diff) < 1e-5f) {
        diff = 0.001f;
      }
      slope = -1.f / diff;
      bias = 1.f + a / diff;
    }
    g.s[5] = slope;
    g.s[6] = bias;
    const float scale = value(kPropFogNoiseScale, 20.f);
    g.s[7] = std::fabs(scale) < 1e-5f ? 1.f : 1.f / scale;
    g.s[8] = value(kPropFogNoiseStrength, 1.f);
    g.s[9] = value(kPropFogLightCap, 3e37f);
    vec4(kPropFogColorA, g.colorA);
    const float intensity = value(kPropFogIntensity, 1.f);
    for (int i = 0; i < 3; ++i) {
      g.colorA[i] *= intensity;
    }
    vec4(kPropFogColorB, g.colorB);
    g.noProbe = flag(kPropFogNoProbe, false);
    Span ws;
    if (r.room.Nested(fog, {kPropFogWind, kPropFogWindOn}, ws) && ws.size >= 1) {
      g.useScriptWind = r.room.Bytes(ws)[0] != 0;
    }
    if (r.room.Nested(fog, {kPropFogWind, kPropFogWindVec}, ws) && ws.size >= 12) {
      const Vec3 v = MulR2G({ReadLEFloat(r.room.Bytes(ws)), ReadLEFloat(r.room.Bytes(ws) + 4), ReadLEFloat(r.room.Bytes(ws) + 8)});
      for (size_t i = 0; i < 3; ++i) {
        g.wind[i] = std::isfinite(v[i]) ? float(v[i]) : 0.f;
      }
    }
    // The density spline, sampled at (i/63)^2 * range. Without one it is the loader's empty
    // spline, which evaluates to 0: no fog.
    PortMayaSpline lut;
    Span ls;
    if (!r.room.Nested(fog, {kPropFogLut}, ls) || !lut.Load(r.room.Bytes(ls), ls.size)) {
      lut = PortMayaSpline();
    }
    for (int i = 0; i < 64; ++i) {
      const float u = float(i) / 63.f;
      g.lut[i] = lut.Eval(u * u * g.s[0]);
    }
    // The fade interpolations: a wrapper property, then a property list holding the
    // interpolation's type and its spline.
    auto fade = [&](uint32_t prop) {
      Span sp;
      if (r.room.Nested(*h, {prop}, sp) && sp.size > 6) {
        std::vector<Prop> props;
        if (PropList(r.room.Bytes(sp) + 6, sp.size - 6, &props)) {
          for (const Prop& p : props) {
            const uint8_t* const d = r.room.Bytes(sp) + 6 + p.data.start;
            PortMayaSpline sline;
            if (p.id == kPropFogSpline && sline.Load(d, p.data.size)) {
              return std::vector<uint8_t>(d, d + p.data.size);
            }
          }
          // A Time interpolation without its spline is an empty one (done at once).
          static const uint8_t kEmpty[15] = {};
          return std::vector<uint8_t>(kEmpty, kEmpty + sizeof(kEmpty));
        }
      }
      return std::vector<uint8_t>();
    };
    g.fadeIn = fade(kPropFogFadeIn);
    g.fadeOut = fade(kPropFogFadeOut);
    Span priority;
    if (r.room.Nested(*h, {kPropFogPriority[0], kPropFogPriority[1]}, priority) && priority.size >= 4) {
      g.priority = int32_t(ReadLE32(r.room.Bytes(priority)));
    }
    g.on = isGlobal && r.room.Active(*h);
    g.links = std::move(links);
    const auto layer = scripts.layer.find(h->entity);
    g.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    out.push_back(std::move(g));
  }
}

void Writer::ReadSuns(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                      std::vector<SunData>& out) const {
  for (const Component* c : r.room.Of(kLightDynamic)) {
    const auto f = r.room.Flat(*c);
    const auto type = f.find(kPropLightType);
    if (type == f.end() || type->second.size < 4 || ReadLE32(r.room.Bytes(type->second)) != kLightDirectional) {
      continue;
    }
    Vec3 pos, euler, scale;
    if (!r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    // The light shines along its local +Y (rotated by Rz*Ry*Rx), so -Y points at the sun. Checked
    // against Remastered's hangar, where the white key light casts the beam's shadow toward the door.
    constexpr double kDegToRad = 3.14159265358979323846 / 180; // M_PI isn't in MSVC's <cmath>
    const double x = euler[0] * kDegToRad, y = euler[1] * kDegToRad, z = euler[2] * kDegToRad;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
                 sz = std::sin(z);
    const Vec3 up = MulR2G({cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx});
    Vec3 d{};
    for (size_t i = 0; i < 3; ++i) {
      d[i] = xf[i][0] * up[0] + xf[i][1] * up[1] + xf[i][2] * up[2];
    }
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-6)) {
      continue;
    }
    SunData s;
    float color[4] = {1, 1, 1, 1};
    Span sp;
    if (r.room.Nested(*c, {kPropLightColor[0], kPropLightColor[1]}, sp) && sp.size >= 12) {
      for (int i = 0; i < 3; ++i) {
        color[i] = ReadLEFloat(r.room.Bytes(sp) + 4 * i);
      }
    }
    float intensity = 1.f;
    if (r.room.Nested(*c, {kPropLightIntensity[0], kPropLightIntensity[1]}, sp) && sp.size >= 4) {
      intensity = ReadLEFloat(r.room.Bytes(sp));
    }
    for (int i = 0; i < 3; ++i) {
      s.toSun[i] = float(-d[size_t(i)] / len);
      s.color[i] = std::isfinite(color[i] * intensity) ? std::max(0.f, color[i] * intensity) : 0.f;
    }
    // LightFlags (35b27754) byte 0, a103d675: RebuildLightProxy gates the shadow map on it
    // (build_directional_shadow_map_data 0x89da60); absent (the struct's count 0) is 0.
    if (r.room.Nested(*c, {kPropLightFlags[0], kPropLightFlags[1]}, sp) && sp.size >= 1) {
      s.casts = r.room.Bytes(sp)[0] != 0;
    }
    const auto layer = scripts.layer.find(c->entity);
    s.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    // A sun on no story layer is the room's own and starts on even if its entity doesn't: the
    // hangar's key sun is only switched on by the escape's camera, yet Remastered casts its
    // shadows from the first visit. Story-layer suns (the intro's) follow their script.
    s.on = r.room.Active(*c) || s.layer < 0;
    const auto group = scripts.group.find(c->entity);
    if (group != scripts.group.end()) {
      s.group = group->second;
    }
    char line[200];
    std::snprintf(line, sizeof line, "  %s: sun (%.3f, %.3f, %.3f) colour (%.2f, %.2f, %.2f) layer %d%s%s",
                  r.name.c_str(), s.toSun[0], s.toSun[1], s.toSun[2], s.color[0], s.color[1], s.color[2],
                  int(s.layer), s.on ? "" : " (off)",
                  s.group != PortRoomGeo::kNoGroup ? (" group " + std::to_string(s.group)).c_str() : "");
    Log(line);
    out.push_back(s);
  }
}

void Writer::ReadPointLights(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                             std::vector<PointLightData>& out) const {
  // Read once for the room, not once per light.
  const std::vector<Connection> conns = ReadConnections(r.room);
  for (const Component* c : r.room.Of(kLightDynamic)) {
    const auto f = r.room.Flat(*c);
    const auto type = f.find(kPropLightType);
    const uint32_t kind = type != f.end() && type->second.size >= 4 ? ReadLE32(r.room.Bytes(type->second)) : 0;
    if (kind != kLightPoint && kind != kLightSpot) {
      continue;
    }
    Vec3 pos, euler, scale;
    if (!r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    PointLightData l;
    l.spot = kind == kLightSpot;
    const Vec3 w = Apply(xf, MulR2G(pos));
    for (int i = 0; i < 3; ++i) {
      l.pos[i] = float(w[size_t(i)]);
    }
    if (l.spot) {
      // A spot shines along its local +Z (CLightSceneNodeProxy::UpdateLightFromTransform), turned
      // Rz * Ry * Rx as the suns' +Y.
      constexpr double kDegToRad = 3.14159265358979323846 / 180;
      const double x = euler[0] * kDegToRad, y = euler[1] * kDegToRad, z = euler[2] * kDegToRad;
      const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
                   sz = std::sin(z);
      const Vec3 fwd = MulR2G({cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx});
      Vec3 d{};
      for (size_t i = 0; i < 3; ++i) {
        d[i] = xf[i][0] * fwd[0] + xf[i][1] * fwd[1] + xf[i][2] * fwd[2];
      }
      const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (!(len > 1e-6)) {
        continue;
      }
      for (int i = 0; i < 3; ++i) {
        l.toLight[i] = float(-d[size_t(i)] / len);
      }
    }
    float color[3] = {1, 1, 1};
    Span sp;
    if (r.room.Nested(*c, {kPropLightColor[0], kPropLightColor[1]}, sp) && sp.size >= 12) {
      for (int i = 0; i < 3; ++i) {
        color[i] = ReadLEFloat(r.room.Bytes(sp) + 4 * i);
      }
    }
    float intensity = 3.14159265f;
    if (r.room.Nested(*c, {kPropLightIntensity[0], kPropLightIntensity[1]}, sp) && sp.size >= 4) {
      intensity = ReadLEFloat(r.room.Bytes(sp));
    }
    for (int i = 0; i < 3; ++i) {
      l.color[i] = std::isfinite(color[i] * intensity) ? std::max(0.f, color[i] * intensity) : 0.f;
      l.rgb[i] = std::isfinite(color[i]) ? std::max(0.f, color[i]) : 0.f;
    }
    l.intensity = std::isfinite(intensity) ? intensity : 0.f;
    if (r.room.Nested(*c, {kPropLightDistance, kPropLightDistanceKind}, sp) && sp.size >= 4) {
      const uint32_t falloff = ReadLE32(r.room.Bytes(sp));
      l.falloff = falloff == kLightFalloffSquared ? 2 : falloff == kLightFalloffLinear ? 1 : 3;
    }
    if (r.room.Nested(*c, {kPropLightDistance, kPropLightDistanceNear}, sp) && sp.size >= 4) {
      l.nearFar[0] = ReadLEFloat(r.room.Bytes(sp));
    }
    if (r.room.Nested(*c, {kPropLightDistance, kPropLightDistanceFar}, sp) && sp.size >= 4) {
      l.nearFar[1] = ReadLEFloat(r.room.Bytes(sp));
    }
    // build_light scales the range by the entity's scale: near by x, far by y.
    l.nearFar[0] *= float(std::max(scale[0], 0.0));
    l.nearFar[1] *= float(std::max(scale[1], 0.0));
    if (r.room.Nested(*c, {kPropLightCone, kPropLightConeInner}, sp) && sp.size >= 4) {
      l.cone[0] = ReadLEFloat(r.room.Bytes(sp));
    }
    if (r.room.Nested(*c, {kPropLightCone, kPropLightConeOuter}, sp) && sp.size >= 4) {
      l.cone[1] = ReadLEFloat(r.room.Bytes(sp));
    }
    l.scale[0] = float(std::max(scale[0], 0.0));
    l.scale[1] = float(std::max(scale[1], 0.0));
    const auto byte = [&](uint32_t a, uint32_t b) {
      return r.room.Nested(*c, {a, b}, sp) && sp.size >= 1 && r.room.Bytes(sp)[0] != 0;
    };
    l.animated = byte(kPropLightAttributes, kPropLightAnimated);
    if (l.animated) {
      l.playing = byte(kPropLightAttributes, kPropLightPlaying);
      l.loop = byte(kPropLightAttributes, kPropLightLoop);
      if (r.room.Nested(*c, {kPropLightAttributes, kPropLightLength}, sp) && sp.size >= 4) {
        const float length = ReadLEFloat(r.room.Bytes(sp));
        l.length = std::isfinite(length) ? std::clamp(length, 0.f, 3600.f) : 0.f;
      }
      if (r.room.Nested(*c, {kPropLightDistanceDynamic, kPropLightDistanceDynamicKind}, sp) && sp.size >= 4) {
        const uint32_t falloff = ReadLE32(r.room.Bytes(sp));
        l.animFalloff = falloff == kLightFalloffSquared ? 2 : falloff == kLightFalloffLinear ? 1 : 3;
      }
      // One spline, checked; the gradient's four follow one another.
      const auto take = [&](uint32_t a, uint32_t b, std::vector<uint8_t>& to) {
        PortMayaSpline check;
        if (r.room.Nested(*c, {a, b}, sp) && check.Load(r.room.Bytes(sp), sp.size)) {
          to.assign(r.room.Bytes(sp), r.room.Bytes(sp) + sp.size);
        }
      };
      take(kPropLightIntensitySpline[0], kPropLightIntensitySpline[1], l.splines[PortRoomEnv::kLightSplineIntensity]);
      take(kPropLightDistanceDynamic, kPropLightDistanceDynamicNear, l.splines[PortRoomEnv::kLightSplineNear]);
      take(kPropLightDistanceDynamic, kPropLightDistanceDynamicFar, l.splines[PortRoomEnv::kLightSplineFar]);
      take(kPropLightConeDynamic, kPropLightConeDynamicInner, l.splines[PortRoomEnv::kLightSplineInner]);
      take(kPropLightConeDynamic, kPropLightConeDynamicOuter, l.splines[PortRoomEnv::kLightSplineOuter]);
      take(kPropLightColorDynamic, kPropLightColorDynamicSpline, l.splines[PortRoomEnv::kLightSplineColor]);
      if (r.room.Nested(*c, {kPropLightColorDynamic, kPropLightColorDynamicGradient}, sp) && sp.size >= 4 &&
          ReadLE32(r.room.Bytes(sp)) == 4) {
        const uint8_t* d = r.room.Bytes(sp) + 4;
        size_t left = sp.size - 4;
        for (int i = 0; i < 4; ++i) {
          PortMayaSpline check;
          size_t used = 0;
          if (!check.Load(d, left, &used)) {
            break;
          }
          l.splines[PortRoomEnv::kLightSplineGradient + i].assign(d, d + used);
          d += used;
          left -= used;
        }
      }
      for (const Connection& conn : conns) {
        const int target = r.room.ByGuid(conn.target);
        if (r.room.Components()[conn.sender].entity == c->entity && target >= 0 &&
            r.room.Components()[size_t(target)].entity == c->entity && conn.event == kEventLightReachedStart &&
            (conn.action == kActionComponentDeactivate || conn.action == kActionEntityDeactivate)) {
          l.offAtStart = true;
        }
      }
    }
    if (const auto linked = scripts.links.find(c->entity); linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle &&
            (link.action < PortRoomEnv::kLightStart || link.action > PortRoomEnv::kLightReverse)) {
          continue;
        }
        if (!PortRoomEnv::PseudoSender(link.sender)) {
          link.sender &= 0x3ffffff;
        }
        l.links.push_back(link);
      }
    }
    const auto shown = f.find(kPropLightShown);
    l.on = r.room.Active(*c) && (shown == f.end() || shown->second.size < 1 || r.room.Bytes(shown->second)[0] != 0);
    const auto layer = scripts.layer.find(c->entity);
    l.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    const auto group = scripts.group.find(c->entity);
    if (group != scripts.group.end()) {
      l.group = group->second;
    }
    char line[320];
    std::snprintf(line, sizeof line,
                  "  %s: %s (%.2f, %.2f, %.2f) colour (%.2f, %.2f, %.2f) range %.2f..%.2f kind %d cone %g/%g "
                  "layer %d%s%s, %zu link(s)%s",
                  r.name.c_str(), l.spot ? "spot" : "point", l.pos[0], l.pos[1], l.pos[2], l.color[0], l.color[1],
                  l.color[2], l.nearFar[0], l.nearFar[1], int(l.falloff), l.cone[0], l.cone[1], int(l.layer),
                  l.on ? "" : " (off)",
                  l.group != PortRoomGeo::kNoGroup ? (" group " + std::to_string(l.group)).c_str() : "",
                  l.links.size(), l.animated ? (l.playing ? ", animated, playing" : ", animated") : "");
    Log(line);
    out.push_back(l);
  }
}

void Writer::ReadFogRegions(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                            std::vector<FogRegionData>& out, std::vector<FogTransitionData>& transitions) const {
  const std::vector<const Component*> regions = r.room.Of(kVolumetricFogRegion);
  if (regions.empty()) {
    return;
  }
  std::map<int, uint32_t> regionOf; // the first region written on each entity
  // Retail world -> Remastered world: R2G times the area's inverse.
  double g[3][3], gt[3];
  {
    double a[3][3], inv[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        a[i][j] = xf[size_t(i)][size_t(j)];
      }
    }
    if (!Invert3(a, inv)) {
      return;
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        g[i][j] = kR2G[i][0] * inv[0][j] + kR2G[i][1] * inv[1][j] + kR2G[i][2] * inv[2][j];
      }
    }
    for (int i = 0; i < 3; ++i) {
      gt[i] = -(g[i][0] * xf[0][3] + g[i][1] * xf[1][3] + g[i][2] * xf[2][3]);
    }
  }
  for (const Component* c : regions) {
    Vec3 pos, euler, scale;
    if (!r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    const auto f = r.room.Flat(*c);
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      return it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
    };
    auto word = [&](uint32_t prop) {
      const auto it = f.find(prop);
      return it != f.end() && it->second.size >= 4 ? ReadLE32(r.room.Bytes(it->second)) : 0u;
    };
    float color[4] = {1, 1, 1, 1};
    {
      const auto it = f.find(kPropRegionColor);
      if (it != f.end() && it->second.size >= 16) {
        for (int i = 0; i < 4; ++i) {
          color[i] = ReadLEFloat(r.room.Bytes(it->second) + 4 * i);
        }
      }
    }
    const uint32_t mode = word(kPropRegionMode);
    const uint32_t fluid = word(kPropRegionFluid);
    const float distance = value(kPropRegionDistance, 250.f);
    const float transmittance = value(kPropRegionTransmittance, 0.01f);
    const float intensity = value(kPropRegionIntensity, -1.f);
    const float cap = value(kPropRegionCap, -1.f);
    const float edge = value(kPropRegionEdge, 2.f);
    // CVolumetricFogRegionGOC::AddRegion: a subtracting region's density, colour and cap are
    // negative; an overriding one scales what is there by 0 at its core.
    FogRegionData d;
    const float sign = mode == kRegionSubtract ? -1.f : 1.f;
    d.mult = mode == kRegionOverride ? 0.f : 1.f;
    d.density = sign * (-std::log(transmittance) / distance);
    if (intensity >= 0) {
      d.hasColor = true;
      for (int i = 0; i < 3; ++i) {
        d.color[i] = color[i] * sign * intensity;
      }
      d.color[3] = color[3];
    }
    if (cap >= 0) {
      d.hasCap = true;
      d.cap = sign * cap;
    }
    d.fluid = fluid == kRegionInsideFluid ? 1 : fluid == kRegionOutsideFluid ? 2 : 0;
    // RebuildPositionalData: the entity's transform (CTransform4f::RotateZYXTranslate of its
    // euler degrees) with its columns scaled, over the unit box around the origin.
    double m[3][3];
    {
      const double k = 3.14159265358979323846 / 180.0;
      const double cx = std::cos(euler[0] * k), sx = std::sin(euler[0] * k);
      const double cy = std::cos(euler[1] * k), sy = std::sin(euler[1] * k);
      const double cz = std::cos(euler[2] * k), sz = std::sin(euler[2] * k);
      const double rot[3][3] = {{cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz},
                                {cy * sz, cx * cz + sx * sy * sz, cx * sy * sz - sx * cz},
                                {-sy, sx * cy, cx * cy}};
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          m[i][j] = rot[i][j] * scale[size_t(j)];
        }
      }
    }
    double minv[3][3];
    if (!Invert3(m, minv)) {
      continue;
    }
    // world (retail) -> 0..1 over the box: minv (g p + gt - pos) + 0.5.
    for (int i = 0; i < 3; ++i) {
      double t = 0.5;
      for (int j = 0; j < 3; ++j) {
        d.m[i][j] = float(minv[i][0] * g[0][j] + minv[i][1] * g[1][j] + minv[i][2] * g[2][j]);
        t += minv[i][j] * (gt[j] - pos[size_t(j)]);
      }
      d.m[i][3] = float(t);
    }
    // The edge ramps come from Remastered's world box (its axes, not the region's), as the
    // game makes them; the cull box is the same box in retail world space.
    const float width = std::max(edge, 0.001f);
    for (int i = 0; i < 3; ++i) {
      const double size = std::fabs(m[i][0]) + std::fabs(m[i][1]) + std::fabs(m[i][2]);
      const float h = std::max(float(0.5 * size), 0.001f);
      const float frac = std::min(h, width) / h;
      const float rest = std::max(1.f - frac, 0.001f);
      d.edgeScale[i] = -1.f / frac;
      d.edgeBias[i] = 1.f + rest / frac;
    }
    for (int i = 0; i < 3; ++i) {
      d.box[i] = std::numeric_limits<float>::max();
      d.box[3 + i] = -std::numeric_limits<float>::max();
    }
    for (int corner = 0; corner < 8; ++corner) {
      Vec3 rem;
      for (int i = 0; i < 3; ++i) {
        rem[size_t(i)] = pos[size_t(i)];
        for (int j = 0; j < 3; ++j) {
          rem[size_t(i)] += m[i][j] * ((corner >> j & 1) != 0 ? 0.5 : -0.5);
        }
      }
      const Vec3 w = Apply(xf, MulR2G(rem));
      for (int i = 0; i < 3; ++i) {
        d.box[i] = std::min(d.box[i], float(w[size_t(i)]));
        d.box[3 + i] = std::max(d.box[3 + i], float(w[size_t(i)]));
      }
    }
    bool finite = std::isfinite(d.density);
    for (const float v : d.m[0]) finite = finite && std::isfinite(v);
    for (const float v : d.m[1]) finite = finite && std::isfinite(v);
    for (const float v : d.m[2]) finite = finite && std::isfinite(v);
    for (const float v : d.box) finite = finite && std::isfinite(v);
    if (!finite) {
      continue;
    }
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (!PortRoomEnv::PseudoSender(link.sender)) {
          link.sender &= 0x3ffffff;
        }
        d.links.push_back(link);
      }
    }
    d.on = r.room.Active(*c);
    const auto layer = scripts.layer.find(c->entity);
    d.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    d.distance = distance;
    d.transmittance = transmittance;
    d.subtract = mode == kRegionSubtract;
    regionOf.emplace(c->entity, uint32_t(out.size()));
    out.push_back(std::move(d));
  }
  // Each transition moves the region on the entity it links to (the first region of it,
  // as Start's GetFirstComponentByComponentType finds it).
  const std::vector<Component>& comps = r.room.Components();
  for (const Component* c : r.room.Of(kVolumetricFogRegionTransition)) {
    std::vector<int> targets = ReadEntityLinks(r.room, size_t(c - comps.data()));
    const auto target = std::find_if(targets.begin(), targets.end(), [&](int t) {
      return t >= 0 && regionOf.count(t) != 0;
    });
    if (target == targets.end()) {
      Log("room " + r.name + ": a fog region transition links to no region");
      continue;
    }
    const auto f = r.room.Flat(*c);
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      const float v = it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    auto flag = [&](std::initializer_list<uint32_t> path) {
      Span s;
      return r.room.Nested(*c, path, s) && s.size >= 1 && r.room.Bytes(s)[0] != 0;
    };
    FogTransitionData t;
    t.region = regionOf.at(*target);
    t.autoStart = flag({kPropTransitionOptions, kPropTransitionAutoStart});
    t.loop = flag({kPropTransitionOptions, kPropTransitionLoop});
    for (int i = 0; i < 4; ++i) {
      if (flag({kPropTransitionSelect, kPropTransitionSelects[i]})) {
        t.select |= uint8_t(1u << i);
      }
    }
    {
      const auto it = f.find(kPropTransitionPhase);
      PortMayaSpline check;
      if (it != f.end() && check.Load(r.room.Bytes(it->second), it->second.size)) {
        t.phase.assign(r.room.Bytes(it->second), r.room.Bytes(it->second) + it->second.size);
      }
    }
    {
      const auto it = f.find(kPropTransitionColor);
      if (it != f.end() && it->second.size >= 16) {
        for (int i = 0; i < 4; ++i) {
          const float v = ReadLEFloat(r.room.Bytes(it->second) + 4 * i);
          t.color[i] = std::isfinite(v) ? v : t.color[i];
        }
      }
    }
    // The GOC's target colour: CColor4f::ScaleRGB by the intensity.
    const float intensity = value(kPropTransitionIntensity, 1.f);
    for (int i = 0; i < 3; ++i) {
      t.color[i] *= intensity;
    }
    t.distance = value(kPropTransitionDistance, 250.f);
    t.transmittance = value(kPropTransitionTransmittance, 0.01f);
    t.cap = value(kPropTransitionCap, 0.f);
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            (link.action < PortRoomEnv::kTransitionStart || link.action > PortRoomEnv::kTransitionDelete)) {
          continue;
        }
        if (!PortRoomEnv::PseudoSender(link.sender)) {
          link.sender &= 0x3ffffff;
        }
        t.links.push_back(link);
      }
    }
    t.on = r.room.Active(*c);
    const auto layer = scripts.layer.find(c->entity);
    t.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    transitions.push_back(std::move(t));
  }
}

// One decoded LTPB texture, placed in the room's grid of 64x64x16 blocks.
struct GridTexture {
  int32_t bx, by, bz;
  uint32_t index, kind, format, w, h, depth;
  std::vector<float> rgba;
};

// The textures of an LTPB: the TXTR forms inside it, each preceded by a 77 byte record.
// Only those `want` accepts (it sees the record fields, not the data) are decoded.
bool ReadGridTextures(const std::vector<uint8_t>& d, const std::function<bool(const GridTexture&)>& want,
                      std::vector<GridTexture>& textures, std::string& note) {
  std::string error;
  for (size_t o = 77; o + 32 < d.size(); ++o) {
    if (std::memcmp(&d[o], "RFRM", 4) != 0 || std::memcmp(&d[o + 20], "TXTR", 4) != 0) {
      continue;
    }
    const uint8_t* g = &d[o - 77];
    if (o + 76 > d.size()) {
      note = "grid not decoded";
      return false;
    }
    const uint32_t decomp = ReadLE32(g + 20), bufOff = ReadLE32(g + 45), bufSize = ReadLE32(g + 49);
    GridTexture t;
    t.bx = int32_t(ReadLE32(g + 61));
    t.by = int32_t(ReadLE32(g + 65));
    t.bz = int32_t(ReadLE32(g + 69));
    t.index = ReadLE32(g + 73);
    const uint8_t* h = &d[o + 56];
    t.kind = ReadLE32(h);
    t.format = ReadLE32(h + 4);
    t.w = ReadLE32(h + 8);
    t.h = ReadLE32(h + 12);
    t.depth = ReadLE32(h + 16);
    if (bufOff > d.size() - o || bufSize > d.size() - o - bufOff) {
      note = "grid not decoded";
      return false;
    }
    if (t.index > 5 || t.h != 64 || t.depth != 16 || (t.w != 64 && t.w != 128)) {
      note = "grid texture " + std::to_string(t.w) + "x" + std::to_string(t.h) + "x" + std::to_string(t.depth) +
             " index " + std::to_string(t.index);
      return false;
    }
    if (!want(t)) {
      continue;
    }
    if (textures.size() >= 4096 ||
        !DecodeVolumeFloat(&d[o + bufOff], bufSize, decomp, t.format, t.w, t.h, t.depth, t.rgba, error)) {
      note = "grid not decoded";
      return false;
    }
    textures.push_back(std::move(t));
  }
  return true;
}

// The room's own baked-lighting tiles (its LTPB), keyed by world tile and written as the
// version 21 tile set of port_room_env.h. Remastered keeps one world-wide virtual texture
// of such tiles: each loaded area registers its own copy of every tile it bakes, and which
// copy is mapped depends on the load order (see port_room_env_tiles.h), so the runtime
// decides that, and nothing here borrows from a neighbour.
bool Writer::Grid(const RoomPak& rp, const Vec3& shift, const std::vector<Vec3>& check, std::vector<uint8_t>& out,
                  std::string& note) const {
  out.clear();
  std::vector<uint8_t> d;
  const PakAsset* asset = rp.pak != nullptr ? FirstOfType(*rp.pak, FourCC("LTPB")) : nullptr;
  if (asset == nullptr) {
    note = "no grid";
    return false;
  }
  std::string error;
  if (!rp.pak->ReadAsset(*asset, d, error)) {
    note = "grid not decoded";
    return false;
  }
  size_t phdr = std::string::npos;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (std::memcmp(&d[i], "PHDR", 4) == 0) {
      phdr = i;
      break;
    }
  }
  if (phdr == std::string::npos || phdr + 24 + 26 + 6 > d.size()) {
    note = "no grid";
    return false;
  }
  int32_t box[6];
  for (size_t i = 0; i < 3; ++i) {
    box[i] = int16_t(ReadLE16(&d[phdr + 44 + 2 * i]));
    box[3 + i] = int16_t(ReadLE16(&d[phdr + 50 + 2 * i]));
  }

  std::vector<GridTexture> textures;
  if (!ReadGridTextures(d, [](const GridTexture&) { return true; }, textures, note)) {
    return false;
  }
  if (textures.empty()) {
    note = "no grid";
    return false;
  }

  auto clip = [](float v) { return std::isnan(v) ? v : std::min(std::max(v, 0.f), 60000.f); };
  auto u8 = [](float v) {
    const float r = std::nearbyint(v * 255.f);
    return std::isnan(r) ? uint8_t(0) : uint8_t(std::min(std::max(r, 0.f), 255.f));
  };
  const float floor = 1e-4f;

  struct Built {
    PortRoomEnvTiles::Tile tile;
    std::vector<uint8_t> bytes;
  };
  std::vector<Built> built;
  size_t litCount = 0, litTotal = 0;
  for (const GridTexture& t : textures) {
    const int layer = int(t.index);
    const int w = int(t.w), bytes = PortRoomEnvTiles::TileBytes(layer);
    if (w != PortRoomEnvTiles::TileWidth(layer)) {
      note = "grid texture " + std::to_string(t.w) + " wide at layer " + std::to_string(layer);
      return false;
    }
    std::vector<uint8_t> full(size_t(w) * 64 * 16 * size_t(bytes), 0);
    int32_t lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    for (int z = 0; z < 16; ++z) {
      for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < w; ++x) {
          const size_t n = (size_t(z) * 64 + size_t(y)) * size_t(w) + size_t(x);
          const float* src = &t.rgba[n * 4];
          uint8_t* p = &full[n * size_t(bytes)];
          if (layer == 0) {
            float mean[3] = {clip(src[0]), clip(src[1]), clip(src[2])};
            const bool lit = (mean[0] + mean[1]) + mean[2] > 0;
            ++litTotal;
            if (!lit) {
              continue;
            }
            ++litCount;
            for (int k = 0; k < 3; ++k) {
              // So a lit point never packs as an empty one.
              const float m = std::isnan(mean[k]) ? mean[k] : std::max(mean[k], floor);
              const uint16_t h = FloatToHalf(m);
              p[2 * k] = uint8_t(h);
              p[2 * k + 1] = uint8_t(h >> 8);
            }
          } else if (layer == 1) {
            for (int k = 0; k < 3; ++k) {
              const uint16_t h = FloatToHalf(clip(src[k]));
              p[2 * k] = uint8_t(h);
              p[2 * k + 1] = uint8_t(h >> 8);
            }
          } else {
            for (int k = 0; k < 3; ++k) {
              p[k] = u8(src[k]);
            }
          }
          bool any = false;
          for (int k = 0; k < bytes; ++k) {
            any = any || p[k] != 0;
          }
          if (any) {
            const int32_t q[3] = {x, y, z};
            for (int i = 0; i < 3; ++i) {
              lo[i] = std::min(lo[i], q[i]);
              hi[i] = std::max(hi[i], q[i] + 1);
            }
          }
        }
      }
    }
    Built b;
    b.tile.key = PortRoomEnvTiles::TileKey{layer, t.bx, t.by, t.bz};
    if (hi[0] > lo[0]) {
      for (int i = 0; i < 3; ++i) {
        b.tile.lo[i] = lo[i];
        b.tile.size[i] = hi[i] - lo[i];
      }
      for (int z = lo[2]; z < hi[2]; ++z) {
        for (int y = lo[1]; y < hi[1]; ++y) {
          const uint8_t* row = &full[((size_t(z) * 64 + size_t(y)) * size_t(w) + size_t(lo[0])) * size_t(bytes)];
          b.bytes.insert(b.bytes.end(), row, row + size_t(hi[0] - lo[0]) * size_t(bytes));
        }
      }
    }
    built.push_back(std::move(b));
  }
  if (litCount == 0) {
    note = "grid empty";
    return false;
  }
  std::stable_sort(built.begin(), built.end(), [](const Built& a, const Built& b) { return a.tile.key < b.tile.key; });
  built.erase(std::unique(built.begin(), built.end(),
                          [](const Built& a, const Built& b) { return a.tile.key == b.tile.key; }),
              built.end());

  // Point k sits at 2k metres in Remastered's axes, not in the middle of a 2 m cell:
  // CBakedLightingProbeTexture::Initialize (0x1c8cbc) rounds the box to points
  // (floor(x * 0.5 + 0.5)) and maps to texels by Scale(1 / size) * Translate(0.5 - lo) *
  // Scale(0.5), so texel k's middle is point lo + k. Tiles are keyed by world point, so
  // there is no crop origin here.
  float m[12];
  const Vec3 shifted = MulR2G(shift);  // R2G is its own transpose
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      m[i * 4 + j] = float(kR2G[j][i] / 2);
    }
    m[i * 4 + 3] = float(-shifted[size_t(i)] / 2);
  }
  for (float v : m) {
    AppendLEFloat(out, v);
  }
  for (int32_t v : box) {
    AppendLE32(out, uint32_t(v));
  }
  uint32_t dataBytes = 0;
  for (const Built& b : built) {
    dataBytes += uint32_t(b.bytes.size());
  }
  AppendLE32(out, uint32_t(built.size()));
  AppendLE32(out, dataBytes);
  uint32_t offset = 0;
  for (const Built& b : built) {
    AppendLE32(out, uint32_t(b.tile.key.layer));
    AppendLE32(out, uint32_t(b.tile.key.x));
    AppendLE32(out, uint32_t(b.tile.key.y));
    AppendLE32(out, uint32_t(b.tile.key.z));
    for (int i = 0; i < 3; ++i) {
      AppendLE32(out, uint32_t(b.tile.lo[i]));
    }
    for (int i = 0; i < 3; ++i) {
      AppendLE32(out, uint32_t(b.tile.size[i]));
    }
    AppendLE32(out, offset);
    offset += uint32_t(b.bytes.size());
  }
  for (const Built& b : built) {
    out.insert(out.end(), b.bytes.begin(), b.bytes.end());
  }

  char buf[96];
  std::snprintf(buf, sizeof buf, "grid %zu tiles, %zu lit points", built.size(), litCount);
  note = buf;
  if (!check.empty()) {
    PortRoomEnvTiles::TileSet set;
    for (const Built& b : built) {
      PortRoomEnvTiles::Tile t = b.tile;
      t.offset = uint32_t(set.data.size());
      set.tiles.push_back(t);
      set.data.insert(set.data.end(), b.bytes.begin(), b.bytes.end());
    }
    int ok = 0;
    for (const Vec3& c : check) {
      int64_t q[3];
      for (int i = 0; i < 3; ++i) {
        q[i] = int64_t(std::nearbyint(double(c[0]) * m[i * 4] + double(c[1]) * m[i * 4 + 1] +
                                      double(c[2]) * m[i * 4 + 2] + double(m[i * 4 + 3])));
      }
      if (set.Lit(q[0], q[1], q[2])) {
        ++ok;
      }
    }
    note += ", " + std::to_string(ok) + "/" + std::to_string(check.size()) + " doors on lit points";
  }
  (void)litTotal;
  return true;
}

// An MCON's instances: instance i draws models[index[i]] at transforms[i], twelve
// floats that are the rows of a 3x4 matrix in the room's own frame.
struct Mcon {
  std::vector<Id16> models;  // in a pak's byte order
  std::vector<uint16_t> index;
  const uint8_t* transforms = nullptr;
  // Per instance, the lightmap lookup (offU, offV, scale, 0); empty when the MCON has none.
  std::vector<std::array<float, 4>> lookups;
};

bool ReadMcon(const std::vector<uint8_t>& d, Mcon& out) {
  if (d.size() < 32 || ReadBE32(&d[0]) != FourCC("RFRM") || ReadLE64(&d[4]) > d.size() - 32) {
    return false;
  }
  const size_t end = 32 + size_t(ReadLE64(&d[4]));
  for (size_t o = 32; o + 24 <= end && ReadBE32(&d[o]) != FourCC("PEEK");) {
    const uint64_t size = ReadLE64(&d[o + 4]);
    const size_t start = o + 24;
    if (size > end - start) {
      return false;
    }
    if (ReadBE32(&d[o]) == FourCC("MCVD")) {
      size_t p = start;
      const size_t stop = start + size_t(size);
      // A counted vector of `width` byte items; null when it runs past the chunk.
      auto vec = [&](size_t width, size_t& count) -> const uint8_t* {
        if (stop - p < 4) {
          return nullptr;
        }
        count = ReadLE32(&d[p]);
        p += 4;
        if (count > (stop - p) / width) {
          return nullptr;
        }
        const uint8_t* at = d.data() + p;
        p += count * width;
        return at;
      };
      size_t models = 0, other = 0, transforms = 0, indices = 0;
      const uint8_t* m = vec(16, models);
      // Then ids, colours, the instance transforms, object transforms and a byte each.
      if (m == nullptr || vec(16, other) == nullptr || vec(16, other) == nullptr) {
        return false;
      }
      out.transforms = vec(48, transforms);
      if (out.transforms == nullptr || vec(64, other) == nullptr || vec(1, other) == nullptr) {
        return false;
      }
      const uint8_t* ix = vec(2, indices);
      if (ix == nullptr || indices != transforms) {
        return false;
      }
      for (size_t i = 0; i < models; ++i) {
        out.models.push_back(SwapUuid(m + 16 * i));
      }
      for (size_t i = 0; i < indices; ++i) {
        out.index.push_back(ReadLE16(ix + 2 * i));
      }
      // Then a vec(2) of zeros, a vec(1) per instance and one per model, and the lookups.
      // They are optional: anything that does not fit leaves the MCON without.
      size_t lookups = 0;
      if (vec(2, other) != nullptr && vec(1, other) != nullptr && vec(1, other) != nullptr) {
        const uint8_t* lm = vec(16, lookups);
        if (lm != nullptr && lookups == indices) {
          for (size_t i = 0; i < lookups; ++i) {
            std::array<float, 4> v;
            for (size_t k = 0; k < 4; ++k) {
              v[k] = ReadLEFloat(lm + 16 * i + 4 * k);
            }
            out.lookups.push_back(v);
          }
        }
      }
      return true;
    }
    o = start + size_t(size);
  }
  return false;
}

static void EditorBasis(const Vec3& rot, const Vec3& scale, double out[3][3]);

void Writer::WriteGeometry(const RoomData& r, uint32_t mrea, const Area& area) {
  if (!m_io.model || (m_io.wantsGeometry && !m_io.wantsGeometry(r.name))) {
    return;
  }
  // kR2G as a signed permutation: gc[i] = kSign[i] * remastered[kAxis[i]].
  static const int kAxis[3] = {0, 2, 1};
  static const double kSign[3] = {-1, 1, 1};
  const RoomPak home{r.name, r.pak};
  std::vector<PortRoomGeo::Instance> instances;
  size_t dropped = 0;
  SceneryScripts scripts = MatchScripts(r.room, area);  // `pieces` adds the objects hidden
  for (const auto& [key, stat] : scripts.missStats) {
    Log("  " + r.name + ": untraced " + std::to_string(stat.first) + " connection(s) on " +
        std::to_string(stat.second.size()) + " entities: " + key);
  }
  // Whether an entity that starts inactive can be shown: by a retail object (links) or by
  // Remastered's own script. One that cannot is never seen: the ships of the landing
  // cutscene sit in the sky and on the pad, where retail's own ship already is.
  auto canShow = [&](int entity) {
    const auto links = scripts.links.find(entity);
    return scripts.scriptShows.count(entity) != 0 ||
           (links != scripts.links.end() &&
            std::any_of(links->second.begin(), links->second.end(),
                        [](const PortRoomGeo::Link& l) { return l.action != PortRoomGeo::kHide; }));
  };
  // Whether a link shows the entity and another hides it again, the hide carrying its Timer's
  // delay: the hangar's boundary shield flashes for 0.25 s when a hit is blocked.
  auto showsAndHides = [&](int entity) {
    const auto links = scripts.links.find(entity);
    if (links == scripts.links.end()) {
      return false;
    }
    const auto& v = links->second;
    return std::any_of(v.begin(), v.end(), [](const PortRoomGeo::Link& l) { return l.action == PortRoomGeo::kShow; }) &&
           std::any_of(v.begin(), v.end(), [](const PortRoomGeo::Link& l) { return l.action == PortRoomGeo::kHide; });
  };
  size_t gated = 0, linked = 0, grouped = 0, inactive = 0, unresolved = 0;
  // What the entity's scripts make of an instance: its layer, whether it starts shown, and
  // what shows and hides it.
  auto script = [&](PortRoomGeo::Instance& inst, int entity, bool active) {
    inst.active = active;
    const auto layer = scripts.layer.find(entity);
    if (layer != scripts.layer.end() && layer->second != 0) {
      // Layer 0 is always on; leaving it unset skips the lookup.
      inst.layer = layer->second;
      ++gated;
    }
    const auto links = scripts.links.find(entity);
    if (links != scripts.links.end()) {
      inst.links = links->second;
      ++linked;
    }
    const auto group = scripts.group.find(entity);
    if (group != scripts.group.end()) {
      inst.group = group->second;
      ++grouped;
    }
    if (scripts.unresolved.count(entity) != 0) {
      ++unresolved;
    }
  };
  // An entity's transform in GC axes: Rz * Ry * Rx of its angles, as retail builds an
  // editor transform, then its scale and position.
  auto place = [&](PortRoomGeo::Instance& inst, const Vec3& pos, const Vec3& rot, const Vec3& scale) {
    double s[3], k[3];
    for (int i = 0; i < 3; ++i) {
      s[i] = std::sin(rot[i] * (3.14159265358979323846 / 180.0));
      k[i] = std::cos(rot[i] * (3.14159265358979323846 / 180.0));
    }
    const double m[3][3] = {
        {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
        {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
        {-s[1], k[1] * s[0], k[1] * k[0]},
    };
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        inst.transform[4 * row + col] = float(kSign[row] * kSign[col] * m[kAxis[row]][kAxis[col]] * scale[kAxis[col]]);
      }
      inst.transform[4 * row + 3] = float(kSign[row] * pos[kAxis[row]]);
    }
  };
  // The instance's slot in the room's baked lightmap, when the lightmap loaded and the
  // lookup is one (PortRoomGeo::HasLightmapLookup: a scale of 0 is one texel).
  const bool lightmapOk = Lightmap(r).ok;
  size_t lookups = 0;
  auto setLookup = [&](PortRoomGeo::Instance& inst, const float* v) {
    if (PortRoomGeo::HasLightmapLookup(v)) {
      inst.lightmap[0] = v[0];
      inst.lightmap[1] = v[1];
      inst.lightmap[2] = v[2];
      ++lookups;
    }
  };
  size_t modcons = 0;
  for (const Component* c : r.room.Of(kModCon)) {
    const bool active = r.room.Active(*c);
    if (!active && !canShow(c->entity)) {
      ++inactive;
      continue;
    }
    const auto f = r.room.Flat(*c);
    const auto prop = f.find(kPropModConMcon);
    std::vector<uint8_t> data;
    Mcon mcon;
    if (prop == f.end() || prop->second.size < 16 ||
        !FindResource(r.room.Bytes(prop->second), FourCC("MCON"), home, data, nullptr, nullptr)) {
      continue;
    }
    if (!ReadMcon(data, mcon)) {
      Log("  " + r.name + ": unreadable MCON");
      continue;
    }
    // The instances are in room space, not relative to the component's entity: where an
    // entity is off the origin, adding its position moves the geometry off the room's doors.
    std::vector<int> state(mcon.models.size(), 0);  // 1 converted, 2 not
    std::vector<uint32_t> ids(mcon.models.size(), 0);
    for (size_t i = 0; i < mcon.index.size(); ++i) {
      const size_t model = mcon.index[i];
      if (model >= mcon.models.size()) {
        ++dropped;
        continue;
      }
      if (state[model] == 0) {
        if (m_io.cancelled && m_io.cancelled()) {
          return;
        }
        state[model] = m_io.model(mcon.models[model], ids[model]) ? 1 : 2;
      }
      if (state[model] != 1) {
        ++dropped;
        continue;
      }
      const uint8_t* t = mcon.transforms + 48 * i;
      PortRoomGeo::Instance& inst = instances.emplace_back();
      inst.model = ids[model];
      for (int row = 0; row < 3; ++row) {
        const uint8_t* from = t + 16 * kAxis[row];
        for (int col = 0; col < 3; ++col) {
          inst.transform[4 * row + col] = float(kSign[row] * kSign[col] * double(ReadLEFloat(from + 4 * kAxis[col])));
        }
        inst.transform[4 * row + 3] = float(kSign[row] * double(ReadLEFloat(from + 12)));
      }
      script(inst, c->entity, active);
      if (lightmapOk && i < mcon.lookups.size()) {
        setLookup(inst, mcon.lookups[i].data());
      }
      ++modcons;
    }
  }
  // A world model (WMDL) that the room only lists among its resources, which no component
  // places: a piece of the room's own geometry in room space, such as the grating floor of
  // Elite Quarters Access (00o) that no MCON has. A WMDL a component names is a liquid's.
  for (const size_t at : r.room.Listed()) {
    std::vector<uint8_t> data;
    uint32_t id = 0;
    if (r.room.NamedElsewhere(at) ||
        !FindResource(r.room.Bytes({at, 16}), FourCC("WMDL"), home, data, nullptr, nullptr)) {
      continue;
    }
    if (m_io.cancelled && m_io.cancelled()) {
      return;
    }
    if (!m_io.model(SwapUuid(r.room.Bytes({at, 16})), id)) {
      ++dropped;
      continue;
    }
    PortRoomGeo::Instance& inst = instances.emplace_back();
    inst.model = id;
    for (int i = 0; i < 3; ++i) {
      inst.transform[5 * i] = 1.0f;
    }
    char line[96];
    std::snprintf(line, sizeof line, "  %s: world model %08X", r.name.c_str(), id);
    Log(line);
  }
  // Scenery Remastered added as actors rather than as room geometry: the frame around each
  // door, and pieces of the room itself that sit on its "RS" layer. They carry kPropActorAdded,
  // which no actor that retail also has does. Each is drawn on the retail layer its own layer
  // stands for, shown and hidden by the retail objects that drive it, and moved by the
  // platform that carries it, if one does (MatchScripts). Property 1285da4d, which some
  // carry, names no parent: most actors with it stand still, and the room's script
  // connections are what make one a platform's.
  std::map<Id16, uint32_t> actorModels;  // 0 when the model did not convert
  size_t actors = 0, riding = 0, glowing = 0, retailDrawn = 0, animated = 0;
  // Characters read so far, by pak id; null when one did not read.
  std::map<Id16, std::unique_ptr<PortRemasteredAnim::Character>> characters;
  // By component index: the glow its incandescence modulator gives it. A channel or
  // intensity the modulator leaves out is 1. The door frames' modulators fade in no time
  // and nothing starts them, so the glow holds from the start; one something else starts
  // (6 in all) leaves its actor as the material has it.
  std::map<size_t, std::array<float, 3>> glows;
  const std::vector<Component>& comps = r.room.Components();
  const std::vector<Connection> links = ReadConnections(r.room);
  std::set<int> started;
  for (const Connection& link : links) {
    started.insert(r.room.ByGuid(link.target));
  }
  for (const Connection& link : links) {
    const Component& from = comps[link.sender];
    const int target = r.room.ByGuid(link.target);
    if (from.type != kColorModulateMP1 || started.count(int(link.sender)) != 0 || target < 0 ||
        comps[size_t(target)].type != kActorMP1) {
      continue;
    }
    const auto f = r.room.Flat(from);
    const auto blend = f.find(kPropModulateBlend);
    if (blend == f.end() || blend->second.size != 4 || ReadLE32(r.room.Bytes(blend->second)) != kModulateIncandescence) {
      continue;
    }
    const auto intensity = f.find(kPropModulateIntensity);
    const float scale =
        intensity != f.end() && intensity->second.size == 4 ? ReadLEFloat(r.room.Bytes(intensity->second)) : 1.f;
    std::array<float, 3> glow;
    for (int i = 0; i < 3; ++i) {
      Span s;
      glow[i] = r.room.Nested(from, {kPropModulateColorB, kPropColorChannel[i]}, s) && s.size == 4
                    ? ReadLEFloat(r.room.Bytes(s))
                    : 1.f;
      glow[i] = std::isfinite(glow[i] * scale) ? std::max(glow[i] * scale, 0.f) : 0.f;
    }
    glows[size_t(target)] = glow;
  }
  // Entities a Timer hides. One that also starts hidden is shown only while an event plays:
  // the waterfalls of Ice Shorelines and the Hive Totem pour for 17 and 6.25 s and fade with
  // their Waterfalls.GRDU driver, and pieces of the Frigate's hangar flash for 0.25 s. Their
  // links keep the timers' delays (Link::delay); those with a float driver that moves a
  // parameter of their material (driverOf) are kept with the driver, those with both a show and
  // a hide link (the hangar's boundary shield) are kept with the links, the others are left out. An
  // animated actor is drawn: the Mines turbine's explosion, shown 5.5 s after its pickup is
  // taken and hidden 2.5 s later, partway through its clip.
  std::set<int> timedOff;
  for (const Connection& link : links) {
    const int target = r.room.ByGuid(link.target);
    if (target >= 0 && RetailType(comps[link.sender].type) == kRetailTimer &&
        LinkAct(link.action) == PortRoomGeo::kHide) {
      timedOff.insert(comps[size_t(target)].entity >= 0 ? comps[size_t(target)].entity : target);
    }
  }
  // An animated actor whose character has more than one bone: the hangar's floating debris,
  // the Mines spinner's and Omega's tank's explosions, the Omega Pirate's death in the phazon
  // pool. Their joints move rigid pieces (each triangle is on the joint that weighs most in
  // it, TriangleJoint), so each joint's triangles are a model of their own (RoomIO::piece),
  // posed by the joint's skin matrix. Its clips: the actor's own animation, looped when it
  // starts shown and else played once from each time it is shown, then the animation of each
  // ActorKeyframe that plays it (SceneryScripts::keyframes), started by the keyframe's Play.
  std::map<std::pair<Id16, int>, uint32_t> pieceModels;  // 0 when the piece did not convert
  auto pieces = [&](const Component& c, const PortRemasteredAnim::Character& ch, const PortRemasteredAnim::Anim& own,
                    const Id16& model, const Vec3& pos, const Vec3& rot, const Vec3& scale, bool active) {
    std::vector<uint8_t> data;
    PortRemastered::Model smdl;
    std::string error;
    if (!m_io.piece || !FindResource(ch.skinnedModel.data(), FourCC("SMDL"), home, data, nullptr, nullptr) ||
        !PortRemastered::ParseModel(data.data(), data.size(), smdl, error)) {
      Log("  " + r.name + ": an animated actor's skinned model did not read" + (error.empty() ? "" : ": " + error));
      return false;
    }
    std::set<int> joints;
    for (const PortRemastered::ModelMesh& mesh : smdl.meshes) {
      if (mesh.vertexBuffer >= smdl.vertexBuffers.size()) {
        continue;
      }
      for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        joints.insert(PortRemastered::TriangleJoint(smdl.vertexBuffers[mesh.vertexBuffer], &mesh.indices[t]));
      }
    }
    if (joints.empty() || *joints.begin() < 0 || size_t(*joints.rbegin()) >= ch.inverseBind.size()) {
      Log("  " + r.name + ": an animated actor's model is not skinned to its character");
      return false;
    }
    struct Clip {
      const PortRemasteredAnim::Anim* anim;
      bool loop;
    };
    std::vector<Clip> clips{{&own, active}};
    auto order = scripts.keyframes.find(c.entity);
    if (order != scripts.keyframes.end()) {
      std::vector<size_t> senders = order->second;
      std::sort(senders.begin(), senders.end());
      senders.erase(std::unique(senders.begin(), senders.end()), senders.end());
      for (const size_t k : senders) {
        const auto f = r.room.Flat(comps[k]);
        const auto name = f.find(kPropKeyframeAnim);
        const auto loop = f.find(kPropKeyframeLoop);
        std::string text;
        const PortRemasteredAnim::Anim* anim =
            name != f.end() && name->second.size == 8 &&
                    r.room.String(ReadLE32(r.room.Bytes(name->second)), ReadLE32(r.room.Bytes(name->second) + 4), text)
                ? PortRemasteredAnim::Find(ch, text)
                : nullptr;
        if (anim == nullptr || anim->frames < 2 || !(anim->fps > 0.f)) {
          Log("  " + r.name + ": a keyframe's animation \"" + text + "\" is not in its character");
          continue;
        }
        clips.push_back({anim, loop != f.end() && loop->second.size == 1 && r.room.Bytes(loop->second)[0] != 0});
      }
    }
    // Per clip, per frame, the joints' skin matrices in GC axes (M' = A M A^T, t' = A t for the
    // axis change A). A joint whose matrices are all rotations bakes as rotation quaternions
    // and translations. One that scales (Remastered keys joint scale, inherited down the
    // chain as abs_bone_rel does, 0xba19a0) bakes M = R U per frame: R the rotation of the
    // polar decomposition as a quaternion, U = R^T M the stretch (kept in `stretch`, nine per
    // frame). Only a reflection or a collapsed matrix cannot be a piece's pose.
    struct Frame {
      double m[3][3];
      double t[3];
    };
    std::vector<std::map<int, std::vector<Frame>>> frames(clips.size());
    std::vector<std::map<int, std::vector<float>>> keys(clips.size()), stretch(clips.size());
    std::vector<std::array<float, 12>> skin;
    for (size_t i = 0; i < clips.size(); ++i) {
      for (uint32_t frame = 0; frame < clips[i].anim->frames; ++frame) {
        if (!PortRemasteredAnim::SkinPose(ch, *clips[i].anim, frame, skin)) {
          Log("  " + r.name + ": an animated actor's skeleton did not pose");
          return false;
        }
        for (const int j : joints) {
          const std::array<float, 12>& s = skin[size_t(j)];
          Frame fr;
          for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
              fr.m[row][col] = kSign[row] * kSign[col] * double(s[4 * kAxis[row] + kAxis[col]]);
            }
            fr.t[row] = kSign[row] * double(s[4 * kAxis[row] + 3]);
          }
          frames[i][j].push_back(fr);
        }
      }
      for (const int j : joints) {
        const std::vector<Frame>& list = frames[i][j];
        bool rigid = true;
        for (const Frame& fr : list) {
          const double(&m)[3][3] = fr.m;
          double worst = 0;
          for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
              const double dot = m[0][a] * m[0][b] + m[1][a] * m[1][b] + m[2][a] * m[2][b];
              worst = std::max(worst, std::fabs(dot - (a == b ? 1.0 : 0.0)));
            }
          }
          const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
          if (!(det > 0)) {
            Log("  " + r.name + ": animation \"" + clips[i].anim->name + "\" reflects joint " + std::to_string(j));
            return false;
          }
          rigid = rigid && worst < 0.02;
        }
        std::vector<float>& out = keys[i][j];
        std::vector<float>& st = stretch[i][j];
        for (const Frame& fr : list) {
          double m[3][3];
          std::memcpy(m, fr.m, sizeof m);
          double rot[3][3];
          std::memcpy(rot, m, sizeof rot);
          if (!rigid) {
            // Polar decomposition: R <- (R + R^-T) / 2 until it settles.
            bool settled = false;
            for (int it = 0; it < 64 && !settled; ++it) {
              const double det = rot[0][0] * (rot[1][1] * rot[2][2] - rot[1][2] * rot[2][1]) -
                                 rot[0][1] * (rot[1][0] * rot[2][2] - rot[1][2] * rot[2][0]) +
                                 rot[0][2] * (rot[1][0] * rot[2][1] - rot[1][1] * rot[2][0]);
              if (!(det > 1e-12)) {
                Log("  " + r.name + ": animation \"" + clips[i].anim->name + "\" collapses joint " + std::to_string(j));
                return false;
              }
              double inv[3][3];  // the inverse transpose = the cofactor matrix / det
              for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                  const int a1 = (a + 1) % 3, a2 = (a + 2) % 3, b1 = (b + 1) % 3, b2 = (b + 2) % 3;
                  inv[a][b] = (rot[a1][b1] * rot[a2][b2] - rot[a1][b2] * rot[a2][b1]) / det;
                }
              }
              double change = 0;
              for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                  const double next = 0.5 * (rot[a][b] + inv[a][b]);
                  change = std::max(change, std::fabs(next - rot[a][b]));
                  rot[a][b] = next;
                }
              }
              settled = change < 1e-13;
            }
            if (!settled) {
              Log("  " + r.name + ": animation \"" + clips[i].anim->name + "\" has no rotation for joint " + std::to_string(j));
              return false;
            }
            for (int a = 0; a < 3; ++a) {
              for (int b = 0; b < 3; ++b) {
                st.push_back(float(rot[0][a] * m[0][b] + rot[1][a] * m[1][b] + rot[2][a] * m[2][b]));
              }
            }
            std::memcpy(m, rot, sizeof m);
          }
          // The quaternion of m, from its largest component.
          double q[4];
          const double trace = m[0][0] + m[1][1] + m[2][2];
          if (trace > 0) {
            const double w = std::sqrt(1 + trace) * 2;
            q[3] = w / 4;
            q[0] = (m[2][1] - m[1][2]) / w;
            q[1] = (m[0][2] - m[2][0]) / w;
            q[2] = (m[1][0] - m[0][1]) / w;
          } else {
            int a = 0;
            if (m[1][1] > m[a][a]) {
              a = 1;
            }
            if (m[2][2] > m[a][a]) {
              a = 2;
            }
            const int b = (a + 1) % 3, d = (a + 2) % 3;
            const double w = std::sqrt(1 + m[a][a] - m[b][b] - m[d][d]) * 2;
            q[a] = w / 4;
            q[b] = (m[b][a] + m[a][b]) / w;
            q[d] = (m[d][a] + m[a][d]) / w;
            q[3] = (m[d][b] - m[b][d]) / w;
          }
          const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
          for (int k = 0; k < 4; ++k) {
            out.push_back(float(q[k] / len));
          }
          for (int k = 0; k < 3; ++k) {
            out.push_back(float(fr.t[k]));
          }
        }
      }
      frames[i].clear();
    }
    // Each piece's bounds as its first clip starts, in world space, and the retail objects in
    // them it stands for (Script::hidden): debris, and Actors in the same Active state - the
    // hangar's floating debris, but not Omega's whole tank, which our explosion replaces only
    // once it is shown.
    if (!PortRemasteredAnim::SkinPose(ch, *clips[0].anim, 0, skin)) {
      return false;
    }
    PortRoomGeo::Instance placed;
    place(placed, pos, rot, scale);
    std::map<int, std::pair<Vec3, Vec3>> bounds;
    for (const PortRemastered::ModelMesh& mesh : smdl.meshes) {
      if (mesh.vertexBuffer >= smdl.vertexBuffers.size()) {
        continue;
      }
      const PortRemastered::ModelVertexBuffer& vb = smdl.vertexBuffers[mesh.vertexBuffer];
      for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const int j = PortRemastered::TriangleJoint(vb, &mesh.indices[t]);
        const std::array<float, 12>& s = skin[size_t(j)];
        auto box = bounds.try_emplace(j, Vec3{HUGE_VAL, HUGE_VAL, HUGE_VAL}, Vec3{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL}).first;
        for (size_t k = 0; k < 3; ++k) {
          const size_t v = mesh.indices[t + k];
          if (3 * v + 2 >= vb.positions.size()) {
            continue;
          }
          const float* p = &vb.positions[3 * v];
          Vec3 posed, gc, local;
          for (int row = 0; row < 3; ++row) {
            posed[size_t(row)] = s[4 * row] * p[0] + s[4 * row + 1] * p[1] + s[4 * row + 2] * p[2] + s[4 * row + 3];
          }
          for (int row = 0; row < 3; ++row) {
            gc[size_t(row)] = kSign[row] * posed[size_t(kAxis[row])];
          }
          for (int row = 0; row < 3; ++row) {
            local[size_t(row)] = placed.transform[4 * row] * gc[0] + placed.transform[4 * row + 1] * gc[1] +
                                 placed.transform[4 * row + 2] * gc[2] + placed.transform[4 * row + 3];
          }
          const Vec3 w = Apply(area.xf, local);
          for (size_t a = 0; a < 3; ++a) {
            box->second.first[a] = std::min(box->second.first[a], w[a]);
            box->second.second[a] = std::max(box->second.second[a], w[a]);
          }
        }
      }
    }
    // How far a retail object's origin is outside the nearest piece (0 inside).
    auto outside = [&](const Vec3& at) {
      double best = HUGE_VAL;
      for (const auto& box : bounds) {
        double d = 0;
        for (size_t a = 0; a < 3; ++a) {
          d = std::max({d, box.second.first[a] - at[a], at[a] - box.second.second[a]});
        }
        best = std::min(best, d);
      }
      return best;
    };
    auto isDebris = [](const ScriptObject& o) { return o.type == kRetailDebris || o.type == kRetailDebrisExtended; };
    std::vector<uint32_t> hidden;
    for (const ScriptObject& o : area.objects) {
      if (!o.hasPos) {
        continue;
      }
      bool hide = isDebris(o) && outside(o.pos) <= kPieceDebrisNear;
      if (o.type == kRetailActor && o.propCount == kRetailActorProps && o.props.size() >= kRetailActorActiveFromEnd &&
          (o.props[o.props.size() - kRetailActorActiveFromEnd] != 0) == active) {
        // An Actor whose debris starts where it stands is that debris's intact form.
        const bool twin = std::any_of(area.objects.begin(), area.objects.end(), [&](const ScriptObject& d) {
          return isDebris(d) && d.hasPos && MaxAbs(d.pos, o.pos) < 0.01;
        });
        hide = outside(o.pos) <= (twin ? kPieceDebrisNear : kPieceNear);
      }
      if (hide) {
        hidden.push_back(o.id & 0x3FFFFFFu);  // as TEditorId::Value, without the layer
      }
    }
    size_t made = 0;
    const size_t first = instances.size();  // the first piece, whose shown state hides them
    std::string models;
    for (const int j : joints) {
      auto known = pieceModels.find({model, j});
      if (known == pieceModels.end()) {
        uint32_t id = 0;
        known = pieceModels.emplace(std::make_pair(model, j), m_io.piece(model, j, id) ? id : 0).first;
      }
      if (known->second == 0) {
        continue;
      }
      PortRoomGeo::Instance& inst = instances.emplace_back();
      inst.model = known->second;
      place(inst, pos, rot, scale);
      inst.animOnShow = !active;
      for (size_t i = 0; i < clips.size(); ++i) {
        PortRoomGeo::Instance::AnimClip& clip = inst.anim.emplace_back();
        clip.fps = clips[i].anim->fps;
        clip.loop = clips[i].loop;
        clip.keys = std::move(keys[i][j]);
        clip.stretch = std::move(stretch[i][j]);
      }
      script(inst, c.entity, active);
      ++made;
      char hex[10];
      std::snprintf(hex, sizeof hex, " %08X", known->second);
      models += hex;
    }
    std::string names;
    for (const Clip& clip : clips) {
      char length[24];
      std::snprintf(length, sizeof length, " %.2f s", clip.anim->fps > 0 ? clip.anim->frames / clip.anim->fps : 0.f);
      names += (names.empty() ? "" : ", ") + clip.anim->name + length + (clip.loop ? " (loop)" : "");
    }
    if (const auto links = scripts.links.find(c.entity); links != scripts.links.end()) {
      names += ", linked from";
      for (const PortRoomGeo::Link& l : links->second) {
        char hex[48];
        std::snprintf(hex, sizeof hex, " %08X (%u: %u after %.2f s)", l.sender, l.state, l.action, l.delay);
        names += hex;
      }
    }
    if (made != 0) {
      for (const uint32_t id : hidden) {
        scripts.script.hidden.push_back({id, uint32_t(first)});
      }
    }
    Log("  " + r.name + ": an actor's " + names + " in " + std::to_string(made) + " of " +
        std::to_string(joints.size()) + " pieces:" + models + "; " + std::to_string(made != 0 ? hidden.size() : 0) +
        " retail objects hidden");
    return made != 0;
  };
  // A static model's bounds in its own (Remastered) axes; false when it does not read.
  auto modelBounds = [&](const uint8_t* id, Vec3& lo, Vec3& hi) {
    std::vector<uint8_t> data;
    PortRemastered::Model cmdl;
    std::string error;
    if (!FindResource(id, FourCC("CMDL"), home, data, nullptr, nullptr) ||
        !PortRemastered::ParseModel(data.data(), data.size(), cmdl, error)) {
      return false;
    }
    lo = Vec3{HUGE_VAL, HUGE_VAL, HUGE_VAL};
    hi = Vec3{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
    for (const PortRemastered::ModelVertexBuffer& vb : cmdl.vertexBuffers) {
      for (size_t v = 0; v + 2 < vb.positions.size(); v += 3) {
        for (size_t k = 0; k < 3; ++k) {
          lo[k] = std::min(lo[k], double(vb.positions[v + k]));
          hi[k] = std::max(hi[k], double(vb.positions[v + k]));
        }
      }
    }
    return lo[0] <= hi[0];
  };
  // Whether a static model's bounds hold its origin, give or take half a metre.
  auto aroundOrigin = [&](const uint8_t* id) {
    Vec3 lo, hi;
    if (!modelBounds(id, lo, hi)) {
      return false;
    }
    for (size_t k = 0; k < 3; ++k) {
      if (!(lo[k] <= 0.5 && hi[k] >= -0.5)) {
        return false;
      }
    }
    return true;
  };
  // The float driver of an actor's material variable: its model's material of that name binds
  // the variable to a kind parameter the port can drive, and exactly one start (a retail
  // object's state, after the timers' wait) is traced. One that does not hold keeps the
  // actor as it was: left out when timed off.
  auto driverOf = [&](const Component& c, const uint8_t* modelId, PortRoomGeo::Instance::Driver& out) {
    const auto found = scripts.drivers.find(c.entity);
    if (found == scripts.drivers.end() || found->second.size() != 1 || modelId == nullptr) {
      return false;
    }
    const SceneryScripts::Driver& d = found->second[0];
    const auto starts = scripts.links.find(d.start);
    PortRoomGeo::Link first;
    if (starts == scripts.links.end() || !SingleDriverStart(starts->second, first) ||
        first.delay > PortRoomGeo::kMaxDriverDelay) {
      return false;
    }
    std::vector<uint8_t> data;
    PortRemastered::Model cmdl;
    std::string error;
    if (!FindResource(modelId, FourCC("CMDL"), home, data, nullptr, nullptr) ||
        !PortRemastered::ParseModel(data.data(), data.size(), cmdl, error)) {
      return false;
    }
    const int slot = DriverSlot(cmdl.materials, d);
    if (slot < 0) {
      return false;
    }
    out.slot = uint8_t(slot);
    out.sender = first.sender;
    out.state = first.state;
    out.delay = first.delay;
    out.spline = d.spline;
    return true;
  };
  size_t posed = 0, drivenActors = 0;
  // The synthetic-model actors written (m_syntheticEntities): their instance and the world
  // box (GC axes) their model covers, for the retail objects they stand in for.
  struct Standin {
    uint32_t instance;
    Vec3 pos, lo, hi;
  };
  std::vector<Standin> standins;
  std::vector<const Component*> renderActors = r.room.Of(kActorMP1);
  const auto renderPlatforms = r.room.Of(kPlatformComponent);
  renderActors.insert(renderActors.end(), renderPlatforms.begin(), renderPlatforms.end());
  for (const Component* c : renderActors) {
    const bool isPlatform = c->type == kPlatformComponent;
    const auto f = r.room.Flat(*c);
    const auto prop = f.find(isPlatform ? kPropPlatformModel : kPropActorModel);
    const bool hasModel = prop != f.end() && prop->second.size == 16;
    Span chprProp, nameProp;
    const uint32_t animProperty = isPlatform ? kPropPlatformAnim : kPropActorAnim;
    const bool hasAnim = !hasModel && r.room.Nested(*c, {animProperty, kPropAnimCharacter}, chprProp) &&
                         chprProp.size == 16 && r.room.Nested(*c, {animProperty, kPropAnimName}, nameProp) &&
                         nameProp.size == 8;
    Vec3 pos, rot, scale;
    const bool synthetic = hasModel && m_syntheticEntities.count(c->entity) != 0;
    if ((!isPlatform && f.find(kPropActorAdded) == f.end() && !synthetic) || (!hasModel && !hasAnim) ||
        !r.room.Xform(*c, pos, rot, scale)) {
      continue;
    }
    const bool active = r.room.Active(*c);
    PortRoomGeo::Instance::Driver driver;
    const bool driven = hasModel && driverOf(*c, r.room.Bytes(prop->second), driver);
    if (!active && (!canShow(c->entity) || (timedOff.count(c->entity) != 0 && !hasAnim && !driven && !showsAndHides(c->entity)))) {
      ++inactive;
      continue;
    }
    // A model-less actor that plays an animation draws its character's skinned model,
    // posed by the animation's frames. A character of one bone (the Intro Elevator rings,
    // whose bone binds at the model's origin) is drawn whole, the pose moving the whole
    // model; one of more is cut into rigid pieces (`pieces`).
    const PortRemasteredAnim::Anim* anim = nullptr;
    Id16 model{};
    if (hasModel) {
      model = SwapUuid(r.room.Bytes(prop->second));
    } else {
      const Id16 key = SwapUuid(r.room.Bytes(chprProp));
      auto ch = characters.find(key);
      if (ch == characters.end()) {
        std::vector<uint8_t> data;
        std::string error;
        auto character = std::make_unique<PortRemasteredAnim::Character>();
        if (!FindResource(r.room.Bytes(chprProp), FourCC("CHPR"), home, data, nullptr, nullptr) ||
            !PortRemasteredAnim::ReadCharacter(data, *character, error)) {
          Log("  " + r.name + ": a character did not read" + (error.empty() ? "" : ": " + error));
          character.reset();
        }
        ch = characters.emplace(key, std::move(character)).first;
      }
      std::string name;
      if (ch->second != nullptr &&
          r.room.String(ReadLE32(r.room.Bytes(nameProp)), ReadLE32(r.room.Bytes(nameProp) + 4), name)) {
        anim = PortRemasteredAnim::Find(*ch->second, name);
      }
      if (anim == nullptr || anim->frames < 2 || !(anim->fps > 0.f)) {
        ++dropped;
        continue;
      }
      model = SwapUuid(ch->second->skinnedModel.data());
      if (model == Id16{}) {
        ++dropped;
        continue;
      }
      if (anim->bones.size() != 1 || anim->bones[0].size() != anim->frames) {
        if (m_io.cancelled && m_io.cancelled()) {
          return;
        }
        if (!pieces(*c, *ch->second, *anim, model, pos, rot, scale, active)) {
          ++dropped;
          continue;
        }
        ++animated;
        ++actors;
        continue;
      }
    }
    if (model == Id16{}) {
      continue;
    }
    // A static one that starts hidden at the room's origin, its model built around its own
    // origin, is posed by a cinematic, which the port does not play: the gunships of the
    // Frigate hangar's landing cutscene, shown by retail's links, would stand whole on the
    // dock. One whose model lies off its origin is room geometry placed at the origin (the
    // Frigate reactor's fallen pieces), and stays.
    if (!isPlatform && hasModel && !active && MaxAbs(pos, Vec3{}) < 0.01 && aroundOrigin(r.room.Bytes(prop->second))) {
      ++posed;
      continue;
    }
    // Some carry a retail model (Remastered's id 10000000-0000-f000-f000-0000XXXXXXXX)
    // and stand where a retail Actor with that model already does, a little moved at most:
    // the pipes and fittings of the Frigate's hangar. The retail actor draws it, as the
    // Remastered model when the import table replaces it, so it is not written again.
    if (const uint8_t* id = hasModel ? r.room.Bytes(prop->second) : nullptr;
        id != nullptr && std::memcmp(id, kRetailIdPrefix, 12) == 0) {
      const Vec3 w = Apply(area.xf, MulR2G(pos));
      const bool standing = std::any_of(area.objects.begin(), area.objects.end(), [&](const ScriptObject& o) {
        return o.type == (isPlatform ? kRetailPlatform : kRetailActor) && o.hasPos && MaxAbs(o.pos, w) < kRetailActorNear &&
               std::search(o.props.begin(), o.props.end(), id + 12, id + 16) != o.props.end();
      });
      if (standing || m_retailEntities.count(c->entity) != 0) {
        ++retailDrawn;
        continue;
      }
    }
    auto known = actorModels.find(model);
    if (known == actorModels.end()) {
      if (m_io.cancelled && m_io.cancelled()) {
        return;
      }
      uint32_t id = 0;
      known = actorModels.emplace(model, m_io.model(model, id) ? id : 0).first;
    }
    if (known->second == 0) {
      ++dropped;
      continue;
    }
    PortRoomGeo::Instance& inst = instances.emplace_back();
    inst.model = known->second;
    place(inst, pos, rot, scale);
    if (Vec3 lo, hi; synthetic && modelBounds(r.room.Bytes(prop->second), lo, hi)) {
      Standin& s = standins.emplace_back();
      s.instance = uint32_t(instances.size() - 1);
      s.pos = Apply(area.xf, MulR2G(pos));
      s.lo = Vec3{HUGE_VAL, HUGE_VAL, HUGE_VAL};
      s.hi = Vec3{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
      for (int corner = 0; corner < 8; ++corner) {
        const Vec3 v = MulR2G(Vec3{(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]});
        Vec3 local;
        for (size_t row = 0; row < 3; ++row) {
          local[row] = inst.transform[4 * row] * v[0] + inst.transform[4 * row + 1] * v[1] +
                       inst.transform[4 * row + 2] * v[2] + inst.transform[4 * row + 3];
        }
        const Vec3 w = Apply(area.xf, local);
        for (size_t k = 0; k < 3; ++k) {
          s.lo[k] = std::min(s.lo[k], w[k]);
          s.hi[k] = std::max(s.hi[k], w[k]);
        }
      }
    }
    if (anim != nullptr) {
      // The bone's pose in GC axes: the axis change is a rotation, so a quaternion's
      // vector part and a translation change the same way a position does.
      PortRoomGeo::Instance::AnimClip& clip = inst.anim.emplace_back();
      clip.fps = anim->fps;
      clip.keys.reserve(size_t(anim->frames) * 7);
      for (const PortRemasteredAnim::Key& key : anim->bones[0]) {
        for (int i = 0; i < 3; ++i) {
          clip.keys.push_back(float(kSign[i] * key.rotation[kAxis[i]]));
        }
        clip.keys.push_back(key.rotation[3]);
        for (int i = 0; i < 3; ++i) {
          clip.keys.push_back(float(kSign[i] * key.translation[kAxis[i]]));
        }
      }
      ++animated;
    }
    script(inst, c->entity, active);
    if (isPlatform) {
      const auto bound = scripts.platformTransform.find(c->entity);
      if (bound != scripts.platformTransform.end() && bound->second->hasXf) {
        const ScriptObject& carrier = *bound->second;
        // Model scale is separate from the retail actor's rigid transform.
        // local = inverse(retail initial rigid) * area * placed model.
        double rotation[3][3];
        EditorBasis(carrier.rot, Vec3{1, 1, 1}, rotation);
        float local[12]{};
        for (int row = 0; row < 3; ++row) {
          for (int col = 0; col < 4; ++col) {
            double v = 0;
            for (int k = 0; k < 3; ++k) {
              double world = col == 3 ? area.xf[k][3] - carrier.pos[k] : 0.;
              for (int j = 0; j < 3; ++j) world += area.xf[k][j] * inst.transform[j * 4 + col];
              v += rotation[k][row] * world;
            }
            local[row * 4 + col] = float(v);
          }
        }
        std::copy(local, local + 12, inst.transform);
        inst.links.push_back({carrier.id, 0, PortRoomGeo::kActorTransform});
        // A directly matched model replaces the retail platform's visual;
        // a co-driven sibling supplements it and must not hide the carrier.
        const auto own = scripts.retail.find(c->entity);
        if (own != scripts.retail.end() && &area.objects[own->second] == &carrier) {
          scripts.script.hidden.push_back({carrier.id & 0x3ffffffu, uint32_t(instances.size() - 1)});
        }
        ++riding;
      }
    }
    if (driven) {
      inst.drivers.push_back(driver);
      ++drivenActors;
    }
    if (lightmapOk && c->hasGuid) {
      const auto lookup = r.room.Lighting().lookups.find(c->guid);
      if (lookup != r.room.Lighting().lookups.end()) {
        setLookup(inst, lookup->second.data());
      }
    }
    const auto glow = glows.find(size_t(c - comps.data()));
    if (glow != glows.end()) {
      inst.glows = true;
      std::copy(glow->second.begin(), glow->second.end(), inst.glow);
      ++glowing;
    }
    const auto platform = scripts.platform.find(c->entity);
    if (platform != scripts.platform.end()) {
      inst.platform = platform->second->id;
      for (int i = 0; i < 3; ++i) {
        inst.platformStart[i] = float(platform->second->pos[i]);
      }
      ++riding;
    }
    ++actors;
  }
  // The retail Actors the synthetic-model actors stand in for, hidden while those are drawn:
  // one standing where such an actor does, and one Remastered dropped whose origin its model
  // covers (Central Dynamo's three grates under the wider centre one). Neither may draw a model
  // the room keeps under its retail id, nor (the dropped one) stand where any entity does.
  size_t standinHidden = 0;
  if (!standins.empty()) {
    for (const ScriptObject& o : area.objects) {
      if (o.type != kRetailActor || !o.hasPos ||
          std::any_of(m_roomRetailIds.begin(), m_roomRetailIds.end(), [&](uint32_t id) {
            const uint8_t be[4] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
            return std::search(o.props.begin(), o.props.end(), be, be + 4) != o.props.end();
          })) {
        continue;
      }
      const Standin* by = nullptr;
      for (const Standin& s : standins) {
        if (MaxAbs(s.pos, o.pos) < kRetailActorNear) {
          by = &s;
          break;
        }
      }
      if (by == nullptr && std::none_of(m_entityPositions.begin(), m_entityPositions.end(), [&](const Vec3& p) {
            return MaxAbs(p, o.pos) < kRetailActorNear;
          })) {
        for (const Standin& s : standins) {
          if (o.pos[0] >= s.lo[0] - kStandinMargin && o.pos[0] <= s.hi[0] + kStandinMargin &&
              o.pos[1] >= s.lo[1] - kStandinMargin && o.pos[1] <= s.hi[1] + kStandinMargin &&
              o.pos[2] >= s.lo[2] - kStandinMargin && o.pos[2] <= s.hi[2] + kStandinMargin) {
            by = &s;
            break;
          }
        }
      }
      if (by != nullptr) {
        scripts.script.hidden.push_back({o.id & 0x3FFFFFFu, by->instance});
        ++standinHidden;
      }
    }
  }
  if (!m_syntheticEntities.empty()) {
    Log("  " + r.name + ": " + std::to_string(standins.size()) + " of " + std::to_string(m_syntheticEntities.size()) +
        " Remastered stand-in actors drawn, " + std::to_string(standinHidden) + " retail actors hidden");
  }
  // The room's skies, drawn in place of the world's: retail has one per world, Remastered
  // one per room, each turned and scaled to suit it (and two in the Frigate's hangar, one
  // on a layer).
  size_t skies = 0;
  for (const Component* c : r.room.Of(kSkybox)) {
    const auto f = r.room.Flat(*c);
    const auto prop = f.find(kPropSkyboxModel);
    Vec3 pos, rot, scale;
    if (prop == f.end() || prop->second.size != 16 || !r.room.Xform(*c, pos, rot, scale)) {
      continue;
    }
    const bool active = r.room.Active(*c);
    if (!active && !canShow(c->entity)) {
      ++inactive;
      continue;
    }
    const Id16 model = SwapUuid(r.room.Bytes(prop->second));
    if (m_io.cancelled && m_io.cancelled()) {
      return;
    }
    uint32_t id = 0;
    if (model == Id16{} || !m_io.model(model, id)) {
      ++dropped;
      continue;
    }
    PortRoomGeo::Instance& inst = instances.emplace_back();
    inst.model = id;
    inst.sky = true;
    SkyRadiance(r.room, *c, inst.skyRadiance);
    place(inst, Vec3{}, rot, scale);
    script(inst, c->entity, active);
    ++skies;
  }
  // A room with no Skybox of its own (not even one on a layer) carries the world master's,
  // which Remastered draws there (Phendrana's: !IceWorld's). It stands in for retail's world
  // sky only where retail draws that. The master's triggers that switch it are not followed.
  size_t worldSkies = 0;
  if (r.room.Of(kSkybox).empty()) {
    for (const WorldSky& sky : m_worldSkies) {
      if (m_io.cancelled && m_io.cancelled()) {
        return;
      }
      uint32_t id = 0;
      if (!m_io.model(sky.model, id)) {
        ++dropped;
        continue;
      }
      PortRoomGeo::Instance& inst = instances.emplace_back();
      inst.model = id;
      inst.sky = true;
      inst.worldSky = true;
      std::copy(sky.radiance, sky.radiance + 3, inst.skyRadiance);
      place(inst, Vec3{}, sky.rot, sky.scale);
      ++worldSkies;
    }
  }
  if (instances.empty()) {
    return;
  }
  const uint32_t count = uint32_t(instances.size());
  std::vector<PortRoomGeo::Script::Hidden>& hidden = scripts.script.hidden;
  std::sort(hidden.begin(), hidden.end());
  hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
  const std::vector<uint8_t> out = PortRoomGeo::Write(instances, &scripts.script);
  char file[32];
  std::snprintf(file, sizeof file, "%08X.roomgeo", mrea);
  if (!m_io.write || !m_io.write(file, out)) {
    Log("  " + r.name + ": could not write " + file);
    return;
  }
  Lightmap(r).lit = lookups;
  if (r.room.Lighting().HasLightmap()) {
    const LightmapData& lm = Lightmap(r);
    char text[160];
    if (lm.ok) {
      std::snprintf(text, sizeof text, "  %s: lightmap %s %ux%u x %zu layers, %zu of %u instances have a lookup",
                    r.name.c_str(), UuidText(r.room.Lighting().lightmap.data()).c_str(), lm.tex.width, lm.tex.height,
                    lm.tex.layers.size(), lookups, count);
    } else {
      std::snprintf(text, sizeof text, "  %s: no lightmap, %u instances", r.name.c_str(), count);
    }
    Log(text);
  }
  char line[512];
  std::snprintf(line, sizeof line,
                "  %s (%08X): %u instances (%zu from MCON, %zu actors, %zu skies, %zu world skies, %zu inactive objects left out; %zu on a layer, "
                "%zu scripted, %zu in %zu groups of %zu script objects and %zu connections (%zu untraced), %zu on a "
                "platform, %zu with untraced links, %zu glowing, %zu animated, %zu left to retail actors, %zu left to "
                "cinematics; %zu of %zu entities matched; %zu with a material driver), %zu dropped",
                r.name.c_str(), mrea, count, modcons, actors, skies, worldSkies, inactive, gated, linked, grouped, scripts.group.size(),
                scripts.script.nodes.size(), scripts.script.edges.size(), scripts.scriptUnresolved, riding, unresolved,
                glowing, animated, retailDrawn, posed, scripts.matched, scripts.entities, drivenActors, dropped);
  Log(line);
}

void Writer::WriteLiquids(const RoomData& r, uint32_t mrea) {
  // Liquids are room geometry: they come with it, so a models-only import keeps the
  // retail planes (and the small arena its mod starts with).
  if (!m_io.liquid || (m_io.wantsGeometry && !m_io.wantsGeometry(r.name))) {
    return;
  }
  static const int kAxis[3] = {0, 2, 1};
  static const double kSign[3] = {-1, 1, 1};
  // What each entity's retail water object is filled with, and every water object's camera
  // filter colour (CScriptWaterMP1+0x520, build/mpr/water/E-under.md Q1) where it stands.
  std::map<int, int> fluids;
  std::map<int, float> xrayOpacity;
  std::vector<uint8_t> filters;
  uint32_t filterCount = 0;
  for (const Component* c : r.room.Of(kWaterMP1)) {
    Span s;
    int type = RoomLiquid::kWater;
    if (r.room.Nested(*c, {kPropWaterFluid[0], kPropWaterFluid[1], kPropWaterFluid[2]}, s) && s.size >= 4) {
      const uint32_t fluid = ReadLE32(r.room.Bytes(s));
      type = fluid == 10 ? RoomLiquid::kPoison : fluid == 11 ? RoomLiquid::kLava : RoomLiquid::kWater;
    }
    fluids[c->entity] = type;
    if (r.room.Nested(*c, {kPropWaterXrayOpacity}, s) && s.size >= 4) {
      xrayOpacity[c->entity] = ReadLEFloat(r.room.Bytes(s));
    }
    Vec3 pos, rot, scale;
    if (r.room.Xform(*c, pos, rot, scale)) {
      for (int row = 0; row < 3; ++row) {
        AppendLEFloat(filters, kSign[row] * pos[kAxis[row]]);
      }
      for (const uint32_t channel : kPropColorRGBA) {
        // SLdrColor_MP1Typedef's default is (1, 1, 1, 1).
        float value = 1.f;
        if (r.room.Nested(*c, {kPropWaterFilterColor, channel}, s) && s.size >= 4) {
          value = ReadLEFloat(r.room.Bytes(s));
        }
        AppendLEFloat(filters, value);
      }
      ++filterCount;
    }
  }
  std::vector<uint8_t> body;
  uint32_t count = 0;
  size_t dropped = 0;
  for (const bool lava : {false, true}) {
    for (const Component* c : r.room.Of(lava ? kLavaRenderVolume : kWaterRenderVolume)) {
      const auto f = r.room.Flat(*c);
      const auto prop = f.find(lava ? kPropLavaModel : kPropWaterModel);
      Vec3 pos, rot, scale;
      if (prop == f.end() || prop->second.size != 16 || !r.room.Xform(*c, pos, rot, scale)) {
        continue;
      }
      RoomLiquid liquid;
      liquid.model = SwapUuid(r.room.Bytes(prop->second));
      if (liquid.model == Id16{}) {
        continue;
      }
      const auto fluid = fluids.find(c->entity);
      liquid.type = lava ? RoomLiquid::kLava : fluid != fluids.end() ? fluid->second : RoomLiquid::kWater;
      for (int i = 0; lava && i < 6; ++i) {
        if (const auto v = f.find(kPropLava[i]); v != f.end() && v->second.size == 4) {
          liquid.lava[i] = ReadLEFloat(r.room.Bytes(v->second));
        }
      }
      if (!lava) {
        Span s;
        // Each field is the room's, else the loader's default (RoomLiquid's).
        auto value = [&](std::initializer_list<uint32_t> path, float* out, int n) {
          if (r.room.Nested(*c, path, s) && s.size >= size_t(4 * n)) {
            for (int i = 0; i < n; ++i) {
              out[i] = ReadLEFloat(r.room.Bytes(s) + 4 * i);
            }
          }
        };
        auto texture = [&](std::initializer_list<uint32_t> path, Id16& out) {
          if (r.room.Nested(*c, path, s) && s.size == 16) {
            out = SwapUuid(r.room.Bytes(s));
          }
        };
        for (int i = 0; i < 11; ++i) {
          if (r.room.Nested(*c, {kPropWaterFeatures, kPropWaterFeature[i]}, s) && s.size >= 1) {
            liquid.features[i] = r.room.Bytes(s)[0] != 0 ? 1 : 0;
          }
        }
        for (int w = 0; w < 2; ++w) {
          for (int i = 0; i < 5; ++i) {
            value({kPropWaterWaves[w], kPropWave[i]}, &liquid.waves[w][i], 1);
          }
        }
        value({kPropWaterLook, kPropWaterTint}, liquid.tint, 4);
        texture({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalTexture}, liquid.normalMap);
        for (int i = 0; i < 2; ++i) {
          value({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalDir, kPropWaterNormalDirXY[i]},
                &liquid.normalDir[i], 1);
        }
        value({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalSpeed}, &liquid.normalSpeed, 1);
        value({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalScale}, &liquid.normalScale, 1);
        value({kPropWaterLook, kPropWaterFog, kPropWaterFogColor}, liquid.fogColor, 4);
        value({kPropWaterLook, kPropWaterFog, kPropWaterFogDistance}, &liquid.fogDistance, 1);
        texture({kPropWaterLook, kPropWaterRain, kPropWaterRainNoise}, liquid.rainNoise);
        for (int i = 0; i < 10; ++i) {
          value({kPropWaterLook, kPropWaterRain, kPropWaterRainValue[i]}, &liquid.rain[i], 1);
          value({kPropWaterLook, kPropWaterFlow, kPropWaterFlowValue[i]}, &liquid.flow[i], 1);
        }
        texture({kPropWaterLook, kPropWaterFlow, kPropWaterFlowMap}, liquid.flowMap);
        if (const auto x = xrayOpacity.find(c->entity); x != xrayOpacity.end()) {
          liquid.xrayOpacity = x->second;
        }
        for (int i = 0; i < 5; ++i) {
          value({kPropWaterLook, kPropWaterMaterial[i]}, &liquid.material[i], 1);
        }
      }
      if (m_io.cancelled && m_io.cancelled()) {
        return;
      }
      uint32_t id = 0;
      RoomWaterAssets assets;
      if (lava ? !m_io.liquid(liquid, id) : !m_io.water || !m_io.water(liquid, assets)) {
        ++dropped;
        continue;
      }
      double s[3], k[3];
      for (int i = 0; i < 3; ++i) {
        s[i] = std::sin(rot[i] * (3.14159265358979323846 / 180.0));
        k[i] = std::cos(rot[i] * (3.14159265358979323846 / 180.0));
      }
      const double m[3][3] = {
          {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
          {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
          {-s[1], k[1] * s[0], k[1] * k[0]},
      };
      AppendLE32(body, uint32_t(liquid.type));
      AppendLE32(body, id);
      for (int row = 0; row < 3; ++row) {
        // A lava pool is converted into the area's axes. A water mesh is not: it stays in
        // Remastered's model space and the transform takes it there, (x, y, z) -> (-x, z, y).
        double col[3];
        for (int c = 0; c < 3; ++c) {
          col[c] = kSign[row] * kSign[c] * m[kAxis[row]][kAxis[c]];
        }
        if (!lava) {
          const double old[3] = {col[0], col[1], col[2]};
          col[0] = -old[0];
          col[1] = old[2];
          col[2] = old[1];
        }
        for (int c = 0; c < 3; ++c) {
          // The entity's scale is the size of the volume; the model is in units already.
          AppendLEFloat(body, col[c]);
        }
        AppendLEFloat(body, kSign[row] * pos[kAxis[row]]);
      }
      if (!lava) {
        AppendLE32(body, uint32_t(assets.vertices.size()));
        AppendLE32(body, uint32_t(assets.indices.size()));
        for (const float v : assets.boundsMin) {
          AppendLEFloat(body, v);
        }
        for (const float v : assets.boundsMax) {
          AppendLEFloat(body, v);
        }
        for (const uint8_t v : liquid.features) {
          body.push_back(v);
        }
        body.push_back(0);
        auto floats = [&](const float* v, int n) {
          for (int i = 0; i < n; ++i) {
            AppendLEFloat(body, v[i]);
          }
        };
        floats(liquid.waves[0], 5);
        floats(liquid.waves[1], 5);
        floats(liquid.tint, 4);
        floats(liquid.normalDir, 2);
        floats(&liquid.normalSpeed, 1);
        floats(&liquid.normalScale, 1);
        floats(liquid.fogColor, 4);
        floats(&liquid.fogDistance, 1);
        floats(liquid.material, 5);
        floats(liquid.rain, 10);
        floats(liquid.flow, 10);
        floats(&liquid.xrayOpacity, 1);
        AppendLE32(body, assets.normalMap);
        AppendLE32(body, assets.flowMap);
        AppendLE32(body, assets.rainNoise);
        AppendLE32(body, assets.rainNoiseWidth);
        AppendLE32(body, assets.rainNoiseHeight);
        for (const RoomWaterAssets::Vertex& v : assets.vertices) {
          floats(v.pos, 3);
          floats(v.uv, 4);
          body.insert(body.end(), v.color, v.color + 4);
        }
        for (const uint32_t i : assets.indices) {
          AppendLE32(body, i);
        }
      }
      ++count;
    }
  }
  if (count == 0 && filterCount == 0) {
    return;
  }
  std::vector<uint8_t> out;
  AppendLE32(out, 0x4C52504D);  // 'MPRL'
  AppendLE32(out, 4);
  AppendLE32(out, count);
  out.insert(out.end(), body.begin(), body.end());
  AppendLE32(out, filterCount);
  out.insert(out.end(), filters.begin(), filters.end());
  char file[32];
  std::snprintf(file, sizeof file, "%08X.roomliquid", mrea);
  if (!m_io.write || !m_io.write(file, out)) {
    Log("  " + r.name + ": could not write " + file);
    return;
  }
  char line[160];
  std::snprintf(line, sizeof line, "  %s: %u liquid surfaces, %zu dropped, %u water filters", r.name.c_str(),
                count, dropped, filterCount);
  Log(line);
}

// Rz * Ry * Rx of `rot` (degrees), then `scale`, column by column: an editor transform.
static void EditorBasis(const Vec3& rot, const Vec3& scale, double out[3][3]) {
  double s[3], k[3];
  for (int i = 0; i < 3; ++i) {
    s[i] = std::sin(rot[size_t(i)] * (3.14159265358979323846 / 180.0));
    k[i] = std::cos(rot[size_t(i)] * (3.14159265358979323846 / 180.0));
  }
  const double m[3][3] = {
      {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
      {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
      {-s[1], k[1] * s[0], k[1] * k[0]},
  };
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      out[row][col] = m[row][col] * scale[size_t(col)];
    }
  }
}

// Remastered keeps many retail models under their retail ids and places them with entities of
// its own. The ROOM keeps no retail editor id (an entity's IDTA is a guid of its own), but its
// entities stand in the retail area's script order: the objects of a layer in turn, each in its
// SCLY order. So the n-th object of the area naming a model is the n-th entity of the room
// carrying it, when the two counts are equal. The retail object draws the model, so the model's
// transform in its own space is where the entity puts it as the object sees it: the object's
// inverse transform times the entity's (orient) and applied to the entity's position (offset).
void Writer::PairRetailModels(const RoomData& r, const Area& area) {
  m_retailEntities.clear();
  m_syntheticEntities.clear();
  m_roomRetailIds.clear();
  m_entityPositions.clear();
  if (!m_io.retailModel) {
    return;
  }
  // kR2G as a signed permutation, as WriteGeometry places an entity.
  static const int kAxis[3] = {0, 2, 1};
  static const double kSign[3] = {-1, 1, 1};
  struct Placed {
    int entity;
    double basis[3][3];  // in GC axes, the area's rotation included
    Vec3 pos;            // in GC world space
  };
  std::map<uint32_t, std::pair<Id16, std::vector<Placed>>> byRetail;
  std::vector<int> seenEntities;
  for (const Component& c : r.room.Components()) {
    Vec3 pos, rot, scale;
    if (c.entity < 0 || c.raw.size < 16 || !r.room.Xform(c, pos, rot, scale)) {
      continue;
    }
    if (std::find(seenEntities.begin(), seenEntities.end(), c.entity) == seenEntities.end()) {
      seenEntities.push_back(c.entity);
      m_entityPositions.push_back(Apply(area.xf, MulR2G(pos)));
    }
    const uint8_t* b = r.room.Bytes(c.raw);
    for (size_t at = 0; at + 16 <= c.raw.size; ++at) {
      if (std::memcmp(b + at, kRetailIdPrefix, 12) != 0) {
        continue;
      }
      auto& [model, placed] = byRetail[ReadBE32(b + at + 12)];
      model = SwapUuid(b + at);
      at += 15;
      if (std::any_of(placed.begin(), placed.end(), [&](const Placed& p) { return p.entity == c.entity; })) {
        continue;
      }
      Placed& p = placed.emplace_back();
      p.entity = c.entity;
      p.pos = Apply(area.xf, MulR2G(pos));
      double rem[3][3], gc[3][3];
      EditorBasis(rot, scale, rem);
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          gc[row][col] = kSign[row] * kSign[col] * rem[kAxis[row]][kAxis[col]];
        }
      }
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          p.basis[row][col] = area.xf[size_t(row)][0] * gc[0][col] + area.xf[size_t(row)][1] * gc[1][col] +
                              area.xf[size_t(row)][2] * gc[2][col];
        }
      }
    }
  }
  for (auto& [retail, entry] : byRetail) {
    auto& [model, placed] = entry;
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.entity < b.entity; });
    const uint8_t be[4] = {uint8_t(retail >> 24), uint8_t(retail >> 16), uint8_t(retail >> 8), uint8_t(retail)};
    std::vector<const ScriptObject*> named;
    for (const ScriptObject& o : area.objects) {
      if (o.hasXf && std::search(o.props.begin(), o.props.end(), be, be + 4) != o.props.end()) {
        named.push_back(&o);
      }
    }
    if (named.empty()) {
      for (const Placed& p : placed) {
        m_syntheticEntities.insert(p.entity);
      }
      continue;
    }
    m_roomRetailIds.insert(retail);
    for (size_t n = 0; n < named.size(); ++n) {
      const ScriptObject& o = *named[n];
      RetailPlacement out;
      std::copy(model.begin(), model.end(), out.model.begin());
      out.retail = retail;
      out.mrea = area.mrea;
      out.object = o.id;
      out.room = r.name;
      double basis[3][3], inverse[3][3];
      EditorBasis(o.rot, o.scale, basis);
      if (named.size() == placed.size() && Invert3(basis, inverse)) {
        const Placed& p = placed[n];
        out.ok = true;
        out.entity = p.entity;
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            out.orient[i][j] =
                inverse[i][0] * p.basis[0][j] + inverse[i][1] * p.basis[1][j] + inverse[i][2] * p.basis[2][j];
          }
          out.offset[i] = inverse[i][0] * (p.pos[0] - o.pos[0]) + inverse[i][1] * (p.pos[1] - o.pos[1]) +
                          inverse[i][2] * (p.pos[2] - o.pos[2]);
        }
        m_retailEntities.insert(p.entity);
      }
      m_io.retailModel(out);
    }
  }
}

std::string Writer::WriteRoom(const RoomData& r, const std::map<std::string, Placement>& placed, const Vec3& shift,
                              const float tonemap[5], const BloomData& worldBloom,
                              const std::vector<GradeData>& worldGrades,
                              const std::vector<BacklightData>& worldBacklights, const std::vector<FogData>& worldFogs,
                              const std::vector<PortRoomEnv::WorldVolume>& worldVolumes, const Match& m, int& written,
                              std::string& matched) {
  matched.clear();
  char head[96];
  PairRetailModels(r, *m.area);
  WriteGeometry(r, m.mrea, *m.area);
  WriteLiquids(r, m.mrea);
  const std::vector<Vec3>& gdoors = m.area->doors;
  double rot[3][3], trans[3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      rot[i][j] = m.a[size_t(i)][size_t(j)];
    }
    trans[i] = m.a[size_t(i)][3];
  }

  const SceneryScripts scripts = MatchScripts(r.room, *m.area);
  {
    const std::string line = WriteRoomEffects(r.name, r.room, m.mrea, *m.area, scripts, m_io);
    if (!line.empty()) {
      Log(line);
    }
  }
  std::vector<uint8_t> probes;
  size_t probeCount = 0;
  std::vector<std::vector<uint8_t>> cubes;
  std::map<Id16, size_t> index;
  for (const Component* c : r.room.Of(kReflectionProbe)) {
    const auto f = r.room.Flat(*c);
    Vec3 pos, euler, scale;
    const auto reflProp = f.find(kPropProbeRefl);
    if (reflProp == f.end() || reflProp->second.size < 16 || !r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    const RoomPak home{r.name, r.pak};
    std::vector<uint8_t> refl;
    if (!FindResource(r.room.Bytes(reflProp->second), FourCC("REFL"), home, refl, nullptr, nullptr) ||
        refl.size() < 0x50) {
      continue;
    }
    const float probeScale = ReadLEFloat(&refl[0x4c]);
    const Pak* txtrPak = nullptr;
    Id16 txtrId;
    std::vector<uint8_t> txtr;
    if (!FindResource(&refl[0x3c], FourCC("TXTR"), home, txtr, &txtrPak, &txtrId)) {
      continue;
    }
    if (index.find(txtrId) == index.end()) {
      TxtrCubeBc6h cube;
      std::string error;
      if (!ReadTxtrCubeBc6h(txtr.data(), txtr.size(), cube, error)) {
        continue;
      }
      std::vector<uint8_t> blob;
      AppendLE32(blob, cube.size);
      AppendLE32(blob, cube.mipCount);
      AppendLE32(blob, cube.isSigned ? 1 : 0);
      size_t bytes = 0;
      for (const auto& mip : cube.mips) {
        bytes += mip.size();
      }
      AppendLE32(blob, uint32_t(bytes));
      for (const auto& mip : cube.mips) {
        blob.insert(blob.end(), mip.begin(), mip.end());
      }
      index[txtrId] = cubes.size();
      cubes.push_back(std::move(blob));
    }
    // Remastered tests the world box around the rotated unit box (half = |R| |scale| / 2) and samples
    // the cube with the world direction, so a rotation only grows the box. Rz Ry Rx in degrees; the
    // order does not matter for the disc's probes (one axis, or under 1.2 degrees off one).
    double rr[3][3];
    {
      const double d = 3.14159265358979323846 / 180.0;
      const double cx = std::cos(euler[0] * d), sx = std::sin(euler[0] * d);
      const double cy = std::cos(euler[1] * d), sy = std::sin(euler[1] * d);
      const double cz = std::cos(euler[2] * d), sz = std::sin(euler[2] * d);
      const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
      const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
      const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
      double zy[3][3];
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          zy[i][j] = rz[i][0] * ry[0][j] + rz[i][1] * ry[1][j] + rz[i][2] * ry[2][j];
        }
      }
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          rr[i][j] = zy[i][0] * rx[0][j] + zy[i][1] * rx[1][j] + zy[i][2] * rx[2][j];
        }
      }
    }
    Vec3 remHalf;
    for (int i = 0; i < 3; ++i) {
      remHalf[size_t(i)] = 0;
      for (int j = 0; j < 3; ++j) {
        remHalf[size_t(i)] += std::fabs(rr[i][j]) * std::fabs(scale[size_t(j)]) / 2;
      }
    }
    const Vec3 centre = MulR2G(pos);
    Vec3 half = MulR2G(remHalf);
    for (double& v : half) {
      v = std::max(std::fabs(v), 1e-4);
    }
    // world -> area -> unit box; inv is the area's rotation transposed.
    double inv[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        inv[i][j] = rot[j][i];
      }
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        AppendLEFloat(probes, inv[i][j] / half[size_t(i)]);
      }
      const double it = (inv[i][0] * trans[0] + inv[i][1] * trans[1]) + inv[i][2] * trans[2];
      AppendLEFloat(probes, (-it - centre[size_t(i)]) / half[size_t(i)]);
    }
    // CUBE @ R2G.T @ inv, in two products with CUBE the identity, to keep numpy's signs of zero.
    constexpr double kCube[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double cr[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        cr[i][j] = (kCube[i][0] * kR2G[j][0] + kCube[i][1] * kR2G[j][1]) + kCube[i][2] * kR2G[j][2];
      }
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        AppendLEFloat(probes, (cr[i][0] * inv[0][j] + cr[i][1] * inv[1][j]) + cr[i][2] * inv[2][j]);
      }
    }
    const auto prop = [&](uint32_t id, float fallback) {
      const auto p = f.find(id);
      return p != f.end() && p->second.size >= 4 ? ReadLEFloat(r.room.Bytes(p->second)) : fallback;
    };
    const auto priority = f.find(kPropProbePriority);
    // The retail layer its own layer is drawn on (MatchScripts), as for colour grades.
    const auto layer = scripts.layer.find(c->entity);
    AppendLE32(probes, uint32_t(layer != scripts.layer.end() ? int32_t(layer->second) : -1));
    AppendLE32(probes, uint32_t(index[txtrId]));
    AppendLEFloat(probes, probeScale);
    AppendLEFloat(probes, prop(kPropProbePadding, 1.0f));
    AppendLE32(probes, priority != f.end() && priority->second.size >= 4 ? ReadLE32(r.room.Bytes(priority->second)) : 0u);
    AppendLEFloat(probes, prop(kPropProbeMin, 0.0f));
    AppendLEFloat(probes, prop(kPropProbeMax, 1.0f));
    ++probeCount;
  }

  // Only a room the world shift lands on its area has a grid in the right place.
  std::vector<uint8_t> grid;
  std::string gnote = "grid not placed";
  const auto it = placed.find(r.name);
  if (it != placed.end() && Distance(it->second.pos, {trans[0], trans[1], trans[2]}) < 0.5) {
    Grid(RoomPak{r.name, r.pak}, shift, gdoors, grid, gnote);
  }
  const LightmapData& lightmap = Lightmap(r);
  const bool withLightmap = lightmap.ok && lightmap.lit != 0;
  if (cubes.empty() && grid.empty() && !withLightmap) {
    return r.name + ": no probes, " + gnote;
  }

  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  AppendLE32(out, 26);
  float tone[5];
  std::copy(tonemap, tonemap + 5, tone);
  Tonemap(r, tone);
  for (int i = 0; i < 4; ++i) {
    AppendLEFloat(out, tone[i]);
  }
  AppendLE32(out, uint32_t(probeCount));
  AppendLE32(out, uint32_t(cubes.size()));
  out.insert(out.end(), probes.begin(), probes.end());
  for (const auto& blob : cubes) {
    out.insert(out.end(), blob.begin(), blob.end());
  }
  AppendLE32(out, grid.empty() ? 0 : 1);
  out.insert(out.end(), grid.begin(), grid.end());
  float exposure[5];
  std::vector<PortRoomEnv::FluidHint> fluidHints;
  Exposure(r, exposure, &fluidHints);
  AppendLEFloat(out, exposure[0]);
  AppendLEFloat(out, exposure[1]);
  AppendLEFloat(out, exposure[2]);
  AppendLEFloat(out, tone[4]);
  BloomData bloom = worldBloom;
  ReadBloom(r, bloom);
  AppendLEFloat(out, bloom.threshold);
  AppendLE32(out, bloom.present ? uint32_t(bloom.tints.size() / 4) : 0);
  if (bloom.present) {
    for (const float v : bloom.tints) {
      AppendLEFloat(out, v);
    }
  }
  std::vector<GradeData> grades = worldGrades;
  for (GradeData& h : grades) {
    h.master = true;
  }
  ReadGrades(r, m.area, grades);
  AppendLE32(out, uint32_t(grades.size()));
  for (const GradeData& g : grades) {
    AppendLE32(out, uint32_t(g.layer));
    AppendLEFloat(out, g.fadeIn);
    AppendLEFloat(out, g.fadeOut);
    out.push_back(g.on ? 1 : 0);
    out.push_back(g.master ? 1 : 0);
    out.insert(out.end(), 2, 0);
    AppendLE32(out, uint32_t(g.priority));
    AppendLE32(out, uint32_t(g.links.size()));
    for (const PortRoomGeo::Link& link : g.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    out.insert(out.end(), g.lut.begin(), g.lut.end());
  }
  AppendLEFloat(out, exposure[3]);
  AppendLEFloat(out, exposure[4]);
  std::vector<BacklightData> backlights = worldBacklights;
  for (BacklightData& h : backlights) {
    h.master = true;
  }
  ReadBacklights(r, m.area, backlights);
  AppendLE32(out, uint32_t(backlights.size()));
  for (const BacklightData& b : backlights) {
    AppendLE32(out, uint32_t(b.layer));
    AppendLEFloat(out, b.fadeIn);
    AppendLEFloat(out, b.fadeOut);
    out.push_back(b.on ? 1 : 0);
    out.push_back(b.master ? 1 : 0);
    out.insert(out.end(), 2, 0);
    AppendLE32(out, uint32_t(b.priority));
    AppendLEFloat(out, b.top);
    AppendLEFloat(out, b.back);
    AppendLE32(out, uint32_t(b.links.size()));
    for (const PortRoomGeo::Link& link : b.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
  }
  std::vector<FogData> fogs = worldFogs;
  for (FogData& h : fogs) {
    h.master = true;
  }
  ReadFogs(r, m.area, fogs);
  AppendLE32(out, uint32_t(fogs.size()));
  for (const FogData& g : fogs) {
    AppendLE32(out, uint32_t(g.layer));
    for (const std::vector<uint8_t>* fade : {&g.fadeIn, &g.fadeOut}) {
      PortMayaSpline sline;
      AppendLEFloat(out, sline.Load(fade->data(), fade->size()) ? std::max(0.f, sline.LastTime()) : 0.f);
    }
    out.push_back(g.on ? 1 : 0);
    out.push_back(g.master ? 1 : 0);
    out.insert(out.end(), 2, 0);
    AppendLE32(out, uint32_t(g.priority));
    for (size_t i = 0; i < 10; ++i) {
      // The height term over retail z: the room's shift moves the heights.
      AppendLEFloat(out, i == 6 ? g.s[6] - g.s[5] * float(shift[2]) : g.s[i]);
    }
    for (float v : g.wind) {
      AppendLEFloat(out, v);
    }
    out.push_back(g.useScriptWind ? 1 : 0);
    out.push_back(g.noProbe ? 1 : 0);
    out.insert(out.end(), 2, 0);
    for (float v : g.colorB) {
      AppendLEFloat(out, v);
    }
    for (float v : g.colorA) {
      AppendLEFloat(out, v);
    }
    for (float v : g.lut) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, uint32_t(g.links.size()));
    for (const PortRoomGeo::Link& link : g.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    for (const std::vector<uint8_t>* fade : {&g.fadeIn, &g.fadeOut}) {
      AppendLE32(out, uint32_t(fade->size()));
      out.insert(out.end(), fade->begin(), fade->end());
      out.insert(out.end(), (4 - fade->size() % 4) % 4, 0);
    }
  }
  std::vector<FogRegionData> regions;
  std::vector<FogTransitionData> transitions;
  ReadFogRegions(r, scripts, m.a, regions, transitions);
  AppendLE32(out, uint32_t(regions.size()));
  for (const FogRegionData& g : regions) {
    AppendLE32(out, uint32_t(g.layer));
    out.push_back(g.on ? 1 : 0);
    out.push_back(g.fluid);
    out.push_back(g.hasColor ? 1 : 0);
    out.push_back(g.hasCap ? 1 : 0);
    for (const auto& row : g.m) {
      for (float v : row) {
        AppendLEFloat(out, v);
      }
    }
    for (float v : g.edgeScale) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, g.mult);
    for (float v : g.edgeBias) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, g.cap);
    for (float v : g.color) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, g.density);
    for (float v : g.box) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, uint32_t(g.links.size()));
    for (const PortRoomGeo::Link& link : g.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    AppendLEFloat(out, g.distance);
    AppendLEFloat(out, g.transmittance);
    out.push_back(g.subtract ? 1 : 0);
    out.insert(out.end(), 3, 0);
  }
  AppendLE32(out, uint32_t(transitions.size()));
  for (const FogTransitionData& t : transitions) {
    AppendLE32(out, t.region);
    AppendLE32(out, uint32_t(t.layer));
    out.push_back(t.on ? 1 : 0);
    out.push_back(t.autoStart ? 1 : 0);
    out.push_back(t.loop ? 1 : 0);
    out.push_back(t.select);
    AppendLEFloat(out, t.distance);
    AppendLEFloat(out, t.transmittance);
    for (float v : t.color) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, t.cap);
    AppendLE32(out, uint32_t(t.phase.size()));
    out.insert(out.end(), t.phase.begin(), t.phase.end());
    out.insert(out.end(), (4 - t.phase.size() % 4) % 4, 0);
    AppendLE32(out, uint32_t(t.links.size()));
    for (const PortRoomGeo::Link& link : t.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
  }
  std::vector<SunData> suns;
  ReadSuns(r, scripts, m.a, suns);
  AppendLE32(out, uint32_t(suns.size()));
  for (const SunData& s : suns) {
    AppendLE32(out, uint32_t(s.layer));
    out.push_back(s.on ? 1 : 0);
    out.insert(out.end(), 3, 0);
    for (float v : s.toSun) {
      AppendLEFloat(out, v);
    }
    for (float v : s.color) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, s.group);
    out.push_back(s.casts ? 1 : 0);
    out.insert(out.end(), 3, 0);
  }
  // The lightmap the room geometry's lookups index, or a width of 0.
  if (withLightmap) {
    const PortRemastered::TxtrLayersBc6h& tex = lightmap.tex;
    AppendLE32(out, tex.width);
    AppendLE32(out, tex.height);
    AppendLE32(out, uint32_t(tex.layers.size()));
    AppendLE32(out, tex.isSigned ? 1 : 0);
    size_t bytes = 0;
    for (const auto& layer : tex.layers) {
      bytes += layer.size();
    }
    AppendLE32(out, uint32_t(bytes));
    for (const auto& layer : tex.layers) {
      out.insert(out.end(), layer.begin(), layer.end());
    }
  } else {
    AppendLE32(out, 0);
  }
  std::vector<PointLightData> lights;
  ReadPointLights(r, scripts, m.a, lights);
  AppendLE32(out, uint32_t(lights.size()));
  for (const PointLightData& l : lights) {
    AppendLE32(out, uint32_t(l.layer));
    out.push_back(l.on ? 1 : 0);
    out.push_back(l.spot ? 1 : 0);
    out.push_back(l.falloff);
    out.push_back(0);
    for (float v : l.pos) {
      AppendLEFloat(out, v);
    }
    for (float v : l.toLight) {
      AppendLEFloat(out, v);
    }
    for (float v : l.color) {
      AppendLEFloat(out, v);
    }
    for (float v : l.nearFar) {
      AppendLEFloat(out, v);
    }
    for (float v : l.cone) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, l.group);
    AppendLE32(out, uint32_t(l.links.size()));
    for (const PortRoomGeo::Link& link : l.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    out.push_back(l.animated ? 1 : 0);
    out.push_back(l.playing ? 1 : 0);
    out.push_back(l.loop ? 1 : 0);
    out.push_back(l.offAtStart ? 1 : 0);
    out.push_back(l.animFalloff);
    out.insert(out.end(), 3, 0);
    AppendLEFloat(out, l.length);
    AppendLEFloat(out, l.scale[0]);
    AppendLEFloat(out, l.scale[1]);
    AppendLEFloat(out, l.intensity);
    for (const float c : l.rgb) {
      AppendLEFloat(out, c);
    }
    for (const std::vector<uint8_t>& spline : l.splines) {
      AppendLE32(out, uint32_t(spline.size()));
      out.insert(out.end(), spline.begin(), spline.end());
      out.insert(out.end(), (4 - spline.size() % 4) % 4, 0);
    }
  }
  AppendLE32(out, uint32_t(fluidHints.size()));
  for (const PortRoomEnv::FluidHint& h : fluidHints) {
    out.push_back(h.mode);
    out.push_back(h.role);
    out.push_back(h.pair);
    out.push_back(0);
    AppendLEFloat(out, h.exposure[0]);
    AppendLEFloat(out, h.exposure[1]);
    AppendLEFloat(out, h.bias);
    AppendLEFloat(out, h.sigma);
    AppendLEFloat(out, h.staticLerp);
  }
  AppendLE32(out, uint32_t(worldVolumes.size()));
  for (const PortRoomEnv::WorldVolume& v : worldVolumes) {
    for (const float* field : {v.centre, v.half}) {
      for (int i = 0; i < 3; ++i) {
        AppendLEFloat(out, field[i]);
      }
    }
    for (float a : v.axes) {
      AppendLEFloat(out, a);
    }
  }
  for (int i = 0; i < 3; ++i) {
    AppendLEFloat(out, float(shift[size_t(i)]));
  }
  {
    std::vector<std::string> notes;
    const float intensity = EnvFxIntensity(r.room, notes);
    AppendLEFloat(out, intensity);
    if (intensity != 1.f) {
      char text[96];
      std::snprintf(text, sizeof text, "EnvironmentFX colour intensity %g set at load", intensity);
      Log("  " + r.name + ": " + text);
    }
    for (const std::string& n : notes) {
      Log("  " + r.name + ": " + n);
    }
  }
  if (!fogs.empty() || !regions.empty()) {
    Log("  " + r.name + ": " + std::to_string(fogs.size()) + " fog hint(s), " + std::to_string(regions.size()) +
        " fog region(s), " + std::to_string(transitions.size()) + " transition(s)");
  }
  char file[32];
  std::snprintf(file, sizeof file, "%08X.roomenv", m.mrea);
  if (!m_io.write || !m_io.write(file, out)) {
    return r.name + ": could not write " + file;
  }
  ++written;
  std::snprintf(head, sizeof head, "%08X", m.mrea);
  matched = head;
  char tail[160];
  std::snprintf(tail, sizeof tail, ": %08X doors %.2f m, %zu probes, %zu cubes, ", m.mrea, m.err, probeCount,
                cubes.size());
  return r.name + tail + gnote + ", " + std::to_string(out.size() / 1024) + " KB";
}

bool Writer::Run(uint32_t mlvl, int& written, std::string& error) {
  written = 0;
  if (m_master.pak == nullptr) {
    error = "no master pak";
    return false;
  }
  if (!LoadAreas(mlvl, error)) {
    return false;
  }
  RoomData master;
  if (!ReadRoomFile(m_master, master, error)) {
    error = "master: " + error;
    return false;
  }

  // Where the world puts each room, by ROOM asset id, in GameCube world coordinates.
  std::map<Id16, Vec3> byId;
  for (const Component* c : master.room.Of(kRoomController)) {
    const auto f = master.room.Flat(*c);
    Vec3 pos, rot, scale;
    const auto id = f.find(kPropRoomId);
    if (id != f.end() && id->second.size == 16 && master.room.Xform(*c, pos, rot, scale)) {
      Id16 key;
      std::memcpy(key.data(), master.room.Bytes(id->second), 16);
      byId[key] = MulR2G(pos);
    }
  }

  std::vector<std::unique_ptr<RoomData>> rooms;
  for (const RoomPak& rp : m_rooms) {
    if (rp.name == m_master.name || (rp.name.size() >= 5 && rp.name.compare(rp.name.size() - 5, 5, "_Copy") == 0)) {
      continue;
    }
    auto r = std::make_unique<RoomData>();
    std::string roomError;
    if (rp.pak == nullptr || FirstOfType(*rp.pak, FourCC("ROOM")) == nullptr) {
      Log(rp.name + ": no ROOM");
      continue;
    }
    if (!ReadRoomFile(rp, *r, roomError)) {
      Log(rp.name + ": " + roomError);
      continue;
    }
    rooms.push_back(std::move(r));
  }

  std::map<std::string, Vec3> spots;
  std::vector<std::string> spotOrder;
  for (const auto& r : rooms) {
    const auto it = byId.find(r->id);
    if (it != byId.end()) {
      if (!spots.count(r->name)) {
        spotOrder.push_back(r->name);
      }
      spots[r->name] = it->second;
    }
  }
  if (spots.empty()) {
    error = "the master places no room";
    return false;
  }
  // Remastered moved each world's origin; the shift is the one that lands the most rooms on areas.
  auto hits = [&](const Vec3& shift) {
    int n = 0;
    for (const auto& [name, p] : spots) {
      double best = 1e300;
      for (const Area& a : m_areas) {
        best = std::min(best, Distance({a.xf[0][3], a.xf[1][3], a.xf[2][3]},
                                       {p[0] + shift[0], p[1] + shift[1], p[2] + shift[2]}));
      }
      n += best < 0.5;
    }
    return n;
  };
  Vec3 shift{};
  int bestHits = -1;
  for (const Area& a : m_areas) {
    for (size_t i = 0; i < std::min<size_t>(4, spotOrder.size()); ++i) {
      const Vec3& p = spots[spotOrder[i]];
      const Vec3 candidate{a.xf[0][3] - p[0], a.xf[1][3] - p[1], a.xf[2][3] - p[2]};
      const int n = hits(candidate);
      if (n > bestHits) {
        bestHits = n;
        shift = candidate;
      }
    }
  }
  {
    char line[160];
    std::snprintf(line, sizeof line, "world shift (%.2f, %.2f, %.2f), %d of %zu rooms land on an area", shift[0],
                  shift[1], shift[2], bestHits, spots.size());
    Log(line);
  }
  std::map<std::string, Placement> placed;
  for (const auto& [name, p] : spots) {
    placed[name].pos = {p[0] + shift[0], p[1] + shift[1], p[2] + shift[2]};
  }

  // A room without a Tonemap or BloomEffect of its own takes the world's.
  float tonemap[5] = {4.0f, 0.18f, 0.6f, 0.15f, 0.f};
  Tonemap(master, tonemap);
  BloomData bloom;
  ReadBloom(master, bloom);
  // The world's global grade sits under the room's own (the last active one is drawn).
  // Its hints that are not global follow its own camera volumes, which every room carries.
  std::vector<PortRoomEnv::WorldVolume> volumes;
  const SceneryScripts masterScripts = MasterScripts(master.room, shift, volumes);
  for (size_t i = 0; i < volumes.size(); ++i) {
    // The nearest retail door, to check the placement by.
    const PortRoomEnv::WorldVolume& v = volumes[i];
    double best = 1e300;
    uint32_t bestArea = 0;
    for (const Area& a : m_areas) {
      for (const Vec3& d : a.doors) {
        const double dist = Distance(d, {v.centre[0], v.centre[1], v.centre[2]});
        if (dist < best) {
          best = dist;
          bestArea = a.mrea;
        }
      }
    }
    char line[192];
    std::snprintf(line, sizeof line,
                  "world volume %zu: centre (%.2f, %.2f, %.2f) half (%.2f, %.2f, %.2f), nearest door %.2f m (area %08X)", i,
                  v.centre[0], v.centre[1], v.centre[2], v.half[0], v.half[1], v.half[2], best, bestArea);
    Log(line);
  }
  // The world's skies: the master's Skyboxes, one per model (!IceWorld's two are the same
  // sky, switched by its triggers), an active one's transform first.
  m_worldSkies.clear();
  for (const bool wantActive : {true, false}) {
    for (const Component* c : master.room.Of(kSkybox)) {
      const auto f = master.room.Flat(*c);
      const auto prop = f.find(kPropSkyboxModel);
      Vec3 pos, rot, scale;
      if (master.room.Active(*c) != wantActive || prop == f.end() || prop->second.size != 16 ||
          !master.room.Xform(*c, pos, rot, scale)) {
        continue;
      }
      const Id16 model = SwapUuid(master.room.Bytes(prop->second));
      if (model == Id16{} || std::any_of(m_worldSkies.begin(), m_worldSkies.end(),
                                         [&](const WorldSky& s) { return s.model == model; })) {
        continue;
      }
      WorldSky& sky = m_worldSkies.emplace_back();
      sky.model = model;
      sky.rot = rot;
      sky.scale = scale;
      SkyRadiance(master.room, *c, sky.radiance);
    }
  }
  std::vector<GradeData> grades;
  ReadGrades(master, nullptr, grades, &masterScripts);
  std::vector<BacklightData> backlights;
  ReadBacklights(master, nullptr, backlights, &masterScripts);
  std::vector<FogData> fogs;
  ReadFogs(master, nullptr, fogs, &masterScripts);
  Log("world: " + std::to_string(m_worldSkies.size()) + " sky model(s), " + std::to_string(grades.size()) + " grade(s), " + std::to_string(backlights.size()) +
      " backlight(s), " + std::to_string(fogs.size()) + " fog hint(s), " + std::to_string(volumes.size()) +
      " camera volume(s)");
  // Every room is matched before any is written: two rooms matching one area
  // would write the same files, so only the closer match (the first, on a tie)
  // is written and the other is logged as a clash.
  std::vector<Match> matches(rooms.size());
  std::vector<bool> placedByWorld(rooms.size());
  std::map<uint32_t, size_t> winner;
  for (size_t i = 0; i < rooms.size(); ++i) {
    placedByWorld[i] = MatchRoom(*rooms[i], placed, matches[i]);
    if (!placedByWorld[i] || matches[i].err > 1.0) {
      continue;
    }
    const auto [w, fresh] = winner.emplace(matches[i].mrea, i);
    if (!fresh && matches[i].err < matches[w->second].err) {
      w->second = i;
    }
  }
  std::map<std::string, std::string> seen;
  for (size_t i = 0; i < rooms.size(); ++i) {
    if (m_io.cancelled && m_io.cancelled()) {
      error = "cancelled";
      return false;
    }
    const RoomData& r = *rooms[i];
    const Match& m = matches[i];
    if (!placedByWorld[i]) {
      Log(r.name + ": not placed by the world");
      continue;
    }
    if (m.err > 1.0) {
      char line[96];
      std::snprintf(line, sizeof line, "%s: no area matches (best %08X, %.1f m)", r.name.c_str(), m.mrea, m.err);
      Log(line);
      continue;
    }
    const size_t kept = winner.at(m.mrea);
    if (kept != i) {
      char line[64];
      std::snprintf(line, sizeof line, " both match %08X (%.2f m against %.2f m)", m.mrea, m.err,
                    matches[kept].err);
      Log("  CLASH: " + r.name + " and " + rooms[kept]->name + line + "; kept " + rooms[kept]->name);
      continue;
    }
    std::string matched;
    Log(WriteRoom(r, placed, shift, tonemap, bloom, grades, backlights, fogs, volumes, m, written, matched));
    if (!matched.empty()) {
      seen[matched] = r.name;
    }
  }
  Log(std::to_string(seen.size()) + " of " + std::to_string(m_areas.size()) + " areas");
  return true;
}

}  // namespace
}  // namespace RoomDetail

const std::vector<RoomWorld>& RoomWorlds() {
  static const std::vector<RoomWorld> worlds = {
      {"Intro_Master", 0x158EFE17}, {"RuinsWorld", 0x83F6FF6F},   {"IceWorld", 0xA8BE6291},
      {"Over_Master", 0x39F2DE28},  {"Mines_Master", 0xB1AC4D65}, {"Lava_Master", 0x3EF8237C},
      {"Crater_Master", 0xC13B09D1}};
  return worlds;
}

bool WriteWorldRoomEnvs(uint32_t mlvl, const RoomPak& master, const std::vector<RoomPak>& rooms,
                        const std::vector<RoomPak>& others, const RoomIO& io, int& written, std::string& error) {
  RoomDetail::Writer writer(master, rooms, others, io);
  return writer.Run(mlvl, written, error);
}

}  // namespace PortRemastered
