#!/usr/bin/env python3
"""Exercise production HEVC/Dolby Vision parsing without a Sony decoder."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/vlc-ps5/modules/ps5vdec.c"
HARNESS = ROOT / "tests/test_ps5vdec_dovi.c"


def main():
    source = SOURCE.read_text()
    start = source.index("static bool hevc_skip")
    helpers = source[start:source.index("/* The parameter sets from the extra data:", start)]
    compiler = os.environ.get("HOST_CC") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("A host C compiler is required")
    with tempfile.TemporaryDirectory(prefix="stremio-ps5vdec-dovi-") as temporary:
        folder = Path(temporary)
        (folder / "ps5vdec_dovi_under_test.h").write_text(helpers)
        binary = folder / "ps5vdec-dovi"
        subprocess.run([compiler, "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(folder),
            str(HARNESS), "-o", str(binary)], check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        result = subprocess.run([str(binary)], capture_output=True, text=True,
            check=True, timeout=30, env=environment)
    print(result.stdout, end="")
    checks = int(result.stdout.split("checks=")[1].split()[0])
    report = {"result": "PASS", "checks": checks, "sanitizers": ["address", "undefined"],
        "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (SOURCE, HARNESS, Path(__file__).resolve())},
        "limits": "Production bitstream helper tests; no PS5 driver, scanout or sustained-performance validation."}
    output = ROOT / "build/tests/ps5vdec-dovi.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()
