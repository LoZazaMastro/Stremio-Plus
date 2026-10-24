// Local diagnostic: real Player/static VLC with counters at output boundaries.
#include "player.h"
#include "diagnostics.h"
#include "http.h"
#include <vlc/vlc.h>
#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_variables.h>
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <set>
#include <fstream>
#include <stdexcept>
struct picture_t;

extern "C" {
libvlc_instance_t* __real_libvlc_new(int, const char* const*);
void __real_libvlc_log_set(libvlc_instance_t*, libvlc_log_cb, void*);
void __real_libvlc_video_set_format_callbacks(libvlc_media_player_t*, libvlc_video_format_cb, libvlc_video_cleanup_cb);
int __real_sws_scale(struct SwsContext*, const uint8_t* const[], const int[], int, int, uint8_t* const[], const int[]);
void __real_vlc_ps5_copy_plane(uint8_t*, size_t, const uint8_t*, size_t, size_t, unsigned);
void __real_picture_CopyPixels(picture_t*, const picture_t*);
int __real_var_SetChecked(vlc_object_t*, const char*, int, vlc_value_t);
}
static std::atomic<unsigned> scale_calls{}, copy_calls{}, filters{}, picture_copies{};
static std::atomic<unsigned long long> copy_bytes{};
static libvlc_video_format_cb setup_callback;
static unsigned setup_in_w{}, setup_in_h{}, setup_out_w{}, setup_out_h{}, setups{};
static const char* forced_format = "";
static void (*colour_callback)(void*, const video_format_t*);
static unsigned source_w{}, source_h{}, source_x{}, source_y{}, source_vw{}, source_vh{};
static void colour_probe(void* opaque, const video_format_t* f) {
    source_w = f->i_width; source_h = f->i_height;
    source_vw = f->i_visible_width; source_vh = f->i_visible_height;
    source_x = f->i_x_offset; source_y = f->i_y_offset;
    colour_callback(opaque, f);
}
extern "C" int __wrap_var_SetChecked(vlc_object_t* o, const char* name, int type, vlc_value_t value) {
    if (!strcmp(name, "vmem-colour")) {
        colour_callback = reinterpret_cast<decltype(colour_callback)>(value.p_address);
        value.p_address = reinterpret_cast<void*>(colour_probe);
    }
    return __real_var_SetChecked(o, name, type, value);
}
static void logger(void*, int, const libvlc_log_t* context, const char* format, va_list args) {
    char text[4096]; vsnprintf(text, sizeof(text), format, args);
    const char* module = nullptr; libvlc_log_get_context(context, &module, nullptr, nullptr);
    if (strstr(text, "filter to adapt decoder")) ++filters;
    if ((module && (!strcmp(module, "swscale") || !strcmp(module, "vmem"))) || strstr(text, "converter module"))
        std::fprintf(stderr, "PROBE_VLC %s: %s\n", module ? module : "?", text);
}
extern "C" libvlc_instance_t* __wrap_libvlc_new(int argc, const char* const* argv) {
    std::vector<const char*> arguments(argv, argv + argc);
    for (auto& arg : arguments) if (!strcmp(arg, "--verbose=-1")) arg = "--verbose=2";
    return __real_libvlc_new(argc, arguments.data());
}
extern "C" void __wrap_libvlc_log_set(libvlc_instance_t* vlc, libvlc_log_cb, void*) {
    __real_libvlc_log_set(vlc, logger, nullptr);
}
static unsigned setup(void** opaque, char* chroma, unsigned* w, unsigned* h, unsigned* pitches, unsigned* lines) {
    setup_in_w = *w; setup_in_h = *h; ++setups;
    if (*forced_format) memcpy(chroma, forced_format, 4);
    unsigned count = setup_callback(opaque, chroma, w, h, pitches, lines);
    setup_out_w = *w; setup_out_h = *h;
    std::fprintf(stderr, "PROBE_SETUP %ux%u -> %ux%u %.4s pitches=%u,%u,%u lines=%u,%u,%u\n",
        setup_in_w, setup_in_h, *w, *h, chroma, pitches[0], pitches[1], pitches[2], lines[0], lines[1], lines[2]);
    return count;
}
extern "C" void __wrap_libvlc_video_set_format_callbacks(libvlc_media_player_t* p, libvlc_video_format_cb f, libvlc_video_cleanup_cb c) {
    setup_callback = f; __real_libvlc_video_set_format_callbacks(p, setup, c);
}
extern "C" int __wrap_sws_scale(struct SwsContext* c, const uint8_t* const src[], const int sp[], int y, int h, uint8_t* const dst[], const int dp[]) {
    ++scale_calls; return __real_sws_scale(c, src, sp, y, h, dst, dp);
}
extern "C" void __wrap_vlc_ps5_copy_plane(uint8_t* d, size_t dp, const uint8_t* s, size_t sp, size_t bytes, unsigned rows) {
    ++copy_calls; copy_bytes += bytes * rows; __real_vlc_ps5_copy_plane(d, dp, s, sp, bytes, rows);
}
extern "C" void __wrap_picture_CopyPixels(picture_t* d, const picture_t* s) {
    ++picture_copies; __real_picture_CopyPixels(d, s);
}
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static std::string visible_hash(const Player::VideoFrame& frame) {
    if (frame.w <= 0 || frame.h <= 0 || !frame.data()) throw std::runtime_error("missing frame data");
    const unsigned count = frame.layout == Player::VideoFrame::Layout::Nv12 ? 2 : 3;
    AVSHA* sha = av_sha_alloc();
    if (!sha || av_sha_init(sha, 256) < 0) throw std::runtime_error("SHA allocation failed");
    for (unsigned p = 0; p < count; ++p) {
        const size_t rows = p ? (frame.h + 1) / 2 : frame.h;
        const size_t width = p ? (frame.w + 1) / 2 : frame.w;
        const size_t bytes = width * (frame.wide ? 2 : 1) * (p && count == 2 ? 2 : 1);
        const size_t pitch = frame.pitches[p], offset = frame.offsets[p];
        if (pitch < bytes || offset > frame.size() || (rows - 1) * pitch + bytes > frame.size() - offset)
            throw std::runtime_error("visible plane outside retained allocation");
        for (size_t row = 0; row < rows; ++row)
            av_sha_update(sha, frame.data() + offset + row * pitch, bytes);
    }
    unsigned char sum[32]; av_sha_final(sha, sum); av_free(sha);
    char text[65]; for (unsigned i = 0; i < 32; ++i) snprintf(text + 2 * i, 3, "%02x", sum[i]);
    return text;
}
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    forced_format = argv[5];
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1); SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    diagnostics_start(std::string(argv[1]) + "/logs"); http_init(std::string(argv[2]) + "/ca-bundle.crt");
    unsigned frames = 0; bool pixels_ok = true, lease_ok = true; Player::VideoFrame frame;
    std::set<std::string> observed;
    try {
        std::ifstream input(argv[4]); std::set<std::string> clean; std::string value;
        while (input >> value) clean.insert(value);
        if (clean.size() < 30) throw std::runtime_error("missing clean reference hashes");
        Player player; Player::Options options; options.url = argv[3]; player.open(options);
        Player::VideoFrame held; std::string held_hash; double until = now() + 6;
        while (now() < until && frames < 30 && player.state() != Player::State::Failed) {
            if (player.take_frame(frame)) {
                ++frames; const auto hash = visible_hash(frame);
                pixels_ok = pixels_ok && clean.count(hash) != 0; observed.insert(hash);
                if (frames == 1) { held = frame; held_hash = hash; }
            }
            SDL_Delay(1);
        }
        lease_ok = !held_hash.empty() && visible_hash(held) == held_hash;
        player.close(); lease_ok = lease_ok && visible_hash(held) == held_hash;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PROBE_FAILURE %s\n", error.what()); pixels_ok = false;
    }
    std::printf("PROBE_RESULT {\"frames\":%u,\"setups\":%u,\"setup_input\":[%u,%u],\"setup_output\":[%u,%u],\"visible\":[%d,%d],\"wide\":%s,\"planes\":%u,\"adapt_filters\":%u,\"sws_scale_calls\":%u,\"picture_copies\":%u,\"copy_plane_calls\":%u,\"copy_plane_bytes\":%llu,\"all_visible_pixel_sha256_match\":%s,\"retained_lease_stable\":%s,\"observed_hashes\":[",
        frames, setups, setup_in_w, setup_in_h, setup_out_w, setup_out_h, frame.w, frame.h,
        frame.wide ? "true" : "false", frame.layout == Player::VideoFrame::Layout::Nv12 ? 2u : 3u,
        filters.load(), scale_calls.load(), picture_copies.load(), copy_calls.load(), copy_bytes.load(),
        pixels_ok ? "true" : "false", lease_ok ? "true" : "false");
    bool first = true; for (const auto& h : observed) { printf("%s\"%s\"", first ? "" : ",", h.c_str()); first = false; }
    printf("],\"source_coded\":[%u,%u],\"source_visible\":[%u,%u],\"source_origin\":[%u,%u]}\n",
        source_w, source_h, source_vw, source_vh, source_x, source_y);
    diagnostics_stop(); SDL_Quit(); return frames >= 20 && pixels_ok && lease_ok ? 0 : 1;
}
