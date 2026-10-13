#!/usr/bin/env python3
"""USA 1.02 (rev2) skippable-cutscene tables: generator and replay checks.

    port_skip_rev2.py <source dir> [version=disc.iso ...]

Always: the rev2 section of platform/port_skip_cutscenes_data.inc is exactly
what tools/gen_skippable_cutscenes.py derives from the USA table (and
--rev2-only leaves it unchanged), it is the USA Chapel stream minus the one
moved remconn, and every stream splits into whole ops.

With `100=`, `101=`, `102=` or `pal=` disc images (local data, nothing is
tracked): every room's stream, chosen as the port does, applies on that disc
without a miss, together with the Landing Site and Arboretum patches.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

src = sys.argv[1]
sys.path.insert(0, os.path.join(src, 'tools'))
import gen_skippable_cutscenes as G  # noqa: E402

inc_path = os.path.join(src, 'platform', 'port_skip_cutscenes_data.inc')
inc = open(inc_path).read()
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAILED:', msg)


usa = G.parse_inc_table(inc, 'kSkipRooms', 'kSkipOps')
pal = G.parse_inc_table(inc, 'kSkipRoomsPal', 'kSkipOpsPal')
rev2 = G.parse_inc_table(inc, 'kSkipRoomsRev2', 'kSkipOpsRev2')

# --- generator ---
check(rev2 == G.rev2_table(usa), 'rev2 section matches the generator')
check([r for r, _ in rev2] == [G.CHAPEL], 'rev2 holds only Chapel of the Elders')
drop = struct.pack('>BIIII', G.OP_REMCONN, *G.CHAPEL_REV2_DROP)
keep = struct.pack('>BIIII', G.OP_ADDCONN, 0x000E0180, 9, 13, 0x000E00B2)
base_ops = dict(usa)[G.CHAPEL]
raw = [r for _, r in G.split_ops(base_ops)]
check(raw.count(drop) == 1 and keep in raw, 'USA Chapel stream holds the remconn and addconn 0180')
check(dict(rev2)[G.CHAPEL] == b''.join(r for r in raw if r != drop),
      'rev2 Chapel = USA minus only the moved remconn')
check(len(dict(rev2)[G.CHAPEL]) == len(base_ops) - 17, 'rev2 Chapel is 17 bytes shorter')
check(keep in [r for _, r in G.split_ops(dict(rev2)[G.CHAPEL])], 'rev2 Chapel keeps addconn 0180')
for name, table in (('USA', usa), ('PAL', pal), ('rev2', rev2)):
    for r, ops in table:
        try:
            G.split_ops(ops)
        except (SystemExit, struct.error, AssertionError) as e:
            check(False, '%s %08X does not split: %s' % (name, r, e))
# USA-only output (no PAL streams): empty tables use a row no room matches and
# still parse back as empty, and the rev2 section is derived the same.
import io  # noqa: E402
buf = io.StringIO()
G.write_tables(buf, 'kSkipRoomsPal', 'kSkipOpsPal', [])
check('0xFFFFFFFF' in buf.getvalue(), 'an empty table writes a sentinel row')
check(G.parse_inc_table(buf.getvalue(), 'kSkipRoomsPal', 'kSkipOpsPal') == [],
      'an empty table parses back empty')
check(G.parse_inc_table('', 'kSkipRoomsPal', 'kSkipOpsPal') == [], 'an absent PAL table is empty')
buf = io.StringIO()
G.write_tables(buf, 'kSkipRooms', 'kSkipOps', usa)
check(G.parse_inc_table(buf.getvalue(), 'kSkipRooms', 'kSkipOps') == usa, 'table writer round-trips')
try:
    G.rev2_table([(G.CHAPEL, b''.join(r for r in raw if r != drop))])
    check(False, 'rev2_table must reject a stream without the remconn')
except SystemExit:
    pass
tmp = tempfile.mkdtemp()
try:
    copy = os.path.join(tmp, 'data.inc')
    shutil.copy(inc_path, copy)
    subprocess.run([sys.executable, '-I', os.path.join(src, 'tools', 'gen_skippable_cutscenes.py'),
                    '--rev2-only', copy], check=True, stdout=subprocess.DEVNULL)
    check(open(copy).read() == inc, '--rev2-only reproduces the committed file')
finally:
    shutil.rmtree(tmp)

# --- replay on discs ---
ARB = struct.pack('>BIIII', 3, 0x130140, 5, 4, 0x1302AA) + struct.pack('>BI', 6, 0x1302AA)
CHAPEL_ROOM = G.CHAPEL
ARBORETUM = 0x18AB6106
LANDING = G.LANDING_SITE


def landing_ops():
    import re
    m = re.search(r'kLandingOps\[\] = \{(.*?)\};', inc, re.S)
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1)))


def stream_for(version, mrea):
    tables = {'pal': dict(pal), '102': dict(rev2)}
    if version in tables and mrea in tables[version]:
        return tables[version][mrea]
    return dict(usa).get(mrea)


discs = sys.argv[2:]
if discs:
    # configured (PORT_SKIP_DISCS): nothing may be skipped quietly
    try:
        import numpy  # noqa: F401  (tools/extract_textures.py reads the discs with it)
    except ImportError:
        check(False, 'the disc replay is configured but numpy is missing')
        discs = []

for arg in discs:
    version, path = arg.split('=', 1)
    if not os.path.exists(path):
        check(False, '%s: configured disc %s is missing' % (version, path))
        continue
    rooms = G.load_sclys(path)
    misses, skipped = [], 0
    for mrea, scly in sorted(rooms.items()):
        ops = stream_for(version, mrea)
        cur = scly
        if ops is not None:
            cur, m = G.apply_ops(scly, ops)
            if m:
                misses.append(('skip', '%08X' % mrea, m))
                continue
            skipped += 1
            if mrea == LANDING:
                cur, m = G.apply_ops(cur, landing_ops())
                if m:
                    misses.append(('landing', '%08X' % mrea, m))
        if mrea == ARBORETUM and version in ('101', '102'):
            _, m = G.apply_ops(cur, ARB)
            if m:
                misses.append(('arboretum', '%08X' % mrea, m))
    check(not misses, '%s: %d op misses %s' % (version, len(misses), misses))
    check(skipped == len(usa) or version == 'pal', '%s: %d of %d skips applied' % (version, skipped, len(usa)))
    print('%s: %d rooms, %d skips applied, %d misses' % (version, len(rooms), skipped, len(misses)))

sys.exit(1 if failures else 0)
