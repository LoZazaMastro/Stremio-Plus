#!/usr/bin/env python3
"""Verify folder creation and native record bounds without SDL or online services."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "build/tests/download-directory"
out.mkdir(parents=True, exist_ok=True)
for native in (False, True):
    name = "test_download_directory" + ("_native" if native else "")
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic",
               "-I" + str(root / "src"), str(root / "src/download_directory.cpp"),
               str(root / "tests" / (name + ".cpp")), "-o", str(out / name)]
    if native:
        command.append("-DPLATFORM_PS5_NATIVE=1")
    else:
        command.append("-Wl,--wrap=fchmod")
    subprocess.run(command, check=True)
    subprocess.run([str(out / name)], check=True, timeout=15)
