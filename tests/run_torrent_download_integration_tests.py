#!/usr/bin/env python3
"""Production torrent engine + transfer, synthetic media and offline peer input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    checked_sources = [root / name for name in (
        "src/torrent/engine.cpp", "src/torrent/engine.h", "src/torrent/bencode.cpp",
        "src/download_transfer.cpp", "src/download_transfer.h", "src/download_telemetry.h", "src/growing_file.h",
        "src/growing_file_stream.cpp", "src/growing_file_stream.h", "src/netstream.cpp", "src/http.cpp", "src/util.cpp",
        "tests/test_torrent_download_integration.cpp", "tests/run_torrent_download_integration_tests.py")]

    def source_hashes():
        return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in checked_sources}

    initial_sources = source_hashes()
    with tempfile.TemporaryDirectory(prefix="stremio-torrent-download-") as temporary:
        folder = Path(temporary)
        fixture = folder / "synthetic.mp4"
        subprocess.run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "8",
            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "15", "-c:a", "aac",
            "-movflags", "+faststart", str(fixture),
        ], check=True, timeout=30)
        tail_fixture = folder / "synthetic-tail.mp4"
        subprocess.run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "20",
            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "15", "-c:a", "aac", str(tail_fixture),
        ], check=True, timeout=30)
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil", "libcurl", "openssl"
        ], text=True))
        executable = folder / "test_torrent_download_integration"
        compiler = os.environ.get("HOST_CXX", "clang++-18")
        subprocess.run([
            compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-deprecated-declarations", "-Wno-unused-function", "-Wno-missing-field-initializers",
            "-Wno-reorder-ctor", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(root / "src"), "-I" + str(root / "third_party"),
            str(root / "tests/test_torrent_download_integration.cpp"),
            str(root / "src/torrent/bencode.cpp"), str(root / "src/download_transfer.cpp"),
            str(root / "src/growing_file_stream.cpp"),
            str(root / "src/netstream.cpp"), str(root / "src/http.cpp"), str(root / "src/util.cpp"),
            "-Wl,--wrap=socket", "-Wl,--wrap=connect", "-Wl,--wrap=getaddrinfo", "-Wl,--wrap=fsync",
            *flags, "-o", str(executable),
        ], check=True, timeout=180)
        environment = dict(os.environ)
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        run = subprocess.run([str(executable), str(folder / "jobs"), str(fixture), str(tail_fixture)],
                             timeout=60, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(run.stdout, end="", flush=True)
        run.check_returncode()
        for mode in ("cold", "resume", "large"):
            subprocess.run([
                "ffmpeg", "-v", "error", "-xerror", "-i", str(folder / "jobs" / mode / "media.part"),
                "-map", "0:v", "-map", "0:a", "-f", "null", "-"
            ], check=True, timeout=30)
        print("FFmpeg full offline decode: cold, resumed and 64 MiB large-piece torrent downloads passed")
        subprocess.run([
            "ffmpeg", "-v", "error", "-xerror", "-i", str(folder / "jobs" / "progressive" / "media.mp4"),
            "-map", "0:v", "-map", "0:a", "-f", "null", "-"
        ], check=True, timeout=30)
        print("Real-engine progressive MP4: first frame before download completion, shared auxiliary tail metadata and full final decode passed")
        assert source_hashes() == initial_sources, "sources changed during integration; report was not published"
        assertions = re.search(r"PASS: (\d+) production torrent download integration assertions", run.stdout)
        cold = re.search(r"Cold download: (\d+) bytes, (\d+) progress events, (\d+) SHA-1-verified pieces", run.stdout)
        resume = re.search(r"Resume: (\d+) -> (\d+) bytes; first piece (\d+)", run.stdout)
        large = re.search(r"Large-piece download: (\d+) bytes, (\d+) verified 8 MiB pieces, (\d+) durable media syncs", run.stdout)
        progressive = re.search(r"Progressive real-engine decode: frames=(\d+) first_frame_prefix=(\d+) total=(\d+) percent=([\d.]+) auxiliary_tail_pieces=(\d+)", run.stdout)
        assert assertions and cold and resume and large and progressive
        report = {
            "result": "PASS", "assertions": int(assertions[1]),
            "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
            "engine_methods_stubbed": False, "network_attempts": 0,
            "cold_download_bytes": int(cold[1]), "progress_events": int(cold[2]), "sha1_verified_pieces": int(cold[3]),
            "paused_bytes": int(resume[1]), "resumed_bytes": int(resume[2]), "first_resumed_piece": int(resume[3]),
            "media_file_torrent_offset": 81937, "disk_cache_slots": 8,
            "large_piece_transfer": {
                "bytes": int(large[1]), "piece_length": 8 << 20, "verified_pieces": int(large[2]), "media_syncs": int(large[3]),
            },
            "real_engine_progressive": {
                "decoded_frames": int(progressive[1]), "first_frame_prefix": int(progressive[2]),
                "total": int(progressive[3]), "first_frame_percent": float(progressive[4]),
                "auxiliary_tail_pieces": int(progressive[5]), "auxiliary_read_window_bytes": 65536,
                "writer_held_until_first_frame": True, "same_torrent_and_cache": True,
                "download_continues_after_metadata_reader_closes": True, "completed_file_bytes_exact": True,
            },
            "offline_full_decodes": 4, "input_sha256": initial_sources,
            "console_validation": "Not performed; production code exercised on host with synthetic valid peer blocks, no external swarm.",
            "stdout": run.stdout,
        }
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
