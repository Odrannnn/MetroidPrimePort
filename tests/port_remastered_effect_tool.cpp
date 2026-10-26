// Dev tool for the Remastered particle reader (port_remastered_effect).
//
//   tool dump <file.GENP>            print the parsed effect
//   tool kssm <file.GENP>            print its spawn tables as the converter reads them
//   tool mtin <file.GENP>...         one line per generator with a material
//                                    instance: its MATI and PMTR values
//   tool scan <romfs> [outdir]       parse every GENP in every pak under <romfs>
//   tool convert <romfs> <retail> <outdir>
//                                    convert every GENP to retail PART/SWHC/ELSC
//
// scan reports parse coverage (unique ids and every copy), lists the files that
// do not parse with the offset and FourCC the parse stopped at, and resolves the
// ids the effects refer to against every asset in the paks, by type. With
// <outdir> it also writes one dump per effect, <outdir>/<id>.txt, for diffing
// against retail PART dumps.
//
// convert writes <outdir>/<id>.PART for every effect that converts (the
// root under its retail id when it kept one, else under its own id; children
// as <root>-<child>.PART, .SWHC or .ELSC), and a line per effect of what was
// left out. It prints the embedded children found by form and the files
// written by type. With <retail> a folder of the disc's files named
// <8 hex digits>.PART, .SWHC and .ELSC ("-" for none), the splitters are run
// on each of them, and each converted file whose id is a retail one (the root,
// or a child that kept its retail id) is compared with the disc's file
// property by property; the totals per property are printed (prefixed with the
// type for SWHC and ELSC): identical, different, only on the disc, only
// converted.
//
// import runs the import's effect step (port_remastered_effect_import.h) on
// the paks under <romfs> into <outdir>, as the game's import would with
// MP_REMASTERED_EFFECTS=1. A retail id counts as on the disc when <retail>
// holds a file named <8 hex digits>.<type>.
//
// import also writes every texture the step makes (a flipbook's or a TXP2's atlas
// among them) as a PNG in /tmp/fx-atlas, and logs the meshes and triangles of each
// Remastered-only model it converts.
//
// Built by the CMake target remastered_effect_tool (not part of `all`; Linux/desktop):
//   cmake --build build/<dir> --target remastered_effect_tool
//   build/<dir>/remastered_effect_tool <mode> ...
//
//   tool pdump <file.PART|.SWHC|.ELSC>
//                                    a retail effect's properties, one per line: FourCC, then the
//                                    value decoded as a tree when it reads as one, else hex
//   tool pdiff <a> <b>               the properties that differ between two retail effect files
//   tool explain <romfs> <retail> <GENP id>
//                                    the import's effect step on one effect, with every decision:
//                                    pairing, result, dropped and approximated properties, and
//                                    the models it converts. <id> is the id in the effects
//                                    report's genp column (either byte order)
//   tool mat <romfs> <model id|name> [material index]
//                                    converts one Remastered model standalone and prints each
//                                    output material's decision (the materials report's columns)
//
// import also writes <outdir>/effects.tsv, the effects report of docs/DEBUGGING.md.

#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_effect.h"
#include "port_remastered_effect_convert.h"
#include "port_remastered_effect_import.h"
#include "port_remastered_txtr.h"
#include "port_remastered_pak.h"
#include "port_remastered_report.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FileReader {
public:
  bool Open(const std::string& path, std::string& error) {
    m_stream.open(path, std::ios::binary);
    if (!m_stream.is_open()) {
      error = "could not open '" + path + "'";
      return false;
    }
    m_stream.seekg(0, std::ios::end);
    m_size = uint64_t(m_stream.tellg());
    m_stream.seekg(0, std::ios::beg);
    return true;
  }

  uint64_t Size() const { return m_size; }

  bool Read(uint64_t offset, void* out, size_t size) {
    m_stream.clear();
    m_stream.seekg(std::streamoff(offset));
    if (!m_stream) {
      return false;
    }
    m_stream.read(static_cast<char*>(out), std::streamsize(size));
    return m_stream.gcount() == std::streamsize(size);
  }

private:
  std::ifstream m_stream;
  uint64_t m_size = 0;
};

constexpr uint32_t kGenp = PortRemastered::EffectFourCC("GENP");
constexpr uint32_t kSwsh = PortRemastered::EffectFourCC("SWSH");
constexpr uint32_t kMati = PortRemastered::EffectFourCC("MATI");
constexpr uint32_t kTxtr = PortRemastered::EffectFourCC("TXTR");
constexpr uint32_t kPartType = PortRemastered::EffectFourCC("PART");

uint32_t FourCCOf(const std::string& text) {
  uint32_t fourcc = 0;
  for (size_t i = 0; i < 4; ++i) {
    fourcc = fourcc << 8 | uint8_t(i < text.size() ? text[i] : ' ');
  }
  return fourcc;
}

// How many embedded children of each form an effect has, at any depth.
void CountForms(const PortRemastered::EffectNode& node, std::map<uint32_t, size_t>& out) {
  for (const PortRemastered::EffectNode& child : node.children) {
    ++out[child.form];
    CountForms(child, out);
  }
}

// The ids of an effect's embedded children, at any depth.
void CollectChildren(const PortRemastered::EffectNode& node, std::set<PortRemastered::EffectGuid>& out) {
  for (const PortRemastered::EffectNode& child : node.children) {
    out.insert(child.id);
    CollectChildren(child, out);
  }
}

// Ids inside an effect are stored as little-endian UUIDs; the pak reader keeps
// asset ids in printed order. Swap the first three groups to look one up.
PortRemastered::EffectGuid PakId(const PortRemastered::EffectGuid& guid) {
  PortRemastered::EffectGuid out = guid;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

int Dump(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  PortRemastered::EffectNode effect;
  std::string error;
  if (!PortRemastered::ParseEffect(data.data(), data.size(), effect, error)) {
    std::cerr << path << ": " << error << "\n";
    return 1;
  }
  std::cout << PortRemastered::DumpEffect(effect, data.data());
  return 0;
}

// Every KSSM in the effect as ParseSpawnTable reads it.
void SpawnTables(const PortRemastered::EffectNode& node, const uint8_t* data, size_t size) {
  using namespace PortRemastered;
  for (const EffectProperty& property : node.properties) {
    if (property.fourcc != EffectFourCC("KSSM")) {
      continue;
    }
    EffectSpawnTable table;
    if (!ParseSpawnTable(data, size, property, table)) {
      std::cout << EffectGuidString(node.id) << " KSSM does not parse\n";
      continue;
    }
    std::cout << EffectGuidString(node.id) << " KSSM end " << table.header[2] << " events " << table.events
              << " tables " << table.tables.size() << "\n";
    for (const EffectValue& component : table.components) {
      std::cout << "  component " << DumpEffectValue(component, data) << "\n";
    }
    for (const EffectSpawnTable::Table& each : table.tables) {
      std::cout << "  start " << each.word << " period " << EffectFourCCString(each.selector.fourcc) << "\n";
      for (const EffectSpawnTable::Frame& frame : each.frames) {
        for (const EffectSpawnTable::Spawn& spawn : frame.spawns) {
          std::cout << "    frame " << frame.frame << " " << EffectFourCCString(spawn.form) << " "
                    << EffectGuidString(spawn.id) << " component " << int32_t(spawn.component) << " instances "
                    << EffectFourCCString(spawn.instances.fourcc) << "\n";
        }
      }
    }
  }
  for (const EffectNode& child : node.children) {
    SpawnTables(child, data, size);
  }
}

int Kssm(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  PortRemastered::EffectNode effect;
  std::string error;
  if (!PortRemastered::ParseEffect(data.data(), data.size(), effect, error)) {
    std::cerr << path << ": " << error << "\n";
    return 1;
  }
  SpawnTables(effect, data.data(), data.size());
  return 0;
}

// One line per generator with a material instance: file, form, the MTIN's
// MATI (pak order), whether it has a TEXR, PBDM, then PMTR's items as
// group/slot=value (a constant vec4 as its four reals, else the element's
// FourCC) and SMTR's item count. For joining with `re.sh mtrls`.
void MaterialLines(const std::string& file, const PortRemastered::EffectNode& node, const uint8_t* data,
                   size_t size) {
  using namespace PortRemastered;
  std::string mati, pbdm = "-", pmtr, smtr = "-";
  bool texr = false;
  for (const EffectProperty& property : node.properties) {
    if (property.fourcc == EffectFourCC("MTIN")) {
      for (const EffectValue& value : property.value) {
        if (value.kind == EffectValue::Kind::Guid) {
          mati = EffectGuidString(value.guid);
        }
      }
    } else if (property.fourcc == EffectFourCC("TEXR")) {
      texr = true;
    } else if (property.fourcc == EffectFourCC("PBDM") && property.value.size() == 1) {
      const EffectValue& v = property.value[0];
      const EffectValue& mode = v.args.size() == 1 ? v.args[0] : v;
      pbdm = std::to_string(mode.word);
    } else if (property.fourcc == EffectFourCC("PMTR") || property.fourcc == EffectFourCC("SMTR")) {
      EffectMaterialTrack track;
      if (!ParseMaterialTrack(data, size, property, track)) {
        (property.fourcc == EffectFourCC("PMTR") ? pmtr : smtr) = "unparsed";
        continue;
      }
      if (property.fourcc == EffectFourCC("SMTR")) {
        smtr = std::to_string(track.items.size());
        continue;
      }
      for (const EffectMaterialTrack::Item& item : track.items) {
        std::ostringstream text;
        text << item.group << "/" << (item.trailer[0] | item.trailer[1] << 8) << "=";
        const EffectValue& v = item.value;
        bool constant = v.fourcc == EffectFourCC("CNST") && !v.args.empty();
        for (const EffectValue& c : v.args) {
          constant = constant && c.kind == EffectValue::Kind::Element && c.fourcc == EffectFourCC("CNST") && c.args.size() == 1 &&
                     c.args[0].kind == EffectValue::Kind::Word;
        }
        if (constant) {
          for (size_t i = 0; i < v.args.size(); ++i) {
            float real;
            std::memcpy(&real, &v.args[i].args[0].word, 4);
            text << (i ? "," : "") << real;
          }
        } else {
          text << EffectFourCCString(v.fourcc);
        }
        pmtr += (pmtr.empty() ? "" : " ") + text.str();
      }
    }
  }
  if (!mati.empty()) {
    std::cout << file << "\t" << EffectFourCCString(node.form) << "\t" << mati << "\t" << (texr ? "TEXR" : "-") << "\t"
              << pbdm << "\t" << (pmtr.empty() ? "-" : pmtr) << "\t" << smtr << "\n";
  }
  for (const PortRemastered::EffectNode& child : node.children) {
    MaterialLines(file, child, data, size);
  }
}

int Materials(int count, char** paths) {
  for (int i = 0; i < count; ++i) {
    std::ifstream file(paths[i], std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    PortRemastered::EffectNode effect;
    std::string error;
    if (!PortRemastered::ParseEffect(data.data(), data.size(), effect, error)) {
      std::cerr << paths[i] << ": " << error << "\n";
      continue;
    }
    const std::string path = paths[i];
    MaterialLines(path.substr(path.find_last_of('/') + 1), effect, data.data(), data.size());
  }
  return 0;
}

int Scan(const std::string& romfs, const std::string& outDir, bool rawOut = false) {
  std::vector<std::filesystem::path> paks;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paks.push_back(entry.path());
    }
  }
  std::sort(paks.begin(), paks.end());
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
  }
  if (rawOut) {
    // Every unique GENP as its raw bytes, <id>.GENP, for grepping ids the
    // dump prints as raw blocks (PVAR, the parameter tables).
    std::set<PortRemastered::EffectGuid> written;
    for (const std::filesystem::path& path : paks) {
      FileReader reader;
      std::string error;
      PortRemastered::Pak pak;
      if (!reader.Open(path.string(), error) ||
          !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                    reader.Size(), error)) {
        std::cerr << path.string() << ": " << error << "\n";
        return 1;
      }
      for (const PortRemastered::PakAsset& asset : pak.Assets()) {
        std::vector<uint8_t> data;
        if ((asset.type != kGenp && asset.type != kSwsh) || !written.insert(asset.id).second ||
            !pak.ReadAsset(asset, data, error)) {
          continue;
        }
        std::ofstream out(std::filesystem::path(outDir) / (PortRemastered::IdToString(asset.id) +
                                                           (asset.type == kSwsh ? ".SWSH" : ".GENP")),
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      }
    }
    std::cout << written.size() << " GENPs and SWSHs written\n";
    return 0;
  }

  std::map<PortRemastered::EffectGuid, uint32_t> types; // every asset id -> type
  std::map<PortRemastered::EffectGuid, std::vector<PortRemastered::EffectGuid>> references;
  std::map<PortRemastered::EffectGuid, std::string> failures;
  std::set<PortRemastered::EffectGuid> seen;
  size_t copies = 0;
  size_t copiesParsed = 0;
  size_t parsed = 0;
  for (const std::filesystem::path& path : paks) {
    FileReader reader;
    std::string error;
    PortRemastered::Pak pak;
    if (!reader.Open(path.string(), error) ||
        !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                  reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    for (const PortRemastered::PakAsset& asset : pak.Assets()) {
      types[asset.id] = asset.type;
      if (asset.type != kGenp) {
        continue;
      }
      copies += 1;
      const bool first = seen.insert(asset.id).second;
      if (!first) {
        copiesParsed += failures.count(asset.id) ? 0 : 1;
        continue;
      }
      std::vector<uint8_t> data;
      if (!pak.ReadAsset(asset, data, error)) {
        std::cerr << path.string() << ": " << error << "\n";
        return 1;
      }
      PortRemastered::EffectNode effect;
      size_t failOffset = 0;
      const std::string name = PortRemastered::IdToString(asset.id);
      if (!PortRemastered::ParseEffect(data.data(), data.size(), effect, error, &failOffset)) {
        char text[64];
        std::snprintf(text, sizeof(text), "0x%zx ", failOffset);
        std::string fourcc = failOffset + 4 <= data.size()
                                 ? std::string(data.begin() + long(failOffset), data.begin() + long(failOffset) + 4)
                                 : std::string("????");
        std::reverse(fourcc.begin(), fourcc.end());
        failures[asset.id] = text + fourcc + (asset.names.empty() ? "" : " " + asset.names.front());
        continue;
      }
      parsed += 1;
      copiesParsed += 1;
      references[asset.id] = PortRemastered::EffectReferences(effect);
      if (!outDir.empty()) {
        std::ofstream out(std::filesystem::path(outDir) / (name + ".txt"));
        if (!asset.names.empty()) {
          out << "# " << asset.names.front() << "\n";
        }
        out << PortRemastered::DumpEffect(effect, data.data());
      }
    }
  }

  std::map<std::string, size_t> referenceTypes;
  std::set<PortRemastered::EffectGuid> instances;
  size_t referenceCount = 0;
  for (const auto& [id, refs] : references) {
    for (const PortRemastered::EffectGuid& ref : refs) {
      auto it = types.find(PakId(ref));
      referenceTypes[it == types.end() ? "(not an asset)" : PortRemastered::EffectFourCCString(it->second)] += 1;
      referenceCount += 1;
      if (it != types.end() && it->second == kMati) {
        instances.insert(it->first);
      }
    }
  }

  // A MATI (material instance) names its MTRL and the textures bound to its
  // samplers. Resolve every 16-byte window of each referenced one against the
  // asset list to see what the effects' materials end up drawing with.
  std::map<std::string, size_t> instanceTypes;
  size_t instancesRead = 0;
  for (const std::filesystem::path& path : paks) {
    if (instances.empty()) {
      break;
    }
    FileReader reader;
    std::string error;
    PortRemastered::Pak pak;
    if (!reader.Open(path.string(), error) ||
        !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                  reader.Size(), error)) {
      continue;
    }
    for (const PortRemastered::PakAsset& asset : pak.Assets()) {
      std::vector<uint8_t> data;
      if (instances.count(asset.id) == 0 || !pak.ReadAsset(asset, data, error)) {
        continue;
      }
      instances.erase(asset.id);
      instancesRead += 1;
      std::set<uint32_t> found;
      for (size_t at = 0; at + 16 <= data.size(); ++at) {
        PortRemastered::EffectGuid guid;
        std::copy(data.begin() + long(at), data.begin() + long(at) + 16, guid.begin());
        auto it = types.find(PakId(guid));
        if (it != types.end() && it->first != asset.id) {
          found.insert(it->second);
        }
      }
      for (uint32_t type : found) {
        instanceTypes[PortRemastered::EffectFourCCString(type)] += 1;
      }
    }
  }

  std::cout << paks.size() << " paks, " << types.size() << " unique assets\n";
  std::cout << "GENP: " << parsed << "/" << seen.size() << " unique parse, " << copiesParsed << "/" << copies
            << " counting every copy\n";
  std::cout << referenceCount << " ids referenced by parsed effects:\n";
  for (const auto& [type, count] : referenceTypes) {
    std::cout << "  " << type << " " << count << "\n";
  }
  std::cout << instancesRead << " referenced MATI, by the asset types they name:\n";
  for (const auto& [type, count] : instanceTypes) {
    std::cout << "  " << type << " " << count << "\n";
  }
  std::cout << failures.size() << " failures:\n";
  for (const auto& [id, text] : failures) {
    std::cout << "  " << PortRemastered::IdToString(id) << " " << text << "\n";
  }
  return 0;
}


struct PropertyTally {
  size_t same = 0;
  size_t different = 0;
  size_t discOnly = 0;
  size_t convertedOnly = 0;
};

// Error text can carry the raw bytes of a FourCC that is not one.
std::string Printable(std::string text) {
  for (char& c : text) {
    if (c < 0x20 || c > 0x7e) {
      c = '?';
    }
  }
  return text;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int Convert(const std::string& romfs, const std::string& retailDir, const std::string& outDir, bool census = false) {
  using namespace PortRemastered;
  std::vector<std::filesystem::path> paks;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paks.push_back(entry.path());
    }
  }
  std::sort(paks.begin(), paks.end());
  std::filesystem::create_directories(outDir);

  // Every effect and material instance, read once.
  std::map<EffectGuid, uint32_t> types;
  std::map<EffectGuid, std::vector<uint8_t>> effects;
  std::map<EffectGuid, std::vector<uint8_t>> materials;
  std::map<EffectGuid, std::string> names;
  for (const std::filesystem::path& path : paks) {
    FileReader reader;
    std::string error;
    Pak pak;
    if (!reader.Open(path.string(), error) ||
        !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                  reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    for (const PakAsset& asset : pak.Assets()) {
      types[asset.id] = asset.type;
      auto& store = asset.type == kGenp ? effects : materials;
      if ((asset.type != kGenp && asset.type != kMati) || store.count(asset.id) != 0) {
        continue;
      }
      if (!pak.ReadAsset(asset, store[asset.id], error)) {
        std::cerr << path.string() << ": " << error << "\n";
        return 1;
      }
      if (!asset.names.empty()) {
        names[asset.id] = asset.names.front();
      }
    }
  }

  if (census) {
    size_t parsed = 0, failed = 0, generators = 0, enabled = 0, disabled = 0;
    size_t primitiveEnabled = 0, primitiveDisabled = 0;
    std::map<int, size_t> primitives;
    std::ofstream rows(std::filesystem::path(outDir) / "primitive-census.tsv");
    rows << "asset\tnode\tname\tprimitive\told_sprite_suppressed\tnew_sprite_suppressed\told_draws_nothing\tnew_draws_nothing\tbase_material\tcustom_material\n";
    for (const auto& [id, data] : effects) {
      EffectNode root;
      std::string error;
      if (!ParseEffect(data.data(), data.size(), root, error)) {
        ++failed;
        std::cerr << IdToString(id) << ": " << error << '\n';
        continue;
      }
      ++parsed;
      std::function<void(const EffectNode&, std::string)> visit = [&](const EffectNode& node, std::string path) {
        if (node.form == EffectFourCC("GPSM")) {
          const auto has = [&](const char (&tag)[5]) {
            return std::any_of(node.properties.begin(), node.properties.end(), [&](const EffectProperty& p) {
              return p.fourcc == EffectFourCC(tag);
            });
          };
          bool nil = false;
          for (const auto& p : node.properties) {
            if (p.fourcc != EffectFourCC("MTIN")) continue;
            for (const auto& v : p.value) {
              if (v.kind != EffectValue::Kind::Guid) continue;
              const auto mat = materials.find(PakId(v.guid));
              if (mat != materials.end()) {
                const auto& bytes = mat->second;
                const uint8_t tag[] = {'B', 'C', 'L', 'R'};
                const auto at = std::search(bytes.begin(), bytes.end(), std::begin(tag), std::end(tag));
                nil = at != bytes.end() && bytes.end() - at >= 20 &&
                      std::all_of(at + 4, at + 20, [](uint8_t b) { return b == 0; });
              }
              break;
            }
          }
          const bool oldNothing = !has("TEXR") && (!has("MTIN") || nil) && !has("PMDL");
          const bool oldSprite = oldNothing || (!has("TEXR") && !has("MTIN") && has("PMDL"));
          const bool custom = std::any_of(node.properties.begin(), node.properties.end(), [](const auto& p) {
            return p.fourcc == EffectFourCC("MTIN") && std::any_of(p.value.begin(), p.value.end(), [](const auto& v) {
              return v.kind == EffectValue::Kind::Guid && v.guid != EffectGuid{};
            });
          });
          const bool newNothing = node.primitiveType == 0 || node.primitiveType == 4 || node.primitiveType == 5 ||
                                  (node.primitiveType == 3 && !has("PMDL")) ||
                                  ((node.primitiveType == 1 || node.primitiveType == 2) &&
                                   node.baseMaterial == EffectGuid{} && !custom);
          const bool newSprite = newNothing || node.primitiveType == 3;
          ++generators;
          ++primitives[node.primitiveType];
          enabled += oldSprite && !newSprite;
          disabled += !oldSprite && newSprite;
          primitiveEnabled += oldNothing && !newNothing;
          primitiveDisabled += !oldNothing && newNothing;
          rows << IdToString(id) << '\t' << path << '\t' << names[id] << '\t' << node.primitiveType
               << '\t' << oldSprite << '\t' << newSprite << '\t' << oldNothing << '\t' << newNothing
               << '\t' << EffectGuidString(node.baseMaterial) << '\t' << custom << '\n';
        }
        for (size_t i = 0; i < node.children.size(); ++i) visit(node.children[i], path + "/" + std::to_string(i));
      };
      visit(root, "root");
    }
    std::cout << "paks=" << paks.size() << " unique_GENP=" << effects.size() << " parsed=" << parsed
              << " failed=" << failed << " GPSM=" << generators << '\n'
              << "sprite_suppressed_to_drawn=" << enabled << " sprite_drawn_to_suppressed=" << disabled << '\n'
              << "primitive_suppressed_to_drawn=" << primitiveEnabled << " primitive_drawn_to_suppressed=" << primitiveDisabled << '\n';
    for (const auto& [primitive, count] : primitives) std::cout << "primitive " << primitive << ": " << count << '\n';
    return failed ? 1 : 0;
  }
  EffectConvertIO io;
  // The first TXTR a material instance names, when it is one carried over from retail.
  io.materialTexture = [&](const EffectGuid& material) -> uint32_t {
    auto it = materials.find(PakId(material));
    if (it == materials.end()) {
      return 0;
    }
    const std::vector<uint8_t>& data = it->second;
    for (size_t at = 0; at + 16 <= data.size(); ++at) {
      EffectGuid guid;
      std::copy(data.begin() + long(at), data.begin() + long(at) + 16, guid.begin());
      // Ids inside a material instance are stored as they are in an effect.
      auto type = types.find(PakId(guid));
      if (type != types.end() && type->second == kTxtr) {
        return EffectRetailId(guid).value_or(0);
      }
    }
    return 0;
  };

  // The MATI of a material instance, and its textures as stand-in ids (the importer writes fresh ones).
  io.materialData = [&](const EffectGuid& material) -> std::vector<uint8_t> {
    auto it = materials.find(PakId(material));
    return it == materials.end() ? std::vector<uint8_t>() : it->second;
  };
  io.vfxTexture = [&](const EffectGuid& texture) -> FlipbookAtlas {
    auto type = types.find(PakId(texture));
    if (type == types.end() || type->second != kTxtr) {
      return {};
    }
    if (const std::optional<uint32_t> retail = EffectRetailId(texture)) {
      return FlipbookAtlas{*retail, 1, 1, 1};
    }
    uint32_t hash = 0x80000000u;
    for (uint8_t byte : texture) {
      hash = (hash * 31 + byte) | 0x80000000u;
    }
    return FlipbookAtlas{hash, 1, 1, 1};
  };

  std::map<std::string, PropertyTally> tally;
  std::map<std::string, size_t> dropReasons;
  std::map<uint32_t, size_t> found;  // embedded children by form
  std::map<std::string, size_t> writtenByType, droppedByType;
  size_t parsed = 0, written = 0, clean = 0, compared = 0, identical = 0, invalid = 0;
  std::ofstream log(std::filesystem::path(outDir) / "convert.txt");
  for (const auto& [id, data] : effects) {
    EffectNode effect;
    std::string error;
    if (!ParseEffect(data.data(), data.size(), effect, error)) {
      continue;
    }
    ++parsed;
    // An asset id in pak order is an effect id with its first groups swapped.
    const EffectGuid effectId = PakId(id);
    const std::optional<uint32_t> retailId = EffectRetailId(effectId);
    char rootName[16];
    std::snprintf(rootName, sizeof(rootName), "%08X", retailId.value_or(0));
    const std::string root = retailId ? std::string(rootName) : IdToString(id);
    CountForms(effect, found);
    // Embedded children get stand-in ids (the importer's are fresh ones), so a
    // spawn table naming them converts.
    std::set<EffectGuid> children;
    CollectChildren(effect, children);
    io.assetId = [&](const EffectGuid& guid, uint32_t type) -> uint32_t {
      if (children.count(guid) != 0) {
        uint32_t hash = 0x80000000u;
        for (uint8_t byte : guid) {
          hash = (hash * 31 + byte) | 0x80000000u;
        }
        return hash;
      }
      return EffectRetailId(guid).value_or(0);
    };
    const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
    bool allClean = true;
    for (const ConvertedPart& part : parts) {
      const std::string type = EffectFourCCString(part.type);
      std::vector<RetailPartProperty> check;
      if (!SplitRetailEffect(part.type, part.part.data(), part.part.size(), check, error)) {
        ++invalid;
        log << root << " writes a " << type << " retail does not read: " << Printable(error) << "\n";
        continue;
      }
      const std::string file = part.root ? root : root + "-" + EffectGuidString(part.id);
      std::ofstream(std::filesystem::path(outDir) / (file + "." + type), std::ios::binary)
          .write(reinterpret_cast<const char*>(part.part.data()), std::streamsize(part.part.size()));
      ++written;
      ++writtenByType[type];
      droppedByType[type] += size_t(part.droppedRetail);
      allClean = allClean && part.droppedRetail == 0;
      for (const std::string& dropped : part.dropped) {
        log << file << (names.count(id) ? " " + names[id] : "") << " dropped " << Printable(dropped) << "\n";
        dropReasons[(part.type == kPartType ? "" : type + " ") + dropped.substr(0, 4)] += 1;
      }
    }
    clean += allClean ? 1 : 0;

    if (retailDir == "-") {
      continue;
    }
    // Each converted file whose id is a retail one, against the disc's file of
    // that id: the effect's own PART, and children that kept their retail id.
    for (const ConvertedPart& part : parts) {
      const std::optional<uint32_t> discId = part.root ? retailId : EffectRetailId(part.id);
      if (!discId) {
        continue;
      }
      const std::string type = EffectFourCCString(part.type);
      char discName[24];
      std::snprintf(discName, sizeof(discName), "%08X.%s", *discId, type.c_str());
      const std::filesystem::path disc = std::filesystem::path(retailDir) / discName;
      if (!std::filesystem::exists(disc)) {
        continue;
      }
      const std::vector<uint8_t> retail = ReadFile(disc);
      std::vector<RetailPartProperty> want, got;
      if (!SplitRetailEffect(part.type, retail.data(), retail.size(), want, error)) {
        log << discName << " on the disc does not split: " << error << "\n";
        continue;
      }
      // A converted file that does not read is counted above, not compared.
      if (!SplitRetailEffect(part.type, part.part.data(), part.part.size(), got, error)) {
        continue;
      }
      if (!part.root) {
        log << discName << " compared with the child of " << root << "\n";
      }
      ++compared;
      const std::string prefix = part.type == kPartType ? "" : type + " ";
      bool same = want.size() == got.size();
      for (const RetailPartProperty& w : want) {
        const std::string name = prefix + EffectFourCCString(w.fourcc);
        auto g = std::find_if(got.begin(), got.end(), [&](const RetailPartProperty& p) { return p.fourcc == w.fourcc; });
        if (g == got.end()) {
          tally[name].discOnly += 1;
          same = false;
        } else if (g->value == w.value) {
          tally[name].same += 1;
        } else {
          tally[name].different += 1;
          same = false;
        }
      }
      for (const RetailPartProperty& g : got) {
        if (std::none_of(want.begin(), want.end(), [&](const RetailPartProperty& p) { return p.fourcc == g.fourcc; })) {
          tally[prefix + EffectFourCCString(g.fourcc)].convertedOnly += 1;
        }
      }
      identical += same ? 1 : 0;
    }
  }

  std::cout << parsed << " effects parse, " << written << " files written, " << clean
            << " effects with no retail property left out, " << invalid << " files retail would not read\n";
  std::cout << "embedded children, by form (found / written as):\n";
  for (const auto& [form, count] : found) {
    const uint32_t type = EffectRetailType(form);
    std::cout << "  " << EffectFourCCString(form) << " " << count;
    if (type != 0) {
      std::cout << " / " << EffectFourCCString(type);
    }
    std::cout << "\n";
  }
  std::cout << "written, by type (files / retail properties left out):\n";
  for (const auto& [type, count] : writtenByType) {
    std::cout << "  " << type << " " << count << " / " << droppedByType[type] << "\n";
  }
  if (retailDir != "-") {
    // The splitters against every file of their types on the disc.
    std::map<std::string, std::pair<size_t, size_t>> splits;
    for (const auto& entry : std::filesystem::directory_iterator(retailDir)) {
      const std::string ext = entry.path().extension().string();
      if (ext != ".PART" && ext != ".SWHC" && ext != ".ELSC") {
        continue;
      }
      const std::vector<uint8_t> file = ReadFile(entry.path());
      std::vector<RetailPartProperty> split;
      std::string error;
      auto& [all, ok] = splits[ext.substr(1)];
      ++all;
      if (SplitRetailEffect(FourCCOf(ext.substr(1)), file.data(), file.size(), split, error)) {
        ++ok;
      } else {
        log << entry.path().filename().string() << " on the disc does not split: " << error << "\n";
      }
    }
    for (const auto& [type, counts] : splits) {
      std::cout << counts.second << " of the disc's " << counts.first << " " << type << " split\n";
    }
  }
  std::cout << "left out, by property:\n";
  for (const auto& [name, count] : dropReasons) {
    std::cout << "  " << name << " " << count << "\n";
  }
  if (compared != 0) {
    std::cout << compared << " compared with the disc, " << identical << " identical; per property"
              << " (same / different / disc only / converted only):\n";
    for (const auto& [name, t] : tally) {
      std::cout << "  " << name << " " << t.same << " / " << t.different << " / " << t.discOnly << " / "
                << t.convertedOnly << "\n";
    }
  }
  return 0;
}

// A PNG with stored (uncompressed) deflate blocks: enough to look at an atlas.
bool WritePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
  auto be32 = [](std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) {
      out.push_back(uint8_t(v >> s));
    }
  };
  static uint32_t table[256];
  if (table[1] == 0) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[n] = c;
    }
  }
  auto crc = [&](const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
      c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
  };
  std::vector<uint8_t> raw;
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * size_t(width) * 4, rgba.begin() + size_t(y + 1) * size_t(width) * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (n == 0) {
      break;
    }
  }
  uint32_t a = 1;
  uint32_t b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  be32(z, b << 16 | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
    be32(png, uint32_t(body.size()));
    std::vector<uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    png.insert(png.end(), typed.begin(), typed.end());
    be32(png, crc(typed.data(), typed.size()));
  };
  std::vector<uint8_t> head;
  be32(head, uint32_t(width));
  be32(head, uint32_t(height));
  head.insert(head.end(), {8, 6, 0, 0, 0});
  chunk("IHDR", head);
  chunk("IDAT", z);
  chunk("IEND", {});
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  return bool(file);
}

int Import(const std::string& romfs, const std::string& retailDir, const std::string& outDir, const std::string& only = "") {
  using namespace PortRemastered;
  std::filesystem::create_directories(outDir);
  std::set<uint32_t> disc;
  for (const auto& entry : std::filesystem::directory_iterator(retailDir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > 9 && name[8] == '.') {
      disc.insert(uint32_t(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16)));
    }
  }
  struct Open {
    FileReader reader;
    Pak pak;
  };
  std::vector<std::unique_ptr<Open>> paks;
  std::map<EffectGuid, std::pair<size_t, size_t>> where;  // pak, asset
  std::map<EffectGuid, uint32_t> types;
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const std::filesystem::path& path : paths) {
    auto open = std::make_unique<Open>();
    std::string error;
    FileReader& reader = open->reader;
    if (!reader.Open(path.string(), error) ||
        !open->pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                        reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    const std::vector<PakAsset>& assets = open->pak.Assets();
    for (size_t a = 0; a < assets.size(); ++a) {
      if (types.emplace(assets[a].id, assets[a].type).second) {
        where[assets[a].id] = {paks.size(), a};
      }
    }
    paks.push_back(std::move(open));
  }
  EffectImportIO io;
  auto lower = [](std::string text) {
    for (char& c : text) {
      c = char(std::tolower(uint8_t(c)));
    }
    return text;
  };
  for (const auto& [id, type] : types) {
    if ((type == kGenp || type == PortRemastered::EffectFourCC("SWSH")) && (only.empty() || lower(IdToString(id)) == lower(only) || lower(EffectGuidString(id)) == lower(only))) {
      io.effects.push_back(id);
    }
  }
  if (!only.empty() && io.effects.empty()) {
    std::cerr << only << ": no such GENP in the image\n";
    return 1;
  }
  auto read = [&](const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    const auto found = where.find(id);
    if (found == where.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = paks[found->second.first]->pak;
    return pak.ReadAsset(pak.Assets()[found->second.second], out, error);
  };
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    const auto found = types.find(id);
    if (found == types.end() || found->second != type) {
      error = "not in the image";
      return false;
    }
    return read(id, out, error);
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    const auto found = types.find(id);
    return found == types.end() ? 0 : found->second;
  };
  io.retailId = [&](uint32_t id) { return disc.count(id) != 0; };
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X.%c%c%c%c", id, char(type >> 24), char(type >> 16), char(type >> 8),
                  char(type));
    std::ifstream file(std::filesystem::path(retailDir) / name, std::ios::binary);
    if (!file) {
      return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), {});
    return !out.empty();
  };
  std::set<uint32_t> taken;
  io.freshId = [&](uint32_t seed) {
    uint32_t id = seed;
    while (id == 0 || id == 0xFFFFFFFFu || disc.count(id) != 0 || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    return id;
  };
  io.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba, std::string& error) {
    std::vector<uint8_t> raw;
    TxtrImage image;
    if (!read(id, raw, error) || !DecodeTxtr(raw.data(), raw.size(), image, error)) {
      return false;
    }
    width = int(image.width);
    height = int(image.height);
    rgba = std::move(image.rgba);
    return true;
  };
  // As the game's import: the VFX copies (linear or sRGB, a BC5's red and green) follow the format.
  io.textureSrgb = [&](const EffectGuid& id) {
    std::vector<uint8_t> raw;
    std::string ignored;
    TxtrImage info;
    return read(id, raw, ignored) && ReadTxtrInfo(raw.data(), raw.size(), info, ignored) && info.srgb;
  };
  io.textureRg = [&](const EffectGuid& id) {
    std::vector<uint8_t> raw;
    std::string ignored;
    TxtrImage info;
    return read(id, raw, ignored) && ReadTxtrInfo(raw.data(), raw.size(), info, ignored) &&
           (info.format == kTxtrFormatBc5Unorm || info.format == kTxtrFormatBc5Snorm);
  };
  io.layers = [&](const EffectGuid& id, int& width, int& height, int& layers, std::vector<uint8_t>& rgba,
                  std::string& error) {
    std::vector<uint8_t> raw;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t n = 0;
    if (!read(id, raw, error) || !DecodeTxtrLayersRgba8(raw.data(), raw.size(), w, h, n, rgba, error)) {
      return false;
    }
    width = int(w);
    height = int(h);
    layers = int(n);
    return true;
  };
  auto store = [&](const std::string& name, const std::vector<uint8_t>& data) {
    std::ofstream file(std::filesystem::path(outDir) / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(file);
  };
  // Models as the game's import converts them: standalone, one converter for all.
  std::set<uint32_t> claimed;
  std::unique_ptr<Converter> converter;
  // Match the production import: Converter fills the embedded VMSH alongside the CMDL.
  // Keep it by the assigned model id for the subsequent effect conversion callback.
  std::map<uint32_t, std::vector<uint8_t>> modelMeshes;
  io.modelMesh = [&](uint32_t model) {
    const auto found = modelMeshes.find(model);
    return found != modelMeshes.end() ? found->second : std::vector<uint8_t>();
  };
  io.model = [&](const EffectGuid& id, uint32_t retailId, std::string& error) {
    if (!converter) {
      ConvertIO cio;
      cio.retailId = [&](uint32_t other) { return disc.count(other) != 0; };
      cio.claim = [&](uint32_t other) { return claimed.insert(other).second; };
      cio.texture = [&](const ModelUuid& tex, Image& out, std::string& textureError) {
        return io.texture(tex, out.width, out.height, out.rgba, textureError);
      };
      cio.write = store;
      converter = std::make_unique<Converter>(std::move(cio));
    }
    ConvertOptions options;
    options.retail = retailId;
    options.standalone = true;
    options.skip.clear();
    options.nativeMax = 1024;
    options.vmsh = &modelMeshes[retailId];
    std::vector<uint8_t> raw;
    Model model;
    if (!read(id, raw, error) || !ParseModel(raw.data(), raw.size(), model, error) ||
        !converter->Convert(model, options, error)) {
      return false;
    }
    size_t triangles = 0;
    for (const auto& mesh : model.meshes) {
      triangles += mesh.indexCount / 3;
    }
    std::cout << "  model " << EffectGuidString(id) << " -> " << std::hex << retailId << std::dec << ": "
              << model.meshes.size() << " meshes, " << triangles << " triangles, " << converter->PbrMaterials()
              << " PBR / " << converter->TevMaterials() << " TEV materials so far\n";
    return true;
  };
  std::filesystem::create_directories("/tmp/fx-atlas");
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.substr(name.size() - 5) == ".TXTR") {
      // The retail RGBA8 (format 9) the step writes: level 0 is 4x4 blocks of
      // (alpha, red) pairs then (green, blue) pairs.
      auto be = [&](size_t at, int bytes) {
        uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) {
          v = v << 8 | data[at + i];
        }
        return v;
      };
      if (data.size() > 12 && be(0, 4) == 9) {
        const int w = int(be(4, 2)), h = int(be(6, 2));
        std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
        size_t at = 12;
        for (int by = 0; by < h && at + 64 <= data.size(); by += 4) {
          for (int bx = 0; bx < w; bx += 4, at += 64) {
            for (int i = 0; i < 16; ++i) {
              uint8_t* p = &rgba[(size_t(by + i / 4) * size_t(w) + size_t(bx + i % 4)) * 4];
              p[3] = data[at + i * 2];
              p[0] = data[at + i * 2 + 1];
              p[1] = data[at + 32 + i * 2];
              p[2] = data[at + 32 + i * 2 + 1];
            }
          }
        }
        WritePng("/tmp/fx-atlas/" + name.substr(0, name.size() - 5) + ".png", w, h, rgba);
      }
    }
    return store(name, data);
  };
  io.log = [](const std::string& line) { std::cout << "  " << line << "\n"; };
  std::vector<std::string> reportRows;
  io.report = [&](const EffectReportRow& row) {
    reportRows.push_back(FormatEffectRow(row));
    if (!only.empty()) {
      std::cout << "genp " << row.genp << " -> retail " << std::hex << row.retail << std::dec << ": " << row.result
                << " via " << row.method << (row.reason.empty() ? "" : " (" + row.reason + ")") << ", wrote "
                << row.kinds << ", " << row.dropped << " retail properties left out\n";
      for (const std::string& line : row.droppedList) {
        std::cout << "    dropped: " << Printable(line) << "\n";
      }
      for (const std::string& line : row.approximatedList) {
        std::cout << "    approximated: " << Printable(line) << "\n";
      }
    }
  };
  const EffectImportResult result = ImportEffects(io);
  {
    std::ofstream report(std::filesystem::path(outDir) / "effects.tsv", std::ios::binary);
    report << JoinReport(EffectReportHeader(), reportRows);
  }
  std::cout << result.written << " of " << result.candidates << " effects written, " << result.failed << " failed, "
            << result.parts << " PARTs, " << result.textures << " textures (" << result.flipbooks << " flipbooks), "
            << result.models << " models, " << result.dropped << " retail properties left out\n";
  return 0;
}

// A generator's port-only properties (VMAT and its data, XFMD) as text:
// `vmat <file.GENP> <dir of <uuid>.MATI files>`.
int VmatSummary(const std::string& genpPath, const std::string& matiDir) {
  using namespace PortRemastered;
  const std::vector<uint8_t> data = ReadFile(genpPath);
  EffectNode effect;
  std::string error;
  if (data.empty() || !ParseEffect(data.data(), data.size(), effect, error)) {
    std::cerr << genpPath << ": " << error << "\n";
    return 1;
  }
  EffectConvertIO io;
  io.assetId = [](const EffectGuid& guid, uint32_t) -> uint32_t { return EffectRetailId(guid).value_or(0); };
  io.materialData = [&](const EffectGuid& material) {
    return ReadFile(std::filesystem::path(matiDir) / (IdToString(PakId(material)) + ".MATI"));
  };
  io.vfxTexture = [](const EffectGuid& texture) {
    uint32_t hash = 0x80000000u;
    for (uint8_t byte : texture) {
      hash = (hash * 31 + byte) | 0x80000000u;
    }
    return FlipbookAtlas{hash, 1, 1, 1};
  };
  auto be32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
  auto real = [&](const uint8_t* p) {
    const uint32_t bits = be32(p);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  // The generators in the order they convert, to list each one's PMTR rows beside its VPMT.
  std::vector<const EffectNode*> nodes;
  std::function<void(const EffectNode&)> collect = [&](const EffectNode& node) {
    if (EffectRetailType(node.form) != 0) {
      nodes.push_back(&node);
    }
    for (const EffectNode& child : node.children) {
      collect(child);
    }
  };
  collect(effect);
  size_t index = 0, with = 0, without = 0;
  for (const ConvertedPart& part : ConvertEffect(effect, data.data(), io)) {
    ++index;
    std::vector<RetailPartProperty> properties;
    std::cout << "generator " << index << (part.root ? " (root)" : "") << " " << EffectFourCCString(part.type) << "\n";
    if (!SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error)) {
      std::cout << "  does not read: " << error << "\n";
      continue;
    }
    bool has = false;
    for (const RetailPartProperty& property : properties) {
      const uint8_t* b = property.value.data();
      const std::string name = EffectFourCCString(property.fourcc);
      if (name == "VMAT") {
        has = true;
        b += 8;  // CNST, length
        const uint32_t version = be32(b);
        std::cout << "  VMAT v" << be32(b) << " features 0x" << std::hex << be32(b + 4) << std::dec << " blend "
                  << be32(b + 8) << "\n";
        const uint32_t textures = be32(b + 12);
        b += 16;
        for (uint32_t i = 0; i < textures; ++i, b += 44) {
          std::cout << "    tex" << i << " id " << std::hex << be32(b) << std::dec << " uv " << be32(b + 4) << " wrap "
                    << be32(b + 8) << "," << be32(b + 12) << " linear " << be32(b + 16) << " grid " << be32(b + 20) << "x"
                    << be32(b + 24) << "x" << be32(b + 28) << " warped " << be32(b + 32) << " scale " << real(b + 36)
                    << "," << real(b + 40) << "\n";
        }
        static const char* slots[] = {"color", "opacity", "ramp", "ramp2", "threshold", "indirect", "palette"};
        std::cout << "    slots";
        for (const char* slot : slots) {
          std::cout << " " << slot << "=" << int32_t(be32(b));
          b += 4;
        }
        b += 12;  // rampRow[2], addRow
        static const char* srcs[] = {"erosion", "thrX", "thrY", "thrW", "fresnelX", "fresnelY", "fadeX", "fadeY",
                                     "indexScale", "indexOffset", "indexRow"};
        std::cout << "\n    src";
        for (const char* src : srcs) {
          std::cout << " " << src << "=" << (int32_t(be32(b)) >= 0 ? "extra[" + std::to_string(int32_t(be32(b))) + "][" +
                                                                         std::to_string(be32(b + 4)) + "]"
                                                                   : std::to_string(real(b + 8)));
          b += 12;
        }
        std::cout << "\n    modulate " << real(b) << " depthSoften " << real(b + 4) << " spriteCenter " << be32(b + 8) << "\n";
        if (version == 3) {
          std::cout << "    opacityGain " << real(b + 12) << " opacityBias " << real(b + 16) << "\n";
        }
      } else if (name == "VPMT" || name == "VSMT") {
        std::cout << "  " << name << " " << be32(b + 4) << " entries\n";
        if (name == "VPMT" && index <= nodes.size()) {
          for (const EffectProperty& pmtr : nodes[index - 1]->properties) {
            for (const EffectValue& item : pmtr.fourcc == EffectFourCC("PMTR") ? pmtr.value : std::vector<EffectValue>()) {
              std::cout << "    PMTR group " << item.fourcc << " row " << (item.word & 0xff) << " comp "
                        << (item.word >> 8 & 0xff) << " count " << (item.word >> 16 & 0xff) << " "
                        << (item.args.empty() ? "?" : EffectFourCCString(item.args[0].fourcc)) << "\n";
            }
          }
        }
      } else if (name == "VTMT") {
        std::cout << "  VTMT " << be32(b + 4) << " UV sets\n";
      } else if (name == "VORN" || name == "XFMD" || name == "PIRN") {
        std::cout << "  " << name << " " << be32(b + 4) << "\n";
      } else if (name == "SSZE" || name == "ITEN") {
        std::cout << "  " << name << " " << property.value.size() << " bytes\n";
      }
    }
    (has ? with : without) += 1;
    for (const std::string& line : part.approximated) {
      std::cout << "  approximated: " << Printable(line) << "\n";
    }
    for (const std::string& line : part.dropped) {
      std::cout << "  dropped: " << Printable(line) << "\n";
    }
  }
  std::cout << with << " with VMAT, " << without << " without\n";
  return 0;
}

// ---- retail effect files as text -------------------------------------------------------------

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

uint32_t TypeOfFile(const std::string& path) {
  const std::string ext = std::filesystem::path(path).extension().string();
  return ext.size() == 5 ? FourCCOf(ext.substr(1)) : kPartType;
}

std::string FourCCText(uint32_t f) {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    out += char(f >> shift & 0xFF);
  }
  return out;
}

std::string HexText(const std::vector<uint8_t>& bytes, size_t at = 0) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = at; i < bytes.size(); ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 15];
  }
  return out;
}

// Best effort: element trees read as CParticleDataFactory does, for the operators whose
// operands are known (a constant is a float, or an int when `ints`). The caller falls back
// to hex when the value does not consume exactly.
bool ElementText(const std::vector<uint8_t>& v, size_t& at, bool ints, std::string& out, int depth = 0) {
  if (at + 4 > v.size() || depth > 16) {
    return false;
  }
  const std::string type = FourCCText(uint32_t(v[at]) << 24 | uint32_t(v[at + 1]) << 16 | uint32_t(v[at + 2]) << 8 | v[at + 3]);
  at += 4;
  if (type == "CNST") {
    if (at + 4 > v.size()) {
      return false;
    }
    uint32_t bits = uint32_t(v[at]) << 24 | uint32_t(v[at + 1]) << 16 | uint32_t(v[at + 2]) << 8 | v[at + 3];
    at += 4;
    char text[32];
    if (ints) {
      std::snprintf(text, sizeof(text), "%d", int32_t(bits));
    } else {
      float f;
      std::memcpy(&f, &bits, 4);
      std::snprintf(text, sizeof(text), "%g", double(f));
    }
    out += text;
    return true;
  }
  int operands = 0;
  if (type == "ADD_" || type == "MULT" || type == "RAND" || type == "LFTW" || type == "SUB_") {
    operands = 2;
  } else if (type == "CLMP") {
    operands = 3;
  } else if (type == "RLPT") {
    operands = 1;
  } else {
    return false;
  }
  out += type + "(";
  for (int i = 0; i < operands; ++i) {
    out += i != 0 ? ", " : "";
    if (!ElementText(v, at, ints, out, depth + 1)) {
      return false;
    }
  }
  out += ")";
  return true;
}

std::string ValueText(const std::vector<uint8_t>& value) {
  for (const bool ints : {false, true}) {
    size_t at = 0;
    std::string text;
    if (ElementText(value, at, ints, text) && at == value.size()) {
      return text;
    }
  }
  return "hex " + HexText(value);
}

bool Split(const std::string& path, std::vector<PortRemastered::RetailPartProperty>& props) {
  const std::vector<uint8_t> data = ReadFile(path);
  std::string error;
  if (data.empty() || !PortRemastered::SplitRetailEffect(TypeOfFile(path), data.data(), data.size(), props, error)) {
    std::cerr << path << ": " << (data.empty() ? "cannot be read" : error) << "\n";
    return false;
  }
  return true;
}

int PDump(const std::string& path) {
  std::vector<PortRemastered::RetailPartProperty> props;
  if (!Split(path, props)) {
    return 1;
  }
  for (const auto& p : props) {
    std::cout << FourCCText(p.fourcc) << "\t" << ValueText(p.value) << "\n";
  }
  return 0;
}

int PDiff(const std::string& a, const std::string& b) {
  std::vector<PortRemastered::RetailPartProperty> pa, pb;
  if (!Split(a, pa) || !Split(b, pb)) {
    return 1;
  }
  std::map<std::string, std::vector<std::string>> ma, mb;
  for (const auto& p : pa) {
    ma[FourCCText(p.fourcc)].push_back(ValueText(p.value));
  }
  for (const auto& p : pb) {
    mb[FourCCText(p.fourcc)].push_back(ValueText(p.value));
  }
  std::set<std::string> keys;
  for (const auto& [k, v] : ma) keys.insert(k);
  for (const auto& [k, v] : mb) keys.insert(k);
  int differing = 0;
  for (const std::string& key : keys) {
    const auto x = ma.find(key);
    const auto y = mb.find(key);
    const std::vector<std::string> none;
    const auto& xs = x == ma.end() ? none : x->second;
    const auto& ys = y == mb.end() ? none : y->second;
    if (xs == ys) {
      continue;
    }
    ++differing;
    for (const std::string& v : xs) std::cout << "- " << key << "\t" << v << "\n";
    for (const std::string& v : ys) std::cout << "+ " << key << "\t" << v << "\n";
  }
  std::cout << differing << " of " << keys.size() << " properties differ\n";
  return differing == 0 ? 0 : 1;
}

// One Remastered model, converted standalone with every texture its materials name, printing
// each output material's decision. `index` < 0: all of them.
int Mat(const std::string& romfs, const std::string& which, int index) {
  using namespace PortRemastered;
  struct Open {
    FileReader reader;
    Pak pak;
  };
  std::vector<std::unique_ptr<Open>> paks;
  std::map<EffectGuid, std::pair<size_t, size_t>> where;
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  std::string wanted;
  for (char c : which) {
    if (c != '-') wanted += char(std::tolower(uint8_t(c)));
  }
  const EffectGuid* found = nullptr;
  std::map<EffectGuid, std::string> labels;
  for (const std::filesystem::path& path : paths) {
    auto open = std::make_unique<Open>();
    std::string error;
    FileReader& reader = open->reader;
    if (!reader.Open(path.string(), error) ||
        !open->pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                        reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    const std::vector<PakAsset>& assets = open->pak.Assets();
    for (size_t a = 0; a < assets.size(); ++a) {
      if (where.emplace(assets[a].id, std::make_pair(paks.size(), a)).second) {
        std::string idText = IdToString(assets[a].id);
        std::string plain;
        for (char c : idText) {
          if (c != '-') plain += char(std::tolower(uint8_t(c)));
        }
        bool match = plain == wanted;
        for (const std::string& name : assets[a].names) {
          std::string n;
          for (char c : name) n += char(std::tolower(uint8_t(c)));
          if (n.find(wanted) != std::string::npos && assets[a].type == FourCCOf("CMDL")) match = true;
        }
        if (match && (assets[a].type == FourCCOf("CMDL") || assets[a].type == FourCCOf("SMDL"))) {
          labels[assets[a].id] = idText;
        }
      }
    }
    paks.push_back(std::move(open));
  }
  if (labels.empty()) {
    std::cerr << which << ": no such model\n";
    return 1;
  }
  auto read = [&](const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    const auto at = where.find(id);
    if (at == where.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = paks[at->second.first]->pak;
    return pak.ReadAsset(pak.Assets()[at->second.second], out, error);
  };
  (void)found;
  for (const auto& [id, label] : labels) {
    ConvertIO cio;
    cio.retailId = [](uint32_t) { return false; };
    cio.texture = [&](const ModelUuid& tex, Image& out, std::string& error) {
      std::vector<uint8_t> raw;
      TxtrImage image;
      if (!read(tex, raw, error) || !DecodeTxtr(raw.data(), raw.size(), image, error)) {
        return false;
      }
      out.width = int(image.width);
      out.height = int(image.height);
      out.rgba = std::move(image.rgba);
      return true;
    };
    cio.write = [](const std::string&, const std::vector<uint8_t>&) { return true; };
    std::vector<MaterialDecision> decisions;
    cio.decision = [&](const MaterialDecision& d) { decisions.push_back(d); };
    Converter converter(std::move(cio));
    ConvertOptions options;
    options.retail = 0x7F000001;
    options.source = label;
    options.standalone = true;
    options.skip.clear();
    options.nativeMax = 256;
    std::vector<uint8_t> raw;
    Model model;
    std::string error;
    if (!read(id, raw, error) || !ParseModel(raw.data(), raw.size(), model, error) ||
        !converter.Convert(model, options, error)) {
      std::cout << label << ": " << error << "\n";
      continue;
    }
    std::cout << label << ": " << model.meshes.size() << " meshes, " << decisions.size() << " output materials\n";
    std::cout << MaterialReportHeader() << "\n";
    for (const MaterialDecision& d : decisions) {
      if (index < 0 || d.sourceIndex == index) {
        std::cout << FormatMaterialRow(d) << "\n";
      }
    }
    std::cout << "(standalone: every surface is one PBR material; a retail-model conversion needs the disc, so "
                 "its TEV path and `retail model` reasons are not shown)\n";
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "census" && argc == 4) {
    return Convert(argv[2], "-", argv[3], true);
  }
  if (mode == "vmat" && argc == 4) {
    return VmatSummary(argv[2], argv[3]);
  }
  if (mode == "dump" && argc == 3) {
    return Dump(argv[2]);
  }
  if (mode == "kssm" && argc == 3) {
    return Kssm(argv[2]);
  }
  if (mode == "mtin" && argc >= 3) {
    return Materials(argc - 2, argv + 2);
  }
  if (mode == "scan" && (argc == 3 || argc == 4)) {
    return Scan(argv[2], argc == 4 ? argv[3] : "");
  }
  if (mode == "extract" && argc == 4) {
    return Scan(argv[2], argv[3], true);
  }
  if (mode == "convert" && argc == 5) {
    return Convert(argv[2], argv[3], argv[4]);
  }
  if (mode == "import" && argc == 5) {
    return Import(argv[2], argv[3], argv[4]);
  }
  if (mode == "pdump" && argc == 3) {
    return PDump(argv[2]);
  }
  if (mode == "pdiff" && argc == 4) {
    return PDiff(argv[2], argv[3]);
  }
  if (mode == "explain" && argc == 5) {
    const std::string out = (std::filesystem::temp_directory_path() / "effect-tool-explain").string();
    return Import(argv[2], argv[3], out, argv[4]);
  }
  if (mode == "mat" && (argc == 4 || argc == 5)) {
    return Mat(argv[2], argv[3], argc == 5 ? std::atoi(argv[4]) : -1);
  }
  std::cerr << "usage: " << argv[0]
            << " dump <file.GENP> | mtin <file.GENP>... | vmat <file.GENP> <dir of <uuid>.MATI files>"
               " | scan <romfs> [outdir] | extract <romfs> <outdir>"
               " | convert <romfs> <retail|-> <outdir>"
               " | import <romfs> <retail> <outdir> | pdump <file> | pdiff <a> <b>"
               " | explain <romfs> <retail> <GENP id> | mat <romfs> <model id|name> [material index]\n";
  return 2;
}
