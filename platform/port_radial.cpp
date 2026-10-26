#include "port_radial.h"

#include <atomic>
#include <cmath>

namespace PortRadial {
namespace {

// Pointer travel for a full radius: about a fifth of a 1000 count/inch mouse swipe.
constexpr float kTravelFull = 200.f;
// The C-stick points the selection past half deflection.
constexpr float kStickPoint = 0.5f;

std::atomic<bool> sHeld[2]{};
std::atomic<bool> sOpen[2]{};
std::atomic<bool> sAvailable{false};
std::atomic<int> sPick[2]{-1, -1};
float sSelX = 0.f;
float sSelY = 0.f;
// The console's `radial`: a menu held and its selection, -1 for none.
std::atomic<int> sForced{-1};
std::atomic<float> sForcedX{0.f};
std::atomic<float> sForcedY{0.f};
// SetPoint's latest, until Tick takes it.
std::atomic<bool> sPointSet{false};
std::atomic<float> sPointX{0.f};
std::atomic<float> sPointY{0.f};

void Clamp() {
  const float len = std::sqrt(sSelX * sSelX + sSelY * sSelY);
  if (len > 1.f) {
    sSelX /= len;
    sSelY /= len;
  }
}

} // namespace

void Tick(bool beamHeld, bool visorHeld, float cx, float cy) {
  const int forced = sForced;
  beamHeld = beamHeld || forced == kBeam;
  visorHeld = visorHeld || forced == kVisor;
  const bool wasHeld = sHeld[kBeam] || sHeld[kVisor];
  // A menu opening starts from the centre (OnOpen resets the Wii selection too).
  if ((beamHeld && !sHeld[kBeam]) || (visorHeld && !sHeld[kVisor])) {
    if (!wasHeld && !sOpen[kBeam] && !sOpen[kVisor]) sSelX = sSelY = 0.f;
  }
  sHeld[kBeam] = beamHeld;
  sHeld[kVisor] = visorHeld;
  // A touch menu button's drag; taken here, before the release this poll may bring is picked.
  if (sPointSet.exchange(false) && Capturing()) {
    sSelX = sPointX;
    sSelY = sPointY;
    Clamp();
  }
  if (Capturing() && cx * cx + cy * cy > kStickPoint * kStickPoint) {
    sSelX = cx;
    sSelY = cy;
    Clamp();
  }
  if (forced >= 0) {
    sSelX = sForcedX;
    sSelY = sForcedY;
    Clamp();
  }
}

void Force(int kind, float x, float y) {
  sForcedX = x;
  sForcedY = y;
  sForced = kind;
}

bool Held(Kind kind) { return sHeld[kind]; }

void SetState(bool available, bool visorOpen, bool beamOpen) {
  sAvailable = available;
  sOpen[kVisor] = available && visorOpen;
  sOpen[kBeam] = available && beamOpen;
  if (!available) sPick[kVisor] = sPick[kBeam] = -1;
}

bool Capturing() { return sAvailable && (sHeld[0] || sHeld[1] || sOpen[0] || sOpen[1]); }

void AddTravel(float dx, float dy) {
  sSelX += dx / kTravelFull;
  sSelY -= dy / kTravelFull;
  Clamp();
}

void SetPoint(float x, float y) {
  sPointX = x;
  sPointY = y;
  sPointSet = true;
}

void GetSelection(float& x, float& y) {
  x = sSelX;
  y = sSelY;
}

void Pick(Kind kind, int id) { sPick[kind] = id; }

int TakePick(Kind kind) { return sPick[kind].exchange(-1); }

} // namespace PortRadial
