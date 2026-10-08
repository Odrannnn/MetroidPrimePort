#include "GuiSys/CGuiFrame.hpp"

#include "GuiSys/CAuiEnergyBarT01.hpp"
#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiFeeHelper.hpp"
#include "GuiSys/CGuiHeadWidget.hpp"
#include "GuiSys/CGuiLight.hpp"
#include "GuiSys/CGuiModel.hpp"
#include "GuiSys/CGuiSys.hpp"
#include "GuiSys/CGuiTextPane.hpp"
#include "GuiSys/CGuiWidget.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Input/CFinalInput.hpp"
#include "rstl/algorithm.hpp"

#include "port_hud_bars.h"

#include <dolphin/gx/GXGet.h>
#include <dolphin/gx/GXTransform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rstl {
class CWidgetFartherFromCamera {
public:
  CWidgetFartherFromCamera() {}
  bool operator()(const CGuiWidget* a, const CGuiWidget* b) const {
    const CVector3f aPos = a->GetWorldPosition();
    const CVector3f bPos = b->GetWorldPosition();
    return aPos.GetY() > bPos.GetY();
  }
};
} // namespace rstl

CGuiFrame::CGuiFrame(uint id, CGuiSys& sys, int a, int b, int c, CSimplePool* sp)
: x0_id(id)
, x4_(0)
, x8_guiSys(sys)
, xc_headWidget(nullptr)
, x10_rootWidget(nullptr)
, x14_camera(nullptr)
, x3c_lights(rstl::vector< CGuiLight* >(8, static_cast< CGuiLight* >(nullptr),
                                       rstl::rmemory_allocator()))
, x4c_a(a)
, x50_b(b)
, x54_c(c)
, x58_24_loaded(false) {
  x10_rootWidget = rs_new CGuiWidget(CGuiWidget::CGuiWidgetParms(
      this, false, CGuiWidget::gkDummyWidgetID, CGuiWidget::gkDummyWidgetID, false, false,
      false, CColor::White(), CGuiWidget::kGMDF_Alpha, false,
      !x8_guiSys.GetIsUsedInGame()));
}

CGuiFrame::~CGuiFrame() {
  if (x10_rootWidget) {
    delete x10_rootWidget;
  }
}

CGuiFrame* CGuiFrame::CreateFrame(uint id, CGuiSys& sys, CInputStream& in, CSimplePool* sp) {
  uint version = in.Get< uint >();
  int a = in.ReadLong();
  int b = in.ReadLong();
  int c = in.ReadLong();
  CGuiFrame* frame = rs_new CGuiFrame(id, sys, a, b, c, sp);
  CGuiFeeHelper::SetCurrentLoadingFrame(frame);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  frame->LoadWidgetsInGame(in, sp, version);
#elif defined(TARGET_PC)
  CGuiTextPane::sPortFrameVersion = version;
  frame->LoadWidgetsInGame(in, sp);
  CGuiTextPane::sPortFrameVersion = 0;
#else
  frame->LoadWidgetsInGame(in, sp);
#endif
  return frame;
}

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
int CGuiFrame::LoadWidgetsInGame(CInputStream& in, CSimplePool* sp, uint version) {
#else
int CGuiFrame::LoadWidgetsInGame(CInputStream& in, CSimplePool* sp) {
#endif
  int count = in.Get< int >();
  x2c_widgets.reserve(count);
  x18_db.Reserve(count);
  for (int i = 0; i < count; ++i) {
    FourCC type = in.ReadLong();
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
    CGuiWidget* widget = FGuiWidgetFactoryInGame(type, this, in, sp, version);
#else
    CGuiWidget* widget = CGuiSys::CreateWidgetInGame(type, in, this, sp);
#endif
    if (widget->GetWidgetTypeID() != 'CAMR' && widget->GetWidgetTypeID() != 'LITE' &&
        widget->GetWidgetTypeID() != 'BGND') {
      x2c_widgets.push_back(widget);
    }
  }
  Initialize();
  return 0;
}

void CGuiFrame::Initialize() {
  SortDrawOrder();
  CGuiHeadWidget* head = xc_headWidget;
  head->SetColor(head->GetColor());
  head->InitializeRecursive();
}

#ifdef TARGET_PC
// The view-space centre of a loaded model widget's bounds, through `invView`.
static bool ModelEyeCenter(const CGuiWidget* widget, const CTransform4f& invView, CVector3f& out) {
  if (widget->GetWidgetTypeID() != 'MODL' || !widget->GetIsFinishedLoading()) {
    return false;
  }
  const auto& token = static_cast< const CGuiModel* >(widget)->GetModel();
  const CModel* model = token ? token->GetObject() : nullptr;
  if (!model) {
    return false;
  }
  out = invView * (widget->GetWorldTransform() * model->GetBoundingBox().GetCenterPoint());
  return true;
}
#endif

void CGuiFrame::Draw(const CGuiWidgetDrawParms& parms) const {
  CGraphics::SetCullMode(kCM_None);
  CGraphics::ResetGfxStates();
  CGraphics::SetAmbientColor(CColor::White());
  DisableLights();
  x14_camera->Draw(parms);
  int clipLeft, clipTop, clipWidth, clipHeight;
  const bool clipped = x14_camera->GetClipBand(clipLeft, clipTop, clipWidth, clipHeight);
  u32 oldScissor[4];
  if (clipped) {
    GXGetScissor(&oldScissor[0], &oldScissor[1], &oldScissor[2], &oldScissor[3]);
    GXSetScissor(clipLeft, clipTop, clipWidth, clipHeight);
  }
  CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
  CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
  // Widescreen HUD: spread compact elements rigidly, but stretch screen-spanning
  // decoration about the view axis. Restore each transform after drawing.
  const float spread = x14_camera->GetAspectSpread();
  // HUD scale shrinks the whole frame about the view centre (see CGuiCamera).
  const float hudScale = x14_camera->GetHudScale();
  const CTransform4f hudScaleXf = x14_camera->GetHudScaleTransform();
  // Perspective elements move in their own plane and turn in place to face the
  // eye, preserving their depth ordering. Orthographic elements only slide.
  const float spreadY = x14_camera->GetAspectSpreadY();
  const bool aboutEye = (spread != 1.f || spreadY != 1.f) && x14_camera->GetAspectSpreadAboutEye();
  const CTransform4f spreadView =
      aboutEye ? x14_camera->GetAspectSpreadView() : CTransform4f::Identity();
  const CTransform4f invView = aboutEye ? spreadView.GetInverse() : CTransform4f::Identity();
  const CTransform4f stretch =
      aboutEye ? spreadView * CTransform4f::Scale(spread, 1.f, spreadY) * invView
               : CTransform4f::Identity();
  const CGraphics::CProjectionState& projection = CGraphics::GetProjectionState();
  const float authoredQuarterHeightPerDepth =
      aboutEye ? (projection.GetTop() - projection.GetBottom()) /
                     (4.f * projection.GetNear() * spreadY)
               : 0.f;
  const float authoredQuarterWidthPerDepth =
      aboutEye ? (projection.GetRight() - projection.GetLeft()) /
                     (4.f * projection.GetNear() * spread)
               : 0.f;
  // The spread is a remap of the authored layout, so it has to be a pure
  // function of where the element was authored to sit. Measuring from a
  // per-frame reference instead (the centroid of whichever widgets happen to be
  // visible) makes the same screen position map differently in each HUD frame -
  // and the HUD is several independent frames, so elements that belong together
  // register differently. Spread about the view axis, which is the screen
  // centre, using the element's own depth, so any two elements at the same place
  // get the same answer whichever frame draws them.
  // A slide group moves each half as one piece, by as far as the screen edge moves at the depth of
  // its outermost piece (the helmet shell): the shell keeps its 4:3 place against the edge and the
  // glass along it keeps touching it, instead of each sliding by its own height.
  const bool slide = aboutEye && spread == 1.f && spreadY != 1.f && !mSpreadSlide.empty();
  float slideTop = 0.f;
  float slideBottom = 0.f;
  // A member reaching well into both halves (Remastered's shell is one model with its top and
  // bottom pieces) is drawn twice, each half under a scissor and moved with its own edge.
  const auto spansBoth = [&](const CGuiWidget* member) {
    const CModel* model = static_cast< const CGuiModel* >(member)->GetModel()->GetObject();
    const CAABox bounds =
        model->GetBoundingBox().GetTransformedAABox(invView * member->GetWorldTransform());
    const float below = -bounds.GetMinPoint().GetZ();
    const float above = bounds.GetMaxPoint().GetZ();
    return below > 0.f && above > 0.f && std::min(below, above) > 0.25f * (below + above);
  };
  if (slide) {
    CVector3f top = CVector3f::Zero();
    CVector3f bottom = CVector3f::Zero();
    bool split = false;
    for (const CGuiWidget* member : mSpreadSlide) {
      CVector3f center;
      if (!ModelEyeCenter(member, invView, center)) {
        continue;
      }
      if (spansBoth(member)) {
        // It holds the outermost pieces of both halves: the edges follow its depth.
        if (!split || center.GetY() < top.GetY()) {
          top = bottom = center;
        }
        split = true;
      } else if (!split) {
        if (center.GetZ() > top.GetZ()) {
          top = center;
        }
        if (center.GetZ() < bottom.GetZ()) {
          bottom = center;
        }
      }
    }
    const float edgePerDepth = (spreadY - 1.f) / (projection.GetNear() * spreadY);
    slideTop = edgePerDepth * projection.GetTop() * top.GetY();
    slideBottom = edgePerDepth * projection.GetBottom() * bottom.GetY();
  }
  // Wider than authored, a model spanning the screen (the visor frame, the helmet) is warped in
  // slices instead of stretched (CGuiCamera::GetAspectSlices): the middle with the energy bar's
  // housing and lettering keeps its authored size, a band either side stretches, and the rest
  // slides out with the screen edge, as the compact widgets do. Each slice is drawn under its own
  // scissor with x' = scale * x + offset * depth in view space, a screen-space scale and slide.
  float sliceInner = 0.f;
  float sliceOuter = 0.f;
  const bool sliced = aboutEye && spreadY == 1.f && x14_camera->GetAspectSlices(sliceInner, sliceOuter);
  const auto sliceXf = [&](float scale, float offset) {
    return hudScaleXf * spreadView *
           CTransform4f(scale, offset, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f) * invView;
  };
  const auto drawSliced = [&](CGuiWidget* widget, const CTransform4f& world, bool curved) {
    int vpLeft, vpTop, vpWidth, vpHeight;
    CGraphics::GetViewport(vpLeft, vpTop, vpWidth, vpHeight);
    u32 old[4];
    GXGetScissor(&old[0], &old[1], &old[2], &old[3]);
    const float screenTan = projection.GetRight() / projection.GetNear();
    const auto column = [&](float t) {
      t = std::max(-screenTan, std::min(screenTan, t * hudScale));
      return vpLeft + static_cast< int >(0.5f * vpWidth * (1.f + t / screenTan) + 0.5f);
    };
    const auto draw = [&](float from, float to, float scale, float offset) {
      const int left = std::max(column(from), static_cast< int >(old[0]));
      const int right = std::min(column(to), static_cast< int >(old[0] + old[2]));
      if (right > left) {
        GXSetScissor(left, old[1], right - left, old[3]);
        widget->DrawWithWorldTransform(parms, sliceXf(scale, offset) * world);
      }
    };
    float inner, outer;
    x14_camera->GetAspectSlices(inner, outer, curved);
    draw(-inner, inner, 1.f, 0.f);
    // The band is one linear piece, or for the eased curve enough chords that the joints don't
    // show; each maps its authored span onto the warped one, mirrored on the left.
    const int pieces = curved ? 16 : 1;
    float from = inner;
    float fromWarped = inner;
    for (int piece = 1; piece <= pieces; ++piece) {
      const float to = inner + (outer - inner) * piece / pieces;
      const float toWarped = to + x14_camera->GetAspectSliceOffset(to, curved);
      const float scale = (toWarped - fromWarped) / (to - from);
      const float offset = fromWarped - scale * from;
      draw(fromWarped, toWarped, scale, offset);
      draw(-toWarped, -fromWarped, scale, -offset);
      from = to;
      fromWarped = toWarped;
    }
    const float shift = fromWarped - outer;
    const float edge = 2.f * screenTan;
    draw(fromWarped, edge, 1.f, shift);
    draw(-edge, -fromWarped, 1.f, -shift);
    GXSetScissor(old[0], old[1], old[2], old[3]);
  };
  for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
    CGuiWidget* widget = *it;
    if (!widget->GetIsVisible()) {
      continue;
    }
    if (spread == 1.f && spreadY == 1.f && hudScale == 1.f) {
      widget->Draw(parms);
      continue;
    }
    const CTransform4f world = widget->GetWorldTransform();
    CVector3f anchor = world.GetTranslation();
    // A cluster member (visor/beam selector) shares its group's anchor, so the cluster moves as
    // one unit instead of each icon and lozenge drifting by its own bounding box.
    const CGuiWidget* group = nullptr;
    for (const auto& entry : mSpreadAnchors) {
      if (entry.first == widget) {
        group = entry.second;
        break;
      }
    }
    if (aboutEye && spreadY != 1.f &&
        std::find(mSpreadStretch.begin(), mSpreadStretch.end(), widget) != mSpreadStretch.end()) {
      // A bar drawn along a side strut: the strut stretches over the extra height, so the bar
      // must too, or it stays a short piece in the middle of the screen.
      widget->DrawWithWorldTransform(parms, hudScaleXf * stretch * world);
      continue;
    }
    // The helmet's arcs take the eased warp wider than authored, and so do its lights with them.
    const bool curved =
        sliced && std::find(mSpreadSlide.begin(), mSpreadSlide.end(), widget) != mSpreadSlide.end();
    CVector3f slideCenter;
    if (slide &&
        std::find(mSpreadSlide.begin(), mSpreadSlide.end(), widget) != mSpreadSlide.end() &&
        ModelEyeCenter(widget, invView, slideCenter)) {
      const auto slid = [&](float offset) {
        return hudScaleXf * spreadView * CTransform4f::Translate(0.f, 0.f, offset) * invView * world;
      };
      if (!spansBoth(widget)) {
        widget->DrawWithWorldTransform(parms,
                                       slid(slideCenter.GetZ() >= 0.f ? slideTop : slideBottom));
        continue;
      }
      int vpLeft, vpTop, vpWidth, vpHeight;
      CGraphics::GetViewport(vpLeft, vpTop, vpWidth, vpHeight);
      u32 old[4];
      GXGetScissor(&old[0], &old[1], &old[2], &old[3]);
      // Each half is cut where the authored middle row lands once moved (at the model's depth),
      // so a half moved past the screen middle doesn't bring the other half's pieces with it.
      const float rowsPerUnit =
          0.5f * vpHeight * projection.GetNear() / (projection.GetTop() * slideCenter.GetY());
      const int middle = vpTop + vpHeight / 2;
      const int topCut = middle - static_cast< int >(slideTop * rowsPerUnit + 0.5f);
      const int bottomCut = middle - static_cast< int >(slideBottom * rowsPerUnit - 0.5f);
      const int first = static_cast< int >(old[1]);
      const int last = static_cast< int >(old[1] + old[3]);
      if (std::min(topCut, last) > first) {
        GXSetScissor(old[0], old[1], old[2], std::min(topCut, last) - first);
        widget->DrawWithWorldTransform(parms, slid(slideTop));
      }
      if (last > std::max(bottomCut, first)) {
        const int from = std::max(bottomCut, first);
        GXSetScissor(old[0], from, old[2], last - from);
        widget->DrawWithWorldTransform(parms, slid(slideBottom));
      }
      GXSetScissor(old[0], old[1], old[2], old[3]);
      continue;
    }
    if (group) {
      anchor = group->GetWorldTransform().GetTranslation();
    } else if (aboutEye && widget->GetWidgetTypeID() == 'ENRG') {
      // A mod's bar (port_hud_bars.h) keeps its widget at the frame centre and its strip out by a
      // side strut (Remastered's missile and threat bars): anchor it at the strip's middle, or it
      // stays put while the strut slides out.
      const PortHudBars::Bar* bar = static_cast< const CAuiEnergyBarT01* >(widget)->PortBar();
      if (bar && !bar->stations.empty()) {
        CVector3f sum = CVector3f::Zero();
        for (const PortHudBars::Station& station : bar->stations) {
          sum += CVector3f(station.a[0] + station.b[0], station.a[1] + station.b[1],
                           station.a[2] + station.b[2]);
        }
        anchor = world * (sum * (0.5f / static_cast< float >(bar->stations.size())));
      }
    } else if (widget->GetWidgetTypeID() == 'MODL' && widget->GetIsFinishedLoading()) {
      const auto& token = static_cast< const CGuiModel* >(widget)->GetModel();
      const CModel* model = token ? token->GetObject() : nullptr;
      if (model) {
        // Some icons have their position baked into vertices while their widget
        // origin is at the frame centre (notably threat and missile icons).
        if (aboutEye) {
          anchor = world * model->GetBoundingBox().GetCenterPoint();
          // SetViewPointMatrix takes a camera-to-world transform; camera-local
          // +Y is forward (the graphics layer maps it to projection-space -Z).
          const CAABox bounds = model->GetBoundingBox().GetTransformedAABox(invView * world);
          // Require the mesh bounds to reach both outer quarters of the authored
          // viewport, even at their farthest depth. Unlike extent / translation,
          // this does not stretch small icons at the origin or under a translated
          // parent. Only a fixed-size bounding box is examined, never vertices.
          const float farDepth = bounds.GetMaxPoint().GetY();
          const float quarterWidth = farDepth * authoredQuarterWidthPerDepth;
          const float quarterHeight = farDepth * authoredQuarterHeightPerDepth;
          const bool spansWidth = spread != 1.f && quarterWidth > 0.f &&
                                  bounds.GetMinPoint().GetX() < -quarterWidth &&
                                  bounds.GetMaxPoint().GetX() > quarterWidth;
          const bool spansHeight = spreadY != 1.f && quarterHeight > 0.f &&
                                   bounds.GetMinPoint().GetZ() < -quarterHeight &&
                                   bounds.GetMaxPoint().GetZ() > quarterHeight;

          if (sliced && spansWidth && bounds.GetMinPoint().GetY() > projection.GetNear()) {
            drawSliced(widget, world, curved);
            continue;
          }
          if (bounds.GetMinPoint().GetY() > projection.GetNear() && (spansWidth || spansHeight)) {
            // This intentionally widens vertical strokes too, but leaves view Y
            // and depth untouched; compact widgets retain the rigid path below.
            widget->DrawWithWorldTransform(parms, hudScaleXf * stretch * world);
            continue;
          }
        }
      }
    }
    // Never round-trip through SetO2WTransform: its quick parent inverse assumes
    // an orthonormal basis. Even slightly non-unit HUD-lag rotations would then
    // compound scale errors every draw, eventually shrinking the helmet lights.
    CTransform4f hudXf = x14_camera->GetHudTransform(anchor);
    if (group && aboutEye && spread == 1.f && spreadY != 1.f) {
      // A cluster off to the side: the rigid path pitches it to face the eye, which tips its
      // inward-turned cards into a slant against the struts. Sliding it straight down (or up) at
      // its own depth moves its image without changing its shape, so it looks as it does at 4:3.
      const float eyeZ = (invView * anchor).GetZ();
      hudXf = hudScaleXf * spreadView *
              CTransform4f::Translate(0.f, 0.f, (spreadY - 1.f) * eyeZ) * invView;
    }
    if (curved) {
      const CVector3f eye = invView * anchor;
      if (eye.GetY() > 0.f) {
        hudXf = sliceXf(1.f, x14_camera->GetAspectSliceOffset(eye.GetX() / eye.GetY(), true));
      }
    }
    widget->DrawWithWorldTransform(parms, hudXf * world);
  }
  if (clipped) {
    GXSetScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
  }
  CGraphics::SetCullMode(kCM_Front);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
#endif
}

void CGuiFrame::SetSpreadAnchor(const CGuiWidget* member, const CGuiWidget* anchor) {
  for (auto& entry : mSpreadAnchors) {
    if (entry.first == member) {
      entry.second = anchor;
      return;
    }
  }
  mSpreadAnchors.emplace_back(member, anchor);
}

void CGuiFrame::SetSpreadStretch(const CGuiWidget* widget) {
  if (widget && std::find(mSpreadStretch.begin(), mSpreadStretch.end(), widget) ==
                    mSpreadStretch.end()) {
    mSpreadStretch.push_back(widget);
  }
}

void CGuiFrame::SetSpreadStretchTree(const CGuiWidget* root) {
  for (CGuiWidget* widget : x2c_widgets) {
    for (const CGuiObject* obj = widget; obj; obj = obj->GetParent()) {
      if (obj == root) {
        SetSpreadStretch(widget);
        break;
      }
    }
  }
}

void CGuiFrame::SetSpreadSlideTree(const CGuiWidget* root) {
  for (CGuiWidget* widget : x2c_widgets) {
    for (const CGuiObject* obj = widget; obj; obj = obj->GetParent()) {
      if (obj == root) {
        if (std::find(mSpreadSlide.begin(), mSpreadSlide.end(), widget) == mSpreadSlide.end()) {
          mSpreadSlide.push_back(widget);
        }
        break;
      }
    }
  }
}

void CGuiFrame::SetSpreadAnchorTree(const CGuiWidget* root) {
  for (CGuiWidget* widget : x2c_widgets) {
    for (const CGuiObject* obj = widget->GetParent(); obj; obj = obj->GetParent()) {
      if (obj == root) {
        SetSpreadAnchor(widget, root);
        break;
      }
    }
  }
}

void CGuiFrame::Update(float dt) { xc_headWidget->Update(dt); }

void CGuiFrame::ProcessUserInput(const CFinalInput& input) {
  if (input.ControllerNumber() == 0) {
    for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
      CGuiWidget* widget = *it;
      if (widget->GetIsActive()) {
        widget->ProcessUserInput(input);
      }
    }
  }
}

void CGuiFrame::Touch() const {
  for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
    (*it)->Touch();
  }
}

bool CGuiFrame::GetIsFinishedLoading() const {
  if (x58_24_loaded) {
    return true;
  }
  x58_24_loaded = true;
  for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
    // Models stream in asynchronously and CGuiModel::Draw already skips itself
    // until ready. Blocking the whole frame on them meant a single slow/stuck
    // model texture kept the entire HUD (including the energy/missile bars)
    // hidden, e.g. after a morph-ball round trip.
    if ((*it)->GetWidgetTypeID() == 'MODL') {
      continue;
    }
    if (!(*it)->GetIsFinishedLoading()) {
      x58_24_loaded = false;
      if (std::getenv("MP_LOG_HUD") != nullptr) {
        static int sLogCount = 0;
        if (sLogCount < 80) {
          ++sLogCount;
          const FourCC type = (*it)->GetWidgetTypeID();
          std::fprintf(stderr, "[hud] frame blocked by %c%c%c%c id=%u\n",
                       static_cast< char >(type >> 24), static_cast< char >(type >> 16),
                       static_cast< char >(type >> 8), static_cast< char >(type),
                       (*it)->GetWidgetID());
        }
      }
      return false;
    }
  }
  return true;
}

void CGuiFrame::AddLight(CGuiLight* light) { x3c_lights[light->xd8_lightId] = light; }

void CGuiFrame::RemoveLight(CGuiLight* light) {
  if (x3c_lights[light->xd8_lightId] == light) {
    x3c_lights[light->xd8_lightId] = nullptr;
  }
}

void CGuiFrame::DisableLights() const { CGraphics::DisableAllLights(); }

void CGuiFrame::EnableLights(uint mask) const {
  CGraphics::DisableAllLights();
  CColor ambient = CColor::Black();
  int enabledLights = 0;
  for (int i = 0; i < x3c_lights.size(); ++i) {
    if (mask & (1 << i)) {
      CGuiLight* light = x3c_lights[i];
      if (light && light->GetIsVisible()) {
        const CColor& color = light->GetModifiedColor();
        if (color.GetRedu8() != 0 || color.GetGreenu8() != 0 || color.GetBlueu8() != 0) {
          CGraphics::LoadLight(static_cast< ERglLight >(i), light->BuildLight());
          CGraphics::EnableLight(static_cast< ERglLight >(i));
        }
        ambient = CColor::Add(ambient, CColor(light->xdc_ambColor));
        ++enabledLights;
      }
    }
  }
  if (enabledLights == 0) {
    CGraphics::SetAmbientColor(CColor::White());
  } else {
    CGraphics::SetAmbientColor(ambient);
  }
}

void CGuiFrame::SortDrawOrder() {
  rstl::sort(x2c_widgets.begin(), x2c_widgets.end(), rstl::CWidgetFartherFromCamera());
}

void CGuiFrame::RemoveWidgetFromDrawList(CGuiWidget* widget) {
  AUTO(it, x2c_widgets.begin());
  AUTO(end, x2c_widgets.end());
  for (; it != end; ++it) {
    if (*it == widget) {
      x2c_widgets.erase(it);
      break;
    }
  }
}

CGuiWidget* CGuiFrame::FindWidget(const rstl::string& name) const {
  short id = x18_db.FindWidgetID(name);
  if (id != CGuiWidget::InvalidWidgetId()) {
    return FindWidget(id);
  }
  return nullptr;
}

CGuiWidget* CGuiFrame::FindWidget(short id) const { return x10_rootWidget->FindWidget(id); }

void CGuiFrame::SetHeadWidget(CGuiHeadWidget* widget) { xc_headWidget = widget; }

void CGuiFrame::SetFrameCamera(CGuiCamera* camera) { x14_camera = camera; }

CGuiWidget* CGuiFrame::FindWidget(const char* name) const { return FindWidget(rstl::string_l(name)); }

CGuiLight* CGuiFrame::GetFrameLight(int idx) { return x3c_lights[idx]; }

#ifdef TARGET_PC
bool CGuiFrame::PortHasHudInterference() const {
  for (CGuiWidget* widget : x2c_widgets) {
    if (widget->GetWidgetTypeID() != FourCC('MODL')) {
      continue;
    }
    const rstl::optional_object< TCachedToken< CModel > >& token =
        static_cast< const CGuiModel* >(widget)->GetModel();
    if (!token.valid() || (*token).GetObject() == nullptr) {
      continue;
    }
    const CCubeModel* cube = (*token).GetObject()->GetCubeModel();
    if (cube == nullptr) {
      continue;
    }
    const uint count = cube->PortMaterialCount();
    for (uint i = 0; i < count; ++i) {
      if (cube->GetMaterialByIndex(static_cast< int >(i)).IsFlagSet(kStateFlag_PortHudInterference)) {
        return true;
      }
    }
  }
  return false;
}
#endif
