#!/usr/bin/env bash
# Fetch public, pinned dependencies into .deps; never change a system SDK.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain-env.sh"
# Archives carry upstream user IDs which need not exist in a rootless container.
export TAR_OPTIONS="${TAR_OPTIONS:-} --no-same-owner"
CACHE="$STREMIO_ROOT/.deps/downloads"
BP_REV=f98de734b68b0b980d38ff03a330066afd6ae16d
SDK_HASH=8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da
PORTS_HASH=a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d
GL_HASH=f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5
GL_MANIFEST=f4b91f672be037fbac3f82494f1225deaf4c227a03f37ac3ffa56abb213b943f
ZLIB_HASH=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
for tool in clang-18 clang++-18 llvm-config-18 llvm-ar-18 ld.lld-18 cmake ninja make pkg-config python3 git curl wget unzip tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; install the host packages listed in BUILDING.md" >&2; exit 2; }
done
mkdir -p "$CACHE"
fetch() {
    local url=$1 file=$2 hash=$3
    if [[ ! -f $file ]] || ! printf '%s  %s\n' "$hash" "$file" | sha256sum --check --status; then
        curl -fL --retry 3 --connect-timeout 20 --output "$file.part" "$url"
        printf '%s  %s\n' "$hash" "$file.part" | sha256sum --check --strict
        mv -- "$file.part" "$file"
    fi
}
if [[ ! -x $PS5_PAYLOAD_SDK/bin/prospero-clang ]]; then
    [[ $PS5_PAYLOAD_SDK == "$STREMIO_ROOT/.deps/ps5-payload-sdk" ]] || {
        echo "The supplied PS5_PAYLOAD_SDK is incomplete: $PS5_PAYLOAD_SDK" >&2; exit 2;
    }
    fetch https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip \
        "$CACHE/ps5-payload-sdk-v0.42.zip" "$SDK_HASH"
    unzip -q -o "$CACHE/ps5-payload-sdk-v0.42.zip" -d "$STREMIO_ROOT/.deps"
fi
if [[ ! -f $PS5_PAYLOAD_SDK/target/user/homebrew/lib/libavcodec.a ]]; then
    [[ $PS5_PAYLOAD_SDK == "$STREMIO_ROOT/.deps/ps5-payload-sdk" ]] || {
        echo "The supplied SDK lacks the PacBrew ports: $PS5_PAYLOAD_SDK" >&2; exit 2;
    }
    fetch https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz \
        "$CACHE/ps5-payload-dev-v0.40.2.tar.gz" "$PORTS_HASH"
    # The archive also holds an older SDK; copy only its port libraries.
    tar -xzf "$CACHE/ps5-payload-dev-v0.40.2.tar.gz" -C "$STREMIO_ROOT/.deps" \
        --strip-components=1 --no-same-owner opt/ps5-payload-sdk/target/user/homebrew
fi
if [[ ! -f $PS5_OPENGL_PREFIX/manifest.sha256 ]]; then
    [[ $PS5_OPENGL_PREFIX == "$STREMIO_ROOT/.deps/ps5-opengl-sdk-1.0.0/sdk" ]] || {
        echo "The supplied PS5_OPENGL_PREFIX has no manifest" >&2; exit 2;
    }
    fetch https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.0/ps5-opengl-sdk-1.0.0.tar.gz \
        "$CACHE/ps5-opengl-sdk-1.0.0.tar.gz" "$GL_HASH"
    tar -xzf "$CACHE/ps5-opengl-sdk-1.0.0.tar.gz" -C "$STREMIO_ROOT/.deps" --no-same-owner \
        ps5-opengl-sdk-1.0.0/sdk ps5-opengl-sdk-1.0.0/LICENSE \
        ps5-opengl-sdk-1.0.0/LICENSES ps5-opengl-sdk-1.0.0/THIRD_PARTY_NOTICES.md
fi
printf '%s  %s\n' "$GL_MANIFEST" "$PS5_OPENGL_PREFIX/manifest.sha256" | sha256sum --check --strict
(cd "$PS5_OPENGL_PREFIX" && sha256sum --check --strict --quiet manifest.sha256)
if [[ ! -d $BOILERPLATE_DIR/.git ]]; then
    mkdir -p "$BOILERPLATE_DIR"
    git -C "$BOILERPLATE_DIR" init -q
    git -C "$BOILERPLATE_DIR" remote add origin https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git
    git -C "$BOILERPLATE_DIR" fetch -q --depth 1 origin "$BP_REV"
    git -C "$BOILERPLATE_DIR" checkout -q --detach FETCH_HEAD
fi
[[ $(git -C "$BOILERPLATE_DIR" rev-parse HEAD) == "$BP_REV" ]] || {
    echo "Boilerplate must be pinned to $BP_REV; use an isolated checkout" >&2; exit 2;
}
git -C "$BOILERPLATE_DIR" diff --quiet || { echo "Boilerplate has modified sources" >&2; exit 2; }
# Seed the bootstrap with the same pinned zlib from its official GitHub release.
ZLIB_DIR="$BOILERPLATE_DIR/.deps/native/zlib"
mkdir -p "$ZLIB_DIR"
fetch https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz \
    "$ZLIB_DIR/zlib-1.3.2.tar.gz" "$ZLIB_HASH"
bash "$BOILERPLATE_DIR/tools/setup-native-dependencies.sh" --skip-sdk
bash "$BOILERPLATE_DIR/tools/build-host-tools.sh"
if [[ ! -f $BOILERPLATE_DIR/runtime/libc.prx ]]; then
    bash "$BOILERPLATE_DIR/tools/rebuild-libc.sh"
fi
(cd "$BOILERPLATE_DIR/runtime" && sha256sum --check --strict libc.prx.sha256)
echo "PS5 SDK v0.42, PacBrew v0.40.2, OpenGL v1.0.0 and native tools are ready."
