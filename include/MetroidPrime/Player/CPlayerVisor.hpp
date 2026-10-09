#ifndef _CPLAYERVISOR
#define _CPLAYERVISOR

#include "types.h"

#include "MetroidPrime/Cameras/CCameraBlurPass.hpp"
#include "MetroidPrime/Cameras/CCameraFilterPass.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/TGameTypes.hpp"

#include "Kyoto/Audio/CSfxHandle.hpp"
#include "Kyoto/Math/CVector2f.hpp"
#include "Kyoto/TToken.hpp"

#include "rstl/reserved_vector.hpp"

class CModel;
class CStateManager;
class CTargetingManager;
class CTexture;

class CPlayerVisor {
  struct SScanObjectIndicatorInfo {
    SScanObjectIndicatorInfo(TUniqueId id, float timer, float inRangeTimer)
    : x0_objId(id), x4_timer(timer), x8_inRangeTimer(inRangeTimer), xc_inBox(false)
#ifdef TARGET_PC
    , xPort_t(0.f)
    , xPort_visible(false)
#endif
    {}

    TUniqueId x0_objId;
    float x4_timer;
    float x8_inRangeTimer;
    bool xc_inBox;
#ifdef TARGET_PC
    // Port: Remastered's per-entry pop-in (entry +0xc), 0..1 at 3/s toward "in the box".
    float xPort_t;
    // Port: IsScanOrGrapplePointVisibleToRender as of the previous update (one frame of lag).
    bool xPort_visible;
#endif
  };

  enum EScanWindowState {
    kSWS_NotInScanVisor,
    kSWS_Idle,
    kSWS_Scan,
  };

public:
  CPlayerVisor(const CStateManager& mgr);
  ~CPlayerVisor();

  void Update(float dt, const CStateManager& mgr);
  void Draw(const CStateManager& mgr, const CTargetingManager* tgtMgr) const;
  void Touch() const;
  float GetDesiredViewportScaleX(const CStateManager& mgr) const;
  float GetDesiredViewportScaleY(const CStateManager& mgr) const;
#ifdef TARGET_PC
  // Port (sharp scan window): the magnification and the screen-copy size (logical pixels) the
  // scan window shows this frame; false when it shows nothing magnified.
  bool PortScanZoom(const CStateManager& mgr, float& zoom, int& width, int& height) const;
  // Copies the centre of a view drawn zoomed by `zoom` as the window's picture (in place of
  // the width x height screen copy) and clears the EFB for the frame's own world.
  static void PortCopyScanZoom(int width, int height, float zoom);
  static void PortDropScanZoom();
  // What DrawScanEffect draws before its copy (scan shield, icons, target reticle).
  void PortDrawScanOverlay(const CStateManager& mgr, const CTargetingManager* tgtMgr) const;
#endif

private:
  void BeginTransitionOut();
  void FinishTransitionOut(const CStateManager& mgr);
  void BeginTransitionIn(const CStateManager& mgr);
  void FinishTransitionIn();
  void UpdateCurrentVisor(float transFactor);
  void DrawThermalEffect(const CStateManager& mgr) const;
  void DrawXRayEffect(const CStateManager& mgr) const;
  void DrawScanEffect(const CStateManager& mgr, const CTargetingManager* tgtMgr) const;
  float ScanWindowCopySize(const CStateManager& mgr, int vpWidth, int vpHeight, int& width,
                           int& height) const;
  void LockUnlockAssets();
  EScanWindowState GetDesiredScanWindowState(const CStateManager& mgr) const;
  void UpdateScanWindow(float dt, const CStateManager& mgr);
  void UpdateScanObjectIndicators(const CStateManager& mgr, float dt);
  bool DrawScanObjectIndicators(const CStateManager& mgr) const;
  int FindCachedInactiveScanTarget(TUniqueId uid) const;
  int FindEmptyInactiveScanTarget() const;

  rstl::reserved_vector< CVector2f, 3 > x0_scanWindowSizes;
  CPlayerState::EPlayerVisor x1c_curVisor;
  CPlayerState::EPlayerVisor x20_nextVisor;
  uchar x24_visorSfxVol;
  bool x25_24_visorTransitioning : 1;
  bool x25_25_ : 1;
  float x28_scanTimer;
  float x2c_scanDimInterp;
  EScanWindowState x30_prevState;
  EScanWindowState x34_nextState;
  float x38_windowInterpDuration;
  float x3c_windowInterpTimer;
  CVector2f x40_prevWindowDims;
  CVector2f x48_interpWindowDims;
  CVector2f x50_nextWindowDims;
  float x58_scanMagInterp;
  CSfxHandle x5c_visorLoopSfx;
  CSfxHandle x60_scanningLoopSfx;
  CCameraFilterPass x64_scanDim;
  CCameraBlurPass x90_xrayBlur;
  float xc4_vpScaleX;
  float xc8_vpScaleY;
  TCachedToken< CModel > xcc_scanFrameCorner;
  TCachedToken< CModel > xd8_scanFrameCenterSide;
  TCachedToken< CModel > xe4_scanFrameCenterTop;
  TCachedToken< CModel > xf0_scanFrameStretchSide;
  TCachedToken< CModel > xfc_scanFrameStretchTop;
  TCachedToken< CModel > x108_newScanPane;
  TCachedToken< CModel > x114_scanShield;
  int x120_assetLockCountdown;
  mutable TCachedToken< CModel > x124_scanIconNoncritical;
  mutable TCachedToken< CModel > x130_scanIconCritical;
  rstl::reserved_vector< SScanObjectIndicatorInfo, 64 > x13c_scanTargets;
  TCachedToken< CTexture > x540_xrayPalette;
  float x54c_scanFrameColorInterp;
  float x550_scanFrameColorImpulseInterp;
};
CHECK_SIZEOF(CPlayerVisor, 0x554)

#endif // _CPLAYERVISOR
