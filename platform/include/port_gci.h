#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
// `name` is only used in messages. `scratch` is a writable directory for the
// raw card, which Aurora reads from a path.
Report ImportBytes(const std::vector<uint8_t>& bytes, const std::string& name,
                   const std::filesystem::path& folder, const std::filesystem::path& scratch);
// The same, reading the file first.
Report ImportFile(const std::filesystem::path& source, const std::filesystem::path& folder,
                  const std::filesystem::path& scratch);
// Imports every Metroid Prime file from another GCI folder (a Dolphin card).
Report ImportFolder(const std::filesystem::path& source, const std::filesystem::path& folder);

// Metroid Prime's .gci files in a GCI folder, sorted by disk name.
std::vector<std::filesystem::path> GameFiles(const std::filesystem::path& folder);
// Copies them into another GCI folder (Dolphin's, or any directory).
Report ExportFolder(const std::filesystem::path& folder, const std::filesystem::path& dest);
// Inserts them into a raw card image, replacing the game's files there (ones
// not in the folder are deleted); the image is first copied to <image>.bak. A
// missing image is created.
Report ExportRaw(const std::filesystem::path& folder, const std::filesystem::path& image);

// Dolphin's USA card A as this machine has it: its GCI folder, its raw image,
// or both (whichever exist). Empty on Android, which has no Dolphin to find.
struct DolphinCard {
  std::filesystem::path gciFolder;
  std::filesystem::path rawImage;
  bool Found() const { return !gciFolder.empty() || !rawImage.empty(); }
};
DolphinCard FindDolphinCard();

// The mounted card's GCI folder, or empty when the card is a raw image or
// there is none.
std::filesystem::path MountedCardFolder();
// After an import: the front end's save screen remounts the card and probes it
// again once the card is idle. Returns true (once) when it did the remount.
void MarkCardChanged();
bool RemountIfChanged();

} // namespace PortGci
