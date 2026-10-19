// Real libVLC video-decoder error propagation and bounded application recovery.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "diagnostics.h"
#include "http.h"
#include <vlc/vlc.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
void __real_libvlc_media_player_set_media(libvlc_media_player_t*, libvlc_media_t*);
}
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
std::atomic<int> armed_error{0};
std::atomic<unsigned> media_opens{0}, injected{0};
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
        if (const int error = armed_error.exchange(0)) {
            ++injected;
            return error;
        }
    }
    return __real_avcodec_send_packet(context, packet);
}
extern "C" void __wrap_libvlc_media_player_set_media(libvlc_media_player_t* player, libvlc_media_t* media) {
    if (media) ++media_opens;
    __real_libvlc_media_player_set_media(player, media);
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    diagnostics_start(std::string(argv[1]) + "/logs");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    try {
        Player player;
        Player::Options options;
        options.url = argv[3]; options.start = 4;
        player.open(options);
        Player::VideoFrame frame, held;
        unsigned frames = 0, resumed_frames = 0;
        double minimum_resumed_pts = 1e9;
        auto pump = [&] {
            if (player.take_frame(frame)) {
                require(frame.w == 640 && frame.h == 360 && frame.wide,
                    "Recovery changed the decoded source format");
                ++frames;
            }
            SDL_Delay(4);
        };
        double deadline = now() + 10;
        while (now() < deadline && player.position() < 5) {
            require(player.state() != Player::State::Failed, "Initial playback failed");
            pump();
        }
        require(frames > 8 && player.position() >= 5 && media_opens == 1,
            "Fixture did not establish playback from the saved position");
        held = frame;
        const auto held_digest = digest(held);
        const double failed_at = player.position();
        armed_error = AVERROR(ENOMEM);
        bool reopening = false, recovery_buffer = false;
        deadline = now() + 12;
        while (now() < deadline && !(media_opens == 2 && player.position() > failed_at + 1.5)) {
            require(player.state() != Player::State::Failed, "Transient decoder failure was not recovered");
            reopening |= injected > 0 && player.state() == Player::State::Opening;
            int percent = 100;
            if (injected && player.buffering(&percent)) recovery_buffer |= percent < 100;
            const unsigned before = frames;
            pump();
            if (media_opens == 2 && frames != before) {
                ++resumed_frames;
                minimum_resumed_pts = std::min(minimum_resumed_pts, frame.pts);
            }
        }
        require(injected == 1 && media_opens == 2 && resumed_frames > 12,
            "Critical error did not reopen and resume the real VLC input exactly once");
        require(reopening && recovery_buffer,
            "A decoder failure retained the healthy local-file 100 percent state");
        require(minimum_resumed_pts >= failed_at - .25 && player.position() > failed_at + 1.5,
            "Recovery restarted from zero or failed to advance from the last presented picture");
        require(digest(held) == held_digest, "Recovery invalidated a retained clean-picture lease");

        // A second critical failure must finish with a usable error, rather
        // than loop through reopen or keep an apparently healthy frozen frame.
        armed_error = AVERROR(EINVAL);
        deadline = now() + 10;
        while (now() < deadline && player.state() != Player::State::Failed) pump();
        require(injected == 2 && player.state() == Player::State::Failed && !player.error().empty(),
            "Repeated decoder failure did not leave playback with an explicit error");
        require(media_opens == 2, "Persistent decoder failure caused an unbounded restart loop");
        require(!player.buffering(), "Failed playback was still displayed as buffering");
        player.close();
        require(digest(held) == held_digest, "Closing recovered playback freed a retained picture");
        std::printf("DECODER_RECOVERY_RESULT {\"result\":\"PASS\",\"checks\":%u,\"frames\":%u,"
            "\"resumed_frames\":%u,\"media_opens\":%u,\"injected_errors\":%u,\"failed_position\":%.3f,"
            "\"minimum_resumed_position\":%.3f}\n", checks, frames, resumed_frames,
            media_opens.load(), injected.load(), failed_at, minimum_resumed_pts);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DECODER_RECOVERY_FAILURE %s\n", error.what());
        diagnostics_stop(); SDL_Quit(); return 1;
    }
    diagnostics_stop(); SDL_Quit(); return 0;
}
