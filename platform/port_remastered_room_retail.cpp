// The Remastered room environment writer's retail world reader (MLVL areas and SCLY
// objects), a port of build/mpr/roomtools/gcres.py. See port_remastered_room_internal.h.

#include "port_remastered_room_internal.h"

namespace PortRemastered {
namespace RoomDetail {

// ---------------------------------------------------------------------------
// Retail world (gcres.py)
// ---------------------------------------------------------------------------


// The MLVL's areas: id and transform.
bool ReadMlvl(const std::vector<uint8_t>& d, std::vector<std::pair<uint32_t, Mat34>>& out) {
  size_t o = 0;
  auto need = [&](size_t n) { return o <= d.size() && n <= d.size() - o; };
  if (!need(24)) {
    return false;
  }
  o += 20;
  uint32_t n = ReadBE32(&d[o]);
  if (n > d.size() / 11 || !need(4 + size_t(n) * 11)) {
    return false;
  }
  o += 4 + size_t(n) * 11;
  if (!need(8)) {
    return false;
  }
  n = ReadBE32(&d[o]);
  o += 8;
  if (n > 4096) {
    return false;
  }
  // Skips a count and `unit` bytes per entry.
  auto skip = [&](size_t unit) {
    if (!need(4)) {
      return false;
    }
    const uint64_t c = ReadBE32(&d[o]);
    if (c * unit > d.size()) {
      return false;
    }
    o += 4;
    if (!need(size_t(c * unit))) {
      return false;
    }
    o += size_t(c * unit);
    return true;
  };
  for (uint32_t i = 0; i < n; ++i) {
    if (!need(84)) {
      return false;
    }
    Mat34 xf{};
    for (size_t k = 0; k < 12; ++k) {
      xf[k / 4][k % 4] = port::ReadBEFloat(&d[o + 4 + 4 * k]);
    }
    out.push_back({ReadBE32(&d[o + 76]), xf});
    o += 84;
    if (!skip(2)) {
      return false;
    }
    o += 4;
    if (!skip(8) || !skip(4) || !need(4)) {
      return false;
    }
    const uint32_t c = ReadBE32(&d[o]);
    o += 4;
    if (c > d.size()) {
      return false;
    }
    for (uint32_t k = 0; k < c; ++k) {
      if (!skip(8) || !skip(12)) {
        return false;
      }
    }
  }
  return true;
}

// The bytes between a script object's name and its position (ScriptLoader): Dock's active
// bool, the patterned flavors (Beetle, WarWasp, FlickerBat, PuddleSpore, PuddleToadGamma,
// Parasite, Ripper, Drone, Metroid, EyeBall) and GunTurret's component.
size_t PositionLead(uint8_t type) {
  switch (type) {
  case 0x0b:
    return 1;
  case 0x16:
  case 0x21:
  case 0x2e:
  case 0x31:
  case 0x34:
  case 0x3d:
  case 0x3f:
  case 0x43:
  case 0x44:
  case 0x64:
  case 0x67:
    return 4;
  default:
    return 0;
  }
}

// The script objects of an MREA's SCLY section.
bool ReadScly(const std::vector<uint8_t>& m, std::vector<ScriptObject>& objects) {
  if (m.size() < 100) {
    return false;
  }
  const uint32_t sections = ReadBE32(&m[60]), scly = ReadBE32(&m[68]);
  if (sections > m.size() / 4 || 96 + size_t(sections) * 4 > m.size() || scly > sections) {
    return false;
  }
  size_t o = (96 + 4 * size_t(sections) + 31) & ~size_t(31);
  for (uint32_t i = 0; i < scly; ++i) {
    o += ReadBE32(&m[96 + 4 * size_t(i)]);
    if (o > m.size()) {
      return false;
    }
  }
  if (o + 12 > m.size() || std::memcmp(&m[o], "SCLY", 4) != 0) {
    return false;
  }
  const uint32_t layers = ReadBE32(&m[o + 8]);
  if (layers > m.size() / 4 || o + 12 + 4 * size_t(layers) > m.size()) {
    return false;
  }
  const size_t sizesAt = o + 12;
  o += 12 + 4 * size_t(layers);
  for (uint32_t l = 0; l < layers; ++l) {
    size_t p = o + 1;
    if (p + 4 > m.size()) {
      return false;
    }
    const uint32_t count = ReadBE32(&m[p]);
    p += 4;
    for (uint32_t i = 0; i < count; ++i) {
      if (p + 5 > m.size()) {
        return false;
      }
      const uint8_t type = m[p];
      const size_t size = ReadBE32(&m[p + 1]);
      const size_t objectEnd = p + 5 + size;
      if (objectEnd > m.size()) {
        return false;
      }
      size_t q = p + 5;
      if (q + 8 > objectEnd) {
        return false;
      }
      ScriptObject object;
      object.id = ReadBE32(&m[q]);
      object.type = type;
      object.layer = int(l);
      const uint64_t children = ReadBE32(&m[q + 4]);
      if (children > (objectEnd - q - 8) / 12) {
        return false;
      }
      for (uint64_t k = 0; k < children; ++k) {
        const size_t c = q + 8 + 12 * size_t(k);
        object.links.push_back({ReadBE32(&m[c]), ReadBE32(&m[c + 4]), ReadBE32(&m[c + 8])});
        object.targets.push_back(object.links.back().target);
      }
      q += 8 + size_t(12 * children) + 4;
      if (q > objectEnd) {
        return false;
      }
      object.propCount = ReadBE32(&m[q - 4]);
      const uint8_t* zero = static_cast<const uint8_t*>(std::memchr(&m[q], 0, objectEnd - q));
      if (zero == nullptr) {
        return false;
      }
      const size_t rest = size_t(zero - m.data()) + 1;
      const size_t at = rest + PositionLead(type);
      if (objectEnd - rest >= PositionLead(type) + 12) {
        object.hasPos = true;
        object.pos = {port::ReadBEFloat(&m[at]), port::ReadBEFloat(&m[at + 4]), port::ReadBEFloat(&m[at + 8])};
        if (objectEnd - at >= 36) {
          object.hasXf = true;
          for (size_t i = 0; i < 3; ++i) {
            object.rot[i] = port::ReadBEFloat(&m[at + 12 + 4 * i]);
            object.scale[i] = port::ReadBEFloat(&m[at + 24 + 4 * i]);
          }
        }
      } else if (type == 3) {
        return false;
      }
      object.props.assign(m.begin() + std::ptrdiff_t(rest), m.begin() + std::ptrdiff_t(objectEnd));
      objects.push_back(std::move(object));
      p = objectEnd;
    }
    o += ReadBE32(&m[sizesAt + 4 * size_t(l)]);
  }
  return true;
}

}  // namespace RoomDetail
}  // namespace PortRemastered
