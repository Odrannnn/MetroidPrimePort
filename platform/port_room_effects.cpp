#include "port_room_effects.h"

#include "port_env.h"
#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"

#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Enemies/CWallCrawlerSwarm.hpp"
#include "MetroidPrime/ScriptObjects/CScriptEffect.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpecialFunction.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace PortRoomEffects {
namespace {

// The area being loaded: its file, and the retail ids it leaves out (raw values, layer
// bits dropped as TEditorId compares them).
bool sLoading = false;
uint32_t sMrea = 0;
File sFile;
std::set<uint32_t> sSkips;
std::map<uint32_t, File> sKept;  // by MREA: each loaded area's file, for its generated copies
bool sGenerating = false;
std::map<uint32_t, std::vector<Spawned>> sSpawned;  // by MREA
std::map<uint32_t, size_t> sSkipped;                // by MREA
size_t sPatched = 0;                                // property patches applied, this area

// The drives of each loaded area, with the messages their timers still hold back.
struct Pending {
  float due;
  size_t drive;
};
// A generator's template as LoadLayer read it: its SCLY object, to load again for each copy.
struct Template {
  int area = 0;  // TAreaId
  std::vector<uint8_t> object;
  std::vector<Variable> vars;
};
struct Driven {
  std::vector<Drive> drives;
  std::vector<Pending> pending;
  float clock = 0.f;
  std::vector<Generator> generators;
  std::vector<bool> active;               // by generator
  std::vector<size_t> made;               // by generator: copies made
  std::map<uint32_t, Template> templates;  // by add-on editor id
  std::vector<Ambient> ambients;
  std::vector<float> carry;     // by ambient: the fraction of a point carried over
  std::vector<size_t> spawned;  // by ambient: points inside its shapes so far
};
std::map<uint32_t, Driven> sDriven;  // by MREA
CRandom16 sAmbientRandom;

CScriptEffect* EffectOf(CStateManager& mgr, uint32_t id) {
  return TCastToPtr< CScriptEffect >(mgr.ObjectById(mgr.GetIdForScript(TEditorId(id))));
}

bool InShape(const AmbientShape& s, const CVector3f& p) {
  const float d[3] = {p.GetX() - s.center[0], p.GetY() - s.center[1], p.GetZ() - s.center[2]};
  const auto along = [&](int axis) {
    return s.axes[axis][0] * d[0] + s.axes[axis][1] * d[1] + s.axes[axis][2] * d[2];
  };
  switch (s.type) {
  case kAmbientBox:
    return std::fabs(along(0)) <= s.half[0] && std::fabs(along(1)) <= s.half[1] && std::fabs(along(2)) <= s.half[2];
  case kAmbientCylinder: {
    const float t = along(0);
    return std::fabs(t) <= s.half[0] && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - t * t <= s.half[1] * s.half[1];
  }
  default:
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= s.half[0] * s.half[0];
  }
}

// SpawnParticlesAtPositions 0x4bb0a0: the points, in each effect linked with `index`.
void SpawnAmbient(CStateManager& mgr, const Ambient& a, uint32_t index, const CVector3f* points, int n) {
  if (n <= 0) {
    return;
  }
  std::vector<CVector3f> local(size_t(n), CVector3f(0.f, 0.f, 0.f));
  for (const AmbientEmitter& e : a.emitters) {
    CScriptEffect* const effect = e.index == index ? EffectOf(mgr, e.id) : nullptr;
    CElementGen* const gen = effect != nullptr && effect->GetActive() ? effect->PortParticleSystem() : nullptr;
    if (gen == nullptr) {
      continue;
    }
    const CVector3f& t = gen->GetGlobalTranslation();
    const CVector3f& s = gen->GetGlobalScale();
    for (int k = 0; k < n; ++k) {
      local[size_t(k)] = CVector3f((points[k].GetX() - t.GetX()) / s.GetX(), (points[k].GetY() - t.GetY()) / s.GetY(),
                                   (points[k].GetZ() - t.GetZ()) / s.GetZ());
    }
    gen->PortCreateParticlesAtPositionsIncludingChildren(n, local.data());
  }
}

// CAmbientParticleEffectGOC::BeforeThink 0x4ba150 (see Ambient).
void ThinkAmbient(CStateManager& mgr, const Ambient& a, float& carry, size_t& spawned, float dt) {
  const CScriptEffect* const owner = EffectOf(mgr, a.owner);
  if (owner == nullptr || !owner->GetActive()) {
    return;
  }
  // The game scales the rate, and removes particles older than its life, by two tweak splines of
  // SGPUFrameTimingData::CalculateGPUAvailabilityScale 0xb2a94c; that scale is 1 unless the GPU
  // runs late, and both splines are 1 there (read live), so neither applies.
  const float want = a.rate * dt + carry;
  const int n = int(want);
  carry = want - float(n);
  if (n <= 0) {
    return;
  }
  const CTransform4f& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr).GetTransform();
  const CVector3f center = cam.GetTranslation() + cam.Rotate(CVector3f(a.offset[0], a.offset[1], a.offset[2]));
  const float r = a.radius;
  std::vector<CVector3f> drawn;
  drawn.reserve(size_t(n));
  for (int i = 0; i < n; ++i) {
    CVector3f p(0.f, 0.f, 0.f);
    do {
      const float x = sAmbientRandom.Float(), y = sAmbientRandom.Float(), z = sAmbientRandom.Float();
      p = CVector3f(2.f * r * x - r, 2.f * r * y - r, 2.f * r * z - r);
    } while (p.MagSquared() > r * r);
    drawn.push_back(center + p);
  }
  // ErasePointsInsideShapeList 0x897be0: shape by shape, each point inside moved out, the last
  // point taking its place.
  std::vector<CVector3f> points;
  for (const AmbientShape& s : a.shapes) {
    for (size_t i = 0; i < drawn.size();) {
      if (!InShape(s, drawn[i])) {
        ++i;
        continue;
      }
      points.push_back(drawn[i]);
      drawn[i] = drawn.back();
      drawn.pop_back();
    }
  }
  const int kept = int(points.size());
  spawned += size_t(kept);
  const int count = int(a.weights.size());
  int sum = 0, minPositive = 0;
  for (int32_t w : a.weights) {
    if (w > 0) {
      sum += w;
      minPositive = minPositive == 0 ? w : std::min(minPositive, int(w));
    }
  }
  int done = 0;
  if (count >= 1 && sum > 0 && minPositive * kept / sum >= 1) {
    for (int i = 0; i < count; ++i) {
      if (a.weights[size_t(i)] >= 1) {
        const int k = std::min(a.weights[size_t(i)] * kept / sum, kept - done);
        SpawnAmbient(mgr, a, uint32_t(i), points.data() + done, k);
        done += k;
      }
    }
  }
  for (; count >= 1 && done < kept; ++done) {
    SpawnAmbient(mgr, a, uint32_t(sAmbientRandom.Range(0, count - 1)), points.data() + done, 1);
  }
}

// CScriptGeneratorMP1::AcceptScriptMsg 0xf44128 for one of the file's generators (see Generator).
void Generate(CStateManager& mgr, Driven& area, size_t index, TUniqueId sender, const Drive& d) {
  const Generator& g = area.generators[index];
  if (d.message != kSM_SetToZero) {
    area.active[index] = d.message == kSM_Activate ? true : d.message == kSM_Deactivate ? false : !area.active[index];
    return;
  }
  if (!area.active[index]) {
    return;
  }
  // A relay or timer in between is no actor: the copies keep their template's placement.
  const CEntity* const sent = (d.flags & kDriveRelayed) != 0 ? nullptr : mgr.GetObjectById(sender);
  const CActor* const follow = TCastToConstPtr< CActor >(sent);
  const CWallCrawlerSwarm* const swarm = TCastToConstPtr< CWallCrawlerSwarm >(sent);
  for (uint32_t id : g.templates) {
    // Only a template that still lives (BuildGenerationList 0xf44c68).
    const auto t = area.templates.find(id);
    if (t == area.templates.end() || mgr.GetObjectById(mgr.GetIdForScript(TEditorId(id))) == nullptr) {
      continue;
    }
    CMemoryInStream in(t->second.object.data(), int(t->second.object.size()));
    const char type = in.ReadChar();
    const uint length = in.ReadLong();
    const rstl::pair<TEditorId, TUniqueId> made = mgr.LoadScriptObject(
        TAreaId(t->second.area), static_cast<EScriptObjectType>(static_cast<uchar>(type)), length, in);
    CEntity* const copy = mgr.ObjectById(made.second);
    if (copy == nullptr) {
      continue;
    }
    ++area.made[index];
    if (CScriptEffect* const effect = TCastToPtr< CScriptEffect >(copy)) {
      if (!t->second.vars.empty()) {
        effect->PortSetExternalVars(ExternalVars(t->second.vars));
      }
      if (follow != nullptr) {
        if ((g.flags & kGeneratorNoInheritXf) == 0) {
          effect->SetTransform(follow->GetTransform());
        }
        effect->SetTranslation((swarm != nullptr ? swarm->GetLastKilledOffset() : follow->GetTranslation()) +
                               CVector3f(g.offset[0], g.offset[1], g.offset[2]));
      }
    }
    mgr.DeliverScriptMsg(copy, kInvalidUniqueId, kSM_Activate);
  }
}

void Deliver(CStateManager& mgr, Driven& area, TUniqueId sender, const Drive& d) {
  if ((d.flags & kDriveToGenerator) != 0) {
    Generate(mgr, area, d.target, sender, d);
    return;
  }
  mgr.SendScriptMsg(sender, TEditorId(d.target), static_cast<EScriptObjectMessage>(d.message),
                    static_cast<EScriptObjectState>(d.state));
}

}  // namespace

bool Enabled() { return port::EnvFlag("MP_REMASTERED_EFFECT_ACTORS", true); }

void BeginArea(uint32_t mrea) {
  sLoading = true;
  sMrea = mrea;
  sPatched = 0;
  sFile = {};
  sSkips.clear();
  sKept.erase(mrea);
  sSpawned.erase(mrea);
  sSkipped.erase(mrea);
  sDriven.erase(mrea);
  if (!Enabled()) {
    return;
  }
  const std::string path = PortMods::RoomEffectsPath(mrea);
  if (path.empty()) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::string error;
  if (data.empty() || !Parse(data, sFile, error)) {
    PortLog::Write("room effects: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    sFile = {};
    return;
  }
  for (uint32_t id : sFile.skips) {
    sSkips.insert(id & 0x3FFFFFF);
  }
  if (!sFile.drives.empty() || !sFile.generators.empty() || !sFile.ambients.empty()) {
    Driven& driven = sDriven[mrea];
    driven.ambients = sFile.ambients;
    driven.carry.assign(sFile.ambients.size(), 0.f);
    driven.spawned.assign(sFile.ambients.size(), 0);
    driven.drives = sFile.drives;
    driven.generators = sFile.generators;
    driven.made.assign(sFile.generators.size(), 0);
    for (const Generator& g : sFile.generators) {
      driven.active.push_back((g.flags & kGeneratorActive) != 0);
    }
  }
}

void EndArea() {
  if (sLoading && !sFile.patches.empty()) {
    PortLog::Write("room effects: %08X: %zu of %zu particle property patch(es) applied\n", sMrea, sPatched,
                   sFile.patches.size());
  }
  if (sLoading) {
    sKept[sMrea] = std::move(sFile);
  }
  sLoading = false;
  sFile = {};
  sSkips.clear();
}

bool BeginGenerate(uint32_t mrea) {
  const auto kept = sKept.find(mrea);
  if (sLoading || kept == sKept.end()) {
    return false;
  }
  sLoading = true;
  sGenerating = true;
  sMrea = mrea;
  std::swap(sFile, kept->second);
  return true;
}

void EndGenerate() {
  if (!sGenerating) {
    return;
  }
  std::swap(sFile, sKept[sMrea]);
  sFile = {};
  sLoading = false;
  sGenerating = false;
}

bool Skips(uint32_t eid) {
  if (!sLoading || sGenerating || sSkips.count(eid & 0x3FFFFFF) == 0) {
    return false;
  }
  ++sSkipped[sMrea];
  return true;
}

bool Hidden(uint32_t eid) {
  return sLoading && std::any_of(sFile.hidden.begin(), sFile.hidden.end(),
                                 [&](uint32_t id) { return (id & 0x3FFFFFF) == (eid & 0x3FFFFFF); });
}

const Override* FindOverride(uint32_t eid) {
  if (!sLoading) {
    return nullptr;
  }
  for (const Override& over : sFile.overrides) {
    if ((over.id & 0x3FFFFFF) == (eid & 0x3FFFFFF)) {
      return &over;
    }
  }
  return nullptr;
}

const PatternedEffect* FindPatternedEffect(uint32_t eid, uint32_t slot) {
  if (!sLoading) {
    return nullptr;
  }
  for (const PatternedEffect& p : sFile.patterned) {
    if (p.slot == slot && (p.id & 0x3FFFFFF) == (eid & 0x3FFFFFF)) {
      return &p;
    }
  }
  return nullptr;
}

const Steam* FindSteam(uint32_t eid) {
  if (!sLoading) {
    return nullptr;
  }
  for (const Steam& s : sFile.steams) {
    if ((s.id & 0x3FFFFFF) == (eid & 0x3FFFFFF)) {
      return &s;
    }
  }
  return nullptr;
}

const FishCloud* FindFishCloud(uint32_t eid) {
  if (!sLoading) {
    return nullptr;
  }
  for (const FishCloud& f : sFile.fishClouds) {
    if ((f.id & 0x3FFFFFF) == (eid & 0x3FFFFFF)) {
      return &f;
    }
  }
  return nullptr;
}

const ActorSlotEffect* FindActorSlotEffect(uint32_t eid, uint32_t slot) {
  if (!sLoading) {
    return nullptr;
  }
  for (const ActorSlotEffect& a : sFile.actorSlots) {
    if (a.slot == slot && (a.id & 0x3FFFFFF) == (eid & 0x3FFFFFF)) {
      return &a;
    }
  }
  return nullptr;
}

bool HasPatches(uint32_t eid) {
  return sLoading && std::any_of(sFile.patches.begin(), sFile.patches.end(), [eid](const PropertyPatch& p) {
           return (p.id & 0x3FFFFFF) == (eid & 0x3FFFFFF);
         });
}

size_t ApplyPatches(uint32_t eid, uint8_t* props, size_t size, bool (*canBuild)(uint32_t part)) {
  const size_t n = sLoading ? ApplyPatches(sFile.patches, eid, props, size, canBuild) : 0;
  if (!sGenerating) {
    sPatched += n;
  }
  return n;
}

std::vector< SPortExternalVar > ExternalVars(const std::vector< Variable >& vars) {
  std::vector< SPortExternalVar > out(vars.size());
  for (size_t k = 0; k < vars.size(); ++k) {
    const Variable& v = vars[k];
    std::copy(v.guid.begin(), v.guid.end(), out[k].guid.bytes.begin());
    std::copy(std::begin(v.color), std::end(v.color), out[k].color);
    std::copy(std::begin(v.vec), std::end(v.vec), out[k].vec);
    out[k].real = v.real;
    out[k].integer = v.integer;
  }
  return out;
}

void LoadLayer(CStateManager& mgr, int area, uint32_t layer, std::vector<uint32_t>& ids) {
  if (!sLoading || sGenerating) {
    return;
  }
  for (const Layer& l : sFile.layers) {
    if (l.layer != layer || l.objects.empty()) {
      continue;
    }
    // Parse() checked the framing; each object is read as the area's own are.
    CMemoryInStream in(l.objects.data(), int(l.objects.size()));
    size_t at = 0;
    const auto driven = sDriven.find(sMrea);
    for (uint32_t i = 0; i < l.count; ++i) {
      Effect info;
      size_t used = 0;
      ReadEffect(l.objects.data() + at, l.objects.size() - at, info, used);
      if (driven != sDriven.end() &&
          std::any_of(driven->second.generators.begin(), driven->second.generators.end(), [&](const Generator& g) {
            return std::find(g.templates.begin(), g.templates.end(), info.id) != g.templates.end();
          })) {
        Template& t = driven->second.templates[info.id];
        t.area = area;
        t.object.assign(l.objects.begin() + std::ptrdiff_t(at), l.objects.begin() + std::ptrdiff_t(at + used));
        t.vars = i < l.vars.size() ? l.vars[i] : std::vector<Variable>();
      }
      at += used;
      const char type = in.ReadChar();
      const uint length = in.ReadLong();
      const rstl::pair<TEditorId, TUniqueId> made =
          mgr.LoadScriptObject(TAreaId(area), static_cast<EScriptObjectType>(static_cast<uchar>(type)), length, in);
      if (made.first == kInvalidEditorId) {
        continue;  // e.g. its PART is not in the mods
      }
      ids.push_back(made.first.value);
      if (i < l.vars.size() && !l.vars[i].empty()) {
        if (CScriptEffect* const effect = TCastToPtr< CScriptEffect >(mgr.ObjectById(made.second))) {
          effect->PortSetExternalVars(ExternalVars(l.vars[i]));
        }
      }
      sSpawned[sMrea].push_back({sMrea, made.first.value, made.second.value, info.part, info.name,
                                 i < l.vars.size() ? l.vars[i].size() : 0});
    }
  }
  // The Remastered timers, as the SCLY Timer object ScriptLoader::LoadTimer reads: no
  // connections (drives deliver them), name, start time, max random delay, loop, autostart, active.
  for (const Timer& t : sFile.timers) {
    if ((t.id >> 26) != layer) continue;
    std::vector<uint8_t> object;
    auto be32 = [&](uint32_t v) {
      for (int s = 24; s >= 0; s -= 8) object.push_back(uint8_t(v >> s));
    };
    auto beFloat = [&](float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      be32(v);
    };
    be32(t.id);
    be32(0);
    be32(6);
    for (char c : std::string("timer")) object.push_back(uint8_t(c));
    object.push_back(0);
    beFloat(t.startTime);
    beFloat(t.maxRandDelay);
    object.push_back((t.flags & kTimerLoop) != 0);
    object.push_back((t.flags & kTimerAutoStart) != 0);
    object.push_back((t.flags & kTimerActive) != 0);
    CMemoryInStream in(object.data(), int(object.size()));
    const rstl::pair<TEditorId, TUniqueId> made = mgr.LoadScriptObject(
        TAreaId(area), static_cast<EScriptObjectType>(kTimerType), uint(object.size()), in);
    if (made.first != kInvalidEditorId) {
      ids.push_back(made.first.value);
      sSpawned[sMrea].push_back({sMrea, made.first.value, made.second.value, 0xFFFFFFFF, "Remastered timer", 0});
    }
  }
  // The Remastered triggers, as the SCLY Trigger object ScriptLoader::LoadTrigger reads: no
  // connections, name, position, extent, no damage (CDamageInfo: 4 properties, weapon type 0,
  // damage, radius, knockback), no force, flags, active, deactivate on entered and on exited.
  for (const Trigger& t : sFile.triggers) {
    if ((t.id >> 26) != layer) continue;
    std::vector<uint8_t> object;
    auto be32 = [&](uint32_t v) {
      for (int s = 24; s >= 0; s -= 8) object.push_back(uint8_t(v >> s));
    };
    auto beFloat = [&](float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      be32(v);
    };
    be32(t.id);
    be32(0);
    be32(9);
    for (char c : std::string("trigger")) object.push_back(uint8_t(c));
    object.push_back(0);
    for (float v : t.pos) beFloat(v);
    for (float v : t.extent) beFloat(v);
    be32(4);
    be32(0);
    for (int i = 0; i < 3; ++i) beFloat(0.f);
    for (int i = 0; i < 3; ++i) beFloat(0.f);
    be32(t.triggerFlags);
    object.push_back((t.flags & kTriggerActive) != 0);
    object.push_back((t.flags & kTriggerDeactivateOnEntered) != 0);
    object.push_back((t.flags & kTriggerDeactivateOnExited) != 0);
    CMemoryInStream in(object.data(), int(object.size()));
    const rstl::pair<TEditorId, TUniqueId> made = mgr.LoadScriptObject(
        TAreaId(area), static_cast<EScriptObjectType>(kTriggerType), uint(object.size()), in);
    if (made.first != kInvalidEditorId) {
      ids.push_back(made.first.value);
      sSpawned[sMrea].push_back({sMrea, made.first.value, made.second.value, 0xFFFFFFFF, "Remastered trigger", 0});
    }
  }
  // The Remastered relays, as the SCLY Relay object ScriptLoader::LoadFaultyRelay reads: no
  // connections, name, active.
  for (const Relay& r : sFile.relays) {
    if ((r.id >> 26) != layer) continue;
    std::vector<uint8_t> object;
    auto be32 = [&](uint32_t v) {
      for (int s = 24; s >= 0; s -= 8) object.push_back(uint8_t(v >> s));
    };
    be32(r.id);
    be32(0);
    be32(2);
    for (char c : std::string("relay")) object.push_back(uint8_t(c));
    object.push_back(0);
    object.push_back((r.flags & kRelayActive) != 0);
    CMemoryInStream in(object.data(), int(object.size()));
    const rstl::pair<TEditorId, TUniqueId> made = mgr.LoadScriptObject(
        TAreaId(area), static_cast<EScriptObjectType>(kRelayType), uint(object.size()), in);
    if (made.first != kInvalidEditorId) {
      ids.push_back(made.first.value);
      sSpawned[sMrea].push_back({sMrea, made.first.value, made.second.value, 0xFFFFFFFF, "Remastered relay", 0});
    }
  }
  // The follows, as the SCLY SpecialFunction ScriptLoader::LoadSpecialFunction reads: Play ->
  // Activate to the followed actor and Play -> Deactivate to the follower (never sent: the
  // function reads them), then its 15 properties, the unused ones as retail's FogVolume has them.
  for (const Follow& f : sFile.follows) {
    if ((f.id >> 26) != layer) continue;
    std::vector<uint8_t> object;
    auto be32 = [&](uint32_t v) {
      for (int s = 24; s >= 0; s -= 8) object.push_back(uint8_t(v >> s));
    };
    auto text = [&](const std::string& s) {
      object.insert(object.end(), s.begin(), s.end());
      object.push_back(0);
    };
    be32(f.id);
    be32(2);
    be32(kSS_Play);
    be32(kSM_Activate);
    be32(f.followed);
    be32(kSS_Play);
    be32(kSM_Deactivate);
    be32(f.follower);
    be32(15);
    text("follow locator");
    for (int i = 0; i < 6; ++i) be32(0);  // position, rotation: the function moves the follower only
    be32(kFollowLocatorFunction);
    text(f.locator);
    for (int i = 0; i < 3; ++i) be32(0);  // f1-f3
    be32(0xFFFFFFFF);                     // w2
    be32(0xFFFFFFFF);                     // w3
    be32(0xFFFFFFFF);                     // item: kIT_Invalid
    object.push_back((f.flags & kFollowActive) != 0);
    be32(0);  // f4
    for (int i = 0; i < 3; ++i) be32(0xFFFF);  // sfx: kInternalInvalidSfxId
    CMemoryInStream in(object.data(), int(object.size()));
    const rstl::pair<TEditorId, TUniqueId> made = mgr.LoadScriptObject(
        TAreaId(area), static_cast<EScriptObjectType>(kSpecialFunctionType), uint(object.size()), in);
    if (made.first != kInvalidEditorId) {
      ids.push_back(made.first.value);
      sSpawned[sMrea].push_back({sMrea, made.first.value, made.second.value, 0xFFFFFFFF,
                                 "Remastered follow " + f.locator, 0});
    }
  }
  for (const BarrierGroup& group : sFile.barriers) {
    if ((group.controller >> 26) != layer) continue;
    const TUniqueId uid = mgr.GetIdForScript(TEditorId(group.controller));
    if (CScriptSpecialFunction* controller = TCastToPtr< CScriptSpecialFunction >(mgr.ObjectById(uid))) {
      controller->PortConfigureBarrier(group, sMrea);
    }
    for (const BarrierRing& ring : group.rings) {
      for (uint32_t eid : {ring.refraction, ring.glow}) {
        if (CScriptEffect* e = TCastToPtr< CScriptEffect >(mgr.ObjectById(mgr.GetIdForScript(TEditorId(eid))))) {
          e->PortAttachBarrier(uid, mgr.GetIdForScript(TEditorId(ring.platform)));
        }
      }
    }
  }
}

void OnScriptState(CStateManager& mgr, uint16_t senderUid, uint32_t editorId, int state) {
  if (sDriven.empty()) {
    return;
  }
  // As PortRoomGeo::OnScriptState: the sender's area, made after it counts as loaded.
  const TAreaId areaId(int((editorId >> 16) & 0x3ff));
  const CWorld* const world = mgr.GetWorld();
  if (world == nullptr || areaId.Value() >= world->GetNumAreas() || !world->GetArea(areaId)->IsLoaded()) {
    return;
  }
  const auto found = sDriven.find(world->GetArea(areaId)->GetAreaAssetId());
  if (found == sDriven.end()) {
    return;
  }
  Driven& area = found->second;
  const uint32_t sender = editorId & 0x3ffffff;
  for (size_t i = 0; i < area.drives.size(); ++i) {
    const Drive& d = area.drives[i];
    if ((d.sender & 0x3ffffff) != sender || int(d.state) != state) {
      continue;
    }
    if (d.delay > 0.f) {
      // The ResetAndStart timers it stands for, sent again before they are up: started over.
      std::erase_if(area.pending, [&](const Pending& p) { return p.drive == i; });
      area.pending.push_back({area.clock + d.delay, i});
      continue;
    }
    TUniqueId uid;
    uid.value = senderUid;
    Deliver(mgr, area, uid, d);
  }
}

void Think(CStateManager& mgr, float dt) {
  if (sDriven.empty()) {
    return;
  }
  const CWorld* const world = mgr.GetWorld();
  std::set<uint32_t> loaded;
  for (int i = 0; world != nullptr && i < world->GetNumAreas(); ++i) {
    const CGameArea* const area = world->GetArea(TAreaId(i));
    if (area->IsLoaded()) {
      loaded.insert(area->GetAreaAssetId());
    }
  }
  for (auto it = sDriven.begin(); it != sDriven.end();) {
    if (loaded.count(it->first) == 0) {
      it = sDriven.erase(it);
      continue;
    }
    Driven& area = it->second;
    area.clock += dt;
    // In the order they fall due.
    while (!area.pending.empty()) {
      const auto next = std::min_element(area.pending.begin(), area.pending.end(),
                                         [](const Pending& a, const Pending& b) { return a.due < b.due; });
      if (next->due > area.clock) {
        break;
      }
      const size_t drive = next->drive;
      area.pending.erase(next);
      Deliver(mgr, area, kInvalidUniqueId, area.drives[drive]);
    }
    for (size_t i = 0; i < area.ambients.size(); ++i) {
      ThinkAmbient(mgr, area.ambients[i], area.carry[i], area.spawned[i], dt);
    }
    ++it;
  }
}

std::vector<std::pair<Drive, size_t>> DrivesOf(uint32_t mrea) {
  std::vector<std::pair<Drive, size_t>> out;
  const auto found = sDriven.find(mrea);
  if (found == sDriven.end()) {
    return out;
  }
  for (size_t i = 0; i < found->second.drives.size(); ++i) {
    const size_t pending = size_t(std::count_if(found->second.pending.begin(), found->second.pending.end(),
                                                [i](const Pending& p) { return p.drive == i; }));
    out.emplace_back(found->second.drives[i], pending);
  }
  return out;
}

std::vector<GeneratorState> GeneratorsOf(uint32_t mrea) {
  std::vector<GeneratorState> out;
  const auto found = sDriven.find(mrea);
  for (size_t i = 0; found != sDriven.end() && i < found->second.generators.size(); ++i) {
    out.push_back({found->second.generators[i], found->second.active[i], found->second.made[i]});
  }
  return out;
}

std::vector<AmbientState> AmbientsOf(uint32_t mrea) {
  std::vector<AmbientState> out;
  const auto found = sDriven.find(mrea);
  for (size_t i = 0; found != sDriven.end() && i < found->second.ambients.size(); ++i) {
    out.push_back({found->second.ambients[i], found->second.spawned[i]});
  }
  return out;
}

std::vector<Spawned> List(uint32_t mrea) {
  const auto found = sSpawned.find(mrea);
  return found != sSpawned.end() ? found->second : std::vector<Spawned>();
}

size_t SkippedCount(uint32_t mrea) {
  const auto found = sSkipped.find(mrea);
  return found != sSkipped.end() ? found->second : 0;
}

}  // namespace PortRoomEffects
