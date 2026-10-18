#!/usr/bin/env python3
"""Compile the real C++ backend and test it with offline JSON + loopback HTTP.

No provider account, Internet access, media downloads or PS5 SDK are required.
Host prerequisites: Python 3, a C++17 compiler and libcurl development files.
"""
import argparse
import gzip
import http.server
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading

RANGE_DATA = bytes((i * 17 + 3) % 256 for i in range(196608))


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Expected when the bounded C++ client aborts oversized fixtures.
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, body, status=200, headers=None):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/redirect":
            self.reply(b"redirect-body-must-not-be-parsed", 302, {"Location": "/ok", "ETag": "discard"})
        elif path == "/ok":
            self.reply(b'{"metas":[]}', headers={"ETag": '"fixture"', "Cache-Control": "max-age=60"})
        elif path == "/gzip":
            self.reply(gzip.compress(b'{"metas":[]}'), headers={"Content-Encoding": "gzip"})
        elif path == "/headers":
            valid = self.headers.get("User-Agent") == "Fixture Player" and self.headers.get("Referer") == "https://media.example/"
            self.reply(b'{"ok":true}' if valid else b'{"ok":false}', 200 if valid else 400)
        elif path == "/oversize":
            self.reply(b"x" * 4096)
        elif path == "/gzip-large":
            self.reply(gzip.compress(b"x" * 4096), headers={"Content-Encoding": "gzip"})
        elif path == "/chunked-large":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for _ in range(8):
                self.wfile.write(b"200\r\n" + b"x" * 512 + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/unavailable":
            self.reply(b"{}", 503, {"Retry-After": "30"})
        elif path == "/non-http-redirect":
            self.reply(b"", 302, {"Location": "file:///unused-protocol-fixture"})
        elif path == "/provider-error":
            self.reply(b'{"error":{"code":42,"message":"private-token-must-not-be-logged"}}')
        elif path.startswith("/range-"):
            if self.headers.get("Authorization") != "Bearer fixture":
                self.reply(b"{}", 401)
                return
            if self.headers.get("Accept-Encoding") != "identity":
                self.reply(b"{}", 400)
                return
            ranges = {"bytes=0-65535": (0, 65535), "bytes=131072-196607": (131072, 196607)}
            selected = ranges.get(self.headers.get("Range"))
            if selected is None:
                self.reply(b"{}", 416)
                return
            begin, end = selected
            if begin and self.headers.get("If-Range") != '"range-one"':
                self.reply(b"{}", 412)
                return
            if path == "/range-ignored":
                self.reply(RANGE_DATA)
                return
            if path == "/range-tail-fails" and begin:
                self.reply(b"{}", 503)
                return
            content_range = f"bytes {begin}-{end}/{len(RANGE_DATA)}"
            if path == "/range-wrong":
                content_range = f"bytes 1-65536/{len(RANGE_DATA)}"
            etag = '"range-two"' if path == "/range-changed" and begin else '"range-one"'
            headers = {"Content-Range": content_range, "ETag": etag}
            body = RANGE_DATA[begin : end + 1]
            if path == "/range-gzip":
                body = gzip.compress(body)
                headers["Content-Encoding"] = "gzip"
            self.reply(body, 206, headers)
        else:
            self.reply(b"{}", 404)

    def do_POST(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        parsed = json.loads(data)
        if self.path == "/post" and parsed.get("request") == 7 and self.headers.get("Content-Type") == "application/json":
            self.reply(b'{"received":7}')
        else:
            self.reply(b"{}", 400)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-http", action="store_true", help="Run JSON/routing tests without opening a loopback socket")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    curl_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl"], text=True))
    with tempfile.TemporaryDirectory(prefix="stremio-addon-tests-") as temp:
        binary = Path(temp) / "test-addon-protocol"
        command = [os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
                   "-I" + str(project / "src"), "-I" + str(project / "third_party"),
                   str(project / "tests/test_addon_protocol.cpp"), str(project / "src/stremio.cpp"),
                   str(project / "src/http.cpp"), str(project / "src/util.cpp"), "-o", str(binary), *curl_flags]
        subprocess.run(command, check=True)
        if args.no_http:
            subprocess.run([str(binary)], check=True)
            return
        server = FixtureServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([str(binary), f"http://127.0.0.1:{server.server_port}"], check=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
