#!/usr/bin/env python3
"""Exercise the delivery helper in isolated temporary source/cache trees."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path('/workspace/scratch/7fa7f080734e/Stremio-Plus-work')
OUT = ROOT.parent / 'validation-current'
SUPPORT = OUT / 'bundle-support'
INVENTORY = json.loads((SUPPORT / 'inventory.json').read_text())


def link_or_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def skeleton(path):
    for relative in ('tools/setup-toolchain.sh', 'tools/setup-vlc.sh', 'vendor/vlc-ps5/sources.json',
                     'source-archives/nasm-2.16.03.tar.xz', 'source-archives/freetype-2.13.3.tar.xz'):
        link_or_copy(ROOT / relative, path / relative)


report = dict(scope='Isolated offline restore, no application rebuild, no writes to active production/build trees',
              tests=[], result='FAIL')
with tempfile.TemporaryDirectory(prefix='stremio-delivery-restore-') as temporary:
    base = Path(temporary)
    bundle = base / 'Stremio-Plus-0.5.19-dependencies'
    bundle.mkdir()
    for name in ('restore-cache.py', 'inventory.json', 'SHA256SUMS'):
        shutil.copyfile(SUPPORT / name, bundle / name)
    for entry in INVENTORY['files']:
        if not entry['include_in_dependency_zip']:
            continue
        target = base / entry['archive_member']
        # Original dependency archives are never rewritten by this test.
        link_or_copy(Path(entry['source_path']), target)

    def invoke(label, target):
        started = time.monotonic()
        result = subprocess.run(['python3', str(bundle / 'restore-cache.py'), str(target)],
                                text=True, capture_output=True, timeout=240)
        item = dict(name=label, exit=result.returncode, elapsed_seconds=round(time.monotonic()-started, 3),
                    stdout=result.stdout, stderr=result.stderr)
        report['tests'].append(item)
        return result, item

    # Reject damaged delivery metadata before any cache write. Break the local
    # hardlink first; shared original cache bytes must remain untouched.
    requirements = bundle / 'packaging-wheels/requirements-offline.txt'
    original_requirements = requirements.read_bytes()
    requirements.unlink()
    requirements.write_bytes(original_requirements + b'\n# damaged test copy\n')
    rejected = base / 'rejected-source'
    skeleton(rejected)
    result, item = invoke('corrupt_input_rejected_before_mutation', rejected)
    assert result.returncode != 0 and 'Checksum/size mismatch' in result.stderr
    assert not (rejected / '.deps').exists()
    item['result'] = 'PASS'
    requirements.write_bytes(original_requirements)

    target = base / 'restored-source'
    skeleton(target)
    result, item = invoke('complete_offline_restore', target)
    assert result.returncode == 0, result.stderr
    first = json.loads((target / '.deps/offline-cache-restore.json').read_text())
    assert first['result'] == 'PASS' and first['restored_git_repositories'] == 2
    assert first['copied_cache_files'] == 23 and first['network_downloads'] == 0
    item['restore_report'] = first
    item['result'] = 'PASS'

    tracked_files = sorted((target / '.deps/downloads').glob('*')) + sorted((target / '.deps/vlc/dl').glob('*'))
    before = {str(path): (path.stat().st_size, path.stat().st_mtime_ns) for path in tracked_files}
    result, item = invoke('idempotent_second_restore', target)
    assert result.returncode == 0, result.stderr
    again = json.loads((target / '.deps/offline-cache-restore.json').read_text())
    assert again['copied_cache_files'] == 0 and again['restored_git_repositories'] == 0
    assert before == {str(path): (path.stat().st_size, path.stat().st_mtime_ns) for path in tracked_files}
    item['restore_report'] = again
    item['result'] = 'PASS'

    conflict = target / '.deps/vlc/dl/utf8cpp-3.2.5.tar.gz'
    bad = conflict.read_bytes() + b'local modification'
    conflict.write_bytes(bad)
    marker = target / '.deps/offline-cache-restore.json'
    marker_time = marker.stat().st_mtime_ns
    result, item = invoke('existing_conflicting_cache_preserved', target)
    assert result.returncode != 0 and 'Checksum/size mismatch' in result.stderr
    assert conflict.read_bytes() == bad and marker.stat().st_mtime_ns == marker_time
    item['result'] = 'PASS'

    linked = base / 'source-with-external-cache-link'
    external = base / 'unrelated-directory'
    skeleton(linked)
    external.mkdir()
    (linked / '.deps').mkdir()
    (linked / '.deps/downloads').symlink_to(external, target_is_directory=True)
    result, item = invoke('external_cache_symlink_rejected', linked)
    assert result.returncode != 0 and 'found symlink' in result.stderr
    assert not list(external.iterdir())
    item['result'] = 'PASS'
    report['result'] = 'PASS'

report['helper_sha256'] = hashlib.sha256((SUPPORT / 'restore-cache.py').read_bytes()).hexdigest()
(OUT / 'offline-restore-helper-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'result': report['result'], 'helper_sha256': report['helper_sha256'],
                  'tests': [{k:v for k,v in item.items() if k in ('name','result','elapsed_seconds')}
                            for item in report['tests']]}, indent=2))
