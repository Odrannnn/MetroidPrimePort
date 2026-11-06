// F1 overlay: The Debug tab and log section. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_disc.h"
#include "port_disc_replace.h"
#include "port_disc_verify.h"
#include "port_log.h"
#include "port_log_file.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <cfloat>
#include <string>

namespace PortDebug {

void DrawLogSection() {
  ImGui::SeparatorText("Log");
  bool logFile = sLogFile || PortLogFile::Active();
  if (ImGui::Checkbox("Write the log to a file", &logFile)) {
    SetLogFile(logFile);
    if (logFile) {
      PortLogFile::Start();
    }
  }
  const std::string logPath = PortLogFile::Path();
  if (!logPath.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Copy log path")) {
      ImGui::SetClipboardText(logPath.c_str());
    }
  }
  if (PortLogFile::Active()) {
    ImGui::TextWrapped("Writing to %s (last run's: metroid_prime_port.old.log).", logPath.c_str());
    if (!sLogFile) {
      ImGui::TextDisabled("Stops at the next start.");
    }
  } else {
    ImGui::TextWrapped("Everything the game logs, including the reason for a crash, goes to %s.",
                       logPath.empty() ? "(no user folder)" : logPath.c_str());
  }
  if (const std::string shared = PortLogFile::SharedPath(); !shared.empty()) {
    ImGui::TextWrapped("A copy goes to %s, which the phone's file manager can open.", shared.c_str());
  }

}

void DrawDiscCheckSection() {
  using PortDiscVerify::State;
  ImGui::SeparatorText("Disc image");
  const PortDiscVerify::Result r = PortDiscVerify::Snapshot();
  const bool running = r.state == State::Running;
  const PortDiscReplace::Status replace = PortDiscReplace::Snapshot();
  const bool replacing = replace.state == PortDiscReplace::State::Running;
  // The result of a pick that finished: remembered here, on the main thread.
  PortDiscReplace::Poll();
  const bool needsNewImage = r.state == State::Done && !r.Ok();
  if (const std::string image = PortDiscVerify::ImagePath(); !image.empty()) {
    ImGui::TextWrapped("Disc image: %s", image.c_str());
  }
  if (const PortDisc::Version version = PortDisc::Current(); version != PortDisc::Version::Unknown) {
    ImGui::TextWrapped("Version: Metroid Prime %s (%s)", PortDisc::Name(version), PortDisc::GameCode(version));
  }
  ImGui::BeginDisabled(running || replacing);
  if (ImGui::Button("Check disc image")) {
    PortDiscVerify::Start();
  }
  ImGui::EndDisabled();
  if (running) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      PortDiscVerify::Cancel();
    }
    ImGui::ProgressBar(static_cast<float>(r.Progress()), ImVec2(-FLT_MIN, 0.0f));
  }
  switch (r.state) {
  case State::Idle:
    break;
  case State::Running:
    ImGui::TextWrapped("Checking the disc image (%.0f%%)...", r.Progress() * 100.0);
    break;
  case State::Cancelled:
    ImGui::TextWrapped("The check was cancelled.");
    break;
  case State::Error:
    ImGui::TextWrapped("The disc image couldn't be checked: %s.", r.error.c_str());
    break;
  case State::Done:
    if (r.Ok()) {
      ImGui::TextWrapped("This disc image is complete and unaltered (%s).", r.revision.c_str());
      ImGui::TextDisabled("SHA-1 %s", r.sha1.c_str());
    } else if (!r.known) {
      ImGui::TextWrapped("This disc image is not one of the releases the port can check it against.");
      ImGui::TextDisabled("SHA-1 %s", r.sha1.c_str());
    } else if (r.bad.empty()) {
      ImGui::TextWrapped("Every part of this %s image matches, but the file does not end where the original does.",
                         r.revision.c_str());
    } else {
      ImGui::TextWrapped("This disc image is damaged: %zu of %u MiB differ from %s (%u of them are blank).",
                         r.bad.size(), r.chunks, r.revision.c_str(), r.zeroed);
      if (!r.files.empty()) {
        std::string files;
        for (size_t i = 0; i < r.files.size() && i < 8; ++i) {
          files += (i ? ", " : "") + r.files[i];
        }
        if (r.files.size() > 8) {
          files += ", ...";
        }
        ImGui::TextWrapped("Damaged: %s", files.c_str());
      }
      ImGui::TextWrapped("Copy the disc image again; this part of it is damaged.");
    }
    break;
  }
  ImGui::Spacing();
  if (needsNewImage) {
    ImGui::TextWrapped("Pick a good copy of the disc image to use from the next start:");
  }
  ImGui::BeginDisabled(running || replacing);
  if (ImGui::Button("Replace disc image...")) {
    PortDiscReplace::PickAsync();
  }
  ImGui::EndDisabled();
  if (replacing) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel##replace")) {
      PortDiscReplace::Cancel();
    }
    if (replace.copying) {
      ImGui::ProgressBar(static_cast<float>(replace.bytesTotal ? static_cast<double>(replace.bytesDone) /
                                                                    static_cast<double>(replace.bytesTotal)
                                                                : 0.0),
                         ImVec2(-FLT_MIN, 0.0f));
      ImGui::TextWrapped("Copying the picked disc image...");
    } else {
      ImGui::TextWrapped("Checking the picked file...");
    }
  } else if (!replace.message.empty()) {
    ImGui::TextWrapped("%s", replace.message.c_str());
    if (replace.state == PortDiscReplace::State::Done) {
      ImGui::TextDisabled("%s (%s)", replace.path.c_str(), replace.revision.c_str());
    }
  }
}

void DrawDebugTab() {
  ImGui::SeparatorText("Cheats");
  bool cheats = sCheats;
  if (ImGui::Checkbox("Show cheats (items, health, teleport)", &cheats)) {
    sCheats = cheats;
    MarkDirty();
  }
  if (sCheats) {
    DrawCheats();
  }

  ImGui::SeparatorText("Camera");
  DrawFreeCam();

  ImGui::SeparatorText("Rendering");
  DrawRendering();

  ImGui::SeparatorText("Audio backends");
  DrawAudio();
  if (ImGui::CollapsingHeader("Sounds playing")) {
    DrawVoices();
  }

  DrawRemasteredDebug();
}

} // namespace PortDebug
