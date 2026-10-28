#!/usr/bin/env python3
"""Generate per-device button-prompt replacements for Metroid Prime.

The game's prompts are individual textures: 32x32 buttons (A, B, X, Y, Start,
L, R) and 64x32 stick, C-stick, D-pad and Z art, each listed with its hash in
kKeys in platform/port_prompts.cpp (found with tools/extract_textures.py, whose
index.tsv carries the developers' names such as LStickN or DPadR). Each device
gets its own icon for every action, so the in-game prompt matches the pad (or
keyboard) in use. The keyboard icons label the keys the port binds by default
(A action is X, B action is Z, the sticks WASD and IJKL, the D-pad the arrows).
The bindings/ set holds one icon per key and pad input, which the port swaps
in when an action is rebound.

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
"""
import os
import re
import struct
import sys
from PIL import Image

SIZE = 32
ICON_SIZE = 22
INSET = (SIZE - ICON_SIZE) // 2
ICONS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_icons")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_PROMPTS = os.path.join(ROOT, "platform", "port_prompts.cpp")
OUT = sys.argv[1] if len(sys.argv) > 1 else "textures"

# Icon per device and action. The keyboard set is the fallback used when a
# binding has no icon of its own, so it labels the port's default keys.
#
# Z is the right shoulder in every default pad mapping Aurora ships, so the pad
# sets give it the bumper. There is no "gamecube" set on purpose: the game's own
# art already is the GameCube set.
#
# Y is the pause screen's Zoom. The Switch labels are positional, as in
# PAD_ICONS: SDL3 names buttons by position, so A is the bottom face button,
# which a Switch pad labels B.
DIRECTIONS = ("up", "down", "left", "right")


def pad_sticks_and_dpad(prefix):
    """The control stick (left), the C-stick (right) and the D-pad, whole and
    per direction, for a pad whose Kenney icons start with `prefix`."""
    icons = {"stick": f"{prefix}_stick_l.png", "cstick": f"{prefix}_stick_r.png",
             "dpad": f"{prefix}_dpad.png"}
    for d in DIRECTIONS:
        icons[f"stick_{d}"] = f"{prefix}_stick_l_{d}.png"
        icons[f"cstick_{d}"] = f"{prefix}_stick_r_{d}.png"
        icons[f"dpad_{d}"] = f"{prefix}_dpad_{d}.png"
    return icons


DEVICE_ICONS = {
    "xbox": {"a": "xbox_button_color_a.png", "b": "xbox_button_color_b.png",
             "x": "xbox_button_color_x.png", "y": "xbox_button_color_y.png",
             "start": "xbox_button_start.png",
             "l": "xbox_lt.png", "r": "xbox_rt.png", "z": "xbox_rb.png",
             **pad_sticks_and_dpad("xbox")},
    "playstation": {"a": "playstation_button_color_cross.png",
                    "b": "playstation_button_color_circle.png",
                    "x": "playstation_button_color_square.png",
                    "y": "playstation_button_color_triangle.png",
                    "start": "playstation3_button_start.png",
                    "l": "playstation_trigger_l2.png", "r": "playstation_trigger_r2.png",
                    "z": "playstation_trigger_r1.png",
                    **pad_sticks_and_dpad("playstation")},
    # Nintendo pads map by label (pad.cpp's Pro Controller default), so A is A.
    "switch": {"a": "switch_button_a.png", "b": "switch_button_b.png",
               "x": "switch_button_x.png", "y": "switch_button_y.png",
               "start": "switch_button_plus.png",
               "l": "switch_button_zl.png", "r": "switch_button_zr.png",
               "z": "switch_button_r.png",
               **pad_sticks_and_dpad("switch")},
    # The sticks and the D-pad on their default keys: WASD, IJKL and the arrows.
    "keyboard": {"a": "keyboard_x.png", "b": "keyboard_z.png", "x": "keyboard_c.png",
                 "y": "keyboard_v.png", "start": "keyboard_enter.png",
                 "l": "keyboard_q.png", "r": "keyboard_e.png", "z": "keyboard_f.png",
                 "stick": "keyboard_wasd", "cstick": "keyboard_ijkl", "dpad": "keyboard_arrows",
                 **{f"stick_{d}": f"keyboard_{k}.png" for d, k in zip(DIRECTIONS, "wsad")},
                 **{f"cstick_{d}": f"keyboard_{k}.png" for d, k in zip(DIRECTIONS, "ikjl")},
                 **{f"dpad_{d}": f"keyboard_arrow_{d}.png" for d in DIRECTIONS}},
}

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

# PAD_BUTTON_* / PAD_TRIGGER_* / PROMPT_* to the action name used above.
ACTION_FOR_BUTTON = {
    "PAD_BUTTON_A": "a",
    "PAD_BUTTON_B": "b",
    "PAD_BUTTON_X": "x",
    "PAD_BUTTON_Y": "y",
    "PAD_BUTTON_START": "start",
    "PAD_TRIGGER_L": "l",
    "PAD_TRIGGER_R": "r",
    "PAD_TRIGGER_Z": "z",
    "PROMPT_STICK": "stick",
    "PROMPT_CSTICK": "cstick",
    "PROMPT_DPAD": "dpad",
    **{f"PROMPT_STICK_{d.upper()}": f"stick_{d}" for d in DIRECTIONS},
    **{f"PROMPT_CSTICK_{d.upper()}": f"cstick_{d}" for d in DIRECTIONS},
    **{f"PAD_BUTTON_{d.upper()}": f"dpad_{d}" for d in DIRECTIONS},
}


def read_prompt_keys():
    """Parse the prompt texture table out of platform/port_prompts.cpp.

    Keeping the table in one place means a texture only has to be identified
    once, in the C++ where it is used.
    """
    pattern = re.compile(
        r"\{\s*((?:PAD|PROMPT)_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*0x([0-9a-fA-F]+)ull\s*,\s*\"(\w+)\"\s*\}")
    with open(PORT_PROMPTS) as f:
        keys = pattern.findall(f.read())
    if not keys:
        raise SystemExit(f"no prompt keys found in {PORT_PROMPTS}")
    missing = sorted({k[0] for k in keys} - set(ACTION_FOR_BUTTON))
    if missing:
        raise SystemExit(f"no icon mapping for: {', '.join(missing)}")
    return [(ACTION_FOR_BUTTON[button], int(w), int(h), hsh, fmt) for button, w, h, hsh, fmt in keys]


def texture_name(w, h, hsh, fmt):
    return f"tex1_{w}x{h}_{hsh}_{fmt}.dds"

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


def make_icon(name, width=SIZE, height=SIZE):
    """Scale the icon to the game's inset for a texture of this size: 22px in
    the 32x32 buttons, 28px in the 64x32 stick and D-pad art (which the game
    draws as a centred 28px square)."""
    if width == height:
        size = max(1, int(round(ICON_SIZE * width / SIZE)))
    else:
        size = max(1, min(width, height) - 4)
    tile = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    stem = name[:-4] if name.endswith(".png") else name
    if stem in COMPOSITES:
        # Four keys at the height of one: as tall as the inset, and wider.
        icon = make_cluster(COMPOSITES[stem])
        fit = min(width / icon.width, size / icon.height)
        icon = icon.resize((max(1, round(icon.width * fit)), max(1, round(icon.height * fit))),
                           Image.LANCZOS)
        tile.alpha_composite(icon, ((width - icon.width) // 2, (height - icon.height) // 2))
        return tile
    icon = load_icon(name).resize((size, size), Image.LANCZOS)
    tile.alpha_composite(icon, ((width - size) // 2, (height - size) // 2))
    return tile


def main():
    keys = read_prompt_keys()
    for device, icons in DEVICE_ICONS.items():
        d = os.path.join(OUT, device)
        os.makedirs(d, exist_ok=True)
        for action, w, h, hsh, fmt in keys:
            if action not in icons:
                continue  # this device has no icon for that action
            write_dds(make_icon(icons[action], w, h), os.path.join(d, texture_name(w, h, hsh, fmt)))
        print(f"{device}: {len(keys)} textures -> {d}")

    bindings = os.path.join(OUT, "bindings")
    os.makedirs(bindings, exist_ok=True)
    padCount = 0
    # One binding stem can serve textures of more than one size - the C-stick
    # prompts here are one 32x32 and one 64x32 - so each binding is also written
    # with its size in the name. platform/port_prompts.cpp looks for the exact
    # size first and falls back to the unsuffixed 32x32 file. Without this a
    # 64x32 slot gets 32x32 bytes: the game does not scale the image, it reads
    # 64x32 of pixels out of half the data and draws the rest as noise.
    sizes = sorted({(w, h) for _, w, h, _, _ in keys})
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
        # At every size in the table, not just 32x32. The key stems serve the
        # same actions as the pad stems, so the two 64x32 rows need 64x32 key
        # art; writing only 32x32 made Apply fall back to it, refuse it on size,
        # and leave the key showing the static set for the default bindings -
        # so a rebound Z still read "F" on the map.
        for w, h in sizes:
            suffix = "" if (w, h) == (SIZE, SIZE) else f"_{w}x{h}"
            write_dds(make_icon(stem + ".png", w, h),
                      os.path.join(bindings, f"{stem}{suffix}.dds"))
    print(f"bindings: {len(BINDING_ICONS) - len(missing)} key icons, {padCount} pad icons -> {bindings}")
    if missing:
        print("  missing from tools/prompt_icons: " + ", ".join(missing))


if __name__ == "__main__":
    main()
