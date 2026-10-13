#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CActor;
class CCubeModel;
class CFrustumPlanes;
class CGameArea;
class CModel;
class CStateManager;
class CTransform4f;
class CVector3f;

// A room's static geometry as a list of models and where each stands. A mod supplies one
// per area as <MREA id>.roomgeo, with the models as ordinary CMDL files; the port draws
// them in place of the area's own world geometry.
//
// The file is little endian:
//   'MPRG', u32 version (1 to 10), u32 instances
//   instance: u32 CMDL id, f32 transform[12] (rows of model -> area)
//     version 2 adds: u8 layer, u8 active, u16 links,
//     version 3 (and 4) then: u32 platform, f32 platformStart[3],
//     then (2 to 4) per link: u32 sender, u8 state, u8 action, u16 delay (1/100 s; 0 in
//     files made before delays, which read as none)
// An instance whose CMDL does not exist is skipped.
//
// Version 2 is for scenery Remastered added as actors, which its scripts show and hide:
// such an instance is drawn only while the area's script layer `layer` is on (kEveryLayer:
// always), starts shown or hidden by `active`, and changes when the retail object with
// editor id `sender` sends `state` (an EScriptObjectState).
//
// Version 3 adds the actors a platform carries (its Play -> Activate connections, which
// retail's CScriptPlatform::BuildSlaveList takes as slaves): `platform` is the retail
// platform's editor id (0: none) and `platformStart` where it stands in the world when its
// area is made. Such an instance moves by what the platform has moved since, as a slave
// is dragged by the platform's translation alone.
//
// Version 3 may end with Remastered's own script objects that show and hide geometry, which
// retail has no object for (Script):
//   'SCRP', u32 nodes, u32 edges
//   node: u8 kind, u8 active, u16 0, u32 counter max, f32 centre[3], half[3], axes[9]
//   edge: u8 retail, u8 event, u8 action, u8 0, u32 from, u32 to
//   then u32 group per instance (kNoGroup: none)
//
// Version 4 may then end with the instances that glow in a colour of their own:
//   'GLOW', u32 count, per instance: u32 index, f32 glow[3]
// Remastered colours its door frames' lights with a ColorModulateMP1 in its "incandescence"
// mode, which stands its colour B (times its intensity) in for the strength of every
// material's emissive map. `glow` is that colour.
//
// Version 5 may then end with Remastered's animated scenery actors (the Intro Elevator's
// spinning rings, for one):
//   'ANIM', u32 count, per animated instance:
//     u32 index, f32 fps, u32 frames (2 or more), then per frame f32 rotation x, y, z, w
//     (unit quaternion) and f32 translation x, y, z
//
// Version 8 gives an animated instance a list of clips instead of one looping clip. Its entry
// is: u32 index, u8 flags (bit 0: start on show), u8 clips (1 or more), u16 0, then per clip
// f32 fps, u32 frames (2 or more), u8 loop (0 or 1), u8[3] 0, then the frames as above. A
// clip that does not loop holds its last pose, and the instance moves on to the next clip
// when it ends (or when a kGroupNextClip edge says so). Entries of versions 5 to 7 are one
// looping clip that starts at the area's load.
//
// Version 6 may then end with the room's skies:
//   'SKY ', u32 count, per sky: u32 index, and from version 7 f32 radiance r, g, b
// The radiance is Remastered's Skybox colour times its intensity, which takes the place of
// the sky materials' diffuse colour (DIFC): the base map times it is the light the sky
// gives, in Remastered's HDR units; 0 (version 6) for not known. A sky instance is not
// drawn with the room: the world's sky is drawn as it, centred on the camera, turned and scaled by its transform (whose translation is left out). Its layer,
// `active`, links and group show and hide it as they do any instance's.
//
// Version 9 may then end with the retail objects the instances stand in for:
//   'HIDE', u32 count (1 or more), per object: u32 editor id (its low 26 bits, as
//   TEditorId::Value), u32 instance
// Such an object is not drawn while the room geometry stands in for its area and that
// instance is shown (Hides): the Frigate hangar's floating debris, which Remastered draws as
// one animated actor, or Omega's tank while Remastered's own explosion plays.
//
// Version 10 may then end with the instances' lookups into the room's baked lightmap (the
// area's .roomenv holds the lightmap itself):
//   'LMAP', u32 count (the instances'), per instance: f32 offU, offV, scale
// A model's atlas UV is (offU + u * scale, offV + v * scale) for its UV0 (u, v), as Remastered
// takes it from the instance's vec4(offU, offV, scale, 0). Scale 0 with an offset is one texel
// for the whole model (Remastered's props: 17,969 of its MCON instances); all three 0: none.
// The section is left out when no instance has a lookup.
namespace PortRoomGeo {

// Whether (offU, offV, scale) is a lookup into the room's lightmap (see above).
inline bool HasLightmapLookup(const float lookup[3]) {
  return std::isfinite(lookup[0]) && std::isfinite(lookup[1]) && std::isfinite(lookup[2]) && lookup[2] >= 0.f &&
         (lookup[0] != 0.f || lookup[1] != 0.f || lookup[2] != 0.f);
}

enum : uint8_t { kEveryLayer = 0xff };
enum : uint32_t { kNoGroup = 0xffffffff };
// kFollow (with state MaxReached): `sender` is a DamageableTrigger, and the instance is one
// of the actors retail's trigger shows and fades with itself (SetLinkedObjectAlpha): drawn
// at the trigger's puddle alpha while it is active, hidden once it goes inactive.
enum LinkAction : uint8_t { kShow = 1, kHide = 2, kToggle = 3, kFollow = 4 };

struct Link {
  uint32_t sender; // retail editor id, layer bits included
  uint8_t state;
  uint8_t action; // LinkAction
  // Seconds from the state to the action: the Remastered timers the link was traced through,
  // retail having none of them. Sent again before then, the wait starts over. Instances only.
  float delay = 0.f;
};

struct Instance {
  uint32_t model = 0;
  float transform[12] = {};
  uint8_t layer = kEveryLayer;
  bool active = true;
  std::vector<Link> links;
  uint32_t platform = 0; // retail editor id, layer bits included
  float platformStart[3] = {};
  uint32_t group = kNoGroup; // the Remastered entity the scripts show and hide it by
  bool glows = false;        // whether `glow` replaces its materials' emissive strength
  float glow[3] = {};
  // Remastered's animated scenery (ANIM): a rigid pose per frame, in model space, applied
  // before `transform`, played clip by clip from the area's load (or, with `animOnShow`, from
  // each time the instance goes from hidden to shown). Empty: it stands still.
  struct AnimClip {
    float fps = 0.f;
    bool loop = true;
    std::vector<float> keys; // 7 per frame: rotation x, y, z, w, translation x, y, z
  };
  std::vector<AnimClip> anim;
  bool animOnShow = false;
  bool sky = false; // the room's sky (version 6), drawn in place of the world's
  float skyRadiance[3] = {}; // version 7; 0: not known
  float lightmap[3] = {};    // version 10: offU, offV, scale into the room's lightmap; all 0: none
};

// Remastered's script objects between what happens in game and a group of instances.
enum NodeKind : uint8_t {
  // A TriggerMP1 that detects the camera: sends Entered (event 0) when the camera comes into
  // its box and Exited (1) when it leaves. The box is in area space: a point p is inside when
  // |dot(p - centre, axis i)| <= half[i] for each axis (axes[3i..3i+2], unit length).
  kCameraVolume = 1,
  // A Counter: kIncrement/kDecrement move it within 0..max; a change to 1 or more sends
  // MaxReached (event 2, at max) and then NonZero (0), a change to 0 sends Zero (1).
  kCounter = 2,
  // Remastered's Relay: kFire sends Fired (event 0).
  kRelay = 3,
};
enum ScriptAction : uint8_t {
  kIncrement = 1,
  kDecrement = 2,
  kFire = 3,
  kGroupShow = 4, // `to` is a group
  kGroupHide = 5,
  kGroupToggle = 6,
  kNodeActivate = 7, // `to` is a node; an inactive node takes no action and sends nothing
  kNodeDeactivate = 8,
  // `to` is a group: each animated instance of it starts its next clip now (on its last clip,
  // it restarts that one).
  kGroupNextClip = 9,
};

struct ScriptNode {
  uint8_t kind = 0;
  bool active = true;
  uint32_t max = 0;
  float centre[3] = {};
  float half[3] = {};
  float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// `from` is a node index, or for `retail` a retail editor id (layer bits included) whose
// state `event` (an EScriptObjectState) sets it off.
struct ScriptEdge {
  bool retail = false;
  uint8_t event = 0;
  uint8_t action = 0;
  uint32_t from = 0;
  uint32_t to = 0;
};

struct Script {
  std::vector<ScriptNode> nodes;
  std::vector<ScriptEdge> edges;
  struct Hidden {
    uint32_t editorId;
    uint32_t instance;
    bool operator==(const Hidden& o) const { return editorId == o.editorId && instance == o.instance; }
    bool operator<(const Hidden& o) const {
      return editorId != o.editorId ? editorId < o.editorId : instance < o.instance;
    }
  };
  std::vector<Hidden> hidden; // version 9, their own section
  bool Empty() const { return nodes.empty() && edges.empty(); }
};

// --- The file (port_room_geo_file.cpp; no game or GX dependencies) -------------

// "1A2B3C4D.roomgeo" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error,
           Script* script = nullptr);
// The file for these instances (version 9), with the script section when there is a script
// or a group, the glow section when an instance glows, the animation section when one is
// animated, the sky section when one is a sky and the hidden objects when there are any.
std::vector<uint8_t> Write(const std::vector<Instance>& instances, const Script* script = nullptr);

// The coarser levels of detail of the models, one table for the whole mod (kLodFileName in
// the folder of the .roomgeo files), little endian:
//   'RLOD', u32 version (1), u32 models
//   model: u32 CMDL id, u32 levels (1 to kLodLevels - 1),
//     per level, coarsest last: f32 distance squared it starts at, u32 CMDL id
// The distances are in model space, from the eye to the model's bounds.
constexpr const char* kLodFileName = "lods.bin";
constexpr int kLodLevels = 5; // the model itself and up to four coarser ones
struct LodLevel {
  float distanceSq = 0.f;
  uint32_t model = 0;
};
struct Lods {
  uint32_t model = 0;
  std::vector<LodLevel> levels;
};
bool ParseLods(const std::vector<uint8_t>& data, std::vector<Lods>& out, std::string& error);
std::vector<uint8_t> WriteLods(const std::vector<Lods>& models);

// --- The game side (port_room_geo.cpp) -----------------------------------------

// The areas in memory now. Loads the files of new ones and frees those of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);
// Draws the area's room geometry. True when it stands in for the area's own, which is
// when the area has a file, the mode says so and every model of it has loaded.
bool Draw(const CStateManager& mgr, const CGameArea& area, const CFrustumPlanes& frustum);
// Queues the blended surfaces of what Draw drew for the area this frame in the renderer's
// sorted pass; the renderer hands each back to DrawSorted through the state manager's
// drawable callback, as type kDrawableType.
enum { kDrawableType = 3 };
void AddSorted(const CGameArea& area);
void DrawSorted(const void* drawable);
// Set around the renderer's sorted pass for an area Draw stood in for, so that pass
// draws the actors and leaves the area's own surfaces out.
extern bool sReplacingArea;
// The skies themselves (Skies) are in port_room_sky.h.
// Whether the area has a sky of its own, shown and on a layer that is on.
bool HasSky(const CGameArea& area);
// The area DrawSky drew the skies of this frame (its MREA id, 0 for none). Set by DrawSky.
extern uint32_t sSkyDrawnFor;
// Whether `actor` of the area is one of retail's sky domes (Tallon's "cloud layer" actors,
// hundreds of units across), which the room's own skies stand in for while one is drawn.
bool HidesSky(const CGameArea& area, const CActor& actor);
// Whether the retail object `editorId` of the area is one its room geometry stands in for
// right now (Script::hidden, its instance shown). The caller asks only while that geometry
// is drawn in its place.
bool Hides(const CGameArea& area, uint32_t editorId);
// Lets go of every model (the mods folder is about to change).
void Reset();
// A script object sent a state (CEntity::SendScriptMsgs): shows or hides the instances
// linked to it. Loads the area's file if the area is newer than the last SetLoadedAreas,
// since objects send states while their area is still being set up.
void OnScriptState(CStateManager& mgr, uint32_t editorId, int state);
// Once a frame, where the game's objects think (CStateManager::Update): runs the camera
// volumes of every loaded area's Script against the current camera, as CScriptTrigger
// does for its own, and plays the animated instances on by `dt` seconds.
void Think(CStateManager& mgr, float dt);
// The console's `roomgeo script`: each loaded area's Script now (camera, nodes, groups).
std::string ScriptInfo();
// The console's `roomgeo group <n> show|hide`: sets every loaded area's group n until its
// script next changes it; how many instances it set.
int SetGroupShown(uint32_t group, bool shown);
// What the area's script last did to group `group`: 1 shown, 0 hidden, -1 nothing yet (or no
// such area or group; a toggle of a group it has not set shows it).
int GroupShown(uint32_t mrea, uint32_t group);
// A new game, a death or a save state builds a new CStateManager: every instance goes back
// to how the file starts it, since the scripts will not resend what already happened.
void ResetScriptState();

// Whether the frame's buffers were sized for room geometry at startup (main.cpp). Until
// they are, nothing is drawn: a room of it overflows the default ones.
void SetBuffersReady(bool ready);
bool BuffersReady();
// Whether Aurora set room aside at startup to keep models on the GPU (main.cpp): each model's
// vertex arrays and display lists are then retained when it loads (GXPortRetainResident)
// and released before it goes, so its draws no longer copy them into the frame's buffers.
void SetResident(bool resident);
bool Resident();

enum class Mode {
  Off,
  Replace, // in place of the area's world geometry
  Overlay, // on top of it
};
// MP_ROOM_GEO=0|1|overlay, the console's `roomgeo`.
void SetMode(Mode mode);
Mode GetMode();
// Whether room geometry takes the area's lights where the room has baked light too
// (MP_ROOM_GEO_AREA_LIGHTS, the console's `roomgeo lights`).
void SetAreaLights(bool on);
bool AreaLights();
// Instances whose bounds span fewer pixels than this, in the game's own resolution (its
// 640x480-ish viewport, whatever the render scale), are left out. 0 draws every one.
// A room is thousands of small props, and on a phone the draws cost more than the
// triangles do.
void SetMinPixels(float pixels);
float MinPixels();
// Off draws merged copies one by one again, each with its own lights (an A/B check; not saved).
void SetMergedDraws(bool on);
bool MergedDraws();
// Draws the opaque room models nearest first, the cut-out ones after them, so the GPU skips
// the shading of what is already covered. Off goes model by model, to keep pipelines bound
// (an A/B check; not saved).
void SetFrontToBack(bool on);
bool FrontToBack();
// Out-of-view room models in the sun's shadow map still cast into it (an A/B check; not saved).
void SetOffscreenCasters(bool on);
bool OffscreenCasters();
// How many out-of-view models the last whole frame drew caster-only.
uint32_t OffscreenCasterCount();
// With FrontToBack, draws the cut-out models (grass, leaves) once for depth only, then shaded
// where the depth is equal, so what they hide of each other isn't shaded. Looks the same
// (an A/B check; not saved).
void SetDepthPrepass(bool on);
bool DepthPrepass();
// Scales the distances where an instance switches to a coarser level of detail (the
// import's lods.bin): 1 is Remastered's own, 2 keeps the full model twice as far, 0 never
// switches (MP_ROOM_GEO_LOD, the console's `roomgeo lod`). Merged copies keep the full one.
void SetLodDistance(float scale);
float LodDistance();
// Coarser levels in the loaded areas, how many have loaded, and the instances drawn with
// one in the last frame.
void LodStats(int& levels, int& loaded, int& drawnCoarse);
// Areas with a file, their instances, the distinct models and how many have loaded, and
// the instances drawn in the last frame.
void Stats(int& areas, int& instances, int& models, int& loaded, int& drawn);


// For finding which model a surface belongs to (the console's `roomgeo at|hide|show`).
// One line per drawn instance whose box holds the point, give or take the margin.
std::string At(const CVector3f& point, float margin);
// Stops or resumes drawing a model, or every model for id 0; how many it matched.
int SetHidden(uint32_t id, bool hidden);
// What a ray meets (the console's `roomgeo pick`): one line per instance whose box it
// passes through, nearest first, then the boxes the origin is already inside, smallest
// first. Boxes, not triangles, so the surface looked at is among the first few. Returns
// the first one's model, 0 for none.
uint32_t Pick(const CVector3f& origin, const CVector3f& direction, std::string& out);
// One line per material of a loaded model: flags, whether it is drawn through PBR, and
// its record (see CCubeModel::PortSetPBRMaterial). Empty when no loaded model has the id.
// Models other than room geometry (actors, characters, the viewmodel) are found once
// they have drawn since `drawlog on` or `view drawid`; the values set with SetMaterialValue
// go to the model instance found then, so they lapse when that model is freed.
std::string Materials(uint32_t id);
// One material's line of Materials, for a model that is already in hand (the `pick` command).
std::string MaterialLine(const CCubeModel* cube, uint32_t id, int material);
// "roomgeo <MREA>" when the model is one of a room geometry's, else empty.
std::string Owner(const CCubeModel* cube);
// Draws a model's material with one value of its record replaced (index 0 to 18), until
// cleared; kept across room loads. False when no loaded model has that material.
bool SetMaterialValue(uint32_t id, int material, int field, float value);
int ClearMaterialValues();

} // namespace PortRoomGeo
