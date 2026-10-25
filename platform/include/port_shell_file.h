#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port_shell_assets.h"

// The shell.bin file (format in port_shell_file.cpp): written by the Remastered import,
// parsed by PortShell::Get(). Split from port_shell_assets.h so the import and the tests
// need not link the mod loader.
namespace PortShell {
// The file for `assets`: the three images, in the order strength, body, doll.
std::vector<uint8_t> Serialize(const Assets& assets);
// Parses and validates a file: magic, version, every image's size and mip count, and that
// nothing is truncated or trails. False with `error` set; `out` is then unspecified.
bool Parse(const uint8_t* data, size_t size, Assets& out, std::string& error);
// The images' GUIDs as the Remastered pak lists them, for the importer. The gun's own shell
// (silhouette type 0) samples a strength gradient of its own, 8ad9bbf0-d388-40fd-93c6-6ff3c319bddf,
// which this file leaves out.
inline constexpr const char* kGuid[kCount] = {"f847b393-cf25-42fe-a46c-a72e835e6b53",
                                              "fc152c4e-483b-4a94-8b18-15fc55101b6f",
                                              "7667ddd9-93a2-4700-b6d5-664ddfad3a69"};
} // namespace PortShell
