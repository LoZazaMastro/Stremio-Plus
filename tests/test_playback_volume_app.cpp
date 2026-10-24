// Real App/UI preference routing and decoded PCM captured at SDL submission.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "home_ui.h"
#include "core/input.hpp"
#include "ui/feedback.hpp"
#include "ui/components/component.hpp"
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks = 0, unexpected_http = 0;
int route_override = -1;
void require(bool passed, const char* what) {
    ++checks;
    if (!passed) throw std::runtime_error(what);
}
std::atomic<SDL_AudioCallback> actual_callback{nullptr};
std::atomic<bool> measuring{false};
std::mutex capture_mutex;
struct Capture { unsigned frames = 0; int left = 0, right = 0; } captured;

void observe(const Uint8* bytes, int length) {
    if (!measuring.load() || !bytes || length <= 0) return;
    std::lock_guard lock(capture_mutex);
    for (int offset = 0; offset + 4 <= length; offset += 4) {
        int16_t samples[2];
        std::memcpy(samples, bytes + offset, sizeof(samples));
        captured.left = std::max(captured.left, std::abs(int(samples[0])));
        captured.right = std::max(captured.right, std::abs(int(samples[1])));
        ++captured.frames;
    }
}
void audio_intercept(void* opaque, Uint8* output, int length) {
    if (auto callback = actual_callback.load()) callback(opaque, output, length);
    observe(output, length);
}
double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void pump(Player& player, double duration) {
    const double end = seconds() + duration;
    Player::VideoFrame frame;
    while (seconds() < end) {
        if (player.state() == Player::State::Failed) throw std::runtime_error(player.error());
        player.take_frame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void measure(Player& player, int expected, const char* label) {
    // The reference FFmpeg backend has a <=350 ms SDL queue. Native VLC gain
    // applies at output; this grace period also excludes startup silence.
    measuring = false;
    pump(player, .50);
    { std::lock_guard lock(capture_mutex); captured = {}; }
    measuring = true;
    pump(player, .35);
    measuring = false;
    Capture result;
    { std::lock_guard lock(capture_mutex); result = captured; }
    std::cout << "PCM " << label << " frames=" << result.frames << " left=" << result.left
              << " right=" << result.right << " expected=" << expected << '\n';
    require(result.frames >= 4800, "The real audio path did not deliver enough PCM to measure");
    require(result.left == expected && result.right == expected, "Decoded PCM does not match the selected volume");
}
}

extern "C" {
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
int __real_SDL_QueueAudio(SDL_AudioDeviceID, const void*, Uint32);
bool __real__ZNK6Player24audio_passthrough_activeEv(const Player*);
CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }
bool __wrap__ZNK6Player24audio_passthrough_activeEv(const Player* player) {
    return route_override < 0 ? __real__ZNK6Player24audio_passthrough_activeEv(player) : route_override != 0;
}
SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* name, int capture,
    const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int changes) {
    if (capture || !desired || !desired->callback)
        return __real_SDL_OpenAudioDevice(name, capture, desired, obtained, changes);
    auto hooked = *desired;
    actual_callback = desired->callback;
    hooked.callback = audio_intercept;
    return __real_SDL_OpenAudioDevice(name, capture, &hooked, obtained, changes);
}
int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void* bytes, Uint32 length) {
    observe(static_cast<const Uint8*>(bytes), int(length));
    return __real_SDL_QueueAudio(device, bytes, length);
}
}

struct AppProtocolTest {
    static void prepare(App& app, const std::string& directory) {
        std::filesystem::create_directories(directory);
        app.offline_ = true;
        app.data_dir_ = app.base_dir_ = directory;
        app.settings_.ui_language_mode = app.settings_.ui_language = app.ui_language = "en";
        app.view = "home"; app.zone = "content";
    }
    static void preferences(const std::string& directory) {
        App app; prepare(app, directory);
        require(app.settings_.media_volume == 100, "new profiles preserve full media volume");
        app.settings_.sound_volume = 35;
        app.settings_.media_volume = 65;
        app.settings_.audio_passthrough = true;
        app.settings_.auth_key = "offline-volume-fixture";
        app.offline_ = false; app.save_settings(); app.flush_settings(true);
        App restored; prepare(restored, directory); restored.load_settings();
        require(restored.settings_.media_volume == 65 && restored.settings_.sound_volume == 35,
                "media volume survives a real settings round trip independently of interface sounds");
        require(restored.settings_.audio_passthrough && restored.settings_.auth_key == "offline-volume-fixture",
                "volume persistence preserves passthrough and the existing session");
        const std::pair<json, int> cases[] = {{json(0), 0}, {json(100), 100}, {json(-300), 0},
            {json(1e90), 100}, {json(-1e90), 0}, {json(42.5), 100}, {json("50"), 100},
            {json(true), 100}, {json(nullptr), 100}};
        for (const auto& [value, expected] : cases) {
            require(save_json(directory + "/settings.json", json{{"media_volume", value}}), "volume fixture saved");
            App next; prepare(next, directory); next.load_settings();
            if (next.settings_.media_volume != expected)
                std::cerr << "preference=" << value.dump() << " actual=" << next.settings_.media_volume
                          << " expected=" << expected << '\n';
            require(next.settings_.media_volume == expected, "invalid or boundary media-volume preference was mishandled");
        }
        require(save_json(directory + "/settings.json", json{{"sound_volume", 10}}), "legacy preference fixture saved");
        App legacy; prepare(legacy, directory); legacy.load_settings();
        require(legacy.settings_.media_volume == 100 && legacy.settings_.sound_volume == 10,
                "legacy profiles do not confuse interface volume with media volume");
    }
    static void input(const std::string& directory) {
        App app; prepare(app, directory);
        app.settings_.media_volume = 50; app.player_.set_volume(50);
        app.watching_ = true; app.info_visible = false;
        app.home_row = 2; app.home_col = 3;
        route_override = 0;
        app.watch_volume_input(0, 0, true, 10);
        app.watch_volume_input(0, -1, true, 11);
        require(app.settings_.media_volume == 55 && app.player_.volume() == 55 && app.w_volume_visible,
                "right-stick up reaches both saved settings and the player");
        require(!app.info_visible && app.home_row == 2 && app.home_col == 3,
                "volume does not reveal controls or move selection");
        app.watch_volume_update(12.79);
        require(app.w_volume_visible, "volume indicator remains visible during its bounded timeout");
        app.watch_volume_update(12.81);
        require(!app.w_volume_visible, "volume indicator disappears after its timeout");
        app.watch_volume_input(0, 0, true, 13);
        app.watch_volume_input(0, 1, true, 14);
        require(app.settings_.media_volume == 50 && app.player_.volume() == 50,
                "right-stick down lowers the media level");
        app.watch_volume_input(0, -1, false, 15);
        app.watch_volume_input(0, -1, true, 16);
        require(app.settings_.media_volume == 50 && !app.w_volume_visible,
                "focus loss clears stale input and hides the temporary indicator");
        app.watch_volume_input(0, 0, true, 17);
        app.settings_.audio_passthrough = true;
        app.offline_ = false; app.save_settings(); app.flush_settings(true);
        route_override = 1;
        const auto epoch = app.settings_io_epoch_->load();
        app.watch_volume_input(0, -1, true, 18);
        require(app.w_volume_visible && app.audio_passthrough_active(),
                "active bitstream selects the receiver-volume guidance path");
        require(app.settings_.media_volume == 50 && app.player_.volume() == 50 && app.settings_io_epoch_->load() == epoch,
                "passthrough input changes neither PCM gain nor the persisted level");
        json stored;
        require(load_json(directory + "/settings.json", stored) && jnum(stored, "media_volume", -1) == 50,
                "active bitstream leaves the actual settings file at its previous PCM volume");
        app.offline_ = true;
        route_override = 0;
        app.watch_volume_input(0, 0, true, 19);
        app.watch_volume_input(0, -1, true, 20);
        require(app.settings_.media_volume == 55, "actual PCM fallback remains adjustable when passthrough was requested");
        app.launch_visible = true;
        app.watch_volume_input(0, 1, true, 21);
        require(app.settings_.media_volume == 55 && !app.w_volume_visible, "loading overlays cannot accidentally adjust volume");
        app.launch_visible = false;
        app.watch_volume_input(0, 1, true, 22);
        require(app.settings_.media_volume == 55, "leaving a modal with a held stick requires neutral first");

        HomeUi ui;
        hui::ui::Feedback feedback;
        auto send = [&](float y, hui::Action action = hui::Action::count) {
            hui::InputFrame frame;
            frame.connected = true; frame.stick2_y = y;
            if (action != hui::Action::count) frame.pressed = hui::action_bit(action);
            ui.update(app, .016f); ui.handle(app, frame, feedback); ui.update(app, .016f);
        };
        send(0); send(-1);
        require(app.settings_.media_volume == 60 && !app.info_visible,
                "real HomeUi routing forwards right-stick volume without opening transport");
        const double delay = app.w_sub_delay_;
        send(0, hui::Action::jump_prev);
        require(app.w_sub_delay_ == delay - .25 && app.settings_.media_volume == 60,
                "L2 retains subtitle-delay behavior and does not adjust volume");
        send(0, hui::Action::jump_next);
        require(app.w_sub_delay_ == delay && app.settings_.media_volume == 60,
                "R2 retains subtitle-delay behavior and does not adjust volume");
        app.watching_ = false; app.view = "downloads";
        const int sort = app.settings_.download_sort;
        send(0, hui::Action::jump_next);
        require(app.settings_.download_sort == (sort + 1) % 3 && app.settings_.media_volume == 60,
                "R2 retains Downloads sorting outside playback");
        send(0, hui::Action::jump_prev);
        require(app.settings_.download_sort == sort && app.settings_.media_volume == 60,
                "L2 retains reverse Downloads sorting outside playback");
        send(-1);
        require(app.settings_.media_volume == 60, "right stick outside playback never changes saved media volume");
        route_override = -1;
    }
    static void intercepted_input(const std::string& directory) {
        App app; prepare(app, directory);
        app.watching_ = true; app.info_visible = false;
        app.settings_.media_volume = 50; app.player_.set_volume(50);
        HomeUi ui;
        hui::InputTracker tracker;
        hui::ui::Feedback feedback;
        hui::PadSample sample;
        sample.connected = true;
        std::uint64_t tick = 0;
        route_override = 0;
        const auto dispatch = [&](std::span<const hui::PadSample> samples) {
            tick += 16000;
            auto frame = tracker.update(samples, tick);
            ui.update(app, .016f); ui.handle(app, frame, feedback); ui.update(app, .016f);
            return frame;
        };
        dispatch({&sample, 1});
        sample.right_y = 0;
        dispatch({&sample, 1});
        require(app.settings_.media_volume == 55, "real raw-pad right-stick input reaches volume");
        sample.buttons = hui::pad_bits::kIntercepted;
        auto frame = dispatch({&sample, 1});
        require(frame.focus_lost && frame.intercepted && frame.connected && !app.w_volume_visible,
                "the system ownership transition resets the volume control");
        frame = dispatch({&sample, 1});
        require(!frame.focus_lost && frame.intercepted && frame.stick2_y == 0,
                "system ownership stays explicit after the single focus-lost edge");
        frame = dispatch({});
        require(frame.intercepted && !frame.focus_lost,
                "a frame without new samples preserves intercepted ownership");
        sample.buttons = 0;
        frame = dispatch({&sample, 1});
        require(!frame.intercepted && app.settings_.media_volume == 55 && !app.w_volume_visible,
                "returning from the system overlay with the stick held never changes volume");
        dispatch({&sample, 1});
        require(app.settings_.media_volume == 55,
                "a second held frame cannot rearm the control using intercepted zeroes");
        sample.right_y = 128; dispatch({&sample, 1});
        sample.right_y = 0; dispatch({&sample, 1});
        require(app.settings_.media_volume == 60 && app.player_.volume() == 60,
                "a real recenter re-enables volume after system interception");
        require(app.home_row == 0 && app.home_col == 0 && !app.info_visible,
                "raw right-stick and system ownership events leave navigation and transport alone");
        route_override = -1;
    }
    static void indicator(const std::string& directory, const std::string& font_directory) {
        App app; prepare(app, directory);
        app.watching_ = true; app.info_visible = false; app.w_volume_visible = true;
        app.settings_.media_volume = 65;
        hui::gfx::Font regular, semibold;
        std::string bytes;
        require(read_file(font_directory + "/inter-regular.huifont", bytes) && regular.load(bytes),
                "production regular font loads without a GL context");
        require(read_file(font_directory + "/inter-semibold.huifont", bytes) && semibold.load(bytes),
                "production semibold font loads without a GL context");
        hui::ui::Fonts fonts;
        fonts.regular = fonts.mono = fonts.pixel = fonts.hand = {&regular, hui::gfx::kFontHandleBase + 1};
        fonts.semibold = fonts.display = {&semibold, hui::gfx::kFontHandleBase + 2};
        hui::gfx::DrawList list;
        hui::ui::Canvas canvas{list, fonts};
        HomeUi ui; ui.snap(app);
        std::vector<hui::gfx::GlyphQuad> percent;
        semibold.layout("%", 0, 0, 28, hui::gfx::Align::left, percent);
        require(percent.size() == 1, "the production percentage glyph is identifiable");
        const auto count_percent = [&] {
            return std::count_if(list.instances().begin(), list.instances().end(), [&](const auto& instance) {
                const auto& glyph = percent.front();
                return instance.params[3] == float(hui::gfx::Shape::glyph) && instance.params[2] == 2 &&
                    instance.extra[0] == glyph.u0 && instance.extra[1] == glyph.v0 &&
                    instance.extra[2] == glyph.u1 && instance.extra[3] == glyph.v1;
            });
        };
        route_override = 0;
        ui.draw_overlays(app, canvas);
        require(count_percent() == 1, "the real PCM indicator draws one percentage");
        route_override = 1; list.clear();
        ui.draw_overlays(app, canvas);
        require(!list.empty() && count_percent() == 0,
                "the real passthrough indicator displays guidance without a fictitious percentage");
        require(std::all_of(list.instances().begin(), list.instances().end(), [](const auto& instance) {
            return instance.rect[0] >= 1296 && instance.rect[1] >= 72 &&
                instance.rect[0] + instance.rect[2] <= 1824 && instance.rect[1] + instance.rect[3] <= 228;
        }), "passthrough guidance fits its panel using the actual font atlas");
        route_override = 0; list.clear();
        ui.draw_overlays(app, canvas);
        require(count_percent() == 1, "PCM fallback updates the indicator immediately without a stale route snapshot");
        app.w_volume_visible = false; list.clear();
        ui.draw_overlays(app, canvas);
        require(list.empty(), "the expired standalone indicator emits no remaining overlay geometry");
        route_override = -1;
    }
};

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        AppProtocolTest::preferences(std::string(argv[1]) + "/preferences");
        AppProtocolTest::input(std::string(argv[1]) + "/input");
        AppProtocolTest::intercepted_input(std::string(argv[1]) + "/intercepted-input");
        AppProtocolTest::indicator(std::string(argv[1]) + "/indicator", argv[3]);
        require(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0, "SDL dummy audio did not initialize");
        Player player;
        Player::Options options; options.url = argv[2]; options.volume_percent = 50;
        player.open(options);
        require(player.volume() == 50, "saved volume is installed before playback starts");
        const double deadline = seconds() + 8;
        while (player.state() != Player::State::Playing && seconds() < deadline) pump(player, .02);
        require(player.state() == Player::State::Playing, "PCM fixture did not start playing");
        measure(player, 6000, "initial 50 percent");
        player.set_volume(100); measure(player, 12000, "100 percent");
        player.set_volume(0); measure(player, 0, "mute");
        player.set_volume(25); measure(player, 3000, "25 percent after mute");
        player.set_volume(999); measure(player, 12000, "upper clamp");
        player.close();
        SDL_Quit();
        require(unexpected_http == 0, "volume controls unexpectedly attempted a network request");
        std::cout << "{\"result\":\"PASS\",\"checks\":" << checks
                  << ",\"actual_pcm_capture\":true,\"mocked_bitstream_route\":true}\n";
    } catch (const std::exception& error) {
        measuring = false;
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
