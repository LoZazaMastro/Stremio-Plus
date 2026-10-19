#!/usr/bin/env python3
"""Inject real libavcodec critical returns and verify libVLC/application recovery."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/host-vlc"
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
flags_text = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
flags = []
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search("^" + name + r" = (.*)$", flags_text, re.MULTILINE)
    if match:
        flags += shlex.split(match[1])
if not any("STREMIO_USE_LIBVLC" in flag for flag in flags):
    raise SystemExit("Requires the static libVLC host build")
output = root / "build/tests/player-decoder-recovery"
output.mkdir(parents=True, exist_ok=True)
obj, binary = output / "test.o", output / "test"
subprocess.run([link[0], *flags, "-O1", "-g", "-c",
                str(root / "tests/test_player_decoder_recovery.cpp"), "-o", str(obj)], check=True)
link[link.index("CMakeFiles/stremio.dir/src/main.cpp.o")] = str(obj)
link[link.index("-o") + 1] = str(binary)
link += ["-Wl,--wrap=avcodec_send_packet", "-Wl,--wrap=libvlc_media_player_set_media"]
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix="stremio-decoder-recovery-") as directory:
    fixture = Path(directory) / "video.mkv"
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=24:duration=18", "-vf", "format=yuv420p10le",
        "-c:v", "libx265", "-preset", "ultrafast", "-profile:v", "main10", "-x265-params",
        "pools=2:frame-threads=2:log-level=error:keyint=24:bframes=4:ref=3", "-an", str(fixture)],
        check=True, capture_output=True, timeout=70)
    result = subprocess.run([str(binary), directory, str(root / "app"), str(fixture)],
        capture_output=True, text=True, timeout=45, env=dict(os.environ, SDL_AUDIODRIVER="dummy"))
    logs = "".join(path.read_text(errors="replace") for path in (Path(directory) / "logs").glob("*.txt"))
    text = result.stdout + result.stderr
    (output / "output.txt").write_text(text + "\n" + logs)
    print(text[-5000:])
    result.check_returncode()
    line = next(line for line in result.stdout.splitlines() if line.startswith("DECODER_RECOVERY_RESULT "))
    report = json.loads(line[len("DECODER_RECOVERY_RESULT "):])
    report["scope"] = "Real Player and patched static VLC/FFmpeg, local HEVC Main10 playback and retained frame leases"
    report["fault_boundary"] = "avcodec_send_packet returns ENOMEM once after playback starts, then EINVAL after recovery; all other decoding and media control are real"
    report["limits"] = "Host software decode. Real allocator exhaustion is tested separately; this does not establish PS5 hardware stability."
    report["fixture_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    report["source_sha256"] = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (root / "src/player_vlc.cpp", root / "vendor/vlc-ps5/patches/0023-stremio-decoder-error.patch",
                     root / "tests/test_player_decoder_recovery.cpp", Path(__file__).resolve())}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
