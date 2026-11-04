#include "port_pause_lighting.h"
#include "port_test.h"

int main() {
  using namespace PortPauseLighting;
  const float identity[3][4] = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}};
  const float rotate[3][4] = {{0,-1,0,40}, {1,0,0,80}, {0,0,1,120}};
  float lights[32], rotated[32];
  auto Order = [](std::vector<int> want) {
    return std::vector<int>(order, order + orderCount) == want;
  };
  // Lights start inactive: nothing reaches the doll.
  CHECK_EQ(Lights(identity, lights), 0);
  // Inventory (relay #97): #102, #100, #101 in that order; #101 (orange) is third.
  EnterScreen(true, false);
  CHECK(Order({2, 0, 1}));
  CHECK_EQ(Lights(identity, lights), 2);
  CHECK(std::fabs(lights[4] - FloatBits(0x3f1a946e) * 0.25f / 3.14159265f) < 1e-6f);
  CHECK(std::fabs(lights[20] - 2.5f / 3.14159265f) < 1e-6f);
  // Re-entering keeps the order; Phazon (relay #99) then drops #100, appends #103.
  EnterScreen(true, false);
  CHECK(Order({2, 0, 1}));
  EnterScreen(true, true);
  CHECK(Order({2, 1, 3}));
  CHECK_EQ(Lights(identity, lights), 2);
  CHECK(lights[20] > 3.f); // HDR orange red must not clamp to 1.
  CHECK(lights[21] < lights[20] && lights[22] < lights[21]);
  // Log book/options (#96/#98) turn them all off; Phazon from scratch.
  EnterScreen(false, true);
  CHECK(Order({}));
  EnterScreen(true, true);
  CHECK(Order({2, 3, 1}));
  CHECK_EQ(Lights(identity, lights), 2);
  CHECK(std::fabs(lights[20] - 2.f / 3.14159265f) < 1e-6f);
  // Back to normal without leaving the inventory: #100 appends after #101.
  EnterScreen(true, false);
  CHECK(Order({2, 1, 0}));
  // All four records: unit directions, view rotation applied.
  for (int l = 0; l < 4; ++l) {
    orderCount = 1, order[0] = (unsigned char)l;
    Lights(identity, lights);
    Lights(rotate, rotated);
    float norm = 0;
    for (int c = 0; c < 3; ++c) norm += lights[8+c] * lights[8+c];
    CHECK(std::fabs(norm - 1.f) < 1e-6f);
    CHECK_EQ(rotated[8], -lights[9]);
    CHECK_EQ(rotated[9], lights[8]);
    CHECK_EQ(rotated[10], lights[10]);
    CHECK_EQ(lights[14], 2.f);
    CHECK_EQ(lights[16+14], 0.f);
    // Native UI camera rotation is diag(-1,+1,-1); native view rows cancel
    // those signs. Converting the port-world directions back gives -local +Z.
    float uiView[3][4], uiLights[32];
    LightView(uiView);
    Lights(uiView, uiLights);
    CHECK(std::fabs(uiLights[8] + lights[8]) < 1e-6f);
    CHECK(std::fabs(uiLights[9] - lights[10]) < 1e-6f);
    CHECK(std::fabs(uiLights[10] - lights[9]) < 1e-6f);
  }
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
