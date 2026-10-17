#!/bin/sh
# Build the Windows release: cross-compile with MinGW-w64 and package
#   build-win/release/AlienResurrection-<version>-windows-x64.zip
#
# Needs: the generated code (run tools/regen.sh with your disc first),
# MinGW-w64 (brew install mingw-w64), cmake, ninja, curl, zip.
# SDL3 for Windows is downloaded from the official release and verified.
# JOBS (default 4) limits parallel compiler processes.
set -e
cd "$(dirname "$0")/.."

SDL_VERSION=3.4.18
SDL_SHA256=049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf
JOBS=${JOBS:-4}
OUT=build-win

if [ ! -f src/gen/alien/alien_funcs.h ]; then
    echo "src/gen is missing: run tools/regen.sh first (needs your disc image)" >&2
    exit 1
fi

# SDL3 development package (MinGW)
mkdir -p $OUT/deps
TARBALL=$OUT/deps/SDL3-devel-$SDL_VERSION-mingw.tar.gz
if [ ! -f "$TARBALL" ]; then
    curl -fL -o "$TARBALL.part" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-devel-$SDL_VERSION-mingw.tar.gz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SDL_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null || {
    echo "checksum mismatch for $TARBALL" >&2
    exit 1
}
SDL_ROOT=$OUT/deps/SDL3-$SDL_VERSION
[ -d "$SDL_ROOT" ] || tar -xzf "$TARBALL" -C $OUT/deps
SDL_PREFIX=$(cd "$SDL_ROOT/x86_64-w64-mingw32" && pwd)

cmake -S . -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
    -DCMAKE_PREFIX_PATH="$SDL_PREFIX" >/dev/null
cmake --build $OUT -j "$JOBS"

VERSION=$(sed -n 's/^#define PORT_VERSION "\(.*\)"/\1/p' src/platform/launcher.h)
NAME=AlienResurrection-$VERSION-windows-x64
PKG=$OUT/release/$NAME
rm -rf "$PKG" "$PKG.zip"
mkdir -p "$PKG/licenses"
cp $OUT/AlienResurrection.exe "$PKG/"
x86_64-w64-mingw32-strip "$PKG/AlienResurrection.exe"
cp "$SDL_PREFIX/bin/SDL3.dll" "$PKG/"
cp packaging/windows/README.txt "$PKG/README.txt"
cp LICENSE "$PKG/LICENSE.txt"
cp "$SDL_PREFIX/share/licenses/SDL3/LICENSE.txt" "$PKG/licenses/SDL3.txt"
cp third_party/imgui/LICENSE.txt "$PKG/licenses/imgui.txt"
(cd $OUT/release && zip -qr "$NAME.zip" "$NAME")
echo "$OUT/release/$NAME.zip"
