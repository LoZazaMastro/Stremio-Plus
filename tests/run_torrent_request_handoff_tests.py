#!/usr/bin/env python3
"""Sanitized ownership, explicit CANCEL and bounded newcomer-probe regressions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path)
    parser.add_argument('--output', type=Path, default=root / 'build/tests/torrent-request-handoff.json')
    args = parser.parse_args()
    engine = (args.engine or root / 'src/torrent/engine.cpp').resolve()
    test = root / 'tests/test_torrent_request_handoff.cpp'
    source = engine.read_text()
    inputs = [engine, test, root / 'tests/test_torrent_network.cpp', Path(__file__).resolve(),
              root / 'src/torrent/bencode.cpp', root / 'src/torrent/bencode.h',
              root / 'src/http.cpp', root / 'src/http.h', root / 'src/util.cpp', root / 'src/util.h']
    if (engine.parent / 'engine.h').is_file():
        inputs.append(engine.parent / 'engine.h')
    hashes = lambda: {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    before = hashes()
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcurl', 'openssl'], text=True))
    with tempfile.TemporaryDirectory(prefix='stremio-handoff-') as temporary:
        binary = Path(temporary) / 'handoff'
        cmd = [os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++17', '-O1', '-g',
               '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-Wno-unused-function',
               '-pthread', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-DBT_ENGINE_SOURCE="' + str(engine) + '"', '-I' + str(engine.parent.parent),
               '-I' + str(root / 'src'), '-I' + str(root / 'third_party'), str(test),
               str(root / 'src/torrent/bencode.cpp'), str(root / 'src/http.cpp'), str(root / 'src/util.cpp'),
               '-Wl,--wrap=connect', '-Wl,--wrap=getaddrinfo', '-Wl,--wrap=fwrite', '-Wl,--wrap=pwrite',
               *flags, '-o', str(binary)]
        for symbol, marker in [('BT_ASYNC_DISK_WORKER', 'start_disk_worker_locked'),
                               ('BT_PEER_TURNOVER', 'void turnover_peer('),
                               ('BT_TIMEOUT_CANCEL', '++t->timeout_cancels;'),
                               ('BT_HEAD_HANDOFF', 'uint64_t request_head_handoffs ='),
                               ('BT_HANDOFF_BOOTSTRAP', 'unsigned handoff_probe_requests')]:
            if marker in source:
                cmd.insert(1, '-D' + symbol + '=1')
        subprocess.run(cmd, check=True, timeout=180)
        run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=30, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        print(run.stdout, end='', flush=True)
        run.check_returncode()
    assert hashes() == before, 'source changed during regression'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'result': 'PASS', 'asan': True, 'ubsan': True,
                                      'scope': 'Production request picker and real protocol messages; no external swarm',
                                      'output': run.stdout, 'sha256': before}, indent=2) + '\n')


if __name__ == '__main__':
    main()
