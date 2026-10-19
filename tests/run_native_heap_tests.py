#!/usr/bin/env python3
"""Exercise native allocator wrappers with a mapped host arena and failing requests."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess

root = Path(__file__).resolve().parents[1]
output = root / "build/tests/native-heap"
output.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC", "cc")
obj = output / "heap.o"
binary = output / "test_native_heap"
common = [compiler, "-std=gnu11", "-O2", "-g", "-fno-pie", "-fno-omit-frame-pointer", "-pthread", "-I", str(root / "src")]
subprocess.run([*common, "-c", str(root / "native/heap.c"), "-o", str(obj)], check=True)
subprocess.run([*common, "-no-pie", str(root / "tests/test_native_heap.c"), str(root / "native/vlc_memory.c"),
                str(obj), "-o", str(binary)], check=True)
result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
(output / "test-output.txt").write_text(result.stdout)
events = []
for line in result.stdout.splitlines():
    match = re.fullmatch(r"NATIVE_HEAP_EVENT operation=(\d+) requested=(\d+) alignment=(\d+) caller=(0x[0-9a-f]+)", line)
    if match:
        events.append(dict(operation=int(match[1]), requested=int(match[2]), alignment=int(match[3]), caller=match[4]))
errors = []
if result.returncode or "NATIVE_HEAP_TESTS_OK" not in result.stdout:
    errors.append("Native allocator checks did not complete.")
if events:
    symbols = subprocess.run(["addr2line", "-f", "-C", "-e", str(binary), *(event["caller"] for event in events)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout.splitlines()
    for index, event in enumerate(events):
        event["function"], event["source"] = symbols[index * 2:index * 2 + 2]
        if "tests/test_native_heap.c:" not in event["source"]:
            errors.append(f"Operation {event['operation']} recorded an inner allocator caller: {event['source']}")
else:
    errors.append("No allocation failure was exercised.")
if set(event["operation"] for event in events) != set(range(1, 9)):
    errors.append("The failure test did not cover all eight public allocation operations.")
files = [root / "native/heap.c", root / "native/vlc_memory.c", root / "native/dlmalloc.c",
         root / "src/diagnostics_native.h", root / "src/native_memory.h",
         root / "tests/test_native_heap.c", Path(__file__).resolve()]
report = {"result": "FAIL" if errors else "PASS",
    "scope": "Production native heap wrappers, shared direct allocator and dlmalloc; real host-backed mappings verify 256 MiB arena fallback, all large allocation entry points, aligned FFmpeg-size buffers, realloc migration/failure, physical ownership, retained cleanup, concurrent use and diagnostic attribution",
    "limits": ["Existing dlmalloc size-wraparound rejection may leave errno unchanged for malloc(SIZE_MAX) and realloc(NULL, SIZE_MAX); the diagnostic recorder preserves that result. Actual arena exhaustion sets ENOMEM."],
    "checks": [line for line in result.stdout.splitlines() if line.startswith("NATIVE_HEAP_TESTS_OK")],
    "events": events, "errors": errors,
    "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
(output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout)
if errors:
    raise SystemExit(" ".join(errors))
print(f"PASS outer allocation caller attribution: {len(events)} symbolized failure records")
print(output / "result.json")
