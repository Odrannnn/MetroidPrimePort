#include "port_clustered_lights.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace PortClusteredLights;

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool Same(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// An identity view: world and view space coincide, which keeps the expected values readable.
const float kIdentityView[3][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};

Camera TestCamera() {
  // A 16:9-ish frustum: tanHalfX = 16/9 * tanHalfY, as MakeCamera would give from a projection.
  Camera camera;
  std::memcpy(camera.worldToView, kIdentityView, sizeof(camera.worldToView));
  camera.tanHalfX = 0.9f;
  camera.tanHalfY = 0.5f;
  camera.nearZ = 0.1f;
  camera.farZ = kMaxDepth;
  return camera;
}

LightSource PointLight(float x, float y, float z, float farD) {
  LightSource light;
  light.color[0] = 1.f;
  light.color[1] = 0.5f;
  light.color[2] = 0.25f;
  light.pos[0] = x;
  light.pos[1] = y;
  light.pos[2] = z;
  light.nearD = 0.25f;
  light.farD = farD;
  light.falloff = 3;
  return light;
}

} // namespace

int main() {
  // The grid: 48 px tiles, capped 34 x 19 (SetupClusterContextBase 0x3b43a4).
  {
    const Grid hd = MakeGrid(1280, 720);
    CHECK_MSG(hd.tilesX == 27 && hd.tilesY == 15, "1280x720 -> 27x15 tiles (ceil 26.7 x 15)");
    CHECK_MSG(hd.Clusters() == 27 * 15 * kSlices, "one cluster per tile per slice");
    const Grid full = MakeGrid(1920, 1080);
    CHECK_MSG(full.tilesX == kMaxTilesX && full.tilesY == kMaxTilesY, "1920x1080 caps at 34x19");
    const Grid tiny = MakeGrid(0, 0);
    CHECK_MSG(tiny.tilesX == 0 && tiny.tilesY == 0, "a zero viewport has no tiles");
    const Grid edge = MakeGrid(48, 48);
    CHECK_MSG(edge.tilesX == 1 && edge.tilesY == 1, "exactly one tile");
  }

  // The slice: trunc(clamp(depth, 0, 500) * 32/500), clamped to the last slice.
  {
    CHECK_MSG(SliceForDepth(0.f) == 0, "depth 0 is slice 0");
    CHECK_MSG(SliceForDepth(15.f) == 0, "15.0 is still slice 0 (0.96)");
    CHECK_MSG(SliceForDepth(16.f) == 1, "16.0 is slice 1 (1.024)");
    CHECK_MSG(SliceForDepth(kSliceDepth) == 1, "one slice width in is slice 1");
    CHECK_MSG(SliceForDepth(-5.f) == 0, "behind the eye clamps to 0");
    CHECK_MSG(SliceForDepth(499.f) == 31, "just inside the range is the last slice");
    CHECK_MSG(SliceForDepth(5000.f) == 31, "past the range stays on the last slice");
    CHECK_MSG(SliceForViewZ(-16.f) == 1, "view z -16 is depth 16");
    CHECK_MSG(SliceForDepth(16.f, 15.f) == 0, "the depth origin shifts the slice");
  }

  // The header the shader reads (c5[0..2]).
  {
    float header[kHeaderFloats];
    Header(MakeGrid(1280, 720), header, 0x5u, 0.f);
    CHECK_MSG(Near(header[0], 1.f / 48.f) && Near(header[1], 1.f / 48.f), "c5[0].xy = 1 / 48");
    CHECK_MSG(Near(header[2], 27.f) && Near(header[3], 15.f), "c5[0].zw = tilesX, tilesY");
    CHECK_MSG(Near(header[4], 1.f), "c5[1].x = 1 / the depth exponent");
    CHECK_MSG(Near(header[5], 32.f / 500.f), "c5[1].y = slices / maxDepth");
    CHECK_MSG(Near(header[6], 0.f) && Near(header[7], 500.f), "c5[1].z = origin, .w = maxDepth");
    uint32_t tiles = 0, across = 0, validity = 0, source = 1;
    std::memcpy(&tiles, &header[8], 4);
    std::memcpy(&across, &header[9], 4);
    std::memcpy(&validity, &header[10], 4);
    std::memcpy(&source, &header[11], 4);
    CHECK_MSG(tiles == 27u * 15u && across == 27u, "c5[2].xy hold tile count and row pitch as int bits");
    CHECK_MSG(validity == 0x5u, "c5[2].z is the validity mask");
    CHECK_MSG(source == 0u, "c5[2].w = 0, the mask is a texture");
  }

  // The 80-byte record (SCachedLight::Initialize 0x16b5c8), for a point light at view z -20.
  {
    const LightSource source = PointLight(0.f, 0.f, -20.f, 20.25f);
    const RawLight light = MakeLight(source, kIdentityView);
    CHECK_MSG(Near(light.record[0], 1.f) && Near(light.record[1], 0.5f) && Near(light.record[2], 0.25f),
              "+0x00 colour rgb, as given (no exposure, no 1/pi)");
    CHECK_MSG(Near(light.record[3], 0.f), "+0x0c widen is 0 for a script light");
    CHECK_MSG(Near(light.record[4], 0.f) && Near(light.record[5], 0.f) && Near(light.record[6], -20.f),
              "+0x10 the view position");
    CHECK_MSG(Near(light.record[7], 0.f), "+0x1c a point light's cone scale is 0");
    CHECK_MSG(Near(light.record[8], 0.f) && Near(light.record[9], 0.f) && Near(light.record[10], 0.f),
              "+0x20 a point light's direction is zero");
    CHECK_MSG(Near(light.record[11], 1.f), "+0x2c a point light's cone bias is 1");
    CHECK_MSG(Near(light.record[12], 1.f / 20.f), "+0x30 A = 1/(far-near)");
    CHECK_MSG(Near(light.record[13], -0.25f / 20.f), "+0x34 B = -near/(far-near)");
    CHECK_MSG(Near(light.record[14], 0.f) && Near(light.record[15], 0.f), "+0x38 is padding");
    CHECK_MSG(Near(light.record[16], 2.f) && Near(light.record[17], -3.f) && Near(light.record[18], 0.f) &&
                  Near(light.record[19], 1.f),
              "+0x40 the smooth falloff cubic (a3,a2,a1,a0)");
    CHECK_MSG(Near(light.radius, 20.25f), "the influence radius is the far distance");
  }

  // A spot's cone and the four falloff rows.
  {
    LightSource spot = PointLight(0.f, 0.f, -10.f, 10.f);
    spot.spot = true;
    spot.innerDeg = 30.f;
    spot.outerDeg = 60.f;
    spot.toLight[2] = 1.f;
    spot.falloff = 1;
    const RawLight light = MakeLight(spot, kIdentityView);
    // cos(15deg) = .9659, cos(30deg) = .8660; span .0999; scale 10.01, bias -8.669.
    CHECK_MSG(Near(light.record[7], 1.f / (std::cos(15.f * 3.14159265f / 180.f) -
                                          std::cos(30.f * 3.14159265f / 180.f)),
                   1e-3f),
              "a spot's cone scale is 1/(cosIn - cosOut)");
    CHECK_MSG(Near(light.record[10], 1.f), "the spot's direction passes through the view rows");
    CHECK_MSG(Near(light.record[16], 0.f) && Near(light.record[17], 0.f) && Near(light.record[18], -1.f) &&
                  Near(light.record[19], 1.f),
              "falloff type 1 is (0,0,-1,1)");
    float cubic[4];
    FalloffCubic(0, cubic);
    CHECK_MSG(Near(cubic[3], 1.f) && Near(cubic[2], 0.f), "type 0 is constant 1");
    FalloffCubic(2, cubic);
    CHECK_MSG(Near(cubic[1], 1.f) && Near(cubic[2], -2.f), "type 2 is (1-t)^2");
    FalloffCubic(9, cubic);
    CHECK_MSG(Near(cubic[0], 2.f), "an unknown type clamps to the smooth row");
  }

  // Equal/reversed cones retain a lit centre, rather than becoming black. Source
  // FromValues widens their half-angle by 0.001 radians, including after angle wrapping.
  {
    float scale, bias;
    for (float outer : {60.f, 30.f}) {
      ConeAttenuation(true, 60.f, outer, scale, bias);
      CHECK(scale > 1000.f && std::isfinite(scale));
      CHECK(std::clamp(scale + bias, 0.f, 1.f) == 1.f);
      CHECK(std::clamp(std::cos(31.f * 3.14159265f / 180.f) * scale + bias, 0.f, 1.f) == 0.f);
    }
    float referenceScale, referenceBias;
    ConeAttenuation(true, 60.f, 120.f, referenceScale, referenceBias);
    for (float shift : {-720.f, 720.f}) {
      ConeAttenuation(true, 60.f + shift, 120.f + shift, scale, bias);
      CHECK(Near(scale, referenceScale, 1e-4f) && Near(bias, referenceBias, 1e-4f));
    }
    ConeAttenuation(false, 60.f, 30.f, scale, bias);
    CHECK(scale == 0.f && bias == 1.f);
  }

  // A light in the middle of the view sets the centre cluster of its own slice and nothing else.
  {
    const Camera camera = TestCamera();
    const LightSource source = PointLight(0.f, 0.f, -20.f, 2.f); // a 2 m influence sphere
    const RawLight light[1] = {MakeLight(source, kIdentityView)};
    Frame frame;
    Build(camera, 1280, 720, light, 1, frame);
    CHECK_MSG(frame.lightCount == 1, "the light is in the list");
    CHECK_MSG(frame.mask.size() == size_t(frame.grid.Clusters()), "one mask word per cluster");
    const int slice = SliceForDepth(20.f);
    CHECK_MSG(slice == 1, "the light sits in slice 1");
    CHECK_MSG(frame.Mask(13, 7, slice) == 1u, "the centre tile of that slice carries bit 0");
    CHECK_MSG(frame.Mask(0, 0, slice) == 0u, "a corner tile does not");
    CHECK_MSG(frame.Mask(13, 7, 20) == 0u, "a far slice does not");
    CHECK_MSG(frame.SetBits() >= 1, "at least one (cluster, light) pair is set");
  }

  // The culls: black, zero radius, beyond the distance cut, and outside the frustum.
  {
    const Camera camera = TestCamera();
    LightSource black = PointLight(0.f, 0.f, -20.f, 22.f);
    black.color[0] = black.color[1] = black.color[2] = 0.f;
    LightSource noRadius = PointLight(0.f, 0.f, -20.f, 0.f);
    LightSource tooFar = PointLight(0.f, 0.f, -400.f, 5.f); // no distance cut: kept (see header)
    LightSource offScreen = PointLight(60.f, 0.f, -20.f, 2.f);
    LightSource good = PointLight(0.f, 0.f, -20.f, 22.f);
    const LightSource sources[5] = {black, noRadius, tooFar, offScreen, good};
    RawLight lights[5];
    for (int i = 0; i < 5; ++i) {
      lights[i] = MakeLight(sources[i], kIdentityView);
    }
    lights[1].radius = 0.f; // FromValues never leaves a light at zero; the cull still checks it
    Frame frame;
    Build(camera, 1280, 720, lights, 5, frame);
    CHECK_MSG(frame.lightCount == 2, "black, zero-radius and off-screen are dropped; the rest stay");
    CHECK_MSG(Near(frame.Light(0)[6], -400.f) && Near(frame.Light(1)[6], -20.f),
              "the survivors keep the input order");
  }

  // The cap: one 32-bit word per cluster holds at most 32 lights.
  {
    const Camera camera = TestCamera();
    RawLight lights[40];
    for (int i = 0; i < 40; ++i) {
      lights[i] = MakeLight(PointLight(0.f, 0.f, -20.f, 22.f), kIdentityView);
    }
    Frame frame;
    Build(camera, 1280, 720, lights, 40, frame);
    CHECK_MSG(frame.lightCount == kMaxLights, "the list caps at 32");
    const int slice = SliceForDepth(20.f);
    CHECK_MSG(frame.Mask(13, 7, slice) == 0xFFFFFFFFu, "the centre cluster's word is full");
  }

  // An empty frame: no lights still gives a grid and a cleared mask.
  {
    const Camera camera = TestCamera();
    Frame frame;
    Build(camera, 1280, 720, nullptr, 0, frame);
    CHECK_MSG(frame.lightCount == 0 && frame.SetBits() == 0, "no lights, no bits");
    CHECK_MSG(frame.grid.tilesX == 27, "the grid is still built");
  }

  if (port_test::Failures() == 0) {
    std::puts("clustered light checks passed");
  }
  return PORT_TEST_RESULT();
}
