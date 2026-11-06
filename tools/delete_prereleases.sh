#!/bin/bash
# Delete a branch's pre-releases and their tags (.github/workflows/pre-release.yml).
#
#   tools/delete_prereleases.sh BRANCH
set -euo pipefail
cd "$(dirname "$0")/.."

id=$(tools/prerelease_id.sh "$1")
tags=$(gh release list --limit 1000 --json tagName --jq '.[].tagName')
for tag in $tags; do
    if [[ $tag =~ ^v[0-9.]+-pre-$id$ ]]; then
        gh release delete "$tag" --cleanup-tag --yes
        echo "deleted $tag"
    fi
done
