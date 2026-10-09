#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Read an embedded resource from a mapped DOL section on the already-mounted
// disc. The native executable contains no generated game asset arrays.
// Addresses are GM8E01 revision 0's (config/GM8E01_00/symbols.txt).
std::vector<uint8_t> PortReadDolResource(uint32_t address, uint32_t length);

// The same resource on any supported disc: read at its 1.00 address when it
// starts with signature there, else found by its signature anywhere in the
// DOL (other revisions and regions move the data section).
std::vector<uint8_t> PortFindDolResource(uint32_t address, uint32_t length, const uint8_t* signature,
                                         size_t signatureLength);

// The default font embedded in the DOL, found by the texture it names (its
// bytes differ by region: PAL's is a FONT v4): the zlib stream that inflates
// to that FONT, and the one after it, its TXTR. False if there is none.
bool PortFindDolFont(uint32_t textureId, std::vector<uint8_t>& font, std::vector<uint8_t>& texture);

namespace PortDisc {

// The releases the port knows. The game code is always 1.00's; the other
// discs only supply their data.
enum class Version { Unknown, Usa100, Usa101, Usa102, Pal };

// Game id (six characters, not terminated), disc number and revision byte.
Version Identify(const char* id6, unsigned diskNumber, unsigned revision);
// The mounted disc's, or Unknown with none mounted.
Version Current();
// "USA 1.00", "PAL", ...
const char* Name(Version version);
// Whether the port accepts this disc. Only 1.00 for now: the others are
// accepted with MP_DISC_ANY_VERSION=1 until their data is known to load.
bool IsAccepted(Version version);

} // namespace PortDisc
