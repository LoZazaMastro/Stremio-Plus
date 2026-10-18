#!/usr/bin/env python3
"""Exercise runtime download labels without a host application or PS5 toolchain."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="stremio-download-text-") as directory:
    binary = Path(directory) / "download-text-test"
    subprocess.run([
        os.environ.get("HOST_CXX", "c++"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsanitize=undefined",
        "-fno-sanitize-recover=undefined", "-I" + str(root / "src"),
        str(root / "src/download_text.cpp"), str(root / "tests/test_download_text.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
