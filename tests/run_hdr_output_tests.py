#!/usr/bin/env python3
"""Real EGL HDR composition and native transition failure-injection checks."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
hui = root / 'vendor/ps5-homebrew-ui'
flags = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','sdl2','egl','gl'],text=True))
sources = [root/'tests/test_hdr_output.cpp', root/'src/video_gl.cpp', root/'src/yuv_convert.cpp',
           root/'src/util.cpp', hui/'host/platform_host.cpp', *sorted((hui/'src/gfx').glob('*.cpp'))]
with tempfile.TemporaryDirectory(prefix='stremio-hdr-output-') as directory:
    binary = Path(directory)/'test-hdr-output'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O2','-Wall','-Wextra','-pthread',
        '-DGL_GLEXT_PROTOTYPES=1','-I'+str(root/'src'),'-I'+str(root/'third_party'),'-I'+str(hui/'src'),
        '-I'+str(hui/'third_party'),*map(str,sources),'-o',str(binary),
        '-Wl,--wrap=glTexImage2D','-Wl,--wrap=glGetError',*flags],check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout,end='')
    if result.stderr: print(result.stderr,end='')
    result.check_returncode()
    match=re.search(r'HDR tests: (\d+) checks, 0 failures',result.stdout)
    if not match: raise RuntimeError('Incomplete HDR test result')
    audited=[*sources,root/'src/hdr_out.cpp',root/'src/hdr_out.h',root/'src/main.cpp',Path(__file__)]
    report={'result':'PASS','checks':int(match[1]),
        'scope':'Real EGL direct-plane PQ ten-bit scanout without RGB10_A2 intermediate, packed final output, linear-light UI coverage, small tone-mapped glass capture; preparation before scanout changes, retained paused-frame reconversion, injected HDR/UI allocation failures, native switch refusal, bounded restoration, captured handle ownership',
        'limits':'Sony calls are mocked. Physical PS5 graphics, HDMI signaling, sustained 4K cadence and television output require console testing.',
        'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in audited},
        'stdout':result.stdout,'stderr':result.stderr}
    target=root/'build/tests/hdr-output.json'
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_text(json.dumps(report,indent=2)+'\n')
