#!/usr/bin/env bash
# Stremio Plus: compile, link and sign the current sources as a native title.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain-env.sh"
ROOT=$STREMIO_ROOT
HERE="$ROOT/native"
TITLE=${1:-PPSA74126}
NAME=${2:-"Stremio Plus"}
[[ $TITLE =~ ^PPSA[0-9]{5}$ && -n $NAME ]] || { echo "Invalid title identity" >&2; exit 2; }
APP_FILES=${APP_FILES:-"$ROOT/app"}
SDK=$PS5_PAYLOAD_SDK
HB="$SDK/target/user/homebrew/lib"
BP=$BOILERPLATE_DIR
GL=$PS5_OPENGL_PREFIX
VLC_ROOT=${STREMIO_VLC_ROOT:-"$ROOT/.deps/vlc"}
VLC_PREFIX="$VLC_ROOT/prefix-ps5"
TOOL="$BP/build/host/ps5-native-tool"
B="$BUILD_DIR/native"
OUT=$OUT_DIR
[[ -x $TOOL && -f $BP/runtime/libc.prx ]] || { echo "Run ./build.sh deps first" >&2; exit 2; }
[[ -s $VLC_ROOT/static-ps5/link.rsp ]] || {
    echo "Run tools/setup-vlc.sh before building the VLC title" >&2; exit 2;
}
export STREMIO_VLC_ROOT="$VLC_ROOT"
mkdir -p "$B/tools" "$B/stubs" "$OUT"
PKG_CONFIG_WRAPPER="$B/tools/stremio-app-pkg-config"
printf '#!/usr/bin/env bash\nexec bash %q "$@"\n' "$ROOT/tools/vlc-pkg-config.sh" > "$PKG_CONFIG_WRAPPER"
chmod 0755 "$PKG_CONFIG_WRAPPER"
python3 "$ROOT/tools/validate-app-metadata.py" "$APP_FILES/sce_sys/param.json"
python3 "$ROOT/tools/vlc-build-receipt.py" --verify "$VLC_ROOT" ps5 "$ROOT/vendor/vlc-ps5"
sha256sum "$VLC_ROOT/build-receipt-ps5.json" > "$B/vlc-dependencies.sha256"
python3 "$ROOT/tools/build-receipt.py" --fingerprint "$ROOT" > "$B/source-tree.sha256"

"$SDK/bin/prospero-cmake" -S "$ROOT" -B "$B/cmake" -DCMAKE_BUILD_TYPE=Release \
    -DPS5_NATIVE=ON -DPS5_OPENGL_PREFIX="$GL" -DSTREMIO_USE_LIBVLC=ON \
    -DSTREMIO_VLC_ROOT="$VLC_ROOT" -DPKG_CONFIG_EXECUTABLE="$PKG_CONFIG_WRAPPER"
# pipefail is deliberate: a compiler error can never package an older archive.
cmake --build "$B/cmake" --parallel "$BUILD_JOBS" 2>&1 | tee "$B/compile.log"
[[ -s $B/cmake/libstremio.a ]] || { echo "The Stremio Plus archive was not built" >&2; exit 1; }

CC="$SDK/bin/prospero-clang"
"$SDK/bin/prospero-clang++" -std=c++20 -O2 -fno-exceptions -fno-rtti \
    -c "$BP/tooling/native/app_crt.cpp" -o "$B/app_crt.o"
for source in shims console_curl ps5_modules heap posix_fixes opengl_shims vlc_memory videoout_capture; do
    "$CC" -O2 -I"$SDK/target/user/homebrew/include" -c "$HERE/$source.c" -o "$B/$source.o"
done
"$CC" -shared -nostdlib -fPIC -Wl,-soname,libSceVideodec2.sprx \
    "$HERE/stubs/videodec2.c" -o "$B/stubs/libSceVideodec2.so"
bash "$ROOT/tools/vlc-pkg-config.sh" --static --libs sdl2 freetype2 libavformat libavcodec \
    libavutil libswresample libswscale libcurl libwebp > "$B/dependencies.txt"
mapfile -d '' -t DEPS < <(python3 - "$B/dependencies.txt" <<'PY'
import pathlib, shlex, sys
for flag in shlex.split(pathlib.Path(sys.argv[1]).read_text()):
    if flag.startswith(('-l', '-L')) and flag not in ('-lm', '-lc'):
        print(flag, end='\0')
PY
)
VLC_INPUTS=()
while IFS= read -r item; do
    case $item in
        -Wl,--start-group|-Wl,--end-group|-L*) ;;
        -lm|-lc|-ldl|-lpthread|-latomic) ;;
        -l*)
            if [[ -s $VLC_PREFIX/lib/lib${item#-l}.a ]]; then
                VLC_INPUTS+=("$VLC_PREFIX/lib/lib${item#-l}.a")
            else
                VLC_INPUTS+=("$item")
            fi
            ;;
        *) VLC_INPUTS+=("$item") ;;
    esac
done < "$VLC_ROOT/static-ps5/link.rsp"
VLC_INPUTS+=("$VLC_PREFIX/lib/libps5compat.a")
BUILTINS="$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -s $BUILTINS ]] || { echo "Missing clang builtins; install libclang-rt-18-dev" >&2; exit 2; }
# Payload libc includes inline mmap/mprotect syscalls. Native titles must use
# libkernel's entry points, as in the working ProsperoLight reference.
python3 "$ROOT/tools/native-startup.py" prepare-libc "$SDK/target/lib/libc.a" \
    "$B/libc-native-compat.a" --ar "$SDK/bin/prospero-ar" | tee "$B/native-libc.json"
WRAPS=()
for symbol in fcntl pthread_create pipe abort _Exit malloc free calloc realloc reallocf posix_memalign \
    memalign aligned_alloc valloc malloc_usable_size sceSystemServiceHideSplashScreen \
    sceVideoOutOpen sceVideoOutClose; do
    WRAPS+=("--wrap=$symbol")
done
"$SDK/bin/prospero-lld" --error-limit=0 -T "$HERE/ps5-app.ld" --eh-frame-hdr \
    --version-script "$BP/tooling/native/app-symbols.map" -e _start -o "$B/pie.elf" \
    "${WRAPS[@]}" -L "$VLC_PREFIX/lib" -L "$SDK/target/lib" -L "$HB" -L "$GL/lib" \
    --undefined=ps5_agc_gate2_run \
    --undefined=vlc_static_modules --defsym=__isthreaded=vlc_ps5_isthreaded \
    --allow-multiple-definition --defsym=__cxa_thread_atexit_impl=0 --defsym=__syscall=0 \
    "$B/app_crt.o" "$B/shims.o" "$B/console_curl.o" "$B/ps5_modules.o" \
    "$B/heap.o" "$B/posix_fixes.o" "$B/opengl_shims.o" "$B/vlc_memory.o" "$B/videoout_capture.o" \
    --whole-archive "$B/cmake/libstremio.a" --no-whole-archive \
    --start-group "${VLC_INPUTS[@]}" "$GL/lib/libPS5OpenGL.a" "$HB/libdht.a" "$HB/libminiupnpc.a" \
    "$VLC_PREFIX/lib/libfribidi.a" "${DEPS[@]}" "$SDK/target/lib/libc++.a" \
    "$SDK/target/lib/libc++abi.a" "$SDK/target/lib/libunwind.a" "$B/libc-native-compat.a" \
    "$BUILTINS" --end-group \
    --as-needed "$GL/lib/libSceAgc.so" "$GL/lib/libSceAgcDriver.so" "$SDK"/target/lib/*.so "$B"/stubs/*.so
python3 "$ROOT/tools/native-startup.py" audit "$B/pie.elf" | tee "$B/native-startup-audit.json"
python3 "$HERE/check_imports.py" "$B/pie.elf" "$GL/lib" "$SDK/target/lib" "$B/stubs" \
    | tee "$B/imports.log"
if [[ ${STREMIO_NATIVE_LINK_ONLY:-0} == 1 ]]; then
    echo "Native VLC link and startup imports verified: $B/pie.elf"
    exit 0
fi
python3 "$ROOT/tools/vlc-build-receipt.py" --verify "$VLC_ROOT" ps5 "$ROOT/vendor/vlc-ps5"
sha256sum --check --strict "$B/vlc-dependencies.sha256"
"$TOOL" link --in "$B/pie.elf" --out "$B/eboot.elf" --stub-dir "$SDK/target/lib" \
    --stub "$GL/lib/libSceAgc.so" --stub "$GL/lib/libSceAgcDriver.so" \
    --stub "$B/stubs/libSceVideodec2.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
python3 "$ROOT/tools/native-startup.py" configure-process "$B/eboot.elf" \
    | tee "$B/native-process.json"

# This separate SDK payload is requested by the native filesystem client.
# It is validated independently; the no-inline-syscall gate above applies to
# the native title's pie.elf, not to this loader-run helper.
make -C "$HERE/filesystem/helper" PS5_PAYLOAD_SDK="$SDK" OUTPUT="$B/sandbox-elevator.elf"
python3 "$HERE/filesystem/validate-helper.py" "$B/sandbox-elevator.elf"
make -C "$HERE/download_writer/helper" PS5_PAYLOAD_SDK="$SDK" OUTPUT="$B/download-writer.elf"
python3 "$HERE/filesystem/validate-helper.py" "$B/download-writer.elf"

# Assemble separately; the previous output survives any compile/sign/asset error.
APP=$(mktemp -d "$OUT/.$TITLE.assemble.XXXXXX")
trap 'rm -rf -- "$APP"' EXIT
mkdir -p "$APP/sce_sys" "$APP/sce_module"
python3 "$ROOT/tools/vlc-build-receipt.py" --verify "$VLC_ROOT" ps5 "$ROOT/vendor/vlc-ps5"
sha256sum --check --strict "$B/vlc-dependencies.sha256"
"$TOOL" self --sign --in "$B/eboot.elf" --out "$APP/eboot.bin" --magic 0x1D3D154F
(cd "$BP/runtime" && sha256sum --check --strict libc.prx.sha256)
cp "$BP/runtime/libc.prx" "$APP/sce_module/libc.prx"
cp "$B/sandbox-elevator.elf" "$APP/sandbox-elevator.elf"
cp "$B/download-writer.elf" "$APP/download-writer.elf"
python3 - "$APP_FILES/sce_sys/param.json" "$APP/sce_sys/param.json" "$TITLE" "$NAME" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
title, name = sys.argv[3:]
if data['titleId'] != title:
    data['titleId'] = title
    data['conceptId'] = title[4:]
    data['contentId'] = f'UP9000-{title}_00-STREMIOPS5000001'
localized = data['localizedParameters']
localized.setdefault(localized.get('defaultLanguage', 'en-US'), {})['titleName'] = name
localized.setdefault('en-US', {})['titleName'] = name
pathlib.Path(sys.argv[2]).write_text(json.dumps(data, indent=2) + '\n')
PY
python3 "$ROOT/tools/validate-app-metadata.py" "$APP/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ ! -f $APP_FILES/sce_sys/$asset ]] || cp "$APP_FILES/sce_sys/$asset" "$APP/sce_sys/"
done
bash "$BP/tools/validate-assets.sh" "$APP/sce_sys"
for directory in assets fonts hui licenses; do
    [[ ! -d $APP_FILES/$directory ]] || cp -a "$APP_FILES/$directory" "$APP/"
done
cp "$APP_FILES/ca-bundle.crt" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" "$APP/"
# The PS5 cannot enumerate app0 sound folders; preserve an explicit index.
python3 - "$APP/hui/audio" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
if root.is_dir():
    for folder in [root, *root.rglob('*')]:
        if folder.is_dir():
            names = sorted(p.name for p in folder.iterdir() if p.is_file() and p.name != 'index.txt')
            if names:
                (folder / 'index.txt').write_text('\n'.join(names) + '\n')
PY
# Preserve folder ZIP modes: executable eboot/modules and readable data.
# The PFS packer applies its own read/execute-only inode policy to the image.
find "$APP" -type d -exec chmod 0755 {} +
find "$APP" -type f -exec chmod 0644 {} +
chmod 0755 "$APP/eboot.bin" "$APP/sce_module/libc.prx"
python3 "$ROOT/tools/build-receipt.py" "$ROOT" "$APP" "$B/pie.elf" "$B/source-tree.sha256"
rm -rf -- "$OUT/$TITLE"
mv -- "$APP" "$OUT/$TITLE"
trap - EXIT
rm -f -- "$OUT/$TITLE.zip"
(cd "$OUT" && zip -qr "$TITLE.zip" "$TITLE")
echo "Built $OUT/$TITLE from the current sources."
