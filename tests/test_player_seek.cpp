// Real HEVC playback must restore normal frame selection after seek preroll.
// The wrapper only observes libavcodec state; it never injects or changes it.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "diagnostics.h"
#include "http.h"
extern "C" {
#include <libavcodec/avcodec.h>
int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
}
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>

namespace {
constexpr unsigned kPhases = 5;
std::atomic<unsigned> phase{0};
std::array<std::atomic<unsigned>, kPhases> packets{}, defaults{}, nonrefs{};
unsigned checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t digest(const Player::VideoFrame& frame) {
    uint64_t value = 1469598103934665603ull;
    for (size_t i = 0; i < frame.size(); i += 1223)
        value = (value ^ frame.data()[i]) * 1099511628211ull;
    return value;
}
}

extern "C" int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    if (context->codec_type == AVMEDIA_TYPE_VIDEO && packet && packet->size > 0) {
        const unsigned at = phase.load();
        ++packets[at];
        if (context->skip_frame == AVDISCARD_DEFAULT) ++defaults[at];
        if (context->skip_frame >= AVDISCARD_NONREF) ++nonrefs[at];
    }
    return __real_avcodec_send_packet(context, packet);
}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    diagnostics_start(std::string(argv[1]) + "/logs");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    try {
        std::set<uint64_t> reference;
        std::ifstream hashes(argv[4]); uint64_t value = 0;
        while (hashes >> value) reference.insert(value);
        require(reference.size() > 200, "The independent clean-frame oracle is incomplete");
        Player player; Player::Options options; options.url = argv[3];
        player.open(options);
        Player::VideoFrame frame, retained;
        unsigned all_frames = 0;
        auto pump = [&] {
            require(player.state() != Player::State::Failed, "The real player failed during a seek");
            bool fresh = player.take_frame(frame);
            if (fresh) {
                require(frame.w == 640 && frame.h == 360 && frame.wide,
                    "The decoded HEVC Main10 format changed");
                require(reference.count(digest(frame)) != 0,
                    "A seek delivered a picture absent from the clean decode");
                ++all_frames;
            }
            SDL_Delay(3);
            return fresh;
        };
        double deadline = now() + 8;
        while (now() < deadline && !(all_frames > 8 && player.position() > .4)) pump();
        require(all_frames > 8, "Initial HEVC playback did not start");
        retained = frame; const uint64_t retained_hash = digest(retained);

        bool success = true, baseline_ok = false;
        unsigned degraded_stages = 0;
        auto measure = [&](const char* name, unsigned at) {
            phase = at;
            const unsigned packet_start = packets[at], default_start = defaults[at], nonref_start = nonrefs[at];
            const double start = now(), from = player.position();
            std::set<uint64_t> distinct;
            unsigned count = 0;
            while (now() - start < 2.0) {
                if (pump()) { ++count; distinct.insert(digest(frame)); }
            }
            const double elapsed = now() - start, to = player.position();
            const unsigned sent = packets[at] - packet_start;
            const unsigned normal = defaults[at] - default_start;
            const unsigned dropping = nonrefs[at] - nonref_start;
            const double density = distinct.size() / elapsed;
            const bool healthy = sent > 12 && normal == sent && dropping == 0 && density >= 18 && to > from + 1.2;
            if (at == 0) baseline_ok = healthy;
            else if (!healthy) ++degraded_stages;
            success &= healthy;
            std::printf("SEEK_STAGE {\"stage\":\"%s\",\"result\":\"%s\",\"frames\":%u,"
                "\"unique_frames\":%zu,\"seconds\":%.3f,\"unique_fps\":%.3f,\"position_start\":%.3f,"
                "\"position_end\":%.3f,\"packets\":%u,\"default_discard_packets\":%u,\"nonref_discard_packets\":%u}\n",
                name, healthy ? "PASS" : "FAIL", count, distinct.size(), elapsed, density, from, to,
                sent, normal, dropping);
            std::fflush(stdout);
        };
        measure("before_seek", 0);
        require(baseline_ok, "Initial playback cannot establish the 24 fps control case");
        auto seek = [&](const char* name, double target, unsigned seeking_phase, unsigned measured_phase) {
            phase = seeking_phase;
            const unsigned before = all_frames;
            player.seek(target);
            const double limit = now() + 8;
            bool confirmed = false;
            while (now() < limit && !confirmed) {
                if (pump()) confirmed = all_frames > before && frame.pts >= target - .2 && frame.pts <= target + 1.1;
            }
            require(confirmed, "The real player did not confirm the requested seek");
            require(digest(retained) == retained_hash, "Seeking invalidated a retained render lease");
            measure(name, measured_phase);
        };
        seek("forward_seek", 8.4, 1, 2);
        seek("backward_seek", 1.4, 3, 4);
        const unsigned preroll = nonrefs[1] + nonrefs[3];
        require(preroll > 12, "The fixture did not exercise actual NONREF seek preroll");
        player.close();
        require(digest(retained) == retained_hash, "Close invalidated a picture retained across both seeks");
        std::printf("SEEK_RESULT {\"result\":\"%s\",\"checks\":%u,\"frames\":%u,"
            "\"seeks\":2,\"preroll_nonref_packets\":%u,\"degraded_stages\":%u}\n",
            success ? "PASS" : "FAIL", checks, all_frames, preroll, degraded_stages);
        diagnostics_stop(); SDL_Quit(); return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SEEK_FAILURE %s\n", error.what());
        diagnostics_stop(); SDL_Quit(); return 2;
    }
}
