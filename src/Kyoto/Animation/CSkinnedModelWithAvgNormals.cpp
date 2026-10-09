#include "rstl/list.hpp"

#include "Kyoto/Animation/CSkinnedModel.hpp"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CVector3f.hpp"

#include "rstl/pair.hpp"
#include "rstl/vector.hpp"

#ifdef TARGET_PC
#include "Kyoto/Basics/CBasics.hpp"

#include <rstl/math.hpp>

#include <vector>

// The model's vertex arrays stay big-endian on PC (the skinning loads swap them).
// Entries past the array's `bytes` read as zero.
static std::vector< CVector3f > NativeVectors(const float* in, uint bytes, uint count,
                                              uint stride) {
  std::vector< CVector3f > out(count, CVector3f(0.f, 0.f, 0.f));
  const uint avail = rstl::min_val(count, bytes / (stride * 12));
  for (uint i = 0; i < avail; ++i) {
    const float* v = in + i * stride * 3;
    out[i] = CVector3f(CBasics::SwapBytes(v[0]), CBasics::SwapBytes(v[1]), CBasics::SwapBytes(v[2]));
  }
  return out;
}
#endif

typedef rstl::pair< CVector3f, rstl::list< uint > > TPosToVertListPair;

CSkinnedModelWithAvgNormals::CSkinnedModelWithAvgNormals(const CSkinnedModel& skinnedModel)
: x0_skinnedModel(skinnedModel), x3c_avgNormals(rs_new float[skinnedModel.GetNumPoints() * 12]) {
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  const uint vertexCount = skinnedModel.GetNumPoints();
#else
  int vertexCount = skinnedModel.GetNumPoints();
#endif
#ifdef TARGET_PC
  const std::vector< CVector3f > nativePositions =
      NativeVectors(skinnedModel.GetModel()->GetPositions(),
                    skinnedModel.GetModel()->GetCubeModel()->GetModelInstance().GetVertexSize(),
                    vertexCount, 1);
  const CVector3f* modelPositions = nativePositions.data();
#else
  const CVector3f* modelPositions =
      reinterpret_cast< const CVector3f* >(skinnedModel.GetModel()->GetPositions());
#endif

  rstl::vector< TPosToVertListPair > vertMap;
  vertMap.reserve(vertexCount);

  for (uint vertIdx = 0; vertIdx < vertexCount; ++vertIdx) {
    bool foundEqPos = false;
    uint vopolSize = vertMap.size();
    for (uint i = 0; i < vopolSize; ++i) {
      if (vertMap[i].first.IsEqu(modelPositions[vertIdx])) {
        foundEqPos = true;
        break;
      }
    }

    if (!foundEqPos) {
      rstl::list< uint > tmpList;
      for (uint j = vertIdx; j < vertexCount; ++j) {
        if (modelPositions[j] == modelPositions[vertIdx]) {
          tmpList.push_back(j);
        }
      }
      vertMap.push_back(TPosToVertListPair(modelPositions[vertIdx], tmpList));
    }
  }

#ifdef TARGET_PC
  // NBT normals are nine (N, B, T) or fifteen floats per vertex; N is the first.
  const std::vector< CVector3f > nativeNormals =
      NativeVectors(skinnedModel.GetModel()->GetNormals(),
                    skinnedModel.GetModel()->GetCubeModel()->GetModelInstance().GetNormalSize(),
                    vertexCount,
                    skinnedModel.GetModel()->GetCubeModel()->NormalVecs());
  const CVector3f* normals = nativeNormals.data();
  const uint normalStride = 1;
#else
  const CVector3f* normals =
      reinterpret_cast< const CVector3f* >(skinnedModel.GetModel()->GetNormals());
  const uint normalStride = 1;
#endif
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CVector3f* avgNormals = reinterpret_cast< CVector3f* >(x3c_avgNormals.get());
  AUTO(mapCur, vertMap.begin());
  AUTO(mapEnd, vertMap.end());
#else
  float* avgNormals = x3c_avgNormals.get();
  TPosToVertListPair* mapCur = vertMap.xc_items;
  TPosToVertListPair* mapEnd = mapCur + vertMap.x4_count;
#endif
  for (; mapCur != mapEnd; ++mapCur) {
    CVector3f accum(0.f, 0.f, 0.f);

    AUTO(lit, mapCur->second.begin());
    AUTO(listEnd, mapCur->second.end());
    for (; lit != listEnd; ++lit) {
      accum += normals[*lit * normalStride];
    }

    lit = mapCur->second.begin();
    CVector3f normalized = accum.AsNormalized();
    for (; lit != listEnd; ++lit) {
      reinterpret_cast< CVector3f* >(avgNormals)[*lit] = normalized;
    }
  }
}
