#pragma once

#include "port_dread_bctex.h"
#include "port_dread_bsmat.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_image.h"

#include <cstdint>
#include <string>
#include <vector>

// Maps a Metroid Dread Samus suit material (BSMAT `mp_samuspbr` + its _bc/_at/_nm BCTEX) onto
// the port's Remastered PBR path (step 5 of the Dread Samus plan). The result is a SYNTHETIC
// Remastered ModelMaterial (the standard lit PBR shader, BCLR/METL/NMAP/ICAN + ICNC) and the
// decoded RGBA8 images it names, so Dread meshes can go through PortRemastered::Converter
// unchanged: ConvertIO::texture is answered by FindTexture(). Shader channel roles and the
// emissive formula: build/mpr/kb/material/dread-mp-samuspbr.md. Nothing here is Dread data;
// the images are made at run time from the user's own game files.
namespace PortDread {

// The Remastered standard lit PBR shader (a model's first 4 shaderId bytes, read big-endian).
constexpr uint32_t kSuitShaderId = 0xBFB300B6;

// Texture slots of a suit material, as the synthetic id's second hash input.
enum SuitSlot : uint32_t { kSuitBclr = 0, kSuitMetl = 1, kSuitNmap = 2, kSuitIcan = 3 };

struct SuitImage {
  PortRemastered::ModelUuid id{};  // pak byte order: what ConvertIO::texture is asked for
  PortRemastered::Image image;
};

struct SuitMaterial {
  std::string name;                          // the BSMAT's material name
  PortRemastered::ModelMaterial material;    // synthetic; its texture ids are in MODEL byte order
  std::vector<SuitImage> images;             // BCLR, METL, NMAP (and ICAN when there is emissive)
  bool cullFront = false;                    // the BSMAT's cull mode is FRONT (recorded, not interpreted)
  float emissiveLinear[3] = {0, 0, 0};       // the unnormalised linear emissive colour C (HDR)
};

// The id of a material's texture slot in pak byte order: a deterministic hash of (name, slot),
// never all zero, distinct per slot.
PortRemastered::ModelUuid SuitTextureId(const std::string& materialName, SuitSlot slot);
// The same id with the first three groups swapped to model byte order (what a ModelTextureRef
// holds; the converter swaps it back before asking ConvertIO::texture).
PortRemastered::ModelUuid ToModelOrder(const PortRemastered::ModelUuid& pakOrder);

// Builds from exactly system/shd/mp_samuspbr.bshdat or mp_opaque_constant_selfilum.bshdat.
// The latter bakes alpha^2.2 emissive with roughness floor .05 and requires disabled tint/frostbite;
// this is texture-baked fidelity, not Dread's dynamic shading (see docs/DREAD_SAMUS.md).
// Fails (false, *error) for other shaders, unsupported state or a texture that does not decode. Images
// of different sizes are fine: each map keeps its own. Missing uniforms take the shader's
// defaults (vConstant0.w 1, vConstant4.r 1, everything else 0).
// What Dread sets at run time and the BSMAT leaves at zero (the cannon's beam colour).
struct SuitOptions {
  const float* emissiveColor = nullptr;  // rgb in place of vAlbedoEmissiveColor
  // Above 0, the ICNC strength the ICAN colour is normalised by (one for every beam's copy, so
  // only ICAN differs); 0 for the colour's own peak.
  double strength = 0.0;
  std::string icanKey;  // tells this copy's ICAN id from the material's own
};

// The linear self-illumination colour C (below), the beam colour in place of the BSMAT's when set.
void SuitEmissive(const Bsmat& bsmat, const float* emissiveColor, double out[3]);

bool BuildSuitMaterial(const Bsmat& bsmat, const Bctex& bc, const Bctex& at, const Bctex& nm,
                       SuitMaterial& out, std::string* error, const SuitOptions& options = {});

// The three BCTEX paths a BSMAT references, joined onto `texturesRoot` (the directory holding
// `actors/...`): the sampler filepaths of texBaseColor / texAttributes / texNormals. False with
// *error when one is missing or empty.
bool SuitTexturePaths(const Bsmat& bsmat, const std::string& texturesRoot, std::string& bc,
                      std::string& at, std::string& nm, std::string* error);

// For a ConvertIO::texture lambda: the image whose synthetic pak-order id is `pakOrderId`.
bool FindTexture(const std::vector<SuitMaterial>& materials, const PortRemastered::ModelUuid& pakOrderId,
                 PortRemastered::Image& out);

} // namespace PortDread
