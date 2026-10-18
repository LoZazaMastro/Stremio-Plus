#!/usr/bin/env python3
"""Verify real VLC resume and automatic subtitles across delayed HTTP startup."""
import argparse
import ctypes.util
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--baseline", action="store_true", help="Reproduce the prior resume/subtitle bug")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
main_object = "CMakeFiles/stremio.dir/src/main.cpp.o"
if main_object not in link:
    raise SystemExit("Build the host application with CMake Unix Makefiles first.")
flags = (build / "CMakeFiles/stremio.dir/flags.make").read_text()
compile_flags = []
for name in ("CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"):
    match = re.search(r"^" + name + r" = (.*)$", flags, re.MULTILINE)
    if match:
        compile_flags.extend(shlex.split(match[1]))
if not any("STREMIO_USE_LIBVLC" in flag for flag in compile_flags):
    raise SystemExit("This regression requires the production libVLC backend.")
source_include = next((flag[2:] for flag in compile_flags if flag.startswith("-I") and flag.endswith("/src")
                       and (Path(flag[2:]) / "player_vlc.cpp").is_file()), None)
if not source_include:
    raise SystemExit("Cannot identify the host build's source tree.")
compiled_root = Path(source_include).parent
output = root / "build/tests" / ("player-resume-baseline" if args.baseline else "player-resume")
output.mkdir(parents=True, exist_ok=True)
obj, binary = output / "test_player_resume.o", output / "test_player_resume"
subprocess.run([link[0], *compile_flags, "-O1", "-g", "-c", str(root / "tests/test_player_resume.cpp"),
                "-o", str(obj)], check=True)
link[link.index(main_object)] = str(obj)
link[link.index("-o") + 1] = str(binary)
for symbol in ("libvlc_media_player_set_media", "libvlc_media_player_set_time", "SDL_OpenAudioDevice"):
    link.append("-Wl,--wrap=" + symbol)
egl = ctypes.util.find_library("EGL")
if not egl:
    raise SystemExit("The GL resume check requires the host EGL runtime.")
link.append("-l:" + egl)
subprocess.run(link, cwd=build, check=True)

records, release_at = [], {}
lock = threading.Lock()
with tempfile.TemporaryDirectory(prefix="stremio-player-resume-") as directory:
    fixture = Path(directory)
    subtitles = "".join(f"{i + 1}\n00:00:{i:02d},500 --> 00:00:{i + 1:02d},300\nResume cue {i}\n\n" for i in range(29))
    (fixture / "resume.srt").write_text(subtitles)
    generation = subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "color=red:size=320x180:rate=24:duration=8",
        "-f", "lavfi", "-i", "color=blue:size=320x180:rate=24:duration=22",
        "-f", "lavfi", "-i", r"aevalsrc=if(lt(t\,8)\,0.1*sin(2*PI*440*t)\,0.1*sin(2*PI*880*t)):s=48000:d=30",
        "-i", str(fixture / "resume.srt"), "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]",
        "-map", "[v]", "-map", "2:a", "-map", "3:s", "-c:v", "libx264", "-preset", "ultrafast",
        "-crf", "23", "-g", "24", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k", "-c:s", "srt",
        "-metadata:s:a:0", "language=eng", "-metadata:s:s:0", "language=ita", str(fixture / "resume.mkv")],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    if generation.returncode:
        print(generation.stdout)
        raise SystemExit("Synthetic resume fixture generation failed.")
    media = (fixture / "resume.mkv").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *arguments):
            pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def serve(self, body):
            path = urlsplit(self.path).path
            authorized = (self.headers.get("X-Stremio-Test") == "resume-fixture" and
                          self.headers.get("Authorization") == "Bearer resume-fixture-credential")
            requested_range = self.headers.get("Range")
            with lock:
                records.append({"path": path, "method": self.command, "range": requested_range, "authorized": authorized})
                if body:
                    release = release_at.setdefault(path, time.monotonic() + 6.5)
            if not authorized or path not in ("/resume.mkv", "/cancel.mkv"):
                self.send_response(403); self.send_header("Content-Length", "0"); self.end_headers()
                return
            start, end, status = 0, len(media) - 1, 200
            if requested_range:
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested_range)
                if not match:
                    self.send_response(416); self.send_header("Content-Length", "0"); self.end_headers()
                    return
                start = int(match[1]); end = min(end, int(match[2]) if match[2] else end)
                if start > end:
                    self.send_response(416); self.send_header("Content-Length", "0")
                    self.send_header("Content-Range", f"bytes */{len(media)}"); self.end_headers()
                    return
                status = 206
            self.send_response(status)
            self.send_header("Content-Type", "video/x-matroska")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(media)}")
            self.end_headers()
            if body:
                time.sleep(max(0, release - time.monotonic()))
                try:
                    self.wfile.write(media[start:end + 1])
                except (BrokenPipeError, ConnectionResetError):
                    pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        environment = dict(os.environ, SDL_AUDIODRIVER="dummy", LIBGL_ALWAYS_SOFTWARE="1", EGL_PLATFORM="surfaceless",
            NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
        command = [str(binary), directory, str(compiled_root / "app"), f"http://127.0.0.1:{server.server_port}"]
        if args.baseline:
            command.append("baseline")
        result = subprocess.run(command, env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    finally:
        server.shutdown(); server.server_close(); worker.join()
    (output / "test-output.txt").write_text(result.stdout)
    diagnostics = output / "diagnostics"
    if diagnostics.exists():
        shutil.rmtree(diagnostics)
    if (fixture / "diagnostics").is_dir():
        shutil.copytree(fixture / "diagnostics", diagnostics)
    cases = [json.loads(line[len("RESUME_CASE "):]) for line in result.stdout.splitlines() if line.startswith("RESUME_CASE ")]
    marker = "PLAYER_RESUME_BUG_REPRODUCED" if args.baseline else "PLAYER_RESUME_TESTS_OK"
    errors = []
    if result.returncode or marker not in result.stdout:
        errors.append("The App/VLC resume checks did not complete.")
    if not records or any(not item["authorized"] for item in records):
        errors.append("An input request lost its configured HTTP headers.")
    if not any(item["path"] == "/resume.mkv" and item["range"] for item in records):
        errors.append("The delayed input did not exercise HTTP Range.")
    log_text = "".join(path.read_text(errors="replace") for path in diagnostics.rglob("*") if path.is_file())
    if "resume-fixture-credential" in log_text:
        errors.append("A configured input credential escaped into automatic logs.")
    files = [root / "tests/test_player_resume.cpp", Path(__file__).resolve(),
        *(compiled_root / "src" / name for name in ("app.h", "app.cpp", "app_watch.cpp", "player.h",
            "player_vlc.cpp", "player_vlc_audio.h", "video_gl.cpp", "video_gl.h"))]
    report = {"result": "FAIL" if errors else "BUG_REPRODUCED" if args.baseline else "PASS",
        "scope": "Real App, VLC demux/decode, HTTP Range delayed 6.5 seconds, automatic subtitles, SDL PCM and OpenGL upload",
        "instrumentation": "Calls to set_time and set_media are counted and forwarded unchanged; PCM and rendered colors are observed",
        "entry_point": "The test harness replaces main.cpp and executes App update, frame acquisition, GL upload and acknowledgement in that order",
        "cases": cases, "http_requests": records, "errors": errors,
        "fixture_sha256": hashlib.sha256(media).hexdigest(), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    for line in result.stdout.splitlines():
        if line.startswith(("RESUME_CASE ", "RESUME_TIMEOUT ", "PLAYER_RESUME_")):
            print(line)
    if errors:
        print(result.stdout[-6000:])
        raise SystemExit(" ".join(errors))
    print(f"PASS delayed loopback input: {len(records)} authorized requests; Range headers and redaction verified")
    print(output / "result.json")
