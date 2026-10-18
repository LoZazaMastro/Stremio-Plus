#!/usr/bin/env python3
"""Exercise the production asynchronous storage pipeline without a swarm."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sources = [root / name for name in (
    "src/torrent/engine.cpp", "src/torrent/engine.h", "src/torrent/bencode.cpp",
    "tests/test_torrent_disk_worker.cpp", "tests/run_torrent_disk_worker_tests.py")]
before = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True))
with tempfile.TemporaryDirectory(prefix="stremio-disk-worker-") as temporary:
    folder = Path(temporary)
    binary = folder / "test_disk_worker"
    subprocess.run([
        os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-deprecated-declarations", "-Wno-unused-function", "-pthread", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(root / "src"), "-I" + str(root / "third_party"),
        str(root / "tests/test_torrent_disk_worker.cpp"), str(root / "src/torrent/bencode.cpp"),
        str(root / "src/http.cpp"), str(root / "src/util.cpp"), "-Wl,--wrap=socket", "-Wl,--wrap=connect",
        "-Wl,--wrap=getaddrinfo", "-Wl,--wrap=pread", "-Wl,--wrap=pwrite", *flags, "-o", str(binary)
    ], check=True, timeout=180)
    environment = dict(os.environ)
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    run = subprocess.run([str(binary), str(folder / "fixtures")], capture_output=True, text=True,
                         timeout=60, env=environment)
    print(run.stdout, end="", flush=True)
    print(run.stderr, end="", flush=True)
    run.check_returncode()
    assert before == {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    match = re.search(r"PASS: (\d+) asynchronous disk worker assertions; pwrite=(\d+) pread=(\d+) network=(\d+)", run.stdout)
    assert match
    report = {"result": "PASS", "assertions": int(match[1]), "pwrite_calls": int(match[2]),
              "pread_calls": int(match[3]), "network_attempts": int(match[4]),
              "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
              "source_sha256": before, "stdout": run.stdout,
              "scope": "Actual worker, SHA-1, disk I/O and engine state with controlled gates, no console measurement"}
    output = root / "build/tests/torrent-disk-worker.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
