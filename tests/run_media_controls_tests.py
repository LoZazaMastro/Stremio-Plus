#!/usr/bin/env python3
"""Exercise remote input batches and native refresh rollback with mocked OS calls."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CXX", "g++")
common = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
          "-I", str(root / "src"), "-I", str(root / "third_party"),
          "-I", str(root / "vendor/ps5-homebrew-ui/src")]
cases = [
    ("media_remote", ["-DPLATFORM_PS5"], ["src/media_remote.cpp", "vendor/ps5-homebrew-ui/src/core/input.cpp"]),
    ("display_refresh", ["-DPLATFORM_PS5_NATIVE"], ["src/display_refresh.cpp"]),
]
with tempfile.TemporaryDirectory(prefix="stremio-media-controls-") as directory:
    for name, defines, sources in cases:
        binary = str(Path(directory) / name)
        subprocess.run(common + defines + [str(root / p) for p in sources] +
                       [str(root / "tests" / ("test_" + name + ".cpp")), "-o", binary], check=True)
        subprocess.run([binary], check=True, timeout=15)
        print(name + ": passed")
