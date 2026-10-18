// Preserve Stremio's existing PS5 VP9 hardware decoder under libVLC.
// The H.264/HEVC backend comes from VLC-PS5; this narrow adapter reuses
// hwdec_ps5.cpp for 8-bit VP9 and leaves other profiles to VLC's decoders.
// SPDX-License-Identifier: GPL-3.0-or-later
#define MODULE_NAME stremiovp9
#define MODULE_STRING "stremiovp9"

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_variables.h>
#include "hwdec_ps5.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
void vlc_ps5_vdec_failed();
void vlc_ps5_vdec_active(int);
void vlc_ps5_copy_plane(uint8_t*, size_t, const uint8_t*, size_t, size_t, unsigned);
}
#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <new>

struct decoder_sys_t { std::unique_ptr<HwDecoder> decoder; };

namespace {
bool pictures(decoder_t* decoder) {
    HwDecoder::Picture input;
    for (;;) {
        const int result = decoder->p_sys->decoder->receive(&input);
        if (result < 0) return false;
        if (!result) return true;
        if (input.ten_bit || input.width <= 0 || input.height <= 0 || input.pitch < input.width) return false;
        auto& format = decoder->fmt_out.video;
        format.i_width = format.i_visible_width = unsigned(input.width);
        format.i_height = format.i_visible_height = unsigned(input.height);
        if (decoder_UpdateVideoFormat(decoder) != 0) return false;
        picture_t* output = decoder_NewPicture(decoder);
        if (!output) continue;
        vlc_ps5_copy_plane(output->p[0].p_pixels, output->p[0].i_pitch,
            input.y, size_t(input.pitch), size_t(input.width), unsigned(input.height));
        vlc_ps5_copy_plane(output->p[1].p_pixels, output->p[1].i_pitch,
            input.uv, size_t(input.pitch), size_t((input.width + 1) / 2) * 2, unsigned((input.height + 1) / 2));
        output->date = input.pts_us == INT64_MIN ? VLC_TICK_INVALID : input.pts_us;
        output->b_progressive = true;
        decoder_QueueVideo(decoder, output);
    }
}

int decode(decoder_t* decoder, block_t* block) {
    bool okay = true;
    if (block && (block->i_flags & BLOCK_FLAG_CORRUPTED)) {
        decoder->p_sys->decoder->flush();
        block_Release(block);
        return VLCDEC_SUCCESS;
    }
    if (block && block->i_buffer > INT_MAX) { block_Release(block); return VLCDEC_SUCCESS; }
    if (block && (block->i_flags & BLOCK_FLAG_DISCONTINUITY)) decoder->p_sys->decoder->flush();
    const int64_t pts = block && block->i_pts > VLC_TICK_INVALID ? block->i_pts : INT64_MIN;
    for (;;) {
        const int accepted = decoder->p_sys->decoder->send(block ? block->p_buffer : nullptr,
            block ? int(block->i_buffer) : 0, pts);
        if (accepted == 1) { if (!pictures(decoder)) { okay = false; break; } continue; }
        okay = accepted >= 0 && pictures(decoder);
        break;
    }
    if (block) block_Release(block);
    if (!okay) { vlc_ps5_vdec_failed(); return VLCDEC_ECRITICAL; }
    return VLCDEC_SUCCESS;
}
void flush(decoder_t* decoder) { decoder->p_sys->decoder->flush(); }

int open_decoder(vlc_object_t* object) {
    auto* decoder = reinterpret_cast<decoder_t*>(object);
    if (decoder->fmt_in.i_codec != VLC_CODEC_VP9 || !HwDecoder::available() ||
        !var_InheritBool(decoder, "ps5vdec") || decoder->fmt_in.i_profile > 0) return VLC_EGENERIC;
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    if (!parameters) return VLC_ENOMEM;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_VP9;
    parameters->format = AV_PIX_FMT_YUV420P;
    parameters->profile = decoder->fmt_in.i_profile;
    const auto& format = decoder->fmt_in.video;
    parameters->width = int(format.i_visible_width ? format.i_visible_width : format.i_width);
    parameters->height = int(format.i_visible_height ? format.i_visible_height : format.i_height);
    if (decoder->fmt_in.i_extra > 0 && decoder->fmt_in.i_extra <= (1 << 20)) {
        parameters->extradata = static_cast<uint8_t*>(av_mallocz(size_t(decoder->fmt_in.i_extra) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!parameters->extradata) { avcodec_parameters_free(&parameters); return VLC_ENOMEM; }
        parameters->extradata_size = decoder->fmt_in.i_extra;
        std::memcpy(parameters->extradata, decoder->fmt_in.p_extra, size_t(parameters->extradata_size));
    }
    std::string error;
    std::unique_ptr<HwDecoder> hardware(HwDecoder::open(parameters, &error));
    avcodec_parameters_free(&parameters);
    if (!hardware) return VLC_EGENERIC;
    auto* state = new(std::nothrow) decoder_sys_t;
    if (!state) return VLC_ENOMEM;
    state->decoder = std::move(hardware);
    decoder->p_sys = state;
    es_format_Copy(&decoder->fmt_out, &decoder->fmt_in);
    decoder->fmt_out.i_cat = VIDEO_ES;
    decoder->fmt_out.i_codec = decoder->fmt_out.video.i_chroma = VLC_CODEC_NV12;
    decoder->pf_decode = decode;
    decoder->pf_flush = flush;
    vlc_ps5_vdec_active(1);
    return VLC_SUCCESS;
}
void close_decoder(vlc_object_t* object) {
    auto* decoder = reinterpret_cast<decoder_t*>(object);
    delete decoder->p_sys;
    vlc_ps5_vdec_active(0);
}
}

vlc_module_begin()
    set_shortname("Stremio PS5 VP9")
    set_description("Stremio PS5 VP9 hardware decoder")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_capability("video decoder", 130)
    set_callbacks(open_decoder, close_decoder)
vlc_module_end()
