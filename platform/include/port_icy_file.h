#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port_icy_assets.h"

// icy.bin: Remastered's frozen-enemy material (port_icy_assets.h), decoded by the import.
namespace PortIcy {
std::vector<uint8_t> Serialize(const Assets& assets);
// False, with why in `error`, for anything but a whole icy.bin of this version.
bool Parse(const uint8_t* data, size_t size, Assets& out, std::string& error);
// The MATI's parameter tags of each image and constant, in the file's order.
inline constexpr const char* kImageTag[kCount] = {"NMAP", "METL", "TCH0"};
inline constexpr const char* kVectorTag[kVectors] = {"CCH0", "CCH1", "BKLT", "BKLA"};
} // namespace PortIcy
