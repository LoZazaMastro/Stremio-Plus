// Exercise native audio failure notifications through the real Player worker.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "diagnostics.h"
#include "http.h"
#include <vlc/vlc.h>
#include <vlc_common.h>
#include <vlc_variables.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>

extern "C" {
libvlc_media_player_t* __real_libvlc_media_player_new(libvlc_instance_t*);
void __real_libvlc_media_player_set_media(libvlc_media_player_t*, libvlc_media_t*);
void __real_libvlc_media_player_stop(libvlc_media_player_t*);
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
void __real_SDL_CloseAudioDevice(SDL_AudioDeviceID);
}

namespace {
std::atomic<bool> pcm_available{true}, fail_during_stop{false};
std::atomic<libvlc_media_player_t*> current_player{nullptr};
std::atomic<unsigned> pcm_opens{0}, pcm_closes{0}, media_opens{0}, injected_failures{0};
unsigned checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void notify(libvlc_media_player_t* player, bool active, const char* reason) {
    auto* object = reinterpret_cast<vlc_object_t*>(player);
    auto callback = reinterpret_cast<void (*)(void*, bool, const char*)>(
        var_GetAddress(object, "stremio-audio-mode"));
    void* opaque = var_GetAddress(object, "amem-data");
    require(callback && opaque, "The real Player did not register the native audio callback");
    callback(opaque, active, reason);
}
}

// The actual HDMI module has its own mocked-import test. This boundary models
// its ownership result while using the real Player, VLC objects and SDL device.
extern "C" bool stremio_ps5_audio_pcm_available() { return pcm_available.load(); }
extern "C" libvlc_media_player_t* __wrap_libvlc_media_player_new(libvlc_instance_t* instance) {
    auto* player = __real_libvlc_media_player_new(instance);
    current_player = player;
    return player;
}
extern "C" void __wrap_libvlc_media_player_set_media(libvlc_media_player_t* player, libvlc_media_t* media) {
    if (media) ++media_opens;
    __real_libvlc_media_player_set_media(player, media);
}
extern "C" void __wrap_libvlc_media_player_stop(libvlc_media_player_t* player) {
    if (fail_during_stop.exchange(false)) {
        ++injected_failures;
        pcm_available = false;
        notify(player, true, "native_shutdown_failed");
    }
    __real_libvlc_media_player_stop(player);
}
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* device, int capture,
    const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained, int allowed) {
    ++pcm_opens;
    return __real_SDL_OpenAudioDevice(device, capture, wanted, obtained, allowed);
}
extern "C" void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID device) {
    ++pcm_closes;
    __real_SDL_CloseAudioDevice(device);
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    diagnostics_start(std::string(argv[1]) + "/logs");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    try {
        for (bool during_stop : {false, true}) {
            // Each case models a fresh native-module lifetime. The real
            // module's process-wide failure latch is never reset at runtime.
            pcm_available = true;
            const unsigned before_pcm = pcm_opens, before_media = media_opens;
            Player player;
            Player::Options options; options.url = argv[3]; options.audio_passthrough = false;
            player.open(options);
            Player::VideoFrame frame;
            unsigned frames = 0;
            double deadline = now() + 10;
            while (now() < deadline && frames < 8) {
                require(player.state() != Player::State::Failed, "Control playback failed before audio injection");
                if (player.take_frame(frame)) ++frames;
                SDL_Delay(4);
            }
            require(frames >= 8 && pcm_opens == before_pcm + 1 && media_opens == before_media + 1,
                "Control playback did not establish exactly one PCM device and media input");
            auto* native_player = current_player.load();
            pcm_available = false;
            notify(native_player, true, "opening");
            notify(native_player, true, "active_ac3");
            const unsigned closed_after_native_open = pcm_closes;

            if (during_stop) {
                // The worker sets input.abort before stopping VLC. The fatal
                // notification must survive that abort and prevent its retry.
                fail_during_stop = true;
                notify(native_player, false, "output_failed");
            } else {
                ++injected_failures;
                notify(native_player, true, "native_shutdown_failed");
            }
            deadline = now() + 5;
            while (now() < deadline && player.state() != Player::State::Failed) SDL_Delay(4);
            require(player.state() == Player::State::Failed,
                "Unsafe native shutdown did not leave playback with an explicit failure");
            require(player.error().find("could not be released safely") != std::string::npos,
                "Native shutdown failure did not retain its useful English error");
            require(player.audio_output_status() == "native_shutdown_failed",
                "Automatic fallback overwrote the native shutdown failure status");
            require(pcm_opens == before_pcm + 1 && media_opens == before_media + 1,
                "Native shutdown failure reopened PCM or restarted the media");
            require(!player.buffering(), "Fatal native output failure was still shown as buffering");
            player.close();
            require(pcm_closes == closed_after_native_open, "Cleanup closed an already released PCM device");

            Player blocked;
            blocked.open(options);
            deadline = now() + 5;
            while (now() < deadline && blocked.state() != Player::State::Failed) SDL_Delay(4);
            require(blocked.state() == Player::State::Failed && !blocked.error().empty(),
                "A new Player ignored native audio ownership remaining unavailable");
            require(pcm_opens == before_pcm + 1 && media_opens == before_media + 1,
                "A new Player opened PCM or media after the native output was quarantined");
            blocked.close();
        }
        std::printf("PLAYER_AUDIO_LIFECYCLE_RESULT {\"result\":\"PASS\",\"checks\":%u,"
            "\"injected_failures\":%u,\"pcm_opens\":%u,\"pcm_closes\":%u,\"media_opens\":%u}\n",
            checks, injected_failures.load(), pcm_opens.load(), pcm_closes.load(), media_opens.load());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PLAYER_AUDIO_LIFECYCLE_FAILURE %s\n", error.what());
        diagnostics_stop(); SDL_Quit(); return 1;
    }
    diagnostics_stop(); SDL_Quit(); return 0;
}
