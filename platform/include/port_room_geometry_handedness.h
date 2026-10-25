#pragma once

#include <dolphin/gx/GXGet.h>
#include <dolphin/gx/GXCull.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace PortRoomGeometryHandedness {

// The view/projection already has the engine's usual winding convention. Only
// a reflection in this model-to-world transform reverses it a second time.
template <class Matrix>
bool Mirrored(const Matrix& m) {
  const double determinant = double(m.Get00()) * (double(m.Get11()) * m.Get22() - double(m.Get12()) * m.Get21()) -
                             double(m.Get01()) * (double(m.Get10()) * m.Get22() - double(m.Get12()) * m.Get20()) +
                             double(m.Get02()) * (double(m.Get10()) * m.Get21() - double(m.Get11()) * m.Get20());
  return determinant < 0.0;
}

// GX records culling into each queued pipeline. Preserve its actual state (a
// shadow or diagnostic pass may have selected another convention), including
// NONE/ALL, and restore it before the next model. Material/fallback/depth/shadow
// draws inside this scope all see the same corrected winding.
class CullScope {
public:
  explicit CullScope(bool mirrored) {
    if (!mirrored) return;
    GXGetCullMode(&saved);
    const GXCullMode next = saved == GX_CULL_FRONT ? GX_CULL_BACK : saved == GX_CULL_BACK ? GX_CULL_FRONT : saved;
    changed = next != saved;
    if (changed) GXSetCullMode(next);
  }
  ~CullScope() {
    if (changed) GXSetCullMode(saved);
  }
  CullScope(const CullScope&) = delete;
  CullScope& operator=(const CullScope&) = delete;

private:
  GXCullMode saved = GX_CULL_NONE;
  bool changed = false;
};

// Baking a mirrored instance into a merged CMDL removes its model matrix, so
// its primitive winding must be corrected in the bytes instead. Triangle strips
// and fans become triangle lists: reversing a strip in place changes winding
// only for some vertex counts. Attribute bytes remain attached to their vertex.
// Unsupported commands reject merging and keep the ordinary scoped draw path.
inline bool PrimitiveOrder(uint8_t opcode, size_t count, bool mirrored,
                           uint8_t& outputOpcode, std::vector<size_t>& order) {
  outputOpcode = opcode;
  order.clear();
  if (!mirrored) {
    order.resize(count);
    std::iota(order.begin(), order.end(), size_t(0));
    return true;
  }
  const uint8_t primitive = opcode & 0xf8;
  const auto triangle = [&](size_t a, size_t b, size_t c) {
    order.push_back(a);
    order.push_back(c);
    order.push_back(b);
  };
  switch (primitive) {
  case GX_TRIANGLES:
    if (count % 3 != 0) return false;
    for (size_t i = 0; i < count; i += 3) triangle(i, i + 1, i + 2);
    break;
  case GX_QUADS:
    if (count % 4 != 0) return false;
    for (size_t i = 0; i < count; i += 4) {
      triangle(i, i + 1, i + 2);
      triangle(i, i + 2, i + 3);
    }
    break;
  case GX_TRIANGLESTRIP:
    if (count < 3) return false;
    for (size_t i = 0; i + 2 < count; ++i) {
      if ((i & 1) != 0) triangle(i + 1, i, i + 2);
      else triangle(i, i + 1, i + 2);
    }
    break;
  case GX_TRIANGLEFAN:
    if (count < 3) return false;
    for (size_t i = 1; i + 1 < count; ++i) triangle(0, i, i + 1);
    break;
  case GX_LINES:
  case GX_LINESTRIP:
  case GX_POINTS:
    order.resize(count);
    std::iota(order.begin(), order.end(), size_t(0));
    return true;
  default:
    return false;
  }
  outputOpcode = GX_TRIANGLES | (opcode & 7); // retain its vertex format
  return true;
}

struct PrimitiveLayout {
  size_t stride = 0;
  size_t normalAt = SIZE_MAX;
  size_t positionBase = 0;
  size_t normalBase = 0;
};

// Append one merged display-list primitive. Positions start each attribute
// record as INDEX16; an optional normal index is also INDEX16. Other attribute
// bytes travel with their vertex unchanged. Validate before appending so a bad
// source layout/index can fall back to an ordinary draw without partial output.
inline bool AppendPrimitive(uint8_t opcode, size_t count, std::span<const uint8_t> vertices,
                            const PrimitiveLayout& layout, bool mirrored, std::vector<uint8_t>& out) {
  constexpr size_t kMaxVertices = 0xffff;
  const size_t stride = layout.stride;
  const bool normals = layout.normalAt != SIZE_MAX;
  if (count == 0 || count > kMaxVertices || stride < 2 || count > SIZE_MAX / stride ||
      vertices.size() != count * stride || layout.positionBase > kMaxVertices ||
      (normals && (layout.normalAt < 2 || layout.normalAt > stride - 2 || layout.normalBase > kMaxVertices))) {
    return false;
  }
  // The writer normally reads a separate CMDL. Reject an aliased source span
  // rather than letting output growth invalidate its source records.
  if (!out.empty()) {
    const uintptr_t source = reinterpret_cast<uintptr_t>(vertices.data());
    const uintptr_t destination = reinterpret_cast<uintptr_t>(out.data());
    if (source >= destination && source - destination < out.size()) return false;
  }
  switch (opcode & 0xf8) {
  case GX_TRIANGLES: if (count % 3 != 0) return false; break;
  case GX_QUADS: if (count % 4 != 0) return false; break;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN: if (count < 3) return false; break;
  case GX_LINES: if (count % 2 != 0) return false; break;
  case GX_LINESTRIP: if (count < 2) return false; break;
  case GX_POINTS: break;
  default: return false;
  }
  const auto readIndex = [&](size_t at) {
    return size_t(vertices[at]) << 8 | size_t(vertices[at + 1]);
  };
  for (size_t i = 0; i < count; ++i) {
    if (readIndex(i * stride) > kMaxVertices - layout.positionBase ||
        (normals && readIndex(i * stride + layout.normalAt) > kMaxVertices - layout.normalBase)) {
      return false;
    }
  }
  std::vector<size_t> order;
  uint8_t outputOpcode = opcode;
  if (mirrored && !PrimitiveOrder(opcode, count, true, outputOpcode, order)) return false;
  const size_t outputCount = mirrored ? order.size() : count;
  const size_t primitiveCount = (outputCount + kMaxVertices - 1) / kMaxVertices;
  const size_t headerBytes = primitiveCount * 3;
  if (out.max_size() - out.size() < headerBytes ||
      outputCount > (out.max_size() - out.size() - headerBytes) / stride) return false;
  for (size_t first = 0; first < outputCount;) {
    // Reflected fans/strips become lists. 65535 is a whole number of triangles,
    // so splitting here preserves connectivity even for the largest GX input.
    const size_t chunk = std::min(outputCount - first, kMaxVertices);
    out.push_back(outputOpcode);
    out.push_back(uint8_t(chunk >> 8));
    out.push_back(uint8_t(chunk));
    for (size_t i = 0; i < chunk; ++i) {
      const size_t source = (mirrored ? order[first + i] : first + i) * stride;
      const size_t target = out.size();
      out.insert(out.end(), vertices.begin() + source, vertices.begin() + source + stride);
      const auto writeIndex = [&](size_t at, size_t base) {
        const size_t index = readIndex(source + at) + base;
        out[target + at] = uint8_t(index >> 8);
        out[target + at + 1] = uint8_t(index);
      };
      writeIndex(0, layout.positionBase);
      if (normals) writeIndex(layout.normalAt, layout.normalBase);
    }
    first += chunk;
  }
  return true;
}

} // namespace PortRoomGeometryHandedness
