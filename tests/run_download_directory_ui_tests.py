#!/usr/bin/env python3
"""Link real folder picker and controller tests against completed host objects."""
from pathlib import Path
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/desktop"
link = shlex.split((build / "CMakeFiles/stremio.dir/link.txt").read_text())
out = root / "build/tests/download-directory-ui"
out.mkdir(parents=True, exist_ok=True)
obj = out / "test_download_directory_ui.o"
executable = out / "test_download_directory_ui"
sdl = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
subprocess.run([link[0], "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
                "-I" + str(root / "src"), "-I" + str(root / "third_party"),
                "-I" + str(root / "vendor/ps5-homebrew-ui/src"), *sdl,
                "-c", str(root / "tests/test_download_directory_ui.cpp"), "-o", str(obj)], check=True)
link[link.index("CMakeFiles/stremio.dir/src/main.cpp.o")] = str(obj)
link[link.index("-o") + 1] = str(executable)
subprocess.run(link + ["-Wl,--wrap=curl_easy_perform"], cwd=build, check=True)
subprocess.run([str(executable), str(root / "app")], check=True, timeout=45)
