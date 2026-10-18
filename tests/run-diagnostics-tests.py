#!/usr/bin/env python3
"""Host-only diagnostics regression checks; no console or network access."""
from pathlib import Path
import copy
import hashlib
import importlib.util
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_receipt', root / 'tools/build-receipt.py')
build_receipt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_receipt)
version, title_id, content_version = build_receipt.release_identity(root, root / 'app')
# Reject a stale package even if its binaries and checksums are internally
# consistent. All mutation here happens in a disposable host fixture.
with tempfile.TemporaryDirectory(prefix='stremio-release-identity-') as folder:
    fixture = Path(folder)
    (fixture / 'sce_sys').mkdir()
    param = json.loads((root / 'app/sce_sys/param.json').read_text())
    for field, stale in (('contentVersion', '00.041.000'), ('titleId', 'PPSA00000')):
        changed = dict(param, **{field: stale})
        (fixture / 'sce_sys/param.json').write_text(json.dumps(changed))
        try:
            build_receipt.release_identity(root, fixture)
        except SystemExit:
            pass
        else:
            raise SystemExit(f'Release verification accepted mismatched {field}')
    for language in ('en-US', 'it-IT'):
        changed = copy.deepcopy(param)
        changed['localizedParameters']['defaultLanguage'] = language
        changed['localizedParameters'][language] = {'titleName': 'Stremio'}
        (fixture / 'sce_sys/param.json').write_text(json.dumps(changed))
        try:
            build_receipt.release_identity(root, fixture)
        except SystemExit:
            pass
        else:
            raise SystemExit(f'Release verification accepted the old application name for {language}')
        validation = subprocess.run(
            ['python3', str(root / 'tools/validate-app-metadata.py'), str(fixture / 'sce_sys/param.json')],
            capture_output=True, text=True, check=False)
        if validation.returncode == 0:
            raise SystemExit(f'Metadata validation accepted the old application name for {language}')
with tempfile.TemporaryDirectory(prefix='stremio-receipt-identity-') as folder:
    fixture = Path(folder)
    source = fixture / 'source'
    package = fixture / 'package'
    source.mkdir()
    (package / 'sce_sys').mkdir(parents=True)
    (source / 'CMakeLists.txt').write_text((root / 'CMakeLists.txt').read_text())
    (source / 'cmake').mkdir()
    (source / 'cmake/vlc.cmake').write_text('# fixture playback configuration\n')
    (source / 'tools').mkdir()
    (source / 'tools/vlc-build-receipt.py').write_text((root / 'tools/vlc-build-receipt.py').read_text())
    vendor = source / 'vendor/vlc-ps5'
    vendor.mkdir(parents=True)
    (vendor / 'plugins.txt').write_text('fixture\n')
    work = source / '.deps/vlc'
    (work / 'dl').mkdir(parents=True)
    source_archive = b'receipt source archive fixture'
    (work / 'dl/fixture.tar').write_bytes(source_archive)
    (vendor / 'sources.sha256').write_text(hashlib.sha256(source_archive).hexdigest() + '  fixture.tar\n')
    # An archive member fixture exercises integrity checks without PS5 dependencies.
    object_data = b'receipt-object'
    archive_header = f'{"fixture.o/":<16}{0:<12}{0:<6}{0:<6}{"644":<8}{len(object_data):<10}`\n'.encode()
    archive_data = b'!<arch>\n' + archive_header + object_data + b'\n' * (len(object_data) % 2)
    for name in ('build-vlc-ps5/lib/.libs/libvlc.a', 'build-vlc-ps5/src/.libs/libvlccore.a',
                 'prefix-ps5/lib/libavcodec.a', 'build-vlc-ps5/modules/.libs/libfixture_plugin.a'):
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(archive_data)
    (work / 'build-vlc-ps5/config.h').write_text('#define VLC_FIXTURE 1\n')
    static = work / 'static-ps5'
    static.mkdir()
    (static / 'static_modules.c').write_text('vlc_entry__fixture\n')
    (static / 'link.rsp').write_text(str(work / 'build-vlc-ps5/modules/.libs/libfixture_plugin.a') + '\n')
    (work / 'src/ffmpeg-8.1.2/ffbuild').mkdir(parents=True)
    (work / 'src/ffmpeg-8.1.2/ffbuild/config.mak').write_text('# FFmpeg build fixture\n')
    environment = dict(os.environ, STREMIO_VLC_ROOT=str(work))
    subprocess.run(['python3', str(root / 'tools/vlc-build-receipt.py'), str(work), 'ps5', str(vendor)],
                   capture_output=True, text=True, check=True)
    playback_receipt = work / 'build-receipt-ps5.json'
    (package / 'sce_sys/param.json').write_text((root / 'app/sce_sys/param.json').read_text())
    receipt = {
        'application': 'Stremio Plus',
        'applicationVersion': version,
        'titleId': title_id,
        'contentVersion': content_version,
        'sourceTreeSha256': build_receipt.tree_hash(build_receipt.sources(source)),
        'artifactsSha256': build_receipt.artifacts(package),
        'playbackDependencies': {'receiptSha256': build_receipt.file_hash(playback_receipt),
                                 'details': json.loads(playback_receipt.read_text())},
    }
    receipt_file = package / 'build-receipt.json'
    receipt_file.write_text(json.dumps(receipt))
    command = ['python3', str(root / 'tools/build-receipt.py'), '--verify', str(source), str(package)]
    subprocess.run(command, env=environment, capture_output=True, text=True, check=True)
    # A valid but different static archive must invalidate the application receipt.
    library = work / 'prefix-ps5/lib/libavcodec.a'
    library.write_bytes(archive_data.replace(object_data, b'changed-object'))
    validation = subprocess.run(command, env=environment, capture_output=True, text=True, check=False)
    if validation.returncode == 0:
        raise SystemExit('Receipt validation accepted changed playback dependencies')
    library.write_bytes(archive_data)
    (source / 'cmake/vlc.cmake').write_text('# changed playback configuration\n')
    validation = subprocess.run(command, env=environment, capture_output=True, text=True, check=False)
    if validation.returncode == 0 or 'Sources/assets changed' not in validation.stderr:
        raise SystemExit('Receipt validation accepted changed CMake playback configuration')
    (source / 'cmake/vlc.cmake').write_text('# fixture playback configuration\n')
    receipt['application'] = 'Stremio'
    receipt_file.write_text(json.dumps(receipt))
    validation = subprocess.run(command, env=environment, capture_output=True, text=True, check=False)
    if validation.returncode == 0 or 'identity differs' not in validation.stderr:
        raise SystemExit('Receipt validation did not reject the old application name')
build = root / 'build' / 'diagnostics-tests'
build.mkdir(parents=True, exist_ok=True)
binary = build / 'diagnostics-test'
subprocess.run([
    os.environ.get('HOST_CXX', 'clang++-18'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
    f'-DSTREMIO_VERSION="{version}"', f'-DSTREMIO_TITLE_ID="{title_id}"',
    '-DSTREMIO_DIAGNOSTICS_ROTATE_BYTES=16384', '-I' + str(root / 'src'), '-I' + str(root / 'third_party'),
    str(root / 'tests/diagnostics_test.cpp'), str(root / 'src/diagnostics.cpp'), str(root / 'src/util.cpp'),
    '-pthread', '-o', str(binary),
], check=True)
with tempfile.TemporaryDirectory(prefix='stremio-diagnostics-') as folder:
    result = subprocess.run([str(binary), folder], text=True, capture_output=True, timeout=45)
    (build / 'test-output.txt').write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr[-4000:])
        raise SystemExit(result.returncode)
    if 'DIAGNOSTICS_TESTS_OK' not in result.stdout:
        raise SystemExit('Diagnostics test did not reach its completion marker')
    forbidden = ('account-private-key', 'short-sensitive-token', 'cookie-value', 'also-sensitive',
                 'header-secret', 'runtime-secret', 'nested-secret', 'private-runtime@account.example')
    if any(secret in result.stdout + result.stderr for secret in forbidden):
        raise SystemExit('A sensitive fixture escaped into the console output')
print(f'PASS: release identity {version} / {title_id} / {content_version} agrees across CMake, PS5 metadata, session/runtime files and crash receipts; stale identities, changed CMake files and changed playback archives are rejected.')
print('PASS: redaction, safe runtime context, bounded rotation, JSONL, clean/unclean sessions, fatal-signal receipts.')
