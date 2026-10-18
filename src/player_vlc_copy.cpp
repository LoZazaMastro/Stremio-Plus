// Bounded parallel plane copies for VLC-PS5's decoder and vmem output.
// Adapted from ZiZc3/VLC-PS5 src/player.cc (GPL-2.0-or-later).
// Workers have an explicit stop/join lifetime; small pictures stay on caller.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr unsigned kParts = 4;
struct Copy {
    uint8_t* destination;
    const uint8_t* source;
    size_t destination_pitch, source_pitch, bytes;
    unsigned rows, shift;
    uint8_t* second;
    size_t second_pitch;
};

void copy_rows(const Copy& copy, unsigned part, unsigned parts) {
    const unsigned start = copy.rows * part / parts, end = copy.rows * (part + 1) / parts;
    for (unsigned row = start; row < end; ++row) {
        auto* destination = copy.destination + size_t(row) * copy.destination_pitch;
        const auto* source = copy.source + size_t(row) * copy.source_pitch;
        if (copy.second) {
            auto* u = reinterpret_cast<uint16_t*>(destination);
            auto* v = reinterpret_cast<uint16_t*>(copy.second + size_t(row) * copy.second_pitch);
            const auto* uv = reinterpret_cast<const uint16_t*>(source);
            for (size_t i = 0; i < copy.bytes / 4; ++i) { u[i] = uv[i * 2]; v[i] = uv[i * 2 + 1]; }
        } else if (copy.shift) {
            auto* output = reinterpret_cast<uint16_t*>(destination);
            const auto* input = reinterpret_cast<const uint16_t*>(source);
            for (size_t i = 0; i < copy.bytes / 2; ++i) output[i] = uint16_t(input[i] << copy.shift);
        } else std::memcpy(destination, source, copy.bytes);
    }
}

class CopyPool {
public:
    ~CopyPool() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        start_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
    void run(const Copy& copy) {
        if (!copy.rows || !copy.bytes) return;
        if (copy.bytes < (1u << 20) / copy.rows) { copy_rows(copy, 0, 1); return; }
        std::lock_guard use(use_);
        if (workers_.empty()) {
            try {
                for (unsigned part = 1; part < kParts; ++part)
                    workers_.emplace_back([this, part] { worker(part); });
            } catch (...) {
                { std::lock_guard lock(mutex_); stopping_ = true; }
                start_.notify_all();
                for (auto& thread : workers_) if (thread.joinable()) thread.join();
                workers_.clear();
                copy_rows(copy, 0, 1);
                return;
            }
        }
        if (stopping_) { copy_rows(copy, 0, 1); return; }
        {
            std::lock_guard lock(mutex_);
            copy_ = copy; completed_ = 0; ++generation_;
        }
        start_.notify_all();
        copy_rows(copy, 0, kParts);
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return completed_ == kParts - 1; });
    }
private:
    void worker(unsigned part) {
        unsigned observed = 0;
        for (;;) {
            Copy copy;
            {
                std::unique_lock lock(mutex_);
                start_.wait(lock, [&] { return stopping_ || generation_ != observed; });
                if (stopping_) return;
                observed = generation_; copy = copy_;
            }
            copy_rows(copy, part, kParts);
            std::lock_guard lock(mutex_);
            if (++completed_ == kParts - 1) done_.notify_one();
        }
    }
    std::mutex mutex_, use_;
    std::condition_variable start_, done_;
    std::vector<std::thread> workers_;
    Copy copy_{};
    unsigned generation_ = 0, completed_ = 0;
    bool stopping_ = false;
};
CopyPool& pool() { static CopyPool instance; return instance; }
}

extern "C" void vlc_ps5_copy_plane_shift(uint8_t* destination, size_t destination_pitch,
    const uint8_t* source, size_t source_pitch, size_t bytes, unsigned rows, unsigned shift) {
    if (!destination || !source || destination_pitch < bytes || source_pitch < bytes || shift > 15) return;
    pool().run({destination, source, destination_pitch, source_pitch, bytes, rows, shift, nullptr, 0});
}
extern "C" void vlc_ps5_copy_plane(uint8_t* destination, size_t destination_pitch,
    const uint8_t* source, size_t source_pitch, size_t bytes, unsigned rows) {
    vlc_ps5_copy_plane_shift(destination, destination_pitch, source, source_pitch, bytes, rows, 0);
}
extern "C" void vlc_ps5_split_plane16(uint8_t* u, size_t u_pitch, uint8_t* v, size_t v_pitch,
    const uint8_t* source, size_t source_pitch, size_t bytes, unsigned rows) {
    if (!u || !v || !source || bytes % 4 || u_pitch < bytes / 2 || v_pitch < bytes / 2 || source_pitch < bytes) return;
    pool().run({u, source, u_pitch, source_pitch, bytes, rows, 0, v, v_pitch});
}
