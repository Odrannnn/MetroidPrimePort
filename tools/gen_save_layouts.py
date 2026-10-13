#!/usr/bin/env python3
"""Emit the per-world save layout tables for one Metroid Prime disc as a C++ .inc file.

usage: python3 -I tools/gen_save_layouts.py <paks-dir> <name> > platform/save_layouts/<name>.inc

Reads every *.pak in <paks-dir> (the world paks of one disc). For each MLVL, in MLVL id
order, it records the SAVW lists (cinematics, relays, doors, scans) and the MLVL areas
in area order with their layer names. The parsing is a stdlib-only port of
build/wii-save/savw/scripts/paklib.py: the same decompression (u32 size + zlib), the same
0xFF padding rules, and the same field walk up to the MLVL layer tables.
"""
import glob
import os
import struct
import sys
import zlib

MAGIC_SAVW = 0xC001D00D


class Cur:
    def __init__(self, d):
        self.d = d
        self.o = 0

    def u16(self):
        v = struct.unpack('>H', self.d[self.o:self.o + 2])[0]
        self.o += 2
        return v

    def u32(self):
        v = struct.unpack('>I', self.d[self.o:self.o + 4])[0]
        self.o += 4
        return v

    def i32(self):
        v = struct.unpack('>i', self.d[self.o:self.o + 4])[0]
        self.o += 4
        return v

    def u64(self):
        v = struct.unpack('>Q', self.d[self.o:self.o + 8])[0]
        self.o += 8
        return v

    def skip(self, n):
        self.o += n

    def cstr(self):
        e = self.d.index(b'\x00', self.o)
        s = self.d[self.o:e]
        self.o = e + 1
        return s


def parse_pak(path):
    with open(path, 'rb') as f:
        d = f.read()
    c = Cur(d)
    c.skip(4)  # major, minor
    c.skip(4)  # zero
    for _ in range(c.u32()):  # named table: fourcc, id, name
        c.skip(4)
        c.skip(4)
        c.skip(c.u32())
    res = {}
    order = []
    for _ in range(c.u32()):  # resource table: compressed, fourcc, id, size, offset
        comp = c.u32()
        typ = d[c.o:c.o + 4].decode('ascii')
        c.skip(4)
        aid = c.u32()
        sz = c.u32()
        off = c.u32()
        res[(typ, aid)] = (off, sz, comp)
        order.append((typ, aid))
    return {'data': d, 'res': res, 'order': order}


def get_resource(pak, typ, aid):
    d = pak['data']
    off, sz, comp = pak['res'][(typ, aid)]
    raw = d[off:off + sz]
    if comp:
        decsz = struct.unpack('>I', raw[:4])[0]
        out = zlib.decompress(raw[4:])
        assert len(out) == decsz, (typ, hex(aid), len(out), decsz)
        return out
    return raw


def strip_ff_pad(raw):
    """Decompressed resources are padded with 0xFF to a 32-byte boundary. Returns the content."""
    n = len(raw)
    i = n
    while i > 0 and raw[i - 1] == 0xFF:
        i -= 1
    pad = n - i
    assert n % 32 == 0, n
    assert pad < 32, (n, pad)
    return raw[:i]


def parse_savw(raw):
    c = Cur(strip_ff_pad(raw))
    magic = c.u32()
    assert magic == MAGIC_SAVW, hex(magic)
    ver = c.u32()
    area_count = c.u32() if ver > 1 else 0
    cine = [c.u32() for _ in range(c.u32())] if ver > 2 else []
    relays = [c.u32() for _ in range(c.u32())] if ver > 2 else []
    for _ in range(c.u32()):  # layer toggles: (u32, u32), not part of the layout tables
        c.skip(8)
    doors = [c.u32() for _ in range(c.u32())]
    scans = []
    if ver > 0:
        for _ in range(c.u32()):
            scans.append((c.u32(), c.u32()))
    return {'area_count': area_count, 'cine': cine, 'relays': relays, 'doors': doors, 'scans': scans}


def parse_mlvl(raw):
    """Returns the SAVW id, the area MREA ids, and each area's layer names (wii2gci's
    area_layer_names: worldlayers[a].count names from layernames at layerindices[a])."""
    c = Cur(strip_ff_pad(raw))
    c.skip(4)  # magic
    ver = c.i32()
    c.skip(4)  # strg
    savw = c.u32() if ver >= 15 else None
    if ver >= 12:
        c.skip(4)  # sky
    if ver >= 17:
        for _ in range(c.u32()):  # mlvl relays: id, target, msg (u16), action (u8)
            c.skip(4)
            c.skip(4)
            c.skip(3)
    narea = c.i32()
    c.skip(4)
    mreas = []
    for _ in range(narea):
        c.skip(4)  # name STRG
        c.skip(48)
        c.skip(24)
        mreas.append(c.u32())
        if ver > 15:
            c.skip(4)  # area id
        na = c.u32()
        c.skip(2 * na)  # attached area ids (u16)
        n1 = c.u32()
        c.skip(8 * n1)
        n2 = c.u32()
        c.skip(8 * n2)
        if ver > 13:
            c.skip(4 * c.u32())  # layer dep offsets
        ndock = c.i32()
        for _ in range(ndock):
            nr = c.i32()
            c.skip(8 * nr)  # dock refs (u32, u32)
            nv = c.i32()
            c.skip(12 * nv)
    c.skip(4)  # mapw
    c.skip(1)
    for _ in range(c.u32()):  # world script objects: type (u8), length, bytes
        c.skip(1)
        c.skip(c.u32())
    if ver > 10:
        for _ in range(c.i32()):  # audio groups: (i32, u32)
            c.skip(8)
    if ver > 12:
        c.cstr()  # default track
    worldlayers = []
    names = []
    indices = []
    if ver > 14:
        for _ in range(c.u32()):  # per area: layer count (i32), layer bits (u64)
            worldlayers.append((c.i32(), c.u64()))
        for _ in range(c.u32()):
            names.append(c.cstr())
        indices = [c.i32() for _ in range(c.u32())]
    assert len(worldlayers) == len(mreas) == len(indices), (len(worldlayers), len(mreas), len(indices))
    layers = []
    for a, (cnt, _) in enumerate(worldlayers):
        assert cnt >= 0, cnt
        layers.append(names[indices[a]:indices[a] + cnt])
    return {'savw': savw, 'mreas': mreas, 'layers': layers, 'defaults': [bits for _, bits in worldlayers]}


def load_worlds(paks_dir):
    worlds = {}
    for path in sorted(glob.glob(os.path.join(paks_dir, '*.pak'))):
        pak = parse_pak(path)
        for typ, mid in pak['order']:
            if typ != 'MLVL':
                continue
            assert mid not in worlds, 'MLVL %08X in more than one pak' % mid
            ml = parse_mlvl(get_resource(pak, 'MLVL', mid))
            sv = parse_savw(get_resource(pak, 'SAVW', ml['savw']))
            worlds[mid] = {'mlvl': ml, 'savw': sv}
    return worlds


def hexid(v):
    return '0x%08X' % v


def c_string(b):
    out = []
    for ch in b:
        if ch == 0x22:
            out.append('\\"')
        elif ch == 0x5C:
            out.append('\\\\')
        elif ch == 0x3F:  # '?', so no trigraph can form
            out.append('\\?')
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append('\\%03o' % ch)
    return '"' + ''.join(out) + '"'


def chunked(items, per_line):
    for i in range(0, len(items), per_line):
        yield '    ' + ', '.join(items[i:i + per_line]) + ','


def emit(paks_dir, name):
    worlds = load_worlds(paks_dir)
    assert worlds, 'no MLVL in ' + paks_dir
    out = ['// Generated by tools/gen_save_layouts.py from %s world paks. Do not edit.' % name]
    entries = []
    for mid in sorted(worlds):
        ml = worlds[mid]['mlvl']
        sv = worlds[mid]['savw']
        tag = '%08X' % mid
        out.append('')
        lists = []  # (pointer, count) pairs for WorldEntry, in field order

        if sv['relays']:
            out.append('static const u32 kRelays_%s[] = {' % tag)
            out.extend(chunked([hexid(v) for v in sv['relays']], 8))
            out.append('};')
            lists.append(('kRelays_' + tag, len(sv['relays'])))
        else:
            lists.append(('nullptr', 0))

        if sv['cine']:
            out.append('static const u32 kCine_%s[] = {' % tag)
            out.extend(chunked([hexid(v) for v in sv['cine']], 8))
            out.append('};')
            lists.append(('kCine_' + tag, len(sv['cine'])))
        else:
            lists.append(('nullptr', 0))

        if sv['doors']:
            out.append('static const u32 kDoors_%s[] = {' % tag)
            out.extend(chunked([hexid(v) for v in sv['doors']], 8))
            out.append('};')
            lists.append(('kDoors_' + tag, len(sv['doors'])))
        else:
            lists.append(('nullptr', 0))

        if sv['scans']:
            out.append('static const ScanEntry kScans_%s[] = {' % tag)
            out.extend(chunked(['{%s, %d}' % (hexid(a), cat) for a, cat in sv['scans']], 4))
            out.append('};')
            lists.append(('kScans_' + tag, len(sv['scans'])))
        else:
            lists.append(('nullptr', 0))

        # Areas: firstLayerName is the offset of the area's first name in kLayerNames_<id>.
        names = []
        areas = []
        for mrea, layers, bits in zip(ml['mreas'], ml['layers'], ml['defaults']):
            areas.append((mrea, len(names), len(layers), bits))
            names.extend(layers)
        if names:
            out.append('static const char* const kLayerNames_%s[] = {' % tag)
            out.extend(chunked([c_string(n) for n in names], 4))
            out.append('};')
            layer_ptr = 'kLayerNames_' + tag
        else:
            layer_ptr = 'nullptr'

        out.append('static const AreaEntry kAreas_%s[] = {' % tag)
        out.extend(chunked(['{%s, %d, %d, 0x%XULL}' % (hexid(m), first, cnt, bits) for m, first, cnt, bits in areas], 4))
        out.append('};')

        entries.append('{%s, %d, %s, %d, %s, %d, %s, %d, %s, %d, kAreas_%s, %d, %s}' % (
            hexid(mid), sv['area_count'],
            lists[0][0], lists[0][1], lists[1][0], lists[1][1],
            lists[2][0], lists[2][1], lists[3][0], lists[3][1],
            tag, len(areas), layer_ptr))

    out.append('')
    out.append('static const WorldEntry kWorlds[] = {')
    out.extend('    ' + e + ',' for e in entries)
    out.append('};')
    return '\n'.join(out) + '\n'


def main(argv):
    if len(argv) != 3:
        sys.stderr.write('usage: gen_save_layouts.py <paks-dir> <name>\n')
        return 2
    sys.stdout.write(emit(argv[1], argv[2]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
