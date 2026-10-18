// Persisted episode labels and finite-download classification.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "download_manager.h"

#include <charconv>
#include <cstdio>

inline bool download_is_live(const Meta& meta, const std::string& type) {
    // hasScheduledVideos describes a catalog schedule, including unreleased
    // episodes of an ordinary series. It does not make its video streams live.
    return meta.is_live || type == "tv";
}

inline std::string download_episode_label(int season, int episode, const std::string& title) {
    std::string result;
    char number[32];
    if (season >= 0) { std::snprintf(number, sizeof(number), "S%02d", season); result = number; }
    if (episode >= 0) { std::snprintf(number, sizeof(number), "E%02d", episode); result += number; }
    if (!title.empty()) result += (result.empty() ? "" : " · ") + title;
    return result;
}

inline void download_episode_metadata(DownloadRequest& request, const Meta& meta) {
    for (const auto& video : meta.videos) if (video.id == request.video_id) {
        if (request.type != "series" && request.type != "anime" &&
            (!video.raw.is_object() || !video.raw.contains("season") || !video.raw.contains("episode"))) return;
        request.season = video.season;
        request.episode = video.episode;
        request.episode_title = video.title;
        request.subtitle = download_episode_label(request.season, request.episode, request.episode_title);
        return;
    }
    // Recover standard series IDs when a catalog supplied only a partial
    // metadata response. Opaque provider IDs are kept unchanged.
    const auto prefix = request.media_id + ':';
    if (request.type != "series" || request.video_id.rfind(prefix, 0) != 0) return;
    const auto split = request.video_id.find(':', prefix.size());
    if (split == std::string::npos) return;
    const auto* begin = request.video_id.data();
    const auto* end = begin + request.video_id.size();
    int season = -1, episode = -1;
    const auto s = std::from_chars(begin + prefix.size(), begin + split, season);
    const auto e = std::from_chars(begin + split + 1, end, episode);
    if (s.ec != std::errc{} || s.ptr != begin + split || e.ec != std::errc{} || e.ptr != end ||
        season < 0 || season > 99999 || episode < 0 || episode > 999999) return;
    request.season = season; request.episode = episode;
    request.subtitle = download_episode_label(season, episode, request.episode_title);
}

inline std::string download_legacy_episode_title(int season, int episode, const std::string& subtitle) {
    if (season < 0 || episode < 0) return {};
    const std::string prefixes[] = {
        download_episode_label(season, episode, {}) + " · ",
        "S" + std::to_string(season) + "E" + std::to_string(episode) + " · ",
        "S" + std::to_string(season) + " · E" + std::to_string(episode) + " · "
    };
    for (const auto& prefix : prefixes)
        if (subtitle.rfind(prefix, 0) == 0) return subtitle.substr(prefix.size());
    return {};
}
