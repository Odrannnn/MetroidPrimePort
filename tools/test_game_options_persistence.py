#!/usr/bin/env python3
"""Rig regression check for globally persisted game audio modes (issue #65).

Requires the normal mprig setup (disc image, Xvfb, and a built port). Uses only
fresh rig user dirs and a disposable MPRIG_USER profile; it never touches the
person's configured user settings or saves. Example:

    python3 tools/test_game_options_persistence.py --disc /path/to/MetroidPrime.iso \
        --build build/port-i65
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MODE_NAMES = ("mono", "stereo", "surround")


def run_rig(env, *args):
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "mprig.py"), *map(str, args)],
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode:
        raise RuntimeError(f"mprig {' '.join(map(str, args))} failed ({result.returncode})")
    return result.stdout


def read_settings(path):
    return dict(
        line.split("=", 1)
        for line in path.read_text().splitlines()
        if line and not line.startswith("#") and "=" in line
    )


def read_mode(settings):
    raw = settings.get("game_options", "")
    data = bytes.fromhex(raw)
    if len(data) != 72:
        raise RuntimeError(f"game_options is {len(data)} bytes, expected 72")
    return (data[64] >> 6) & 3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", required=True, type=Path, help="USA v1.00 disc image")
    parser.add_argument("--build", required=True, type=Path, help="build dir or executable")
    parser.add_argument("--evidence-dir", type=Path, default=ROOT / "build" / "i65")
    parser.add_argument("--prefix", default="i65r", help="short mprig run-name prefix")
    args = parser.parse_args()

    disc = args.disc.resolve()
    build = args.build.resolve()
    evidence = args.evidence_dir.resolve()
    if not disc.is_file():
        parser.error(f"disc image not found: {disc}")
    binary = build / "metroid_prime_port" if build.is_dir() else build
    if not binary.is_file():
        parser.error(f"port executable not found: {binary}")

    evidence.mkdir(parents=True, exist_ok=True)
    profile = evidence / "isolated-profile"
    profile.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({"MPRIG_DISC": str(disc), "MPRIG_USER": str(profile), "MPRIG_BUILD": str(build)})

    def start(name, settings):
        run_rig(
            env,
            "start",
            name,
            "--build",
            build,
            "--disc",
            disc,
            "--settings",
            settings,
            "--mods",
            "none",
            "--room",
            "landing site",
            "--wait",
            "45",
        )
        return ROOT / "build" / "rig" / name / "user" / "port_settings.ini"

    def stop(name):
        run_rig(env, "stop", name)

    default_name = f"{args.prefix}-default"
    default_settings_path = start(default_name, "none")
    try:
        default_settings = read_settings(default_settings_path)
        if read_mode(default_settings) != 1:
            raise RuntimeError("fresh options did not default to stereo (mode 1)")
        (evidence / "default-settings.ini").write_text(default_settings_path.read_text())
    finally:
        stop(default_name)

    base = bytearray.fromhex(read_settings(evidence / "default-settings.ini")["game_options"])
    records = ["fresh defaults: Stereo (mode=1), byte64=0x%02x" % base[64]]

    for mode, label in enumerate(MODE_NAMES):
        encoded = bytearray(base)
        encoded[64] = (encoded[64] & 0x3F) | (mode << 6)
        game_options = encoded.hex()
        fixture = evidence / f"settings-{label}.ini"
        values = read_settings(evidence / "default-settings.ini")
        values["game_options"] = game_options
        fixture.write_text("\n".join(f"{key}={value}" for key, value in values.items()) + "\n")

        name = f"{args.prefix}-{label}"
        saved_path = start(name, fixture)
        try:
            saved = read_settings(saved_path)
            if saved.get("game_options") != game_options or read_mode(saved) != mode:
                raise RuntimeError(f"{label}: Restore+Sync changed persisted game_options")

            output = run_rig(env, "cmd", name, "title", "wait 1000", "status --json")
            status_line = next((line for line in output.splitlines() if line.startswith("{")), None)
            if status_line is None or not json.loads(status_line).get("first_person"):
                raise RuntimeError(f"{label}: title transition did not return to gameplay")
            after_title = read_settings(saved_path)
            if after_title.get("game_options") != game_options or read_mode(after_title) != mode:
                raise RuntimeError(f"{label}: title/new-game flow changed persisted game_options")

            log = ROOT / "build" / "rig" / name / "game.log"
            records.append(
                f"{label}: startup+Sync and title/new-game restart preserved mode={mode}, "
                f"byte64=0x{encoded[64]:02x}; log={log}"
            )
        finally:
            stop(name)

    (evidence / "restart-evidence.txt").write_text("\n".join(records) + "\n")
    print(f"PASS: default Stereo and Mono/Stereo/Dolby Surround survive startup Sync and title flow")
    print(f"Evidence: {evidence / 'restart-evidence.txt'}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, StopIteration) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
