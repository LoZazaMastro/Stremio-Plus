# EVO Player components

Stremio Plus incorporates selected playback algorithms from [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5), created by **sainsaji and EVO Player contributors**, under GPL-3.0-or-later. EVO Player derives from ProsperoPlayer by KINGDKAK and contributors. The original copyright and attribution notice is retained in `NOTICE-upstream` and the full GPL text in `LICENSE`.

The application retains its Prospero UI and libVLC playback integration. These are modified components, adapted on **2026-10-19**:

- `src/video_evo_shaders.h` and `src/video_gl.cpp`: Dolby Vision signal reshaping adapted to OpenGL and integrated with the source-resolution colour-conversion pass. FSR 1, Lanczos, sharpening and motion-interpolation passes introduced in 0.5.12 were removed in 0.5.15, including their frame history and intermediate surfaces. Display scaling uses the ordinary bilinear texture filter.
- `src/video_dovi.h`, `src/video_dovi.cpp` and `src/player_vlc_dovi.cpp`: Dolby Vision Profile 5 reshaping and colour matrices, carried through FFmpeg's public `AVFrame` metadata. EVO's private FFmpeg 7.1 DOVIContext mirror is not used. The shader reshapes the signal and tone maps to SDR; it does not enable Dolby Vision HDMI output. Dolby Vision HEVC uses the metadata-aware software decoder.
- `src/player_iec61937.h`, `src/player_iec61937.cpp` and `src/player_vlc_passthrough.cpp`: IEC61937 framing, HDMI receiver capability interpretation and the PS5 Ex audio-port sequence. Local changes add per-stream ownership, strict bounds, receiver checks, the libVLC adapter and PCM fallback. Version 0.5.17 retains submitted grains in separate persistent buffers, drains before pause/flush/close, preserves the HDMI reset-to-open interval across output instances, and blocks further audio opens after an unconfirmed native shutdown. Only AC-3, E-AC-3 and DTS core at 48 kHz are enabled for native passthrough; AAC and experimental TrueHD transports remain unused.
- `native/heap.c` and `native/vlc_memory.c`: cached direct-memory allocation policy, including routing allocations of at least 1 MiB outside the general arena as in EVO's native `malloc_shim.c`. Stremio retains its own bounded ownership table, aligned-allocation and resizing contracts, conservative native-memory reserve, and retryable kernel-release handling.
- `src/display_refresh.cpp`: the output-mode ABI and 119.88 Hz mode, adapted to the Mesa-owned display with capability probing, resolution readback and rollback.

## Source revision

The supplied upstream archive records Git revision [`875f766dc9de762c1a1f608005d0afd1f25116f4`](https://github.com/sainsaji/EVO-PLAYER-PS5/tree/875f766dc9de762c1a1f608005d0afd1f25116f4) in its GitHub ZIP comment. Archive SHA-256: `27f2b819c8dc6a0ceb3f267a7063640a5bc4ff0d88a65b204a232c50df5953ea`.

The following hashes identify the exact upstream files used for this adaptation, before local modifications:

| Upstream file | SHA-256 |
| --- | --- |
| `tools/gen_upscale_pipes.py` | `9820887dd8df16ad81a0aec66ee9aa6f68ac958e5719bab380cb39f5f895382d` |
| `tools/gen_interp_pipes.py` | `7896b6a743c66206e9d5f06f23903296caf59ff5c76c411eee4ace7660e3329e` |
| `tools/gen_video_pipes.py` | `7b8596aca37befa3870baa1b2e94717296689a2b704d262bde0ba2ac72097231` |
| `projects/evoplayer/media/src/evo_dovi.c` | `5bc5b5bbb76db4b0167daf43899bc553afe3d73d5bc608f822baf75a2e0632fc` |
| `projects/evoplayer/media/include/evo_dovi.h` | `45b469622a2c910e1721b8d74a701d117171b591200a7e0e86ac371c81ecb91c` |
| `projects/evoplayer/media/src/evo_direct_mem.c` | `68c8b0562fa32b2dc315112b0539b25b3e88ba45ded272b52495860ebd403e7c` |
| `tools/native-app/stubs/malloc_shim.c` | `303650ddfef3e2a24b047c55146b198b799e8f2c87d07a334754b2f9441df66a` |
| `projects/evoplayer/media/src/evo_agc_runtime.c` | `d2b03e4fe7f6c4a063865dd280dbae5fbc32db1c9916dd6a2e742a8b71039e6b` |
| `projects/evoplayer/core/include/evo/audio/iec61937.hpp` | `58c2fbdb4d820d8c9e9c42d03796545b94945b53eb50b626221e2bc78d8942ae` |
| `projects/evoplayer/core/src/audio/iec61937.cpp` | `e244a9e04ccb82b79f865e5ab2bfd29f54fa0a2b7828336b6a73897de445bdbb` |
| `projects/evoplayer/core/src/audio/bitstream.cpp` | `4115af015a9f168fed5cfe2328567e55c0f47292255a08f3c7cbc9da1266caee` |
| `projects/evoplayer/core/src/audio/evo_pt.cpp` | `1743fefe738c62c4308ca6d312d6f6755d6e08a5ad433d64fcdfff158f22b8c7` |
| `projects/evoplayer/NOTICE` | `4e96508a514f7588ba6e1667443f717698494c5240170c1a5559cfb4de407d71` |
| `LICENSE` | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |

## Additional notices

Earlier FSR 1 passes incorporated [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) code, Copyright (c) 2021 Advanced Micro Devices, Inc. Those passes were removed in 0.5.15; their historical MIT permission and warranty notice remains in `LICENSE-FSR1`.

Dolby Vision colour mathematics in EVO Player derives from [libplacebo](https://github.com/haasn/libplacebo). Its LGPL-2.1-or-later notice and complete license are retained in `NOTICE-libplacebo` and `LICENSE-libplacebo`.

The complete modified source and build scripts accompany Stremio Plus binaries. These component notices impose no additional restrictions beyond their respective licenses.
