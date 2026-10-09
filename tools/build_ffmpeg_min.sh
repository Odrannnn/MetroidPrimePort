#!/bin/bash
# build_ffmpeg_min.sh <work dir> [extra configure arguments]
#
# Builds the small ffmpeg the Windows package carries for the Remastered movie
# import (platform/port_remastered_ffmpeg.cpp): it reads H.264 in MP4 and
# writes JPEG frames to a pipe, and nothing else. Everything in it is under the
# LGPL (no --enable-gpl, no --enable-nonfree, no external libraries), and it is
# a separate program the port runs, not code the port links.
#
# On x86 it needs nasm. Do not pass --disable-x86asm to get round a missing one: in
# 8.0.1 the plain C path of the colour matrix conversion the import asks for
# writes garbage chroma (seen 2026-10-02), while the assembly build's output
# is byte for byte the distribution ffmpeg's.
#
# The result is <work dir>/ffmpeg[.exe], with the exact source and configure
# line recorded in <work dir>/ffmpeg-build.txt for the licence notice.
set -euo pipefail

version=8.0.1
sha256=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
url=https://ffmpeg.org/releases/ffmpeg-$version.tar.xz

case "$(uname -m)" in
  x86_64|i?86) command -v nasm > /dev/null || { echo "build_ffmpeg_min.sh: nasm not found" >&2; exit 1; } ;;
esac

work=${1:?usage: build_ffmpeg_min.sh <work dir> [configure arguments]}
shift
mkdir -p "$work"
cd "$work"

if [ ! -f ffmpeg-$version.tar.xz ]; then
  curl -fsSL -o ffmpeg-$version.tar.xz "$url"
fi
if command -v sha256sum > /dev/null; then
  echo "$sha256  ffmpeg-$version.tar.xz" | sha256sum -c -
else
  echo "$sha256  ffmpeg-$version.tar.xz" | shasum -a 256 -c -
fi
rm -rf ffmpeg-$version
tar xf ffmpeg-$version.tar.xz

# What the import asks of it: see ConvertMovie(). `format` is the filter ffmpeg
# inserts by itself for -pix_fmt.
configure=(
  --disable-everything --disable-autodetect --disable-doc --disable-debug
  --disable-network --disable-ffplay --disable-ffprobe
  --disable-avdevice --disable-swresample
  --enable-small
  --enable-protocol=file,pipe
  --enable-demuxer=mov
  --enable-parser=h264
  --enable-decoder=h264
  --enable-encoder=mjpeg
  --enable-muxer=image2pipe
  --enable-filter=fps,scale,format
  --enable-swscale
  "$@"
)

cd ffmpeg-$version
./configure "${configure[@]}"
make -j"$(nproc 2> /dev/null || sysctl -n hw.ncpu)"
cd ..

exe=ffmpeg
[ -f ffmpeg-$version/ffmpeg.exe ] && exe=ffmpeg.exe
cp ffmpeg-$version/$exe .
cp ffmpeg-$version/COPYING.LGPLv2.1 .
{
  echo "FFmpeg $version, built for Metroid Prime native port."
  echo "Licence: GNU Lesser General Public License, version 2.1 or later (COPYING.LGPLv2.1)."
  echo "Source: $url"
  echo "sha256: $sha256"
  echo "Unmodified. Build script: tools/build_ffmpeg_min.sh in the port's repository."
  echo "Configure arguments: ${configure[*]}"
} > ffmpeg-build.txt
