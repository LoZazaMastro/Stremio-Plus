#!/usr/bin/env python3
"""Verify actual weighted decode admission without allocating huge images."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="stremio-art-decode-budget-") as folder:
    binary = Path(folder) / "test-art-decode-budget"
    subprocess.run([
        os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(root / "src"),
        str(root / "tests/test_art_decode_budget.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
