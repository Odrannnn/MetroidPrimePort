#!/usr/bin/env python3
"""Generate the button-prompt replacements the port registers as bindings change.

The game's prompts are individual textures: 32x32 buttons (A, B, X, Y, Start,
L, R) and 64x32 stick, C-stick, D-pad and Z art, each listed by asset id in
kAssets in platform/port_prompts.cpp (found with tools/extract_textures.py,
whose index.tsv carries the developers' names such as LStickN or DPadR).
Nothing here is keyed by the game's art: the output is one icon per input that
can be bound (every key, mouse button and pad button, stick and D-pad), written
into a single bindings/ folder. The port picks the icon for whatever the input
in use is bound to for each prompt, so the in-game prompt follows a rebind and
the device; an action with no icon of its own keeps the game's art.

Icons come from Kenney's "Input Prompts" pack, which is CC0; the ones used are
vendored in tools/prompt_icons/ (see the README there). Run with an output
directory, usually the `textures` folder beside the executable:

    tools/make_prompt_glyphs.py textures

Output is an uncompressed 32-bit RGBA DDS. DDS rather than PNG because the
game's texture is RGB5A3 and Aurora's DDS path keeps the alpha where the PNG
path does not; the vertical flip matches how Aurora reads and writes DDS (the
dumps come out flipped). The icon is inset to match the game's own art: the
32x32 buttons are opaque only from about (6,5) to (27,26), so the glyph is
22px (a full-bleed icon reads as a solid square), and the 64x32 art is a
centred 28px square.

Each icon is written at SCALE times the size of the texture it replaces (the
names keep the game's size); the port takes any whole multiple. At the game's
own size the glyph is 22px of the 64px source art, which reads blurry once
the HUD is scaled up (#229).
"""
import os
import struct
import sys
from PIL import Image

SIZE = 32
ICON_SIZE = 22
INSET = (SIZE - ICON_SIZE) // 2
SCALE = 2
ICONS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_icons")
OUT = sys.argv[1] if len(sys.argv) > 1 else "textures"

DIRECTIONS = ("up", "down", "left", "right")


# Key clusters drawn as an inverted T of the keys' own icons (up on top; left,
# down, right below). The pack's keyboard_arrows is a small unlabelled glyph
# that reads much weaker than the letter clusters, so the arrows are built too.
COMPOSITES = {
    "keyboard_arrows": ("keyboard_arrow_up", "keyboard_arrow_left",
                        "keyboard_arrow_down", "keyboard_arrow_right"),
    "keyboard_wasd": ("keyboard_w", "keyboard_a", "keyboard_s", "keyboard_d"),
    "keyboard_ijkl": ("keyboard_i", "keyboard_j", "keyboard_k", "keyboard_l"),
}

def pad_icons_for_axes(prefix):
    """The stems the port builds for sticks and the D-pad: <device>_stick_l
    or _stick_r, optionally with the way it is pushed, and <device>_dpad; the
    D-pad's directions are buttons and named like them (dpad_up...)."""
    icons = {"stick_l": f"{prefix}_stick_l.png", "stick_r": f"{prefix}_stick_r.png",
             "dpad": f"{prefix}_dpad.png"}
    for d in DIRECTIONS:
        icons[f"stick_l_{d}"] = f"{prefix}_stick_l_{d}.png"
        icons[f"stick_r_{d}"] = f"{prefix}_stick_r_{d}.png"
        icons[f"dpad_{d}"] = f"{prefix}_dpad_{d}.png"
    return icons


# Per-device icons for the pad's own buttons, keyed by the SDL gamepad button
# the port reports in the controller's mapping. The port builds the icon name as
# "<device>_<key>", so a remapped pad button follows the new button.
PAD_ICONS = {
    "xbox": {
        "south": "xbox_button_color_a.png", "east": "xbox_button_color_b.png",
        "west": "xbox_button_color_x.png", "north": "xbox_button_color_y.png",
        "start": "xbox_button_start.png", "back": "xbox_button_view.png",
        "leftshoulder": "xbox_lb.png", "rightshoulder": "xbox_rb.png",
        "leftstick": "xbox_stick_l_press.png", "rightstick": "xbox_stick_r_press.png",
        "lt": "xbox_lt.png", "rt": "xbox_rt.png",
        **pad_icons_for_axes("xbox"),
    },
    "playstation": {
        "south": "playstation_button_color_cross.png",
        "east": "playstation_button_color_circle.png",
        "west": "playstation_button_color_square.png",
        "north": "playstation_button_color_triangle.png",
        "start": "playstation3_button_start.png", "back": "playstation3_button_select.png",
        "leftshoulder": "playstation_trigger_l1.png", "rightshoulder": "playstation_trigger_r1.png",
        "leftstick": "playstation_stick_l_press.png", "rightstick": "playstation_stick_r_press.png",
        "lt": "playstation_trigger_l2.png", "rt": "playstation_trigger_r2.png",
        **pad_icons_for_axes("playstation"),
    },
    "switch": {
        "south": "switch_button_b.png", "east": "switch_button_a.png",
        "west": "switch_button_y.png", "north": "switch_button_x.png",
        "start": "switch_button_plus.png", "back": "switch_button_minus.png",
        "leftshoulder": "switch_button_l.png", "rightshoulder": "switch_button_r.png",
        "leftstick": "switch_stick_l_press.png", "rightstick": "switch_stick_r_press.png",
        "lt": "switch_button_zl.png", "rt": "switch_button_zr.png",
        **pad_icons_for_axes("switch"),
    },
    # Keyed by the GameCube button rather than the SDL one: the port names a GC
    # pad's button after the action its default mapping gives it (see
    # GameCubeStemForButton in platform/port_prompts.cpp), and only when remapped.
    "gamecube": {
        "a": "gamecube_button_color_a.png", "b": "gamecube_button_color_b.png",
        "x": "gamecube_button_x.png", "y": "gamecube_button_y.png",
        "z": "gamecube_button_z.png", "start": "gamecube_button_start.png",
        "l": "gamecube_trigger_l.png", "r": "gamecube_trigger_r.png",
    },
}
PAD_ICONS["standard"] = PAD_ICONS["xbox"]

# One icon per input that can be bound, written to <out>/bindings/<stem>.dds.
# The port registers the icon for whichever input is bound to a prompt's
# action, so a rebound key or mouse button is reflected in game. Stems match
# the tables in platform/port_prompts.cpp.
BINDING_ICONS = [
    *[f"keyboard_{c}" for c in "abcdefghijklmnopqrstuvwxyz"],
    *[f"keyboard_{d}" for d in "0123456789"],
    "keyboard_arrow_up", "keyboard_arrow_down", "keyboard_arrow_left", "keyboard_arrow_right",
    *[f"keyboard_f{i}" for i in range(1, 13)],
    "keyboard_space", "keyboard_enter", "keyboard_escape", "keyboard_tab",
    "keyboard_backspace", "keyboard_delete", "keyboard_insert", "keyboard_home",
    "keyboard_end", "keyboard_page_up", "keyboard_page_down",
    "keyboard_shift", "keyboard_ctrl", "keyboard_alt",
    "keyboard_minus", "keyboard_equals", "keyboard_bracket_open", "keyboard_bracket_close",
    "keyboard_semicolon", "keyboard_apostrophe", "keyboard_comma", "keyboard_period",
    "keyboard_slash_forward", "keyboard_slash_back", "keyboard_tilde", "keyboard_capslock",
    "keyboard_win", "keyboard_printscreen", "keyboard_scroll_lock", "keyboard_pause",
    # Keypad keys without art of their own share the main keys' (digits, minus,
    # period, slash); these are the ones that have it.
    "keyboard_numpad_enter", "keyboard_numpad_plus", "keyboard_asterisk", "keyboard_numlock",
    "mouse_left", "mouse_right", "mouse_scroll", "mouse_side_back", "mouse_side_forward",
    "keyboard_arrows", "keyboard_wasd", "keyboard_ijkl",
]


def write_dds(img, path):
    img = img.convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)
    w, h = img.size
    header = b"DDS " + struct.pack(
        "<7I44x",
        124,                        # dwSize
        0x1 | 0x2 | 0x4 | 0x1000,   # CAPS|HEIGHT|WIDTH|PIXELFORMAT
        h, w,
        0,                          # pitch (unused)
        0,                          # depth
        1,                          # mip count
    )
    pf = struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)  # CAPS, CAPS2-4, reserved2
    with open(path, "wb") as f:
        f.write(header + pf + caps + img.tobytes())


def load_icon(name):
    return Image.open(os.path.join(ICONS_DIR, name)).convert("RGBA")


def make_cluster(stems):
    """An inverted T of key icons (up, left, down, right), cropped to the keys."""
    keys = [load_icon(s + ".png") for s in stems]
    keys = [k.crop(k.getbbox()) for k in keys]
    kw, kh = keys[0].size
    gap = max(1, kw // 16)
    img = Image.new("RGBA", (3 * kw + 2 * gap, 2 * kh + gap), (0, 0, 0, 0))
    up, left, down, right = keys
    img.alpha_composite(up, (kw + gap, 0))
    for i, key in enumerate((left, down, right)):
        img.alpha_composite(key, (i * (kw + gap), kh + gap))
    return img


def make_icon(name, width=SIZE, height=SIZE, scale=SCALE):
    """Scale the icon to the game's inset for a texture of this size: 22px in
    the 32x32 buttons, 28px in the 64x32 stick and D-pad art (which the game
    draws as a centred 28px square). The image is scale times that size."""
    if width == height:
        size = max(1, int(round(ICON_SIZE * width / SIZE)))
    else:
        size = max(1, min(width, height) - 4)
    width, height, size = width * scale, height * scale, size * scale
    tile = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    stem = name[:-4] if name.endswith(".png") else name
    if stem in COMPOSITES:
        # Four keys at the height of one: as tall as the inset, and wider, but
        # inside what the game shows of a 64x32 stick texture. The map draws it
        # with "SI,0.6,1.0": only the middle 60% of the width, starting right
        # where the quad does and with the text right after it. Wider, the
        # cluster's outer keys are cut and touch the text; 4px clear each side.
        icon = make_cluster(COMPOSITES[stem])
        shown = width if width == height else int(width * 0.6) - 8 * scale
        fit = min(shown / icon.width, size / icon.height)
        icon = icon.resize((max(1, round(icon.width * fit)), max(1, round(icon.height * fit))),
                           Image.LANCZOS)
        tile.alpha_composite(icon, ((width - icon.width) // 2, (height - icon.height) // 2))
        return tile
    icon = load_icon(name).resize((size, size), Image.LANCZOS)
    tile.alpha_composite(icon, ((width - size) // 2, (height - size) // 2))
    return tile


def main():
    bindings = os.path.join(OUT, "bindings")
    os.makedirs(bindings, exist_ok=True)
    padCount = 0
    # One binding stem can serve textures of more than one size - the C-stick
    # prompts here are one 32x32 and one 64x32 - so each binding is also written
    # with its size in the name. platform/port_prompts.cpp looks for the exact
    # size first and falls back to the unsuffixed 32x32 file. Without this a
    # 64x32 slot gets 32x32 bytes: the game does not scale the image, it reads
    # 64x32 of pixels out of half the data and draws the rest as noise.
    sizes = [(32, 32), (64, 32)]
    for device, icons in PAD_ICONS.items():
        for key, icon in icons.items():
            for w, h in sizes:
                suffix = "" if (w, h) == (SIZE, SIZE) else f"_{w}x{h}"
                write_dds(make_icon(icon, w, h),
                          os.path.join(bindings, f"{device}_{key}{suffix}.dds"))
                padCount += 1
    missing = []
    for stem in BINDING_ICONS:
        src = os.path.join(ICONS_DIR, stem + ".png")
        if stem not in COMPOSITES and not os.path.exists(src):
            missing.append(stem)
            continue
        # At both sizes, not just 32x32. The key stems serve the same prompt
        # textures as the pad stems, so the 64x32 ones need 64x32 key art;
        # writing only 32x32 left the port refusing the file on size, so a
        # rebound Z still read "F" on the map, or nothing replaced the art.
        for w, h in sizes:
            suffix = "" if (w, h) == (SIZE, SIZE) else f"_{w}x{h}"
            write_dds(make_icon(stem + ".png", w, h),
                      os.path.join(bindings, f"{stem}{suffix}.dds"))
    print(f"bindings: {len(BINDING_ICONS) - len(missing)} key icons, {padCount} pad icons -> {bindings}")
    if missing:
        print("  missing from tools/prompt_icons: " + ", ".join(missing))


if __name__ == "__main__":
    main()
