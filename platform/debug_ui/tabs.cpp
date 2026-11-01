// F1 overlay: The pages assembled from their sections (Game, Controls, Video, Mods, System), and
// the Game and System pages' own sections. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_photo.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_config_reset.h"
#include "port_input_devices.h"
#include "port_input_remap_ui.h"
#include "port_data_folder.h"
#include "port_build_info.h"

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_events.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include <string>

namespace aurora {
void request_screenshot() noexcept;
}

namespace PortDebug {

// Restart, screenshot and exit, plus the settings file; the top of the Game page (settings are on System).
void DrawGameSection() {
  ImGui::SeparatorText("Game");
  if (ImGui::Button("Restart to menu")) {
    RequestReset();
  }
  ImGui::SameLine();
  const std::string shotKeys = PortInputRemap::BindingsText(PortInput::Action::Screenshot, " / ");
  const std::string shotLabel =
      (shotKeys.empty() ? std::string("Screenshot") : "Screenshot (" + shotKeys + ")") + "###screenshot";
  if (ImGui::Button(shotLabel.c_str())) {
    aurora::request_screenshot();
  }
  ImGui::SameLine();
  const std::string photoKeys = PortInputRemap::BindingsText(PortInput::Action::PhotoMode, " / ");
  const std::string photoLabel =
      (photoKeys.empty() ? std::string("Photo mode") : "Photo mode (" + photoKeys + ")") + "###photomode";
  if (ImGui::Button(photoLabel.c_str())) {
    PortPhoto::RequestEnter();
    // The overlay would cover the picture; photo mode starts at the next game tick.
    sVisible = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Exit game")) {
    ImGui::OpenPopup("Exit game?");
  }
  if (ImGui::BeginPopupModal("Exit game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Progress since the last save station is lost.");
    if (ImGui::Button("Exit")) {
      // The same path as closing the window: the main loop sees AURORA_EXIT
      // and shuts down cleanly.
      SaveSettings();
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void DrawSettingsSection() {
  ImGui::SeparatorText("Settings");
  if (ImGui::Button("Save settings now")) {
    sSettingsDirty = true;
    SaveSettings();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sSettingsDirty ? "Unsaved changes" : "Saved");
  ImGui::SetItemTooltip("Settings are saved automatically when changed.");
  const std::string path = SettingsFilePath();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("File: %s", path.c_str());
  ImGui::PopStyleColor();

  static std::string sResetError;
  if (ImGui::Button("Reset configuration...")) {
    sResetError.clear();
    ImGui::OpenPopup("Reset configuration?");
  }
  ImGui::SetItemTooltip("Back up and clear every port setting and control mapping.");
  if (ImGui::BeginPopupModal("Reset configuration?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted("This resets ALL port settings and control mappings (keyboard, mouse, touch and "
                           "controllers) to their defaults on the next launch. The current files are moved to a "
                           "backup folder inside the user folder, not deleted.");
    ImGui::TextUnformatted("Save files, save states, mods, the disc image and the data folder location are not "
                           "touched, and neither is the disc image path. Game options (invert Y, volumes...) stored "
                           "inside a save file come back when that save loads.");
    ImGui::TextUnformatted("The game closes now: progress since the last save station is lost. Start it again to "
                           "get the defaults.");
    if (!sResetError.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeBadColor());
      ImGui::TextWrapped("Reset failed: %s", sResetError.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Reset and exit")) {
      // Back up what is current, not what was last saved; a failed write stops here.
      sSettingsDirty = true;
      SaveSettings();
      if (sSettingsDirty) {
        sResetError = "could not save the current settings first, so nothing was moved.";
      } else {
        const std::string& folder = PortPaths::UserFolder();
        const PortConfigReset::Result result =
            PortConfigReset::Reset(PortConfigReset::detail::FromUtf8Path(folder.empty() ? "./" : folder));
        if (!result.ok) {
          sResetError = result.error;
          if (!result.backupDir.empty()) {
            sResetError += " Files that could not be restored are in " + result.backupDir + ".";
          } else {
            sResetError += " Your configuration files were not moved.";
          }
          PortLog::Write("port: reset configuration failed: %s\n", result.error.c_str());
        } else {
          PortLog::Write("port: reset configuration: %s\n",
                         result.moved ? ("backup in " + result.backupDir).c_str() : "no files to move");
          // From here no code path may write the configuration again.
          sConfigReset = true;
          aurora_disable_config_persistence();
          if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::GetIO().IniFilename = nullptr;
          }
          SDL_Event quit{};
          quit.type = SDL_EVENT_QUIT;
          SDL_PushEvent(&quit);
          ImGui::CloseCurrentPopup();
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// The game's own volumes, the same as the pause menu's two sliders.
void DrawGameAudio() {
  ImGui::SeparatorText("Audio");
  ImGui::BeginDisabled(gpGameState == nullptr);
  // Up to 100% is the game's own volume (0..127); above it the game volume stays at full and
  // the port's boost scales the mix.
  const auto volumeRow = [](const char* label, int volume, int& boost, auto&& setVolume) {
    int pct = volume >= 127 ? boost : (volume * 100 + 63) / 127;
    if (ImGui::SliderInt(label, &pct, 0, PortDebug::kVolumeBoostMax, "%d%%",
                         ImGuiSliderFlags_AlwaysClamp) &&
        gpGameState != nullptr) {
      if (pct > PortDebug::kVolumeBoostDefault) {
        setVolume(127);
        boost = pct;
      } else {
        setVolume((pct * 127 + 50) / 100);
        boost = PortDebug::kVolumeBoostDefault;
      }
      ApplyVolumeBoosts();
      MarkDirty();
    }
    ItemHelp("Above 100% the port amplifies the game's mix; loud passages can clip.");
  };
  volumeRow("Music volume", gpGameState != nullptr ? gpGameState->GameOptions().GetMusicVolume() : 0,
            sMusicBoost, [](int v) { gpGameState->GameOptions().SetMusicVolume(v, true); });
  volumeRow("Sound effects volume", gpGameState != nullptr ? gpGameState->GameOptions().GetSfxVolume() : 0,
            sSfxBoost, [](int v) { gpGameState->GameOptions().SetSfxVolume(v, true); });
  // Sound mode: Mono / Stereo / Dolby Surround
  int soundMode = gpGameState != nullptr ? static_cast<int>(gpGameState->GameOptions().GetSurroundMode()) : CAudioSys::kSM_Stereo;
  constexpr const char* kSoundModeLabels[] = {"Mono", "Stereo", "Dolby Surround"};
  if (ImGui::Combo("Sound mode", &soundMode, kSoundModeLabels, IM_ARRAYSIZE(kSoundModeLabels)) && gpGameState != nullptr) {
    gpGameState->GameOptions().SetSurroundMode(static_cast<CAudioSys::ESurroundModes>(soundMode), true);
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("These audio options are saved globally and persist across restarts.");
  DrawFlaahgraTheme();
}

void DrawOriginalSection() {
  bool original = sOriginalExperience;
  if (ImGui::Checkbox("Original experience", &original)) {
    SetOriginalExperience(original);
  }
  ItemHelp("Plays the game as on the GameCube: 4:3, 60 Hz, no mods, retail HUD and cutscenes, and "
           "none of the gameplay extras (spring ball, fast morph, charge and lock-on options, "
           "turbo fire, unlocks). Other settings return when it's turned off. Controls, cheats, "
           "save states, the randomizer, Archipelago and the timer still work.");
}

void DrawGameTab() {
  DrawOriginalSection();
  DrawGameSection();
  DrawCutscenesSection();
  DrawUnlocksSection();
  DrawGameAudio();
  DrawLanguageSection();
  DrawSpeedrunSection();
}

void DrawControlsTab() {
  if (!ImGui::BeginTabBar("controlsPages")) {
    return;
  }
  if (SubTab("Controls", "Options")) {
    DrawControlsOptions();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Mouse settings")) {
    DrawControlsKeyboardMouse();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Controller settings")) {
    DrawControlsController();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Touch & gyro")) {
    DrawControlsTouchGyro();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Remap")) {
    PortInputRemap::Draw();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void DrawVideoTab() {
  if (!ImGui::BeginTabBar("videoPages")) {
    return;
  }
  if (SubTab("Video", "Display")) {
    DrawVideoDisplay();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Quality")) {
    DrawVideoQuality();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Frame rate")) {
    DrawPerformanceTab();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Compatibility")) {
    DrawVideoCompatibility();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void DrawModsTab() {
  DrawMods();
  DrawTexturesSection();
}

void DrawSystemTab() {
  DrawOverlaySection();
  DrawSettingsSection();
  DrawMemoryCard();
#if defined(__ANDROID__)
  PortDataFolder::DrawPanel();
#endif
  DrawLogSection();
  DrawDiscCheckSection();
  DrawDiscordSection();
  DrawUpdateSection();
  ImGui::SeparatorText("About");
  ImGui::TextDisabled("Version %s, build %s", MP_BUILD_VERSION, MP_BUILD_REVISION);
}


} // namespace PortDebug
