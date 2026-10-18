#!/usr/bin/env bash
# Repository-local defaults; callers may supply an existing verified toolchain.
STREMIO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-"$STREMIO_ROOT/.deps/ps5-payload-sdk"}
PS5_OPENGL_PREFIX=${PS5_OPENGL_PREFIX:-"$STREMIO_ROOT/.deps/ps5-opengl-sdk-1.0.0/sdk"}
BOILERPLATE_DIR=${BOILERPLATE_DIR:-"$STREMIO_ROOT/.deps/ps5-native-app-boilerplate"}
BUILD_DIR=${BUILD_DIR:-"$STREMIO_ROOT/build"}
OUT_DIR=${OUT_DIR:-"$STREMIO_ROOT/dist"}
BUILD_JOBS=${BUILD_JOBS:-4}
# Pin LLVM selection even when newer compilers also exist on the host.
LLVM_CONFIG=${LLVM_CONFIG:-llvm-config-18}
# The SDK resolves LLVM_CONFIG relative to each CMake scratch directory. Pass
# the executable's absolute path, including when the caller supplied a name.
LLVM_CONFIG=$(command -v -- "$LLVM_CONFIG")
export PS5_PAYLOAD_SDK PS5_OPENGL_PREFIX BOILERPLATE_DIR BUILD_DIR OUT_DIR BUILD_JOBS LLVM_CONFIG
