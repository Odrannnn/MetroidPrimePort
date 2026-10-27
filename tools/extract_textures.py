#!/usr/bin/env python3
"""Extract every TXTR from a Metroid Prime disc, named the way Aurora names them.

Aurora's texture dumps and replacements are keyed by
tex1_<w>x<h>_<xxh64 of the base level>[_<xxh64 of the used palette>]_<gx format>,
so the names written here can go straight into the prompt table in
platform/port_prompts.cpp or into textures/<device>/. Unlike MP_DUMP_TEXTURES,
this sees every texture on the disc, not only the ones a run happened to draw.

It also lists the textures the game's strings draw inline with &image= tags
(the HUD hint memos, the pause and map screens), which is where button prompts
inside text come from.

    python3 tools/extract_textures.py <disc.iso or extracted dir> <outdir> [--max-dim N] [--png]

Writes <outdir>/index.tsv (every texture, with its PAK name where it has one), <outdir>/strg_images.tsv (the
textures &image= tags use, with the text around the tag) and, with --png, a PNG
per texture (only those up to --max-dim on each side, default 128) plus
<outdir>/strg_images.png, a labelled sheet of the &image= textures.
"""

import argparse
import ctypes
import ctypes.util
import os
import re
import struct
import sys
import zlib

import numpy as np

# TXTR format -> (GX format, block width, block height, bytes per block).
TXTR_FORMATS = {
    0: (0x0, 8, 8, 32),   # I4
    1: (0x1, 8, 4, 32),   # I8
    2: (0x2, 8, 4, 32),   # IA4
    3: (0x3, 4, 4, 32),   # IA8
    4: (0x8, 8, 8, 32),   # C4
    5: (0x9, 8, 4, 32),   # C8
    6: (0xA, 4, 4, 32),   # C14X2
    7: (0x4, 4, 4, 32),   # RGB565
    8: (0x5, 4, 4, 32),   # RGB5A3
    9: (0x6, 4, 4, 64),   # RGBA8
    10: (0xE, 8, 8, 32),  # CMPR
}
GX_C4, GX_C8, GX_C14X2 = 0x8, 0x9, 0xA


def _load_xxh64():
    name = ctypes.util.find_library("xxhash") or "libxxhash.so.0"
    try:
        lib = ctypes.CDLL(name)
    except OSError:
        sys.exit("needs libxxhash (the xxhash system library)")
    lib.XXH64.restype = ctypes.c_uint64
    lib.XXH64.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64]
    return lambda data: lib.XXH64(bytes(data), len(data), 0)


xxh64 = _load_xxh64()


def base_level_size(fmt, w, h):
    _, bw, bh, bb = TXTR_FORMATS[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bb


# --- Disc and PAK -----------------------------------------------------------

def disc_files(iso):
    """(name, offset, size) for every file in a GameCube disc's FST."""
    iso.seek(0x424)
    fst_off, fst_size = struct.unpack(">II", iso.read(8))
    iso.seek(fst_off)
    fst = iso.read(fst_size)
    count = struct.unpack_from(">I", fst, 8)[0]
    names = fst[count * 12:]
    for i in range(1, count):
        word, off, size = struct.unpack_from(">III", fst, i * 12)
        if word >> 24:
            continue  # directory
        name_off = word & 0xFFFFFF
        name = names[name_off:names.index(b"\0", name_off)].decode("ascii", "replace")
        yield name, off, size


def pak_names(pak):
    """{id: name} from a Prime 1 PAK's named-resource table. The names are the
    developers' own (LStickN, AButtonIn, ...), which is how prompts are found."""
    names = {}
    pos = 12
    for _ in range(struct.unpack_from(">I", pak, 8)[0]):
        rid, name_len = struct.unpack_from(">II", pak, pos + 4)
        names.setdefault(rid, pak[pos + 12:pos + 12 + name_len].decode("ascii", "replace"))
        pos += 12 + name_len
    return names


def pak_resources(pak):
    """(type, id, data) for every resource in a Prime 1 PAK."""
    named = struct.unpack_from(">I", pak, 8)[0]
    pos = 12
    for _ in range(named):
        name_len = struct.unpack_from(">I", pak, pos + 8)[0]
        pos += 12 + name_len
    count = struct.unpack_from(">I", pak, pos)[0]
    pos += 4
    for _ in range(count):
        compressed, rtype, rid, size, off = struct.unpack_from(">I4sIII", pak, pos)
        pos += 20
        data = pak[off:off + size]
        if compressed:
            data = zlib.decompress(data[4:])
        yield rtype.decode("ascii", "replace"), rid, data


# --- TXTR -------------------------------------------------------------------

def parse_txtr(data):
    fmt, w, h, mips = struct.unpack_from(">IHHI", data, 0)
    if fmt not in TXTR_FORMATS:
        return None
    pos = 12
    palette = None
    gx = TXTR_FORMATS[fmt][0]
    if gx in (GX_C4, GX_C8, GX_C14X2):
        pal_fmt, pw, ph = struct.unpack_from(">IHH", data, pos)
        pos += 8
        entries = pw * ph
        palette = (pal_fmt, data[pos:pos + entries * 2])
        pos += entries * 2
    base = data[pos:pos + base_level_size(fmt, w, h)]
    return fmt, w, h, palette, base


def palette_indices(gx, base):
    raw = np.frombuffer(base, np.uint8)
    if gx == GX_C4:
        return np.stack([raw >> 4, raw & 15], axis=1).reshape(-1).astype(np.int32)
    if gx == GX_C8:
        return raw.astype(np.int32)
    return (np.frombuffer(base, ">u2") & 0x3FFF).astype(np.int32)


def aurora_name(fmt, w, h, palette, base):
    gx = TXTR_FORMATS[fmt][0]
    name = f"tex1_{w}x{h}_{xxh64(base):016x}"
    if palette is not None:
        # Aurora hashes only the palette entries the texture indexes
        # (compute_referenced_tlut_hash in extern/aurora/lib/gfx/texture_replacement.cpp).
        idx = palette_indices(gx, base)
        lo, hi = int(idx.min()), int(idx.max())
        tlut = palette[1][2 * lo:2 * (hi + 1)]
        name += f"_{xxh64(tlut) if len(tlut) == 2 * (hi + 1 - lo) else 0:016x}"
    return f"{name}_{gx}"


# --- Decoding, for looking at them ------------------------------------------

def untile(pixels, w, h, bw, bh):
    """Tiled pixel order (blocks of bw x bh, row-major) -> an (h, w, ...) image."""
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    rest = pixels.shape[1:]
    img = pixels.reshape((by, bx, bh, bw) + rest).swapaxes(1, 2).reshape((by * bh, bx * bw) + rest)
    return img[:h, :w]


def rgb5a3(v):
    v = v.astype(np.uint32)
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    b = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return np.stack([r, g, b, a], axis=-1).astype(np.uint8)


def rgb565(v):
    v = v.astype(np.uint32)
    r, g, b = ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
    return np.stack([r, g, b, np.full_like(r, 255)], axis=-1).astype(np.uint8)


def ia8(v):
    v = v.astype(np.uint32)
    i, a = v & 255, v >> 8
    return np.stack([i, i, i, a], axis=-1).astype(np.uint8)


def decode_cmpr(base, w, h):
    blocks = np.frombuffer(base, np.uint8).reshape(-1, 8)
    c0 = (blocks[:, 0].astype(np.uint32) << 8) | blocks[:, 1]
    c1 = (blocks[:, 2].astype(np.uint32) << 8) | blocks[:, 3]
    p0, p1 = rgb565(c0).astype(np.int32), rgb565(c1).astype(np.int32)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], axis=1)  # (n, 4, 4)
    bits = blocks[:, 4:8]
    idx = np.stack([(bits >> s) & 3 for s in (6, 4, 2, 0)], axis=2).reshape(-1, 16)
    px = np.take_along_axis(pal, idx[:, :, None].astype(np.int64), axis=1).astype(np.uint8)
    # Four 4x4 DXT1 sub-blocks per 8x8 block: untile the sub-blocks, then the blocks.
    sub = px.reshape(-1, 2, 2, 4, 4, 4).swapaxes(2, 3).reshape(-1, 64, 4)
    return untile(sub.reshape(-1, 4), w, h, 8, 8)


def decode(fmt, w, h, palette, base):
    gx, bw, bh, _ = TXTR_FORMATS[fmt]
    raw = np.frombuffer(base, np.uint8)
    if gx == 0xE:
        return decode_cmpr(base, w, h)
    if gx in (GX_C4, GX_C8, GX_C14X2):
        pal_fmt, pal = palette
        entries = np.frombuffer(pal, ">u2")
        colors = (ia8, rgb565, rgb5a3)[min(pal_fmt, 2)](entries)
        idx = np.clip(palette_indices(gx, base), 0, len(colors) - 1)
        return untile(colors[idx], w, h, bw, bh)
    if gx == 0x0:  # I4
        i = np.stack([raw >> 4, raw & 15], axis=1).reshape(-1) * 17
        px = np.stack([i, i, i, i], axis=-1).astype(np.uint8)
    elif gx == 0x1:  # I8
        px = np.stack([raw, raw, raw, raw], axis=-1)
    elif gx == 0x2:  # IA4
        i, a = (raw & 15) * 17, (raw >> 4) * 17
        px = np.stack([i, i, i, a], axis=-1).astype(np.uint8)
    elif gx == 0x3:
        px = ia8(np.frombuffer(base, ">u2"))
    elif gx == 0x4:
        px = rgb565(np.frombuffer(base, ">u2"))
    elif gx == 0x5:
        px = rgb5a3(np.frombuffer(base, ">u2"))
    elif gx == 0x6:  # RGBA8: per 4x4 block, 16 AR pairs then 16 GB pairs
        b = raw.reshape(-1, 2, 16, 2)
        px = np.stack([b[:, 0, :, 1], b[:, 1, :, 0], b[:, 1, :, 1], b[:, 0, :, 0]], axis=-1).reshape(-1, 4)
    else:
        return None
    return untile(px, w, h, bw, bh)


# --- STRG -------------------------------------------------------------------

IMAGE_TAG = re.compile(r"&image=([^;]*);")


def strg_image_uses(data):
    """(texture id, surrounding text) for every &image= tag in a STRG."""
    if len(data) < 16 or struct.unpack_from(">I", data, 0)[0] != 0x87654321:
        return
    text = data[16:].decode("utf-16-be", "replace") if len(data) % 2 == 0 else data[16:-1].decode("utf-16-be", "replace")
    for m in IMAGE_TAG.finditer(text):
        context = text[max(0, m.start() - 60):m.end() + 60]
        context = re.sub(r"&[^;]*;|[\x00-\x1f�]", " ", context)
        for tid in re.findall(r"\b[0-9A-Fa-f]{8}\b", m.group(1)):
            yield int(tid, 16), m.group(1), " ".join(context.split())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("outdir")
    ap.add_argument("--png", action="store_true", help="also write PNGs")
    ap.add_argument("--max-dim", type=int, default=128, help="largest side to write a PNG for")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    textures = {}  # id -> (pak, fmt, w, h, palette, base)
    image_uses = {}  # (id, tag) -> [(strg id, text)]
    res_names = {}  # id -> the PAK's name for it, where it has one
    if os.path.isdir(args.iso):
        # An extracted disc (a Wii disc's partition is encrypted, so Trilogy's
        # MP1 directory comes from dolphin-tool extract): its PAKs as files.
        def paks():
            for root, _, files in sorted(os.walk(args.iso)):
                for name in sorted(files):
                    if name.lower().endswith(".pak"):
                        with open(os.path.join(root, name), "rb") as f:
                            yield name, f.read()
    else:
        def paks():
            with open(args.iso, "rb") as iso:
                for name, off, size in disc_files(iso):
                    if name.lower().endswith(".pak"):
                        iso.seek(off)
                        yield name, iso.read(size)
    for name, pak in paks():
        for rid, rname in pak_names(pak).items():
            res_names.setdefault(rid, rname)
        for rtype, rid, data in pak_resources(pak):
            if rtype == "TXTR" and rid not in textures:
                parsed = parse_txtr(data)
                if parsed is not None:
                    textures[rid] = (name,) + parsed
            elif rtype == "STRG":
                for tid, tag, text in strg_image_uses(data):
                    uses = image_uses.setdefault((tid, tag), [])
                    if (rid, text) not in uses:
                        uses.append((rid, text))

    names = {}
    with open(os.path.join(args.outdir, "index.tsv"), "w") as index:
        index.write("asset_id\tpak\twidth\theight\taurora_name\tname\n")
        for rid, (pak, fmt, w, h, palette, base) in sorted(textures.items()):
            names[rid] = aurora_name(fmt, w, h, palette, base)
            index.write(f"{rid:08X}\t{pak}\t{w}\t{h}\t{names[rid]}\t{res_names.get(rid, '')}\n")

    with open(os.path.join(args.outdir, "strg_images.tsv"), "w") as out:
        out.write("asset_id\taurora_name\ttag\tstrg_id\ttext\n")
        for (tid, tag), uses in sorted(image_uses.items()):
            for sid, text in uses:
                out.write(f"{tid:08X}\t{names.get(tid, '?')}\t{tag}\t{sid:08X}\t{text}\n")

    print(f"{len(textures)} textures, {len({t for t, _ in image_uses})} used by &image= tags -> {args.outdir}")
    if not args.png:
        return

    from PIL import Image, ImageDraw

    images = {}
    for rid, (pak, fmt, w, h, palette, base) in textures.items():
        if max(w, h) > args.max_dim:
            continue
        px = decode(fmt, w, h, palette, base)
        if px is not None:
            # The game's UI draws these bottom row first, as Aurora's dumps
            # also show; flip them so they read the right way up.
            images[rid] = Image.fromarray(np.ascontiguousarray(px[::-1]), "RGBA")
            images[rid].save(os.path.join(args.outdir, f"{rid:08X}_{names[rid]}.png"))

    # A labelled sheet of the &image= textures, on grey so alpha shows.
    ids = sorted({tid for tid, _ in image_uses if tid in images})
    cell, cols = 136, 8
    sheet = Image.new("RGBA", (cols * cell, max(1, (len(ids) + cols - 1) // cols) * (cell + 14)), (90, 90, 90, 255))
    draw = ImageDraw.Draw(sheet)
    for i, tid in enumerate(ids):
        img = images[tid]
        scale = max(1, min(4, 128 // max(img.size)))
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
        x, y = (i % cols) * cell, (i // cols) * (cell + 14)
        sheet.alpha_composite(img, (x + 4, y + 4))
        draw.text((x + 4, y + cell - 2), f"{tid:08X}", fill=(255, 255, 0, 255))
    sheet.save(os.path.join(args.outdir, "strg_images.png"))


if __name__ == "__main__":
    main()
