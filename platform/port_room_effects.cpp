#include "port_room_effects.h"

#include "port_env.h"
#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"

#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/ScriptObjects/CScriptEffect.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpecialFunction.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <algorithm>
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
std::map<uint32_t, std::vector<Spawned>> sSpawned;  // by MREA
std::map<uint32_t, size_t> sSkipped;                // by MREA
size_t sPatched = 0;                                // property patches applied, this area

// The drives of each loaded area, with the messages their timers still hold back.
struct Pending {
  float due;
  size_t drive;
};
struct Driven {
  std::vector<Drive> drives;
  std::vector<Pending> pending;
  float clock = 0.f;
};
std::map<uint32_t, Driven> sDriven;  // by MREA

void Deliver(CStateManager& mgr, TUniqueId sender, const Drive& d) {
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
  if (!sFile.drives.empty()) {
    sDriven[mrea].drives = sFile.drives;
  }
}

void EndArea() {
  if (sLoading && !sFile.patches.empty()) {
    PortLog::Write("room effects: %08X: %zu of %zu particle property patch(es) applied\n", sMrea, sPatched,
                   sFile.patches.size());
  }
  sLoading = false;
  sFile = {};
  sSkips.clear();
}

bool Skips(uint32_t eid) {
  if (!sLoading || sSkips.count(eid & 0x3FFFFFF) == 0) {
    return false;
  }
  ++sSkipped[sMrea];
  return true;
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
  sPatched += n;
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
  if (!sLoading) {
    return;
  }
  for (const Layer& l : sFile.layers) {
    if (l.layer != layer || l.objects.empty()) {
      continue;
    }
    // Parse() checked the framing; each object is read as the area's own are.
    CMemoryInStream in(l.objects.data(), int(l.objects.size()));
    size_t at = 0;
    for (uint32_t i = 0; i < l.count; ++i) {
      Effect info;
      size_t used = 0;
      ReadEffect(l.objects.data() + at, l.objects.size() - at, info, used);
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
    Deliver(mgr, uid, d);
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
      Deliver(mgr, kInvalidUniqueId, area.drives[drive]);
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

std::vector<Spawned> List(uint32_t mrea) {
  const auto found = sSpawned.find(mrea);
  return found != sSpawned.end() ? found->second : std::vector<Spawned>();
}

size_t SkippedCount(uint32_t mrea) {
  const auto found = sSkipped.find(mrea);
  return found != sSkipped.end() ? found->second : 0;
}

}  // namespace PortRoomEffects
