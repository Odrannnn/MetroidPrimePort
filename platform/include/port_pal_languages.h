#pragma once

// Languages from a PAL disc for a USA one.
//
// A USA disc has only English text. A player who plays it can still read the
// French, German, Spanish or Italian of their own PAL disc: the import copies
// every STRG off the PAL image into <user>/languages, as <id>.lang, and
// CStringTable adds those tables' languages to the USA disc's
// (FStringTableFactory; the PAL layout is put back in 1.00's order first).
// The F1 Language setting then picks one. The PAL fonts (<id>.font, and the
// <id>.txtr each names) come along, since the USA ones lack accented letters.
// Not a mod, so turning mods off keeps the languages. Nothing else of the PAL
// disc is used, and a PAL disc as the game's disc needs none of this.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace PortPalLanguages {

// On a USA disc, the PAL map-legend icons use different TXTR ids. Rewrite
// those ids only inside complete &image=...; tags (not text or font tags).
bool RemapImportedImageTextureIds(std::wstring& text);

// Reads up to `size` bytes at `offset` of a PAK; returns how many it read.
using ReadAt = std::function<size_t(uint64_t offset, uint8_t* out, size_t size)>;

// A resource's type ('STRG') and id.
using ResourceKey = std::pair<uint32_t, uint32_t>;

// Adds every resource of the PAK `read` reaches that `want` takes to `out`
// (decompressed), unless `out` has it already. False when it isn't a Prime 1
// PAK or a wanted resource can't be read.
bool ReadPakResources(const ReadAt& read, const std::function<bool(uint32_t type, uint32_t id)>& want,
                      std::map<ResourceKey, std::vector<uint8_t>>& out, std::string& error);
// ReadPakResources for the STRGs, by id.
bool ReadPakStrgs(const ReadAt& read, std::map<uint32_t, std::vector<uint8_t>>& out, std::string& error);
// The TXTR id a FONT names; false when `font` isn't one.
bool FontTexture(const std::vector<uint8_t>& font, uint32_t& texture);

// Whether `data` is a version-0 STRG whose sections fit in it.
bool IsStringTable(const std::vector<uint8_t>& data);

struct State {
  bool running = false;
  bool finished = false;  // a run ended; ok and message are its result
  bool ok = false;
  int tables = 0;
  std::string message;
};

// Imports the languages of the PAL image at `path` in the background (the
// path may be an Android content:// address). False (see Status().message)
// when one is already running.
bool Start(const std::string& path);
State Status();
// The number of imported tables (0: none).
int Installed();
// The imported <id>.lang (a PAL STRG, as on the disc); false when there is none.
bool ReadTable(uint32_t id, std::vector<uint8_t>& out);
// The imported PAL FONT `id` and the TXTR it names; false when there is none.
// The USA fonts have no accented letters, so a PAL language draws with these.
bool ReadFont(uint32_t id, std::vector<uint8_t>& font, std::vector<uint8_t>& texture);
// Deletes the imported tables; they stay in tables already loaded.
bool Remove();
// --import-pal-languages <image>: imports in the foreground; the exit code.
int RunFromCommandLine(const std::string& path);

}  // namespace PortPalLanguages
