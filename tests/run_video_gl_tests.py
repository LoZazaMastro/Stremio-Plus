#!/usr/bin/env python3
"""Render real 8/10-bit video planes on host OpenGL and compare color output."""
import os
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
hui = root / 'vendor/ps5-homebrew-ui'
flags = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','sdl2','egl','gl'],text=True))
with tempfile.TemporaryDirectory(prefix='stremio-video-gl-') as directory:
    binary = Path(directory) / 'test-video-gl'
    sources = [root/'tests/test_video_gl.cpp',root/'src/video_gl.cpp',root/'src/yuv_convert.cpp',
               root/'src/util.cpp',hui/'src/gfx/gl_program.cpp',hui/'host/platform_host.cpp']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O2','-Wall','-Wextra','-pthread',
        '-DGL_GLEXT_PROTOTYPES=1','-I'+str(root/'src'),'-I'+str(root/'third_party'),'-I'+str(hui/'src'),
        *map(str,sources),'-o',str(binary),*flags],check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout, end='')
    if result.returncode: print(result.stderr)
    result.check_returncode()
    match = re.search(r'GPU tests: (\d+) checks, 0 failures', result.stdout)
    if not match: raise RuntimeError('GPU test result is incomplete')
    files = [root / name for name in ('src/video_gl.cpp', 'src/video_gl.h', 'src/video_evo_shaders.h',
                                     'src/video_dovi.h', 'tests/test_video_gl.cpp', 'tests/run_video_gl_tests.py')]
    report = {'result': 'PASS', 'checks': int(match[1]),
              'scope': 'Real host OpenGL shader compilation, texture rendering and readback: YUV8/10, PQ/HLG, source-resolution 1080p/4K output, bilinear texture filtering, distinct consecutive frames without blending, cadence/seek reset, invalid-picture retention and Dolby Vision parameters',
              'limits': 'Host OpenGL validates rendering behavior; physical PS5 GPU performance and HDMI output require console testing.',
              'source_sha256': {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    target = root / 'build/tests/video-gl.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + '\n')
