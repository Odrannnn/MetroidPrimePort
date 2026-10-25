// Internal to the Remastered room environment writer (port_remastered_room.h). What its
// parts share: the ids and helpers below, and one part's types and entry points for the
// next, in the order of the Python sources they port:
//
//   port_remastered_room_file.cpp     ROOM files (roomlib.py)
//   port_remastered_room_retail.cpp   the retail world (gcres.py)
//   port_remastered_room_scripts.cpp  script links of scenery actors (match.py, trace.py)
//   port_remastered_room.cpp          the writer (envwrite.py)
//
// Nothing outside those files should include this.

#pragma once

#include "port_env.h"
#include "port_remastered_room.h"
#include "port_bytes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>

#include "port_maya_spline.h"
#include "port_remastered_anim.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_txtr.h"
#include "port_room_env.h"
#include "port_room_geo.h"

namespace PortRemastered {
namespace RoomDetail {

using Vec3 = std::array<double, 3>;
using Mat34 = std::array<std::array<double, 4>, 3>;
using Id16 = std::array<uint8_t, 16>;

using port::FourCC;

// Component type ids (retrotool's root.json).
constexpr uint32_t kEntity = 0x749749f1;
constexpr uint32_t kRoomController = 0x83cc17aa;
constexpr uint32_t kTonemap = 0xddea916d;
constexpr uint32_t kReflectionProbe = 0x27807e39;
constexpr uint32_t kAutoExposureHint = 0x98694074;
constexpr uint32_t kAutoExposureHintMP1 = 0x451c85b6;  // CAutoExposureHintMP1GOC: swaps two hints on the visor
constexpr uint32_t kLinkExposureHintA = 0x9ea1739d;     // its link to the hint for the normal view
constexpr uint32_t kLinkExposureHintB = 0x4ff221ac;     // its link to the thermal one
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
// SLdrLightFlags (35b27754): byte 0, a103d675, is the cast bit (kb func/light-dynamic.md).
constexpr uint32_t kPropLightFlags[2] = {0x35b27754, 0xa103d675};
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



// float32 -> float16, round to nearest even (numpy's astype(float16)).
inline uint16_t FloatToHalf(float f) {
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
inline Id16 SwapUuid(const uint8_t* b) {
  Id16 id;
  static const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    id[size_t(i)] = b[kOrder[i]];
  }
  return id;
}

// A property-order uuid as it prints (8-4-4-4-12).
inline std::string UuidText(const uint8_t* propertyId) {
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
inline Vec3 MulR2G(const Vec3& v) {
  Vec3 out{};
  for (int i = 0; i < 3; ++i) {
    out[size_t(i)] = kR2G[i][0] * v[0] + kR2G[i][1] * v[1] + kR2G[i][2] * v[2];
  }
  return out;
}

inline double Distance(const Vec3& a, const Vec3& b) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

// `out` = the inverse of `m`; false when it is singular.
inline bool Invert3(const double m[3][3], double out[3][3]) {
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

inline Vec3 Apply(const Mat34& a, const Vec3& d) {
  Vec3 out{};
  for (int i = 0; i < 3; ++i) {
    out[size_t(i)] = (d[0] * a[size_t(i)][0] + d[1] * a[size_t(i)][1] + d[2] * a[size_t(i)][2]) + a[size_t(i)][3];
  }
  return out;
}

// ---------------------------------------------------------------------------
// ROOM files (roomlib.py): port_remastered_room_file.cpp
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

bool PropList(const uint8_t* b, size_t len, std::vector<Prop>* out);

// ---------------------------------------------------------------------------
// Retail world (gcres.py): port_remastered_room_retail.cpp
// ---------------------------------------------------------------------------

// A retail script object (SCLY), as far as matching Remastered's entities to it needs.
struct ScriptObject {
  // One connection: the state it is sent in and the message, as the enums in
  // MetroidPrime/CEntityInfo.hpp number them, and the editor id it reaches.
  struct Link {
    uint32_t state = 0;
    uint32_t message = 0;
    uint32_t target = 0;
  };
  uint32_t id = 0;  // editor id, layer bits included
  uint8_t type = 0;
  int layer = 0;    // the SCLY layer it is in
  uint32_t propCount = 0;
  std::vector<uint32_t> targets;  // of its connections
  std::vector<Link> links;        // the same connections, with their state and message
  bool hasPos = false;            // the three floats after its name, for any type
  Vec3 pos{};
  // The six floats after those: an editor transform's rotation (degrees) and scale for the
  // types that start with one, anything for the others.
  bool hasXf = false;
  Vec3 rot{}, scale{};
  std::vector<uint8_t> props;     // from the end of its name on: what names its model
};

struct Area {
  uint32_t mrea = 0;
  Mat34 xf{};
  std::vector<Vec3> doors;
  std::vector<ScriptObject> objects;
};

bool ReadMlvl(const std::vector<uint8_t>& d, std::vector<std::pair<uint32_t, Mat34>>& out);
bool ReadScly(const std::vector<uint8_t>& m, std::vector<ScriptObject>& objects);

// ---------------------------------------------------------------------------
// Script links of scenery actors (match.py): port_remastered_room_scripts.cpp
// ---------------------------------------------------------------------------

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
constexpr uint32_t kActionAction = 0xd5be6778;  // Action (19): every Timer/Trigger -> SpecialFunction link
// What an Entity is told to do with itself and every component it holds.
constexpr uint32_t kActionEntityActivate = 0x41435456;    // 'ACTV'
constexpr uint32_t kActionEntityDeactivate = 0x49435456;  // 'ICTV'
// Remastered's own script objects, which show and hide room geometry and which retail has
// no object for (PortRoomGeo::Script).
constexpr uint32_t kTriggerMP1 = 0xf526fc2a;
constexpr uint32_t kCounter = 0xa7db53c1;
constexpr uint32_t kRelay = 0x8fe0bfc9;
// A float driver for a material variable: RealNumberDriver (a spline over time) -> typed link ->
// RealValueRelay -> typed link -> MaterialVariableDriverMP1 -> typed link -> the actors.
constexpr uint32_t kRealNumberDriver = 0xd81ef16f;
constexpr uint32_t kRealValueRelay = 0x63dd6841;
constexpr uint32_t kMaterialVariableDriver = 0x8a12a2f9;
constexpr uint32_t kActionDriverStart = 0xd1cd9262;  // ResetAndStart
constexpr uint32_t kPropDriverSpline = 0x4675e9d0;
constexpr uint32_t kPropMaterialSet[2] = {0x0238c7cb, 0x7501b39e};  // names in STRP
constexpr uint32_t kPropMaterialVariable[2] = {0x0238c7cb, 0x4c107e13};
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
// The EnvironmentFX colour intensity: CEnvFxDensityControllerMP1GOC applies its float property
// kPropEnvFxIntensity (default 1) to CEnvFxManagerMP1 on the script action kActionEnvFxApply;
// a RelayAutoFire fires its links when its layer loads (kb topic/rain-snow-splash-render.md (3)).
constexpr uint32_t kEnvFxDensityController = 0x3c832ec7;
constexpr uint32_t kRelayAutoFire = 0x7f6d1cba;
constexpr uint32_t kPropEnvFxIntensity = 0x19da7ec9;
constexpr uint32_t kActionEnvFxApply = 0x582c90b4;
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
// What a VolumetricFogHint takes instead (CVolumetricFogHintGOC::AcceptScriptMsg 0x6b8c04):
// AddHintRequest, RemoveHintRequest, and ReallyRemoveHint (as DLEX); also QueryHintState
// (0x20c4c0f9) and (de)activation.
constexpr uint32_t kActionFogHintOn = 0xb1766945;
constexpr uint32_t kActionFogHintOff = 0xd8d934c6;
constexpr uint32_t kActionFogHintRemove = 0x1c67758f;
// What a VolumetricFogRegionTransition takes (its AcceptScriptMsg): Start, ResetAndStart,
// Stop, DLEX and (de)activation, the entity's and the component's own.
constexpr uint32_t kActionTransitionStart = 0x6c4d551c;
constexpr uint32_t kActionTransitionRestart = 0xfb05eadb;
constexpr uint32_t kActionTransitionStop = 0x4208824e;
constexpr uint32_t kActionComponentActivate = 0x41434d50;    // 'ACMP'
constexpr uint32_t kActionComponentDeactivate = 0x49434d50;  // 'ICMP'
constexpr uint32_t kProxyPlayer = 0x5797d3c7;
constexpr uint32_t kEventPlayerFluidIn = 0xcc17e9b1;
constexpr uint32_t kEventPlayerFluidOut = 0x42604bc6;
constexpr uint32_t kCameraWaterProxy = 0x6a7a53b0;
constexpr uint32_t kEventCameraWaterIn = 0x99655851;
constexpr uint32_t kEventCameraWaterOut = 0x5b16cc17;
constexpr uint32_t kEventCounterUp = 0x40e54906;
constexpr uint32_t kEventCounterDown = 0xc163beb5;
// SpecialFunctionMP1: the function (5, InventoryActivator) and its item (an EItemType); on
// Action it sends Zero if the player has the item, MaxReached if not.
constexpr uint32_t kSpecialFunction = 0x71337f4f;
constexpr uint32_t kPropSpecialFunctionType = 0x0d86d1bf;
constexpr uint32_t kPropSpecialFunctionItem = 0x04fc7524;
constexpr uint32_t kSpecialFunctionInventoryActivator = 5;
constexpr uint32_t kEventSpecialFunctionZero = 0x58951d90;
constexpr uint32_t kEventSpecialFunctionMaxReached = 0x52583665;
constexpr double kMatchTolerance = 0.02;
constexpr double kMatchNear = 2.0;
// A model carried over from retail, in a room's byte order (EffectRetailId); the last
// four bytes are the retail id, big-endian as retail's own properties hold it.
constexpr uint8_t kRetailIdPrefix[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
constexpr uint8_t kRetailActor = 0x00;
// How far Remastered moved a scenery actor off the retail one it stands for (0.38 at most).
constexpr double kRetailActorNear = 0.5;
// How far outside a Remastered stand-in model's world box a dropped retail Actor's origin
// may lie and still be hidden with it (WriteGeometry): a floor's box is a few cm thick.
constexpr double kStandinMargin = 0.25;
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

int RetailType(uint32_t type);
uint8_t LinkAct(uint32_t action);

struct Connection {
  size_t sender;  // component index
  uint32_t event;
  uint32_t action;
  Id16 target;  // a component's guid
};

std::vector<Connection> ReadConnections(const Room& room);
std::vector<int> ReadEntityLinks(const Room& room, size_t i);
// The same links with their link ids: (u32 id, linked component or -1).
std::vector<std::pair<uint32_t, int>> ReadEntityLinkIds(const Room& room, size_t i);
// The EnvironmentFX colour intensity the room sets when it loads: the density controller (kEnvFxDensityController)
// that a RelayAutoFire reaches, through Relays, with kActionEnvFxApply, else 1. A controller
// nothing reaches at load (fired by a trigger, or by nothing) is left out, and noted in `notes`.
float EnvFxIntensity(const Room& room, std::vector<std::string>& notes);
double MaxAbs(const Vec3& a, const Vec3& b);

// What drives the scenery actors of a room.
struct SceneryScripts {
  std::map<int, uint8_t> layer;  // by entity: the retail layer it is drawn on
  std::map<int, std::vector<PortRoomGeo::Link>> links;  // by entity
  std::map<int, size_t> unresolved;                      // by entity: connections not traced
  std::map<int, std::string> unresolvedHow;              // a grade hint's: sender type/event/action
  std::map<std::string, std::pair<size_t, std::set<int>>> missStats;  // MP_REMASTERED_MISS: class -> connections, entities
  std::map<int, const ScriptObject*> platform;           // by entity: the platform carrying it
  std::map<int, size_t> retail;                           // by entity: the retail object it matched
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
  // A float driver of a material variable (see kRealNumberDriver): by the entity of each actor it
  // feeds. The importer checks that the actor's model binds the variable to a parameter it can
  // drive; the start links are in `links` under `driverStart`.
  struct Driver {
    int start = -1;  // the RealNumberDriver's entity: its incoming links are the start triggers
    std::string set, variable;
    std::vector<uint8_t> spline;
  };
  std::map<int, std::vector<Driver>> drivers;
};

// Remastered's own particle effect actors (port_remastered_room_effects.cpp).
constexpr uint32_t kEffectMP1 = 0xb421cdcb;
// The room's EffectMP1 actors the disc has no object for, as "<MREA id>.roomfx"
// (port_room_effects.h); a line for the log, empty when there is nothing to say.
std::string WriteRoomEffects(const std::string& name, const Room& room, uint32_t mrea, const Area& area,
                             const SceneryScripts& scripts, const RoomIO& io);
// The GENPs (in a pak's byte order) those actors are for: the room's unconnected ones, and the
// barrier's ten when its graph validates.
std::vector<std::array<uint8_t, 16>> RoomEffectGenpsOf(const Room& room);
// The entities (their Entity components) that some connection in the room reaches.
std::set<int> ConnectedEntities(const Room& room);

// The Parasite Queen barrier's Remastered graph, which the import validates as a whole before
// it treats the ten effects as anything but connected actors (port_queen_barrier_assets.h has
// the ids). By ring, in the table's order: the PlatformMP1 whose component guid the table names,
// the two EffectMP1s it has Active links to (the two-variable refraction family and the
// one-variable glow one), and the WaypointMP1 its Play link names.
struct BarrierRingRefs {
  size_t platform = 0;
  size_t refraction = 0;
  size_t glow = 0;
  size_t waypoint = 0;
};
struct BarrierGraph {
  size_t controller = 0;  // the SpecialFunctionMP1 whose function is the ring controller
  float duration = 0;     // seconds, its fourth float
  std::array<BarrierRingRefs, 5> rings{};
};
// Whether the room ships that graph whole and unambiguous: one main controller, exactly its
// fifteen links (five platforms, ten effects), each platform's two effects and one waypoint,
// and one type 19 follower per platform-effect pair (unlinked ones are allowed). False for a
// partial, mutated or ambiguous graph, and then nothing is admitted.
bool BarrierGraphOf(const Room& room, BarrierGraph& out, std::string* why = nullptr);
// The GENP (in a pak's byte order) of an EffectMP1 component, or an all-zero id when it has none.
Id16 EffectGenpOf(const Room& room, size_t component);

bool SingleDriverStart(const std::vector<PortRoomGeo::Link>& starts, PortRoomGeo::Link& first);
int DriverSlot(const std::vector<PortRemastered::ModelMaterial>& materials, const SceneryScripts::Driver& d);
std::map<int, std::vector<SceneryScripts::Driver>> FindMaterialDrivers(const Room& room, std::set<int>& driverStarts);
SceneryScripts MatchScripts(const Room& room, const Area& area);
uint32_t FluidSender(const Room& room, const std::map<int, std::vector<const Connection*>>& incoming,
                     const Connection& c, int& state);
void CameraBox(const Vec3& pos, const Vec3& rot, const Vec3& scale, float centre[3], float half[3], float axes[9]);
SceneryScripts MasterScripts(const Room& room, const Vec3& shift, std::vector<PortRoomEnv::WorldVolume>& volumes);

}  // namespace RoomDetail
}  // namespace PortRemastered
