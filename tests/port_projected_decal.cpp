#include "port_projected_decal.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace PortProjectedDecal;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static bool Near(Vec3 a, Vec3 b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }

// A triangle whose stream-out GS normal ((v2 - v1) x (v0 - v1), unmirrored) is t x s.
static void Facing(Vec3 c, Vec3 t, Vec3 s, Vec3& v0, Vec3& v1, Vec3& v2) {
  v0 = c;
  v1 = c + t * 0.1f;
  v2 = c + s * 0.1f;
}

int main() {
  // IsNormalizable's threshold constant.
  const float minMag = kMinNormalizableMagSq;
  uint32_t bits;
  std::memcpy(&bits, &minMag, 4);
  REQUIRE(bits == 0x10fd87b5u);

  // LookAt: a floor normal is exactly parallel to up; the fallbacks give a proper basis.
  const Basis floor = LookAt({0.f, 0.f, 1.f});
  REQUIRE(Near(floor.x, {1.f, 0.f, 0.f}) && Near(floor.y, {0.f, 1.f, 0.f}) && Near(floor.z, {0.f, 0.f, 1.f}));
  // A wall: up survives.
  const Basis wall = LookAt({0.f, 1.f, 0.f});
  REQUIRE(Near(wall.x, {-1.f, 0.f, 0.f}) && Near(wall.y, {0.f, 0.f, 1.f}) && Near(wall.z, {0.f, 1.f, 0.f}));
  // A tilted normal: orthonormal, right-handed, z the normal, y leaning up.
  const Basis tilt = LookAt({1.f, 0.f, 1.f});
  REQUIRE(Near(tilt.z, {0.70710678f, 0.f, 0.70710678f}));
  REQUIRE(Near(Dot(tilt.x, tilt.y), 0.f) && Near(Dot(tilt.y, tilt.z), 0.f) && Near(Dot(tilt.x, tilt.z), 0.f));
  REQUIRE(Near(Dot(tilt.x, tilt.x), 1.f) && Near(Dot(tilt.y, tilt.y), 1.f));
  REQUIRE(Near(Cross(tilt.x, tilt.y), tilt.z));
  REQUIRE(tilt.y.z > 0.f);
  // A zero normal falls back to forward.
  REQUIRE(Near(LookAt({0.f, 0.f, 0.f}).z, {0.f, 1.f, 0.f}));

  // A floor decal: 2 x 4 m, 1 m deep.
  const Box box{{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}}, {0.f, 0.f, 0.f}, {1.f, 2.f, kBoxDepth}};
  const Vec3 outward[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  const float extent[6] = {0.5f, 0.5f, 1.f, 1.f, 2.f, 2.f};
  for (int i = 0; i < 6; ++i) {
    const Face f = GetFace(box, i);
    REQUIRE(Near(f.plane.n, outward[i]));
    REQUIRE(Near(f.plane.d, extent[i]));
  }

  const Command cmd = MakeShotCommand(box);
  REQUIRE(Near(cmd.projection.n, {0.f, 0.f, 1.f}) && Near(cmd.projection.d, -0.5f));
  REQUIRE(Near(cmd.origin, {1.f, -2.f, -0.5f}));
  REQUIRE(Near(cmd.u, {-0.5f, 0.f, 0.f}) && Near(cmd.v, {0.f, 0.25f, 0.f}));
  REQUIRE(Near(cmd.lo, {-1.f, -2.f, -0.5f}) && Near(cmd.hi, {1.f, 2.f, 0.5f}));
  // Clip planes: faces 4, 3, 5, 2, 0, 1 turned inward.
  const int order[6] = {4, 3, 5, 2, 0, 1};
  for (int i = 0; i < 6; ++i) {
    REQUIRE(Near(cmd.clip[i].n, outward[order[i]] * -1.f));
    REQUIRE(Distance(cmd.clip[i], {0.f, 0.f, 0.f}) > 0.f);
    REQUIRE(Distance(cmd.clip[i], outward[order[i]] * 3.f) < 0.f);
  }
  // UVs run 0..1 across the box.
  const Vertex c0 = MakeVertex(cmd, {1.f, -2.f, 0.f});
  const Vertex c1 = MakeVertex(cmd, {-1.f, -2.f, 0.f});
  const Vertex c2 = MakeVertex(cmd, {1.f, 2.f, 0.f});
  const Vertex mid = MakeVertex(cmd, {0.f, 0.f, 0.3f});
  REQUIRE(Near(c0.u, 0.f) && Near(c0.v, 0.f));
  REQUIRE(Near(c1.u, 1.f) && Near(c1.v, 0.f));
  REQUIRE(Near(c2.u, 0.f) && Near(c2.v, 1.f));
  REQUIRE(Near(mid.u, 0.5f) && Near(mid.v, 0.5f));

  const float lim = NormalLimitCos();
  REQUIRE(Near(lim, -0.0523360f));

  // A small floor triangle, counter-clockwise from above like the receiver lists' front faces:
  // kept whole, emitted in its own order.
  {
    Mesh m;
    const Vec3 v0{0.f, 0.f, 0.f}, v1{0.1f, 0.f, 0.f}, v2{0.f, 0.1f, 0.f};
    REQUIRE(AddTriangle(cmd, v0, v1, v2, false, lim, m) == Result::Kept);
    REQUIRE(m.Triangles() == 1);
    REQUIRE(Near(m.vertices[0].pos, v0) && Near(m.vertices[1].pos, v1) && Near(m.vertices[2].pos, v2));
    // The same triangle the other way round faces down.
    Mesh n;
    REQUIRE(AddTriangle(cmd, v0, v2, v1, false, lim, n) == Result::Facing);
    REQUIRE(n.Triangles() == 0);
    // Under a mirroring model the swap flips it back.
    REQUIRE(AddTriangle(cmd, v0, v2, v1, true, lim, n) == Result::Kept);
    REQUIRE(n.Triangles() == 1);
    REQUIRE(Near(n.vertices[0].pos, v0) && Near(n.vertices[1].pos, v1) && Near(n.vertices[2].pos, v2));
  }
  // The 93 degree limit.
  {
    const float deg = 3.14159265f / 180.f;
    for (float a : {92.f, 94.f}) {
      Vec3 v0, v1, v2;
      const float r = a * deg;
      Facing({0.f, 0.f, 0.f}, {std::cos(r), 0.f, -std::sin(r)}, {0.f, 1.f, 0.f}, v0, v1, v2);
      Mesh m;
      REQUIRE(AddTriangle(cmd, v0, v1, v2, false, lim, m) == (a < 93.f ? Result::Kept : Result::Facing));
    }
  }
  // Outside the box, and degenerate.
  {
    Mesh m;
    REQUIRE(AddTriangle(cmd, {5.f, 0.f, 0.f}, {5.1f, 0.f, 0.f}, {5.f, 0.1f, 0.f}, false, lim, m) == Result::Outside);
    REQUIRE(AddTriangle(cmd, {0.f, 0.f, 0.f}, {0.1f, 0.f, 0.f}, {0.2f, 0.f, 0.f}, false, lim, m) == Result::Outside);
    REQUIRE(m.Triangles() == 0);
  }
  // A floor triangle larger than the box: clipped to the 2 x 4 rectangle, UVs within 0..1.
  {
    Mesh m;
    REQUIRE(AddTriangle(cmd, {-3.f, -5.f, 0.f}, {10.f, -5.f, 0.f}, {-3.f, 10.f, 0.f}, false, lim, m) ==
            Result::Clipped);
    REQUIRE(m.Triangles() == 2);
    float area = 0.f;
    for (size_t t = 0; t < m.Triangles(); ++t) {
      const Vertex* v = &m.vertices[t * 3];
      const Vec3 n = Cross(v[1].pos - v[0].pos, v[2].pos - v[0].pos);
      REQUIRE(n.z > 0.f); // emitted counter-clockwise from above, like the kept triangle
      area += 0.5f * std::sqrt(Dot(n, n));
      for (int k = 0; k < 3; ++k) {
        for (const Plane& p : cmd.clip) {
          REQUIRE(Distance(p, v[k].pos) > -1e-4f);
        }
        REQUIRE(v[k].u > -1e-4f && v[k].u < 1.f + 1e-4f && v[k].v > -1e-4f && v[k].v < 1.f + 1e-4f);
      }
    }
    REQUIRE(Near(area, 8.f));
  }
  // A triangle crossing the box's top: cut by the depth planes.
  {
    Mesh m;
    Vec3 v0, v1, v2;
    Facing({0.f, 0.f, -0.2f}, {1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}, v0, v1, v2); // a ceiling: away
    REQUIRE(AddTriangle(cmd, v0, v1, v2, false, lim, m) == Result::Facing);
    Facing({0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {0.f, 1.f, 3.f}, v0, v1, v2); // tilted up, 0.3 m tall
    REQUIRE(AddTriangle(cmd, v0, v1, v2, false, lim, m) == Result::Kept);
    Facing({0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {0.f, 3.f, 9.f}, v0, v1, v2); // 0.9 m tall: cut at 0.5
    Mesh cut;
    REQUIRE(AddTriangle(cmd, v0, v1, v2, false, lim, cut) == Result::Clipped);
    for (const Vertex& v : cut.vertices) {
      REQUIRE(v.pos.z < 0.5f + 1e-4f && v.pos.x < 1.f + 1e-4f);
    }
  }

  // Display lists: big-endian indices, the position first.
  {
    const uint8_t list[] = {
        0x90, 0, 3, 0, 1, 0, 2, 0, 3,       // triangles
        0x98, 0, 4, 0, 4, 0, 5, 0, 6, 0, 7, // strip
        0xa0, 0, 4, 0, 8, 0, 9, 0, 10, 0, 11, // fan
        0x80, 0, 4, 1, 0, 1, 1, 1, 2, 1, 3, // quads
        0, 0, 0,
    };
    std::vector<uint32_t> got;
    REQUIRE(ForEachTriangle(list, sizeof(list), 2, [&](uint32_t a, uint32_t b, uint32_t c) {
      got.insert(got.end(), {a, b, c});
    }));
    const std::vector<uint32_t> want = {1, 2, 3, 4, 5, 6, 6, 5, 7, 8, 9, 10, 8, 10, 11,
                                        256, 257, 258, 256, 258, 259};
    REQUIRE(got == want);
    // A wider vertex: the other attribute indices are skipped.
    const uint8_t wide[] = {0x90, 0, 3, 0, 1, 0xff, 0xff, 0, 2, 0xff, 0xff, 0, 3, 0xff, 0xff};
    got.clear();
    REQUIRE(ForEachTriangle(wide, sizeof(wide), 4, [&](uint32_t a, uint32_t b, uint32_t c) {
      got.insert(got.end(), {a, b, c});
    }));
    REQUIRE((got == std::vector<uint32_t>{1, 2, 3}));
    // An overrun and an unknown primitive fail.
    const uint8_t over[] = {0x90, 0, 6, 0, 1, 0, 2, 0, 3};
    REQUIRE(!ForEachTriangle(over, sizeof(over), 2, [](uint32_t, uint32_t, uint32_t) {}));
    const uint8_t lines[] = {0xa8, 0, 2, 0, 1, 0, 2};
    REQUIRE(!ForEachTriangle(lines, sizeof(lines), 2, [](uint32_t, uint32_t, uint32_t) {}));
  }
  return PORT_TEST_RESULT();
}
