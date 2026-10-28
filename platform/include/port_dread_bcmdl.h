#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Reader for Metroid Dread model files (.bcmdl, magic "MMDL", v1.58) from the user's own
// copy of the game. Parses the skeleton, meshes, vertex/index buffers and submeshes into plain
// structs and applies no skinning (except the bind-pose helpers below). Layout notes:
// build/mpr/kb/format/dread-bcmdl.md. Every pointer and count in the file is bounds-checked
// against the span handed to ParseBcmdl; bad data returns false with a message.
namespace PortDread {

// Row-major 4x4, column-vector convention: p' = m * p, translation in m[0][3], m[1][3], m[2][3].
struct Mat4 {
  float m[4][4];
  static Mat4 Identity();
};
Mat4 operator*(const Mat4& a, const Mat4& b);
// p' = m * (p, 1)
void TransformPoint(const Mat4& m, const float p[3], float out[3]);

struct BcmdlJoint {
  std::string name;
  int parent = -1;        // index into Bcmdl::joints, -1 for a root
  float pos[3] = {};      // local to the parent
  float euler[3] = {};    // radians, applied as R = Rz * Ry * Rx (x, y, z = euler[0..2])
  float scale[3] = {1, 1, 1};
};

// Vertex streams of one mesh. A stream that the mesh lacks is empty. All arrays are
// vertex_count * N entries, interleaved per vertex.
struct BcmdlVertices {
  uint32_t count = 0;
  std::vector<float> pos;        // 3  (semantic 0)
  std::vector<float> normal;     // 3  (1)
  std::vector<float> uv[3];      // 2  (2, 3, 4)
  std::vector<float> color;      // 4  (5, FX meshes only)
  std::vector<int32_t> joints;   // 4  (6; stored as floats in the file) index into the SUBMESH jmap
  std::vector<float> weights;    // 4  (7)
  std::vector<float> tangent;    // 4  (8; w = +-1)
};

struct BcmdlSubmesh {
  uint32_t skin_type = 0;        // 2 = bind-pose model space, 1 = bone-local, 0 = rigid (unverified)
  uint32_t first_index = 0;      // into BcmdlMesh::indices
  uint32_t index_count = 0;      // triangle list
  std::vector<int32_t> joint_map; // BcmdlVertices::joints values index this; entries are joint indices
};

struct BcmdlMesh {
  std::string name;
  bool enabled = false;          // true = drawn by default (false: fx/cine/variant parts)
  std::string material_name;
  std::string bsmat_path;        // e.g. actors/characters/samus/models/imats/samuspower_powerbody.bsmat
  float transform[3] = {};       // offset ADDED to every vertex position
  BcmdlVertices vertices;
  std::vector<uint16_t> indices; // triangle list, CCW-outward
  std::vector<BcmdlSubmesh> submeshes;
};

struct Bcmdl {
  uint16_t version_major = 0;
  uint16_t version_minor = 0;
  std::vector<BcmdlJoint> joints; // parents precede children in the files seen, but not relied on
  std::vector<BcmdlMesh> meshes;

  // -1 when absent.
  int FindJoint(const std::string& name) const;
};

// Parse an MMDL v1.58 file held in memory. On failure returns false, sets *error (if non-null)
// and leaves `out` unspecified.
bool ParseBcmdl(const uint8_t* data, size_t size, Bcmdl& out, std::string* error);

// Local transform T * Rz * Ry * Rx * S of one joint.
Mat4 JointLocalMatrix(const BcmdlJoint& joint);

// World bind matrices (one per joint): the local matrices concatenated down the parent chain.
// Returns false (and sets *error) on a parent cycle or bad parent index.
bool ComputeWorldBind(const Bcmdl& model, std::vector<Mat4>& world, std::string* error);

// Bind-pose model-space position of every vertex of `mesh` as seen through `submesh`
// (out = vertex_count * 3 floats), given world bind matrices from ComputeWorldBind:
//   skin_type 2: p = pos + transform (the vertices are already in bind-pose model space)
//   skin_type 1: p = sum_k w[k] * worldBind[joint_map[joints[k]]] * (pos + transform)
//                (vertices are bone-local; weights with w == 0 are skipped)
// Vertices that no triangle of the submesh references are still computed the same way, so
// the joint indices of unused vertices must be valid too, or the call fails. Other skin types
// return false. BindPosePosition does a single vertex.
bool BindPoseModelPositions(const Bcmdl& model, const std::vector<Mat4>& world,
                            const BcmdlMesh& mesh, const BcmdlSubmesh& submesh,
                            std::vector<float>& out, std::string* error);
bool BindPosePosition(const Bcmdl& model, const std::vector<Mat4>& world, const BcmdlMesh& mesh,
                      const BcmdlSubmesh& submesh, uint32_t vertex, float out[3],
                      std::string* error);

// One joint's local transform at `frame` of a Dread skeletal animation (.bcskla, magic "MANM",
// v1.10; layout from mercury-engine-data-structures formats/bcskla.py). Tracks are keyed by the
// CRC-64 of the joint name, case kept; a track has nine channels (pos, euler, scale), each a
// constant or Hermite keys (time, value, slope). Fills pose.pos/euler/scale and sets found when
// the animation has a track for `joint`; leaves them alone otherwise. False (and *error) on
// bad data.
bool SampleBcskla(const uint8_t* data, size_t size, const std::string& joint, float frame, BcmdlJoint& pose,
                  bool& found, std::string* error);

} // namespace PortDread
