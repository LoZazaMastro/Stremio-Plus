#!/usr/bin/env python3
"""Verify native-audio failure callbacks and the PCM ownership gate in the real Player."""
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
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/host"
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
flags_text = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
flags = []
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search("^" + name + r" = (.*)$", flags_text, re.MULTILINE)
    if match:
        flags += shlex.split(match[1])
if not any("STREMIO_USE_LIBVLC" in flag for flag in flags):
    raise SystemExit("Requires the static VLC host build")
output = root / "build/tests/player-audio-lifecycle"
output.mkdir(parents=True, exist_ok=True)
test_object, player_object, binary = output / "test.o", output / "player.o", output / "test"
sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for source, obj in ((root / "tests/test_player_audio_lifecycle.cpp", test_object),
                    (root / "src/player_vlc.cpp", player_object)):
    subprocess.run([link[0], *flags, "-DPLATFORM_PS5=1", "-O1", "-g", *sanitizers,
                    "-c", str(source), "-o", str(obj)], check=True)
link[link.index("CMakeFiles/stremio.dir/src/main.cpp.o")] = str(test_object)
link[link.index("CMakeFiles/stremio.dir/src/player_vlc.cpp.o")] = str(player_object)
link[link.index("-o") + 1] = str(binary)
link += sanitizers
for symbol in ("libvlc_media_player_new", "libvlc_media_player_set_media", "libvlc_media_player_stop",
               "SDL_OpenAudioDevice", "SDL_CloseAudioDevice"):
    link.append("-Wl,--wrap=" + symbol)
subprocess.run(link, cwd=build, check=True)
print("PLAYER_AUDIO_LIFECYCLE_LINK_READY", flush=True)
with tempfile.TemporaryDirectory(prefix="stremio-player-audio-lifecycle-") as directory:
    fixture = Path(directory) / "video.mkv"
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=24:duration=8", "-vf", "format=yuv420p10le",
        "-c:v", "libx265", "-preset", "ultrafast", "-profile:v", "main10", "-x265-params",
        "pools=2:frame-threads=2:log-level=error:keyint=24:bframes=4:ref=3", "-an", str(fixture)],
        check=True, capture_output=True, timeout=60)
    result = subprocess.run([str(binary), directory, str(root / "app"), str(fixture)],
        capture_output=True, text=True, timeout=40,
        env=dict(os.environ, SDL_AUDIODRIVER="dummy", ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                 UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
    text = result.stdout + result.stderr
    logs = "".join(path.read_text(errors="replace") for path in (Path(directory) / "logs").glob("*.txt"))
    (output / "output.txt").write_text(text + "\n" + logs)
    print(text[-6000:])
    result.check_returncode()
    line = next(line for line in result.stdout.splitlines() if line.startswith("PLAYER_AUDIO_LIFECYCLE_RESULT "))
    report = json.loads(line[len("PLAYER_AUDIO_LIFECYCLE_RESULT "):])
    report["sanitizers"] = ["AddressSanitizer", "UndefinedBehaviorSanitizer"]
    report["scope"] = "Real Player worker, static VLC/FFmpeg, actual registered audio-mode callback and SDL dummy audio device"
    report["fault_boundary"] = "Native audio availability and shutdown notification, including injection during libvlc_stop when input.abort is already true"
    report["limits"] = "Host callback/ownership integration. Native HDMI transport is tested separately; no physical PS5 stability claim."
    report["fixture_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    report["source_sha256"] = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (root / "src/player_vlc.cpp", root / "tests/test_player_audio_lifecycle.cpp", Path(__file__).resolve())}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
