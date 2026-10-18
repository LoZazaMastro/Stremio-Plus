#!/usr/bin/env python3
"""Check automatic stalled-torrent recovery with ASan/UBSan and exact payload reads."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    sources = [root / p for p in (
        "tests/test_torrent_recovery.cpp", "tests/test_torrent_network.cpp",
        "tests/run_torrent_recovery_tests.py", "src/torrent/engine.cpp", "src/torrent/engine.h",
        "src/torrent/bencode.cpp", "src/torrent/bencode.h", "src/http.cpp", "src/http.h", "src/util.cpp", "src/util.h")]
    hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True))
    with tempfile.TemporaryDirectory(prefix="stremio-torrent-recovery-") as temporary:
        work = Path(temporary)
        binary = work / "torrent-recovery"
        command = [os.environ.get("HOST_CXX", "clang++-18"), "-std=c++17", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-function",
                   "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-DBT_ASYNC_DISK_WORKER=1", "-DBT_BATCHED_PICKER=1",
                   "-I" + str(root / "src"), "-I" + str(root / "third_party"),
                   str(root / "tests/test_torrent_recovery.cpp"), str(root / "src/torrent/bencode.cpp"),
                   str(root / "src/http.cpp"), str(root / "src/util.cpp"),
                   "-Wl,--wrap=connect", "-Wl,--wrap=getaddrinfo", "-Wl,--wrap=fwrite", "-Wl,--wrap=pwrite",
                   *flags, "-o", str(binary)]
        subprocess.run(command, check=True, timeout=180)
        run = subprocess.run([str(binary), str(work / "cache")], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=20, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        print(run.stdout, end="", flush=True)
        run.check_returncode()
        result = json.loads(run.stdout.splitlines()[-1])
    assert hashes == {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    report = {"result": "PASS", "scope": "Host automatic recovery, production parser, verified reader and restrictive directory permissions",
              "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
              "results": result, "sha256": hashes}
    target = root / "build/tests/torrent-recovery.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {result['checks']} torrent-recovery assertions; {target}", flush=True)


if __name__ == "__main__":
    main()
