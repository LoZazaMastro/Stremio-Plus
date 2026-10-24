#!/usr/bin/env python3
"""Check the production libVLC audio clock and parallel YUV transfer paths."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    files = [root / name for name in ("src/player_vlc_audio.h", "src/playback_volume.h", "src/player_vlc_copy.cpp",
        "tests/test_vlc_output.cpp", "tests/run_vlc_output_tests.py")]
    hashes = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    with tempfile.TemporaryDirectory(prefix="stremio-vlc-output-") as temporary:
        binary = Path(temporary) / "output"
        subprocess.run([os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(root / "src"), str(root / "src/player_vlc_copy.cpp"),
            str(root / "tests/test_vlc_output.cpp"), "-o", str(binary)], check=True, timeout=90)
        run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
            env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        print(run.stdout, end="", flush=True)
        run.check_returncode()
        result = json.loads(run.stdout.splitlines()[-1])
    report = {"result": "PASS", "scope": "Production timestamped PCM queue and padded/10-bit/concurrent YUV plane copies",
        "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "results": result, "sha256": hashes}
    target = root / "build/tests/vlc-output.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + "\n")
    print(target)


if __name__ == "__main__":
    main()
