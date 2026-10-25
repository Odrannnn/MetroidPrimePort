#!/usr/bin/env python3
"""Pre-release crash soak on the headless rig (`tools/mprig.py`).

Drives the shipping matrix -- Remastered and retail, Dawn validation/robustness on
and off, MSAA 1 and 4 -- through a fixed tour of rooms, capturing one screenshot
per room, and fails on anything that killed the game: a crash, a WebGPU error, a
device loss, a vertex-array overrun or an aurora FATAL.

    python3 tools/mpsoak.py --evidence-dir build/s-release-<date>/soak
    python3 tools/mpsoak.py --dry-run --evidence-dir /tmp/soak-dry    # print the commands

One game per (mode, toggles), named `soak-<mode>-<toggles>`, started with
`--turbo`; `msaa N` switches the sample count inside that game, so the same
process covers both values. Screenshots go to
`<evidence>/<mode>-<toggles>-msaa<N>/`, one row per (mode, toggles, msaa, room)
to `<evidence>/summary.tsv`; exit status is 1 when any row FAILs.

Per-room captures use `mprig shot <run> <out.png> "warp MLVL MREA"`: `mprig
shots` starts and stops a game of its own, so it cannot share one process across
two MSAA settings, and a per-room `shot` also says which room killed the game.
After a death the run is restarted (`--replace`) and the tour continues with the
next room.
"""

import argparse
import atexit
import difflib
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MPRIG = ROOT / "tools" / "mprig.py"
RIG = ROOT / "build" / "rig"
TIMEOUT = 900  # seconds for one mprig call (a cold warp can take ~60 s)

# The default tour: one room per area plus the Tallon Overworld <-> Magmoor link
# and the Frigate, so every world is drawn. `rooms` resolves names; the two
# entries that are not unique by name are given as MLVL:MREA. `room@id` also
# turns to face the object with that editor id (console `look`) before the shot.
DEFAULT_ROOMS = [
    "Exterior Docking Hangar",
    "Tallon Canyon@0006007E",  # facing its water's fog volume (issue #200)
    "Root Cave",  # Tallon Overworld's root room; there is no "Root Cavern"
    "Transport to Magmoor Caverns East",  # Tallon side of the Magmoor elevator
    "Transport to Tallon Overworld West@0010006F",  # Magmoor side, facing the lava (issue #200)
    "Phendrana Shorelines",
    "Main Plaza",
    "Main Quarry",
    "Sunchamber",
    "Hive Totem",
    "Crater Entry Point",
    "158EFE17:87452DC1",  # Reactor Core (Frigate Orpheon); the name matches two rooms
]

MODES = {"remastered": [], "retail": ["--mods", "none"]}
TOGGLES = {
    # Dawn validation and robustness back on: any WebGPU error is fatal in game.
    "validate": ["--env", "MP_DAWN_DISABLE=skip_validation,disable_robustness"],
    "release": [],  # the shipping toggles
}
# Both toggles also check the GX array bounds.
ALWAYS = ["--env", "MP_GX_CHECK_ARRAYS=1"]

# Desktop aurora logs through fmt::println(stderr, "[{level}] [{module}] {message}")
# (logCallback is null there), so a Log.fatal/FATAL line starts "[fatal]".
FAIL_TEXT = ("port: crashed:", "WebGPU error", "Device lost", "gx: vertex array overrun:", "[fatal]")
WARN_TEXT = ("not supported; skipped",)

HEX_ROOM = re.compile(r"[0-9A-Fa-f]{1,8}(:[0-9A-Fa-f]{1,8})?")


def die(msg):
    print(f"mpsoak: {msg}", file=sys.stderr)
    sys.exit(1)


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "room"


def rig(args):
    """Run one mprig subcommand; return CompletedProcess with merged output."""
    cmd = [sys.executable, str(MPRIG), *map(str, args)]
    try:
        r = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=TIMEOUT)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, 124, (e.output or "") + f"\nmpsoak: mprig timed out after {TIMEOUT}s\n")
    return r


def render(args):
    return shlex.join(["python3", "tools/mprig.py", *map(str, args)])


def last_line(text):
    lines = [l for l in text.splitlines() if l.strip()]
    return lines[-1].strip() if lines else "(no output)"


# ---------------------------------------------------------------- rooms

class RoomError(Exception):
    pass


def room_table():
    """[(name, 'MLVL:MREA')] from `mprig rooms` (the kRooms[] table, no second copy)."""
    r = rig(["rooms"])
    if r.returncode:
        die(f"`mprig.py rooms` failed: {last_line(r.stdout)}")
    rooms = []
    for line in r.stdout.splitlines():
        if "\t" in line:
            name, room = line.rsplit("\t", 1)
            rooms.append((name.strip(), room.strip().upper()))
    if not rooms:
        die("`mprig.py rooms` printed no rooms")
    return rooms


def near_matches(text, rooms, limit=8):
    names = [n for n, _ in rooms]
    hits = []
    words = [w for w in re.split(r"[^a-z0-9]+", text.lower()) if len(w) > 2]
    for name, room in rooms:
        low = name.lower()
        if any(w in low for w in words):
            hits.append((name, room))
    for guess in difflib.get_close_matches(text, names, n=limit, cutoff=0.4):
        if all(guess != n for n, _ in hits):
            hits.extend(r for r in rooms if r[0] == guess)
    seen, out = set(), []
    for name, room in hits:
        if (name, room) not in seen:
            seen.add((name, room))
            out.append((name, room))
    return out[:limit]


def resolve(text, rooms):
    """Name or MLVL[:MREA] -> (name, 'MLVL:MREA'); mprig's exact/prefix/substring order."""
    text = text.strip()
    if not text:
        raise RoomError("empty room name")
    if HEX_ROOM.fullmatch(text) and (":" in text or len(text) == 8):
        want = text.upper()
        for name, room in rooms:
            if room == want:
                return name, room
        return want, want
    want = text.lower()
    picks = (
        lambda n: n.lower() == want,
        lambda n: n.lower().startswith(want),
        lambda n: want in n.lower(),
    )
    for pick in picks:
        hit = [r for r in rooms if pick(r[0])]
        if len(hit) == 1:
            return hit[0]
        if hit:
            ids = {r[1] for r in hit}
            if len(ids) == 1:
                return hit[0]
            raise RoomError(f"room '{text}' is ambiguous: "
                            + "; ".join(f"{n} {w}" for n, w in hit[:8]))
    near = near_matches(text, rooms)
    raise RoomError(f"no room named '{text}'" + (
        ": closest are " + "; ".join(f"{n} {w}" for n, w in near) if near else " (see: mprig.py rooms)"))


def split_axis(values, choices, label):
    if values is None:
        return list(choices)
    out = []
    for v in str(values).split(","):
        v = v.strip()
        if not v:
            continue
        if v not in choices:
            die(f"--{label} '{v}' is not one of: {', '.join(choices)}")
        if v not in out:
            out.append(v)
    if not out:
        die(f"--{label} is empty")
    return out


def split_msaa(values):
    if values is None:
        return [1, 4]
    out = []
    for v in str(values).replace(",", " ").split():
        try:
            n = int(v)
        except ValueError:
            die(f"--msaa '{v}' is not a number")
        if n < 1 or n > 16:
            die(f"--msaa {n} is outside 1..16 (the console's range)")
        if n not in out:
            out.append(n)
    if not out:
        die("--msaa is empty")
    return out


# ---------------------------------------------------------------- log scanning

def log_path(name):
    return RIG / name / "game.log"


def log_size(name):
    p = log_path(name)
    return p.stat().st_size if p.is_file() else 0


def log_since(name, pos):
    """The part of game.log written since `pos` (everything, if the file was truncated)."""
    p = log_path(name)
    if not p.is_file():
        return ""
    if p.stat().st_size < pos:
        pos = 0
    with open(p, "rb") as f:
        f.seek(pos)
        return f.read().decode("utf-8", "replace")


def classify(text):
    """(first fail line, first warn line) of a log chunk."""
    fail = warn = None
    for line in text.splitlines():
        if any(w in line for w in WARN_TEXT):
            warn = warn or line.strip()
            continue
        if any(f in line for f in FAIL_TEXT):
            fail = fail or line.strip()
    return fail, warn


def alive(name):
    """mprig's own view of the run's game process."""
    r = rig(["ls"])
    for line in r.stdout.splitlines():
        parts = line.split()
        if parts and parts[0] == name and len(parts) > 1:
            return parts[1] == "alive"
    return False


# ---------------------------------------------------------------- soak

class Soak:
    def __init__(self, args):
        self.args = args
        self.evidence = Path(args.evidence_dir).resolve()
        self.modes = split_axis(args.modes, list(MODES), "modes")
        self.toggles = split_axis(args.toggles, list(TOGGLES), "toggles")
        self.msaa = split_msaa(args.msaa)
        table = room_table()
        wanted = args.rooms.split(",") if args.rooms else DEFAULT_ROOMS
        self.rooms = []
        problems = []
        for text in wanted:
            text, _, look = text.partition("@")
            try:
                name, room = resolve(text, table)
                self.rooms.append((name, f"{room}@{look.strip()}" if look.strip() else room))
            except RoomError as e:
                problems.append(str(e))
        if problems:
            die("\n".join(problems))
        self.rows = []
        self.names = []  # run names, in matrix order
        self.summary = None

    # -- output

    def open_summary(self):
        self.evidence.mkdir(parents=True, exist_ok=True)
        self.summary = open(self.evidence / "summary.tsv", "w", encoding="utf-8")
        self.summary.write("mode\ttoggles\tmsaa\troom\tresult\tfirst matching line\n")
        self.summary.flush()

    def row(self, mode, toggle, msaa, room, result, detail=""):
        self.rows.append((mode, toggle, str(msaa), room, result, detail))
        detail = " ".join(detail.split())
        print(f"{mode:<11} {toggle:<8} {msaa:<4} {room:<48} {result:<4} {detail[:110]}", flush=True)
        if self.summary:
            self.summary.write(f"{mode}\t{toggle}\t{msaa}\t{room}\t{result}\t{detail}\n")
            self.summary.flush()

    def note(self, text):
        print(f"mpsoak: {text}", file=sys.stderr, flush=True)

    # -- commands

    def start_args(self, name, mode, toggle):
        args = ["start", name]
        if self.args.build:
            args += ["--build", self.args.build]
        if self.args.disc:
            args += ["--disc", self.args.disc]
        return args + ["--turbo", "--replace"] + MODES[mode] + TOGGLES[toggle] + ALWAYS

    def shot_args(self, name, out, room_id):
        room_id, _, look = room_id.partition("@")
        mlvl, _, mrea = room_id.partition(":")
        warp = f"warp {mlvl} {mrea}" if mrea else f"warp {mlvl}"
        args = ["shot", "--settle", self.args.settle, name, out, warp]
        # The spawn view can miss what a room has to draw (fog volumes over lava): face it.
        return args + [f"look {look}"] if look else args

    def out_path(self, mode, toggle, msaa, index, name):
        return self.evidence / f"{mode}-{toggle}-msaa{msaa}" / f"{index:02d}-{slug(name)}.png"

    # -- dry run

    def plan(self):
        print(f"# {len(self.rooms)} rooms, {len(self.modes)} modes x {len(self.toggles)} toggles "
              f"x {len(self.msaa)} msaa")
        for name, room in self.rooms:
            print(f"#   {name} {room}")
        for mode in self.modes:
            for toggle in self.toggles:
                name = f"soak-{mode}-{toggle}"
                print(f"\n# {mode} + {toggle}: one game, {len(self.msaa)} msaa x {len(self.rooms)} rooms")
                print(render(self.start_args(name, mode, toggle)))
                for msaa in self.msaa:
                    print(render(["cmd", name, f"msaa {msaa}"]))
                    for i, (room_name, room) in enumerate(self.rooms, 1):
                        out = self.out_path(mode, toggle, msaa, i, room_name)
                        print(render(self.shot_args(name, out, room)))
                    print(render(["crash", name]) + "   # only if the game died")
                print(render(["stop", name]))
        print(f"\n# rows -> {self.evidence / 'summary.tsv'}")

    # -- run

    def ensure_started(self, name, mode, toggle, msaa):
        """Start (or restart) the run and put MSAA back; return None or a failure detail."""
        args = self.start_args(name, mode, toggle)
        r = rig(args)
        with open(self.evidence / f"{mode}-{toggle}-start.txt", "a", encoding="utf-8") as f:
            f.write("$ " + render(args) + "\n" + r.stdout)
        if r.returncode:
            return f"start failed: {last_line(r.stdout)}"
        return self.set_msaa(name, msaa)

    def set_msaa(self, name, msaa):
        """`msaa N` in the running game; return None or a failure detail."""
        r = rig(["cmd", name, f"msaa {msaa}"])
        return None if not r.returncode else f"msaa {msaa} failed: {last_line(r.stdout)}"

    def record_crash(self, name, mode, toggle, msaa, room_name):
        out = self.evidence / f"{mode}-{toggle}-msaa{msaa}-{slug(room_name)}-crash.txt"
        r = rig(["crash", name])
        out.write_text(f"$ {render(['crash', name])}\n{r.stdout}", encoding="utf-8")
        return str(out)

    def soak(self):
        self.open_summary()
        for mode in self.modes:
            for toggle in self.toggles:
                self.matrix(mode, toggle)
        return self.finish()

    def matrix(self, mode, toggle):
        name = f"soak-{mode}-{toggle}"
        self.names.append(name)
        self.note(f"{name}: {len(self.msaa)} msaa x {len(self.rooms)} rooms")
        t0 = time.monotonic()
        for msaa in self.msaa:
            # One game for the whole matrix: switch MSAA in it, and only start a game
            # when the previous room killed the last one.
            dead = self.set_msaa(name, msaa) if alive(name) else \
                self.ensure_started(name, mode, toggle, msaa)
            if dead:
                for room_name, room in self.rooms:
                    self.row(mode, toggle, msaa, f"{room_name} {room}", "FAIL", dead)
                continue
            for i, (room_name, room) in enumerate(self.rooms, 1):
                label = f"{room_name} {room}"
                if not alive(name):
                    dead = self.ensure_started(name, mode, toggle, msaa)
                if dead:
                    self.row(mode, toggle, msaa, label, "FAIL", dead)
                    continue
                pos = log_size(name)
                out = self.out_path(mode, toggle, msaa, i, room_name)
                out.parent.mkdir(parents=True, exist_ok=True)
                r = rig(self.shot_args(name, out, room))
                fail, warn = classify(log_since(name, pos))
                running = alive(name)
                if fail is None and not running:
                    fail = f"game process died ({last_line(r.stdout)})"
                if fail is None and r.returncode:
                    fail = f"mprig shot failed: {last_line(r.stdout)}"
                if fail:
                    self.row(mode, toggle, msaa, label, "FAIL", fail)
                    if not running:
                        self.note(f"{name} died in {label} (msaa {msaa}): "
                                  f"{self.record_crash(name, mode, toggle, msaa, room_name)}")
                    continue
                if not out.is_file():
                    self.row(mode, toggle, msaa, label, "FAIL", "no screenshot was written")
                    continue
                self.row(mode, toggle, msaa, label, "warn" if warn else "ok", warn or "")
        self.note(f"{name}: {time.monotonic() - t0:.0f}s")

    def finish(self):
        if self.summary:
            self.summary.close()
        counts = {}
        for *_, result, _detail in self.rows:
            counts[result] = counts.get(result, 0) + 1
        total = len(self.rows)
        print(f"\n{total} rows: " + ", ".join(f"{k}={counts[k]}" for k in sorted(counts))
              + f" -> {self.evidence / 'summary.tsv'}")
        return 1 if counts.get("FAIL") else 0

    def stop_all(self):
        for name in self.names:
            rig(["stop", name])


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--evidence-dir", required=True,
                   help="where summary.tsv and the screenshots go (created)")
    p.add_argument("--build", help="build dir under build/ or a path (default: rig.ini)")
    p.add_argument("--disc", help="disc image (default: rig.ini / MPRIG_DISC)")
    p.add_argument("--rooms", help="comma-separated room names or MLVL:MREA (default: the tour)")
    p.add_argument("--modes", help="comma-separated: remastered, retail (default: both)")
    p.add_argument("--toggles", help="comma-separated: validate, release (default: both)")
    p.add_argument("--msaa", help="comma-separated MSAA values (default: 1,4)")
    p.add_argument("--settle", type=int, default=30,
                   help="frames to wait before each screenshot (default: 30)")
    p.add_argument("--keep", action="store_true", help="leave the runs going at exit")
    p.add_argument("--dry-run", action="store_true",
                   help="print the mprig commands (only `mprig rooms` runs)")
    args = p.parse_args()

    soak = Soak(args)
    if args.dry_run:
        soak.plan()
        return 0
    if not args.keep:
        atexit.register(soak.stop_all)
    try:
        return soak.soak()
    finally:
        if args.keep:
            print("mpsoak: leaving runs going (--keep): "
                  + " ".join(f"mprig stop {n}" for n in soak.names), file=sys.stderr)
        else:
            soak.stop_all()
            atexit.unregister(soak.stop_all)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
