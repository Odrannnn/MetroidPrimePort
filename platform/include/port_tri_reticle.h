#pragma once

// Pure Trilogy reticle/input equations; addresses refer to RS5MP1 USA.
namespace PortTriReticle {
inline float Clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
// fn_80235C98: morph precedes the command latch, then the ordinary visor mapping.
inline int State(int morph, bool latch, int visor) {
  return morph != 0 ? 4 : latch ? 5 : visor == 0 ? 0 : visor == 1 ? 2 : visor == 2 ? 1 : visor == 3 ? 3 : 0;
}
inline bool AimVisible(int state) { return state == 0 || state == 2 || state == 3; }
inline bool LockVisible(int state) { return state == 0 || state == 5; }
// fn_80307210/80307370: two secant-tangent keys, constant extrapolation.
inline float ArmResponse(float d) { return d <= 10.f ? 1.f : d >= 30.f ? .5f : 1.f - (d - 10.f) * .025f; }
inline float CentreResponse(float d) { return d <= 10.f ? 1.f : d >= 30.f ? .6f : 1.f - (d - 10.f) * .02f; }
// fn_80236A98: per-update, NOT dt scaled.
inline float ScaleStep(float value, float target) {
  return Clamp(value + Clamp(target - value, -1.f / 45.f, 1.f / 45.f), 0.f, 1.f);
}
// fn_8018292C: byte1188 bits 7|6.
inline bool LockHeld(bool orbitHeld, bool grapple, bool latch) { return (orbitHeld && !grapple) || latch; }
// fn_80015DAC: pointer+a4 is a signed-positive test, not a flag.
inline bool OffScreen(unsigned invalidFrames, bool cinematic, int morph, int pointerA4,
                      bool ballAllowsPointer) {
  return invalidFrames != 0 && !cinematic && morph != 2 && morph != 3 && pointerA4 <= 0 &&
         (morph == 0 || ballAllowsPointer);
}
// fn_80075A68/80075BE0/80075E84: visor (0) is processed before beam (1). eligible[i] is
// fn_80075B58 for that command; a latched command that stops being eligible is cancelled
// (fn_800772CC), without setting +190 (released command); the HUD sees either as a release.
struct Latch {
  float held[2] = {};
  bool down[2] = {};
  int active = -1;
  void Tick(bool visor, bool beam, const bool eligible[2]) {
    const bool now[2] = {visor, beam};
    for (int i = 0; i < 2; ++i) {
      held[i] = !now[i] || !down[i] ? 0.f : held[i] + 1.f / 60.f;
      down[i] = now[i];
    }
    for (int i = 0; i < 2; ++i) {
      if (active == i && !eligible[i]) {
        active = -1;
        continue;
      }
      if (active == i && !now[i]) active = -1;
      else if (active == -1 && now[i] && held[i] > .175f && eligible[i]) active = i;
    }
  }
};
} // namespace PortTriReticle
