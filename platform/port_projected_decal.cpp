#include "port_projected_decal.h"
#include "port_log.h"
#include "port_room_geo.h"

#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CCubeSurface.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Weapons/CDecal.hpp"

#include <array>

namespace PortProjectedDecal {
namespace {
constexpr int kSlots = 64;

struct Slot {
  bool pending = false;
  bool built = false;
  uint32_t asset = 0;
  Command command;
  Mesh mesh;
};
std::array< Slot, kSlots > sSlots;
bool sAnyPending = false;

Vec3 ToVec(const CVector3f& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }

struct Counts {
  int receivers = 0;
  int tested = 0;
  int facing = 0;
  int clipped = 0;
};

struct BuildContext {
  Slot* slot;
  float cosLimit;
  Counts counts;
};

Vec3 WorldPosition(const CTransform4f& xf, const float* be) {
  const CVector3f p(CBasics::SwapBytes(be[0]), CBasics::SwapBytes(be[1]), CBasics::SwapBytes(be[2]));
  return ToVec(xf * p);
}

// CModelSceneNode::RenderDecalCustom 0x223190: every surface of the receiver through the GS, with
// the model's transform; the GS swaps v1 and v2 back when det < 0.
void AddReceiver(void* ctx, const CModel& model, const CTransform4f& xf) {
  BuildContext& b = *static_cast< BuildContext* >(ctx);
  const Command& c = b.slot->command;
  const CCubeModel& cube = *model.GetCubeModel();
  const float* positions = static_cast< const float* >(cube.GetPositions());
  const size_t positionCount = cube.GetModelInstance().GetVertexSize() / 12;
  const double det = double(xf.Get00()) * (double(xf.Get11()) * xf.Get22() - double(xf.Get12()) * xf.Get21()) -
                     double(xf.Get01()) * (double(xf.Get10()) * xf.Get22() - double(xf.Get12()) * xf.Get20()) +
                     double(xf.Get02()) * (double(xf.Get10()) * xf.Get21() - double(xf.Get11()) * xf.Get20());
  const bool mirrored = det < 0.0;
  ++b.counts.receivers;
  for (CCubeSurface s = cube.GetNormalSurfaces(); s.IsValid(); s = s.GetNextSurface()) {
    uint attrs = cube.GetMaterialByIndex(s.GetMaterialIndex()).GetVertexDesc();
    size_t stride = 0;
    for (int i = 0; i < 16; ++i) {
      if ((attrs >> (i * 2)) & 3) {
        stride += 2;
      }
    }
    uint base[CCubeSurface::kPB_Count] = {};
    if (!s.PortBaseIndices(base)) {
      base[CCubeSurface::kPB_Pos] = 0;
    }
    const uint posBase = base[CCubeSurface::kPB_Pos];
    ForEachTriangle(static_cast< const uint8_t* >(s.GetDisplayList()), s.GetDisplayListSize(), stride,
                    [&](uint32_t i0, uint32_t i1, uint32_t i2) {
                      i0 += posBase;
                      i1 += posBase;
                      i2 += posBase;
                      if (i0 >= positionCount || i1 >= positionCount || i2 >= positionCount) {
                        return;
                      }
                      const Vec3 v0 = WorldPosition(xf, positions + i0 * 3);
                      const Vec3 v1 = WorldPosition(xf, positions + i1 * 3);
                      const Vec3 v2 = WorldPosition(xf, positions + i2 * 3);
                      // Outside the box's bounds: no clip plane would keep any of it.
                      if (std::fmax(v0.x, std::fmax(v1.x, v2.x)) < c.lo.x ||
                          std::fmin(v0.x, std::fmin(v1.x, v2.x)) > c.hi.x ||
                          std::fmax(v0.y, std::fmax(v1.y, v2.y)) < c.lo.y ||
                          std::fmin(v0.y, std::fmin(v1.y, v2.y)) > c.hi.y ||
                          std::fmax(v0.z, std::fmax(v1.z, v2.z)) < c.lo.z ||
                          std::fmin(v0.z, std::fmin(v1.z, v2.z)) > c.hi.z) {
                        return;
                      }
                      ++b.counts.tested;
                      switch (AddTriangle(c, v0, v1, v2, mirrored, b.cosLimit, b.slot->mesh)) {
                      case Result::Facing:
                        ++b.counts.facing;
                        break;
                      case Result::Clipped:
                        ++b.counts.clipped;
                        break;
                      default:
                        break;
                      }
                    });
  }
}
} // namespace

void Spawn(int slot, const CDecal& decal, uint32_t asset) {
  if (slot < 0 || slot >= kSlots) {
    return;
  }
  Slot& s = sSlots[slot];
  // COBBox(the decal's transform +0x4c, extents +0x7c), as AddCollisionDecal 0xb3af5c builds it.
  const CTransform4f& xf = decal.GetPortTransform();
  const Box box{{ToVec(xf.GetColumn(kDX)), ToVec(xf.GetColumn(kDY)), ToVec(xf.GetColumn(kDZ))},
                ToVec(xf.GetTranslation()),
                {decal.GetPortWidth(), decal.GetPortHeight(), decal.GetPortBoxDepth()}};
  s.command = MakeShotCommand(box);
  s.mesh.vertices.clear();
  s.asset = asset;
  s.built = false;
  s.pending = true;
  sAnyPending = true;
}

void Release(int slot) {
  if (slot < 0 || slot >= kSlots) {
    return;
  }
  Slot& s = sSlots[slot];
  s.pending = false;
  s.built = false;
  s.mesh.vertices.clear();
  s.mesh.vertices.shrink_to_fit();
}

void ReleaseAll() {
  for (int i = 0; i < kSlots; ++i) {
    Release(i);
  }
  sAnyPending = false;
}

void Build() {
  if (!sAnyPending) {
    return;
  }
  sAnyPending = false;
  const float cosLimit = NormalLimitCos();
  for (int i = 0; i < kSlots; ++i) {
    Slot& s = sSlots[i];
    if (!s.pending) {
      continue;
    }
    s.pending = false;
    BuildContext ctx{&s, cosLimit, {}};
    const float lo[3] = {s.command.lo.x, s.command.lo.y, s.command.lo.z};
    const float hi[3] = {s.command.hi.x, s.command.hi.y, s.command.hi.z};
    PortRoomGeo::ForEachShadowReceiver(lo, hi, AddReceiver, &ctx);
    s.built = true;
    PortLog::Write("decal: %08X clipped mesh slot %d: %zu tris (%d receivers, %d tested, %d facing away, "
                   "%d cut)\n",
                   s.asset, i, s.mesh.Triangles(), ctx.counts.receivers, ctx.counts.tested, ctx.counts.facing,
                   ctx.counts.clipped);
  }
}

const Mesh* MeshFor(int slot) {
  if (slot < 0 || slot >= kSlots || !sSlots[slot].built) {
    return nullptr;
  }
  return &sSlots[slot].mesh;
}

} // namespace PortProjectedDecal
