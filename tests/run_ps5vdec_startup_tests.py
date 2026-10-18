#!/usr/bin/env python3
"""Compile and fault-inject the production PS5 decoder startup on the host."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/vlc-ps5/modules/ps5vdec.c"
HARNESS = ROOT / "tests/test_ps5vdec_startup.c"
REPORT = ROOT / "build/tests/ps5vdec-startup.json"


def section(source, first, last):
    start = source.index(first)
    return source[start:source.index(last, start)]


def main():
    source = SOURCE.read_text()
    # Keep startup functions byte-for-byte; the host replaces only VLC types,
    # console imports and decode/flush callbacks that this suite does not run.
    startup = "\n".join((
        section(source, "enum {", "static void pending_add"),
        section(source, "static void read_headers", "static void give_up"),
        section(source, "static void free_buffers", "vlc_module_begin()"),
    ))
    compiler = os.environ.get("HOST_CC") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("A host C compiler is required")
    with tempfile.TemporaryDirectory(prefix="stremio-ps5vdec-startup-") as temporary:
        folder = Path(temporary)
        (folder / "ps5vdec_startup_under_test.h").write_text(startup)
        binary = folder / "ps5vdec-startup"
        command = [compiler, "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                   "-I", str(folder), str(HARNESS), "-o", str(binary)]
        subprocess.run(command, check=True)
        environment = os.environ.copy()
        # Console allocations are tracked by the harness. LSan's process scan
        # is unavailable in restricted host runners; ASan still checks C heap use.
        environment["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=environment, timeout=30)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError(f"Decoder startup regression failed: {result.returncode}")
    scenarios = []
    for line in result.stdout.splitlines():
        values = line.split("\t")
        if values[0] == "PASS":
            scenarios.append({"scenario": values[1], "checks": int(values[2]), "result": "PASS"})
        elif values[0] == "TOTAL":
            total_cases, total_checks = map(int, values[1:])
    if len(scenarios) != total_cases or sum(s["checks"] for s in scenarios) != total_checks:
        raise RuntimeError("Incomplete test output")
    report = {
        "result": "PASS", "cases": total_cases, "checks": total_checks,
        "sanitizers": ["address", "undefined"],
        "nativeAllocationOwnership": "Every physical allocation and mapping is tracked; each case ends with no owned allocations or mapped buffers.",
        "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in (SOURCE, HARNESS, Path(__file__).resolve())},
        "compiledStartupSha256": hashlib.sha256(startup.encode()).hexdigest(),
        "scenarios": scenarios,
        "limits": "Host fault injection covers startup and native allocation ownership. It does not validate PS5 hardware decode or diagnose a console SIGABRT.",
    }
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PS5 decoder startup: {total_cases} cases / {total_checks} checks PASS (ASan, UBSan, native ownership ledger)")
    print(REPORT)


if __name__ == "__main__":
    main()
