#!/usr/bin/env python3
"""Exercise subtitle parsing, ASS cleanup and caption placement without a player or UI."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cxx = os.environ.get("HOST_CXX", "c++")
# The app builds bidi.cpp with FriBidi (HAVE_FRIBIDI), as here.
fribidi = subprocess.run(["pkg-config", "--cflags", "--libs", "fribidi"], check=True,
                         capture_output=True, text=True).stdout.split()
common = ["-std=c++20", "-O1", "-g", "-fsanitize=undefined,address", "-fno-sanitize-recover=undefined",
          "-DHAVE_FRIBIDI=1", "-I" + str(root / "src"), "-I" + str(root / "third_party"), *fribidi]
with tempfile.TemporaryDirectory(prefix="stremio-subtitles-") as directory:
    binary = Path(directory) / "subtitles-test"
    bidi = Path(directory) / "bidi.o"
    # bidi.cpp is only a dependency here; the subtitle code and test build with -Werror.
    subprocess.run([cxx, *common, "-c", str(root / "src/bidi.cpp"), "-o", str(bidi)], check=True)
    subprocess.run([
        cxx, *common, "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        str(root / "src/subtitles.cpp"), str(root / "src/util.cpp"),
        str(root / "tests/test_subtitles.cpp"), str(bidi), "-o", str(binary), *fribidi,
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
