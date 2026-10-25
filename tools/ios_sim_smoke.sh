#!/usr/bin/env bash
# Starts the simulator build on an iPhone and an iPad simulator (macOS only).
#   tools/ios_sim_smoke.sh <path to metroid_prime_port.app> <out dir>
# CI has no disc image, so this checks the part before one is needed: the app
# installs, starts, brings up the renderer and reaches the disc step. Each
# device leaves <name>.log (the app's console), <name>-data.txt (what it wrote
# to its data folder), <name>-files/ (its logs) and <name>.png (the screen while
# it waits for a disc) in <out dir>. Fails if a device never reaches the disc step.
set -uo pipefail

app=$1
out=$2
id=io.github.odrannnn.metroidprimeport
mkdir -p "$out"
status=0

# The newest available device of each family. The runner image decides which
# models and runtimes exist, so they are looked up rather than named.
pick() {
  xcrun simctl list devices available -j | python3 -c '
import json, re, sys
family = sys.argv[1]
best = None
for runtime, devices in json.load(sys.stdin)["devices"].items():
    if "iOS" not in runtime:
        continue
    for d in devices:
        if d["name"].startswith(family):
            version = tuple(int(n) for n in re.findall(r"\d+", runtime))
            if best is None or version > best[0]:
                best = (version, d["udid"])
print(best[1] if best else "")
' "$1"
}

run_device() {
  local name=$1 udid=$2
  echo "== $name: $(xcrun simctl list devices | grep "$udid" | sed 's/^ *//')"
  xcrun simctl boot "$udid" 2>/dev/null || true
  xcrun simctl bootstatus "$udid" -b > /dev/null
  xcrun simctl install "$udid" "$app" || { echo "$name: install failed" >&2; return 1; }

  # 1. No disc and no dialog: the app should log that and exit by itself. A
  # freshly booted simulator sometimes never starts the first launch (no output,
  # no files), so a silent attempt gets one retry.
  local pid waited attempt
  for attempt in 1 2; do
    SIMCTL_CHILD_MP_NO_DISC_DIALOG=1 xcrun simctl launch --console-pty --terminate-running-process \
      "$udid" "$id" > "$out/$name.log" 2>&1 &
    pid=$! waited=0
    while kill -0 "$pid" 2>/dev/null && [ $waited -lt 180 ]; do sleep 1; waited=$((waited + 1)); done
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    xcrun simctl terminate "$udid" "$id" 2>/dev/null
    [ -s "$out/$name.log" ] && break
    echo "$name: attempt $attempt printed nothing" >&2
  done

  local data
  data=$(xcrun simctl get_app_container "$udid" "$id" data 2>/dev/null)
  if [ -n "$data" ]; then
    (cd "$data" && find . -type f | sort) > "$out/$name-data.txt"
    mkdir -p "$out/$name-files"
    find "$data" -type f \( -name '*.log' -o -name '*.txt' -o -name '*.ini' \) -size -4M \
      -exec cp {} "$out/$name-files/" \; 2>/dev/null
  fi
  local reached=0
  if grep -qs 'no disc image given' "$out/$name.log" "$out/$name-files/"* ; then
    reached=1
    echo "$name: reached the disc step"
  else
    echo "$name: did not reach the disc step (after ${waited}s)" >&2
    tail -n 60 "$out/$name.log" >&2
  fi

  # 2. As a player would start it: whatever it shows while it waits for a disc.
  xcrun simctl launch --terminate-running-process "$udid" "$id" > /dev/null
  sleep 25
  xcrun simctl io "$udid" screenshot "$out/$name.png" > /dev/null
  xcrun simctl terminate "$udid" "$id" 2>/dev/null
  xcrun simctl shutdown "$udid" 2>/dev/null
  [ $reached = 1 ]
}

for family in iPhone iPad; do
  udid=$(pick "$family")
  if [ -z "$udid" ]; then
    echo "no $family simulator on this runner" >&2
    status=1
    continue
  fi
  run_device "$(echo "$family" | tr '[:upper:]' '[:lower:]')" "$udid" || status=1
done
exit $status
