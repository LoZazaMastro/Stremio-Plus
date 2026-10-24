# EVO versus the current Stremio playback path — local source audit

Reviewed archive: `upload/EVO-PLAYER-PS5-main.zip`, SHA-256 `27f2b819c8dc6a0ceb3f267a7063640a5bc4ff0d88a65b204a232c50df5953ea`, revision `875f766dc9de762c1a1f608005d0afd1f25116f4` (same upstream archive already named in `vendor/evo-player/NOTICE.md`). No production files changed by this audit. No EVO build or PS5 execution was performed.

## Conclusion

Replacing libVLC is technically feasible. It is a backend integration, not a library-name swap. There are two separate changes: demux/decode/audio/session replacement, and graphics presentation replacement. Replacing only libVLC while still feeding `VideoGl::upload()` leaves the currently measured upload bottleneck in place. Adopting EVO's current AGC presentation avoids the Mesa texture-upload path, but requires unified ownership of GPU/VideoOut and integration of the Stremio UI on that renderer. It does not prove that the particular console will accept hardware decoder setup; EVO also uses FFmpeg demuxing and software fallback.

## Evidence from executable paths, not stale comments

- EVO canonical `projects/evoplayer/Makefile`, lines 157 onward: `AGC_DEVICE ?= 1`; includes `evo_agc_runtime.c`, writer and transient ring. The build is an application ELF, not an exported `libevo` playback library.
- `core/src/Application.cpp`, `Application::initHardware()`: calls `evo_agc_runtime_init` and initializes the full device context. Main loop obtains `pp_playback_get_video_frame`, calls `evo_agc_blit_yuv`, then `evo_agc_runtime_present`.
- `media/src/evo_agc_runtime.c`, `evo_agc_runtime_init`: allocates direct memory, scanout and command buffers, and calls `sceVideoOutOpen` (line 1434). This cannot simply run alongside the existing Mesa-owned display as if it were a standalone video widget.
- `pp/include/pp_playback.h` still describes the older GL-4/GL-5 path. Those comments are historical: the actual Makefile/Application runtime proves current AGC. Do not cite that comment as current architecture.

## What can be reused

- `media/include/evo_vdec.h` provides a relatively clean decoder seam: open/send/receive/flush/close, native-versus-FFmpeg selection, probe and recovery state. It intentionally does not own demux, playback clock or presentation.
- `pp_playback` and `pp_clock` supply pacing/seek publication. Their borrowed plane pointers require careful ownership adaptation to Stremio's `Player::VideoFrame::storage` lease; the interface promises validity only until the next decoder call, and steady-state EVO relies on pacing/pool lifetime.
- `ui_kit/include/hui_agc_batch.hpp` and its implementation already draw `hui::gfx::DrawList` from ps5-homebrew-ui via AGC, with font/image handles and deferred texture release. This makes retaining Stremio's screens practical; it does not require rewriting everything in RmlUi. Stremio's `TextureCache` currently takes `hui::gfx::GlBatch&`, and `main.cpp` creates GL surfaces, VideoGl, renderer/glass and composition paths: these need an adapter/backend boundary plus equivalent memory/fence protections.

## Session features that cannot be dropped

- `core/src/services/PlaybackController.cpp` has a usable high-level interface but still depends on global `play_fmt`, `audio_ctx`, `g_vdec`, `g_pp_pb`, `player_paused`, queue limits and resume globals. `evo_playback.c`, `evo_demux.c` and `evo_audio_out.c` share these globals. The modules are not independent drop-in instances.
- HTTP headers and network support exist: `evo_stream_io.c` sets FFmpeg headers/user_agent, `evo_parallel_io.c` implements range fetches. Their global/fixed-size header buffers differ from Stremio's per-request options and must preserve add-on headers, cancellation, HLS/DASH segments and source identity.
- Stremio's `Player::Options` includes growing downloads and a verified progressive resume limit. `player_vlc.cpp` connects `GrowingFileStream` and `NetStream` with generation/source checks. The audited EVO interfaces do not expose that Stremio download contract; an adapter is necessary rather than assuming ordinary EOF/reopen semantics are sufficient.
- EVO has PCM AudioOut, passthrough, audio-master clocks, seek settling, track switches and subtitles. `PlaybackController::switchAudioTrack` reopens at the current position; subtitle modules retain globals and timestamp gates. They must be mapped to Stremio's public Player API, its playback choice/bookmarks and the existing crash-safe audio lifecycle.

## Components already adapted in Stremio

`vendor/evo-player/NOTICE.md` documents source provenance and local changes for Dolby Vision colour conversion/metadata, IEC61937 passthrough, native memory policy and display refresh. It explicitly retains Stremio's own bounded ownership, output drain/close guards and GPU memory protections. A full migration must preserve those guarantees rather than discard them with the old backend.

## Recommendation for the active microstutter build

Finish the targeted upload correction and use its new per-stage timing on PS5. The supplied long log establishes steady 25 fps decoder delivery but about 22.3 fps app presentation; it does not establish a VLC decoder throughput failure. A future EVO backend can be a separate tested replacement with the same Player interface and one renderer, without weakening the current fix or claiming success from compilation alone.
