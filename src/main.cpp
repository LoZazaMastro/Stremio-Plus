// Stremio Plus - Native OpenGL interface, controller input and application lifecycle.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include "artcache.h"
#include "diagnostics.h"
#include "native_memory.h"
#include "controller_ambient.h"
#include "home_ui.h"
#include "http.h"
#include "hwdec_ps5.h"
#include "tasks.h"
#include "texture_cache.h"
#include "torrent/engine.h"
#include "util.h"
#include "video_gl.h"
#include "display_refresh.h"
#include "media_remote.h"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/frame_stats.hpp"
#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/component.hpp"
#include "ui/glyphs.hpp"

#include <SDL.h>
#include <GL/glcorearb.h>
extern "C" {
#include <libavutil/log.h>
}
#ifdef PLATFORM_PS5
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "display_output.h"
#include "ps5_storage.h"
#include "ui_language.h"
extern "C" void ps5_load_modules(void);
#ifdef STREMIO_USE_LIBVLC
extern "C" void vlc_ps5_vdec_prepare(void);
extern "C" int vlc_ps5_vdec_queue_status(const char** stage, int* native_code);
extern "C" bool stremio_ps5_audio_pcm_available(void);
#endif
#else
#include "preview_fixture.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
constexpr int kWidth = 1920, kHeight = 1080;

#ifdef PLATFORM_PS5_NATIVE
void log_video_memory(const char* phase) {
    stremio_video_memory_stats memory{};
    stremio_video_memory_get_stats(&memory);
    dlog("native large-block memory: phase=%s direct_MiB=%.2f peak_MiB=%.2f mapped=%u retired=%u heap_fallbacks=%llu allocation_failures=%llu release_failures=%llu",
         phase, double(memory.direct_bytes) / (1 << 20), double(memory.direct_peak_bytes) / (1 << 20),
         memory.active_mappings, memory.retired_mappings,
         (unsigned long long)memory.heap_fallbacks, (unsigned long long)memory.allocation_failures,
         (unsigned long long)memory.release_failures);
}
#endif

void boot_step(const char* stage) {
#ifdef PLATFORM_PS5
    ps5_boot_note(stage);
#else
    (void)stage;
#endif
}

struct Options {
    std::string base, data, snapshot, scenario = "home", fixture;
    int width = 1920, height = 1080;
    bool help = false;
};

std::string find_base(const char* executable) {
#ifdef PLATFORM_PS5
    (void)executable;
    return "/app0";
#else
    std::vector<std::string> candidates;
    if (const char* base = std::getenv("STREMIO_BASE")) candidates.emplace_back(base);
    if (executable && std::strchr(executable, '/')) {
        auto dir = path_dir(executable);
        candidates.push_back(dir + "/../app");
        candidates.push_back(dir + "/app");
        candidates.push_back(dir);
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
        candidates.push_back(std::string(cwd) + "/app");
        candidates.emplace_back(cwd);
    }
    for (const auto& dir : candidates)
        if (file_exists(dir + "/hui/fonts/inter-regular.huifont")) return dir;
    return "app";
#endif
}
Options options(int argc, char** argv) {
    Options o;
    o.base = find_base(argc ? argv[0] : nullptr);
#ifdef PLATFORM_PS5
    o.data = "/data/Stremio/appdata";
#else
    const char* account_home = std::getenv("HOME");
    o.data = std::string(account_home ? account_home : ".") + "/.stremio-ps5";
#endif
    bool custom_data = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") o.help = true;
        else if (i + 1 < argc) {
            if (arg == "--base") o.base = argv[++i];
            else if (arg == "--data" || arg == "-d") { o.data = argv[++i]; custom_data = true; }
#ifndef PLATFORM_PS5
            else if (arg == "--snapshot") o.snapshot = argv[++i];
            else if (arg == "--scenario") o.scenario = argv[++i];
            else if (arg == "--fixture") o.fixture = argv[++i];
            else if (arg == "--width") o.width = std::clamp(std::atoi(argv[++i]), 640, 3840);
            else if (arg == "--height") o.height = std::clamp(std::atoi(argv[++i]), 360, 2160);
#endif
        }
    }
    if (!o.snapshot.empty() && !custom_data) o.data = o.snapshot + "/test-data";
    if (o.fixture.empty()) o.fixture = o.base + "/../tests/fixtures/showcase.json";
    return o;
}

#ifndef PLATFORM_PS5
// The OS closes this descriptor on exit. Offline screenshot tests are isolated.
bool claim_single_instance() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return true;
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(41337);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(fd, 1) == 0) return true;
    close(fd);
    return false;
}
#endif

// The GL context outlives every renderer, font, image and video texture.
struct Surface {
    int width = kWidth, height = kHeight;
    DisplayRefresh refresh;
#ifdef PLATFORM_PS5
    hui::ps5::Display display;
    ~Surface() { refresh.restore([] { glFinish(); }); }
    bool open(const Options&, int mode) {
        boot_step("querying PS5 output resolution");
        const auto selected = display_output_select(mode, hui::ps5::Display::supports_display_modes());
        if (!selected.safe_to_open) return false;
        boot_step("opening EGL display");
        if (!display.open(selected.width, selected.height)) {
            if ((selected.width == 1920 && selected.height == 1080) || !display.open(1920, 1080)) return false;
            dlog("Display: requested mode unavailable, opened 1920x1080");
        }
        width = display.width(); height = display.height();
        dlog("Display: PS5 configured=%ux%u requested=%dx%d effective=%dx%d SDR",
             selected.configured_width, selected.configured_height,
             selected.width, selected.height, width, height);
        diagnostics_set_runtime({{"display", {{"configured_width", selected.configured_width},
            {"configured_height", selected.configured_height}, {"requested_width", selected.width},
            {"requested_height", selected.height}, {"width", width}, {"height", height},
            {"automatic", selected.automatic}}}});
        boot_step("EGL display ready");
        return true;
    }
    bool swap() { return display.swap(); }
#else
    SDL_Window* window = nullptr;
    SDL_GLContext sdl_context = nullptr;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    bool headless = false;
    ~Surface() {
        if (sdl_context) SDL_GL_DeleteContext(sdl_context);
        if (window) SDL_DestroyWindow(window);
        if (egl_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
            eglTerminate(egl_display);
        }
    }
    bool open(const Options& o, int) {
        headless = !o.snapshot.empty(); width = o.width; height = o.height;
        if (headless) {
            const auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
            egl_display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                      : eglGetDisplay(EGL_DEFAULT_DISPLAY);
            EGLint major = 0, minor = 0;
            if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, &major, &minor) ||
                !eglBindAPI(EGL_OPENGL_API)) return false;
            const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
            egl_context = eglCreateContext(egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
            return egl_context != EGL_NO_CONTEXT &&
                eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context);
        }
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window = SDL_CreateWindow("Stremio Plus", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window || !(sdl_context = SDL_GL_CreateContext(window))) return false;
        SDL_GL_SetSwapInterval(1);
        SDL_GL_GetDrawableSize(window, &width, &height);
        return true;
    }
    bool swap() {
        if (window) { SDL_GL_SwapWindow(window); SDL_GL_GetDrawableSize(window, &width, &height); }
        return true;
    }
#endif
};

bool load_font(hui::gfx::Renderer& renderer, const std::string& path,
               hui::gfx::Font& font, hui::ui::FontRef& ref) {
    std::string bytes;
    if (!hui::save::read_file(path, &bytes) || !font.load(bytes)) {
        dlog("Font failed: %s", font.error().c_str());
        return false;
    }
    ref.font = &font;
    ref.texture = renderer.batch().create_font_texture(font);
    return ref.texture != 0;
}

#ifdef PLATFORM_PS5
// Storage failure is shown before an account or any cache worker is opened.
// Retrying never interprets unreadable settings as a new, signed-out account.
bool storage_error_screen(const Options& o, const Ps5StoragePaths& storage) {
    Surface surface;
    if (!surface.open(o, 0)) return false;
    hui::gfx::Renderer renderer;
    if (!renderer.init()) return false;
    hui::gfx::Font regular, semibold;
    hui::ui::Fonts fonts;
    if (!load_font(renderer, o.base + "/hui/fonts/inter-regular.huifont", regular, fonts.regular) ||
        !load_font(renderer, o.base + "/hui/fonts/inter-semibold.huifont", semibold, fonts.semibold)) return false;
    fonts.display = fonts.semibold;
    fonts.mono = fonts.pixel = fonts.hand = fonts.regular;
    const bool italian = platform_ui_language() == "it";
    const std::string detail = std::string(storage.data_error ? storage.data_error : "storage") +
        " (errno " + std::to_string(storage.data_errno) + ")";
    hui::ps5::Pad pad;
    if (!pad.open()) dlog("Controller unavailable on storage recovery screen");
    hui::InputTracker tracker;
    hui::gfx::DrawList list;
    bool first = true;
    while (true) {
        const auto now = hui::sys::monotonic_us();
        std::array<hui::PadSample, 64> samples;
        const auto count = pad.read(samples);
        const auto input = tracker.update(std::span<const hui::PadSample>(samples.data(), count), std::uint64_t(now));
        if (input.pressed & hui::action_bit(hui::Action::back)) return false;
        if (input.pressed & hui::action_bit(hui::Action::confirm)) return true;
        list.clear();
        list.gradient_rect({0, 0, kWidth, kHeight}, 0,
            hui::gfx::Color::rgb(0x181127), hui::gfx::Color::rgb(0x070910));
        const auto white = hui::gfx::Color::rgb(0xf5f3ff);
        const auto muted = hui::gfx::Color::rgb(0xc2bfd0);
        list.text(semibold, fonts.semibold.texture,
            italian ? "Cartella dati non disponibile" : "App storage is unavailable", 140, 338, 56, white);
        list.text(regular, fonts.regular.texture, "/data/Stremio/appdata", 140, 426, 32, white);
        list.text(regular, fonts.regular.texture,
            italian ? "Verifica lo spazio libero e l'accesso a /data/Stremio." :
                      "Check free space and filesystem access to /data/Stremio.", 140, 510, 30, muted);
        list.text(regular, fonts.regular.texture,
            italian ? "Poi riprova. Account e download esistenti vengono conservati." :
                      "Then retry. Your existing account and downloads are preserved.", 140, 558, 30, muted);
        list.text(regular, fonts.regular.texture, detail, 140, 644, 23, muted);
        const hui::ui::Hint hints[] = {
            {hui::ui::Button::cross, italian ? "Riprova" : "Retry"},
            {hui::ui::Button::circle, italian ? "Esci" : "Exit"}};
        hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints, 2, 140, false);
        renderer.begin(); renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(0, surface.width, surface.height);
        if (!surface.swap()) return false;
        if (first) {
            first = false;
            hui::sys::hide_splash_screen();
            boot_step("storage recovery screen presented");
        }
    }
}
#endif

struct InterfaceAudio {
    hui::audio::Mixer mixer;
    hui::audio::SoundBank bank;
    SDL_AudioDeviceID device = 0;
    int previous_gain = -1;
    bool enabled = false, suspended = false;
    ~InterfaceAudio() { if (device) SDL_CloseAudioDevice(device); }
    void open(const std::string& base, bool silent) {
        if (silent) return;
        enabled = true;
        const auto sounds = bank.load(base + "/hui/audio/sfx");
        dlog("UI sound bank: %d recordings, %d rejected", sounds.files, sounds.rejected);
        open_device();
    }
    void open_device() {
        if (!enabled || device) return;
#if defined(PLATFORM_PS5) && defined(STREMIO_USE_LIBVLC)
        // A failed native HDMI shutdown also blocks the interface sound port.
        if (!stremio_ps5_audio_pcm_available()) return;
#endif
        SDL_AudioSpec desired{};
        desired.freq = hui::audio::kSampleRate;
        desired.format = AUDIO_S16SYS; desired.channels = 2; desired.samples = 512;
        desired.userdata = &mixer;
        desired.callback = [](void* context, Uint8* stream, int size) {
            auto& mix = *static_cast<hui::audio::Mixer*>(context);
            mix.render(reinterpret_cast<std::int16_t*>(stream), size / 4);
        };
        device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
        if (device) SDL_PauseAudioDevice(device, 0);
        else dlog("UI audio unavailable: %s", SDL_GetError());
    }
    void suspend() {
        // SDL's PS5 driver permits only one output device. Pause leaves it
        // claimed, so close it before Player::open, then reopen after close.
        if (device) { SDL_CloseAudioDevice(device); device = 0; }
        mixer.stop_all();
        suspended = true;
    }
    void play(const App& app, const hui::ui::Feedback& feedback) {
        if (suspended && !app.watching()) { suspended = false; open_device(); }
        const int gain = app.ui_sounds ? std::clamp(app.ui_sound_volume, 0, 100) : 0;
        if (previous_gain != gain) { mixer.set_master_gain(float(gain) / 100.0f); previous_gain = gain; }
        if (device && gain)
            for (const auto& event : feedback.cues) bank.play(mixer, hui::audio::SoundSet::glass, event);
    }
};

#ifndef PLATFORM_PS5
std::uint32_t key_bit(SDL_Keycode key, bool typing) {
    using namespace hui::pad_bits;
    switch (key) {
        case SDLK_UP: return kUp; case SDLK_DOWN: return kDown;
        case SDLK_LEFT: return kLeft; case SDLK_RIGHT: return kRight;
        case SDLK_RETURN: case SDLK_KP_ENTER: return typing ? kOptions : kCross;
        case SDLK_ESCAPE: return kCircle;
        case SDLK_TAB: return kOptions;
        default: break;
    }
    if (typing) return 0;
    switch (key) {
        case SDLK_SPACE: return kCross; case SDLK_BACKSPACE: return kCircle;
        case SDLK_s: return kSquare; case SDLK_t: return kTriangle;
        case SDLK_o: return kOptions; case SDLK_q: return kL1; case SDLK_e: return kR1;
        case SDLK_1: return kL2; case SDLK_3: return kR2;
        default: return 0;
    }
}
std::uint32_t controller_bit(Uint8 button) {
    using namespace hui::pad_bits;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return kCross; case SDL_CONTROLLER_BUTTON_B: return kCircle;
        case SDL_CONTROLLER_BUTTON_X: return kSquare; case SDL_CONTROLLER_BUTTON_Y: return kTriangle;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return kUp; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kDown;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kLeft; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kRight;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kL1; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kR1;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return kL3; case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return kR3;
        case SDL_CONTROLLER_BUTTON_START: return kOptions; case SDL_CONTROLLER_BUTTON_TOUCHPAD: return kTouchpad;
        default: return 0;
    }
}
struct HostInput {
    SDL_GameController* controller = nullptr;
    hui::PadSample sample{};
    std::uint32_t keyboard_buttons = 0, controller_buttons = 0;
    bool text_active = false, quit = false;
    HostInput() { sample.connected = true; find_controller(); }
    ~HostInput() { if (controller) SDL_GameControllerClose(controller); }
    void find_controller() {
        if (controller && SDL_GameControllerGetAttached(controller)) return;
        if (controller) SDL_GameControllerClose(controller);
        controller = nullptr; controller_buttons = 0;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i) && (controller = SDL_GameControllerOpen(i))) break;
    }
    std::vector<hui::PadSample> poll(App& app, std::uint64_t now) {
        if (app.text_entry_active() != text_active) {
            text_active = app.text_entry_active();
            if (text_active) SDL_StartTextInput(); else SDL_StopTextInput();
            keyboard_buttons = 0;
        }
        std::vector<hui::PadSample> samples;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            bool changed = false;
            if (event.type == SDL_QUIT) quit = true;
            else if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED) find_controller();
            else if (event.type == SDL_TEXTINPUT && text_active) app.on_text(event.text.text);
            else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                if (event.type == SDL_KEYDOWN && event.key.repeat) continue;
                if (text_active && event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_BACKSPACE) {
                    std::string text = app.input_value;
                    if (!text.empty()) {
                        size_t at = text.size() - 1;
                        while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
                        text.erase(at); app.replace_input(text);
                    }
                }
                const auto bit = key_bit(event.key.keysym.sym, text_active);
                if (event.type == SDL_KEYDOWN) keyboard_buttons |= bit; else keyboard_buttons &= ~bit;
                changed = true;
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
                const auto bit = controller_bit(event.cbutton.button);
                if (event.type == SDL_CONTROLLERBUTTONDOWN) controller_buttons |= bit; else controller_buttons &= ~bit;
                changed = true;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                keyboard_buttons = controller_buttons = 0;
                auto lost = sample; lost.buttons = hui::pad_bits::kIntercepted; samples.push_back(lost);
            }
            if (changed) {
                sample.buttons = keyboard_buttons | controller_buttons;
                sample.timestamp_us = now; samples.push_back(sample);
            }
        }
        sample.buttons = keyboard_buttons | controller_buttons;
        if (controller) {
            const auto axis = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(controller, a); };
            sample.left_x = static_cast<std::uint8_t>((int(axis(SDL_CONTROLLER_AXIS_LEFTX)) + 32768) / 256);
            sample.left_y = static_cast<std::uint8_t>((int(axis(SDL_CONTROLLER_AXIS_LEFTY)) + 32768) / 256);
            sample.right_x = static_cast<std::uint8_t>((int(axis(SDL_CONTROLLER_AXIS_RIGHTX)) + 32768) / 256);
            sample.right_y = static_cast<std::uint8_t>((int(axis(SDL_CONTROLLER_AXIS_RIGHTY)) + 32768) / 256);
            sample.l2 = static_cast<std::uint8_t>(std::max(0, int(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT))) / 128);
            sample.r2 = static_cast<std::uint8_t>(std::max(0, int(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT))) / 128);
        } else {
            sample.left_x = sample.left_y = sample.right_x = sample.right_y = 128;
            sample.l2 = sample.r2 = 0;
        }
        sample.timestamp_us = now; samples.push_back(sample);
        return samples;
    }
};
#endif

std::string gl_string(GLenum name) {
    const auto* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "unavailable";
}

int run_interface(App& app, const Options& o) {
    Surface surface;
    if (!surface.open(o, app.settings().display_resolution)) {
        dlog("Display/context initialization failed: %s", SDL_GetError());
        return 1;
    }
#ifndef PLATFORM_PS5
    hui::gfx::set_glsl_prefix("#version 450 core\n");
#endif
    dlog("OpenGL %s / %s; output %dx%d SDR", gl_string(GL_VERSION).c_str(),
        gl_string(GL_RENDERER).c_str(), surface.width, surface.height);
    hui::gfx::Renderer renderer;
    VideoGl video;
    boot_step("initializing UI and video renderers");
    if (!renderer.init() || !video.init()) { dlog("Renderer initialization failed"); return 1; }
    hui::gfx::Font regular, semibold, mono, subtitles, subtitles_serif, subtitles_mono;
    hui::ui::Fonts fonts;
    boot_step("loading UI font textures");
    if (!load_font(renderer, o.base + "/hui/fonts/inter-regular.huifont", regular, fonts.regular) ||
        !load_font(renderer, o.base + "/hui/fonts/inter-semibold.huifont", semibold, fonts.semibold) ||
        !load_font(renderer, o.base + "/hui/fonts/dejavu-sans-mono.huifont", mono, fonts.mono)) return 1;
    boot_step("UI font textures ready");
    // The extended Inter atlas includes accented titles and European scripts.
    fonts.display = fonts.semibold; fonts.pixel = fonts.mono; fonts.hand = fonts.regular;
    TextureCache textures(renderer.batch(), o.base);
    HomeUi ui([&](const std::string& path) { return textures.get(path); });
    ui.set_texture_size_lookup([&](const std::string& path) { return textures.dimensions(path); });
    hui::ui::FontRef subtitle_font, subtitle_serif, subtitle_mono;
    if (load_font(renderer, o.base + "/hui/fonts/subtitles.huifont", subtitles, subtitle_font)) {
        if (!load_font(renderer, o.base + "/hui/fonts/subtitles-serif.huifont", subtitles_serif, subtitle_serif))
            subtitle_serif = subtitle_font;
        if (!load_font(renderer, o.base + "/hui/fonts/subtitles-mono.huifont", subtitles_mono, subtitle_mono))
            subtitle_mono = subtitle_font;
        ui.set_subtitle_fonts(subtitle_font, subtitle_serif, subtitle_mono);
    }
    InterfaceAudio audio;
    audio.open(o.base, !o.snapshot.empty());
    struct PlaybackHook {
        App& app;
        ~PlaybackHook() { app.before_playback = {}; }
    } playback_hook{app};
    app.before_playback = [&audio, &textures] {
        audio.suspend();
        // Release the previous gallery before the decoder and video textures
        // compete for native memory. Playback artwork remains available.
        textures.set_playback(true);
    };
    hui::InputTracker tracker;
#ifdef PLATFORM_PS5
    hui::ps5::Pad pad;
    if (!pad.open()) dlog("Controller unavailable at startup");
    MediaRemote remote;
    ControllerAmbientLight ambient;
#else
    HostInput input_source;
#endif
    json runtime = {{"gl_version", gl_string(GL_VERSION)}, {"gl_vendor", gl_string(GL_VENDOR)},
        {"gl_renderer", gl_string(GL_RENDERER)}, {"output_width", surface.width}, {"output_height", surface.height},
        {"ui_width", kWidth}, {"ui_height", kHeight}, {"gpu_yuv", true}, {"hdr_mode", "SDR output; HDR sources tone mapped"},
        {"addon_count", app.addon_rows.size()}, {"server_configured", !app.settings().server_url.empty()}};
    diagnostics_set_runtime(runtime);
    Player::VideoFrame frame;
    hui::gfx::DrawList base_list, overlay_list;
    float elapsed = 0;
    const auto compose = [&](GLuint target, bool fixture) {
        base_list.clear(); overlay_list.clear();
        base_list.rounded_rect({0, 0, kWidth, kHeight}, 0, hui::gfx::Color::rgb(0));
        if (app.watching()) {
            const auto texture = video.texture();
            if (texture) {
                float width = kWidth, height = width / float(video.aspect());
                if (height > kHeight) { height = kHeight; width = height * float(video.aspect()); }
                // Conversion renders source row 0 into FBO row 0. Keep this
                // UV direction so source row 0 is the top of the TV picture.
                base_list.image(texture, {(kWidth-width)/2, (kHeight-height)/2, width, height},
                    hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff));
            } else if (fixture && !app.launch_image.empty()) {
                if (const auto art = textures.get(app.launch_image))
                    base_list.image(art, {0, 0, kWidth, kHeight}, hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff));
            }
        }
        hui::ui::Canvas base{base_list, fonts, 0, elapsed};
        hui::ui::Canvas overlay{overlay_list, fonts, renderer.glass_texture(), elapsed};
        ui.draw_base(app, base); ui.draw_overlays(app, overlay);
        renderer.begin(); renderer.draw(base_list);
        if (ui.wants_glass()) renderer.glass();
        renderer.draw(overlay_list);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(target, surface.width, surface.height);
    };

#ifndef PLATFORM_PS5
    if (!o.snapshot.empty()) {
        make_dirs(o.snapshot);
        hui::gfx::Canvas target;
        if (!target.create(surface.width, surface.height, 1)) return 1;
        std::vector<std::string> scenarios = {o.scenario};
        if (o.scenario == "all") scenarios = {"home", "home_page2", "home_last", "nav", "search",
            "discover", "library", "detail", "streams", "addons", "settings", "dropdown", "login",
            "login_error", "login_expired", "keyboard", "player", "tracks", "subtitles", "launch",
            "dialog", "source_info", "subtitle_appearance", "audio_languages", "movie_streams", "episodes",
            "date_options", "launch_direct", "episodes_last", "downloads", "downloads_empty", "downloads_last", "buffering"};
        bool ok = true;
        for (const auto& scenario : scenarios) {
            if (!load_preview_fixture(app, scenario == "source_info" ? "streams" : scenario, o.fixture)) { dlog("Unknown or invalid fixture: %s", scenario.c_str()); ok = false; continue; }
            ui.snap(app);
            if (scenario == "source_info") {
                hui::InputFrame input{};
                input.pressed = hui::action_bit(hui::Action::north);
                hui::ui::Feedback feedback{};
                ui.handle(app, input, feedback);
                ui.snap(app);
            }
            // Artwork has its own bounded workers; give them frames to finish.
            // These are deterministic UI models, never evidence of playback.
            for (int warm = 0; warm < 240; ++warm) {
                textures.begin_frame(); ui.update(app, 1.0f/60.0f); elapsed += 1.0f/60.0f;
                compose(target.framebuffer(), true);
                if (warm >= 30 && !textures.pending()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
            std::vector<unsigned char> pixels(size_t(surface.width) * surface.height * 4);
            glReadPixels(0, 0, surface.width, surface.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
            stbi_flip_vertically_on_write(1);
            const auto path = o.snapshot + "/" + scenario + ".png";
            const bool saved = stbi_write_png(path.c_str(), surface.width, surface.height, 4, pixels.data(), surface.width*4) != 0;
            const auto error = glGetError();
            dlog("UI screenshot %s: saved=%d instances=%zu draws=%zu GL=0x%x", scenario.c_str(), int(saved),
                renderer.last_instances(), renderer.last_draw_calls(), error);
            ok = ok && saved && error == GL_NO_ERROR;
        }
        // Leave the real player closed, irrespective of the final fixture flag.
        app.watching_ = false;
        return ok ? 0 : 1;
    }
#endif

    std::int64_t previous = hui::sys::monotonic_us(), report_start = previous;
    std::uint64_t frames = 0, report_frames = 0;
    hui::FrameStats loop_times, upload_times, swap_times, video_swap_times;
    hui::FrameStats allocation_times, plane_times, conversion_times;
    std::int64_t previous_video_swap = 0;
    bool quit = false, first = true;
#ifdef PLATFORM_PS5
    bool first_video_heap_snapshot = true;
#endif
    int last_video_w = 0, last_video_h = 0;
    bool last_video_accepted = false;
    while (!quit && !app.wants_exit()) {
        const auto now = hui::sys::monotonic_us();
        // Keep the real interval before animation dt is clamped. The latter
        // deliberately hides long frames and cannot diagnose playback hitches.
        loop_times.add(double(now - previous) / 1000.0);
        const float dt = std::clamp(float(now - previous)/1000000.0f, 0.0f, 0.05f);
        previous = now; elapsed += dt;
        hui::InputFrame input;
#ifdef PLATFORM_PS5
        std::array<hui::PadSample, 64> samples;
        const auto count = pad.read(samples);
        input = tracker.update(std::span<const hui::PadSample>(samples.data(), count), std::uint64_t(now));
        const auto remote_frame = remote.poll(std::uint64_t(now));
        merge_remote_input(input, remote_frame.input);
        for (std::size_t i = 0; i < remote_frame.count; ++i) app.on_media_command(remote_frame.commands[i]);
#else
        const auto samples = input_source.poll(app, std::uint64_t(now));
        input = tracker.update(samples, std::uint64_t(now));
        quit = input_source.quit;
#endif
        hui::ui::Feedback feedback;
        ui.handle(app, input, feedback);
        app.update();
        if (surface.refresh.update(app.settings().display_refresh, app.watching(), [] { glFinish(); })) {
            runtime["display_refresh"] = {{"preference", app.settings().display_refresh},
                {"available", surface.refresh.available()}, {"active", surface.refresh.active()},
                {"refresh_code", surface.refresh.refresh_code()}, {"status", surface.refresh.status()}};
            diagnostics_set_runtime(runtime);
        }
        if (!surface.refresh.safe()) {
            diagnostics_note("display", "Output mode could not be restored safely");
            break;
        }
        audio.play(app, feedback);
#ifdef PLATFORM_PS5
        const auto [title_key, artwork_path] = app.ambient_artwork();
        ambient.select(title_key, artwork_path);
        ambient.update(dt, app.settings().controller_ambient_light,
            [&pad](std::uint8_t r, std::uint8_t g, std::uint8_t b) { return pad.set_light_bar(r, g, b); });
        if (feedback.rumble_strength > 0) pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
#endif
        bool video_uploaded = false;
        if (app.take_video_frame(frame)) {
            const auto upload_started = hui::sys::monotonic_us();
            const bool accepted = video.upload(frame);
            upload_times.add(double(hui::sys::monotonic_us() - upload_started) / 1000.0);
            const auto& upload_timing = video.last_upload_timing();
            if (upload_timing.reconfigured) allocation_times.add(upload_timing.allocation_ms);
            if (accepted) {
                plane_times.add(upload_timing.plane_ms);
                conversion_times.add(upload_timing.draw_ms);
            }
            if (accepted) {
                app.video_frame_presented();
                video_uploaded = true;
            }
            else {
                diagnostics_note("video", "GPU frame rejected");
#ifdef PLATFORM_PS5_NATIVE
                log_video_memory("frame-rejected");
#endif
                app.video_frame_rejected();
            }
            if (frame.w != last_video_w || frame.h != last_video_h ||
                accepted != last_video_accepted || !accepted) {
                last_video_w = frame.w; last_video_h = frame.h;
                last_video_accepted = accepted;
                runtime["width"] = frame.w; runtime["height"] = frame.h;
                runtime["bit_depth"] = frame.wide ? 10 : 8;
                runtime["pixel_format"] = frame.layout == Player::VideoFrame::Layout::Bgra ? "BGRA" :
                    frame.layout == Player::VideoFrame::Layout::Nv12 ? "NV12/P010" : "YUV420P";
                runtime["color_range"] = frame.full_range ? "full" : "limited";
                runtime["color_transfer"] = frame.hlg ? "HLG" : frame.colors == YuvColors::Hdr10 ? "PQ" : "SDR";
                runtime["source_fps"] = app.source_video_fps();
                runtime["dolby_vision_reshape"] = video.dolby_vision_active();
                runtime["audio_output"] = app.audio_output_status();
                runtime["video_frame_accepted"] = accepted;
                diagnostics_set_runtime(runtime);
            }
        }
        if (!app.watching()) {
            video.clear();
            frame = {};
            last_video_w = last_video_h = 0;
            last_video_accepted = false;
            textures.set_playback(false);
        }
        textures.begin_frame();
        // Warm only the initial visible artwork while the loading page owns
        // the screen. Network completion alone does not mean GPU textures are
        // ready. Off-screen work never holds this bounded first reveal open.
        bool startup_textures_ready = true;
        if (app.home_startup_visible()) {
            for (const auto& path : app.home_startup_artwork())
                if (!textures.get(path)) startup_textures_ready = false;
        }
        app.update_home_startup(startup_textures_ready);
        ui.update(app, dt);
        compose(0, false);
        const auto swap_started = hui::sys::monotonic_us();
        const bool swapped = surface.swap();
        const auto swap_finished = hui::sys::monotonic_us();
        swap_times.add(double(swap_finished - swap_started) / 1000.0);
        if (!app.watching() || app.w_paused || app.w_buffering || !swapped) previous_video_swap = 0;
        else if (video_uploaded) {
            if (previous_video_swap > 0)
                video_swap_times.add(double(swap_finished - previous_video_swap) / 1000.0);
            previous_video_swap = swap_finished;
        }
        if (!swapped) { diagnostics_note("display", "Present failed"); quit = true; }
#ifdef PLATFORM_PS5
        if (swapped && video_uploaded && first_video_heap_snapshot) {
            first_video_heap_snapshot = false;
            stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
#ifdef PLATFORM_PS5_NATIVE
            log_video_memory("first-picture");
#endif
        }
#endif
        if (first) {
            first = false;
            hui::sys::hide_splash_screen();
            diagnostics_note("lifecycle", "First frame presented");
            boot_step("first frame presented");
        }
        ++frames; ++report_frames;
        if (now - report_start >= 30000000) {
            const double fps = double(report_frames)*1000000.0/double(now-report_start);
            runtime["fps"] = fps; runtime["addon_count"] = app.addon_rows.size();
            runtime["output_width"] = surface.width; runtime["output_height"] = surface.height;
            runtime["source_fps"] = app.source_video_fps();
            runtime["video_processing"] = {{"upscaler", "bilinear"}, {"smoothing", "off"},
                {"dolby_vision_reshape", video.dolby_vision_active()},
                {"source_fps", app.source_video_fps()}, {"accepted_frame_fps", video.fps()},
                {"upload_path", VideoGl::upload_path()},
                {"audio_passthrough_requested", app.settings().audio_passthrough},
                {"audio_output", app.audio_output_status()}};
            const auto timing_json = [](const hui::FrameStats& value) {
                return json{{"samples", value.count()}, {"mean_ms", value.mean_ms()},
                    {"max_ms", value.max_ms()}, {"histogram", value.buckets()}};
            };
            runtime["presentation_timing"] = {
                {"scope", "CPU monotonic timings; conversion_submit is GL submission, not GPU completion or HDMI scanout"},
                {"histogram_upper_ms", json::array({8.4, 17.5, 25.0, 34.0, 50.0, 100.0})},
                {"histogram_last_bucket", "100 ms and above"},
                {"loop", timing_json(loop_times)}, {"upload", timing_json(upload_times)},
                {"allocation", timing_json(allocation_times)}, {"plane_transfer", timing_json(plane_times)},
                {"conversion_submit", timing_json(conversion_times)},
                {"swap_wait", timing_json(swap_times)}, {"accepted_video_swap", timing_json(video_swap_times)}};
            diagnostics_set_runtime(runtime);
            dlog("UI frame pacing: %.1f FPS over 30s, %zu draw calls, video=%d", fps, renderer.last_draw_calls(), int(app.watching()));
            char loop_text[256], upload_text[256], swap_text[256], video_swap_text[256];
            loop_times.format(loop_text, sizeof(loop_text));
            upload_times.format(upload_text, sizeof(upload_text));
            swap_times.format(swap_text, sizeof(swap_text));
            video_swap_times.format(video_swap_text, sizeof(video_swap_text));
            dlog("presentation timing: loop {%s} upload {%s} swap_wait {%s} video_swap_return {%s} refresh_code=%llu",
                 loop_text, upload_text, swap_text, video_swap_text,
                 static_cast<unsigned long long>(surface.refresh.refresh_code()));
            char allocation_text[256], plane_text[256], conversion_text[256];
            allocation_times.format(allocation_text, sizeof(allocation_text));
            plane_times.format(plane_text, sizeof(plane_text));
            conversion_times.format(conversion_text, sizeof(conversion_text));
            dlog("video upload timing: path=%s allocation {%s} plane_transfer {%s} conversion_submit {%s} source_fps=%.6f",
                 VideoGl::upload_path(), allocation_text, plane_text, conversion_text, app.source_video_fps());
            loop_times.reset(); upload_times.reset(); swap_times.reset(); video_swap_times.reset();
            allocation_times.reset(); plane_times.reset(); conversion_times.reset();
#ifdef PLATFORM_PS5_NATIVE
            if (app.watching()) stremio_heap_snapshot(STREMIO_HEAP_PLAYBACK_PERIODIC);
            log_video_memory(app.watching() ? "playing" : "idle");
#endif
            report_frames = 0; report_start = now;
        }
    }
    dlog("Interface closed after %llu frames", static_cast<unsigned long long>(frames));
    return 0;
}
} // namespace

int main(int argc, char** argv) {
#ifdef PLATFORM_PS5
    // VideoDec2 rejects module/compute-queue creation after the filesystem
    // helper changes the process credentials. Prepare both under the title's
    // original credentials, as required by the VLC PS5 decoder.
    HwDecoder::load_module();
#ifdef STREMIO_USE_LIBVLC
    if (HwDecoder::available()) vlc_ps5_vdec_prepare();
#endif
    // Resolve application/data paths before starting application workers.
    auto native_storage = ps5_prepare_storage();
#endif
    boot_step("entered application main");
    std::signal(SIGPIPE, SIG_IGN);
    auto o = options(argc, argv);
#ifdef PLATFORM_PS5
    o.base = native_storage.app;
    o.data = native_storage.data;
#endif
    if (o.help) {
        std::puts("Stremio Plus for PS5\n  --base APP_DIR --data DATA_DIR\nHost only:\n  --snapshot OUTPUT_DIR --scenario home|detail|streams|discover|library|downloads|addons|settings|dropdown|login|keyboard|player|source_info|all\n  --fixture JSON --width 1920 --height 1080");
        return 0;
    }
#ifndef PLATFORM_PS5
    if (o.snapshot.empty() && !claim_single_instance()) return 0;
    if (!o.snapshot.empty()) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
#ifndef PLATFORM_PS5
    make_dirs(o.data);
#endif
#ifdef PLATFORM_PS5
    const std::string log_directory = native_storage.logs;
#else
    const std::string log_directory = o.data + "/logs";
#endif
    boot_step("starting asynchronous diagnostics");
#ifdef PLATFORM_PS5
    // Do not let the generic recursive mkdir create a sandbox-local /data
    // when the console's real filesystem was not made available.
    bool diagnostics_ready = native_storage.logs_available && diagnostics_start(log_directory);
#else
    const bool diagnostics_ready = diagnostics_start(log_directory);
#endif
    if (!diagnostics_ready) std::fprintf(stderr, "Stremio Plus: diagnostics folder unavailable\n");
    boot_step(diagnostics_ready ? "asynchronous diagnostics ready" : "asynchronous diagnostics unavailable");
    dlog("Stremio Plus %s (%s) starting", STREMIO_VERSION, STREMIO_TITLE_ID);
    av_log_set_callback(diagnostics_ffmpeg_log);
    SDL_LogSetOutputFunction([](void*, int, SDL_LogPriority, const char* message) {
        diagnostics_note("sdl", message ? message : "");
    }, nullptr);
#ifdef PLATFORM_PS5
    dlog("hwdec: module prepared before filesystem setup: %s",
         HwDecoder::available() ? "loaded" : "unavailable");
#ifdef STREMIO_USE_LIBVLC
    const char* queue_stage = nullptr;
    int queue_code = 0;
    const int queue_status = vlc_ps5_vdec_queue_status(&queue_stage, &queue_code);
    dlog("hwdec: pre-storage compute queue status=%d stage=%s rc=0x%08x",
         queue_status, queue_stage ? queue_stage : "unknown", unsigned(queue_code));
#endif
    boot_step("loading system modules");
    ps5_load_modules();
    boot_step("system modules ready");
#endif
    int result = 1;
    boot_step("initializing SDL audio and events");
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        dlog("SDL initialization failed: %s", SDL_GetError());
    } else {
#ifdef PLATFORM_PS5
        while (!native_storage.data_available) {
            dlog("App storage unavailable: stage=%s errno=%d", native_storage.data_error, native_storage.data_errno);
            if (!storage_error_screen(o, native_storage)) break;
            native_storage = ps5_prepare_storage();
            o.base = native_storage.app;
            o.data = native_storage.data;
            if (!diagnostics_ready && native_storage.logs_available)
                diagnostics_ready = diagnostics_start(native_storage.logs);
        }
        if (native_storage.data_available) {
#endif
        boot_step("initializing HTTP and application workers");
        http_init(o.base + "/ca-bundle.crt");
        g_tasks.start(4);
        g_art.start(o.data + "/art", 4);
        App app;
        bool initialized = false;
        try {
            initialized = app.init(o.base, o.data, !o.snapshot.empty());
            if (initialized) {
#ifdef PLATFORM_PS5
                stremio_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED);
                if (!diagnostics_ready) app.notify(app.ui_language == "it"
                    ? "Log non disponibili in /data/Stremio. Verifica elfldr e accesso al filesystem."
                    : "Logs unavailable in /data/Stremio. Check elfldr and filesystem access.", 12);
#endif
                boot_step("application initialized; preparing interface");
                result = run_interface(app, o);
            }
        } catch (const std::exception& error) {
            dlog("Fatal application exception: %s", error.what());
        } catch (...) {
            dlog("Fatal application exception of unknown type");
        }
        if (initialized) app.shutdown();
        g_art.stop();
        g_tasks.stop();
        bt::Engine::get().shutdown();
#ifdef PLATFORM_PS5
        }
#endif
        SDL_Quit();
    }
    diagnostics_note("lifecycle", result ? "Application stopped with an error" : "Clean application shutdown");
    diagnostics_stop();
#ifdef PLATFORM_PS5
    ps5_boot_note(result ? "application stopped with error" : "clean application shutdown");
    ps5_boot_close();
    hui::sys::quit();
#else
    return result;
#endif
}
