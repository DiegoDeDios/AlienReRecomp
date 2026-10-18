#!/bin/sh
# Build the Linux release: AlienResurrection-<version>-linux-<arch>.tar.gz in
# build-linux-<arch>/release/, containing no game code (GAME_CODE=interpreted):
# players point it at their own disc image.
#
# Needs a C/C++ toolchain, cmake, ninja, curl, tar and SDL3's build
# dependencies (X11, Wayland, audio, udev, dbus development packages;
# tools/linux/install_deps_ubuntu.sh installs them on Ubuntu/Debian). SDL3 is built
# from the official source release (pinned, checksum verified) and shipped
# in lib/ next to the program. Build on an old distribution (the release is
# built on Ubuntu 22.04) so the result runs on newer ones.
# JOBS (default 4) limits parallel compiler processes.
set -e
cd "$(dirname "$0")/.."

SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
JOBS=${JOBS:-4}
ARCH=$(uname -m)
OUT=build-linux-$ARCH

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

mkdir -p $OUT/deps
TARBALL=$OUT/deps/SDL3-$SDL_VERSION.tar.gz
if [ ! -f "$TARBALL" ]; then
    curl -fL -o "$TARBALL.part" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    mv "$TARBALL.part" "$TARBALL"
fi
if [ "$(sha256 "$TARBALL")" != "$SDL_SHA256" ]; then
    echo "checksum mismatch for $TARBALL" >&2
    exit 1
fi
SDL_SRC=$OUT/deps/SDL3-$SDL_VERSION
[ -d "$SDL_SRC" ] || tar -xzf "$TARBALL" -C $OUT/deps
SDL_PREFIX=$(pwd)/$OUT/deps/sdl-install-$ARCH
if [ ! -f "$SDL_PREFIX/lib/cmake/SDL3/SDL3Config.cmake" ]; then
    cmake -S "$SDL_SRC" -B $OUT/deps/sdl-build-$ARCH -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF >/dev/null
    cmake --build $OUT/deps/sdl-build-$ARCH -j "$JOBS"
    cmake --install $OUT/deps/sdl-build-$ARCH >/dev/null
fi

cmake -S . -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_CODE=interpreted \
    -DPORT_LINUX_PACKAGE=ON -DCMAKE_PREFIX_PATH="$SDL_PREFIX" >/dev/null
cmake --build $OUT -j "$JOBS"

VERSION=$(sed -n 's/^#define PORT_VERSION "\(.*\)"/\1/p' src/platform/launcher.h)
NAME=AlienResurrection-$VERSION-linux-$ARCH
PKG=$OUT/release/$NAME
rm -rf "$PKG" "$PKG.tar.gz"
mkdir -p "$PKG/lib" "$PKG/licenses"
cp $OUT/AlienResurrection "$PKG/"
strip "$PKG/AlienResurrection"
cp -L "$SDL_PREFIX/lib/libSDL3.so.0" "$PKG/lib/libSDL3.so.0"
strip --strip-unneeded "$PKG/lib/libSDL3.so.0"
cp packaging/icon/icon_1024.png "$PKG/AlienResurrection.png"
cp packaging/linux/AlienResurrection.desktop "$PKG/"
cp packaging/linux/README.txt "$PKG/README.txt"
cp LICENSE "$PKG/LICENSE.txt"
cp "$SDL_SRC/LICENSE.txt" "$PKG/licenses/SDL3.txt"
cp third_party/imgui/LICENSE.txt "$PKG/licenses/imgui.txt"
(cd $OUT/release && tar -czf "$NAME.tar.gz" "$NAME")
echo "$OUT/release/$NAME.tar.gz"
