Stremio Plus - license notices

The app is licensed under the GNU GPL v3.0 (LICENSE, next to this folder in
the app). It is derived from the unofficial Stremio PS5 port:
https://github.com/Sp9nky/unofficial-stremio-ps5-port.
It contains or links the components below; each keeps its own license, whose
text is in this folder. THIRD_PARTY.md lists where each one is used.

Component                                  License                         File
-----------------------------------------  ------------------------------  ---------------------
ps5-native-app-boilerplate (BlackBear-     GPL-3.0-or-later                ../LICENSE
  Reloaded), ps5-payload-sdk, PacBrew,
  MkPFS, SharpProspero
ps5-payload-sdk libc (FreeBSD parts)       BSD                             FreeBSD.txt
Kodi port for PS5 (Team Kodi, VivaLaVent)  GPL-2.0-or-later                GPL-2.0.txt
Nuvio PS5 (Husam Osman): hardware decoder  GPL-3.0-or-later                ../LICENSE
FFmpeg 8.1.2                               LGPL-2.1-or-later               FFmpeg.txt, vlc/FFmpeg-LGPL-2.1.txt
VLC PS5 (ZiZc3), libVLC 3.0.24            GPL-3.0; GPL/LGPL portions      vlc/README.txt
EVO Player PS5 (sainsaji, contributors)   GPL-3.0-or-later                evo-player/NOTICE.md, evo-player/LICENSE
AMD FidelityFX FSR 1                      MIT                             evo-player/LICENSE-FSR1
libplacebo-derived Dolby Vision math     LGPL-2.1-or-later               evo-player/NOTICE-libplacebo, evo-player/LICENSE-libplacebo
VLC media and TLS dependencies            Component-specific              vlc/README.txt
LLVM C++ runtimes and compiler-rt          Apache-2.0 WITH LLVM-exception  LLVM.txt
SDL2                                       zlib                            SDL2.txt
libsamplerate                              BSD-2-Clause                    libsamplerate.txt
RmlUi (historical, not linked)             MIT                             RmlUi.txt
libcurl                                    curl                            curl.txt
OpenSSL                                    Apache-2.0                      OpenSSL.txt
libpsl                                     MIT                             libpsl.txt
Public Suffix List data, ca-bundle.crt     MPL-2.0                         MPL-2.0.txt
FreeType                                   FreeType License (FTL)          FreeType.txt
libwebp, libsharpyuv                       BSD-3-Clause (+ patent grant)   libwebp.txt, libwebp-PATENTS.txt
libpng (historical, not linked)            libpng License                  libpng.txt
zlib                                       zlib                            zlib.txt
zstd                                       BSD-3-Clause                    zstd.txt
xz / liblzma (historical, not linked)       0BSD                            xz.txt, xz-0BSD.txt
bzip2 (historical, not linked)             bzip2 License                   bzip2.txt
GNU libiconv                               LGPL-2.1-or-later               LGPL-2.1.txt
GNU FriBidi                                LGPL-2.1-or-later               LGPL-2.1.txt
dlmalloc (Doug Lea)                        MIT-0                           dlmalloc.txt
nlohmann/json (Niels Lohmann)              MIT                             nlohmann-json.txt
dht (Juliusz Chroboczek)                   MIT                             dht.txt
miniupnpc                                  BSD-3-Clause                    miniupnpc.txt
stb_image (Sean Barrett)                   MIT / public domain             stb_image.txt
Stremio icons, stremio-video               MIT                             Stremio-MIT.txt
dashboard-icons (logo)                     Apache-2.0                      dashboard-icons.txt
Noto Sans, Noto Emoji (Google)             SIL OFL 1.1                     ../fonts/OFL.txt
Noto Naskh Arabic UI (Google)              SIL OFL 1.1                     ../fonts/NotoNaskhArabicUI-NOTICE.txt

"Stremio" and the Stremio logo are trademarks of their owner, used only to
identify the service this unofficial client connects to. This app is not
affiliated with or endorsed by Stremio or Sony Interactive Entertainment.
"PlayStation", "PS5" and "DualSense" are trademarks of Sony Interactive
Entertainment Inc. No Sony code, SDK, keys or firmware files are included. The home-screen
backgrounds use Stremio's brand artwork from
https://github.com/Stremio/stremio-brand (logo-presentation.png).

Interface components and additional artwork notices:
ps5-homebrew-ui, its UI components, renderer and interface sound effects:
  GPL-3.0-or-later, Copyright (C) 2026 BlackBearReloaded;
  see ps5-homebrew-ui-NOTICES.md and ../hui/fonts/*-LICENSE.txt.
ps5-opengl, Mesa and OpenGNM: see ps5-opengl/ for retained complete notices.
DejaVu Sans subtitle atlas: Bitstream Vera license, DejaVu additions public
  domain; see ../hui/fonts/DejaVu-LICENSE.txt.
stb_image_write: MIT/public domain, same terms as stb_image.txt (host only).
No separate license has been provided for the custom selected-tile background.

The x264 GPL-2.0 notice is retained from earlier builds. The native 0.5.8
FFmpeg decoder-only configuration does not enable or link x264.
