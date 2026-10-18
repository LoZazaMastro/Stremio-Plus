#!/usr/bin/env python3
"""Link focused App/DownloadManager wiring checks against the finished host build.

Usage: python3 tests/run_download_app_tests.py [build-verified]
Real queue persistence and callbacks run; transfer, artwork and decoder entry
are controlled boundaries. Production objects are never rebuilt by this runner.
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
    raise SystemExit("Expected main.cpp.o in the completed host link command")
for item in link:
    if item.endswith(".o") and not (build / item).is_file():
        raise SystemExit("Incomplete host build; finish it before running App download checks")
names = [line.split()[0] for line in subprocess.check_output(
    ["nm", "-g", "--defined-only", "--format=posix", str(build / "stremio")], text=True).splitlines()]
readable = subprocess.check_output(["c++filt"], input="\n".join(names) + "\n", text=True).splitlines()
boundaries = {"WRAP_DOWNLOAD_TRANSFER": "download_transfer", "WRAP_PLAYER_OPEN": "Player::open",
              "WRAP_ART_GET": "ArtCache::get", "WRAP_ART_PEEK": "ArtCache::peek", "WRAP_FETCH_JSON": "fetch_json",
              "WRAP_ART_GET_ASYNC": "ArtCache::get_async", "WRAP_ART_PEEK_CACHED": "ArtCache::peek_cached"}
symbols = {}
for macro, prefix in boundaries.items():
    matches = [name for name, description in zip(names, readable)
               if description.startswith(prefix + "(") or description.startswith(prefix + "[abi:")]
    if len(matches) != 1:
        raise SystemExit(f"Expected one production symbol for {prefix}; got {matches}")
    symbols[macro] = matches[0]
output = root / "build/tests/download-app"
output.mkdir(parents=True, exist_ok=True)
header = output / "download_app_wrap_symbols.h"
header.write_text("#pragma once\n" + "".join(
    f"#define {macro} {json.dumps('__wrap_' + symbol)}\n" for macro, symbol in symbols.items()))
obj, executable = output / "test_download_app.o", output / "test_download_app"
sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
subprocess.run([link[0], "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
                "-I" + str(root / "src"), "-I" + str(root / "third_party"), *sdl,
                "-include", str(header), "-c", str(root / "tests/test_download_app.cpp"), "-o", str(obj)], check=True)
link[link.index(main_object)] = str(obj)
link[link.index("-o") + 1] = str(executable)
link += ["-Wl,--wrap=" + symbol for symbol in symbols.values()] + ["-Wl,--wrap=curl_easy_perform"]
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix="stremio-download-app-") as directory:
    environment = os.environ.copy(); environment["SDL_AUDIODRIVER"] = "dummy"
    result = subprocess.run([str(executable), directory], env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (output / "download-app-test-output.txt").write_text(result.stdout)
    print(result.stdout, end="")
    raise SystemExit(result.returncode)
