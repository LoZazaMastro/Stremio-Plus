// Stremio - disk reads and image decoding never run in the UI draw loop.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "texture_cache.h"
#include "artcache.h"
#include "gfx/gl_batch.hpp"
#include "util.h"
#include <GL/glcorearb.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

struct TextureCache::Impl {
    struct Entry {
        uint32_t texture = 0;
        int width = 0, height = 0;
        size_t bytes = 0;
        uint64_t used = 0, retry_frame = 0;
        unsigned failures = 0;
        bool loading = false, failed = false;
    };
    struct Request { std::string path; uint64_t frame = 0, generation = 0; int priority = 0; };
    struct Ready { std::string path; std::vector<uint8_t> pixels; int w = 0, h = 0; uint64_t generation = 0; };
    hui::gfx::GlBatch& batch;
    std::string base;
    size_t budget, bytes = 0, ready_bytes = 0;
    uint64_t frame = 0, generation = 0;
    std::map<std::string, Entry> entries;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;
    std::deque<Ready> ready;
    std::vector<std::thread> workers;
    bool stopping = false, playback = false;
    unsigned active = 0;
    static int priority(const std::string& path) {
        // ArtCache's already-resized hero/title files. Local chrome/assets
        // remain ordinary FIFO work, and no URL parsing happens here.
        for (const char* suffix : {"bd.rgba", "bd2.rgba", "lb.rgba", "ll.rgba", "b.rgba", "l.rgba", "q.rgba"})
            if (ends_with(path, suffix)) return 1;
        return 0;
    }
    Impl(hui::gfx::GlBatch& b, std::string dir, size_t cap) : batch(b), base(std::move(dir)), budget(cap) {
        for (int i = 0; i < 2; ++i) workers.emplace_back([this] { run(); });
    }
    void run() {
        for (;;) {
            std::string path;
            uint64_t request_generation = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                // Requests from the current screen take precedence over old
                // queued pages. Within a screen, hero/logo first, then FIFO.
                auto chosen = queue.begin();
                for (auto it = queue.begin(); it != queue.end(); ++it)
                    if (it->frame > chosen->frame || (it->frame == chosen->frame && it->priority > chosen->priority)) chosen = it;
                path = std::move(chosen->path); request_generation = chosen->generation;
                queue.erase(chosen); ++active;
            }
            Ready image; image.path = path; image.generation = request_generation;
            try {
                if (!load_image_rgba(path, image.pixels, image.w, image.h) || image.w <= 0 || image.h <= 0 ||
                    image.w > 4096 || image.h > 4096 || image.pixels.size() != size_t(image.w)*image.h*4)
                    image.pixels.clear();
            } catch (const std::exception& error) {
                dlog("Artwork decoding failed: %s", error.what());
                image.pixels.clear();
            }
            {
                std::unique_lock<std::mutex> lock(mutex);
                // Workers cannot accumulate a screenful of decoded 4K art
                // while the GPU is busy presenting video.
                wake.wait(lock, [&] { return stopping || request_generation != generation || (ready.size() < 6 &&
                    ready_bytes + image.pixels.size() <= (64u << 20)); });
                --active;
                if (stopping) return;
                // A decode already running when playback started must not
                // refill the gallery queue or replace a new same-path load.
                if (request_generation != generation) continue;
                ready_bytes += image.pixels.size();
                ready.push_back(std::move(image));
            }
        }
    }
};

TextureCache::TextureCache(hui::gfx::GlBatch& batch, std::string base, size_t budget)
    : impl_(std::make_unique<Impl>(batch, std::move(base), budget)) {}
TextureCache::~TextureCache() { release(); }
void TextureCache::set_playback(bool active) {
    if (!impl_ || impl_->playback == active) return;
    auto& p = *impl_; p.playback = active;
    if (!active) return;
    std::deque<Impl::Ready> discarded;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        ++p.generation;
        p.queue.clear();
        discarded.swap(p.ready); p.ready_bytes = 0;
    }
    p.wake.notify_all();
    for (auto& [path, entry] : p.entries) if (entry.texture) glDeleteTextures(1, &entry.texture);
    p.entries.clear(); p.bytes = 0;
    // On-disk artwork and the configured gallery budget are unchanged. No
    // worker join or decode is needed at this playback transition.
}
void TextureCache::begin_frame() {
    if (!impl_) return;
    auto& p = *impl_; ++p.frame;
    // Upload at most two textures and 12 MiB per frame. A pair of 1080p heroes
    // must not monopolize the render thread when several downloads complete.
    size_t uploaded = 0;
    for (int i = 0; i < 2; ++i) {
        Impl::Ready image;
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            if (p.ready.empty()) break;
            auto chosen = p.ready.begin();
            auto rank = [&](const Impl::Ready& ready) {
                auto found = p.entries.find(ready.path);
                return std::pair<uint64_t, int>{found == p.entries.end() ? 0 : found->second.used,
                                                Impl::priority(ready.path)};
            };
            for (auto it = p.ready.begin(); it != p.ready.end(); ++it) if (rank(*it) > rank(*chosen)) chosen = it;
            if (uploaded && uploaded + chosen->pixels.size() > (12u << 20)) break;
            image = std::move(*chosen); p.ready.erase(chosen);
            p.ready_bytes -= image.pixels.size();
        }
        p.wake.notify_all();
        if (image.generation != p.generation) continue;
        auto it = p.entries.find(image.path);
        if (it == p.entries.end()) continue;
        auto& entry = it->second; entry.loading = false;
        if (image.pixels.empty()) {
            entry.failed = true;
            entry.failures = std::min(entry.failures + 1, 5u);
            entry.retry_frame = p.frame + std::min<uint64_t>(1800, 120u << (entry.failures - 1));
            continue;
        }
        // A decoded picture left behind by fast navigation can be reloaded
        // from its on-disk RGBA later; do not spend GPU upload time on it now.
        if (entry.used + 120 < p.frame) { p.entries.erase(it); continue; }
        entry.texture = p.batch.create_texture(image.w, image.h, image.pixels.data());
        if (!entry.texture) {
            entry.failed = true; entry.retry_frame = p.frame + 120;
            continue;
        }
        entry.failed = false; entry.failures = 0;
        entry.width = image.w; entry.height = image.h;
        entry.bytes = image.pixels.size(); p.bytes += entry.bytes; uploaded += entry.bytes;
    }
    const size_t budget = p.playback ? (16u << 20) : p.budget;
    while (p.bytes > budget) {
        auto oldest = p.entries.end();
        for (auto it = p.entries.begin(); it != p.entries.end(); ++it)
            if (it->second.texture && it->second.used + 2 < p.frame &&
                (oldest == p.entries.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == p.entries.end()) break;
        glDeleteTextures(1, &oldest->second.texture); p.bytes -= oldest->second.bytes;
        p.entries.erase(oldest);
    }
    if (p.entries.size() > 1024) {
        for (auto it = p.entries.begin(); it != p.entries.end(); )
            if (!it->second.texture && !it->second.loading && it->second.used + 600 < p.frame) it = p.entries.erase(it);
            else ++it;
    }
}
uint32_t TextureCache::get(const std::string& path) {
    if (!impl_ || path.empty() || starts_with(path, "http:") || starts_with(path, "https:")) return 0;
    auto& p = *impl_;
    const std::string key = path.front() == '/' ? path : p.base + "/" + path;
    auto& e = p.entries[key]; e.used = p.frame;
    if (e.texture) return e.texture;
    if (e.failed && p.frame < e.retry_frame) return 0;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (e.loading) {
            for (auto& queued : p.queue) if (queued.path == key) { queued.frame = p.frame; break; }
            return 0;
        }
        if (p.queue.size() >= 128 || p.stopping) return 0;
        e.loading = true; e.failed = false;
        p.queue.push_back(Impl::Request{key, p.frame, p.generation, Impl::priority(key)});
    }
    p.wake.notify_one(); return 0;
}
std::pair<int, int> TextureCache::dimensions(const std::string& path) const {
    if (!impl_ || path.empty()) return {};
    const auto& p = *impl_;
    const auto key = path.front() == '/' ? path : p.base + "/" + path;
    const auto found = p.entries.find(key);
    if (found == p.entries.end() || !found->second.texture) return {};
    return {found->second.width, found->second.height};
}
bool TextureCache::pending() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active || !impl_->queue.empty() || !impl_->ready.empty();
}
void TextureCache::release() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true; impl_->queue.clear();
    }
    impl_->wake.notify_all();
    for (auto& worker : impl_->workers) if (worker.joinable()) worker.join();
    for (auto& [path, entry] : impl_->entries) if (entry.texture) glDeleteTextures(1, &entry.texture);
    impl_.reset();
}
