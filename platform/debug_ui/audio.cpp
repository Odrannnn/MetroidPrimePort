// F1 overlay: Audio and voice mutes. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_disc.h"
#include "port_flaahgra_music.h"
#include "port_pointer.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <musyx/port_voices.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace PortDebug {

void DrawAudio() {
  bool ai = AiAudioEnabled();
  if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
    SetAiAudioEnabled(ai);
  }
  bool musyx = sMusyxAudioEnabled;
  if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
    SetMusyxAudioEnabled(musyx);
  }
}

// The full Flaahgra boss theme for an NTSC disc (port_flaahgra_music.h).
void DrawFlaahgraTheme() {
  if (PortDisc::HasPalData()) {
    return;  // PAL and Trilogy discs already have the whole track
  }
  ImGui::SeparatorText("Flaahgra boss theme");
  std::string picked;
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    // A pick the previous process never saw (the game was killed behind the picker).
    static bool sTookFile = false;
    if (!sTookFile) {
      sTookFile = true;
      std::ifstream in(RemasteredPickFile(kFlaahgraPick));
      std::string uri;
      if (std::getline(in, uri) && !uri.empty()) {
        sFlaahgraPick = uri;
      }
    }
#endif
    picked.swap(sFlaahgraPick);
  }
  static std::string sStartError;
  if (!picked.empty()) {
#if defined(__ANDROID__)
    std::remove(RemasteredPickFile(kFlaahgraPick).c_str());
#endif
    sStartError = PortFlaahgraMusic::Start(picked) ? "" : "An import is already running.";
  }
  const PortFlaahgraMusic::State state = PortFlaahgraMusic::Status();
  const bool installed = PortFlaahgraMusic::Installed();
  ImGui::BeginDisabled(state.running);
  if (ImGui::Button(installed ? "Import the full Flaahgra theme again..." : "Full Flaahgra theme...")) {
    OpenRemasteredDialog(kFlaahgraPick);
  }
  ImGui::EndDisabled();
  ItemHelp("The NTSC discs cut the Flaahgra theme to 36 s. Pick a PAL or Japanese GameCube disc, "
           "or Metroid Prime Trilogy (Wii), to add the full 114 s track as a mod. Reload mods (F1 "
           "> Mods) or restart to hear it.");
  if (installed && !state.running) {
    ImGui::SameLine();
    if (ImGui::Button("Remove##flaahgra")) {
      PortFlaahgraMusic::Remove();
      sStartError.clear();
    }
    ImGui::SetItemTooltip("Deletes the imported theme. Reload mods or restart to go back to the short one.");
  }
  if (state.running) {
    ImGui::TextDisabled("%s...", state.message.c_str());
  } else if (state.finished && !state.ok) {
    ImGui::TextColored(ThemeWarnColor(), "%s", state.message.c_str());
  } else if (!sStartError.empty()) {
    ImGui::TextColored(ThemeWarnColor(), "%s", sStartError.c_str());
  } else if (state.finished && state.ok) {
    ImGui::TextWrapped("%s", state.message.c_str());
  } else if (installed) {
    ImGui::TextDisabled("Full theme added. Reload mods or restart to apply it.");
  } else {
    ImGui::TextDisabled("Not added: the fight plays the short NTSC theme.");
  }
}

void DrawVoices() {
  struct Agg {
    PortMusyxVoice voice;
    int instances;
  };

  // Held between frames so "Freeze list" can keep the last collection on screen.
  // Ordered by id, not loudness: sorting by rms made rows swap places every
  // frame as the levels moved, which made the checkboxes hard to hit and hid
  // which sample a row belonged to.
  static std::vector< Agg > aggs;
  static bool freeze = false;

  ImGui::Checkbox("Freeze list", &freeze);
  ImGui::SameLine();
  ImGui::TextUnformatted("(hold the current list so its rows stay put)");
  ImGui::SetItemTooltip(
      "Freezing keeps the rows shown right now so you can read them. Mute still works\n"
      "on any sample, playing or not. Untick to follow live voices again. Not saved.");

  if (!freeze) {
    aggs.clear();
    PortMusyxVoice voices[64];
    const int count = MusyxPortCopyVoices(voices, 64);
    for (int i = 0; i < count; ++i) {
      bool found = false;
      for (Agg& agg : aggs) {
        if (agg.voice.smpId == voices[i].smpId) {
          ++agg.instances;
          if (voices[i].rms > agg.voice.rms) {
            agg.voice = voices[i];
          }
          found = true;
          break;
        }
      }
      if (!found) {
        aggs.push_back(Agg{voices[i], 1});
      }
    }
    std::sort(aggs.begin(), aggs.end(),
              [](const Agg& a, const Agg& b) { return a.voice.smpId < b.voice.smpId; });
  }

  ImGui::TextUnformatted("Active samples, lowest id first. Mute one to isolate it.");
  if (aggs.empty()) {
    ImGui::TextUnformatted(freeze ? "No samples in the frozen list." : "No active MusyX voices.");
  }
  for (const Agg& agg : aggs) {
    ImGui::PushID(static_cast< int >(agg.voice.smpId));
    bool muted = MusyxPortIsSampleMuted(agg.voice.smpId) != 0;
    if (ImGui::Checkbox("##mute", &muted)) {
      MusyxPortSetSampleMuted(agg.voice.smpId, muted ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::Text("smp %u  %s  len %u  pitch %u  rms %d  vol %u/%u  x%d", agg.voice.smpId,
                agg.voice.looped ? "loop" : "one-shot", agg.voice.length, agg.voice.pitch,
                agg.voice.rms, agg.voice.volL, agg.voice.volR, agg.instances);
    ImGui::PopID();
  }
  // Deliberately not behind the empty-list case above: with nothing playing
  // there is still a mute set to clear. Also turns both audio paths back on, so
  // nothing stays silent. None of these mutes is saved: a relaunch has sound.
  if (ImGui::Button("Unmute all")) {
    MusyxPortClearSampleMutes();
    SetAiAudioEnabled(true);
    SetMusyxAudioEnabled(true);
  }
}

} // namespace PortDebug
