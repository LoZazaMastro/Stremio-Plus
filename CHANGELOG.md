# Changelog

## 0.5.11 - 2026-10-18

This release includes the playback, download and torrent improvements
developed in 0.5.7 through 0.5.10, plus a fix for memory pressure during
HEVC 4K 10-bit playback. Diagnostics from 0.5.10 captured an allocation
failure followed by a C++ abort; the general-purpose heap was already
nearly full at the first displayed frame.

- Allocate large VLC picture buffers in a separate, bounded direct-memory
  pool instead of placing every picture in the application heap. The pool
  uses at most 512 MiB and leaves a 128 MiB contiguous-memory reserve for
  other native allocations. VLC retains its existing aligned-heap fallback.
- Track each mapping until the picture's final release. Failed unmap or
  release operations retain ownership, count against the pool limit, and
  are retried by subsequent allocator activity.
- Roll back a torrent piece reservation if its C++ allocation fails, before
  publishing the piece or requesting its blocks, and retry on a later pass.
  Existing reserved transfers and peer connections remain intact. The
  ultra-fast peer, request and payload-memory limits are unchanged.
- Log direct picture memory, pool fallbacks and release errors alongside
  the existing playback and crash diagnostics in `/data/Stremio`.

Video resolution, bit depth, decoder thread count, the player interface and
the 0.5.10 resume/subtitle fixes are preserved. The native hardware decoder
configuration remains unchanged; unsupported configurations can still use
software decoding. Playback compatibility depends on the stream profile
and the console's homebrew environment.

The title ID remains `PPSA74126`, with no reserved download installation
space. Settings, download locations, downloaded media and bookmarks retain
their existing formats.

## 0.5.10 - 2026-10-17 (development)

This development build corrects a reproduced playback-resume regression and
improves native decoder allocation and crash diagnostics. The resulting
diagnostics informed the memory-management changes included in 0.5.11.

- Keep the saved playback position while a slow source is still loading.
  A seek is considered unconfirmed only after five seconds of actual decoder
  progress, excluding time spent paused or waiting for input. Frames with an
  unknown playback position cannot replace the saved timeline.
- Select subtitle tracks without seeking or reopening the video. Automatic
  subtitle selection can no longer rewind a newly resumed stream to zero.
- Let the PS5 decoder use direct memory for its CPU workspace when flexible
  memory cannot satisfy the allocation, matching the fallback supported by
  the existing PS5 decoder implementation.
- Record compute-queue preparation, decoder rejection reasons, and the memory
  allocation path in the existing logs. Preserve the requirement to prepare
  the compute queue before filesystem access is granted.
- Capture application abort callers and assertion details in the crash file,
  with bounded allocation-failure records and native heap usage. Fatal
  recording avoids allocation and logging locks; the original fatal action
  still terminates the process.
- Preserve the last fatal crash record across clean launches and nonfatal
  memory snapshots. A failed rotation keeps the original evidence intact.

The title ID remains `PPSA74126`; existing settings, download folders and
saved progress keep their current formats.

## 0.5.9 - 2026-10-17 (development)

This development build fixes a playback startup regression where audio could play
while the preparation screen remained visible without video or controls.

- Initialize the PS5 video module and its compute queue before the filesystem
  grant, following the initialization order required by the VLC PS5 port.
- Leave the preparation screen after the first video frame has been uploaded
  successfully to the GPU. Show the playback controls from that point, even
  if VLC's buffering event has not yet reached 100 percent.
- Retry the same input once with software decoding if eight seconds of audio
  have actually played without a video picture. Keep the original starting
  position and existing download. Stop playback with an error if the retry
  also fails, rather than leaving audio running behind the loading screen.
- Limit software frame decoding to four threads on PS5 to keep its retained
  picture buffers within the app's memory budget. Hardware decoding keeps its
  existing pipeline.
- Record video setup, output allocation failures, first-picture delivery and
  startup progress in the existing logs under `/data/Stremio`.

The title ID remains `PPSA74126`. Download folders, saved progress and account
settings retain their existing formats.

## 0.5.8 - 2026-10-17 (development)

This development build updates the native playback engine and fixes download
permissions, episode identification, and playback recovery.

### Playback

- Integrate VideoLAN libVLC 3.0.24 through ZiZc3's VLC PS5 adaptation, with
  FFmpeg 8.1.2 and PS5 H.264, HEVC, and 8-bit VP9 hardware decoders. Keep the existing
  Prospero UI, playback controls, controller lighting, and subtitle styling.
- Retain separate bookmarks for every episode, including downloaded videos.
  Completed local files offer playback from the beginning or the saved time.
- Resume an already viewed position in a growing download only after checking
  its source identity, file size, and confirmed byte prefix. Preserve the
  bookmark if a crash checkpoint has not yet recovered the required prefix.
- Reuse an exactly matching saved download when its online source is selected
  again. Resume incomplete transfers through the existing queue and wait for
  playback readiness without opening another torrent transfer.

### Downloads and torrents

- Stop treating scheduled episodes as live TV. Catalog release schedules no
  longer prevent ordinary movies and series episodes from being downloaded.
- Persist the series title, season number, episode number, and episode title
  in each download's metadata and display them in the offline list.
- Apply and verify `0777` on app-owned storage, torrent cache and download job
  directories, including when the system umask is restrictive. Repair known
  directories on startup without replacing them or deleting existing media.
  Account and source configuration files retain private permissions.
- Recover stalled torrent readers automatically after 30 seconds without
  useful data. Reconnect stale peers and refresh discovery while keeping
  verified data and partially assembled pieces. Repeated retries use bounded
  backoff; paused playback and local storage backpressure do not trigger a
  restart merely because the transfer rate is zero.

### Source and validation

- Retain VLC PS5 and VideoLAN attribution, licenses, source patches and build
  provenance with the corresponding source delivery.
- Add regression checks for scheduled episodes, persistent labels, per-episode
  bookmarks, download reuse, restrictive permissions, and stalled readers.

The title ID remains `PPSA74126`; installation storage reservation remains
zero.

## 0.5.7 - 2026-10-11 (development)

This update improves torrent scheduling during busy downloads and reduces
unnecessary recovery work with mixed-speed peers.

### Improvements

- **Batched request scheduling.** Received piece replies now refill a peer's request window together, reducing repeated work for every 16 KiB block.
- **Shorter network lock holds.** Each ready peer has a bounded receive turn, with opportunities for download readers, verification and status updates to run between peers.
- **Less redundant traffic.** Bulk recovery uses idle peers and a limited request batch. Urgent recovery of a missing playback or download head remains available.
- **Progress-aware peer timeouts.** Recent useful delivery prevents whole-peer timeout penalties for older queued requests. Missing urgent heads retain a separate recovery deadline. Silent peers still time out; keepalives and duplicate replies do not reset the useful-data timer.
- **Better recovery after a timeout.** A sender skips refill in the housekeeping iteration that cancels its stale requests, giving other peers a chance to take the released work.
- **More useful diagnostics.** Torrent logs include scheduling counters alongside useful transfer rates, duplicate traffic, timeout and storage timings.

### Validation and upgrading

The TCP regression harness now includes sustained transfers with many local
seeders and concurrent status-call latency measurements. It checks the complete
downloaded payload and records source hashes for before-and-after comparisons.
These are controlled host tests; transfer speeds on a PS5 depend on its live
swarm, network and storage.

The title ID remains `PPSA74126`. Existing settings, selected download folders
and saved download checkpoints use the same formats as 0.5.6.

## 0.5.6 — 2026-10-08

Stremio Plus 0.5.6 gives you more control over your downloads and makes it easier to keep watching your favorite series.

### What's new

- **Choose your download folder.** Browse internal, M.2 and external storage from Settings, or create a "Stremio Plus Downloads" folder. The app applies and verifies `0777` permissions and checks that the folder is writable.
- **Move your existing downloads.** Changing folders moves completed videos, partial downloads, queued items, artwork and saved progress together. Transfers pause during the move, and interrupted moves can be recovered.
- **Better download recovery.** Improved handling of checkpoint failures and full disks. Existing files remain visible when their folder is readable, including unrecognized downloads that you can remove to recover space.
- **Fixed excessive installation storage.** Removed the 16 GiB private storage reservation requested by earlier builds. Streaming caches grow only as data is written.
- **Watch the next episode.** A new overlay shows its thumbnail, title and episode number, with "Watch now" and "Ignore" controls. The countdown defaults to 15 seconds and can be adjusted or disabled in Settings. Circle dismisses it.
- **Refined the loading screen.** Reduced the title logo to half its previous width and height while preserving its proportions.

### Upgrading

An old installation may retain the 16 GiB reservation after its files are replaced. To reclaim it, uninstall the registered app through the console or loader, then install this version. Keep `/data/Stremio` and your downloaded-video folders. You may need to sign in again.
