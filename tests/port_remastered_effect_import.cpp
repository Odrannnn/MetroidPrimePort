#include "port_remastered_effect_import.h"

#include "port_remastered_effect_convert.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t kGenp = EffectFourCC("GENP");
constexpr uint32_t kElsm = EffectFourCC("ELSM");
constexpr uint32_t kSwsh = EffectFourCC("SWSH");
constexpr uint32_t kSwhc = EffectFourCC("SWHC");
constexpr uint32_t kMati = EffectFourCC("MATI");
constexpr uint32_t kTxtr = EffectFourCC("TXTR");
constexpr uint32_t kVecf = EffectFourCC("VECF");

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void Put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void Set32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.at(at + size_t(i)) = uint8_t(value >> (i * 8));
}

void Set64(std::vector<uint8_t>& out, size_t at, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.at(at + size_t(i)) = uint8_t(value >> (i * 8));
}

uint64_t Get64(const std::vector<uint8_t>& data, size_t at) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= uint64_t(data.at(at + size_t(i))) << (i * 8);
  return value;
}

void PutFourCC(std::vector<uint8_t>& out, const char* text) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(text[i]));
  }
}

void PutRawFourCC(std::vector<uint8_t>& out, const char* text) {
  out.insert(out.end(), text, text + 4);
}

void PutProperty(std::vector<uint8_t>& out, const char* name, uint8_t tag) {
  PutFourCC(out, name);
  out.push_back(tag);
}

void PutGenerator(std::vector<uint8_t>& out, bool root) {
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, root ? 2 : 0); // GPSM sprite primitive + 1
}

void PutGuid(std::vector<uint8_t>& out, const EffectGuid& id);
EffectGuid Fresh(uint8_t seed);

std::vector<uint8_t> MakeChildForm(const char* form) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, form, 4);
  Set32(out, 24, 61);
  Set32(out, 28, 62);
  std::memcpy(out.data() + 0x20, "DESC", 4);
  Set32(out, 44, 1);
  std::memcpy(out.data() + 0x38, "\x19\x6e\x01\x00", 4);
  PutFourCC(out, form);
  out.resize(0x3c + 21, 0);
  return out;
}

void FinishChildForm(std::vector<uint8_t>& out) {
  Set64(out, 4, out.size() - 32);
  Set64(out, 36, out.size() - 56);
}

void AppendExtractedFoot(std::vector<uint8_t>& out) {
  const size_t start = out.size();
  out.resize(start + 32, 0);
  std::memcpy(out.data() + start, "RFRM", 4);
  std::memcpy(out.data() + start + 20, "FOOT", 4);
  Set32(out, start + 24, 1);
  Set32(out, start + 28, 1);
  const auto appendChunk = [&](const char* id, size_t payloadSize) {
    PutRawFourCC(out, id);
    Put64(out, payloadSize);
    Put32(out, 0);
    Put64(out, 0);
    out.resize(out.size() + payloadSize, 0);
  };
  appendChunk("AINF", 28);
  appendChunk("META", 8);
  Set64(out, start + 4, out.size() - start - 32);
}

std::vector<uint8_t> MakeElectric(const EffectGuid& swoosh, int references = 1) {
  std::vector<uint8_t> out = MakeChildForm("ELSM");
  for (int i = 0; i < references; ++i) {
    PutProperty(out, "SSWH", 0);
    PutGuid(out, swoosh);
  }
  PutProperty(out, "_END", 4);
  FinishChildForm(out);
  return out;
}

std::vector<uint8_t> MakeElectricRefs(const std::vector<EffectGuid>& swooshes) {
  std::vector<uint8_t> out = MakeChildForm("ELSM");
  for (const EffectGuid& swoosh : swooshes) {
    PutProperty(out, "SSWH", 0);
    PutGuid(out, swoosh);
  }
  PutProperty(out, "_END", 4);
  FinishChildForm(out);
  return out;
}

std::vector<uint8_t> MakeSwoosh() {
  std::vector<uint8_t> out = MakeChildForm("SWSH");
  PutProperty(out, "LENG", 1);
  PutFourCC(out, "CNST");
  Put32(out, 1);
  PutProperty(out, "_END", 4);
  FinishChildForm(out);
  AppendExtractedFoot(out);
  return out;
}

std::vector<uint8_t> MakeSwooshWithSswh(const EffectGuid& nestedSwoosh) {
  std::vector<uint8_t> out = MakeChildForm("SWSH");
  PutProperty(out, "LENG", 1);
  PutFourCC(out, "CNST");
  Put32(out, 1);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, nestedSwoosh);
  PutProperty(out, "_END", 4);
  FinishChildForm(out);
  AppendExtractedFoot(out);
  return out;
}

std::vector<uint8_t> MakeSwooshWithTrailingPayload(const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out = MakeSwoosh();
  const size_t formEnd = 32 + size_t(Get64(out, 4));
  out.resize(formEnd);
  out.insert(out.end(), payload.begin(), payload.end());
  FinishChildForm(out);
  AppendExtractedFoot(out);
  return out;
}

std::vector<uint8_t> MakeGeneratorWithEmbeddedSwoosh(const EffectGuid& child) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, child);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, child);
  PutFourCC(out, "SWSH");
  out.resize(out.size() + 17, 0);
  PutProperty(out, "LENG", 1);
  PutFourCC(out, "CNST");
  Put32(out, 1);
  PutProperty(out, "_END", 4);
  return out;
}

std::vector<uint8_t> MakeGeneratorWithWrongSwooshChild(const EffectGuid& child) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, child);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, child);
  PutGenerator(out, false);
  PutProperty(out, "_END", 4);
  return out;
}

void PutGuid(std::vector<uint8_t>& out, const EffectGuid& id) { out.insert(out.end(), id.begin(), id.end()); }

// Between a pak's byte order and an effect's.
EffectGuid Swap(const EffectGuid& id) {
  EffectGuid out = id;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

// An id carried over from retail, as an effect stores it.
EffectGuid Legacy(uint32_t retail) {
  EffectGuid id = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  for (int i = 0; i < 4; ++i) {
    id[size_t(12 + i)] = uint8_t(retail >> (24 - 8 * i));
  }
  return id;
}

EffectGuid Fresh(uint8_t seed) {
  EffectGuid id;
  for (size_t i = 0; i < id.size(); ++i) {
    id[i] = uint8_t(0xa0 + seed + i);
  }
  return id;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string HexId(uint32_t id) {
  char text[9];
  std::snprintf(text, sizeof(text), "%08X", id);
  return text;
}

// An effect carried over from retail PART 0x00001234: an embedded child it
// spawns through ICTS, and a material instance whose texture is Remastered's own.
void TestImport() {
  const EffectGuid child = Fresh(0x40);
  const EffectGuid material = Fresh(0x10);  // as the effect stores it
  const EffectGuid texture = Fresh(0x20);   // as the material stores it
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "ICTS", 1);
  PutFourCC(genp, "CNST");
  PutGuid(genp, child);
  PutProperty(genp, "MTIN", 1);
  genp.push_back(0);
  PutGuid(genp, material);
  PutProperty(genp, "LTYP", 0);
  genp.push_back(2);
  PutProperty(genp, "LFOT", 0);
  genp.push_back(2);  // retail 1; the disc's 2 wins
  PutProperty(genp, "_END", 4);
  Put32(genp, 1);
  PutGuid(genp, child);
  PutGenerator(genp, false);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 2);
  PutProperty(genp, "_END", 4);

  std::vector<uint8_t> mati(8, 0);
  PutGuid(mati, texture);

  const EffectGuid effectId = Swap(Legacy(0x1234));  // in pak order
  const EffectGuid unrelated = Fresh(0x60);           // not carried over: skipped
  std::map<std::string, std::vector<uint8_t>> written;
  EffectImportIO io;
  io.effects = {effectId, unrelated};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    if (type == kMati && id == Swap(material)) {
      out = mati;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    return id == Swap(texture) ? kTxtr : id == Swap(material) ? kMati : 0;
  };
  io.retailId = [](uint32_t id) { return id == 0x1234; };
  // The disc's PART: its light replaces the converted one, the rest is ignored.
  std::vector<uint8_t> disc;
  for (uint32_t word : {EffectFourCC("GPSM"), EffectFourCC("MAXP"), EffectFourCC("CNST"), 9u, EffectFourCC("LTYP"),
                        EffectFourCC("CNST"), 1u, EffectFourCC("LFOT"), EffectFourCC("CNST"), 2u,
                        EffectFourCC("LFOR"), EffectFourCC("CNST"), 0x40400000u, EffectFourCC("_END")}) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      disc.push_back(uint8_t(word >> shift));
    }
  }
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    if (type != EffectFourCC("PART") || id != 0x1234) {
      return false;
    }
    out = disc;
    return true;
  };
  uint32_t next = 0x00ABC000;
  io.freshId = [&](uint32_t) { return next++; };
  io.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba, std::string& error) {
    if (id != Swap(texture)) {
      error = "no such texture";
      return false;
    }
    width = 512;  // written as a .dds, with a 64-texel TXTR stub
    height = 512;
    rgba.assign(size_t(width) * height * 4, 0xff);
    return true;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  CHECK_MSG(result.candidates == 1 && result.written == 1 && result.failed == 0, "the carried-over effect is written");
  CHECK_MSG(result.parts == 2 && result.textures == 1, "its child and texture are written");
  CHECK_MSG(written.size() == 4 && written.count("00001234.PART") == 1 && written.count("00ABC000.PART") == 1 &&
            written.count("00ABC001.TXTR") == 1 && written.count("00ABC001.dds") == 1,
        "files are named by their retail and new ids");
  if (written.count("00001234.PART") == 0) {
    return;
  }
  std::vector<RetailPartProperty> root;
  std::string error;
  const std::vector<uint8_t>& part = written["00001234.PART"];
  CHECK_MSG(SplitRetailPart(part.data(), part.size(), root, error), "the root reads as a PART");
  bool spawnsChild = false;
  bool drawsTexture = false;
  int lights = 0;
  uint32_t maxp = 0;
  uint32_t lfot = 0;
  uint32_t lfor = 0;
  for (const RetailPartProperty& property : root) {
    const uint32_t word = property.value.size() == 8 ? Be32(property.value.data() + 4) : 0;
    lights += property.fourcc == EffectFourCC("LTYP");
    maxp = property.fourcc == EffectFourCC("MAXP") ? word : maxp;
    lfot = property.fourcc == EffectFourCC("LFOT") ? word : lfot;
    lfor = property.fourcc == EffectFourCC("LFOR") ? word : lfor;
    if (property.fourcc == EffectFourCC("ICTS")) {
      spawnsChild = property.value.size() == 8 && Be32(property.value.data() + 4) == 0x00ABC000;
    }
    if (property.fourcc == EffectFourCC("TEXR")) {
      drawsTexture = property.value.size() == 12 && Be32(property.value.data() + 8) == 0x00ABC001;
    }
  }
  CHECK_MSG(spawnsChild, "the root spawns its child by the child's new id");
  CHECK_MSG(drawsTexture, "the root draws its material's texture by its new id");
  CHECK_MSG(lights == 1 && lfot == 2 && lfor == 0x40400000u, "the root's light is the disc's");
  CHECK_MSG(maxp == 5, "the root keeps its other properties");
  const std::vector<uint8_t>& txtr = written["00ABC001.TXTR"];
  CHECK_MSG(txtr.size() > 12 && Be32(txtr.data()) == 9 && (txtr[4] << 8 | txtr[5]) == 64 && (txtr[6] << 8 | txtr[7]) == 64,
            "the texture's TXTR is an RGBA8 stub of 64");
}

struct ExternalRun {
  EffectImportResult result;
  std::map<std::string, std::vector<uint8_t>> written;
  std::vector<EffectReportRow> rows;
  int childReads = 0;
  int childWriteAttempts = 0;
};

ExternalRun RunExternal(const std::vector<uint32_t>& retailIds, const EffectGuid& storedSwoosh, uint32_t childType,
                        const std::vector<uint8_t>& childData, int references = 1, bool failSwhcWrite = false,
                        const std::vector<uint32_t>& extraRetailIds = {}, bool failChildRead = false) {
  std::map<EffectGuid, std::vector<uint8_t>> parents;
  std::map<EffectGuid, uint32_t> parentRetail;
  for (uint32_t retail : retailIds) {
    const EffectGuid id = Swap(Legacy(retail));
    parents[id] = MakeElectric(storedSwoosh, references);
    parentRetail[id] = retail;
  }
  const EffectGuid childPak = Swap(storedSwoosh);
  std::set<uint32_t> taken(retailIds.begin(), retailIds.end());
  taken.insert(extraRetailIds.begin(), extraRetailIds.end());
  bool forceCollision = true;
  ExternalRun out;
  EffectImportIO io;
  for (const auto& [id, bytes] : parents) {
    (void)bytes;
    io.effects.push_back(id);
  }
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& data, std::string& error) {
    if (type == kElsm && parents.count(id) != 0) {
      data = parents.at(id);
      return true;
    }
    if (type == kSwsh && id == childPak) {
      ++out.childReads;
      if (failChildRead) {
        error = "fixture read failure";
        return false;
      }
      data = childData;
      return true;
    }
    error = "not in the fixture";
    return false;
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    if (parentRetail.count(id) != 0) return kElsm;
    return id == childPak ? childType : 0;
  };
  io.retailId = [&](uint32_t id) { return taken.count(id) != 0; };
  io.freshId = [&](uint32_t seed) {
    uint32_t id = seed;
    if (forceCollision) {
      // Mirror the production allocator's reserved-id handling on the first
      // external child, then keep every generated id unique in this import.
      taken.insert(id);
      forceCollision = false;
    }
    while (id == 0 || id == 0xFFFFFFFFu || taken.count(id) != 0) ++id;
    taken.insert(id);
    return id;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() >= 5 && name.substr(name.size() - 5) == ".SWHC") {
      ++out.childWriteAttempts;
      if (failSwhcWrite) return false;
    }
    out.written[name] = data;
    return true;
  };
  io.report = [&](const EffectReportRow& row) { out.rows.push_back(row); };
  out.result = ImportEffects(io);
  return out;
}

ExternalRun RunGenerator(uint32_t retail, const std::vector<uint8_t>& source) {
  const EffectGuid parent = Swap(Legacy(retail));
  ExternalRun out;
  EffectImportIO io;
  io.effects = {parent};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& data, std::string& error) {
    if (type == kGenp && id == parent) {
      data = source;
      return true;
    }
    error = "not in the fixture";
    return false;
  };
  io.typeOf = [&](const EffectGuid& id) { return id == parent ? kGenp : 0; };
  io.retailId = [&](uint32_t id) { return id == retail; };
  io.freshId = [](uint32_t seed) { return seed == 0 ? 1u : seed; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    out.written[name] = data;
    return true;
  };
  io.report = [&](const EffectReportRow& row) { out.rows.push_back(row); };
  out.result = ImportEffects(io);
  return out;
}

bool SwooshIdInElectric(const std::vector<uint8_t>& data, uint32_t& id) {
  std::vector<RetailPartProperty> properties;
  std::string error;
  if (!SplitRetailEffect(EffectFourCC("ELSC"), data.data(), data.size(), properties, error)) return false;
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("SSWH") && property.value.size() == 8) {
      id = Be32(property.value.data() + 4);
      return true;
    }
  }
  return false;
}

void TestExternalSwoosh() {
  const EffectGuid storedSwoosh = Fresh(0x32);
  const std::vector<uint32_t> parents = {0x1234, 0x2345};
  const std::vector<uint8_t> source = MakeSwoosh();
  EffectNode sourceTree;
  std::string sourceError;
  CHECK_MSG(ParseEffect(source.data(), source.size(), sourceTree, sourceError) && sourceTree.form == kSwsh &&
                sourceTree.children.empty(),
            "a production-shaped standalone SWSH with the extracted FOOT framing parses without child-list support");
  const ExternalRun first = RunExternal(parents, storedSwoosh, kSwsh, source);
  const ExternalRun again = RunExternal(parents, storedSwoosh, kSwsh, source);
  CHECK_MSG(first.result.candidates == 2 && first.result.written == 2 && first.result.failed == 0,
            "both ELSM parents import with their external swoosh");
  CHECK_MSG(first.childReads == 2 && first.childWriteAttempts == 2, "each parent converts and writes one SWHC");
  CHECK_MSG(first.written.size() == 4 && first.written.count("00001234.ELSC") && first.written.count("00002345.ELSC"),
            "both ELSC roots and two external SWHCs are emitted");
  std::vector<uint32_t> swooshIds;
  for (uint32_t retail : parents) {
    const std::string rootName = retail == 0x1234 ? "00001234.ELSC" : "00002345.ELSC";
    uint32_t id = 0;
    CHECK_MSG(SwooshIdInElectric(first.written.at(rootName), id), "converted ELSC retains its SSWH property");
    swooshIds.push_back(id);
    const std::string childName = HexId(id) + ".SWHC";
    CHECK_MSG(first.written.count(childName) == 1, "the ELSC SSWH id has a corresponding SWHC file");
    std::vector<RetailPartProperty> properties;
    std::string error;
    CHECK_MSG(SplitRetailEffect(kSwhc, first.written.at(childName).data(), first.written.at(childName).size(), properties,
                                error),
              "the external child is a valid converted SWHC");
  }
  CHECK_MSG(swooshIds.size() == 2 && swooshIds[0] != swooshIds[1],
            "the same external child gets parent-specific ids without colliding with retail roots");
  CHECK_MSG(first.written == again.written, "external SWHC ids and output are deterministic across imports");
  CHECK_MSG(first.rows.size() == 2 && first.rows[0].kinds.find("SWHC:1") != std::string::npos,
            "the effects report accounts for the external SWHC");

  const ExternalRun duplicate = RunExternal({0x1234}, storedSwoosh, kSwsh, source, 2);
  CHECK_MSG(duplicate.result.written == 1 && duplicate.result.failed == 0 && duplicate.childReads == 1 &&
                duplicate.childWriteAttempts == 1,
            "duplicate SSWH references deduplicate conversion and output within their parent");

  const EffectGuid carriedSwoosh = Legacy(0x5678);
  const ExternalRun carried = RunExternal({0x1234}, carriedSwoosh, kSwsh, {}, 1, false, {0x5678});
  uint32_t carriedId = 0;
  CHECK_MSG(carried.result.written == 1 && carried.childReads == 0 && carried.childWriteAttempts == 0 &&
                carried.written.count("00005678.SWHC") == 0 &&
                SwooshIdInElectric(carried.written.at("00001234.ELSC"), carriedId) && carriedId == 0x5678,
            "a carried-over SWHC keeps precedence over the external lookup");
}

void TestExternalSwooshFailures() {
  const EffectGuid storedSwoosh = Fresh(0x35);
  for (const auto& [type, data, reason] : std::vector<std::tuple<uint32_t, std::vector<uint8_t>, const char*>>{
           {0, MakeSwoosh(), "is missing"}, {kMati, MakeSwoosh(), "has type MATI"},
           {kSwsh, std::vector<uint8_t>{'R', 'F', 'R', 'M'}, "parse"},
           {kSwsh, MakeElectric(EffectGuid{}), "expected SWSH"}}) {
    const ExternalRun failed = RunExternal({0x1234}, storedSwoosh, type, data);
    CHECK_MSG(failed.result.written == 0 && failed.result.failed == 1 && failed.written.count("00001234.ELSC") == 0,
              "missing, wrong-type, and truncated external children fail closed");
    CHECK_MSG(failed.rows.size() == 1 && failed.rows[0].result == "failed" &&
                  failed.rows[0].reason.find(reason) != std::string::npos,
              "external dependency failure has a clear report reason");
  }

  const ExternalRun failedWrite = RunExternal({0x1234}, storedSwoosh, kSwsh, MakeSwoosh(), 2, true);
  CHECK_MSG(failedWrite.result.failed == 1 && failedWrite.childWriteAttempts == 1 &&
                failedWrite.written.count("00001234.ELSC") == 0,
            "a failed child write is not retried and never leaves a parent ELSC reference");

  const ExternalRun failedRead = RunExternal({0x1234}, storedSwoosh, kSwsh, MakeSwoosh(), 1, false, {}, true);
  CHECK_MSG(failedRead.result.failed == 1 && failedRead.written.count("00001234.ELSC") == 0 &&
                failedRead.rows[0].reason.find("read failed") != std::string::npos,
            "a failed source read has a distinct report and emits no parent ELSC");

  for (const std::vector<uint8_t>& trailing :
       std::vector<std::vector<uint8_t>>{{0x01, 0x00, 0x00, 0x00},
                                         {0x01, 0x00, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd,
                                          'E', 'P', 'O', 'N'}}) {
    const ExternalRun malformed =
        RunExternal({0x1234}, storedSwoosh, kSwsh, MakeSwooshWithTrailingPayload(trailing));
    CHECK_MSG(malformed.result.failed == 1 && malformed.written.count("00001234.ELSC") == 0 &&
                  malformed.childWriteAttempts == 0 && malformed.rows.size() == 1 &&
                  malformed.rows[0].reason.find("unparsed dependency or trailing payload") != std::string::npos,
              "truncated or unrecognized trailing child-table payload fails before parent or child output");
  }
  std::vector<uint8_t> fakeFoot = MakeSwoosh();
  fakeFoot.push_back(0xff);
  const ExternalRun invalidFoot = RunExternal({0x1234}, storedSwoosh, kSwsh, fakeFoot);
  CHECK_MSG(invalidFoot.result.failed == 1 && invalidFoot.written.count("00001234.ELSC") == 0 &&
                invalidFoot.childWriteAttempts == 0 &&
                invalidFoot.rows[0].reason.find("bounded extracted FOOT") != std::string::npos,
            "arbitrary bytes after an otherwise valid extracted FOOT are not accepted as footer data");

  std::vector<uint8_t> legacyWithTrailing = MakeElectric(EffectGuid{});
  legacyWithTrailing.insert(legacyWithTrailing.end(), {0xde, 0xad, 0xbe, 0xef});
  EffectNode legacyTree;
  std::string legacyError;
  CHECK_MSG(ParseEffect(legacyWithTrailing.data(), legacyWithTrailing.size(), legacyTree, legacyError) &&
                legacyTree.form == kElsm,
            "the legacy non-external-SSWH ELSM parser still accepts its established trailing data");

  const EffectGuid nestedSwoosh = Fresh(0x78);
  const std::vector<uint8_t> nestedSource = MakeSwooshWithSswh(nestedSwoosh);
  EffectNode nestedTree;
  std::string nestedParseError;
  CHECK_MSG(ParseEffect(nestedSource.data(), nestedSource.size(), nestedTree, nestedParseError) &&
                nestedTree.children.empty() &&
                std::any_of(nestedTree.properties.begin(), nestedTree.properties.end(),
                            [](const EffectProperty& property) { return property.fourcc == EffectFourCC("SSWH"); }),
            "an SWSH's own SSWH dependency parses as a property, without synthetic descendant support");
  const ExternalRun nested = RunExternal({0x1234}, storedSwoosh, kSwsh, nestedSource);
  CHECK_MSG(nested.result.failed == 1 && nested.written.count("00001234.ELSC") == 0 &&
                nested.childWriteAttempts == 0 && nested.rows.size() == 1 &&
                nested.rows[0].reason.find("nested SSWH") != std::string::npos,
            "an unsupported nested SSWH dependency rejects the parent before any child or root is written");

  const EffectGuid mismatchedChild = Fresh(0x79);
  const ExternalRun mismatch = RunGenerator(0x1234, MakeGeneratorWithWrongSwooshChild(mismatchedChild));
  CHECK_MSG(mismatch.result.failed == 1 && mismatch.result.written == 0 && mismatch.written.empty() &&
                mismatch.rows.size() == 1 &&
                mismatch.rows[0].reason.find("conflicts with embedded PART") != std::string::npos,
            "a parent SSWH that aliases an embedded non-SWHC child fails closed without writing the parent");

  const EffectGuid parentId = Fresh(0x42);
  const EffectGuid selfReference = Swap(parentId);
  std::vector<EffectReportRow> cycleRows;
  EffectImportIO cycleIO;
  cycleIO.effects = {parentId};
  cycleIO.pairings = {{parentId, 0x1234, "fixture"}};
  cycleIO.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& data, std::string& error) {
    if (type == kElsm && id == parentId) {
      data = MakeElectric(selfReference);
      return true;
    }
    error = "unexpected dependency read";
    return false;
  };
  cycleIO.typeOf = [&](const EffectGuid& id) { return id == parentId ? kElsm : 0; };
  cycleIO.retailId = [](uint32_t id) { return id == 0x1234; };
  cycleIO.freshId = [](uint32_t seed) { return seed == 0 ? 1u : seed; };
  cycleIO.write = [](const std::string&, const std::vector<uint8_t>&) { return true; };
  cycleIO.report = [&](const EffectReportRow& row) { cycleRows.push_back(row); };
  const EffectImportResult cycle = ImportEffects(cycleIO);
  CHECK_MSG(cycle.failed == 1 && cycle.written == 0 && cycleRows.size() == 1 &&
                cycleRows[0].reason.find("cycle") != std::string::npos,
            "a self-referential SSWH dependency is rejected before reading or writing it");

  std::vector<EffectGuid> dependencies;
  std::map<EffectGuid, std::vector<uint8_t>> dependencyData;
  std::map<EffectGuid, uint32_t> dependencyTypes;
  for (uint8_t i = 0; i < 17; ++i) {
    const EffectGuid stored = Fresh(uint8_t(0x70 + i));
    dependencies.push_back(stored);
    dependencyData[Swap(stored)] = MakeSwoosh();
    dependencyTypes[Swap(stored)] = kSwsh;
  }
  const EffectGuid boundedParent = Swap(Legacy(0x3456));
  const std::vector<uint8_t> boundedData = MakeElectricRefs(dependencies);
  int boundedWrites = 0;
  EffectImportIO boundedIO;
  boundedIO.effects = {boundedParent};
  boundedIO.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& data, std::string& error) {
    if (type == kElsm && id == boundedParent) {
      data = boundedData;
      return true;
    }
    if (type == kSwsh && dependencyData.count(id) != 0) {
      data = dependencyData.at(id);
      return true;
    }
    error = "unexpected dependency";
    return false;
  };
  boundedIO.typeOf = [&](const EffectGuid& id) {
    return id == boundedParent ? kElsm : dependencyTypes.count(id) ? dependencyTypes.at(id) : 0;
  };
  boundedIO.retailId = [](uint32_t id) { return id == 0x3456; };
  boundedIO.freshId = [](uint32_t seed) { return seed == 0 ? 1u : seed; };
  boundedIO.write = [&](const std::string&, const std::vector<uint8_t>&) {
    ++boundedWrites;
    return true;
  };
  const EffectImportResult bounded = ImportEffects(boundedIO);
  CHECK_MSG(bounded.failed == 1 && bounded.written == 0 && boundedWrites == 0,
            "external SSWH traversal fails closed at its fixed dependency bound");
}

void TestEmbeddedSwooshPrecedence() {
  const EffectGuid child = Fresh(0x38);
  const EffectGuid parent = Swap(Legacy(0x1234));
  const std::vector<uint8_t> data = MakeGeneratorWithEmbeddedSwoosh(child);
  std::map<std::string, std::vector<uint8_t>> written;
  EffectImportIO io;
  io.effects = {parent};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == parent) {
      out = data;
      return true;
    }
    error = "unexpected asset read";
    return false;
  };
  io.typeOf = [](const EffectGuid&) { return 0; };
  io.retailId = [](uint32_t id) { return id == 0x1234; };
  io.freshId = [](uint32_t seed) { return seed; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& bytes) {
    written[name] = bytes;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  uint32_t swooshId = 0;
  CHECK_MSG(result.written == 1 && result.failed == 0 && written.count("00001234.PART") == 1,
            "the embedded-child regression still imports its GENP root");
  CHECK_MSG(result.parts == 2 && written.size() == 2,
            "the embedded SSWH target is emitted as a separate child part");
  if (written.count("00001234.PART") == 0) return;
  std::vector<RetailPartProperty> properties;
  std::string error;
  const auto& root = written.at("00001234.PART");
  bool linked = false;
  CHECK_MSG(SplitRetailPart(root.data(), root.size(), properties, error), "embedded test root reads as PART");
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("SSWH") && property.value.size() == 8) {
      swooshId = Be32(property.value.data() + 4);
      linked = written.count(HexId(swooshId) + ".SWHC") == 1;
    }
  }
  CHECK_MSG(linked, "an embedded SWSH keeps the existing embedded ID resolution and file");
  if (linked) {
    const auto& swoosh = written.at(HexId(swooshId) + ".SWHC");
    std::vector<RetailPartProperty> childProperties;
    CHECK_MSG(SplitRetailEffect(kSwhc, swoosh.data(), swoosh.size(), childProperties, error),
              "the linked embedded SWHC is a valid converted child");
  }
}

void TestEmbeddedAndExternalIdsDoNotCollide() {
  const EffectGuid embeddedStored = Fresh(0x41);
  const EffectGuid externalStored = Fresh(0x52);
  const EffectGuid generatorId = Swap(Legacy(0x1234));
  const EffectGuid electricId = Swap(Legacy(0x2345));
  const EffectGuid externalPak = Swap(externalStored);
  const std::vector<uint8_t> generator = MakeGeneratorWithEmbeddedSwoosh(embeddedStored);
  const std::vector<uint8_t> electric = MakeElectric(externalStored);
  const std::vector<uint8_t> externalSource = MakeSwoosh();
  std::map<std::string, std::vector<uint8_t>> written;
  std::set<uint32_t> taken = {0x1234, 0x2345};
  EffectImportIO io;
  io.effects = {generatorId, electricId};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& data, std::string& error) {
    if (type == kGenp && id == generatorId) data = generator;
    else if (type == kElsm && id == electricId) data = electric;
    else if (type == kSwsh && id == externalPak) data = externalSource;
    else {
      error = "not in the fixture";
      return false;
    }
    return true;
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    if (id == generatorId) return kGenp;
    if (id == electricId) return kElsm;
    return id == externalPak ? kSwsh : 0;
  };
  io.retailId = [&](uint32_t id) { return id == 0x1234 || id == 0x2345; };
  io.freshId = [&](uint32_t) {
    // Force the external dependency to collide with the already allocated
    // embedded child; the shared allocator must advance to a distinct id.
    uint32_t id = 0x00ABC000;
    while (taken.count(id) != 0) ++id;
    taken.insert(id);
    return id;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  uint32_t embeddedId = 0;
  uint32_t externalId = 0;
  std::vector<RetailPartProperty> partProperties;
  std::vector<RetailPartProperty> electricProperties;
  std::string error;
  const bool generatorValid = written.count("00001234.PART") != 0 &&
      SplitRetailPart(written["00001234.PART"].data(), written["00001234.PART"].size(), partProperties, error);
  const bool electricValid = written.count("00002345.ELSC") != 0 &&
      SplitRetailEffect(EffectFourCC("ELSC"), written["00002345.ELSC"].data(),
                        written["00002345.ELSC"].size(), electricProperties, error);
  for (const RetailPartProperty& property : partProperties)
    if (property.fourcc == EffectFourCC("SSWH") && property.value.size() == 8)
      embeddedId = Be32(property.value.data() + 4);
  for (const RetailPartProperty& property : electricProperties)
    if (property.fourcc == EffectFourCC("SSWH") && property.value.size() == 8)
      externalId = Be32(property.value.data() + 4);
  CHECK_MSG(result.written == 2 && result.failed == 0 && generatorValid && electricValid,
            "embedded and external swoosh parents both import");
  CHECK_MSG(embeddedId == 0x00ABC000 && externalId == 0x00ABC001 && embeddedId != externalId,
            "the external child advances past a colliding embedded-child id");
  CHECK_MSG(written.count("00ABC000.SWHC") == 1 && written.count("00ABC001.SWHC") == 1,
            "both root references resolve to the distinct emitted SWHC files");
}
// A generator whose material has no texture that converts would lose its
// TEXR: the import keeps the disc's PART and writes nothing.
void TestLostTexture() {
  const EffectGuid material = Fresh(0x10);
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "MTIN", 1);
  genp.push_back(0);
  PutGuid(genp, material);
  PutProperty(genp, "_END", 4);
  Put32(genp, 0);

  const EffectGuid effectId = Swap(Legacy(0x1234));
  std::map<std::string, std::vector<uint8_t>> written;
  std::vector<std::string> log;
  EffectImportIO io;
  io.effects = {effectId};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [](const EffectGuid&) -> uint32_t { return 0; };
  io.retailId = [](uint32_t id) { return id == 0x1234; };
  uint32_t next = 0x00ABC000;
  io.freshId = [&](uint32_t) { return next++; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  io.log = [&](const std::string& line) { log.push_back(line); };
  const EffectImportResult result = ImportEffects(io);
  CHECK_MSG(result.candidates == 1 && result.written == 0 && result.failed == 1, "an effect that lost its texture fails");
  CHECK_MSG(written.empty(), "nothing is written for it");
  CHECK_MSG(log.size() == 1 && log[0] == "00001234.PART: the root has no texture, the disc's is kept",
            "the log says the disc's PART is kept");
}

// A CRLN's vector field (VECF) is read once however many properties use it, and written as the
// port's big-endian VECF file; the PART names its new id.
void TestVectorField() {
  const EffectGuid material = Fresh(0x10);
  const EffectGuid texture = Fresh(0x20);
  const EffectGuid field = Fresh(0x30);
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "MTIN", 1);
  genp.push_back(0);
  PutGuid(genp, material);
  for (const char* name : {"VEL1", "VEL2"}) {
    PutProperty(genp, name, 3);
    PutFourCC(genp, "CRLN");
    PutFourCC(genp, "CRLN");
    genp.push_back(0);
    genp.push_back(0);
    PutFourCC(genp, "VECF");
    PutGuid(genp, field);
    PutFourCC(genp, "PLOC");
    PutFourCC(genp, "CNST");
    Put32(genp, 0x3f800000u);
    PutFourCC(genp, "NONE");
    PutFourCC(genp, "NONE");
    PutFourCC(genp, "CNST");
    for (int i = 0; i < 3; ++i) {
      PutFourCC(genp, "CNST");
      Put32(genp, 0x3f800000u);
    }
    genp.push_back(0);
    PutFourCC(genp, "CNST");
    Put32(genp, 0);
  }
  PutProperty(genp, "_END", 4);
  Put32(genp, 0);

  std::vector<uint8_t> mati(8, 0);
  PutGuid(mati, texture);

  // A 2 x 2 x 2 field: 'RFRM', size, 'VECF' at 0x14, versions, dims, min, max, count, cells, a footer.
  std::vector<uint8_t> vecf(0x14, 0);
  std::memcpy(vecf.data(), "RFRM", 4);
  PutRawFourCC(vecf, "VECF");
  Put32(vecf, 4);
  Put32(vecf, 5);
  for (uint32_t dim : {2u, 2u, 2u}) Put32(vecf, dim);
  for (float v : {-16.f, -16.f, -16.f, 16.f, 16.f, 16.f}) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    Put32(vecf, bits);
  }
  Put32(vecf, 8);
  for (uint32_t i = 0; i < 24; ++i) Put32(vecf, 0x3f800000u + i);
  vecf.resize(vecf.size() + 20, 0xee);

  const EffectGuid effectId = Swap(Legacy(0x1234));
  std::map<std::string, std::vector<uint8_t>> written;
  int fieldReads = 0;
  EffectImportIO io;
  io.effects = {effectId};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    if (type == kMati && id == Swap(material)) {
      out = mati;
      return true;
    }
    if (type == kVecf && id == Swap(field)) {
      ++fieldReads;
      out = vecf;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    return id == Swap(texture) ? kTxtr : id == Swap(material) ? kMati : id == Swap(field) ? kVecf : 0;
  };
  io.retailId = [](uint32_t id) { return id == 0x1234; };
  uint32_t next = 0x00ABC000;
  io.freshId = [&](uint32_t) { return next++; };
  io.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba, std::string& error) {
    if (id != Swap(texture)) {
      error = "no such texture";
      return false;
    }
    width = height = 8;
    rgba.assign(size_t(width) * height * 4, 0xff);
    return true;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  CHECK_MSG(result.written == 1 && result.failed == 0, "the effect with a vector field is written");
  std::string fieldName;
  int fields = 0;
  for (const auto& [name, data] : written) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".VECF") == 0) {
      fieldName = name;
      ++fields;
    }
  }
  CHECK_MSG(fields == 1 && fieldReads == 1, "the vector field is read and written once");
  if (fields != 1) {
    return;
  }
  std::vector<uint8_t> want;
  const auto be = [&want](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) want.push_back(uint8_t(v >> shift));
  };
  be(EffectFourCC("VECF"));
  be(1);
  for (size_t at = 0x20; at < vecf.size() - 20; at += 4) {
    be(uint32_t(vecf[at]) | uint32_t(vecf[at + 1]) << 8 | uint32_t(vecf[at + 2]) << 16 | uint32_t(vecf[at + 3]) << 24);
  }
  CHECK_MSG(written[fieldName] == want, "the VECF file is the field in big-endian, header and cells");
  CHECK_MSG(want.size() == 8 + 40 + 24 * 4, "the VECF file is 8 + 40 + 12 bytes per cell");
  const uint32_t id = uint32_t(std::stoul(fieldName.substr(0, 8), nullptr, 16));
  const std::vector<uint8_t>& part = written["00001234.PART"];
  const uint8_t idBytes[4] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
  int named = 0;
  for (auto at = part.begin(); (at = std::search(at, part.end(), idBytes, idBytes + 4)) != part.end(); ++at) {
    ++named;
  }
  CHECK_MSG(named == 2, "both properties name the field's id in the PART");
}

// An effect Remastered gave a fresh id replaces the retail PART of the same
// pak name (BombExplo, 1EF973EA).
void TestNamedEffect() {
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "_END", 4);
  Put32(genp, 0);

  // fb4d5181-cd7e-4c5e-8b11-afe35d30e231, in pak order.
  const EffectGuid effectId = {0xfb, 0x4d, 0x51, 0x81, 0xcd, 0x7e, 0x4c, 0x5e,
                               0x8b, 0x11, 0xaf, 0xe3, 0x5d, 0x30, 0xe2, 0x31};
  std::map<std::string, std::vector<uint8_t>> written;
  EffectImportIO io;
  io.effects = {effectId, Fresh(0x60)};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [](const EffectGuid&) -> uint32_t { return 0; };
  io.retailId = [](uint32_t id) { return id == 0x1EF973EA; };
  io.freshId = [](uint32_t) { return 0x00ABC000u; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  CHECK_MSG(result.candidates == 1 && result.written == 1, "the named effect is a candidate and written");
  CHECK_MSG(written.size() == 1 && written.count("1EF973EA.PART") == 1, "it replaces the PART of its name");
}

// The FlickerBat's 0B9E48EB is adopted on a visual A/B, not on a CHPR: the table
// entry (#visual-inferred) must keep pairing 3b29b0b3 with it.
void TestVisualInferredEffect() {
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "_END", 4);
  Put32(genp, 0);

  // 3b29b0b3-0c3d-42b6-9159-f4863f6c3ed9, in pak order.
  const EffectGuid effectId = {0x3b, 0x29, 0xb0, 0xb3, 0x0c, 0x3d, 0x42, 0xb6,
                               0x91, 0x59, 0xf4, 0x86, 0x3f, 0x6c, 0x3e, 0xd9};
  std::map<std::string, std::vector<uint8_t>> written;
  EffectImportIO io;
  io.effects = {effectId, Fresh(0x60)};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [](const EffectGuid&) -> uint32_t { return 0; };
  io.retailId = [](uint32_t id) { return id == 0x0B9E48EB; };
  io.freshId = [](uint32_t) { return 0x00ABC000u; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  CHECK_MSG(result.candidates == 1 && result.written == 1, "the inferred effect is a candidate and written");
  CHECK_MSG(written.size() == 1 && written.count("0B9E48EB.PART") == 1,
            "it replaces retail PART 0B9E48EB");
}
// These replacement PARTs used to be invisible to the rooms step: none belongs
// to the shared-genp section, even though ImportEffects writes all three.
void TestRoomPlacementPairings() {
  const EffectGuid flame = {0x69, 0x16, 0x27, 0x2d, 0x42, 0x72, 0x4e, 0xc4,
                           0xa0, 0xb2, 0xa4, 0xf9, 0xb3, 0x16, 0x0e, 0xf6};
  const EffectGuid landingA = {0x60, 0x98, 0x59, 0x73, 0x2d, 0x34, 0x42, 0xf2,
                              0x8c, 0x43, 0x2d, 0x57, 0x46, 0x0e, 0x81, 0x8a};
  const EffectGuid landingB = {0xdb, 0x9d, 0xd5, 0x87, 0xd5, 0xc7, 0x41, 0x03,
                              0x86, 0x2e, 0x7c, 0x1e, 0x33, 0xc1, 0x6a, 0x59};
  for (const auto& item : {std::make_pair(flame, 0x3A617BEBu),
                           std::make_pair(landingA, 0xBD3DC521u),
                           std::make_pair(landingB, 0xD0376F71u)}) {
    const auto ids = MatchedRetailEffects(item.first);
    CHECK(std::count(ids.begin(), ids.end(), item.second) == 1);
  }
  CHECK(MatchedRetailEffects(Fresh(0xfa)).empty());
}
} // namespace

int main() {
  TestRoomPlacementPairings();
  TestImport();
  TestExternalSwoosh();
  TestExternalSwooshFailures();
  TestEmbeddedSwooshPrecedence();
  TestEmbeddedAndExternalIdsDoNotCollide();
  TestLostTexture();
  TestNamedEffect();
  TestVisualInferredEffect();
  TestVectorField();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_effect_import: ok\n");
  }
  return PORT_TEST_RESULT();
}
