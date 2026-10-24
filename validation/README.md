# Validation records in the published source tree

This tree retains selected validation reports, JSON results, build receipts,
reproducible harnesses and comparison patches from the development of Stremio
Plus 0.5.19 and 0.5.20. All application source, tests, assets and dependency
patches from the original source delivery are preserved.

Raw build and test logs, the full native symbol-address inventory and temporary
DWARF-verification output are retained in the original validation archive.
They are omitted from this Git tree to keep generated diagnostic output out
of the source history. Dated reports retain their original outcomes, including
failed checks and intermediate results. Omitting raw logs does not change
those outcomes or make a host test equivalent to a PS5 test.

The final 0.5.20 build record is
[LOCAL_0_5_20_VALIDATION.md](../docs/LOCAL_0_5_20_VALIDATION.md).
The earlier [resume-choice snapshot](../docs/LOCAL_0_5_20_RESUME_CHOICE.md)
predates the final video-upload correction and remains historical.

## Original archive index

The original index below is preserved as a historical record of the complete
validation archive. Its inventories, checksums and references to raw logs
refer to that original archive, which includes the generated files omitted
from this Git tree. Its dated publication status describes the time of those
checks rather than the current release status.

---

> **Current candidate0.5.20:** completed builds, tests, package verification and
> the new log-based analysis are in [`resume-choice/`](resume-choice/).
> The sections below describe the historical0.5.19 verification archive.

# Local verification evidence

This directory preserves selected, previously completed checks for the local
Stremio Plus 0.5.19 candidate, title `PPSA74126`. The evidence was collected
on 23 October 2026. Organizing this archive did **not** run another test or
build, modify production code, or contact an account service.

The initial selection contains **60 text files** (944,929 bytes before this
index and the generated inventories). It contains logs, reports and public
upstream API receipts. Test binaries, media fixtures, dependency caches and
account data are not included.

## Index and provenance

| Path | Contents and boundary |
| --- | --- |
| [`baseline-host/`](baseline-host/) | Earlier desktop and host-VLC verification of Home, account, UI and volume, with the corresponding host build summary. These predate the completed upstream-backport verification phase. |
| [`baseline-safety/`](baseline-safety/) | Previously completed allocator, native startup, decoder startup and native-audio harnesses. Sony kernel/decoder/HDMI boundaries are simulated on the host. |
| [`backport-vp9/`](backport-vp9/) | Before-fix reproduction and two completed FIFO regression runs, using distribution FFmpeg and pinned static FFmpeg 8.1.2 respectively. |
| [`upstream/evo/`](upstream/evo/) | EVO review and selected original public GitHub API responses: main, release, tag, main comparison and relevant branch comparisons. |
| [`upstream/vlc/`](upstream/vlc/) | VLC review, release/main API responses, remote-ref verification, tag-to-main diff summary and official VideoLAN source checksum. Large geometry/cadence evidence is managed separately by the final assembly. |
| [`upstream/vlc-hardware/REVIEW.md`](upstream/vlc-hardware/REVIEW.md) | Independent hardware, VP9 metadata, interlace, clock and display review, including the scope of the selected backport. |
| [`inventory-selected.json`](inventory-selected.json) | Exact list of the 60 copied files, their original workspace paths, phases, sizes and SHA-256 values. No inferred test result is added to the copied data. |
| [`SHA256SUMS-selected`](SHA256SUMS-selected) | Checksums of those 60 copied files only. Later final-assembly additions are outside this checksum list. |
| [`source-comparison-selected.json`](source-comparison-selected.json) | Comparison of source hashes already recorded in 12 selected test reports with the source tree at archive time. This is a content comparison, not a new execution or package attestation. |

## Recorded results

All counts below come from the included reports or logs; they do not count
archiving as another run.

| Check | Included evidence | Recorded result |
| --- | --- | --- |
| Home startup, desktop FFmpeg | `baseline-host/home-startup-ffmpeg/` | PASS, 32 checks |
| Home startup, host VLC | `baseline-host/home-startup-vlc/` | PASS, 32 checks |
| Account integration | `baseline-host/account.log` | PASS, 102 checks, zero real HTTP requests |
| App interaction integration | `baseline-host/interactions.log` | PASS, 102 checks, zero real HTTP requests |
| Desktop UI/controller integration | `baseline-host/desktop-ui.log` | PASS; the log does not give a numeric check total |
| Volume policy and signed PCM gain | `baseline-host/playback-volume.json` and `playback-volume-helper.log` | PASS, 29 checks, ASan/UBSan |
| Timestamped PCM and YUV copy regression | `baseline-host/vlc-output.json` and `vlc-output.log` | PASS, 21 checks, ASan/UBSan |
| Real App volume and captured decoded PCM, FFmpeg | `baseline-host/playback-volume-app-ffmpeg/` | PASS, 71 checks |
| Real App volume and captured decoded PCM, VLC | `baseline-host/playback-volume-app-vlc/` | PASS, 71 checks |
| Native heap wrappers and ownership | `baseline-safety/native-heap/` | PASS, 921 checks; the 30 recorded failures are deliberately injected allocation failures |
| Native video-memory mapper | `baseline-safety/native-video-memory/` | PASS, 14 scenarios and 93,039 checks, ASan/UBSan |
| Native audio with mocked Sony HDMI imports | `baseline-safety/ps5-audio/` | PASS, 21 scenarios and 2,198 checks, ASan/UBSan |
| Native startup scripts and ordering gates | `baseline-safety/native-startup.log` | PASS, 5 tests |
| Decoder startup fault injection | `baseline-safety/ps5vdec-startup.json` and `.log` | PASS, 62 scenarios and 8,234 checks, ASan/UBSan |
| VP9 output association, distribution FFmpeg | `backport-vp9/vp9-output/` | PASS, 17 scenarios and 220 checks, ASan/UBSan |
| VP9 output association, pinned static FFmpeg 8.1.2 | `backport-vp9/vp9-output-pinned/` | PASS, the same 17 scenarios and 220 checks, ASan/UBSan |

The App volume checks capture real decoded PCM at the SDL output boundary.
They verify full, half, quarter and zero gain, saved settings, actual input
tracking and unchanged L2/R2 actions. The decision that a passthrough route
is active is mocked; the tests do not play sound through a physical receiver.

The allocator checks use production allocation code with host-backed
mappings and independent ownership ledgers. Their kernel API fixtures do
not reproduce a console driver's memory acceptance or sustained performance.

The account tests exercise the application locally with controlled responses.
They do not establish the remote service's revocation behavior after logging
out in a phone browser. That sequence still requires a real account and PS5.

## Earlier hashes and final package identity

`baseline-host/builds.json` records the **earlier** host executable hashes:

- Desktop: `0df7389f9a638f69a71c37077eb216f1e2efceffaaeca8d4eae04b1ffed90f50`.
- Host VLC: `9f9612eb646407155a3272a91d8f5b497187834fc877568a8f844ce532af1b43`.

These identify that verification phase. They are not the identity of the
final rebuilt native application or its package. Some original filenames
contained `final`; the copies here are deliberately grouped as baseline
evidence rather than presented as the final upstream rebuild.

The recorded source hashes in the selected baseline reports still matched
their listed files when this archive was prepared. That does not establish
that their earlier linked dependencies or executables equal a later build.
The final build and package receipts must provide that separate evidence.

The first VP9 run predates the test runner's optional static-prefix mode and
the addition of runtime-version reporting. Its two test-source hashes differ
from the current test files; the tested production decoder hash is unchanged.
The second, pinned-library run records the current test-source hashes and
confirms runtime FFmpeg **8.1.2**, `libavcodec` **62.28.102** and `libavutil`
**60.26.102**. Its output remains separate from the distribution-library run.

## VP9 scope and upstream findings

The FIFO change does not introduce a new codec or a new decoder-selection
policy. It corrects `HwDecoder` metadata association, including delayed
outputs and hidden frames. The existing `src/player_vlc_vp9.cpp` adapter,
module `stremiovp9`, already calls this backend for supported 8-bit VP9.
CMake includes the adapter in the PS5 target and the generated static VLC
module list registers it. The adapter is unchanged from the supplied source.
This confirms the path exists; it does not show which decoder is selected
for the user's sample on a physical console.

The VP9 harness uses the real FFmpeg superframe bitstream filter and the
production native decoder implementation, with deterministic mocked
`sceVideodec2` calls. It checks hidden+shown access units emitted from one
AVPacket, EAGAIN/retry, errors, queue limits, wraparound, seek/reset and
unchanged AVC timestamp handling. `before-fix-output.log` intentionally
records **DEFECT_REPRODUCED** for the previous algorithm; it is not a failing
run of the fixed implementation.

The selected EVO API receipts show main and v0.12.5 at the supplied
`875f766dc9de762c1a1f608005d0afd1f25116f4`, with no commits ahead of the pin.
The VLC comparison is against v0.6.0 at
`c0ffdca8351a3a7210eabae26897e36f83b26bd8`. The reviewed main revision is
recorded separately; an older VLC attachment is not assigned an exact Git
tag by inference. Public API responses preserve their original contents,
including public upstream author/release metadata.

Review documents retain references to their original analysis paths. For
the selected receipts copied here, use the adjacent archive files or
`inventory-selected.json` to resolve the original location. The central
project validation document and final assembly contain the broader result.

## Final assembly

The final records are separate from the earlier selection:

| Path | Final evidence |
| --- | --- |
| `final-native/` | Final app and engine receipts, signing/import/startup audits, 722 package comparisons, matching symbol identities and the failed global DWARF check with its full log |
| `final-host-upstream/` | Host build, object integrity, repaired patch reentry and final binary identity; earlier recovery/failure logs are explicitly retained as history |
| `final-host-tests/` | Final geometry, cadence, Main10 memory-pressure, Home and volume executions against the rebuilt player |
| `video-geometry-comparison/` | Isolated A/B comparison, independent pixel references and the separately failing pre-existing nonzero-origin H.264 crop case |
| `patch-reentry-proof/` | All 25 patches, 4,383 source files, identical generated code and unchanged second application |
| `dependency-recovery/` | Toolchain recovery, offline Git/wheel identities and five restore-helper checks |
| `dependency-bundle/` | The delivered dependency inventory, checksums and restore helper; archives themselves are in the separate dependency ZIPs |
| `preserved-safeguards.json` | Comparison of 89 selected original protection-related files: 86 byte-identical, with three changes reviewed individually |
| `changes-from-input.json` and `.patch` | Exact source/asset/doc/test differences from the supplied main archive; generated validation evidence is excluded from that comparison |
| `inventory-final-evidence.json` | Paths, sizes and hashes of final evidence copied into this directory |
| `SHA256SUMS-all` | Checksums for the complete evidence folder, except this checksum file itself |

The final host executable is
`e8abe2ecf32dd5058f60021f0ea500ce157e35905479f55bd4b3bdb93e97946e`.
Repeating setup after the hunk correction left all core archives and that
binary identical. The final native source fingerprint is
`9016a56286cf954b99942ccb067acea4cf39297c36fa288be7a69da03baf5144`.
The packaged native app and its matching symbols are identified in
`final-native/native-artifact-validation.json` and
`final-native/native-symbols-bundle.json`.

The native global DWARF verification is **not a pass**; eight sampled
address lookups and two individual-object checks passed separately.
The explicit nonzero-origin crop case is **FAIL for correct cropping** and
**PASS for no new regression**. It is not included among the five passing
regular geometry cases. Preserving these records does not convert either
failed diagnostic into a successful test.

No included report is a PS5 execution, an actual HDMI scanout or receiver
compatibility test, or proof of the cause of the reported firmware-12.70
microstutter. Console verification remains necessary before publication.
