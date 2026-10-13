// The .roomgeo file. See port_room_geo.h.
#include "port_room_geo.h"
#include "port_strings.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortRoomGeo {
namespace {

constexpr uint32_t kMagic = 0x4752504D; // 'MPRG'
constexpr uint32_t kVersion = 10;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kInstanceBytes = 4 + 12 * 4; // version 1; version 2 adds 4 + links
constexpr size_t kPlatformBytes = 4 + 3 * 4;   // version 3's, after version 2's 4
constexpr size_t kLinkBytes = 8;
constexpr uint32_t kScriptMagic = 0x50524353; // 'SCRP'
constexpr size_t kNodeBytes = 8 + 15 * 4;
constexpr size_t kEdgeBytes = 12;
constexpr uint32_t kGlowMagic = 0x574F4C47; // 'GLOW'
constexpr size_t kGlowBytes = 4 + 3 * 4;
constexpr uint32_t kAnimMagic = 0x4D494E41; // 'ANIM'
constexpr size_t kAnimHeadBytes = 3 * 4;
constexpr size_t kAnimKeyBytes = 7 * 4;
constexpr uint32_t kSkyMagic = 0x20594B53; // 'SKY '
constexpr uint32_t kHideMagic = 0x45444948; // 'HIDE'
constexpr uint32_t kLmapMagic = 0x50414D4C; // 'LMAP'
constexpr uint32_t kLodMagic = 0x444F4C52; // 'RLOD'
constexpr uint32_t kLodVersion = 1;

using port::HexDigit;

using port::ReadLE32;

using port::AppendLE32;
using port::AppendLEFloat;

bool ReadF32(const uint8_t* p, float& out) {
  const uint32_t bits = ReadLE32(p);
  std::memcpy(&out, &bits, 4);
  return std::isfinite(out);
}

// The script section at `at`, which is moved past it.
bool ParseScript(const std::vector<uint8_t>& data, size_t& at, std::vector<Instance>& instances, Script& script,
                 std::string& error) {
  if (data.size() - at < 12) {
    error = "truncated script";
    return false;
  }
  const uint32_t nodes = ReadLE32(data.data() + at + 4), edges = ReadLE32(data.data() + at + 8);
  at += 12;
  const size_t left = data.size() - at;
  if (nodes > left / kNodeBytes || edges > (left - nodes * kNodeBytes) / kEdgeBytes ||
      (left - nodes * kNodeBytes - edges * kEdgeBytes) / 4 < instances.size()) {
    error = "truncated script";
    return false;
  }
  script.nodes.resize(nodes);
  for (ScriptNode& node : script.nodes) {
    const uint8_t* const p = data.data() + at;
    node.kind = p[0];
    node.active = p[1] != 0;
    node.max = ReadLE32(p + 4);
    float* const fields[] = {node.centre, node.half, node.axes};
    const int counts[] = {3, 3, 9};
    size_t o = 8;
    for (int f = 0; f < 3; ++f) {
      for (int j = 0; j < counts[f]; ++j, o += 4) {
        if (!ReadF32(p + o, fields[f][j])) {
          error = "bad script volume";
          return false;
        }
      }
    }
    if (node.kind < kCameraVolume || node.kind > kRelay) {
      error = "unknown script node";
      return false;
    }
    at += kNodeBytes;
  }
  script.edges.resize(edges);
  for (ScriptEdge& edge : script.edges) {
    const uint8_t* const p = data.data() + at;
    edge.retail = p[0] != 0;
    edge.event = p[1];
    edge.action = p[2];
    edge.from = ReadLE32(p + 4);
    edge.to = ReadLE32(p + 8);
    const bool toNode = edge.action != kGroupShow && edge.action != kGroupHide && edge.action != kGroupToggle &&
                        edge.action != kGroupNextClip;
    if ((!edge.retail && edge.from >= nodes) || edge.action < kIncrement || edge.action > kGroupNextClip ||
        (toNode && edge.to >= nodes)) {
      error = "bad script edge";
      return false;
    }
    at += kEdgeBytes;
  }
  for (Instance& instance : instances) {
    instance.group = ReadLE32(data.data() + at);
    at += 4;
  }
  return true;
}

// The glow section at `at`, which is moved past it.
bool ParseGlow(const std::vector<uint8_t>& data, size_t& at, std::vector<Instance>& instances, std::string& error) {
  if (data.size() - at < 8 || ReadLE32(data.data() + at + 4) > (data.size() - at - 8) / kGlowBytes) {
    error = "truncated glow";
    return false;
  }
  const uint32_t count = ReadLE32(data.data() + at + 4);
  at += 8;
  for (uint32_t i = 0; i < count; ++i, at += kGlowBytes) {
    const uint32_t index = ReadLE32(data.data() + at);
    if (index >= instances.size() || instances[index].glows) {
      error = "bad glow instance";
      return false;
    }
    Instance& instance = instances[index];
    for (int j = 0; j < 3; ++j) {
      if (!ReadF32(data.data() + at + 4 + 4 * j, instance.glow[j]) || instance.glow[j] < 0.f) {
        error = "bad glow";
        return false;
      }
    }
    instance.glows = true;
  }
  return true;
}

// The frames of one clip, `frames` of them at `at`, which is moved past them.
bool ParseFrames(const std::vector<uint8_t>& data, size_t& at, uint32_t frames, Instance::AnimClip& clip,
                 std::string& error) {
  if (frames > (data.size() - at) / kAnimKeyBytes) {
    error = "truncated animation";
    return false;
  }
  clip.keys.resize(size_t(frames) * 7);
  for (size_t f = 0; f < frames; ++f, at += kAnimKeyBytes) {
    float* const key = clip.keys.data() + f * 7;
    for (int j = 0; j < 7; ++j) {
      if (!ReadF32(data.data() + at + 4 * j, key[j])) {
        error = "bad animation";
        return false;
      }
    }
    const float length = std::sqrt(key[0] * key[0] + key[1] * key[1] + key[2] * key[2] + key[3] * key[3]);
    if (!(std::fabs(length - 1.f) <= 1e-3f)) {
      error = "bad animation";
      return false;
    }
  }
  return true;
}

// The animation section at `at`, which is moved past it.
bool ParseAnim(const std::vector<uint8_t>& data, size_t& at, uint32_t version, std::vector<Instance>& instances,
               std::string& error) {
  if (data.size() - at < 8 || ReadLE32(data.data() + at + 4) > (data.size() - at - 8) / kAnimHeadBytes) {
    error = "truncated animation";
    return false;
  }
  const uint32_t count = ReadLE32(data.data() + at + 4);
  at += 8;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < (version >= 8 ? 8 : kAnimHeadBytes)) {
      error = "truncated animation";
      return false;
    }
    const uint32_t index = ReadLE32(data.data() + at);
    if (index >= instances.size() || !instances[index].anim.empty()) {
      error = "bad animation instance";
      return false;
    }
    Instance& instance = instances[index];
    uint32_t clips = 1;
    if (version >= 8) {
      const uint8_t flags = data[at + 4];
      clips = data[at + 5];
      if ((flags & ~1u) != 0 || clips == 0 || data[at + 6] != 0 || data[at + 7] != 0) {
        error = "bad animation";
        return false;
      }
      instance.animOnShow = (flags & 1) != 0;
      at += 8;
    } else {
      at += 4;
    }
    instance.anim.resize(clips);
    for (Instance::AnimClip& clip : instance.anim) {
      if (data.size() - at < 8 + (version >= 8 ? 4u : 0u)) {
        error = "truncated animation";
        return false;
      }
      float fps;
      const uint32_t frames = ReadLE32(data.data() + at + 4);
      if (!ReadF32(data.data() + at, fps) || fps <= 0.f || frames < 2) {
        error = "bad animation";
        return false;
      }
      clip.fps = fps;
      at += 8;
      if (version >= 8) {
        if (data[at] > 1 || data[at + 1] != 0 || data[at + 2] != 0 || data[at + 3] != 0) {
          error = "bad animation";
          return false;
        }
        clip.loop = data[at] != 0;
        at += 4;
      }
      if (!ParseFrames(data, at, frames, clip, error)) {
        return false;
      }
    }
  }
  return true;
}

// The sky section at `at`, which is moved past it.
bool ParseSky(const std::vector<uint8_t>& data, size_t& at, uint32_t version, std::vector<Instance>& instances,
              std::string& error) {
  const size_t stride = version >= 7 ? 16 : 4;
  if (data.size() - at < 8 || ReadLE32(data.data() + at + 4) > (data.size() - at - 8) / stride) {
    error = "truncated sky";
    return false;
  }
  const uint32_t count = ReadLE32(data.data() + at + 4);
  at += 8;
  for (uint32_t i = 0; i < count; ++i, at += stride) {
    const uint32_t index = ReadLE32(data.data() + at);
    if (index >= instances.size() || instances[index].sky) {
      error = "bad sky instance";
      return false;
    }
    Instance& instance = instances[index];
    instance.sky = true;
    for (size_t j = 0; j < 3 && stride == 16; ++j) {
      float v = 0.f;
      instance.skyRadiance[j] = ReadF32(data.data() + at + 4 + 4 * j, v) && v > 0.f ? v : 0.f;
    }
  }
  return true;
}

void WriteScript(std::vector<uint8_t>& out, const std::vector<Instance>& instances, const Script* script) {
  static const Script kNone;
  const Script& s = script != nullptr ? *script : kNone;
  AppendLE32(out, kScriptMagic);
  AppendLE32(out, uint32_t(s.nodes.size()));
  AppendLE32(out, uint32_t(s.edges.size()));
  for (const ScriptNode& node : s.nodes) {
    out.push_back(node.kind);
    out.push_back(node.active ? 1 : 0);
    out.push_back(0);
    out.push_back(0);
    AppendLE32(out, node.max);
    for (float v : node.centre) {
      AppendLEFloat(out, v);
    }
    for (float v : node.half) {
      AppendLEFloat(out, v);
    }
    for (float v : node.axes) {
      AppendLEFloat(out, v);
    }
  }
  for (const ScriptEdge& edge : s.edges) {
    out.push_back(edge.retail ? 1 : 0);
    out.push_back(edge.event);
    out.push_back(edge.action);
    out.push_back(0);
    AppendLE32(out, edge.from);
    AppendLE32(out, edge.to);
  }
  for (const Instance& instance : instances) {
    AppendLE32(out, instance.group);
  }
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  return port::ParseHexFileName(fileName, ".roomgeo", id);
}

bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error, Script* script) {
  out.clear();
  Script scratch;
  Script& parsed = script != nullptr ? *script : scratch;
  parsed = {};
  if (data.size() < kHeaderBytes || ReadLE32(data.data()) != kMagic) {
    error = "not a room geometry file";
    return false;
  }
  const uint32_t version = ReadLE32(data.data() + 4);
  if (version < 1 || version > kVersion) {
    error = "unknown version";
    return false;
  }
  const uint32_t count = ReadLE32(data.data() + 8);
  const size_t instanceBytes =
      version == 1 ? kInstanceBytes : kInstanceBytes + 4 + (version >= 3 ? kPlatformBytes : 0);
  if (count > (data.size() - kHeaderBytes) / instanceBytes) {
    error = "truncated";
    return false;
  }
  out.clear();
  out.resize(count);
  size_t at = kHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < instanceBytes) {
      error = "truncated";
      out.clear();
      return false;
    }
    const uint8_t* const p = data.data() + at;
    Instance& instance = out[i];
    instance.model = ReadLE32(p);
    for (int j = 0; j < 12; ++j) {
      const uint32_t bits = ReadLE32(p + 4 + j * 4);
      std::memcpy(&instance.transform[j], &bits, 4);
      if (!std::isfinite(instance.transform[j])) {
        error = "bad transform";
        out.clear();
        return false;
      }
    }
    at += instanceBytes;
    if (version == 1) {
      continue;
    }
    instance.layer = p[kInstanceBytes];
    instance.active = p[kInstanceBytes + 1] != 0;
    const size_t links = size_t(p[kInstanceBytes + 2]) | size_t(p[kInstanceBytes + 3]) << 8;
    if (version >= 3) {
      const uint8_t* const q = p + kInstanceBytes + 4;
      instance.platform = ReadLE32(q);
      for (int j = 0; j < 3; ++j) {
        const uint32_t bits = ReadLE32(q + 4 + j * 4);
        std::memcpy(&instance.platformStart[j], &bits, 4);
        if (!std::isfinite(instance.platformStart[j])) {
          error = "bad platform position";
          out.clear();
          return false;
        }
      }
    }
    if ((data.size() - at) / kLinkBytes < links) {
      error = "truncated";
      out.clear();
      return false;
    }
    instance.links.resize(links);
    for (Link& link : instance.links) {
      link.sender = ReadLE32(data.data() + at);
      link.state = data[at + 4];
      link.action = data[at + 5];
      link.delay = float(size_t(data[at + 6]) | size_t(data[at + 7]) << 8) / 100.f;
      at += kLinkBytes;
    }
  }
  // Version 3 may end with the script section, version 4 then with the glow section,
  // version 5 then with the animation section, version 6 then with the sky section
  // (with radiances from version 7), version 9 then with the hidden objects and version 10
  // with the lightmap lookups.
  bool ok = true;
  if (version >= 3 && data.size() - at >= 4 && ReadLE32(data.data() + at) == kScriptMagic) {
    ok = ParseScript(data, at, out, parsed, error);
  }
  if (ok && version >= 4 && data.size() - at >= 4 && ReadLE32(data.data() + at) == kGlowMagic) {
    ok = ParseGlow(data, at, out, error);
  }
  if (ok && version >= 5 && data.size() - at >= 4 && ReadLE32(data.data() + at) == kAnimMagic) {
    ok = ParseAnim(data, at, version, out, error);
  }
  if (ok && version >= 6 && data.size() - at >= 4 && ReadLE32(data.data() + at) == kSkyMagic) {
    ok = ParseSky(data, at, version, out, error);
  }
  if (ok && version >= 9 && data.size() - at >= 4 && ReadLE32(data.data() + at) == kHideMagic) {
    const uint32_t count = data.size() - at >= 8 ? ReadLE32(data.data() + at + 4) : 0;
    if (count == 0 || count > (data.size() - at - 8) / 8) {
      error = "truncated hidden objects";
      ok = false;
    } else {
      for (uint32_t i = 0; i < count && ok; ++i) {
        const Script::Hidden h{ReadLE32(data.data() + at + 8 + 8 * size_t(i)),
                               ReadLE32(data.data() + at + 12 + 8 * size_t(i))};
        if (h.instance >= out.size()) {
          error = "hidden object of a missing instance";
          ok = false;
        } else {
          parsed.hidden.push_back(h);
        }
      }
      at += 8 + 8 * size_t(count);
    }
  }
  if (ok && version >= 10 && data.size() - at >= 4 && ReadLE32(data.data() + at) == kLmapMagic) {
    if (data.size() - at < 8 || ReadLE32(data.data() + at + 4) != out.size() ||
        (data.size() - at - 8) / 12 < out.size()) {
      error = "truncated lightmap lookups";
      ok = false;
    } else {
      at += 8;
      for (Instance& instance : out) {
        for (size_t j = 0; j < 3; ++j) {
          float v = 0.f;
          instance.lightmap[j] = ReadF32(data.data() + at + 4 * j, v) ? v : 0.f;
        }
        if (!HasLightmapLookup(instance.lightmap)) {
          instance.lightmap[0] = instance.lightmap[1] = instance.lightmap[2] = 0.f;
        }
        at += 12;
      }
    }
  }
  if (ok && at != data.size()) {
    error = "unknown data after the instances";
    ok = false;
  }
  if (!ok) {
    out.clear();
    parsed = {};
    return false;
  }
  return true;
}

std::vector<uint8_t> Write(const std::vector<Instance>& instances, const Script* script) {
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + instances.size() * kInstanceBytes);
  AppendLE32(out, kMagic);
  AppendLE32(out, kVersion);
  AppendLE32(out, uint32_t(instances.size()));
  for (const Instance& instance : instances) {
    AppendLE32(out, instance.model);
    for (int j = 0; j < 12; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &instance.transform[j], 4);
      AppendLE32(out, bits);
    }
    const size_t links = instance.links.size() < 0xffff ? instance.links.size() : 0xffff;
    out.push_back(instance.layer);
    out.push_back(instance.active ? 1 : 0);
    out.push_back(uint8_t(links));
    out.push_back(uint8_t(links >> 8));
    AppendLE32(out, instance.platform);
    for (int j = 0; j < 3; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &instance.platformStart[j], 4);
      AppendLE32(out, bits);
    }
    for (size_t j = 0; j < links; ++j) {
      AppendLE32(out, instance.links[j].sender);
      out.push_back(instance.links[j].state);
      out.push_back(instance.links[j].action);
      const float delay = std::isfinite(instance.links[j].delay) ? instance.links[j].delay * 100.f : 0.f;
      const auto centis = uint16_t(std::clamp(std::lround(delay), 0l, 0xffffl));
      out.push_back(uint8_t(centis));
      out.push_back(uint8_t(centis >> 8));
    }
  }
  const bool grouped =
      std::any_of(instances.begin(), instances.end(), [](const Instance& i) { return i.group != kNoGroup; });
  if ((script != nullptr && !script->Empty()) || grouped) {
    WriteScript(out, instances, script);
  }
  const size_t glows =
      size_t(std::count_if(instances.begin(), instances.end(), [](const Instance& i) { return i.glows; }));
  if (glows != 0) {
    AppendLE32(out, kGlowMagic);
    AppendLE32(out, uint32_t(glows));
    for (size_t i = 0; i < instances.size(); ++i) {
      if (instances[i].glows) {
        AppendLE32(out, uint32_t(i));
        for (float v : instances[i].glow) {
          AppendLEFloat(out, v);
        }
      }
    }
  }
  const size_t anims =
      size_t(std::count_if(instances.begin(), instances.end(), [](const Instance& i) { return !i.anim.empty(); }));
  if (anims != 0) {
    AppendLE32(out, kAnimMagic);
    AppendLE32(out, uint32_t(anims));
    for (size_t i = 0; i < instances.size(); ++i) {
      const Instance& instance = instances[i];
      if (!instance.anim.empty()) {
        AppendLE32(out, uint32_t(i));
        out.push_back(instance.animOnShow ? 1 : 0);
        out.push_back(uint8_t(instance.anim.size()));
        out.push_back(0);
        out.push_back(0);
        for (const Instance::AnimClip& clip : instance.anim) {
          AppendLEFloat(out, clip.fps);
          AppendLE32(out, uint32_t(clip.keys.size() / 7));
          out.push_back(clip.loop ? 1 : 0);
          out.push_back(0);
          out.push_back(0);
          out.push_back(0);
          for (size_t k = 0; k < clip.keys.size() / 7 * 7; ++k) {
            AppendLEFloat(out, clip.keys[k]);
          }
        }
      }
    }
  }
  const size_t skies =
      size_t(std::count_if(instances.begin(), instances.end(), [](const Instance& i) { return i.sky; }));
  if (skies != 0) {
    AppendLE32(out, kSkyMagic);
    AppendLE32(out, uint32_t(skies));
    for (size_t i = 0; i < instances.size(); ++i) {
      if (instances[i].sky) {
        AppendLE32(out, uint32_t(i));
        for (float v : instances[i].skyRadiance) {
          AppendLEFloat(out, v);
        }
      }
    }
  }
  if (script != nullptr && !script->hidden.empty()) {
    AppendLE32(out, kHideMagic);
    AppendLE32(out, uint32_t(script->hidden.size()));
    for (const Script::Hidden& h : script->hidden) {
      AppendLE32(out, h.editorId);
      AppendLE32(out, h.instance);
    }
  }
  const bool lit = std::any_of(instances.begin(), instances.end(), [](const Instance& i) { return HasLightmapLookup(i.lightmap); });
  if (lit) {
    AppendLE32(out, kLmapMagic);
    AppendLE32(out, uint32_t(instances.size()));
    for (const Instance& instance : instances) {
      for (float v : instance.lightmap) {
        AppendLEFloat(out, v);
      }
    }
  }
  return out;
}

bool ParseLods(const std::vector<uint8_t>& data, std::vector<Lods>& out, std::string& error) {
  out.clear();
  if (data.size() < kHeaderBytes || ReadLE32(data.data()) != kLodMagic) {
    error = "not a level of detail table";
    return false;
  }
  if (ReadLE32(data.data() + 4) != kLodVersion) {
    error = "unknown version " + std::to_string(ReadLE32(data.data() + 4));
    return false;
  }
  const uint32_t count = ReadLE32(data.data() + 8);
  size_t at = kHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    Lods& lods = out.emplace_back();
    lods.model = ReadLE32(data.data() + at);
    const uint32_t levels = ReadLE32(data.data() + at + 4);
    at += 8;
    if (levels == 0 || levels >= uint32_t(kLodLevels) || data.size() - at < size_t(levels) * 8) {
      error = "a model with " + std::to_string(levels) + " levels";
      return false;
    }
    for (uint32_t l = 0; l < levels; ++l) {
      LodLevel& level = lods.levels.emplace_back();
      level.model = ReadLE32(data.data() + at + 4);
      if (!ReadF32(data.data() + at, level.distanceSq) || level.distanceSq <= 0.f ||
          (l > 0 && level.distanceSq <= lods.levels[l - 1].distanceSq)) {
        error = "a level's distance is out of order";
        return false;
      }
      at += 8;
    }
  }
  return true;
}

std::vector<uint8_t> WriteLods(const std::vector<Lods>& models) {
  std::vector<uint8_t> out;
  AppendLE32(out, kLodMagic);
  AppendLE32(out, kLodVersion);
  AppendLE32(out, uint32_t(models.size()));
  for (const Lods& lods : models) {
    AppendLE32(out, lods.model);
    AppendLE32(out, uint32_t(lods.levels.size()));
    for (const LodLevel& level : lods.levels) {
      AppendLEFloat(out, level.distanceSq);
      AppendLE32(out, level.model);
    }
  }
  return out;
}

} // namespace PortRoomGeo
