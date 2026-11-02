#ifndef _CVECTORELEMENT
#define _CVECTORELEMENT

#include "Kyoto/Streams/CInputStream.hpp"
#include "types.h"

#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/IElement.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/PortParticleMath.hpp"
#endif

#include "rstl/single_ptr.hpp"

class CVEFastConstant : public CVectorElement {
public:
  CVEFastConstant(float x, float y, float z);
  ~CVEFastConstant() override;
  bool GetValue(int frame, CVector3f& valOut) const override;
  bool IsFastConstant() const override { return true; }

private:
  CVector3f x4_val;
};

class CVEParticleLocation : public CVectorElement {
public:
  ~CVEParticleLocation() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEParticlePreviousLocation : public CVectorElement {
public:
  ~CVEParticlePreviousLocation() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEParticleVelocity : public CVectorElement {
public:
  ~CVEParticleVelocity() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEParticleSystemOrientationFront : public CVectorElement {
public:
  ~CVEParticleSystemOrientationFront() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEParticleSystemOrientationUp : public CVectorElement {
public:
  ~CVEParticleSystemOrientationUp() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEParticleSystemOrientationRight : public CVectorElement {
public:
  ~CVEParticleSystemOrientationRight() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEParticleSystemTranslation : public CVectorElement {
public:
  ~CVEParticleSystemTranslation() override {}
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEConstant : public CVectorElement {
public:
  CVEConstant(CRealElement* x, CRealElement* y, CRealElement* z);
  ~CVEConstant();
  bool GetValue(int frame, CVector3f& valOut) const override;

public:
  CRealElement* x4_x;
  CRealElement* x8_y;
  CRealElement* xc_z;
};

class CVECircleCluster : public CVectorElement {
public:
  CVECircleCluster(CVectorElement* circleOffset, CVectorElement* circleNormal,
                   CIntElement* cycleFrames, CRealElement* randomFactor);
  ~CVECircleCluster() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mCircleOffset;
  CVector3f mXVec;
  CVector3f mYVec;
  float mRadius;
  CRealElement* mRandomFactor;
};

class CVECone : public CVectorElement {
  CVectorElement* x4_direction;
  CRealElement* x8_magnitude;
  CVector3f xc_xVec;
  CVector3f x18_yVec;

public:
  CVECone(CVectorElement* direction, CRealElement* magnitude);
  ~CVECone();
  bool GetValue(int frame, CVector3f& valOut) const override;
};

class CVEAngleCone : public CVectorElement {
public:
  CVEAngleCone(CRealElement* angleXConstant, CRealElement* angleYConstant,
               CRealElement* angleXRange, CRealElement* angleYRange, CRealElement* magnitude);
  ~CVEAngleCone();
  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CRealElement* x4_angleXConstant;
  CRealElement* x8_angleYConstant;
  CRealElement* xc_angleXRange;
  CRealElement* x10_angleYRange;
  CRealElement* x14_magnitude;
};

class CVECircle : public CVectorElement {
public:
  CVECircle(CVectorElement* circleOffset, CVectorElement* circleNormal, CRealElement* angleConstant,
            CRealElement* angleLinear, CRealElement* radius);
  ~CVECircle() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mCircleOffset;
  CVector3f mXVec;
  CVector3f mYVec;
  CRealElement* mAngleConstant;
  CRealElement* mAngleLinear;
  CRealElement* mRadius;
};

class CVEKeyframeEmitter : public CVectorElement {
public:
  CVEKeyframeEmitter(CInputStream& in);
  ~CVEKeyframeEmitter() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  int mPercent;
  int mUnk1;
  bool mLoop;
  bool mUnk2;
  int mLoopEnd;
  int mLoopStart;
  rstl::vector< CVector3f > mKeys;
};

class CVEAdd : public CVectorElement {
public:
  CVEAdd(CVectorElement* a, CVectorElement* b);
  ~CVEAdd() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mA;
  CVectorElement* mB;
};

class CVEMultiply : public CVectorElement {
public:
  CVEMultiply(CVectorElement* a, CVectorElement* b);
  ~CVEMultiply() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mA;
  CVectorElement* mB;
};

class CVETimeChain : public CVectorElement {
public:
  CVETimeChain(CVectorElement* a, CVectorElement* b, CIntElement* mSwitchFrame);
  ~CVETimeChain() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mA;
  CVectorElement* mB;
  CIntElement* mSwitchFrame;
};

class CVEPulse : public CVectorElement {
public:
  CVEPulse(CIntElement* durationA, CIntElement* durationB, CVectorElement* a, CVectorElement* b);
  ~CVEPulse() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CIntElement* mDurationA;
  CIntElement* mDurationB;
  CVectorElement* mA;
  CVectorElement* mB;
};
class CVERealToVector : public CVectorElement {
public:
  CVERealToVector(CRealElement* value);
  ~CVERealToVector() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CRealElement* mValue;
};

class CVESubtract : public CVectorElement {
public:
  CVESubtract(CVectorElement* a, CVectorElement* b);
  ~CVESubtract() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mA;
  CVectorElement* mB;
};

class CVEColorToVector : public CVectorElement {
public:
  CVEColorToVector(CColorElement* value);
  ~CVEColorToVector() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CColorElement* mValue;
};

#ifdef TARGET_PC
// Composes two Euler rotations in source order and returns an equivalent
// Z-Y-X Euler vector in degrees. Used by the Remastered PMRQ RADD carrier.
class CVEOrderedEulerProduct : public CVectorElement {
public:
  CVEOrderedEulerProduct(CVectorElement* first, CVectorElement* second);
  ~CVEOrderedEulerProduct() override;

  bool GetValue(int frame, CVector3f& valOut) const override;
  bool IsFastConstant() const override;

private:
  CVectorElement* mFirst;
  CVectorElement* mSecond;
};

// Remastered RNDV (CVERandomVector::GetValue 0x29e954): a cube point from the
// current RNG scaled to the magnitude (1 when absent) by an ARM rsqrt estimate.
class CVEPortRandomVector : public CVectorElement {
public:
  explicit CVEPortRandomVector(CRealElement* magnitude);
  ~CVEPortRandomVector() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CRealElement* mMagnitude;
};

// Remastered ANCR (CVEAngleCone::GetValue 0x29eebc) with its REUL rotation (axis order 0)
// given as the three angles: a direction in Remastered's space.
class CVEPortAngleCone : public CVectorElement {
public:
  CVEPortAngleCone(CRealElement* rotateX, CRealElement* rotateY, CRealElement* rotateZ,
                   CRealElement* xRange, CRealElement* yRange, CRealElement* magnitude);
  ~CVEPortAngleCone() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CRealElement* mRotate[3];
  CRealElement* mXRange;
  CRealElement* mYRange;
  CRealElement* mMagnitude;
};

// Remastered MPAC (0x29fec0): the angle cone of a centre and a spread per axis, five reals
// (x centre, y centre, x range, y range, magnitude), drawing u for x and v for y.
class CVEPortMpac : public CVectorElement {
public:
  CVEPortMpac(CRealElement* xCentre, CRealElement* yCentre, CRealElement* xRange, CRealElement* yRange,
              CRealElement* magnitude);
  ~CVEPortMpac() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CRealElement* mXCentre;
  CRealElement* mYCentre;
  CRealElement* mXRange;
  CRealElement* mYRange;
  CRealElement* mMagnitude;
};

// Remastered MPCN (MPT_CVECone::GetValue 0x29eca0): a point of the disc of the given radius
// (at most 1) around the position, in the plane perpendicular to the direction, whose frame is
// cached on the first call at frame 0 (zero until then).
class CVEPortCone : public CVectorElement {
public:
  CVEPortCone(CVectorElement* direction, CRealElement* radius);
  ~CVEPortCone() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mDirection;
  CRealElement* mRadius;
  mutable bool mCached = false;
  mutable CVector3f mU;
  mutable CVector3f mV;
};

// Remastered ANCM (CVEAngleConeMinMax::GetValue 0x29f304) with its REUL rotation (axis order 0)
// given as the three angles: a direction between two cone angles in Remastered's space.
class CVEPortAngleConeMinMax : public CVectorElement {
public:
  CVEPortAngleConeMinMax(CRealElement* rotateX, CRealElement* rotateY, CRealElement* rotateZ, CRealElement* minAngle,
                         CRealElement* maxAngle, CRealElement* magnitude);
  ~CVEPortAngleConeMinMax() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CRealElement* mRotate[3];
  CRealElement* mMinAngle;
  CRealElement* mMaxAngle;
  CRealElement* mMagnitude;
};

// Remastered CRCV (CVECircleWithVariance::GetValue 0x2a0a7c), in Remastered's numbers: a point of
// the circle around the centre in the plane of the axis (constant, read at frame 0), the ring
// divided into n steps visited in creation order or, with a shuffler of n cells, a random one.
class CVEPortCircle : public CVectorElement {
public:
  CVEPortCircle(CVectorElement* centre, CVectorElement* axis, CRealElement* startDegrees, CIntElement* count,
                CRealElement* variance, CRealElement* radius, u8 shuffle);
  ~CVEPortCircle() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mCentre;
  CVectorElement* mAxis;
  CRealElement* mStartDegrees;
  CIntElement* mCount;
  CRealElement* mVariance;
  CRealElement* mRadius;
  u8 mShuffle; // the size of Remastered's index shuffler, 0 for none
  mutable PortParticleMath::IndexShuffler mFallbackShuffler;
};

// Remastered LINV (CVELineWithVariance::GetValue 0x2a119c), in Remastered's numbers: a point of the
// segment start..end divided into n steps, visited in creation order or, with a shuffler of n cells, a
// random one, jittered by one draw. A segment shorter than 1e-5 gives the start and takes no draw.
class CVEPortLine : public CVectorElement {
public:
  CVEPortLine(CVectorElement* start, CVectorElement* end, CIntElement* count, CRealElement* variance, u8 shuffle);
  ~CVEPortLine() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mStart;
  CVectorElement* mEnd;
  CIntElement* mCount;
  CRealElement* mVariance;
  u8 mShuffle; // the size of Remastered's index shuffler, 0 for none
  mutable PortParticleMath::IndexShuffler mFallbackShuffler;
};

// Remastered NORM (CVENormalizeVector::GetValue 0x29e620): the child scaled to unit length,
// unchanged when its length is at most 2^-23.
class CVEPortNormalize : public CVectorElement {
public:
  explicit CVEPortNormalize(CVectorElement* a);
  ~CVEPortNormalize() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mA;
};

// Port-only change of basis between Remastered's particle space (X left, Y up, Z forward) and
// retail's (Z up): BSIS takes a direction or position, (x, y, z) to (-x, z, y), as
// MPT_CVEConvertToXLeftYUpZForward 0x2a3020; BSSC a scale, (x, y, z) to (x, z, y), as
// MPT_CVEConvertScaleToXLeftYUpZForward 0x2a30f8. Each is its own inverse.
class CVEPortBasis : public CVectorElement {
public:
  CVEPortBasis(CVectorElement* a, bool scale);
  ~CVEPortBasis() override;

  bool GetValue(int frame, CVector3f& valOut) const override;
  bool IsFastConstant() const override;

private:
  CVectorElement* mA;
  bool mScale;
};

// Port-only PRZY: Remastered's rotation RAZY(first, second, fallback) (CERotationFromAxis 0x29d298,
// PortParticleMath::RotationFromAxisZY) as the Euler degrees PQAD and REUL use, all in
// Remastered's particle space; the fallback is Euler degrees too. PRYZ is RAYZ the same way
// (PortParticleMath::RotationFromAxisYZ).
class CVEPortRotationFromAxis : public CVectorElement {
public:
  CVEPortRotationFromAxis(CVectorElement* first, CVectorElement* second, CVectorElement* fallback,
                          bool firstIsY = false);
  ~CVEPortRotationFromAxis() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mFirst;
  CVectorElement* mSecond;
  CVectorElement* mFallback;
  bool mFirstIsY;
};

// Port-only PEMV: Remastered's EMRV, the generator's per-tick change of its translation over its
// scale (CElementGen::PortStepPemv), in the port's axes. Zero outside a generator.
class CVEPortEmitterVelocity : public CVectorElement {
public:
  bool GetValue(int frame, CVector3f& valOut) const override;
};

// Remastered system vectors of the generator being evaluated: CMPS (the camera position in world
// space, CVECameraPositionInWorldSpace 0x2a23ec), CPSS (the camera position in the system's global
// frame, CVECameraPositionInSystemSpace 0x2a24fc), EXTT and EXTS (the external translation and
// scale, CVEExternalValue 0x2a2774), and CMRT (the camera's rotation as EXTR's Euler vector). Zero outside a generator evaluation (swooshes).
class CVEPortSystemVector : public CVectorElement {
public:
  explicit CVEPortSystemVector(FourCC kind) : mKind(kind) {}

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  FourCC mKind;
};

// Port-only PRTV: Remastered's ROTV (CVERotateVector::GetValue 0x2a0328) outside a component
// tree: the rotation first, then the vector, then CQuaternion::Transform, all in Remastered's
// particle space. The rotation is EXTR (the system orientation), CMRT (the camera's rotation,
// CERotationFromCamera 0x29d180) or REUL(x, y, z) in degrees, order 0.
class CVEPortRotateVector : public CVectorElement {
public:
  CVEPortRotateVector(CVectorElement* vector, FourCC rotation, CRealElement* x, CRealElement* y,
                      CRealElement* z);
  ~CVEPortRotateVector() override;

  bool GetValue(int frame, CVector3f& valOut) const override;

private:
  CVectorElement* mVector;
  FourCC mRotation;
  CRealElement* mAngles[3];
};

// Port-only PNCV: Remastered's PNCV (0x2a2800), the particle's velocity normalised by one
// refined FRSQRTE step (PortParticleMath::NormalizedVelocity), in Remastered's particle space.
class CVEPortNormalizedVelocity : public CVectorElement {
public:
  bool GetValue(int frame, CVector3f& valOut) const override;
};
#endif
#endif // _CVECTORELEMENT
