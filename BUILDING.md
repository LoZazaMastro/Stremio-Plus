# Building Stremio Plus

The project has a native PS5 target and a Linux development target. Run the
commands below from the repository root. Linux or WSL with Ubuntu 24.04 is
the reference build environment.

## Source delivery

Use [tag 0.5.18](https://github.com/LoZazaMastro/Stremio-Plus/tree/0.5.18)
for the source corresponding to the 0.5.18 native packages. GitHub provides
ZIP and tar.gz downloads of that tag on the
[release page](https://github.com/LoZazaMastro/Stremio-Plus/releases/tag/0.5.18),
or check it out directly:

```sh
git clone --branch 0.5.18 --depth 1 https://github.com/LoZazaMastro/Stremio-Plus.git
cd Stremio-Plus
```

The repository contains the application source and assets, the modified
EVO Player components, vendored UI source, all 24 numbered VLC patches,
dependency recipes and build scripts. Media dependency archive URLs are in
[`vendor/vlc-ps5/sources.json`](vendor/vlc-ps5/sources.json), with exact
checksums in [`vendor/vlc-ps5/sources.sha256`](vendor/vlc-ps5/sources.sha256).
[`tools/setup-vlc.sh`](tools/setup-vlc.sh) downloads those sources into
`.deps/vlc/dl/`, verifies them and applies the retained patches.
[`tools/setup-toolchain.sh`](tools/setup-toolchain.sh) pins the native SDK
and graphics dependencies. See **Dependency versions** below for their
versions and source locations. No separate application-source archive is
required beyond the release tag.

The release also supplies `Stremio-Plus-0.5.18-source.zip` and
`Stremio-Plus-0.5.18-dependency-source.zip` for a local copy of the application
and its verified dependency archives. Extract both into the same directory,
then run the build commands from
`Stremio-Plus-0.5.18-source/repository/Stremio-Plus/`. The sibling
`dependency-sources/` directory contains the pinned upstream archives and
its own checksum list; `provenance/build-receipt.json` preserves the receipt
from the accepted native build. The separate repository ZIP contains the
application repository alone.

The [dependency source index](docs/DEPENDENCY_SOURCES.md) records exact
source revisions and retrieval links for the native SDK, PacBrew libraries,
SDL, LLVM runtimes and OpenGL components. The repository also includes the
exact NASM and FreeType source archives used by this release in
`source-archives/`. Copy them into the appropriate build cache with the
commands below; the normal checksum checks still apply.

## Prerequisites

Install the host compiler, development libraries, and packaging tools:

```sh
sudo apt-get update
sudo apt-get install build-essential clang-18 lld-18 llvm-18 libclang-rt-18-dev ccache \
  cmake ninja-build meson pkg-config git curl wget zip unzip python3-venv \
  autoconf automake libtool gettext autopoint bison flex patch xz-utils \
  libsdl2-dev libfreetype-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libwebp-dev libfribidi-dev libegl-dev libgl-dev libgl1-mesa-dri ffmpeg
```

Native dependencies are downloaded into `.deps/`. The setup script verifies
their checksums and revisions before using them. It uses the public homebrew
SDK; a proprietary Sony SDK is not required.

## Native PS5 build

```sh
mkdir -p .deps/vlc/dl
cp source-archives/nasm-2.16.03.tar.xz source-archives/freetype-2.13.3.tar.xz .deps/vlc/dl/
BUILD_JOBS=4 ./build.sh deps
BUILD_JOBS=4 ./build.sh ps5
```

The `./build.sh deps` command prepares the toolchain and builds the pinned
static VLC engine and its media dependencies. The `./build.sh ps5` command
compiles the app, links the native executable, validates imports and
metadata, signs the executable, and packages the title. Subsequent builds
reuse verified dependencies. Adjust `BUILD_JOBS` for your machine's
available memory.
The native target requires `STREMIO_USE_LIBVLC=ON`; there is no player-engine
selector in the PS5 application. The decoder, application and VLC modules all
use the same FFmpeg 8.1.2 headers and static archives to avoid mixing ABIs.

The video renderer and encoded-audio output include adaptations from
[EVO Player PS5](https://github.com/sainsaji/EVO-PLAYER-PS5), with attribution
and retained license notices in `THIRD_PARTY.md`. These changes use the same
pinned media dependencies. The native link also wraps `sceVideoOutOpen` and
`sceVideoOutClose` so display-refresh selection observes the output owned by
the OpenGL renderer; it does not open another VideoOut port. The Sony Media
Remote input adapter and encoded-audio output are compiled only for the
appropriate platform paths.

| Output | Purpose |
| --- | --- |
| `dist/PPSA74126.ffpfsc` | Native image for a compatible homebrew loader |
| `dist/PPSA74126.zip` | The same application as a folder archive |
| `dist/PPSA74126/` | Uncompressed application directory |
| `build/native/pie.elf` | Matching ELF with source line information for debugging |

The title ID is `PPSA74126`; the displayed app name is **Stremio Plus**.
Version information comes from `CMakeLists.txt` and `app/sce_sys/param.json`,
and the build rejects inconsistent metadata. A build receipt records the
source and packaged file identities. Packaging checks that receipt before
creating the image.

The metadata gate also requires `downloadDataSize` to remain `0`. Appdata and
selected video folders grow on demand; increasing the private-volume field
would reserve storage at installation and is rejected before signing.

To keep a clean build separate from an existing one:

```sh
BUILD_DIR="$PWD/build/clean" OUT_DIR="$PWD/dist-clean" BUILD_JOBS=4 ./build.sh ps5
```

Build scripts prepare local files only. Load the resulting image using your
console's homebrew environment. When deploying the loose application folder,
preserve executable permission on `eboot.bin` and `sce_module/libc.prx`.
Keep `pie.elf` from the same build if you need to investigate a crash.

The native build also compiles and packages `sandbox-elevator.elf` and
`download-writer.elf` with the public Payload SDK. Keep both beside
`eboot.bin`; torrent downloads start the writer through the console's local
ELF loader on port `9021`. These payloads are separate from the native
application executable and are validated for the loader's ELF framing.

## Linux development build

```sh
CMAKE_GENERATOR="Unix Makefiles" BUILD_JOBS=4 ./build.sh desktop
./build/desktop/stremio --base "$PWD/app" --data "$PWD/build/desktop-data"
```

The default desktop target uses SDL2, OpenGL and the host FFmpeg backend. It
runs the same application screens, queue and account logic, making it useful
for development and integration testing. Native builds use libVLC; their
decoder and callback behavior must also be tested with the VLC configuration.
Native display selection, the PS5 hardware video decoder, and controller
light-bar behavior still need testing on the console.

### Test the VLC backend on Linux

Build the same patched VLC core and media dependencies for the host, then
select that prefix for the application build:

```sh
mkdir -p .deps/vlc-host/dl
cp source-archives/nasm-2.16.03.tar.xz source-archives/freetype-2.13.3.tar.xz .deps/vlc-host/dl/
STREMIO_VLC_TARGET=host STREMIO_VLC_ROOT="$PWD/.deps/vlc-host" BUILD_JOBS=4 \
  bash tools/setup-vlc.sh
cmake -S . -B build/host-vlc -G 'Unix Makefiles' \
  -DSTREMIO_USE_LIBVLC=ON -DSTREMIO_VLC_ROOT="$PWD/.deps/vlc-host"
cmake --build build/host-vlc --parallel 4
python3 tests/run_player_tests.py build/host-vlc
python3 tests/run_player_memory_tests.py build/host-vlc
python3 tests/run_player_decoder_recovery_tests.py build/host-vlc
python3 tests/run_player_audio_lifecycle_tests.py build/host-vlc
python3 tests/run_vlc_output_tests.py
```

The setup builds NASM from its pinned source archive. These tests exercise
libVLC demuxing, decoding, media callbacks, and application output with local
fixtures. The PS5 hardware decoder and display modules remain console tests.

## Dependency versions

Versions are pinned for reproducible builds. The repository does not silently
track the newest upstream release.

| Component | Version or revision |
| --- | --- |
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) | `v0.42` |
| [PacBrew ports](https://github.com/ps5-payload-dev/pacbrew-repo) | `v0.40.2` |
| [LLVM](https://llvm.org/) C++ runtimes from the PS5 SDK | `18.1.8` |
| LLVM compiler-rt builtins used by the native build | `18.1.3`, Ubuntu package `1:18.1.3-1` |
| [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) | `v1.0.0` |
| [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) | `4bd942579dd981b3df9c740438489ca6a614ddc0` |
| [VLC PS5](https://github.com/ZiZc3/VLC) | Supplied port `0.4`, exact archive identity in `vendor/vlc-ps5/NOTICE.md` |
| [VideoLAN VLC](https://www.videolan.org/vlc/) | `3.0.24` |
| [FFmpeg](https://ffmpeg.org/) for the native VLC engine | `8.1.2` |
| [dav1d](https://code.videolan.org/videolan/dav1d) | `1.5.4` |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | `f98de734b68b0b980d38ff03a330066afd6ae16d` |
| [zlib](https://github.com/madler/zlib), for the host converter | `1.3.2` |
| [MkPFS](https://github.com/PSBrew/MkPFS) | `6cb8313dfe0c988ac52617794553f343243d3a56` |

The UI sources are vendored. The remaining native dependencies are fetched
by `tools/setup-toolchain.sh` and the pinned boilerplate's packaging setup.
Archive hashes and the OpenGL SDK manifest are checked during setup.
The VLC engine is built by `tools/setup-vlc.sh`, using the retained recipes
and patches in `vendor/vlc-ps5/`. That directory's `sources.json` and
`sources.sha256` pin its media and TLS dependency archives. Downloads are
stored in `.deps/vlc/dl`. To build with previously downloaded sources, place
the exact pinned archives there before running setup; they pass the same
checksum checks. Changes to the dependency recipes invalidate their build
fingerprint.

To use an existing compatible toolchain, set `PS5_PAYLOAD_SDK`,
`PS5_OPENGL_PREFIX`, and `BOILERPLATE_DIR`. The same revision and file checks
apply. `BUILD_DIR`, `OUT_DIR`, and `BUILD_JOBS` control the build location,
output location, and parallelism.

## Tests

Focused checks compile their own test binaries; the player and application
integration checks use the completed desktop build:

```sh
python3 tests/run_download_manager_tests.py
python3 tests/run_download_move_failures_tests.py
python3 tests/run_download_io_tests.py
python3 tests/run_download_writer_posix_at_tests.py
python3 tests/run_download_writer_integration_tests.py
python3 tests/run_metadata_storage_tests.py
python3 tests/run_ps5_storage_tests.py
python3 tests/run_download_directory_tests.py
python3 tests/run_native_fcntl_tests.py
python3 tests/run_native_startup_tests.py
python3 tests/run_native_diagnostics_tests.py
python3 tests/run_native_heap_tests.py
python3 tests/run_native_video_memory_tests.py
python3 tests/run_torrent_memory_tests.py
python3 tests/run_ps5vdec_startup_tests.py
python3 tests/run_torrent_download_integration_tests.py
python3 tests/run_growing_file_tests.py
python3 tests/run_download_bookmark_tests.py
python3 tests/run_torrent_recovery_tests.py
python3 tests/run_torrent_request_handoff_tests.py
python3 tests/run_torrent_receive_batch_tests.py
python3 tests/run_player_tests.py build/desktop
python3 tests/run_player_startup_tests.py build/desktop
python3 tests/run_player_resume_tests.py build/desktop
python3 tests/run_download_app_tests.py build/desktop
python3 tests/run_next_episode_tests.py build/desktop
python3 tests/run_preferences_tests.py build/desktop
python3 tests/run_preferences_tests.py build/desktop --console-policy
python3 tests/run_media_controls_tests.py
python3 tests/run_ps5_audio_tests.py
python3 tests/run_video_gl_tests.py
python3 tests/run_video_gl_allocation_tests.py
python3 tests/run_texture_cache_tests.py
```

The host integration runners expect CMake's Unix Makefiles generator. They
create synthetic media with FFmpeg and use temporary files and controlled
local servers. More focused tests for account sync, add-on handling, artwork,
UI behavior, and diagnostics are available under `tests/`.

The playback startup and resume runners require the libVLC desktop build
described above. The resume fixture delays HTTP input for more than six
seconds and observes decoded video colors and PCM frequencies to verify the
actual media position. Native decoder startup checks compile the production
C allocation path with simulated PS5 calls; they cover allocation ownership
and error recovery, without claiming to exercise console hardware decoding.
Fatal-diagnostic checks run aborts and signals in separate processes and
reject allocator, stdio, and mutex use while recording the fatal event.
The native picture-memory runner checks mapping ownership, size limits,
failure cleanup and concurrent use with real host mappings. The player-memory
runner decodes real 4K Main10 HEVC through the production native arena and
bounded shared direct-memory allocator, with simulated PS5 memory syscalls
and separate application and graphics memory pressure. It validates decoded
planes, actual allocation failures and retained-frame ownership. The decoder
recovery runner injects critical FFmpeg returns into real libVLC playback and
verifies one resume at the last presented position followed by an explicit
error if failure repeats. OpenGL presentation is checked separately by the
player runner.

These checks validate behavior on the build host. Playback compatibility,
download throughput, and filesystem behavior also require testing on the
intended PS5 and homebrew environment.

Media-control checks use simulated Sony input and display calls to verify
remote-button handling, supported refresh modes, output readback and rollback.
The native-audio check uses real host VLC objects and mocked HDMI imports to
exercise receiver negotiation, encoded carriers, PCM restoration and output
failure recovery under AddressSanitizer and UndefinedBehaviorSanitizer.
Its deferred-consumption sink checks submitted-buffer lifetime and output
draining; it models the native contract without reproducing console firmware.
The player audio-lifecycle runner checks that a failed native shutdown blocks
later PCM opens, including a failure reported while the player is stopping.
The OpenGL allocation runner injects exhausted allocations and incomplete
framebuffers into real host GL to verify cleanup and reopening. Texture-cache
checks cover playback budget changes and rejection of stale decoding work.
They complement the native compile and import checks. HDMI audio passthrough,
120 Hz output and controller behavior still require a PS5 connected to the
intended television or receiver.

See [Torrent engine](docs/torrent-engine.md) for the scheduling and storage
model, diagnostic counters, and repeatable TCP performance comparisons.

## Assets

Launcher artwork and baked font atlases are included, so a normal build does
not need image conversion or font baking.

To rebuild the home-screen background from `app/sce_sys/pic0-source.png`,
install `Pillow` and `ispc_texcomp==1.0.1` in a Python virtual environment,
then run:

```sh
python3 tools/make-background.py app/sce_sys/pic0-source.png app/sce_sys/pic0.dds
```

The converter produces a 3840×2160 BC7 DDS from 16:9 artwork. Rebuild the
subtitle font atlases with:

```sh
bash tools/bake-subtitle-font.sh
```

Source fonts and their licenses are included. Component and asset attribution
is recorded in [THIRD_PARTY.md](THIRD_PARTY.md).
