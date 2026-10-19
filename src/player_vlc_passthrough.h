// Shared by the Stremio VLC audio modules: the variables the player sets on
// the media player object, and which HDMI route a compressed format takes.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vlc_common.h>
#include <vlc_fourcc.h>

#include "ps5_audio_sink.h"

namespace stremio_audio {

// Settings → Audio output. The value of the "stremio-audio-mode" variable.
enum Mode { kStereo = 0, kSurround = 1, kPassthrough = 2 };

constexpr const char* kVarMode = "stremio-audio-mode";      // integer, Mode
constexpr const char* kVarSink = "stremio-audio-sink";      // address, const ps5audio::Sink*
constexpr const char* kVarOpaque = "stremio-audio-opaque";  // address, the player
constexpr const char* kVarPlay = "stremio-audio-play";      // void (*)(void*, const void* s16 stereo, unsigned frames, int64_t pts)
constexpr const char* kVarPause = "stremio-audio-pause";    // void (*)(void*, int64_t pts)
constexpr const char* kVarResume = "stremio-audio-resume";  // void (*)(void*, int64_t pts)
constexpr const char* kVarFlush = "stremio-audio-flush";    // void (*)(void*, int64_t)
constexpr const char* kVarDrain = "stremio-audio-drain";    // void (*)(void*)
constexpr const char* kVarVolume = "stremio-audio-volume";  // void (*)(void*, float volume, bool mute)
constexpr const char* kVarNote = "stremio-audio-note";      // void (*)(void*, const char* route)
constexpr const char* kVarPlayed = "stremio-audio-played";  // void (*)(void*, unsigned frames at 48 kHz)

// True when `codec` can go to the receiver as a bitstream; *format says how.
// A sink that could not be read (a splitter, a capture card) is trusted with
// the formats the user asked for.
inline bool passthrough_route(vlc_fourcc_t codec, const ps5audio::Sink& sink, ps5audio::Format* format,
                              const char** why) {
    using ps5audio::Format;
    const bool unknown = !sink.queried || sink.formats.empty();
    const auto listed = [&](int coding) { return unknown || sink.supports(coding); };
    switch (codec) {
    case VLC_CODEC_A52:
        if (listed(ps5audio::kAc3)) { *format = Format::Ac3; return true; }
        *why = "the receiver does not list Dolby Digital";
        return false;
    case VLC_CODEC_EAC3:
        if (listed(ps5audio::kEac3)) { *format = Format::Eac3; return true; }
        *why = "the receiver does not list Dolby Digital Plus";
        return false;
    case VLC_CODEC_DTS:
        // DTS-HD framing carries the core and the HD substreams; a DTS-only
        // receiver gets the core. An unread sink gets plain DTS, the safer bet.
        if (!unknown && sink.supports(ps5audio::kDtsHd)) { *format = Format::DtsHd; return true; }
        if (listed(ps5audio::kDts)) { *format = Format::Dts; return true; }
        *why = "the receiver does not list DTS";
        return false;
    case VLC_CODEC_MLP:
    case VLC_CODEC_TRUEHD:
        if (listed(ps5audio::kMat)) { *format = Format::TrueHd; return true; }
        *why = "the receiver does not list Dolby TrueHD";
        return false;
    default:
        *why = "not a bitstream format";
        return false;
    }
}

}  // namespace stremio_audio
