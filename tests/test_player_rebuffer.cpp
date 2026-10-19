// Recover a long-GOP HEVC stream after its live input stalls mid-GOP.
// Every delivered picture must match an independently decoded clean frame.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "diagnostics.h"
#include "http.h"
#include <SDL.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>
#include <fcntl.h>

int checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint64_t digest(const Player::VideoFrame& frame) {
    uint64_t value = 1469598103934665603ull;
    for (size_t i = 0; i < frame.size(); i += 1223) value = (value ^ frame.data()[i]) * 1099511628211ull;
    return value;
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const std::string folder = argv[1], app = argv[2];
    const size_t prefix = std::stoull(argv[3]);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    diagnostics_start(folder + "/logs"); http_init(app + "/ca-bundle.crt");
    try {
        std::set<uint64_t> reference;
        std::ifstream hashes(argv[4]); uint64_t value = 0;
        while (hashes >> value) reference.insert(value);
        require(reference.size() > 200, "clean-frame oracle did not contain enough distinct pictures");
        std::ifstream original(folder + "/long-gop.mkv", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(original), {}};
        require(prefix > 0 && prefix < bytes.size(), "invalid live-prefix fixture");
        const auto path = folder + "/stream.part";
        { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), std::streamsize(prefix)); }
        auto source = std::make_shared<GrowingFilePlayback>();
        source->path = path; source->descriptor = ::open(path.c_str(), O_RDONLY);
        source->state = std::make_shared<GrowingFileState>(); source->generation = 1; source->total = int64_t(bytes.size());
        source->state->publish({int64_t(prefix), source->total, 1, GrowingFilePhase::Downloading});
        Player player; Player::Options options; options.url = path; options.growing_download = source;
        player.open(options);
        Player::VideoFrame frame, held;
        unsigned frames = 0, resumed = 0;
        uint64_t held_digest = 0;
        auto pump = [&] {
            require(player.state() != Player::State::Failed, "player failed during live HEVC input");
            if (player.take_frame(frame)) {
                require(frame.w == 640 && frame.h == 360 && frame.wide, "HEVC source format changed");
                require(frame.presentation_time > 0 && (frames > 0 || std::abs(frame.presentation_time - now()) < 2),
                    "per-picture display clock was not propagated through VLC");
                require(reference.count(digest(frame)) != 0, "a corrupted decoded picture reached the output");
                ++frames;
            }
            SDL_Delay(3);
        };
        const double deadline = now() + 12;
        while (now() < deadline && !(frames > 12 && player.buffering() && player.position() > 1.5)) pump();
        require(frames > 12 && player.buffering(), "live input did not reach a real decoder starvation");
        held = frame; held_digest = digest(held);
        const double stopped_at = player.position();
        const double stall_end = now() + 4;
        while (now() < stall_end) pump();
        require(std::abs(player.position() - stopped_at) < 0.1, "starved playback advanced through missing data");
        require(digest(held) == held_digest, "starvation overwrote a held clean picture");
        { std::ofstream output(path, std::ios::binary | std::ios::app);
          output.write(bytes.data() + prefix, std::streamsize(bytes.size() - prefix)); }
        // Keep a live-download state while the appended stream is playing;
        // completion is published only after the decoder catches up.
        source->state->publish({source->total, source->total, 1, GrowingFilePhase::Downloading});
        resumed = frames;
        const double recovery_start = now(), recovery_deadline = now() + 8;
        while (now() < recovery_deadline && player.position() < 10) pump();
        require(frames > resumed + 100 && player.position() > 9.5,
            "HEVC reference pictures were lost across rebuffering; playback could not resume mid-GOP");
        require(digest(held) == held_digest, "resumed decode overwrote a retained render lease");
        source->state->publish({source->total, source->total, 1, GrowingFilePhase::Complete});
        player.close();
        require(digest(held) == held_digest, "decoder close invalidated a retained render lease");
        std::printf("REBUFFER_RESULT {\"result\":\"PASS\",\"checks\":%d,\"frames\":%u,\"frames_after_stall\":%u,\"starved_position\":%.3f,\"recovery_seconds\":%.3f}\n",
            checks, frames, frames - resumed, stopped_at, now() - recovery_start);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "REBUFFER_FAILURE %s\n", error.what()); diagnostics_stop(); SDL_Quit(); return 1;
    }
    diagnostics_stop(); SDL_Quit(); return 0;
}
