// Playback bookmarks and bounded resume of an already viewed download prefix.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

struct Progress {
    std::string type, name, poster, video_id;
    double time = 0, duration = 0;
    int64_t updated = 0;
    std::string download_id{}, download_source{};
    int64_t download_prefix = 0, download_total = -1;
};

inline std::string video_progress_key(const std::string& type, const std::string& media_id,
                                     const std::string& video_id) {
    // Lengths keep opaque addon IDs unambiguous, even if they contain colons.
    return std::to_string(type.size()) + ':' + type + std::to_string(media_id.size()) + ':' +
        media_id + std::to_string(video_id.size()) + ':' + video_id;
}

inline bool resumable_progress(const Progress& progress, const std::string& video_id) {
    return progress.video_id == video_id && std::isfinite(progress.time) && progress.time > 5 &&
        std::isfinite(progress.duration) && progress.duration >= 0 &&
        (progress.duration <= 0 || progress.time < progress.duration);
}

inline bool progressive_resume_proven(const Progress& progress, const std::string& download_id,
                                     const std::string& source_identity, int64_t prefix, int64_t total) {
    return !download_id.empty() && !source_identity.empty() && progress.download_id == download_id &&
        progress.download_source == source_identity && progress.download_prefix > 0 &&
        progress.download_total == total && total > 0 && progress.download_prefix <= total &&
        prefix >= progress.download_prefix && prefix <= total;
}
