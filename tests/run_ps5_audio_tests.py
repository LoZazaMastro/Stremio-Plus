#!/usr/bin/env python3
"""Fault-inject native HDMI transport around the production output and real VLC objects."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    work = Path(os.environ.get('STREMIO_VLC_HOST_ROOT', ROOT / '.deps/vlc-host'))
    source = work / 'src/vlc-3.0.24'
    build = work / 'build-vlc-host'
    target = ROOT / 'build/tests/ps5-audio'
    target.mkdir(parents=True, exist_ok=True)
    binary = target / 'ps5-audio'
    includes = [ROOT / 'src', source / 'include', build, build / 'include', work / 'prefix-host/include']
    command = [os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-pthread',
               '-DHAVE_CONFIG_H', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
    command += ['-I' + str(path) for path in includes]
    command += [str(ROOT / 'tests/test_ps5_audio.cpp'), str(ROOT / 'src/player_iec61937.cpp'),
                '-o', str(binary), '@' + str(work / 'static-host/link.rsp'), '-ldl', '-pthread']
    subprocess.run(command, check=True, timeout=120)
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, timeout=35, env=environment)
    (target / 'test-output.txt').write_text(run.stdout)
    print(run.stdout, end='', flush=True)
    run.check_returncode()
    result = json.loads(run.stdout.splitlines()[-1])
    names = ('src/player_vlc_passthrough.cpp', 'src/player_iec61937.cpp', 'src/player_iec61937.h',
             'tests/test_ps5_audio.cpp', 'tests/run_ps5_audio_tests.py')
    report = dict(result, sanitizers=['AddressSanitizer', 'UndefinedBehaviorSanitizer'],
                  scope='Production native audio output with real host VLC objects and mocked Sony HDMI imports',
                  limits='Does not establish HDMI receiver compatibility or timing on a physical PS5.',
                  sourceSha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in names})
    (target / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(target / 'result.json')


if __name__ == '__main__':
    main()
