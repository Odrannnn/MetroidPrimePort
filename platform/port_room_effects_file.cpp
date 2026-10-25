#include "port_room_effects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace PortRoomEffects {
namespace {

// The light parameters of 2952 of retail's Effect objects, the most common by far: 14
// properties, cast shadow, shadow scale 1, tessellation 0, shadow alpha 1, max shadow height
// 20, ambient white, no lights of its own, world lighting 1, light recalculation 1, no
// position bias, 4 dynamic and 4 area lights at most, no ambient channel overflow, layer 0.
constexpr uint8_t kLight[] = {
    0x00, 0x00, 0x00, 0x0e, 0x01, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00,
    0x41, 0xa0, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x3f,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}
void PutBEFloat(std::vector<uint8_t>& out, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutBE32(out, v);
}
void PutLE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 24));
}

struct BEReader {
  const uint8_t* p;
  size_t size;
  size_t at = 0;
  bool ok = true;
  uint32_t U32() {
    if (size - at < 4 || at > size) {
      ok = false;
      return 0;
    }
    const uint32_t v = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3];
    at += 4;
    return v;
  }
  float F32() {
    const uint32_t v = U32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  bool Bool() {
    if (at >= size) {
      ok = false;
      return false;
    }
    return p[at++] != 0;
  }
};

void PutLEFloat(std::vector<uint8_t>& out, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutLE32(out, v);
}

bool ReadLE32(const std::vector<uint8_t>& d, size_t& at, uint32_t& v) {
  if (at > d.size() || d.size() - at < 4) {
    return false;
  }
  v = uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
  at += 4;
  return true;
}

// Reads `n` Variable records at `at` (the caller checked that they are all there).
void ReadVariables(const std::vector<uint8_t>& d, size_t& at, uint32_t n, std::vector<Variable>& out) {
  out.resize(n);
  for (Variable& v : out) {
    std::memcpy(v.guid.data(), d.data() + at, 16);
    at += 16;
    uint32_t bits = 0;
    for (float* f : {&v.color[0], &v.color[1], &v.color[2], &v.color[3], &v.vec[0], &v.vec[1], &v.vec[2], &v.real}) {
      ReadLE32(d, at, bits);
      std::memcpy(f, &bits, 4);
    }
    ReadLE32(d, at, bits);
    v.integer = int32_t(bits);
  }
}

// A parsed layer object as a barrier's checks see it.
struct LayerObject {
  uint32_t id = 0;
  uint32_t part = 0xFFFFFFFF;
  bool active = true;
};

// An editor id without its layer bits (the room's and the object's are the same for every id
// a room's script holds, so two ids that differ only there name one object).
uint32_t NormalizedId(uint32_t eid) { return eid & 0x03FFFFFF; }
uint32_t LayerBits(uint32_t eid) { return eid >> 26; }
uint32_t AreaBits(uint32_t eid) { return (eid >> 16) & 0x3FF; }

// The barrier checks, all of them about the file alone: which room it is, whether the retail
// objects it names exist and whether their PARTs are installed is the area's business, not the
// codec's. Called with `objects` the layer objects in file order.
bool ValidateBarriers(const File& file, const std::vector<LayerObject>& objects, std::string& error) {
  if (file.barriers.size() > kMaxBarrierGroups) {
    error = "too many barrier groups";
    return false;
  }
  if (file.barriers.empty()) {
    return true;
  }
  // A barrier names its effects by their whole editor id, so no two layer objects may share
  // one (their layer bits included, the ids are already required to be add-ons).
  for (size_t i = 0; i < objects.size(); ++i) {
    for (size_t j = i + 1; j < objects.size(); ++j) {
      if (NormalizedId(objects[i].id) == NormalizedId(objects[j].id)) {
        error = "two layer objects share an editor id";
        return false;
      }
    }
  }
  const BarrierGroup& group = file.barriers[0];
  if (!std::isfinite(group.duration) || !(group.duration > 0.f) || group.duration > kMaxBarrierDuration) {
    error = "barrier duration outside (0, 60]";
    return false;
  }
  // The controller, then each ring's platform, then that ring's refraction and glow effects.
  constexpr uint32_t kRetail = 1 + kBarrierRings;       // the controller and the five platforms
  constexpr uint32_t kMembers = 1 + kBarrierRings * 3;  // and the ten effects the rings show
  uint32_t members[kMembers];
  members[0] = group.controller;
  for (uint32_t r = 0; r < kBarrierRings; ++r) {
    members[1 + r] = group.rings[r].platform;
    members[kRetail + 2 * r] = group.rings[r].refraction;
    members[kRetail + 2 * r + 1] = group.rings[r].glow;
  }
  // The controller and the platforms are the room's own retail objects, each a different one.
  for (uint32_t i = 0; i < kRetail; ++i) {
    if (IsAddOnId(members[i])) {
      error = "barrier controller or platform is an add-on";
      return false;
    }
    if (members[i] == 0) {
      error = "barrier controller or platform is unset";
      return false;
    }
    for (uint32_t j = i + 1; j < kRetail; ++j) {
      if (NormalizedId(members[i]) == NormalizedId(members[j])) {
        error = "barrier controller and platforms are not distinct";
        return false;
      }
    }
  }
  // All of them are in the controller's room and on its layer, and none is an object the file
  // leaves out or rewrites.
  for (uint32_t i = 0; i < kMembers; ++i) {
    if (LayerBits(members[i]) != LayerBits(group.controller) ||
        AreaBits(members[i]) != AreaBits(group.controller)) {
      error = "barrier member is on another layer or area than its controller";
      return false;
    }
    for (uint32_t id : file.skips) {
      if (NormalizedId(id) == NormalizedId(members[i])) {
        error = "barrier member is one the file skips";
        return false;
      }
    }
    for (const Override& over : file.overrides) {
      if (NormalizedId(over.id) == NormalizedId(members[i])) {
        error = "barrier member is one the file overrides";
        return false;
      }
    }
  }
  // Each ring shows two of the file's own effects: exactly one layer object, one no other ring
  // shows, added inactive and with a PART to show.
  for (uint32_t i = kRetail; i < kMembers; ++i) {
    for (uint32_t j = kRetail; j < i; ++j) {
      if (members[i] == members[j]) {
        error = "two barrier rings show the same effect";
        return false;
      }
    }
    const LayerObject* found = nullptr;
    uint32_t matches = 0;
    for (const LayerObject& object : objects) {
      if (object.id == members[i]) {
        found = &object;
        ++matches;
      }
    }
    if (matches != 1) {
      error = "a barrier effect is not one of the file's objects";
      return false;
    }
    if (found->active) {
      error = "a barrier effect starts active";
      return false;
    }
    if (found->part == 0 || found->part == 0xFFFFFFFF) {
      error = "a barrier effect has no PART";
      return false;
    }
  }
  // Fifteen guids: every one set, and no two the same.
  constexpr uint32_t kGuids = kBarrierRings * 3;
  const std::array<uint8_t, 16>* guids[kGuids];
  for (uint32_t r = 0; r < kBarrierRings; ++r) {
    guids[3 * r] = &group.rings[r].breakupRefraction;
    guids[3 * r + 1] = &group.rings[r].motion;
    guids[3 * r + 2] = &group.rings[r].breakupGlow;
  }
  for (uint32_t i = 0; i < kGuids; ++i) {
    bool set = false;
    for (uint8_t byte : *guids[i]) {
      set = set || byte != 0;
    }
    if (!set) {
      error = "a barrier guid is zero";
      return false;
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (*guids[i] == *guids[j]) {
        error = "two barrier guids are the same";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

void AppendEffect(const Effect& e, std::vector<uint8_t>& out) {
  std::vector<uint8_t> body;
  PutBE32(body, e.id);
  PutBE32(body, 0);  // connections
  PutBE32(body, kEffectProps);
  body.insert(body.end(), e.name.begin(), e.name.end());
  body.push_back(0);
  for (const float* v : {e.pos, e.rot, e.scale}) {
    for (int i = 0; i < 3; ++i) {
      PutBEFloat(body, v[i]);
    }
  }
  PutBE32(body, e.part);
  PutBE32(body, 0xFFFFFFFF);  // ELSC
  body.push_back(e.hotInThermal);
  body.push_back(e.noTimerUnlessAreaOccluded);
  body.push_back(e.rebuildSystemsOnActivate);
  body.push_back(e.active);
  body.push_back(e.useRateInverseCamDist);
  PutBEFloat(body, e.rateInverseCamDist);
  PutBEFloat(body, e.rateInverseCamDistRate);
  PutBEFloat(body, e.duration);
  PutBEFloat(body, e.durationResetWhileVisible);
  body.push_back(e.useRateCamDistRange);
  PutBEFloat(body, e.rateCamDistRangeMin);
  PutBEFloat(body, e.rateCamDistRangeMax);
  PutBEFloat(body, e.rateCamDistRangeFarRate);
  body.push_back(e.combatVisorVisible);
  body.push_back(e.thermalVisorVisible);
  body.push_back(e.xrayVisorVisible);
  body.push_back(e.dieWhenSystemsDone);
  body.insert(body.end(), std::begin(kLight), std::end(kLight));
  out.push_back(kEffectType);
  PutBE32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

bool ReadEffect(const uint8_t* data, size_t size, Effect& e, size_t& used) {
  if (size < 5 || data[0] != kEffectType) {
    return false;
  }
  BEReader r{data + 1, size - 1};
  const uint32_t length = r.U32();
  if (!r.ok || length > size - 5) {
    return false;
  }
  r.size = 4 + size_t(length);
  e.id = r.U32();
  const uint32_t conns = r.U32();
  const uint32_t props = r.U32();
  if (!r.ok || conns != 0 || props != kEffectProps) {
    return false;
  }
  const void* end = std::memchr(r.p + r.at, 0, r.size - r.at);
  if (end == nullptr) {
    return false;
  }
  const size_t nameSize = size_t(static_cast<const uint8_t*>(end) - (r.p + r.at));
  e.name.assign(reinterpret_cast<const char*>(r.p + r.at), nameSize);
  r.at += nameSize + 1;
  for (float* v : {e.pos, e.rot, e.scale}) {
    for (int i = 0; i < 3; ++i) {
      v[i] = r.F32();
    }
  }
  e.part = r.U32();
  r.U32();  // ELSC
  e.hotInThermal = r.Bool();
  e.noTimerUnlessAreaOccluded = r.Bool();
  e.rebuildSystemsOnActivate = r.Bool();
  e.active = r.Bool();
  e.useRateInverseCamDist = r.Bool();
  e.rateInverseCamDist = r.F32();
  e.rateInverseCamDistRate = r.F32();
  e.duration = r.F32();
  e.durationResetWhileVisible = r.F32();
  e.useRateCamDistRange = r.Bool();
  e.rateCamDistRangeMin = r.F32();
  e.rateCamDistRangeMax = r.F32();
  e.rateCamDistRangeFarRate = r.F32();
  e.combatVisorVisible = r.Bool();
  e.thermalVisorVisible = r.Bool();
  e.xrayVisorVisible = r.Bool();
  e.dieWhenSystemsDone = r.Bool();
  if (!r.ok || r.size - r.at != sizeof kLight || std::memcmp(r.p + r.at, kLight, sizeof kLight) != 0) {
    return false;
  }
  used = 5 + size_t(length);
  return true;
}

std::vector<uint8_t> Write(const File& file) {
  std::vector<uint8_t> out;
  PutLE32(out, kFileMagic);
  const bool placements = std::any_of(file.overrides.begin(), file.overrides.end(),
                                    [](const Override& o) { return o.hasPlacement; });
  PutLE32(out, placements ? kFileVersion : 4);
  PutLE32(out, uint32_t(file.skips.size()));
  for (uint32_t id : file.skips) {
    PutLE32(out, id);
  }
  PutLE32(out, file.connected);
  PutLE32(out, uint32_t(file.layers.size()));
  auto putVariables = [&out](const std::vector<Variable>& vars) {
    PutLE32(out, uint32_t(vars.size()));
    for (const Variable& v : vars) {
      out.insert(out.end(), v.guid.begin(), v.guid.end());
      for (float f : v.color) {
        PutLEFloat(out, f);
      }
      for (float f : v.vec) {
        PutLEFloat(out, f);
      }
      PutLEFloat(out, v.real);
      PutLE32(out, uint32_t(v.integer));
    }
  };
  for (const Layer& layer : file.layers) {
    PutLE32(out, layer.layer);
    PutLE32(out, layer.count);
    PutLE32(out, uint32_t(layer.objects.size()));
    out.insert(out.end(), layer.objects.begin(), layer.objects.end());
    for (uint32_t i = 0; i < layer.count; ++i) {
      static const std::vector<Variable> kNoVariables;
      putVariables(i < layer.vars.size() ? layer.vars[i] : kNoVariables);
    }
  }
  PutLE32(out, uint32_t(file.overrides.size()));
  for (const Override& over : file.overrides) {
    PutLE32(out, over.id);
    PutLE32(out, over.part);
    putVariables(over.vars);
  }
  PutLE32(out, uint32_t(file.barriers.size()));
  for (const BarrierGroup& group : file.barriers) {
    PutLE32(out, group.controller);
    PutLEFloat(out, group.duration);
    for (const BarrierRing& ring : group.rings) {
      PutLE32(out, ring.platform);
      PutLE32(out, ring.refraction);
      PutLE32(out, ring.glow);
      for (const std::array<uint8_t, 16>* guid : {&ring.breakupRefraction, &ring.motion, &ring.breakupGlow}) {
        out.insert(out.end(), guid->begin(), guid->end());
      }
    }
  }
  if (placements) {
    for (const Override& over : file.overrides) {
      PutLE32(out, over.hasPlacement ? 1 : 0);
      if (over.hasPlacement) {
        for (const float* v : {over.pos, over.rot, over.scale}) {
          for (int i = 0; i < 3; ++i) PutLEFloat(out, v[i]);
        }
      }
    }
  }
  return out;
}

bool Parse(const std::vector<uint8_t>& data, File& out, std::string& error) {
  out = {};
  // Everything lands in `file`, which `out` only takes once the whole file has parsed (a
  // barrier is a set of cross references, so half of one is worth less than nothing).
  File file;
  std::vector<LayerObject> objects;
  size_t at = 0;
  uint32_t magic = 0, version = 0, skips = 0, layers = 0;
  if (!ReadLE32(data, at, magic) || magic != kFileMagic) {
    error = "not a roomfx file";
    return false;
  }
  if (!ReadLE32(data, at, version) || version < 1 || version > kFileVersion) {
    error = "unknown version";
    return false;
  }
  if (!ReadLE32(data, at, skips) || skips > (data.size() - at) / 4) {
    error = "truncated skip list";
    return false;
  }
  for (uint32_t i = 0; i < skips; ++i) {
    uint32_t id;
    ReadLE32(data, at, id);
    file.skips.push_back(id);
  }
  if (!ReadLE32(data, at, file.connected) || !ReadLE32(data, at, layers) || layers > 64) {
    error = "truncated header";
    return false;
  }
  size_t total = 0;
  for (uint32_t l = 0; l < layers; ++l) {
    Layer layer;
    uint32_t bytes = 0;
    if (!ReadLE32(data, at, layer.layer) || !ReadLE32(data, at, layer.count) || !ReadLE32(data, at, bytes) ||
        bytes > data.size() - at || layer.layer >= 64) {
      error = "truncated layer " + std::to_string(l);
      return false;
    }
    layer.objects.assign(data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + bytes));
    at += bytes;
    size_t o = 0;
    for (uint32_t i = 0; i < layer.count; ++i) {
      Effect e;
      size_t used = 0;
      if (!ReadEffect(layer.objects.data() + o, layer.objects.size() - o, e, used)) {
        error = "layer " + std::to_string(layer.layer) + ": bad object " + std::to_string(i);
        return false;
      }
      if (!IsAddOnId(e.id) || (e.id >> 26) != layer.layer) {
        error = "layer " + std::to_string(layer.layer) + ": object " + std::to_string(i) + " has an id outside the range";
        return false;
      }
      objects.push_back({e.id, e.part, e.active});
      o += used;
    }
    if (o != layer.objects.size()) {
      error = "layer " + std::to_string(layer.layer) + ": bytes after the objects";
      return false;
    }
    layer.vars.resize(layer.count);
    for (uint32_t i = 0; version >= 2 && i < layer.count; ++i) {
      uint32_t n = 0;
      if (!ReadLE32(data, at, n) || n > kMaxVariables || n > (data.size() - at) / kVariableBytes) {
        error = "layer " + std::to_string(layer.layer) + ": truncated variables of object " + std::to_string(i);
        return false;
      }
      ReadVariables(data, at, n, layer.vars[i]);
    }
    total += layer.count;
    file.layers.push_back(std::move(layer));
  }
  if (total > kMaxAddOns) {
    error = "too many objects";
    return false;
  }
  if (version >= 3) {
    uint32_t overrides = 0;
    if (!ReadLE32(data, at, overrides) || overrides > (data.size() - at) / 12) {
      error = "truncated override list";
      return false;
    }
    for (uint32_t i = 0; i < overrides; ++i) {
      Override over;
      uint32_t n = 0;
      if (!ReadLE32(data, at, over.id) || !ReadLE32(data, at, over.part) || !ReadLE32(data, at, n) ||
          n > kMaxVariables || n > (data.size() - at) / kVariableBytes) {
        error = "truncated override " + std::to_string(i);
        return false;
      }
      ReadVariables(data, at, n, over.vars);
      file.overrides.push_back(std::move(over));
    }
  }
  if (version >= 4) {
    uint32_t groups = 0;
    if (!ReadLE32(data, at, groups)) {
      error = "truncated barrier list";
      return false;
    }
    if (groups > kMaxBarrierGroups) {
      error = "too many barrier groups";
      return false;
    }
    if (groups > (data.size() - at) / kBarrierGroupBytes) {
      error = "truncated barriers";
      return false;
    }
    for (uint32_t g = 0; g < groups; ++g) {
      BarrierGroup group;
      uint32_t bits = 0;
      if (!ReadLE32(data, at, group.controller) || !ReadLE32(data, at, bits)) {
        error = "truncated barrier " + std::to_string(g);
        return false;
      }
      std::memcpy(&group.duration, &bits, 4);
      for (BarrierRing& ring : group.rings) {
        if (!ReadLE32(data, at, ring.platform) || !ReadLE32(data, at, ring.refraction) ||
            !ReadLE32(data, at, ring.glow)) {
          error = "truncated barrier " + std::to_string(g);
          return false;
        }
        for (std::array<uint8_t, 16>* guid : {&ring.breakupRefraction, &ring.motion, &ring.breakupGlow}) {
          if (at > data.size() || data.size() - at < 16) {
            error = "truncated barrier " + std::to_string(g);
            return false;
          }
          std::memcpy(guid->data(), data.data() + at, 16);
          at += 16;
        }
      }
      file.barriers.push_back(group);
    }
  }
  if (version >= 5) {
    for (Override& over : file.overrides) {
      uint32_t flag = 0;
      if (!ReadLE32(data, at, flag) || flag > 1) {
        error = "invalid override placement flag";
        return false;
      }
      over.hasPlacement = flag != 0;
      if (!over.hasPlacement) continue;
      for (float* v : {over.pos, over.rot, over.scale}) {
        for (int i = 0; i < 3; ++i) {
          uint32_t bits = 0;
          if (!ReadLE32(data, at, bits)) {
            error = "truncated override placement";
            return false;
          }
          std::memcpy(v + i, &bits, 4);
          if (!std::isfinite(v[i])) {
            error = "non-finite override placement";
            return false;
          }
        }
      }
    }
  }
  if (at != data.size()) {
    error = "bytes after the layers";
    return false;
  }
  if (!ValidateBarriers(file, objects, error)) {
    return false;
  }
  out = std::move(file);
  return true;
}

bool ReadExternalVariables(const uint8_t* data, size_t size, std::vector<Variable>& out) {
  constexpr uint32_t kInfo = 0xae893172, kGuid = 0x82dde61c;
  constexpr uint32_t kReal = 0xfd41dc4b, kInt = 0xcd774f7a, kColor = 0x45eff31d, kVector = 0x282df928;
  auto le16 = [&](size_t at) { return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8; };
  auto le32 = [&](size_t at) { return le16(at) | le16(at + 2) << 16; };
  auto f32 = [&](size_t at) {
    const uint32_t v = le32(at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  out.clear();
  if (size < 4) {
    return false;
  }
  const uint32_t count = le32(0);
  if (count > kMaxVariables) {
    return false;
  }
  size_t at = 4;
  for (uint32_t i = 0; i < count; ++i) {
    if (size - at < 2) {
      return false;
    }
    const uint32_t props = le16(at);
    at += 2;
    Variable v;
    bool guid = false;
    for (uint32_t k = 0; k < props; ++k) {
      if (size - at < 6) {
        return false;
      }
      const uint32_t id = le32(at);
      const size_t n = le16(at + 4);
      at += 6;
      if (size - at < n) {
        return false;
      }
      if (id == kInfo) {
        // A property list of its own: the guid is the one 16-byte property it needs.
        size_t p = at + 2;
        for (uint32_t j = 0, m = n >= 2 ? le16(at) : 0; j < m && at + n - p >= 6; ++j) {
          const uint32_t sub = le32(p);
          const size_t subSize = le16(p + 4);
          p += 6;
          if (at + n - p < subSize) {
            return false;
          }
          if (sub == kGuid && subSize == 16) {
            std::memcpy(v.guid.data(), data + p, 16);
            guid = true;
          }
          p += subSize;
        }
      } else if (id == kReal && n == 4) {
        v.real = f32(at);
      } else if (id == kInt && n == 4) {
        v.integer = int32_t(le32(at));
      } else if (id == kColor && n == 16) {
        for (int c = 0; c < 4; ++c) {
          v.color[c] = f32(at + 4 * size_t(c));
        }
      } else if (id == kVector && n == 12) {
        for (int c = 0; c < 3; ++c) {
          v.vec[c] = f32(at + 4 * size_t(c));
        }
      }
      at += n;
    }
    if (!guid) {
      return false;
    }
    out.push_back(v);
  }
  return at == size;
}

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kExt[] = ".roomfx";
  const size_t ext = sizeof kExt - 1;
  if (fileName.size() != 8 + ext) {
    return false;
  }
  for (size_t i = 0; i < ext; ++i) {
    if (std::tolower(static_cast<unsigned char>(fileName[8 + i])) != kExt[i]) {
      return false;
    }
  }
  for (size_t i = 0; i < 8; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(fileName[i]))) {
      return false;
    }
  }
  id = uint32_t(std::strtoul(fileName.substr(0, 8).c_str(), nullptr, 16));
  return true;
}

}  // namespace PortRoomEffects
