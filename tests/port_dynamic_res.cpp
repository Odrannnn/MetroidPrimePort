// Dynamic resolution controller (platform/port_dynamic_res.h) against a model of a
// scene: the GPU's frame time is a fixed part plus a part that grows with the scale
// squared, the CPU's is constant, and a frame takes the longer of the two (they
// overlap), capped at the frame limit.
#include "port_dynamic_res.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

using PortDynamicRes::Controller;
using PortDynamicRes::Reason;
using PortDynamicRes::Sample;

struct Scene {
  double fixedMs = 1.0;     // GPU time that does not scale (present, small passes)
  double perPixelMs = 8.0;  // GPU time per scale^2
  double cpuMs = 5.0;
  double capFps = 60.0;     // 0 = uncapped
  // Idle time inside the span, between passes (the GPU waiting on the CPU); and the
  // share of the GPU's work the timed passes cover.
  double gapMs = 0.0;
  double passShare = 1.0;
  bool publishes = true;    // false: GPU times supported but never published
  bool compiling = false;   // pipelines building in the background
  double Gpu(float scale) const { return fixedMs + perPixelMs * double(scale) * scale; }
  double Span(float scale) const { return Gpu(scale) + gapMs; }
  double Passes(float scale) const { return Gpu(scale) * passShare; }
  double Fps(float scale) const {
    const double fps = 1000.0 / std::max(cpuMs, Gpu(scale));
    return capFps > 0.0 ? std::min(fps, capFps) : fps;
  }
};

struct Run {
  Controller controller;
  float top = 2.f;
  float bottom = 0.5f;
  double target = 60.0;
  bool gpuTimes = true;
  int changes = 0;
  // Like gpu_prof: GPU times are an average published every 60 frames or half a
  // second of GPU time, so after a change they describe the old scale for a while.
  double published = 0.0;
  double publishedPasses = 0.0;
  double windowMs = 0.0;
  double windowPassMs = 0.0;
  int windowFrames = 0;
  Run() { controller.Reset(top); }
  // Seconds of a scene; returns the changes made in them.
  int Seconds(const Scene& scene, int seconds) {
    int made = 0;
    for (int i = 0; i < seconds; ++i) {
      const float scale = controller.Scale();
      Sample s;
      s.fps = scene.Fps(scale);
      s.frames = unsigned(s.fps);
      for (unsigned f = 0; f < std::max(1u, s.frames); ++f) {
        windowMs += scene.Span(scale);
        windowPassMs += scene.Passes(scale);
        if (++windowFrames == 60 || windowMs >= 500.0) {
          published = windowMs / windowFrames;
          publishedPasses = windowPassMs / windowFrames;
          windowMs = windowPassMs = 0.0;
          windowFrames = 0;
        }
      }
      s.gpuMs = !gpuTimes ? -1.0 : scene.publishes ? published : 0.0;
      s.gpuPassMs = gpuTimes && scene.publishes ? publishedPasses : -1.0;
      s.targetFps = target;
      s.compiling = scene.compiling;
      s.top = top;
      s.bottom = bottom;
      if (controller.Update(s).changed) {
        ++made;
      }
    }
    changes += made;
    return made;
  }
};

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
} // namespace

int main() {
  // GPU-bound: 33 ms at 2x against a 16.7 ms budget. It goes straight to the
  // largest grid scale that fits 85% of the budget (1 + 8 s^2 <= 14.17: 1.25x),
  // in a couple of changes, and then stays there.
  {
    Run run;
    Scene heavy;
    run.Seconds(heavy, 20);
    CHECK_MSG(run.changes >= 1 && run.changes <= 3, "GPU-bound: settles in at most three changes");
    CHECK_MSG(run.controller.Scale() <= 1.31f && run.controller.Scale() >= 1.15f,
              "GPU-bound: lands at the scale that fits the budget");
    CHECK_MSG(heavy.Fps(run.controller.Scale()) >= 59.9, "GPU-bound: the target is reached");
    CHECK_MSG(run.Seconds(heavy, 120) == 0, "GPU-bound: no change once settled");
  }
  // The fixed part is learnt once two scales have been measured.
  {
    Run run;
    Scene heavy;
    heavy.fixedMs = 3.0;
    run.Seconds(heavy, 30);
    CHECK_MSG(run.controller.FixedGpuMs() > 2.5 && run.controller.FixedGpuMs() < 3.5,
              "learns the fixed part of the GPU time");
  }
  // CPU-bound: 25 ms on the CPU, the GPU idle half the frame. Fewer pixels would not
  // help, so it never lowers.
  {
    Run run;
    Scene cpu;
    cpu.cpuMs = 25.0;
    cpu.perPixelMs = 2.0;
    CHECK_MSG(run.Seconds(cpu, 60) == 0 && Near(run.controller.Scale(), 2.f), "CPU-bound: never lowers");
    CHECK_MSG(run.controller.LastReason() == Reason::NotGpuBound, "CPU-bound: says why");
  }
  // Without GPU times, slow seconds while pipelines compile are the compiles, not the
  // pixels: held at the top until they finish, then followed as usual.
  {
    Run run;
    run.gpuTimes = false;
    Scene heavy;
    heavy.compiling = true;
    CHECK_MSG(run.Seconds(heavy, 30) == 0 && Near(run.controller.Scale(), 2.f), "compiling: never lowers");
    CHECK_MSG(run.controller.LastReason() == Reason::Compiling, "compiling: says why");
    heavy.compiling = false;
    CHECK_MSG(run.Seconds(heavy, 30) >= 1 && run.controller.Scale() < 2.f, "compiling done: lowers again");
  }
  // One slow second (a hitch) costs nothing.
  {
    Run run;
    Scene light;
    light.perPixelMs = 2.0;
    Scene spike;
    run.Seconds(light, 10);
    run.Seconds(spike, 1);
    run.Seconds(light, 10);
    CHECK_MSG(run.changes == 0 && Near(run.controller.Scale(), 2.f), "a single slow second changes nothing");
  }
  // A heavy room, then a light one: down, then back up to the top, without stepping
  // past what fits on the way.
  {
    Run run;
    Scene heavy;
    Scene light;
    light.perPixelMs = 2.0;
    run.Seconds(heavy, 20);
    const float low = run.controller.Scale();
    CHECK_MSG(low < 1.5f, "heavy room lowers the scale");
    run.Seconds(light, 60);
    CHECK_MSG(Near(run.controller.Scale(), 2.f), "light room raises it back to the top");
    CHECK_MSG(run.changes <= 5, "down and up again in a handful of changes");
  }
  // A scene at the edge, where the old stepping went back and forth: GPU time just over
  // the budget at 1.25x and well under at 1x. It must not oscillate.
  {
    Run run;
    Scene edge;
    edge.perPixelMs = 10.0;  // 1 + 10 s^2: 16.6 ms at 1.25x, 11 ms at 1x
    run.Seconds(edge, 30);
    CHECK_MSG(run.Seconds(edge, 300) <= 1, "no oscillation at the edge");
    CHECK_MSG(edge.Fps(run.controller.Scale()) >= 59.9, "the edge scene holds the target");
  }
  // A slow GPU (20 fps at 2x): the published times lag behind a
  // change, which must not make it drop past what fits.
  {
    Run run;
    Scene slow;
    slow.perPixelMs = 12.0;  // 1 + 12 s^2: 49 ms at 2x; fits 14.17 ms at 1.05x
    run.Seconds(slow, 40);
    CHECK_MSG(run.controller.Scale() >= 1.0f, "a lagging reading does not overshoot");
    CHECK_MSG(slow.Fps(run.controller.Scale()) >= 59.9, "the slow GPU reaches the target");
    CHECK_MSG(run.changes <= 3, "the slow GPU settles in a few changes");
    CHECK_MSG(run.controller.Scale() < 2.f * 0.6f + 0.01f || run.changes >= 2,
              "one lowering goes no further than 0.6x of the scale");
  }
  // Uncapped, aiming at 120 fps.
  {
    Run run;
    run.target = 120.0;
    Scene heavy;
    heavy.capFps = 0.0;
    heavy.cpuMs = 3.0;
    heavy.perPixelMs = 4.0;  // 1 + 4 s^2: 17 ms at 2x; 0.85 * 8.33 = 7.08 ms -> 1.20x
    run.Seconds(heavy, 30);
    CHECK_MSG(heavy.Fps(run.controller.Scale()) >= 0.97 * 120.0, "uncapped: reaches 120 fps");
    CHECK_MSG(run.controller.Scale() >= 1.15f, "uncapped: no lower than it needs");
  }
  // Never below the lowest scale, even when even that is too slow; and a GPU so
  // overloaded that it draws under 10 frames a second is not taken for a pause.
  {
    Run run;
    Scene hopeless;
    hopeless.perPixelMs = 200.0;
    run.Seconds(hopeless, 30);
    CHECK_MSG(Near(run.controller.Scale(), 0.5f), "stops at the lowest scale");
    CHECK_MSG(run.controller.LastReason() == Reason::AtLowest, "at the lowest scale: says why");
  }
  // No range (the lowest scale is the top): nothing moves.
  {
    Run run;
    run.bottom = 2.f;
    Scene heavy;
    CHECK_MSG(run.Seconds(heavy, 30) == 0 && Near(run.controller.Scale(), 2.f), "no range, no change");
  }
  // The render scale lowered under it: it follows at once.
  {
    Run run;
    Scene light;
    light.perPixelMs = 1.0;
    run.Seconds(light, 5);
    run.top = 1.5f;
    run.Seconds(light, 1);
    CHECK_MSG(run.controller.Scale() <= 1.5f, "follows a lowered render scale");
  }
  // Without GPU times: the old frame-rate stepping, a quarter step after two slow seconds.
  {
    Run run;
    run.gpuTimes = false;
    Scene heavy;
    run.Seconds(heavy, 2);
    CHECK_MSG(Near(run.controller.Scale(), 1.75f), "frame rate only: a quarter step after two slow seconds");
    run.Seconds(heavy, 30);
    CHECK_MSG(heavy.Fps(run.controller.Scale()) >= 59.9 || Near(run.controller.Scale(), 0.5f),
              "frame rate only: still reaches the target");
  }
  // Paused or loading: too few frames, nothing learnt or changed.
  {
    Controller c;
    c.Reset(2.f);
    Sample s;
    s.fps = 5.0;
    s.frames = 5;
    s.gpuMs = 50.0;
    s.top = 2.f;
    s.bottom = 0.5f;
    for (int i = 0; i < 10; ++i) {
      CHECK_MSG(!c.Update(s).changed, "paused: no change");
    }
    CHECK_MSG(c.LastReason() == Reason::Paused, "paused: says why");
  }

  // CPU-bound, but the GPU waits inside its span, so the span alone reads as busy. The
  // passes cover less than half of it: their sum is used, and it never lowers.
  {
    Run run;
    Scene gappy;
    gappy.cpuMs = 25.0;
    gappy.perPixelMs = 1.0;  // 5 ms of work at 2x
    gappy.gapMs = 15.0;      // span 20 ms, over the 13.3 ms "busy" line
    CHECK_MSG(run.Seconds(gappy, 120) == 0 && Near(run.controller.Scale(), 2.f),
              "gaps in the span: the passes' sum shows it is not GPU-bound");
  }
  // The same, with gaps the passes do not give away: the lowering is checked against the
  // frame rate, undone when it gains nothing, and after twice the GPU times are dropped.
  {
    Run run;
    Scene gappy;
    gappy.cpuMs = 25.0;
    gappy.perPixelMs = 2.0;  // 9 ms of work at 2x
    gappy.gapMs = 7.0;       // span 16 ms: passes are over half of it
    run.Seconds(gappy, 300);
    CHECK_MSG(Near(run.controller.Scale(), 2.f), "a lowering that gained nothing is undone");
    CHECK_MSG(run.controller.GpuTimesDropped(), "twice wrong: the GPU times are no longer used");
    CHECK_MSG(run.changes <= 10, "and it stops trying soon");
  }
  // GPU times reported as supported but never published: after a few seconds it follows
  // the frame rate instead of waiting for ever.
  {
    Run run;
    Scene broken;
    broken.publishes = false;
    run.Seconds(broken, 4);
    CHECK_MSG(run.controller.LastReason() == Reason::Collecting && run.changes == 0, "waits a little");
    run.Seconds(broken, 60);
    CHECK_MSG(run.controller.GpuTimesDropped(), "then gives up on GPU times");
    CHECK_MSG(run.controller.Scale() < 2.f, "and lowers on the frame rate");
  }
  // A long load at a crawl publishes no GPU time for many seconds; those seconds are a
  // pause, not a driver that never publishes, so the GPU times are still used after it.
  {
    Controller c;
    c.Reset(2.f);
    Sample load;
    load.fps = 3.0;
    load.frames = 3;
    load.gpuMs = 0.0;
    load.top = 2.f;
    load.bottom = 0.5f;
    for (int i = 0; i < 20; ++i) {
      c.Update(load);
    }
    CHECK_MSG(!c.GpuTimesDropped() && c.LastReason() == Reason::Paused, "a long load does not drop the GPU times");
    Run run;
    run.controller = c;
    Scene heavy;
    run.Seconds(heavy, 20);
    CHECK_MSG(!run.controller.GpuTimesDropped() && run.controller.Scale() < 1.5f, "and they are used once it ends");
  }
  // The fixed part is forgotten a minute after it was measured.
  {
    Run run;
    Scene heavy;
    heavy.fixedMs = 3.0;
    run.Seconds(heavy, 20);
    CHECK_MSG(run.controller.FixedGpuMs() >= 0.0, "fixed part learnt");
    run.Seconds(heavy, 70);
    CHECK_MSG(run.controller.FixedGpuMs() < 0.0, "and forgotten once it is old");
  }

  if (port_test::Failures() == 0) {
    std::puts("dynamic resolution tests passed");
  }
  return PORT_TEST_RESULT();
}
