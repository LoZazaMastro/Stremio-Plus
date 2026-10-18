#!/usr/bin/env python3
"""Convert the supplied 16:9 artwork to PS5 BC7_UNORM DX10 DDS, without a crop.

Build dependency only: Pillow and ispc_texcomp==1.0.1. Prebuilt pic0.dds is shipped.
"""
import argparse
from pathlib import Path
import struct

from PIL import Image
import ispc_texcomp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    artwork = Image.open(args.source).convert('RGBA')
    if artwork.width * 9 != artwork.height * 16:
        raise SystemExit('The background must be 16:9; no implicit crop or distortion is applied.')
    artwork = artwork.resize((3840, 2160), Image.Resampling.LANCZOS)
    surface = ispc_texcomp.RGBASurface(artwork.tobytes(), 3840, 2160)
    blocks = ispc_texcomp.compress_blocks_bc7(surface, ispc_texcomp.BC7EncSettings.from_profile('slow'))
    if len(blocks) != 3840 * 2160:
        raise SystemExit('Unexpected BC7 block length')
    # DDS_HEADER and DDS_HEADER_DXT10; one 2D BC7_UNORM level, no mipmap chain.
    header = bytearray(148)
    header[:4] = b'DDS '
    struct.pack_into('<7I', header, 4, 124, 0xA1007, 2160, 3840, len(blocks), 0, 1)
    struct.pack_into('<II4s', header, 76, 32, 4, b'DX10')
    struct.pack_into('<I', header, 108, 0x1000)
    struct.pack_into('<5I', header, 128, 98, 3, 0, 1, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + blocks)
    print(f'{args.output}: 3840x2160 BC7_UNORM DX10, {len(header) + len(blocks)} bytes')


if __name__ == '__main__':
    main()
