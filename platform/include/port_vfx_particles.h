#pragma once

// Runtime half of the native Remastered particle material (VMAT). The draw itself is
// aurora::gfx::vfx::draw_quads; this file evaluates the per-particle properties (SSZE, ITEN, VPMT)
// and the per-frame VSMT, and expands the quads. Contract: docs/REMASTERED_VFX.md.
// A PART without VMAT never reaches any of it.

#include "Kyoto/Particles/CElementGen.hpp"
#include <cmath>
#include <cstdint>
#include <vector>

#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "aurora/vfx.hpp"

// True when the description carries a VMAT this runtime understands (version 2 or 3).
inline bool PortVfxActive(const CGenDescription& desc) {
  return desc.xPortVfx != nullptr && (desc.xPortVfx->mat.version == 2 || desc.xPortVfx->mat.version == 3);
}

// Evaluates SSZE (default: the particle's SIZE), ITEN (default 1) and the VPMT rows into the
// particle. The caller has set CParticleGlobals' particle context for `frame`.
void PortVfxEvalParticle(const CGenDescription& desc, CElementGen::CParticle& particle, int frame);

// Stores the unit launch direction (zero when the particle launched at rest); for VORN 1.
void PortVfxSetLaunchDir(CElementGen::CParticle& particle);
float PortVfxExposureGain(const CGenDescription& desc, bool model = false);
// Explicit inverse-exposed transparent models snapshot the matching forward frame exposure.
std::optional<float> PortVfxModelExposureOverride(const CGenDescription& desc);

// Model particles with a VMSH mesh: RenderModels adds each live particle's CPU-transformed
// triangles (world space) and PortRenderMeshesVfx draws them all in one aurora draw with the
// PART's VMAT. A PART with PMDV variants keeps the retail model path (VMSH holds one model).
struct CPortVfxMeshBatch {
  std::vector< aurora::gfx::vfx::Vertex > verts;
  // Where the batch's clustered lighting looks: the Lit material (IceChargeCollisionLit) samples the
  // room's baked probe at the first particle's model origin, as the stand-in for Remastered's per-instance
  // BLPD record. The cluster grid and its light list need nothing per batch (they are per camera).
  float probePos[3] = {};
  bool probePosSet = false;

  // True when adding `vfx`'s mesh would take the batch past one aurora draw (the size
  // draw_triangles splits at); the caller draws and clears it first, so it stays bounded.
  bool WouldOverflow(const CPortVfxData& vfx) const {
    return !verts.empty() &&
           verts.size() + size_t(vfx.meshTris) * 3 > size_t(aurora::gfx::vfx::MaxTrianglesPerDraw) * 3;
  }

  // `model` is the particle's full model matrix; the particle's UV context must be set.
  void Add(const CPortVfxData& vfx, const CTransform4f& model, const CElementGen::CParticle& particle,
           int partFrame, const CColor& modulate, float inverseExposure = 1.f);
};

// Fills the draw description from the VMAT and the VSMT overrides in `vsmt` (bit i of
// `vsmtMask` set: slot i is overridden); false while a texture is not streamed in. Loads the
// textures into their GX slots.
bool PortVfxBuildDesc(const CPortVfxData& vfx, const float* vsmt, uint vsmtMask,
                      aurora::gfx::vfx::DrawDesc& desc);

// Evaluates the VSMT overrides at `frame` into `vsmt`; returns the mask.
uint PortVfxEvalVsmt(const CPortVfxData& vfx, int frame, float vsmt[kPortVfxVsmtCount]);

// ITEN (default 1) and the VPMT rows at `frame`, for a swoosh point. The caller has set
// CParticleGlobals' particle context.
void PortVfxEvalPoint(const CPortVfxData& vfx, int frame, float& iten, float vpmt[4][4]);
// The two halves of PortVfxEvalPoint. PortVfxEvalIten keeps `iten` when the element writes no
// value: an init-only IRND (CREInitialRandom) writes at frame 0 only, as Remastered's init list
// evaluates it once and its update list leaves it.
void PortVfxEvalIten(const CPortVfxData& vfx, int frame, float& iten);
void PortVfxEvalVpmt(const CPortVfxData& vfx, int frame, float vpmt[4][4]);

// A TRSS UV offset (A or B) after pack_texture_transform 0x20e8f0 and the VS unpack: the signed
// fraction goes out as the u16 trunc((f + 1) / 2 * 65535) and comes back as u16 * (2 / 65535) - 1.
inline float PortVfxTrssOffset(float x) {
  const float f = x - std::trunc(x);
  return std::fma(float(uint32_t((f + 1.f) * 0.5f * 65535.f)), 2.f / 65535.f, -1.f);
}

// A TRSS rotation (E, in degrees: GetValue 0x2de7dc takes it times 0.0174533) in radians as the VS
// sees it: wrapped to [0, 2pi), packed as the u16 trunc(min(E / 2pi, 1) * 65535), unpacked as
// u16 * 2pi / 65536.
inline float PortVfxTrssAngle(float degrees) {
  float a = degrees * 0.017453292f;
  a -= float(int(a * 0.15915494f)) * 6.2831855f;
  if (a < 0.f) {
    a += 6.2831855f;
  }
  return float(uint32_t(std::fmin(a / 6.2831855f, 1.f) * 65535.f)) * 9.58752644e-05f;
}

// The VTMT rows evaluated at one frame, as the shipped particle VS (461071b8 and 5f2a0715 perm
// 000/012) applies them: uv = (A, B) + 0.5 + R(-E) diag(C, D) (q - 0.5), layer F, i.e.
// u' = A + 0.5 + cos E du + sin E dv, v' = B + 0.5 - sin E du + cos E dv.
struct CPortVfxUvXf {
  float a[3] = {}, b[3] = {}, c[3] = {1.f, 1.f, 1.f}, d[3] = {1.f, 1.f, 1.f};
  float cosE[3] = {1.f, 1.f, 1.f}, sinE[3] = {}, f[3] = {};
  void Eval(const CPortVfxData& vfx, int frame);
  void Apply(float qx, float qy, float uv[3][3]) const;
};

// IceChargeBeam's feed needs VMSH v2 with a real tangent and colour, and VPRM's NMAP slot and
// CCH0..2; IceChargeCollision's a real tangent, VPRM v2's NMAP and TCH0 slots and CCH0..6. Without
// them the PART keeps the retail model path.
inline bool PortVfxIceReady(const CPortVfxData& vfx) {
  if ((vfx.mat.features & aurora::gfx::vfx::IceChargeCollision) != 0) {
    return vfx.meshStride == 16 && (vfx.meshFlags & 1) == 1 && vfx.normalSlot >= 0 && vfx.envSlot >= 0 &&
           vfx.paramCount >= 7;
  }
  return (vfx.mat.features & aurora::gfx::vfx::IceChargeBeam) == 0 ||
         (vfx.meshStride == 16 && (vfx.meshFlags & 3) == 3 && vfx.normalSlot >= 0 && vfx.paramCount >= 3);
}

// VFX_BarrierShield's feed needs the mesh's own vertex colour (VMSH v2, flags bit 1; the shader reads it
// apart from the instance colour), both pattern maps (TCH0 through the colour slot, TCH1 through the
// indirect slot) and CCH0. Without them the PART keeps the retail model path rather than draw the
// recipe with a mesh colour or a map it does not have.
inline bool PortVfxBarrierReady(const CPortVfxData& vfx) {
  return (vfx.mat.features & aurora::gfx::vfx::BarrierShield) == 0 ||
         (vfx.meshStride == 16 && (vfx.meshFlags & 2) == 2 && vfx.mat.colorSlot >= 0 &&
          vfx.mat.indirectSlot >= 0 && vfx.paramCount >= 1);
}

// VFX_BarrierGlow's feed is BarrierShield's without the second map (this perm reads TCH0 alone, three
// times) and without the scene copy: it needs the mesh's own vertex colour (VMSH v2, flags bit 1), the
// pattern map through the colour slot and CCH0. Without them the PART keeps the retail model path.
inline bool PortVfxBarrierGlowReady(const CPortVfxData& vfx) {
  return (vfx.mat.features & aurora::gfx::vfx::BarrierGlow) == 0 ||
         (vfx.meshStride == 16 && (vfx.meshFlags & 2) == 2 && vfx.mat.colorSlot >= 0 && vfx.paramCount >= 1);
}

inline bool PortVfxHasMesh(const CGenDescription& desc) {
  return PortVfxActive(desc) && desc.xPortVfx->meshTris != 0 && desc.xPortPMDV.empty() &&
         PortVfxIceReady(*desc.xPortVfx) && PortVfxBarrierReady(*desc.xPortVfx) &&
         PortVfxBarrierGlowReady(*desc.xPortVfx);
}

// VPRM's CCHn rows with the VSMT overrides (targets kPortVfxParamTarget..) at `frame`.
void PortVfxEvalParams(const CPortVfxData& vfx, int frame, float params[7][4]);
