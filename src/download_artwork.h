// Download gallery covers include the opened 332 x 498 layout at 4K output.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr int kDownloadPosterWidth = 720;
inline constexpr int kDownloadPosterHeight = 1080;

inline bool download_poster_full_size(int width, int height) {
    return width >= kDownloadPosterWidth && height >= kDownloadPosterHeight;
}
