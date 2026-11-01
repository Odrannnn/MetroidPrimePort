// Suit colour customisation's sources and cache (port_suit_colours.h): reads
// each suit's retail textures (and the generated balls'), recolours them to
// the picks, and keeps them in <user folder>/generated/suit-colours/<suit>/
// under a stamp of the generator version, the disc and the picks.

#include "port_suit_colours.h"

#include "port_ball_looks.h"
#include "port_disc.h"
#include "port_dread_samus_mod.h"
#include "port_disc_reader.h"
#include "port_log.h"
#include "port_model_variant.h"
#include "port_paths.h"
#include "port_remastered_txtr.h"
#include "port_strings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace PortSuitColours {
namespace {

constexpr uint32_t kTXTR = 0x54585452;
constexpr const char* kStampName = "stamp.txt";

std::string FileName(uint32_t id) {
  char name[16];
  std::snprintf(name, sizeof(name), "%08X.TXTR", id);
  return name;
}

// A texture to recolour: retail, or a generated ball's (port_ball_looks.h)
// when `look` is set. `from` is a texture of `fromSuit` with the same UVs
// whose slot split it takes.
struct Source {
  uint32_t id;
  int look = -1;
  uint32_t from = 0;
  int fromSuit = 0;
};

constexpr int kPower = 0, kGravity = 1, kVaria = 2, kPhazon = 3;

// Each suit's own diffuse textures: the body (cutscenes, the pause screen's
// doll), the first-person arm, then the morph ball. See
// build/s-suitcolour-20261022/suit_table.txt for where each is drawn.
std::vector<Source> Sources(int suit) {
  using PortBallLooks::Look;
  switch (suit) {
  case kPower:
    return {{0x50BA2229}, {0xC0625536}, {0xE323D51D}, {0xEBD3C428}, {0xB2F8703C},
            {PortBallLooks::kBallPower},
            {PortModelVariant::Id(PortBallLooks::kSpiderVaria, kPower), int(Look::PowerSpider)},
            {PortModelVariant::Id(PortBallLooks::kLowPolyVaria, kPower), int(Look::PowerSpider)}};
  case kGravity:
    return {{0x10E6A980}, {0x33A729AB}, {0xC901566D}, {0xFE5F7B41}, {0x7FD98F23}, {0x985C0EAA},
            {PortBallLooks::kSpiderGravity}, {PortBallLooks::kLowPolyGravity},
            {PortModelVariant::Id(PortBallLooks::kBallVaria, kGravity), int(Look::Gravity)}};
  case kVaria:
    return {{0x18443363, -1, 0xC0625536, kPower}, {0x1C48ADEA, -1, 0x50BA2229, kPower}, {0x3B05B348},
            {0x8B774EA2, -1, 0xEBD3C428, kPower}, {0x309AA3D4, -1, 0xB2F8703C, kPower},
            {PortBallLooks::kBallVaria, -1, PortBallLooks::kBallPower, kPower},
            {PortBallLooks::kSpiderVaria}, {PortBallLooks::kLowPolyVaria}};
  case kPhazon:
    return {{0x127E7854}, {0x287BD568}, {0x604DA35A}, {0x74C46891}, {0x32541D97}, {0xC94DD270},
            {PortBallLooks::kPhazonShell},
            {PortModelVariant::Id(PortBallLooks::kBallVaria, kPhazon), int(Look::Phazon)}};
  case 4: return {{0x83BF93AA}, {0x9733896D}, {0xC00F4EB6}, {0x917987E9}};
  case 5: return {{0xD2DBEDB7}, {0xDE41AE5E}, {0x5629B84C}, {0x62D5C2D0}};
  case 6: return {{0x0E483BE6}, {0x4445373A}, {0x3FAAF7A9}, {0x424EC5F7}};
  case 7: return {{0x68EC596F}, {0xD033616E}, {0x9DF3B473}, {0xB1986E4B}};
  default: return {};
  }
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream in(PortPaths::detail::FromUtf8(path), std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

bool HasTransparency(const PortRemastered::Image& image) {
  for (size_t at = 3; at < image.rgba.size(); at += 4) {
    if (image.rgba[at] < 128) {
      return true;
    }
  }
  return false;
}

// Each file goes in whole (written aside, then renamed), and the stamp last,
// so an interrupted write leaves no stamp and is made again.
bool Put(const fs::path& dir, const std::string& name, const char* data, size_t size) {
  std::error_code ec;
  const fs::path file = dir / name;
  fs::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(data, std::streamsize(size));
    out.close();
    if (!out) {
      fs::remove(temporary, ec);
      PortLog::Write("suit colours: cannot write %s\n", PortPaths::detail::ToUtf8(file).c_str());
      return false;
    }
  }
  fs::rename(temporary, file, ec);
  if (ec) {
    fs::remove(temporary, ec);
    PortLog::Write("suit colours: cannot write %s\n", PortPaths::detail::ToUtf8(file).c_str());
    return false;
  }
  return true;
}

}  // namespace

std::vector<PortMods::LooseResource> Collect(
    int suit, const Picks& picks, const std::map<std::pair<uint32_t, uint32_t>, PortMods::LooseResource>& loose,
    bool (*native)(uint32_t id)) {
  const std::string picksKey = PicksKey(suit, picks);
  const std::string& user = PortPaths::UserFolder();
  if (picksKey.empty() || user.empty()) {
    return {};
  }
  const Suit& info = SuitInfo(suit);

  // What to make: a mod's own texture stays; a generated ball's is read from
  // its cache file, and only while that look is served.
  struct Job {
    Source source;
    std::string generated;  // host path of the generated texture, or empty for the disc's
  };
  std::vector<Job> jobs;
  std::string inputs;  // the generated balls read, which change with port_ball_looks.h
  for (const Source& source : Sources(suit)) {
    const auto found = loose.find({kTXTR, source.id});
    if (source.look >= 0) {
      if (found == loose.end() || found->second.mod != "generated") {
        continue;
      }
      jobs.push_back({source, found->second.hostPath});
      inputs += " " + FileName(source.id) + "@" + std::to_string(PortBallLooks::kVersion);
      continue;
    }
    if (found != loose.end() || (native != nullptr && native(source.id))) {
      PortLog::Write("suit colours: %s: a mod has its own %s; left as it is\n", info.name,
                     FileName(source.id).c_str());
      continue;
    }
    jobs.push_back({source, {}});
  }
  if (jobs.empty()) {
    return {};
  }

  const fs::path dir = PortPaths::detail::FromUtf8(user) / "generated" / "suit-colours" / info.key;
  const std::string stamp = std::string("suit-colours ") + std::to_string(kVersion) + " " +
                            PortDisc::Name(PortDisc::Current()) + " " + info.key + " " + picksKey + inputs + "\n";
  std::error_code ec;
  const auto cached = [&]() {
    std::vector<PortMods::LooseResource> found;
    for (const Job& job : jobs) {
      const fs::path file = dir / FileName(job.source.id);
      const uintmax_t size = fs::file_size(file, ec);
      if (ec || size == 0) {
        continue;  // could not be made: logged when it was tried
      }
      found.push_back({kTXTR, job.source.id, PortPaths::detail::ToUtf8(file), size, "generated"});
    }
    return found;
  };
  {
    std::ifstream in(dir / kStampName);
    if (in.is_open() && port::ReadAllText(in) == stamp) {
      std::vector<PortMods::LooseResource> found = cached();
      if (!found.empty()) {
        PortLog::Write("suit colours: %s from the cache in %s\n", info.name, PortPaths::detail::ToUtf8(dir).c_str());
        return found;
      }
    }
  }

  fs::remove(dir / kStampName, ec);
  for (const Job& job : jobs) {
    fs::remove(dir / FileName(job.source.id), ec);
  }
  PortDiscReader disc;
  disc.Index();
  const auto decode = [&](const Job& job, PortRemastered::Image& image) {
    std::vector<uint8_t> txtr;
    const bool read = job.generated.empty() ? disc.Read(kTXTR, job.source.id, txtr) : ReadFile(job.generated, txtr);
    return read && PortBallLooks::DecodeCmprTxtr(txtr, image);
  };
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> outputs;
  for (const Job& job : jobs) {
    PortRemastered::Image image;
    if (!decode(job, image)) {
      PortLog::Write("suit colours: %s: %s is missing or not CMPR; left as it is\n", info.name,
                     FileName(job.source.id).c_str());
      continue;
    }
    PortRemastered::Image from;
    bool haveFrom = false;
    if (job.source.from != 0) {
      haveFrom = decode({{job.source.from}, {}}, from);
    }
    const bool alpha = HasTransparency(image);
    const size_t changed = Recolour(image, suit, picks, haveFrom ? &from : nullptr, job.source.fromSuit);
    PortLog::Write("suit colours: %s: %s: %zu of %d texels recoloured\n", info.name, FileName(job.source.id).c_str(),
                   changed, image.width * image.height);
    outputs.emplace_back(job.source.id, PortRemastered::EncodeTxtrCmpr(image, alpha, PortRemastered::MapKind::Colour));
  }
  if (outputs.empty()) {
    return {};
  }
  fs::create_directories(dir, ec);
  for (const auto& [id, data] : outputs) {
    if (!Put(dir, FileName(id), reinterpret_cast<const char*>(data.data()), data.size())) {
      return {};
    }
  }
  if (!Put(dir, kStampName, stamp.data(), stamp.size())) {
    return {};
  }
  PortLog::Write("suit colours: %s made in %s\n", info.name, PortPaths::detail::ToUtf8(dir).c_str());
  return cached();
}

std::vector<std::pair<uint32_t, std::string>> CollectDread(int suit, const Picks& picks, const std::string& modDir,
                                                           const std::function<std::string(uint32_t)>& source) {
  const std::string picksKey = PicksKey(suit, picks);
  const std::string& user = PortPaths::UserFolder();
  if (suit < kDreadFirst || suit >= kSuitCount || picksKey.empty() || user.empty() || modDir.empty()) {
    return {};
  }
  const Suit& info = SuitInfo(suit);
  const fs::path mod = PortPaths::detail::FromUtf8(modDir);

  // "<suit> <texture id>" lines, the suit as CPlayerState::EPlayerSuit.
  std::vector<std::pair<uint32_t, std::string>> jobs;  // (id, the .dds served now)
  {
    std::ifstream list(mod / PortDreadSamus::kSuitColoursName);
    if (!list.is_open()) {
      PortLog::Write("suit colours: %s: the Dread Samus import has no %s; import it again\n", info.name,
                     PortDreadSamus::kSuitColoursName);
      return {};
    }
    std::string line;
    while (std::getline(list, line)) {
      line = line.substr(0, line.find('#'));
      std::istringstream fields(line);
      int listed = -1;
      std::string hex;
      if (!(fields >> listed >> hex) || listed != suit - kDreadFirst) {
        continue;
      }
      char* end = nullptr;
      const unsigned long id = std::strtoul(hex.c_str(), &end, 16);
      if (end == hex.c_str() || *end != '\0') {
        continue;
      }
      std::string path = source(uint32_t(id));
      if (path.empty()) {
        PortLog::Write("suit colours: %s: another mod has its own %08X; left as it is\n", info.name, unsigned(id));
        continue;
      }
      jobs.emplace_back(uint32_t(id), std::move(path));
    }
  }
  if (jobs.empty()) {
    return {};
  }

  const auto ddsName = [](uint32_t id) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X.dds", id);
    return std::string(name);
  };
  std::string sources;  // the import's stamp and the sources' sizes: a new import makes them again
  {
    std::ifstream in(mod / PortDreadSamus::kStampName);
    std::string text = in.is_open() ? port::ReadAllText(in) : std::string();
    std::replace(text.begin(), text.end(), '\n', ' ');
    sources = text;
  }
  std::error_code ec;
  for (const auto& [id, path] : jobs) {
    const uintmax_t size = fs::file_size(PortPaths::detail::FromUtf8(path), ec);
    sources += " " + ddsName(id) + "@" + std::to_string(ec ? 0 : size);
  }
  const fs::path dir = PortPaths::detail::FromUtf8(user) / "generated" / "suit-colours" / info.key;
  const PortRemastered::DdsFormat format = PortRemastered::ColourDdsFormat();
  const std::string stamp = std::string("suit-colours ") + std::to_string(kVersion) + " " +
                            PortRemastered::TextureFormatName() + " " + info.key + " " + picksKey + " " + sources +
                            "\n";
  const auto cached = [&]() {
    std::vector<std::pair<uint32_t, std::string>> found;
    for (const auto& job : jobs) {
      const fs::path file = dir / ddsName(job.first);
      const uintmax_t size = fs::file_size(file, ec);
      if (ec || size == 0) {
        continue;  // could not be made: logged when it was tried
      }
      found.emplace_back(job.first, PortPaths::detail::ToUtf8(file));
    }
    return found;
  };
  {
    std::ifstream in(dir / kStampName);
    if (in.is_open() && port::ReadAllText(in) == stamp) {
      std::vector<std::pair<uint32_t, std::string>> found = cached();
      if (!found.empty()) {
        PortLog::Write("suit colours: %s from the cache in %s\n", info.name, PortPaths::detail::ToUtf8(dir).c_str());
        return found;
      }
    }
  }

  fs::remove(dir / kStampName, ec);
  for (const auto& job : jobs) {
    fs::remove(dir / ddsName(job.first), ec);
  }
  fs::create_directories(dir, ec);
  size_t made = 0;
  for (const auto& [id, path] : jobs) {
    std::vector<uint8_t> dds;
    PortRemastered::TxtrImage decoded;
    std::string error;
    if (!ReadFile(path, dds) || !PortRemastered::DecodeDds(dds.data(), dds.size(), decoded, error)) {
      PortLog::Write("suit colours: %s: cannot read %s (%s); left as it is\n", info.name, path.c_str(),
                     error.empty() ? "missing" : error.c_str());
      continue;
    }
    PortRemastered::Image image;
    image.width = int(decoded.width);
    image.height = int(decoded.height);
    image.rgba = std::move(decoded.rgba);
    image.srgb = decoded.srgb;
    const size_t changed = Recolour(image, suit, picks);
    PortLog::Write("suit colours: %s: %s: %zu of %d texels recoloured\n", info.name, ddsName(id).c_str(), changed,
                   image.width * image.height);
    const std::vector<uint8_t> out =
        PortRemastered::EncodeDds(image, format, false, PortRemastered::MapKind::Colour, image.srgb);
    if (out.empty() || !Put(dir, ddsName(id), reinterpret_cast<const char*>(out.data()), out.size())) {
      return {};
    }
    ++made;
  }
  if (made == 0 || !Put(dir, kStampName, stamp.data(), stamp.size())) {
    return {};
  }
  PortLog::Write("suit colours: %s made in %s\n", info.name, PortPaths::detail::ToUtf8(dir).c_str());
  return cached();
}

}  // namespace PortSuitColours
