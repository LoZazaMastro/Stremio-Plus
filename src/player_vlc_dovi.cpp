// Preserve per-picture Dolby Vision metadata through VLC filters and clones.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player_vlc_dovi.h"
#include <vlc_common.h>
#include <vlc_picture.h>
#include <new>

namespace {
struct DoviContext {
    picture_context_t base;
    picture_context_t* previous = nullptr;
    std::shared_ptr<const VideoDoviParams> metadata;
};

void destroy(picture_context_t* base) {
    auto* context = reinterpret_cast<DoviContext*>(base);
    if (context->previous) context->previous->destroy(context->previous);
    delete context;
}

picture_context_t* copy(picture_context_t* base) {
    const auto* source = reinterpret_cast<DoviContext*>(base);
    auto* result = new (std::nothrow) DoviContext;
    if (!result) return nullptr;
    result->base = {destroy, copy};
    result->metadata = source->metadata;
    if (source->previous) {
        result->previous = source->previous->copy(source->previous);
        if (!result->previous) { delete result; return nullptr; }
    }
    return &result->base;
}
}

extern "C" void vlc_ps5_video_sidedata(picture_t* picture, const AVFrame* frame) {
    auto metadata = video_dovi_from_frame(frame);
    if (!picture || !metadata) return;
    auto* context = new (std::nothrow) DoviContext;
    if (!context) return;
    context->base = {destroy, copy};
    context->previous = picture->context;
    context->metadata = std::move(metadata);
    picture->context = &context->base;
}

std::shared_ptr<const VideoDoviParams> vlc_picture_dovi(const picture_t* picture) {
    if (!picture || !picture->context || picture->context->destroy != destroy) return {};
    return reinterpret_cast<const DoviContext*>(picture->context)->metadata;
}
