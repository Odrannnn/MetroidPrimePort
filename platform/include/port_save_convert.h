#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Converts Metroid Prime saves between the formats of its releases.
//
// Every reader parses one format into the same ID-keyed intermediate (Save);
// Retarget() maps the ids onto the other release's tables; every writer emits
// one GameCube format from it. Same-release conversion is lossless. The
// per-world tables (relays, doors, cinematics, scans, area and layer names)
// are generated into platform/save_layouts by tools/gen_save_layouts.py.
//
// Formats read: USA GameCube (GM8E01, 1.00 to 1.02 share one layout), real PAL
// GameCube (GM8P01, one 3-block "MetroidPrime" with two copies), the port's own
// earlier PAL saves (GM8P01, one block, "MetroidPrime A"/"B": the USA container
// holding PAL worlds), the port's card files for a Trilogy disc (R3ME/R3MP:
// PAL's container holding the Trilogy's own worlds, as that disc's game orders
// them), and the Wii Trilogy's save.bin or data.bin.
// Formats written: USA, real PAL, the Trilogy disc's card files, and the
// Trilogy's save.bin (into an
// existing one: the console signs data.bin, which only the Wii can do).
namespace PortSaveConvert {

// TriGc: the Trilogy's worlds (the Wii tables) in a GameCube stream, its scans
// keyed by id and category as on GameCube.
enum class Layout { Usa, Pal, Wii, TriGc };
enum class Format { Unknown, UsaGci, PalGci, PalLegacyGci, WiiSave, WiiDataBin, TriGci };

// "USA", "PAL", "PAL (earlier port save)", "Wii", "Trilogy (GameCube card)".
const char* FormatName(Format format);

// One scan: its id and the category the game files it under. The Trilogy keys
// scans by id alone and uses kAnyCategory.
constexpr uint32_t kAnyCategory = 0xFFFFFFFFu;
constexpr size_t kApWords = 15;

using LayerKey = std::tuple<uint32_t, std::string, int>; // area MREA, layer name, nth of that name

struct World {
  bool present = false; // false: the stream ended before this world
  bool areaKnown = false;
  uint32_t areaRaw = 0;  // the stored area index
  uint32_t areaMrea = 0; // that area's MREA, when the index is in range
  uint32_t desired = 0;
  std::set<uint32_t> relays, doors;
  std::set<uint32_t> visited, mapped; // MREA ids
  bool mapStation = false;
  // Only layers the stream held (layer 0 is never stored). A layer missing
  // here takes the target's default state.
  std::map<LayerKey, bool> layers;
  bool operator==(const World&) const = default;
};

struct Slot {
  bool present = false;
  uint32_t timestamp = 0;
  bool hard = false, init = false;
  uint32_t mlvl = 0, playHi = 0, playLo = 0;
  uint32_t enabled = 0, health = 0, beam = 0, suit = 0;
  std::array<uint32_t, 41> amount{}, capacity{};
  std::set<std::pair<uint32_t, uint32_t>> scans;
  std::array<uint32_t, 2> rates{};
  std::array<uint8_t, 64> opts{};
  // Raw stored values (screen offsets and stretch still carry their bias).
  uint32_t sound = 1, bright = 4, xoff = 0x1e, yoff = 0x1e, stretch = 10, sfx = 0x7f, music = 0x7f,
           hud = 0xff, helmet = 0xff;
  // hudlag, hint, invertY, rumble, swap, then PAL's sixth.
  std::array<bool, 6> flags{true, true, false, true, false, false};
  std::array<uint32_t, 32> hintState{}, hintTime{};
  std::map<uint32_t, World> worlds; // by MLVL
  // The port's Archipelago trailer ("APX4"), when the stream carried one.
  bool hasAp = false;
  std::array<uint32_t, kApWords> ap{};
  // The port's seen-pickups trailer ("PKSN" plus four words), after the AP one.
  bool hasSeen = false;
  std::array<uint32_t, 4> seen{};
  // The port's room-marks trailer ("RMMK", port_map_marks.h), after the seen one.
  std::vector<uint16_t> marks;
  // A Trilogy slot's own cinematics (System::cine has every slot's); dropped
  // by a retarget, after which the slot takes System::cine.
  std::optional<std::set<std::pair<uint32_t, uint32_t>>> cine;
  bool operator==(const Slot&) const = default;
};

// Which shape the opaque leading bytes of the system block came from.
enum class Lead { None, Usa, Pal, Wii };

struct System {
  bool present = false;
  Lead lead = Lead::None;
  std::vector<uint8_t> leadBytes;
  uint32_t fps = 0, ball = 0, powerBombs = 0, logScan = 0;
  std::array<bool, 5> flags{};
  uint32_t autoMapper = 0;
  // PAL only: the selected language (0 English .. 4 Italian, 6 Japanese).
  uint32_t palField = 0;
  std::set<std::pair<uint32_t, uint32_t>> cine; // (MLVL, cinematic id)
  bool operator==(const System&) const = default;
};

struct Save {
  Layout layout = Layout::Usa;
  std::array<Slot, 3> slots;
  System system;
  bool operator==(const Save&) const = default;
};

// What a conversion could not carry over.
struct Stats {
  int relays = 0, doors = 0, cinematics = 0, scans = 0, worlds = 0, areas = 0;
  int truncatedSlots = 0; // a stream longer than the target's slot
  std::vector<std::string> ids; // what was dropped, "relay 0x...", capped
  bool Any() const { return relays + doors + cinematics + scans + worlds + areas + truncatedSlots > 0; }
  std::string Summary() const;
};

// Decrypts a Wii SD export (data.bin) and returns its save.bin.
bool DecryptDataBin(const uint8_t* data, size_t size, std::vector<uint8_t>& saveBin, std::string& error);
// CRC32 (zlib's) and one AES-128 block decrypt, exposed for the tests.
uint32_t Crc32(const uint8_t* data, size_t size);
void AesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size, uint8_t* out);

Format Detect(const uint8_t* data, size_t size);
// The GCI format the mounted disc's game reads natively (TriGci for a Trilogy disc).
Format NativeFormat(bool pal, bool trilogy = false);

// Parses any readable format. `art` receives the source's 5120 banner/icon
// bytes when it is a GCI.
bool Read(const uint8_t* data, size_t size, Format& format, Save& out, std::vector<uint8_t>* art,
          std::string& error);
// Maps the ids onto `to`'s tables (no-op when already there).
void Retarget(Save& save, Layout to, Stats& stats);
// `art`: banner, icon and palette bytes (5120) or empty for a blank banner.
// `now`: the stamp for the file; 0 means the current time. `pal`: PAL's
// container, for a Layout::Pal or Layout::TriGc save.
bool WriteGci(const Save& save, bool pal, const std::vector<uint8_t>& art, int64_t now,
              std::vector<uint8_t>& gci, Stats& stats, std::string& error);

struct Conversion {
  Format from = Format::Unknown;
  Stats stats;
  std::vector<uint8_t> gci;
  uint32_t newest = 0; // the newest slot's timestamp, to choose between several files
};
// Reads `data` in any format and writes it as `target` (UsaGci, PalGci or TriGci).
bool ConvertTo(const uint8_t* data, size_t size, Format target, const std::vector<uint8_t>& art,
               Conversion& out, std::string& error);
// The same, to the mounted disc's GCI.
bool ConvertToDisc(const uint8_t* data, size_t size, bool pal, const std::vector<uint8_t>& art,
                   Conversion& out, std::string& error);

// Writes a Layout::Wii save into a copy of the Trilogy save.bin `base`: each
// present slot replaces that MP1 file (its file-select summary too); the other
// files, MP2 and MP3 are kept.
bool WriteWii(const Save& save, const uint8_t* base, size_t baseSize, std::vector<uint8_t>& bin, Stats& stats,
              std::string& error);
// Reads `data` in any format and writes its newest slot into the save.bin
// `base`, over the MP1 file the Trilogy last used (it shows one per Mii, and a
// file it never used has no Mii); fails when `base` has none. `out.gci`
// receives the new save.bin.
bool ConvertToTrilogy(const uint8_t* data, size_t size, const uint8_t* base, size_t baseSize, Conversion& out,
                      std::string& error);

// The 5120 banner/icon bytes of a GCI image, or empty.
std::vector<uint8_t> ArtOf(const uint8_t* gci, size_t size);

} // namespace PortSaveConvert
