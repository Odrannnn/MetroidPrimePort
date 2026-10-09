#!/usr/bin/env python3
"""Record and check the initial pipeline cache the port ships (assets/initial_pipeline_cache.db).

aurora replays the seed's rows at startup and compiles them in the background, so a new
install doesn't skip draws (or stutter) the first time it meets a shader. The rows are
raw pipeline configs; a version bump of one of them (GXPipelineConfigVersion,
ClearPipelineConfigVersion, RmlPipelineConfigVersion) makes aurora ignore every row of
that type, so the seed has to be recorded again:

    python3 tools/pipeline_seed.py tour            # ~1-2 h: front end + every room on the rig
    python3 tools/pipeline_seed.py merge --order build/pipeline-seed/order.txt \
        build/pipeline-seed/cache/pipeline_cache.db
    python3 tools/pipeline_seed.py check           # also run by ctest (port_pipeline_seed)

The seed holds retail rows only: `merge` and `check` reject GX rows that only Remastered
content or mods produce (PBR, sun shadow, volumetric fog, distance-field fonts).
"""

import argparse
import os
import shutil
import re
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEED = ROOT / "assets" / "initial_pipeline_cache.db"
MPRIG = [sys.executable, str(ROOT / "tools" / "mprig.py")]

TYPE_CLEAR, TYPE_GX, TYPE_RML = 0, 1, 2
TYPE_NAMES = {TYPE_CLEAR: "clear", TYPE_GX: "gx", TYPE_RML: "rml"}
VERSION_SOURCES = {  # type: (header, constant)
    TYPE_CLEAR: ("extern/aurora/lib/gfx/clear.hpp", "ClearPipelineConfigVersion"),
    TYPE_GX: ("extern/aurora/lib/gx/pipeline.hpp", "GXPipelineConfigVersion"),
    TYPE_RML: ("extern/aurora/lib/rmlui/pipeline.hpp", "RmlPipelineConfigVersion"),
}
GX_VA_NULL = 0xFF
# Worlds in the order a first playthrough meets them, so the seed compiles the frigate first.
WORLD_ORDER = ["158EFE17", "39F2DE28", "83F6FF6F", "3EF8237C", "A8BE6291", "B1AC4D65", "C13B09D1"]

SCHEMA = """
CREATE TABLE aurora_schema(value INTEGER);
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
"""


def current_versions():
    out = {}
    for t, (header, name) in VERSION_SOURCES.items():
        m = re.search(rf"constexpr\s+uint32_t\s+{name}\s*=\s*(\d+)\s*;", (ROOT / header).read_text())
        if not m:
            sys.exit(f"pipeline_seed: {name} not found in {header}")
        out[t] = int(m.group(1))
    return out


def remastered_reason(config):
    """Why a GX row can't come from retail content, or None. The blob is gx::PipelineConfig:
    u32 version, u32 msaaSamples, then gx::ShaderConfig (extern/aurora/lib/gx/gx.hpp)."""
    sc = config[8:]
    if len(sc) < 9:
        return "truncated"
    if (sc[2] >> 4) & 1:
        return "shadow"
    if sc[3]:
        return "pbr"
    if sc[4]:
        return "sdf"
    if sc[6]:
        return "pbrKind"
    if sc[7]:
        return "volFog"
    if sc[8] != GX_VA_NULL:
        return "pbrLightmapAttr"
    return None


def read_rows(path):
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        return db.execute("SELECT type, hash, config_version, config_size, config, first_frame_used "
                          "FROM pipeline_cache").fetchall()
    finally:
        db.close()


def problems(rows, versions):
    """(row index, reason) for every row the seed must not hold."""
    out = []
    sizes = {}
    for i, (t, _h, ver, size, config, _f) in enumerate(rows):
        if t not in versions:
            out.append((i, f"unknown type {t}"))
        elif ver != versions[t]:
            out.append((i, f"{TYPE_NAMES[t]} version {ver}, current {versions[t]}"))
        elif size != len(config):
            out.append((i, f"size {size} != blob {len(config)}"))
        elif t == TYPE_GX and int.from_bytes(config[0:4], "little") != ver:
            out.append((i, "blob version differs from its row"))
        elif t == TYPE_GX and remastered_reason(config):
            out.append((i, f"Remastered-only ({remastered_reason(config)})"))
        else:
            sizes.setdefault(t, set()).add(size)
    for t, s in sizes.items():
        if len(s) > 1:
            out.append((-1, f"{TYPE_NAMES[t]} rows of several sizes {sorted(s)} (recorded by different builds)"))
    return out


def cmd_merge(a):
    versions = current_versions()
    rank = {}
    if a.order:
        for n, line in enumerate(Path(a.order).read_text().split()):
            rank.setdefault(tuple(int(x) for x in line.split(":")), n)
    kept, dropped = {}, {}
    for src in a.inputs:
        rows = read_rows(src)
        bad = {i: r for i, r in problems(rows, versions) if i >= 0}
        if any(i < 0 for i, _ in problems(rows, versions)):
            sys.exit(f"pipeline_seed: {src}: rows of several sizes; record with one build")
        for i, row in enumerate(rows):
            if i in bad:
                key = bad[i].split(" (")[0].split(" version")[0]
                dropped[key] = dropped.get(key, 0) + 1
                continue
            t, h, ver, size, config, first = row
            # first_frame_used restarts with every run: the tour's order file ranks rows across runs.
            row = (t, h, ver, size, config, (rank.get((t, h), len(rank)), first))
            old = kept.get((t, h))
            if old is None or row[5] < old[5]:
                kept[(t, h)] = row
    out = Path(a.output)
    tmp = out.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)
    db = sqlite3.connect(tmp)
    db.executescript(SCHEMA)
    src_schema = sqlite3.connect(f"file:{a.inputs[0]}?mode=ro", uri=True)
    schema = src_schema.execute("SELECT value FROM aurora_schema").fetchall()
    src_schema.close()
    db.executemany("INSERT INTO aurora_schema VALUES (?)", schema)
    # first_frame_used renumbered 0..n-1 in the order the tour met them: the load order.
    ordered = sorted(kept.values(), key=lambda r: (r[5], r[0], r[1]))
    db.executemany("INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, ?)",
                   [(t, h, v, s, c, n) for n, (t, h, v, s, c, _f) in enumerate(ordered)])
    db.commit()
    db.execute("VACUUM")
    db.close()
    os.replace(tmp, out)
    counts = {}
    for r in ordered:
        counts[TYPE_NAMES[r[0]]] = counts.get(TYPE_NAMES[r[0]], 0) + 1
    print(f"{out}: {len(ordered)} rows {counts}, {out.stat().st_size // 1024} KiB; dropped {dropped or 'none'}")


def cmd_check(a):
    versions = current_versions()
    rows = read_rows(a.seed)
    bad = problems(rows, versions)
    for i, reason in bad[:20]:
        print(f"{a.seed}: row {i}: {reason}" if i >= 0 else f"{a.seed}: {reason}")
    if bad:
        print(f"{len(bad)} bad rows; re-record the seed (python3 tools/pipeline_seed.py --help)")
        sys.exit(1)
    if not any(r[0] == TYPE_GX for r in rows):
        sys.exit(f"{a.seed}: no GX rows")
    print(f"{a.seed}: {len(rows)} rows, all current and retail")


# --- tour -------------------------------------------------------------------

def rig(*args, check=True, timeout=None):
    r = subprocess.run(MPRIG + list(args), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode:
        raise RuntimeError(f"mprig {' '.join(args[:3])}: {(r.stderr or r.stdout).strip()[-300:]}")
    return r


def rooms():
    out = []
    for line in rig("rooms").stdout.splitlines():
        name, room = line.rsplit("\t", 1)
        out.append((name, room))
    order = {w: i for i, w in enumerate(WORLD_ORDER)}
    return sorted(out, key=lambda r: order.get(r[1].split(":")[0], len(order)))


ITEMS = ["PowerBeam", "IceBeam", "WaveBeam", "PlasmaBeam", "Missiles 250", "ScanVisor", "MorphBallBombs",
         "PowerBombs 8", "Flamethrower", "ThermalVisor", "ChargeBeam", "SuperMissile", "GrappleBeam",
         "XRayVisor", "IceSpreader", "SpaceJumpBoots", "MorphBall", "CombatVisor", "BoostBall", "SpiderBall",
         "Wavebuster", "EnergyTanks 14", "VariaSuit", "GravitySuit"]


def room_actions(first):
    """What a player does in a room, so its weapons, visors and ball draw there."""
    c = ["god on"] + [f"give {i}" for i in ITEMS] + ["heal", "wait 30"]
    for yaw in (90, 180, 270, 0):
        c += [f"face {yaw}", "wait 20"]
    for beam in range(4):
        c += [f"beam {beam}", "wait 40", "press a 4", "wait 30", "press a 90", "wait 40",
              "press y 4", "wait 40"]
    c += ["beam 0", "wait 30"]
    for visor in (3, 2, 1, 0):  # thermal, scan, x-ray, combat
        c += [f"visor {visor}", "wait 45"]
    c += ["press x 4", "wait 60", "press a 4", "wait 90", "press y 4", "wait 120",
          "press b 4", "wait 30", "press x 4", "wait 60"]
    if first:
        # pause screen and map once
        c += ["press start 4", "wait 90", "press start 4", "wait 60", "press z 4", "wait 120",
              "press z 4", "wait 60"]
    return c


def settle(name, tries=30):
    """Press A until the game ticks. Some spawns open a modal message (Missile Station Mines spawns
    on its pickup; Ruined Fountain's drop shows one) that pauses the world, so console commands fail."""
    for _ in range(tries):
        rig("cmd", name, "press a 10", check=False, timeout=60)
        if rig("cmd", name, "status", check=False, timeout=60).returncode == 0:
            return
        time.sleep(2)
    raise RuntimeError("the game never started ticking")


def start(name, cache, room, build):
    # Detached with --wait: mprig's readiness check needs a ticking game and kills it otherwise.
    subprocess.Popen(MPRIG + ["start", name, "--room", room, "--mods", "none", "--settings", "none",
                              "--replace", "--wait", "3000", "--env", f"MP_CACHE_PATH={cache}",
                              "--env", "MP_PIPELINE_SEED=0"]
                     + (["--build", build] if build else []),
                     cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(45)
    settle(name)


def cmd_tour(a):
    out = Path(a.out).resolve()
    cache = out / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    db = cache / "pipeline_cache.db"
    if db.exists() and not a.resume:
        sys.exit(f"pipeline_seed: {db} exists (--resume to add to it, or delete it)")
    name = a.name
    plan = rooms()
    if a.limit:
        plan = plan[:a.limit]
    done = set()
    progress = out / "done.txt"
    if a.resume and progress.exists():
        done = set(progress.read_text().split())
    log = open(out / "tour.log", "a")
    order = out / "order.txt"
    seen = set(order.read_text().split()) if order.exists() else set()

    def note_order():
        # first_frame_used restarts with each run, so record the order rows appeared in.
        if not db.exists():
            return 0
        rows = read_rows(db)
        new = sorted((r for r in rows if f"{r[0]}:{r[1]}" not in seen), key=lambda r: r[5])
        with open(order, "a") as f:
            for r in new:
                seen.add(f"{r[0]}:{r[1]}")
                f.write(f"{r[0]}:{r[1]}\n")
        return len(rows)

    def say(msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    if not a.no_front_end and "front-end" not in done:
        # Logos, attract, title, file select and options, without --room (MP_FAST_BOOT=0).
        say("front end")
        p = subprocess.Popen(MPRIG + ["start", name, "--mods", "none", "--settings", "none", "--replace",
                                      "--wait", "3000", "--env", "MP_FAST_BOOT=0", "--env",
                                      f"MP_CACHE_PATH={cache}", "--env", "MP_PIPELINE_SEED=0"] + (["--build", a.build] if a.build else []),
                             cwd=ROOT,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(45)
        for cmds in (["wait 600"], ["press start 4", "wait 240"], ["press a 4", "wait 240"],
                     ["press down 4", "wait 60", "press down 4", "wait 60", "press a 4", "wait 240"],
                     ["press b 4", "wait 120", "press b 4", "wait 120"]):
            r = rig("cmd", name, *cmds, check=False, timeout=600)
            if r.returncode:
                say(f"front end: {(r.stderr or r.stdout).strip()[-200:]}")
                break
        rig("stop", name, check=False)
        p.wait(timeout=60)
        note_order()
        with open(progress, "a") as f:
            f.write("front-end\n")

    running = False
    for i, (label, room) in enumerate(plan):
        if room in done:
            continue
        mlvl, mrea = room.split(":")
        t0 = time.monotonic()
        try:
            if not running:
                start(name, cache, room, a.build)
                running = True
            else:
                rig("cmd", name, f"warp {mlvl} {mrea}", timeout=600)
                settle(name)
            rig("cmd", name, *room_actions(i == 0), timeout=900)
            status = "ok"
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            status = f"FAILED {e}"
            rig("stop", name, check=False)
            running = False
            # The next start wipes the run dir; keep the game log to tell a crash from the rig.
            game_log = ROOT / "build" / "rig" / name / "game.log"
            if game_log.exists():
                shutil.copy(game_log, out / f"fail-{mlvl}-{mrea}.log")
        if status == "ok":  # failed rooms stay out of done.txt, so --resume retries them
            with open(progress, "a") as f:
                f.write(room + "\n")
        n = note_order()
        say(f"[{i + 1}/{len(plan)}] {label} {room} {status} {time.monotonic() - t0:.0f}s rows={n}")
    if running:
        rig("stop", name, check=False)
    say(f"done: {db} (then: python3 tools/pipeline_seed.py merge --order {order} {db})")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tour", help="record a cache: front end, then every room (retail, default settings)")
    t.add_argument("--out", default=str(ROOT / "build" / "pipeline-seed"))
    t.add_argument("--name", default="pseed")
    t.add_argument("--resume", action="store_true", help="add to an existing cache, skipping rooms done")
    t.add_argument("--limit", type=int, help="only the first N rooms (testing)")
    t.add_argument("--no-front-end", action="store_true")
    t.add_argument("--build", help="build dir or binary path for mprig (default: rig.ini's)")
    m = sub.add_parser("merge", help="keep current retail rows of one or more caches, write the seed")
    m.add_argument("inputs", nargs="+")
    m.add_argument("-o", "--output", default=str(SEED))
    m.add_argument("--order", help="the tour's order.txt (type:hash per line, first seen first)")
    c = sub.add_parser("check", help="fail if the seed holds stale or Remastered-only rows")
    c.add_argument("seed", nargs="?", default=str(SEED))
    a = p.parse_args()
    {"tour": cmd_tour, "merge": cmd_merge, "check": cmd_check}[a.cmd](a)


if __name__ == "__main__":
    main()
