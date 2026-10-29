#include "port_icy_file.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
// An image whose chain stops at 1x1, every level filled with a pattern of its own.
PortIcy::Image Make(uint32_t w, uint32_t h, bool srgb, uint8_t seed) {
  PortIcy::Image image;
  image.width = w;
  image.height = h;
  image.srgb = srgb;
  for (uint32_t side = std::max(w, h); side != 0; side >>= 1) {
    ++image.mips;
  }
  for (uint32_t mip = 0; mip < image.mips; ++mip) {
    const size_t texels = size_t(std::max(w >> mip, 1u)) * std::max(h >> mip, 1u);
    for (size_t i = 0; i < texels * 4; ++i) {
      image.rgba.push_back(uint8_t(i * 5 + mip * 3 + seed));
    }
  }
  return image;
}

PortIcy::Assets Make() {
  PortIcy::Assets assets;
  assets.image[PortIcy::kNormal] = Make(16, 16, false, 1);
  assets.image[PortIcy::kMetal] = Make(8, 4, false, 2);
  assets.image[PortIcy::kColor] = Make(32, 16, true, 3);
  // The shipped material's constants (ba70a559).
  assets.vector[PortIcy::kCch0] = {0.1f, 1.f, 1.f, -0.06f};
  assets.vector[PortIcy::kCch1] = {1.f, 1.f, 1.f, 1.f};
  return assets;
}

void TestRoundTrip() {
  const PortIcy::Assets assets = Make();
  const std::vector<uint8_t> file = PortIcy::Serialize(assets);
  PortIcy::Assets back;
  std::string error;
  CHECK_MSG(PortIcy::Parse(file.data(), file.size(), back, error), error);
  for (int i = 0; i < PortIcy::kCount; ++i) {
    CHECK_EQ(back.image[i].width, assets.image[i].width);
    CHECK_EQ(back.image[i].height, assets.image[i].height);
    CHECK_EQ(back.image[i].mips, assets.image[i].mips);
    CHECK_EQ(back.image[i].srgb, assets.image[i].srgb);
    CHECK(back.image[i].rgba == assets.image[i].rgba);
  }
  for (int i = 0; i < PortIcy::kVectors; ++i) {
    CHECK(back.vector[i] == assets.vector[i]);
  }
}

void TestRefusals() {
  const std::vector<uint8_t> file = PortIcy::Serialize(Make());
  PortIcy::Assets out;
  std::string error;
  CHECK_MSG(!PortIcy::Parse(file.data(), file.size() - 1, out, error), "a truncated file is refused");
  std::vector<uint8_t> trailing = file;
  trailing.push_back(0);
  CHECK_MSG(!PortIcy::Parse(trailing.data(), trailing.size(), out, error), "trailing bytes are refused");
  std::vector<uint8_t> magic = file;
  magic[0] ^= 1;
  CHECK_MSG(!PortIcy::Parse(magic.data(), magic.size(), out, error), "a wrong magic is refused");
  std::vector<uint8_t> version = file;
  version[4] = 2;
  CHECK_MSG(!PortIcy::Parse(version.data(), version.size(), out, error), "a wrong version is refused");
  // The first image's mip count field: 12 bytes of header, then width, height, mips.
  std::vector<uint8_t> mips = file;
  std::memset(mips.data() + 12 + 8, 0, 4);
  mips[12 + 8] = 2;
  CHECK_MSG(!PortIcy::Parse(mips.data(), mips.size(), out, error), "a short mip chain is refused");
  std::vector<uint8_t> nan = file;
  const float bad = std::nanf("");
  std::memcpy(nan.data() + nan.size() - 4, &bad, 4);
  CHECK_MSG(!PortIcy::Parse(nan.data(), nan.size(), out, error), "a constant that is not finite is refused");
  CHECK_MSG(!PortIcy::Parse(nullptr, 0, out, error), "no data is refused");
}
}  // namespace

int main() {
  TestRoundTrip();
  TestRefusals();
  if (port_test::Failures() == 0) {
    std::printf("port_icy_file: all tests passed\n");
  }
  return PORT_TEST_RESULT();
}
