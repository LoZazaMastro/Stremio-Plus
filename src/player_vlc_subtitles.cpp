// Keep ASS captions in Stremio's user-configurable subtitle renderer.
// Packet fields follow the Matroska/ASS packet format used by VLC's libass
// decoder. Timing and stream selection remain owned by libVLC.
// SPDX-License-Identifier: GPL-3.0-or-later
#define MODULE_NAME stremiosub
#define MODULE_STRING "stremiosub"

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_subpicture.h>
#include <vlc_variables.h>
#include <algorithm>
#include <map>
#include <string>

#include "subtitles.h"

namespace {
// Style alignments from the track's ASS header (the codec private data).
struct StyleSys {
    std::map<std::string, int> alignments;
};

int decode_subtitle(decoder_t* decoder, block_t* block) {
    if (!block) return VLCDEC_SUCCESS;
    if ((block->i_flags & BLOCK_FLAG_CORRUPTED) || block->i_pts <= VLC_TICK_INVALID || block->i_buffer > (1u << 20)) {
        block_Release(block);
        return VLCDEC_SUCCESS;
    }
    std::string text(reinterpret_cast<const char*>(block->p_buffer), block->i_buffer);
    size_t at = 0, style_at = 0, style_end = 0;
    // ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text.
    for (int field = 0; field < 8; ++field) {
        const size_t comma = text.find(',', at);
        if (comma == std::string::npos) { at = text.size(); break; }
        if (field == 2) { style_at = at; style_end = comma; }
        at = comma + 1;
    }
    // Comments and drawings hidden, style placement kept as an \an tag.
    std::string event;
    try {
        const auto* sys = reinterpret_cast<const StyleSys*>(decoder->p_sys);
        event = ass_event_text(text.substr(at), text.substr(style_at, style_end - style_at),
                               sys ? sys->alignments : std::map<std::string, int>{});
    } catch (...) { block_Release(block); return VLCDEC_SUCCESS; }
    subpicture_t* picture = decoder_NewSubpicture(decoder, nullptr);
    if (picture) {
        video_format_t format;
        video_format_Init(&format, VLC_CODEC_TEXT);
        picture->p_region = subpicture_region_New(&format);
        video_format_Clean(&format);
        if (picture->p_region) picture->p_region->p_text = text_segment_New(event.c_str());
        picture->i_start = block->i_pts;
        picture->i_stop = block->i_pts + std::max<vlc_tick_t>(block->i_length, 1);
        picture->b_ephemer = block->i_length <= 0;
        picture->b_absolute = false;
        picture->b_subtitle = true;
        decoder_QueueSub(decoder, picture);
    }
    block_Release(block);
    return VLCDEC_SUCCESS;
}

int open_subtitle(vlc_object_t* object) {
    auto* decoder = reinterpret_cast<decoder_t*>(object);
    if (decoder->fmt_in.i_codec != VLC_CODEC_SSA ||
        !var_InheritAddress(decoder, "stremio-subtitle-callback")) return VLC_EGENERIC;
    StyleSys* sys = nullptr;
    try {
        sys = new StyleSys;
        if (decoder->fmt_in.p_extra && decoder->fmt_in.i_extra > 0)
            sys->alignments = ass_style_alignments(std::string(static_cast<const char*>(decoder->fmt_in.p_extra),
                                                               size_t(decoder->fmt_in.i_extra)));
    } catch (...) { delete sys; return VLC_ENOMEM; }
    // VLC 3 types p_sys as an opaque decoder_sys_t; store ours through it.
    decoder->p_sys = reinterpret_cast<decoder_sys_t*>(sys);
    decoder->fmt_out.i_codec = VLC_CODEC_TEXT;
    decoder->pf_decode = decode_subtitle;
    return VLC_SUCCESS;
}

void close_subtitle(vlc_object_t* object) {
    auto* decoder = reinterpret_cast<decoder_t*>(object);
    delete reinterpret_cast<StyleSys*>(decoder->p_sys);
    decoder->p_sys = nullptr;
}
}

vlc_module_begin()
    set_shortname("Stremio ASS text")
    set_description("ASS text for the Stremio subtitle renderer")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_SCODEC)
    set_capability("spu decoder", 250)
    set_callbacks(open_subtitle, close_subtitle)
vlc_module_end()
