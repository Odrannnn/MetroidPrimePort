#!/usr/bin/env python3
"""Generate platform/port_ap_pickups_data.inc: randomprime's per-pickup room
patches and the retail pickup models, for Archipelago games.

In a randomized game a pickup no longer holds its retail item, so the retail
item's cutscene (Morph Ball, Varia, artifact totems...) must not play and the
pickup must look like what it holds. randomprime (MIT, see NOTICE) does the
first with two things, precomputed per pickup in its src/pickup_meta.rs.in:
  - objects_to_remove: the cinema relay, "Player Hint Disable Controls", the
    artifact totem layer switch and logbook screen, per room;
  - post_pickup_relay_connections: what the retail cinema's end did (jingle,
    HUD memo timers, doors, the Varia room's lights), moved onto a new
    "Randomizer Post Pickup Relay" that the pickup sets to zero on Arrived.

This turns both into the op stream of platform/port_skip_cutscenes.cpp,
applied after the skippable cutscene ops (always on in AP games), and checks
every op against the retail disc with those ops already applied.

    tools/gen_ap_pickup_patches.py <disc> <randomprime>/src/pickup_meta.rs.in \\
        platform/port_ap_pickups_data.inc
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_skippable_cutscenes import (  # noqa: E402
    OP_ADDCONN, OP_DELETE, OP_PUSH, apply_ops, load_sclys, parse_scly, split_name)

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP_INC = os.path.join(HERE, '..', 'platform', 'port_skip_cutscenes_data.inc')
LANDING_SITE = 0xB2701146

STATES = {'ZERO': 9}
MSGS = {'ACTIVATE': 0x1, 'DECREMENT': 0x5, 'INCREMENT': 0x7, 'RESET_AND_START': 0xB,
        'SET_TO_MAX': 0xC, 'SET_TO_ZERO': 0xD, 'PLAY': 0x14}
ARRIVED, SET_TO_ZERO = 1, 0xD
RELAY = 0x15

# Model keys past the item types (0-40).
MODEL_MAIN_POWER_BOMB = 41
MODEL_OTHER = 42  # items for other games; see CUSTOM_MODELS


def read_skip_inc(path):
    text = open(path).read()
    # Only the USA table: kSkipRoomsPal's offsets index kSkipOpsPal.
    usa = text.split('static const SkipRoom kSkipRooms[] = {')[1].split('};')[0]
    rooms = {int(m[1], 16): (int(m[2]), int(m[3]))
             for m in re.finditer(r'\{0x([0-9A-F]{8}), (\d+), (\d+)\}', usa)}

    def blob(name):
        body = text.split('static const unsigned char %s[] = {' % name)[1].split('};')[0]
        return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', body))

    ops = blob('kSkipOps')
    return {r: ops[o:o + n] for r, (o, n) in rooms.items()}, blob('kLandingOps')


def parse_meta(path):
    """[(mrea, [(pickup id, [conn])], [removed id])] from pickup_meta.rs.in."""
    text = open(path).read()
    rooms = []
    for block in text.split('RoomInfo {')[1:]:
        mrea = int(re.search(r'res_id::MREA>::new\(0x([0-9A-Fa-f]+)\)', block)[1], 16)
        pickups = []
        body = block.split('pickup_locations: &[', 1)[1]
        for loc in body.split('PickupLocation {')[1:]:
            pid = int(re.search(r'location: ScriptObjectLocation \{ layer: \d+, instance_id: (\d+)',
                                loc)[1])
            conns = []
            post = loc.split('post_pickup_relay_connections: &[', 1)[1].split(']', 1)[0]
            for st, msg, tgt in re.findall(r'ConnectionState::(\w+),\s*message: ConnectionMsg::(\w+),'
                                           r'\s*target_object_id: (0x[0-9a-fA-F]+|\d+)', post):
                conns.append((STATES[st], MSGS[msg], int(tgt, 0)))
            pickups.append((pid, conns))
        removed = []
        otr = block.split('objects_to_remove: &[', 1)[1].split('\n        },', 1)[0]
        for ids in re.findall(r'instance_ids: &\[([^\]]*)\]', otr):
            removed += [int(x) for x in ids.replace(' ', '').split(',') if x]
        rooms.append((mrea, pickups, removed))
    return rooms


def model_dependencies(path):
    """Every disc resource a pickup model needs (PickupModel::dependencies),
    past the custom 0xDEAF ids. A pickup can show a model from another world,
    whose PAK isn't loaded; randomprime copies these into the room's PAK, the
    port reads them from the disc instead."""
    text = open(path).read()
    body = text.split('pub fn dependencies', 1)[1].split('pub fn ', 1)[0]
    ids = {int(i, 16) for i in re.findall(r'\(0x([0-9A-Fa-f]{8}), FourCC', body)}
    return sorted(i for i in ids if i >> 16 != 0xDEAF)


def room_ops(mrea, scly, pickups, removed):
    _, layers = parse_scly(scly)
    objs = {o['id']: o for _, ob in layers for o in ob}
    ops = b''
    for oid in removed:
        # The skippable patch already dropped a few of these.
        if oid in objs:
            ops += struct.pack('>BI', OP_DELETE, oid)
    top = max(o['id'] & 0xFFFF for o in objs.values())
    area = layers[0][1][0]['id'] & 0x03FF0000
    for pid, conns in pickups:
        assert pid in objs and objs[pid]['type'] == 0x11, '%08X: pickup %X' % (mrea, pid)
        if not conns:
            continue
        top += 1
        relay = area | top
        assert relay not in objs, '%08X: relay id %X taken' % (mrea, relay)
        props = struct.pack('>I', 2) + b'Randomizer Post Pickup Relay\0' + b'\x01'
        ops += struct.pack('>BBBIH', OP_PUSH, 0, RELAY, relay, len(conns))
        ops += b''.join(struct.pack('>III', *c) for c in conns)
        ops += struct.pack('>I', len(props)) + props
        ops += struct.pack('>BIIII', OP_ADDCONN, pid, ARRIVED, SET_TO_ZERO, relay)
    return ops


def pickup_models(sclys):
    """Model key -> (model, acs, character, animation), from the disc's
    upgrade pickups (capacity > 0)."""
    out = {}
    for mrea in sorted(sclys):
        for _, objs in parse_scly(sclys[mrea])[1]:
            for o in objs:
                if o['type'] != 0x11:
                    continue
                t = split_name(o['props'])[2]
                item, cap = struct.unpack_from('>II', t, 60)
                model = struct.unpack_from('>IIII', t, 84)
                if cap == 0 or (model[0] == 0xFFFFFFFF and model[1] == 0xFFFFFFFF):
                    continue
                key = MODEL_MAIN_POWER_BOMB if item == 7 and cap > 1 else item
                # Elite Quarters' Phazon Suit borrows the Gravity Suit model.
                if item != 23:
                    out.setdefault(key, model)
    return out


MODEL_OTHER_PROGRESSION = 43
MODEL_OTHER_USEFUL = 44

# Models the disc lacks, as the AP world picks them. The 0xDEAF ids are
# randomprime's custom assets, which platform/port_custom_res.cpp builds from
# its extra_assets and disc models (a randomprime disc's own copies win).
CUSTOM_MODELS = {
    0: (0x853A56F0, 0x7C04E388, 0, 0),    # Power Beam: the Super Missile
    5: (0x61DAB956, 0x9F0C908A, 0, 0),    # Scan Visor: the retail visor
    9: (0xDEAF000B, 0xDEAF000C, 0, 0),    # Thermal Visor
    13: (0xDEAF000D, 0xDEAF000E, 0, 0),   # X-Ray Visor
    17: (0xDEAF000F, 0xDEAF0010, 0, 0),   # Combat Visor
    23: (0xDEAF0002, 0xDEAF0003, 0, 0),   # Phazon Suit
    MODEL_OTHER: (0xDEAF0005, 0xDEAF0006, 0, 0),              # Nothing
    MODEL_OTHER_PROGRESSION: (0xDEAF0009, 0xDEAF000A, 0, 0),  # Cog
    MODEL_OTHER_USEFUL: (0xDEAF0007, 0xDEAF0008, 0, 0),       # Zoomer
}


def c_float(v):
    s = '%.9g' % v
    return s + ('f' if '.' in s or 'e' in s else '.0f')


def pickup_geometry(path):
    """CMDL -> (aabb[6], rotation[3], scale[3]) from pickup_meta.rs.in: the
    model's bounds and the rotation and scale randomprime gives a pickup that
    shows it (update_pickup recentres a replaced model on the retail one)."""
    text = open(path).read()
    aabbs = {}
    table = text.split('const PICKUP_CMDL_AABBS', 1)[1].split('];', 1)[0]
    for cmdl, words in re.findall(r'\(0x([0-9A-F]{8}), \[([^\]]*)\]\)', table):
        aabbs[int(cmdl, 16)] = struct.unpack('>6f', struct.pack(
            '>6I', *(int(w, 16) for w in words.split(','))))
    placement = {}
    raw = text.split('fn raw_pickup_data', 1)[1]
    for name, body in re.findall(r'PickupModel::(\w+) => &\[([^\]]*)\]', raw):
        data = bytes(int(b, 16) for b in body.replace('\n', '').split(',') if b.strip())
        start = data.index(b'\0', 4) + 1
        rot = struct.unpack_from('>3f', data, start + 12)
        scale = struct.unpack_from('>3f', data, start + 24)
        cmdl = struct.unpack_from('>I', data, start + 84)[0]
        # pickup_data()'s overrides of the raw scale.
        scale = {'Nothing': (1.0,) * 3, 'Cog': (0.7,) * 3}.get(name, scale)
        placement.setdefault(cmdl, (rot, scale))
    return {m: (aabbs[m],) + placement[m] for m in aabbs if m in placement}


def main():
    disc, meta, out = sys.argv[1:4]
    sclys = load_sclys(disc)
    skip, landing = read_skip_inc(SKIP_INC)
    table = []
    for mrea, pickups, removed in parse_meta(meta):
        base = sclys[mrea]
        if mrea in skip:
            base, misses = apply_ops(base, skip[mrea])
            assert not misses, (hex(mrea), misses)
        if mrea == LANDING_SITE:
            base, misses = apply_ops(base, landing)
            assert not misses, 'landing'
        ops = room_ops(mrea, base, pickups, removed)
        if not ops:
            continue
        result, misses = apply_ops(base, ops)
        assert not misses, (hex(mrea), misses)
        # Every object the new relays talk to must still be there (randomprime
        # keeps a few that point outside the room; those it drops too).
        left = {o['id'] for _, ob in parse_scly(result)[1] for o in ob}
        for pid, conns in pickups:
            for c in conns:
                if c[2] not in left and c[2] in {o['id'] for _, ob in parse_scly(sclys[mrea])[1]
                                                  for o in ob}:
                    raise SystemExit('%08X: relay target %X removed' % (mrea, c[2]))
        table.append((mrea, ops))
    table.sort()
    models = pickup_models(sclys)
    models.update(CUSTOM_MODELS)
    deps = model_dependencies(meta)
    geometry = pickup_geometry(meta)
    for key, (model, _, _, _) in models.items():
        if model not in geometry:
            print('warning: no bounds for model %08X (key %d)' % (model, key))

    blob = bytearray()
    with open(out, 'w') as f:
        f.write('// Generated by tools/gen_ap_pickup_patches.py from randomprime\'s\n'
                '// pickup_meta.rs.in (MIT, see NOTICE) and the retail disc. Do not edit.\n')
        f.write('static const PickupRoom kPickupRooms[] = {\n')
        for r, ops in table:
            f.write('    {0x%08X, %d, %d},\n' % (r, len(blob), len(ops)))
            blob += ops
        f.write('};\n\nstatic const unsigned char kPickupOps[] = {\n')
        for i in range(0, len(blob), 24):
            f.write('    ' + ','.join('0x%02X' % b for b in blob[i:i + 24]) + ',\n')
        f.write('};\n\n// Item type (0-40), 41 = main Power Bomb, 42-44 = another game\'s\n'
                '// filler, progression and useful items.\n'
                'static const PickupModelEntry kPickupModels[] = {\n')
        for key in sorted(models):
            f.write('    {%d, 0x%08X, 0x%08X, %d, %d},\n' % ((key,) + models[key]))
        f.write('};\n\n// What those models load, sorted; read from the disc when no loaded\n'
                '// PAK has them.\nstatic const uint32_t kPickupDependencies[] = {\n')
        for i in range(0, len(deps), 6):
            f.write('    ' + ', '.join('0x%08X' % d for d in deps[i:i + 6]) + ',\n')
        f.write('};\n\n// Pickup models\' bounds and the rotation (degrees) and scale a pickup\n'
                '// showing them gets, sorted by model.\n'
                'static const PickupGeometry kPickupGeometry[] = {\n')
        for m in sorted(geometry):
            f.write('    {0x%08X, {%s}, {%s}, {%s}},\n' % ((m,) + tuple(
                ', '.join(c_float(v) for v in vals) for vals in geometry[m])))
        f.write('};\n')
    print('%d rooms, %d bytes of ops, %d models, %d dependencies'
          % (len(table), len(blob), len(models), len(deps)))


if __name__ == '__main__':
    main()
