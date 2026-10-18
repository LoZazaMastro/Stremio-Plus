#!/usr/bin/env python3
"""Compile and exercise the real native torrent scheduler without starting networking."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    project = Path(__file__).resolve().parents[1]
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True))
    with tempfile.TemporaryDirectory(prefix="stremio-torrent-reader-tests-") as temp:
        executable = Path(temp) / "test_torrent_readers"
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-Wall", "-Wextra",
                        "-Wno-deprecated-declarations", "-Wno-unused-function", "-pthread",
                        "-I" + str(project / "src"), "-I" + str(project / "third_party"),
                        str(project / "tests/test_torrent_readers.cpp"), str(project / "src/torrent/bencode.cpp"),
                        str(project / "src/http.cpp"), str(project / "src/util.cpp"), *flags,
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()
