#pragma once

#include <algorithm>
#include <cmath>

namespace PortTiming {
// Accumulates wall-clock time and releases whole fixed-size simulation steps.
// The step defaults to the GameCube's 60 Hz tick; the port can raise it to run
// the simulation at the display rate (see PortDebug::SimRate()).
class FixedStepClock {
public:
  static constexpr double kPeriod = 1.0 / 60.0;

  void SetPeriod(double period) {
    if (std::isfinite(period) && period > 0.0) {
      mPeriod = period;
    }
  }
  double Period() const { return mPeriod; }

  unsigned Advance(double elapsed, bool oneTick = false, bool cappedCadence = false) {
    if (oneTick) {
      mRemainder = 0.0;
      return 1;
    }
    if (!std::isfinite(elapsed) || elapsed < 0.0) return 0;
    // Bound long stalls, but retain fractional time through ordinary jitter.
    mRemainder = std::min(mRemainder + elapsed, 0.25);
    // A precise deadline still jitters by microseconds. Without a little
    // scheduling leeway it can alternate zero/two ticks at the same rendered
    // frame rate. Keep the borrowed time as debt; no time is lost.
    const double leeway = cappedCadence ? 0.00025 : 0.0;
    const unsigned steps = static_cast<unsigned>(std::max(0.0, mRemainder + leeway) / mPeriod);
    mRemainder -= steps * mPeriod;
    return steps;
  }
  // Adaptive stepping: a step already covered the frame, so leftover time (from
  // a clamped hitch or a change of period) would only cause a stray extra step.
  void DropRemainder() { mRemainder = 0.0; }
  float Interpolation() const {
    return static_cast<float>(std::clamp(mRemainder / mPeriod, 0.0, 1.0));
  }

private:
  double mPeriod = kPeriod;
  double mRemainder = 0.0;
};
} // namespace PortTiming
