#!/usr/bin/env bash
# Rebuild the included subtitle atlas; no console connection or SDK required.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
FONT_BUILD_DIR=${FONT_BUILD_DIR:-"$ROOT/build/font-baker"}
HOST_CXX=${HOST_CXX:-clang++-18}
mkdir -p "$FONT_BUILD_DIR"
"$HOST_CXX" -std=c++20 -O2 -w "$ROOT/tools/bake-subtitle-font.cpp" -o "$FONT_BUILD_DIR/bake-subtitle-font"
"$FONT_BUILD_DIR/bake-subtitle-font" "$ROOT/vendor/ps5-homebrew-ui/third_party/fonts/DejaVuSans.ttf" \
    "$ROOT/app/hui/fonts/subtitles.huifont" 56 8 4096 european
"$FONT_BUILD_DIR/bake-subtitle-font" "$ROOT/third_party/fonts/DejaVuSerif.ttf" \
    "$ROOT/app/hui/fonts/subtitles-serif.huifont" 56 8 4096 european
"$FONT_BUILD_DIR/bake-subtitle-font" "$ROOT/vendor/ps5-homebrew-ui/third_party/fonts/DejaVuSansMono.ttf" \
    "$ROOT/app/hui/fonts/subtitles-mono.huifont" 56 8 4096 european
