// F1 overlay: Cheats, the free camera and rendering debug. State and helpers shared with the other
// parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_collision_view.h"
#include "port_debug.h"
#include "port_freecam.h"
#include "port_pointer.h"
#include "port_room_geo.h"
#include "port_paths.h"
#include "port_tracker.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace PortDebug {

void GrantItem(CPlayerState& ps, CPlayerState::EItemType type, int amount, int capacity) {
  ps.SetPowerUp(type, capacity);
  ps.SetPickup(type, amount);
}

namespace {

// A slider takes the value under the finger the moment it is pressed, before
// UpdateTouchScroll can tell a scroll from a drag, so scrolling the page past
// these sliders filled Samus's missiles and tanks. The value is held back
// until release and dropped when the drag became a scroll.
bool CheatSlider(const char* label, int& value, int max) {
  static ImGuiID sEditing = 0;
  static int sPending = 0;
  const ImGuiID id = ImGui::GetID(label);
  int shown = sEditing == id ? sPending : value;
  ImGui::SliderInt(label, &shown, 0, max);
  if (ImGui::IsItemActive()) {
    sEditing = id;
    sPending = shown;
    return false;
  }
  if (sEditing != id) {
    return false;
  }
  sEditing = 0;
  if (TouchScrolling() || sPending == value) {
    return false;
  }
  value = sPending;
  return true;
}

// Raises the capacity to the amount, never lowers it, so setting a count does
// not hand out the full expansion.
void SetAmount(CPlayerState& ps, CPlayerState::EItemType type, int amount) {
  GrantItem(ps, type, amount, std::max(amount, ps.GetItemCapacity(type)));
}

} // namespace

void DrawCheats() {
  CStateManager* mgr = sStateManager;
  if (mgr == nullptr) {
    ImGui::TextUnformatted("Waiting for gameplay...");
    return;
  }
  CPlayerState* ps = mgr->PlayerState();
  if (ps == nullptr) {
    ImGui::TextUnformatted("No player state.");
    return;
  }

  ImGui::Text("Health: %.0f / %.0f", ps->HealthInfo()->GetHP(), ps->CalculateHealth());
  if (ImGui::Button("Full health")) {
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  ImGui::SameLine();
  if (ImGui::Button("Grant everything")) {
    for (int i = CPlayerState::kIT_PowerBeam; i < CPlayerState::kIT_Max; ++i) {
      GrantItem(*ps, static_cast< CPlayerState::EItemType >(i), 1, 1);
    }
    GrantItem(*ps, CPlayerState::kIT_Missiles, 250, 250);
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, 8, 8);
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, 14, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  ImGui::SameLine();
  bool invulnerable = Invulnerable();
  if (ImGui::Checkbox("Invulnerable", &invulnerable)) {
    SetInvulnerable(invulnerable);
  }
  ImGui::SetItemTooltip("Samus takes no damage from anything. Stays on until unticked.");
  bool phazonBeam = PhazonBeamCheat();
  if (ImGui::Checkbox("Phazon Beam", &phazonBeam)) {
    SetPhazonBeamCheat(phazonBeam);
  }
  ImGui::SetItemTooltip(
      "The Phazon Beam takes the Power Beam's place, in any room, without the Phazon Suit. "
      "Wave, Ice and Plasma leave it; Power enters it again. Stays on until unticked.");

  ImGui::SeparatorText("Abilities");
  struct SItemToggle {
    const char* name;
    CPlayerState::EItemType type;
  };
  static const SItemToggle kItems[] = {
      {"Power Beam", CPlayerState::kIT_PowerBeam},
      {"Ice Beam", CPlayerState::kIT_IceBeam},
      {"Wave Beam", CPlayerState::kIT_WaveBeam},
      {"Plasma Beam", CPlayerState::kIT_PlasmaBeam},
      {"Charge Beam", CPlayerState::kIT_ChargeBeam},
      {"Super Missile", CPlayerState::kIT_SuperMissile},
      {"Ice Spreader", CPlayerState::kIT_IceSpreader},
      {"Wavebuster", CPlayerState::kIT_Wavebuster},
      {"Flamethrower", CPlayerState::kIT_Flamethrower},
      {"Combat Visor", CPlayerState::kIT_CombatVisor},
      {"Scan Visor", CPlayerState::kIT_ScanVisor},
      {"Thermal Visor", CPlayerState::kIT_ThermalVisor},
      {"X-Ray Visor", CPlayerState::kIT_XRayVisor},
      {"Morph Ball", CPlayerState::kIT_MorphBall},
      {"Morph Ball Bombs", CPlayerState::kIT_MorphBallBombs},
      {"Boost Ball", CPlayerState::kIT_BoostBall},
      {"Spider Ball", CPlayerState::kIT_SpiderBall},
      {"Space Jump Boots", CPlayerState::kIT_SpaceJumpBoots},
      {"Grapple Beam", CPlayerState::kIT_GrappleBeam},
      {"Gravity Suit", CPlayerState::kIT_GravitySuit},
      {"Varia Suit", CPlayerState::kIT_VariaSuit},
      {"Phazon Suit", CPlayerState::kIT_PhazonSuit},
  };
  // As many columns as the widest name allows.
  float nameWidth = 0.f;
  for (const SItemToggle& item : kItems) {
    nameWidth = std::max(nameWidth, ImGui::CalcTextSize(item.name).x);
  }
  nameWidth += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::GetStyle().ItemSpacing.x * 2.f;
  const int columns = std::clamp(int(ImGui::GetContentRegionAvail().x / nameWidth), 1, 4);
  if (ImGui::BeginTable("##abilities", columns)) {
    for (const SItemToggle& item : kItems) {
      ImGui::TableNextColumn();
      bool owned = ps->HasPowerUp(item.type);
      if (ImGui::Checkbox(item.name, &owned)) {
        if (owned) {
          GrantItem(*ps, item.type, 1, 1);
        } else {
          ps->SetPowerUp(item.type, 0);
          ps->SetPickup(item.type, 0);
        }
      }
    }
    ImGui::EndTable();
  }

  int missiles = ps->GetItemAmount(CPlayerState::kIT_Missiles);
  if (CheatSlider("Missiles", missiles, 250)) {
    SetAmount(*ps, CPlayerState::kIT_Missiles, missiles);
  }
  int powerBombs = ps->GetItemAmount(CPlayerState::kIT_PowerBombs);
  if (CheatSlider("Power Bombs", powerBombs, 8)) {
    SetAmount(*ps, CPlayerState::kIT_PowerBombs, powerBombs);
  }
  int tanks = ps->GetItemAmount(CPlayerState::kIT_EnergyTanks);
  if (CheatSlider("Energy Tanks", tanks, 14)) {
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, tanks, tanks); // tanks have no ammo: count is capacity
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }

  ImGui::SeparatorText("Teleport");
  CWorld* world = mgr->World();
  if (world == nullptr) {
    ImGui::TextUnformatted("No world.");
    return;
  }
  if (mgr->GetGameState() != CStateManager::kGS_Running) {
    ImGui::TextUnformatted("(waiting for gameplay)");
  }
  const int areaCount = world->IGetAreaCount();
  const int current = world->GetCurrentAreaId().Value();
  const std::vector< std::string >& roomNames = PortTracker::RoomNames(*world);
  const auto roomLabel = [&](int i) {
    char label[160];
    if (i >= 0 && i < int(roomNames.size()) && !roomNames[i].empty()) {
      std::snprintf(label, sizeof(label), "%d  %s", i, roomNames[i].c_str());
    } else {
      std::snprintf(label, sizeof(label), "Area %d", i);
    }
    return std::string(label);
  };
  // Only loaded areas (the current one and its neighbours) can be entered in place; any
  // other area reloads the world straight into it.
  const auto goToArea = [&](int i) {
    const bool loaded = world->GetArea(TAreaId(i))->IsPostConstructed();
    if (loaded || gpGameState == nullptr) {
      PortDebug::RequestTeleport(i);
    } else {
      PortDebug::RequestWorldTeleport(
          static_cast< uint32_t >(gpGameState->CurrentWorldAssetId()),
          static_cast< uint32_t >(world->IGetAreaAlways(TAreaId(i))->IGetAreaAssetId()));
    }
  };
  static char sRoomFilter[64] = {};
  const float goWidth = ImGui::CalcTextSize("Reload").x + ImGui::GetStyle().FramePadding.x * 2.f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - goWidth - ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::BeginCombo("##room", roomLabel(current).c_str(), ImGuiComboFlags_HeightLarge)) {
#if !defined(__ANDROID__) // it would raise the on-screen keyboard every time
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
#endif
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter rooms", sRoomFilter, sizeof(sRoomFilter));
    for (int i = 0; i < areaCount; ++i) {
      const std::string label = roomLabel(i);
      if (sRoomFilter[0] != '\0' && SDL_strcasestr(label.c_str(), sRoomFilter) == nullptr) {
        continue;
      }
      const bool loaded = world->GetArea(TAreaId(i))->IsPostConstructed();
      ImGui::PushID(i);
      if (!loaded) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      }
      if (ImGui::Selectable(label.c_str(), i == current)) {
        goToArea(i);
      }
      if (!loaded) {
        ImGui::PopStyleColor();
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Pick a room to go there. Greyed rooms aren't loaded: going there reloads the world.");
  ImGui::SameLine();
  if (ImGui::Button("Reload")) {
    goToArea(current);
  }

  if (gpMemoryCard == nullptr) {
    ImGui::TextUnformatted("(memory card not ready)");
    return;
  }
  static bool sWorldListBuilt = false;
  static std::vector< std::pair< uint32_t, std::string > > sWorldList;
  if (!sWorldListBuilt && !gpMemoryCard->GetMemoryWorlds().empty()) {
    sWorldListBuilt = true;
    const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
    for (int i = 0; i < worlds.size(); ++i) {
      const uint32_t id = static_cast< uint32_t >(worlds[i].first);
      std::string name;
      const wchar_t* wide = worlds[i].second.GetFrontEndName();
      if (wide != nullptr) {
        for (const wchar_t* p = wide; *p != 0; ++p) {
          name.push_back(static_cast< char >(*p));
        }
      }
      if (name.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "MLVL %08X", static_cast< unsigned >(id));
        name = buf;
      }
      sWorldList.emplace_back(id, name);
    }
  }
  if (!sWorldListBuilt) {
    ImGui::TextUnformatted("(loading worlds...)");
    return;
  }
  const char* currentWorld = "(another world)";
  for (const std::pair< uint32_t, std::string >& entry : sWorldList) {
    if (gpGameState != nullptr && gpGameState->CurrentWorldAssetId() == entry.first) {
      currentWorld = entry.second.c_str();
    }
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##world", currentWorld)) {
    for (const std::pair< uint32_t, std::string >& entry : sWorldList) {
      ImGui::PushID(static_cast< int >(entry.first));
      const bool isCurrent = gpGameState != nullptr && gpGameState->CurrentWorldAssetId() == entry.first;
      if (ImGui::Selectable(entry.second.c_str(), isCurrent)) {
        PortDebug::RequestWorldTeleport(entry.first, 0u);
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Pick a world to go to its start.");
}

// The free camera: the view leaves the player, who stands still meanwhile.
void DrawFreeCam() {
  CStateManager* mgr = StateManager();
  bool on = PortFreeCam::Active();
  ImGui::BeginDisabled(mgr == nullptr && !on);
  if (ImGui::Checkbox("Free camera", &on)) {
    PortFreeCam::SetActive(on, mgr);
  }
  ImGui::EndDisabled();
  if (!PortFreeCam::Active()) {
    return;
  }
  ImGui::SameLine();
  bool frozen = PortFreeCam::Frozen();
  if (ImGui::Checkbox("Freeze the game", &frozen)) {
    PortFreeCam::SetFrozen(frozen);
  }
  ImGui::SameLine();
  bool showPlayer = PortFreeCam::ShowPlayer();
  if (ImGui::Checkbox("Show Samus", &showPlayer)) {
    PortFreeCam::SetShowPlayer(showPlayer);
  }
  float speed = PortFreeCam::Speed();
  if (ImGui::SliderFloat("Speed", &speed, 1.f, 100.f, "%.0f m/s", ImGuiSliderFlags_Logarithmic)) {
    PortFreeCam::SetSpeed(speed);
  }
  PortFreeCam::Pose pose = PortFreeCam::GetPose();
  float pos[3] = {pose.x, pose.y, pose.z};
  float look[2] = {pose.yaw, pose.pitch};
  bool moved = ImGui::InputFloat3("Position", pos, "%.2f");
  moved |= ImGui::InputFloat2("Yaw, pitch", look, "%.1f");
  if (moved) {
    pose.x = pos[0];
    pose.y = pos[1];
    pose.z = pos[2];
    pose.yaw = look[0];
    pose.pitch = look[1];
    PortFreeCam::SetPose(pose);
  }
  ImGui::TextWrapped("Close this menu to fly: stick moves, C stick or mouse looks, Z / D-pad up "
                     "rises, L / D-pad down sinks, R goes four times as fast.");
}

// The switches and readouts for working on what mods draw: room geometry, room
// environments, PBR.
void DrawRendering() {
  ImGui::TextDisabled("Hover a setting for what it does and the recommended value. They reset at each start.");
  static const char* const kCollisionModes[] = {"off", "on top of the world", "only (hide the world)"};
  int collision = int(PortCollisionView::GetMode());
  if (ImGui::Combo("Collision", &collision, kCollisionModes, 3)) {
    PortCollisionView::SetMode(PortCollisionView::Mode(collision));
  }
  ImGui::SetItemTooltip("Recommended: off.\n"
                        "Draws what Samus collides with: walls grey, floors blue, ceilings red,\n"
                        "lava orange, grates yellow, solid objects as orange boxes.");

  if (ImGui::CollapsingHeader("Frame statistics")) {
    if (const AuroraStats* stats = aurora_get_stats()) {
      ImGui::Text("%.0f fps, %u draws (%u merged), %u PBR, %u passes", aurora_get_fps(), stats->drawCallCount,
                  stats->mergedDrawCallCount, CCubeMaterial::sPortPBRDraws, stats->renderPassCount);
      ImGui::Text("vertices %.1f MiB, indices %.1f, arrays %.1f, uniforms %.1f, texture uploads %.1f",
                  stats->lastVertSize / 1048576.f, stats->lastIndexSize / 1048576.f,
                  stats->lastStorageSize / 1048576.f, stats->lastUniformSize / 1048576.f,
                  stats->lastTextureUploadSize / 1048576.f);
      if (const uint32_t resident = aurora_get_resident_geometry_mib()) {
        ImGui::Text("kept on the GPU %.1f of %u MiB", aurora_get_resident_geometry_used() / 1048576.f, resident);
      }
      ImGui::Text("pipelines %u made, %u waiting", stats->createdPipelines, stats->queuedPipelines);
    }
    AuroraTextureStats textures{};
    aurora_get_texture_stats(&textures);
    ImGui::Text("textures %u, %.0f MiB; render targets %u, %.0f MiB; lighting %u, %.0f MiB", textures.count[0],
                textures.bytes[0] / 1048576.f, textures.count[1], textures.bytes[1] / 1048576.f, textures.count[2],
                textures.bytes[2] / 1048576.f);
    int areas = 0;
    int instances = 0;
    int models = 0;
    int loaded = 0;
    int drawn = 0;
    PortRoomGeo::Stats(areas, instances, models, loaded, drawn);
    ImGui::Text("room geometry: %d area(s), %d of %d model(s) loaded, %d of %d instance(s) drawn", areas, loaded,
                models, drawn, instances);
  }
}

} // namespace PortDebug
