#!/usr/bin/env python3
"""Verify torrent allocation rollback and unchanged payload budgets with production code."""
from pathlib import Path
import hashlib
import json
import os
import shlex
import subprocess

root = Path(__file__).resolve().parents[1]
output = root / "build/tests/torrent-memory"
output.mkdir(parents=True, exist_ok=True)
binary = output / "test_torrent_memory"
flags = shlex.split(subprocess.check_output([os.environ.get("PKG_CONFIG", "pkg-config"), "--cflags", "--libs", "libcurl", "openssl"], text=True))
sources = [root / "tests/test_torrent_memory.cpp", root / "src/torrent/bencode.cpp", root / "src/http.cpp", root / "src/util.cpp"]
command = [os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
           "-Wno-deprecated-declarations", "-Wno-unused-function", "-pthread", "-I" + str(root / "src"),
           "-I" + str(root / "third_party"), *map(str, sources), *flags, "-o", str(binary)]
subprocess.run(command, check=True)
result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
(output / "test-output.txt").write_text(result.stdout)
files = [*sources, root / "src/torrent/engine.cpp", root / "src/torrent/engine.h", root / "src/native_memory.h", Path(__file__).resolve()]
passed = result.returncode == 0 and "TORRENT_MEMORY_TESTS_OK" in result.stdout
report = {"result": "PASS" if passed else "FAIL",
          "scope": "Production native torrent scheduler; existing payload-budget pressure and a real operator-new failure while allocating a 512 KiB piece; reservation rollback, retry and retained peer state",
          "output": result.stdout, "exitCode": result.returncode,
          "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files},
          "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
(output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout)
if not passed:
    raise SystemExit(1)
print(output / "result.json")
