#pragma once

// Writes a Remastered particle effect (a parsed GENP, port_remastered_effect.h)
// as retail PART: the big-endian GPSM stream CParticleDataFactory reads (and
// its swoosh and electric children as SWHC and ELSC).
//
// The conversion is driven by retail's own reader: each property retail knows
// is read as the type retail reads it as (int, real, vector, mod vector,
// colour, emitter, texture, bool or an asset id), and each element in it must
// be one retail has for that type, with retail's arguments. Remastered's
// re-encodings are undone on the way (docs/REMASTERED_EFFECTS.md):
//
// - LTM2 is written as LTME, one frame shorter; LFOT and LTYP enum bytes go
//   back down by one.
// - MPCB around a vector is stripped; its angle form (MPAC) becomes retail's
//   ANGC. MPRD becomes RAND.
// - ROTA is negated back.
// - TEXR's `CNST(id), NONE` becomes retail's `CNST(CNST id)`, and an MTIN
//   material stands in for a missing TEXR through its texture.
// - Ids become 32-bit retail ids through ConvertIO.
// - ANCR and ASPR become the port's own elements of the same fourccs, with
//   their REUL (axis order 0) as three angle reals (CVEPortAngleCone,
//   CEEPortAngleSphere); RNDV and GRAD likewise run Remastered's own code.
// - DFCP/DFCS (a ramp of the depth from the camera) are written as the port's own
//   real elements of the same fourccs (CREDistanceFromCameraBlend).
// - Keyframe blocks and words are byte-swapped; a colour's half keys are
//   widened to floats.
// - Port-only (build/fx-port-contract.md): a TEXR or TIND of TXP2 (an atlas with a
//   random tile) or TXFB (an array texture played over the particle's life, with
//   an optional random mirror in its TRST) or ATX2 (an atlas played over the
//   particle's life) becomes PATL; a PMDL of SLCT(IRND, ARRY
//   of ids) becomes the first model as PMDL and all of them as PMDV.
// - Port-only (build/mpr/vfx/DESIGN.md): an MTIN whose MATI shader has a recipe
//   (RECIPES.md) also writes VMAT (the shader's features, textures and uniform
//   sources), VTMT (TMTR's UV transforms), VPMT (PMTR) and VSMT (SMTR's CCH0
//   uniforms), SSZE, VORN (ORNT), ITEN and SCTR (as VMAT's sprite centre). A
//   shader with no recipe writes none and is listed in `approximated`.
// - Port-only (build/mpr/vfx/RESOLVED-xfmd.md): XFMD 3 and 4 (particles that
//   follow the emitter) are written as they are; SMVR is read only as XFMD 4's
//   mover.
//
// A property that does not convert (a Remastered-only property, an element
// retail does not have, an id with no retail id) is left out and listed in
// `dropped`, so the caller can decide whether the effect is still worth
// writing. Retail fills a left-out property with its default.
//
// Embedded children come out as files of their own, under the ids ConvertIO
// gives their child ids: generators (GPSM) as PART, swooshes (SWSH) as SWHC
// (CParticleSwooshDataFactory's SWSH stream, Remastered's SBDM blend mode as
// AALP) and electric effects (ELC2, ELSM) as ELSC (CParticleElectricDataFactory's
// ELSM stream). An ELC2 starts with the port-only marker PEL2 and carries its
// own fields (SIDE, RADS, LRAD, ..., see Elc2Properties) plus the VMAT material,
// so the runtime draws it as CParticleElectric2's camera-facing ribbons. The
// other embedded forms (weapon, collision, decal) and KSSM spawn tables are not
// converted yet.

#include "port_remastered_effect.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace PortRemastered {

// An atlas of `cols` x `rows` tiles holding `frames` frames, row-major from the top.
struct FlipbookAtlas {
  uint32_t id = 0;  // the retail TXTR, or 0
  int32_t cols = 0;
  int32_t rows = 0;
  int32_t frames = 0;
};

struct EffectConvertIO {
  // A PART under XFMD 2 or 5 with no ITPE gets Remastered's default 1 (a PITP flagged as the
  // default). The import sets it; left false, an absent ITPE writes nothing.
  bool spreadDefault = false;
  // The retail id an asset id stands for, as the type it is read as ('TXTR',
  // 'CMDL', 'PART', 'SWHC', 'ELSC'), or 0 when it has none. Left empty, only
  // ids carried over from retail (EffectRetailId) resolve.
  std::function<uint32_t(const EffectGuid& id, uint32_t type)> assetId;
  // The retail TXTR id of a material instance's texture (MTIN), or 0.
  std::function<uint32_t(const EffectGuid& material)> materialTexture;
  // An array TXTR (TXFB's flipbook) packed into one atlas TXTR, or id 0 when it cannot be.
  // Left empty, a TXFB does not convert.
  std::function<FlipbookAtlas(const EffectGuid& texture)> flipbook;
  // The MATI file of a material instance (MTIN), or empty. Left empty, no VMAT is written.
  std::function<std::vector<uint8_t>(const EffectGuid& material)> materialData;
  // A texture a VMAT slot draws, imported as a TXTR the import writes: a single texture comes
  // back with cols = rows = frames = 1, an array packed as by `flipbook`. Id 0: it cannot be.
  std::function<FlipbookAtlas(const EffectGuid& texture)> vfxTexture;
  // The VMSH blob of a converted model (one the import wrote under this retail CMDL id), or
  // empty for one that is not (a disc model). Left empty, no VMSH is written.
  std::function<std::vector<uint8_t>(uint32_t model)> modelMesh;
};

struct ConvertedPart {
  EffectGuid id{};  // the child id it was embedded under; zero for the root
  uint32_t type = 0;  // the retail asset type: 'PART', 'SWHC' or 'ELSC'
  bool root = false;  // the effect's own PART, not an embedded child
  bool drawsNothing = false;  // inactive primitive or proven null sprite material resource
  std::vector<uint8_t> part;          // the retail file, of that type
  std::vector<std::string> dropped;   // "FOURCC: why", one per left-out property
  // Properties retail reads that were left out. Remastered-only ones are not
  // counted: retail never had them.
  int droppedRetail = 0;
  // Values written as an approximation of something retail cannot do.
  std::vector<std::string> approximated;
};

// The retail id an id carried over from retail holds: Remastered writes those
// as 10000000-0000-f000-f000-0000XXXXXXXX (in pak order).
std::optional<uint32_t> EffectRetailId(const EffectGuid& id);

// The retail asset type an embedded form converts to ('PART' for GPSM, 'SWHC'
// for SWSH, 'ELSC' for ELC2 and ELSM), or 0 for one that is not converted.
uint32_t EffectRetailType(uint32_t form);

// The root and every embedded child that converts, root first. `data` is the
// GENP the effect was parsed from (keyframe blocks are copied out of it).
std::vector<ConvertedPart> ConvertEffect(const EffectNode& effect, const uint8_t* data, const EffectConvertIO& io);

// One property of a retail PART: its FourCC and its value's bytes.
struct RetailPartProperty {
  uint32_t fourcc = 0;
  std::vector<uint8_t> value;
};

// Splits a retail PART into its properties, reading each as CParticleDataFactory
// does. False (with the property that does not read in `error`) for a file
// retail's reader would not take. Checks the converter's output, and lets
// a converted effect be compared property by property with the disc's.
bool SplitRetailPart(const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out, std::string& error);

// The same for any type EffectRetailType gives: a PART, or a SWHC or ELSC read
// as CParticleSwooshDataFactory and CParticleElectricDataFactory do.
bool SplitRetailEffect(uint32_t type, const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out,
                       std::string& error);

// A MATI's parameters as the effect converter reads them: each texture's GUID, in the
// MATI's byte order, and each vec4 constant, by tag. False when `mati` is too short to be one.
struct MaterialParams {
  uint32_t shader = 0;  // the shader id, big-endian as the MATI holds it (700e8e71 for the icy one)
  std::map<std::string, EffectGuid> textures;
  std::map<std::string, std::array<float, 4>> vectors;
};
bool ReadMaterialParams(const std::vector<uint8_t>& mati, MaterialParams& out);

}  // namespace PortRemastered
