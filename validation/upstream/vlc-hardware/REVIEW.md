# Independent VLC PS5 hardware, timing and display review

Reviewed on 2026-10-23. This review is separate from the software-vmem and
framebuffer-initialization work owned by the playback reviewer.

## Exact comparison

- Upstream repository: <https://github.com/ZiZc3/VLC>.
- Reviewed release: `v0.6.0`, commit
  `c0ffdca8351a3a7210eabae26897e36f83b26bd8`, dated
  2026-10-21T21:38:04Z.
- Isolated checkout:
  `validation-current/upstream-vlc-review/VLC`.
- Compared against the actual supplied Stremio files, including the local
  console safety modifications. The supplied VLC ZIP does not establish an
  exact older Git commit; do not describe it as an exact v0.4.5 checkout.
- Current EVO main and v0.12.5 remain the already supplied
  `875f766dc9de762c1a1f608005d0afd1f25116f4`; the adjacent EVO review records
  API receipts for that comparison.

## Adopted change: associate native VP9 output with its own input

The existing `src/hwdec_ps5.cpp` queued PTS only for visible inputs and used
the *current input's* show flag when a native picture became available.
That association is wrong when the native decoder returns a picture from
an earlier input. A deterministic one-call-delay mock reproduced a lost
visible frame followed by a hidden frame being presented under another
frame's PTS. The immediate-output baseline behaved correctly, so the test
distinguishes the defect from a generic mock or fixture failure.

The implemented backport adopts only the corresponding upstream idea:
retain a bounded FIFO of `{PTS, show}` for every successfully submitted VP9
access unit, including hidden reference frames. Each native output consumes
its own FIFO entry. Hidden and errored outputs consume their entry without
being displayed. AVC/HEVC keep the pre-existing minimum-PTS policy.

The FIFO has the existing fixed `kPtsPool` capacity, and overflow is checked
before another native submission. Failed Decode calls do not add an entry;
an unexpected output without an entry fails to the existing fallback path
instead of inventing a timestamp. Seek/flush clear the FIFO. A framebuffer's
`accepted` flag describes ownership of that output buffer and is not used as
the success condition for the input access unit.

No decoder-allocation, resident-slot, queue-creation, framebuffer-lifetime,
profile-selection or native-memory policy was replaced. The only production
file changed by this hardware review is `src/hwdec_ps5.cpp`.

### Executed verification

```sh
python3 tests/run_vp9_output_tests.py
```

Result: **PASS, 17 scenarios and 220 checks, AddressSanitizer and
UndefinedBehaviorSanitizer enabled**. The harness compiles the complete
production `PLATFORM_PS5_NATIVE` decoder implementation and substitutes the
`sceVideodec2` boundary. It uses the real installed FFmpeg VP9 superframe
filter (`libavcodec` 60.31.102 and `libavutil` 58.29.100).

The same scenarios also passed against the candidate's **pinned FFmpeg
8.1.2 host static archives**, with no production changes:

```sh
python3 tests/run_vp9_output_tests.py --ffmpeg-prefix .deps/vlc-host/prefix-host
```

This second run is **PASS, 17 scenarios and 220 checks, ASan/UBSan**.
The runner restricts pkg-config resolution to that prefix, uses `--static`,
and verifies the linked runtime library versions against the pkg-config
versions: `libavcodec` 62.28.102 and `libavutil` 60.26.102. The runtime reports
FFmpeg 8.1.2. Distribution and pinned-library reports are kept separately.

The tests cover immediate and delayed output; multiple hidden frames;
unknown, nonmonotonic and equal PTS; native output errors; public send/receive
EAGAIN and retry; native submission rejection; framebuffer `accepted=false`;
FIFO bounds and wraparound; seek reset and failed decoder renewal; orphan
drain output; real bitstream-filter buffering; a hidden+shown VP9 superframe
from a single AVPacket; unchanged AVC reordering; and invalid input lengths.

The superframe case uses two actual access units emitted by the real filter.
With a preceding visible frame and a one-call-delayed native output, the
preceding frame retains PTS 100, the hidden frame is never presented, and the
shown frame retains PTS 200 when drained.

Evidence:

- Pre-fix reproduction: `evo-review/vlc-hardware/vp9-delayed-output.log`.
- Final test log: `validation-current/upstream-vp9-output.log`.
- Pinned-library test log:
  `validation-current/upstream-vp9-output-pinned.log`.
- Structured report and source hashes:
  `Stremio-Plus-work/build/tests/vp9-output/result.json`.
- Pinned-library report and current test-source hashes:
  `Stremio-Plus-work/build/tests/vp9-output-pinned/result.json`.
- Reproducible source:
  `Stremio-Plus-work/tests/test_vp9_output.cpp` and
  `Stremio-Plus-work/tests/run_vp9_output_tests.py`.

These are host tests with a mocked hardware boundary. They do not validate
native decode timing, GPU buffer ownership or an actual PS5. In particular,
they do not establish that this defect caused the reported firmware-12.70
microstutter. Native rebuilds validate compilation against the candidate's
pinned native media dependencies separately from these host-filter tests.

## Other upstream changes assessed

| Upstream change | Compatibility and decision |
| --- | --- |
| General VP9 support and decoder recreation in `ps5/modules/ps5vdec.c` | Stremio already routes supported 8-bit VP9 through its separate native adapter and resident slots. A full replacement would change queue/memory policy and remove local validation. Retain the existing adapter and backport the proven association fix only. |
| Global reusable CPU block and memory-query cache | Upstream introduces a reusable 96 MiB CPU allocation and stores query answers before full-access setup. These are substantial lifetime and initialization-policy changes; no evidence here warrants replacing the local protected allocation path. |
| Compute-queue creation | Upstream retries when the global queue is null. Local code deliberately caches failure and avoids retry after the credential context changes. Preserve that protection. |
| H.264 field-coded 1080i | Upstream parses SPS/slice field information, pairs fields for timestamp accounting, marks interlacing/top-field order and uses I420 so VLC's deinterlacer can operate. This concerns field-coded 1080i, not evidence for a progressive-1080p timing failure. Importing it requires paired/unpaired field, MBAFF, SPS-change and malformed/truncated-header tests and progressive regressions. No such parser replacement was made. |
| Shorter input-clock gap, patch `0022-clock-ps5-shorter-gap.patch` | Changes `CR_MAX_GAP` from 60 s to 10 s for unannounced stream-clock discontinuities, with an upstream example of a 35 s timestamp jump at an ad break. It resets clock reference/drift and buffering when consecutive stream clock references differ by over 10 s. This does not adjust ordinary frame cadence and is not supported as a cure for this reported microstutter. Not imported by this review. |
| Output refresh matching | Upstream requests 23.976/24/50 Hz through a Vulkan/Mesa hook, waits for the device, measures vblank and rejects deviations over 5%. Stremio's existing OpenGL output integration additionally verifies resolution, rolls back an incompatible mode and stops unsafe rendering if rollback fails. A direct replacement would lose those protections. No display-mode policy was changed. |
| Neutralizing decoder framebuffers before creation | A narrowly scoped initialization after allocation validation and before native decoder creation can be compatible. It must not clear buffers while the decoder owns them, and must retain overflow/alignment/null checks and the current allocation fallback. The playback reviewer owns any such adaptation. A profile-2 neutral value is not proof of the same neutral color for every accepted 8-bit HEVC stream. |

## Remaining console checks

Exercise a known VP9 alt-reference/superframe sample on the PS5 and confirm
shown-frame order through play, pause, seek and drain. Record the selected
decoder, stream dimensions/rate and output cadence. Compare a controlled
1080p sample with hardware and software decode using the candidate's
diagnostics. Repeat mode changes and stop/restart on the user's display to
validate the unchanged rollback and resident-buffer protections. These
remain console work, not locally completed tests.
