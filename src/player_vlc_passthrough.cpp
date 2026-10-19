// PS5 HDMI passthrough adapted from EVO Player by sainsaji and contributors.
// VLC output integration and bounded per-stream ownership added 2026-10-19.
// SPDX-License-Identifier: GPL-3.0-or-later
#define MODULE_NAME stremiops5audio
#define MODULE_STRING "stremiops5audio"
#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_variables.h>
#include "player_iec61937.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

extern "C" {
int sceAudioOutOutput(int, const void*);
int sceAudioOutExOpen(int, int);
int sceAudioOutExClose(int);
int sceAudioOutExConfigureOutput(int, unsigned, int, int, uint64_t);
int sceAudioOutSysGetHdmiMonitorInfo(int, void*, unsigned);
}

struct aout_sys_t {
    void* opaque = nullptr;
    void (*pcm)(void*, const void*, unsigned, int64_t) = nullptr;
    void (*pause)(void*, int64_t) = nullptr;
    void (*resume)(void*, int64_t) = nullptr;
    void (*flush)(void*, int64_t) = nullptr;
    void (*drain)(void*) = nullptr;
    void (*volume)(void*, float, bool) = nullptr;
    void (*mode)(void*, bool, const char*) = nullptr;
    pt::Codec codec = pt::Codec::unknown;
    pt::Carrier carrier;
    pt::Packer packer{pt::Codec::unknown};
    std::vector<uint8_t> pending, burst;
    // The last submitted grain may remain queued after Output returns. Keep
    // it separate from the packer's reusable vector until the next submission
    // has returned, and retain both buffers until an explicit native drain.
    alignas(64) std::array<std::array<uint8_t, 4096>, 2> native_grains{};
    unsigned next_grain = 0;
    int handle = -1;
    int first_native_error = 0;
    bool failed = false, muted = false, configured = false, submitted = false;
    bool shutdown_failed = false;
    float gain = 1.f;
    int64_t next_pts = VLC_TICK_INVALID;
};

namespace {
constexpr size_t maximum_pending = 256 * 1024;
std::mutex hdmi_lifecycle_mutex;
aout_sys_t* hdmi_owner = nullptr;
std::atomic<bool> pcm_available{true};
std::chrono::steady_clock::time_point last_hdmi_reset{};

pt::Codec codec_for(vlc_fourcc_t codec) {
    switch (codec) {
        case VLC_CODEC_A52: return pt::Codec::ac3;
        case VLC_CODEC_EAC3: return pt::Codec::eac3;
        case VLC_CODEC_DTS: return pt::Codec::dts;
        default: return pt::Codec::unknown;
    }
}
const char* active_status(pt::Codec codec) {
    switch (codec) {
        case pt::Codec::ac3: return "active_ac3";
        case pt::Codec::eac3: return "active_eac3";
        case pt::Codec::dts: return "active_dts_core";
        default: return "pcm";
    }
}
bool sink_supports(pt::Codec codec) {
    const int coding = codec == pt::Codec::ac3 ? 2 : codec == pt::Codec::eac3 ? 10 :
        codec == pt::Codec::dts ? 7 : 0;
    if (!coding) return false;
    std::array<uint8_t, 0x180> information{};
    if (sceAudioOutSysGetHdmiMonitorInfo(1, information.data(), unsigned(information.size())) < 0) return false;
    uint32_t count = 0;
    std::memcpy(&count, information.data() + 0x94, sizeof(count));
    // Unknown or malformed EDID remains PCM. Encoded bursts must never be
    // sent to a receiver that has not advertised this transport format.
    if (count > (information.size() - 0x98) / 8) return false;
    for (uint32_t i = 0; i < count; ++i) if (information[0x98 + i * 8] == coding) return true;
    return false;
}
void notify(aout_sys_t& state, bool active, const char* reason) {
    if (state.mode) state.mode(state.opaque, active, reason);
}
void native_error(audio_output_t* output, const char* operation, int result) {
    auto& state = *output->sys;
    if (!state.first_native_error) state.first_native_error = result;
    msg_Err(output, "PS5 HDMI %s failed: 0x%08x (first error 0x%08x)",
            operation, unsigned(result), unsigned(state.first_native_error));
}
int submit_grain(audio_output_t* output, const uint8_t* bytes) {
    auto& state = *output->sys;
    auto& grain = state.native_grains[state.next_grain];
    const size_t count = size_t(state.carrier.grain_frames) * state.carrier.frame_bytes;
    if (!count || count > grain.size()) return VLC_EGENERIC;
    if (bytes) std::memcpy(grain.data(), bytes, count);
    else {
        grain.fill(0);
        grain[0] = 0x72; grain[1] = 0xf8; grain[2] = 0x1f; grain[3] = 0x4e;
    }
    state.submitted = true;
    const int result = sceAudioOutOutput(state.handle, grain.data());
    if (result < 0) native_error(output, "output", result);
    else state.next_grain ^= 1;
    return result;
}
int null_bursts(audio_output_t* output, int milliseconds) {
    auto& state = *output->sys;
    if (state.handle < 1 || !state.configured) return 0;
    const int count = milliseconds * state.carrier.frame_rate() / 1000 / state.carrier.grain_frames;
    for (int i = 0; i < count; ++i) {
        const int result = submit_grain(output, nullptr);
        if (result < 0) return result;
    }
    return 0;
}
bool shutdown_failure(audio_output_t* output, const char* operation, int result) {
    auto& state = *output->sys;
    native_error(output, operation, result);
    state.failed = state.shutdown_failed = true;
    // Keep the sole native owner and its submitted buffers alive. Do not
    // switch HDMI or reopen PCM after an unconfirmed native shutdown.
    pcm_available.store(false, std::memory_order_release);
    notify(state, true, "native_shutdown_failed");
    return false;
}
bool drain_port_locked(audio_output_t* output) {
    auto& state = *output->sys;
    if (state.shutdown_failed) return false;
    if (state.handle < 1 || !state.submitted) return true;
    const int result = sceAudioOutOutput(state.handle, nullptr);
    if (result < 0) return shutdown_failure(output, "drain", result);
    state.submitted = false;
    state.next_grain = 0;
    return true;
}
bool drain_port(audio_output_t* output) {
    std::lock_guard lock(hdmi_lifecycle_mutex);
    return drain_port_locked(output);
}
bool close_port_locked(audio_output_t* output) {
    auto& state = *output->sys;
    if (state.shutdown_failed) return false;
    if (state.handle < 1) return true;
    if (!state.failed && null_bursts(output, 500) < 0) state.failed = true;
    if (!drain_port_locked(output)) return false;
    const int closed = sceAudioOutExClose(state.handle);
    if (closed < 0) return shutdown_failure(output, "close", closed);
    state.handle = -1;
    state.configured = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int reset = sceAudioOutExConfigureOutput(0, 0, 0xff, 0xff, 0);
    if (reset < 0) return shutdown_failure(output, "PCM restore", reset);
    last_hdmi_reset = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    state.pending.clear(); state.burst.clear(); state.next_pts = VLC_TICK_INVALID;
    hdmi_owner = nullptr;
    pcm_available.store(true, std::memory_order_release);
    notify(state, false, "pcm");
    return true;
}
bool close_port(audio_output_t* output) {
    std::lock_guard lock(hdmi_lifecycle_mutex);
    return close_port_locked(output);
}
void stop(audio_output_t* output) {
    auto& state = *output->sys;
    if (!close_port(output)) return;
    state.pending.clear(); state.burst.clear(); state.next_pts = VLC_TICK_INVALID;
}
int start(audio_output_t* output, audio_sample_format_t* format) {
    auto& state = *output->sys;
    std::unique_lock lifecycle(hdmi_lifecycle_mutex);
    if (hdmi_owner) {
        notify(state, true, hdmi_owner->shutdown_failed ? "native_shutdown_failed" : "native_output_busy");
        return VLC_EGENERIC;
    }
    const auto codec = codec_for(format->i_format);
    if (!AOUT_FMT_LINEAR(format)) {
        if (codec == pt::Codec::unknown || format->i_rate != 48000 ||
            !var_InheritBool(output, "stremio-audio-passthrough")) return VLC_EGENERIC;
        if (!sink_supports(codec)) {
            notify(state, false, "pcm_sink_unsupported");
            return VLC_EGENERIC;
        }
        state.codec = codec; state.carrier = pt::carrier_for(codec); state.packer.set_codec(codec);
        state.failed = false; state.first_native_error = 0; state.next_pts = VLC_TICK_INVALID;
        try { state.pending.reserve(maximum_pending); state.burst.reserve(24576); }
        catch (...) { notify(state, false, "pcm_allocation_failed"); return VLC_ENOMEM; }
        // Closing the ordinary SDL port happens through this callback before
        // HDMI switches mode, so a second PCM producer cannot overwrite it.
        hdmi_owner = &state;
        pcm_available.store(false, std::memory_order_release);
        notify(state, true, "opening");
        // Match EVO's reset-to-open settling interval across tracks and
        // output instances; the HDMI mode belongs to the whole process.
        if (last_hdmi_reset != std::chrono::steady_clock::time_point{})
            std::this_thread::sleep_until(last_hdmi_reset + std::chrono::milliseconds(1000));
        state.handle = sceAudioOutExOpen(0xff, state.carrier.mode);
        if (state.handle < 1) {
            native_error(output, "open", state.handle);
            hdmi_owner = nullptr;
            pcm_available.store(true, std::memory_order_release);
            notify(state, false, "pcm_port_unavailable"); return VLC_EGENERIC;
        }
        const int result = sceAudioOutExConfigureOutput(0, 0, state.carrier.mode, 1, 0);
        if (result < 0) {
            native_error(output, "configuration", result);
            if (close_port_locked(output)) notify(state, false, "pcm_configuration_failed");
            return VLC_EGENERIC;
        }
        state.configured = true;
        if (null_bursts(output, 400) < 0) {
            state.failed = true;
            if (close_port_locked(output)) notify(state, false, "pcm_leadin_failed");
            return VLC_EGENERIC;
        }
        notify(state, true, active_status(codec));
        msg_Info(output, "PS5 HDMI passthrough active: %s", pt::codec_name(codec));
        return VLC_SUCCESS;
    }
    // The same output can be retried by VLC after a passthrough decoder is
    // rejected. Its PCM branch preserves the existing SDL timeline and clock.
    state.codec = pt::Codec::unknown;
    format->i_format = VLC_CODEC_S16N;
    format->i_rate = 48000;
    format->i_physical_channels = AOUT_CHANS_2_0;
    format->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
    if (state.volume) state.volume(state.opaque, state.gain, state.muted);
    return VLC_SUCCESS;
}
bool output_bursts(audio_output_t* output, bool drain) {
    auto& state = *output->sys;
    size_t cursor = 0;
    const char* error = nullptr;
    for (;;) {
        state.burst.clear();
        if (!state.packer.next_burst(state.pending, &cursor, &state.burst, &error, drain)) break;
        const size_t grain = size_t(state.carrier.grain_frames) * state.carrier.frame_bytes;
        if (state.burst.empty() || state.burst.size() % grain != 0) return false;
        if (state.next_pts > VLC_TICK_INVALID) {
            const int64_t delay = state.next_pts - mdate();
            if (delay > 2000000) return false;
            if (delay > 0) std::this_thread::sleep_for(std::chrono::microseconds(delay));
        }
        for (size_t offset = 0; offset < state.burst.size(); offset += grain)
            if (submit_grain(output, state.burst.data() + offset) < 0) return false;
        if (state.next_pts > VLC_TICK_INVALID)
            state.next_pts += int64_t(state.burst.size()) * CLOCK_FREQ /
                (int64_t(state.carrier.frame_rate()) * state.carrier.frame_bytes);
    }
    if (cursor) state.pending.erase(state.pending.begin(), state.pending.begin() + cursor);
    return !error;
}
void play(audio_output_t* output, block_t* block) {
    auto& state = *output->sys;
    if (state.codec == pt::Codec::unknown) {
        state.pcm(state.opaque, block->p_buffer, block->i_nb_samples, block->i_pts);
    } else if (!state.failed) {
        bool okay = false;
        try {
            if (block->i_buffer <= maximum_pending - state.pending.size()) {
                if (state.next_pts == VLC_TICK_INVALID && block->i_pts > VLC_TICK_INVALID) state.next_pts = block->i_pts;
                state.pending.insert(state.pending.end(), block->p_buffer, block->p_buffer + block->i_buffer);
                okay = output_bursts(output, false);
            }
        } catch (...) { okay = false; }
        if (!okay) {
            state.failed = true;
            msg_Warn(output, "PS5 bitstream output failed; requesting PCM restart");
            // The player owns the retry; it stops this output before restoring
            // PCM and reopens once at the last presented media position.
            notify(state, false, "output_failed");
        }
    }
    block_Release(block);
}
void pause(audio_output_t* output, bool paused, vlc_tick_t date) {
    auto& state = *output->sys;
    if (state.codec != pt::Codec::unknown) {
        if (paused && !drain_port(output)) return;
        state.pending.clear(); state.next_pts = VLC_TICK_INVALID;
        return;
    }
    auto callback = paused ? state.pause : state.resume;
    if (callback) callback(state.opaque, date);
}
void flush(audio_output_t* output, bool wait) {
    auto& state = *output->sys;
    if (state.codec != pt::Codec::unknown) {
        if (wait && !state.failed) {
            bool okay = false;
            try { okay = output_bursts(output, true); } catch (...) {}
            if (!okay) { state.failed = true; notify(state, false, "output_failed"); }
        }
        if (!drain_port(output)) return;
        state.pending.clear(); state.burst.clear(); state.next_pts = VLC_TICK_INVALID;
    } else if (wait) { if (state.drain) state.drain(state.opaque); }
    else if (state.flush) state.flush(state.opaque, mdate());
}
int volume(audio_output_t* output, float gain) {
    auto& state = *output->sys; state.gain = gain;
    if (state.codec != pt::Codec::unknown) return -1;
    if (state.volume) state.volume(state.opaque, gain, state.muted);
    return 0;
}
int mute(audio_output_t* output, bool muted) {
    auto& state = *output->sys; state.muted = muted;
    if (state.codec != pt::Codec::unknown) return -1;
    if (state.volume) state.volume(state.opaque, state.gain, muted);
    return 0;
}
int open_output(vlc_object_t* object) {
    auto* output = reinterpret_cast<audio_output_t*>(object);
    auto* state = new(std::nothrow) aout_sys_t;
    if (!state) return VLC_ENOMEM;
    state->opaque = var_InheritAddress(object, "amem-data");
    state->pcm = reinterpret_cast<decltype(state->pcm)>(var_InheritAddress(object, "amem-play"));
    state->pause = reinterpret_cast<decltype(state->pause)>(var_InheritAddress(object, "amem-pause"));
    state->resume = reinterpret_cast<decltype(state->resume)>(var_InheritAddress(object, "amem-resume"));
    state->flush = reinterpret_cast<decltype(state->flush)>(var_InheritAddress(object, "amem-flush"));
    state->drain = reinterpret_cast<decltype(state->drain)>(var_InheritAddress(object, "amem-drain"));
    state->volume = reinterpret_cast<decltype(state->volume)>(var_InheritAddress(object, "amem-set-volume"));
    state->mode = reinterpret_cast<decltype(state->mode)>(var_InheritAddress(object, "stremio-audio-mode"));
    if (!state->pcm || !state->mode) { delete state; return VLC_EGENERIC; }
    output->sys = state;
    output->start = start; output->stop = stop; output->play = play;
    output->pause = pause; output->flush = flush; output->time_get = nullptr;
    output->volume_set = volume; output->mute_set = mute;
    return VLC_SUCCESS;
}
void close_output(vlc_object_t* object) {
    auto* output = reinterpret_cast<audio_output_t*>(object);
    // An unconfirmed shutdown retains one process-wide owner. Its callback
    // opaque is never used again after this point; future native/PCM opens
    // are blocked until the application exits.
    if (close_port(output)) delete output->sys;
    output->sys = nullptr;
}
int decode(decoder_t* decoder, block_t* block) {
    if (block) decoder_QueueAudio(decoder, block);
    return VLCDEC_SUCCESS;
}
int open_decoder(vlc_object_t* object) {
    auto* decoder = reinterpret_cast<decoder_t*>(object);
    if (!var_InheritBool(decoder, "stremio-audio-passthrough") ||
        codec_for(decoder->fmt_in.i_codec) == pt::Codec::unknown) return VLC_EGENERIC;
    decoder->fmt_out.i_codec = decoder->fmt_in.i_codec;
    decoder->fmt_out.audio = decoder->fmt_in.audio;
    decoder->fmt_out.audio.i_format = decoder->fmt_in.i_codec;
    decoder->fmt_out.i_profile = decoder->fmt_in.i_profile;
    if (decoder_UpdateAudioFormat(decoder)) return VLC_EGENERIC;
    decoder->pf_decode = decode; decoder->pf_flush = nullptr;
    return VLC_SUCCESS;
}
}

extern "C" bool stremio_ps5_audio_pcm_available() {
    return pcm_available.load(std::memory_order_acquire);
}

vlc_module_begin()
    set_shortname("Stremio PS5 audio")
    set_description("PS5 HDMI bitstream and PCM audio output")
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    set_capability("audio output", 0)
    set_callbacks(open_output, close_output)
    add_submodule()
    set_description("PS5 receiver-aware audio passthrough")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACODEC)
    set_capability("audio decoder", 140)
    set_callbacks(open_decoder, nullptr)
vlc_module_end()
