// Part of platform/debug_ui.cpp: The settings file: one table of keys (kSettingKeys) that reads and
// writes port_settings.ini, and saving it. Included by debug_ui.cpp at this point in the file, not
// compiled on its own: it shares that file's state and namespaces.

namespace {

// One key of port_settings.ini: how its value is read and how it is written.
// The table is the file's order. ApplySetting reads through it, SettingsText
// writes through it, and SettingsSelfTest checks that each key's write reads
// back as the same value.
struct SettingKey {
  // nullptr: a block of keys another module writes (PortInput), read by it too.
  const char* key;
  void (*read)(const std::string& value);
  // The value only, after "key="; nullptr for an old name that is only read.
  void (*write)(std::ostream& out);
  // Write no line while the value is empty.
  bool omitEmpty;
  // Two different values as written, which the self-test applies in turn and
  // expects back; nullptr when the written form is not the one read (a path
  // that is normalised, a value with its own text format).
  const char* sampleA;
  const char* sampleB;
};

void WriteBool(std::ostream& out, bool value) { out << (value ? 1 : 0); }

const SettingKey kSettingKeys[] = {
    {"aspect",
     [](const std::string& value) {
       if (value == "16:9") {
         sAspectMode = PortDebug::kAspect_16_9;
       } else if (value == "window") {
         sAspectMode = PortDebug::kAspect_Window;
       } else if (value == "4:3") {
         sAspectMode = PortDebug::kAspect_4_3;
       }
     },
     [](std::ostream& out) {
       out << (sAspectMode == PortDebug::kAspect_16_9    ? "16:9"
               : sAspectMode == PortDebug::kAspect_Window ? "window"
                                                          : "4:3");
     },
     false, "16:9", "window"},
    {"hud_wide", [](const std::string& value) { sHudWide = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sHudWide); }, false, "1", "0"},
    {"cinema_bars", [](const std::string& value) { sCinemaBars = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sCinemaBars); }, false, "1", "0"},
    {"sharp_scan_window", [](const std::string& value) { sSharpScanWindow = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSharpScanWindow); }, false, "1", "0"},
    {"auto_aim", [](const std::string& value) { sAutoAim = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sAutoAim); }, false, "1", "0"},
    {"scan_speed",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kScanSpeedMin && s <= PortDebug::kScanSpeedMax) {
         sScanSpeed = s;
       }
     },
     [](std::ostream& out) { out << sScanSpeed; }, false, "100", "500"},
    {"secondary_crosshairs", [](const std::string& value) { sSecondaryCrosshairs = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSecondaryCrosshairs); }, false, "1", "0"},
    {"menu_standard_buttons", [](const std::string& value) { sMenuStandardButtons = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sMenuStandardButtons); }, false, "1", "0"},
    {"pointer_menus", [](const std::string& value) { PortPointer::SetEnabled(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortPointer::Enabled()); }, false, "1", "0"},
    {"prompt_glyphs",
     [](const std::string& value) {
       for (int i = 0; i < int(std::size(kPromptGlyphs)); ++i) {
         if (value == kPromptGlyphs[i]) {
           sPromptGlyphs.store(i, std::memory_order_relaxed);
         }
       }
     },
     [](std::ostream& out) { out << kPromptGlyphs[sPromptGlyphs.load(std::memory_order_relaxed)]; }, false,
     "playstation", "keyboard"},
    {"show_shader_compilation", [](const std::string& value) { sShowShaderCompilation = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sShowShaderCompilation); }, false, "1", "0"},
    {"show_menu_hint", [](const std::string& value) { sShowMenuHint = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sShowMenuHint); }, false, "1", "0"},
    {"hud_scale",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kHudScaleMin && s <= PortDebug::kHudScaleMax) {
         sHudScale = s;
       }
     },
     [](std::ostream& out) { out << sHudScale; }, false, "50", "100"},
    {"scan_spread",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kScanSpreadMin && s <= PortDebug::kScanSpreadMax) {
         sScanSpread = s;
       }
     },
     [](std::ostream& out) { out << sScanSpread; }, false, "0", "100"},
    {"crosshair_opacity",
     [](const std::string& value) {
       const int s = std::atoi(value.c_str());
       if (s >= PortDebug::kCrosshairOpacityMin && s <= PortDebug::kCrosshairOpacityMax) {
         sCrosshairOpacity = s;
       }
     },
     [](std::ostream& out) { out << sCrosshairOpacity; }, false, "0", "100"},
    {"crosshair_color",
     [](const std::string& value) {
       // RRGGBB, or empty for the game's own colour.
       unsigned rgb = 0;
       sCrosshairCustomColor = value.size() == 6 && std::sscanf(value.c_str(), "%6x", &rgb) == 1;
       if (sCrosshairCustomColor) {
         for (int i = 0; i < 3; ++i) {
           sCrosshairColor[i] = static_cast< float >((rgb >> (16 - 8 * i)) & 0xff) / 255.f;
         }
       }
     },
     [](std::ostream& out) {
       char hex[8] = "";
       if (sCrosshairCustomColor) {
         unsigned rgb = 0;
         for (const float c : sCrosshairColor) {
           rgb = rgb << 8 | static_cast< unsigned >(std::lround(std::clamp(c, 0.f, 1.f) * 255.f));
         }
         std::snprintf(hex, sizeof(hex), "%06x", rgb);
       }
       out << hex;
     },
     false, "ff8000", ""},
    {"hide_helmet", [](const std::string& value) { sHideHelmet = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sHideHelmet); }, false, "1", "0"},
    {"hide_visor_effects", [](const std::string& value) { sHideVisorEffects = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sHideVisorEffects); }, false, "1", "0"},
    {"reveal_map", [](const std::string& value) { sRevealMap = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sRevealMap); }, false, "1", "0"},
    {"map_pickups",
     [](const std::string& value) {
       // Old hand-edited on/true/yes meant the all-pickups dots.
       const int mode = ParseBool(value) ? PortDebug::kMapPickups_All : std::atoi(value.c_str());
       sMapPickups = mode >= PortDebug::kMapPickups_Off && mode <= PortDebug::kMapPickups_Rooms
                         ? mode
                         : PortDebug::kMapPickups_Off;
     },
     [](std::ostream& out) { out << sMapPickups; }, false, "2", "3"},
    {"tracker_progress", [](const std::string& value) { sTrackerProgress = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sTrackerProgress); }, false, "1", "0"},
    {"map_logic_colors", [](const std::string& value) { sMapLogicColors = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sMapLogicColors); }, false, "1", "0"},
    {"map_pickup_kinds", [](const std::string& value) { sMapPickupKinds = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sMapPickupKinds); }, false, "1", "0"},
    {"map_station_pickups", [](const std::string& value) { sMapStationPickups = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sMapStationPickups); }, false, "1", "0"},
    {"map_extra_ui", [](const std::string& value) { sMapExtraUi = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sMapExtraUi); }, false, "1", "0"},
    {"ap_suit_damage",
     [](const std::string& value) {
       const int mode = std::atoi(value.c_str());
       sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
     },
     [](std::ostream& out) { out << sApSuitDamage; }, false, "0", "2"},
    {"cheats", [](const std::string& value) { sCheats = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sCheats); }, false, "1", "0"},
    {"skippable_cutscenes", [](const std::string& value) { sSkippableCutscenes = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSkippableCutscenes); }, false, "1", "0"},
    {"revision_100_compatibility",
     [](const std::string& value) { sRevision100Compatibility = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sRevision100Compatibility); }, false, "1", "0"},
    // Normalised on the way in (NormalizeEmulatorFolder), so not sampled.
    {"emulator_folder", [](const std::string& value) { PortGci::SetEmulatorFolder(value); },
     [](std::ostream& out) { out << PortGci::EmulatorFolder(); }, false, nullptr, nullptr},
    {"share_emulator_card", [](const std::string& value) { PortGci::SetShareEmulatorCard(ParseBool(value)); },
     [](std::ostream& out) { WriteBool(out, PortGci::ShareEmulatorCard()); }, false, "1", "0"},
    {"text_language", [](const std::string& value) { sTextLanguage = value; },
     [](std::ostream& out) { out << sTextLanguage; }, false, "fr", "de"},
    {"elevator_ride",
     [](const std::string& value) {
       const int mode = std::atoi(value.c_str());
       sElevatorRide = mode >= PortDebug::kElevatorRide_Original && mode <= PortDebug::kElevatorRide_Skip
                           ? mode
                           : PortDebug::kElevatorRide_Original;
     },
     [](std::ostream& out) { out << sElevatorRide; }, false, "1", "2"},
    {"boot_logos",
     [](const std::string& value) {
       const int mode = std::atoi(value.c_str());
       sBootLogos = mode >= PortDebug::kBootLogos_Show && mode <= PortDebug::kBootLogos_Off
                        ? mode
                        : PortDebug::kBootLogos_Skippable;
     },
     [](std::ostream& out) { out << sBootLogos; }, false, "0", "2"},
    {"savestate_hotkeys", [](const std::string& value) { sSaveStateHotkeys = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSaveStateHotkeys); }, false, "1", "0"},
    {"quick_resume", [](const std::string& value) { sQuickResume = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sQuickResume); }, false, "1", "0"},
    {"savestate_room_state", [](const std::string& value) { sSaveStateRoomState = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSaveStateRoomState); }, false, "1", "0"},
    {"fov",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f) && f >= PortDebug::kFovMin && f <= PortDebug::kFovMax) {
         sFirstPersonFov = f;
       }
     },
     [](std::ostream& out) { out << sFirstPersonFov; }, false, "45", "72.5"},
    {"gun_offset",
     [](const std::string& value) {
       float v[3] = {};
       if (std::sscanf(value.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
         for (int i = 0; i < 3; ++i) {
           sGunOffset[i] =
               std::isfinite(v[i]) ? std::clamp(v[i], -PortDebug::kGunOffsetMax, PortDebug::kGunOffsetMax) : 0.f;
         }
       }
     },
     [](std::ostream& out) { out << sGunOffset[0] << ',' << sGunOffset[1] << ',' << sGunOffset[2]; }, false,
     "0.5,-0.25,1", "0,0,0"},
    {"msaa", [](const std::string& value) { sMsaa = std::atoi(value.c_str()) >= 4 ? 4 : 1; },
     [](std::ostream& out) { out << sMsaa; }, false, "4", "1"},
    {"opengles",
     [](const std::string& value) {
       sOpenGles = ParseBool(value);
       sOpenGlesAtStart = sOpenGles;
     },
     [](std::ostream& out) { WriteBool(out, sOpenGles); }, false, "1", "0"},
    {"graphics_backend",
     [](const std::string& value) {
       // An unknown value keeps what is already there (the default, Auto, at
       // startup), so a hand-edited or older file never breaks the start.
       PortDebug::EGraphicsBackend parsed = sGraphicsBackend;
       if (PortDebug::ParseGraphicsBackend(value, parsed)) {
         sGraphicsBackend = parsed;
         sGraphicsBackendAtStart = parsed;
       }
     },
     [](std::ostream& out) { out << PortDebug::GraphicsBackendConfigName(sGraphicsBackend); }, false, "d3d12",
     "vulkan"},
    {"gpu_driver",
     [](const std::string& value) {
       sGpuDriver = value;
       sGpuDriverAtStart = value;
     },
     [](std::ostream& out) { out << sGpuDriver; }, false, "turnip", ""},
    {"anisotropy",
     [](const std::string& value) {
       const int a = std::atoi(value.c_str());
       if (a >= 1 && a <= 16) {
         sAnisotropy = a;
       }
     },
     [](std::ostream& out) { out << sAnisotropy; }, false, "1", "16"},
    {"per_pixel_lighting", [](const std::string& value) { sPerPixelLighting = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sPerPixelLighting); }, false, "1", "0"},
    {"sun_shadows", [](const std::string& value) { sSunShadows = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSunShadows); }, false, "1", "0"},
    {"projected_shadows", [](const std::string& value) { sProjectedShadows = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sProjectedShadows); }, false, "1", "0"},
    {"unlock_hard_mode", [](const std::string& value) { sUnlockHardMode = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sUnlockHardMode); }, false, "1", "0"},
    {"unlock_fusion_suit", [](const std::string& value) { sUnlockFusionSuit = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sUnlockFusionSuit); }, false, "1", "0"},
    {"unlock_galleries", [](const std::string& value) { sUnlockGalleries = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sUnlockGalleries); }, false, "1", "0"},
    {"storage_clamp",
     [](const std::string& value) {
       const int v = std::atoi(value.c_str());
       sStorageClamp = v < 0 ? -1 : (v > 0 ? 1 : 0);
       sStorageClampAtStart = sStorageClamp;
     },
     [](std::ostream& out) { out << sStorageClamp; }, false, "-1", "1"},
    {"driver_workarounds",
     [](const std::string& value) {
       sDriverWorkarounds = ParseDriverWorkarounds(value);
       sDriverWorkaroundsAtStart = sDriverWorkarounds;
     },
     [](std::ostream& out) {
       const char* sep = "";
       for (size_t i = 0; i < kDriverWorkaroundCount; ++i) {
         if (sDriverWorkarounds & (1u << i)) {
           out << sep << kDriverWorkarounds[i].key;
           sep = ",";
         }
       }
     },
     true, "keep_depth", "unmonitored_fence,validation"},
    {"speedrun_timer", [](const std::string& value) { sSpeedrunTimer = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSpeedrunTimer); }, false, "1", "0"},
    {"livesplit", [](const std::string& value) { sLiveSplit = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sLiveSplit); }, false, "1", "0"},
    {"livesplit_address",
     [](const std::string& value) {
       if (!value.empty()) {
         sLiveSplitAddress = value;
       }
     },
     [](std::ostream& out) { out << sLiveSplitAddress; }, false, "127.0.0.1:16834", "localhost:1"},
    {"livesplit_split_upgrades", [](const std::string& value) { sLiveSplitSplitUpgrades = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sLiveSplitSplitUpgrades); }, false, "1", "0"},
    // Not "discord": 0.16.0 saved discord=0 for everyone while it was opt-in.
    {"discord_presence", [](const std::string& value) { sDiscord = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sDiscord); }, false, "1", "0"},
    {"update_check", [](const std::string& value) { sUpdateCheck = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sUpdateCheck); }, false, "1", "0"},
    {"mods", [](const std::string& value) { sModsEnabled = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sModsEnabled); }, false, "1", "0"},
    {"original_experience", [](const std::string& value) { sOriginalExperience = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sOriginalExperience); }, false, "1", "0"},
    {"mods_disabled", [](const std::string& value) { sModsDisabled = value; },
     [](std::ostream& out) { out << sModsDisabled; }, false, "a,b", ""},
    {"vsync", [](const std::string& value) { sVsyncEnabled = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sVsyncEnabled); }, false, "1", "0"},
    {"fullscreen", [](const std::string& value) { sFullscreen = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sFullscreen); }, false, "1", "0"},
    {"overlay_windowed", [](const std::string& value) { sOverlayWindowed = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sOverlayWindowed); }, false, "1", "0"},
    {"ui_theme",
     [](const std::string& value) {
       sUiTheme = SDL_strcasecmp(value.c_str(), "prime") == 0        ? 1
                  : SDL_strcasecmp(value.c_str(), "remastered") == 0 ? 2
                  : SDL_strcasecmp(value.c_str(), "plain") == 0      ? 3
                                                                     : 0;
     },
     [](std::ostream& out) {
       out << (sUiTheme == 1 ? "prime" : sUiTheme == 2 ? "remastered" : sUiTheme == 3 ? "plain" : "auto");
     },
     false, "remastered", "auto"},
    {"ui_sounds", [](const std::string& value) { sUiSounds = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sUiSounds); }, false, "1", "0"},
    {"photo_supersample", [](const std::string& value) { PortPhoto::SetSupersample(std::atoi(value.c_str())); },
     [](std::ostream& out) { out << PortPhoto::Supersample(); }, false, "4", "1"},
    {"photo_speed",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f)) {
         PortPhoto::SetSpeed(f);
       }
     },
     [](std::ostream& out) { out << PortPhoto::Speed(); }, false, "0.5", "20"},
    {"render_scale",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f) && f >= 0.f && f <= 4.f) {
         sRenderScale = f;
       }
     },
     [](std::ostream& out) { out << sRenderScale; }, false, "0.5", "2"},
    {"dynamic_res", [](const std::string& value) { sDynamicRes = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sDynamicRes); }, false, "1", "0"},
    {"dynamic_res_target",
     [](const std::string& value) { sDynamicResTarget = std::clamp(std::atoi(value.c_str()), 0, 240); },
     [](std::ostream& out) { out << sDynamicResTarget; }, false, "0", "60"},
    {"dynamic_res_min",
     [](const std::string& value) {
       const float f = static_cast< float >(std::atof(value.c_str()));
       if (std::isfinite(f)) {
         sDynamicResMin = std::clamp(f, 0.5f, 1.f);
       }
     },
     [](std::ostream& out) { out << sDynamicResMin; }, false, "0.5", "0.75"},
    {"upscale_sharpen",
     [](const std::string& value) {
       sSharpenMode = value == "off" ? 0 : value == "always" ? 2 : 1;
     },
     [](std::ostream& out) { out << (sSharpenMode == 0 ? "off" : sSharpenMode == 2 ? "always" : "dynamic"); },
     false, "off", "always"},
    {"upscale_sharpen_amount",
     [](const std::string& value) { sSharpenAmount = std::clamp(std::atoi(value.c_str()), 0, 100); },
     [](std::ostream& out) { out << sSharpenAmount; }, false, "0", "80"},
    {"frame_limit", [](const std::string& value) { sFrameLimitEnabled = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sFrameLimitEnabled); }, false, "1", "0"},
    {"fps_cap", [](const std::string& value) { sFpsCap = ClampFpsCap(std::atoi(value.c_str())); },
     [](std::ostream& out) { out << sFpsCap; }, false, "0", "144"},
    {"sim_rate",
     [](const std::string& value) {
       const long rate = std::strtol(value.c_str(), nullptr, 10);
       if (rate >= 30 && rate <= 480) {
         sSimRate = static_cast< unsigned >(rate);
       }
     },
     [](std::ostream& out) { out << sSimRate; }, false, "30", "120"},
    {"sim_adaptive", [](const std::string& value) { sSimAdaptive = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sSimAdaptive); }, false, "1", "0"},
    {"smooth_frames",
     [](const std::string& value) {
       sSmoothFrames = ParseBool(value);
       sFrameInterpolation = sActorInterpolation = sPoseInterpolation = sParticleInterpolation = sSmoothFrames;
     },
     [](std::ostream& out) { WriteBool(out, sSmoothFrames); }, false, "1", "0"},
    {"room_geo_gpu", [](const std::string& value) { sRoomGeoResident = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sRoomGeoResident); }, false, "1", "0"},
    // The old key, saved as 0 by everyone while it was off by default: only an explicit 1 counts.
    {"room_geo_resident", [](const std::string& value) { sRoomGeoResident = sRoomGeoResident || ParseBool(value); },
     nullptr, false, nullptr, nullptr},
    {"room_geo_min_px",
     [](const std::string& value) { PortRoomGeo::SetMinPixels(std::strtof(value.c_str(), nullptr)); },
     [](std::ostream& out) { out << PortRoomGeo::MinPixels(); }, false, "0", "3.5"},
    {"room_geo_lod",
     [](const std::string& value) { PortRoomGeo::SetLodDistance(std::strtof(value.c_str(), nullptr)); },
     [](std::ostream& out) { out << PortRoomGeo::LodDistance(); }, false, "0.5", "2"},
    {nullptr, nullptr, [](std::ostream& out) { PortInput::WriteControlSettings(out); }, false, nullptr, nullptr},
    {"invulnerable", [](const std::string& value) { sInvulnerable = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sInvulnerable); }, false, "1", "0"},
    // Not "log_file": builds from before the log was on by default wrote
    // log_file=0 into every settings file, which kept it off after an update.
    {"logging", [](const std::string& value) { sLogFile = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sLogFile); }, false, "1", "0"},
    {nullptr, nullptr, [](std::ostream& out) { PortInput::WriteGameplaySettings(out); }, false, nullptr, nullptr},
    // Its own JSON text (PortRandoGen::SettingsText), so not sampled here.
    {"rando_settings",
     [](const std::string& value) {
       PortRandoGen::Settings parsed;
       if (!PortRandoGen::ParseSettings(value, parsed)) {
         parsed = PortRandoGen::Settings();
       }
       std::lock_guard< std::mutex > lock(sRandoMutex);
       sRandoSettings = parsed;
     },
     [](std::ostream& out) {
       std::lock_guard< std::mutex > lock(sRandoMutex);
       out << PortRandoGen::SettingsText(sRandoSettings);
     },
     false, nullptr, nullptr},
    {nullptr, nullptr, [](std::ostream& out) { PortInput::WriteAimSettings(out); }, false, nullptr, nullptr},
    {"game_options", [](const std::string& value) { sGameOptions = value; },
     [](std::ostream& out) { out << sGameOptions; }, true, "0a1b", "ff"},
    {"disc_path", [](const std::string& value) { sDiscPath = value; }, [](std::ostream& out) { out << sDiscPath; },
     true, "/games/Metroid Prime #1 (v1.00).iso", "C:\\Games\\mp.rvz"},
    {"remastered_nsp", [](const std::string& value) { sRemasteredImagePath = value; },
     [](std::ostream& out) { out << sRemasteredImagePath; }, true, "/dumps/mpr.nsp", "/dumps/mpr.xci"},
    {"remastered_keys", [](const std::string& value) { sRemasteredKeysPath = value; },
     [](std::ostream& out) { out << sRemasteredKeysPath; }, true, "/keys/prod.keys", "/k/p.keys"},
    {"remastered_skip", [](const std::string& value) { sRemasteredSkip = value; },
     [](std::ostream& out) { out << sRemasteredSkip; }, true, "geometry,movies", "hud"},
    {"ai_audio", [](const std::string& value) { sAiAudioEnabled = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sAiAudioEnabled); }, false, "1", "0"},
    {"musyx_audio", [](const std::string& value) { sMusyxAudioEnabled = ParseBool(value); },
     [](std::ostream& out) { WriteBool(out, sMusyxAudioEnabled); }, false, "1", "0"},
    {"voices_muted",
     [](const std::string& value) {
       MusyxPortClearSampleMutes();
       const char* cursor = value.c_str();
       while (*cursor != '\0') {
         char* end = nullptr;
         const unsigned long id = std::strtoul(cursor, &end, 10);
         if (end == cursor) {
           break;
         }
         MusyxPortSetSampleMuted(static_cast< unsigned >(id), 1);
         cursor = end;
         while (*cursor == ',' || *cursor == ' ') {
           ++cursor;
         }
       }
     },
     [](std::ostream& out) {
       unsigned muted[64];
       const int mutedCount = MusyxPortGetMutedSamples(muted, 64);
       for (int i = 0; i < mutedCount; ++i) {
         out << (i == 0 ? "" : ",") << muted[i];
       }
     },
     true, "3,7", "12"},
};

// False for a key nothing reads.
bool ApplySetting(const std::string& key, const std::string& value) {
  if (PortInput::ApplySetting(key, value)) {
    return true;
  }
  for (const SettingKey& entry : kSettingKeys) {
    if (entry.key != nullptr && key == entry.key) {
      entry.read(value);
      return true;
    }
  }
  return false;
}

// Applies the key=value lines of a settings file. Only whole-line comments:
// values are written raw, and a path may contain '#'.
void ApplySettingsText(std::istream& text) {
  std::string line;
  while (std::getline(text, line)) {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (!key.empty()) {
      ApplySetting(key, value);
    }
  }
}

// The settings file's text, as written by SaveSettings and copied into the log.
std::string SettingsText() {
  std::ostringstream file;
  file << "# Metroid Prime native port settings. Written by the F1 debug overlay.\n";
  file << "# Environment variables (MP_*) override these for a single run.\n";
  for (const SettingKey& entry : kSettingKeys) {
    if (entry.write == nullptr) {
      continue;
    }
    if (entry.key == nullptr) {
      entry.write(file);
      continue;
    }
    std::ostringstream value;
    entry.write(value);
    if (entry.omitEmpty && value.str().empty()) {
      continue;
    }
    file << entry.key << '=' << value.str() << '\n';
  }
  return file.str();
}

void LoadSettings() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::fprintf(stderr, "metroid_prime_port: loaded settings from %s\n", path.c_str());
  ApplySettingsText(file);
  PortInput::PostLoad();
}

// key=value lines of a settings text, comments skipped.
std::map< std::string, std::string > SettingsValues(const std::string& text) {
  std::map< std::string, std::string > values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t separator = line.find('=');
    if (!line.empty() && line[0] != '#' && separator != std::string::npos) {
      values[line.substr(0, separator)] = line.substr(separator + 1);
    }
  }
  return values;
}

// Paths in the log keep only their file name: the folders say nothing about a bug.
std::string LoggedSettingValue(const std::string& key, const std::string& value) {
  if (key == "disc_path" || key == "remastered_nsp" || key == "remastered_keys") {
    const size_t slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
  }
  return value;
}

// The settings last copied into the log; SaveSettings logs what changed since.
std::map< std::string, std::string > sLoggedSettings;
bool sSettingsLogged = false;

void LogSettingChanges(const std::string& text) {
  std::map< std::string, std::string > values = SettingsValues(text);
  if (sSettingsLogged) {
    for (const auto& [key, value] : values) {
      const auto old = sLoggedSettings.find(key);
      const std::string before = old == sLoggedSettings.end() ? "(unset)" : LoggedSettingValue(key, old->second);
      if (old == sLoggedSettings.end() || old->second != value) {
        PortLog::Write("port: setting %s: %s -> %s\n", key.c_str(), before.c_str(),
                       LoggedSettingValue(key, value).c_str());
      }
    }
    for (const auto& [key, value] : sLoggedSettings) {
      if (values.find(key) == values.end()) {
        PortLog::Write("port: setting %s: %s -> (unset)\n", key.c_str(), LoggedSettingValue(key, value).c_str());
      }
    }
  }
  sLoggedSettings = std::move(values);
}

void SaveSettings() {
  if (!sInitialized || !sSettingsDirty || sConfigReset) {
    return;
  }
  const std::string text = SettingsText();
  LogSettingChanges(text);
  const std::string path = SettingsFilePath();
  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    std::fprintf(stderr, "metroid_prime_port: could not write settings to %s\n", path.c_str());
    return;
  }
  file << text;
  file.flush();
  if (!file) {
    std::fprintf(stderr, "metroid_prime_port: could not write settings to %s (write failed)\n", path.c_str());
    return;
  }
  file.close();
  if (file.fail()) {
    std::fprintf(stderr, "metroid_prime_port: could not write settings to %s (close failed)\n", path.c_str());
    return;
  }
  std::fprintf(stderr, "metroid_prime_port: saved settings to %s\n", path.c_str());
  sSettingsDirty = false;
}

} // namespace
