#!/bin/sh
# Stamp a version into the source before a CI build.
#
#   tools/set_version.sh VERSION
set -e
cd "$(dirname "$0")/.."

perl -pi -e "s/^(#define PORT_VERSION \")[^\"]*\"/\${1}$1\"/" src/platform/launcher.h
grep -F "#define PORT_VERSION \"$1\"" src/platform/launcher.h
