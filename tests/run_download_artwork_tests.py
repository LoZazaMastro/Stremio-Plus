#!/usr/bin/env python3
"""Check visible queued covers and deferred persistence around an active writer."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
args = parser.parse_args()
sources = [root / name for name in ("src/download_manager.cpp", "src/download_manager.h", "src/download_transfer.h", "src/growing_file.h",
                                   "tests/test_download_artwork.cpp", "tests/run_download_artwork_tests.py")]
before = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
with tempfile.TemporaryDirectory(prefix="stremio-download-artwork-") as folder:
    folder = Path(folder)
    binary = folder / "artwork-test"
    subprocess.run([
        os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wl,--wrap=write",
        "-I" + str(root / "src"), "-I" + str(root / "third_party"),
        str(root / "src/download_manager.cpp"), str(root / "tests/test_download_artwork.cpp"),
        "-o", str(binary)
    ], check=True)
    environment = dict(os.environ)
    # LeakSanitizer cannot enumerate threads in the hosted ptrace sandbox.
    # Address and undefined-behaviour instrumentation remain fully enabled.
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    result = subprocess.run([str(binary), str(folder / "data")], env=environment, check=True, timeout=40, text=True, capture_output=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    assert before == {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"result": "PASS", "source_sha256": before,
            "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "stdout": result.stdout,
            "scope": "Actual artwork worker: cached queued covers remain visible while bulk copies wait for an active transfer, late callbacks, idle persistence, restart and concurrent removal"}, indent=2) + "\n")
