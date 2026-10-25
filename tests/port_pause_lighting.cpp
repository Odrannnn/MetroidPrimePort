#include "port_pause_lighting.h"
#include "port_test.h"

int main() {
  using namespace PortPauseLighting;
  const float identity[3][4] = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}};
  const float rotate[3][4] = {{0,-1,0,40}, {1,0,0,80}, {0,0,1,120}};
  float ordinary[48], phazonLights[48], rotated[48];
  Lights(identity, false, ordinary);
  Lights(identity, true, phazonLights);
  Lights(rotate, false, rotated);
  for (int i = 0; i < 3; ++i) {
    float norm = 0;
    for (int c = 0; c < 3; ++c) {
      norm += ordinary[i*16+8+c] * ordinary[i*16+8+c];
      CHECK(std::fabs(phazonLights[i*16+4+c] - ordinary[i*16+4+c] * (i == 0 ? 0.8f : 1.f)) < 1e-6f);
    }
    CHECK(std::fabs(norm - 1.f) < 1e-6f);
    CHECK_EQ(rotated[i*16+8], -ordinary[i*16+9]);
    CHECK_EQ(rotated[i*16+9], ordinary[i*16+8]);
    CHECK_EQ(rotated[i*16+10], ordinary[i*16+10]);
    CHECK_EQ(ordinary[i*16+14], 2.f);
  }
  CHECK(ordinary[20] > 3.f); // HDR orange red must not clamp to 1.
  CHECK(ordinary[21] < ordinary[20] && ordinary[22] < ordinary[21]);
  float tints[5][3];
  Bloom(tints);
  CHECK_EQ(tints[0][0], FloatBits(0x3ed0d0d1));
  CHECK_EQ(tints[4][2], FloatBits(0x3f05c3f9));
  CHECK(!active);
  { Scope outer(active); CHECK(active); { Scope inner(active, false); CHECK(!active); } CHECK(active); }
  CHECK(!active);
  std::vector<int> order;
  {
    TextPass text(true);
    TextPass::Queue([&] { CHECK(!TextPass::Deferring()); order.push_back(3); });
    TextPass::Queue([&] { order.push_back(4); });
    order.push_back(1); // post
    order.push_back(2); // helmet
    CHECK_EQ(order.size(), 2u);
    { TextPass retail(false); CHECK(!TextPass::Deferring()); }
    CHECK(TextPass::Deferring());
    text.Flush();
    CHECK(!TextPass::Deferring());
    CHECK((order == std::vector<int>{1, 2, 3, 4}));
    text.Flush();
    CHECK_EQ(order.size(), 4u); // replay exactly once
  }
  CHECK(!TextPass::Deferring());
  return PORT_TEST_RESULT();
}
