#!/usr/bin/env python3
"""Exercise real artwork workers, cache, image decoding and UI completion queue."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libwebp"], text=True))
with tempfile.TemporaryDirectory(prefix="stremio-artcache-") as folder:
    binary = Path(folder) / "test-artcache"
    subprocess.run([
        os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-pthread", "-Wl,--wrap=stat", "-I" + str(root / "src"), "-I" + str(root / "third_party"),
        str(root / "src/artcache.cpp"), str(root / "src/tasks.cpp"), str(root / "src/util.cpp"),
        str(root / "tests/test_artcache.cpp"), "-o", str(binary), *flags,
    ], check=True)
    subprocess.run([str(binary), str(Path(folder) / "data")], check=True, timeout=35)
