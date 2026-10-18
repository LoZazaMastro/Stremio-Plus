#!/usr/bin/env python3
"""Exercise real App/Tasks login and logout against controlled offline boundaries.

Usage: python3 tests/run_account_tests.py [build-verified]
Finish the host build first. This runner never rebuilds or replaces its objects.
"""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build-verified'
link_file = build / 'CMakeFiles/stremio.dir/link.txt'
link = shlex.split(link_file.read_text())
main_object = 'CMakeFiles/stremio.dir/src/main.cpp.o'
if main_object not in link:
    raise SystemExit('The existing host link command does not contain main.cpp.o')
for item in link:
    if item.endswith('.o') and not (build / item).is_file():
        raise SystemExit('Incomplete host build; wait for the application build to finish')

names = [line.split()[0] for line in subprocess.check_output(
    ['nm', '-g', '--defined-only', '--format=posix', str(build / 'stremio')], text=True).splitlines()]
demangled = subprocess.check_output(['c++filt'], input='\n'.join(names) + '\n', text=True).splitlines()
boundaries = {'WRAP_LINK_CREATE': 'link_create', 'WRAP_LINK_READ': 'link_read',
              'WRAP_GET_USER': 'api_get_user', 'WRAP_LOGOUT': 'api_logout',
              'WRAP_ADDON_COLLECTION': 'api_addon_collection', 'WRAP_LIBRARY_GET': 'api_library_get',
              'WRAP_ADDONS': 'App::load_addons', 'WRAP_LIBRARY': 'App::load_library', 'WRAP_ART': 'App::art'}
symbols = {}
for macro, prefix in boundaries.items():
    matches = [name for name, readable in zip(names, demangled)
               if readable.startswith(prefix + '(') or readable.startswith(prefix + '[abi:')]
    if len(matches) != 1:
        raise SystemExit(f'Expected one real symbol for {prefix}; found {matches}')
    symbols[macro] = matches[0]

output = root / 'build/tests/account'
output.mkdir(parents=True, exist_ok=True)
header = output / 'account_wrap_symbols.h'
header.write_text('#pragma once\n' + ''.join(
    f'#define {macro} {json.dumps("__wrap_" + name)}\n' for macro, name in symbols.items()))
obj, executable = output / 'test_account.o', output / 'test_account'
sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True))
subprocess.run([link[0], '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-pthread',
                '-I' + str(root / 'src'), '-I' + str(root / 'third_party'), *sdl,
                '-include', str(header), '-c', str(root / 'tests/test_account.cpp'), '-o', str(obj)], check=True)
link[link.index(main_object)] = str(obj)
link[link.index('-o') + 1] = str(executable)
link += ['-Wl,--wrap=' + name for name in symbols.values()] + ['-Wl,--wrap=curl_easy_perform']
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix='stremio-account-state-') as profile:
    environment = os.environ.copy()
    environment['SDL_AUDIODRIVER'] = 'dummy'
    result = subprocess.run([str(executable), profile], env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (output / 'account-test-output.txt').write_text(result.stdout)
    print(result.stdout, end='')
    raise SystemExit(result.returncode)
