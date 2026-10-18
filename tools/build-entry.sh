#!/usr/bin/env bash
# Build Stremio Plus for Linux preview or as a native PS5 application.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build"}
BUILD_JOBS=${BUILD_JOBS:-$(nproc)}
export BUILD_DIR BUILD_JOBS
case ${1:-ps5} in
    desktop)
        for tool in cmake pkg-config; do
            command -v "$tool" >/dev/null || { echo "Missing $tool; see BUILDING.md" >&2; exit 2; }
        done
        cmake -S "$ROOT" -B "$BUILD_DIR/desktop" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$BUILD_DIR/desktop" --parallel "$BUILD_JOBS"
        echo "Built $BUILD_DIR/desktop/stremio"
        ;;
    deps)
        bash "$ROOT/tools/setup-toolchain.sh"
        bash "$ROOT/tools/setup-vlc.sh"
        ;;
    ps5)
        bash "$ROOT/tools/setup-toolchain.sh"
        bash "$ROOT/tools/setup-vlc.sh"
        bash "$ROOT/native/build.sh" "${TITLE_ID:-PPSA74126}" "Stremio Plus"
        bash "$ROOT/native/pack.sh" "${TITLE_ID:-PPSA74126}"
        ;;
    *) echo "Usage: ./build.sh [desktop|deps|ps5]" >&2; exit 2 ;;
esac
