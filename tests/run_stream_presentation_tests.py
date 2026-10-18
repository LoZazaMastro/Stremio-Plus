#!/usr/bin/env python3
"""Compile the actual, pure stream badge parser against offline addon shapes."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="stremio-stream-presentation-") as folder:
    binary = Path(folder) / "test-stream-presentation"
    subprocess.run([
        os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-I" + str(root / "src"), "-I" + str(root / "third_party"),
        str(root / "src/stream_presentation.cpp"), str(root / "tests/test_stream_presentation.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
