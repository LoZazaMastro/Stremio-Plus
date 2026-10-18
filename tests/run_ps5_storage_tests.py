#!/usr/bin/env python3
"""Verify PS5 storage startup against real filesystem operations and denied access."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_receipt", root / "tools/build-receipt.py")
build_receipt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_receipt)
version, title_id, content_version = build_receipt.release_identity(root, root / "app")
output = root / "build/tests/ps5-storage"
output.mkdir(parents=True, exist_ok=True)
executable = output / "test_ps5_storage"
operations = "stat lstat mkdir chmod fchmod open close fstat fsync read write rename unlink rmdir opendir readdir closedir".split()
command = ["clang++-18", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(root / "native"),
           f'-DSTREMIO_VERSION="{version}"', f'-DSTREMIO_TITLE_ID="{title_id}"',
           str(root / "native/ps5_storage.cpp"), str(root / "tests/test_ps5_storage.cpp"),
           *("-Wl,--wrap=" + operation for operation in operations), "-o", str(executable)]
subprocess.run(command, check=True)
with tempfile.TemporaryDirectory(prefix="stremio-ps5-storage-") as temporary:
    environment = os.environ.copy()
    # The isolated runner cannot enumerate /proc task entries for LeakSanitizer.
    # AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.
    environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    result = subprocess.run([str(executable), temporary + "/filesystem"], env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    (output / "ps5-storage-test-output.txt").write_text(result.stdout)
    print(result.stdout, end="")
    raise SystemExit(result.returncode)
