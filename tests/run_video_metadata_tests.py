#!/usr/bin/env python3
"""Check DV metadata bounds and compare native bitstream packing with FFmpeg."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
prefix = root / '.deps/vlc-host/prefix-host'
sources = [root / path for path in ('tests/test_video_metadata.cpp', 'src/video_dovi.cpp', 'src/player_iec61937.cpp')]
with tempfile.TemporaryDirectory(prefix='stremio-metadata-') as directory:
    folder = Path(directory)
    for name, encoder in (('ac3', 'ac3'), ('eac3', 'eac3'), ('dts', 'dca')):
        target = folder / (name + '.bin')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'sine=frequency=440:sample_rate=48000:duration=0.256', '-ac', '2',
                        '-strict', '-2', '-c:a', encoder, '-b:a', '384k', '-f', name, str(target)], check=True)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', name, '-i', str(target), '-c', 'copy',
                        '-f', 'spdif', str(folder / (name + '.spdif'))], check=True)
    binary = folder / 'metadata'
    subprocess.run([os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root / 'src'),
                    '-I' + str(prefix / 'include'), *map(str, sources), str(prefix / 'lib/libavutil.a'),
                    '-lm', '-pthread', '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), str(folder)], text=True, capture_output=True,
                            env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'), timeout=40)
    print(result.stdout, end='')
    if result.returncode: print(result.stderr)
    result.check_returncode()
    report = json.loads(result.stdout)
    report.update(scope='Public FFmpeg Dolby Vision frame metadata; IEC61937 bytes independently compared with FFmpeg muxer',
                  sanitizers=['address', 'undefined'],
                  limits='The receiver, HDMI routing and PS5 audio port are tested separately; host packing is not a hardware audio test.',
                  source_sha256={str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources})
    target = root / 'build/tests/video-metadata.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + '\n')
