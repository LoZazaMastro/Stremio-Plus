#!/usr/bin/env python3
"""Run production libVLC through a four-second mid-GOP HEVC input starvation."""
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
flags = []
flag_text = (build / 'CMakeFiles/stremio.dir/flags.make').read_text()
for name in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    match = re.search('^' + name + r' = (.*)$', flag_text, re.MULTILINE)
    if match: flags += shlex.split(match[1])
if not any('STREMIO_USE_LIBVLC' in flag for flag in flags): raise SystemExit('Requires a libVLC host build')
report_dir = root / 'build/tests/player-rebuffer'
report_dir.mkdir(parents=True, exist_ok=True)
obj = report_dir / 'test.o'; binary = report_dir / 'test'
subprocess.run([link[0], *flags, '-O1', '-g', '-c', str(root / 'tests/test_player_rebuffer.cpp'), '-o', str(obj)], check=True)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
link[link.index('-o') + 1] = str(binary)
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix='stremio-rebuffer-') as directory:
    folder = Path(directory); video = folder / 'long-gop.mkv'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                    'testsrc2=size=640x360:rate=24:duration=12', '-vf', 'format=yuv420p10le',
                    '-c:v', 'libx265', '-preset', 'ultrafast', '-profile:v', 'main10', '-x265-params',
                    'pools=2:frame-threads=2:log-level=error:keyint=300:min-keyint=300:scenecut=0:bframes=8:ref=4:rc-lookahead=12',
                    '-an', '-reserve_index_space', '4096', '-cues_to_front', '1', '-cluster_time_limit', '1000', str(video)], check=True, capture_output=True, timeout=80)
    probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_packets',
                       '-show_entries', 'packet=pts_time,pos,size,flags', '-of', 'json', str(video)], text=True))
    packets = probe['packets']
    keys = [p for p in packets if 'K' in p.get('flags', '')]
    if len(keys) != 1: raise RuntimeError('Fixture must have only its initial random-access picture')
    cutoff = next(p for p in packets if float(p.get('pts_time', '0')) >= 4.0)
    prefix = int(cutoff['pos']) + int(cutoff['size'])
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(video), '-pix_fmt', 'yuv420p10le',
                                   '-f', 'rawvideo', '-'], timeout=40)
    size = 640 * 360 * 3
    hashes = []
    for offset in range(0, len(raw), size):
        value = 1469598103934665603
        for byte in raw[offset:offset + size:1223]: value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        hashes.append(value)
    (folder / 'hashes.txt').write_text(''.join(str(value) + '\n' for value in hashes))
    result = subprocess.run([str(binary), str(folder), str(root / 'app'), str(prefix), str(folder / 'hashes.txt')],
        capture_output=True, text=True, timeout=45, env=dict(os.environ, SDL_AUDIODRIVER='dummy'))
    text = result.stdout + result.stderr
    logs = ''.join(path.read_text(errors='replace') for path in (folder / 'logs').glob('*.txt'))
    (report_dir / 'output.txt').write_text(text + '\n' + logs)
    print(text[-5000:])
    result.check_returncode()
    line = next(line for line in result.stdout.splitlines() if line.startswith('REBUFFER_RESULT '))
    report = json.loads(line[len('REBUFFER_RESULT '):])
    report['pcr_rebuffer_observed'] = 'pts_delay increased' in logs or 'pts_delay increased' in text
    report['missing_reference_errors'] = (logs + text).count('Could not find ref with POC')
    report['fixture_sha256'] = hashlib.sha256(video.read_bytes()).hexdigest()
    report['scope'] = 'Real Player and static VLC/FFmpeg HEVC; one initial keyframe, four-second missing live input, independently decoded picture hashes'
    report['limits'] = 'Host playback with software decode; no PS5 GPU or physical network throughput measurement'
    report['source_sha256'] = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in (
        root / 'src/player_vlc.cpp', root / 'vendor/vlc-ps5/patches/0021-reference-safe-rebuffering.patch',
        root / 'tests/test_player_rebuffer.cpp', Path(__file__).resolve())}
    if report['missing_reference_errors']: raise RuntimeError('Decoder lost its HEVC references during buffering')
    (report_dir / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
