#!/usr/bin/env python3
"""Check episode labels and prefix-based progressive resume without networking."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
args = parser.parse_args()
sources = [root / path for path in ("src/download_metadata.h", "src/playback_progress.h",
    "src/download_manager.h", "tests/test_download_bookmarks.cpp", "tests/run_download_bookmark_tests.py")]
hashes = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
with tempfile.TemporaryDirectory(prefix="stremio-bookmarks-") as temporary:
    binary = Path(temporary) / "download-bookmarks"
    subprocess.run([os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(root / "src"), "-I" + str(root / "third_party"),
        str(root / "tests/test_download_bookmarks.cpp"), "-o", str(binary)], check=True)
    environment = dict(os.environ)
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    result = subprocess.run([str(binary)], check=False, capture_output=True, text=True, timeout=30, env=environment)
    print(result.stdout, end="")
    print(result.stderr, end="")
    result.check_returncode()
assert hashes == {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"result": "PASS", "source_sha256": hashes,
        "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "stdout": result.stdout,
        "scope": "Production episode metadata and progressive bookmark admission; no decoder or console timing claim"}, indent=2) + "\n")
