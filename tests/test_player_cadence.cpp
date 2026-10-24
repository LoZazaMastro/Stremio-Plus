// Real VLC presentation recovery with one delayed vmem copy. Decoder packets,
// reference pictures, audio timing and the application's output leases are real.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "diagnostics.h"
#include "http.h"
#include <vlc/vlc.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
libvlc_instance_t* __real_libvlc_new(int, const char* const*);
void __real_libvlc_video_set_callbacks(libvlc_media_player_t*, libvlc_video_lock_cb,
    libvlc_video_unlock_cb, libvlc_video_display_cb, void*);
void __real_libvlc_media_player_set_media(libvlc_media_player_t*, libvlc_media_t*);
void __real_vlc_ps5_copy_plane(uint8_t*, size_t, const uint8_t*, size_t, size_t, unsigned);
}

namespace {
bool baseline = false;
std::atomic<bool> armed{false}, injected{false}, guards_intact{false};
std::atomic<int64_t> stall_ended{0};
std::atomic<unsigned> burst_displays{0};
libvlc_video_display_cb real_display = nullptr;
std::mutex media_mutex;
libvlc_media_t* observed_media = nullptr;
unsigned checks = 0;
int64_t micros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
double seconds() { return double(micros()) / 1000000.0; }
void require(bool condition, const char* reason) {
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
uint64_t digest(const Player::VideoFrame& frame) {
    uint64_t value = 1469598103934665603ull;
    // Match the independently decoded packed image, excluding coded padding
    // and row alignment that are intentionally outside the visible picture.
    const unsigned count = frame.layout == Player::VideoFrame::Layout::Nv12 ? 2 : 3;
    size_t packed = 0, sample = 0;
    for (unsigned plane = 0; plane < count; ++plane) {
        const size_t rows = plane ? (frame.h + 1) / 2 : frame.h;
        const size_t bytes = size_t(plane ? (frame.w + 1) / 2 : frame.w) *
            (frame.wide ? 2 : 1) * (plane && count == 2 ? 2 : 1);
        const size_t pitch = frame.pitches[plane], offset = frame.offsets[plane];
        if (!rows || pitch < bytes || offset > frame.size() ||
            (rows - 1) * pitch + bytes > frame.size() - offset)
            throw std::runtime_error("visible frame plane exceeds its retained allocation");
        for (size_t row = 0; row < rows; ++row) {
            while (sample < packed + bytes) {
                value = (value ^ frame.data()[offset + row * pitch + sample - packed]) * 1099511628211ull;
                sample += 4093;
            }
            packed += bytes;
        }
    }
    return value;
}
void display_probe(void* opaque, void* picture) {
    const int64_t end = stall_ended.load();
    if (end > 0 && micros() - end < 50000) ++burst_displays;
    real_display(opaque, picture);
}
}

extern "C" libvlc_instance_t* __wrap_libvlc_new(int argc, const char* const* argv) {
    std::vector<const char*> arguments(argv, argv + argc);
    bool display_drop = false, no_skip = false, no_hurry = false, no_corrupt = false, skip_zero = false;
    for (auto& value : arguments) {
        if (std::strcmp(value, "--drop-late-frames") == 0) {
            display_drop = true;
            if (baseline) value = "--no-drop-late-frames";
        }
        no_skip |= std::strcmp(value, "--no-skip-frames") == 0;
        no_hurry |= std::strcmp(value, "--no-avcodec-hurry-up") == 0;
        no_corrupt |= std::strcmp(value, "--no-avcodec-corrupted") == 0;
        skip_zero |= std::strcmp(value, "--avcodec-skip-frame=0") == 0;
    }
    guards_intact = display_drop && no_skip && no_hurry && no_corrupt && skip_zero;
    return __real_libvlc_new(argc, arguments.data());
}
extern "C" void __wrap_libvlc_video_set_callbacks(libvlc_media_player_t* player,
    libvlc_video_lock_cb lock, libvlc_video_unlock_cb unlock, libvlc_video_display_cb display, void* opaque) {
    real_display = display;
    __real_libvlc_video_set_callbacks(player, lock, unlock, display_probe, opaque);
}
extern "C" void __wrap_libvlc_media_player_set_media(libvlc_media_player_t* player, libvlc_media_t* media) {
    {
        std::lock_guard lock(media_mutex);
        if (media) libvlc_media_retain(media);
        if (observed_media) libvlc_media_release(observed_media);
        observed_media = media;
    }
    __real_libvlc_media_player_set_media(player, media);
}
extern "C" void __wrap_vlc_ps5_copy_plane(uint8_t* destination, size_t destination_pitch,
    const uint8_t* source, size_t source_pitch, size_t bytes, unsigned rows) {
    if (rows == 1080 && armed.exchange(false)) {
        // Delay only one output copy, never a packet/decode or native syscall.
        // This creates stale decoded pictures waiting in VLC's vout queue.
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        stall_ended = micros();
        injected = true;
    }
    __real_vlc_ps5_copy_plane(destination, destination_pitch, source, source_pitch, bytes, rows);
}

int main(int argc, char** argv) {
    if (argc != 6) return 2;
    baseline = std::strcmp(argv[5], "baseline") == 0;
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    diagnostics_start(std::string(argv[1]) + "/logs");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    int result = 0;
    try {
        std::set<uint64_t> clean;
        std::ifstream hashes(argv[4]); uint64_t value = 0;
        while (hashes >> value) clean.insert(value);
        require(clean.size() > 100, "independently decoded 1080p reference pictures are missing");
        Player player; Player::Options options; options.url = argv[3]; player.open(options);
        Player::VideoFrame frame, held;
        uint64_t held_hash = 0;
        unsigned frames = 0, frames_after_stall = 0;
        double last_picture_lateness = 0;
        auto pump = [&] {
            require(player.state() != Player::State::Failed, "real 1080p playback failed");
            if (player.take_frame(frame)) {
                require(frame.w == 1920 && frame.h == 1080 && !frame.wide, "1080p source format changed");
                require(clean.count(digest(frame)) != 0, "display drop damaged a decoded reference picture");
                if (injected) {
                    ++frames_after_stall;
                    last_picture_lateness = seconds() - frame.presentation_time;
                }
                ++frames;
            }
            SDL_Delay(2);
        };
        double deadline = seconds() + 8;
        while (seconds() < deadline && (frames < 14 || player.position() < .7)) pump();
        require(frames >= 14 && player.position() >= .7, "fixture did not establish initial 1080p playback");
        require(std::abs(player.source_fps() - 24.0) < .001,
            "source frame rate must come from media metadata, not renderer pickup cadence");
        require(guards_intact, "production decoder safety options were removed");
        held = frame; held_hash = digest(held); armed = true;
        deadline = seconds() + 8;
        while (seconds() < deadline && (!injected || frames_after_stall < 65 || player.position() < 4)) pump();
        require(injected && frames_after_stall >= 65 && player.position() >= 4,
            "output did not resume and advance after the controlled display stall");
        require(std::abs(player.source_fps() - 24.0) < .001,
            "a presentation stall must not change the declared source frame rate");
        require(digest(held) == held_hash, "recovering output overwrote a retained render lease");
        libvlc_media_stats_t statistics{};
        {
            std::lock_guard lock(media_mutex);
            require(observed_media && libvlc_media_get_stats(observed_media, &statistics), "VLC output statistics unavailable");
        }
        player.close();
        require(digest(held) == held_hash, "closing output invalidated a retained render lease");
        std::printf("CADENCE_RESULT {\"mode\":\"%s\",\"checks\":%u,\"frames\":%u,\"frames_after_stall\":%u,"
            "\"burst_displays_50ms\":%u,\"decoded_pictures\":%d,\"vout_lost_pictures\":%d,"
            "\"final_pickup_lateness_ms\":%.3f,\"decoder_guards_preserved\":true}\n",
            baseline ? "baseline" : "fixed", checks, frames, frames_after_stall, burst_displays.load(),
            statistics.i_decoded_video, statistics.i_lost_pictures, last_picture_lateness * 1000.0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CADENCE_FAILURE %s\n", error.what()); result = 1;
    }
    {
        std::lock_guard lock(media_mutex);
        if (observed_media) libvlc_media_release(observed_media);
        observed_media = nullptr;
    }
    diagnostics_stop(); SDL_Quit(); return result;
}
