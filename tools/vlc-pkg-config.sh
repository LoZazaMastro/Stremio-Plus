#!/usr/bin/env bash
# Use one FFmpeg ABI for libVLC and the application, alongside pinned SDK ports.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${STREMIO_VLC_ROOT:-"$ROOT/.deps/vlc"}
TARGET=${STREMIO_VLC_TARGET:-ps5}
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$WORK/prefix-$TARGET/lib/pkgconfig:$WORK/prefix-$TARGET/libdata/pkgconfig:$WORK/sdk-pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
exec pkg-config --static "$@"
