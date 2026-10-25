// The dread-samus import's core: Dread suit bodies -> re-skin -> suit materials -> converter.
// See port_dread_samus.h.

#include "port_dread_samus.h"

#include "port_ball_looks.h"
#include "port_dread_bcmdl.h"
#include "port_dread_bctex.h"
#include "port_dread_bsmat.h"
#include "port_dread_pkg.h"
#include "port_remastered_convert.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace PortDreadSamus {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

float BeF(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t u = Be32(d, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

std::string Hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}

bool Fail(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

}  // namespace

bool RetailSkinFromCmdl(const std::vector<uint8_t>& m, const std::vector<uint8_t>& k, PortDread::RetailSkinRef& out,
                        std::string* error) {
  out = {};
  if (m.size() < 0x2c || Be32(m, 0) != 0xDEADBABE || k.size() < 4) {
    return Fail(error, "retail body: not a CMDL and CSKR");
  }
  const uint32_t nsec = Be32(m, 0x24), nmat = Be32(m, 0x28);
  if (nmat >= nsec || 0x2c + 4ull * nsec > m.size()) {
    return Fail(error, "retail body: bad section table");
  }
  size_t o = (0x2c + 4 * size_t(nsec) + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nmat; ++i) {
    o += Be32(m, 0x2c + 4 * size_t(i));
  }
  const size_t psize = Be32(m, 0x2c + 4 * size_t(nmat));
  if (o + psize > m.size()) {
    return Fail(error, "retail body: positions past the end");
  }
  size_t ko = 4;
  const uint32_t groups = Be32(k, 0);
  for (uint32_t g = 0; g < groups; ++g) {
    if (ko + 4 > k.size()) {
      return Fail(error, "retail skin: cut short");
    }
    const uint32_t nw = Be32(k, ko);
    ko += 4;
    if (nw > 64 || ko + 8ull * nw + 4 > k.size()) {
      return Fail(error, "retail skin: cut short");
    }
    std::vector<std::pair<uint32_t, float>> ws;
    for (uint32_t i = 0; i < nw; ++i, ko += 8) {
      ws.emplace_back(Be32(k, ko), BeF(k, ko + 4));
    }
    const uint32_t count = Be32(k, ko);
    ko += 4;
    if (out.weights.size() + count > psize / 12 + 1) {
      return Fail(error, "retail skin: more vertices than the model");
    }
    out.weights.insert(out.weights.end(), count, ws);
  }
  const size_t n = std::min(out.weights.size(), psize / 12);
  out.weights.resize(n);
  out.pos.reserve(n * 3);
  for (size_t i = 0; i < n * 3; ++i) {
    out.pos.push_back(BeF(m, o + 4 * i));
  }
  return n > 0 ? true : Fail(error, "retail body: no skinned vertices");
}

bool BuildModel(const PortDread::ReskinResult& reskin, const std::vector<const PortDread::SuitMaterial*>& meshMaterials,
                PortRemastered::Model& out, std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>>& boneWeights,
                std::string* error) {
  using namespace PortRemastered;
  out = {};
  boneWeights.clear();
  out.form = FourCC('C', 'M', 'D', 'L');
  out.skinned = true;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  std::map<const PortDread::SuitMaterial*, uint32_t> matIndex;
  for (size_t mi = 0; mi < reskin.meshes.size(); ++mi) {
    const PortDread::ReskinMesh& rm = reskin.meshes[mi];
    const PortDread::SuitMaterial* sm = mi < meshMaterials.size() ? meshMaterials[mi] : nullptr;
    if (sm == nullptr || rm.vertex_count == 0) {
      continue;
    }
    if (rm.pos.size() != size_t(rm.vertex_count) * 3 || rm.normal.size() != rm.pos.size() ||
        rm.weights.size() != rm.vertex_count) {
      return Fail(error, "mesh " + rm.name + ": inconsistent arrays");
    }
    auto [it, added] = matIndex.emplace(sm, uint32_t(out.materials.size()));
    if (added) {
      out.materials.push_back(sm->material);
    }
    const uint32_t buffer = uint32_t(out.vertexBuffers.size());
    ModelVertexBuffer& vb = out.vertexBuffers.emplace_back();
    vb.vertexCount = rm.vertex_count;
    vb.positions = rm.pos;
    vb.normals = rm.normal;
    if (rm.tangent.size() == size_t(rm.vertex_count) * 4) {
      vb.tangents = rm.tangent;
    }
    size_t sets = 0;
    for (size_t s = 0; s < 3; ++s) {
      if (rm.uv[s].size() == size_t(rm.vertex_count) * 2) {
        sets = s + 1;
      }
    }
    vb.uvs.resize(std::max<size_t>(sets, 1));
    for (size_t s = 0; s < vb.uvs.size(); ++s) {
      vb.uvs[s] = rm.uv[s].size() == size_t(rm.vertex_count) * 2 ? rm.uv[s]
                                                                   : std::vector<float>(size_t(rm.vertex_count) * 2, 0.f);
    }
    for (uint32_t v = 0; v < rm.vertex_count; ++v) {
      for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], rm.pos[size_t(v) * 3 + a]);
        hi[a] = std::max(hi[a], rm.pos[size_t(v) * 3 + a]);
      }
    }
    boneWeights.resize(buffer + 1);
    boneWeights[buffer] = rm.weights;
    for (const PortDread::ReskinSubmesh& sub : rm.submeshes) {
      if (sub.indices.size() < 3) {
        continue;
      }
      for (uint32_t i : sub.indices) {
        if (i >= rm.vertex_count) {
          return Fail(error, "mesh " + rm.name + ": index past its vertices");
        }
      }
      ModelMesh& mesh = out.meshes.emplace_back();
      mesh.material = it->second;
      mesh.vertexBuffer = buffer;
      mesh.indexBuffer = buffer;
      mesh.indexCount = uint32_t(sub.indices.size() / 3 * 3);
      mesh.indexWidth = 4;
      mesh.vertexCount = rm.vertex_count;
      mesh.bits2 = 0;
      mesh.indices.assign(sub.indices.begin(), sub.indices.begin() + mesh.indexCount);
    }
  }
  if (out.meshes.empty()) {
    return Fail(error, "no mesh with a suit material");
  }
  for (int a = 0; a < 3; ++a) {
    out.boundsMin[a] = lo[a];
    out.boundsMax[a] = hi[a];
  }
  return true;
}

// The cannon's lights are a glowing core on its barrel axis, which in Dread only shows through the
// shell's slits; seen from behind, through the back opening Samus's arm fills in Dread, they sit
// deep in the middle. Each triangle the glow map (ICAN, texcoord 0) lights is pushed out from the
// axis (the glow's centroid in x and z; y runs along the barrel) onto the inside of the shell, so
// the lights line the shell's edge. Its vertices are copied first where unlit triangles share them.
void PushGlowToShell(PortDread::ReskinMesh& rm, const PortRemastered::Image& ican) {
  if (rm.uv[0].empty() || ican.width == 0 || ican.height == 0) return;
  auto lit = [&](const uint32_t* t) {
    double u = 0.0, v = 0.0;
    for (int c = 0; c < 3; ++c) u += rm.uv[0][2 * t[c]] / 3.0, v += rm.uv[0][2 * t[c] + 1] / 3.0;
    u -= std::floor(u), v -= std::floor(v);
    const uint32_t x = std::min(uint32_t(u * ican.width), uint32_t(ican.width) - 1);
    const uint32_t y = std::min(uint32_t(v * ican.height), uint32_t(ican.height) - 1);
    const uint8_t* p = &ican.rgba[(size_t(y) * ican.width + x) * 4];
    return std::max({p[0], p[1], p[2]}) >= 32;
  };
  std::vector<uint8_t> glowVertex(rm.vertex_count, 0), plainVertex(rm.vertex_count, 0);
  std::vector<std::array<float, 9>> shell;  // the unlit triangles, for the push
  for (const auto& sub : rm.submeshes) {
    for (size_t i = 0; i + 2 < sub.indices.size(); i += 3) {
      const uint32_t* t = &sub.indices[i];
      const bool glow = lit(t);
      std::array<float, 9> tri;
      for (int c = 0; c < 3; ++c) {
        (glow ? glowVertex : plainVertex)[t[c]] = 1;
        std::copy_n(&rm.pos[3 * t[c]], 3, &tri[3 * c]);
      }
      if (!glow) shell.push_back(tri);
    }
  }
  double cx = 0.0, cz = 0.0, n = 0.0;
  for (uint32_t v = 0; v < rm.vertex_count; ++v) {
    if (glowVertex[v]) cx += rm.pos[3 * v], cz += rm.pos[3 * v + 2], n += 1.0;
  }
  if (n == 0.0) return;
  cx /= n, cz /= n;
  // A glow vertex an unlit triangle shares becomes two: the unlit side keeps the original.
  std::vector<uint32_t> moved(rm.vertex_count, UINT32_MAX);
  auto copyVertex = [&](uint32_t v) {
    auto grow = [&](std::vector<float>& a, size_t k) {
      if (!a.empty()) a.insert(a.end(), a.begin() + k * v, a.begin() + k * (v + 1));
    };
    grow(rm.pos, 3), grow(rm.normal, 3), grow(rm.tangent, 4);
    for (auto& uv : rm.uv) grow(uv, 2);
    if (!rm.weights.empty()) rm.weights.push_back(rm.weights[v]);
    if (!rm.source.empty()) rm.source.push_back(rm.source[v]);
    return rm.vertex_count++;
  };
  for (auto& sub : rm.submeshes) {
    for (size_t i = 0; i + 2 < sub.indices.size(); i += 3) {
      uint32_t* t = &sub.indices[i];
      if (!lit(t)) continue;
      for (int c = 0; c < 3; ++c) {
        const uint32_t v = t[c];
        if (v >= moved.size()) continue;
        if (moved[v] == UINT32_MAX) moved[v] = plainVertex[v] ? copyVertex(v) : v;
        t[c] = moved[v];
      }
    }
  }
  // Each moved vertex goes along its ray from the axis to just short of the nearest shell
  // triangle beyond it (Moller-Trumbore); one on the axis takes its normal's direction.
  constexpr float kInset = 0.002f;
  for (uint32_t v = 0; v < moved.size(); ++v) {
    if (moved[v] == UINT32_MAX) continue;
    float* p = &rm.pos[3 * size_t(moved[v])];
    float dx = float(p[0] - cx), dz = float(p[2] - cz);
    float r = std::sqrt(dx * dx + dz * dz);
    if (r < 1e-4f) {
      dx = rm.normal[3 * size_t(moved[v])], dz = rm.normal[3 * size_t(moved[v]) + 2];
      const float m = std::sqrt(dx * dx + dz * dz);
      if (m < 1e-4f) continue;
      dx /= m, dz /= m;
    } else {
      dx /= r, dz /= r;
    }
    const float o[3] = {float(cx), p[1], float(cz)}, d[3] = {dx, 0.f, dz};
    float best = INFINITY;
    for (const auto& s : shell) {
      const float e1[3] = {s[3] - s[0], s[4] - s[1], s[5] - s[2]}, e2[3] = {s[6] - s[0], s[7] - s[1], s[8] - s[2]};
      const float q[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
      const float det = e1[0] * q[0] + e1[1] * q[1] + e1[2] * q[2];
      if (std::fabs(det) < 1e-9f) continue;
      const float tv[3] = {o[0] - s[0], o[1] - s[1], o[2] - s[2]};
      const float a = (tv[0] * q[0] + tv[1] * q[1] + tv[2] * q[2]) / det;
      if (a < 0.f || a > 1.f) continue;
      const float w[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
      const float b = (d[0] * w[0] + d[1] * w[1] + d[2] * w[2]) / det;
      if (b < 0.f || a + b > 1.f) continue;
      const float h = (e2[0] * w[0] + e2[1] * w[1] + e2[2] * w[2]) / det;
      if (h > r && h < best) best = h;
    }
    if (best == INFINITY) continue;
    p[0] = o[0] + dx * (best - kInset);
    p[2] = o[2] + dz * (best - kInset);
  }
}

bool FitCannon(const PortDread::ReskinResult& body, const float muzzle[3], float backY, PortDread::ReskinResult& out,
               float& scale, std::string* error) {
  out = {};
  const int elbow = body.skeleton.Find("R_elbow");
  if (elbow < 0) {
    return Fail(error, "cannon: no R_elbow");
  }
  std::vector<const PortDread::ReskinMesh*> cannon;
  double centre[3] = {0, 0, 0};
  size_t total = 0;
  for (const PortDread::ReskinMesh& rm : body.meshes) {
    std::string lower = rm.name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.find("cannon") != std::string::npos && rm.pos.size() == size_t(rm.vertex_count) * 3) {
      cannon.push_back(&rm);
      for (uint32_t v = 0; v < rm.vertex_count; ++v, ++total) {
        for (int a = 0; a < 3; ++a) {
          centre[a] += rm.pos[size_t(v) * 3 + a];
        }
      }
    }
  }
  auto fail = [&] {
    std::string names;
    for (const PortDread::ReskinMesh& rm : body.meshes) {
      names += " " + rm.name;
    }
    return Fail(error, "cannon: no cannon meshes past the elbow (meshes:" + names + ")");
  };
  if (total == 0) {
    return fail();
  }
  for (double& c : centre) {
    c /= double(total);
  }
  // The barrel's axis is the cannon's longest principal axis (power iteration on
  // the vertex covariance), pointing away from the elbow. The bind pose angles
  // the forearm forward, so the X axis alone leaves the cannon rolled and skewed.
  double cov[3][3] = {};
  for (const PortDread::ReskinMesh* rm : cannon) {
    for (uint32_t v = 0; v < rm->vertex_count; ++v) {
      double d[3];
      for (int a = 0; a < 3; ++a) {
        d[a] = rm->pos[size_t(v) * 3 + a] - centre[a];
      }
      for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
          cov[a][b] += d[a] * d[b];
        }
      }
    }
  }
  const float* head = body.skeleton.joints[size_t(elbow)].head;
  double axis[3] = {centre[0] - head[0], centre[1] - head[1], centre[2] - head[2]};
  for (int it = 0; it < 64; ++it) {
    double next[3] = {0, 0, 0};
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) {
        next[a] += cov[a][b] * axis[b];
      }
    }
    const double len = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
    if (len <= 0.0) {
      return fail();
    }
    for (int a = 0; a < 3; ++a) {
      axis[a] = next[a] / len;
    }
  }
  if (axis[0] * (centre[0] - head[0]) + axis[1] * (centre[1] - head[1]) + axis[2] * (centre[2] - head[2]) < 0.0) {
    for (double& a : axis) {
      a = -a;
    }
  }
  auto along = [&](const float* p) { return (p[0] - head[0]) * axis[0] + (p[1] - head[1]) * axis[1] + (p[2] - head[2]) * axis[2]; };
  double far = -1e30, back = 1e30;
  for (const PortDread::ReskinMesh* rm : cannon) {
    for (uint32_t v = 0; v < rm->vertex_count; ++v) {
      far = std::max(far, along(&rm->pos[size_t(v) * 3]));
      back = std::min(back, along(&rm->pos[size_t(v) * 3]));
    }
  }
  // The muzzle is the centre of the barrel's end: the mean of the vertices
  // within 2 cm of the far end.
  double sum[3] = {0, 0, 0};
  size_t count = 0;
  for (const PortDread::ReskinMesh* rm : cannon) {
    for (uint32_t v = 0; v < rm->vertex_count; ++v) {
      if (along(&rm->pos[size_t(v) * 3]) >= far - 0.02) {
        for (int a = 0; a < 3; ++a) {
          sum[a] += rm->pos[size_t(v) * 3 + a];
        }
        ++count;
      }
    }
  }
  float tip[3];
  for (int a = 0; a < 3; ++a) {
    tip[a] = float(sum[a] / double(count));
  }
  const double length = double(muzzle[1]) - backY;
  const double reach = along(tip) - back;
  if (along(tip) < 0.05 || reach < 0.05 || length <= 0.0) {
    return fail();
  }
  scale = float(length / reach);
  // Rotate the barrel onto +Y (the gun's forward). The bind pose holds the arm
  // palm down, which puts the glow strips on the cannon's top and bottom; Dread
  // and Prime hold it thumb up, glow on the sides. So the body's up, made square
  // to the barrel, goes to +X (the back of the hand faces out), and Z = X x Y.
  double up[3] = {-axis[2] * axis[0], -axis[2] * axis[1], 1.0 - axis[2] * axis[2]};
  const double upLen = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
  if (upLen <= 1e-6) {
    return fail();
  }
  for (double& u : up) {
    u /= upLen;
  }
  const double side[3] = {axis[1] * up[2] - axis[2] * up[1], axis[2] * up[0] - axis[0] * up[2],
                          axis[0] * up[1] - axis[1] * up[0]};
  auto turn = [&](const float* in, float* o) {
    const double x = in[0], y = in[1], z = in[2];
    o[0] = float(x * up[0] + y * up[1] + z * up[2]);
    o[1] = float(x * axis[0] + y * axis[1] + z * axis[2]);
    o[2] = -float(x * side[0] + y * side[1] + z * side[2]);
  };
  // The cannon shell is open at the elbow end, which faces the first-person
  // camera; retail closes it with the upper arm (kSuitArms). Add the body's arm
  // skin (triangles weighted at least half to R_shoulder's subtree), compacted
  // to the vertices those triangles use.
  const int shoulder = body.skeleton.Find("R_shoulder");
  if (shoulder < 0) {
    return Fail(error, "cannon: no R_shoulder");
  }
  std::vector<bool> arm(body.skeleton.joints.size(), false);
  for (size_t j = 0; j < body.skeleton.joints.size(); ++j) {
    for (int k = int(j), guard = 0; k >= 0 && guard < 256; ++guard) {
      if (k == shoulder) {
        arm[j] = true;
        break;
      }
      k = body.skeleton.IndexOfId(body.skeleton.joints[size_t(k)].parent);
    }
  }
  auto onArm = [&](const PortDread::ReskinMesh& rm, uint32_t v) {
    float w = 0.f;
    for (const auto& [bone, weight] : rm.weights[v]) {
      const int j = body.skeleton.IndexOfId(bone);
      if (j >= 0 && arm[size_t(j)]) {
        w += weight;
      }
    }
    return w >= 0.5f;
  };
  std::vector<PortDread::ReskinMesh> skin;
  for (const PortDread::ReskinMesh& rm : body.meshes) {
    if (std::find(cannon.begin(), cannon.end(), &rm) != cannon.end() ||
        rm.pos.size() != size_t(rm.vertex_count) * 3 || rm.weights.size() != rm.vertex_count) {
      continue;
    }
    std::vector<uint32_t> remap(rm.vertex_count, UINT32_MAX);
    PortDread::ReskinMesh part;
    part.name = rm.name;
    part.material_name = rm.material_name;
    part.bsmat_path = rm.bsmat_path;
    auto keep = [&](uint32_t v) {
      if (remap[v] == UINT32_MAX) {
        remap[v] = part.vertex_count++;
        part.pos.insert(part.pos.end(), &rm.pos[size_t(v) * 3], &rm.pos[size_t(v) * 3 + 3]);
        if (rm.normal.size() == rm.pos.size()) {
          part.normal.insert(part.normal.end(), &rm.normal[size_t(v) * 3], &rm.normal[size_t(v) * 3 + 3]);
        }
        if (rm.tangent.size() == size_t(rm.vertex_count) * 4) {
          part.tangent.insert(part.tangent.end(), &rm.tangent[size_t(v) * 4], &rm.tangent[size_t(v) * 4 + 4]);
        }
        for (int s = 0; s < 3; ++s) {
          if (rm.uv[s].size() == size_t(rm.vertex_count) * 2) {
            part.uv[s].insert(part.uv[s].end(), &rm.uv[s][size_t(v) * 2], &rm.uv[s][size_t(v) * 2 + 2]);
          }
        }
        part.weights.push_back(rm.weights[v]);
        if (rm.source.size() == rm.vertex_count) {
          part.source.push_back(rm.source[v]);
        }
      }
      return remap[v];
    };
    for (const PortDread::ReskinSubmesh& sub : rm.submeshes) {
      PortDread::ReskinSubmesh kept;
      for (size_t i = 0; i + 2 < sub.indices.size(); i += 3) {
        const uint32_t* tri = &sub.indices[i];
        if (tri[0] < rm.vertex_count && tri[1] < rm.vertex_count && tri[2] < rm.vertex_count &&
            onArm(rm, tri[0]) && onArm(rm, tri[1]) && onArm(rm, tri[2])) {
          for (int c = 0; c < 3; ++c) {
            kept.indices.push_back(keep(tri[c]));
          }
        }
      }
      if (!kept.indices.empty()) {
        part.submeshes.push_back(std::move(kept));
      }
    }
    if (!part.submeshes.empty()) {
      skin.push_back(std::move(part));
    }
  }
  std::vector<const PortDread::ReskinMesh*> parts = cannon;
  for (const PortDread::ReskinMesh& m : skin) {
    parts.push_back(&m);
  }
  out.skeleton = body.skeleton;
  for (const PortDread::ReskinMesh* rm : parts) {
    PortDread::ReskinMesh& m = out.meshes.emplace_back(*rm);
    for (uint32_t v = 0; v < m.vertex_count; ++v) {
      float d[3], r[3];
      for (int a = 0; a < 3; ++a) {
        d[a] = rm->pos[size_t(v) * 3 + a] - tip[a];
      }
      turn(d, r);
      for (int a = 0; a < 3; ++a) {
        m.pos[size_t(v) * 3 + a] = scale * r[a];
      }
      if (m.normal.size() == m.pos.size()) {
        turn(&rm->normal[size_t(v) * 3], &m.normal[size_t(v) * 3]);
      }
      if (m.tangent.size() == size_t(m.vertex_count) * 4) {
        turn(&rm->tangent[size_t(v) * 4], &m.tangent[size_t(v) * 4]);
      }
    }
  }
  return true;
}

bool FirstTextureOfSets(const std::vector<uint8_t>& m, std::vector<uint32_t>& out, std::string* error) {
  out.clear();
  if (m.size() < 0x2c || Be32(m, 0) != 0xDEADBABE) {
    return Fail(error, "not a CMDL");
  }
  const uint32_t nsec = Be32(m, 0x24), nmat = Be32(m, 0x28);
  if (nmat >= nsec || 0x2c + 4ull * nsec > m.size()) {
    return Fail(error, "bad section table");
  }
  size_t o = (0x2c + 4 * size_t(nsec) + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nmat; ++i) {
    const size_t size = Be32(m, 0x2c + 4 * size_t(i));
    if (o + size > m.size() || size < 8) {
      return Fail(error, "material set past the end");
    }
    // u32 textures, ids; u32 materials, end offsets; then the materials: u32 flags, u32 count,
    // texture indices.
    const size_t ntex = Be32(m, o);
    const size_t at = o + 4 + 4 * ntex;
    if (at + 4 > o + size) {
      return Fail(error, "material set cut short");
    }
    const size_t first = at + 4 + 4 * size_t(Be32(m, at));
    if (Be32(m, at) == 0 || first + 12 > o + size || Be32(m, first + 4) == 0 || Be32(m, first + 8) >= ntex) {
      return Fail(error, "material set " + std::to_string(i) + ": material 0 has no texture");
    }
    out.push_back(Be32(m, o + 4 + 4 * size_t(Be32(m, first + 8))));
    o += size;
  }
  return !out.empty() ? true : Fail(error, "no material sets");
}

bool BeamTint(const PortRemastered::Image& image, float out[3]) {
  double sum[3] = {0, 0, 0};
  for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    const double c[3] = {image.rgba[i] / 255.0, image.rgba[i + 1] / 255.0, image.rgba[i + 2] / 255.0};
    const double w = std::pow(std::max({c[0], c[1], c[2]}), 4.0);
    for (int a = 0; a < 3; ++a) {
      sum[a] += w * c[a];
    }
  }
  const double peak = std::max({sum[0], sum[1], sum[2]});
  if (!(peak > 0.0)) {
    return false;
  }
  for (int a = 0; a < 3; ++a) {
    out[a] = float(sum[a] / peak);
  }
  return true;
}

bool FitBall(const PortDread::Bcmdl& source, const std::vector<uint8_t>& retailCmdl,
             uint32_t joint, PortDread::ReskinResult& out, std::string* error) {
  out = {};
  if (retailCmdl.size() < 44 || Be32(retailCmdl, 0) != 0xDEADBABE)
    return Fail(error, "ball fit: bad retail CMDL");
  const PortDread::BcmdlMesh* shell = nullptr;
  for (const auto& mesh : source.meshes) {
    if (!mesh.enabled) continue;
    if (shell) return Fail(error, "ball fit: more than one enabled shell");
    shell = &mesh;
  }
  if (!shell || shell->vertices.count == 0 || shell->submeshes.empty() ||
      shell->vertices.normal.size() != size_t(shell->vertices.count) * 3)
    return Fail(error, "ball fit: missing shell or normals");
  std::vector<PortDread::Mat4> world;
  if (!PortDread::ComputeWorldBind(source, world, error)) return false;
  PortDread::ReskinMesh mesh;
  mesh.name = shell->name;
  mesh.bsmat_path = shell->bsmat_path;
  mesh.vertex_count = shell->vertices.count;
  mesh.normal = shell->vertices.normal;
  mesh.tangent = shell->vertices.tangent;
  for (int i = 0; i < 3; ++i) mesh.uv[i] = shell->vertices.uv[i];
  for (const auto& sub : shell->submeshes) {
    std::vector<float> positions;
    if (!PortDread::BindPoseModelPositions(source, world, *shell, sub, positions, error)) return false;
    if (!mesh.pos.empty() && positions != mesh.pos)
      return Fail(error, "ball fit: submeshes disagree on bind positions");
    mesh.pos = std::move(positions);
    if (uint64_t(sub.first_index) + sub.index_count > shell->indices.size() || sub.index_count % 3)
      return Fail(error, "ball fit: bad triangle range");
    auto& triangles = mesh.submeshes.emplace_back().indices;
    triangles.assign(shell->indices.begin() + sub.first_index,
                     shell->indices.begin() + sub.first_index + sub.index_count);
  }
  // Rigid bone-local shells need their bind rotation applied to normals/tangents too.
  for (const auto& sub : shell->submeshes) {
    if (sub.skin_type != shell->submeshes.front().skin_type ||
        sub.joint_map != shell->submeshes.front().joint_map)
      return Fail(error, "ball fit: inconsistent normal spaces");
  }
  const auto& first = shell->submeshes.front();
  if (first.skin_type == 1) {
    const auto& vertices = shell->vertices;
    if (vertices.weights.size() != size_t(mesh.vertex_count) * 4 || vertices.joints.size() != vertices.weights.size())
      return Fail(error, "ball fit: missing rigid source weights");
    for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
      int bind = -1;
      for (int k = 0; k < 4; ++k) {
        const size_t i = size_t(v) * 4 + k;
        if (vertices.weights[i] == 0) continue;
        const int j = vertices.joints[i];
        if (bind != -1 || std::fabs(vertices.weights[i] - 1.f) > 1e-5f || j < 0 || size_t(j) >= first.joint_map.size())
          return Fail(error, "ball fit: shell is not rigid");
        bind = first.joint_map[j];
      }
      if (bind < 0 || size_t(bind) >= world.size()) return Fail(error, "ball fit: bad source joint");
      const auto& matrix = world[bind];
      for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
        float dot = 0;
        for (int r = 0; r < 3; ++r) dot += matrix.m[r][a] * matrix.m[r][b];
        if (std::fabs(dot - (a == b ? 1.f : 0.f)) > 1e-3f)
          return Fail(error, "ball fit: non-rigid bind transform");
      }
      auto normal = [&](float* n) {
        const float old[3] = {n[0],n[1],n[2]};
        for (int a = 0; a < 3; ++a) {
          n[a] = 0;
          for (int b = 0; b < 3; ++b) n[a] += matrix.m[a][b] * old[b];
        }
      };
      normal(&mesh.normal[size_t(v)*3]);
      if (mesh.tangent.size() == size_t(mesh.vertex_count)*4) normal(&mesh.tangent[size_t(v)*4]);
    }
  } else if (first.skin_type != 2) {
    return Fail(error, "ball fit: unsupported shell normal space");
  }
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t v = 0; v < mesh.pos.size(); v += 3) {
    for (int a = 0; a < 3; ++a) {
      if (!std::isfinite(mesh.pos[v + a])) return Fail(error, "ball fit: non-finite position");
      lo[a] = std::min(lo[a], mesh.pos[v + a]);
      hi[a] = std::max(hi[a], mesh.pos[v + a]);
    }
  }
  float sourceSize = 0, targetSize = 0, center[3];
  for (int a = 0; a < 3; ++a) {
    const float l = BeF(retailCmdl, 12 + 4 * a), h = BeF(retailCmdl, 24 + 4 * a);
    if (!std::isfinite(l) || !std::isfinite(h) || h <= l)
      return Fail(error, "ball fit: bad retail bounds");
    center[a] = (l + h) * 0.5f;
    sourceSize = std::max(sourceSize, hi[a] - lo[a]);
    targetSize = std::max(targetSize, h - l);
  }
  if (!(sourceSize > 0)) return Fail(error, "ball fit: degenerate shell");
  out.scale = targetSize / sourceSize;
  // Dread Y-up to Prime Z-up, same proper rotation as the converter's default.
  auto rotate = [](float* p) { const float x = p[0], y = p[1]; p[0] = -x; p[1] = p[2]; p[2] = y; };
  for (size_t v = 0; v < mesh.pos.size(); v += 3) {
    for (int a = 0; a < 3; ++a) mesh.pos[v + a] = (mesh.pos[v + a] - (lo[a] + hi[a]) * 0.5f) * out.scale;
    rotate(&mesh.pos[v]);
    for (int a = 0; a < 3; ++a) mesh.pos[v + a] += center[a];
    rotate(&mesh.normal[v]);
    if (mesh.tangent.size() == size_t(mesh.vertex_count) * 4) rotate(&mesh.tangent[v / 3 * 4]);
  }
  mesh.weights.assign(mesh.vertex_count, {{joint, 1.f}});
  out.meshes.push_back(std::move(mesh));
  return true;
}

bool Import(const Io& io, Result& result, std::string& error) {
  result = {};
  auto log = [&](const std::string& line) {
    if (io.log) {
      io.log(line);
    }
  };
  auto cancelled = [&] { return io.cancelled && io.cancelled(); };

  std::vector<uint8_t> pkgData;
  if (!io.dread("packs/players/samus.pkg", pkgData, error)) {
    error = "cannot read packs/players/samus.pkg: " + error;
    return false;
  }
  PortDread::Pkg pkg;
  if (!pkg.Parse(pkgData.data(), pkgData.size(), error, pkgData.size())) {
    error = "samus.pkg: " + error;
    return false;
  }
  auto pkgFile = [&](const std::string& path, std::vector<uint8_t>& out) {
    const PortDread::PkgEntry* e = pkg.Find(path);
    if (e == nullptr || e->end > pkgData.size() || e->start > e->end) {
      return false;
    }
    out.assign(pkgData.begin() + e->start, pkgData.begin() + e->end);
    return true;
  };

  // Suit materials, one per BSMAT path; null where the BSMAT does not build (not mp_samuspbr,
  // a texture missing): its meshes are left out.
  std::map<std::string, std::unique_ptr<PortDread::SuitMaterial>> materials;
  auto readSuit = [&](const std::string& bsmatPath, PortDread::Bsmat& bsmat, PortDread::Bctex tex[3],
                      std::string& why) {
    std::vector<uint8_t> data;
    std::string bc, at, nm;
    if (!pkgFile(bsmatPath, data)) {
      why = "not in samus.pkg";
      return false;
    }
    if (!PortDread::ParseBsmat(data.data(), data.size(), bsmat, &why) ||
        !PortDread::SuitTexturePaths(bsmat, "textures", bc, at, nm, &why)) {
      return false;
    }
    const std::string* paths[3] = {&bc, &at, &nm};
    for (int i = 0; i < 3; ++i) {
      std::vector<uint8_t> bytes;
      std::string readError;
      if (!io.dread(*paths[i], bytes, readError)) {
        why = "cannot read " + *paths[i] + ": " + readError;
        return false;
      }
      if (!tex[i].Parse(bytes.data(), bytes.size(), readError)) {
        why = *paths[i] + ": " + readError;
        return false;
      }
    }
    return true;
  };
  auto material = [&](const std::string& bsmatPath) -> const PortDread::SuitMaterial* {
    auto found = materials.find(bsmatPath);
    if (found != materials.end()) {
      return found->second.get();
    }
    std::unique_ptr<PortDread::SuitMaterial> built;
    std::string why;
    PortDread::Bsmat bsmat;
    PortDread::Bctex tex[3];
    if (readSuit(bsmatPath, bsmat, tex, why)) {
      built = std::make_unique<PortDread::SuitMaterial>();
      if (!PortDread::BuildSuitMaterial(bsmat, tex[0], tex[1], tex[2], *built, &why)) {
        built.reset();
      }
    }
    if (!built) {
      log("dread: material " + bsmatPath + " left out: " + why);
    }
    return (materials[bsmatPath] = std::move(built)).get();
  };
  // One copy of a material per beam, under "<path>#beam<N>": the glow takes the beam's colour, all
  // normalised by one strength so only the ICAN texture differs between the copies. Empty when
  // the material does not build or does not glow.
  auto beamMaterials = [&](const std::string& bsmatPath, const float (*tints)[3]) {
    std::vector<const PortDread::SuitMaterial*> out;
    auto key = [&](uint32_t b) { return "#beam" + std::to_string(b); };
    if (materials.count(bsmatPath + key(0))) {
      for (uint32_t b = 0; b < kBeams; ++b) {
        out.push_back(materials[bsmatPath + key(b)].get());
      }
      return out[0] != nullptr ? out : std::vector<const PortDread::SuitMaterial*>{};
    }
    PortDread::Bsmat bsmat;
    PortDread::Bctex tex[3];
    std::string why;
    // The beam textures average to pale colours (Ice is nearly white), and a bright pale light
    // tone-maps to white. Taking out the grey (the smallest channel) keeps each colour's hue at full
    // saturation, so the lights stay coloured however bright they are.
    float pure[kBeams][3];
    for (uint32_t b = 0; b < kBeams; ++b) {
      const float lo = std::min({tints[b][0], tints[b][1], tints[b][2]});
      const float hi = std::max({tints[b][0], tints[b][1], tints[b][2]});
      for (int k = 0; k < 3; ++k) {
        pure[b][k] = hi > lo ? (tints[b][k] - lo) / (hi - lo) : 1.f;
      }
    }
    double strength = 0.0;
    bool ok = readSuit(bsmatPath, bsmat, tex, why);
    for (uint32_t b = 0; ok && b < kBeams; ++b) {
      double c[3];
      PortDread::SuitEmissive(bsmat, pure[b], c);
      strength = std::max({strength, c[0], c[1], c[2]});
    }
    if (ok && !(strength > 0.0)) {
      ok = false, why = "it does not glow";
    }
    // The cannon lights' own strength (11) blows the bloom out over the cannon; a peak of 4 still
    // reads as a bright light.
    constexpr double kBeamGlowPeak = 4.0;
    const double scale = strength > kBeamGlowPeak ? kBeamGlowPeak / strength : 1.0;
    strength *= scale;
    for (uint32_t b = 0; ok && b < kBeams; ++b) {
      auto built = std::make_unique<PortDread::SuitMaterial>();
      PortDread::SuitOptions options;
      const float tint[3] = {float(pure[b][0] * scale), float(pure[b][1] * scale), float(pure[b][2] * scale)};
      options.emissiveColor = tint;
      options.strength = strength;
      options.icanKey = key(b);
      ok = PortDread::BuildSuitMaterial(bsmat, tex[0], tex[1], tex[2], *built, &why, options);
      out.push_back(built.get());
      materials[bsmatPath + key(b)] = ok ? std::move(built) : nullptr;
    }
    if (!ok) {
      log("dread: beam glow of " + bsmatPath + " left out: " + why);
      materials[bsmatPath + key(0)] = nullptr;
      out.clear();
    }
    return out;
  };

  PortRemastered::ConvertIO cio;
  cio.retail = io.retail;
  std::set<uint32_t> reserved;
  for (const auto& t : kTargets) reserved.insert(DreadCinf(t.cmdl));
  std::map<std::string, std::vector<uint8_t>> pending;
  cio.retailId = [&](uint32_t id) { return reserved.count(id) || (io.retailId && io.retailId(id)); };
  cio.texture = [&](const PortRemastered::ModelUuid& id, PortRemastered::Image& out, std::string& why) {
    for (const auto& [path, m] : materials) {
      if (m) {
        for (const PortDread::SuitImage& si : m->images) {
          if (si.id == id) {
            out = si.image;
            return true;
          }
        }
      }
    }
    why = "no such suit texture";
    return false;
  };
  cio.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    pending[name] = data;
    // A CMDL does not name itself, so the alias is the same bytes.
    if (name == Hex8(kPhazonCutsceneBody) + ".CMDL") {
      const std::string alias = Hex8(kPhazonCutsceneAlias) + ".CMDL";
      pending[alias] = data;
    }
    return true;
  };
  cio.log = [&](const std::string& line) {
    if (io.detail) {
      io.detail("dread: " + line);
    } else {
      log("dread: " + line);
    }
  };
  PortRemastered::Converter converter(cio);

  std::map<std::string, PortDread::Bcmdl> models;
  std::map<uint32_t, PortDread::PrimeSkeleton> skeletons;
  std::map<uint32_t, std::vector<uint8_t>> cinfData;
  const size_t bodyTotal = std::size(kTargets);
  const size_t total = bodyTotal + std::size(kCannonTargets) + std::size(kBallTargets);
  std::map<std::string, PortDread::ReskinResult> cutsceneBodies;  // by suit, for the cannons
  for (size_t ti = 0; ti < bodyTotal; ++ti) {
    const Target& t = kTargets[ti];
    if (io.progress) {
      io.progress(ti, total);
    }
    if (cancelled()) {
      error = "cancelled";
      return false;
    }
    const std::string label = std::string(t.suit) + " " + t.skeleton + " -> " + Hex8(t.cmdl);
    auto failTarget = [&](const std::string& why) {
      log("dread: " + label + " failed: " + why);
      ++result.failed;
    };
    // The Dread model, parsed once per suit.
    auto model = models.find(t.suit);
    if (model == models.end()) {
      const std::string path = std::string("actors/characters/samus/models/") + t.suit + ".bcmdl";
      std::vector<uint8_t> data;
      PortDread::Bcmdl parsed;
      std::string why;
      if (!pkgFile(path, data)) {
        failTarget(path + " is not in samus.pkg");
        continue;
      }
      if (!PortDread::ParseBcmdl(data.data(), data.size(), parsed, &why)) {
        failTarget(path + ": " + why);
        continue;
      }
      model = models.emplace(t.suit, std::move(parsed)).first;
    }
    auto skeleton = skeletons.find(t.cinf);
    if (skeleton == skeletons.end()) {
      std::vector<uint8_t> data;
      PortDread::PrimeSkeleton parsed;
      std::string why;
      if (!io.retail(FourCC('C', 'I', 'N', 'F'), t.cinf, data)) {
        failTarget("CINF " + Hex8(t.cinf) + " is not on the disc");
        continue;
      }
      if (!PortDread::ParseCinf(data.data(), data.size(), parsed, &why)) {
        failTarget("CINF " + Hex8(t.cinf) + ": " + why);
        continue;
      }
      skeleton = skeletons.emplace(t.cinf, std::move(parsed)).first;
      cinfData[t.cinf] = std::move(data);
    }
    std::vector<uint8_t> retailCmdl, retailCskr;
    PortDread::RetailSkinRef retailSkin;
    std::string why;
    if (!io.retail(FourCC('C', 'M', 'D', 'L'), t.cmdl, retailCmdl) ||
        !io.retail(FourCC('C', 'S', 'K', 'R'), t.cskr[0], retailCskr)) {
      failTarget("the retail body is not on the disc");
      continue;
    }
    const bool haveVotes = RetailSkinFromCmdl(retailCmdl, retailCskr, retailSkin, &why);
    if (!haveVotes) {
      log("dread: " + label + ": no retail votes (" + why + "), table weights only");
    }
    PortDread::ReskinOptions ro;
    ro.retail = haveVotes ? &retailSkin : nullptr;
    ro.repose = false;
    PortDread::ReskinResult reskin;
    if (!PortDread::ReskinToPrime(model->second, skeleton->second, ro, reskin, &why)) {
      failTarget("re-skin: " + why);
      continue;
    }
    // The mesh keeps Dread's proportions; the skeleton it is bound to ships as its own CINF.
    const uint32_t dreadCinf = DreadCinf(t.cmdl);
    std::vector<uint8_t> patchedCinf;
    if (dreadCinf == 0 || (io.retailId && io.retailId(dreadCinf))) {
      failTarget("CINF id " + Hex8(dreadCinf) + " is taken");
      continue;
    }
    const std::vector<uint8_t>& baseCinf = cinfData[t.cinf];
    if (!PortDread::PatchCinfHeads(baseCinf.data(), baseCinf.size(), reskin.skeleton, patchedCinf, &why)) {
      failTarget("CINF " + Hex8(dreadCinf) + ": " + why);
      continue;
    }
    for (const std::string& line : reskin.log) {
      log("dread: " + label + ": " + line);
    }
    std::vector<const PortDread::SuitMaterial*> meshMats;
    for (const PortDread::ReskinMesh& rm : reskin.meshes) {
      meshMats.push_back(rm.bsmat_path.empty() ? nullptr : material(rm.bsmat_path));
      if (meshMats.back() == nullptr) {
        log("dread: " + label + ": mesh " + rm.name + " left out (no suit material)");
      }
    }
    PortRemastered::Model built;
    PortRemastered::ConvertOptions opt;
    if (!BuildModel(reskin, meshMats, built, opt.boneWeights, &why)) {
      failTarget(why);
      continue;
    }
    opt.retail = t.cmdl;
    opt.source = std::string("dread ") + t.suit;
    for (uint32_t s : t.cskr) {
      if (s != 0) {
        opt.skins.push_back(s);
      }
    }
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        opt.orient[r][c] = r == c ? 1.0 : 0.0;
      }
      opt.offset[r] = 0.0;
    }
    opt.skip.clear();
    opt.texturePrefix = "dread:";
    if (!converter.Convert(built, opt, why)) {
      failTarget("convert: " + why);
      continue;
    }
    if (!cio.write(Hex8(dreadCinf) + ".CINF", patchedCinf)) {
      failTarget("could not write CINF " + Hex8(dreadCinf));
      continue;
    }
    ++result.converted;
    log("dread: " + label + ": " + std::to_string(built.meshes.size()) + " meshes, " +
        std::to_string(built.materials.size()) + " materials, CINF " + Hex8(dreadCinf) + " (" +
        std::to_string(reskin.skeleton_mapped) + " joints from Dread)");

    // Where the Dread cannon's tip sits against the gun locators, carried at Prime's offsets (step 12).
    if (!result.measured && std::strcmp(t.skeleton, "cine50") == 0) {
      const int gun = reskin.skeleton.Find("GUN_LCTR");
      const int charge = reskin.skeleton.Find("Charge_LCTR");
      float maxX = -1e30f;
      for (const PortDread::ReskinMesh& rm : reskin.meshes) {
        if (rm.name.find("Cannon") != std::string::npos) {
          for (uint32_t v = 0; v < rm.vertex_count; ++v) {
            maxX = std::max(maxX, rm.pos[size_t(v) * 3]);
          }
        }
      }
      double sum[3] = {0, 0, 0};
      size_t count = 0;
      for (const PortDread::ReskinMesh& rm : reskin.meshes) {
        if (rm.name.find("Cannon") != std::string::npos) {
          for (uint32_t v = 0; v < rm.vertex_count; ++v) {
            if (rm.pos[size_t(v) * 3] >= maxX - 0.02f) {
              for (int a = 0; a < 3; ++a) {
                sum[a] += rm.pos[size_t(v) * 3 + a];
              }
              ++count;
            }
          }
        }
      }
      if (gun >= 0 && charge >= 0 && count > 0) {
        result.measured = true;
        for (int a = 0; a < 3; ++a) {
          result.cannonTip[a] = float(sum[a] / double(count));
          result.tipFromGun[a] = result.cannonTip[a] - reskin.skeleton.joints[size_t(gun)].head[a];
          result.tipFromCharge[a] = result.cannonTip[a] - reskin.skeleton.joints[size_t(charge)].head[a];
        }
        char line[256];
        std::snprintf(line, sizeof(line),
                      "dread: cannon tip (%.3f %.3f %.3f): GUN_LCTR %+.3f %+.3f %+.3f, Charge_LCTR %+.3f %+.3f %+.3f",
                      result.cannonTip[0], result.cannonTip[1], result.cannonTip[2], result.tipFromGun[0],
                      result.tipFromGun[1], result.tipFromGun[2], result.tipFromCharge[0], result.tipFromCharge[1],
                      result.tipFromCharge[2]);
        log(line);
      }
    }
    if (std::strcmp(t.skeleton, "cine50") == 0) {
      cutsceneBodies[t.suit] = std::move(reskin);
    }
  }
  if (result.failed != 0 || result.converted != bodyTotal) {
    error = "incomplete Dread bodies (the notes say why); no output published";
    return false;
  }

  // The first-person cannons, one material set per beam like the gun model's: each beam's colour
  // is read from that set of the gun model, and only the cannon's glow takes it.
  float tints[kBeams][3];
  float muzzle[3];
  float gunBack = 0.f;
  {
    std::vector<uint8_t> gun, cinf;
    std::vector<uint32_t> textures;
    PortDread::PrimeSkeleton skeleton;
    if (!io.retail(FourCC('C', 'M', 'D', 'L'), kGunModel, gun) ||
        !FirstTextureOfSets(gun, textures, &error) || textures.size() != kBeams) {
      return Fail(&error, "cannon: gun CMDL " + Hex8(kGunModel) + ": " + (error.empty() ? "not 5 sets" : error));
    }
    for (uint32_t b = 0; b < kBeams; ++b) {
      std::vector<uint8_t> txtr;
      PortRemastered::Image image;
      if (!io.retail(FourCC('T', 'X', 'T', 'R'), textures[b], txtr) || !PortBallLooks::DecodeCmprTxtr(txtr, image) ||
          !BeamTint(image, tints[b])) {
        return Fail(&error, "cannon: beam texture " + Hex8(textures[b]) + " is missing or not CMPR");
      }
      char line[128];
      std::snprintf(line, sizeof(line), "dread: beam %u colour %.3f %.3f %.3f (TXTR %s)", b, tints[b][0], tints[b][1],
                    tints[b][2], Hex8(textures[b]).c_str());
      log(line);
    }
    if (!io.retail(FourCC('C', 'I', 'N', 'F'), kGunCinf, cinf) ||
        !PortDread::ParseCinf(cinf.data(), cinf.size(), skeleton, &error) || skeleton.Find("LBEAM") < 0) {
      return Fail(&error, "cannon: gun CINF " + Hex8(kGunCinf) + " has no LBEAM " + error);
    }
    std::copy_n(skeleton.joints[size_t(skeleton.Find("LBEAM"))].head, 3, muzzle);
    // CMDL header: magic, version, flags, then the bounds (min xyz, max xyz), big-endian.
    if (gun.size() < 0x24) {
      return Fail(&error, "cannon: gun CMDL " + Hex8(kGunModel) + " is truncated");
    }
    const uint32_t bits = uint32_t(gun[0x10]) << 24 | uint32_t(gun[0x11]) << 16 | uint32_t(gun[0x12]) << 8 | gun[0x13];
    std::memcpy(&gunBack, &bits, sizeof(gunBack));
  }
  for (const CannonTarget& t : kCannonTargets) {
    if (io.progress) io.progress(result.converted, total);
    if (cancelled()) return Fail(&error, "cancelled");
    const std::string label = std::string(t.suit) + " first-person cannon (suit " + std::to_string(t.primeSuit) + ")";
    const uint32_t id = FirstPersonCannon(t.primeSuit);
    if (id == 0 || (io.retailId && io.retailId(id)) || !reserved.insert(id).second)
      return Fail(&error, label + ": output id collision " + Hex8(id));
    for (const auto& [name, data] : pending) {
      if (name.compare(0, 8, Hex8(id)) == 0)
        return Fail(&error, label + ": generated resource id collision " + Hex8(id));
    }
    const auto body = cutsceneBodies.find(t.suit);
    PortDread::ReskinResult fit;
    float scale = 0.f;
    if (body == cutsceneBodies.end() || !FitCannon(body->second, muzzle, gunBack, fit, scale, &error))
      return Fail(&error, label + ": " + (error.empty() ? "no cutscene body" : error));
    std::vector<const PortDread::SuitMaterial*> mats;
    PortRemastered::ConvertOptions opt;
    opt.setTextures.resize(kBeams);
    for (PortDread::ReskinMesh& rm : fit.meshes) {
      const PortDread::SuitMaterial* plain = rm.bsmat_path.empty() ? nullptr : material(rm.bsmat_path);
      const auto beams = rm.bsmat_path.find("cannon") != std::string::npos ? beamMaterials(rm.bsmat_path, tints)
                                                                          : std::vector<const PortDread::SuitMaterial*>{};
      mats.push_back(beams.empty() ? plain : beams[0]);
      if (!beams.empty()) {
        const PortRemastered::ModelUuid icanId = PortDread::SuitTextureId(beams[0]->name + "#beam0", PortDread::kSuitIcan);
        for (const PortDread::SuitImage& si : beams[0]->images) {
          if (si.id == icanId) PushGlowToShell(rm, si.image);
        }
      }
      for (uint32_t b = 1; b < beams.size(); ++b) {
        opt.setTextures[b][PortDread::SuitTextureId(beams[0]->name + "#beam0", PortDread::kSuitIcan)] =
            PortDread::SuitTextureId(beams[b]->name + "#beam" + std::to_string(b), PortDread::kSuitIcan);
      }
      if (mats.back() == nullptr) {
        log("dread: " + label + ": mesh " + rm.name + " left out (no suit material)");
      }
    }
    PortRemastered::Model built;
    if (!BuildModel(fit, mats, built, opt.boneWeights, &error)) return Fail(&error, label + ": " + error);
    built.skinned = false;
    opt.boneWeights.clear();
    opt.retail = kSuitArms[t.primeSuit];
    opt.outputModel = id;
    opt.materialSets = kBeams;
    opt.texturePrefix = "dread:";
    opt.source = "dread " + label;
    opt.skip.clear();
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) opt.orient[r][c] = r == c ? 1.0 : 0.0;
    if (!converter.Convert(built, opt, error)) return Fail(&error, label + ": convert: " + error);
    ++result.converted;
    char line[256];
    std::snprintf(line, sizeof(line), "dread: %s -> %s: %zu meshes, scale %.3f, bounds %.3f %.3f %.3f .. %.3f %.3f %.3f",
                  label.c_str(), Hex8(id).c_str(), built.meshes.size(), scale, built.boundsMin[0], built.boundsMin[1],
                  built.boundsMin[2], built.boundsMax[0], built.boundsMax[1], built.boundsMax[2]);
    log(line);
  }
  // Keep Prime's ANCS and animations intact, changing only character 0's model and skin.
  // All resources are buffered until every target succeeds; the installer additionally
  // publishes the staging directory atomically, so failed writes cannot replace a mod.
  for (const auto& t : kBallTargets) {
    if (io.progress) io.progress(result.converted, total);
    if (cancelled()) return Fail(&error, "cancelled");
    std::vector<uint8_t> ancs;
    if (!io.retail(FourCC('A','N','C','S'), t.ancs, ancs) || ancs.size() < 14 || Be32(ancs, 4) == 0)
      return Fail(&error, "ball: missing ANCS " + Hex8(t.ancs));
    size_t at = 14;
    while (at < ancs.size() && ancs[at]) ++at;
    ++at;
    if (at + 12 > ancs.size() || Be32(ancs, at) != t.cmdl)
      return Fail(&error, "ball: unexpected ANCS character model");
    const uint32_t skin = Be32(ancs, at + 4), cinf = Be32(ancs, at + 8);
    const uint32_t modelId = PortModelVariant::Id(t.cmdl, t.suit);
    const uint32_t skinId = PortModelVariant::Id(skin, t.suit);
    const uint32_t ancsId = PortModelVariant::Id(t.ancs, t.suit);
    const uint32_t lowId = PortModelVariant::Id(t.low, t.suit);
    for (uint32_t id : {modelId, skinId, ancsId, lowId}) {
      if (id == 0 || (io.retailId && io.retailId(id)) || !reserved.insert(id).second)
        return Fail(&error, "ball: output id collision " + Hex8(id));
      for (const auto& [name, data] : pending) {
        if (name.compare(0, 8, Hex8(id)) == 0)
          return Fail(&error, "ball: generated resource id collision " + Hex8(id));
      }
    }
    const std::string path = std::string("actors/characters/morphball/models/") + t.source + ".bcmdl";
    std::vector<uint8_t> bytes, retailCmdl, retailSkin, cinfBytes;
    PortDread::Bcmdl source;
    PortDread::PrimeSkeleton skeleton;
    PortDread::RetailSkinRef ref;
    if (!pkgFile(path, bytes) || !PortDread::ParseBcmdl(bytes.data(), bytes.size(), source, &error) ||
        !io.retail(FourCC('C','M','D','L'), t.cmdl, retailCmdl) ||
        !io.retail(FourCC('C','S','K','R'), skin, retailSkin) ||
        !io.retail(FourCC('C','I','N','F'), cinf, cinfBytes) ||
        !PortDread::ParseCinf(cinfBytes.data(), cinfBytes.size(), skeleton, &error) ||
        !RetailSkinFromCmdl(retailCmdl, retailSkin, ref, &error))
      return Fail(&error, "ball " + std::string(t.source) + ": missing or invalid model/skin/skeleton: " + error);
    std::map<uint32_t, double> votes;
    for (const auto& weights : ref.weights) for (const auto& [joint, weight] : weights) votes[joint] += weight;
    const auto rigid = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (rigid == votes.end() || skeleton.IndexOfId(rigid->first) < 0)
      return Fail(&error, "ball: no retail shell joint");
    PortDread::ReskinResult fit;
    if (!FitBall(source, retailCmdl, rigid->first, fit, &error)) return false;
    std::vector<const PortDread::SuitMaterial*> mats;
    for (const auto& mesh : fit.meshes) {
      mats.push_back(material(mesh.bsmat_path));
      if (!mats.back()) return Fail(&error, "ball: unsupported or missing shell material " + mesh.bsmat_path);
    }
    PortRemastered::Model built;
    PortRemastered::ConvertOptions opt;
    if (!BuildModel(fit, mats, built, opt.boneWeights, &error)) return false;
    opt.retail = t.cmdl;
    opt.outputModel = modelId;
    opt.skins = {skin};
    opt.outputSkins = {skinId};
    opt.texturePrefix = "dread:";
    opt.source = std::string("dread ") + t.source;
    opt.skip.clear();
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) opt.orient[r][c] = r == c ? 1.0 : 0.0;
    if (!converter.Convert(built, opt, error)) return false;
    auto put = [&](size_t offset, uint32_t value) {
      for (int i = 0; i < 4; ++i) ancs[offset + i] = uint8_t(value >> (24 - i * 8));
    };
    put(at, modelId);
    put(at + 4, skinId);
    if (!cio.write(Hex8(ancsId) + ".ANCS", ancs)) return Fail(&error, "ball: ANCS write failed");
    opt.retail = t.low;
    opt.outputModel = lowId;
    opt.skins.clear();
    opt.outputSkins.clear();
    opt.boneWeights.clear();
    built.skinned = false;
    if (!converter.Convert(built, opt, error)) return false;
    ++result.converted;
    log("dread: " + std::string(t.source) + (t.spider ? " Spider" : " normal") +
        ": measured fit " + std::to_string(fit.scale) + ", rigid joint " + std::to_string(rigid->first));
  }
  if (cancelled()) return Fail(&error, "cancelled");
  for (const auto& [name, bytes] : pending) {
    if (cancelled()) return Fail(&error, "cancelled; discard staging");
    if (!io.write(name, bytes)) return Fail(&error, "cannot write " + name + "; discard staging");
    result.files.push_back(name);
  }
  if (cancelled()) return Fail(&error, "cancelled; discard staging");
  if (io.progress) io.progress(total, total);
  return true;
}

}  // namespace PortDreadSamus
