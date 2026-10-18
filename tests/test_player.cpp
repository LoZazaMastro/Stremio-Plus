// Exercise the production VLC or reference FFmpeg Player with synthetic media.
#include "diagnostics.h"
#include "http.h"
#include "player.h"
#include "util.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <execinfo.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <signal.h>
#include <stdexcept>
#include <unistd.h>

#ifdef STREMIO_USE_LIBVLC
extern "C" void vlc_ps5_vdec_failed();
#endif

namespace {
int checks = 0;
struct sigaction diagnostic_abort_action{};
// Host-test-only stack trace, chained to the application's minimal receipt.
// This is deliberately not installed by the console application.
void trace_abort(int number, siginfo_t* info, void* context) {
    static const char heading[] = "PLAYER_TEST_SIGABRT_BACKTRACE\n";
    (void)write(STDERR_FILENO, heading, sizeof(heading) - 1);
    void* frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    diagnostic_abort_action.sa_sigaction(number, info, context);
}
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Pump {
    Player& player;
    Player::VideoFrame frame;
    int frames = 0;
    bool forbid_network_buffering = false;
    void step() {
        if (player.take_frame(frame)) ++frames;
        if (forbid_network_buffering && player.buffering())
            throw std::runtime_error("Complete local playback exposed network buffering");
        if (player.state() == Player::State::Failed)
            throw std::runtime_error("Player failed: " + diagnostics_redact(player.error()));
    }
    void until(const std::function<bool()>& condition, double timeout, const char* message) {
        const double end = seconds() + timeout;
        do {
            step();
            if (condition()) { require(true, message); return; }
            SDL_Delay(4);
        } while (seconds() < end);
        std::cerr << "state=" << int(player.state()) << " position=" << player.position()
                  << " frames=" << frames << " last_pts=" << frame.pts << '\n';
        require(false, message);
    }
    void for_time(double duration) {
        const double end = seconds() + duration;
        while (seconds() < end) { step(); SDL_Delay(4); }
    }
};
SDL_AudioDeviceID audio_probe() {
    SDL_AudioSpec want{};
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    return SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
}
void audio_exclusivity() {
    const auto sfx = audio_probe();
    require(sfx != 0, "Dummy audio did not open its first output");
    const auto concurrent = audio_probe();
    if (concurrent) SDL_CloseAudioDevice(concurrent);
    require(concurrent == 0, "The single-output test backend unexpectedly allowed two outputs");
    require(std::string(SDL_GetError()).find("already open") != std::string::npos,
            "SDL did not expose its single-output contract");
    SDL_CloseAudioDevice(sfx);
    const auto handed_off = audio_probe();
    require(handed_off != 0, "Closing the UI device did not release the audio output");
    SDL_CloseAudioDevice(handed_off);
    std::cout << "PASS SDL single-output contract and close-before-open handoff\n";
}

void local_av(const std::string& folder) {
    Player player;
    Player::Options options;
    options.url = folder + "/multi.mkv";
    options.audio_langs = {"ita", "eng"};
    player.open(options);
    Pump pump{player};
    pump.forbid_network_buffering = true;
    require(!player.buffering() && !player.progressive_download(), "Local opening was classified as a network download");
    pump.until([&] { return player.state() == Player::State::Playing && pump.frames > 1 && player.position() >= .3; },
               6, "H264/AAC did not start with an advancing clock");
    require(pump.frame.w == 640 && pump.frame.h == 360, "H264 dimensions changed");
    require(!pump.frame.wide && pump.frame.layout == Player::VideoFrame::Layout::Planar420,
            "H264 did not retain its source YUV420 planes");
    require(pump.frame.colors == YuvColors::Bt709, "H264 matrix metadata was lost");
    require(player.duration() > 5.8 && player.duration() < 6.2, "Container duration was not detected");
    require(player.seekable_until() == player.duration(), "Complete local input did not expose its full timeline");
    const auto tracks = player.audio_tracks();
    require(tracks.size() == 2, "The two audio tracks were not exposed");
    int italian = -1, english = -1;
    for (const auto& track : tracks) {
        if (track.lang == "ita") italian = track.stream;
        if (track.lang == "eng") english = track.stream;
    }
    require(italian >= 0 && english >= 0, "Track language normalization failed");
    require(player.audio_stream() == italian, "Preferred Italian audio was not selected");
    const auto occupied = audio_probe();
    if (occupied) SDL_CloseAudioDevice(occupied);
    require(occupied == 0, "Player did not acquire the audio output");
    const auto subs = player.subtitle_tracks();
    require(subs.size() == 2, "Embedded SRT and ASS tracks were not exposed");
    int srt = -1, ass = -1;
    for (const auto& track : subs) {
        if (track.lang == "ita") srt = track.stream;
        if (track.lang == "eng") ass = track.stream;
    }
    require(srt >= 0 && ass >= 0, "Subtitle languages were not preserved");
    player.select_subtitle(srt);
#ifdef STREMIO_USE_LIBVLC
    // Request the earlier cue explicitly. Selecting a track must not rewind playback.
    player.seek(0);
#endif
    pump.until([&] { return player.embedded_subtitle(srt, 1.0).find("Primo") != std::string::npos; },
               2, "Embedded SRT was not decoded");
#ifdef STREMIO_USE_LIBVLC
    player.select_subtitle(ass);
    player.seek(.4);
    pump.until([&] { return player.embedded_subtitle(ass, 1.5).find("VLC ASS subtitle") != std::string::npos; },
               3, "Embedded ASS was not delivered to the custom subtitle renderer");
    require(player.embedded_subtitle(srt, 1.0).empty(), "Old subtitle selection survived its track change");
    player.select_subtitle(-1);
    pump.for_time(.1);
    require(player.embedded_subtitle(ass, 1.5).empty(), "Subtitles Off retained decoded text");
    player.select_subtitle(srt);
#endif

    player.set_paused(true);
    pump.for_time(.05);
    const double paused_at = player.position();
    pump.for_time(.22);
    require(player.paused() && std::abs(player.position() - paused_at) < .075, "Pause did not stop the playback clock");
    player.select_audio(english);
    pump.until([&] { return player.audio_stream() == english; }, 2, "Audio-track switching failed");
    const int before_seek = pump.frames;
    player.seek(2.0);
    require(!player.buffering(), "Seeking a completed file exposed a buffering label");
    player.set_paused(false);
    pump.until([&] { return pump.frames > before_seek && pump.frame.pts >= 1.9 && player.position() >= 2.0; },
               4, "Seek/resume did not reach the requested media time");
    require(!player.paused(), "Resume left the player paused");
    pump.until([&] { return player.state() == Player::State::Ended; }, 7, "Playback did not drain to EOF");
    require(player.position() > 5.5 && player.duration() > 5.8, "EOF discarded the final playback bookmark");
    const auto retained_frame = pump.frame;
    const std::vector<uint8_t> retained_bytes(retained_frame.data(), retained_frame.data() + 64);
    const int before_restart = pump.frames;
    player.seek(1.0);
    require(!player.buffering(), "Restarting a completed file after EOF exposed a buffering label");
    pump.until([&] { return player.state() == Player::State::Playing && pump.frames > before_restart &&
                           pump.frame.pts >= .9 && pump.frame.pts < 2.0; },
               4, "Seeking back after EOF did not restart decoding");
    const double closing = seconds();
    player.close();
    require(player.state() == Player::State::Idle && seconds() - closing < 3.0,
            "Closing playback did not stop its workers promptly");
    require(std::equal(retained_bytes.begin(), retained_bytes.end(), retained_frame.data()),
            "Reopening or closing playback overwrote a frame still leased by the renderer");
    const auto released = audio_probe();
    require(released != 0, "Player close did not release the audio output for UI sounds");
    SDL_CloseAudioDevice(released);
    std::cout << "PASS H264, Italian/English tracks, embedded text subtitles, pause, seek, EOF restart, retained frames and audio release; no network buffering for local files\n";
}

void main10(const std::string& folder) {
    Player player;
    Player::Options options;
    options.url = folder + "/main10.mkv";
    player.open(options);
    Pump pump{player};
    pump.forbid_network_buffering = true;
    pump.until([&] { return pump.frames >= 2; }, 8, "Main10 software decode did not produce frames");
    require(pump.frame.w == 3840 && pump.frame.h == 2160, "Main10 was downscaled instead of preserving 4K");
    require(pump.frame.wide && pump.frame.layout == Player::VideoFrame::Layout::Planar420,
            "Main10 did not retain its wide YUV source planes");
    require(pump.frame.shift == 0 && pump.frame.colors == YuvColors::Hdr10 && !pump.frame.full_range && !pump.frame.hlg,
            "Main10 alignment, PQ or limited-range metadata was lost");
    require(pump.frame.size() == std::size_t(3840) * 2160 * 3, "Packed Main10 plane size is incorrect");
    require(pump.frame.offsets[1] == std::size_t(3840) * 2160 * 2 &&
            pump.frame.offsets[2] == pump.frame.offsets[1] + std::size_t(1920) * 1080 * 2,
            "Main10 chroma plane offsets are incorrect");
    require(player.codec_summary().find("software") != std::string::npos,
            "Host test unexpectedly bypassed the software decoder");
    pump.until([&] { return player.state() == Player::State::Ended; }, 6, "Main10 EOF was not reached");
    player.close();
    std::cout << "PASS 4K HEVC Main10 software decode, source resolution, plane packing and PQ metadata\n";
}

double prefix_video_time(const std::string& path, int64_t prefix) {
    AVFormatContext* format = nullptr;
    require(avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0 &&
            avformat_find_stream_info(format, nullptr) >= 0, "Prefix oracle could not open the complete fixture");
    const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(video >= 0, "Prefix oracle did not find a video track");
    const double scale = av_q2d(format->streams[video]->time_base);
    AVPacket* packet = av_packet_alloc();
    double last = 0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video && packet->pts >= 0 && packet->pos >= 0 &&
            packet->pos <= prefix && packet->size <= prefix - packet->pos)
            last = std::max(last, double(packet->pts) * scale);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    return last;
}

void growing_download(const std::string& folder) {
    std::ifstream original(folder + "/multi.mkv", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(original), {}};
    const auto prefix = bytes.size() * 3 / 5;
    const double local_end = prefix_video_time(folder + "/multi.mkv", int64_t(prefix));
    const std::string path = folder + "/live-download.part";
    { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), std::streamsize(prefix)); }
    auto source = std::make_shared<GrowingFilePlayback>();
    source->path = path; source->descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    source->state = std::make_shared<GrowingFileState>(); source->generation = 4; source->total = int64_t(bytes.size());
    source->state->publish({int64_t(prefix), source->total, 4, GrowingFilePhase::Downloading});
    require(source->descriptor >= 0, "Progressive fixture did not open its local prefix");
    Player player;
    Player::Options options; options.url = path; options.growing_download = source;
    player.open(options);
    Pump pump{player};
    pump.until([&] { return pump.frames > 1 && player.position() >= .3; }, 6,
               "Real Player did not render partial download before it completed");
    require(player.progressive_download() && player.seekable_until() > 0 && player.seekable_until() < player.duration(),
            "Partial file did not publish a conservative demuxed seek limit");
    require(source->state->snapshot().phase == GrowingFilePhase::Downloading &&
            std::filesystem::file_size(path) == prefix, "Starting Player unexpectedly completed or rewrote the download");
    int previous = pump.frames;
    const double before_future = player.position();
    player.seek(5.3);
    require(std::abs(player.position() - before_future) < .1, "Rejected future seek changed the displayed clock");
    pump.until([&] { return pump.frames > previous && player.position() < 4.5; }, 4,
               "Future seek disturbed demux state instead of leaving the current partial video playable");
    previous = pump.frames;
    player.seek(.1);
    pump.until([&] { return pump.frames > previous && pump.frame.pts >= .08 && pump.frame.pts < 1.3; }, 4,
               "A seek inside the downloaded portion did not replay its local frames");
    pump.until([&] { return player.buffering() && player.position() > 1.0; }, 7,
               "Playback did not wait when it caught the background download");
    require(player.state() == Player::State::Playing, "Temporary prefix EOF incorrectly ended playback");
    require(player.position() <= local_end + .05 && player.seekable_until() <= local_end + .05,
            "Input starvation advanced the bookmark or seek proof beyond actual local video packets");
    const double starved_position = player.position();
    pump.for_time(.35);
    require(std::abs(player.position() - starved_position) < .01,
            "Repeated last-picture displays advanced the playback clock during starvation");
    previous = pump.frames;
    player.seek(.1);
    pump.until([&] { return pump.frames > previous && player.position() >= .08 && player.position() < 1.0; }, 3,
               "Rewinding a starved download stayed blocked in the old prefix read");
    { std::ofstream output(path, std::ios::binary | std::ios::app);
      output.write(bytes.data() + prefix, std::streamsize(bytes.size() - prefix)); }
    std::filesystem::rename(path, folder + "/live-download.mkv");
    source->state->publish({source->total, source->total, 4, GrowingFilePhase::Complete});
    pump.forbid_network_buffering = true;
    require(!player.progressive_download() && player.seekable_until() == player.duration() && !player.buffering(),
            "Completion did not unlock the full timeline and clear network buffering");
    previous = pump.frames;
    pump.until([&] { return pump.frames > previous && player.position() > 4.5; }, 7,
               "Real Player did not continue through appended bytes and final rename");
    pump.until([&] { return player.state() == Player::State::Ended; }, 4,
               "Progressive Player did not finish at the true completed file end");
    player.seek(1.0); // deliberately close before the queued seek can finish
    player.close();
    // Reopen uses an independent AVIO cursor on the same held inode.
    options.start = 2;
    player.open(options);
    pump.until([&] { return player.state() == Player::State::Playing && pump.frame.pts >= 1.9 && player.position() >= 2; }, 5,
               "Completed progressive file did not reopen at a saved resume position after a cancelled seek");
    player.close();
    std::cout << "PASS real Player progressive rendering, bounded future seek, local backward seek, temporary EOF, append, rename and resume\n";
}

void growing_vbr(const std::string& folder) {
    const std::string original = folder + "/vbr.mp4";
    std::ifstream input(original, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    AVFormatContext* format = nullptr;
    require(avformat_open_input(&format, original.c_str(), nullptr, nullptr) >= 0 &&
            avformat_find_stream_info(format, nullptr) >= 0, "VBR oracle could not open its complete fixture");
    const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(video >= 0, "VBR fixture has no video");
    const double duration = double(format->duration) / AV_TIME_BASE;
    const double tb = av_q2d(format->streams[video]->time_base);
    AVPacket* packet = av_packet_alloc();
    int64_t prefix = 0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video && packet->pts >= 0 && packet->pts * tb > 3.0) {
            prefix = packet->pos;
            av_packet_unref(packet);
            break;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    require(prefix > 0 && prefix < int64_t(bytes.size()), "VBR oracle did not find a partial prefix");
    const double linear_estimate = duration * double(prefix) / double(bytes.size());
    require(linear_estimate > 4.0, "VBR fixture did not meaningfully separate byte percentage from media time");
    const std::string path = folder + "/vbr.part";
    { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), prefix); }
    auto source = std::make_shared<GrowingFilePlayback>();
    source->path = path; source->descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    source->state = std::make_shared<GrowingFileState>(); source->generation = 14; source->total = int64_t(bytes.size());
    source->state->publish({prefix, source->total, 14, GrowingFilePhase::Downloading});
    require(source->descriptor >= 0, "VBR prefix descriptor did not open");
    Player player;
    Player::Options options; options.url = path; options.growing_download = source;
    player.open(options);
    Pump pump{player};
    pump.until([&] { return pump.frames > 1 && player.position() > .2; }, 6,
               "VBR partial playback did not start from local bytes");
    require(player.seekable_until() > .2 && player.seekable_until() <= 3.01 && player.seekable_until() < linear_estimate - .5,
            "VBR seek limit was derived from file percentage instead of actual local timestamps");
    pump.until([&] { return player.position() > 1.5 && !player.buffering(); }, 4,
               "VBR fixture did not reach its short remaining local queue");
#ifdef STREMIO_USE_LIBVLC
    // Native VLC exposes a conservative range of already presented times.
    // Freeze its coarse clock to separate a rejected seek from natural
    // playback reaching that same timestamp during the next clock update.
    player.set_paused(true);
    pump.for_time(.2);
    const double before_index_request = player.position();
    const int before_index_frames = pump.frames;
    const double index_request = player.seekable_until() + .5;
    player.seek(index_request);
    pump.for_time(.15);
    require(std::abs(player.position() - before_index_request) < .08 &&
            player.state() == Player::State::Playing && player.paused() && !player.buffering(),
            "A rejected VBR seek changed the paused timeline or readiness state");
    player.set_paused(false);
    pump.until([&] { return pump.frames > before_index_frames && player.position() > before_index_request + .1; }, 2,
               "A rejected VBR seek prevented playback from continuing within the local prefix");
    require(player.position() <= 3.01 && player.seekable_until() <= 3.01 && !player.buffering(),
            "VBR playback exceeded the physical packet prefix or entered a new buffering wait");
#else
    // The next keyframe is at 4 s, outside this prefix. Even a timestamp
    // below the demuxed limit must be rejected if its index span is absent.
    const double before_index_request = player.position();
    const int before_index_frames = pump.frames;
    const double index_request = 2.0;
    player.seek(index_request);
    pump.until([&] { return pump.frames > before_index_frames && player.position() > before_index_request + .1; }, 2,
               "A rejected local-index seek started a new buffering wait instead of continuing playback");
    require(player.position() < index_request && !player.buffering(),
            "A rejected local-index seek changed the timeline or readiness state");
#endif
    player.set_paused(true);
    const double old_position = player.position();
    player.seek(linear_estimate - .2);
    pump.for_time(.1);
    require(std::abs(player.position() - old_position) < .08 && player.state() == Player::State::Playing,
            "VBR future seek changed the clock or left playback waiting on nonexistent bytes");
    // Repeated requests at an unsupported future time must not build a queue
    // or make closing wait for the download's two-minute read timeout.
    for (int attempt = 0; attempt < 20; ++attempt) player.seek(linear_estimate);
    const double closing = seconds();
    player.close();
    require(seconds() - closing < 1.0, "Rejected VBR seeks left a blocked future read during close");
    options.start = linear_estimate;
    const int before_reopen = pump.frames;
    player.open(options);
    pump.until([&] { return player.state() == Player::State::Playing && pump.frames > before_reopen &&
                           pump.frame.pts < 1.0 && player.position() < 1.0; }, 6,
               "A stale resume offset opened an incomplete file beyond its actual downloaded time");
    player.close();
    std::cout << "PASS VBR physical prefix, conservative timestamp range, index rejection without new buffering, repeated future seek rejection and safe partial resume\n";
}

void network_av(const std::string& base, bool hls, bool byteranges = false) {
    Player player;
    Player::Options options;
    options.url = base + (byteranges ? "/single.m3u8" : hls ? "/stream.m3u8" : "/multi.mkv");
    options.headers = {"X-Stremio-Test: fixture-only-value", "Authorization: Bearer player-fixture-credential"};
    options.audio_langs = {"ita", "eng"};
    player.open(options);
    Pump pump{player};
    pump.until([&] { return pump.frames > 1 && player.position() >= .25; },
               8, hls ? "Protected HLS did not start" : "Protected HTTP file did not start");
    require(pump.frame.w == 640 && pump.frame.h == 360, "HTTP transport changed video dimensions");
    const int previous = pump.frames;
    player.seek(2.0);
    pump.until([&] { return pump.frames > previous && pump.frame.pts >= 1.9 && player.position() >= 2.0; },
               6, hls ? "Protected HLS seek failed" : "HTTP Range seek failed");
    player.close();
    std::cout << "PASS " << (byteranges ? "HLS single-file byte ranges" : hls ? "HLS playlist/segments" : "HTTP Range file")
              << " with custom headers and seek\n";
}

#ifdef STREMIO_USE_LIBVLC
void fallback_resume(const std::string& folder) {
    Player player;
    Player::Options options;
    options.url = folder + "/multi.mkv";
    options.start = 2.5;
    player.open(options);
    // Exercise the same failure notification used by a rejected PS5 stream,
    // before any frame is presented. The host still performs real VLC decode.
    vlc_ps5_vdec_failed();
    Pump pump{player};
    require(player.position() == 0, "An unpresented resume target was exposed as a saved position");
    pump.until([&] { return pump.frames > 1 && player.state() == Player::State::Playing; },
               6, "Software fallback did not start after a decoder rejection");
    require(pump.frame.pts >= 2.4 && player.position() >= 2.4,
            "Fallback before the first frame discarded the chosen resume time");
    player.close();
    std::cout << "PASS decoder-failure fallback preserves the validated resume target before the first frame\n";
}
#endif
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::cout.setf(std::ios::unitbuf);
    std::set_terminate([] {
        try {
            if (const auto error = std::current_exception()) std::rethrow_exception(error);
            std::cerr << "TERMINATE without active exception\n";
        } catch (const std::exception& error) {
            std::cerr << "TERMINATE: " << diagnostics_redact(error.what()) << '\n';
        } catch (...) { std::cerr << "TERMINATE with unknown exception\n"; }
        void* frames[48];
        const int count = backtrace(frames, 48);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        std::abort();
    });
    const std::string folder = argv[1], base = argv[2], app = argv[3];
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) return 3;
    diagnostics_start(folder + "/diagnostics/logs");
    void* initial_stack[1];
    (void)backtrace(initial_stack, 1); // load the host unwinder before a failure
    struct sigaction trace{};
    trace.sa_sigaction = trace_abort;
    trace.sa_flags = SA_SIGINFO;
    sigemptyset(&trace.sa_mask);
    sigaction(SIGABRT, &trace, &diagnostic_abort_action);
    av_log_set_callback(diagnostics_ffmpeg_log);
    http_init(app + "/ca-bundle.crt");
    int result = 0;
    const std::pair<const char*, std::function<void()>> cases[] = {
        {"audio", [&] { audio_exclusivity(); }},
        {"local", [&] { local_av(folder); }},
        {"main10", [&] { main10(folder); }},
        {"progressive", [&] { growing_download(folder); }},
        {"progressive-vbr", [&] { growing_vbr(folder); }},
        {"http", [&] { network_av(base, false); }},
        {"hls", [&] { network_av(base, true); }},
        {"hls-byteranges", [&] { network_av(base, true, true); }},
#ifdef STREMIO_USE_LIBVLC
        {"fallback-resume", [&] { fallback_resume(folder); }},
#endif
    };
    for (const auto& test : cases) {
        std::cout << "RUN " << test.first << '\n';
        try { test.second(); }
        catch (const std::exception& error) {
            std::cerr << "PLAYER_TEST_FAILURE " << test.first << ": " << diagnostics_redact(error.what()) << '\n';
            result = 1;
        }
    }
    if (!result) std::cout << "PLAYER_TESTS_OK checks=" << checks << '\n';
    diagnostics_stop();
    SDL_Quit();
    return result;
}
