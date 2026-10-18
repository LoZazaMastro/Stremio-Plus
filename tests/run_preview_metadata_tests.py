#!/usr/bin/env python3
"""Exercise focused artwork enrichment with real App/Tasks and wrapped HTTP."""
from pathlib import Path
import json
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'build-verified'
link = shlex.split((build / 'CMakeFiles/stremio.dir/link.txt').read_text())
names = [line.split()[0] for line in subprocess.check_output(
    ['nm', '-g', '--defined-only', '--format=posix', str(build / 'stremio')], text=True).splitlines()]
readable = subprocess.check_output(['c++filt'], input='\n'.join(names)+'\n', text=True).splitlines()
symbols = {}
for macro, prefix in {'WRAP_FETCH_JSON':'fetch_json', 'WRAP_APP_ART':'App::art'}.items():
    matches = [name for name, demangled in zip(names, readable) if demangled.startswith(prefix+'(') or demangled.startswith(prefix+'[abi:')]
    if len(matches) != 1: raise SystemExit(f'Expected one {prefix} boundary; found {matches}')
    symbols[macro] = matches[0]
output = root / 'build/tests/preview-metadata'
output.mkdir(parents=True, exist_ok=True)
header = output / 'wrap.h'
header.write_text('#pragma once\n'+''.join(f'#define {macro} {json.dumps("__wrap_"+name)}\n' for macro,name in symbols.items()))
obj, executable = output/'test_preview_metadata.o', output/'test_preview_metadata'
sdl = shlex.split(subprocess.check_output(['pkg-config','--cflags','sdl2'], text=True))
subprocess.run([link[0],'-std=c++20','-O1','-g','-Wall','-Wextra','-Wpedantic','-pthread',
    '-I'+str(root/'src'),'-I'+str(root/'third_party'),*sdl,'-include',str(header),
    '-c',str(root/'tests/test_preview_metadata.cpp'),'-o',str(obj)], check=True)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
link[link.index('-o')+1] = str(executable)
subprocess.run(link+['-Wl,--wrap='+name for name in symbols.values()]+['-Wl,--wrap=curl_easy_perform'], cwd=build, check=True)
result = subprocess.run([str(executable)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)
(output/'preview-metadata-test-output.txt').write_text(result.stdout)
print(result.stdout,end='')
raise SystemExit(result.returncode)
