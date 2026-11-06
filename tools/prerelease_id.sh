#!/bin/bash
# The hash in a branch's pre-release tag (v<version>-pre-<hash>).
#
#   tools/prerelease_id.sh BRANCH
set -euo pipefail
printf '%s' "$1" | sha256sum | cut -c1-7
