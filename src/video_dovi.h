// Dolby Vision reshaping adapted from sainsaji/EVO-PLAYER-PS5.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <cstddef>
struct AVFrame;
#define EVO_DOVI_PIECES   8
#define EVO_DOVI_MMR_VEC4 48

typedef struct VideoDoviParams {
    float ycc[3][4];        /* YCC -> L'M'S' rows                     */
    float off[4];           /* signal offset subtracted before ycc     */
    float lms[3][4];        /* linear LMS -> linear BT.2020 RGB rows   */
    float lohi[3][4];
    float piv[6][4];
    float coef[3 * EVO_DOVI_PIECES][4];
    float mmr[3 * EVO_DOVI_MMR_VEC4][4];
} VideoDoviParams;


std::shared_ptr<const VideoDoviParams> video_dovi_from_frame(const AVFrame* frame);
bool video_dovi_parse_metadata(const void* data, size_t size, VideoDoviParams& result);
