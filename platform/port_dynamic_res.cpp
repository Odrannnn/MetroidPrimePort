#include "port_dynamic_res.h"

#include <algorithm>
#include <cmath>

namespace PortDynamicRes {
namespace {

// Below this share of the target the second counts as slow; at or above the
// other it counts as on target.
constexpr double kSlow = 0.93;
constexpr double kOnTarget = 0.97;
// The GPU is what holds the frame back only when it is busy for at least this
// share of the frame budget; otherwise the CPU (or a stall) is.
constexpr double kGpuBusy = 0.8;
// A new scale is aimed at this share of the budget, leaving room for heavier
// moments without dropping again.
constexpr double kHeadroom = 0.85;
// Scales are picked on this grid, and a change smaller than kMinStep is not
// worth the reallocation stutter (except to reach the top or the bottom).
constexpr float kGrid = 0.05f;
constexpr float kMinStep = 0.1f;
// Seconds skipped after a change: the stall itself, then one more so the GPU times
// (an average published every 60 frames or half a second) hold only the new scale.
constexpr int kSettleFps = 1;
constexpr int kSettleGpu = 2;
constexpr float kMaxRaise = 0.5f;
// One lowering goes no further than this share of the scale (36% of the pixels), so a
// wrong estimate cannot send it to the bottom, from where it climbs back slowly.
constexpr float kMaxDropRatio = 0.6f;
// A learnt fixed part is from two seconds that may have been in different rooms; it is
// forgotten after this long, and never taken as more than this share of the GPU time.
constexpr int kFixedLife = 60;
constexpr double kFixedShare = 0.5;
// Seconds to wait for a first GPU time before following the frame rate instead.
constexpr int kCollectTimeout = 5;
// GPU-time lowerings that gained nothing before the GPU times are distrusted.
constexpr int kMaxNoGains = 2;

bool Worth(float from, float to, float top, float bottom) {
  const float d = std::fabs(to - from);
  if (d >= kMinStep - 1e-4f) {
    return true;
  }
  return d > 1e-3f && (to == top || to == bottom);
}

} // namespace

const char* ReasonText(Reason reason) {
  switch (reason) {
  case Reason::Off: return "off";
  case Reason::Settling: return "settling after a change";
  case Reason::Paused: return "paused or loading";
  case Reason::Collecting: return "waiting for GPU times";
  case Reason::Steady: return "steady";
  case Reason::NotGpuBound: return "below target, but not GPU-bound";
  case Reason::GpuBound: return "GPU-bound: lowering";
  case Reason::AtLowest: return "GPU-bound at the lowest scale";
  case Reason::Headroom: return "GPU has headroom: raising";
  case Reason::RaiseFailed: return "raise was too slow, undone";
  case Reason::NoGain: return "lowering gained nothing, undone";
  case Reason::NoGpuTimes: return "GPU times unavailable or unreliable; following the frame rate";
  case Reason::Compiling: return "pipelines compiling: held";
  }
  return "?";
}

void Controller::Reset(float top) {
  *this = Controller();
  mScale = top;
}

Decision Controller::Change(float next, Reason reason, int settle) {
  mScale = next;
  mReason = reason;
  mSettle = settle;
  mSlow = 0;
  return {mScale, true, reason};
}

void Controller::Learn(float scale, double gpuMs) {
  // Two seconds at scales far enough apart give the fixed part:
  // gpu = fixed + perPixel * scale^2.
  if (mMeasuredScale > 0.f && std::fabs(scale - mMeasuredScale) >= kMinStep - 1e-4f) {
    const double s1 = double(mMeasuredScale) * mMeasuredScale;
    const double s2 = double(scale) * scale;
    const double perPixel = (mMeasuredMs - gpuMs) / (s1 - s2);
    if (perPixel > 0.0) {
      const double fixed = gpuMs - perPixel * s2;
      // A noisy pair can put it outside what one frame holds; keep what is plausible.
      if (fixed >= 0.0 && fixed < std::min(gpuMs, mMeasuredMs)) {
        mFixedMs = fixed;
        mFixedAge = 0;
      }
    }
  }
  mMeasuredScale = scale;
  mMeasuredMs = gpuMs;
  if (mFixedMs >= 0.0 && ++mFixedAge > kFixedLife) {
    mFixedMs = -1.0;
  }
}

double Controller::Predict(float from, double gpuMs, float to) const {
  const double fixed = mFixedMs >= 0.0 ? std::min(mFixedMs, gpuMs * kFixedShare) : 0.0;
  const double perPixel = (gpuMs - fixed) / (double(from) * from);
  return fixed + perPixel * double(to) * to;
}

float Controller::FitBudget(float from, double gpuMs, double budgetMs, float top, float bottom) const {
  // The largest scale on the grid (or the top itself) predicted to fit.
  const double allowed = budgetMs * kHeadroom;
  if (Predict(from, gpuMs, top) <= allowed) {
    return top;
  }
  for (int k = int(std::floor((top - 1e-4f) / kGrid)); k * kGrid >= bottom - 1e-4f; --k) {
    const float s = k * kGrid;
    if (Predict(from, gpuMs, s) <= allowed) {
      return std::max(s, bottom);
    }
  }
  return bottom;
}

Decision Controller::Update(const Sample& s) {
  const float top = s.top;
  const float bottom = std::min(s.bottom, top);
  if (top <= bottom) {
    if (mScale != top) {
      return Change(top, Reason::Off, 0);
    }
    mReason = Reason::Off;
    return {mScale, false, mReason};
  }
  // The range moved under it (the render scale was changed).
  if (mScale > top || mScale < bottom) {
    return Change(std::clamp(mScale, bottom, top), Reason::Off, kSettleFps);
  }
  if (mSettle > 0) {
    --mSettle;
    mReason = Reason::Settling;
    return {mScale, false, mReason};
  }
  // GPU times are coming but none is published yet: wait rather than guess, for a few
  // seconds; a driver that never publishes one is followed by the frame rate instead.
  // Only seconds with frames to judge count: a long load at a crawl can publish nothing
  // for that long without anything being wrong with the GPU times.
  if (s.gpuMs == 0.0 && !mGpuDropped) {
    const bool judgeable = s.frames >= 10 && s.targetFps > 0.0;
    if (!judgeable || ++mCollecting < kCollectTimeout) {
      mReason = judgeable ? Reason::Collecting : Reason::Paused;
      return {mScale, false, mReason};
    }
    mGpuDropped = true;
  }
  const bool useGpu = s.gpuMs > 0.0 && !mGpuDropped;
  if (useGpu) {
    mCollecting = 0;
  }
  // The GPU's busy time: the span, unless the timed passes cover under half of it. Then
  // the span is mostly gaps between passes (the GPU waiting on the CPU, a readback, a
  // second submit) and the passes' own sum is the better measure.
  double busy = s.gpuMs;
  if (useGpu && s.gpuPassMs > 0.0 && s.gpuPassMs < s.gpuMs * 0.5) {
    busy = s.gpuPassMs;
  }
  // Few frames is a pause or a load, unless the GPU was busy for most of the second:
  // then it is a scene too heavy for it, which is exactly when to lower.
  const bool gpuFull = useGpu && busy * s.frames >= 700.0;
  if ((s.frames < 10 && !gpuFull) || s.targetFps <= 0.0) {
    mReason = Reason::Paused;
    return {mScale, false, mReason};
  }
  if (!useGpu) {
    // A compile stall looks like a GPU too slow for the scale; GPU times see through it.
    if (s.compiling) {
      mSlow = 0;
      mSteady = 0;
      mReason = Reason::Compiling;
      return {mScale, false, mReason};
    }
    const Decision d = UpdateFromFps(s, top, bottom);
    if (!d.changed && mGpuDropped && d.reason == Reason::Steady) {
      mReason = Reason::NoGpuTimes;
      return {mScale, false, mReason};
    }
    return d;
  }
  return UpdateFromGpu(s, busy, top, bottom);
}

Decision Controller::UpdateFromGpu(const Sample& s, double busy, float top, float bottom) {
  Learn(mScale, busy);
  const double budget = 1000.0 / s.targetFps;
  const bool slow = s.fps < s.targetFps * kSlow;
  const float fit = std::max(FitBudget(mScale, busy, budget, top, bottom), std::max(mScale * kMaxDropRatio, bottom));
  if (mHoldLow > 0) {
    --mHoldLow;
  }

  // The first settled second after a lowering: the GPU times promised it would help.
  // If the frame rate did not rise, they were wrong about what holds the frame back
  // (gaps the span counts as busy, a tiler's pass timings): undo it and hold off, and
  // after twice stop trusting them.
  if (mDropFrom > 0.f) {
    const float from = mDropFrom;
    mDropFrom = 0.f;
    const bool gained = s.fps >= mDropFps * 1.03 || !slow;
    if (!gained) {
      mHoldLow = mHoldFor;
      mHoldFor = std::min(mHoldFor * 2, 240);
      if (++mNoGains >= kMaxNoGains) {
        mGpuDropped = true;
      }
      return Change(from, Reason::NoGain, kSettleGpu);
    }
  }

  if (mProbation >= 0) {
    if (slow && busy >= budget * kGpuBusy) {
      // The raise was too much after all: back to where it was, and wait longer next time.
      mRaiseAfter = std::min(mRaiseAfter * 2, 64);
      mProbation = -1;
      mSteady = 0;
      return Change(mBeforeRaise, Reason::RaiseFailed, kSettleGpu);
    }
    if (++mProbation >= 3) {
      mProbation = -1;
    }
  }

  if (slow) {
    mSteady = 0;
    if (busy < budget * kGpuBusy) {
      // The GPU had time to spare: fewer pixels would not bring the frames back.
      mSlow = 0;
      mReason = Reason::NotGpuBound;
      return {mScale, false, mReason};
    }
    // Two slow seconds in a row, so a single hitch does not cost a stutter.
    if (++mSlow >= 2 && mHoldLow == 0 && fit < mScale && Worth(mScale, fit, top, bottom)) {
      mDropFrom = mScale;
      mDropFps = s.fps;
      return Change(fit, Reason::GpuBound, kSettleGpu);
    }
    mReason = mScale <= bottom ? Reason::AtLowest : fit < mScale ? Reason::GpuBound : Reason::NotGpuBound;
    return {mScale, false, mReason};
  }

  mSlow = 0;
  if (mProbation < 0 && s.fps >= s.targetFps * kOnTarget && mScale < top && ++mSteady >= mRaiseAfter) {
    mSteady = 0;
    const float next = std::min(fit, mScale + kMaxRaise);
    if (next > mScale && Worth(mScale, next, top, bottom)) {
      mBeforeRaise = mScale;
      mProbation = 0;
      return Change(next, Reason::Headroom, kSettleGpu);
    }
  }
  mReason = Reason::Steady;
  return {mScale, false, mReason};
}

// Without GPU times: the frame rate alone. Down a step after two slow seconds;
// a step down that gains nothing means the GPU was not the limit, so it goes
// back up and holds there; up a step after a steady stretch that doubles each
// time a raise turns out too slow.
Decision Controller::UpdateFromFps(const Sample& s, float top, float bottom) {
  const float scale = mScale;
  // Half steps above 2x, where a quarter step changes the pixel count by little.
  const float stepUp = scale >= 2.f ? 0.5f : 0.25f;
  const float stepDown = scale > 2.f ? 0.5f : 0.25f;
  const double target = s.targetFps;
  if (mHoldLow > 0) {
    --mHoldLow;
  }
  float next = scale;
  Reason reason = Reason::Steady;
  if (mFpsBefore > 0.0) {
    if (s.fps < mFpsBefore * 1.03) {
      next = scale + stepUp;
      reason = Reason::NoGain;
      mHoldLow = mHoldFor;
      mHoldFor = std::min(mHoldFor * 2, 240);
    }
    mFpsBefore = 0.0;
  } else if (s.fps < target * kSlow) {
    mSteady = 0;
    if (mProbation >= 0) {
      next = scale - stepDown;
      reason = Reason::RaiseFailed;
      mRaiseAfter = std::min(mRaiseAfter * 2, 64);
      mProbation = -1;
    } else if (++mSlow >= 2 && mHoldLow == 0 && scale > bottom) {
      next = scale - stepDown;
      reason = Reason::GpuBound;
      mFpsBefore = s.fps;
    }
  } else {
    mSlow = 0;
    if (mProbation >= 0 && ++mProbation >= 3) {
      mProbation = -1;
    }
    if (mProbation < 0 && s.fps >= target * kOnTarget && scale < top && ++mSteady >= mRaiseAfter) {
      next = scale + stepUp;
      reason = Reason::Headroom;
      mSteady = 0;
      mProbation = 0;
    }
  }
  next = std::clamp(next, bottom, top);
  if (next != scale) {
    return Change(next, reason, kSettleFps);
  }
  mReason = reason;
  return {mScale, false, mReason};
}

} // namespace PortDynamicRes
