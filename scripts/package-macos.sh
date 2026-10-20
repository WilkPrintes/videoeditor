#!/usr/bin/env bash
# Packages a macOS build the way the in-app updater expects it:
#   <out>/VideoEditor-<version>-macos-<arch>.zip and <...>.zip.sha256
# cmake --install runs macdeployqt (Qt and the FFmpeg libraries go into the bundle), then the bundle
# gets an ad-hoc signature (there is no Developer ID yet) and a check that every library it loads
# comes from the bundle or from the system.
#
#   scripts/package-macos.sh <build-dir> <out-dir>
set -euo pipefail

build="${1:?usage: package-macos.sh <build-dir> <out-dir>}"
out="${2:?usage: package-macos.sh <build-dir> <out-dir>}"
version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")"
arch="$(uname -m)"
[ -n "$version" ] || { echo "no project version in $build/CMakeCache.txt"; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cmake --install "$build" --prefix "$stage"
app="$stage/VideoEditor.app"
test -x "$app/Contents/MacOS/VideoEditor"
plist_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist")"
[ "$plist_version" = "$version" ] || { echo "Info.plist says $plist_version, CMake $version"; exit 1; }
ls "$app/Contents/Frameworks" | grep -q '^libavcodec' || { echo "FFmpeg was not deployed into the bundle"; exit 1; }

# Qt comes as a universal build: keep only this machine's architecture (about half the size).
ditto --arch "$arch" "$app" "$app.thin"
rm -rf "$app"
mv "$app.thin" "$app"

codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

outside="$(mktemp)"
while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q 'Mach-O'; then
        # Skip the headers ("<file>:", one per architecture in a universal file) and the library's own id.
        id="$(otool -D "$f" | tail -n +2)"
        otool -L "$f" | grep -v ':$' | awk '{print $1}' | grep -v -x -F -e "${id:-/nonexistent}" \
            | grep -v -E '^(@rpath/|@executable_path/|@loader_path/|/usr/lib/|/System/Library/)' \
            | sed "s|^|$f -> |" >>"$outside" || true
    fi
done < <(find "$app/Contents" -type f -print0)
if [ -s "$outside" ]; then
    echo "libraries loaded from outside the bundle:"
    cat "$outside"
    exit 1
fi

mkdir -p "$out"
zip="VideoEditor-$version-macos-$arch.zip"
rm -f "$out/$zip" "$out/$zip.sha256"
ditto -c -k --keepParent "$app" "$out/$zip"
(cd "$out" && shasum -a 256 "$zip" >"$zip.sha256")
cat "$out/$zip.sha256"
