#!/usr/bin/env bash
# Builds the FFmpeg the macOS app ships with: shared libraries, LGPL only (no GPL or non-free parts),
# VideoToolbox and AudioToolbox, and nothing but the system's own libraries (autodetection is off, so
# Homebrew packages never get linked in).
#
#   scripts/build-macos-ffmpeg.sh <prefix> [version]
#
# MACOSX_DEPLOYMENT_TARGET (default 13.0) should match the Qt build. Another version needs its
# SHA-256 in FFMPEG_SHA256.
set -euo pipefail

prefix="${1:?usage: build-macos-ffmpeg.sh <prefix> [version]}"
version="${2:-9.0.2}"
if [ "$version" = "9.0.2" ]; then
    sha256="${FFMPEG_SHA256:-8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e}"
else
    sha256="${FFMPEG_SHA256:?set FFMPEG_SHA256 for FFmpeg $version}"
fi
target="${MACOSX_DEPLOYMENT_TARGET:-13.0}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
curl -sSfL -o "$work/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
echo "$sha256  $work/ffmpeg.tar.xz" | shasum -a 256 -c -
tar -xf "$work/ffmpeg.tar.xz" -C "$work"
cd "$work/ffmpeg-$version"

./configure --prefix="$prefix" \
    --enable-shared --disable-static \
    --disable-gpl --disable-nonfree --disable-autodetect \
    --enable-videotoolbox --enable-audiotoolbox --enable-zlib --enable-bzlib --enable-iconv \
    --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
    --extra-cflags="-mmacosx-version-min=$target" --extra-ldflags="-mmacosx-version-min=$target"
make -j"$(sysctl -n hw.ncpu)"
make install
grep -E "^(license|ARCH|HAVE_VIDEOTOOLBOX|CONFIG_H264_VIDEOTOOLBOX_ENCODER)[ =]" ffbuild/config.mak config.h 2>/dev/null || true
echo "FFmpeg $version installed in $prefix"
