#!/usr/bin/env python3
"""Test the real PS5 output helper with fake VideoOut imports, without a console."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    project = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="stremio-display-output-") as temp:
        executable = Path(temp) / "test_display_output"
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-DPLATFORM_PS5=1", "-I" + str(project / "src"), "-I" + str(project / "third_party"),
                        str(project / "tests/test_display_output.cpp"), str(project / "src/display_output.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
