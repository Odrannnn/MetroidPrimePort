// icy.bin: Remastered's frozen-enemy material (the icy MATI RenderLooseDataMP1 names), its
// textures decoded and mip-chained by the import so the port's frozen draw needs no TXTR
// or MATI reader of its own.
//
// Layout, little-endian throughout:
//   u32 magic    'MPIC' (bytes 4D 50 49 43)
//   u32 version  1
//   u32 count    3 (NMAP, METL, TCH0, in that order)
//   count x { u32 width, u32 height, u32 mips, u32 srgb,
//             every mip of RGBA8, the top level first, each level
//             max(width >> i, 1) * max(height >> i, 1) * 4 bytes }
//   4 x f32[4]   CCH0, CCH1, BKLT, BKLA
// The images are laid out as shell.bin's (port_shell_file.cpp): stored row order, no row
// padding, `mips` the whole chain down to 1x1.
#include "port_icy_file.h"

#include <algorithm>
#include <cmath>

#include "port_bytes.h"

namespace PortIcy {
namespace {
constexpr uint32_t kMagic = 0x4349504D;  // 'MPIC'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kImages = kCount;
// The largest side a file may hold: a bigger one is a corrupt header.
constexpr uint32_t kMaxSide = 4096;

uint32_t ChainMips(uint32_t width, uint32_t height) {
  uint32_t mips = 0;
  for (uint32_t side = std::max(width, height); side != 0; side >>= 1) {
    ++mips;
  }
  return mips;
}

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
  for (const auto& vector : assets.vector) {
    for (float v : vector) {
      port::AppendLEFloat(out, v);
    }
  }
  return out;
}

bool Parse(const uint8_t* data, size_t size, Assets& out, std::string& error) {
  if (data == nullptr || size < 12 || port::ReadLE32(data) != kMagic) {
    error = "not an icy.bin";
    return false;
  }
  if (port::ReadLE32(data + 4) != kVersion) {
    error = "icy.bin version " + std::to_string(port::ReadLE32(data + 4)) + " (expected " +
            std::to_string(kVersion) + ")";
    return false;
  }
  if (port::ReadLE32(data + 8) != kImages) {
    error = "icy.bin image count " + std::to_string(port::ReadLE32(data + 8));
    return false;
  }
  size_t at = 12;
  for (uint32_t i = 0; i < kImages; ++i) {
    if (size - at < 16) {
      error = "icy.bin truncated";
      return false;
    }
    Image& image = out.image[i];
    image.width = port::ReadLE32(data + at);
    image.height = port::ReadLE32(data + at + 4);
    image.mips = port::ReadLE32(data + at + 8);
    image.srgb = port::ReadLE32(data + at + 12) != 0;
    at += 16;
    if (image.width < 1 || image.height < 1 || image.width > kMaxSide || image.height > kMaxSide) {
      error = "icy.bin image " + std::to_string(i) + " is " + std::to_string(image.width) + "x" +
              std::to_string(image.height);
      return false;
    }
    if (image.mips != ChainMips(image.width, image.height)) {
      error = "icy.bin image " + std::to_string(i) + " has " + std::to_string(image.mips) + " mips (expected " +
              std::to_string(ChainMips(image.width, image.height)) + ")";
      return false;
    }
    const size_t bytes = ChainBytes(image.width, image.height);
    if (size - at < bytes) {
      error = "icy.bin truncated";
      return false;
    }
    image.rgba.assign(data + at, data + at + bytes);
    at += bytes;
  }
  constexpr size_t kVectorBytes = sizeof(float) * 4 * kVectors;
  if (size - at != kVectorBytes) {
    error = size - at < kVectorBytes ? "icy.bin truncated" : "icy.bin has trailing bytes";
    return false;
  }
  for (auto& vector : out.vector) {
    for (float& v : vector) {
      v = port::ReadLEFloat(data + at);
      at += 4;
      if (!std::isfinite(v)) {
        error = "icy.bin has a constant that is not finite";
        return false;
      }
    }
  }
  return true;
}
}  // namespace PortIcy
