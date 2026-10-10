#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include <cmath>
#include <Kyoto/Graphics/CGraphics.hpp>

#include <Kyoto/Streams/CInputStream.hpp>

#include "port_debug.h"

CGuiWidget* CGuiCamera::Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp) {
  CGuiWidgetParms parms = ReadWidgetHeader(frame, in);
  EProjection proj = static_cast< EProjection >(in.ReadLong());
  CGuiCamera* camera = nullptr;
  if (proj == kProjection_Perspective) {
    const float fov = in.ReadFloat();
    const float aspect = in.ReadFloat();
    const float znear = in.ReadFloat();
    const float zfar = in.ReadFloat();
    camera = rs_new CGuiCamera(parms, fov, aspect, znear, zfar);
  } else if (proj == kProjection_Orthographic) {
    const float left = in.ReadFloat();
    const float right = in.ReadFloat();
    const float top = in.ReadFloat();
    const float bottom = in.ReadFloat();
    const float znear = in.ReadFloat();
    const float zfar = in.ReadFloat();
    camera = rs_new CGuiCamera(parms, left, right, top, bottom, znear, zfar);
  }

  frame->SetFrameCamera(camera);
  camera->ParseBaseInfo(frame, in, parms);
  return camera;
}

CGuiCamera::CGuiCamera(const CGuiWidgetParms& parms, float fov, float aspect, float znear,
                       float zfar)
: CGuiWidget(parms) {
  xb8_projection = kProjection_Perspective;
  CVector3f(1.f, 0.f, 0.f).Normalize();
  mCameraParms.perspective.fov = fov;
  mCameraParms.perspective.aspect = aspect;
  mCameraParms.perspective.znear = znear;
  mCameraParms.perspective.zfar = zfar;
}
CGuiCamera::CGuiCamera(const CGuiWidgetParms& parms, float left, float right, float top,
                       float bottom, float znear, float zfar)
: CGuiWidget(parms) {
  xb8_projection = kProjection_Orthographic;
  mCameraParms.orthographic.left = left;
  mCameraParms.orthographic.right = right;
  mCameraParms.orthographic.top = top;
  mCameraParms.orthographic.bottom = bottom;
  mCameraParms.orthographic.znear = znear;
  mCameraParms.orthographic.zfar = zfar;
}

void CGuiCamera::Draw(const CGuiWidgetDrawParms& parms) const {
  // The FRME cameras are authored for the original 4:3 render target. When the
  // port widens the framebuffer for widescreen, using the stored aspect would
  // stretch the whole GUI (HUD, menus) across the wider viewport. Match the
  // current render aspect instead so UI keeps its proportions; 4:3 is left
  // exactly as authored.
  float renderAspect = 0.f;
  // A mod frame authored for a wider screen (Remastered's are 16:9) is fitted
  // to 4:3 as well: its width is kept by widening the vertical view instead of
  // being squeezed into the narrower viewport.
  bool fitWidth = false;
  if (xb9_aspectMatch) {
    const float vw = static_cast< float >(CGraphics::GetViewportWidth());
    const float vh = static_cast< float >(CGraphics::GetViewportHeight());
    if (vw > 0.f && vh > 0.f) {
      renderAspect = vw / vh;
    }
    const float authored =
        xb8_projection == kProjection_Perspective
            ? mCameraParms.perspective.aspect
            : (mCameraParms.orthographic.right - mCameraParms.orthographic.left) /
                  (mCameraParms.orthographic.top - mCameraParms.orthographic.bottom);
    if (PortDebug::AspectMode() != PortDebug::kAspect_4_3) {
      // A window narrower than the frame was authored for (a portrait window, below 4:3): keep
      // the authored width and grow the vertical extent, instead of cutting off the sides.
      if (renderAspect > 0.f && renderAspect < 4.f / 3.f && authored > renderAspect * 1.001f) {
        fitWidth = true;
      }
    } else {
      if (renderAspect > 0.f && authored > renderAspect * 1.1f) {
        fitWidth = !PortDebug::HudWide();
      } else {
        renderAspect = 0.f;
      }
    }
  }

  mSpread = 1.f;
  mSpreadCenterX = 0.f;
  mSpreadY = 1.f;
  mSpreadAboutEye = false;
  mSpreadHalfTan = 0.f;
  mHudScale = mHudScaled ? static_cast< float >(PortDebug::HudScale()) / 100.f : 1.f;

  if (xb8_projection == kProjection_Perspective) {
    const float authored = mCameraParms.perspective.aspect;
    const float aspect = renderAspect > 0.f ? renderAspect : authored;
    // Widening a fixed FOV keeps the projection uniform; the HUD elements stay
    // correctly shaped but are pulled toward the centre, so spread their
    // positions to reach the true corners.
    if (!fitWidth && renderAspect > 0.f && authored > 0.f && mSpreadable &&
        PortDebug::HudWide()) {
      mSpread = renderAspect / authored;
      mSpreadAboutEye = true;
      mSpreadHalfTan =
          std::tan(0.5f * mCameraParms.perspective.fov * 3.14159265f / 180.f) * authored;
    }
    float fov = mCameraParms.perspective.fov;
    if (fitWidth) {
      // The window is taller than authored: spread the layout over the extra height, as the
      // widescreen spread does over extra width.
      if (mSpreadable) {
        mSpreadY = authored / renderAspect;
        mSpreadAboutEye = true;
      }
      constexpr float kDegToRad = 3.14159265f / 180.f;
      fov = 2.f * std::atan(std::tan(0.5f * fov * kDegToRad) * authored / renderAspect) / kDegToRad;
    }
    mCenterX = 0.f;
    mCenterZ = 0.f;
    CGraphics::SetPerspective(fov, aspect, mCameraParms.perspective.znear,
                              mCameraParms.perspective.zfar);
  } else {
    float left = mCameraParms.orthographic.left;
    float right = mCameraParms.orthographic.right;
    float top = mCameraParms.orthographic.top;
    float bottom = mCameraParms.orthographic.bottom;
    if (fitWidth) {
      const float middle = 0.5f * (top + bottom);
      const float halfHeight = 0.5f * (right - left) / renderAspect;
      if (mSpreadable && top > bottom) {
        mSpreadY = halfHeight / (0.5f * (top - bottom));
      }
      top = middle + halfHeight;
      bottom = middle - halfHeight;
    } else if (renderAspect > 0.f) {
      const float center = 0.5f * (left + right);
      const float halfWidth = 0.5f * (top - bottom) * renderAspect;
      left = center - halfWidth;
      right = center + halfWidth;
      const float authoredWidth = mCameraParms.orthographic.right - mCameraParms.orthographic.left;
      if (authoredWidth > 0.f && top > bottom && mSpreadable && PortDebug::HudWide()) {
        mSpread = renderAspect / (authoredWidth / (top - bottom));
        mSpreadCenterX = center;
      }
    }
    mCenterX = 0.5f * (left + right);
    mCenterZ = 0.5f * (top + bottom);
    CGraphics::SetOrtho(left, right, top, bottom, mCameraParms.orthographic.znear,
                        mCameraParms.orthographic.zfar);
  }

  mSpreadView =
      CTransform4f::Translate(parms.GetCameraOffset()) * GetWorldTransform();
  CGraphics::SetViewPointMatrix(mSpreadView);
  // A pillarboxed frame (no spread) in a window taller than authored would show widgets placed
  // just off the authored screen (the file select's corner brackets above its panel): clip it to
  // the authored band, as the original screen did.
  // CGuiFrame::Draw applies it around the frame's widgets.
  mClipHeight = 0;
  if (fitWidth && !mSpreadable) {
    int vpLeft, vpTop, vpWidth, vpHeight;
    CGraphics::GetViewport(vpLeft, vpTop, vpWidth, vpHeight);
    const float authored =
        xb8_projection == kProjection_Perspective
            ? mCameraParms.perspective.aspect
            : (mCameraParms.orthographic.right - mCameraParms.orthographic.left) /
                  (mCameraParms.orthographic.top - mCameraParms.orthographic.bottom);
    const int band =
        authored > 0.f ? static_cast< int >(static_cast< float >(vpWidth) / authored + 0.5f) : 0;
    if (band > 0 && band < vpHeight) {
      mClipLeft = vpLeft;
      mClipTop = vpTop + (vpHeight - band) / 2;
      mClipWidth = vpWidth;
      mClipHeight = band;
    }
  }
  CGuiWidget::Draw(parms);
}

CTransform4f CGuiCamera::GetAspectSpreadTransform(const CVector3f& worldAnchor) const {
  if (mSpread == 1.f && mSpreadY == 1.f) {
    return CTransform4f::Identity();
  }
  const CTransform4f invView = mSpreadView.GetInverse();
  const CVector3f eyePos = invView * worldAnchor;
  if (!mSpreadAboutEye) {
    return CTransform4f::Translate(mSpreadView.Rotate(CVector3f(
        (mSpread - 1.f) * (eyePos.GetX() - mSpreadCenterX), 0.f,
        (mSpreadY - 1.f) * (eyePos.GetZ() - mCenterZ))));
  }
  // Camera +Y is forward; +Z is screen-up. Keep the anchor's depth unchanged.
  if (eyePos.GetY() <= 0.f) {
    return CTransform4f::Identity();
  }
  if (mSpreadY != 1.f) {
    // The vertical spread: move in view Z and pitch to face the eye.
    const float pitch = std::atan2(eyePos.GetZ(), eyePos.GetY());
    const float delta = std::atan2(mSpreadY * eyePos.GetZ(), eyePos.GetY()) - pitch;
    const CVector3f offset(0.f, 0.f, (mSpreadY - 1.f) * eyePos.GetZ());
    return mSpreadView * CTransform4f::Translate(eyePos + offset) *
           CTransform4f::RotateX(CRelAngle(delta)) * CTransform4f::Translate(-eyePos) * invView;
  }
  // The horizontal spread: x += offset * depth is an exact image translation, so the widget keeps
  // its authored shape (turning it toward the eye squashed the side clusters at very wide ratios).
  const float offset = GetAspectSliceOffset(eyePos.GetX() / eyePos.GetY(), false,
                                            GetSliceCenter(eyePos.GetY()));
  return mSpreadView *
         CTransform4f(1.f, offset, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f) * invView;
}

bool CGuiCamera::GetAspectSlices(float& inner, float& outer, bool curved) const {
  if (!mSpreadAboutEye || mSpread == 1.f || mSpreadHalfTan <= 0.f) {
    return false;
  }
  // The middle holds the energy bar with its housing and lettering, and the helmet's top lights;
  // the band out to `outer` only the frame's top line and the helmet arc. Fractions of the
  // authored half width.
  // A curved model (the helmet's arcs) spreads the stretch out to its corners instead, so the arc
  // stays one smooth curve; a narrow band would flatten it into straight runs with kinks.
  if (mSliceBand) {
    inner = mSliceInner * mSpreadHalfTan;
    outer = mSliceOuter * mSpreadHalfTan;
    return true;
  }
  inner = 0.4f * mSpreadHalfTan;
  outer = (curved ? kCurveOuter : 0.455f) * mSpreadHalfTan;
  return true;
}

void CGuiCamera::SetHudO2WTransform(const CTransform4f& xf, const CVector3f& idlePos) {
  SetO2WTransform(xf);
  mSpreadLag = xf.GetQuickInverse() * CTransform4f::Translate(idlePos);
}

float CGuiCamera::GetSliceCenter(float depth) const {
  const CVector3f center = mSpreadLag * CVector3f(0.f, depth, 0.f);
  return center.GetY() > 0.f ? center.GetX() / center.GetY() : 0.f;
}

float CGuiCamera::GetAspectSliceOffset(float tangent, bool curved, float center) const {
  float inner, outer;
  if (!GetAspectSlices(inner, outer, curved)) {
    return 0.f;
  }
  curved = IsSliceCurved(curved);
  tangent -= center;
  // Past `outer` everything keeps its authored distance to the screen edge.
  const float shift = (mSpread - 1.f) * mSpreadHalfTan;
  const float a = std::fabs(tangent);
  float along = a <= inner ? 0.f : a >= outer ? 1.f : (a - inner) / (outer - inner);
  if (curved) {
    // Smoothstep: no change of slope where the stretch starts and ends.
    along = along * along * (3.f - 2.f * along);
  }
  const float moved = shift * along;
  return tangent < 0.f ? -moved : moved;
}

CTransform4f CGuiCamera::GetHudScaleTransform(float scaleWeight) const {
  const float scale = 1.f + (mHudScale - 1.f) * scaleWeight;
  if (scale == 1.f) {
    return CTransform4f::Identity();
  }
  // Scale view X and Z about the view centre, keeping depth: a screen-space
  // scale for perspective and orthographic cameras alike. The whole frame
  // shrinks together, so widgets stay registered with the visor frame mesh.
  const CVector3f center(mCenterX, 0.f, mCenterZ);
  return mSpreadView * CTransform4f::Translate(center) * CTransform4f::Scale(scale, 1.f, scale) *
         CTransform4f::Translate(-center) * mSpreadView.GetInverse();
}

CTransform4f CGuiCamera::GetHudTransform(const CVector3f& worldAnchor, float scaleWeight) const {
  return GetHudScaleTransform(scaleWeight) * GetAspectSpreadTransform(worldAnchor);
}

CVector3f CGuiCamera::ConvertToScreenSpace(const CVector3f& point) const {
  CVector3f rotated = RotateTranslateW2O(point);

  if (rotated.IsNonZero()) {
    CMatrix4f xf = CGraphics::CalculatePerspectiveMatrix(
        mCameraParms.perspective.fov, mCameraParms.perspective.aspect,
        mCameraParms.perspective.znear, mCameraParms.perspective.zfar);

    return xf.MultiplyOneOverW(rotated);
  }

  return CVector3f(-1.f, -1.f, 1.f);
}
