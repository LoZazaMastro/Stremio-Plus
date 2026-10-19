#!/usr/bin/env python3
"""Verify full-resolution AV1 Main10 output using the production static player."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build/host-vlc'
link = shlex.split((build / 'CMakeFiles/stremio.dir/link.txt').read_text())
flags_text = (build / 'CMakeFiles/stremio.dir/flags.make').read_text()
flags = []
for key in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    match = re.search('^' + key + r' = (.*)$', flags_text, re.MULTILINE)
    if match: flags += shlex.split(match[1])
if not any('STREMIO_USE_LIBVLC' in flag for flag in flags): raise SystemExit('Requires a static libVLC host build')
report_dir = root / 'build/tests/player-av1'; report_dir.mkdir(parents=True, exist_ok=True)
obj = report_dir / 'test.o'; binary = report_dir / 'test'
subprocess.run([link[0], *flags, '-O1', '-c', str(root / 'tests/test_player_av1.cpp'), '-o', str(obj)], check=True)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
link[link.index('-o') + 1] = str(binary)
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix='stremio-av1-') as directory:
    fixture = Path(directory) / 'av1-4k10.mkv'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                    'color=c=0x5c80a0:size=3840x2160:rate=8:duration=1', '-vf', 'format=yuv420p10le',
                    '-c:v', 'libaom-av1', '-cpu-used', '8', '-row-mt', '1', '-threads', '4',
                    '-b:v', '0', '-crf', '40', '-an', str(fixture)], check=True, timeout=80)
    result = subprocess.run([str(binary), str(fixture), str(root / 'app')], text=True, capture_output=True,
                            timeout=20, env=dict(os.environ, SDL_AUDIODRIVER='dummy'))
    (report_dir / 'output.txt').write_text(result.stdout + result.stderr)
    print(result.stdout[-2500:])
    if result.returncode: print(result.stderr[-2500:])
    result.check_returncode()
    line = next(line for line in result.stdout.splitlines() if line.startswith('AV1_RESULT '))
    report = json.loads(line[len('AV1_RESULT '):])
    report['fixture_sha256'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    report['scope'] = 'Actual AV1 Main10 decoded by the production static libVLC/dav1d software path; full source resolution'
    report['limits'] = 'Short synthetic host smoke test; no PS5 AV1 hardware support or realtime 4K performance claim'
    report['source_sha256'] = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in (
        root / 'src/player_vlc.cpp', root / 'tests/test_player_av1.cpp', Path(__file__).resolve())}
    (report_dir / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
