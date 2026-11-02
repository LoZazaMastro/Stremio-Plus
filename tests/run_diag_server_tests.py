#!/usr/bin/env python3
"""Real TCP deadline, cap, follow and restart checks for production diag_server."""
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
checks = 0


def check(condition, description):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(description)


def main():
    compiler = os.environ.get("HOST_CXX") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("A host C++ compiler is required")
    source = ROOT / "src/diag_server.cpp"
    driver = ROOT / "tests/test_diag_server_driver.cpp"
    with tempfile.TemporaryDirectory(prefix="stremio-diag-http-") as temporary:
        folder = Path(temporary)
        binary = folder / "diag-server"
        subprocess.run([compiler, "-std=c++17", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "third_party"), str(source),
            str(driver), "-o", str(binary)], check=True)
        log = folder / "log.txt"
        # Larger than socket send/receive windows, to force an actual EAGAIN.
        log.write_bytes(b"LOG-LINE\n" * (8 * 1024 * 1024))
        (folder / "events.jsonl").write_text('{"event":1}\n')
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        process = subprocess.Popen([str(binary), str(folder)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, env=environment)
        clients = []

        def command(value):
            process.stdin.write(value + "\n"); process.stdin.flush()
            if not select.select([process.stdout], [], [], 4)[0]:
                raise RuntimeError("Server control deadline: " + value)
            response = process.stdout.readline().split()
            if len(response) != 4:
                raise RuntimeError("Server driver exited: " + process.stderr.read())
            return tuple(map(int, response[1:]))

        def state(wanted):
            until = time.monotonic() + 3
            while time.monotonic() < until:
                active, port, _ = command("state")
                if bool(active) == wanted:
                    return port
                time.sleep(0.02)
            raise RuntimeError("Server did not apply desired state")

        def connect(port, receive_buffer=None):
            client = socket.socket()
            if receive_buffer:
                client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
            client.settimeout(4)
            client.connect(("127.0.0.1", port)); clients.append(client)
            return client

        def request(port, target):
            client = connect(port)
            client.sendall(("GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n").encode())
            data = bytearray()
            while True:
                try:
                    part = client.recv(65536)
                except ConnectionResetError:
                    if data:  # Busy closes without consuming another request body.
                        break
                    raise
                if not part:
                    break
                data.extend(part)
            client.close()
            return bytes(data)

        try:
            _, _, elapsed = command("enable")
            check(elapsed < 100000, "configure must not bind or wait for clients on its caller")
            port = state(True)
            response = request(port, "/status")
            check(response.endswith(b'{"test":true}\n') and b"200 OK" in response, "status body")
            check(b'"event":1' in request(port, "/events"), "events endpoint")
            check(b"404 Not Found" in request(port, "/unknown"), "unknown endpoint")

            # Bytes arriving every half-second cannot renew the total request deadline.
            partial = connect(port); partial.sendall(b"GET /status HTTP/1.1\r\nHost:")
            start = time.monotonic()
            for _ in range(6):
                time.sleep(0.45)
                partial.sendall(b"a")
            check(partial.recv(1) == b"", "partial request must be closed")
            check(time.monotonic() - start < 3.8, "request deadline must be absolute")
            partial.close()

            # Four occupied workers are bounded and a fifth receives Busy.
            held = [connect(port) for _ in range(4)]
            time.sleep(0.15)
            check(b"503 Busy" in request(port, "/status"), "fifth client cap")
            for client in held:
                client.close()
            time.sleep(0.15)

            # A client that never reads a large log cannot hold a worker forever.
            slow = connect(port, 1024)
            slow.sendall(b"GET /log?tail=all HTTP/1.1\r\nHost: localhost\r\n\r\n")
            held = [connect(port) for _ in range(3)]
            time.sleep(2.5)
            check(b"200 OK" in request(port, "/status"), "slow-reader write deadline releases its slot")
            slow.close()
            for client in held:
                client.close()
            time.sleep(0.15)

            follower = connect(port)
            follower.sendall(b"GET /log?follow=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
            data = bytearray()
            while b"LOG-LINE" not in data:
                data.extend(follower.recv(65536))
            check(b"Transfer-Encoding: chunked" in data, "follow response uses chunks")
            with log.open("ab") as file:
                file.write(b"NEW-FOLLOW-MARKER\n")
            until = time.monotonic() + 2
            while b"NEW-FOLLOW-MARKER" not in data and time.monotonic() < until:
                data.extend(follower.recv(65536))
            check(b"NEW-FOLLOW-MARKER" in data, "follow delivers appended bytes")

            # Disable returns immediately; its manager joins follow + partial workers.
            pending = connect(port); pending.sendall(b"GET /status HTTP/1.1\r\n")
            _, _, elapsed = command("disable")
            check(elapsed < 100000, "disable must not join on the render thread")
            state(False)
            check(pending.recv(1) == b"", "disable cancels partial request")
            pending.close(); follower.close()
            command("enable"); port = state(True)
            check(b"200 OK" in request(port, "/status"), "restart after asynchronous disable")

            # Explicit shutdown cancels a blocked writer and every owned thread.
            slow = connect(port, 1024)
            slow.sendall(b"GET /log?tail=all HTTP/1.1\r\nHost: localhost\r\n\r\n")
            partial = connect(port); partial.sendall(b"G")
            time.sleep(0.1)
            active, _, elapsed = command("stop")
            check(not active and elapsed < 800000, "stop joins all clients within cancellation bound")
            check(partial.recv(1) == b"", "final stop cancels partial request")
            slow.close(); partial.close()
            for _ in range(3):
                command("enable"); port = state(True)
                check(b"200 OK" in request(port, "/status"), "clean explicit stop/start generation")
                command("stop"); state(False)
            process.stdin.write("quit\n"); process.stdin.flush()
            check(process.wait(timeout=3) == 0, "server driver clean exit")
            check(not process.stderr.read(), "ASan/UBSan must remain clean")
        finally:
            for client in clients:
                client.close()
            if process.poll() is None:
                process.kill(); process.wait()
    report = {"result": "PASS", "checks": checks, "sanitizers": ["address", "undefined"],
        "boundary": "Production server with real loopback TCP sockets; only dlog and diagnostics flush are stubbed.",
        "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (source, driver, Path(__file__).resolve())},
        "limits": "Host sockets; PS5 socket ABI and LAN reachability still require console verification."}
    output = ROOT / "build/tests/diag-server.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Diagnostics HTTP server: {checks} checks PASS (real TCP, ASan, UBSan)")
    print(output)


if __name__ == "__main__":
    main()
