#!/usr/bin/env python3
"""Run production SPS recovery against the real pinned VLC HEVC parser."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/vlc-ps5/modules/ps5vdec.c"
HARNESS = ROOT / "tests/test_ps5vdec_sps.c"
REPORT = ROOT / "build/tests/ps5vdec-sps.json"


def main():
    vlc = Path(os.environ.get("VLC_SOURCE_DIR", str(ROOT / ".deps/vlc/src/vlc-3.0.24")))
    parser = vlc / "modules/packetizer/hevc_nal.c"
    if not parser.is_file():
        raise RuntimeError("VLC_SOURCE_DIR must name the pinned extracted VLC 3.0.24 source")
    source = SOURCE.read_text()
    start = source.index("/* HEVC SPS VUI colour recovery")
    helper = source[start:source.index("/* A picture the decoder gave back:", start)]
    compiler = os.environ.get("HOST_CC") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("A host C compiler is required")
    with tempfile.TemporaryDirectory(prefix="stremio-ps5vdec-sps-") as temporary:
        folder = Path(temporary)
        (folder / "ps5vdec_sps_under_test.h").write_text(helper)
        binary = folder / "ps5vdec-sps"
        command = [compiler, "-std=c11", "-D_GNU_SOURCE", "-g", "-O1", "-Wall", "-Wextra",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                   "-I", str(folder), "-I", str(vlc / "include"),
                   "-I", str(parser.parent), str(HARNESS), str(parser), "-o", str(binary)]
        subprocess.run(command, check=True)
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run([str(binary)], env=environment, capture_output=True,
                                text=True, timeout=30)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError(f"SPS colour recovery failed: {result.returncode}")
    marker, cases, checks = result.stdout.strip().split("\t")
    if marker != "PASS":
        raise RuntimeError("Incomplete SPS parser result")
    report = {"result": "PASS", "cases": int(cases), "checks": int(checks),
              "sanitizers": ["address", "undefined"],
              "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in (SOURCE, HARNESS, Path(__file__).resolve())},
              "vlc_parser_sha256": hashlib.sha256(parser.read_bytes()).hexdigest(),
              "compiledHelperSha256": hashlib.sha256(helper.encode()).hexdigest(),
              "coverage": "Real emulation-prevented SPS; PQ, HLG, SDR, missing VUI/colour, range, known-field preservation, three/four-byte start codes, NAL header rejection, truncated and oversized NALs.",
              "limits": "Host validation of actual VLC SPS parsing and production recovery helper; does not prove console decode or HDMI colour."}
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PS5 HEVC SPS recovery: {cases} cases / {checks} checks PASS (real VLC parser, ASan, UBSan)")
    print(REPORT)


if __name__ == "__main__":
    main()
