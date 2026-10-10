#include "dolphin/thp.h"

#include "../../internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
// Error codes
constexpr s32 kBadSyntax = 3;
constexpr s32 kBadPrecision = 10;
constexpr s32 kUnsupportedMarker = 11;
constexpr s32 kBadComponentCount = 12;
constexpr s32 kMissingHuffmanTable = 15;
constexpr s32 kBadSampling = 19;
constexpr s32 kNoInput = 25;
constexpr s32 kNoOutput = 27;

constexpr std::array<u8, 64> kNaturalOrder{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                           12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                           35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                           58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr std::array<double, 8> kAanScale{1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                          1.0f, 0.785694958f, 0.541196100f, 0.275899379f};

// AAN butterfly constants
constexpr float kSqrt2 = 1.414213562f;
constexpr float kC2 = 1.847759065f;        // 2 cos(pi/8)
constexpr float kC2MinusC6 = 1.082392200f; // 2 cos(pi/8) - 2 cos(3pi/8)
constexpr float kC2PlusC6 = 2.613125930f;  // 2 cos(pi/8) + 2 cos(3pi/8)
constexpr float kC6 = kC2 - kC2MinusC6;

constexpr float kOutputBias = 128.0f * 8.0f;

// Whether to reproduce a bug from SDK THP that swaps columns 3 and 4 in _quarterIDCT
constexpr bool kBuggyQuarterIdct = false;

bool read_segment(aurora::ByteReader& reader, const u8*& data, size_t& size) noexcept {
  u16 encodedSize = 0;
  if (!reader.try_read(encodedSize) || encodedSize < 2) {
    return false;
  }

  std::span<const u8> bytes;
  if (!reader.try_take(encodedSize - 2, bytes)) {
    return false;
  }
  data = bytes.data();
  size = bytes.size();
  return true;
}

struct QuantizationTable {
  std::array<float, 64> values{};
  bool valid = false;
};

struct HuffmanTable {
  std::array<u8, 17> counts{};
  std::array<u16, 17> firstCodes{};
  std::array<u16, 17> symbolOffsets{};
  std::array<u8, 256> symbols{};
  // Lookahead: top kLookBits of the stream -> (length << 8 | symbol); 0 = code longer than kLookBits.
  std::array<u16, 1 << 9> lookahead{};
  bool valid = false;
};
constexpr u32 kLookBits = 9;

struct Component {
  u8 quantizationTable = 0;
  u8 dcTable = 0;
  u8 acTable = 0;
  s32 predictedDc = 0;
};

struct DecodeContext {
  std::array<QuantizationTable, 3> quantizationTables{};
  std::array<HuffmanTable, 4> huffmanTables{};
  std::array<Component, 3> components{};
  u16 width = 0;
  u16 height = 0;
  u16 restartInterval = 0;
  size_t scanOffset = 0;
};

s32 parse_quantization_tables(const u8* data, size_t size, DecodeContext& context) noexcept {
  size_t position = 0;
  while (position < size) {
    if (size - position < 65) {
      return kBadSyntax;
    }
    const u8 descriptor = data[position++];
    const u8 precision = descriptor >> 4;
    const u8 id = descriptor & 15;
    if (precision != 0 || id >= context.quantizationTables.size()) {
      return kBadSyntax;
    }

    std::array<float, 64> natural{};
    for (size_t i = 0; i < 64; ++i) {
      natural[kNaturalOrder[i]] = static_cast<float>(data[position++]);
    }
    QuantizationTable& table = context.quantizationTables[id];
    for (size_t row = 0; row < 8; ++row) {
      for (size_t column = 0; column < 8; ++column) {
        const size_t index = row * 8 + column;
        table.values[index] =
            static_cast<float>(static_cast<double>(natural[index]) * kAanScale[row] * kAanScale[column]);
      }
    }
    table.valid = true;
  }
  return 0;
}

s32 parse_frame_header(const u8* data, size_t size, DecodeContext& context) noexcept {
  if (size < 6) {
    return kBadSyntax;
  }
  if (data[0] != 8) {
    return kBadPrecision;
  }
  context.height = static_cast<u16>((static_cast<u16>(data[1]) << 8) | data[2]);
  context.width = static_cast<u16>((static_cast<u16>(data[3]) << 8) | data[4]);
  if (data[5] != 3) {
    return kBadComponentCount;
  }
  if (size < 15 || context.width == 0 || context.height == 0) {
    return kBadSyntax;
  }

  for (size_t i = 0; i < 3; ++i) {
    const u8 sampling = data[7 + i * 3];
    if ((i == 0 && sampling != 0x22) || (i != 0 && sampling != 0x11)) {
      return kBadSampling;
    }
    const u8 table = data[8 + i * 3];
    if (table >= context.quantizationTables.size()) {
      return kBadSyntax;
    }
    context.components[i].quantizationTable = table;
  }
  return 0;
}

s32 parse_huffman_tables(const u8* data, size_t size, DecodeContext& context) noexcept {
  size_t position = 0;
  while (position < size) {
    if (size - position < 17) {
      return kBadSyntax;
    }
    const u8 descriptor = data[position++];
    const u8 tableClass = descriptor >> 4;
    const u8 id = descriptor & 15;
    if (tableClass > 1 || id > 1) {
      return kBadSyntax;
    }

    HuffmanTable table{};
    size_t symbolCount = 0;
    for (size_t length = 1; length <= 16; ++length) {
      table.counts[length] = data[position++];
      symbolCount += table.counts[length];
    }
    if (symbolCount > table.symbols.size() || symbolCount > size - position) {
      return kBadSyntax;
    }
    std::copy_n(data + position, symbolCount, table.symbols.begin());
    position += symbolCount;

    u32 code = 0;
    u16 symbolOffset = 0;
    for (size_t length = 1; length <= 16; ++length) {
      const u32 count = table.counts[length];
      if (code + count > (u32{1} << length)) {
        return kBadSyntax;
      }
      table.firstCodes[length] = static_cast<u16>(code);
      table.symbolOffsets[length] = symbolOffset;
      code = (code + count) << 1;
      symbolOffset = static_cast<u16>(symbolOffset + count);
    }
    for (u32 length = 1; length <= kLookBits; ++length) {
      for (u32 i = 0; i < table.counts[length]; ++i) {
        const u32 first = (table.firstCodes[length] + i) << (kLookBits - length);
        const u16 entry = static_cast<u16>((length << 8) | table.symbols[table.symbolOffsets[length] + i]);
        std::fill_n(table.lookahead.begin() + first, u32{1} << (kLookBits - length), entry);
      }
    }
    table.valid = true;
    context.huffmanTables[id * 2 + tableClass] = table;
  }
  return 0;
}

s32 parse_scan_header(const u8* data, size_t size, DecodeContext& context) noexcept {
  if (size < 1) {
    return kBadSyntax;
  }
  if (data[0] != 3) {
    return kBadComponentCount;
  }
  if (size < 10) {
    return kBadSyntax;
  }

  for (size_t i = 0; i < 3; ++i) {
    const u8 selectors = data[2 + i * 2];
    const u8 dcTable = selectors >> 4;
    const u8 acTable = selectors & 15;
    if (dcTable > 1 || acTable > 1 || !context.huffmanTables[dcTable * 2].valid ||
        !context.huffmanTables[acTable * 2 + 1].valid) {
      return kMissingHuffmanTable;
    }
    context.components[i].dcTable = dcTable;
    context.components[i].acTable = acTable;
    context.components[i].predictedDc = 0;
  }
  if (data[7] != 0 || data[8] != 63 || data[9] != 0) {
    return kBadSyntax;
  }
  return 0;
}

s32 parse_headers(const void* file, DecodeContext& context) noexcept {
  auto reader = aurora::ByteReader::unbounded(file);
  while (true) {
    u8 prefix = 0;
    if (!reader.try_read(prefix) || prefix != 0xFF) {
      return kBadSyntax;
    }

    u8 marker = 0;
    do {
      if (!reader.try_read(marker)) {
        return kBadSyntax;
      }
    } while (marker == 0xFF);

    if (marker == 0xD8) {
      continue;
    }
    if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
      const u8* ignored = nullptr;
      size_t ignoredSize = 0;
      if (!read_segment(reader, ignored, ignoredSize)) {
        return kBadSyntax;
      }
      continue;
    }

    const u8* segment = nullptr;
    size_t segmentSize = 0;
    switch (marker) {
    case 0xC0: {
      if (!read_segment(reader, segment, segmentSize)) {
        return kBadSyntax;
      }
      if (const s32 result = parse_frame_header(segment, segmentSize, context); result != 0) {
        return result;
      }
      break;
    }
    case 0xC4: {
      if (!read_segment(reader, segment, segmentSize)) {
        return kBadSyntax;
      }
      if (const s32 result = parse_huffman_tables(segment, segmentSize, context); result != 0) {
        return result;
      }
      break;
    }
    case 0xDA: {
      if (!read_segment(reader, segment, segmentSize)) {
        return kBadSyntax;
      }
      if (const s32 result = parse_scan_header(segment, segmentSize, context); result != 0) {
        return result;
      }
      context.scanOffset = reader.offset();
      return 0;
    }
    case 0xDB: {
      if (!read_segment(reader, segment, segmentSize)) {
        return kBadSyntax;
      }
      const s32 result = parse_quantization_tables(segment, segmentSize, context);
      if (result != 0) {
        return result;
      }
      break;
    }
    case 0xDD: {
      if (!read_segment(reader, segment, segmentSize) || segmentSize != 2) {
        return kBadSyntax;
      }
      context.restartInterval = read_bits<u16>(segment);
      break;
    }
    default:
      return kUnsupportedMarker;
    }
  }
}

// Bits are consumed MSB first from the raw bytes (THP has no 0xFF stuffing). The buffer is left-aligned
// and refilled four bytes at a time, so it reads at most 4 bytes past the last bit consumed.
class BitReader {
public:
  BitReader(const u8* data, size_t byteOffset) noexcept : mData{data + byteOffset} {}

  // Makes at least 32 bits available.
  void refill() noexcept {
    if (mCount < 32) {
      const u64 word = (u64{mData[0]} << 24) | (u64{mData[1]} << 16) | (u64{mData[2]} << 8) | u64{mData[3]};
      mBuffer |= word << (32 - mCount);
      mData += 4;
      mCount += 32;
    }
  }

  // Next `count` (1..32) bits without consuming; call refill() first.
  u32 peek(u32 count) const noexcept { return static_cast<u32>(mBuffer >> (64 - count)); }
  void skip(u32 count) noexcept {
    mBuffer <<= count;
    mCount -= count;
  }

  u32 read(u8 count) noexcept {
    if (count == 0) {
      return 0;
    }
    refill();
    const u32 value = peek(count);
    skip(count);
    return value;
  }

  void byte_align() noexcept { skip(mCount & 7); }

private:
  const u8* mData;
  u64 mBuffer = 0; // left-aligned
  u32 mCount = 0;
};

bool decode_huffman(BitReader& reader, const HuffmanTable& table, u8& symbol) noexcept {
  reader.refill();
  const u16 entry = table.lookahead[reader.peek(kLookBits)];
  if (entry != 0) {
    reader.skip(entry >> 8);
    symbol = static_cast<u8>(entry);
    return true;
  }
  const u32 bits = reader.peek(16);
  for (size_t length = 1; length <= 16; ++length) {
    const u32 code = bits >> (16 - length);
    const u32 firstCode = table.firstCodes[length];
    const u32 count = table.counts[length];
    if (code >= firstCode && code - firstCode < count) {
      symbol = table.symbols[table.symbolOffsets[length] + code - firstCode];
      reader.skip(static_cast<u32>(length));
      return true;
    }
  }
  reader.skip(16);
  return false;
}

s32 extend_value(u32 value, u8 bitCount) noexcept {
  if (bitCount != 0 && value < (u32{1} << (bitCount - 1))) {
    return static_cast<s32>(value) - static_cast<s32>(u32{1} << bitCount) + 1;
  }
  return static_cast<s32>(value);
}

bool decode_block(BitReader& reader, DecodeContext& context, size_t componentIndex,
                  std::array<s16, 64>& block) noexcept {
  block.fill(0);
  Component& component = context.components[componentIndex];
  const HuffmanTable& dcTable = context.huffmanTables[component.dcTable * 2];
  const HuffmanTable& acTable = context.huffmanTables[component.acTable * 2 + 1];

  u8 bitCount = 0;
  if (!decode_huffman(reader, dcTable, bitCount) || bitCount > 16) {
    return false;
  }
  const u32 encodedDifference = reader.read(bitCount);
  component.predictedDc += extend_value(encodedDifference, bitCount);
  block[0] = static_cast<s16>(component.predictedDc);

  size_t coefficient = 1;
  while (coefficient < 64) {
    u8 runAndSize = 0;
    if (!decode_huffman(reader, acTable, runAndSize)) {
      return false;
    }
    const u8 run = runAndSize >> 4;
    bitCount = runAndSize & 15;
    if (bitCount == 0) {
      if (run == 15) {
        coefficient += 16;
        continue;
      }
      break;
    }

    coefficient += run;
    if (coefficient >= 64) {
      return false;
    }
    const u32 encodedValue = reader.read(bitCount);
    block[kNaturalOrder[coefficient]] = static_cast<s16>(extend_value(encodedValue, bitCount));
    ++coefficient;
  }
  return true;
}

// The IDCT stages are templates over the element type: float for the row pass and Lanes (eight columns at once,
// plain loops the compiler can vectorise) for the column pass. Both do the same operations in the same order.
// fmaf without a libm call: float products are exact in double, so one double add and one rounding to float
// gives the fused result (bar a double-rounding tie). Hardware FMA is used where the target has it.
inline float fmaf_exact(float a, float b, float c) noexcept {
#ifdef __FMA__
  return std::fma(a, b, c);
#else
  return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
#endif
}

struct Lanes {
  std::array<float, 8> v;
};
inline Lanes operator+(const Lanes& a, const Lanes& b) noexcept {
  Lanes r;
  for (size_t i = 0; i < 8; ++i) {
    r.v[i] = a.v[i] + b.v[i];
  }
  return r;
}
inline Lanes operator-(const Lanes& a, const Lanes& b) noexcept {
  Lanes r;
  for (size_t i = 0; i < 8; ++i) {
    r.v[i] = a.v[i] - b.v[i];
  }
  return r;
}
inline Lanes operator+(const Lanes& a, float b) noexcept {
  Lanes r;
  for (size_t i = 0; i < 8; ++i) {
    r.v[i] = a.v[i] + b;
  }
  return r;
}
inline Lanes operator*(const Lanes& a, float b) noexcept {
  Lanes r;
  for (size_t i = 0; i < 8; ++i) {
    r.v[i] = a.v[i] * b;
  }
  return r;
}
inline Lanes operator-(const Lanes& a) noexcept {
  Lanes r;
  for (size_t i = 0; i < 8; ++i) {
    r.v[i] = -a.v[i];
  }
  return r;
}
inline Lanes fmaf_exact(const Lanes& a, float b, const Lanes& c) noexcept {
  Lanes r;
  for (size_t i = 0; i < 8; ++i) {
    r.v[i] = fmaf_exact(a.v[i], b, c.v[i]);
  }
  return r;
}

template <class T> struct EvenHalfT {
  T out0, out1, out2, out3;
};

template <class T> struct OddHalfT {
  T out7, out6, out5, out4;
};

using FloatRow = std::array<float, 8>;
using EvenHalf = EvenHalfT<float>;
using OddHalf = OddHalfT<float>;

template <class T> EvenHalfT<T> aan_even(const T& sum04, const T& dif04, const T& sum26, const T& dif26) noexcept {
  const T rotated = fmaf_exact(dif26, kSqrt2, -sum26);
  return {sum04 + sum26, dif04 + rotated, dif04 - rotated, sum04 - sum26};
}

template <class T> OddHalfT<T> aan_odd(const T& z10, const T& z11, const T& z12, const T& z13) noexcept {
  const T out7 = z11 + z13;
  const T z5 = (z10 + z12) * kC2;
  const T out6 = fmaf_exact(-z10, kC2PlusC6, z5) - out7;
  const T out5 = fmaf_exact(z11 - z13, kSqrt2, -out6);
  const T out4 = fmaf_exact(-z12, kC2MinusC6, z5) - out5;
  return {out7, out6, out5, out4};
}

template <class T> std::array<T, 8> combine(const EvenHalfT<T>& even, const OddHalfT<T>& odd) noexcept {
  return {
      even.out0 + odd.out7, even.out1 + odd.out6, even.out2 + odd.out5, even.out3 + odd.out4,
      even.out3 - odd.out4, even.out2 - odd.out5, even.out1 - odd.out6, even.out0 - odd.out7,
  };
}

FloatRow transform_row(const s16* coefficients, const float* quant) noexcept {
  const float x0 = static_cast<float>(coefficients[0]) * quant[0];
  const float x1 = static_cast<float>(coefficients[1]) * quant[1];
  const float x2 = static_cast<float>(coefficients[2]) * quant[2];
  const float x3 = static_cast<float>(coefficients[3]) * quant[3];
  const auto c4 = static_cast<float>(coefficients[4]);
  const auto c5 = static_cast<float>(coefficients[5]);
  const auto c6 = static_cast<float>(coefficients[6]);
  const auto c7 = static_cast<float>(coefficients[7]);
  const EvenHalf even = aan_even(fmaf_exact(c4, quant[4], x0), fmaf_exact(-c4, quant[4], x0), fmaf_exact(c6, quant[6], x2),
                                 fmaf_exact(-c6, quant[6], x2));
  const OddHalf odd = aan_odd(fmaf_exact(c5, quant[5], -x3), fmaf_exact(c7, quant[7], x1), fmaf_exact(-c7, quant[7], x1),
                              fmaf_exact(c5, quant[5], x3));
  return combine(even, odd);
}

FloatRow transform_row_low4(const FloatRow& x) noexcept {
  const float sum02 = x[0] + x[2];
  const float dif02 = x[0] - x[2];
  const EvenHalf even{sum02, fmaf_exact(x[2], kSqrt2, dif02), fmaf_exact(-x[2], kSqrt2, sum02), dif02};
  const OddHalf odd = aan_odd(-x[3], x[1], x[1], x[3]);
  return combine(even, odd);
}

FloatRow transform_row_dc_ac(float dc, float ac) noexcept {
  const float a1 = fmaf_exact(ac, kC2, -ac);
  const float a2 = fmaf_exact(ac, kSqrt2, -a1);
  const float a3 = fmaf_exact(-ac, kC6, a2);
  if constexpr (kBuggyQuarterIdct) {
    return {dc + ac, dc + a1, dc + a2, dc + a3, dc - a3, dc - a2, dc - a1, dc - ac};
  }
  return {dc + ac, dc + a1, dc + a2, dc - a3, dc + a3, dc - a2, dc - a1, dc - ac};
}

template <class T> std::array<T, 8> transform_column(const std::array<T, 8>& x) noexcept {
  const EvenHalfT<T> even = aan_even<T>((x[0] + x[4]) + kOutputBias, (x[0] - x[4]) + kOutputBias, x[2] + x[6], x[2] - x[6]);
  const OddHalfT<T> odd = aan_odd<T>(x[5] - x[3], x[1] + x[7], x[1] - x[7], x[5] + x[3]);
  return combine(even, odd);
}

size_t row_extent(const s16* coefficients) noexcept {
  size_t extent = 8;
  while (extent > 0 && coefficients[extent - 1] == 0) {
    --extent;
  }
  return extent;
}

std::array<u8, 64> inverse_dct(const std::array<s16, 64>& coefficients,
                               const QuantizationTable& quantization) noexcept {
  std::array<float, 64> workspace{};
  for (size_t row = 0; row < 8; ++row) {
    const s16* rowCoefficients = &coefficients[row * 8];
    const float* rowQuant = &quantization.values[row * 8];
    FloatRow transformed{};
    switch (row_extent(rowCoefficients)) {
    case 0:
    case 1:
      transformed.fill(static_cast<float>(rowCoefficients[0]) * rowQuant[0]);
      break;
    case 2:
      transformed = transform_row_dc_ac(static_cast<float>(rowCoefficients[0]) * rowQuant[0],
                                        static_cast<float>(rowCoefficients[1]) * rowQuant[1]);
      break;
    case 3:
    case 4: {
      FloatRow dequantized{};
      for (size_t column = 0; column < 4; ++column) {
        dequantized[column] = static_cast<float>(rowCoefficients[column]) * rowQuant[column];
      }
      transformed = transform_row_low4(dequantized);
      break;
    }
    default:
      transformed = transform_row(rowCoefficients, rowQuant);
      break;
    }
    std::copy(transformed.begin(), transformed.end(), workspace.begin() + row * 8);
  }

  std::array<Lanes, 8> input;
  for (size_t row = 0; row < 8; ++row) {
    std::copy_n(workspace.begin() + row * 8, 8, input[row].v.begin());
  }
  const std::array<Lanes, 8> transformed = transform_column(input);
  std::array<u8, 64> output{};
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 8; ++column) {
      const float scaled = transformed[row].v[column] * 0.125f;
      output[row * 8 + column] = scaled <= 0.0f ? 0 : scaled >= 255.0f ? 255 : static_cast<u8>(scaled);
    }
  }
  return output;
}

void write_block(u8* output, u16 width, u16 height, u16 blockX, u16 blockY, const std::array<u8, 64>& pixels) noexcept {
  if (blockX >= width) {
    return;
  }
  const size_t tilesPerRow = (width + 7) / 8;
  // blockX is a multiple of 8, so a block row is 8 contiguous bytes of one tile.
  const size_t columns = std::min<size_t>(8, width - blockX);
  for (u16 row = 0; row < 8 && blockY + row < height; ++row) {
    const u16 y = blockY + row;
    const size_t tile = static_cast<size_t>(y / 4) * tilesPerRow + blockX / 8;
    std::copy_n(&pixels[row * 8], columns, output + tile * 32 + static_cast<size_t>(y & 3) * 8);
  }
}

bool decode_and_write_block(BitReader& reader, DecodeContext& context, size_t component, u8* output, u16 width,
                            u16 height, u16 x, u16 y) noexcept {
  std::array<s16, 64> coefficients{};
  if (!decode_block(reader, context, component, coefficients)) {
    return false;
  }
  const QuantizationTable& quantization = context.quantizationTables[context.components[component].quantizationTable];
  if (!quantization.valid) {
    return false;
  }
  write_block(output, width, height, x, y, inverse_dct(coefficients, quantization));
  return true;
}
} // namespace

extern "C" {
BOOL THPInit(void) { return TRUE; }

s32 THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void*) {
  if (file == nullptr) {
    return kNoInput;
  }
  if (tileY == nullptr || tileU == nullptr || tileV == nullptr) {
    return kNoOutput;
  }

  DecodeContext context{};
  const s32 headerResult = parse_headers(file, context);
  if (headerResult != 0) {
    return headerResult;
  }

  const u16 chromaWidth = static_cast<u16>((context.width + 1) / 2);
  const u16 chromaHeight = static_cast<u16>((context.height + 1) / 2);

  BitReader bits{static_cast<const uint8_t*>(file), context.scanOffset};
  const u16 mcuColumns = static_cast<u16>((context.width + 15) / 16);
  const u16 mcuRows = static_cast<u16>((context.height + 15) / 16);
  u32 restartCount = 0;
  for (u16 mcuY = 0; mcuY < mcuRows; ++mcuY) {
    for (u16 mcuX = 0; mcuX < mcuColumns; ++mcuX) {
      const u16 lumaX = static_cast<u16>(mcuX * 16);
      const u16 lumaY = static_cast<u16>(mcuY * 16);
      if (!decode_and_write_block(bits, context, 0, static_cast<uint8_t*>(tileY), context.width, context.height, lumaX,
                                  lumaY) ||
          !decode_and_write_block(bits, context, 0, static_cast<uint8_t*>(tileY), context.width, context.height,
                                  static_cast<u16>(lumaX + 8), lumaY) ||
          !decode_and_write_block(bits, context, 0, static_cast<uint8_t*>(tileY), context.width, context.height, lumaX,
                                  static_cast<u16>(lumaY + 8)) ||
          !decode_and_write_block(bits, context, 0, static_cast<uint8_t*>(tileY), context.width, context.height,
                                  static_cast<u16>(lumaX + 8), static_cast<u16>(lumaY + 8)) ||
          !decode_and_write_block(bits, context, 1, static_cast<uint8_t*>(tileU), chromaWidth, chromaHeight,
                                  static_cast<u16>(mcuX * 8), static_cast<u16>(mcuY * 8)) ||
          !decode_and_write_block(bits, context, 2, static_cast<uint8_t*>(tileV), chromaWidth, chromaHeight,
                                  static_cast<u16>(mcuX * 8), static_cast<u16>(mcuY * 8))) {
        return kBadSyntax;
      }

      if (context.restartInterval != 0 && ++restartCount == context.restartInterval) {
        bits.byte_align();
        restartCount = 0;
        for (Component& component : context.components) {
          component.predictedDc = 0;
        }
      }
    }
  }
  return 0;
}
}
