#!/usr/bin/env python3
"""Exercise the signing manifest and reject private-volume reservations."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
validate = root / 'tools/validate-app-metadata.py'
metadata = json.loads((root / 'app/sce_sys/param.json').read_text())
checks = 0

with tempfile.TemporaryDirectory(prefix='stremio-metadata-') as temporary:
    directory = Path(temporary)
    source = directory / 'source.json'
    output = directory / 'signed.json'

    def check(value, expected):
        global checks
        data = dict(metadata, downloadDataSize=value)
        source.write_text(json.dumps(data))
        result = subprocess.run([sys.executable, str(validate), str(source)],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == expected, (value, result.stdout, result.stderr)
        checks += 1

    check(0, True)
    for size in (16384, 256, 1, -1, False, True, '0', None, 0.5):
        check(size, False)

    source.write_text(json.dumps(metadata))
    script = (root / 'tools/build-native-entry.sh').read_text()
    match = re.search(r'python3 - "\$APP_FILES/sce_sys/param.json"[^\n]*<<\'PY\'\n(.*?)\nPY',
                      script, re.S)
    assert match, 'Native manifest assembly step not found'
    subprocess.run([sys.executable, '-', str(source), str(output), 'PPSA74126', 'Stremio Plus'],
                   input=match.group(1), text=True, check=True)
    assembled = json.loads(output.read_text())
    assert assembled['downloadDataSize'] == 0, 'Assembly reintroduced a reserved volume'
    assert assembled['contentVersion'] == metadata['contentVersion']
    subprocess.run([sys.executable, str(validate), str(output)], check=True)
    checks += 3

print(f'Metadata storage tests passed: {checks} checks.')
