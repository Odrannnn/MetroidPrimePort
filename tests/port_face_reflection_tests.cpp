// Remastered's explosion-lit face reflection state (include/MetroidPrime/PortFaceReflection.hpp):
// the mode splines, explosion registration and the billboard fade.
#include "MetroidPrime/PortFaceReflection.hpp"

#include "port_test.h"

#include <cmath>

using namespace PortFaceReflection;

namespace {

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

void TestWeight() {
  CHECK(Near(Weight(1, 0.f), 0.f));
  CHECK(Near(Weight(1, 0.05f), 0.3f));
  CHECK(Near(Weight(1, 0.1f), 0.6f));
  CHECK(Near(Weight(1, 0.501f), 0.3f));
  CHECK(Near(Weight(1, 1.f), 0.f));
  CHECK(Near(Weight(2, 0.1f), 0.99991f * 0.3f));
  CHECK(Near(Weight(3, 0.55f), 0.5f));
  CHECK(Near(Weight(3, -0.1f), 0.f));
  CHECK(Near(Weight(3, 1.5f), 0.f));
}

void TestExplosions() {
  State s;
  AddExplosion(s, TUniqueId(0, 1), 1, 7.9f);
  AddExplosion(s, TUniqueId(0, 2), 1, 8.f);
  AddExplosion(s, TUniqueId(0, 3), 3, 15.f);
  AddExplosion(s, TUniqueId(0, 4), 2, 1.f);
  AddExplosion(s, TUniqueId(0, 5), 0, 1.f);
  CHECK_MSG(s.explosions.size() == 3, "only modes 1..3 within their distance register");
  RemoveExplosion(s, TUniqueId(0, 1));
  CHECK_MSG(s.explosions.size() == 2 && s.explosions[0] == TUniqueId(0, 4) && s.explosions[1] == TUniqueId(0, 3),
            "the last explosion takes the removed one's slot");
  RemoveExplosion(s, TUniqueId(0, 9));
  CHECK(s.explosions.size() == 2);
  AddBillboard(s);
  AddExplosion(s, TUniqueId(0, 6), 1, 1.f);
  CHECK_MSG(s.explosions.size() == 2, "no explosion registers while a billboard exists");
}

void TestFade() {
  State s;
  CHECK(Near(AlphaModifier(s), 1.f));
  AddBillboard(s);
  CHECK(s.fade == 2 && Near(AlphaModifier(s), 1.f));
  Update(s, 0.25f);
  CHECK(Near(AlphaModifier(s), 0.5f));
  AddBillboard(s);
  CHECK_MSG(Near(s.t, 0.25f), "a second billboard does not restart the fade");
  Update(s, 1.f);
  CHECK(s.fade == 0 && s.t == 0.f && Near(AlphaModifier(s), 0.f));
  RemoveBillboard(s);
  CHECK(s.fade == 0 && Near(AlphaModifier(s), 0.f));
  RemoveBillboard(s);
  CHECK(s.fade == 1 && Near(AlphaModifier(s), 0.f));
  Update(s, 0.125f);
  CHECK(Near(AlphaModifier(s), 0.25f));
  Update(s, 1.f);
  CHECK(Near(AlphaModifier(s), 1.f));
  RemoveBillboard(s);
  CHECK_MSG(s.billboards == 0 && s.fade == 0, "an extra remove does nothing");
}

} // namespace

int main() {
  TestWeight();
  TestExplosions();
  TestFade();
  return PORT_TEST_RESULT();
}
