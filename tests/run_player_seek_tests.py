#!/usr/bin/env python3
"""Compare real Player frame cadence and FFmpeg discard policy across HEVC seeks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--baseline-plugin", type=Path,
                    help="Use the saved unpatched VLC avcodec plugin with otherwise identical dependencies")
parser.add_argument("--fixture", type=Path, help="Reuse the exact synthetic video from the baseline run")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
output = args.output.resolve() if args.output else root / "build/tests" / (
    "player-seek-baseline" if args.baseline_plugin else "player-seek")
output.mkdir(parents=True, exist_ok=True)
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
flags = []
flag_text = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search("^" + name + r" = (.*)$", flag_text, re.MULTILINE)
    if match:
        flags += shlex.split(match[1])
if not any("STREMIO_USE_LIBVLC" in flag for flag in flags):
    raise SystemExit("Requires the static VLC host build")
obj, binary = output / "test.o", output / "test"
subprocess.run([link[0], *flags, "-O1", "-g", "-c", root / "tests/test_player_seek.cpp", "-o", obj], check=True)
link[link.index("CMakeFiles/stremio.dir/src/main.cpp.o")] = str(obj)
link[link.index("-o") + 1] = str(binary)
plugin = next(path for path in link if path.endswith("libavcodec_plugin.a"))
if args.baseline_plugin:
    link[link.index(plugin)] = str(args.baseline_plugin.resolve())
    plugin = str(args.baseline_plugin.resolve())
link.append("-Wl,--wrap=avcodec_send_packet")
subprocess.run(link, cwd=build, check=True)
print("PLAYER_SEEK_LINK_READY " + str(binary), flush=True)
fixture = args.fixture.resolve() if args.fixture else output / "seek-main10.mkv"
if not args.fixture:
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-filter_threads", "1",
        "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=24:duration=16", "-vf", "format=yuv420p10le",
        "-c:v", "libx265", "-preset", "ultrafast", "-profile:v", "main10", "-x265-params",
        "pools=2:frame-threads=2:log-level=error:keyint=120:min-keyint=120:scenecut=0:"
        "bframes=8:b-adapt=0:b-pyramid=0:ref=4:rc-lookahead=12:open-gop=0",
        "-an", "-reserve_index_space", "4096", "-cues_to_front", "1", str(fixture)],
        check=True, capture_output=True, timeout=60)
oracle = output / "hashes.txt"
hashes = []
with subprocess.Popen(["ffmpeg", "-nostdin", "-v", "error", "-i", str(fixture), "-pix_fmt", "yuv420p10le",
                       "-f", "rawvideo", "-"], stdout=subprocess.PIPE, stderr=subprocess.PIPE) as decoder:
    frame_size = 640 * 360 * 3
    while True:
        frame = decoder.stdout.read(frame_size)
        if not frame:
            break
        if len(frame) != frame_size:
            raise RuntimeError("The independent frame oracle ended mid-frame")
        value = 1469598103934665603
        for byte in frame[::1223]:
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        hashes.append(value)
    decoder.wait(timeout=10)
    if decoder.returncode:
        raise RuntimeError("The independent HEVC frame decoder failed")
oracle.write_text("".join(str(value) + "\n" for value in hashes))
result = subprocess.run([str(binary), str(output), str(root / "app"), str(fixture), str(oracle)],
    capture_output=True, text=True, timeout=40, env=dict(os.environ, SDL_AUDIODRIVER="dummy"))
text = result.stdout + result.stderr
logs = "".join(path.read_text(errors="replace") for path in (output / "logs").glob("*.txt"))
(output / "output.txt").write_text(text + "\n" + logs)
print(text[-6000:])
cases = [json.loads(line[len("SEEK_STAGE "):]) for line in result.stdout.splitlines() if line.startswith("SEEK_STAGE ")]
summary = next((json.loads(line[len("SEEK_RESULT "):]) for line in result.stdout.splitlines()
                if line.startswith("SEEK_RESULT ")), {})
reproduced = result.returncode == 1 and summary.get("degraded_stages") == 2 and len(cases) == 3 and all(
    case["nonref_discard_packets"] > 12 and case["default_discard_packets"] == 0 and case["unique_fps"] < 12
    for case in cases[1:])
passed = reproduced if args.baseline_plugin else result.returncode == 0 and summary.get("result") == "PASS"
report = {
    "result": "BUG_REPRODUCED" if passed and args.baseline_plugin else "PASS" if passed else "FAIL",
    "process_exit_code": result.returncode,
    "scope": "Real Player, real static VLC/FFmpeg HEVC Main10 playback and forward/backward seeks; read-only avcodec_send_packet observation",
    "limits": "Synthetic 640x360 host software decode; verifies frame-selection regression and frame leases, not physical PS5 rendering speed or boot",
    "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
    "fixture_frames": len(hashes),
    "avcodec_plugin_sha256": hashlib.sha256(Path(plugin).read_bytes()).hexdigest(),
    "player_object_sha256": hashlib.sha256((build / "CMakeFiles/stremio.dir/src/player_vlc.cpp.o").read_bytes()).hexdigest(),
    "source_sha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (root / "src/player_vlc.cpp", root / "tests/test_player_seek.cpp", Path(__file__).resolve())},
    "stages": cases, "summary": summary,
}
(output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print("PLAYER_SEEK_RESULT " + report["result"])
if not passed:
    raise SystemExit(1)
