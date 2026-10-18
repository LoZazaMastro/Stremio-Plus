#!/usr/bin/env python3
"""Repeatable actual TCP comparison. Never contacts an Internet peer or tracker.

Example:
  python3 tests/run_torrent_network_tests.py \
    --candidate-engine src/torrent/engine.cpp --repeat 3 \
    --output build/tests/torrent-network.json

The seeder delays each application's piece response, which is a controlled
peer turnaround delay, not a simulation of TCP congestion or Internet RTT.
Any cache latency is injected before a rolling-cache write. Standalone download
readers intentionally bypass those writes through the bounded verified memory
handoff. Every delivered byte is compared to the fixture payload.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile

CASES = {
    "local_fast": [4, 20, 32, 256, 8, 0],
    "peer_delay_300ms": [4, 300, 32, 128, 8, 0],
    "cache_write_20ms": [4, 20, 64, 256, 8, 20],
    "mixed_senders": [6, 20, 24, 128, 8, 0, 2, 0.2],
    "healthy_request_queues": [4, 20, 1, 32, 4, 0],
    "many_seeders_large_pieces": [16, 80, 4, 256, 16, 0],
    "many_mixed_seeders": [40, 80, 8, 256, 4, 0, 30, 0.15],
    "sustained_local_fast": [16, 20, 64, 768, 4, 0],
    "sustained_mixed_seeders": [40, 80, 16, 512, 4, 0, 30, 0.15],
    "peer_delay_1500ms": [8, 1500, 8, 96, 4, 0],
    "saturated_peer_pool": [84, 20, 32, 32, 8, 0, 80, 0.005],
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-engine", type=Path)
    parser.add_argument("--candidate-engine", type=Path)
    parser.add_argument("--variant", action="append", default=[], help="Additional isolated variant NAME=engine.cpp")
    parser.add_argument("--output", type=Path, default=root / "build/tests/torrent-network.json")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--watchdog-seconds", type=int, default=55)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=[key for key in CASES if key != "saturated_peer_pool"])
    args = parser.parse_args()
    variants = {name: path.resolve() for name, path in
                (("baseline", args.baseline_engine), ("candidate", args.candidate_engine)) if path}
    for item in args.variant:
        name, separator, source = item.partition("=")
        assert separator and name and all(ch.isalnum() or ch == "_" for ch in name) and name not in variants
        variants[name] = Path(source).resolve()
    if not variants:
        variants["candidate"] = root / "src/torrent/engine.cpp"
    assert 1 <= args.repeat <= 10
    assert 5 <= args.watchdog_seconds <= 240
    sources = [root / name for name in (
        "tests/test_torrent_network.cpp", "tests/run_torrent_network_tests.py",
        "src/torrent/bencode.cpp", "src/torrent/bencode.h", "src/http.cpp", "src/http.h", "src/util.cpp", "src/util.h")]
    sources += list(variants.values())
    sources += [path.parent / "engine.h" for path in variants.values() if (path.parent / "engine.h").is_file()]
    original = {str(path): sha(path) for path in sources}
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True))
    results = []
    with tempfile.TemporaryDirectory(prefix="stremio-real-tcp-") as temporary:
        work = Path(temporary)
        executables = {}
        for name, engine in variants.items():
            binary = work / name
            cmd = [os.environ.get("HOST_CXX", "clang++-18"), "-std=c++17", "-O2", "-g",
                   "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-Wno-unused-function",
                   "-pthread", '-DBT_ENGINE_SOURCE="' + str(engine) + '"',
                   "-I" + str(engine.parent.parent), "-I" + str(root / "src"), "-I" + str(root / "third_party"),
                   str(root / "tests/test_torrent_network.cpp"), str(root / "src/torrent/bencode.cpp"),
                   str(root / "src/http.cpp"), str(root / "src/util.cpp"),
                   "-Wl,--wrap=connect", "-Wl,--wrap=getaddrinfo", "-Wl,--wrap=fwrite", "-Wl,--wrap=pwrite",
                   *flags, "-o", str(binary)]
            if "start_disk_worker_locked" in engine.read_text():
                cmd.insert(1, "-DBT_ASYNC_DISK_WORKER=1")
            if "void turnover_peer(" in engine.read_text():
                cmd.insert(1, "-DBT_PEER_TURNOVER=1")
            if "uint64_t request_handoffs =" in engine.read_text():
                cmd.insert(1, "-DBT_REQUEST_HANDOFF=1")
            if "picker_passes =" in engine.read_text():
                cmd.insert(1, "-DBT_BATCHED_PICKER=1")
            subprocess.run(cmd, check=True, timeout=180)
            executables[name] = binary
        for case in args.cases:
            for repetition in range(args.repeat):
                # Reverse alternating runs to reduce consistent order bias.
                order = list(variants) if repetition % 2 == 0 else list(reversed(variants))
                for name in order:
                    print(f"Running {name} {case} #{repetition+1}", flush=True)
                    with tempfile.TemporaryDirectory(prefix=case + "-", dir=work) as cache:
                        run = subprocess.run([str(executables[name]), cache, *map(str, CASES[case])],
                                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                             text=True, timeout=args.watchdog_seconds + 10,
                                             env=dict(os.environ, STREMIO_TCP_WATCHDOG_SECONDS=str(args.watchdog_seconds)))
                    if run.returncode:
                        print(run.stdout, flush=True)
                    run.check_returncode()
                    value = json.loads(run.stdout.splitlines()[-1])
                    assert value["result"] == "PASS" and value["exact_bytes"] and value["external_network_attempts"] == 0
                    value.update(variant=name, case=case, repetition=repetition + 1)
                    results.append(value)
                    print(f'{name} {case} #{repetition+1}: {value["MiB_s"]:.2f} MiB/s, '
                          f'first piece {value["first_piece_s"]:.3f}s, exact {value["bytes"]} bytes', flush=True)
    assert {str(path): sha(path) for path in sources} == original, "source changed during comparison"
    medians = {}
    for case in args.cases:
        medians[case] = {}
        for name in variants:
            chosen = [v for v in results if v["case"] == case and v["variant"] == name]
            medians[case][name] = {key: statistics.median(v[key] for v in chosen)
                                    for key in ("MiB_s", "elapsed_s", "first_piece_s", "duplicate_bytes",
                                                "network_thread_cpu_s", "stats_wait_max_ms", "stats_wait_p99_ms")}
        if "baseline" in variants and "candidate" in variants:
            medians[case]["throughput_change_percent"] = 100 * (
                medians[case]["candidate"]["MiB_s"] / medians[case]["baseline"]["MiB_s"] - 1)
        if "baseline" in variants:
            medians[case]["variant_change_percent"] = {
                name: 100 * (medians[case][name]["MiB_s"] / medians[case]["baseline"]["MiB_s"] - 1)
                for name in variants if name != "baseline"}
    report = {"result": "PASS", "scope": "Host real TCP peer-wire; no physical PS5 or Internet swarm",
              "network_loop_stubbed": False, "engine_methods_stubbed": False,
              "fixture_boundary": "Synthetic valid torrent metadata and local candidate addresses",
              "seeder_delay_model": "Application response turnaround, no emulated TCP congestion/Internet RTT",
              "cache_delay_model": "Extra latency before rolling-cache fwrite/pwrite only; standalone download readers intentionally bypass these writes",
              "fast_peer_counters": "TCP accepted, completed wire handshake and received at least one REQUEST are separate; fast_admitted is a legacy alias for fast_requested",
              "acceptance": "Exact delivered payload and completion; throughput is evidence, not a fragile timing assertion",
              "runs": results, "medians": medians, "sha256": original}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(results)} actual TCP runs, exact payloads; {args.output}", flush=True)


if __name__ == "__main__":
    main()
