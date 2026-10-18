#!/usr/bin/env python3
"""Offline production-engine regression and controlled before/after benchmark.

An optional --baseline is a frozen engine.cpp from the previous release, never
an old binary. Virtual transfer rates are a deterministic link model, not a PS5
speed test. Host CPU timing is reported separately and is not an acceptance gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cache-mode", choices=("disk", "legacy-ram", "bounded-ram"), default="disk")
    parser.add_argument("--candidate", type=Path, help="Explicit frozen candidate source for controlled experiments")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    common_sources = [root / name for name in (
        "src/torrent/engine.h", "src/torrent/bencode.cpp", "src/http.cpp", "src/util.cpp",
        "tests/test_torrent_performance.cpp", "tests/run_torrent_performance_tests.py")]
    def common_hashes():
        return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in common_sources}
    initial_common = common_hashes()
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True))
    report = {"result": "PASS", "description": "Controlled real scheduler/SHA-1/cache benchmark; no console or Internet speed claims",
              "virtual_model": {"duration_s": 20, "round_trip_s": 0.3, "initial_peer_gap_s": 1.25,
                                "fast_peers": 8, "fast_peer_MiBps": 12, "mixed_peers": "six 12 MiB/s plus two 0.2 MiB/s"},
              "cache_model": {"mode": args.cache_mode,
                              "rolling_cache_configured": args.cache_mode == "disk",
                              "standalone_download_handoff": "Bounded verified memory, independent of the configured rolling-cache descriptor",
                              "resident_cache_comparable": args.cache_mode != "legacy-ram",
                              "legacy_ram_warning": "Historical fixture grants old engine512 MiB; new engine bounds RAM64 MiB. Compare disk or bounded-ram for equal cache capacity." if args.cache_mode == "legacy-ram" else None,
                              "virtual_cache_slots": 64 if args.cache_mode != "bounded-ram" else 8,
                              "piece_bytes": 8 << 20,
                              "cpu_timing_scope": "Host SHA-1, picker and selected cache backend; not isolated CPU or console speed"}}
    with tempfile.TemporaryDirectory(prefix="stremio-torrent-performance-") as temporary:
        for name, implementation in [("baseline", args.baseline), ("current", args.candidate or root / "src/torrent/engine.cpp")]:
            if implementation is None:
                continue
            implementation = implementation.resolve()
            initial_engine_hash = hashlib.sha256(implementation.read_bytes()).hexdigest()
            executable = Path(temporary) / name
            command = [os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O2", "-Wall", "-Wextra",
                       "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-function", "-pthread",
                       "-I" + str(root / "src"), "-I" + str(root / "src/torrent"),
                       "-I" + str(root / "third_party"), '-DENGINE_IMPLEMENTATION="' + str(implementation) + '"',
                       str(root / "tests/test_torrent_performance.cpp"), str(root / "src/torrent/bencode.cpp"),
                       str(root / "src/http.cpp"), str(root / "src/util.cpp"),
                       "-Wl,--wrap=socket", "-Wl,--wrap=connect", "-Wl,--wrap=getaddrinfo",
                       *flags, "-o", str(executable)]
            if name == "current":
                command.append("-DPERFORMANCE_NEW=1")
            if "start_disk_worker_locked" in implementation.read_text():
                command.append("-DBT_ASYNC_DISK_WORKER=1")
            subprocess.run(command, check=True, timeout=180)
            completed = subprocess.run([str(executable), args.cache_mode], text=True, capture_output=True, timeout=120)
            if completed.returncode:
                raise RuntimeError(name + " benchmark failed: " + completed.stdout + completed.stderr)
            lines = [line for line in completed.stdout.splitlines() if line.startswith("RESULT ")]
            if len(lines) != 1:
                raise RuntimeError("missing benchmark result")
            report[name] = json.loads(lines[0][7:])
            if hashlib.sha256(implementation.read_bytes()).hexdigest() != initial_engine_hash or common_hashes() != initial_common:
                raise RuntimeError("sources changed during the benchmark; no report was published")
            report[name]["engine_sha256"] = initial_engine_hash
            print(name + ": " + lines[0][7:], flush=True)
        failures = []
        if "baseline" in report:
            before, after = report["baseline"], report["current"]
            report["comparisons"] = {
                "cpu_speedup": before["cpu_256MiB_median_s"] / after["cpu_256MiB_median_s"],
                "fast_contiguous_gain": after["fast_contiguous_bytes"] / before["fast_contiguous_bytes"],
                "mixed_contiguous_gain": after["mixed_contiguous_bytes"] / before["mixed_contiguous_bytes"],
            }
            if after["fast_contiguous_bytes"] < before["fast_contiguous_bytes"] or after["mixed_contiguous_bytes"] < before["mixed_contiguous_bytes"]:
                report["result"] = "FAIL"
                failures.append("contiguous progress regressed in a controlled link scenario")
        report["source_sha256"] = initial_common
        if failures:
            report["failures"] = failures
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        if failures:
            raise RuntimeError("; ".join(failures))


if __name__ == "__main__":
    main()
