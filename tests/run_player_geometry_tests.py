#!/usr/bin/env python3
"""Check real VLC coded/visible output, full pixel hashes and retained leases.

The optional nonzero SPS crop diagnostic intentionally reports a known failure
in the pre-existing decoder path. It is not part of the successful geometry
regression set and is never relabeled as a pass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
parser.add_argument('--include-known-crop', action='store_true',
                    help='Also diagnose nonzero SPS crop; returns failure while that known decoder limitation remains')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build.resolve()
link = shlex.split((build / 'CMakeFiles/stremio.dir/link.txt').read_text())
flagtext = (build / 'CMakeFiles/stremio.dir/flags.make').read_text()
flags = []
for name in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    flags += shlex.split(re.search('^' + name + r' = (.*)$', flagtext, re.MULTILINE)[1])
if not any('STREMIO_USE_LIBVLC' in flag for flag in flags):
    raise SystemExit('Requires the completed static VLC host application build')
output = root / 'build/tests/player-geometry'
output.mkdir(parents=True, exist_ok=True)
commands = []


def run(command, **kwargs):
    commands.append(list(map(str, command)))
    return subprocess.run(list(map(str, command)), **kwargs)


obj, binary = output / 'test.o', output / 'test'
run([link[0], *flags, '-O1', '-g', '-c', root / 'tests/test_player_geometry.cpp', '-o', obj], check=True)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
link[link.index('-o') + 1] = str(binary)
for symbol in ('libvlc_new', 'libvlc_log_set', 'libvlc_video_set_format_callbacks',
               'sws_scale', 'vlc_ps5_copy_plane', 'picture_CopyPixels', 'var_SetChecked'):
    link.append('-Wl,--wrap=' + symbol)
run(link, cwd=build, check=True)
cases = [
    ('h264-720', '1280x720', 'libx264', 'yuv420p', ''),
    ('h264-1080', '1920x1080', 'libx264', 'yuv420p', ''),
    ('hevc-1080-main10', '1920x1080', 'libx265', 'yuv420p10le', ''),
    ('h264-1080-nv12', '1920x1080', 'libx264', 'nv12', 'NV12'),
    ('hevc-1080-p010', '1920x1080', 'libx265', 'p010le', 'P010')]
if args.include_known_crop:
    cases.append(('h264-nonzero-sps-crop', '1280x720', 'libx264', 'yuv420p', ''))
runs = {}
with tempfile.TemporaryDirectory(prefix='stremio-geometry-') as temporary:
    folder = Path(temporary)
    for name, size, codec, pixels, force in cases:
        ten = pixels in ('p010le', 'yuv420p10le')
        fixture = folder / ('main10-' + size + '.mkv' if ten else size + '.mkv')
        if not fixture.exists():
            command = ['ffmpeg', '-nostdin', '-v', 'error', '-y', '-filter_threads', '1', '-f', 'lavfi', '-i',
                f'testsrc2=size={size}:rate=24:duration=3', '-c:v', codec, '-threads', '2', '-preset',
                'ultrafast', '-pix_fmt', 'yuv420p10le' if ten else 'yuv420p']
            if ten:
                command += ['-x265-params', 'pools=2:frame-threads=2:log-level=error']
            run([*command, fixture], capture_output=True, check=True, timeout=90)
        if name == 'h264-nonzero-sps-crop':
            cropped = folder / 'cropped.mkv'
            run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', fixture, '-c:v', 'copy',
                 '-bsf:v', 'h264_metadata=crop_left=16:crop_right=16:crop_top=8:crop_bottom=8', cropped],
                check=True, capture_output=True, timeout=30)
            fixture = cropped
            # Decode full coded picture, then apply the exact SPS rectangle.
            # The default FFmpeg cropping policy may round a 16-pixel left
            # origin for alignment; comparing that different width is invalid.
            ref_input = ['-apply_cropping', '0', '-i', fixture, '-vf', 'crop=1248:704:16:8:exact=1']
        else:
            ref_input = ['-i', fixture]
        reference = run(['ffmpeg', '-nostdin', '-v', 'error', *ref_input, '-map', '0:v:0', '-pix_fmt', pixels,
                         '-f', 'framehash', '-hash', 'sha256', '-'], capture_output=True, text=True, check=True, timeout=45)
        hashes = output / (name + '-reference.txt')
        hashes.write_text(''.join(line.rsplit(',', 1)[-1].strip() + '\n' for line in reference.stdout.splitlines()
                                  if line and not line.startswith('#')))
        profile = folder / name
        profile.mkdir()
        result = run([binary, profile, root / 'app', fixture, hashes, force], capture_output=True, text=True,
                     timeout=12, env=dict(os.environ, SDL_AUDIODRIVER='dummy'))
        (output / (name + '.log')).write_text(result.stdout + result.stderr)
        line = next((v for v in result.stdout.splitlines() if v.startswith('GEOMETRY_RESULT ')), None)
        if line is None:
            raise RuntimeError(f'{name}: no output report; exit {result.returncode}')
        data = json.loads(line[len('GEOMETRY_RESULT '):])
        data['exit_code'] = result.returncode
        data['fixture_sha256'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
        data['reference_sha256'] = hashlib.sha256(hashes.read_bytes()).hexdigest()
        data['forced_output_chroma'] = force or None
        data['known_preexisting_crop_diagnostic'] = name == 'h264-nonzero-sps-crop'
        if not force and not data['known_preexisting_crop_diagnostic']:
            data['avoids_padding_conversion'] = data['adapt_filters'] == 0 and data['picture_copies'] == 0
        else:
            data['avoids_padding_conversion'] = None
        data['result'] = 'PASS' if result.returncode == 0 and data['avoids_padding_conversion'] is not False else 'FAIL'
        runs[name] = data
        print(name, {k: v for k, v in data.items() if k != 'observed_hashes'}, flush=True)
success = all(case['result'] == 'PASS' for case in runs.values())
report = {'result': 'PASS' if success else 'FAIL', 'cases': runs,
          'scope': 'Real Player/static host VLC; H264 and HEVC Main10; full visible-pixel SHA256 and retained leases',
          'forced_chroma_scope': 'NV12/P010 requested only at test vmem boundary; real software conversion, not PS5 hardware decode',
          'limits': 'No PS5 device, firmware 12.7 or physical HDMI cadence is exercised. No performance/FPS speedup is inferred from copy counts.',
          'known_crop': 'Optional nonzero SPS crop is a pre-existing decoder limitation, explicitly FAIL while present.',
          'source_sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                           (root / 'src/player_vlc.cpp', root / 'vendor/vlc-ps5/patches/0025-vmem-ps5-keep-visible.patch',
                            root / 'tests/test_player_geometry.cpp', Path(__file__).resolve())},
          'commands': commands}
(output / ('result-with-crop.json' if args.include_known_crop else 'result.json')).write_text(json.dumps(report, indent=2) + '\n')
raise SystemExit(0 if success else 1)
