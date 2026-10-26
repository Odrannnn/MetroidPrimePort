#include "port_skip_streams.h"

#include <algorithm>
#include <iterator>

namespace PortSkipCutscenes {
namespace {

struct SkipRoom {
  uint32_t mrea;
  uint32_t offset;
  uint32_t size;
};

#include "port_skip_cutscenes_data.inc"

template < size_t N >
const SkipRoom* FindRoom(const SkipRoom (&rooms)[N], uint32_t mreaId) {
  const SkipRoom* end = rooms + N;
  const SkipRoom* room = std::lower_bound(
      rooms, end, mreaId, [](const SkipRoom& r, uint32_t id) { return r.mrea < id; });
  return room != end && room->mrea == mreaId ? room : nullptr;
}

} // namespace

bool SkipStreamFor(PortDisc::Version version, uint32_t mreaId, SkipStream& out) {
  // PAL and USA 1.02 lay some rooms out differently: their own streams, where
  // the USA one doesn't do the same there. Trilogy's rooms are PAL's.
  const SkipRoom* room = nullptr;
  const uint8_t* ops = kSkipOps;
  if (version == PortDisc::Version::Pal || version == PortDisc::Version::Trilogy) {
    room = FindRoom(kSkipRoomsPal, mreaId);
    ops = kSkipOpsPal;
  } else if (version == PortDisc::Version::Usa102) {
    room = FindRoom(kSkipRoomsRev2, mreaId);
    ops = kSkipOpsRev2;
  }
  if (room == nullptr) {
    room = FindRoom(kSkipRooms, mreaId);
    ops = kSkipOps;
  }
  if (room == nullptr)
    return false;
  out.ops = ops + room->offset;
  out.size = room->size;
  return true;
}

SkipStream LandingStream() { return {kLandingOps, sizeof(kLandingOps)}; }

} // namespace PortSkipCutscenes
