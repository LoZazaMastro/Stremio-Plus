#!/usr/bin/env python3
"""Run real App interaction regressions with offline API boundaries.

Usage: python3 tests/run_interaction_tests.py [build-verified]
Finish the host build first; this runner never rebuilds production objects.
"""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build-verified"
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
main_object = "CMakeFiles/stremio.dir/src/main.cpp.o"
if main_object not in link:
    raise SystemExit("Host link command does not contain main.cpp.o")
for item in link:
    if item.endswith(".o") and not (build / item).is_file():
        raise SystemExit("Incomplete host build; wait for the full application build")
names = [line.split()[0] for line in subprocess.check_output(
    ["nm", "-g", "--defined-only", "--format=posix", str(build / "stremio")], text=True).splitlines()]
readable = subprocess.check_output(["c++filt"], input="\n".join(names) + "\n", text=True).splitlines()
boundaries = {"WRAP_LIBRARY_PUT": "api_library_put", "WRAP_LIBRARY_GET": "api_library_get",
              "WRAP_FETCH_JSON": "fetch_json", "WRAP_ART_GET": "ArtCache::get", "WRAP_ART_PEEK": "ArtCache::peek"}
symbols = {}
for macro, prefix in boundaries.items():
    matches = [name for name, description in zip(names, readable)
               if description.startswith(prefix + "(") or description.startswith(prefix + "[abi:")]
    if len(matches) != 1:
        raise SystemExit(f"Expected one production symbol for {prefix}; found {matches}")
    symbols[macro] = matches[0]
output = root / "build/tests/interactions"
output.mkdir(parents=True, exist_ok=True)
header = output / "interaction_wrap_symbols.h"
header.write_text("#pragma once\n" + "".join(
    f"#define {macro} {json.dumps('__wrap_' + symbol)}\n" for macro, symbol in symbols.items()))
obj, executable = output / "test_interactions.o", output / "test_interactions"
sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
subprocess.run([link[0], "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
                "-I" + str(root / "src"), "-I" + str(root / "third_party"), *sdl,
                "-include", str(header), "-c", str(root / "tests/test_interactions.cpp"), "-o", str(obj)], check=True)
link[link.index(main_object)] = str(obj)
link[link.index("-o") + 1] = str(executable)
link += ["-Wl,--wrap=" + symbol for symbol in symbols.values()] + ["-Wl,--wrap=curl_easy_perform"]
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix="stremio-interactions-") as directory:
    environment = os.environ.copy(); environment["SDL_AUDIODRIVER"] = "dummy"
    result = subprocess.run([str(executable), directory], env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (output / "interaction-test-output.txt").write_text(result.stdout)
    print(result.stdout, end="")
    raise SystemExit(result.returncode)
