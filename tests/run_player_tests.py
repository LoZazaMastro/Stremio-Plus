#!/usr/bin/env python3
"""Real Player tests with synthetic media and a protected loopback Range/HLS server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build-host'
link_file = build / 'CMakeFiles/stremio.dir/link.txt'
if not link_file.is_file():
    raise SystemExit('Build the host application with CMake Unix Makefiles first.')
link = shlex.split(link_file.read_text())
main_object = 'CMakeFiles/stremio.dir/src/main.cpp.o'
if main_object not in link:
    raise SystemExit('Host link command does not contain the expected main object.')
test_object = build / 'test_player.cpp.o'
binary = build / 'test_player'
flags = (build / 'CMakeFiles/stremio.dir/flags.make').read_text()
test_flags = []
for variable in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    match = re.search(r'^' + variable + r' = (.*)$', flags, re.MULTILINE)
    if match:
        test_flags.extend(shlex.split(match[1]))
uses_vlc = any('STREMIO_USE_LIBVLC' in flag for flag in test_flags)
subprocess.run([
    link[0], *test_flags, '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Wno-missing-field-initializers',
    '-c', str(root / 'tests/test_player.cpp'), '-o', str(test_object),
], check=True)
link[link.index(main_object)] = str(test_object)
link[link.index('-o') + 1] = str(binary)
subprocess.run(link, cwd=build, check=True)

records = []
record_lock = threading.Lock()

def ffmpeg(arguments):
    result = subprocess.run(['ffmpeg', '-nostdin', '-y', '-hide_banner', '-loglevel', 'error', *arguments],
                            timeout=90, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        print(result.stderr.decode(errors='replace')[-4000:])
        raise SystemExit('FFmpeg could not generate the local test fixtures.')

with tempfile.TemporaryDirectory(prefix='stremio-player-') as temporary:
    fixtures = Path(temporary)
    (fixtures / 'subtitle.srt').write_text(
        '1\n00:00:00,100 --> 00:00:03,000\nPrimo test: è già pronto.\n\n'
        '2\n00:00:03,000 --> 00:00:05,800\nSecondo sottotitolo.\n', encoding='utf-8')
    (fixtures / 'subtitle.ass').write_text(
        '[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n'
        '[V4+ Styles]\n'
        'Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, '
        'Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, '
        'Alignment, MarginL, MarginR, MarginV, Encoding\n'
        'Style: Default,Arial,28,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n'
        '[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n'
        'Dialogue: 0,0:00:00.10,0:00:05.80,Default,,0,0,0,,VLC ASS subtitle\n', encoding='utf-8')
    ffmpeg([
        '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=24:duration=6',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=6',
        '-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=48000:duration=6',
        '-i', str(fixtures / 'subtitle.srt'), '-i', str(fixtures / 'subtitle.ass'),
        '-map', '0:v', '-map', '1:a', '-map', '2:a', '-map', '3:s', '-map', '4:s',
        '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '23', '-g', '24', '-pix_fmt', 'yuv420p',
        '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-color_range', 'tv',
        '-c:a', 'aac', '-b:a', '96k', '-c:s:0', 'srt', '-c:s:1', 'copy',
        '-metadata:s:a:0', 'language=eng', '-metadata:s:a:1', 'language=ita',
        '-metadata:s:s:0', 'language=ita', '-metadata:s:s:1', 'language=eng',
        str(fixtures / 'multi.mkv'),
    ])
    ffmpeg([
        '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=24:duration=4',
        '-f', 'lavfi', '-i', 'color=c=black:size=640x360:rate=24:duration=4',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=8',
        '-filter_complex', '[0:v][1:v]concat=n=2:v=1:a=0[v]', '-map', '[v]', '-map', '2:a',
        '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '18', '-g', '96', '-pix_fmt', 'yuv420p',
        '-c:a', 'aac', '-b:a', '64k', '-movflags', '+faststart', str(fixtures / 'vbr.mp4'),
    ])
    ffmpeg([
        '-f', 'lavfi', '-i', 'color=c=0x5c80a0:size=3840x2160:rate=8:duration=3',
        '-vf', 'format=yuv420p10le', '-c:v', 'libx265', '-preset', 'ultrafast', '-profile:v', 'main10',
        '-x265-params', 'pools=1:frame-threads=1:log-level=error:keyint=8:repeat-headers=1',
        '-color_primaries', 'bt2020', '-color_trc', 'smpte2084', '-colorspace', 'bt2020nc', '-color_range', 'tv',
        '-an', str(fixtures / 'main10.mkv'),
    ])
    ffmpeg([
        '-i', str(fixtures / 'multi.mkv'), '-map', '0:v:0', '-map', '0:a:0', '-c', 'copy',
        '-hls_time', '1', '-hls_list_size', '0', '-hls_segment_filename', str(fixtures / 'segment-%02d.ts'),
        '-f', 'hls', str(fixtures / 'stream.m3u8'),
    ])
    ffmpeg([
        '-i', str(fixtures / 'multi.mkv'), '-map', '0:v:0', '-map', '0:a:0', '-c', 'copy',
        '-hls_time', '1', '-hls_list_size', '0', '-hls_flags', 'single_file',
        '-hls_segment_filename', str(fixtures / 'single.ts'), '-f', 'hls', str(fixtures / 'single.m3u8'),
    ])
    if '#EXT-X-BYTERANGE:' not in (fixtures / 'single.m3u8').read_text():
        raise SystemExit('FFmpeg did not generate the expected single-file byte-range playlist.')

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *arguments):
            pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                pass  # seeking/closing intentionally cancels the previous request

        def do_HEAD(self):
            self.serve(False)

        def do_GET(self):
            self.serve(True)

        def serve(self, body):
            path = urlsplit(self.path).path
            authorized = (self.headers.get('X-Stremio-Test') == 'fixture-only-value' and
                          self.headers.get('Authorization') == 'Bearer player-fixture-credential')
            range_value = self.headers.get('Range')
            with record_lock:
                records.append({'path': path, 'range': range_value, 'authorized': authorized})
            if not authorized:
                self.send_response(403); self.send_header('Content-Length', '0'); self.end_headers()
                return
            file = fixtures / path.lstrip('/')
            if file.parent != fixtures or not file.is_file():
                self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers()
                return
            data = file.read_bytes()
            start, end, status = 0, len(data) - 1, 200
            if range_value:
                match = re.fullmatch(r'bytes=(\d+)-(\d*)', range_value)
                if not match:
                    self.send_response(416); self.send_header('Content-Length', '0'); self.end_headers()
                    return
                start = int(match[1]); end = min(end, int(match[2]) if match[2] else end)
                if start > end:
                    self.send_response(416); self.send_header('Content-Length', '0')
                    self.send_header('Content-Range', f'bytes */{len(data)}'); self.end_headers()
                    return
                status = 206
            self.send_response(status)
            self.send_header('Content-Type', 'application/vnd.apple.mpegurl' if file.suffix == '.m3u8' else 'application/octet-stream')
            self.send_header('Content-Length', str(end - start + 1))
            self.send_header('Accept-Ranges', 'bytes')
            if status == 206:
                self.send_header('Content-Range', f'bytes {start}-{end}/{len(data)}')
            self.end_headers()
            if body:
                try:
                    self.wfile.write(data[start:end + 1])
                except (BrokenPipeError, ConnectionResetError):
                    pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        environment = dict(os.environ, SDL_AUDIODRIVER='dummy', NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
        result = subprocess.run([str(binary), str(fixtures), f'http://127.0.0.1:{server.server_port}', str(root / 'app')],
                                env=environment, text=True, capture_output=True, timeout=150)
        (build / 'player-test-output.txt').write_text(result.stdout + result.stderr)
        if result.returncode:
            failure_logs = build / 'player-test-failure-logs'
            if failure_logs.exists():
                shutil.rmtree(failure_logs)
            if (fixtures / 'diagnostics/logs').is_dir():
                shutil.copytree(fixtures / 'diagnostics/logs', failure_logs)
            print((result.stdout + result.stderr)[-9000:])
            print(f'Player test exited {result.returncode}; automatic logs preserved in {failure_logs}')
            raise SystemExit(result.returncode)
        if 'PLAYER_TESTS_OK' not in result.stdout:
            raise SystemExit('Player checks did not reach the completion marker.')
        if not records or any(not record['authorized'] for record in records):
            raise SystemExit('A stream request lost its configured headers.')
        if not any(record['path'] == '/multi.mkv' and record['range'] for record in records):
            raise SystemExit('The real HTTP input did not exercise Range requests.')
        if not any(record['path'] == '/stream.m3u8' for record in records) or not any(record['path'].endswith('.ts') for record in records):
            raise SystemExit('HLS playlist and segment transport were not both exercised.')
        if not any(record['path'] == '/single.m3u8' for record in records) or not any(record['path'] == '/single.ts' for record in records):
            raise SystemExit('Single-file HLS byte ranges were not exercised.')
        log_text = ''.join(path.read_text(errors='replace') for path in (fixtures / 'diagnostics/logs').glob('*') if path.is_file())
        if 'player-fixture-credential' in log_text:
            raise SystemExit('A configured stream credential escaped into automatic logs.')
        for line in result.stdout.splitlines():
            if line.startswith(('PASS ', 'PLAYER_TESTS_OK')):
                print(line)
        print(f'PASS protected loopback server: {len(records)} requests, Range and HLS header propagation, logs redacted')
        report = {
            'result': 'PASS', 'engine': 'libVLC 3.0.24' if uses_vlc else 'FFmpeg reference',
            'scope': 'Real Player with local generated media, growing local files and authenticated loopback HTTP/HLS',
            'checks': [line for line in result.stdout.splitlines() if line.startswith(('PASS ', 'PLAYER_TESTS_OK'))],
            'http_requests': records,
            'sha256': {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in [root / 'tests/test_player.cpp', Path(__file__).resolve(),
                                    *sorted((root / 'src').glob('player*'))] if path.is_file()},
        }
        report_path = root / 'build/tests' / ('vlc-player.json' if uses_vlc else 'player-reference.json')
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        print(report_path)
    finally:
        server.shutdown(); server.server_close(); worker.join()
