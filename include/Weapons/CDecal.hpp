#ifndef _CDECAL
#define _CDECAL

#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/TToken.hpp"
#include "Weapons/CDecalDescription.hpp"

#include <types.h>

class CDecalDescription;
#ifdef TARGET_PC
namespace PortProjectedDecal {
struct Mesh;
}
#endif
class CDecal {
  static CRandom16 sDecalRandom;
  static bool sMoveRedToAlphaBuffer;

public:
  class CQuadDecal {
  public:
    CQuadDecal() : x0_24_invalid(true), x4_lifetime(0), x8_rotation(0.f) {}
    CQuadDecal(int lifetime, float rotation)
    : x0_24_invalid(true), x4_lifetime(lifetime), x8_rotation(rotation) {}

    inline bool IsInvalid() const { return x0_24_invalid; }
    inline void SetInvalid(bool invalid) { x0_24_invalid = invalid; }

    inline int GetLifetime() const { return x4_lifetime; }
    inline void SetLifetime(int lifetime) { x4_lifetime = lifetime; }

    inline float GetRotation() const { return x8_rotation; }
    inline void SetRotation(float rotation) { x8_rotation = rotation; }

    // private:
    bool x0_24_invalid : 1;
    int x4_lifetime;
    float x8_rotation;
  };

  static void SetGlobalSeed(ushort seed);
  static void SetMoveRedToAlphaBuffer(const bool move) { sMoveRedToAlphaBuffer = move; }
  CDecal(const TToken< CDecalDescription >& desc, const CTransform4f& xf);
#ifdef TARGET_PC
  // Remastered's CDecal (ctor 0xb7ca04) for a description with xPortRemastered: its own random
  // seeded by the manager, MSTL lifetime, MSTR rotation and the MSTW/MSTH box.
  CDecal(const TToken< CDecalDescription >& desc, uint seed, const CTransform4f& xf);
  bool IsPortRemastered() const { return xPortRemastered; }
  // Remastered's done state: lifetime and model life both over (flags == 3).
  bool IsPortRemasteredDone() const { return xPortFlags == 3; }
  int GetPortFrame() const { return xPortFrame; }
  int GetPortLifetime() const { return xPortLifetime; }
  float GetPortAngle() const { return xPortAngle; }
  // The projection box: half-extents along the decal's local X and Z (MSTW, MSTH) and its depth
  // along local Y (skDecalBoxHeight).
  float GetPortWidth() const { return xPortWidth; }
  float GetPortHeight() const { return xPortHeight; }
  float GetPortBoxDepth() const { return xPortBoxDepth; }
  // Remastered's decal transform (+0x4c): the spawn transform times RotateZ(MSTR).
  const CTransform4f& GetPortTransform() const { return xPortTransform; }
  const CDecalDescription& GetDescription() const { return *x0_description.operator->(); }
  // A material slot's draw data at the current frame (UpdateMaterialData 0xb7d07c): the MATI's
  // DIFC/CCH0/CCH1 with the SMTR writes, the material alpha (DUL) and the TMTR's TRSS(A..F).
  struct SPortSlotData {
    float xAlpha;
    float xParams[3][4];
    float xTrss[6];
  };
  void PortUpdateMaterialData(int slot, SPortSlotData& out) const;
  // RenderDecalGeometry 0xb386a8's draw of this decal's clipped mesh (world-space triangles with
  // their box UV) into the HDR scene: each loaded material slot of the pass, slot 0 then 1.
  void PortRenderProjected(const PortProjectedDecal::Mesh& mesh, int pass) const;
#endif

  void RenderQuad(CQuadDecal& quad, const CDecalDescription::SQuadDescr& quadDesc) const;
  void RenderMdl() const;
  void Render() const;
  void Update(float dt);

  bool IsDone() const { return x5c_flags == 7; }
  CVector3f GetTranslation() const { return xc_transform.GetTranslation(); }

private:
  TLockedToken< CDecalDescription > x0_description;
  CTransform4f xc_transform;
  CQuadDecal x3c_quad1;
  CQuadDecal x48_quad2;
  int x54_modelLifetime;
  int x58_frameIdx;
  int x5c_flags;
  mutable CVector3f x60_rotation;
  // Port: real elapsed time. The description tables are authored per 60 Hz
  // frame, so x58_frameIdx is derived from this rather than incremented per
  // tick; at a raised simulation rate the decal keeps its authored duration.
  float x6c_elapsedTime;
#ifdef TARGET_PC
  bool xPortRemastered;
  CRandom16 xPortRandom;
  CTransform4f xPortBaseTransform;
  CTransform4f xPortTransform;
  float xPortWidth;
  float xPortHeight;
  float xPortBoxDepth;
  int xPortLifetime;
  int xPortModelLifetime;
  int xPortFrame;
  double xPortTime;
  float xPortAngle;
  uint xPortFlags;

  void PortRemasteredUpdate(float dt);
#endif

  void InitQuad(CQuadDecal& quad, const CDecalDescription::SQuadDescr& desc, int flag) {
    if (!desc.x14_TEX.null()) {
      if (!desc.x0_LFT.null()) {
        desc.x0_LFT->GetValue(0, quad.x4_lifetime);
      } else {
        quad.x4_lifetime = 0x7FFFFF;
      }

      if (!desc.x8_ROT.null()) {
        desc.x8_ROT->GetValue(0, quad.x8_rotation);
        quad.x0_24_invalid &= desc.x8_ROT->IsConstant();
      }

      if (!desc.x4_SZE.null()) {
        quad.x0_24_invalid &= desc.x4_SZE->IsConstant();
        if (quad.x0_24_invalid) {
          float size = 1.f;
          desc.x4_SZE->GetValue(0, size);
          quad.x0_24_invalid = size <= 1.f;
        }
      }

      if (!desc.xc_OFF.null()) {
        quad.x0_24_invalid &= desc.xc_OFF->IsFastConstant();
      }
    } else {
      quad.x0_24_invalid = false;
      x5c_flags |= flag;
    }
  }

  void ProcessQuad(CQuadDecal& quad, const CDecalDescription::SQuadDescr& desc, int flag) const {
    if (!desc.x14_TEX.null() && (x5c_flags & flag) == 0) {
      CParticleGlobals::SetParticleLifetime(quad.x4_lifetime);
      CParticleGlobals::UpdateParticleLifetimeTweenValues(x58_frameIdx);
      RenderQuad(quad, desc);
    }
  }
};
#endif // _CDECAL
