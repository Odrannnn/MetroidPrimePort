#!/usr/bin/env python3
"""Resource inventory/diff for Metroid Prime GameCube disc images (.iso/.gcm).

    python3 tools/disc_diff.py info <disc.iso>
    python3 tools/disc_diff.py list <disc.iso> <out.tsv>
    python3 tools/disc_diff.py diff <a.tsv> <b.tsv> [--out report.txt]

The list TSV has one row per resource occurrence with columns
pak, type, id, compressed, size, sha1, where size/sha1 cover the
decompressed bytes. Non-PAK disc files get type FILE (id = disc path) and
the DOL gets a single type DOL row. diff compares by (type, id).
"""

import argparse
import hashlib
import struct
import sys
import zlib

CHUNK = 1 << 20


# --- Disc and PAK (same layout as tools/extract_textures.py) -----------------


def read_header(iso):
    iso.seek(0)
    game_id = iso.read(6).decode("ascii", "replace")
    disc_no, revision = struct.unpack("BB", iso.read(2))
    iso.seek(0x20)
    name = iso.read(64).split(b"\0", 1)[0].decode("ascii", "replace")
    iso.seek(0x420)
    dol_off = struct.unpack(">I", iso.read(4))[0]
    return game_id, disc_no, revision, name, dol_off


def disc_files(iso):
    """(path, offset, size) for every file in a GameCube disc's FST."""
    iso.seek(0x424)
    fst_off, fst_size = struct.unpack(">II", iso.read(8))
    iso.seek(fst_off)
    fst = iso.read(fst_size)
    count = struct.unpack_from(">I", fst, 8)[0]

    def name(i):
        word = struct.unpack_from(">I", fst, i * 12)[0]
        off = word & 0xFFFFFF
        return fst[count * 12 + off:fst.index(b"\0", count * 12 + off)].decode("ascii", "replace")

    out = []

    def walk(i, prefix):
        word, off, size = struct.unpack_from(">III", fst, i * 12)
        if word >> 24:  # directory: size is the first index past its subtree
            path = prefix + name(i) + "/" if i else ""
            j = i + 1
            while j < size:
                walk(j, path)
                w = struct.unpack_from(">I", fst, j * 12)[0]
                if w >> 24:
                    j = struct.unpack_from(">I", fst, j * 12 + 8)[0]
                else:
                    j += 1
        else:
            out.append((prefix + name(i), off, size))

    walk(0, "")
    return out


def pak_table(pak):
    """(compressed, type, id, size, offset) for every resource in an MP1 PAK."""
    major, minor = struct.unpack_from(">HH", pak, 0)
    if (major, minor) != (3, 5):
        raise ValueError(f"unexpected PAK version {major}.{minor}")
    pos = 12
    for _ in range(struct.unpack_from(">I", pak, 8)[0]):
        pos += 12 + struct.unpack_from(">I", pak, pos + 8)[0]
    count = struct.unpack_from(">I", pak, pos)[0]
    pos += 4
    entries = []
    for _ in range(count):
        compressed, rtype, rid, size, off = struct.unpack_from(">I4sIII", pak, pos)
        pos += 20
        entries.append((compressed, rtype.decode("ascii", "replace"), rid, size, off))
    return entries


def pak_resource_data(pak, size, off, compressed):
    data = pak[off:off + size]
    if compressed:
        data = zlib.decompress(data[4:])
    return data


def hash_range(iso, off, size):
    h = hashlib.sha1()
    iso.seek(off)
    left = size
    while left:
        chunk = iso.read(min(CHUNK, left))
        if not chunk:
            break
        h.update(chunk)
        left -= len(chunk)
    return h.hexdigest()


def dol_size(iso, dol_off):
    iso.seek(dol_off)
    hdr = iso.read(0x100)
    offs = struct.unpack_from(">18I", hdr, 0)
    sizes = struct.unpack_from(">18I", hdr, 0x90)
    return max((o + s for o, s in zip(offs, sizes) if o and s), default=0)


# --- Subcommands --------------------------------------------------------------


def cmd_info(args):
    with open(args.iso, "rb") as iso:
        game_id, disc_no, revision, name, _ = read_header(iso)
        files = disc_files(iso)
    paks = [f for f in files if f[0].lower().endswith(".pak")]
    n_res = 0
    with open(args.iso, "rb") as iso:
        for path, off, size in paks:
            iso.seek(off)
            n_res += len(pak_table(iso.read(size)))
    print(f"game id:  {game_id}")
    print(f"disc:     {disc_no}")
    print(f"revision: {revision}")
    print(f"name:     {name}")
    print(f"paks:     {len(paks)}")
    print(f"resources: {n_res}")


def cmd_list(args):
    with open(args.iso, "rb") as iso:
        _, _, _, _, dol_off = read_header(iso)
        files = disc_files(iso)
        rows = []
        for path, off, size in files:
            if not path.lower().endswith(".pak"):
                continue
            iso.seek(off)
            pak = iso.read(size)
            for compressed, rtype, rid, rsize, roff in pak_table(pak):
                data = pak_resource_data(pak, rsize, roff, compressed)
                rows.append((path, rtype, f"{rid:08X}", compressed, len(data),
                             hashlib.sha1(data).hexdigest()))
        for path, off, size in files:
            if path.lower().endswith(".pak"):
                continue
            rows.append((path, "FILE", path, 0, size, hash_range(iso, off, size)))
        dsize = dol_size(iso, dol_off)
        rows.append(("DOL", "DOL", "DOL", 0, dsize, hash_range(iso, dol_off, dsize)))
    with open(args.out, "w") as out:
        out.write("pak\ttype\tid\tcompressed\tsize\tsha1\n")
        for row in rows:
            out.write("\t".join(str(c) for c in row) + "\n")
    print(f"{len(rows)} rows -> {args.out}", file=sys.stderr)


def load_tsv(path):
    """key (type, id) -> {shas: {sha: size}, paks: set}."""
    inv = {}
    with open(path) as f:
        header = f.readline()
        assert header.rstrip("\n") == "pak\ttype\tid\tcompressed\tsize\tsha1", header
        for line in f:
            pak, rtype, rid, _, size, sha = line.rstrip("\n").split("\t")
            e = inv.setdefault((rtype, rid), {"shas": {}, "paks": set()})
            e["shas"].setdefault(sha, int(size))
            e["paks"].add(pak)
    return inv


def fmt_paks(paks):
    return ",".join(sorted(paks))


def fmt_sizes(shas):
    return ",".join(str(shas[s]) for s in sorted(shas))


def cmd_diff(args):
    a, b = load_tsv(args.a), load_tsv(args.b)
    types = sorted({t for t, _ in list(a) + list(b)})
    per_type = {}
    changed_rows, only_rows = [], []
    for t in types:
        st = {"same": 0, "changed": 0, "only_a": 0, "only_b": 0}
        for key in sorted(set(list(a) + list(b))):
            if key[0] != t:
                continue
            ea, eb = a.get(key), b.get(key)
            if ea is None:
                st["only_b"] += 1
                only_rows.append((t, key[1], "B", eb, None))
            elif eb is None:
                st["only_a"] += 1
                only_rows.append((t, key[1], "A", ea, None))
            elif ea["shas"] == eb["shas"]:
                st["same"] += 1
            else:
                st["changed"] += 1
                changed_rows.append((t, key[1], None, ea, eb))
        per_type[t] = st
    lines = ["type same changed only_a only_b"]
    for t in types:
        s = per_type[t]
        lines.append(f"{t} {s['same']} {s['changed']} {s['only_a']} {s['only_b']}")
    lines.append("")
    lines.append("changed and only-in rows (type id side paks size_a size_b):")
    last = None
    both = sorted(changed_rows + only_rows, key=lambda r: (r[0], r[1]))
    for t, rid, side, ea, eb in both:
        if t != last:
            lines.append(f"[{t}]")
            last = t
        if side == "A":
            lines.append(f"{t} {rid} only-a {fmt_paks(ea['paks'])} {fmt_sizes(ea['shas'])} -")
        elif side == "B":
            lines.append(f"{t} {rid} only-b {fmt_paks(ea['paks'])} - {fmt_sizes(ea['shas'])}")
        else:
            paks = fmt_paks(ea["paks"] | eb["paks"])
            lines.append(f"{t} {rid} changed {paks} {fmt_sizes(ea['shas'])} {fmt_sizes(eb['shas'])}")
    lines.append("")
    member = [k for k in a if k in b and a[k]["shas"] == b[k]["shas"]
              and a[k]["paks"] != b[k]["paks"]]
    lines.append(f"pak-membership differences: {len(member)}")
    for t, rid in sorted(member)[:50]:
        lines.append(f"{t} {rid} {fmt_paks(a[(t, rid)]['paks'])} -> {fmt_paks(b[(t, rid)]['paks'])}")
    for label, inv in (("a", a), ("b", b)):
        multi = sorted(k for k in inv if len(inv[k]["shas"]) > 1)
        lines.append(f"same-disc sha1 differences in {label}: {len(multi)}")
        for t, rid in multi[:50]:
            lines.append(f"{t} {rid} {fmt_paks(inv[(t, rid)]['paks'])}")
    report = "\n".join(lines) + "\n"
    print(report, end="")
    if args.out:
        with open(args.out, "w") as f:
            f.write(report)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(required=True)
    p = sub.add_parser("info")
    p.add_argument("iso")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("list")
    p.add_argument("iso")
    p.add_argument("out")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--out", default=None)
    p.set_defaults(fn=cmd_diff)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
