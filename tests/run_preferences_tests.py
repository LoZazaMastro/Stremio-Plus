#!/usr/bin/env python3
"""Link real App settings tests with already-completed host build objects."""
from pathlib import Path
import argparse
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('build', nargs='?', type=Path, default=root / 'build-verified')
parser.add_argument('--console-policy', action='store_true',
                    help='Compile the production App settings paths with the native PS5 policy while using host I/O.')
args = parser.parse_args()
build = args.build.resolve()
link = shlex.split((build / 'CMakeFiles/stremio.dir/link.txt').read_text())
output = root / ('build/tests/preferences-console-policy' if args.console_policy else 'build/tests/preferences')
output.mkdir(parents=True, exist_ok=True)
obj, executable = output / 'test_preferences.o', output / 'test_preferences'
sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True))
policy_flags = ['-DPLATFORM_PS5_NATIVE=1'] if args.console_policy else []
subprocess.run([link[0], '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-pthread',
    '-I' + str(root / 'src'), '-I' + str(root / 'third_party'), *sdl, *policy_flags,
    '-c', str(root / 'tests/test_preferences.cpp'), '-o', str(obj)], check=True)
if args.console_policy:
    print('Policy scope: native PS5 settings compiled with host filesystem and platform dependencies.', flush=True)
    app_object = output / 'app_console_policy.o'
    subprocess.run([link[0], '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-pthread',
        '-I' + str(root / 'src'), '-I' + str(root / 'third_party'), *sdl, *policy_flags,
        '-c', str(root / 'src/app.cpp'), '-o', str(app_object)], check=True)
    link[link.index('CMakeFiles/stremio.dir/src/app.cpp.o')] = str(app_object)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
link[link.index('-o') + 1] = str(executable)
subprocess.run(link + ['-Wl,--wrap=curl_easy_perform'], cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix='stremio-preferences-') as profile:
    result = subprocess.run([str(executable), profile], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=30)
    (output / 'preferences-test-output.txt').write_text(result.stdout)
    print(result.stdout, end='')
    raise SystemExit(result.returncode)
