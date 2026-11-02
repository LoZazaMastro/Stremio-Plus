// Minimal VLC boundary for compiling the complete production audio module.
// This fixture does not validate libVLC object/module integration.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
using vlc_fourcc_t = uint32_t;
using vlc_tick_t = int64_t;
constexpr int VLC_SUCCESS = 0, VLC_EGENERIC = -1, VLC_ENOMEM = -2, VLCDEC_SUCCESS = 0;
constexpr int64_t CLOCK_FREQ = 1000000, VLC_TICK_INVALID = INT64_MIN;
enum { VLC_CODEC_A52=1, VLC_CODEC_EAC3, VLC_CODEC_DTS, VLC_CODEC_TRUEHD,
       VLC_CODEC_MLP, VLC_CODEC_OPUS, VLC_CODEC_S16N, VLC_CODEC_FL32 };
constexpr unsigned AOUT_CHANS_2_0 = 3, AUDIO_CHANNEL_TYPE_BITMAP = 1;
struct vlc_object_t {};
struct audio_sample_format_t {
    vlc_fourcc_t i_format = 0;
    unsigned i_rate = 0, i_physical_channels = 0, channel_type = 0;
};
struct block_t { uint8_t* p_buffer; size_t i_buffer; unsigned i_nb_samples; int64_t i_pts; };
inline void block_Release(block_t* block) { std::free(block->p_buffer); delete block; }
struct aout_sys_t;
struct audio_output_t {
    aout_sys_t* sys = nullptr;
    int (*start)(audio_output_t*, audio_sample_format_t*);
    void (*stop)(audio_output_t*);
    void (*play)(audio_output_t*, block_t*);
    void (*pause)(audio_output_t*, bool, vlc_tick_t);
    void (*flush)(audio_output_t*, bool);
    void* time_get;
    int (*volume_set)(audio_output_t*, float);
    int (*mute_set)(audio_output_t*, bool);
};
struct decoder_format_t { vlc_fourcc_t i_codec; audio_sample_format_t audio; int i_profile; };
struct decoder_t {
    decoder_format_t fmt_in, fmt_out;
    int (*pf_decode)(decoder_t*, block_t*);
    void* pf_flush;
};
inline void decoder_QueueAudio(decoder_t*, block_t* b) { block_Release(b); }
inline int decoder_UpdateAudioFormat(decoder_t*) { return 0; }
template<class T> inline bool var_InheritBool(T*, const char*) { return true; }
inline void* var_InheritAddress(vlc_object_t*, const char*) { return nullptr; }
inline int64_t mdate() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#define AOUT_FMT_LINEAR(f) ((f)->i_format == VLC_CODEC_S16N || (f)->i_format == VLC_CODEC_FL32)
#define msg_Err(...) ((void)0)
#define msg_Info(...) ((void)0)
#define msg_Warn(...) ((void)0)
#define vlc_module_begin()
#define vlc_module_end()
#define set_shortname(...)
#define set_description(...)
#define set_category(...)
#define set_subcategory(...)
#define set_capability(...)
#define set_callbacks(...)
#define add_submodule()
