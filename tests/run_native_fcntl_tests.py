#!/usr/bin/env python3
"""Exercise the production native fcntl wrapper on host files and sockets.

Only the fcntl section is compiled: the other console shims have a different
libc ABI. The test maps the console nonblocking option to host socket flags.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_path = root / "native/console_curl.c"
    source = source_path.read_text()
    start = source.index("/* ---- fcntl on sockets ")
    end = source.index("/* ---- Name lookup ", start)
    option = re.search(r"enum\s*\{\s*CONSOLE_SO_NBIO\s*=.*?\};", source, re.DOTALL)
    if option is None:
        raise RuntimeError("Console nonblocking option declaration was not found")
    unit = "\n".join((
        "#include <errno.h>", "#include <fcntl.h>", "#include <stdarg.h>",
        "#include <stdint.h>", "#include <sys/socket.h>",
        option.group(), source[start:end],
        "int test_console_nbio_option(void) { return CONSOLE_SO_NBIO; }", "",
    ))
    source_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="stremio-native-fcntl-") as temporary:
        folder = Path(temporary)
        compiled_source = folder / "console_fcntl.c"
        compiled_source.write_text(unit)
        executable = folder / "test_native_fcntl"
        subprocess.run([
            os.environ.get("HOST_CC", "clang-18"), "-std=c11", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", str(compiled_source),
            str(root / "tests/test_native_fcntl.c"), "-Wl,--wrap=getsockopt",
            "-Wl,--wrap=setsockopt", "-o", str(executable),
        ], check=True, timeout=60)
        environment = dict(os.environ)
        environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        completed = subprocess.run([str(executable)], check=True, text=True,
                                   stdout=subprocess.PIPE, timeout=30, env=environment)
    report = json.loads(completed.stdout)
    report.update({
        "scope": "Host files and sockets with injected native fcntl errors",
        "sourceSha256": source_hash,
        "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"],
    })
    if hashlib.sha256(source_path.read_bytes()).hexdigest() != source_hash:
        raise RuntimeError("The production source changed during the test")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
