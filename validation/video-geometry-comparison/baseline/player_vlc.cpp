// Stremio Plus native playback through libVLC.
// Callback output and PS5 decoder hooks are adapted from ZiZc3/VLC-PS5's
// src/player.cc (GPL-2.0-or-later), supplied with VLC 3.0.24. Video frames
// feed Stremio's existing OpenGL renderer and subtitles feed Prospero UI.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "player.h"
#include "player_vlc_audio.h"
#include "player_vlc_bridge.h"
#include "native_memory.h"
#include "player_vlc_dovi.h"
#include "growing_file_stream.h"
#include "torrent/torrent_stream.h"
#include "stremio.h"
#include "util.h"
#include "core/frame_stats.hpp"

#include <vlc/vlc.h>
#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_subpicture.h>
#include <vlc_picture.h>
#include <vlc_variables.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#ifdef PLATFORM_PS5
bool stremio_ps5_audio_pcm_available();
#endif
}

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace {
constexpr int kSlots = 4;
constexpr size_t kPlaneAlignment = 64;
constexpr uint64_t kAudioWithoutVideoLimit = VlcAudioTimeline::rate * 8;
#ifdef PLATFORM_PS5
constexpr const char* kNativeAudioShutdownError =
    "The native audio output could not be released safely. Playback has been stopped.";
#endif
std::atomic<int> hardware_decoders{0};
std::atomic<bool> hardware_failed{false};

bool regular_local_file(std::string path) {
    if (starts_with(path, "file://")) path.erase(0, 7);
    else if (starts_with(path, "file:")) path.erase(0, 5);
    struct stat status{};
    return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

size_t align_plane(size_t value) { return (value + kPlaneAlignment - 1) & ~(kPlaneAlignment - 1); }

struct InputOwner {
    Player::Options options;
    std::atomic<bool> abort{false}, playing{false}, fatal{false};
    std::mutex error_mutex;
    std::string error;
    void report(const std::string& message, bool terminal) {
        if (abort || message.empty()) return;
        { std::lock_guard lock(error_mutex); error = message; }
        if (terminal) fatal = true;
    }
    std::string failure() { std::lock_guard lock(error_mutex); return error; }
};

struct Input {
    InputOwner* owner = nullptr;
    AVIOContext* io = nullptr;
    bool growing = false, strict = false, opened = false;
    ~Input() {
        if (GrowingFileStream::of(io)) GrowingFileStream::close_avio(&io);
        else if (TorrentStream::is_ours(io)) TorrentStream::close_avio(&io);
        else NetStream::close_avio(&io);
    }
    std::string failure() const {
        if (auto* source = GrowingFileStream::of(io)) return source->failure();
        if (auto* source = TorrentStream::of(io)) return source->failure();
        if (auto* source = NetStream::of(io)) return source->failure();
        return "The video input could not be read.";
    }
};

int open_input(InputOwner* owner, const std::string& url, bool custom, void** output, int64_t* size) {
    auto input = std::make_unique<Input>();
    input->owner = owner;
    input->strict = custom;
    std::string error;
    if (custom && owner->options.growing_download) {
        input->growing = true;
        input->io = GrowingFileStream::open_avio(owner->options.growing_download, &owner->abort, nullptr, &error);
        if (input->io && owner->options.growing_download->torrent_hash.empty()) {
            // There is no torrent reader from which to fetch a tail index.
            // Reject unavailable index seeks immediately so demuxers can
            // continue from the local prefix instead of waiting at startup.
            GrowingFileStream::of(input->io)->finish_opening();
            input->opened = true;
        }
    } else if (custom && TorrentStream::is_url(url)) {
        input->io = TorrentStream::open_avio(url, &owner->abort, &error);
    } else {
        input->io = NetStream::open_avio(url, owner->options.headers, &owner->abort, &error,
            owner->options.parallel && url == owner->options.url);
    }
    if (!input->io) { owner->report(error, custom); return -1; }
    *size = input->io->seek ? input->io->seek(input->io->opaque, 0, AVSEEK_SIZE) : -1;
    *output = input.release();
    return 0;
}

void vlc_log(void*, int level, const libvlc_log_t* context, const char* format, va_list args) {
    if (level < LIBVLC_WARNING) return;
    char message[2048];
    vsnprintf(message, sizeof(message), format, args);
    const char* module = nullptr;
    libvlc_log_get_context(context, &module, nullptr, nullptr);
    dlog("vlc/%s: %s", module ? module : "core", message);
}
}

extern "C" int stremio_vlc_http_open(void* opaque, const char* url, void** output, int64_t* size) {
    if (!opaque || !url || !output || !size) return -1;
    try { return open_input(static_cast<InputOwner*>(opaque), url, false, output, size); }
    catch (...) { return -1; }
}

extern "C" ssize_t stremio_vlc_input_read(void* opaque, unsigned char* data, size_t size) {
    auto* input = static_cast<Input*>(opaque);
    if (!input || !data || !size) return 0;
    if (input->owner->abort) return -1;
    try {
        if (input->growing && !input->opened && input->owner->playing) {
            GrowingFileStream::of(input->io)->finish_opening();
            input->opened = true;
        }
        const int result = input->io->read_packet(input->io->opaque, data, int(std::min<size_t>(size, 256u << 10)));
        if (result == AVERROR_EOF) return 0;
        if (result < 0) {
            if (!input->owner->abort) input->owner->report(input->failure(), input->strict);
            return -1;
        }
        return result;
    } catch (...) { input->owner->fatal = true; return -1; }
}

extern "C" int stremio_vlc_input_seek(void* opaque, uint64_t offset) {
    auto* input = static_cast<Input*>(opaque);
    if (!input || offset > uint64_t(INT64_MAX) || input->owner->abort || !input->io->seek) return -1;
    try { return input->io->seek(input->io->opaque, int64_t(offset), SEEK_SET) == int64_t(offset) ? 0 : -1; }
    catch (...) { input->owner->fatal = true; return -1; }
}

extern "C" void stremio_vlc_input_close(void* opaque) { delete static_cast<Input*>(opaque); }

struct Player::Vlc {
    explicit Vlc(Player& parent) : parent(parent) {
        input.options = parent.opts_;
        local = !input.options.growing_download && regular_local_file(input.options.url);
        known_seek = std::max(0.0, input.options.progressive_resume_limit);
        clock_position = input.options.start;
        clock_stamp = libvlc_clock();
        next_format.display_aspect = 0;
        video_format_Init(&subtitle_format, VLC_CODEC_I420);
        subtitle_format.i_width = subtitle_format.i_visible_width = 1920;
        subtitle_format.i_height = subtitle_format.i_visible_height = 1080;
        subtitle_format.i_sar_num = subtitle_format.i_sar_den = 1;
    }
    ~Vlc() { stop(); video_format_Clean(&subtitle_format); }

    enum class Phase { Free, Writing, Ready, Shown };
    struct Slot { VideoFrame frame; Phase phase = Phase::Free; };
    struct Session {
        Vlc* owner = nullptr;
        std::mutex mutex;
        std::array<Slot, kSlots> slots;
        VideoFrame format;
        std::shared_ptr<void> scratch;
        int shown = -1;
    };

    Player& parent;
    InputOwner input;
    std::thread worker;
    libvlc_instance_t* instance = nullptr;
    libvlc_media_player_t* player = nullptr;
    libvlc_media_t* media = nullptr;
    std::atomic<bool> stop_requested{false}, seeking{false}, seek_time_seen{false};
    std::atomic<bool> native_error{false}, end_reached{false}, opened_video{false};
    std::atomic<bool> native_playing{false}, frame_presented{false}, native_position_known{false};
    std::atomic<bool> expects_video{true};
    std::atomic<int> decoder_error{0};
    std::atomic<bool> recovering_decoder{false};
    unsigned decoder_recoveries = 0;
    std::atomic<int> buffering_percent{0}, serial{0}, subtitle_serial{0}, wanted_audio{-2}, wanted_subtitle{-1};
    std::atomic<double> requested_seek{-1}, presented{0}, known_seek{0}, native_position{0};
    std::atomic<uint64_t> decoded{0}, shown{0}, dropped{0};
    std::atomic<int> decoded_pictures{0}, late_pictures{0};
    std::atomic<uint64_t> rendered_audio_frames{0}, setup_calls{0}, lock_calls{0}, display_calls{0};
    std::atomic<int64_t> last_picture_clock{0}, last_progress_clock{0}, picture_gap_limit{750000};
    std::atomic<double> last_display_position{-1};
    bool local = false, metadata_ready = false;
    std::atomic<bool> software_fallback{false};
    int applied_audio = -2, applied_subtitle = -2;
    bool applied_pause = false;
    std::atomic<double> seek_target{0};
    double last_info = 0, last_log = 0, media_start = 0;
    double seek_progress = 0, seek_previous_position = -1;
    bool audio_started_logged = false;
    uint64_t last_shown = 0, last_dropped = 0;

    mutable std::mutex error_mutex;
    std::string error;
    mutable std::mutex info_mutex;
    std::vector<Track> audio_tracks, subtitle_tracks;
    std::string description, telemetry;
    double fps = 0;

    // Bounded, allocation-free measurements at two distinct output stages.
    // VLC's callback clock is not the television's scanout clock; the main
    // loop logs swap intervals separately. Averages alone hide short hitches.
    struct Cadence {
        hui::FrameStats intervals, lateness;
        int64_t previous = 0;
        int serial = -1;
        void sample(int64_t now, double due, int generation) {
            if (serial != generation) { previous = 0; serial = generation; }
            if (previous > 0 && now > previous) intervals.add(double(now - previous) / 1000.0);
            previous = now;
            if (std::isfinite(due) && due > 0)
                lateness.add(std::max(0.0, double(now) / 1000.0 - due * 1000.0));
        }
        void clear_window() { intervals.reset(); lateness.reset(); }
    };
    std::mutex cadence_mutex;
    Cadence callback_cadence, pickup_cadence;
    double last_source_stamp = 0;
    uint64_t repeated_source_stamps = 0;

    std::mutex frames_mutex;
    std::vector<std::shared_ptr<Session>> sessions;
    std::shared_ptr<Session> current_session;
    VideoFrame next_format;
    video_format_t subtitle_format{};

    mutable std::mutex time_mutex;
    double clock_position = 0;
    int64_t clock_stamp = 0;
    bool clock_running = false;

    std::mutex audio_mutex;
    VlcAudioTimeline audio;
    SDL_AudioDeviceID device = 0;
    std::atomic<bool> passthrough_active{false}, passthrough_retry{false};
    std::atomic<bool> native_audio_shutdown_failed{false};
    std::atomic<const char*> audio_mode_status{"pcm"};
    int audio_period_us = 0;
    bool audio_paused = false;
    int64_t audio_pause_stamp = 0;
    float audio_gain = 1;

    mutable std::mutex subtitle_mutex;
    std::map<int, std::vector<Cue>> subtitles;
    std::map<int, bool> ephemeral_subtitle;

    void fail(const std::string& message) {
        if (stop_requested) return;
        const std::string reason = message.empty() ? "VLC could not open this video." : message;
        { std::lock_guard lock(error_mutex); error = reason; }
        dlog("player/vlc: %s", reason.c_str());
        parent.state_ = State::Failed;
    }

    static void video_error(void* opaque, int code) {
        if (!opaque) return;
        auto& self = *static_cast<Vlc*>(opaque);
        if (self.stop_requested || self.input.abort) return;
        // This runs on VLC's decoder/frame workers. Stop and reopen belong
        // to our worker, after these callbacks have released VLC's locks.
        int expected = 0;
        self.decoder_error.compare_exchange_strong(expected, code ? code : AVERROR(EINVAL));
    }

    double media_at(int64_t stamp, bool extrapolate = false) const {
        std::lock_guard lock(time_mutex);
        const double delta = (clock_running || extrapolate) ? double(stamp - clock_stamp) / 1000000.0 : 0.0;
        const double value = std::max(0.0, clock_position + delta);
        const double length = parent.duration_;
        return length > 0 ? std::min(value, length) : value;
    }

    void set_clock_running(bool running) {
        const int64_t now = libvlc_clock();
        std::lock_guard lock(time_mutex);
        if (clock_running) clock_position = std::max(0.0, clock_position + double(now - clock_stamp) / 1000000.0);
        clock_stamp = now;
        clock_running = running;
    }

    static void event(const libvlc_event_t* event, void* opaque) {
        auto& self = *static_cast<Vlc*>(opaque);
        if (self.stop_requested) return;
        switch (event->type) {
            case libvlc_MediaPlayerPlaying:
                self.native_playing = true;
                self.buffering_percent = 100;
                self.set_clock_running(!self.parent.paused_);
                break;
            case libvlc_MediaPlayerPaused:
                self.set_clock_running(false);
                break;
            case libvlc_MediaPlayerBuffering: {
                const float cache = event->u.media_player_buffering.new_cache;
                const int percent = std::isfinite(cache) ? std::clamp(int(cache), 0, 100) : 0;
                self.buffering_percent = percent;
                self.set_clock_running(percent >= 100 && self.native_playing && !self.parent.paused_);
                break;
            }
            case libvlc_MediaPlayerTimeChanged: {
                const double time = double(event->u.media_player_time_changed.new_time) / 1000.0;
                if (time < 0 || self.end_reached) break;
                self.native_position = time;
                self.native_position_known = true;
                { std::lock_guard lock(self.time_mutex); self.clock_position = time; self.clock_stamp = libvlc_clock(); }
                if (self.seeking && std::abs(time - self.seek_target.load()) < 1.0) self.seek_time_seen = true;
                break;
            }
            case libvlc_MediaPlayerLengthChanged: {
                const double length = double(event->u.media_player_length_changed.new_length) / 1000.0;
                if (length > 0) self.parent.duration_ = length;
                break;
            }
            case libvlc_MediaPlayerEndReached: self.end_reached = true; self.set_clock_running(false); break;
            case libvlc_MediaPlayerEncounteredError: self.native_error = true; self.set_clock_running(false); break;
            default: break;
        }
    }

    static void colour(void* opaque, const video_format_t* format) {
        auto& self = *static_cast<Vlc*>(opaque);
        std::lock_guard lock(self.frames_mutex);
        auto& frame = self.next_format;
        frame.colors = format->space == COLOR_SPACE_BT601 ? YuvColors::Bt601 : YuvColors::Bt709;
        frame.hlg = format->transfer == TRANSFER_FUNC_HLG;
        if (format->transfer == TRANSFER_FUNC_SMPTE_ST2084 || frame.hlg) frame.colors = YuvColors::Hdr10;
        frame.full_range = format->b_color_range_full;
        const unsigned width = format->i_visible_width ? format->i_visible_width : format->i_width;
        const unsigned height = format->i_visible_height ? format->i_visible_height : format->i_height;
        frame.w = int(width);
        frame.h = int(height);
        frame.display_aspect = height ? double(width) / height : 16.0 / 9.0;
        if (format->i_sar_num && format->i_sar_den) frame.display_aspect *= double(format->i_sar_num) / format->i_sar_den;
        self.subtitle_format.i_width = self.subtitle_format.i_visible_width = std::max(1u, width);
        self.subtitle_format.i_height = self.subtitle_format.i_visible_height = std::max(1u, height);
        self.subtitle_format.i_sar_num = format->i_sar_num ? format->i_sar_num : 1;
        self.subtitle_format.i_sar_den = format->i_sar_den ? format->i_sar_den : 1;
    }

    static unsigned video_setup(void** opaque, char* chroma, unsigned* width, unsigned* height,
        unsigned* pitches, unsigned* lines) {
        auto& self = *static_cast<Vlc*>(*opaque);
        ++self.setup_calls;
        dlog("player/vlc: video setup requested %.4s %ux%u", chroma, *width, *height);
        if (!*width || !*height || *width > 8192 || *height > 8192) {
            dlog("player/vlc: invalid video dimensions in output setup");
            return 0;
        }
        try {
            auto session = std::make_shared<Session>();
            session->owner = &self;
            const bool nv12 = std::memcmp(chroma, "NV12", 4) == 0;
            const bool p010 = std::memcmp(chroma, "P010", 4) == 0;
            const bool ten = p010 || !std::memcmp(chroma, "I0A", 3) ||
                !std::memcmp(chroma, "I2A", 3) || !std::memcmp(chroma, "I4A", 3);
            const bool full = !std::memcmp(chroma, "J4", 2);
            const bool interleaved = nv12 || p010;
            std::memcpy(chroma, interleaved ? (ten ? "P010" : "NV12") : (ten ? "I0AL" : "I420"), 4);
            VideoFrame frame;
            { std::lock_guard lock(self.frames_mutex); frame = self.next_format; }
            // VLC supplies coded dimensions here (for example 3840x2176).
            // Request the visible source rectangle, preserving its 4K image
            // without exposing codec padding or stretching the picture.
            if (frame.w > 0 && frame.h > 0 && frame.w <= int(*width) && frame.h <= int(*height)) {
                *width = unsigned(frame.w);
                *height = unsigned(frame.h);
            }
            frame.w = int(*width); frame.h = int(*height);
            frame.layout = interleaved ? VideoFrame::Layout::Nv12 : VideoFrame::Layout::Planar420;
            frame.wide = ten; frame.shift = p010 ? 6 : 0;
            frame.full_range = frame.full_range || full;
            if (!(frame.display_aspect > 0)) frame.display_aspect = double(*width) / *height;
            const size_t bytes = ten ? 2 : 1;
            const unsigned count = interleaved ? 2 : 3;
            size_t size = 0;
            for (unsigned i = 0; i < 3; ++i) {
                frame.offsets[i] = size;
                const size_t row = size_t(i ? (*width + 1) / 2 : *width) * bytes * ((i && interleaved) ? 2 : 1);
                pitches[i] = i < count ? unsigned(align_plane(row)) : 0;
                lines[i] = i < count ? (i ? (*height + 1) / 2 : *height) : 0;
                frame.pitches[i] = pitches[i];
                size += size_t(pitches[i]) * lines[i];
                size = align_plane(size);
            }
            // Four 4K10 output slots use about 100 MiB. Reject implausible
            // formats before multiplication can become an unbounded pool.
            if (!size || size > (64u << 20)) {
                dlog("player/vlc: video output plane size rejected (%zu bytes)", size);
                return 0;
            }
            frame.mapped_size = size;
            session->format = frame;
            auto allocate = [size]() -> std::shared_ptr<void> {
                void* pixels = nullptr;
#ifdef PLATFORM_PS5_NATIVE
                pixels = vlc_ps5_big_alloc(size);
                if (pixels) return std::shared_ptr<void>(pixels, [](void* address) {
                    vlc_ps5_big_free(address);
                });
#endif
                if (posix_memalign(&pixels, kPlaneAlignment, size)) return {};
                return std::shared_ptr<void>(pixels, std::free);
            };
            for (auto& slot : session->slots) {
                slot.frame = frame;
                slot.frame.storage = allocate();
                if (!slot.frame.storage) {
                    dlog("player/vlc: video output allocation failed (%zu bytes per slot)", size);
                    return 0;
                }
                slot.frame.mapped = static_cast<const uint8_t*>(slot.frame.storage.get());
            }
            session->scratch = allocate();
            if (!session->scratch) {
                dlog("player/vlc: video output scratch allocation failed (%zu bytes)", size);
                return 0;
            }
            { std::lock_guard lock(self.frames_mutex); self.sessions.push_back(session); self.current_session = session; }
            *opaque = session.get();
            dlog("player/vlc: %.4s %ux%u, %d aligned slots of %zu KiB, GPU color conversion",
                chroma, *width, *height, kSlots, size / 1024);
            return kSlots;
        } catch (...) {
            dlog("player/vlc: video output setup failed with an allocation exception");
            return 0;
        }
    }

    static void video_cleanup(void* opaque) {
        auto* session = static_cast<Session*>(opaque);
        auto& self = *session->owner;
        std::lock_guard lock(self.frames_mutex);
        if (self.current_session.get() == session) self.current_session.reset();
        self.sessions.erase(std::remove_if(self.sessions.begin(), self.sessions.end(),
            [session](const auto& entry) { return entry.get() == session; }), self.sessions.end());
    }

    static void* video_lock(void* opaque, void** planes) {
        auto& session = *static_cast<Session*>(opaque);
        ++session.owner->lock_calls;
        std::lock_guard lock(session.mutex);
        int found = -1;
        for (int i = 0; i < kSlots; ++i) {
            auto& slot = session.slots[i];
            if (slot.phase == Phase::Free && slot.frame.storage.use_count() == 1) { found = i; break; }
        }
        if (found < 0 && session.shown >= 0 && session.slots[session.shown].frame.storage.use_count() == 1) {
            found = session.shown;
            session.shown = -1;
            ++session.owner->dropped;
        }
        uint8_t* memory = static_cast<uint8_t*>(session.scratch.get());
        if (found >= 0) {
            auto& slot = session.slots[found];
            slot.phase = Phase::Writing;
            slot.frame.serial = session.owner->serial;
            memory = static_cast<uint8_t*>(slot.frame.storage.get());
        } else ++session.owner->dropped;
        for (unsigned i = 0; i < PICTURE_PLANE_MAX; ++i) planes[i] = nullptr;
        for (unsigned i = 0; i < 3; ++i) planes[i] = memory + session.format.offsets[i];
        return found >= 0 ? reinterpret_cast<void*>(intptr_t(found + 1)) : nullptr;
    }

    static void video_unlock(void* opaque, void* picture, void* const*) {
        auto& session = *static_cast<Session*>(opaque);
        const int index = int(reinterpret_cast<intptr_t>(picture)) - 1;
        if (index < 0 || index >= kSlots) return;
        std::lock_guard lock(session.mutex);
        if (session.slots[index].phase == Phase::Writing) session.slots[index].phase = Phase::Ready;
    }

    static void video_metadata(void* opaque, void* picture, const picture_t* source) {
        auto& session = *static_cast<Session*>(opaque);
        const int index = int(reinterpret_cast<intptr_t>(picture)) - 1;
        if (index < 0 || index >= kSlots || !source) return;
        std::lock_guard lock(session.mutex);
        auto& frame = session.slots[index].frame;
        frame.presentation_time = source->date > 0 ? double(source->date) / 1000000.0 : 0;
        frame.dovi = vlc_picture_dovi(source);
    }

    static void video_display(void* opaque, void* picture) {
        auto& session = *static_cast<Session*>(opaque);
        auto& self = *session.owner;
        ++self.display_calls;
        const int index = int(reinterpret_cast<intptr_t>(picture)) - 1;
        if (index < 0 || index >= kSlots) return;
        const int64_t now = libvlc_clock();
        // VLC adjusts TimeChanged for the decoded buffer. Extrapolating it
        // with wall time would falsely advance the bookmark when a growing
        // file blocks without emitting a new buffering event.
        const bool position_known = self.native_position_known;
        const double pts = self.native_position.load();
        std::lock_guard lock(session.mutex);
        auto& slot = session.slots[index];
        if (slot.phase != Phase::Ready) return;
        if (self.decoder_error || slot.frame.serial != self.serial || !position_known ||
            (self.seeking && !self.seek_time_seen)) {
            slot.phase = Phase::Free;
            return;
        }
        if (session.shown >= 0 && session.shown != index) {
            session.slots[session.shown].phase = Phase::Free;
            ++self.dropped;
        }
        slot.frame.pts = pts;
        {
            std::lock_guard timing(self.cadence_mutex);
            if (!self.parent.paused_ && !self.seeking) {
                if (self.callback_cadence.serial == slot.frame.serial && slot.frame.presentation_time > 0 &&
                    slot.frame.presentation_time == self.last_source_stamp) ++self.repeated_source_stamps;
                self.callback_cadence.sample(now, slot.frame.presentation_time, slot.frame.serial);
                self.last_source_stamp = slot.frame.presentation_time;
            }
        }
        if (std::abs(pts - self.last_display_position) > 0.0005 || !self.last_progress_clock) {
            self.last_display_position = pts;
            self.last_progress_clock = now;
        }
        slot.phase = Phase::Shown;
        session.shown = index;
        ++self.decoded;
        self.last_picture_clock = now;
        if (!self.opened_video.exchange(true))
            dlog("player/vlc: first video picture ready, %dx%d, media %.3fs",
                slot.frame.w, slot.frame.h, pts);
        if (self.seeking) self.seeking = false;
    }

    static void audio_output(void* opaque, Uint8* output, int length) {
        auto& self = *static_cast<Vlc*>(opaque);
        if (length <= 0) return;
        std::memset(output, 0, size_t(length));
        if (self.stop_requested || self.parent.paused_) return;
        std::lock_guard lock(self.audio_mutex);
        if (self.audio_paused) return;
        const size_t played = self.audio.render(reinterpret_cast<int16_t*>(output), size_t(length) / 4,
            libvlc_clock() + self.audio_period_us, self.audio_gain * playback_volume::gain(self.parent.volume()));
        self.rendered_audio_frames.fetch_add(played, std::memory_order_relaxed);
    }
    static void audio_play(void* opaque, const void* samples, unsigned count, int64_t pts) {
        auto& self = *static_cast<Vlc*>(opaque);
        if (self.stop_requested || self.input.abort) return;
        std::lock_guard lock(self.audio_mutex);
        try { self.audio.push(static_cast<const int16_t*>(samples), count, pts); }
        catch (...) { self.input.fatal = true; }
    }
    static void audio_pause(void* opaque, int64_t pts) {
        auto& self = *static_cast<Vlc*>(opaque);
        std::lock_guard lock(self.audio_mutex);
        self.audio_paused = true;
        self.audio_pause_stamp = pts;
    }
    static void audio_resume(void* opaque, int64_t pts) {
        auto& self = *static_cast<Vlc*>(opaque);
        std::lock_guard lock(self.audio_mutex);
        if (self.audio_paused && self.audio_pause_stamp > 0 && pts >= self.audio_pause_stamp)
            self.audio.shift(pts - self.audio_pause_stamp);
        self.audio_paused = false;
        self.audio_pause_stamp = 0;
    }
    static void audio_flush(void* opaque, int64_t) {
        auto& self = *static_cast<Vlc*>(opaque);
        std::lock_guard lock(self.audio_mutex);
        self.audio.clear();
    }
    static void audio_drain(void* opaque) {
        auto& self = *static_cast<Vlc*>(opaque);
        const double deadline = now_seconds() + 4.5;
        while (!self.stop_requested && !self.input.abort && now_seconds() < deadline) {
            { std::lock_guard lock(self.audio_mutex); if (!self.audio.queued() || self.audio_paused) return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    static void audio_volume(void* opaque, float volume, bool mute) {
        auto& self = *static_cast<Vlc*>(opaque);
        std::lock_guard lock(self.audio_mutex);
        self.audio_gain = mute ? 0.0f : std::clamp(volume, 0.0f, 4.0f);
    }

    bool open_pcm_device() {
#ifdef PLATFORM_PS5
        if (!stremio_ps5_audio_pcm_available()) {
            SDL_SetError("Native audio output is still owned or could not be released safely.");
            return false;
        }
#endif
        if (device) return true;
        SDL_AudioSpec wanted{}, obtained{};
        wanted.freq = int(VlcAudioTimeline::rate); wanted.format = AUDIO_S16SYS; wanted.channels = 2;
        wanted.samples = 1024; wanted.callback = audio_output; wanted.userdata = this;
        device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (!device) return false;
        audio_period_us = int(int64_t(obtained.samples) * 1000000 / VlcAudioTimeline::rate);
        SDL_PauseAudioDevice(device, 0);
        return true;
    }

#ifdef PLATFORM_PS5
    static void audio_mode(void* opaque, bool bitstream, const char* reason) {
        auto& self = *static_cast<Vlc*>(opaque);
        self.audio_mode_status = reason;
        dlog("player/vlc: audio output %s", reason);
        const bool shutdown_failed = std::strcmp(reason, "native_shutdown_failed") == 0;
        if (shutdown_failed) {
            // The native module retains its buffers and port ownership after
            // a failed shutdown. Only the worker may stop VLC; opening PCM
            // from this callback would race a still-live encoded output.
            self.native_audio_shutdown_failed = true;
            self.input.fatal = true;
            self.passthrough_retry = false;
        }
        if (std::strcmp(reason, "output_failed") == 0) {
            // Stop the encoded port before the PCM port is reopened. This
            // retry is handled by the player worker outside VLC's aout lock.
            self.passthrough_retry = true;
            return;
        }
        const bool encoded_owned = bitstream || shutdown_failed;
        self.passthrough_active = encoded_owned;
        if (encoded_owned && self.device) {
            SDL_CloseAudioDevice(self.device); self.device = 0;
            std::lock_guard lock(self.audio_mutex); self.audio.clear();
        } else if (!encoded_owned && !self.stop_requested && !self.input.abort && !self.open_pcm_device()) {
            self.input.fatal = true;
        }
    }
#endif

    static bool subtitle(void* opaque, int stream, subpicture_t* picture) {
        auto& self = *static_cast<Vlc*>(opaque);
        if (self.stop_requested || stream != self.wanted_subtitle.load()) return true;
        const int generation = self.subtitle_serial;
        try {
            video_format_t format;
            { std::lock_guard lock(self.frames_mutex); format = self.subtitle_format; }
            subpicture_Update(picture, &format, &format, picture->i_start);
            std::string text;
            bool has_text = false, has_bitmap = false;
            for (auto* region = picture->p_region; region; region = region->p_next) {
                if (region->p_picture && !region->p_text) has_bitmap = true;
                if (!region->p_text) continue;
                has_text = true;
                if (!text.empty()) text += '\n';
                for (auto* segment = region->p_text; segment; segment = segment->p_next) {
                    if (!segment->psz_text) continue;
                    const bool italic = segment->style && (segment->style->i_style_flags & STYLE_ITALIC);
                    const bool bold = segment->style && (segment->style->i_style_flags & STYLE_BOLD);
                    if (italic) text += "<i>";
                    if (bold) text += "<b>";
                    text += segment->psz_text;
                    if (bold) text += "</b>";
                    if (italic) text += "</i>";
                    if (text.size() > (1u << 20)) return true;
                }
            }
            // Image-based PGS/DVD captions keep VLC's bitmap compositor.
            if (!has_text && has_bitmap) {
                std::lock_guard lock(self.subtitle_mutex);
                return generation != self.subtitle_serial || stream != self.wanted_subtitle;
            }
            Cue cue;
            cue.start = self.media_at(picture->i_start, true);
            cue.end = picture->i_stop > picture->i_start ? self.media_at(picture->i_stop, true) : cue.start + 8;
            cue.rml = subtitle_text_to_rml(text);
            std::lock_guard lock(self.subtitle_mutex);
            if (generation != self.subtitle_serial || stream != self.wanted_subtitle) return true;
            auto& cues = self.subtitles[stream];
            if (self.ephemeral_subtitle[stream] && !cues.empty()) cues.back().end = std::min(cues.back().end, cue.start);
            self.ephemeral_subtitle[stream] = picture->b_ephemer;
            if (!cue.rml.empty()) {
                auto at = std::lower_bound(cues.begin(), cues.end(), cue.start,
                    [](const Cue& a, double start) { return a.start < start; });
                if (at == cues.end() || at->start != cue.start || at->rml != cue.rml) cues.insert(at, std::move(cue));
            }
            if (cues.size() > 512) cues.erase(cues.begin(), cues.begin() + ptrdiff_t(cues.size() - 512));
            return true;
        } catch (...) { return true; }
    }

    static int media_open(void* opaque, void** output, uint64_t* size) {
        auto* self = static_cast<Vlc*>(opaque);
        int64_t bytes = -1;
        try {
            const int result = open_input(&self->input, self->input.options.url, true, output, &bytes);
            *size = bytes >= 0 ? uint64_t(bytes) : UINT64_MAX;
            return result;
        } catch (...) { return -1; }
    }

    void refresh_info() {
        std::vector<Track> audio_list, subtitle_list;
        std::vector<std::string> codecs;
        libvlc_media_track_t** tracks = nullptr;
        const unsigned count = libvlc_media_tracks_get(media, &tracks);
        bool has_video = false;
        double frame_rate = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto* track = tracks[i];
            const char* codec = libvlc_media_get_codec_description(track->i_type, track->i_codec);
            const std::string name = codec ? codec : "";
            const std::string language = track->psz_language ? language_to_iso639_2(track->psz_language) : "";
            std::vector<std::string> labels;
            if (!language.empty() && language != "und") labels.push_back(language_name(language));
            if (track->psz_description && *track->psz_description) labels.emplace_back(track->psz_description);
            if (!name.empty()) labels.push_back(name);
            if (track->i_type == libvlc_track_audio) {
                const unsigned channels = track->audio->i_channels;
                labels.push_back(channels == 1 ? "mono" : channels == 2 ? "stereo" : channels == 6 ? "5.1" :
                    channels == 8 ? "7.1" : std::to_string(channels) + "ch");
                audio_list.push_back({track->i_id, join(labels, " · "), language});
            } else if (track->i_type == libvlc_track_text) {
                subtitle_list.push_back({track->i_id, join(labels, " · "), language});
            } else if (track->i_type == libvlc_track_video && !has_video) {
                has_video = true;
                const auto& video = *track->video;
                frame_rate = video.i_frame_rate_den ? double(video.i_frame_rate_num) / video.i_frame_rate_den : 0;
                codecs.push_back(name + " " + std::to_string(video.i_width) + "x" + std::to_string(video.i_height) +
                    (hardware_decoders > 0 ? " (PS5 hardware)" : " (software)"));
            }
        }
        if (tracks) libvlc_media_tracks_release(tracks, count);
        const int active_audio = libvlc_audio_get_track(player);
        parent.audio_stream_ = active_audio;
        for (const auto& track : audio_list) if (track.stream == active_audio) { codecs.push_back(track.label); break; }
        const libvlc_time_t length = libvlc_media_player_get_length(player);
        if (length > 0) parent.duration_ = double(length) / 1000;
        libvlc_media_stats_t stats{};
        libvlc_media_get_stats(media, &stats);
        decoded_pictures = stats.i_decoded_video;
        late_pictures = stats.i_lost_pictures;
        char line[512];
        const std::string summary = "VLC " + std::string(libvlc_get_version()) + " · " + join(codecs, " · ");
        snprintf(line, sizeof(line), "%s · %.3g fps · shown %llu · late %d · output drops %llu",
            summary.c_str(), frame_rate, (unsigned long long)shown.load(), stats.i_lost_pictures,
            (unsigned long long)dropped.load());
        {
            std::lock_guard lock(info_mutex);
            audio_tracks = std::move(audio_list);
            subtitle_tracks = std::move(subtitle_list);
            description = summary;
            telemetry = line;
            fps = frame_rate;
        }
        if (count) { metadata_ready = true; expects_video = has_video; }
        if (native_playing && metadata_ready && (opened_video || !has_video) && !native_error && !end_reached) {
            parent.state_ = State::Playing;
            input.playing = true;
            if (!has_video && native_position_known) {
                if (seek_time_seen) seeking = false;
                if (!seeking) presented = native_position.load();
            }
        }
        picture_gap_limit = frame_rate > 0 ? std::max<int64_t>(750000, int64_t(3000000 / frame_rate)) : 750000;
        parent.too_heavy_ = stats.i_decoded_video > 180 && stats.i_lost_pictures * 4 > stats.i_decoded_video;
    }

    bool start_media(double start) {
        if (stop_requested) return false;
#ifdef PLATFORM_PS5
        if (native_audio_shutdown_failed) {
            fail(kNativeAudioShutdownError);
            return false;
        }
#endif
        const auto& options = input.options;
        if (options.growing_download || TorrentStream::is_url(options.url))
            media = libvlc_media_new_callbacks(instance, media_open, stremio_vlc_input_read,
                stremio_vlc_input_seek, stremio_vlc_input_close, this);
        else if (regular_local_file(options.url)) {
            std::string path = options.url;
            if (starts_with(path, "file://")) path.erase(0, 7);
            else if (starts_with(path, "file:")) path.erase(0, 5);
            media = libvlc_media_new_path(instance, path.c_str());
        } else media = libvlc_media_new_location(instance, options.url.c_str());
        if (!media) { fail(libvlc_errmsg() ? libvlc_errmsg() : "VLC could not create this media input."); return false; }
        libvlc_media_add_option(media, ":network-caching=1500");
        libvlc_media_add_option(media, ":file-caching=750");
        libvlc_media_add_option(media, ":no-sub-autodetect-file");
        libvlc_media_add_option(media, ":sub-track=-1");
        const int initial_subtitle = wanted_subtitle;
        if (initial_subtitle >= 0) {
            const std::string option = ":sub-track-id=" + std::to_string(initial_subtitle);
            libvlc_media_add_option(media, option.c_str());
        }
        if (!options.audio_langs.empty()) {
            const std::string languages = ":audio-language=" + join(options.audio_langs, ",");
            libvlc_media_add_option(media, languages.c_str());
        }
        if (start > 0) {
            char option[96]; snprintf(option, sizeof(option), ":start-time=%.6f", start);
            libvlc_media_add_option(media, option);
        }
        if (software_fallback) libvlc_media_add_option(media, ":no-ps5vdec");
#ifdef PLATFORM_PS5_NATIVE
        // An exhausted decoder gets one retry with fewer retained threaded
        // pictures. The normal playback path retains its four workers.
        if (decoder_recoveries) libvlc_media_add_option(media, ":avcodec-threads=2");
#endif
        decoder_error = 0;
        native_error = end_reached = native_playing = opened_video = false;
        frame_presented = false;
        expects_video = true;
        setup_calls = lock_calls = display_calls = 0;
        audio_started_logged = false;
        media_start = start;
        seeking = start > 0;
        seek_time_seen = false;
        seek_target = start;
        seek_progress = 0;
        seek_previous_position = -1;
        native_position_known = false;
        native_position = 0;
        last_picture_clock = 0;
        last_progress_clock = 0;
        buffering_percent = 0;
        metadata_ready = false;
        applied_subtitle = initial_subtitle >= 0 ? initial_subtitle : -2;
        applied_audio = -2;
        applied_pause = !parent.paused_.load();
        input.playing = input.fatal = false;
        { std::lock_guard lock(input.error_mutex); input.error.clear(); }
        { std::lock_guard lock(audio_mutex); audio.clear(); audio_paused = false; audio_pause_stamp = 0; rendered_audio_frames = 0; }
        { std::lock_guard lock(time_mutex); clock_position = start; clock_stamp = libvlc_clock(); clock_running = false; }
        libvlc_media_player_set_media(player, media);
        if (stop_requested) return false;
        if (libvlc_media_player_play(player) < 0) { fail(libvlc_errmsg() ? libvlc_errmsg() : "VLC could not start playback."); return false; }
        dlog("player/vlc: opening %s at %.3fs%s", local ? "local file" : "stream", start,
            software_fallback ? " with software fallback" : "");
        return true;
    }

    void clear_output() {
        ++serial;
        { std::lock_guard lock(subtitle_mutex); ++subtitle_serial; subtitles.clear(); ephemeral_subtitle.clear(); }
        { std::lock_guard lock(audio_mutex); audio.clear(); }
        std::vector<std::shared_ptr<Session>> held;
        { std::lock_guard lock(frames_mutex); held = sessions; }
        for (auto& session : held) {
            std::lock_guard lock(session->mutex);
            if (session->shown >= 0) session->slots[session->shown].phase = Phase::Free;
            session->shown = -1;
        }
    }

    bool recover_video_output(const char* reason) {
        if (software_fallback) {
            dlog("player/vlc: video startup failed after software retry (%s)", reason);
            fail("The video decoder did not produce a picture. Playback has been stopped; try another stream.");
            return false;
        }
        const double resume = frame_presented ? presented.load() : media_start;
        dlog("player/vlc: %s; reopening the same input at %.3fs with software decoding", reason, resume);
        parent.state_ = State::Opening;
        input.abort = true;
        libvlc_media_player_stop(player);
        libvlc_media_release(media); media = nullptr;
        clear_output();
        if (stop_requested) return false;
        input.abort = false;
        software_fallback = true;
        return start_media(resume);
    }

    bool recover_decoder(int code) {
        if (decoder_recoveries >= 1) {
            dlog("player/vlc: video decoder failed again after recovery (error=%d)", code);
            fail("The video decoder could not continue after recovery. Reopen the file or choose another stream.");
            return false;
        }
        ++decoder_recoveries;
        // Only a picture consumed by the renderer establishes a safe resume
        // point. The demux/audio clocks can run ahead of a failed decoder.
        const double resume = frame_presented ? presented.load() : media_start;
        dlog("player/vlc: video decoder error=%d; recovery %u/1 at last presented position %.3fs",
            code, decoder_recoveries, resume);
        recovering_decoder = true;
        parent.state_ = State::Opening;
        input.abort = true;
        libvlc_media_player_stop(player);
        libvlc_media_release(media); media = nullptr;
        clear_output();
        if (stop_requested) return false;
        input.abort = false;
        // A decoder error must not send this input through an already
        // rejected PS5 hardware path again.
        software_fallback = true;
        return start_media(resume);
    }

    void run() {
        const char* arguments[] = {
            "--ignore-config", "--no-video-title-show", "--no-sub-autodetect-file", "--stats",
            // VLC 3.0.24 reads drop-late-frames only in its video output:
            // discard stale, fully decoded pictures instead of sending a
            // catch-up burst through vmem/GL. Keep compressed-frame skipping
            // disabled so the decoder retains every P/B reference picture.
            "--drop-late-frames", "--no-skip-frames", "--avcodec-skip-frame=0",
            "--no-avcodec-hurry-up", "--no-avcodec-corrupted",
            "--aout=adummy",
#ifdef PLATFORM_PS5_NATIVE
            // Frame threading adds two retained VLC pictures per worker.
            // Bound the software fallback within the native memory budget;
            // PS5 hardware decoding keeps its own pipeline and worker setup.
            "--avcodec-threads=4",
#else
            "--avcodec-threads=0",
#endif
            "--subsdec-encoding=UTF-8", "--verbose=-1",
            "--adaptive-use-access",
        };
        try {
            instance = libvlc_new(int(sizeof(arguments) / sizeof(arguments[0])), arguments);
            if (!instance) { fail(libvlc_errmsg() ? libvlc_errmsg() : "VLC initialization failed."); return; }
            libvlc_log_set(instance, vlc_log, nullptr);
            libvlc_set_user_agent(instance, "Stremio Plus", "StremioPlus/" STREMIO_VERSION " libVLC");
            player = libvlc_media_player_new(instance);
            if (!player) { fail("VLC could not create the media player."); cleanup(); return; }
            auto* object = reinterpret_cast<vlc_object_t*>(player);
            const auto variable = [object](const char* name, void* address) {
                var_Create(object, name, VLC_VAR_ADDRESS); var_SetAddress(object, name, address);
            };
            variable("vmem-colour", reinterpret_cast<void*>(colour));
            variable("stremio-video-metadata", reinterpret_cast<void*>(video_metadata));
            variable("stremio-video-error", reinterpret_cast<void*>(video_error));
            variable("stremio-video-error-opaque", this);
            variable("stremio-subtitle-callback", reinterpret_cast<void*>(subtitle));
            variable("stremio-subtitle-opaque", this);
            variable("stremio-http-opaque", &input);
            var_Create(object, "stremio-input-direct", VLC_VAR_BOOL);
            var_SetBool(object, "stremio-input-direct", input.options.growing_download != nullptr);
            libvlc_video_set_callbacks(player, video_lock, video_unlock, video_display, this);
            libvlc_video_set_format_callbacks(player, video_setup, video_cleanup);
            libvlc_audio_set_callbacks(player, audio_play, audio_pause, audio_resume, audio_flush, audio_drain, this);
            libvlc_audio_set_volume_callback(player, audio_volume);
            libvlc_audio_set_format(player, "S16N", VlcAudioTimeline::rate, VlcAudioTimeline::channels);
#ifdef PLATFORM_PS5
            var_Create(object, "stremio-audio-passthrough", VLC_VAR_BOOL);
            var_SetBool(object, "stremio-audio-passthrough", input.options.audio_passthrough);
            variable("stremio-audio-mode", reinterpret_cast<void*>(audio_mode));
            if (input.options.audio_passthrough) var_SetString(object, "aout", "stremiops5audio,amem");
#endif
            if (!open_pcm_device()) { fail(std::string("Audio output could not be opened: ") + SDL_GetError()); cleanup(); return; }
            auto* events = libvlc_media_player_event_manager(player);
            const libvlc_event_e event_types[] = {libvlc_MediaPlayerPlaying, libvlc_MediaPlayerPaused,
                libvlc_MediaPlayerBuffering, libvlc_MediaPlayerTimeChanged, libvlc_MediaPlayerLengthChanged,
                libvlc_MediaPlayerEndReached, libvlc_MediaPlayerEncounteredError};
            for (auto type : event_types) libvlc_event_attach(events, type, event, this);
            if (!stop_requested && start_media(input.options.start)) {
                while (!stop_requested) {
#ifdef PLATFORM_PS5
                    if (native_audio_shutdown_failed) { fail(kNativeAudioShutdownError); break; }
#endif
                    if (const int code = decoder_error.load()) {
                        if (!recover_decoder(code)) break;
                        continue;
                    }
#ifdef PLATFORM_PS5
                    if (passthrough_retry.exchange(false)) {
                        const double resume = frame_presented ? presented.load() : media_start;
                        parent.state_ = State::Opening;
                        input.abort = true;
                        libvlc_media_player_stop(player);
                        libvlc_media_release(media); media = nullptr;
                        if (native_audio_shutdown_failed) { fail(kNativeAudioShutdownError); break; }
                        var_SetBool(object, "stremio-audio-passthrough", false);
                        audio_mode_status = "pcm_after_output_error";
                        clear_output();
                        if (stop_requested) break;
                        input.abort = false;
                        if (!open_pcm_device()) {
                            fail(std::string("Audio output could not be opened: ") + SDL_GetError());
                            break;
                        }
                        if (!start_media(resume)) break;
                    }
#endif
                    if (hardware_failed.exchange(false) && !software_fallback) {
                        if (!recover_video_output("PS5 decoder rejected the stream")) break;
                    }
                    if (input.fatal || native_error) { fail(input.failure()); break; }
                    if (metadata_ready && expects_video && !opened_video &&
                        (end_reached || rendered_audio_frames >= kAudioWithoutVideoLimit)) {
                        // Count PCM actually sent to the audio device, not
                        // elapsed startup time: waiting for peers, a paused
                        // player and an empty audio queue cannot trigger this.
                        if (!recover_video_output(end_reached ? "input ended without a video picture" :
                            "audio played for eight seconds without a video picture")) break;
                        continue;
                    }
                    const double target = requested_seek.exchange(-1);
                    if (end_reached) {
                        parent.state_ = State::Ended;
                        if (target >= 0) {
                            dlog("player/vlc: seek_apply branch=ended_reopen current_s=%.9g target_s=%.9g",
                                presented.load(), target);
                            input.abort = true;
                            libvlc_media_player_stop(player);
                            libvlc_media_release(media); media = nullptr;
                            clear_output();
                            input.abort = false;
                            parent.state_ = State::Opening;
                            if (!start_media(target)) break;
                        }
                    } else if (target >= 0 && parent.progressive_download()) {
                        dlog("player/vlc: seek_apply branch=growing_reopen current_s=%.9g target_s=%.9g",
                            presented.load(), target);
                        // A demuxer blocked in a growing-file read cannot
                        // service an asynchronous seek. Cancel that reader
                        // and reopen the same held file at the proven point.
                        input.abort = true;
                        libvlc_media_player_stop(player);
                        libvlc_media_release(media); media = nullptr;
                        clear_output();
                        input.abort = false;
                        parent.state_ = State::Opening;
                        if (!start_media(target)) break;
                    } else if (target >= 0 && libvlc_media_player_is_seekable(player)) {
                        dlog("player/vlc: seek_apply branch=direct current_s=%.9g target_s=%.9g",
                            presented.load(), target);
                        seek_target = target;
                        seeking = true; seek_time_seen = false;
                        seek_progress = 0;
                        seek_previous_position = -1;
                        native_position_known = false;
                        clear_output();
                        libvlc_media_player_set_time(player, libvlc_time_t(target * 1000));
                    } else if (target >= 0) {
                        dlog("player/vlc: seek_apply branch=not_seekable current_s=%.9g target_s=%.9g",
                            presented.load(), target);
                    }
                    if (seeking && native_position_known) {
                        const double position = native_position;
                        // A demuxer may ignore a seek. Count actual timeline
                        // progress before accepting that outcome; time spent
                        // fetching metadata or waiting for peers proves nothing.
                        if (!parent.paused_ && seek_previous_position >= 0 && position > seek_previous_position)
                            seek_progress += std::min(position - seek_previous_position, 1.0);
                        seek_previous_position = position;
                        if (seek_progress >= 5.0) {
                            seeking = false;
                            dlog("player/vlc: seek to %.3fs was not confirmed after %.3fs of decoder progress; following %.3fs",
                                seek_target.load(), seek_progress, position);
                        }
                    }
                    const bool paused = parent.paused_;
                    if (applied_pause != paused) {
                        libvlc_media_player_set_pause(player, paused ? 1 : 0);
                        applied_pause = paused;
                    }
                    const int audio_track = wanted_audio;
                    if (audio_track >= -1 && applied_audio != audio_track && native_playing &&
                        libvlc_audio_set_track(player, audio_track) == 0) applied_audio = audio_track;
                    const int subtitle_track = wanted_subtitle;
                    if (applied_subtitle != subtitle_track && native_playing &&
                        libvlc_video_set_spu(player, subtitle_track) == 0) {
                        applied_subtitle = subtitle_track;
                        // Track selection must not seek or reopen the media.
                        // VLC supplies cues from this point onward.
                    }
                    const double now = now_seconds();
                    if (now - last_info > 0.2) { refresh_info(); last_info = now; }
                    if (!audio_started_logged && rendered_audio_frames > 0) {
                        audio_started_logged = true;
                        dlog("player/vlc: audio output active; first video picture=%d", int(opened_video.load()));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            cleanup();
        } catch (const std::exception& exception) { fail(exception.what()); cleanup(); }
        catch (...) { fail("VLC playback stopped unexpectedly."); cleanup(); }
    }

    void cleanup() {
        input.abort = true;
        if (player) { libvlc_media_player_stop(player); libvlc_media_player_release(player); player = nullptr; }
        if (media) { libvlc_media_release(media); media = nullptr; }
        if (instance) { libvlc_log_unset(instance); libvlc_release(instance); instance = nullptr; }
        if (device) { SDL_CloseAudioDevice(device); device = 0; }
    }

    void stop() {
        stop_requested = true;
        input.abort = true;
        if (worker.joinable()) worker.join();
    }
};

Player::Player() = default;
Player::~Player() { close(); }

void Player::open(const Options& options) {
    close();
    opts_ = options;
    set_volume(options.volume_percent);
    if (!std::isfinite(opts_.start)) opts_.start = 0;
    opts_.start = std::max(0.0, opts_.start);
    if (!std::isfinite(opts_.progressive_resume_limit)) opts_.progressive_resume_limit = 0;
    opts_.progressive_resume_limit = std::max(0.0, opts_.progressive_resume_limit);
    if (progressive_download()) opts_.start = std::min(opts_.start, opts_.progressive_resume_limit);
    duration_ = 0; paused_ = false; too_heavy_ = false; audio_stream_ = -1;
    hardware_failed = false;
    vlc_ = std::make_unique<Vlc>(*this);
    state_ = State::Opening;
    vlc_->worker = std::thread([this] { vlc_->run(); });
}

void Player::close() {
    if (vlc_) { vlc_->stop(); vlc_.reset(); }
    state_ = State::Idle;
    opts_.growing_download.reset();
}

std::string Player::error() const {
    if (!vlc_) return {};
    std::lock_guard lock(vlc_->error_mutex);
    return vlc_->error;
}

void Player::set_paused(bool paused) {
    paused_ = paused;
    if (vlc_) {
        {
            std::lock_guard timing(vlc_->cadence_mutex);
            vlc_->callback_cadence.previous = vlc_->pickup_cadence.previous = 0;
        }
        vlc_->set_clock_running(!paused && vlc_->native_playing && vlc_->buffering_percent >= 100);
        if (!paused) vlc_->last_progress_clock = libvlc_clock();
    }
}

bool Player::buffering(int* percent) const {
    if (vlc_ && (vlc_->decoder_error || vlc_->recovering_decoder) &&
        (state_ == State::Opening || state_ == State::Playing)) {
        if (percent) *percent = 0;
        return true;
    }
    const bool complete = vlc_ && (vlc_->local || (opts_.growing_download && !progressive_download()));
    // VLC may redisplay its last picture while an input callback is waiting.
    // Those repeated displays are not evidence of new playback progress.
    const int64_t last = vlc_ ? vlc_->last_progress_clock.load() : 0;
    const bool stalled = vlc_ && last > 0 && !paused_ && !vlc_->end_reached &&
        libvlc_clock() - last > vlc_->picture_gap_limit;
    const int cache = stalled ? 0 : vlc_ ? vlc_->buffering_percent.load() : 0;
    if (percent) *percent = complete ? 100 : cache;
    return !complete && (state_ == State::Opening || (state_ == State::Playing && cache < 100));
}

bool Player::video_expected() const { return !vlc_ || vlc_->expects_video.load(); }

double Player::position() { return vlc_ ? vlc_->presented.load() : 0; }

bool Player::progressive_download() const {
    if (!opts_.growing_download) return false;
    const auto& source = *opts_.growing_download;
    if (!source.state || source.total <= 0) return true;
    const auto status = source.state->snapshot();
    return status.generation != source.generation || status.total != source.total ||
        status.phase != GrowingFilePhase::Complete || status.available < source.total;
}

double Player::seekable_until() const {
    if (!progressive_download()) return std::max(0.0, duration_.load());
    if (!vlc_ || !opts_.growing_download->state) return 0;
    const auto& source = *opts_.growing_download;
    const auto status = source.state->snapshot();
    if (status.generation != source.generation || status.total != source.total || status.phase == GrowingFilePhase::Removed) return 0;
    const double known = std::max(0.0, vlc_->known_seek.load());
    return duration_ > 0 ? std::min(known, duration_.load()) : known;
}

void Player::seek(double time) {
    if (!vlc_ || !std::isfinite(time)) return;
    const double requested = time;
    time = std::max(0.0, time);
    if (duration_ > 0) time = std::min(time, std::max(0.0, duration_.load() - 0.1));
    const bool accepted = !progressive_download() || time <= seekable_until();
    // Fixed numeric fields are emitted only for requested seek actions, not
    // playback ticks. No media identifiers or input locations enter the log.
    dlog("player/vlc: seek_request current_s=%.9g requested_s=%.9g target_s=%.9g accepted=%d",
        vlc_->presented.load(), requested, time, int(accepted));
    if (!accepted) return;
    vlc_->requested_seek = time;
}

std::vector<Player::Track> Player::audio_tracks() const {
    if (!vlc_) return {};
    std::lock_guard lock(vlc_->info_mutex);
    return vlc_->audio_tracks;
}
std::vector<Player::Track> Player::subtitle_tracks() const {
    if (!vlc_) return {};
    std::lock_guard lock(vlc_->info_mutex);
    return vlc_->subtitle_tracks;
}
void Player::select_audio(int stream) { if (vlc_) vlc_->wanted_audio = stream; }
void Player::select_subtitle(int stream) {
    if (!vlc_ || vlc_->wanted_subtitle == stream) return;
    std::lock_guard lock(vlc_->subtitle_mutex);
    if (vlc_->wanted_subtitle.exchange(stream) == stream) return;
    ++vlc_->subtitle_serial;
    vlc_->subtitles.clear();
    vlc_->ephemeral_subtitle.clear();
}
std::string Player::embedded_subtitle(int stream, double time) {
    if (!vlc_) return {};
    std::lock_guard lock(vlc_->subtitle_mutex);
    const auto found = vlc_->subtitles.find(stream);
    return found == vlc_->subtitles.end() ? std::string() : cues_at(found->second, time);
}

bool Player::take_frame(VideoFrame& output) {
    if (!vlc_ || vlc_->decoder_error || state_ == State::Failed || state_ == State::Idle) return false;
    std::shared_ptr<Vlc::Session> session;
    { std::lock_guard lock(vlc_->frames_mutex); session = vlc_->current_session; }
    if (!session) return false;
    std::lock_guard lock(session->mutex);
    const int index = session->shown;
    if (index < 0) return false;
    auto& slot = session->slots[index];
    session->shown = -1;
    slot.phase = Vlc::Phase::Free;
    if (slot.frame.serial != vlc_->serial) return false;
    output = slot.frame;
    vlc_->presented = output.pts;
    vlc_->frame_presented = true;
    vlc_->recovering_decoder = false;
    const double known = vlc_->known_seek;
    if (output.pts > known) vlc_->known_seek = output.pts;
    ++vlc_->shown;
    if (!paused_) {
        std::lock_guard timing(vlc_->cadence_mutex);
        vlc_->pickup_cadence.sample(libvlc_clock(), output.presentation_time, output.serial);
    }
    return true;
}

std::string Player::stats() {
    if (!vlc_) return {};
    std::lock_guard lock(vlc_->info_mutex);
    return vlc_->telemetry;
}
std::string Player::codec_summary() const {
    if (!vlc_) return {};
    std::lock_guard lock(vlc_->info_mutex);
    return vlc_->description;
}
void Player::log_stats() {
    if (!vlc_ || (state_ != State::Opening && state_ != State::Playing)) return;
    const double now = now_seconds();
    if (!vlc_->last_log) { vlc_->last_log = now; vlc_->last_shown = vlc_->shown; vlc_->last_dropped = vlc_->dropped; return; }
    if (now - vlc_->last_log < (state_ == State::Opening ? 2.0 : 30.0)) return;
    const uint64_t shown = vlc_->shown, dropped = vlc_->dropped;
    dlog("player/vlc: %s | presented %.2f fps, output drops %llu, position %.3fs, buffer %d%%%s",
        codec_summary().c_str(), double(shown - vlc_->last_shown) / (now - vlc_->last_log),
        (unsigned long long)(dropped - vlc_->last_dropped), position(), vlc_->buffering_percent.load(), paused_ ? ", paused" : "");
    if (state_ == State::Opening)
        dlog("player/vlc: startup video_expected=%d playing=%d setup=%llu lock=%llu display=%llu picture=%d audio=%.3fs",
            int(vlc_->expects_video.load()), int(vlc_->native_playing.load()),
            (unsigned long long)vlc_->setup_calls.load(), (unsigned long long)vlc_->lock_calls.load(),
            (unsigned long long)vlc_->display_calls.load(), int(vlc_->opened_video.load()),
            double(vlc_->rendered_audio_frames.load()) / VlcAudioTimeline::rate);
    vlc_->last_log = now; vlc_->last_shown = shown; vlc_->last_dropped = dropped;
    dlog("player/vlc: audio_route=%s passthrough=%d", vlc_->audio_mode_status.load(), int(vlc_->passthrough_active.load()));
    double source_fps = 0;
    { std::lock_guard lock(vlc_->info_mutex); source_fps = vlc_->fps; }
    const char* decoder = hardware_decoders.load() > 0 ? "ps5-hardware" :
        vlc_->opened_video.load() ? "software" : "pending";
    dlog("player/vlc: decoder=%s hardware_available=%d software_fallback=%d source_fps=%.6f "
         "decoded=%d vout_lost_pictures=%d vmem_drops=%llu",
         decoder, int(HwDecoder::available()), int(vlc_->software_fallback.load()), source_fps,
         vlc_->decoded_pictures.load(), vlc_->late_pictures.load(), (unsigned long long)dropped);
    Vlc::Cadence callback, pickup;
    uint64_t repeated = 0;
    {
        std::lock_guard timing(vlc_->cadence_mutex);
        callback = vlc_->callback_cadence; pickup = vlc_->pickup_cadence;
        repeated = vlc_->repeated_source_stamps;
        vlc_->callback_cadence.clear_window(); vlc_->pickup_cadence.clear_window();
        vlc_->repeated_source_stamps = 0;
    }
    char callback_text[256], pickup_text[256];
    callback.intervals.format(callback_text, sizeof(callback_text));
    pickup.intervals.format(pickup_text, sizeof(pickup_text));
    dlog("player/vlc cadence: callback {%s} pickup {%s} callback_late_mean_ms=%.3f callback_late_max_ms=%.3f "
         "pickup_late_mean_ms=%.3f pickup_late_max_ms=%.3f repeated_source_timestamps=%llu",
         callback_text, pickup_text, callback.lateness.mean_ms(), callback.lateness.max_ms(),
         pickup.lateness.mean_ms(), pickup.lateness.max_ms(), (unsigned long long)repeated);
}

extern "C" int vlc_ps5_vdec_available() { return HwDecoder::available() ? 1 : 0; }
extern "C" void vlc_ps5_vdec_failed() { hardware_failed = true; }
extern "C" void vlc_ps5_vdec_active(int active) { hardware_decoders.fetch_add(active ? 1 : -1); }

std::string Player::audio_output_status() const {
    return vlc_ ? vlc_->audio_mode_status.load() : "pcm";
}

bool Player::audio_passthrough_active() const {
    return vlc_ && vlc_->passthrough_active.load();
}
