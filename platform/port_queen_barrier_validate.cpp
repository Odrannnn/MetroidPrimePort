#include "port_queen_barrier_validate.h"
#include "port_remastered_effect_convert.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace PortQueenBarrier {
namespace {
constexpr uint32_t Tag(const char* s) {
  return uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | uint32_t(s[3]);
}
uint32_t Word(const std::vector<uint8_t>& b, size_t p) {
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 | uint32_t(b[p + 2]) << 8 | b[p + 3];
}
bool Finite(const std::vector<uint8_t>& b, size_t p) {
  uint32_t w = Word(b, p);
  float f;
  std::memcpy(&f, &w, 4);
  return std::isfinite(f);
}
}

bool ValidatePart(const std::vector<uint8_t>& part, uint32_t feature,
                  const std::vector<std::array<uint8_t, 16>>& realVars,
                  const std::function<bool(uint32_t, uint32_t)>& dependency, std::string& error) {
  error.clear();
  auto fail = [&](const char* why) { error = why; return false; };
  const bool refract = feature == (1u << 29);
  if ((!refract && feature != (1u << 30)) || realVars.size() != (refract ? 2u : 1u) || !dependency)
    return fail("invalid barrier validation request");
  std::vector<PortRemastered::RetailPartProperty> props;
  if (!PortRemastered::SplitRetailPart(part.data(), part.size(), props, error)) return false;
  size_t parsedBytes = 8; // GPSM plus _END; the splitter intentionally allows trailing data.
  for (const auto& p : props) parsedBytes += 4 + p.value.size();
  if (parsedBytes != part.size()) return fail("trailing PART data");
  std::map<uint32_t, const std::vector<uint8_t>*> values;
  for (const auto& p : props) {
    if (!values.emplace(p.fourcc, &p.value).second) return fail("duplicate PART property");
  }
  auto property = [&](const char* name) -> const std::vector<uint8_t>* {
    auto it = values.find(Tag(name));
    return it == values.end() ? nullptr : it->second;
  };
  auto blob = [&](const char* name) -> const std::vector<uint8_t>* {
    const auto* b = property(name);
    return b && b->size() >= 8 && Word(*b, 0) == Tag("CNST") && Word(*b, 4) == b->size() - 8 ? b : nullptr;
  };
  const auto* mat = blob("VMAT");
  if (!mat || mat->size() < 24) return fail("missing VMAT");
  const uint32_t version = Word(*mat, 8), textures = Word(*mat, 20);
  // Exact exposure policy: neither verified barrier recipe emits AlreadyExposed.
  // The converter's ModelBackCull bit (1 << 17) is a draw-state flag and leaves the layout unchanged.
  constexpr uint32_t kModelBackCull = 1u << 17;
  if ((version != 2 && version != 3) || (Word(*mat, 12) & ~kModelBackCull) != feature || Word(*mat, 16) > 7 || textures == 0 || textures > 4 ||
      mat->size() != 24 + 44 * textures + 184 + (version == 3 ? 8 : 0)) return fail("invalid VMAT layout or feature");
  for (uint32_t i = 0; i < textures; ++i) {
    size_t at = 24 + 44 * i;
    uint32_t id = Word(*mat, at);
    if (!id || id == 0xffffffff || !dependency(Tag("TXTR"), id)) return fail("missing barrier texture");
    // These barrier maps are ordinary 2D textures, not tiled flipbook atlases.
    if (Word(*mat, at + 4) > 2 || Word(*mat, at + 8) > 2 || Word(*mat, at + 12) > 2 ||
        Word(*mat, at + 16) > 1 || Word(*mat, at + 20) != 1 || Word(*mat, at + 24) != 1 ||
        Word(*mat, at + 28) != 1 || Word(*mat, at + 32) != 0)
      return fail("unsupported barrier texture layout");
    if (!Finite(*mat, at + 36) || !Finite(*mat, at + 40)) return fail("nonfinite texture transform");
  }
  const size_t slots = 24 + 44 * textures;
  if (Word(*mat, slots) >= textures || (refract && (Word(*mat, slots + 20) >= textures ||
      Word(*mat, slots) == Word(*mat, slots + 20))))
    return fail("missing barrier texture slot");
  for (size_t i = 0; i < 7; ++i) {
    const uint32_t slot = Word(*mat, slots + 4 * i);
    if (slot != 0xffffffff && slot >= textures) return fail("invalid material texture slot");
  }
  // Eleven Src triples follow the ten slot/row words; only the third word is a float.
  for (size_t i = 0; i < 11; ++i)
    if (!Finite(*mat, slots + 40 + 12 * i + 8)) return fail("nonfinite material source");
  if (!Finite(*mat, slots + 172) || !Finite(*mat, slots + 176) ||
      (version == 3 && (!Finite(*mat, slots + 184) || !Finite(*mat, slots + 188))))
    return fail("nonfinite material parameter");
  if (Word(*mat, slots + 180) > 8) return fail("invalid sprite centre");
  const auto* mesh = blob("VMSH");
  if (!mesh || mesh->size() < 72 || Word(*mesh, 8) != 2 || !(Word(*mesh, 20) & 2) || (Word(*mesh, 20) & ~3u))
    return fail("missing coloured VMSH v2");
  const uint32_t nv = Word(*mesh, 12), nt = Word(*mesh, 16);
  const size_t width = nv > 65535 ? 4 : 2;
  if (!nv || !nt || uint64_t(mesh->size()) != 72ull + uint64_t(nv) * 64 + uint64_t(nt) * 3 * width)
    return fail("invalid VMSH framing");
  for (size_t at = 24; at < 72 + size_t(nv) * 64; at += 4)
    if (!Finite(*mesh, at)) return fail("nonfinite VMSH");
  for (size_t at = 72 + size_t(nv) * 64; at < mesh->size(); at += width) {
    uint32_t index = width == 4 ? Word(*mesh, at) : (uint32_t((*mesh)[at]) << 8 | (*mesh)[at + 1]);
    if (index >= nv) return fail("VMSH index out of bounds");
  }
  const auto* params = blob("VPRM");
  if (!params || params->size() != 36 || Word(*params, 8) != 1 || Word(*params, 12) != 0xffffffff || Word(*params, 16) != 1)
    return fail("missing CCH0 row");
  for (size_t at = 20; at < 36; at += 4) if (!Finite(*params, at)) return fail("nonfinite CCH0");
  const auto* xfmd = property("XFMD");
  if (!xfmd || xfmd->size() != 8 || Word(*xfmd, 0) != Tag("CNST") || Word(*xfmd, 4) != 3)
    return fail("barrier does not follow emitter");
  const auto* vars = property("PVRT");
  if (!vars || vars->size() != 8 + 36 * realVars.size() || Word(*vars, 0) != Tag("CNST") ||
      Word(*vars, 4) != realVars.size()) return fail("invalid barrier variable table");
  std::vector<uint32_t> indices;
  for (size_t i = 0; i < realVars.size(); ++i)
    for (size_t c = 0; c < 4; ++c)
      if (!Finite(*vars, 8 + 36 * i + 20 + 4 * c)) return fail("nonfinite variable default");
  for (const auto& guid : realVars) {
    if (std::all_of(guid.begin(), guid.end(), [](uint8_t x) { return x == 0; })) return fail("zero variable GUID");
    uint32_t found = 0xffffffff;
    for (uint32_t i = 0; i < realVars.size(); ++i) {
      size_t at = 8 + i * 36;
      if (std::equal(guid.begin(), guid.end(), vars->begin() + at)) {
        if (found != 0xffffffff || Word(*vars, at + 16) != 0) return fail("ambiguous or nonreal variable");
        found = i;
      }
    }
    if (found == 0xffffffff || std::find(indices.begin(), indices.end(), found) != indices.end())
      return fail("missing or repeated variable GUID");
    indices.push_back(found);
  }
  const auto* track = property("VSMT");
  if (!track || track->size() != 8 + 16 * indices.size() || Word(*track, 0) != Tag("CNST") ||
      Word(*track, 4) != indices.size()) return fail("invalid barrier material track");
  std::vector<bool> seen(indices.size());
  for (size_t at = 8; at < track->size(); at += 16) {
    uint32_t target = Word(*track, at);
    if (target < 32 || target >= 32 + indices.size() || seen[target - 32] || Word(*track, at + 4) != 0 ||
        Word(*track, at + 8) != Tag("VARF") || Word(*track, at + 12) != indices[target - 32])
      return fail("incorrect barrier variable target");
    seen[target - 32] = true;
  }
  return true;
}
}
