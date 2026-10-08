#include "port_disc.h"
#include "port_env.h"

#include <dolphin/dvd.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
uint32_t ReadBig32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

}

std::vector<uint8_t> PortReadDolResource(uint32_t address, uint32_t length) {
  s32 dolSize = 0;
  const uint8_t* dol = DVDGetDOLLocation(&dolSize);
  if (dol == nullptr || dolSize < 0x100) {
    throw std::runtime_error("Could not read the mounted disc's DOL");
  }
  for (uint32_t i = 0; i < 18; ++i) {
    const uint32_t sectionOffset = ReadBig32(dol + i * 4);
    const uint32_t sectionAddress = ReadBig32(dol + 0x48 + i * 4);
    const uint32_t sectionSize = ReadBig32(dol + 0x90 + i * 4);
    if (address < sectionAddress || address - sectionAddress > sectionSize ||
        length > sectionSize - (address - sectionAddress))
      continue;
    const uint64_t start = uint64_t(sectionOffset) + address - sectionAddress;
    if (start > static_cast<uint32_t>(dolSize) || length > dolSize - start)
      break;
    return {dol + start, dol + start + length};
  }
  throw std::runtime_error("Expected embedded resource is absent from the disc's DOL");
}

std::vector<uint8_t> PortFindDolResource(uint32_t address, uint32_t length, const uint8_t* signature,
                                         size_t signatureLength) {
  try {
    auto data = PortReadDolResource(address, length);
    if (signatureLength <= data.size() && std::memcmp(data.data(), signature, signatureLength) == 0)
      return data;
  } catch (const std::runtime_error&) {
  }
  s32 dolSize = 0;
  const uint8_t* dol = DVDGetDOLLocation(&dolSize);
  if (dol == nullptr || dolSize <= 0 || signatureLength == 0) {
    throw std::runtime_error("Could not read the mounted disc's DOL");
  }
  const uint8_t* end = dol + dolSize;
  const uint8_t* found = std::search(dol, end, signature, signature + signatureLength);
  if (found == end || length > static_cast<size_t>(end - found)) {
    throw std::runtime_error("Expected embedded resource is absent from the disc's DOL");
  }
  return {found, found + length};
}

namespace PortDisc {

Version Identify(const char* id6, unsigned diskNumber, unsigned revision) {
  if (id6 == nullptr || diskNumber != 0)
    return Version::Unknown;
  if (std::memcmp(id6, "GM8E01", 6) == 0) {
    switch (revision) {
    case 0: return Version::Usa100;
    case 1: return Version::Usa101;
    case 2: return Version::Usa102;
    default: return Version::Unknown;
    }
  }
  if (std::memcmp(id6, "GM8P01", 6) == 0 && revision == 0)
    return Version::Pal;
  return Version::Unknown;
}

Version Current() {
  const DVDDiskID* id = DVDGetCurrentDiskID();
  if (id == nullptr)
    return Version::Unknown;
  char id6[6];
  std::memcpy(id6, id->gameName, 4);
  std::memcpy(id6 + 4, id->company, 2);
  return Identify(id6, id->diskNumber, id->gameVersion);
}

const char* Name(Version version) {
  switch (version) {
  case Version::Usa100: return "USA 1.00";
  case Version::Usa101: return "USA 1.01";
  case Version::Usa102: return "USA 1.02";
  case Version::Pal: return "PAL";
  case Version::Unknown: break;
  }
  return "unknown";
}

bool IsAccepted(Version version) {
  if (version == Version::Usa100)
    return true;
  return version != Version::Unknown && port::EnvFlag("MP_DISC_ANY_VERSION");
}

} // namespace PortDisc
