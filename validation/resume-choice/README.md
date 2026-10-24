# Local verification — 0.5.20

This directory contains the completed local build/test/package evidence for
Stremio Plus0.5.20, titlePPSA74126, content version00.070.000.
It does not turn the previous0.5.19 test reports into new executions.

- `final-builds.json`: completed desktop, static host-VLC and native builds.
- `final-resume-app-tests.json`: five final App suites, measured durations and
  unchanged hashes of all linked production objects before/after the tests.
- `final-playback-choice-vlc.log`: same choice logic on the final host-VLC target.
- `final-player-cadence.*`: real VLC/FFmpeg1080p, independent picture references,
  one controlled350ms output stall; nominal FPS remains independent of cadence.
- `video-gl-*.json` / `.log`: real host GL shader/readback and API fault injection,
  including rectangle planes, nonuniform/padded/odd inputs and native-size model.
- `ps5vdec-startup.json`: production startup under ASan/UBSan with simulated
  Sony imports and independent native-memory ownership ledger.
- `diagnostics-tests.log`: bounded/redacted JSON runtime and release identity.
- `log-evidence-1080p.json`, `1080p-long-analysis.json`: sanitized numeric evidence
  from the user's actual0.5.19 PS5 session in Downloads(4).zip. Historical
  sessions and duplicated text/JSON logs are not counted as new frames.
- `decoder-and-mesa-findings.md`: source evidence for limited AVC compatibility
  retry and the pinned driver/Mesa layout decision. SDK source references are
  reproducible from the existing dependency archive, not a new driver build.
- `evo-architecture-review.md`: read-only review of the attached EVO snapshot.
- `diagnostics-review.md`: independent review, not another test execution.
- `final-native/`: app/source/dependency receipt, image deep-unpack comparison,
  symbol identities, address lookups and original verification logs.
- `changes-from-0.5.19.*`: exact delivered-source comparison and baseline identity.

The source archive excludes test executables, temporary video fixtures, the
duplicate unpacked app and raw user account/playback log archives.
Some reports retain original workspace paths as provenance. Use neighboring
files, the matching symbols ZIP and BUILDING.md to reproduce the checks.

## Explicit boundaries

The native app compiles and its package is verified. No execution of0.5.20 on
physical PS5 hardware is claimed. Host GL is not the PS5 driver in execution;
Sony API fault fixtures do not establish hardware configuration acceptance.
Final improvement in upload time, presentation cadence, firmware compatibility,
HDMI/audio behavior and sustained console stability requires the owner's test.

Global DWARF verification remains FAIL (exit1,37831 errors). Thirteen address
lookups and six individual-object DWARF checks passed separately. The log is
preserved; symbols match the packaged app, but complete DWARF validity is not
claimed. The pre-existing nonzero-origin SPS crop diagnostic from0.5.19 is also
not reclassified as fixed or passed.

No GitHub mutation, branch, commit, workflow, release or store request was made.
