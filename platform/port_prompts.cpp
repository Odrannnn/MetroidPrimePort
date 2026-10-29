// Binding-aware button prompt icons; see port_prompts.h.

#include "port_prompts.h"
#include "port_log.h"
#include "port_env.h"

#include "port_debug.h"
#include "port_embedded.h"
#include "port_input_devices.h"
#include "port_pointer.h"
#include "port_textures.h"

#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <aurora/texture.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// Prompts that are not one button: the control stick and the C-stick, at rest
// or pushed one way, and the D-pad as a whole (its directions are buttons).
// Numbered past PADButton's range so they share the table with the buttons.
enum : uint32_t {
  PROMPT_STICK = 0x10000,
  PROMPT_STICK_UP,
  PROMPT_STICK_DOWN,
  PROMPT_STICK_LEFT,
  PROMPT_STICK_RIGHT,
  PROMPT_CSTICK,
  PROMPT_CSTICK_UP,
  PROMPT_CSTICK_DOWN,
  PROMPT_CSTICK_LEFT,
  PROMPT_CSTICK_RIGHT,
  PROMPT_DPAD,
};

// The game's prompt art by asset id, sorted by id. CTexture binds these by
// pointer (BindTexture), which holds whatever the content: matching on a texel
// hash missed on any disc whose art differs by a pixel. Covers GC USA and
// Trilogy's Metroid1.pak (whose stick dash frames are not on the GC disc).
struct PromptAsset {
  uint32_t id;
  uint32_t prompt;
};
constexpr PromptAsset kAssets[] = {
    {0x04361847, PAD_BUTTON_B},       {0x05AF9CAA, PAD_BUTTON_A},       {0x08A2E4B9, PAD_TRIGGER_R},
    {0x12A12131, PROMPT_CSTICK_DOWN}, {0x13A7E65D, PROMPT_STICK},       {0x1A29C0E6, PROMPT_CSTICK},
    {0x2176CFF9, PAD_BUTTON_X},       {0x217C015A, PROMPT_STICK},       {0x2A13C23E, PROMPT_CSTICK},
    {0x2B97F6A0, PROMPT_STICK},       {0x34E79314, PAD_TRIGGER_L},      {0x3A446C61, PROMPT_STICK},
    {0x3DBB2482, PAD_BUTTON_A},       {0x4050F102, PROMPT_STICK_LEFT},  {0x409AA72E, PROMPT_CSTICK_RIGHT},
    {0x46434ED3, PAD_TRIGGER_L},      {0x4A414F04, PROMPT_STICK},       {0x5400AEEF, PROMPT_CSTICK},
    {0x56705CA9, PROMPT_STICK},       {0x5D9F9796, PROMPT_CSTICK_UP},   {0x5F556002, PAD_BUTTON_DOWN},
    {0x63E1497C, PROMPT_CSTICK},      {0x6CBE6EF2, PAD_TRIGGER_Z},      {0x6D22966B, PAD_BUTTON_START},
    {0x778CCD6A, PROMPT_STICK_RIGHT}, {0x7946C4C5, PROMPT_CSTICK_LEFT}, {0x833BEE04, PAD_BUTTON_B},
    {0x8ADA8184, PAD_BUTTON_RIGHT},   {0x8B0C22A7, PROMPT_STICK_LEFT},  {0x951546A8, PROMPT_CSTICK_DOWN},
    {0x9530337E, PAD_TRIGGER_R},      {0x95EB0B4C, PROMPT_STICK_UP},    {0x99F6C3B4, PROMPT_STICK},
    {0x9C8A7FD1, PROMPT_CSTICK},      {0x9E9624C9, PROMPT_STICK},       {0xA91A7703, PROMPT_CSTICK_UP},
    {0xA9798329, PROMPT_CSTICK_LEFT}, {0xAB6B9842, PROMPT_CSTICK},      {0xB306E26F, PAD_BUTTON_LEFT},
    {0xBBED079B, PROMPT_STICK},       {0xBCD01ECF, PROMPT_STICK_RIGHT}, {0xC042EC91, PAD_BUTTON_UP},
    {0xC6C483AC, PROMPT_STICK},       {0xCD7B1ACA, PROMPT_CSTICK_RIGHT}, {0xD717196D, PROMPT_STICK},
    {0xD979433D, PAD_TRIGGER_R},      {0xDFEF9E78, PROMPT_STICK},       {0xEA2A1C5C, PAD_BUTTON_Y},
    {0xF13452F8, PROMPT_DPAD},        {0xF2425B21, PAD_TRIGGER_R},      {0xFA931047, PROMPT_STICK_DOWN},
};

const PromptAsset* FindAsset(uint32_t id) {
  const auto it = std::lower_bound(std::begin(kAssets), std::end(kAssets), id,
                                   [](const PromptAsset& asset, uint32_t value) { return asset.id < value; });
  return it != std::end(kAssets) && it->id == id ? &*it : nullptr;
}
constexpr bool AssetsSorted() {
  for (size_t i = 1; i < std::size(kAssets); ++i) {
    if (kAssets[i - 1].id >= kAssets[i].id) {
      return false;
    }
  }
  return true;
}
static_assert(AssetsSorted(), "kAssets must be sorted by id");

// The actions to resolve bindings for, resolved once per action rather than
// once per texture.
struct PromptAction {
  uint32_t prompt;
  const char* label;
};
constexpr PromptAction kActions[] = {
    {PAD_BUTTON_A, "A"},
    {PAD_BUTTON_B, "B"},
    {PAD_BUTTON_X, "X"},
    {PAD_BUTTON_Y, "Y"},
    {PAD_BUTTON_START, "Start"},
    {PAD_TRIGGER_L, "L"},
    {PAD_TRIGGER_R, "R"},
    {PAD_TRIGGER_Z, "Z"},
    {PROMPT_STICK, "stick"},
    {PROMPT_STICK_UP, "stick up"},
    {PROMPT_STICK_DOWN, "stick down"},
    {PROMPT_STICK_LEFT, "stick left"},
    {PROMPT_STICK_RIGHT, "stick right"},
    {PROMPT_CSTICK, "C-stick"},
    {PROMPT_CSTICK_UP, "C-stick up"},
    {PROMPT_CSTICK_DOWN, "C-stick down"},
    {PROMPT_CSTICK_LEFT, "C-stick left"},
    {PROMPT_CSTICK_RIGHT, "C-stick right"},
    {PROMPT_DPAD, "D-pad"},
    {PAD_BUTTON_UP, "D-pad up"},
    {PAD_BUTTON_DOWN, "D-pad down"},
    {PAD_BUTTON_LEFT, "D-pad left"},
    {PAD_BUTTON_RIGHT, "D-pad right"},
};

// Scancodes mapped to icon stems. Aurora reports mouse buttons as negative
// codes; the rest are SDL scancodes. Stems match the files generated into
// <textures>/bindings/ by tools/make_prompt_glyphs.py.
struct KeyIcon {
  int scancode;
  const char* stem;
};
constexpr KeyIcon kKeyIcons[] = {
    {SDL_SCANCODE_A, "keyboard_a"}, {SDL_SCANCODE_B, "keyboard_b"},
    {SDL_SCANCODE_C, "keyboard_c"}, {SDL_SCANCODE_D, "keyboard_d"},
    {SDL_SCANCODE_E, "keyboard_e"}, {SDL_SCANCODE_F, "keyboard_f"},
    {SDL_SCANCODE_G, "keyboard_g"}, {SDL_SCANCODE_H, "keyboard_h"},
    {SDL_SCANCODE_I, "keyboard_i"}, {SDL_SCANCODE_J, "keyboard_j"},
    {SDL_SCANCODE_K, "keyboard_k"}, {SDL_SCANCODE_L, "keyboard_l"},
    {SDL_SCANCODE_M, "keyboard_m"}, {SDL_SCANCODE_N, "keyboard_n"},
    {SDL_SCANCODE_O, "keyboard_o"}, {SDL_SCANCODE_P, "keyboard_p"},
    {SDL_SCANCODE_Q, "keyboard_q"}, {SDL_SCANCODE_R, "keyboard_r"},
    {SDL_SCANCODE_S, "keyboard_s"}, {SDL_SCANCODE_T, "keyboard_t"},
    {SDL_SCANCODE_U, "keyboard_u"}, {SDL_SCANCODE_V, "keyboard_v"},
    {SDL_SCANCODE_W, "keyboard_w"}, {SDL_SCANCODE_X, "keyboard_x"},
    {SDL_SCANCODE_Y, "keyboard_y"}, {SDL_SCANCODE_Z, "keyboard_z"},
    {SDL_SCANCODE_0, "keyboard_0"}, {SDL_SCANCODE_1, "keyboard_1"},
    {SDL_SCANCODE_2, "keyboard_2"}, {SDL_SCANCODE_3, "keyboard_3"},
    {SDL_SCANCODE_4, "keyboard_4"}, {SDL_SCANCODE_5, "keyboard_5"},
    {SDL_SCANCODE_6, "keyboard_6"}, {SDL_SCANCODE_7, "keyboard_7"},
    {SDL_SCANCODE_8, "keyboard_8"}, {SDL_SCANCODE_9, "keyboard_9"},
    {SDL_SCANCODE_UP, "keyboard_arrow_up"}, {SDL_SCANCODE_DOWN, "keyboard_arrow_down"},
    {SDL_SCANCODE_LEFT, "keyboard_arrow_left"}, {SDL_SCANCODE_RIGHT, "keyboard_arrow_right"},
    {SDL_SCANCODE_F1, "keyboard_f1"}, {SDL_SCANCODE_F2, "keyboard_f2"},
    {SDL_SCANCODE_F3, "keyboard_f3"}, {SDL_SCANCODE_F4, "keyboard_f4"},
    {SDL_SCANCODE_F5, "keyboard_f5"}, {SDL_SCANCODE_F6, "keyboard_f6"},
    {SDL_SCANCODE_F7, "keyboard_f7"}, {SDL_SCANCODE_F8, "keyboard_f8"},
    {SDL_SCANCODE_F9, "keyboard_f9"}, {SDL_SCANCODE_F10, "keyboard_f10"},
    {SDL_SCANCODE_F11, "keyboard_f11"}, {SDL_SCANCODE_F12, "keyboard_f12"},
    {SDL_SCANCODE_SPACE, "keyboard_space"}, {SDL_SCANCODE_RETURN, "keyboard_enter"},
    {SDL_SCANCODE_ESCAPE, "keyboard_escape"}, {SDL_SCANCODE_TAB, "keyboard_tab"},
    {SDL_SCANCODE_BACKSPACE, "keyboard_backspace"}, {SDL_SCANCODE_DELETE, "keyboard_delete"},
    {SDL_SCANCODE_INSERT, "keyboard_insert"}, {SDL_SCANCODE_HOME, "keyboard_home"},
    {SDL_SCANCODE_END, "keyboard_end"}, {SDL_SCANCODE_PAGEUP, "keyboard_page_up"},
    {SDL_SCANCODE_PAGEDOWN, "keyboard_page_down"},
    {SDL_SCANCODE_LSHIFT, "keyboard_shift"}, {SDL_SCANCODE_RSHIFT, "keyboard_shift"},
    {SDL_SCANCODE_LCTRL, "keyboard_ctrl"}, {SDL_SCANCODE_RCTRL, "keyboard_ctrl"},
    {SDL_SCANCODE_LALT, "keyboard_alt"}, {SDL_SCANCODE_RALT, "keyboard_alt"},
    {SDL_SCANCODE_LGUI, "keyboard_win"}, {SDL_SCANCODE_RGUI, "keyboard_win"},
    {SDL_SCANCODE_MINUS, "keyboard_minus"}, {SDL_SCANCODE_EQUALS, "keyboard_equals"},
    {SDL_SCANCODE_LEFTBRACKET, "keyboard_bracket_open"},
    {SDL_SCANCODE_RIGHTBRACKET, "keyboard_bracket_close"},
    {SDL_SCANCODE_SEMICOLON, "keyboard_semicolon"}, {SDL_SCANCODE_APOSTROPHE, "keyboard_apostrophe"},
    {SDL_SCANCODE_COMMA, "keyboard_comma"}, {SDL_SCANCODE_PERIOD, "keyboard_period"},
    {SDL_SCANCODE_SLASH, "keyboard_slash_forward"}, {SDL_SCANCODE_BACKSLASH, "keyboard_slash_back"},
    {SDL_SCANCODE_NONUSBACKSLASH, "keyboard_slash_back"}, {SDL_SCANCODE_GRAVE, "keyboard_tilde"},
    {SDL_SCANCODE_CAPSLOCK, "keyboard_capslock"}, {SDL_SCANCODE_PRINTSCREEN, "keyboard_printscreen"},
    {SDL_SCANCODE_SCROLLLOCK, "keyboard_scroll_lock"}, {SDL_SCANCODE_PAUSE, "keyboard_pause"},
    // The keypad. The pack has no keypad digits, so those share the main row's.
    {SDL_SCANCODE_KP_0, "keyboard_0"}, {SDL_SCANCODE_KP_1, "keyboard_1"},
    {SDL_SCANCODE_KP_2, "keyboard_2"}, {SDL_SCANCODE_KP_3, "keyboard_3"},
    {SDL_SCANCODE_KP_4, "keyboard_4"}, {SDL_SCANCODE_KP_5, "keyboard_5"},
    {SDL_SCANCODE_KP_6, "keyboard_6"}, {SDL_SCANCODE_KP_7, "keyboard_7"},
    {SDL_SCANCODE_KP_8, "keyboard_8"}, {SDL_SCANCODE_KP_9, "keyboard_9"},
    {SDL_SCANCODE_KP_ENTER, "keyboard_numpad_enter"}, {SDL_SCANCODE_KP_PLUS, "keyboard_numpad_plus"},
    {SDL_SCANCODE_KP_MINUS, "keyboard_minus"}, {SDL_SCANCODE_KP_MULTIPLY, "keyboard_asterisk"},
    {SDL_SCANCODE_KP_DIVIDE, "keyboard_slash_forward"}, {SDL_SCANCODE_KP_PERIOD, "keyboard_period"},
    {SDL_SCANCODE_NUMLOCKCLEAR, "keyboard_numlock"},
    {PAD_KEY_MOUSE_LEFT, "mouse_left"}, {PAD_KEY_MOUSE_RIGHT, "mouse_right"},
    {PAD_KEY_MOUSE_MIDDLE, "mouse_scroll"},
    {PAD_KEY_MOUSE_X1, "mouse_side_back"}, {PAD_KEY_MOUSE_X2, "mouse_side_forward"},
};

// A GameCube pad's buttons carry GameCube labels, which SDL's positional names
// don't give; Aurora's default GC mappings (lib/dolphin/pad/pad.cpp) pin each
// SDL button to the GC button of the same name, so they double as the labels.
// Kept in step with g_defaultButtonsGamecube / g_defaultButtonsNSOGamecube.
struct GameCubeButton {
  int sdlButton;
  PADButton action;
  const char* stem;
};
constexpr GameCubeButton kGameCubeButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A, "gamecube_a"},
    {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_B, "gamecube_b"},
    {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_X, "gamecube_x"},
    {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y, "gamecube_y"},
    {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START, "gamecube_start"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z, "gamecube_z"},
    {SDL_GAMEPAD_BUTTON_MISC3, PAD_TRIGGER_L, "gamecube_l"},
    {SDL_GAMEPAD_BUTTON_MISC4, PAD_TRIGGER_R, "gamecube_r"},
    {PAD_NATIVE_BUTTON_TRIGGER_LEFT, PAD_TRIGGER_L, "gamecube_l"},
    {PAD_NATIVE_BUTTON_TRIGGER_RIGHT, PAD_TRIGGER_R, "gamecube_r"},
};
constexpr GameCubeButton kNsoGameCubeButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A, "gamecube_a"},
    {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_B, "gamecube_b"},
    {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_X, "gamecube_x"},
    {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y, "gamecube_y"},
    {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START, "gamecube_start"},
    {SDL_GAMEPAD_BUTTON_BACK, PAD_TRIGGER_Z, "gamecube_z"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_TRIGGER_L, "gamecube_l"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_R, "gamecube_r"},
    {PAD_NATIVE_BUTTON_TRIGGER_LEFT, PAD_TRIGGER_L, "gamecube_l"},
    {PAD_NATIVE_BUTTON_TRIGGER_RIGHT, PAD_TRIGGER_R, "gamecube_r"},
};

// The native pad button that presses `button` for the prompts: the bindings in
// use (Remap's, and the "Menus and map" rows in menus and the map while
// "Separate menu buttons" applies), or PAD_NATIVE_BUTTON_INVALID.
u32 PadNativeForButton(PADButton button) {
  PortInput::Input in;
  if (!PortInputDevices::PromptButtonInput(button, PortInput::Family::Pad, in)) {
    return PAD_NATIVE_BUTTON_INVALID;
  }
  return PortInputDevices::PromptNativeButton(in);
}

// The icon for `button` on a GameCube pad, or empty to leave the game's own art,
// which is already right for a GC pad on its default mapping. So only an action
// moved to another button gets an icon. The GameCube touch layout also resolves
// to "gamecube", hence the check that a GC pad really is in port 0.
std::string GameCubeStemForButton(PADButton button) {
  const PADControllerType type = PADGetControllerType(PAD_CHAN0);
  const GameCubeButton* table = nullptr;
  size_t tableSize = 0;
  if (type == PAD_TYPE_GAMECUBE) {
    table = kGameCubeButtons;
    tableSize = sizeof(kGameCubeButtons) / sizeof(kGameCubeButtons[0]);
  } else if (type == PAD_TYPE_NSO_GAMECUBE) {
    table = kNsoGameCubeButtons;
    tableSize = sizeof(kNsoGameCubeButtons) / sizeof(kNsoGameCubeButtons[0]);
  } else {
    return {};
  }
  const int native = static_cast<int>(PadNativeForButton(button));
  for (size_t j = 0; j < tableSize; ++j) {
    if (table[j].sdlButton == native) {
      return table[j].action == button ? std::string() : std::string(table[j].stem);
    }
  }
  return {};
}

// The SDL button a mapping points at, named the way the generated pad icons
// are (tools/make_prompt_glyphs.py writes "<device>_<suffix>").
const char* SuffixForSdlButton(int button) {
  switch (button) {
  case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
  case SDL_GAMEPAD_BUTTON_EAST: return "east";
  case SDL_GAMEPAD_BUTTON_WEST: return "west";
  case SDL_GAMEPAD_BUTTON_NORTH: return "north";
  case SDL_GAMEPAD_BUTTON_START: return "start";
  case SDL_GAMEPAD_BUTTON_BACK: return "back";
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "leftshoulder";
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "rightshoulder";
  case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "leftstick";
  case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "rightstick";
  case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpad_up";
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpad_down";
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpad_left";
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpad_right";
  case PAD_NATIVE_BUTTON_TRIGGER_LEFT: return "lt";
  case PAD_NATIVE_BUTTON_TRIGGER_RIGHT: return "rt";
  default: return nullptr;
  }
}

std::string sBindingsDir;
bool sEnabled = false;

// Which input the player last used. The prompts, and the device folder a
// texture pack is read from (PortTextures asks ActiveDevice too), follow that
// rather than whatever happens to be plugged in, so switching between a
// keyboard, a pad and the touch overlay swaps the icons over. Pad is the
// starting state: with no pad connected the pad device resolves to "keyboard"
// anyway.
enum class ActiveInput { Pad, Keyboard, TouchXbox, TouchGameCube };
std::atomic< ActiveInput > sActiveInput{ActiveInput::Pad};
// Within Keyboard, whether a key rather than the mouse was used last. When the
// mouse was (PortPointer::MouseIsLast), a screen the pointer drives shows A and B
// as its clicks.
std::atomic< bool > sKeyInput{false};

// Keys sent by the phone or the OS rather than a keyboard in the player's hands.
bool IsSystemKey(SDL_Scancode scancode) {
  switch (scancode) {
  case SDL_SCANCODE_AC_BACK:
  case SDL_SCANCODE_VOLUMEUP:
  case SDL_SCANCODE_VOLUMEDOWN:
  case SDL_SCANCODE_MUTE:
  case SDL_SCANCODE_MEDIA_PLAY:
  case SDL_SCANCODE_MEDIA_PAUSE:
  case SDL_SCANCODE_MEDIA_PLAY_PAUSE:
  case SDL_SCANCODE_MEDIA_NEXT_TRACK:
  case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK:
  case SDL_SCANCODE_MEDIA_STOP:
  case SDL_SCANCODE_POWER:
    return true;
  default:
    return false;
  }
}

bool IsTouchMouse(SDL_MouseID which) { return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID; }

// Only deliberate input switches the device. The touch overlay is itself a
// virtual pad, whose events would undo NoteTouchInput on the next pump; a touch
// or pen also arrives as a mouse; and a stick resting a little off centre would
// flip back to the pad every frame while the keyboard is in use, and each flip
// re-registers textures and clears Aurora's texture cache. Mouse motion is not
// deliberate either: aiming with the mouse while playing on a pad (or a Steam
// Deck trackpad or gyro sent as a mouse) would swap the icons back and forth
// (#215), so it never leaves the pad; it only turns keys into clicks once the
// keyboard is already in use.
bool SDLCALL active_input_watch(void*, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_MOUSE_MOTION:
    if (!IsTouchMouse(event->motion.which) &&
        sActiveInput.load(std::memory_order_relaxed) == ActiveInput::Keyboard) {
      sKeyInput.store(false, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (!IsTouchMouse(event->button.which)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
      sKeyInput.store(false, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    if (!IsTouchMouse(event->wheel.which)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
      sKeyInput.store(false, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_KEY_DOWN:
    // Not TEXT_INPUT: on Android that is the soft keyboard typing into the
    // overlay, not a physical keyboard.
    if (!event->key.repeat && !IsSystemKey(event->key.scancode)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
      sKeyInput.store(true, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    if (!SDL_IsJoystickVirtual(event->gbutton.which)) {
      sActiveInput.store(ActiveInput::Pad, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    // The same threshold the touch overlay uses to decide a pad is in use.
    if (std::abs(static_cast< int >(event->gaxis.value)) > 16000 &&
        !SDL_IsJoystickVirtual(event->gaxis.which)) {
      sActiveInput.store(ActiveInput::Pad, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_GAMEPAD_ADDED:
    if (!SDL_IsJoystickVirtual(event->gdevice.which)) {
      sActiveInput.store(ActiveInput::Pad, std::memory_order_relaxed);
    }
    break;
  default:
    break;
  }
  return true;
}

// True when the icons are served from the executable (PortEmbedded) rather than
// <textures>/bindings; sBindingsDir is then the embedded folder prefix.
bool sEmbedded = false;

// The whole file at `path`: an embedded entry, else a file on disk.
bool ReadIconFile(const std::string& path, std::vector<uint8_t>& out) {
  if (sEmbedded) {
    const std::span<const uint8_t> bytes = PortEmbedded::Find(path);
    out.assign(bytes.begin(), bytes.end());
    return !out.empty();
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(out.data()), size);
  return !in.fail();
}

// The file `name` in the bindings folder, or an empty string when it is not there.
std::string FindIconFile(const std::string& name) {
  if (sEmbedded) {
    std::string path = sBindingsDir + "/" + name;
    return PortEmbedded::Find(path).empty() ? std::string() : path;
  }
  const std::filesystem::path path = std::filesystem::path(sBindingsDir) / name;
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) ? path.string() : std::string();
}

// The width and height a DDS header declares, or false if the file is too short
// to hold one. A DDS opens with the "DDS " magic then a 124-byte header, of
// which the height and width are the two little-endian uint32 at offset 12.
bool IconDimensions(const std::string& path, uint32_t& width, uint32_t& height) {
  std::vector<uint8_t> bytes;
  if (!ReadIconFile(path, bytes) || bytes.size() < 20 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
    return false;
  }
  uint32_t both[2] = {};
  std::memcpy(both, bytes.data() + 12, sizeof(both));
  height = both[0];
  width = both[1];
  return true;
}

// The read callback's userData. Aurora worker threads can still be reading an
// icon after Apply has unregistered it and moved on to another path, so each
// path gets storage that is never changed or freed. Only the main thread adds
// to the set, and there are only as many entries as distinct icon files.
const std::string* StablePath(const std::string& path) {
  static std::set<std::string> sPaths;
  return &*sPaths.insert(path).first;
}

// Serves one generated icon as if it were a replacement file. Called from
// Aurora worker threads, so it only touches the filesystem.
bool ReadIconBytes(void* userData, const char* path, std::vector<uint8_t>& out) {
  // The prompts have a single mip, so any mip sidecar Aurora probes
  // for is a miss rather than the same image again.
  if (path != nullptr && std::strstr(path, "_mip") != nullptr) {
    return false;
  }
  return ReadIconFile(*static_cast<const std::string*>(userData), out);
}

const char* StemForScancode(int scancode) {
  for (const KeyIcon& icon : kKeyIcons) {
    if (icon.scancode == scancode) {
      return icon.stem;
    }
  }
  return nullptr;
}

// A keyboard-family input as the scancode the icon table uses (a mouse button
// as Aurora's PAD_KEY_MOUSE_*), or PAD_KEY_INVALID.
int ScancodeForInput(const PortInput::Input& in) {
  if (in.device == PortInput::Device::Key) {
    return in.code;
  }
  if (in.device == PortInput::Device::MouseButton) {
    // Each run of mouse codes (PortInputDevices::kMouse*) is SDL's left, middle,
    // right, X1, X2.
    const int button = in.code % PortInputDevices::kMouseKeySlot;
    return button <= PAD_KEY_MOUSE_LEFT - PAD_KEY_MOUSE_X2 ? PAD_KEY_MOUSE_LEFT - button : PAD_KEY_INVALID;
  }
  return PAD_KEY_INVALID;
}

// The key (or mouse button) bound to `button`, or PAD_KEY_INVALID.
int KeyForButton(PADButton button) {
  PortInput::Input in;
  return PortInputDevices::PromptButtonInput(button, PortInput::Family::Keyboard, in) ? ScancodeForInput(in)
                                                                                       : PAD_KEY_INVALID;
}

// The key that pushes `axis`, or PAD_KEY_INVALID.
int KeyForAxis(PADAxis axis) {
  PortInput::Input in;
  return PortInputDevices::PromptAxisInput(axis, PortInput::Family::Keyboard, in) ? ScancodeForInput(in)
                                                                                  : PAD_KEY_INVALID;
}

// A whole stick or D-pad on keys: the cluster icon when the four keys are one
// the pack draws, else the up key's own icon, as the nearest single thing to
// name. Keys are up, down, left, right.
std::string KeyClusterStem(const int (&keys)[4]) {
  struct Cluster {
    int keys[4];
    const char* stem;
  };
  static constexpr Cluster kClusters[] = {
      {{SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D}, "keyboard_wasd"},
      {{SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L}, "keyboard_ijkl"},
      {{SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT}, "keyboard_arrows"},
  };
  for (const Cluster& cluster : kClusters) {
    if (std::equal(std::begin(keys), std::end(keys), std::begin(cluster.keys))) {
      return cluster.stem;
    }
  }
  const char* up = StemForScancode(keys[0]);
  return up != nullptr ? std::string(up) : std::string();
}

// The icon stem for whatever is bound to `button` on `device`, or empty when
// the port has no icon for it (in which case the game's own art shows).
// Both follow the bindings in use (PortInputDevices::PromptButtonInput), so a
// remapped button shows the key or button it was remapped to.
std::string IconStemForButton(PADButton button, const char* device) {
  if (std::strcmp(device, "gamecube") == 0) {
    return GameCubeStemForButton(button);
  }
  if (std::strcmp(device, "keyboard") == 0) {
    // On a screen the pointer drives, a click is A and a right click B.
    if (!sKeyInput.load(std::memory_order_relaxed) && PortPointer::MouseIsLast() &&
        PortPointer::ScreenActive()) {
      if (button == PAD_BUTTON_A) {
        return "mouse_left";
      }
      if (button == PAD_BUTTON_B) {
        return "mouse_right";
      }
    }
    const char* stem = StemForScancode(KeyForButton(button));
    return stem != nullptr ? std::string(stem) : std::string();
  }

  // A pad's L and R are analog triggers unless remapped to a button (a preset
  // puts R on a stick click); the generated set carries the triggers under one
  // name per action.
  const bool trigger = button == PAD_TRIGGER_L || button == PAD_TRIGGER_R;
  const std::string triggerStem = std::string(device) + (button == PAD_TRIGGER_L ? "_lt" : "_rt");
  const char* suffix = SuffixForSdlButton(static_cast<int>(PadNativeForButton(button)));
  if (suffix != nullptr) {
    return std::string(device) + "_" + suffix;
  }
  return trigger ? triggerStem : std::string();
}

// The pad axes a stick prompt stands for, in up, down, left, right order.
constexpr PADAxis kStickAxes[4] = {PAD_AXIS_LEFT_Y_POS, PAD_AXIS_LEFT_Y_NEG, PAD_AXIS_LEFT_X_NEG,
                                   PAD_AXIS_LEFT_X_POS};
constexpr PADAxis kCStickAxes[4] = {PAD_AXIS_RIGHT_Y_POS, PAD_AXIS_RIGHT_Y_NEG, PAD_AXIS_RIGHT_X_NEG,
                                    PAD_AXIS_RIGHT_X_POS};

// The icon for one direction of a pad axis: the stick and the way it is pushed,
// or the button driving the axis. With `whole`, the stick itself.
std::string PadStemForAxis(PADAxis axis, const char* device, bool whole) {
  PortInput::Input in;
  if (!PortInputDevices::PromptAxisInput(axis, PortInput::Family::Pad, in)) {
    return {};
  }
  if (in.device == PortInput::Device::PadAxis) {
    const int native = in.code;
    const bool leftStick = native == SDL_GAMEPAD_AXIS_LEFTX || native == SDL_GAMEPAD_AXIS_LEFTY;
    const bool rightStick = native == SDL_GAMEPAD_AXIS_RIGHTX || native == SDL_GAMEPAD_AXIS_RIGHTY;
    if (!leftStick && !rightStick) {
      return {};  // a trigger driving a stick: no art for that
    }
    std::string stem = std::string(device) + (leftStick ? "_stick_l" : "_stick_r");
    if (whole || in.dir == 0) {
      return stem;
    }
    // SDL's Y axis is positive downwards.
    const bool positive = in.dir > 0;
    const bool vertical = native == SDL_GAMEPAD_AXIS_LEFTY || native == SDL_GAMEPAD_AXIS_RIGHTY;
    return stem + (vertical ? (positive ? "_down" : "_up") : (positive ? "_right" : "_left"));
  }
  const char* suffix = SuffixForSdlButton(static_cast<int>(PortInputDevices::PromptNativeButton(in)));
  if (suffix == nullptr) {
    return {};
  }
  // A stick moved onto the D-pad reads as the D-pad; any other set of
  // buttons has no single icon.
  if (whole) {
    return std::strncmp(suffix, "dpad_", 5) == 0 ? std::string(device) + "_dpad" : std::string();
  }
  return std::string(device) + "_" + suffix;
}

// The icon for a stick prompt: the whole stick, or one way (dir 0-3 = up, down,
// left, right; -1 = the whole stick).
std::string IconStemForStick(const PADAxis (&axes)[4], int dir, const char* device) {
  if (std::strcmp(device, "gamecube") == 0) {
    return {};  // the game's stick art is the GameCube's
  }
  if (std::strcmp(device, "keyboard") == 0) {
    if (dir >= 0) {
      const char* stem = StemForScancode(KeyForAxis(axes[dir]));
      return stem != nullptr ? std::string(stem) : std::string();
    }
    const int keys[4] = {KeyForAxis(axes[0]), KeyForAxis(axes[1]), KeyForAxis(axes[2]),
                         KeyForAxis(axes[3])};
    return KeyClusterStem(keys);
  }
  // The whole stick is named by its up direction's mapping.
  return PadStemForAxis(axes[dir >= 0 ? dir : 0], device, dir < 0);
}

std::string IconStemForDpad(const char* device) {
  if (std::strcmp(device, "gamecube") == 0) {
    return {};
  }
  if (std::strcmp(device, "keyboard") == 0) {
    const int keys[4] = {KeyForButton(PAD_BUTTON_UP), KeyForButton(PAD_BUTTON_DOWN),
                         KeyForButton(PAD_BUTTON_LEFT), KeyForButton(PAD_BUTTON_RIGHT)};
    return KeyClusterStem(keys);
  }
  // The pad's D-pad unless up has moved elsewhere, in which case that button.
  const std::string up = IconStemForButton(PAD_BUTTON_UP, device);
  const std::string dpadUp = std::string(device) + "_dpad_up";
  return up == dpadUp ? std::string(device) + "_dpad" : up;
}

// The touch twin-stick layout's hints. The overlay is no pad, so the real pad's mappings
// (PADGetButtonMappings) mean nothing here: this is what the overlay sends for each GameCube
// action, in Remastered's Dual Sticks positions. Z is the Map pill (Menu glyph) and Start the
// Pause button (View glyph); the overlay draws those glyphs on them. The D-pad picks visors, and
// with the Beam button (Y) held the beams, so the C-stick's beam hints show the D-pad direction
// that picks them (the pad preset shows the right stick there, which twin stick consumes).
std::string TouchTwinStemForPrompt(uint32_t prompt) {
  switch (prompt) {
  case PROMPT_STICK: return "xbox_stick_l";
  case PROMPT_STICK_UP: return "xbox_stick_l_up";
  case PROMPT_STICK_DOWN: return "xbox_stick_l_down";
  case PROMPT_STICK_LEFT: return "xbox_stick_l_left";
  case PROMPT_STICK_RIGHT: return "xbox_stick_l_right";
  case PROMPT_CSTICK: return "xbox_dpad";
  case PROMPT_CSTICK_UP: return "xbox_dpad_up";
  case PROMPT_CSTICK_DOWN: return "xbox_dpad_down";
  case PROMPT_CSTICK_LEFT: return "xbox_dpad_left";
  case PROMPT_CSTICK_RIGHT: return "xbox_dpad_right";
  case PROMPT_DPAD: return "xbox_dpad";
  case PAD_BUTTON_UP: return "xbox_dpad_up";
  case PAD_BUTTON_DOWN: return "xbox_dpad_down";
  case PAD_BUTTON_LEFT: return "xbox_dpad_left";
  case PAD_BUTTON_RIGHT: return "xbox_dpad_right";
  case PAD_BUTTON_A: return "xbox_rt";
  case PAD_BUTTON_B: return "xbox_south";
  case PAD_BUTTON_X: return "xbox_west";
  case PAD_BUTTON_Y: return "xbox_rightshoulder";
  case PAD_TRIGGER_L: return "xbox_lt";
  case PAD_TRIGGER_R: return "xbox_rightstick";
  case PAD_TRIGGER_Z: return "xbox_start";
  case PAD_BUTTON_START: return "xbox_back";
  default: return {};
  }
}

std::string IconStemForPrompt(uint32_t prompt, const char* device) {
  if (sActiveInput.load(std::memory_order_relaxed) == ActiveInput::TouchXbox &&
      std::strcmp(device, "xbox") == 0) {
    return TouchTwinStemForPrompt(prompt);
  }
  switch (prompt) {
  case PROMPT_STICK: return IconStemForStick(kStickAxes, -1, device);
  case PROMPT_STICK_UP: return IconStemForStick(kStickAxes, 0, device);
  case PROMPT_STICK_DOWN: return IconStemForStick(kStickAxes, 1, device);
  case PROMPT_STICK_LEFT: return IconStemForStick(kStickAxes, 2, device);
  case PROMPT_STICK_RIGHT: return IconStemForStick(kStickAxes, 3, device);
  case PROMPT_CSTICK: return IconStemForStick(kCStickAxes, -1, device);
  case PROMPT_CSTICK_UP: return IconStemForStick(kCStickAxes, 0, device);
  case PROMPT_CSTICK_DOWN: return IconStemForStick(kCStickAxes, 1, device);
  case PROMPT_CSTICK_LEFT: return IconStemForStick(kCStickAxes, 2, device);
  case PROMPT_CSTICK_RIGHT: return IconStemForStick(kCStickAxes, 3, device);
  case PROMPT_DPAD: return IconStemForDpad(device);
  default: return IconStemForButton(static_cast<PADButton>(prompt), device);
  }
}

// The icon file for `stem` at width x height, or an empty string. Prefers the
// binding generated for this texture's size, then the unsuffixed one. A
// single stem can serve textures of different sizes - the C-stick prompts are
// one 32x32 and one 64x32 - and a mismatch is not scaled by the game: it reads
// 64x32 of pixels out of 32x32 of data and draws the right-hand half as noise.
// tools/make_prompt_glyphs.py writes the sized variants for exactly this reason.
std::string FindIconForSize(const std::string& stem, uint32_t width, uint32_t height) {
  char sized[160];
  std::snprintf(sized, sizeof(sized), "%s_%ux%u.dds", stem.c_str(), width, height);
  std::string path = FindIconFile(sized);
  return path.empty() ? FindIconFile(stem + ".dds") : path;
}

// Prompt textures bound by asset id (BindTexture), keyed by their CTexture.
struct BoundPrompt {
  uint32_t prompt;
  uint32_t width;
  uint32_t height;
  aurora::texture::ReplacementRegistration handle{};
  bool registered = false;
};
std::unordered_map<const void*, BoundPrompt> sBound;
// Each action's stem as Poll last resolved it, by kActions index.
std::string sActionStems[std::size(kActions)];

const std::string& ActionStem(uint32_t prompt) {
  for (size_t i = 0; i < std::size(kActions); ++i) {
    if (kActions[i].prompt == prompt) {
      return sActionStems[i];
    }
  }
  static const std::string kNone;
  return kNone;
}

void ApplyBound(const void* owner, BoundPrompt& bound, const std::string& stem) {
  if (bound.registered) {
    aurora::texture::unregister_replacement(bound.handle);
    bound.handle = {};
    bound.registered = false;
  }
  if (stem.empty()) {
    return;
  }
  const std::string iconPath = FindIconForSize(stem, bound.width, bound.height);
  uint32_t iconWidth = 0;
  uint32_t iconHeight = 0;
  // Any whole multiple of the texture's size, the same on both axes: the icons are drawn
  // larger than the game's art so they stay sharp on a scaled-up HUD (#229).
  if (iconPath.empty() || !IconDimensions(iconPath, iconWidth, iconHeight) || bound.width == 0 ||
      bound.height == 0 || iconWidth % bound.width != 0 || iconWidth < bound.width ||
      iconHeight * bound.width != iconWidth * bound.height) {
    return;
  }
  const std::string* path = StablePath(iconPath);
  // Above the user's texture pack (priority 1): a remapped action still shows
  // the key or button it is bound to, whatever art the pack has for it.
  bound.handle = aurora::texture::register_virtual_replacement(
      aurora::texture::TexturePointerKey{owner}, *path,
      aurora::texture::VirtualFileSource{&ReadIconBytes, const_cast<std::string*>(path)},
      aurora::texture::ReplacementOptions{.priority = 2});
  bound.registered = bound.handle.id != 0;
}

} // namespace

namespace PortPrompts {
// "gamecube" gets no icons of its own, which is deliberate: the game's own
// prompt art already is the GameCube set, so only a remapped GC pad button gets
// an icon (GameCubeStemForButton).
const char* ActiveDevice() {
  // Read once: it is asked every frame, and nothing sets it while the game runs.
  static const char* const env = port::EnvString("MP_TEXTURE_DEVICE");
  if (env != nullptr && env[0] != '\0') {
    return env;
  }
  // Original experience shows the disc's own prompts, whatever is being pressed.
  if (PortDebug::OriginalExperience()) {
    return "gamecube";
  }
  if (const char* forced = PortDebug::PromptGlyphs()) {
    return forced;
  }
  switch (sActiveInput.load(std::memory_order_relaxed)) {
  case ActiveInput::Keyboard:
    return "keyboard";
  case ActiveInput::TouchXbox:
    return "xbox";
  case ActiveInput::TouchGameCube:
    return "gamecube";
  case ActiveInput::Pad:
  default:
    return PortTextures::PadDeviceName();
  }
}

void Initialize(const char* textureRoot) {
  const bool embedded = textureRoot == nullptr && !PortEmbedded::Under("textures/bindings/").empty();
  // Tracked even without generated icons, since a texture pack's device
  // folder and rumble (KeyboardActive) follow it too.
  SDL_AddEventWatch(active_input_watch, nullptr);
  if (!embedded && (textureRoot == nullptr || textureRoot[0] == '\0')) {
    return;
  }
  if (embedded) {
    sBindingsDir = "textures/bindings";
    sEmbedded = true;
  } else {
    const std::filesystem::path bindingsDir = std::filesystem::path(textureRoot) / "bindings";
    std::error_code ec;
    if (!std::filesystem::is_directory(bindingsDir, ec)) {
      return;
    }
    sBindingsDir = bindingsDir.string();
  }
  sEnabled = true;
  Poll();
}

void Poll() {
  if (!sEnabled) {
    return;
  }
  const char* device = ActiveDevice();
  // Resolving every action's binding builds strings and rescans the bindings, so it runs when
  // the device changes and otherwise a few times a second, which still follows a rebind.
  static std::string sLastDevice;
  static uint64_t sLastResolveNs = 0;
  const uint64_t nowNs = SDL_GetTicksNS();
  const bool deviceChanged = sLastDevice != device;
  if (!deviceChanged && sLastResolveNs != 0 && nowNs - sLastResolveNs < 250000000ull) {
    return;
  }
  sLastDevice = device;
  sLastResolveNs = nowNs != 0 ? nowNs : 1;
  for (size_t a = 0; a < std::size(kActions); ++a) {
    const PromptAction& action = kActions[a];
    const std::string stem = IconStemForPrompt(action.prompt, device);
    if (stem == sActionStems[a]) {
      continue;
    }
    sActionStems[a] = stem;
    size_t applied = 0;
    for (auto& [owner, bound] : sBound) {
      if (bound.prompt == action.prompt) {
        ApplyBound(owner, bound, stem);
        ++applied;
      }
    }
    if (applied != 0) {
      PortLog::Write("metroid_prime_port: prompt %s %s\n", action.label,
                     stem.empty() ? "(the game's own art)" : stem.c_str());
    }
  }
}
bool KeyboardActive() { return sActiveInput.load(std::memory_order_relaxed) == ActiveInput::Keyboard; }

bool IsPromptAsset(uint32_t id) { return FindAsset(id) != nullptr; }

bool BindTexture(const void* owner, uint32_t id, uint32_t width, uint32_t height) {
  UnbindTexture(owner);
  const PromptAsset* asset = FindAsset(id);
  if (!sEnabled || asset == nullptr || owner == nullptr) {
    return false;
  }
  BoundPrompt& bound = sBound[owner] = BoundPrompt{.prompt = asset->prompt, .width = width, .height = height};
  ApplyBound(owner, bound, ActionStem(asset->prompt));
  return true;
}

void UnbindTexture(const void* owner) {
  const auto found = sBound.find(owner);
  if (found == sBound.end()) {
    return;
  }
  if (found->second.registered) {
    aurora::texture::unregister_replacement(found->second.handle);
  }
  sBound.erase(found);
}

void NoteTouchInput(bool xboxLayout) {
  sActiveInput.store(xboxLayout ? ActiveInput::TouchXbox : ActiveInput::TouchGameCube,
                     std::memory_order_relaxed);
}
} // namespace PortPrompts
