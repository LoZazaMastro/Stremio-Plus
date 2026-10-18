#!/usr/bin/env python3
"""Exercise actual App/VLC/SDL/GL startup with controlled missing-video callbacks."""
import argparse
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path, help="Completed host libVLC CMake build")
parser.add_argument("--baseline", action="store_true", help="Reproduce the pre-fix loader/audio failure")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
link_file = build / "CMakeFiles/stremio.dir/link.txt"
if not link_file.is_file():
    raise SystemExit("Build the host application with CMake Unix Makefiles first.")
link = shlex.split(link_file.read_text())
main_object = "CMakeFiles/stremio.dir/src/main.cpp.o"
if main_object not in link:
    raise SystemExit("The host build does not contain its production main object.")
flags = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
compile_flags = []
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search(r"^" + name + r" = (.*)$", flags, re.MULTILINE)
    if match:
        compile_flags.extend(shlex.split(match[1]))
if not any("STREMIO_USE_LIBVLC" in flag for flag in compile_flags):
    raise SystemExit("This regression must run against the actual libVLC backend.")
source_include = next((flag[2:] for flag in compile_flags if flag.startswith("-I") and flag.endswith("/src")
                       and (Path(flag[2:]) / "player_vlc.cpp").is_file()), None)
if not source_include:
    raise SystemExit("Cannot identify the source tree of the completed host build.")
compiled_root = Path(source_include).parent
if args.baseline:
    compile_flags.append("-DSTREMIO_STARTUP_BASELINE=1")
output = root / "build/tests" / ("player-startup-baseline" if args.baseline else "player-startup")
output.mkdir(parents=True, exist_ok=True)
obj = output / "test_player_startup.o"
binary = output / "test_player_startup"
subprocess.run([link[0], *compile_flags, "-O1", "-g", "-c", str(root / "tests/test_player_startup.cpp"),
                "-o", str(obj)], check=True)
link[link.index(main_object)] = str(obj)
link[link.index("-o") + 1] = str(binary)
for symbol in ("libvlc_video_set_callbacks", "libvlc_media_player_set_media", "libvlc_event_attach", "SDL_OpenAudioDevice"):
    link.append("-Wl,--wrap=" + symbol)
egl = ctypes.util.find_library("EGL")
if not egl:
    raise SystemExit("The real GPU startup check requires the host EGL runtime.")
link.append("-l:" + egl)
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix="stremio-player-startup-") as directory:
    fixture = Path(directory)
    generation = subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=40",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=40",
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "25", "-g", "24", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "96k", "-movflags", "+faststart", str(fixture / "startup.mp4")],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    if generation.returncode:
        print(generation.stdout)
        raise SystemExit("Synthetic startup fixture generation failed.")
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(fixture / "startup.mp4"), "-map", "0:a:0", "-c", "copy", str(fixture / "startup.m4a")],
        check=True, timeout=15)
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy", LIBGL_ALWAYS_SOFTWARE="1", EGL_PLATFORM="surfaceless")
    result = subprocess.run([str(binary), directory, str(compiled_root / "app")], env=environment,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (output / "test-output.txt").write_text(result.stdout)
    diagnostics = output / "diagnostics"
    if diagnostics.exists():
        shutil.rmtree(diagnostics)
    if (fixture / "diagnostics").is_dir():
        shutil.copytree(fixture / "diagnostics", diagnostics)
    cases = []
    for line in result.stdout.splitlines():
        if line.startswith("STARTUP_CASE "):
            cases.append(json.loads(line[len("STARTUP_CASE "):]))
        if line.startswith(("STARTUP_CASE ", "STARTUP_TIMEOUT ", "PLAYER_STARTUP_")):
            print(line)
    marker = "PLAYER_STARTUP_BUG_REPRODUCED" if args.baseline else "PLAYER_STARTUP_TESTS_OK"
    if result.returncode or marker not in result.stdout:
        print(result.stdout[-6000:])
        raise SystemExit(result.returncode or 1)
    files = [root / "tests/test_player_startup.cpp", Path(__file__).resolve(),
             *(compiled_root / "src" / name for name in ("app.h", "app.cpp", "app_watch.cpp",
                "player.h", "player_vlc.cpp", "player_vlc_audio.h", "video_gl.cpp", "video_gl.h"))]
    report = {"result": "BUG_REPRODUCED" if args.baseline else "PASS",
        "scope": "Real App, libVLC decode, SDL PCM output, OpenGL upload and renderer acknowledgement",
        "fault_boundary": "Only video display delivery and requested cache events are intercepted; decoding and PCM output remain real",
        "entry_point": "The test harness replaces main.cpp and executes App update, frame acquisition, GL upload and acknowledgement in that order",
        "cases": cases, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
