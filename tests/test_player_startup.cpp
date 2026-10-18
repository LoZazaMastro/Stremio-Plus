// Real VLC, SDL output, GPU upload and App startup transitions.
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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
void __real_libvlc_video_set_callbacks(libvlc_media_player_t*, libvlc_video_lock_cb,
    libvlc_video_unlock_cb, libvlc_video_display_cb, void*);
void __real_libvlc_media_player_set_media(libvlc_media_player_t*, libvlc_media_t*);
int __real_libvlc_event_attach(libvlc_event_manager_t*, libvlc_event_type_t, libvlc_callback_t, void*);
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
}

namespace {
enum class Fault { Off, Healthy, FirstMediaVideo, AllVideo, CacheHalf };
std::atomic<Fault> fault{Fault::Off};
std::atomic<unsigned> media_opens{0}, video_callbacks{0}, lost_callbacks{0};
std::atomic<uint64_t> audible_frames{0};
std::atomic<libvlc_video_display_cb> display_callback{nullptr};
std::atomic<SDL_AudioCallback> audio_callback{nullptr};
std::atomic<libvlc_callback_t> event_callback{nullptr};
int checks = 0;

void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void display_intercept(void* opaque, void* picture) {
    ++video_callbacks;
    const auto active = fault.load();
    if (active == Fault::AllVideo || (active == Fault::FirstMediaVideo && media_opens.load() == 1)) {
        ++lost_callbacks;
        return;
    }
    if (auto callback = display_callback.load()) callback(opaque, picture);
}
void audio_intercept(void* opaque, Uint8* output, int length) {
    if (auto callback = audio_callback.load()) callback(opaque, output, length);
    if (fault == Fault::Off || length <= 0) return;
    uint64_t nonzero = 0;
    for (int offset = 0; offset + 4 <= length; offset += 4) {
        int16_t channels[2];
        std::memcpy(channels, output + offset, 4);
        nonzero += channels[0] != 0 || channels[1] != 0;
    }
    audible_frames.fetch_add(nonzero);
}
void event_intercept(const libvlc_event_t* event, void* opaque) {
    const auto callback = event_callback.load();
    if (!callback) return;
    if (fault != Fault::CacheHalf) { callback(event, opaque); return; }
    if (event->type != libvlc_MediaPlayerBuffering) callback(event, opaque);
    libvlc_event_t cache = *event;
    cache.type = libvlc_MediaPlayerBuffering;
    cache.u.media_player_buffering.new_cache = 50.0f;
    callback(&cache, opaque);
}

struct FaultScope {
    explicit FaultScope(Fault mode) {
        media_opens = video_callbacks = lost_callbacks = 0;
        audible_frames = 0;
        fault = mode;
    }
    ~FaultScope() { fault = Fault::Off; }
};
struct Graphics {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    Graphics() {
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
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
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            eglTerminate(display);
        }
    }
};
std::string contents(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
}

extern "C" void __wrap_libvlc_video_set_callbacks(libvlc_media_player_t* player, libvlc_video_lock_cb lock,
    libvlc_video_unlock_cb unlock, libvlc_video_display_cb display, void* opaque) {
    if (fault == Fault::Off) { __real_libvlc_video_set_callbacks(player, lock, unlock, display, opaque); return; }
    display_callback = display;
    __real_libvlc_video_set_callbacks(player, lock, unlock, display_intercept, opaque);
}
extern "C" void __wrap_libvlc_media_player_set_media(libvlc_media_player_t* player, libvlc_media_t* media) {
    if (fault != Fault::Off && media) ++media_opens;
    __real_libvlc_media_player_set_media(player, media);
}
extern "C" int __wrap_libvlc_event_attach(libvlc_event_manager_t* manager, libvlc_event_type_t type,
    libvlc_callback_t callback, void* opaque) {
    if (fault == Fault::CacheHalf && (type == libvlc_MediaPlayerPlaying || type == libvlc_MediaPlayerBuffering)) {
        event_callback = callback;
        return __real_libvlc_event_attach(manager, type, event_intercept, opaque);
    }
    return __real_libvlc_event_attach(manager, type, callback, opaque);
}
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* name, int capture,
    const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int changes) {
    if (fault == Fault::Off || capture || !desired || !desired->callback)
        return __real_SDL_OpenAudioDevice(name, capture, desired, obtained, changes);
    auto hooked = *desired;
    audio_callback = desired->callback;
    hooked.callback = audio_intercept;
    return __real_SDL_OpenAudioDevice(name, capture, &hooked, obtained, changes);
}

struct AppProtocolTest {
    static void open(App& app, const std::string& app_dir, const std::string& data_dir,
        const std::string& media, double resume = 0, const std::shared_ptr<GrowingFilePlayback>& growing = {}) {
        std::filesystem::create_directories(data_dir);
        require(app.init(app_dir, data_dir, true), "Real App initialization failed");
        app.settings_.auto_subtitles = false;
        app.settings_.autoplay_next = false;
        app.w_offline_ = true;
        app.w_item_.id = "startup-fixture";
        app.w_item_.type = "movie";
        app.w_item_.name = "Startup fixture";
        app.w_video_id_ = "startup-fixture";
        app.w_growing_download_ = growing;
        app.view = "downloads";
        app.watch_start(media, {}, resume, "Startup fixture", {}, false);
    }
    static Player::State state(App& app) { return app.player_.state(); }
    static bool buffering(App& app, int* percent) { return app.player_.buffering(percent); }
    static void expire_controls(App& app) { app.info_visible = false; app.w_info_until_ = now_seconds() - 1; }
    static double controls_remaining(const App& app) { return app.w_info_until_ - now_seconds(); }
    static const GrowingFilePlayback* growing(const App& app) { return app.w_growing_download_.get(); }
};

namespace {
struct Session {
    App app;
    VideoGl gpu;
    Player::VideoFrame frame;
    unsigned frames = 0;
    unsigned rejected_frames = 0;
    double first_pts = -1;
    bool acknowledge = true;
    bool reject_upload = false;
    Session() { require(gpu.init(), "Production video shader did not initialize"); }
    ~Session() { app.shutdown(); gpu.release(); }
    void step() {
        // Same ordering as main.cpp: App update, VLC frame, GL upload, acknowledgement.
        app.update();
        if (!app.take_video_frame(frame)) return;
        if (reject_upload) {
            auto invalid = frame;
            invalid.w = 0;
            require(!gpu.upload(invalid), "Invalid decoded picture unexpectedly passed the production GPU validator");
            ++rejected_frames;
#ifndef STREMIO_STARTUP_BASELINE
            app.video_frame_rejected();
#endif
            return;
        }
        require(gpu.upload(frame) && gpu.texture() != 0, "Production GPU upload rejected a decoded VLC picture");
        if (!frames) {
            first_pts = frame.pts;
            std::vector<unsigned char> pixels(size_t(frame.w) * frame.h * 4);
            glBindTexture(GL_TEXTURE_2D, gpu.texture());
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            require(glGetError() == GL_NO_ERROR, "Decoded video texture readback failed");
            bool visible = false;
            for (size_t i = 0; i + 3 < pixels.size(); i += 4)
                visible = visible || pixels[i] > 16 || pixels[i + 1] > 16 || pixels[i + 2] > 16;
            require(visible, "Decoded video texture is black");
        }
        ++frames;
#ifndef STREMIO_STARTUP_BASELINE
        if (acknowledge) app.video_frame_presented();
#endif
    }
    void until(const std::function<bool()>& condition, double timeout, const char* message) {
        const double deadline = seconds() + timeout;
        do {
            step();
            if (condition()) { require(true, message); return; }
            SDL_Delay(4);
        } while (seconds() < deadline);
        std::cerr << "STARTUP_TIMEOUT " << message << " state=" << int(AppProtocolTest::state(app))
            << " frames=" << frames << " opens=" << media_opens << " audio_seconds=" << double(audible_frames) / 48000
            << " loader=" << app.launch_visible << " toast=" << diagnostics_redact(app.toast) << '\n';
        require(false, message);
    }
};
void record(const char* name, const Session& session) {
    std::cout << "STARTUP_CASE " << json{{"name", name}, {"frames", session.frames},
        {"media_opens", media_opens.load()}, {"audible_seconds", double(audible_frames.load()) / 48000},
        {"video_callbacks", video_callbacks.load()}, {"dropped_video_callbacks", lost_callbacks.load()},
        {"rejected_gpu_frames", session.rejected_frames},
        {"launch_visible", session.app.launch_visible}, {"watching", session.app.watching()},
        {"first_presented_pts", session.first_pts}}.dump() << '\n';
}
void healthy(const std::string& folder, const std::string& app_dir) {
    FaultScope injected(Fault::Healthy);
    Session session;
    AppProtocolTest::open(session.app, app_dir, folder + "/healthy-data", folder + "/startup.mp4");
    session.until([&] { return session.frames > 2 && !session.app.launch_visible && audible_frames > 12000; },
        5, "Healthy real VLC/GL playback did not leave startup");
    require(media_opens == 1, "Healthy playback unnecessarily reopened its input");
    session.app.on_button(Btn::Up);
    require(session.app.info_visible && session.app.watching(), "Up did not reveal playback controls");
    session.app.on_button(Btn::Circle);
    require(!session.app.info_visible && session.app.watching(), "Circle did not hide only the playback controls");
    record("healthy", session);
}
#ifndef STREMIO_STARTUP_BASELINE
void rejected_gpu_frames(const std::string& folder, const std::string& app_dir) {
    FaultScope injected(Fault::Healthy);
    Session session;
    session.reject_upload = true;
    AppProtocolTest::open(session.app, app_dir, folder + "/gpu-error-data", folder + "/startup.mp4");
    session.until([&] { return !session.app.watching(); }, 5,
        "Repeated GPU rejection left playback running behind its launch screen");
    require(session.rejected_frames == 3 && session.frames == 0 && !session.app.launch_visible,
        "GPU failure did not stop after exactly three rejected frames without acknowledging any picture");
    require(session.app.toast == "The video frame could not be displayed.",
        "GPU rejection did not expose its specific display error");
    const auto stopped_at = audible_frames.load();
    SDL_Delay(100);
    require(audible_frames == stopped_at, "GPU failure left audio output running");
    record("bounded-gpu-output-failure", session);
}
void audio_only(const std::string& folder, const std::string& app_dir) {
    FaultScope injected(Fault::Healthy);
    Session session;
    AppProtocolTest::open(session.app, app_dir, folder + "/audio-data", folder + "/startup.m4a");
    session.until([&] { return !session.app.launch_visible && audible_frames > 12000; },
        5, "Confirmed audio-only media did not expose playback controls");
    require(session.frames == 0 && media_opens == 1 && session.app.watching(),
        "Audio-only media was treated as a missing-video failure");
    record("confirmed-audio-only", session);
}
std::shared_ptr<GrowingFilePlayback> partial(const std::string& folder, std::string& expected);
void buffering_acknowledgement(const std::string& folder, const std::string& app_dir) {
    FaultScope injected(Fault::CacheHalf);
    Session session;
    session.acknowledge = false;
    std::string expected;
    auto source = partial(folder, expected);
    AppProtocolTest::open(session.app, app_dir, folder + "/cache-data", source->path, 0, source);
    // A callback-backed download exercises network buffer state without an external service.
    session.until([&] { return session.frames > 2 && AppProtocolTest::state(session.app) == Player::State::Playing; },
        5, "Cache-state fixture did not produce actual GPU pictures");
    require(session.app.launch_visible, "Loader disappeared before the first renderer acknowledgement");
    int percent = 0;
    require(AppProtocolTest::buffering(session.app, &percent) && percent == 50,
        "The fixture did not hold the actual VLC buffer state below 100 percent");
    AppProtocolTest::expire_controls(session.app);
    session.app.video_frame_presented();
    session.app.update();
    require(!session.app.launch_visible && session.app.info_visible,
        "Successful first GPU upload did not replace loader with playback controls");
    require(AppProtocolTest::controls_remaining(session.app) > 4.5,
        "The playback-control timer still includes time spent in startup");
    record("first-renderer-acknowledgement", session);
}
#endif
std::shared_ptr<GrowingFilePlayback> partial(const std::string& folder, std::string& expected) {
    const auto full = contents(folder + "/startup.mp4");
    require(!full.empty(), "Progressive fixture is empty");
    expected = full.substr(0, full.size() * 4 / 5);
    const std::string path = folder + "/resume-download.part";
    { std::ofstream file(path, std::ios::binary); file.write(expected.data(), std::streamsize(expected.size())); }
    auto source = std::make_shared<GrowingFilePlayback>();
    source->path = path;
    source->descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    source->generation = 7;
    source->total = int64_t(full.size());
    source->state = std::make_shared<GrowingFileState>();
    source->state->publish({int64_t(expected.size()), source->total, 7, GrowingFilePhase::Downloading});
    require(source->descriptor >= 0, "Progressive fixture descriptor was not opened");
    return source;
}
void missing_first_picture(const std::string& folder, const std::string& app_dir) {
    FaultScope injected(Fault::FirstMediaVideo);
    Session session;
    std::string expected;
    auto source = partial(folder, expected);
    AppProtocolTest::open(session.app, app_dir, folder + "/resume-data", source->path, 3.0, source);
    session.until([&] { return audible_frames > 30000; }, 5,
        "Fault injection did not leave real PCM audio advancing while pictures are absent");
    require(media_opens == 1 && session.frames == 0 && session.app.launch_visible && session.app.watching(),
        "The initial fault did not reproduce audible playback hidden behind startup");
#ifdef STREMIO_STARTUP_BASELINE
    session.until([&] { return audible_frames >= 48000 * 9; }, 15,
        "Baseline did not produce nine seconds of actual audio under the loader");
    require(media_opens == 1 && session.frames == 0 && session.app.launch_visible && session.app.watching(),
        "Baseline did not reproduce the reported permanent startup state");
    record("baseline-audio-without-video", session);
#else
    session.until([&] { return media_opens >= 2 && session.frames > 2 && !session.app.launch_visible; },
        20, "First-picture watchdog did not recover actual video and the App interface");
    require(media_opens == 2, "Video startup recovery reopened more than once");
    require(session.first_pts >= 2.8 && session.first_pts < 4.8,
        "Recovery followed unheard video time instead of the validated resume position");
    require(AppProtocolTest::growing(session.app) == source.get(), "Recovery replaced the shared download identity");
    const auto status = source->state->snapshot();
    require(status.phase == GrowingFilePhase::Downloading && status.generation == 7 &&
        status.available == int64_t(expected.size()) && status.total == source->total,
        "Recovery rewrote download checkpoint or ownership state");
    require(contents(source->path) == expected && ::fcntl(source->descriptor, F_GETFD) >= 0,
        "Recovery truncated download data or closed its shared file descriptor");
    record("first-picture-recovery-and-download-retention", session);
#endif
}
#ifndef STREMIO_STARTUP_BASELINE
void missing_all_pictures(const std::string& folder, const std::string& app_dir) {
    FaultScope injected(Fault::AllVideo);
    Session session;
    AppProtocolTest::open(session.app, app_dir, folder + "/failed-data", folder + "/startup.mp4");
    session.until([&] { return !session.app.watching() && !session.app.launch_visible && !session.app.toast.empty(); },
        24, "Repeated video startup failure left audio and loading UI running indefinitely");
    require(media_opens == 2 && session.frames == 0 && audible_frames >= 48000 * 8,
        "Terminal startup test did not exercise one retry with real audio and no video");
    require(session.app.toast.find("Playback failed") != std::string::npos,
        "The terminal startup failure did not reach the user-facing error path");
    const uint64_t stopped_at = audible_frames;
    SDL_Delay(100);
    require(audible_frames == stopped_at, "Failed video playback left its audio callback running");
    record("bounded-failure-after-one-retry", session);
}
#endif
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::cout.setf(std::ios::unitbuf);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) return 3;
    diagnostics_start(std::string(argv[1]) + "/diagnostics");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    g_tasks.start(2);
    int result = 0;
    try {
        Graphics graphics;
        healthy(argv[1], argv[2]);
#ifndef STREMIO_STARTUP_BASELINE
        audio_only(argv[1], argv[2]);
        buffering_acknowledgement(argv[1], argv[2]);
        rejected_gpu_frames(argv[1], argv[2]);
#endif
        missing_first_picture(argv[1], argv[2]);
#ifndef STREMIO_STARTUP_BASELINE
        missing_all_pictures(argv[1], argv[2]);
        std::cout << "PLAYER_STARTUP_TESTS_OK checks=" << checks << '\n';
#else
        std::cout << "PLAYER_STARTUP_BUG_REPRODUCED checks=" << checks << '\n';
#endif
    } catch (const std::exception& error) {
        std::cerr << "PLAYER_STARTUP_FAILURE " << diagnostics_redact(error.what()) << '\n';
        result = 1;
    }
    g_tasks.stop();
    diagnostics_stop();
    SDL_Quit();
    return result;
}
