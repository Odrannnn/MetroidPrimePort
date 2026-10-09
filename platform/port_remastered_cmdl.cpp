// See platform/include/port_remastered_cmdl.h for what this is and why it exists.
// A port of retrotool's retrolib cmdl/mtrl/chunk/rfrm readers (build/mpr-tools/
// retrotool/lib/src/format/) and of the decoding half of `retrotool cmdl convert`.

#include "port_remastered_cmdl.h"
#include "port_bytes.h"

#include <cstring>
#include <functional>

namespace PortRemastered {
namespace {

// Every scalar in the format is little endian and the vertex data is interleaved
// without guarantees worth trusting, so all reads go through these.
using port::ReadLE16;
using port::ReadLE32;
using port::ReadLE64;
using port::ReadLEFloat;

// An IEEE half, the width most of the Remastered vertex data uses, widened to
// float. Exact, so the result matches any other correct decoder bit for bit.
float HalfToFloat(uint16_t bits) {
  const uint32_t sign = uint32_t(bits & 0x8000) << 16;
  const uint32_t exponent = uint32_t((bits >> 10) & 0x1F);
  const uint32_t mantissa = uint32_t(bits & 0x3FF);
  uint32_t out = sign;
  if (exponent == 0) {
    if (mantissa != 0) {
      // Subnormal: shift the mantissa up until it is normalised, then pay for the
      // shifts in the exponent (113 is 127 - 15 + 1, the +1 for the missing bit).
      uint32_t shifted = mantissa;
      uint32_t shift = 0;
      while ((shifted & 0x400) == 0) {
        shifted <<= 1;
        ++shift;
      }
      out |= (113 - shift) << 23;
      out |= (shifted & 0x3FF) << 13;
    }
  } else if (exponent == 0x1F) {
    out |= 0x7F800000u | (mantissa << 13);
  } else {
    out |= (exponent + (127 - 15)) << 23;
    out |= mantissa << 13;
  }
  float value = 0.0f;
  std::memcpy(&value, &out, sizeof(value));
  return value;
}

constexpr uint32_t FourCc(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) |
         uint32_t(uint8_t(d));
}

const uint32_t kFormRFRM = FourCc('R', 'F', 'R', 'M');
const uint32_t kFormFoot = FourCc('F', 'O', 'O', 'T');
const uint32_t kChunkMeta = FourCc('M', 'E', 'T', 'A');
const uint32_t kFormCmdl = FourCc('C', 'M', 'D', 'L');
const uint32_t kFormSmdl = FourCc('S', 'M', 'D', 'L');
const uint32_t kFormWmdl = FourCc('W', 'M', 'D', 'L');
const uint32_t kChunkHead = FourCc('H', 'E', 'A', 'D');
const uint32_t kChunkWdhd = FourCc('W', 'D', 'H', 'D');
const uint32_t kChunkSkhd = FourCc('S', 'K', 'H', 'D');
const uint32_t kChunkMtrl = FourCc('M', 'T', 'R', 'L');
const uint32_t kChunkMesh = FourCc('M', 'E', 'S', 'H');
const uint32_t kChunkVbuf = FourCc('V', 'B', 'U', 'F');
const uint32_t kChunkIbuf = FourCc('I', 'B', 'U', 'F');
const uint32_t kChunkGpu = FourCc('G', 'P', 'U', ' ');

// Material parameter types. The file stores these as FourCCs (binrw reads the Rust
// enums by magic), so the discriminants in cmdl.rs are never on disc.
const uint32_t kDataTexture = FourCc('T', 'X', 'T', 'R');
const uint32_t kDataColor = FourCc('C', 'O', 'L', 'R');
const uint32_t kDataScalar = FourCc('S', 'C', 'L', 'R');
const uint32_t kDataInt1 = FourCc('I', 'N', 'T', '1');
const uint32_t kDataInt4 = FourCc('I', 'N', 'T', '4');
const uint32_t kDataMatrix4 = FourCc('M', 'A', 'T', '4');
const uint32_t kDataComplex = FourCc('C', 'P', 'L', 'X');

// The EVertexComponent values the port names; the rest go to ModelAttribute.
const uint32_t kComponentPosition = 0;
const uint32_t kComponentNormal = 1;
const uint32_t kComponentTangent0 = 2;
const uint32_t kComponentTexCoord0 = 5;
const uint32_t kComponentColor = 9;
const uint32_t kComponentBoneIndices = 10;
const uint32_t kComponentBoneWeights = 11;

const char* ComponentName(uint32_t component) {
  static const char* const kNames[] = {
      "POSITION",    "NORMAL",     "TANGENT_0",       "TANGENT_1",         "TANGENT_2",
      "TEXCOORD_0",  "TEXCOORD_1", "TEXCOORD_2",      "TEXCOORD_3",        "COLOR",
      "BONE_INDICES", "BONE_WEIGHTS", "BAKED_LIGHTING_COORD", "BAKED_LIGHTING_TANGENT",
      "VERT_INSTANCE_PARAMS", "VERT_INSTANCE_COLOR", "VERT_TRANSFORM_0", "VERT_TRANSFORM_1",
      "VERT_TRANSFORM_2", "CURRENT_POSITION", "VERT_INSTANCE_OPACITY_PARAMS",
      "VERT_INSTANCE_COLOR_INDEXING_PARAMS", "VERT_INSTANCE_OPACITY_INDEXING_PARAMS",
      "VERT_INSTANCE_PAINT_PARAMS", "BAKED_LIGHTING_LOOKUP", "MATERIAL_CHOICE_0",
      "MATERIAL_CHOICE_1", "MATERIAL_CHOICE_2", "MATERIAL_CHOICE_3"};
  if (component < sizeof(kNames) / sizeof(kNames[0])) {
    return kNames[component];
  }
  return "UNKNOWN";
}

// A bounds checked little endian cursor over one region of the file: everything
// the parsers need in order not to read past the end, and it keeps the first
// failure's message so the caller can report something useful.
class Cursor {
public:
  Cursor(const uint8_t* data, size_t size, std::string* error)
      : m_data(data), m_size(size), m_error(error) {}

  size_t offset() const { return m_offset; }
  size_t remaining() const { return m_offset <= m_size ? m_size - m_offset : 0; }
  bool ok() const { return m_ok; }

  // Steps to a position already known to be in range, so a nested structure can be
  // read after its header.
  void Seek(size_t offset) {
    if (offset > m_size) {
      Fail("offset past the end of the data");
      return;
    }
    m_offset = offset;
  }

  const uint8_t* Peek(size_t count) const { return count <= m_size - m_offset ? m_data + m_offset : nullptr; }

  bool Skip(size_t count) { return Bytes(count) != nullptr; }

  const uint8_t* Bytes(size_t count) {
    if (!m_ok) {
      return nullptr;
    }
    if (count > m_size - m_offset) {
      Fail("read of " + std::to_string(count) + " bytes past the end of the data");
      return nullptr;
    }
    const uint8_t* out = m_data + m_offset;
    m_offset += count;
    return out;
  }

  uint8_t U8() {
    const uint8_t* p = Bytes(1);
    return p ? p[0] : 0;
  }

  uint16_t U16() {
    const uint8_t* p = Bytes(2);
    return p ? ReadLE16(p) : 0;
  }

  uint32_t U32() {
    const uint8_t* p = Bytes(4);
    return p ? ReadLE32(p) : 0;
  }

  uint64_t U64() {
    const uint8_t* p = Bytes(8);
    return p ? ReadLE64(p) : 0;
  }

  int32_t I32() {
    const uint8_t* p = Bytes(4);
    return p ? int32_t(ReadLE32(p)) : 0;
  }

  float F32() {
    const uint8_t* p = Bytes(4);
    return p ? ReadLEFloat(p) : 0.0f;
  }

  ModelUuid Uuid() {
    ModelUuid id{};
    const uint8_t* p = Bytes(16);
    if (p) {
      std::memcpy(id.data(), p, 16);
    }
    return id;
  }

  uint32_t FourCC() {
    const uint8_t* p = Bytes(4);
    return p ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3])
             : 0;
  }

  // Records the first failure only; anything after it is a consequence.
  bool Fail(const std::string& what) {
    if (m_ok) {
      m_ok = false;
      if (m_error) {
        *m_error = "remastered model: " + what + " at offset " + std::to_string(m_offset);
      }
    }
    return false;
  }

private:
  const uint8_t* m_data = nullptr;
  size_t m_size = 0;
  size_t m_offset = 0;
  std::string* m_error = nullptr;
  bool m_ok = true;
};

// An RFRM form header or a plain chunk header. Both start with a FourCC and a
// length; a form adds versions and an id, a chunk a skip, and a form is told apart
// by its magic (retrotool's rfrm.rs and chunk.rs).
struct BlockHeader {
  uint32_t magic = 0;  // 'RFRM' for a form, the chunk id for a chunk
  uint32_t id = 0;     // the form's id; for a chunk, same as magic
  uint32_t readerVersion = 0;
  uint32_t writerVersion = 0;
  size_t body = 0;
  size_t end = 0;
};

bool ReadBlockHeader(Cursor& cursor, BlockHeader& out) {
  const uint8_t* peek = cursor.Peek(4);
  if (peek == nullptr) {
    return cursor.Fail("truncated block header");
  }
  out.magic = (uint32_t(peek[0]) << 24) | (uint32_t(peek[1]) << 16) | (uint32_t(peek[2]) << 8) |
              uint32_t(peek[3]);
  if (out.magic == kFormRFRM) {
    cursor.Skip(4);
    const uint64_t size = cursor.U64();
    cursor.U64();  // unk
    out.id = cursor.FourCC();
    out.readerVersion = cursor.U32();
    out.writerVersion = cursor.U32();
    out.body = cursor.offset();
    if (!cursor.ok()) {
      return false;
    }
    if (size > cursor.remaining()) {
      return cursor.Fail("form longer than the data");
    }
    out.end = out.body + size_t(size);
    return true;
  }
  cursor.Skip(4);
  const uint64_t size = cursor.U64();
  cursor.U32();  // unk
  const uint64_t skip = cursor.U64();
  out.id = out.magic;
  if (!cursor.ok()) {
    return false;
  }
  if (skip > cursor.remaining() || size > cursor.remaining() - size_t(skip)) {
    return cursor.Fail("chunk longer than the data");
  }
  out.body = cursor.offset() + size_t(skip);
  out.end = out.body + size_t(size);
  return true;
}

// retrotool's lzss.rs: mode is the log2 of the group's byte count, a clear header
// bit copies a group verbatim and a set bit copies a back reference.
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
      if (inSize - inCur < 2) {
        return false;
      }
      const size_t count = size_t(in[inCur] >> 4) + (4 - size_t(mode));
      const size_t length = (size_t(in[inCur] & 0x0F) * 256 + size_t(in[inCur + 1])) << (mode - 1);
      inCur += 2;
      if (length > outCur) {
        return false;
      }
      const size_t total = count * groupLen;
      if (outSize - outCur < total) {
        return false;
      }
      // One byte at a time: a back reference may overlap the bytes it is writing,
      // which is how the format spells a run of the same byte.
      for (size_t i = 0; i < total; ++i) {
        out[outCur + i] = out[outCur - length + i];
      }
      outCur += total;
    }
    header = uint8_t(header << 1);
    group -= 1;
  }
  return outCur == outSize;
}

// The largest decompressed vertex or index buffer accepted, well above anything the
// game's models hold (a few megabytes).
const uint32_t kMaxBufferSize = 256u * 1024u * 1024u;

// retrotool's decompress_buffer over one vertex or index buffer: a four byte mode,
// where a zero one means the rest of the block is stored as is. That shortcut
// takes the block whatever its length, and the vertex buffers rely on it.
bool DecompressGpuBuffer(const uint8_t* in, size_t inSize, uint32_t destSize, std::vector<uint8_t>& out,
                         std::string& error) {
  if (inSize < 4) {
    error = "remastered model: compressed buffer is shorter than its mode";
    return false;
  }
  const uint8_t* data = in + 4;
  const size_t dataSize = inSize - 4;
  if (in[0] == 0 && in[1] == 0 && in[2] == 0 && in[3] == 0) {
    out.assign(data, data + dataSize);
    return true;
  }
  const uint32_t mode = ReadLE32(in);
  // The decompressed length is file data and is used to size an allocation, so it is
  // capped: the largest buffer in the game's models is a few megabytes.
  if (destSize > kMaxBufferSize) {
    error = "remastered model: buffer claims " + std::to_string(destSize) + " bytes";
    return false;
  }
  out.assign(destSize, 0);
  if (mode == 0) {
    if (dataSize != out.size()) {
      error = "remastered model: stored buffer is " + std::to_string(dataSize) +
              " bytes, the metadata says " + std::to_string(destSize);
      return false;
    }
    std::memcpy(out.data(), data, dataSize);
    return true;
  }
  if (mode > 3) {
    error = "remastered model: unsupported buffer compression mode " + std::to_string(mode);
    return false;
  }
  if (!LzssDecompress(mode, data, dataSize, out.data(), out.size())) {
    error = "remastered model: LZSS mode " + std::to_string(mode) + " decompression failed";
    return false;
  }
  return true;
}

// Where one compressed buffer sits: a window into one of the file's read buffers,
// and how big it is once decompressed.
struct MetaBuffer {
  uint32_t readIndex = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t destSize = 0;
};

struct MetaReadBuffer {
  uint32_t offset = 0;
  uint32_t size = 0;
};

// The META blob: which parts of the file hold the vertex and index buffers.
bool ParseMeta(Cursor& cursor, std::vector<MetaReadBuffer>& readBuffers,
               std::vector<MetaBuffer>& vertexBuffers, std::vector<MetaBuffer>& indexBuffers) {
  cursor.U32();  // unk
  cursor.U32();  // gpu_offset
  // Every count comes out of the file, so each is bounded by what is left before
  // anything is sized from it.
  const uint32_t readCount = cursor.U32();
  if (!cursor.ok() || readCount > cursor.remaining() / 8) {
    return cursor.Fail("implausible read buffer count");
  }
  readBuffers.resize(readCount);
  for (uint32_t i = 0; i < readCount; ++i) {
    readBuffers[i].size = cursor.U32();
    readBuffers[i].offset = cursor.U32();
  }
  const uint32_t vertexCount = cursor.U32();
  if (!cursor.ok() || vertexCount > cursor.remaining() / 16) {
    return cursor.Fail("implausible vertex buffer count");
  }
  vertexBuffers.resize(vertexCount);
  for (uint32_t i = 0; i < vertexCount; ++i) {
    vertexBuffers[i].readIndex = cursor.U32();
    vertexBuffers[i].offset = cursor.U32();
    vertexBuffers[i].size = cursor.U32();
    vertexBuffers[i].destSize = cursor.U32();
  }
  const uint32_t indexCount = cursor.U32();
  if (!cursor.ok() || indexCount > cursor.remaining() / 16) {
    return cursor.Fail("implausible index buffer count");
  }
  indexBuffers.resize(indexCount);
  for (uint32_t i = 0; i < indexCount; ++i) {
    indexBuffers[i].readIndex = cursor.U32();
    indexBuffers[i].offset = cursor.U32();
    indexBuffers[i].size = cursor.U32();
    indexBuffers[i].destSize = cursor.U32();
  }
  return cursor.ok();
}

bool DecompressBuffers(const uint8_t* data, size_t size, const std::vector<MetaReadBuffer>& readBuffers,
                       const std::vector<MetaBuffer>& infos, std::vector<std::vector<uint8_t>>& out,
                       std::string& error) {
  out.resize(infos.size());
  for (size_t i = 0; i < infos.size(); ++i) {
    const MetaBuffer& info = infos[i];
    if (info.readIndex >= readBuffers.size()) {
      error = "remastered model: buffer " + std::to_string(i) + " names read buffer " +
              std::to_string(info.readIndex) + " of " + std::to_string(readBuffers.size());
      return false;
    }
    const MetaReadBuffer& read = readBuffers[info.readIndex];
    if (read.offset > size || read.size > size - read.offset) {
      error = "remastered model: read buffer " + std::to_string(info.readIndex) + " is outside the file";
      return false;
    }
    if (info.offset > read.size || info.size > read.size - info.offset) {
      error = "remastered model: buffer " + std::to_string(i) + " is outside its read buffer";
      return false;
    }
    if (!DecompressGpuBuffer(data + read.offset + info.offset, info.size, info.destSize, out[i], error)) {
      return false;
    }
  }
  return true;
}

// The texture token inside a material parameter: an id, and a sampler block unless
// the id is nil, which is how the file says the slot is unused.
void ParseTextureToken(Cursor& cursor, ModelTextureRef& out, uint32_t usage) {
  out.usage = usage;
  out.id = cursor.Uuid();
  bool nil = true;
  for (uint8_t i = 0; i < 16; ++i) {
    if (out.id[i] != 0) {
      nil = false;
      break;
    }
  }
  if (!nil) {
    out.hasUsage = true;
    out.texCoord = cursor.U32();
    out.filter = cursor.I32();
    out.wrapX = cursor.I32();
    out.wrapY = cursor.I32();
    out.wrapZ = cursor.I32();
  }
}

void ParseMaterialData(Cursor& cursor, ModelMaterialData& out) {
  out.usage = cursor.FourCC();
  const uint32_t type = cursor.FourCC();
  switch (type) {
    case kDataTexture:
      out.kind = ModelMaterialData::Kind::Texture;
      ParseTextureToken(cursor, out.texture, out.usage);
      break;
    case kDataColor:
      out.kind = ModelMaterialData::Kind::Color;
      for (int i = 0; i < 4; ++i) {
        out.color[i] = cursor.F32();
      }
      break;
    case kDataScalar:
      out.kind = ModelMaterialData::Kind::Scalar;
      out.scalar = cursor.F32();
      break;
    case kDataInt1:
      out.kind = ModelMaterialData::Kind::Int1;
      out.int1 = cursor.I32();
      break;
    case kDataInt4:
      out.kind = ModelMaterialData::Kind::Int4;
      for (int i = 0; i < 4; ++i) {
        out.int4[i] = cursor.I32();
      }
      break;
    case kDataMatrix4:
      out.kind = ModelMaterialData::Kind::Matrix4;
      for (int i = 0; i < 16; ++i) {
        out.matrix4[i] = cursor.F32();
      }
      break;
    case kDataComplex:
      // retrotool only allows this for the layered texture ids (BCRL, MTLL, NRML);
      // reading it for any id is harmless and keeps the reader from refusing a file
      // over a tag it does not need to judge.
      out.kind = ModelMaterialData::Kind::LayeredTexture;
      out.layeredUnknown = cursor.U32();
      for (int layer = 0; layer < 3; ++layer) {
        for (int c = 0; c < 4; ++c) {
          out.layeredColors[size_t(layer * 4 + c)] = cursor.F32();
        }
      }
      out.layeredFlags = cursor.U8();
      for (int layer = 0; layer < 3; ++layer) {
        ParseTextureToken(cursor, out.layeredTextures[size_t(layer)], out.usage);
      }
      break;
    default:
      cursor.Fail("unknown material data type " + std::to_string(type));
      break;
  }
}

bool ParseMaterials(Cursor& cursor, std::vector<ModelMaterial>& out) {
  cursor.U32();  // unk
  const uint32_t count = cursor.U32();
  if (!cursor.ok() || count > cursor.remaining() / 16) {
    return cursor.Fail("implausible material count");
  }
  out.clear();
  out.reserve(count);
  for (uint32_t i = 0; i < count && cursor.ok(); ++i) {
    ModelMaterial material;
    const uint32_t nameLength = cursor.U32();
    const uint8_t* nameBytes = cursor.Bytes(nameLength);
    if (nameBytes) {
      // retrotool insists the name is UTF-8; keeping the bytes is friendlier, and
      // every name in the game is ASCII.
      material.name.assign(reinterpret_cast<const char*>(nameBytes), nameLength);
    }
    material.shaderId = cursor.Uuid();
    material.unkGuid = cursor.Uuid();
    material.unk1 = cursor.U32();
    material.unk2 = cursor.U32();
    const uint32_t typeCount = cursor.U32();
    if (!cursor.ok() || typeCount > cursor.remaining() / 4) {
      cursor.Fail("implausible material type count");
      break;
    }
    material.types.resize(typeCount);
    for (uint32_t t = 0; t < typeCount; ++t) {
      material.types[t] = cursor.FourCC();
    }
    const uint32_t renderCount = cursor.U32();
    if (!cursor.ok() || renderCount > cursor.remaining() / 10) {
      cursor.Fail("implausible render type count");
      break;
    }
    material.renderTypes.resize(renderCount);
    for (uint32_t r = 0; r < renderCount; ++r) {
      material.renderTypes[r].dataId = cursor.FourCC();
      material.renderTypes[r].dataType = cursor.FourCC();
      material.renderTypes[r].flag1 = cursor.U8();
      material.renderTypes[r].flag2 = cursor.U8();
    }
    const uint32_t dataCount = cursor.U32();
    if (!cursor.ok() || dataCount > cursor.remaining() / 8) {
      cursor.Fail("implausible material data count");
      break;
    }
    // A tag table and a payload table, both as long as dataCount. The payloads
    // repeat their tags, so the tag table is read only to step over it; the tags
    // that key the parameters are the ones inside the payload table.
    cursor.Skip(size_t(dataCount) * 8);
    material.data.resize(dataCount);
    for (uint32_t d = 0; d < dataCount && cursor.ok(); ++d) {
      ParseMaterialData(cursor, material.data[d]);
    }
    out.push_back(material);
  }
  return cursor.ok();
}

// One declared vertex attribute: where in its sub-buffer, how wide, and what it is.
struct VertexComponent {
  uint32_t bufferIndex = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
  uint32_t format = 0;
  uint32_t component = 0;
};

struct VertexEntry {
  uint32_t vertexCount = 0;
  uint32_t subBuffers = 0;
  std::vector<VertexComponent> components;
  std::vector<uint32_t> strides;  // one per sub-buffer, 0 when the buffer has none
};

bool ParseVertexInfos(Cursor& cursor, std::vector<VertexEntry>& out) {
  const uint32_t count = cursor.U32();
  if (!cursor.ok() || count > cursor.remaining() / 9) {
    return cursor.Fail("implausible vertex buffer info count");
  }
  out.clear();
  out.reserve(count);
  for (uint32_t i = 0; i < count && cursor.ok(); ++i) {
    VertexEntry entry;
    entry.vertexCount = cursor.U32();
    const uint32_t componentCount = cursor.U32();
    if (!cursor.ok() || componentCount > cursor.remaining() / 20) {
      cursor.Fail("implausible vertex component count");
      break;
    }
    entry.components.resize(componentCount);
    for (uint32_t c = 0; c < componentCount; ++c) {
      VertexComponent& component = entry.components[c];
      component.bufferIndex = cursor.U32();
      component.offset = cursor.U32();
      component.stride = cursor.U32();
      component.format = cursor.U32();
      component.component = cursor.U32();
    }
    entry.subBuffers = cursor.U8();
    if (!cursor.ok() || entry.subBuffers > cursor.remaining() / 20) {
      cursor.Fail("implausible vertex sub-buffer count");
      break;
    }
    entry.strides.assign(entry.subBuffers, 0);
    for (const VertexComponent& component : entry.components) {
      if (component.bufferIndex >= entry.subBuffers) {
        cursor.Fail("vertex component names a sub-buffer that does not exist");
        break;
      }
      uint32_t& stride = entry.strides[component.bufferIndex];
      if (stride == 0) {
        stride = component.stride;
      } else if (stride != component.stride) {
        // retrotool refuses a mismatch, and so does this: reading with the wrong
        // stride would quietly produce garbage vertices.
        cursor.Fail("mismatched vertex strides " + std::to_string(stride) + " and " +
                    std::to_string(component.stride));
        break;
      }
    }
    out.push_back(entry);
  }
  return cursor.ok();
}

// What one vertex format decodes to: how wide, how many components, and whether it
// reads as integers or as floats. `components == 0` means the format is unknown.
struct FormatInfo {
  uint32_t components = 0;
  uint32_t size = 0;
  bool isInteger = false;
  bool isSigned = false;
  bool isFloat = false;
  bool isNormalized = false;
};

FormatInfo FormatTable(uint32_t format) {
  // EVertexDataFormat, in the order cmdl.rs lists them: {components, bytes,
  // integer, signed, float, normalized}.
  static const FormatInfo kFormats[] = {
      {1, 1, false, false, false, true},   {1, 1, true, false, false, false},   // R8Unorm, R8Uint
      {1, 1, true, true, false, true},     {1, 1, true, true, false, false},     // R8Snorm, R8Sint
      {1, 2, false, false, false, true},   {1, 2, true, false, false, false},    // R16Unorm, R16Uint
      {1, 2, true, true, false, true},     {1, 2, true, true, false, false},     // R16Snorm, R16Sint
      {1, 2, false, false, true, false},                                      // R16Float
      {2, 2, false, false, false, true},   {2, 2, true, false, false, false},    // Rg8Unorm, Rg8Uint
      {2, 2, true, true, false, true},     {2, 2, true, true, false, false},     // Rg8Snorm, Rg8Sint
      {1, 4, true, false, false, false},   {1, 4, true, true, false, false},     // R32Uint, R32Sint
      {1, 4, false, false, true, false},                                      // R32Float
      {2, 4, false, false, false, true},   {2, 4, true, false, false, false},    // Rg16Unorm, Rg16Uint
      {2, 4, true, true, false, true},     {2, 4, true, true, false, false},     // Rg16Snorm, Rg16Sint
      {2, 4, false, false, true, false},                                      // Rg16Float
      {4, 4, false, false, false, true},   {4, 4, true, false, false, false},    // Rgba8Unorm, Rgba8Uint
      {4, 4, true, true, false, true},     {4, 4, true, true, false, false},     // Rgba8Snorm, Rgba8Sint
      {4, 4, false, false, false, true},   {4, 4, true, false, false, false},    // Rgb10a2Unorm, Rgb10a2Uint
      {2, 8, true, false, false, false},   {2, 8, true, true, false, false},     // Rg32Uint, Rg32Sint
      {2, 8, false, false, true, false},                                      // Rg32Float
      {4, 8, false, false, false, true},   {4, 8, true, false, false, false},    // Rgba16Unorm, Rgba16Uint
      {4, 8, true, true, false, true},     {4, 8, true, true, false, false},     // Rgba16Snorm, Rgba16Sint
      {4, 8, false, false, true, false},                                      // Rgba16Float
      {3, 12, true, false, false, false},  {3, 12, true, true, false, false},    // Rgb32Uint, Rgb32Sint
      {3, 12, false, false, true, false},                                     // Rgb32Float
      {4, 16, true, false, false, false}, {4, 16, true, true, false, false},    // Rgba32Uint, Rgba32Sint
      {4, 16, false, false, true, false},                                     // Rgba32Float
  };
  if (format < sizeof(kFormats) / sizeof(kFormats[0])) {
    return kFormats[format];
  }
  return FormatInfo();
}

// One vertex of one attribute, decoded from its sub-buffer.
void DecodeVertex(const uint8_t* p, const FormatInfo& info, uint32_t format, float* floats,
                  uint32_t* uints) {
  if (format == 25 || format == 26) {  // Rgb10a2, packed: red at bit 0, alpha at bit 30
    const uint32_t packed = ReadLE32(p);
    if (format == 26) {
      uints[0] = packed & 0x3FF;
      uints[1] = (packed >> 10) & 0x3FF;
      uints[2] = (packed >> 20) & 0x3FF;
      uints[3] = (packed >> 30) & 0x3;
      return;
    }
    floats[0] = float(packed & 0x3FF) * (1.0f / 1023.0f);
    floats[1] = float((packed >> 10) & 0x3FF) * (1.0f / 1023.0f);
    floats[2] = float((packed >> 20) & 0x3FF) * (1.0f / 1023.0f);
    floats[3] = float((packed >> 30) & 0x3) * (1.0f / 3.0f);
    return;
  }
  // Everything else is a run of equal sized scalars.
  const uint32_t size = info.size / info.components;
  for (uint32_t c = 0; c < info.components; ++c) {
    const uint8_t* scalar = p + c * size;
    uint32_t asUint = 0;
    int32_t asInt = 0;
    float asFloat = 0.0f;
    if (size == 1) {
      asUint = scalar[0];
      asInt = int32_t(int8_t(scalar[0]));
    } else if (size == 2) {
      const uint16_t bits = ReadLE16(scalar);
      asUint = bits;
      asInt = int32_t(int16_t(bits));
      if (info.isFloat) {
        asFloat = HalfToFloat(bits);
      }
    } else {
      const uint32_t bits = ReadLE32(scalar);
      asUint = bits;
      asInt = int32_t(bits);
      if (info.isFloat) {
        asFloat = ReadLEFloat(scalar);
      }
    }
    if (info.isFloat) {
      floats[c] = asFloat;
    } else if (info.isNormalized) {
      // What a glTF consumer computes from a normalized accessor, so the numbers
      // here are the ones a renderer would use.
      if (info.isSigned) {
        const float scaled = float(asInt) / ((size == 1) ? 127.0f : 32767.0f);
        floats[c] = scaled < -1.0f ? -1.0f : scaled;
      } else {
        floats[c] = float(asUint) / ((size == 1) ? 255.0f : 65535.0f);
      }
    } else if (info.isInteger) {
      uints[c] = info.isSigned ? uint32_t(asInt) : asUint;
    } else {
      floats[c] = float(asUint);
    }
  }
}

// Decodes one declared attribute across every vertex of its VBUF entry.
bool DecodeAttribute(const std::vector<uint8_t>& buffer, const VertexEntry& entry,
                     const VertexComponent& component, ModelAttribute& out, std::string& error) {
  const FormatInfo info = FormatTable(component.format);
  if (info.components == 0) {
    error = "remastered model: unknown vertex format " + std::to_string(component.format);
    return false;
  }
  const uint64_t stride = component.stride;
  const uint64_t needed =
      entry.vertexCount == 0 ? 0 : uint64_t(entry.vertexCount - 1) * stride + component.offset + info.size;
  if (needed > buffer.size()) {
    error = "remastered model: vertex attribute " + std::string(ComponentName(component.component)) +
            " runs past the end of its buffer";
    return false;
  }
  out.component = component.component;
  out.name = ComponentName(component.component);
  out.components = info.components;
  out.isInteger = info.isInteger;
  float floats[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t uints[4] = {0, 0, 0, 0};
  std::vector<float>& outFloats = out.data;
  std::vector<uint32_t>& outUints = out.uints;
  if (info.isInteger) {
    outUints.resize(size_t(entry.vertexCount) * info.components);
    for (uint32_t v = 0; v < entry.vertexCount; ++v) {
      DecodeVertex(buffer.data() + size_t(uint64_t(v) * stride) + component.offset, info, component.format,
                   floats, uints);
      for (uint32_t c = 0; c < info.components; ++c) {
        outUints[size_t(v) * info.components + c] = uints[c];
      }
    }
  } else {
    outFloats.resize(size_t(entry.vertexCount) * info.components);
    for (uint32_t v = 0; v < entry.vertexCount; ++v) {
      DecodeVertex(buffer.data() + size_t(uint64_t(v) * stride) + component.offset, info, component.format,
                   floats, uints);
      for (uint32_t c = 0; c < info.components; ++c) {
        outFloats[size_t(v) * info.components + c] = floats[c];
      }
    }
  }
  return true;
}

// One component of one vertex, whichever way the source format decoded. A position
// or a UV stored as plain integers (none are, but nothing rules it out) still has to
// land in a float array.
float AttributeValue(const ModelAttribute& attribute, uint32_t vertex, uint32_t component) {
  if (attribute.isInteger) {
    return float(attribute.uints[size_t(vertex) * attribute.components + component]);
  }
  return attribute.data[size_t(vertex) * attribute.components + component];
}

std::vector<float> AttributeFloats(const ModelAttribute& attribute) {
  if (!attribute.isInteger) {
    return attribute.data;
  }
  std::vector<float> out;
  out.reserve(attribute.uints.size());
  for (uint32_t value : attribute.uints) {
    out.push_back(float(value));
  }
  return out;
}

// One mesh as the MESH chunk declares it, before the indices are read.
struct MeshDecl {
  uint32_t material = 0;
  uint32_t vertexBuffer = 0;
  uint32_t indexBuffer = 0;
  uint32_t indexStart = 0;
  uint32_t indexCount = 0;
  uint16_t unkC = 0;
  uint16_t unkE = 0;
  uint8_t bits2 = 0;  // this mesh's two bits of the first bitmap after the meshes
  bool twoSided = false;  // its bit of the second: drawn with culling off
};

bool ParseMesh(Cursor& cursor, std::vector<MeshDecl>& meshes, std::vector<ModelLod>& lods,
               std::vector<float>& lodRules, std::vector<uint16_t>& lodMeshes) {
  const uint32_t meshCount = cursor.U32();
  if (!cursor.ok() || meshCount > cursor.remaining() / 16) {
    return cursor.Fail("implausible mesh count");
  }
  meshes.resize(meshCount);
  for (uint32_t i = 0; i < meshCount; ++i) {
    MeshDecl& mesh = meshes[i];
    mesh.material = cursor.U16();
    mesh.vertexBuffer = cursor.U8();
    mesh.indexBuffer = cursor.U8();
    mesh.indexStart = cursor.U32();
    mesh.indexCount = cursor.U32();
    mesh.unkC = cursor.U16();
    mesh.unkE = cursor.U16();
  }
  // Two bitmaps follow the meshes, each rounded up to whole bytes: two bits per
  // mesh, then one.
  for (uint32_t i = 0; i < (meshCount + 3) / 4; ++i) {
    const uint8_t byte = cursor.U8();
    for (uint32_t j = 0; j < 4 && i * 4 + j < meshCount; ++j) {
      meshes[i * 4 + j].bits2 = (byte >> (j * 2)) & 3;
    }
  }
  for (uint32_t i = 0; i < (meshCount + 7) / 8; ++i) {
    const uint8_t byte = cursor.U8();
    for (uint32_t j = 0; j < 8 && i * 8 + j < meshCount; ++j) {
      meshes[i * 8 + j].twoSided = (byte >> j) & 1;
    }
  }
  const uint32_t listCount = cursor.U32();
  if (!cursor.ok() || listCount > cursor.remaining() / 2) {
    return cursor.Fail("implausible LOD mesh list length");
  }
  lodMeshes.resize(listCount);
  for (uint32_t i = 0; i < listCount; ++i) {
    lodMeshes[i] = cursor.U16();
  }
  const uint8_t lodCount = cursor.U8();
  if (!cursor.ok() || lodCount > cursor.remaining() / 40) {
    return cursor.Fail("implausible LOD count");
  }
  for (uint8_t lod = 0; lod < lodCount; ++lod) {
    // Five index ranges per LOD entry; the game uses one or two of them, and the
    // rest come out as zeroes, which is harmless.
    for (int i = 0; i < 5; ++i) {
      ModelLod range;
      range.indexOffset = cursor.U32();
      range.indexCount = cursor.U32();
      lods.push_back(range);
    }
  }
  const uint32_t hasLodRules = cursor.U32();
  if (hasLodRules == 1 && lodCount > 0) {
    lodRules.resize(lodCount);
    for (uint8_t i = 0; i < lodCount; ++i) {
      lodRules[i] = cursor.F32();
    }
  }
  return cursor.ok();
}

// Walks the blocks in [body, end) depth first, the order retrotool's slice_chunks
// visits them in, handing each chunk's body to `visit`. That order matters: the last
// HEAD, SKHD or WDHD chunk is the one that counts.
bool WalkBlocks(const uint8_t* data, size_t size, size_t body, size_t end, std::string& error,
                const std::function<bool(uint32_t magic, Cursor& content)>& visit) {
  Cursor cursor(data, size, &error);
  cursor.Seek(body);
  while (cursor.ok() && cursor.offset() < end) {
    BlockHeader header;
    if (!ReadBlockHeader(cursor, header)) {
      return false;
    }
    if (header.end > end) {
      return cursor.Fail("block runs past the end of its form");
    }
    if (header.magic == kFormRFRM) {
      if (!WalkBlocks(data, size, header.body, header.end, error, visit)) {
        return false;
      }
      cursor.Seek(header.end);
      continue;
    }
    Cursor content(data, size, &error);
    content.Seek(header.body);
    if (!visit(header.magic, content)) {
      return false;
    }
    cursor.Seek(header.end);
  }
  return cursor.ok();
}


// The vertex buffers. Each sub-buffer of a VBUF entry has its own compressed block,
// in the order the entries declare them, so the flattened list lines up with them.
// Components come out grouped by sub-buffer and ordered by offset within it, which is
// the order retrotool writes its accessors in.
bool DecodeVertexBuffers(const std::vector<VertexEntry>& entries,
                         const std::vector<std::vector<uint8_t>>& buffers,
                         std::vector<ModelVertexBuffer>& out, std::string& error) {
  out.resize(entries.size());
  size_t bufferIndex = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const VertexEntry& entry = entries[i];
    ModelVertexBuffer& decoded = out[i];
    decoded.vertexCount = entry.vertexCount;
    if (bufferIndex + entry.subBuffers > buffers.size()) {
      error = "remastered model: VBUF entry " + std::to_string(i) + " names " +
              std::to_string(entry.subBuffers) + " sub-buffers, past the " +
              std::to_string(buffers.size()) + " the metadata holds";
      return false;
    }
    std::vector<std::vector<const VertexComponent*>> byBuffer(entry.subBuffers);
    for (const VertexComponent& component : entry.components) {
      if (component.bufferIndex < entry.subBuffers) {
        byBuffer[component.bufferIndex].push_back(&component);
      }
    }
    for (std::vector<const VertexComponent*>& components : byBuffer) {
      for (size_t a = 1; a < components.size(); ++a) {
        const VertexComponent* key = components[a];
        size_t b = a;
        while (b > 0 && components[b - 1]->offset > key->offset) {
          components[b] = components[b - 1];
          --b;
        }
        components[b] = key;
      }
    }
    for (uint32_t sub = 0; sub < entry.subBuffers; ++sub) {
      const std::vector<uint8_t>& source = buffers[bufferIndex + sub];
      for (const VertexComponent* component : byBuffer[sub]) {
        ModelAttribute attribute;
        if (!DecodeAttribute(source, entry, *component, attribute, error)) {
          return false;
        }
        const uint32_t vertexCount = entry.vertexCount;
        const uint32_t sourceComponents = attribute.components;
        switch (component->component) {
          case kComponentPosition:
          case kComponentNormal: {
            // Three components per vertex, taken from whatever the format holds; glTF
            // types a position and a normal as VEC3 whatever the source width.
            const uint32_t take = sourceComponents < 3 ? sourceComponents : 3;
            std::vector<float>& target =
                component->component == kComponentPosition ? decoded.positions : decoded.normals;
            target.assign(size_t(vertexCount) * 3, 0.0f);
            for (uint32_t v = 0; v < vertexCount; ++v) {
              for (uint32_t c = 0; c < take; ++c) {
                target[size_t(v) * 3 + c] = AttributeValue(attribute, v, c);
              }
            }
            break;
          }
          case kComponentTangent0:
            decoded.tangents = AttributeFloats(attribute);
            break;
          case kComponentColor:
            decoded.colors = AttributeFloats(attribute);
            break;
          case kComponentBoneWeights:
            decoded.weights = AttributeFloats(attribute);
            break;
          case kComponentBoneIndices:
            // Four per vertex, as glTF's JOINTS_0 wants them.
            decoded.joints.resize(size_t(vertexCount) * 4);
            for (size_t k = 0; k < decoded.joints.size(); ++k) {
              const float value = AttributeValue(attribute, uint32_t(k / 4), uint32_t(k % 4));
              decoded.joints[k] = value <= 0.0f ? 0 : (value >= 65535.0f ? 65535 : uint16_t(value));
            }
            break;
          default:
            if (component->component >= kComponentTexCoord0 &&
                component->component < kComponentTexCoord0 + 4) {
              const size_t set = component->component - kComponentTexCoord0;
              if (decoded.uvs.size() <= set) {
                decoded.uvs.resize(set + 1);
              }
              std::vector<float>& uv = decoded.uvs[set];
              uv.assign(size_t(vertexCount) * 2, 0.0f);
              const uint32_t take = sourceComponents < 2 ? sourceComponents : 2;
              for (uint32_t v = 0; v < vertexCount; ++v) {
                for (uint32_t c = 0; c < take; ++c) {
                  uv[size_t(v) * 2 + c] = AttributeValue(attribute, v, c);
                }
              }
              if (decoded.uvsZw.size() <= set) {
                decoded.uvsZw.resize(set + 1);
              }
              if (sourceComponents >= 4) {
                std::vector<float>& zw = decoded.uvsZw[set];
                zw.resize(size_t(vertexCount) * 2);
                for (uint32_t v = 0; v < vertexCount; ++v) {
                  for (uint32_t c = 0; c < 2; ++c) {
                    zw[size_t(v) * 2 + c] = AttributeValue(attribute, v, c + 2);
                  }
                }
              }
            } else {
              // The rest (TANGENT_1 and TANGENT_2, the baked lighting set, the
              // per-instance parameters) is kept as it was declared.
              decoded.attributes.push_back(attribute);
            }
            break;
        }
      }
    }
    bufferIndex += entry.subBuffers;
  }
  return true;
}

// The meshes, with the indices widened to 32 bit whatever width the buffer stores.
bool ReadMeshes(const std::vector<MeshDecl>& decls, const std::vector<uint32_t>& indexTypes,
                const std::vector<std::vector<uint8_t>>& buffers, Model& out, std::string& error) {
  out.meshes.resize(decls.size());
  for (size_t i = 0; i < decls.size(); ++i) {
    const MeshDecl& decl = decls[i];
    ModelMesh& mesh = out.meshes[i];
    mesh.material = decl.material;
    mesh.vertexBuffer = decl.vertexBuffer;
    mesh.indexBuffer = decl.indexBuffer;
    mesh.indexStart = decl.indexStart;
    mesh.indexCount = decl.indexCount;
    mesh.unkC = decl.unkC;
    mesh.unkE = decl.unkE;
    mesh.bits2 = decl.bits2;
    mesh.twoSided = decl.twoSided;
    if (decl.indexBuffer >= indexTypes.size() || decl.indexBuffer >= buffers.size()) {
      error = "remastered model: mesh " + std::to_string(i) + " names index buffer " +
              std::to_string(decl.indexBuffer) + " of " + std::to_string(indexTypes.size());
      return false;
    }
    // EBufferType: 0 is 8 bit (no model uses it), 1 is 16, 2 is 32.
    switch (indexTypes[decl.indexBuffer]) {
      case 0: mesh.indexWidth = 1; break;
      case 1: mesh.indexWidth = 2; break;
      case 2: mesh.indexWidth = 4; break;
      default:
        error = "remastered model: index buffer " + std::to_string(decl.indexBuffer) + " has width " +
                std::to_string(indexTypes[decl.indexBuffer]);
        return false;
    }
    if (decl.vertexBuffer < out.vertexBuffers.size()) {
      mesh.vertexCount = out.vertexBuffers[decl.vertexBuffer].vertexCount;
    }
    const std::vector<uint8_t>& buffer = buffers[decl.indexBuffer];
    const uint64_t needed =
        (uint64_t(decl.indexStart) + uint64_t(decl.indexCount)) * mesh.indexWidth;
    if (needed > buffer.size()) {
      error = "remastered model: mesh " + std::to_string(i) + " reads past the end of its index buffer";
      return false;
    }
    mesh.indices.resize(decl.indexCount);
    const uint8_t* p = buffer.data() + uint64_t(decl.indexStart) * mesh.indexWidth;
    for (uint32_t k = 0; k < decl.indexCount; ++k) {
      const uint8_t* at = p + size_t(uint64_t(k) * mesh.indexWidth);
      if (mesh.indexWidth == 1) {
        mesh.indices[k] = at[0];
      } else if (mesh.indexWidth == 2) {
        mesh.indices[k] = ReadLE16(at);
      } else {
        mesh.indices[k] = ReadLE32(at);
      }
    }
  }
  return true;
}

}  // namespace

const ModelAttribute* ModelVertexBuffer::Find(const std::string& name) const {
  for (const ModelAttribute& attribute : attributes) {
    if (attribute.name == name) {
      return &attribute;
    }
  }
  return nullptr;
}

bool ParseModel(const uint8_t* data, size_t size, Model& out, std::string& error) {
  out = Model();
  error.clear();
  if (data == nullptr || size < 32) {
    error = "remastered model: not an RFRM file";
    return false;
  }

  // The model form, and the versions that go with it.
  BlockHeader form;
  {
    Cursor cursor(data, size, &error);
    if (!ReadBlockHeader(cursor, form) || form.magic != kFormRFRM) {
      if (error.empty()) {
        error = "remastered model: missing RFRM header";
      }
      return false;
    }
    // retrotool pins the reader and writer versions per model type, and so does
    // this: another pair, or another form, means another layout, and parsing it as this
    // one would be guesswork.
    const bool known = (form.id == kFormCmdl && form.readerVersion == 114 && form.writerVersion == 125) ||
                       (form.id == kFormSmdl && form.readerVersion == 127 && form.writerVersion == 133) ||
                       (form.id == kFormWmdl && form.readerVersion == 118 && form.writerVersion == 124);
    if (!known) {
      error = "remastered model: not a model form this reader knows (" + std::to_string(form.id) +
              " " + std::to_string(form.readerVersion) + "/" + std::to_string(form.writerVersion) + ")";
      return false;
    }
  }
  out.form = form.id;
  out.readerVersion = form.readerVersion;
  out.writerVersion = form.writerVersion;

  // META, out of the FOOT form retrotool appends after the model. Only that chunk is
  // wanted here, so the rest are stepped over.
  std::vector<MetaReadBuffer> readBuffers;
  std::vector<MetaBuffer> vertexMeta;
  std::vector<MetaBuffer> indexMeta;
  {
    Cursor foot(data, size, &error);
    foot.Seek(form.end);
    BlockHeader footForm;
    if (!ReadBlockHeader(foot, footForm) || footForm.magic != kFormRFRM || footForm.id != kFormFoot) {
      if (error.empty()) {
        error = "remastered model: missing FOOT form after the model";
      }
      return false;
    }
    bool foundMeta = false;
    if (!WalkBlocks(data, size, footForm.body, footForm.end, error,
                    [&](uint32_t magic, Cursor& content) {
                      if (magic != kChunkMeta) {
                        return true;
                      }
                      if (!ParseMeta(content, readBuffers, vertexMeta, indexMeta)) {
                        return false;
                      }
                      foundMeta = true;
                      return true;
                    })) {
      return false;
    }
    if (!foundMeta) {
      error = "remastered model: no META chunk in the footer";
      return false;
    }
  }

  std::vector<std::vector<uint8_t>> vertexBuffers;
  std::vector<std::vector<uint8_t>> indexBuffers;
  if (!DecompressBuffers(data, size, readBuffers, vertexMeta, vertexBuffers, error) ||
      !DecompressBuffers(data, size, readBuffers, indexMeta, indexBuffers, error)) {
    return false;
  }

  // The chunks themselves, in the order retrotool reads them.
  std::vector<VertexEntry> vertexInfos;
  std::vector<uint32_t> indexTypes;
  std::vector<MeshDecl> meshDecls;
  bool sawHeader = false;
  bool sawMesh = false;
  bool sawVbuf = false;
  bool sawIbuf = false;
  bool sawMtrl = false;
  if (!WalkBlocks(data, size, form.body, form.end, error,
                  [&](uint32_t magic, Cursor& content) {
                    switch (magic) {
                      case kChunkHead:
                      case kChunkWdhd:
                      case kChunkSkhd:
                        // The last of the three wins, as in retrotool: an SMDL carries
                        // both SKHD and HEAD, and HEAD holds the real bounds.
                        out.headerUnknown = content.U32();
                        for (int i = 0; i < 3; ++i) {
                          out.boundsMin[i] = content.F32();
                        }
                        for (int i = 0; i < 3; ++i) {
                          out.boundsMax[i] = content.F32();
                        }
                        if (magic == kChunkSkhd) {
                          out.skinned = true;
                        }
                        if (magic == kChunkHead && content.ok()) {
                          const size_t left = content.remaining();
                          const uint8_t* rest = content.Peek(left);
                          for (size_t i = 0; rest != nullptr && i + 4 < left; ++i) {
                            if (std::memcmp(rest + i, "ANUV", 4) == 0) {
                              out.anuv.assign(rest + i + 4, rest + left);
                              break;
                            }
                          }
                          out.windSets.clear();
                          out.windMaterialSet.clear();
                          for (size_t i = 0; rest != nullptr && i + 8 < left; ++i) {
                            if (std::memcmp(rest + i, "WIND", 4) != 0) {
                              continue;
                            }
                            size_t at = i + 4;
                            uint32_t sets = 0;
                            std::memcpy(&sets, rest + at, 4);
                            at += 4;
                            if (sets > (left - at) / 36) {
                              break;
                            }
                            std::vector<std::array<float, 9>> parsed(sets);
                            for (uint32_t s = 0; s < sets; ++s, at += 36) {
                              std::memcpy(parsed[s].data(), rest + at, 36);
                            }
                            uint32_t mats = 0;
                            if (at + 4 > left) {
                              break;
                            }
                            std::memcpy(&mats, rest + at, 4);
                            at += 4;
                            if (mats > left - at) {
                              break;
                            }
                            out.windSets = std::move(parsed);
                            out.windMaterialSet.assign(rest + at, rest + at + mats);
                            break;
                          }
                        }
                        sawHeader = true;
                        return content.ok();
                      case kChunkMtrl:
                        sawMtrl = true;
                        return ParseMaterials(content, out.materials);
                      case kChunkMesh:
                        sawMesh = true;
                        return ParseMesh(content, meshDecls, out.lods, out.lodRules, out.lodMeshes);
                      case kChunkVbuf:
                        sawVbuf = true;
                        return ParseVertexInfos(content, vertexInfos);
                      case kChunkIbuf: {
                        sawIbuf = true;
                        const uint32_t count = content.U32();
                        if (!content.ok() || count > content.remaining() / 4) {
                          content.Fail("implausible index buffer info count");
                          return false;
                        }
                        indexTypes.resize(count);
                        for (uint32_t i = 0; i < count; ++i) {
                          indexTypes[i] = content.U32();
                        }
                        return content.ok();
                      }
                      case kChunkGpu:
                        // The buffers were read through META already.
                        return true;
                      default:
                        return content.Fail("unknown chunk " + std::to_string(magic));
                    }
                  })) {
    return false;
  }
  if (!sawHeader || !sawMesh || !sawVbuf || !sawIbuf || !sawMtrl) {
    error = "remastered model: a HEAD, MESH, VBUF, IBUF or MTRL chunk is missing";
    return false;
  }

  // Vertex buffers and meshes: the chunks said how, this reads what they describe.
  if (!DecodeVertexBuffers(vertexInfos, vertexBuffers, out.vertexBuffers, error)) {
    return false;
  }
  if (!ReadMeshes(meshDecls, indexTypes, indexBuffers, out, error)) {
    return false;
  }
  return true;
}

}  // namespace PortRemastered