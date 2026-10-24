#!/usr/bin/env python3
"""Exercise production startup readiness, selection and carousel draw commands.

Uses the existing coherent host App build. No network, GPU context, account,
decoder or PS5 API is required; texture handles model already uploaded artwork.
"""
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build/desktop'
link = shlex.split((build / 'CMakeFiles/stremio.dir/link.txt').read_text())
make_flags = (build / 'CMakeFiles/stremio.dir/flags.make').read_text()
flags = []
for key in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    found = re.search(r'^' + key + r' = (.*)$', make_flags, re.MULTILINE)
    if found:
        flags.extend(shlex.split(found[1]))
backend = 'vlc' if any('STREMIO_USE_LIBVLC' in flag for flag in flags) else 'ffmpeg'
output = root / 'build/tests' / ('home-startup-' + backend)
output.mkdir(parents=True, exist_ok=True)
obj, binary = output / 'test.o', output / 'test'
subprocess.run([link[0], *flags, '-O1', '-g', '-c', str(root / 'tests/test_home_startup.cpp'),
                '-o', str(obj)], check=True, timeout=90)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
link[link.index('-o') + 1] = str(binary)
subprocess.run(link, cwd=build, check=True, timeout=90)
result = subprocess.run([str(binary), str(root / 'app')], text=True,
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
(output / 'output.txt').write_text(result.stdout)
print(result.stdout, end='')
result.check_returncode()
checks = json.loads(result.stdout.splitlines()[-1])
files = ['src/home_startup.h', 'src/app.h', 'src/app.cpp', 'src/app_board.cpp',
         'src/home_ui.cpp', 'src/main.cpp', 'tests/test_home_startup.cpp',
         'tests/run_home_startup_tests.py', 'vendor/ps5-homebrew-ui/src/ui/components/carousel.cpp',
         'vendor/ps5-homebrew-ui/src/ui/components/carousel.hpp']
report = {'result': 'PASS', 'backend': backend,
          'scope': 'Production App startup and selection; actual HomeUi draw geometry and opacity with fixture texture handles',
          'checks': checks, 'sha256': {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in files}}
(output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(output / 'result.json')
