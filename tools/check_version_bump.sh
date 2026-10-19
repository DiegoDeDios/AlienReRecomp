#!/bin/bash
# Pull request check (.github/workflows/version-check.yml): PORT_VERSION in
# src/platform/launcher.h must be higher than on the target branch and not
# tagged yet, because merging the pull request releases that version
# (.github/workflows/release.yml tags it and drafts the release).
#
#   tools/check_version_bump.sh [BASE]    (default: origin/main)
set -euo pipefail
cd "$(dirname "$0")/.."

BASE=${1:-origin/main}
FILE=src/platform/launcher.h

version_of() { sed -n 's/^#define PORT_VERSION "\(.*\)"/\1/p'; }
fail() {
    echo "::error file=$FILE::$1"
    echo "error: $1" >&2
    exit 1
}

new=$(version_of < "$FILE")
old=$(git show "$BASE:$FILE" | version_of)
[[ $new =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "PORT_VERSION \"$new\" is not MAJOR.MINOR.PATCH"
[[ $old =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "PORT_VERSION on $BASE (\"$old\") is not MAJOR.MINOR.PATCH"

IFS=. read -r major minor patch <<< "$old"
if [ "$new" = "$old" ] || [ "$(printf '%s\n%s\n' "$old" "$new" | sort -V | tail -n 1)" != "$new" ]; then
    fail "PORT_VERSION is $new: raise it above $old (the version on $BASE), e.g. to $major.$minor.$((patch + 1)). Every merged pull request is released as a new version."
fi
if git rev-parse -q --verify "refs/tags/v$new" > /dev/null; then
    fail "v$new is already tagged: use a higher version"
fi
echo "PORT_VERSION $old -> $new: merging releases v$new"
