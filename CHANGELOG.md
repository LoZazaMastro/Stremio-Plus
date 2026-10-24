# Changelog

## 0.5.20 - 2026-10-24

Changes since 0.5.18.

- **Stable Home loading.** Fixed the repeated flashing and repositioning of shelves during startup. The loading screen now stays visible until the first shelves and their artwork are ready, with bounded waits for slow add-ons. Later catalog updates preserve the current selection and scroll position.

- **Persistent PS5 sign-in.** Corrected phone pairing to exchange the approval credential for a PS5 device session and save it immediately. Failed exchanges and expired or cancelled approvals can no longer complete a stale sign-in attempt.

- **Smoother 1080p playback.** Removed the expensive per-frame texture upload path that caused dropped frames and reduced playback speed. Improved late-frame handling and removed an unnecessary video-output copy.

- **Right-stick volume control.** Move the right stick up or down to adjust playback volume in 5% steps. Added a dead zone, controlled repeat, a temporary volume indicator and a saved volume level. Audio passthrough displays TV/receiver volume guidance. L2/R2 keep their existing controls.

- **Consistent start and resume choices.** Every playback request now asks before starting, including online streams, downloads and the next episode. A saved position offers **Start from the beginning** or **Resume**; choosing the beginning starts at zero, and cancelling preserves the bookmark. Invalid or repeated confirmations cannot launch playback, and incomplete downloads retain their verified-data checks.

## 0.5.18 - 2026-10-19

This release brings together the playback fixes and Downloads improvements developed since 0.5.11.

### What's new

- **Smoother playback after seeking.** Fixed severe stuttering after skipping forward, going backward or resuming a video. Reduced GPU workload during 1080p playback and improved memory handling during longer sessions.

- **Improved video startup and cleanup.** Fixed video texture allocation and cleanup issues that could prevent frames from being displayed. Playback resources are released when a video stops, and artwork memory use is reduced during playback while preserving saved poster quality.

- **Better playback recovery.** Improved HEVC buffering recovery, preserved the last valid picture when damaged frames are rejected, and added one recovery attempt at the last displayed position after a critical decoder failure.

- **Improved HDMI audio handling.** Corrected audio buffer lifetime and output handling during pause, seeking and playback closure. Added checks to stop playback when the audio output cannot be safely closed or restored.

- **A cleaner Downloads page.** Browse films in a poster coverflow and series in dedicated season and episode panels. Completed downloads no longer show duplicate headings, completion labels or unnecessary transfer information. Active downloads retain progress, speed, remaining time and torrent details.

- **Sharper posters and L2/R2 sorting.** Added dedicated 720×1080 download covers and automatic upgrades for older saved artwork when a better source is available. Use L2/R2 to sort by title A–Z, release year or download date, with the newest entries first for date-based sorting. The app remembers your choice and keeps the same title selected.

- **More playback options.** Added Dolby Vision Profile 5 reshaping for SDR output, optional HDMI audio passthrough for AC-3, E-AC-3 and DTS core on compatible receivers, display refresh preferences and Sony PS5 Media Remote support.

- **Refreshed browsing and diagnostics.** Updated Discover with the selected title’s logo and metadata, prioritized visible artwork, reorganized settings and improved playback error logging.

### Credits and license

Stremio Plus is distributed under the **GNU General Public License v3**.

Selected playback components are adapted from [**EVO-PLAYER-PS5 by sainsaji and contributors**](https://github.com/sainsaji/EVO-PLAYER-PS5), based on [**ProsperoPlayer by KINGDKAK and contributors**](https://github.com/KINGDKAK/ProsperoPlayer). These components retain their **GPL-3.0-or-later** terms, original copyright notices and attribution. License texts and descriptions of local modifications are included in the source and application packages.

Thanks also to **ZiZc3 and VideoLAN** for the VLC playback foundation, and **BlackBearReloaded** for Prospero UI and the native graphics tooling.

## 0.5.17 - 2026-10-19 (unpublished test build)

This unpublished build addressed audio-buffer lifetime hazards reproduced
with a deferred-consumption test sink, and missing diagnostics. Those tests
did not establish the cause of the reported whole-console crash. The
retained changes are included in 0.5.18.

### Audio

- Keep submitted HDMI audio grains in separate, persistent buffers until
  the native output has released them, including AC-3 null-burst lead-in
  and lead-out. Do not submit reusable packet storage or temporary stack
  buffers to the output port.
- Drain submitted audio before pause, seek flush and port closure. Follow
  EVO Player's minimum one-second interval between HDMI reset and the
  next encoded-output open.
- Preserve the original native error. If drain, close or PCM restoration
  fails, retain port ownership and its buffers, stop playback, and block
  further player and interface audio opens for that application session.

### Diagnostics

- Record requested and applied seek positions and the selected seek path
  without recording media locations or identifiers.
- Preserve a bounded exit-status and caller-address receipt when native
  code terminates through a nonzero `_Exit`, which bypasses signal handlers.
  Keep the original immediate-exit behavior and preserve the receipt across
  subsequent launches.
- Sync diagnostic checkpoints before replacing the previous file and sync
  the containing directory where supported. Retain the previous complete
  snapshot if writing or syncing the replacement file fails before rename.

Audio-port sequencing and IEC 61937 support remain adapted from
[EVO Player PS5 by sainsaji and contributors](https://github.com/sainsaji/EVO-PLAYER-PS5)
under GPL-3.0-or-later, with the original notices retained.

Title ID: `PPSA74126`. Native content version: `00.067.000`.

## 0.5.16 - 2026-10-19 (unpublished test build)

This test build brings together the playback, download and browsing changes
developed after 0.5.11.

### Downloads

- Use a dedicated 720x1080 poster cache for the Downloads gallery. Upgrade
  existing saved covers when a better source is available, retaining the
  previous offline image until its replacement has been saved successfully.
- Cycle gallery sorting with L2/R2: title A-Z, release year newest first,
  or download date newest first. Remember the selected mode and keep the
  same film or series selected when the order changes.
- Retain release-year and completion-time metadata for new downloads.
  Older entries use their existing saved download timestamp as a fallback.
  Gallery sorting leaves the transfer queue and episode order unchanged.
- Browse saved films in a poster coverflow and saved series under one cover,
  with a separate season and episode panel. Use L1/R1 to switch between
  downloaded seasons; episode thumbnails remain available offline.
- Show each completed film's title once. Completed entries no longer show
  transfer statistics or completion labels; unfinished downloads retain
  progress, speed, remaining time and torrent connection details.

### Playback

- Fix severe stutter after seeking forward, seeking backward or resuming
  a video by restoring the decoder's normal frame-discard policy.
- Improve memory handling during extended playback. Large decoder buffers
  share a bounded native memory pool, and critical decoder errors trigger
  one recovery attempt at the last displayed position.
- Preserve HEVC reference pictures when recovering from buffering and
  keep the last valid picture visible if damaged output is rejected.
- Simplify video presentation to reduce GPU work during 1080p playback.
- Add Dolby Vision Profile 5 GPU reshaping before SDR tone mapping,
  optional HDMI passthrough for supported AC-3, E-AC-3 and DTS core
  receivers, and output refresh preferences where supported by the display.
- Support Sony PS5 Media Remote navigation and playback controls through
  its dedicated input port.

### Browsing

- Refresh Discover with the selected title's logo and metadata alongside
  the catalog posters.
- Load and decode visible artwork asynchronously, prioritizing the selected
  item, and organize settings into native UI sections.

### Credits

Selected playback components are adapted from
[EVO Player PS5 by sainsaji and contributors](https://github.com/sainsaji/EVO-PLAYER-PS5)
under GPL-3.0-or-later, with credit to its ProsperoPlayer origins and the
other component authors. Original notices, the full license and dated local
modifications are retained with the source and in the application packages.

Title ID: `PPSA74126`. Native content version: `00.066.000`.

## 0.5.15 - 2026-10-19 (test build)

- Simplify video presentation to one colour-conversion pass at the source
  resolution and direct display of decoded frames with bilinear scaling.
- Reduce per-frame GPU work and memory use during 1080p playback. Retain
  output resolution, refresh-rate selection, colour conversion, HDR tone
  mapping, Dolby Vision reshaping and audio output controls.
- Preserve the seek-discard correction from 0.5.14 and the native-memory
  and completed-download changes from 0.5.13.

Title ID: `PPSA74126`. Native content version: `00.065.000`.

## 0.5.14 - 2026-10-19 (test build)

- Restore the configured FFmpeg frame-discard policy after seek preroll,
  including when VLC's hurry-up mode is disabled. The temporary NONREF
  setting no longer persists into normal playback after seeking forward
  or backward, or resuming at a nonzero position.
- Preserve the existing reference-safe buffering behavior, native-memory
  fixes and completed-download presentation from 0.5.13.

Title ID: `PPSA74126`. Native content version: `00.064.000`.

## 0.5.13 - 2026-10-19 (test build)

This update addresses the native-memory allocation path implicated by the
0.5.12 playback logs and simplifies completed downloads. Its changes are
included in 0.5.18.

### Playback

- Route all allocations of at least 1 MiB through the bounded cached direct
  memory pool, including FFmpeg allocations that previously stayed in the
  fixed general arena. Reduce the initial general arena to at most 768 MiB
  so free space is available to the shared native workload on demand.
- Preserve alignment, zero initialization, resize contents and ownership
  across allocation failures and deferred kernel releases. Account for
  multiple free physical extents before rejecting a large allocation,
  while retaining the pool limit and native-memory reserve.
- Propagate critical FFmpeg video-decoder failures to the application.
  Reopen the same source once at the last presented position, outside VLC's
  decoder locks; the retry uses fewer software decode workers on PS5.
  A repeated failure stops with an error instead of retaining an apparently
  healthy player. Normal input buffering continues to preserve references.
- Record general-arena snapshots every 30 seconds during playback alongside
  the shared large-allocation statistics, so a later failure can be compared
  with recent memory use instead of only the first displayed frame.

### Downloads

- Show a completed film's title once, with its cover. Remove completion
  text, offline labels, size, percentage and completed progress bars.
- Keep one series heading and the episode thumbnails, numbers and titles.
  Remove repeated headings and offline episode counts, including while
  the series panel animates in and out.
- Keep progress, transfer rate, remaining time and torrent connection data
  for unfinished downloads.

### Build

- Make VLC patch application repeatable when reusing an existing source
  directory, preventing duplicate subtitle-callback definitions.
- Add decoder-error recovery coverage and native-memory stress cases.
  Test reports distinguish actual decoding from simulated PS5 memory calls;
  they do not claim physical console validation.

Title ID: `PPSA74126`. Native content version: `00.063.000`.

## 0.5.12 - 2026-10-19 (test build)

This test build addresses reference-picture loss during HEVC buffering and
memory pressure in sustained 4K playback. It also introduces a dedicated
Downloads coverflow and selected EVO Player playback components while
retaining the Stremio Plus interface. The retained changes are included
in 0.5.18.

### Playback

- Preserve decoder reference pictures when recovering from a late input
  clock or empty stream buffer. Explicit seeks still flush the decoder.
  Reject corrupt output and retain the last clean picture instead of
  presenting damaged blocks during buffering.
- Stop discarding compressed reference frames as a timing shortcut. Video
  callback buffers join decoded pictures in a bounded, CPU-cacheable
  direct-memory pool, reducing pressure on the general application heap.
- Carry Dolby Vision metadata per decoded picture and apply Profile 5
  RPU reshaping on the GPU before SDR tone mapping.
- Add opt-in HDMI passthrough for supported AC-3, E-AC-3 and DTS core
  receivers at 48 kHz, with decoded PCM as the default and fallback.
- Add **Off / Auto / Playback only** high-refresh output preferences.
  The supported request is 119.88 Hz; the output mode and dimensions are
  checked before presenting, with rollback when a mode is refused.
- Read Sony PS5 Media Remote navigation and transport events through its
  dedicated input port, independently of the DualSense.
- Record effective processing modes, fallback reasons and display-mode
  results in the existing logs under `/data/Stremio`.

### Downloads and browsing

- Replace the Downloads grid with an animated poster coverflow and
  progress display, retaining transfer rate, remaining time and peer data.
- Group series episodes under one poster. Opening the series moves its
  poster to the left, fades the remaining covers and opens a floating
  episode panel with thumbnails, episode numbers and titles.
- Use L1 / R1 inside that panel to select only represented seasons.
  Playback, pause/resume and deletion act on the selected episode.
- Persist episode thumbnails separately from series covers, with
  backward-compatible recovery for existing download metadata.
- Remove Discover's background hero and show the title logo, year,
  rating, runtime, genres, director, principal cast and description.
- Make visible artwork lookup asynchronous, prioritize the selected
  artwork, and keep artwork file operations away from active media writes.
- Organize settings into native UI sections without changing saved
  preferences or existing control assignments.

### Compatibility and attribution

EVO Player PS5 by sainsaji and contributors is credited in the README and
retained component notices. Adapted source, GPL license and libplacebo
notices accompany the binary. AV1 remains a dav1d software decoding path.
Dolby Vision output is tone-mapped to SDR; native HDMI audio passthrough
supports receiver-advertised AC-3, E-AC-3 and DTS core formats at 48 kHz.

Native builds and host regression checks do not establish sustained 4K
playback, HDMI negotiation or Media Remote operation on a physical console.
Those checks remain part of this build's PS5 acceptance testing. The title
ID stays `PPSA74126`; installation reserves no private download volume.
Download locations, saved media, account data and bookmarks are retained.

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
