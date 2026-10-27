#ifndef _CPARTICLEELECTRIC2PORT
#define _CPARTICLEELECTRIC2PORT

#include <array>
#include <vector>

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "port_electric2_math.h"

class CElectricDescription;

// Remastered CParticleElectric2 (ELC2): camera-facing bolts baked into world space at creation
// (CreateNewParticles 0xb76508), aged and re-evaluated per substep (UpdateElectricalEffects
// 0xb757a4, UpdateBolt 0xb77638) and drawn as SIDE 2 ribbons (UpdateRenderData_Electric2
// 0x2b8694). A CParticleElectric whose description carries the PEL2 marker owns one and
// delegates its update, bounds and render to it.
class CParticleElectric2Port {
public:
  // Per-call view of the owner's state.
  struct Ctx {
    CTransform4f xf = CTransform4f::Identity(); // global * local transform
    CVector3f globalScale{1.f, 1.f, 1.f};
    CVector3f localScale{1.f, 1.f, 1.f};
    // PortSetSpawnSpread in the bolt points' space, applied to the emitted end points only.
    CTransform4f spread = CTransform4f::Identity();
    CColor modu = CColor::White();
    const CVector3f* ipos = nullptr; // overrides (SetOverrideIPos etc.)
    const CVector3f* ivel = nullptr;
    const CVector3f* fpos = nullptr;
    const CVector3f* fvel = nullptr;
  };

  explicit CParticleElectric2Port(const CElectricDescription& desc);

  // One 1/60 s substep at `frame`: ages and updates the bolts, then (emitting) spawns new ones.
  void Step(int frame, bool emitting, const Ctx& ctx);
  // CreateNewParticles for `count` bolts, ignoring GRAT.
  void Create(int frame, int count, const Ctx& ctx);

  int BoltCount() const { return int(mBolts.size()); }
  int LineCount() const;
  int NodeCount() const { return mNodes; }
  int PointCount() const;
  int MaxBolts() const { return mMaxBolts; }
  CAABox Bounds() const;
  // Draws all bolts as ribbons; the caller has set the GX state.
  void Render(int frame, const CTransform4f& view);

  struct Bolt {
    std::vector< std::vector< PortElectric2::V3 > > lines; // world space; [0] is the primary
    int life = 1;
    int startFrame = 0;
    int endFrame = 1;
    std::vector< float > leftRad;  // per node (UpdateBolt 0xb77638 node+0x28 x RADS)
    std::vector< float > rightRad; // per node (node+0x2c x RADS)
    std::vector< std::array< float, 4 > > color; // per node: NCLR x COLR
    std::vector< float > iten;                   // per node (the sub-particle AddVar buffer)
    float vpmt[4][4] = {};                       // per bolt (render indexes PMTR by bolt)
    // The primary line's end points before the bake (the owner's space, unscaled), where
    // CreateNewParticles 0xb76508 places the GPSM and EPSM children, and the children taken.
    PortElectric2::V3 first{};
    PortElectric2::V3 last{};
    int gpsm = -1;
    int epsm = -1;
  };
  const std::vector< Bolt >& Bolts() const { return mBolts; }
  std::vector< Bolt >& Bolts() { return mBolts; }
  // The bolts the last Step created (the last Created() of Bolts()), and the children of the ones
  // it retired (UpdateElectricalEffects 0xb757a4 shuts those down).
  int Created() const { return mCreated; }
  struct Retired {
    int gpsm;
    int epsm;
  };
  const std::vector< Retired >& RetiredChildren() const { return mRetired; }
  // Beam_Reset 0xb784ec: no bolts, no carried generation.
  void Reset();

private:
  void CreateBolt(int frame, const Ctx& ctx);
  void MakeBranches(const std::vector< PortElectric2::V3 >& parent, float amp, float ampd, int depth, int maxDepth,
                    const PortElectric2::BoltParams& prm, std::vector< std::vector< PortElectric2::V3 > >& out,
                    int frame);
  void UpdateBolt(Bolt& bolt, int frame);
  // A bolt point in world space: points are baked at creation, or (XFMD 3) local and moved by the
  // owner's current transform here.
  PortElectric2::V3 WorldPoint(const PortElectric2::V3& p) const;

  const CElectricDescription* mDesc;
  PortElectric2::Rng mRng;
  int mNodes = 8;
  int mSide = 2;
  int mMaxBolts = 1;
  float mRem = 0.f;
  CColor mModu = CColor::White();
  CTransform4f mXf = CTransform4f::Identity(); // the owner's latest xf, for XFMD 3 bolts
  CVector3f mScale{1.f, 1.f, 1.f};              // global * local scale of the same
  std::vector< Bolt > mBolts;
  int mCreated = 0;
  std::vector< Retired > mRetired;
};

#endif // _CPARTICLEELECTRIC2PORT
