#!/usr/bin/env python3
"""Record and revalidate the complete static playback engine before packaging."""
import hashlib
import json
from pathlib import Path
import re
import sys


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_archive(path):
    """Reject truncated archives and empty object files that ar accepts silently."""
    length = path.stat().st_size
    with path.open('rb') as stream:
        if stream.read(8) != b'!<arch>\n':
            raise SystemExit(f'Not a regular static archive: {path}')
        while stream.tell() < length:
            header = stream.read(60)
            if len(header) != 60 or header[-2:] != b'`\n':
                raise SystemExit(f'Invalid archive member header: {path}')
            try:
                size = int(header[48:58].strip())
            except ValueError:
                raise SystemExit(f'Invalid archive member size: {path}')
            name = header[:16].decode('ascii', 'replace').strip()
            extended = int(name[3:]) if name.startswith('#1/') else 0
            if size < extended or stream.tell() + size > length:
                raise SystemExit(f'Truncated archive member {name}: {path}')
            if size == extended and name not in ('/', '//', '/SYM64/'):
                raise SystemExit(f'Empty object member {name}: {path}')
            stream.seek(size + size % 2, 1)
        if stream.tell() != length:
            raise SystemExit(f'Invalid archive padding: {path}')


def snapshot(work, target, vendor):
    sources = []
    for line in (vendor / 'sources.sha256').read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        expected, name = line.split(None, 1)
        archive = work / 'dl' / name.strip()
        actual = digest(archive)
        if actual != expected:
            raise SystemExit(f'Source checksum mismatch: {name}')
        sources.append({'filename': archive.name, 'sha256': actual, 'bytes': archive.stat().st_size})
    prefix = work / f'prefix-{target}'
    build = work / f'build-vlc-{target}'
    static = work / f'static-{target}'
    archives = [build / 'lib/.libs/libvlc.a', build / 'src/.libs/libvlccore.a']
    archives += sorted((prefix / 'lib').glob('*.a'))
    archives += [Path(line) for line in (static / 'link.rsp').read_text().splitlines()
                 if line.endswith('.a') and '/modules/' in line]
    archive_records = []
    for path in archives:
        validate_archive(path)
        archive_records.append({'path': path.relative_to(work).as_posix(),
                                'sha256': digest(path), 'bytes': path.stat().st_size})
    plugins = [line.strip() for line in (vendor / 'plugins.txt').read_text().splitlines()
               if line.strip() and not line.lstrip().startswith('#')]
    return {
        'result': 'PASS', 'target': target, 'vlc_version': '3.0.24', 'ffmpeg_version': '8.1.2',
        'sources': sources,
        'patches': [{'path': p.relative_to(vendor).as_posix(), 'sha256': digest(p)}
                    for p in sorted((vendor / 'patches').rglob('*.patch'))],
        'plugins': plugins,
        'registered_module_entries': sorted(set(re.findall(
            r'vlc_entry__[A-Za-z0-9_]+', (static / 'static_modules.c').read_text()))),
        'archives': archive_records,
        'vlc_config_sha256': digest(build / 'config.h'),
        'static_modules_sha256': digest(static / 'static_modules.c'),
        'link_response_sha256': digest(static / 'link.rsp'),
        'ffmpeg_build_configuration': (work / 'src/ffmpeg-8.1.2/ffbuild/config.mak').read_text(),
    }


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--check-build':
        directory = Path(args[1])
        archives = sorted(directory.rglob('*.a'))
        if not archives:
            raise SystemExit(f'No static archives built in {directory}')
        for path in archives:
            validate_archive(path)
        objects = sorted(directory.rglob('*.o'))
        for path in objects:
            if path.stat().st_size == 0:
                raise SystemExit(f'Empty compiled object: {path}')
        print(f'Build integrity: {len(archives)} archives and {len(objects)} objects checked')
        return
    verify = bool(args and args[0] == '--verify')
    if verify:
        args = args[1:]
    if len(args) != 3 or args[1] not in ('ps5', 'host'):
        raise SystemExit('Usage: vlc-build-receipt.py [--verify] WORK {ps5|host} VENDOR')
    work, target, vendor = Path(args[0]), args[1], Path(args[2])
    receipt = snapshot(work, target, vendor)
    destination = work / f'build-receipt-{target}.json'
    if verify:
        if receipt != json.loads(destination.read_text()):
            raise SystemExit('VLC sources, patches, libraries or module selection changed since the engine was built.')
    else:
        destination.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f"VLC {target}: {len(receipt['plugins'])} plugins and {len(receipt['archives'])} static archives verified")


if __name__ == '__main__':
    main()
