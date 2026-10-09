#include "MetroidPrime/COptionsScreen.hpp"
#include "MetroidPrime/CQuitGameScreen.hpp"
#include "MetroidPrime/SOptionsFrontEndFrame.hpp"

#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiSliderGroup.hpp"
#include "GuiSys/CGuiTableGroup.hpp"
#include "GuiSys/CGuiTextPane.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Input/CFinalInput.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "MetroidPrime/CArchitectureQueue.hpp"
#include "MetroidPrime/CGameCubeDoll.hpp"
#include "MetroidPrime/CSaveGameScreen.hpp"
#include "MetroidPrime/Cameras/CCameraFilterPass.hpp"
#include "MetroidPrime/Decode.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/RumbleFxTable.hpp"
#include "MetroidPrime/Tweaks/CTweakGuiColors.hpp"
#include "rstl/StringExtras.hpp"
#include "rstl/math.hpp"
#include <stdio.h>

#ifdef TARGET_PC
#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiModel.hpp"
#include "port_debug.h"
#include "port_map_pickups.h"
#include "port_skip_cutscenes.h"
#include <math.h>
#endif

static const int skQuitTitles[] = {24, 25, 26, 27, 28};

CQuitGameScreen::CQuitGameScreen(EQuitType type)
: x0_type(type)
, x4_frame(gpSimplePool->GetObj("FRME_QuitScreen"))
, x10_loadedFrame(nullptr)
, x14_tablegroup_quitgame(nullptr)
, x18_action(kQA_None) {
  x4_frame.Lock();
}

void CQuitGameScreen::ProcessUserInput(const CFinalInput& input) {
  if (input.ControllerNumber() != 0) {
    return;
  }
  if (x10_loadedFrame != nullptr) {
    x10_loadedFrame->ProcessUserInput(input);
    if (input.PB() && x0_type != kQT_ContinueFromLastSave) {
      x18_action = kQA_No;
    }
  }
}

void CQuitGameScreen::Draw() const {
  if (x0_type == kQT_QuitGame) {
    CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Blend, CCameraFilterPass::kFS_Fullscreen,
                                  CColor::Black().WithAlphaOf(0.5f), nullptr, 1.f);
  }
  const float offsets[] = {0.f, 1.6f, 1.f, 0.f, 1.f};
  if (x10_loadedFrame != nullptr) {
    x10_loadedFrame->Draw(CGuiWidgetDrawParms(1.f, CVector3f(0.f, 0.f, offsets[x0_type])));
  }
}

EQuitAction CQuitGameScreen::Update(float dt) {
  if (x10_loadedFrame == nullptr && x4_frame.TryCache()) {
    FinishedLoading();
  }
  return x18_action;
}

void CQuitGameScreen::DoAdvance(CGuiTableGroup* caller) {
  if (caller->GetUserSelection() == 0) {
    CSfxManager::SfxStart(0x598, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
    x18_action = kQA_Yes;
  } else {
    CSfxManager::SfxStart(0x597, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
    x18_action = kQA_No;
  }
}

void CQuitGameScreen::DoSelectionChange(CGuiTableGroup* caller, int oldSel) {
  SetColors();
  CSfxManager::SfxStart(0x590, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                        CSfxManager::kAllAreas);
}

void CQuitGameScreen::FinishedLoading() {
  x10_loadedFrame = x4_frame.GetObject();
  x14_tablegroup_quitgame =
      static_cast< CGuiTableGroup* >(x10_loadedFrame->FindWidget("tablegroup_quitgame"));
  x14_tablegroup_quitgame->SetVertical(false);
  x14_tablegroup_quitgame->SetMenuAdvanceCallback(
      TFunctor1FromMethod< CQuitGameScreen, CGuiTableGroup* const >::Make(
          *this, &CQuitGameScreen::DoAdvance));
  x14_tablegroup_quitgame->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< CQuitGameScreen, CGuiTableGroup* const, const int >::Make(
          *this, &CQuitGameScreen::DoSelectionChange));
  CGuiTextPane* title = static_cast< CGuiTextPane* >(x10_loadedFrame->FindWidget("textpane_title"));
  title->TextSupport().SetText(rstl::wstring_l(gpStringTable->GetString(skQuitTitles[x0_type])));
  CGuiTextPane* yes = static_cast< CGuiTextPane* >(x10_loadedFrame->FindWidget("textpane_yes"));
  yes->TextSupport().SetText(rstl::wstring_l(gpStringTable->GetString(22)));
  CGuiTextPane* no = static_cast< CGuiTextPane* >(x10_loadedFrame->FindWidget("textpane_no"));
  no->TextSupport().SetText(rstl::wstring_l(gpStringTable->GetString(23)));
  const int defaults[] = {1, 0, 1, 1, 0};
  x14_tablegroup_quitgame->SetUserSelection(defaults[x0_type]);
  SetColors();
}

void CQuitGameScreen::SetColors() {
  const CColor selected(uchar(200), uchar(200), uchar(200), uchar(255));
  const CColor unselected(uchar(50), uchar(50), uchar(50), uchar(255));
  const int selection = x14_tablegroup_quitgame->GetUserSelection();
  for (int i = 0; i < 2; ++i) {
    CGuiWidget* worker = x14_tablegroup_quitgame->GetWorkerWidget(i);
    worker->SetColor(i == selection ? selected : unselected);
  }
}

static void SetTextPanePair(CGuiFrame* frame, const char* name, const wchar_t* text) {
  CGuiTextPane* pane = static_cast< CGuiTextPane* >(frame->FindWidget(name));
  pane->TextSupport().SetText(rstl::wstring(text));
  CGuiTextPane* shadow =
      static_cast< CGuiTextPane* >(frame->FindWidget(CBasics::Stringize("%sb", name)));
  shadow->TextSupport().SetText(rstl::wstring(text));
}

enum EOptionType { kOT_Float, kOT_DoubleEnum, kOT_TripleEnum, kOT_RestoreDefaults };
struct SGameOption {
  EGameOption option;
  int stringId;
  float minVal;
  float maxVal;
  float increment;
  EOptionType type;
};

static const SGameOption skVisorOptions[] = {
    {kGO_VisorOpacity, 21, 0.f, 255.f, 1.f, kOT_Float},
    {kGO_HelmetOpacity, 22, 0.f, 255.f, 1.f, kOT_Float},
    {kGO_HUDLag, 23, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_HintSystem, 24, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
static const SGameOption skDisplayOptions[] = {
    {kGO_ScreenBrightness, 25, 0.f, 8.f, 1.f, kOT_Float},
    {kGO_ScreenOffsetX, 26, -30.f, 30.f, 1.f, kOT_Float},
    {kGO_ScreenOffsetY, 27, -30.f, 30.f, 1.f, kOT_Float},
    {kGO_ScreenStretch, 28, -10.f, 10.f, 1.f, kOT_Float},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
static const SGameOption skSoundOptions[] = {
    {kGO_SFXVolume, 29, 0.f, 127.f, 1.f, kOT_Float},
    {kGO_MusicVolume, 30, 0.f, 127.f, 1.f, kOT_Float},
    {kGO_SoundMode, 31, 0.f, 2.f, 1.f, kOT_TripleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
static const SGameOption skControllerOptions[] = {
    {kGO_ReverseYAxis, 32, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_Rumble, 33, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_SwapBeamControls, 34, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
struct SOptionCategory {
  int count;
  const SGameOption* options;
};
static SOptionCategory skGameOptions[] = {
    {5, skVisorOptions},      {5, skDisplayOptions}, {4, skSoundOptions},
    {4, skControllerOptions}, {0, nullptr},
};

#ifdef TARGET_PC
// Port settings (the ones also in the debug overlay) shown on the pause screen,
// after each category's retail rows. The left table has no free category (the
// fifth is Quit Game), so the right table scrolls instead. The title-screen
// Options keeps the retail tables: its frame has no scrolling. These rows have
// no STRG entries, so their labels live here.
enum EPortOption {
  kPO_AspectRatio = kGO_RestoreDefaults + 1,
  kPO_WidescreenHUD,
  kPO_TwinStick,
  kPO_AimSpeed,
  kPO_FastMorph,
  kPO_LockOnToggle,
  kPO_StickyCharge,
  kPO_Fov,
  kPO_AntiAliasing,
  kPO_HudScale,
  kPO_HideHelmet,
  kPO_HideVisorEffects,
  kPO_SpeedrunTimer,
  kPO_RevealMap,
  kPO_CrosshairSize,
  kPO_SkippableCutscenes,
  kPO_MapPickups,
  kPO_ElevatorRide,
  kPO_RapidCharge,
  kPO_OriginalExperience,
};
#define PORT_OPTION(opt) static_cast< EGameOption >(opt)

// Aim speed steps: 900 px/s (the default) at 10, x9 per 10 steps, so 0 is
// 100 px/s and 20 is 8100 px/s (the overlay's slider covers 100-8100).
static const float kAimSpeedDefault = 900.f;
static const float kAimSpeedDefaultStep = 10.f;
static float AimSpeedFromStep(int step) {
  return kAimSpeedDefault * powf(9.f, (step - kAimSpeedDefaultStep) / 10.f);
}
static int AimSpeedToStep(float rate) {
  return static_cast< int >(
      floorf(kAimSpeedDefaultStep + 10.f * logf(rate / kAimSpeedDefault) / logf(9.f) + 0.5f));
}

static const SGameOption skPortVisorOptions[] = {
    {kGO_VisorOpacity, 21, 0.f, 255.f, 1.f, kOT_Float},
    {kGO_HelmetOpacity, 22, 0.f, 255.f, 1.f, kOT_Float},
    {kGO_HUDLag, 23, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_HintSystem, 24, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_HideHelmet), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_HideVisorEffects), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_RevealMap), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_MapPickups), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_SkippableCutscenes), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_ElevatorRide), -1, 0.f, 2.f, 1.f, kOT_TripleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
static const SGameOption skPortDisplayOptions[] = {
    {kGO_ScreenBrightness, 25, 0.f, 8.f, 1.f, kOT_Float},
    {kGO_ScreenOffsetX, 26, -30.f, 30.f, 1.f, kOT_Float},
    {kGO_ScreenOffsetY, 27, -30.f, 30.f, 1.f, kOT_Float},
    {kGO_ScreenStretch, 28, -10.f, 10.f, 1.f, kOT_Float},
    {PORT_OPTION(kPO_OriginalExperience), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_AspectRatio), -1, 0.f, 2.f, 1.f, kOT_TripleEnum},
    {PORT_OPTION(kPO_WidescreenHUD), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_HudScale), -1, PortDebug::kHudScaleMin, PortDebug::kHudScaleMax, 5.f, kOT_Float},
    {PORT_OPTION(kPO_Fov), -1, PortDebug::kFovMin, PortDebug::kFovMax, 1.f, kOT_Float},
    {PORT_OPTION(kPO_AntiAliasing), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_SpeedrunTimer), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
static const SGameOption skPortControllerOptions[] = {
    {kGO_ReverseYAxis, 32, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_Rumble, 33, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_SwapBeamControls, 34, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_TwinStick), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_AimSpeed), -1, 0.f, 20.f, 1.f, kOT_Float},
    {PORT_OPTION(kPO_CrosshairSize), -1, PortDebug::kCrosshairSizeMin,
     PortDebug::kCrosshairSizeMax, 5.f, kOT_Float},
    {PORT_OPTION(kPO_FastMorph), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_LockOnToggle), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_StickyCharge), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_RapidCharge), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
// The same without Stick Aim Speed, which only shows while Twin Stick is on.
static const SGameOption skPortControllerOptionsNoAim[] = {
    {kGO_ReverseYAxis, 32, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_Rumble, 33, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_SwapBeamControls, 34, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_TwinStick), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_CrosshairSize), -1, PortDebug::kCrosshairSizeMin,
     PortDebug::kCrosshairSizeMax, 5.f, kOT_Float},
    {PORT_OPTION(kPO_FastMorph), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_LockOnToggle), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_StickyCharge), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {PORT_OPTION(kPO_RapidCharge), -1, 0.f, 1.f, 1.f, kOT_DoubleEnum},
    {kGO_RestoreDefaults, 35, 0.f, 1.f, 1.f, kOT_RestoreDefaults},
};
static const SOptionCategory skPortControllerCategoryNoAim = {10, skPortControllerOptionsNoAim};
static SOptionCategory skPauseOptions[] = {
    {11, skPortVisorOptions},     {12, skPortDisplayOptions}, {4, skSoundOptions},
    {11, skPortControllerOptions}, {0, nullptr},
};

static bool IsPortOption(EGameOption option) { return option > kGO_RestoreDefaults; }

static const wchar_t* PortOptionTitle(EGameOption option) {
  switch (static_cast< int >(option)) {
  case kPO_AspectRatio:
    return L"Aspect Ratio";
  case kPO_WidescreenHUD:
    return L"Widescreen HUD";
  case kPO_TwinStick:
    return L"Twin Stick Aim";
  case kPO_AimSpeed:
    return L"Stick Aim Speed";
  case kPO_FastMorph:
    return L"Fast Morph";
  case kPO_LockOnToggle:
    return L"Toggle Lock-On";
  case kPO_StickyCharge:
    return L"Sticky Charge";
  case kPO_RapidCharge:
    return L"Remastered Charge";
  case kPO_Fov:
    return L"Field of View";
  case kPO_AntiAliasing:
    return L"Anti-Aliasing";
  case kPO_HudScale:
    return L"HUD Scale";
  case kPO_HideHelmet:
    return L"Hide Helmet";
  case kPO_HideVisorEffects:
    return L"Hide Visor Effects";
  case kPO_SpeedrunTimer:
    return L"In-Game Timer";
  case kPO_RevealMap:
    return L"Reveal Map";
  case kPO_MapPickups:
    return L"Pickup Dots";
  case kPO_CrosshairSize:
    return L"Crosshair Size";
  case kPO_SkippableCutscenes:
    return L"Skippable Cutscenes";
  case kPO_ElevatorRide:
    return L"Elevator Ride";
  case kPO_OriginalExperience:
    return L"Original Experience";
  default:
    return L"";
  }
}

static int GetPortOption(EGameOption option) {
  switch (static_cast< int >(option)) {
  case kPO_AspectRatio:
    return PortDebug::AspectMode();
  case kPO_WidescreenHUD:
    return PortDebug::HudWide() ? 1 : 0;
  case kPO_TwinStick:
    return PortDebug::PadTwinStick() ? 1 : 0;
  case kPO_AimSpeed:
    return AimSpeedToStep(PortDebug::StickAimRate());
  case kPO_FastMorph:
    return PortDebug::FastMorph() ? 1 : 0;
  case kPO_LockOnToggle:
    return PortDebug::LockOnToggle() ? 1 : 0;
  case kPO_StickyCharge:
    return PortDebug::StickyCharge() ? 1 : 0;
  case kPO_RapidCharge:
    return PortDebug::RapidCharge() ? 1 : 0;
  case kPO_Fov:
    return static_cast< int >(PortDebug::FirstPersonFov() + 0.5f);
  case kPO_AntiAliasing:
    return PortDebug::Msaa() > 1 ? 1 : 0;
  case kPO_HudScale:
    return PortDebug::HudScale();
  case kPO_HideHelmet:
    return PortDebug::HideHelmet() ? 1 : 0;
  case kPO_HideVisorEffects:
    return PortDebug::HideVisorEffects() ? 1 : 0;
  case kPO_SpeedrunTimer:
    return PortDebug::SpeedrunTimer() ? 1 : 0;
  case kPO_RevealMap:
    return PortDebug::RevealMap() ? 1 : 0;
  case kPO_MapPickups:
    // Randomized games force it on; show what is in effect.
    return PortMapPickups::Active() ? 1 : 0;
  case kPO_SkippableCutscenes:
    // Randomized games force it on; show what is in effect.
    return PortSkipCutscenes::Active() ? 1 : 0;
  case kPO_ElevatorRide:
    return PortDebug::ElevatorRide();
  case kPO_CrosshairSize:
    return PortDebug::CrosshairSize();
  case kPO_OriginalExperience:
    return PortDebug::OriginalExperience() ? 1 : 0;
  default:
    return 0;
  }
}

// Rows the Original experience overrides: they show the retail value and keep
// the saved one until it's turned off.
static bool IsOriginalLocked(EGameOption option) {
  if (!PortDebug::OriginalExperience()) {
    return false;
  }
  switch (static_cast< int >(option)) {
  case kPO_AspectRatio:
  case kPO_WidescreenHUD:
  case kPO_FastMorph:
  case kPO_LockOnToggle:
  case kPO_StickyCharge:
  case kPO_RapidCharge:
  case kPO_Fov:
  case kPO_AntiAliasing:
  case kPO_HudScale:
  case kPO_HideHelmet:
  case kPO_HideVisorEffects:
  case kPO_RevealMap:
  case kPO_SkippableCutscenes:
  case kPO_ElevatorRide:
    return true;
  default:
    return false;
  }
}

static void SetPortOption(EGameOption option, int value) {
  if (IsOriginalLocked(option)) {
    return;
  }
  switch (static_cast< int >(option)) {
  case kPO_AspectRatio:
    if (value >= PortDebug::kAspect_4_3 && value <= PortDebug::kAspect_Window) {
      PortDebug::SetAspectMode(static_cast< PortDebug::EAspectMode >(value));
    }
    break;
  case kPO_WidescreenHUD:
    PortDebug::SetHudWide(value > 0);
    break;
  case kPO_TwinStick:
    PortDebug::SetTwinStick(value > 0);
    break;
  case kPO_AimSpeed:
    PortDebug::SetStickAimRate(AimSpeedFromStep(value));
    break;
  case kPO_FastMorph:
    PortDebug::SetFastMorph(value > 0);
    break;
  case kPO_LockOnToggle:
    PortDebug::SetLockOnToggle(value > 0);
    break;
  case kPO_StickyCharge:
    PortDebug::SetStickyCharge(value > 0);
    break;
  case kPO_RapidCharge:
    PortDebug::SetRapidCharge(value > 0);
    break;
  case kPO_Fov:
    PortDebug::SetFirstPersonFov(static_cast< float >(value));
    break;
  case kPO_AntiAliasing:
    PortDebug::SetMsaa(value > 0 ? 4 : 1);
    break;
  case kPO_HudScale:
    PortDebug::SetHudScale(value);
    break;
  case kPO_HideHelmet:
    PortDebug::SetHideHelmet(value > 0);
    break;
  case kPO_SpeedrunTimer:
    PortDebug::SetSpeedrunTimer(value > 0);
    break;
  case kPO_HideVisorEffects:
    PortDebug::SetHideVisorEffects(value > 0);
    break;
  case kPO_RevealMap:
    PortDebug::SetRevealMap(value > 0);
    break;
  case kPO_MapPickups:
    PortDebug::SetMapPickups(value > 0);
    break;
  case kPO_SkippableCutscenes:
    PortDebug::SetSkippableCutscenes(value > 0);
    break;
  case kPO_ElevatorRide:
    PortDebug::SetElevatorRide(static_cast< PortDebug::EElevatorRide >(value));
    break;
  case kPO_CrosshairSize:
    PortDebug::SetCrosshairSize(value);
    break;
  case kPO_OriginalExperience:
    PortDebug::SetOriginalExperience(value > 0);
    break;
  default:
    break;
  }
}

static const SOptionCategory& GetOptionCategory(int category, bool frontEnd) {
  if (frontEnd) {
    return skGameOptions[category];
  }
  if (category == 3 && !PortDebug::PadTwinStick()) {
    return skPortControllerCategoryNoAim;
  }
  return skPauseOptions[category];
}
#else
static const SOptionCategory& GetOptionCategory(int category, bool frontEnd) {
  return skGameOptions[category];
}
#endif

int CGameOptions::GetOption(EGameOption option) {
#ifdef TARGET_PC
  if (IsPortOption(option)) {
    return GetPortOption(option);
  }
#endif
  const CGameOptions& options = gpGameState->GameOptions();
  switch (option) {
  case kGO_VisorOpacity:
    return options.x60_hudAlpha;
  case kGO_HelmetOpacity:
    return options.x64_helmetAlpha;
  case kGO_HUDLag:
    return options.GetHUDLag() ? 1 : 0;
  case kGO_HintSystem:
    return options.GetIsHintSystemEnabled() ? 1 : 0;
  case kGO_ScreenBrightness:
    return options.x48_screenBrightness;
  case kGO_ScreenOffsetX:
    return options.x4c_screenXOffset;
  case kGO_ScreenOffsetY:
    return options.x50_screenYOffset;
  case kGO_ScreenStretch:
    return options.x54_screenStretch;
  case kGO_SFXVolume:
    return options.x58_sfxVol;
  case kGO_MusicVolume:
    return options.x5c_musicVol;
  case kGO_SoundMode:
    return options.x44_soundMode;
  case kGO_ReverseYAxis:
    return options.GetInvertYAxis() ? 1 : 0;
  case kGO_Rumble:
    return options.GetIsRumbleEnabled() ? 1 : 0;
  case kGO_SwapBeamControls:
    return options.GetSwapBeamControls() ? 1 : 0;
  default:
    return 0;
  }
}

void CGameOptions::SetOption(EGameOption option, int value) {
#ifdef TARGET_PC
  if (IsPortOption(option)) {
    SetPortOption(option, value);
    return;
  }
#endif
  CGameOptions& options = gpGameState->GameOptions();
  switch (option) {
  case kGO_VisorOpacity:
    options.x60_hudAlpha = value;
    break;
  case kGO_HelmetOpacity:
    options.SetHelmetAlpha(value);
    break;
  case kGO_HUDLag:
    options.SetHUDLag(value > 0);
    break;
  case kGO_HintSystem:
    options.SetIsHintSystemEnabled(value > 0);
    break;
  case kGO_ScreenBrightness:
    options.SetScreenBrightness(value, true);
    break;
  case kGO_ScreenOffsetX:
    options.SetScreenPositionX(value, true);
    break;
  case kGO_ScreenOffsetY:
    options.SetScreenPositionY(value, true);
    break;
  case kGO_ScreenStretch:
    options.SetScreenStretch(value, true);
    break;
  case kGO_SFXVolume:
    options.SetSfxVolume(value, true);
    break;
  case kGO_MusicVolume:
    options.SetMusicVolume(value, true);
    break;
  case kGO_SoundMode:
    options.SetSurroundMode(static_cast< CAudioSys::ESurroundModes >(value), true);
    break;
  case kGO_ReverseYAxis:
    options.SetInvertYAxis(value > 0);
    break;
  case kGO_Rumble:
    options.SetIsRumbleEnabled(value > 0);
    break;
  case kGO_SwapBeamControls:
    options.ToggleControls(value > 0);
    break;
  default:
    break;
  }
}

void CGameOptions::TryRestoreDefaults(const CFinalInput& input, int category, int option,
                                      bool frontEnd) {
  const SOptionCategory& cat = GetOptionCategory(category, frontEnd);
  if (cat.count != 0 && cat.options[option].option == kGO_RestoreDefaults && input.PA()) {
    if (frontEnd) {
      CSfxManager::SfxStart(0x448, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
      CSfxManager::SfxStart(0x443, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
    } else {
      CSfxManager::SfxStart(0x598, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
    }
    CGameOptions& options = gpGameState->GameOptions();
    switch (category) {
    case 0:
      options.x60_hudAlpha = 255;
      options.SetHelmetAlpha(255);
      options.SetHUDLag(skDefaultHudLag);
      options.SetIsHintSystemEnabled(skDefaultHintSystem);
      break;
    case 1:
      options.SetScreenBrightness(4, true);
      options.SetScreenPositionX(0, true);
      options.SetScreenPositionY(0, true);
      options.SetScreenStretch(0, true);
      break;
    case 2:
      options.SetSfxVolume(127, true);
      options.SetMusicVolume(127, true);
      options.SetSurroundMode(CAudioSys::kSM_Stereo, true);
      break;
    case 3:
      options.SetInvertYAxis(skDefaultInvertY);
      options.SetIsRumbleEnabled(skDefaultRumble);
      options.ToggleControls(skDefaultSwapBeamsControls);
      break;
    default:
      break;
    }
#ifdef TARGET_PC
    // The port rows only show on the pause screen, so only it resets them.
    if (!frontEnd) {
      switch (category) {
      case 0:
        PortDebug::SetHideHelmet(false);
        PortDebug::SetHideVisorEffects(false);
        PortDebug::SetRevealMap(false);
        PortDebug::SetMapPickups(false);
        PortDebug::SetSkippableCutscenes(false);
        PortDebug::SetElevatorRide(PortDebug::kElevatorRide_Original);
        break;
      case 1:
        PortDebug::SetAspectMode(PortDebug::kAspect_4_3);
        PortDebug::SetHudWide(false);
        PortDebug::SetFirstPersonFov(PortDebug::kFovRetail);
        PortDebug::SetMsaa(1);
        PortDebug::SetHudScale(PortDebug::kHudScaleMax);
        PortDebug::SetSpeedrunTimer(false);
        PortDebug::SetOriginalExperience(false);
        break;
      case 3:
        PortDebug::SetTwinStick(false);
        PortDebug::SetStickAimRate(kAimSpeedDefault);
        PortDebug::SetCrosshairSize(PortDebug::kCrosshairSizeDefault);
        PortDebug::SetFastMorph(false);
        PortDebug::SetLockOnToggle(false);
        PortDebug::SetStickyCharge(false);
        PortDebug::SetRapidCharge(false);
        break;
      default:
        break;
      }
    }
#endif
  }
}

COptionsScreen::COptionsScreen(const CStateManager& mgr, CGuiFrame& frame,
                               const CStringTable& pauseStrg)
: CPauseScreenBase(mgr, frame, pauseStrg)
, x19c_quitGame(nullptr)
, x1a0_gameCube(rs_new CGameCubeDoll())
, x29c_optionAlpha(0.f)
, x2a0_24_inOptionBody(false)
#ifdef TARGET_PC
, mPortRowCount(0)
#endif
{
}

COptionsScreen::~COptionsScreen() {
  CSfxManager::SfxStop(x1a4_sliderSfx);
#ifdef TARGET_PC
  // Write any port setting changed here now, not only at exit: Android can
  // kill the app without running exit handlers.
  PortDebug::SaveSettingsNow();
#endif
}

bool COptionsScreen::VReady() const { return true; }

bool COptionsScreen::InputDisabled() const { return !x19c_quitGame.null(); }

void COptionsScreen::Update(float dt, CRandom16& rand, CArchitectureQueue& queue) {
  x1a8_rumble.Update(dt);
#ifdef TARGET_PC
  // Twin Stick (its row, Restore Defaults or F1) shows or hides the Stick Aim
  // Speed row: redraw the titles and keep the selection on a row.
  const uint rowCount = GetRightTableCount();
  if (rowCount != mPortRowCount) {
    if (mPortRowCount == 0) {
      // First frame: the rows were just set up.
    } else if (x10_mode == kM_RightTable) {
      SetRightTableSelection(x1c_rightSel, x1c_rightSel);
    } else {
      UpdateRightTitles();
    }
    mPortRowCount = rowCount;
  }
#endif
  CPauseScreenBase::Update(dt, rand, queue);
  const bool sliding = x18c_slidergroup_slider->GetState() != CGuiSliderGroup::kS_None;
  if (bool(x1a4_sliderSfx) != sliding) {
    if (sliding) {
      x1a4_sliderSfx = CSfxManager::SfxStart(0x5ab, 0x7f, 0x40, false, CSfxManager::kMedPriority,
                                             false, CSfxManager::kAllAreas);
    } else {
      CSfxManager::SfxStop(x1a4_sliderSfx);
      x1a4_sliderSfx.Clear();
    }
  }
  if (x2a0_24_inOptionBody) {
    x29c_optionAlpha = rstl::min_val(1.f, x29c_optionAlpha + 4.f * dt);
  } else {
    x29c_optionAlpha = rstl::max_val(0.f, x29c_optionAlpha - 4.f * dt);
  }
  if (close_enough(x29c_optionAlpha, 0.f)) {
    ResetOptionWidgetVisibility();
    x174_textpane_body->SetIsVisible(false);
  }
  const CColor color =
      gpTweakGuiColors->GetPauseItemAmberColor().WithAlphaModulatedBy(x29c_optionAlpha);
  x18c_slidergroup_slider->SetColor(color);
  x190_tablegroup_double->SetColor(color);
  x194_tablegroup_triple->SetColor(color);
  if (!x19c_quitGame.null()) {
    const EQuitAction action = x19c_quitGame->Update(dt);
    if (action == kQA_Yes) {
      queue.Push(MakeMsg::CreateQuitGameplay(kAMT_Game));
      CSfxManager::SetChannel(CSfxManager::kSC_Default);
      CSfxManager::SfxStart(0x58e, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
    } else if (action == kQA_No) {
      CSfxManager::SfxStart(0x58f, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
      x19c_quitGame = nullptr;
    }
  }
  x1a0_gameCube->Update(dt);
}

void COptionsScreen::Touch() {
  CPauseScreenBase::Touch();
  x1a0_gameCube->Touch();
}

void COptionsScreen::ProcessInput(const CFinalInput& input) {
  if (x19c_quitGame.null()) {
    CPauseScreenBase::ProcessInput(input);
    CGameOptions::TryRestoreDefaults(input, x70_tablegroup_leftlog->GetUserSelection(),
                                     x1c_rightSel, false);
    if (x70_tablegroup_leftlog->GetUserSelection() == 4 && input.PA()) {
      x19c_quitGame = rs_new CQuitGameScreen(kQT_QuitGame);
    }
  } else {
    x19c_quitGame->ProcessUserInput(input);
  }
}

void COptionsScreen::Draw(float transInterp, float totalAlpha, float yOff) const {
  CPauseScreenBase::Draw(transInterp, totalAlpha, yOff);
  x1a0_gameCube->Draw(transInterp * (1.f - x29c_optionAlpha));
  if (!x19c_quitGame.null()) {
    CGraphics::SetDepthRange(0.f, 0.001f);
    x19c_quitGame->Draw();
    CGraphics::SetDepthRange(0.f, 1.f);
  }
}

void COptionsScreen::VActivate() {
  for (int i = 0; i < 5; ++i) {
    CGuiTextPane* pane = xa8_textpane_categories[i];
    pane->TextSupport().SetText(rstl::wstring(xc_pauseStrg.GetString(i + 16)));
  }
  x178_textpane_title->TextSupport().SetText(rstl::wstring(xc_pauseStrg.GetString(15)));
  for (int i = ARRAY_SIZE(skGameOptions); i < xa8_textpane_categories.capacity(); ++i) {
    x70_tablegroup_leftlog->GetWorkerWidget(i)->SetIsSelectable(false);
  }
  x174_textpane_body->TextSupport().SetJustification(kJustification_Center);
  x174_textpane_body->TextSupport().SetVerticalJustification(kVerticalJustification_Bottom);
  CGuiTextPane* off = static_cast< CGuiTextPane* >(x190_tablegroup_double->GetWorkerWidget(0));
  off->TextSupport().SetText(rstl::wstring_l(xc_pauseStrg.GetString(95)));
  CGuiTextPane* on = static_cast< CGuiTextPane* >(x190_tablegroup_double->GetWorkerWidget(1));
  on->TextSupport().SetText(rstl::wstring_l(xc_pauseStrg.GetString(94)));
  CGuiTextPane* mono = static_cast< CGuiTextPane* >(x194_tablegroup_triple->GetWorkerWidget(0));
  mono->TextSupport().SetText(rstl::wstring_l(xc_pauseStrg.GetString(96)));
  CGuiTextPane* stereo = static_cast< CGuiTextPane* >(x194_tablegroup_triple->GetWorkerWidget(1));
  stereo->TextSupport().SetText(rstl::wstring_l(xc_pauseStrg.GetString(97)));
  CGuiTextPane* surround = static_cast< CGuiTextPane* >(x194_tablegroup_triple->GetWorkerWidget(2));
  surround->TextSupport().SetText(rstl::wstring_l(xc_pauseStrg.GetString(98)));
  x18c_slidergroup_slider->SetSelectionChangedCallback(
      TFunctor2FromMethod< COptionsScreen, CGuiSliderGroup* const, const float >::Make(
          *this, &COptionsScreen::OnSliderChanged));
  x190_tablegroup_double->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< COptionsScreen, CGuiTableGroup* const, const int >::Make(
          *this, &COptionsScreen::OnEnumChanged));
  x194_tablegroup_triple->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< COptionsScreen, CGuiTableGroup* const, const int >::Make(
          *this, &COptionsScreen::OnEnumChanged));
}

bool COptionsScreen::ShouldRightTableAdvance() { return false; }

bool COptionsScreen::ShouldLeftTableAdvance() {
  return x70_tablegroup_leftlog->GetUserSelection() != 4;
}

uint COptionsScreen::GetRightTableCount() const {
  return GetOptionCategory(x70_tablegroup_leftlog->GetUserSelection(), false).count;
}

void COptionsScreen::UpdateRightTable() {
  CPauseScreenBase::UpdateRightTable();
#ifdef TARGET_PC
  UpdateRightTitles();
#else
  const SOptionCategory& category =
      GetOptionCategory(x70_tablegroup_leftlog->GetUserSelection(), false);
  for (int i = 0; i < 5; ++i) {
    CGuiTextPane* pane = xd8_textpane_titles[i];
    if (i < category.count) {
      pane->TextSupport().SetText(
          rstl::wstring(xc_pauseStrg.GetString(category.options[i].stringId)));
    } else {
      pane->TextSupport().SetText(rstl::wstring_l(L""));
    }
  }
#endif
}

#ifdef TARGET_PC
// The titles of the rows in view, for categories that scroll. The title models
// behind them are a ring, rotated as in CLogBookScreen::UpdateRightTitles, so
// the base class's highlight (row index % 5) lands on the selected row.
void COptionsScreen::UpdateRightTitles() {
  const SOptionCategory& category =
      GetOptionCategory(x70_tablegroup_leftlog->GetUserSelection(), false);
  for (int i = 0; i < 5; ++i) {
    CGuiTextPane* pane = xd8_textpane_titles[i];
    const int row = x18_firstViewRightSel + i;
    if (row < category.count) {
      const SGameOption& opt = category.options[row];
      if (opt.stringId < 0) {
        pane->TextSupport().SetText(rstl::wstring_l(PortOptionTitle(opt.option)));
      } else {
        pane->TextSupport().SetText(rstl::wstring(xc_pauseStrg.GetString(opt.stringId)));
      }
    } else {
      pane->TextSupport().SetText(rstl::wstring_l(L""));
    }
  }
  const int firstMod = x18_firstViewRightSel % 5;
  for (int i = 0; i < x144_model_titles.size(); ++i) {
    CGuiModel* model = x144_model_titles[i];
    const int row = i >= firstMod ? -firstMod : 5 - firstMod;
    model->SetO2PTransform(CTransform4f::Translate(0.f, 0.f, x38_highlightPitch * row) *
                           model->GetTransform());
  }
}
#endif

void COptionsScreen::ChangedMode(EMode oldMode) {
#ifdef TARGET_PC
  // Going back to the left table resets the scroll position.
  if (x10_mode == kM_LeftTable) {
    UpdateRightTitles();
  }
#endif
  if (x10_mode == kM_RightTable) {
    x174_textpane_body->SetIsVisible(true);
    UpdateOptionView();
    x2a0_24_inOptionBody = true;
  } else {
    x2a0_24_inOptionBody = false;
  }
}

void COptionsScreen::RightTableSelectionChanged(int oldSel, int newSel) {
#ifdef TARGET_PC
  UpdateRightTitles();
#endif
  UpdateOptionView();
}

void COptionsScreen::ResetOptionWidgetVisibility() {
  x18c_slidergroup_slider->SetIsActive(false);
  x18c_slidergroup_slider->SetVisibility(false, kTM_Children);
  x190_tablegroup_double->SetIsVisible(false);
  x190_tablegroup_double->SetIsActive(false);
  x194_tablegroup_triple->SetIsActive(false);
  x194_tablegroup_triple->SetIsVisible(false);
}

void COptionsScreen::UpdateOptionView() {
  ResetOptionWidgetVisibility();
  const SOptionCategory& category =
      GetOptionCategory(x70_tablegroup_leftlog->GetUserSelection(), false);
  if (category.count == 0) {
    return;
  }
  const SGameOption& opt = category.options[x1c_rightSel];
#ifdef TARGET_PC
  const float zOff = x38_highlightPitch * (x1c_rightSel - x18_firstViewRightSel);
#else
  const float zOff = x38_highlightPitch * x1c_rightSel;
#endif
  switch (opt.type) {
  case kOT_Float:
    x18c_slidergroup_slider->SetIsActive(true);
    x18c_slidergroup_slider->SetVisibility(true, kTM_Children);
    x18c_slidergroup_slider->SetMinVal(opt.minVal);
    x18c_slidergroup_slider->SetMaxVal(opt.maxVal);
    x18c_slidergroup_slider->SetIncrement(opt.increment);
    x18c_slidergroup_slider->SetCurVal(CGameOptions::GetOption(opt.option));
    x18c_slidergroup_slider->SetLocalPosition(x3c_sliderStart + CVector3f(0.f, 0.f, zOff));
    break;
  case kOT_DoubleEnum:
    x190_tablegroup_double->SetUserSelection(CGameOptions::GetOption(opt.option));
    x190_tablegroup_double->SetIsVisible(true);
    x190_tablegroup_double->SetIsActive(true);
    UpdateSideTable(x190_tablegroup_double);
    x190_tablegroup_double->SetLocalPosition(x48_tableDoubleStart + CVector3f(0.f, 0.f, zOff));
    break;
  case kOT_TripleEnum:
#ifdef TARGET_PC
    // The triple labels are the sound modes' (strings 96-98) except on port rows.
    for (int i = 0; i < 3; ++i) {
      static const wchar_t* const kAspectLabels[] = {L"4:3", L"16:9", L"Window"};
      CGuiTextPane* label =
          static_cast< CGuiTextPane* >(x194_tablegroup_triple->GetWorkerWidget(i));
      static const wchar_t* const kElevatorLabels[] = {L"Original", L"Fast", L"Skip"};
      if (opt.option == PORT_OPTION(kPO_AspectRatio)) {
        label->TextSupport().SetText(rstl::wstring_l(kAspectLabels[i]));
      } else if (opt.option == PORT_OPTION(kPO_ElevatorRide)) {
        label->TextSupport().SetText(rstl::wstring_l(kElevatorLabels[i]));
      } else {
        label->TextSupport().SetText(rstl::wstring_l(xc_pauseStrg.GetString(96 + i)));
      }
    }
#endif
    x194_tablegroup_triple->SetUserSelection(CGameOptions::GetOption(opt.option));
    x194_tablegroup_triple->SetIsVisible(true);
    x194_tablegroup_triple->SetIsActive(true);
    UpdateSideTable(x194_tablegroup_triple);
    x194_tablegroup_triple->SetLocalPosition(x54_tableTripleStart + CVector3f(0.f, 0.f, zOff));
    break;
  case kOT_RestoreDefaults:
    break;
  default:
    break;
  }
}

void COptionsScreen::OnSliderChanged(CGuiSliderGroup* caller, float value) {
  if (x10_mode == kM_RightTable) {
    const SOptionCategory& category =
        GetOptionCategory(x70_tablegroup_leftlog->GetUserSelection(), false);
    const EGameOption option = category.options[x1c_rightSel].option;
    CGameOptions::SetOption(option, caller->GetCurVal());
  }
}

void COptionsScreen::OnEnumChanged(CGuiTableGroup* caller, int oldSel) {
  if (x10_mode == kM_RightTable) {
    const SOptionCategory& category =
        GetOptionCategory(x70_tablegroup_leftlog->GetUserSelection(), false);
    const SGameOption& option = category.options[x1c_rightSel];
    const int selection = caller->GetUserSelection();
    CGameOptions::SetOption(option.option, selection);
    if (option.option == kGO_Rumble && selection > 0) {
      x1a8_rumble.HardStopAll();
      x1a8_rumble.Rumble(skRumbleFxTable[kRFX_PlayerBump], 1.f, kRP_One, kIOP_Player1);
    }
    CPauseScreenBase::UpdateSideTable(caller);
    CSfxManager::SfxStart(0x59d, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
  }
}

SOptionsFrontEndFrame::SOptionsFrontEndFrame()
: x0_uiAlpha(0.f)
, x4_frme(gpSimplePool->GetObj("FRME_OptionsFrontEnd"))
, x10_pauseScreen(gpSimplePool->GetObj("STRG_PauseScreen"))
, x1c_loadedFrame(nullptr)
, x20_loadedPauseStrg(nullptr)
, x24_tablegroup_leftmenu(nullptr)
, x28_tablegroup_rightmenu(nullptr)
, x2c_tablegroup_double(nullptr)
, x30_tablegroup_triple(nullptr)
, x34_slidergroup_slider(nullptr)
, x38_rowPitch(0.f)
, x134_24_visible(true)
, x134_25_exitOptions(false) {
  x4_frme.Lock();
  x10_pauseScreen.Lock();
}

SOptionsFrontEndFrame::~SOptionsFrontEndFrame() { CSfxManager::SfxStop(x3c_sliderSfx); }

bool SOptionsFrontEndFrame::PumpLoad() {
  if (x1c_loadedFrame != nullptr) {
    return true;
  }
  if (x4_frme.TryCache() && x10_pauseScreen.TryCache()) {
    CGuiFrame* frame = x4_frme.GetObject();
    if (frame->GetIsFinishedLoading()) {
      x1c_loadedFrame = frame;
#ifdef TARGET_PC
      // Port: pillarbox the options like the file select they open over (issue
      // #14); stretched, their text ran out past the file select's panel.
      if (CGuiCamera* cam = frame->GetFrameCamera()) {
        cam->SetAspectMatch(true, false);
      }
#endif
      x20_loadedPauseStrg = x10_pauseScreen.GetObject();
      FinishedLoading();
      return true;
    }
  }
  return false;
}

void SOptionsFrontEndFrame::FinishedLoading() {
  x24_tablegroup_leftmenu =
      static_cast< CGuiTableGroup* >(x1c_loadedFrame->FindWidget("tablegroup_leftmenu"));
  x28_tablegroup_rightmenu =
      static_cast< CGuiTableGroup* >(x1c_loadedFrame->FindWidget("tablegroup_rightmenu"));
  x2c_tablegroup_double =
      static_cast< CGuiTableGroup* >(x1c_loadedFrame->FindWidget("tablegroup_double"));
  x30_tablegroup_triple =
      static_cast< CGuiTableGroup* >(x1c_loadedFrame->FindWidget("tablegroup_triple"));
  x34_slidergroup_slider =
      static_cast< CGuiSliderGroup* >(x1c_loadedFrame->FindWidget("slidergroup_slider"));
  x24_tablegroup_leftmenu->SetMenuAdvanceCallback(
      TFunctor1FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const >::Make(
          *this, &SOptionsFrontEndFrame::DoLeftMenuAdvance));
  x24_tablegroup_leftmenu->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const, const int >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuSelectionChange));
  x38_rowPitch = x24_tablegroup_leftmenu->GetWorkerWidget(1)->GetIdlePosition().GetZ() -
                 x24_tablegroup_leftmenu->GetWorkerWidget(0)->GetIdlePosition().GetZ();
  x28_tablegroup_rightmenu->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const, const int >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuSelectionChange));
  x28_tablegroup_rightmenu->SetMenuCancelCallback(
      TFunctor1FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuCancel));
  x2c_tablegroup_double->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const, const int >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuSelectionChange));
  x2c_tablegroup_double->SetMenuCancelCallback(
      TFunctor1FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuCancel));
  x30_tablegroup_triple->SetMenuSelectionChangeCallback(
      TFunctor2FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const, const int >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuSelectionChange));
  x30_tablegroup_triple->SetMenuCancelCallback(
      TFunctor1FromMethod< SOptionsFrontEndFrame, CGuiTableGroup* const >::Make(
          *this, &SOptionsFrontEndFrame::DoMenuCancel));
  x34_slidergroup_slider->SetSelectionChangedCallback(
      TFunctor2FromMethod< SOptionsFrontEndFrame, CGuiSliderGroup* const, const float >::Make(
          *this, &SOptionsFrontEndFrame::DoSliderChange));

  SetTextPanePair(x1c_loadedFrame, "textpane_double0", x20_loadedPauseStrg->GetString(95));
  SetTextPanePair(x1c_loadedFrame, "textpane_double1", x20_loadedPauseStrg->GetString(94));
  SetTextPanePair(x1c_loadedFrame, "textpane_triple0", x20_loadedPauseStrg->GetString(96));
  SetTextPanePair(x1c_loadedFrame, "textpane_triple1", x20_loadedPauseStrg->GetString(97));
  SetTextPanePair(x1c_loadedFrame, "textpane_triple2", x20_loadedPauseStrg->GetString(98));
  SetTextPanePair(x1c_loadedFrame, "textpane_title", gpStringTable->GetString(99));
  if (CGuiTextPane* proceed =
          static_cast< CGuiTextPane* >(x1c_loadedFrame->FindWidget("textpane_proceed"))) {
    proceed->TextSupport().SetText(rstl::wstring_l(gpStringTable->GetString(85)));
  }
  if (CGuiTextPane* cancel =
          static_cast< CGuiTextPane* >(x1c_loadedFrame->FindWidget("textpane_cancel"))) {
    cancel->TextSupport().SetText(rstl::wstring_l(gpStringTable->GetString(82)));
  }
  for (int i = 0; i < 4; ++i) {
    char name[32];
    sprintf(name, "textpane_filename%d", i);
    SetTextPanePair(x1c_loadedFrame, name, x20_loadedPauseStrg->GetString(16 + i));
  }
  x2c_tablegroup_double->SetVertical(false);
  x30_tablegroup_triple->SetVertical(false);
  x24_tablegroup_leftmenu->SetIsActive(true);
  x28_tablegroup_rightmenu->SetIsActive(false);
  SetTableColors(x24_tablegroup_leftmenu);
  SetTableColors(x28_tablegroup_rightmenu);
  SetTableColors(x2c_tablegroup_double);
  SetTableColors(x30_tablegroup_triple);
  SetRightUIText();
  DeactivateRightMenu();
}

void SOptionsFrontEndFrame::SetRightUIText() {
  const SOptionCategory& options = skGameOptions[x24_tablegroup_leftmenu->GetUserSelection()];
  for (int i = 0; i < 5; ++i) {
    char name[32];
    sprintf(name, "textpane_right%d", i);
    if (i < options.count) {
      SetTextPanePair(x1c_loadedFrame, name,
                      x20_loadedPauseStrg->GetString(options.options[i].stringId));
      x28_tablegroup_rightmenu->GetWorkerWidget(i)->SetIsSelectable(true);
    } else {
      SetTextPanePair(x1c_loadedFrame, name, L"");
      x28_tablegroup_rightmenu->GetWorkerWidget(i)->SetIsSelectable(false);
    }
  }
}

void SOptionsFrontEndFrame::HandleRightSelectionChange() {
  DeactivateRightMenu();
  const SOptionCategory& category = skGameOptions[x24_tablegroup_leftmenu->GetUserSelection()];
  const SGameOption& option = category.options[x28_tablegroup_rightmenu->GetUserSelection()];
  switch (option.type) {
  case kOT_Float:
    x34_slidergroup_slider->SetIsActive(true);
    x34_slidergroup_slider->SetVisibility(true, kTM_Children);
    x34_slidergroup_slider->SetMinVal(option.minVal);
    x34_slidergroup_slider->SetMaxVal(option.maxVal);
    x34_slidergroup_slider->SetIncrement(option.increment);
    x34_slidergroup_slider->SetCurVal(CGameOptions::GetOption(option.option));
    x34_slidergroup_slider->SetO2PTransform(
        CTransform4f::Translate(0.f, 0.f,
                                x28_tablegroup_rightmenu->GetUserSelection() * x38_rowPitch) *
        x34_slidergroup_slider->GetTransform());
    break;
  case kOT_DoubleEnum:
    x2c_tablegroup_double->SetUserSelection(CGameOptions::GetOption(option.option));
    x2c_tablegroup_double->SetIsVisible(true);
    x2c_tablegroup_double->SetIsActive(true);
    x2c_tablegroup_double->SetO2PTransform(
        CTransform4f::Translate(0.f, 0.f,
                                x28_tablegroup_rightmenu->GetUserSelection() * x38_rowPitch) *
        x2c_tablegroup_double->GetTransform());
    SetTableColors(x2c_tablegroup_double);
    break;
  case kOT_TripleEnum:
    x30_tablegroup_triple->SetUserSelection(CGameOptions::GetOption(option.option));
    x30_tablegroup_triple->SetIsVisible(true);
    x30_tablegroup_triple->SetIsActive(true);
    x30_tablegroup_triple->SetO2PTransform(
        CTransform4f::Translate(0.f, 0.f,
                                x28_tablegroup_rightmenu->GetUserSelection() * x38_rowPitch) *
        x30_tablegroup_triple->GetTransform());
    SetTableColors(x30_tablegroup_triple);
    break;
  default:
    break;
  }
}

void SOptionsFrontEndFrame::DeactivateRightMenu() {
  x2c_tablegroup_double->SetIsActive(false);
  x30_tablegroup_triple->SetIsActive(false);
  x34_slidergroup_slider->SetIsActive(false);
  x2c_tablegroup_double->SetVisibility(false, kTM_Children);
  x30_tablegroup_triple->SetVisibility(false, kTM_Children);
  x34_slidergroup_slider->SetVisibility(false, kTM_Children);
}

void SOptionsFrontEndFrame::DoLeftMenuAdvance(CGuiTableGroup* caller) {
  if (caller == x24_tablegroup_leftmenu) {
    HandleRightSelectionChange();
    x28_tablegroup_rightmenu->SetUserSelection(0);
    x24_tablegroup_leftmenu->SetIsActive(false);
    x28_tablegroup_rightmenu->SetIsActive(true);
    CSfxManager::SfxStart(0x448, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
    CSfxManager::SfxStart(0x443, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
  }
}

void SOptionsFrontEndFrame::DoMenuSelectionChange(CGuiTableGroup* caller, int oldSel) {
  SetTableColors(caller);
  if (caller == x24_tablegroup_leftmenu) {
    SetRightUIText();
    CSfxManager::SfxStart(0x445, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
  } else if (caller == x28_tablegroup_rightmenu) {
    HandleRightSelectionChange();
    CSfxManager::SfxStart(0x445, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
  } else if (caller == x2c_tablegroup_double || caller == x30_tablegroup_triple) {
    if (x28_tablegroup_rightmenu->GetIsActive()) {
      const SOptionCategory& category = skGameOptions[x24_tablegroup_leftmenu->GetUserSelection()];
      const SGameOption& option = category.options[x28_tablegroup_rightmenu->GetUserSelection()];
      const int selection = caller->GetUserSelection();
      CGameOptions::SetOption(option.option, selection);
      CSfxManager::SfxStart(0x447, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
      if (option.option == kGO_Rumble && selection > 0) {
        x40_rumbleGen.HardStopAll();
        x40_rumbleGen.Rumble(skRumbleFxTable[kRFX_PlayerBump], 1.f, kRP_One, kIOP_Player1);
      }
    }
  }
}

void SOptionsFrontEndFrame::DoMenuCancel(CGuiTableGroup* caller) {
  if (caller == x28_tablegroup_rightmenu) {
    DeactivateRightMenu();
    x24_tablegroup_leftmenu->SetIsActive(true);
    x28_tablegroup_rightmenu->SetIsActive(false);
    x28_tablegroup_rightmenu->SetUserSelection(0);
    SetTableColors(x28_tablegroup_rightmenu);
    CSfxManager::SfxStart(0x446, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
  }
}

void SOptionsFrontEndFrame::Update(float dt, const CSaveGameScreen* saveUI) {
  x40_rumbleGen.Update(dt);
  x134_24_visible = saveUI == nullptr || saveUI->GetUIType() == CSaveGameScreen::kUIT_SaveReady;
  if (!PumpLoad()) {
    return;
  }
  x0_uiAlpha = rstl::min_val(1.f, x0_uiAlpha + dt);
  x1c_loadedFrame->Update(dt);
  const bool sliding = x34_slidergroup_slider->GetState() != CGuiSliderGroup::kS_None;
  if (bool(x3c_sliderSfx) != sliding) {
    if (sliding) {
      x3c_sliderSfx = CSfxManager::SfxStart(0x5b2, 0x7f, 0x40, false, CSfxManager::kMedPriority,
                                            false, CSfxManager::kAllAreas);
    } else {
      CSfxManager::SfxStop(x3c_sliderSfx);
      x3c_sliderSfx.Clear();
    }
  }
}

bool SOptionsFrontEndFrame::ProcessUserInput(const CFinalInput& input, CSaveGameScreen* saveUI) {
  x134_25_exitOptions = false;
  if (saveUI != nullptr) {
    saveUI->ProcessUserInput(input);
  }
  if (x1c_loadedFrame != nullptr && x134_24_visible) {
    if (input.PB() && x24_tablegroup_leftmenu->GetIsActive()) {
      x134_25_exitOptions = true;
      CSfxManager::SfxStart(0x446, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
    } else {
      x1c_loadedFrame->ProcessUserInput(input);
      CGameOptions::TryRestoreDefaults(input, x24_tablegroup_leftmenu->GetUserSelection(),
                                       x28_tablegroup_rightmenu->GetUserSelection(), true);
    }
  }
  return !x134_25_exitOptions;
}

void SOptionsFrontEndFrame::Draw() const {
  if (x1c_loadedFrame != nullptr && x134_24_visible) {
    x1c_loadedFrame->Draw(CGuiWidgetDrawParms(x0_uiAlpha, CVector3f::Zero()));
  }
}

void SOptionsFrontEndFrame::SetTableColors(CGuiTableGroup* table) const {
  const CColor selected(uchar(255), uchar(255), uchar(255), uchar(255));
  const CColor unselected(uchar(160), uchar(160), uchar(160), uchar(200));
  table->SetColors(selected, unselected);
}

void SOptionsFrontEndFrame::DoSliderChange(CGuiSliderGroup* caller, float value) {
  if (x28_tablegroup_rightmenu->GetIsActive()) {
    const SOptionCategory& category = skGameOptions[x24_tablegroup_leftmenu->GetUserSelection()];
    const EGameOption option =
        category.options[x28_tablegroup_rightmenu->GetUserSelection()].option;
    CGameOptions::SetOption(option, caller->GetCurVal());
  }
}
