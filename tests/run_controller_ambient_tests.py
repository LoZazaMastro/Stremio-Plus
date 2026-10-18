#!/usr/bin/env python3
"""Compile real palette worker and PS5 Pad adapter against controlled boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
check_environment = dict(os.environ)
# Address/undefined checks work in the execution container; LeakSanitizer's
# separate /proc task inspection does not. Callers can override this setting.
check_environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
with tempfile.TemporaryDirectory(prefix="stremio-controller-light-") as folder:
    folder = Path(folder)
    common = [os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
              "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    binary = folder / "test-controller-ambient"
    subprocess.run(common + ["-I" + str(root / "src"), str(root / "src/controller_ambient.cpp"),
                            str(root / "tests/test_controller_ambient.cpp"),
                            "-Wl,--wrap=fopen", "-Wl,--wrap=fread", "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(folder / "art")], check=True, timeout=20, env=check_environment)
    pad = folder / "test-ps5-pad-light"
    hui = root / "vendor/ps5-homebrew-ui/src"
    subprocess.run(common + ["-I" + str(hui), str(hui / "platform/ps5/pad.cpp"),
                            str(root / "tests/test_ps5_pad_light.cpp"), "-o", str(pad)], check=True)
    subprocess.run([str(pad)], check=True, timeout=10, env=check_environment)
