#ifndef _CSAMUSFACEREFLECTION
#define _CSAMUSFACEREFLECTION

#include "MetroidPrime/CModelData.hpp"

#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CVector3f.hpp"

#include "rstl/single_ptr.hpp"

class CStateManager;
class CRandom16;
class CActorLights;

class CSamusFaceReflection {

public:
  explicit CSamusFaceReflection(const CStateManager& mgr);
  void PreDraw(const CStateManager& mgr);
  void Draw(const CStateManager& mgr) const;
  void Update(float dt, const CStateManager& mgr, CRandom16& rand);

private:
  CModelData x0_modelData;
  rstl::single_ptr< CActorLights > x4c_lights;
  CQuaternion x50_lookRot;
  CVector3f x60_lookDir;
  int x6c_;
  bool x70_hidden;
#ifdef TARGET_PC
  // Remastered: the colour FRMD explosions light the face with (PreRender 0xe7c044), and
  // whether that pass draws.
  CColor xPortColor = CColor::Black();
  bool xPortHidden = true;
  // Remastered: the camera up vector of the previous Update (this+0x18); the eyes follow
  // how far the new right vector leans into it (Update 0xe7be44).
  CVector3f xPortFaceUp = CVector3f::Forward();
#endif
};

#endif // _CSAMUSFACEREFLECTION
