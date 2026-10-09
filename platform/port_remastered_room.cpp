// Room environment writer. See port_remastered_room.h.
//
// A port of build/mpr/roomtools/envwrite.py (with roomlib.py, gcres.py and
// ltpb.cpp). Everything read here is untrusted file content: every read is
// bounds checked and a malformed file fails its room, never the process.
#include "port_env.h"
#include "port_remastered_room.h"
#include "port_bytes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>

#include "port_maya_spline.h"
#include "port_remastered_anim.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_txtr.h"
#include "port_room_env.h"
#include "port_room_geo.h"

namespace PortRemastered {
namespace {

using Vec3 = std::array<double, 3>;
using Mat34 = std::array<std::array<double, 4>, 3>;
using Id16 = std::array<uint8_t, 16>;

constexpr uint32_t Tag(const char (&s)[5]) {
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) |
         uint32_t(uint8_t(s[3]));
}

// Component type ids (retrotool's root.json).
constexpr uint32_t kEntity = 0x749749f1;
constexpr uint32_t kRoomController = 0x83cc17aa;
constexpr uint32_t kTonemap = 0xddea916d;
constexpr uint32_t kReflectionProbe = 0x27807e39;
constexpr uint32_t kAutoExposureHint = 0x98694074;
constexpr uint32_t kBloomEffect = 0x7dcaf170;
constexpr uint32_t kColorGrade = 0x6b091e44;
constexpr uint32_t kColorGradeHint = 0xa36cd908;
constexpr uint32_t kBacklight = 0x190d20d7;
constexpr uint32_t kVolumetricFogHint = 0x84fb5798;
constexpr uint32_t kVolumetricFog = 0x1b9cd84f;
constexpr uint32_t kVolumetricFogRegion = 0xaffe9cf9;
constexpr uint32_t kVolumetricFogRegionTransition = 0xdf95ac1a;
constexpr uint32_t kDoorMP1 = 0x564a1641;
constexpr uint32_t kModCon = 0x451740eb;
constexpr uint32_t kActorMP1 = 0xb6200be6;
constexpr uint32_t kLightDynamic = 0x9ee5541d;

// Property ids.
constexpr uint32_t kPropRoomId = 0x30a4d63d;
constexpr uint32_t kPropProbeRefl = 0x020e5559;
// A probe's padding (blend distance, default 1), priority (s32), intensity min (0) and max (1)
// (CReflectionProbe's loader).
constexpr uint32_t kPropProbePadding = 0x422b37b8;
constexpr uint32_t kPropProbePriority = 0x0e68307f;
constexpr uint32_t kPropProbeMin = 0x362216cf;
constexpr uint32_t kPropProbeMax = 0xcd8ea2be;
// Exposure value, middle grey, toe, shoulder, contrast (SLdrTonemap::Load).
constexpr uint32_t kPropTonemap[5] = {0x44a2e298, 0x34bb937d, 0x49ee7747, 0x295132dd, 0x3373d845};
// Bloom threshold, and a u32 count of RGBA float tints (SLdrBloomEffect).
constexpr uint32_t kPropBloomThreshold = 0xa8e016d1;
constexpr uint32_t kPropBloomTints = 0x033f2e2c;
// A ColorGrade's 33^3 LUT (TXTR), and the hint on the same entity: global flag, fade times.
constexpr uint32_t kPropGradeLut = 0x59a7df11;
constexpr uint32_t kPropHintGlobal = 0x6182fd5e;
constexpr uint32_t kPropHintFadeIn = 0x5c6f53d8;
constexpr uint32_t kPropHintFadeOut = 0xa4967532;
constexpr uint32_t kPropHintPriority[2] = {0xb3d40a89, 0x8ab8fd40};  // default 50
constexpr uint32_t kGradeLutSize = 33;
// A Backlight is its own hint (CBacklightManager): the character backlight's top and back
// coefficients (1 when absent), fades (1 s), global flag and priority (the base struct's, 50).
constexpr uint32_t kPropBacklightTop = 0x059e985d;
constexpr uint32_t kPropBacklightBack = 0xaa1a5fd9;
constexpr uint32_t kPropBacklightFadeIn = 0x15245fd2;
constexpr uint32_t kPropBacklightFadeOut = 0x4c97e999;
constexpr uint32_t kPropBacklightGlobal = 0xea3e22c0;
constexpr uint32_t kPropBacklightPriority[2] = {0x2833c9d2, 0x8ab8fd40};
// A VolumetricFogHint and the VolumetricFog on its entity (CVolumetricFog; defaults from
// build/mpr/volfog/A-properties.md). The hint: auto-request flag (1), priority, the fade
// interpolations (a Time interpolation holding the CMayaSpline from elapsed time to phase;
// none: the fog changes at once).
constexpr uint32_t kPropFogAuto = 0x2928fabd;
constexpr uint32_t kPropFogPriority[2] = {0xf49eb82a, 0x8ab8fd40};
constexpr uint32_t kPropFogFadeIn = 0x1666a3dc;
constexpr uint32_t kPropFogFadeOut = 0x7528254e;
constexpr uint32_t kPropFogSpline = 0x1eb6e23f;
constexpr uint32_t kPropFogRange = 0xf259966e;
// SLdrVolumetricFogRegion's properties and enums (build/mpr/volfog/E-regions-exact.md).
constexpr uint32_t kPropRegionColor = 0xc35e3f33;
// LightDynamic's (build/mpr/ldyn/ldyn.py).
constexpr uint32_t kPropLightType = 0x932dd7d8;
constexpr uint32_t kPropLightColor[2] = {0xb73ed9c3, 0x8f421907};
constexpr uint32_t kPropLightIntensity[2] = {0xc3eabcbf, 0xbdfc8a9e};
constexpr uint32_t kLightDirectional = 1;
constexpr uint32_t kLightPoint = 2;
constexpr uint32_t kLightSpot = 3;
constexpr uint32_t kPropLightShown = 0x2c66744c;  // u8, default 1
constexpr uint32_t kPropLightDistance = 0x76df827d;  // .kind (enum), .near, .far
constexpr uint32_t kPropLightDistanceKind = 0x174c8abc;
constexpr uint32_t kPropLightDistanceNear = 0xe5ceaf7c;
constexpr uint32_t kPropLightDistanceFar = 0x68ac20b3;
constexpr uint32_t kLightFalloffSquared = 0x5e3252a0;  // (1-t)^2
constexpr uint32_t kLightFalloffLinear = 0xf75d18cf;   // 1-t; any other value is the smooth cubic
constexpr uint32_t kPropLightCone = 0xf5125ed1;  // .outer, .inner: full angles in degrees
constexpr uint32_t kPropLightConeOuter = 0xb0a71764;
constexpr uint32_t kPropLightConeInner = 0x635dfcc7;
// The animated path (kb func/light-dynamic.md): Attributes .length (f32 s), .animated, .playing
// (at load), .loop (u8 each), and the CMayaSplines over the timeline: intensity, near and far
// (with their own falloff enum, smooth when absent), inner and outer cone, and the colour, a
// spline into a CColorGradient (u32 4, then four CMayaSplines R, G, B, A).
constexpr uint32_t kPropLightAttributes = 0x31f63c93;
constexpr uint32_t kPropLightLength = 0xdd24b4b2;
constexpr uint32_t kPropLightAnimated = 0x4440e44c;
constexpr uint32_t kPropLightPlaying = 0xc5e97577;
constexpr uint32_t kPropLightLoop = 0x319a91c2;
constexpr uint32_t kPropLightIntensitySpline[2] = {0xe8a62a55, 0xef7256b5};
constexpr uint32_t kPropLightDistanceDynamic = 0xa327083f;  // .kind, .near, .far
constexpr uint32_t kPropLightDistanceDynamicKind = 0xb234567c;
constexpr uint32_t kPropLightDistanceDynamicNear = 0xe15a6749;
constexpr uint32_t kPropLightDistanceDynamicFar = 0xba147368;
constexpr uint32_t kPropLightConeDynamic = 0xe2fe5508;  // .inner, .outer
constexpr uint32_t kPropLightConeDynamicInner = 0xa306f8b6;
constexpr uint32_t kPropLightConeDynamicOuter = 0xca7e1d6f;
constexpr uint32_t kPropLightColorDynamic = 0xe9141c0f;  // .spline, .gradient
constexpr uint32_t kPropLightColorDynamicSpline = 0x085586c6;
constexpr uint32_t kPropLightColorDynamicGradient = 0xe6222427;
// What a LightDynamic sends when a backward, non-looping play reaches t = 0 (CLightDynamicGOC::Think,
// CTimePlaybackManager::Update result 1). The forward end sends c6ef90ff, which no room links.
constexpr uint32_t kEventLightReachedStart = 0xeb2a4a08;
constexpr uint32_t kPropRegionMode = 0xd4aa2ccb;
constexpr uint32_t kPropRegionDistance = 0xbaf7ac02;
constexpr uint32_t kPropRegionTransmittance = 0xd1fc0ce8;
constexpr uint32_t kPropRegionIntensity = 0x41dcfe38;
constexpr uint32_t kPropRegionCap = 0x1c44e1d4;
constexpr uint32_t kPropRegionEdge = 0x46b11b67;
constexpr uint32_t kPropRegionFluid = 0x5eab5677;
// SLdrVolumetricFogRegionTransition (0x5e59c8): Options (auto-start, loop), Selections (which
// targets it moves: distance, transmittance, colour, cap; absent = 0), the phase spline, the
// target colour (1,1,1,1) scaled by its intensity (1), distance (250), transmittance (0.01)
// and cap (0). The two Options are only ever set together, so which is which is moot.
constexpr uint32_t kPropTransitionOptions = 0xfb469204;
constexpr uint32_t kPropTransitionAutoStart = 0x1e50d67f;
constexpr uint32_t kPropTransitionLoop = 0xf6ff0b7b;
constexpr uint32_t kPropTransitionSelect = 0xea16bcb9;
constexpr uint32_t kPropTransitionSelects[4] = {0x53bc68b9, 0xe744f258, 0x63f8e57f, 0x04ba6886};
constexpr uint32_t kPropTransitionPhase = 0x342d67cf;
constexpr uint32_t kPropTransitionColor = 0x1cb90daf;
constexpr uint32_t kPropTransitionDistance = 0x553bab47;
constexpr uint32_t kPropTransitionTransmittance = 0xcc1dc65a;
constexpr uint32_t kPropTransitionIntensity = 0xce534ba6;
constexpr uint32_t kPropTransitionCap = 0x08e48e96;
constexpr uint32_t kRegionSubtract = 0x3f843a1a;
constexpr uint32_t kRegionOverride = 0x97699d49;
constexpr uint32_t kRegionInsideFluid = 0xb33d20b9;
constexpr uint32_t kRegionOutsideFluid = 0x4ccba47b;
constexpr uint32_t kPropFogResidual = 0x33cd9d58;
constexpr uint32_t kPropFogNoiseScale = 0x9c1e9f8f;
constexpr uint32_t kPropFogNoiseStrength = 0xbf9b3481;
constexpr uint32_t kPropFogColorA = 0xdac710d4;
constexpr uint32_t kPropFogIntensity = 0xe020e7c4;
constexpr uint32_t kPropFogLightCap = 0x2074d13d;
constexpr uint32_t kPropFogScatter = 0x22096751;
constexpr uint32_t kPropFogAbsorb = 0x95e259b4;
constexpr uint32_t kPropFogM1z = 0xdd36a237;
constexpr uint32_t kPropFogNoProbe = 0xe3b56cab;
constexpr uint32_t kPropFogColorB = 0x3029e6aa;
constexpr uint32_t kPropFogLut = 0x7bc5ef27;
constexpr uint32_t kPropFogAtten = 0xac68c5ae;  // a, b (height range), enable
constexpr uint32_t kPropFogAttenA = 0x95a9aa57;
constexpr uint32_t kPropFogAttenB = 0x719bec17;
constexpr uint32_t kPropFogAttenOn = 0x12a756f5;
constexpr uint32_t kPropFogWind = 0x8efd902b;  // vector, "use this vector" flag
constexpr uint32_t kPropFogWindVec = 0x0ad3c808;
constexpr uint32_t kPropFogWindOn = 0x33c69fd5;
constexpr uint32_t kPropHintMin = 0x682f8a1f;
constexpr uint32_t kPropHintMax = 0x839d334c;
constexpr uint32_t kPropHintMode = 0x590d6843;
constexpr uint32_t kPropHintBias = 0x038f85da;
constexpr uint32_t kPropHintSigma = 0x3dace129;
constexpr uint32_t kPropHintStaticLerp = 0xb9f6606a;
constexpr uint32_t kPropModConMcon = 0xa8e2ba93;
constexpr uint32_t kPropActorModel = 0xcb1c52f6;
// Unnamed in retrotool's templates; what they mean is read off which actors carry them.
constexpr uint32_t kPropActorAdded = 0x9a25df3b;
// The actor's animation: a character (CHPR) and the name of the animation it plays, as an
// offset and length into the room's string pool. The spinning rings of the Intro Elevator
// have one and no model; the character's skinned model is what they draw.
constexpr uint32_t kPropActorAnim = 0x54446d42;
constexpr uint32_t kPropAnimCharacter = 0xa589d885;
constexpr uint32_t kPropAnimName = 0x87c03a01;
// A room's sky: its model, drawn about the camera and turned and scaled by its entity's
// transform, and lit by its colour times its intensity (SLdrSkybox: 1, 1, 1, 1 and 1 when
// left out), which CSkyboxSceneNode puts in place of the materials' DIFC.
constexpr uint32_t kSkybox = 0x5112a065;
constexpr uint32_t kPropSkyboxModel = 0x387bb786;
constexpr uint32_t kPropSkyboxIntensity = 0x63328a04;
constexpr uint32_t kPropSkyboxColor = 0x34184350;
// A ColorModulateMP1 in its incandescence mode (blend 5, CColorModulateMP1GOC): what it
// targets glows in its colour B times its intensity, which takes the place of every
// material's ICNC. Each door frame has one, with times of 0, so B applies from the start.
constexpr uint32_t kColorModulateMP1 = 0xa856f484;
constexpr uint32_t kPropModulateBlend = 0x3d76c67c;
constexpr uint32_t kModulateIncandescence = 0x754d6cdc;
constexpr uint32_t kPropModulateColorB = 0x3f7113d9;
// SLdrColor_MP1Typedef sub-properties in struct order (r, g, b, a; Load at 0xd1e3ec).
constexpr uint32_t kPropColorRGBA[4] = {0x110889d1, 0x8a7aff22, 0x2a5349e9, 0xe364c93a};
constexpr uint32_t kPropColorChannel[3] = {kPropColorRGBA[0], kPropColorRGBA[1], kPropColorRGBA[2]};
constexpr uint32_t kPropModulateIntensity = 0xf936079e;
// Liquids. A WaterMP1 is the retail water object; the surface drawn for it is a render
// volume on the same entity.
constexpr uint32_t kWaterMP1 = 0x12db855d;
constexpr uint32_t kWaterRenderVolume = 0x23c5dff4;
constexpr uint32_t kLavaRenderVolume = 0xa7ee9c33;
constexpr uint32_t kPropWaterFluid[3] = {0xce78300b, 0x18706e5c, 0x6e9e14b9};  // 0 water, 10 poison, 11 lava
// SLdrWaterMP1+0x50: the camera filter colour (CScriptWaterMP1+0x520) that
// CCameraManagerMP1::UpdateFilters multiplies the screen by while the camera is inside.
constexpr uint32_t kPropWaterFilterColor = 0xcc1af173;
// SLdrWaterMP1+0x18C (CScriptWaterMP1+0x548): the surface's tint alpha is multiplied by it in
// the X-Ray visor (build/mpr/water/H-opacity.md).
constexpr uint32_t kPropWaterXrayOpacity = 0x13264102;
constexpr uint32_t kPropWaterModel = 0x736e5890;
constexpr uint32_t kPropLavaModel = 0xcaf8e8c3;
// RoomLiquid::lava's fields, in order (build/mpr/water/I-lava-cpu.md Q6; the ids were read off
// SLdrLavaRenderVolume::Load's stores). 4b27a58d and 8dce5669 only go to the fluid sim.
constexpr uint32_t kPropLava[6] = {0x82330004, 0x30a47f17, 0x4ec7028f, 0x36062e0f, 0xe7993ff7, 0xfb6e9dc6};
constexpr uint32_t kPropWaterLook = 0xd1e9d29d;
constexpr uint32_t kPropWaterFeatures = 0x54f39685;
constexpr uint32_t kPropWaterFeature[11] = {0x8b294d2e, 0x68999d0e, 0xd7f0419b, 0xbf2c11db, 0xf2d5d4be, 0xfcfce6e6,
                                            0x54ad680d, 0x1878894a, 0xf0965f0d, 0x8c79533e, 0x802d7816};
constexpr uint32_t kPropWaterWaves[2] = {0x30fbb790, 0x0951bf3e};
constexpr uint32_t kPropWave[5] = {0xd1edd7b5, 0xabe1bacd, 0xf6266364, 0x4574de4f, 0x921415eb};
constexpr uint32_t kPropWaterTint = 0xe8969fad;
constexpr uint32_t kPropWaterNormalMap = 0x03e33f4b;
constexpr uint32_t kPropWaterNormalTexture = 0x90a143ef;
constexpr uint32_t kPropWaterNormalDir = 0x138db2f0;
constexpr uint32_t kPropWaterNormalDirXY[2] = {0xb9303984, 0x51f42492};
constexpr uint32_t kPropWaterNormalSpeed = 0x522abd8a;
constexpr uint32_t kPropWaterNormalScale = 0xd4483c7e;
constexpr uint32_t kPropWaterFog = 0x7982e07b;
constexpr uint32_t kPropWaterFogColor = 0x6f7355a3;
constexpr uint32_t kPropWaterFogDistance = 0x3b29e512;
constexpr uint32_t kPropWaterRain = 0x1a6aac69;
constexpr uint32_t kPropWaterRainNoise = 0xa61e08d3;
constexpr uint32_t kPropWaterRainValue[10] = {0xe4732770, 0x5e94c719, 0xe89f4385, 0xc5dad5ec, 0xbc212b78,
                                              0x7b56b6dd, 0x3bda1181, 0x6e5a8b34, 0x851d5509, 0x53160e29};
constexpr uint32_t kPropWaterFlow = 0xa57ab877;
constexpr uint32_t kPropWaterFlowMap = 0xb541d215;
constexpr uint32_t kPropWaterFlowValue[10] = {0xf60b04b8, 0x50cab2c8, 0x41dd7b3f, 0x86e7335b, 0xd3a38db6,
                                              0x7ec506d9, 0x2aebbce6, 0xfe1e05a7, 0x74ee7d86, 0xf7a95b7e};
constexpr uint32_t kPropWaterMaterial[5] = {0x9201a855, 0x00c267c0, 0x50c8ac86, 0xd8afdce6, 0x513d8344};

constexpr size_t kMaxChunks = 1u << 20;
constexpr size_t kMaxVolumeFloats = size_t(1) << 28;
// A grid above this is halved: 24 bytes a point. Desktop keeps every grid whole (the
// largest, the hangar's, is 14.5M points: 349 MB on disk, 465 MB of volume textures),
// as Remastered samples them; Android keeps each file's grid under 24 MB so low-memory
// devices aren't killed in the 9 rooms past it.
#ifdef __ANDROID__
constexpr size_t kMaxGridPoints = size_t(1) << 20;
#else
constexpr size_t kMaxGridPoints = size_t(1) << 30;
#endif

// Remastered room coordinates -> GameCube area coordinates: (x, y, z) -> (-x, z, y).
constexpr double kR2G[3][3] = {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}};

using port::AppendBE32;
using port::AppendLE32;
using port::AppendLEFloat;
using port::ReadBE32;
using port::ReadLE16;
using port::ReadLE32;
using port::ReadLE64;
using port::ReadLEFloat;
float BeFloat(const uint8_t* p) {
  const uint32_t bits = ReadBE32(p);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}



// float32 -> float16, round to nearest even (numpy's astype(float16)).
uint16_t FloatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint16_t sign = uint16_t((x >> 16) & 0x8000);
  const uint32_t exponent = (x >> 23) & 0xFF;
  uint32_t mantissa = x & 0x7FFFFF;
  if (exponent == 0xFF) {
    return uint16_t(sign | 0x7C00 | (mantissa ? 0x200 | (mantissa >> 13) : 0));
  }
  const int e = int(exponent) - 127 + 15;
  if (e >= 31) {
    return uint16_t(sign | 0x7C00);
  }
  if (e <= 0) {
    if (e < -10) {
      return sign;
    }
    mantissa |= 0x800000;
    const int shift = 14 - e;
    uint32_t half = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1), mid = 1u << (shift - 1);
    if (rest > mid || (rest == mid && (half & 1))) {
      ++half;
    }
    return uint16_t(sign | half);
  }
  uint32_t half = (uint32_t(e) << 10) | (mantissa >> 13);
  const uint32_t rest = mantissa & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) {
    ++half;  // a carry out of the mantissa rolls into the exponent, as it should
  }
  return uint16_t(sign | half);
}

// A uuid in the order a property stores it (Python's bytes_le) and the order a
// pak id holds it (the printed order) differ by the same swap of the first
// three fields, so one function serves both ways.
Id16 SwapUuid(const uint8_t* b) {
  Id16 id;
  static const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    id[size_t(i)] = b[kOrder[i]];
  }
  return id;
}

// A property-order uuid as it prints (8-4-4-4-12).
std::string UuidText(const uint8_t* propertyId) {
  const Id16 id = SwapUuid(propertyId);
  std::string out;
  char hex[3];
  for (size_t i = 0; i < 16; ++i) {
    std::snprintf(hex, sizeof hex, "%02x", id[i]);
    out += hex;
    if (i == 3 || i == 5 || i == 7 || i == 9) {
      out += '-';
    }
  }
  return out;
}

// Numpy-style products, in the same order, so the signs of zeros come out alike.
Vec3 MulR2G(const Vec3& v) {
  Vec3 out{};
  for (int i = 0; i < 3; ++i) {
    out[size_t(i)] = kR2G[i][0] * v[0] + kR2G[i][1] * v[1] + kR2G[i][2] * v[2];
  }
  return out;
}

double Distance(const Vec3& a, const Vec3& b) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

// `out` = the inverse of `m`; false when it is singular.
bool Invert3(const double m[3][3], double out[3][3]) {
  const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
  const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
  const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  if (!std::isfinite(det) || std::fabs(det) < 1e-12) {
    return false;
  }
  const double k = 1.0 / det;
  out[0][0] = c00 * k;
  out[1][0] = c01 * k;
  out[2][0] = c02 * k;
  out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
  out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
  out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
  out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
  out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
  out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
  return true;
}

Vec3 Apply(const Mat34& a, const Vec3& d) {
  Vec3 out{};
  for (int i = 0; i < 3; ++i) {
    out[size_t(i)] = (d[0] * a[size_t(i)][0] + d[1] * a[size_t(i)][1] + d[2] * a[size_t(i)][2]) + a[size_t(i)][3];
  }
  return out;
}

// ---------------------------------------------------------------------------
// ROOM files (roomlib.py)
// ---------------------------------------------------------------------------

struct Span {
  size_t start = 0;
  size_t size = 0;
};

struct Prop {
  uint32_t id = 0;
  Span data;
};

struct Component {
  uint32_t type = 0;
  int layer = 0;
  bool baseLayer = false;  // on the layer that is always loaded (no layer unit, "LU__")
  Span raw;
  Span idta;
  Id16 guid{};
  bool hasGuid = false;
  int entity = -1;  // index of the Entity component this one belongs to
};

// The room's baked lighting record (HEAD/BLIT): its lightmap texture, and the atlas lookup
// (offU, offV, scale, 0) of each component that is lit by it, by the component's guid.
struct Blit {
  bool present = false;
  uint32_t flags = 0;  // 1 lightmap, 2 probe
  Id16 lightmap{};     // the TXTR's id in property byte order
  std::map<Id16, std::array<float, 4>> lookups;
  bool HasLightmap() const { return present && (flags & 1) != 0 && lightmap != Id16{}; }
};

class Room {
public:
  // `data` must outlive the Room.
  bool Parse(const std::vector<uint8_t>& data, std::string& error);

  const std::vector<Component>& Components() const { return m_comps; }
  const Blit& Lighting() const { return m_blit; }
  std::vector<const Component*> Of(uint32_t type) const {
    std::vector<const Component*> out;
    for (const Component& c : m_comps) {
      if (c.type == type) {
        out.push_back(&c);
      }
    }
    return out;
  }
  const uint8_t* Bytes(const Span& s) const { return m_d->data() + s.start; }
  // The index of the component with this guid, -1 for none.
  int ByGuid(const Id16& guid) const {
    const auto it = m_byGuid.find(guid);
    return it == m_byGuid.end() ? -1 : int(it->second);
  }

  // The entity's position, rotation and scale; false when the component has no entity.
  bool Xform(const Component& c, Vec3& pos, Vec3& rot, Vec3& scale) const {
    if (c.entity < 0) {
      return false;
    }
    const Span& raw = m_comps[size_t(c.entity)].raw;
    if (raw.size < 2 + 36) {
      return false;
    }
    const uint8_t* p = Bytes(raw) + 2;
    for (size_t i = 0; i < 3; ++i) {
      pos[i] = ReadLEFloat(p + 4 * i);
      rot[i] = ReadLEFloat(p + 12 + 4 * i);
      scale[i] = ReadLEFloat(p + 24 + 4 * i);
    }
    return true;
  }

  // Whether the component's entity starts active (the first byte of an entity).
  bool Active(const Component& c) const {
    if (c.entity < 0) {
      return false;
    }
    const Span& raw = m_comps[size_t(c.entity)].raw;
    return raw.size >= 1 && Bytes(raw)[0] != 0;
  }

  // The component's own top level properties. A value that is itself a
  // property list is a nested group, which is not a top level key.
  std::map<uint32_t, Span> Flat(const Component& c) const;
  // A property inside nested groups, named by the groups' ids and then its own.
  bool Nested(const Component& c, std::initializer_list<uint32_t> path, Span& out) const;
  // The `size` bytes at `offset` in the room's string pool (STRP); false when they lie outside it.
  bool String(uint32_t offset, uint32_t size, std::string& out) const {
    if (offset > m_strings.size || size > m_strings.size - offset) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(Bytes(m_strings)) + offset, size);
    return true;
  }
  // Where each resource id in the load units' lists (HEAD/LUNT/LRES) sits, in property byte order.
  const std::vector<size_t>& Listed() const { return m_listed; }
  // Whether these 16 bytes occur anywhere in the room besides `at`.
  bool NamedElsewhere(size_t at) const {
    const uint8_t* id = m_d->data() + at;
    for (auto it = m_d->begin(); (it = std::search(it, m_d->end(), id, id + 16)) != m_d->end(); ++it) {
      if (size_t(it - m_d->begin()) != at) {
        return true;
      }
    }
    return false;
  }

private:
  struct Chunk {
    uint32_t id;
    size_t start;
    size_t size;
  };
  bool Chunks(size_t o, size_t end, std::vector<Chunk>& out, std::string& error) const;
  bool Find(size_t o, size_t end, const uint32_t* path, size_t depth, std::vector<Span>& out,
            std::string& error) const;

  const std::vector<uint8_t>* m_d = nullptr;
  std::vector<Component> m_comps;
  std::map<Id16, size_t> m_byGuid;
  Span m_strings;
  std::vector<size_t> m_listed;
  Blit m_blit;
};

bool Room::Chunks(size_t o, size_t end, std::vector<Chunk>& out, std::string& error) const {
  const std::vector<uint8_t>& d = *m_d;
  while (o < end) {
    if (out.size() > kMaxChunks) {
      error = "too many chunks";
      return false;
    }
    if (d.size() < 4 || o > d.size() - 4) {
      error = "truncated chunk";
      return false;
    }
    const bool form = std::memcmp(&d[o], "RFRM", 4) == 0;
    const size_t header = form ? 32 : 24;
    if (o > d.size() || d.size() - o < header) {
      error = "truncated chunk header";
      return false;
    }
    const uint64_t size = ReadLE64(&d[o + 4]);
    if (size > d.size() - o - header) {
      error = "a chunk runs past the end of the file";
      return false;
    }
    out.push_back({form ? ReadBE32(&d[o + 20]) : ReadBE32(&d[o]), o + header, size_t(size)});
    o += header + size_t(size);
  }
  return true;
}

bool Room::Find(size_t o, size_t end, const uint32_t* path, size_t depth, std::vector<Span>& out,
                std::string& error) const {
  std::vector<Chunk> chunks;
  if (!Chunks(o, end, chunks, error)) {
    return false;
  }
  for (const Chunk& c : chunks) {
    if (c.id != path[0]) {
      continue;
    }
    if (depth == 1) {
      out.push_back({c.start, c.size});
    } else if (!Find(c.start, c.start + c.size, path + 1, depth - 1, out, error)) {
      return false;
    }
  }
  return true;
}

bool Room::Parse(const std::vector<uint8_t>& data, std::string& error) {
  m_d = &data;
  m_comps.clear();
  m_byGuid.clear();
  m_strings = {};
  m_listed.clear();
  m_blit = {};
  const std::vector<uint8_t>& d = data;
  if (d.size() < 32 || std::memcmp(d.data(), "RFRM", 4) != 0) {
    error = "not an RFRM file";
    return false;
  }
  const size_t rs = 32;
  const uint64_t rsz = ReadLE64(&d[4]);
  if (rsz > d.size() - rs) {
    error = "the form runs past the end of the file";
    return false;
  }
  const size_t re = rs + size_t(rsz);
  std::vector<Span> sden, idta, layers, strp;
  const uint32_t pSden[] = {Tag("SDTA"), Tag("SDEN")}, pIdta[] = {Tag("SDTA"), Tag("IDTA")},
                 pLayr[] = {Tag("LYRS"), Tag("LAYR")}, pStrp[] = {Tag("STRP")};
  if (!Find(rs, re, pSden, 2, sden, error) || !Find(rs, re, pIdta, 2, idta, error) ||
      !Find(rs, re, pLayr, 2, layers, error) || !Find(rs, re, pStrp, 1, strp, error)) {
    return false;
  }
  // STRP: u32 16, u32 1, u32 the pool's size, then the pool (NUL-separated names).
  if (!strp.empty() && strp[0].size >= 12) {
    const size_t pool = ReadLE32(&d[strp[0].start + 8]);
    m_strings = {strp[0].start + 12, std::min(pool, strp[0].size - 12)};
  }
  // LRES: u32 count, then that many 16-byte ids.
  std::vector<Span> lres;
  const uint32_t pLres[] = {Tag("HEAD"), Tag("LUNT"), Tag("LRES")};
  if (!Find(rs, re, pLres, 3, lres, error)) {
    return false;
  }
  for (const Span& s : lres) {
    const size_t n = s.size >= 4 ? std::min<size_t>(ReadLE32(&d[s.start]), (s.size - 4) / 16) : 0;
    for (size_t i = 0; i < n; ++i) {
      m_listed.push_back(s.start + 4 + 16 * i);
    }
  }
  // BLIT: u32 flags, the lightmap's guid, u32 n and n component guids, u32 n and n vec4
  // lookups (parallel to the guids), then an LTPB guid. One that does not fit is left out.
  std::vector<Span> blit;
  const uint32_t pBlit[] = {Tag("HEAD"), Tag("BLIT")};
  if (!Find(rs, re, pBlit, 2, blit, error)) {
    return false;
  }
  if (!blit.empty() && blit[0].size >= 24) {
    const uint8_t* b = &d[blit[0].start];
    const size_t size = blit[0].size;
    const size_t n = ReadLE32(b + 20);
    if (n <= (size - 24) / 16 && size - 24 - 16 * n >= 4) {
      const size_t at = 24 + 16 * n;
      const size_t m = ReadLE32(b + at);
      if (m == n && m <= (size - at - 4) / 16) {
        m_blit.present = true;
        m_blit.flags = ReadLE32(b);
        std::memcpy(m_blit.lightmap.data(), b + 4, 16);
        for (size_t i = 0; i < n; ++i) {
          Id16 g;
          std::memcpy(g.data(), b + 24 + 16 * i, 16);
          std::array<float, 4> v;
          for (size_t k = 0; k < 4; ++k) {
            v[k] = ReadLEFloat(b + at + 4 + 16 * i + 4 * k);
          }
          m_blit.lookups[g] = v;
        }
      }
    }
  }
  std::map<Id16, size_t>& byGuid = m_byGuid;
  for (size_t li = 0; li < layers.size(); ++li) {
    // The header (LHED) names the layer "LU_<layer unit>_PG_…"; the base layer has no unit.
    static constexpr char kUnit[] = "LU_";
    const uint8_t* const head = d.data() + layers[li].start;
    const uint8_t* const headEnd = head + std::min<size_t>(layers[li].size, 256);
    const uint8_t* const unit = std::search(head, headEnd, kUnit, kUnit + 3);
    const bool baseLayer = unit + 3 < headEnd && unit[3] == '_';
    std::vector<Span> comps;
    const uint32_t pComp[] = {Tag("SRIP"), Tag("COMP")};
    if (!Find(layers[li].start, layers[li].start + layers[li].size, pComp, 2, comps, error)) {
      return false;
    }
    for (const Span& cs : comps) {
      // The records are 12 bytes; a short tail is not one.
      for (size_t o = cs.start; o + 12 <= cs.start + cs.size; o += 12) {
        const uint32_t type = ReadLE32(&d[o]), pi = ReadLE32(&d[o + 4]), ii = ReadLE32(&d[o + 8]);
        if (pi >= sden.size() || ii >= idta.size()) {
          error = "a component points outside the data tables";
          return false;
        }
        Component c;
        c.type = type;
        c.layer = int(li);
        c.baseLayer = baseLayer;
        const Span s = sden[pi];
        c.raw = s.size >= 4 ? Span{s.start + 4, s.size - 4} : Span{s.start, 0};
        c.idta = idta[ii];
        if (c.idta.start <= d.size() && d.size() - c.idta.start >= 16) {
          std::memcpy(c.guid.data(), &d[c.idta.start], 16);
          c.hasGuid = true;
        }
        if (c.hasGuid) {
          byGuid[c.guid] = m_comps.size();
        }
        m_comps.push_back(c);
        if (m_comps.size() > kMaxChunks) {
          error = "too many components";
          return false;
        }
      }
    }
  }
  // An entity lists the components it owns as 'PMOC' + guid in its id data.
  for (size_t i = 0; i < m_comps.size(); ++i) {
    if (m_comps[i].type != kEntity) {
      continue;
    }
    const Span a = m_comps[i].idta;
    if (a.start > d.size() || a.size > d.size() - a.start) {
      continue;
    }
    const uint8_t* blob = &d[a.start];
    size_t o = 0;
    while (o + 4 <= a.size) {
      const uint8_t* hit = nullptr;
      for (size_t k = o; k + 4 <= a.size; ++k) {
        if (std::memcmp(blob + k, "PMOC", 4) == 0) {
          hit = blob + k;
          break;
        }
      }
      if (hit == nullptr) {
        break;
      }
      o = size_t(hit - blob);
      if (o + 20 <= a.size) {
        Id16 g;
        std::memcpy(g.data(), blob + o + 4, 16);
        const auto it = byGuid.find(g);
        if (it != byGuid.end()) {
          m_comps[it->second].entity = int(i);
        }
      }
      o += 20;
    }
  }
  return true;
}

// [(id, data)] if b is exactly a property list; false otherwise.
bool PropList(const uint8_t* b, size_t len, std::vector<Prop>* out) {
  if (len < 2) {
    return false;
  }
  const size_t n = ReadLE16(b);
  size_t o = 2;
  for (size_t i = 0; i < n; ++i) {
    if (o + 6 > len) {
      return false;
    }
    const uint32_t id = ReadLE32(b + o);
    const size_t size = ReadLE16(b + o + 4);
    o += 6;
    if (o + size > len) {
      return false;
    }
    if (out != nullptr) {
      out->push_back({id, {o, size}});
    }
    o += size;
  }
  return o == len;
}

bool Room::Nested(const Component& c, std::initializer_list<uint32_t> path, Span& out) const {
  Span at = c.raw;
  for (const uint32_t id : path) {
    std::vector<Prop> props;
    if (!PropList(Bytes(at), at.size, &props)) {
      return false;
    }
    bool found = false;
    for (const Prop& p : props) {
      if (p.id == id) {
        at = {at.start + p.data.start, p.data.size};
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  out = at;
  return true;
}

std::map<uint32_t, Span> Room::Flat(const Component& c) const {
  std::map<uint32_t, Span> out;
  const uint8_t* b = Bytes(c.raw);
  std::vector<Prop> props;
  if (!PropList(b, c.raw.size, &props) || props.empty()) {
    return out;
  }
  for (const Prop& p : props) {
    if (p.data.size > 4) {
      std::vector<Prop> sub;
      if (PropList(b + p.data.start, p.data.size, &sub) && !sub.empty()) {
        continue;
      }
    }
    out[p.id] = {c.raw.start + p.data.start, p.data.size};
  }
  return out;
}

// ---------------------------------------------------------------------------
// Retail world (gcres.py)
// ---------------------------------------------------------------------------

// A retail script object (SCLY), as far as matching Remastered's entities to it needs.
struct ScriptObject {
  uint32_t id = 0;  // editor id, layer bits included
  uint8_t type = 0;
  int layer = 0;    // the SCLY layer it is in
  uint32_t propCount = 0;
  std::vector<uint32_t> targets;  // of its connections
  bool hasPos = false;            // the three floats after its name, for any type
  Vec3 pos{};
  std::vector<uint8_t> props;     // from the end of its name on: what names its model
};

struct Area {
  uint32_t mrea = 0;
  Mat34 xf{};
  std::vector<Vec3> doors;
  std::vector<ScriptObject> objects;
};

// The MLVL's areas: id and transform.
bool ReadMlvl(const std::vector<uint8_t>& d, std::vector<std::pair<uint32_t, Mat34>>& out) {
  size_t o = 0;
  auto need = [&](size_t n) { return o <= d.size() && n <= d.size() - o; };
  if (!need(24)) {
    return false;
  }
  o += 20;
  uint32_t n = ReadBE32(&d[o]);
  if (n > d.size() / 11 || !need(4 + size_t(n) * 11)) {
    return false;
  }
  o += 4 + size_t(n) * 11;
  if (!need(8)) {
    return false;
  }
  n = ReadBE32(&d[o]);
  o += 8;
  if (n > 4096) {
    return false;
  }
  // Skips a count and `unit` bytes per entry.
  auto skip = [&](size_t unit) {
    if (!need(4)) {
      return false;
    }
    const uint64_t c = ReadBE32(&d[o]);
    if (c * unit > d.size()) {
      return false;
    }
    o += 4;
    if (!need(size_t(c * unit))) {
      return false;
    }
    o += size_t(c * unit);
    return true;
  };
  for (uint32_t i = 0; i < n; ++i) {
    if (!need(84)) {
      return false;
    }
    Mat34 xf{};
    for (size_t k = 0; k < 12; ++k) {
      xf[k / 4][k % 4] = BeFloat(&d[o + 4 + 4 * k]);
    }
    out.push_back({ReadBE32(&d[o + 76]), xf});
    o += 84;
    if (!skip(2)) {
      return false;
    }
    o += 4;
    if (!skip(8) || !skip(4) || !need(4)) {
      return false;
    }
    const uint32_t c = ReadBE32(&d[o]);
    o += 4;
    if (c > d.size()) {
      return false;
    }
    for (uint32_t k = 0; k < c; ++k) {
      if (!skip(8) || !skip(12)) {
        return false;
      }
    }
  }
  return true;
}

// The script objects of an MREA's SCLY section.
bool ReadScly(const std::vector<uint8_t>& m, std::vector<ScriptObject>& objects) {
  if (m.size() < 100) {
    return false;
  }
  const uint32_t sections = ReadBE32(&m[60]), scly = ReadBE32(&m[68]);
  if (sections > m.size() / 4 || 96 + size_t(sections) * 4 > m.size() || scly > sections) {
    return false;
  }
  size_t o = (96 + 4 * size_t(sections) + 31) & ~size_t(31);
  for (uint32_t i = 0; i < scly; ++i) {
    o += ReadBE32(&m[96 + 4 * size_t(i)]);
    if (o > m.size()) {
      return false;
    }
  }
  if (o + 12 > m.size() || std::memcmp(&m[o], "SCLY", 4) != 0) {
    return false;
  }
  const uint32_t layers = ReadBE32(&m[o + 8]);
  if (layers > m.size() / 4 || o + 12 + 4 * size_t(layers) > m.size()) {
    return false;
  }
  const size_t sizesAt = o + 12;
  o += 12 + 4 * size_t(layers);
  for (uint32_t l = 0; l < layers; ++l) {
    size_t p = o + 1;
    if (p + 4 > m.size()) {
      return false;
    }
    const uint32_t count = ReadBE32(&m[p]);
    p += 4;
    for (uint32_t i = 0; i < count; ++i) {
      if (p + 5 > m.size()) {
        return false;
      }
      const uint8_t type = m[p];
      const size_t size = ReadBE32(&m[p + 1]);
      const size_t objectEnd = p + 5 + size;
      if (objectEnd > m.size()) {
        return false;
      }
      size_t q = p + 5;
      if (q + 8 > objectEnd) {
        return false;
      }
      ScriptObject object;
      object.id = ReadBE32(&m[q]);
      object.type = type;
      object.layer = int(l);
      const uint64_t children = ReadBE32(&m[q + 4]);
      if (children > (objectEnd - q - 8) / 12) {
        return false;
      }
      for (uint64_t k = 0; k < children; ++k) {
        object.targets.push_back(ReadBE32(&m[q + 8 + 12 * size_t(k) + 8]));
      }
      q += 8 + size_t(12 * children) + 4;
      if (q > objectEnd) {
        return false;
      }
      object.propCount = ReadBE32(&m[q - 4]);
      const uint8_t* zero = static_cast<const uint8_t*>(std::memchr(&m[q], 0, objectEnd - q));
      if (zero == nullptr) {
        return false;
      }
      const size_t rest = size_t(zero - m.data()) + 1;
      if (objectEnd - rest >= 12) {
        object.hasPos = true;
        object.pos = {BeFloat(&m[rest]), BeFloat(&m[rest + 4]), BeFloat(&m[rest + 8])};
      } else if (type == 3) {
        return false;
      }
      object.props.assign(m.begin() + std::ptrdiff_t(rest), m.begin() + std::ptrdiff_t(objectEnd));
      objects.push_back(std::move(object));
      p = objectEnd;
    }
    o += ReadBE32(&m[sizesAt + 4 * size_t(l)]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Script links of scenery actors (build/mpr/scenery/match.py and trace.py)
// ---------------------------------------------------------------------------
//
// Remastered's scripts show and hide the scenery actors it added; the port draws them as
// room geometry, so what drives them has to be found among retail's objects. Remastered's
// entities are matched to the area's retail objects (same type at the same place, then
// the nearest one, then the same connection targets), and an actor's incoming
// connections are followed back, through Remastered's own relays and timers, to a matched
// sender. Which retail state a Remastered event is was voted on over every matched room.

struct RemasteredScriptType {
  uint32_t type;
  uint8_t retailType;
};
struct RemasteredScriptEvent {
  uint32_t type;
  uint32_t event;
  int state;
};
#include "port_remastered_script_tables.inc"

constexpr uint32_t kTemplateManager = 0xd645278a;
constexpr uint8_t kRetailPlatform = 0x08;
constexpr int kStatePlay = 18;  // EScriptObjectState kSS_Play
constexpr int kRetailDamageableTrigger = 0x1a;
constexpr int kStateMaxReached = 7;
constexpr uint8_t kRetailCounter = 0x06;
constexpr uint8_t kRetailTimer = 0x05;
// A retail Counter as Remastered remade it; the ones it added match no retail object.
constexpr uint32_t kCounterMP1 = 0x32aef7dd;
constexpr uint32_t kEventCounterMP1Max = 0x18977288;
// Actions a connection asks of its target.
constexpr uint32_t kActionActivate = 0xa34e100f;
constexpr uint32_t kActionDeactivate = 0xdd169ff3;
constexpr uint32_t kActionIncrement = 0xd5883f10;
constexpr uint32_t kActionDecrement = 0x767a0969;
constexpr uint32_t kActionToggleActive = 0xcdeb03ba;
// What an Entity is told to do with itself and every component it holds.
constexpr uint32_t kActionEntityActivate = 0x41435456;    // 'ACTV'
constexpr uint32_t kActionEntityDeactivate = 0x49435456;  // 'ICTV'
// Remastered's own script objects, which show and hide room geometry and which retail has
// no object for (PortRoomGeo::Script).
constexpr uint32_t kTriggerMP1 = 0xf526fc2a;
constexpr uint32_t kCounter = 0xa7db53c1;
constexpr uint32_t kRelay = 0x8fe0bfc9;
constexpr uint32_t kDebugOptions = 0xef2184e8;  // answers the debug menu only
constexpr uint32_t kPropTriggerFlags = 0x7ad69562;
constexpr uint32_t kTriggerDetectCamera = 0x4000;
constexpr uint32_t kPropCounterMax = 0xce73c1a7;
constexpr uint32_t kEventEntered = 0xcba5a77b;
constexpr uint32_t kEventExited = 0x8e8dd42d;
constexpr uint32_t kEventCounterNonZero = 0x646cf055;
constexpr uint32_t kEventCounterZero = 0x14f6ecb8;
constexpr uint32_t kEventCounterMax = 0xe5d0b94a;
constexpr uint32_t kEventRelayFired = 0xb4a0c8c1;
constexpr uint32_t kActionCounterIncrement = 0x787f7b2b;
constexpr uint32_t kActionCounterDecrement = 0x93c513fb;
constexpr uint32_t kActionRelayFire = 0xd432447e;
constexpr uint32_t kActionTriggerActivate = 0x3067f115;
constexpr uint32_t kActionTriggerDeactivate = 0xb5dd4543;
// An ActorKeyframe's Play (retail kSS_Play) tells its actor to play the keyframe's
// animation (property kPropKeyframeAnim, a name in STRP; kPropKeyframeLoop: it loops).
constexpr uint32_t kActorKeyframeMP1 = 0x3ce6630a;
constexpr uint32_t kActionPlayAnim = 0x7ae20cd8;
constexpr uint32_t kPropKeyframeAnim = 0x589ff022;
constexpr uint32_t kPropKeyframeLoop = 0x3c3ec403;
// What turns a ColorGradeHint's request on and off, and the senders of it retail has no
// object for: the player entering and leaving a fluid, the camera entering and leaving
// water, and a Counter that counts the camera's water volumes.
constexpr uint32_t kActionHintOn = 0x25592fa2;   // OnRequest
constexpr uint32_t kActionHintOff = 0x332214ff;  // OffRequest
// Also take a hint out (CColorGradeManager::ReallyRemoveHint); CColorGradeHintGOC's
// AcceptScriptMsg ignores every action but these, QueryHintState and (de)activation.
constexpr uint32_t kActionHintRemove = 0x3580a822;
constexpr uint32_t kActionHintDelete = 0x5844454c;  // 'DLEX'
bool HintTakes(uint32_t action) {
  return action == kActionHintOn || action == kActionHintOff || action == kActionHintRemove ||
         action == kActionHintDelete || action == kActionEntityActivate || action == kActionEntityDeactivate;
}
// What a VolumetricFogRegionTransition takes (its AcceptScriptMsg): Start, ResetAndStart,
// Stop, DLEX and (de)activation, the entity's and the component's own.
constexpr uint32_t kActionTransitionStart = 0x6c4d551c;
constexpr uint32_t kActionTransitionRestart = 0xfb05eadb;
constexpr uint32_t kActionTransitionStop = 0x4208824e;
constexpr uint32_t kActionComponentActivate = 0x41434d50;    // 'ACMP'
constexpr uint32_t kActionComponentDeactivate = 0x49434d50;  // 'ICMP'
uint8_t TransitionAct(uint32_t action) {
  switch (action) {
  case kActionTransitionStart:
    return PortRoomEnv::kTransitionStart;
  case kActionTransitionRestart:
    return PortRoomEnv::kTransitionRestart;
  case kActionTransitionStop:
    return PortRoomEnv::kTransitionStop;
  case kActionHintDelete:
    return PortRoomEnv::kTransitionDelete;
  case kActionEntityActivate:
  case kActionComponentActivate:
    return PortRoomGeo::kShow;
  case kActionEntityDeactivate:
  case kActionComponentDeactivate:
    return PortRoomGeo::kHide;
  default:
    return 0;
  }
}
// What a LightDynamic takes (CLightDynamicGOC::AcceptScriptMsg): its timeline's Start, Stop,
// Reset, PlayForward, PlayBackward and TogglePlaybackDirection, and (de)activation.
uint8_t LightAct(uint32_t action) {
  switch (action) {
  case 0x01bedeb9:
    return PortRoomEnv::kLightStart;
  case 0x2ebaa1ae:
    return PortRoomEnv::kLightStop;
  case 0x360ef3eb:
    return PortRoomEnv::kLightReset;
  case 0x2bebd3d6:
    return PortRoomEnv::kLightForward;
  case 0x65a5300e:
    return PortRoomEnv::kLightBackward;
  case 0x91651688:
    return PortRoomEnv::kLightReverse;
  case kActionComponentActivate:
    return PortRoomGeo::kShow;
  case kActionComponentDeactivate:
    return PortRoomGeo::kHide;
  default:
    return 0;
  }
}
constexpr uint32_t kProxyPlayer = 0x5797d3c7;
constexpr uint32_t kEventPlayerFluidIn = 0xcc17e9b1;
constexpr uint32_t kEventPlayerFluidOut = 0x42604bc6;
constexpr uint32_t kCameraWaterProxy = 0x6a7a53b0;
constexpr uint32_t kEventCameraWaterIn = 0x99655851;
constexpr uint32_t kEventCameraWaterOut = 0x5b16cc17;
constexpr uint32_t kEventCounterUp = 0x40e54906;
constexpr uint32_t kEventCounterDown = 0xc163beb5;
// Retail types of the Remastered objects a message passes through, and the actions that
// make each pass it on.
struct Pass {
  uint8_t retailType;
  uint32_t actions[2];
};
// A TimerMP1's wait, in seconds.
constexpr uint32_t kPropTimerDelay = 0xfb2a2cf0;
constexpr Pass kPasses[] = {
    {0x15, {0x379d362e, 0x379d362e}},  // Relay
    {kRetailTimer, {0xd63b8f04, 0x55193b90}},  // Timer
    {0x13, {0x326ddb0d, 0x326ddb0d}},  // MemoryRelay
    {0x5e, {0x144d0f29, 0x8ed2a8c7}},  // ColorModulate
};
constexpr double kMatchTolerance = 0.02;
constexpr double kMatchNear = 2.0;
// A model carried over from retail, in a room's byte order (EffectRetailId); the last
// four bytes are the retail id, big-endian as retail's own properties hold it.
constexpr uint8_t kRetailIdPrefix[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
constexpr uint8_t kRetailActor = 0x00;
// How far Remastered moved a scenery actor off the retail one it stands for (0.38 at most).
constexpr double kRetailActorNear = 0.5;
// Retail's debris: what a scenery actor that breaks apart is made of, shown when it does.
constexpr uint8_t kRetailDebris = 0x1B;
constexpr uint8_t kRetailDebrisExtended = 0x45;
// A retail Actor of 24 properties keeps its Active flag 13 bytes before its end (after it:
// the shader index, the x-ray alpha and four flags).
constexpr uint32_t kRetailActorProps = 24;
constexpr size_t kRetailActorActiveFromEnd = 13;
// How far past a piece's bounds a retail object it stands for may start: an Actor, or
// debris (and the Actor it breaks off), whose origin can sit a metre off its mesh (the
// Frigate hangar's, 1.09 at most).
constexpr double kPieceNear = 0.25;
constexpr double kPieceDebrisNear = 1.25;

// The retail type of a Remastered component type, -1 for one retail has no object for.
int RetailType(uint32_t type) {
  static const std::map<uint32_t, int> table = [] {
    std::map<uint32_t, int> t;
    for (const RemasteredScriptType& row : kRemasteredScriptTypes) {
      t[row.type] = row.retailType;
    }
    return t;
  }();
  const auto it = table.find(type);
  return it == table.end() ? -1 : it->second;
}

// The retail state a component type's event stands for, -1 when unknown.
int RetailState(uint32_t type, uint32_t event) {
  static const std::map<std::pair<uint32_t, uint32_t>, int> table = [] {
    std::map<std::pair<uint32_t, uint32_t>, int> t;
    for (const RemasteredScriptEvent& row : kRemasteredScriptEvents) {
      t[{row.type, row.event}] = row.state;
    }
    return t;
  }();
  const auto it = table.find({type, event});
  return it == table.end() ? -1 : it->second;
}

// What a connection's action does to the geometry or hint it reaches.
uint8_t LinkAct(uint32_t action) {
  return action == kActionActivate || action == kActionIncrement || action == kActionEntityActivate ||
                 action == kActionHintOn
             ? PortRoomGeo::kShow
         : action == kActionDeactivate || action == kActionDecrement || action == kActionEntityDeactivate ||
                 action == kActionHintOff || action == kActionHintRemove || action == kActionHintDelete
             ? PortRoomGeo::kHide
         : action == kActionToggleActive ? PortRoomGeo::kToggle
                                         : 0;
}

bool IsPass(int retailType) {
  for (const Pass& p : kPasses) {
    if (p.retailType == retailType) {
      return true;
    }
  }
  return false;
}

bool PassFires(int retailType, uint32_t action) {
  for (const Pass& p : kPasses) {
    if (p.retailType == retailType && (p.actions[0] == action || p.actions[1] == action)) {
      return true;
    }
  }
  return false;
}

struct Connection {
  size_t sender;  // component index
  uint32_t event;
  uint32_t action;
  Id16 target;  // a component's guid
};

// Component `i`'s outgoing connections, from its id data: guid, a block that may be
// skipped, then the list. False when the list does not read; `end` is where it ends.
bool ReadComponentConnections(const Room& room, size_t i, std::vector<Connection>& mine, size_t& end) {
  const std::vector<Component>& comps = room.Components();
  {
    const uint8_t* const b = room.Bytes(comps[i].idta);
    const size_t n = comps[i].idta.size;
    size_t o = 16;
    auto has = [&](size_t k) { return o <= n && n - o >= k; };
    if (!has(8)) {
      return false;
    }
    const uint32_t x = ReadLE32(b + o), y = ReadLE32(b + o + 4);
    o += 8;
    if (x == 0xffffffff) {
      if (!has(y)) {
        return false;
      }
      o += y;
    }
    if (!has(2)) {
      return false;
    }
    const size_t count = ReadLE16(b + o);
    o += 2;
    bool ok = true;
    // Two optional strings, then a fixed tail.
    auto block = [&]() {
      if (!has(4)) {
        return false;
      }
      const uint32_t present = ReadLE32(b + o);
      o += 4;
      if (present == 0) {
        return true;
      }
      if (!has(2) || (o += 2, !has(ReadLE16(b + o - 2)))) {
        return false;
      }
      o += ReadLE16(b + o - 2);
      if (!has(4)) {
        return false;
      }
      const uint32_t size = ReadLE32(b + o);
      o += 4;
      if (!has(size)) {
        return false;
      }
      o += size;
      return true;
    };
    for (size_t k = 0; k < count && ok; ++k) {
      if (!has(26)) {
        ok = false;
        break;
      }
      Connection c{i, ReadLE32(b + o), ReadLE32(b + o + 4), {}};
      std::memcpy(c.target.data(), b + o + 8, 16);
      o += 26;
      ok = block() && block() && has(19);
      o += 19;
      mine.push_back(c);
    }
    end = o;
    return ok;
  }
}

std::vector<Connection> ReadConnections(const Room& room) {
  std::vector<Connection> out;
  for (size_t i = 0; i < room.Components().size(); ++i) {
    std::vector<Connection> mine;
    size_t end = 0;
    if (ReadComponentConnections(room, i, mine, end)) {
      out.insert(out.end(), mine.begin(), mine.end());
    }
  }
  return out;
}

// A component's typed links, after its connections in its id data: u16 count, then per link
// u32 link id, the target entity's guid, u32, u8, u8, and a guid (42 bytes). The linked
// components (-1 for a guid the room lacks), in order.
std::vector<int> ReadEntityLinks(const Room& room, size_t i) {
  std::vector<int> out;
  std::vector<Connection> conns;
  size_t o = 0;
  if (!ReadComponentConnections(room, i, conns, o)) {
    return out;
  }
  const Span& idta = room.Components()[i].idta;
  const uint8_t* const b = room.Bytes(idta);
  const size_t n = idta.size;
  if (o > n || n - o < 2) {
    return out;
  }
  const size_t count = ReadLE16(b + o);
  o += 2;
  if ((n - o) / 42 < count) {
    return out;
  }
  for (size_t k = 0; k < count; ++k, o += 42) {
    Id16 target;
    std::memcpy(target.data(), b + o + 4, 16);
    out.push_back(room.ByGuid(target));
  }
  return out;
}

double MaxAbs(const Vec3& a, const Vec3& b) {
  return std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
}

// What drives the scenery actors of a room.
struct SceneryScripts {
  std::map<int, uint8_t> layer;  // by entity: the retail layer it is drawn on
  std::map<int, std::vector<PortRoomGeo::Link>> links;  // by entity
  std::map<int, size_t> unresolved;                      // by entity: connections not traced
  std::map<int, std::string> unresolvedHow;              // a grade hint's: sender type/event/action
  std::map<int, const ScriptObject*> platform;           // by entity: the platform carrying it
  size_t entities = 0, matched = 0;
  // Remastered's own objects in between (camera volumes, counters, relays) and the
  // entities they show and hide, by group number.
  PortRoomGeo::Script script;
  std::map<int, uint32_t> group;  // by entity
  std::set<int> scriptShows;      // entities a script edge can show
  // By entity: the ActorKeyframeMP1 components that start their next clip (kGroupNextClip),
  // in component order, which is the order the clips are written in.
  std::map<int, std::vector<size_t>> keyframes;
  size_t scriptUnresolved = 0;    // connections into the script not traced
};

SceneryScripts MatchScripts(const Room& room, const Area& area) {
  SceneryScripts result;
  const std::vector<Component>& comps = room.Components();
  const std::vector<ScriptObject>& objects = area.objects;
  // Entities in the order their first component comes, as that component's retail type
  // and layer.
  struct Ent {
    int entity;
    int layer;
    bool baseLayer;  // always loaded: drawn on no retail layer
    int type;
    bool hasPos;
    Vec3 w;  // GameCube world position
  };
  std::vector<Ent> ents;
  std::map<int, size_t> entIndex;
  for (const Component& c : comps) {
    if (c.entity < 0 || c.type == kEntity || entIndex.count(c.entity) != 0) {
      continue;
    }
    Ent e{c.entity, c.layer, c.baseLayer, RetailType(c.type), false, {}};
    Vec3 pos, rot, scale;
    if (room.Xform(c, pos, rot, scale)) {
      e.hasPos = true;
      e.w = Apply(area.xf, MulR2G(pos));
    }
    entIndex[c.entity] = ents.size();
    ents.push_back(e);
  }
  result.entities = ents.size();
  auto valid = [](const ScriptObject& o) {
    return o.hasPos && std::isfinite(o.pos[0]) && std::isfinite(o.pos[1]) && std::isfinite(o.pos[2]) &&
           std::fabs(o.pos[0]) < 1e5 && std::fabs(o.pos[1]) < 1e5 && std::fabs(o.pos[2]) < 1e5;
  };
  std::vector<int> match(ents.size(), -1);  // object index
  std::vector<bool> used(objects.size(), false);
  auto take = [&](size_t k, size_t j) {
    match[k] = int(j);
    used[j] = true;
  };
  // Same type at the same place; among several, the one on the same layer.
  for (size_t k = 0; k < ents.size(); ++k) {
    const Ent& e = ents[k];
    if (!e.hasPos || e.type < 0) {
      continue;
    }
    std::vector<size_t> cands, same;
    for (size_t j = 0; j < objects.size(); ++j) {
      if (objects[j].type == e.type && !used[j] && valid(objects[j]) && MaxAbs(objects[j].pos, e.w) < kMatchTolerance) {
        cands.push_back(j);
        if (objects[j].layer == e.layer) {
          same.push_back(j);
        }
      }
    }
    if (cands.size() > 1 && !same.empty()) {
      cands = same;
    }
    if (cands.size() == 1) {
      take(k, cands[0]);
    }
  }
  // Moved a little (triggers resized): the mutual nearest of the same type.
  {
    std::vector<size_t> left;
    for (size_t k = 0; k < ents.size(); ++k) {
      if (match[k] < 0 && ents[k].hasPos && ents[k].type >= 0) {
        left.push_back(k);
      }
    }
    std::vector<size_t> pool;
    for (size_t j = 0; j < objects.size(); ++j) {
      if (!used[j] && valid(objects[j])) {
        pool.push_back(j);
      }
    }
    std::vector<bool> alive(ents.size(), false);
    for (size_t k : left) {
      alive[k] = true;
    }
    for (size_t k : left) {
      size_t best = pool.size();
      double bestDist = kMatchNear;
      for (size_t i = 0; i < pool.size(); ++i) {
        const double d = MaxAbs(objects[pool[i]].pos, ents[k].w);
        if (objects[pool[i]].type == ents[k].type && d < bestDist) {
          best = i;
          bestDist = d;
        }
      }
      if (best == pool.size()) {
        continue;
      }
      const Vec3& p = objects[pool[best]].pos;
      size_t back = ents.size();
      double backDist = 0;
      for (size_t k2 : left) {
        const double d = MaxAbs(ents[k2].w, p);
        if (alive[k2] && ents[k2].type == ents[k].type && (back == ents.size() || d < backDist)) {
          back = k2;
          backDist = d;
        }
      }
      if (back == k) {
        take(k, pool[best]);
        pool.erase(pool.begin() + std::ptrdiff_t(best));
        alive[k] = false;
      }
    }
  }
  // The rest by where their connections go: one retail object of the type whose targets
  // are the matched targets.
  const std::vector<Connection> conns = ReadConnections(room);
  auto entityOf = [&](int comp) -> int {
    if (comp < 0 || comps[size_t(comp)].entity < 0) {
      return -1;
    }
    const auto it = entIndex.find(comps[size_t(comp)].entity);
    return it == entIndex.end() ? -1 : int(it->second);
  };
  std::vector<std::vector<uint32_t>> out(ents.size());
  for (const Connection& c : conns) {
    const int s = entityOf(int(c.sender)), t = entityOf(room.ByGuid(c.target));
    if (s >= 0 && t >= 0 && match[size_t(t)] >= 0) {
      out[size_t(s)].push_back(objects[size_t(match[size_t(t)])].id);
    }
  }
  for (auto& o : out) {
    std::sort(o.begin(), o.end());
    o.erase(std::unique(o.begin(), o.end()), o.end());
  }
  std::set<uint32_t> ids;
  for (const ScriptObject& o : objects) {
    ids.insert(o.id);
  }
  for (int round = 0; round < 3; ++round) {
    std::map<std::pair<int, std::vector<uint32_t>>, std::vector<size_t>> sig;
    for (size_t j = 0; j < objects.size(); ++j) {
      if (used[j]) {
        continue;
      }
      std::vector<uint32_t> s;
      for (uint32_t t : objects[j].targets) {
        if (ids.count(t) != 0) {
          s.push_back(t);
        }
      }
      std::sort(s.begin(), s.end());
      s.erase(std::unique(s.begin(), s.end()), s.end());
      if (!s.empty()) {
        sig[{objects[j].type, s}].push_back(j);
      }
    }
    int found = 0;
    for (size_t k = 0; k < ents.size(); ++k) {
      if (match[k] >= 0 || out[k].empty() || ents[k].type < 0) {
        continue;
      }
      const auto it = sig.find({ents[k].type, out[k]});
      if (it != sig.end() && it->second.size() == 1 && !used[it->second[0]]) {
        take(k, it->second[0]);
        ++found;
      }
    }
    if (found == 0) {
      break;
    }
  }
  // A Remastered layer is drawn on the retail layer most of its matched entities are on.
  // Remastered's base layer is always loaded, whatever its entities' retail layers (in
  // Phendrana Shorelines its vote picked a 15-object layer), so it is on no layer.
  std::map<int, std::vector<std::pair<uint8_t, int>>> votes;
  for (size_t k = 0; k < ents.size(); ++k) {
    if (match[k] < 0) {
      continue;
    }
    ++result.matched;
    const uint8_t layer = uint8_t(objects[size_t(match[k])].id >> 26);
    auto& v = votes[ents[k].layer];
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& p) { return p.first == layer; });
    if (it == v.end()) {
      v.push_back({layer, 1});
    } else {
      ++it->second;
    }
  }
  // Incoming connections, followed back to matched senders.
  // A connection to an Entity is to that entity (its own `entity` is its parent's), one to
  // any other component is to the entity holding it.
  auto targetEntity = [&](const Connection& c) -> int {
    const int t = room.ByGuid(c.target);
    return t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
  };
  std::map<int, std::vector<const Connection*>> incoming;  // by target entity component
  for (const Connection& c : conns) {
    const int t = targetEntity(c);
    if (t >= 0) {
      incoming[t].push_back(&c);
    }
  }
  // The entities that are Remastered's own script objects, and the component of each that
  // says what it is.
  std::map<int, std::pair<uint8_t, size_t>> scriptObject;  // by entity: node kind, component
  for (size_t i = 0; i < comps.size(); ++i) {
    const Component& c = comps[i];
    if (c.entity < 0) {
      continue;
    }
    uint8_t kind = 0;
    if (c.type == kTriggerMP1) {
      const auto f = room.Flat(c);
      const auto flags = f.find(kPropTriggerFlags);
      if (flags != f.end() && flags->second.size == 4 &&
          (ReadLE32(room.Bytes(flags->second)) & kTriggerDetectCamera) != 0) {
        kind = PortRoomGeo::kCameraVolume;
      }
    } else if (c.type == kCounter) {
      kind = PortRoomGeo::kCounter;
    } else if (c.type == kRelay) {
      kind = PortRoomGeo::kRelay;
    }
    if (kind != 0) {
      scriptObject[c.entity] = {kind, i};
    }
  }
  auto isScriptObject = [&](size_t comp) { return comps[comp].entity >= 0 && scriptObject.count(comps[comp].entity) != 0; };
  // A grade hint's senders that are no retail object: the sender (PortRoomEnv's
  // kSenderPlayerFluid/kSenderCameraWater) and state (0: in, 1: out), or sender 0.
  std::set<int> hintEntities;
  for (const Component& c : comps) {
    if ((c.type == kColorGradeHint || c.type == kBacklight || c.type == kVolumetricFogHint ||
         c.type == kVolumetricFogRegion) &&
        c.entity >= 0) {
      hintEntities.insert(c.entity);
    }
  }
  std::set<int> transitionEntities;
  for (const Component& c : comps) {
    if (c.type == kVolumetricFogRegionTransition && c.entity >= 0) {
      hintEntities.insert(c.entity);
      transitionEntities.insert(c.entity);
    }
  }
  // Point and spot lights, whose links also play their timelines (LightAct).
  std::set<int> lightEntities;
  for (const Component& c : comps) {
    if (c.type != kLightDynamic || c.entity < 0) {
      continue;
    }
    const auto f = room.Flat(c);
    const auto type = f.find(kPropLightType);
    const uint32_t kind = type != f.end() && type->second.size >= 4 ? ReadLE32(room.Bytes(type->second)) : 0;
    if (kind == kLightPoint || kind == kLightSpot) {
      lightEntities.insert(c.entity);
    }
  }
  auto fluidSender = [&](const Connection& c, int& state) -> uint32_t {
    const Component& sender = comps[c.sender];
    if (sender.type == kProxyPlayer && (c.event == kEventPlayerFluidIn || c.event == kEventPlayerFluidOut)) {
      state = c.event == kEventPlayerFluidIn ? 0 : 1;
      return PortRoomEnv::kSenderPlayerFluid;
    }
    if (sender.type == kCameraWaterProxy && (c.event == kEventCameraWaterIn || c.event == kEventCameraWaterOut)) {
      state = c.event == kEventCameraWaterIn ? 0 : 1;
      return PortRoomEnv::kSenderCameraWater;
    }
    // A Counter fed only by camera water proxies is non-zero while the camera is in water.
    if (sender.type != kCounter || sender.entity < 0) {
      return 0;
    }
    const bool up = c.event == kEventCounterUp || c.event == kEventCounterNonZero;
    if (!up && c.event != kEventCounterDown && c.event != kEventCounterZero) {
      return 0;
    }
    const auto in = incoming.find(sender.entity);
    if (in == incoming.end() || in->second.empty()) {
      return 0;
    }
    for (const Connection* feed : in->second) {
      if (comps[feed->sender].type != kCameraWaterProxy) {
        return 0;
      }
    }
    state = up ? 0 : 1;
    return PortRoomEnv::kSenderCameraWater;
  };
  // A counter Remastered added for a grade hint that counts what a retail counter counts (a
  // fight's deaths, say) is that retail counter: the one every matched sender feeding it also targets.
  auto RetailCounterFor = [&](int entity) -> const ScriptObject* {
    const auto in = incoming.find(entity);
    if (in == incoming.end()) {
      return nullptr;
    }
    std::vector<const ScriptObject*> feeds;
    for (const Connection* feed : in->second) {
      const int f = entityOf(int(feed->sender));
      if (f >= 0 && match[size_t(f)] >= 0) {
        feeds.push_back(&objects[size_t(match[size_t(f)])]);
      }
    }
    if (feeds.empty()) {
      return nullptr;
    }
    auto targets = [](const ScriptObject& from, const ScriptObject& to) {
      return std::find_if(from.targets.begin(), from.targets.end(), [&](uint32_t t) {
               return (t & 0x3ffffff) == (to.id & 0x3ffffff);
             }) != from.targets.end();
    };
    // Fed by every matched sender, and by as many retail objects as the Remastered counter has
    // feeds; one such counter, or none.
    const ScriptObject* found = nullptr;
    for (const ScriptObject& o : objects) {
      if (o.type != kRetailCounter ||
          !std::all_of(feeds.begin(), feeds.end(), [&](const ScriptObject* feed) { return targets(*feed, o); })) {
        continue;
      }
      size_t fedBy = 0;
      for (const ScriptObject& from : objects) {
        fedBy += targets(from, o) ? 1 : 0;
      }
      if (fedBy == in->second.size()) {
        // Two counting the same senders from the same start to the same maximum reach it
        // together (a Counter's first properties: initial count, maximum).
        if (found != nullptr && (!found->hasPos || !o.hasPos || std::memcmp(&found->pos, &o.pos, 8) != 0)) {
          return nullptr;
        }
        if (found == nullptr) {
          found = &o;
        }
      }
    }
    return found;
  };
  // A platform's Play -> Activate connections name the actors it carries
  // (CScriptPlatform::BuildSlaveList).
  for (const Connection& c : conns) {
    const int t = room.ByGuid(c.target), s = entityOf(int(c.sender));
    if (t < 0 || comps[size_t(t)].entity < 0 || s < 0 || match[size_t(s)] < 0 || c.action != kActionActivate) {
      continue;
    }
    const ScriptObject& sender = objects[size_t(match[size_t(s)])];
    if (sender.type == kRetailPlatform && RetailState(comps[c.sender].type, c.event) == kStatePlay) {
      result.platform[comps[size_t(t)].entity] = &sender;
    }
  }
  for (const Ent& e : ents) {
    const auto v = votes.find(e.layer);
    if (v != votes.end() && !e.baseLayer) {
      auto best = v->second.begin();
      for (auto it = v->second.begin(); it != v->second.end(); ++it) {
        if (it->second > best->second) {
          best = it;
        }
      }
      result.layer[e.entity] = best->first;
    }
    std::vector<PortRoomGeo::Link> links;
    size_t bad = 0;
    std::string how;
    // One more connection not traced; a grade hint's are named in the import log.
    const auto miss = [&](const Component& sender, const Connection& c, uint32_t first, int depth) {
      ++bad;
      if (hintEntities.count(e.entity) != 0) {
        const int s = entityOf(int(c.sender));
        char text[96];
        std::snprintf(text, sizeof(text), " %08X/%08X/%08X->%08X@%d%s%s", sender.type, c.event, c.action, first, depth,
                      s < 0 || match[size_t(s)] < 0 ? " unmatched" : "",
                      RetailState(sender.type, c.event) < 0 ? " nostate" : "");
        how += text;
        const auto feeds = incoming.find(sender.entity);
        for (size_t f = 0; sender.entity >= 0 && feeds != incoming.end() && f < feeds->second.size(); ++f) {
          const Connection& fc = *feeds->second[f];
          const int fs = entityOf(int(fc.sender));
          std::snprintf(text, sizeof(text), " [%08X/%08X/%08X%s]", comps[fc.sender].type, fc.event, fc.action,
                        fs < 0 || match[size_t(fs)] < 0 ? " u" : "");
          how += text;
        }
      }
    };
    std::vector<int> seen{e.entity};
    const bool transition = transitionEntities.count(e.entity) != 0;
    const bool light = lightEntities.count(e.entity) != 0;
    const auto act = [&](uint32_t action) {
      if (transition) {
        return TransitionAct(action);
      }
      const uint8_t lightAct = light ? LightAct(action) : 0;
      return lightAct != 0 ? lightAct : LinkAct(action);
    };
    // `delay`: what the timers passed through so far wait.
    std::function<void(int, uint32_t, int, int, float)> walk = [&](int entity, uint32_t action, int depth, int via,
                                                                   float delay) {
      const auto in = incoming.find(entity);
      if (in == incoming.end()) {
        return;
      }
      for (const Connection* c : in->second) {
        const Component& sender = comps[c->sender];
        int fluidState = 0;
        const uint32_t fluid =
            depth == 0 && hintEntities.count(entity) != 0 ? fluidSender(*c, fluidState) : 0;
        // Remastered drops what a hint does not take (a Relay's own action, say).
        if (depth == 0 && hintEntities.count(entity) != 0 &&
            (transition ? TransitionAct(c->action) == 0 : !HintTakes(c->action))) {
          continue;
        }
        if (fluid != 0) {
          const uint8_t fluidAct = act(c->action);
          if (fluidAct != 0) {
            links.push_back({fluid, uint8_t(fluidState), fluidAct});
          } else {
            miss(sender, *c, c->action, depth);
          }
          continue;
        }
        // A light that switches itself off as a backward play reaches its start (PointLightData::offAtStart).
        if (light && depth == 0 && sender.entity == e.entity && c->event == kEventLightReachedStart) {
          continue;
        }
        // Remastered's own objects, and keyframes, are followed by the script (below).
        if (sender.type == kTemplateManager || sender.type == kDebugOptions || isScriptObject(c->sender) ||
            (sender.type == kActorKeyframeMP1 && c->action == kActionPlayAnim) ||
            (depth > 0 && !PassFires(via, c->action))) {
          continue;
        }
        const int s = entityOf(int(c->sender));
        const int state = RetailState(sender.type, c->event);
        const uint32_t first = depth > 0 ? action : c->action;
        const int type = RetailType(sender.type);
        if (s >= 0 && match[size_t(s)] >= 0 && state >= 0) {
          // A DamageableTrigger reads its own MaxReached -> Activate connections and never
          // sends them, so only a direct one counts.
          const bool follows = depth == 0 && type == kRetailDamageableTrigger && state == kStateMaxReached &&
                               first == kActionActivate;
          const uint8_t linkAct = follows ? static_cast<uint8_t>(PortRoomGeo::kFollow) : act(first);
          if (linkAct != 0 && state < 256) {
            links.push_back({objects[size_t(match[size_t(s)])].id, uint8_t(state), linkAct, delay});
          }
        } else if (IsPass(type) && sender.entity >= 0 && depth < 6 &&
                   std::find(seen.begin(), seen.end(), sender.entity) == seen.end()) {
          seen.push_back(sender.entity);
          float wait = 0.f;
          if (type == kRetailTimer) {
            const auto f = room.Flat(sender);
            const auto prop = f.find(kPropTimerDelay);
            if (prop != f.end() && prop->second.size == 4) {
              wait = ReadLEFloat(room.Bytes(prop->second));
              wait = std::isfinite(wait) ? std::clamp(wait, 0.f, 600.f) : 0.f;
            }
          }
          walk(sender.entity, first, depth + 1, type, delay + wait);
          seen.pop_back();
        } else if (const ScriptObject* counter = hintEntities.count(e.entity) != 0 && sender.type == kCounterMP1 &&
                                                         c->event == kEventCounterMP1Max
                                                     ? RetailCounterFor(sender.entity)
                                                     : nullptr) {
          const uint8_t counterAct = act(first);
          if (counterAct != 0) {
            links.push_back({counter->id, uint8_t(kStateMaxReached), counterAct, delay});
          } else {
            miss(sender, *c, first, depth);
          }
        } else {
          miss(sender, *c, first, depth);
        }
      }
    };
    walk(e.entity, 0, 0, -1, 0.f);
    if (!links.empty()) {
      result.links[e.entity] = std::move(links);
    }
    if (bad != 0) {
      result.unresolved[e.entity] = bad;
      if (!how.empty()) {
        result.unresolvedHow[e.entity] = how;
      }
    }
  }
  // Remastered's own objects that show and hide geometry, followed back from the geometry
  // through whatever drives them: the camera volumes that count which side of a wall the
  // camera is on, the counters they count with and the relays in between.
  // The same objects switch the suns (the hangar's intro shots and its escape's camera).
  std::set<int> geometry;  // entities with a ModCon, an added actor or a sun, point or spot light
  for (const Component& c : comps) {
    if (c.entity >= 0 && (c.type == kModCon || (c.type == kActorMP1 && room.Flat(c).count(kPropActorAdded) != 0))) {
      geometry.insert(c.entity);
    } else if (c.entity >= 0 && c.type == kLightDynamic) {
      const auto f = room.Flat(c);
      const auto type = f.find(kPropLightType);
      if (type != f.end() && type->second.size >= 4) {
        const uint32_t kind = ReadLE32(room.Bytes(type->second));
        if (kind == kLightDirectional || kind == kLightPoint || kind == kLightSpot) {
          geometry.insert(c.entity);
        }
      }
    }
  }
  PortRoomGeo::Script& script = result.script;
  std::map<int, uint32_t> nodeOf;  // by entity
  std::vector<int> pending;
  auto nodeFor = [&](int entity) -> uint32_t {
    const auto known = nodeOf.find(entity);
    if (known != nodeOf.end()) {
      return known->second;
    }
    const auto& [kind, comp] = scriptObject.at(entity);
    const Component& c = comps[comp];
    PortRoomGeo::ScriptNode node;
    node.kind = kind;
    node.active = room.Active(c);
    if (kind == PortRoomGeo::kCounter) {
      const auto f = room.Flat(c);
      const auto max = f.find(kPropCounterMax);
      if (max != f.end() && max->second.size == 4) {
        node.max = ReadLE32(room.Bytes(max->second));
      }
    }
    Vec3 pos, rot, scale;
    if (kind == PortRoomGeo::kCameraVolume && !room.Xform(c, pos, rot, scale)) {
      // No box to be in: a volume no point is inside, even once something activates it.
      for (float& half : node.half) {
        half = -1.f;
      }
    } else if (kind == PortRoomGeo::kCameraVolume) {
      // The box is the entity's: centred on it, `scale` across, turned Rz * Ry * Rx, here
      // in area space (gc[i] = kSign[i] * remastered[kAxis[i]]) as the instances are.
      static const int kAxis[3] = {0, 2, 1};
      static const double kSign[3] = {-1, 1, 1};
      double s[3], k[3];
      for (int i = 0; i < 3; ++i) {
        s[i] = std::sin(rot[i] * (3.14159265358979323846 / 180.0));
        k[i] = std::cos(rot[i] * (3.14159265358979323846 / 180.0));
      }
      const double m[3][3] = {
          {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
          {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
          {-s[1], k[1] * s[0], k[1] * k[0]},
      };
      for (int col = 0; col < 3; ++col) {
        node.centre[col] = float(kSign[col] * pos[kAxis[col]]);
        node.half[col] = float(std::fabs(scale[kAxis[col]]) / 2);
        for (int row = 0; row < 3; ++row) {
          node.axes[3 * col + row] = float(kSign[row] * kSign[col] * m[kAxis[row]][kAxis[col]]);
        }
      }
    }
    nodeOf[entity] = uint32_t(script.nodes.size());
    script.nodes.push_back(node);
    pending.push_back(entity);
    return nodeOf[entity];
  };
  // The node event a script object's event is, -1 for one it does not send.
  auto nodeEvent = [](uint8_t kind, uint32_t event) -> int {
    switch (kind) {
    case PortRoomGeo::kCameraVolume:
      return event == kEventEntered ? 0 : event == kEventExited ? 1 : -1;
    case PortRoomGeo::kCounter:
      return event == kEventCounterNonZero ? 0 : event == kEventCounterZero ? 1 : event == kEventCounterMax ? 2 : -1;
    default:
      return event == kEventRelayFired ? 0 : -1;
    }
  };
  // What an action asks of a group (kind 0) or a node of this kind, 0 for nothing known.
  auto scriptAction = [](uint8_t kind, uint32_t action) -> uint8_t {
    const bool on = action == kActionActivate || action == kActionEntityActivate;
    const bool off = action == kActionDeactivate || action == kActionEntityDeactivate;
    if (kind == 0) {
      return on ? PortRoomGeo::kGroupShow
             : off ? PortRoomGeo::kGroupHide
             : action == kActionToggleActive ? PortRoomGeo::kGroupToggle
                                              : 0;
    }
    if (on || (kind == PortRoomGeo::kCameraVolume && action == kActionTriggerActivate)) {
      return PortRoomGeo::kNodeActivate;
    }
    if (off || (kind == PortRoomGeo::kCameraVolume && action == kActionTriggerDeactivate)) {
      return PortRoomGeo::kNodeDeactivate;
    }
    if (kind == PortRoomGeo::kCounter) {
      return action == kActionCounterIncrement ? PortRoomGeo::kIncrement
             : action == kActionCounterDecrement ? PortRoomGeo::kDecrement
                                                 : 0;
    }
    return kind == PortRoomGeo::kRelay && action == kActionRelayFire ? PortRoomGeo::kFire : 0;
  };
  // The edges into a geometry entity (group: from the script objects only, the walk above
  // has the rest) or a node (from anything).
  auto inputs = [&](int target, bool group, uint8_t kind) {
    const auto in = incoming.find(target);
    if (in == incoming.end()) {
      return;
    }
    for (const Connection* c : in->second) {
      const Component& sender = comps[c->sender];
      const bool fromScript = isScriptObject(c->sender);
      // A matched ActorKeyframe's Play moves an animated actor on to its next clip.
      const bool keyframe = group && sender.type == kActorKeyframeMP1 && c->action == kActionPlayAnim;
      if (sender.type == kTemplateManager || sender.type == kDebugOptions || (group && !fromScript && !keyframe)) {
        continue;
      }
      const uint8_t action =
          keyframe ? static_cast<uint8_t>(PortRoomGeo::kGroupNextClip) : scriptAction(kind, c->action);
      int event = -1;
      if (fromScript) {
        event = nodeEvent(scriptObject.at(sender.entity).first, c->event);
      } else {
        const int s = entityOf(int(c->sender));
        const int state = RetailState(sender.type, c->event);
        event = s >= 0 && match[size_t(s)] >= 0 && state < 256 ? state : -1;
      }
      if (action == 0 || event < 0) {
        ++result.scriptUnresolved;
        continue;
      }
      PortRoomGeo::ScriptEdge edge;
      edge.retail = !fromScript;
      edge.event = uint8_t(event);
      edge.action = action;
      if (group) {
        const uint32_t next = uint32_t(result.group.size());
        edge.to = result.group.emplace(target, next).first->second;
        if (keyframe) {
          result.keyframes[target].push_back(c->sender);
        } else if (action != PortRoomGeo::kGroupHide) {
          result.scriptShows.insert(target);
        }
      } else {
        edge.to = nodeOf.at(target);
      }
      edge.from = fromScript ? nodeFor(sender.entity) : objects[size_t(match[size_t(entityOf(int(c->sender)))])].id;
      script.edges.push_back(edge);
    }
  };
  for (int entity : geometry) {
    inputs(entity, true, 0);
  }
  for (size_t i = 0; i < pending.size(); ++i) {
    const int entity = pending[i];
    inputs(entity, false, scriptObject.at(entity).first);
  }
  return result;
}

// ---------------------------------------------------------------------------
// The writer (envwrite.py)
// ---------------------------------------------------------------------------

struct RoomData {
  std::string name;
  const Pak* pak = nullptr;
  std::vector<uint8_t> bytes;  // the ROOM file
  Room room;
  Id16 id{};                   // the ROOM asset's id, as property bytes (Python's bytes_le)
};

// A room's BloomEffect; SLdrBloomEffect's threshold when the component leaves it out.
struct BloomData {
  bool present = false;
  float threshold = 0.9f;
  std::vector<float> tints; // RGBA
};

// A ColorGrade: the retail layer it is on (-1 = always), its hint's fades, whether it is
// requested from the start, its priority, what turns it on and off, the LUT (33^3 RGBA8,
// red fastest).
struct GradeData {
  int32_t layer = -1;
  float fadeIn = 0, fadeOut = 0;
  bool on = true;
  int32_t priority = 50;
  std::vector<PortRoomGeo::Link> links;
  std::vector<uint8_t> lut;
};

// A Backlight: as a GradeData, with the coefficients in place of the LUT.
struct BacklightData {
  int32_t layer = -1;
  float fadeIn = 1, fadeOut = 1;
  bool on = false;
  int32_t priority = 50;
  float top = 1, back = 1;
  std::vector<PortRoomGeo::Link> links;
};

// A VolumetricFogHint with its fog, in retail axes (see PortRoomEnv::FogHint).
struct FogData {
  int32_t layer = -1;
  // The raw fade splines; empty: no interpolation, the fog changes at once.
  std::vector<uint8_t> fadeIn, fadeOut;
  bool on = false;
  int32_t priority = 50;
  float s[10] = {};  // range, scatter, absorb, m1z, decay, attenSlope, attenBias, noiseFreq, noiseStrength, lightCap
  float wind[3] = {};
  bool useScriptWind = false, noProbe = false;
  float colorB[4] = {1, 1, 1, 1}, colorA[4] = {0, 0, 0, 1};
  float lut[64] = {};
  std::vector<PortRoomGeo::Link> links;
};

// A VolumetricFogRegion as CVolumetricFogRegionGOC::AddRegion and RebuildPositionalData make
// its state, over retail world space (see PortRoomEnv::FogRegion).
struct FogRegionData {
  int32_t layer = -1;
  bool on = false;
  uint8_t fluid = 0; // 0 always, 1 while the camera is in a fluid, 2 while it is not
  bool hasColor = false, hasCap = false;
  float m[3][4] = {};
  float edgeScale[3] = {}, mult = 1, edgeBias[3] = {}, cap = 0;
  float color[4] = {};
  float density = 0;
  float box[6] = {}; // min, max
  std::vector<PortRoomGeo::Link> links;
  // What a transition starts from (CVolumetricFogRegionGOC::GetTransitionState).
  float distance = 250, transmittance = 0.01f;
  bool subtract = false;
};

// A VolumetricFogRegionTransition (see PortRoomEnv::FogTransition).
struct FogTransitionData {
  uint32_t region = 0; // index into the room's FogRegionData
  int32_t layer = -1;
  bool on = false, autoStart = false, loop = false;
  uint8_t select = 0;
  float distance = 250, transmittance = 0.01f, color[4] = {1, 1, 1, 1}, cap = 0;
  std::vector<uint8_t> phase; // CMayaSpline bytes
  std::vector<PortRoomGeo::Link> links;
};

// A directional LightDynamic (see PortRoomEnv::Sun).
struct SunData {
  int32_t layer = -1;
  bool on = false;
  float toSun[3] = {}; // retail world, unit length
  float color[3] = {}; // linear colour times intensity
  uint32_t group = PortRoomGeo::kNoGroup; // the script group that shows and hides it
};

// A point or spot LightDynamic (see PortRoomEnv::RoomLights), as SLdrLightDynamic::Load and
// NLightLoaders::build_light read it (kb func/light-dynamic.md).
struct PointLightData {
  int32_t layer = -1;
  bool on = false;
  bool spot = false;
  uint8_t falloff = 3;         // 1 linear, 2 squared, 3 smooth cubic
  float pos[3] = {};           // retail world
  float toLight[3] = {};       // spots: unit, against the beam
  float color[3] = {};         // linear colour times intensity
  float nearFar[2] = {0.25f, 1.f};  // times the entity's scale x and y
  float cone[2] = {0.f, 45.f}; // inner, outer full angles in degrees
  uint32_t group = PortRoomGeo::kNoGroup;
  std::vector<PortRoomGeo::Link> links;  // show/hide and the timeline's actions (LightAct)
  // The animated path: timeline flags, its falloff, whether its end deactivates it (its
  // finished event -> ICMP/ICTV on itself), the entity's scale x and y (for near and far),
  // and the splines' bytes (PortRoomEnv::kLightSplines; empty: absent).
  bool animated = false, playing = false, loop = false, offAtStart = false;
  uint8_t animFalloff = 3;
  float length = 0.f;
  float scale[2] = {1.f, 1.f};
  float intensity = 0.f; // the static colour and intensity, for a timeline that lacks a spline
  float rgb[3] = {};
  std::vector<uint8_t> splines[PortRoomEnv::kLightSplines];
};

struct Placement {
  Vec3 pos{};  // GameCube world coordinates, once the world shift is added
};

class Writer {
public:
  Writer(const RoomPak& master, const std::vector<RoomPak>& rooms, const std::vector<RoomPak>& others,
         const RoomIO& io)
      : m_master(master), m_rooms(rooms), m_others(others), m_io(io) {}

  bool Run(uint32_t mlvl, int& written, std::string& error);

private:
  void Log(const std::string& line) const {
    if (m_io.log) {
      m_io.log(line);
    }
  }

  static const PakAsset* FirstOfType(const Pak& pak, uint32_t type) {
    for (const PakAsset& a : pak.Assets()) {
      if (a.type == type) {
        return &a;
      }
    }
    return nullptr;
  }
  bool ReadRoomFile(const RoomPak& rp, RoomData& out, std::string& error) const;
  bool LoadAreas(uint32_t mlvl, std::string& error);
  // A REFL or TXTR by its uuid in property byte order: the room's own pak, the master's, then the others.
  bool FindResource(const uint8_t* propertyId, uint32_t type, const RoomPak& home, std::vector<uint8_t>& out,
                    const Pak** foundIn, Id16* foundId) const;

  // The room's baked lightmap (BLIT's TXTR) as BC6H layers, read once per room: the
  // geometry's lookups index it and the roomenv carries it. `lit` is how many of the
  // geometry's instances have a lookup (set by WriteGeometry).
  struct LightmapData {
    std::string room;
    bool ok = false;
    PortRemastered::TxtrLayersBc6h tex;
    size_t lit = 0;
  };
  LightmapData& Lightmap(const RoomData& r) const;
  mutable LightmapData m_lightmap;

  struct Match {
    uint32_t mrea = 0;
    Mat34 a{};
    double err = 0;
    const Area* area = nullptr;
  };
  bool MatchRoom(const RoomData& r, const std::map<std::string, Placement>& placed, Match& out) const;
  void Tonemap(const RoomData& r, float out[5]) const;
  void Exposure(const RoomData& r, float out[5]) const;
  void ReadBloom(const RoomData& r, BloomData& out) const;
  // The room's global ColorGrades in component order; `area` gives their retail layers.
  void ReadGrades(const RoomData& r, const Area* area, std::vector<GradeData>& out) const;
  // The room's Backlight hints, likewise.
  void ReadBacklights(const RoomData& r, const Area* area, std::vector<BacklightData>& out) const;
  // The room's fog hints, heights in Remastered axes (the writer applies the room's shift).
  void ReadFogs(const RoomData& r, const Area* area, std::vector<FogData>& out) const;
  // The room's fog regions, placed by the area's transform `xf`.
  void ReadFogRegions(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                      std::vector<FogRegionData>& out, std::vector<FogTransitionData>& transitions) const;
  void ReadSuns(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf, std::vector<SunData>& out) const;
  void ReadPointLights(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                       std::vector<PointLightData>& out) const;
  bool Grid(const RoomPak& rp, const Vec3& shift, const std::vector<Vec3>& check, std::vector<uint8_t>& out,
            std::string& note) const;
  // The room's static geometry (its ModCon components), as "<MREA id>.roomgeo".
  void WriteGeometry(const RoomData& r, uint32_t mrea, const Area& area);
  // The room's liquid surfaces (its water and lava render volumes), as "<MREA id>.roomliquid".
  void WriteLiquids(const RoomData& r, uint32_t mrea);
  std::string WriteRoom(const RoomData& r, const std::map<std::string, Placement>& placed, const Vec3& shift,
                        const float tonemap[5], const BloomData& bloom, const std::vector<GradeData>& worldGrades,
                        const std::vector<BacklightData>& worldBacklights, const std::vector<FogData>& worldFogs,
                        int& written, std::string& matched);

  const RoomPak& m_master;
  const std::vector<RoomPak>& m_rooms;
  const std::vector<RoomPak>& m_others;
  const RoomIO& m_io;
  std::vector<Area> m_areas;
  // Per LTPB block (layer, x, y, z), the world's room whose copy has the most lit points.
  using GridKey = std::array<int32_t, 4>;
  mutable std::map<GridKey, const RoomPak*> m_gridOwners;
  mutable bool m_gridOwnersReady = false;
};

double Spread(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
  double sum = 0;
  for (const Vec3& p : a) {
    double best = 1e300;
    for (const Vec3& q : b) {
      best = std::min(best, Distance(q, p));
    }
    sum += best;
  }
  return sum / double(a.size());
}

bool Writer::ReadRoomFile(const RoomPak& rp, RoomData& out, std::string& error) const {
  if (rp.pak == nullptr) {
    error = "no pak";
    return false;
  }
  const PakAsset* asset = FirstOfType(*rp.pak, Tag("ROOM"));
  if (asset == nullptr) {
    error = "no ROOM";
    return false;
  }
  if (!rp.pak->ReadAsset(*asset, out.bytes, error)) {
    return false;
  }
  out.name = rp.name;
  out.pak = rp.pak;
  out.id = SwapUuid(asset->id.data());
  return out.room.Parse(out.bytes, error);
}

bool Writer::LoadAreas(uint32_t mlvl, std::string& error) {
  std::vector<uint8_t> mlvlData;
  if (!m_io.retail || !m_io.retail(Tag("MLVL"), mlvl, mlvlData)) {
    error = "no retail MLVL";
    return false;
  }
  std::vector<std::pair<uint32_t, Mat34>> list;
  if (!ReadMlvl(mlvlData, list) || list.empty()) {
    error = "the retail MLVL is unreadable";
    return false;
  }
  for (const auto& [mrea, xf] : list) {
    Area a;
    a.mrea = mrea;
    a.xf = xf;
    std::vector<uint8_t> data;
    if (!m_io.retail(Tag("MREA"), mrea, data) || !ReadScly(data, a.objects)) {
      Log("  retail area " + std::to_string(mrea) + ": doors unreadable");
      a.objects.clear();
    }
    for (const ScriptObject& o : a.objects) {
      if (o.type == 3) {
        a.doors.push_back(o.pos);
      }
    }
    m_areas.push_back(std::move(a));
  }
  return true;
}

Writer::LightmapData& Writer::Lightmap(const RoomData& r) const {
  LightmapData& lm = m_lightmap;
  if (lm.room == r.name) {
    return lm;
  }
  lm = LightmapData{};
  lm.room = r.name;
  const Blit& blit = r.room.Lighting();
  if (!blit.HasLightmap()) {
    return lm;
  }
  std::vector<uint8_t> txtr;
  std::string error;
  if (!FindResource(blit.lightmap.data(), Tag("TXTR"), RoomPak{r.name, r.pak}, txtr, nullptr, nullptr)) {
    Log("  " + r.name + ": lightmap texture not found");
  } else if (!PortRemastered::ReadTxtrLayersBc6h(txtr.data(), txtr.size(), lm.tex, error)) {
    Log("  " + r.name + ": lightmap not used: " + error);
  } else if (lm.tex.layers.size() < 4) {
    Log("  " + r.name + ": lightmap not used: " + std::to_string(lm.tex.layers.size()) + " layers");
  } else {
    lm.ok = true;
  }
  return lm;
}

bool Writer::FindResource(const uint8_t* propertyId, uint32_t type, const RoomPak& home,
                          std::vector<uint8_t>& out, const Pak** foundIn, Id16* foundId) const {
  const Id16 id = SwapUuid(propertyId);
  std::vector<const Pak*> order{home.pak, m_master.pak};
  for (const RoomPak& r : m_rooms) {
    order.push_back(r.pak);
  }
  for (const RoomPak& r : m_others) {
    order.push_back(r.pak);
  }
  for (const Pak* pak : order) {
    if (pak == nullptr) {
      continue;
    }
    const PakAsset* asset = pak->Find(id);
    std::string error;
    if (asset != nullptr && asset->type == type && pak->ReadAsset(*asset, out, error)) {
      if (foundIn != nullptr) {
        *foundIn = pak;
      }
      if (foundId != nullptr) {
        *foundId = id;
      }
      return true;
    }
  }
  return false;
}

bool Writer::MatchRoom(const RoomData& r, const std::map<std::string, Placement>& placed, Match& out) const {
  std::vector<Vec3> doors;
  for (const Component* c : r.room.Of(kDoorMP1)) {
    Vec3 pos, rot, scale;
    if (r.room.Xform(*c, pos, rot, scale)) {
      doors.push_back(MulR2G(pos));
    }
  }
  auto transformed = [&](const Mat34& a) {
    std::vector<Vec3> t;
    for (const Vec3& d : doors) {
      t.push_back(Apply(a, d));
    }
    return t;
  };
  auto err = [&](const Mat34& a, const std::vector<Vec3>& g) {
    return !doors.empty() && !g.empty() ? Spread(transformed(a), g) : 0.0;
  };
  // Both ways, or a one-door room fits a neighbour that has that door among its own.
  auto both = [&](const Mat34& a, const std::vector<Vec3>& g) { return err(a, g) + Spread(g, transformed(a)); };
  const auto it = placed.find(r.name);
  if (it != placed.end()) {
    // Areas can share an origin (the Frigate's do), so the doors pick among those.
    const Area* best = nullptr;
    double bestKey = 0;
    for (const Area& t : m_areas) {
      if (Distance({t.xf[0][3], t.xf[1][3], t.xf[2][3]}, it->second.pos) >= 0.5) {
        continue;
      }
      const double key = !doors.empty() && !t.doors.empty() ? both(t.xf, t.doors)
                         : doors.size() == t.doors.size()   ? 0.0
                                                            : 99.0;
      if (best == nullptr || key < bestKey) {
        best = &t;
        bestKey = key;
      }
    }
    if (best != nullptr) {
      out = {best->mrea, best->xf, err(best->xf, best->doors), best};
      return true;
    }
  }
  // Not placed: fall back on the doors alone.
  if (doors.empty()) {
    return false;
  }
  const Area* best = nullptr;
  double bestErr = 0;
  for (const Area& t : m_areas) {
    if (t.doors.empty()) {
      continue;
    }
    const double e = err(t.xf, t.doors);
    if (best == nullptr || e < bestErr) {
      best = &t;
      bestErr = e;
    }
  }
  if (best == nullptr) {
    return false;
  }
  Log("  " + r.name + ": matched by its doors");
  out = {best->mrea, best->xf, bestErr, best};
  return true;
}

// The first Tonemap component's values, where the room has one; `out` keeps the rest.
void Writer::Tonemap(const RoomData& r, float out[5]) const {
  for (const Component* c : r.room.Of(kTonemap)) {
    const auto f = r.room.Flat(*c);
    for (int i = 0; i < 5; ++i) {
      const auto it = f.find(kPropTonemap[i]);
      if (it != f.end() && it->second.size >= 4) {
        out[i] = ReadLEFloat(r.room.Bytes(it->second));
      }
    }
    break;
  }
}

// The range of exposure values the room's auto exposure is held to, the bias it adds, the
// sigma it eases by and where in the range the static exposure sits, or 0, 0, 0, 32, 0.5
// for a room without auto exposure. A room can have several hints: the plain
// one for the whole room is wanted, not those with a mode (they are for a state of the
// room, such as a cutscene) or a volume of their own. A value the hint leaves out is
// SLdrAutoExposureHint's default.
void Writer::Exposure(const RoomData& r, float out[5]) const {
  out[0] = out[1] = out[2] = 0.f;
  out[3] = 32.f;
  out[4] = 0.5f;
  int best = 0;
  for (const Component* c : r.room.Of(kAutoExposureHint)) {
    const auto f = r.room.Flat(*c);
    const auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      return it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
    };
    const float range[2] = {value(kPropHintMin, -24.f), value(kPropHintMax, 24.f)};
    const float bias = value(kPropHintBias, 0.f);
    const float sigma = value(kPropHintSigma, 32.f);
    const float lerp = value(kPropHintStaticLerp, 0.5f);
    if (!std::isfinite(range[0]) || !std::isfinite(range[1]) || range[1] < range[0]) {
      continue;
    }
    Vec3 pos, rot, scale;
    const bool whole = !r.room.Xform(*c, pos, rot, scale) ||
                       (std::fabs(scale[0] - 1.0) < 1e-3 && std::fabs(scale[1] - 1.0) < 1e-3 &&
                        std::fabs(scale[2] - 1.0) < 1e-3);
    const int rank = f.find(kPropHintMode) != f.end() ? 1 : whole ? 3 : 2;
    if (rank > best) {
      best = rank;
      out[0] = range[0];
      out[1] = range[1];
      out[2] = std::isfinite(bias) ? bias : 0.f;
      out[3] = sigma >= 0.f && sigma < 10000.f ? sigma : 32.f;
      out[4] = lerp >= 0.f && lerp <= 1.f ? lerp : 0.5f;
    }
  }
}

// The first BloomEffect component's values, where the room has one; `out` keeps the rest.
void Writer::ReadBloom(const RoomData& r, BloomData& out) const {
  for (const Component* c : r.room.Of(kBloomEffect)) {
    const auto f = r.room.Flat(*c);
    auto it = f.find(kPropBloomThreshold);
    if (it != f.end() && it->second.size >= 4) {
      out.threshold = ReadLEFloat(r.room.Bytes(it->second));
    }
    it = f.find(kPropBloomTints);
    if (it != f.end() && it->second.size >= 4) {
      const uint8_t* const p = r.room.Bytes(it->second);
      const uint32_t count = ReadLE32(p);
      if (count <= 16 && it->second.size >= 4 + count * 16) {
        out.tints.assign(count * 4, 0.f);
        for (uint32_t i = 0; i < count * 4; ++i) {
          out.tints[i] = ReadLEFloat(p + 4 + i * 4);
        }
      }
    }
    out.present = true;
    break;
  }
}

void Writer::ReadGrades(const RoomData& r, const Area* area, std::vector<GradeData>& out) const {
  const std::vector<const Component*> grades = r.room.Of(kColorGrade);
  if (grades.empty()) {
    return;
  }
  std::map<int, const Component*> hints;  // by entity
  for (const Component* c : r.room.Of(kColorGradeHint)) {
    if (c->entity >= 0) {
      hints.emplace(c->entity, c);
    }
  }
  SceneryScripts scripts;
  if (area != nullptr) {
    scripts = MatchScripts(r.room, *area);
  }
  const RoomPak home{r.name, r.pak};
  for (const Component* c : grades) {
    const auto h = hints.find(c->entity);
    if (c->entity < 0 || h == hints.end()) {
      continue;
    }
    const auto hf = r.room.Flat(*h->second);
    const auto global = hf.find(kPropHintGlobal);
    const bool isGlobal = global != hf.end() && global->second.size >= 1 && r.room.Bytes(global->second)[0] != 0;
    // A local hint is requested only by its connections (OnRequest/OffRequest); one with
    // none that the port can follow never shows.
    std::vector<PortRoomGeo::Link> links;
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (link.sender != PortRoomEnv::kSenderPlayerFluid && link.sender != PortRoomEnv::kSenderCameraWater) {
          link.sender &= 0x3ffffff;  // as CEntity sends it: no layer bits
        }
        links.push_back(link);
      }
    }
    const auto unresolved = scripts.unresolved.find(c->entity);
    if (unresolved != scripts.unresolved.end()) {
      Log("  " + r.name + ": " + (isGlobal ? "global" : "local") + " colour grade hint with " +
          std::to_string(unresolved->second) +
          " connection(s) not followed (sender type/event/action:" +
          (scripts.unresolvedHow.count(c->entity) != 0 ? scripts.unresolvedHow.at(c->entity) : std::string()) +
          "), " + std::to_string(links.size()) + " followed");
    }
    if (!isGlobal && links.empty()) {
      continue;
    }
    const auto f = r.room.Flat(*c);
    const auto lut = f.find(kPropGradeLut);
    std::vector<uint8_t> txtr;
    if (lut == f.end() || lut->second.size < 16 ||
        !FindResource(r.room.Bytes(lut->second), Tag("TXTR"), home, txtr, nullptr, nullptr)) {
      Log("  " + r.name + ": colour grade LUT not found");
      continue;
    }
    GradeData g;
    uint32_t w = 0, hgt = 0, d = 0;
    std::string error;
    if (!DecodeTxtrVolumeRgba8(txtr.data(), txtr.size(), w, hgt, d, g.lut, error) || w != kGradeLutSize ||
        hgt != kGradeLutSize || d != kGradeLutSize) {
      Log("  " + r.name + ": colour grade LUT unreadable (" + error + ")");
      continue;
    }
    // Defaults as CGameHintBase's.
    auto fade = [&](uint32_t prop) {
      const auto it = hf.find(prop);
      return it != hf.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : 2.f;
    };
    g.fadeIn = fade(kPropHintFadeIn);
    g.fadeOut = fade(kPropHintFadeOut);
    Span priority;
    if (r.room.Nested(*h->second, {kPropHintPriority[0], kPropHintPriority[1]}, priority) && priority.size >= 4) {
      g.priority = int32_t(ReadLE32(r.room.Bytes(priority)));
    }
    // A global hint is requested while its entity is active; a local one once told to.
    g.on = isGlobal && r.room.Active(*h->second);
    g.links = std::move(links);
    const auto layer = scripts.layer.find(c->entity);
    g.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    out.push_back(std::move(g));
  }
}

void Writer::ReadBacklights(const RoomData& r, const Area* area, std::vector<BacklightData>& out) const {
  const std::vector<const Component*> lights = r.room.Of(kBacklight);
  if (lights.empty()) {
    return;
  }
  SceneryScripts scripts;
  if (area != nullptr) {
    scripts = MatchScripts(r.room, *area);
  }
  for (const Component* c : lights) {
    if (c->entity < 0) {
      continue;
    }
    const auto f = r.room.Flat(*c);
    const auto global = f.find(kPropBacklightGlobal);
    const bool isGlobal = global != f.end() && global->second.size >= 1 && r.room.Bytes(global->second)[0] != 0;
    std::vector<PortRoomGeo::Link> links;
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (link.sender != PortRoomEnv::kSenderPlayerFluid && link.sender != PortRoomEnv::kSenderCameraWater) {
          link.sender &= 0x3ffffff;  // as CEntity sends it: no layer bits
        }
        links.push_back(link);
      }
    }
    const auto unresolved = scripts.unresolved.find(c->entity);
    if (unresolved != scripts.unresolved.end()) {
      Log("  " + r.name + ": " + (isGlobal ? "global" : "local") + " backlight hint with " +
          std::to_string(unresolved->second) + " connection(s) not followed (sender type/event/action:" +
          (scripts.unresolvedHow.count(c->entity) != 0 ? scripts.unresolvedHow.at(c->entity) : std::string()) + "), " +
          std::to_string(links.size()) + " followed");
    }
    if (!isGlobal && links.empty()) {
      continue;
    }
    BacklightData b;
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      const float v = it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    b.top = value(kPropBacklightTop, 1.f);
    b.back = value(kPropBacklightBack, 1.f);
    b.fadeIn = value(kPropBacklightFadeIn, 1.f);
    b.fadeOut = value(kPropBacklightFadeOut, 1.f);
    Span priority;
    if (r.room.Nested(*c, {kPropBacklightPriority[0], kPropBacklightPriority[1]}, priority) && priority.size >= 4) {
      b.priority = int32_t(ReadLE32(r.room.Bytes(priority)));
    }
    b.on = isGlobal && r.room.Active(*c);
    b.links = std::move(links);
    const auto layer = scripts.layer.find(c->entity);
    b.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    out.push_back(std::move(b));
  }
}

void Writer::ReadFogs(const RoomData& r, const Area* area, std::vector<FogData>& out) const {
  const std::vector<const Component*> hints = r.room.Of(kVolumetricFogHint);
  if (hints.empty()) {
    return;
  }
  std::map<int32_t, const Component*> fogOf;
  for (const Component* c : r.room.Of(kVolumetricFog)) {
    fogOf[c->entity] = c;
  }
  SceneryScripts scripts;
  if (area != nullptr) {
    scripts = MatchScripts(r.room, *area);
  }
  for (const Component* h : hints) {
    const auto fogIt = fogOf.find(h->entity);
    if (h->entity < 0 || fogIt == fogOf.end()) {
      continue;
    }
    const Component& fog = *fogIt->second;
    const auto hf = r.room.Flat(*h);
    const auto autoReq = hf.find(kPropFogAuto);
    const bool isGlobal = autoReq == hf.end() || autoReq->second.size < 1 || r.room.Bytes(autoReq->second)[0] != 0;
    std::vector<PortRoomGeo::Link> links;
    const auto linked = scripts.links.find(h->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (link.sender != PortRoomEnv::kSenderPlayerFluid && link.sender != PortRoomEnv::kSenderCameraWater) {
          link.sender &= 0x3ffffff;
        }
        links.push_back(link);
      }
    }
    if (!isGlobal && links.empty()) {
      continue;
    }
    FogData g;
    const auto ff = r.room.Flat(fog);
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = ff.find(prop);
      const float v = it != ff.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    auto vec4 = [&](uint32_t prop, float* dst) {
      const auto it = ff.find(prop);
      if (it != ff.end() && it->second.size >= 16) {
        for (int i = 0; i < 4; ++i) {
          const float v = ReadLEFloat(r.room.Bytes(it->second) + 4 * i);
          dst[i] = std::isfinite(v) ? v : dst[i];
        }
      }
    };
    auto flag = [&](uint32_t prop, bool fallback) {
      const auto it = ff.find(prop);
      return it != ff.end() && it->second.size >= 1 ? r.room.Bytes(it->second)[0] != 0 : fallback;
    };
    const float range = value(kPropFogRange, 250.f);
    g.s[0] = range > 0 ? range : 250.f;
    g.s[1] = value(kPropFogScatter, 1.f);
    g.s[2] = value(kPropFogAbsorb, 1.f);
    g.s[3] = value(kPropFogM1z, 0.95f);
    const float residual = value(kPropFogResidual, 0.01f);
    g.s[4] = residual > 0 && residual < 1 ? -std::log(residual) / g.s[0] : 0.f;
    // Attenuation by height: factor = slope * y + bias, between heights a and b (Remastered y;
    // retail's z is that plus the room's shift, which the writer folds into the bias).
    Span at;
    float a = 0, b = 0, slope = 0, bias = 1;
    auto atten = [&](uint32_t id, float& dst) {
      Span sp;
      if (r.room.Nested(fog, {kPropFogAtten, id}, sp) && sp.size >= 4) {
        const float v = ReadLEFloat(r.room.Bytes(sp));
        dst = std::isfinite(v) ? v : dst;
      }
    };
    atten(kPropFogAttenA, a);
    atten(kPropFogAttenB, b);
    if (r.room.Nested(fog, {kPropFogAtten, kPropFogAttenOn}, at) && at.size >= 1 && r.room.Bytes(at)[0] != 0) {
      float diff = b - a;
      if (std::fabs(diff) < 1e-5f) {
        diff = 0.001f;
      }
      slope = -1.f / diff;
      bias = 1.f + a / diff;
    }
    g.s[5] = slope;
    g.s[6] = bias;
    const float scale = value(kPropFogNoiseScale, 20.f);
    g.s[7] = std::fabs(scale) < 1e-5f ? 1.f : 1.f / scale;
    g.s[8] = value(kPropFogNoiseStrength, 1.f);
    g.s[9] = value(kPropFogLightCap, 3e37f);
    vec4(kPropFogColorA, g.colorA);
    const float intensity = value(kPropFogIntensity, 1.f);
    for (int i = 0; i < 3; ++i) {
      g.colorA[i] *= intensity;
    }
    vec4(kPropFogColorB, g.colorB);
    g.noProbe = flag(kPropFogNoProbe, false);
    Span ws;
    if (r.room.Nested(fog, {kPropFogWind, kPropFogWindOn}, ws) && ws.size >= 1) {
      g.useScriptWind = r.room.Bytes(ws)[0] != 0;
    }
    if (r.room.Nested(fog, {kPropFogWind, kPropFogWindVec}, ws) && ws.size >= 12) {
      const Vec3 v = MulR2G({ReadLEFloat(r.room.Bytes(ws)), ReadLEFloat(r.room.Bytes(ws) + 4), ReadLEFloat(r.room.Bytes(ws) + 8)});
      for (size_t i = 0; i < 3; ++i) {
        g.wind[i] = std::isfinite(v[i]) ? float(v[i]) : 0.f;
      }
    }
    // The density spline, sampled at (i/63)^2 * range. Without one it is the loader's empty
    // spline, which evaluates to 0: no fog.
    PortMayaSpline lut;
    Span ls;
    if (!r.room.Nested(fog, {kPropFogLut}, ls) || !lut.Load(r.room.Bytes(ls), ls.size)) {
      lut = PortMayaSpline();
    }
    for (int i = 0; i < 64; ++i) {
      const float u = float(i) / 63.f;
      g.lut[i] = lut.Eval(u * u * g.s[0]);
    }
    // The fade interpolations: a wrapper property, then a property list holding the
    // interpolation's type and its spline.
    auto fade = [&](uint32_t prop) {
      Span sp;
      if (r.room.Nested(*h, {prop}, sp) && sp.size > 6) {
        std::vector<Prop> props;
        if (PropList(r.room.Bytes(sp) + 6, sp.size - 6, &props)) {
          for (const Prop& p : props) {
            const uint8_t* const d = r.room.Bytes(sp) + 6 + p.data.start;
            PortMayaSpline sline;
            if (p.id == kPropFogSpline && sline.Load(d, p.data.size)) {
              return std::vector<uint8_t>(d, d + p.data.size);
            }
          }
          // A Time interpolation without its spline is an empty one (done at once).
          static const uint8_t kEmpty[15] = {};
          return std::vector<uint8_t>(kEmpty, kEmpty + sizeof(kEmpty));
        }
      }
      return std::vector<uint8_t>();
    };
    g.fadeIn = fade(kPropFogFadeIn);
    g.fadeOut = fade(kPropFogFadeOut);
    Span priority;
    if (r.room.Nested(*h, {kPropFogPriority[0], kPropFogPriority[1]}, priority) && priority.size >= 4) {
      g.priority = int32_t(ReadLE32(r.room.Bytes(priority)));
    }
    g.on = isGlobal && r.room.Active(*h);
    g.links = std::move(links);
    const auto layer = scripts.layer.find(h->entity);
    g.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    out.push_back(std::move(g));
  }
}

void Writer::ReadSuns(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                      std::vector<SunData>& out) const {
  for (const Component* c : r.room.Of(kLightDynamic)) {
    const auto f = r.room.Flat(*c);
    const auto type = f.find(kPropLightType);
    if (type == f.end() || type->second.size < 4 || ReadLE32(r.room.Bytes(type->second)) != kLightDirectional) {
      continue;
    }
    Vec3 pos, euler, scale;
    if (!r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    // The light shines along its local +Y (rotated by Rz*Ry*Rx), so -Y points at the sun. Checked
    // against Remastered's hangar, where the white key light casts the beam's shadow toward the door.
    constexpr double kDegToRad = 3.14159265358979323846 / 180; // M_PI isn't in MSVC's <cmath>
    const double x = euler[0] * kDegToRad, y = euler[1] * kDegToRad, z = euler[2] * kDegToRad;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
                 sz = std::sin(z);
    const Vec3 up = MulR2G({cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx});
    Vec3 d{};
    for (size_t i = 0; i < 3; ++i) {
      d[i] = xf[i][0] * up[0] + xf[i][1] * up[1] + xf[i][2] * up[2];
    }
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-6)) {
      continue;
    }
    SunData s;
    float color[4] = {1, 1, 1, 1};
    Span sp;
    if (r.room.Nested(*c, {kPropLightColor[0], kPropLightColor[1]}, sp) && sp.size >= 12) {
      for (int i = 0; i < 3; ++i) {
        color[i] = ReadLEFloat(r.room.Bytes(sp) + 4 * i);
      }
    }
    float intensity = 1.f;
    if (r.room.Nested(*c, {kPropLightIntensity[0], kPropLightIntensity[1]}, sp) && sp.size >= 4) {
      intensity = ReadLEFloat(r.room.Bytes(sp));
    }
    for (int i = 0; i < 3; ++i) {
      s.toSun[i] = float(-d[size_t(i)] / len);
      s.color[i] = std::isfinite(color[i] * intensity) ? std::max(0.f, color[i] * intensity) : 0.f;
    }
    const auto layer = scripts.layer.find(c->entity);
    s.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    // A sun on no story layer is the room's own and starts on even if its entity doesn't: the
    // hangar's key sun is only switched on by the escape's camera, yet Remastered casts its
    // shadows from the first visit. Story-layer suns (the intro's) follow their script.
    s.on = r.room.Active(*c) || s.layer < 0;
    const auto group = scripts.group.find(c->entity);
    if (group != scripts.group.end()) {
      s.group = group->second;
    }
    char line[200];
    std::snprintf(line, sizeof line, "  %s: sun (%.3f, %.3f, %.3f) colour (%.2f, %.2f, %.2f) layer %d%s%s",
                  r.name.c_str(), s.toSun[0], s.toSun[1], s.toSun[2], s.color[0], s.color[1], s.color[2],
                  int(s.layer), s.on ? "" : " (off)",
                  s.group != PortRoomGeo::kNoGroup ? (" group " + std::to_string(s.group)).c_str() : "");
    Log(line);
    out.push_back(s);
  }
}

void Writer::ReadPointLights(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                             std::vector<PointLightData>& out) const {
  for (const Component* c : r.room.Of(kLightDynamic)) {
    const auto f = r.room.Flat(*c);
    const auto type = f.find(kPropLightType);
    const uint32_t kind = type != f.end() && type->second.size >= 4 ? ReadLE32(r.room.Bytes(type->second)) : 0;
    if (kind != kLightPoint && kind != kLightSpot) {
      continue;
    }
    Vec3 pos, euler, scale;
    if (!r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    PointLightData l;
    l.spot = kind == kLightSpot;
    const Vec3 w = Apply(xf, MulR2G(pos));
    for (int i = 0; i < 3; ++i) {
      l.pos[i] = float(w[size_t(i)]);
    }
    if (l.spot) {
      // A spot shines along its local +Z (CLightSceneNodeProxy::UpdateLightFromTransform), turned
      // Rz * Ry * Rx as the suns' +Y.
      constexpr double kDegToRad = 3.14159265358979323846 / 180;
      const double x = euler[0] * kDegToRad, y = euler[1] * kDegToRad, z = euler[2] * kDegToRad;
      const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
                   sz = std::sin(z);
      const Vec3 fwd = MulR2G({cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx});
      Vec3 d{};
      for (size_t i = 0; i < 3; ++i) {
        d[i] = xf[i][0] * fwd[0] + xf[i][1] * fwd[1] + xf[i][2] * fwd[2];
      }
      const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (!(len > 1e-6)) {
        continue;
      }
      for (int i = 0; i < 3; ++i) {
        l.toLight[i] = float(-d[size_t(i)] / len);
      }
    }
    float color[3] = {1, 1, 1};
    Span sp;
    if (r.room.Nested(*c, {kPropLightColor[0], kPropLightColor[1]}, sp) && sp.size >= 12) {
      for (int i = 0; i < 3; ++i) {
        color[i] = ReadLEFloat(r.room.Bytes(sp) + 4 * i);
      }
    }
    float intensity = 3.14159265f;
    if (r.room.Nested(*c, {kPropLightIntensity[0], kPropLightIntensity[1]}, sp) && sp.size >= 4) {
      intensity = ReadLEFloat(r.room.Bytes(sp));
    }
    for (int i = 0; i < 3; ++i) {
      l.color[i] = std::isfinite(color[i] * intensity) ? std::max(0.f, color[i] * intensity) : 0.f;
      l.rgb[i] = std::isfinite(color[i]) ? std::max(0.f, color[i]) : 0.f;
    }
    l.intensity = std::isfinite(intensity) ? intensity : 0.f;
    if (r.room.Nested(*c, {kPropLightDistance, kPropLightDistanceKind}, sp) && sp.size >= 4) {
      const uint32_t falloff = ReadLE32(r.room.Bytes(sp));
      l.falloff = falloff == kLightFalloffSquared ? 2 : falloff == kLightFalloffLinear ? 1 : 3;
    }
    if (r.room.Nested(*c, {kPropLightDistance, kPropLightDistanceNear}, sp) && sp.size >= 4) {
      l.nearFar[0] = ReadLEFloat(r.room.Bytes(sp));
    }
    if (r.room.Nested(*c, {kPropLightDistance, kPropLightDistanceFar}, sp) && sp.size >= 4) {
      l.nearFar[1] = ReadLEFloat(r.room.Bytes(sp));
    }
    // build_light scales the range by the entity's scale: near by x, far by y.
    l.nearFar[0] *= float(std::max(scale[0], 0.0));
    l.nearFar[1] *= float(std::max(scale[1], 0.0));
    if (r.room.Nested(*c, {kPropLightCone, kPropLightConeInner}, sp) && sp.size >= 4) {
      l.cone[0] = ReadLEFloat(r.room.Bytes(sp));
    }
    if (r.room.Nested(*c, {kPropLightCone, kPropLightConeOuter}, sp) && sp.size >= 4) {
      l.cone[1] = ReadLEFloat(r.room.Bytes(sp));
    }
    l.scale[0] = float(std::max(scale[0], 0.0));
    l.scale[1] = float(std::max(scale[1], 0.0));
    const auto byte = [&](uint32_t a, uint32_t b) {
      return r.room.Nested(*c, {a, b}, sp) && sp.size >= 1 && r.room.Bytes(sp)[0] != 0;
    };
    l.animated = byte(kPropLightAttributes, kPropLightAnimated);
    if (l.animated) {
      l.playing = byte(kPropLightAttributes, kPropLightPlaying);
      l.loop = byte(kPropLightAttributes, kPropLightLoop);
      if (r.room.Nested(*c, {kPropLightAttributes, kPropLightLength}, sp) && sp.size >= 4) {
        const float length = ReadLEFloat(r.room.Bytes(sp));
        l.length = std::isfinite(length) ? std::clamp(length, 0.f, 3600.f) : 0.f;
      }
      if (r.room.Nested(*c, {kPropLightDistanceDynamic, kPropLightDistanceDynamicKind}, sp) && sp.size >= 4) {
        const uint32_t falloff = ReadLE32(r.room.Bytes(sp));
        l.animFalloff = falloff == kLightFalloffSquared ? 2 : falloff == kLightFalloffLinear ? 1 : 3;
      }
      // One spline, checked; the gradient's four follow one another.
      const auto take = [&](uint32_t a, uint32_t b, std::vector<uint8_t>& to) {
        PortMayaSpline check;
        if (r.room.Nested(*c, {a, b}, sp) && check.Load(r.room.Bytes(sp), sp.size)) {
          to.assign(r.room.Bytes(sp), r.room.Bytes(sp) + sp.size);
        }
      };
      take(kPropLightIntensitySpline[0], kPropLightIntensitySpline[1], l.splines[PortRoomEnv::kLightSplineIntensity]);
      take(kPropLightDistanceDynamic, kPropLightDistanceDynamicNear, l.splines[PortRoomEnv::kLightSplineNear]);
      take(kPropLightDistanceDynamic, kPropLightDistanceDynamicFar, l.splines[PortRoomEnv::kLightSplineFar]);
      take(kPropLightConeDynamic, kPropLightConeDynamicInner, l.splines[PortRoomEnv::kLightSplineInner]);
      take(kPropLightConeDynamic, kPropLightConeDynamicOuter, l.splines[PortRoomEnv::kLightSplineOuter]);
      take(kPropLightColorDynamic, kPropLightColorDynamicSpline, l.splines[PortRoomEnv::kLightSplineColor]);
      if (r.room.Nested(*c, {kPropLightColorDynamic, kPropLightColorDynamicGradient}, sp) && sp.size >= 4 &&
          ReadLE32(r.room.Bytes(sp)) == 4) {
        const uint8_t* d = r.room.Bytes(sp) + 4;
        size_t left = sp.size - 4;
        for (int i = 0; i < 4; ++i) {
          PortMayaSpline check;
          size_t used = 0;
          if (!check.Load(d, left, &used)) {
            break;
          }
          l.splines[PortRoomEnv::kLightSplineGradient + i].assign(d, d + used);
          d += used;
          left -= used;
        }
      }
      for (const Connection& conn : ReadConnections(r.room)) {
        const int target = r.room.ByGuid(conn.target);
        if (r.room.Components()[conn.sender].entity == c->entity && target >= 0 &&
            r.room.Components()[size_t(target)].entity == c->entity && conn.event == kEventLightReachedStart &&
            (conn.action == kActionComponentDeactivate || conn.action == kActionEntityDeactivate)) {
          l.offAtStart = true;
        }
      }
    }
    if (const auto linked = scripts.links.find(c->entity); linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle &&
            (link.action < PortRoomEnv::kLightStart || link.action > PortRoomEnv::kLightReverse)) {
          continue;
        }
        link.sender &= 0x3ffffff;
        l.links.push_back(link);
      }
    }
    const auto shown = f.find(kPropLightShown);
    l.on = r.room.Active(*c) && (shown == f.end() || shown->second.size < 1 || r.room.Bytes(shown->second)[0] != 0);
    const auto layer = scripts.layer.find(c->entity);
    l.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    const auto group = scripts.group.find(c->entity);
    if (group != scripts.group.end()) {
      l.group = group->second;
    }
    char line[320];
    std::snprintf(line, sizeof line,
                  "  %s: %s (%.2f, %.2f, %.2f) colour (%.2f, %.2f, %.2f) range %.2f..%.2f kind %d cone %g/%g "
                  "layer %d%s%s, %zu link(s)%s",
                  r.name.c_str(), l.spot ? "spot" : "point", l.pos[0], l.pos[1], l.pos[2], l.color[0], l.color[1],
                  l.color[2], l.nearFar[0], l.nearFar[1], int(l.falloff), l.cone[0], l.cone[1], int(l.layer),
                  l.on ? "" : " (off)",
                  l.group != PortRoomGeo::kNoGroup ? (" group " + std::to_string(l.group)).c_str() : "",
                  l.links.size(), l.animated ? (l.playing ? ", animated, playing" : ", animated") : "");
    Log(line);
    out.push_back(l);
  }
}

void Writer::ReadFogRegions(const RoomData& r, const SceneryScripts& scripts, const Mat34& xf,
                            std::vector<FogRegionData>& out, std::vector<FogTransitionData>& transitions) const {
  const std::vector<const Component*> regions = r.room.Of(kVolumetricFogRegion);
  if (regions.empty()) {
    return;
  }
  std::map<int, uint32_t> regionOf; // the first region written on each entity
  // Retail world -> Remastered world: R2G times the area's inverse.
  double g[3][3], gt[3];
  {
    double a[3][3], inv[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        a[i][j] = xf[size_t(i)][size_t(j)];
      }
    }
    if (!Invert3(a, inv)) {
      return;
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        g[i][j] = kR2G[i][0] * inv[0][j] + kR2G[i][1] * inv[1][j] + kR2G[i][2] * inv[2][j];
      }
    }
    for (int i = 0; i < 3; ++i) {
      gt[i] = -(g[i][0] * xf[0][3] + g[i][1] * xf[1][3] + g[i][2] * xf[2][3]);
    }
  }
  for (const Component* c : regions) {
    Vec3 pos, euler, scale;
    if (!r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    const auto f = r.room.Flat(*c);
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      return it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
    };
    auto word = [&](uint32_t prop) {
      const auto it = f.find(prop);
      return it != f.end() && it->second.size >= 4 ? ReadLE32(r.room.Bytes(it->second)) : 0u;
    };
    float color[4] = {1, 1, 1, 1};
    {
      const auto it = f.find(kPropRegionColor);
      if (it != f.end() && it->second.size >= 16) {
        for (int i = 0; i < 4; ++i) {
          color[i] = ReadLEFloat(r.room.Bytes(it->second) + 4 * i);
        }
      }
    }
    const uint32_t mode = word(kPropRegionMode);
    const uint32_t fluid = word(kPropRegionFluid);
    const float distance = value(kPropRegionDistance, 250.f);
    const float transmittance = value(kPropRegionTransmittance, 0.01f);
    const float intensity = value(kPropRegionIntensity, -1.f);
    const float cap = value(kPropRegionCap, -1.f);
    const float edge = value(kPropRegionEdge, 2.f);
    // CVolumetricFogRegionGOC::AddRegion: a subtracting region's density, colour and cap are
    // negative; an overriding one scales what is there by 0 at its core.
    FogRegionData d;
    const float sign = mode == kRegionSubtract ? -1.f : 1.f;
    d.mult = mode == kRegionOverride ? 0.f : 1.f;
    d.density = sign * (-std::log(transmittance) / distance);
    if (intensity >= 0) {
      d.hasColor = true;
      for (int i = 0; i < 3; ++i) {
        d.color[i] = color[i] * sign * intensity;
      }
      d.color[3] = color[3];
    }
    if (cap >= 0) {
      d.hasCap = true;
      d.cap = sign * cap;
    }
    d.fluid = fluid == kRegionInsideFluid ? 1 : fluid == kRegionOutsideFluid ? 2 : 0;
    // RebuildPositionalData: the entity's transform (CTransform4f::RotateZYXTranslate of its
    // euler degrees) with its columns scaled, over the unit box around the origin.
    double m[3][3];
    {
      const double k = 3.14159265358979323846 / 180.0;
      const double cx = std::cos(euler[0] * k), sx = std::sin(euler[0] * k);
      const double cy = std::cos(euler[1] * k), sy = std::sin(euler[1] * k);
      const double cz = std::cos(euler[2] * k), sz = std::sin(euler[2] * k);
      const double rot[3][3] = {{cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz},
                                {cy * sz, cx * cz + sx * sy * sz, cx * sy * sz - sx * cz},
                                {-sy, sx * cy, cx * cy}};
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          m[i][j] = rot[i][j] * scale[size_t(j)];
        }
      }
    }
    double minv[3][3];
    if (!Invert3(m, minv)) {
      continue;
    }
    // world (retail) -> 0..1 over the box: minv (g p + gt - pos) + 0.5.
    for (int i = 0; i < 3; ++i) {
      double t = 0.5;
      for (int j = 0; j < 3; ++j) {
        d.m[i][j] = float(minv[i][0] * g[0][j] + minv[i][1] * g[1][j] + minv[i][2] * g[2][j]);
        t += minv[i][j] * (gt[j] - pos[size_t(j)]);
      }
      d.m[i][3] = float(t);
    }
    // The edge ramps come from Remastered's world box (its axes, not the region's), as the
    // game makes them; the cull box is the same box in retail world space.
    const float width = std::max(edge, 0.001f);
    for (int i = 0; i < 3; ++i) {
      const double size = std::fabs(m[i][0]) + std::fabs(m[i][1]) + std::fabs(m[i][2]);
      const float h = std::max(float(0.5 * size), 0.001f);
      const float frac = std::min(h, width) / h;
      const float rest = std::max(1.f - frac, 0.001f);
      d.edgeScale[i] = -1.f / frac;
      d.edgeBias[i] = 1.f + rest / frac;
    }
    for (int i = 0; i < 3; ++i) {
      d.box[i] = std::numeric_limits<float>::max();
      d.box[3 + i] = -std::numeric_limits<float>::max();
    }
    for (int corner = 0; corner < 8; ++corner) {
      Vec3 rem;
      for (int i = 0; i < 3; ++i) {
        rem[size_t(i)] = pos[size_t(i)];
        for (int j = 0; j < 3; ++j) {
          rem[size_t(i)] += m[i][j] * ((corner >> j & 1) != 0 ? 0.5 : -0.5);
        }
      }
      const Vec3 w = Apply(xf, MulR2G(rem));
      for (int i = 0; i < 3; ++i) {
        d.box[i] = std::min(d.box[i], float(w[size_t(i)]));
        d.box[3 + i] = std::max(d.box[3 + i], float(w[size_t(i)]));
      }
    }
    bool finite = std::isfinite(d.density);
    for (const float v : d.m[0]) finite = finite && std::isfinite(v);
    for (const float v : d.m[1]) finite = finite && std::isfinite(v);
    for (const float v : d.m[2]) finite = finite && std::isfinite(v);
    for (const float v : d.box) finite = finite && std::isfinite(v);
    if (!finite) {
      continue;
    }
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            link.action != PortRoomGeo::kToggle) {
          continue;
        }
        if (link.sender != PortRoomEnv::kSenderPlayerFluid && link.sender != PortRoomEnv::kSenderCameraWater) {
          link.sender &= 0x3ffffff;
        }
        d.links.push_back(link);
      }
    }
    d.on = r.room.Active(*c);
    const auto layer = scripts.layer.find(c->entity);
    d.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    d.distance = distance;
    d.transmittance = transmittance;
    d.subtract = mode == kRegionSubtract;
    regionOf.emplace(c->entity, uint32_t(out.size()));
    out.push_back(std::move(d));
  }
  // Each transition moves the region on the entity it links to (the first region of it,
  // as Start's GetFirstComponentByComponentType finds it).
  const std::vector<Component>& comps = r.room.Components();
  for (const Component* c : r.room.Of(kVolumetricFogRegionTransition)) {
    std::vector<int> targets = ReadEntityLinks(r.room, size_t(c - comps.data()));
    const auto target = std::find_if(targets.begin(), targets.end(), [&](int t) {
      return t >= 0 && regionOf.count(t) != 0;
    });
    if (target == targets.end()) {
      Log("room " + r.name + ": a fog region transition links to no region");
      continue;
    }
    const auto f = r.room.Flat(*c);
    auto value = [&](uint32_t prop, float fallback) {
      const auto it = f.find(prop);
      const float v = it != f.end() && it->second.size >= 4 ? ReadLEFloat(r.room.Bytes(it->second)) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    auto flag = [&](std::initializer_list<uint32_t> path) {
      Span s;
      return r.room.Nested(*c, path, s) && s.size >= 1 && r.room.Bytes(s)[0] != 0;
    };
    FogTransitionData t;
    t.region = regionOf.at(*target);
    t.autoStart = flag({kPropTransitionOptions, kPropTransitionAutoStart});
    t.loop = flag({kPropTransitionOptions, kPropTransitionLoop});
    for (int i = 0; i < 4; ++i) {
      if (flag({kPropTransitionSelect, kPropTransitionSelects[i]})) {
        t.select |= uint8_t(1u << i);
      }
    }
    {
      const auto it = f.find(kPropTransitionPhase);
      PortMayaSpline check;
      if (it != f.end() && check.Load(r.room.Bytes(it->second), it->second.size)) {
        t.phase.assign(r.room.Bytes(it->second), r.room.Bytes(it->second) + it->second.size);
      }
    }
    {
      const auto it = f.find(kPropTransitionColor);
      if (it != f.end() && it->second.size >= 16) {
        for (int i = 0; i < 4; ++i) {
          const float v = ReadLEFloat(r.room.Bytes(it->second) + 4 * i);
          t.color[i] = std::isfinite(v) ? v : t.color[i];
        }
      }
    }
    // The GOC's target colour: CColor4f::ScaleRGB by the intensity.
    const float intensity = value(kPropTransitionIntensity, 1.f);
    for (int i = 0; i < 3; ++i) {
      t.color[i] *= intensity;
    }
    t.distance = value(kPropTransitionDistance, 250.f);
    t.transmittance = value(kPropTransitionTransmittance, 0.01f);
    t.cap = value(kPropTransitionCap, 0.f);
    const auto linked = scripts.links.find(c->entity);
    if (linked != scripts.links.end()) {
      for (PortRoomGeo::Link link : linked->second) {
        if (link.action != PortRoomGeo::kShow && link.action != PortRoomGeo::kHide &&
            (link.action < PortRoomEnv::kTransitionStart || link.action > PortRoomEnv::kTransitionDelete)) {
          continue;
        }
        if (link.sender != PortRoomEnv::kSenderPlayerFluid && link.sender != PortRoomEnv::kSenderCameraWater) {
          link.sender &= 0x3ffffff;
        }
        t.links.push_back(link);
      }
    }
    t.on = r.room.Active(*c);
    const auto layer = scripts.layer.find(c->entity);
    t.layer = layer != scripts.layer.end() ? int32_t(layer->second) : -1;
    transitions.push_back(std::move(t));
  }
}

// One decoded LTPB texture, placed in the room's grid of 64x64x16 blocks.
struct GridTexture {
  int32_t bx, by, bz;
  uint32_t index, kind, format, w, h, depth;
  std::vector<float> rgba;
};

// The textures of an LTPB: the TXTR forms inside it, each preceded by a 77 byte record.
// Only those `want` accepts (it sees the record fields, not the data) are decoded.
bool ReadGridTextures(const std::vector<uint8_t>& d, const std::function<bool(const GridTexture&)>& want,
                      std::vector<GridTexture>& textures, std::string& note) {
  std::string error;
  for (size_t o = 77; o + 32 < d.size(); ++o) {
    if (std::memcmp(&d[o], "RFRM", 4) != 0 || std::memcmp(&d[o + 20], "TXTR", 4) != 0) {
      continue;
    }
    const uint8_t* g = &d[o - 77];
    if (o + 76 > d.size()) {
      note = "grid not decoded";
      return false;
    }
    const uint32_t decomp = ReadLE32(g + 20), bufOff = ReadLE32(g + 45), bufSize = ReadLE32(g + 49);
    GridTexture t;
    t.bx = int32_t(ReadLE32(g + 61));
    t.by = int32_t(ReadLE32(g + 65));
    t.bz = int32_t(ReadLE32(g + 69));
    t.index = ReadLE32(g + 73);
    const uint8_t* h = &d[o + 56];
    t.kind = ReadLE32(h);
    t.format = ReadLE32(h + 4);
    t.w = ReadLE32(h + 8);
    t.h = ReadLE32(h + 12);
    t.depth = ReadLE32(h + 16);
    if (bufOff > d.size() - o || bufSize > d.size() - o - bufOff) {
      note = "grid not decoded";
      return false;
    }
    if (t.index > 5 || t.h != 64 || t.depth != 16 || (t.w != 64 && t.w != 128)) {
      note = "grid texture " + std::to_string(t.w) + "x" + std::to_string(t.h) + "x" + std::to_string(t.depth) +
             " index " + std::to_string(t.index);
      return false;
    }
    if (!want(t)) {
      continue;
    }
    if (textures.size() >= 4096 ||
        !DecodeVolumeFloat(&d[o + bufOff], bufSize, decomp, t.format, t.w, t.h, t.depth, t.rgba, error)) {
      note = "grid not decoded";
      return false;
    }
    textures.push_back(std::move(t));
  }
  return true;
}

bool Writer::Grid(const RoomPak& rp, const Vec3& shift, const std::vector<Vec3>& check, std::vector<uint8_t>& out,
                  std::string& note) const {
  out.clear();
  const auto ltpb = [](const RoomPak& r, std::vector<uint8_t>& d) {
    const PakAsset* asset = r.pak != nullptr ? FirstOfType(*r.pak, Tag("LTPB")) : nullptr;
    std::string error;
    return asset != nullptr && r.pak->ReadAsset(*asset, d, error);
  };
  std::vector<uint8_t> d;
  if (FirstOfType(*rp.pak, Tag("LTPB")) == nullptr) {
    note = "no grid";
    return false;
  }
  if (!ltpb(rp, d)) {
    note = "grid not decoded";
    return false;
  }
  size_t phdr = std::string::npos;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (std::memcmp(&d[i], "PHDR", 4) == 0) {
      phdr = i;
      break;
    }
  }
  if (phdr == std::string::npos || phdr + 24 + 26 + 6 > d.size()) {
    note = "no grid";
    return false;
  }
  int lo[3], hi[3];
  for (size_t i = 0; i < 3; ++i) {
    lo[i] = int16_t(ReadLE16(&d[phdr + 44 + 2 * i]));
    hi[i] = int16_t(ReadLE16(&d[phdr + 50 + 2 * i]));
  }

  std::vector<GridTexture> textures;
  if (!ReadGridTextures(d, [](const GridTexture&) { return true; }, textures, note)) {
    return false;
  }
  if (textures.empty()) {
    note = "no grid";
    return false;
  }

  // Blocks of the room's PHDR box that its own LTPB lacks read 0 in the room alone, but
  // Remastered's probe texture is world-wide: a loaded neighbour area keeps its tiles mapped
  // (EnsureGroupIdTilesActive), so far scenery outside the room's bake is lit by theirs. Each
  // missing block comes from the world's room whose copy has the most lit points.
  // MP_REMASTERED_GRID_NEIGHBOURS=0 turns this off.
  int borrowed = 0;
  if (port::EnvFlag("MP_REMASTERED_GRID_NEIGHBOURS", true)) {
    const auto key = [](const GridTexture& t) { return GridKey{int32_t(t.index), t.bx, t.by, t.bz}; };
    if (!m_gridOwnersReady) {
      m_gridOwnersReady = true;
      std::map<GridKey, size_t> lit;
      for (const RoomPak& other : m_rooms) {
        std::vector<uint8_t> od;
        std::vector<GridTexture> found;
        std::string ignored;
        if (other.pak == nullptr || !ltpb(other, od) ||
            !ReadGridTextures(od, [](const GridTexture& t) { return t.index == 0; }, found, ignored)) {
          continue;
        }
        for (const GridTexture& t : found) {
          size_t n = 0;
          for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) {
            n += (t.rgba[i] + t.rgba[i + 1]) + t.rgba[i + 2] > 0 ? 1 : 0;
          }
          size_t& best = lit[key(t)];
          const RoomPak*& owner = m_gridOwners[key(t)];
          // Ties go to the lower name, so the output doesn't hang on the room order.
          if (owner == nullptr || n > best || (n == best && other.name < owner->name)) {
            best = n;
            m_gridOwners[key(t)] = &other;
          }
        }
      }
    }
    std::set<GridKey> own;
    for (const GridTexture& t : textures) {
      own.insert(key(t));
    }
    const auto floorDiv = [](int64_t a, int64_t b) { return int32_t(a >= 0 ? a / b : -((-a + b - 1) / b)); };
    // Per lending room, the layer-0 blocks it fills.
    std::map<const RoomPak*, std::vector<GridKey>> lenders;
    for (int32_t z = floorDiv(lo[2], 16); z <= floorDiv(hi[2], 16); ++z) {
      for (int32_t y = floorDiv(lo[1], 64); y <= floorDiv(hi[1], 64); ++y) {
        for (int32_t x = floorDiv(lo[0], 64); x <= floorDiv(hi[0], 64); ++x) {
          const GridKey hole{0, x, y, z};
          const auto owner = m_gridOwners.find(hole);
          if (own.count(hole) == 0 && owner != m_gridOwners.end() && owner->second->name != rp.name) {
            lenders[owner->second].push_back(hole);
          }
        }
      }
    }
    // Each layer of the lender's block, where the room lacks that layer's block (layers 2..5
    // are 128 wide, so their block holds two of layer 0's).
    std::set<GridKey> taken;
    for (const auto& [lender, holes] : lenders) {
      std::vector<uint8_t> od;
      if (!ltpb(*lender, od)) {
        continue;
      }
      std::vector<GridTexture> found;
      std::string ignored;
      ReadGridTextures(od, [&](const GridTexture& t) {
        const GridKey k = key(t);
        if (own.count(k) != 0 || taken.count(k) != 0) {
          return false;
        }
        for (const GridKey& hole : holes) {
          if (t.by == hole[2] && t.bz == hole[3] && (t.w == 64 ? t.bx == hole[1] : t.bx == floorDiv(hole[1], 2))) {
            taken.insert(k);
            return true;
          }
        }
        return false;
      }, found, ignored);
      for (GridTexture& t : found) {
        textures.push_back(std::move(t));
        ++borrowed;
      }
    }
  }

  // Where each texture starts, in points. A block is 64 x 64 x 16 points, and a texture
  // 128 wide is two blocks along x, counted in its own width.
  const auto start = [](const GridTexture& t, int axis) -> int64_t {
    return axis == 0 ? int64_t(t.bx) * int64_t(t.w) : axis == 1 ? int64_t(t.by) * 64 : int64_t(t.bz) * 16;
  };
  int64_t base[3] = {INT64_MAX, INT64_MAX, INT64_MAX};
  for (const GridTexture& t : textures) {
    for (int i = 0; i < 3; ++i) {
      base[i] = std::min(base[i], start(t, i));
    }
  }
  // The blocks are whole tiles of the world's bake, lit well past the room's PHDR box
  // (Main Plaza has 23k lit points outside it), and Remastered samples all of them, with
  // unmapped tiles reading zero. So the kept part is the lit points' bounds and one empty
  // point around them: the texture's clamped reads past it are zero too.
  int64_t c0[3] = {INT64_MAX, INT64_MAX, INT64_MAX}, c1[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
  for (const GridTexture& t : textures) {
    if (t.index != 0) {
      continue;
    }
    const int64_t x0 = start(t, 0) - base[0], y0 = start(t, 1) - base[1], z0 = start(t, 2) - base[2];
    for (int64_t z = 0; z < 16; ++z) {
      for (int64_t y = 0; y < 64; ++y) {
        for (int64_t x = 0; x < int64_t(t.w); ++x) {
          const float* p = &t.rgba[((size_t(z) * 64 + size_t(y)) * t.w + size_t(x)) * 4];
          if (!((p[0] + p[1]) + p[2] > 0)) {
            continue;
          }
          const int64_t q[3] = {x0 + x, y0 + y, z0 + z};
          for (int i = 0; i < 3; ++i) {
            c0[i] = std::min(c0[i], q[i] - 1);
            c1[i] = std::max(c1[i], q[i] + 2);
          }
        }
      }
    }
  }
  if (c1[0] <= c0[0] || c1[1] <= c0[1] || c1[2] <= c0[2]) {
    note = "grid empty";
    return false;
  }
  int64_t size[3] = {c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2]};
  const int64_t origin[3] = {base[0] + c0[0], base[1] + c0[1], base[2] + c0[2]};
  size_t points = size_t(size[0]) * size_t(size[1]) * size_t(size[2]);
  if (size[0] > 1024 || size[1] > 1024 || size[2] > 1024 || points * 18 > kMaxVolumeFloats) {
    note = "grid too large";
    return false;
  }

  // Only the cropped part of the room's volume is kept: [index][z][y][x][rgb].
  std::vector<float> g(points * 18, 0.f);
  for (const GridTexture& t : textures) {
    const int64_t x0 = start(t, 0) - base[0], y0 = start(t, 1) - base[1], z0 = start(t, 2) - base[2];
    for (int64_t z = std::max(z0, c0[2]); z < std::min(z0 + 16, c1[2]); ++z) {
      for (int64_t y = std::max(y0, c0[1]); y < std::min(y0 + 64, c1[1]); ++y) {
        for (int64_t x = std::max(x0, c0[0]); x < std::min(x0 + int64_t(t.w), c1[0]); ++x) {
          const float* src = &t.rgba[((size_t(z - z0) * 64 + size_t(y - y0)) * t.w + size_t(x - x0)) * 4];
          float* dst = &g[((size_t(t.index) * size_t(size[2]) + size_t(z - c0[2])) * size_t(size[1]) +
                           size_t(y - c0[1])) * size_t(size[0]) * 3 + size_t(x - c0[0]) * 3];
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
        }
      }
    }
  }
  // Point k sits at 2k metres in Remastered's axes, not in the middle of a 2 m cell:
  // CBakedLightingProbeTexture::Initialize (0x1c8cbc) rounds the box to points
  // (floor(x * 0.5 + 0.5)) and maps to texels by Scale(1 / size) * Translate(0.5 - lo) *
  // Scale(0.5), so texel k's middle is point lo + k.
  double m[3][4];
  const Vec3 shifted = MulR2G(shift);  // R2G is its own transpose
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      m[i][j] = kR2G[j][i] / 2;
    }
    m[i][3] = -shifted[size_t(i)] / 2 - double(origin[i]);
  }
  // Android only (see kMaxGridPoints): the largest rooms have millions of points and
  // ambient light varies slowly, so those are kept at half the resolution, each point
  // the average of its lit ones.
  while (points > kMaxGridPoints) {
    const int64_t half[3] = {(size[0] + 1) / 2, (size[1] + 1) / 2, (size[2] + 1) / 2};
    const size_t fewer = size_t(half[0]) * size_t(half[1]) * size_t(half[2]);
    std::vector<float> coarse(fewer * 18, 0.f);
    std::vector<float> sum(18);
    for (int64_t z = 0; z < half[2]; ++z) {
      for (int64_t y = 0; y < half[1]; ++y) {
        for (int64_t x = 0; x < half[0]; ++x) {
          std::fill(sum.begin(), sum.end(), 0.f);
          int count = 0;
          for (int corner = 0; corner < 8; ++corner) {
            const int64_t fx = 2 * x + (corner & 1), fy = 2 * y + ((corner >> 1) & 1), fz = 2 * z + (corner >> 2);
            if (fx >= size[0] || fy >= size[1] || fz >= size[2]) {
              continue;
            }
            const size_t p = (size_t(fz) * size_t(size[1]) + size_t(fy)) * size_t(size[0]) + size_t(fx);
            const float* mean = &g[p * 3];
            if (!((mean[0] + mean[1]) + mean[2] > 0)) {
              continue;
            }
            bool finite = true;
            for (size_t k = 0; k < 18; ++k) {
              finite = finite && std::isfinite(g[(k / 3) * points * 3 + p * 3 + k % 3]);
            }
            if (!finite) {
              continue;
            }
            for (size_t k = 0; k < 18; ++k) {
              sum[k] += g[(k / 3) * points * 3 + p * 3 + k % 3];
            }
            ++count;
          }
          if (count == 0) {
            continue;
          }
          const size_t q = (size_t(z) * size_t(half[1]) + size_t(y)) * size_t(half[0]) + size_t(x);
          for (size_t k = 0; k < 18; ++k) {
            coarse[(k / 3) * fewer * 3 + q * 3 + k % 3] = sum[k] / float(count);
          }
        }
      }
    }
    g = std::move(coarse);
    points = fewer;
    for (int i = 0; i < 3; ++i) {
      size[i] = half[i];
      for (int j = 0; j < 3; ++j) {
        m[i][j] /= 2;
      }
      // Point k of the coarse grid sits between points 2k and 2k + 1 of the fine one.
      m[i][3] = (m[i][3] - 0.5) / 2;
    }
  }

  const size_t plane = points * 3;
  auto at = [&](size_t index, size_t point) { return &g[index * plane + point * 3]; };
  auto clip = [](float v) { return std::isnan(v) ? v : std::min(std::max(v, 0.f), 60000.f); };
  auto u8 = [](float v) {
    const float r = std::nearbyint(v * 255.f);
    return std::isnan(r) ? uint8_t(0) : uint8_t(std::min(std::max(r, 0.f), 255.f));
  };

  std::vector<uint8_t> grid;
  grid.reserve(60 + points * 24);
  std::vector<bool> lit(points);
  size_t litCount = 0;
  std::vector<uint8_t> pts(points * 24, 0);
  const float floor = 1e-4f;
  for (size_t i = 0; i < points; ++i) {
    float mean[3], lobe[3];
    for (int k = 0; k < 3; ++k) {
      mean[k] = clip(at(0, i)[k]);
      lobe[k] = clip(at(1, i)[k]);
    }
    if (!((mean[0] + mean[1]) + mean[2] > 0)) {
      continue;
    }
    lit[i] = true;
    ++litCount;
    uint8_t* p = &pts[i * 24];
    for (int k = 0; k < 3; ++k) {
      // So a lit point never packs as an empty one.
      const float m = std::isnan(mean[k]) ? mean[k] : std::max(mean[k], floor);
      const uint16_t hm = FloatToHalf(m), hl = FloatToHalf(lobe[k]);
      p[2 * k] = uint8_t(hm);
      p[2 * k + 1] = uint8_t(hm >> 8);
      p[6 + 2 * k] = uint8_t(hl);
      p[6 + 2 * k + 1] = uint8_t(hl >> 8);
      p[12 + k] = u8(at(2, i)[k]);
      for (int ch = 0; ch < 3; ++ch) {
        p[15 + ch * 3 + k] = u8(at(size_t(3 + ch), i)[k]);
      }
    }
  }
  if (litCount == 0) {
    note = "grid empty";
    return false;
  }

  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      AppendLEFloat(out, m[i][j]);
    }
  }
  for (int i = 0; i < 3; ++i) {
    AppendLE32(out, uint32_t(size[i]));
  }
  out.insert(out.end(), pts.begin(), pts.end());

  char buf[96];
  std::snprintf(buf, sizeof buf, "grid %lldx%lldx%lld %.0f%% lit", (long long)size[0], (long long)size[1],
                (long long)size[2], 100.0 * double(litCount) / double(points));
  note = buf;
  if (borrowed > 0) {
    note += ", " + std::to_string(borrowed) + " borrowed blocks";
  }
  if (!check.empty()) {
    int ok = 0;
    for (const Vec3& c : check) {
      int64_t q[3];
      for (int i = 0; i < 3; ++i) {
        q[i] = int64_t(std::nearbyint((c[0] * m[i][0] + c[1] * m[i][1] + c[2] * m[i][2]) + m[i][3]));
      }
      const bool inside = q[0] >= 0 && q[1] >= 0 && q[2] >= 0 && q[0] < size[0] && q[1] < size[1] && q[2] < size[2];
      if (inside && lit[(size_t(q[2]) * size_t(size[1]) + size_t(q[1])) * size_t(size[0]) + size_t(q[0])]) {
        ++ok;
      }
    }
    note += ", " + std::to_string(ok) + "/" + std::to_string(check.size()) + " doors on lit points";
  }
  return true;
}

// An MCON's instances: instance i draws models[index[i]] at transforms[i], twelve
// floats that are the rows of a 3x4 matrix in the room's own frame.
struct Mcon {
  std::vector<Id16> models;  // in a pak's byte order
  std::vector<uint16_t> index;
  const uint8_t* transforms = nullptr;
  // Per instance, the lightmap lookup (offU, offV, scale, 0); empty when the MCON has none.
  std::vector<std::array<float, 4>> lookups;
};

bool ReadMcon(const std::vector<uint8_t>& d, Mcon& out) {
  if (d.size() < 32 || ReadBE32(&d[0]) != Tag("RFRM") || ReadLE64(&d[4]) > d.size() - 32) {
    return false;
  }
  const size_t end = 32 + size_t(ReadLE64(&d[4]));
  for (size_t o = 32; o + 24 <= end && ReadBE32(&d[o]) != Tag("PEEK");) {
    const uint64_t size = ReadLE64(&d[o + 4]);
    const size_t start = o + 24;
    if (size > end - start) {
      return false;
    }
    if (ReadBE32(&d[o]) == Tag("MCVD")) {
      size_t p = start;
      const size_t stop = start + size_t(size);
      // A counted vector of `width` byte items; null when it runs past the chunk.
      auto vec = [&](size_t width, size_t& count) -> const uint8_t* {
        if (stop - p < 4) {
          return nullptr;
        }
        count = ReadLE32(&d[p]);
        p += 4;
        if (count > (stop - p) / width) {
          return nullptr;
        }
        const uint8_t* at = d.data() + p;
        p += count * width;
        return at;
      };
      size_t models = 0, other = 0, transforms = 0, indices = 0;
      const uint8_t* m = vec(16, models);
      // Then ids, colours, the instance transforms, object transforms and a byte each.
      if (m == nullptr || vec(16, other) == nullptr || vec(16, other) == nullptr) {
        return false;
      }
      out.transforms = vec(48, transforms);
      if (out.transforms == nullptr || vec(64, other) == nullptr || vec(1, other) == nullptr) {
        return false;
      }
      const uint8_t* ix = vec(2, indices);
      if (ix == nullptr || indices != transforms) {
        return false;
      }
      for (size_t i = 0; i < models; ++i) {
        out.models.push_back(SwapUuid(m + 16 * i));
      }
      for (size_t i = 0; i < indices; ++i) {
        out.index.push_back(ReadLE16(ix + 2 * i));
      }
      // Then a vec(2) of zeros, a vec(1) per instance and one per model, and the lookups.
      // They are optional: anything that does not fit leaves the MCON without.
      size_t lookups = 0;
      if (vec(2, other) != nullptr && vec(1, other) != nullptr && vec(1, other) != nullptr) {
        const uint8_t* lm = vec(16, lookups);
        if (lm != nullptr && lookups == indices) {
          for (size_t i = 0; i < lookups; ++i) {
            std::array<float, 4> v;
            for (size_t k = 0; k < 4; ++k) {
              v[k] = ReadLEFloat(lm + 16 * i + 4 * k);
            }
            out.lookups.push_back(v);
          }
        }
      }
      return true;
    }
    o = start + size_t(size);
  }
  return false;
}

void Writer::WriteGeometry(const RoomData& r, uint32_t mrea, const Area& area) {
  if (!m_io.model || (m_io.wantsGeometry && !m_io.wantsGeometry(r.name))) {
    return;
  }
  // kR2G as a signed permutation: gc[i] = kSign[i] * remastered[kAxis[i]].
  static const int kAxis[3] = {0, 2, 1};
  static const double kSign[3] = {-1, 1, 1};
  const RoomPak home{r.name, r.pak};
  std::vector<PortRoomGeo::Instance> instances;
  size_t dropped = 0;
  SceneryScripts scripts = MatchScripts(r.room, area);  // `pieces` adds the objects hidden
  // Whether an entity that starts inactive can be shown: by a retail object (links) or by
  // Remastered's own script. One that cannot is never seen: the ships of the landing
  // cutscene sit in the sky and on the pad, where retail's own ship already is.
  auto canShow = [&](int entity) {
    const auto links = scripts.links.find(entity);
    return scripts.scriptShows.count(entity) != 0 ||
           (links != scripts.links.end() &&
            std::any_of(links->second.begin(), links->second.end(),
                        [](const PortRoomGeo::Link& l) { return l.action != PortRoomGeo::kHide; }));
  };
  size_t gated = 0, linked = 0, grouped = 0, inactive = 0, unresolved = 0;
  // What the entity's scripts make of an instance: its layer, whether it starts shown, and
  // what shows and hides it.
  auto script = [&](PortRoomGeo::Instance& inst, int entity, bool active) {
    inst.active = active;
    const auto layer = scripts.layer.find(entity);
    if (layer != scripts.layer.end() && layer->second != 0) {
      // Layer 0 is always on; leaving it unset skips the lookup.
      inst.layer = layer->second;
      ++gated;
    }
    const auto links = scripts.links.find(entity);
    if (links != scripts.links.end()) {
      inst.links = links->second;
      ++linked;
    }
    const auto group = scripts.group.find(entity);
    if (group != scripts.group.end()) {
      inst.group = group->second;
      ++grouped;
    }
    if (scripts.unresolved.count(entity) != 0) {
      ++unresolved;
    }
  };
  // An entity's transform in GC axes: Rz * Ry * Rx of its angles, as retail builds an
  // editor transform, then its scale and position.
  auto place = [&](PortRoomGeo::Instance& inst, const Vec3& pos, const Vec3& rot, const Vec3& scale) {
    double s[3], k[3];
    for (int i = 0; i < 3; ++i) {
      s[i] = std::sin(rot[i] * (3.14159265358979323846 / 180.0));
      k[i] = std::cos(rot[i] * (3.14159265358979323846 / 180.0));
    }
    const double m[3][3] = {
        {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
        {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
        {-s[1], k[1] * s[0], k[1] * k[0]},
    };
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        inst.transform[4 * row + col] = float(kSign[row] * kSign[col] * m[kAxis[row]][kAxis[col]] * scale[kAxis[col]]);
      }
      inst.transform[4 * row + 3] = float(kSign[row] * pos[kAxis[row]]);
    }
  };
  // The instance's slot in the room's baked lightmap, when the lightmap loaded and the
  // lookup is one (a scale above 0).
  const bool lightmapOk = Lightmap(r).ok;
  size_t lookups = 0;
  auto setLookup = [&](PortRoomGeo::Instance& inst, const float* v) {
    if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && v[2] > 0.f) {
      inst.lightmap[0] = v[0];
      inst.lightmap[1] = v[1];
      inst.lightmap[2] = v[2];
      ++lookups;
    }
  };
  size_t modcons = 0;
  for (const Component* c : r.room.Of(kModCon)) {
    const bool active = r.room.Active(*c);
    if (!active && !canShow(c->entity)) {
      ++inactive;
      continue;
    }
    const auto f = r.room.Flat(*c);
    const auto prop = f.find(kPropModConMcon);
    std::vector<uint8_t> data;
    Mcon mcon;
    if (prop == f.end() || prop->second.size < 16 ||
        !FindResource(r.room.Bytes(prop->second), Tag("MCON"), home, data, nullptr, nullptr)) {
      continue;
    }
    if (!ReadMcon(data, mcon)) {
      Log("  " + r.name + ": unreadable MCON");
      continue;
    }
    // The instances are in room space, not relative to the component's entity: where an
    // entity is off the origin, adding its position moves the geometry off the room's doors.
    std::vector<int> state(mcon.models.size(), 0);  // 1 converted, 2 not
    std::vector<uint32_t> ids(mcon.models.size(), 0);
    for (size_t i = 0; i < mcon.index.size(); ++i) {
      const size_t model = mcon.index[i];
      if (model >= mcon.models.size()) {
        ++dropped;
        continue;
      }
      if (state[model] == 0) {
        if (m_io.cancelled && m_io.cancelled()) {
          return;
        }
        state[model] = m_io.model(mcon.models[model], ids[model]) ? 1 : 2;
      }
      if (state[model] != 1) {
        ++dropped;
        continue;
      }
      const uint8_t* t = mcon.transforms + 48 * i;
      PortRoomGeo::Instance& inst = instances.emplace_back();
      inst.model = ids[model];
      for (int row = 0; row < 3; ++row) {
        const uint8_t* from = t + 16 * kAxis[row];
        for (int col = 0; col < 3; ++col) {
          inst.transform[4 * row + col] = float(kSign[row] * kSign[col] * double(ReadLEFloat(from + 4 * kAxis[col])));
        }
        inst.transform[4 * row + 3] = float(kSign[row] * double(ReadLEFloat(from + 12)));
      }
      script(inst, c->entity, active);
      if (lightmapOk && i < mcon.lookups.size()) {
        setLookup(inst, mcon.lookups[i].data());
      }
      ++modcons;
    }
  }
  // A world model (WMDL) that the room only lists among its resources, which no component
  // places: a piece of the room's own geometry in room space, such as the grating floor of
  // Elite Quarters Access (00o) that no MCON has. A WMDL a component names is a liquid's.
  for (const size_t at : r.room.Listed()) {
    std::vector<uint8_t> data;
    uint32_t id = 0;
    if (r.room.NamedElsewhere(at) ||
        !FindResource(r.room.Bytes({at, 16}), Tag("WMDL"), home, data, nullptr, nullptr)) {
      continue;
    }
    if (m_io.cancelled && m_io.cancelled()) {
      return;
    }
    if (!m_io.model(SwapUuid(r.room.Bytes({at, 16})), id)) {
      ++dropped;
      continue;
    }
    PortRoomGeo::Instance& inst = instances.emplace_back();
    inst.model = id;
    for (int i = 0; i < 3; ++i) {
      inst.transform[5 * i] = 1.0f;
    }
    char line[96];
    std::snprintf(line, sizeof line, "  %s: world model %08X", r.name.c_str(), id);
    Log(line);
  }
  // Scenery Remastered added as actors rather than as room geometry: the frame around each
  // door, and pieces of the room itself that sit on its "RS" layer. They carry kPropActorAdded,
  // which no actor that retail also has does. Each is drawn on the retail layer its own layer
  // stands for, shown and hidden by the retail objects that drive it, and moved by the
  // platform that carries it, if one does (MatchScripts). Property 1285da4d, which some
  // carry, names no parent: most actors with it stand still, and the room's script
  // connections are what make one a platform's.
  std::map<Id16, uint32_t> actorModels;  // 0 when the model did not convert
  size_t actors = 0, riding = 0, glowing = 0, retailDrawn = 0, animated = 0;
  // Characters read so far, by pak id; null when one did not read.
  std::map<Id16, std::unique_ptr<PortRemasteredAnim::Character>> characters;
  // By component index: the glow its incandescence modulator gives it. A channel or
  // intensity the modulator leaves out is 1. The door frames' modulators fade in no time
  // and nothing starts them, so the glow holds from the start; one something else starts
  // (6 in all) leaves its actor as the material has it.
  std::map<size_t, std::array<float, 3>> glows;
  const std::vector<Component>& comps = r.room.Components();
  const std::vector<Connection> links = ReadConnections(r.room);
  std::set<int> started;
  for (const Connection& link : links) {
    started.insert(r.room.ByGuid(link.target));
  }
  for (const Connection& link : links) {
    const Component& from = comps[link.sender];
    const int target = r.room.ByGuid(link.target);
    if (from.type != kColorModulateMP1 || started.count(int(link.sender)) != 0 || target < 0 ||
        comps[size_t(target)].type != kActorMP1) {
      continue;
    }
    const auto f = r.room.Flat(from);
    const auto blend = f.find(kPropModulateBlend);
    if (blend == f.end() || blend->second.size != 4 || ReadLE32(r.room.Bytes(blend->second)) != kModulateIncandescence) {
      continue;
    }
    const auto intensity = f.find(kPropModulateIntensity);
    const float scale =
        intensity != f.end() && intensity->second.size == 4 ? ReadLEFloat(r.room.Bytes(intensity->second)) : 1.f;
    std::array<float, 3> glow;
    for (int i = 0; i < 3; ++i) {
      Span s;
      glow[i] = r.room.Nested(from, {kPropModulateColorB, kPropColorChannel[i]}, s) && s.size == 4
                    ? ReadLEFloat(r.room.Bytes(s))
                    : 1.f;
      glow[i] = std::isfinite(glow[i] * scale) ? std::max(glow[i] * scale, 0.f) : 0.f;
    }
    glows[size_t(target)] = glow;
  }
  // Entities a Timer hides. One that also starts hidden is shown only while an event plays:
  // the waterfalls of Ice Shorelines and the Hive Totem pour for 17 and 6.25 s and fade with
  // their Waterfalls.GRDU driver, and pieces of the Frigate's hangar flash for 0.25 s. Their
  // links keep the timers' delays (Link::delay), but not the fades, so they are left out. An
  // animated actor is drawn: the Mines turbine's explosion, shown 5.5 s after its pickup is
  // taken and hidden 2.5 s later, partway through its clip.
  std::set<int> timedOff;
  for (const Connection& link : links) {
    const int target = r.room.ByGuid(link.target);
    if (target >= 0 && RetailType(comps[link.sender].type) == kRetailTimer &&
        LinkAct(link.action) == PortRoomGeo::kHide) {
      timedOff.insert(comps[size_t(target)].entity >= 0 ? comps[size_t(target)].entity : target);
    }
  }
  // An animated actor whose character has more than one bone: the hangar's floating debris,
  // the Mines spinner's and Omega's tank's explosions, the Omega Pirate's death in the phazon
  // pool. Their joints move rigid pieces (each triangle is on the joint that weighs most in
  // it, TriangleJoint), so each joint's triangles are a model of their own (RoomIO::piece),
  // posed by the joint's skin matrix. Its clips: the actor's own animation, looped when it
  // starts shown and else played once from each time it is shown, then the animation of each
  // ActorKeyframe that plays it (SceneryScripts::keyframes), started by the keyframe's Play.
  std::map<std::pair<Id16, int>, uint32_t> pieceModels;  // 0 when the piece did not convert
  auto pieces = [&](const Component& c, const PortRemasteredAnim::Character& ch, const PortRemasteredAnim::Anim& own,
                    const Id16& model, const Vec3& pos, const Vec3& rot, const Vec3& scale, bool active) {
    std::vector<uint8_t> data;
    PortRemastered::Model smdl;
    std::string error;
    if (!m_io.piece || !FindResource(ch.skinnedModel.data(), Tag("SMDL"), home, data, nullptr, nullptr) ||
        !PortRemastered::ParseModel(data.data(), data.size(), smdl, error)) {
      Log("  " + r.name + ": an animated actor's skinned model did not read" + (error.empty() ? "" : ": " + error));
      return false;
    }
    std::set<int> joints;
    for (const PortRemastered::ModelMesh& mesh : smdl.meshes) {
      if (mesh.vertexBuffer >= smdl.vertexBuffers.size()) {
        continue;
      }
      for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        joints.insert(PortRemastered::TriangleJoint(smdl.vertexBuffers[mesh.vertexBuffer], &mesh.indices[t]));
      }
    }
    if (joints.empty() || *joints.begin() < 0 || size_t(*joints.rbegin()) >= ch.inverseBind.size()) {
      Log("  " + r.name + ": an animated actor's model is not skinned to its character");
      return false;
    }
    struct Clip {
      const PortRemasteredAnim::Anim* anim;
      bool loop;
    };
    std::vector<Clip> clips{{&own, active}};
    auto order = scripts.keyframes.find(c.entity);
    if (order != scripts.keyframes.end()) {
      std::vector<size_t> senders = order->second;
      std::sort(senders.begin(), senders.end());
      senders.erase(std::unique(senders.begin(), senders.end()), senders.end());
      for (const size_t k : senders) {
        const auto f = r.room.Flat(comps[k]);
        const auto name = f.find(kPropKeyframeAnim);
        const auto loop = f.find(kPropKeyframeLoop);
        std::string text;
        const PortRemasteredAnim::Anim* anim =
            name != f.end() && name->second.size == 8 &&
                    r.room.String(ReadLE32(r.room.Bytes(name->second)), ReadLE32(r.room.Bytes(name->second) + 4), text)
                ? PortRemasteredAnim::Find(ch, text)
                : nullptr;
        if (anim == nullptr || anim->frames < 2 || !(anim->fps > 0.f)) {
          Log("  " + r.name + ": a keyframe's animation \"" + text + "\" is not in its character");
          continue;
        }
        clips.push_back({anim, loop != f.end() && loop->second.size == 1 && r.room.Bytes(loop->second)[0] != 0});
      }
    }
    // Per clip, per frame, the joints' skin matrices in GC axes (R' = A R A^T, t' = A t for the
    // axis change A), as rotation quaternions and translations. A matrix that is not a
    // rotation (a joint that scales or shears) cannot be a rigid piece's pose.
    std::vector<std::map<int, std::vector<float>>> keys(clips.size());
    std::vector<std::array<float, 12>> skin;
    for (size_t i = 0; i < clips.size(); ++i) {
      for (uint32_t frame = 0; frame < clips[i].anim->frames; ++frame) {
        if (!PortRemasteredAnim::SkinPose(ch, *clips[i].anim, frame, skin)) {
          Log("  " + r.name + ": an animated actor's skeleton did not pose");
          return false;
        }
        for (const int j : joints) {
          const std::array<float, 12>& s = skin[size_t(j)];
          double m[3][3], t[3];
          for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
              m[row][col] = kSign[row] * kSign[col] * double(s[4 * kAxis[row] + kAxis[col]]);
            }
            t[row] = kSign[row] * double(s[4 * kAxis[row] + 3]);
          }
          double worst = 0;
          for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
              const double dot = m[0][a] * m[0][b] + m[1][a] * m[1][b] + m[2][a] * m[2][b];
              worst = std::max(worst, std::fabs(dot - (a == b ? 1.0 : 0.0)));
            }
          }
          const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
          if (!(worst < 0.02) || !(det > 0)) {
            Log("  " + r.name + ": animation \"" + clips[i].anim->name + "\" scales joint " + std::to_string(j));
            return false;
          }
          // The quaternion of m, from its largest component.
          double q[4];
          const double trace = m[0][0] + m[1][1] + m[2][2];
          if (trace > 0) {
            const double w = std::sqrt(1 + trace) * 2;
            q[3] = w / 4;
            q[0] = (m[2][1] - m[1][2]) / w;
            q[1] = (m[0][2] - m[2][0]) / w;
            q[2] = (m[1][0] - m[0][1]) / w;
          } else {
            int a = 0;
            if (m[1][1] > m[a][a]) {
              a = 1;
            }
            if (m[2][2] > m[a][a]) {
              a = 2;
            }
            const int b = (a + 1) % 3, d = (a + 2) % 3;
            const double w = std::sqrt(1 + m[a][a] - m[b][b] - m[d][d]) * 2;
            q[a] = w / 4;
            q[b] = (m[b][a] + m[a][b]) / w;
            q[d] = (m[d][a] + m[a][d]) / w;
            q[3] = (m[d][b] - m[b][d]) / w;
          }
          const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
          std::vector<float>& out = keys[i][j];
          for (int k = 0; k < 4; ++k) {
            out.push_back(float(q[k] / len));
          }
          for (int k = 0; k < 3; ++k) {
            out.push_back(float(t[k]));
          }
        }
      }
    }
    // Each piece's bounds as its first clip starts, in world space, and the retail objects in
    // them it stands for (Script::hidden): debris, and Actors in the same Active state - the
    // hangar's floating debris, but not Omega's whole tank, which our explosion replaces only
    // once it is shown.
    if (!PortRemasteredAnim::SkinPose(ch, *clips[0].anim, 0, skin)) {
      return false;
    }
    PortRoomGeo::Instance placed;
    place(placed, pos, rot, scale);
    std::map<int, std::pair<Vec3, Vec3>> bounds;
    for (const PortRemastered::ModelMesh& mesh : smdl.meshes) {
      if (mesh.vertexBuffer >= smdl.vertexBuffers.size()) {
        continue;
      }
      const PortRemastered::ModelVertexBuffer& vb = smdl.vertexBuffers[mesh.vertexBuffer];
      for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const int j = PortRemastered::TriangleJoint(vb, &mesh.indices[t]);
        const std::array<float, 12>& s = skin[size_t(j)];
        auto box = bounds.try_emplace(j, Vec3{HUGE_VAL, HUGE_VAL, HUGE_VAL}, Vec3{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL}).first;
        for (size_t k = 0; k < 3; ++k) {
          const size_t v = mesh.indices[t + k];
          if (3 * v + 2 >= vb.positions.size()) {
            continue;
          }
          const float* p = &vb.positions[3 * v];
          Vec3 posed, gc, local;
          for (int row = 0; row < 3; ++row) {
            posed[size_t(row)] = s[4 * row] * p[0] + s[4 * row + 1] * p[1] + s[4 * row + 2] * p[2] + s[4 * row + 3];
          }
          for (int row = 0; row < 3; ++row) {
            gc[size_t(row)] = kSign[row] * posed[size_t(kAxis[row])];
          }
          for (int row = 0; row < 3; ++row) {
            local[size_t(row)] = placed.transform[4 * row] * gc[0] + placed.transform[4 * row + 1] * gc[1] +
                                 placed.transform[4 * row + 2] * gc[2] + placed.transform[4 * row + 3];
          }
          const Vec3 w = Apply(area.xf, local);
          for (size_t a = 0; a < 3; ++a) {
            box->second.first[a] = std::min(box->second.first[a], w[a]);
            box->second.second[a] = std::max(box->second.second[a], w[a]);
          }
        }
      }
    }
    // How far a retail object's origin is outside the nearest piece (0 inside).
    auto outside = [&](const Vec3& at) {
      double best = HUGE_VAL;
      for (const auto& box : bounds) {
        double d = 0;
        for (size_t a = 0; a < 3; ++a) {
          d = std::max({d, box.second.first[a] - at[a], at[a] - box.second.second[a]});
        }
        best = std::min(best, d);
      }
      return best;
    };
    auto isDebris = [](const ScriptObject& o) { return o.type == kRetailDebris || o.type == kRetailDebrisExtended; };
    std::vector<uint32_t> hidden;
    for (const ScriptObject& o : area.objects) {
      if (!o.hasPos) {
        continue;
      }
      bool hide = isDebris(o) && outside(o.pos) <= kPieceDebrisNear;
      if (o.type == kRetailActor && o.propCount == kRetailActorProps && o.props.size() >= kRetailActorActiveFromEnd &&
          (o.props[o.props.size() - kRetailActorActiveFromEnd] != 0) == active) {
        // An Actor whose debris starts where it stands is that debris's intact form.
        const bool twin = std::any_of(area.objects.begin(), area.objects.end(), [&](const ScriptObject& d) {
          return isDebris(d) && d.hasPos && MaxAbs(d.pos, o.pos) < 0.01;
        });
        hide = outside(o.pos) <= (twin ? kPieceDebrisNear : kPieceNear);
      }
      if (hide) {
        hidden.push_back(o.id & 0x3FFFFFFu);  // as TEditorId::Value, without the layer
      }
    }
    size_t made = 0;
    const size_t first = instances.size();  // the first piece, whose shown state hides them
    std::string models;
    for (const int j : joints) {
      auto known = pieceModels.find({model, j});
      if (known == pieceModels.end()) {
        uint32_t id = 0;
        known = pieceModels.emplace(std::make_pair(model, j), m_io.piece(model, j, id) ? id : 0).first;
      }
      if (known->second == 0) {
        continue;
      }
      PortRoomGeo::Instance& inst = instances.emplace_back();
      inst.model = known->second;
      place(inst, pos, rot, scale);
      inst.animOnShow = !active;
      for (size_t i = 0; i < clips.size(); ++i) {
        PortRoomGeo::Instance::AnimClip& clip = inst.anim.emplace_back();
        clip.fps = clips[i].anim->fps;
        clip.loop = clips[i].loop;
        clip.keys = std::move(keys[i][j]);
      }
      script(inst, c.entity, active);
      ++made;
      char hex[10];
      std::snprintf(hex, sizeof hex, " %08X", known->second);
      models += hex;
    }
    std::string names;
    for (const Clip& clip : clips) {
      char length[24];
      std::snprintf(length, sizeof length, " %.2f s", clip.anim->fps > 0 ? clip.anim->frames / clip.anim->fps : 0.f);
      names += (names.empty() ? "" : ", ") + clip.anim->name + length + (clip.loop ? " (loop)" : "");
    }
    if (const auto links = scripts.links.find(c.entity); links != scripts.links.end()) {
      names += ", linked from";
      for (const PortRoomGeo::Link& l : links->second) {
        char hex[48];
        std::snprintf(hex, sizeof hex, " %08X (%u: %u after %.2f s)", l.sender, l.state, l.action, l.delay);
        names += hex;
      }
    }
    if (made != 0) {
      for (const uint32_t id : hidden) {
        scripts.script.hidden.push_back({id, uint32_t(first)});
      }
    }
    Log("  " + r.name + ": an actor's " + names + " in " + std::to_string(made) + " of " +
        std::to_string(joints.size()) + " pieces:" + models + "; " + std::to_string(made != 0 ? hidden.size() : 0) +
        " retail objects hidden");
    return made != 0;
  };
  // Whether a static model's bounds hold its origin, give or take half a metre.
  auto aroundOrigin = [&](const uint8_t* id) {
    std::vector<uint8_t> data;
    PortRemastered::Model cmdl;
    std::string error;
    if (!FindResource(id, Tag("CMDL"), home, data, nullptr, nullptr) ||
        !PortRemastered::ParseModel(data.data(), data.size(), cmdl, error)) {
      return false;
    }
    Vec3 lo{HUGE_VAL, HUGE_VAL, HUGE_VAL}, hi{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
    for (const PortRemastered::ModelVertexBuffer& vb : cmdl.vertexBuffers) {
      for (size_t v = 0; v + 2 < vb.positions.size(); v += 3) {
        for (size_t k = 0; k < 3; ++k) {
          lo[k] = std::min(lo[k], double(vb.positions[v + k]));
          hi[k] = std::max(hi[k], double(vb.positions[v + k]));
        }
      }
    }
    for (size_t k = 0; k < 3; ++k) {
      if (!(lo[k] <= 0.5 && hi[k] >= -0.5)) {
        return false;
      }
    }
    return true;
  };
  size_t posed = 0;
  for (const Component* c : r.room.Of(kActorMP1)) {
    const auto f = r.room.Flat(*c);
    const auto prop = f.find(kPropActorModel);
    const bool hasModel = prop != f.end() && prop->second.size == 16;
    Span chprProp, nameProp;
    const bool hasAnim = !hasModel && r.room.Nested(*c, {kPropActorAnim, kPropAnimCharacter}, chprProp) &&
                         chprProp.size == 16 && r.room.Nested(*c, {kPropActorAnim, kPropAnimName}, nameProp) &&
                         nameProp.size == 8;
    Vec3 pos, rot, scale;
    if (f.find(kPropActorAdded) == f.end() || (!hasModel && !hasAnim) || !r.room.Xform(*c, pos, rot, scale)) {
      continue;
    }
    const bool active = r.room.Active(*c);
    if (!active && (!canShow(c->entity) || (timedOff.count(c->entity) != 0 && !hasAnim))) {
      ++inactive;
      continue;
    }
    // A model-less actor that plays an animation draws its character's skinned model,
    // posed by the animation's frames. A character of one bone (the Intro Elevator rings,
    // whose bone binds at the model's origin) is drawn whole, the pose moving the whole
    // model; one of more is cut into rigid pieces (`pieces`).
    const PortRemasteredAnim::Anim* anim = nullptr;
    Id16 model{};
    if (hasModel) {
      model = SwapUuid(r.room.Bytes(prop->second));
    } else {
      const Id16 key = SwapUuid(r.room.Bytes(chprProp));
      auto ch = characters.find(key);
      if (ch == characters.end()) {
        std::vector<uint8_t> data;
        std::string error;
        auto character = std::make_unique<PortRemasteredAnim::Character>();
        if (!FindResource(r.room.Bytes(chprProp), Tag("CHPR"), home, data, nullptr, nullptr) ||
            !PortRemasteredAnim::ReadCharacter(data, *character, error)) {
          Log("  " + r.name + ": a character did not read" + (error.empty() ? "" : ": " + error));
          character.reset();
        }
        ch = characters.emplace(key, std::move(character)).first;
      }
      std::string name;
      if (ch->second != nullptr &&
          r.room.String(ReadLE32(r.room.Bytes(nameProp)), ReadLE32(r.room.Bytes(nameProp) + 4), name)) {
        anim = PortRemasteredAnim::Find(*ch->second, name);
      }
      if (anim == nullptr || anim->frames < 2 || !(anim->fps > 0.f)) {
        ++dropped;
        continue;
      }
      model = SwapUuid(ch->second->skinnedModel.data());
      if (model == Id16{}) {
        ++dropped;
        continue;
      }
      if (anim->bones.size() != 1 || anim->bones[0].size() != anim->frames) {
        if (m_io.cancelled && m_io.cancelled()) {
          return;
        }
        if (!pieces(*c, *ch->second, *anim, model, pos, rot, scale, active)) {
          ++dropped;
          continue;
        }
        ++animated;
        ++actors;
        continue;
      }
    }
    if (model == Id16{}) {
      continue;
    }
    // A static one that starts hidden at the room's origin, its model built around its own
    // origin, is posed by a cinematic, which the port does not play: the gunships of the
    // Frigate hangar's landing cutscene, shown by retail's links, would stand whole on the
    // dock. One whose model lies off its origin is room geometry placed at the origin (the
    // Frigate reactor's fallen pieces), and stays.
    if (hasModel && !active && MaxAbs(pos, Vec3{}) < 0.01 && aroundOrigin(r.room.Bytes(prop->second))) {
      ++posed;
      continue;
    }
    // Some carry a retail model (Remastered's id 10000000-0000-f000-f000-0000XXXXXXXX)
    // and stand where a retail Actor with that model already does, a little moved at most:
    // the pipes and fittings of the Frigate's hangar. The retail actor draws it, as the
    // Remastered model when the import table replaces it, so it is not written again.
    if (const uint8_t* id = hasModel ? r.room.Bytes(prop->second) : nullptr;
        id != nullptr && std::memcmp(id, kRetailIdPrefix, 12) == 0) {
      const Vec3 w = Apply(area.xf, MulR2G(pos));
      const bool standing = std::any_of(area.objects.begin(), area.objects.end(), [&](const ScriptObject& o) {
        return o.type == kRetailActor && o.hasPos && MaxAbs(o.pos, w) < kRetailActorNear &&
               std::search(o.props.begin(), o.props.end(), id + 12, id + 16) != o.props.end();
      });
      if (standing) {
        ++retailDrawn;
        continue;
      }
    }
    auto known = actorModels.find(model);
    if (known == actorModels.end()) {
      if (m_io.cancelled && m_io.cancelled()) {
        return;
      }
      uint32_t id = 0;
      known = actorModels.emplace(model, m_io.model(model, id) ? id : 0).first;
    }
    if (known->second == 0) {
      ++dropped;
      continue;
    }
    PortRoomGeo::Instance& inst = instances.emplace_back();
    inst.model = known->second;
    place(inst, pos, rot, scale);
    if (anim != nullptr) {
      // The bone's pose in GC axes: the axis change is a rotation, so a quaternion's
      // vector part and a translation change the same way a position does.
      PortRoomGeo::Instance::AnimClip& clip = inst.anim.emplace_back();
      clip.fps = anim->fps;
      clip.keys.reserve(size_t(anim->frames) * 7);
      for (const PortRemasteredAnim::Key& key : anim->bones[0]) {
        for (int i = 0; i < 3; ++i) {
          clip.keys.push_back(float(kSign[i] * key.rotation[kAxis[i]]));
        }
        clip.keys.push_back(key.rotation[3]);
        for (int i = 0; i < 3; ++i) {
          clip.keys.push_back(float(kSign[i] * key.translation[kAxis[i]]));
        }
      }
      ++animated;
    }
    script(inst, c->entity, active);
    if (lightmapOk && c->hasGuid) {
      const auto lookup = r.room.Lighting().lookups.find(c->guid);
      if (lookup != r.room.Lighting().lookups.end()) {
        setLookup(inst, lookup->second.data());
      }
    }
    const auto glow = glows.find(size_t(c - comps.data()));
    if (glow != glows.end()) {
      inst.glows = true;
      std::copy(glow->second.begin(), glow->second.end(), inst.glow);
      ++glowing;
    }
    const auto platform = scripts.platform.find(c->entity);
    if (platform != scripts.platform.end()) {
      inst.platform = platform->second->id;
      for (int i = 0; i < 3; ++i) {
        inst.platformStart[i] = float(platform->second->pos[i]);
      }
      ++riding;
    }
    ++actors;
  }
  // The room's skies, drawn in place of the world's: retail has one per world, Remastered
  // one per room, each turned and scaled to suit it (and two in the Frigate's hangar, one
  // on a layer).
  size_t skies = 0;
  for (const Component* c : r.room.Of(kSkybox)) {
    const auto f = r.room.Flat(*c);
    const auto prop = f.find(kPropSkyboxModel);
    Vec3 pos, rot, scale;
    if (prop == f.end() || prop->second.size != 16 || !r.room.Xform(*c, pos, rot, scale)) {
      continue;
    }
    const bool active = r.room.Active(*c);
    if (!active && !canShow(c->entity)) {
      ++inactive;
      continue;
    }
    const Id16 model = SwapUuid(r.room.Bytes(prop->second));
    if (m_io.cancelled && m_io.cancelled()) {
      return;
    }
    uint32_t id = 0;
    if (model == Id16{} || !m_io.model(model, id)) {
      ++dropped;
      continue;
    }
    PortRoomGeo::Instance& inst = instances.emplace_back();
    inst.model = id;
    inst.sky = true;
    const auto intensity = f.find(kPropSkyboxIntensity);
    const float level =
        intensity != f.end() && intensity->second.size == 4 ? ReadLEFloat(r.room.Bytes(intensity->second)) : 1.f;
    const auto colour = f.find(kPropSkyboxColor);
    for (int i = 0; i < 3; ++i) {
      Span s;
      const float channel = colour != f.end() && colour->second.size >= 12 ? ReadLEFloat(r.room.Bytes(colour->second) + 4 * i)
                            : r.room.Nested(*c, {kPropSkyboxColor, kPropColorChannel[i]}, s) && s.size == 4
                                ? ReadLEFloat(r.room.Bytes(s))
                                : 1.f;
      inst.skyRadiance[i] = std::isfinite(channel * level) ? std::max(channel * level, 0.f) : 0.f;
    }
    place(inst, Vec3{}, rot, scale);
    script(inst, c->entity, active);
    ++skies;
  }
  if (instances.empty()) {
    return;
  }
  const uint32_t count = uint32_t(instances.size());
  std::vector<PortRoomGeo::Script::Hidden>& hidden = scripts.script.hidden;
  std::sort(hidden.begin(), hidden.end());
  hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
  const std::vector<uint8_t> out = PortRoomGeo::Write(instances, &scripts.script);
  char file[32];
  std::snprintf(file, sizeof file, "%08X.roomgeo", mrea);
  if (!m_io.write || !m_io.write(file, out)) {
    Log("  " + r.name + ": could not write " + file);
    return;
  }
  Lightmap(r).lit = lookups;
  if (r.room.Lighting().HasLightmap()) {
    const LightmapData& lm = Lightmap(r);
    char text[160];
    if (lm.ok) {
      std::snprintf(text, sizeof text, "  %s: lightmap %s %ux%u x %zu layers, %zu of %u instances have a lookup",
                    r.name.c_str(), UuidText(r.room.Lighting().lightmap.data()).c_str(), lm.tex.width, lm.tex.height,
                    lm.tex.layers.size(), lookups, count);
    } else {
      std::snprintf(text, sizeof text, "  %s: no lightmap, %u instances", r.name.c_str(), count);
    }
    Log(text);
  }
  char line[512];
  std::snprintf(line, sizeof line,
                "  %s (%08X): %u instances (%zu from MCON, %zu actors, %zu skies, %zu inactive objects left out; %zu on a layer, "
                "%zu scripted, %zu in %zu groups of %zu script objects and %zu connections (%zu untraced), %zu on a "
                "platform, %zu with untraced links, %zu glowing, %zu animated, %zu left to retail actors, %zu left to "
                "cinematics; %zu of %zu entities matched), %zu dropped",
                r.name.c_str(), mrea, count, modcons, actors, skies, inactive, gated, linked, grouped, scripts.group.size(),
                scripts.script.nodes.size(), scripts.script.edges.size(), scripts.scriptUnresolved, riding, unresolved,
                glowing, animated, retailDrawn, posed, scripts.matched, scripts.entities, dropped);
  Log(line);
}

void Writer::WriteLiquids(const RoomData& r, uint32_t mrea) {
  // Liquids are room geometry: they come with it, so a models-only import keeps the
  // retail planes (and the small arena its mod starts with).
  if (!m_io.liquid || (m_io.wantsGeometry && !m_io.wantsGeometry(r.name))) {
    return;
  }
  static const int kAxis[3] = {0, 2, 1};
  static const double kSign[3] = {-1, 1, 1};
  // What each entity's retail water object is filled with, and every water object's camera
  // filter colour (CScriptWaterMP1+0x520, build/mpr/water/E-under.md Q1) where it stands.
  std::map<int, int> fluids;
  std::map<int, float> xrayOpacity;
  std::vector<uint8_t> filters;
  uint32_t filterCount = 0;
  for (const Component* c : r.room.Of(kWaterMP1)) {
    Span s;
    int type = RoomLiquid::kWater;
    if (r.room.Nested(*c, {kPropWaterFluid[0], kPropWaterFluid[1], kPropWaterFluid[2]}, s) && s.size >= 4) {
      const uint32_t fluid = ReadLE32(r.room.Bytes(s));
      type = fluid == 10 ? RoomLiquid::kPoison : fluid == 11 ? RoomLiquid::kLava : RoomLiquid::kWater;
    }
    fluids[c->entity] = type;
    if (r.room.Nested(*c, {kPropWaterXrayOpacity}, s) && s.size >= 4) {
      xrayOpacity[c->entity] = ReadLEFloat(r.room.Bytes(s));
    }
    Vec3 pos, rot, scale;
    if (r.room.Xform(*c, pos, rot, scale)) {
      for (int row = 0; row < 3; ++row) {
        AppendLEFloat(filters, kSign[row] * pos[kAxis[row]]);
      }
      for (const uint32_t channel : kPropColorRGBA) {
        // SLdrColor_MP1Typedef's default is (1, 1, 1, 1).
        float value = 1.f;
        if (r.room.Nested(*c, {kPropWaterFilterColor, channel}, s) && s.size >= 4) {
          value = ReadLEFloat(r.room.Bytes(s));
        }
        AppendLEFloat(filters, value);
      }
      ++filterCount;
    }
  }
  std::vector<uint8_t> body;
  uint32_t count = 0;
  size_t dropped = 0;
  for (const bool lava : {false, true}) {
    for (const Component* c : r.room.Of(lava ? kLavaRenderVolume : kWaterRenderVolume)) {
      const auto f = r.room.Flat(*c);
      const auto prop = f.find(lava ? kPropLavaModel : kPropWaterModel);
      Vec3 pos, rot, scale;
      if (prop == f.end() || prop->second.size != 16 || !r.room.Xform(*c, pos, rot, scale)) {
        continue;
      }
      RoomLiquid liquid;
      liquid.model = SwapUuid(r.room.Bytes(prop->second));
      if (liquid.model == Id16{}) {
        continue;
      }
      const auto fluid = fluids.find(c->entity);
      liquid.type = lava ? RoomLiquid::kLava : fluid != fluids.end() ? fluid->second : RoomLiquid::kWater;
      for (int i = 0; lava && i < 6; ++i) {
        if (const auto v = f.find(kPropLava[i]); v != f.end() && v->second.size == 4) {
          liquid.lava[i] = ReadLEFloat(r.room.Bytes(v->second));
        }
      }
      if (!lava) {
        Span s;
        // Each field is the room's, else the loader's default (RoomLiquid's).
        auto value = [&](std::initializer_list<uint32_t> path, float* out, int n) {
          if (r.room.Nested(*c, path, s) && s.size >= size_t(4 * n)) {
            for (int i = 0; i < n; ++i) {
              out[i] = ReadLEFloat(r.room.Bytes(s) + 4 * i);
            }
          }
        };
        auto texture = [&](std::initializer_list<uint32_t> path, Id16& out) {
          if (r.room.Nested(*c, path, s) && s.size == 16) {
            out = SwapUuid(r.room.Bytes(s));
          }
        };
        for (int i = 0; i < 11; ++i) {
          if (r.room.Nested(*c, {kPropWaterFeatures, kPropWaterFeature[i]}, s) && s.size >= 1) {
            liquid.features[i] = r.room.Bytes(s)[0] != 0 ? 1 : 0;
          }
        }
        for (int w = 0; w < 2; ++w) {
          for (int i = 0; i < 5; ++i) {
            value({kPropWaterWaves[w], kPropWave[i]}, &liquid.waves[w][i], 1);
          }
        }
        value({kPropWaterLook, kPropWaterTint}, liquid.tint, 4);
        texture({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalTexture}, liquid.normalMap);
        for (int i = 0; i < 2; ++i) {
          value({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalDir, kPropWaterNormalDirXY[i]},
                &liquid.normalDir[i], 1);
        }
        value({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalSpeed}, &liquid.normalSpeed, 1);
        value({kPropWaterLook, kPropWaterNormalMap, kPropWaterNormalScale}, &liquid.normalScale, 1);
        value({kPropWaterLook, kPropWaterFog, kPropWaterFogColor}, liquid.fogColor, 4);
        value({kPropWaterLook, kPropWaterFog, kPropWaterFogDistance}, &liquid.fogDistance, 1);
        texture({kPropWaterLook, kPropWaterRain, kPropWaterRainNoise}, liquid.rainNoise);
        for (int i = 0; i < 10; ++i) {
          value({kPropWaterLook, kPropWaterRain, kPropWaterRainValue[i]}, &liquid.rain[i], 1);
          value({kPropWaterLook, kPropWaterFlow, kPropWaterFlowValue[i]}, &liquid.flow[i], 1);
        }
        texture({kPropWaterLook, kPropWaterFlow, kPropWaterFlowMap}, liquid.flowMap);
        if (const auto x = xrayOpacity.find(c->entity); x != xrayOpacity.end()) {
          liquid.xrayOpacity = x->second;
        }
        for (int i = 0; i < 5; ++i) {
          value({kPropWaterLook, kPropWaterMaterial[i]}, &liquid.material[i], 1);
        }
      }
      if (m_io.cancelled && m_io.cancelled()) {
        return;
      }
      uint32_t id = 0;
      RoomWaterAssets assets;
      if (lava ? !m_io.liquid(liquid, id) : !m_io.water || !m_io.water(liquid, assets)) {
        ++dropped;
        continue;
      }
      double s[3], k[3];
      for (int i = 0; i < 3; ++i) {
        s[i] = std::sin(rot[i] * (3.14159265358979323846 / 180.0));
        k[i] = std::cos(rot[i] * (3.14159265358979323846 / 180.0));
      }
      const double m[3][3] = {
          {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
          {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
          {-s[1], k[1] * s[0], k[1] * k[0]},
      };
      AppendLE32(body, uint32_t(liquid.type));
      AppendLE32(body, id);
      for (int row = 0; row < 3; ++row) {
        // A lava pool is converted into the area's axes. A water mesh is not: it stays in
        // Remastered's model space and the transform takes it there, (x, y, z) -> (-x, z, y).
        double col[3];
        for (int c = 0; c < 3; ++c) {
          col[c] = kSign[row] * kSign[c] * m[kAxis[row]][kAxis[c]];
        }
        if (!lava) {
          const double old[3] = {col[0], col[1], col[2]};
          col[0] = -old[0];
          col[1] = old[2];
          col[2] = old[1];
        }
        for (int c = 0; c < 3; ++c) {
          // The entity's scale is the size of the volume; the model is in units already.
          AppendLEFloat(body, col[c]);
        }
        AppendLEFloat(body, kSign[row] * pos[kAxis[row]]);
      }
      if (!lava) {
        AppendLE32(body, uint32_t(assets.vertices.size()));
        AppendLE32(body, uint32_t(assets.indices.size()));
        for (const float v : assets.boundsMin) {
          AppendLEFloat(body, v);
        }
        for (const float v : assets.boundsMax) {
          AppendLEFloat(body, v);
        }
        for (const uint8_t v : liquid.features) {
          body.push_back(v);
        }
        body.push_back(0);
        auto floats = [&](const float* v, int n) {
          for (int i = 0; i < n; ++i) {
            AppendLEFloat(body, v[i]);
          }
        };
        floats(liquid.waves[0], 5);
        floats(liquid.waves[1], 5);
        floats(liquid.tint, 4);
        floats(liquid.normalDir, 2);
        floats(&liquid.normalSpeed, 1);
        floats(&liquid.normalScale, 1);
        floats(liquid.fogColor, 4);
        floats(&liquid.fogDistance, 1);
        floats(liquid.material, 5);
        floats(liquid.rain, 10);
        floats(liquid.flow, 10);
        floats(&liquid.xrayOpacity, 1);
        AppendLE32(body, assets.normalMap);
        AppendLE32(body, assets.flowMap);
        AppendLE32(body, assets.rainNoise);
        AppendLE32(body, assets.rainNoiseWidth);
        AppendLE32(body, assets.rainNoiseHeight);
        for (const RoomWaterAssets::Vertex& v : assets.vertices) {
          floats(v.pos, 3);
          floats(v.uv, 4);
          body.insert(body.end(), v.color, v.color + 4);
        }
        for (const uint32_t i : assets.indices) {
          AppendLE32(body, i);
        }
      }
      ++count;
    }
  }
  if (count == 0 && filterCount == 0) {
    return;
  }
  std::vector<uint8_t> out;
  AppendLE32(out, 0x4C52504D);  // 'MPRL'
  AppendLE32(out, 4);
  AppendLE32(out, count);
  out.insert(out.end(), body.begin(), body.end());
  AppendLE32(out, filterCount);
  out.insert(out.end(), filters.begin(), filters.end());
  char file[32];
  std::snprintf(file, sizeof file, "%08X.roomliquid", mrea);
  if (!m_io.write || !m_io.write(file, out)) {
    Log("  " + r.name + ": could not write " + file);
    return;
  }
  char line[160];
  std::snprintf(line, sizeof line, "  %s: %u liquid surfaces, %zu dropped, %u water filters", r.name.c_str(),
                count, dropped, filterCount);
  Log(line);
}

std::string Writer::WriteRoom(const RoomData& r, const std::map<std::string, Placement>& placed, const Vec3& shift,
                              const float tonemap[5], const BloomData& worldBloom,
                              const std::vector<GradeData>& worldGrades,
                              const std::vector<BacklightData>& worldBacklights, const std::vector<FogData>& worldFogs,
                              int& written, std::string& matched) {
  matched.clear();
  Match m;
  if (!MatchRoom(r, placed, m)) {
    return r.name + ": not placed by the world";
  }
  char head[96];
  if (m.err > 1.0) {
    std::snprintf(head, sizeof head, "%s: no area matches (best %08X, %.1f m)", r.name.c_str(), m.mrea, m.err);
    return head;
  }
  WriteGeometry(r, m.mrea, *m.area);
  WriteLiquids(r, m.mrea);
  const std::vector<Vec3>& gdoors = m.area->doors;
  double rot[3][3], trans[3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      rot[i][j] = m.a[size_t(i)][size_t(j)];
    }
    trans[i] = m.a[size_t(i)][3];
  }

  const SceneryScripts scripts = MatchScripts(r.room, *m.area);
  std::vector<uint8_t> probes;
  size_t probeCount = 0;
  std::vector<std::vector<uint8_t>> cubes;
  std::map<Id16, size_t> index;
  for (const Component* c : r.room.Of(kReflectionProbe)) {
    const auto f = r.room.Flat(*c);
    Vec3 pos, euler, scale;
    const auto reflProp = f.find(kPropProbeRefl);
    if (reflProp == f.end() || reflProp->second.size < 16 || !r.room.Xform(*c, pos, euler, scale)) {
      continue;
    }
    const RoomPak home{r.name, r.pak};
    std::vector<uint8_t> refl;
    if (!FindResource(r.room.Bytes(reflProp->second), Tag("REFL"), home, refl, nullptr, nullptr) ||
        refl.size() < 0x50) {
      continue;
    }
    const float probeScale = ReadLEFloat(&refl[0x4c]);
    const Pak* txtrPak = nullptr;
    Id16 txtrId;
    std::vector<uint8_t> txtr;
    if (!FindResource(&refl[0x3c], Tag("TXTR"), home, txtr, &txtrPak, &txtrId)) {
      continue;
    }
    if (index.find(txtrId) == index.end()) {
      TxtrCubeBc6h cube;
      std::string error;
      if (!ReadTxtrCubeBc6h(txtr.data(), txtr.size(), cube, error)) {
        continue;
      }
      std::vector<uint8_t> blob;
      AppendLE32(blob, cube.size);
      AppendLE32(blob, cube.mipCount);
      AppendLE32(blob, cube.isSigned ? 1 : 0);
      size_t bytes = 0;
      for (const auto& mip : cube.mips) {
        bytes += mip.size();
      }
      AppendLE32(blob, uint32_t(bytes));
      for (const auto& mip : cube.mips) {
        blob.insert(blob.end(), mip.begin(), mip.end());
      }
      index[txtrId] = cubes.size();
      cubes.push_back(std::move(blob));
    }
    // Remastered tests the world box around the rotated unit box (half = |R| |scale| / 2) and samples
    // the cube with the world direction, so a rotation only grows the box. Rz Ry Rx in degrees; the
    // order does not matter for the disc's probes (one axis, or under 1.2 degrees off one).
    double rr[3][3];
    {
      const double d = 3.14159265358979323846 / 180.0;
      const double cx = std::cos(euler[0] * d), sx = std::sin(euler[0] * d);
      const double cy = std::cos(euler[1] * d), sy = std::sin(euler[1] * d);
      const double cz = std::cos(euler[2] * d), sz = std::sin(euler[2] * d);
      const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
      const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
      const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
      double zy[3][3];
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          zy[i][j] = rz[i][0] * ry[0][j] + rz[i][1] * ry[1][j] + rz[i][2] * ry[2][j];
        }
      }
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          rr[i][j] = zy[i][0] * rx[0][j] + zy[i][1] * rx[1][j] + zy[i][2] * rx[2][j];
        }
      }
    }
    Vec3 remHalf;
    for (int i = 0; i < 3; ++i) {
      remHalf[size_t(i)] = 0;
      for (int j = 0; j < 3; ++j) {
        remHalf[size_t(i)] += std::fabs(rr[i][j]) * std::fabs(scale[size_t(j)]) / 2;
      }
    }
    const Vec3 centre = MulR2G(pos);
    Vec3 half = MulR2G(remHalf);
    for (double& v : half) {
      v = std::max(std::fabs(v), 1e-4);
    }
    // world -> area -> unit box; inv is the area's rotation transposed.
    double inv[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        inv[i][j] = rot[j][i];
      }
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        AppendLEFloat(probes, inv[i][j] / half[size_t(i)]);
      }
      const double it = (inv[i][0] * trans[0] + inv[i][1] * trans[1]) + inv[i][2] * trans[2];
      AppendLEFloat(probes, (-it - centre[size_t(i)]) / half[size_t(i)]);
    }
    // CUBE @ R2G.T @ inv, in two products with CUBE the identity, to keep numpy's signs of zero.
    constexpr double kCube[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double cr[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        cr[i][j] = (kCube[i][0] * kR2G[j][0] + kCube[i][1] * kR2G[j][1]) + kCube[i][2] * kR2G[j][2];
      }
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        AppendLEFloat(probes, (cr[i][0] * inv[0][j] + cr[i][1] * inv[1][j]) + cr[i][2] * inv[2][j]);
      }
    }
    const auto prop = [&](uint32_t id, float fallback) {
      const auto p = f.find(id);
      return p != f.end() && p->second.size >= 4 ? ReadLEFloat(r.room.Bytes(p->second)) : fallback;
    };
    const auto priority = f.find(kPropProbePriority);
    // The retail layer its own layer is drawn on (MatchScripts), as for colour grades.
    const auto layer = scripts.layer.find(c->entity);
    AppendLE32(probes, uint32_t(layer != scripts.layer.end() ? int32_t(layer->second) : -1));
    AppendLE32(probes, uint32_t(index[txtrId]));
    AppendLEFloat(probes, probeScale);
    AppendLEFloat(probes, prop(kPropProbePadding, 1.0f));
    AppendLE32(probes, priority != f.end() && priority->second.size >= 4 ? ReadLE32(r.room.Bytes(priority->second)) : 0u);
    AppendLEFloat(probes, prop(kPropProbeMin, 0.0f));
    AppendLEFloat(probes, prop(kPropProbeMax, 1.0f));
    ++probeCount;
  }

  // Only a room the world shift lands on its area has a grid in the right place.
  std::vector<uint8_t> grid;
  std::string gnote = "grid not placed";
  const auto it = placed.find(r.name);
  if (it != placed.end() && Distance(it->second.pos, {trans[0], trans[1], trans[2]}) < 0.5) {
    Grid(RoomPak{r.name, r.pak}, shift, gdoors, grid, gnote);
  }
  const LightmapData& lightmap = Lightmap(r);
  const bool withLightmap = lightmap.ok && lightmap.lit != 0;
  if (cubes.empty() && grid.empty() && !withLightmap) {
    return r.name + ": no probes, " + gnote;
  }

  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  AppendLE32(out, 19);
  float tone[5];
  std::copy(tonemap, tonemap + 5, tone);
  Tonemap(r, tone);
  for (int i = 0; i < 4; ++i) {
    AppendLEFloat(out, tone[i]);
  }
  AppendLE32(out, uint32_t(probeCount));
  AppendLE32(out, uint32_t(cubes.size()));
  out.insert(out.end(), probes.begin(), probes.end());
  for (const auto& blob : cubes) {
    out.insert(out.end(), blob.begin(), blob.end());
  }
  AppendLE32(out, grid.empty() ? 0 : 1);
  out.insert(out.end(), grid.begin(), grid.end());
  float exposure[5];
  Exposure(r, exposure);
  AppendLEFloat(out, exposure[0]);
  AppendLEFloat(out, exposure[1]);
  AppendLEFloat(out, exposure[2]);
  AppendLEFloat(out, tone[4]);
  BloomData bloom = worldBloom;
  ReadBloom(r, bloom);
  AppendLEFloat(out, bloom.threshold);
  AppendLE32(out, bloom.present ? uint32_t(bloom.tints.size() / 4) : 0);
  if (bloom.present) {
    for (const float v : bloom.tints) {
      AppendLEFloat(out, v);
    }
  }
  std::vector<GradeData> grades = worldGrades;
  ReadGrades(r, m.area, grades);
  AppendLE32(out, uint32_t(grades.size()));
  for (const GradeData& g : grades) {
    AppendLE32(out, uint32_t(g.layer));
    AppendLEFloat(out, g.fadeIn);
    AppendLEFloat(out, g.fadeOut);
    out.push_back(g.on ? 1 : 0);
    out.insert(out.end(), 3, 0);
    AppendLE32(out, uint32_t(g.priority));
    AppendLE32(out, uint32_t(g.links.size()));
    for (const PortRoomGeo::Link& link : g.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    out.insert(out.end(), g.lut.begin(), g.lut.end());
  }
  AppendLEFloat(out, exposure[3]);
  AppendLEFloat(out, exposure[4]);
  std::vector<BacklightData> backlights = worldBacklights;
  ReadBacklights(r, m.area, backlights);
  AppendLE32(out, uint32_t(backlights.size()));
  for (const BacklightData& b : backlights) {
    AppendLE32(out, uint32_t(b.layer));
    AppendLEFloat(out, b.fadeIn);
    AppendLEFloat(out, b.fadeOut);
    out.push_back(b.on ? 1 : 0);
    out.insert(out.end(), 3, 0);
    AppendLE32(out, uint32_t(b.priority));
    AppendLEFloat(out, b.top);
    AppendLEFloat(out, b.back);
    AppendLE32(out, uint32_t(b.links.size()));
    for (const PortRoomGeo::Link& link : b.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
  }
  std::vector<FogData> fogs = worldFogs;
  ReadFogs(r, m.area, fogs);
  AppendLE32(out, uint32_t(fogs.size()));
  for (const FogData& g : fogs) {
    AppendLE32(out, uint32_t(g.layer));
    for (const std::vector<uint8_t>* fade : {&g.fadeIn, &g.fadeOut}) {
      PortMayaSpline sline;
      AppendLEFloat(out, sline.Load(fade->data(), fade->size()) ? std::max(0.f, sline.LastTime()) : 0.f);
    }
    out.push_back(g.on ? 1 : 0);
    out.insert(out.end(), 3, 0);
    AppendLE32(out, uint32_t(g.priority));
    for (size_t i = 0; i < 10; ++i) {
      // The height term over retail z: the room's shift moves the heights.
      AppendLEFloat(out, i == 6 ? g.s[6] - g.s[5] * float(shift[2]) : g.s[i]);
    }
    for (float v : g.wind) {
      AppendLEFloat(out, v);
    }
    out.push_back(g.useScriptWind ? 1 : 0);
    out.push_back(g.noProbe ? 1 : 0);
    out.insert(out.end(), 2, 0);
    for (float v : g.colorB) {
      AppendLEFloat(out, v);
    }
    for (float v : g.colorA) {
      AppendLEFloat(out, v);
    }
    for (float v : g.lut) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, uint32_t(g.links.size()));
    for (const PortRoomGeo::Link& link : g.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    for (const std::vector<uint8_t>* fade : {&g.fadeIn, &g.fadeOut}) {
      AppendLE32(out, uint32_t(fade->size()));
      out.insert(out.end(), fade->begin(), fade->end());
      out.insert(out.end(), (4 - fade->size() % 4) % 4, 0);
    }
  }
  std::vector<FogRegionData> regions;
  std::vector<FogTransitionData> transitions;
  ReadFogRegions(r, scripts, m.a, regions, transitions);
  AppendLE32(out, uint32_t(regions.size()));
  for (const FogRegionData& g : regions) {
    AppendLE32(out, uint32_t(g.layer));
    out.push_back(g.on ? 1 : 0);
    out.push_back(g.fluid);
    out.push_back(g.hasColor ? 1 : 0);
    out.push_back(g.hasCap ? 1 : 0);
    for (const auto& row : g.m) {
      for (float v : row) {
        AppendLEFloat(out, v);
      }
    }
    for (float v : g.edgeScale) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, g.mult);
    for (float v : g.edgeBias) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, g.cap);
    for (float v : g.color) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, g.density);
    for (float v : g.box) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, uint32_t(g.links.size()));
    for (const PortRoomGeo::Link& link : g.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    AppendLEFloat(out, g.distance);
    AppendLEFloat(out, g.transmittance);
    out.push_back(g.subtract ? 1 : 0);
    out.insert(out.end(), 3, 0);
  }
  AppendLE32(out, uint32_t(transitions.size()));
  for (const FogTransitionData& t : transitions) {
    AppendLE32(out, t.region);
    AppendLE32(out, uint32_t(t.layer));
    out.push_back(t.on ? 1 : 0);
    out.push_back(t.autoStart ? 1 : 0);
    out.push_back(t.loop ? 1 : 0);
    out.push_back(t.select);
    AppendLEFloat(out, t.distance);
    AppendLEFloat(out, t.transmittance);
    for (float v : t.color) {
      AppendLEFloat(out, v);
    }
    AppendLEFloat(out, t.cap);
    AppendLE32(out, uint32_t(t.phase.size()));
    out.insert(out.end(), t.phase.begin(), t.phase.end());
    out.insert(out.end(), (4 - t.phase.size() % 4) % 4, 0);
    AppendLE32(out, uint32_t(t.links.size()));
    for (const PortRoomGeo::Link& link : t.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
  }
  std::vector<SunData> suns;
  ReadSuns(r, scripts, m.a, suns);
  AppendLE32(out, uint32_t(suns.size()));
  for (const SunData& s : suns) {
    AppendLE32(out, uint32_t(s.layer));
    out.push_back(s.on ? 1 : 0);
    out.insert(out.end(), 3, 0);
    for (float v : s.toSun) {
      AppendLEFloat(out, v);
    }
    for (float v : s.color) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, s.group);
  }
  // The lightmap the room geometry's lookups index, or a width of 0.
  if (withLightmap) {
    const PortRemastered::TxtrLayersBc6h& tex = lightmap.tex;
    AppendLE32(out, tex.width);
    AppendLE32(out, tex.height);
    AppendLE32(out, uint32_t(tex.layers.size()));
    AppendLE32(out, tex.isSigned ? 1 : 0);
    size_t bytes = 0;
    for (const auto& layer : tex.layers) {
      bytes += layer.size();
    }
    AppendLE32(out, uint32_t(bytes));
    for (const auto& layer : tex.layers) {
      out.insert(out.end(), layer.begin(), layer.end());
    }
  } else {
    AppendLE32(out, 0);
  }
  std::vector<PointLightData> lights;
  ReadPointLights(r, scripts, m.a, lights);
  AppendLE32(out, uint32_t(lights.size()));
  for (const PointLightData& l : lights) {
    AppendLE32(out, uint32_t(l.layer));
    out.push_back(l.on ? 1 : 0);
    out.push_back(l.spot ? 1 : 0);
    out.push_back(l.falloff);
    out.push_back(0);
    for (float v : l.pos) {
      AppendLEFloat(out, v);
    }
    for (float v : l.toLight) {
      AppendLEFloat(out, v);
    }
    for (float v : l.color) {
      AppendLEFloat(out, v);
    }
    for (float v : l.nearFar) {
      AppendLEFloat(out, v);
    }
    for (float v : l.cone) {
      AppendLEFloat(out, v);
    }
    AppendLE32(out, l.group);
    AppendLE32(out, uint32_t(l.links.size()));
    for (const PortRoomGeo::Link& link : l.links) {
      AppendLE32(out, link.sender);
      out.push_back(link.state);
      out.push_back(link.action);
      out.insert(out.end(), 2, 0);
    }
    out.push_back(l.animated ? 1 : 0);
    out.push_back(l.playing ? 1 : 0);
    out.push_back(l.loop ? 1 : 0);
    out.push_back(l.offAtStart ? 1 : 0);
    out.push_back(l.animFalloff);
    out.insert(out.end(), 3, 0);
    AppendLEFloat(out, l.length);
    AppendLEFloat(out, l.scale[0]);
    AppendLEFloat(out, l.scale[1]);
    AppendLEFloat(out, l.intensity);
    for (const float c : l.rgb) {
      AppendLEFloat(out, c);
    }
    for (const std::vector<uint8_t>& spline : l.splines) {
      AppendLE32(out, uint32_t(spline.size()));
      out.insert(out.end(), spline.begin(), spline.end());
      out.insert(out.end(), (4 - spline.size() % 4) % 4, 0);
    }
  }
  if (!fogs.empty() || !regions.empty()) {
    Log("  " + r.name + ": " + std::to_string(fogs.size()) + " fog hint(s), " + std::to_string(regions.size()) +
        " fog region(s), " + std::to_string(transitions.size()) + " transition(s)");
  }
  char file[32];
  std::snprintf(file, sizeof file, "%08X.roomenv", m.mrea);
  if (!m_io.write || !m_io.write(file, out)) {
    return r.name + ": could not write " + file;
  }
  ++written;
  std::snprintf(head, sizeof head, "%08X", m.mrea);
  matched = head;
  char tail[160];
  std::snprintf(tail, sizeof tail, ": %08X doors %.2f m, %zu probes, %zu cubes, ", m.mrea, m.err, probeCount,
                cubes.size());
  return r.name + tail + gnote + ", " + std::to_string(out.size() / 1024) + " KB";
}

bool Writer::Run(uint32_t mlvl, int& written, std::string& error) {
  written = 0;
  if (m_master.pak == nullptr) {
    error = "no master pak";
    return false;
  }
  if (!LoadAreas(mlvl, error)) {
    return false;
  }
  RoomData master;
  if (!ReadRoomFile(m_master, master, error)) {
    error = "master: " + error;
    return false;
  }

  // Where the world puts each room, by ROOM asset id, in GameCube world coordinates.
  std::map<Id16, Vec3> byId;
  for (const Component* c : master.room.Of(kRoomController)) {
    const auto f = master.room.Flat(*c);
    Vec3 pos, rot, scale;
    const auto id = f.find(kPropRoomId);
    if (id != f.end() && id->second.size == 16 && master.room.Xform(*c, pos, rot, scale)) {
      Id16 key;
      std::memcpy(key.data(), master.room.Bytes(id->second), 16);
      byId[key] = MulR2G(pos);
    }
  }

  std::vector<std::unique_ptr<RoomData>> rooms;
  for (const RoomPak& rp : m_rooms) {
    if (rp.name == m_master.name || (rp.name.size() >= 5 && rp.name.compare(rp.name.size() - 5, 5, "_Copy") == 0)) {
      continue;
    }
    auto r = std::make_unique<RoomData>();
    std::string roomError;
    if (rp.pak == nullptr || FirstOfType(*rp.pak, Tag("ROOM")) == nullptr) {
      Log(rp.name + ": no ROOM");
      continue;
    }
    if (!ReadRoomFile(rp, *r, roomError)) {
      Log(rp.name + ": " + roomError);
      continue;
    }
    rooms.push_back(std::move(r));
  }

  std::map<std::string, Vec3> spots;
  std::vector<std::string> spotOrder;
  for (const auto& r : rooms) {
    const auto it = byId.find(r->id);
    if (it != byId.end()) {
      if (!spots.count(r->name)) {
        spotOrder.push_back(r->name);
      }
      spots[r->name] = it->second;
    }
  }
  if (spots.empty()) {
    error = "the master places no room";
    return false;
  }
  // Remastered moved each world's origin; the shift is the one that lands the most rooms on areas.
  auto hits = [&](const Vec3& shift) {
    int n = 0;
    for (const auto& [name, p] : spots) {
      double best = 1e300;
      for (const Area& a : m_areas) {
        best = std::min(best, Distance({a.xf[0][3], a.xf[1][3], a.xf[2][3]},
                                       {p[0] + shift[0], p[1] + shift[1], p[2] + shift[2]}));
      }
      n += best < 0.5;
    }
    return n;
  };
  Vec3 shift{};
  int bestHits = -1;
  for (const Area& a : m_areas) {
    for (size_t i = 0; i < std::min<size_t>(4, spotOrder.size()); ++i) {
      const Vec3& p = spots[spotOrder[i]];
      const Vec3 candidate{a.xf[0][3] - p[0], a.xf[1][3] - p[1], a.xf[2][3] - p[2]};
      const int n = hits(candidate);
      if (n > bestHits) {
        bestHits = n;
        shift = candidate;
      }
    }
  }
  {
    char line[160];
    std::snprintf(line, sizeof line, "world shift (%.2f, %.2f, %.2f), %d of %zu rooms land on an area", shift[0],
                  shift[1], shift[2], bestHits, spots.size());
    Log(line);
  }
  std::map<std::string, Placement> placed;
  for (const auto& [name, p] : spots) {
    placed[name].pos = {p[0] + shift[0], p[1] + shift[1], p[2] + shift[2]};
  }

  // A room without a Tonemap or BloomEffect of its own takes the world's.
  float tonemap[5] = {4.0f, 0.18f, 0.6f, 0.15f, 0.f};
  Tonemap(master, tonemap);
  BloomData bloom;
  ReadBloom(master, bloom);
  // The world's global grade sits under the room's own (the last active one is drawn).
  std::vector<GradeData> grades;
  ReadGrades(master, nullptr, grades);
  std::vector<BacklightData> backlights;
  ReadBacklights(master, nullptr, backlights);
  std::vector<FogData> fogs;
  ReadFogs(master, nullptr, fogs);
  std::map<std::string, std::string> seen;
  for (const auto& r : rooms) {
    if (m_io.cancelled && m_io.cancelled()) {
      error = "cancelled";
      return false;
    }
    std::string matched;
    const std::string line = WriteRoom(*r, placed, shift, tonemap, bloom, grades, backlights, fogs, written, matched);
    Log(line);
    if (!matched.empty()) {
      const auto s = seen.find(matched);
      if (s != seen.end()) {
        Log("  CLASH: " + r->name + " and " + s->second + " both match " + matched);
      }
      seen[matched] = r->name;
    }
  }
  Log(std::to_string(seen.size()) + " of " + std::to_string(m_areas.size()) + " areas");
  return true;
}

}  // namespace

const std::vector<RoomWorld>& RoomWorlds() {
  static const std::vector<RoomWorld> worlds = {
      {"Intro_Master", 0x158EFE17}, {"RuinsWorld", 0x83F6FF6F},   {"IceWorld", 0xA8BE6291},
      {"Over_Master", 0x39F2DE28},  {"Mines_Master", 0xB1AC4D65}, {"Lava_Master", 0x3EF8237C},
      {"Crater_Master", 0xC13B09D1}};
  return worlds;
}

bool WriteWorldRoomEnvs(uint32_t mlvl, const RoomPak& master, const std::vector<RoomPak>& rooms,
                        const std::vector<RoomPak>& others, const RoomIO& io, int& written, std::string& error) {
  Writer writer(master, rooms, others, io);
  return writer.Run(mlvl, written, error);
}

}  // namespace PortRemastered
