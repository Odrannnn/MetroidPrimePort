#!/usr/bin/python3
"""mprig: isolated headless runs of the port for debugging (Xvfb + game + debug console).

    mprig.py start t1 --room 83F6FF6F:492CBF4A     # own Xvfb + game; prints "name display port log"
    mprig.py cmd t1 status 'roomenv info'          # one console command per argument
    mprig.py shot t1 /tmp/a.png 'roomenv bloom off'
    mprig.py ab t1 /tmp/x --a 'roomenv bloom off' --b 'roomenv bloom on'
    mprig.py stop --all t                          # your runs whose name starts with "t"

State lives in build/rig/ (git-ignored): build/rig/<name>/{run.json,game.log,user/,shots/}.
Defaults (disc, real user dir, build) come from build/rig.ini [rig], overridden by env
MPRIG_DISC / MPRIG_USER / MPRIG_BUILD (disc also $MP_DISC) and by flags. Every command exits
non-zero with a one-line "mprig: <reason>" on stderr when it fails. Processes are only ever
signalled by the pids recorded in run.json (never by pattern); captures never touch :0.
"""
import argparse
import atexit
import configparser
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RIG = ROOT / "build" / "rig"
LOCKS = RIG / "locks"
CACHE = RIG / "cache"
NOISE = ["MP frame", "prompt ", "mpstream"]  # lines `log` hides unless --raw
FIRST_DISPLAY, FIRST_PORT = 200, 5100
CRASH_RE = re.compile(r"port:\s+(?:#(\d+)\s+)?(?:at\s+)?(\S+)\+0x([0-9a-fA-F]+)")


def die(msg, code=1):
    print(f"mprig: {msg}", file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------- config

def config():
    cp = configparser.ConfigParser()
    cp.read(ROOT / "build" / "rig.ini")
    sec = cp["rig"] if cp.has_section("rig") else {}
    disc = os.environ.get("MPRIG_DISC") or sec.get("disc") or os.environ.get("MP_DISC") or ""
    user = os.environ.get("MPRIG_USER") or sec.get("user") or str(Path.home() / ".local/share/Metroid Prime")
    build = os.environ.get("MPRIG_BUILD") or sec.get("build") or "port-gcc"
    return {"disc": disc, "user": user, "build": build}


def resolve_binary(build):
    p = Path(build)
    if "/" not in build:
        p = ROOT / "build" / build
    p = p.resolve()
    return p / "metroid_prime_port" if p.is_dir() else p


# ---------------------------------------------------------------- process helpers

def proc_start(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
        rest = s[s.rindex(")") + 2:].split()
        return rest[19] if rest[0] != "Z" else None
    except (OSError, ValueError, IndexError):
        return None


def alive(entry):
    """entry = {'pid':, 'st':}: the recorded process still runs (and the pid was not reused)."""
    return bool(entry) and proc_start(entry["pid"]) == entry["st"]


def ident(pid):
    return {"pid": pid, "st": proc_start(pid)}


def kill_entry(entry, group=False, wait=3.0):
    if not alive(entry):
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            (os.killpg if group else os.kill)(entry["pid"], sig)
        except OSError:
            return
        end = time.monotonic() + wait
        while time.monotonic() < end:
            if not alive(entry):
                return
            time.sleep(0.1)


# ---------------------------------------------------------------- registry

def run_dir(name):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        die(f"bad run name '{name}'")
    return RIG / name


def load_run(name):
    f = run_dir(name) / "run.json"
    if not f.exists():
        die(f"no run '{name}' (see: mprig.py ls)")
    return json.loads(f.read_text())


def save_run(run):
    f = Path(run["dir"]) / "run.json"
    tmp = f.with_suffix(".tmp")
    tmp.write_text(json.dumps(run, indent=1))
    tmp.replace(f)


def all_runs():
    out = []
    if RIG.is_dir():
        for d in sorted(RIG.iterdir()):
            f = d / "run.json"
            if f.is_file():
                try:
                    out.append(json.loads(f.read_text()))
                except ValueError:
                    pass
    return out


def game_alive(run):
    return alive(run.get("game"))


def xvfb_alive(run):
    return alive(run.get("xvfb"))


class Lock:
    def __init__(self, name):
        LOCKS.mkdir(parents=True, exist_ok=True)
        self.f = open(LOCKS / name, "w")

    def __enter__(self):
        fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *a):
        fcntl.flock(self.f, fcntl.LOCK_UN)
        self.f.close()


def port_free(port):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def allocate(taken_displays, taken_ports):
    d = FIRST_DISPLAY
    while d in taken_displays or Path(f"/tmp/.X{d}-lock").exists() or Path(f"/tmp/.X11-unix/X{d}").exists():
        d += 1
    p = FIRST_PORT
    while p in taken_ports or not port_free(p):
        p += 1
    return d, p


# ---------------------------------------------------------------- console

def console(port, commands, wait=0.0, timeout=600.0):
    """Send commands; return [(cmd, ok, reply_lines)]. Raises OSError/ConnectionError if unreachable."""
    deadline = time.monotonic() + wait
    while True:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=5)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)
    sock.settimeout(timeout)
    reader = sock.makefile("r", encoding="utf-8", errors="replace")
    out = []
    try:
        for c in commands:
            sock.sendall((c + "\n").encode())
            lines = []
            while True:
                r = reader.readline()
                if not r:
                    lines.append("=> connection closed")
                    break
                r = r.rstrip("\n")
                lines.append(r)
                if r.startswith("=> "):
                    break
            out.append((c, lines[-1] == "=> ok", lines))
            if lines[-1] == "=> connection closed":
                break
    finally:
        sock.close()
    return out


def console_seq(run, commands, echo=True, timeout=600.0):
    """Send commands, waiting for a settled screen after each `warp` (not on --cutscenes runs).
    Returns [(cmd, ok, lines)]; exits 2 if the game is unreachable."""
    if not game_alive(run):
        die(f"run '{run['name']}' is not running (see: mprig.py log {run['name']}; crash {run['name']})", 2)
    segments, cur = [], []
    for c in commands:
        cur.append(c)
        if c.split()[:1] == ["warp"]:
            segments.append(cur)
            cur = []
    if cur:
        segments.append(cur)
    res = []
    for seg in segments:
        try:
            part = console(run["port"], seg, timeout=timeout)
        except (OSError, ConnectionError) as e:
            die(f"console of '{run['name']}' unreachable on port {run['port']}: {e}", 2)
        res += part
        if len(part) < len(seg) or not all(g for _, g, _ in part):
            break
        if seg[-1].split()[:1] == ["warp"] and not run.get("cutscenes") and not wait_settled(run, 60):
            print(f"mprig: WARN: not settled 60s after '{seg[-1]}'", file=sys.stderr)
    return res


def run_console(run, commands, echo=True, timeout=600.0):
    """Print replies; return True when every command succeeded. Exits 2 if unreachable."""
    res = console_seq(run, commands, timeout=timeout)
    ok = True
    for c, good, lines in res:
        if echo:
            if len(commands) > 1:
                print(f"> {c}")
            print("\n".join(lines))
        if not good:
            ok = False
            print(f"mprig: command '{c}' failed: {lines[-1]}", file=sys.stderr)
    if len(res) < len(commands):
        ok = False
        print("mprig: the game closed the connection (crashed? see: mprig.py crash)", file=sys.stderr)
    return ok


def read_cmds(args):
    cmds = list(args.commands)
    if getattr(args, "file", None):
        with open(args.file, encoding="utf-8") as f:
            cmds += [x.strip() for x in f if x.strip() and not x.lstrip().startswith("#")]
    return cmds


def take_shot(run, cmds, settle=0, force=True):
    """Run cmds, optionally wait frames, `shot`; return the BMP path. Unless force, first wait (up to
    20 s) for a settled screen and warn on stderr if it never is (not on --cutscenes runs)."""
    seq = list(cmds) + ([f"wait {settle}"] if settle else []) + ["shot"]
    if not game_alive(run):
        die(f"run '{run['name']}' is not running", 2)
    if not force and not run.get("cutscenes"):
        st = status_json(run, timeout=5)
        # st is None for a frozen game (hold 1) or no world: nothing to wait for
        if st and not is_settled(st) and not wait_settled(run, 20):
            print("mprig: WARN: capturing before the screen settled "
                  "(cinematic or fade-in still running; --force skips the wait)", file=sys.stderr)
    res = console_seq(run, seq)
    for c, good, lines in res:
        if not good:
            die(f"command '{c}' failed: {lines[-1]}")
    if len(res) < len(seq):
        die("the game closed the connection while taking the shot (crashed? see: mprig.py crash)")
    for line in reversed(res[-1][2]):
        m = re.search(r"(\S+\.bmp)", line)
        if m:
            p = Path(m.group(1))
            if not p.is_absolute():
                p = Path(run["dir"]) / p
            if p.exists():
                return p
    die("`shot` printed no screenshot path: " + " | ".join(res[-1][2])[:200])


def bmp_to_png(bmp, out, crop=None):
    from PIL import Image
    img = Image.open(bmp).convert("RGB")
    if crop:
        x, y, w, h = crop
        img = img.crop((x, y, x + w, y + h))
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    bmp.unlink()
    return out


def parse_crop(s):
    try:
        v = [int(x) for x in s.split(",")]
        assert len(v) == 4
        return v
    except (ValueError, AssertionError):
        die(f"bad --crop '{s}' (want x,y,w,h)")


# ---------------------------------------------------------------- crash symbolizing

def find_crash(text):
    """Frames of the last crash block: [(label, module, offset)], plus the header and build lines."""
    lines = text.splitlines()
    idx = [i for i, l in enumerate(lines) if "port: crashed:" in l]
    if not idx:
        return None
    block = lines[idx[-1]:idx[-1] + 80]
    head = block[0].strip()
    rev = None
    frames = []
    for l in block[1:]:
        m = re.search(r"port: build (\S+)", l)
        if m:
            rev = m.group(1)
            continue
        m = CRASH_RE.search(l)
        if m and l.lstrip().startswith("port:"):
            frames.append(("#" + m.group(1) if m.group(1) else "at", m.group(2), int(m.group(3), 16)))
    return {"head": head, "rev": rev, "frames": frames}


def short_path(p):
    p = p.replace(str(ROOT) + "/", "")
    return re.sub(r"^build/[^/]+/\.\./\.\./", "", p)


def symbolize(crash, binary, extra_binary=None):
    """Return printable lines; the binary's own module is resolved with one addr2line call."""
    bins = {Path(binary).name: str(binary)}
    if extra_binary:
        bins[Path(extra_binary).name] = str(extra_binary)
    out = [crash["head"]]
    if crash["rev"] and Path(binary).exists():
        try:
            ver = subprocess.run([str(binary), "--version"], capture_output=True, text=True, timeout=15).stdout
            if crash["rev"][:8] not in ver:
                out.append(f"WARN log build {crash['rev']} differs from {Path(binary).name} --version "
                           f"({ver.strip()[:60]}): symbols may be wrong")
        except (OSError, subprocess.TimeoutExpired):
            pass
    resolved = {}
    for mod, path in bins.items():
        offs = sorted({o for _, m, o in crash["frames"] if m == mod})
        if not offs or not Path(path).exists():
            continue
        r = subprocess.run(["addr2line", "-a", "-f", "-C", "-i", "-e", path] + [hex(o) for o in offs],
                           capture_output=True, text=True)
        cur, pending = None, []
        for l in r.stdout.splitlines():
            if l.startswith("0x") and re.fullmatch(r"0x[0-9a-fA-F]+", l):
                cur = int(l, 16)
                resolved[(mod, cur)] = []
                pending = []
            elif cur is not None:
                if not pending:
                    pending.append(l)
                else:
                    resolved[(mod, cur)].append(f"{pending.pop()} {short_path(l)}")
    for label, mod, off in crash["frames"]:
        syms = resolved.get((mod, off))
        if syms:
            out.append(f"{label} {syms[0]}" + "".join(f"  [inl {s}]" for s in syms[1:]))
        else:
            out.append(f"{label} {mod}+{off:#x}")
    return out


def crash_report(text, binary, extra=None):
    c = find_crash(text)
    if not c:
        return None
    return symbolize(c, binary, extra)


# ---------------------------------------------------------------- subcommands


ROOMS_INC = ROOT / "platform" / "port_ap_world_data.inc"
VIEWS = ROOT / "build" / "rig-views.json"
ROOM_RE = re.compile(r'\{\s*\d+,\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*0x[0-9A-Fa-f]+,\s*"([^"]*)"\s*\}')
HEX_ROOM = re.compile(r"[0-9A-Fa-f]{1,8}(:[0-9A-Fa-f]{1,8})?")


def room_table():
    """[(name, 'MLVL', 'MREA')] parsed from kRooms[] of the AP world data (no copy of the table)."""
    if not ROOMS_INC.is_file():
        die(f"room table not found: {ROOMS_INC}")
    return [(m.group(3), f"{int(m.group(1), 16):08X}", f"{int(m.group(2), 16):08X}")
            for m in ROOM_RE.finditer(ROOMS_INC.read_text())]


def room_name(mlvl, mrea):
    for n, w, r in room_table():
        if w == mlvl.upper() and r == mrea.upper():
            return n
    return None


def resolve_room(text):
    """'MLVL[:MREA]' hex, or a room name (exact, then unique prefix, then unique substring) -> 'MLVL[:MREA]'."""
    if HEX_ROOM.fullmatch(text) and (":" in text or len(text) == 8):
        return text.upper()
    want = text.lower()
    rooms = room_table()
    for pick in (lambda n: n.lower() == want, lambda n: n.lower().startswith(want), lambda n: want in n.lower()):
        hit = [r for r in rooms if pick(r[0])]
        if len(hit) == 1:
            return f"{hit[0][1]}:{hit[0][2]}"
        if hit:
            if len({(h[1], h[2]) for h in hit}) == 1:
                return f"{hit[0][1]}:{hit[0][2]}"
            die(f"room '{text}' is ambiguous: " + "; ".join(f"{n} ({w}:{r})" for n, w, r in hit[:12]))
    die(f"no room named '{text}' (see: mprig.py rooms)")


def cmd_rooms(a):
    want = (a.filter or "").lower()
    for n, w, r in room_table():
        if want in n.lower() or want in f"{w}:{r}".lower():
            print(f"{n}\t{w}:{r}")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "room"


def status_json(run, timeout=10):
    """The parsed `status --json` of a run, or None (not up yet, not in a world, old build, frozen)."""
    try:
        res = console(run["port"], ["status --json"], timeout=timeout)
    except (OSError, ConnectionError, socket.timeout):
        return None
    if not res or not res[0][1]:
        return None
    for line in res[0][2]:
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


def is_settled(st):
    """Gameplay is on screen: first person, no cinematic, the fade-in filter done."""
    return bool(st) and st["first_person"] and not st["cinematic"] and not st["fade"]


class Settled:
    """Settled means two polls >= 12 frames apart are both settled: the first frames of a world
    read first-person with no cinematic and no fade just before the cinematic camera starts."""
    def __init__(self):
        self.first = None

    def feed(self, st):
        if not is_settled(st):
            self.first = None
            return False
        if self.first is None:
            self.first = st["frame"]
            return False
        return st["frame"] - self.first >= 12


def wait_settled(run, timeout, interval=0.2):
    """Poll `status --json` until settled; True on success, False on timeout (or a game that is gone)."""
    end = time.monotonic() + timeout
    sett = Settled()
    while True:
        if not game_alive(run):
            return False
        if sett.feed(status_json(run, timeout=5)):
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(interval)


def load_views():
    return json.loads(VIEWS.read_text()) if VIEWS.is_file() else {}


def get_view(name):
    views = load_views()
    if name not in views:
        die(f"no saved view '{name}' (have: {', '.join(sorted(views)) or 'none'})")
    return views[name]


def apply_view(run, view):
    """Warp to the view's room if it is not the current one, restore the pose, wait settled."""
    st = status_json(run)
    mlvl, mrea = view["room"].split(":")
    if not st or st["mrea"] != mrea or st["world"] != mlvl:
        console_seq(run, [f"warp {mlvl} {mrea}"], echo=False)
    x, y, z = view["pos"]
    cmds = [f"tp {x} {y} {z}", f"face {view['yaw']}"]
    fc = view.get("freecam")
    if fc and fc.get("on"):
        cmds += ["freecam on", f"freecam pos {fc['pos'][0]} {fc['pos'][1]} {fc['pos'][2]}",
                 f"freecam look {fc['yaw']} {fc['pitch']}"]
    else:
        cmds.append("freecam off")
    for c, good, lines in console(run["port"], cmds, timeout=60):
        if not good:
            die(f"restoring view: '{c}' failed: {lines[-1]}")
    if not run.get("cutscenes") and not wait_settled(run, 20):
        print("mprig: WARN: not settled after restoring the view", file=sys.stderr)


def cmd_view(a):
    views = load_views()
    if a.action == "ls":
        for n, v in sorted(views.items()):
            nm = room_name(*v["room"].split(":")) or "?"
            fc = " freecam" if v.get("freecam", {}).get("on") else ""
            print(f"{n}\t{v['room']}\t{nm}\tpos={v['pos']} yaw={v['yaw']}{fc}")
    elif a.action == "rm":
        if not a.args or a.args[0] not in views:
            die("no such view")
        del views[a.args[0]]
        VIEWS.write_text(json.dumps(views, indent=1) + "\n")
    elif a.action == "save":
        if len(a.args) != 2:
            die("usage: view save <run> <viewname>")
        run = load_run(a.args[0])
        st = status_json(run)
        if not st:
            die("no `status --json` from the run (not in a world, or a build without it)")
        v = {"room": f"{st['world']}:{st['mrea']}", "pos": st["pos"], "yaw": st["yaw"]}
        if st["freecam"]["on"]:
            v["freecam"] = st["freecam"]
        views[a.args[1]] = v
        VIEWS.parent.mkdir(parents=True, exist_ok=True)
        VIEWS.write_text(json.dumps(views, indent=1) + "\n")
        print(f"{a.args[1]}\t{v['room']}\tpos={v['pos']} yaw={v['yaw']}")
    else:
        die("usage: view save <run> <name> | ls | rm <name>")


def cmd_start(a):
    run = start_run(a)
    print(f"{run['name']} :{run['display']} {run['port']} {run['log']}")


def start_run(a):
    cfg = config()
    name = a.name
    d = run_dir(name)
    RIG.mkdir(parents=True, exist_ok=True)
    CACHE.mkdir(exist_ok=True)
    disc = a.disc or cfg["disc"]
    if not disc or not Path(disc).is_file():
        die(f"disc image not found ('{disc}'): set disc in build/rig.ini, MPRIG_DISC or --disc")
    binary = resolve_binary(a.build or cfg["build"])
    if not binary.is_file():
        die(f"binary not found: {binary}")
    for kv in a.env:
        if "=" not in kv:
            die(f"bad --env '{kv}' (want K=V)")
    m = re.fullmatch(r"(\d+)x(\d+)", a.size)
    if not m:
        die(f"bad --size '{a.size}' (want WxH)")
    view = get_view(a.at) if getattr(a, "at", None) else None
    if a.room:
        a.room = resolve_room(a.room)
    elif view:
        a.room = view["room"]

    with Lock("alloc.lock"):
        old = d / "run.json"
        if old.exists():
            prev = json.loads(old.read_text())
            if game_alive(prev):
                if not a.replace:
                    die(f"run '{name}' is already running (use --replace or stop it)")
                stop_run(prev, quiet=True)
        if d.exists():
            shutil.rmtree(d)
        live = [r for r in all_runs() if game_alive(r) or xvfb_alive(r) or r.get("state") == "starting"]
        display, port = allocate({r["display"] for r in live}, {r["port"] for r in live})
        (d / "user").mkdir(parents=True)
        (d / "shots").mkdir()
        run = {"name": name, "dir": str(d), "display": display, "port": port, "binary": str(binary),
               "build": binary.parent.name, "room": a.room, "disc": disc, "user": str(d / "user"),
               "log": str(d / "game.log"), "started": time.time(), "state": "starting",
               "xvfb": None, "game": None, "env": a.env, "owner": os.getpid(), "cutscenes": bool(a.cutscenes)}
        save_run(run)

    ok = False

    def cleanup():
        if not ok:
            kill_entry(run.get("game"), group=True)
            kill_entry(run.get("xvfb"))
            run["state"] = "failed"
            save_run(run)
    atexit.register(cleanup)
    for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, lambda *_: sys.exit(130))

    # user dir: settings + imgui only (the real one is GBs)
    real = Path(cfg["user"])
    user = d / "user"
    if a.settings == "none":
        pass
    else:
        src = Path(a.settings) if a.settings else real / "port_settings.ini"
        if a.settings and not src.is_file():
            die(f"--settings file not found: {src}")
        if src.is_file():
            shutil.copy(src, user / "port_settings.ini")
    ini = user / "port_settings.ini"
    if a.mods != "none" and ini.is_file():
        # The real settings may have the Remastered mod switched off (mods_disabled); a rig run
        # is meant to test it, so it is always on here. --mods none is the retail A/B.
        lines = []
        for l in ini.read_text().splitlines():
            if l.startswith("mods_disabled="):
                rest = [m for m in l.split("=", 1)[1].split(",") if m and m != "remastered-models"]
                if not rest:
                    continue
                l = "mods_disabled=" + ",".join(rest)
            lines.append(l)
        ini.write_text("\n".join(lines) + "\n")
    if not a.cutscenes:
        # Cutscenes become skippable on first sight (and get randomprime's room patches, as
        # with the F1 setting), so MP_SKIP_CUTSCENES skips them instead of fast-forwarding x8.
        ini = user / "port_settings.ini"
        lines = [l for l in (ini.read_text().splitlines() if ini.is_file() else [])
                 if not l.startswith("skippable_cutscenes=")]
        ini.write_text("\n".join(lines + ["skippable_cutscenes=1"]) + "\n")
    if (real / "imgui.ini").is_file():
        shutil.copy(real / "imgui.ini", user / "imgui.ini")
    if a.saves:
        for sub in ("USA", "savestates"):
            if (real / sub).is_dir():
                shutil.copytree(real / sub, user / sub)
    if a.mods == "none":
        mods = d / "emptymods"
        mods.mkdir()
    else:
        mods = Path(a.mods) if a.mods else real / "mods"
    mods = mods.resolve() if mods.exists() else mods

    host = getattr(a, "host_display", False)
    if host:
        # The user's own X display (Xwayland under GNOME): the real GPU driver, DRI3 and all.
        # Xvfb's llvmpipe can't run Dawn's GL backends, so GL work needs this.
        if not os.environ.get("DISPLAY"):
            die("--host-display needs DISPLAY set")
    else:
        xlog = open(d / "xvfb.log", "w")
        xv = subprocess.Popen(["Xvfb", f":{display}", "-screen", "0", f"{a.size}x24", "-ac"],
                              stdout=xlog, stderr=xlog, start_new_session=True)
        run["xvfb"] = ident(xv.pid)
        save_run(run)
        end = time.monotonic() + 15
        while not Path(f"/tmp/.X11-unix/X{display}").exists():
            if xv.poll() is not None or time.monotonic() > end:
                die(f"Xvfb :{display} failed to start: {(d / 'xvfb.log').read_text()[-200:].strip()}")
            time.sleep(0.1)

    env = {k: v for k, v in os.environ.items() if not k.startswith("MP_") and k not in ("WAYLAND_DISPLAY",)}
    env.update({
        "DISPLAY": os.environ["DISPLAY"] if host else f":{display}",
        "XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}", "SDL_AUDIO_DRIVER": "dummy", "SDL_VIDEODRIVER": "x11",
    })
    if host:
        # GNOME rewrites Xwayland's auth file on a shell restart; a long-lived agent shell keeps the old name.
        if not Path(env.get("XAUTHORITY", "")).is_file():
            auths = sorted(Path(f"/run/user/{os.getuid()}").glob(".mutter-Xwaylandauth.*"), key=lambda p: p.stat().st_mtime)
            if auths:
                env["XAUTHORITY"] = str(auths[-1])
    else:
        env["MESA_VK_WSI_DEBUG"] = "sw"  # RADV on Xvfb has no DRI3
    env.update({
        "MP_USER_PATH": str(user), "MP_CACHE_PATH": str(CACHE), "MP_MODS": str(mods),
        "MP_FAST_BOOT": "0" if getattr(a, "front", False) else "1", "MP_DISABLE_AI_AUDIO": "1", "MP_UPDATE_CHECK": "0", "MP_ASPECT": "16:9", "MP_CONSOLE": str(port),
        "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
        # Xvfb presents are CPU copies (MESA_VK_WSI_DEBUG=sw); present one frame in 6. Every frame is
        # still rendered and `shot` reads the EFB. --env AURORA_PRESENT_EVERY=1 to see every frame.
        "AURORA_PRESENT_EVERY": "6",
    })
    if a.room:
        env["MP_BOOT_WORLD"] = a.room
    if not a.cutscenes:
        env["MP_SKIP_CUTSCENES"] = "1"
    # Test runs die to hazards and enemies while an agent reads a screenshot; damage tests opt in.
    env["MP_GODMODE"] = "0" if getattr(a, "mortal", False) else "1"
    if getattr(a, "turbo", None) is not None:
        if not 1 <= a.turbo <= 16:
            die("--turbo takes 1..16 ticks per frame")
        env["MP_TURBO"] = str(a.turbo)
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    argv = [str(binary), disc]
    if a.gdb:
        argv = ["gdb", "-batch", "-ex", "run", "-ex", "bt", "-ex", "thread apply all bt 8", "--args"] + argv
    log = open(d / "game.log", "w")
    game = subprocess.Popen(argv, cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    run["game"] = ident(game.pid)
    run["state"] = "running"
    save_run(run)

    end = time.monotonic() + a.wait
    ready = False
    # with --room, wait for gameplay: an ok status alone comes back during the intro cinematic
    want_play = a.room and not a.cutscenes
    sett = Settled()
    legacy = False  # a build without `status --json`: text status, then a fixed wait
    while time.monotonic() < end:
        if game.poll() is not None:
            break
        try:
            if getattr(a, "front", False):
                # the front end has no game state for `status`; `pointer where` needs none
                res = console(port, ["pointer where"], timeout=10)
                if res and res[0][1]:
                    ready = True
                    break
            elif not legacy:
                res = console(port, ["status --json"], timeout=10)
                if res and not res[0][1] and "unknown" in res[0][2][-1].lower():
                    legacy = True
            if legacy:
                res = console(port, ["status"], timeout=10)
                if res and res[0][1] and (not want_play or "first person 1, cinematic 0" in "\n".join(res[0][2])):
                    ready = True
                    console(port, ["wait 90"], timeout=60)  # a skipped cinematic fades in: ~70 frames
                    break
            elif res and res[0][1]:
                st = json.loads(next(l for l in res[0][2] if l.startswith("{")))
                if not want_play or sett.feed(st):
                    ready = True
                    break
        except (OSError, ConnectionError, ValueError, StopIteration):
            pass
        time.sleep(0.2)
    if not ready:
        text = (d / "game.log").read_text(errors="replace")
        died = game.poll() is not None
        print("\n".join(text.splitlines()[-15:]), file=sys.stderr)
        rep = crash_report(text, binary)
        if rep:
            print("\n".join(rep), file=sys.stderr)
        die(f"'{name}' " + (f"died during startup (exit {game.returncode})" if died
                             else f"console not ready after {a.wait:.0f}s"))
    if view:
        apply_view(run, view)
    ok = True
    return run


def stop_run(run, quiet=False):
    name = run["name"]
    if game_alive(run):
        try:
            console(run["port"], ["quit"], timeout=3)
        except (OSError, ConnectionError, socket.timeout):
            pass
        end = time.monotonic() + 5
        while game_alive(run) and time.monotonic() < end:
            time.sleep(0.2)
        kill_entry(run["game"], group=True)
    kill_entry(run.get("xvfb"))
    run["state"] = "stopped"
    save_run(run)
    log = Path(run["log"])
    if log.exists():
        rep = crash_report(log.read_text(errors="replace"), run["binary"])
        if rep:
            print(f"{name}: crash found in log:\n" + "\n".join(rep))
    if not quiet:
        print(f"{name}: stopped")


def cmd_stop(a):
    if a.all is not None or a.everyone:
        # Other sessions share build/rig/: a bulk stop is scoped to the caller's own name prefix.
        if a.all is not None and not a.all:
            die("--all needs the prefix of your own run names (e.g. --all emucard-); --everyone stops every session's runs")
        runs = [r for r in all_runs() if (game_alive(r) or xvfb_alive(r)) and (a.everyone or r["name"].startswith(a.all))]
        if not runs:
            print("no live runs")
        if a.everyone and runs:
            print("stopping every session's runs: " + " ".join(r["name"] for r in runs), file=sys.stderr)
        for r in runs:
            stop_run(r)
        return
    if not a.name:
        die("give a run name or --all PREFIX")
    stop_run(load_run(a.name))


def cmd_cmd(a):
    cmds = read_cmds(a)
    if not cmds:
        die("no commands (give them as arguments or -f FILE)")
    sys.exit(0 if run_console(load_run(a.name), cmds) else 1)


def cmd_shot(a):
    run = load_run(a.name)
    crop = parse_crop(a.crop) if a.crop else None
    if a.at:
        apply_view(run, get_view(a.at))
    bmp = take_shot(run, a.commands, a.settle, force=a.force)
    print(bmp_to_png(bmp, a.out, crop))


def cmd_shots(a):
    outdir = Path(a.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    targets = ([("@" + a.at)] if a.at else []) + list(a.rooms)
    if not targets:
        die("give at least one room (name, MLVL[:MREA] or @view)")
    plan = []  # (label, room 'MLVL:MREA', view or None)
    for t in targets:
        if t.startswith("@"):
            v = get_view(t[1:])
            plan.append((t[1:], v["room"], v))
        else:
            r = resolve_room(t)
            plan.append((room_name(*r.split(":")) if ":" in r and room_name(*r.split(":")) else r.replace(":", "-"), r, None))
    if ":" not in plan[0][1]:
        die("shots needs MLVL:MREA (or a room name) for the first room")
    a.room, a.at = plan[0][1], None
    t0 = time.monotonic()
    run = start_run(a)
    print(f"started {run['name']} in {time.monotonic() - t0:.1f}s")
    try:
        for i, (label, room, view) in enumerate(plan):
            t1 = time.monotonic()
            if view:
                apply_view(run, view)
            elif i:
                mlvl, mrea = room.split(":") if ":" in room else (room, None)
                res = console_seq(run, [f"warp {mlvl} {mrea}" if mrea else f"warp {mlvl}"])
                if not res or not res[0][1]:
                    die(f"warp {room} failed: {res[0][2][-1] if res else 'no reply'}")
            cmds = split_cmds(a.cmds) if a.cmds else []
            bmp = take_shot(run, cmds, 0, force=False)
            out = outdir / f"{i + 1:02d}-{slug(label)}.png"
            bmp_to_png(bmp, str(out))
            print(f"{out}\t{room}\t{time.monotonic() - t1:.1f}s")
    finally:
        stop_run(run, quiet=True)


def cmd_pick(a):
    run = load_run(a.name)
    ok = run_console(run, [f"pick {a.x} {a.y}"])
    if a.shot:
        # A normal frame (pick put the view back) with a crosshair where it looked.
        from PIL import Image, ImageDraw
        bmp = take_shot(run, [])
        img = Image.open(bmp).convert("RGB")
        bmp.unlink()
        d = ImageDraw.Draw(img)
        for c, w in (((0, 0, 0), 3), ((255, 0, 255), 1)):
            d.line([(a.x - 12, a.y), (a.x + 12, a.y)], fill=c, width=w)
            d.line([(a.x, a.y - 12), (a.x, a.y + 12)], fill=c, width=w)
        out = Path(a.shot).resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out)
        print(out)
    sys.exit(0 if ok else 1)


def image_stats(pa, pb):
    import numpy as np
    from PIL import Image
    A = Image.open(pa).convert("RGB")
    B = Image.open(pb).convert("RGB")
    if A.size != B.size:
        die(f"size mismatch: {A.size} vs {B.size}")
    x = np.asarray(A).astype(np.int16)
    y = np.asarray(B).astype(np.int16)
    d = np.abs(x - y)
    mad = float(d.mean())
    mse = float((d.astype(np.float64) ** 2).mean())
    psnr = 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    mx = d.max(axis=2)
    ch = mx > 8
    if ch.any():
        ys, xs = np.nonzero(ch)
        bbox = f"{xs.min()},{ys.min()},{xs.max() - xs.min() + 1},{ys.max() - ys.min() + 1}"
    else:
        bbox = "0,0,0,0"
    w = np.array([0.2126, 0.7152, 0.0722])
    la = float((x * w).sum(axis=2).mean())
    lb = float((y * w).sum(axis=2).mean())
    line = (f"mad={mad:.3f} psnr={psnr:.2f} changed={100 * ch.mean():.2f}% bbox={bbox} "
            f"luma_a={la:.1f} luma_b={lb:.1f}")
    return mad, line, mx


def heatmap(mx):
    import numpy as np
    from PIL import Image
    v = np.clip(mx.astype(np.int32) * 4, 0, 255)
    r = v
    g = np.clip(v * 2 - 255, 0, 255)
    b = np.clip(v * 2 - 400, 0, 255)
    return Image.fromarray(np.stack([r, g, b], axis=2).astype(np.uint8))


def cmd_diff(a):
    for p in (a.a, a.b):
        if not Path(p).is_file():
            die(f"no such image: {p}")
    mad, line, mx = image_stats(a.a, a.b)
    if a.out:
        heatmap(mx).save(a.out)
    print(line)
    if a.fail_above is not None and mad > a.fail_above:
        die(f"mad {mad:.3f} above {a.fail_above}", 3)


def captioned(imgs, labels, cols, width):
    from PIL import Image, ImageDraw
    thumbs = []
    for p, lab in zip(imgs, labels):
        im = Image.open(p).convert("RGB")
        h = max(1, round(im.height * width / im.width))
        thumbs.append((im.resize((width, h)), lab))
    cap = 16
    rows = [thumbs[i:i + cols] for i in range(0, len(thumbs), cols)]
    rh = [max(t.height for t, _ in r) + cap for r in rows]
    sheet = Image.new("RGB", (cols * width, sum(rh)), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    y = 0
    for r, h in zip(rows, rh):
        for i, (t, lab) in enumerate(r):
            sheet.paste(t, (i * width, y + cap))
            dr.text((i * width + 4, y + 3), lab, fill=(255, 255, 255))
        y += h
    return sheet


def cmd_sheet(a):
    for p in a.images:
        if not Path(p).is_file():
            die(f"no such image: {p}")
    labels = a.labels.split(",") if a.labels else [Path(p).name for p in a.images]
    if len(labels) != len(a.images):
        die("--labels count differs from the image count")
    cols = a.cols or min(len(a.images), 3)
    captioned(a.images, labels, cols, a.width).save(a.out)
    print(Path(a.out).resolve())


def cmd_ab(a):
    run = load_run(a.name)
    split = lambda s: [c.strip() for c in s.split(";") if c.strip()]
    pre = Path(a.prefix).resolve()
    pre.parent.mkdir(parents=True, exist_ok=True)
    hold = [] if a.no_hold else ["hold 1"]  # freeze game ticks so only the toggle differs
    pa = bmp_to_png(take_shot(run, hold + split(a.a), a.settle), f"{pre}-a.png")
    pb = bmp_to_png(take_shot(run, split(a.b), a.settle), f"{pre}-b.png")
    _, line, mx = image_stats(pa, pb)
    heatmap(mx).save(f"{pre}-diff.png")
    captioned([pa, pb], [f"A: {a.a}", f"B: {a.b}"], 2, 640).save(f"{pre}-ab.png")
    print(line)
    print(f"{pre}-a.png {pre}-b.png {pre}-diff.png {pre}-ab.png")


def split_cmds(s):
    return [c.strip() for c in s.split(";") if c.strip()]


def cmd_film(a):
    """Run --pre, hold the simulation, step to each listed frame and shoot: a labelled filmstrip."""
    run = load_run(a.name)
    frames = sorted({int(x) for x in a.frames.split(",") if x.strip()})
    if not frames or frames[0] < 0:
        die("--frames wants non-negative integers, e.g. 0,4,8,16,32")
    out = Path(a.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    stem = out.with_suffix("")
    if a.pre:
        if not run_console(run, split_cmds(a.pre), echo=False):
            die("a --pre command failed")
    pngs, at = [], 0
    for i, f in enumerate(frames):
        cmds = ["hold 1"] if i == 0 else []
        if f > at:
            cmds.append(f"step {f - at}")
            at = f
        pngs.append(bmp_to_png(take_shot(run, cmds, 2), f"{stem}-f{f}.png"))
    labels = [f"frame {f}" for f in frames]
    cols = a.cols or min(len(frames), 3)
    captioned(pngs, labels, cols, a.width).save(out)
    print(out)
    for f, prev, cur, pf in zip(frames[1:], pngs, pngs[1:], frames):
        print(f"frame {pf} -> {f}: {image_stats(prev, cur)[1]}")
    run_console(run, ["hold 0"], echo=False)


def region_mad(pa, pb, region):
    import numpy as np
    from PIL import Image
    x, y, w, h = region
    A = np.asarray(Image.open(pa).convert("RGB")).astype(np.int16)[y:y + h, x:x + w]
    B = np.asarray(Image.open(pb).convert("RGB")).astype(np.int16)[y:y + h, x:x + w]
    if A.size == 0:
        die(f"--bad-region {region} is outside the image")
    return float(np.abs(A - B).mean())


def cmd_fxbisect(a):
    """Mute each asset in an effect's tree in turn; the culprit changes the bad region most."""
    run = load_run(a.name)
    region = parse_crop(a.bad_region)
    shot_cmds = split_cmds(a.shot_cmds)
    root = a.root.lower().lstrip("#")
    if not game_alive(run):
        die(f"run '{a.name}' is not running", 2)
    pre = Path(a.out_dir).resolve() if a.out_dir else Path(run["dir"]) / "fxbisect"
    pre.mkdir(parents=True, exist_ok=True)

    def shot(label, muted):
        cmds = ["hold 0", "fx mute clear"] + ([f"fx mute {muted}"] if muted else []) + shot_cmds
        return Path(bmp_to_png(take_shot(run, cmds, a.settle), str(pre / f"{label}.png")))

    ref = shot("ref", None)
    floor = region_mad(ref, shot("ref2", None), region)  # the same shot again: camera sway, timing
    res = console(run["port"], ["fx list PART"])[0][2]
    gid = None
    for line in res:
        m = re.match(r"#(\d+) PART (\w{8})", line)
        if m and m.group(2).lower() == root:
            gid = int(m.group(1))  # the newest matching root wins
    if gid is None:
        die(f"no live PART {root} after --shot-cmds (fx list PART printed: {' | '.join(res)[:200]})")
    tree = console(run["port"], [f"fx tree #{gid}"])[0][2]
    assets = []
    for line in tree:
        m = re.match(r"\s*#\d+ (\w{4}) (\w{8}) ", line)
        if m and m.group(2) not in [x[1] for x in assets]:
            assets.append((m.group(1), m.group(2)))
    print(f"tree of #{gid} ({len(assets)} distinct assets):")
    print("\n".join(tree))
    scores = []
    for kind, asset in assets:
        img = shot(f"mute-{asset}", asset)
        scores.append((region_mad(ref, img, region), kind, asset, img))
    run_console(run, ["fx mute clear", "hold 0"], echo=False)
    scores = [(max(0.0, m - floor), k, x, i) for m, k, x, i in scores]
    scores.sort(reverse=True)
    total = sum(sc[0] for sc in scores) or 1.0
    print(f"\nregion {a.bad_region}: difference vs the reference when the asset is muted "
          f"(noise floor {floor:.3f} from an unmuted repeat, already subtracted)")
    for mad, kind, asset, img in scores:
        print(f"  {kind} {asset}  mad={mad:7.3f}  {100 * mad / total:5.1f}%  {img}")
    print(f"culprit: {scores[0][1]} {scores[0][2]}" if scores and scores[0][0] > 0.25
          else "no asset's muting changes the region (is it the shot commands, or the root's own draw?)")
    sheet = pre / "sheet.png"
    top = scores[:5]
    captioned([ref] + [t[3] for t in top], ["reference"] + [f"mute {t[2]}" for t in top], 3, 480).save(sheet)
    print(sheet)


def fmt_age(s):
    s = int(s)
    return f"{s // 3600}h{s % 3600 // 60:02d}m" if s >= 3600 else f"{s // 60}m{s % 60:02d}s"


def cmd_ls(a):
    with Lock("alloc.lock"):
        runs = all_runs()
        for r in runs:
            if not game_alive(r) and xvfb_alive(r):
                kill_entry(r["xvfb"])  # stale: game gone, its recorded Xvfb is not
            if r.get("state") in ("running", "starting") and not game_alive(r):
                r["state"] = "dead"
                save_run(r)
    if not runs:
        print("no runs")
    for r in runs:
        st = "alive" if game_alive(r) else "dead"
        print(f"{r['name']} {st} :{r['display']} {r['port']} {r['build']} room={r.get('room') or '-'} "
              f"up={fmt_age(time.time() - r['started'])}")


def cmd_log(a):
    run = load_run(a.name)
    log = Path(run["log"])
    if not log.exists():
        die(f"no log at {log}")
    lines = log.read_text(errors="replace").splitlines()
    if not a.raw:
        lines = [l for l in lines if not any(n in l for n in NOISE)]
    if a.grep:
        rx = re.compile(a.grep)
        lines = [l for l in lines if rx.search(l)]
    print("\n".join(l[:300] for l in lines[-a.n:]))


def cmd_crash(a):
    binary = Path(a.binary) if a.binary else None
    target = a.target
    if target and Path(target).is_file():
        log = Path(target)
    elif target:
        run = load_run(target)
        log = Path(run["log"])
        binary = binary or Path(run["binary"])
    else:
        runs = [r for r in all_runs() if Path(r["log"]).exists()]
        if not runs:
            die("no runs with a log")
        run = max(runs, key=lambda r: r["started"])
        log = Path(run["log"])
        binary = binary or Path(run["binary"])
    binary = binary or resolve_binary(config()["build"])
    text = log.read_text(errors="replace")
    c = find_crash(text)
    if not c:
        die(f"no 'port: crashed:' block in {log}")
    extra = Path(a.binary) if a.binary else None
    main = binary if not (extra and extra.name.startswith("lib")) else resolve_binary(config()["build"])
    print("\n".join(symbolize(c, main, extra)))


def cmd_doctor(a):
    cfg = config()
    bad = 0

    def rep(level, msg):
        nonlocal bad
        bad += level == "FAIL"
        print(f"{level} {msg}")
    for tool in ("Xvfb", "addr2line", "gdb"):
        p = shutil.which(tool)
        rep("OK" if p else ("WARN" if tool == "gdb" else "FAIL"), f"{tool}: {p or 'missing'}")
    binary = resolve_binary(cfg["build"])
    if binary.is_file():
        try:
            v = subprocess.run([str(binary), "--version"], capture_output=True, text=True, timeout=15)
            rep("OK", f"binary {binary}: {(v.stdout or v.stderr).strip()[:80]}")
        except (OSError, subprocess.TimeoutExpired) as e:
            rep("WARN", f"binary {binary}: --version failed ({e})")
    else:
        rep("FAIL", f"default binary missing: {binary}")
    rep("OK" if Path(cfg["disc"]).is_file() else "FAIL", f"disc: {cfg['disc'] or 'unset'}")
    user = Path(cfg["user"])
    rep("OK" if user.is_dir() else "FAIL", f"real user dir: {user}")
    mod = user / "mods" / "remastered-models"
    cur = current_import_version()
    if not mod.is_dir():
        rep("WARN", "no remastered-models mod installed")
    else:
        st = mod / ".import-version"
        # First line: the version; the second is the commit that imported it.
        have = st.read_text().split("\n", 1)[0].strip() if st.is_file() else None
        if have is None:
            rep("WARN", f"remastered-models has no .import-version (current kImportVersion={cur})")
        elif cur is not None and str(cur) != have:
            rep("WARN", f"remastered-models .import-version={have}, current kImportVersion={cur}: re-import")
        else:
            rep("OK", f"remastered-models .import-version={have} (kImportVersion={cur})")
    st = os.statvfs("/tmp")
    free = st.f_bavail * st.f_frsize / 2 ** 30
    rep("OK" if free > 5 else "WARN", f"/tmp free: {free:.1f} GiB")
    runs = all_runs()
    n = sum(game_alive(r) for r in runs)
    stale = [r["name"] for r in runs if not game_alive(r)]
    rep("OK", f"live rig runs: {n}")
    rep("OK" if len(stale) < 10 else "WARN", f"dead rig runs: {len(stale)} (mprig.py clean)")
    sys.exit(1 if bad else 0)


def current_import_version():
    h = ROOT / "platform/include/port_remastered_import.h"
    try:
        t = h.read_text()
        stages = dict(re.findall(r"inline constexpr int (k\w+) = (\d+);", t.split("namespace ImportStage {")[1]
                                 .split("}  // namespace ImportStage")[0]))
        m = re.search(r"kImportVersion =\s*(\d+)\s*((?:\+\s*ImportStage::\w+\s*)*);", t)
        return int(m.group(1)) + sum(int(stages[k]) for k in re.findall(r"ImportStage::(\w+)", m.group(2)))
    except (OSError, IndexError, KeyError, AttributeError):
        return None


def cmd_clean(a):
    with Lock("alloc.lock"):
        dead = [r for r in all_runs() if not game_alive(r)]
        dead.sort(key=lambda r: r["started"], reverse=True)
        n = 0
        for r in dead[a.keep:]:
            d = Path(r["dir"]).resolve()
            if d.parent != RIG.resolve():
                continue
            kill_entry(r.get("xvfb"))
            shutil.rmtree(d)
            n += 1
    print(f"removed {n} dead runs, kept {min(len(dead), a.keep)}")


def build_parser():
    p = argparse.ArgumentParser(prog="mprig.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="sub", required=True)

    def add(name, fn, help_):
        sp = sub.add_parser(name, help=help_, description=help_)
        sp.set_defaults(fn=fn)
        return sp
    def start_args(s, room_help):
        s.add_argument("--build", help="build dir name under build/ or a path (default: rig.ini build)")
        s.add_argument("--disc")
        s.add_argument("--front", action="store_true", help="boot the real front end (no fast boot); ready once the console answers")
        s.add_argument("--env", action="append", default=[], metavar="K=V")
        s.add_argument("--mods", help="mods dir, or 'none' for an empty one (default: the real user's)")
        s.add_argument("--settings", help="port_settings.ini to use, or 'none' (default: the real user's)")
        s.add_argument("--cutscenes", action="store_true",
                       help="keep cutscenes (default: MP_SKIP_CUTSCENES=1 + skippable_cutscenes=1, which also "
                            "applies randomprime's room patches; with --room, start waits for gameplay)")
        s.add_argument("--mortal", action="store_true",
                       help="the player can take damage and die (default: MP_GODMODE=1, as `god on`)")
        s.add_argument("--turbo", type=int, nargs="?", const=8, metavar="TICKS",
                       help="MP_TURBO: uncapped, TICKS (1..16, default 8) game ticks per drawn frame, so "
                            "scripted runs finish sooner (one tick per frame during screen fades)")
        s.add_argument("--saves", action="store_true", help="copy USA/ and savestates/ from the real user dir")
        s.add_argument("--size", default="1280x720", help="Xvfb screen size WxH")
        s.add_argument("--wait", type=float, default=120, help="seconds to wait for the console")
        s.add_argument("--gdb", action="store_true", help="run under gdb so a crash leaves a backtrace in game.log")
        s.add_argument("--host-display", action="store_true",
                       help="run on the user's $DISPLAY with the real GPU driver instead of Xvfb (GL backends; opens a window)")
        s.add_argument("--replace", action="store_true", help="stop a running run of this name first")

    s = add("start", cmd_start, "start Xvfb + game with a console; prints 'name display port log'")
    s.add_argument("name")
    s.add_argument("--room", help="MLVL[:MREA] hex or a room name (see `rooms`): boot straight into a room")
    s.add_argument("--at", metavar="VIEW", help="a saved view (`view save`): boot into its room and restore its pose")
    start_args(s, "")
    s = add("shots", cmd_shots, "one game, many rooms: warp to each, wait settled, capture <outdir>/<NN>-<room>.png")
    s.add_argument("name")
    s.add_argument("outdir")
    s.add_argument("rooms", nargs="*", help="room names, MLVL:MREA, or @view; the first one is booted into")
    s.add_argument("--cmds", help="';'-separated console commands to run before each capture")
    s.add_argument("--at", metavar="VIEW", help="a saved view, shot first (same as a leading @VIEW)")
    start_args(s, "")
    s = add("rooms", cmd_rooms, "list room names with MLVL:MREA (from platform/port_ap_world_data.inc)")
    s.add_argument("filter", nargs="?")
    s = add("view", cmd_view, "saved camera viewpoints in build/rig-views.json: save <run> <name> | ls | rm <name>")
    s.add_argument("action", choices=["save", "ls", "rm"])
    s.add_argument("args", nargs="*")
    s = add("cmd", cmd_cmd, "send console commands (one per argument, or -f FILE); exit 1 if one fails")
    s.add_argument("name")
    s.add_argument("-f", "--file")
    s.add_argument("commands", nargs="*")
    s = add("shot", cmd_shot, "run commands, take a screenshot, write it as PNG")
    s.add_argument("name")
    s.add_argument("out")
    s.add_argument("commands", nargs="*", help="console commands to run first (one per argument)")
    s.add_argument("--crop", help="x,y,w,h")
    s.add_argument("--settle", type=int, default=0, help="frames to wait before the shot")
    s.add_argument("--force", action="store_true", help="capture at once, without waiting for a settled screen")
    s.add_argument("--at", metavar="VIEW", help="restore a saved view first (warps if the room differs)")
    s = add("pick", cmd_pick, "which draw is at a window pixel: owner, CMDL, material, record, shader hash")
    s.add_argument("name")
    s.add_argument("x", type=int)
    s.add_argument("y", type=int)
    s.add_argument("--shot", metavar="OUT.png", help="also save a frame with a crosshair at x,y")
    s = add("ab", cmd_ab, "A/B two command sets: <prefix>-{a,b,diff,ab}.png + diff stats")
    s.add_argument("name")
    s.add_argument("prefix")
    s.add_argument("--a", required=True, help="';'-separated console commands for A")
    s.add_argument("--b", required=True)
    s.add_argument("--settle", type=int, default=30, help="frames to wait before each shot")
    s.add_argument("--no-hold", action="store_true", help="do not send 'hold 1' (freeze ticks) first")
    s = add("diff", cmd_diff, "image difference stats (mad psnr changed bbox luma)")
    s.add_argument("a")
    s.add_argument("b")
    s.add_argument("--out", help="write an amplified heatmap here")
    s.add_argument("--fail-above", type=float, metavar="MAD", help="exit 3 if mad exceeds this")
    s = add("sheet", cmd_sheet, "contact sheet with captions")
    s.add_argument("out")
    s.add_argument("images", nargs="+")
    s.add_argument("--cols", type=int)
    s.add_argument("--labels", help="comma-separated captions (default: file names)")
    s.add_argument("--width", type=int, default=480, help="thumbnail width")
    s = add("film", cmd_film, "step a held simulation and shoot at the listed frames: labelled filmstrip + per-frame diffs")
    s.add_argument("name")
    s.add_argument("out")
    s.add_argument("--frames", default="0,4,8,16,32", help="comma-separated tick counts after the pre commands")
    s.add_argument("--pre", help="';'-separated console commands to run first (e.g. 'fx 12345678 5')")
    s.add_argument("--cols", type=int)
    s.add_argument("--width", type=int, default=480)
    s = add("fxbisect", cmd_fxbisect, "mute each asset of an effect's tree and rank which one changes a region")
    s.add_argument("name")
    s.add_argument("root", help="root PART asset id (hex) as spawned by --shot-cmds")
    s.add_argument("--shot-cmds", required=True, help="';'-separated commands that spawn the effect (and step/hold)")
    s.add_argument("--bad-region", required=True, help="x,y,w,h of the wrong-looking area")
    s.add_argument("--settle", type=int, default=2, help="frames to wait before each shot")
    s.add_argument("--out-dir", help="where the shots go (default: build/rig/<name>/fxbisect)")
    s = add("stop", cmd_stop, "quit the game, then kill only the recorded processes")
    s.add_argument("name", nargs="?")
    s.add_argument("--all", metavar="PREFIX", nargs="?", const="",
                   help="stop your live runs whose name starts with PREFIX")
    s.add_argument("--everyone", action="store_true", help="stop every live run, other sessions' included")
    add("ls", cmd_ls, "list runs; reaps the Xvfb of dead runs")
    s = add("log", cmd_log, "tail game.log without the noisy lines")
    s.add_argument("name")
    s.add_argument("-n", type=int, default=40)
    s.add_argument("--grep")
    s.add_argument("--raw", action="store_true", help="do not filter noise (" + "|".join(NOISE) + ")")
    s = add("crash", cmd_crash, "symbolize the last 'port: crashed:' block of a run or log file")
    s.add_argument("target", nargs="?", help="run name or log file (default: newest run)")
    s.add_argument("--binary", help="binary to resolve against (also for libmain.so)")
    add("doctor", cmd_doctor, "check tools, binary, disc, user dir, Remastered import stamp, disk")
    s = add("clean", cmd_clean, "delete dead runs under build/rig/")
    s.add_argument("--keep", type=int, default=5)
    return p


def main():
    a = build_parser().parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
