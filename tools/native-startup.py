#!/usr/bin/env python3
"""Keep payload-only syscall code out of a PS5 native application's startup.

The SDK's libc.a supplies useful compatibility functions, but mman.o and
syscalls.o contain inline syscall instructions intended for payloads. Native
titles must import mmap/mprotect from libkernel instead. The final audit uses
LLVM's disassembler, not a byte-pattern search through code or embedded data.

The process-parameter layout below is validated against the pinned native
converter and its relocation table before setting the OpenGL process heap.
This is the same 256 MiB setting used by the supplied ProsperoLight 01.000.080.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare_libc(source, destination, ar):
    if source.resolve() == destination.resolve():
        raise ValueError('Refusing to modify the SDK archive in place')
    members = subprocess.check_output([ar, 't', str(source)], text=True).splitlines()
    removed = ['mman.o', 'syscalls.o']
    if any(members.count(name) != 1 for name in removed):
        raise ValueError('Unexpected SDK libc archive: expected one mman.o and syscalls.o')
    source_hash = digest(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    subprocess.run([ar, 'd', str(destination), *removed], check=True)
    after = subprocess.check_output([ar, 't', str(destination)], text=True).splitlines()
    if after != [name for name in members if name not in removed]:
        raise ValueError('Native libc archive contains unexpected members')
    if digest(source) != source_hash:
        raise ValueError('SDK archive changed during preparation')
    return {'sourceSha256': source_hash, 'nativeArchiveSha256': digest(destination),
            'removedPayloadObjects': removed, 'remainingObjects': len(after)}


def process_heap(data):
    if data[:6] != b'\x7fELF\x02\x01' or len(data) < 64:
        raise ValueError('Expected a little-endian ELF64 native executable')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[1:3] != (0xfe10, 62) or header[9] != 56:
        raise ValueError('Expected a converted PS5 executable with ELF64 program headers')
    programs = [struct.unpack_from('<IIQQQQQQ', data, header[5] + i * header[9])
                for i in range(header[10])]

    def unique_program(kind):
        selected = [p for p in programs if p[0] == kind]
        if len(selected) != 1:
            raise ValueError(f'Expected one program header {kind:#x}')
        return selected[0]

    def file_offset(address, size):
        candidates = [p[2] + address - p[3] for p in programs
                      if p[0] == 1 and p[3] <= address and address + size <= p[3] + p[5]]
        if len(candidates) != 1 or candidates[0] + size > len(data):
            raise ValueError('Process parameters or relocations are outside a file-backed load')
        return candidates[0]

    process = unique_program(0x61000001)
    offset, address = process[2], process[3]
    if process[5] != 0x60 or file_offset(address, 0x60) != offset:
        raise ValueError('Unexpected process-parameter segment')
    if (struct.unpack_from('<Q', data, offset)[0] != 0x60 or
            data[offset + 8:offset + 12] != b'ORBI' or
            struct.unpack_from('<I', data, offset + 12)[0] != 5):
        raise ValueError('Unsupported process-parameter ABI')
    sizes = (0xa8, 0x38, 0x10, 0x78, 0xc0, 0x38)
    counts = (0x10000000e, 0, 0, 2, 3, 1)
    blocks_address = address + 0x60
    blocks_offset = file_offset(blocks_address, sum(sizes) + 16)
    block_addresses = []
    cursor = 0
    for size, count in zip(sizes, counts):
        if struct.unpack_from('<QQ', data, blocks_offset + cursor) != (size, count):
            raise ValueError('Unexpected process-parameter block layout')
        block_addresses.append(blocks_address + cursor)
        cursor += size
    heap_address = blocks_address + cursor
    heap_offset = file_offset(heap_address, 16)
    if struct.unpack_from('<I', data, heap_offset + 8)[0] != 1:
        raise ValueError('Expected extended process heap enabled')

    dynamic = unique_program(2)
    tags = dict(struct.unpack_from('<qQ', data, pos)
                for pos in range(dynamic[2], dynamic[2] + dynamic[5], 16))
    if tags.get(9) != 24 or tags.get(8, 0) % 24:
        raise ValueError('Unsupported relocation table')
    relocation_offset = file_offset(tags[7], tags[8])
    pointers = {}
    wanted = {
        address + 0x38: block_addresses[0], address + 0x40: block_addresses[1],
        address + 0x48: block_addresses[2], blocks_address + 0x30: block_addresses[3],
        blocks_address + 0x38: block_addresses[4], blocks_address + 0x60: block_addresses[5],
        blocks_address + 0x10: heap_address, blocks_address + 0x20: heap_address + 8,
    }
    for pos in range(relocation_offset, relocation_offset + tags[8], 24):
        target, info, addend = struct.unpack_from('<QQq', data, pos)
        if target in wanted:
            if target in pointers or info != 8:
                raise ValueError('Invalid or duplicate process-parameter relocation')
            pointers[target] = addend
    if pointers != wanted:
        raise ValueError('Process-parameter relocations do not match the pinned converter ABI')
    return heap_offset, struct.unpack_from('<Q', data, heap_offset)[0]


def configure_process(path):
    data = bytearray(path.read_bytes())
    offset, previous = process_heap(data)
    heap_bytes = 256 * 1024 * 1024
    if previous not in (0xffffffffffffffff, heap_bytes):
        raise ValueError(f'Unexpected preexisting process heap {previous:#x}')
    struct.pack_into('<Q', data, offset, heap_bytes)
    if process_heap(data)[1] != heap_bytes:
        raise ValueError('Process heap verification failed')
    path.write_bytes(data)
    return {'previousHeapBytes': previous, 'processHeapBytes': heap_bytes,
            'validatedParameterRelocations': 8, 'elfSha256': digest(path)}


def audit(path, objdump, nm):
    command = [objdump, '-d', '--no-show-raw-insn', str(path)]
    prohibited = []
    owner = ''
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
        for line in process.stdout:
            if re.match(r'^[0-9a-fA-F]+ <.*>:', line):
                owner = line.strip()
            instruction = re.match(r'^\s*[0-9a-fA-F]+:\s+(.+)$', line)
            if instruction and re.match(r'(?:syscall|sysenter)\b|int\s+\$0x80\b', instruction.group(1)):
                prohibited.append({'function': owner, 'instruction': line.strip()})
        stderr = process.stderr.read()
        if process.wait() != 0:
            raise ValueError(f'Disassembly failed: {stderr.strip()}')
    if prohibited:
        raise ValueError('Payload-only system calls in native executable: ' + json.dumps(prohibited))
    symbols = subprocess.check_output([nm, '--format=posix', str(path)], text=True)
    kinds = {parts[0]: parts[1] for line in symbols.splitlines()
             if len(parts := line.split()) >= 2}
    if any(kinds.get(name) != 'U' for name in ('mmap', 'mprotect')):
        raise ValueError('mmap and mprotect must be native platform imports')
    return {'inlineSystemCallInstructions': 0, 'memoryFunctions': 'mmap/mprotect are platform imports',
            'disassembler': objdump, 'elfSha256': digest(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prepare = commands.add_parser('prepare-libc')
    prepare.add_argument('source', type=Path)
    prepare.add_argument('destination', type=Path)
    prepare.add_argument('--ar', default='llvm-ar-18')
    configure = commands.add_parser('configure-process')
    configure.add_argument('elf', type=Path)
    check = commands.add_parser('audit')
    check.add_argument('elf', type=Path)
    check.add_argument('--objdump', default='llvm-objdump-18')
    check.add_argument('--nm', default='llvm-nm-18')
    args = parser.parse_args()
    try:
        if args.command == 'prepare-libc':
            result = prepare_libc(args.source, args.destination, args.ar)
        elif args.command == 'configure-process':
            result = configure_process(args.elf)
        else:
            result = audit(args.elf, args.objdump, args.nm)
    except (ValueError, KeyError, struct.error, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Native startup validation failed: {error}') from error
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
