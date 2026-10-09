#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CMain.hpp"

// Port: Aurora owns the application/window/GPU loop; the game entry is renamed
// and driven by platform/main.cpp.
#include <algorithm>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/phase.hpp>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>

#include "port_debug.h"
#include "port_log.h"
#include "port_disc.h"
#include "port_mods.h"
#include "port_textures.h"
#include "port_prompts.h"
#include "port_crash.h"
#include "port_watchdog.h"

#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"

#include "dolphin/PPCArch.h"
#include "dolphin/ar.h"
#include "dolphin/arq.h"
#include "dolphin/ai.h"
#include "dolphin/dvd.h"
#include "dolphin/os.h"
#include "dolphin/os/OSCache.h"
#include "dolphin/os/OSMemory.h"
#include "dolphin/os/OSMutex.h"
#include "dolphin/pad.h"
#include "dolphin/vi.h"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Audio/CDSPStreamManager.hpp"
#include "Kyoto/Audio/CAudioGroupSet.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Audio/CStreamAudioManager.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Basics/RAssertDolphin.hpp"
#include "Kyoto/CARAMManager.hpp"
#include "Kyoto/CARAMToken.hpp"
#include "Kyoto/CFrameDelayedKiller.hpp"
#include "Kyoto/Input/IController.hpp"
#include "Kyoto/Math/CloseEnough.hpp"

// Port: drives the streamed-audio AI DMA callback (see platform/ai_dma.cpp).
extern "C" void AIPortPoll(void);
#include "port_console.h"
#include "port_speedrun_timer.h"
#ifdef MP_ENABLE_SMOKE_DRIVER
#include "port_smoke.h"
extern bool PortSmokeFrame(unsigned frame);
#endif

#include "Kyoto/CMemoryCardSys.hpp"
#include "Kyoto/CPakFile.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CMemoryStreamOut.hpp"
#include "Kyoto/Streams/CZipInputStream.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAudioStateWin.hpp"
#include "MetroidPrime/CConsoleOutputWindow.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/CDecalManager.hpp"
#include "MetroidPrime/CEnvFxManager.hpp"
#include "MetroidPrime/CErrorOutputWindow.hpp"
#include "MetroidPrime/CGameGlobalObjects.hpp"
#include "MetroidPrime/CInGameTweakManager.hpp"
#include "MetroidPrime/CMainFlow.hpp"
#include "MetroidPrime/Decode.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CSplashScreen.hpp"
#include "MetroidPrime/Factories/CCharacterFactoryBuilder.hpp"
#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Player/CSystemState.hpp"
#include "MetroidPrime/Player/CWorldTransManager.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMazeNode.hpp"
#include "MetroidPrime/Tweaks/CTweakGame.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"

const CFactoryFnReturn FStringTableFactory(const SObjectTag&, CInputStream&,
                                           const CVParamTransfer&);
const CFactoryFnReturn FModelFactory(const SObjectTag&, const rstl::auto_ptr< uchar[] >&, int,
                                     const CVParamTransfer&);
const CFactoryFnReturn FTextureFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FSkinRulesFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn AnimSourceFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FCharLayoutInfo(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FAnimCharacterSet(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FCollisionResponseDataFactory(const SObjectTag&, CInputStream&,
                                                     const CVParamTransfer&);
const CFactoryFnReturn FParticleSwooshDataFactory(const SObjectTag&, CInputStream&,
                                                  const CVParamTransfer&);
const CFactoryFnReturn FParticleFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FParticleElectricDataFactory(const SObjectTag&, CInputStream&,
                                                    const CVParamTransfer&);
const CFactoryFnReturn FProjectileWeaponDataFactory(const SObjectTag&, CInputStream&,
                                                    const CVParamTransfer&);
const CFactoryFnReturn RGuiFrameFactoryInGame(const SObjectTag&, CInputStream&,
                                              const CVParamTransfer&);
const CFactoryFnReturn FRasterFontFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FScannableObjectInfoFactory(const SObjectTag&, CInputStream&,
                                                   const CVParamTransfer&);
const CFactoryFnReturn AnimPOIDataFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FAiFiniteStateMachineFactory(const SObjectTag&, CInputStream&,
                                                    const CVParamTransfer&);
const CFactoryFnReturn FAudioGroupSetLocDataFactory(const SObjectTag&,
                                                    const rstl::auto_ptr< uchar[] >&, int,
                                                    const CVParamTransfer&);
const CFactoryFnReturn FCollidableOBBTreeGroupFactory(const SObjectTag&, CInputStream&,
                                                      const CVParamTransfer&);
const CFactoryFnReturn FDecalDataFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FAudioTranslationTableFactory(const SObjectTag&, CInputStream&,
                                                     const CVParamTransfer&);
const CFactoryFnReturn FPathFindAreaFactory(const SObjectTag&, const rstl::auto_ptr< uchar[] >&, int,
                                            const CVParamTransfer&);
const CFactoryFnReturn FMapWorldFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FMapAreaFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FMapUniverseFactory(const SObjectTag&, CInputStream&,
                                           const CVParamTransfer&);
const CFactoryFnReturn FMidiDataFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FDependencyGroupFactory(const SObjectTag&, CInputStream&,
                                               const CVParamTransfer&);
const CFactoryFnReturn FSaveWorldFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);
const CFactoryFnReturn FHintFactory(const SObjectTag&, CInputStream&, const CVParamTransfer&);

CResFactory* gpResourceFactory;
CSimplePool* gpSimplePool;
CCubeRenderer* gpRender;
CCharacterFactoryBuilder* gpCharacterFactoryBuilder;
CGuiSys* gGuiSystem;
CStringTable* gpStringTable;
CMain* gpMain;
IController* gpController;
CGameState* gpGameState;
CMemoryCard* gpMemoryCard;
CInGameTweakManager* gpTweakManager;
const TToken< CRasterFont >* gpDefaultFont;
void* CSaveRegion::mSaveBuffer;
void* CSaveRegion::mNonVolatileSettingsBuf;
bool COsContext::mProgressiveMode;
u32 sARAMMemArray[2];
float sInfiniteLoopTime;
static uint sTicksAdvanced = 0;

// Port: apply the selected aspect ratio to the game's framebuffer and viewport.
// Aurora derives the internal EFB from the render mode, so this also resizes the
// presented image, and it applies live when the debug overlay changes the mode.
static void ApplyAspectMode() {
  const GXRenderModeObj& renderMode = CGraphics::GetRenderMode();
  const int efbHeight = renderMode.efbHeight;
  int fbWidth = renderMode.fbWidth;
  switch (PortDebug::AspectMode()) {
  case PortDebug::kAspect_4_3:
    fbWidth = efbHeight * 4 / 3;
    break;
  case PortDebug::kAspect_16_9:
    fbWidth = (efbHeight * 16 + 8) / 9;
    break;
  case PortDebug::kAspect_Window: {
    // Ask the window rather than track resize events: the loading loops that
    // run before the main loop drain the startup resize (on Android the window
    // is already full screen at the phone's shape by then).
    int windowWidth = 0;
    int windowHeight = 0;
    if (!PortDebug::WindowSize(windowWidth, windowHeight)) {
      windowWidth = 854;
      windowHeight = 480;
    }
    fbWidth = static_cast< int >(static_cast< double >(efbHeight) *
                                     static_cast< double >(windowWidth) /
                                     static_cast< double >(windowHeight) +
                                 0.5);
    break;
  }
  }
  fbWidth &= ~1;
  // The GX scissor is a 12-bit field biased by 342, so a framebuffer wider than 3754 wraps it to zero
  // width and clips every draw (an all-black picture). That is ~7.8:1 at 480 lines, far past any real display.
  const int maxWidth = 3754;
  if (fbWidth > maxWidth) {
    fbWidth = maxWidth;
  }
  if (fbWidth == renderMode.fbWidth) {
    return;
  }
  CGraphics::PortResizeFrameBuffer(static_cast< u16 >(fbWidth));
  CCameraManager::RefreshAspectRatio();
}

#define GRAPHICS_FIFO_SIZE 0x60000
static uchar sGraphicsFifo[GRAPHICS_FIFO_SIZE];
ALIGNAS(CMain) static uchar sMainSpace[sizeof(CMain)];

// Generated includes belong only to the matching console build. Native builds
// read these resources from the user's disc instead of embedding extracted data.
#ifndef TARGET_PC
#include "MetroidPrime/DefaultFontData.inc"
#include "MetroidPrime/DefaultFontTexture.inc"
#endif

struct SAudioGroupInfo {
  const char* name;
  uchar groupId;
};

static const SAudioGroupInfo skPreLoadGroups[] = {
    {"Misc_AGSC", 39},    {"MiscSamus_AGSC", 41}, {"UI_AGSC", 40},
    {"Weapons_AGSC", 43}, {"ZZZ_AGSC", 65},
};
// sdata
bool lbl_805A6BC0 = true;

extern "C" void OSGetSavedRegion(void** start, void** end);
extern "C" void OSSetSaveRegion(void* start, void* end);
static inline void AddFrameTime(TReservedAverage< float, 4 >& average, const float& value) {
  if (average.size() < average.capacity()) {
    average.push_back(value);
  }
  for (int i = average.size() - 1; i > 0; --i) {
    average[i] = average[i - 1];
  }
  average[0] = value;
}

#define UNUSED_STACK_VAL 0x7337D00D

CSaveRegion::CSaveRegion(CMain& main) {
  void* end;
  OSGetSavedRegion(&mNonVolatileSettingsBuf, &end);
  OSSetSaveRegion(nullptr, nullptr);
  mSaveBuffer = main.OsContext().AllocFromArena(128);
}

// Port: called from platform/main.cpp after Aurora and the disc are initialized.
extern "C" int metroid_main(int argc, char** argv) {
  DVDSetAutoFatalMessaging(TRUE);
  SetErrorHandlers();
  CMain* main = new (&sMainSpace) CMain();
  try {
    gpMain->RsMain(argc, argv);
  } catch (...) {
    main->~CMain();
    gpMain = nullptr;
    throw;
  }
  main->~CMain();
  gpMain = nullptr;
  return 0;
}

extern "C" void* __sys_alloc(const size_t len) { return CMemory::Alloc(len); }

extern "C" void __sys_free(const void* ptr) { CMemory::Free(ptr); }

COsContext& CMain::OpenWindow() {
  if (CSaveRegion::GetNonVolatileSettingsBuffer() != nullptr) {
    CMemoryInStream stream(CSaveRegion::GetNonVolatileSettingsBuffer(), 128);
    COsContext::SetProgressiveMode(stream.ReadBits(1));
  }
  x0_osContext.OpenWindow("Metaforce", 0, 0, 640, 480, true);
  return x0_osContext;
}

CMain::CMain()
: x0_osContext(true, true)
, x6c_saveRegion(*this)
, x6d_memorySys(OpenWindow(), CMemorySys::GetGameAllocator())
, xe8_unknown(0.0)
, x118_averageTickTime(0.f)
, x11c_averageDrawTime(0.f)
, x120_softResetHoldTime(0.f)
, x124_resetInputDelay(0.f)
, x128_gameGlobalObjects(nullptr)
, x12c_restartMode(kRM_Default)
, x130_frameTimes(0xF4240)
, x15c_frameTimeIdx(0)
, x160_24_finished(false)
, x160_25_mfGameBuilt(false)
, x160_26_screenFading(false)
, x160_27_resetButtonHeld(false)
, x160_28_manageCard(false)
, x160_29_resetRequested(false)
, x160_30_gameExitReset(false)
, x160_31_cardBusy(false)
, x161_24_gameFrameDrawn(false)
, x164_archSupport(nullptr) {
  gpMain = this;
}

CMain::~CMain() {}

void CMain::InitializeSubsystems() {
  ARInit(sARAMMemArray, 2);
  ARAlloc(0x5fc000);
  CARAMManager::PreInitializeAlloc(0x5fc000);
  ARQInit();
  OSThread* thread = OSGetCurrentThread();
  printf("Protecting stack...  ");
  uchar* stackEnd =
      reinterpret_cast< uchar* >(ALIGN_UP(reinterpret_cast< uintptr_t >(thread->stackEnd), 0x400));
  uchar* stackBase = thread->stackBase;
#ifdef TARGET_PC
  // Port: there is no emulated guest stack to fill here; this is a debug aid.
  (void)stackBase;
  (void)stackEnd;
#else
  OSProtectRange(OS_PROTECT_CHAN3, stackEnd, 0x400, OS_PROTECT_CONTROL_NONE);

  uchar* ptr = stackEnd + 0x400;
  for (; ptr < stackBase - 0x2000; ptr += 4) {
    *reinterpret_cast< int* >(ptr) = UNUSED_STACK_VAL;
  }

  DCFlushRange(stackEnd + 0x400, static_cast< uint >(stackBase - 0x2000 - (stackEnd + 0x400)));
#endif
  printf("Stack: %p down to %p\n", static_cast<void*>(thread->stackBase),
         static_cast<void*>(thread->stackEnd));
  CElementGen::Initialize();
  CAnimData::InitializeCache();
  CARAMManager::Initialize(0x800);
  CDecalManager::Initialize();
  CFrameDelayedKiller::Initialize();
}

void CMain::ShutdownSubsystems() {
  CFrameDelayedKiller::ShutDown();
  CDecalManager::ShutDown();
  CElementGen::ShutDown();
  CAnimData::FreeCache();

#ifndef TARGET_PC
  OSThread* thread = OSGetCurrentThread();
  uchar* stackEnd =
      reinterpret_cast< uchar* >(ALIGN_UP(reinterpret_cast< uintptr_t >(thread->stackEnd), 0x400));
  uchar* stackBase = thread->stackBase;

  uchar* ptr = stackEnd + 0x400;
  for (; ptr < stackBase - 0x2000; ptr += 4) {
    if (*reinterpret_cast< uint* >(ptr) != UNUSED_STACK_VAL) {
      break;
    }
  }
  const int used = static_cast< int >(stackBase - 0x2000 - ptr) + 0x2000;
  OSReport("Stack usage: %d bytes (%dk)\n", used, static_cast< uint >(used) / 1024);
#endif
}

CGameGlobalObjects::CGameGlobalObjects(COsContext& osContext, CMemorySys& memorySys)
: xcc_simplePool(x4_resFactory)
, x130_graphicsSys(osContext, memorySys, GRAPHICS_FIFO_SIZE, sGraphicsFifo)
, x134_gameState(rs_new CGameState())
, x150_inGameTweakManager(rs_new CInGameTweakManager())
, x154_defaultFont(LoadDefaultFont()) {
  gpResourceFactory = &x4_resFactory;
  gpSimplePool = &xcc_simplePool;
  gpCharacterFactoryBuilder = &xec_characterFactoryBuilder;
  gpGameState = x134_gameState.get();
  gpTweakManager = x150_inGameTweakManager.get();
  gpDefaultFont = &x154_defaultFont;
}

CRasterFont* CGameGlobalObjects::LoadDefaultFont() {
#ifdef TARGET_PC
  // Verified GM8E01_00 symbols; see config/GM8E01_00/symbols.txt. Other
  // discs have them elsewhere: found by their first bytes (zlib streams).
  static const uint8_t kFontDataStart[] = {0x78, 0xda, 0x8d, 0x57, 0x5b, 0x6f, 0x54, 0x55,
                                           0x14, 0x5e, 0xa5, 0x74, 0x3a, 0x9d, 0x96, 0x82};
  static const uint8_t kFontTextureStart[] = {0x78, 0xda, 0xed, 0x59, 0xdb, 0x92, 0x14, 0x31,
                                              0x08, 0x05, 0xcb, 0x07, 0x1f, 0xc3, 0x1f, 0xf9};
  std::vector< uint8_t > fontData, fontTexture;
  // PAL's has other bytes (FONT v4): the font naming the same texture. Looked up
  // first there, so a PAL boot doesn't throw (and log) on the way.
  const bool palFont = PortDisc::Current() == PortDisc::Version::Pal &&
                       PortFindDolFont(0x6065853f, fontData, fontTexture);
  if (!palFont) {
    try {
      fontData = PortFindDolResource(0x803cb3a0, 0x650, kFontDataStart, sizeof(kFontDataStart));
      fontTexture =
          PortFindDolResource(0x803cb9f0, 0x45c, kFontTextureStart, sizeof(kFontTextureStart));
    } catch (const std::runtime_error&) {
      if (!PortFindDolFont(0x6065853f, fontData, fontTexture)) {
        throw;
      }
    }
  }
  CZipInputStream fontDataStream(rs_new CMemoryInStream(fontData.data(), fontData.size()));
  rstl::single_ptr<CRasterFont> font(rs_new CRasterFont(fontDataStream, nullptr));
  CZipInputStream fontTextureStream(rs_new CMemoryInStream(fontTexture.data(), fontTexture.size()));
  font->SetTexture(rs_new CTexture(fontTextureStream, CTexture::kAM_Zero, CTexture::kBK_Zero));
  return font.release();
#else
  CZipInputStream fontDataStream(
      rs_new CMemoryInStream(sDefaultFontData, sizeof(sDefaultFontData)));
  CRasterFont* font = rs_new CRasterFont(fontDataStream, nullptr);
  CZipInputStream fontTextureStream(
      rs_new CMemoryInStream(sDefaultFontTexture, sizeof(sDefaultFontTexture)));
  font->SetTexture(rs_new CTexture(fontTextureStream, CTexture::kAM_Zero, CTexture::kBK_Zero));
  return font;
#endif
}

void CGameGlobalObjects::PostInitialize(COsContext& osContext, CMemorySys& memorySys) {
#if VERSION != 0
  AddPaksAndFactories(osContext);
#else
  AddPaksAndFactories();
#endif
  LoadStringTable();
  printf("Initializing renderer...\n");
  x14c_renderer = Renderer::AllocateRenderer(xcc_simplePool, osContext, memorySys, x4_resFactory);
  gpRender = reinterpret_cast< CCubeRenderer* >(x14c_renderer.get());
  CEnvFxManager::Initialize();
  CScriptMazeNode::LoadMazeSeeds();
}

void CGameGlobalObjects::LoadStringTable() {
  x13c_stringTable = gpSimplePool->GetObj("STRG_Main");
  gpStringTable = **x13c_stringTable;
}

void InitializeApplicationUI(CGuiSys&);

void InfiniteLoopAlarm(OSAlarm* alarm, OSContext* context) {
  if (sInfiniteLoopTime >= 10.f) {
    rs_debugger_printf("INFINITE LOOP");
  }
  sInfiniteLoopTime += alarm->period / OS_TIMER_CLOCK;
}

CGameArchitectureSupport::CGameArchitectureSupport(COsContext& osContext)
: x0_audioSys(0x30, 0x30, 0x30, 0x30, 0x5fc000)
, x30_inputGenerator(&osContext, gpTweakPlayer->GetLeftAnalogMax(),
                     gpTweakPlayer->GetRightAnalogMax())
, x44_guiSys(gpResourceFactory, gpSimplePool, CGuiSys::kUM_Zero)
, x78_gameFrameCount(0)
, x7c_tickClock()
, x88_audioLoadStatus(kALS_Uninitialized)
, xc8_infiniteLoopAlarmSet(false) {
  CAudioSys::SysSetVolume(0x7F, 0, 0xFF);
  CAudioSys::SetDefaultVolumeScale(0x75);
  CAudioSys::SetVolumeScale(CAudioSys::GetDefaultVolumeScale());
  CDSPStreamManager::Initialize();
  CStreamAudioManager::SetMusicVolume(0x7F);
  CAudioSys::TrkSetSampleRate(kTSR_One);
  gpMain->SetMaxSpeed(false);
  gpMain->ResetGameState();
  CIOWinManager& ioWinManager = x58_ioWinMgr;
  uint32_t bootWorld, bootArea;
  if (!gpTweakGame->GetSplashScreensDisabled() && !PortDebug::BootWorld(bootWorld, bootArea)) {
    ioWinManager.AddIOWin(rs_new CSplashScreen(CSplashScreen::kSplashScreen_Nintendo), 1000, 10000);
  }
  ioWinManager.AddIOWin(rs_new CMainFlow(), 0, 0);
  ioWinManager.AddIOWin(rs_new CConsoleOutputWindow(8, 5.f, 0.75f), 100, 0);
  ioWinManager.AddIOWin(rs_new CAudioStateWin(), 100, -1);
  ioWinManager.AddIOWin(rs_new CErrorOutputWindow(CErrorOutputWindow::kF_Zero), 10000, 100000);
  InitializeApplicationUI(x44_guiSys);
  CGuiSys::SetGlobalGuiSys(&x44_guiSys);
  gpController = x30_inputGenerator.GetController();
  gpGameState->GameOptions().EnsureOptions();
  sInfiniteLoopTime = 0.f;
  OSSetPeriodicAlarm(&xa0_infiniteLoopAlarm, OSGetTime(), (float)OS_TIMER_CLOCK, InfiniteLoopAlarm);
  xc8_infiniteLoopAlarmSet = true;
}

CGameArchitectureSupport::~CGameArchitectureSupport() {
  if (xc8_infiniteLoopAlarmSet) {
    OSCancelAlarm(&xa0_infiniteLoopAlarm);
    xc8_infiniteLoopAlarmSet = false;
  }
  x58_ioWinMgr.RemoveAllIOWins();
  UnloadAudio();
  CSfxManager::Shutdown();
  CDSPStreamManager::Shutdown();
}

bool CGameArchitectureSupport::UpdateTicks() {
  bool terminate = false;
  const BOOL interrupts = OSDisableInterrupts();
  const float elapsed = x20_tickStopwatch.GetElapsedTime();
  x20_tickStopwatch.Reset();
  OSRestoreInterrupts(interrupts);
  sInfiniteLoopTime = 0.f;

  // Adaptive mode steps once per frame with dt = the measured frame time (so a
  // variable frame rate is matched exactly); otherwise a fixed 1/SimRate() step
  // catches up with whole ticks.
  double period;
  if (PortDebug::SimAdaptive() && !PortDebug::Turbo()) {
    period = static_cast< double >(elapsed);
    if (period < 1.0 / 480.0) {
      period = 1.0 / 480.0;
    } else if (period > 1.0 / 30.0) {
      period = 1.0 / 30.0;
    }
  } else {
    period = 1.0 / static_cast< double >(PortDebug::SimRate());
  }
  x7c_tickClock.SetPeriod(period);
  unsigned ticks =
      x7c_tickClock.Advance(elapsed, gpMain->GetScreenFading() || PortDebug::Turbo(),
                            PortDebug::FrameLimitEnabled());
  if (PortDebug::Turbo() && !gpMain->GetScreenFading()) {
    ticks = PortDebug::TurboTicks();
  }
  if (PortDebug::TickHold()) {
    ticks = PortDebug::TakeHeldTicks();
  }

  const float tickPeriod = static_cast< float >(period);
  PortDebug::SetTickPeriod(tickPeriod);
  sTicksAdvanced = 0;
  x4_archQueue.Push(MakeMsg::CreateFrameBegin(kAMT_Game, x78_gameFrameCount));
  for (unsigned tick = 0; tick < ticks; ++tick) {
    PortDebug::BeginFrameMouse();
    // A tick that doesn't reach CStateManager::Update (paused) leaves every
    // actor's snapshot stale, so actors don't blend between old transforms.
    CActor::PortBeginTickSnapshot();
    if (!x30_inputGenerator.Update(tickPeriod, x4_archQueue)) {
      terminate = true;
    }
    x4_archQueue.Push(MakeMsg::CreateTimerTick(kAMT_Game, tickPeriod));
    x58_ioWinMgr.PumpMessages(x4_archQueue);
    ++sTicksAdvanced;
  }

  x58_ioWinMgr.PumpMessages(x4_archQueue);
  return !terminate;
}

void CGameArchitectureSupport::Update() {
  gpGameState->WorldTransitionManager()->TouchModels();
  x4_archQueue.Push(MakeMsg::CreateFrameEnd(kAMT_Game, x78_gameFrameCount));
  x58_ioWinMgr.PumpMessages(x4_archQueue);
}

void CGameArchitectureSupport::PreloadAudio() {
  if (x88_audioLoadStatus == kALS_Uninitialized) {
    x8c_pendingAudioGroups = rstl::vector< CToken >();
    x8c_pendingAudioGroups.reserve(5);
    for (int i = 0; i < 5; ++i) {
      CToken group = gpSimplePool->GetObj(skPreLoadGroups[i].name);
      if (i == 0) {
        group.Lock();
      }
      x8c_pendingAudioGroups.push_back(group);
    }
    x88_audioLoadStatus = kALS_Loading;
  }
}

bool CGameArchitectureSupport::LoadAudio() {
  if (x88_audioLoadStatus == kALS_Loaded) {
    return true;
  }

  bool loaded = true;
  for (int i = 0; i < sizeof(skPreLoadGroups) / sizeof(skPreLoadGroups[0]); ++i) {
    CToken& token = *(x8c_pendingAudioGroups.begin() + i);
    const SAudioGroupInfo& info = skPreLoadGroups[i];
    if (token.IsLocked()) {
      if (token.IsLoaded()) {
        TToken< CAudioGrpSetLoc > group(token);
        if (!CAudioSys::SysIsGroupSetLoaded(group->GetGroupSetName())) {
          const CAssetId id = gpResourceFactory->GetResourceIdByName(info.name)->GetId();
          CAudioSys::SysLoadGroupSet(group, group->GetGroupSetName(), id);
          const rstl::string& name = group->GetGroupSetName();
          CAudioSys::SysPushGroupIntoARAM(name, info.groupId);
          CAudioSys::SysUnloadSampleData(name);
        }
      } else {
        loaded = false;
        break;
      }
    } else {
      loaded = false;
      token.Lock();
      break;
    }
  }
  if (!loaded) {
    return false;
  }
  CSfxManager::LoadTranslationTable(gpSimplePool,
                                    gpResourceFactory->GetResourceIdByName("sound_lookup"));
  x8c_pendingAudioGroups = rstl::vector< CToken >();
  x88_audioLoadStatus = kALS_Loaded;
  return true;
}

bool CMain::LoadAudio() {
  if (x164_archSupport != nullptr) {
    return x164_archSupport->LoadAudio();
  }
  return true;
}

void CGameArchitectureSupport::UnloadAudio() {
  if (x88_audioLoadStatus == kALS_Loaded) {
    for (uint i = 0; i < 5; ++i) {
      CAudioSys::SysPopGroupFromARAM();
      rstl::string name = CAudioSys::SysGetGroupSetName(
          gpResourceFactory->GetResourceIdByName(skPreLoadGroups[4 - i].name)->GetId());
      CAudioSys::SysUnloadGroupSet(name);
    }
  }
  x8c_pendingAudioGroups = rstl::vector< CToken >();
  x88_audioLoadStatus = kALS_Uninitialized;
}

void CMain::MemoryCardInitializePump() {
  if (gpMemoryCard == nullptr) {
    if (x128_gameGlobalObjects->MemoryCard().get() == nullptr) {
      x128_gameGlobalObjects->MemoryCard() = rs_new CMemoryCard();
    }
    CMemoryCard* card = x128_gameGlobalObjects->MemoryCard().get();
    if (card->InitializePump()) {
      gpMemoryCard = card;
      gpGameState->InitializeMemoryStates();
    }
  }
}

#if VERSION != 0
void CGameGlobalObjects::AddPaksAndFactories(const COsContext& osContext) {
#else
void CGameGlobalObjects::AddPaksAndFactories() {
#endif
  CResFactory& factory = *gpResourceFactory;
  CGraphics::SetViewPointMatrix(CTransform4f::Identity());
  CGraphics::SetModelMatrix(CTransform4f::Identity());
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:Tweaks"), false, false);
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("NoARAM"), false, false);
  // Mod resources that did not fit in NoARAM.pak (2 GiB file limit).
  for (int i = 0; i < PortMods::ExtraPakCount(); ++i) {
    factory.GetResLoader().AddPakFileAsync(rstl::string(PortMods::ExtraPakName(i).c_str()), false,
                                           false);
  }
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("AudioGrp"), false, false);
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:MiscData"), false, false);

  CErrorOutputWindow errorWindow(CErrorOutputWindow::kF_One);
  CGraphics::SetIsBeginSceneClearFb(true);
  CGraphics::SetViewport(0, 0, CGraphics::GetViewportWidth(), CGraphics::GetViewportHeight());
#if VERSION != 0
  rstl::single_ptr< IController > controller(IController::Create(osContext));
  gpController = controller.get();
#endif
  while (!factory.GetResLoader().AreAllPaksLoaded()) {
    const AuroraEvent* event = aurora_update();
    for (; event != nullptr && event->type != AURORA_NONE; ++event) {
      if (event->type == AURORA_EXIT) {
        // Finish pending reads so their callback owners remain alive. The main
        // loop observes the exit flag immediately after initialization.
        gpMain->SetFinished();
      }
    }
    ARQPoll();
    gpResourceFactory->GetResLoader().AsyncIdlePakLoading();
    errorWindow.Update();
    if (CGraphics::BeginScene()) {
      errorWindow.ShowMessage();
      CGraphics::EndScene();
    } else {
      SDL_Delay(5);
    }
#if VERSION != 0
    controller->Poll();
    gpMain->CheckReset();
#endif
  }
#if VERSION != 0
  gpController = nullptr;
#endif

  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:SamusGun"), true, false);
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:TestAnim"), true, false);
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:SamGunFx"), true, false);
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:MidiData"), false, false);
  factory.GetResLoader().AddPakFileAsync(rstl::string_l("aram:GGuiSys"), false, false);

  factory.GetFactoryMgr().AddFactory('STRG', FStringTableFactory);
  factory.GetFactoryMgr().AddFactory('CMDL', FModelFactory);
  factory.GetFactoryMgr().AddFactory('TXTR', FTextureFactory);
  factory.GetFactoryMgr().AddFactory('CSKR', FSkinRulesFactory);
  factory.GetFactoryMgr().AddFactory('ANIM', AnimSourceFactory);
  factory.GetFactoryMgr().AddFactory('CINF', FCharLayoutInfo);
  factory.GetFactoryMgr().AddFactory('ANCS', FAnimCharacterSet);
  factory.GetFactoryMgr().AddFactory('CRSC', FCollisionResponseDataFactory);
  factory.GetFactoryMgr().AddFactory('SWHC', FParticleSwooshDataFactory);
  factory.GetFactoryMgr().AddFactory('PART', FParticleFactory);
  factory.GetFactoryMgr().AddFactory('ELSC', FParticleElectricDataFactory);
  factory.GetFactoryMgr().AddFactory('WPSC', FProjectileWeaponDataFactory);
  factory.GetFactoryMgr().AddFactory('FRME', RGuiFrameFactoryInGame);
  factory.GetFactoryMgr().AddFactory('FONT', FRasterFontFactory);
  factory.GetFactoryMgr().AddFactory('SCAN', FScannableObjectInfoFactory);
  factory.GetFactoryMgr().AddFactory('EVNT', AnimPOIDataFactory);
  factory.GetFactoryMgr().AddFactory('AFSM', FAiFiniteStateMachineFactory);
  factory.GetFactoryMgr().AddFactory('AGSC', FAudioGroupSetLocDataFactory);
  factory.GetFactoryMgr().AddFactory('DCLN', FCollidableOBBTreeGroupFactory);
  factory.GetFactoryMgr().AddFactory('DPSC', FDecalDataFactory);
  factory.GetFactoryMgr().AddFactory('ATBL', FAudioTranslationTableFactory);
  factory.GetFactoryMgr().AddFactory('PATH', FPathFindAreaFactory);
  factory.GetFactoryMgr().AddFactory('MAPW', FMapWorldFactory);
  factory.GetFactoryMgr().AddFactory('MAPA', FMapAreaFactory);
  factory.GetFactoryMgr().AddFactory('MAPU', FMapUniverseFactory);
  factory.GetFactoryMgr().AddFactory('CSNG', FMidiDataFactory);
  factory.GetFactoryMgr().AddFactory('DGRP', FDependencyGroupFactory);
  factory.GetFactoryMgr().AddFactory('SAVW', FSaveWorldFactory);
  factory.GetFactoryMgr().AddFactory('HINT', FHintFactory);
}

void CMain::FillInAssetIDs() {
  const SObjectTag* tag =
      gpResourceFactory->GetResourceIdByName(gpTweakGame->GetDefaultRoom().data());
  if (tag != nullptr) {
    gpGameState->SetCurrentWorldId(tag->GetId());
  }
}

void CMain::DoPredrawMetrics() {}

void CMain::DrawDebugMetrics(double, CStopwatch&) {}

bool CMain::CheckTerminate() { return false; }

bool CMain::CheckReset() {
  const BOOL resetButton = OSGetResetButtonState();
  const CControllerGamepadData& pad = gpController->GetGamepadData(0);
  if (pad.GetButton(kBU_B).GetIsPressed() && pad.GetButton(kBU_X).GetIsPressed() &&
      pad.GetButton(kBU_Start).GetIsPressed()) {
    if (x124_resetInputDelay >= 0.5f) {
      x120_softResetHoldTime += 1.f / 60.f;
      if (x120_softResetHoldTime > 0.5f) {
        x160_27_resetButtonHeld = true;
      }
    }
  } else {
    if (x124_resetInputDelay < 0.5f) {
      x124_resetInputDelay += 1.f / 60.f;
    }
    x120_softResetHoldTime = 0.f;
  }
  if (!resetButton && x160_27_resetButtonHeld) {
    x160_29_resetRequested = true;
  }

  if (!x160_31_cardBusy &&
      (x160_29_resetRequested || x160_28_manageCard || x160_30_gameExitReset)) {
#ifdef TARGET_PC
    // A native reset rebuilds the game architecture below. Do not cancel all
    // DVD work or stop audio for a console reboot that will never occur.
    x160_27_resetButtonHeld = false;
    x160_29_resetRequested = false;
    x160_30_gameExitReset = false;
    x160_28_manageCard = false;
    return true;
#endif
    if (x164_archSupport != nullptr && x164_archSupport->IsInfiniteLoopAlarmSet()) {
      OSCancelAlarm(&x164_archSupport->GetInfiniteLoopAlarm());
      x164_archSupport->SetInfiniteLoopAlarmSet(false);
    }
    GXDrawDone();
    GXAbortFrame();
#if VERSION == VERSION_GM8E_00
    CAudioSys::TrkFlushTracks();
    AISetStreamPlayState(0);
#endif
    if (!x160_30_gameExitReset) {
      gpGameState->GameOptions() = CGameOptions();
    } else {
      CGameOptions& options = gpGameState->GameOptions();
      options.SetScreenBrightness(4, false);
      options.SetScreenPositionX(0, false);
      options.SetScreenPositionY(0, false);
      options.SetScreenStretch(0, false);
    }
    {
      CMemoryStreamOut stream(CSaveRegion::GetSaveBuffer(), 128);
      stream.WriteBits(CGraphics::GetProgressiveMode() ? 1 : 0, 1);
      gpGameState->GameOptions().PutTo(stream);
      stream.WriteBits(lbl_805A6BC0 ? 1 : 0, 1);
      stream.Flush();
    }
    gpGameState->GameOptions().EnsureOptions();
    DCFlushRange(CSaveRegion::GetSaveBuffer(), 128);
    OSSetSaveRegion(CSaveRegion::GetSaveBuffer(),
                    static_cast< uchar* >(CSaveRegion::GetSaveBuffer()) + 128);
    VISetBlack(TRUE);
    VIFlush();
    VIWaitForRetrace();
    if (x160_28_manageCard) {
      OSResetSystem(OS_RESET_HOTRESET, 0, TRUE);
    } else if (DVDCheckDisk()) {
      DVDCancelAll();
      DVDCommandBlock block;
      DVDCancelStream(&block);
#if VERSION != 0
      if (CAudioSys::mInitialized) {
        CAudioSys::TrkFlushTracks();
      }
      AISetStreamPlayState(0);
      if (CAudioSys::mInitialized) {
        sndQuit();
      }
#endif
      OSResetSystem(OS_RESET_RESTART, 0, FALSE);
    } else {
      OSResetSystem(OS_RESET_HOTRESET, 0, FALSE);
    }
    x160_27_resetButtonHeld = false;
    x160_29_resetRequested = false;
    x160_30_gameExitReset = false;
    x160_28_manageCard = false;
    return true;
  }
  x160_27_resetButtonHeld = resetButton;
  return false;
}

int CMain::RsMain(int argc, const char* const* argv) {
  PPCSetFpIEEEMode();
  CStopwatch timer;
  LCEnable();

  rstl::single_ptr< CGameGlobalObjects > gameGlobalObjects(
      rs_new CGameGlobalObjects(x0_osContext, x6d_memorySys));
  x128_gameGlobalObjects = gameGlobalObjects.get();

  for (int i = 0; i < 4; ++i) {
    AddFrameTime(xf0_tickTimes, 0.3f);
    AddFrameTime(x104_drawTimes, 0.2f);
  }

  x118_averageTickTime = 0.3f;
  x11c_averageDrawTime = 0.2f;
  InitializeSubsystems();
  gameGlobalObjects->PostInitialize(x0_osContext, x6d_memorySys);
  x70_tweaks.RegisterTweaks();
  AddWorldPaks();

  {
    rstl::string str;
    bool logAudioTweaks;
    if (gpTweakManager->ReadFromMemoryCard(rstl::string_l("AudioTweaks"))) {
      str = rstl::string_l("Loaded audio tweaks from memory card\n");
      logAudioTweaks = true;
    } else {
      str = rstl::string_l("FAILED to load audio tweaks from memory card\n");
      logAudioTweaks = true;
    }

    FillInAssetIDs();

    rstl::single_ptr< CGameArchitectureSupport > archSupport(
        rs_new CGameArchitectureSupport(x0_osContext));
    x164_archSupport = archSupport.get();
    archSupport->PreloadAudio();

    srand(timer.GetElapsedMicros());

    if (CSaveRegion::GetNonVolatileSettingsBuffer() != nullptr) {
      CMemoryInStream stream(CSaveRegion::GetNonVolatileSettingsBuffer(), 0x80);
      stream.ReadBits(1);
      gpGameState->GameOptions() = CGameOptions(stream);
      gpGameState->GameOptions().EnsureOptions();
      lbl_805A6BC0 = stream.ReadBits(1);
    }

    double dt = 1.0 / 60.0;
    constexpr uint64_t framePeriodNs = 1000000000ull / 60;
    uint64_t nextFrameDeadline = SDL_GetTicksNS();
    // When the first frame was actually presented, and when the process started.
    // Startup is a number people ask about and the frame log is only a counter,
    // so nothing could report it. The process clock is taken here rather than at
    // entry because the interesting part is from the first real frame, and this
    // point is where the game's own loading is done.
    uint64_t firstFrameNs = 0;
    const uint64_t loopBeganNs = SDL_GetTicksNS();
    unsigned s_frameLog = 0;
    // Frame milestones in the log: where a slow start or a slow device shows up.
    // The counter line above is only the first frame.
    uint64_t milestoneSpanStartNs = 0;
    unsigned milestoneSpanStartFrame = 0;
    uint64_t milestoneWorstNs = 0;
    // The counter line once a second made most of a play session's log. Only the
    // first one prints (the game reached its loop), unless a test build or
    // MP_FRAME_LOG=1 asks for all of them; test scripts time runs by them.
#ifdef MP_ENABLE_SMOKE_DRIVER
    const bool frameLogAll = true;
#else
    const char* frameLogEnv = getenv("MP_FRAME_LOG");
    const bool frameLogAll = frameLogEnv != nullptr && frameLogEnv[0] != '\0' && frameLogEnv[0] != '0';
#endif
    try {
    while (!x160_24_finished) {
      const uint64_t loopStartNs = SDL_GetTicksNS();
      bool presented = false;
      PortWatchdog::Heartbeat(s_frameLog + 1);
      if ((s_frameLog++ % 60) == 0 && (frameLogAll || s_frameLog == 1)) {
        fprintf(stderr, "MP frame %u\n", s_frameLog);
      }
#ifdef MP_ENABLE_SMOKE_DRIVER
      if (PortSmokeFrame(s_frameLog)) {
        break;
      }
#else
      if (PortConsoleFrame(s_frameLog)) {
        break;
      }
#endif
      // Port: pump Aurora's window/input events (it marks the watchdog's phase).
      {
        const AuroraEvent* event = aurora_update();
        while (event != nullptr && event->type != AURORA_NONE) {
          if (event->type == AURORA_EXIT) {
            x160_24_finished = true;
          } else if (event->type == AURORA_SDL_EVENT && event->sdl.type == SDL_EVENT_KEY_DOWN &&
                     !event->sdl.key.repeat && event->sdl.key.scancode == SDL_SCANCODE_F10) {
            PortDebug::SetFrameLimitEnabled(!PortDebug::FrameLimitEnabled());
            nextFrameDeadline = SDL_GetTicksNS();
            fprintf(stderr, "Frame limit: %s\n",
                    PortDebug::FrameLimitEnabled() ? "60 FPS" : "unlimited");
          } else if (event->type == AURORA_WINDOW_RESIZED ||
                     event->type == AURORA_DISPLAY_SCALE_CHANGED) {
            ApplyAspectMode();
          } else if (event->type == AURORA_SDL_EVENT &&
                     event->sdl.type == SDL_EVENT_MOUSE_MOTION) {
            // Touches and pens also arrive as mouse motion (Android turns that
            // on for ImGui); only a real mouse aims.
            if (event->sdl.motion.which != SDL_TOUCH_MOUSEID &&
                event->sdl.motion.which != SDL_PEN_MOUSEID) {
              PortDebug::AddMouseDelta(event->sdl.motion.xrel, event->sdl.motion.yrel);
            }
          } else if (event->type == AURORA_SDL_EVENT &&
                     (event->sdl.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                      event->sdl.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            // Android's SDL reports button 0 when its button state is stale.
            if (event->sdl.button.button != 0) {
              PortDebug::NoteMouseButton(event->sdl.button.which == SDL_TOUCH_MOUSEID ||
                                             event->sdl.button.which == SDL_PEN_MOUSEID,
                                         SDL_BUTTON_MASK(event->sdl.button.button),
                                         event->sdl.button.down);
            }
          } else if (event->type == AURORA_SDL_EVENT && event->sdl.type == SDL_EVENT_FINGER_UP) {
            PortDebug::TapUpdateToast(event->sdl.tfinger.x, event->sdl.tfinger.y);
          } else if (event->type == AURORA_SDL_EVENT &&
                     event->sdl.type == SDL_EVENT_MOUSE_REMOVED) {
            PortDebug::ClearMouseButtons();
          } else if (event->type == AURORA_SDL_EVENT &&
                     event->sdl.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            PortDebug::SetMouseCaptured(false);
            PortDebug::ClearMouseButtons();
          }
          ++event;
        }
      }
      const uint64_t eventsDoneNs = SDL_GetTicksNS();
      // SDL/compositor relative capture owns cursor visibility and warp handling.
#ifdef MP_ENABLE_SMOKE_DRIVER
      if (PortSmokeMouseEnabled()) {
        PortDebug::SetMouseCaptured(PortDebug::MouseGameplayActive() && !PortDebug::Visible());
      } else
#endif
      {
        static SDL_Window* sCaptureWindow = nullptr;
        if (sCaptureWindow == nullptr) {
          sCaptureWindow = SDL_GetKeyboardFocus();
          if (sCaptureWindow == nullptr) {
            sCaptureWindow = SDL_GetMouseFocus();
          }
          if (sCaptureWindow == nullptr) {
            int count = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            if (windows != nullptr && count > 0) {
              sCaptureWindow = windows[0];
            }
            SDL_free(windows);
          }
        }
        if (sCaptureWindow != nullptr) {
          const bool focused = SDL_GetKeyboardFocus() == sCaptureWindow;
          const bool wantRelative = PortDebug::MouseAim() && PortDebug::MouseGameplayActive() &&
                                    !PortDebug::Visible() && focused;
#ifdef __ANDROID__
          // A free (hidden) cursor flung to the screen edge brings up the nav bar
          // or taskbar, so keep the pointer captured unless the overlay is open.
          const bool wantLock = !PortDebug::Visible() && focused;
#else
          const bool wantLock = wantRelative;
#endif
          if (SDL_GetWindowRelativeMouseMode(sCaptureWindow) != wantLock) {
            SDL_SetWindowRelativeMouseMode(sCaptureWindow, wantLock);
          }
          PortDebug::SetMouseCaptured(wantRelative && SDL_GetWindowRelativeMouseMode(sCaptureWindow));
          // Keep the cursor hidden during play; show it only over the overlay,
          // which is navigated with the mouse (and the controller), and while the
          // clickable update toast is up and the mouse isn't captured.
          const bool wantCursor =
              (PortDebug::Visible() || (PortDebug::UpdateToastShowing() && !wantLock)) &&
              SDL_GetKeyboardFocus() == sCaptureWindow;
          if (wantCursor) {
            SDL_ShowCursor();
          } else {
            SDL_HideCursor();
          }
        }
      }
      // Port: apply the selected aspect ratio; no-op unless it changed (e.g. the
      // debug overlay's aspect combo).
      ApplyAspectMode();
      // Port: feed the pad into ImGui's gamepad navigation (and toggle the
      // overlay with Back/Select) before the frame is built.
      PortDebug::UpdateControllerNav();
      // Port: feed the pad's or the phone's gyro into the aim.
      PortDebug::PollGyro();
      // Port: swap the HD texture set if the active controller changed, and
      // re-icon the prompts if a binding changed.
      PortTextures::Poll();
      PortPrompts::Poll();
      const uint64_t inputDoneNs = SDL_GetTicksNS();
      // Port: run ARAM transfer callbacks completed by Aurora's ARQ.
      aurora::phase::set(aurora::phase::Main, "audio and loader polling");
      ARQPoll();
      // Port: service the streamed-audio AI DMA callback on the main thread.
      AIPortPoll();
      archSupport->GetStopwatch2().Reset();
      aurora::phase::set(aurora::phase::Main, "resource loading (AsyncIdlePakLoading)");
      gpResourceFactory->GetResLoader().AsyncIdlePakLoading();
      if (gpMemoryCard == nullptr && gpResourceFactory->GetResLoader().AreAllPaksLoaded()) {
        MemoryCardInitializePump();
      }
      CARAMManager::CollectGarbage();
      CARAMToken::UpdateAllDMAs();
      aurora::phase::set(aurora::phase::Main, "game tick (UpdateTicks)");
      if (!archSupport->UpdateTicks()) {
        x160_24_finished = true;
      }
      // Track the step UpdateTicks actually used (fixed or adaptive).
      dt = archSupport->GetTickPeriod();
      // Advance the animation clock with simulation ticks so draw-time
      // animations stay real-time at any presentation frame rate.
      CGraphics::TickRenderTimings(sTicksAdvanced);
      double t1 = archSupport->GetStopwatch2().GetElapsedTime();
      AddFrameTime(xf0_tickTimes, t1 / dt);
      x118_averageTickTime = xf0_tickTimes.GetAverage().data();
      archSupport->GetStopwatch2().Reset();
      DoPredrawMetrics();

      if (logAudioTweaks) {
        logAudioTweaks = false;
        // rs_log_print(str.data());
      }
      // Port: BeginScene blocks until Aurora has a free frame slot, i.e. on
      // vsync or on a GPU that is behind. That wait is not draw work; counted
      // as such it zeroed the loader's budget below whenever presentation was
      // the bottleneck, and a big area's first load took 20 s and more.
      const double beginSceneStart = archSupport->GetStopwatch2().GetElapsedTime();
      const uint64_t tickDoneNs = SDL_GetTicksNS();
      if (!x160_26_screenFading && gpRender->BeginScene()) {
        const double beginSceneWait =
            archSupport->GetStopwatch2().GetElapsedTime() - beginSceneStart;
        // Port: Aurora frames are bracketed inside CGraphics::Begin/EndScene.
        float interpolation = archSupport->GetTickInterpolation();
        if (interpolation < 0.f)
          interpolation = 0.f;
        else if (interpolation > 1.f)
          interpolation = 1.f;
        if (PortDebug::FrameLimitEnabled())
          interpolation = -1.f;
        PortDebug::PresentOverride(interpolation);
        CCameraManager::SetPresentationInterpolation(interpolation);
        aurora::phase::set(aurora::phase::Main, "game draw");
        archSupport->GetIOWinManager().Draw();
        CCameraManager::SetPresentationInterpolation(-1.f);
        PortSpeedrunTimer::Draw();
        DrawDebugMetrics(t1, archSupport->GetStopwatch2());

        double t2 = archSupport->GetStopwatch2().GetElapsedTime();
        AddFrameTime(x104_drawTimes, t2 / dt);
        x11c_averageDrawTime = x104_drawTimes.GetAverage().data();

        uint idleMicros;
        double idleTime = (dt - (t1 + (t2 - beginSceneWait))) - 0.00075;
        if (idleTime > 0)
          idleMicros = idleTime * 1000000;
        else
          idleMicros = 0;
        AsyncIdle(idleMicros);

        gpRender->EndScene();
        aurora::phase::set(aurora::phase::Main, "after EndScene");
        presented = true;

        if (x161_24_gameFrameDrawn) {
          ++archSupport->GetFramesDrawn();
          x161_24_gameFrameDrawn = false;
        }
      } else {
        aurora::phase::set(aurora::phase::Main, "loader idle (frame not drawn)");
        gpResourceFactory->AsyncIdle(1000);
        // A minimized/paused window still services events and audio, but never
        // accumulates a frame's GX commands or advances delayed render frees.
        SDL_Delay(5);
      }

      archSupport->Update();
      for (uint i = 0; i < sTicksAdvanced; ++i) {
        CSfxManager::Update(dt);
        UpdateStreamedAudio(dt);
      }

      if (CheckTerminate())
        break;
      bool needsReset = false;
      if (archSupport->GetIOWinManager().IsEmpty()) {
        // rs_log_print("IOWinManager got empty. Resetting game architecture\n");
        needsReset = true;
      } else if (PortDebug::ConsumeResetRequest()) {
        // Debug overlay: restart the architecture, which returns to the menu.
        needsReset = true;
      } else if (CheckReset()) {
        // rs_log_print("Reset pressed...\n");
        needsReset = true;
      }
      if (needsReset) {
        x12c_restartMode = kRM_Default;
        CStreamAudioManager::StopAll();
        PADRecalibrate(0xf0000000);
        CGraphics::SetIsBeginSceneClearFb(true);
        if (CGraphics::BeginScene()) {
          CGraphics::EndScene();
        }
        CFrameDelayedKiller::StallAndFlushAllAllocations();

        archSupport = nullptr;
        CGameArchitectureSupport* tmp = rs_new CGameArchitectureSupport(x0_osContext);
        archSupport = tmp;
        x164_archSupport = archSupport.get();
        tmp->PreloadAudio();
      }
      CheckTweakManagerDebugOptions();

      // Taken before the frame cap, deliberately. Measuring after it folded the
      // pacing sleep into the frame's cost, so the one number being reported was
      // the presented rate whatever the machine's actual headroom was, and the
      // two could never be told apart - which is the only thing worth reporting
      // when a frame overruns its budget.
      const uint64_t workEndNs = SDL_GetTicksNS();
      aurora::phase::set(aurora::phase::Main, "frame pacing");
      if (PortDebug::FrameLimitEnabled() && !PortDebug::Turbo()) {
        nextFrameDeadline += framePeriodNs;
        const uint64_t now = SDL_GetTicksNS();
        if (nextFrameDeadline > now) {
          // SDL_DelayPrecise sleeps in 1 ms slices, about sixteen wake-ups a frame at
          // 60 Hz. One plain sleep up to 2 ms short of the deadline first leaves it only
          // the last stretch, which saves the wake-ups (and battery on phones).
          const uint64_t kCoarseMarginNs = 2000000;
          if (nextFrameDeadline - now > kCoarseMarginNs) {
            SDL_DelayNS(nextFrameDeadline - now - kCoarseMarginNs);
          }
          const uint64_t afterCoarse = SDL_GetTicksNS();
          if (nextFrameDeadline > afterCoarse) {
            SDL_DelayPrecise(nextFrameDeadline - afterCoarse);
          }
        } else if (now - nextFrameDeadline > framePeriodNs) {
          nextFrameDeadline = now;
        }
      } else {
        nextFrameDeadline = SDL_GetTicksNS();
      }
      PortDebug::RecordFrame(workEndNs - loopStartNs, sTicksAdvanced, presented);
      {
        // The whole iteration, pacing included: what a player sees as a frame.
        const uint64_t frameEndNs = SDL_GetTicksNS();
        milestoneWorstNs = std::max(milestoneWorstNs, frameEndNs - loopStartNs);
        if (s_frameLog == 1) {
          milestoneSpanStartNs = frameEndNs;
          milestoneSpanStartFrame = 1;
          milestoneWorstNs = 0;
        } else if (s_frameLog == 2 || s_frameLog == 10 || s_frameLog == 60 || s_frameLog == 300 ||
                   s_frameLog == 1800) {
          const double span = static_cast< double >(frameEndNs - milestoneSpanStartNs) / 1e9;
          PortLog::Write("port: frame %u at %.2f s (avg fps %.1f over the last %u frames, worst frame %.1f ms)\n",
                         s_frameLog, static_cast< double >(frameEndNs - loopBeganNs) / 1e9,
                         span > 0.0 ? (s_frameLog - milestoneSpanStartFrame) / span : 0.0,
                         s_frameLog - milestoneSpanStartFrame, static_cast< double >(milestoneWorstNs) / 1e6);
          milestoneSpanStartNs = frameEndNs;
          milestoneSpanStartFrame = s_frameLog;
          milestoneWorstNs = 0;
        }
      }
      // Port: a stalled loop shows up as input that does nothing and then lands
      // all at once, since SDL and ImGui keep queueing events meanwhile. Say
      // where the time went; on Android this is the only trace a report has.
      {
        static const bool sLogStalls = getenv("MP_NO_STALL_LOG") == nullptr;
        static uint32_t sSkipped = 0;
        static uint64_t sSkippedSinceNs = 0;
        const auto ms = [](uint64_t ns) { return static_cast< unsigned long long >(ns / 1000000ull); };
        // At most a line every two seconds: a device that is slow throughout
        // would otherwise write one per frame.
        static uint64_t sLastStallLogNs = 0;
        static uint32_t sStallsUnlogged = 0;
        if (sLogStalls && workEndNs - loopStartNs > 250000000ull) {
          if (sLastStallLogNs != 0 && workEndNs - sLastStallLogNs < 2000000000ull) {
            ++sStallsUnlogged;
          } else {
            PortLog::Write("MP stall: frame %u took %llu ms (events %llu, input %llu, tick %llu, draw %llu)%s, "
                           "%u more since the last line\n",
                           s_frameLog, ms(workEndNs - loopStartNs), ms(eventsDoneNs - loopStartNs),
                           ms(inputDoneNs - eventsDoneNs), ms(tickDoneNs - inputDoneNs),
                           ms(workEndNs - tickDoneNs), presented ? "" : ", not presented", sStallsUnlogged);
            sLastStallLogNs = workEndNs;
            sStallsUnlogged = 0;
          }
        }
        // Frames that are not presented never start an ImGui frame either, so
        // the overlay is deaf for as long as a run of them lasts.
        if (!presented) {
          if (sSkipped++ == 0) {
            sSkippedSinceNs = loopStartNs;
          }
        } else if (sSkipped != 0) {
          if (sLogStalls && workEndNs - sSkippedSinceNs > 250000000ull) {
            PortLog::Write("MP stall: %u frames not presented over %llu ms\n", sSkipped,
                           ms(workEndNs - sSkippedSinceNs));
          }
          sSkipped = 0;
        }
      }
      if (firstFrameNs == 0 && presented)
        firstFrameNs = SDL_GetTicksNS();
    }
    } catch (...) {
      // Port: an exception out of the loop would unwind into ~CResLoader, which
      // waits for paks that no longer load and hangs on the logo (issue #8).
      // Report it and crash instead.
      PortCrash::AbortOnException("the main loop");
    }
  // What the run cost. The frame log is only a counter, so nothing could report
  // this before; a run that never reached the loop has nothing to say.
  if (firstFrameNs != 0) {
    fprintf(stderr, "MP startup: first frame %llu ms after the main loop began, %u frames run\n",
            static_cast< unsigned long long >(firstFrameNs / 1000000ull), s_frameLog);
    // The whole loop's wall time, so a test run's speed (MP_TURBO or not) is a
    // number rather than a guess from the log's timestamps.
    fprintf(stderr, "MP run: %u frames in %.1f s\n", s_frameLog,
            static_cast< double >(SDL_GetTicksNS() - loopBeganNs) / 1e9);
  }
  // Which audio backend the run actually got, and at what rate. Nothing reported
  // this, so "is there any sound" was unanswerable from a run: a machine with no
  // server, a build without a backend and a correctly working setup all look the
  // same from the outside. On Linux the answer is normally pipewire or pulse,
  // whichever SDL reached first, and SDL loads them at runtime - so this is also
  // the only place a missing libpulse shows up.
  if (const char* audioDriver = SDL_GetCurrentAudioDriver(); audioDriver != nullptr) {
    fprintf(stderr, "MP audio: driver %s\n", audioDriver);
  } else {
    fprintf(stderr, "MP audio: no driver; SDL opened no audio device\n");
  }
  }
  ShutdownSubsystems();
  gameGlobalObjects = nullptr;
  CARAMManager::Shutdown();
  return 0;
}

// Port: the least time the resource loader gets per frame, in microseconds.
static const uint kPortMinIdleMicros = 2000;

void CMain::AsyncIdle(uint time) {
  if (time < 500) {
    uint total = 0;
    for (int i = 0; i < x130_frameTimes.capacity(); ++i) {
      total += x130_frameTimes[i];
    }
    if (total < 500 * x130_frameTimes.capacity()) {
      time = 500;
    } else {
      time = 0;
    }
  }
  x130_frameTimes[x15c_frameTimeIdx] = time;
  // Port: retail falls back to 500 us every other frame when a frame has no
  // idle time, about 30 resources a second: a whole area's dependencies then
  // take tens of seconds, and its doors stay shut until they are in. A fixed
  // floor costs nothing when the load list is empty and keeps a slow frame
  // from stalling streaming.
  if (time < kPortMinIdleMicros) {
    time = kPortMinIdleMicros;
  }
  gpResourceFactory->AsyncIdle(time);
  x15c_frameTimeIdx = x15c_frameTimeIdx + 1;
  if (x15c_frameTimeIdx >= x130_frameTimes.capacity()) {
    x15c_frameTimeIdx = 0;
  }
}

namespace rstl {
string string_l(const char* data) { return string(string::literal_t(), data); }

string operator+(const string& a, const string& b) {
  string result(a);
  result.append(b);
  return result;
}
} // namespace rstl

void CMain::AddWorldPaks() {
  rstl::rmemory_allocator allocator;
  rstl::string basePath = gpTweakGame->GetWorldPrefix();
  for (int i = 0; i < 9; ++i) {
    rstl::string pak =
        basePath +
        (i == 0 ? rstl::string_l("") : rstl::string(CBasics::Stringize("%d", i), -1, allocator));
    if (CDvdFile::FileExists((pak + rstl::string_l(".pak")).data())) {
      gpResourceFactory->GetResLoader().AddPakFileAsync(pak, false, true);
    }
  }
}

void CMain::EnsureWorldPakReady(CAssetId id) {
  CResLoader& resLoader = gpResourceFactory->GetResLoader();
  for (int i = 0; i < resLoader.GetPakCount(); ++i) {
    bool notInNameList = true;
    CPakFile* pakFile = resLoader.GetPakFile(i);
    if (pakFile->IsWorldPak()) {
      rstl::vector< rstl::pair< rstl::string, SObjectTag > > nameList = pakFile->NameList();
      rstl::vector< rstl::pair< rstl::string, SObjectTag > >::iterator cur = nameList.begin();
      while (cur != nameList.end()) {
        if (cur->second.GetId() == id) {
          notInNameList = false;
        }
        ++cur;
      }
      if (notInNameList) {
        pakFile->sub_8036742c();
      } else {
        pakFile->EnsureWorldPakReady();
      }
    }
  }
}

void CMain::EnsureWorldPaksReady(void) {
  CResLoader& resLoader = gpResourceFactory->GetResLoader();
  for (int i = 0; i < resLoader.GetPakCount(); ++i) {
    CPakFile* file = resLoader.GetPakFile(i);
    if (file->IsWorldPak()) {
      file->EnsureWorldPakReady();
    }
  }
}

void CMain::CheckTweakManagerDebugOptions() {}

void CMain::RefreshGameState() {
  CSystemState systemState = gpGameState->SystemState();
  uint saveIdx = gpGameState->SaveIdx();
  u64 cardSerial = gpGameState->CardSerial();
  rstl::vector< uchar > backupBuf = gpGameState->BackupBuf();
  CGameOptions gameOptions = gpGameState->GameOptions();
  x128_gameGlobalObjects->GameState() = nullptr;
  gpGameState = nullptr;
  {
    CMemoryInStream stream(backupBuf.data(), backupBuf.size(), CMemoryInStream::kOS_NotOwned);
    x128_gameGlobalObjects->GameState() = rs_new CGameState(stream, saveIdx);
  }
  gpGameState = x128_gameGlobalObjects->GameState().get();
  gpGameState->SystemState() = systemState;
  gpGameState->GameOptions() = gameOptions;
  gpGameState->GameOptions().EnsureOptions();
  gpGameState->CardSerial() = cardSerial;
  gpGameState->PlayerState()->SetIsFusionEnabled(gpGameState->SystemState().GetHasFusion());
}

#ifdef TARGET_PC
void CMain::PortLoadGameState(CInputStream& in) {
  CSystemState systemState = gpGameState->SystemState();
  uint saveIdx = gpGameState->SaveIdx();
  u64 cardSerial = gpGameState->CardSerial();
  CGameOptions gameOptions = gpGameState->GameOptions();
  x128_gameGlobalObjects->GameState() = nullptr;
  gpGameState = nullptr;
  x128_gameGlobalObjects->GameState() = rs_new CGameState(in, saveIdx);
  gpGameState = x128_gameGlobalObjects->GameState().get();
  gpGameState->SystemState() = systemState;
  gpGameState->GameOptions() = gameOptions;
  gpGameState->GameOptions().EnsureOptions();
  gpGameState->CardSerial() = cardSerial;
  gpGameState->PlayerState()->SetIsFusionEnabled(gpGameState->SystemState().GetHasFusion());
}
#endif

void CMain::StreamNewGameState(CInputStream& in, int saveIdx) {
  bool hasFusion = gpGameState->SystemState().GetHasFusion();
  x128_gameGlobalObjects->GameState() = nullptr;
  gpGameState = nullptr;
  x128_gameGlobalObjects->GameState() = rs_new CGameState(in, saveIdx);
  gpGameState = x128_gameGlobalObjects->GameState().get();
  gpGameState->SystemState().SetHasFusion(hasFusion);
  gpGameState->PlayerState()->SetIsFusionEnabled(gpGameState->SystemState().GetHasFusion());
  gpGameState->HintOptions().SetHintNextTime();
}

void CMain::ResetGameState() {
  CSystemState systemState = gpGameState->SystemState();
  CGameOptions gameOptions = gpGameState->GameOptions();
  x128_gameGlobalObjects->GameState() = nullptr;
  gpGameState = nullptr;
  x128_gameGlobalObjects->GameState() = rs_new CGameState();
  gpGameState = x128_gameGlobalObjects->GameState().get();
  gpGameState->SystemState() = systemState;
  gpGameState->GameOptions() = gameOptions;
  gpGameState->GameOptions().EnsureOptions();
  gpGameState->PlayerState()->SetIsFusionEnabled(gpGameState->SystemState().GetHasFusion());
}

void CMain::RegisterResourceTweaks() { x70_tweaks.RegisterResourceTweaks(); }

void CMain::UpdateStreamedAudio(float dt) { CStreamAudioManager::Update(dt); }
