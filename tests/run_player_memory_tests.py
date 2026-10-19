#!/usr/bin/env python3
"""Measure real HEVC playback with bounded native heap and driver memory pressure."""
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
                    help="Previous source tree (heap, picture allocator and player), or a previous vlc_memory.c")
parser.add_argument("--baseline-heap", type=Path,
                    help="Previous heap.c when --baseline-source identifies only a picture allocator file")
parser.add_argument("--baseline-player", type=Path,
                    help="Previous player_vlc.cpp when comparing separately retained source files")
parser.add_argument("--fixture", type=Path, help="Reuse the exact synthetic fixture from a previous run")
parser.add_argument("--heap-pressure-mib", type=int, default=512)
parser.add_argument("--graphics-pressure-mib", type=int, default=384)
parser.add_argument("--graphics-layout", choices=("contiguous", "split"), default="split",
                    help="Place driver memory first, or in the center leaving two free extents")
parser.add_argument("--direct-capacity-mib", type=int, default=2304)
parser.add_argument("--sessions", type=int, default=3)
parser.add_argument("--frames", type=int, default=72)
parser.add_argument("--cycle-timeout", type=float, default=35)
parser.add_argument("--expect-exhaustion", action="store_true",
                    help="Require a measured ENOMEM from real playback to reproduce the baseline bug")
parser.add_argument("--output", type=Path, help="Keep separate reports for each explicitly chosen pressure scenario")
args = parser.parse_args()
if not (0 <= args.heap_pressure_mib <= 1024 and 0 <= args.graphics_pressure_mib < args.direct_capacity_mib):
    parser.error("Pressure values must fit the test's bounded allocations.")
if args.sessions < 1 or args.frames < 24 or args.cycle_timeout <= 0:
    parser.error("Use at least one session, 24 frames and a positive timeout.")
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
baseline = args.baseline_source is not None
name = "player-memory-baseline" if baseline else "player-memory"
output = args.output.resolve() if args.output else root / "build/tests" / name
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
    raise SystemExit("The host link must contain the production Player and main objects.")
source_root = args.baseline_source.resolve() if baseline and args.baseline_source.is_dir() else root
memory_source = (args.baseline_source.resolve() if baseline and args.baseline_source.is_file()
                 else source_root / "native/vlc_memory.c")
heap_source = args.baseline_heap.resolve() if args.baseline_heap else source_root / "native/heap.c"
player_source = args.baseline_player.resolve() if args.baseline_player else source_root / "src/player_vlc.cpp"
files = [root / "tests/test_player_memory.cpp", Path(__file__).resolve(), heap_source,
         heap_source.with_name("dlmalloc.c"), memory_source, memory_source.parent.parent / "src/native_memory.h",
         heap_source.parent.parent / "src/diagnostics_native.h", player_source,
         *(player_source.parent / name for name in ("player.h", "player_vlc_audio.h"))]
source_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
commands = []


def run(command, **kwargs):
    commands.append(list(map(str, command)))
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


# Inspection is compiled only into the test executable. The selected production
# heap source is included verbatim, without changing its allocation decisions.
heap_probe = output / "heap_probe.c"
heap_probe.write_text("""#define MALLOC_INSPECT_ALL 1
#include %s
static void test_free_chunk(void *start, void *end, size_t used, void *context) {
    size_t *largest = context;
    if (!used && (uintptr_t)end > (uintptr_t)start) {
        size_t length = (uintptr_t)end - (uintptr_t)start;
        if (length > *largest) *largest = length;
    }
}
void stremio_test_heap_probe(size_t *used, size_t *available, size_t *largest) {
    *used = *available = *largest = 0;
    if (g_heap) {
        const struct mallinfo usage = mspace_mallinfo(g_heap);
        *used = usage.uordblks; *available = usage.fordblks;
        mspace_inspect_all(g_heap, test_free_chunk, largest);
    }
}
""" % json.dumps(str(heap_source)))

test_obj, player_obj = output / "test_player_memory.o", output / "player_vlc.o"
heap_obj, memory_obj, binary = output / "heap.o", output / "vlc_memory.o", output / "test_player_memory"
run([link[0], *flags, "-O1", "-g", "-c", root / "tests/test_player_memory.cpp", "-o", test_obj])
# PLATFORM_PS5_NATIVE selects the real native player policy. Demux, HEVC decode,
# codec references, output slots and leases are the actual production code.
run([link[0], *flags, "-O2", "-g", "-DPLATFORM_PS5_NATIVE=1", "-c", player_source, "-o", player_obj])
for source, obj in ((heap_probe, heap_obj), (memory_source, memory_obj)):
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
print("PLAYER_MEMORY_LINK_READY " + str(binary), flush=True)

with tempfile.TemporaryDirectory(prefix="stremio-player-memory-") as directory:
    fixture = Path(directory) / "main10.mkv"
    generation = ["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-threads", "4",
                  "-filter_threads", "1", "-f", "lavfi", "-i",
                  "testsrc2=size=1280x720:rate=24:duration=5", "-vf",
                  "scale=3840:2160:flags=bilinear,format=yuv420p10le",
                  "-c:v", "libx265", "-preset", "ultrafast", "-profile:v", "main10", "-x265-params",
                  "pools=4:frame-threads=2:log-level=error:keyint=48:min-keyint=48:scenecut=0:"
                  "repeat-headers=1:bframes=4:b-adapt=0:ref=4:rc-lookahead=8",
                  "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
                  "-color_range", "tv", "-an", fixture]
    if args.fixture:
        shutil.copyfile(args.fixture.resolve(), fixture)
    else:
        run(generation, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
    shutil.copyfile(fixture, output / "main10.mkv")
    probe = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                 "stream=codec_name,width,height,pix_fmt,r_frame_rate,refs:frame=pict_type",
                 "-of", "json", fixture], stdout=subprocess.PIPE, text=True, timeout=60)
    fixture_info = json.loads(probe.stdout)
    picture_types = {}
    for frame in fixture_info.pop("frames", []):
        kind = frame.get("pict_type", "unknown")
        picture_types[kind] = picture_types.get(kind, 0) + 1
    fixture_info["picture_types"] = picture_types
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy", LIBGL_ALWAYS_SOFTWARE="1", EGL_PLATFORM="surfaceless",
                       NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost", LP_NUM_THREADS="4",
                       STREMIO_TEST_DIRECT_CAPACITY_MIB=str(args.direct_capacity_mib))
    command = [str(binary), directory, str(root / "app"), str(fixture), str(args.heap_pressure_mib),
               str(args.graphics_pressure_mib), str(args.sessions), str(args.frames), str(args.cycle_timeout),
               args.graphics_layout]
    commands.append(command)
    timed_out = False
    try:
        result = subprocess.run(command, env=environment,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                timeout=args.cycle_timeout * args.sessions + 30)
        log, exit_code = result.stdout, result.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        log = error.stdout or ""
        if isinstance(log, bytes):
            log = log.decode(errors="replace")
        exit_code = None
    (output / "test-output.txt").write_text(log)
    if (Path(directory) / "diagnostics").is_dir():
        shutil.copytree(Path(directory) / "diagnostics", output / "diagnostics", dirs_exist_ok=True)
    cases = [json.loads(line[len("PLAYER_MEMORY_RESULT "):]) for line in log.splitlines()
             if line.startswith("PLAYER_MEMORY_RESULT ")]
    for case in cases:
        case["heap_sampling"] = ("Main-thread snapshots after frame acquisition/close; first ENOMEM also probes "
                                 "actual free bytes and largest free chunk after the failed allocator releases its lock.")
        if not case["frames"]:
            case["minimum_heap_free"] = None
    errors = []
    if timed_out:
        errors.append("The playback or close operation exceeded the bounded test deadline.")
    if not picture_types.get("B") or not picture_types.get("P"):
        errors.append("The supplied fixture does not exercise both B and P pictures.")
    reproduced = bool(cases) and cases[0]["sessions"] > 0 and cases[0]["allocation_failures"] > 0
    if args.expect_exhaustion:
        if not reproduced:
            errors.append("Arena exhaustion was not reproduced in this measured scenario.")
    elif exit_code != 0 or not cases or cases[0]["result"] != "PASS":
        errors.append("Native-allocator HEVC playback checks did not complete successfully.")
    report = {
        "result": "FAIL" if errors else "BUG_REPRODUCED" if args.expect_exhaustion else "PASS",
        "scope": "Real Player, static VLC/FFmpeg Main10 with B/P references, native allocator and bounded direct mappings",
        "source_selection": {"baseline": baseline, "heap": str(heap_source), "picture_allocator": str(memory_source),
                             "player": str(player_source)},
        "boundaries": [
            f"PS5 direct-memory syscalls use separately owned host mmap mappings and a {args.direct_capacity_mib} MiB logical physical budget.",
            f"{args.heap_pressure_mib} MiB in 64 KiB resident allocations represents app/UI/network heap pressure; this is an explicit assumption, not a console attribution.",
            f"A separate {args.graphics_pressure_mib} MiB direct mapping represents driver/render surfaces. No real GPU or torrent-throughput simulation is claimed.",
            f"Driver placement is {args.graphics_layout}: split leaves two free extents around one center mapping, an assumed topology rather than a measured PS5 layout.",
            "The console's 903 MiB heap snapshot was taken at the first video frame and already includes playback; it is not used as app-only pressure.",
            "The selected player source uses its native decoder policy; host hardware decode is absent.",
            "The test-only heap translation unit enables dlmalloc inspection without changing its allocation behavior.",
            "Failure probes take consecutive snapshots under allocator/mapping locks; concurrent releases can still change state between snapshots.",
            "The harness verifies decoded plane changes, PTS progression, held frame leases across close/reopen, and allocation/release ownership.",
            "Five seconds of dynamic synthetic content exercises multiple GOPs but cannot establish long-duration console stability or exact driver fragmentation.",
        ],
        "cases": cases, "exit_code": exit_code, "errors": errors,
        "fixture": fixture_info,
        "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": source_hashes,
        "commands": commands,
    }
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(log[-8000:])
    print(output / "result.json")
    if errors:
        raise SystemExit(" ".join(errors))
