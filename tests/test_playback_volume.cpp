// Deterministic right-stick policy and production timestamped PCM gain checks.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback_volume.h"
#include "player_vlc_audio.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned checks = 0;
void require(bool passed, const char* what) {
    ++checks;
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
}

int main() {
    using namespace playback_volume;
    require(saved_percent(50) == 50 && saved_percent(0) == 0 && saved_percent(100) == 100,
            "stored whole percentages retain zero, middle and full volume");
    require(saved_percent(-1e90) == 0 && saved_percent(1e90) == 100,
            "extreme finite preferences clamp before integer conversion");
    require(saved_percent(std::numeric_limits<double>::quiet_NaN()) == 100 &&
            saved_percent(std::numeric_limits<double>::infinity()) == 100 && saved_percent(33.5) == 100,
            "non-finite and fractional preferences restore a safe full-volume default");
    require(gain(-1) == 0 && gain(50) == 0.5f && gain(101) == 1,
            "media gain is limited to attenuation without boost");

    StickRepeat stick;
    require(stick.update(0, -1, 0) == 0 && stick.update(0, -1, 1) == 0,
            "held input on entry cannot change volume before neutral");
    require(stick.update(0, 0, 2) == 0, "neutral arms the control without a step");
    require(stick.update(0, -0.1f, 3) == 0 && stick.update(0, -0.54f, 4) == 0,
            "drift and movement below the engage threshold produce no step");
    require(stick.update(.9f, -.6f, 5) == 0, "predominantly horizontal movement does not change volume");
    require(stick.update(0, -.8f, 6) == 1, "right stick up increases volume immediately");
    require(stick.update(0, -.8f, 6.34) == 0, "holding respects initial repeat delay");
    require(stick.update(0, -.4f, 6.36) == 1, "hysteresis retains a deliberate hold below engage threshold");
    require(stick.update(0, -.4f, 6.47) == 0 && stick.update(0, -.4f, 6.49) == 1,
            "held input repeats at the bounded interval");
    require(stick.update(0, -.8f, 60) == 1 && stick.update(0, -.8f, 60) == 0,
            "a long frame stall gives one step without a catch-up burst");
    require(stick.update(0, -.2f, 61) == 0 && stick.update(0, -.4f, 62) == 0,
            "release stops repetition and needs a fresh engage threshold");
    require(stick.update(0, .8f, 63) == -1, "right stick down decreases volume");
    require(stick.update(0, -.4f, 64) == 0 && stick.update(0, -.8f, 65) == 1,
            "direction reversal must pass the engage threshold");
    require(stick.update(0, -1, 66, false) == 0 && stick.update(0, -1, 67) == 0,
            "intercepted input clears a held repeat and requires neutral on return");
    stick.update(0, 0, 68);
    require(stick.update(0, -1, 69) == 1, "neutral re-arms after focus loss");
    require(stick.update(0, -1, 68) == 0 && stick.update(0, -1, 70) == 0,
            "a clock rollback clears repeat state safely");
    stick.update(0, 0, 71);
    require(stick.update(0, std::numeric_limits<float>::quiet_NaN(), 72) == 0 &&
            stick.update(0, -1, 73) == 0, "invalid analog input cannot trigger or retain a repeat");

    const std::array<std::int16_t, 8> original{-32768, 32767, -12000, 12000, -1, 1, 0, 2222};
    auto pcm = original;
    scale_s16(pcm.data(), pcm.size(), 1);
    require(pcm == original, "100 percent is bit-exact PCM unity gain");
    scale_s16(pcm.data(), pcm.size(), .5f);
    require(pcm == std::array<std::int16_t, 8>{-16384, 16383, -6000, 6000, 0, 0, 0, 1111},
            "50 percent halves signed PCM in both channels");
    scale_s16(pcm.data(), pcm.size(), 0);
    require(pcm == std::array<std::int16_t, 8>{}, "zero percent mutes every signed PCM sample");
    pcm = original; scale_s16(pcm.data(), pcm.size(), 4);
    require(pcm == std::array<std::int16_t, 8>{-32768, 32767, -32768, 32767, -4, 4, 0, 8888},
            "pre-existing VLC boost saturates instead of overflowing signed samples");
    pcm = original; scale_s16(pcm.data(), pcm.size(), std::numeric_limits<float>::infinity());
    require(pcm == std::array<std::int16_t, 8>{}, "non-finite callback gain produces silence without undefined conversion");
    scale_s16(nullptr, 32, .5f);

    VlcAudioTimeline timeline;
    constexpr std::int64_t start = 1000000;
    const std::array<std::int16_t, 8> source{12000, -12000, 12000, -12000, 12000, -12000, 12000, -12000};
    std::array<std::int16_t, 2> output{};
    timeline.push(source.data(), 4, start);
    require(timeline.render(output.data(), 1, start, gain(50)) == 1 &&
            output == std::array<std::int16_t, 2>{6000, -6000}, "queued production VLC PCM observes current half gain");
    require(timeline.render(output.data(), 1, start + 21, gain(0)) == 1 &&
            output == std::array<std::int16_t, 2>{0, 0}, "muting still consumes timestamped PCM instead of accumulating audio");
    require(timeline.render(output.data(), 1, start + 42, gain(100)) == 1 &&
            output == std::array<std::int16_t, 2>{12000, -12000}, "restoring volume affects already queued PCM without a flush");
    require(timeline.queued() == 1 && source[0] == 12000 && source[1] == -12000,
            "gain preserves source ownership and exact queue accounting");
    std::printf("{\"result\":\"PASS\",\"checks\":%u}\n", checks);
}
