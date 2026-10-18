# VLC PS5 playback integration

Stremio Plus 0.5.8 incorporates playback components from **ZiZc3/VLC**,
<https://github.com/ZiZc3/VLC>, whose supplied source identifies the PS5
application as version **0.4** and its media engine as **libVLC 3.0.24**.
The original project README and GPL v3 license are retained as
`UPSTREAM_README.md` and `LICENSE`. Copyright remains with the original authors.

## Source identity

The integration was prepared from the supplied `VLC-main.zip`, SHA-256:

`06a5c7f2ad085a3f272037fd7dcf07c2029a77a2bfed97b89b174df03ae7b9dc`

The archive did not identify a Git commit. It is recorded by its exact digest,
without claiming that a moving upstream branch is the same source snapshot.
The original README may contain the previous repository name `VLC-PS5`.

VideoLAN's unmodified VLC 3.0.24 source archive is pinned to SHA-256:

`e7cab503d1d7d5849b89d2cf0e1ee60d0ef6d012407791b644b9cfc0cc225fdf`

<https://download.videolan.org/pub/videolan/vlc/3.0.24/vlc-3.0.24.tar.xz>

## Components and changes

Original PS5 decoder and HTTP modules, compatibility shims, build recipes,
and VLC/dependency patches are retained in this directory. Their original
copyright headers and file-specific license notices remain in place.
The PS5 project uses GPL v3; several compatibility files expressly use
GPL-2.0-or-later. VideoLAN library portions and modules retain their own
LGPL-2.1-or-later or GPL-2.0-or-later notices. Linking them into Stremio Plus
does not replace those notices with a claim of exclusive authorship.

Stremio Plus modifications, dated **2026-10-17**, adapt this media engine to:

- the existing Prospero UI and OpenGL presentation path;
- callback-based video, audio, and custom subtitle rendering;
- the app's native torrent reader and validated growing download files;
- configured add-on HTTP request headers;
- the existing native title build, static module registration, and packaging.

Modified vendored files carry a dated change notice. The standalone VLC
frontend, Vulkan renderer, remote-upload server, and library screens are not
part of this application. Unused original patches are retained for source
provenance; the selected build configuration determines linked features.

## Corresponding source

The binary delivery includes the exact application source and these patches,
with pinned dependency source archives and build instructions. `BUILDING.md`
in the repository root describes the native build. `THIRD_PARTY.md` and
`app/licenses/` contain the dependency attribution and license texts shipped
with the application. The build receipt records the actual source and static
library identities used by the executable.
