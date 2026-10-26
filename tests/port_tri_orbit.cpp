#include "port_tri_orbit.h"
#include "port_test.h"

namespace {

bool Near(float a, float b) { return a > b - 0.0001f && a < b + 0.0001f; }

void TestAspectK() {
  CHECK(Near(PortTriOrbit::AspectK(4.f / 3.f), 1.f));
  CHECK(Near(PortTriOrbit::AspectK(1.f), 1.f));
  CHECK(Near(PortTriOrbit::AspectK(16.f / 9.f), 1.246f));
  CHECK(Near(PortTriOrbit::AspectK(21.f / 9.f), 1.246f));
  const float mid = PortTriOrbit::AspectK((4.f / 3.f + 16.f / 9.f) * 0.5f);
  CHECK(Near(mid, 1.123f));
}

void TestExtents() {
  // Float fmuls then fctiwz: 0.65f * 180 is 116.99999, which truncates to 116.
  CHECK_EQ(PortTriOrbit::HalfHeight(180.f), 116);
  CHECK_EQ(PortTriOrbit::HalfHeight(100.f), 65);
  CHECK_EQ(PortTriOrbit::HalfWidth(200.f, 1.f), 130);
  CHECK_EQ(PortTriOrbit::HalfWidth(200.f, 1.246f), 104); // 130 / 1.246 = 104.3
  CHECK_EQ(PortTriOrbit::HalfWidth(100.f, 1.246f), 52);  // 52.17
}

void TestPointerCentre() {
  // Centre of every zone, scan and combat, is the pointer; no pointer = viewport centre.
  CHECK_EQ(PortTriOrbit::PointerCentre(0.f, 640.f), 320);
  CHECK_EQ(PortTriOrbit::PointerCentre(0.f, 1920.f), 960);
  CHECK_EQ(PortTriOrbit::PointerCentre(0.f, 448.f), 224);
  CHECK_EQ(PortTriOrbit::PointerCentre(-1.f, 640.f), 0);
  CHECK_EQ(PortTriOrbit::PointerCentre(1.f, 640.f), 640);
  CHECK_EQ(PortTriOrbit::PointerCentre(0.5f, 641.f), 480); // 480.75 truncates
}

void TestFadeTimer() {
  const float dur = 0.28f;
  float t = 0.f;
  t = PortTriOrbit::FadeTimerStep(t, true, 0.1f, dur);
  CHECK(Near(t, 0.1f));
  CHECK(Near(PortTriOrbit::FadeRatio(t, true, dur), 0.1f / dur));
  t = PortTriOrbit::FadeTimerStep(t, true, 1.f, dur);
  CHECK(Near(t, dur)); // clamps
  CHECK(Near(PortTriOrbit::FadeRatio(t, true, dur), 1.f));
  t = PortTriOrbit::FadeTimerStep(t, false, 0.1f, dur);
  CHECK(Near(t, 0.18f)); // decays
  t = PortTriOrbit::FadeTimerStep(t, false, 1.f, dur);
  CHECK(Near(t, 0.f)); // floors
  CHECK(Near(PortTriOrbit::FadeRatio(t, false, dur), 0.f));
  CHECK(Near(PortTriOrbit::FadeRatio(-1.f, false, dur), 0.f));
  CHECK(Near(PortTriOrbit::FadeRatio(5.f, true, dur), 1.f));
}

void TestNonPositiveDuration() {
  CHECK(Near(PortTriOrbit::FadeTimerStep(0.f, true, 0.1f, 0.f), 0.f));
  CHECK(Near(PortTriOrbit::FadeTimerStep(0.5f, false, 0.1f, -1.f), 0.f));
  CHECK(Near(PortTriOrbit::FadeRatio(0.f, true, 0.f), 1.f));
  CHECK(Near(PortTriOrbit::FadeRatio(0.f, false, 0.f), 0.f));
  CHECK(Near(PortTriOrbit::FadeRatio(0.f, true, -1.f), 1.f));
}

} // namespace

int main() {
  TestAspectK();
  TestExtents();
  TestPointerCentre();
  TestFadeTimer();
  TestNonPositiveDuration();
  return PORT_TEST_RESULT();
}
