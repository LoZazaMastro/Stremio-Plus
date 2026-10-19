#!/usr/bin/env python3
"""Verify fatal native hooks on the host without a console or network access."""
from pathlib import Path
import hashlib
import json
import os
import re
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/tests/native-diagnostics'
BUILD.mkdir(parents=True, exist_ok=True)
VERSION = re.search(r'project\([^)]*VERSION\s+([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text(), re.S).group(1)
CXX = os.environ.get('HOST_CXX', 'clang++-18')
CC = os.environ.get('HOST_CC', 'clang-18')
shims = BUILD / 'opengl-shims.o'
binary = BUILD / 'native-diagnostics-test'
subprocess.run([CC, '-std=c11', '-O2', '-g', '-ffunction-sections', '-fdata-sections',
                '-fno-builtin-abort', '-c', str(ROOT / 'native/opengl_shims.c'), '-o', str(shims)], check=True)
wraps = ['abort', '_Exit', 'malloc', 'calloc', 'realloc', 'free', 'pthread_mutex_lock', 'pthread_mutex_trylock',
         'fprintf', 'vfprintf', 'fflush', 'snprintf', 'vsnprintf']
command = [CXX, '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
           '-fno-builtin-abort', '-fno-optimize-sibling-calls',
           '-ffunction-sections', '-fdata-sections', '-pthread',
           f'-DSTREMIO_VERSION="{VERSION}"', '-DSTREMIO_TITLE_ID="PPSA74126"',
           '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'third_party'),
           str(ROOT / 'tests/test_native_diagnostics.cpp'), str(ROOT / 'src/diagnostics.cpp'),
           str(ROOT / 'src/util.cpp'), str(shims), '-Wl,--gc-sections',
           *['-Wl,--wrap=' + symbol for symbol in wraps], '-o', str(binary)]
subprocess.run(command, check=True)
cases = []
output = []
checks = 0


def require(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1


with tempfile.TemporaryDirectory(prefix='stremio-native-diagnostics-') as folder:
    for name, expected in [('abort', -signal.SIGABRT), ('assert', -signal.SIGABRT),
                           ('assert_privacy', -signal.SIGABRT), ('signal_abort', -signal.SIGABRT),
                           ('segv', -signal.SIGSEGV), ('allocation', 0), ('immediate_exit', 37)]:
        path = Path(folder) / name
        result = subprocess.run([str(binary), str(path), name], capture_output=True, text=True, timeout=15)
        output.append(name + '\n' + result.stdout + result.stderr)
        require(result.returncode == expected,
                f'{name}: expected native fatal status {expected}, got {result.returncode}; 120 means unsafe runtime service')
        receipt = (path / 'crash-current.txt').read_text()
        require(receipt.startswith(f'Stremio Plus {VERSION} (PPSA74126) build='), name + ': build identity missing')
        require(all(len(line.encode()) <= 1024 for line in receipt.splitlines()), name + ': unbounded record')
        require('/private/build' not in receipt and 'private.example' not in receipt and 'secret' not in receipt,
                name + ': private assertion text leaked')
        if name not in ('allocation', 'immediate_exit'):
            require('signal=0x' in receipt and ' si_code=' in receipt, name + ': signal metadata missing')
            for field in ('pc', 'sp', 'anchor'):
                value = re.search(r'\b' + field + r'=(0x[0-9a-f]+)', receipt)
                require(value is not None and int(value.group(1), 16) != 0, name + ': missing raw ' + field)
            require((' fault=' in receipt) == (name == 'segv'), name + ': signal union interpreted as fault address')
        if name in ('abort', 'assert', 'assert_privacy'):
            require('event=abort caller=0x' in receipt, name + ': native abort wrapper bypassed')
            require(int(re.search(r'event=abort caller=(0x[0-9a-f]+)', receipt).group(1), 16) != 0,
                    name + ': no external abort caller')
        if name == 'assert':
            require('source=video.c line=217 function=render_frame expression=frame != NULL && width > 0' in receipt,
                    'assert: assertion location/expression missing')
            require(int(re.search(r'event=assert caller=(0x[0-9a-f]+)', receipt).group(1), 16) != 0,
                    'assert: no external assertion caller')
        if name == 'assert_privacy':
            require('expression=[redacted source text]' in receipt, 'assert privacy: bounded source redaction absent')
        if name == 'segv':
            observed = int(re.search(r' fault=(0x[0-9a-f]+)', receipt).group(1), 16)
            expected_fault = int(re.search(r'expected_fault=(0x[0-9a-f]+)', result.stdout).group(1), 16)
            require(observed == expected_fault, 'segv: fault address differs from actual inaccessible page')
        if name == 'allocation':
            require(receipt.count('event=allocation_failure') == 8, 'allocation: record cap differs from eight')
            require('ordinal=8 operation=1 requested=1048583 alignment=32 caller=0x000000000000567f' in receipt,
                    'allocation: requested size, operation or caller not preserved')
            require('heap_capacity=1024 heap_used_snapshot=256 heap_free_snapshot=768 heap_observed_peak=256' in receipt,
                    'allocation: cached heap snapshot missing')
            require('signal=' not in receipt, 'allocation: nonfatal failure misclassified as a signal')
        if name == 'immediate_exit':
            require('event=exit status=37 caller=0x' in receipt, 'immediate exit: status/caller missing')
            for field in ('caller', 'anchor'):
                value = re.search(r'\b' + field + r'=(0x[0-9a-f]+)', receipt)
                require(value is not None and int(value.group(1), 16) != 0, 'immediate exit: missing ' + field)
            require('signal=' not in receipt, 'immediate exit: ordinary exit misclassified as signal')
        if name in ('abort', 'immediate_exit'):
            # A healthy session writes numeric snapshots/failures into current;
            # it must not replace the last real crash on the following launch.
            for restart in range(2):
                clean = subprocess.run([str(binary), str(path), 'allocation'], capture_output=True, text=True, timeout=15)
                require(clean.returncode == 0, 'rotation: clean fixture failed')
                require((path / 'crash-last.txt').read_text() == receipt,
                        'rotation: a clean session replaced the last fatal receipt')
            # Exercise a real rename failure even when the host user is root.
            blocked = Path(folder) / (name + '-blocked-rotation')
            blocked.mkdir()
            (blocked / 'crash-current.txt').write_text(receipt)
            (blocked / 'crash-last.txt').mkdir()
            recovered = subprocess.run([str(binary), str(blocked), 'allocation'], capture_output=True, text=True, timeout=15)
            require(recovered.returncode == 0, 'rotation: append after rename failure failed')
            require((blocked / 'crash-current.txt').read_text().startswith(receipt),
                    'rotation: rename failure truncated the original fatal evidence')
            require(json.loads((blocked / 'session.json').read_text())['crash_rotation_error'] != 0,
                    'rotation: rename failure was not recorded')
            require('could not be rotated' in (blocked / 'log.txt').read_text(),
                    'rotation: rename failure was not logged')
            # The 0.5.9 receipt used signal= without an event= prefix.
            legacy = Path(folder) / (name + '-legacy-rotation')
            legacy.mkdir()
            old_receipt = 'Stremio Plus 0.5.9 (PPSA74126) build=fixture\nsignal=0x0000000000000006 pc=0x1234 sp=0x5678\n'
            (legacy / 'crash-current.txt').write_text(old_receipt)
            upgraded = subprocess.run([str(binary), str(legacy), 'allocation'], capture_output=True, text=True, timeout=15)
            require(upgraded.returncode == 0, 'rotation: legacy upgrade fixture failed')
            require((legacy / 'crash-last.txt').read_text() == old_receipt,
                    'rotation: the old signal receipt was not preserved')
        cases.append({'name': name, 'status': 'PASS', 'returncode': result.returncode,
                      'receiptBytes': len(receipt.encode())})

inputs = ['src/diagnostics.h', 'src/diagnostics_native.h', 'src/diagnostics.cpp', 'src/util.cpp', 'src/util.h',
          'third_party/json.hpp',
          'native/opengl_shims.c', 'tools/build-native-entry.sh',
          'tests/test_native_diagnostics.cpp', 'tests/run_native_diagnostics_tests.py']
report = {'status': 'PASS', 'version': VERSION, 'scope': 'Real host subprocesses executing native abort/assert/nonzero _Exit hooks and fatal signal capture',
          'checks': checks, 'cases': cases,
          'rotationChecks': 'Real abort and immediate exit followed by two clean starts; actual rename failure; legacy signal= receipt',
          'runtimeServicesRejectedInEmergencyPath': ['malloc', 'calloc', 'realloc', 'free', 'operator new/delete',
                                                    'pthread mutex lock/trylock', 'fprintf/vfprintf', 'fflush', 'snprintf/vsnprintf'],
          'limitations': 'Host signal ABI only; console fatal behavior and PS5 addresses require hardware verification.',
          'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs},
          'binarySha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
(BUILD / 'test-output.txt').write_text('\n'.join(output))
(BUILD / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS: native emergency diagnostics; {len(cases)} real subprocess cases, {checks} checks')
