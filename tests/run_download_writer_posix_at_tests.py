#!/usr/bin/env python3
"""Verify native syscall error normalization through the actual writer helper."""
import argparse
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import run_download_writer_tests as protocol


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = [root / path for path in (
        "native/download_writer/posix_at.hpp", "native/download_writer/protocol.hpp",
        "native/download_writer/helper/main.cpp", "tests/download_writer_at_syscalls.cpp",
        "tests/download_writer_faults.c", "tests/run_download_writer_tests.py",
        "tests/run_download_writer_posix_at_tests.py",
    )]
    hashes = lambda: {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    initial_hashes = hashes()
    compiler = os.environ.get("HOST_CXX", "clang++-18")
    common = [compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
              "-DSTREMIO_DOWNLOAD_WRITER_TEST", "-DSTREMIO_DOWNLOAD_WRITER_AT_TEST",
              "-I" + str(root / "native")]
    results = []
    with tempfile.TemporaryDirectory(prefix="stremio-native-checkpoint-") as temporary:
        folder = Path(temporary)
        unit = folder / "adapter-tests"
        hook = root / "tests/download_writer_at_syscalls.cpp"
        subprocess.run(common + ["-DSTREMIO_DOWNLOAD_WRITER_AT_UNIT_TEST", str(hook), "-o", str(unit)],
                       check=True, timeout=60)
        unit_result = subprocess.check_output([str(unit)], text=True, timeout=10).strip()
        print(unit_result, flush=True)
        helper = folder / "download-writer-helper"
        subprocess.run(common + [str(root / "native/download_writer/helper/main.cpp"), str(hook), "-o", str(helper)],
                       check=True, timeout=120)
        shim = folder / "io-faults.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(root / "tests/download_writer_faults.c"), "-ldl", "-o", str(shim)],
                       check=True, timeout=30)
        downloads = folder / "downloads"
        downloads.mkdir(mode=0o777)
        os.chmod(downloads, 0o777)
        job_number = 0

        def job():
            nonlocal job_number
            job_number += 1
            name = "d" + f"{job_number:032x}"
            directory = downloads / name
            directory.mkdir(mode=0o777)
            os.chmod(directory, 0o777)
            return name, directory

        @contextlib.contextmanager
        def client(faults=None):
            connection = protocol.Client(helper, downloads, faults, shim)
            try:
                yield connection
            finally:
                connection.stop()

        name, directory = job()
        with client() as connection:
            connection.begin(name, 29110304133)
            state = connection.checkpoint()
            protocol.check(json.loads((directory / "transfer.json").read_text()) == state,
                           "absent temporary file is normal with positive ENOENT and stale errno zero")
            connection.write(protocol.data_bytes(4097))
            state = connection.checkpoint()
            protocol.check(json.loads((directory / "transfer.json").read_text()) == state,
                           "later checkpoint atomically replaces the first checkpoint through native adapters")
            protocol.check(not (directory / "transfer.json.tmp").exists(), "successful checkpoint leaves no temporary file")
            connection.close()
        protocol.check((directory / "media.part").read_bytes() == protocol.data_bytes(4097),
                       "checkpoint normalization preserves exact acknowledged media bytes")
        results.append("missing temp, initial checkpoint and later replacement with raw positive errno")

        for fault, expected_errno, expected_stage in (
            ("unlink-temp-denied", errno.EACCES, 6),
            ("open-temp-full", errno.ENOSPC, 7),
            ("rename-state-denied", errno.EACCES, 12),
        ):
            name, directory = job()
            prefix = b"verified prefix"
            with client() as connection:
                connection.begin(name, 64)
                connection.checkpoint()
                connection.write(prefix)
                connection.checkpoint()
                connection.close()
            previous = (directory / "transfer.json").read_bytes()
            with client({"STREMIO_TEST_AT_FAULT": fault}) as connection:
                connection.begin(name, 64, offset=len(prefix))
                reply = connection.frame(protocol.CHECKPOINT, json.dumps(connection.state()).encode())
                protocol.check(reply[7] == expected_errno and reply[8] == expected_stage,
                               "real filesystem failure retains its errno and precise helper stage: " + fault)
            protocol.check((directory / "transfer.json").read_bytes() == previous,
                           "failed checkpoint does not replace the previous durable state: " + fault)
            protocol.check((directory / "media.part").read_bytes() == prefix,
                           "failed checkpoint preserves the verified media prefix: " + fault)
            with client() as connection:
                connection.begin(name, 64, offset=len(prefix))
                connection.checkpoint()
                connection.write(b" resumed")
                connection.checkpoint()
                connection.close()
            protocol.check((directory / "media.part").read_bytes() == prefix + b" resumed",
                           "a failed native syscall can be retried without losing or duplicating media bytes")
        results.append("native EACCES and ENOSPC remain fatal, keep previous checkpoint and resume safely")

        for ordinal, stage in ((1, 5), (2, 10), (3, 13)):
            name, directory = job()
            with client({"STREMIO_TEST_SYNC_FAILURE": str(ordinal)}) as connection:
                connection.begin(name, 64)
                reply = connection.frame(protocol.CHECKPOINT, json.dumps(connection.state()).encode())
                protocol.check(reply[7] == errno.EIO and reply[8] == stage,
                               "media, state and directory fsync errors are never ignored")
            if ordinal < 3:
                protocol.check(not (directory / "transfer.json").exists(),
                               "failed pre-rename flush does not publish a checkpoint")
            else:
                protocol.check(json.loads((directory / "transfer.json").read_text())["bytes"] == 0,
                               "directory fsync failure retains a valid atomic state but never acknowledges durability")
        results.append("media, checkpoint file and directory fsync retain distinct fatal error stages")

        for change in ({"bytes": "0"}, {"total": 64.0}, {"source": "invalid"}, {"extension": "../bad"}):
            name, directory = job()
            with client() as connection:
                connection.begin(name, 64)
                state = connection.state()
                state.update(change)
                reply = connection.frame(protocol.CHECKPOINT, json.dumps(state).encode())
                protocol.check(reply[7] == errno.EINVAL and reply[8] == 4,
                               "malformed state is rejected before filesystem changes")
            protocol.check(not (directory / "transfer.json").exists(), "invalid state never creates a checkpoint")
        results.append("JSON types, torrent identity and media extension validation remain enforced")

    assert hashes() == initial_hashes, "sources changed during the run; report was not published"
    report = {"result": "PASS", "adapter": unit_result, "protocol_checks": protocol.checks,
              "scenarios": results, "sourceHashes": initial_hashes,
              "scope": "Host helper with production native syscall normalization and injected FreeBSD carry/errno results; no PS5 throughput claim"}
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
