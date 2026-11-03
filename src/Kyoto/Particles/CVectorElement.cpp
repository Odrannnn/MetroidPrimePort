#include "Kyoto/Particles/CVectorElement.hpp"

#include <algorithm>
#include <cmath>

#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "rstl/math.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/PortParticleMath.hpp"
#endif

CVEConstant::CVEConstant(CRealElement* x, CRealElement* y, CRealElement* z)
: x4_x(x), x8_y(y), xc_z(z) {}

CVEConstant::~CVEConstant() {
  delete x4_x;
  delete x8_y;
  delete xc_z;
}

bool CVEConstant::GetValue(int frame, CVector3f& valOut) const {
  float x, y, z;
  x4_x->GetValue(frame, x);
  x8_y->GetValue(frame, y);
  xc_z->GetValue(frame, z);
  valOut = CVector3f(x, y, z);
  return false;
}

CVEFastConstant::CVEFastConstant(float x, float y, float z) : x4_val(x, y, z) {}

CVEFastConstant::~CVEFastConstant() {}

bool CVEFastConstant::GetValue(int frame, CVector3f& valOut) const {
  valOut = x4_val;
  return false;
}

CVECone::CVECone(CVectorElement* direction, CRealElement* magnitude)
: x4_direction(direction)
, x8_magnitude(magnitude)
, xc_xVec(CVector3f::Zero())
, x18_yVec(CVector3f::Zero()) {
  CVector3f av(0.f, 0.f, 0.f);
  x4_direction->GetValue(0, av);
  CVector3f avNorm = av.AsNormalized();
  if (avNorm.GetX() > 0.8f) {
    xc_xVec = CVector3f::Cross(av, CVector3f(0.f, 1.f, 0.f));
  } else {
    xc_xVec = CVector3f::Cross(av, CVector3f(1.f, 0.f, 0.f));
  }
  x18_yVec = CVector3f::Cross(avNorm, xc_xVec);
}

CVECone::~CVECone() {
  delete x4_direction;
  delete x8_magnitude;
}

bool CVECone::GetValue(int frame, CVector3f& valOut) const {
  float b;
  CVector3f dir(0.f, 0.f, 0.f);

  x8_magnitude->GetValue(frame, b);
  x4_direction->GetValue(frame, dir);
  b = rstl::min_val(1.f, b);

  float randX = 0.f, randY = 0.f;
  do {
    randX = 2.f * b * (CRandom16::GetRandomNumber()->Float() - 0.5f);
    randY = 2.f * b * (CRandom16::GetRandomNumber()->Float() - 0.5f);
  } while (randX * randX + randY * randY > 1.f);

  valOut = dir + xc_xVec * randX + x18_yVec * randY;
  return false;
}

CVEAngleCone::CVEAngleCone(CRealElement* angleXConstant, CRealElement* angleYConstant,
                           CRealElement* angleXRange, CRealElement* angleYRange,
                           CRealElement* magnitude)
: x4_angleXConstant(angleXConstant)
, x8_angleYConstant(angleYConstant)
, xc_angleXRange(angleXRange)
, x10_angleYRange(angleYRange)
, x14_magnitude(magnitude) {}

CVEAngleCone::~CVEAngleCone() {
  delete x4_angleXConstant;
  delete x8_angleYConstant;
  delete xc_angleXRange;
  delete x10_angleYRange;
  delete x14_magnitude;
}

bool CVEAngleCone::GetValue(int frame, CVector3f& valOut) const {
  float xc, xr, yc, yr;
  x4_angleXConstant->GetValue(frame, xc);
  x8_angleYConstant->GetValue(frame, yc);
  xc_angleXRange->GetValue(frame, xr);
  x10_angleYRange->GetValue(frame, yr);

  xc += (xr * 0.5f - (xr * CRandom16::GetRandomNumber()->Float()));
  xc *= M_PIF / 180.f;

  yc += (yr * 0.5f - (yr * CRandom16::GetRandomNumber()->Float()));
  yc *= M_PIF / 180.f;

  CVector3f vec = CVector3f(-CMath::FastSinR(yc) * CMath::FastCosR(xc), CMath::FastSinR(xc),
                            CMath::FastCosR(xc) * CMath::FastCosR(yc));
  float mag = 0.f;
  x14_magnitude->GetValue(frame, mag);

  valOut = vec * mag;
  return false;
}

CVECircle::CVECircle(CVectorElement* circleOffset, CVectorElement* circleNormal,
                     CRealElement* angleConstant, CRealElement* angleLinear, CRealElement* radius)
: mCircleOffset(circleOffset)
, mXVec(CVector3f::Zero())
, mYVec(CVector3f::Zero())
, mAngleConstant(angleConstant)
, mAngleLinear(angleLinear)
, mRadius(radius) {
  CVector3f direction = CVector3f(0.f, 0.f, 0.f);
  circleNormal->GetValue(0, direction);
  CVector3f normal = direction.AsNormalized();

  if (normal.GetX() > 0.8f) {
    mXVec = CVector3f::Cross(normal, CVector3f(0.f, 1.f, 0.f));
  } else {
    mXVec = CVector3f::Cross(normal, CVector3f(1.f, 0.f, 0.f));
  }
  mYVec = CVector3f::Cross(normal, mXVec);

  delete circleNormal;
}

CVECircle::~CVECircle() {
  delete mCircleOffset;
  delete mAngleConstant;
  delete mAngleLinear;
  delete mRadius;
}

bool CVECircle::GetValue(int frame, CVector3f& valOut) const {
  float radius;
  float angleLinear;
  float angleConstant;
  mAngleLinear->GetValue(frame, angleLinear);
  mRadius->GetValue(frame, radius);
  mAngleConstant->GetValue(frame, angleConstant);

  float curAngle = (angleLinear * frame + angleConstant) * (M_PIF / 180.f);

  CVector3f offset = CVector3f(0.f, 0.f, 0.f);
  mCircleOffset->GetValue(frame, offset);

  valOut = offset + (mXVec * radius * cosf(curAngle)) + (mYVec * radius * sinf(curAngle));
  return false;
}

CVETimeChain::CVETimeChain(CVectorElement* a, CVectorElement* b, CIntElement* switchFrame)
: mA(a), mB(b), mSwitchFrame(switchFrame) {}

CVETimeChain::~CVETimeChain() {
  delete mA;
  delete mB;
  delete mSwitchFrame;
}

bool CVETimeChain::GetValue(int frame, CVector3f& valOut) const {
  int switchFrame;
  mSwitchFrame->GetValue(frame, switchFrame);

  if (frame < switchFrame) {
    return mA->GetValue(frame, valOut);
  } else {
    return mB->GetValue(frame - switchFrame, valOut);
  }
}

CVECircleCluster::CVECircleCluster(CVectorElement* circleOffset, CVectorElement* circleNormal,
                                   CIntElement* cycleFrames, CRealElement* randomFactor)
: mCircleOffset(circleOffset)
, mXVec(CVector3f::Zero())
, mYVec(CVector3f::Zero())
, mRadius(0.f)
, mRandomFactor(randomFactor) {
  int _cycleFrames;
  cycleFrames->GetValue(0, _cycleFrames);
  mRadius = (M_PIF / 180.f) * (360.f / _cycleFrames);

  CVector3f normal = CVector3f(0.f, 0.f, 0.f);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CVector3f tmp(0.f, 0.f, 0.f);
#else
  CVector3f tmp;
#endif
  circleNormal->GetValue(0, normal);
  tmp = normal;
  if (normal.CanBeNormalized()) {
    normal = normal.AsNormalized();
  } else {
    normal = CVector3f::Up();
  }

  if (normal.GetX() > 0.8f) {
    mXVec = CVector3f::Cross(tmp, CVector3f(0.f, 1.f, 0.f));
  } else {
    mXVec = CVector3f::Cross(tmp, CVector3f(1.f, 0.f, 0.f));
  }

  mYVec = CVector3f::Cross(normal, mXVec);

  delete cycleFrames;
  delete circleNormal;
}

CVECircleCluster::~CVECircleCluster() {
  delete mCircleOffset;
  delete mRandomFactor;
}

bool CVECircleCluster::GetValue(int frame, CVector3f& valOut) const {
  float curAngle;
  curAngle = mRadius;
  curAngle *= frame;
  CVector3f offset = CVector3f(0.f, 0.f, 0.f);
  mCircleOffset->GetValue(frame, offset);

  CVector3f tv = offset + (mXVec * cosf(curAngle)) + (mYVec * sinf(curAngle));

  float dv;
  mRandomFactor->GetValue(frame, dv);

  float magnitude = dv * tv.Magnitude();
  float x = magnitude * CRandom16::GetRandomNumber()->Float();
  float y = magnitude * CRandom16::GetRandomNumber()->Float();
  float z = magnitude * CRandom16::GetRandomNumber()->Float();
  valOut = CVector3f(x + tv.GetX(), y + tv.GetY(), z + tv.GetZ());

  return false;
}

CVEAdd::CVEAdd(CVectorElement* a, CVectorElement* b) : mA(a), mB(b) {}
CVEAdd::~CVEAdd() {
  delete mA;
  delete mB;
}

bool CVEAdd::GetValue(int frame, CVector3f& valOut) const {
  CVector3f a = CVector3f::Zero();
  CVector3f b = CVector3f::Zero();

  mA->GetValue(frame, a);
  mB->GetValue(frame, b);

  valOut = a + b;
  return false;
}

CVEMultiply::CVEMultiply(CVectorElement* a, CVectorElement* b) : mA(a), mB(b) {}
CVEMultiply::~CVEMultiply() {
  delete mA;
  delete mB;
}

bool CVEMultiply::GetValue(int frame, CVector3f& valOut) const {
  CVector3f a = CVector3f::Zero();
  CVector3f b = CVector3f::Zero();

  mA->GetValue(frame, a);
  mB->GetValue(frame, b);

  valOut = CVector3f::ByElementMultiply(a, b);
  return false;
}

CVEPulse::CVEPulse(CIntElement* durationA, CIntElement* durationB, CVectorElement* a,
                   CVectorElement* b)
: mDurationA(durationA), mDurationB(durationB), mA(a), mB(b) {}

CVEPulse::~CVEPulse() {
  delete mDurationA;
  delete mDurationB;
  delete mA;
  delete mB;
}

bool CVEPulse::GetValue(int frame, CVector3f& valOut) const {
  int a;
  int b;
  mDurationA->GetValue(frame, a);
  mDurationB->GetValue(frame, b);
  int cv = a + b + 1;

  if (cv < 0) {
    cv = 1;
  }

  if (b >= 1) {
    if (frame % cv > a) {
      mB->GetValue(frame, valOut);
    } else {
      mA->GetValue(frame, valOut);
    }
  } else {
    mA->GetValue(frame, valOut);
  }
  return false;
}

CVEKeyframeEmitter::CVEKeyframeEmitter(CInputStream& in)
: mPercent(in.Get< uint >())
, mUnk1(in.Get< uint >())
, mLoop(in.Get< bool >())
, mUnk2(in.Get< bool >())
, mLoopEnd(in.Get< uint >())
, mLoopStart(in.Get< uint >())
, mKeys(in) {}

CVEKeyframeEmitter::~CVEKeyframeEmitter() {}

bool CVEKeyframeEmitter::GetValue(int frame, CVector3f& valOut) const {
  if (!mPercent) {
    int emitterTime = CParticleGlobals::GetEmitterTime();
    if (mLoop) {
      if (emitterTime >= mLoopEnd) {
        emitterTime -= mLoopStart;
        emitterTime %= mLoopEnd - mLoopStart;
        emitterTime += mLoopStart;
      }
      valOut = mKeys[emitterTime];
    } else {
      emitterTime = rstl::min_val(emitterTime, mLoopEnd - 1);
      valOut = mKeys[emitterTime];
    }
    return false;
  }

  if (CParticleGlobals::GetParticleLifetimePercentage() == 100) {
    valOut = mKeys[CParticleGlobals::GetParticleLifetimePercentage()];
  } else {
    float remainder = CParticleGlobals::GetParticleLifetimePercentageRemainder();
    valOut = (1.f - remainder) * mKeys[CParticleGlobals::GetParticleLifetimePercentage()] +
             remainder * mKeys[CParticleGlobals::GetParticleLifetimePercentage() + 1];
  }

  return false;
}

CVERealToVector::CVERealToVector(CRealElement* value) : mValue(value) {}
CVERealToVector::~CVERealToVector() { delete mValue; }

bool CVERealToVector::GetValue(int frame, CVector3f& valOut) const {
  float val = 0.f;
  mValue->GetValue(frame, val);
  valOut = CVector3f(val, val, val);

  return false;
}

bool CVEParticleLocation::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticle()->x4_pos;
  return false;
}

bool CVEParticlePreviousLocation::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticle()->x10_prevPos;
  return false;
}

bool CVEParticleVelocity::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticle()->x1c_vel;
  return false;
}

bool CVEParticleSystemOrientationFront::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticleSystem()->x4_system->GetOrientation().GetForward();
  return false;
}

bool CVEParticleSystemOrientationUp::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticleSystem()->x4_system->GetOrientation().GetUp();
  return false;
}

bool CVEParticleSystemOrientationRight::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticleSystem()->x4_system->GetOrientation().GetRight();
  return false;
}

bool CVEParticleSystemTranslation::GetValue(int frame, CVector3f& valOut) const {
  valOut = CParticleGlobals::GetCurrentParticleSystem()->x4_system->GetTranslation();
  return false;
}

CVESubtract::CVESubtract(CVectorElement* a, CVectorElement* b) : mA(a), mB(b) {}
CVESubtract::~CVESubtract() {
  delete mA;
  delete mB;
}

bool CVESubtract::GetValue(int frame, CVector3f& valOut) const {
  CVector3f a = CVector3f::Zero();
  CVector3f b = CVector3f::Zero();

  mA->GetValue(frame, a);
  mB->GetValue(frame, b);

  valOut = a - b;
  return false;
}

CVEColorToVector::CVEColorToVector(CColorElement* value) : mValue(value) {}
CVEColorToVector::~CVEColorToVector() { delete mValue; }

bool CVEColorToVector::GetValue(int frame, CVector3f& valOut) const {
  CColor val = CColor::Black();
  mValue->GetValue(frame, val);
  valOut.SetX(val.GetRed());
  valOut.SetY(val.GetGreen());
  valOut.SetZ(val.GetBlue());

  return false;
}

#ifdef TARGET_PC
namespace {
CQuaternion EulerQuaternion(const CVector3f& degrees) {
  constexpr float kDegreesToRadians = 0.01745329251994329577f;
  const float x = std::remainder(degrees.GetX(), 360.f) * kDegreesToRadians * 0.5f;
  const float y = std::remainder(degrees.GetY(), 360.f) * kDegreesToRadians * 0.5f;
  const float z = std::remainder(degrees.GetZ(), 360.f) * kDegreesToRadians * 0.5f;
  const CQuaternion qx(std::cos(x), std::sin(x), 0.f, 0.f);
  const CQuaternion qy(std::cos(y), 0.f, std::sin(y), 0.f);
  const CQuaternion qz(std::cos(z), 0.f, 0.f, std::sin(z));
  return qz * qy * qx;
}

CVector3f EulerDegreesFromMatrix(const CMatrix3f& matrix) {
  constexpr float kRadiansToDegrees = 57.295779513082320876f;
  constexpr float kGimbalCosineThreshold = 1.e-5f;
  const float sineY = std::clamp(-matrix.Get20(), -1.f, 1.f);
  const float cosineY = std::hypot(matrix.Get00(), matrix.Get10());
  const float y = std::atan2(sineY, cosineY);

  float x;
  float z;
  if (cosineY > kGimbalCosineThreshold) {
    x = std::atan2(matrix.Get21(), matrix.Get22());
    z = std::atan2(matrix.Get10(), matrix.Get00());
  } else {
    // At either Y gimbal, only a combined X/Z angle is observable. Choosing X=0
    // keeps the returned matrix equivalent without claiming angle continuity.
    x = 0.f;
    z = std::atan2(-matrix.Get01(), matrix.Get11());
  }
  return CVector3f(x * kRadiansToDegrees, y * kRadiansToDegrees, z * kRadiansToDegrees);
}

CVector3f EulerDegreesFromQuaternion(const CQuaternion& rotation) {
  return EulerDegreesFromMatrix(rotation.BuildTransform());
}
} // namespace

CVEPortRotationFromAxis::CVEPortRotationFromAxis(CVectorElement* first, CVectorElement* second,
                                                 CVectorElement* fallback, bool firstIsY)
: mFirst(first), mSecond(second), mFallback(fallback), mFirstIsY(firstIsY) {}

CVEPortRotationFromAxis::~CVEPortRotationFromAxis() {
  delete mFirst;
  delete mSecond;
  delete mFallback;
}

bool CVEPortRotationFromAxis::GetValue(int frame, CVector3f& valOut) const {
  CVector3f first = CVector3f::Zero();
  CVector3f second = CVector3f::Zero();
  mFirst->GetValue(frame, first);
  mSecond->GetValue(frame, second);
  CVector3f x, y, z;
  if (mFirstIsY ? PortParticleMath::RotationFromAxisYZ(first, second, x, y, z)
                : PortParticleMath::RotationFromAxisZY(first, second, x, y, z)) {
    valOut = EulerDegreesFromMatrix(CMatrix3f(x.GetX(), y.GetX(), z.GetX(), x.GetY(), y.GetY(),
                                              z.GetY(), x.GetZ(), y.GetZ(), z.GetZ()));
    return false;
  }
  valOut = CVector3f::Zero();
  if (mFallback) {
    mFallback->GetValue(frame, valOut);
  }
  return false;
}

CVEOrderedEulerProduct::CVEOrderedEulerProduct(CVectorElement* first, CVectorElement* second)
: mFirst(first), mSecond(second) {}

CVEOrderedEulerProduct::~CVEOrderedEulerProduct() {
  delete mFirst;
  delete mSecond;
}

bool CVEOrderedEulerProduct::GetValue(int frame, CVector3f& valOut) const {
  CVector3f first = CVector3f::Zero();
  CVector3f second = CVector3f::Zero();
  mFirst->GetValue(frame, first);
  mSecond->GetValue(frame, second);

  if (!std::isfinite(first.GetX()) || !std::isfinite(first.GetY()) || !std::isfinite(first.GetZ()) ||
      !std::isfinite(second.GetX()) || !std::isfinite(second.GetY()) || !std::isfinite(second.GetZ())) {
    valOut = CVector3f::Zero();
    return false;
  }

  valOut = EulerDegreesFromQuaternion(EulerQuaternion(first) * EulerQuaternion(second));
  return false;
}

bool CVEOrderedEulerProduct::IsFastConstant() const {
  return mFirst->IsFastConstant() && mSecond->IsFastConstant();
}

CVEPortNormalize::CVEPortNormalize(CVectorElement* a) : mA(a) {}

CVEPortNormalize::~CVEPortNormalize() { delete mA; }

bool CVEPortNormalize::GetValue(int frame, CVector3f& valOut) const {
  mA->GetValue(frame, valOut);
  valOut = PortParticleMath::NormalizeVector(valOut);
  return false;
}

CVEPortBasis::CVEPortBasis(CVectorElement* a, bool scale) : mA(a), mScale(scale) {}

CVEPortBasis::~CVEPortBasis() { delete mA; }

bool CVEPortBasis::GetValue(int frame, CVector3f& valOut) const {
  CVector3f v = CVector3f::Zero();
  const bool ret = mA ? mA->GetValue(frame, v) : false;
  valOut = CVector3f(mScale ? v.GetX() : -v.GetX(), v.GetZ(), v.GetY());
  return ret;
}

bool CVEPortBasis::IsFastConstant() const { return mA != nullptr && mA->IsFastConstant(); }

bool CVEPortSystemVector::GetValue(int, CVector3f& valOut) const {
  valOut = CVector3f::Zero();
  CParticleGlobals::SParticleSystem* sys = CParticleGlobals::GetCurrentParticleSystem();
  if (CParticleGlobals::xPortSwooshEval != nullptr || sys == nullptr || sys->x4_system == nullptr) {
    return false;
  }
  const CElementGen& gen = *sys->x4_system;
  switch (mKind) {
  case 'EXTT':
    valOut = gen.PortExternalTranslation();
    break;
  case 'EXTS':
    valOut = gen.PortExternalScale();
    break;
  case 'EXTR': {
    // The rotation element EXTR (AddRefVar(0, 4, 10), GetRotationElement 0x25d59c) reads the system
    // orientation; inside a PMRQ tree it is an Euler vector in Remastered's particle space, where
    // the quaternion's axis is (-x, z, y).
    const CQuaternion q = gen.PortExternalRotation();
    valOut = EulerDegreesFromQuaternion(
        CQuaternion(q.GetScalar(), CVector3f(-q.AxisX(), q.AxisZ(), q.AxisY())));
  } break;
  case 'CMRT': {
    // CERotationFromCamera 0x29d180: FromMatrix of GetCameraTransform's camera-to-world matrix
    // (the game camera), as EXTR in Remastered's particle space.
    const CQuaternion q = CQuaternion::FromMatrix(CParticleGlobals::PortCameraTransform());
    valOut = EulerDegreesFromQuaternion(
        CQuaternion(q.GetScalar(), CVector3f(-q.AxisX(), q.AxisZ(), q.AxisY())));
  } break;
  case 'CMPS':
    // CVECameraPositionInWorldSpace 0x2a23ec: GetCameraTransform 0x27b304's translation (the
    // game camera, camera-to-world).
    valOut = CParticleGlobals::PortCameraTransform().GetTranslation();
    break;
  default: {
    // CPSS: CVECameraPositionInSystemSpace 0x2a24fc, the world camera position taken into the
    // system's translation, orientation and scale (Compile 0x2a246c).
    CVector3f t, s;
    CQuaternion q;
    gen.PortGlobalFrame(t, q, s);
    valOut = PortParticleMath::CameraPositionInSystemSpace(
        CParticleGlobals::PortCameraTransform().GetTranslation(), t, q.GetScalar(), q.AxisX(),
        q.AxisY(), q.AxisZ(), s);
  } break;
  }
  return false;
}

bool CVEPortEmitterVelocity::GetValue(int, CVector3f& valOut) const {
  valOut = CVector3f::Zero();
  CParticleGlobals::SParticleSystem* sys = CParticleGlobals::GetCurrentParticleSystem();
  if (CParticleGlobals::xPortSwooshEval == nullptr && sys != nullptr && sys->x4_system != nullptr) {
    valOut = sys->x4_system->PortEmitterVelocity();
  }
  return false;
}

CVEPortRandomVector::CVEPortRandomVector(CRealElement* magnitude) : mMagnitude(magnitude) {}

CVEPortRandomVector::~CVEPortRandomVector() { delete mMagnitude; }

bool CVEPortRandomVector::GetValue(int frame, CVector3f& valOut) const {
  float magnitude = 1.f;
  if (mMagnitude) {
    mMagnitude->GetValue(frame, magnitude);
  }
  CRandom16* random = CRandom16::GetRandomNumber();
  const float u0 = random->PortFloat28();
  const float u1 = random->PortFloat28();
  const float u2 = random->PortFloat28();
  valOut = PortParticleMath::RandomVector(u0, u1, u2, magnitude);
  return false;
}

CVEPortAngleCone::CVEPortAngleCone(CRealElement* rotateX, CRealElement* rotateY,
                                   CRealElement* rotateZ, CRealElement* xRange,
                                   CRealElement* yRange, CRealElement* magnitude)
: mRotate{rotateX, rotateY, rotateZ}, mXRange(xRange), mYRange(yRange), mMagnitude(magnitude) {}

CVEPortAngleCone::~CVEPortAngleCone() {
  for (CRealElement* angle : mRotate) {
    delete angle;
  }
  delete mXRange;
  delete mYRange;
  delete mMagnitude;
}

// Children in Remastered's order (rotation, x range, y range, magnitude), each from 0, then
// the two draws.
bool CVEPortAngleCone::GetValue(int frame, CVector3f& valOut) const {
  float angles[3] = {0.f, 0.f, 0.f};
  for (int i = 0; i < 3; ++i) {
    mRotate[i]->GetValue(frame, angles[i]);
  }
  const PortParticleMath::Quat q = PortParticleMath::EulerQuat(angles[0], angles[1], angles[2]);
  float xRange = 0.f;
  float yRange = 0.f;
  float magnitude = 0.f;
  mXRange->GetValue(frame, xRange);
  mYRange->GetValue(frame, yRange);
  mMagnitude->GetValue(frame, magnitude);
  CRandom16* random = CRandom16::GetRandomNumber();
  const float u = random->PortFloat28();
  const float v = random->PortFloat28();
  valOut = PortParticleMath::AngleCone(q, xRange, yRange, u, v, magnitude);
  return false;
}
CVEPortMpac::CVEPortMpac(CRealElement* xCentre, CRealElement* yCentre, CRealElement* xRange, CRealElement* yRange,
                         CRealElement* magnitude)
: mXCentre(xCentre), mYCentre(yCentre), mXRange(xRange), mYRange(yRange), mMagnitude(magnitude) {}

CVEPortMpac::~CVEPortMpac() {
  delete mXCentre;
  delete mYCentre;
  delete mXRange;
  delete mYRange;
  delete mMagnitude;
}

// Children in order, each from 0, then the two draws (u for x, v for y).
bool CVEPortMpac::GetValue(int frame, CVector3f& valOut) const {
  float xCentre = 0.f;
  float yCentre = 0.f;
  float xRange = 0.f;
  float yRange = 0.f;
  float magnitude = 0.f;
  mXCentre->GetValue(frame, xCentre);
  mYCentre->GetValue(frame, yCentre);
  mXRange->GetValue(frame, xRange);
  mYRange->GetValue(frame, yRange);
  mMagnitude->GetValue(frame, magnitude);
  CRandom16* random = CRandom16::GetRandomNumber();
  const float u = random->PortFloat28();
  const float v = random->PortFloat28();
  const float ax = PortParticleMath::MpacAngle(xCentre, xRange, u);
  const float ay = PortParticleMath::MpacAngle(yCentre, yRange, v);
  valOut = PortParticleMath::MpacDirection(ax, ay, magnitude);
  return false;
}

CVEPortCircle::CVEPortCircle(CVectorElement* centre, CVectorElement* axis, CRealElement* startDegrees,
                             CIntElement* count, CRealElement* variance, CRealElement* radius, u8 shuffle)
: mCentre(centre)
, mAxis(axis)
, mStartDegrees(startDegrees)
, mCount(count)
, mVariance(variance)
, mRadius(radius)
, mShuffle(shuffle) {}

CVEPortCircle::~CVEPortCircle() {
  delete mCentre;
  delete mAxis;
  delete mStartDegrees;
  delete mCount;
  delete mVariance;
  delete mRadius;
}

// Children in order (centre, start, count, variance, radius), the index (the creation count, or the
// shuffler's pick) before the one draw.
bool CVEPortCircle::GetValue(int frame, CVector3f& valOut) const {
  CVector3f centre = CVector3f::Zero();
  CVector3f axis = CVector3f::Zero();
  float start = 0.f;
  int count = 0;
  float variance = 0.f;
  float radius = 0.f;
  mCentre->GetValue(frame, centre);
  mAxis->GetValue(0, axis);
  mStartDegrees->GetValue(frame, start);
  mCount->GetValue(frame, count);
  mVariance->GetValue(frame, variance);
  mRadius->GetValue(frame, radius);
  CRandom16* random = CRandom16::GetRandomNumber();
  CElementGen* generator = nullptr;
  const CParticleGlobals::SParticleSystem* system = CParticleGlobals::GetCurrentParticleSystem();
  if (system != nullptr && CParticleGlobals::xPortSwooshEval == nullptr) {
    generator = system->x4_system;
  }
  int index;
  if (mShuffle != 0) {
    PortParticleMath::IndexShuffler& shuffler =
        generator != nullptr ? generator->PortShuffler(this) : mFallbackShuffler;
    index = shuffler.Next(mShuffle, [random] { return uint32_t(random->Next()); }) & 0xff;
  } else {
    // Retail counts a particle before its emitter runs; Remastered's total is one less.
    index = generator != nullptr ? std::max(generator->GetCumulativeParticleCount() - 1, 0) : 0;
  }
  const float d = random->PortFloat28();
  const float angle = PortParticleMath::CircleAngle(start, count, variance, index, d);
  valOut = PortParticleMath::CirclePoint(centre, PortParticleMath::MakeCircleFrame(axis), radius, angle);
  return false;
}

CVEPortLine::CVEPortLine(CVectorElement* start, CVectorElement* end, CIntElement* count, CRealElement* variance,
                         u8 shuffle)
: mStart(start), mEnd(end), mCount(count), mVariance(variance), mShuffle(shuffle) {}

CVEPortLine::~CVEPortLine() {
  delete mStart;
  delete mEnd;
  delete mCount;
  delete mVariance;
}

// Children in order (start, end, count, variance); a count below 2 is 1. Only a segment of length
// 1e-5 or more takes the index (the creation count modulo n, or the shuffler's pick) and the one draw.
bool CVEPortLine::GetValue(int frame, CVector3f& valOut) const {
  CVector3f start = CVector3f::Zero();
  CVector3f end = CVector3f::Zero();
  int count = 0;
  float variance = 0.f;
  mStart->GetValue(frame, start);
  mEnd->GetValue(frame, end);
  mCount->GetValue(frame, count);
  mVariance->GetValue(frame, variance);
  count = count < 2 ? 1 : count;
  const float dx = end.GetX() - start.GetX();
  const float dy = end.GetY() - start.GetY();
  const float dz = end.GetZ() - start.GetZ();
  const float len = PortParticleMath::SqrtF((dx * dx + dy * dy) + dz * dz);
  if (!(std::fabs(len) >= 1e-5f)) {
    valOut = start;
    return false;
  }
  CRandom16* random = CRandom16::GetRandomNumber();
  CElementGen* generator = nullptr;
  const CParticleGlobals::SParticleSystem* system = CParticleGlobals::GetCurrentParticleSystem();
  if (system != nullptr && CParticleGlobals::xPortSwooshEval == nullptr) {
    generator = system->x4_system;
  }
  int index;
  if (mShuffle != 0) {
    PortParticleMath::IndexShuffler& shuffler =
        generator != nullptr ? generator->PortShuffler(this) : mFallbackShuffler;
    index = shuffler.Next(mShuffle, [random] { return uint32_t(random->Next()); }) & 0xff;
  } else {
    const int total = generator != nullptr ? std::max(generator->GetCumulativeParticleCount() - 1, 0) : 0;
    index = total % count;
  }
  const float d = random->PortFloat28();
  valOut = PortParticleMath::LinePoint(start, end, count, variance, index, d, len);
  return false;
}

CVEPortCone::CVEPortCone(CVectorElement* direction, CRealElement* radius)
: mDirection(direction), mRadius(radius), mU(0.f, 0.f, 0.f), mV(0.f, 0.f, 0.f) {}

CVEPortCone::~CVEPortCone() {
  delete mDirection;
  delete mRadius;
}

bool CVEPortCone::GetValue(int frame, CVector3f& valOut) const {
  if (frame == 0 && !mCached) {
    CVector3f direction(0.f, 0.f, 0.f);
    mDirection->GetValue(0, direction);
    const PortParticleMath::ConeFrame cone = PortParticleMath::MakeConeFrame(direction);
    mCached = true;
    mU = cone.u;
    mV = cone.v;
  }
  CVector3f position(0.f, 0.f, 0.f);
  mDirection->GetValue(frame, position);
  float radius = 0.f;
  mRadius->GetValue(frame, radius);
  if (1.f < radius) {
    radius = 1.f;
  }
  CRandom16* random = CRandom16::GetRandomNumber();
  float x;
  float y;
  do {
    const float a = random->PortFloat28();
    const float b = random->PortFloat28();
    x = PortParticleMath::ConeOffset(radius, a);
    y = PortParticleMath::ConeOffset(radius, b);
    const float xx = x * x;
    const float yy = y * y;
    if (!(1.f < xx + yy)) {
      break;
    }
  } while (true);
  valOut = PortParticleMath::ConePoint(PortParticleMath::ConeFrame{mU, mV}, position, x, y);
  return false;
}

CVEPortAngleConeMinMax::CVEPortAngleConeMinMax(CRealElement* rotateX, CRealElement* rotateY, CRealElement* rotateZ,
                                               CRealElement* minAngle, CRealElement* maxAngle, CRealElement* magnitude)
: mRotate{rotateX, rotateY, rotateZ}, mMinAngle(minAngle), mMaxAngle(maxAngle), mMagnitude(magnitude) {}

CVEPortAngleConeMinMax::~CVEPortAngleConeMinMax() {
  for (CRealElement* angle : mRotate) {
    delete angle;
  }
  delete mMinAngle;
  delete mMaxAngle;
  delete mMagnitude;
}

// Children in Remastered's order (rotation, A, B, magnitude), each from 0, then the polar and
// azimuth draws.
bool CVEPortAngleConeMinMax::GetValue(int frame, CVector3f& valOut) const {
  float angles[3] = {0.f, 0.f, 0.f};
  for (int i = 0; i < 3; ++i) {
    mRotate[i]->GetValue(frame, angles[i]);
  }
  const PortParticleMath::Quat q = PortParticleMath::EulerQuat(angles[0], angles[1], angles[2]);
  float minAngle = 0.f;
  float maxAngle = 0.f;
  float magnitude = 0.f;
  mMinAngle->GetValue(frame, minAngle);
  mMaxAngle->GetValue(frame, maxAngle);
  mMagnitude->GetValue(frame, magnitude);
  CRandom16* random = CRandom16::GetRandomNumber();
  const float u0 = random->PortFloat28();
  const float u1 = random->PortFloat28();
  valOut = PortParticleMath::AngleConeMinMax(q, minAngle, maxAngle, magnitude, u0, u1);
  return false;
}

CVEPortAngleConeMinMaxVariance::CVEPortAngleConeMinMaxVariance(
    CRealElement* rotateX, CRealElement* rotateY, CRealElement* rotateZ, CRealElement* minAngle,
    CRealElement* maxAngle, CRealElement* magnitude, CIntElement* polarCount, CRealElement* polarVariance,
    CIntElement* azimuthCount, CRealElement* azimuthVariance, u8 shuffle)
: mRotate{rotateX, rotateY, rotateZ}
, mMinAngle(minAngle)
, mMaxAngle(maxAngle)
, mMagnitude(magnitude)
, mPolarCount(polarCount)
, mPolarVariance(polarVariance)
, mAzimuthCount(azimuthCount)
, mAzimuthVariance(azimuthVariance)
, mShuffle(shuffle) {}

CVEPortAngleConeMinMaxVariance::~CVEPortAngleConeMinMaxVariance() {
  for (CRealElement* angle : mRotate) {
    delete angle;
  }
  delete mMinAngle;
  delete mMaxAngle;
  delete mMagnitude;
  delete mPolarCount;
  delete mPolarVariance;
  delete mAzimuthCount;
  delete mAzimuthVariance;
}

// Children in Remastered's order (rotation, A, B, magnitude, polar count and variance, azimuth count
// and variance), the counts at least 1, the index (the creation count modulo the grid, or the
// shuffler's pick), then the polar and azimuth draws.
bool CVEPortAngleConeMinMaxVariance::GetValue(int frame, CVector3f& valOut) const {
  float angles[3] = {0.f, 0.f, 0.f};
  for (int i = 0; i < 3; ++i) {
    mRotate[i]->GetValue(frame, angles[i]);
  }
  const PortParticleMath::Quat q = PortParticleMath::EulerQuat(angles[0], angles[1], angles[2]);
  float minAngle = 0.f;
  float maxAngle = 0.f;
  float magnitude = 0.f;
  int polarCount = 0;
  float polarVariance = 0.f;
  int azimuthCount = 0;
  float azimuthVariance = 0.f;
  mMinAngle->GetValue(frame, minAngle);
  mMaxAngle->GetValue(frame, maxAngle);
  mMagnitude->GetValue(frame, magnitude);
  mPolarCount->GetValue(frame, polarCount);
  mPolarVariance->GetValue(frame, polarVariance);
  mAzimuthCount->GetValue(frame, azimuthCount);
  mAzimuthVariance->GetValue(frame, azimuthVariance);
  polarCount = polarCount < 2 ? 1 : polarCount;
  azimuthCount = azimuthCount < 2 ? 1 : azimuthCount;
  CRandom16* random = CRandom16::GetRandomNumber();
  CElementGen* generator = nullptr;
  const CParticleGlobals::SParticleSystem* system = CParticleGlobals::GetCurrentParticleSystem();
  if (system != nullptr && CParticleGlobals::xPortSwooshEval == nullptr) {
    generator = system->x4_system;
  }
  int index;
  if (mShuffle != 0) {
    PortParticleMath::IndexShuffler& shuffler =
        generator != nullptr ? generator->PortShuffler(this) : mFallbackShuffler;
    index = shuffler.Next(mShuffle, [random] { return uint32_t(random->Next()); }) & 0xff;
  } else {
    // Retail counts a particle before its emitter runs; Remastered's total is one less.
    const int total = generator != nullptr ? std::max(generator->GetCumulativeParticleCount() - 1, 0) : 0;
    index = total % (polarCount * azimuthCount);
  }
  const float u0 = random->PortFloat28();
  const float u1 = random->PortFloat28();
  valOut = PortParticleMath::AngleConeMinMaxVariance(q, minAngle, maxAngle, magnitude, polarCount, polarVariance,
                                                     azimuthCount, azimuthVariance, index, u0, u1);
  return false;
}
CVEPortRotateVector::CVEPortRotateVector(CVectorElement* vector, FourCC rotation, CRealElement* x,
                                         CRealElement* y, CRealElement* z)
: mVector(vector), mRotation(rotation), mAngles{x, y, z} {}

CVEPortRotateVector::~CVEPortRotateVector() {
  delete mVector;
  for (CRealElement* angle : mAngles) {
    delete angle;
  }
}

// The rotation first (REUL's angles draw before the vector), then the vector, as 0x2a0328; a
// child that fails leaves its identity / zero default and the result is still the transform.
bool CVEPortRotateVector::GetValue(int frame, CVector3f& valOut) const {
  PortParticleMath::Quat q{1.f, 0.f, 0.f, 0.f};
  switch (mRotation) {
  case 'REUL': {
    float angles[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < 3; ++i) {
      if (mAngles[i]) {
        mAngles[i]->GetValue(frame, angles[i]);
      }
    }
    q = PortParticleMath::EulerQuat(angles[0], angles[1], angles[2]);
  } break;
  case 'EXTR': {
    // The system orientation, taken into Remastered's particle space (the basis conjugation).
    CParticleGlobals::SParticleSystem* sys = CParticleGlobals::GetCurrentParticleSystem();
    if (CParticleGlobals::xPortSwooshEval == nullptr && sys != nullptr && sys->x4_system != nullptr) {
      const CQuaternion r = sys->x4_system->PortExternalRotation();
      q = {r.GetScalar(), -r.AxisX(), r.AxisZ(), r.AxisY()};
    }
  } break;
  case 'INTR': {
    // INTR and RLCL: the system rotation var (GetRotationElement 0x25d59c, AddRefVar(ctx 0,
    // rotation, 0xb)), INTR's when authored, else the external one.
    CParticleGlobals::SParticleSystem* sys = CParticleGlobals::GetCurrentParticleSystem();
    if (CParticleGlobals::xPortSwooshEval == nullptr && sys != nullptr && sys->x4_system != nullptr) {
      const CQuaternion r = sys->x4_system->PortSystemRotationVar();
      q = {r.GetScalar(), -r.AxisX(), r.AxisZ(), r.AxisY()};
    }
  } break;
  case 'CMRT': {
    // CMRT: CERotationFromCamera 0x29d180, CQuaternion::FromMatrix of the camera transform
    // (CVECameraPositionInWorldSpace's matrix, the game camera), in Remastered's space.
    const CQuaternion r = CQuaternion::FromMatrix(CParticleGlobals::PortCameraTransform());
    q = {r.GetScalar(), -r.AxisX(), r.AxisZ(), r.AxisY()};
  } break;
  default:
    // A rotation kind the converter does not decode (it drops those): the vector passes through.
    break;
  }
  CVector3f v = CVector3f::Zero();
  if (mVector) {
    mVector->GetValue(frame, v);
  }
  valOut = PortParticleMath::QuatTransform(q, v);
  return false;
}

bool CVEPortNormalizedVelocity::GetValue(int, CVector3f& valOut) const {
  // The velocity in Remastered's axes, (-x, z, y) of the port's, so the sum of squares runs in
  // Remastered's order.
  const CVector3f& vel = CParticleGlobals::GetCurrentParticle()->x1c_vel;
  valOut = PortParticleMath::NormalizedVelocity(CVector3f(-vel.GetX(), vel.GetZ(), vel.GetY()));
  return false;
}
#endif
