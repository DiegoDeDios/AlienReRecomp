#!/bin/sh
# Build dependencies for tools/build_linux.sh on Ubuntu/Debian: toolchain and
# the development packages SDL3 needs for X11, Wayland, audio and input
# (SDL loads the libraries at run time, so players need none of these).
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config curl ca-certificates tar zip file \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-dev libsndio-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
    libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev \
    libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
    libwayland-dev libdecor-0-dev wayland-protocols \
    libdbus-1-dev libibus-1.0-dev libudev-dev liburing-dev
