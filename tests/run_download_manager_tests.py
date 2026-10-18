#!/usr/bin/env python3
"""Exercise the real persistent queue with a controlled transfer boundary."""
import os
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
args = parser.parse_args()
sources = [root / name for name in ("src/download_manager.cpp", "src/download_manager.h", "src/download_metadata.h", "src/download_transfer.h", "src/growing_file.h",
                                   "tests/test_download_manager.cpp", "tests/run_download_manager_tests.py")]
before = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
environment = dict(os.environ)
environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
with tempfile.TemporaryDirectory(prefix="stremio-download-manager-") as folder:
    directory = Path(folder)
    binary = directory / "test-download-manager"
    command = [os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
               "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-Wl,--wrap=lstat,--wrap=opendir,--wrap=mkdir,--wrap=chmod,--wrap=fchmod,--wrap=write,--wrap=fsync",
               "-I" + str(root / "src"), "-I" + str(root / "third_party"),
               str(root / "src/download_manager.cpp"),
               str(root / "tests/test_download_manager.cpp"), "-o", str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary), str(directory / "data")], check=False,
                            timeout=45, env=environment, text=True, capture_output=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    result.check_returncode()
    assert before == {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"result": "PASS", "source_sha256": before,
            "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "stdout": result.stdout,
            "scope": "Production queue and filesystem with controlled transfer; active progress during blocked enqueue fsync, durable asynchronous acceptance, bounded pending memory, logout/shutdown races, deferred artwork, progress-only flush accounting and interrupted recovery; no console timing claim"}, indent=2) + "\n")
