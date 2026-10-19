# Stremio Plus

A native Stremio app for PlayStation 5, built around the way you watch on a TV.
Browse your catalogs, pick a stream, save something for later, and keep your
library together in an interface designed for the DualSense.

See the [changelog](CHANGELOG.md) for the latest changes and upgrade notes.

![Stremio Plus home screen](docs/images/home.jpg)

## Made for PS5

**Built with Prospero UI.** Stremio Plus uses BlackBearReloaded's
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) to bring
fluid carousels, large artwork, smooth transitions, and translucent playback
controls to a native C++ interface. Films get room for their artwork; series
get a dedicated season selector and episode thumbnails.

**Your Stremio add-ons, including Torrentio.** Sign in with your Stremio
account to bring over your installed add-ons and library. Torrentio sources
appear alongside compatible providers, with quality, language, file size, and
seeder information when the add-on supplies it. Direct links and torrents
play on the console using its built-in playback and torrent support, without
a separate Stremio streaming server to configure.

**A dedicated torrent engine and offline downloads.** Stremio Plus has its
own native torrent engine, designed for high-throughput transfers directly
to the PS5. Download the selected movie or episode while you keep browsing,
then play it offline from a dedicated poster coverflow. Films appear as
individual covers, while a series has one cover for all its saved episodes.
Open a series to slide its poster to the left and browse episode thumbnails
in a floating panel. L1 and R1 select only the seasons represented in your
downloads. Completed films show their cover and title; saved episodes show
their thumbnail, episode number and title under the series heading. Progress,
speed, estimated time remaining, and connected peers and seeders appear only
while a transfer is unfinished. The queue
supports pause, resume, and deletion. Actual speed depends on the swarm,
connection, and storage; download performance is under active development.
Choose a download folder in Settings, including a mounted M.2 or external
volume, and create a **Stremio Plus Downloads** folder directly from the picker.
Changing that folder moves your existing downloads to the new destination.
Episode downloads keep the series title, season and episode numbers, and the
episode title together, including after a restart or when you are offline.

**VLC playback, inside the Stremio Plus interface.** The native player uses
VideoLAN's libVLC 3.0.24, adapted from [ZiZc3's VLC port for PS5](https://github.com/ZiZc3/VLC).
VLC handles media demuxing, decoding, and synchronization while Stremio Plus
keeps its Prospero UI, DualSense controls, and subtitle customization. The
PS5 modules supply hardware decoding for supported H.264, HEVC, and 8-bit
VP9 profile 0 streams. FFmpeg 8.1.2 and dav1d provide software decoding for
other supported formats.
The app follows the PS5's output resolution, with 1080p, 1440p, and 4K options.
Compatibility depends on the stream's profile and container. The OpenGL
presentation path outputs SDR, with HDR10 sources tone-mapped for display.
Playback controls appear when the first video frame reaches the GPU. If audio
starts without a video picture, the player retries the same source once using
software decoding, preserving the selected download and starting position.
If video output still fails, playback stops with an error instead of remaining
on the preparation screen indefinitely.
Slow input startup preserves the selected resume position. Changing or
automatically selecting a subtitle track does not seek or reopen the video.
Allocations of at least 1 MiB, including FFmpeg's internal buffers, decoded
pictures and video callback buffers, share a bounded, CPU-cacheable direct
memory pool. A smaller general arena leaves more of the console's memory
available on demand. Ownership tracking covers aligned allocations, resizing,
and failed kernel releases; a native-memory reserve remains available for
other resources. Critical video-decoder errors are reported to the app and
trigger one reopen at the last presented position. A repeated error stops
playback with an explicit message. Recovering from input buffering preserves HEVC reference
pictures; damaged decoder output is rejected while the last valid picture
remains visible. An explicit seek still resets the decoder as required.
Video textures are checked individually during allocation and released
when playback ends, including partial allocations from failed startup.
The app reduces its artwork cache during playback and restores the browsing
budget afterwards; saved poster images keep their original resolution.

**EVO Player contributions.** Dolby Vision Profile 5 reshaping, HDMI audio
transport, display refresh selection and native memory management incorporate
work from [EVO Player PS5 by sainsaji and contributors](https://github.com/sainsaji/EVO-PLAYER-PS5),
licensed under **GPL-3.0-or-later**. These components are adapted to Stremio
Plus's OpenGL renderer and libVLC pipeline. Their original authors, source
revision and local modifications are recorded in
[vendor/evo-player/NOTICE.md](vendor/evo-player/NOTICE.md).

**Discover without a background image.** The selected title's logo, year,
rating, runtime, genres, director, principal cast and description accompany
its catalog posters. Artwork lookups and decoding run away from the interface
thread, with selected artwork prioritized over adjacent covers and prefetch.
Downloaded series retain their cover and separate episode thumbnails for
offline browsing.

**Artwork that reaches your controller.** As you highlight a movie or series,
the DualSense light bar picks up a dominant color from its poster and eases
into the new shade. That color stays with you through loading and playback.
You can turn the effect off in Settings.

**Subtitles your way.** Choose a font, size, color, background opacity, and
effects such as an outline, shadow, or embossing. A live preview shows each
change before you return to the video. Audio and subtitle language preferences
are available separately.

**Quick add-on sync.** Your account's add-on collection loads in the background,
bringing existing configurations across without entering provider URLs on
the console. Install or configure add-ons in Stremio on your phone or computer,
then sync them in Stremio Plus.

## Screenshots

These PS5 captures show the 0.5.11 interface. Version 0.5.18 has a revised
Downloads coverflow with sharper posters, gallery sorting and simpler
completed entries, together with a revised Discover layout.

| Stream selection | Library |
| --- | --- |
| ![Torrentio stream selection](docs/images/streams.jpg) | ![Library](docs/images/library.jpg) |

| Discover | Subtitle customization |
| --- | --- |
| ![Discover with genre filters](docs/images/discover.jpg) | ![Subtitle settings and live preview](docs/images/subtitle-settings.jpg) |

![Downloads with transfer progress and connection statistics](docs/images/downloads.jpg)

## Getting started

You need a PS5 with a working native homebrew loader, a Stremio account, and
the add-ons you want to use configured on that account. Download the native
`PPSA74126.ffpfsc` image or folder archive from the
[latest release](https://github.com/LoZazaMastro/Stremio-Plus/releases/latest),
then install the format supported by your loader. Build instructions are in
[BUILDING.md](BUILDING.md). The application's title ID is `PPSA74126`.

App settings, the download directory registry, and streaming caches live in
**`/data/Stremio/appdata`**. Logs stay in **`/data/Stremio`**. The app prepares
these directories on launch. If your homebrew environment cannot create or
access the parent directory, create **`/data/Stremio`** and set its directory
permissions to **`0777`** with your file manager or FTP client. From a console
shell, the equivalent is:

```sh
mkdir -p /data/Stremio
chmod 0777 /data/Stremio
```

The homebrew environment must allow the app to access that directory and
provide a local ELF loader on port `9021`. The app uses that loader for its
filesystem grant and automatically starts a PS5 download writer when a
torrent download begins. Choose a video destination with the folder picker
in Settings before starting a new download. If no folder has been selected,
the download action shows a reminder with an OK button. Existing downloads
in `/data/Stremio/downloads/` are discovered and included when moving to your
chosen folder. Keep enough free space on the destination filesystem.

Installing the app itself on an M.2 volume, for example at
`/mnt/ext1/homebrew/PPSA74126`, does not change the settings or log paths.
To store videos on that volume, select a folder under `/mnt/ext1` in the
download folder picker.

On first launch, scan the sign-in QR code or open the displayed link on
another device. Once connected, Stremio Plus opens your home screen. The
interface follows the console language where supported, with English as the
fallback. English and Italian are currently available, and you can choose
either in Settings. Signing out returns to the sign-in screen.

## Controls

| Button | Action |
| --- | --- |
| D-pad | Move through items and controls |
| Cross | Open the selected item or confirm |
| Circle | Go back; hide visible playback controls before leaving the video |
| Triangle | Open search while browsing |
| L1 / R1 | Change top-level tabs, seasons, or stream providers, depending on the screen |
| L2 / R2 in Downloads | Cycle gallery sorting between title, release year and download date |
| Square on a cover | Add or remove it from your library |
| Square in Continue Watching | Remove that item from Continue Watching |
| Square on a stream | Download **only the selected stream** |
| Up / Down during playback | Show playback controls |
| Options during playback | Open audio and subtitle controls |

The Sony PlayStation 5 Media Remote is read through its dedicated input port,
independently of the DualSense. Direction, Select and Back navigate the app;
Play/Pause, Rewind and Fast-forward control playback. Outside playback,
Rewind and Fast-forward move between tabs or seasons. Additional transport,
audio and subtitle commands are handled when reported by a compatible remote.
The Media Remote must first be paired in the console's system settings.

Playback skip intervals for the D-pad and L1 / R1 can be set independently.
Completed downloads offer a choice between starting again and resuming from
the saved position for that movie or episode. Each episode has its own saved
position, so watching another episode does not replace its bookmark.

In Downloads, L2/R2 cycles between title A-Z, release year newest first and
download date newest first. The app remembers the selected order and keeps
the same film or series selected. This changes the gallery order only;
transfers keep their queue order and episodes retain their season ordering.
The gallery uses dedicated 720x1080 covers and upgrades previously saved
posters when a better source is available, retaining the old offline image
until its replacement is ready.

In Downloads, Options starts playback of an active download once its confirmed
file prefix reaches 5%. Seeking stays within timestamps the player has already
read; a file percentage is not treated as a time limit. A previously viewed
position is offered again only when the same source and its saved byte prefix
are available. If a crash rolls a download back to an earlier checkpoint,
the later bookmark is preserved until the required prefix has been recovered.

When a series episode ends, an optional card shows the next episode's
thumbnail, title, and number. Its countdown defaults to 15 seconds. Choose
**Watch now** to continue immediately or **Ignore** to dismiss it; Circle
also dismisses it. If the countdown reaches zero, the next episode starts
automatically. Autoplay and the countdown duration can be changed in Settings.

## Video and display options

Video frames are converted at their source resolution and displayed with
ordinary bilinear scaling. The PS5's configured resolution is the default.
These display and audio controls are available:

| Setting | Behavior |
| --- | --- |
| **Video & Display → 120 Hz output** | Off, Auto or Playback only. The app checks output support and the resulting mode, restores the previous mode when required, and refuses changes that reduce the registered output resolution. |
| **Audio → Audio passthrough** | Opt-in HDMI bitstream transport for receiver-advertised AC-3, E-AC-3 and DTS core formats at 48 kHz. Unsupported formats or unavailable HDMI transport use decoded PCM. Changes apply to the next video. |

System refresh behavior and decoded audio are the defaults. The video
renderer presents decoded source frames directly.
The HDMI output keeps submitted audio buffers alive until they are released
and drains pending audio before pause, seek and closure. If the native port
cannot be closed or restored correctly, playback stops and audio remains
disabled until the application is restarted.

Dolby Vision Profile 5 metadata is carried with the decoded frame and used
for RPU-based GPU reshaping before SDR tone mapping. This does not enable
native Dolby Vision HDMI output. AV1 uses the bundled dav1d **software**
decoder, including supported 4K 10-bit input; this build does not claim an
AV1 hardware decoding path or guaranteed real-time playback for every file.
The native decoder can still reject a stream or platform configuration, in
which case the existing software fallback remains available.

The currently supported high-refresh request is 119.88 Hz. It gives a 5:5
cadence for 23.976 fps content; it is not an exact 120.000 Hz mode for 24.000
fps content. Display, cable, receiver and console support determine whether
the change is accepted.

## Downloads and recovery

### Choosing a folder

Open the download folder setting to browse accessible directories, including
mounted volumes under `/mnt`. Use the D-pad to move and Cross to enter a
folder. Left, or the `..` entry, goes to its parent; Circle closes the picker.
Triangle selects the folder currently
displayed. Square creates **Stremio Plus Downloads** in that folder and opens
it; press Triangle to select it. Options refreshes the directory listing.

Choosing a different folder moves all existing downloads there, including
completed videos, partial downloads, saved artwork, and queued items. The
queue is suspended while the move runs, and the picker shows its progress.
After a successful move, the files are removed from their old locations and
downloads continue from the new folder. Unrelated files in either folder are
left alone.

Moving between volumes requires enough free space at the destination. The
original files are kept until their transferred data is verified and saved;
the app does not leave a second copy after a successful move. If a drive is
disconnected or a write fails, the app reports the problem and preserves the
saved state needed to recover the move. Reconnect any source drive before
changing the destination. A directory must pass a write check before use.
For an interrupted move, reopen the download folder setting: the picker
returns to the pending destination. Confirm it again to continue. New
downloads and file deletion wait until that move has finished.
The app applies permissions `0777` to the selected folder, reads the mode
back, and performs a real write check before accepting it. Creating
**Stremio Plus Downloads** applies and checks the same permissions. Download
job directories receive the same verified mode despite a restrictive system
umask. Startup repairs the permissions of known download directories in place,
without recreating them. The picker does not recursively change permissions
on unrelated files or directories.

### Transfers and saved state

Torrent downloads use a dedicated writer process on the PS5, started
automatically through the homebrew environment's ELF loader. This moves bulk
file writes out of the native application's throttled storage context,
following the approach documented by
[ProsperoStore](https://github.com/blackbearreloaded/ProsperoStore).
The app keeps the torrent engine, queue, and playback controls; no external
computer or streaming server is involved.

The writer accepts one sequential, page-aligned block of up to 4 MiB at a
time. Progress advances only after the writer confirms the complete block.
The torrent engine continues receiving and verifying pieces within its
memory budget. Only the active download transfers media; queued titles wait
for their turn.

Progress is saved durably after 256 MiB or 30 seconds of new torrent data,
whichever comes first. Pausing, stopping the app normally, or finishing a
download also saves the current position. If the console loses power or the
app crashes, the next attempt resumes from the last durable checkpoint and
downloads any uncommitted tail again. A partially written file is never
marked complete. If the writer cannot start or its connection is interrupted,
the app reports an error and retains the recoverable partial download.

For downloads with a known file size, the app checks space on the destination
filesystem before starting or resuming. The check uses the remaining bytes,
plus a small allowance for saved state. If storage runs out during a transfer,
the last valid checkpoint is kept and queued transfers wait until you free
space and retry. Other apps can consume space after the check, so write errors
are still handled throughout the transfer.

Existing downloads remain listed whenever their folder can be read, even if
the app cannot write to it. Startup checks the saved inventory and its recovery
copies. If a download's information is missing or damaged, its files appear
as an **Unrecognized download**, with the storage they occupy when measurable.
These files are not played or restarted using guessed metadata. Press Square
to delete the selected download and its files; deletion can free space even
when there is no room to save new queue state. No existing download is deleted
automatically just because its information could not be read.

Selecting an online source which exactly matches a saved download reuses that
download. Completed videos play locally; an incomplete transfer resumes through
the existing queue and becomes playable at 5%. Source matching includes the
episode, torrent file index, and request headers. A different episode or source
does not reuse the wrong file, and playback does not start a second torrent
transfer for the same saved download.

Streaming torrents also recover automatically when an active reader has been
waiting for missing data for 30 seconds without useful progress. Recovery
refreshes stale connections and peer discovery while retaining verified and
partially received pieces. Repeated attempts back off to a maximum interval of
two minutes. Paused playback, a full read-ahead buffer, and pending disk writes
do not trigger this recovery simply because the displayed rate is zero.

App-owned storage, torrent cache and download directories use verified `0777`
permissions. Files containing account or source configuration keep private
permissions. Moving to another volume preserves this distinction.

### Installation storage

The native manifest sets `downloadDataSize` to `0`. Stremio Plus does not
reserve a private `/download0` volume on installation. Streaming caches grow
as received data is written, within their existing capacity limits; their
maximum capacity is not preallocated. Downloaded media occupies space only
on its selected destination filesystem.

Versions up to 0.5.5 requested a 16 GiB private volume. Replacing application
files may leave that volume registered by the loader. To reclaim an existing
reservation, close the app, uninstall the registered title using the normal
console or loader uninstall operation, then install the current build.
Simply deleting or replacing files through FTP may leave the registration.
Keep `/data/Stremio` and your video directories. Do not manually modify a
mounted private volume. An uninstall can remove account settings from the
old private volume, so you may need to sign in again.

## Troubleshooting

Logs are kept together in `/data/Stremio/`. For a bug report, include the app
version, the steps that triggered it, and these files:

- `boot-current.txt`
- `log.txt`
- `runtime.json`
- `crash-current.txt` and `crash-last.txt`, if present

For download problems, include when the slowdown started and whether a video
was playing at the time. Checkpoint failures record the operation that failed
and its system error code. Storage errors also record the measured capacity
of the destination filesystem when available. Startup logs show directory
permissions before and after filesystem access is established. The downloaded
media files are not needed.

Playback logs distinguish loading the PS5 video module, preparing its compute
queue, and actually opening the native decoder. They include decoder rejection
reasons and native memory diagnostics. When available, the crash file records
the application abort caller or assertion that triggered termination; retain
the matching version's symbols when investigating a crash.
Clean launches and memory snapshots do not replace the last fatal crash
record. If that record cannot be rotated safely, it is preserved and the
storage error is reported.
Playback logs also report direct picture-memory usage, fallback allocations
and failed releases. These figures distinguish picture storage from the
general-purpose heap recorded in the crash diagnostics.
Video-output records distinguish decoded frames from frames accepted by
the renderer and identify failed texture or framebuffer setup stages.
Seek requests and applied positions are recorded without media locations.
Diagnostic snapshots are synced before replacing the previous complete
file. Nonzero native immediate exits retain a bounded exit-status and
caller-address record when that path reaches the application hook.

## Credits and license

Stremio Plus is developed by [LoZazaMastro](https://github.com/LoZazaMastro)
and builds on [Sp9nky's unofficial Stremio PS5 port](https://github.com/Sp9nky/unofficial-stremio-ps5-port).
Its native playback engine incorporates [ZiZc3's VLC PS5 port](https://github.com/ZiZc3/VLC)
and [VideoLAN's libVLC](https://www.videolan.org/vlc/libvlc.html). Their original
copyright and license notices are retained; the PS5 adaptation, local changes,
and source provenance are documented in [vendor/vlc-ps5/NOTICE.md](vendor/vlc-ps5/NOTICE.md).
Stremio Plus is based in part on
[EVO Player PS5 by sainsaji and contributors](https://github.com/sainsaji/EVO-PLAYER-PS5),
which derives from [ProsperoPlayer by KINGDKAK and contributors](https://github.com/KINGDKAK/ProsperoPlayer).
The incorporated work uses **GPL-3.0-or-later**. The original copyright
notice, full license text, exact upstream revision and dated descriptions
of Stremio Plus's modifications are retained in
[vendor/evo-player/](vendor/evo-player/) and included in both application
packages under `licenses/evo-player/`. The libplacebo authors are also
credited for the Dolby Vision colour mathematics used by EVO Player;
their applicable notices and license text are retained there.
Special thanks to [BlackBearReloaded](https://github.com/blackbearreloaded)
for the UI kit, OpenGL implementation, native app tooling, and ProsperoStore's
work on PS5 file-worker performance; to
[ps5-payload-dev](https://github.com/ps5-payload-dev) for the public SDK and
library ports; and to [Stremio](https://github.com/Stremio) and the other
open-source projects that make this app possible.

The project is licensed under the [GNU GPL v3](LICENSE). Dependency licenses
and attribution are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

### Corresponding source

The matching application source for this release is available at
[tag 0.5.18](https://github.com/LoZazaMastro/Stremio-Plus/tree/0.5.18), including
the modified playback components, retained VLC patches, dependency recipes,
assets and build scripts. [BUILDING.md](BUILDING.md) identifies the pinned
dependency sources and explains how to reproduce the native packages.
The [dependency source index](docs/DEPENDENCY_SOURCES.md) lists exact
upstream revisions and source locations for the linked libraries and SDK.
The [release page](https://github.com/LoZazaMastro/Stremio-Plus/releases/tag/0.5.18)
links these sources alongside the binary downloads. Source access is free
and does not require downloading the application packages first.
For a local copy, download
[the application source bundle](https://github.com/LoZazaMastro/Stremio-Plus/releases/download/0.5.18/Stremio-Plus-0.5.18-source.zip)
and [the dependency source bundle](https://github.com/LoZazaMastro/Stremio-Plus/releases/download/0.5.18/Stremio-Plus-0.5.18-dependency-source.zip).
Extract both into the same parent directory to bring the application,
dependency archives, build receipt and checksum records together.

Stremio Plus is an independent homebrew project, unaffiliated with Stremio
or Sony Interactive Entertainment. Their names, logos, and other trademarks
belong to their respective owners.
