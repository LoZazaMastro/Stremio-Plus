#!/usr/bin/env python3
"""Read immutable local dependency inputs; emit a deduplicated delivery plan."""
import collections
import datetime
import hashlib
import json
from pathlib import Path
import re

ROOT = Path('/workspace/scratch/7fa7f080734e/Stremio-Plus-work')
OUT = ROOT.parent / 'validation-current'
FILES = []


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def add(path, kind, group, destination, expected=None, **extra):
    path = Path(path)
    observed = digest(path)
    if expected and observed != expected:
        raise RuntimeError(f'Checksum mismatch: {path}')
    entry = dict(source_path=str(path), resolved_path=str(path.resolve()),
                 filename=path.name, bytes=path.stat().st_size, sha256=observed,
                 kind=kind, group=group, archive_member=destination,
                 include_in_dependency_zip=group in ('core', 'pacbrew'))
    if expected:
        entry.update(expected_sha256=expected, checksum_matches_pin=True)
    entry.update(extra)
    FILES.append(entry)
    return entry


download = ROOT / '.deps/downloads'
add(download / 'ps5-payload-sdk-v0.42.zip', 'binary_sdk_with_headers_and_examples',
    'core', 'Stremio-Plus-0.5.19-dependencies/downloads/ps5-payload-sdk-v0.42.zip',
    '8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da',
    version='v0.42', complete_library_sources_in_archive=False,
    source_url='https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip')
add(download / 'ps5-opengl-sdk-1.0.0.tar.gz', 'binary_sdk_and_complete_matching_source_snapshots',
    'core', 'Stremio-Plus-0.5.19-dependencies/downloads/ps5-opengl-sdk-1.0.0.tar.gz',
    'f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5',
    version='1.0.0', source_commit='dce74910af7f4274c48d05e0ae001b3038462588',
    sdk_manifest_sha256='f4b91f672be037fbac3f82494f1225deaf4c227a03f37ac3ffa56abb213b943f',
    source_url='https://github.com/MyLuxy/overdrive-deps/releases/download/ps5-opengl-v1.0.0/ps5-opengl-sdk-1.0.0.tar.gz',
    embedded_sources=['ps5-opengl', 'Mesa 26.2.0', 'OpenGNM', 'OpenGNM-PSBC',
                      'SPIRV-Headers', 'Vulkan-Headers', 'ImGui', 'NanoVG', 'Sokol', 'Sokol samples'],
    archive_contents_inspected=True)
add(download / 'ps5-payload-dev-v0.40.2.tar.gz', 'binary_library_packages_with_headers',
    'pacbrew', 'Stremio-Plus-0.5.19-dependencies/downloads/ps5-payload-dev-v0.40.2.tar.gz',
    'a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d',
    version='v0.40.2', complete_library_sources_in_archive=False,
    source_url='https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz',
    installation_scope='Only opt/ps5-payload-sdk/target/user/homebrew; retain SDK v0.42')
zlib = add(download / 'zlib-1.3.2.tar.gz', 'source_archive', 'core',
           'Stremio-Plus-0.5.19-dependencies/downloads/zlib-1.3.2.tar.gz',
           'bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16',
           source_url='https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz',
           purpose='Host native converter; distinct from media zlib 1.3.1')
zlib_copy = ROOT / '.deps/ps5-native-app-boilerplate/.deps/native/zlib/zlib-1.3.2.tar.gz'
assert digest(zlib_copy) == zlib['sha256']
zlib['duplicate_copies_excluded'] = [str(zlib_copy)]

media_sources = json.loads((ROOT / 'vendor/vlc-ps5/sources.json').read_text())['sources']
for source in media_sources:
    name = source['filename']
    in_repository = (ROOT / 'source-archives' / name).is_file()
    group = 'application_source' if in_repository else 'core'
    member = ('source-archives/' if in_repository else 'Stremio-Plus-0.5.19-dependencies/media/') + name
    entry = add(ROOT / '.deps/vlc/dl' / name, 'source_archive', group, member,
                source['sha256'], name=source['name'], version=source['version'],
                license=source['license'], build_tool_only=source['build_tool_only'],
                source_url=source['url'])
    copies = [ROOT / '.deps/vlc-host/dl' / name]
    if in_repository:
        copies.append(ROOT / 'source-archives' / name)
        entry['delivery_source_path'] = str(ROOT / 'source-archives' / name)
        entry['delivery_note'] = 'Already in application source; do not duplicate in dependency ZIP.'
    for copy in copies:
        assert digest(copy) == entry['sha256']
    entry['duplicate_copies_excluded'] = [str(copy) for copy in copies]

git_tests = json.loads((OUT / 'offline-git-bundles.json').read_text())
for result in git_tests:
    assert result['result'] == 'PASS'
    bundle = Path(result['path'])
    shallow = Path(result['required_shallow_file'])
    add(bundle, 'git_source_snapshot_bundle', 'core', 'Stremio-Plus-0.5.19-dependencies/git/' + bundle.name,
        result['sha256'], commit=result['actual_commit'], tree=result['actual_tree'],
        shallow_boundary_required=shallow.name, standalone_without_boundary=False,
        offline_detached_restore_test='PASS')
    add(shallow, 'original_git_shallow_boundary_metadata', 'core',
        'Stremio-Plus-0.5.19-dependencies/git/' + shallow.name, result['shallow_sha256'])

wheels = download / 'mkpfs-python-wheels'
wheel_manifest = {e['file']: e for e in json.loads((wheels / 'wheel-manifest.json').read_text())}
for name, metadata in sorted(wheel_manifest.items()):
    kind = 'python_wheel_pure' if name.endswith('none-any.whl') else 'python_wheel_native_binary'
    add(wheels / name, kind, 'core', 'Stremio-Plus-0.5.19-dependencies/packaging-wheels/' + name,
        metadata['sha256'], host='CPython 3.12 / Linux x86_64 (Ubuntu 24.04 environment)')
for name in ('requirements-offline.txt', 'SHA256SUMS', 'wheel-manifest.json'):
    add(wheels / name, 'wheel_installation_metadata', 'core', 'Stremio-Plus-0.5.19-dependencies/packaging-wheels/' + name)

codeload = OUT / 'boilerplate-probe-drakmor.tar.gz'
alternative = dict(path=str(codeload), bytes=codeload.stat().st_size, sha256=digest(codeload),
                   commit='f98de734b68b0b980d38ff03a330066afd6ae16d',
                   included=False,
                   reason='Same source already in verified Git bundle; archive lacks Git metadata required by setup.')
assert alternative['sha256'] == 'da57cd98c1a41d0c26dfc239d747db0387fabd932a6b20b2a7cf1d859615eaff'

known_hashes = {entry['sha256'] for entry in FILES}
source_gaps = []
for line in (ROOT / 'docs/DEPENDENCY_SOURCES.md').read_text().splitlines():
    if not line.startswith('|') or 'ps5-native-app-boilerplate' in line:
        continue
    hash_match = re.search(r'`([a-f0-9]{64})`', line)
    link_match = re.search(r'\[[^]]+\]\((https?://[^)]+)\)', line)
    if hash_match and link_match and hash_match[1] not in known_hashes:
        source_gaps.append(dict(component=line.split('|')[1].strip(), url=link_match[1],
                                expected_sha256=hash_match[1], cached=False))

groups = []
selected_hashes = set()
for group, filename in [('core', 'Stremio-Plus-0.5.19-dependencies-core.zip'),
                        ('pacbrew', 'Stremio-Plus-0.5.19-dependencies-pacbrew.zip')]:
    selected = [entry for entry in FILES if entry['group'] == group]
    for entry in selected:
        assert entry['sha256'] not in selected_hashes, 'Duplicate bytes in delivery plan'
        selected_hashes.add(entry['sha256'])
    size = sum(entry['bytes'] for entry in selected)
    assert size < 450_000_000
    groups.append(dict(group=group, proposed_zip=filename, member_count=len(selected),
                       input_bytes=size, input_decimal_mb=round(size / 1_000_000, 3),
                       maximum_zip_bytes=500_000_000, zip_created=False,
                       note='Store the already compressed archives as files; materialize symlink targets.',
                       archive_members=[entry['archive_member'] for entry in selected]))

host_receipt_path = ROOT / '.deps/vlc-host/build-receipt-host.json'
host_receipt = json.loads(host_receipt_path.read_text())
host_files = []
for item in host_receipt['archives']:
    path = ROOT / '.deps/vlc-host' / item['path']
    assert path.stat().st_size == item['bytes'] and digest(path) == item['sha256']
    host_files.append(dict(source_path=str(path), resolved_path=str(path.resolve()),
                           relative_path=item['path'], bytes=item['bytes'], sha256=item['sha256']))
host_bytes = sum(entry['bytes'] for entry in host_files)
host_binary = dict(target='host', result=host_receipt['result'],
                   receipt=str(host_receipt_path), receipt_sha256=digest(host_receipt_path),
                   archive_count=len(host_files), static_archive_bytes=host_bytes,
                   checksum_verification='PASS', archives=host_files,
                   included_in_input_cache=False, relocatable_as_is=False,
                   reason='Generated config, pkg-config files and static link response contain absolute build paths; rebuild from cached source after relocation.')

native_receipt_path = ROOT / '.deps/vlc/build-receipt-ps5.json'
native_binary = dict(status='Compilation in progress; do not snapshot mutable outputs',
                     future_receipt=str(native_receipt_path))
if native_receipt_path.is_file():
    native_receipt = json.loads(native_receipt_path.read_text())
    native_files = []
    for item in native_receipt['archives']:
        path = ROOT / '.deps/vlc' / item['path']
        assert path.stat().st_size == item['bytes'] and digest(path) == item['sha256']
        native_files.append(dict(source_path=str(path), resolved_path=str(path.resolve()),
                                 relative_path=item['path'], bytes=item['bytes'], sha256=item['sha256']))
    native_binary = dict(status='Available and checked against completed native VLC receipt',
                         target='ps5', result=native_receipt['result'],
                         receipt=str(native_receipt_path), receipt_sha256=digest(native_receipt_path),
                         archive_count=len(native_files), static_archive_bytes=sum(f['bytes'] for f in native_files),
                         checksum_verification='PASS', archives=native_files,
                         included_in_input_cache=False, relocatable_as_is=False,
                         reason='These are compiled libraries from this build. Generated config and link response contain absolute paths; the offline input cache rebuilds them for the new source location.')

report = dict(schema_version=1, generated_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              root=str(ROOT), result='PASS', scope='Available immutable dependency inputs and proposed local delivery, no ZIP creation',
              input_archive_hashes_checked=True, no_remote_mutations=True, no_new_git_branches_or_commits=True,
              file_count=len(FILES), files=FILES, proposed_groups=groups,
              dependency_zip_input_bytes=sum(group['input_bytes'] for group in groups),
              media_source_archive_count=len(media_sources),
              media_source_archive_bytes=sum((ROOT / '.deps/vlc/dl' / e['filename']).stat().st_size for e in media_sources),
              source_archives_delivered_in_application=['source-archives/nasm-2.16.03.tar.xz', 'source-archives/freetype-2.13.3.tar.xz'],
              excluded_alternative_source_archive=alternative,
              sources_documented_but_not_cached=source_gaps,
              available_host_binary_cache=host_binary,
              native_binary_cache=native_binary,
              offline_restore_tests=dict(git_bundles='offline-git-bundles.json', wheels='offline-mkpfs-wheels.json'),
              exclusions=['Partial/temporary downloads', 'Duplicate host media archive cache',
                          'Duplicate OpenGL/SDK/PacBrew extracted trees already inside original archives',
                          'Build directories and compiler cache', 'Copied virtual environments with absolute paths'],
              limits=['Host compiler/build/system packages must already be installed as documented in BUILDING.md; no OS package cache is included.',
                      'SDK and PacBrew inputs are verified binary packages, not a full offline source rebuild of those packages.',
                      'Git bundles preserve original shallow snapshots and require the original companion boundary metadata.',
                      'Compiled media libraries are inventoried separately; they are not a preconfigured relocatable build directory.'])
(OUT / 'dependency-inventory.json').write_text(json.dumps(report, indent=2) + '\n')
selected = [entry for entry in FILES if entry['include_in_dependency_zip']]
(OUT / 'dependency-cache-SHA256SUMS').write_text(''.join(
    f"{entry['sha256']}  {entry['archive_member'].removeprefix('Stremio-Plus-0.5.19-dependencies/')}\n"
    for entry in sorted(selected, key=lambda entry: entry['archive_member'])))
print(json.dumps(dict(result=report['result'], file_count=len(FILES), groups=groups,
                      missing_source_archives=len(source_gaps)), indent=2))
