// Real reader and evaluator/DrawDesc builder, without a renderer or asset pool.
#include "Kyoto/Particles/CPortVfxMat.hpp"
#include "Kyoto/Particles/CColorElement.hpp"
#include "port_vfx_particles.h"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

// The fixtures have no textures. Fail hard if parsing/building unexpectedly reaches asset loading.
CTexture* CreateTexture(int) { std::abort(); }
CTexture::~CTexture() { std::abort(); }
CToken CSimplePool::GetObj(const SObjectTag&) { std::abort(); }
CToken::CToken(IObj*) { std::abort(); }
CToken::CToken(const CToken&) { std::abort(); }
CToken::~CToken() { std::abort(); }
void CToken::Unlock() { std::abort(); }
void CTexture::PortLoad(GXTexMapID, EClampMode, EClampMode) const { std::abort(); }
void* IElement::operator new(size_t n, const char*, const char*) { return ::operator new(n); }
void IElement::operator delete(void* p, size_t) { ::operator delete(p); }
const CPortVarMemory* CPortVarGlobals::sCurrent = nullptr;

// Distinct exposures verify which frame the production renderer selects.
namespace PortRoomEnv {
float ParticleInverseExposure(bool blended) { return blended ? 0.25f : 0.5f; }
}

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
void Word(std::vector<u8>& b, u32 v) {
  for (int shift = 24; shift >= 0; shift -= 8) { b.push_back(u8(v >> shift)); }
}
void Float(std::vector<u8>& b, float v) { Word(b, std::bit_cast<u32>(v)); }
std::vector<u8> Blob(u32 version) {
  std::vector<u8> b;
  Word(b, version); Word(b, 0x838); Word(b, 2); Word(b, 0);
  for (int i = 0; i < 10; ++i) { Word(b, u32(-1)); } // slots, ramp rows, add row
  for (int i = 0; i < 11; ++i) { Word(b, u32(-1)); Word(b, 0); Float(b, float(i)); }
  Float(b, 1.f); Float(b, 0.f); Word(b, 4);
  if (version == 3) { Float(b, 2.f); Float(b, 0.2f); }
  return b;
}
void Parse(CPortVfxData& vfx, std::vector<u8> blob) {
  const auto size = blob.size();
  Word(blob, 0x12345678); // next property must remain unread, also after fallback
  CMemoryInStream in(blob.data(), blob.size());
  Check(PortReadVfxMat(vfx, in, u32(size), nullptr), "reader can resynchronise");
  Check(in.ReadLong() == 0x12345678, "reader consumes exactly the blob");
}
void TestParsing() {
  CPortVfxData vfx;
  Parse(vfx, Blob(3));
  Check(vfx.mat.version == 3 && vfx.mat.opacityGain == 2.f && vfx.mat.opacityBias == 0.2f,
        "v3 reads appended BE gain/bias");
  Parse(vfx, Blob(2));
  Check(vfx.mat.version == 2 && vfx.mat.opacityGain == 1.f && vfx.mat.opacityBias == 0.f,
        "v2 resets previous gain/bias to neutral");
  Check(vfx.mat.src[10].value == 10.f && vfx.mat.spriteCenter == 4, "original sources and tail unchanged");
  for (u32 version : {0u, 1u, 4u, 0xffffffffu}) {
    Parse(vfx, Blob(version));
    Check(vfx.mat.version == 0, "unknown version leaves retail fallback");
  }
  for (u32 version : {2u, 3u}) {
    for (int delta : {-8, -4, 4, 8}) {
      auto b = Blob(version);
      b.resize(b.size() + delta);
      Parse(vfx, b);
      Check(vfx.mat.version == 0, "known versions reject short/long tails");
    }
    for (int field : {11, 15}) { // invalid blend or texture count, before loading any texture
      auto b = Blob(version); b[field] = 8;
      Parse(vfx, b);
      Check(vfx.mat.version == 0, "bad headers leave fallback");
    }
  }
  Parse(vfx, std::vector<u8>(12));
  Check(vfx.mat.version == 0, "short header leaves fallback");
  for (u32 version : {2u, 3u, 0x12345678u}) {
    for (u32 length : {1u, 2u, 3u, 13u, 201u, 209u}) {
      auto b = Blob(version);
      b.resize(length);
      Parse(vfx, b);
      Check(vfx.mat.version == 0, "nonaligned malformed/unknown blob leaves fallback and consumes residual bytes");
    }
  }
}
void TestPhysicalTruncation() {
  for (u32 version : {2u, 3u, 0x12345678u}) {
    for (u32 length : {1u, 2u, 3u, 13u, 200u, 201u, 208u, 209u, 0xffffffffu}) {
      auto b = Blob(version);
      // Include valid-layout truncation, malformed residual-byte truncation and a
      // maximum-u32 declaration without allocating a huge blob or overflowing.
      b.resize(length == 0xffffffffu ? 16 : length - 1);
      CMemoryInStream in(b.data(), b.size());
      CPortVfxData vfx;
      bool threw = false;
      try {
        PortReadVfxMat(vfx, in, length, nullptr);
      } catch (const std::runtime_error&) {
        threw = true;
      }
      Check(threw, "physical truncation still raises the stream exception");
    }
  }
}
class FrameReal : public CRealElement {
  float scale;
public:
  explicit FrameReal(float s) : scale(s) {}
  bool GetValue(int frame, float& value) const override { value = scale * frame; return false; }
};
class FrameColor : public CColorElement {
public:
  mutable int sdrCalls = 0, hdrCalls = 0;
  bool GetValue(int frame, CColor& value) const override {
    ++sdrCalls;
    value = CColor(8.f + frame * 0.125f, 5.95460939f, 1.43719971f,
                   0.4321f + frame * 0.001f); // Deliberately clipped/quantized SDR path.
    return false;
  }
  bool GetValueHdr(int frame, float out[4]) const override {
    ++hdrCalls;
    out[0] = 8.f + frame * 0.125f;
    out[1] = 5.95460939f;
    out[2] = 1.43719971f;
    out[3] = 0.4321f + frame * 0.001f;
    return false;
  }
};
class FrameVector : public CVectorElement {
public:
  bool GetValue(int frame, CVector3f& value) const override {
    value = CVector3f(float(frame), 2.5f, -3.75f);
    return false;
  }
};
class ConstReal : public CRealElement {
  float value_;
public:
  explicit ConstReal(float v) : value_(v) {}
  bool GetValue(int, float& value) const override { value = value_; return false; }
};
// The bomb ring's first TMTR, TRSS(-0.15, 0, 4, 1, 90, 1): E is degrees (GetValue 0x2de7dc), and the
// shipped VS (5f2a0715/461071b8 perm 000) unpacks the u16 fields and rotates by -E.
void TestTrss() {
  Check(std::fabs(PortVfxTrssOffset(-1.15f) + 0.15f) < 1e-4f && std::fabs(PortVfxTrssOffset(2.25f) - 0.25f) < 1e-4f,
        "TRSS offsets keep their signed fraction");
  Check(PortVfxTrssOffset(0.f) == std::fma(32767.f, 2.f / 65535.f, -1.f), "a zero offset unpacks to u16 32767");
  Check(PortVfxTrssAngle(90.f) == 16383.f * 9.58752644e-05f, "90 degrees packs to u16 16383");
  Check(PortVfxTrssAngle(450.f) == PortVfxTrssAngle(90.f), "angles wrap to one turn");
  Check(std::fabs(PortVfxTrssAngle(-90.f) - 4.712389f) < 2e-4f, "negative angles wrap into [0, 2pi)");
  Check(PortVfxTrssAngle(0.f) == 0.f, "no rotation stays exact");
  CPortVfxData vfx;
  vfx.vtmtCount = 1;
  for (float v : {-0.15f, 0.f, 4.f, 1.f, 90.f, 1.f}) {
    vfx.vtmt.push_back(new ConstReal(v));
  }
  CPortVfxUvXf xf;
  xf.Eval(vfx, 0);
  float uv[3][3];
  xf.Apply(1.f, 0.75f, uv);
  // The shader's o1.xy: off + 0.5 - 0.5 (c X + s Y) + (c X u + s Y v), off + 0.5 + 0.5 (s X - c Y) + (c Y v - s X u).
  const float e = 16383.f * 9.58752644e-05f, c = std::cos(e), sn = std::sin(e);
  const float u = PortVfxTrssOffset(-0.15f) + 0.5f - 0.5f * (c * 4.f + sn * 1.f) + (c * 4.f * 1.f + sn * 1.f * 0.75f);
  const float v = PortVfxTrssOffset(0.f) + 0.5f + 0.5f * (sn * 4.f - c * 1.f) + (c * 1.f * 0.75f - sn * 4.f * 1.f);
  Check(std::fabs(uv[0][0] - u) < 1e-5f && std::fabs(uv[0][1] - v) < 1e-5f && uv[0][2] == 1.f,
        "TRSS applies like the shipped vertex shader");
  Check(std::fabs(uv[0][0] - 0.6f) < 1e-3f && std::fabs(uv[0][1] + 1.5f) < 1e-3f,
        "90 degrees turns +u into -v");
}
void TestHdrEvaluation() {
  CGenDescription desc;
  desc.xPortVfx = std::make_unique<CPortVfxData>();
  auto& vfx = *desc.xPortVfx;
  const float high[4] = {8.f, 5.95460939f, 1.43719971f, 1.f};
  const float low[4] = {1.f, 0.3295990526676178f, 0.f, 1.f};
  auto addColor = [&](u32 row, u32 component, CColorElement* color) {
    CPortVfxElem e; e.kind = CPortVfxElem::kColor; e.a = row; e.b = component; e.color = color;
    vfx.vpmt.push_back(e);
  };
  addColor(0, 0, new CCEFastConstant(high[0], high[1], high[2], high[3]));
  addColor(1, 0, new CCEFastConstant(low[0], low[1], low[2], low[3]));
  auto* dynamic = new FrameColor;
  addColor(2, 0, dynamic);
  addColor(3, 2, new FrameColor); // Four-channel result must truncate at the row edge.
  addColor(3, 0, nullptr); // Null must not overwrite preceding values.
  auto* invalidRow = new FrameColor;
  addColor(4, 0, invalidRow); // Invalid row must not evaluate/write.
  CPortVfxElem real; real.real = new FrameReal(0.25f); real.a = 3;
  vfx.vpmt.push_back(real);
  CPortVfxElem vec; vec.kind = CPortVfxElem::kVector; vec.vec = new FrameVector; vec.a = 3; vec.b = 1;
  vfx.vpmt.push_back(vec);
  // VSMT shares EvalElem, consuming only its first component.
  CPortVfxElem system; system.kind = CPortVfxElem::kColor; system.a = 4; system.color = new FrameColor;
  vfx.vsmt.push_back(system);
  system.a = 5; system.color = nullptr; vfx.vsmt.push_back(system);
  for (int frame : {0, 7, 119}) {
    CElementGen::CParticle particle;
    particle.x2c_lineLengthOrSize = 2.75f;
    PortVfxEvalParticle(desc, particle, frame);
    float point[4][4] = {};
    float iten = -1.f;
    PortVfxEvalPoint(vfx, frame, iten, point);
    for (int c = 0; c < 4; ++c) {
      Check(particle.xPortVpmt[0][c] == high[c] && point[0][c] == high[c], "particle/point preserve exact HDR source endpoint");
      Check(particle.xPortVpmt[1][c] == low[c] && point[1][c] == low[c], "particle/point preserve fractional low endpoint");
      const float expected[] = {8.f + frame * 0.125f, high[1], high[2], 0.4321f + frame * 0.001f};
      Check(particle.xPortVpmt[2][c] == expected[c] && point[2][c] == expected[c], "particle/point reevaluate HDR RGB and fractional alpha each frame");
    }
    Check(particle.xPortSsze == 2.75f && particle.xPortIten == 1.f && iten == 1.f, "null size/intensity defaults unchanged");
    const float expected[] = {frame * 0.25f, float(frame), 2.5f, -3.75f};
    for (int c = 0; c < 4; ++c) {
      Check(particle.xPortVpmt[3][c] == expected[c] && point[3][c] == expected[c], "real/vector widths and evaluation unchanged");
    }
    float values[kPortVfxVsmtCount] = {};
    values[5] = 123.f;
    Check(PortVfxEvalVsmt(vfx, frame, values) == (1u << 4) && values[4] == 8.f + frame * 0.125f && values[5] == 123.f,
          "VSMT preserves HDR first component and skips null color");
  }
  Check(dynamic->sdrCalls == 0 && dynamic->hdrCalls == 6, "production particle/point path dispatches HDR, never SDR");
  Check(invalidRow->sdrCalls == 0 && invalidRow->hdrCalls == 0, "invalid row never evaluates color");
  // Isolate row-edge truncation and null behavior, without the real/vector overrides.
  delete vfx.vpmt.back().vec;
  vfx.vpmt.pop_back();
  delete vfx.vpmt.back().real;
  vfx.vpmt.pop_back();
  for (u32 kind : {CPortVfxElem::kReal, CPortVfxElem::kVector, CPortVfxElem::kInt}) {
    CPortVfxElem e; e.kind = kind; e.a = 3; vfx.vpmt.push_back(e);
  }
  float point[4][4] = {}; point[3][0] = 123.f; point[3][1] = 456.f;
  CElementGen::CParticle particle;
  particle.xPortVpmt[3][0] = 123.f; particle.xPortVpmt[3][1] = 456.f;
  float iten;
  PortVfxEvalParticle(desc, particle, 0);
  PortVfxEvalPoint(vfx, 0, iten, point);
  Check(point[3][0] == 123.f && point[3][1] == 456.f && point[3][2] == 8.f && point[3][3] == high[1],
        "null leaves row intact and color width truncates at component four");
  for (int c = 0; c < 4; ++c) {
    Check(particle.xPortVpmt[3][c] == point[3][c], "particle shares point null/width behavior");
  }
}
void TestDynamic() {
  CPortVfxData vfx;
  Parse(vfx, Blob(3));
  float values[kPortVfxVsmtCount + 1] = {};
  values[kPortVfxVsmtCount] = 123.f;
  aurora::gfx::vfx::DrawDesc desc;
  Check(PortVfxBuildDesc(vfx, values, 0, desc) && desc.opacityGain == 2.f && desc.opacityBias == 0.2f,
        "DrawDesc uses defaults without overrides");
  for (unsigned target : {0u, 18u, 19u, 20u, 21u, 31u}) {
    CPortVfxElem e; e.a = target; e.real = new FrameReal(float(target + 1) * 0.01f);
    vfx.vsmt.push_back(e);
  }
  for (int frame : {1, 2, 5}) {
    const auto mask = PortVfxEvalVsmt(vfx, frame, values);
    Check(mask == ((1u << 0) | (1u << 18) | (1u << 19) | (1u << 20)), "21-slot evaluation rejects out-of-range targets");
    Check(values[21] == 123.f, "evaluation does not overrun storage");
    Check(PortVfxBuildDesc(vfx, values, mask, desc) && desc.opacityGain == values[19] &&
              desc.opacityBias == values[20] && desc.erosion.value == values[0], "dynamic gain/bias and legacy source override");
    Check(std::fabs(values[19] - 0.2f * frame) < 1e-6f && std::fabs(values[20] - 0.21f * frame) < 1e-6f,
          "gain/bias evaluate anew each frame");
  }
  // Partial masks must not read a stale slot when only the other component is overridden.
  PortVfxBuildDesc(vfx, values, 1u << 19, desc);
  Check(desc.opacityGain == values[19] && desc.opacityBias == 0.2f, "gain-only override retains MATI bias");
  PortVfxBuildDesc(vfx, values, 1u << 20, desc);
  Check(desc.opacityGain == 2.f && desc.opacityBias == values[20], "bias-only override retains MATI gain");
}
} // namespace

int main() {
  TestParsing(); TestPhysicalTruncation(); TestDynamic(); TestHdrEvaluation(); TestTrss();
  CGenDescription exposure;
  exposure.xPortVfx = std::make_unique<CPortVfxData>();
  exposure.xPortExposurePolicy = true;
  exposure.xPortInverseExposure = true;
  exposure.xPortVfx->mat.blend = uint32_t(aurora::gfx::vfx::Blend::Opaque);
  Check(PortVfxExposureGain(exposure) == 0.5f, "opaque particles select static inverse exposure");
  exposure.xPortVfx->mat.blend = uint32_t(aurora::gfx::vfx::Blend::Alpha);
  Check(PortVfxExposureGain(exposure) == 0.25f, "blended particles select auto inverse exposure");
  exposure.xPortVfx->mat.features = aurora::gfx::vfx::ParticleLit;
  Check(PortVfxExposureGain(exposure) == 1.f, "lit sprites do not consume the intensity scalar");
  Check(PortVfxExposureGain(exposure, true) == 0.25f, "model instances consume inverse exposure even when lit");
  exposure.xPortInverseExposure = false;
  Check(PortVfxExposureGain(exposure, true) == 1.f, "explicit IEXP false leaves intensity unchanged");
  exposure.xPortInverseExposure = true;
  exposure.xPortExposurePolicy = false;
  Check(PortVfxExposureGain(exposure, true) == 1.f, "legacy PART keeps its separate compatibility policy");
  if (!failures) { std::puts("port_vfx_material: ok"); }
  return failures ? 1 : 0;
}
