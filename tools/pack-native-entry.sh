#!/usr/bin/env bash
# Pack a verified current app folder; file modes are preserved inside the image.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain-env.sh"
TITLE=${1:-PPSA74126}
[[ $TITLE =~ ^PPSA[0-9]{5}$ ]] || { echo "Invalid title ID" >&2; exit 2; }
SRC="$OUT_DIR/$TITLE"
[[ -f $SRC/build-receipt.json && -s $SRC/eboot.bin ]] || {
    echo "Build the current native app before packaging" >&2; exit 2;
}
python3 "$STREMIO_ROOT/tools/build-receipt.py" --verify "$STREMIO_ROOT" "$SRC"
MKPFS=$(bash "$BOILERPLATE_DIR/tools/setup-packaging-dependencies.sh" ffpfsc)
mkdir -p "$BUILD_DIR"
STAGE=$(mktemp -d "$BUILD_DIR/pack.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
cp -a "$SRC" "$STAGE/$TITLE"
# The image is read-only app data; only eboot/modules need execute permission.
find "$STAGE/$TITLE" -type d -exec chmod 0755 {} +
find "$STAGE/$TITLE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/$TITLE/eboot.bin" "$STAGE/$TITLE/sce_module/libc.prx"
TEMP="$OUT_DIR/.$TITLE.ffpfsc.part"
bash "$MKPFS" pack folder --no-adjust-output-file-extension --version PS5 --verify \
    "$STAGE/$TITLE" "$TEMP"
mv -- "$TEMP" "$OUT_DIR/$TITLE.ffpfsc"
echo "Packed $OUT_DIR/$TITLE.ffpfsc ($(stat -c %s "$OUT_DIR/$TITLE.ffpfsc") bytes)"
