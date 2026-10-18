#!/usr/bin/env bash
# Stremio Plus build entry point.
set -euo pipefail
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/tools/build-entry.sh" "$@"
