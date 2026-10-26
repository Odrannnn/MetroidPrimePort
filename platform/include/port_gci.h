#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "port_save_convert.h"

// Moving saves between the port's memory card and Dolphin's.
//
// The port's card is a GCI folder: one file per CARD file, a 64-byte
// big-endian directory entry followed by the file's 8 KiB blocks (Dolphin's
// .gci format and its GCI-folder naming). Dolphin keeps its cards either the
// same way or as a raw image (MemoryCardA.USA.raw); both can be read, and a
// .gci can be written into either.
//
// Only Metroid Prime's own files (game GM8E or GM8P, maker 01) are copied. The game
// alternates its save between "MetroidPrime A" and "MetroidPrime B" and loads
// the newer, so an import replaces the whole set: every game file already in
// the folder is moved into a "_replaced" folder next to it first, never deleted.
namespace PortGci {

constexpr size_t kHeaderSize = 64;
constexpr size_t kBlockSize = 8192;

// UTF-8 text to and from paths (std::filesystem's u8 forms are char8_t).
std::string PathString(const std::filesystem::path& path);
std::filesystem::path PathFromString(const std::string& text);

struct Header {
  std::string game;     // four characters, GM8E for this game
  std::string maker;    // two characters, 01
  std::string fileName; // the CARD file name, e.g. "MetroidPrime A"
  uint16_t blockCount = 0;
};

// Checks a .gci image: a whole header, whole blocks, and sizes that agree.
bool ParseHeader(const uint8_t* data, size_t size, Header& out, std::string& error);
// The mounted disc's game code: GM8E (the default), or GM8P on a PAL disc.
// Only its files are copied, and Dolphin's card for its region (USA, EUR) is
// the one looked for: a PAL save's worlds are laid out differently.
void SetGameCode(const char* code4);
const char* GameCode();
// "USA" or "EUR", as Dolphin names its cards.
const char* CardRegion();
// Whether this is one of Metroid Prime's files.
bool IsGameFile(const Header& header);
// The name Dolphin and Aurora give it in a GCI folder: 01-GM8E-<name>.gci.
std::string DiskName(const Header& header);

struct Report {
  int copied = 0;
  int replaced = 0; // earlier files moved aside to _replaced
  int skipped = 0;  // other games' files, or ones that failed; see errors
  std::vector<std::string> errors;
  std::string note;
  std::string Summary(const char* verb) const;
};

// Writes one .gci image into a GCI folder, moving aside any file there with
// the same identity. The folder is created if missing.
bool InstallGci(const std::filesystem::path& folder, const std::vector<uint8_t>& gci,
                std::string& error, bool* replacedOut = nullptr);

// Imports a .gci image, or a raw card image (every Metroid Prime file in it).
// A save of the other release (USA or PAL GameCube, or a Wii Trilogy
// save.bin/data.bin) is converted to the mounted disc's format; the report's
// note says so. See port_save_convert.h.
// `name` is only used in messages. `scratch` is a writable directory for the
// raw card, which Aurora reads from a path.
Report ImportBytes(const std::vector<uint8_t>& bytes, const std::string& name,
                   const std::filesystem::path& folder, const std::filesystem::path& scratch);
// Moves the port's earlier PAL saves ("MetroidPrime A"/"B": the USA container
// holding PAL worlds) into one real PAL "MetroidPrime" file, once, on a PAL
// disc. The old files go to _replaced. An empty report means nothing to do.
Report MigrateLegacyPal(const std::filesystem::path& folder);
// The same, reading the file first.
Report ImportFile(const std::filesystem::path& source, const std::filesystem::path& folder,
                  const std::filesystem::path& scratch);
// Imports the Metroid Prime save from another GCI folder (a Dolphin card),
// converted to the disc's release if it is another one's.
Report ImportFolder(const std::filesystem::path& source, const std::filesystem::path& folder);

// The disc's .gci files in a GCI folder, sorted by disk name.
std::vector<std::filesystem::path> GameFiles(const std::filesystem::path& folder);
// What ImportFolder takes from a folder: the disc's own files if there are
// any, else one other release's (a Trilogy disc imports GameCube saves).
std::vector<std::filesystem::path> ImportableFiles(const std::filesystem::path& folder);

// What an export writes: the card's own files as they are (the disc's native
// format), or the newest save converted to the other GameCube release.
using ExportFormat = PortSaveConvert::Format;
ExportFormat NativeExport();
// The game code and Dolphin card region a format's files belong to.
const char* GameCodeOf(ExportFormat format);
const char* CardRegionOf(ExportFormat format);
// The newest save on the card in `format`, with the note for the report.
// WiiSave writes it into a copy of the Trilogy save.bin `trilogyBase` (the
// console signs data.bin, so only save.bin, as Dolphin keeps it, can be written).
bool ExportBytes(const std::filesystem::path& folder, ExportFormat format, std::vector<uint8_t>& gci,
                 std::string& note, std::string& error, const std::vector<uint8_t>& trilogyBase = {});
// Writes the newest save into the Trilogy save.bin at `saveBin` as its Metroid
// Prime files, keeping its other files and the other games; the old file is
// first copied to <saveBin>.bak.
Report ExportTrilogy(const std::filesystem::path& folder, const std::filesystem::path& saveBin);
// Copies them into another GCI folder (Dolphin's, or any directory). The
// format's files already there move to _replaced.
Report ExportFolder(const std::filesystem::path& folder, const std::filesystem::path& dest,
                    ExportFormat format = NativeExport());
// Inserts them into a raw card image, replacing the format's files there (ones
// not exported are deleted); the image is first copied to <image>.bak. A
// missing image is created.
Report ExportRaw(const std::filesystem::path& folder, const std::filesystem::path& image,
                 ExportFormat format = NativeExport());

// Dolphin's card A for a region ("USA", "EUR"; default the disc's) as this
// machine has it: its GCI folder, its raw image, or both (whichever exist).
// Empty on Android, which has no Dolphin to find.
struct DolphinCard {
  std::filesystem::path gciFolder;
  std::filesystem::path rawImage;
  bool Found() const { return !gciFolder.empty() || !rawImage.empty(); }
};
DolphinCard FindDolphinCard(const char* region = nullptr);
// The emulator's user folder the player chose (Dolphin or PrimeHack, the one
// with GC/ in it; needed for portable installs), or empty for autodetection. It
// is normalised: a GC folder, a GC/<region> folder or a GC/<region>/Card A
// folder resolve to the user folder above them. When set it is the only
// folder looked at.
std::filesystem::path NormalizeEmulatorFolder(const std::filesystem::path& picked);
void SetEmulatorFolder(const std::string& path);
std::string EmulatorFolder();
// The emulator user folders autodetection knows about that exist on this
// machine (they contain GC/), in preference order, ignoring the chosen folder.
// Lets the player pick PrimeHack when Dolphin is installed too (issue #115).
std::vector<std::filesystem::path> DetectedEmulatorFolders();
// Share the emulator's card A with the port (desktop only): the port mounts
// <emulator folder>/GC as its card base, so both read and write the same files.
void SetShareEmulatorCard(bool share);
bool ShareEmulatorCard();
// The base to mount for sharing, <root>/GC of the first emulator folder whose
// GC/<region>/Card A is a directory; empty when sharing is off or there is none,
// with `why` (if given) saying why for the player.
std::filesystem::path SharedCardBase(std::string* why = nullptr);
// The card base the game should mount: the Archipelago/randomizer card when it
// has one, else the shared emulator card, else "" (the port's own card).
std::string CardTargetDirectory(const std::string& apDirectory);
// Dolphin's Trilogy save.bin (USA, else PAL), or empty when it has none.
std::filesystem::path FindDolphinTrilogy();
// Sharing on a Trilogy disc: the emulator keeps Trilogy's saves in a Wii
// save.bin, not a GCI folder, so nothing is mounted (SharedCardBase is empty);
// the port keeps its own card and mirrors it with that save.bin instead. The
// emulator's save.bin for the running disc's region, or empty when not sharing
// on a Trilogy disc or the emulator has none.
std::filesystem::path TrilogyShareBin();
// Mirrors card folder and save.bin: a newer card goes out (ExportTrilogy), and
// with `pull` a newer save.bin comes in (marked for remount). The replaced side
// is kept (save.bin.bak, _replaced); afterwards both have the same time, which
// means in step. True when the card changed.
bool SyncTrilogy(const std::filesystem::path& folder, const std::filesystem::path& saveBin, bool pull);
// SyncTrilogy on the mounted card and TrilogyShareBin, at most twice a second;
// nothing when that is empty. Pull only in the front end: in game the card
// never changes.
bool SyncTrilogyShare(bool pull);

// The mounted card's GCI folder, or empty when the card is a raw image or
// there is none.
std::filesystem::path MountedCardFolder();
// MigrateLegacyPal on the card folder in use (nothing off the PAL disc). Run once
// the card API is up, and again when another card folder is switched in.
Report MigrateMountedLegacyPal();

// Saves another release left on the port's card: each disc keeps its own files
// (GM8E, GM8P, R3ME, R3MP; PAL ones under EUR), so after switching discs the
// old saves are still there but the game does not see them.
struct OtherRelease {
  std::filesystem::path folder;
  std::string code;  // game code of its files
  std::string label; // "Metroid Prime (USA GameCube)"
  std::filesystem::file_time_type written{};
};
// Those beside the mounted card, newest first; empty when the card is not a
// GCI folder.
std::vector<OtherRelease> OtherReleaseSaves();
// Copies one into the mounted card, converted to this disc's release; the
// card's own saves move to _replaced, as an import does.
Report RestoreFrom(const OtherRelease& source);
// What the startup offer asks (CMemoryCardSys): the mounted card has no save
// of this release, others are there, and it was not asked on this card for
// this release before (a marker file in the card folder remembers).
std::vector<OtherRelease> RestoreToOffer();
void MarkRestoreOffered();
// After an import: the front end's save screen remounts the card and probes it
// again once the card is idle. Returns true (once) when it did the remount.
void MarkCardChanged();
bool RemountIfChanged();

} // namespace PortGci
