#include "port_timing.h"
#include "port_test.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

int main() {
  PortTiming::FixedStepClock clock;
  unsigned steps = 0;
  for (unsigned frame = 0; frame < 6000; ++frame) {
    steps += clock.Advance(1.0 / 60.0 + (frame % 2 ? 0.000005 : -0.000005));
    REQUIRE(clock.Interpolation() >= 0.f && clock.Interpolation() <= 1.f);
  }
  // The previous remainder-reset heuristic produced only 4500 steps here.
  REQUIRE(steps >= 5999 && steps <= 6000);
  PortTiming::FixedStepClock capped;
  for (unsigned frame = 0; frame < 6000; ++frame)
    REQUIRE(capped.Advance(1.0 / 60.0 + (frame % 2 ? 0.000005 : -0.000005), false, true) == 1);
  // Borrowing near a deadline must be repaid even if the cap is then disabled.
  PortTiming::FixedStepClock debt;
  REQUIRE(debt.Advance(PortTiming::FixedStepClock::kPeriod - 0.0001, false, true) == 1);
  REQUIRE(debt.Advance(0.0001) == 0);
  REQUIRE(debt.Interpolation() < 0.00001f);
  PortTiming::FixedStepClock half;
  REQUIRE(half.Advance(PortTiming::FixedStepClock::kPeriod * 0.5) == 0);
  REQUIRE(std::fabs(half.Interpolation() - 0.5f) < 0.00001f);
  REQUIRE(half.Advance(PortTiming::FixedStepClock::kPeriod * 0.5) == 1);
  REQUIRE(half.Advance(2.0) == 15); // bounded stall catch-up
  REQUIRE(half.Advance(0.0) == 0);
  REQUIRE(half.Advance(-1.0) == 0);
  REQUIRE(half.Advance(std::numeric_limits<double>::quiet_NaN()) == 0);
  REQUIRE(half.Advance(0.0, true) == 1);
  // Adaptive stepping (period = frame time): a leftover from a hitch would
  // otherwise persist and turn a shorter frame into two steps.
  PortTiming::FixedStepClock adaptive;
  adaptive.SetPeriod(1.0 / 30.0);
  REQUIRE(adaptive.Advance(0.05) == 1);
  adaptive.SetPeriod(1.0 / 144.0);
  REQUIRE(adaptive.Advance(1.0 / 144.0) == 3);
  adaptive.DropRemainder();
  for (unsigned frame = 0; frame < 1000; ++frame) {
    const double elapsed = frame % 2 ? 1.0 / 120.0 : 1.0 / 160.0;
    adaptive.SetPeriod(elapsed);
    REQUIRE(adaptive.Advance(elapsed) == 1);
    adaptive.DropRemainder();
  }
  std::puts("fixed-step jitter, carry and stall regressions passed");
}
