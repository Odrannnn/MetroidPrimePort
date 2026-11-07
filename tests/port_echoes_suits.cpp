// The Echoes suits conversion (port_echoes_suits.h) over hand-built fixtures: LZO streams, PAK
// entry segments, the CINF reader and the skin's joint mapping.

#include "port_echoes_suits.h"
#include "port_dread_samus_mod.h"
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

// Every body the import writes is one the loader counts as a body (port_echoes_suits_mod.h), so the
// game leaves out Prime's cannon, boots and visor for it; and every body the loader counts is written.
// The arms are not bodies.
void TestBodies() {
  size_t written = 0;
  for (const Target& t : kTargets) {
    const bool arm = std::string(t.skeleton) == "grapple";
    if (std::string(t.suit) == "Dark Samus") {
      CHECK(!IsBody(t.cmdl) && t.cmdl == DarkSamusId(t.cinf) && t.cskr == DarkSamusSkinId(t.cinf));
      continue;
    }
    CHECK(IsBody(t.cmdl) == !arm);
    written += arm ? 0 : 1;
  }
  // Dark Samus is written on each skeleton kDarkBodies names, and covers every body the mod writes.
  for (const DarkBody& b : kDarkBodies) {
    bool converted = false;
    for (const Target& t : kTargets) converted |= std::string(t.suit) == "Dark Samus" && t.cinf == b.cinf;
    CHECK(converted);
  }
  for (const uint32_t body : kBodies) {
    bool dark = false;
    for (const DarkBody& b : kDarkBodies) dark |= b.cmdl == body;
    CHECK(dark);
  }
  for (const Arm& a : kArms) {
    CHECK(!IsBody(a.cmdl));
  }
  for (const Copy& c : kCopies) {
    if (c.type == 0x434D444C) {
      CHECK(IsBody(c.to));
      ++written;
    }
  }
  CHECK(written == sizeof(kBodies) / sizeof(kBodies[0]));
  // Dark Samus's ball ids: none a body, a suit's ball, or another of hers.
  for (const auto& t : PortDreadSamus::kBallTargets) {
    for (const uint32_t id : {DarkBallId(t.ancs), DarkBallId(t.cmdl), DarkBallSkinId(t.cmdl), DarkBallId(t.low)}) {
      CHECK(!IsBody(id));
      for (int suit = 0; suit < 4; ++suit) {
        CHECK(id != PortModelVariant::Id(t.ancs, suit) && id != PortModelVariant::Id(t.cmdl, suit) &&
              id != PortModelVariant::Id(t.low, suit));
      }
    }
    CHECK(DarkBallId(t.cmdl) != DarkBallSkinId(t.cmdl) && DarkBallId(t.ancs) != DarkBallId(t.low));
  }
}

// A ball-shaped model: short normals and UVs, one material on texture `texture` with 8-bit
// position, normal and texture 0 indices, one triangle over two vertices.
PrimeCmdl BallFixture(uint32_t texture, float extent) {
  PrimeCmdl m;
  Put32(m.header, 0xDEADBABE);
  Put32(m.header, 2);
  Put32(m.header, 0x6);
  for (int i = 0; i < 3; ++i) PutF(m.header, -extent);
  for (int i = 0; i < 3; ++i) PutF(m.header, extent);
  Put32(m.header, 0);
  Put32(m.header, 0);
  std::vector<uint8_t> set;
  Put32(set, 1);
  Put32(set, texture);
  Put32(set, 1);
  Put32(set, 16);  // the material's end
  Put32(set, 0);   // flags
  Put32(set, 1);   // textures
  Put32(set, 0);   // texture index
  Put32(set, 0x20A);
  set.resize(64, 0);
  m.materialSets.push_back(set);
  std::vector<uint8_t> pos, nrm, uv;
  for (float v : {0.f, 0.f, 0.f, 1.f, 1.f, 1.f}) PutF(pos, v);
  for (uint16_t v : {16384, 0, 0, 0, 0xC000, 0}) {
    nrm.push_back(uint8_t(v >> 8));
    nrm.push_back(uint8_t(v));
  }
  for (float v : {0.f, 0.f, 1.f, 1.f}) PutF(uv, v);
  for (auto* a : {&pos, &nrm, &uv}) a->resize(32, 0);
  m.arrays = {pos, nrm, {}, uv, {}};
  std::vector<uint8_t> surface(0x40, 0);
  surface.insert(surface.end(), {0x91, 0x00, 0x03, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0});
  surface.resize(0x60, 0);
  surface[0x13] = 0x20;  // list size
  m.surfaces.push_back(surface);
  return m;
}

// The Morph Ball steps: a merge, float normals, a rigid skin and the retail ball's main joint.
void TestBall() {
  std::string error;
  PrimeCmdl a = BallFixture(0x11111111, 0.5f);
  const PrimeCmdl b = BallFixture(0x22222222, 0.75f);
  REQUIRE_MSG(MergeCmdl(a, b, &error), error);
  CHECK_EQ(Get32(a.header, 8), 0x2u);  // short UVs dropped
  CHECK_EQ(a.arrays.size(), size_t(4));
  CHECK_EQ(a.surfaces.size(), size_t(2));
  float lo;
  const uint32_t loBits = Get32(a.header, 0x0C);
  std::memcpy(&lo, &loBits, 4);
  CHECK(lo == -0.75f);
  // b's list: material 1, 16-bit indices past a's 2 positions, 5 normals and 4 UVs.
  const std::vector<uint8_t>& merged = a.surfaces[1];
  CHECK_EQ(Get32(merged, 0x0C), 1u);
  const std::vector<uint8_t> list(merged.begin() + 0x40, merged.begin() + 0x40 + 3 + 9 * 2 + 1);
  CHECK(list == std::vector<uint8_t>({0x91, 0, 3, 0, 2, 0, 5, 0, 4, 0, 3, 0, 6, 0, 5, 0, 3, 0, 5, 0, 5, 0}));
  CHECK_EQ(Get32(a.materialSets[0], 0), 2u);
  CHECK_EQ(Get32(a.materialSets[0], 8), 0x22222222u);
  CHECK_EQ(Get32(a.materialSets[0], 12), 2u);  // materials
  CHECK_EQ(Get32(a.materialSets[0], 24 + 16 + 8), 1u);  // b's texture index
  CHECK_EQ(Get32(a.materialSets[0], 24 + 16 + 12), 0x30Fu);

  std::vector<uint8_t> cskr;
  CHECK(!RigidSkin(a, 4, cskr, &error));  // short normals
  REQUIRE_MSG(FloatNormals(a, &error), error);
  CHECK_EQ(Get32(a.header, 8), 0x0u);
  CHECK_EQ(a.surfaces[0][0x40], 0x90);
  CHECK_EQ(a.surfaces[1][0x40], 0x90);
  CHECK_EQ(Get32(a.arrays[1], 0), 0x3F800000u);
  CHECK_EQ(Get32(a.arrays[1], 16), 0xBF800000u);
  CHECK_EQ(a.arrays[1].size() % 12, size_t(8));  // whole normals, padded to 32
  REQUIRE_MSG(RigidSkin(a, 4, cskr, &error), error);
  CHECK_EQ(Get32(a.header, 8), 0x1u);
  const uint32_t count = Get32(cskr, 16);
  CHECK_EQ(count % 8, 0u);
  CHECK(a.arrays[0].size() == count * 12 && a.arrays[1].size() == count * 12);
  CHECK(count * 12 >= 128);
  CHECK(cskr == [&] {
    std::vector<uint8_t> want;
    for (uint32_t v : {1u, 1u, 4u, 0x3F800000u, count, 0xFFFFFFFFu, count, 0xFFFFFFFFu, count}) Put32(want, v);
    return want;
  }());
  const std::vector<uint8_t> out = WriteCmdl(a);
  CHECK_EQ(Get32(out, 0x24), 8u);  // set, 4 arrays, offsets, 2 surfaces
  CHECK_EQ(Get32(out, 0x28), 1u);

  // Weight times vertices: joint 4 (0.75 x 10) over joint 5 (1 x 1 + 0.25 x 10), though 5 has
  // more weight summed over the groups alone.
  std::vector<uint8_t> retail;
  Put32(retail, 2);
  Put32(retail, 1);
  Put32(retail, 5);
  PutF(retail, 1.f);
  Put32(retail, 1);
  Put32(retail, 2);
  Put32(retail, 4);
  PutF(retail, 0.75f);
  Put32(retail, 5);
  PutF(retail, 0.25f);
  Put32(retail, 10);
  uint32_t joint = 0;
  REQUIRE_MSG(MainJoint(retail, joint, &error), error);
  CHECK_EQ(joint, 4u);
  retail.pop_back();
  CHECK(!MainJoint(retail, joint, &error));
}

// One material set kept of several: the arm's, with that set's textures only.
void TestKeepMaterialSet() {
  std::string error;
  PrimeCmdl m = BallFixture(0x11111111, 0.5f);
  m.materialSets.push_back(BallFixture(0x22222222, 0.5f).materialSets[0]);
  m.materialSets.push_back(BallFixture(0x33333333, 0.5f).materialSets[0]);
  std::vector<uint32_t> textures;
  CHECK(!KeepMaterialSet(m, 3, textures, &error));
  REQUIRE_MSG(KeepMaterialSet(m, 1, textures, &error), error);
  CHECK_EQ(m.materialSets.size(), size_t(1));
  CHECK(textures == std::vector<uint32_t>({0x22222222}));
  CHECK_EQ(Get32(m.materialSets[0], 4), 0x22222222u);
  CHECK_EQ(Get32(WriteCmdl(m), 0x28), 1u);
}

// A beam ANCS's gun and hologram characters (unaligned, as in Prime's) pointed at the variants.
void TestGunAncs() {
  std::string error;
  std::vector<uint8_t> ancs = {0x00, 0x01, 0x02};
  const auto put = [&ancs](const Gun& gun) {
    for (const uint32_t v : {gun.cmdl, gun.cskr, kGunCinf, 0x12345678u}) Put32(ancs, v);
  };
  put(kGuns[0]);
  ancs.push_back(0x7F);
  put(kGuns[1]);
  std::vector<uint8_t> patched = ancs;
  REQUIRE_MSG(PatchGunAncs(patched, &error), error);
  CHECK_EQ(patched.size(), ancs.size());
  CHECK_EQ(Get32(patched, 3), Cannon(kGuns[0].cmdl));
  CHECK_EQ(Get32(patched, 7), Cannon(kGuns[0].cskr));
  CHECK_EQ(Get32(patched, 11), kGunCinf);
  CHECK_EQ(Get32(patched, 15), 0x12345678u);
  CHECK_EQ(Get32(patched, 20), Cannon(kGuns[1].cmdl));
  CHECK_EQ(Get32(patched, 24), Cannon(kGuns[1].cskr));
  // Patched again, nothing is left to find.
  CHECK(!PatchGunAncs(patched, &error));
  // The gun twice: which one is the character's is unknown.
  put(kGuns[0]);
  CHECK(!PatchGunAncs(ancs, &error));
  // Every variant id is new.
  std::vector<uint32_t> ids;
  for (const Gun& gun : kGuns) {
    ids.insert(ids.end(), {gun.cmdl, gun.cskr, Cannon(gun.cmdl), Cannon(gun.cskr)});
  }
  for (const uint32_t a : kBeamAncs) ids.insert(ids.end(), {a, Cannon(a)});
  for (size_t i = 0; i < ids.size(); ++i) {
    for (size_t j = i + 1; j < ids.size(); ++j) CHECK(ids[i] != ids[j]);
  }
}

}  // namespace

int main() {
  TestLzo();
  TestDecompress();
  TestSkin();
  TestBodies();
  TestBall();
  TestKeepMaterialSet();
  TestGunAncs();
  return PORT_TEST_RESULT();
}
