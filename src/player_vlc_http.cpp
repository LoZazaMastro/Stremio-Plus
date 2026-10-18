// HTTP access for libVLC through Stremio's cancellable curl input.
// The access callback layout follows VLC-PS5's ps5http.c and VideoLAN's
// modules/access/imem-access.c. Addon headers also reach HLS/DASH segments.
// SPDX-License-Identifier: GPL-3.0-or-later
#define MODULE_NAME stremiohttp
#define MODULE_STRING "stremiohttp"

#include <vlc_common.h>
#include <vlc_access.h>
#include <vlc_plugin.h>
#include <vlc_variables.h>

#include "player_vlc_bridge.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {
struct HttpInput {
    void* input = nullptr;
    int64_t size = -1;
    std::string type;
};

ssize_t read_input(stream_t* access, void* data, size_t size) {
    auto* input = static_cast<HttpInput*>(access->p_sys);
    return stremio_vlc_input_read(input->input, static_cast<unsigned char*>(data), size);
}

int seek_input(stream_t* access, uint64_t offset) {
    auto* input = static_cast<HttpInput*>(access->p_sys);
    return stremio_vlc_input_seek(input->input, offset) == 0 ? VLC_SUCCESS : VLC_EGENERIC;
}

int control_input(stream_t* access, int query, va_list args) {
    auto* input = static_cast<HttpInput*>(access->p_sys);
    switch (query) {
        case STREAM_CAN_SEEK: *va_arg(args, bool*) = input->size >= 0; break;
        case STREAM_CAN_FASTSEEK: *va_arg(args, bool*) = false; break;
        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE: *va_arg(args, bool*) = true; break;
        case STREAM_GET_SIZE:
            if (input->size < 0) return VLC_EGENERIC;
            *va_arg(args, uint64_t*) = uint64_t(input->size);
            break;
        case STREAM_GET_PTS_DELAY:
            *va_arg(args, int64_t*) = INT64_C(1000) * var_InheritInteger(access, "network-caching");
            break;
        case STREAM_GET_CONTENT_TYPE:
            if (input->type.empty()) return VLC_EGENERIC;
            *va_arg(args, char**) = strdup(input->type.c_str());
            break;
        case STREAM_SET_PAUSE_STATE: break;
        default: return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int open_input(vlc_object_t* object) {
    auto* access = reinterpret_cast<stream_t*>(object);
    void* owner = var_InheritAddress(access, "stremio-http-opaque");
    if (!owner || !access->psz_url) return VLC_EGENERIC;
    auto* input = new(std::nothrow) HttpInput;
    if (!input) return VLC_ENOMEM;
    if (stremio_vlc_http_open(owner, access->psz_url, &input->input, &input->size)) {
        delete input;
        return VLC_EGENERIC;
    }
    std::string path = access->psz_url;
    path.resize(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".m3u8") == 0)
        input->type = "application/vnd.apple.mpegurl";
    else if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".mpd") == 0)
        input->type = "application/dash+xml";
    access->p_sys = input;
    access->pf_read = read_input;
    access->pf_block = nullptr;
    access->pf_seek = input->size >= 0 ? seek_input : nullptr;
    access->pf_control = control_input;
    return VLC_SUCCESS;
}

void close_input(vlc_object_t* object) {
    auto* access = reinterpret_cast<stream_t*>(object);
    auto* input = static_cast<HttpInput*>(access->p_sys);
    stremio_vlc_input_close(input->input);
    delete input;
}
}

vlc_module_begin()
    set_shortname("Stremio HTTP")
    set_description("Stremio HTTP input with addon request headers")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    set_capability("access", 500)
    add_shortcut("http", "https")
    set_callbacks(open_input, close_input)
vlc_module_end()
