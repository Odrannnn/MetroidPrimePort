// Reader for the skeletal animations in a Metroid Prime Remastered character (CHPR)
// file. A port of build/mpr/anim/chpr_anim.py and chpr_skel.py (the notes next to them
// say how the format was found). It has no game or GX dependencies.
//
// A CHPR holds a pool of names, one compressed animation blob per animation
// (CAnimCompStream in the exe) and a reference to the skinned model (SMDL) it moves.
// Only the stream layouts the Python decoder knows are supported: rotation tracks (type 0)
// and vec3 tracks (types 1 and 2, linear) for rotation, translation and scale, with
// constant-pool values where an op names no track, in a single stream info. Anything else
// makes ReadCharacter fail with a message rather than guess.
//
// The skeleton (bones, their bind pose, the skin palette) is read from the same file.
// SkinPose combines it with an animation into the matrices a skinned model's joints use.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PortRemasteredAnim {

// One bone at one frame, in Remastered model space.
struct Key {
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // unit quaternion, x y z w
  float translation[3] = {0.0f, 0.0f, 0.0f};
  float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct Bone {
  std::string name;
  int parent = -1;  // bone index, -1 for a root
  // The abs op that places the bone: 4/5 plain, 6 inverse parent scale (SkinPose applies
  // it), 7-9 base / extract-inverse-scale forms (not applied), 0 unknown.
  uint8_t absOp = 0;
  bool invScale = false;
  Key bind;         // the bind pose relative to the parent (scale = the matrix column lengths)
};

struct Anim {
  std::string name;
  uint32_t id = 0;  // the record id in the file
  // Frames per second: 60 times the header float. The exe advances the frame count by
  // skNativeRate32 (60.0, 0x1d1f308) times the header float per second (CAnimCompStreamInst
  // Advance, CChar::AdvanceAnim). That makes the 749-frame debris/hologram loops last
  // 12.5 s, retail's 12.458 s spin; 240 frames are 4.0 s, retail's 4.0 s.
  float fps = 0.0f;
  uint32_t frames = 0;
  // bones[b][frame] for every frame 0..frames-1, sampled at integer frame times.
  // Bones the animation has no track for hold the identity key.
  // The index is the animation's own bone id; Character::animBone maps it to a skeleton bone.
  std::vector<std::vector<Key>> bones;
  // tracked[b]: the animation drives bone b (the same size as `bones`).
  std::vector<bool> tracked;
};

struct Character {
  // The SMDL the character skins, as the 16 bytes sit in the file. That is the
  // property order (Python's bytes_le), the same form a room property GUID has
  // there, so pass it through the same swap as port_remastered_room_internal.h's SwapUuid
  // (first three fields byte-reversed) to get the pak id the model lookup takes.
  // All zero when the file has no reference where the reader looks for it (it is
  // located by position after the animation records, which is only confirmed on the
  // elevator paddle character).
  std::array<uint8_t, 16> skinnedModel{};
  std::vector<Anim> anims;

  // The skeleton, empty (with skeletonError set) when the file's skeleton part is not
  // understood; the animations above are still good then.
  std::vector<Bone> bones;
  // Animation bone id -> index into `bones`, -1 when the name is not a bone.
  std::vector<int> animBone;
  // Skinned-model vertex joint index (the SMDL JOINTS_0 value) -> index into `bones`.
  std::vector<int> jointBone;
  // Per joint, the inverse bind matrix: 3x4 row-major, translation in column 3, in
  // Remastered model space.
  std::vector<std::array<float, 12>> inverseBind;
  std::string skeletonError;
};

// Parses a whole CHPR file. Returns false with a message in `error` for input that is
// malformed or uses a track layout that is not implemented; never reads out of range.
// With `anims` false the animations are not decoded (`anims` stays empty), so a file whose
// tracks are not implemented still gives its skeleton and model.
bool ReadCharacter(const std::vector<uint8_t>& chpr, Character& out, std::string& error, bool anims = true);

// The animation called `name`, or nullptr.
const Anim* Find(const Character& c, std::string_view name);

// The skin matrix of every joint at integer `frame` of `anim`: world(bone of joint) *
// inverseBind. World matrices compose parent * local from the roots down, where a bone's
// local is its bind pose times the animation's key for it if the animation has a track for
// it, else its bind pose. A key's scale is the bone's own, so the bind's scale is left out
// under a key (its rotation and translation are not). Each matrix is 3x4 row-major. Returns false (out is
// empty) without a skeleton, for a frame out of range or a damaged parent chain.
bool SkinPose(const Character& c, const Anim& anim, uint32_t frame, std::vector<std::array<float, 12>>& out);

}  // namespace PortRemasteredAnim
