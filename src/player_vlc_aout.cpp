// Stremio Plus audio output for libVLC. Three routes, chosen per stream:
//   stereo        decoded S16 stereo at 48 kHz to the player's clocked PCM
//                 queue and SDL device (the previous behaviour, via callbacks
//                 like VLC's amem);
//   multichannel  decoded 5.1 / 7.1 S16 to the console's 8-channel PCM port;
//   bitstream     Dolby Digital, Dolby Digital Plus, DTS, DTS-HD or Dolby TrueHD
//                 frames, framed by VLC's tospdif converter into IEC 61937
//                 bursts, to the console's HDMI bitstream port, so the
//                 receiver decodes them (Atmos and DTS:X ride along).
// The callback layout follows VLC's modules/audio_output/amem.c and the
// pass-through format negotiation its audiotrack.c (both LGPL-2.1-or-later).
// SPDX-License-Identifier: GPL-3.0-or-later
#define MODULE_NAME stremioaout
#define MODULE_STRING "stremioaout"

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_plugin.h>
#include <vlc_variables.h>

#include "player_vlc_passthrough.h"
#include "ps5_audio_sink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
using namespace stremio_audio;

typedef void (*play_cb)(void*, const void*, unsigned, int64_t);
typedef void (*stamp_cb)(void*, int64_t);
typedef void (*plain_cb)(void*);
typedef void (*volume_cb)(void*, float, bool);
typedef void (*note_cb)(void*, const char*);
typedef void (*played_cb)(void*, unsigned);

enum class Path { None, Stereo, Multichannel, Bitstream };

constexpr unsigned kRate = 48000;
constexpr unsigned kPs5Slots = 8;  // L R C LFE Ls Rs Lb Rb

template <typename T> T inherit(vlc_object_t* object, const char* name) {
    return reinterpret_cast<T>(var_InheritAddress(object, name));
}
}  // namespace

struct aout_sys_t {
    void* opaque = nullptr;
    play_cb play = nullptr;
    stamp_cb pause = nullptr, resume = nullptr, flush = nullptr;
    plain_cb drain = nullptr;
    volume_cb volume = nullptr;
    note_cb note = nullptr;
    played_cb played = nullptr;
    const ps5audio::Sink* sink = nullptr;
    int mode = kStereo;

    Path path = Path::None;
    ps5audio::Port port;
    unsigned in_channels = 0;
    int slot[kPs5Slots] = {};  // multichannel: VLC interleaved index per PS5 slot, -1 silent
    std::vector<uint8_t> scratch;
    float gain = 1.0f;
    bool muted = false;
    bool ready = false;
};

namespace {
void tell(aout_sys_t* sys, const char* text) {
    if (sys->note) sys->note(sys->opaque, text);
}

void count_played(aout_sys_t* sys, const block_t* block, unsigned rate) {
    if (!sys->played || !rate) return;
    const unsigned frames = block->i_length > 0 ? unsigned(block->i_length * kRate / CLOCK_FREQ)
                                                : unsigned(uint64_t(block->i_nb_samples) * kRate / rate);
    if (frames) sys->played(sys->opaque, frames);
}

// VLC hands interleaved S16 in its WG4 channel order; the console's 8-channel
// port wants L R C LFE Ls Rs Lb Rb. VLC's "middle" pair is the side pair; a
// 5.1 layout's rear pair takes the side slots.
void build_slots(aout_sys_t* sys, uint16_t mask) {
    static const uint32_t order[] = {AOUT_CHAN_LEFT, AOUT_CHAN_RIGHT, AOUT_CHAN_MIDDLELEFT, AOUT_CHAN_MIDDLERIGHT,
        AOUT_CHAN_REARLEFT, AOUT_CHAN_REARRIGHT, AOUT_CHAN_REARCENTER, AOUT_CHAN_CENTER, AOUT_CHAN_LFE};
    int index[9];
    int next = 0;
    for (int i = 0; i < 9; ++i) index[i] = (mask & order[i]) ? next++ : -1;
    const bool middle = (mask & AOUT_CHANS_MIDDLE) != 0;
    sys->slot[0] = index[0];
    sys->slot[1] = index[1];
    sys->slot[2] = index[7];
    sys->slot[3] = index[8];
    sys->slot[4] = middle ? index[2] : index[4];
    sys->slot[5] = middle ? index[3] : index[5];
    sys->slot[6] = middle ? index[4] : -1;
    sys->slot[7] = middle ? index[5] : -1;
    sys->in_channels = unsigned(next);
}

void apply_volume(audio_output_t* aout) {
    auto* sys = aout->sys;
    switch (sys->path) {
    case Path::Multichannel:
        aout_GainRequest(aout, sys->muted ? 0.0f : sys->gain);
        break;
    case Path::Bitstream:
        sys->port.set_muted(sys->muted);  // the receiver owns the volume
        break;
    default:
        if (sys->volume) sys->volume(sys->opaque, sys->gain, sys->muted);
        break;
    }
}

bool start_bitstream(audio_output_t* aout, audio_sample_format_t* fmt) {
    auto* sys = aout->sys;
    if (sys->mode != kPassthrough || !sys->sink) return false;
    ps5audio::Format format;
    const char* why = nullptr;
    if (!passthrough_route(fmt->i_format, *sys->sink, &format, &why)) {
        msg_Warn(aout, "no passthrough for %4.4s: %s", (const char*)&fmt->i_format, why);
        return false;
    }
    std::string error;
    if (!sys->port.open(format, &error)) {
        msg_Err(aout, "passthrough unavailable: %s", error.c_str());
        return false;
    }
    const ps5audio::Carrier& carrier = sys->port.carrier();
    // VLC's tospdif converter turns the frames into IEC 61937 bursts of S16
    // frames at the carrier's rate; the TrueHD port takes 16-byte frames.
    fmt->i_format = VLC_CODEC_SPDIFL;
    fmt->i_rate = unsigned(carrier.frame_rate);
    fmt->i_physical_channels = carrier.frame_bytes == 16 ? AOUT_CHANS_7_1 : AOUT_CHANS_2_0;
    fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
    fmt->i_bytes_per_frame = unsigned(carrier.frame_bytes);
    fmt->i_frame_length = 1;
    sys->path = Path::Bitstream;
    char text[160];
    snprintf(text, sizeof(text), "%s passthrough to %s", ps5audio::format_name(format),
             sys->sink->name.empty() ? "the HDMI receiver" : sys->sink->name.c_str());
    tell(sys, text);
    return true;
}

bool start_multichannel(audio_output_t* aout, audio_sample_format_t* fmt, unsigned in_channels) {
    auto* sys = aout->sys;
    if (sys->mode == kStereo || in_channels <= 2) return false;
    if (sys->sink && sys->sink->queried && !sys->sink->formats.empty() && sys->sink->lpcm_channels() <= 2) {
        msg_Warn(aout, "the HDMI sink lists stereo PCM only; decoding to stereo");
        return false;
    }
    std::string error;
    if (!sys->port.open(ps5audio::Format::Pcm8, &error)) {
        msg_Warn(aout, "multichannel PCM unavailable: %s", error.c_str());
        return false;
    }
    fmt->i_format = VLC_CODEC_S16N;
    fmt->i_rate = kRate;
    fmt->i_physical_channels = in_channels > 6 ? AOUT_CHANS_7_1 : AOUT_CHANS_5_1;
    fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
    build_slots(sys, fmt->i_physical_channels);
    sys->path = Path::Multichannel;
    tell(sys, in_channels > 6 ? "7.1 PCM" : "5.1 PCM");
    return true;
}

int Start(audio_output_t* aout, audio_sample_format_t* fmt) {
    auto* sys = aout->sys;
    sys->path = Path::None;
    if (!AOUT_FMT_LINEAR(fmt)) {
        if (!start_bitstream(aout, fmt)) return VLC_EGENERIC;  // VLC decodes the track instead
        apply_volume(aout);
        sys->ready = true;
        return VLC_SUCCESS;
    }
    const unsigned in_channels = aout_FormatNbChannels(fmt);
    if (in_channels == 0) return VLC_EGENERIC;
    if (!start_multichannel(aout, fmt, in_channels)) {
        fmt->i_format = VLC_CODEC_S16N;
        fmt->i_rate = kRate;
        fmt->i_physical_channels = AOUT_CHANS_2_0;
        fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
        sys->path = Path::Stereo;
        aout_GainRequest(aout, 1.0f);  // the player applies its own gain on this route
        tell(sys, sys->mode == kStereo ? "" : "stereo PCM");
    }
    apply_volume(aout);
    sys->ready = true;
    return VLC_SUCCESS;
}

void Stop(audio_output_t* aout) {
    auto* sys = aout->sys;
    if (sys->path == Path::Multichannel || sys->path == Path::Bitstream) {
        sys->port.close();
        tell(sys, "");
    }
    sys->path = Path::None;
    sys->ready = false;
}

void Play(audio_output_t* aout, block_t* block) {
    auto* sys = aout->sys;
    switch (sys->path) {
    case Path::Stereo:
        sys->play(sys->opaque, block->p_buffer, block->i_nb_samples, block->i_pts);
        break;
    case Path::Multichannel: {
        const unsigned frames = block->i_nb_samples, channels = sys->in_channels;
        if (channels && block->i_buffer >= size_t(frames) * channels * sizeof(int16_t)) {
            sys->scratch.resize(size_t(frames) * kPs5Slots * sizeof(int16_t));
            const auto* in = reinterpret_cast<const int16_t*>(block->p_buffer);
            auto* out = reinterpret_cast<int16_t*>(sys->scratch.data());
            for (unsigned f = 0; f < frames; ++f)
                for (unsigned s = 0; s < kPs5Slots; ++s)
                    out[f * kPs5Slots + s] = sys->slot[s] >= 0 ? in[f * channels + unsigned(sys->slot[s])] : 0;
            sys->port.write(sys->scratch.data(), sys->scratch.size());
            count_played(sys, block, kRate);
        }
        break;
    }
    case Path::Bitstream: {
        const uint8_t* data = block->p_buffer;
        // tospdif output starts with the IEC 61937 preamble; VLC's inserted
        // silence is plain zeros, which becomes null bursts here.
        const bool burst = block->i_buffer >= 4 && data[0] == 0x72 && data[1] == 0xF8 && data[2] == 0x1F && data[3] == 0x4E;
        if (burst) sys->port.write(data, block->i_buffer);
        else sys->port.write_silence(block->i_buffer);
        count_played(sys, block, unsigned(sys->port.carrier().frame_rate));
        break;
    }
    case Path::None:
        break;
    }
    block_Release(block);
}

int TimeGet(audio_output_t* aout, vlc_tick_t* delay) {
    auto* sys = aout->sys;
    if (sys->path != Path::Multichannel && sys->path != Path::Bitstream) return -1;  // the player clocks stereo by pts
    *delay = sys->port.delay_us();
    return 0;
}

void Pause(audio_output_t* aout, bool paused, vlc_tick_t date) {
    auto* sys = aout->sys;
    if (sys->path == Path::Stereo) {
        stamp_cb cb = paused ? sys->pause : sys->resume;
        if (cb) cb(sys->opaque, date);
    } else if (sys->path != Path::None) {
        sys->port.set_paused(paused);
    }
}

void Flush(audio_output_t* aout, bool wait) {
    auto* sys = aout->sys;
    if (sys->path == Path::Stereo) {
        if (wait) { if (sys->drain) sys->drain(sys->opaque); }
        else if (sys->flush) sys->flush(sys->opaque, 0);
    } else if (sys->path != Path::None) {
        if (wait) sys->port.drain(4500);
        else sys->port.flush();
    }
}

int VolumeSet(audio_output_t* aout, float volume) {
    auto* sys = aout->sys;
    sys->gain = std::clamp(volume, 0.0f, 4.0f);
    if (sys->ready) apply_volume(aout);
    return 0;
}

int MuteSet(audio_output_t* aout, bool mute) {
    auto* sys = aout->sys;
    sys->muted = mute;
    if (sys->ready) apply_volume(aout);
    return 0;
}

int Open(vlc_object_t* object) {
    auto* aout = reinterpret_cast<audio_output_t*>(object);
    auto* sys = new (std::nothrow) aout_sys_t;
    if (!sys) return VLC_ENOMEM;
    sys->opaque = var_InheritAddress(object, kVarOpaque);
    sys->play = inherit<play_cb>(object, kVarPlay);
    sys->pause = inherit<stamp_cb>(object, kVarPause);
    sys->resume = inherit<stamp_cb>(object, kVarResume);
    sys->flush = inherit<stamp_cb>(object, kVarFlush);
    sys->drain = inherit<plain_cb>(object, kVarDrain);
    sys->volume = inherit<volume_cb>(object, kVarVolume);
    sys->note = inherit<note_cb>(object, kVarNote);
    sys->played = inherit<played_cb>(object, kVarPlayed);
    sys->sink = static_cast<const ps5audio::Sink*>(var_InheritAddress(object, kVarSink));
    sys->mode = int(var_InheritInteger(object, kVarMode));
    if (!sys->opaque || !sys->play) {
        delete sys;
        return VLC_EGENERIC;
    }
    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout->time_get = TimeGet;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    aout->volume_set = VolumeSet;
    aout->mute_set = MuteSet;
    aout->device_select = nullptr;
    return VLC_SUCCESS;
}

void Close(vlc_object_t* object) {
    auto* aout = reinterpret_cast<audio_output_t*>(object);
    delete aout->sys;
    aout->sys = nullptr;
}
}  // namespace

vlc_module_begin()
    set_shortname("Stremio audio")
    set_description("Stremio Plus audio output: stereo, 7.1 PCM or HDMI bitstream")
    set_capability("audio output", 0)
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    add_shortcut("stremioaout")
    set_callbacks(Open, Close)
vlc_module_end()
