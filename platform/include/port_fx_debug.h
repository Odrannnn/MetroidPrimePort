#ifndef METROID_PRIME_PORT_PORT_FX_DEBUG_H
#define METROID_PRIME_PORT_PORT_FX_DEBUG_H
#include <dolphin/gx/GXExtra.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Debug view of the live particle generators (console `fx list|tree|stats|mute|solo|timescale`).
// Every CParticleGen links itself into an intrusive list in its constructor and out in its
// destructor (O(1) each); everything else here runs only when a console command asks, or, for the
// mute/time-scale/timing hooks, behind one global flag that is off by default.

class CParticleGen;

// What a generator reports about itself (CParticleGen::PortFxDescribe).
struct PortFxInfo {
  uint32_t kind = 0; // 'PART', 'SWHC', 'ELSC'
  uint32_t asset = 0;
  int particles = 0;
  int maxParticles = -1; // -1: n/a
  int frame = 0;
  int lifetime = -1;
  bool emitting = false;
  bool deletable = false;
  float pos[3] = {0.f, 0.f, 0.f};
  std::string vfx;                          // native VFX props in use ("VMAT VTMT ..."), empty = retail draw
  std::vector< CParticleGen* > children;    // generators this one owns and updates
};

namespace PortFx {
// Hooks read by the generators; all off/neutral by default.
extern bool gMuteActive;      // a mute or solo set is in force
extern bool gTiming;          // `fx stats` has been asked for: time update and render
extern float gTimeScale;      // 1 = off
extern std::atomic< uint32_t > gQuads, gTriangles, gDraws; // native VFX, this frame so far
extern std::atomic< int64_t > gUpdateNs, gRenderNs;        // outermost update/render calls, this frame

void Register(CParticleGen* gen);
void Unregister(CParticleGen* gen);

// True when `gen` must not draw (mute set, or solo with a different asset).
bool IsMuted(const CParticleGen& gen);

// Called once per rendered frame: publishes the counters above as "last frame" and clears them.
void FrameBoundary();

// Console back end. Each line goes to `out`.
using Emit = std::function< void(const std::string&) >;
void List(const std::string& filter, const Emit& out);
bool Tree(uint32_t id, const Emit& out);
void Stats(const Emit& out);
// Mute set. Assets are PART/SWHC/ELSC ids; `solo` inverts the meaning (only these draw).
void MuteAdd(uint32_t asset);
void SoloSet(const std::vector< uint32_t >& assets);
void MuteClear();
void MuteList(const Emit& out);
// Assets of a generator and all its descendants; empty when `id` is not live.
std::vector< uint32_t > TreeAssets(uint32_t id);
// The short id of a generator (0 = none).
uint32_t IdOf(const CParticleGen* gen);
// The newest live root generator, or nullptr (for `fx <PART>` to report its id).
CParticleGen* Newest();
bool Alive(uint32_t id);

// Update/render scopes in the generators. The outermost update applies the time scale; every
// outermost call is timed when gTiming is on. Nested calls (children) do neither.
struct UpdateScope {
  explicit UpdateScope(double dtIn) : dt(dtIn) {
    top = depth()++ == 0;
    if (top) {
      if (gTimeScale != 1.f) {
        if (gTimeScale <= 0.f) {
          skip = true;
        } else {
          dt *= gTimeScale;
        }
      }
      if (gTiming) {
        t0 = std::chrono::steady_clock::now();
      }
    }
  }
  ~UpdateScope() {
    --depth();
    if (top && gTiming) {
      gUpdateNs += std::chrono::duration_cast< std::chrono::nanoseconds >(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    }
  }
  UpdateScope(const UpdateScope&) = delete;
  UpdateScope& operator=(const UpdateScope&) = delete;
  double dt;
  bool skip = false;

private:
  static int& depth() {
    static thread_local int d = 0;
    return d;
  }
  bool top = false;
  std::chrono::steady_clock::time_point t0;
};

struct RenderScope {
  RenderScope() {
    top = depth()++ == 0;
    if (top) {
      GXPortSetParticleFog(GX_TRUE);
    }
    if (top && gTiming) {
      t0 = std::chrono::steady_clock::now();
    }
  }
  ~RenderScope() {
    --depth();
    if (top) {
      GXPortSetParticleFog(GX_FALSE);
    }
    if (top && gTiming) {
      gRenderNs += std::chrono::duration_cast< std::chrono::nanoseconds >(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    }
  }
  RenderScope(const RenderScope&) = delete;
  RenderScope& operator=(const RenderScope&) = delete;

private:
  static int& depth() {
    static thread_local int d = 0;
    return d;
  }
  bool top = false;
  std::chrono::steady_clock::time_point t0;
};
} // namespace PortFx

#endif
