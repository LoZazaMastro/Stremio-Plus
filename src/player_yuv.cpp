// Stremio - preserve source YUV precision for GPU conversion and scaling.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "player.h"
#include <cstring>
#include <cstdlib>

#ifndef STREMIO_USE_LIBVLC
bool Player::pack_yuv(const YuvPicture& pic, Frame& f) {
    if (!pic.y || !pic.u || (!pic.interleaved && !pic.v) || pic.width <= 0 || pic.height <= 0 ||
        pic.width > 8192 || pic.height > 8192) return false;
    const int bytes = pic.wide ? 2 : 1;
    const int cw = (pic.width + 1) / 2, ch = (pic.height + 1) / 2;
    const size_t y_row = size_t(pic.width) * bytes;
    const size_t c_row = size_t(cw) * bytes * (pic.interleaved ? 2 : 1);
    if (size_t(std::abs(pic.y_stride)) < y_row || size_t(std::abs(pic.c_stride)) < c_row) return false;
    f.layout = pic.interleaved ? Frame::Layout::Nv12 : Frame::Layout::Planar420;
    f.w = pic.width; f.h = pic.height; f.wide = pic.wide; f.shift = pic.shift;
    f.colors = colors_; f.full_range = full_range_; f.hlg = hlg_;
    f.offsets[0] = 0;
    f.offsets[1] = y_row * pic.height;
    f.offsets[2] = f.offsets[1] + c_row * ch;
    f.pixels.resize(f.offsets[2] + (pic.interleaved ? 0 : c_row * ch));
    for (int y = 0; y < pic.height; ++y)
        std::memcpy(f.pixels.data() + size_t(y) * y_row, pic.y + ptrdiff_t(y) * pic.y_stride, y_row);
    for (int y = 0; y < ch; ++y) {
        std::memcpy(f.pixels.data() + f.offsets[1] + size_t(y) * c_row, pic.u + ptrdiff_t(y) * pic.c_stride, c_row);
        if (!pic.interleaved)
            std::memcpy(f.pixels.data() + f.offsets[2] + size_t(y) * c_row, pic.v + ptrdiff_t(y) * pic.c_stride, c_row);
    }
    return true;
}
#endif
