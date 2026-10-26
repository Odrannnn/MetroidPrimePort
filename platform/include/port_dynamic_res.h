#pragma once

// Dynamic resolution: picks the EFB scale once a second so the frame rate holds
// its target. Kept apart from the overlay so it can be tested on recorded or
// made-up seconds (tests/port_dynamic_res.cpp); platform/debug_ui/options.cpp
// feeds it and applies what it returns.
//
// With the GPU's own time per frame (timestamp queries, Aurora's gpu_prof) it
// knows whether the GPU is what holds the frame back, and by how much: GPU time
// is taken as a fixed part plus a part that grows with the pixel count (the
// scale squared), so it can go straight to the scale that fits the frame budget.
// Each change reallocates the EFB and stutters, so it moves only when the
// difference is worth it, down quickly and up slowly. A lowering is checked
// against the frame rate once settled and undone when it gained nothing; after
// twice, or when no GPU time arrives, it falls back to stepping on the frame rate
// alone, as it does on a GPU that cannot time itself.

namespace PortDynamicRes {

// One second of frames.
struct Sample {
  double fps = 0.0;       // presented frames per wall-clock second
  unsigned frames = 0;    // presented frames in it
  // GPU time per frame (gpu_prof's span: first pass start to last pass end); negative
  // when the GPU cannot time itself, 0 while the first average is still being taken.
  double gpuMs = -1.0;
  // The sum of the timed passes in the same frames. Below the span by the gaps between
  // passes; when those are most of it, the span is not the GPU's busy time.
  double gpuPassMs = -1.0;
  double targetFps = 60.0;
  // Pipelines were queued or building: their compiles stall the CPU (under GL they
  // hold the one context the frame needs), so the frame rate says nothing about pixels.
  bool compiling = false;
  float top = 1.f;        // the configured scale: the most it draws at
  float bottom = 1.f;     // the lowest scale allowed
};

enum class Reason {
  Off,          // nothing to do (no range, or just reset)
  Settling,     // the second after a change holds its stall
  Paused,       // too few frames to judge
  Collecting,   // GPU times are on but not published yet
  Steady,       // at the target, no change worth making
  NotGpuBound,  // below the target, but the GPU has time to spare
  GpuBound,     // the GPU cannot fit the frame budget: lowered, or about to be
  AtLowest,     // GPU-bound, but already at the lowest scale allowed
  Headroom,     // the GPU has room for more pixels: raised
  RaiseFailed,  // a raise turned out too slow and was undone
  NoGain,       // a lowering gained nothing and was undone
  NoGpuTimes,   // GPU times never arrived, or proved wrong: following the frame rate
  Compiling,    // following the frame rate while pipelines compile: held
};
const char* ReasonText(Reason reason);

struct Decision {
  float scale = 1.f;     // the scale to draw at
  bool changed = false;  // differs from the previous second's
  Reason reason = Reason::Off;
};

class Controller {
public:
  // Back to the top scale with nothing learnt.
  void Reset(float top);
  Decision Update(const Sample& sample);

  float Scale() const { return mScale; }
  Reason LastReason() const { return mReason; }
  // The share of GPU time found not to depend on the scale, in ms; -1 until two
  // scales have been measured, and again once that measurement is a minute old.
  double FixedGpuMs() const { return mFixedMs; }
  // The GPU times are not used any more: none arrived, or lowering on them twice
  // gained nothing. Lasts until Reset.
  bool GpuTimesDropped() const { return mGpuDropped; }

private:
  Decision UpdateFromGpu(const Sample& s, double busyMs, float top, float bottom);
  Decision UpdateFromFps(const Sample& s, float top, float bottom);
  Decision Change(float next, Reason reason, int settle);
  void Learn(float scale, double gpuMs);
  double Predict(float from, double gpuMs, float to) const;
  float FitBudget(float from, double gpuMs, double budgetMs, float top, float bottom) const;

  float mScale = 1.f;
  Reason mReason = Reason::Off;
  int mSettle = 0;
  int mSlow = 0;
  int mSteady = 0;
  int mRaiseAfter = 4;   // steady seconds before a raise; doubles when one fails
  int mProbation = -1;   // seconds since the last raise, while it may still be undone
  float mBeforeRaise = 0.f;
  // GPU model: the last measurement at a different scale, and the fixed part.
  float mMeasuredScale = 0.f;
  double mMeasuredMs = 0.0;
  double mFixedMs = -1.0;
  int mFixedAge = 0;       // seconds since the fixed part was measured
  // The last lowering on GPU times, checked against the frame rate once settled.
  float mDropFrom = 0.f;
  double mDropFps = 0.0;
  int mNoGains = 0;        // GPU-time lowerings that gained nothing
  int mCollecting = 0;     // seconds waiting for the first GPU time
  bool mGpuDropped = false;
  // Frame-rate-only fallback.
  double mFpsBefore = 0.0;
  int mHoldLow = 0;
  int mHoldFor = 30;
};

} // namespace PortDynamicRes
