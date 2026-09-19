#include "Kyoto/Graphics/CCubeSurface.hpp"
#include "Kyoto/Basics/CBasics.hpp"

const CVector3f CCubeSurface::skDefaultNormal(1.f, 0.f, 0.f);

void CCubeSurface::ConvertSurfaceHeader(void* rawData) {
#if TARGET_LITTLE_ENDIAN
  uchar* data = static_cast< uchar* >(rawData);
  float center[3];
  float normal[3];
  for (int i = 0; i < 3; ++i) {
    center[i] = CBasics::SwapBytes(*reinterpret_cast< const float* >(data + i * 4));
    normal[i] = CBasics::SwapBytes(*reinterpret_cast< const float* >(data + 0x20 + i * 4));
  }

  const uint materialIndex = CBasics::SwapBytes(*reinterpret_cast< const uint* >(data + 0xc));
  const uint displayListSize = CBasics::SwapBytes(*reinterpret_cast< const uint* >(data + 0x10));
  const uint extraSize = CBasics::SwapBytes(*reinterpret_cast< const uint* >(data + 0x1c));
  float bounds[6];
  if (extraSize != 0) {
    for (int i = 0; i < 6; ++i) {
      bounds[i] = CBasics::SwapBytes(*reinterpret_cast< const float* >(data + 0x2c + i * 4));
    }
  }

  SSurfaceData* surface = static_cast< SSurfaceData* >(rawData);
  memcpy(&surface->mCenter, center, sizeof(center));
  surface->mMaterialIndex = materialIndex;
  surface->mDisplayListSizeAndNormalHint = displayListSize;
  surface->mExtraSize = extraSize;
  memcpy(&surface->mNormal, normal, sizeof(normal));
  if (extraSize != 0) {
    memcpy(&surface->mBounds, bounds, sizeof(bounds));
  }
#endif
}

CAABox CCubeSurface::GetBounds() const {
  if (x0_data->mExtraSize != 0) {
    return x0_data->mBounds;
  }

  return CAABox(x0_data->mCenter, x0_data->mCenter);
}
