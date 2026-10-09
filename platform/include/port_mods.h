#pragma once

// Mods folder: files served over the disc through Aurora's DVD overlays.
//
// Each folder in <pref>/mods (or MP_MODS) is a mod, applied in name order, so
// a later mod wins over an earlier one. Inside a mod:
//  - a file at a disc path (Metroid1.pak, Audio/frigate.dsp, Video/attract0.thp)
//    replaces that disc file;
//  - a file named <8 hex digits>.<4 letters> (1A2B3C4D.TXTR), anywhere in the
//    mod, replaces that resource in every PAK that holds it. The PAK is served
//    as a virtual file: its table patched to point past the original data, where
//    the loose file is appended. An id no PAK holds is added to NoARAM.pak,
//    which stays loaded from boot, so mods can bring new resources. What
//    would take NoARAM.pak past kMaxFileSize goes into new PAKs instead
//    (ExtraPakName), which the game loads right after it.
//  - a file named <8 hex digits>.dds, anywhere in the mod, is the full-size
//    image of the TXTR with that id (BC7, BC5, BC3, BC1 or RGBA8, with mips).
//    The TXTR itself still loads (the mod's own small one, or the disc's) and
//    supplies the sampler state; the .dds is what gets drawn, streamed by
//    Aurora outside the game heap, so its size does not count against the
//    arena. A mod that brings a TXTR without a .dds drops an earlier mod's
//    .dds for that id. Without BC support on the GPU the TXTR is drawn.
// Mods are read at startup only: the game caches PAK tables when it boots.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace PortMods {

// --- PAK tables (big-endian, Prime 1 version 3.5) ---------------------------

struct PakResource {
  uint32_t compressed = 0;
  uint32_t type = 0;
  uint32_t id = 0;
  uint32_t size = 0;
  uint32_t offset = 0;
  // Where this entry's 20 bytes sit in the file.
  size_t entryOffset = 0;
};

struct PakTable {
  // End of the resource table: everything before it is header.
  size_t headerEnd = 0;
  std::vector<PakResource> resources;
};

// False when the data is not a Prime 1 PAK, or is cut short before the table's
// end (read more of the file and try again; `needed` says how much at least).
bool ParsePakTable(const uint8_t* data, size_t size, PakTable& table, size_t& needed);

// "1A2B3C4D.TXTR" (any case) -> id 0x1A2B3C4D, type 'TXTR'.
bool ParseLooseName(const std::string& fileName, uint32_t& type, uint32_t& id);
// "1A2B3C4D.dds" (any case) -> id 0x1A2B3C4D: a native texture.
bool ParseNativeTextureName(const std::string& fileName, uint32_t& id);
// "1A2B3C4D.envcube" (any case) -> id 0x1A2B3C4D: a converted material
// reflection cube (written by the Remastered import, see PbrRecord's 'PBR7').
bool ParseMaterialCubeName(const std::string& fileName, uint32_t& id);
std::string FourCCString(uint32_t type);

// --- Virtual files ------------------------------------------------------------

struct Segment {
  enum Kind { kMemory, kSource, kHost };
  Kind kind = kMemory;
  uint64_t start = 0;
  uint64_t length = 0;
  size_t memory = 0;         // kMemory: index into VirtualFile::memory
  uint64_t sourceOffset = 0; // kSource, kHost: where the segment starts in its file
  std::string hostPath{};    // kHost: read from this file, zeros past hostSize
  uint64_t hostSize = 0;
};

struct VirtualFile {
  std::string discPath;
  // The base disc entry kSource segments read.
  int32_t sourceEntry = -1;
  uint64_t size = 0;
  std::vector<std::vector<uint8_t>> memory;
  std::vector<Segment> segments; // contiguous, in order, covering [0, size)
};

struct LooseResource {
  uint32_t type = 0;
  uint32_t id = 0;
  std::string hostPath;
  uint64_t hostSize = 0;
  std::string mod;
};

// A PAK with `loose` swapped in: header is at least table.headerEnd bytes of
// the original, which is originalSize long. Every table entry matching a loose
// resource's type and id points at that resource's appended copy. The original
// data comes from the base disc (kSource), or from sourceHost when that is set
// (a PAK a mod replaced whole). Each `added` resource gets a new table entry;
// the data after the table moves down to make room, 32-byte aligned.
VirtualFile PatchPak(const std::vector<uint8_t>& header, const PakTable& table, uint64_t originalSize,
                     const std::vector<const LooseResource*>& loose, const std::string& sourceHost = {},
                     const std::vector<const LooseResource*>& added = {});
// A PAK holding only `added` (none: an empty but valid PAK).
VirtualFile NewPak(const std::vector<const LooseResource*>& added);

// The largest file the game can read: CDvdFile and the DVD calls hold its
// length and offsets as s32, so a bigger PAK fails its first read at boot.
constexpr uint64_t kMaxFileSize = 0x80000000u - 32;
// Splits the resources nothing holds into PAK-sized groups: the first fills
// what `homeSize` (NoARAM.pak with its own resources swapped in) leaves under
// kMaxFileSize and may be empty, each later one is a new PAK. A resource too
// big for any PAK is left out.
std::vector<std::vector<const LooseResource*>> SplitAdded(const std::vector<const LooseResource*>& added,
                                                           uint64_t homeSize);

// How a Reader reaches the file being patched. Tests supply their own.
struct SourceIo {
  void* (*open)(const VirtualFile& file) = nullptr;
  int64_t (*readAt)(void* handle, uint64_t offset, uint8_t* buffer, size_t length) = nullptr;
  void (*close)(void* handle) = nullptr;
};

// One open handle on a virtual file. Not thread safe; each open gets its own.
class Reader {
public:
  Reader(std::shared_ptr<const VirtualFile> file, const SourceIo* io);
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  int64_t Read(uint8_t* buffer, size_t length);
  int64_t Seek(int64_t offset, int32_t whence);

private:
  int64_t ReadSegment(const Segment& segment, uint64_t at, uint8_t* buffer, size_t length);

  std::shared_ptr<const VirtualFile> mFile;
  const SourceIo* mIo;
  uint64_t mPos = 0;
  void* mSource = nullptr;
  bool mSourceTried = false;
  struct Host;
  std::unique_ptr<Host> mHost;
};

// --- Startup --------------------------------------------------------------------

struct ModInfo {
  std::string name;
  bool enabled = true;
  int files = 0;     // disc files replaced
  int resources = 0; // loose resources used
  int textures = 0;  // <id>.dds images
  bool import = false;       // made by the Remastered import (stamp present, or its folder name)
  bool importStale = false;  // ... and by an older importer than this build's
};

struct Status {
  std::string folder;
  bool active = false; // mods setting on
  std::vector<ModInfo> mods;
  int overlays = 0; // disc files served from mods
  std::vector<std::string> messages;
  std::string staleImport; // name of the Remastered import that needs redoing; empty if none
};

// Whether a Remastered import's stamp file (its whole text; empty when the file is missing) is older
// than `current`: missing, unreadable and lower numbers are stale, equal and higher are not. The
// stamp's first line is the number, and anything after it (the build's commit) is ignored.
bool ImportStampStale(const std::string& stampText, int current);
// The name of the mod that is a stale Remastered import, or null. Set at each scan.
const char* StaleRemasteredImport();

// Scans the mods folder and registers the overlays. Call once, after the disc
// is open and before the game starts.
void Initialize();
// Reloading while the game runs (PortSaveState::RequestModReload drives it, at
// a world reload with every PAK closed). BeginReload lets go of the mods'
// files, so the folder can change; FinishReload scans again, replaces the
// overlays and binds the live textures to the new images.
void BeginReload();
void FinishReload();
const Status& CurrentStatus();
// While suspended no mod loads, whatever the settings say: a Remastered import
// unloads them so their memory is free while it runs. Takes effect at the next
// reload or start.
void SetSuspended(bool suspended);
bool Suspended();
// The mods folder, created if missing. Empty if there is no pref folder.
std::string Folder();
// Every .pak on the disc (with mods applied), as (entry number, path).
std::vector<std::pair<int32_t, std::string>> DiscPaks();
// The PAKs holding the added resources NoARAM.pak has no room for, for
// AddPakFileAsync (no ".pak"): "PortMods1", "PortMods2", ... A reload keeps the
// count from going down (the dropped ones stay as empty PAKs), since the
// game reopens every PAK it had.
int ExtraPakCount();
std::string ExtraPakName(int index);

// Native textures, for CTexture. HasNativeTexture: a mod has <id>.dds.
// BindTexture: registers that .dds under `owner`, which the texture then passes
// to GXInitTexObjUserData so its draws show the image; false when there is
// none. UnbindTexture: `owner` is being destroyed. Main thread only.
bool HasNativeTexture(uint32_t id);
bool BindTexture(const void* owner, uint32_t id);
void UnbindTexture(const void* owner);
// How many .dds files the mods supply, and how many are bound now.
size_t NativeTextureCount();
size_t NativeTexturesBound();
// The .sdfont a mod supplies (port_hd_font.h), the last mod's when several do; empty when none.
std::string FontPath();
// The <MREA id>.roomenv a mod supplies for an area (port_room_env.h); empty when none.
std::string RoomEnvPath(uint32_t mrea);
// The <id>.envcube a mod supplies for a material's own reflection cube
// (PortRoomEnv::MaterialCube); empty when none.
std::string MaterialCubePath(uint32_t id);
// The roomenv/brdf.lut a mod supplies (port_room_env.h), the first mod's when several do; empty when none.
std::string BrdfLutPath();
// The gallery/NNN.jpg pictures the mods supply (the Extras gallery), in file name order; a later mod's file
// replaces an earlier one's of the same name.
std::vector<std::string> GalleryPaths();
// The <FRME id>.hudbars a mod supplies for a HUD frame (port_hud_bars.h); empty when none.
std::string HudBarsPath(uint32_t frame);
// The thermal.lut a mod supplies (the thermal visor's heat gradient, 256x4 RGBA8); empty when none.
std::string ThermalLutPath();
// The <MREA id>.roomgeo a mod supplies for an area (port_room_geo.h); empty when none.
std::string RoomGeoPath(uint32_t mrea);
// The same for its liquid surfaces (port_room_liquid.h).
std::string RoomLiquidPath(uint32_t mrea);
// Every roomgeo/lods.bin the mods supply (port_room_geo.h), in mod order: a later one's
// entry for a model stands in for an earlier one's.
std::vector<std::string> RoomLodPaths();
// Whether any mod folder holds room geometry. Reads the disk, and needs no Initialize:
// the frame buffers are sized from it before there is a renderer.
bool HasRoomGeometry();
// Whether the loaded mods supply room geometry for any area.
bool RoomGeometryLoaded();

// Folder names the settings disable, '/'-separated (no folder name has one).
std::vector<std::string> SplitDisabled(const std::string& list);
std::string JoinDisabled(const std::vector<std::string>& names);

} // namespace PortMods
