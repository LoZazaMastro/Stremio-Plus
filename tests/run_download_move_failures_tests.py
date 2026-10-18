#!/usr/bin/env python3
"""Fault-inject real download relocation across distinct host filesystems."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
args = parser.parse_args()
sources = [root / name for name in ('src/download_manager.cpp', 'src/download_manager.h',
    'tests/test_download_move_failures.cpp', 'tests/run_download_move_failures_tests.py')]
fingerprint = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
with tempfile.TemporaryDirectory(prefix='stremio-move-faults-') as work, \
     tempfile.TemporaryDirectory(prefix='stremio-move-external-', dir='/dev/shm') as external:
    binary = Path(work) / 'test-move-failures'
    command = [os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g',
        '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-Wl,--wrap=write,--wrap=unlink,--wrap=rename,--wrap=rmdir',
        '-I' + str(root / 'src'), '-I' + str(root / 'third_party'),
        str(root / 'src/download_manager.cpp'), str(root / 'tests/test_download_move_failures.cpp'), '-o', str(binary)]
    subprocess.run(command, check=True)
    environment = os.environ.copy(); environment.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    result = subprocess.run([str(binary), work + '/data', external], text=True, capture_output=True,
                            env=environment, timeout=120)
    print(result.stdout, end=''); print(result.stderr, end='')
    unchanged = fingerprint == {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    if not unchanged:
        raise RuntimeError('Relocation production/test sources changed during this run; rerun after edits settle')
    report = {'result': 'PASS' if result.returncode == 0 else 'FAIL', 'source_sha256': fingerprint,
        'sanitizers': ['AddressSanitizer', 'UndefinedBehaviorSanitizer'], 'stdout': result.stdout,
        'stderr': result.stderr, 'scope': 'Real same/cross-filesystem relocation; injected storage failure and native helper-equivalent directory lock; no console claim.'}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    result.check_returncode()
