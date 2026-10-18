#!/usr/bin/env python3
"""Verify native large-picture ownership with real host mappings and kernel faults."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/vlc_memory.c"
HEADER = ROOT / "src/native_memory.h"
HARNESS = ROOT / "tests/test_native_video_memory.c"
REPORT = ROOT / "build/tests/native-video-memory/result.json"


def main():
    compiler = os.environ.get("HOST_CC") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("A host C compiler is required")
    with tempfile.TemporaryDirectory(prefix="stremio-native-video-memory-") as temporary:
        binary = Path(temporary) / "native-video-memory"
        command = [compiler, "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                   "-I", str(ROOT / "src"), str(SOURCE), str(HARNESS), "-o", str(binary)]
        command += [f"-Wl,--wrap={name}" for name in
                    ("malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc")]
        subprocess.run(command, check=True)
        environment = os.environ.copy()
        # Native physical extents and mappings have their own complete ledger.
        # Restricted host runners cannot perform LSan's process-wide scan.
        environment["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                timeout=45, env=environment)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError(f"Native video-memory regression failed: {result.returncode}")
    scenarios = []
    total_cases = total_checks = 0
    kernel = {}
    for line in result.stdout.splitlines():
        values = line.split("\t")
        if values[0] == "PASS":
            scenarios.append({"scenario": values[1], "checks": int(values[2]), "result": "PASS"})
        elif values[0] == "TOTAL":
            total_cases, total_checks = map(int, values[1:])
        elif values[0] == "KERNEL":
            kernel = dict(zip(("availabilityQueries", "physicalAllocationAttempts", "mappingAttempts",
                               "unmapAttempts", "physicalReleaseAttempts"), map(int, values[1:])))
    if not scenarios or len(scenarios) != total_cases or sum(s["checks"] for s in scenarios) != total_checks:
        raise RuntimeError("Incomplete native video-memory test output")
    report = {
        "result": "PASS", "cases": total_cases, "checks": total_checks,
        "sanitizers": ["address", "undefined"],
        "kernelCalls": kernel,
        "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (SOURCE, HEADER, HARNESS, Path(__file__).resolve())},
        "nativeAllocationOwnership": "Real host mmap/munmap pages plus an independent physical-extent ledger verify alignment, retained ownership, cleanup ordering, the 512 MiB/128-entry bound, retries and four-thread use. Every case ends with zero owned extents and mapped pages.",
        "heapIsolation": "Link wrappers forbid malloc, calloc, realloc, free, posix_memalign and aligned_alloc while any production mapping hook executes. Unknown heap pointers return to the caller unchanged.",
        "scenarios": scenarios,
        "limits": "Host tests simulate the console kernel API and physical-memory availability. They verify the production mapper, not physical PS5 playback, hardware decoder acceptance or sustained console performance. LSan is disabled; the independent ledger verifies native ownership.",
    }
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Native video memory: {total_cases} cases / {total_checks} checks PASS (ASan, UBSan, real mappings)")
    print(REPORT)


if __name__ == "__main__":
    main()
