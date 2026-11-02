#!/usr/bin/env python3
"""Exercise production fallback imports and module-audit rejection paths."""
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
folder = root / 'build/tests/native-import-shims'
folder.mkdir(parents=True, exist_ok=True)
binary = folder / 'native-import-shims'
subprocess.run([os.environ.get('HOST_CC', 'clang-18'), '-std=c11', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(root / 'tests/test_native_import_shims.c'), '-o', str(binary)], check=True)
run = subprocess.run([str(binary)], text=True, capture_output=True,
                     env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'), timeout=10)
print(run.stdout, end='')
if run.returncode: print(run.stderr)
run.check_returncode()
report = json.loads(run.stdout)
spec = importlib.util.spec_from_file_location('native_import_check', root / 'native/check_imports.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
assert 'libkernel_sys' not in audit.LOADED
cases = []
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for module in ('libkernel', 'libkernel_sys', 'libSceNotLoaded'):
        (directory / (module + '.so')).touch()
    exports = {'libkernel': {'open', 'stat'}, 'libkernel_sys': {'pathconf'}, 'libSceNotLoaded': {'unsafe_api'}}
    for wanted, expected in [({'open', 'stat'}, 0), ({'pathconf'}, 1), ({'unsafe_api'}, 1), ({'missing'}, 1)]:
        audit.symbols = lambda path, *flags: wanted if path == 'fixture.elf' else exports[Path(path).stem]
        previous = sys.argv
        sys.argv = ['check_imports.py', 'fixture.elf', str(directory)]
        try:
            status = 0
            with redirect_stdout(io.StringIO()):
                try: audit.main()
                except SystemExit as error: status = error.code
            assert status == expected, (wanted, status, expected)
            cases.append({'imports': sorted(wanted), 'exit': status})
        finally:
            sys.argv = previous
report.update(importAuditCases=cases, scope='Production POSIX import shims plus permitted-module import-audit rejection paths',
              limits='Filesystem calls use the host kernel; actual signed PS5 import binding still requires the native build audit.',
              sourceSha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                            for name in ('native/shims.c', 'native/check_imports.py', 'tests/test_native_import_shims.c')})
(folder / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: four module import-audit scenarios')
