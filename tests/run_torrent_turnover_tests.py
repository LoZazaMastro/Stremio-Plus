#!/usr/bin/env python3
"""Exercise production peer turnover with optional frozen-source comparison.

Connections and link rates in the discovery model are simulated. Request
reservations, payload credit, SHA-1 rejection and async completion are real.
No external or loopback network operation is permitted by this suite.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument('--current', type=Path, default=root / 'src/torrent/engine.cpp')
    parser.add_argument('--baseline', type=Path,
                        help='Optional frozen engine.cpp from before turnover; never an old binary')
    parser.add_argument('--output', type=Path, default=root / 'build/tests/torrent-turnover.json')
    args = parser.parse_args()
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcurl', 'openssl'], text=True))
    inputs = [root / 'tests/test_torrent_turnover.cpp', Path(__file__).resolve(), args.current.resolve(),
              root / 'src/torrent/engine.h', root / 'src/torrent/bencode.cpp', root / 'src/torrent/bencode.h',
              root / 'src/http.cpp', root / 'src/http.h', root / 'src/util.cpp', root / 'src/util.h']
    if args.baseline:
        inputs.append(args.baseline.resolve())
    # Quoted includes can resolve beside an isolated/frozen implementation.
    # Bind those headers too, so a comparison cannot silently change its inputs.
    for engine in [args.current, args.baseline]:
        if engine is not None:
            inputs.extend(path.resolve() for path in (
                engine.parent / 'engine.h', engine.parent / 'bencode.h',
                engine.parent.parent / 'http.h', engine.parent.parent / 'util.h') if path.is_file())
    inputs = list(dict.fromkeys(path.resolve() for path in inputs))
    before = {str(path): digest(path) for path in inputs}
    report = {
        'result': 'PASS', 'scope': 'Deterministic peer policy; no PS5 or Internet speed measurement',
        'scenario': '80 slow peers connected, 8 fast seeds queued; connection completion and rates simulated',
        'sanitizers': ['AddressSanitizer', 'UndefinedBehaviorSanitizer'],
        'leak_detection': 'disabled: execution environment ptrace constraint',
    }
    configurations = [('baseline', args.baseline.resolve())] if args.baseline else []
    configurations.append(('current', args.current.resolve()))
    with tempfile.TemporaryDirectory(prefix='stremio-torrent-turnover-') as temporary:
        for label, engine in configurations:
            executable = Path(temporary) / ('test-turnover-' + label)
            command = [os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-pthread',
                       '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-Wno-unused-function',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I' + str(root / 'src'), '-I' + str(root / 'third_party'),
                       '-DENGINE_IMPLEMENTATION="' + str(engine) + '"',
                       str(root / 'tests/test_torrent_turnover.cpp'), str(root / 'src/torrent/bencode.cpp'),
                       str(root / 'src/http.cpp'), str(root / 'src/util.cpp'),
                       '-Wl,--wrap=socket', '-Wl,--wrap=connect', '-Wl,--wrap=getaddrinfo',
                       *flags, '-o', str(executable)]
            if label == 'current':
                command.append('-DTURNOVER_NEW=1')
            async_worker = 'void start_disk_worker_locked()' in engine.read_text()
            if async_worker:
                command.append('-DBT_ASYNC_DISK_WORKER=1')
            subprocess.run(command, check=True, timeout=180)
            environment = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120, env=environment)
            if result.returncode:
                print(result.stdout, end='', flush=True)
                print(result.stderr, end='', flush=True)
                result.check_returncode()
            lines = [line[7:] for line in result.stdout.splitlines() if line.startswith('RESULT ')]
            if len(lines) != 1:
                raise RuntimeError('Expected one result from ' + label)
            report[label] = json.loads(lines[0])
            report[label]['engine_sha256'] = digest(engine)
            report[label]['async_disk_worker'] = async_worker
            print(label + ': ' + lines[0], flush=True)
    after = {str(path): digest(path) for path in inputs}
    if before != after:
        raise RuntimeError('A test input changed during validation; rerun against frozen sources')
    report['source_sha256'] = before
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(str(args.output))


if __name__ == '__main__':
    main()
