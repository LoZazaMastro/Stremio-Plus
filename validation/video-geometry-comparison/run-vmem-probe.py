#!/usr/bin/env python3
import json, os, re, shlex, subprocess, hashlib
from pathlib import Path
here = Path(__file__).resolve().parent
root = here.parents[1] / 'Stremio-Plus-work'
build = root / 'build/host-vlc'
link = shlex.split((build / 'CMakeFiles/stremio.dir/link.txt').read_text())
flags = []
flagtext = (build / 'CMakeFiles/stremio.dir/flags.make').read_text()
for name in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    flags += shlex.split(re.search('^' + name + r' = (.*)$', flagtext, re.M)[1])
obj = here / 'vmem-probe.o'
subprocess.run([link[0], *flags, '-O1', '-g', '-c', str(here / 'vmem-probe.cpp'), '-o', str(obj)], check=True)
link[link.index('CMakeFiles/stremio.dir/src/main.cpp.o')] = str(obj)
for sym in ('libvlc_new', 'libvlc_log_set', 'libvlc_video_set_format_callbacks', 'sws_scale', 'vlc_ps5_copy_plane', 'picture_CopyPixels', 'var_SetChecked'):
    link.append('-Wl,--wrap=' + sym)
candidate = here / 'candidate'
player_obj = candidate / 'player_vlc.o'
subprocess.run([link[0], *flags, '-O2', '-g', '-c', str(candidate / 'src/player_vlc.cpp'), '-o', str(player_obj)], check=True)
vlcbase = root / '.deps/vlc-host'
source = vlcbase / 'src/vlc-3.0.24'
vbuild = vlcbase / 'build-vlc-host'
vmem_obj = candidate / 'vmem.o'
cc = ['gcc', '-DHAVE_CONFIG_H', '-DMODULE_STRING="vmem"', '-DMODULE_NAME=vmem',
    '-I' + str(vbuild / 'modules'), '-I' + str(source / 'modules'), '-I' + str(vbuild),
    '-I' + str(source / 'modules/access'), '-I' + str(source / 'modules/codec'),
    '-I' + str(source / 'include'), '-I' + str(vbuild / 'include'),
    '-I' + str(vlcbase / 'prefix-host/include'), '-I' + str(source / 'compat/stdckdint'),
    '-O2', '-g', '-fPIC', '-c', str(candidate / 'engine/modules/video_output/vmem.c'), '-o', str(vmem_obj)]
subprocess.run(cc, check=True)
runs = {}
for mode in ('baseline', 'candidate'):
    binary = here / ('vmem-probe-' + mode)
    mode_link = list(link)
    mode_link[mode_link.index('-o') + 1] = str(binary)
    vm = next(i for i, a in enumerate(mode_link) if a.endswith('/libvmem_plugin.a'))
    if mode == 'candidate':
        mode_link[mode_link.index('CMakeFiles/stremio.dir/src/player_vlc.cpp.o')] = str(player_obj)
        mode_link[vm] = str(vmem_obj)
    else:
        mode_link[mode_link.index('CMakeFiles/stremio.dir/src/player_vlc.cpp.o')] = str(here / 'baseline/player_vlc.cpp.o')
        mode_link[vm] = str(here / 'baseline/libvmem_plugin.a')
    subprocess.run(mode_link, cwd=build, check=True)
    runs[mode] = {}
    cases = [
        ('h264-720', '1280x720', 'libx264', 'yuv420p', ''),
        ('h264-1080', '1920x1080', 'libx264', 'yuv420p', ''),
        ('hevc-1080-main10', '1920x1080', 'libx265', 'yuv420p10le', ''),
        ('h264-1080-nv12', '1920x1080', 'libx264', 'nv12', 'NV12'),
        ('hevc-1080-p010', '1920x1080', 'libx265', 'p010le', 'P010'),
        ('h264-cropped', '1280x720', 'libx264', 'yuv420p', '')]
    for name, size, codec, pixels, force in cases:
        ten = pixels in ('p010le', 'yuv420p10le')
        fixture = here / ('probe-' + ('main10-' if ten else '') + size + '.mkv')
        if not fixture.exists():
            command = ['ffmpeg', '-nostdin', '-v', 'error', '-y', '-filter_threads', '1', '-f', 'lavfi', '-i',
                f'testsrc2=size={size}:rate=24:duration=3', '-c:v', 'libx264', '-threads', '2', '-preset',
                'ultrafast', '-pix_fmt', 'yuv420p10le' if ten else 'yuv420p']
            command[command.index('-c:v') + 1] = codec
            if ten: command += ['-x265-params', 'pools=2:frame-threads=2:log-level=error']
            subprocess.run([*command, str(fixture)], check=True, timeout=60, capture_output=True)
        if name == 'h264-cropped':
            cropped = here / 'probe-cropped.mkv'
            if not cropped.exists():
                subprocess.run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-i', str(fixture), '-c:v', 'copy',
                    '-bsf:v', 'h264_metadata=crop_left=16:crop_right=16:crop_top=8:crop_bottom=8', str(cropped)], check=True)
            fixture = cropped
        hashes = here / ('reference-h264-cropped-exact.txt' if name == 'h264-cropped' else 'reference-' + name + '.txt')
        if not hashes.exists():
            reference_input = ['-apply_cropping', '0', '-i', str(fixture), '-vf', 'crop=1248:704:16:8:exact=1'] if name == 'h264-cropped' else ['-i', str(fixture)]
            ref = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', *reference_input, '-map', '0:v:0',
                '-pix_fmt', pixels, '-f', 'framehash', '-hash', 'sha256', '-'], capture_output=True, text=True, check=True, timeout=30)
            hashes.write_text(''.join(line.rsplit(',', 1)[-1].strip() + '\n' for line in ref.stdout.splitlines()
                if line and not line.startswith('#')))
        profile = here / ('profile-' + mode + '-' + name); profile.mkdir(exist_ok=True)
        run = subprocess.run([str(binary), str(profile), str(root / 'app'), str(fixture), str(hashes), force],
            capture_output=True, text=True, timeout=10, env=dict(os.environ, SDL_AUDIODRIVER='dummy'))
        (here / ('vmem-' + mode + '-' + name + '.log')).write_text(run.stdout + run.stderr)
        line = next(v for v in run.stdout.splitlines() if v.startswith('PROBE_RESULT '))
        runs[mode][name] = json.loads(line[len('PROBE_RESULT '):])
        runs[mode][name]['exit_code'] = run.returncode
        print(mode, name, {k:v for k,v in runs[mode][name].items() if k != 'observed_hashes'}, flush=True)
        if name != 'h264-cropped': run.check_returncode()
        runs[mode][name]['fixture_sha256'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
        runs[mode][name]['reference_sha256'] = hashlib.sha256(hashes.read_bytes()).hexdigest()
(here / 'vmem-ab-results.json').write_text(json.dumps(runs, indent=2) + '\n')
