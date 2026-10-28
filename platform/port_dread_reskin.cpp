#include "port_dread_reskin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

// How the re-skin works (one pass per Dread model and Prime skeleton):
//
// 1. Fit. Dread is Y up, +X = the character's left, +Z forward, centimetres; Prime is Z up,
//    -X left, +Y forward, metres. Rather than assume either, the frame is built from the joints
//    of both skeletons (up = feet -> head, left = right shoulder -> left shoulder, forward =
//    left x up) and snapped to the nearest axis permutation when it is one; the scale and
//    offset are the least-squares (Procrustes) fit of a dozen paired joints (kMarkers).
// 2. Repose. Both skeletons are T-poses, but their proportions differ (Prime's shoulders sit
//    lower and wider, its ankles lower, Dread's forearms and cannon point ~19 degrees forward).
//    Each joint of kRepose is an anchor: its head is pinned on its Prime joint, it is turned by
//    the minimal rotation that aims its bone (head -> tail joint) along Prime's bone, parents
//    first, so a chain keeps its twist, and it is stretched along the bone to Prime's length.
//    kFollow joints (shoulder pads, armpit helpers) keep their parent's rotation but move with
//    the arm's root; every other joint is carried rigidly by its parent. The feet are also
//    squashed towards Prime's floor (kFloor: Prime's ankles sit lower). The mesh is then
//    linear-blended with Dread's own weights through those transforms, normals with the
//    inverse transpose.
// 3. Weights. Each Dread weight moves to the Prime joint kWeights names for its joint, or for
//    the nearest ancestor that has a row. Rows marked vote (shoulder pads and helpers, where
//    Dread and Prime cut the shoulder differently and Prime's varia suits use their own
//    *_varia_SDK joints) take the bones the nearest retail vertices vote for, as the
//    Remastered converter's SkinWeights does; without retail weights they use the row's
//    candidates. Weights are merged per bone, cut to four, quantised to 1/64 and renormalised.
namespace PortDread {
namespace {

using V3 = std::array<double, 3>;
struct M3 {
  double m[3][3];
  static M3 I() {
    M3 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
    return r;
  }
};

V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Mul(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Unit(const V3& a) {
  const double l = Len(a);
  return l > 0.0 ? Mul(a, 1.0 / l) : V3{0, 0, 0};
}
V3 Apply(const M3& m, const V3& v) {
  return {m.m[0][0] * v[0] + m.m[0][1] * v[1] + m.m[0][2] * v[2],
          m.m[1][0] * v[0] + m.m[1][1] * v[1] + m.m[1][2] * v[2],
          m.m[2][0] * v[0] + m.m[2][1] * v[1] + m.m[2][2] * v[2]};
}
M3 Mul(const M3& a, const M3& b) {
  M3 r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      for (int k = 0; k < 3; ++k) {
        r.m[i][j] += a.m[i][k] * b.m[k][j];
      }
    }
  }
  return r;
}

// The smallest rotation taking direction a onto direction b.
M3 MinRotation(const V3& from, const V3& to) {
  const V3 a = Unit(from), b = Unit(to);
  const V3 v = Cross(a, b);
  const double c = Dot(a, b);
  if (c < -0.999999) {
    // Opposite: half a turn about any axis perpendicular to a.
    V3 axis = Unit(Cross(a, std::fabs(a[0]) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0}));
    M3 r{};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        r.m[i][j] = 2.0 * axis[i] * axis[j] - (i == j ? 1.0 : 0.0);
      }
    }
    return r;
  }
  const double k = 1.0 / (1.0 + c);
  M3 r = M3::I();
  const double vx[3][3] = {{0, -v[2], v[1]}, {v[2], 0, -v[0]}, {-v[1], v[0], 0}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double sq = 0.0;
      for (int t = 0; t < 3; ++t) {
        sq += vx[i][t] * vx[t][j];
      }
      r.m[i][j] += vx[i][j] + sq * k;
    }
  }
  return r;
}

// I + (k - 1) u u^T: scale by k along unit u.
M3 AxialScale(const V3& u, double k) {
  M3 r = M3::I();
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.m[i][j] += (k - 1.0) * u[i] * u[j];
    }
  }
  return r;
}

// Inverse by cofactors; identity when singular.
M3 Inverse(const M3& a) {
  const auto& m = a.m;
  M3 c;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
      c.m[j][i] = m[i1][j1] * m[i2][j2] - m[i1][j2] * m[i2][j1];  // transposed: the adjugate
    }
  }
  const double det = m[0][0] * c.m[0][0] + m[0][1] * c.m[1][0] + m[0][2] * c.m[2][0];
  if (std::fabs(det) < 1e-12) {
    return M3::I();
  }
  for (auto& row : c.m) {
    for (double& x : row) {
      x /= det;
    }
  }
  return c;
}

M3 Transpose(const M3& a) {
  M3 r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r.m[i][j] = a.m[j][i];
    }
  }
  return r;
}

// ---- the joint table ----
//
// Names: "{S}" expands to L and R (Dread's side prefix), "{s}" to the same side in Prime's
// names; a trailing '*' matches any rest. A row whose Prime joint the skeleton lacks is skipped
// (the joint is then carried, or for weights inherits from its ancestor).

enum : uint8_t {
  kPin = 1,      // move the head onto the Prime joint
  kAim = 2,      // turn the bone (head -> tail) onto Prime's bone
  kStretch = 4,  // scale along the bone to Prime's bone length
  kFloor = 8,    // squash vertically (about the pinned head) so the sole nears Prime's floor
};
constexpr uint8_t kAll = kPin | kAim | kStretch;

struct Tail {
  const char* dread;
  const char* prime;
};

struct ReposeRow {
  const char* dread;
  const char* prime;
  Tail tails[2];  // the first pair both skeletons have defines the bone
  uint8_t flags;
};

// Anchors, root to tips (order does not matter; parents are resolved first).
const ReposeRow kRepose[] = {
    {"C01_Master_SKN", "Pelvis", {}, kPin},
    {"C01_Spine0_SKN", "Spine_1", {{"C01_Spine1_SKN", "Spine_2"}}, kAll},
    // Prime has a Neck_1 only on the 50-joint body; the 20-joint one aims the chest at the head.
    {"C01_Spine1_SKN", "Spine_2", {{"C01_Neck_SKN", "Neck_1"}, {"C01_Head_SKN", "Head_1"}}, kAll},
    {"C01_Neck_SKN", "Neck_1", {{"C01_Head_SKN", "Head_1"}}, kAll},
    {"C01_Head_SKN", "Head_1", {}, kPin},
    {"{S}01_UpperArm_SKN", "{s}_shoulder", {{"{S}01_Forearm_SKN", "{s}_elbow"}}, kAll},
    {"L01_Forearm_SKN", "L_elbow", {{"L01_Hand_SKN", "L_wrist"}}, kAll},
    {"L01_Hand_SKN", "L_wrist", {{"L01_Finger2_SKN", "L_middle_1"}}, kPin | kAim},
    // The right forearm is the cannon: aimed along Prime's gun locators, never stretched (the
    // cannons differ in length; Dread's reaches past the hand).
    {"R01_Forearm_SKN", "R_elbow", {{"R01_Cannon_Barrel", "Charge_LCTR"}, {"R01_Cannon_Barrel", "GUN_LCTR"}},
     kPin | kAim},
    {"{S}01_Thigh_SKN", "{s}_hip", {{"{S}01_Calf_SKN", "{s}_knee"}}, kAll},
    {"{S}01_Calf_SKN", "{s}_knee", {{"{S}01_Foot_SKN", "{s}_ankle"}}, kAll},
    // Prime's ankles sit 11.5 cm above its floor, Dread's 17.5 cm (fitted): pinned, the boot
    // would sink 6 cm. It is squashed instead, at most by kMinFloorSquash.
    {"{S}01_Foot_SKN", "{s}_ankle", {{"{S}01_Toe_SKN", "{s}_ball"}}, kPin | kAim | kFloor},
};

// The most a kFloor joint is squashed (the rest of the sink stays: ~2 cm on Samus).
constexpr double kMinFloorSquash = 0.75;

struct FollowRow {
  const char* dread;
  const char* follow;  // the anchor whose head displacement this joint takes
  double share;        // of that displacement
};

// Shoulder-pad and shoulder helper joints hang off Dread's chest but sit on the arm's root;
// Prime's shoulder joint is ~8 cm further out and ~6 cm lower, so they go with the arm.
const FollowRow kFollow[] = {
    {"{S}01_ShoulderPad*", "{S}01_UpperArm_SKN", 1.0},
    {"{S}01_Shoulder_Aux2_SKN", "{S}01_UpperArm_SKN", 1.0},
    {"{S}01_Shoulder_Armpit_SKN", "{S}01_UpperArm_SKN", 1.0},
    {"{S}01_Shoulder_Aux1_SKN", "{S}01_UpperArm_SKN", 0.5},  // trapezius, half way to the neck
};

struct WeightRow {
  const char* dread;
  const char* prime[2];  // the first one the skeleton has
  bool vote;
};

// First matching row wins: specific rows before wildcards.
const WeightRow kWeights[] = {
    {"C01_Master_SKN", {"Pelvis"}, false},
    {"C01_MasterHip_SKN", {"Pelvis"}, false},
    {"C01_MasterSpine_SKN", {"Spine_1"}, false},
    {"C01_Spine0_SKN", {"Spine_1"}, false},
    {"C01_Spine1_SKN", {"Spine_2"}, false},
    {"C01_Spine2_SKN", {"Collar", "Spine_2"}, false},
    {"C01_Neck_SKN", {"Neck_1"}, false},
    {"C01_Head_SKN", {"Head_1"}, false},
    // The pads are fixed to the chest plate, as the chest joint: voted from Prime's collar-weighted
    // pads they shrugged up against the helmet, and on the shoulder they turned with the arm.
    {"{S}01_ShoulderPad*", {"Collar", "Spine_2"}, false},
    {"{S}01_Shoulder_Aux*", {"{s}_collar", "Spine_2"}, true},
    {"{S}01_Shoulder_Armpit_SKN", {"{s}_shoulder"}, true},
    {"{S}01_UpperArm*", {"{s}_shoulder"}, false},  // with the twist joints
    {"{S}01_Forearm*", {"{s}_elbow"}, false},
    {"R01_Cannon*", {"R_elbow"}, false},  // the skinned cannon rides the right forearm
    {"L01_Hand_SKN", {"L_wrist"}, false},
    {"L01_Finger0_SKN", {"L_thumb_1"}, false},
    {"L01_Finger01_SKN", {"L_thumb_2"}, false},
    {"L01_Finger1_SKN", {"L_Index_1"}, false},
    {"L01_Finger11_SKN", {"L_Index_2"}, false},
    {"L01_Finger2_SKN", {"L_middle_1"}, false},
    {"L01_Finger21_SKN", {"L_Ring_1"}, false},
    {"Cine_L01_Finger00_01_SKN", {"L_thumb_1"}, false},
    {"Cine_L01_Finger00_*", {"L_thumb_2"}, false},
    {"Cine_L01_Finger01_01_SKN", {"L_Index_1"}, false},
    {"Cine_L01_Finger01_*", {"L_Index_2"}, false},
    {"Cine_L01_Finger02_01_SKN", {"L_middle_1"}, false},
    {"Cine_L01_Finger02_*", {"L_middle_2"}, false},
    {"Cine_L01_Finger03_01_SKN", {"L_Ring_1"}, false},
    {"Cine_L01_Finger03_*", {"L_Ring_2"}, false},
    {"Cine_L01_Finger04_01_SKN", {"L_Pinky_1"}, false},
    {"Cine_L01_Finger04_*", {"L_Pinky_2"}, false},
    {"{S}01_Thigh*", {"{s}_hip"}, false},
    {"{S}01_Calf*", {"{s}_knee"}, false},
    {"{S}01_Foot*", {"{s}_ankle"}, false},
    {"{S}01_Toe_SKN", {"{s}_ball"}, false},
};

// Joints paired for the frame and scale fit.
const Tail kMarkers[] = {
    {"C01_Head_SKN", "Head_1"},          {"C01_Master_SKN", "Pelvis"},
    {"L01_UpperArm_SKN", "L_shoulder"},  {"R01_UpperArm_SKN", "R_shoulder"},
    {"L01_Forearm_SKN", "L_elbow"},      {"R01_Forearm_SKN", "R_elbow"},
    {"L01_Thigh_SKN", "L_hip"},          {"R01_Thigh_SKN", "R_hip"},
    {"L01_Calf_SKN", "L_knee"},          {"R01_Calf_SKN", "R_knee"},
    {"L01_Foot_SKN", "L_ankle"},         {"R01_Foot_SKN", "R_ankle"},
};

// Joints of the Dread-proportioned Prime skeleton placed on a Dread joint's head. Prime's
// Skeleton_Root, Pelvis and Spine_1 share one head at the hips; Dread's spine starts higher, and
// Spine_1 takes it so the mesh's spine bends where it was built to. Prime's collars, eyes, SDK
// pads and locators have no Dread joint and keep their offsets from their parents.
const Tail kBind[] = {
    {"C01_Master_SKN", "Skeleton_Root"},
    {"C01_Master_SKN", "Pelvis"},
    {"C01_Spine0_SKN", "Spine_1"},
    {"C01_Spine1_SKN", "Spine_2"},
    {"C01_Spine2_SKN", "Collar"},
    {"C01_Neck_SKN", "Neck_1"},
    {"C01_Head_SKN", "Head_1"},
    {"{S}01_UpperArm_SKN", "{s}_shoulder"},
    {"{S}01_Forearm_SKN", "{s}_elbow"},
    {"L01_Hand_SKN", "L_wrist"},
    {"L01_Finger0_SKN", "L_thumb_1"},
    {"L01_Finger01_SKN", "L_thumb_2"},
    {"L01_Finger1_SKN", "L_Index_1"},
    {"L01_Finger11_SKN", "L_Index_2"},
    {"L01_Finger2_SKN", "L_middle_1"},
    {"Cine_L01_Finger02_02_SKN", "L_middle_2"},
    {"Cine_L01_Finger03_01_SKN", "L_Ring_1"},
    {"Cine_L01_Finger03_02_SKN", "L_Ring_2"},
    {"Cine_L01_Finger04_01_SKN", "L_Pinky_1"},
    {"Cine_L01_Finger04_02_SKN", "L_Pinky_2"},
    {"{S}01_Thigh_SKN", "{s}_hip"},
    {"{S}01_Calf_SKN", "{s}_knee"},
    {"{S}01_Foot_SKN", "{s}_ankle"},
    {"{S}01_Toe_SKN", "{s}_ball"},
};

// "{S}"/"{s}" expanded for one side.
std::string Expand(const char* pattern, char side) {
  std::string out;
  for (const char* p = pattern; *p != '\0'; ++p) {
    if (p[0] == '{' && (p[1] == 'S' || p[1] == 's') && p[2] == '}') {
      out += side;
      p += 2;
    } else {
      out += *p;
    }
  }
  return out;
}

bool HasSide(const char* pattern) { return std::strstr(pattern, "{S}") != nullptr || std::strstr(pattern, "{s}") != nullptr; }

bool Matches(const std::string& pattern, const std::string& name) {
  if (!pattern.empty() && pattern.back() == '*') {
    return name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
  }
  return name == pattern;
}

// The side of the first expansion of `pattern` that matches `name`: 'L', 'R', or 0 for a
// side-less pattern that matches; -1 when none does.
int MatchSide(const char* pattern, const std::string& name) {
  if (!HasSide(pattern)) {
    return Matches(pattern, name) ? 0 : -1;
  }
  for (char side : {'L', 'R'}) {
    if (Matches(Expand(pattern, side), name)) {
      return side;
    }
  }
  return -1;
}

std::string ForSide(const char* pattern, int side) { return side > 0 ? Expand(pattern, char(side)) : std::string(pattern); }

bool Fail(std::string* error, const std::string& what) {
  if (error != nullptr) {
    *error = what;
  }
  return false;
}

} // namespace

// ---- CINF ----

int PrimeSkeleton::Find(const std::string& name) const {
  for (size_t i = 0; i < joints.size(); ++i) {
    if (joints[i].name == name) {
      return int(i);
    }
  }
  return -1;
}

int PrimeSkeleton::IndexOfId(uint32_t id) const {
  for (size_t i = 0; i < joints.size(); ++i) {
    if (joints[i].id == id) {
      return int(i);
    }
  }
  return -1;
}

bool ParseCinf(const uint8_t* data, size_t size, PrimeSkeleton& out, std::string* error) {
  out.joints.clear();
  size_t o = 0;
  auto u32 = [&](uint32_t& v) {
    if (size < 4 || o > size - 4) {
      return false;
    }
    v = (uint32_t(data[o]) << 24) | (uint32_t(data[o + 1]) << 16) | (uint32_t(data[o + 2]) << 8) | data[o + 3];
    o += 4;
    return true;
  };
  auto f32 = [&](float& f) {
    uint32_t v;
    if (!u32(v)) {
      return false;
    }
    std::memcpy(&f, &v, 4);
    return true;
  };
  uint32_t n;
  if (!u32(n) || n > 1024) {
    return Fail(error, "CINF: bad joint count");
  }
  for (uint32_t i = 0; i < n; ++i) {
    PrimeJoint j;
    uint32_t links;
    if (!u32(j.id) || !u32(j.parent) || !f32(j.head[0]) || !f32(j.head[1]) || !f32(j.head[2]) || !u32(links) ||
        links > 1024 || links * 4ull > size - o) {
      return Fail(error, "CINF: joint " + std::to_string(i) + " out of range");
    }
    o += size_t(links) * 4;
    out.joints.push_back(std::move(j));
  }
  uint32_t order;
  if (!u32(order) || order > 1024 || order * 4ull > size - o) {
    return Fail(error, "CINF: bad build order");
  }
  o += size_t(order) * 4;
  uint32_t names;
  if (!u32(names) || names > 1024) {
    return Fail(error, "CINF: bad name count");
  }
  for (uint32_t i = 0; i < names; ++i) {
    const void* end = o < size ? std::memchr(data + o, 0, size - o) : nullptr;
    if (end == nullptr) {
      return Fail(error, "CINF: unterminated joint name");
    }
    std::string name(reinterpret_cast<const char*>(data + o), static_cast<const uint8_t*>(end) - (data + o));
    o += name.size() + 1;
    uint32_t id;
    if (!u32(id)) {
      return Fail(error, "CINF: joint name table out of range");
    }
    for (PrimeJoint& j : out.joints) {
      if (j.id == id) {
        j.name = name;
      }
    }
  }
  for (PrimeJoint& j : out.joints) {
    bool found = false;
    for (const PrimeJoint& p : out.joints) {
      found = found || (p.id == j.parent && &p != &j);
    }
    if (!found) {
      j.parent = kPrimeNoParent;
    }
  }
  return true;
}

bool PatchCinfHeads(const uint8_t* cinf, size_t size, const PrimeSkeleton& skeleton, std::vector<uint8_t>& out,
                    std::string* error) {
  PrimeSkeleton check;
  if (!ParseCinf(cinf, size, check, error)) {
    return false;
  }
  out.assign(cinf, cinf + size);
  // ParseCinf has bounds-checked the joint records this walk reads.
  auto u32 = [&](size_t o) {
    return (uint32_t(out[o]) << 24) | (uint32_t(out[o + 1]) << 16) | (uint32_t(out[o + 2]) << 8) | out[o + 3];
  };
  const uint32_t n = u32(0);
  size_t o = 4;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t id = u32(o);
    const int j = skeleton.IndexOfId(id);
    if (j < 0) {
      return Fail(error, "CINF patch: joint id " + std::to_string(id) + " missing from the skeleton");
    }
    for (int a = 0; a < 3; ++a) {
      uint32_t v;
      std::memcpy(&v, &skeleton.joints[size_t(j)].head[a], 4);
      const size_t at = o + 8 + size_t(a) * 4;
      out[at] = uint8_t(v >> 24);
      out[at + 1] = uint8_t(v >> 16);
      out[at + 2] = uint8_t(v >> 8);
      out[at + 3] = uint8_t(v);
    }
    o += 20 + size_t(u32(o + 20)) * 4 + 4;
  }
  return true;
}

// ---- re-skin ----

namespace {

struct JointXf {
  bool done = false;
  bool busy = false;
  M3 rot = M3::I();   // the joint's rotation change (no stretch), for children's aims
  M3 lin = M3::I();   // x' = lin * x + t
  M3 nrm = M3::I();   // inverse transpose of lin, for normals
  V3 t = {0, 0, 0};
};

struct Ctx {
  const Bcmdl& model;
  const PrimeSkeleton& skel;
  std::vector<V3> head;      // fitted Dread joint heads (Prime space, before the repose)
  std::vector<JointXf> xf;
  V3 up;                     // Prime's up
  double primeFloor = 0.0;   // along up: Prime's root joint
  double dreadFloor = 0.0;   // along up: Dread's DC_Floor, fitted
  bool hasFloor = false;
  ReskinStats& stats;
  std::vector<std::string>& log;
};

V3 PrimeHead(const PrimeSkeleton& s, int i) { return {s.joints[i].head[0], s.joints[i].head[1], s.joints[i].head[2]}; }

V3 XfPoint(const JointXf& x, const V3& p) { return Add(Apply(x.lin, p), x.t); }

// The repose transform of joint j, parents (and followed joints) first.
bool Solve(Ctx& c, int j, std::string* error) {
  JointXf& x = c.xf[size_t(j)];
  if (x.done) {
    return true;
  }
  if (x.busy) {
    return Fail(error, "joint table cycle at '" + c.model.joints[size_t(j)].name + "'");
  }
  x.busy = true;
  const BcmdlJoint& joint = c.model.joints[size_t(j)];
  JointXf parent;
  if (joint.parent >= 0) {
    if (!Solve(c, joint.parent, error)) {
      return false;
    }
    parent = c.xf[size_t(joint.parent)];
  }
  const V3 h = c.head[size_t(j)];
  // Carried: the parent's transform as it is.
  x.rot = parent.rot;
  x.lin = parent.lin;
  x.nrm = parent.nrm;
  x.t = parent.t;
  bool anchored = false;
  for (const ReposeRow& row : kRepose) {
    const int side = MatchSide(row.dread, joint.name);
    if (side < 0) {
      continue;
    }
    const int pj = c.skel.Find(ForSide(row.prime, side));
    if (pj < 0) {
      break;  // the skeleton lacks it: carried
    }
    const V3 ph = PrimeHead(c.skel, pj);
    const V3 nh = (row.flags & kPin) ? ph : XfPoint(parent, h);
    M3 rot = parent.rot;
    M3 scale = M3::I(), inv = M3::I();
    for (const Tail& tail : row.tails) {
      if (tail.dread == nullptr) {
        break;
      }
      const int dt = c.model.FindJoint(ForSide(tail.dread, side));
      const int pt = c.skel.Find(ForSide(tail.prime, side));
      if (dt < 0 || pt < 0) {
        continue;
      }
      const V3 dbone = Sub(c.head[size_t(dt)], h);
      const V3 pbone = Sub(PrimeHead(c.skel, pt), ph);
      if (Len(dbone) < 1e-6 || Len(pbone) < 1e-6) {
        continue;
      }
      if (row.flags & kAim) {
        rot = Mul(MinRotation(Apply(parent.rot, dbone), pbone), parent.rot);
      }
      if (row.flags & kStretch) {
        const double k = std::clamp(Len(pbone) / Len(dbone), 0.5, 2.0);
        scale = AxialScale(Unit(dbone), k);
        inv = AxialScale(Unit(dbone), 1.0 / k);
      }
      {
        const double turn = std::acos(std::clamp(Dot(Unit(Apply(parent.rot, dbone)), Unit(pbone)), -1.0, 1.0));
        char buf[192];
        std::snprintf(buf, sizeof(buf), "bone: %s -> %s: dread %.3f prime %.3f (x%.2f), turn %.1f deg, head moved %.3f",
                      joint.name.c_str(), ForSide(tail.prime, side).c_str(), Len(dbone), Len(pbone),
                      Len(pbone) / Len(dbone), turn * 180.0 / 3.14159265358979323846,  // MSVC has no M_PI
                      Len(Sub(nh, XfPoint(parent, h))));
        c.log.push_back(buf);
      }
      break;
    }
    M3 squash = M3::I(), unsquash = M3::I();
    if ((row.flags & kFloor) && c.hasFloor) {
      const double above = Dot(h, c.up) - c.dreadFloor, want = Dot(nh, c.up) - c.primeFloor;
      if (above > 1e-6 && want > 0.0 && want < above) {
        const double k = std::max(want / above, kMinFloorSquash);
        squash = AxialScale(c.up, k);
        unsquash = AxialScale(c.up, 1.0 / k);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "floor: %s squashed to %.2f", joint.name.c_str(), k);
        c.log.push_back(buf);
      }
    }
    x.rot = rot;
    x.lin = Mul(squash, Mul(rot, scale));
    x.nrm = Mul(unsquash, Mul(rot, inv));
    x.t = Sub(nh, Apply(x.lin, h));
    anchored = true;
    ++c.stats.anchors;
    break;
  }
  if (!anchored) {
    bool followed = false;
    for (const FollowRow& row : kFollow) {
      const int side = MatchSide(row.dread, joint.name);
      if (side < 0) {
        continue;
      }
      const int f = c.model.FindJoint(ForSide(row.follow, side));
      if (f < 0 || f == j) {
        break;
      }
      if (!Solve(c, f, error)) {
        return false;
      }
      const V3 hf = c.head[size_t(f)];
      const V3 shift = Sub(XfPoint(c.xf[size_t(f)], hf), XfPoint(parent, hf));
      x.t = Add(x.t, Mul(shift, row.share));
      followed = true;
      ++c.stats.follows;
      break;
    }
    if (!followed) {
      ++c.stats.carried;
    }
  }
  x.busy = false;
  x.done = true;
  return true;
}

// The weight target of each Dread joint: a Prime joint index (for a vote joint, the one used
// when no retail vertex votes) and whether retail votes decide instead.
struct Target {
  int prime = -1;
  bool vote = false;
  enum { Direct, Vote, Inherited } how = Inherited;
};

int RowCandidate(const WeightRow& row, int side, const PrimeSkeleton& s) {
  for (const char* p : row.prime) {
    if (p == nullptr) {
      break;
    }
    const int i = s.Find(ForSide(p, side));
    if (i >= 0) {
      return i;
    }
  }
  return -1;
}

int RootCandidate(const PrimeSkeleton& s) {
  for (const char* n : {"Pelvis", "Skeleton_Root", "Spine_1"}) {
    const int i = s.Find(n);
    if (i >= 0) {
      return i;
    }
  }
  return s.joints.empty() ? -1 : 0;
}

std::vector<Target> WeightTargets(const Bcmdl& model, const PrimeSkeleton& skel, bool canVote) {
  const size_t n = model.joints.size();
  std::vector<Target> out(n);
  std::vector<uint8_t> state(n, 0);
  std::function<void(size_t)> resolve = [&](size_t j) {
    if (state[j] != 0) {
      return;
    }
    state[j] = 1;
    const std::string& name = model.joints[j].name;
    for (const WeightRow& row : kWeights) {
      const int side = MatchSide(row.dread, name);
      if (side < 0) {
        continue;
      }
      const int cand = RowCandidate(row, side, skel);
      if (row.vote) {
        out[j].vote = canVote;
        out[j].how = Target::Vote;
      } else if (cand >= 0) {
        out[j].how = Target::Direct;
      }
      if (cand >= 0) {
        out[j].prime = cand;
        state[j] = 2;
        return;
      }
      break;  // the row's joints are missing here: inherit
    }
    const int par = model.joints[j].parent;
    if (par >= 0 && size_t(par) < n && state[size_t(par)] != 1) {
      resolve(size_t(par));
      out[j].prime = out[size_t(par)].prime;
      if (out[j].how != Target::Vote) {
        out[j].vote = out[size_t(par)].vote;
      }
    } else {
      out[j].prime = RootCandidate(skel);
    }
    state[j] = 2;
  };
  for (size_t j = 0; j < n; ++j) {
    resolve(j);
  }
  return out;
}

int FindMarker(const Bcmdl& m, const char* name, std::string* error) {
  const int i = m.FindJoint(name);
  if (i < 0 && error != nullptr) {
    *error = std::string("Dread model lacks joint ") + name;
  }
  return i;
}

} // namespace

bool ReskinToPrime(const Bcmdl& model, const PrimeSkeleton& skel, const ReskinOptions& options, ReskinResult& out,
                   std::string* error) {
  out = ReskinResult{};
  std::vector<Mat4> world;
  if (!ComputeWorldBind(model, world, error)) {
    return false;
  }
  const size_t nj = model.joints.size();
  std::vector<V3> dhead(nj);
  for (size_t j = 0; j < nj; ++j) {
    dhead[j] = {world[j].m[0][3], world[j].m[1][3], world[j].m[2][3]};
  }
  // options.mesh_pose: per joint the move from its world bind to its posed world, for the
  // vertices only (posed[j] * world[j]^-1); unposed joints (and their subtrees) have none.
  std::vector<JointXf> meshPose(nj);
  {
    std::vector<int> pose(nj, -1);
    for (size_t i = 0; i < options.mesh_pose.size(); ++i) {
      const int j = model.FindJoint(options.mesh_pose[i].name);
      if (j >= 0) {
        pose[size_t(j)] = int(i);
      }
    }
    auto toXf = [](const Mat4& m, M3& lin, V3& t) {
      for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
          lin.m[r][k] = m.m[r][k];
        }
        t[r] = m.m[r][3];
      }
    };
    std::vector<Mat4> posed(nj);
    std::vector<uint8_t> state(nj, 0);  // 0 todo, 1 busy, 2 done
    std::function<void(size_t)> place = [&](size_t j) {
      if (state[j] != 0) {
        return;  // done, or a cycle (ComputeWorldBind already refused those)
      }
      state[j] = 1;
      const int par = model.joints[j].parent;
      bool moved = pose[j] >= 0;
      Mat4 parentPosed = Mat4::Identity();
      if (par >= 0) {
        place(size_t(par));
        moved = moved || meshPose[size_t(par)].done;
        parentPosed = posed[size_t(par)];
      }
      if (!moved) {
        posed[j] = world[j];
      } else {
        BcmdlJoint local = model.joints[j];
        if (pose[j] >= 0) {
          const BcmdlJoint& p = options.mesh_pose[size_t(pose[j])];
          std::copy(p.pos, p.pos + 3, local.pos);
          std::copy(p.euler, p.euler + 3, local.euler);
          std::copy(p.scale, p.scale + 3, local.scale);
        }
        posed[j] = par >= 0 ? parentPosed * JointLocalMatrix(local) : JointLocalMatrix(local);
        M3 pl, wl;
        V3 pt, wt;
        toXf(posed[j], pl, pt);
        toXf(world[j], wl, wt);
        JointXf& x = meshPose[j];
        x.lin = Mul(pl, Inverse(wl));
        x.nrm = Transpose(Inverse(x.lin));
        x.t = Sub(pt, Apply(x.lin, wt));
        x.done = true;  // = this joint moves
        ++out.stats.posed;
      }
      state[j] = 2;
    };
    for (size_t j = 0; j < nj; ++j) {
      place(j);
    }
  }

  // ---- 1. fit ----
  std::vector<std::pair<V3, V3>> pairs;  // (Dread, Prime)
  for (const Tail& mk : kMarkers) {
    const int d = model.FindJoint(mk.dread), p = skel.Find(mk.prime);
    if (d >= 0 && p >= 0) {
      pairs.emplace_back(dhead[size_t(d)], PrimeHead(skel, p));
    }
  }
  auto need = [&](const char* d, const char* p, V3& dv, V3& pv) {
    const int di = FindMarker(model, d, error);
    const int pi = skel.Find(p);
    if (di < 0) {
      return false;
    }
    if (pi < 0) {
      return Fail(error, std::string("Prime skeleton lacks joint ") + p);
    }
    dv = dhead[size_t(di)];
    pv = PrimeHead(skel, pi);
    return true;
  };
  V3 dHeadP, pHeadP, dLF, pLF, dRF, pRF, dLA, pLA, dRA, pRA;
  if (!need("C01_Head_SKN", "Head_1", dHeadP, pHeadP) || !need("L01_Foot_SKN", "L_ankle", dLF, pLF) ||
      !need("R01_Foot_SKN", "R_ankle", dRF, pRF) || !need("L01_UpperArm_SKN", "L_shoulder", dLA, pLA) ||
      !need("R01_UpperArm_SKN", "R_shoulder", dRA, pRA)) {
    return false;
  }
  if (skel.Find("Pelvis") < 0 || model.FindJoint("C01_Master_SKN") < 0) {
    return Fail(error, "pelvis joint missing (C01_Master_SKN / Pelvis)");
  }
  // Orthonormal (left, up, forward) bases of both skeletons; frame = Prime * Dread^T.
  auto basis = [](const V3& head, const V3& lf, const V3& rf, const V3& la, const V3& ra, V3 b[3]) {
    const V3 up = Unit(Sub(head, Mul(Add(lf, rf), 0.5)));
    V3 left = Sub(la, ra);
    left = Unit(Sub(left, Mul(up, Dot(left, up))));
    b[0] = left;
    b[1] = up;
    b[2] = Cross(left, up);
  };
  V3 bd[3], bp[3];
  basis(dHeadP, dLF, dRF, dLA, dRA, bd);
  basis(pHeadP, pLF, pRF, pLA, pRA, bp);
  M3 frame{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      for (int k = 0; k < 3; ++k) {
        frame.m[i][j] += bp[k][i] * bd[k][j];
      }
    }
  }
  // Snap to an axis permutation when the skeletons are that close to one (they are: both
  // T-poses facing their forward axis); a skew of a degree or two is pose, not frame.
  {
    M3 snap{};
    bool ok = true;
    for (int col = 0; col < 3 && ok; ++col) {
      int best = 0;
      for (int row = 1; row < 3; ++row) {
        if (std::fabs(frame.m[row][col]) > std::fabs(frame.m[best][col])) {
          best = row;
        }
      }
      ok = std::fabs(frame.m[best][col]) > 0.97;
      snap.m[best][col] = frame.m[best][col] > 0 ? 1.0 : -1.0;
    }
    const V3 c0 = {snap.m[0][0], snap.m[1][0], snap.m[2][0]}, c1 = {snap.m[0][1], snap.m[1][1], snap.m[2][1]},
             c2 = {snap.m[0][2], snap.m[1][2], snap.m[2][2]};
    if (ok && Dot(Cross(c0, c1), c2) > 0.5) {
      frame = snap;
      out.log.push_back("frame: snapped to an axis permutation");
    } else {
      out.log.push_back("frame: not an axis permutation, kept as fitted");
    }
  }
  if (pairs.size() < 4) {
    return Fail(error, "too few paired joints for the scale fit");
  }
  V3 dc = {0, 0, 0}, pc = {0, 0, 0};
  for (const auto& pr : pairs) {
    dc = Add(dc, pr.first);
    pc = Add(pc, pr.second);
  }
  dc = Mul(dc, 1.0 / double(pairs.size()));
  pc = Mul(pc, 1.0 / double(pairs.size()));
  double num = 0.0, den = 0.0;
  for (const auto& pr : pairs) {
    const V3 d = Apply(frame, Sub(pr.first, dc));
    num += Dot(Sub(pr.second, pc), d);
    den += Dot(d, d);
  }
  if (!(den > 0.0) || !(num > 0.0)) {
    return Fail(error, "degenerate scale fit");
  }
  const double scale = num / den;
  const V3 offset = Sub(pc, Mul(Apply(frame, dc), scale));
  out.scale = float(scale);
  for (int i = 0; i < 3; ++i) {
    out.offset[i] = float(offset[i]);
    for (int j = 0; j < 3; ++j) {
      out.frame[i][j] = float(frame.m[i][j]);
    }
  }
  {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "fit: scale %.6f (Dread units per Prime unit %.2f), offset (%.3f, %.3f, %.3f)",
                  scale, 1.0 / scale, offset[0], offset[1], offset[2]);
    out.log.push_back(buf);
  }
  auto fit = [&](const V3& d) { return Add(Mul(Apply(frame, d), scale), offset); };

  // ---- 2. repose transforms ----
  Ctx ctx{model, skel, {}, std::vector<JointXf>(nj), bp[1], 0.0, 0.0, false, out.stats, out.log};
  ctx.head.resize(nj);
  for (size_t j = 0; j < nj; ++j) {
    ctx.head[j] = fit(dhead[j]);
  }
  // Floors: Dread's DC_Floor joint and Prime's root (the joint with no parent; at the origin).
  for (const PrimeJoint& pj : skel.joints) {
    if (pj.parent == kPrimeNoParent) {
      ctx.primeFloor = Dot(V3{pj.head[0], pj.head[1], pj.head[2]}, ctx.up);
      const int df = model.FindJoint("DC_Floor");
      if (df >= 0) {
        ctx.dreadFloor = Dot(ctx.head[size_t(df)], ctx.up);
        ctx.hasFloor = true;
      }
      break;
    }
  }
  // Prime's skeleton at Dread's proportions, parents first.
  {
    const size_t np = skel.joints.size();
    std::vector<int> bind(np, -1);  // Prime index -> Dread joint
    for (const Tail& row : kBind) {
      for (char side : {'L', 'R'}) {
        const int pj = skel.Find(Expand(row.prime, side)), dj = model.FindJoint(Expand(row.dread, side));
        if (pj >= 0 && dj >= 0) {
          bind[size_t(pj)] = dj;
        }
        if (!HasSide(row.prime)) {
          break;
        }
      }
    }
    out.skeleton = skel;
    std::vector<uint8_t> state(np, 0);
    std::function<void(size_t)> place = [&](size_t i) {
      if (state[i] != 0) {
        return;  // done, or a parent cycle (left where it is)
      }
      state[i] = 1;
      PrimeJoint& j = out.skeleton.joints[i];
      const int par = j.parent == kPrimeNoParent ? -1 : skel.IndexOfId(j.parent);
      V3 h = PrimeHead(skel, int(i));
      const char* how = "kept";
      if (bind[i] >= 0) {
        h = ctx.head[size_t(bind[i])];
        how = "mapped";
        ++out.skeleton_mapped;
      } else if (par >= 0) {
        place(size_t(par));
        h = Add(PrimeHead(out.skeleton, par), Sub(PrimeHead(skel, int(i)), PrimeHead(skel, par)));
        how = "carried";
      }
      for (int a = 0; a < 3; ++a) {
        j.head[a] = float(h[a]);
      }
      char buf[192];
      std::snprintf(buf, sizeof(buf), "skel: %s %s%s%s, (%.3f, %.3f, %.3f), moved %.3f", j.name.c_str(), how,
                    bind[i] >= 0 ? " from " : "", bind[i] >= 0 ? model.joints[size_t(bind[i])].name.c_str() : "", h[0],
                    h[1], h[2], Len(Sub(h, PrimeHead(skel, int(i)))));
      out.log.push_back(buf);
      state[i] = 2;
    };
    for (size_t i = 0; i < np; ++i) {
      place(i);
    }
  }
  out.stats.joints = nj;
  for (size_t j = 0; j < nj; ++j) {
    if (!Solve(ctx, int(j), error)) {
      return false;
    }
  }
  out.repose.resize(nj);
  for (size_t j = 0; j < nj; ++j) {
    Mat4 m = Mat4::Identity();
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) {
        m.m[r][k] = float(ctx.xf[j].lin.m[r][k]);
      }
      m.m[r][3] = float(ctx.xf[j].t[r]);
    }
    out.repose[j] = m;
  }

  // ---- 3. weight targets ----
  const bool canVote = options.retail != nullptr && !options.retail->weights.empty() &&
                       options.retail->pos.size() >= options.retail->weights.size() * 3;
  const std::vector<Target> targets = WeightTargets(model, skel, canVote);
  out.weight_joint.resize(nj);
  for (size_t j = 0; j < nj; ++j) {
    out.weight_joint[j] = targets[j].vote ? -1 : targets[j].prime;
    switch (targets[j].how) {
    case Target::Direct:
      ++out.stats.weight_direct;
      break;
    case Target::Vote:
      ++out.stats.weight_vote;
      break;
    default:
      ++out.stats.weight_inherited;
      break;
    }
  }

  // ---- 4. meshes: bind pose, repose, Dread weights ----
  // Per output vertex, its Dread (joint, weight) pairs; votes need every mesh's first.
  std::vector<std::vector<std::vector<std::pair<int, float>>>> dw;
  std::vector<std::vector<V3>> reposed;  // per vertex, in Prime's bind pose: the votes' lookup
  M3 frameN = frame;  // a rotation: its own inverse transpose
  for (const BcmdlMesh& mesh : model.meshes) {
    if (options.enabled_only && !mesh.enabled) {
      continue;
    }
    const BcmdlVertices& vs = mesh.vertices;
    const uint32_t nv = vs.count;
    if (vs.pos.size() != size_t(nv) * 3 || mesh.submeshes.empty()) {
      return Fail(error, "mesh '" + mesh.name + "': no positions or submeshes");
    }
    ReskinMesh rm;
    rm.name = mesh.name;
    rm.material_name = mesh.material_name;
    rm.bsmat_path = mesh.bsmat_path;
    rm.vertex_count = nv;
    // The submesh that first draws each vertex owns it (its joint map decodes the vertex).
    std::vector<int> owner(nv, -1);
    for (size_t s = 0; s < mesh.submeshes.size(); ++s) {
      const BcmdlSubmesh& sm = mesh.submeshes[s];
      if (size_t(sm.first_index) + sm.index_count > mesh.indices.size()) {
        return Fail(error, "mesh '" + mesh.name + "': submesh indices out of range");
      }
      ReskinSubmesh out_sm;
      out_sm.indices.reserve(sm.index_count);
      for (uint32_t i = 0; i < sm.index_count; ++i) {
        const uint16_t v = mesh.indices[sm.first_index + i];
        if (v >= nv) {
          return Fail(error, "mesh '" + mesh.name + "': index out of range");
        }
        if (owner[v] < 0) {
          owner[v] = int(s);
        }
        out_sm.indices.push_back(v);
      }
      rm.submeshes.push_back(std::move(out_sm));
    }
    const bool hasN = vs.normal.size() == size_t(nv) * 3;
    const bool hasT = vs.tangent.size() == size_t(nv) * 4;
    const bool hasW = vs.joints.size() == size_t(nv) * 4 && vs.weights.size() == size_t(nv) * 4;
    rm.pos.resize(size_t(nv) * 3);
    rm.normal.resize(size_t(nv) * 3);
    if (hasT) {
      rm.tangent.resize(size_t(nv) * 4);
    }
    for (int u = 0; u < 3; ++u) {
      if (vs.uv[u].size() == size_t(nv) * 2) {
        rm.uv[u] = vs.uv[u];
      }
    }
    std::vector<std::vector<std::pair<int, float>>> meshDw(nv);
    std::vector<V3> meshReposed(nv);
    for (uint32_t v = 0; v < nv; ++v) {
      const BcmdlSubmesh& sm = mesh.submeshes[size_t(std::max(owner[v], 0))];
      // Dread joints and weights of the vertex (an unreferenced vertex whose indices do not
      // decode rides the root).
      std::vector<std::pair<int, float>>& jw = meshDw[v];
      if (sm.skin_type == 1 || sm.skin_type == 2) {
        if (!hasW) {
          return Fail(error, "mesh '" + mesh.name + "': skinned submesh without weights");
        }
        for (int k = 0; k < 4; ++k) {
          const float w = vs.weights[4 * size_t(v) + k];
          if (w == 0.f) {
            continue;
          }
          const int32_t ji = vs.joints[4 * size_t(v) + k];
          if (ji < 0 || size_t(ji) >= sm.joint_map.size() || sm.joint_map[size_t(ji)] < 0 ||
              size_t(sm.joint_map[size_t(ji)]) >= nj) {
            if (owner[v] < 0) {
              jw.clear();
              break;
            }
            return Fail(error, "mesh '" + mesh.name + "': vertex joint outside the submesh joint map");
          }
          jw.emplace_back(sm.joint_map[size_t(ji)], w);
        }
      } else if (sm.skin_type == 0 && !sm.joint_map.empty()) {
        jw.emplace_back(sm.joint_map[0], 1.f);
      } else {
        return Fail(error, "mesh '" + mesh.name + "': unsupported skin type " + std::to_string(sm.skin_type));
      }
      if (jw.empty()) {
        jw.emplace_back(model.FindJoint("C01_Master_SKN"), 1.f);
      }
      float wsum = 0.f;
      for (const auto& p : jw) {
        wsum += p.second;
      }
      // Bind pose, Dread model space: skin type 2 is there already; 1 is bone-local (and 0
      // rigid on its joint), moved by the joint's world bind.
      const V3 local = {vs.pos[3 * size_t(v)] + mesh.transform[0], vs.pos[3 * size_t(v) + 1] + mesh.transform[1],
                        vs.pos[3 * size_t(v) + 2] + mesh.transform[2]};
      V3 bp = local, bn = {0, 0, 1}, bt = {1, 0, 0};
      if (hasN) {
        bn = {vs.normal[3 * size_t(v)], vs.normal[3 * size_t(v) + 1], vs.normal[3 * size_t(v) + 2]};
      }
      if (hasT) {
        bt = {vs.tangent[4 * size_t(v)], vs.tangent[4 * size_t(v) + 1], vs.tangent[4 * size_t(v) + 2]};
      }
      if (sm.skin_type != 2) {
        V3 ap = {0, 0, 0}, an = {0, 0, 0}, at = {0, 0, 0};
        for (const auto& p : jw) {
          const Mat4& m = world[size_t(p.first)];
          M3 r;
          for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
              r.m[a][b] = m.m[a][b];
            }
          }
          const V3 tp = Add(Apply(r, local), V3{m.m[0][3], m.m[1][3], m.m[2][3]});
          ap = Add(ap, Mul(tp, p.second / wsum));
          an = Add(an, Mul(Apply(r, bn), p.second));
          at = Add(at, Mul(Apply(r, bt), p.second));
        }
        bp = ap;
        bn = an;
        bt = at;
      }
      // Posed joints, blended by the same weights (unposed ones stay put).
      if (std::any_of(jw.begin(), jw.end(), [&](const auto& p) { return meshPose[size_t(p.first)].done; })) {
        V3 ap = {0, 0, 0}, an = {0, 0, 0}, at = {0, 0, 0};
        for (const auto& p : jw) {
          const JointXf& x = meshPose[size_t(p.first)];
          const double w = p.second / wsum;
          ap = Add(ap, Mul(x.done ? XfPoint(x, bp) : bp, w));
          an = Add(an, Mul(x.done ? Apply(x.nrm, bn) : bn, w));
          at = Add(at, Mul(x.done ? Apply(x.lin, bt) : bt, w));
        }
        bp = ap;
        bn = an;
        bt = at;
      }
      // Fit, then blend through the repose transforms.
      const V3 fp = fit(bp);
      const V3 fn = Apply(frameN, bn), ft = Apply(frameN, bt);
      V3 rp = {0, 0, 0}, rn = {0, 0, 0}, rt = {0, 0, 0};
      for (const auto& p : jw) {
        const JointXf& x = ctx.xf[size_t(p.first)];
        const double w = p.second / wsum;
        rp = Add(rp, Mul(XfPoint(x, fp), w));
        rn = Add(rn, Mul(Apply(x.nrm, fn), w));
        rt = Add(rt, Mul(Apply(x.lin, ft), w));
      }
      meshReposed[v] = rp;
      if (options.repose) {
        rn = Unit(rn);
        rt = Unit(rt);
      } else {
        rp = fp;
        rn = Unit(fn);
        rt = Unit(ft);
      }
      for (int a = 0; a < 3; ++a) {
        rm.pos[3 * size_t(v) + a] = float(rp[a]);
        rm.normal[3 * size_t(v) + a] = float(rn[a]);
        if (hasT) {
          rm.tangent[4 * size_t(v) + a] = float(rt[a]);
        }
      }
      if (hasT) {
        // The bitangent sign flips with the frame's handedness (a rotation keeps it).
        rm.tangent[4 * size_t(v) + 3] = vs.tangent[4 * size_t(v) + 3];
      }
    }
    dw.push_back(std::move(meshDw));
    reposed.push_back(std::move(meshReposed));
    out.meshes.push_back(std::move(rm));
  }

  // ---- 5. votes ----
  // For each vote joint, the bones the nearest retail vertices of its vertices carry, weighted
  // by the vertex's weight on it; a bone with at least kVoteShare of the total is kept.
  constexpr double kVoteShare = 0.15;
  std::map<int, std::vector<std::pair<int, double>>> voteBones;  // Dread joint -> (Prime index, share)
  std::vector<std::vector<uint32_t>> nearest(out.meshes.size());
  if (canVote) {
    const RetailSkinRef& r = *options.retail;
    const size_t nr = r.weights.size();
    std::map<int, std::map<int, double>> tally;
    for (size_t mi = 0; mi < out.meshes.size(); ++mi) {
      const ReskinMesh& rm = out.meshes[mi];
      nearest[mi].assign(rm.vertex_count, 0);
      for (uint32_t v = 0; v < rm.vertex_count; ++v) {
        bool votes = false;
        for (const auto& p : dw[mi][v]) {
          votes = votes || targets[size_t(p.first)].vote;
        }
        if (!votes) {
          continue;
        }
        const V3& q = reposed[mi][v];
        double best = -1.0;
        uint32_t at = 0;
        for (size_t i = 0; i < nr; ++i) {
          const double dx = q[0] - r.pos[3 * i], dy = q[1] - r.pos[3 * i + 1], dz = q[2] - r.pos[3 * i + 2];
          const double d = dx * dx + dy * dy + dz * dz;
          if (best < 0.0 || d < best) {
            best = d;
            at = uint32_t(i);
          }
        }
        nearest[mi][v] = at;
        for (const auto& p : dw[mi][v]) {
          if (!targets[size_t(p.first)].vote) {
            continue;
          }
          for (const auto& bw : r.weights[at]) {
            const int pi = skel.IndexOfId(bw.first);
            if (pi >= 0) {
              tally[p.first][pi] += double(p.second) * double(bw.second);
            }
          }
        }
      }
    }
    for (const auto& jt : tally) {
      double total = 0.0;
      for (const auto& b : jt.second) {
        total += b.second;
      }
      std::vector<std::pair<int, double>> kept;
      for (const auto& b : jt.second) {
        if (b.second >= kVoteShare * total) {
          kept.emplace_back(b.first, b.second / total);
        }
      }
      std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      if (kept.size() > 3) {
        kept.resize(3);
      }
      std::string line = "vote: " + model.joints[size_t(jt.first)].name + " ->";
      for (const auto& k : kept) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " %s %.2f", skel.joints[size_t(k.first)].name.c_str(), k.second);
        line += buf;
      }
      out.log.push_back(line);
      voteBones[jt.first] = std::move(kept);
    }
  }

  // ---- 6. final weights ----
  for (size_t mi = 0; mi < out.meshes.size(); ++mi) {
    ReskinMesh& rm = out.meshes[mi];
    rm.weights.resize(rm.vertex_count);
    rm.source.resize(rm.vertex_count);
    for (uint32_t v = 0; v < rm.vertex_count; ++v) {
      std::map<int, double> acc;  // Prime index -> weight
      bool table = false, voted = false;
      for (const auto& p : dw[mi][v]) {
        const Target& t = targets[size_t(p.first)];
        const auto vb = voteBones.find(p.first);
        if (t.vote && vb != voteBones.end() && !vb->second.empty()) {
          // Split by the nearest retail vertex's weights on the joint's bones, else by the
          // joint's shares.
          const auto& rw = options.retail->weights[nearest[mi][v]];
          double sum = 0.0;
          std::vector<double> part(vb->second.size(), 0.0);
          for (size_t b = 0; b < vb->second.size(); ++b) {
            for (const auto& bw : rw) {
              if (skel.IndexOfId(bw.first) == vb->second[b].first) {
                part[b] += bw.second;
              }
            }
            sum += part[b];
          }
          for (size_t b = 0; b < vb->second.size(); ++b) {
            const double share = sum > 0.0 ? part[b] / sum : vb->second[b].second;
            acc[vb->second[b].first] += p.second * share;
          }
          voted = true;
        } else {
          acc[t.prime] += p.second;
          table = true;
        }
      }
      std::vector<std::pair<int, double>> ws(acc.begin(), acc.end());
      std::stable_sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      if (ws.size() > kReskinMaxWeights) {
        ws.resize(kReskinMaxWeights);
      }
      double total = 0.0;
      for (const auto& w : ws) {
        total += w.second;
      }
      std::vector<std::pair<uint32_t, float>> q;
      if (total > 0.0) {
        for (const auto& w : ws) {
          const double r = std::nearbyint(w.second / total * 64.0);
          if (r != 0.0 && w.first >= 0) {
            q.emplace_back(skel.joints[size_t(w.first)].id, float(r));
          }
        }
      }
      if (q.empty()) {
        const int root = ws.empty() || ws[0].first < 0 ? RootCandidate(skel) : ws[0].first;
        q.emplace_back(skel.joints[size_t(root)].id, 1.f);
      }
      double s = 0.0;
      for (const auto& w : q) {
        s += w.second;
      }
      for (auto& w : q) {
        w.second = float(w.second / s);
      }
      std::sort(q.begin(), q.end());
      rm.weights[v] = std::move(q);
      rm.source[v] = voted ? (table ? WeightSource::Mixed : WeightSource::Vote) : WeightSource::Table;
      switch (rm.source[v]) {
      case WeightSource::Table:
        ++out.stats.verts_table;
        break;
      case WeightSource::Vote:
        ++out.stats.verts_vote;
        break;
      default:
        ++out.stats.verts_mixed;
        break;
      }
    }
  }
  return true;
}

} // namespace PortDread
