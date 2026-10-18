#!/usr/bin/env python3
"""Real curl/FFmpeg transfers on a private local HTTP server; no provider access."""
import collections
import http.server
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time
import urllib.parse

root = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="stremio-download-transfer-") as folder:
    folder = Path(folder)
    media = folder / "media"
    media.mkdir()
    (media / "subtitle.srt").write_text("1\n00:00:00,000 --> 00:00:03,900\nSottotitolo di prova offline\n", encoding="utf-8")
    fixture = media / "fixture.mp4"
    subprocess.run([
        "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000", "-i", str(media / "subtitle.srt"),
        "-t", "4", "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s",
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "15", "-g", "25", "-sc_threshold", "0",
        "-c:a", "aac", "-c:s", "mov_text", "-metadata:s:a:0", "language=ita", "-metadata:s:a:1", "language=eng",
        "-metadata:s:s:0", "language=ita", "-movflags", "+faststart", str(fixture),
    ], check=True, timeout=30)
    subprocess.run([
        "ffmpeg", "-v", "error", "-y", "-i", str(fixture), "-map", "0:v", "-map", "0:a", "-c", "copy",
        "-f", "hls", "-hls_time", "1", "-hls_list_size", "0", "-hls_playlist_type", "vod",
        "-hls_segment_filename", str(media / "segment%02d.ts"), str(media / "index.m3u8"),
    ], check=True, timeout=20)
    payload = fixture.read_bytes()
    manifest = (media / "index.m3u8").read_text()
    counts = collections.Counter()
    seen = collections.defaultdict(list)
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def handle(self):
            # Cancelling an actual curl transfer closes its keep-alive socket.
            # That reset is the intended test stimulus, not a server failure.
            try:
                super().handle()
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            key = path.lstrip("/")
            with lock:
                counts[key] += 1
                attempt = counts[key]
                seen[key].append((self.headers.get("Range"), self.headers.get("If-Range")))
            if self.headers.get("X-Download-Token") != "TOKEN_DO_NOT_LOG":
                self.send_error(403)
                return
            if key == "hls-redirect.m3u8":
                self.send_response(302)
                self.send_header("Location", "/nested/index.m3u8")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if key == "hls-master.m3u8":
                body = b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=640x360\nhls-good.m3u8\n'
                mime = "application/vnd.apple.mpegurl"
            elif key.startswith("hls-") and (key.endswith(".m3u8") or key == "hls-alias") or key == "nested/index.m3u8":
                text = manifest
                if key == "hls-live.m3u8":
                    text = text.replace("#EXT-X-ENDLIST", "")
                if key == "hls-encrypted.m3u8":
                    text = text.replace("#EXTM3U", '#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI="https://private.invalid/key"')
                if key == "hls-cycle.m3u8":
                    text = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=4000000\nhls-cycle.m3u8\n"
                if key == "hls-local.m3u8":
                    text = text.replace("segment00.ts", "file:///etc/passwd")
                if key == "hls-missing.m3u8":
                    text = text.replace("segment01.ts", "missing-segment.ts")
                if key == "hls-slow.m3u8":
                    text = text.replace("segment", "slow-segment")
                body, mime = text.encode(), "application/vnd.apple.mpegurl"
            elif "segment" in key:
                name = Path(key).name.replace("slow-", "")
                source = media / name
                if not source.is_file():
                    self.send_error(404)
                    return
                body, mime = source.read_bytes(), "video/mp2t"
            elif key in ("html-error", "dash.mpd"):
                body = b"<html>private error TOKEN_DO_NOT_LOG</html>"
                mime = "text/html" if key == "html-error" else "application/dash+xml"
            elif key == "missing":
                self.send_error(404)
                return
            else:
                body, mime = payload, "video/mp4"
            if key in ("resume-ignored", "resume-changed"):
                body += (b"A" if attempt == 1 else b"B") * 64
            status = 200
            start = 0
            requested = self.headers.get("Range")
            total = len(body)
            if requested and key != "resume-ignored":
                start = int(requested.split("=")[1].split("-")[0])
                if start >= total or key == "resume-bad416":
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{total if key != 'resume-bad416' else total - 1}")
                    self.send_header("ETag", '"v1"')
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                status = 206
            self.send_response(status)
            self.send_header("Content-Type", mime)
            if key != "resume-novalidator":
                if key == "resume-lastmodified":
                    self.send_header("Last-Modified", "Mon, 05 Oct 2026 12:00:00 GMT")
                else:
                    self.send_header("ETag", '"v2"' if key in ("resume-ignored", "resume-changed") and attempt > 1 else '"v1"')
            if status == 206:
                returned_start = start + 1 if key == "resume-wrongstart" else start
                self.send_header("Content-Range", f"bytes {returned_start}-{total - 1}/{total}")
            length = total - start
            if key == "resume-badlength" and status == 206:
                length += 1
            if key == "unknown-length":
                self.send_header("Connection", "close")
                self.close_connection = True
            else:
                self.send_header("Content-Length", str(length))
            self.end_headers()
            content = body[start:]
            if key == "truncated" and attempt == 1:
                content = content[:len(content) // 2]
                self.close_connection = True
            slow = (key.startswith("resume") and attempt == 1) or "slow-segment" in key
            try:
                for offset in range(0, len(content), 8192):
                    self.wfile.write(content[offset:offset + 8192])
                    self.wfile.flush()
                    if slow:
                        time.sleep(.007)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    binary = folder / "test-download-transfer"
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil", "libcurl"], text=True))
    compiler = os.environ.get("HOST_CXX", "clang++-18")
    command = [compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-Wno-reorder-ctor",
               "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(root / "src"), "-I" + str(root / "third_party"),
               str(root / "src/download_transfer.cpp"), str(root / "src/netstream.cpp"), str(root / "src/http.cpp"), str(root / "src/util.cpp"),
               str(root / "tests/test_download_transfer.cpp"), "-Wl,--wrap=write", "-Wl,--wrap=fsync", "-o", str(binary), *flags]
    environment = dict(os.environ)
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    try:
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary), str(folder / "jobs"), f"http://127.0.0.1:{server.server_port}", str(fixture)], check=True, timeout=180, env=environment)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    # The original HTTP server is now stopped. Every container is read locally,
    # and ffmpeg decodes all copied audio/video packets without a network source.
    subprocess.run([str(binary), "--offline-probe", str(folder / "jobs")], check=True, timeout=30, env=environment)
    for job in ("direct", "hls-good.m3u8", "hls-master.m3u8"):
        subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(folder / "jobs" / job / "media.part"),
                        "-map", "0:v", "-map", "0:a", "-f", "null", "-"], check=True, timeout=30)
    assert any(rng and validator == '"v1"' for rng, validator in seen["resume"])
    assert any(rng and validator == "Mon, 05 Oct 2026 12:00:00 GMT" for rng, validator in seen["resume-lastmodified"])
    assert all(rng is None for rng, _ in seen["resume-novalidator"])
    assert len(seen["resume-changed"]) >= 3 and seen["resume-changed"][-1][0] is None
    print("HTTP request audit: safe Range/If-Range resume and restart verified")
    print("FFmpeg server-stopped full decode: 3 offline containers passed")
