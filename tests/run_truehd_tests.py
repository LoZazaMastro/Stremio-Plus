#!/usr/bin/env python3
"""Independent FFmpeg TrueHD encoder/muxer comparison; no console access."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
folder = root / 'build/tests/truehd'
folder.mkdir(parents=True, exist_ok=True)
audio = folder / 'audio.thd'
reference = folder / 'audio.spdif'
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                'sine=frequency=997:sample_rate=48000:duration=1.401', '-ac', '2',
                '-c:a', 'truehd', '-strict', '-2', '-f', 'truehd', str(audio)], check=True)
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'truehd', '-i', str(audio),
                '-c', 'copy', '-f', 'spdif', str(reference)], check=True)
encoded = audio.read_bytes()
first_size = ((encoded[0] & 15) << 8 | encoded[1]) * 2
resumed = folder / 'resumed.thd'
resumed.write_bytes(encoded[first_size:])
resumed_reference = folder / 'resumed.spdif'
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'truehd', '-i', str(resumed),
                '-c', 'copy', '-f', 'spdif', str(resumed_reference)], check=True)
binary = folder / 'truehd-test'
inputs = ['src/player_iec61937.cpp', 'src/player_iec61937.h', 'tests/test_truehd.cpp']
subprocess.run([os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I' + str(root / 'src'),
                str(root / 'tests/test_truehd.cpp'), str(root / 'src/player_iec61937.cpp'),
                '-o', str(binary)], check=True)
run = subprocess.run([str(binary), str(audio), str(reference), str(resumed), str(resumed_reference)], text=True, capture_output=True,
                     env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'), timeout=45)
print(run.stdout, end='')
if run.returncode: print(run.stderr)
run.check_returncode()
report = json.loads(run.stdout)
report.update(scope='Production TrueHD MAT framing compared byte for byte to independent FFmpeg spdif muxer',
              limits='Host framing only; physical PS5, receiver, Atmos signalling and native port timing require console tests.',
              ffmpeg=subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
              sourceSha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs})
(folder / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
