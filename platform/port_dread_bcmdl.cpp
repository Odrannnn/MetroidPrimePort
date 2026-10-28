#include "port_dread_bcmdl.h"

#include <zlib.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace PortDread {
namespace {

// Bounds-checked little-endian reader over the file span. Every Get* fails (returns false and
// records the first error) when the access leaves the span.
class Span {
public:
  Span(const uint8_t* d, size_t n) : d_(d), n_(n) {}
  size_t size() const { return n_; }
  const uint8_t* data() const { return d_; }
  const std::string& error() const { return err_; }
  bool Fail(const std::string& what) {
    if (err_.empty()) {
      err_ = what;
    }
    return false;
  }
  bool Range(uint64_t off, uint64_t len, const char* what) {
    if (off > n_ || len > n_ - off) {
      return Fail(std::string(what) + ": offset out of range");
    }
    return true;
  }
  bool U8(uint64_t off, uint8_t& v, const char* what) {
    if (!Range(off, 1, what)) {
      return false;
    }
    v = d_[off];
    return true;
  }
  bool U16(uint64_t off, uint16_t& v, const char* what) {
    if (!Range(off, 2, what)) {
      return false;
    }
    v = uint16_t(d_[off] | (d_[off + 1] << 8));
    return true;
  }
  bool U32(uint64_t off, uint32_t& v, const char* what) {
    if (!Range(off, 4, what)) {
      return false;
    }
    v = uint32_t(d_[off]) | (uint32_t(d_[off + 1]) << 8) | (uint32_t(d_[off + 2]) << 16) |
        (uint32_t(d_[off + 3]) << 24);
    return true;
  }
  bool U64(uint64_t off, uint64_t& v, const char* what) {
    uint32_t lo, hi;
    if (!U32(off, lo, what) || !U32(off + 4, hi, what)) {
      return false;
    }
    v = uint64_t(lo) | (uint64_t(hi) << 32);
    return true;
  }
  bool F32(uint64_t off, float& v, const char* what) {
    uint32_t u;
    if (!U32(off, u, what)) {
      return false;
    }
    std::memcpy(&v, &u, 4);
    return true;
  }
  bool F32n(uint64_t off, float* v, int n, const char* what) {
    for (int i = 0; i < n; ++i) {
      if (!F32(off + 4 * uint64_t(i), v[i], what)) {
        return false;
      }
    }
    return true;
  }
  // NUL-terminated string at `off`.
  bool Str(uint64_t off, std::string& s, const char* what) {
    if (off >= n_) {
      return Fail(std::string(what) + ": string offset out of range");
    }
    const void* nul = std::memchr(d_ + off, 0, n_ - off);
    if (nul == nullptr) {
      return Fail(std::string(what) + ": unterminated string");
    }
    s.assign(reinterpret_cast<const char*>(d_ + off),
             size_t(static_cast<const uint8_t*>(nul) - (d_ + off)));
    return true;
  }

private:
  const uint8_t* d_;
  size_t n_;
  std::string err_;
};

bool IsGzip(const uint8_t* p, size_t n) {
  return n >= 4 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 0x08 && p[3] == 0x00;
}

// A vertex/index buffer: gzip (zlib windowBits 15+32) or raw; `raw_size` is the expected
// decompressed size and `stored` the size in the file.
bool LoadBuffer(Span& s, uint64_t off, uint64_t stored, uint64_t raw_size, std::vector<uint8_t>& out,
                const char* what) {
  if (!s.Range(off, 0, what)) {
    return false;
  }
  const size_t avail = s.size() - size_t(off);
  const uint8_t* src = s.data() + off;
  if (IsGzip(src, avail)) {
    if (stored > avail || stored < 4) {
      return s.Fail(std::string(what) + ": compressed size out of range");
    }
    if (raw_size > (uint64_t(1) << 30)) {
      return s.Fail(std::string(what) + ": decompressed size implausible");
    }
    out.assign(size_t(raw_size), 0);
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
      return s.Fail("zlib init failed");
    }
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = uInt(stored);
    zs.next_out = out.data();
    zs.avail_out = uInt(out.size());
    const int rc = inflate(&zs, Z_FINISH);
    const uLong produced = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
      return s.Fail(std::string(what) + ": inflate failed or output larger than declared");
    }
    if (produced != raw_size) {
      return s.Fail(std::string(what) + ": decompressed size differs from declared size");
    }
    return true;
  }
  if (raw_size > avail) {
    return s.Fail(std::string(what) + ": raw buffer out of range");
  }
  out.assign(src, src + size_t(raw_size));
  return true;
}

constexpr uint64_t kMaxEntries = 1u << 20;

} // namespace

Mat4 Mat4::Identity() {
  Mat4 r{};
  for (int i = 0; i < 4; ++i) {
    r.m[i][i] = 1.f;
  }
  return r;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      float v = 0.f;
      for (int k = 0; k < 4; ++k) {
        v += a.m[i][k] * b.m[k][j];
      }
      r.m[i][j] = v;
    }
  }
  return r;
}

void TransformPoint(const Mat4& m, const float p[3], float out[3]) {
  const float x = p[0], y = p[1], z = p[2];
  for (int i = 0; i < 3; ++i) {
    out[i] = m.m[i][0] * x + m.m[i][1] * y + m.m[i][2] * z + m.m[i][3];
  }
}

int Bcmdl::FindJoint(const std::string& name) const {
  for (size_t i = 0; i < joints.size(); ++i) {
    if (joints[i].name == name) {
      return int(i);
    }
  }
  return -1;
}

namespace {

bool ParseJoints(Span& s, uint64_t jtoc, Bcmdl& out) {
  uint32_t count;
  uint64_t list;
  if (!s.U32(jtoc, count, "joint count") || !s.U64(jtoc + 8, list, "joint list")) {
    return false;
  }
  if (count > kMaxEntries) {
    return s.Fail("joint count implausible");
  }
  std::vector<std::string> parent_names(count);
  std::vector<bool> has_parent(count, false);
  out.joints.assign(count, BcmdlJoint{});
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t entry, tr, name_ptr, parent_ptr;
    if (!s.U64(list + 16ull * i, entry, "joint entry") ||
        !s.U64(entry, tr, "joint transform ptr") || !s.U64(entry + 8, name_ptr, "joint name ptr") ||
        !s.U64(entry + 16, parent_ptr, "joint parent ptr")) {
      return false;
    }
    BcmdlJoint& j = out.joints[i];
    if (!s.Str(name_ptr, j.name, "joint name") || !s.F32n(tr, j.pos, 3, "joint pos") ||
        !s.F32n(tr + 12, j.euler, 3, "joint euler") || !s.F32n(tr + 24, j.scale, 3, "joint scale")) {
      return false;
    }
    if (parent_ptr != 0) {
      has_parent[i] = true;
      if (!s.Str(parent_ptr, parent_names[i], "joint parent name")) {
        return false;
      }
    }
  }
  std::unordered_map<std::string, int> by_name;
  for (uint32_t i = 0; i < count; ++i) {
    by_name.emplace(out.joints[i].name, int(i)); // first wins
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!has_parent[i]) {
      continue;
    }
    auto it = by_name.find(parent_names[i]);
    if (it == by_name.end()) {
      return s.Fail("joint '" + out.joints[i].name + "': unknown parent '" + parent_names[i] + "'");
    }
    out.joints[i].parent = it->second;
  }
  return true;
}

bool ParseMesh(Span& s, uint64_t entry, size_t joint_count, BcmdlMesh& m) {
  uint64_t spec, mat, name_entry;
  if (!s.U64(entry, spec, "mesh spec") || !s.U64(entry + 8, mat, "mesh material") ||
      !s.U64(entry + 16, name_entry, "mesh name entry")) {
    return false;
  }
  if (name_entry != 0) {
    uint64_t name_ptr;
    uint8_t enabled;
    if (!s.U64(name_entry, name_ptr, "mesh name ptr") ||
        !s.U8(name_entry + 8, enabled, "mesh enabled") || !s.Str(name_ptr, m.name, "mesh name")) {
      return false;
    }
    m.enabled = enabled != 0;
  }
  if (mat != 0) {
    uint64_t np, pp;
    if (!s.U64(mat, np, "material name ptr") || !s.U64(mat + 8, pp, "material path ptr") ||
        !s.Str(np, m.material_name, "material name") || !s.Str(pp, m.bsmat_path, "bsmat path")) {
      return false;
    }
  }

  uint64_t ib, vb, smlist;
  uint32_t submesh_count;
  if (!s.U64(spec + 0x50, ib, "index info ptr") || !s.U64(spec + 0x58, vb, "vertex info ptr") ||
      !s.U32(spec + 0x60, submesh_count, "submesh count") ||
      !s.U64(spec + 0x68, smlist, "submesh list ptr") ||
      !s.F32n(spec + 0x70, m.transform, 3, "mesh transform")) {
    return false;
  }
  if (submesh_count > kMaxEntries) {
    return s.Fail("submesh count implausible");
  }

  // Vertex buffer.
  uint32_t raw_size, vcount, comp_size, attr_count;
  uint64_t vdata;
  if (!s.U32(vb + 12, raw_size, "vertex raw size") || !s.U32(vb + 16, vcount, "vertex count") ||
      !s.U32(vb + 20, comp_size, "vertex comp size") || !s.U64(vb + 24, vdata, "vertex data ptr") ||
      !s.U32(vb + 32, attr_count, "attribute count")) {
    return false;
  }
  if (attr_count > 64) {
    return s.Fail("attribute count implausible");
  }
  std::vector<uint8_t> vbuf;
  if (!LoadBuffer(s, vdata, comp_size, raw_size, vbuf, "vertex buffer")) {
    return false;
  }
  m.vertices.count = vcount;
  for (uint32_t a = 0; a < attr_count; ++a) {
    const uint64_t ao = vb + 40 + 16ull * a;
    uint32_t semantic, byte_off;
    uint16_t type, comps;
    if (!s.U32(ao, semantic, "attr semantic") || !s.U32(ao + 4, byte_off, "attr offset") ||
        !s.U16(ao + 8, type, "attr type") || !s.U16(ao + 10, comps, "attr comps")) {
      return false;
    }
    if (semantic > 8) {
      continue; // unknown stream
    }
    static const uint16_t kComps[9] = {3, 3, 2, 2, 2, 4, 4, 4, 4};
    if (type != 3) {
      return s.Fail("attribute " + std::to_string(semantic) + ": unsupported type " +
                    std::to_string(type));
    }
    if (comps != kComps[semantic]) {
      return s.Fail("attribute " + std::to_string(semantic) + ": unexpected component count " +
                    std::to_string(comps));
    }
    const uint64_t bytes = uint64_t(vcount) * comps * 4;
    if (byte_off > vbuf.size() || bytes > vbuf.size() - byte_off) {
      return s.Fail("attribute " + std::to_string(semantic) + ": plane outside vertex buffer");
    }
    const size_t n = size_t(vcount) * comps;
    std::vector<float> f(n);
    if (n != 0) {
      std::memcpy(f.data(), vbuf.data() + byte_off, n * 4); // planar f32, file is little-endian
    }
    switch (semantic) {
    case 0: m.vertices.pos = std::move(f); break;
    case 1: m.vertices.normal = std::move(f); break;
    case 2:
    case 3:
    case 4: m.vertices.uv[semantic - 2] = std::move(f); break;
    case 5: m.vertices.color = std::move(f); break;
    case 6: {
      m.vertices.joints.resize(n);
      for (size_t i = 0; i < n; ++i) {
        const float v = f[i];
        if (!(v > -1.f && v < 65536.f)) {
          return s.Fail("joint index attribute out of range");
        }
        m.vertices.joints[i] = int32_t(std::lround(v));
      }
      break;
    }
    case 7: m.vertices.weights = std::move(f); break;
    case 8: m.vertices.tangent = std::move(f); break;
    }
  }

  // Index buffer.
  uint32_t icount, icomp;
  uint64_t idata;
  if (!s.U32(ib + 12, icount, "index count") || !s.U32(ib + 16, icomp, "index comp size") ||
      !s.U64(ib + 24, idata, "index data ptr")) {
    return false;
  }
  std::vector<uint8_t> ibuf;
  if (!LoadBuffer(s, idata, icomp, uint64_t(icount) * 2, ibuf, "index buffer")) {
    return false;
  }
  m.indices.resize(icount);
  for (uint32_t i = 0; i < icount; ++i) {
    const uint16_t v = uint16_t(ibuf[2 * i] | (ibuf[2 * i + 1] << 8));
    if (v >= vcount) {
      return s.Fail("index " + std::to_string(i) + " >= vertex count");
    }
    m.indices[i] = v;
  }

  // Submeshes.
  m.submeshes.assign(submesh_count, BcmdlSubmesh{});
  for (uint32_t k = 0; k < submesh_count; ++k) {
    uint64_t so;
    uint32_t jcount;
    uint64_t jptr;
    BcmdlSubmesh& sm = m.submeshes[k];
    if (!s.U64(smlist + 16ull * k, so, "submesh entry") ||
        !s.U32(so, sm.skin_type, "submesh skin type") ||
        !s.U32(so + 4, sm.first_index, "submesh first index") ||
        !s.U32(so + 8, sm.index_count, "submesh index count") ||
        !s.U32(so + 12, jcount, "submesh joint count") || !s.U64(so + 16, jptr, "submesh jmap ptr")) {
      return false;
    }
    if (uint64_t(sm.first_index) + sm.index_count > icount) {
      return s.Fail("submesh index range outside index buffer");
    }
    if (jcount > kMaxEntries || !s.Range(jptr, uint64_t(jcount) * 4, "submesh jmap")) {
      return s.Fail("submesh joint map out of range");
    }
    sm.joint_map.resize(jcount);
    for (uint32_t i = 0; i < jcount; ++i) {
      uint32_t v;
      s.U32(jptr + 4ull * i, v, "jmap");
      if (int32_t(v) < 0 || size_t(v) >= joint_count) {
        return s.Fail("submesh joint map entry is not a joint index");
      }
      sm.joint_map[i] = int32_t(v);
    }
  }
  return true;
}

} // namespace

bool ParseBcmdl(const uint8_t* data, size_t size, Bcmdl& out, std::string* error) {
  Span s(data, size);
  out = Bcmdl{};
  auto done = [&](bool ok) {
    if (!ok && error != nullptr) {
      *error = s.error().empty() ? "bad bcmdl" : s.error();
    }
    return ok;
  };
  if (data == nullptr || size < 0x58) {
    s.Fail("file too small");
    return done(false);
  }
  if (std::memcmp(data, "MMDL", 4) != 0) {
    s.Fail("not an MMDL file");
    return done(false);
  }
  uint32_t ver;
  s.U32(4, ver, "version");
  // The header word 0x003A0001 reads as u16 major = 1 (low), u16 minor = 0x3A = 58 (high).
  out.version_major = uint16_t(ver & 0xFFFF);
  out.version_minor = uint16_t(ver >> 16);
  if (ver != 0x003A0001u) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", ver);
    s.Fail(std::string("unsupported MMDL version word 0x") + b);
    return done(false);
  }

  uint64_t toc[10];
  for (int i = 0; i < 10; ++i) {
    if (!s.U64(8 + 8ull * i, toc[i], "toc")) {
      return done(false);
    }
  }
  if (toc[7] == 0 || toc[4] == 0) {
    s.Fail("missing joint or mesh table");
    return done(false);
  }
  if (!ParseJoints(s, toc[7], out)) {
    return done(false);
  }

  // Meshes: contiguous {entry, next} records, ended by next == 0.
  uint64_t p = toc[4];
  for (uint64_t n = 0;; ++n) {
    uint64_t entry, next;
    if (n >= kMaxEntries || !s.U64(p, entry, "mesh table") || !s.U64(p + 8, next, "mesh table")) {
      s.Fail("mesh table too long or truncated");
      return done(false);
    }
    out.meshes.emplace_back();
    if (!ParseMesh(s, entry, out.joints.size(), out.meshes.back())) {
      return done(false);
    }
    if (next == 0) {
      break;
    }
    p += 16;
  }
  return done(true);
}

Mat4 JointLocalMatrix(const BcmdlJoint& j) {
  const float cx = std::cos(j.euler[0]), sx = std::sin(j.euler[0]);
  const float cy = std::cos(j.euler[1]), sy = std::sin(j.euler[1]);
  const float cz = std::cos(j.euler[2]), sz = std::sin(j.euler[2]);
  // R = Rz * Ry * Rx
  const float r[3][3] = {
      {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
      {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
      {-sy, cy * sx, cy * cx},
  };
  Mat4 m = Mat4::Identity();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      m.m[row][col] = r[row][col] * j.scale[col]; // R * S
    }
    m.m[row][3] = j.pos[row];
  }
  return m;
}

bool ComputeWorldBind(const Bcmdl& model, std::vector<Mat4>& world, std::string* error) {
  const size_t n = model.joints.size();
  world.assign(n, Mat4::Identity());
  std::vector<uint8_t> state(n, 0); // 0 todo, 1 in progress, 2 done
  std::vector<size_t> stack;
  for (size_t start = 0; start < n; ++start) {
    if (state[start] == 2) {
      continue;
    }
    stack.assign(1, start);
    while (!stack.empty()) {
      const size_t i = stack.back();
      const int par = model.joints[i].parent;
      if (par >= 0 && size_t(par) >= n) {
        if (error != nullptr) {
          *error = "joint '" + model.joints[i].name + "': bad parent index";
        }
        return false;
      }
      if (state[i] == 0) {
        state[i] = 1;
        if (par >= 0 && state[par] != 2) {
          if (state[par] == 1) {
            if (error != nullptr) {
              *error = "joint parent cycle at '" + model.joints[i].name + "'";
            }
            return false;
          }
          stack.push_back(size_t(par));
          continue;
        }
      }
      const Mat4 local = JointLocalMatrix(model.joints[i]);
      world[i] = par >= 0 ? world[par] * local : local;
      state[i] = 2;
      stack.pop_back();
    }
  }
  return true;
}

bool BindPosePosition(const Bcmdl& model, const std::vector<Mat4>& world, const BcmdlMesh& mesh,
                      const BcmdlSubmesh& sm, uint32_t v, float out[3], std::string* error) {
  auto fail = [&](const char* msg) {
    if (error != nullptr) {
      *error = msg;
    }
    return false;
  };
  const BcmdlVertices& vs = mesh.vertices;
  if (v >= vs.count || vs.pos.size() != size_t(vs.count) * 3) {
    return fail("vertex out of range or mesh has no positions");
  }
  if (world.size() != model.joints.size()) {
    return fail("world bind matrix count differs from joint count");
  }
  const float p[3] = {vs.pos[3 * size_t(v)] + mesh.transform[0],
                      vs.pos[3 * size_t(v) + 1] + mesh.transform[1],
                      vs.pos[3 * size_t(v) + 2] + mesh.transform[2]};
  if (sm.skin_type == 2) {
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    return true;
  }
  if (sm.skin_type != 1) {
    return fail("unsupported skin type");
  }
  if (vs.joints.size() != size_t(vs.count) * 4 || vs.weights.size() != size_t(vs.count) * 4) {
    return fail("mesh lacks joint indices or weights");
  }
  float acc[3] = {0, 0, 0};
  for (int k = 0; k < 4; ++k) {
    const float w = vs.weights[4 * size_t(v) + k];
    if (w == 0.f) {
      continue;
    }
    const int32_t ji = vs.joints[4 * size_t(v) + k];
    if (ji < 0 || size_t(ji) >= sm.joint_map.size()) {
      return fail("vertex joint index outside the submesh joint map");
    }
    float t[3];
    TransformPoint(world[size_t(sm.joint_map[size_t(ji)])], p, t);
    for (int i = 0; i < 3; ++i) {
      acc[i] += w * t[i];
    }
  }
  out[0] = acc[0];
  out[1] = acc[1];
  out[2] = acc[2];
  return true;
}

bool BindPoseModelPositions(const Bcmdl& model, const std::vector<Mat4>& world,
                            const BcmdlMesh& mesh, const BcmdlSubmesh& sm, std::vector<float>& out,
                            std::string* error) {
  out.assign(size_t(mesh.vertices.count) * 3, 0.f);
  for (uint32_t v = 0; v < mesh.vertices.count; ++v) {
    if (!BindPosePosition(model, world, mesh, sm, v, &out[3 * size_t(v)], error)) {
      return false;
    }
  }
  return true;
}

bool SampleBcskla(const uint8_t* data, size_t size, const std::string& joint, float frame, BcmdlJoint& pose,
                  bool& found, std::string* error) {
  found = false;
  Span s(data, size);
  auto fail = [&]() {
    if (error != nullptr) {
      *error = "bcskla: " + s.error();
    }
    return false;
  };
  uint32_t magic = 0, tracks = 0;
  if (!s.U32(0, magic, "magic") || !s.U32(16, tracks, "track count")) {
    return fail();
  }
  if (magic != 0x4D4E414Du) {  // "MANM"
    s.Fail("not a MANM file");
    return fail();
  }
  // CRC-64 (ECMA-182, reflected, ~0 in, no final xor) of the name as written.
  static const auto kTable = [] {
    std::array<uint64_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint64_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (c >> 1) ^ 0xC96C5795D7870F42ull : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  uint64_t want = ~uint64_t(0);
  for (char ch : joint) {
    want = kTable[(want ^ uint8_t(ch)) & 0xFF] ^ (want >> 8);
  }
  for (uint32_t k = 0; k < tracks; ++k) {
    const uint64_t o = 0x18 + uint64_t(k) * 0x30;
    uint64_t hash = 0;
    uint32_t flags = 0;
    if (!s.U64(o, hash, "track") || !s.U32(o + 8, flags, "track flags")) {
      return fail();
    }
    if (hash != want) {
      continue;
    }
    // Channel i at base + 4i: a constant, or (flag bit i) an offset from base to its keys:
    // u16 timing type (0: u16 times, 8: u8 times), u16 count, the times, 4-aligned, then
    // count (value, slope) pairs.
    const uint64_t base = o + 12;
    float v[9];
    for (int i = 0; i < 9; ++i) {
      const uint64_t at = base + 4 * uint64_t(i);
      if ((flags & (1u << i)) == 0) {
        if (!s.F32(at, v[i], "channel")) {
          return fail();
        }
        continue;
      }
      uint32_t rel = 0;
      uint16_t timing = 0, count = 0;
      if (!s.U32(at, rel, "channel offset") || !s.U16(base + rel, timing, "keys") ||
          !s.U16(base + rel + 2, count, "keys")) {
        return fail();
      }
      if (count == 0 || (timing != 0 && timing != 8)) {
        s.Fail("unknown key layout");
        return fail();
      }
      const uint64_t times = base + rel + 4, width = timing == 0 ? 2 : 1;
      const uint64_t values = (times + width * count + 3) & ~uint64_t(3);
      auto key = [&](uint32_t n, float& t, float& val, float& slope) {
        if (timing == 0) {
          uint16_t u = 0;
          if (!s.U16(times + 2 * uint64_t(n), u, "key time")) {
            return false;
          }
          t = float(u);
        } else {
          uint8_t u = 0;
          if (!s.U8(times + n, u, "key time")) {
            return false;
          }
          t = float(u);
        }
        return s.F32(values + 8 * uint64_t(n), val, "key") && s.F32(values + 8 * uint64_t(n) + 4, slope, "key");
      };
      float t0, v0, d0;
      if (!key(0, t0, v0, d0)) {
        return fail();
      }
      v[i] = v0;
      for (uint32_t n = 1; n < count && frame > t0; ++n) {
        float t1, v1, d1;
        if (!key(n, t1, v1, d1)) {
          return fail();
        }
        if (frame <= t1) {
          const float h = t1 - t0, u = h > 0.f ? (frame - t0) / h : 0.f, u2 = u * u, u3 = u2 * u;
          v[i] = (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * h * d0 + (-2 * u3 + 3 * u2) * v1 +
                 (u3 - u2) * h * d1;
          break;
        }
        v[i] = v1;
        t0 = t1;
        v0 = v1;
        d0 = d1;
      }
    }
    for (int a = 0; a < 3; ++a) {
      pose.pos[a] = v[a];
      pose.euler[a] = v[3 + a];
      pose.scale[a] = v[6 + a];
    }
    found = true;
    return true;
  }
  return true;
}

} // namespace PortDread
