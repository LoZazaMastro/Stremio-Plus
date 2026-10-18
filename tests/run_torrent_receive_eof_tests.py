#!/usr/bin/env python3
"""Actual TCP EOF regression with AddressSanitizer/UndefinedBehaviorSanitizer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-engine", type=Path)
    parser.add_argument("--candidate-engine", type=Path, default=root / "src/torrent/engine.cpp")
    parser.add_argument("--output", type=Path, default=root / "build/tests/torrent-receive-eof.json")
    args = parser.parse_args()
    variants = {name: value.resolve() for name, value in (
        ("baseline", args.baseline_engine), ("candidate", args.candidate_engine)) if value}
    checked = list(variants.values()) + [root / name for name in (
        "tests/test_torrent_receive_eof.cpp", "tests/test_torrent_network.cpp",
        "tests/run_torrent_receive_eof_tests.py", "src/torrent/bencode.cpp", "src/torrent/bencode.h",
        "src/http.cpp", "src/http.h", "src/util.cpp", "src/util.h")]
    checked += [path.parent / "engine.h" for path in variants.values() if (path.parent / "engine.h").is_file()]
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in checked}
    results = []
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True))
    with tempfile.TemporaryDirectory(prefix="stremio-tcp-fin-") as temporary:
        work = Path(temporary)
        for name, engine in variants.items():
            executable = work / name
            command = [os.environ.get("HOST_CXX", "clang++-18"), "-std=c++17", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-function",
                       "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       '-DBT_ENGINE_SOURCE="' + str(engine) + '"', "-I" + str(engine.parent.parent),
                       "-I" + str(root / "src"), "-I" + str(root / "third_party"),
                       str(root / "tests/test_torrent_receive_eof.cpp"), str(root / "src/torrent/bencode.cpp"),
                       str(root / "src/http.cpp"), str(root / "src/util.cpp"),
                       "-Wl,--wrap=connect", "-Wl,--wrap=getaddrinfo", "-Wl,--wrap=fwrite", "-Wl,--wrap=pwrite",
                       *flags, "-o", str(executable)]
            if "start_disk_worker_locked" in engine.read_text():
                command.insert(1, "-DBT_ASYNC_DISK_WORKER=1")
            subprocess.run(command, check=True, timeout=180)
            environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
            invocation = [str(executable), str(work / (name + "-cache"))]
            if name == "baseline":
                invocation.append("expect-old-loss")
            run = subprocess.run(invocation, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=15, env=environment)
            print(run.stdout, end="", flush=True)
            run.check_returncode()
            value = json.loads(run.stdout.splitlines()[-1])
            assert value["result"] == "PASS" and value["external_attempts"] == 0
            value["variant"] = name
            results.append(value)
    assert hashes == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in checked}, "sources changed during test"
    report = {"result": "PASS", "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
              "leak_sanitizer": "Disabled because LSan is unsupported under the current ptrace runtime",
              "scope": "Host real TCP with 128 KiB buffered in kernel before production recv loop and FIN",
              "fixture_boundary": "Synthetic metadata and already-handshaken peer; real sockets/recv/EOF/SHA/cache/read",
              "results": results, "sha256": hashes}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: peer payload followed by TCP FIN", flush=True)


if __name__ == "__main__":
    main()
