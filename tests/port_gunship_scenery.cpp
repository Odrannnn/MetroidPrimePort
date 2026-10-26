#include "port_gunship_scenery.h"
#include "port_test.h"

#include <cstddef>
#include <utility>

namespace {
// Known source assets, independent of the policy constants. Keep fixture UUIDs in the source
// inspection's spelling so a typo in an exclusion constant cannot make the positive test tautological.
template<std::size_t N>
constexpr std::array<uint8_t, 16> SourceUuid(const char (&text)[N]) {
  static_assert(N == 33);
  std::array<uint8_t, 16> result{};
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = uint8_t((nibble(text[2 * i]) << 4) | nibble(text[2 * i + 1]));
  }
  return result;
}
} // namespace

int main() {
  using namespace PortGunshipScenery;
  constexpr auto sourceHull = SourceUuid("A4DE141BEA3B44AD812346E45C649B06");
  constexpr auto sourceUnlit = SourceUuid("F318ACF83D2B461B8AF7A66E61DBB4D0");
  static_assert(sourceHull == kHull && sourceUnlit == kUnlitHull);
  static_assert(kLandingSite == 0xB2701146);
  for (const auto& model : {sourceHull, sourceUnlit}) {
    CHECK(ExcludeSource(kLandingSite, model));
    CHECK(!ExcludeSource(kLandingSite, model, true));
    // Frigate and ending cinematics, adjacent room IDs and unrecognised rooms stay eligible.
    for (uint32_t room : {0u, 0xD1241219u, 0xB4B41C48u, 0xB2701147u, 0xFFFFFFFFu}) {
      CHECK(!ExcludeSource(room, model));
    }
    for (std::size_t byte = 0; byte < model.size(); ++byte) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto other = model;
        other[byte] ^= 1u << bit;
        CHECK(!ExcludeSource(kLandingSite, other));
      }
    }
    // Passing the mixed-endian ROOM-property bytes instead of the pak UUID must not match.
    auto property = model;
    std::swap(property[0], property[3]);
    std::swap(property[1], property[2]);
    std::swap(property[4], property[5]);
    std::swap(property[6], property[7]);
    CHECK(!ExcludeSource(kLandingSite, property));
  }
  // Retail's actual animated hull and parked actor are not replacement scenery.
  for (uint32_t id : {0x578C5D12u, 0x1C0B0BEFu}) {
    std::array<uint8_t, 16> retail = {
        0x10, 0, 0, 0, 0, 0, 0xF0, 0, 0xF0, 0, 0, 0,
        uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
    CHECK(!ExcludeSource(kLandingSite, retail));
  }
  CHECK(!ExcludeSource(kLandingSite, {}));
  return PORT_TEST_RESULT();
}
