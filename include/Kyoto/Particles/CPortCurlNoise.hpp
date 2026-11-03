#ifndef _CPORTCURLNOISE
#define _CPORTCURLNOISE

#ifdef TARGET_PC
#include <vector>

#include "types.h"

#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/IElement.hpp"

class CInputStream;
class CVParamTransfer;
struct SObjectTag;

// Port-only curl noise for Remastered's CVECurlNoise_Perlin3d / CMVECurlNoise (CRLN).
namespace PortCurlNoise {
// FUN_000675c0: Ken Perlin's reference noise with a power-of-two period. The fraction is
// x - (float)(int)x, so a negative input has a negative fraction, as in Remastered.
float PerlinNoise(float x, float y, float z, int period);
// 0x68700: (dy - dz, dz - dx, dx - dy) of central differences of the noise.
CVector3f CurlNoise3d(float x, float y, float z, float epsilon, int period);
// CurlNoise2dSliceXY/XZ/YZ 0x684c0/0x68580/0x68640 (slice 1/2/3): the noise on the plane through
// the origin, the left-out coordinate 0, a and b its two axes in order. Returns
// (db, -da) of central differences.
void CurlNoise2dSlice(int slice, float a, float b, float epsilon, int period, float& outA,
                      float& outB);
// 0x28708c, CParticleMath::CurlNoise_PerlinOctave3d: the curl of the octave sum
// S = sum_k gain^k * noise(p * freq * 2^k), period 256. The step is added to a coordinate before
// the frequency multiplies it. octaves < 1 gives zero.
CVector3f CurlNoiseOctave3d(int octaves, float x, float y, float z, float freq, float gain,
                            float epsilon);
} // namespace PortCurlNoise

// CVectorField 0x2a72e0 / ComputeFieldVector 0x2a7390, loaded from the port VECF asset: a
// periodic grid of vectors, laid out y * (X * Z) + z * X + x.
class CVectorField {
public:
  CVectorField() = default;
  CVectorField(int dimX, int dimY, int dimZ, const CVector3f& min, const CVector3f& max,
               std::vector< CVector3f > cells);

  // Fails (returns false, leaves *this empty) on a wrong magic, version or cell count.
  bool Parse(CInputStream& in);
  CVector3f Compute(const CVector3f& p) const;
  bool IsEmpty() const { return x1c_cells.empty(); }

private:
  int x0_dim[3] = {0, 0, 0};
  CVector3f xc_min = CVector3f::Zero();
  CVector3f x18_max = CVector3f::Zero();
  std::vector< CVector3f > x1c_cells;
};

// Factory for the port 'VECF' resource.
const CFactoryFnReturn FVectorFieldFactory(const SObjectTag& tag, CInputStream& in,
                                           const CVParamTransfer& params);

// PCRN: the vector element CVECurlNoise_Perlin3d (slice 0, no octaves). Reads P, F, A, then
// the field token (NONE or CNST + asset id). With a field the noise and F are skipped.
class CVEPortCurlNoise : public CVectorElement {
public:
  struct FieldHandle;
  // handle may be null (no field); the element owns it.
  CVEPortCurlNoise(CVectorElement* p, CRealElement* f, CVectorElement* a, FieldHandle* handle);
  ~CVEPortCurlNoise() override;

  bool GetValue(int frame, CVector3f& valOut) const override;
  // For tests: use this field instead of resolving the handle.
  void SetField(const CVectorField* field) { mField = field; }
  static FieldHandle* MakeHandle(uint assetId);

private:
  const CVectorField* ResolveField() const;

  CVectorElement* mP;
  CRealElement* mF;
  CVectorElement* mA;
  FieldHandle* mHandle;
  mutable const CVectorField* mField = nullptr;
};

// PCRO: the vector element CVECurlNoise_PerlinOctave3d (slice 0, 0x2a33c0). Reads P, the
// frequency, the octave count (int), the gain and the amplitude A; a missing frequency or gain is
// 1 and a missing octave count is 1. No field.
class CVEPortCurlNoiseOctave : public CVectorElement {
public:
  CVEPortCurlNoiseOctave(CVectorElement* p, CRealElement* freq, CIntElement* octaves,
                         CRealElement* gain, CVectorElement* a);
  ~CVEPortCurlNoiseOctave() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mP;
  CRealElement* mFreq;
  CIntElement* mOctaves;
  CRealElement* mGain;
  CVectorElement* mA;
};

// PCRS: the vector element CVECurlNoise_Perlin2dSliceXY/XZ/YZ (0x2a35d8/0x2a3988/0x2a3d38).
// Reads the slice (raw u32: 1 XY, 2 XZ, 3 YZ), P, F, A. The two slice axes of P*F feed the noise;
// the result's slice axes are scaled by A's, the third is 0. No field.
class CVEPortCurlNoiseSlice : public CVectorElement {
public:
  CVEPortCurlNoiseSlice(uint slice, CVectorElement* p, CRealElement* f, CVectorElement* a);
  ~CVEPortCurlNoiseSlice() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  uint mSlice;
  CVectorElement* mP;
  CRealElement* mF;
  CVectorElement* mA;
};

// PCRM: the mod-vector element CMVECurlNoise. Reads mode (raw u32), V, t. Mode 1 blends the
// particle velocity into the position, mode 0 adds V; both finalize the position only in mode 1.
class CMVEPortCurlNoise : public CModVectorElement {
public:
  CMVEPortCurlNoise(uint mode, CVectorElement* v, CRealElement* t);
  ~CMVEPortCurlNoise() override;

  bool GetValue(int frame, CVector3f& pVel, CVector3f& pPos) const override;
  bool PortFinalizesPosition() const override { return mMode == 1; }

private:
  uint mMode;
  CVectorElement* mV;
  CRealElement* mT;
};
#endif // TARGET_PC

#endif // _CPORTCURLNOISE
