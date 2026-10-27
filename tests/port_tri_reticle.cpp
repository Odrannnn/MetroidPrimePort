#include "port_tri_reticle.h"
#include "port_test.h"

using namespace PortTriReticle;
static bool Near(float a, float b) { return a > b - .00001f && a < b + .00001f; }
int main() {
  for (int visor = 0; visor < 4; ++visor) {
    const int states[] = {0, 2, 1, 3};
    CHECK_EQ(State(0, false, visor), states[visor]);
    CHECK_EQ(State(0, true, visor), 5);
    CHECK_EQ(State(1, true, visor), 4);
    CHECK_EQ(State(2, false, visor), 4);
    CHECK_EQ(State(3, true, visor), 4);
  }
  for (int state = 0; state < 8; ++state) {
    CHECK_EQ(AimVisible(state), state == 0 || state == 2 || state == 3);
    CHECK_EQ(LockVisible(state), state == 0 || state == 5);
  }
  CHECK(Near(ArmResponse(0), 1)); CHECK(Near(ArmResponse(10), 1));
  CHECK(Near(ArmResponse(20), .75f)); CHECK(Near(ArmResponse(30), .5f));
  CHECK(Near(ArmResponse(100), .5f)); CHECK(Near(CentreResponse(20), .8f));
  CHECK(Near(CentreResponse(30), .6f)); CHECK(Near(CentreResponse(100), .6f));
  CHECK(Near(ScaleStep(.5f, 1), .5f + 1.f / 45.f));
  CHECK(Near(ScaleStep(.5f, 0), .5f - 1.f / 45.f));
  CHECK(Near(ScaleStep(.5f, .51f), .51f));
  CHECK(Near(ScaleStep(0, -1), 0)); CHECK(Near(ScaleStep(1, 2), 1));
  CHECK(LockHeld(true, false, false)); CHECK(!LockHeld(true, true, false));
  CHECK(LockHeld(false, true, true)); CHECK(!LockHeld(false, false, false));
  CHECK(OffScreen(1, false, 0, 0, false));
  CHECK(!OffScreen(0, false, 0, 0, true)); CHECK(!OffScreen(1, true, 0, 0, true));
  CHECK(!OffScreen(1, false, 2, 0, true)); CHECK(!OffScreen(1, false, 3, 0, true));
  CHECK(!OffScreen(1, false, 0, 1, true)); CHECK(OffScreen(1, false, 0, -1, false));
  CHECK(!OffScreen(1, false, 1, 0, false)); CHECK(OffScreen(1, false, 1, 0, true));
  const bool yes[2] = {true, true}, no[2] = {false, false}, visorOnly[2] = {true, false};
  Latch latch;
  for (int i = 1; i <= 11; ++i) { latch.Tick(true, true, yes); CHECK_EQ(latch.active, -1); }
  latch.Tick(true, true, yes); CHECK_EQ(latch.active, 0); // 12th held tick; visor wins.
  for (int i = 0; i < 20; ++i) latch.Tick(true, true, yes);
  CHECK_EQ(latch.active, 0); // no second latch
  latch.Tick(false, true, yes); CHECK_EQ(latch.active, 1);
  latch.Tick(false, false, yes); CHECK_EQ(latch.active, -1);
  CHECK(Near(latch.held[0], 0)); CHECK(Near(latch.held[1], 0));
  for (int i = 0; i < 20; ++i) latch.Tick(false, true, no);
  CHECK_EQ(latch.active, -1);
  latch.Tick(false, true, yes); CHECK_EQ(latch.active, 1);
  // fn_800772CC: a latched command that turns ineligible is cancelled while still held,
  // and relatches once eligible again (the hold timer kept running).
  latch.Tick(false, true, visorOnly); CHECK_EQ(latch.active, -1);
  latch.Tick(false, true, yes); CHECK_EQ(latch.active, 1);
  latch.Tick(false, false, yes); CHECK_EQ(latch.active, -1);
  CHECK(Near(latch.held[0], 0)); CHECK(Near(latch.held[1], 0));
  for (int i = 0; i < 20; ++i) latch.Tick(false, true, no);
  CHECK_EQ(latch.active, -1);
  latch.Tick(false, true, yes); CHECK_EQ(latch.active, 1);
  return PORT_TEST_RESULT();
}
