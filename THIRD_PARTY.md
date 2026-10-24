# Third-party code, tools and assets

This project is licensed under the GNU General Public License v3.0 (see
`LICENSE`), as required by the GPL-licensed application, UI and playback code
it includes and links. It is an unofficial port and is not
affiliated with or endorsed by Stremio.

## Code included in this repository

| Where | What | License |
| --- | --- | --- |
| `native/console_curl.c`, `native/console_curl.h` | libcurl support for PS5 native titles, from [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (BlackBearReloaded; its `gmtime_r` follows Howard Hinnant's public-domain `civil_from_days`) | GPL-3.0-or-later |
| `native/ps5-app.ld` | linker layout, from ps5-native-app-boilerplate's `tooling/native/ps5-pie.ld`, with unwind-table symbols added | GPL-3.0-or-later |
| `native/filesystem/` | Filesystem capability client and one-request helper adapted from ProsperoLight 01.000.080 and ps5-native-app-boilerplate `examples/sandbox-elevation` (BlackBearReloaded), targeting `PPSA74126`. | GPL-3.0-or-later |
| `native/download_writer/` | PS5 sequential download writer and bounded client protocol. The loader-run file-worker architecture follows the console performance findings documented by [ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore) (BlackBearReloaded, revision `aa139cb5c05c50a3f902f1413ae72316fee18daa`). | GPL-3.0-or-later |
| `native/heap.c`, `native/posix_fixes.c`, `native/ps5_modules.c` | approach and parts adapted from the [Kodi port for PS5](https://github.com/VivaLaVent/kodi-ps5) (Team Kodi / VivaLaVent: `shims/native-app/heap_dmem.c`, `thread_stack.c`, `pipe_fallback.c`, `PS5ImeDialog.cpp`) | GPL-2.0-or-later |
| `native/dlmalloc.c` | Doug Lea's malloc 2.8.6 | MIT-0 |
| `third_party/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (Niels Lohmann) | MIT |
| `third_party/stb_image.h` | [stb_image](https://github.com/nothings/stb) 2.30 (Sean Barrett) | MIT / public domain |
| `src/hwdec_ps5.cpp` (hardware video decoding), `native/stubs/videodec2.c` | ported from [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) (`app/engine/media/src/evo_vdec_native.c`, Husam Osman, itself built on an open-source GPL-3.0 PS5 media player); the libSceVideodec2 structures and call sequence come from there and from [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) (BlackBearReloaded) and [SharpProspero](https://github.com/SvenGDK/SharpProspero) (SvenGDK) | GPL-3.0-or-later |
| `src/netstream.cpp` (several connections for big files) | the approach and sizes of Nuvio PS5's `evo_parallel_io.c` | GPL-3.0-or-later |
| `src/app_detail.cpp` (torrent stream set-up) | follows the behaviour of `createTorrent` in [stremio-video](https://github.com/Stremio/stremio-video) (Stremio) | MIT |
| `vendor/vlc-ps5/`, native VLC callback integration in `src/player_vlc*`, VP9 output association in `src/hwdec_ps5.cpp` | [ZiZc3/VLC](https://github.com/ZiZc3/VLC), PS5 port 0.4 of VideoLAN VLC 3.0.24 with selected video-output and VP9 corrections adapted from v0.6.0, retaining Stremio Plus's UI, media inputs and native safety policies | GPL-3.0 for the PS5 project; retained file-specific GPL-2.0-or-later compatibility shims and VideoLAN licenses; exact upstream revision and local changes in `vendor/vlc-ps5/NOTICE.md` |
| `src/video_evo_shaders.h`, Dolby Vision frame metadata, native HDMI audio transport, `src/display_refresh.cpp`, picture-memory policy | Selected algorithms and PS5 API integration adapted from [EVO Player PS5](https://github.com/sainsaji/EVO-PLAYER-PS5), sainsaji and contributors, based on KINGDKAK's ProsperoPlayer. Retained attribution, source archive identity, upstream paths and local changes are in `vendor/evo-player/NOTICE.md`. | GPL-3.0-or-later; AMD FidelityFX FSR 1 MIT and libplacebo component notices retained |
| `src/media_remote.cpp` | Independent input adapter using the Media Remote port, data layout and key definitions documented in `src/video/ps5/SDL_ps5remote.c` from [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL), source blob `615aa00c429335fbdf6726e6f45ab37630162ece` | Application adapter GPL-3.0-or-later; SDL upstream zlib notice retained in `app/licenses/SDL2.txt` |

## Linked into the app (downloaded at build time, not included here)

The native `eboot.bin` statically links these. The build combines the
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and its
[PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) packages
(both GPL-3.0 for their own recipes and code) with the pinned VLC source
dependencies. Every library keeps its own license:

| Component | License |
| --- | --- |
| ps5-payload-sdk `libc.a`, start-up code and system-module stubs | GPL-3.0-or-later; FreeBSD-derived parts BSD |
| ps5-native-app-boilerplate start-up code (`tooling/native/app_crt.cpp`) and the clean-room `sce_module/libc.prx` loader shim shipped with the app | GPL-3.0-or-later |
| [LLVM](https://llvm.org/) libc++, libc++abi and libunwind 18.1.8; compiler-rt builtins 18.1.3 (Ubuntu `1:18.1.3-1`) | Apache-2.0 WITH LLVM-exception |
| [SDL2 PS5](https://github.com/ps5-payload-dev/SDL) 2.30.12, commit `a47182a20e741e4735d1fcd41e8656bf338b0c72`; libsamplerate 0.2.2 | zlib; BSD-2-Clause |
| [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui), BlackBearReloaded | GPL-3.0-or-later |
| [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), Mesa, OpenGNM PSBC | GPL-3.0-or-later; component-specific MIT/BSD and other retained notices |
| [dht](https://github.com/jech/dht) (Juliusz Chroboczek): the built-in torrent engine's DHT | MIT |
| [miniupnpc](https://miniupnp.tuxfamily.org/) 2.2.4, UPnP port mapping | BSD-3-Clause |
| [VLC / libVLC](https://www.videolan.org/vlc/) 3.0.24, VideoLAN and contributors | GPL-2.0-or-later modules; LGPL-2.1-or-later library portions, with retained file-specific notices |
| [FFmpeg](https://ffmpeg.org/) 8.1.2 (libavformat, libavcodec, libavutil, libswresample, libswscale); native decoder-only build with GPL, version3 and nonfree features disabled | LGPL-2.1-or-later |
| [dav1d](https://code.videolan.org/videolan/dav1d), AV1 software decoder | BSD-2-Clause |
| [libebml](https://github.com/Matroska-Org/libebml), [libmatroska](https://github.com/Matroska-Org/libmatroska) | LGPL-2.1-or-later |
| [libdvbpsi](https://www.videolan.org/developers/libdvbpsi.html) | LGPL-2.1-or-later |
| [libass](https://github.com/libass/libass) | ISC |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | MIT-style, with retained component notices |
| [UTF8-CPP](https://github.com/nemtrif/utfcpp) 3.2.5, HarfBuzz's pinned header dependency | Boost Software License 1.0 |
| [GnuTLS](https://www.gnutls.org/) library | LGPL-2.1-or-later; retained component notices |
| [Nettle](https://www.lysator.liu.se/~nisse/nettle/), [GMP](https://gmplib.org/) | LGPL-3.0-or-later / GPL-2.0-or-later dual licensing |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2) | MIT |
| [libcurl](https://curl.se/) | curl license (MIT/X derivative) |
| [OpenSSL](https://www.openssl.org/) | Apache-2.0 |
| [libpsl](https://github.com/rockdaboot/libpsl) | MIT; Public Suffix List data MPL-2.0 |
| [FreeType](https://freetype.org/) | FreeType License (FTL) |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) and libsharpyuv | BSD-3-Clause |
| [zlib](https://zlib.net/) | zlib |
| [zstd](https://github.com/facebook/zstd) | BSD-3-Clause (dual GPL-2.0) |
| [GNU libiconv](https://www.gnu.org/software/libiconv/) | LGPL-2.1-or-later |
| [GNU FriBidi](https://github.com/fribidi/fribidi): right-to-left subtitles (Arabic, Hebrew) | LGPL-2.1-or-later |

## Build tools (downloaded at build time, not included here)

| What | License |
| --- | --- |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): compiler wrappers, headers, sysroot | GPL-3.0-or-later (FreeBSD headers BSD) |
| [LLVM/Clang/lld](https://llvm.org/) 18 | Apache-2.0 WITH LLVM-exception |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (BlackBearReloaded): native-app converter, signer, asset validation; its converter and FSELF writer are derived from [SharpProspero](https://github.com/SvenGDK/SharpProspero) (SvenGDK) | GPL-3.0-or-later; GPL-3.0 |
| [MkPFS](https://github.com/PSBrew/MkPFS) (PSBrew): packs the `.ffpfsc` image | GPL-3.0 |
| [DirectXTex texconv](https://github.com/microsoft/DirectXTex) (Microsoft): converts the home-screen backgrounds | MIT |
| [librsvg](https://gitlab.gnome.org/GNOME/librsvg) `rsvg-convert`: renders the UI icons (`tools/make_icons.sh`) | LGPL-2.1-or-later |

## Assets in `app/`

| What | Source | License |
| --- | --- | --- |
| UI icons (`app/assets/icons/`, made by `tools/make_icons.sh`) | [Stremio/stremio-icons](https://github.com/Stremio/stremio-icons) | MIT (as declared in its `package.json`) |
| App and menu logo (`app/sce_sys/icon0.png`, `app/assets/icons/logo.png`) | [homarr-labs/dashboard-icons](https://github.com/homarr-labs/dashboard-icons) | Apache-2.0 (the logo itself is Stremio's trademark) |
| Fonts (`app/fonts/`) | Noto Sans and Noto Emoji (Google) | SIL Open Font License 1.1 (`app/fonts/OFL.txt`) |
| Arabic font (`app/fonts/NotoNaskhArabicUI-*`) | Noto Naskh Arabic UI (Google), as shipped by [Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5) | SIL Open Font License 1.1 (`app/fonts/NotoNaskhArabicUI-NOTICE.txt`) |
| `app/ca-bundle.crt` | Mozilla's CA certificate list, as distributed by curl | MPL-2.0 |
| Launch background (`app/sce_sys/pic1*`) | panel 1 of `logo-presentation.png` from [Stremio/stremio-brand](https://github.com/Stremio/stremio-brand) | No explicit license file. The Stremio name, logo and artwork belong to their respective owners. |
| Selected-tile background (`app/sce_sys/pic0*`) | Custom Stremio background artwork | No separate license specified; not covered by the project's code license. |

## License texts

`app/licenses/` holds the license text of every component above, with an
index (`app/licenses/README.txt`). The build copies that folder,
`LICENSE` and this file into the app, so they ship inside the `.ffpfsc`.

## Services and runtime

The app talks to Stremio's public services (`api.strem.io` for the account,
library and addon collection, `link.stremio.com` for sign-in) and to the addons
the user installed. Streams play directly on the console using the built-in
torrent engine or the stream's own link. PS5 builds do not require or select
an external Stremio streaming server. On the console the app relies on
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (drakmor),
[etaHEN](https://github.com/etaHEN/etaHEN) and
[kstuff](https://github.com/EchoStretch/kstuff); none of them is included.

"Stremio" and the Stremio logo are trademarks of their owner. They are used
here only to identify the service this unofficial client connects to.

This project is not affiliated with, endorsed by or connected to Sony
Interactive Entertainment. "PlayStation", "PS5" and "DualSense" are trademarks
of Sony Interactive Entertainment Inc., used only to say which console the
app runs on. No Sony code, SDK, keys or firmware files are included: the
system-module stubs and the `libc.prx` loader shim are independently written
(ps5-payload-sdk, ps5-native-app-boilerplate).


## Application and UI provenance

The starting application is Sp9nky's unofficial-stremio-ps5-port 0.3.1 at
`64dd1e8ce8cf9eaac8291ff03fdad3e59255882e`, GPL-3.0-or-later. Original source
and component notices are retained. The Stremio Plus adaptation replaces the
RmlUi presentation and SDL video output with ps5-homebrew-ui and OpenGL;
RmlUi is not linked by this build. Its old license notice remains with
historical files from the starting project.

`vendor/ps5-homebrew-ui` is based on commit
`4bd942579dd981b3df9c740438489ca6a614ddc0`, Copyright (C) 2026 BlackBearReloaded,
GPL-3.0-or-later. Stremio Plus supplies its own application model and screens.
The vendor system-log hooks are adapted to use the app's redacted diagnostics.
See `vendor/ps5-homebrew-ui/THIRD_PARTY_NOTICES.md` for upstream component
attribution. That upstream notice describes the complete gallery: Stremio Plus
ships its sound effects and font assets, but does not link the gallery's
background-music player or include gallery music.

`app/hui/audio/sfx` contains the kit's GPL-licensed interface sound effects.
`app/hui/fonts` contains Inter and other retained kit font atlases and their
licenses. The additional `subtitles.huifont`, `subtitles-serif.huifont` and
`subtitles-mono.huifont` are baked from DejaVu Sans, DejaVu Serif and DejaVu Sans Mono,
Bitstream Vera license with DejaVu additions in the public domain. The font
source, notice and baker are provided. `third_party/stb_image_write.h` is
stb_image_write under MIT/public-domain terms and is used only by host
screenshot capture.

The native OpenGL SDK's complete notices and license texts are retained in
`app/licenses/ps5-opengl/`. Corresponding dependency source and build
provenance are supplied by the pinned SDK release; `BUILDING.md` records
versions and reproduction instructions.

The icon and launch background retain their upstream attribution. Film and
series artwork is supplied by runtime catalogs. The console screenshots in
`docs/images/` illustrate the interface; any depicted film and series artwork
belongs to its respective rights holders and is not covered by the code license.
