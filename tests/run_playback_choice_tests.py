#!/usr/bin/env python3
"""Exercise real online App playback choices against the completed host build.

Only decoder opening, artwork and HTTP boundaries are wrapped. Source selection,
bookmarks, Tasks, dropdown navigation, cancellation and launch are production.
"""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/desktop"
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
main_object = "CMakeFiles/stremio.dir/src/main.cpp.o"
if main_object not in link:
    raise SystemExit("Expected main.cpp.o in the completed host link command")
for item in link:
    if item.endswith(".o") and not (build / item).is_file():
        raise SystemExit("Incomplete host build; finish it before testing playback choices")
names = [line.split()[0] for line in subprocess.check_output(
    ["nm", "-g", "--defined-only", "--format=posix", str(build / "stremio")], text=True).splitlines()]
readable = subprocess.check_output(["c++filt"], input="\n".join(names) + "\n", text=True).splitlines()
boundaries = {"WRAP_PLAYER_OPEN": "Player::open", "WRAP_ART_GET": "ArtCache::get",
              "WRAP_ART_PEEK": "ArtCache::peek", "WRAP_FETCH_JSON": "fetch_json",
              "WRAP_ART_GET_ASYNC": "ArtCache::get_async", "WRAP_ART_PEEK_CACHED": "ArtCache::peek_cached"}
symbols = {}
for macro, prefix in boundaries.items():
    matches = [name for name, description in zip(names, readable)
               if description.startswith(prefix + "(") or description.startswith(prefix + "[abi:")]
    if len(matches) != 1:
        raise SystemExit(f"Expected one production symbol for {prefix}; got {matches}")
    symbols[macro] = matches[0]
output = root / "build/tests/playback-choice"
output.mkdir(parents=True, exist_ok=True)
header = output / "playback_choice_wrap_symbols.h"
header.write_text("#pragma once\n" + "".join(
    f"#define {macro} {json.dumps('__wrap_' + symbol)}\n" for macro, symbol in symbols.items()))
obj, executable = output / "test_playback_choice.o", output / "test_playback_choice"
sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
definitions = shlex.split(next(line.split("=", 1)[1] for line in
    (build / "CMakeFiles/stremio.dir/flags.make").read_text().splitlines() if line.startswith("CXX_DEFINES =")))
subprocess.run([link[0], "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
                *definitions, "-I" + str(root / "src"), "-I" + str(root / "third_party"), *sdl,
                "-include", str(header), "-c", str(root / "tests/test_playback_choice.cpp"), "-o", str(obj)], check=True)
link[link.index(main_object)] = str(obj)
link[link.index("-o") + 1] = str(executable)
link += ["-Wl,--wrap=" + symbol for symbol in symbols.values()] + ["-Wl,--wrap=curl_easy_perform"]
subprocess.run(link, cwd=build, check=True)
with tempfile.TemporaryDirectory(prefix="stremio-playback-choice-") as directory:
    environment = os.environ.copy(); environment["SDL_AUDIODRIVER"] = "dummy"
    result = subprocess.run([str(executable), directory], env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (output / "playback-choice-test-output.txt").write_text(result.stdout)
    print(result.stdout, end="")
    raise SystemExit(result.returncode)
