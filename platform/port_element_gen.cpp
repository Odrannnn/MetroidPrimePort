// CElementGen's port-only members (the uncapped-presentation interpolation of
// the system and its newborn particles, and the effect debugger's queries),
// moved out of src/Kyoto/Particles/CElementGen.cpp so the decomp file keeps only
// its hooks (see docs/NATIVE_PORT.md, "Where port code goes"). They are declared
// under TARGET_PC in include/Kyoto/Particles/CElementGen.hpp.
//
// Built as part of mp_game, with the same flags and includes as the file it
// came from; the code was moved verbatim.

#include "Kyoto/Particles/CElementGen.hpp"

#include "Kyoto/Basics/CStopwatch.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CLight.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Graphics/CTevCombiners.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleElectric.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CParticleSwoosh.hpp"
#include "Kyoto/Particles/CSpawnSystemKeyframeData.hpp"
#include "Kyoto/Particles/CWarp.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/TToken.hpp"

#include "rstl/algorithm.hpp"
#include "rstl/math.hpp"

#include "alloca.h"
#include "math.h"

#include "dolphin/gx/GXGeometry.h"
#include "dolphin/gx/GXTev.h"

#include <cmath>

#ifdef TARGET_PC
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "port_clustered_lights.h"
#include "port_debug.h"
#include "port_fx_debug.h"
#include "port_room_env.h"
#include "port_vfx_particles.h"

#include "aurora/vis_query.hpp"
#endif

#ifdef TARGET_PC
// VISQ's occlusion queries, UpdateOcclusionQueries 0x2b0dd0, once per frame for a system whose
// description reads VISQ. It first takes last frames' results: with both a depth-tested (A) and
// an untested (B) count, the visibility is A/B (0 when B covered nothing); else it keeps its
// value. Then it draws both queries again: a box of half extent max((max-min)/2, 0.1) around the
// AABB pos +- (VISD/2, VISD/2, 0.05), VISD = (1, 1), so (0.5, 0.5, 0.1), at the render state's
// +0xa0 (below), turned to the camera (render_queries 0x2c2810: LookAt(center, eye, up = the
// camera's forward) x Scale), its back faces culled.
void CElementGen::PortVisibilityQuery() {
  static uint32_t sNextId = 0;
  const int frame = CGraphics::GetFrameCounter();
  if (xPortVisibilityFrame == frame) {
    return;
  }
  xPortVisibilityFrame = frame;
  if (xPortVisibilityId == 0) {
    xPortVisibilityId = ++sNextId;
    if (xPortVisibilityId == 0) {
      xPortVisibilityId = ++sNextId;
    }
  }
  const uint64_t keyA = uint64_t(xPortVisibilityId) << 32;
  const uint64_t keyB = keyA | 1;

  uint64_t samplesA = 0;
  uint64_t samplesB = 0;
  const bool haveA = aurora::gfx::vis_query::take(keyA, samplesA);
  const bool haveB = aurora::gfx::vis_query::take(keyB, samplesB);
  if (haveA && haveB) {
    xPortVisibility = samplesB != 0 ? float(double(samplesA) / double(samplesB)) : 0.f;
  }

  const CTransform4f& camera = CParticleGlobals::PortCameraTransform();
  // The render state's +0xa0, vec 0x12 (desc +0xb8, SetupTransformMode 0x2887d8) as is: under
  // XFMD 1 TEMT, under 2 the system translation over the system scale, under 3 and 4 (no EMVR)
  // zero. XFMD 5
  // (T_l/S_g + INTT) reads as 2 here; no VISQ generator uses it.
  CVector3f center = CVector3f::Zero();
  if (x28_loadedGenDesc->xPortXfmd == kPortXfmdSystem) {
    center = xPortTemTranslation; // TEMT
  } else if (x28_loadedGenDesc->xPortXfmd == kPortXfmdRetail) {
    const CVector3f scale = PortSystemScale();
    const CVector3f t = PortSystemTranslation();
    center = CVector3f(t.GetX() / scale.GetX(), t.GetY() / scale.GetY(), t.GetZ() / scale.GetZ());
  }
  const CVector3f eye = camera.GetTranslation();
  const CVector3f up = camera.GetForward();
  const float eps = 1.f / 524288.f; // 2^-19

  CVector3f f = eye - center;
  const float fLen = f.Magnitude();
  f = fLen > eps ? f * (1.f / fLen) : CVector3f(0.f, 0.f, 1.f);
  const float upDot = rstl::max_val(-1.f, rstl::min_val(1.f, CVector3f::Dot(up, f)));
  CVector3f r = up - f * upDot;
  if (r.Magnitude() <= eps) {
    r = CVector3f(0.f, 1.f, 0.f) - f * f.GetY();
    if (r.Magnitude() <= eps) {
      r = CVector3f(0.f, 0.f, 1.f) - f * f.GetZ();
    }
  }
  r = r.AsNormalized();
  const CVector3f axes[3] = {CVector3f::Cross(r, f), r, f};
  const float half[3] = {0.5f, 0.5f, 0.1f};

  // The six faces of the +-1 cube, each as its axis, sign and the two other axes.
  float verts[36][3];
  uint32_t count = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    for (int sign = -1; sign <= 1; sign += 2) {
      const CVector3f normal = axes[axis] * float(sign);
      const CVector3f faceCenter = center + normal * half[axis];
      if (CVector3f::Dot(normal, eye - faceCenter) <= 0.f) {
        continue;
      }
      const CVector3f du = axes[u] * half[u];
      const CVector3f dv = axes[v] * half[v];
      const CVector3f corners[4] = {faceCenter - du - dv, faceCenter + du - dv, faceCenter + du + dv,
                                    faceCenter - du + dv};
      static const int kTri[6] = {0, 1, 2, 0, 2, 3};
      for (int i = 0; i < 6; ++i) {
        const CVector3f& c = corners[kTri[i]];
        verts[count][0] = c.GetX();
        verts[count][1] = c.GetY();
        verts[count][2] = c.GetZ();
        ++count;
      }
    }
  }
  if (count == 0) {
    // The eye inside the box: Remastered's culled cube covers nothing, both counts read 0.
    for (int i = 0; i < 3; ++i) {
      verts[i][0] = center.GetX();
      verts[i][1] = center.GetY();
      verts[i][2] = center.GetZ();
    }
    count = 3;
  }
  CGraphics::SetModelMatrix(CTransform4f::Identity());
  aurora::gfx::vis_query::draw(keyA, verts, count, true);
  aurora::gfx::vis_query::draw(keyB, verts, count, false);
}

// Draws the system between its last two tick states when uncapped. The
// retail sub-frame path (x80_timeDeltaScale < 1) already draws particles
// between their previous and current positions; this feeds it the
// presentation factor, and blends the system's global translation. Systems
// drawn inside a shifted actor draw follow the actor instead.
bool CElementGen::PortBeginPresent(CVector3f& savedGlobal) {
  savedGlobal = xe8_globalTranslation;
  const float t = CCameraManager::GetPresentationInterpolation();
  if (t < 0.f || t >= 1.f || !PortDebug::ParticleInterpolation() ||
      CActor::PortRenderScopeActive())
    return false;
  const uint gen = CActor::PortTickGeneration();
  bool presenting = false;
  if (xPortGlobalGeneration == gen) {
    const CVector3f delta = xe8_globalTranslation - xPortPrevGlobalTranslation;
    // Same snap rule as actors: a jump of more than 4 units is a teleport.
    if (delta.MagSquared() <= 16.f) {
      xe8_globalTranslation = xPortPrevGlobalTranslation + delta * t;
      presenting = true;
    }
  }
  if (xPortStepGeneration == gen && x80_timeDeltaScale == 1.f) {
    x80_timeDeltaScale = t;
    presenting = true;
  }
  return presenting;
}

// How far (in particle space) the local emitter moved in this tick's step, when
// it stepped in this tick and the last one. Particles born in the step sit at
// the new emitter position, a tick ahead of the presented actor.
bool CElementGen::PortSpawnOffset(CVector3f& offset) const {
  const float t = CCameraManager::GetPresentationInterpolation();
  if (t < 0.f || t >= 1.f || !PortDebug::ParticleInterpolation() || PortFollowsEmitter())
    return false;
  const uint gen = CActor::PortTickGeneration();
  if (gen <= 1 || xPortStepGeneration != gen || x80_timeDeltaScale != 1.f ||
      xPortSpawnGeneration != gen || xPortPrevSpawnGeneration != gen - 1)
    return false;
  const CVector3f delta = xPortSpawnTranslation - xPortPrevSpawnTranslation;
  // Same snap rule as actors: a jump of more than 4 units is a teleport.
  const float magSq = delta.MagSquared();
  if (magSq == 0.f || magSq > 16.f)
    return false;
  offset = (x13c_globalScaleTransformInverse * x1a8_localScaleTransformInverse).Rotate(delta);
  return true;
}

void CElementGen::PortShiftNewborns(const CVector3f& shift) {
  xPortNewbornSaved.clear();
  const int born = x74_curFrame - 1;
  for (int i = 0; i < x30_particles.size(); ++i) {
    CParticle& particle = x30_particles[i];
    if (particle.x28_startFrame != born)
      continue;
    xPortNewbornSaved.push_back(particle.x4_pos);
    xPortNewbornSaved.push_back(particle.x10_prevPos);
    particle.x4_pos += shift;
    particle.x10_prevPos += shift;
  }
}

void CElementGen::PortSetNewbornLineShift(const CVector3f& firstEnd, const CVector3f& lastEnd,
                                          const CVector3f& firstShift,
                                          const CVector3f& lastShift) {
  xPortLineEnd[0] = firstEnd;
  xPortLineEnd[1] = lastEnd;
  xPortLineShift[0] = firstShift;
  xPortLineShift[1] = lastShift;
  xPortLineShiftSet = true;
}

void CElementGen::PortShiftNewbornsAlongLine() {
  xPortNewbornSaved.clear();
  // Same world → particle space mapping as the spawn position.
  const CTransform4f toLocal = x13c_globalScaleTransformInverse * x1a8_localScaleTransformInverse;
  const CVector3f first = toLocal * xPortLineEnd[0];
  const CVector3f line = toLocal * xPortLineEnd[1] - first;
  const float lenSq = line.MagSquared();
  const CVector3f firstShift = toLocal.Rotate(xPortLineShift[0]);
  const CVector3f lastShift = toLocal.Rotate(xPortLineShift[1]);
  const int born = x74_curFrame - 1;
  for (int i = 0; i < x30_particles.size(); ++i) {
    CParticle& particle = x30_particles[i];
    if (particle.x28_startFrame != born)
      continue;
    float s = 0.f;
    if (lenSq > 0.f) {
      s = CVector3f::Dot(particle.x4_pos - first, line) / lenSq;
      s = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
    }
    const CVector3f shift = firstShift + (lastShift - firstShift) * s;
    xPortNewbornSaved.push_back(particle.x4_pos);
    xPortNewbornSaved.push_back(particle.x10_prevPos);
    particle.x4_pos += shift;
    particle.x10_prevPos += shift;
  }
}

void CElementGen::PortRestoreNewborns() {
  if (xPortNewbornSaved.empty())
    return;
  const int born = x74_curFrame - 1;
  int saved = 0;
  for (int i = 0; i < x30_particles.size() && saved < xPortNewbornSaved.size(); ++i) {
    CParticle& particle = x30_particles[i];
    if (particle.x28_startFrame != born)
      continue;
    particle.x4_pos = xPortNewbornSaved[saved++];
    particle.x10_prevPos = xPortNewbornSaved[saved++];
  }
  xPortNewbornSaved.clear();
}
#endif

#ifdef TARGET_PC
uint CElementGen::PortFxAsset() const { return CToken(x1c_genDesc).GetTag().GetId(); }

void CElementGen::PortCollectNestedLightSnapshots(
    std::vector< CPortNestedLightSnapshot >& out, bool includeSelf) const {
  if (includeSelf && x28_loadedGenDesc->xPortNestedHdrLight &&
      x28_loadedGenDesc->xPortNestedLightInnerRadius != nullptr &&
      x28_loadedGenDesc->xPortNestedLightOuterRadius != nullptr && SystemHasLight() &&
      xPortNestedHdrRadiiValid) {
    CLight light = GetLight();
    // CGameLight applies its actor transform to this zero local position;
    // its PBR carrier then uses this same world origin with no EMTR/LOFF.
    const float intensity = x310_LINT * PortRoomEnv::ParticleLightGain(
                                            x28_loadedGenDesc->xPortNestedLightInverseExposure);
    PortSetNestedHdrCarrier(light, xPortNestedHdrColor, intensity, xPortNestedHdrInnerRadius,
                            xPortNestedHdrOuterRadius,
                            x28_loadedGenDesc->xPortNestedLightFalloff);
    light.SetPortHdrWiden(x28_loadedGenDesc->xPortNestedLightWiden);
    CVector3f origin;
    CVector3f axis;
    PortNestedLightFrame(origin, axis);
    if (x28_loadedGenDesc->xPortNestedLightSpot) {
      // CGameLight moves this light by translation only, so the axis stays in world space.
      float scale;
      float bias;
      PortClusteredLights::ConeAttenuation(true, x28_loadedGenDesc->xPortNestedLightInnerAngle,
                                           x28_loadedGenDesc->xPortNestedLightOuterAngle, scale,
                                           bias);
      light.SetPortHdrCone(axis, scale, bias);
    }
    out.push_back({this, origin, light});
  }

  for (int i = 0; i < GetNumActiveChildParticles(); ++i) {
    CParticleGen* child = GetActiveChildParticle(i);
    if (child != nullptr && child->Get4CharId() == 'PART') {
      static_cast< CElementGen* >(child)->PortCollectNestedLightSnapshots(out, true);
    }
  }
}

void CElementGen::PortFxDescribe(PortFxInfo& out) const {
  out.kind = 'PART';
  out.asset = PortFxAsset();
  out.particles = static_cast< int >(x30_particles.size());
  out.maxParticles = x90_MAXP;
  out.frame = x74_curFrame;
  out.lifetime = x268_PSLT;
  out.emitting = x88_particleEmission;
  out.deletable = IsSystemDeletable();
  out.pos[0] = xe8_globalTranslation.GetX();
  out.pos[1] = xe8_globalTranslation.GetY();
  out.pos[2] = xe8_globalTranslation.GetZ();
  for (AUTO(it, x290_activePartChildren.begin()); it != x290_activePartChildren.end(); ++it) {
    out.children.push_back(*it);
  }
  for (size_t i = 0; i < xPortPsps.size(); ++i) {
    if (xPortPsps[i].sys != nullptr) {
      out.children.push_back(xPortPsps[i].sys);
    }
  }
  const CGenDescription& desc = *x28_loadedGenDesc;
  char buf[64];
  if (const CPortVfxData* vfx = desc.xPortVfx.get()) {
    out.vfx = vfx->mat.Supported() ? "VMAT" : "VMAT(unsupported)";
    if (vfx->vtmtCount != 0) {
      std::snprintf(buf, sizeof(buf), " VTMT*%u", vfx->vtmtCount);
      out.vfx += buf;
    }
    if (vfx->vtrs[0] != nullptr) {
      out.vfx += " VTRS";
    }
    if (vfx->vtrt[0] != nullptr) {
      out.vfx += " VTRT";
    }
    if (vfx->txfbLayers != 0) {
      std::snprintf(buf, sizeof(buf), " VTFB*%u", vfx->txfbLayers);
      out.vfx += buf;
    }
    if (!vfx->atexRects.empty()) {
      std::snprintf(buf, sizeof(buf), " %s*%zu", vfx->txp2Index != nullptr       ? "VTP2"
                                                    : vfx->atexCycleElem != nullptr ? "VAT2"
                                                                                    : "VATX",
                    vfx->atexRects.size() / 4);
      out.vfx += buf;
    }
    if (vfx->atfbLayers > 0) {
      std::snprintf(buf, sizeof(buf), " VATF*%u", vfx->atfbLayers);
      out.vfx += buf;
    }
    if (!vfx->vpmt.empty()) {
      std::snprintf(buf, sizeof(buf), " VPMT*%zu", vfx->vpmt.size());
      out.vfx += buf;
    }
    if (!vfx->vsmt.empty()) {
      std::snprintf(buf, sizeof(buf), " VSMT*%zu", vfx->vsmt.size());
      out.vfx += buf;
    }
    if (vfx->iten != nullptr) {
      out.vfx += " ITEN";
    }
    if (vfx->vorn != 0) {
      std::snprintf(buf, sizeof(buf), " VORN%u", vfx->vorn);
      out.vfx += buf;
    }
    if (desc.xPortOvec != nullptr) {
      out.vfx += " OVEC";
    }
    if (vfx->meshTris != 0) {
      std::snprintf(buf, sizeof(buf), " VMSH(%u tris)", vfx->meshTris);
      out.vfx += buf;
    }
  }
  if (desc.xPortXfmd != kPortXfmdRetail) {
    std::snprintf(buf, sizeof(buf), "%sXFMD%u", out.vfx.empty() ? "" : " ", unsigned(desc.xPortXfmd));
    out.vfx += buf;
  }
  if (desc.xPortIrnd) {
    out.vfx += out.vfx.empty() ? "IRND" : " IRND";
  }
  if (desc.xPortVisibilityQuery) {
    std::snprintf(buf, sizeof(buf), "%sVISQ=%.3f", out.vfx.empty() ? "" : " ", xPortVisibility);
    out.vfx += buf;
  }
  if (desc.xPortSsze != nullptr) {
    out.vfx += out.vfx.empty() ? "SSZE" : " SSZE";
  }
  if (!desc.xPortPMDV.empty()) {
    std::snprintf(buf, sizeof(buf), "%sPMDV*%zu", out.vfx.empty() ? "" : " ", desc.xPortPMDV.size());
    out.vfx += buf;
  }
}
#endif
