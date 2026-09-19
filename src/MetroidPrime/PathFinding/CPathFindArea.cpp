#include "MetroidPrime/PathFinding/CPathFindArea.hpp"

#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"

#include "rstl/algorithm.hpp"

#include <dolphin/os.h>
#include <float.h>

class CVParamTransfer;

CPFAreaOctree::CPFAreaOctree(CInputStream& in)
: x0_isLeaf(in.ReadLong())
, x4_bounds(in)
, x1c_center(in) {
  for (int i = 0; i < 8; ++i) {
    x28_children[i] =
        reinterpret_cast< CPFAreaOctree* >(static_cast< uintptr_t >(in.ReadLong()));
  }
  x48_regions.set_size(in.ReadLong());
  x48_regions.set_data(
      reinterpret_cast< CPFRegion** >(static_cast< uintptr_t >(in.ReadLong())));
}

uint CPFAreaOctree::GetChildIndex(const CVector3f& point) const {
  uint index = 0;
  if (point[kDX] > x1c_center[kDX]) {
    index = 1;
  }
  if (point[kDY] > x1c_center[kDY]) {
    index |= 2;
  }
  if (point[kDZ] > x1c_center[kDZ]) {
    index |= 4;
  }
  return index;
}

prereserved_vector< CPFRegion* >* CPFAreaOctree::GetRegionList(const CVector3f& point) {
  if (x0_isLeaf) {
    return &x48_regions;
  }
  return x28_children[GetChildIndex(point)]->GetRegionList(point);
}

void CPFAreaOctree::GetRegionListList(
    rstl::reserved_vector< prereserved_vector< CPFRegion* >*, 32 >& lists, const CVector3f& point,
    float padding) {
  if (lists.size() >= lists.capacity()) {
    return;
  }
  if (x0_isLeaf) {
    lists.push_back(&x48_regions);
  } else {
    for (int i = 0; i < 8; ++i) {
      if (x28_children[i]->IsPointInsidePaddedAABox(point, padding)) {
        x28_children[i]->GetRegionListList(lists, point, padding);
      }
    }
  }
}

CPFArea::CPFArea(const rstl::auto_ptr< uchar >& data, int size)
: x0_bestPointDistSq(FLT_MAX)
, x4_closestPoint(CVector3f::Zero())
, x20_cachedRegionList(nullptr)
, x24_cachedRegionListPoint(CVector3f::Zero())
, x30_hasCachedRegionList(false)
, x34_regionFindCookie(0)
, x13c_data(data.release())
, x188_transform(CTransform4f::Identity()) {
  CMemoryInStream stream(x13c_data.get(), size);
  stream.ReadLong();

  int numNodes = stream.ReadLong();
  x140_nodes.reserve(numNodes);
  for (int i = 0; i < numNodes; ++i) {
    x140_nodes.push_back(CPFNode(stream));
  }
  int numLinks = stream.ReadLong();
  x148_links.reserve(numLinks);
  for (int i = 0; i < numLinks; ++i) {
    x148_links.push_back(CPFLink(stream));
  }
  const int numRegions = stream.ReadLong();
  x150_regions.reserve(numRegions);
  for (int i = 0; i < numRegions; ++i) {
    x150_regions.push_back(CPFRegion(stream));
  }
  x178_regionData.reserve(numRegions);
  CPFRegionData dataValue = CPFRegionData();
  x178_regionData.resize(numRegions, dataValue);
  int maxRegionNodes = 0;
  int i;
  for (i = 0; i < numRegions; ++i) {
    x150_regions[i].Fixup(*this, maxRegionNodes);
  }
  maxRegionNodes = maxRegionNodes > 4 ? maxRegionNodes : 4;
  x10_polyPoints.reserve(maxRegionNodes);

  int numWords = (numRegions * (numRegions - 1) / 2 + 31) / 32;
  x168_connectionsGround.reserve(numWords);
  x170_connectionsFlyers.reserve(numWords);
  for (i = 0; i < numWords; ++i) {
    x168_connectionsGround.push_back(stream.ReadLong());
  }
  for (i = 0; i < numWords; ++i) {
    x170_connectionsFlyers.push_back(stream.ReadLong());
  }
  const int paddingWords = ((numRegions * numRegions + 31) / 32 - numWords) * 2;
  for (i = 0; i < paddingWords; ++i) {
    stream.ReadLong();
  }

  int numRegionPtrs = stream.ReadLong();
  x160_octreeRegions.reserve(numRegionPtrs);
  for (i = 0; i < numRegionPtrs; ++i) {
    x160_octreeRegions.push_back(
        reinterpret_cast< CPFRegion* >(static_cast< uintptr_t >(stream.ReadLong())));
  }
  for (i = 0; i < numRegionPtrs; ++i) {
    x160_octreeRegions[i] =
        &x150_regions[reinterpret_cast< uintptr_t >(x160_octreeRegions[i])];
  }
  int numOctreeNodes = stream.ReadLong();
  x158_octree.reserve(numOctreeNodes);
  for (i = 0; i < numOctreeNodes; ++i) {
    x158_octree.push_back(CPFAreaOctree(stream));
  }
  for (i = 0; i < numOctreeNodes; ++i) {
    x158_octree[i].Fixup(*this);
  }
}

prereserved_vector< CPFRegion* >* CPFArea::GetOctreeRegionList(const CVector3f& point) {
  if (x30_hasCachedRegionList && close_enough(point, x24_cachedRegionListPoint)) {
    return x20_cachedRegionList;
  }
  return x158_octree.back().GetRegionList(point);
}

int CPFArea::FindRegions(rstl::reserved_vector< CPFRegion*, 4 >& regions, const CVector3f& point,
                         uint flags, uint indexMask) {
  prereserved_vector< CPFRegion* >* list = GetOctreeRegionList(point);
  for (int i = 0; i < list->size(); ++i) {
    CPFRegion* region = (*list)[i];
    if ((region->GetFlags() & 0xff & flags) && ((region->GetFlags() >> 16) & 0xff & indexMask) &&
        region->IsPointInside(point) &&
        ((flags & 2) || (flags & 4) || region->PointHeight(point) < 3.f)) {
      regions.push_back(region);
      if (regions.size() == regions.capacity()) {
        break;
      }
    }
  }
  return regions.size();
}

CPFRegion* CPFArea::FindClosestRegion(const CVector3f& point, uint flags, uint indexMask,
                                      float padding) {
  rstl::reserved_vector< prereserved_vector< CPFRegion* >*, 32 > lists;
  CPFRegion* result = nullptr;
  OSGetTick();
  int i, j;
  uint searchTicks = 0;
  x158_octree.back().GetRegionListList(lists, point, padding);
  OSGetTick();
  for (i = 0; i < lists.size(); ++i) {
    prereserved_vector< CPFRegion* >* list = lists[i];
    for (j = 0; j < list->size(); ++j) {
      CPFRegion* region = (*list)[j];
      if (region->Data()->GetCookie() != x34_regionFindCookie) {
        if ((region->GetFlags() & 0xff & flags) &&
            ((region->GetFlags() >> 16) & 0xff & indexMask) &&
            region->IsPointInsidePaddedAABox(point, padding)) {
          uint startTick = OSGetTick();
          if ((flags & 2) || region->PointHeight(point) < 3.f) {
            if (region->FindBestPoint(x10_polyPoints, point, flags, padding * padding)) {
              padding = CMath::FastSqrtF(region->Data()->GetBestDistanceSquared());
              result = region;
              x4_closestPoint = region->Data()->GetBestPoint();
            }
            searchTicks += OSGetTick() - startTick;
          }
        }
        region->Data()->SetCookie(x34_regionFindCookie);
      }
    }
  }
  OSGetTick();
  ++x34_regionFindCookie;
  return result;
}

CVector3f CPFArea::FindClosestReachablePoint(rstl::reserved_vector< CPFRegion*, 4 >& regions,
                                             const CVector3f& point, uint flags, uint indexMask) {
  CVector3f result = CVector3f::Zero();
  float closestDistanceSq = FLT_MAX;
  for (int i = 0; i < GetNumRegions(); ++i) {
    CPFRegion& region = GetRegion(i);
    if ((region.GetFlags() & 0xff & flags) && ((region.GetFlags() >> 16) & 0xff & indexMask)) {
      for (int j = 0; j < regions.size(); ++j) {
        CPFRegion* source = regions[j];
        if (PathExists(source, &region, flags)) {
          const CVector3f& delta = region.GetCentroid() - point;
          float distanceSq = delta.MagSquared();
          if (distanceSq < closestDistanceSq) {
            closestDistanceSq = distanceSq;
            result = region.GetCentroid();
            break;
          }
        }
      }
    }
  }
  return result;
}

bool CPFArea::PathExists(const CPFRegion* source, const CPFRegion* destination, uint flags) const {
  if (source == destination || (flags & 4)) {
    return true;
  }
  int numRegions = GetNumRegions();
  int sourceIndex = source->GetIndex();
  int destinationIndex = destination->GetIndex();
  if (sourceIndex > destinationIndex) {
    rstl::swap(sourceIndex, destinationIndex);
  }
  int totalConnections = numRegions * (numRegions - 1) / 2;
  int remainingConnections = (numRegions - sourceIndex - 1) * (numRegions - sourceIndex) / 2;
  uint bit = totalConnections - remainingConnections + destinationIndex - (sourceIndex + 1);
  if (flags & 2) {
    return (x170_connectionsFlyers[bit / 32] >> (bit % 32)) & 1;
  }
  return (x168_connectionsGround[bit / 32] >> (bit % 32)) & 1;
}

const CFactoryFnReturn FPathFindAreaFactory(const SObjectTag& tag, const rstl::auto_ptr< uchar >& data,
                                      int size, const CVParamTransfer& xfer) {
  return rs_new CPFArea(data, size);
}
