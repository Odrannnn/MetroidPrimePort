// USA 1.02 (rev2) skip-stream selection and the Chapel of the Elders override:
// on a synthetic room holding exactly what the USA stream expects, the USA
// stream applies, and on one laid out as rev2 does (the remconn moved away) it
// misses once, where the override applies cleanly. No disc data is used; the
// all-disc replay is tests/port_skip_rev2.py.
#include "port_skip_cutscenes.h"
#include "port_skip_streams.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <set>
#include <vector>

namespace {

// The generated tables again, to check the selection against every room.
struct SkipRoom {
  uint32_t mrea;
  uint32_t offset;
  uint32_t size;
};
#include "port_skip_cutscenes_data.inc"

using PortDisc::Version;
using PortSkipCutscenes::SkipStream;

const uint32_t kChapel = 0x40C548E9;
const uint32_t kDropSender = 0x000E0173, kDropTarget = 0x000E00B2;
const uint32_t kKeepSender = 0x000E0180;

uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Op {
  const uint8_t* at;
  size_t len;
  uint8_t kind() const { return at[0]; }
};

std::vector< Op > Split(const SkipStream& s) {
  std::vector< Op > ops;
  size_t p = 0;
  while (p < s.size) {
    const uint8_t* o = s.ops + p;
    size_t n = 0;
    switch (o[0]) {
    case 1: n = 10; break;
    case 2: {
      const size_t nlen = U16(o + 5);
      const size_t count = U16(o + 7 + nlen);
      n = 9 + nlen;
      for (size_t i = 0; i < count; ++i)
        n += 4 + U16(o + n + 2);
      break;
    }
    case 3:
    case 4: n = 17; break;
    case 5:
      n = 9 + 12 * size_t(U16(o + 7));
      n += 4 + U32(o + n);
      break;
    case 6: n = 5; break;
    case 7: n = 6; break;
    default: CHECK_MSG(false, "op kind"); return ops;
    }
    ops.push_back({o, n});
    p += n;
  }
  CHECK_MSG(p == s.size, "stream splits exactly");
  return ops;
}

bool IsConn(const Op& op, uint8_t kind, uint32_t sender, uint32_t target) {
  return op.kind() == kind && U32(op.at + 1) == sender && U32(op.at + 13) == target;
}

struct Obj {
  std::vector< uint32_t > conns; // state, msg, target triples
};

void Put32(std::vector< uint8_t >& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8)
    v.push_back(uint8_t(x >> s));
}

// A one-layer room holding every object the stream edits or sends from, with
// the connections its REMCONN ops remove (all but `omit`).
std::vector< uint8_t > BuildRoom(const SkipStream& stream, const Op* omit) {
  std::map< uint32_t, Obj > objs;
  uint32_t maxLayer = 0;
  for (const Op& op : Split(stream)) {
    const uint8_t* o = op.at;
    switch (op.kind()) {
    case 1: objs[U32(o + 2)]; maxLayer = std::max<uint32_t>(maxLayer, o[1]); break;
    case 2: objs[U32(o + 1)]; break;
    case 3:
    case 4: objs[U32(o + 1)]; break;
    case 5: maxLayer = std::max<uint32_t>(maxLayer, o[1]); break;
    case 6: break;
    case 7: objs[U32(o + 2)]; maxLayer = std::max<uint32_t>(maxLayer, o[1]); break;
    }
  }
  for (const Op& op : Split(stream)) {
    if (op.kind() != 3 || (omit && op.at == omit->at))
      continue;
    Obj& obj = objs[U32(op.at + 1)];
    for (int i = 0; i < 3; ++i)
      obj.conns.push_back(U32(op.at + 5 + 4 * i));
  }
  std::vector< uint8_t > room = {'S', 'C', 'L', 'Y', 0, 0, 0, 1};
  Put32(room, maxLayer + 1);
  std::vector< std::vector< uint8_t > > layers(maxLayer + 1);
  for (auto& layer : layers) {
    layer.push_back(0);
    Put32(layer, 0);
  }
  for (const auto& [id, obj] : objs) {
    std::vector< uint8_t > body;
    Put32(body, id);
    Put32(body, uint32_t(obj.conns.size() / 3));
    for (uint32_t c : obj.conns)
      Put32(body, c);
    Put32(body, 1);
    body.push_back(0); // empty name
    body.resize(body.size() + 4096, 0);
    std::vector< uint8_t >& layer = layers[0];
    layer.push_back(0x05);
    Put32(layer, uint32_t(body.size()));
    layer.insert(layer.end(), body.begin(), body.end());
    Put32(layer, 0); // placeholder, fixed below
    layer.resize(layer.size() - 4);
    const uint32_t count = U32(layer.data() + 1) + 1;
    for (int i = 0; i < 4; ++i)
      layer[1 + i] = uint8_t(count >> (24 - 8 * i));
  }
  for (const auto& layer : layers)
    Put32(room, uint32_t(layer.size()));
  for (const auto& layer : layers)
    room.insert(room.end(), layer.begin(), layer.end());
  return room;
}

} // namespace

int main() {
  SkipStream usa, rev2, pal, usa101;
  CHECK_MSG(PortSkipCutscenes::SkipStreamFor(Version::Usa100, kChapel, usa), "USA 1.00 has the Chapel skip");
  CHECK_MSG(PortSkipCutscenes::SkipStreamFor(Version::Usa101, kChapel, usa101), "USA 1.01 has the Chapel skip");
  CHECK_MSG(PortSkipCutscenes::SkipStreamFor(Version::Usa102, kChapel, rev2), "USA 1.02 has the Chapel skip");
  CHECK_MSG(PortSkipCutscenes::SkipStreamFor(Version::Pal, kChapel, pal), "PAL has the Chapel skip");
  SkipStream trilogy;
  CHECK_MSG(PortSkipCutscenes::SkipStreamFor(Version::Trilogy, kChapel, trilogy) &&
                trilogy.ops == pal.ops && trilogy.size == pal.size,
            "Trilogy uses the PAL stream");
  CHECK_MSG(usa101.ops == usa.ops && usa101.size == usa.size, "1.01 uses the generic stream");
  CHECK_MSG(rev2.ops != usa.ops && rev2.size != usa.size, "1.02 has its own Chapel stream");

  // The override is the generic stream minus exactly the moved remconn.
  const std::vector< Op > base = Split(usa), over = Split(rev2);
  int drops = 0, keeps = 0;
  for (const Op& op : base) {
    drops += IsConn(op, 3, kDropSender, kDropTarget);
    keeps += IsConn(op, 4, kKeepSender, kDropTarget);
  }
  CHECK_MSG(drops == 1, "USA Chapel stream has the moved remconn once");
  CHECK_MSG(keeps == 1, "USA Chapel stream has the addconn 0180 -> 00B2");
  CHECK_MSG(over.size() + 1 == base.size(), "override has one op fewer");
  CHECK_MSG(rev2.size + 17 == usa.size, "override is 17 bytes shorter");
  size_t j = 0;
  bool inOrder = true, kept = false;
  for (const Op& op : base) {
    if (IsConn(op, 3, kDropSender, kDropTarget))
      continue;
    if (j >= over.size() || over[j].len != op.len || std::memcmp(over[j].at, op.at, op.len) != 0)
      inOrder = false;
    kept |= IsConn(op, 4, kKeepSender, kDropTarget);
    ++j;
  }
  CHECK_MSG(inOrder, "every other op is kept, byte for byte and in order");
  CHECK_MSG(kept, "the override keeps addconn 0180");
  for (const Op& op : over)
    CHECK_MSG(!IsConn(op, 3, kDropSender, kDropTarget), "override drops the moved remconn");

  // Replay: a room as 1.00 has it, and as rev2 has it (the connection gone).
  std::vector< uint8_t > out;
  const std::vector< uint8_t > oldRoom = BuildRoom(usa, nullptr);
  CHECK_MSG(PortSkipCutscenes::ApplyOps(oldRoom.data(), oldRoom.size(), usa.ops, usa.size, out) == 0,
            "USA stream applies on a 1.00 Chapel");
  CHECK_MSG(PortSkipCutscenes::ApplyOps(oldRoom.data(), oldRoom.size(), rev2.ops, rev2.size, out) == 0,
            "override also applies where the connection exists");
  const Op* dropped = nullptr;
  for (const Op& op : base)
    if (IsConn(op, 3, kDropSender, kDropTarget))
      dropped = &op;
  const std::vector< uint8_t > newRoom = BuildRoom(usa, dropped);
  CHECK_MSG(PortSkipCutscenes::ApplyOps(newRoom.data(), newRoom.size(), usa.ops, usa.size, out) == 1,
            "USA stream misses once on a rev2 Chapel");
  CHECK_MSG(PortSkipCutscenes::ApplyOps(newRoom.data(), newRoom.size(), rev2.ops, rev2.size, out) == 0,
            "override applies on a rev2 Chapel");

  // Only Chapel differs on 1.02; PAL and the other versions are unchanged.
  // Whole tables: PAL's own rooms, rev2's override, then the USA stream.
  const auto expect = [](const SkipRoom* rooms, size_t n, const uint8_t* ops, uint32_t mrea,
                         const uint8_t*& at, uint32_t& size) {
    for (size_t i = 0; i < n; ++i) {
      if (rooms[i].mrea == mrea) {
        at = ops + rooms[i].offset;
        size = rooms[i].size;
        return true;
      }
    }
    return false;
  };
  size_t checked = 0;
  const Version versions[] = {Version::Usa100, Version::Usa101, Version::Usa102, Version::Pal};
  for (const SkipRoom& usaRoom : kSkipRooms) {
    for (Version v : versions) {
      const uint8_t* at = nullptr;
      uint32_t size = 0;
      bool found = false;
      if (v == Version::Pal)
        found = expect(kSkipRoomsPal, sizeof(kSkipRoomsPal) / sizeof(SkipRoom), kSkipOpsPal,
                       usaRoom.mrea, at, size);
      else if (v == Version::Usa102)
        found = expect(kSkipRoomsRev2, sizeof(kSkipRoomsRev2) / sizeof(SkipRoom), kSkipOpsRev2,
                       usaRoom.mrea, at, size);
      if (!found)
        found = expect(kSkipRooms, sizeof(kSkipRooms) / sizeof(SkipRoom), kSkipOps, usaRoom.mrea,
                       at, size);
      SkipStream got;
      const bool has = PortSkipCutscenes::SkipStreamFor(v, usaRoom.mrea, got);
      CHECK_MSG(has && found && got.size == size && std::memcmp(got.ops, at, size) == 0,
                "selection matches the tables for every room and version");
      ++checked;
    }
  }
  CHECK_MSG(checked == 4 * (sizeof(kSkipRooms) / sizeof(SkipRoom)), "every room was checked");
  // Rooms only PAL has a stream for.
  for (const SkipRoom& palRoom : kSkipRoomsPal) {
    SkipStream got;
    const bool has = PortSkipCutscenes::SkipStreamFor(Version::Pal, palRoom.mrea, got);
    const bool other = PortSkipCutscenes::SkipStreamFor(Version::Usa100, palRoom.mrea, got);
    CHECK_MSG(palRoom.mrea == 0xFFFFFFFF ? !has && !other : has, "PAL-only rooms resolve on PAL");
  }

  const uint32_t others[] = {0xB2701146 /* Landing Site */, 0xFB54A0CB /* Hall of the Elders */,
                             0x18AB6106 /* Arboretum */, 0x2398E906 /* Artifact Temple */};
  for (uint32_t mrea : others) {
    SkipStream a, b;
    const bool ha = PortSkipCutscenes::SkipStreamFor(Version::Usa100, mrea, a);
    const bool hb = PortSkipCutscenes::SkipStreamFor(Version::Usa102, mrea, b);
    CHECK_MSG(ha == hb && (!ha || (a.ops == b.ops && a.size == b.size)), "other rooms share the USA stream");
  }
  SkipStream none;
  CHECK_MSG(!PortSkipCutscenes::SkipStreamFor(Version::Usa102, 0xDEADBEEF, none), "unknown room has no skip");
  CHECK_MSG(PortSkipCutscenes::LandingStream().size > 0, "landing ops present");
  CHECK_MSG(pal.ops != rev2.ops, "PAL does not take the 1.02 override");

  if (port_test::Failures() == 0)
    std::puts("[skip-streams] OK");
  return PORT_TEST_RESULT();
}
