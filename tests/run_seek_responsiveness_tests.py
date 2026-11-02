#!/usr/bin/env python3
"""Production VLC seek handoff/dispatch with controlled lifecycle boundaries."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/player_vlc.cpp').read_text()
start = source.index('    bool reopen_for_seek(')
at = source.index('{', start)
depth = 1
end = at + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
folder = root / 'build/tests/seek-responsiveness'
folder.mkdir(parents=True, exist_ok=True)
(folder / 'seek_reopen_body.inc').write_text(source[start:end] + '\n')
first = source.index('                    const auto seek = seek_requests.take_ready(')
last = source.index('                    if (seeking && native_position_known)', first)
(folder / 'seek_dispatch_body.inc').write_text(source[first:last])
binary = folder / 'test'
subprocess.run([os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-pthread',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(root / 'src'), '-I' + str(folder),
                str(root / 'tests/test_seek_responsiveness.cpp'), '-o', str(binary)], check=True)
run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10,
                     env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))
print(run.stdout, end='')
if run.returncode: print(run.stderr)
run.check_returncode()
report = json.loads(run.stdout)
report.update(scope='Production request handoff and extracted production VLC seek dispatch/reopen bodies; native stop/start mocked',
              limits='Does not establish real libVLC demux/decoder timing or physical HDMI track restoration. Actual torrent-read cancellation is tested separately.',
              sourceSha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in
                            ('src/player_vlc.cpp', 'src/player_seek.h', 'tests/test_seek_responsiveness.cpp')})
(folder / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
