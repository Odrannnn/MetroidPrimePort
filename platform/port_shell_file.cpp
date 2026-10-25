// shell.bin: Remastered's silhouette-shell textures (the Phazon Suit's aura), decoded and
// mip-chained by the import so the port's shell draw needs no TXTR reader and no mip
// generator of its own.
//
// Layout, little-endian throughout:
//   u32 magic    'MPSH' (bytes 4D 50 53 48)
//   u32 version  1
//   u32 count    3 (strength, body indirect, doll indirect, in that order)
//   count x { u32 width, u32 height, u32 mips, u32 srgb,
//             every mip of RGBA8, the top level first, each level
//             max(width >> i, 1) * max(height >> i, 1) * 4 bytes }
// Rows are in the texture's stored order, no flip, and the levels are tightly packed (no
// row padding). `mips` is the whole chain, floor(log2(max(width, height))) + 1 down to 1x1,
// as the import writes it. `srgb` is 1 when the texels are sRGB-encoded.
#include "port_shell_file.h"

#include <algorithm>

#include "port_bytes.h"

namespace PortShell {
namespace {
constexpr uint32_t kMagic = 0x4853504D;  // 'MPSH'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kImages = 3;
// The largest side a file may hold: the shell's textures are small, and a huge one is a
// corrupt header rather than a level worth uploading.
constexpr uint32_t kMaxSide = 4096;

// The levels a width x height chain has: one per halving down to 1x1.
uint32_t ChainMips(uint32_t width, uint32_t height) {
  uint32_t mips = 0;
  for (uint32_t side = std::max(width, height); side != 0; side >>= 1) {
    ++mips;
  }
  return mips;
}

// Every level of a width x height image as RGBA8: the top level first.
size_t ChainBytes(uint32_t width, uint32_t height) {
  size_t bytes = 0;
  for (uint32_t mip = 0; mip < ChainMips(width, height); ++mip) {
    bytes += size_t(std::max(width >> mip, 1u)) * std::max(height >> mip, 1u) * 4;
  }
  return bytes;
}
}  // namespace

std::vector<uint8_t> Serialize(const Assets& assets) {
  std::vector<uint8_t> out;
  port::AppendLE32(out, kMagic);
  port::AppendLE32(out, kVersion);
  port::AppendLE32(out, kImages);
  for (uint32_t i = 0; i < kImages; ++i) {
    const Image& image = assets.image[i];
    port::AppendLE32(out, image.width);
    port::AppendLE32(out, image.height);
    port::AppendLE32(out, image.mips);
    port::AppendLE32(out, image.srgb ? 1 : 0);
    out.insert(out.end(), image.rgba.begin(), image.rgba.end());
  }
  return out;
}

bool Parse(const uint8_t* data, size_t size, Assets& out, std::string& error) {
  if (data == nullptr || size < 12 || port::ReadLE32(data) != kMagic) {
    error = "not a shell.bin";
    return false;
  }
  if (port::ReadLE32(data + 4) != kVersion) {
    error = "shell.bin version " + std::to_string(port::ReadLE32(data + 4)) + " (expected " +
            std::to_string(kVersion) + ")";
    return false;
  }
  if (port::ReadLE32(data + 8) != kImages) {
    error = "shell.bin image count " + std::to_string(port::ReadLE32(data + 8));
    return false;
  }
  size_t at = 12;
  for (uint32_t i = 0; i < kImages; ++i) {
    if (size - at < 16) {
      error = "shell.bin truncated";
      return false;
    }
    Image& image = out.image[i];
    image.width = port::ReadLE32(data + at);
    image.height = port::ReadLE32(data + at + 4);
    image.mips = port::ReadLE32(data + at + 8);
    image.srgb = port::ReadLE32(data + at + 12) != 0;
    at += 16;
    if (image.width < 1 || image.height < 1 || image.width > kMaxSide || image.height > kMaxSide) {
      error = "shell.bin image " + std::to_string(i) + " is " + std::to_string(image.width) + "x" +
              std::to_string(image.height);
      return false;
    }
    if (image.mips != ChainMips(image.width, image.height)) {
      error = "shell.bin image " + std::to_string(i) + " has " + std::to_string(image.mips) + " mips (expected " +
              std::to_string(ChainMips(image.width, image.height)) + ")";
      return false;
    }
    const size_t bytes = ChainBytes(image.width, image.height);
    if (size - at < bytes) {
      error = "shell.bin truncated";
      return false;
    }
    image.rgba.assign(data + at, data + at + bytes);
    at += bytes;
  }
  if (at != size) {
    error = "shell.bin has trailing bytes";
    return false;
  }
  return true;
}
}  // namespace PortShell
