#!/usr/bin/env python3
"""Link the UI integration test against an already built host application.

Usage: python3 tests/run_home_ui_tests.py [build-host]
The test performs no online requests and uses a temporary offline App profile.
"""
import pathlib
import shlex
import subprocess
import sys


def run(command, **options):
    result = subprocess.run(command, **options)
    if result.returncode:
        raise SystemExit(result.returncode)

project = pathlib.Path(__file__).resolve().parents[1]
build = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else project / "build-host"
link_file = build / "CMakeFiles/stremio.dir/link.txt"
if not link_file.is_file():
    sys.exit("Build the host application first (CMake Unix Makefiles generator).")
link = shlex.split(link_file.read_text())
main_object = "CMakeFiles/stremio.dir/src/main.cpp.o"
if main_object not in link:
    sys.exit("Unrecognized host link command: main object not found.")
missing = [name for name in link if name.endswith(".o") and not (build / name).is_file()]
if missing:
    sys.exit("Host build is incomplete; finish it before running UI checks.")
test_object = build / "test_home_ui.cpp.o"
run([
    link[0], "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-D_REENTRANT",
    "-I" + str(project / "src"), "-I" + str(project / "third_party"),
    "-I" + str(project / "vendor/ps5-homebrew-ui/src"), "-I/usr/include/SDL2",
    "-c", str(project / "tests/test_home_ui.cpp"), "-o", str(test_object)
])
link[link.index(main_object)] = str(test_object)
output_index = link.index("-o") + 1
link[output_index] = str(build / "test_home_ui")
run(link, cwd=build)
run([str(build / "test_home_ui"), str(project / "app"),
     str(project / "tests/fixtures/showcase.json")], cwd=project)
