// Decode actual AV1 Main 10 at 4K through the production libVLC/dav1d path.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "http.h"
#include <SDL.h>
#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) return 2;
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    Player player; Player::Options options; options.url = argv[1];
    player.open(options);
    unsigned frames = 0;
    bool valid = true;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < end && frames < 4) {
        Player::VideoFrame frame;
        if (player.take_frame(frame)) {
            valid = valid && frame.w == 3840 && frame.h == 2160 && frame.wide &&
                frame.layout == Player::VideoFrame::Layout::Planar420 && frame.size() == 3840u * 2160u * 3u;
            ++frames;
        }
        if (player.state() == Player::State::Failed || player.state() == Player::State::Ended) break;
        SDL_Delay(5);
    }
    valid = valid && frames >= 4 && player.codec_summary().find("software") != std::string::npos;
    player.close(); SDL_Quit();
    std::printf("AV1_RESULT {\"result\":\"%s\",\"frames\":%u,\"width\":3840,\"height\":2160,\"bit_depth\":10}\n",
        valid ? "PASS" : "FAIL", frames);
    return valid ? 0 : 1;
}
