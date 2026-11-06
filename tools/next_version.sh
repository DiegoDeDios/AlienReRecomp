#!/bin/bash
# The version the next release gets: the highest vX.Y.Z tag, patch + 1.
set -euo pipefail

latest=$(git tag -l 'v*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -n 1)
IFS=. read -r major minor patch <<< "${latest#v}"
echo "$major.$minor.$((patch + 1))"
