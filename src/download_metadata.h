// Persisted episode labels and finite-download classification.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "download_manager.h"

#include <charconv>
#include <cstdio>
#include <string_view>

inline int download_release_year(std::string_view value) {
    // Catalog releaseInfo may be a year or a series range. Never infer a
    // release year from a filename, episode label or provider description.
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return 0;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.size() < 4) return 0;
    int year = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + 4, year);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + 4 || year < 1000 || year > 9999) return 0;
    value.remove_prefix(4);
    if (value.empty()) return year;
    const auto separator = value.find_first_not_of(" \t");
    if (separator == std::string_view::npos) return year;
    value.remove_prefix(separator);
    if (value.starts_with("–") || value.starts_with("—")) value.remove_prefix(3);
    else if (value.starts_with("-")) value.remove_prefix(1);
    else return 0;
    const auto range_start = value.find_first_not_of(" \t");
    if (range_start == std::string_view::npos) return year;
    value.remove_prefix(range_start);
    if (value.size() != 4) return 0;
    int last_year = 0;
    const auto end = std::from_chars(value.data(), value.data() + 4, last_year);
    return end.ec == std::errc{} && end.ptr == value.data() + 4 && last_year >= year && last_year <= 9999 ? year : 0;
}

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
        request.thumbnail_url = video.thumbnail;
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
