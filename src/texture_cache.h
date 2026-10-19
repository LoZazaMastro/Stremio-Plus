// Stremio - asynchronous image loading and bounded GPU artwork cache.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace hui::gfx { class GlBatch; }
class TextureCache {
public:
    explicit TextureCache(hui::gfx::GlBatch& batch, std::string base, size_t budget = 128u << 20);
    ~TextureCache();
    // Call on the render thread between composed frames. Entering playback
    // releases gallery residency; visible player logos/thumbnails still load
    // at their original resolution under a smaller cache budget.
    void set_playback(bool active);
    void begin_frame();
    uint32_t get(const std::string& path);
    // Already-loaded dimensions only; never initiates disk/network work.
    std::pair<int, int> dimensions(const std::string& path) const;
    void release();
    bool pending() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
