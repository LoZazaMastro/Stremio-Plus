# Local 0.5.19 candidate: implementation and verification

Date: 2026-10-23. Application: **Stremio Plus**. Title ID: **PPSA74126**.
Content version: **00.069.000**. This is an unpublished candidate for the
owner's PS5 test. No GitHub branch, commit, workflow, release or store request
was created for the application.

## Source basis and historical note

The working application comes from the attached `Stremio-Plus-main.zip`.
The earlier attached source snapshot supplied the `loginWithToken` patch,
its helper and tests, and `docs/POST_0_5_18_WORK_IN_PROGRESS.md`.
The latest main archive did not contain that patch, so it was restored from
the supplied snapshot and reviewed before testing.

The post-0.5.18 note is historical. Its statements that compilation and
tests had not run apply to that earlier snapshot, not to the work recorded
here. The current terminal, host compiler and PS5 cross compiler were tested
successfully before continuing.

## Changes

### Initial Home presentation

`HomeStartupGate` stages the initial model behind a dedicated loading page.
The first two configured catalogs, initial account/Continue Watching data,
the first visible posters and selected background/logo determine readiness.
The model must be unchanged for 300 ms. Actual texture availability is
checked through the existing bounded texture cache before the first reveal.

The gate does not wait for every add-on. After content first arrives, slow
leading providers/account data get 1.5 seconds of grace; unavailable artwork
gets 1.25 seconds after the model becomes usable. A separate eight-second
deadline cannot be restarted by callbacks. All-settled empty results can
finish earlier. Normal late callbacks never reopen the loading page.

Carousel instances now survive catalog-row insertion, artwork completion
and account updates. Stable row identities, reattached artwork callbacks and
compensation of both the current and target vertical scroll preserve the
selected title's screen position. Continue Watching captures the selected
item's identity before applying a new account ordering. Existing shelves
do not replay their entrance animation on each content update.

**Bounded fallback:** an account request or serial extra-manifest request
can still delay all catalog requests. At the deadline the page becomes
navigable even if still empty. The gate cannot make an unresponsive provider
return data. An explicit reload of a still-empty Home is a new loading
request and may show the loading page again.

### Device account session

The supplied patch is retained: `link/read` approval is exchanged through
`loginWithToken` with `type: LoginWithToken` and `token`. Only the successful
response's `authKey` is saved; failure never falls back to the browser
approval credential. Saving is flushed before exposing successful login.
Generation, cancellation and expiry checks remain active. Cleanup revokes
only a separately issued session, never an ambiguous same-key response.

Existing stored sessions are not silently changed. A fresh pairing is
needed to test migration from the previous flow. Local tests model distinct
browser/device credentials and browser-only revocation. They do not prove
the live service's revocation behavior.

### Video presentation and diagnosis

The existing VLC configuration disabled both presentation dropping and
decoder skipping. Inspection of the exact VLC 3.0.24 source confirmed that
`drop-late-frames` acts in video output on pictures that are already decoded.
The candidate enables that output policy while retaining:

```text
--no-skip-frames
--avcodec-skip-frame=0
--no-avcodec-hurry-up
--no-avcodec-corrupted
```

Native allocation limits, decoder startup/retry guards, retained frame
leases, texture checks and audio-port failure/lifetime protection remain
in place. No forced hardware selection or firmware-specific driver fix is
claimed.

Bounded diagnostics distinguish the active hardware decoder count from
hardware availability, software fallback, source rate, VLC output losses,
application pickup drops, callback/pickup intervals and lateness, texture
upload time, swap wait and accepted-video swap intervals. Measurements use
fixed-size accumulators. The swap-return timestamp is an application
measurement, not the monitor's physical HDMI scanout timestamp.

The supplied logs contain older application versions and do not identify
the reported firmware. They cannot establish the cause of the 12.7 report.

### Current EVO and VLC upstream review

At the owner's request, the public upstreams were checked on 2026-10-23.
EVO Player's latest release is `v0.12.5`, and its main/tag commit
`875f766dc9de762c1a1f608005d0afd1f25116f4` is exactly the revision already
recorded in the supplied source and attachment. The API comparison reports
zero commits or files changed. No newer EVO playback code was substituted.

VideoLAN's stable `last/` source is still VLC 3.0.24, with the same pinned
archive hash. The PS5 port has a newer release, `v0.6.0`, at
`c0ffdca8351a3a7210eabae26897e36f83b26bd8`. Its later main snapshot
`c1862643f84b68d3b4f01cb484b887196093bfa2` has the same patch/module/compat
trees; subsequent changes concern its standalone frontend and controls.

Two applicable mechanisms were adapted:

- The upstream `0018-vmem-ps5-keep-visible.patch` is retained byte-for-byte
  as local patch `0025`. The application keeps coded plane storage while
  rendering only the visible rectangle when its origin is zero. The earlier
  Stremio setup already avoided geometric scaling, but caused VLC to insert
  an intermediate format-adapter picture. A real host comparison shows that
  this adapter and its complete-picture copy are removed. The owned vmem
  copies and retained frame leases remain in place; this is not zero-copy
  access to a decoder-owned buffer.
- The retained FFmpeg `HwDecoder` component now keeps a bounded FIFO of
  each accepted VP9 access unit's timestamp and
  visibility. A delayed output consumes its own metadata. The reproduced
  old behavior could discard the preceding visible frame and display a
  hidden reference frame with the wrong timestamp. The correction also
  handles drain and flush, and fails through the existing fallback before
  an overflow can mislabel output. Resident decoder slots, open/destructor
  policy, compute-queue handling and native allocations are unchanged.

The delivered PS5 application uses libVLC. Its `ps5vdec` module handles
H.264/HEVC, while the existing `src/player_vlc_vp9.cpp` adapter registers
`stremiovp9` for 8-bit VP9 and calls `HwDecoder::open/send/receive`.
CMake includes that adapter in the native target and the generated static
module table registers it. The FIFO backport therefore corrects an existing
backend reachable through the delivered VLC integration. It adds no new
VP9 profile or hardware capability, and actual selection and decoding on
the PS5 have not been tested. The vmem backport also applies to the delivered
VLC path.

The complete upstream decoder was not substituted: its memory/lifetime
policies differ from this application's existing crash protections. Its
1080i field timing and additional codec policies are separate from the
reported progressive 1080p issue. The attribution notice records exact
upstream paths and the limited scope of the backports.

Primary upstream locations and comparisons are retained in `validation/`:
[EVO](https://github.com/sainsaji/EVO-PLAYER-PS5),
[VLC PS5 release](https://github.com/ZiZc3/VLC/releases/tag/v0.6.0), and
[VideoLAN stable source](https://download.videolan.org/pub/videolan/vlc/last/).

### Right-stick playback volume

Media volume is separate from interface sound volume, defaults to 100%,
is clamped to 0–100 and changes in five-percent steps. Intentional vertical
motion uses 0.55 engagement, 0.30 release and vertical-axis dominance after
the existing input dead zone. Repeat starts after 350 ms and then uses
120 ms intervals, with at most one step per UI update. The indicator lasts
1.8 seconds. The level is persisted in `media_volume`.

Playback start, disconnection or system input interception requires a
physical neutral sample before a new volume action. A persistent
`InputFrame::intercepted` flag prevents the zeroed axes during a system
overlay from falsely satisfying that requirement. Trigger/navigation
mapping is unchanged; L2/R2 retain their existing actions.

Gain is applied once to owned decoded signed-16 PCM, in both host FFmpeg
and native/host VLC paths. Active encoded passthrough bypasses PCM gain:
the UI shows TV/receiver guidance without a fictitious percentage or a
change to the saved PCM level. Actual fallback to PCM remains adjustable,
even if passthrough is still the requested preference.

## Tests actually executed

The repository's `validation/` folder contains the selected logs and
machine-readable reports. Test programs and runners remain under `tests/`.
The host application was rebuilt as 0.5.19 in both FFmpeg and patched VLC
configurations; the final Home/UI/account checks use those coherent builds.

| Test | Actual result | Boundary and limitation |
| --- | --- | --- |
| Final PS5 VLC core build | PASS; 106 plugins, 132 static archives | Clean VLC source with all 25 patches; native target compiled locally |
| Final PS5 application build, import audit and signing | PASS; 152 application objects | Zero inline syscalls; 535 resolved imports; process heap 256 MiB; not console execution |
| Final ZIP/folder/FFPFSC equivalence | PASS; 722 checks, 227 files | Deep extraction, SHA-256, sizes, paths, metadata and container policy |
| Desktop FFmpeg application build | PASS | Linux executable; not PS5 |
| Patched static VLC host build | PASS; 106 plugins, 129 archives verified | Same retained media recipes/patches, host target |
| Host VLC application build | PASS | Linux executable; not PS5 |
| Home startup, desktop | PASS, 32 checks | Real App and HomeUi; synthetic clock and texture handles |
| Home startup, host VLC | PASS, 32 checks | Same Home behavior with VLC-compatible object layout |
| Existing HomeUi integration | PASS | Real UI routing with offline fixtures |
| Existing App interaction integration | PASS, 102 checks; zero HTTP | Real workers/model; controlled API/art boundaries |
| Account integration | PASS, 102 checks; zero HTTP | Real App/tasks/settings; simulated service replies |
| Volume policy/gain/timeline | PASS, 29 checks; ASan/UBSan | Production helper and PCM queue |
| Existing VLC output regression | PASS, 21 checks; ASan/UBSan | PCM timing and padded/10-bit/concurrent plane copies |
| Volume App/UI and FFmpeg PCM | PASS, 71 checks | Actual decoded PCM at SDL; simulated HDMI route |
| Volume App/UI and VLC PCM | PASS, 71 checks | Actual patched VLC decode/callback output; simulated HDMI route |
| Native heap regression | PASS, 921 checks and 30 injected fault records | Production allocator, real host mappings, modeled native API |
| Native video memory regression | PASS, 14 cases / 93,039 checks; ASan/UBSan | Ownership/failure cleanup with modeled console calls |
| PS5 video decoder startup regression | PASS, 62 cases / 8,234 checks; ASan/UBSan | Production allocation/startup path; simulated PS5 decoder calls |
| Native startup/import policy test | PASS, 5 tests | Local tooling and synthetic ELF fixtures |
| Native HDMI audio regression | PASS, 21 scenarios / 2,198 checks; ASan/UBSan | Real VLC objects and production audio module; modeled Sony HDMI calls |
| VP9 delayed-output association, distribution FFmpeg | PASS, 17 scenarios / 220 checks; ASan/UBSan | Complete production backend and real VP9 bitstream filter; modeled Sony decode boundary |
| Same VP9 test with pinned static FFmpeg 8.1.2 | PASS, 17 scenarios / 220 checks; ASan/UBSan | Actual linked versions checked: libavcodec 62.28.102 and libavutil 60.26.102; not physical decode |
| Final coded/visible picture regression | PASS, five cases / 30 frames each | All visible pixels SHA-256 checked; held frame remains valid after close; software decode and conversion |
| Final Main10 memory-pressure playback | PASS, 464 checks / three sessions / 216 frames | Real 4K HEVC B/P decode and native allocator; modeled direct-memory calls and pressure |
| Final controlled 1080p cadence comparison | PASS, 2,207 baseline / 2,191 candidate checks | Real playback with a controlled 350 ms output stall; host only |
| Repeated complete VLC patch application | PASS, 25 patches / 4,383 source files | First output identical to the original patch series; second application changes no files |

The pre-existing interaction harness initially failed its artwork assertion
because it still wrapped `ArtCache::get`/`peek`, while production already
used `get_async`/`peek_cached`. The harness boundary was updated to the actual
API, without weakening its assertions; all 102 checks then passed.

### Real PCM measurements

Both App volume runners decode a local stereo WAV with absolute sample
amplitude 12,000. Each phase captures more than 16,000 output frames:

| Requested volume | Measured sample magnitude |
| --- | ---: |
| 50% | 6,000 |
| 100% | 12,000 |
| 0% | 0 |
| 25% after mute | 3,000 |
| Above maximum | 12,000 after clamping |

The tests also check actual preference writes/reloads and invalid JSON,
right-stick routing, focus interception over multiple frames, held-stick
return/recentering, L2/R2 behavior and indicator draw geometry with the
bundled fonts. The passthrough route decision is simulated in these tests;
receiver compatibility is not established.

### Coded/visible picture comparison

The upstream backport was first compared using isolated baseline/candidate
Player and vmem objects. Five regular cases passed: H.264 720p and 1080p,
HEVC 1080p Main10, and real software conversions to NV12 and P010 requested
only at the test's output-format boundary. Each run observes 30 frames and
compares SHA-256 over **all visible pixels** against an independent FFmpeg
decode. A retained frame remains unchanged after advancement and close.

| Matching planar case | Old intermediate adapter | Candidate adapter | Old complete-picture copies in run | Candidate copies |
| --- | ---: | ---: | ---: | ---: |
| H.264 720p | 1 | 0 | 37 | 0 |
| H.264 1080p | 1 | 0 | 37 | 0 |
| HEVC 1080p Main10 | 1 | 0 | 38 | 0 |

`sws_scale` calls are zero in both versions for these cases: the old setup
was already avoiding geometric rescaling. The benefit measured here is
removal of the adapter's full-picture copy. The necessary plane copies
into the application's owned buffers remain. Decoder/output buffering can
process a few more frames than the 30 observed by the test. Forced chroma
conversion still requires its conversion adapter. No FPS improvement or
physical hardware-decoder performance is inferred from these counts.

The same five regular cases were then repeated against the fully rebuilt
host application and core. All 30 visible-frame digests per case and held
leases passed. The final result and the isolated A/B comparison are retained
as separate records; the comparison objects are not shipped as production
binaries.

**Known crop limitation, not fixed by this change:** a separate H.264
fixture encodes an SPS crop with origin (16, 8), intended visible size
1248×704 inside 1280×720. Both the old and new VLC paths supply a zero-origin
format and display the top-left rectangle instead. All 30 candidate hashes
equal the old behavior, but none match the correct cropped reference. The
reference was checked with the exact FFmpeg 8.1.2 decoder and an independent
explicit crop. This is **FAIL for crop correctness, PASS for absence of a
new regression**. `run_player_geometry_tests.py --include-known-crop`
returns failure while the limitation remains; it is never counted as a
successful pixel-correctness test. Native hardware behavior for that
fixture has not been tested.

### Controlled 1080p cadence comparison

`run_player_cadence_tests.py build/host-vlc` passed with real playback:
1920×1080 H.264 at 24 fps, a long GOP with B/P reference frames, PCM audio
and one controlled 350 ms delay in the video-output luma copy. The baseline
process restores only the old `no-drop-late-frames` argument; production
source/build objects are not rewritten between runs.

| Measurement | Previous setting | Candidate |
| --- | ---: | ---: |
| Display callbacks in first 50 ms after injected stall | 10 | 3 |
| VLC video-output lost pictures | 0 | 7 |
| Observed frames after stall | 76 | 74 |
| Last observed pickup lateness | 1.269 ms | 0.726 ms |
| Checks performed | 2,207 | 2,191 |
| Decoder safety options | Retained | Retained |

Every observed frame matches a sampled FNV digest from an independent
FFmpeg decode (one byte every 4,093 bytes); this is not a byte-for-byte
comparison of the whole movie. A retained render lease remains unchanged
through recovery and closure. This demonstrates reduced stale-frame burst
under that controlled host condition. It does not establish the cause or
resolution of the PS5 12.7 report.

### Main10 playback under modeled memory pressure

The final native-policy Player and static VLC/FFmpeg decoded a synthetic
3840×2160 HEVC Main10 stream at 24 fps containing I, P and B pictures.
Three open/play/close sessions observed 216 frames and passed 464 checks.
The production heap and picture allocator were used, with console memory
calls represented by separate host mappings. The test deliberately reserves
512 MiB of app-heap pressure and a separate 384 MiB graphics allocation
inside a 2,304 MiB logical direct-memory budget. These are test assumptions,
not measurements of the user's console.

There were zero allocation failures or mapping errors; all 200 dynamic
allocations were released. The measured direct-memory peak was 2,281,357,312
bytes, and the minimum sampled free heap was 238,361,360 bytes. Content
progression and held frame leases across close/reopen were checked. This
exercises the revised coded-size storage under pressure, but does not prove
long-duration stability, actual PS5 fragmentation or hardware decoding.

## Dependency recovery and reproducibility

### Repeatable patch application

The initial complete build succeeded, but a subsequent setup invocation
exposed an interaction between the new `0025` patch and the reverse check
for existing patch `0014`: the later insertion changed the earlier patch's
context. The earlier hook could be inserted twice on reuse.

Only the hunk boundaries of `0014-vmem-ps5-colour-info.patch` were adjusted.
The actual 25-patch loop was tested on a pristine verified VLC archive.
All 4,383 generated files match the former first-pass result byte-for-byte;
a second complete pass recognizes all 25 patches and changes no files.
No success marker, checksum exception or disabled validation was added.
The native and host setup commands were also rerun on their existing complete caches successfully. All media archives and the host executable remained byte-identical, so the final playback tests identify the same code.
The final application receipt records the corrected patch identity.

### Native artifacts and matching symbols

The final native build includes the selected upstream backports. Its source
fingerprint is
`9016a56286cf954b99942ccb067acea4cf39297c36fa288be7a69da03baf5144`.
`tools/build-receipt.py --verify` passed against the current application
source, media libraries and every file of the packaged application.

| Final artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `PPSA74126.zip` | 38,271,004 | `aaab0ca60c8cd5d63643a3b43dba03bcb9a40fec2b43ba938d54ba17588eb15c` |
| `PPSA74126.ffpfsc` | 42,926,080 | `ec759630ece44c94f3454d1ce2943f6e0281026a4b9130414e849ba74d3456bd` |
| `Stremio-Plus-0.5.19-symbols.zip` | 69,959,406 | `927ce565669f02bfa9d5ffb65404b05e8884d6dfa7b8a288d757544ed24cb476` |

The ZIP and deeply extracted FFPFSC contain the same 227 files. The pinned
MkPFS stores the application in an exFAT image inside PFS/PFSC. The outer
inodes use its read/execute policy; exFAT itself has no POSIX mode bits.
The folder ZIP preserves 0755 for `eboot.bin` and `sce_module/libc.prx`, and
0644 for other files. File bytes, lengths, paths and metadata were compared;
no claim is made that the two filesystem formats have identical POSIX modes.

The symbols bundle contains the exact linked `pie.elf` and `eboot.elf`, both
helper ELFs, the build receipt, internal checksums and debugging instructions.
The linked PIE hash is
`c63cc78cbb749a4aa68bf9749761e9692ed0d431ead036ae4a594b580abbf9ad`.
Eight real address-to-source-line lookups passed, including Home startup,
VP9 `decode_one`/`harvest` and `Player::take_frame`. Individual DWARF checks
for `app.cpp.o` and `hwdec_ps5.cpp.o` also passed.

**Debugging limitation:** global DWARF verification of the complete linked
ELF returned exit 1, with 37,812 range/inline/type diagnostics. Its complete
cause has not been established. This optimized build uses line-table
information; complete variable/type inspection is not promised. Matching
hashes and the successful address lookups do not turn the failed global
check into a pass. No debug sections were removed to suppress diagnostics.

### Recovered native inputs

The initial native setup failed because the two BlackBearReloaded upstream
locations were no longer available. The compiler itself was functional.
Those dependency failures were resolved without changing their versions:

- OpenGL SDK 1.0.0 was downloaded from the MyLuxy/overdrive-deps mirror.
  Its measured SHA-256 is exactly the original
  `f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5`.
  The original SDK manifest and its file checksums also pass.
- The native app boilerplate was fetched from the existing drakmor mirror
  at the exact original commit
  `f98de734b68b0b980d38ff03a330066afd6ae16d`, tree
  `723111f8b760d912f975edce4a821085c1167c2a`. Its sources are unmodified.
- The clean-room `libc.prx` was generated locally twice with identical
  output and original SHA-256
  `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
- `tools/setup-toolchain.sh` now has checked fallback locations. Archive
  hashes, the boilerplate commit check, clean-source check and manifest
  checks are retained. No checksum mismatch is accepted as a replacement.

The fork's codeload tarball is a separate archive identity, not the original
documented tarball. The build uses the verified Git commit; it does not
relabel the different archive digest. Selected recovery records and the
available dependency inventory accompany this source.

See [BUILDING.md](../BUILDING.md) for build commands and prerequisites, and
[DEPENDENCY_SOURCES.md](DEPENDENCY_SOURCES.md) for exact versions, source
locations and archive checksums.

### Offline inputs and rebuild helper

Both dependency ZIPs extract into `Stremio-Plus-0.5.19-dependencies/`.
They include the verified SDK/PacBrew/OpenGL archives, media source archives,
exact detached Git snapshots with their original shallow boundaries, and
offline Python wheels for the pinned MkPFS. NASM and FreeType source archives
are already in the application source ZIP. No branch or commit is created
by the restore helper.

`restore-cache.py` was run in isolated fresh directories. All five checks
passed: full restore including offline MkPFS installation, a second
idempotent restore, rejection of a corrupted input before writes,
preservation/rejection of a modified cache, and rejection of an external
cache symlink. The complete restore verified 39 inputs, copied 23 cache
files and restored two exact Git revisions; the second run copied zero
files. The helper itself performs no application compilation.

SDK and PacBrew are binary packages with headers. Their additional upstream
source locations are documented separately and are not represented as
locally cached source. Host operating-system compiler/development packages
are not included. The optional native-libraries ZIP preserves the 132 static
archives from this build; generated build configurations contain absolute
paths, so a new checkout should regenerate the libraries from the verified
source cache instead of treating that ZIP as a relocatable build prefix.

## Console validation still required

No physical PS5, DualSense, firmware 12.7, live account session revocation,
HDMI receiver or display scanout was exercised here. The console procedure
is in [PROVA_PS5_IT.md](PROVA_PS5_IT.md). In particular, verify startup with
fast/slow add-ons, browser-only logout followed by app restart, the reported
1080p file with decoder/cadence logs, PCM volume persistence and active
passthrough/fallback behavior. Publication awaits the owner's test.
