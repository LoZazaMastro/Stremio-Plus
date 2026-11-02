// Latest-request seek handoff between the controller and playback worker.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>

namespace playback_seek {
struct Target { double seconds; std::uint64_t generation; };
class Requests {
public:
    static constexpr std::int64_t quiet_us = 300000;
    void request(double seconds, std::int64_t now) {
        std::lock_guard lock(mutex_);
        pending_ = preview_ = Target{seconds, ++generation_};
        last_request_ = now;
    }
    std::optional<Target> take_ready(std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (!pending_ || now - last_request_ < quiet_us) return {};
        auto result = pending_;
        pending_.reset();
        return result;
    }
    bool pending() const { std::lock_guard lock(mutex_); return pending_.has_value(); }
    double position(double displayed) const {
        std::lock_guard lock(mutex_);
        return preview_ ? preview_->seconds : displayed;
    }
    void displayed(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        // A picture from the previous seek must not erase a newer preview.
        if (preview_ && preview_->generation == generation) preview_.reset();
    }
private:
    mutable std::mutex mutex_;
    std::optional<Target> pending_, preview_;
    std::int64_t last_request_ = 0;
    std::uint64_t generation_ = 0;
};
} // namespace playback_seek
