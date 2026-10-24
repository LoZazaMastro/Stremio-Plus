# Read-only review: nominal FPS, upload timing, diagnostic persistence and GL state

Result: no concrete blocking defect found in the reviewed changes. This was source review only; no additional runtime or PS5 tests were executed by this reviewer and no production files were edited.

## Nominal source rate and concurrency

`Player::source_fps()` on VLC reads `Vlc::fps` under `info_mutex`, matching the write in `refresh_info()`. The metadata enumeration and libVLC API calls happen before acquiring that mutex; the new accessor does not introduce a nested VLC lock or wait for decoder work. Player open/close and the diagnostic calls remain on the UI owner thread, just like existing `codec_summary()` and `stats()` accessors. The FFmpeg backend now uses `std::atomic<double>` for its worker-written `fps_`, with an explicit relaxed load for diagnostic consumption. A rational with positive numerator and denominator produces a finite stream rate. The App wrapper returns zero when no session is watched. The new periodic update removes the earlier stale initial-zero nominal FPS field.

The two rates are now distinct: `source_fps` is stream metadata; `accepted_frame_fps` is VideoGl's smoothed cadence inferred from presentation timestamps of frames accepted by the renderer. The latter is not HDMI scanout measurement. Actual swap-return intervals remain a separate histogram, and the JSON scope explicitly says the measurements are CPU monotonic timings rather than GPU completion/HDMI scanout.

## Timing collection and persistence

`last_upload_timing()` resets at the beginning of each upload. Resource allocation is measured only when reconfiguration occurs; plane-transfer and conversion-submit samples are added for accepted uploads. The total upload metric remains available and includes setup/validation overhead. No additional GPU fences, queries or `glFinish()` calls were introduced by the timing API. Empty windows report zero counts/means. All new durations derive from monotonic clock differences rather than untrusted stream timestamps.

The runtime allowlist now retains the new timing objects and their counters, including the previously dropped `video_frame_accepted`. Nested objects fit within the existing depth/count limits. Only `histogram` and `histogram_upper_ms` admit arrays; they must have at most eight numeric finite nonnegative elements. The emitted seven count buckets and six upper bounds match `hui::FrameStats`' thresholds; the final bucket is explicitly labelled 100 ms and above. Strings continue through credential redaction, and no URL, token, media title or path has been added to the new records.

## Rectangle-plane GL state versus UI

Video samplers are consistently `sampler2DRect` on units 0/1/2; input storage, parameters and subimage uploads use `GL_TEXTURE_RECTANGLE`. The output remains `GL_TEXTURE_2D`, so the existing UI texture handle contract is retained. Rectangle coordinates scale each plane by its own extent, including chroma at odd dimensions. Non-mipmap filtering and clamp-to-edge are explicit.

Leaving rectangle bindings on the units does not change the separate `GL_TEXTURE_2D` bindings used by the subsequent UI shader. `GlBatch::draw()` rebinds its shader, vertex array, viewport, blend/depth/cull state, all active font 2D textures and each image run's 2D texture on unit 0; `Renderer::present()` rebinds the framebuffer and viewport, including its glass passes. Video upload restores active unit 0 and the previous framebuffer. No sampler-object overrides were found in the UI path that would replace the rectangle planes' filter/wrap state.

## Allocation and buffer safety

The existing dimensions/plane-size/pitch validation remains before GL uploads. Mutable allocation still checks GL errors. The larger RGBA output is still allocated before the smaller planes. Setup failure still discards partial resources, detaches the FBO attachment before deletion, restores the prior framebuffer and reports rejection. Frame storage leases and native audio/memory ownership are unchanged by these edits. No new claim of console performance success follows from this review; the PS5 log must still verify the expected lower plane-transfer time and recovered presentation cadence.

## Reviewed source identities

Review UTC: 2026-10-23T17:51:50.557472+00:00

| File | SHA-256 |
| --- | --- |
| `src/main.cpp` | `6009c3002f9bd6114024e4ae37f609ce9009edb19755912d8b2016488dbf15d6` |
| `src/diagnostics.cpp` | `aecaa25a9ce5cd6182a020fbbd22d7a731cf5bbbf536514cc41d69213544ec0e` |
| `src/player.h` | `599680445327f1dc337f31764b7d422a05c09bdefbff7e8cd352be49a3f7a729` |
| `src/player.cpp` | `5210647b8a671a7a6140cf8bc65cb52b3d0f73845a314aaedcc9d5ce7acc928c` |
| `src/player_vlc.cpp` | `016d5be248e1b67e5f6b351c5f15c197ebbffa160cb632d009fff12f37a313ba` |
| `src/app.h` | `19844d387393141e2257fddc83993e58af8de757c19ccdc22f4ee58b82ef17bb` |
| `src/video_gl.h` | `022c3441f8fce271c9c7b15875f56f71052304e07e47d537cb06913b59ca5785` |
| `src/video_gl.cpp` | `231fc93ea7423a175ab07fcf84d32da8f98ce347278d42a2af9c5e2163dc8f8f` |
