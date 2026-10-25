#pragma once

// Reads a Metroid Prime Remastered particle effect (a "GENP" asset, the
// successor of retail's PART) into a tree of properties.
//
// A GENP is an RFRM form holding one GPSM, the generator. A GPSM is a flat list
// of properties, each a FourCC, a tag byte and a value, ended by _END. The tag
// says when the value is evaluated: 00 once (static), 01 per emitter, 03 per
// particle; _END carries 04. The root's _END is followed by a count and that
// many embedded children, each a 16-byte id and a form: another GPSM (retail's
// child generators: ICTS, IDTS, IITS...), or one of the swoosh, electric,
// weapon, collision and decal forms (SWSH, ELC2/ELSM, WPSM, CRSM, DPSM, plus
// EPSM and SPSM), which replace retail's separate SWHC/ELSC/WPSC/CRSC/DPSC
// assets.
//
// Values are retail's element trees (CNST, RAND, KEYE, CHAN...) plus the
// elements Remastered added. Nothing in the file records an element's arity or
// a value's length, and a FourCC means different things in different slots
// (CNST is an int, a float, three or four sub-elements, a byte or a GUID), so
// the reader is a backtracking parse: every reading the grammar allows is
// tried, and the one that lets the rest of the file parse to its last _END is
// kept. Positions are memoised, so this stays fast. The grammar was learned
// from the shipped files; docs/REMASTERED_EFFECTS.md lists it and what does
// not parse yet.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port_bytes.h"

namespace PortRemastered {

// FourCCs here are packed big-endian, as written: 'CNST' = 0x434E5354.
constexpr uint32_t EffectFourCC(const char (&text)[5]) { return port::FourCC(text); }

using EffectGuid = std::array<uint8_t, 16>;

// One argument of a property or element.
struct EffectValue {
  enum class Kind : uint8_t {
    Element, // a FourCC with its own arguments in `args`
    Byte,    // a raw u8 (a flag), in `word`
    Word,    // a raw 4 bytes (an int or float CNST, a sort key), in `word`
    Guid,    // an asset or parameter id, in `guid`
    Keys,    // a keyframe block (KEYE/KEYP/KEYF...), bytes at `offset`
    Raw,     // bytes the reader does not break down (KSSM, PVAR, GRAD...)
  };
  Kind kind = Kind::Element;
  uint32_t fourcc = 0;
  uint32_t word = 0;
  EffectGuid guid{};
  size_t offset = 0; // where the value starts in the file
  size_t size = 0;   // its length in bytes
  std::vector<EffectValue> args;
};

struct EffectProperty {
  uint32_t fourcc = 0;
  uint8_t tag = 0; // 00 static, 01 per emitter, 03 per particle
  size_t offset = 0;
  size_t size = 0; // FourCC and tag included
  std::vector<EffectValue> value;
};

struct EffectNode {
  uint32_t form = 0;  // GPSM, SWSH, ELC2...
  EffectGuid id{};    // the id the parent lists it under (zero for the root)
  bool root = false;
  // GPSM's header word is primitive type + 1 (LoadGeneratorDesc 0x256880).
  // -1 means unavailable/invalid; 1 is a sprite. Kept separately from parser child-list handling.
  int primitiveType = -1;
  std::vector<EffectProperty> properties;
  std::vector<EffectNode> children;
};

// Parses a GENP file (RFRM header included; a FOOT after the form is ignored).
// On failure `error` says why and `failOffset`, if given, is the furthest
// property start the parse reached, which is where to look for the element or
// property the grammar does not know yet.
bool ParseEffect(const uint8_t* data, size_t size, EffectNode& out, std::string& error,
                 size_t* failOffset = nullptr);

// Walks a WPSM or CRSM resource's top-level DESC properties using only the
// form-specific factory allowlist (global property names are not sufficient).
// `data` may also include FOOT/metadata bytes appended by Pak::ReadAsset; only
// the length-checked RFRM/DESC form is parsed. The root walk is iterative and
// capped at 1024 properties; typed expressions are capped at depth 64 and 4096
// value nodes. Nested child/property payloads are not exposed as role fields.
bool ParseRemasteredRoleAsset(const uint8_t* data, size_t size, uint32_t expectedForm, EffectNode& out,
                             std::string& error);

// Walks a retail WPSC/WPSM or CRSC/CRSM tagged-field stream using the field
// value schemas consumed by the corresponding game factories. Returns the
// unique top-level asset id for `field`; malformed, truncated, unknown, or
// ambiguous streams fail closed. As in the factories, bytes after _END are
// outside the tagged-field stream and are not inspected.
bool ReadRetailRoleAssetId(const uint8_t* data, size_t size, uint32_t expectedClass, uint32_t field,
                           uint32_t& out, std::string* error = nullptr);

// A KSSM spawn table: the child systems to start on given frames. Retail has
// one table; Remastered may hold several, each behind a selector element.
struct EffectSpawnTable {
  struct Spawn {
    EffectGuid id{};           // an embedded child's id
    uint32_t form = 0;         // its form's type ('GENP' or 'SWSH')
    uint32_t component = ~0u;  // the component it is placed by, ~0 for none
    bool conditional = false;  // an instance count element instead of NONE
    EffectValue instances;     // that element (NONE: one instance)
  };
  struct Frame {
    uint32_t frame = 0;
    std::vector<Spawn> spawns;
  };
  struct Table {
    uint32_t word = 0;     // the start frame of its period
    EffectValue selector;  // the period: CNST (0 = none), or a parameter (TPVI) with a default
    std::vector<Frame> frames;
  };
  uint32_t header[3]{};  // retail's first three words; the third is the end frame
  uint32_t events = 0;   // SEVT events (Remastered only)
  std::vector<EffectValue> components;  // those events, indexed by Spawn::component
  std::vector<Table> tables;  // empty for NONE
};

// Breaks down a KSSM property of an effect parsed from `data`.
bool ParseSpawnTable(const uint8_t* data, size_t size, const EffectProperty& kssm, EffectSpawnTable& out);

// A PMTR or SMTR: values a generator hands its material instance, in five
// groups. Each item is an element and a trailer of 3 bytes in PMTR (the slot,
// 0, then 4 for the vec4s of group 4) or 6 in SMTR.
struct EffectMaterialTrack {
  struct Item {
    int group = 0;
    std::array<uint8_t, 6> trailer{};
    EffectValue value;
  };
  std::vector<Item> items;
};

// Breaks down a PMTR or SMTR property of an effect parsed from `data`.
bool ParseMaterialTrack(const uint8_t* data, size_t size, const EffectProperty& property, EffectMaterialTrack& out);


// Indented text dump of a parsed effect, one property per line, for diffing
// against retail PART dumps.
std::string DumpEffect(const EffectNode& effect, const uint8_t* data);
// One value of an effect parsed from `data`, as DumpEffect writes it.
std::string DumpEffectValue(const EffectValue& value, const uint8_t* data);

// Every GUID the effect refers to (textures, materials, models, sounds,
// parameter ids), in file order, without duplicates. Embedded children are
// not listed; their ids are in EffectNode::id.
std::vector<EffectGuid> EffectReferences(const EffectNode& effect);

std::string EffectGuidString(const EffectGuid& guid);
std::string EffectFourCCString(uint32_t fourcc);

} // namespace PortRemastered
