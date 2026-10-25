// The third-person body's clip choice (platform/include/port_third_person_body.h).
#include "port_test.h"
#include "port_third_person_body.h"

using namespace PortThirdPersonBody;

int main() {
  CHECK(ChooseClip(0.f, 0.f, true).clip == Clip::Idle);
  CHECK(ChooseClip(0.2f, 0.1f, true).clip == Clip::Idle);
  CHECK(ChooseClip(2.f, 0.f, true).clip == Clip::SlowWalk);
  CHECK(ChooseClip(5.f, 0.f, true).clip == Clip::Walk);
  CHECK(ChooseClip(9.f, 0.f, true).clip == Clip::Run);
  CHECK(ChooseClip(-5.f, 0.f, true).clip == Clip::Backward);
  CHECK(ChooseClip(0.5f, 5.f, true).clip == Clip::Hold); // strafing
  CHECK(ChooseClip(9.f, 0.f, false).clip == Clip::Hold); // airborne
  CHECK(ChooseClip(2.f, 0.f, true).rate >= 0.5f && ChooseClip(30.f, 0.f, true).rate <= 1.6f);
  // The cannon raised: forward at any speed walks with it; backward and standing are unchanged.
  CHECK(ChooseClip(2.f, 0.f, true, true).clip == Clip::WalkGun);
  CHECK(ChooseClip(9.f, 0.f, true, true).clip == Clip::WalkGun);
  CHECK(ChooseClip(-5.f, 0.f, true, true).clip == Clip::Backward);
  CHECK(ChooseClip(0.f, 0.f, true, true).clip == Clip::Idle);
  // Stopping from a walk plays the stop clip to its end, unless the body moves again.
  const Choice idle = ChooseClip(0.f, 0.f, true);
  CHECK(Settle(idle, Clip::Walk, true).clip == Clip::Stop);
  CHECK(Settle(idle, Clip::WalkGun, true).clip == Clip::Stop);
  CHECK(Settle(idle, Clip::Stop, false).clip == Clip::Stop);
  CHECK(Settle(idle, Clip::Stop, true).clip == Clip::Idle);
  CHECK(Settle(idle, Clip::Backward, true).clip == Clip::Idle);
  CHECK(Settle(idle, Clip::Hold, true).clip == Clip::Idle);
  CHECK(Settle(ChooseClip(5.f, 0.f, true), Clip::Stop, false).clip == Clip::Walk);
  return 0;
}
