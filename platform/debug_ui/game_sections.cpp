// F1 overlay: Sections of the Game and System pages: cutscenes, unlocks, speedrun, Discord,
// updates, the texture pack status and the overlay options. State and helpers shared with the other
// parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_paths.h"
#include "port_discord.h"
#include "port_update_check.h"
#include "port_livesplit.h"
#include "port_skip_cutscenes.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CMorphBall.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_misc.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#include <jni.h>
#endif

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>

namespace PortDebug {

void DrawCutscenesSection() {
  ImGui::SeparatorText("Cutscenes");
  const bool locked = BeginOriginalLocked();
  bool skippable = sSkippableCutscenes;
  if (ImGui::Checkbox("Skippable cutscenes", &skippable)) {
    SetSkippableCutscenes(skippable);
  }
  ItemHelp("Skip any cutscene with Start or A, even ones the game never lets you skip. Applies to "
           "rooms loaded afterwards.");
  if (PortSkipCutscenes::Forced()) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(on in randomized games)");
  }
  int elevatorRide = sElevatorRide;
  if (ImGui::Combo("Elevator ride", &elevatorRide, "Original\0Fast\0Skip\0")) {
    SetElevatorRide(static_cast< EElevatorRide >(elevatorRide));
  }
  ItemHelp("The ride between worlds. Original lasts at least 5 s; Fast ends about 2 s in, once the "
           "next area has loaded; Skip shows black until then.");
  int bootLogos = sBootLogos;
  if (ImGui::Combo("Boot logos", &bootLogos, "Show\0Skip with A or Start\0Off\0")) {
    SetBootLogos(static_cast< EBootLogos >(bootLogos));
  }
  ItemHelp("The logos at launch. Skip with A or Start ends the current logo; Off goes straight to "
           "the opening. Applies at the next launch.");
  if (ImGui::SliderInt("Scan speed", &sScanSpeed, kScanSpeedMin, kScanSpeedMax, "%d%%",
                       ImGuiSliderFlags_AlwaysClamp)) {
    MarkDirty();
  }
  ItemHelp("How fast the scan visor fills its bar: 200% scans in half the time. What a scan "
           "records is unchanged.");
  if (ImGui::SliderInt("Hint delay", &sHintDelay, kHintDelayMin, kHintDelayMax,
                       sHintDelay == 0 ? "Game default" : "%d min", ImGuiSliderFlags_AlwaysClamp)) {
    MarkDirty();
  }
  ItemHelp("The longest a hint waits before it first shows. Game default keeps the game's own "
           "timing; a shorter wait also applies to hints already waiting in your save. Needs the "
           "in-game hint system on.");
  ImGui::BeginDisabled(gpGameState == nullptr || !gpGameState->GameOptions().GetIsHintSystemEnabled());
  if (ImGui::Button("Show current hint")) {
    RequestShowHint();
  }
  ImGui::EndDisabled();
  ItemHelp("Shows the hint the game would show next, now: the one on screen again, or the one "
           "due soonest. Needs the in-game hint system on.");
  bool beamDoors = sBeamDoorsStayUnlocked;
  if (ImGui::Checkbox("Unlock beam doors once opened", &beamDoors)) {
    SetBeamDoorsStayUnlocked(beamDoors);
  }
  ItemHelp("A beam door you've opened stays a plain blue door for the rest of the save, on both "
           "sides, so any beam opens it. Missile, Bomb and Disabled doors and blast shields are "
           "unaffected. Applies from the next door you open.");
  bool stackBeams = sStackBeams;
  if (ImGui::Checkbox("Stack beams", &stackBeams)) {
    SetStackBeams(stackBeams);
  }
  ItemHelp("Each normal or charged shot also fires every other beam you own. Charge combos, "
           "missiles and the Phazon Beam are unchanged.");
  int walkSpeed = sWalkSpeed;
  if (ImGui::SliderInt("Walk speed", &walkSpeed, kWalkSpeedMin, kWalkSpeedMax, "%d%%",
                       ImGuiSliderFlags_AlwaysClamp)) {
    SetWalkSpeed(walkSpeed);
  }
  ItemHelp("How fast Samus moves on foot, walking, strafing and circle-strafing, in percent of "
           "the original speed. Jumps, dashes, turning and the Morph Ball are unchanged.");
  EndOriginalLocked(locked);

  // Not under the Original experience lock: the stored choice applies there too.
  ImGui::SeparatorText("Revision compatibility");
  const bool revisionForced = PortSkipCutscenes::Forced();
  bool revisionCompat = sRevision100Compatibility || revisionForced;
  ImGui::BeginDisabled(revisionForced);
  if (ImGui::Checkbox("1.00 sequence-break compatibility", &revisionCompat)) {
    SetRevision100Compatibility(revisionCompat);
  }
  ImGui::EndDisabled();
  ItemHelp(
      "Restores revision-specific sequence breaks on newer and PAL discs. Needs a game reload.");
  if (revisionForced) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(automatic in randomizer and Archipelago games)");
  }
}

void WearFusionSuit(CStateManager& mgr, bool wearing) {
  if (gpGameState != nullptr) {
    gpGameState->SystemState().SetHasFusion(wearing);
  }
  mgr.PlayerState()->SetIsFusionEnabled(wearing);
  mgr.Player()->AsyncLoadSuit(mgr);
  if (mgr.Player()->GetMorphballTransitionState() != CPlayer::kMS_Unmorphed) {
    mgr.Player()->MorphBall()->LoadMorphBallModel(mgr);
  }
}

void DrawUnlocksSection() {
  ImGui::SeparatorText("Unlocks");
  const bool locked = BeginOriginalLocked();
  bool hardMode = sUnlockHardMode;
  if (ImGui::Checkbox("Hard mode", &hardMode)) {
    SetUnlockHardMode(hardMode);
  }
  ImGui::SetItemTooltip("Offered when starting a file, as after finishing the game.");
  ImGui::SameLine();
  bool fusionSuit = sUnlockFusionSuit;
  if (ImGui::Checkbox("Fusion Suit", &fusionSuit)) {
    SetUnlockFusionSuit(fusionSuit);
    // The suit choice itself is saved; without the unlock (or a real link)
    // there is no menu left to switch it back off, so drop it here.
    if (!fusionSuit && gpGameState != nullptr &&
        !(gpGameState->SystemState().GetFusionLinked() &&
          gpGameState->SystemState().GetNormalModeBeat())) {
      gpGameState->SystemState().SetHasFusion(false);
      gpGameState->PlayerState()->SetIsFusionEnabled(false);
    }
  }
  ImGui::SetItemTooltip("Under Fusion Bonus; retail needs a GBA link to Metroid Fusion.");
  ImGui::SameLine();
  // The Fusion Bonus menu's choice, switched in game: the suit, arm and ball
  // load again as after a suit pickup.
  {
    CStateManager* mgr = StateManager();
    const bool available = gpGameState != nullptr &&
                           (UnlockFusionSuit() || (gpGameState->SystemState().GetFusionLinked() &&
                                                   gpGameState->SystemState().GetNormalModeBeat()));
    bool wearing = gpGameState != nullptr && gpGameState->PlayerState()->GetIsFusionEnabled();
    ImGui::BeginDisabled(!available || mgr == nullptr);
    if (ImGui::Checkbox("Wear Fusion Suit", &wearing)) {
      WearFusionSuit(*mgr, wearing);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Switches between the Fusion Suit and the normal suits now.\nNeeds the "
                          "Fusion Suit unlocked and a game running.");
  }
  ImGui::SameLine();
  bool galleries = sUnlockGalleries;
  if (ImGui::Checkbox("Image galleries", &galleries)) {
    SetUnlockGalleries(galleries);
  }
  ImGui::SetItemTooltip("All four of them.");
  ImGui::TextDisabled("What finishing the game unlocks. Not written into the save.");
  ItemHelp("Turning an option off locks it again. Metroid (NES) stays locked: its emulator can't run "
           "in the port.");
  EndOriginalLocked(locked);

}

void DrawSpeedrunSection() {
  ImGui::SeparatorText("Speedrun");
  bool timer = sSpeedrunTimer;
  if (ImGui::Checkbox("On-screen in-game time", &timer)) {
    SetSpeedrunTimer(timer);
  }
  ItemHelp("The play time the save file shows: it stops in cutscenes, menus and loads.");
  bool liveSplit = sLiveSplit;
  if (ImGui::Checkbox("LiveSplit", &liveSplit)) {
    SetLiveSplit(liveSplit);
  }
  ItemHelp("In LiveSplit: right-click, Control > Start TCP Server (port 16834), and compare "
           "against Game Time. A new file starts the timer; it splits on the final blow.");
  SameLineAfterHelp();
  switch (PortLiveSplit::Status()) {
  case PortLiveSplit::kStatus_Off:
    ImGui::TextDisabled("off");
    break;
  case PortLiveSplit::kStatus_Connecting:
    ImGui::TextUnformatted("connecting...");
    break;
  case PortLiveSplit::kStatus_Connected:
    ImGui::TextColored(ThemeGoodColor(), "connected");
    break;
  case PortLiveSplit::kStatus_Failed:
    ImGui::TextColored(ThemeBadColor(), "%s", PortLiveSplit::LastError().c_str());
    break;
  }
  // Applied when the field loses focus; until then the text is left alone.
  static char address[128];
  static bool editingAddress = false;
  if (!editingAddress) {
    std::snprintf(address, sizeof(address), "%s", sLiveSplitAddress.c_str());
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
  ImGui::InputText("Server (host:port)", address, sizeof(address));
  editingAddress = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    SetLiveSplitAddress(address);
  }
  ImGui::SameLine();
  bool splitUpgrades = sLiveSplitSplitUpgrades;
  if (ImGui::Checkbox("Split on upgrades", &splitUpgrades)) {
    sLiveSplitSplitUpgrades = splitUpgrades;
    ApplyLiveSplit();
    MarkDirty();
  }
  ImGui::SetItemTooltip("Also splits on each new upgrade or artifact (not expansions or energy tanks).");

}

void DrawDiscordSection() {
  if (PortDiscord::Supported()) {
    ImGui::SeparatorText("Discord");
    bool discord = sDiscord;
    if (ImGui::Checkbox("Rich Presence", &discord)) {
      SetDiscordPresence(discord);
    }
    ImGui::SetItemTooltip("Shows the room, world, energy, missiles and item percentage on your "
                          "Discord profile while the Discord app runs.");
    ImGui::SameLine();
    switch (PortDiscord::Status()) {
    case PortDiscord::kStatus_Off:
      ImGui::TextDisabled("off");
      break;
    case PortDiscord::kStatus_Connecting:
      ImGui::TextUnformatted("connecting...");
      break;
    case PortDiscord::kStatus_Connected:
      ImGui::TextColored(ThemeGoodColor(), "connected");
      break;
    case PortDiscord::kStatus_Failed:
      ImGui::TextColored(ThemeBadColor(), "%s", PortDiscord::LastError().c_str());
      break;
    }
    if (sDiscord) {
      ImGui::TextDisabled("Showing: %s", PortDiscord::CurrentText().c_str());
    }
  }

}

void DrawUpdateSection() {
  ImGui::SeparatorText("Updates");
  bool check = sUpdateCheck;
  if (ImGui::Checkbox("Check for updates", &check)) {
    sUpdateCheck = check;
    ApplyUpdateCheck();
    PortUpdateCheck::CheckNow(); // switched on: check now, not next launch
    MarkDirty();
  }
  ImGui::SetItemTooltip("Asks GitHub at launch whether a newer release is out. Nothing else is sent.");
  ImGui::SameLine();
  const PortUpdateCheck::EStatus status = PortUpdateCheck::Status();
  switch (status) {
  case PortUpdateCheck::kStatus_Off:
    ImGui::TextDisabled(sUpdateCheck ? "off (MP_UPDATE_CHECK=0)" : "off");
    break;
  case PortUpdateCheck::kStatus_Checking:
    ImGui::TextUnformatted("checking...");
    break;
  case PortUpdateCheck::kStatus_UpToDate:
    ImGui::TextColored(ThemeGoodColor(), "up to date");
    break;
  case PortUpdateCheck::kStatus_Available:
    ImGui::TextColored(ThemeGoodColor(), "%s is out", PortUpdateCheck::Latest().version.c_str());
    break;
  case PortUpdateCheck::kStatus_Failed:
    ImGui::TextColored(ThemeBadColor(), "%s", PortUpdateCheck::LastError().c_str());
    break;
  }
  if (status == PortUpdateCheck::kStatus_Off) {
    return;
  }
  ImGui::BeginDisabled(status == PortUpdateCheck::kStatus_Checking);
  if (ImGui::Button("Check now")) {
    PortUpdateCheck::CheckNow();
  }
  ImGui::EndDisabled();
  if (status == PortUpdateCheck::kStatus_Available) {
    ImGui::SameLine();
    if (ImGui::Button("Open release page")) {
      SDL_OpenURL(PortUpdateCheck::Latest().url.c_str());
    }
  }
  if (const int64_t checked = PortUpdateCheck::LastChecked(); checked > 0) {
    const time_t when = static_cast< time_t >(checked);
    char text[64] = {};
    if (const std::tm* local = std::localtime(&when)) {
      std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", local);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("last checked %s", text);
  }
}

// The texture pack import's progress: set from the Java copy thread on Android,
// from the zip import thread on desktop.
std::mutex sTexturePackStatusMutex;
std::string sTexturePackStatus;

std::string TexturePackStatus() {
  std::lock_guard lock(sTexturePackStatusMutex);
  return sTexturePackStatus;
}

void SetTexturePackStatus(std::string status) {
  std::lock_guard lock(sTexturePackStatusMutex);
  sTexturePackStatus = std::move(status);
}

#if defined(__ANDROID__)

// MetroidPrimeActivity.pickTexturePack opens the system folder (or, with zip,
// file) picker and copies the chosen pack in the background, reporting back
// through the nativeTexturePack* functions below.
void PickTexturePack(bool zip) {
  const char* failed = zip ? "Could not open the file picker." : "Could not open the folder picker.";
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    SetTexturePackStatus(failed);
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickTexturePack", "(Ljava/lang/String;Z)V");
  if (method != nullptr) {
    // The pack is copied into the data folder, wherever that is.
    jstring folder = env->NewStringUTF(PortPaths::UserFolder().c_str());
    env->CallVoidMethod(activity, method, folder, static_cast< jboolean >(zip));
    env->DeleteLocalRef(folder);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    SetTexturePackStatus(failed);
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#endif

// The look of this overlay: the floating window, the theme and the menu sounds.
void DrawOverlaySection() {
  ImGui::SeparatorText("Overlay");
#if !defined(__ANDROID__)
  if (ImGui::Checkbox("Overlay as a floating window", &sOverlayWindowed)) {
    MarkDirty();
  }
#endif
  {
    int theme = sUiTheme;
    if (ImGui::Combo("Menu theme", &theme, "Auto\0Metroid Prime\0Remastered\0Plain\0")) {
      sUiTheme = theme;
      MarkDirty();
    }
    ItemHelp("The overlay's look. Auto uses the Remastered style while the Remastered import is "
             "loaded, else the Metroid Prime style; Plain is stock Dear ImGui.");
    if (ImGui::Checkbox("Menu sounds", &sUiSounds)) {
      MarkDirty();
    }
    ItemHelp("The game's own menu sounds for opening, choosing and moving through the overlay. Off "
             "with the Plain theme.");
  }
}

} // namespace PortDebug
