#include "port_shell_file.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
// An image whose chain stops at 1x1, every level filled with a pattern of its own.
PortShell::Image Make(uint32_t w, uint32_t h, bool srgb, uint8_t seed) {
  PortShell::Image image;
  image.width = w;
  image.height = h;
  image.srgb = srgb;
  image.mips = 0;
  for (uint32_t side = std::max(w, h); side != 0; side >>= 1) {
    ++image.mips;
  }
  for (uint32_t mip = 0; mip < image.mips; ++mip) {
    const size_t texels = size_t(std::max(w >> mip, 1u)) * std::max(h >> mip, 1u);
    for (size_t i = 0; i < texels * 4; ++i) {
      image.rgba.push_back(uint8_t(i * 7 + mip * 3 + seed));
    }
  }
  return image;
}

PortShell::Assets Make() {
  PortShell::Assets assets;
  assets.image[PortShell::kStrength] = Make(64, 32, true, 1);
  assets.image[PortShell::kIndirectBody] = Make(32, 32, false, 2);
  assets.image[PortShell::kIndirectDoll] = Make(8, 1, false, 3);
  return assets;
}

void TestRoundTrip() {
  const PortShell::Assets assets = Make();
  const std::vector<uint8_t> file = PortShell::Serialize(assets);
  PortShell::Assets back;
  std::string error;
  CHECK_MSG(PortShell::Parse(file.data(), file.size(), back, error), error);
  for (int i = 0; i < PortShell::kCount; ++i) {
    CHECK_EQ(back.image[i].width, assets.image[i].width);
    CHECK_EQ(back.image[i].height, assets.image[i].height);
    CHECK_EQ(back.image[i].mips, assets.image[i].mips);
    CHECK_EQ(back.image[i].srgb, assets.image[i].srgb);
    CHECK(back.image[i].rgba == assets.image[i].rgba);
  }
}

void TestRefusals() {
  const std::vector<uint8_t> file = PortShell::Serialize(Make());
  PortShell::Assets out;
  std::string error;
  CHECK_MSG(!PortShell::Parse(file.data(), file.size() - 1, out, error), "a truncated file is refused");
  std::vector<uint8_t> trailing = file;
  trailing.push_back(0);
  CHECK_MSG(!PortShell::Parse(trailing.data(), trailing.size(), out, error), "trailing bytes are refused");
  std::vector<uint8_t> magic = file;
  magic[0] ^= 1;
  CHECK_MSG(!PortShell::Parse(magic.data(), magic.size(), out, error), "a wrong magic is refused");
  std::vector<uint8_t> version = file;
  version[4] = 2;
  CHECK_MSG(!PortShell::Parse(version.data(), version.size(), out, error), "a wrong version is refused");
  // The first image's mip count field: 12 bytes of header, then width, height, mips.
  std::vector<uint8_t> mips = file;
  mips[12 + 8] = 2;
  mips[12 + 9] = 0;
  mips[12 + 10] = 0;
  mips[12 + 11] = 0;
  CHECK_MSG(!PortShell::Parse(mips.data(), mips.size(), out, error), "a short mip chain is refused");
  std::vector<uint8_t> side = file;
  side[12] = 0;
  side[13] = 0;
  side[14] = 0;
  side[15] = 0;
  CHECK_MSG(!PortShell::Parse(side.data(), side.size(), out, error), "a zero width is refused");
  CHECK_MSG(!PortShell::Parse(nullptr, 0, out, error), "no data is refused");
}
}  // namespace

int main() {
  TestRoundTrip();
  TestRefusals();
  if (port_test::Failures() == 0) {
    std::printf("port_shell_file: all tests passed\n");
  }
  return PORT_TEST_RESULT();
}
