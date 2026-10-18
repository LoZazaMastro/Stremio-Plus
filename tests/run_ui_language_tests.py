#!/usr/bin/env python3
"""Compile the actual PS5 locale branch against a system-service test boundary."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
output = root / "build/tests/ui-language"
output.mkdir(parents=True, exist_ok=True)
binary = output / "test-ui-language"
subprocess.run([os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
    "-Wall", "-Wextra", "-Werror", "-DPLATFORM_PS5_NATIVE=1", "-I" + str(root / "src"),
    "-I" + str(root / "third_party"), str(root / "src/ui_language.cpp"),
    str(root / "tests/test_ui_language.cpp"), "-o", str(binary)], check=True)
result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
(output / "ui-language-test-output.txt").write_text(result.stdout)
print(result.stdout, end="")
raise SystemExit(result.returncode)
