#!/bin/sh
# Build the Linux release package inside Docker (e.g. on macOS with Colima or
# Docker Desktop). ARCH: amd64 (default) or arm64. Output as with
# tools/build_linux.sh: build-linux-<arch>/release/*.tar.gz
#
# The build image is Ubuntu 22.04 (glibc 2.35, like the release) plus
# tools/linux/install_deps_ubuntu.sh. It is made with run + commit instead of
# "docker build", which fails for cross-architecture images with the classic
# builder (no buildx needed).
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-amd64}
IMAGE=alien-port-linux-build:$ARCH
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
    docker rm -f alien-port-deps >/dev/null 2>&1 || true
    docker run --platform linux/$ARCH --name alien-port-deps \
        -v "$(pwd)/tools/linux":/deps:ro ubuntu:22.04 sh /deps/install_deps_ubuntu.sh
    docker commit alien-port-deps $IMAGE >/dev/null
    docker rm alien-port-deps >/dev/null
fi
docker run --rm --platform linux/$ARCH --memory 3g -e JOBS="${JOBS:-4}" \
    -v "$(pwd)":/src -w /src $IMAGE sh tools/build_linux.sh
