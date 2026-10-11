#ifndef _CGUICAMERA
#define _CGUICAMERA

#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"

class CGuiCamera : public CGuiWidget {
public:
  enum EProjection {
    kProjection_Perspective,
    kProjection_Orthographic,
  };

  union UCameraParms {
    struct {
      float fov;
      float aspect;
      float znear;
      float zfar;
    } perspective;
    struct {
      float left;
      float right;
      float top;
      float bottom;
      float znear;
      float zfar;
    } orthographic;
  };

  static CGuiWidget* Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp);
  CGuiCamera(const CGuiWidgetParms& parms, float fov, float aspect, float znear, float zfar);
  CGuiCamera(const CGuiWidgetParms& parms, float left, float right, float top, float bottom,
             float znear, float zfar);

  void Draw(const CGuiWidgetDrawParms& parms) const override;
  UCameraParms GetParms() const { return mCameraParms; }
  void SetParms(UCameraParms parms) { mCameraParms = parms; }
  // GUI cameras opt in so their projection tracks the widescreen render aspect
  // instead of stretching. Set for the in-game HUD, the pause/map screens and
  // the front end; anything left unset keeps its authored aspect. spread = false
  // opts out of the Widescreen HUD spread, so the frame is pillarboxed instead.
  void SetAspectMatch(bool match, bool spread = true) {
    xb9_aspectMatch = match;
    mSpreadable = spread;
  }
  // Widescreen HUD spread for this frame's widgets: 1.0 when inactive.
  float GetAspectSpread() const { return mSpread; }
  float GetAspectSpreadCenterX() const { return mSpreadCenterX; }
  // Vertical spread, the same remap on the screen-up axis: 1.0 unless the window is narrower than
  // 4:3 and the frame keeps its authored width.
  float GetAspectSpreadY() const { return mSpreadY; }
  // Perspective widgets move about the eye: wider than authored by a screen-space slide
  // (GetAspectSliceOffset), taller by sliding in their plane and pitching toward the eye.
  bool GetAspectSpreadAboutEye() const { return mSpreadAboutEye; }
  const CTransform4f& GetAspectSpreadView() const { return mSpreadView; }
  // A pillarboxed frame in a window taller than authored is clipped to the authored band (GX
  // scissor, top-left origin); false when the whole viewport is shown.
  bool GetClipBand(int& left, int& top, int& width, int& height) const {
    left = mClipLeft;
    top = mClipTop;
    width = mClipWidth;
    height = mClipHeight;
    return mClipHeight > 0;
  }
  CTransform4f GetAspectSpreadTransform(const CVector3f& worldAnchor) const;
  // The horizontal spread about the eye warps the screen in slices, in screen tangents (view X
  // over depth): up to `inner` it is left as authored, from `inner` to `outer` it stretches, and
  // past `outer` it slides by the whole spread. False unless that spread is active.
  // `curved` is the smooth variant for the helmet's arcs: a wider band with an eased stretch.
  bool GetAspectSlices(float& inner, float& outer, bool curved = false) const;
  // How far that warp moves the image at screen tangent `tangent`, for a warp centred on screen
  // tangent `center` (GetSliceCenter).
  float GetAspectSliceOffset(float tangent, bool curved = false, float center = 0.f) const;
  // HUD lag moves the frame's content on screen. `idleToView` maps the frame's view space without
  // lag onto the one drawn (identity at rest), so the warp's slices follow the content instead of
  // staying put while it moves through them.
  void SetSpreadLag(const CTransform4f& idleToView) { mSpreadLag = idleToView; }
  // Places a HUD camera at `xf` (CSamusHud::BuildFinalCameraTransform) whose unlagged pose is
  // unrotated at `idlePos`, and sets the spread lag to match.
  void SetHudO2WTransform(const CTransform4f& xf, const CVector3f& idlePos);
  // Where the lagged content's screen centre is, as a screen tangent at view depth `depth`.
  float GetSliceCenter(float depth) const;
  // Where the curved warp's band ends, as a fraction of the authored half width.
  static constexpr float kCurveOuter = 0.85f;
  // A frame whose centre piece is wider than the combat HUD's energy bar (the thermal and X-ray
  // reticle frames) moves the band out past it, in fractions of the authored half width; `curved`
  // eases the stretch for every sliced model of the frame. Combat's band when unset.
  void SetSliceBand(float inner, float outer, bool curved) {
    mSliceInner = inner;
    mSliceOuter = outer;
    mSliceCurved = curved;
    mSliceBand = true;
  }
  // Whether the warp for a model asking for `curved` is eased (a frame band decides for all).
  bool IsSliceCurved(bool curved) const { return mSliceBand ? mSliceCurved : curved; }
  // HUD scale: frames that opt in shrink about the view centre
  // (see PortDebug::HudScale).
  void SetHudScaled(bool scaled) { mHudScaled = scaled; }
  // 1.0 when inactive; computed by the last Draw.
  float GetHudScale() const { return mHudScale; }
  // The HUD scale alone, as a world-space transform; scaleWeight fades it
  // (0 = identity).
  CTransform4f GetHudScaleTransform(float scaleWeight = 1.f) const;
  // The widescreen spread followed by the HUD scale for a widget anchored at
  // worldAnchor.
  CTransform4f GetHudTransform(const CVector3f& worldAnchor, float scaleWeight = 1.f) const;

  FourCC GetWidgetTypeID() const override { return 'CAMR'; }

  CVector3f ConvertToScreenSpace(const CVector3f& point) const;

public:
  EProjection xb8_projection;
  bool xb9_aspectMatch = false;
  UCameraParms mCameraParms;
  // Port: horizontal spread applied to this frame's top-level widgets so the
  // HUD reaches the wide viewport edges without distorting element shapes.
  // Computed in the const Draw() and read immediately by CGuiFrame::Draw.
  bool mSpreadable = true;
  mutable float mSpread = 1.f;
  mutable float mSpreadCenterX = 0.f;
  mutable float mSpreadY = 1.f;
  mutable bool mSpreadAboutEye = false;
  // Half the authored screen width as a tangent, for the slices.
  mutable float mSpreadHalfTan = 0.f;
  bool mSliceBand = false;
  bool mSliceCurved = false;
  float mSliceInner = 0.f;
  float mSliceOuter = 0.f;
  CTransform4f mSpreadLag = CTransform4f::Identity();
  // Camera-to-world transform used by the last Draw.
  mutable CTransform4f mSpreadView = CTransform4f::Identity();
  bool mHudScaled = false;
  mutable float mHudScale = 1.f;
  // The view centre drawn by the last Draw (orthographic cameras may be offset).
  mutable float mCenterX = 0.f;
  mutable float mCenterZ = 0.f;
  mutable int mClipLeft = 0;
  mutable int mClipTop = 0;
  mutable int mClipWidth = 0;
  mutable int mClipHeight = 0;
};

#endif // _CGUICAMERA
