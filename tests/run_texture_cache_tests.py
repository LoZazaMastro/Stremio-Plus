#!/usr/bin/env python3
"""Check real texture scheduling/limits without needing a GPU or native PS5."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="stremio-texture-cache-") as folder:
    binary = Path(folder) / "test-texture-cache"
    subprocess.run([
        os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-pthread", "-DGL_GLEXT_PROTOTYPES=1",
        "-I" + str(root / "src"), "-I" + str(root / "third_party"),
        "-I" + str(root / "vendor/ps5-homebrew-ui/src"),
        str(root / "src/texture_cache.cpp"), str(root / "src/util.cpp"),
        str(root / "tests/test_texture_cache.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=25)
