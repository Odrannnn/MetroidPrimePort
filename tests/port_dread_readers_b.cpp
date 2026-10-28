// Dread BCMDL / BSMAT readers (platform/port_dread_bcmdl.cpp, port_dread_bsmat.cpp).
//
// The fixtures are synthetic: built here, byte by byte, from the layout notes. The real-data
// checks run only when PORT_DREAD_SAMUS_PKG_DIR names a directory holding the user's extracted
// Samus pkg files (build/dread/samus_pkg) and print that they skipped otherwise.
#include "port_dread_bcmdl.h"
#include "port_dread_bsmat.h"
#include "port_test.h"

#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace PortDread;

namespace {

constexpr float kPi = 3.14159265358979f;

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool Near3(const float* a, float x, float y, float z, float eps = 1e-4f) {
  return Near(a[0], x, eps) && Near(a[1], y, eps) && Near(a[2], z, eps);
}

// Little-endian byte builder.
struct Buf {
  std::vector<uint8_t> b;
  size_t pos() const { return b.size(); }
  void align(size_t n) {
    while (b.size() % n != 0) {
      b.push_back(0);
    }
  }
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(v & 0xFF), u8(v >> 8); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      u8(uint8_t(v >> (8 * i)));
    }
  }
  void i32(int32_t v) { u32(uint32_t(v)); }
  void u64(uint64_t v) {
    u32(uint32_t(v));
    u32(uint32_t(v >> 32));
  }
  void f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
  }
  void str(const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
  }
  void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
  void pad(size_t n) { b.insert(b.end(), n, 0); }
  void patch64(size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      b[off + i] = uint8_t(v >> (8 * i));
    }
  }
};

std::vector<uint8_t> Gzip(const std::vector<uint8_t>& raw) {
  z_stream zs{};
  deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> out(deflateBound(&zs, uLong(raw.size())) + 64);
  zs.next_in = const_cast<Bytef*>(raw.data());
  zs.avail_in = uInt(raw.size());
  zs.next_out = out.data();
  zs.avail_out = uInt(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

struct MmdlOpts {
  uint32_t skin_type = 2;
  bool gzip_vertices = true;
  bool gzip_indices = false; // raw index buffer exercises the non-gzip path
  uint32_t first_index = 0;
  uint32_t index_count = 6;
  uint16_t bad_index = 0xFFFF; // replaces the last index when != 0xFFFF
  const char* child_parent = "Root";
  uint32_t jmap0 = 1, jmap1 = 0;
};

// 2 joints (Root, Child under Root), 1 mesh "Quad" (4 vertices, 2 triangles), 1 submesh.
// Vertices (pos, weights): v0 = skinned between jmap[0]/jmap[1] with 0.25/0.75, others rigid.
std::vector<uint8_t> BuildMmdl(const MmdlOpts& o = {}) {
  Buf f;
  f.b.assign(0x58, 0);
  std::memcpy(&f.b[0], "MMDL", 4);
  f.b[4] = 0x01, f.b[5] = 0x00, f.b[6] = 0x3A, f.b[7] = 0x00;

  // Joints.
  const size_t tr0 = f.pos();
  for (float v : {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f}) f.f32(v);
  const size_t tr1 = f.pos();
  for (float v : {10.f, 0.f, 0.f, kPi / 2, 0.f, kPi / 2, 1.f, 1.f, 1.f}) f.f32(v);
  f.align(8);
  const size_t n_root = f.pos();
  f.str("Root");
  const size_t n_child = f.pos();
  f.str("Child");
  const size_t n_parent = f.pos();
  f.str(o.child_parent);
  f.align(8);
  const size_t je0 = f.pos();
  f.u64(tr0), f.u64(n_root), f.u64(0), f.u64(0);
  const size_t je1 = f.pos();
  f.u64(tr1), f.u64(n_child), f.u64(n_parent), f.u64(0);
  const size_t jlist = f.pos();
  f.u64(je0), f.u64(jlist + 16);
  f.u64(je1), f.u64(0);
  const size_t jhdr = f.pos();
  f.u32(2), f.i32(-1), f.u64(jlist), f.u64(0);

  // Mesh strings and entries.
  const size_t s_mesh = f.pos();
  f.str("Quad");
  const size_t s_mat = f.pos();
  f.str("quadmat");
  const size_t s_path = f.pos();
  f.str("actors/test/quadmat.bsmat");
  f.align(8);
  const size_t name_entry = f.pos();
  f.u64(s_mesh), f.u8(1), f.pad(7);
  const size_t mat_entry = f.pos();
  f.u64(s_mat), f.u64(s_path), f.pad(16);

  // Joint map and submesh.
  const size_t jmap = f.pos();
  f.u32(o.jmap0), f.u32(o.jmap1);
  const size_t sub = f.pos();
  f.u32(o.skin_type), f.u32(o.first_index), f.u32(o.index_count), f.u32(2), f.u64(jmap);
  const size_t sublist = f.pos();
  f.u64(sub), f.u64(0);

  // Index buffer.
  std::vector<uint8_t> idx;
  {
    Buf ib;
    const uint16_t ids[6] = {0, 1, 2, 2, 3, 0};
    for (int i = 0; i < 6; ++i) {
      ib.u16(i == 5 && o.bad_index != 0xFFFF ? o.bad_index : ids[i]);
    }
    idx = ib.b;
  }
  const std::vector<uint8_t> idx_stored = o.gzip_indices ? Gzip(idx) : idx;
  const size_t idata = f.pos();
  f.bytes(idx_stored);
  f.align(8);
  const size_t ibi = f.pos();
  f.pad(8), f.u16(2), f.u16(1), f.u32(6), f.u32(uint32_t(idx_stored.size())), f.i32(-1), f.u64(idata);

  // Vertex info (data pointer patched once the buffer is placed).
  // Planar: pos(0) 48 B @0, normal(1) 48 @48, uv0(2) 32 @96, joints(6) 64 @128, weights(7) 64 @192,
  // tangent(8) 64 @256 => 320 bytes.
  Buf vb;
  const float pos[4][3] = {{-1, -1, -3}, {1, 0, 0}, {0, 2, 0}, {0, 0, 3}};
  for (auto& p : pos) vb.f32(p[0]), vb.f32(p[1]), vb.f32(p[2]);
  for (int i = 0; i < 4; ++i) vb.f32(0), vb.f32(0), vb.f32(1);          // normals
  for (int i = 0; i < 4; ++i) vb.f32(0.25f * float(i)), vb.f32(1.f - 0.25f * float(i)); // uv0
  for (int i = 0; i < 4; ++i) {                                          // joint indices (floats)
    vb.f32(i == 0 ? 0.f : 1.f), vb.f32(i == 0 ? 1.f : 0.f), vb.f32(0), vb.f32(0);
  }
  for (int i = 0; i < 4; ++i) {                                          // weights
    if (i == 0) vb.f32(0.25f), vb.f32(0.75f), vb.f32(0), vb.f32(0);
    else vb.f32(1.f), vb.f32(0), vb.f32(0), vb.f32(0);
  }
  for (int i = 0; i < 4; ++i) vb.f32(1), vb.f32(0), vb.f32(0), vb.f32(i & 1 ? -1.f : 1.f); // tangent
  const std::vector<uint8_t> vstored = o.gzip_vertices ? Gzip(vb.b) : vb.b;

  f.align(8);
  const size_t vbi = f.pos();
  f.pad(12), f.u32(uint32_t(vb.b.size())), f.u32(4), f.u32(uint32_t(vstored.size()));
  const size_t vdata_ptr_at = f.pos();
  f.u64(0);
  f.u32(6), f.i32(-1);
  const struct {
    uint32_t sem, off, comps;
  } attrs[6] = {{0, 0, 3}, {1, 48, 3}, {2, 96, 2}, {6, 128, 4}, {7, 192, 4}, {8, 256, 4}};
  for (auto& a : attrs) f.u32(a.sem), f.u32(a.off), f.u16(3), f.u16(uint16_t(a.comps)), f.u32(0);

  // Mesh spec.
  f.align(8);
  const size_t spec = f.pos();
  f.pad(0x50);
  f.u64(ibi), f.u64(vbi), f.u32(1), f.i32(-1), f.u64(sublist);
  f.f32(1.f), f.f32(2.f), f.f32(3.f);
  f.align(8);
  const size_t mentry = f.pos();
  f.u64(spec), f.u64(mat_entry), f.u64(name_entry);
  const size_t mtable = f.pos();
  f.u64(mentry), f.u64(0);

  // The vertex buffer goes last: any truncation of the file cuts it.
  f.align(8);
  f.patch64(vdata_ptr_at, f.pos());
  f.bytes(vstored);

  f.patch64(8 + 8 * 4, mtable);
  f.patch64(8 + 8 * 7, jhdr);
  return f.b;
}

// ---- BSMAT builder ----
void Uniform(Buf& b, const char* name, char type, const std::vector<uint32_t>& raw) {
  b.str(name);
  b.u8(uint8_t(type));
  b.u32(uint32_t(raw.size()));
  for (uint32_t v : raw) b.u32(v);
}
uint32_t FBits(float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  return u;
}
void Sampler(Buf& b, const char* name, const char* sampler, uint32_t index, const char* path,
             uint32_t wrap_u, uint32_t wrap_v) {
  b.str(name), b.str(sampler), b.str("texture2D"), b.u32(index), b.str(path);
  b.u32(1), b.u32(1), b.u32(5), b.u32(0), b.u32(wrap_u), b.u32(wrap_v);
  b.u8(0xFF), b.u8(0), b.u8(0), b.u8(0xFF);
  b.f32(0), b.f32(-1.5f), b.f32(1), b.f32(15), b.f32(16);
}

std::vector<uint8_t> BuildBsmat() {
  Buf b;
  b.bytes({'M', 'S', 'U', 'R', 0x02, 0x00, 0x11, 0x00});
  b.str("synthmat");
  b.u32(8);   // translucency type
  b.u32(31);  // render layer
  b.str("system/shd/synth.bshdat");
  b.u8(1), b.u32(0), b.u32(6), b.u32(7);   // blend: enabled, ADD, SRC_ALPHA, ONE_MINUS_SRC_ALPHA
  b.u32(3);                                // cull FRONT
  b.u8(0), b.u32(0xFFFFFF), b.u32(0xFFFFFFFF), b.u32(1), b.u32(2), b.u32(3), b.u32(4), b.u32(5);
  b.u8(1), b.u32(5), b.f32(0.5f);          // alpha test
  b.u32(0);                                // fill mode
  b.u8(1), b.u8(0), b.u32(5), b.u8(1);     // depth test on, write off, mode, z prepass
  b.u32(0), b.u32(2), b.u32(0);
  b.u32(1);                                // extra uniforms
  Uniform(b, "iExtra", 'i', {uint32_t(-5), 9});
  b.u32(0);                                // extra samplers
  b.u32(2);                                // stages
  b.u32(3);                                // stage 0 uniforms
  Uniform(b, "vColor", 'f', {FBits(0.04f), FBits(1.f), FBits(0.01f), FBits(1.f)});
  Uniform(b, "fFactor", 'f', {FBits(50.f)});
  Uniform(b, "uMask", 'u', {0xDEADBEEFu});
  b.u32(2);                                // stage 0 samplers
  Sampler(b, "texBaseColor", "sampler0", 0, "actors/test/synth_bc.bctex", 2, 0);
  Sampler(b, "texNormals", "sampler1", 2, "", 1, 3);
  b.u32(0), b.u32(0);                      // stage 1: empty
  return b.b;
}

void TestSyntheticBcmdl() {
  for (uint32_t skin : {2u, 1u}) {
    MmdlOpts o;
    o.skin_type = skin;
    const auto file = BuildMmdl(o);
    Bcmdl m;
    std::string err;
    REQUIRE_MSG(ParseBcmdl(file.data(), file.size(), m, &err), err);
    CHECK_EQ(m.version_major, 1);
    CHECK_EQ(m.version_minor, 58);
    REQUIRE(m.joints.size() == size_t(2));
    CHECK_EQ(m.joints[0].name, std::string("Root"));
    CHECK_EQ(m.joints[0].parent, -1);
    CHECK_EQ(m.joints[1].name, std::string("Child"));
    CHECK_EQ(m.joints[1].parent, 0);
    CHECK(Near(m.joints[1].pos[0], 10.f) && Near(m.joints[1].euler[2], kPi / 2));
    CHECK_EQ(m.FindJoint("Child"), 1);
    CHECK_EQ(m.FindJoint("nope"), -1);

    REQUIRE(m.meshes.size() == size_t(1));
    const BcmdlMesh& mesh = m.meshes[0];
    CHECK_EQ(mesh.name, std::string("Quad"));
    CHECK(mesh.enabled);
    CHECK_EQ(mesh.material_name, std::string("quadmat"));
    CHECK_EQ(mesh.bsmat_path, std::string("actors/test/quadmat.bsmat"));
    CHECK(Near3(mesh.transform, 1, 2, 3));
    const BcmdlVertices& v = mesh.vertices;
    CHECK_EQ(v.count, 4u);
    REQUIRE(v.pos.size() == size_t(12));
    CHECK(Near3(&v.pos[0], -1, -1, -3) && Near3(&v.pos[9], 0, 0, 3));
    CHECK_EQ(v.normal.size(), size_t(12));
    CHECK(Near(v.normal[2], 1.f));
    CHECK_EQ(v.uv[0].size(), size_t(8));
    CHECK(Near(v.uv[0][6], 0.75f) && Near(v.uv[0][7], 0.25f));
    CHECK(v.uv[1].empty() && v.uv[2].empty() && v.color.empty());
    REQUIRE(v.joints.size() == size_t(16));
    CHECK(v.joints[0] == 0 && v.joints[1] == 1 && v.joints[4] == 1 && v.joints[5] == 0);
    REQUIRE(v.weights.size() == size_t(16));
    CHECK(Near(v.weights[0], 0.25f) && Near(v.weights[1], 0.75f) && Near(v.weights[4], 1.f));
    REQUIRE(v.tangent.size() == size_t(16));
    CHECK(Near(v.tangent[7], -1.f) && Near(v.tangent[3], 1.f));
    REQUIRE(mesh.indices.size() == size_t(6));
    CHECK(mesh.indices[0] == 0 && mesh.indices[2] == 2 && mesh.indices[5] == 0);
    REQUIRE(mesh.submeshes.size() == size_t(1));
    const BcmdlSubmesh& sm = mesh.submeshes[0];
    CHECK_EQ(sm.skin_type, skin);
    CHECK_EQ(sm.first_index, 0u);
    CHECK_EQ(sm.index_count, 6u);
    REQUIRE(sm.joint_map.size() == size_t(2));
    CHECK(sm.joint_map[0] == 1 && sm.joint_map[1] == 0);

    // World bind: Child = T(10,0,0) * Rz(90) * Rx(90). (0,1,0) -> Rx -> (0,0,1) -> Rz -> (0,0,1);
    // the reverse order would give (-1,0,0), so this pins R = Rz*Ry*Rx.
    std::vector<Mat4> world;
    REQUIRE_MSG(ComputeWorldBind(m, world, &err), err);
    REQUIRE(world.size() == size_t(2));
    float p[3];
    const float up[3] = {0, 1, 0};
    TransformPoint(world[0], up, p);
    CHECK(Near3(p, 0, 1, 0));
    TransformPoint(world[1], up, p);
    CHECK(Near3(p, 10, 0, 1));
    const float fwd[3] = {1, 0, 0};
    TransformPoint(world[1], fwd, p); // Rx leaves x, Rz(90): (1,0,0) -> (0,1,0)
    CHECK(Near3(p, 10, 1, 0));

    std::vector<float> bind;
    REQUIRE_MSG(BindPoseModelPositions(m, world, mesh, sm, bind, &err), err);
    REQUIRE(bind.size() == size_t(12));
    if (skin == 2) {
      // Already model space: pos + transform.
      CHECK(Near3(&bind[0], 0, 1, 0) && Near3(&bind[3], 2, 2, 3) && Near3(&bind[9], 1, 2, 6));
    } else {
      // v0 = 0.25 * Child * (0,1,0) + 0.75 * Root * (0,1,0) = (2.5, 0.75, 0.25);
      // v1 (weight 1 on jmap[1] = Root... joints[4]=1 -> jmap[1] = Root) = pos + transform;
      // v2 and v3 likewise.
      CHECK(Near3(&bind[0], 2.5f, 0.75f, 0.25f));
      CHECK(Near3(&bind[3], 2, 2, 3) && Near3(&bind[6], 1, 4, 3) && Near3(&bind[9], 1, 2, 6));
      float one[3];
      REQUIRE_MSG(BindPosePosition(m, world, mesh, sm, 0, one, &err), err);
      CHECK(Near3(one, 2.5f, 0.75f, 0.25f));
      CHECK(!BindPosePosition(m, world, mesh, sm, 4, one, &err));
    }
    BcmdlSubmesh bad = sm;
    bad.skin_type = 7;
    CHECK(!BindPoseModelPositions(m, world, mesh, bad, bind, &err));
  }

  // Gzip indices and raw vertices parse the same.
  {
    MmdlOpts o;
    o.gzip_indices = true;
    o.gzip_vertices = false;
    const auto file = BuildMmdl(o);
    Bcmdl m;
    std::string err;
    REQUIRE_MSG(ParseBcmdl(file.data(), file.size(), m, &err), err);
    CHECK_EQ(m.meshes[0].indices.size(), size_t(6));
    CHECK(Near(m.meshes[0].vertices.pos[9], 0.f) && Near(m.meshes[0].vertices.pos[11], 3.f));
  }
  // A second submesh range inside the buffer is fine.
  {
    MmdlOpts o;
    o.first_index = 3;
    o.index_count = 3;
    const auto file = BuildMmdl(o);
    Bcmdl m;
    CHECK(ParseBcmdl(file.data(), file.size(), m, nullptr));
    CHECK_EQ(m.meshes[0].submeshes[0].first_index, 3u);
  }
  // Euler order on a bare joint: Ry(90) maps +X to -Z.
  {
    BcmdlJoint j;
    j.euler[1] = kPi / 2;
    const float x[3] = {1, 0, 0};
    float p[3];
    TransformPoint(JointLocalMatrix(j), x, p);
    CHECK(Near3(p, 0, 0, -1));
  }
}

void TestBadBcmdl() {
  const auto good = BuildMmdl();
  Bcmdl m;
  std::string err;
  // Every truncation fails cleanly (the vertex buffer is the file's last bytes).
  size_t truncated_ok = 0;
  for (size_t n = 0; n < good.size(); ++n) {
    err.clear();
    if (ParseBcmdl(good.data(), n, m, &err)) {
      ++truncated_ok;
    } else {
      CHECK_MSG(!err.empty(), "failure without a message");
    }
  }
  CHECK_EQ(truncated_ok, size_t(0));

  auto mutated = [&](auto&& edit) {
    auto f = good;
    edit(f);
    err.clear();
    const bool ok = ParseBcmdl(f.data(), f.size(), m, &err);
    return std::make_pair(ok, err);
  };
  CHECK(!mutated([](auto& f) { f[0] = 'X'; }).first);                // magic
  CHECK(!mutated([](auto& f) { f[6] = 0x3B; }).first);               // version
  CHECK(!mutated([](auto& f) { f[8 + 8 * 7] = 0xFF, f[8 + 8 * 7 + 7] = 0x7F; }).first); // joint ptr
  CHECK(!ParseBcmdl(nullptr, 0, m, &err));
  {
    MmdlOpts o;
    o.child_parent = "Missing";
    const auto f = BuildMmdl(o);
    err.clear();
    CHECK(!ParseBcmdl(f.data(), f.size(), m, &err));
    CHECK_MSG(err.find("Missing") != std::string::npos, err);
  }
  {
    MmdlOpts o;
    o.bad_index = 4; // vertex count is 4
    const auto f = BuildMmdl(o);
    CHECK(!ParseBcmdl(f.data(), f.size(), m, &err));
  }
  {
    MmdlOpts o;
    o.first_index = 4;
    o.index_count = 4; // runs past the 6 indices
    const auto f = BuildMmdl(o);
    CHECK(!ParseBcmdl(f.data(), f.size(), m, &err));
  }
  {
    MmdlOpts o;
    o.jmap0 = 2; // only 2 joints
    const auto f = BuildMmdl(o);
    CHECK(!ParseBcmdl(f.data(), f.size(), m, &err));
  }
  // Corrupt the gzip stream (flip bytes in its body, past the 10 byte header).
  {
    auto f = good;
    for (size_t i = f.size() - 12; i < f.size() - 8; ++i) f[i] ^= 0x5A;
    CHECK(!ParseBcmdl(f.data(), f.size(), m, &err));
  }
  // A parent cycle fails the world bind rather than looping.
  {
    Bcmdl c;
    c.joints.resize(2);
    c.joints[0].parent = 1;
    c.joints[1].parent = 0;
    std::vector<Mat4> w;
    CHECK(!ComputeWorldBind(c, w, &err));
  }
}

void TestSyntheticBsmat() {
  const auto file = BuildBsmat();
  Bsmat m;
  std::string err;
  REQUIRE_MSG(ParseBsmat(file.data(), file.size(), m, &err), err);
  CHECK_EQ(m.version_major, 2);
  CHECK_EQ(m.version_minor, 17);
  CHECK_EQ(m.version_patch, 0);
  CHECK_EQ(m.name, std::string("synthmat"));
  CHECK_EQ(m.translucency_type, 8u);
  CHECK_EQ(m.render_layer, 31u);
  CHECK_EQ(m.shader_path, std::string("system/shd/synth.bshdat"));
  CHECK(m.blend_enabled);
  CHECK(m.blend_src == 6 && m.blend_dst == 7 && m.blend_op == 0);
  CHECK_EQ(m.cull_mode, uint32_t(kCullFront));
  CHECK(!m.stencil_enabled && m.stencil_mask == 0xFFFFFF && m.stencil_ref == 0xFFFFFFFFu);
  CHECK(m.stencil_fail == 1 && m.stencil_cmp == 5);
  CHECK(m.alpha_test_enabled && m.alpha_test_func == 5 && Near(m.alpha_test_ref, 0.5f));
  CHECK(m.depth_test && !m.depth_write && m.z_prepass && m.depth_mode == 5);
  REQUIRE(m.extra_uniforms.size() == size_t(1));
  CHECK_EQ(m.extra_uniforms[0].type, 'i');
  CHECK(m.extra_uniforms[0].i.size() == 2 && m.extra_uniforms[0].i[0] == -5);
  CHECK(m.extra_samplers.empty());
  REQUIRE(m.stages.size() == size_t(2));
  REQUIRE(m.stages[0].uniforms.size() == size_t(3));
  const BsmatUniform* color = m.FindUniform("vColor");
  REQUIRE(color != nullptr);
  CHECK(color->type == 'f' && color->f.size() == 4 && Near(color->f[0], 0.04f) && Near(color->f[3], 1.f));
  CHECK(Near(m.FindUniform("fFactor")->f[0], 50.f));
  CHECK_EQ(m.FindUniform("uMask")->u[0], 0xDEADBEEFu);
  CHECK(m.FindUniform("nothing") == nullptr);
  REQUIRE(m.stages[0].samplers.size() == size_t(2));
  const BsmatSampler* bc = m.FindSampler("texBaseColor");
  REQUIRE(bc != nullptr);
  CHECK_EQ(bc->sampler, std::string("sampler0"));
  CHECK_EQ(bc->type, std::string("texture2D"));
  CHECK_EQ(bc->index, 0u);
  CHECK_EQ(bc->filepath, std::string("actors/test/synth_bc.bctex"));
  CHECK(bc->wrap_u == kWrapRepeat && bc->wrap_v == kWrapClamp);
  CHECK(bc->border_color[0] == 0xFF && bc->border_color[3] == 0xFF && bc->border_color[1] == 0);
  CHECK(Near(bc->lod_bias, -1.5f) && Near(bc->max_mip_level, 15.f) && Near(bc->max_anisotropy, 16.f));
  const BsmatSampler* nm = m.FindSampler("texNormals");
  REQUIRE(nm != nullptr);
  CHECK(nm->index == 2 && nm->filepath.empty() && nm->wrap_u == kWrapClampColor && nm->wrap_v == kWrapMirror);
  CHECK(m.stages[1].uniforms.empty() && m.stages[1].samplers.empty());

  // Truncations fail cleanly; so do a bad magic, version and an absurd count.
  for (size_t n = 0; n < file.size(); ++n) {
    Bsmat t;
    CHECK_MSG(!ParseBsmat(file.data(), n, t, &err), "truncated bsmat parsed");
  }
  auto bad = file;
  bad[0] = 'X';
  CHECK(!ParseBsmat(bad.data(), bad.size(), m, &err));
  bad = file;
  bad[6] = 0x12;
  CHECK(!ParseBsmat(bad.data(), bad.size(), m, &err));
  // Stage count 0xFFFFFFFF: the last 4 + ... bytes; patch the stage count word.
  bad = file;
  const std::string tag = "iExtra";
  size_t at = 0;
  for (size_t i = 0; i + tag.size() < bad.size(); ++i) {
    if (std::memcmp(&bad[i], tag.data(), tag.size()) == 0) {
      at = i;
      break;
    }
  }
  REQUIRE(at != 0);
  bad[at - 4] = 0xFF, bad[at - 3] = 0xFF, bad[at - 2] = 0xFF, bad[at - 1] = 0xFF; // extra uniform count
  CHECK(!ParseBsmat(bad.data(), bad.size(), m, &err));
}

// ---- real data (opt-in) ----

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool LoadModel(const std::string& dir, const char* name, Bcmdl& m) {
  std::vector<uint8_t> data;
  const std::string path = dir + "/actors/characters/samus/models/" + name + ".bcmdl";
  if (!ReadFile(path, data)) {
    CHECK_MSG(false, "cannot read " + path);
    return false;
  }
  std::string err;
  if (!ParseBcmdl(data.data(), data.size(), m, &err)) {
    CHECK_MSG(false, path + ": " + err);
    return false;
  }
  return true;
}

void TestRealData() {
  const char* env = std::getenv("PORT_DREAD_SAMUS_PKG_DIR");
  if (env == nullptr || *env == '\0') {
    std::printf("SKIP: real-data Dread checks (PORT_DREAD_SAMUS_PKG_DIR not set)\n");
    return;
  }
  const std::string dir = env;
  struct Suit {
    const char* file;
    size_t joints, meshes;
  };
  const Suit suits[] = {{"samuspower", 106, 27}, {"samusvaria", 112, 27}, {"samusgravity", 116, 38}};
  for (const Suit& s : suits) {
    Bcmdl m;
    if (!LoadModel(dir, s.file, m)) {
      continue;
    }
    CHECK_EQ(m.joints.size(), s.joints);
    CHECK_EQ(m.meshes.size(), s.meshes);
    size_t enabled_verts = 0, enabled_idx = 0;
    for (const BcmdlMesh& mesh : m.meshes) {
      CHECK(!mesh.submeshes.empty());
      if (mesh.enabled) {
        enabled_verts += mesh.vertices.count;
        for (const auto& sm : mesh.submeshes) enabled_idx += sm.index_count;
      }
    }
    std::printf("%s: %zu joints, %zu meshes, enabled %zu verts %zu tris\n", s.file, m.joints.size(),
                m.meshes.size(), enabled_verts, enabled_idx / 3);
    std::vector<Mat4> world;
    std::string err;
    CHECK_MSG(ComputeWorldBind(m, world, &err), err);
    // The kb: world bind of L01_Hand_SKN equals DC_L_Hand's.
    const int hand = m.FindJoint("L01_Hand_SKN"), dc = m.FindJoint("DC_L_Hand");
    if (CHECK_MSG(hand >= 0 && dc >= 0, "hand joints missing") && !world.empty()) {
      const float o[3] = {0, 0, 0};
      float a[3], b[3];
      TransformPoint(world[hand], o, a);
      TransformPoint(world[dc], o, b);
      CHECK_MSG(Near3(a, b[0], b[1], b[2], 0.01f), "L01_Hand_SKN world pos != DC_L_Hand");
      CHECK_MSG(Near3(b, 91.3384f, 49.9668f, 8.8237f, 0.01f), "DC_L_Hand world pos");
      std::printf("  L01_Hand_SKN world (%.4f, %.4f, %.4f)\n", a[0], a[1], a[2]);
    }
    // Every skinned submesh yields finite bind-pose positions.
    for (const BcmdlMesh& mesh : m.meshes) {
      for (const BcmdlSubmesh& sm : mesh.submeshes) {
        if (sm.skin_type != 1 && sm.skin_type != 2) {
          continue;
        }
        std::vector<float> bind;
        if (!BindPoseModelPositions(m, world, mesh, sm, bind, &err)) {
          CHECK_MSG(false, mesh.name + ": " + err);
          continue;
        }
        for (float v : bind) {
          if (!std::isfinite(v)) {
            CHECK_MSG(false, mesh.name + ": non-finite bind position");
            break;
          }
        }
      }
    }
  }

  // Vertex values against the Python reference dumps.
  Bcmdl power;
  if (LoadModel(dir, "samuspower", power)) {
    const BcmdlMesh& body = power.meshes[23];
    CHECK_EQ(body.name, std::string("Mesh_Power"));
    CHECK(body.enabled);
    CHECK_EQ(body.vertices.count, 3126u);
    CHECK_EQ(body.indices.size(), size_t(12966));
    REQUIRE(body.submeshes.size() == size_t(2));
    CHECK(body.submeshes[0].skin_type == 2 && body.submeshes[0].first_index == 0 &&
          body.submeshes[0].index_count == 6333);
    CHECK(body.submeshes[1].first_index == 6333 && body.submeshes[1].index_count == 6633);
    CHECK_EQ(body.submeshes[0].joint_map.size(), size_t(20));
    CHECK_EQ(body.bsmat_path, std::string("actors/characters/samus/models/imats/samuspower_powerbody.bsmat"));
    const auto& v = body.vertices;
    CHECK(Near3(&v.pos[0], 1.6443777f, 88.471657f, 14.495136f, 1e-3f));
    CHECK(Near3(&v.normal[0], 0.66593009f, 0.30176845f, 0.68225574f, 1e-4f));
    CHECK(Near(v.uv[0][0], 0.10308158f, 1e-5f) && Near(v.uv[0][1], 0.9616946f, 1e-5f));
    CHECK(v.joints[0] == 0 && v.joints[1] == 0 && Near(v.weights[0], 1.f));
    CHECK(Near(v.tangent[3], -1.f));
    CHECK(Near3(&v.pos[3 * 3125], -23.546057f, -81.334976f, 3.9876056f, 1e-3f));
    CHECK(v.joints[4 * 3125] == 19 && Near(v.tangent[4 * 3125 + 3], 1.f));
    CHECK(body.indices[0] == 0 && body.indices[1] == 1 && body.indices[2] == 2 && body.indices[3] == 2 &&
          body.indices[4] == 3 && body.indices[5] == 0);

    const BcmdlMesh& limbs = power.meshes[24];
    CHECK_EQ(limbs.vertices.count, 1480u);
    const auto& l = limbs.vertices;
    CHECK(l.joints[0] == 0 && l.joints[1] == 3 && l.joints[2] == 4);
    CHECK(Near(l.weights[0], 0.9057759f, 1e-5f) && Near(l.weights[1], 0.07602795f, 1e-5f) &&
          Near(l.weights[2], 0.018196136f, 1e-5f) && l.weights[3] == 0.f);

    // Arm cannon (skin_type 1, bone-local): its bind-pose world bbox x runs about -120..-51
    // (DC_Shot sits at x = -117.7).
    const BcmdlMesh& cannon = power.meshes[19];
    CHECK_EQ(cannon.name, std::string("Samus_Cannon"));
    CHECK(Near3(&cannon.vertices.pos[0], -15.23695f, 2.526426f, -8.4201727f, 1e-4f));
    CHECK(Near3(cannon.transform, 12.58237f, 0.7948819f, 0.1887489f, 1e-4f));
    REQUIRE(cannon.submeshes.size() == size_t(1));
    CHECK_EQ(cannon.submeshes[0].skin_type, 1u);
    std::vector<Mat4> world;
    std::vector<float> bind;
    std::string err;
    if (ComputeWorldBind(power, world, &err) &&
        BindPoseModelPositions(power, world, cannon, cannon.submeshes[0], bind, &err)) {
      float lo = 1e9f, hi = -1e9f;
      for (size_t i = 0; i < bind.size(); i += 3) {
        lo = std::fmin(lo, bind[i]);
        hi = std::fmax(hi, bind[i]);
      }
      std::printf("  cannon bind-pose x range %.2f .. %.2f\n", lo, hi);
      CHECK_MSG(lo > -125.f && lo < -115.f && hi > -56.f && hi < -46.f, "cannon world x range");
    } else {
      CHECK_MSG(false, err);
    }
  }
  Bcmdl varia;
  if (LoadModel(dir, "samusvaria", varia)) {
    const BcmdlMesh& mv = varia.meshes[24];
    CHECK_EQ(mv.name, std::string("Mesh_Varia"));
    CHECK_EQ(mv.vertices.count, 1838u);
    CHECK_EQ(mv.indices.size(), size_t(7104));
    CHECK_EQ(mv.submeshes.size(), size_t(2));
    CHECK(Near3(&mv.vertices.pos[0], 50.58602f, 95.24713f, 4.3124361f, 1e-3f));
    const size_t last = 4 * 1837;
    CHECK(mv.vertices.joints[last] == 4 && mv.vertices.joints[last + 1] == 5 &&
          mv.vertices.joints[last + 2] == 6);
    CHECK(Near(mv.vertices.weights[last], 0.08160638f, 1e-5f) &&
          Near(mv.vertices.weights[last + 2], 0.5723936f, 1e-5f));
  }

  // The power body material.
  {
    std::vector<uint8_t> data;
    const std::string path =
        dir + "/actors/characters/samus/models/imats/samuspower_powerbody.bsmat";
    if (!ReadFile(path, data)) {
      CHECK_MSG(false, "cannot read " + path);
      return;
    }
    Bsmat m;
    std::string err;
    if (!ParseBsmat(data.data(), data.size(), m, &err)) {
      CHECK_MSG(false, err);
      return;
    }
    CHECK_EQ(m.name, std::string("samuspower_powerbody"));
    CHECK_EQ(m.shader_path, std::string("system/shd/mp_samuspbr.bshdat"));
    CHECK_EQ(m.cull_mode, uint32_t(kCullFront));
    CHECK(m.depth_test && m.depth_write && !m.alpha_test_enabled);
    CHECK(!m.stages.empty());
    const char* base = "actors/characters/samus/models/textures/powerbody_";
    const struct {
      const char* name;
      uint32_t index;
      const char* suffix;
    } want[] = {{"texBaseColor", 0, "bc.bctex"}, {"texAttributes", 1, "at.bctex"},
                {"texNormals", 2, "nm.bctex"}};
    for (const auto& w : want) {
      const BsmatSampler* s = m.FindSampler(w.name);
      if (CHECK_MSG(s != nullptr, w.name)) {
        CHECK_EQ(s->index, w.index);
        CHECK_EQ(s->filepath, std::string(base) + w.suffix);
      }
    }
    const BsmatSampler* cube = m.FindSampler("texCubemap");
    CHECK(cube != nullptr && cube->filepath.empty());
    const BsmatUniform* factor = m.FindUniform("fAlbedoEmissiveFactor");
    if (CHECK_MSG(factor != nullptr && factor->f.size() == 1, "fAlbedoEmissiveFactor")) {
      CHECK(Near(factor->f[0], 50.f));
    }
    const BsmatUniform* col = m.FindUniform("vAlbedoEmissiveColor");
    if (CHECK_MSG(col != nullptr && col->f.size() == 4, "vAlbedoEmissiveColor")) {
      CHECK(Near(col->f[0], 0.04f, 1e-3f) && Near(col->f[1], 1.f) && Near(col->f[2], 0.01f, 1e-3f));
    }
    CHECK(Near(m.FindUniform("fRough")->f[0], 1.f));
  }
}

uint64_t NameCrc(const std::string& s) {
  uint64_t c = ~uint64_t(0);
  for (char ch : s) {
    c ^= uint8_t(ch);
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (c >> 1) ^ 0xC96C5795D7870F42ull : c >> 1;
    }
  }
  return c;
}

void TestSyntheticBcskla() {
  // Two tracks: "Other", then "Pad" with pos.x keyed (u8 times 0, 10; values 1, 3, flat) and
  // euler.z keyed (u16 times 0, 4; values 0, 2, slopes 0.5).
  Buf b;
  b.u32(0x4D4E414Du);
  b.u32(0);
  b.u32(0);
  b.f32(11.f);
  b.u32(2);
  b.u32(0);
  auto track = [&](const char* name, uint32_t flags) {
    b.u64(NameCrc(name));
    b.u32(flags);
    const size_t base = b.pos();
    for (int i = 0; i < 9; ++i) {
      b.f32(i < 6 ? float(i + 1) * 10.f : 1.f);
    }
    return base;
  };
  track("Other", 0);
  const size_t base = track("Pad", 1u | (1u << 5));
  auto patch = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      b.b[at + i] = uint8_t(v >> (8 * i));
    }
  };
  patch(base, uint32_t(b.pos() - base));
  b.u16(8), b.u16(2), b.u8(0), b.u8(10), b.align(4);
  b.f32(1.f), b.f32(0.f), b.f32(3.f), b.f32(0.f);
  patch(base + 20, uint32_t(b.pos() - base));
  b.u16(0), b.u16(2), b.u16(0), b.u16(4);
  b.f32(0.f), b.f32(0.5f), b.f32(2.f), b.f32(0.5f);

  BcmdlJoint p;
  bool found = false;
  std::string err;
  CHECK_MSG(SampleBcskla(b.b.data(), b.b.size(), "Pad", 5.f, p, found, &err), err);
  CHECK(found);
  CHECK(Near(p.pos[0], 2.f) && Near(p.pos[1], 20.f) && Near(p.euler[0], 40.f) && Near(p.scale[2], 1.f));
  // Hermite at u = 1 (the last key) and past it: the last value.
  CHECK(Near(p.euler[2], 2.f));
  CHECK(SampleBcskla(b.b.data(), b.b.size(), "Pad", 2.f, p, found, &err));
  CHECK(Near(p.euler[2], 1.f));  // symmetric keys, equal slopes: the midpoint
  CHECK(SampleBcskla(b.b.data(), b.b.size(), "Pad", 0.f, p, found, &err));
  CHECK(Near(p.pos[0], 1.f));
  CHECK(SampleBcskla(b.b.data(), b.b.size(), "pad", 0.f, p, found, &err) && !found);  // case kept
  CHECK(!SampleBcskla(b.b.data(), 40, "Pad", 0.f, p, found, &err));                  // truncated
}

void TestRealBcskla() {
  const char* env = std::getenv("PORT_DREAD_SAMUS_PKG_DIR");
  if (env == nullptr || *env == '\0') {
    return;
  }
  std::vector<uint8_t> data;
  const std::string path = std::string(env) + "/actors/characters/samus/animations/relaxalt01_loop.bcskla";
  if (!CHECK_MSG(ReadFile(path, data), "cannot read " + path)) {
    return;
  }
  BcmdlJoint p;
  bool found = false;
  std::string err;
  CHECK_MSG(SampleBcskla(data.data(), data.size(), "L01_ShoulderPad", 0.f, p, found, &err), err);
  CHECK(found && Near3(p.pos, 19.923f, 27.503f, 11.217f, 2e-3f) && Near3(p.euler, 0.1948f, 0.0150f, -0.1244f, 2e-4f));
}

} // namespace

int main() {
  TestSyntheticBcmdl();
  TestBadBcmdl();
  TestSyntheticBsmat();
  TestRealData();
  TestSyntheticBcskla();
  TestRealBcskla();
  return PORT_TEST_RESULT();
}
