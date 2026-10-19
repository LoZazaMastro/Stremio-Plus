// Exercise the production VLC audio module with real VLC objects and a mock HDMI sink.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include <vlc_common.h>
#include <vlc/vlc.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include "../src/player_vlc_passthrough.cpp"

extern "C" {
int (*vlc_static_modules[])(vlc_set_cb, void*) = {vlc_entry__stremiops5audio, nullptr};
}

namespace {
unsigned checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Sink {
    int monitor_result = 0, open_result = 7, configure_result = 0;
    int drain_result = 0, close_result = 0, reset_result = 0;
    uint32_t count_override = 0;
    bool override_count = false, configured = false, fail_payload = false, capture = false;
    unsigned monitor_calls = 0, open_calls = 0, close_calls = 0, output_calls = 0;
    unsigned reset_calls = 0, retry_calls = 0, pcm_calls = 0, pcm_frames = 0;
    unsigned pause_calls = 0, resume_calls = 0, flush_calls = 0, drain_calls = 0;
    int mode = -1;
    bool deferred = false;
    const uint8_t* in_flight = nullptr;
    std::vector<uint8_t> in_flight_copy;
    unsigned overwritten_buffers = 0, undrained_closes = 0, native_drains = 0;
    std::chrono::steady_clock::time_point reset_stamp{};
    int64_t reset_to_open_ms = -1;
    std::vector<int> formats{2, 7, 10};
    std::vector<uint8_t> data;
    std::vector<std::pair<bool, std::string>> events;
};
Sink* sink = nullptr;

void pcm(void*, const void*, unsigned frames, int64_t) { ++sink->pcm_calls; sink->pcm_frames += frames; }
void paused(void*, int64_t) { ++sink->pause_calls; }
void resumed(void*, int64_t) { ++sink->resume_calls; }
void flushed(void*, int64_t) { ++sink->flush_calls; }
void drained(void*) { ++sink->drain_calls; }
void changed(void*, bool active, const char* reason) {
    sink->events.emplace_back(active, reason);
    if (std::string(reason) == "output_failed") ++sink->retry_calls;
}

class Fixture {
public:
    explicit Fixture(libvlc_instance_t* instance) {
        sink = &state;
        player = libvlc_media_player_new(instance);
        check(player != nullptr, "create real libVLC media player");
        libvlc_audio_set_callbacks(player, pcm, paused, resumed, flushed, drained, &state);
        auto* parent = reinterpret_cast<vlc_object_t*>(player);
        var_Create(parent, "stremio-audio-mode", VLC_VAR_ADDRESS);
        var_SetAddress(parent, "stremio-audio-mode", reinterpret_cast<void*>(changed));
        var_Create(parent, "stremio-audio-passthrough", VLC_VAR_BOOL);
        var_SetBool(parent, "stremio-audio-passthrough", true);
        output = static_cast<audio_output_t*>(vlc_object_create(parent, sizeof(audio_output_t)));
        check(output != nullptr, "create output parented to media player");
        check(open_output(VLC_OBJECT(output)) == VLC_SUCCESS, "inherit real memory-audio callbacks");
    }
    ~Fixture() {
        state.capture = false;
        close_output(VLC_OBJECT(output));
        vlc_object_release(output);
        libvlc_media_player_release(player);
        sink = nullptr;
    }
    int encoded(vlc_fourcc_t codec = VLC_CODEC_A52, unsigned rate = 48000) {
        audio_sample_format_t format{};
        format.i_format = codec;
        format.i_rate = rate;
        return output->start(output, &format);
    }
    void verify_pcm() {
        audio_sample_format_t format{};
        format.i_format = VLC_CODEC_FL32;
        format.i_rate = 44100;
        format.i_physical_channels = AOUT_CHANS_5_1;
        check(output->start(output, &format) == VLC_SUCCESS, "fallback accepts decoded PCM");
        check(format.i_format == VLC_CODEC_S16N && format.i_rate == 48000 &&
              format.i_physical_channels == AOUT_CHANS_2_0, "PCM keeps existing stereo SDL contract");
        const unsigned before = state.output_calls;
        auto* block = block_Alloc(128 * 4);
        check(block != nullptr, "allocate PCM block");
        block->i_nb_samples = 128;
        block->i_pts = VLC_TICK_INVALID;
        output->play(output, block);
        check(state.pcm_calls == 1 && state.pcm_frames == 128, "fallback delivers PCM through inherited callback");
        check(state.output_calls == before, "decoded PCM never reaches encoded HDMI transport");
        output->pause(output, true, 1000);
        output->pause(output, false, 2000);
        output->flush(output, false);
        output->flush(output, true);
        check(state.pause_calls == 1 && state.resume_calls == 1 && state.flush_calls == 1 &&
              state.drain_calls == 1, "fallback preserves pause, resume, flush and drain callbacks");
    }
    void send(const std::vector<uint8_t>& bytes) {
        auto* block = block_Alloc(bytes.size());
        check(block != nullptr, "allocate encoded packet");
        std::memcpy(block->p_buffer, bytes.data(), bytes.size());
        block->i_pts = VLC_TICK_INVALID;
        output->play(output, block);
    }
    Sink state;
    libvlc_media_player_t* player = nullptr;
    audio_output_t* output = nullptr;
};

std::vector<uint8_t> frame(pt::Codec codec) {
    std::vector<uint8_t> bytes(128, 0x5a);
    if (codec == pt::Codec::dts) {
        const uint8_t header[] = {0x7f, 0xfe, 0x80, 0x01, 0x00, 0x3c, 0x07, 0xf0};
        std::copy(std::begin(header), std::end(header), bytes.begin());
    } else {
        bytes[0] = 0x0b; bytes[1] = 0x77; bytes[2] = 0; bytes[3] = 0;
        bytes[4] = 0; bytes[5] = 0x40;
        if (codec == pt::Codec::eac3) { bytes[3] = 0x3f; bytes[4] = 0x30; bytes[5] = 0x80; }
    }
    return bytes;
}
void negotiation_failures(libvlc_instance_t* instance) {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        Fixture fixture(instance);
        auto& state = fixture.state;
        if (scenario == 0) state.monitor_result = -1;
        if (scenario == 1) state.formats.clear();
        if (scenario == 2) state.formats = {1};
        if (scenario == 3) { state.override_count = true; state.count_override = 100; }
        if (scenario == 4) state.open_result = -1;
        if (scenario == 5) state.configure_result = -1;
        if (scenario == 6) var_SetBool(reinterpret_cast<vlc_object_t*>(fixture.player), "stremio-audio-passthrough", false);
        check(fixture.encoded() != VLC_SUCCESS, "unsafe sink or disabled option rejects encoded output");
        check(state.output_calls == 0, "rejected negotiation sends no IEC61937 bytes");
        if (scenario < 4 || scenario == 6) check(state.open_calls == 0, "unsupported EDID cannot open encoded port");
        if (scenario == 5) check(state.close_calls == 1 && state.reset_calls == 1,
                                 "failed configuration closes owned port and restores PCM mode");
        fixture.verify_pcm();
    }
    for (auto codec : {VLC_CODEC_TRUEHD, VLC_CODEC_OPUS}) {
        Fixture fixture(instance);
        check(fixture.encoded(codec) != VLC_SUCCESS, "unsupported codec stays decoded");
        check(fixture.state.monitor_calls == 0 && fixture.state.open_calls == 0,
              "unsupported codec cannot negotiate encoded transport");
        fixture.verify_pcm();
    }
    Fixture fixture(instance);
    check(fixture.encoded(VLC_CODEC_A52, 44100) != VLC_SUCCESS, "non-48kHz stream cannot use fixed-rate carrier");
    check(fixture.state.open_calls == 0, "wrong sample rate never opens encoded port");
    fixture.verify_pcm();
}
void carriers(libvlc_instance_t* instance) {
    struct Case { pt::Codec codec; vlc_fourcc_t fourcc; int mode; size_t bytes; unsigned type; };
    for (const auto& test : {Case{pt::Codec::ac3, VLC_CODEC_A52, 0, 6144, 1},
                            Case{pt::Codec::eac3, VLC_CODEC_EAC3, 3, 24576, 0x15},
                            Case{pt::Codec::dts, VLC_CODEC_DTS, 2, 2048, 0x0b}}) {
        Fixture fixture(instance);
        auto& state = fixture.state;
        check(fixture.encoded(test.fourcc) == VLC_SUCCESS, "advertised codec starts native audio path");
        check(state.mode == test.mode && state.configured, "correct HDMI carrier is negotiated");
        check(state.events.front() == std::make_pair(true, std::string("opening")),
              "PCM producer is stopped before native mode activation");
        state.capture = true;
        const auto payload = frame(test.codec);
        fixture.send(payload);
        check(state.data.size() == test.bytes, "production output sends one complete carrier burst");
        check(state.data[0] == 0x72 && state.data[1] == 0xf8 && state.data[2] == 0x1f &&
              state.data[3] == 0x4e && state.data[4] == test.type, "carrier preserves IEC61937 preamble and codec type");
        check(state.data[8] == payload[1] && state.data[9] == payload[0], "carrier transports the selected compressed frame");
        check(state.pcm_calls == 0, "active passthrough never duplicates playback through PCM");
        state.capture = false;
        fixture.output->stop(fixture.output);
        check(state.close_calls == 1 && state.reset_calls == 1 && !state.configured,
              "stopping restores PCM and releases exactly the owned native port");
        check(state.events.back() == std::make_pair(false, std::string("pcm")),
              "restored PCM mode is reported to the player");
        fixture.verify_pcm();
    }
}
void output_failure(libvlc_instance_t* instance) {
    Fixture fixture(instance);
    auto& state = fixture.state;
    check(fixture.encoded() == VLC_SUCCESS, "start transport for runtime failure case");
    state.fail_payload = true;
    fixture.send(frame(pt::Codec::ac3));
    check(state.retry_calls == 1 && fixture.output->sys->failed,
          "runtime output failure requests one controlled PCM retry");
    const unsigned before = state.output_calls;
    fixture.send(frame(pt::Codec::ac3));
    check(state.output_calls == before && state.retry_calls == 1,
          "failed output cannot spin or repeatedly request a restart");
    check(state.pcm_calls == 0, "retry does not reinterpret compressed input as PCM");
    state.fail_payload = false;
    fixture.output->stop(fixture.output);
    check(state.close_calls == 1 && state.reset_calls == 1, "controlled retry closes and resets native output");
    fixture.verify_pcm();
}
void deferred_output_lifetime(libvlc_instance_t* instance) {
    Fixture fixture(instance);
    auto& state = fixture.state;
    check(fixture.encoded(VLC_CODEC_DTS) == VLC_SUCCESS, "start transport for deferred native consumption");
    state.deferred = true;
    auto bytes = frame(pt::Codec::dts);
    bytes.resize(2040, 0x5a);
    bytes[6] = 0x7f; bytes[7] = 0x70;
    fixture.send(bytes);
    std::fill(bytes.begin() + 10, bytes.end(), 0x6b);
    fixture.send(bytes);
    check(state.overwritten_buffers == 0,
          "submitted native grain remains unchanged until the following output returns");
    fixture.output->stop(fixture.output);
    check(state.native_drains == 1 && state.undrained_closes == 0,
          "native output is explicitly drained before its port closes");
}
void deferred_null_lifetime(libvlc_instance_t* instance) {
    Fixture fixture(instance);
    auto& state = fixture.state;
    state.deferred = true;
    check(fixture.encoded() == VLC_SUCCESS, "start AC-3 with deferred native consumption");
    check(state.in_flight != nullptr &&
          std::memcmp(state.in_flight, state.in_flight_copy.data(), state.in_flight_copy.size()) == 0,
          "lead-in null grain remains alive after start returns");
    fixture.send(frame(pt::Codec::ac3));
    fixture.output->pause(fixture.output, true, 1000);
    check(state.in_flight == nullptr && state.native_drains == 1,
          "pause drains the last native grain before resetting timestamps");
    fixture.output->pause(fixture.output, false, 2000);
    fixture.send(frame(pt::Codec::ac3));
    fixture.output->flush(fixture.output, false);
    check(state.in_flight == nullptr && state.native_drains == 2,
          "seek flush drains native data before the next timeline starts");
    fixture.send(frame(pt::Codec::ac3));
    fixture.output->stop(fixture.output);
    check(state.native_drains == 3 && state.undrained_closes == 0 && state.overwritten_buffers == 0,
          "lead-out remains alive through native drain and close");
}
void rapid_restart(libvlc_instance_t* instance) {
    Fixture fixture(instance);
    auto& state = fixture.state;
    state.deferred = true;
    check(fixture.encoded() == VLC_SUCCESS, "start transport before a rapid track restart");
    check(!stremio_ps5_audio_pcm_available(), "encoded port reserves native output before PCM can reopen");
    fixture.send(frame(pt::Codec::ac3));
    fixture.output->stop(fixture.output);
    check(stremio_ps5_audio_pcm_available(), "confirmed native shutdown releases the PCM gate");
    check(fixture.encoded(VLC_CODEC_EAC3) == VLC_SUCCESS, "reopen for another encoded track");
    check(state.reset_to_open_ms >= 1000,
          "new track waits a full second after the previous HDMI reset");
    fixture.send(frame(pt::Codec::eac3));
    fixture.output->stop(fixture.output);
    check(state.native_drains == 2 && !state.undrained_closes && !state.overwritten_buffers,
          "rapid track changes drain the exact live owner without stale native grains");
}
void failed_shutdown_retains_owner(libvlc_instance_t* instance) {
    for (int stage = 0; stage < 4; ++stage) {
        aout_sys_t* retained = nullptr;
        {
            Fixture fixture(instance);
            auto& state = fixture.state;
            state.deferred = true;
            if (stage == 3) {
                state.configure_result = -104;
                state.close_result = -102;
                check(fixture.encoded() != VLC_SUCCESS, "reject an unconfigured port when cleanup also fails");
            } else {
                check(fixture.encoded() == VLC_SUCCESS, "start transport for shutdown fault injection");
                fixture.send(frame(pt::Codec::ac3));
                if (stage == 0) state.drain_result = -101;
                if (stage == 1) state.close_result = -102;
                if (stage == 2) state.reset_result = -103;
                fixture.output->stop(fixture.output);
            }
            retained = fixture.output->sys;
            check(retained->shutdown_failed && !stremio_ps5_audio_pcm_available(),
                  "failed native shutdown latches the PCM gate closed");
            check(retained->first_native_error == -(101 + stage),
                  "shutdown preserves the original native error code");
            check(hdmi_owner == retained, "failed native shutdown keeps the only owner alive");
            check(state.events.back() == std::make_pair(true, std::string("native_shutdown_failed")),
                  "unconfirmed shutdown never reports successful PCM restoration");
            if (stage == 0) {
                check(state.close_calls == 0 && state.reset_calls == 0,
                      "failed drain does not close or reconfigure an in-flight port");
                check(state.in_flight != nullptr &&
                      std::memcmp(state.in_flight, state.in_flight_copy.data(), state.in_flight_copy.size()) == 0,
                      "failed drain retains the exact submitted grain bytes");
            } else if (stage != 2) check(state.reset_calls == 0,
                                       "failed close cannot reconfigure HDMI behind an owned port");
            const unsigned outputs = state.output_calls, closes = state.close_calls, resets = state.reset_calls;
            fixture.output->stop(fixture.output);
            check(state.output_calls == outputs && state.close_calls == closes && state.reset_calls == resets,
                  "repeated stop does not retry a failed native shutdown");
            {
                Fixture next(instance);
                check(next.encoded() != VLC_SUCCESS && next.state.open_calls == 0,
                      "another output cannot replace the quarantined native owner");
            }
            sink = &state;
        }
        check(hdmi_owner == retained && retained->shutdown_failed,
              "native buffers survive release of the VLC output object after a shutdown error");
        // This test-only reset simulates terminating the mock process. No
        // production path releases a quarantined native port or its buffers.
        std::lock_guard lock(hdmi_lifecycle_mutex);
        delete hdmi_owner;
        hdmi_owner = nullptr;
        pcm_available = true;
    }
}
}

extern "C" int sceAudioOutSysGetHdmiMonitorInfo(int port, void* data, unsigned size) {
    check(sink != nullptr && port == 1 && size == 0x180, "HDMI monitor ABI uses bounded advertised buffer");
    ++sink->monitor_calls;
    if (sink->monitor_result < 0) return sink->monitor_result;
    std::memset(data, 0, size);
    const uint32_t count = sink->override_count ? sink->count_override : sink->formats.size();
    std::memcpy(static_cast<uint8_t*>(data) + 0x94, &count, sizeof(count));
    for (size_t i = 0; i < sink->formats.size(); ++i)
        static_cast<uint8_t*>(data)[0x98 + i * 8] = sink->formats[i];
    return 0;
}
extern "C" int sceAudioOutExOpen(int user, int mode) {
    check(user == 0xff, "native audio uses expected system-user sentinel");
    ++sink->open_calls; sink->mode = mode;
    if (sink->reset_stamp != std::chrono::steady_clock::time_point{})
        sink->reset_to_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - sink->reset_stamp).count();
    return sink->open_result;
}
extern "C" int sceAudioOutExConfigureOutput(int first, unsigned second, int mode, int last, uint64_t reserved) {
    check(first == 0 && second == 0 && reserved == 0, "native mode ABI retains reserved fields");
    if (mode == 0xff) {
        ++sink->reset_calls;
        if (sink->reset_result < 0) return sink->reset_result;
        sink->configured = false;
        sink->reset_stamp = std::chrono::steady_clock::now();
        return 0;
    }
    check(last == 1 && mode == sink->mode, "configured carrier matches opened mode");
    sink->configured = sink->configure_result >= 0;
    return sink->configure_result;
}
extern "C" int sceAudioOutExClose(int handle) {
    check(handle == sink->open_result && handle > 0, "close only the handle owned by this output");
    ++sink->close_calls;
    if (sink->close_result < 0) return sink->close_result;
    if (sink->in_flight) ++sink->undrained_closes;
    sink->in_flight = nullptr;
    return 0;
}
extern "C" int sceAudioOutOutput(int handle, const void* data) {
    ++sink->output_calls;
    check(handle == sink->open_result && sink->configured, "no encoded output before successful negotiation");
    if (!data) {
        ++sink->native_drains;
        if (sink->drain_result < 0) return sink->drain_result;
    }
    if (sink->in_flight) {
        if (std::memcmp(sink->in_flight, sink->in_flight_copy.data(), sink->in_flight_copy.size()) != 0)
            ++sink->overwritten_buffers;
        sink->in_flight = nullptr;
    }
    if (!data) return 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const bool payload = bytes[0] == 0x72 && bytes[1] == 0xf8 && (bytes[4] || bytes[5]);
    if (sink->fail_payload && payload) return -1;
    if (sink->capture) {
        const size_t count = sink->mode == 3 ? 4096 : 1024;
        sink->data.insert(sink->data.end(), bytes, bytes + count);
    }
    if (sink->deferred) {
        const size_t count = sink->mode == 3 ? 4096 : 1024;
        sink->in_flight = bytes;
        sink->in_flight_copy.assign(bytes, bytes + count);
    }
    return 0;
}

int main() {
    const char* arguments[] = {"--quiet", "--no-video", "--no-audio"};
    auto* instance = libvlc_new(3, arguments);
    check(instance != nullptr, "initialize real host libVLC core");
    deferred_null_lifetime(instance);
    deferred_output_lifetime(instance);
    rapid_restart(instance);
    negotiation_failures(instance);
    carriers(instance);
    output_failure(instance);
    failed_shutdown_retains_owner(instance);
    libvlc_release(instance);
    std::printf("{\"result\":\"PASS\",\"checks\":%u,\"scenarios\":21}\n", checks);
}
