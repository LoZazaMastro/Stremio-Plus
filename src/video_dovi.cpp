// Dolby Vision colour matrices and reshaping adapted from sainsaji/EVO-PLAYER-PS5.
// Uses FFmpeg public metadata types, with no private DOVIContext ABI mirror.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_dovi.h"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <array>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/dovi_meta.h>
}
namespace {
static const double k_lms2rgb[3][3] = {
    {  3.06441879, -2.16597676,  0.10155818 },
    { -0.65612108,  1.78554118, -0.12943749 },
    {  0.01736321, -0.04725154,  1.03004253 },
};

static double q2d(AVRational q) { return q.den ? (double)q.num / (double)q.den : 0.0; }

static void set_color(VideoDoviParams *p, const AVDOVIColorMetadata *c)
{
    double rgb2lms[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            p->ycc[i][j] = (float)q2d(c->ycc_to_rgb_matrix[i * 3 + j]);
            rgb2lms[i][j] = q2d(c->rgb_to_lms_matrix[i * 3 + j]);
        }
        p->ycc[i][3] = 0.0f;
        p->off[i] = (float)q2d(c->ycc_to_rgb_offset[i]);
    }
    p->off[3] = 0.0f;
    /* lms = k_lms2rgb * rgb_to_lms, as libplacebo composes them */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double s = 0.0;
            for (int k = 0; k < 3; k++)
                s += k_lms2rgb[i][k] * rgb2lms[k][j];
            p->lms[i][j] = (float)s;
        }
        p->lms[i][3] = 0.0f;
    }
}

static void set_identity_curves(VideoDoviParams *p)
{
    for (int c = 0; c < 3; c++) {
        p->lohi[c][0] = 0.0f;
        p->lohi[c][1] = 1.0f;
        p->lohi[c][2] = 0.0f;   /* no curve: the shader passes the signal through */
        p->lohi[c][3] = 0.0f;
    }
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 4; k++)
            p->piv[i][k] = 1e9f;
}

static int from_metadata(const void *av_dovi_metadata, VideoDoviParams *p)
{
    const AVDOVIMetadata *md = (const AVDOVIMetadata *)av_dovi_metadata;
    if (!md || !p)
        return -1;
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(md);
    const AVDOVIDataMapping   *map = av_dovi_get_mapping(md);
    const AVDOVIColorMetadata *col = av_dovi_get_color(md);
    if (hdr->bl_bit_depth < 8 || hdr->bl_bit_depth > 16 ||
        hdr->coef_log2_denom > 32)
        return -1;

    memset(p, 0, sizeof *p);
    set_identity_curves(p);
    set_color(p, col);

    const double piv_scale  = 1.0 / (double)((1u << hdr->bl_bit_depth) - 1u);
    const double coef_scale = 1.0 / (double)(1ull << hdr->coef_log2_denom);

    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *cv = &map->curves[c];
        const int np = cv->num_pivots;
        if (np == 0) continue;
        if (np < 2 || np > EVO_DOVI_PIECES + 1) return -1;
        for (int i = 0; i < np; ++i)
            if (cv->pivots[i] >= (1u << hdr->bl_bit_depth) ||
                (i > 0 && cv->pivots[i] <= cv->pivots[i - 1])) return -1;

        p->lohi[c][0] = (float)(piv_scale * cv->pivots[0]);
        p->lohi[c][1] = (float)(piv_scale * cv->pivots[np - 1]);
        p->lohi[c][2] = 1.0f;

        /* inner pivots pick the piece; the ends only clamp */
        for (int i = 1; i < np - 1; i++)
            p->piv[c * 2 + (i - 1) / 4][(i - 1) % 4] = (float)(piv_scale * cv->pivots[i]);

        int mmr_idx = 0;
        for (int i = 0; i < np - 1; i++) {
            float *co = p->coef[c * EVO_DOVI_PIECES + i];
            if (cv->mapping_idc[i] == AV_DOVI_MAPPING_POLYNOMIAL) {
                if (cv->poly_order[i] > 2) return -1;
                for (int k = 0; k < 3; k++)
                    co[k] = (k <= cv->poly_order[i]) ? (float)(coef_scale * cv->poly_coef[i][k])
                                                     : 0.0f;
                co[3] = 0.0f;
            } else if (cv->mapping_idc[i] == AV_DOVI_MAPPING_MMR) {
                const int order = cv->mmr_order[i];
                if (order < 1 || order > 3 || mmr_idx + 2 * order > EVO_DOVI_MMR_VEC4)
                    return -1;
                co[0] = (float)(coef_scale * cv->mmr_constant[i]);
                co[1] = (float)mmr_idx;
                co[2] = 0.0f;
                co[3] = (float)order;
                for (int j = 0; j < order; j++) {
                    float *a = p->mmr[c * EVO_DOVI_MMR_VEC4 + mmr_idx];
                    float *b = p->mmr[c * EVO_DOVI_MMR_VEC4 + mmr_idx + 1];
                    a[0] = (float)(coef_scale * cv->mmr_coef[i][j][0]);
                    a[1] = (float)(coef_scale * cv->mmr_coef[i][j][1]);
                    a[2] = (float)(coef_scale * cv->mmr_coef[i][j][2]);
                    a[3] = 0.0f;
                    b[0] = (float)(coef_scale * cv->mmr_coef[i][j][3]);
                    b[1] = (float)(coef_scale * cv->mmr_coef[i][j][4]);
                    b[2] = (float)(coef_scale * cv->mmr_coef[i][j][5]);
                    b[3] = (float)(coef_scale * cv->mmr_coef[i][j][6]);
                    mmr_idx += 2;
                }
            } else {
                return -1;
            }
        }
    }
    return 0;
}


}

bool video_dovi_parse_metadata(const void* data, size_t size, VideoDoviParams& result) {
    if (!data || size < sizeof(AVDOVIMetadata) ||
        reinterpret_cast<uintptr_t>(data) % alignof(AVDOVIMetadata)) return false;
    const auto* metadata = static_cast<const AVDOVIMetadata*>(data);
    const auto fits = [size](size_t offset, size_t length, size_t alignment) {
        return offset >= sizeof(AVDOVIMetadata) && offset % alignment == 0 &&
            offset <= size && length <= size - offset;
    };
    if (!fits(metadata->header_offset, sizeof(AVDOVIRpuDataHeader), alignof(AVDOVIRpuDataHeader)) ||
        !fits(metadata->mapping_offset, sizeof(AVDOVIDataMapping), alignof(AVDOVIDataMapping)) ||
        !fits(metadata->color_offset, sizeof(AVDOVIColorMetadata), alignof(AVDOVIColorMetadata))) return false;
    const auto* header = av_dovi_get_header(metadata);
    // The same Profile 5 inference used by FFmpeg's HEVC RPU parser.
    // Profile 8 has a displayable base layer and must keep its YCbCr path.
    if (header->vdr_rpu_profile != 0 || !header->bl_video_full_range_flag) return false;
    VideoDoviParams candidate{};
    if (from_metadata(metadata, &candidate) != 0) return false;
    std::array<float, sizeof(candidate) / sizeof(float)> values{};
    std::memcpy(values.data(), &candidate, sizeof(candidate));
    for (const float value : values) if (!std::isfinite(value)) return false;
    result = candidate;
    return true;
}

std::shared_ptr<const VideoDoviParams> video_dovi_from_frame(const AVFrame* frame) {
    if (!frame) return {};
    const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!side) return {};
    VideoDoviParams params{};
    if (!video_dovi_parse_metadata(side->data, side->size, params)) return {};
    try { return std::make_shared<const VideoDoviParams>(params); }
    catch (...) { return {}; }
}
