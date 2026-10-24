// Stremio - GPU video conversion on the same OpenGL context as the UI.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "player.h"
#include <cstdint>

class VideoGl {
public:
    enum class Failure { None, InvalidFrame, InvalidPlane, Allocation, Framebuffer };
    // CPU wall times around the GL calls, including any driver wait encountered
    // there. These are not GPU execution times and introduce no GPU fence.
    struct UploadTiming {
        double allocation_ms = 0, plane_ms = 0, draw_ms = 0;
        bool reconfigured = false;
    };
    VideoGl();
    ~VideoGl();
    bool init();
    bool upload(const Player::VideoFrame& frame);
    void clear();
    void release();
    uint32_t texture() const;
    double aspect() const { return aspect_; }
    double fps() const;
    bool dolby_vision_active() const { return dovi_active_; }
    Failure last_failure() const { return failure_; }
    const UploadTiming& last_upload_timing() const { return upload_timing_; }
    static constexpr const char* upload_path() { return "rectangle-planes"; }
private:
    void discard_textures();
    bool reject(Failure failure, const char* stage, const Player::VideoFrame& frame,
                int plane = -1, uint32_t gl_error = 0, uint32_t framebuffer_status = 0);
    uint32_t program_ = 0, vao_ = 0, framebuffer_ = 0, output_ = 0, planes_[3]{};
    uint32_t dovi_buffer_ = 0;
    int width_ = 0, height_ = 0, format_ = -1;
    bool valid_ = false;
    bool dovi_active_ = false;
    double aspect_ = 16.0 / 9.0;
    int serial_ = -1;
    double stamp_ = 0, period_ = 0, previous_media_pts_ = -1;
    Failure failure_ = Failure::None;
    UploadTiming upload_timing_;
    unsigned rejection_notes_ = 0, prior_gl_notes_ = 0;
};
