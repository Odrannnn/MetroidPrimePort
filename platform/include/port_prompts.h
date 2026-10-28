#pragma once

#include <cstdint>

// Binding-aware button prompt icons.
//
// The game draws each prompt from a fixed texture, so a shipped replacement can
// only ever show one device's icon. This module instead registers, at runtime,
// the icon for the input actually bound to each prompt's action, so rebinding a
// key or mouse button is reflected in game. The icons are pre-rendered per
// input by tools/make_prompt_glyphs.py into <textures>/bindings/.
//
// A keyboard follows the key bindings and a pad follows its button mapping. The
// icons are registered above the static per-device set, which stays in place
// for any prompt the module has no icon for.
namespace PortPrompts {
// The device the player last used: "keyboard", the connected pad's type (see
// PortTextures::PadDeviceName), or the touch overlay's layout ("xbox" or
// "gamecube"). MP_TEXTURE_DEVICE overrides it. Both the prompts and the static
// texture set follow this.
const char* ActiveDevice();

// True while the keyboard or mouse is the input the player last used, ignoring
// the MP_TEXTURE_DEVICE and prompt glyph overrides. Rumble is held off then, so a
// pad left plugged in does not shake while the game is played on keyboard/mouse.
bool KeyboardActive();

// Call once after Aurora is up, with the same texture root the replacements
// came from (null or empty disables the module).
void Initialize(const char* textureRoot);

// Re-registers an action's icon when its binding changed. Call once per frame.
void Poll();

// Tells the prompts that the touch overlay was just used, and in which layout
// (true for the twin-stick Xbox arrangement, false for the GameCube one), so the
// in-game prompts follow the input the player actually reached for.
void NoteTouchInput(bool xboxLayout);

// True when `id` is one of the disc's prompt textures (any version, Trilogy's
// Metroid1.pak included). Trilogy's loader skips RS5.pak's Wii art for these.
bool IsPromptAsset(uint32_t id);

// Binds the icon for the prompt texture `id` to the CTexture `owner` (a
// TexturePointerKey, so the texture's user data must be `owner`), following the
// binding from then on. False when `id` is no prompt or the module is off.
// Content hashes stay as a fallback for prompt art that is not bound this way.
bool BindTexture(const void* owner, uint32_t id, uint32_t width, uint32_t height);
void UnbindTexture(const void* owner);
} // namespace PortPrompts
