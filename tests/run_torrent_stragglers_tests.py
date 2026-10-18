#!/usr/bin/env python3
"""Download straggler rescue, real engine, ASan/UBSan."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--engine', type=Path, default=root/'src/torrent/engine.cpp')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
engine = args.engine.resolve()
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcurl', 'openssl'], text=True))
with tempfile.TemporaryDirectory(prefix='stremio-stragglers-') as temp:
    exe = Path(temp)/'policy'
    command = [os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-deprecated-declarations', '-Wno-unused-function', '-pthread', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I'+str(root/'src'), '-I'+str(root/'third_party'),
               '-DENGINE_IMPLEMENTATION="'+str(engine)+'"', str(root/'tests/test_torrent_stragglers.cpp'),
               str(root/'src/torrent/bencode.cpp'), str(root/'src/http.cpp'), str(root/'src/util.cpp'),
               '-Wl,--wrap=socket', '-Wl,--wrap=connect', '-Wl,--wrap=getaddrinfo', *flags, '-o', str(exe)]
    subprocess.run(command, check=True, timeout=180)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    result = subprocess.run([str(exe)], check=True, text=True, capture_output=True, timeout=30, env=env)
    lines = [line[7:] for line in result.stdout.splitlines() if line.startswith('RESULT ')]
    assert len(lines) == 1, result.stdout
    report = json.loads(lines[0])
    report.update(engineSha256=hashlib.sha256(engine.read_bytes()).hexdigest(), sanitizers=['ASan', 'UBSan'],
                  leakSanitizer=False, environment='offline host policy regression, no PS5 throughput claim')
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
