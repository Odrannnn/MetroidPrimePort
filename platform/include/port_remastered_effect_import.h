#pragma once

// The Remastered import's effect step: every Remastered particle effect that
// stands for one of the disc's PARTs is converted (port_remastered_effect_convert.h)
// and written into the mod as "<ID>.PART", replacing the disc's.
//
// An effect stands for a disc PART when its id is one carried over from
// retail (EffectRetailId) and the disc has that id. Its embedded children are
// written as PARTs of their own under new ids. Textures it names that are not
// on the disc (a material instance's, or a TXTR of Remastered's own) are
// converted and written as "<ID>.TXTR" under new ids.
//
// The step is off unless the import menu's toggle or MP_REMASTERED_EFFECTS=1
// turns it on: the conversion leaves out what retail cannot draw, and some of
// its mappings are not yet confirmed in game (docs/REMASTERED_EFFECTS.md).

#include "port_remastered_effect.h"
#include "port_remastered_report.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PortRemastered {

// A retail PART (or SWHC) an effect replaces that the import found by itself, not from the
// table in the importer: how `method` says.
struct EffectPairing {
  EffectGuid id;  // in a pak's byte order
  uint32_t retail;
  std::string method;
};

// Ids are in a pak's byte order (what IdToString prints) unless said otherwise.
struct EffectImportIO {
  // Every GENP and standalone swoosh (SWSH) in the image, each once.
  std::vector<EffectGuid> effects;
  // Optional: pairings the image itself gives (a projectile's particles, from its WPSM and the
  // disc's WPSC), used after the importer's own table.
  std::vector<EffectPairing> pairings;
  // A Remastered asset's bytes by type ('GENP', 'MATI', 'TXTR') and id; false
  // when the image has none.
  std::function<bool(uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error)> read;
  // The asset type of an id in the image, or 0.
  std::function<uint32_t(const EffectGuid& id)> typeOf;
  // Whether the disc has a resource with this id.
  std::function<bool(uint32_t id)> retailId;
  // A disc resource's bytes by type and id; false when the disc has none.
  // Optional: without it a converted effect keeps its own light.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // A new id for a resource of the import, never one the disc or the import
  // already has; `seed` makes it the same in every import.
  std::function<uint32_t(uint32_t seed)> freshId;
  // A Remastered texture's top mip as RGBA8, as ConvertIO::texture.
  std::function<bool(const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba, std::string& error)>
      texture;
  // Whether the texture's bytes are sRGB, which Remastered's VFX shaders see decoded to
  // linear. Optional: without it every texture is taken as data.
  std::function<bool(const EffectGuid& id)> textureSrgb;
  // Every layer of a Remastered array texture as RGBA8, layer slowest (a TXFB's
  // frames). Optional: without it a flipbook does not convert.
  std::function<bool(const EffectGuid& id, int& width, int& height, int& layers, std::vector<uint8_t>& rgba,
                     std::string& error)>
      layers;
  // Converts a Remastered-only model (a CMDL the disc has no id for) as a
  // standalone CMDL written under `retailId`, with the textures it needs.
  // Optional: without it such a model does not convert.
  std::function<bool(const EffectGuid& id, uint32_t retailId, std::string& error)> model;
  // Optional: the VMSH blob of a model `model` converted under `retailId` (empty for any
  // other id), so a model particle with a VMAT carries its mesh.
  std::function<std::vector<uint8_t>(uint32_t retailId)> modelMesh;
  // Stores one output file: "<ID>.PART", "<ID>.TXTR" or "<ID>.CMDL".
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;  // optional
  // Optional: called once per effect considered, in `effects` order.
  std::function<void(const EffectReportRow& row)> report;
};

struct EffectImportResult {
  int candidates = 0;  // effects standing for a disc PART
  int written = 0;     // of those, written (root and children)
  int failed = 0;      // that did not parse or convert
  int parts = 0;       // PART files written, children included
  int textures = 0;    // TXTR files written, flipbook atlases included
  int flipbooks = 0;   // of those, atlases of an array texture's layers
  int models = 0;      // Remastered-only models converted for the effects
  int dropped = 0;     // retail properties left out across the written PARTs
};

// Whether the next import converts effects: MP_REMASTERED_EFFECTS when set
// ("1" on, anything else off), else what SetImportEffects() last said (off).
bool WantsRemasteredEffects();
void SetImportEffects(bool on);

EffectImportResult ImportEffects(const EffectImportIO& io);

}  // namespace PortRemastered

