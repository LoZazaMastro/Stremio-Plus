#!/usr/bin/env python3
"""Exercise the isolated download writer through its real framed transport."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


HEADER = struct.Struct("<IHHIIIIiIqqQQ")
MAGIC = 0x31574453
VERSION = 3
BLOCK = 4 << 20
BEGIN, WRITE, CHECKPOINT, CLOSE, RESPONSE = range(1, 6)
checks = 0
scenarios = []


def check(condition, description):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(description)


def data_bytes(size):
    pattern = bytes((i * 17 + 31) % 251 for i in range(251))
    return (pattern * (size // len(pattern) + 1))[:size]


class Client:
    def __init__(self, executable, root, faults=None, shim=None):
        parent, child = socket.socketpair()
        parent.settimeout(5)
        environment = os.environ.copy()
        if faults:
            environment.update(faults)
            environment["LD_PRELOAD"] = str(shim)
        self.process = subprocess.Popen([str(executable), str(root)], stdin=child,
                                        stdout=child, stderr=subprocess.PIPE, env=environment)
        child.close()
        self.socket = parent
        self.sequence = 0
        self.offset = 0
        self.total = 0
        self.root = str(root)

    def stop(self):
        self.socket.close()
        deadline = time.monotonic() + 3
        while True:
            pid, status, usage = os.wait4(self.process.pid, os.WNOHANG)
            if pid:
                self.process.returncode = os.waitstatus_to_exitcode(status)
                self.peak_rss_kib = usage.ru_maxrss
                break
            if time.monotonic() >= deadline:
                break
            time.sleep(0.01)
        if self.process.returncode is None:
            self.process.kill()
            self.process.wait()
            raise AssertionError("helper did not exit after client EOF")
        self.process.stderr.close()

    def receive(self):
        data = bytearray()
        while len(data) < HEADER.size:
            part = self.socket.recv(HEADER.size - len(data))
            if not part:
                raise EOFError("helper closed the protocol connection")
            data.extend(part)
        values = HEADER.unpack(data)
        check(values[:3] == (MAGIC, VERSION, HEADER.size), "valid reply envelope")
        check(values[3] == RESPONSE and values[4] == os.getpid(), "reply operation and process")
        check(values[6] == 0 and 0 <= values[8] <= 15 and (values[7] != 0 or values[8] == 0),
              "reply has no body and a valid error stage")
        return values

    def frame(self, operation, payload=b"", offset=None, total=None, sequence=None,
              declared_size=None, fragmented=False):
        self.sequence += 1
        sequence = self.sequence if sequence is None else sequence
        offset = self.offset if offset is None else offset
        total = self.total if total is None else total
        frame = HEADER.pack(MAGIC, VERSION, HEADER.size, operation, os.getpid(), sequence,
                            len(payload) if declared_size is None else declared_size,
                            0, 0, offset, total, 0, 0) + payload
        if fragmented:
            for byte in frame[:HEADER.size]:
                self.socket.sendall(bytes([byte]))
            for at in range(HEADER.size, len(frame), 23):
                self.socket.sendall(frame[at:at + 23])
        else:
            self.socket.sendall(frame)
        reply = self.receive()
        check(reply[5] == sequence and reply[10] == total, "reply matches request sequence and total")
        return reply

    def begin_payload(self, job_id, directory=None):
        return job_id.encode() + b'\0' + str(directory or self.root).encode()

    def begin(self, job_id, total, offset=0, fragmented=False, directory=None):
        self.total = total
        reply = self.frame(BEGIN, self.begin_payload(job_id, directory), offset=offset, fragmented=fragmented)
        check(reply[7] == 0, "begin succeeded")
        check(reply[9] in (0, offset), "begin returned only a validated candidate prefix")
        self.offset = reply[9]
        return reply

    def write(self, data, fragmented=False):
        expected = self.offset + len(data)
        reply = self.frame(WRITE, data, fragmented=fragmented)
        check(reply[7] == 0 and reply[9] == expected, "write acknowledged the exact full prefix")
        self.offset = expected

    def state(self):
        return {"version": 1, "kind": "torrent", "source": "1" * 40 + ":0",
                "bytes": self.offset, "total": self.total, "extension": ".mkv"}

    def checkpoint(self):
        state = self.state()
        payload = json.dumps(state, separators=(",", ":")).encode()
        reply = self.frame(CHECKPOINT, payload)
        check(reply[7] == 0 and reply[9] == self.offset, "checkpoint acknowledged the written prefix")
        return state

    def close(self):
        reply = self.frame(CLOSE)
        check(reply[7] == 0 and reply[9] == self.offset, "close acknowledged the committed prefix")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    executable = args.helper.resolve()
    with tempfile.TemporaryDirectory(prefix="stremio-writer-tests-") as temporary:
        base = Path(temporary)
        root = base / "downloads"
        root.mkdir()
        shim = base / "io-faults.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                        str(Path(__file__).with_name("download_writer_faults.c")),
                        "-ldl", "-o", str(shim)], check=True)
        next_job = 0

        def job():
            nonlocal next_job
            next_job += 1
            name = "d" + f"{next_job:032x}"
            folder = root / name
            folder.mkdir()
            return name, folder

        @contextlib.contextmanager
        def client(faults=None):
            connection = Client(executable, root, faults, shim)
            try:
                yield connection
            finally:
                connection.stop()

        def rejected(connection, operation, payload=b"", **kwargs):
            try:
                reply = connection.frame(operation, payload, **kwargs)
                check(reply[7] != 0, "invalid request returned an error")
            except (EOFError, BrokenPipeError, ConnectionResetError):
                check(True, "invalid request closed the connection")

        for length, fragmented, faults in [(BLOCK + 137, False, None),
                                            (997, True, None),
                                            (BLOCK + 19, False, {"STREMIO_TEST_IO_FAULT": "interrupt"})]:
            name, folder = job()
            expected = data_bytes(length)
            with client(faults) as connection:
                connection.begin(name, length, fragmented=fragmented)
                connection.checkpoint()
                for at in range(0, length, BLOCK):
                    connection.write(expected[at:at + BLOCK], fragmented=fragmented)
                    check((folder / "media.part").read_bytes() == expected[:connection.offset],
                          "acknowledged bytes are already readable")
                state = connection.checkpoint()
                check(json.loads((folder / "transfer.json").read_text()) == state,
                      "checkpoint JSON was persisted atomically")
                connection.close()
                descriptor = os.open(folder, os.O_RDONLY)
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    check(True, "close released the directory lock before its acknowledgement")
                finally:
                    os.close(descriptor)
            check(hashlib.sha256((folder / "media.part").read_bytes()).digest() == hashlib.sha256(expected).digest(),
                  "completed bytes match the original fixture")
        scenarios.append("full blocks, final tails, fragmented frames, EINTR and short operations")

        name, folder = job()
        expected = data_bytes(8197)
        (folder / "media.part").write_bytes(expected[:4096] + b"untrusted tail")
        with client() as connection:
            connection.begin(name, len(expected), offset=4096)
            check((folder / "media.part").read_bytes() == expected[:4096], "resume truncated the untrusted tail")
            connection.checkpoint()
            connection.write(expected[4096:])
            connection.checkpoint()
            connection.close()
        check((folder / "media.part").read_bytes() == expected, "resumed bytes are exact")
        name, folder = job()
        with client() as connection:
            connection.begin(name, 8192, offset=4096)
            check(connection.offset == 0, "missing candidate prefix restarts at zero")
            connection.checkpoint()
            connection.close()
        scenarios.append("validated resume and missing-prefix rollback")

        for mutation in ({"sequence": 8}, {"offset": 1}, {"declared_size": BLOCK + 1},
                         {"total": 63}, {"offset": (1 << 63) - 1, "total": (1 << 63) - 1}):
            name, folder = job()
            with client() as connection:
                connection.begin(name, 64)
                connection.checkpoint()
                rejected(connection, WRITE, b"x", **mutation)
            check((folder / "media.part").read_bytes() == b"", "rejected write did not mutate media")
        scenarios.append("wrong sequence, offset, total, oversized block and overflow bounds")

        name, folder = job()
        with client() as connection:
            connection.begin(name, 64)
            state = connection.checkpoint()
            partial = HEADER.pack(MAGIC, VERSION, HEADER.size, WRITE, os.getpid(), 3, 32, 0, 0, 0, 64, 0, 0)
            connection.socket.sendall(partial + b"unfinished")
        check((folder / "media.part").read_bytes() == b"", "truncated body never reached the media writer")
        check(json.loads((folder / "transfer.json").read_text()) == state, "EOF preserved the durable checkpoint")
        scenarios.append("truncated packet and client EOF preserve durable state")

        for failure, faults in [("partial write", {"STREMIO_TEST_WRITE_LIMIT": "1024"}),
                                ("media sync", {"STREMIO_TEST_SYNC_FAILURE": "4"}),
                                ("state sync", {"STREMIO_TEST_SYNC_FAILURE": "5"})]:
            name, folder = job()
            with client(faults) as connection:
                connection.begin(name, 4096)
                old_state = connection.checkpoint()
                if failure == "partial write":
                    rejected(connection, WRITE, data_bytes(4096))
                else:
                    connection.write(data_bytes(4096))
                    rejected(connection, CHECKPOINT, json.dumps(connection.state()).encode())
            check(json.loads((folder / "transfer.json").read_text()) == old_state,
                  failure + " did not replace the durable checkpoint")
            with client() as connection:
                connection.begin(name, 4096, offset=0)
                check((folder / "media.part").stat().st_size == 0, "retry discarded uncommitted failed-write tail")
                connection.checkpoint()
                connection.write(data_bytes(4096))
                connection.checkpoint()
                connection.close()
        scenarios.append("partial write, media fsync and checkpoint fsync failures with safe retry")

        name, folder = job()
        with client() as first:
            first.begin(name, 64)
            first.checkpoint()
            first.write(b"live")
            with client() as second:
                second.total = 64
                rejected(second, BEGIN, second.begin_payload(name), offset=0)
            check((folder / "media.part").read_bytes() == b"live", "conflicting begin did not truncate a live file")
            first.checkpoint()
            first.close()
        scenarios.append("exclusive job-directory ownership")

        for operation in (CLOSE, CHECKPOINT):
            name, folder = job()
            with client() as connection:
                connection.begin(name, 64)
                old_state = connection.checkpoint()
                connection.write(b"pending")
                payload = b""
                if operation == CHECKPOINT:
                    changed = connection.state()
                    changed["source"] = "2" * 40 + ":0"
                    payload = json.dumps(changed).encode()
                rejected(connection, operation, payload)
            check(json.loads((folder / "transfer.json").read_text()) == old_state,
                  "uncommitted close or changed identity preserved the checkpoint")
        scenarios.append("checkpoint identity stability and close requires committed bytes")

        name, folder = job()
        prefix = (1 << 32) + 17
        with (folder / "media.part").open("wb") as media:
            media.truncate(prefix)
        with client() as connection:
            connection.begin(name, prefix + 4097, offset=prefix)
            connection.checkpoint()
            connection.write(data_bytes(4097))
            connection.checkpoint()
            connection.close()
        with (folder / "media.part").open("rb") as media:
            media.seek(prefix)
            check(media.read() == data_bytes(4097), "offsets beyond four GiB retain their full width")
        scenarios.append("64-bit offsets beyond four GiB")

        name, folder = job()
        block = data_bytes(BLOCK)
        expected_hash = hashlib.sha256()
        blocks = 257
        with client() as connection:
            connection.begin(name, blocks * BLOCK)
            connection.checkpoint()
            for index in range(blocks):
                connection.write(block)
                expected_hash.update(block)
            connection.checkpoint()
            connection.close()
        helper_peak_rss_kib = connection.peak_rss_kib
        actual_hash = hashlib.sha256()
        with (folder / "media.part").open("rb") as media:
            while chunk := media.read(BLOCK):
                actual_hash.update(chunk)
        check(actual_hash.digest() == expected_hash.digest(), "sustained transfer matches every byte hash")
        check(0 < helper_peak_rss_kib < 64 * 1024,
              "helper memory stays bounded while transferring more than one GiB")
        scenarios.append("one GiB sustained byte integrity and bounded helper memory")

        outside = base / "outside"
        outside.mkdir()
        target = outside / "target"
        target.write_bytes(b"untouched")
        name, folder = job()
        (folder / "media.part").symlink_to(target)
        with client() as connection:
            connection.total = 64
            rejected(connection, BEGIN, connection.begin_payload(name))
        check(target.read_bytes() == b"untouched", "media symlink target was untouched")
        name, folder = job()
        folder.rmdir()
        folder.symlink_to(outside, target_is_directory=True)
        with client() as connection:
            connection.total = 64
            rejected(connection, BEGIN, connection.begin_payload(name))
        check(not (outside / "media.part").exists(), "job symlink did not redirect creation")
        with client() as connection:
            connection.total = 64
            rejected(connection, BEGIN, connection.begin_payload("d../" + "0" * 29))
        scenarios.append("invalid job ID and symlink refusal")

        selected = root / 'mnt' / 'ext1' / 'Stremio Plus Downloads'
        selected.mkdir(parents=True)
        name, old_folder = job()
        old_folder.rmdir()
        selected_job = selected / name
        selected_job.mkdir()
        expected = data_bytes(8193)
        with client() as connection:
            connection.begin(name, len(expected), directory=selected)
            connection.checkpoint()
            connection.write(expected)
            connection.checkpoint()
            connection.close()
        check((selected_job / 'media.part').read_bytes() == expected,
              'selected destination receives the exact video bytes')
        check(not old_folder.exists(), 'writer never recreates the old default destination')
        with client() as connection:
            connection.begin(name, len(expected) + 1, offset=len(expected), directory=selected)
            connection.checkpoint()
            connection.write(b'!')
            connection.checkpoint()
            connection.close()
        check((selected_job / 'media.part').read_bytes() == expected + b'!',
              'selected destination retains durable resume offsets')
        scenarios.append('selected mounted-volume destination, exact bytes and resume')

        redirect = root / 'redirect'
        redirect.symlink_to(selected, target_is_directory=True)
        for invalid in (str(redirect), str(root) + '/mnt/../mnt/ext1/Stremio Plus Downloads',
                        str(root) + '//mnt', str(root) + '/mnt\0ignored'):
            with client() as connection:
                connection.total = len(expected) + 1
                rejected(connection, BEGIN, connection.begin_payload(name, invalid))
        check((selected_job / 'media.part').read_bytes() == expected + b'!',
              'redirected and noncanonical selected paths never truncate an existing video')
        scenarios.append('selected directory symlink and traversal refusal')

        report = {"checks": checks, "scenarios": scenarios, "passed": True,
                  "helper_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                  "sustained_bytes": blocks * BLOCK, "helper_peak_rss_KiB": helper_peak_rss_kib,
                  "scope": "host protocol, byte integrity, crash recovery and injected I/O failures; no PS5 speed claim"}
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
