// The Remastered room environment writer's ROOM file reader, a port of
// build/mpr/roomtools/roomlib.py. See port_remastered_room_internal.h.

#include "port_remastered_room_internal.h"

namespace PortRemastered {
namespace RoomDetail {

// ---------------------------------------------------------------------------
// ROOM files (roomlib.py)
// ---------------------------------------------------------------------------


bool Room::Chunks(size_t o, size_t end, std::vector<Chunk>& out, std::string& error) const {
  const std::vector<uint8_t>& d = *m_d;
  while (o < end) {
    if (out.size() > kMaxChunks) {
      error = "too many chunks";
      return false;
    }
    if (d.size() < 4 || o > d.size() - 4) {
      error = "truncated chunk";
      return false;
    }
    const bool form = std::memcmp(&d[o], "RFRM", 4) == 0;
    const size_t header = form ? 32 : 24;
    if (o > d.size() || d.size() - o < header) {
      error = "truncated chunk header";
      return false;
    }
    const uint64_t size = ReadLE64(&d[o + 4]);
    if (size > d.size() - o - header) {
      error = "a chunk runs past the end of the file";
      return false;
    }
    out.push_back({form ? ReadBE32(&d[o + 20]) : ReadBE32(&d[o]), o + header, size_t(size)});
    o += header + size_t(size);
  }
  return true;
}

bool Room::Find(size_t o, size_t end, const uint32_t* path, size_t depth, std::vector<Span>& out,
                std::string& error) const {
  std::vector<Chunk> chunks;
  if (!Chunks(o, end, chunks, error)) {
    return false;
  }
  for (const Chunk& c : chunks) {
    if (c.id != path[0]) {
      continue;
    }
    if (depth == 1) {
      out.push_back({c.start, c.size});
    } else if (!Find(c.start, c.start + c.size, path + 1, depth - 1, out, error)) {
      return false;
    }
  }
  return true;
}

bool Room::Parse(const std::vector<uint8_t>& data, std::string& error) {
  m_d = &data;
  m_comps.clear();
  m_byGuid.clear();
  m_strings = {};
  m_meta = {};
  m_listed.clear();
  m_blit = {};
  const std::vector<uint8_t>& d = data;
  if (d.size() < 32 || std::memcmp(d.data(), "RFRM", 4) != 0) {
    error = "not an RFRM file";
    return false;
  }
  const size_t rs = 32;
  const uint64_t rsz = ReadLE64(&d[4]);
  if (rsz > d.size() - rs) {
    error = "the form runs past the end of the file";
    return false;
  }
  const size_t re = rs + size_t(rsz);
  // The world master's variables, in the form that follows the root one: one that does not read
  // is left out.
  {
    std::vector<Span> meta;
    std::string ignored;
    const uint32_t pMeta[] = {FourCC("FOOT"), FourCC("META")};
    if (Find(re, d.size(), pMeta, 2, meta, ignored) && !meta.empty()) {
      m_meta = meta[0];
    }
  }
  std::vector<Span> sden, idta, layers, strp;
  const uint32_t pSden[] = {FourCC("SDTA"), FourCC("SDEN")}, pIdta[] = {FourCC("SDTA"), FourCC("IDTA")},
                 pLayr[] = {FourCC("LYRS"), FourCC("LAYR")}, pStrp[] = {FourCC("STRP")};
  if (!Find(rs, re, pSden, 2, sden, error) || !Find(rs, re, pIdta, 2, idta, error) ||
      !Find(rs, re, pLayr, 2, layers, error) || !Find(rs, re, pStrp, 1, strp, error)) {
    return false;
  }
  // STRP: u32 16, u32 1, u32 the pool's size, then the pool (NUL-separated names).
  if (!strp.empty() && strp[0].size >= 12) {
    const size_t pool = ReadLE32(&d[strp[0].start + 8]);
    m_strings = {strp[0].start + 12, std::min(pool, strp[0].size - 12)};
  }
  // LRES: u32 count, then that many 16-byte ids.
  std::vector<Span> lres;
  const uint32_t pLres[] = {FourCC("HEAD"), FourCC("LUNT"), FourCC("LRES")};
  if (!Find(rs, re, pLres, 3, lres, error)) {
    return false;
  }
  for (const Span& s : lres) {
    const size_t n = s.size >= 4 ? std::min<size_t>(ReadLE32(&d[s.start]), (s.size - 4) / 16) : 0;
    for (size_t i = 0; i < n; ++i) {
      m_listed.push_back(s.start + 4 + 16 * i);
    }
  }
  // BLIT: u32 flags, the lightmap's guid, u32 n and n component guids, u32 n and n vec4
  // lookups (parallel to the guids), then an LTPB guid. One that does not fit is left out.
  std::vector<Span> blit;
  const uint32_t pBlit[] = {FourCC("HEAD"), FourCC("BLIT")};
  if (!Find(rs, re, pBlit, 2, blit, error)) {
    return false;
  }
  if (!blit.empty() && blit[0].size >= 24) {
    const uint8_t* b = &d[blit[0].start];
    const size_t size = blit[0].size;
    const size_t n = ReadLE32(b + 20);
    if (n <= (size - 24) / 16 && size - 24 - 16 * n >= 4) {
      const size_t at = 24 + 16 * n;
      const size_t m = ReadLE32(b + at);
      if (m == n && m <= (size - at - 4) / 16) {
        m_blit.present = true;
        m_blit.flags = ReadLE32(b);
        std::memcpy(m_blit.lightmap.data(), b + 4, 16);
        for (size_t i = 0; i < n; ++i) {
          Id16 g;
          std::memcpy(g.data(), b + 24 + 16 * i, 16);
          std::array<float, 4> v;
          for (size_t k = 0; k < 4; ++k) {
            v[k] = ReadLEFloat(b + at + 4 + 16 * i + 4 * k);
          }
          m_blit.lookups[g] = v;
        }
      }
    }
  }
  std::map<Id16, size_t>& byGuid = m_byGuid;
  for (size_t li = 0; li < layers.size(); ++li) {
    // The header (LHED) names the layer "LU_<layer unit>_PG_…"; the base layer has no unit.
    static constexpr char kUnit[] = "LU_";
    const uint8_t* const head = d.data() + layers[li].start;
    const uint8_t* const headEnd = head + std::min<size_t>(layers[li].size, 256);
    const uint8_t* const unit = std::search(head, headEnd, kUnit, kUnit + 3);
    const bool baseLayer = unit + 3 < headEnd && unit[3] == '_';
    std::vector<Span> comps;
    const uint32_t pComp[] = {FourCC("SRIP"), FourCC("COMP")};
    if (!Find(layers[li].start, layers[li].start + layers[li].size, pComp, 2, comps, error)) {
      return false;
    }
    for (const Span& cs : comps) {
      // The records are 12 bytes; a short tail is not one.
      for (size_t o = cs.start; o + 12 <= cs.start + cs.size; o += 12) {
        const uint32_t type = ReadLE32(&d[o]), pi = ReadLE32(&d[o + 4]), ii = ReadLE32(&d[o + 8]);
        if (pi >= sden.size() || ii >= idta.size()) {
          error = "a component points outside the data tables";
          return false;
        }
        Component c;
        c.type = type;
        c.layer = int(li);
        c.baseLayer = baseLayer;
        const Span s = sden[pi];
        c.raw = s.size >= 4 ? Span{s.start + 4, s.size - 4} : Span{s.start, 0};
        c.idta = idta[ii];
        if (c.idta.start <= d.size() && d.size() - c.idta.start >= 16) {
          std::memcpy(c.guid.data(), &d[c.idta.start], 16);
          c.hasGuid = true;
        }
        if (c.hasGuid) {
          byGuid[c.guid] = m_comps.size();
        }
        m_comps.push_back(c);
        if (m_comps.size() > kMaxChunks) {
          error = "too many components";
          return false;
        }
      }
    }
  }
  // An entity lists the components it owns as 'PMOC' + guid in its id data.
  for (size_t i = 0; i < m_comps.size(); ++i) {
    if (m_comps[i].type != kEntity) {
      continue;
    }
    const Span a = m_comps[i].idta;
    if (a.start > d.size() || a.size > d.size() - a.start) {
      continue;
    }
    const uint8_t* blob = &d[a.start];
    size_t o = 0;
    while (o + 4 <= a.size) {
      const uint8_t* hit = nullptr;
      for (size_t k = o; k + 4 <= a.size; ++k) {
        if (std::memcmp(blob + k, "PMOC", 4) == 0) {
          hit = blob + k;
          break;
        }
      }
      if (hit == nullptr) {
        break;
      }
      o = size_t(hit - blob);
      if (o + 20 <= a.size) {
        Id16 g;
        std::memcpy(g.data(), blob + o + 4, 16);
        const auto it = byGuid.find(g);
        if (it != byGuid.end()) {
          m_comps[it->second].entity = int(i);
        }
      }
      o += 20;
    }
  }
  return true;
}

// [(id, data)] if b is exactly a property list; false otherwise.
bool PropList(const uint8_t* b, size_t len, std::vector<Prop>* out) {
  if (len < 2) {
    return false;
  }
  const size_t n = ReadLE16(b);
  size_t o = 2;
  for (size_t i = 0; i < n; ++i) {
    if (o + 6 > len) {
      return false;
    }
    const uint32_t id = ReadLE32(b + o);
    const size_t size = ReadLE16(b + o + 4);
    o += 6;
    if (o + size > len) {
      return false;
    }
    if (out != nullptr) {
      out->push_back({id, {o, size}});
    }
    o += size;
  }
  return o == len;
}

bool Room::Nested(const Component& c, const uint32_t* path, size_t count, Span& out) const {
  Span at = c.raw;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t id = path[i];
    std::vector<Prop> props;
    if (!PropList(Bytes(at), at.size, &props)) {
      return false;
    }
    bool found = false;
    for (const Prop& p : props) {
      if (p.id == id) {
        at = {at.start + p.data.start, p.data.size};
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  out = at;
  return true;
}

std::map<uint32_t, Span> Room::Flat(const Component& c) const {
  std::map<uint32_t, Span> out;
  const uint8_t* b = Bytes(c.raw);
  std::vector<Prop> props;
  if (!PropList(b, c.raw.size, &props) || props.empty()) {
    return out;
  }
  for (const Prop& p : props) {
    if (p.data.size > 4) {
      std::vector<Prop> sub;
      if (PropList(b + p.data.start, p.data.size, &sub) && !sub.empty()) {
        continue;
      }
    }
    out[p.id] = {c.raw.start + p.data.start, p.data.size};
  }
  return out;
}

}  // namespace RoomDetail
}  // namespace PortRemastered
