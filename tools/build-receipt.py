#!/usr/bin/env python3
"""Bind packaged binaries to the complete source and asset tree that built them."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

APPLICATION_NAME = 'Stremio Plus'


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def sources(root):
    result = {}
    for name in ('src', 'native', 'vendor', 'third_party', 'tools', 'cmake', 'app'):
        folder = root / name
        if folder.exists():
            for path in sorted(folder.rglob('*')):
                if path.is_file() and '__pycache__' not in path.parts and not path.name.endswith('.pyc'):
                    result[path.relative_to(root).as_posix()] = file_hash(path)
    for name in ('CMakeLists.txt', 'build.sh', 'LICENSE', 'THIRD_PARTY.md'):
        path = root / name
        if path.is_file():
            result[name] = file_hash(path)
    return result


def tree_hash(entries):
    data = ''.join(f'{key}\0{entries[key]}\n' for key in sorted(entries)).encode()
    return hashlib.sha256(data).hexdigest()


def artifacts(folder):
    return {p.relative_to(folder).as_posix(): file_hash(p) for p in sorted(folder.rglob('*'))
            if p.is_file() and p.name != 'build-receipt.json'}


def playback_dependencies(root):
    """Rehash the linked engine, including its modules and corresponding sources."""
    work = Path(os.environ.get('STREMIO_VLC_ROOT', str(root / '.deps/vlc')))
    subprocess.run([sys.executable, str(root / 'tools/vlc-build-receipt.py'), '--verify',
                    str(work), 'ps5', str(root / 'vendor/vlc-ps5')],
                   check=True, stdout=subprocess.DEVNULL)
    path = work / 'build-receipt-ps5.json'
    return {'receiptSha256': file_hash(path), 'details': json.loads(path.read_text())}


def release_identity(root, app):
    """Read the build's version once and reject mismatched PS5 metadata."""
    cmake = (root / 'CMakeLists.txt').read_text()
    version_match = re.search(r'project\(stremio-ps5 VERSION (\d+\.\d+\.\d+)', cmake)
    if version_match is None:
        raise SystemExit('Application version missing from CMake')
    if 'STREMIO_VERSION="${PROJECT_VERSION}"' not in cmake:
        raise SystemExit('Compiled application version must come from CMake PROJECT_VERSION')
    version = version_match.group(1)
    title_match = re.search(r'STREMIO_TITLE_ID="([A-Z0-9]+)"', cmake)
    metadata = json.loads((app / 'sce_sys/param.json').read_text())
    localized = metadata.get('localizedParameters', {})
    default = localized.get('defaultLanguage', '')
    if (localized.get(default, {}).get('titleName') != APPLICATION_NAME or
            localized.get('en-US', {}).get('titleName') != APPLICATION_NAME):
        raise SystemExit('Application name differs from the PS5 package metadata')
    if title_match is None or title_match.group(1) != metadata['titleId']:
        raise SystemExit('Compiled title ID differs from PS5 package metadata')
    major, minor, patch = map(int, version.split('.'))
    # Existing PS5 releases encode 0.4.5 as 00.045.000.
    expected_content_version = f'{major:02d}.{minor * 10 + patch:03d}.000'
    if metadata['contentVersion'] != expected_content_version:
        raise SystemExit(f"PS5 contentVersion {metadata['contentVersion']} differs from {version}")
    return version, title_match.group(1), expected_content_version


def main():
    args = sys.argv[1:]
    if args[0] == '--fingerprint':
        print(tree_hash(sources(Path(args[1]))))
        return
    if args[0] == '--verify':
        root, app = map(Path, args[1:])
        receipt = json.loads((app / 'build-receipt.json').read_text())
        version, title_id, content_version = release_identity(root, app)
        if (receipt.get('application') != APPLICATION_NAME or
                receipt.get('applicationVersion') != version or receipt.get('titleId') != title_id or
                receipt.get('contentVersion') != content_version):
            raise SystemExit('Build receipt release identity differs from the compiled app/package metadata.')
        if receipt['sourceTreeSha256'] != tree_hash(sources(root)):
            raise SystemExit('Sources/assets changed since this app was built. Build the native target again.')
        if receipt['artifactsSha256'] != artifacts(app):
            raise SystemExit('Packaged app files changed after signing. Build the native target again.')
        if receipt.get('playbackDependencies') != playback_dependencies(root):
            raise SystemExit('The VLC playback dependencies differ from the packaged application.')
        print('Build receipt verified: current sources, playback dependencies and all packaged files match.')
        return
    root, app, elf, fingerprint = map(Path, args)
    version, title_id, content_version = release_identity(root, app)
    entries = sources(root)
    source_hash = tree_hash(entries)
    if source_hash != fingerprint.read_text().strip():
        raise SystemExit('Sources/assets changed during compilation. Rebuild before packaging.')
    receipt = {
        'application': APPLICATION_NAME, 'applicationVersion': version,
        'titleId': title_id, 'contentVersion': content_version,
        'sourceTreeSha256': source_hash,
        'linkElfSha256': file_hash(elf),
        'compiler': subprocess.check_output(['clang-18', '--version'], text=True).splitlines()[0],
        'toolchain': {
            'sdk': 'v0.42', 'pacbrew': 'v0.40.2', 'opengl': 'v1.0.0',
            'boilerplateCommit': 'f98de734b68b0b980d38ff03a330066afd6ae16d',
            'sdkArchiveSha256': '8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da',
            'portsArchiveSha256': 'a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d',
            'openglManifestSha256': 'f4b91f672be037fbac3f82494f1225deaf4c227a03f37ac3ffa56abb213b943f',
        },
        'sourceFilesSha256': entries,
        'playbackDependencies': playback_dependencies(root),
        'artifactsSha256': artifacts(app),
        'consoleValidation': 'Not performed; compilation and package validation only.',
    }
    (app / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
