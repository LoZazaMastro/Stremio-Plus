#!/usr/bin/env python3
"""Restore verified Stremio Plus build inputs from the adjacent local bundle.

No network, source edits, new commits, branches, or compilation. Run with
CPython 3.12 on Linux x86_64, with git and python3-venv already installed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

PREFIX = 'Stremio-Plus-0.5.19-dependencies'
BP_REV = 'f98de734b68b0b980d38ff03a330066afd6ae16d'
BP_TREE = '723111f8b760d912f975edce4a821085c1167c2a'
MKPFS_REV = '6cb8313dfe0c988ac52617794553f343243d3a56'
MKPFS_TREE = '034378333170989698f7428c322e58b582c68604'


def fail(message):
    raise RuntimeError(message)


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_file(path, entry):
    if not path.is_file():
        fail(f'Missing dependency file: {path}')
    if path.stat().st_size != entry['bytes'] or sha256(path) != entry['sha256']:
        fail(f'Checksum/size mismatch: {path}')


def run(command, env=None):
    result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=180)
    if result.returncode:
        fail(f'Command failed ({result.returncode}): {command!r}\n{result.stdout}{result.stderr}')
    return result.stdout.strip()


def git(path, *arguments):
    return run(['git', '-c', 'core.hooksPath=/dev/null', '-C', str(path), *arguments])


def safe_target(root, relative):
    relative = Path(relative)
    if relative.is_absolute() or '..' in relative.parts:
        fail(f'Invalid target: {relative}')
    current = root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            fail(f'Restore requires real directories/files inside the source tree; found symlink: {current}')
    return current


def inspect_git(path, revision, tree):
    if not path.exists():
        return False
    if (path / '.git').is_symlink():
        fail(f'Dependency Git metadata must be local: {path}')
    if not (path / '.git').is_dir():
        if any(path.iterdir()):
            fail(f'Dependency directory exists without Git metadata: {path}')
        return False
    if git(path, 'rev-parse', 'HEAD') != revision or git(path, 'rev-parse', 'HEAD^{tree}') != tree:
        fail(f'Existing dependency has a different commit/tree: {path}')
    attached = subprocess.run(['git', '-C', str(path), 'symbolic-ref', '-q', 'HEAD'],
                              text=True, capture_output=True, timeout=30)
    if attached.returncode == 0:
        fail(f'Existing dependency must be detached before restore: {path}')
    git(path, 'diff', '--quiet')
    git(path, 'diff', '--cached', '--quiet')
    return True


def copy_verified(source, destination, expected):
    if destination.exists():
        verify_file(destination, expected)
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='stremio-restore-', dir=destination.parent)
    os.close(descriptor)
    temporary = Path(temporary)
    try:
        shutil.copyfile(source, temporary)
        verify_file(temporary, expected)
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
    finally:
        if temporary.exists():
            temporary.unlink()
    return True


def restore_git(path, bundle, boundary, revision, tree):
    if inspect_git(path, revision, tree):
        return False
    path.mkdir(parents=True, exist_ok=True)
    git(path, 'init', '--quiet')
    # Preserve the actual upstream shallow boundary; the bundle does not
    # contain parent history. Without this file Git correctly rejects fetch.
    shutil.copyfile(boundary, path / '.git/shallow')
    git(path, 'fetch', '--no-tags', str(bundle), 'HEAD')
    git(path, 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
    inspect_git(path, revision, tree)
    git(path, 'fsck', '--no-reflogs', '--connectivity-only')
    if git(path, 'for-each-ref', '--format=%(refname)', 'refs/heads'):
        fail(f'Restore unexpectedly created a branch: {path}')
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repository', type=Path, help='Fresh extracted Stremio Plus source directory')
    arguments = parser.parse_args()
    root = arguments.repository.resolve()
    bundle_root = Path(__file__).resolve().parent
    if not root.is_dir():
        fail(f'Source directory does not exist: {root}')
    if sys.platform != 'linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        fail('Included native Python wheels target Linux x86_64.')
    if sys.version_info[:2] != (3, 12):
        fail('Run this helper with Python 3.12; the included native wheels use CPython 3.12.')
    for tool in ('git', 'bash'):
        if not shutil.which(tool):
            fail(f'Required host tool is not installed: {tool}')
    for marker in ('tools/setup-toolchain.sh', 'tools/setup-vlc.sh', 'vendor/vlc-ps5/sources.json'):
        if not (root / marker).is_file():
            fail(f'Not the supplied Stremio Plus source directory: missing {marker}')

    inventory = json.loads((bundle_root / 'inventory.json').read_text())
    if inventory.get('schema_version') != 1 or inventory.get('result') != 'PASS':
        fail('Unsupported or unsuccessful dependency inventory.')
    available = {}
    media = {}
    for entry in inventory['files']:
        if entry['include_in_dependency_zip']:
            member = Path(entry['archive_member'])
            if member.is_absolute() or '..' in member.parts or member.parts[0] != PREFIX:
                fail(f'Invalid archive path in inventory: {member}')
            path = bundle_root.joinpath(*member.parts[1:])
            if not path.resolve().is_relative_to(bundle_root):
                fail(f'Dependency file resolves outside the bundle: {path}')
        elif entry['group'] == 'application_source':
            path = root / entry['archive_member']
        else:
            continue
        verify_file(path, entry)
        if entry['filename'] in available:
            fail(f'Ambiguous dependency filename in inventory: {entry["filename"]}')
        available[entry['filename']] = (path, entry)
        if entry.get('name') and entry.get('source_url') and entry['kind'] == 'source_archive':
            media[entry['filename']] = entry['sha256']
    declared_media = {item['filename']: item['sha256'] for item in
                      json.loads((root / 'vendor/vlc-ps5/sources.json').read_text())['sources']}
    if media != declared_media or len(media) != 18:
        fail('Application media manifest does not match this dependency bundle.')

    # Plan every write and reject existing mismatches before creating caches.
    copies = []
    for name, (source, entry) in available.items():
        if name in media:
            destination = safe_target(root, '.deps/vlc/dl/' + name)
        elif '/downloads/' in entry['archive_member']:
            destination = safe_target(root, '.deps/downloads/' + name)
        else:
            continue
        copies.append((source, destination, entry))
    bp = safe_target(root, '.deps/ps5-native-app-boilerplate')
    mkpfs = safe_target(root, '.deps/ps5-native-app-boilerplate/.deps/MkPFS')
    git_specs = [(bp, 'ps5-native-app-boilerplate-f98de734b68b.bundle', BP_REV, BP_TREE),
                 (mkpfs, 'MkPFS-6cb8313dfe0c.bundle', MKPFS_REV, MKPFS_TREE)]
    for target, filename, revision, tree in git_specs:
        inspect_git(target, revision, tree)
        if filename not in available or filename + '.shallow' not in available:
            fail(f'Missing Git bundle or original boundary: {filename}')
        boundary = available[filename + '.shallow'][0]
        if boundary.read_text().strip() != revision:
            fail(f'Incorrect original shallow boundary: {boundary}')
    zlib_source, zlib_entry = available['zlib-1.3.2.tar.gz']
    copies.append((zlib_source,
                   safe_target(root, '.deps/ps5-native-app-boilerplate/.deps/native/zlib/zlib-1.3.2.tar.gz'),
                   zlib_entry))
    for _, target, entry in copies:
        if target.exists():
            verify_file(target, entry)
    venv = safe_target(root, '.deps/ps5-native-app-boilerplate/.deps/MkPFS/.venv-linux')
    if venv.exists() and not (venv / 'bin/python').is_file():
        fail(f'Existing incomplete MkPFS virtual environment: {venv}')
    safe_target(root, '.deps/ps5-native-app-boilerplate/.deps/MkPFS/.venv-linux/.boilerplate-revision')
    safe_target(root, '.deps/ps5-native-app-boilerplate/.deps/MkPFS/.mkpfs-run-linux')
    safe_target(root, '.deps/offline-cache-restore.json')

    print(f'Verified {len(available)} files before restoring cache.', flush=True)
    restored_repositories = 0
    for target, filename, revision, tree in git_specs:
        restored_repositories += restore_git(target, available[filename][0],
                                             available[filename + '.shallow'][0], revision, tree)
    copied_files = sum(copy_verified(source, destination, entry) for source, destination, entry in copies)

    # Venvs have absolute paths: build a fresh one instead of shipping a copied
    # environment. Every wheel and the exact requirements were checked above.
    if not venv.exists():
        run([sys.executable, '-m', 'venv', str(venv)])
    python = venv / 'bin/python'
    environment = dict(os.environ, PIP_NO_INDEX='1', PIP_DISABLE_PIP_VERSION_CHECK='1', PIP_NO_CACHE_DIR='1')
    wheels = bundle_root / 'packaging-wheels'
    run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', '--no-index',
         '--find-links', str(wheels), '-r', str(wheels / 'requirements-offline.txt')], environment)
    run([str(python), '-m', 'pip', 'check'], environment)
    run([str(python), '-m', 'mkpfs', '--help'], environment)
    (venv / '.boilerplate-revision').write_text(MKPFS_REV + '\n')
    runner = run(['bash', str(bp / 'tools/setup-packaging-dependencies.sh'), 'ffpfsc'], environment)
    result = dict(result='PASS', source_root=str(root), verified_input_files=len(available),
                  copied_cache_files=copied_files, restored_git_repositories=restored_repositories,
                  boilerplate_commit=BP_REV, mkpfs_commit=MKPFS_REV,
                  packaging_runner=runner, network_downloads=0, new_commits=0, new_branches=0,
                  scope='Dependency input cache and packaging environment; no application/media compilation')
    (root / '.deps/offline-cache-restore.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f'Restore failed: {error}', file=sys.stderr)
        raise SystemExit(1)
