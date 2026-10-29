#include "Kyoto/Particles/CParticleElectric2Port.hpp"
#include "port_env.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Particles/CElectricDescription.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "port_fx_debug.h"
#include "port_room_env.h"
#include "port_vfx_particles.h"

using PortElectric2::V3;

namespace {
float RealOr(const CRealElement* e, int frame, float def) {
  float v = def;
  if (e != nullptr) {
    e->GetValue(frame, v);
  }
  return v;
}

int IntOr(const CIntElement* e, int frame, int def) {
  int v = def;
  if (e != nullptr) {
    e->GetValue(frame, v);
  }
  return v;
}

V3 ToV3(const CVector3f& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
CVector3f ToVec(const V3& v) { return CVector3f(v.x, v.y, v.z); }
} // namespace

CParticleElectric2Port::CParticleElectric2Port(const CElectricDescription& desc) : mDesc(&desc) {
  // Ctor 0xb728cc: SSEG (default 8) is clamped to >= 1, then incremented, so a bolt has SSEG + 1
  // nodes (this+0x158/0x670), as retail's ++x150_SSEG; SSEG 1 is a straight two-node beam.
  mNodes = std::max(1, IntOr(desc.x10_SSEG, 0, 8)) + 1;
  if (port::EnvFlag("MP_ELC2_LOG")) {
    std::fprintf(stderr, "elc2: created (nodes %d, vfx %d)\n", mNodes, desc.xPortVfx != nullptr);
  }
  mSide = IntOr(desc.xPortSIDE, 0, 2);
  mMaxBolts = std::clamp(IntOr(desc.xc_SCNT, 0, 1), 1, 32);
}

int CParticleElectric2Port::LineCount() const {
  int n = 0;
  for (const Bolt& b : mBolts) {
    n += int(b.lines.size());
  }
  return n;
}

int CParticleElectric2Port::PointCount() const {
  int n = 0;
  for (const Bolt& b : mBolts) {
    for (const auto& l : b.lines) {
      n += int(l.size());
    }
  }
  return n;
}

CAABox CParticleElectric2Port::Bounds() const {
  bool any = false;
  V3 lo, hi;
  float rad = 0.f;
  for (const Bolt& b : mBolts) {
    for (size_t i = 0; i < b.leftRad.size(); ++i) {
      rad = std::max(rad, std::max(b.leftRad[i], b.rightRad[i]));
    }
    for (const auto& l : b.lines) {
      for (const V3& pl : l) {
        const V3 p = mDesc->xPortXfmdFollow ? WorldPoint(pl) : pl;
        if (!any) {
          lo = hi = p;
          any = true;
          continue;
        }
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
    }
  }
  if (!any) {
    return CAABox::MakeNullBox();
  }
  return CAABox(CVector3f(lo.x - rad, lo.y - rad, lo.z - rad), CVector3f(hi.x + rad, hi.y + rad, hi.z + rad));
}

V3 CParticleElectric2Port::WorldPoint(const V3& p) const {
  return ToV3(mXf * CVector3f(p.x * mScale.GetX(), p.y * mScale.GetY(), p.z * mScale.GetZ()));
}

void CParticleElectric2Port::UpdateBolt(Bolt& bolt, int systemFrame) {
  // UpdateBolt 0xb77638: radii (LLRD copies LRAD to RRAD), colour, material point values.
  // COLR (desc+0x140) is the bolt's own: it is evaluated once before the node loop (into
  // lineMgr+0x38). RADS/LRAD/RRAD (desc+0x138/0x128/0x130) and NCLR (desc+0x148) are inside it, so the
  // original evaluates them once per node, with the context time (ctx+0x68) and life (ctx+0x6c,
  // ctx+0x70 = end - start) of the bolt and the node's own tween (ctx+0x80: frame i, life nodes - 1)
  // alongside, in this order per node: LRAD, RRAD, RADS, NCLR. KESP (slot-2 keyframes) reads the
  // node tween, e.g. the AtomicBeta ELC2's RADS taper 0 -> 1 along the bolt, and an NCLR that draws
  // a random (SLCT(IRND, ...)) draws one per node.
  // Node colour = NCLR modulated by COLR: UpdateRenderState 0xb760e0 builds COLR (lineMgr+0x38)
  // x the owner's modulate colour (this+0x90) with CColor4f16::Modulate and Modulates every node's
  // NCLR colour with it (Modulate is a component multiply (unverified: body not decompiled)).
  // NCLR's GRAD runs on the frame argument (the bolt's age, CCEGradient::GetValue 0x2c8d48) over an
  // ILPT period read from slot 2's life (nodes - 1), which the port sets as the lifetime around it.
  // CreateNewParticles 0xb76508 calls it right after the bolt starts, so the age is 0 there.
  const int frame = systemFrame - bolt.startFrame;
  float colr[4] = {1.f, 1.f, 1.f, 1.f};
  if (mDesc->x14_COLR != nullptr) {
    mDesc->x14_COLR->GetValueHdr(frame, colr);
  }
  const size_t nodes = static_cast<size_t>(mNodes);
  // The AddVar system (desc+0x270) runs next, once per node with the node tween set, into a
  // per-node buffer: _Init_SubParticle when the bolt is created, else _Update_SubParticle. Render
  // (UpdateRenderData_Electric2 0x2b8694, GatherSubParticleAddlVar 0x2b9740) gives a segment's
  // node-i vertices that node's ITEN (+0x8c) and its node-(i+1) vertices the next node's (+0x90),
  // so an init-only IRND ITEN (C30EEDEF, F0826A58: IRND(2, 10)) is one draw per node, kept for
  // the bolt's life. PMTR goes out per bolt (indexed by bolt in FUN_002b9868), so VPMT stays one.
  if (bolt.iten.size() != nodes) {
    bolt.iten.assign(nodes, 1.f);
  }
  if (mDesc->xPortVfx) {
    for (int i = 0; i < mNodes; ++i) {
      CParticleGlobals::PortSetSlot2Time(i, mNodes - 1);
      PortVfxEvalIten(*mDesc->xPortVfx, frame, bolt.iten[size_t(i)]);
    }
  }
  bolt.leftRad.resize(nodes);
  bolt.rightRad.resize(nodes);
  bolt.color.resize(nodes);
  for (int i = 0; i < mNodes; ++i) {
    CParticleGlobals::PortSetSlot2Time(i, mNodes - 1);
    float lrad = 0.f, rrad = 0.f;
    if (mDesc->xPortLRAD != nullptr) {
      mDesc->xPortLRAD->GetValue(frame, lrad);
    }
    if (mDesc->xPortRRAD != nullptr) {
      mDesc->xPortRRAD->GetValue(frame, rrad);
    }
    const float rads = RealOr(mDesc->xPortRADS, frame, 1.f);
    const PortElectric2::BoltRadii radii = PortElectric2::ComputeBoltRadii(
        rads, mDesc->xPortLRAD != nullptr ? &lrad : nullptr, mDesc->xPortRRAD != nullptr ? &rrad : nullptr,
        mDesc->xPortLLRD);
    bolt.leftRad[size_t(i)] = radii.left;
    bolt.rightRad[size_t(i)] = radii.right;
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    if (mDesc->xPortNCLR != nullptr) {
      const int life = bolt.life;
      CParticleGlobals::SetParticleLifetime(std::max(mNodes - 1, 1));
      mDesc->xPortNCLR->GetValueHdr(frame, color);
      CParticleGlobals::SetParticleLifetime(life);
    }
    for (int c = 0; c < 4; ++c) {
      bolt.color[size_t(i)][size_t(c)] = color[c] * colr[c];
    }
  }
  CParticleGlobals::PortClearSlot2Time();
  for (auto& row : bolt.vpmt) {
    std::fill(row, row + 4, 0.f);
  }
  if (mDesc->xPortVfx) {
    PortVfxEvalVpmt(*mDesc->xPortVfx, frame, bolt.vpmt);
  }
}

void CParticleElectric2Port::MakeBranches(const std::vector< V3 >& parent, float amp, float ampd, int depth,
                                          int maxDepth, const PortElectric2::BoltParams& prm,
                                          std::vector< std::vector< V3 > >& out, int frame) {
  // CalculatePointsForBoltBranches 0xb745b0. Index choice, length scale and rotation axis are
  // not confirmed in the decompile.
  const int parentCount = int(parent.size());
  if (parentCount < 3) {
    return;
  }
  int count = mDesc->xPortBNDI != nullptr ? IntOr(mDesc->xPortBNDI, frame, 0) : mRng.RangeI(4, 7);
  count = std::max(0, std::min(count, parentCount - 2));
  const float amdf = RealOr(mDesc->xPortAMDF, frame, 0.5f);
  for (int i = 0; i < count; ++i) {
    const int idx = mRng.RangeI(0, parentCount - 2);
    const float len = mDesc->xPortBLEN != nullptr ? RealOr(mDesc->xPortBLEN, frame, 0.f) : mRng.RangeF(3.f, 6.f);
    float ang = mDesc->xPortBANG != nullptr ? RealOr(mDesc->xPortBANG, frame, 0.f) : mRng.RangeF(15.f, 30.f);
    if (mRng.RangeI(0, 1) != 0) {
      ang = -ang;
    }
    V3 dir = parent[size_t(idx + 1)] - parent[size_t(idx)];
    if (!PortElectric2::IsNormalizable(dir)) {
      continue;
    }
    dir = PortElectric2::Normalized(dir);
    const V3 start = parent[size_t(idx)];
    const V3 end = start + PortElectric2::Rotate(dir * len, prm.camAxis, ang * 0.017453292f);
    std::vector< V3 > pts;
    PortElectric2::CalculatePointsForBolt(mRng, prm, start, end, V3{}, V3{}, pts, amp * amdf, ampd);
    out.push_back(pts);
    if (depth + 1 < maxDepth) {
      MakeBranches(pts, amp * amdf, ampd, depth + 1, maxDepth, prm, out, frame);
    }
  }
}

void CParticleElectric2Port::CreateBolt(int frame, const Ctx& ctx) {
  if (int(mBolts.size()) >= mMaxBolts) {
    return;
  }
  Bolt bolt;
  bolt.life = std::max(1, IntOr(mDesc->x4_SLIF, frame, 1));
  bolt.startFrame = frame;
  bolt.endFrame = frame + bolt.life;
  CParticleGlobals::SetParticleLifetime(bolt.life);
  CParticleGlobals::UpdateParticleLifetimeTweenValues(0);

  CVector3f start = CVector3f::Zero(), startDir = CVector3f::Zero();
  CVector3f end(0.f, 1.f, 0.f), endDir = CVector3f::Zero();
  if (mDesc->x18_IEMT != nullptr) {
    mDesc->x18_IEMT->GetValue(frame, start, startDir);
  }
  if (mDesc->x1c_FEMT != nullptr) {
    mDesc->x1c_FEMT->GetValue(frame, end, endDir);
  }
  start = ctx.spread * start;
  end = ctx.spread * end;
  if (ctx.ipos != nullptr) {
    start = *ctx.ipos;
  }
  if (ctx.ivel != nullptr) {
    startDir = *ctx.ivel;
  }
  if (ctx.fpos != nullptr) {
    end = *ctx.fpos;
  }
  if (ctx.fvel != nullptr) {
    endDir = *ctx.fvel;
  }

  PortElectric2::BoltParams prm;
  prm.nodes = mNodes;
  prm.slvf = RealOr(mDesc->xPortSLVF, frame, 1.f);
  prm.blnc = RealOr(mDesc->xPortBLNC, frame, 1.f);
  prm.bcff = RealOr(mDesc->xPortBCFF, frame, 1.f);
  prm.fplb = mDesc->xPortFPLB;
  prm.zery = mDesc->x70_ZERY;
  prm.camAxis = ToV3(ctx.xf.TransposeRotate(CGraphics::GetViewMatrix().GetUp()));
  if (!PortElectric2::IsNormalizable(prm.camAxis)) {
    prm.camAxis = V3{0.f, 0.f, 1.f};
  } else {
    prm.camAxis = PortElectric2::Normalized(prm.camAxis);
  }

  const float amp = 2.f * RealOr(mDesc->x20_AMPL, frame, 1.f);
  const float ampd = RealOr(mDesc->x24_AMPD, frame, 0.f);

  std::vector< std::vector< V3 > > lines(1);
  PortElectric2::CalculatePointsForBolt(mRng, prm, ToV3(start), ToV3(end), ToV3(startDir), ToV3(endDir), lines[0],
                                        amp, ampd);
  const int bdep = IntOr(mDesc->xPortBDEP, frame, 0);
  if (bdep > 0) {
    MakeBranches(lines[0], amp, ampd, 0, bdep, prm, lines, frame);
  }

  // Bake to world space (InitBoltNodePosition 0xb76e18): pos = xf * (pt * scale). XFMD 3 leaves
  // the points local and unscaled (the mode zeroes vec 0x12 and the local rotation) and the
  // render moves them with the live transform (WorldPoint).
  mXf = ctx.xf;
  mScale = CVector3f(ctx.globalScale.GetX() * ctx.localScale.GetX(), ctx.globalScale.GetY() * ctx.localScale.GetY(),
                     ctx.globalScale.GetZ() * ctx.localScale.GetZ());
  bolt.first = lines[0].front();
  bolt.last = lines[0].back();
  if (!mDesc->xPortXfmdFollow) {
    for (auto& line : lines) {
      for (V3& p : line) {
        p = WorldPoint(p);
      }
    }
  }
  bolt.lines = std::move(lines);
  UpdateBolt(bolt, frame);
  mBolts.push_back(std::move(bolt));
  ++mCreated;
}

void CParticleElectric2Port::Create(int frame, int count, const Ctx& ctx) {
  mCreated = 0;
  mRetired.clear();
  for (int i = 0; i < count; ++i) {
    CreateBolt(frame, ctx);
  }
}

void CParticleElectric2Port::Reset() {
  mBolts.clear();
  mRem = 0.f;
  mCreated = 0;
  mRetired.clear();
}

void CParticleElectric2Port::Step(int frame, bool emitting, const Ctx& ctx) {
  mCreated = 0;
  mRetired.clear();
  mModu = ctx.modu;
  mXf = ctx.xf;
  mScale = CVector3f(ctx.globalScale.GetX() * ctx.localScale.GetX(), ctx.globalScale.GetY() * ctx.localScale.GetY(),
                     ctx.globalScale.GetZ() * ctx.localScale.GetZ());
  // UpdateElectricalEffects 0xb757a4
  for (size_t i = 0; i < mBolts.size();) {
    Bolt& bolt = mBolts[i];
    if (frame >= bolt.endFrame) {
      if (bolt.gpsm != -1 || bolt.epsm != -1) {
        mRetired.push_back(Retired{bolt.gpsm, bolt.epsm});
      }
      mBolts.erase(mBolts.begin() + std::ptrdiff_t(i));
      continue;
    }
    CParticleGlobals::SetParticleLifetime(bolt.life);
    CParticleGlobals::UpdateParticleLifetimeTweenValues(frame - bolt.startFrame);
    UpdateBolt(bolt, frame);
    ++i;
  }
  if (!emitting) {
    return;
  }
  float rate = 0.f;
  if (mDesc->x8_GRAT != nullptr && mDesc->x8_GRAT->GetValue(frame, rate)) {
    for (const Bolt& bolt : mBolts) {
      if (bolt.gpsm != -1 || bolt.epsm != -1) {
        mRetired.push_back(Retired{bolt.gpsm, bolt.epsm});
      }
    }
    mBolts.clear(); // GetValue true = the element is exhausted
    return;
  }
  rate = std::max(rate, 0.f);
  const int n = int(rate + mRem);
  mRem = rate + mRem - float(n);
  Create(frame, n, ctx);
}

void CParticleElectric2Port::Render(int frame, const CTransform4f& view) {
  static const bool logElc2 = port::EnvFlag("MP_ELC2_LOG");
  if (logElc2 && frame % 30 == 0) {
    std::fprintf(stderr, "elc2: frame %d bolts %zu vfx %d\n", frame, mBolts.size(), mDesc->xPortVfx != nullptr);
  }
  if (mBolts.empty() || !mDesc->xPortVfx) {
    return;
  }
  const CPortVfxData& vfx = *mDesc->xPortVfx;
  if (vfx.mat.version != 2 && vfx.mat.version != 3) {
    return;
  }
  float vsmt[kPortVfxVsmtCount] = {};
  const uint vsmtMask = PortVfxEvalVsmt(vfx, frame, vsmt);
  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(vfx, vsmt, vsmtMask, desc)) {
    return; // a texture is not streamed in yet
  }
  CPortVfxUvXf uvXf;
  uvXf.Eval(vfx, frame);
  const float modu[4] = {mModu.GetRed(), mModu.GetGreen(), mModu.GetBlue(), mModu.GetAlpha()};
  const int tspn = IntOr(mDesc->xPortTSPN, frame, 0);
  const V3 cameraForward = ToV3(view.GetForward());
  const int nfom = IntOr(mDesc->xPortNFOM, frame, 0);
  float tone[3][4];
  const float exposure = PortRoomEnv::Tone(tone) ? PortRoomEnv::FrameExposure() : 1.f;
  // The AddVar's inverse exposure is undone by the tonemap's frame exposure, not by the GX tone's
  // inherited w (the last model's 1 or GlowScale).
  desc.exposureOverride = PortVfxFrameExposureOverride();

  std::vector< aurora::gfx::vfx::Vertex > verts;
  auto emit = [&](const V3& p, float u, float v, const float color[4], const float extra[4][4]) {
    aurora::gfx::vfx::Vertex out{};
    out.pos[0] = p.x;
    out.pos[1] = p.y;
    out.pos[2] = p.z;
    uvXf.Apply(u, v, out.uv);
    for (int c = 0; c < 4; ++c) {
      out.color[c] = color[c];
      for (int k = 0; k < 4; ++k) {
        out.extra[c][k] = extra[c][k];
      }
    }
    out.vec[2] = 1.f;
    verts.push_back(out);
  };

  for (const Bolt& bolt : mBolts) {
    std::vector< std::array< float, 4 > > color(bolt.color.size());
    for (size_t i = 0; i < color.size(); ++i) {
      const float iten = PortElectric2::ExposureCompensatedIntensity(
          i < bolt.iten.size() ? bolt.iten[i] : 1.f, exposure);
      for (int c = 0; c < 4; ++c) {
        color[i][size_t(c)] = bolt.color[i][size_t(c)] * (c < 3 ? iten : 1.f) * modu[c];
      }
    }
    for (const auto& localLine : bolt.lines) {
      std::vector<V3> moved;
      if (mDesc->xPortXfmdFollow) {
        moved.reserve(localLine.size());
        for (const V3& p : localLine) {
          moved.push_back(WorldPoint(p));
        }
      }
      const std::vector<V3>& line = mDesc->xPortXfmdFollow ? moved : localLine;
      const int n = int(line.size());
      if (n < 2) {
        continue;
      }
      // UpdateRenderData_Electric2 0x2b8694: u advances 1/TSPN per segment, or 1/nodes when TSPN is absent.
      const float segLen = tspn > 0 ? 1.f / float(tspn) : 1.f / float(n);
      std::vector<V3> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
      bool validFrame = true;
      for (int i = 0; i < n; ++i) {
        V3 width;
        if (!PortElectric2::Electric2RibbonWidthAxis(line, size_t(i), nfom, mSide, cameraForward, width)) {
          validFrame = false;
          break;
        }
        const size_t r = std::min(size_t(i), bolt.leftRad.size() - 1);
        PortElectric2::Electric2RibbonSidePoints(line[size_t(i)], width, bolt.leftRad[r], bolt.rightRad[r],
                                                 a[size_t(i)], b[size_t(i)]);
      }
      if (!validFrame) {
        continue;
      }
      for (int i = 0; i + 1 < n; ++i) {
        const float u0 = float(i) * segLen;
        const float u1 = float(i + 1) * segLen;
        const float* c0 = color[std::min(size_t(i), color.size() - 1)].data();
        const float* c1 = color[std::min(size_t(i + 1), color.size() - 1)].data();
        emit(a[size_t(i)], u0, 0.f, c0, bolt.vpmt);
        emit(b[size_t(i)], u0, 1.f, c0, bolt.vpmt);
        emit(b[size_t(i + 1)], u1, 1.f, c1, bolt.vpmt);
        emit(a[size_t(i + 1)], u1, 0.f, c1, bolt.vpmt);
      }
    }
  }
  constexpr size_t kMaxQuadsPerDraw = 16384;
  const size_t quads = verts.size() / 4;
  for (size_t first = 0; first < quads; first += kMaxQuadsPerDraw) {
    const uint32_t count = static_cast< uint32_t >(std::min(quads - first, kMaxQuadsPerDraw));
    PortFx::gQuads += count;
    ++PortFx::gDraws;
    aurora::gfx::vfx::draw_quads(desc, verts.data() + first * 4, count);
  }
}
