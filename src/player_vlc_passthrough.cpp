// Bitstream pass-through "decoder": hands Dolby Digital, Dolby Digital Plus,
// DTS and Dolby TrueHD frames to the audio output untouched when the user chose
// passthrough and the HDMI receiver lists the format. It follows VLC's own
// modules/codec/spdif.c (LGPL-2.1-or-later); when the output refuses the
// stream, VLC moves on to its software decoder as usual.
// SPDX-License-Identifier: GPL-3.0-or-later
#define MODULE_NAME stremiopass
#define MODULE_STRING "stremiopass"

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_variables.h>

#include "player_vlc_passthrough.h"

namespace {
int decode_passthrough(decoder_t* decoder, block_t* block) {
    if (block) decoder_QueueAudio(decoder, block);
    return VLCDEC_SUCCESS;
}

int open_passthrough(vlc_object_t* object) {
    auto* decoder = reinterpret_cast<decoder_t*>(object);
    if (var_InheritInteger(decoder, stremio_audio::kVarMode) != stremio_audio::kPassthrough) return VLC_EGENERIC;
    const auto* sink = static_cast<const ps5audio::Sink*>(var_InheritAddress(decoder, stremio_audio::kVarSink));
    ps5audio::Format format;
    const char* why = nullptr;
    if (!sink || !stremio_audio::passthrough_route(decoder->fmt_in.i_codec, *sink, &format, &why)) {
        if (why) msg_Dbg(decoder, "no passthrough: %s", why);
        return VLC_EGENERIC;
    }
    decoder->fmt_out.i_codec = decoder->fmt_in.i_codec;
    decoder->fmt_out.audio = decoder->fmt_in.audio;
    decoder->fmt_out.i_profile = decoder->fmt_in.i_profile;
    decoder->fmt_out.audio.i_format = decoder->fmt_out.i_codec;
    // Opens the output stream now. A refusal (the port could not be opened)
    // fails this module, and VLC decodes the track instead.
    if (decoder_UpdateAudioFormat(decoder)) return VLC_EGENERIC;
    decoder->pf_decode = decode_passthrough;
    decoder->pf_flush = nullptr;
    return VLC_SUCCESS;
}
}  // namespace

vlc_module_begin()
    set_shortname("Stremio passthrough")
    set_description("Bitstream pass-through to the HDMI receiver")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACODEC)
    set_capability("audio decoder", 120)
    set_callbacks(open_passthrough, nullptr)
vlc_module_end()
