#pragma once

#include "port_dread_bcmdl.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Re-skin of a Metroid Dread Samus body (.bcmdl, see port_dread_bcmdl.h) onto one of Prime's
// Samus skeletons (CINF): the Dread mesh is fitted into Prime's frame and units, reposed into
// Prime's bind pose with Dread's own skin, and its weights moved onto Prime bones, so that
// Prime's animations drive it. The result is plain arrays for the CMDL/CSKR writer (step 6 of
// the Dread Samus plan); nothing here touches game state.
//
// The two Prime skeletons: CINF 19C53902 (50 joints, the cutscene/third-person body) and
// A496DA02 (20 joints, CPlayer's morph-ball transitions). Both are a T-pose like Dread's, Z up,
// +Y forward, the character's left at -X, metres. The joint table that pairs Dread joints with
// Prime ones is in port_dread_reskin.cpp (kRepose, kWeights).
namespace PortDread {

constexpr uint32_t kPrimeNoParent = 0xFFFFFFFFu;

struct PrimeJoint {
  uint32_t id = 0;                 // CSegId, what CSKR weights and ANIM channels name
  uint32_t parent = kPrimeNoParent; // id; kPrimeNoParent when the parent is not in the file
  std::string name;
  float head[3] = {};              // bind position, model space (CINF bind rotations are identity)
};

struct PrimeSkeleton {
  std::vector<PrimeJoint> joints;  // file order
  // -1 when absent.
  int Find(const std::string& name) const;
  int IndexOfId(uint32_t id) const;
};

// Prime CINF (big-endian): u32 count, {u32 id, u32 parent, f32 head[3], u32 n, u32 links[n]},
// u32 count, u32 build order[], u32 count, {string name, u32 id}. Bounds-checked; returns false
// with *error on bad data (unknown parent ids are kept as kPrimeNoParent, the CINF root's parent
// is never listed).
bool ParseCinf(const uint8_t* data, size_t size, PrimeSkeleton& out, std::string* error);

// `cinf` (a CINF ParseCinf accepts) with each joint's head replaced by the head of the joint
// with the same id in `skeleton`; links, build order and names stay byte for byte. Fails when
// the file does not parse or a joint is missing from `skeleton`.
bool PatchCinfHeads(const uint8_t* cinf, size_t size, const PrimeSkeleton& skeleton, std::vector<uint8_t>& out,
                    std::string* error);

// Retail weights of the Prime body that matches the target skeleton (its CMDL positions and CSKR
// groups expanded per vertex), for the nearest-retail-vertex votes. Optional.
struct RetailSkinRef {
  std::vector<float> pos;                                         // 3 per vertex, bind space
  std::vector<std::vector<std::pair<uint32_t, float>>> weights;   // per vertex: (bone id, weight)
};

// Where a vertex's Prime weights came from.
enum class WeightSource : uint8_t {
  Table = 0,   // every weight through the joint table (directly or from a mapped ancestor)
  Vote = 1,    // every weight from retail votes
  Mixed = 2,   // some of each
};

struct ReskinSubmesh {
  std::vector<uint32_t> indices;   // triangle list into ReskinMesh vertices, CCW-outward
};

struct ReskinMesh {
  std::string name;
  std::string material_name;
  std::string bsmat_path;
  uint32_t vertex_count = 0;
  std::vector<float> pos;          // 3 per vertex, Prime bind pose (repose off: skeleton's), metres
  std::vector<float> normal;       // 3, unit
  std::vector<float> tangent;      // 4 (w = +-1); empty when the mesh has none
  std::vector<float> uv[3];        // 2 each; empty where the mesh lacks the set
  std::vector<std::vector<std::pair<uint32_t, float>>> weights; // per vertex: (bone id, w), sorted
                                   // by bone, <= kReskinMaxWeights, 1/64 steps, sum 1
  std::vector<WeightSource> source; // per vertex
  std::vector<ReskinSubmesh> submeshes;
};

constexpr size_t kReskinMaxWeights = 4; // = the converter's kMaxSkinWeights

struct ReskinStats {
  size_t joints = 0;            // Dread joints
  size_t weight_direct = 0;     // joints whose own table row names a Prime joint of this skeleton
  size_t weight_vote = 0;       // ... a vote row (retail votes; the table candidates without one)
  size_t weight_inherited = 0;  // joints that take their nearest mapped ancestor's
  size_t anchors = 0;           // joints reposed onto a Prime joint (pinned and/or aimed)
  size_t follows = 0;           // joints that follow another's translation
  size_t carried = 0;           // joints carried rigidly by their parent
  size_t posed = 0;             // joints ReskinOptions::mesh_pose moves (with their subtrees)
  size_t verts_table = 0, verts_vote = 0, verts_mixed = 0;
};

struct ReskinResult {
  std::vector<ReskinMesh> meshes;  // the Dread model's enabled meshes, in file order
  // Dread model space -> Prime space before the repose: p = scale * frame * d + offset.
  float scale = 1.f;
  float frame[3][3] = {};
  float offset[3] = {};
  // Per Dread joint: the Prime joint index (into PrimeSkeleton::joints) its weights go to,
  // -1 for a vote joint, and the repose transform (fitted Dread space -> Prime bind space).
  std::vector<int> weight_joint;
  std::vector<Mat4> repose;
  // Prime's skeleton (same ids, parents, names) at Dread's proportions: a joint kBind pairs
  // with a Dread joint takes that joint's fitted head; the others keep their Prime offset
  // from their parent. The bind of the meshes when ReskinOptions::repose is off.
  PrimeSkeleton skeleton;
  size_t skeleton_mapped = 0;      // joints placed on a Dread joint (the rest are carried)
  ReskinStats stats;
  std::vector<std::string> log;    // one line per notable decision (frame, scale, votes)
};

struct ReskinOptions {
  bool enabled_only = true;              // only meshes drawn by default (no FX, cine, missiles)
  const RetailSkinRef* retail = nullptr; // votes for the vote rows; table fallback without it
  // On: the meshes are reposed into Prime's bind pose (Prime's skeleton drives them). Off: they
  // stay at Dread's proportions, only fitted, bound to ReskinResult::skeleton (votes still
  // look up retail vertices from the reposed positions, which match the retail body).
  bool repose = true;
  // Joints posed for the mesh alone, matched by name: each one's pos/euler/scale replaces the
  // bind's local transform when the vertices are placed in Dread's model space (its children
  // follow), so the vertices weighted to it come out where that pose puts them. The fit, the
  // repose and the skeleton keep the bind.
  std::vector<BcmdlJoint> mesh_pose;
};

// Fit, repose and re-weight `model` onto `skeleton`. Fails (false, *error) when the skeleton or
// the model lacks the joints the fit needs (head, ankles/feet, shoulders/upper arms, pelvis) or
// a mesh cannot be skinned.
bool ReskinToPrime(const Bcmdl& model, const PrimeSkeleton& skeleton, const ReskinOptions& options,
                   ReskinResult& out, std::string* error);

} // namespace PortDread
