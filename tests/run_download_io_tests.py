#!/usr/bin/env python3
"""Real loopback transfers and durable-resume failure injection.

Optionally compares the same workload with an explicitly supplied previous
download_transfer.cpp. Results measure host filesystem synchronization, not
PS5 network or SSD performance. No tracker or external provider is contacted.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import shlex
import shutil
import statistics
import struct
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-source", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    checked_sources = [root / name for name in (
        "src/download_transfer.cpp", "src/download_transfer.h", "src/download_telemetry.h", "src/netstream.cpp", "src/http.cpp", "src/util.cpp",
        "tests/test_download_io.cpp", "tests/run_download_io_tests.py")]

    def source_hashes():
        return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in checked_sources}

    initial_sources = source_hashes()
    with tempfile.TemporaryDirectory(prefix="stremio-download-io-") as temporary:
        folder = Path(temporary)
        seed = folder / "seed.mp4"
        subprocess.run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "3",
            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "15", "-c:a", "aac",
            "-movflags", "+faststart", str(seed),
        ], check=True, timeout=30)
        original = seed.read_bytes()

        def padded(total):
            # A valid MP4 free atom creates a large finite file without making
            # the comparison CPU-bound on video encoding or unrelated content.
            extra = total - len(original)
            assert extra >= 8
            return original + struct.pack(">I4s", extra, b"free") + bytes(extra - 8)

        large = padded(320 << 20)
        slow = padded(4 << 20)
        fixture = folder / "large.mp4"
        fixture.write_bytes(large)
        Path(str(fixture) + ".slow").write_bytes(padded(128 << 20))

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_GET(self):
                if self.path not in ("/large", "/slow"):
                    self.send_error(404)
                    return
                body = large if self.path == "/large" else slow
                self.send_response(200)
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("ETag", '"synthetic-v1"')
                self.end_headers()
                try:
                    for offset in range(0, len(body), 65536):
                        self.wfile.write(body[offset:offset + 65536])
                        if self.path == "/slow":
                            self.wfile.flush()
                            time.sleep(.105)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        environment = dict(os.environ)
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil", "libcurl"
        ], text=True))
        compiler = os.environ.get("HOST_CXX", "clang++-18")

        def compile_binary(source, label, sanitize):
            # The copied baseline sees the current source-compatible progress
            # struct. No previous header can shadow it and change its ABI.
            private = folder / label
            private.mkdir()
            transfer = private / "download_transfer.cpp"
            shutil.copyfile(source, transfer)
            executable = private / "test_download_io"
            command = [
                compiler, "-std=c++20", "-O1" if sanitize else "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers", "-Wno-reorder-ctor", "-pthread",
                "-I" + str(root / "src"), "-I" + str(root / "third_party"),
                str(transfer), str(root / "tests/test_download_io.cpp"),
                str(root / "src/netstream.cpp"), str(root / "src/http.cpp"), str(root / "src/util.cpp"),
                "-Wl,--wrap=write", "-Wl,--wrap=fsync", "-Wl,--wrap=rename", "-Wl,--wrap=fcntl", *flags, "-o", str(executable),
            ]
            if sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=180)
            return executable

        try:
            current_source = root / "src/download_transfer.cpp"
            current = compile_binary(current_source, "current", True)
            report_file = folder / "durability.json"
            subprocess.run([str(current), "--durability", str(folder / "durability"), base,
                            str(fixture), str(report_file)], check=True, timeout=90, env=environment)
            report = {
                "result": "PASS",
                "scope": "Instrumented host I/O; real loopback HTTP, verified synthetic torrent input, 25 ms injected sync latency only in comparisons; no PS5 throughput claim",
                "currentSourceSha256": hashlib.sha256(current_source.read_bytes()).hexdigest(),
                "sourceHashes": initial_sources,
                "hostSystem": os.uname().sysname + " " + os.uname().machine,
                "hostFilesystem": subprocess.check_output(["stat", "-f", "-c", "%T", str(folder)], text=True).strip(),
                "durabilitySanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
                "durability": json.loads(report_file.read_text()),
                "baseline": [], "current": [],
            }
            if args.baseline_source:
                assert args.baseline_source.is_file()
                report["baselineSourceSha256"] = hashlib.sha256(args.baseline_source.read_bytes()).hexdigest()
                binaries = {
                    "baseline": compile_binary(args.baseline_source, "baseline-benchmark", False),
                    "current": compile_binary(current_source, "current-benchmark", False),
                }
                # Alternate the order to reduce page-cache/warm-up bias.
                for attempt in range(3):
                    for label in (("baseline", "current") if attempt % 2 == 0 else ("current", "baseline")):
                        result_path = folder / f"{label}-{attempt}.json"
                        subprocess.run([
                            str(binaries[label]), "--benchmark", str(folder / f"run-{label}-{attempt}"),
                            base, str(fixture), str(result_path)
                        ], check=True, timeout=30, env=environment)
                        report[label].append(json.loads(result_path.read_text()))
                for label in binaries:
                    rows = report[label]
                    report[label + "Summary"] = {
                        "runs": len(rows), "bytesEach": len(large),
                        "medianSeconds": statistics.median(row["elapsedSeconds"] for row in rows),
                        "medianSyncMilliseconds": statistics.median(row["syncMilliseconds"] for row in rows),
                        "syncCalls": [sum(row[key] for key in ("mediaSyncs", "checkpointSyncs", "directorySyncs")) for row in rows],
                        "durabilityOrderValid": all(row["durabilityOrderValid"] for row in rows),
                    }
                baseline_calls = statistics.median(report["baselineSummary"]["syncCalls"])
                current_calls = statistics.median(report["currentSummary"]["syncCalls"])
                assert current_calls <= baseline_calls / 2
                report["syncCallReductionPercent"] = 100 * (1 - current_calls / baseline_calls)
                print(json.dumps({key: report[key] for key in (
                    "baselineSummary", "currentSummary", "syncCallReductionPercent")}, indent=2))
            assert source_hashes() == initial_sources, "sources changed during the run; report was not published"
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(report, indent=2) + "\n")
            print("Durable I/O: cancellation, read/write/fsync/rename failure, exact resume and timed checkpoints passed")
        finally:
            server.shutdown(); server.server_close(); thread.join()


if __name__ == "__main__":
    main()
