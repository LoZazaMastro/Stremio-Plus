# EVO upstream review — 23 October 2026

## Result

The current public `main` and latest release `v0.12.5` both point to
`875f766dc9de762c1a1f608005d0afd1f25116f4`, committed on 19 October 2026
at 05:21 UTC. The release was published at 05:27:14 UTC the same day.
This is exactly the revision recorded in Stremio Plus
`vendor/evo-player/NOTICE.md` and the supplied archive's GitHub ZIP comment.
The attachment SHA256 is
`27f2b819c8dc6a0ceb3f267a7063640a5bc4ff0d88a65b204a232c50df5953ea`,
also identical to the existing attribution notice.

The GitHub comparison reports **identical**, **0 commits ahead**, **0 behind**,
and **0 changed files**. No newer EVO decoder, pacing, audio or volume patch
exists on the current main/released revision to import. This does not imply
that all EVO functionality is present in Stremio: the existing integration
intentionally adapts selected components to libVLC/OpenGL and retains its
own memory, decoder lifetime and HDMI audio safeguards.

## Relevant branch check

The following public branches were compared against the supplied pin, without
checking out or changing any repository:

| Branch | Relation to current main | New commits relative to main |
| --- | --- | ---: |
| feat/6-video-buffers-direct-mem | 344 commits behind | 0 |
| feat/27-agc-submit-watchdog | 320 commits behind | 0 |
| feat/37-video-decoder-setting | 257 commits behind | 0 |
| fix/57-4k-seek-sps-reinject | 340 commits behind | 0 |
| claude/nifty-hopper-xtpgp6 | 76 commits behind | 0 |
| research/hardware-decode | diverged, 504 commits behind | 8 old research commits |

The hardware research branch's exclusive commits date from 11 August to
1 September 2026. They are older research/probe documentation, not a newer
release or a verified firmware 12.70 playback fix. They are not a suitable
replacement for the production decoder or ownership guards.

## Concrete patch decision

**No production diff proposed.** The released upstream delta is empty.
Do not reintroduce EVO's AGC renderer, frame interpolation, experimental audio
transports, or decoder lifetime policy under the label of an upstream update.
No application build or source was changed by this review.

No new executable validation is required for this empty delta. Existing local
Stremio tests remain applicable. Actual PS5 firmware 12.70 validation of
hardware/software decoder selection, 1080p cadence, seek/reopen, PCM gain and
HDMI passthrough remains outstanding; this upstream comparison supplies no
hardware measurement and proves no cause for the reported microstutter.

## Primary evidence

- Main commit API: https://api.github.com/repos/sainsaji/EVO-PLAYER-PS5/commits/main
- Main commit: https://github.com/sainsaji/EVO-PLAYER-PS5/commit/875f766dc9de762c1a1f608005d0afd1f25116f4
- Comparison API: https://api.github.com/repos/sainsaji/EVO-PLAYER-PS5/compare/875f766dc9de762c1a1f608005d0afd1f25116f4...main
- Release tag API: https://api.github.com/repos/sainsaji/EVO-PLAYER-PS5/git/ref/tags/v0.12.5
- Latest release API: https://api.github.com/repos/sainsaji/EVO-PLAYER-PS5/releases/latest
- Release: https://github.com/sainsaji/EVO-PLAYER-PS5/releases/tag/v0.12.5
- Branch list: https://api.github.com/repos/sainsaji/EVO-PLAYER-PS5/branches?per_page=100

The adjacent JSON files preserve the actual API responses, branch comparisons,
retrieval time and evidence hashes. The initial terminal request was denied by
sandbox networking; the same public, read-only GETs succeeded with approved
network access. No credentials or GitHub mutations were used.
