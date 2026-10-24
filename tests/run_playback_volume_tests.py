#!/usr/bin/env python3
"""Exercise the production stick policy and PCM gain with ASan and UBSan."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sources = [root / name for name in ("src/playback_volume.h", "src/player_vlc_audio.h",
    "tests/test_playback_volume.cpp", "tests/run_playback_volume_tests.py")]
with tempfile.TemporaryDirectory(prefix="stremio-playback-volume-") as temporary:
    binary = Path(temporary) / "volume"
    subprocess.run([os.environ.get("HOST_CXX", "clang++-18"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(root / "src"), str(root / "tests/test_playback_volume.cpp"), "-o", str(binary)],
        check=True, timeout=90)
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"), timeout=30)
    print(run.stdout, end="")
    run.check_returncode()
    result = json.loads(run.stdout.splitlines()[-1])
report = {"result": "PASS", "scope": "Production right-stick volume policy and decoded signed PCM gain",
    "sanitizers": ["AddressSanitizer", "UndefinedBehaviorSanitizer"], "results": result,
    "sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
target = root / "build/tests/playback-volume.json"
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(json.dumps(report, indent=2) + "\n")
print(target)
