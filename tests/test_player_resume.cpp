// Real App/VLC/SDL/GL resume after delayed HTTP metadata, with automatic subtitles.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "diagnostics.h"
#include "http.h"
#include "video_gl.h"
#include "gfx/gl_program.hpp"
#include <vlc/vlc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>

extern "C" {
void __real_libvlc_media_player_set_media(libvlc_media_player_t*, libvlc_media_t*);
void __real_libvlc_media_player_set_time(libvlc_media_player_t*, libvlc_time_t);
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
}

namespace {
std::atomic<unsigned> media_opens{0}, requested_seeks{0}, unexpected_tone_blocks{0}, correct_tone_blocks{0};
std::atomic<uint64_t> audible_frames{0};
std::atomic<SDL_AudioCallback> audio_callback{nullptr};
std::mutex seeks_mutex;
std::vector<double> seeks;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void audio_intercept(void* opaque, Uint8* output, int length) {
    if (auto callback = audio_callback.load()) callback(opaque, output, length);
    const int frames = length / 4;
    if (frames <= 0) return;
    unsigned nonzero = 0, crossings = 0;
    int previous_sign = 0;
    for (int frame = 0; frame < frames; ++frame) {
        int16_t sample = 0;
        std::memcpy(&sample, output + frame * 4, sizeof(sample));
        nonzero += sample != 0;
        const int sign = sample > 0 ? 1 : sample < 0 ? -1 : 0;
        if (sign && previous_sign && sign != previous_sign) ++crossings;
        if (sign) previous_sign = sign;
    }
    audible_frames += nonzero;
    if (nonzero < unsigned(frames * 9 / 10)) return;
    const double hz = double(crossings) * 48000 / (2 * frames);
    if (hz > 700 && hz < 1000) ++correct_tone_blocks;
    if (hz > 300 && hz < 600) ++unexpected_tone_blocks;
}
struct Graphics {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    Graphics() {
        const auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
        EGLint major = 0, minor = 0;
        require(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor) && eglBindAPI(EGL_OPENGL_API),
            "Headless OpenGL initialization failed");
        const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
        require(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
            "Headless OpenGL context creation failed");
        hui::gfx::set_glsl_prefix("#version 450 core\n");
    }
    ~Graphics() {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
};
}

extern "C" void __wrap_libvlc_media_player_set_media(libvlc_media_player_t* player, libvlc_media_t* media) {
    if (media) ++media_opens;
    __real_libvlc_media_player_set_media(player, media);
}
extern "C" void __wrap_libvlc_media_player_set_time(libvlc_media_player_t* player, libvlc_time_t time) {
    ++requested_seeks;
    { std::lock_guard lock(seeks_mutex); seeks.push_back(double(time) / 1000); }
    __real_libvlc_media_player_set_time(player, time);
}
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* name, int capture,
    const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int changes) {
    if (capture || !desired || !desired->callback)
        return __real_SDL_OpenAudioDevice(name, capture, desired, obtained, changes);
    auto hooked = *desired;
    audio_callback = desired->callback;
    hooked.callback = audio_intercept;
    return __real_SDL_OpenAudioDevice(name, capture, &hooked, obtained, changes);
}

struct AppProtocolTest {
    static void open(App& app, const std::string& app_dir, const std::string& data_dir, const std::string& url) {
        std::filesystem::create_directories(data_dir);
        require(app.init(app_dir, data_dir, true), "Real App initialization failed");
        app.settings_.auto_subtitles = true;
        app.settings_.subtitle_langs = "ita";
        app.settings_.audio_langs = "eng";
        app.settings_.autoplay_next = false;
        app.w_offline_ = true;
        app.w_item_.id = app.w_video_id_ = "resume-fixture";
        app.w_item_.type = "movie";
        app.w_item_.name = "Resume fixture";
        app.view = "downloads";
        app.watch_start(url, {"X-Stremio-Test: resume-fixture", "Authorization: Bearer resume-fixture-credential"},
            10, "Resume fixture", {}, false);
    }
    static Player::State state(const App& app) { return app.player_.state(); }
    static double position(App& app) { return app.player_.position(); }
    static bool subtitles_selected(const App& app) { return app.w_auto_sub_done_ && app.w_sub_active_ >= 0; }
    static std::string subtitle_text(App& app) {
        if (app.w_sub_active_ < 0) return {};
        return app.player_.embedded_subtitle(app.w_subs_.at(app.w_sub_active_).embedded, app.player_.position());
    }
    static void expire_controls(App& app) { app.info_visible = false; app.w_info_until_ = now_seconds() - 1; }
    static double controls_remaining(const App& app) { return app.w_info_until_ - now_seconds(); }
};

namespace {
struct Session {
    App app;
    VideoGl gpu;
    Player::VideoFrame frame;
    unsigned frames = 0, wrong_video_frames = 0;
    double first_pts = -1, first_frame_after = -1, minimum_pts = 1e30;
    double opened_at = seconds();
    bool acknowledge = false;
    Session() { require(gpu.init(), "Production video shader did not initialize"); }
    ~Session() { app.shutdown(); gpu.release(); }
    void step() {
        app.update();
        if (!app.take_video_frame(frame)) return;
        require(gpu.upload(frame) && gpu.texture(), "Production GL upload failed");
        if (!frames) { first_pts = frame.pts; first_frame_after = seconds() - opened_at; }
        minimum_pts = std::min(minimum_pts, frame.pts);
        std::vector<unsigned char> pixels(size_t(frame.w) * frame.h * 4);
        glBindTexture(GL_TEXTURE_2D, gpu.texture());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        require(glGetError() == GL_NO_ERROR, "Rendered texture readback failed");
        const size_t center = (size_t(frame.h / 2) * frame.w + frame.w / 2) * 4;
        if (pixels[center + 2] < pixels[center] + 80) ++wrong_video_frames;
        ++frames;
        if (acknowledge) app.video_frame_presented();
    }
    void until(const std::function<bool()>& condition, double timeout, const char* message) {
        const double deadline = seconds() + timeout;
        do {
            step();
            if (condition()) { require(true, message); return; }
            if (!app.watching()) break;
            SDL_Delay(4);
        } while (seconds() < deadline);
        std::cerr << "RESUME_TIMEOUT " << message << " state=" << int(AppProtocolTest::state(app))
            << " position=" << AppProtocolTest::position(app) << " frames=" << frames
            << " opens=" << media_opens << " seeks=" << requested_seeks << '\n';
        require(false, message);
    }
};
void delayed_resume(const std::string& folder, const std::string& app_dir, const std::string& base, bool baseline) {
    Session session;
    AppProtocolTest::open(session.app, app_dir, folder + "/resume-data", base + "/resume.mkv");
    session.until([&] { return session.frames >= 3 && AppProtocolTest::subtitles_selected(session.app); },
        15, "Delayed resume did not deliver video and select subtitles through the real App");
    require(session.first_frame_after >= 6, "The input was not held beyond the old five-second startup deadline");
    require(session.app.launch_visible, "Startup closed before the actual renderer acknowledgement");
    AppProtocolTest::expire_controls(session.app);
    session.app.video_frame_presented();
    session.acknowledge = true;
    session.app.update();
    require(!session.app.launch_visible && session.app.info_visible && AppProtocolTest::controls_remaining(session.app) > 4.5,
        "First GPU acknowledgement did not expose the player and reset its controls timer");
    const double observation_until = seconds() + 3;
    session.until([&] { return seconds() >= observation_until; }, 4,
        "Playback stopped while automatic subtitles were being applied");
    json record{{"name", "delayed-resume-auto-subtitles"}, {"first_frame_after_seconds", session.first_frame_after},
        {"first_presented_pts", session.first_pts}, {"minimum_presented_pts", session.minimum_pts},
        {"position", AppProtocolTest::position(session.app)}, {"frames", session.frames},
        {"wrong_video_frames", session.wrong_video_frames}, {"correct_tone_blocks", correct_tone_blocks.load()},
        {"unexpected_tone_blocks", unexpected_tone_blocks.load()}, {"media_opens", media_opens.load()},
        {"unrequested_seeks", requested_seeks.load()}, {"subtitle_selected", AppProtocolTest::subtitles_selected(session.app)},
        {"subtitle_text", AppProtocolTest::subtitle_text(session.app)}};
    { std::lock_guard lock(seeks_mutex); record["seek_targets"] = seeks; }
    std::cout << "RESUME_CASE " << record.dump() << '\n';
    if (baseline) {
        require(requested_seeks > 0, "Baseline did not reproduce an unsolicited subtitle seek");
        require(session.first_pts < 1 || session.minimum_pts < 1 || session.wrong_video_frames > 0,
            "Baseline did not reproduce a false starting position or an actual rewind");
    } else {
        require(session.first_pts >= 9.8 && session.first_pts < 11.5 && session.minimum_pts >= 9.8,
            "Resume exposed an unconfirmed zero position or moved back before its target");
        require(media_opens == 1 && requested_seeks == 0, "Automatic subtitles sought or reopened the stream");
        require(session.wrong_video_frames == 0 && unexpected_tone_blocks == 0 && correct_tone_blocks >= 20,
            "Decoded video or audible PCM came from before the saved resume point");
        require(AppProtocolTest::position(session.app) >= 12 && !AppProtocolTest::subtitle_text(session.app).empty(),
            "The resumed timeline or new subtitle cues stopped advancing");
    }
}
void cancel_delayed_input(const std::string& folder, const std::string& app_dir, const std::string& base) {
    Session session;
    AppProtocolTest::open(session.app, app_dir, folder + "/cancel-data", base + "/cancel.mkv");
    const double delay_until = seconds() + 1;
    session.until([&] { return seconds() >= delay_until; }, 2, "Cancellation fixture did not enter its delayed open");
    require(session.frames == 0 && session.app.launch_visible, "Cancellation fixture started before its HTTP delay");
    const unsigned before = media_opens;
    const double closing = seconds();
    session.app.on_button(Btn::Circle);
    if (session.app.watching()) session.app.on_button(Btn::Circle);
    require(!session.app.watching() && !session.app.launch_visible && seconds() - closing < 3,
        "Circle did not cancel and close the pending input promptly");
    const auto stopped_audio = audible_frames.load();
    SDL_Delay(100);
    require(media_opens == before && audible_frames == stopped_audio, "Cancelled input reopened or kept audio running");
    std::cout << "RESUME_CASE " << json{{"name", "cancel-delayed-input"},
        {"close_seconds", seconds() - closing}, {"frames", session.frames}, {"new_media_opens", media_opens.load() - before}}.dump() << '\n';
}
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) return 2;
    const bool baseline = argc == 5 && std::string(argv[4]) == "baseline";
    std::cout.setf(std::ios::unitbuf);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) return 3;
    diagnostics_start(std::string(argv[1]) + "/diagnostics");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    g_tasks.start(2);
    int result = 0;
    try {
        Graphics graphics;
        delayed_resume(argv[1], argv[2], argv[3], baseline);
        if (!baseline) cancel_delayed_input(argv[1], argv[2], argv[3]);
        std::cout << (baseline ? "PLAYER_RESUME_BUG_REPRODUCED" : "PLAYER_RESUME_TESTS_OK") << " checks=" << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << "PLAYER_RESUME_FAILURE " << diagnostics_redact(error.what()) << '\n';
        result = 1;
    }
    g_tasks.stop();
    diagnostics_stop();
    SDL_Quit();
    return result;
}
