#!/usr/bin/env python3
"""Production Sys MAT lifecycle with explicitly mocked VLC/Sony boundaries."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(root / 'tests/run_truehd_tests.py')], check=True)
folder = root / 'build/tests/truehd'
binary = folder / 'truehd-transport-test'
inputs = ['src/player_vlc_passthrough.cpp', 'src/player_iec61937.cpp',
          'src/player_iec61937.h', 'tests/test_truehd_transport.cpp', 'tests/audio_mock_vlc/mock_vlc.h']
subprocess.run([os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-pthread',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(root / 'tests/audio_mock_vlc'), '-I' + str(root / 'src'),
                str(root / 'tests/test_truehd_transport.cpp'), str(root / 'src/player_iec61937.cpp'),
                '-o', str(binary)], check=True)
run = subprocess.run([str(binary), str(folder / 'audio.thd'), str(folder / 'audio.spdif'),
                      str(folder / 'resumed.thd'), str(folder / 'resumed.spdif')],
                     text=True, capture_output=True, timeout=35,
                     env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))
print(run.stdout, end='')
if run.returncode: print(run.stderr)
run.check_returncode()
report = json.loads(run.stdout)
report.update(scope='Complete production passthrough module with mocked VLC declarations/callbacks and Sony HDMI APIs',
              limits='Does not validate real libVLC object negotiation, actual PS5 blocking cadence, HDMI receiver compatibility or Atmos.',
              sourceSha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs})
(folder / 'transport-result.json').write_text(json.dumps(report, indent=2) + '\n')
