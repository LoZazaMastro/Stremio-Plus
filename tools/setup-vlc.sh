#!/usr/bin/env bash
# Build the pinned VLC engine and its static media dependencies locally.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain-env.sh"
TARGET=${STREMIO_VLC_TARGET:-ps5}
[[ $TARGET == ps5 || $TARGET == host ]] || { echo "Invalid VLC target" >&2; exit 2; }
export VLC_PS5_WORK=${STREMIO_VLC_ROOT:-"$STREMIO_ROOT/.deps/vlc"}
export TARGET BUILD_JOBS
export CMAKE_BUILD_PARALLEL_LEVEL=$BUILD_JOBS
export TAR_OPTIONS="${TAR_OPTIONS:-} --no-same-owner"
VENDOR="$STREMIO_ROOT/vendor/vlc-ps5"
WORK=$VLC_PS5_WORK
PREFIX="$WORK/prefix-$TARGET"
mkdir -p "$WORK/dl" "$WORK/src" "$WORK/logs"

VLC_ARCHIVE="$WORK/dl/vlc-3.0.24.tar.xz"
VLC_SHA=e7cab503d1d7d5849b89d2cf0e1ee60d0ef6d012407791b644b9cfc0cc225fdf
if [[ ! -s $VLC_ARCHIVE ]]; then
    curl -fL --retry 3 --connect-timeout 20 --max-time 300 \
        https://download.videolan.org/pub/videolan/vlc/3.0.24/vlc-3.0.24.tar.xz \
        -o "$VLC_ARCHIVE.part"
    printf '%s  %s\n' "$VLC_SHA" "$VLC_ARCHIVE.part" | sha256sum --check --strict
    mv "$VLC_ARCHIVE.part" "$VLC_ARCHIVE"
fi
printf '%s  %s\n' "$VLC_SHA" "$VLC_ARCHIVE" | sha256sum --check --strict
if [[ ! -s $WORK/src/vlc-3.0.24/configure ]]; then
    tar --no-same-owner -xf "$VLC_ARCHIVE" -C "$WORK/src"
fi

DEP_HASH=$(python3 - "$VENDOR" "$TARGET" <<'PY'
from pathlib import Path
import hashlib, sys
root = Path(sys.argv[1])
h = hashlib.sha256(sys.argv[2].encode())
paths = [root / 'deps/build-deps.sh', root / 'sources.sha256', root / 'pkg-config', root / 'ps5-cross.ini']
paths += sorted((root / 'compat').glob('*.c'))
paths += sorted((root / 'patches/ffmpeg').glob('*.patch'))
for path in paths:
    h.update(path.relative_to(root).as_posix().encode())
    h.update(path.read_bytes())
print(h.hexdigest())
PY
)
if [[ ! -s $WORK/dependencies-$TARGET.sha256 ]] || \
        [[ $(cat "$WORK/dependencies-$TARGET.sha256") != "$DEP_HASH" ]] || \
        [[ ! -s $PREFIX/lib/libavcodec.a || ! -s $PREFIX/lib/libgnutls.a ]]; then
    bash "$VENDOR/deps/build-deps.sh"
    printf '%s\n' "$DEP_HASH" > "$WORK/dependencies-$TARGET.sha256"
fi

if [[ $TARGET == ps5 ]]; then
    # The SDK's .pc files use target paths. VLC's .pc files use this local prefix.
    # Normalize the SDK files before combining them; a global pkg-config sysroot
    # would incorrectly prefix VLC's already absolute paths a second time.
    python3 - "$PS5_PAYLOAD_SDK" "$WORK/sdk-pkgconfig" <<'PY'
from pathlib import Path
import sys
sdk, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
homebrew = sdk / 'target/user/homebrew'
for directory in ('lib/pkgconfig', 'libdata/pkgconfig'):
    for path in (homebrew / directory).glob('*.pc'):
        text = path.read_text().replace('/opt/ps5-payload-sdk/target/user/homebrew', '@STREMIO_HB@')
        text = text.replace('/user/homebrew', '@STREMIO_HB@').replace('@STREMIO_HB@', str(homebrew))
        (out / path.name).write_text(text)
PY
fi

bash "$VENDOR/configure-vlc.sh" > "$WORK/logs/$TARGET-vlc.log" 2>&1 || {
    tail -60 "$WORK/logs/$TARGET-vlc.log" >&2
    exit 1
}
APP_MODULES="vlc_entry__stremiohttp vlc_entry__stremiosub"
if [[ $TARGET == ps5 ]]; then
    APP_MODULES+=" vlc_entry__ps5http vlc_entry__ps5vdec vlc_entry__stremiovp9"
    export NM="$PS5_PAYLOAD_SDK/bin/prospero-nm"
fi
export APP_MODULES
bash "$VENDOR/gen-static-modules.sh" "$WORK/static-$TARGET"
python3 "$STREMIO_ROOT/tools/vlc-build-receipt.py" "$WORK" "$TARGET" "$VENDOR"
echo "Static VLC 3.0.24 is ready for $TARGET."
