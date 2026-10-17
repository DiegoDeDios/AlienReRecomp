#!/bin/sh
# Build the macOS release: a universal (Apple Silicon + Intel)
# AlienResurrection.app containing no game code, packaged as
#   build-mac/release/AlienResurrection-<version>-macos.zip
#
# Needs: Xcode command line tools, cmake, ninja, curl. No disc image is
# needed: players point the app at their own copy of the game.
# SDL3 (universal framework) is downloaded from the official release and
# verified. JOBS (default 4) limits parallel compiler processes.
set -e
cd "$(dirname "$0")/.."

SDL_VERSION=3.4.18
SDL_SHA256=c1adf3da91108dbc3e4205da5dacebc203085d283fa89e86eb2c889abd03a853
JOBS=${JOBS:-4}
OUT=build-mac

mkdir -p $OUT/deps
DMG=$OUT/deps/SDL3-$SDL_VERSION.dmg
if [ ! -f "$DMG" ]; then
    curl -fL -o "$DMG.part" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.dmg"
    mv "$DMG.part" "$DMG"
fi
echo "$SDL_SHA256  $DMG" | shasum -a 256 -c - >/dev/null || {
    echo "checksum mismatch for $DMG" >&2
    exit 1
}
SDL_ROOT=$OUT/deps/SDL3-$SDL_VERSION
if [ ! -d "$SDL_ROOT/SDL3.xcframework" ]; then
    MNT=$(mktemp -d)
    hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$DMG" >/dev/null
    mkdir -p "$SDL_ROOT/share"
    cp -R "$MNT/SDL3.xcframework" "$SDL_ROOT/"
    cp -R "$MNT/share/cmake" "$SDL_ROOT/share/"
    cp "$MNT/LICENSE.txt" "$SDL_ROOT/"
    hdiutil detach "$MNT" >/dev/null
    rmdir "$MNT"
fi
SDL_ROOT=$(cd "$SDL_ROOT" && pwd)

cmake -S . -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_CODE=interpreted \
    -DPORT_MACOS_BUNDLE=ON -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_PREFIX_PATH="$SDL_ROOT" >/dev/null
cmake --build $OUT -j "$JOBS"

APP=$OUT/AlienResurrection.app
mkdir -p "$APP/Contents/Frameworks"
rm -rf "$APP/Contents/Frameworks/SDL3.framework"
ditto "$SDL_ROOT/SDL3.xcframework/macos-arm64_x86_64/SDL3.framework" \
    "$APP/Contents/Frameworks/SDL3.framework"
rm -rf "$APP/Contents/Frameworks/SDL3.framework/Headers" \
       "$APP/Contents/Frameworks/SDL3.framework/Versions/A/Headers"
# ad-hoc signature (required on Apple Silicon; not notarized)
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

VERSION=$(sed -n 's/^#define PORT_VERSION "\(.*\)"/\1/p' src/platform/launcher.h)
NAME=AlienResurrection-$VERSION-macos
PKG=$OUT/release/$NAME
rm -rf "$PKG" "$PKG.zip"
mkdir -p "$PKG/licenses"
ditto "$APP" "$PKG/AlienResurrection.app"
cp packaging/macos/README.txt "$PKG/README.txt"
cp LICENSE "$PKG/LICENSE.txt"
cp "$SDL_ROOT/LICENSE.txt" "$PKG/licenses/SDL3.txt"
cp third_party/imgui/LICENSE.txt "$PKG/licenses/imgui.txt"
(cd $OUT/release && ditto -c -k --sequesterRsrc --keepParent "$NAME" "$NAME.zip")
echo "$OUT/release/$NAME.zip"
