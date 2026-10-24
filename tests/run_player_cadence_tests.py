#!/usr/bin/env python3
"""Compare real 1080p VLC output after one controlled 350 ms presentation stall.

The baseline changes only --drop-late-frames back to the previous setting in
the test process. No production source or build object is rewritten.
"""
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
flags = []
flag_text = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search("^" + name + r" = (.*)$", flag_text, re.MULTILINE)
    if match:
        flags += shlex.split(match[1])
if not any("STREMIO_USE_LIBVLC" in flag for flag in flags):
    raise SystemExit("Requires the completed static VLC host build")
output = root / "build/tests/player-cadence"
output.mkdir(parents=True, exist_ok=True)
obj, binary = output / "test.o", output / "test"
subprocess.run([link[0], *flags, "-O1", "-g", "-c", str(root / "tests/test_player_cadence.cpp"),
                "-o", str(obj)], check=True)
link[link.index("CMakeFiles/stremio.dir/src/main.cpp.o")] = str(obj)
link[link.index("-o") + 1] = str(binary)
for name in ("libvlc_new", "libvlc_video_set_callbacks", "libvlc_media_player_set_media", "vlc_ps5_copy_plane"):
    link += ["-Wl,--wrap=" + name]
subprocess.run(link, cwd=build, check=True)

with tempfile.TemporaryDirectory(prefix="stremio-cadence-") as directory:
    folder = Path(directory)
    video, hashes = folder / "long-gop-1080.mkv", folder / "hashes.txt"
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-v", "error", "-f", "lavfi", "-i",
        "testsrc2=size=1920x1080:rate=24:duration=8", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=48000:duration=8", "-c:v", "libx264", "-preset", "ultrafast",
        "-threads", "2", "-x264-params", "keyint=240:min-keyint=240:scenecut=0:bframes=3:ref=3",
        "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le", str(video)], check=True, capture_output=True, timeout=90)
    decoder = subprocess.Popen(["ffmpeg", "-nostdin", "-v", "error", "-i", str(video), "-map", "0:v:0",
        "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    frame_bytes = 1920 * 1080 * 3 // 2
    values = []
    while True:
        data = decoder.stdout.read(frame_bytes)
        if not data:
            break
        if len(data) != frame_bytes:
            raise RuntimeError("Incomplete clean reference picture")
        value = 1469598103934665603
        for byte in data[::4093]:
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        values.append(value)
    if decoder.wait(timeout=10):
        raise RuntimeError(decoder.stderr.read().decode(errors="replace"))
    hashes.write_text("".join(str(value) + "\n" for value in values))
    runs = {}
    for mode in ("baseline", "fixed"):
        profile = folder / mode
        profile.mkdir()
        run = subprocess.run([str(binary), str(profile), str(root / "app"), str(video), str(hashes), mode],
            text=True, capture_output=True, timeout=25, env=dict(os.environ, SDL_AUDIODRIVER="dummy"))
        logs = "".join(path.read_text(errors="replace") for path in (profile / "logs").glob("*.txt"))
        text = run.stdout + run.stderr
        (output / (mode + ".log")).write_text(text + "\n" + logs)
        print(text[-4000:], flush=True)
        run.check_returncode()
        line = next(line for line in run.stdout.splitlines() if line.startswith("CADENCE_RESULT "))
        runs[mode] = json.loads(line[len("CADENCE_RESULT "):])
    baseline, fixed = runs["baseline"], runs["fixed"]
    if fixed["vout_lost_pictures"] <= baseline["vout_lost_pictures"]:
        raise RuntimeError("Controlled lateness did not exercise decoded-picture dropping")
    if fixed["burst_displays_50ms"] >= baseline["burst_displays_50ms"]:
        raise RuntimeError("Presentation-only dropping did not reduce the stale catch-up burst")
    if fixed["final_pickup_lateness_ms"] > 100:
        raise RuntimeError("Fixed presentation failed to catch up with its scheduled clock")
    report = {"result": "PASS", "runs": runs,
        "scope": "Real Player and patched static VLC/FFmpeg; 1080p H264 long GOP with B/P references, PCM audio and independent pixel hashes",
        "fault_boundary": "One 350 ms delay in the vmem luma copy, after steady playback; no decoder packet or native syscall is mocked",
        "limits": "Host software decode/output only. Does not establish firmware 12.7 cause or PS5 HDMI scanout cadence.",
        "fixture_sha256": hashlib.sha256(video.read_bytes()).hexdigest(),
        "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (root / "src/player_vlc.cpp", root / "tests/test_player_cadence.cpp", Path(__file__).resolve())}}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
