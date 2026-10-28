// The automap's port-only pointer input and legend (CAutoMapper::PortRecordPointerHits,
// PortShowsMarkPrompt and PortPointerInput with their helpers), moved out of
// src/MetroidPrime/CAutoMapper.cpp so the decomp file keeps only its hooks (see
// docs/NATIVE_PORT.md, "Where port code goes"). The legend helpers the decomp
// file still calls are declared in port_automapper.h; everything else is as it
// was, moved verbatim.
//
// Built as part of mp_game, with the same flags and includes as the file it
// came from.

#include "MetroidPrime/CAutoMapper.hpp"
#include "rstl/math.hpp"

#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CAbsAngle.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Text/CStringTable.hpp"

#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiTextPane.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"

#include "MetaRender/CCubeRenderer.hpp"

#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/CEulerAngles.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CGameHintInfo.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CMapArea.hpp"
#include "MetroidPrime/CMapUniverse.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Tweaks/CTweakAutoMapper.hpp"
#include "MetroidPrime/Tweaks/CTweakGui.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayerRes.hpp"
#include "rstl/StringExtras.hpp"
#include "rstl/rc_ptr.hpp"

#include <math.h>

#include <utility>
#include <vector>

#include "port_disc.h"
#include "port_debug.h"
#include "port_pointer.h"
#include "port_pointer_menu.h"
#include "port_map_icons.h"
#include "port_custom_res.h"
#include "port_map_marks.h"
#include "port_map_pickups.h"
#include "port_automapper.h"

#ifdef TARGET_PC
namespace {

// What Draw put under the pointer last frame: map areas (world map) or
// universe hexes, as GUI rects. sub is the hex index in the universe.
struct PortMapHit {
  int id;
  int sub;
  float x0, y0, x1, y1;
  float cx, cy;
};
std::vector< PortMapHit > sPortMapHits;
bool sPortMapHitsUniverse = false;
// The world map's area surfaces as GUI triangles; hit indexes sPortMapHits.
// An area with triangles is hit only inside one, not anywhere in its rect.
struct PortMapTri {
  int hit;
  float x[3], y[3];
};
std::vector< PortMapTri > sPortMapTris;
// GUI pixels per two yScale (the pan's view height, see ProcessMapPanInput).
float sPortMapViewPx = 480.f;

// A clicked area or world the area point glides to; the id stays set once it
// lands and is cleared when a glide is cancelled.
bool sPortMapGlide = false;
bool sPortMapGlideUniverse = false;
int sPortMapGlideId = -1;
CVector3f sPortMapGlideTo(0.f, 0.f, 0.f);
// A click on the gliding world enters it once the glide lands.
bool sPortMapEnter = false;

// Degrees the map turns per GUI pixel of a left mouse drag.
const float kPortMapDegPerPx = 0.4f;
// Farthest a click can be from a universe hex's center, in GUI pixels.
const float kPortMapHexReach = 40.f;

void PortMapStopGlide() {
  sPortMapGlide = false;
  sPortMapEnter = false;
  sPortMapGlideId = -1;
}

bool PortMapInTri(const PortMapTri& t, float gx, float gy) {
  float s[3];
  for (int e = 0; e < 3; ++e) {
    const int n = (e + 1) % 3;
    s[e] = (t.x[n] - t.x[e]) * (gy - t.y[e]) - (t.y[n] - t.y[e]) * (gx - t.x[e]);
  }
  return (s[0] >= 0.f && s[1] >= 0.f && s[2] >= 0.f) ||
         (s[0] <= 0.f && s[1] <= 0.f && s[2] <= 0.f);
}

// Collects one area's surface triangles from its projected vertices.
struct PortMapTriSink {
  int hit;
  std::vector< float > px, py;
  std::vector< bool > ok;

  static void Add(void* ctx, int a, int b, int c) {
    const PortMapTriSink& s = *static_cast< const PortMapTriSink* >(ctx);
    const int n = static_cast< int >(s.ok.size());
    if (a >= n || b >= n || c >= n || !s.ok[a] || !s.ok[b] || !s.ok[c]) {
      return;
    }
    PortMapTri t = {s.hit, {s.px[a], s.px[b], s.px[c]}, {s.py[a], s.py[b], s.py[c]}};
    sPortMapTris.push_back(t);
  }
};

int PortMapHitAt(float gx, float gy) {
  // Under the pointer: inside a triangle for map areas, inside the rect for
  // universe hexes. Of several, the smallest rect wins (a room drawn over a
  // bigger one).
  std::vector< bool > inTri;
  if (!sPortMapTris.empty()) {
    inTri.assign(sPortMapHits.size(), false);
    for (const PortMapTri& t : sPortMapTris) {
      if (!inTri[t.hit] && PortMapInTri(t, gx, gy)) {
        inTri[t.hit] = true;
      }
    }
  }
  int best = -1;
  float bestArea = 0.f;
  for (int i = 0; i < static_cast< int >(sPortMapHits.size()); ++i) {
    const PortMapHit& h = sPortMapHits[i];
    if (gx < h.x0 || gx > h.x1 || gy < h.y0 || gy > h.y1) {
      continue;
    }
    if (!inTri.empty() && !inTri[i]) {
      continue;
    }
    const float area = (h.x1 - h.x0) * (h.y1 - h.y0);
    if (best < 0 || area < bestArea) {
      best = i;
      bestArea = area;
    }
  }
  return best;
}

} // namespace

void CAutoMapper::PortRecordPointerHits(const CStateManager& mgr, const CTransform4f& planeXf,
                                        const CTransform4f& mapXf) const {
  sPortMapHits.clear();
  sPortMapTris.clear();
  const bool universe = IsInMapperState(kAMS_MapScreenUniverse);
  if (!PortPointer::Enabled() || (!universe && !IsInMapperState(kAMS_MapScreen))) {
    return;
  }
  sPortMapHitsUniverse = universe;

  // The pan's scale: the screen distance of yScale along the camera's up.
  const float camAngleRad = xa8_renderState0.x1c_camAngle * (1.f / 360.f) * (2.f * M_PIF);
  const float yScale =
      xa8_renderState0.x18_camDist / static_cast< float >(tan(M_PIF / 2.f - 0.5f * camAngleRad));
  const CMatrix3f camRot = xa8_renderState0.x8_camOrientation.BuildTransform();
  const CVector3f& focus = xa8_renderState0.x20_areaPoint;
  float ax, ay, bx, by;
  if (PortGuiHit::Project(planeXf, focus, ax, ay) &&
      PortGuiHit::Project(planeXf, focus + yScale * camRot.GetColumn(2), bx, by)) {
    const float d = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    if (d > 1.f) {
      sPortMapViewPx = 2.f * d;
    }
  }

  if (universe) {
    const CMapUniverse* mapu = x8_mapu.GetObject();
    for (int w = 0; w < static_cast< int >(mapu->GetNumMapWorldDatas()); ++w) {
      const CMapUniverse::CMapWorldData& mwData = mapu->GetMapWorldData(w);
      if (!gpGameState->StateForWorld(mwData.GetWorldAssetId())
               .GetMapWorldInfo()
               .GetPtr()
               ->IsAnythingSet()) {
        continue;
      }
      for (int i = 0; i < static_cast< int >(mwData.GetNumMapAreaDatas()); ++i) {
        float gx, gy;
        if (PortGuiHit::Project(planeXf, mwData.GetMapAreaData(i).GetTransform().GetTranslation(),
                                gx, gy)) {
          PortMapHit h = {w,
                          i,
                          gx - kPortMapHexReach,
                          gy - kPortMapHexReach,
                          gx + kPortMapHexReach,
                          gy + kPortMapHexReach,
                          gx,
                          gy};
          sPortMapHits.push_back(h);
        }
      }
    }
    return;
  }

  if (x24_world == nullptr) {
    return;
  }
  const CMapWorld* mw = x24_world->IGetMapWorld();
  const CMapWorldInfo& mwInfo =
      *gpGameState->StateForWorld(x24_world->IGetWorldAssetId()).GetMapWorldInfo().GetPtr();
  rstl::vector< int > areas = mw->GetVisibleAreas(*x24_world, mwInfo);
  for (int i = 0; i < areas.size(); ++i) {
    const int aid = areas[i];
    CMapArea* mapa = mw->GetMapArea(aid);
    const CTransform4f model = mapXf * mapa->GetAreaPostTransform(*x24_world, aid);
    const CAABox& box = mapa->GetBoundingBox();
    PortMapHit h = {aid, 0, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    bool any = false;
    for (int c = 0; c < 8; ++c) {
      float gx, gy;
      if (!PortGuiHit::Project(model, box.GetPoint(c), gx, gy)) {
        continue;
      }
      if (!any) {
        h.x0 = h.x1 = gx;
        h.y0 = h.y1 = gy;
        any = true;
      } else {
        h.x0 = rstl::min_val(h.x0, gx);
        h.x1 = rstl::max_val(h.x1, gx);
        h.y0 = rstl::min_val(h.y0, gy);
        h.y1 = rstl::max_val(h.y1, gy);
      }
    }
    if (!any || !PortGuiHit::Project(model, mapa->GetAreaCenterPoint(), h.cx, h.cy)) {
      continue;
    }
    sPortMapHits.push_back(h);

    PortMapTriSink sink;
    sink.hit = static_cast< int >(sPortMapHits.size()) - 1;
    const uint numVerts = mapa->GetVertexCount();
    const CVector3f* verts = mapa->GetVertices();
    sink.px.resize(numVerts);
    sink.py.resize(numVerts);
    sink.ok.resize(numVerts);
    for (uint v = 0; v < numVerts; ++v) {
      sink.ok[v] = PortGuiHit::Project(model, verts[v], sink.px[v], sink.py[v]);
    }
    for (int s = 0; s < mapa->GetNumSurfaces(); ++s) {
      mapa->GetSurface(s).PortForEachTriangle(&PortMapTriSink::Add, &sink);
    }
  }
}

// Drag rotates (left mouse, one finger) or pans (right/middle, two fingers; the
// finger gestures swap with map_swap_fingers), the wheel and pinch zoom, a
// click picks an area or world and a click on the picked one presses A, a right
// click presses B. Behind the gates of ProcessMapScreenInput, so hints and leave
// rules are unchanged.
#ifdef TARGET_PC
// Port: the map legend, the disc's text plus a line for each pickup shape in
// use and the marked-room colour. Rebuilt when the settings behind it change.
uint sPortLegendKey = ~0u;

// The swatches are loaded before the text is laid out: text drops an image that
// is not loaded yet, and retail's legend images come with the frame's assets.
// The tokens live for the whole run. The disc's own legend needs none of them,
// so they are only asked for while the port's own entries are shown.
bool PortLegendReady() {
  static TToken< CTexture >* tokens[4] = {};
  if (!PortDebug::MapExtraUi()) {
    return true;
  }
  if (tokens[0] == nullptr) {
    for (int i = 0; i < 4; ++i) {
      tokens[i] = new TToken< CTexture >(
          gpSimplePool->GetObj(SObjectTag('TXTR', PortCustomRes::kLegendDotTxtr + i)));
      tokens[i]->Lock();
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (!tokens[i]->IsLoaded()) {
      return false;
    }
  }
  return true;
}

uint PortLegendKey() {
  const uint mode = static_cast< uint >(PortMapPickups::Mode());
  return mode | (PortDebug::MapPickupKinds() ? 8u : 0u) | (PortDebug::MapExtraUi() ? 16u : 0u);
}

rstl::wstring PortLegendBase() {
  if (!PortDisc::IsTrilogy(PortDisc::Current()) || gpStringTable->GetStringCount() <= 0x7a + 6) {
    return rstl::wstring(gpStringTable->GetString(0x31));
  }
  // Trilogy builds it from lines that say ??? for a door colour whose beam or
  // Missiles Samus doesn't have yet (@ fn_80027BF8). Its lines are kept past
  // 1.00's end of STRG_Main, Trilogy's 0x70 at 0x76.
  const CPlayerState& state = *gpGameState->PlayerState();
  const auto line = [](int label, bool have, int name) {
    rstl::wstring out(L"\n", -1);
    out.append(gpStringTable->GetString(label + 6), -1);
    out.append(gpStringTable->GetString((have ? name : 0x7a) + 6), -1);
    return out;
  };
  // The Frigate's map says ??? for Missiles whatever Samus has.
  const bool missiles = gpGameState->CurrentWorldAssetId() != 0x158EFE17 &&
                        state.HasPowerUp(CPlayerState::kIT_Missiles);
  rstl::wstring text(gpStringTable->GetString(0x70 + 6));
  text.append(line(0x71, state.HasPowerUp(CPlayerState::kIT_WaveBeam), 0x72));
  text.append(line(0x73, state.HasPowerUp(CPlayerState::kIT_IceBeam), 0x74));
  text.append(line(0x75, state.HasPowerUp(CPlayerState::kIT_PlasmaBeam), 0x76));
  text.append(line(0x77, missiles, 0x78));
  text.append(L"\n", -1);
  text.append(gpStringTable->GetString(0x79 + 6), -1);
  return text;
}

rstl::wstring PortLegendText() {
  rstl::wstring text(PortLegendBase());
  if (!PortDebug::MapExtraUi()) {
    return text;
  }
  const PortDebug::EMapPickups mode = PortMapPickups::Mode();
  // Entries go two to a line so the legend stays clear of the Key/Legend prompt.
  // The GameCube disc's legend ends in a newline: the first entry starts its
  // row there rather than leaving a blank one above.
  int entries = 0;
  auto entry = [&](uint32_t id, const wchar_t* label) {
    if (entries % 2 != 0) {
      text.append(L"   ", -1);
    } else if (entries != 0 || text.size() == 0 || text[text.size() - 1] != L'\n') {
      text.append(L"\n", -1);
    }
    ++entries;
    text.append(L"&image=", -1);
    text.append(CStringExtras::ConvertToUNICODE(
        rstl::string(CBasics::Stringize("SI, 0.6875,0.6875,%8.8X", id))));
    text.append(L"; - ", -1);
    text.append(label, -1);
  };
  if (mode == PortDebug::kMapPickups_Seen || mode == PortDebug::kMapPickups_All) {
    if (PortDebug::MapPickupKinds()) {
      entry(PortCustomRes::kLegendDotTxtr, L"Major");
      entry(PortCustomRes::kLegendRingTxtr, L"Minor");
      entry(PortCustomRes::kLegendCrossTxtr, L"Collected");
    } else {
      entry(PortCustomRes::kLegendDotTxtr, L"Pickup");
    }
  }
  entry(PortCustomRes::kLegendMarkTxtr, L"Marked");
  return text;
}

// Port: the entries above make the legend taller than the disc's own, whose rows
// the pane is sized for, and the rows below the fold would run over the
// Key/Legend prompt under it. The legend is drawn scaled by the row ratio, which
// shrinks its text and its line spacing together (the icons in it scale with
// them, so the legend keeps its proportions), and the block ends where the
// disc's does. A text pane is drawn scaled by its dimensions over the font's
// extents (CGuiTextPane::Draw) and its glyphs start at the pane's own top-left
// corner, so the pivot takes back what the smaller pane leaves behind and the
// text keeps its place.
struct SPortLegendFit {
  CGuiTextPane* pane = nullptr;
  CVector2f dims = CVector2f(0.f, 0.f);
  CVector3f pivot = CVector3f::Zero();
  int retailRows = 0;
  int rows = 0;
};
SPortLegendFit sPortLegendFit;

// A trailing newline's empty row is not counted: the GameCube disc's legend
// ends in one, and the Key/Legend prompt sits where that row would be.
static int PortTextRows(const rstl::wstring& text) {
  int rows = 1;
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == L'\n') {
      ++rows;
    }
  }
  return rows;
}

// Rows of the disc's own legend, which its pane is sized for (STRG 0x31): read
// from the string, so a disc whose legend is laid out differently still fits.
static int PortRetailLegendRows() {
  return PortTextRows(rstl::wstring(gpStringTable->GetString(0x31)));
}

void PortSetLegend(CGuiTextPane* legend, const rstl::wstring& text) {
  legend->TextSupport().SetText(text);
  if (sPortLegendFit.pane != legend) {
    sPortLegendFit.pane = legend;
    sPortLegendFit.dims = legend->GetDimensions();
    sPortLegendFit.pivot = legend->GetScaleCenter();
    sPortLegendFit.retailRows = PortRetailLegendRows();
    sPortLegendFit.rows = 0;
  }
  const int rows = PortTextRows(text);
  if (rows == sPortLegendFit.rows) {
    return;
  }
  sPortLegendFit.rows = rows;
  const int base = sPortLegendFit.retailRows;
  const float scale =
      rows > base ? static_cast< float >(base) / static_cast< float >(rows) : 1.f;
  const CVector2f dims(sPortLegendFit.dims.GetX() * scale, sPortLegendFit.dims.GetY() * scale);
  legend->SetDimensions(dims, true);
  // The glyphs start at the pane's top-left corner: the corner moves right and
  // down as the pane shrinks, so the pivot moves the other way by the same
  // amount and the corner (and with it the text) stays where the disc put it.
  legend->ScaleCenter() =
      CVector3f(sPortLegendFit.pivot.GetX() - 0.5f * (sPortLegendFit.dims.GetX() - dims.GetX()),
                sPortLegendFit.pivot.GetY(),
                sPortLegendFit.pivot.GetZ() + 0.5f * (sPortLegendFit.dims.GetY() - dims.GetY()));
}

// Port: "X Mark room" borrows the disc's hint pane, which the disc fills with a
// room's hint text and draws under the middle of the map. The prompt moves to
// the map's upper right: its text (7.6 by 2.0 world units wide and tall, centred
// in the pane and starting at its top) lands at x +5.7..+13.3, z +6.6..+4.7,
// below the room title's line and clear of the legend on the left and the
// prompts along the bottom. The offset is in the pane's parent (the head
// widget, unrotated); the frame's own transform comes back while the pane holds
// anything else, or nothing.
const CVector3f kPortMarkHintOffset(9.5f, 0.f, 15.58f);
struct SPortHintPane {
  CGuiTextPane* pane = nullptr;
  CTransform4f home = CTransform4f::Identity();
  bool placed = false;
};
SPortHintPane sPortHintPane;

// Port: both caches above (and the legend's fit) are keyed on the pane, and a
// released pane can come back at the same address when the frame is loaded
// again - the map screen leaves and is re-entered, and the pause screen's pools
// hand out what was just freed. A stale cache would keep the disc's geometry
// from being re-taken, would leave the last row count in place (skipping the
// fit on the reopened frame) and would keep a placement flag set, so the prompt
// of a reopened frame would never be placed. Cleared when the map frame is
// dropped and when it is loaded again, before anything reads them.
void PortForgetMapPanes() {
  sPortLegendFit.pane = nullptr;
  sPortLegendFit.rows = 0;
  sPortLegendFit.retailRows = 0;
  sPortHintPane.pane = nullptr;
  sPortHintPane.placed = false;
}

void PortPlaceMarkHint(CGuiTextPane* pane, bool show) {
  if (pane == nullptr) {
    return;
  }
  if (sPortHintPane.pane != pane) {
    sPortHintPane.pane = pane;
    sPortHintPane.home = pane->GetTransform();
    sPortHintPane.placed = false;
  }
  if (show == sPortHintPane.placed) {
    return;
  }
  sPortHintPane.placed = show;
  pane->SetO2PTransform(show ? CTransform4f::Translate(kPortMarkHintOffset) * sPortHintPane.home
                             : sPortHintPane.home);
}

// Port: only the prompt's own glyphs take a click. The disc's hint pane lays one
// line at its top and its own rect is far wider than "X Mark room": a hit on the
// empty part of the pane would swallow a click meant for the map and mark the
// room under it. The pane records its glyphs (port_pointer.h LastBounds).
static bool PortMarkPromptUnderPointer(const CGuiTextPane* pane) {
  const PortPointer::Recognizer& gestures = PortPointer::Gestures();
  PortPointer::Click click;
  float gx = 0.f, gy = 0.f;
  PortPointerMath::Rect glyphs;
  return gestures.PeekClick(click) && PortPointer::WindowToGui(click.x, click.y, gx, gy) &&
         PortGuiHit::LastBounds(pane, glyphs) && glyphs.Contains(gx, gy);
}

// Port: the map screen, with the player in control and no room hint text of the
// disc's up, is when "X Mark room" takes the disc's hint pane.
bool CAutoMapper::PortShowsMarkPrompt() {
  if (!PortDebug::MapExtraUi() || x1bc_state != kAMS_MapScreen || x24_world == nullptr ||
      x328_ != 0 || !IsInPlayerControlState()) {
    return false;
  }
  return !(x78_areaHintDesc.valid() && x78_areaHintDesc->TryCache());
}
#endif

void CAutoMapper::PortPointerInput(CFinalInput& input, const CStateManager& mgr) {
  const bool universe = x1bc_state == kAMS_MapScreenUniverse;
  if (sPortMapGlide && sPortMapGlideUniverse != universe) {
    PortMapStopGlide();
  }
  if ((!universe && x1bc_state != kAMS_MapScreen) || !IsInPlayerControlState() ||
      IsRenderStateInterpolating() || !PortPointerMenu::Begin()) {
    PortMapStopGlide();
    return;
  }
  if (PortPointerMenu::TakeBack() && x328_ == 0) {
    PortPointerMenu::PressB(input);
  }
  if (x32c_loadingDummyWorld || !x1e0_hintSteps.empty() || x328_ != 0) {
    PortMapStopGlide();
    return;
  }

  PortPointerMenu::ViewMotion m;
  if (PortPointerMenu::TakeViewMotion(m)) {
    float panX = m.panX;
    float panY = m.panY;
    float rotX = m.rotateX;
    float rotY = m.rotateY;
    const bool swapFingers = PortPointer::TouchIsLast() && PortPointer::MapSwapFingers();
    if (swapFingers) {
      std::swap(panX, rotX);
      std::swap(panY, rotY);
    }
    if (panX != 0.f || panY != 0.f) {
      PortMapStopGlide();
      PortDebug::AddMapPan(panX, panY, sPortMapViewPx, 250);
    }
    if (rotX != 0.f || rotY != 0.f) {
      CEulerAngles eulers = CEulerAngles::FromQuaternion(xa8_renderState0.x8_camOrientation);
      CAbsAngle angX = CAbsAngle::FromRadians(eulers.GetX());
      CAbsAngle angZ = CAbsAngle::FromRadians(eulers.GetZ());
      angZ -= CRelAngle::FromDegrees(rotX * kPortMapDegPerPx);
      angX -= CRelAngle::FromDegrees(rotY * kPortMapDegPerPx);
      float angXDeg = angX.AsDegrees();
      if (angXDeg > 180.f)
        angXDeg -= 360.f;
      angX = CAbsAngle::FromDegrees(CMath::Clamp(gpTweakAutoMapper->x14_minCamRotateX, angXDeg,
                                                 gpTweakAutoMapper->x18_maxCamRotateX));
      xa8_renderState0.x8_camOrientation = CQuaternion::YXZRotation(
          CRelAngle(0.f), CRelAngle(angX.AsRadians()), CRelAngle(angZ.AsRadians()));
    }
    if (m.twist != 0.f) {
      // A two-finger twist turns the map, as on the Android overlay.
      PortDebug::AddMapRotate(m.twist);
    }
    if (m.zoom != 0.f) {
      PortDebug::AddMapZoom(static_cast< float >(pow(2.0, 0.5 * m.zoom)));
    }
  }
  // A held pan drag (right or middle button, two fingers) keeps the map from
  // drifting back to its area. One finger rotates, as a left drag does (it pans
  // and two fingers rotate with the swap setting on).
  PortPointer::Recognizer& g = PortPointer::Gestures();
  const bool touchPan = PortPointer::MapSwapFingers() ? g.FingerCount() == 1 : g.FingerCount() >= 2;
  if (g.Dragging() && (touchPan ||
                       (g.Buttons() & (PortPointer::kButtonRight | PortPointer::kButtonMiddle)))) {
    PortMapStopGlide();
    PortDebug::AddMapPan(0.f, 0.f, sPortMapViewPx, 250);
  }

  // A tap on the corner A prompt (icon or label) presses A, as the button does.
  if (x2c_frmeInitialized != NULL && (universe || HasCurrentMapUniverseWorld(mgr))) {
    const CGuiWidget* icon = x2c_frmeInitialized->FindWidget("textpane_right1");
    const CGuiWidget* label = x2c_frmeInitialized->FindWidget("textpane_right");
    if ((icon != NULL && PortPointerMenu::TakeClickOn(icon)) ||
        (label != NULL && PortPointerMenu::TakeClickOn(label))) {
      PortMapStopGlide();
      PortPointerMenu::PressA(input);
    }
  }

  // A click on the mark prompt presses X, as the button does. It takes only a
  // click on the prompt's own glyphs: the disc's hint pane is far wider than it,
  // and a hit on the empty part of the pane would swallow a click meant for the
  // map. Without the prompt (the extra map UI off, or the pane showing a disc
  // hint) the pane is not a target at all, so nothing invisible marks a room.
  if (!universe && x2fc_textpane_hint != NULL && PortShowsMarkPrompt() &&
      PortMarkPromptUnderPointer(x2fc_textpane_hint) &&
      PortPointerMenu::TakeClickOn(x2fc_textpane_hint)) {
    PortPointerMenu::PressX(input);
  }

  float gx, gy;
  int count;
  if (sPortMapHitsUniverse == universe && PortPointerMenu::TakeClickPoint(gx, gy, count)) {
    const int hit = PortMapHitAt(gx, gy);
    if (hit >= 0) {
      const PortMapHit& h = sPortMapHits[hit];
      const int picked = sPortMapGlide ? sPortMapGlideId : universe ? x9c_worldIdx
                                                                     : xa0_curAreaId.value;
      // The area glided to counts as picked even when the closest-area search
      // settled on an overlapping one.
      const bool landed = !universe && !sPortMapGlide && h.id == sPortMapGlideId;
      if (h.id == picked && universe && sPortMapGlide) {
        // A enters the world under the area point: wait for the glide to land.
        sPortMapEnter = true;
      } else if (h.id == picked || landed) {
        PortPointerMenu::PressA(input);
      } else {
        sPortMapGlide = true;
        sPortMapGlideUniverse = universe;
        sPortMapGlideId = h.id;
        sPortMapGlideTo = universe ? x8_mapu.GetObject()
                                         ->GetMapWorldData(h.id)
                                         .GetMapAreaData(h.sub)
                                         .GetTransform()
                                         .GetTranslation()
                                   : GetAreaPointOfInterest(mgr, h.id);
      }
    }
  }

  if (ControlMapper::GetAnalogInput(ControlMapper::kC_MapMoveForward, input) > 0.f ||
      ControlMapper::GetAnalogInput(ControlMapper::kC_MapMoveBack, input) > 0.f ||
      ControlMapper::GetAnalogInput(ControlMapper::kC_MapMoveLeft, input) > 0.f ||
      ControlMapper::GetAnalogInput(ControlMapper::kC_MapMoveRight, input) > 0.f) {
    PortMapStopGlide();
  }
  if (sPortMapEnter && !sPortMapGlide) {
    // Landed last frame, so x9c_worldIdx has caught up.
    sPortMapEnter = false;
    if (universe && x9c_worldIdx == sPortMapGlideId) {
      PortPointerMenu::PressA(input);
    }
  }
  if (sPortMapGlide) {
    CVector3f& point = xa8_renderState0.x20_areaPoint;
    const CVector3f toTarget = sPortMapGlideTo - point;
    const float t = 1.f - static_cast< float >(exp(-12.0 * input.Time()));
    if (toTarget.Magnitude() < 0.01f) {
      point = sPortMapGlideTo;
      sPortMapGlide = false;
    } else {
      point = point + t * toTarget;
      // Hold off ProcessMapPanInput's drift back to the current area.
      PortDebug::AddMapPan(0.f, 0.f, sPortMapViewPx, 100);
    }
  }
}
#endif
