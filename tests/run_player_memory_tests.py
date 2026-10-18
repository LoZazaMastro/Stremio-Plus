#!/usr/bin/env python3
"""Decode real 4K Main10 HEVC using the native allocator and bounded picture pool."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--baseline-source", type=Path,
                    help="Compare the previous native/vlc_memory.c on the same real HEVC path")
parser.add_argument("--fixture", type=Path, help="Reuse the exact synthetic fixture from a prior comparison run")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
baseline = args.baseline_source is not None
output = root / "build/tests" / ("player-memory-baseline" if baseline else "player-memory")
output.mkdir(parents=True, exist_ok=True)
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
flags_text = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
flags = []
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search(r"^" + name + r" = (.*)$", flags_text, re.MULTILINE)
    if match:
        flags += shlex.split(match[1])
if not any("STREMIO_USE_LIBVLC" in flag for flag in flags):
    raise SystemExit("This test requires a static VLC host application build.")
main_object = "CMakeFiles/stremio.dir/src/main.cpp.o"
player_object = "CMakeFiles/stremio.dir/src/player_vlc.cpp.o"
if main_object not in link or player_object not in link:
    raise SystemExit("The host link does not contain the production Player and main objects.")
memory_source = args.baseline_source.resolve() if baseline else root / "native/vlc_memory.c"
files = [root / "tests/test_player_memory.cpp", Path(__file__).resolve(), root / "native/heap.c",
         root / "native/dlmalloc.c", memory_source, root / "src/native_memory.h", root / "src/diagnostics_native.h",
         *(root / "src" / name for name in ("player.h", "player_vlc.cpp", "player_vlc_audio.h"))]
commands = []


def run(command, **kwargs):
    commands.append(list(map(str, command)))
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


test_obj, player_obj = output / "test_player_memory.o", output / "player_vlc.o"
heap_obj, memory_obj, binary = output / "heap.o", output / "vlc_memory.o", output / "test_player_memory"
run([link[0], *flags, "-O1", "-g", *( ["-DSTREMIO_MEMORY_BASELINE=1"] if baseline else []),
     "-c", root / "tests/test_player_memory.cpp", "-o", test_obj])
# This file uses PLATFORM_PS5_NATIVE only to select the native four-thread
# libavcodec setting. The actual source, VLC demux/decode and video output run.
run([link[0], *flags, "-O2", "-g", "-DPLATFORM_PS5_NATIVE=1", "-c",
     root / "src/player_vlc.cpp", "-o", player_obj])
for source, obj in ((root / "native/heap.c", heap_obj), (memory_source, memory_obj)):
    run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-g", "-pthread", "-c", source, "-o", obj])
link[link.index(main_object)] = str(test_obj)
link[link.index(player_object)] = str(player_obj)
link[link.index("-o") + 1] = str(binary)
link += [str(heap_obj), str(memory_obj)]
for symbol in ("malloc", "free", "calloc", "realloc", "reallocf", "posix_memalign", "memalign",
               "aligned_alloc", "valloc", "malloc_usable_size", "stremio_diagnostics_heap_snapshot",
               "stremio_diagnostics_allocation_failure"):
    link.append("-Wl,--wrap=" + symbol)
run(link, cwd=build)
binary.chmod(binary.stat().st_mode | 0o111)

with tempfile.TemporaryDirectory(prefix="stremio-player-memory-") as directory:
    fixture = Path(directory) / "main10.mkv"
    generation = ["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                  "color=c=0x5c80a0:size=3840x2160:rate=12:duration=6", "-vf", "format=yuv420p10le",
                  "-c:v", "libx265", "-preset", "ultrafast", "-profile:v", "main10", "-x265-params",
                  "pools=1:frame-threads=1:log-level=error:keyint=12:repeat-headers=1",
                  "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
                  "-color_range", "tv", "-an", fixture]
    if args.fixture:
        shutil.copyfile(args.fixture.resolve(), fixture)
    else:
        run(generation, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    shutil.copyfile(fixture, output / "main10.mkv")
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy", LIBGL_ALWAYS_SOFTWARE="1", EGL_PLATFORM="surfaceless",
                       NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost", LP_NUM_THREADS="4")
    result = subprocess.run([str(binary), directory, str(root / "app"), str(fixture)], env=environment,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
    (output / "test-output.txt").write_text(result.stdout)
    if (Path(directory) / "diagnostics").is_dir():
        shutil.copytree(Path(directory) / "diagnostics", output / "diagnostics", dirs_exist_ok=True)
    cases = [json.loads(line[len("PLAYER_MEMORY_RESULT "):]) for line in result.stdout.splitlines()
             if line.startswith("PLAYER_MEMORY_RESULT ")]
    for case in cases:
        case["heap_sampling"] = "Main-thread checkpoints after frame acquisition and after close. Failure callbacks do not sample allocator state."
        if not case["frames"]:
            case["minimum_heap_free"] = None
    errors = []
    expected = bool(cases) and cases[0]["allocation_failures"] > 0 if baseline else (
        result.returncode == 0 and bool(cases) and cases[0]["result"] == "PASS")
    if not expected:
        errors.append("The previous arena exhaustion was not reproduced." if baseline else
                      "Real native-allocator HEVC playback checks did not complete.")
    report = {"result": "FAIL" if errors else "BUG_REPRODUCED" if baseline else "PASS",
              "scope": "Real Player, static VLC/FFmpeg HEVC Main10, native 1280 MiB dlmalloc arena, bounded native picture mappings and render-frame leases",
              "boundaries": ["PS5 direct-memory syscalls are backed by separately owned host mmap mappings with a 2304 MiB logical physical budget.",
                             "A 256 MiB allocation represents simultaneous application and network memory pressure; it does not simulate torrent throughput.",
                             "The player uses the exact native four-thread setting; hardware decode is not present on the host.",
                             "The harness replaces main.cpp. It acquires and verifies real video planes without initializing host LLVM/llvmpipe; GPU upload is covered separately.",
                             "Host completion does not establish PS5 playback performance or hardware stability."],
              "cases": cases, "exit_code": result.returncode, "errors": errors,
              "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
              "commands": commands}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(result.stdout[-8000:])
    print(output / "result.json")
    if errors:
        raise SystemExit(" ".join(errors))
