#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CStateManager;
#ifdef TARGET_PC
struct SPortExternalVar;  // Kyoto/Particles/CPortParticleVars.hpp
#endif

// Remastered's own particle effect actors (EffectMP1 entities the disc has no object for),
// added to an area as retail Effect objects. A mod supplies them per area as
// <MREA id>.roomfx (the Remastered import writes it); while the area loads its script
// objects, each active layer's add-ons are created after the layer's own, and the retail
// objects the file names are not created (the effects Remastered replaced).
//
// The add-ons live outside the area's layer buffers, so nothing saves them: a save, a save
// state, a memory relay and the randomizer's dumps see only the retail objects. An effect a
// script object talks to is added only when every connection into it traces back to retail
// senders (Drive); the import counts the others.
//
// Their editor ids are the area's, on the retail layer they load with, with instance numbers
// from kFirstInstance up: no retail area uses those (the import checks).
//
// The file is little endian:
//   'MPRF', u32 version (4, or 5 when placement overrides are present)
//   u32 skips, u32 editor id[skips] (retail objects not to create, layer bits included)
//   u32 connected (effects left out because something connects to them; for the log)
//   u32 layers, then per layer: u32 retail layer, u32 objects, u32 bytes, then that many
//   bytes of retail SCLY objects (u8 type, then big endian: u32 length, u32 editor id,
//   u32 connections (0), u32 property count, properties), all of type Effect (7);
//   version 2 then has, per object in order, u32 variables and that many Variable
//   records (16 guid bytes as the particle tables store them, f32 colour[4], f32 vector[3],
//   f32 real, s32 int). Version 1 files have no variables.
//   version 3 then has u32 overrides, then per override: u32 editor id (layer bits included),
//   u32 part (0xFFFFFFFF for none), u32 variables and that many Variable records. An override
//   is a retail Effect object a Remastered effect actor matched: the object stays, with its
//   connections, but takes the actor's PART and its particle variables.
//   version 4 then has u32 barriers (0 or 1), then per barrier: u32 controller editor id,
//   f32 duration, then 5 ring records (bottom to top), each u32 platform, u32 refraction,
//   u32 glow and three 16 byte guids (breakup refraction, motion, breakup glow), in the byte
//   order Variable::guid has. A barrier names retail script objects (the controller and each
//   ring's platform) and the add-ons this same file adds (each ring's refraction and glow
//   effect); the codec only checks what the file itself must hold, and says nothing about
//   which room it is: versions 1 to 3 simply have no barriers.
//   version 5 appends one placement flag and optional nine floats per override
//   (see Override). Files without placement overrides are still written as version 4.
//   version 6 then has u32 patterned, then per record: u32 editor id (layer bits included),
//   u32 slot, u32 part, u32 offset flag and (when set) f32 offset[3] (see PatternedEffect).
//   Files without patterned records are still written as version 5 or 4.
//   version 7 then has u32 patches, then per patch: u32 editor id (layer bits included),
//   u32 offset, u32 retail value, u32 part (see PropertyPatch). Files without patches are
//   still written as version 6 or older.
//   version 8 then has u32 steams, then per record: u32 editor id (layer bits included),
//   u32 part, f32 colour[4], f32 a, b, g, h, i, u32 colour flag, u32 bytes and that many
//   bytes of each of the two splines (CMayaSpline data), u32 sources and per source u32
//   editor id (0 for none), f32 position[3] (see Steam). Files without steams are still
//   written as version 7 or older.
//   version 9 then has u32 actor slots, then per record: u32 editor id (layer bits included),
//   u32 slot, u32 asset (see ActorSlotEffect). Files without them are still written as
//   version 8 or older.
//   version 10 then has u32 drives, then per record: u32 add-on editor id, u32 sender editor id
//   (both layer bits included), u32 state, u32 message, f32 delay (see Drive). Files without
//   them are still written as version 9 or older.
//   version 11 appends u32 flags to each drive record (see Drive), then has u32 generators,
//   then per generator: u32 flags (see Generator), f32 offset[3], u32 templates and that many
//   add-on editor ids. Files without generators are still written as version 10 or older.
//   version 12 then has u32 timers, then per timer: u32 add-on editor id, f32 start time, f32
//   max random delay, u32 flags (see Timer). Files without timers are still written as version
//   11 or older.
//   version 13 then has u32 follows, then per follow: u32 add-on editor id, u32 followed editor
//   id, u32 follower editor id, u32 flags (see Follow), u32 locator length and that many bytes.
//   Files without follows are still written as version 12 or older.
//   version 14 then has u32 triggers, then per trigger: u32 add-on editor id, f32 position[3],
//   f32 extent[3], u32 trigger flags, u32 flags (see Trigger). Files without triggers are still
//   written as version 13 or older.
//   version 15 then has u32 relays, then per relay: u32 add-on editor id, u32 flags (see Relay).
//   Files without relays are still written as version 14 or older.
//   version 16 then has u32 ambients, then per ambient: f32 rate, u32 owner add-on editor id, f32
//   sphere offset[3], f32 sphere radius, u32 weights and that many i32, u32 emitters and per
//   emitter u32 index and u32 add-on editor id, u32 shapes and per shape u32 type, f32 center[3], f32 axes[9], f32 half[3]
//   (see Ambient). Files without ambients are still written as version 15 or older.
//   version 17 then has u32 hidden, u32 editor id[hidden] (layer bits included): retail Effect
//   objects Remastered has no effect for that other objects still talk through. They are
//   created, with their connections, but draw nothing and light nothing. Files without them
//   are still written as version 16 or older.
namespace PortRoomEffects {

constexpr uint32_t kFileMagic = 0x4652504D;  // 'MPRF'
constexpr uint32_t kFileVersion = 17;
constexpr uint32_t kMaxSteamSplineBytes = 0x1000;
constexpr uint32_t kMaxSteamSources = 64;
constexpr uint32_t kMaxPatchOffset = 0x10000;
constexpr uint8_t kEffectType = 7;
constexpr uint32_t kEffectProps = 24;
constexpr uint32_t kFirstInstance = 0xF000;
constexpr uint32_t kMaxAddOns = 0x10000 - kFirstInstance;
constexpr size_t kVariableBytes = 52;
constexpr uint32_t kMaxVariables = 256;  // an object's; Remastered's most is 14
constexpr uint32_t kBarrierRings = 5;
constexpr uint32_t kMaxBarrierGroups = 1;
constexpr size_t kBarrierRingBytes = 3 * 4 + 3 * 16;
constexpr size_t kBarrierGroupBytes = 8 + kBarrierRings * kBarrierRingBytes;
constexpr float kMaxBarrierDuration = 60.f;  // seconds

// Whether an editor id is in the add-ons' range.
constexpr bool IsAddOnId(uint32_t eid) { return (eid & 0xFFFF) >= kFirstInstance; }
// The editor id of an area's `index`th add-on on `layer`; `area` is the area bits of the
// area's own editor ids.
constexpr uint32_t AddOnId(uint32_t layer, uint32_t area, uint32_t index) {
  return (layer & 0x3F) << 26 | (area & 0x3FF) << 16 | (kFirstInstance + index);
}

// One retail Effect object's properties (ScriptLoader::LoadEffect, in its order).
struct Effect {
  uint32_t id = 0;
  std::string name;
  float pos[3] = {0, 0, 0};
  float rot[3] = {0, 0, 0};  // degrees, as retail's editor transform takes them
  float scale[3] = {1, 1, 1};
  uint32_t part = 0xFFFFFFFF;
  bool hotInThermal = false;
  bool noTimerUnlessAreaOccluded = false;
  bool rebuildSystemsOnActivate = false;
  bool active = true;
  bool useRateInverseCamDist = false;
  float rateInverseCamDist = 5.0f;
  float rateInverseCamDistRate = 0.5f;
  float duration = 0.2f;
  float durationResetWhileVisible = 0.1f;
  bool useRateCamDistRange = false;
  float rateCamDistRangeMin = 20.0f;
  float rateCamDistRangeMax = 30.0f;
  float rateCamDistRangeFarRate = 0.0f;
  bool combatVisorVisible = true;
  bool thermalVisorVisible = true;
  bool xrayVisorVisible = true;
  bool dieWhenSystemsDone = false;
};

// A Remastered effect actor's value for one of its system's particle variables
// (EffectMP1 property 3def0de6): one value of each type, the variable's own type picks one.
struct Variable {
  std::array<uint8_t, 16> guid{};
  float color[4] = {1, 1, 1, 1};
  float vec[3] = {0, 0, 0};
  float real = 0;
  int32_t integer = 0;
};

struct Layer {
  uint32_t layer = 0;
  uint32_t count = 0;
  std::vector<uint8_t> objects;               // `count` SCLY objects
  std::vector<std::vector<Variable>> vars;    // per object; empty, or `count` of them
};

// A retail Effect object a Remastered effect actor matched (the script matcher paired them):
// it is created as retail has it, with its connections, then given this PART (0xFFFFFFFF for
// none the mod has) and these particle variables.
struct Override {
  uint32_t id = 0;
  uint32_t part = 0xFFFFFFFF;
  std::vector<Variable> vars;
  // Version 5 appends, per override, a u32 flag and (when set) nine f32s:
  // world position, editor Euler degrees and scale. Older files keep retail placement.
  bool hasPlacement = false;
  float pos[3] = {};
  float rot[3] = {};
  float scale[3] = {1, 1, 1};
};

// One of a barrier's five rings: its retail Platform object and the
// two add-ons it shows, with the three particle table guids that drive the breakup.
struct BarrierRing {
  uint32_t platform = 0;
  uint32_t refraction = 0;
  uint32_t glow = 0;
  std::array<uint8_t, 16> breakupRefraction{};
  std::array<uint8_t, 16> motion{};
  std::array<uint8_t, 16> breakupGlow{};
};

// The Intro Boss barrier: its retail SpecialFunction controller, imported breakup
// duration and five explicit platform/effect associations.
struct BarrierGroup {
  uint32_t controller = 0;
  float duration = 0;
  std::array<BarrierRing, kBarrierRings> rings{};
};

// One CPatterned death particle slot of a retail enemy the script matcher paired with a
// Remastered one: its PatternedInfo names a PART (and an offset) the mod has. Slot 0 is the
// death explosion (CPatternedInfo::x11c_particle1, offset x110_particle1Scale), slot 1 the ice
// death explosion (x130_particle2, offset x124_particle2Scale). Without an offset the retail
// offset stays.
constexpr uint32_t kPatternedSlots = 2;
struct PatternedEffect {
  uint32_t id = 0;
  uint32_t slot = 0;
  uint32_t part = 0;
  bool hasOffset = false;
  float offset[3] = {};
};

// A PART asset id property of a retail object the script matcher paired with a Remastered
// one, replaced by the PART the mod has: `offset` counts bytes of the object's properties from
// just after its name's NUL (fixed per object type), where the big endian u32 must still be
// `retail` for the patch to apply (a different disc revision leaves it alone).
struct PropertyPatch {
  uint32_t id = 0;
  uint32_t offset = 0;
  uint32_t retail = 0;
  uint32_t part = 0;
};

// A retail Steam object the script matcher paired with a Remastered SteamMP1: it shows
// Remastered's visor steam (CVisorSteamShip1) instead of retail's. The properties are
// SteamMP1's (SLdr defaults when the instance leaves them out): colour faa2682b, PART
// 4054322b, splines 8030d7a6 (s1) and 911d77bd (s2, both by distance to the eye), a d560318d,
// b 14e3228a, g 5e432bd2, h 67d639e2, i 6b6d170d, colour flag 1c983b7f. `sources` are the
// objects its 0x2ae68d05 link names: the retail object a source paired with (0 for none) and
// where the source stands, for when no retail object lives.
struct SteamSource {
  uint32_t id = 0;
  float pos[3] = {};
};
struct Steam {
  uint32_t id = 0;
  uint32_t part = 0xFFFFFFFF;
  float color[4] = {1, 1, 1, 1};
  float a = 0.8f;
  float b = 0.25f;
  float g = 2.f;
  float h = 1.f;
  float i = 0.5f;
  bool colorFlag = true;
  std::vector<uint8_t> s1;
  std::vector<uint8_t> s2;
  std::vector<SteamSource> sources;
};

// An effect Remastered gives a retail object the script matcher paired, in a slot retail's
// properties have no field for: `part` is the port asset the mod has (its type is the slot's).
// Slots 0-3 are the Metroid Prime relay's four beams' electric (ELSC): Remastered's beam
// struct 5d9bd2fe (SLdr +0x170), copied to the beam record's +0x184 by
// CMetroidPrimeDataMP1::LoadParasiteQueenBeams (0xe2d3dc), to CMetroidPrimeMP1 +0x12ac + i*0x10
// by its constructor (0xe2ec30), and given to the beam's plasma projectile by
// CreateParasiteQueenProjectile (0xe31518).
constexpr uint32_t kActorSlotExoBeamElectric = 0;  // to 3, by beam
// Slot 4 is a GunTurret's visor effect (SLdr +0xc0, de43a94b): an ELC2's ELSC, which
// CScriptGunTurretMP1 gives its projectiles for the HUD billboard (LaunchProjectile 0xee4bbc,
// CGameProjectileMP1::ResolveCollisionWithActor 0x1018f28), or a GENP's PART.
constexpr uint32_t kActorSlotGunTurretVisorEffect = 4;
// Slot 5 is a Drone's scanner effect (SLdr +0x8, d6a76aad), a GENP CDroneMP1GOC::Create
// (0xe943d8) builds into the generator CDroneMP1 keeps at +0x818 in place of retail's
// LaserLight.
constexpr uint32_t kActorSlotDroneScannerEffect = 5;
constexpr uint32_t kActorSlots = 6;
struct ActorSlotEffect {
  uint32_t id = 0;
  uint32_t slot = 0;
  uint32_t part = 0;
};

// A connection into an add-on, as retail would have it on the sender: when the retail object
// `sender` sends `state`, the add-on `target` is sent `message` (Activate, Deactivate or
// ToggleActive), `delay` seconds later when Remastered passed it through timers retail has no
// object for (sent again before then, the wait starts over).
constexpr float kMaxDriveDelay = 600.f;  // seconds
// With kDriveToGenerator, `target` is the index of one of the file's generators and the
// message is SetToZero (13) too. kDriveRelayed: Remastered passed it through relays or timers,
// so the object its target hears it from is not `sender` (a generator's copies then keep their
// template's placement).
constexpr uint32_t kDriveToGenerator = 1;
constexpr uint32_t kDriveRelayed = 2;
struct Drive {
  uint32_t target = 0;
  uint32_t sender = 0;
  uint32_t state = 0;
  uint32_t message = 0;
  float delay = 0.f;
  uint32_t flags = 0;
};

// A Remastered GeneratorMP1 whose Generate (47524e4c) connection reaches an add-on: on SetToZero,
// while active, it makes the copies `templates` lists, in order (CScriptGeneratorMP1::AcceptScriptMsg
// 0xf44128, retail CScriptGenerator's). It picks each at random among its templates and follows;
// the import keeps only generators with one template and no Follow targets, so `templates` is
// that one, spawn count times. Sent directly by an actor, a copy takes the actor's transform
// (unless kGeneratorNoInheritXf) and its translation plus `offset` (a wall crawler swarm's last
// kill instead of its translation); else it keeps the template's. Then it is sent Activate.
// Activate, Deactivate and ToggleActive switch the generator.
constexpr uint32_t kGeneratorActive = 1;
constexpr uint32_t kGeneratorNoInheritXf = 2;
constexpr uint32_t kMaxGeneratorTemplates = 64;
struct Generator {
  uint32_t flags = 0;
  float offset[3] = {0, 0, 0};  // GameCube world space
  std::vector<uint32_t> templates;  // the copies, by add-on id (one may repeat)
};

// A Remastered TimerMP1 retail has no object for, created as a retail Timer (ScriptLoader::LoadTimer,
// CScriptTimer) where it is exact: the flags are its loop, autostart and active. Drives send it
// its messages (Activate, ResetAndStart, ...), and its Zero sends the drives it is the sender of.
// Remastered's restart flag (CScriptTimerMP1 +0x12) comes as loop with autostart.
constexpr uint8_t kTimerType = 5;
constexpr uint32_t kTimerLoop = 1;
constexpr uint32_t kTimerAutoStart = 2;
constexpr uint32_t kTimerActive = 4;
constexpr float kMaxTimerTime = 600.f;  // seconds
struct Timer {
  uint32_t id = 0;
  float startTime = 0.f;
  float maxRandDelay = 0.f;
  uint32_t flags = kTimerActive;
};

// A Remastered SpecialFunctionMP1 of function 3 (ObjectFollowLocator) retail has no object for,
// created as a retail SpecialFunction of that function (ScriptLoader::LoadSpecialFunction,
// CScriptSpecialFunction::ThinkObjectFollowLocator), which Remastered's (0xd951a8) is: its
// Play -> Activate connection names the actor it follows, Play -> Deactivate the one it moves
// to that actor's locator. Remastered's turret explosions follow their gun this way.
constexpr uint8_t kSpecialFunctionType = 0x3A;
constexpr uint32_t kFollowLocatorFunction = 3;
constexpr uint32_t kFollowActive = 1;
constexpr uint32_t kMaxLocatorName = 64;
struct Follow {
  uint32_t id = 0;
  uint32_t followed = 0;  // a retail object
  uint32_t follower = 0;  // a retail object or an add-on
  uint32_t flags = kFollowActive;
  std::string locator;
};

// A Remastered TriggerMP1 retail has no object for, created as a retail Trigger
// (ScriptLoader::LoadTrigger, CScriptTrigger) where it is exact: no force, no damage, and none
// of KillOnEnter, DetectCamera or UseBooleanIntersection (CTriggerMP1GOC::Create 0xf3f230).
// Drives send it Activate, Deactivate and ToggleActive; its Entered and Exited send the drives
// it is the sender of.
constexpr uint8_t kTriggerType = 4;
constexpr uint32_t kTriggerKillOnEnter = 0x800;
constexpr uint32_t kTriggerDetectCamera = 0x4000;
constexpr uint32_t kTriggerBooleanIntersection = 0x8000;
constexpr uint32_t kTriggerRejectedFlags = kTriggerKillOnEnter | kTriggerDetectCamera | kTriggerBooleanIntersection;
constexpr uint32_t kTriggerActive = 1;
constexpr uint32_t kTriggerDeactivateOnEntered = 2;
constexpr uint32_t kTriggerDeactivateOnExited = 4;
struct Trigger {
  uint32_t id = 0;
  float pos[3] = {0, 0, 0};     // GameCube world space, the box's centre
  float extent[3] = {0, 0, 0};  // the box's size, world axes
  uint32_t triggerFlags = 1;    // ETriggerFlags
  uint32_t flags = kTriggerActive;
};

// A Remastered ScriptRelayMP1 retail has no object for, created as a retail Relay
// (ScriptLoader::LoadFaultyRelay, CScriptRelay), which Remastered's is (CScriptRelayMP1::
// AcceptScriptMsg 0xf5e728, Think 0xf5e864). Drives send it SetToZero, Activate, Deactivate and
// ToggleActive; its Zero sends the drives it is the sender of.
constexpr uint8_t kRelayType = 0x15;
constexpr uint32_t kRelayActive = 1;
struct Relay {
  uint32_t id = 0;
  uint32_t flags = kRelayActive;
};

// A Remastered AmbientParticleEffect (CAmbientParticleEffectGOC::BeforeThink 0x4ba150): each
// frame it adds rate * dt points (the fraction carried over) to draw, at random in a sphere
// `offset` ahead of the camera (its current transform, camera axes), keeps those inside any of
// its shapes (NTouchableUtils::ErasePointsInsideShapeList 0x897be0) and starts one particle at
// each in the effects it links (SpawnParticlesAtPositions 0x4bb0a0, link data `index`): the
// points split by `weights` between the indices, the rest at random. The rate is the sphere's
// volume times the density (+0xd8) over the lifetime (+0xdc). Its two splines, of the GPU
// availability, are identity and are taken at full availability. It spawns while the add-on its
// own entity's EffectMP1 became (`owner`) is active.
constexpr uint32_t kAmbientBox = 0;       // COBBoxShapeData cef3fcf1
constexpr uint32_t kAmbientCylinder = 1;  // CCappedCylinderShapeData 06c7e0ae
constexpr uint32_t kAmbientSphere = 2;    // CSphereShapeData 6ede23e6
constexpr uint32_t kMaxAmbientShapes = 256;
constexpr uint32_t kMaxAmbientWeights = 16;
constexpr uint32_t kMaxAmbientEmitters = 16;
constexpr float kMaxAmbientRate = 1e6f;  // points per second
struct AmbientShape {
  uint32_t type = kAmbientBox;
  float center[3] = {0, 0, 0};  // GameCube world space
  // A box: its axes (rows, world space) and half extents along them. A cylinder: axes[0] its
  // axis, half[0] half its height and half[1] its radius. A sphere: half[0] its radius.
  float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  float half[3] = {0, 0, 0};
};
struct AmbientEmitter {
  uint32_t index = 0;
  uint32_t id = 0;  // the effect: an add-on, or a retail one drawing the GENP
};
struct Ambient {
  float rate = 0.f;
  uint32_t owner = 0;            // its own effect, as AmbientEmitter::id
  float offset[3] = {0, 10, 0};  // camera space, GameCube axes
  float radius = 15.f;
  std::vector<int32_t> weights;
  std::vector<AmbientEmitter> emitters;
  std::vector<AmbientShape> shapes;
};

struct File {
  std::vector<uint32_t> skips;
  uint32_t connected = 0;
  std::vector<Layer> layers;
  std::vector<Override> overrides;
  std::vector<BarrierGroup> barriers;
  std::vector<PatternedEffect> patterned;
  std::vector<PropertyPatch> patches;
  std::vector<Steam> steams;
  std::vector<ActorSlotEffect> actorSlots;
  std::vector<Drive> drives;
  std::vector<Generator> generators;
  std::vector<Timer> timers;
  std::vector<Follow> follows;
  std::vector<Trigger> triggers;
  std::vector<Relay> relays;
  std::vector<Ambient> ambients;
  std::vector<uint32_t> hidden;
};

// Appends the effect as one SCLY object. Its light parameters are retail's most common
// Effect lighting.
void AppendEffect(const Effect& effect, std::vector<uint8_t>& out);
// Reads one SCLY object written by AppendEffect at `data`; `used` is its size.
bool ReadEffect(const uint8_t* data, size_t size, Effect& out, size_t& used);
std::vector<uint8_t> Write(const File& file);
// Checks every object's framing, type and id range too, and a version 4 barrier group: its
// duration, its members' ids and guids, the effects it names and where they live. `out` is
// only ever the whole file, or empty when this returns false (a failed parse accepts nothing).
bool Parse(const std::vector<uint8_t>& data, File& out, std::string& error);
// Reads an EffectMP1's external variable list (property 3def0de6: u32 count, then per entry a
// property list with the variable guid, in an 0xae893172 list as 0x82dde61c, and at most one
// value, real 0xfd41dc4b, int 0xcd774f7a, colour 0x45eff31d or vector 0x282df928; the others
// keep their defaults). False if it is malformed.
bool ReadExternalVariables(const uint8_t* data, size_t size, std::vector<Variable>& out);
// `vars` as CScriptEffect::PortSetExternalVars takes them (a value of each type per record).
#ifdef TARGET_PC
std::vector<SPortExternalVar> ExternalVars(const std::vector<Variable>& vars);
#endif
// "1A2B3C4D.roomfx" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);

// MP_REMASTERED_EFFECT_ACTORS=0 turns the add-ons (and the skips) off.
bool Enabled();
// Around CGameArea::LoadScriptObjects: reads the area's file.
void BeginArea(uint32_t mrea);
void EndArea();
// Whether the retail object `eid` is one the area being loaded leaves out.
bool Skips(uint32_t eid);
// Whether the retail Effect `eid` in the area being loaded is created hidden (File::hidden).
bool Hidden(uint32_t eid);
// The override of the retail object `eid` in the area being loaded, or null.
const Override* FindOverride(uint32_t eid);
// The death particle `slot` (PatternedEffect) of the retail object `eid` in the area being
// loaded, or null.
const PatternedEffect* FindPatternedEffect(uint32_t eid, uint32_t slot);
// The steam record of the retail Steam object `eid` in the area being loaded, or null.
const Steam* FindSteam(uint32_t eid);
// The actor slot effect `slot` (ActorSlotEffect) of the retail object `eid` in the area being
// loaded, or null.
const ActorSlotEffect* FindActorSlotEffect(uint32_t eid, uint32_t slot);
// Whether the retail object `eid` in the area being loaded has property patches.
bool HasPatches(uint32_t eid);
// Applies them to `props` (the object's properties from its name on; `size` bytes): finds
// the name's NUL, then writes each patch's PART (big endian) where the retail value still
// stands and `canBuild` accepts the PART. Returns how many applied.
size_t ApplyPatches(uint32_t eid, uint8_t* props, size_t size, bool (*canBuild)(uint32_t part));
// The same over `patches` (no area needed).
size_t ApplyPatches(const std::vector<PropertyPatch>& patches, uint32_t eid, uint8_t* props, size_t size,
                    bool (*canBuild)(uint32_t part));
// Creates the add-ons of `layer` in area `area` (TAreaId) and adds their editor ids to `ids`,
// for InitializedInArea.
void LoadLayer(CStateManager& mgr, int area, uint32_t layer, std::vector<uint32_t>& ids);

struct Spawned {
  uint32_t mrea;
  uint32_t eid;
  uint16_t uid;
  uint32_t part;
  std::string name;
  size_t vars;  // particle variables it sets
};
// From CEntity::SendScriptMsgs, after the sender's own connections: the add-ons of the
// sender's area its state drives get their messages (or wait their delay).
void OnScriptState(CStateManager& mgr, uint16_t senderUid, uint32_t editorId, int state);
// Delivers the delayed messages that are due; drops those of areas no longer loaded.
void Think(CStateManager& mgr, float dt);

// The add-ons created for an area, as loaded (the console checks they still live); a timer
// has part 0xFFFFFFFF.
std::vector<Spawned> List(uint32_t mrea);
// The retail objects left out when the area last loaded.
size_t SkippedCount(uint32_t mrea);
// A loaded area's drives, with how many messages each still holds back.
std::vector<std::pair<Drive, size_t>> DrivesOf(uint32_t mrea);
// A loaded area's generators: whether each is active, and how many copies it made.
struct GeneratorState {
  Generator generator;
  bool active;
  size_t made;
};
std::vector<GeneratorState> GeneratorsOf(uint32_t mrea);

// The area's ambient spawners, with how many points fell inside their shapes so far.
struct AmbientState {
  Ambient ambient;
  size_t spawned;
};
std::vector<AmbientState> AmbientsOf(uint32_t mrea);

}  // namespace PortRoomEffects
