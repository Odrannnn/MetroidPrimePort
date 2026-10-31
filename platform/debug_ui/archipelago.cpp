// F1 overlay: The Archipelago tab (connect, chat, session). State and helpers shared with the other
// parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_paths.h"
#include "port_apclient.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_timer.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace PortDebug {

// Archipelago's Connect screen: the room's address, the slot name and the
// room password, saved to archipelago.json. The built-in Metroid Prime tables
// mean nothing else is needed, so this works where editing a file does not.
void DrawArchipelagoConnect() {
  static bool sLoaded = false;
  static char sServer[256];
  static char sSlot[64];
  static char sPassword[128];
  static std::string sResult;
  if (!sLoaded) {
    sLoaded = true;
    const PortAp::ConnectionDetails saved = PortAp::SavedConnection();
    SDL_strlcpy(sServer, saved.server.c_str(), sizeof(sServer));
    SDL_strlcpy(sSlot, saved.slot.c_str(), sizeof(sSlot));
    SDL_strlcpy(sPassword, saved.password.c_str(), sizeof(sPassword));
  }

  ImGui::SeparatorText("Connect");
  // The server doesn't send this option, so it is set here to match the seed.
  int suitDamage = sApSuitDamage;
  if (ImGui::Combo("Staggered suit damage", &suitDamage, "Default\0Progressive\0Additive\0")) {
    SetApSuitDamage(suitDamage);
  }
  ImGui::SetItemTooltip("Set it to your YAML's staggered_suit_damage (the apworld's default is\n"
                        "Progressive: damage reduction by how many suits you have).");
  ImGui::InputTextWithHint("Server", "archipelago.gg:38281", sServer, sizeof(sServer));
  ImGui::InputTextWithHint("Slot name", "your player name in the seed", sSlot, sizeof(sSlot));
  ImGui::InputTextWithHint("Password", "only if the room has one", sPassword, sizeof(sPassword),
                           ImGuiInputTextFlags_Password);
  if (ImGui::Button(PortAp::Enabled() ? "Reconnect" : "Connect")) {
    PortAp::ConnectionDetails details;
    details.server = sServer;
    details.slot = sSlot;
    details.password = sPassword;
    std::string error;
    sResult = PortAp::Connect(details, error) ? std::string() : error;
    // The saved form (trimmed, "/connect " dropped) goes back into the fields.
    if (sResult.empty())
      sLoaded = false;
  }
  if (PortAp::Enabled()) {
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
      std::string error;
      sResult = PortAp::Disconnect(error) ? std::string() : error;
    }
  }
  if (!sResult.empty())
    ImGui::TextColored(ThemeBadColor(ImVec4(1.f, 0.45f, 0.4f, 1.f)), "%s", sResult.c_str());
  ImGui::TextDisabled("Start a new game after connecting to a new seed.");

  // Games played before, each with its own save card. The list is re-read now
  // and then: a game is recorded when its server answers, after Connect.
  static std::vector<PortAp::ConnectionDetails> sRecent;
  static uint64_t sRecentReadAt = 0;
  if (!ImGui::CollapsingHeader("Recent Archipelago games", ImGuiTreeNodeFlags_DefaultOpen)) {
    sRecentReadAt = 0;
    return;
  }
  const uint64_t now = SDL_GetTicks();
  if (sRecentReadAt == 0 || now - sRecentReadAt > 2000) {
    sRecent = PortAp::RecentGames();
    sRecentReadAt = now == 0 ? 1 : now;
  }
  if (sRecent.empty()) {
    ImGui::TextDisabled("None yet. A game is listed once its server has answered.");
    return;
  }
  // The save card only changes on the title screen, so a game in progress
  // would keep saving to the card it was loaded from.
  const bool inGame = StateManager() != nullptr;
  if (inGame)
    ImGui::TextDisabled("Quit to the title screen to resume another game.");
  const PortAp::ConnectionDetails current = PortAp::SavedConnection();
  for (size_t i = 0; i < sRecent.size(); ++i) {
    const PortAp::ConnectionDetails& game = sRecent[i];
    const bool isCurrent = PortAp::Enabled() && current.server == game.server &&
                           current.slot == game.slot && current.seed == game.seed &&
                           current.room == game.room;
    ImGui::PushID(static_cast<int>(i));
    ImGui::BeginDisabled(inGame || isCurrent);
    if (ImGui::Button(isCurrent ? "Current" : "Resume")) {
      std::string error;
      sResult = PortAp::Connect(game, error) ? std::string() : error;
      if (sResult.empty()) {
        sLoaded = false;
        sRecentReadAt = 0;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    char played[32] = "";
    const time_t when = static_cast<time_t>(game.lastPlayed);
    if (const std::tm* local = game.lastPlayed > 0 ? std::localtime(&when) : nullptr)
      std::strftime(played, sizeof(played), "%Y-%m-%d %H:%M", local);
    ImGui::Text("%s @ %s", game.slot.c_str(), game.server.c_str());
    ImGui::Indent();
    ImGui::TextDisabled("%s%s%s", game.seed.c_str(), played[0] != '\0' ? "  -  " : "", played);
    ImGui::Unindent();
    ImGui::PopID();
  }
}

ImVec4 ChatLineColor(const std::string& type) {
  if (type == "Chat")
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  if (type == "ServerChat" || type == "CommandResult" || type == "AdminCommandResult")
    return ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
  if (type == "Hint")
    return ThemeWarnColor(ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
  if (type == "ItemSend" || type == "ItemCheat")
    return ThemeGoodColor(ImVec4(0.6f, 0.95f, 0.6f, 1.0f));
  if (type == "Goal" || type == "Release" || type == "Collect" || type == "Countdown")
    return ImVec4(1.0f, 0.6f, 1.0f, 1.0f);
  if (type == "port")
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
  return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Join, Part, TagsChanged, Tutorial, ...
}

// The multiworld's chat: every PrintJSON message the server sent, and a box that
// sends Say, so server commands such as !hint work from inside the game.
void DrawChatTab() {
  static char sInput[512] = {};
  static std::string sError;
  static uint64_t sSeenSerial = ~uint64_t{0};

  if (!PortAp::Enabled()) {
    ImGui::TextWrapped("Connect to an Archipelago room on the Connection page to chat.");
    return;
  }
  uint64_t serial = 0;
  const std::vector< PortAp::ChatLine > log = PortAp::ChatLog(&serial);
  // The log takes the tab down to the input row, the error and the help line.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float below = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
  // The window may reach past a small display, so stop at whichever ends first.
  const float room = std::min(ImGui::GetContentRegionAvail().y,
                              ImGui::GetIO().DisplaySize.y - style.WindowPadding.y -
                                  ImGui::GetCursorScreenPos().y);
  const float logHeight = std::max(ImGui::GetTextLineHeightWithSpacing() * 6.0f, room - below);
  if (ImGui::BeginChild("apChat", ImVec2(0.0f, logHeight), ImGuiChildFlags_Borders)) {
    // Follow new lines only while the log is scrolled to the bottom, so reading
    // back is not interrupted.
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (log.empty())
      ImGui::TextDisabled("No messages yet.");
    ImGui::PushTextWrapPos(0.0f);
    for (const PortAp::ChatLine& line : log) {
      ImGui::PushStyleColor(ImGuiCol_Text, ChatLineColor(line.type));
      ImGui::TextUnformatted(line.text.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (serial != sSeenSerial && (atBottom || sSeenSerial == ~uint64_t{0}))
      ImGui::SetScrollHereY(1.0f);
    sSeenSerial = serial;
  }
  ImGui::EndChild();

  const bool connected = PortAp::Connected();
  ImGui::BeginDisabled(!connected);
  const float sendWidth = ImGui::CalcTextSize("Send").x + style.FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendWidth - style.ItemSpacing.x);
  bool send = ImGui::InputTextWithHint("##apSay", connected ? "Message or !command" : "Not connected",
                                       sInput, sizeof(sInput), ImGuiInputTextFlags_EnterReturnsTrue);
  if (send)
    ImGui::SetKeyboardFocusHere(-1); // Enter keeps the box focused for the next line
  ImGui::SameLine();
  send = ImGui::Button("Send") || send;
  ImGui::EndDisabled();
  if (send && sInput[0] != '\0') {
    if (PortAp::SendChat(sInput, sError))
      sInput[0] = '\0';
  }
  if (!sError.empty())
    ImGui::TextColored(ThemeBadColor(ImVec4(1.f, 0.5f, 0.5f, 1.f)), "%s", sError.c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Commands: !hint <item>, !hint_location <location>, !remaining, !release, "
                     "!collect, !help");
  ImGui::PopStyleColor();
}

// The connection page of the Archipelago tab: the Connect form, recent games,
// the session's status and the items received.
void DrawArchipelagoSession() {
  DrawArchipelagoConnect();
  if (PortAp::Enabled()) {
    ImGui::SeparatorText("Archipelago status");
    if (PortAp::Connected()) {
      ImGui::TextColored(ThemeGoodColor(), "connected");
    } else {
      ImGui::TextDisabled("not connected");
    }
    ImGui::SameLine();
    ImGui::TextWrapped("%s", PortAp::StatusText());
    const char* switchNotice = PortAp::SwitchNotice();
    if (switchNotice[0] != '\0') {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeBadColor());
      ImGui::TextWrapped("%s", switchNotice);
      ImGui::PopStyleColor();
    }
    const char* seedName = PortAp::SeedName();
    if (seedName != nullptr && seedName[0] != '\0')
      ImGui::TextWrapped("Seed: %s", seedName);
    ImGui::Text("Items received: %d    Location checks sent: %d", PortAp::ItemCount(),
                PortAp::CheckCount());
    const char* lastMessage = PortAp::LastMessage();
    if (lastMessage != nullptr && lastMessage[0] != '\0')
      ImGui::TextWrapped("Last message: %s", lastMessage);

    // Item tracker: the session's receipts, so an item that arrived while the
    // player was not looking at the HUD is still readable here.
    const std::vector< PortAp::TrackedItem > tracked = PortAp::TrackedItems();
    ImGui::SeparatorText("Received items");
    if (tracked.empty()) {
      ImGui::TextDisabled("Nothing yet.");
    } else {
      // Tall enough for every row, up to half the window, so a long list
      // scrolls inside the table rather than pushing the settings below it off
      // the tab. Not the space left below: on a page already taller than the
      // window that is about nothing, and the table shrank to a sliver.
      const float rowHeight =
          ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f + 1.0f;
      const float height = std::min(rowHeight * static_cast< float >(tracked.size() + 1),
                                    ImGui::GetWindowHeight() * 0.5f);
      if (ImGui::BeginTable("apTracked", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_BordersInnerH,
                            ImVec2(0.0f, height))) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("Step");
        ImGui::TableHeadersRow();
        for (const PortAp::TrackedItem& item : tracked) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.from.empty() ? "(your item)" : item.from.c_str());
          ImGui::TableNextColumn();
          if (item.total > 1) {
            ImGui::Text("%d of %d", item.step, item.total);
          } else {
            ImGui::TextDisabled("-");
          }
        }
        ImGui::EndTable();
      }
    }
  }
}

// Everything Archipelago in one tab: the connection and the multiworld's chat.
bool SubTab(const char* page, const char* name);

void DrawArchipelagoTab() {
  if (!ImGui::BeginTabBar("apPages")) {
    return;
  }
  if (SubTab("Archipelago", "Connection")) {
    DrawArchipelagoSession();
    ImGui::EndTabItem();
  }
  if (SubTab("Archipelago", "Chat")) {
    DrawChatTab();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

} // namespace PortDebug
