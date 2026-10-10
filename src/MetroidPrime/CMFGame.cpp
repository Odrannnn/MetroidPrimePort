#include "MetroidPrime/CMFGame.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "MetroidPrime/CMFGameLoader.hpp"
#include "MetroidPrime/CMemoryCard.hpp"

#include "Kyoto/Audio/CMidiManager.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/CFrameDelayedKiller.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "MetroidPrime/CArchitectureQueue.hpp"
#include "MetroidPrime/CDecalManager.hpp"
#include "MetroidPrime/CInGameGuiManager.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CCinematicCamera.hpp"
#include "MetroidPrime/Decode.hpp"
#include "MetroidPrime/DefaultWorld.hpp"
#include "MetroidPrime/Player/CMorphBall.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Player/CPlayerVisor.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "rstl/math.hpp"

#include "port_debug.h"
#include "port_freecam.h"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "port_room_env.h"
#include <dolphin/gx/GXExtra.h>

CMFGame::CMFGame(rstl::ncrc_ptr< CStateManager > stateManager,
                 rstl::ncrc_ptr< CInGameGuiManager > guiManager,
                 CArchitectureQueue& architectureQueue)
: CIOWin(rstl::string_l("CMFGame"))
, mStateManager(stateManager)
, mGuiManager(guiManager)
, mFlowState(kGFS_InGame)
, mSkippedCineCam(kInvalidUniqueId)
, mInitialized(false)
, mPlayerAlive(true) {
  gpMain->SetGameFlowBuilt(true);
}

CMFGame::~CMFGame() {
  gpMain->SetGameFlowBuilt(false);
  gpMain->SetScreenFading(false);
  CDecalManager::Reinitialize();
}

void CMFGame::Touch() const {
  CStateManager* mgr = &*mStateManager;
  mgr->TouchSky();
  mgr->TouchPlayerActor();

  CPlayer* player = mgr->Player();
  bool touchModel = false;
  bool touchBall = false;
  bool touchGun = false;

  switch (player->GetMorphballTransitionState()) {
  case CPlayer::kMS_Unmorphed:
    touchGun = true;
    break;
  case CPlayer::kMS_Morphed:
    touchBall = true;
    break;
  case CPlayer::kMS_Morphing:
    touchBall = true;
    touchModel = true;
    break;
  case CPlayer::kMS_Unmorphing:
    touchGun = true;
    touchModel = true;
    break;
  }

  if (touchGun) {
    player->GetPlayerGun()->TouchModel(*mgr);
  }

  if (touchModel) {
    player->ModelData()->Touch(*mgr, 0);
  }

  if (touchBall) {
    player->GetMorphBall()->TouchModel(*mgr);
  }
}

CIOWin::EMessageReturn CMFGame::OnMessage(const CArchitectureMessage& message,
                                          CArchitectureQueue& queue) {
  switch (message.GetType()) {
  case kAM_FrameBegin: {
    const CArchMsgParmInt32& parm = MakeMsg::GetParmFrameBegin(message);
    mStateManager->FrameBegin(parm.GetInt32());
    break;
  }
  case kAM_TimerTick: {
    bool wasInitialized = mInitialized;
    mInitialized = true;
    float dt = MakeMsg::GetParmTimerTick(message).GetReal();
    if (PortDebug::SkipCutscenes() && mFlowState == kGFS_InGame) {
      const CGameCamera& camera =
          mStateManager->GetCameraManager()->GetCurrentCamera(*mStateManager);
      if (const CCinematicCamera* const cineCam = TCastToConstPtr< CCinematicCamera >(camera)) {
        if (mStateManager->GetCinematicSkipObject() != kInvalidUniqueId) {
          CMidiManager::StopAll();
          mSkippedCineCam = cineCam->GetUniqueId();
          mFlowState = kGFS_CinematicSkip;
          mCineSkipTime = 0.f;
        } else {
          // The opening cutscene deliberately has no skip object, so fast-forward
          // it instead of waiting out the whole sequence.
          dt *= PortDebug::CutsceneSpeed();
        }
      }
    }
    switch (mFlowState) {
    case kGFS_CinematicSkip: {
      mCineSkipTime += dt;
      const CGameCamera& camera =
          mStateManager->GetCameraManager()->GetCurrentCamera(*mStateManager);
      const CCinematicCamera* const cineCam = TCastToConstPtr< CCinematicCamera >(camera);
      if ((mCineSkipTime >= 1.f && mStateManager->SpecialSkipCinematic()) || !cineCam ||
          ((cineCam->GetFlags() & 0x10) && mSkippedCineCam != cineCam->GetUniqueId())) {
        gpMain->SetScreenFading(false);
        mFlowState = kGFS_InGame;
        mGuiManager->StartFadeIn();
        mSkippedCineCam = kInvalidUniqueId;
        break;
      }
    }
    // Fall through.
    case kGFS_InGame:
      mStateManager->SetRandomAvailable(true);
      switch (mStateManager->GetDeferredStateTransition()) {
      case kSMT_InGame:
        mStateManager->Update(dt);
        if (mStateManager->GetWantsToQuit())
          CGraphics::SetIsBeginSceneClearFb(false);
        break;
      case kSMT_MapScreen:
        EnterMapScreen();
        break;
      case kSMT_PauseGame:
        PauseGame();
        break;
      case kSMT_LogBook:
        EnterLogBook();
        break;
      case kSMT_SaveGame:
        SaveGame();
        break;
      case kSMT_MessageScreen:
        EnterMessageScreen(mStateManager->GetHUDMessageTime());
        break;
      }
      if (mPlayerAlive && !mStateManager->GetPlayerState()->IsAlive())
        PlayerDied();
      mStateManager->SetRandomAvailable(false);
      break;
    case kGFS_Paused:
      if (mGuiManager->IsInGameplayStateNotTransitioning()) {
        mStateManager->SetInSaveUI(mGuiManager->IsInSaveUI());
        UnpauseGame();
        if (mStateManager->GetPauseHUDMessage() != kInvalidAssetId)
          mStateManager->IncrementHUDMessageFrameCounter();
      }
      break;
    case kGFS_PlayerDied:
      if (mStateManager->GetPlayer()->IsPlayerDeadEnough()) {
        gpMain->SetRestartMode(CMain::kRM_LoseGame);
        queue.Push(MakeMsg::CreateQuitGameplay(kAMT_Game));
      } else {
        mStateManager->SetRandomAvailable(true);
        mStateManager->Update(dt);
        mStateManager->SetRandomAvailable(false);
      }
      break;
    }
    CStateManager& mgr = *mStateManager;
    const bool mouseActive = mFlowState == kGFS_InGame && mgr.GetPlayer()->MouseControlsAllowed(mgr);
    if (!mouseActive && PortDebug::MouseAim() && PortDebug::MouseGameplayActive()) {
      mgr.Player()->PlayerGun()->CancelMouseInput(mgr);
    }
    PortDebug::SetMouseGameplayActive(mouseActive);
    mGuiManager->Update(mgr, dt, queue, IsCameraActiveFlow());
    if (!wasInitialized)
      gpGameState->WorldTransitionManager()->EndTransition();
    return kMR_Exit;
  }
  case kAM_UserInput: {
    if (!mInitialized)
      break;
    CArchMsgParmUserInput parm = MakeMsg::GetParmUserInput(message);
    CFinalInput input = parm.GetUserInput();
    if (mFlowState == kGFS_InGame) {
      if (input.ControllerNumber() == 0) {
        const CGameCamera& camera =
            mStateManager->GetCameraManager()->GetCurrentCamera(*mStateManager);
        const CCinematicCamera* const cineCam = TCastToConstPtr< CCinematicCamera >(camera);
#ifdef TARGET_PC
        // A skips a skippable cinematic too (it does nothing else while one plays).
        const bool skipPress = input.PStart() || (cineCam && input.PA());
#else
        const bool skipPress = input.PStart();
#endif
        if (skipPress) {
          if (cineCam && mStateManager->GetCinematicSkipObject() != kInvalidUniqueId) {
            CMidiManager::StopAll();
            mSkippedCineCam = cineCam->GetUniqueId();
            mFlowState = kGFS_CinematicSkip;
            mCineSkipTime = 0.f;
            break;
          } else if (!cineCam) {
            mStateManager->DeferStateTransition(kSMT_PauseGame);
          }
        } else if (input.PZ() && !cineCam && mStateManager->CanShowMapScreen()) {
          mStateManager->DeferStateTransition(kSMT_MapScreen);
        }
      }
      mStateManager->SetRandomAvailable(true);
      mStateManager->ProcessInput(input);
      mStateManager->SetRandomAvailable(false);
    }
    mGuiManager->ProcessControllerInput(*mStateManager, input, queue);
    break;
  }
  case kAM_FrameEnd:
    mStateManager->FrameEnd();
    if (mStateManager->GetWantsToQuit())
      queue.Push(MakeMsg::CreateQuitGameplay(kAMT_Game));
    break;
  case kAM_QuitGameplay:
    CFrameDelayedKiller::StallAndFlushAllAllocations();
    return kMR_RemoveIOWin;
  }
  return kMR_Normal;
}

void CMFGame::Draw() const {
  if (!mInitialized) {
    return;
  }

  Touch();

  if (mGuiManager->GetIsGameDraw()) {
    gpMain->SetGameFrameDrawn(true);
    mStateManager->PreRender();
#ifdef TARGET_PC
    {
      // Remastered's character backlight hint of the camera's room, before the models draw.
      struct Layers {
        CScriptLayerManager* layers;
        TAreaId area;
      } layers{mStateManager->WorldLayerState().GetPtr(), mStateManager->GetNextAreaId()};
      PortRoomEnv::UpdateBacklight(
          [](int32_t layer, void* context) {
            const Layers& l = *static_cast< const Layers* >(context);
            return l.layers == nullptr || l.area == kInvalidAreaId ||
                   l.layers->IsLayerActive(l.area, TLayerId(layer));
          },
          &layers);
    }
#endif
#ifdef TARGET_PC
    // Remastered's bloom and colour grade, over the world and under the visor. Thermal and
    // X-ray draw their own picture, not the room's exposed light, so they keep neither.
    const auto postProcess = [this](bool measure) {
      const CPlayerState::EPlayerVisor visor = mStateManager->GetPlayerState()->GetActiveVisor(*mStateManager);
      if (visor != CPlayerState::kPV_Thermal && visor != CPlayerState::kPV_XRay) {
        float threshold = 0.f;
        float tints[5][3] = {};
        float tone[3][4] = {};
        const bool toned = PortRoomEnv::Tone(tone);
        const bool bloom = toned && PortRoomEnv::Bloom(threshold, tints);
        struct Layers {
          CScriptLayerManager* layers;
          TAreaId area;
        } layers{mStateManager->WorldLayerState().GetPtr(), mStateManager->GetNextAreaId()};
        uint32_t gradeA = 0;
        uint32_t gradeB = 0;
        float gradeWeight = 0.f;
        PortRoomEnv::ColorGrade(
            [](int32_t layer, void* context) {
              const Layers& l = *static_cast< const Layers* >(context);
              return l.layers == nullptr || l.area == kInvalidAreaId ||
                     l.layers->IsLayerActive(l.area, TLayerId(layer));
            },
            &layers, gradeA, gradeB, gradeWeight);
        // The picture is measured for the next frames' auto exposure (UpdateFrame).
        GXPortPostProcess(bloom, threshold, tints, tone, gradeA, gradeB, gradeWeight,
                          toned && measure ? PortRoomEnv::MeasureExposure() : 0.f);
      }
    };
    // Sharp scan window: the window's picture is the world drawn again through a zoomed
    // camera, instead of the screen's centre stretched (the copy clears the EFB after it).
    float scanZoom = 1.f;
    int scanWidth = 0;
    int scanHeight = 0;
    const CPlayerVisor* scanVisor = mGuiManager->PortPlayerVisor();
    if (scanVisor != nullptr && !PortFreeCam::Active() &&
        mGuiManager->PortPlayerVisorDrawn(*mStateManager) &&
        scanVisor->PortScanZoom(*mStateManager, scanZoom, scanWidth, scanHeight)) {
      CStateManager::sPortViewZoom = scanZoom;
      mStateManager->DrawWorld();
      postProcess(false);
      mGuiManager->PortDrawScanZoomReticles(*mStateManager);
      CStateManager::sPortViewZoom = 1.f;
      CPlayerVisor::PortCopyScanZoom(scanWidth, scanHeight, scanZoom);
    }
#endif
    mStateManager->DrawWorld();
    (void)mStateManager->GetPlayer()->IsPlayerDeadEnough();
#ifdef TARGET_PC
    postProcess(true);
#endif
  }

  mGuiManager->PreDraw(*mStateManager, IsCameraActiveFlow());
#ifdef TARGET_PC
  // The debug free camera looks at the world, not through the visor.
  if (!PortFreeCam::Active() || mFlowState != kGFS_InGame)
#endif
  mGuiManager->Draw(*mStateManager);
#ifdef TARGET_PC
  // A zoomed copy the visor didn't draw this frame must not show on a later one.
  CPlayerVisor::PortDropScanZoom();
#endif

  if (mFlowState == kGFS_CinematicSkip) {
    const float intensity = rstl::min_val(1.f, 1.f - mCineSkipTime);
    CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Multiply,
                                  CCameraFilterPass::kFS_Fullscreen,
                                  CColor(intensity, intensity, intensity, intensity), nullptr, 1.f);
  }
}

void CMFGame::EnterMapScreen() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_MapScreen);
  mStateManager->SetInMapScreen(true);
}

void CMFGame::PauseGame() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_PauseGame);
}

void CMFGame::EnterLogBook() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_PauseLogBook);
}

void CMFGame::SaveGame() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_PauseSaveGame);
}

void CMFGame::EnterMessageScreen(const float time) {
  mFlowState = kGFS_Paused;
  mGuiManager->ShowPauseGameHudMessage(*mStateManager, mStateManager->GetPauseHUDMessage(), time);
}

void CMFGame::UnpauseGame() {
  mFlowState = kGFS_InGame;
  CSfxManager::SetChannel(CSfxManager::kSC_Game);
  mStateManager->DeferStateTransition(kSMT_InGame);
}

void CMFGame::PlayerDied() {
  mFlowState = kGFS_PlayerDied;
  mPlayerAlive = false;
}

bool CMFGame::IsCameraActiveFlow() const {
  const bool ret = (mFlowState == kGFS_InGame || mFlowState == kGFS_PlayerDied);
  return ret;
}

extern int gResFactoryUnknown;

CMFGameLoader::CMFGameLoader()
: CIOWin(rstl::string_l("CMFGameLoader"))
, x2c_24_initialized(false)
, x2c_25_transitionFinished(false) {
  gResFactoryUnknown = 1;
  CModel::DisableTextureTimeout();
  if (gpMain->GetRestartMode() == CMain::kRM_Default ||
      gpMain->GetRestartMode() == CMain::kRM_StateSetter) {
    CAssetId worldId = gpGameState->CurrentWorldAssetId();
    if (gpMemoryCard->HasSaveWorldMemory(worldId)) {
      const CSaveWorldMemory& memory = gpMemoryCard->GetSaveWorldMemory(worldId);
      if (memory.GetWorldNameId() != kInvalidAssetId) {
        CWorldTransManager& trans = *gpGameState->WorldTransitionManager();
        trans.EnableTransition(0xB7BBD0B4, memory.GetWorldNameId(), 1, false, 0.1f, 16.f, 1.f);
      }
    }
  }
  CAssetId worldId = gpGameState->CurrentWorldAssetId();
  if (worldId == skDefaultWorld.GetId() &&
      gpGameState->GetCurrentWorldState().GetCurrentArea() == TAreaId(0)) {
    const SObjectTag* tag = gpResourceFactory->GetResourceIdByName("STRG_IntroLevelLoad");
    if (tag) {
      CWorldTransManager& trans = *gpGameState->WorldTransitionManager();
      trans.EnableTransition(kInvalidAssetId, tag->GetId(), 0, false, 0.1f, 16.f, 1.f);
    }
  }
  x2c_24_initialized = true;
}

CMFGameLoader::~CMFGameLoader() {
  CGraphics::SetIsBeginSceneClearFb(true);
  gResFactoryUnknown = 2;
}

void CMFGameLoader::MakeLoadDependancyList() {
  static const char* skPaksToLoad[] = {"aram:TestAnim", "aram:SamusGun", "aram:SamGunFx", nullptr};
  int count = 0;
  for (const char** pak = skPaksToLoad; *pak; ++pak) {
    const rstl::vector< CAssetId >* tags =
        gpResourceFactory->GetTagListForFile(rstl::string_l(*pak));
    if (tags)
      count += tags->size();
  }
  x1c_loadList.reserve(count);
  for (const char** pak = skPaksToLoad; *pak; ++pak) {
    const rstl::vector< CAssetId >* tags =
        gpResourceFactory->GetTagListForFile(rstl::string_l(*pak));
    if (tags) {
      for (AUTO(it, tags->begin()); it != tags->end(); ++it) {
        const CAssetId& id = *it;
        SObjectTag tag(gpResourceFactory->GetResLoader().GetResourceTypeById(id), id);
        CToken token = gpSimplePool->GetObj(tag);
        x1c_loadList.push_back(token);
      }
    }
  }
}

CIOWin::EMessageReturn CMFGameLoader::OnMessage(const CArchitectureMessage& message,
                                                CArchitectureQueue& queue) {
  rstl::ncrc_ptr< CWorldTransManager >& transManager = gpGameState->WorldTransitionManager();
  if (message.GetType() == kAM_TimerTick) {
    float dt = MakeMsg::GetParmTimerTick(message).GetReal();
    if (!x2c_24_initialized) {
      if (x1c_loadList.empty()) {
        MakeLoadDependancyList();
        transManager->StartTransition();
        return kMR_Exit;
      }
      int loadingCount = 0;
      for (AUTO(it, x1c_loadList.begin()); it != x1c_loadList.end(); ++it) {
        it->Lock();
        if (!it->IsLoaded())
          ++loadingCount;
      }
      transManager->Update(dt);
      if (loadingCount)
        return kMR_Exit;
      x2c_24_initialized = true;
    } else {
      transManager->Update(dt);
    }
    if (x14_stateManager.IsNull()) {
      transManager->WaitForModelsAndTextures();
      CWorldState& world = gpGameState->CurrentWorldState();
      const rstl::ncrc_ptr< CStateManager >& stateManager = rstl::ncrc_ptr< CStateManager >(
          rs_new CStateManager(world.Mailbox(), world.MapWorldInfo(), gpGameState->PlayerState(),
                               transManager, world.GetLayerState()));
      x14_stateManager = stateManager;
    }
    if (!x14_stateManager->IsFullyInitialized()) {
      CWorldState& world = gpGameState->CurrentWorldState();
      x14_stateManager->InitializeState(world.GetWorldAssetId(), world.GetCurrentArea(),
                                        world.GetDesiredAreaAssetId());
      return kMR_Exit;
    }
    if (x18_guiManager.IsNull()) {
      gpGameState->CurrentWorldState().SetDesiredAreaAssetId(kInvalidAssetId);
      x18_guiManager = rs_new CInGameGuiManager(*x14_stateManager, queue);
    }
    if (!x18_guiManager->CheckLoadComplete(*x14_stateManager))
      return kMR_Exit;
    x1c_loadList.clear();
    transManager->StartTextFadeOut();
    x2c_25_transitionFinished = transManager->IsTransitionFinished();
    return kMR_Exit;
  } else if (message.GetType() == kAM_FrameEnd) {
    if (x2c_25_transitionFinished) {
      CIOWin* game = rs_new CMFGame(x14_stateManager, x18_guiManager, queue);
      queue.Push(MakeMsg::CreateCreateIOWin(kAMT_IOWinManager, 10, 1000, game));
      CModel::EnableTextureTimeout();
      return kMR_RemoveIOWinAndExit;
    }
  }
  return kMR_Exit;
}

void CMFGameLoader::Draw() const { gpGameState->WorldTransitionManager()->Draw(); }
