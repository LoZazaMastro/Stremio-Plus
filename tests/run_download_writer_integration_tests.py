#!/usr/bin/env python3
"""Exercise the real download writer Client and helper-backed transfer boundary.

Uses a generated playable MP4 and deterministic verified torrent bytes. These
checks establish acknowledgement, cancellation and durable-resume behavior;
they do not estimate PS5 disk throughput or live-swarm performance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = [root / name for name in (
        "native/download_writer/protocol.hpp", "native/download_writer/client.hpp",
        "native/download_writer/client.cpp", "native/download_writer/helper/main.cpp",
        "native/download_writer/posix_at.hpp",
        "src/download_transfer.cpp", "src/download_transfer.h", "src/download_telemetry.h",
        "src/torrent/engine.h", "src/http.cpp", "src/netstream.cpp", "src/util.cpp",
        "tests/test_download_writer_client.cpp", "tests/test_download_writer_transfer.cpp",
        "tests/download_writer_at_syscalls.cpp",
        "tests/download_writer_faults.c",
        "tests/run_download_writer_integration_tests.py",
    )]

    def hashes():
        return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}

    initial = hashes()
    compiler = os.environ.get("HOST_CXX", "clang++-18")
    environment = dict(os.environ)
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    common = [compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]
    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    output = {}
    with tempfile.TemporaryDirectory(prefix="stremio-writer-integration-") as temporary:
        folder = Path(temporary)
        helper = folder / "download-writer-helper"
        faults = folder / "download-writer-faults.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(root / "tests/download_writer_faults.c"), "-ldl", "-o", str(faults)],
                       check=True, timeout=30)
        subprocess.run(common + ["-DSTREMIO_DOWNLOAD_WRITER_TEST", "-DSTREMIO_DOWNLOAD_WRITER_AT_TEST",
                                 "-I" + str(root / "native"),
                                 str(root / "native/download_writer/helper/main.cpp"),
                                 str(root / "tests/download_writer_at_syscalls.cpp"), "-o", str(helper)],
                       check=True, timeout=120)
        client = folder / "test-client"
        subprocess.run(common + sanitizers + ["-I" + str(root / "native/download_writer"),
                       str(root / "native/download_writer/client.cpp"),
                       str(root / "tests/test_download_writer_client.cpp"), "-o", str(client)],
                       check=True, timeout=120)
        output["client"] = subprocess.check_output([str(client)], text=True, timeout=20, env=environment).strip()
        print(output["client"], flush=True)
        dependencies = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil", "libcurl"], text=True))
        transfer = folder / "test-transfer"
        subprocess.run(common + sanitizers + [
            "-DSTREMIO_DOWNLOAD_WRITER_TEST", "-Wno-missing-field-initializers", "-Wno-reorder-ctor",
            "-I" + str(root / "src"), "-I" + str(root / "third_party"), "-I" + str(root / "native"),
            str(root / "src/download_transfer.cpp"), str(root / "native/download_writer/client.cpp"),
            str(root / "tests/test_download_writer_transfer.cpp"), str(root / "src/netstream.cpp"),
            str(root / "src/http.cpp"), str(root / "src/util.cpp"), "-Wl,--wrap=fstatfs", *dependencies, "-o", str(transfer),
        ], check=True, timeout=180)
        fixture = folder / "synthetic.mp4"
        subprocess.run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
            "-t", "2", "-c:v", "libx264", "-preset", "ultrafast", "-movflags", "+faststart", str(fixture),
        ], check=True, timeout=30)
        original = fixture.read_bytes()
        padding = (24 << 20) + 123 - len(original)
        assert padding > 8
        fixture.write_bytes(original + struct.pack(">I4s", padding, b"free") + bytes(padding - 8))
        output["transfer"] = subprocess.check_output(
            [str(transfer), str(helper), str(folder / "downloads"), str(fixture), str(faults)],
            text=True, timeout=45, env=environment).strip()
        print(output["transfer"], flush=True)
    assert hashes() == initial, "sources changed during the run; report was not published"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps({
            "result": "PASS", "scope": "Host Client and helper-backed transfer correctness; deterministic torrent input; no PS5 throughput claim",
            "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
            "sourceHashes": initial, "results": output,
        }, indent=2) + "\n")


if __name__ == "__main__":
    main()
