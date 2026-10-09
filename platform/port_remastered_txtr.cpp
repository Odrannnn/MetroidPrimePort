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
//   * BC6H has no 8 bit form, so its half precision channels are reduced with
//     the usual 16 to 8 bit rule. retrotool cannot write BC6H at all (its PNG
//     writer has no 32 bit float case), which is why nothing checks this one
//     against it.

#include "port_remastered_txtr.h"
#include "port_bytes.h"

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

// retrotool's LZSS (util/lzss.rs), one variant per mode: mode is the log2 of
// the group's byte count, a clear header bit copies a group verbatim and a set
// bit copies a back reference. This is deliberately a separate copy of the one
// in port_remastered_pak.cpp: that file is not to be edited, and until its
// `DecompressAsset` is exposed this is the only way the texel data can be
// decompressed. It is a straight port of the Rust, including its behaviour of
// trusting the data, because retrotool does too.
bool LzssDecompress(uint32_t mode, const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
  const size_t groupLen = size_t(1) << (mode - 1);
  size_t inCur = 0;
  size_t outCur = 0;
  uint8_t header = 0;
  int group = 0;
  while (inCur < inSize) {
    if (group == 0) {
      header = in[inCur++];
      group = 8;
    }
    if ((header & 0x80) == 0) {
      if (inSize - inCur < groupLen || outSize - outCur < groupLen) {
        return false;
      }
      std::memcpy(out + outCur, in + inCur, groupLen);
      inCur += groupLen;
      outCur += groupLen;
    } else {
      if (inSize - inCur < 2 || outCur == 0) {
        return false;
      }
      const size_t count = size_t(in[inCur] >> 4) + (4 - size_t(mode));
      const size_t length = (size_t(in[inCur] & 0x0F) * 256 + size_t(in[inCur + 1])) << (mode - 1);
      inCur += 2;
      if (length > outCur || outSize - outCur < count * groupLen) {
        return false;
      }
      // One byte at a time: a back reference may overlap what it is writing,
      // which is how a run of the same byte is encoded.
      for (size_t i = 0; i < count * groupLen; ++i) {
        out[outCur + i] = out[outCur - length + i];
      }
      outCur += count * groupLen;
    }
    header = uint8_t(header << 1);
    group -= 1;
  }
  return outCur == outSize;
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

// Decompresses every buffer of the META into one buffer, which is the surface
// as it is stored in memory: tiled, and holding every layer and mip.
bool BuildSurface(const uint8_t* file, size_t fileSize, const Meta& meta,
                  std::vector<uint8_t>& surface, std::string& error) {
  if (meta.decompressedSize > 0x40000000u) {
    error = "remastered txtr: META asks for a " + std::to_string(meta.decompressedSize) +
            " byte surface";
    return false;
  }
  surface.assign(meta.decompressedSize, 0);
  for (const Meta::Buffer& buffer : meta.buffers) {
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
// tegra_swizzle 0.3.2: block linear untiling
// ---------------------------------------------------------------------------

constexpr size_t kGobWidthBytes = 64;
constexpr size_t kGobHeightBytes = 8;
constexpr size_t kGobSizeBytes = 64 * 8;

// The block heights a Tegra surface can use, in GOBs (8 rows each).
uint32_t BlockHeightMip0(size_t heightInBlocks) {
  const size_t heightAndHalf = heightInBlocks + heightInBlocks / 2;
  if (heightAndHalf >= 128) {
    return 16;
  }
  if (heightAndHalf >= 64) {
    return 8;
  }
  if (heightAndHalf >= 32) {
    return 4;
  }
  if (heightAndHalf >= 16) {
    return 2;
  }
  return 1;
}

uint32_t MipBlockHeight(size_t mipHeight, uint32_t blockHeightMip0) {
  uint32_t blockHeight = blockHeightMip0;
  while (mipHeight <= (blockHeight / 2) * 8 && blockHeight > 1) {
    blockHeight /= 2;
  }
  return blockHeight;
}

// Blocks are always one GOB wide, so the depth in blocks is the whole story.
size_t BlockDepth(size_t depth) {
  const size_t depthAndHalf = depth + depth / 2;
  if (depthAndHalf >= 16) {
    return 16;
  }
  if (depthAndHalf >= 8) {
    return 8;
  }
  if (depthAndHalf >= 4) {
    return 4;
  }
  if (depthAndHalf >= 2) {
    return 2;
  }
  return 1;
}

size_t MipBlockDepth(size_t mipDepth, size_t blockDepthMip0) {
  size_t blockDepth = blockDepthMip0;
  while (mipDepth <= blockDepth / 2 && blockDepth > 1) {
    blockDepth /= 2;
  }
  return blockDepth;
}

size_t DivRoundUp(size_t x, size_t d) { return (x + d - 1) / d; }

// The tiled size of one mip, which is padded up to whole GOBs. A block is one
// GOB wide, so the width in GOBs follows from the bytes per pixel.
size_t SwizzledMipSize(size_t width, size_t height, size_t depth, uint32_t blockHeight,
                       size_t bytesPerPixel) {
  const size_t widthInGobs = DivRoundUp(width * bytesPerPixel, kGobWidthBytes);
  const size_t heightInBlocks = DivRoundUp(height, size_t(blockHeight) * kGobHeightBytes);
  const size_t heightInGobs = heightInBlocks * size_t(blockHeight);
  const size_t depthUnit = BlockDepth(depth);
  const size_t depthInGobs = DivRoundUp(depth, depthUnit) * depthUnit;
  return widthInGobs * heightInGobs * depthInGobs * kGobSizeBytes;
}

// Offset of the byte at (x, y) inside one GOB, from the Tegra TRM p. 1218.
size_t GobOffset(size_t x, size_t y) {
  return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 +
         (x % 16);
}

// The row offsets of a GOB's eight 16 byte rows, which is what makes the whole
// GOB contiguous: 0, 16, 64, 80, 128, 144, 192, 208.
constexpr size_t kGobRowOffsets[8] = {0, 16, 64, 80, 128, 144, 192, 208};

// Untiles one mip into `dst`, which must hold width * height * depth * bpp
// bytes, reading from `src` at `srcOffset`. This is swizzle.rs's
// swizzle_inner<true> with only the untiling direction kept.
void DeswizzleMip(size_t width, size_t height, size_t depth, uint32_t blockHeight,
                  size_t blockDepth, size_t bytesPerPixel, const uint8_t* src, size_t srcOffset,
                  uint8_t* dst) {
  const size_t rowBytes = width * bytesPerPixel;
  const size_t widthInGobs = DivRoundUp(rowBytes, kGobWidthBytes);
  const size_t blockHeightInBytes = kGobHeightBytes * blockHeight;
  const size_t blockSizeInBytes = kGobSizeBytes * blockHeight * blockDepth;
  // Rows are grouped into blocks of `blockHeight` GOBs, and blocks into slices.
  const size_t sliceSize =
      DivRoundUp(height, blockHeight * kGobHeightBytes) * kGobSizeBytes * blockHeight *
      blockDepth * widthInGobs;

  for (size_t z = 0; z < depth; ++z) {
    const size_t offsetZ =
        (z / blockDepth) * sliceSize + (z & (blockDepth - 1)) * kGobSizeBytes * blockHeight;
    for (size_t y0 = 0; y0 < height; y0 += kGobHeightBytes) {
      const size_t blockY = y0 / blockHeightInBytes;
      const size_t blockInnerRow = (y0 % blockHeightInBytes) / kGobHeightBytes;
      const size_t offsetY =
          blockY * blockSizeInBytes * widthInGobs + blockInnerRow * kGobSizeBytes;
      for (size_t x0 = 0; x0 < rowBytes; x0 += kGobWidthBytes) {
        const size_t offsetX = (x0 / kGobWidthBytes) * blockSizeInBytes;
        const size_t gobAddress = srcOffset + offsetZ + offsetY + offsetX;
        if (x0 + kGobWidthBytes < rowBytes && y0 + kGobHeightBytes < height) {
          // A whole GOB: copy its eight rows, each sixteen bytes at a time.
          for (size_t row = 0; row < kGobHeightBytes; ++row) {
            uint8_t* out = dst + (y0 + row) * rowBytes + x0;
            const uint8_t* in = src + gobAddress + kGobRowOffsets[row];
            for (size_t chunk = 0; chunk < 4; ++chunk) {
              // The eight 16 byte pieces of a row, in the order the GOB holds
              // them: halves, then quarters, then the two eighths.
              static const size_t kDstOffsets[4] = {0, 16, 32, 48};
              static const size_t kSrcOffsets[4] = {0, 32, 256, 288};
              std::memcpy(out + kDstOffsets[chunk], in + kSrcOffsets[chunk], 16);
            }
          }
        } else {
          // A partly filled GOB along the right or bottom edge, byte by byte.
          for (size_t y = 0; y < kGobHeightBytes; ++y) {
            for (size_t x = 0; x < kGobWidthBytes; ++x) {
              if (y0 + y < height && x0 + x < rowBytes) {
                dst[(y0 + y) * rowBytes + x0 + x] = src[gobAddress + GobOffset(x, y)];
              }
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// bcdec_rs 0.1.2: the BC block decoders
// ---------------------------------------------------------------------------

// A 128 bit value as a pair of halves, because MSVC has no __int128 and this
// file has to build there.
struct U128 {
  uint64_t lo = 0;
  uint64_t hi = 0;
};

// bcdec's Bitstream: a little endian bit reader over the block's 16 bytes.
struct BitStream {
  uint64_t low = 0;
  uint64_t high = 0;

  uint32_t ReadBits(uint32_t numBits) {
    if (numBits == 0) {
      return 0;
    }
    const uint64_t mask = numBits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << numBits) - 1);
    const uint32_t bits = uint32_t(low & mask);
    low >>= numBits;
    low |= (high & mask) << (64 - numBits);
    high >>= numBits;
    return bits;
  }

  uint32_t ReadBit() { return ReadBits(1); }

  // BC6H reads a few fields with their bits reversed, as the format defines them.
  int32_t ReadBitsReversed(uint32_t numBits) {
    int32_t bits = int32_t(ReadBits(numBits));
    int32_t result = 0;
    for (uint32_t i = 0; i < numBits; ++i) {
      result <<= 1;
      result |= bits & 1;
      bits >>= 1;
    }
    return result;
  }
};

// The colour half of BC1, BC2 and BC3, 8 bytes to sixteen RGBA texels.
// `onlyOpaqueMode` forces the four colour mode of BC1 even when the endpoints
// are in the wrong order, which BC2 and BC3 do because their alpha is stored
// separately, and which BC1 uses only when they are not.
void DecodeColorBlock(const uint8_t* src, uint8_t* dst, bool onlyOpaqueMode) {
  uint8_t refColors[4][4];
  const uint32_t c0 = uint32_t(src[0]) | (uint32_t(src[1]) << 8);
  const uint32_t c1 = uint32_t(src[2]) | (uint32_t(src[3]) << 8);
  const uint32_t r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  const uint32_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  refColors[0][0] = uint8_t((r0 * 527 + 23) >> 6);
  refColors[0][1] = uint8_t((g0 * 259 + 33) >> 6);
  refColors[0][2] = uint8_t((b0 * 527 + 23) >> 6);
  refColors[0][3] = 255;
  refColors[1][0] = uint8_t((r1 * 527 + 23) >> 6);
  refColors[1][1] = uint8_t((g1 * 259 + 33) >> 6);
  refColors[1][2] = uint8_t((b1 * 527 + 23) >> 6);
  refColors[1][3] = 255;
  if (c0 > c1 || onlyOpaqueMode) {
    refColors[2][0] = uint8_t(((2 * r0 + r1) * 351 + 61) >> 7);
    refColors[2][1] = uint8_t(((2 * g0 + g1) * 2763 + 1039) >> 11);
    refColors[2][2] = uint8_t(((2 * b0 + b1) * 351 + 61) >> 7);
    refColors[2][3] = 255;
    refColors[3][0] = uint8_t(((r0 + r1 * 2) * 351 + 61) >> 7);
    refColors[3][1] = uint8_t(((g0 + g1 * 2) * 2763 + 1039) >> 11);
    refColors[3][2] = uint8_t(((b0 + b1 * 2) * 351 + 61) >> 7);
    refColors[3][3] = 255;
  } else {
    refColors[2][0] = uint8_t(((r0 + r1) * 1053 + 125) >> 8);
    refColors[2][1] = uint8_t(((g0 + g1) * 4145 + 1019) >> 11);
    refColors[2][2] = uint8_t(((b0 + b1) * 1053 + 125) >> 8);
    refColors[2][3] = 255;
    refColors[3][0] = 0;
    refColors[3][1] = 0;
    refColors[3][2] = 0;
    refColors[3][3] = 0;
  }
  uint32_t indices = uint32_t(src[4]) | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) |
                     (uint32_t(src[7]) << 24);
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      std::memcpy(dst + (i * 4 + j) * 4, refColors[indices & 3], 4);
      indices >>= 2;
    }
  }
}

// BC2's alpha: four bits per texel, scaled to eight by 17.
void DecodeSharpAlpha(const uint8_t* src, uint8_t* dst) {
  for (size_t i = 0; i < 4; ++i) {
    const uint32_t alpha = uint32_t(src[i * 2]) | (uint32_t(src[i * 2 + 1]) << 8);
    for (size_t j = 0; j < 4; ++j) {
      dst[(i * 4 + j) * 4 + 3] = uint8_t(((alpha >> (4 * j)) & 0x0F) * 17);
    }
  }
}

// BC3 and BC4's alpha: two endpoints and six (or five) interpolated values,
// written into one channel every `pixelSize` bytes.
void DecodeSmoothAlpha(const uint8_t* src, uint8_t* dst, size_t pixelSize, size_t channel) {
  uint32_t alpha[8];
  alpha[0] = src[0];
  alpha[1] = src[1];
  if (alpha[0] > alpha[1]) {
    for (uint32_t i = 2; i < 8; ++i) {
      alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1] + 1) / 7;
    }
  } else {
    for (uint32_t i = 2; i < 6; ++i) {
      alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1] + 1) / 5;
    }
    alpha[6] = 0x00;
    alpha[7] = 0xFF;
  }
  uint64_t indices = 0;
  for (size_t i = 2; i < 8; ++i) {
    indices |= uint64_t(src[i]) << ((i - 2) * 8);
  }
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      dst[(i * 4 + j) * pixelSize + channel] = uint8_t(alpha[indices & 7]);
      indices >>= 3;
    }
  }
}

// BC6H's 32 partition sets, as one byte per texel. The high bit marks the two
// fix-up indices, whose index field is one bit shorter than the rest.
const uint8_t kBc6hPartitionSets[32][4][4] = {
    {{128, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129}},
    {{128, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 129}},
    {{128, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 1, 129}},
    {{128, 0, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 129}},
    {{128, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 129}},
    {{128, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 129}},
    {{128, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1, 0}, {1, 1, 1, 129}},
    {{128, 1, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 1, 0}},
    {{128, 1, 129, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
    {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 1, 0, 0}, {1, 1, 1, 0}},
    {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
    {{128, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 129}},
    {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
    {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
    {{128, 1, 129, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}},
    {{128, 0, 129, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}},
    {{128, 0, 0, 1}, {0, 1, 1, 1}, {129, 1, 1, 0}, {1, 0, 0, 0}},
    {{128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {0, 0, 0, 0}},
    {{128, 1, 129, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}},
    {{128, 0, 129, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}},
};

const uint8_t kBc6hActualBits[4][14] = {
    {10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16},  // W
    {5, 6, 5, 4, 4, 5, 6, 5, 5, 6, 10, 9, 8, 4},         // dR
    {5, 6, 4, 5, 4, 5, 5, 6, 5, 6, 10, 9, 8, 4},         // dG
    {5, 6, 4, 4, 5, 5, 5, 5, 6, 6, 10, 9, 8, 4},         // dB
};

int32_t ExtendSign(int32_t value, int32_t bits) { return (value << (32 - bits)) >> (32 - bits); }

int32_t TransformInverse(int32_t value, int32_t base, int32_t bits, bool isSigned) {
  int32_t result = (value + base) & ((1 << bits) - 1);
  if (isSigned) {
    result = ExtendSign(result, bits);
  }
  return result;
}

int32_t Unquantize(int32_t value, int32_t bits, bool isSigned) {
  if (!isSigned) {
    if (bits >= 15) {
      return value;
    }
    if (value == 0) {
      return 0;
    }
    if (value == (1 << bits) - 1) {
      return 0xFFFF;
    }
    return ((value << 16) + 0x8000) >> bits;
  }
  // The signed form is the magnitude unquantized to a 16 bit half float and
  // then negated, with the largest magnitude saturated.
  int32_t sign = 0;
  int32_t magnitude = value;
  if (bits < 16 && magnitude < 0) {
    sign = 1;
    magnitude = -magnitude;
  }
  int32_t result;
  if (magnitude == 0) {
    result = 0;
  } else if (magnitude >= ((1 << (bits - 1)) - 1)) {
    result = 0x7FFF;
  } else {
    result = ((magnitude << 15) + 0x4000) >> (bits - 1);
  }
  return sign != 0 ? -result : result;
}

int32_t InterpolateInt(int32_t a, int32_t b, int32_t weight) {
  return (a * (64 - weight) + b * weight + 32) >> 6;
}

uint16_t FinishUnquantize(int32_t value, bool isSigned) {
  if (!isSigned) {
    return uint16_t((value * 31) >> 6);
  }
  int32_t scaled = value < 0 ? -((-value * 31) >> 5) : ((value * 31) >> 5);
  uint32_t sign = 0;
  if (scaled < 0) {
    sign = 0x8000;
    scaled = -scaled;
  }
  return uint16_t(sign | uint32_t(scaled));
}

// BC6H: 16 bytes to sixteen RGB texels of half precision. The 8 bit image
// reduces them; the HDR cube reader keeps them as they are. Every mode stores its endpoints in its own bit layout, so
// the mode decode is mostly a long list of bit field reads in a fixed order.
void DecodeBc6h(const uint8_t* src, uint16_t* dst, bool isSigned) {
  const uint32_t weight3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
  const uint32_t weight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

  BitStream stream;
  for (size_t i = 0; i < 8; ++i) {
    stream.low |= uint64_t(src[i]) << (8 * i);
    stream.high |= uint64_t(src[i + 8]) << (8 * i);
  }

  // The endpoints of each of the four components, held as w, x, y, z.
  int32_t r[4] = {0, 0, 0, 0};
  int32_t g[4] = {0, 0, 0, 0};
  int32_t b[4] = {0, 0, 0, 0};

  uint32_t mode = stream.ReadBits(2);
  if (mode > 1) {
    mode |= stream.ReadBits(3) << 2;
  }
  uint32_t partition = 0;

  switch (mode) {
    case 0b00:
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 0;
      break;
    case 0b01:
      g[2] |= int32_t(stream.ReadBit()) << 5;
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[3] |= int32_t(stream.ReadBit()) << 5;
      r[0] |= int32_t(stream.ReadBits(7));
      b[3] |= int32_t(stream.ReadBit());
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(7));
      b[2] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 2;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(7));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      b[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(6));
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(6));
      r[3] |= int32_t(stream.ReadBits(6));
      partition = stream.ReadBits(5);
      mode = 1;
      break;
    case 0b00010:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(5));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(4));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(4));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 2;
      break;
    case 0b00110:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(4));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(4));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(4));
      b[3] |= int32_t(stream.ReadBit());
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(4));
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 3;
      break;
    case 0b01010:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(4));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(4));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(4));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(4));
      b[3] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 4;
      break;
    case 0b01110:
      r[0] |= int32_t(stream.ReadBits(9));
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(9));
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(9));
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 5;
      break;
    case 0b10010:
      r[0] |= int32_t(stream.ReadBits(8));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(6));
      r[3] |= int32_t(stream.ReadBits(6));
      partition = stream.ReadBits(5);
      mode = 6;
      break;
    case 0b10110:
      r[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit());
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(8));
      g[2] |= int32_t(stream.ReadBit()) << 5;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(8));
      g[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 7;
      break;
    case 0b11010:
      r[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(8));
      b[2] |= int32_t(stream.ReadBit()) << 5;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(8));
      b[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(5));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit());
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(6));
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 2;
      r[3] |= int32_t(stream.ReadBits(5));
      b[3] |= int32_t(stream.ReadBit()) << 3;
      partition = stream.ReadBits(5);
      mode = 8;
      break;
    case 0b11110:
      r[0] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBit()) << 4;
      b[3] |= int32_t(stream.ReadBit());
      b[3] |= int32_t(stream.ReadBit()) << 1;
      b[2] |= int32_t(stream.ReadBit()) << 4;
      g[0] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBit()) << 5;
      b[2] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 2;
      g[2] |= int32_t(stream.ReadBit()) << 4;
      b[0] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 3;
      b[3] |= int32_t(stream.ReadBit()) << 5;
      b[3] |= int32_t(stream.ReadBit()) << 4;
      r[1] |= int32_t(stream.ReadBits(6));
      g[2] |= int32_t(stream.ReadBits(4));
      g[1] |= int32_t(stream.ReadBits(6));
      g[3] |= int32_t(stream.ReadBits(4));
      b[1] |= int32_t(stream.ReadBits(6));
      b[2] |= int32_t(stream.ReadBits(4));
      r[2] |= int32_t(stream.ReadBits(6));
      r[3] |= int32_t(stream.ReadBits(6));
      partition = stream.ReadBits(5);
      mode = 9;
      break;
    case 0b00011:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(10));
      g[1] |= int32_t(stream.ReadBits(10));
      b[1] |= int32_t(stream.ReadBits(10));
      mode = 10;
      break;
    case 0b00111:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(9));
      r[0] |= int32_t(stream.ReadBit()) << 10;
      g[1] |= int32_t(stream.ReadBits(9));
      g[0] |= int32_t(stream.ReadBit()) << 10;
      b[1] |= int32_t(stream.ReadBits(9));
      b[0] |= int32_t(stream.ReadBit()) << 10;
      mode = 11;
      break;
    case 0b01011:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(8));
      r[0] |= stream.ReadBitsReversed(2) << 10;
      g[1] |= int32_t(stream.ReadBits(8));
      g[0] |= stream.ReadBitsReversed(2) << 10;
      b[1] |= int32_t(stream.ReadBits(8));
      b[0] |= stream.ReadBitsReversed(2) << 10;
      mode = 12;
      break;
    case 0b01111:
      r[0] |= int32_t(stream.ReadBits(10));
      g[0] |= int32_t(stream.ReadBits(10));
      b[0] |= int32_t(stream.ReadBits(10));
      r[1] |= int32_t(stream.ReadBits(4));
      r[0] |= stream.ReadBitsReversed(6) << 10;
      g[1] |= int32_t(stream.ReadBits(4));
      g[0] |= stream.ReadBitsReversed(6) << 10;
      b[1] |= int32_t(stream.ReadBits(4));
      b[0] |= stream.ReadBitsReversed(6) << 10;
      mode = 13;
      break;
    default:
      // A reserved mode. The format says such a block must come out black.
      for (size_t i = 0; i < 16; ++i) {
        dst[i * 3 + 0] = 0;
        dst[i * 3 + 1] = 0;
        dst[i * 3 + 2] = 0;
      }
      return;
  }

  const uint32_t numPartitions = mode >= 10 ? 0 : 1;
  const int32_t actualBits0 = kBc6hActualBits[0][mode];
  if (isSigned) {
    r[0] = ExtendSign(r[0], actualBits0);
    g[0] = ExtendSign(g[0], actualBits0);
    b[0] = ExtendSign(b[0], actualBits0);
  }
  // Modes 11 and 12 (the last two of the fourteen) store both endpoints, and
  // mode 10 stores them too, so only the delta ones transform and sign extend.
  const bool hasDelta = mode != 9 && mode != 10;
  if (hasDelta || isSigned) {
    for (uint32_t i = 1; i < (numPartitions + 1) * 2; ++i) {
      r[i] = ExtendSign(r[i], kBc6hActualBits[1][mode]);
      g[i] = ExtendSign(g[i], kBc6hActualBits[2][mode]);
      b[i] = ExtendSign(b[i], kBc6hActualBits[3][mode]);
    }
  }
  if (hasDelta) {
    for (uint32_t i = 1; i < (numPartitions + 1) * 2; ++i) {
      r[i] = TransformInverse(r[i], r[0], actualBits0, isSigned);
      g[i] = TransformInverse(g[i], g[0], actualBits0, isSigned);
      b[i] = TransformInverse(b[i], b[0], actualBits0, isSigned);
    }
  }
  for (uint32_t i = 0; i < (numPartitions + 1) * 2; ++i) {
    r[i] = Unquantize(r[i], actualBits0, isSigned);
    g[i] = Unquantize(g[i], actualBits0, isSigned);
    b[i] = Unquantize(b[i], actualBits0, isSigned);
  }

  const uint32_t* weights = mode >= 10 ? weight4 : weight3;
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      uint32_t set = mode >= 10 ? ((i | j) != 0 ? 0u : 128u)
                                : uint32_t(kBc6hPartitionSets[partition][i][j]);
      uint32_t indexBits = mode >= 10 ? 4 : 3;
      // A fix-up index is stored with one bit fewer; the first one is always 0.
      if ((set & 0x80) != 0) {
        indexBits -= 1;
      }
      set &= 1;
      const uint32_t weight = weights[stream.ReadBits(indexBits)];
      const uint32_t endpoint = set * 2;
      dst[(i * 4 + j) * 3 + 0] =
          (FinishUnquantize(InterpolateInt(r[endpoint], r[endpoint + 1], int32_t(weight)),
                                   isSigned));
      dst[(i * 4 + j) * 3 + 1] =
          (FinishUnquantize(InterpolateInt(g[endpoint], g[endpoint + 1], int32_t(weight)),
                                   isSigned));
      dst[(i * 4 + j) * 3 + 2] =
          (FinishUnquantize(InterpolateInt(b[endpoint], b[endpoint + 1], int32_t(weight)),
                                   isSigned));
    }
  }
}

const uint8_t kBc7ActualBits[2][8] = {
    {4, 6, 5, 7, 5, 7, 7, 5},  // RGB
    {0, 0, 0, 0, 6, 8, 7, 5},  // alpha
};

// BC7's 64 partition sets for two subsets, then its 64 for three. As in BC6H
// the high bit marks a fix-up index.
const uint8_t kBc7PartitionSets[2][64][4][4] = {
    {
        {{128, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129}},
        {{128, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 129}},
        {{128, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 129}},
        {{128, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1, 0}, {1, 1, 1, 129}},
        {{128, 1, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 1, 0}},
        {{128, 1, 129, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
        {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 1, 0, 0}, {1, 1, 1, 0}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
        {{128, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 0, 129}},
        {{128, 0, 129, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {1, 0, 0, 0}, {129, 0, 0, 0}, {1, 1, 0, 0}},
        {{128, 1, 129, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}},
        {{128, 0, 129, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}},
        {{128, 0, 0, 1}, {0, 1, 1, 1}, {129, 1, 1, 0}, {1, 0, 0, 0}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {0, 0, 0, 0}},
        {{128, 1, 129, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}},
        {{128, 0, 129, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}},
        {{128, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 129}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1, 129}},
        {{128, 1, 0, 1}, {1, 0, 129, 0}, {0, 1, 0, 1}, {1, 0, 1, 0}},
        {{128, 0, 1, 1}, {0, 0, 1, 1}, {129, 1, 0, 0}, {1, 1, 0, 0}},
        {{128, 0, 129, 1}, {1, 1, 0, 0}, {0, 0, 1, 1}, {1, 1, 0, 0}},
        {{128, 1, 0, 1}, {0, 1, 0, 1}, {129, 0, 1, 0}, {1, 0, 1, 0}},
        {{128, 1, 1, 0}, {1, 0, 0, 1}, {0, 1, 1, 0}, {1, 0, 0, 129}},
        {{128, 1, 0, 1}, {1, 0, 1, 0}, {1, 0, 1, 0}, {0, 1, 0, 129}},
        {{128, 1, 129, 1}, {0, 0, 1, 1}, {1, 1, 0, 0}, {1, 1, 1, 0}},
        {{128, 0, 0, 1}, {0, 0, 1, 1}, {129, 1, 0, 0}, {1, 0, 0, 0}},
        {{128, 0, 129, 1}, {0, 0, 1, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}},
        {{128, 0, 129, 1}, {1, 0, 1, 1}, {1, 1, 0, 1}, {1, 1, 0, 0}},
        {{128, 1, 129, 0}, {1, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 1, 0}},
        {{128, 0, 1, 1}, {1, 1, 0, 0}, {1, 1, 0, 0}, {0, 0, 1, 129}},
        {{128, 1, 1, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {1, 0, 0, 129}},
        {{128, 0, 0, 0}, {0, 1, 129, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}},
        {{128, 1, 0, 0}, {1, 1, 129, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}},
        {{128, 0, 129, 0}, {0, 1, 1, 1}, {0, 0, 1, 0}, {0, 0, 0, 0}},
        {{128, 0, 0, 0}, {0, 0, 129, 0}, {0, 1, 1, 1}, {0, 0, 1, 0}},
        {{128, 0, 0, 0}, {0, 1, 0, 0}, {129, 1, 1, 0}, {0, 1, 0, 0}},
        {{128, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 1}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {0, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 129}},
        {{128, 1, 129, 0}, {0, 0, 1, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}},
        {{128, 0, 129, 1}, {1, 0, 0, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}},
        {{128, 1, 1, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 129}},
        {{128, 1, 1, 0}, {0, 0, 1, 1}, {0, 0, 1, 1}, {1, 0, 0, 129}},
        {{128, 1, 1, 1}, {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 129}},
        {{128, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 1, 0}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 129}},
        {{128, 0, 129, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}},
        {{128, 0, 129, 0}, {0, 0, 1, 0}, {1, 1, 1, 0}, {1, 1, 1, 0}},
        {{128, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 1}, {0, 1, 1, 129}},
    },
    {
        {{128, 0, 1, 129}, {0, 0, 1, 1}, {0, 2, 2, 1}, {2, 2, 2, 130}},
        {{128, 0, 0, 129}, {0, 0, 1, 1}, {130, 2, 1, 1}, {2, 2, 2, 1}},
        {{128, 0, 0, 0}, {2, 0, 0, 1}, {130, 2, 1, 1}, {2, 2, 1, 129}},
        {{128, 2, 2, 130}, {0, 0, 2, 2}, {0, 0, 1, 1}, {0, 1, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 1, 2, 2}, {1, 1, 2, 130}},
        {{128, 0, 1, 129}, {0, 0, 1, 1}, {0, 0, 2, 2}, {0, 0, 2, 130}},
        {{128, 0, 2, 130}, {0, 0, 2, 2}, {1, 1, 1, 1}, {1, 1, 1, 129}},
        {{128, 0, 1, 1}, {0, 0, 1, 1}, {130, 2, 1, 1}, {2, 2, 1, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {129, 1, 1, 1}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {1, 1, 1, 1}, {129, 1, 1, 1}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {1, 1, 129, 1}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 0, 1, 2}, {0, 0, 129, 2}, {0, 0, 1, 2}, {0, 0, 1, 130}},
        {{128, 1, 1, 2}, {0, 1, 129, 2}, {0, 1, 1, 2}, {0, 1, 1, 130}},
        {{128, 1, 2, 2}, {0, 129, 2, 2}, {0, 1, 2, 2}, {0, 1, 2, 130}},
        {{128, 0, 1, 129}, {0, 1, 1, 2}, {1, 1, 2, 2}, {1, 2, 2, 130}},
        {{128, 0, 1, 129}, {2, 0, 0, 1}, {130, 2, 0, 0}, {2, 2, 2, 0}},
        {{128, 0, 0, 129}, {0, 0, 1, 1}, {0, 1, 1, 2}, {1, 1, 2, 130}},
        {{128, 1, 1, 129}, {0, 0, 1, 1}, {130, 0, 0, 1}, {2, 2, 0, 0}},
        {{128, 0, 0, 0}, {1, 1, 2, 2}, {129, 1, 2, 2}, {1, 1, 2, 130}},
        {{128, 0, 2, 130}, {0, 0, 2, 2}, {0, 0, 2, 2}, {1, 1, 1, 129}},
        {{128, 1, 1, 129}, {0, 1, 1, 1}, {0, 2, 2, 2}, {0, 2, 2, 130}},
        {{128, 0, 0, 129}, {0, 0, 0, 1}, {130, 2, 2, 1}, {2, 2, 2, 1}},
        {{128, 0, 0, 0}, {0, 0, 129, 1}, {0, 1, 2, 2}, {0, 1, 2, 130}},
        {{128, 0, 0, 0}, {1, 1, 0, 0}, {130, 2, 129, 0}, {2, 2, 1, 0}},
        {{128, 1, 2, 130}, {0, 129, 2, 2}, {0, 0, 1, 1}, {0, 0, 0, 0}},
        {{128, 0, 1, 2}, {0, 0, 1, 2}, {129, 1, 2, 2}, {2, 2, 2, 130}},
        {{128, 1, 1, 0}, {1, 2, 130, 1}, {129, 2, 2, 1}, {0, 1, 1, 0}},
        {{128, 0, 0, 0}, {0, 1, 129, 0}, {1, 2, 130, 1}, {1, 2, 2, 1}},
        {{128, 0, 2, 2}, {1, 1, 0, 2}, {129, 1, 0, 2}, {0, 0, 2, 130}},
        {{128, 1, 1, 0}, {0, 129, 1, 0}, {2, 0, 0, 2}, {2, 2, 2, 130}},
        {{128, 0, 1, 1}, {0, 1, 2, 2}, {0, 1, 130, 2}, {0, 0, 1, 129}},
        {{128, 0, 0, 0}, {2, 0, 0, 0}, {130, 2, 1, 1}, {2, 2, 2, 129}},
        {{128, 0, 0, 0}, {0, 0, 0, 2}, {129, 1, 2, 2}, {1, 2, 2, 130}},
        {{128, 2, 2, 130}, {0, 0, 2, 2}, {0, 0, 1, 2}, {0, 0, 1, 129}},
        {{128, 0, 1, 129}, {0, 0, 1, 2}, {0, 0, 2, 2}, {0, 2, 2, 130}},
        {{128, 1, 2, 0}, {0, 129, 2, 0}, {0, 1, 130, 0}, {0, 1, 2, 0}},
        {{128, 0, 0, 0}, {1, 1, 129, 1}, {2, 2, 130, 2}, {0, 0, 0, 0}},
        {{128, 1, 2, 0}, {1, 2, 0, 1}, {130, 0, 129, 2}, {0, 1, 2, 0}},
        {{128, 1, 2, 0}, {2, 0, 1, 2}, {129, 130, 0, 1}, {0, 1, 2, 0}},
        {{128, 0, 1, 1}, {2, 2, 0, 0}, {1, 1, 130, 2}, {0, 0, 1, 129}},
        {{128, 0, 1, 1}, {1, 1, 130, 2}, {2, 2, 0, 0}, {0, 0, 1, 129}},
        {{128, 1, 0, 129}, {0, 1, 0, 1}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {130, 1, 2, 1}, {2, 1, 2, 129}},
        {{128, 0, 2, 2}, {1, 129, 2, 2}, {0, 0, 2, 2}, {1, 1, 2, 130}},
        {{128, 0, 2, 130}, {0, 0, 1, 1}, {0, 0, 2, 2}, {0, 0, 1, 129}},
        {{128, 2, 2, 0}, {1, 2, 130, 1}, {0, 2, 2, 0}, {1, 2, 2, 129}},
        {{128, 1, 0, 1}, {2, 2, 130, 2}, {2, 2, 2, 2}, {0, 1, 0, 129}},
        {{128, 0, 0, 0}, {2, 1, 2, 1}, {130, 1, 2, 1}, {2, 1, 2, 129}},
        {{128, 1, 0, 129}, {0, 1, 0, 1}, {0, 1, 0, 1}, {2, 2, 2, 130}},
        {{128, 2, 2, 130}, {0, 1, 1, 1}, {0, 2, 2, 2}, {0, 1, 1, 129}},
        {{128, 0, 0, 2}, {1, 129, 1, 2}, {0, 0, 0, 2}, {1, 1, 1, 130}},
        {{128, 0, 0, 0}, {2, 129, 1, 2}, {2, 1, 1, 2}, {2, 1, 1, 130}},
        {{128, 2, 2, 2}, {0, 129, 1, 1}, {0, 1, 1, 1}, {0, 2, 2, 130}},
        {{128, 0, 0, 2}, {1, 1, 1, 2}, {129, 1, 1, 2}, {0, 0, 0, 130}},
        {{128, 1, 1, 0}, {0, 129, 1, 0}, {0, 1, 1, 0}, {2, 2, 2, 130}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {2, 1, 129, 2}, {2, 1, 1, 130}},
        {{128, 1, 1, 0}, {0, 129, 1, 0}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 0, 2, 2}, {0, 0, 1, 1}, {0, 0, 129, 1}, {0, 0, 2, 130}},
        {{128, 0, 2, 2}, {1, 1, 2, 2}, {129, 1, 2, 2}, {0, 0, 2, 130}},
        {{128, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {2, 129, 1, 130}},
        {{128, 0, 0, 130}, {0, 0, 0, 1}, {0, 0, 0, 2}, {0, 0, 0, 129}},
        {{128, 2, 2, 2}, {1, 2, 2, 2}, {0, 2, 2, 2}, {129, 2, 2, 130}},
        {{128, 1, 0, 129}, {2, 2, 2, 2}, {2, 2, 2, 2}, {2, 2, 2, 130}},
        {{128, 1, 1, 129}, {2, 0, 1, 1}, {130, 2, 0, 1}, {2, 2, 2, 0}},
    },
};

uint32_t Interpolate(uint32_t a, uint32_t b, uint32_t weight) {
  return (a * (64 - weight) + b * weight + 32) >> 6;
}

// BC7: 16 bytes to RGBA, one of eight modes each with its own endpoint layout.
void DecodeBc7(const uint8_t* src, uint8_t* dst) {
  const uint32_t weight2[4] = {0, 21, 43, 64};
  const uint32_t weight3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
  const uint32_t weight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
  // Which modes have a p-bit per endpoint: 0, 3, 6 and 7. Mode 1 shares one
  // p-bit between the two endpoints of a subset.
  const uint32_t kModeHasPbits = 0xC9U;

  BitStream stream;
  for (size_t i = 0; i < 8; ++i) {
    stream.low |= uint64_t(src[i]) << (8 * i);
    stream.high |= uint64_t(src[i + 8]) << (8 * i);
  }

  uint32_t mode = 0;
  while (mode < 8 && stream.ReadBit() == 0) {
    mode += 1;
  }
  if (mode >= 8) {
    // A reserved mode comes out transparent black.
    std::memset(dst, 0, 16 * 4);
    return;
  }

  uint32_t partition = 0;
  uint32_t numPartitions = 1;
  uint32_t rotation = 0;
  uint32_t indexSelectionBit = 0;
  if (mode <= 3 || mode == 7) {
    numPartitions = (mode == 0 || mode == 2) ? 3 : 2;
    partition = stream.ReadBits(mode == 0 ? 4 : 6);
  }
  const uint32_t numEndpoints = numPartitions * 2;
  if (mode == 4 || mode == 5) {
    rotation = stream.ReadBits(2);
    if (mode == 4) {
      indexSelectionBit = stream.ReadBit();
    }
  }

  uint32_t endpoints[6][4] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      endpoints[e][i] = stream.ReadBits(kBc7ActualBits[0][mode]);
    }
  }
  if (kBc7ActualBits[1][mode] > 0) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      endpoints[e][3] = stream.ReadBits(kBc7ActualBits[1][mode]);
    }
  }

  // Modes with a p-bit: shift the endpoints up to make room, then fold it in.
  const bool hasPbits = mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7;
  if (hasPbits) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      for (uint32_t c = 0; c < 4; ++c) {
        endpoints[e][c] <<= 1;
      }
    }
    if (mode == 1) {
      // One p-bit shared by the first two endpoints of each channel pair.
      const uint32_t first = stream.ReadBit();
      const uint32_t second = stream.ReadBit();
      for (uint32_t c = 0; c < 3; ++c) {
        endpoints[0][c] |= first;
        endpoints[1][c] |= first;
        endpoints[2][c] |= second;
        endpoints[3][c] |= second;
      }
    } else if ((kModeHasPbits & (1u << mode)) != 0) {
      for (uint32_t e = 0; e < numEndpoints; ++e) {
        const uint32_t bit = stream.ReadBit();
        for (uint32_t c = 0; c < 4; ++c) {
          endpoints[e][c] |= bit;
        }
      }
    }
  }

  // Widen each component to 8 bits by putting its top bit at bit 7 and
  // replicating that bit into the bits the shift revealed.
  for (uint32_t e = 0; e < numEndpoints; ++e) {
    uint32_t bits = kBc7ActualBits[0][mode] + (hasPbits ? 1 : 0);
    for (uint32_t c = 0; c < 3; ++c) {
      endpoints[e][c] <<= 8 - bits;
      endpoints[e][c] |= endpoints[e][c] >> bits;
    }
    bits = kBc7ActualBits[1][mode] + (hasPbits ? 1 : 0);
    endpoints[e][3] <<= 8 - bits;
    endpoints[e][3] |= endpoints[e][3] >> bits;
  }
  if (kBc7ActualBits[1][mode] == 0) {
    for (uint32_t e = 0; e < numEndpoints; ++e) {
      endpoints[e][3] = 0xFF;
    }
  }

  uint32_t indexBits = (mode == 0 || mode == 1) ? 3 : (mode == 6 ? 4 : 2);
  const uint32_t indexBits2 = mode == 4 ? 3 : (mode == 5 ? 2 : 0);
  const uint32_t* weights =
      indexBits == 2 ? weight2 : (indexBits == 3 ? weight3 : weight4);
  const uint32_t* weights2 = indexBits2 == 2 ? weight2 : weight3;

  // The indices of a row are not interleaved with the next row's, so they are
  // all read first and then turned into pixels.
  uint8_t indices[4][4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      const uint32_t set = numPartitions == 1
                               ? ((i | j) != 0 ? 0u : 128u)
                               : uint32_t(kBc7PartitionSets[numPartitions - 2][partition][i][j]);
      uint32_t bits = (mode == 0 || mode == 1) ? 3 : (mode == 6 ? 4 : 2);
      if ((set & 0x80) != 0) {
        bits -= 1;
      }
      indices[i][j] = uint8_t(stream.ReadBits(bits));
    }
  }

  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      uint32_t set = numPartitions == 1 ? ((i | j) != 0 ? 0u : 128u)
                                       : uint32_t(kBc7PartitionSets[numPartitions - 2][partition]
                                                                          [i][j]);
      set &= 3;
      const uint32_t weight = weights[indices[i][j]];
      uint32_t r, g, b, a;
      if (indexBits2 == 0) {
        r = Interpolate(endpoints[set * 2][0], endpoints[set * 2 + 1][0], weight);
        g = Interpolate(endpoints[set * 2][1], endpoints[set * 2 + 1][1], weight);
        b = Interpolate(endpoints[set * 2][2], endpoints[set * 2 + 1][2], weight);
        a = Interpolate(endpoints[set * 2][3], endpoints[set * 2 + 1][3], weight);
      } else {
        const uint32_t index2 = stream.ReadBits((i | j) != 0 ? indexBits2 : indexBits2 - 1);
        const uint32_t weight2v = weights2[index2];
        // The index selection bit says which of the two index sets drives the
        // colour and which drives the alpha.
        if (indexSelectionBit == 0) {
          r = Interpolate(endpoints[set * 2][0], endpoints[set * 2 + 1][0], weight);
          g = Interpolate(endpoints[set * 2][1], endpoints[set * 2 + 1][1], weight);
          b = Interpolate(endpoints[set * 2][2], endpoints[set * 2 + 1][2], weight);
          a = Interpolate(endpoints[set * 2][3], endpoints[set * 2 + 1][3], weight2v);
        } else {
          r = Interpolate(endpoints[set * 2][0], endpoints[set * 2 + 1][0], weight2v);
          g = Interpolate(endpoints[set * 2][1], endpoints[set * 2 + 1][1], weight2v);
          b = Interpolate(endpoints[set * 2][2], endpoints[set * 2 + 1][2], weight2v);
          a = Interpolate(endpoints[set * 2][3], endpoints[set * 2 + 1][3], weight);
        }
      }
      switch (rotation) {
        case 1:
          // Scalar(R), vector(AGB).
          std::swap(a, r);
          break;
        case 2:
          // Scalar(G), vector(RAB).
          std::swap(a, g);
          break;
        case 3:
          // Scalar(B), vector(RGA).
          std::swap(a, b);
          break;
        default:
          break;
      }
      dst[(i * 4 + j) * 4 + 0] = uint8_t(r);
      dst[(i * 4 + j) * 4 + 1] = uint8_t(g);
      dst[(i * 4 + j) * 4 + 2] = uint8_t(b);
      dst[(i * 4 + j) * 4 + 3] = uint8_t(a);
    }
  }
}

// ---------------------------------------------------------------------------
// astc-decode 0.3.1: the ASTC block decoder
// ---------------------------------------------------------------------------

// A 128 bit little endian bit stream, which is what ASTC blocks are: 128 bits
// of configuration and data in the order the format defines them.
struct AstcBitStream {
  uint64_t lo = 0;
  uint64_t hi = 0;
  uint32_t bitsRead = 0;

  U128 ReadBits128(uint32_t numBits) {
    if (numBits == 0) {
      return U128{};
    }
    U128 out{};
    if (numBits < 64) {
      out.lo = lo & ((uint64_t(1) << numBits) - 1);
    } else {
      out.lo = lo;
      if (numBits > 64) {
        const uint32_t top = numBits - 64;
        out.hi = top >= 64 ? hi : (hi & ((uint64_t(1) << top) - 1));
      }
    }
    if (numBits >= 64) {
      const uint32_t shift = numBits - 64;
      lo = shift == 0 ? hi : (hi >> shift);
      hi = 0;
    } else {
      lo = (lo >> numBits) | (hi << (64 - numBits));
      hi >>= numBits;
    }
    bitsRead += numBits;
    return out;
  }

  uint32_t ReadBits(uint32_t numBits) { return uint32_t(ReadBits128(numBits).lo); }
  uint32_t ReadBit() { return ReadBits(1); }
};

uint64_t ReverseBits64(uint64_t value) {
  uint64_t out = 0;
  for (int i = 0; i < 64; ++i) {
    out = (out << 1) | ((value >> i) & 1);
  }
  return out;
}

// How many bits are set, which is how the spec's tables spell "the log2 of this
// power of two".
uint32_t CountOnes(uint32_t value) {
  uint32_t count = 0;
  while (value != 0) {
    count += value & 1;
    value >>= 1;
  }
  return count;
}

enum EncodingType { kJustBits, kQuint, kTrit };

struct IntegerEncoding {
  EncodingType encoding = kJustBits;
  uint32_t numBits = 0;
};

// How many bits an integer sequence of n values takes in this encoding.
uint32_t EncodedBitLength(const IntegerEncoding& enc, uint32_t nValues) {
  uint32_t total = enc.numBits * nValues;
  if (enc.encoding == kTrit) {
    total += (nValues * 8 + 4) / 5;
  } else if (enc.encoding == kQuint) {
    total += (nValues * 7 + 2) / 3;
  }
  return total;
}

// The smallest encoding that can hold every value from 0 to maxVal: plain bits
// when that is a power of two, otherwise trits or quints.
IntegerEncoding CreateEncoding(uint32_t maxVal) {
  while (maxVal > 0) {
    const uint32_t check = maxVal + 1;
    if ((check & (check - 1)) == 0) {
      return IntegerEncoding{kJustBits, uint32_t(CountOnes(maxVal))};
    }
    if (check % 3 == 0 && ((check / 3) & ((check / 3) - 1)) == 0) {
      return IntegerEncoding{kTrit, uint32_t(CountOnes(check / 3 - 1))};
    }
    if (check % 5 == 0 && ((check / 5) & ((check / 5) - 1)) == 0) {
      return IntegerEncoding{kQuint, uint32_t(CountOnes(check / 5 - 1))};
    }
    maxVal -= 1;
  }
  return IntegerEncoding{kJustBits, 0};
}

// One table per possible value count, and the deduplicated sequence of
// encodings, which is what the spec's "pick the first that fits" search walks.
// Both are built once, since a table lookup would otherwise cost more than the
// search it is for.
struct EncodingTables {
  IntegerEncoding byValue[256] = {};
  IntegerEncoding sequence[256] = {};
  uint32_t sequenceLength = 0;
};

const EncodingTables& GetEncodingTables() {
  static const EncodingTables tables = [] {
    EncodingTables t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t.byValue[i] = CreateEncoding(i);
    }
    t.sequence[0] = t.byValue[0];
    t.sequenceLength = 1;
    for (uint32_t i = 1; i < 256; ++i) {
      const IntegerEncoding& enc = t.byValue[i];
      const IntegerEncoding& previous = t.sequence[t.sequenceLength - 1];
      if (enc.encoding != previous.encoding || enc.numBits != previous.numBits) {
        t.sequence[t.sequenceLength] = enc;
        t.sequenceLength += 1;
      }
    }
    return t;
  }();
  return tables;
}

uint32_t BitsGet(uint32_t value, uint32_t pos) { return (value >> pos) & 1; }

uint32_t BitsRange(uint32_t value, uint32_t start, uint32_t end) {
  const uint32_t mask = (uint32_t(1) << (end - start + 1)) - 1;
  return (value >> start) & mask;
}

// A trit or quint holds two fields: a value in 0..2 or 0..4, and a bit field
// whose width depends on the value. Each of the four cases below is the spec's
// table (C.2.8) expanded, returning the value's contribution to the final 8 bit
// component or 6 bit weight: `(a & mask) | ((d * c + b) ^ a) >> 2`, where `d` is
// the value, `a` the low bit of the bit field scaled, and `b` and `c` come from
// the rest of the bit field through the table.
uint32_t DecodeTritColor(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  const uint32_t a = (bitValue & 1) * 0x1FF;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 204; b = 0; break;
    case 2: c = 93; b = (x << 8) | (x << 4) | (x << 2) | (x << 1); break;
    case 3: c = 44; b = (x << 7) | (x << 2) | x; break;
    case 4: c = 22; b = (x << 6) | x; break;
    case 5: c = 11; b = (x << 5) | (x >> 2); break;
    case 6: c = 5; b = (x << 4) | (x >> 4); break;
    default: return 0;
  }
  return (a & 0x80) | (((value * c + b) ^ a) >> 2);
}

uint32_t DecodeTritWeight(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  if (numBits == 0) {
    // No bit field, so the value stands on its own.
    static const uint32_t kWeights[3] = {0, 32, 63};
    return kWeights[value < 3 ? value : 2];
  }
  const uint32_t a = (bitValue & 1) * 0x7F;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 50; b = 0; break;
    case 2: c = 23; b = (x << 6) | (x << 2) | x; break;
    case 3: c = 11; b = (x << 5) | x; break;
    default: return 0;
  }
  return (a & 0x20) | (((value * c + b) ^ a) >> 2);
}

uint32_t DecodeQuintColor(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  const uint32_t a = (bitValue & 1) * 0x1FF;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 113; b = 0; break;
    case 2: c = 54; b = (x << 8) | (x << 3) | (x << 2); break;
    case 3: c = 26; b = (x << 7) | (x << 1) | (x >> 1); break;
    case 4: c = 13; b = (x << 6) | (x >> 1); break;
    case 5: c = 6; b = (x << 5) | (x >> 3); break;
    default: return 0;
  }
  return (a & 0x80) | (((value * c + b) ^ a) >> 2);
}

uint32_t DecodeQuintWeight(uint32_t value, uint32_t bitValue, uint32_t numBits) {
  if (numBits == 0) {
    static const uint32_t kWeights[5] = {0, 16, 32, 47, 63};
    return kWeights[value < 5 ? value : 4];
  }
  const uint32_t a = (bitValue & 1) * 0x7F;
  const uint32_t x = bitValue >> 1;
  uint32_t b, c;
  switch (numBits) {
    case 1: c = 28; b = 0; break;
    case 2: c = 13; b = (x << 6) | (x << 1); break;
    default: return 0;
  }
  return (a & 0x20) | (((value * c + b) ^ a) >> 2);
}

// One trit or quint: a value plus the bit field it shares with its neighbours.
struct IntSequence {
  uint32_t value = 0;
  uint32_t bitValue = 0;
  bool valid = false;
};

// Reads five trit encoded values, spec section C.2.12.
void DecodeTritBlock(AstcBitStream& bits, uint32_t bitsPerValue, IntSequence out[5]) {
  uint32_t m[5];
  uint32_t t[5] = {};
  m[0] = bits.ReadBits(bitsPerValue);
  uint32_t tt = bits.ReadBits(2);
  m[1] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBits(2) << 2;
  m[2] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBit() << 4;
  m[3] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBits(2) << 5;
  m[4] = bits.ReadBits(bitsPerValue);
  tt |= bits.ReadBit() << 7;

  uint32_t c;
  if (BitsRange(tt, 2, 4) == 7) {
    c = (BitsRange(tt, 5, 7) << 2) | BitsRange(tt, 0, 1);
    t[3] = 2;
    t[4] = 2;
  } else {
    c = BitsRange(tt, 0, 4);
    if (BitsRange(tt, 5, 6) == 3) {
      t[4] = 2;
      t[3] = BitsGet(tt, 7);
    } else {
      t[4] = BitsGet(tt, 7);
      t[3] = BitsRange(tt, 5, 6);
    }
  }
  if (BitsRange(c, 0, 1) == 3) {
    t[2] = 2;
    t[1] = BitsGet(c, 4);
    t[0] = (BitsGet(c, 3) << 1) | (BitsGet(c, 2) & ~BitsGet(c, 3));
  } else if (BitsRange(c, 2, 3) == 3) {
    t[2] = 2;
    t[1] = 2;
    t[0] = BitsRange(c, 0, 1);
  } else {
    t[2] = BitsGet(c, 4);
    t[1] = BitsRange(c, 2, 3);
    t[0] = (BitsGet(c, 1) << 1) | (BitsGet(c, 0) & ~BitsGet(c, 1));
  }
  for (uint32_t i = 0; i < 5; ++i) {
    out[i].bitValue = m[i];
    out[i].value = t[i];
    out[i].valid = true;
  }
}

// Reads three quint encoded values, spec section C.2.12.
void DecodeQuintBlock(AstcBitStream& bits, uint32_t bitsPerValue, IntSequence out[3]) {
  uint32_t m[3];
  uint32_t q[3] = {};
  m[0] = bits.ReadBits(bitsPerValue);
  uint32_t qq = bits.ReadBits(3);
  m[1] = bits.ReadBits(bitsPerValue);
  qq |= bits.ReadBits(2) << 3;
  m[2] = bits.ReadBits(bitsPerValue);
  qq |= bits.ReadBits(2) << 5;

  uint32_t c;
  if (BitsRange(qq, 1, 2) == 3 && BitsRange(qq, 5, 6) == 0) {
    q[0] = 4;
    q[1] = 4;
    q[2] = (BitsGet(qq, 0) << 2) | ((BitsGet(qq, 4) & ~BitsGet(qq, 0)) << 1) |
           (BitsGet(qq, 3) & ~BitsGet(qq, 0));
  } else {
    if (BitsRange(qq, 1, 2) == 3) {
      q[2] = 4;
      c = (BitsRange(qq, 3, 4) << 3) | (((~BitsRange(qq, 5, 6)) & 3) << 1) | BitsGet(qq, 0);
    } else {
      q[2] = BitsRange(qq, 5, 6);
      c = BitsRange(qq, 0, 4);
    }
    if (BitsRange(c, 0, 2) == 5) {
      q[1] = 4;
      q[0] = BitsRange(c, 3, 4);
    } else {
      q[1] = BitsRange(c, 3, 4);
      q[0] = BitsRange(c, 0, 2);
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    out[i].bitValue = m[i];
    out[i].value = q[i];
    out[i].valid = true;
  }
}

// Spreads a value's low bits up to `toBit`, so a 5 bit value becomes 8 bits.
uint32_t Replicate(uint32_t value, uint32_t numBits, uint32_t toBit) {
  if (numBits == 0 || toBit == 0) {
    return 0;
  }
  uint32_t result = value << (toBit - numBits);
  uint32_t shift = numBits;
  for (int i = 0; i < 8 && shift < 64; ++i) {
    const uint32_t next = result >> shift;
    if (next == 0) {
      break;
    }
    result |= next;
    shift *= 2;
  }
  return result;
}

struct TexelWeightParams {
  uint32_t width = 0;
  uint32_t height = 0;
  bool dualPlane = false;
  uint32_t maxWeight = 0;
  bool isError = false;
  bool voidExtentLdr = false;
  bool voidExtentHdr = false;

  uint32_t NumWeightValues() const { return width * height * (dualPlane ? 2 : 1); }

  uint32_t PackedBitSize() const {
    return EncodedBitLength(GetEncodingTables().byValue[maxWeight], NumWeightValues());
  }
};

// The first eleven bits of a block are the block mode, which fixes the weight
// grid size, the number of bits per weight and whether there are two planes.
TexelWeightParams DecodeBlockInfo(AstcBitStream& stream) {
  TexelWeightParams params;
  const uint32_t modeBits = stream.ReadBits(11);
  if ((modeBits & 0x1FF) == 0x1FC) {
    if ((modeBits & 0x200) != 0) {
      params.voidExtentHdr = true;
    } else {
      params.voidExtentLdr = true;
    }
    if ((modeBits & 0x400) == 0 || stream.ReadBit() == 0) {
      params.isError = true;
    }
    return params;
  }
  if ((modeBits & 0xF) == 0 || ((modeBits & 0x3) == 0 && (modeBits & 0x1C0) == 0x1C0)) {
    params.isError = true;
    return params;
  }

  uint32_t layout;
  if ((modeBits & 0x1) != 0 || (modeBits & 0x2) != 0) {
    if ((modeBits & 0x8) != 0) {
      if ((modeBits & 0x4) != 0) {
        layout = (modeBits & 0x100) != 0 ? 4 : 3;
      } else {
        layout = 2;
      }
    } else {
      layout = (modeBits & 0x4) != 0 ? 1 : 0;
    }
  } else {
    if ((modeBits & 0x100) != 0) {
      if ((modeBits & 0x80) != 0) {
        layout = (modeBits & 0x20) != 0 ? 8 : 7;
      } else {
        layout = 9;
      }
    } else {
      layout = (modeBits & 0x80) != 0 ? 6 : 5;
    }
  }

  uint32_t r = (modeBits & 0x10) >> 4;
  if (layout < 5) {
    r |= (modeBits & 0x3) << 1;
  } else {
    r |= (modeBits & 0xC) >> 1;
  }
  if (r < 2 || r > 7) {
    // Not a real mode; the Rust asserts here and gives up on the whole file.
    params.isError = true;
    return params;
  }

  const uint32_t a = (modeBits >> 5) & 0x3;
  const uint32_t b = (modeBits >> 7) & 0x3;
  switch (layout) {
    case 0: params.width = b + 4; params.height = a + 2; break;
    case 1: params.width = b + 8; params.height = a + 2; break;
    case 2: params.width = a + 2; params.height = b + 8; break;
    case 3: params.width = a + 2; params.height = ((modeBits >> 7) & 0x1) + 6; break;
    case 4: params.width = ((modeBits >> 7) & 0x1) + 2; params.height = a + 2; break;
    case 5: params.width = 12; params.height = a + 2; break;
    case 6: params.width = a + 2; params.height = 12; break;
    case 7: params.width = 6; params.height = 10; break;
    case 8: params.width = 10; params.height = 6; break;
    case 9: params.width = a + 6; params.height = ((modeBits >> 9) & 0x3) + 6; break;
    default: break;
  }

  params.dualPlane = layout != 9 && (modeBits & 0x400) != 0;
  const bool highPrecision = layout != 9 && (modeBits & 0x200) != 0;
  const uint32_t maxWeights[6] = {9, 11, 15, 19, 23, 31};
  const uint32_t lowWeights[6] = {1, 2, 3, 4, 5, 7};
  params.maxWeight = highPrecision ? maxWeights[r - 2] : lowWeights[r - 2];
  return params;
}

uint8_t ClampByte(int32_t value) { return uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value)); }

// One bit moved between two fields of an integer sequence, spec C.2.14. The
// fields are signed: the value keeps its sign, which is what decides between
// the two ways the endpoints can be read.
void BitTransfer(int32_t& a, int32_t& b) {
  b >>= 1;
  b |= a & 0x80;
  a >>= 1;
  a &= 0x3F;
  if ((a & 0x20) != 0) {
    a -= 0x40;
  }
}

// Section C.2.14: the blue channel gets the extra precision when the sum of
// the odd endpoints is below the sum of the even ones.
void BlueContract(int32_t r, int32_t g, int32_t b, int32_t a, uint8_t out[4]) {
  out[0] = ClampByte((r + b) >> 1);
  out[1] = ClampByte((g + b) >> 1);
  out[2] = ClampByte(b);
  out[3] = ClampByte(a);
}

// Turns the decoded colour values into the two endpoints of one partition,
// following spec C.2.14's endpoint modes.
void ComputeEndpoints(const uint8_t* values, uint32_t* used, uint32_t mode, uint8_t ep[2][4]) {
  auto take = [&](uint32_t count, int32_t* dst) {
    for (uint32_t i = 0; i < count; ++i) {
      dst[i] = values[*used];
      *used += 1;
    }
  };
  // Signed, because the endpoint modes that move a bit between fields leave a
  // negative value behind and the mode's own comparison reads that sign.
  int32_t v[8] = {};
  switch (mode) {
    case 0: {
      take(2, v);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(v[0]);
      ep[0][3] = 255;
      ep[1][0] = ep[1][1] = ep[1][2] = ClampByte(v[1]);
      ep[1][3] = 255;
      break;
    }
    case 1: {
      take(2, v);
      const int32_t low = (v[0] >> 2) | (v[1] & 0xC0);
      const int32_t high = low + (v[1] & 0x3F) > 255 ? 255 : low + (v[1] & 0x3F);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(low);
      ep[0][3] = 255;
      ep[1][0] = ep[1][1] = ep[1][2] = ClampByte(high);
      ep[1][3] = 255;
      break;
    }
    case 4: {
      take(4, v);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(v[0]);
      ep[0][3] = ClampByte(v[2]);
      ep[1][0] = ep[1][1] = ep[1][2] = ClampByte(v[1]);
      ep[1][3] = ClampByte(v[3]);
      break;
    }
    case 5: {
      take(4, v);
      BitTransfer(v[1], v[0]);
      BitTransfer(v[3], v[2]);
      ep[0][0] = ep[0][1] = ep[0][2] = ClampByte(v[0]);
      ep[0][3] = ClampByte(v[2]);
      const uint8_t sum = ClampByte(v[0] + v[1]);
      ep[1][0] = ep[1][1] = ep[1][2] = sum;
      ep[1][3] = ClampByte(int32_t(v[2] + v[3]));
      break;
    }
    case 6: {
      take(4, v);
      ep[0][0] = ClampByte((v[0] * v[3]) >> 8);
      ep[0][1] = ClampByte((v[1] * v[3]) >> 8);
      ep[0][2] = ClampByte((v[2] * v[3]) >> 8);
      ep[0][3] = 255;
      ep[1][0] = ClampByte(v[0]);
      ep[1][1] = ClampByte(v[1]);
      ep[1][2] = ClampByte(v[2]);
      ep[1][3] = 255;
      break;
    }
    case 8: {
      take(6, v);
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = 255;
        ep[1][0] = ClampByte(v[1]);
        ep[1][1] = ClampByte(v[3]);
        ep[1][2] = ClampByte(v[5]);
        ep[1][3] = 255;
      } else {
        BlueContract(v[1], v[3], v[5], 255, ep[0]);
        BlueContract(v[0], v[2], v[4], 255, ep[1]);
      }
      break;
    }
    case 9: {
      take(6, v);
      BitTransfer(v[1], v[0]);
      BitTransfer(v[3], v[2]);
      BitTransfer(v[5], v[4]);
      if (v[1] + v[3] + v[5] >= 0) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = 255;
        ep[1][0] = ClampByte(int32_t(v[0] + v[1]));
        ep[1][1] = ClampByte(int32_t(v[2] + v[3]));
        ep[1][2] = ClampByte(int32_t(v[4] + v[5]));
        ep[1][3] = 255;
      } else {
        BlueContract(v[0] + v[1], v[2] + v[3], v[4] + v[5], 255, ep[0]);
        BlueContract(v[0], v[2], v[4], 255, ep[1]);
      }
      break;
    }
    case 10: {
      take(6, v);
      ep[0][0] = ClampByte((v[0] * v[3]) >> 8);
      ep[0][1] = ClampByte((v[1] * v[3]) >> 8);
      ep[0][2] = ClampByte((v[2] * v[3]) >> 8);
      ep[0][3] = ClampByte(v[4]);
      ep[1][0] = ClampByte(v[0]);
      ep[1][1] = ClampByte(v[1]);
      ep[1][2] = ClampByte(v[2]);
      ep[1][3] = ClampByte(v[5]);
      break;
    }
    case 12: {
      take(8, v);
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = ClampByte(v[6]);
        ep[1][0] = ClampByte(v[1]);
        ep[1][1] = ClampByte(v[3]);
        ep[1][2] = ClampByte(v[5]);
        ep[1][3] = ClampByte(v[7]);
      } else {
        BlueContract(v[1], v[3], v[5], v[7], ep[0]);
        BlueContract(v[0], v[2], v[4], v[6], ep[1]);
      }
      break;
    }
    case 13: {
      take(8, v);
      BitTransfer(v[1], v[0]);
      BitTransfer(v[3], v[2]);
      BitTransfer(v[5], v[4]);
      BitTransfer(v[7], v[6]);
      if (v[1] + v[3] + v[5] >= 0) {
        ep[0][0] = ClampByte(v[0]);
        ep[0][1] = ClampByte(v[2]);
        ep[0][2] = ClampByte(v[4]);
        ep[0][3] = ClampByte(v[6]);
        ep[1][0] = ClampByte(int32_t(v[0] + v[1]));
        ep[1][1] = ClampByte(int32_t(v[2] + v[3]));
        ep[1][2] = ClampByte(int32_t(v[4] + v[5]));
        ep[1][3] = ClampByte(int32_t(v[6] + v[7]));
      } else {
        BlueContract(v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7], ep[0]);
        BlueContract(v[0], v[2], v[4], v[6], ep[1]);
      }
      break;
    }
    default: {
      // The HDR endpoint modes, which are not used and are not decoded.
      const uint8_t error[4] = {0xFF, 0, 0xFF, 0xFF};
      std::memcpy(ep[0], error, 4);
      std::memcpy(ep[1], error, 4);
      break;
    }
  }
}

uint32_t Hash52(uint32_t p) {
  p ^= p >> 15;
  p -= p << 17;
  p += p << 7;
  p += p << 4;
  p ^= p >> 5;
  p += p << 16;
  p ^= p >> 7;
  p ^= p >> 3;
  p ^= p << 6;
  p ^= p >> 17;
  return p;
}

// Which of up to four partitions a texel belongs to, spec C.2.21: four hashed
// seeds vote with a weighted sum of the texel's position.
uint32_t SelectPartition(uint32_t seed, uint32_t x, uint32_t y, uint32_t z, uint32_t count,
                         bool smallBlock) {
  if (count == 1) {
    return 0;
  }
  if (smallBlock) {
    x <<= 1;
    y <<= 1;
    z <<= 1;
  }
  seed += (count - 1) * 1024;
  const uint32_t rnum = Hash52(seed);
  uint32_t s[12];
  for (uint32_t i = 0; i < 8; ++i) {
    s[i] = (rnum >> (4 * i)) & 0xF;
  }
  s[8] = (rnum >> 18) & 0xF;
  s[9] = (rnum >> 22) & 0xF;
  s[10] = (rnum >> 26) & 0xF;
  s[11] = ((rnum >> 30) | (rnum << 2)) & 0xF;
  for (uint32_t i = 0; i < 12; ++i) {
    s[i] = s[i] * s[i];
  }
  uint32_t sh1, sh2;
  if ((seed & 1) != 0) {
    sh1 = (seed & 2) != 0 ? 4 : 5;
    sh2 = count == 3 ? 6 : 5;
  } else {
    sh1 = count == 3 ? 6 : 5;
    sh2 = (seed & 2) != 0 ? 4 : 5;
  }
  const uint32_t sh3 = (seed & 0x10) != 0 ? sh1 : sh2;
  s[0] >>= sh1; s[1] >>= sh2; s[2] >>= sh1; s[3] >>= sh2;
  s[4] >>= sh1; s[5] >>= sh2; s[6] >>= sh1; s[7] >>= sh2;
  s[8] >>= sh3; s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3;

  uint32_t a = s[0] * x + s[1] * y + s[10] * z + (rnum >> 14);
  uint32_t b = s[2] * x + s[3] * y + s[11] * z + (rnum >> 10);
  uint32_t c = s[4] * x + s[5] * y + s[8] * z + (rnum >> 6);
  uint32_t d = s[6] * x + s[7] * y + s[9] * z + (rnum >> 2);
  a &= 0x3F;
  b &= 0x3F;
  c &= 0x3F;
  d &= 0x3F;
  if (count < 4) {
    d = 0;
  }
  if (count < 3) {
    c = 0;
  }
  if (a >= b && a >= c && a >= d) {
    return 0;
  }
  if (b >= c && b >= d) {
    return 1;
  }
  if (c >= d) {
    return 2;
  }
  return 3;
}

// Decodes the weight grid of one block: the packed weights are expanded, then
// bilinearly interpolated up to the block's footprint (spec C.2.18).
void UnquantizeTexelWeights(AstcBitStream& stream, const TexelWeightParams& params,
                            uint32_t blockWidth, uint32_t blockHeight, uint32_t out[2][144],
                            bool& ok) {
  const uint32_t planeScale = params.dualPlane ? 2 : 1;
  const uint32_t count = params.NumWeightValues();
  uint32_t unquantized[96] = {};
  const IntegerEncoding& enc = GetEncodingTables().byValue[params.maxWeight];
  uint32_t index = 0;
  IntSequence decoded[5];
  if (enc.encoding == kJustBits) {
    for (index = 0; index < count; ++index) {
      unquantized[index] = Replicate(stream.ReadBits(enc.numBits), enc.numBits, 6);
    }
  } else if (enc.encoding == kTrit) {
    while (index < count) {
      DecodeTritBlock(stream, enc.numBits, decoded);
      for (uint32_t i = 0; i < 5 && index < count; ++i, ++index) {
        unquantized[index] = DecodeTritWeight(decoded[i].value, decoded[i].bitValue, enc.numBits);
      }
    }
  } else {
    while (index < count) {
      DecodeQuintBlock(stream, enc.numBits, decoded);
      for (uint32_t i = 0; i < 3 && index < count; ++i, ++index) {
        unquantized[index] = DecodeQuintWeight(decoded[i].value, decoded[i].bitValue, enc.numBits);
      }
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    // A weight of exactly 32 is left out of the encoding, so the values above
    // it shift up one: that is what turns the replicated 42 and 63 into the
    // 43 and 64 the spec's weight tables use. 64 is legal, a weight that was
    // already 64 is not.
    if (unquantized[i] >= 64) {
      ok = false;
      return;
    }
    if (unquantized[i] > 32) {
      unquantized[i] += 1;
    }
  }

  const uint32_t ds = (1024 + (blockWidth / 2)) / (blockWidth - 1);
  const uint32_t dt = (1024 + (blockHeight / 2)) / (blockHeight - 1);
  for (uint32_t plane = 0; plane < planeScale; ++plane) {
    for (uint32_t t = 0; t < blockHeight; ++t) {
      for (uint32_t s = 0; s < blockWidth; ++s) {
        const uint32_t gs = (ds * s * (params.width - 1) + 32) >> 6;
        const uint32_t gt = (dt * t * (params.height - 1) + 32) >> 6;
        const uint32_t js = gs >> 4, fs = gs & 0xF;
        const uint32_t jt = gt >> 4, ft = gt & 0x0F;
        const uint32_t w11 = (fs * ft + 8) >> 4;
        const uint32_t w10 = ft - w11;
        const uint32_t w01 = fs - w11;
        const uint32_t w00 = 16 + w11 - fs - ft;
        const uint32_t v0 = js + jt * params.width;
        uint32_t p00 = 0, p01 = 0, p10 = 0, p11 = 0;
        const uint32_t gridSize = params.width * params.height;
        if (v0 < gridSize) {
          p00 = unquantized[plane + planeScale * v0];
        }
        if (v0 + 1 < gridSize) {
          p01 = unquantized[plane + planeScale * (v0 + 1)];
        }
        if (v0 + params.width < gridSize) {
          p10 = unquantized[plane + planeScale * (v0 + params.width)];
        }
        if (v0 + params.width + 1 < gridSize) {
          p11 = unquantized[plane + planeScale * (v0 + params.width + 1)];
        }
        out[plane][t * blockWidth + s] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
      }
    }
  }
}

// The colour values of every endpoint in the block, in the order the format
// packs them.
uint32_t DecodeColorValues(U128 data, uint32_t nValues, uint32_t bitsForColor,
                           uint8_t out[18]) {
  const EncodingTables& tables = GetEncodingTables();
  uint32_t index = 0;
  while (index < tables.sequenceLength &&
         EncodedBitLength(tables.sequence[index], nValues) <= bitsForColor) {
    index += 1;
  }
  // The Rust indexes one before the first encoding that does not fit, and
  // wraps around when nothing fits at all.
  const IntegerEncoding enc = tables.sequence[index == 0 ? 0 : index - 1];
  AstcBitStream stream;
  stream.lo = data.lo;
  stream.hi = data.hi;
  IntSequence decoded[5];
  if (enc.encoding == kJustBits) {
    for (uint32_t i = 0; i < nValues; ++i) {
      out[i] = uint8_t(Replicate(stream.ReadBits(enc.numBits), enc.numBits, 8));
    }
  } else if (enc.encoding == kTrit) {
    uint32_t i = 0;
    while (i < nValues) {
      DecodeTritBlock(stream, enc.numBits, decoded);
      for (uint32_t k = 0; k < 5 && i < nValues; ++k, ++i) {
        const uint32_t value = DecodeTritColor(decoded[k].value, decoded[k].bitValue, enc.numBits);
        out[i] = uint8_t(value > 255 ? 255 : value);
      }
    }
  } else {
    uint32_t i = 0;
    while (i < nValues) {
      DecodeQuintBlock(stream, enc.numBits, decoded);
      for (uint32_t k = 0; k < 3 && i < nValues; ++k, ++i) {
        const uint32_t value = DecodeQuintColor(decoded[k].value, decoded[k].bitValue, enc.numBits);
        out[i] = uint8_t(value > 255 ? 255 : value);
      }
    }
  }
  return nValues;
}

// Decodes one ASTC block into `dst`, which is blockWidth * blockHeight RGBA
// texels in row order. False means the block is malformed and `dst` holds
// astc's error colour instead.
bool DecodeAstcBlock(const uint8_t* block, uint32_t blockWidth, uint32_t blockHeight,
                    uint8_t* dst) {
  AstcBitStream stream;
  for (size_t i = 0; i < 8; ++i) {
    stream.lo |= uint64_t(block[i]) << (8 * i);
    stream.hi |= uint64_t(block[i + 8]) << (8 * i);
  }
  auto fillError = [&]() {
    for (uint32_t j = 0; j < blockHeight; ++j) {
      for (uint32_t i = 0; i < blockWidth; ++i) {
        uint8_t* p = dst + (j * blockWidth + i) * 4;
        p[0] = 0xFF;
        p[1] = 0;
        p[2] = 0xFF;
        p[3] = 0xFF;
      }
    }
  };

  const TexelWeightParams params = DecodeBlockInfo(stream);
  if (params.isError) {
    fillError();
    return false;
  }
  if (params.voidExtentLdr) {
    // A constant colour block: the void extent is skipped and the four 16 bit
    // colour components are the high bytes of the image.
    for (uint32_t i = 0; i < 4; ++i) {
      stream.ReadBits(13);
    }
    const uint8_t color[4] = {uint8_t(stream.ReadBits(16) >> 8), uint8_t(stream.ReadBits(16) >> 8),
                              uint8_t(stream.ReadBits(16) >> 8), uint8_t(stream.ReadBits(16) >> 8)};
    for (uint32_t j = 0; j < blockHeight; ++j) {
      for (uint32_t i = 0; i < blockWidth; ++i) {
        std::memcpy(dst + (j * blockWidth + i) * 4, color, 4);
      }
    }
    return true;
  }
  if (params.voidExtentHdr || params.width > blockWidth || params.height > blockHeight ||
      params.NumWeightValues() > 64) {
    fillError();
    return false;
  }
  const uint32_t nWeightBits = params.PackedBitSize();
  if (nWeightBits < 24 || nWeightBits > 96) {
    fillError();
    return false;
  }

  const uint32_t nPartitions = stream.ReadBits(2) + 1;
  if (nPartitions == 4 && params.dualPlane) {
    fillError();
    return false;
  }

  uint32_t endpointMods[4] = {};
  uint32_t partitionIndex = 0;
  uint32_t baseCem = 0;
  if (nPartitions == 1) {
    endpointMods[0] = stream.ReadBits(4);
  } else {
    partitionIndex = stream.ReadBits(10);
    baseCem = stream.ReadBits(6);
  }
  const uint32_t baseMode = baseCem & 3;

  uint32_t nonColorBits = nWeightBits + stream.bitsRead;
  uint32_t extraCemBits = 0;
  if (baseMode != 0) {
    if (nPartitions == 2) {
      extraCemBits = 2;
    } else if (nPartitions == 3) {
      extraCemBits = 5;
    } else if (nPartitions == 4) {
      extraCemBits = 8;
    }
  }
  nonColorBits += extraCemBits;
  const uint32_t planeSelectorBits = params.dualPlane ? 2 : 0;
  nonColorBits += planeSelectorBits;
  if (nonColorBits >= 128) {
    fillError();
    return false;
  }

  const U128 colorData = stream.ReadBits128(128 - nonColorBits);
  const uint32_t planeIdx = stream.ReadBits(planeSelectorBits);
  if (baseMode != 0) {
    const uint32_t extraCem = stream.ReadBits(extraCemBits);
    uint32_t cem = (extraCem << 6) | baseCem;
    cem >>= 2;
    bool switches[4] = {};
    for (uint32_t i = 0; i < nPartitions; ++i) {
      switches[i] = (cem & 1) != 0;
      cem >>= 1;
    }
    uint32_t multiplies[4] = {};
    for (uint32_t i = 0; i < nPartitions; ++i) {
      multiplies[i] = cem & 3;
      cem >>= 2;
    }
    for (uint32_t i = 0; i < nPartitions; ++i) {
      endpointMods[i] = baseMode - (switches[i] ? 0u : 1u);
      endpointMods[i] = (endpointMods[i] << 2) | multiplies[i];
    }
  } else if (nPartitions > 1) {
    const uint32_t cem = baseCem >> 2;
    for (uint32_t i = 0; i < nPartitions; ++i) {
      endpointMods[i] = cem;
    }
  }
  // The block's configuration, colour data and plane selector together must be
  // exactly the 128 bits the weights do not use.
  if (stream.bitsRead + nWeightBits != 128) {
    fillError();
    return false;
  }

  uint32_t nValues = 0;
  for (uint32_t i = 0; i < nPartitions; ++i) {
    nValues += ((endpointMods[i] >> 2) + 1) << 1;
  }
  if (nValues > 18 || (nValues * 13 + 4) / 5 > 128 - nonColorBits) {
    fillError();
    return false;
  }

  uint8_t colorValues[18] = {};
  DecodeColorValues(colorData, nValues, 128 - nonColorBits, colorValues);
  uint8_t endpoints[4][2][4] = {};
  uint32_t used = 0;
  for (uint32_t i = 0; i < nPartitions; ++i) {
    ComputeEndpoints(colorValues, &used, endpointMods[i], endpoints[i]);
  }

  // The weights are the block's bits from the other end, so their stream starts
  // with the block's 128 bits reversed.
  U128 blockBits{};
  for (size_t i = 0; i < 8; ++i) {
    blockBits.lo |= uint64_t(block[i]) << (8 * i);
    blockBits.hi |= uint64_t(block[i + 8]) << (8 * i);
  }
  // Reversing the whole 128 bit value swaps the halves as well as the bits
  // inside them, since the low half of the result is the top half of the block.
  U128 reversed;
  reversed.lo = ReverseBits64(blockBits.hi);
  reversed.hi = ReverseBits64(blockBits.lo);
  // Keep only the low nWeightBits of the reversed value, which is how many bits
  // the weights take: the weight stream is the tail of the block.
  if (nWeightBits < 64) {
    reversed.lo &= (uint64_t(1) << nWeightBits) - 1;
    reversed.hi = 0;
  } else if (nWeightBits < 128) {
    reversed.hi &= (uint64_t(1) << (nWeightBits - 64)) - 1;
  }
  AstcBitStream weightStream;
  weightStream.lo = reversed.lo;
  weightStream.hi = reversed.hi;
  uint32_t weights[2][144] = {};
  bool ok = true;
  UnquantizeTexelWeights(weightStream, params, blockWidth, blockHeight, weights, ok);
  if (!ok) {
    fillError();
    return false;
  }

  const bool smallBlock = (blockHeight * blockWidth) < 32;
  for (uint32_t j = 0; j < blockHeight; ++j) {
    for (uint32_t i = 0; i < blockWidth; ++i) {
      const uint32_t partition =
          SelectPartition(partitionIndex, i, j, 0, nPartitions, smallBlock);
      if (partition >= nPartitions) {
        fillError();
        return false;
      }
      uint8_t* p = dst + (j * blockWidth + i) * 4;
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t c0 = uint32_t(endpoints[partition][0][c]) * 0x101;
        const uint32_t c1 = uint32_t(endpoints[partition][1][c]) * 0x101;
        const uint32_t plane = (params.dualPlane && (planeIdx & 3) == c) ? 1 : 0;
        const uint32_t weight = weights[plane][j * blockWidth + i];
        const uint32_t color = (c0 * (64 - weight) + c1 * weight + 32) / 64;
        p[c] = uint8_t((color * 255 + 32767) / 65536);
      }
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

// Fills the RGBA image from the untiled top mip. BC4 and BC5 become grey and
// grey plus alpha, which is how retrotool writes them out as well; BC6H's half
// float channels are reduced with the usual 16 to 8 bit rule, because it has no
// 8 bit form of its own.
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
            // Three half float channels, reduced to 8 bits by taking their high
            // byte, which is the usual 16 to 8 bit rule.
            uint16_t half[4 * 4 * 3] = {};
            DecodeBc6h(src, half, format == kTxtrFormatBc6hSfloat);
            for (size_t i = 0; i < 16; ++i) {
              decoded[i * 4 + 0] = uint8_t(half[i * 3 + 0] >> 8);
              decoded[i * 4 + 1] = uint8_t(half[i * 3 + 1] >> 8);
              decoded[i * 4 + 2] = uint8_t(half[i * 3 + 2] >> 8);
              decoded[i * 4 + 3] = 255;
            }
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
  std::vector<uint8_t> surface;
  if (!BuildSurface(data, size, meta, surface, error)) {
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
  if (surface.size() < swizzledSize) {
    error = "remastered txtr: the surface is " + std::to_string(surface.size()) +
            " bytes, its top mip alone needs " + std::to_string(swizzledSize);
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

namespace {

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

}  // namespace

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
