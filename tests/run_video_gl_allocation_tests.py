#!/usr/bin/env python3
"""Exercise real video GL allocation cleanup with bounded injected failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path)
parser.add_argument('--expect-failure', action='store_true')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
hui = root / 'vendor/ps5-homebrew-ui'
source = args.source or root / 'src/video_gl.cpp'
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2', 'egl', 'gl'], text=True))
wrapped = ['glGenTextures','glDeleteTextures','glBindFramebuffer','glFramebufferTexture2D',
           'glDeleteFramebuffers','glTexStorage2D','glTexImage2D','glGetError','glDrawArrays',
           'glCheckFramebufferStatus']
with tempfile.TemporaryDirectory(prefix='stremio-video-allocation-') as directory:
    binary = Path(directory) / 'test-video-gl-allocation'
    sources = [root/'tests/test_video_gl_allocation.cpp', source, root/'src/yuv_convert.cpp',
               root/'src/util.cpp', hui/'src/gfx/gl_program.cpp', hui/'host/platform_host.cpp']
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-pthread',
                    '-DGL_GLEXT_PROTOTYPES=1', '-I'+str(root/'src'), '-I'+str(root/'third_party'), '-I'+str(hui/'src'),
                    *map(str, sources), '-o', str(binary), *['-Wl,--wrap='+name for name in wrapped], *flags], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout, end='')
    if result.stderr: print(result.stderr, end='')
    match = re.search(r'GPU allocation tests: (\d+) checks, (\d+) failures', result.stdout)
    if not match: raise RuntimeError('GPU allocation test result is incomplete')
    checks, failures = map(int, match.groups())
    if args.expect_failure:
        if result.returncode != 1 or not failures: raise RuntimeError('Baseline did not reproduce a failing test')
    else:
        result.check_returncode()
    output = args.output or root/'build/tests/video-gl-allocation.json'
    report = {'result': 'REPRODUCED' if args.expect_failure else 'PASS', 'checks': checks, 'failures': failures,
              'scope': 'Real OpenGL output and texture lifetime; injected exhausted API allocations at every stage; fragmented native-size first-fit model; clear/reopen; unrelated preexisting GL errors',
              'limits': 'The failure injection models an exhausted GL allocation after any driver retry. The physical ledger is an independent first-fit model using exact retained PS5 driver footprints, not a hardware trace or a reconstruction of the user session.',
              'source_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                                (source, root/'src/video_gl.h', root/'tests/test_video_gl_allocation.cpp', Path(__file__))},
              'stdout': result.stdout, 'stderr': result.stderr}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2)+'\n')
