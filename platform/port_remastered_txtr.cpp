// Metroid Prime Remastered TXTR reader (port_remastered_txtr.h): a C++ port of
// retrotool's retrolib TXTR code and of the three crates it calls.
//
// A TXTR is an RFRM form with a HEAD chunk (the texture header), a GPU chunk
// (the still tiled, still compressed texels) and, in an extracted file, a FOOT
// footer holding a META chunk. Decoding therefore takes three steps, each a
// port of the Rust original:
//
//   1. walk the forms and chunks (format/rfrm.rs, format/chunk.rs, foot.rs),
//      then decompress the META's buffers with the LZSS of util/compression.rs
//      and util/lzss.rs;
//   2. until the surface, which is Tegra X1 block linear, with tegra_swizzle
//      0.3.2 (surface.rs, swizzle.rs, blockheight.rs, blockdepth.rs,
//      arrays.rs);
//   3. decode the blocks: astc-decode 0.3.1 for ASTC and bcdec_rs 0.1.2 for
//      BC1/BC2/BC3/BC4/BC5/BC6H/BC7, both ported as they are written there so
//      that the pixels match retrotool's, which is the port's reference.
//
// The three crates' ports live in files of their own, each with its licence:
// port_remastered_tegra_swizzle.cpp, port_remastered_bcdec.cpp and
// port_remastered_astc_decode.cpp, declared in port_remastered_txtr_codecs.h.
//
// tegra_swizzle and bcdec_rs are MIT licensed (Copyright (c) 2017-2018 IOrange,
// Copyright (c) 2016-2017 Smith Lin, Copyright (c) 2021 Nathan Kelso, Copyright
// (c) 2020-2021 hds), astc-decode is Apache 2.0 (Copyright 2021 Weiyi Wang,
// itself derived from an Apache 2.0 decoder at UNC Chapel Hill), and retrotool
// is MIT/Apache-2.0. The port keeps their arithmetic, including the "approximate"
// integer expansions in bcdec's colour blocks and astc's tiling, because being
// bit exact with retrotool is the whole point: the assets this reads are the
// ones retrotool extracted.
//
// Two places deliberately differ from the Rust, both because this file must
// return instead of panicking:
//   * astc-decode's asserts on a malformed block (an out of range weight, an
//     illegal block mode) become a failed decode, and the block is filled with
//     astc's own error colour. retrotool aborts the whole conversion there.
//   * BC6H has no 8 bit form, so its half floats are reduced to linear bytes
//     by value, clamped to [0, 1] (HalfToUnormByte). retrotool cannot write
//     BC6H at all (its PNG writer has no 32 bit float case), which is why
//     nothing checks this one against it. A consumer that wants the full range
//     reads the blocks themselves (ReadTxtrMipsBc6h).

#include "port_remastered_txtr_codecs.h"
#include "port_remastered_txtr.h"
#include "port_bytes.h"
#include "port_remastered_lzss.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace PortRemastered {
namespace {

using port::ReadBE32;
using port::ReadLE32;
using port::ReadLE64;
using namespace TxtrDetail;

// A form or chunk range in the file, both of which start with a header that
// may be followed by padding (`skip` for a chunk) before the payload.
struct Slice {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

constexpr uint32_t kFormRFRM = 0x5246524D;  // 'RFRM' big endian packed
constexpr uint32_t kFormTXTR = 0x54585452;  // 'TXTR'
constexpr uint32_t kFormFOOT = 0x464F4F54;  // 'FOOT'
constexpr uint32_t kChunkHEAD = 0x48454144;  // 'HEAD'
constexpr uint32_t kChunkMETA = 0x4D455441;  // 'META'

constexpr size_t kFormSize = 32;
constexpr size_t kChunkSize = 24;

// Cuts one form off the front of `data`. Forms nest: the file is the TXTR form
// followed by the FOOT form, and a form's payload is a run of chunks.
bool TakeForm(const uint8_t* data, size_t size, uint32_t& id, Slice& body, size_t& consumed,
              std::string& error) {
  if (size < kFormSize) {
    error = "remastered txtr: truncated form header";
    return false;
  }
  if (ReadBE32(data) != kFormRFRM) {
    error = "remastered txtr: not an RFRM form";
    return false;
  }
  const uint64_t bodySize = ReadLE64(data + 4);
  if (bodySize > size - kFormSize) {
    error = "remastered txtr: form claims " + std::to_string(bodySize) + " bytes, " +
            std::to_string(size - kFormSize) + " are here";
    return false;
  }
  id = ReadBE32(data + 20);
  body.data = data + kFormSize;
  body.size = size_t(bodySize);
  consumed = kFormSize + size_t(bodySize);
  return true;
}

// Cuts one chunk off the front of `data`. A chunk may ask for `skip` bytes of
// padding before its payload, which the game uses to align the payload.
bool TakeChunk(const uint8_t* data, size_t size, uint32_t& id, Slice& body, size_t& consumed,
               std::string& error) {
  if (size < kChunkSize) {
    error = "remastered txtr: truncated chunk header";
    return false;
  }
  const uint32_t chunkId = ReadBE32(data);
  const uint64_t bodySize = ReadLE64(data + 4);
  const uint64_t skip = ReadLE64(data + 16);
  const uint64_t start = uint64_t(kChunkSize) + skip;
  if (skip > size - kChunkSize || bodySize > size - kChunkSize - skip) {
    error = "remastered txtr: chunk " + std::to_string(chunkId) + " runs past the end of the file";
    return false;
  }
  id = chunkId;
  body.data = data + start;
  body.size = size_t(bodySize);
  consumed = size_t(start + bodySize);
  return true;
}

// The HEAD chunk's texture header (txtr.rs STextureHeader).
struct TextureHeader {
  uint32_t kind = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 0;
  uint32_t tileMode = 0;
  uint32_t swizzle = 0;
  std::vector<uint32_t> mipSizes;
};

// Reads the TXTR form and its HEAD chunk. The GPU chunk that follows is not
// touched: retrotool takes the texels from the META buffers, not from it.
bool ReadHeader(const uint8_t* data, size_t size, TextureHeader& head, std::string& error) {
  uint32_t formId = 0;
  Slice form{};
  size_t consumed = 0;
  if (!TakeForm(data, size, formId, form, consumed, error)) {
    return false;
  }
  if (formId != kFormTXTR) {
    error = "remastered txtr: form is not a TXTR";
    return false;
  }
  // reader and writer versions sit after the form's id field.
  if (ReadLE32(data + 24) != 47 || ReadLE32(data + 28) != 51) {
    error = "remastered txtr: TXTR reader/writer version is " + std::to_string(ReadLE32(data + 24)) +
            "/" + std::to_string(ReadLE32(data + 28)) + ", expected 47/51";
    return false;
  }

  uint32_t chunkId = 0;
  Slice chunk{};
  size_t chunkUsed = 0;
  if (!TakeChunk(form.data, form.size, chunkId, chunk, chunkUsed, error)) {
    return false;
  }
  if (chunkId != kChunkHEAD) {
    error = "remastered txtr: the first chunk is not a HEAD";
    return false;
  }
  // kind, format, width, height, layers, tile mode, swizzle, mip count.
  if (chunk.size < 32) {
    error = "remastered txtr: HEAD chunk is " + std::to_string(chunk.size) + " bytes";
    return false;
  }
  head.kind = ReadLE32(chunk.data + 0);
  head.format = ReadLE32(chunk.data + 4);
  head.width = ReadLE32(chunk.data + 8);
  head.height = ReadLE32(chunk.data + 12);
  head.layers = ReadLE32(chunk.data + 16);
  head.tileMode = ReadLE32(chunk.data + 20);
  head.swizzle = ReadLE32(chunk.data + 24);
  const uint32_t mipCount = ReadLE32(chunk.data + 28);
  if (mipCount > (chunk.size - 32) / 4) {
    error = "remastered txtr: HEAD claims " + std::to_string(mipCount) + " mip sizes in " +
            std::to_string(chunk.size) + " bytes";
    return false;
  }
  head.mipSizes.resize(mipCount);
  for (uint32_t i = 0; i < mipCount; ++i) {
    head.mipSizes[i] = ReadLE32(chunk.data + 32 + i * 4);
  }
  return true;
}

// retrotool's decompress_into (util/compression.rs): a mode word then either
// the data as it is or an LZSS variant of it.
bool DecompressInto(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize,
                    std::string& error) {
  if (inSize < 4) {
    error = "remastered txtr: compressed buffer is shorter than its mode word";
    return false;
  }
  const uint32_t mode = ReadLE32(in);
  const uint8_t* body = in + 4;
  const size_t bodySize = inSize - 4;
  if (mode == 0) {
    if (bodySize != outSize) {
      error = "remastered txtr: stored buffer is " + std::to_string(bodySize) + " bytes, " +
              std::to_string(outSize) + " were expected";
      return false;
    }
    std::memcpy(out, body, bodySize);
    return true;
  }
  if (mode >= 1 && mode <= 3 && LzssDecompress(mode, body, bodySize, out, outSize)) {
    return true;
  }
  error = "remastered txtr: decompression failed (mode " + std::to_string(mode) + ")";
  return false;
}

// The META chunk (txtr.rs STextureMetaData). Its `info` entries point at the
// compressed buffers inside the file and its `buffers` say where each one
// decompresses to, so the texel data is reassembled from several pieces.
struct Meta {
  uint32_t decompressedSize = 0;
  struct Read {
    uint8_t index = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
  };
  struct Buffer {
    uint32_t index = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t destOffset = 0;
    uint64_t destSize = 0;
  };
  std::vector<Read> reads;
  std::vector<Buffer> buffers;
};

bool ReadMeta(const uint8_t* data, size_t size, Meta& meta, std::string& error) {
  uint32_t formId = 0;
  Slice form{};
  size_t consumed = 0;
  if (!TakeForm(data, size, formId, form, consumed, error)) {
    return false;
  }
  if (!TakeForm(data + consumed, size - consumed, formId, form, consumed, error)) {
    return false;
  }
  if (formId != kFormFOOT) {
    error = "remastered txtr: the file has no FOOT footer, so its texel data cannot be found";
    return false;
  }
  Slice metaChunk{};
  size_t used = 0;
  while (form.size > 0) {
    uint32_t chunkId = 0;
    Slice chunk{};
    if (!TakeChunk(form.data, form.size, chunkId, chunk, used, error)) {
      return false;
    }
    form.data += used;
    form.size -= used;
    if (chunkId == kChunkMETA) {
      metaChunk = chunk;
      break;
    }
  }
  if (metaChunk.data == nullptr) {
    error = "remastered txtr: the FOOT footer has no META chunk";
    return false;
  }

  const uint8_t* p = metaChunk.data;
  const size_t n = metaChunk.size;
  // unk, unk, alloc category, gpu offset, align, decompressed size.
  if (n < 28) {
    error = "remastered txtr: META chunk is " + std::to_string(n) + " bytes";
    return false;
  }
  meta.decompressedSize = ReadLE32(p + 20);
  const uint32_t readCount = ReadLE32(p + 24);
  p += 28;
  // Each read entry is a byte, then two u32s, with no padding.
  if (readCount > (n - 28) / 9) {
    error = "remastered txtr: META claims " + std::to_string(readCount) + " read entries";
    return false;
  }
  meta.reads.resize(readCount);
  for (uint32_t i = 0; i < readCount; ++i, p += 9) {
    meta.reads[i].index = p[0];
    meta.reads[i].offset = ReadLE32(p + 1);
    meta.reads[i].size = ReadLE32(p + 5);
  }
  if (n < 28 + size_t(readCount) * 9 + 4) {
    error = "remastered txtr: META has no buffer count";
    return false;
  }
  const uint32_t bufferCount = ReadLE32(p);
  p += 4;
  if (bufferCount > (n - (p - metaChunk.data)) / 20) {
    error = "remastered txtr: META claims " + std::to_string(bufferCount) + " buffers";
    return false;
  }
  meta.buffers.resize(bufferCount);
  for (uint32_t i = 0; i < bufferCount; ++i, p += 20) {
    meta.buffers[i].index = ReadLE32(p + 0);
    meta.buffers[i].offset = ReadLE32(p + 4);
    meta.buffers[i].size = ReadLE32(p + 8);
    meta.buffers[i].destOffset = ReadLE32(p + 12);
    meta.buffers[i].destSize = ReadLE32(p + 16);
  }
  return true;
}

// Decompresses the buffers of the META into one buffer, which is the surface as
// it is stored in memory: tiled, and holding every layer and mip. With `needed`,
// only the start of the surface is wanted (the top mip of the first layer, which
// is laid out first): a buffer that writes nowhere below it is not decompressed,
// and the surface ends with the last buffer that is.
bool BuildSurface(const uint8_t* file, size_t fileSize, const Meta& meta,
                  std::vector<uint8_t>& surface, std::string& error, uint64_t needed = UINT64_MAX) {
  if (meta.decompressedSize > 0x40000000u) {
    error = "remastered txtr: META asks for a " + std::to_string(meta.decompressedSize) +
            " byte surface";
    return false;
  }
  const auto wanted = [&](const Meta::Buffer& buffer) { return buffer.destOffset < needed; };
  uint64_t end = std::min<uint64_t>(needed, meta.decompressedSize);
  for (const Meta::Buffer& buffer : meta.buffers) {
    if (wanted(buffer) && buffer.destOffset <= meta.decompressedSize &&
        buffer.destSize <= meta.decompressedSize - buffer.destOffset) {
      end = std::max(end, buffer.destOffset + buffer.destSize);
    }
  }
  surface.assign(size_t(end), 0);
  for (const Meta::Buffer& buffer : meta.buffers) {
    if (!wanted(buffer)) {
      continue;
    }
    const Meta::Read* read = nullptr;
    for (const Meta::Read& candidate : meta.reads) {
      if (candidate.index == buffer.index) {
        read = &candidate;
        break;
      }
    }
    if (read == nullptr) {
      error = "remastered txtr: no read info for buffer " + std::to_string(buffer.index);
      return false;
    }
    if (read->offset > fileSize || read->size > fileSize - read->offset) {
      error = "remastered txtr: a texel buffer runs past the end of the file";
      return false;
    }
    if (buffer.offset > read->size || buffer.size > read->size - buffer.offset) {
      error = "remastered txtr: a texel buffer runs past the end of its read range";
      return false;
    }
    if (buffer.destOffset > meta.decompressedSize ||
        buffer.destSize > meta.decompressedSize - buffer.destOffset) {
      error = "remastered txtr: a texel buffer decompresses past the end of the surface";
      return false;
    }
    if (!DecompressInto(file + read->offset + buffer.offset, size_t(buffer.size),
                        surface.data() + buffer.destOffset, size_t(buffer.destSize), error)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Format tables (txtr.rs ETextureFormat)
// ---------------------------------------------------------------------------

struct BlockSize {
  uint32_t width = 1;
  uint32_t height = 1;
};

bool AstcBlockSize(uint32_t format, BlockSize& size) {
  if (format < kTxtrFormatAstc4x4 || format > kTxtrFormatAstc12x12Srgb) {
    return false;
  }
  // 53 to 66 are the linear footprints and 67 to 80 the sRGB ones, in the same
  // order: 4x4, 5x4, 5x5, 6x5, 6x6, 8x5, 8x6, 8x8, 10x5, 10x6, 10x8, 10x10,
  // 12x10, 12x12.
  static const uint8_t kWidths[14] = {4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12};
  static const uint8_t kHeights[14] = {4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12};
  const uint32_t index = format >= kTxtrFormatAstc4x4Srgb ? format - kTxtrFormatAstc4x4Srgb
                                                         : format - kTxtrFormatAstc4x4;
  size.width = kWidths[index];
  size.height = kHeights[index];
  return true;
}

// The block a format's texels are grouped in, and how many bytes one of them
// takes. Uncompressed formats are 1x1.
bool FormatBlockSize(uint32_t format, BlockSize& size, size_t& bytesPerPixel) {
  bytesPerPixel = 1;
  switch (format) {
    case kTxtrFormatR8Unorm:
      return true;
    case kTxtrFormatRgb8Unorm:
      bytesPerPixel = 3;
      return true;
    case kTxtrFormatRgba8Unorm:
    case kTxtrFormatRgba8Srgb:
      bytesPerPixel = 4;
      return true;
    case kTxtrFormatBc1Unorm:
    case kTxtrFormatBc1Srgb:
      size = BlockSize{4, 4};
      bytesPerPixel = 8;
      return true;
    case kTxtrFormatBc2Unorm:
    case kTxtrFormatBc2Srgb:
    case kTxtrFormatBc3Unorm:
    case kTxtrFormatBc3Srgb:
      size = BlockSize{4, 4};
      bytesPerPixel = 16;
      return true;
    case kTxtrFormatBc4Unorm:
    case kTxtrFormatBc4Snorm:
      size = BlockSize{4, 4};
      bytesPerPixel = 8;
      return true;
    case kTxtrFormatBc5Unorm:
    case kTxtrFormatBc5Snorm:
      size = BlockSize{4, 4};
      bytesPerPixel = 16;
      return true;
    case kTxtrFormatBc6hUfloat:
    case kTxtrFormatBc6hSfloat:
    case kTxtrFormatBc7Unorm:
    case kTxtrFormatBc7UnormSrgb:
      size = BlockSize{4, 4};
      bytesPerPixel = 16;
      return true;
    default:
      return AstcBlockSize(format, size) && (bytesPerPixel = 16, true);
  }
}

bool FormatIsSrgb(uint32_t format) {
  return format == kTxtrFormatRgba8Srgb || format == kTxtrFormatBc1Srgb || format == kTxtrFormatBc2Srgb ||
         format == kTxtrFormatBc3Srgb || format == kTxtrFormatBc7UnormSrgb ||
         (format >= kTxtrFormatAstc4x4Srgb && format <= kTxtrFormatAstc12x12Srgb);
}

const char* FormatName(uint32_t format) {
  switch (format) {
    case kTxtrFormatR8Unorm: return "R8 UNORM";
    case kTxtrFormatRgb8Unorm: return "RGB8 UNORM";
    case kTxtrFormatRgba8Unorm: return "RGBA8 UNORM";
    case kTxtrFormatRgba8Srgb: return "RGBA8 UNORM (sRGB)";
    case kTxtrFormatBc1Unorm: return "BC1 UNORM";
    case kTxtrFormatBc1Srgb: return "BC1 UNORM (sRGB)";
    case kTxtrFormatBc2Unorm: return "BC2 UNORM";
    case kTxtrFormatBc2Srgb: return "BC2 UNORM (sRGB)";
    case kTxtrFormatBc3Unorm: return "BC3 UNORM";
    case kTxtrFormatBc3Srgb: return "BC3 UNORM (sRGB)";
    case kTxtrFormatBc4Unorm: return "BC4 UNORM";
    case kTxtrFormatBc4Snorm: return "BC4 SNORM";
    case kTxtrFormatBc5Unorm: return "BC5 UNORM";
    case kTxtrFormatBc5Snorm: return "BC5 SNORM";
    case kTxtrFormatBc6hUfloat: return "BC6H UFLOAT";
    case kTxtrFormatBc6hSfloat: return "BC6H SFLOAT";
    case kTxtrFormatBc7Unorm: return "BC7 UNORM";
    case kTxtrFormatBc7UnormSrgb: return "BC7 UNORM (sRGB)";
    default:
      if (format >= kTxtrFormatAstc4x4 && format <= kTxtrFormatAstc12x12) {
        return "ASTC";
      }
      if (format >= kTxtrFormatAstc4x4Srgb && format <= kTxtrFormatAstc12x12Srgb) {
        return "ASTC (sRGB)";
      }
      return "an unsupported format";
  }
}

// Half to float, with an infinity clamped to the largest finite half like the reference tool does.
float HalfBitsToFloat(uint16_t h) {
  const int e = (h >> 10) & 31, m = h & 1023;
  const float sign = (h & 0x8000) ? -1.f : 1.f;
  if (e == 0) {
    return sign * std::ldexp(float(m), -24);
  }
  if (e == 31) {
    return sign * 65504.f;
  }
  return sign * std::ldexp(float(m + 1024), e - 25);
}

// Fills the RGBA image from the untiled top mip. BC4 and BC5 become grey and
// grey plus alpha, which is how retrotool writes them out as well; BC6H's half
// floats become linear bytes by value (HalfToUnormByte), because it has no 8 bit
// form of its own.
bool DecodeBlocks(uint32_t format, uint32_t width, uint32_t height, const uint8_t* blocks,
                  uint8_t* rgba, std::string& error) {
  if (width == 0 || height == 0) {
    error = "remastered txtr: the texture has a zero dimension";
    return false;
  }
  BlockSize block{};
  size_t bytesPerPixel = 1;
  if (!FormatBlockSize(format, block, bytesPerPixel)) {
    error = "remastered txtr: cannot decode " + std::string(FormatName(format));
    return false;
  }
  const uint32_t blocksX = uint32_t(DivRoundUp(width, block.width));
  const uint32_t blocksY = uint32_t(DivRoundUp(height, block.height));

  if (block.width == 1 && block.height == 1) {
    const size_t pixels = size_t(width) * height;
    for (size_t i = 0; i < pixels; ++i) {
      uint8_t* p = rgba + i * 4;
      if (bytesPerPixel == 1) {
        p[0] = p[1] = p[2] = blocks[i];
        p[3] = 255;
      } else if (bytesPerPixel == 3) {
        p[0] = blocks[i * 3 + 0];
        p[1] = blocks[i * 3 + 1];
        p[2] = blocks[i * 3 + 2];
        p[3] = 255;
      } else {
        std::memcpy(p, blocks + i * 4, 4);
      }
    }
    return true;
  }

  if (AstcBlockSize(format, block)) {
    // ASTC: the block grid is walked in the same order the data is stored in,
    // and the texels outside the image (at the right and bottom edges) go.
    for (uint32_t by = 0; by < blocksY; ++by) {
      for (uint32_t bx = 0; bx < blocksX; ++bx) {
        uint8_t decoded[12 * 12 * 4] = {};
        DecodeAstcBlock(blocks + (size_t(by) * blocksX + bx) * 16, block.width, block.height,
                        decoded);
        for (uint32_t y = 0; y < block.height; ++y) {
          const uint32_t py = by * block.height + y;
          if (py >= height) {
            continue;
          }
          for (uint32_t x = 0; x < block.width; ++x) {
            const uint32_t px = bx * block.width + x;
            if (px >= width) {
              continue;
            }
            std::memcpy(rgba + (size_t(py) * width + px) * 4, decoded + (y * block.width + x) * 4,
                        4);
          }
        }
      }
    }
    return true;
  }

  // The 4x4 block decoders. Each block is decoded on its own and then copied in,
  // which keeps the partial blocks at a texture's right and bottom edge from
  // writing outside the image.
  {
    uint8_t decoded[4 * 4 * 4] = {};
    for (uint32_t by = 0; by < blocksY; ++by) {
      for (uint32_t bx = 0; bx < blocksX; ++bx) {
        const uint8_t* src = blocks + (size_t(by) * blocksX + bx) * bytesPerPixel;
        switch (format) {
          case kTxtrFormatBc1Unorm:
          case kTxtrFormatBc1Srgb:
            DecodeColorBlock(src, decoded, false);
            break;
          case kTxtrFormatBc2Unorm:
          case kTxtrFormatBc2Srgb:
            DecodeColorBlock(src + 8, decoded, true);
            DecodeSharpAlpha(src, decoded);
            break;
          case kTxtrFormatBc3Unorm:
          case kTxtrFormatBc3Srgb:
            DecodeColorBlock(src + 8, decoded, true);
            DecodeSmoothAlpha(src, decoded, 4, 3);
            break;
          case kTxtrFormatBc4Unorm:
          case kTxtrFormatBc4Snorm:
            // One channel, so the block is grey with opaque alpha.
            DecodeSmoothAlpha(src, decoded, 4, 0);
            for (size_t i = 0; i < 16; ++i) {
              decoded[i * 4 + 1] = decoded[i * 4 + 2] = decoded[i * 4 + 0];
              decoded[i * 4 + 3] = 255;
            }
            break;
          case kTxtrFormatBc5Unorm:
          case kTxtrFormatBc5Snorm:
            // Two channels: the first is grey, the second is alpha.
            DecodeSmoothAlpha(src, decoded, 4, 0);
            DecodeSmoothAlpha(src + 8, decoded, 4, 3);
            for (size_t i = 0; i < 16; ++i) {
              decoded[i * 4 + 1] = decoded[i * 4 + 2] = decoded[i * 4 + 0];
            }
            break;
          case kTxtrFormatBc6hUfloat:
          case kTxtrFormatBc6hSfloat: {
            // Three half float channels, linear: their values clamped to [0, 1] as
            // linear bytes (the format is not sRGB, so consumers read them raw).
            DecodeBc6hBlockRgba8(src, format == kTxtrFormatBc6hSfloat, decoded);
            break;
          }
          case kTxtrFormatBc7Unorm:
          case kTxtrFormatBc7UnormSrgb:
            DecodeBc7(src, decoded);
            break;
          default:
            error = "remastered txtr: cannot decode " + std::string(FormatName(format));
            return false;
        }
        for (uint32_t y = 0; y < block.height; ++y) {
          const uint32_t py = by * block.height + y;
          if (py >= height) {
            continue;
          }
          for (uint32_t x = 0; x < block.width; ++x) {
            const uint32_t px = bx * block.width + x;
            if (px >= width) {
              continue;
            }
            std::memcpy(rgba + (size_t(py) * width + px) * 4, decoded + (y * block.width + x) * 4,
                        4);
          }
        }
      }
    }
    return true;
  }
}

}  // namespace

bool ReadTxtrInfo(const uint8_t* data, size_t size, TxtrImage& out, std::string& error) {
  out = TxtrImage{};
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  out.width = head.width;
  out.height = head.height;
  out.format = head.format;
  out.srgb = FormatIsSrgb(head.format);
  out.mipCount = uint32_t(head.mipSizes.size());
  return true;
}

bool DecodeDds(const uint8_t* data, size_t size, TxtrImage& out, std::string& error) {
  out = TxtrImage{};
  // 'DDS ', the 124 byte header, then the 20 byte DX10 one.
  constexpr size_t kDataOffset = 4 + 124 + 20;
  if (data == nullptr || size < kDataOffset || std::memcmp(data, "DDS ", 4) != 0 ||
      std::memcmp(data + 84, "DX10", 4) != 0) {
    error = "dds: not a DX10 .dds";
    return false;
  }
  const uint32_t height = port::ReadLE32(data + 12), width = port::ReadLE32(data + 16);
  const uint32_t mips = port::ReadLE32(data + 28), dxgi = port::ReadLE32(data + 128);
  uint32_t format = 0;
  switch (dxgi) {
    case 28: format = kTxtrFormatRgba8Unorm; break;
    case 29: format = kTxtrFormatRgba8Srgb; break;
    case 98: format = kTxtrFormatBc7Unorm; break;
    case 99: format = kTxtrFormatBc7UnormSrgb; break;
    case 134: format = kTxtrFormatAstc4x4; break;
    case 135: format = kTxtrFormatAstc4x4Srgb; break;
    default:
      error = "dds: cannot decode DXGI format " + std::to_string(dxgi);
      return false;
  }
  constexpr uint32_t kMaxSide = 16384;
  if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide) {
    error = "dds: bad size " + std::to_string(width) + "x" + std::to_string(height);
    return false;
  }
  const bool blocks = format != kTxtrFormatRgba8Unorm && format != kTxtrFormatRgba8Srgb;
  const size_t need = blocks ? DivRoundUp(width, 4) * DivRoundUp(height, 4) * 16 : size_t(width) * height * 4;
  if (size - kDataOffset < need) {
    error = "dds: the top mip is cut short";
    return false;
  }
  out.width = width;
  out.height = height;
  out.format = format;
  out.srgb = FormatIsSrgb(format);
  out.mipCount = std::max<uint32_t>(mips, 1);
  out.rgba.assign(size_t(width) * height * 4, 0);
  return DecodeBlocks(format, width, height, data + kDataOffset, out.rgba.data(), error);
}

bool DecodeTxtr(const uint8_t* data, size_t size, TxtrImage& out, std::string& error) {
  out = TxtrImage{};
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  out.width = head.width;
  out.height = head.height;
  out.format = head.format;
  out.srgb = FormatIsSrgb(head.format);
  out.mipCount = uint32_t(head.mipSizes.size());
  if (head.width == 0 || head.height == 0 || head.layers == 0) {
    error = "remastered txtr: the texture has a zero dimension";
    return false;
  }
  // No real texture is larger; a corrupt header would otherwise ask for
  // gigabytes of RGBA below before the surface check could refuse it.
  constexpr uint32_t kMaxSide = 16384;
  if (head.width > kMaxSide || head.height > kMaxSide) {
    error = "remastered txtr: " + std::to_string(head.width) + "x" + std::to_string(head.height) +
            " is larger than any texture can be";
    return false;
  }

  BlockSize block{};
  size_t bytesPerPixel = 1;
  if (!FormatBlockSize(head.format, block, bytesPerPixel)) {
    error = "remastered txtr: cannot decode " + std::string(FormatName(head.format));
    return false;
  }

  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }

  // A 3D texture's depth is its layer count; for everything else the layers sit
  // side by side and layer 0 is the first slice of the surface either way.
  const bool is3d = head.kind == 2;
  const size_t depth = is3d ? head.layers : 1;
  const size_t mipWidth = DivRoundUp(size_t(head.width), block.width);
  const size_t mipHeight = DivRoundUp(size_t(head.height), block.height);
  const size_t mipDepth = depth;  // the block depth is always one
  const uint32_t blockHeightMip0 =
      depth == 1 ? BlockHeightMip0(DivRoundUp(size_t(head.height), block.height)) : 1;
  const uint32_t mipBlockHeight = MipBlockHeight(mipHeight, blockHeightMip0);
  const size_t mipBlockDepth = MipBlockDepth(mipDepth, BlockDepth(depth));

  const size_t swizzledSize = SwizzledMipSize(mipWidth, mipHeight, mipDepth, mipBlockHeight,
                                              bytesPerPixel);
  if (meta.decompressedSize < swizzledSize) {
    error = "remastered txtr: the surface is " + std::to_string(meta.decompressedSize) +
            " bytes, its top mip alone needs " + std::to_string(swizzledSize);
    return false;
  }
  // Only the top mip of the first layer is decoded, and it comes first: the
  // buffers holding the smaller mips and the other layers are left compressed.
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error, swizzledSize)) {
    return false;
  }
  const size_t sliceSize = mipWidth * mipHeight * bytesPerPixel;
  std::vector<uint8_t> untiled(sliceSize);
  // Only layer 0 is decoded: `untiled` holds one slice, and DeswizzleMip writes
  // every slice it is given to the same place. The size check above covers the
  // whole depth, and the slice offsets come from the block depth, not this one.
  DeswizzleMip(mipWidth, mipHeight, 1, mipBlockHeight, mipBlockDepth, bytesPerPixel,
               surface.data(), 0, untiled.data());

  out.rgba.assign(size_t(head.width) * head.height * 4, 0);
  return DecodeBlocks(head.format, head.width, head.height, untiled.data(), out.rgba.data(),
                      error);
}

bool ReadTxtrCubeBc6h(const uint8_t* data, size_t size, TxtrCubeBc6h& out, std::string& error) {
  out = TxtrCubeBc6h{};
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind != 3 || head.layers != 6 || head.width != head.height || head.width == 0 ||
      head.width > 4096 || head.mipSizes.empty()) {
    error = "remastered txtr: not a cube map";
    return false;
  }
  if (head.format != kTxtrFormatBc6hUfloat && head.format != kTxtrFormatBc6hSfloat) {
    error = "remastered txtr: the cube is " + std::string(FormatName(head.format)) + ", not BC6H";
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }

  // The mip chain stops where the file's does, or at 1x1.
  uint32_t mipCount = 0;
  while (mipCount < head.mipSizes.size() && (head.width >> mipCount) != 0) {
    ++mipCount;
  }
  const size_t bytesPerBlock = 16;
  const uint32_t blockHeightMip0 = BlockHeightMip0(DivRoundUp(size_t(head.width), 4));

  out.size = head.width;
  out.mipCount = mipCount;
  out.isSigned = head.format == kTxtrFormatBc6hSfloat;
  out.mips.resize(size_t(mipCount) * 6);

  // tegra_swizzle's surface layout: each layer holds its whole mip chain, and
  // a layer starts on a multiple of the block of GOBs the top mip uses.
  size_t srcOffset = 0;
  for (uint32_t layer = 0; layer < 6; ++layer) {
    for (uint32_t mip = 0; mip < head.mipSizes.size(); ++mip) {
      const uint32_t texels = std::max(head.width >> mip, 1u);
      const size_t blocks = DivRoundUp(size_t(texels), 4);
      const uint32_t mipBlockHeight = MipBlockHeight(blocks, blockHeightMip0);
      const size_t swizzled = SwizzledMipSize(blocks, blocks, 1, mipBlockHeight, bytesPerBlock);
      if (swizzled > surface.size() || srcOffset > surface.size() - swizzled) {
        error = "remastered txtr: the cube's surface is short";
        return false;
      }
      if (mip < mipCount) {
        std::vector<uint8_t>& untiled = out.mips[size_t(mip) * 6 + layer];
        untiled.assign(blocks * blocks * bytesPerBlock, 0);
        DeswizzleMip(blocks, blocks, 1, mipBlockHeight, 1, bytesPerBlock, surface.data(), srcOffset,
                     untiled.data());
      }
      srcOffset += swizzled;
    }
    // align_layer_size, with the block height shrunk to fit the top mip.
    uint32_t gobHeight = blockHeightMip0;
    while (head.width <= (gobHeight / 2) * 8 && gobHeight > 1) {
      gobHeight /= 2;
    }
    const size_t unit = size_t(gobHeight) * kGobSizeBytes;
    srcOffset = DivRoundUp(srcOffset, unit) * unit;
  }
  return true;
}

bool DecodeVolumeFloat(const uint8_t* compressed, size_t compressedSize, size_t surfaceSize, uint32_t format,
                       uint32_t width, uint32_t height, uint32_t depth, std::vector<float>& rgba,
                       std::string& error) {
  rgba.clear();
  const bool bc6h = format == kTxtrFormatBc6hUfloat || format == kTxtrFormatBc6hSfloat;
  if (!bc6h && format != kTxtrFormatBc1Unorm) {
    error = "remastered txtr: a volume of " + std::string(FormatName(format)) + " is not supported";
    return false;
  }
  if (width == 0 || height == 0 || depth == 0 || width % 4 != 0 || height % 4 != 0 || width > 1024 ||
      height > 1024 || depth > 256 || size_t(width) * height * depth > (size_t(1) << 24) || surfaceSize > 0x40000000u) {
    error = "remastered txtr: a volume of " + std::to_string(width) + "x" + std::to_string(height) + "x" +
            std::to_string(depth);
    return false;
  }
  std::vector<uint8_t> surface(surfaceSize);
  if (!DecompressInto(compressed, compressedSize, surface.data(), surfaceSize, error)) {
    return false;
  }
  const size_t bytesPerBlock = bc6h ? 16 : 8;
  const size_t bw = width / 4, bh = height / 4, rowBytes = bw * bytesPerBlock, wg = DivRoundUp(rowBytes, 64);
  const size_t blockDepth = BlockDepth(depth);
  // Untile into plain rows of blocks, a byte at a time: the volumes are small.
  const size_t bhg = BlockHeightMip0(bh), blk = 512 * bhg * blockDepth;
  std::vector<uint8_t> linear(rowBytes * bh * depth);
  for (size_t z = 0; z < depth; ++z) {
    for (size_t y = 0; y < bh; ++y) {
      for (size_t x = 0; x < rowBytes; ++x) {
        const size_t a = (z / blockDepth) * DivRoundUp(bh, 8 * bhg) * blk * wg + (z & (blockDepth - 1)) * 512 * bhg +
                         (y / (8 * bhg)) * blk * wg + (x / 64) * blk + ((y % (8 * bhg)) / 8) * 512 + GobOffset(x, y);
        if (a >= surface.size()) {
          error = "remastered txtr: the volume's surface is short";
          return false;
        }
        linear[(z * bh + y) * rowBytes + x] = surface[a];
      }
    }
  }
  rgba.assign(size_t(width) * height * depth * 4, 0.f);
  for (size_t z = 0; z < depth; ++z) {
    for (size_t y = 0; y < bh; ++y) {
      for (size_t x = 0; x < bw; ++x) {
        const uint8_t* src = &linear[(z * bh + y) * rowBytes + x * bytesPerBlock];
        uint16_t half[48] = {};
        uint8_t color[64] = {};
        if (bc6h) {
          DecodeBc6h(src, half, format == kTxtrFormatBc6hSfloat);
        } else {
          DecodeColorBlock(src, color, false);
        }
        for (int i = 0; i < 16; ++i) {
          float* p = &rgba[((z * height + y * 4 + i / 4) * width + x * 4 + i % 4) * 4];
          if (bc6h) {
            p[0] = HalfBitsToFloat(half[i * 3]);
            p[1] = HalfBitsToFloat(half[i * 3 + 1]);
            p[2] = HalfBitsToFloat(half[i * 3 + 2]);
            p[3] = 1.f;
          } else {
            for (int k = 0; k < 4; ++k) {
              p[k] = color[i * 4 + k] / 255.f;
            }
          }
        }
      }
    }
  }
  return true;
}

bool DecodeTxtrVolumeRgba8(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, uint32_t& depth,
                           std::vector<uint8_t>& rgba, std::string& error) {
  rgba.clear();
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind != 2) {
    error = "remastered txtr: not a 3D texture";
    return false;
  }
  if (head.format != kTxtrFormatRgba8Unorm && head.format != kTxtrFormatRgba8Srgb) {
    error = "remastered txtr: a volume of " + std::string(FormatName(head.format)) + " is not supported";
    return false;
  }
  width = head.width;
  height = head.height;
  depth = head.layers;
  if (width == 0 || height == 0 || depth == 0 || width > 256 || height > 256 || depth > 256) {
    error = "remastered txtr: a volume of " + std::to_string(width) + "x" + std::to_string(height) + "x" +
            std::to_string(depth);
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }
  // The same untiling as DecodeVolumeFloat's, with a texel for a block. A 3D texture's
  // blocks are one GOB tall, as in DecodeTxtr (33^3 LUTs: 368640 bytes, not the 589824 of 4).
  const size_t rowBytes = size_t(width) * 4, wg = DivRoundUp(rowBytes, 64);
  const size_t blockDepth = BlockDepth(depth);
  const size_t bhg = 1, blk = 512 * bhg * blockDepth;
  rgba.assign(rowBytes * height * depth, 0);
  for (size_t z = 0; z < depth; ++z) {
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < rowBytes; ++x) {
        const size_t a = (z / blockDepth) * DivRoundUp(height, 8 * bhg) * blk * wg +
                         (z & (blockDepth - 1)) * 512 * bhg + (y / (8 * bhg)) * blk * wg + (x / 64) * blk +
                         ((y % (8 * bhg)) / 8) * 512 + GobOffset(x, y);
        if (a >= surface.size()) {
          error = "remastered txtr: the volume's surface is short";
          rgba.clear();
          return false;
        }
        rgba[(z * height + y) * rowBytes + x] = surface[a];
      }
    }
  }
  return true;
}

bool DecodeTxtrVolume(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, uint32_t& depth,
                      bool& srgb, std::vector<uint8_t>& rgba, std::string& error) {
  rgba.clear();
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind != 2) {
    error = "remastered txtr: not a 3D texture";
    return false;
  }
  width = head.width;
  height = head.height;
  depth = head.layers;
  srgb = FormatIsSrgb(head.format);
  if (width == 0 || height == 0 || depth == 0 || width > 256 || height > 256 || depth > 256) {
    error = "remastered txtr: a volume of " + std::to_string(width) + "x" + std::to_string(height) + "x" +
            std::to_string(depth);
    return false;
  }
  BlockSize block{};
  size_t bytesPerPixel = 1;
  if (!FormatBlockSize(head.format, block, bytesPerPixel)) {
    error = "remastered txtr: cannot decode " + std::string(FormatName(head.format));
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }
  // DecodeTxtr's layout, every slice: blocks one GOB tall, the block depth from BlockDepth. DeswizzleMip
  // writes each slice it is given to the same place, so it is called per slice with that slice's offset.
  const size_t mipWidth = DivRoundUp(size_t(width), block.width);
  const size_t mipHeight = DivRoundUp(size_t(height), block.height);
  const size_t mipBlockDepth = MipBlockDepth(depth, BlockDepth(depth));
  const uint32_t mipBlockHeight = MipBlockHeight(mipHeight, 1);
  if (surface.size() < SwizzledMipSize(mipWidth, mipHeight, depth, mipBlockHeight, bytesPerPixel)) {
    error = "remastered txtr: the volume's surface is short";
    return false;
  }
  const size_t rowBytes = mipWidth * bytesPerPixel;
  const size_t sliceSize = DivRoundUp(mipHeight, size_t(mipBlockHeight) * kGobHeightBytes) * kGobSizeBytes *
                           mipBlockHeight * mipBlockDepth * DivRoundUp(rowBytes, kGobWidthBytes);
  std::vector<uint8_t> untiled(mipWidth * mipHeight * bytesPerPixel);
  const size_t sliceBytes = size_t(width) * height * 4;
  rgba.assign(sliceBytes * depth, 0);
  for (size_t z = 0; z < depth; ++z) {
    const size_t offsetZ = (z / mipBlockDepth) * sliceSize + (z & (mipBlockDepth - 1)) * kGobSizeBytes * mipBlockHeight;
    DeswizzleMip(mipWidth, mipHeight, 1, mipBlockHeight, mipBlockDepth, bytesPerPixel, surface.data(), offsetZ,
                 untiled.data());
    if (!DecodeBlocks(head.format, width, height, untiled.data(), rgba.data() + z * sliceBytes, error)) {
      rgba.clear();
      return false;
    }
  }
  return true;
}

bool DecodeTxtrCubeLinear(const uint8_t* data, size_t size, uint32_t& edge, std::vector<float>& rgba,
                          std::string& error) {
  rgba.clear();
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind != 3 || head.layers != 6 || head.width != head.height || head.width == 0 ||
      head.width > 4096 || head.mipSizes.empty()) {
    error = "remastered txtr: not a cube map";
    return false;
  }
  BlockSize block{};
  size_t bytesPerPixel = 1;
  if (!FormatBlockSize(head.format, block, bytesPerPixel)) {
    error = "remastered txtr: cannot decode " + std::string(FormatName(head.format));
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }
  // ReadTxtrCubeBc6h's layout, for any block size: each layer holds its whole mip
  // chain and starts on a multiple of the block of GOBs the top mip uses. Only the
  // top mip of each face is decoded.
  edge = head.width;
  const uint32_t blockHeightMip0 = BlockHeightMip0(DivRoundUp(size_t(head.height), block.height));
  const size_t faceFloats = size_t(edge) * edge * 4;
  rgba.assign(faceFloats * 6, 0.0f);
  const bool bc6h = head.format == kTxtrFormatBc6hUfloat || head.format == kTxtrFormatBc6hSfloat;
  const bool srgb = FormatIsSrgb(head.format);
  std::vector<uint8_t> ldr(bc6h ? 0 : faceFloats);
  std::vector<uint16_t> hdr(bc6h ? faceFloats : 0);
  size_t srcOffset = 0;
  for (uint32_t layer = 0; layer < 6; ++layer) {
    for (uint32_t mip = 0; mip < head.mipSizes.size(); ++mip) {
      const uint32_t texels = std::max(head.width >> mip, 1u);
      const size_t blocksX = DivRoundUp(size_t(texels), block.width);
      const size_t blocksY = DivRoundUp(size_t(texels), block.height);
      const uint32_t mipBlockHeight = MipBlockHeight(blocksY, blockHeightMip0);
      const size_t swizzled = SwizzledMipSize(blocksX, blocksY, 1, mipBlockHeight, bytesPerPixel);
      if (swizzled > surface.size() || srcOffset > surface.size() - swizzled) {
        error = "remastered txtr: the cube's surface is short";
        rgba.clear();
        return false;
      }
      if (mip == 0) {
        std::vector<uint8_t> untiled(blocksX * blocksY * bytesPerPixel);
        DeswizzleMip(blocksX, blocksY, 1, mipBlockHeight, 1, bytesPerPixel, surface.data(), srcOffset,
                     untiled.data());
        float* dst = rgba.data() + faceFloats * layer;
        if (bc6h) {
          DecodeBc6hImage(untiled.data(), edge, edge, head.format == kTxtrFormatBc6hSfloat, hdr.data());
          for (size_t i = 0; i < faceFloats; ++i) {
            dst[i] = HalfBitsToFloat(hdr[i]);
          }
        } else {
          if (!DecodeBlocks(head.format, edge, edge, untiled.data(), ldr.data(), error)) {
            rgba.clear();
            return false;
          }
          for (size_t i = 0; i < faceFloats; ++i) {
            const float c = float(ldr[i]) / 255.0f;
            dst[i] = srgb && i % 4 != 3 ? (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) : c;
          }
        }
      }
      srcOffset += swizzled;
    }
    uint32_t gobHeight = blockHeightMip0;
    while (head.width <= (gobHeight / 2) * 8 && gobHeight > 1) {
      gobHeight /= 2;
    }
    const size_t unit = size_t(gobHeight) * kGobSizeBytes;
    srcOffset = DivRoundUp(srcOffset, unit) * unit;
  }
  return true;
}

bool DecodeTxtrLayersRgba8(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height,
                           uint32_t& layers, std::vector<uint8_t>& rgba, std::string& error) {
  rgba.clear();
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind != 5 || head.layers == 0 || head.layers > 256 || head.width == 0 ||
      head.height == 0 || head.width > 4096 || head.height > 4096 || head.mipSizes.empty()) {
    error = "remastered txtr: not an array texture";
    return false;
  }
  BlockSize block{};
  size_t bytesPerPixel = 1;
  if (!FormatBlockSize(head.format, block, bytesPerPixel)) {
    error = "remastered txtr: cannot decode " + std::string(FormatName(head.format));
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }
  // The cube's layout (see DecodeTxtrCubeLinear) with the layer count and a
  // non-square top mip. Only the top mip of each layer is decoded.
  width = head.width;
  height = head.height;
  layers = head.layers;
  const uint32_t blockHeightMip0 = BlockHeightMip0(DivRoundUp(size_t(height), block.height));
  const size_t layerBytes = size_t(width) * height * 4;
  rgba.assign(layerBytes * layers, 0);
  size_t srcOffset = 0;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t mip = 0; mip < head.mipSizes.size(); ++mip) {
      const size_t blocksX = DivRoundUp(size_t(std::max(width >> mip, 1u)), block.width);
      const size_t blocksY = DivRoundUp(size_t(std::max(height >> mip, 1u)), block.height);
      const uint32_t mipBlockHeight = MipBlockHeight(blocksY, blockHeightMip0);
      const size_t swizzled = SwizzledMipSize(blocksX, blocksY, 1, mipBlockHeight, bytesPerPixel);
      if (swizzled > surface.size() || srcOffset > surface.size() - swizzled) {
        error = "remastered txtr: the array's surface is short";
        rgba.clear();
        return false;
      }
      if (mip == 0) {
        std::vector<uint8_t> untiled(blocksX * blocksY * bytesPerPixel);
        DeswizzleMip(blocksX, blocksY, 1, mipBlockHeight, 1, bytesPerPixel, surface.data(),
                     srcOffset, untiled.data());
        if (!DecodeBlocks(head.format, width, height, untiled.data(),
                          rgba.data() + layerBytes * layer, error)) {
          rgba.clear();
          return false;
        }
      }
      srcOffset += swizzled;
    }
    uint32_t gobHeight = blockHeightMip0;
    while (height <= (gobHeight / 2) * 8 && gobHeight > 1) {
      gobHeight /= 2;
    }
    const size_t unit = size_t(gobHeight) * kGobSizeBytes;
    srcOffset = DivRoundUp(srcOffset, unit) * unit;
  }
  return true;
}

bool ReadTxtrLayersBc6h(const uint8_t* data, size_t size, TxtrLayersBc6h& out, std::string& error) {
  out = TxtrLayersBc6h{};
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind != 5 || head.layers == 0 || head.layers > 256 || head.width == 0 || head.height == 0 ||
      head.width > 8192 || head.height > 8192 || head.mipSizes.empty()) {
    error = "remastered txtr: not an array texture";
    return false;
  }
  if (head.format != kTxtrFormatBc6hUfloat && head.format != kTxtrFormatBc6hSfloat) {
    error = "remastered txtr: the array is " + std::string(FormatName(head.format)) + ", not BC6H";
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }
  // DecodeTxtrLayersRgba8's layout: each layer holds its whole mip chain and starts on a
  // multiple of the block of GOBs the top mip uses. Only mip 0 of each layer is kept.
  const size_t bytesPerBlock = 16;
  const uint32_t width = head.width, height = head.height;
  const uint32_t blockHeightMip0 = BlockHeightMip0(DivRoundUp(size_t(height), 4));
  out.width = width;
  out.height = height;
  out.isSigned = head.format == kTxtrFormatBc6hSfloat;
  out.layers.resize(head.layers);
  size_t srcOffset = 0;
  for (uint32_t layer = 0; layer < head.layers; ++layer) {
    for (uint32_t mip = 0; mip < head.mipSizes.size(); ++mip) {
      const size_t blocksX = DivRoundUp(size_t(std::max(width >> mip, 1u)), 4);
      const size_t blocksY = DivRoundUp(size_t(std::max(height >> mip, 1u)), 4);
      const uint32_t mipBlockHeight = MipBlockHeight(blocksY, blockHeightMip0);
      const size_t swizzled = SwizzledMipSize(blocksX, blocksY, 1, mipBlockHeight, bytesPerBlock);
      if (swizzled > surface.size() || srcOffset > surface.size() - swizzled) {
        error = "remastered txtr: the array's surface is short";
        out = TxtrLayersBc6h{};
        return false;
      }
      if (mip == 0) {
        std::vector<uint8_t>& untiled = out.layers[layer];
        untiled.assign(blocksX * blocksY * bytesPerBlock, 0);
        DeswizzleMip(blocksX, blocksY, 1, mipBlockHeight, 1, bytesPerBlock, surface.data(), srcOffset,
                     untiled.data());
      }
      srcOffset += swizzled;
    }
    uint32_t gobHeight = blockHeightMip0;
    while (height <= (gobHeight / 2) * 8 && gobHeight > 1) {
      gobHeight /= 2;
    }
    const size_t unit = size_t(gobHeight) * kGobSizeBytes;
    srcOffset = DivRoundUp(srcOffset, unit) * unit;
  }
  return true;
}

uint8_t HalfToUnormByte(uint16_t half) {
  const float v = HalfBitsToFloat(half);
  if (!(v > 0.f)) {
    return 0;  // negative, zero or NaN
  }
  return v >= 1.f ? 255 : uint8_t(v * 255.f + 0.5f);
}

void DecodeBc6hBlockRgba8(const uint8_t* block, bool isSigned, uint8_t* rgba) {
  uint16_t half[4 * 4 * 3] = {};
  DecodeBc6h(block, half, isSigned);
  for (size_t i = 0; i < 16; ++i) {
    rgba[i * 4 + 0] = HalfToUnormByte(half[i * 3 + 0]);
    rgba[i * 4 + 1] = HalfToUnormByte(half[i * 3 + 1]);
    rgba[i * 4 + 2] = HalfToUnormByte(half[i * 3 + 2]);
    rgba[i * 4 + 3] = 255;
  }
}

bool ReadTxtrMipsBc6h(const uint8_t* data, size_t size, TxtrMipsBc6h& out, std::string& error) {
  out = TxtrMipsBc6h{};
  if (data == nullptr || size == 0) {
    error = "remastered txtr: no data";
    return false;
  }
  TextureHeader head;
  if (!ReadHeader(data, size, head, error)) {
    return false;
  }
  if (head.kind == 2 || head.kind == 3 || head.kind == 5 || head.layers != 1 || head.width == 0 ||
      head.height == 0 || head.width > 16384 || head.height > 16384 || head.mipSizes.empty()) {
    error = "remastered txtr: not a plain 2D texture";
    return false;
  }
  if (head.format != kTxtrFormatBc6hUfloat && head.format != kTxtrFormatBc6hSfloat) {
    error = "remastered txtr: the texture is " + std::string(FormatName(head.format)) + ", not BC6H";
    return false;
  }
  Meta meta;
  if (!ReadMeta(data, size, meta, error)) {
    return false;
  }
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
    return false;
  }
  // The array's layout with one layer (ReadTxtrLayersBc6h): the mips follow one another,
  // each tiled with the block height its own size allows. The chain stops at 1x1.
  uint32_t mipCount = 0;
  while (mipCount < head.mipSizes.size() && (std::max(head.width, head.height) >> mipCount) != 0) {
    ++mipCount;
  }
  const size_t bytesPerBlock = 16;
  const uint32_t blockHeightMip0 = BlockHeightMip0(DivRoundUp(size_t(head.height), 4));
  out.width = head.width;
  out.height = head.height;
  out.isSigned = head.format == kTxtrFormatBc6hSfloat;
  out.mips.resize(mipCount);
  size_t srcOffset = 0;
  for (uint32_t mip = 0; mip < mipCount; ++mip) {
    const size_t blocksX = DivRoundUp(size_t(std::max(head.width >> mip, 1u)), 4);
    const size_t blocksY = DivRoundUp(size_t(std::max(head.height >> mip, 1u)), 4);
    const uint32_t mipBlockHeight = MipBlockHeight(blocksY, blockHeightMip0);
    const size_t swizzled = SwizzledMipSize(blocksX, blocksY, 1, mipBlockHeight, bytesPerBlock);
    if (swizzled > surface.size() || srcOffset > surface.size() - swizzled) {
      error = "remastered txtr: the texture's surface is short";
      out = TxtrMipsBc6h{};
      return false;
    }
    out.mips[mip].assign(blocksX * blocksY * bytesPerBlock, 0);
    DeswizzleMip(blocksX, blocksY, 1, mipBlockHeight, 1, bytesPerBlock, surface.data(), srcOffset,
                 out.mips[mip].data());
    srcOffset += swizzled;
  }
  return true;
}

void DecodeBc6hImage(const uint8_t* blocks, uint32_t width, uint32_t height, bool isSigned, uint16_t* rgba) {
  const size_t perRow = DivRoundUp(size_t(width), 4), rows = DivRoundUp(size_t(height), 4);
  for (size_t by = 0; by < rows; ++by) {
    for (size_t bx = 0; bx < perRow; ++bx) {
      uint16_t half[4 * 4 * 3] = {};
      DecodeBc6h(blocks + (by * perRow + bx) * 16, half, isSigned);
      for (size_t y = 0; y < 4 && by * 4 + y < height; ++y) {
        for (size_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
          uint16_t* p = rgba + ((by * 4 + y) * width + bx * 4 + x) * 4;
          p[0] = half[(y * 4 + x) * 3 + 0];
          p[1] = half[(y * 4 + x) * 3 + 1];
          p[2] = half[(y * 4 + x) * 3 + 2];
          p[3] = 0x3C00;
        }
      }
    }
  }
}

void DecodeBc6hFace(const uint8_t* blocks, uint32_t texels, bool isSigned, uint16_t* rgba) {
  const size_t perSide = DivRoundUp(size_t(texels), 4);
  for (size_t by = 0; by < perSide; ++by) {
    for (size_t bx = 0; bx < perSide; ++bx) {
      uint16_t half[4 * 4 * 3] = {};
      DecodeBc6h(blocks + (by * perSide + bx) * 16, half, isSigned);
      for (size_t y = 0; y < 4 && by * 4 + y < texels; ++y) {
        for (size_t x = 0; x < 4 && bx * 4 + x < texels; ++x) {
          uint16_t* p = rgba + ((by * 4 + y) * texels + bx * 4 + x) * 4;
          p[0] = half[(y * 4 + x) * 3 + 0];
          p[1] = half[(y * 4 + x) * 3 + 1];
          p[2] = half[(y * 4 + x) * 3 + 2];
          p[3] = 0x3C00;  // 1.0
        }
      }
    }
  }
}

}  // namespace PortRemastered
