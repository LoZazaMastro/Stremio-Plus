#!/usr/bin/env python3
"""Exercise production native VP9 association with delayed mocked hardware output.

Compiles the complete PLATFORM_PS5_NATIVE hwdec_ps5.cpp on the host. Hardware
calls are deterministic fixtures; the VP9 bitstream filter is real FFmpeg.
Use --ffmpeg-prefix .deps/vlc-host/prefix-host to link the pinned host archives
instead of the distribution libraries. The two reports are kept separately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ffmpeg-prefix", type=Path,
    help="host FFmpeg static prefix, with lib/pkgconfig and include directories")
args = parser.parse_args()
compiler = os.environ.get("HOST_CXX") or shutil.which("clang++-18") or shutil.which("c++")
if not compiler:
    raise SystemExit("A host C++20 compiler is required")
pkg_env = dict(os.environ)
pkg_command = ["pkg-config"]
prefix = args.ffmpeg_prefix.resolve(strict=True) if args.ffmpeg_prefix else None
if prefix:
    # Restrict resolution to this verified host prefix so an absent dependency
    # cannot silently select incompatible system headers or shared libraries.
    pkg_env["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig")
    pkg_env["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    pkg_env.pop("PKG_CONFIG_SYSROOT_DIR", None)
    pkg_command.append("--static")
    for name in ("avcodec", "avutil", "swresample"):
        if not (prefix / "lib" / ("lib" + name + ".a")).is_file():
            raise SystemExit("Missing static host archive: lib" + name + ".a")
flags = shlex.split(subprocess.check_output(
    [*pkg_command, "--cflags", "--libs", "libavcodec", "libavutil"], env=pkg_env, text=True))
versions = {name: subprocess.check_output(
    [*pkg_command, "--modversion", name], env=pkg_env, text=True).strip()
    for name in ("libavcodec", "libavutil")}
output = root / ("build/tests/vp9-output-pinned" if prefix else "build/tests/vp9-output")
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="stremio-vp9-output-") as directory:
    binary = Path(directory) / "vp9-output"
    subprocess.run([compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
        "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(root / "third_party"), str(root / "tests/test_vp9_output.cpp"),
        "-Wl,--gc-sections", *flags, "-o", str(binary)], check=True, timeout=120)
    result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"),
        timeout=45)
    (output / "output.txt").write_text(result.stdout)
    print(result.stdout, end="")
    result.check_returncode()
    results = json.loads(result.stdout.splitlines()[-1])
for name, expected in versions.items():
    runtime = results[name + "_version"]
    actual = f"{runtime >> 16}.{(runtime >> 8) & 255}.{runtime & 255}"
    if actual != expected:
        raise SystemExit(f"{name} runtime {actual} differs from pkg-config {expected}")
files = ["src/hwdec_ps5.cpp", "src/hwdec_ps5.h", "tests/test_vp9_output.cpp", "tests/run_vp9_output_tests.py"]
report = {"result": "PASS", "results": results, "sanitizers": ["address", "undefined"],
    "ffmpeg_prefix": str(prefix) if prefix else None, "pkg_config_static": bool(prefix),
    "library_versions": versions,
    "scope": "Complete production native HwDecoder code and real FFmpeg VP9 bitstream filter; deterministic sceVideodec2 boundary",
    "limits": "Does not exercise PS5 decode, native memory allocation, GPU buffer ownership or firmware timing.",
    "source_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in files}}
(output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(output / "result.json")
