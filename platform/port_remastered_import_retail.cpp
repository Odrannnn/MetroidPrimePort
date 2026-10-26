// The Remastered import's reader of the retail disc. See port_remastered_import_internal.h.

#include "port_remastered_import_internal.h"

namespace PortRemastered {
namespace ImportDetail {

Retail::~Retail() {
  for (auto& [entry, handle] : m_handles) {
    aurora_dvd_base_close(handle);
  }
}

bool Retail::Index(std::string& error) {
  const int32_t baseCount = aurora_dvd_base_entry_count();
  for (const auto& [entry, path] : PortMods::DiscPaks()) {
    if (entry < 0 || entry >= baseCount) {
      continue;  // a PAK only a mod brings
    }
    void* handle = aurora_dvd_base_open(entry);
    if (handle == nullptr) {
      continue;
    }
    m_handles[entry] = handle;
    std::vector<uint8_t> header;
    PortMods::PakTable table;
    size_t needed = 0x10000;
    bool parsed = false;
    while (needed <= (64u << 20)) {
      header.resize(needed);
      const size_t got = ReadAt(handle, 0, header.data(), header.size());
      if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
        parsed = true;
        break;
      }
      if (got < header.size() || needed <= header.size()) {
        break;
      }
    }
    if (!parsed) {
      continue;
    }
    // The named resources (a pak's first section): the projectile weapons' names pair them
    // with Remastered's WPSMs. Kept out of m_resources, which the fingerprint covers.
    if (header.size() >= 12) {
      const auto be32 = [&header](size_t at) { return port::GetBE32(header, at); };
      size_t at = 12;
      for (uint32_t n = be32(8); n > 0 && at + 12 <= header.size(); --n) {
        const uint32_t length = be32(at + 8);
        if (at + 12 + length > header.size()) {
          break;
        }
        if (be32(at) == kWPSC) {
          std::string name(reinterpret_cast<const char*>(header.data() + at + 12), length);
          std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
          if (!m_wpscNames.emplace(name, be32(at + 4)).second) {
            m_ambiguousWpscNames.insert(std::move(name));
          }
        }
        at += 12 + length;
      }
    }
    for (const PortMods::PakResource& res : table.resources) {
      m_ids.insert(res.id);
      ++m_resourceCounts[Key(res.type, res.id)];
      // The skeletons too (the Dread Samus import's), and the fonts (the HUD's text sizes), kept out
      // of the fingerprint as the weapons are.
      if (res.type == kWPSC || res.type == kCINF || res.type == kFONT) {
        m_wpsc.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
      }
      if (res.type == kCMDL || res.type == kCSKR || res.type == kANCS || res.type == kTXTR || res.type == kMLVL ||
          res.type == kMREA || res.type == kSTRG || res.type == kFRME || res.type == kMAPA || res.type == kMAPW ||
          res.type == kCRSC) {
        m_resources.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
      }
    }
  }
  if (m_resources.empty()) {
    error = "no models found on the disc";
    return false;
  }
  return true;
}

bool Retail::HasUniqueResource(uint32_t type, uint32_t id) const {
  const auto found = m_resourceCounts.find(Key(type, id));
  return found != m_resourceCounts.end() && found->second == 1;
}

std::vector<uint32_t> Retail::Ids(uint32_t type) const {
  std::vector<uint32_t> ids;
  for (const auto& [key, where] : m_resources) {
    if (uint32_t(key >> 32) == type) {
      ids.push_back(uint32_t(key));
    }
  }
  return ids;
}

uint64_t Retail::Fingerprint() const {
  uint64_t sum = m_resources.size() * 0x9E3779B97F4A7C15ull + m_ids.size();
  for (const auto& [key, where] : m_resources) {
    const std::string text = std::to_string(key) + " " + std::to_string(where.entry) + " " +
                             std::to_string(where.offset) + " " + std::to_string(where.size) +
                             (where.compressed ? " z" : "");
    sum += Hash64(text);
  }
  for (const uint32_t id : m_ids) {
    sum ^= Hash64(std::to_string(id)) * 3;
  }
  return sum;
}

bool Retail::WpscId(const std::string& name, uint32_t& id) const {
  if (m_ambiguousWpscNames.count(name) != 0) {
    return false;
  }
  const auto found = m_wpscNames.find(name);
  if (found == m_wpscNames.end()) {
    return false;
  }
  id = found->second;
  return true;
}

bool Retail::Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
  auto found = m_resources.find(Key(type, id));
  if (found == m_resources.end()) {
    found = m_wpsc.find(Key(type, id));
    if (found == m_wpsc.end()) {
      return false;
    }
  }
  const Where& where = found->second;
  if (!where.compressed) {
    std::vector<uint8_t> raw(where.size);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
        return false;
      }
    }
    out = std::move(raw);
    return true;
  }
  // Read straight into the string the inflater takes, so the blob is not copied into one.
  std::string raw(where.size, '\0');
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ReadAt(m_handles[where.entry], where.offset, reinterpret_cast<uint8_t*>(raw.data()), raw.size()) !=
        raw.size()) {
      return false;
    }
  }
  // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
  // data, and a checksum the decoder never reaches.
  if (raw.size() < 6) {
    return false;
  }
  const size_t length = port::ReadBE32(reinterpret_cast<const uint8_t*>(raw.data()));
  raw.erase(0, 6);
  PortWs::Inflater inflater;
  inflater.SetKeepWindow(false);
  std::string inflated;
  if (!inflater.InflateMessage(raw, inflated, length) || inflated.size() != length) {
    return false;
  }
  out.assign(inflated.begin(), inflated.end());
  return true;
}

size_t Retail::ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
  if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
    return 0;
  }
  size_t done = 0;
  while (done < size) {
    const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
    if (got <= 0) {
      break;
    }
    done += size_t(got);
  }
  return done;
}

}  // namespace ImportDetail
}  // namespace PortRemastered
