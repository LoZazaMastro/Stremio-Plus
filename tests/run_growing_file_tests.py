#!/usr/bin/env python3
"""Real local growing-file AVIO, cancellation/lifecycle and full FFmpeg decode."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="stremio-growing-file-") as temporary:
    folder = Path(temporary)
    base = folder / "faststart.mp4"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "20",
                    "-c:v", "libx264", "-threads", "2", "-preset", "ultrafast", "-crf", "15",
                    "-c:a", "aac", "-movflags", "+faststart", str(base)], check=True, timeout=40)
    mkv, tail = folder / "growing.mkv", folder / "tail-moov.mp4"
    for path in (mkv, tail):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(base), "-map", "0", "-c", "copy", str(path)],
                       check=True, timeout=30)
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil"], text=True))
    executable = folder / "test_growing_file"
    subprocess.run([os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(root / "src"), "-I" + str(root / "third_party"),
                    str(root / "tests/test_growing_file.cpp"), str(root / "src/growing_file_stream.cpp"),
                    *flags, "-o", str(executable)], check=True, timeout=120)
    environment = dict(os.environ); environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(executable), str(folder / "data"), str(mkv), str(base), str(tail)],
                   check=True, timeout=45, env=environment)
