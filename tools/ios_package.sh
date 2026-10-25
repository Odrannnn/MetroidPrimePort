#!/usr/bin/env bash
# Finishes the iOS app bundle CMake built and packs it (macOS only).
#   tools/ios_package.sh <build dir> <iphoneos|iphonesimulator> <out dir>
# Adds the icons and the keys CMake's bundle template can't know, signs it ad
# hoc, and for iphoneos writes <out dir>/MetroidPrimePort.ipa. The .ipa is
# unsigned as far as a device is concerned: SideStore, AltStore or Sideloadly
# re-sign it with the player's own Apple ID. The simulator runs the ad hoc
# signed bundle as is.
set -euo pipefail

build=$1
sdk=$2
out=$3
app="$build/metroid_prime_port.app"
[ -d "$app" ] || { echo "no app bundle at $app" >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
icon="$root/packaging/metroid_prime_port.png"

# File names iOS looks up from CFBundleIconFiles (packaging/ios/Info.plist.in).
for spec in AppIcon60x60@2x:120 AppIcon60x60@3x:180 AppIcon76x76@2x~ipad:152 AppIcon83.5x83.5@2x~ipad:167; do
  sips -z "${spec#*:}" "${spec#*:}" "$icon" --out "$app/${spec%:*}.png" > /dev/null
done

plist="$app/Info.plist"
minimum=$(sed -n 's/^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]*=//p' "$build/CMakeCache.txt")
case $sdk in
  iphoneos) platform=iPhoneOS ;;
  iphonesimulator) platform=iPhoneSimulator ;;
  *) echo "unknown sdk $sdk" >&2; exit 1 ;;
esac
plutil -replace MinimumOSVersion -string "${minimum:-16.0}" "$plist"
plutil -replace CFBundleSupportedPlatforms -json "[\"$platform\"]" "$plist"
plutil -replace DTPlatformName -string "$sdk" "$plist"
plutil -lint "$plist"

codesign --force --sign - --timestamp=none "$app"
codesign --verify --strict "$app"

mkdir -p "$out"
out=$(cd "$out" && pwd)
if [ "$sdk" = iphoneos ]; then
  payload=$(mktemp -d)
  mkdir "$payload/Payload"
  cp -R "$app" "$payload/Payload/"
  (cd "$payload" && zip -qry "$out/MetroidPrimePort.ipa" Payload)
  rm -rf "$payload"
else
  cp -R "$app" "$out/"
fi
