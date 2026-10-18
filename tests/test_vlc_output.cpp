// Deterministic output-clock, queue-bound and plane-transfer regressions.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player_vlc_audio.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void vlc_ps5_copy_plane(uint8_t*, size_t, const uint8_t*, size_t, size_t, unsigned);
extern "C" void vlc_ps5_copy_plane_shift(uint8_t*, size_t, const uint8_t*, size_t, size_t, unsigned, unsigned);
extern "C" void vlc_ps5_split_plane16(uint8_t*, size_t, uint8_t*, size_t, const uint8_t*, size_t, size_t, unsigned);

static unsigned checks = 0;
static void check(bool value, const char* what) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

static void audio_clock() {
    VlcAudioTimeline queue;
    constexpr int64_t start = 1000000;
    std::vector<int16_t> source(4800 * 2, 12000), output(480 * 2, -1);
    queue.push(source.data(), 4800, start);
    check(queue.render(output.data(), 480, start - 10000) == 0, "future audio cannot advance the startup watchdog");
    check(std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 0; }), "future sound remains silent");
    check(queue.queued() == 4800, "future sound is retained");
    check(queue.render(output.data(), 480, start) == 480, "startup clock counts PCM delivered to the output");
    check(std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 12000; }), "sound begins at its timestamp");
    check(queue.queued() == 4320, "render consumes exact frame count");
    queue.shift(3000000);
    check(queue.render(output.data(), 480, start + 10000) == 0, "a paused timeline cannot advance the startup watchdog");
    check(std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 0; }), "pause shifts pending timestamps");
    queue.render(output.data(), 480, start + 3010000, 0.5f);
    check(std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 6000; }), "resume and gain preserve samples");
    queue.clear();
    queue.render(output.data(), 480, start + 4000000);
    check(queue.queued() == 0 && std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 0; }),
        "seek flush cannot replay stale audio");
    queue.push(source.data(), 4800, start);
    check(queue.render(output.data(), 480, start + 200000) == 0, "discarded late audio cannot advance the startup watchdog");
    check(queue.queued() == 0 && std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 0; }),
        "late blocks catch up without stale sound");

    std::vector<int16_t> tiny(40 * 2, 2222);
    for (int i = 0; i < 5000; ++i) queue.push(tiny.data(), 40, start + int64_t(i) * 40000000 / 48000);
    check(queue.queued() == VlcAudioTimeline::capacity, "TrueHD-size blocks have a four-second bound");
    queue.render(output.data(), 480, start + int64_t(5000 * 40 - VlcAudioTimeline::capacity) * 1000000 / 48000);
    check(std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 2222; }), "ring wrap preserves PCM");
    queue.clear();
    std::vector<int16_t> large((VlcAudioTimeline::capacity + 480) * 2, 30000);
    queue.push(large.data(), VlcAudioTimeline::capacity + 480, start);
    check(queue.queued() == VlcAudioTimeline::capacity, "oversize callback remains bounded");
    queue.render(output.data(), 480, start + 10000, 2.0f);
    check(std::all_of(output.begin(), output.end(), [](int16_t v) { return v == 32767; }), "amplification clips safely");
}

static bool plane_copy(unsigned seed) {
    constexpr unsigned rows = 1081;
    constexpr size_t width = 3840, pitch = 3904;
    std::vector<uint8_t> source(pitch * rows), target(pitch * rows, 0xa5);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t((i * 13 + seed) % 251);
    const auto original = source;
    vlc_ps5_copy_plane(target.data(), pitch, source.data(), pitch, width, rows);
    bool okay = source == original;
    for (unsigned row = 0; row < rows; ++row) {
        okay = okay && !std::memcmp(target.data() + row * pitch, source.data() + row * pitch, width);
        for (size_t x = width; x < pitch; ++x) okay = okay && target[row * pitch + x] == 0xa5;
    }
    return okay;
}

static void transfers() {
    check(plane_copy(7), "padded 4K plane copies exact rows and preserves padding");
    std::atomic<bool> okay{true};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 3; ++i) threads.emplace_back([&, i] { if (!plane_copy(i * 71)) okay = false; });
    for (auto& thread : threads) thread.join();
    check(okay, "concurrent decoder and output copies cannot exchange jobs");
    constexpr size_t pitch = 4096;
    constexpr unsigned rows = 577;
    std::vector<uint16_t> source(pitch / 2 * rows), shifted(source.size(), 0xffff);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint16_t(i % 1024);
    vlc_ps5_copy_plane_shift(reinterpret_cast<uint8_t*>(shifted.data()), pitch,
        reinterpret_cast<const uint8_t*>(source.data()), pitch, pitch, rows, 6);
    bool same = true;
    for (size_t i = 0; i < source.size(); ++i) same = same && shifted[i] == uint16_t(source[i] << 6);
    check(same, "P010 conversion preserves every ten-bit sample");
    std::vector<uint16_t> u(source.size() / 2), v(source.size() / 2);
    vlc_ps5_split_plane16(reinterpret_cast<uint8_t*>(u.data()), pitch / 2,
        reinterpret_cast<uint8_t*>(v.data()), pitch / 2,
        reinterpret_cast<const uint8_t*>(source.data()), pitch, pitch, rows);
    same = true;
    for (size_t i = 0; i < u.size(); ++i) same = same && u[i] == source[2 * i] && v[i] == source[2 * i + 1];
    check(same, "10-bit UV deinterleave preserves independent channels");
    const auto original = shifted;
    vlc_ps5_copy_plane(reinterpret_cast<uint8_t*>(shifted.data()), 1,
        reinterpret_cast<const uint8_t*>(source.data()), pitch, pitch, rows);
    check(shifted == original, "invalid pitch cannot overwrite destination");
}

int main() {
    audio_clock();
    transfers();
    std::printf("{\"result\":\"PASS\",\"checks\":%u}\n", checks);
}
