// The Echoes suits conversion (port_echoes_suits.h) over hand-built fixtures: LZO streams, PAK
// entry segments, the CINF reader and the skin's joint mapping.

#include "port_echoes_suits.h"
#include "port_test.h"

#include <cstring>
#include <string>
#include <vector>

using namespace PortEchoesSuits;

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}
void PutF(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Put32(out, bits);
}
void PutName(std::vector<uint8_t>& out, const char* name, uint32_t id) {
  out.insert(out.end(), name, name + std::strlen(name) + 1);
  Put32(out, id);
}
uint32_t Get32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
}

// "abcd" as a first-byte literal run, an M2 match of 8 bytes 4 back, the end marker.
const std::vector<uint8_t> kStream = {0x15, 'a', 'b', 'c', 'd', 0xEC, 0x00, 0x11, 0x00, 0x00};

void TestLzo() {
  uint8_t out[32] = {};
  size_t len = sizeof(out);
  CHECK(LzoInflate(kStream.data(), kStream.size(), out, len));
  CHECK_EQ(len, size_t(12));
  CHECK(std::memcmp(out, "abcdabcdabcd", 12) == 0);
  // Too little room: stops, reports what fits.
  len = 6;
  CHECK(!LzoInflate(kStream.data(), kStream.size(), out, len));
  CHECK_EQ(len, size_t(4));
  // A match reaching before the start.
  const std::vector<uint8_t> bad = {0x15, 'a', 'b', 'c', 'd', 0xEC, 0x10, 0x11, 0x00, 0x00};
  len = sizeof(out);
  CHECK(!LzoInflate(bad.data(), bad.size(), out, len));
  // Cut inside an instruction (input that ends between instructions decodes what it has).
  for (size_t cut : {size_t(1), size_t(3), size_t(6), size_t(8), size_t(9)}) {
    len = sizeof(out);
    CHECK_MSG(!LzoInflate(kStream.data(), cut, out, len), "cut at " + std::to_string(cut));
  }
}

void TestDecompress() {
  // A stored segment, then an LZO one: 3 + 12 bytes.
  std::vector<uint8_t> entry;
  Put32(entry, 15);
  entry.push_back(0xFF);
  entry.push_back(0xFD);  // -3: stored
  entry.insert(entry.end(), {'x', 'y', 'z'});
  entry.push_back(0x00);
  entry.push_back(uint8_t(kStream.size()));
  entry.insert(entry.end(), kStream.begin(), kStream.end());
  std::vector<uint8_t> out;
  std::string error;
  REQUIRE_MSG(Decompress(entry.data(), entry.size(), out, &error), error);
  CHECK(std::string(out.begin(), out.end()) == "xyzabcdabcdabcd");
  // A size the segments do not fill, with and without an empty segment after them.
  std::vector<uint8_t> longer = entry;
  longer[3] = 16;
  CHECK(!Decompress(longer.data(), longer.size(), out, &error));
  longer.insert(longer.end(), {0, 0});
  CHECK(!Decompress(longer.data(), longer.size(), out, &error));
  // Cut short.
  for (size_t cut : {size_t(2), size_t(5), size_t(8), size_t(12)}) {
    std::vector<uint8_t> e(entry.begin(), entry.begin() + std::ptrdiff_t(cut));
    CHECK_MSG(!Decompress(e.data(), e.size(), out, &error), "cut at " + std::to_string(cut));
  }
}

struct J {
  uint32_t id, parent;
  const char* name;
};

std::vector<uint8_t> EchoesCinf(const std::vector<J>& joints) {
  std::vector<uint8_t> out;
  Put32(out, uint32_t(joints.size()));
  for (const J& j : joints) {
    Put32(out, j.id);
    Put32(out, j.parent);
    for (int i = 0; i < 11; ++i) PutF(out, 0.f);
    Put32(out, 1);
    Put32(out, j.parent);
  }
  Put32(out, uint32_t(joints.size()));
  for (const J& j : joints) Put32(out, j.id);
  Put32(out, uint32_t(joints.size()));
  for (const J& j : joints) PutName(out, j.name, j.id);
  return out;
}

std::vector<uint8_t> PrimeCinf(const std::vector<J>& joints) {
  std::vector<uint8_t> out;
  Put32(out, uint32_t(joints.size()));
  for (const J& j : joints) {
    Put32(out, j.id);
    Put32(out, j.parent);
    for (int i = 0; i < 3; ++i) PutF(out, 0.f);
    Put32(out, 0);
  }
  Put32(out, uint32_t(joints.size()));
  for (const J& j : joints) Put32(out, j.id);
  Put32(out, uint32_t(joints.size()));
  for (const J& j : joints) PutName(out, j.name, j.id);
  return out;
}

void TestSkin() {
  // Echoes: root 1, spine 2, a twist joint 3 Prime lacks, its child 4, the Varia pad 5.
  const std::vector<uint8_t> echoes =
      EchoesCinf({{1, 0, "root"}, {2, 1, "spine"}, {3, 2, "twist"}, {4, 3, "twist_end"}, {5, 2, "L_varia_SDK"}});
  std::vector<EchoesJoint> from;
  std::string error;
  REQUIRE_MSG(ParseCinf(echoes.data(), echoes.size(), from, &error), error);
  REQUIRE(from.size() == 5);
  CHECK(from[2].name == "twist");
  CHECK_EQ(from[3].parent, 3u);
  for (size_t cut : {size_t(0), size_t(10), echoes.size() / 2, echoes.size() - 1}) {
    std::vector<EchoesJoint> t;
    CHECK_MSG(!ParseCinf(echoes.data(), cut, t, &error), "cut at " + std::to_string(cut));
  }

  const std::vector<uint8_t> prime = PrimeCinf({{10, 0, "root"}, {20, 10, "spine"}, {30, 20, "L_varias2_SDK"}});
  PortDread::PrimeSkeleton to;
  REQUIRE_MSG(PortDread::ParseCinf(prime.data(), prime.size(), to, &error), error);

  // Group 0: twist (0.25) and twist_end (0.25) fold into spine with spine's own 0.5; group 1: the pad.
  std::vector<uint8_t> cskr;
  Put32(cskr, 2);
  Put32(cskr, 3);
  Put32(cskr, 3);
  PutF(cskr, 0.25f);
  Put32(cskr, 2);
  PutF(cskr, 0.5f);
  Put32(cskr, 4);
  PutF(cskr, 0.25f);
  Put32(cskr, 7);
  Put32(cskr, 1);
  Put32(cskr, 5);
  PutF(cskr, 1.f);
  Put32(cskr, 2);
  Put32(cskr, 0xDEADBEEF);  // Echoes' footer: dropped

  std::vector<uint8_t> out;
  std::vector<std::string> folded;
  REQUIRE_MSG(ConvertCskr(cskr, from, to, out, &folded, &error), error);
  REQUIRE(out.size() == 4 + (4 + 8 + 4) * 2 + 16);
  CHECK_EQ(Get32(out, 0), 2u);
  CHECK_EQ(Get32(out, 4), 1u);
  CHECK_EQ(Get32(out, 8), 20u);
  CHECK_EQ(Get32(out, 12), 0x3F800000u);  // 1.0
  CHECK_EQ(Get32(out, 16), 7u);
  CHECK_EQ(Get32(out, 24), 30u);
  CHECK_EQ(Get32(out, 32), 2u);
  CHECK_EQ(Get32(out, 36), 0xFFFFFFFFu);
  CHECK_EQ(Get32(out, 40), 9u);
  CHECK_EQ(Get32(out, 48), 9u);
  REQUIRE(folded.size() == 2);
  CHECK(folded[0] == "twist->spine");
  CHECK(folded[1] == "twist_end->spine");

  // A weight on a joint with no named ancestor in Prime fails.
  std::vector<uint8_t> orphan;
  Put32(orphan, 1);
  Put32(orphan, 1);
  Put32(orphan, 99);
  PutF(orphan, 1.f);
  Put32(orphan, 1);
  CHECK(!ConvertCskr(orphan, from, to, out, nullptr, &error));
}

}  // namespace

int main() {
  TestLzo();
  TestDecompress();
  TestSkin();
  return PORT_TEST_RESULT();
}
