#pragma once

// Reads a Metroid Prime Remastered model resource (a "CMDL", "SMDL" or "WMDL" RFRM form,
// as retrotool extracts them) and hands back the decoded geometry and materials in
// memory. It parses; it converts to nothing and writes nothing.
//
// This is a port of retrotool's retrolib (build/mpr-tools/retrotool/lib/src/format/
// cmdl.rs) and of the decoding half of `retrotool cmdl convert`
// (retrotool/src/cmd/cmdl.rs), because that is the reference the port's Remastered
// importer has to agree with: the same models, the same vertex attributes, the same
// material parameters. retrotool's patched convert also dumps every material
// parameter as materials.tsv (see build/mpr/gc/retrotool-materials.patch); the
// fields modelled here are the ones that file prints.
//
// The input is one asset as retrotool extracts it: the RFRM model form followed by
// a "FOOT" form carrying the asset's META blob, which is what says where the
// compressed vertex and index buffers sit. PortRemastered::Pak::ReadAsset produces
// exactly such a buffer.
//
// Vertex data lives on the buffer, not on the mesh: one VBUF entry is shared by
// many meshes (16 meshes over 3 buffers is common), so keeping a copy per mesh
// would multiply it. A mesh names the buffer it draws from, and the attributes are
// decoded once per buffer, in the order retrotool writes its accessors.
//
// Nothing here throws across the API: a parse either returns true and fills `out`,
// or returns false with a message in `error`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace PortRemastered {

// An asset id, in the byte order it is stored in (retrotool reads its UUIDs with
// from_bytes_le, so the first three groups are little endian). IdToString() in
// port_remastered_pak.h converts one to the printed form.
using ModelUuid = std::array<uint8_t, 16>;

// One texture slot of a material: which map it is (its FourCC, e.g. 'DIFT'), which
// TXTR it points at, and the sampler state that goes with it. The usage block is
// absent when the id is nil, which is how the file says "slot unused".
struct ModelTextureRef {
  uint32_t usage = 0;  // FourCC of the slot this texture was read from
  ModelUuid id{};      // TXTR asset id, as stored
  bool hasUsage = false;
  uint32_t texCoord = 0;  // which TEXCOORD_n set samples it
  int32_t filter = -1;
  int32_t wrapX = -1;
  int32_t wrapY = -1;
  int32_t wrapZ = -1;
};

// One entry of a material's render type table (FREA/CCH1/1/2 and the like).
struct ModelRenderType {
  uint32_t dataId = 0;    // FourCC
  uint32_t dataType = 0;  // FourCC
  uint8_t flag1 = 0;
  uint8_t flag2 = 0;
};

// The value half of one material parameter, keyed by its FourCC in `usage`. The
// file stores a tag per parameter and then a payload whose shape that tag picks;
// only the members named by `kind` are meaningful.
struct ModelMaterialData {
  enum class Kind : uint8_t {
    Texture,         // 'TXTR': a texture slot
    Color,           // 'COLR': rgba
    Scalar,          // 'SCLR': one float
    Int1,            // 'INT1': one int
    Int4,            // 'INT4': four ints
    Matrix4,         // 'MAT4': sixteen floats, row major
    LayeredTexture,  // 'CPLX': three tinted texture slots (BCRL, MTLL, NRML)
  };
  uint32_t usage = 0;  // FourCC of the parameter, e.g. 'DIFC'
  Kind kind = Kind::Texture;
  ModelTextureRef texture;                  // Texture
  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // Color
  float scalar = 0.0f;                      // Scalar
  int32_t int1 = 0;                         // Int1
  int32_t int4[4] = {0, 0, 0, 0};           // Int4
  float matrix4[16] = {};                   // Matrix4
  // LayeredTexture: the base block's two scalars, then one tint and one texture
  // per layer.
  uint32_t layeredUnknown = 0;
  uint8_t layeredFlags = 0;
  std::array<float, 12> layeredColors{};             // three rgba tints
  std::array<ModelTextureRef, 3> layeredTextures{};  // the three slots
};

// One material. Every parameter the file holds is kept, whether or not a converter
// needs it today: the port's material record is built out of these, and dropping
// the ones nothing reads yet would mean reading the files again later.
struct ModelMaterial {
  std::string name;
  ModelUuid shaderId{};
  ModelUuid unkGuid{};
  uint32_t unk1 = 0;
  uint32_t unk2 = 0;
  std::vector<uint32_t> types;  // FourCCs, e.g. 'RLTG'
  std::vector<ModelRenderType> renderTypes;
  std::vector<ModelMaterialData> data;
};

// A vertex attribute with no named slot above, kept as parsed: TANGENT_1 and
// TANGENT_2, the baked lighting set, the per-instance parameters. glTF exposes
// these under "_" prefixed custom names, and the port may want some of them.
struct ModelAttribute {
  uint32_t component = 0;   // EVertexComponent, as stored
  std::string name;         // e.g. "TANGENT_1", "BAKED_LIGHTING_COORD"
  uint32_t components = 0;  // components per vertex, as the format declares
  bool isInteger = false;   // true for the uint/sint formats
  std::vector<float> data;      // floats, components per vertex (unused if isInteger)
  std::vector<uint32_t> uints;  // raw values, sign extended (only if isInteger)
};

// The decoded geometry of one VBUF entry: every attribute of every one of its
// sub-buffers, all over `vertexCount` vertices. Normalized integer formats are
// decoded to the 0..1 (or -1..1) floats a renderer would use, so the numbers here
// are the numbers to hand on.
struct ModelVertexBuffer {
  uint32_t vertexCount = 0;
  std::vector<float> positions;       // xyz per vertex
  std::vector<float> normals;         // xyz per vertex
  std::vector<float> tangents;        // xyzw per vertex (TANGENT)
  std::vector<std::vector<float>> uvs;  // one entry per TEXCOORD_n, uv per vertex
  // The zw of each TEXCOORD_n, empty when its format holds two components. The
  // shaders take two texcoords from each attribute: a material's texcoord c is
  // TEXCOORD_(c/2).xy when c is even and its zw when c is odd.
  std::vector<std::vector<float>> uvsZw;
  std::vector<float> colors;          // rgba per vertex
  std::vector<uint16_t> joints;       // four per vertex, skinned models only
  std::vector<float> weights;         // four per vertex, skinned models only
  std::vector<ModelAttribute> attributes;  // everything else, in buffer order
  // One of the `attributes` by name, or null. The arrays above are not held as
  // attributes, so this finds the rest ("TANGENT_1", "BAKED_LIGHTING_COORD", ...).
  const ModelAttribute* Find(const std::string& name) const;
};

// The joint a triangle goes with when a skinned model is cut into rigid pieces, one per
// joint: the one with the most weight over its three corners (the lowest on a tie), -1
// for a buffer with no skin. Inline, so the converter links without the parser.
inline int TriangleJoint(const ModelVertexBuffer& vb, const uint32_t corner[3]) {
  const size_t n = vb.vertexCount;
  if (vb.joints.size() != n * 4 || vb.weights.size() != n * 4) {
    return -1;
  }
  std::pair<uint16_t, float> sum[12];
  size_t used = 0;
  for (int c = 0; c < 3; ++c) {
    if (corner[c] >= n) {
      continue;
    }
    for (size_t i = size_t(corner[c]) * 4; i < size_t(corner[c]) * 4 + 4; ++i) {
      size_t k = 0;
      while (k < used && sum[k].first != vb.joints[i]) {
        ++k;
      }
      if (k == used) {
        sum[used++] = {vb.joints[i], 0.0f};
      }
      sum[k].second += vb.weights[i];
    }
  }
  int best = -1;
  float most = 0.0f;
  for (size_t k = 0; k < used; ++k) {
    if (sum[k].second > most || (sum[k].second == most && best >= 0 && sum[k].first < best)) {
      best = sum[k].first;
      most = sum[k].second;
    }
  }
  return best;
}

// One drawable mesh. Its vertex data is not here, it is in Model::vertexBuffers at
// `vertexBuffer`; the indices are widened to 32 bit so that a 16 and a 32 bit
// index buffer read the same.
struct ModelMesh {
  uint32_t material = 0;      // index into Model::materials
  uint32_t vertexBuffer = 0;  // VBUF entry, and index into Model::vertexBuffers
  uint32_t indexBuffer = 0;   // IBUF entry
  uint32_t indexStart = 0;    // first index, in elements not bytes
  uint32_t indexCount = 0;
  uint32_t indexWidth = 0;    // bytes per index as stored: 1, 2 or 4
  uint32_t vertexCount = 0;   // vertices in the buffer this mesh draws
  uint16_t unkC = 0;
  uint16_t unkE = 0;
  // The mesh's entry in the two-bit bitmap after the meshes: its class (0 opaque, 1 sorted
  // alpha, 2 alpha tested, 3 sorted additive). Blending comes from this, not from MTRL flags.
  uint8_t bits2 = 0;
  bool twoSided = false;  // the one-bit bitmap: Remastered draws it with culling off
  std::vector<uint32_t> indices;
};

// One of the five ranges an LOD entry declares, flattened: a run of
// Model::lodMeshes. The file stores five per LOD, of which the game uses one or two
// (in the room models looked at, the first and last agree, as do the second and
// fourth, and both pairs list the same meshes).
struct ModelLod {
  uint32_t indexOffset = 0;
  uint32_t indexCount = 0;
};

// A parsed model.
struct Model {
  uint32_t form = 0;  // 'CMDL', 'SMDL' or 'WMDL'
  uint32_t readerVersion = 0;
  uint32_t writerVersion = 0;
  bool skinned = false;  // the file carried an SKHD chunk, so it is a skinned model
  uint32_t headerUnknown = 0;
  float boundsMin[3] = {0.0f, 0.0f, 0.0f};
  float boundsMax[3] = {0.0f, 0.0f, 0.0f};
  std::vector<ModelVertexBuffer> vertexBuffers;
  std::vector<ModelMesh> meshes;
  std::vector<ModelMaterial> materials;
  // The MESH chunk's table of mesh indices (retrotool calls it the shorts table):
  // every level of detail is a set of meshes of its own, and each range in lods
  // is a run of this table naming the meshes that level draws.
  std::vector<uint16_t> lodMeshes;
  std::vector<ModelLod> lods;      // five ranges per LOD entry, in file order
  std::vector<float> lodRules;     // distance thresholds, when the file has them
  // The HEAD chunk's ANUV sub-chunk after its tag, up to the chunk's end (parsed by
  // port_remastered_anuv.h); empty when the model animates no UVs.
  std::vector<uint8_t> anuv;
  // The HEAD chunk's WIND sub-chunk: SWindSet rows (v1 xyz, v2 xyz, rate, b, c) and one
  // set index per material (0xff = none); both empty when the model has no wind data.
  std::vector<std::array<float, 9>> windSets;
  std::vector<uint8_t> windMaterialSet;
};

// Parses one extracted model resource. `size` is the length of `data`, which has to
// be the RFRM form followed by a FOOT form carrying META, as PortRemastered::Pak
// writes it. On success `out` is replaced; on failure it is left empty and `error`
// says why.
bool ParseModel(const uint8_t* data, size_t size, Model& out, std::string& error);

}  // namespace PortRemastered