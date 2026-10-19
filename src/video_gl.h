// Stremio - GPU video conversion on the same OpenGL context as the UI.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "player.h"
#include <cstdint>

class VideoGl {
public:
    ~VideoGl();
    bool init();
    bool upload(const Player::VideoFrame& frame);
    void clear();
    void release();
    uint32_t texture() const { return valid_ ? output_ : 0; }
    double aspect() const { return aspect_; }
    // HDR scan-out: an HDR10 or HLG frame is converted to PQ-coded BT.2020 in a
    // 10-bit texture instead of being tone mapped to SDR. A change re-converts
    // the retained planes at once.
    void set_hdr_output(bool on);
    // The last uploaded frame was HDR10 or HLG.
    bool hdr_source() const { return valid_ && source_hdr_; }
    // texture() holds PQ codes for the HDR final pass, not an SDR picture.
    bool hdr_texture() const { return valid_ && source_hdr_ && hdr_output_; }
private:
    bool convert();
    uint32_t program_ = 0, vao_ = 0, framebuffer_ = 0, output_ = 0, planes_[3]{};
    int width_ = 0, height_ = 0, format_ = -1;
    int layout_ = 0, shift_ = 0, color_mode_ = 1;
    bool wide_ = false, full_range_ = false, hlg_ = false;
    bool valid_ = false, source_hdr_ = false, hdr_output_ = false;
    double aspect_ = 16.0 / 9.0;
};
