#!/usr/bin/env python3
"""Generate platform/port_skip_cutscenes_data.inc from randomprime's
"skippable cutscenes" room patches.

randomprime (https://github.com/randovania/randomprime, MIT) makes every
cutscene skippable by editing the room scripts (SCLY) of 80 rooms. Instead of
reimplementing its patcher, this diffs two ISOs it produced from the same disc
(--qol-cutscenes original vs skippable) and records the difference as a small
op stream per room. The ops only carry randomprime's own objects and edits;
objects it copies from the disc (Flaahgra's clones) are CLONE ops.

    randomprime_patcher --quiet --force-vanilla-layout --input-iso <disc> \\
        --output-iso original.iso --qol-cutscenes original
    (same with --output-iso skippable.iso --qol-cutscenes skippable)
    tools/gen_skippable_cutscenes.py <disc> original.iso skippable.iso \\
        <randomprime>/generated/json_data/skippable_cutscenes.jsonc \\
        platform/port_skip_cutscenes_data.inc

It also emits kLandingOps, randomprime's Landing Site intro skip, which the
port applies on top in Archipelago games.

PAL (GM8P01): randomprime merges extra, PAL-specific edits and PAL lays some
connections out differently. Give the script the PAL disc and its two ISOs
as three more arguments and it appends kSkipRoomsPal/kSkipOpsPal, the PAL
streams for the rooms where the USA stream misses, differs or is absent
(everything else uses the USA one):

    randomprime_patcher ... --input-iso <pal disc> --output-iso pal-original.iso ...
    tools/gen_skippable_cutscenes.py <usa disc> original.iso skippable.iso \\
        <jsonc> platform/port_skip_cutscenes_data.inc \\
        <pal disc> pal-original.iso pal-skippable.iso

The patched PAL files outgrow a disc (about 30 MB past GC_DISC_LENGTH, 1_459_978_240, which PAL's
packing fills exactly): build randomprime with `GC_DISC_LENGTH` in
structs/src/gc_disc.rs raised to 1_600_000_000. The output is only read back
here, so the oversized ISO doesn't matter. Passing only the USA arguments
drops the PAL tables.

The script replays the ops on randomprime's original SCLYs (must match the
skippable ones byte for byte) and on the retail disc (every op must find its
target, apart from the objects randomprime itself added in other patches).
The op format is described in platform/port_skip_cutscenes.cpp.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_textures import disc_files, pak_resources  # noqa: E402

OP_CLONE, OP_EDIT, OP_REMCONN, OP_ADDCONN, OP_PUSH, OP_DELETE, OP_MOVE = 1, 2, 3, 4, 5, 6, 7


def load_sclys(iso_path):
    out = {}
    with open(iso_path, 'rb') as iso:
        for name, off, size in list(disc_files(iso)):
            if not name.lower().endswith('.pak'):
                continue
            iso.seek(off)
            pak = iso.read(size)
            for t, rid, data in pak_resources(pak):
                if t == 'MREA':
                    out[rid] = scly_of(data)
    return out


def scly_of(mrea):
    secs = struct.unpack_from('>I', mrea, 0x3C)[0]
    scly = struct.unpack_from('>I', mrea, 0x44)[0]
    sizes = struct.unpack_from('>%dI' % secs, mrea, 0x60)
    pos = (0x60 + secs * 4 + 31) & ~31
    for i in range(scly):
        pos += sizes[i]
    return mrea[pos:pos + sizes[scly]]


def parse_scly(s):
    assert s[:4] == b'SCLY', s[:4]
    n = struct.unpack_from('>I', s, 8)[0]
    lsz = struct.unpack_from('>%dI' % n, s, 12)
    pos = 12 + 4 * n
    layers = []
    for L in range(n):
        p = pos
        unk = s[p]
        cnt = struct.unpack_from('>I', s, p + 1)[0]
        p += 5
        objs = []
        for _ in range(cnt):
            t, ln, oid = struct.unpack_from('>BII', s, p)
            end = p + 5 + ln
            nc = struct.unpack_from('>I', s, p + 9)[0]
            conns = [struct.unpack_from('>III', s, p + 13 + 12 * k) for k in range(nc)]
            objs.append(dict(type=t, id=oid, conns=conns, props=s[p + 13 + 12 * nc:end]))
            p = end
        layers.append((unk, objs))
        pos += lsz[L]
    return s[4:8], layers


def write_scly(version, layers):
    blobs = []
    for unk, objs in layers:
        b = bytearray([unk]) + struct.pack('>I', len(objs))
        for o in objs:
            body = struct.pack('>II', o['id'], len(o['conns']))
            body += b''.join(struct.pack('>III', *c) for c in o['conns']) + o['props']
            b += struct.pack('>BI', o['type'], len(body)) + body
        b += bytes(-len(b) % 32)  # randomprime pads layers to 32 bytes
        blobs.append(bytes(b))
    head = b'SCLY' + version + struct.pack('>I', len(layers))
    head += b''.join(struct.pack('>I', len(b)) for b in blobs)
    return head + b''.join(blobs)


def split_name(props):
    """props = u32 property count, NUL-terminated name, rest."""
    end = props.index(b'\0', 4) + 1
    return props[:4], props[4:end], props[end:]


def diff_room(orig, skip, disc):
    """Ops turning SCLY `orig` into `skip`. Only objects that are on the
    disc (`disc` SCLY) count as clone sources."""
    _, L0 = parse_scly(orig)
    _, L1 = parse_scly(skip)
    assert len(L0) == len(L1)
    before = {o['id']: o for _, objs in L0 for o in objs}
    after = {o['id']: (li, o) for li, (_, objs) in enumerate(L1) for o in objs}
    on_disc = {o['id']: o for _, objs in parse_scly(disc)[1] for o in objs}
    by_props = {}
    for o in before.values():
        if o['id'] not in on_disc or on_disc[o['id']]['props'] != o['props']:
            continue
        by_props.setdefault((o['type'], o['props']), o['id'])

    edits, rems, adds, pushes, deletes = [], [], [], [], []
    start_conns = {oid: o['conns'] for oid, o in before.items()}
    for li, (_, objs) in enumerate(L1):
        # Objects randomprime left in place form a prefix in the original
        # order; everything after it was appended (new objects, or existing
        # ones it removed and pushed again).
        order = {o['id']: i for i, o in enumerate(L0[li][1])}
        k, last = 0, -1
        while k < len(objs) and order.get(objs[k]['id'], -1) > last:
            last = order[objs[k]['id']]
            k += 1
        for o in objs[k:]:
            if o['id'] in before:
                pushes.append((OP_MOVE, o['id'], None, struct.pack('>BBI', OP_MOVE, li, o['id'])))
                continue
            src = by_props.get((o['type'], o['props']))
            if src is not None:
                pushes.append((OP_CLONE, o['id'], None, struct.pack('>BBII', OP_CLONE, li, src, o['id'])))
                start_conns[o['id']] = before[src]['conns']
            else:
                blob = struct.pack('>BBBIH', OP_PUSH, li, o['type'], o['id'], len(o['conns']))
                blob += b''.join(struct.pack('>III', *c) for c in o['conns'])
                blob += struct.pack('>I', len(o['props'])) + o['props']
                pushes.append((OP_PUSH, o['id'], o['conns'], blob))
    for oid, (li, o) in after.items():
        if oid in before:
            a = before[oid]
            assert a['type'] == o['type'], hex(oid)
            if a['props'] != o['props']:
                c0, n0, t0 = split_name(a['props'])
                c1, n1, t1 = split_name(o['props'])
                assert c0 == c1 and len(t0) == len(t1), hex(oid)
                patches = []
                i = 0
                while i < len(t1):
                    if t0[i] == t1[i]:
                        i += 1
                        continue
                    j = i
                    while j < len(t1) and t0[j] != t1[j]:
                        j += 1
                    patches.append((i, t1[i:j]))
                    i = j
                blob = struct.pack('>BI', OP_EDIT, oid)
                name = n1 if n1 != n0 else b''
                blob += struct.pack('>H', len(name)) + name + struct.pack('>H', len(patches))
                for off, data in patches:
                    blob += struct.pack('>HH', off, len(data)) + data
                edits.append((OP_EDIT, oid, None, blob))
        if oid not in start_conns:
            continue
        cur = list(start_conns[oid])
        want = o['conns']
        for c in list(cur):
            if cur.count(c) > want.count(c):
                cur.remove(c)
                rems.append((OP_REMCONN, oid, c[2], struct.pack('>BIIII', OP_REMCONN, oid, *c)))
        assert want[:len(cur)] == cur, hex(oid)
        for c in want[len(cur):]:
            adds.append((OP_ADDCONN, oid, c[2], struct.pack('>BIIII', OP_ADDCONN, oid, *c)))
    for oid in before:
        if oid not in after:
            deletes.append((OP_DELETE, oid, None, struct.pack('>BI', OP_DELETE, oid)))
    return pushes + edits + rems + adds + deletes


def apply_ops(scly, ops):
    """Python twin of PortSkipCutscenes::Apply. Returns (scly, misses)."""
    version, layers = parse_scly(scly)
    misses = []

    def find(oid):
        for _, objs in layers:
            for o in objs:
                if o['id'] == oid:
                    return o
        return None

    p = 0
    while p < len(ops):
        op = ops[p]
        if op == OP_CLONE:
            li, src, oid = struct.unpack_from('>BII', ops, p + 1)
            p += 10
            s = find(src)
            if s is None or li >= len(layers):
                misses.append(('clone', src))
                continue
            layers[li][1].append(dict(type=s['type'], id=oid, conns=list(s['conns']), props=s['props']))
        elif op == OP_EDIT:
            oid, nlen = struct.unpack_from('>IH', ops, p + 1)
            p += 7
            name = ops[p:p + nlen]
            p += nlen
            count = struct.unpack_from('>H', ops, p)[0]
            p += 2
            patches = []
            for _ in range(count):
                off, ln = struct.unpack_from('>HH', ops, p)
                patches.append((off, ops[p + 4:p + 4 + ln]))
                p += 4 + ln
            o = find(oid)
            if o is None:
                misses.append(('edit', oid))
                continue
            c, n, t = split_name(o['props'])
            t = bytearray(t)
            if any(off + len(d) > len(t) for off, d in patches):
                misses.append(('edit-size', oid))
                continue
            for off, d in patches:
                t[off:off + len(d)] = d
            o['props'] = c + (name or n) + bytes(t)
        elif op in (OP_REMCONN, OP_ADDCONN):
            oid, st, msg, tgt = struct.unpack_from('>IIII', ops, p + 1)
            p += 17
            o = find(oid)
            if o is None:
                misses.append(('conn', oid))
                continue
            if op == OP_ADDCONN:
                o['conns'].append((st, msg, tgt))
            elif (st, msg, tgt) in o['conns']:
                o['conns'].remove((st, msg, tgt))
            else:
                misses.append(('remconn', oid))
        elif op == OP_PUSH:
            li, t, oid, nc = struct.unpack_from('>BBIH', ops, p + 1)
            p += 9
            conns = [struct.unpack_from('>III', ops, p + 12 * k) for k in range(nc)]
            p += 12 * nc
            ln = struct.unpack_from('>I', ops, p)[0]
            props = ops[p + 4:p + 4 + ln]
            p += 4 + ln
            if li >= len(layers):
                misses.append(('push', oid))
                continue
            layers[li][1].append(dict(type=t, id=oid, conns=conns, props=props))
        elif op == OP_MOVE:
            li, oid = struct.unpack_from('>BI', ops, p + 1)
            p += 6
            o = find(oid)
            if o is None or li >= len(layers):
                misses.append(('move', oid))
                continue
            for _, objs in layers:
                objs[:] = [x for x in objs if x is not o]
            layers[li][1].append(o)
        elif op == OP_DELETE:
            oid = struct.unpack_from('>I', ops, p + 1)[0]
            p += 5
            for _, objs in layers:
                objs[:] = [o for o in objs if o['id'] != oid]
        else:
            raise ValueError('bad op %d' % op)
    return write_scly(version, layers), misses


def strip_jsonc(text):
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == '\\':
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif text.startswith('//', i):
            while i < n and text[i] != '\n':
                i += 1
            continue
        elif text.startswith('/*', i):
            i = text.index('*/', i) + 2
            continue
        else:
            out.append(c)
        i += 1
    return re.sub(r',(\s*[}\]])', r'\1', ''.join(out))


def cutscene_ids(jsonc_path):
    """Object ids (24-bit) the cutscene patches create or refer to."""
    data = json.loads(strip_jsonc(open(jsonc_path).read()))
    ids = set()

    def walk(v):
        if isinstance(v, dict):
            for k, x in v.items():
                if k in ('id', 'id1', 'id2', 'senderId', 'targetId') and isinstance(x, int):
                    ids.add(x & 0xFFFFFF)
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)
                if isinstance(x, int):
                    ids.add(x & 0xFFFFFF)  # deleteIds
    walk(data)
    # objects randomprime's room hacks add (Sunchamber Flaahgra clones and
    # their follow-locators, Ruined Courtyard water, Artifact Temple pillar)
    ids |= {0x500000 + i for i in range(3)} | {0x600000 + i for i in range(3)}
    ids |= {0x700000 + i for i in range(3)} | {0x0F28C1, 0x10014F}
    return ids


def select_ops(r, ops, disc, cut):
    """Drop what the diff picked up from randomprime's always-on patches:
    their objects get different ids in the two ISOs (post-pickup relays,
    artifact layer switches), so they show up as deletes, pushes and
    connection changes. Keep ops on disc objects and cutscene objects."""
    on_disc = {o['id'] for _, objs in parse_scly(disc)[1] for o in objs}
    masked = {x & 0xFFFFFF for x in on_disc} | cut

    # 24-bit match: randomprime's Sunchamber hack targets 0x00252ACC, the
    # disc waypoint is 0x04252ACC (a dead link there too; kept as is)
    def known(x):
        return (x & 0xFFFFFF) in masked
    kept = []
    for kind, oid, extra, blob in ops:
        if kind in (OP_PUSH, OP_CLONE):
            keep = (oid & 0xFFFFFF) in cut
            if keep and kind == OP_PUSH:
                for c in extra:
                    if not known(c[2]):
                        raise SystemExit('%08X: new object %08X targets %08X, not on the disc'
                                         % (r, oid, c[2]))
        elif kind in (OP_MOVE, OP_EDIT, OP_DELETE):
            keep = oid in on_disc
        else:
            keep = known(oid) and known(extra)
        if keep:
            kept.append(blob)
    return b''.join(kept), known


def check_result(r, disc, orig, skip, ops, known):
    """The patched disc SCLY must hold skip's version of every disc and
    cutscene object that randomprime's always-on patches left alone."""
    result, misses = apply_ops(disc, ops)
    if misses:
        raise SystemExit('%08X: ops miss %s' % (r, misses))
    objs = lambda s: {o['id']: (li, o) for li, (_, L) in enumerate(parse_scly(s)[1]) for o in L}
    R, D, O, S = objs(result), objs(disc), objs(orig), objs(skip)
    conns = lambda o: [c for c in o['conns'] if known(c[2])]
    for oid, (li, o) in S.items():
        if not known(oid):
            continue
        if oid not in R:
            raise SystemExit('%08X: %08X missing' % (r, oid))
        if R[oid][0] != li:
            raise SystemExit('%08X: %08X on layer %d, not %d' % (r, oid, R[oid][0], li))
        base_same = oid not in D or (D[oid][1]['props'] == O[oid][1]['props'])
        if base_same and R[oid][1]['props'] != o['props']:
            raise SystemExit('%08X: %08X props differ' % (r, oid))
        base_same = oid not in D or conns(D[oid][1]) == conns(O[oid][1])
        if base_same and conns(R[oid][1]) != conns(o):
            raise SystemExit('%08X: %08X connections differ' % (r, oid))
    for oid in R:
        if oid not in S:
            raise SystemExit('%08X: %08X should be gone' % (r, oid))
    return result


LANDING_SITE = 0xB2701146
# Offsets of the `active` byte after the object name (randomprime structs).
ACTIVE_AT = {0x00: 341, 0x04: 60, 0x07: 47, 0x08: 205, 0x15: 0}


def landing_ops(scly):
    """Ops for randomprime's patch_landing_site_cutscene_triggers, applied
    on top of the skippable patch: the new-game intro becomes the short
    'back from load' sequence (used for games that start elsewhere or skip
    the intro, here Archipelago)."""
    version, layers = parse_scly(scly)
    objs = layers[0][1]
    find = {o['id'] & 0xFFFFFF: o for o in objs}

    def set_active(oid, v):
        o = find[oid]
        head, name, rest = split_name(o['props'])
        at = ACTIVE_AT[o['type']]
        o['props'] = head + name + rest[:at] + bytes([v]) + rest[at + 1:]

    top = max(o['id'] & 0xFFFF for _, ob in layers for o in ob)
    area = objs[0]['id'] & 0x03FF0000
    timer1, timer2 = area | (top + 1), area | (top + 2)

    def timer(oid, t, start, conn):
        props = struct.pack('>I', 6) + b'my_timer\0' + struct.pack('>ffBBB', t, 0.0, 0, start, 1)
        return dict(type=0x05, id=oid, conns=[conn], props=props)

    set_active(0xDD, 0)    # Trigger Start Overworld Cinematic
    set_active(0x1F4, 1)   # Relay Player Model Loaded
    find[0x1CE]['conns'] = []  # PlayerActor-B_rready_samus
    back = find[0x1F2]     # Trigger -- Back from Load
    set_active(0x1F2, 1)
    back['conns'] = [c for c in back['conns'] if c[2] != find[0x1F4]['id']]
    back['conns'] += [(3, 11, timer1), (3, 1, find[0x1CE]['id'])]  # Entered: ResetAndStart, Activate
    objs.append(timer(timer1, 0.6, 0, (9, 13, find[0x1F4]['id'])))  # Zero: SetToZero
    objs.append(timer(timer2, 0.02, 1, (9, 4, back['id'])))         # Zero: Deactivate
    set_active(0x1CF, 1)   # Actor Save Station Beam
    set_active(0x1E4, 1)   # Effect_BaseLights
    set_active(0x141, 1)   # Platform Samus Ship
    after = write_scly(version, layers)
    ops = diff_room(scly, after, scly)
    blob = b''.join(op[3] for op in ops)
    replay, misses = apply_ops(scly, blob)
    assert replay == after and not misses, 'landing replay mismatch'
    return blob


def build_table(d, o, s, cut):
    """[(mrea, ops)] for one disc version: randomprime's skippable patch
    replayed on its own original (must equal its skippable SCLY), checked
    against the retail disc `d`."""
    table = []
    for r in sorted(k for k in s if o[k] != s[k]):
        ops = diff_room(o[r], s[r], d[r])
        replay, misses = apply_ops(o[r], b''.join(op[3] for op in ops))
        assert replay == s[r], 'replay mismatch in %08X' % r
        assert not misses, (hex(r), misses)
        kept, known = select_ops(r, ops, d[r], cut)
        check_result(r, d[r], o[r], s[r], kept, known)
        if kept:
            table.append((r, kept))
    return table


def pal_table(usa_table, pal_disc, pal_orig, pal_skip, cut):
    """Streams for the rooms where the USA one doesn't do on the PAL disc
    what randomprime's PAL patch does (misses, other result, or no USA
    stream). Returns (table, report lines)."""
    table, lines = [], []
    usa = dict(usa_table)
    for r, ops in build_table(pal_disc, pal_orig, pal_skip, cut):
        if r in usa:
            result, misses = apply_ops(pal_disc[r], usa[r])
            want = apply_ops(pal_disc[r], ops)[0]
            if not misses and result == want:
                continue
            lines.append('%08X: USA stream %s' % (r, 'misses %d' % len(misses) if misses
                                                  else 'applies but differs'))
        else:
            lines.append('%08X: no USA stream' % r)
        table.append((r, ops))
    return table, lines


def write_tables(f, rooms_name, ops_name, table):
    blob = bytearray()
    f.write('static const SkipRoom %s[] = {\n' % rooms_name)
    for r, ops in table:
        f.write('    {0x%08X, %d, %d},\n' % (r, len(blob), len(ops)))
        blob += ops
    f.write('};\n\nstatic const unsigned char %s[] = {\n' % ops_name)
    for i in range(0, len(blob), 24):
        f.write('    ' + ','.join('0x%02X' % b for b in blob[i:i + 24]) + ',\n')
    f.write('};\n')
    return len(blob)


def main():
    disc, orig_iso, skip_iso, jsonc, out = sys.argv[1:6]
    pal = sys.argv[6:9]  # optional: PAL disc, its original and skippable ISOs
    d, o, s = load_sclys(disc), load_sclys(orig_iso), load_sclys(skip_iso)
    cut = cutscene_ids(jsonc)
    table = build_table(d, o, s, cut)

    base = apply_ops(d[LANDING_SITE], dict(table).get(LANDING_SITE, b''))[0]
    landing = landing_ops(base)

    pal_rooms, pal_lines = [], []
    if pal:
        pd, po, ps = (load_sclys(x) for x in pal)
        pal_rooms, pal_lines = pal_table(table, pd, po, ps, cut)
        # The Landing Site intro skip must replay on whichever stream PAL uses.
        stream = dict(pal_rooms).get(LANDING_SITE, dict(table).get(LANDING_SITE, b''))
        res, misses = apply_ops(pd[LANDING_SITE], stream)
        assert not misses, 'PAL Landing Site skip misses'
        res, misses = apply_ops(res, landing)
        assert not misses, 'PAL kLandingOps misses %s' % misses
        print('PAL: %d rooms differ from the USA streams' % len(pal_rooms))
        for line in pal_lines:
            print('  ' + line)

    with open(out, 'w') as f:
        f.write('// Generated by tools/gen_skippable_cutscenes.py from randomprime\'s\n'
                '// skippable_cutscenes.jsonc (MIT, see NOTICE). Do not edit.\n')
        n = write_tables(f, 'kSkipRooms', 'kSkipOps', table)
        f.write('\n// Landing Site intro skip, applied after kSkipRooms\' entry.\n'
                'static const unsigned char kLandingOps[] = {\n')
        for i in range(0, len(landing), 24):
            f.write('    ' + ','.join('0x%02X' % b for b in landing[i:i + 24]) + ',\n')
        f.write('};\n')
        if pal_rooms:
            f.write('\n// GM8P01: the rooms whose USA stream above misses or differs there.\n')
            write_tables(f, 'kSkipRoomsPal', 'kSkipOpsPal', pal_rooms)
    print('%d rooms, %d bytes of ops, %d landing' % (len(table), n, len(landing)))


if __name__ == '__main__':
    main()
