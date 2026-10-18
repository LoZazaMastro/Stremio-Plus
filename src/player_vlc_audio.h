// Timestamped PCM queue for libVLC's memory audio output.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

// The caller serializes push, render, shift and clear. Samples use a bounded
// ring, so small TrueHD blocks do not allocate a PCM buffer for each callback.
class VlcAudioTimeline {
public:
    static constexpr size_t rate = 48000, channels = 2, capacity = rate * 4;
    VlcAudioTimeline() : pcm_(capacity * channels) {}

    void clear() { blocks_.clear(); head_ = tail_ = 0; }
    size_t queued() const { return size_t(tail_ - head_); }
    void shift(int64_t us) { for (auto& block : blocks_) block.pts += us; }

    void push(const int16_t* data, size_t frames, int64_t pts) {
        if (!data || !frames) return;
        if (frames > capacity) {
            const size_t skip = frames - capacity;
            data += skip * channels;
            pts += int64_t(skip) * 1000000 / rate;
            frames = capacity;
        }
        while (!blocks_.empty() && queued() + frames > capacity) consume(blocks_.front().frames);
        const size_t start = size_t(tail_ % capacity);
        const size_t first = std::min(frames, capacity - start);
        std::memcpy(pcm_.data() + start * channels, data, first * channels * sizeof(int16_t));
        if (first < frames) std::memcpy(pcm_.data(), data + first * channels,
            (frames - first) * channels * sizeof(int16_t));
        blocks_.push_back({tail_, frames, pts});
        tail_ += frames;
    }

    size_t render(int16_t* out, size_t frames, int64_t heard_at, float gain = 1.0f) {
        std::memset(out, 0, frames * channels * sizeof(int16_t));
        size_t filled = 0, played = 0;
        while (filled < frames && !blocks_.empty()) {
            auto& block = blocks_.front();
            const int64_t at = heard_at + int64_t(filled) * 1000000 / rate;
            // Integer microsecond timestamps can differ by a sample at a
            // block boundary. Do not insert silence between adjacent tiny
            // TrueHD blocks just because one rounded upward.
            if (block.pts > at + 42) {
                const size_t silence = size_t((block.pts - at) * rate + 999999) / 1000000;
                filled += std::min(silence, frames - filled);
                continue;
            }
            if (at - block.pts > 60000) {
                consume(std::min(block.frames, size_t((at - block.pts) * rate / 1000000)));
                continue;
            }
            const size_t start = size_t(block.first % capacity);
            const size_t count = std::min({block.frames, frames - filled, capacity - start});
            const int16_t* source = pcm_.data() + start * channels;
            int16_t* target = out + filled * channels;
            if (gain == 1.0f) std::memcpy(target, source, count * channels * sizeof(int16_t));
            else for (size_t i = 0; i < count * channels; ++i)
                target[i] = int16_t(std::clamp(source[i] * gain, -32768.0f, 32767.0f));
            filled += count;
            played += count;
            consume(count);
        }
        return played;
    }

private:
    struct Block { uint64_t first; size_t frames; int64_t pts; };
    void consume(size_t count) {
        auto& block = blocks_.front();
        count = std::min(count, block.frames);
        block.first += count;
        block.frames -= count;
        block.pts += int64_t(count) * 1000000 / rate;
        head_ += count;
        if (!block.frames) blocks_.pop_front();
    }
    std::vector<int16_t> pcm_;
    std::deque<Block> blocks_;
    uint64_t head_ = 0, tail_ = 0;
};
