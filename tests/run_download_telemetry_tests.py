#!/usr/bin/env python3
"""Deterministic time and saved-byte inputs to the production ETA estimator."""
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
sources = [root / name for name in ("src/download_telemetry.h", "tests/test_download_telemetry.cpp", "tests/run_download_telemetry_tests.py")]
before = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
with tempfile.TemporaryDirectory(prefix="stremio-download-telemetry-") as folder:
    binary = Path(folder) / "test-download-telemetry"
    subprocess.run([os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I" + str(root / "src"),
                    str(root / "tests/test_download_telemetry.cpp"), "-o", str(binary)], check=True)
    environment = dict(os.environ)
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    result = subprocess.run([str(binary)], check=True, timeout=10, env=environment, text=True, capture_output=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    assert before == {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"result": "PASS", "source_sha256": before,
            "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "stdout": result.stdout,
            "scope": "Production smoothing with deterministic wall time and saved bytes, including resume, stall and unknown total"}, indent=2) + "\n")
