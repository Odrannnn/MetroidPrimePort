#pragma once

// Builds the "remastered-models" mod inside the port, from the user's own
// copy of Metroid Prime Remastered (.nsp) and their own Switch key file.
//
// Nothing is extracted to disk on the way: the paks are read straight out of
// the encrypted image (port_remastered_nsp.h), the retail models they replace
// come off the unmodded disc that is already open, and each model in
// port_remastered_table.h goes through port_remastered_convert.h. The output
// is the user's, for their use only; the port ships none of it.
//
// The mod is written to a hidden staging folder in the mods folder and only
// takes the place of <mods>/remastered-models at the next start, before mods
// are scanned: the running game may be reading the files of the one it loaded.

#include <string>
#include <vector>

namespace PortRemastered {

struct ImportState {
  bool running = false;
  bool finished = false;   // a run ended; ok, message and lines are its result
  bool ok = false;         // at least one model converted and the mod is staged
  bool cancelled = false;
  int done = 0;            // models tried so far
  int total = 0;
  int failed = 0;
  std::string message;     // what it is doing, or how it ended
  std::vector<std::string> lines;  // the newest kImportMaxLines of its log
};

inline constexpr size_t kImportMaxLines = 200;

// The mod folder an import ends up as.
inline constexpr const char* kImportModName = "remastered-models";

// An import is made in stages, and a re-import links in the previous import's output of every
// stage whose number, options and inputs are unchanged instead of making it again. Bump the
// number of the stage a change needs re-imported (all of them that it touches):
namespace ImportStage {
// port_remastered_convert/cmdl: how models and materials are written (models, effects, rooms,
// room models, HUD).
inline constexpr int kConverter = 49;
// What a converted texture holds: port_remastered_image/txtr/dds/astc, and Converter's Get, Cube
// and Baked. Each converted texture is kept across imports under its tag and this number, so a
// kConverter bump re-imports the models without converting their textures again.
inline constexpr int kTextures = 3;
inline constexpr int kModels = 5;      // the table's models, their looks and the ANCS copies
inline constexpr int kEffects = 7;     // port_remastered_effect_import and the particle converters
inline constexpr int kRooms = 11;      // port_remastered_room: roomenv/, .roomgeo, .roomliquid, water maps
inline constexpr int kRoomModels = 0;  // the rooms' own models and their levels of detail
inline constexpr int kText = 0;        // port_remastered_text, and the font (port_remastered_font)
inline constexpr int kHud = 6;         // port_remastered_hud, port_remastered_map
inline constexpr int kMovies = 0;      // port_remastered_movie
inline constexpr int kGallery = 0;     // the gallery pictures (port_gallery)
}  // namespace ImportStage
// Any stage's bump raises it. A full import writes it to kImportStampName in the mod; the mod scan
// (port_mods.h) tells the player when an import carries a lower number, or none.
inline constexpr int kImportVersion = 38 + ImportStage::kConverter + ImportStage::kTextures + ImportStage::kModels +
                                      ImportStage::kEffects + ImportStage::kRooms + ImportStage::kRoomModels +
                                      ImportStage::kText + ImportStage::kHud + ImportStage::kMovies +
                                      ImportStage::kGallery;
inline constexpr const char* kImportStampName = ".import-version";

// ~/.switch/prod.keys if it is there, else empty.
std::string DefaultKeysPath();

// Starts an import on worker threads; `threads` 0 leaves a couple of cores to
// the game. The disc must be open. False (with the reason in the state's
// message) when one is already running or there is no mods folder.
bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads = 0);
// Only the menu movies (port_remastered_movie.h) and the Extras gallery, into the mod an earlier
// import made: for a player who had no ffmpeg then, or imported before the gallery. Same state and cancelling;
// false when one is running or there is no such mod.
bool StartMovieImport(const std::string& nspPath, const std::string& keysPath);
// Whether the next import also converts the rooms themselves (five times the
// size and twice the time). MP_REMASTERED_GEOMETRY, when set, decides instead.
void SetImportGeometry(bool on);
// Whether the next import links in the unchanged stages of the previous one (the default) or
// makes everything again. MP_REMASTERED_REUSE=0 turns it off whatever this says.
void SetImportReuse(bool on);
ImportState ImportStatus();
// Asks the running import to stop; it ends at the next model.
void CancelImport();
// Cancels and waits. Call before the disc is closed.
void StopImport();

// Moves a finished import into place. Call before PortMods::Initialize().
// True if a mod was installed.
bool ApplyPendingImport();

// `--import-remastered <nsp> [keys]`: imports with every core, progress on
// stdout, and installs the mod. The disc must be open. Returns the exit code.
// `--import-remastered-movies` is StartMovieImport() the same way.
int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly = false);

}  // namespace PortRemastered
