#include "GuiSys/CGuiTextPane.hpp"

#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Basics/CCast.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"

#ifdef TARGET_PC
#include "port_pointer.h"
#include "port_pause_lighting.h"
#include <dolphin/gx.h>

namespace {
// Capture the pane's camera, not the doll/helmet camera current at replay.
// Widget hit testing stays in the original traversal, while text/image draws
// replay with their original alpha, blend flags and model transform.
struct TextViewState {
  CGraphics::CProjectionState projection = CGraphics::GetProjectionState();
  CTransform4f view = CGraphics::GetViewMatrix();
  CTransform4f model = CGraphics::GetModelMatrix();
  CViewport viewport = CGraphics::GetViewport();
  float near = CGraphics::GetDepthNear(), far = CGraphics::GetDepthFar();
  u32 left, top, width, height;
  TextViewState() { GXGetScissor(&left, &top, &width, &height); }
  void Apply() const {
    CGraphics::SetViewport(viewport.mLeft,
                           CGraphics::GetRenderMode().efbHeight - viewport.mTop - viewport.mHeight,
                           viewport.mWidth, viewport.mHeight);
    CGraphics::SetDepthRange(near, far);
    CGraphics::SetProjectionState(projection);
    CGraphics::SetViewPointMatrix(view);
    CGraphics::SetModelMatrix(model);
    GXSetScissor(left, top, width, height);
  }
};
} // namespace
#endif

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
bool CGuiTextPane::sDrawPaneRects = false;
#elif defined(TARGET_PC)
uint CGuiTextPane::sPortFrameVersion = 0;
#endif

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
CGuiWidget* CGuiTextPane::Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp, uint version) {
#else
CGuiWidget* CGuiTextPane::Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp) {
#endif
  const CGuiWidgetParms parms = ReadWidgetHeader(frame, in);
  float dimX = in.Get< float >();
  float dimY = in.Get< float >();
  CVector3f vec(in);
  const CAssetId fontId = in.ReadLong();
  bool wordWrap = in.Get< bool >();
  bool horizontal = in.Get< bool >();
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  const EJustification justification = static_cast< EJustification >(in.ReadLong());
  const uint verticalJustification = in.ReadLong();
  const EVerticalJustification vJustification =
      static_cast< EVerticalJustification >(verticalJustification);
#else
  EJustification justification = static_cast< EJustification >(in.Get< int >());
  EVerticalJustification vJustification = static_cast< EVerticalJustification >(in.Get< int >());
#endif
  CColor fontCol(in);
  CColor outlineColor(in);
  const int extentX = CCast::ToInt32(in.Get< float >());
  const int extentY = CCast::ToInt32(in.Get< float >());
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CAssetId jpFontId = version >= 1 ? in.Get< CAssetId >() : fontId;
  int jpExtentX = version >= 1 ? in.Get< int >() : extentX;
  int jpExtentY = version >= 1 ? in.Get< int >() : extentY;
#elif defined(TARGET_PC)
  if (sPortFrameVersion >= 1) {
    in.Get< CAssetId >();
    in.Get< int >();
    in.Get< int >();
  }
#endif
  const CGuiTextProperties props(wordWrap, horizontal, justification, vJustification, nullptr);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CGuiTextPane* ret = rs_new CGuiTextPane(parms, sp, dimX, dimY, vec, fontId, props, fontCol,
                                        outlineColor, extentX, extentY, jpFontId, jpExtentX, jpExtentY);
#else
  CGuiTextPane* ret = rs_new CGuiTextPane(parms, sp, dimX, dimY, vec, fontId, props, fontCol,
                                        outlineColor, extentX, extentY);
#endif
  ret->ParseBaseInfo(frame, in, parms);
  ret->InitializeBuffers();
  ret->TextSupport().SetText(rstl::string_l(""));
  return ret;
}

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
CGuiTextPane::CGuiTextPane(const CGuiWidgetParms& parms, CSimplePool* sp, const float dimX,
                         const float dimY, const CVector3f& vec, const CAssetId fontId,
                         const CGuiTextProperties& props, const CColor& col1, const CColor& col2,
                         const int padX, const int padY, CAssetId jpFontId, int jpExtentX, int jpExtentY)
: CGuiPane(parms, dimX, dimY, vec)
, xd4_textSupport(fontId, padX, padY, props, col1, col2, CColor::White(), sp)
, xd00_drawShadow(false) {}
#else
CGuiTextPane::CGuiTextPane(const CGuiWidgetParms& parms, CSimplePool* sp, const float dimX,
                         const float dimY, const CVector3f& vec, const CAssetId fontId,
                         const CGuiTextProperties& props, const CColor& col1, const CColor& col2,
                         const int padX, const int padY)
: CGuiPane(parms, dimX, dimY, vec)
, xd4_textSupport(fontId, props, col1, col2, CColor::White(), padX, padY, sp) {}
#endif

CGuiTextPane::~CGuiTextPane() {}

void CGuiTextPane::Draw(const CGuiWidgetDrawParms& parms) const {
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  if (sDrawPaneRects) {
    CGuiPane::Draw(CGuiWidgetDrawParms(0.2f * parms.GetAlpha(), parms.GetCameraOffset()));
  }
#endif
  if (!GetIsVisible()) {
    return;
  }

  const float* vtxBuf = GetVtxBuf();
#ifdef TARGET_PC
  // Port: the pane's rect, for mouse and touch menus (the glyphs follow).
  PortGuiHit::RecordPoints(this, GetWorldTransform() * CTransform4f::Translate(GetScaleCenter()), vtxBuf,
                           4);
#endif
  CVector2f dimensions = GetDimensions();
  float width = GetTextSupport().GetTextBoundingWidth() == 0
                    ? 0.f
                    : dimensions.GetX() / GetTextSupport().GetTextBoundingWidth();
  float height = GetTextSupport().GetTextBoundingHeight() == 0
                     ? 0.f
                     : dimensions.GetY() / GetTextSupport().GetTextBoundingHeight();
  CTransform4f local =
      CTransform4f::Translate(CVector3f(vtxBuf[0], vtxBuf[1], vtxBuf[2]) + GetPivot()) *
      CTransform4f::Scale(width, 1.f, height);
  CTransform4f model = GetWorldTransform() * local;
#ifdef TARGET_PC
  // Text often runs past the pane: the glyphs are hit too (text y runs down, z up).
  {
    const rstl::pair< CVector2i, CVector2i >& bounds = xd4_textSupport.GetBounds();
    if (bounds.first.GetX() < bounds.second.GetX() && bounds.first.GetY() < bounds.second.GetY()) {
      const float x0 = static_cast< float >(bounds.first.GetX());
      const float y0 = static_cast< float >(bounds.first.GetY());
      const float x1 = static_cast< float >(bounds.second.GetX());
      const float y1 = static_cast< float >(bounds.second.GetY());
      const float glyphs[12] = {x0, 0.f, -y0, x1, 0.f, -y0, x0, 0.f, -y1, x1, 0.f, -y1};
      PortGuiHit::RecordPoints(this, model, glyphs, 4);
    }
  }
#endif
  const CColor geomCol = GetModifiedColor().WithAlphaModulatedBy(parms.GetAlpha());
  const EGuiModelDrawFlags flags = GetDrawFlags();
  const bool depthRead = GetIsAlwaysDepthRead(), depthWrite = GetIsAlwaysDepthWrite();
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  const bool drawShadow = xd00_drawShadow;
#else
  const bool drawShadow = false;
#endif
#ifdef TARGET_PC
  const auto render = [this, model, geomCol, flags, depthRead, depthWrite, drawShadow]() {
#endif
  CGraphics::SetDepthWriteMode(depthRead, kE_LEqual, depthWrite);

  if (drawShadow) {
    CGraphics::SetModelMatrix(model * CTransform4f::Translate(2.f, 0.f, -2.f));
    xd4_textSupport.SetGeometryColor(
        CColor(static_cast< uchar >(0), static_cast< uchar >(0), static_cast< uchar >(0))
            .WithAlphaOf(0.99f * geomCol.GetAlpha()));
    CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
    xd4_textSupport.Render();
  }

  CGraphics::SetModelMatrix(model);
  CGuiTextSupport& text = xd4_textSupport;
  text.SetGeometryColor(geomCol);

  switch (flags) {
  case kGMDF_Shadeless:
  case kGMDF_Opaque:
    CGraphics::SetBlendMode(kBM_Blend, kBF_One, kBF_Zero, kLO_Clear);
    text.Render();
    break;
  case kGMDF_Alpha:
    CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
    text.Render();
    break;
  case kGMDF_Additive:
    CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_One, kLO_Clear);
    text.Render();
    break;
  case kGMDF_AlphaAdditiveOverdraw: {
    CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
    text.Render();
    uchar alpha = geomCol.GetAlphau8();
    const CColor alphaColor(alpha, alpha, alpha, static_cast< uchar >(255));
    const CColor additiveColor = CColor::Modulate(geomCol, alphaColor);
    text.SetGeometryColor(additiveColor);
    CGraphics::SetBlendMode(kBM_Blend, kBF_One, kBF_One, kLO_Clear);
    text.Render();
    break;
  }
  }
#ifdef TARGET_PC
  };
  if (PortPauseLighting::TextPass::Deferring()) {
    const TextViewState state;
    PortPauseLighting::TextPass::Queue([state, render]() {
      const TextViewState restore;
      // The helmet drew in between: redo CGuiFrame::Draw's state setup, or its
      // lights and fog leave the glyphs black on the black backdrop.
      CGraphics::SetCullMode(kCM_None);
      CGraphics::ResetGfxStates();
      CGraphics::SetAmbientColor(CColor::White());
      CGraphics::DisableAllLights();
      CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
      state.Apply();
      render();
      restore.Apply();
    });
  } else {
    render();
  }
#endif
}

void CGuiTextPane::ScaleDimensions(const CVector3f&) {}

void CGuiTextPane::SetDimensions(const CVector2f& dim, bool initVBO) {
  CGuiPane::SetDimensions(dim, initVBO);
  if (initVBO) {
    InitializeBuffers();
  }
}

rstl::vector< CAssetId > CGuiTextPane::GetFontAssets() const {
  return rstl::vector< CAssetId >(1, GetTextSupport().GetFontID(), rstl::rmemory_allocator());
}

bool CGuiTextPane::GetIsFinishedLoadingWidgetSpecific() const {
  return GetTextSupport().GetIsTextSupportFinishedLoading();
}

void CGuiTextPane::Update(float dt) {
  CGuiWidget::Update(dt);
  TextSupport().Update(dt);
}
