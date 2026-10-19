# Dependency source locations for Stremio Plus 0.5.18

The [0.5.18 source tag](https://github.com/LoZazaMastro/Stremio-Plus/tree/0.5.18)
contains the application source, assets, local modifications, build scripts,
tests and retained license notices for the `PPSA74126` release. GitHub's
source ZIP and tar archive for that tag contain the same repository files.
[BUILDING.md](../BUILDING.md) describes the native build and packaging steps.

This index identifies the additional upstream sources used by the native
build. Download the listed fixed releases or revisions and retain their
original notices. SHA-256 values identify the exact archives used for the
source provenance records; changing a filename does not change its contents.
The built application contains a `build-receipt.json` recording the source
file hashes, linked playback dependency hashes and packaged file hashes.

The release also provides
[the application source bundle](https://github.com/LoZazaMastro/Stremio-Plus/releases/download/0.5.18/Stremio-Plus-0.5.18-source.zip)
and [the dependency source bundle](https://github.com/LoZazaMastro/Stremio-Plus/releases/download/0.5.18/Stremio-Plus-0.5.18-dependency-source.zip).
Extract both into the same parent directory. They share a
`Stremio-Plus-0.5.18-source/` folder containing the application repository,
dependency archives, checksum lists and the unchanged native build receipt.
The separate repository ZIP is a convenience copy of the tagged repository.
The upstream source archives are unchanged from the verified dependency set;
the dependency bundle's delivery README and its checksum identify this release.

## Application, UI and adapted playback components

The preferred form for modifying the application is in `src/` and `native/`.
`vendor/ps5-homebrew-ui/` contains the modified UI sources, while
`vendor/vlc-ps5/` contains the PS5 playback modules, compatibility code,
configuration and patches. Retained third-party headers are in `third_party/`.

Selected playback components were adapted from
[EVO-PLAYER-PS5 by sainsaji and its contributors](https://github.com/sainsaji/EVO-PLAYER-PS5/tree/875f766dc9de762c1a1f608005d0afd1f25116f4),
revision `875f766dc9de762c1a1f608005d0afd1f25116f4`, under GPL-3.0-or-later.
The modified components are included in this repository.
[The component notice](../vendor/evo-player/NOTICE.md) identifies the source
files, local adaptations, upstream authors and original archive digest.
The original copyright notices and complete license texts are retained in
`vendor/evo-player/` and `app/licenses/evo-player/`.
[THIRD_PARTY.md](../THIRD_PARTY.md) covers the other included components.

## VLC and media dependencies

[The source manifest](../vendor/vlc-ps5/sources.json) gives the versions,
download URLs, licenses and SHA-256 values for all 18 source archives used by
the VLC build. [The checksum list](../vendor/vlc-ps5/sources.sha256) can be
used directly with `sha256sum --check` after downloading them into
`.deps/vlc/dl/`.

The repository contains all 24 numbered VLC patches, including the playback
recovery and seek corrections, as well as the retained dependency patches.
[tools/setup-vlc.sh](../tools/setup-vlc.sh),
[vendor/vlc-ps5/configure-vlc.sh](../vendor/vlc-ps5/configure-vlc.sh) and
[vendor/vlc-ps5/deps/build-deps.sh](../vendor/vlc-ps5/deps/build-deps.sh)
are the complete local recipes for applying them and building the libraries.
They select VLC 3.0.24 and FFmpeg 8.1.2; use the matching headers and archives.

Two exact source archives are also retained in the repository so the setup
does not depend on their original download endpoints:

- [NASM 2.16.03](../source-archives/nasm-2.16.03.tar.xz), a build tool.
- [FreeType 2.13.3](../source-archives/freetype-2.13.3.tar.xz), a linked library.

Their original license texts remain inside the unchanged source archives.
Verify the local copies and seed the native build cache before setup:

```bash
(cd source-archives && sha256sum --check SHA256SUMS)
mkdir -p .deps/vlc/dl
cp source-archives/nasm-2.16.03.tar.xz .deps/vlc/dl/
cp source-archives/freetype-2.13.3.tar.xz .deps/vlc/dl/
```

The desktop VLC build uses `.deps/vlc-host/dl/` instead. The remaining
archives are fetched and checksum-checked by the same build recipe.

## SDK, native runtime and package tools

The setup uses PS5 Payload SDK v0.42, PacBrew v0.40.2 and the pinned native
application boilerplate. Their source archives and recipes are available at
these exact revisions:

| Component | Source archive | SHA-256 |
| --- | --- | --- |
| sdk v0.42 | [4eb701204fc3f8d31e84cf8ca272974e2be9c867](https://codeload.github.com/ps5-payload-dev/sdk/tar.gz/4eb701204fc3f8d31e84cf8ca272974e2be9c867) | `fc9f5974e540c4d4af912f0661cf44075651e67a4d0070a6e8ad44e01bee8ac2` |
| pacbrew-repo v0.40.2 | [1687e827675c66e1b040751c7f0811222bd3aea0](https://codeload.github.com/ps5-payload-dev/pacbrew-repo/tar.gz/1687e827675c66e1b040751c7f0811222bd3aea0) | `0b05d8c04597acf3b8a056b22b83b19e017ceac59f3cb1af0c149229997ecbbe` |
| ps5-native-app-boilerplate f98de734b68b | [f98de734b68b0b980d38ff03a330066afd6ae16d](https://codeload.github.com/blackbearreloaded/ps5-native-app-boilerplate/tar.gz/f98de734b68b0b980d38ff03a330066afd6ae16d) | `c937ca4d5ef170d2c0bf2dc8ad98b09321b8b667f4a92397773cdff8a8cb0532` |

The SDK source supplies its libc, headers, startup support and module stubs.
The PacBrew source supplies the corresponding `PKGBUILD` recipes and local
patches for the SDK libraries. The boilerplate source supplies the native
converter, signer, startup implementation and generated `libc.prx` runtime
sources. Its original license notices remain applicable.

The application's [toolchain setup](../tools/setup-toolchain.sh) records the
separate SHA-256 values of the binary SDK and port packages consumed by the
build. Those binary package hashes are distinct from the source archive
hashes above. Packaging uses
[MkPFS revision `6cb8313dfe0c988ac52617794553f343243d3a56`](https://github.com/PSBrew/MkPFS/tree/6cb8313dfe0c988ac52617794553f343243d3a56),
as pinned by the native boilerplate. The host converter's zlib source is
[zlib 1.3.2](https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz),
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`.

## OpenGL runtime and graphics sources

Download the
[ps5-opengl 1.0.0 SDK and source archive](https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.0/ps5-opengl-sdk-1.0.0.tar.gz)
from its [fixed release](https://github.com/blackbearreloaded/ps5-opengl/releases/tag/v1.0.0).
Its SHA-256 is
`f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5`.
The archive includes the runtime and dependency source snapshots, their
licenses, build configuration, checksum records and provenance.

The supplied runtime source revision is
[`dce74910af7f4274c48d05e0ae001b3038462588`](https://github.com/blackbearreloaded/ps5-opengl/tree/dce74910af7f4274c48d05e0ae001b3038462588).
The recorded SDK manifest hash is
`f4b91f672be037fbac3f82494f1225deaf4c227a03f37ac3ffa56abb213b943f`.
Use the source snapshots and patches from that archive when reproducing its
runtime; its `dependencies.json`, `provenance.json` and `runtime-config.txt`
identify the Mesa, OpenGNM and other fixed source inputs and build settings.

## Additional sources for libraries linked from the SDK

The native link also uses the following source versions. Their build recipes
are in the pinned PacBrew source archive above. The SDL revision below is the
one identified by the installed SDK library; it is distinct from the optional
SDL example dependency supplied by the OpenGL SDK.

| Component | Exact source download | SHA-256 |
| --- | --- | --- |
| SDL2 PS5 2.30.12 | [SDL2-ps5-a47182a20e741e4735d1fcd41e8656bf338b0c72.tar.gz](https://codeload.github.com/ps5-payload-dev/SDL/tar.gz/a47182a20e741e4735d1fcd41e8656bf338b0c72) | `6bf2ea6ef41abdd3788aeeb94b9ea47080f9662c0b93730190c89d875db4dec7` |
| libsamplerate 0.2.2 | [libsamplerate-0.2.2.tar.xz](https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz) | `3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893` |
| curl 8.18.0 | [curl-8.18.0.tar.xz](https://curl.haxx.se/download/curl-8.18.0.tar.xz) | `40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946` |
| libpsl 0.21.5 | [libpsl-0.21.5.tar.gz](https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz) | `1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208` |
| OpenSSL 3.5.2 | [openssl-3.5.2.tar.gz](https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz) | `c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec` |
| zstd 1.5.6 | [zstd-1.5.6.tar.gz](https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz) | `8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1` |
| libwebp 1.4.0 | [libwebp-1.4.0.tar.gz](https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.4.0.tar.gz) | `61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5` |
| dht 0.27 | [dht-0.27.tar.gz](https://github.com/jech/dht/archive/refs/tags/dht-0.27.tar.gz) | `caba469a784a5c359c084099fdc025cfe09b1faec2ba9ba257b7384351c43c0a` |
| dht CMake patch 0.27 | [dht-cmake.patch](https://github.com/transmission/dht/commit/b02da598.patch) | `91fb75029bf04456bb7fd9c7cc14d544e906d35a309cc8de5be081049aeb7649` |
| miniupnpc 2.2.4 | [miniupnpc-2.2.4.tar.gz](https://miniupnp.tuxfamily.org/files/miniupnpc-2.2.4.tar.gz) | `481a5e4aede64e9ef29895b218836c3608d973e77a35b4f228ab1f3629412c4b` |
| LLVM C++ runtimes 18.1.8 | [llvm-project-18.1.8.src.tar.xz](https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/llvm-project-18.1.8.src.tar.xz) | `0b58557a6d32ceee97c8d533a59b9212d87e0fc4d2833924eb6c611247db2f2a` |

The DHT CMake patch is an external input named by its PacBrew recipe and is
listed separately because it is needed in addition to the DHT source archive.
The LLVM 18.1.8 archive supplies libc++, libc++abi, libunwind and their shared
CMake/runtime build inputs.

## Host compiler builtins

The linked compiler-rt builtins come from Ubuntu's `libclang-rt-18-dev`
package version `1:18.1.3-1`, separately from the SDK's LLVM 18.1.8 C++ runtime
libraries. The matching compiler-rt source, shared CMake sources and complete
distribution source delta are:

| Source | Download | SHA-256 |
| --- | --- | --- |
| LLVM compiler-rt builtins | [compiler-rt-18.1.3.src.tar.xz](https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.3/compiler-rt-18.1.3.src.tar.xz) | `9a7df9300413696b0c4f7ff1e2729cb82aca375f35c05d698c44f26a4edf1c27` |
| cmake-18.1.3.src.tar.xz | [cmake-18.1.3.src.tar.xz](https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.3/cmake-18.1.3.src.tar.xz) | `acfecb615d41c5b1a0a31e15324994ca06f7a3f37d8958d719b20de0d217b71b` |
| llvm-toolchain-18_18.1.3-1.debian.tar.xz | [llvm-toolchain-18_18.1.3-1.debian.tar.xz](https://archive.ubuntu.com/ubuntu/pool/main/l/llvm-toolchain-18/llvm-toolchain-18_18.1.3-1.debian.tar.xz) | `0afaf17d8eb52c0547c8d152a44e18fd762a4ee1cf6da99237c27d4509a14eb1` |
| Distribution source descriptor | [llvm-toolchain-18_18.1.3-1.dsc](https://archive.ubuntu.com/ubuntu/pool/main/l/llvm-toolchain-18/llvm-toolchain-18_18.1.3-1.dsc) | `e02fd3dd4004a5751040d5fc597334e37049a14a9b685c628b7ecf88a2365b21` |

The distribution descriptor records the complete Ubuntu source package and
its checksums. Its source delta includes the packaging rules and ordered
patch series. These source identities document the inputs; they do not claim
that an arbitrary host environment will reproduce the Ubuntu package byte
for byte.

## Obtaining and modifying the release

Use the matching application tag together with the source locations above.
Local application and playback changes are committed in that tag; no private
patches, account configuration or runtime logs are required to rebuild it.
Run the native build and packaging commands in `BUILDING.md` to produce the
folder ZIP and FFPFSC image. The source and component notices remain available
alongside the release's download instructions, and each component retains its
own license terms.
