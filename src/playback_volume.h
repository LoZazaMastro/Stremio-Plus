// Playback-only volume policy and signed PCM gain.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace playback_volume {

constexpr int default_percent = 100;
constexpr int step_percent = 5;
constexpr double indicator_seconds = 1.8;

inline int clamp_percent(int value) { return std::clamp(value, 0, 100); }

// Validate before converting to int: corrupted or non-finite preferences must
// never overflow the conversion or unexpectedly mute a new installation.
inline int saved_percent(double value) {
    if (!std::isfinite(value) || std::floor(value) != value) return default_percent;
    return static_cast<int>(std::clamp(value, 0.0, 100.0));
}

inline float gain(int percent) { return static_cast<float>(clamp_percent(percent)) / 100.0f; }

// Only call for owned, decoded signed 16-bit PCM. Encoded HDMI carriers never
// pass through this function. The 4x bound retains VLC's existing gain limit.
inline void scale_s16(std::int16_t* samples, std::size_t count, float gain) {
    if (!samples || !count) return;
    if (!std::isfinite(gain)) gain = 0;
    gain = std::clamp(gain, 0.0f, 4.0f);
    if (gain == 1.0f) return;
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = static_cast<std::int16_t>(std::clamp(samples[i] * gain, -32768.0f, 32767.0f));
}

// InputFrame already removes the radial stick dead zone. This extra vertical
// threshold and hysteresis make intentional volume steps independent of drift.
// A frame stall can issue only one step; it never catches up with a burst.
class StickRepeat {
public:
    void reset() { direction_ = 0; next_ = 0; last_ = -1; armed_ = false; }

    int update(float x, float y, double now, bool usable = true) {
        if (!usable || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(now) ||
            (last_ >= 0 && now < last_)) {
            reset();
            return 0;
        }
        last_ = now;
        const float horizontal = std::abs(x), vertical = std::abs(y);
        if (!armed_) {
            // A held stick when playback starts or focus returns is not a new
            // request. Require one neutral sample before accepting movement.
            if (horizontal <= release && vertical <= release) armed_ = true;
            return 0;
        }
        if (vertical <= release || horizontal > vertical) {
            direction_ = 0;
            return 0;
        }
        const int wanted = y < 0 ? 1 : -1;
        if (wanted != direction_) {
            if (vertical < engage) { direction_ = 0; return 0; }
            direction_ = wanted;
            next_ = now + initial_delay;
            return wanted;
        }
        if (now < next_) return 0;
        next_ = now + repeat_interval;
        return wanted;
    }

    static constexpr float engage = 0.55f, release = 0.30f;
    static constexpr double initial_delay = 0.35, repeat_interval = 0.12;

private:
    int direction_ = 0;
    double next_ = 0, last_ = -1;
    bool armed_ = false;
};

} // namespace playback_volume
