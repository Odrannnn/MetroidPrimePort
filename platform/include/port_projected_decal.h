#pragma once

// Remastered's projected shot decals (kb topic/projected-decals.md): the pure math. A decal is an
// oriented box; the receiver triangles inside it are kept, clipped to it and given box UVs, once,
// when the decal is first rendered (CDecalRenderGen::GenerateDecalGeometry 0xb37444, the
// stream-out pass). The game half is platform/port_projected_decal.cpp.
//
// Remastered streams each accepted triangle out whole, with its six box distances as clip
// distances, and lets the rasterizer clip it when the decal draws. Here the clip happens on the
// CPU (Sutherland-Hodgman on the same six planes), which covers the same pixels: clip distances
// are linear across a triangle, and so are the UVs, which are recomputed at each new vertex.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

class CDecal;
class CStateManager;

namespace PortProjectedDecal {

struct Vec3 {
  float x, y, z;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// skShotDecalPlaneNormalThreshold 0x1d0e0a0, in degrees: a receiver triangle facing further from
// the decal's normal than this is dropped. initialize_decal_stream_out_mtrl 0xed500 stores its cosine.
constexpr float kNormalLimitDegrees = 93.f;
inline float NormalLimitCos() { return std::cos(kNormalLimitDegrees * (3.14159265f / 180.f)); }
// COBBox extents of a shot decal's depth (0x1d0e0b0).
constexpr float kBoxDepth = 0.5f;

// CTransform4f::LookAt 0x6ead8 (Remastered's, not retail's): forward is local Z, not Y. Columns
// x, y, z of the rotation; Explode 0x10121cc calls it with the hit normal and skUpVector.
// Remastered is Y-up: skUpVector 0x1d0c20c = (0,1,0), skForwardVector 0x1d0c23c = (0,0,1), and the
// fallbacks subtract f.y and f.z, their dots with f. The port's (-x,z,y) is a rotation, so the same
// steps run here on the mapped constants: up (0,0,1), forward (0,1,0).
struct Basis {
  Vec3 x, y, z;
};
// CVector3f::IsNormalizable 0x6b030: every component finite and the squared length at least this
// (0x10fd87b5).
constexpr float kMinNormalizableMagSq = 9.9999994e-29f;
inline Basis LookAt(Vec3 forward, Vec3 up = {0.f, 0.f, 1.f}, float eps = 1.9073486e-06f) {
  Vec3 f = forward;
  const float fl2 = Dot(f, f);
  const bool normalizable = std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && fl2 >= kMinNormalizableMagSq;
  f = normalizable ? f * (1.f / std::sqrt(fl2)) : Vec3{0.f, 1.f, 0.f};
  float d = Dot(up, f);
  d = d < -1.f ? -1.f : d > 1.f ? 1.f : d;
  Vec3 u = up - f * d;
  if (std::sqrt(Dot(u, u)) <= eps) {
    u = Vec3{0.f, 0.f, 1.f} - f * f.z;
    if (std::sqrt(Dot(u, u)) <= eps) {
      u = Vec3{0.f, 1.f, 0.f} - f * f.y;
    }
  }
  u = u * (1.f / std::sqrt(Dot(u, u)));
  return {Cross(u, f), u, f};
}

// A COBBox: rotation columns, centre, half extents.
struct Box {
  Basis axes;
  Vec3 center;
  Vec3 extents;
};
inline Vec3 Corner(const Box& b, float sx, float sy, float sz) {
  return b.center + b.axes.x * (sx * b.extents.x) + b.axes.y * (sy * b.extents.y) + b.axes.z * (sz * b.extents.z);
}

// A plane as the shaders read it: inside when Distance >= 0.
struct Plane {
  Vec3 n;
  float d;
};
inline float Distance(const Plane& p, Vec3 v) { return Dot(p.n, v) - p.d; }

// COBBox::GetFace 0x95704: the face's four corners (signs of the half extents, jump table at
// 0x1d0c854) and CPlane(P0, P1, P2) 0x97634 through the first three, normal outward.
struct Face {
  Plane plane;
  Vec3 corner[4];
};
inline Face GetFace(const Box& b, int face) {
  static const signed char kSigns[6][4][3] = {
      {{1, 1, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}}, // -Z
      {{-1, 1, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}},     // +Z
      {{1, 1, 1}, {1, -1, 1}, {1, -1, -1}, {1, 1, -1}},     // +X
      {{-1, 1, -1}, {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}}, // -X
      {{1, 1, 1}, {1, 1, -1}, {-1, 1, -1}, {-1, 1, 1}},     // +Y
      {{1, -1, -1}, {1, -1, 1}, {-1, -1, 1}, {-1, -1, -1}}, // -Y
  };
  Face f;
  for (int i = 0; i < 4; ++i) {
    f.corner[i] = Corner(b, kSigns[face][i][0], kSigns[face][i][1], kSigns[face][i][2]);
  }
  Vec3 n = Cross(f.corner[1] - f.corner[0], f.corner[2] - f.corner[0]);
  n = n * (1.f / std::sqrt(Dot(n, n)));
  f.plane = {n, Dot(n, f.corner[0])};
  return f;
}
inline Plane Negated(const Plane& p) { return {p.n * -1.f, -p.d}; }

// NRenderDecal::CDecalStreamOutCommand as initialize_decal_stream_out_cmd 0xed0a0 fills it.
struct Command {
  Plane clip[6];    // +0x70..: the box's faces, negated (inside >= 0)
  Plane projection; // +0xd0: the projection face, negated; its normal is the facing test's
  Vec3 origin;      // +0xe0
  Vec3 u, v;        // +0xf0, +0x100: an edge over its squared length, so UVs run 0..1 across the box
  Vec3 lo, hi;      // +0x18: the box's world bounds, the receivers' pre-test
};
inline Vec3 OverLengthSq(Vec3 e) {
  const float l = Dot(e, e);
  return l > 0.001f ? e * (1.f / l) : e;
}
// Shot decals use axis 1 (AddCollisionDecal 0xb3af5c): face 0 (-Z) projects, so the facing
// normal is local +Z, the surface's; origin is its corner 1, U runs to corner 2, V to corner 0.
// Axis 0 (face 5 projects) is not used by shot decals.
inline Command MakeShotCommand(const Box& b) {
  Command c;
  const Face p = GetFace(b, 0);
  c.projection = Negated(p.plane);
  c.origin = p.corner[1];
  c.u = OverLengthSq(p.corner[2] - p.corner[1]);
  c.v = OverLengthSq(p.corner[0] - p.corner[1]);
  static const int kClipFaces[6] = {4, 3, 5, 2, 0, 1};
  for (int i = 0; i < 6; ++i) {
    c.clip[i] = Negated(GetFace(b, kClipFaces[i]).plane);
  }
  c.lo = c.hi = Corner(b, -1, -1, -1);
  for (int i = 1; i < 8; ++i) {
    const Vec3 k = Corner(b, (i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : -1.f);
    c.lo = {std::fmin(c.lo.x, k.x), std::fmin(c.lo.y, k.y), std::fmin(c.lo.z, k.z)};
    c.hi = {std::fmax(c.hi.x, k.x), std::fmax(c.hi.y, k.y), std::fmax(c.hi.z, k.z)};
  }
  return c;
}

struct Vertex {
  Vec3 pos;      // world
  float u, v;    // box UV, 0..1
};
// A decal's clipped receiver triangles: a triangle list in the order the stream-out pass emits.
struct Mesh {
  std::vector<Vertex> vertices;
  size_t Triangles() const { return vertices.size() / 3; }
};

inline Vertex MakeVertex(const Command& c, Vec3 p) {
  const Vec3 r = p - c.origin;
  return {p, Dot(c.u, r), Dot(c.v, r)};
}

enum class Result { Kept, Facing, Outside, Clipped };

// The model stream-out GS (000ec67, cmd planes in cb1; terrain's 0035347 does the same tests) on one
// world-space triangle as the receiver lists it, then the clip. The GS takes r0 = v2, r1 = v1 when
// det(modelToWorld) >= 0 and the reverse otherwise (`mirrored` is det < 0), normal cross(r0 - r1, v0 - r1),
// so an unmirrored triangle keeps its CCW normal and order. Kept and Clipped append triangles to `out`.
inline Result AddTriangle(const Command& c, Vec3 v0, Vec3 v1, Vec3 v2, bool mirrored, float cosLimit, Mesh& out) {
  const Vec3 r0 = mirrored ? v1 : v2;
  const Vec3 r1 = mirrored ? v2 : v1;
  const Vec3 n = Cross(r0 - r1, v0 - r1);
  const float nl = Dot(n, n);
  // A zero-area triangle covers no pixel (the GS's normalize would give NaN and keep it).
  if (!(nl > 0.f)) {
    return Result::Outside;
  }
  if (Dot(n * (1.f / std::sqrt(nl)), c.projection.n) < cosLimit) {
    return Result::Facing;
  }
  for (const Plane& p : c.clip) {
    if (Distance(p, v0) < 0.f && Distance(p, v1) < 0.f && Distance(p, v2) < 0.f) {
      return Result::Outside;
    }
  }
  // Emitted v0, r1, r0; clipped as a polygon in that order and fanned from its first vertex.
  Vec3 poly[9] = {v0, r1, r0};
  Vec3 next[9];
  int count = 3;
  bool cut = false;
  for (const Plane& p : c.clip) {
    int kept = 0;
    for (int i = 0; i < count; ++i) {
      const Vec3 a = poly[i];
      const Vec3 b = poly[(i + 1) % count];
      const float da = Distance(p, a);
      const float db = Distance(p, b);
      if (da >= 0.f) {
        next[kept++] = a;
      }
      if ((da >= 0.f) != (db >= 0.f)) {
        next[kept++] = a + (b - a) * (da / (da - db));
        cut = true;
      }
    }
    count = kept;
    for (int i = 0; i < count; ++i) {
      poly[i] = next[i];
    }
    if (count < 3) {
      return Result::Outside;
    }
  }
  for (int i = 1; i + 1 < count; ++i) {
    out.vertices.push_back(MakeVertex(c, poly[0]));
    out.vertices.push_back(MakeVertex(c, poly[i]));
    out.vertices.push_back(MakeVertex(c, poly[i + 1]));
  }
  return cut ? Result::Clipped : Result::Kept;
}

// The triangles of one GX display list (big-endian u16 indices, `stride` bytes a vertex, the
// position index first), as GX draws them: lists, strips (odd triangles swap their first two) and
// fans. Calls fn(i0, i1, i2) with position indices. False on a primitive it does not know.
template <class Fn>
bool ForEachTriangle(const uint8_t* list, size_t size, size_t stride, Fn&& fn) {
  const auto index = [&](size_t at) { return uint32_t(list[at] << 8 | list[at + 1]); };
  size_t at = 0;
  while (at + 3 <= size) {
    const uint8_t type = list[at] & 0xf8;
    if (type == 0) {
      return true;
    }
    const size_t count = index(at + 1);
    at += 3;
    if (stride < 2 || at + count * stride > size) {
      return false;
    }
    const auto vtx = [&](size_t i) { return index(at + i * stride); };
    switch (type) {
    case 0x80: // GX_QUADS
      for (size_t i = 0; i + 3 < count; i += 4) {
        fn(vtx(i), vtx(i + 1), vtx(i + 2));
        fn(vtx(i), vtx(i + 2), vtx(i + 3));
      }
      break;
    case 0x90: // GX_TRIANGLES
      for (size_t i = 0; i + 2 < count; i += 3) fn(vtx(i), vtx(i + 1), vtx(i + 2));
      break;
    case 0x98: // GX_TRIANGLESTRIP
      for (size_t i = 0; i + 2 < count; ++i) {
        if (i & 1) fn(vtx(i + 1), vtx(i), vtx(i + 2));
        else fn(vtx(i), vtx(i + 1), vtx(i + 2));
      }
      break;
    case 0xa0: // GX_TRIANGLEFAN
      for (size_t i = 1; i + 1 < count; ++i) fn(vtx(0), vtx(i), vtx(i + 1));
      break;
    default:
      return false;
    }
    at += count * stride;
  }
  return true;
}

// Game side (platform/port_projected_decal.cpp), per CDecalManager pool slot (64).
// A Remastered decal took the slot: its mesh is built at the next Build.
void Spawn(int slot, const CDecal& decal, uint32_t asset);
void Release(int slot);
void ReleaseAll();
// Builds the meshes of the decals spawned since the last call against this frame's receivers (the
// room geometry drawn, PortRoomGeo::ForEachShadowReceiver). Called once the opaque world is in,
// where RenderPrimaryPass runs GenerateDecalGeometry.
void Build();
// The slot's clipped mesh, or null when it has none (a retail decal, or not built yet).
const Mesh* MeshFor(int slot);

} // namespace PortProjectedDecal
