#include "port_disc.h"
#include "port_env.h"

#include <dolphin/dvd.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <zlib.h>

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

namespace {
// The compressed length of the zlib stream at `data` and its first bytes
// inflated into `head` (all of them if the stream is shorter); 0 if it isn't
// a whole stream.
size_t InflatedStream(const uint8_t* data, size_t size, uint8_t* head, size_t headSize) {
  z_stream stream{};
  if (inflateInit(&stream) != Z_OK)
    return 0;
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(std::min<size_t>(size, 1u << 20));
  uint8_t scratch[4096];
  size_t produced = 0;
  int result = Z_OK;
  while (result == Z_OK) {
    stream.next_out = scratch;
    stream.avail_out = sizeof(scratch);
    result = inflate(&stream, Z_NO_FLUSH);
    const size_t got = sizeof(scratch) - stream.avail_out;
    if (produced < headSize)
      std::memcpy(head + produced, scratch, std::min(got, headSize - produced));
    produced += got;
  }
  const size_t used = stream.total_in;
  inflateEnd(&stream);
  return result == Z_STREAM_END ? used : 0;
}
} // namespace

bool PortFindDolFont(uint32_t textureId, std::vector<uint8_t>& font, std::vector<uint8_t>& texture) {
  s32 dolSize = 0;
  const uint8_t* dol = DVDGetDOLLocation(&dolSize);
  if (dol == nullptr || dolSize <= 0)
    return false;
  const uint8_t* const end = dol + dolSize;
  static const uint8_t kZlib[] = {0x78, 0xda};
  for (const uint8_t* at = dol; (at = std::search(at, end, kZlib, kZlib + 2)) != end; ++at) {
    uint8_t head[96] = {};
    const size_t used = InflatedStream(at, size_t(end - at), head, sizeof(head));
    if (used == 0 || std::memcmp(head, "FONT", 4) != 0)
      continue;
    // FONT header: version, 4 ints (v2+), 2 bools, 2 ints, name, texture id.
    const uint32_t version = ReadBig32(head + 4);
    size_t pos = 8 + (version >= 2 ? 16 : version >= 1 ? 12 : 8) + 10;
    const uint8_t* name = static_cast<const uint8_t*>(std::memchr(head + pos, 0, sizeof(head) - pos));
    if (name == nullptr || size_t(name - head) + 5 > sizeof(head) || ReadBig32(name + 1) != textureId)
      continue;
    // Its texture is the next stream (word aligned).
    const uint8_t* const next = std::search(at + used, std::min(end, at + used + 32), kZlib, kZlib + 2);
    uint8_t txtrHead[4];
    const size_t txtrUsed = next < end ? InflatedStream(next, size_t(end - next), txtrHead, 4) : 0;
    if (txtrUsed == 0)
      continue;
    font.assign(at, at + used);
    texture.assign(next, next + txtrUsed);
    return true;
  }
  return false;
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
  if (version == Version::Usa100 || version == Version::Pal)
    return true;
  return version != Version::Unknown && port::EnvFlag("MP_DISC_ANY_VERSION");
}

} // namespace PortDisc
