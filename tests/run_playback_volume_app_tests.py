#!/usr/bin/env python3
"""Test real App/UI persistence and volume in PCM decoded by the built backend.

The receiver-route decision is mocked; encoded HDMI output remains a PS5 test.
Both the desktop FFmpeg and patched host VLC builds use this same runner.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import wave

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
flags = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
compile_flags = []
for key in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search(r"^" + key + r" = (.*)$", flags, re.MULTILINE)
    if match:
        compile_flags.extend(shlex.split(match[1]))
backend = "vlc" if any("STREMIO_USE_LIBVLC" in flag for flag in compile_flags) else "ffmpeg"
output = root / "build/tests" / ("playback-volume-app-" + backend)
output.mkdir(parents=True, exist_ok=True)
obj, binary = output / "volume.o", output / "volume"
subprocess.run([link[0], *compile_flags, "-O1", "-g", "-c",
    str(root / "tests/test_playback_volume_app.cpp"), "-o", str(obj)], check=True, timeout=120)
link[link.index("CMakeFiles/stremio.dir/src/main.cpp.o")] = str(obj)
link[link.index("-o") + 1] = str(binary)
for symbol in ("curl_easy_perform", "_ZNK6Player24audio_passthrough_activeEv", "SDL_OpenAudioDevice", "SDL_QueueAudio"):
    link.append("-Wl,--wrap=" + symbol)
subprocess.run(link, cwd=build, check=True, timeout=120)
with tempfile.TemporaryDirectory(prefix="stremio-volume-app-") as directory:
    media = Path(directory) / "constant-stereo.wav"
    with wave.open(str(media), "wb") as writer:
        writer.setnchannels(2); writer.setsampwidth(2); writer.setframerate(48000)
        writer.writeframes(struct.pack("<hh", 12000, -12000) * (48000 * 20))
    run = subprocess.run([str(binary), directory, str(media), str(root / "app/hui/fonts")], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=dict(os.environ, SDL_AUDIODRIVER="dummy"), timeout=40)
    (output / "output.txt").write_text(run.stdout)
    print(run.stdout, end="")
    run.check_returncode()
    result = json.loads(run.stdout.splitlines()[-1])
sources = [root / name for name in ("src/playback_volume.h", "src/player_vlc_audio.h", "src/player.h",
    "src/player_vlc.cpp" if backend == "vlc" else "src/player.cpp", "src/app.h", "src/app.cpp",
    "src/app_watch.cpp", "src/home_ui.cpp", "vendor/ps5-homebrew-ui/src/core/input.hpp",
    "vendor/ps5-homebrew-ui/src/core/input.cpp", "tests/test_playback_volume_app.cpp", "tests/run_playback_volume_app_tests.py")]
report = {"result": "PASS", "backend": backend,
    "scope": "Real App/UI input, settings round trip and decoded PCM observed at SDL output; mocked HDMI route decision",
    "results": result, "sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
target = output / "result.json"
target.write_text(json.dumps(report, indent=2) + "\n")
print(target)
