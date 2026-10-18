#!/usr/bin/env python3
"""Render real 8/10-bit video planes on host OpenGL and compare color output."""
import os
from pathlib import Path
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
    subprocess.run([str(binary)],check=True)
