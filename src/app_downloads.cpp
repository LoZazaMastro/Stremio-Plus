// Downloads of the highlighted stream, with independent offline playback.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "download_text.h"
#include "download_metadata.h"
#include "download_artwork.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
std::string pending_relocation_text(bool italian) {
    return download_error_text(
        "A download relocation is pending. Retry the same destination to finish moving the saved files.", italian);
}
std::string download_bytes(int64_t bytes) {
    if (bytes < 0) return {};
    char value[64];
    if (bytes >= (1ll << 30)) std::snprintf(value, sizeof(value), "%.2f GiB", double(bytes) / (1ll << 30));
    else std::snprintf(value, sizeof(value), "%.1f MiB", double(bytes) / (1ll << 20));
    return value;
}
const Progress* saved_download_progress(const DownloadEntry& entry,
    const std::map<std::string, Progress>& videos, const std::map<std::string, Progress>& series) {
    const auto video = videos.find(video_progress_key(entry.type, entry.media_id, entry.video_id));
    if (video != videos.end()) return &video->second;
    const auto legacy = series.find(entry.media_id);
    return legacy == series.end() || legacy->second.type != entry.type ? nullptr : &legacy->second;
}

std::string download_group_key(const UiDownload& row) {
    if (!row.recovery_only && (row.type == "series" || row.type == "anime") && !row.media_id.empty())
        return row.type + ':' + row.media_id;
    return row.id;
}

std::string download_title_key(const std::string& title) {
    // Fold Western titles consistently on PS5 and desktop without relying
    // on installed locales. Italian accents sort alongside their base letter;
    // the original title remains untouched for display and saved metadata.
    std::string key;
    for (size_t index = 0; index < title.size(); ++index) {
        const auto first = static_cast<unsigned char>(title[index]);
        if (first < 0x80) {
            key += first >= 'A' && first <= 'Z' ? char(first - 'A' + 'a') : char(first);
            continue;
        }
        if (index + 1 < title.size()) {
            const auto second = static_cast<unsigned char>(title[index + 1]);
            if (first == 0xc3 && second >= 0x80 && second <= 0xbf) {
                const auto letter = second | 0x20;
                const char* folded = letter <= 0xa5 ? "a" : letter == 0xa6 ? "ae" : letter == 0xa7 ? "c" :
                    letter <= 0xab ? "e" : letter <= 0xaf ? "i" : letter == 0xb0 ? "d" : letter == 0xb1 ? "n" :
                    letter <= 0xb6 || letter == 0xb8 ? "o" : letter >= 0xb9 && letter <= 0xbc ? "u" :
                    letter == 0xbd ? "y" : letter == 0xbe ? "th" : second == 0x9f ? "ss" : second == 0xbf ? "y" : nullptr;
                if (folded) { key += folded; ++index; continue; }
            }
            if ((first == 0xcc && second >= 0x80 && second <= 0xbf) ||
                (first == 0xcd && second >= 0x80 && second <= 0xaf)) { ++index; continue; }
        }
        key += char(first);
    }
    return trim(key);
}

void select_download_season(App& app, bool keep_selection) {
    app.download_episode_rows.clear();
    if (app.download_series_id.empty()) return;
    const auto group = std::find_if(app.download_groups.begin(), app.download_groups.end(),
        [&](const UiDownloadGroup& value) { return value.id == app.download_series_id; });
    if (group == app.download_groups.end() || !group->series || group->seasons.empty()) {
        app.download_series_id.clear();
        return;
    }
    if (std::find(group->seasons.begin(), group->seasons.end(), app.download_season) == group->seasons.end())
        app.download_season = group->seasons.front();
    for (const int row : group->rows)
        if (app.download_rows[static_cast<size_t>(row)].season == app.download_season)
            app.download_episode_rows.push_back(row);
    std::stable_sort(app.download_episode_rows.begin(), app.download_episode_rows.end(), [&](int a, int b) {
        const auto& left = app.download_rows[static_cast<size_t>(a)];
        const auto& right = app.download_rows[static_cast<size_t>(b)];
        if (left.episode < 0 || right.episode < 0)
            return left.episode >= 0 && right.episode < 0;
        return left.episode < right.episode;
    });
    if (!app.download_episode_rows.empty() && (!keep_selection ||
        std::find(app.download_episode_rows.begin(), app.download_episode_rows.end(), app.download_sel) == app.download_episode_rows.end()))
        app.download_sel = app.download_episode_rows.front();
}

void rebuild_download_gallery(App& app) {
    const auto previous_group = app.download_group_sel >= 0 && app.download_group_sel < int(app.download_groups.size())
        ? app.download_groups[static_cast<size_t>(app.download_group_sel)].id : std::string{};
    app.download_groups.clear();
    std::map<std::string, size_t> positions;
    for (size_t row = 0; row < app.download_rows.size(); ++row) {
        const auto& item = app.download_rows[row];
        const auto key = download_group_key(item);
        auto [where, inserted] = positions.emplace(key, app.download_groups.size());
        if (inserted) {
            UiDownloadGroup group;
            group.id = key; group.title = item.title; group.image = item.image;
            group.series = !item.recovery_only && (item.type == "series" || item.type == "anime");
            group.representative = int(row);
            app.download_groups.push_back(std::move(group));
        }
        auto& group = app.download_groups[where->second];
        group.rows.push_back(int(row));
        if (item.release_year > 0 && (group.release_year == 0 || item.release_year < group.release_year))
            group.release_year = item.release_year;
        group.downloaded_ms = std::max(group.downloaded_ms, item.downloaded_ms);
        if (group.image.empty()) group.image = item.image;
        if (item.complete) ++group.complete;
        const auto& representative = app.download_rows[static_cast<size_t>(group.representative)];
        if (item.active || (!representative.active && representative.complete && !item.complete))
            group.representative = int(row);
        if (group.series && std::find(group.seasons.begin(), group.seasons.end(), item.season) == group.seasons.end())
            group.seasons.push_back(item.season);
    }
    for (auto& group : app.download_groups)
        std::sort(group.seasons.begin(), group.seasons.end(), [](int a, int b) {
            return a < 0 ? false : b < 0 || a < b;
        });
    std::map<std::string, std::string> titles;
    for (const auto& group : app.download_groups) titles.emplace(group.id, download_title_key(group.title));
    const int mode = app.settings().download_sort;
    std::sort(app.download_groups.begin(), app.download_groups.end(), [&](const UiDownloadGroup& a, const UiDownloadGroup& b) {
        if (mode == 1 && a.release_year != b.release_year) return a.release_year > b.release_year;
        if (mode == 2 && a.downloaded_ms != b.downloaded_ms) return a.downloaded_ms > b.downloaded_ms;
        const auto& a_title = titles.at(a.id);
        const auto& b_title = titles.at(b.id);
        return a_title == b_title ? a.id < b.id : a_title < b_title;
    });
    app.download_group_sel = std::clamp(app.download_group_sel, 0, std::max(0, int(app.download_groups.size()) - 1));
    for (size_t i = 0; i < app.download_groups.size(); ++i)
        if (app.download_groups[i].id == previous_group) { app.download_group_sel = int(i); break; }
    select_download_season(app, true);
    if (app.download_series_id.empty() && !app.download_groups.empty())
        app.download_sel = app.download_groups[static_cast<size_t>(app.download_group_sel)].representative;
}
}

void App::download_selected_stream(const Stream& stream) {
    if (download_relocation_active()) {
        show_toast(settings_.ui_language == "it" ? "Spostamento dei download in corso." : "Moving downloads. Please wait.");
        return;
    }
    if (downloads_.relocation_status().pending) {
        show_toast(pending_relocation_text(settings_.ui_language == "it"), 7);
        return;
    }
    if (downloads_.download_directory().empty()) {
        download_directory_notice = true;
        dirty_all();
        return;
    }
    const bool it = settings_.ui_language == "it";
    if (!stream.playable()) {
        show_toast(it ? "Questa sorgente non può essere scaricata." : "This source cannot be downloaded.");
        return;
    }
    DownloadRequest request;
    request.media_id = d_item_.id;
    request.type = d_item_.type;
    request.video_id = d_video_id_;
    request.title = d_meta_loaded_ && !d_meta_.name.empty() ? d_meta_.name : d_item_.name;
    if (d_meta_loaded_) {
        request.release_year = download_release_year(d_meta_.release_info);
        if (!request.release_year) request.release_year = download_release_year(d_meta_.year);
    }
    if (!request.release_year) request.release_year = download_release_year(d_item_.release_info);
    request.stream = stream; // Exactly one selected result, including this installation's headers.
    request.live = download_is_live(d_meta_loaded_ ? d_meta_ : Meta{}, d_item_.type);
    const auto poster = d_meta_loaded_ && !d_meta_.poster.empty() ? d_meta_.poster : d_item_.poster;
    const auto background = d_meta_loaded_ && !d_meta_.background.empty() ? d_meta_.background : d_item_.background;
    const auto logo = d_meta_loaded_ && !d_meta_.logo.empty() ? d_meta_.logo : d_item_.logo;
    request.poster_url = poster;
    request.poster_path = g_art.peek_cached(poster, ArtKind::DownloadPoster);
    if (request.poster_path.empty()) request.poster_path = g_art.peek_cached(poster, ArtKind::PosterLarge);
    if (request.poster_path.empty()) request.poster_path = g_art.peek_cached(poster, ArtKind::Poster);
    request.background_path = g_art.peek_cached(background, ArtKind::Backdrop);
    if (request.background_path.empty()) request.background_path = g_art.peek_cached(background, ArtKind::Background);
    request.logo_path = g_art.peek_cached(logo, ArtKind::LaunchLogo);
    if (request.logo_path.empty()) request.logo_path = g_art.peek_cached(logo, ArtKind::LogoBox);
    download_episode_metadata(request, d_meta_loaded_ ? d_meta_ : Meta{});
    request.thumbnail_path = g_art.peek_cached(request.thumbnail_url, ArtKind::Thumb);
    if (request.subtitle.empty()) request.subtitle = d_year;
    if (request.live) {
        show_toast(it ? "Le dirette non possono essere scaricate per la visione offline." :
                      "Live streams cannot be downloaded for offline viewing.");
        return;
    }
    // Response overrides need the same configured proxy as ordinary playback.
    if (!stream.url.empty() && !stream.response_headers.empty() && !server().empty()) {
        request.stream.url = stream_proxy_url(stream, server());
        request.stream.request_headers.clear();
        request.stream.response_headers.clear();
        if (request.stream.url.empty()) {
            show_toast(it ? "Impossibile preparare questa sorgente." : "Could not prepare this source.");
            return;
        }
    }
    const auto account = account_generation_;
    const bool submitted = downloads_.enqueue_async(std::move(request),
        [this, account](std::string id, std::string error) {
            g_tasks.post([this, account, id = std::move(id), error = std::move(error)] {
                if (account != account_generation_) return;
                const bool it = settings_.ui_language == "it";
                if (id.empty()) {
                    dlog("Selected stream could not be queued: %s", error.c_str());
                    const auto storage_error = downloads_.storage_error();
                    if (!storage_error.empty()) dlog("Download storage failure: %s", storage_error.c_str());
                    show_toast((it ? "Download non disponibile: " : "Download unavailable: ") + download_error_text(error, it), 6);
                    return;
                }
                refresh_download_artwork();
                downloads_refresh();
                show_toast(it ? "Video aggiunto a Download" : "Video added to Downloads", 2.5);
            });
        });
    if (submitted) show_toast(it ? "Aggiunta a Download…" : "Adding to Downloads…", 2);
    else show_toast(it ? "Impossibile aggiungere altri download in questo momento." : "Could not add another download right now.", 4);
}

void App::refresh_download_artwork() {
    if (download_relocation_active()) return;
    if (download_art_account_ != account_generation_) {
        if (download_art_cancel_) download_art_cancel_->store(true);
        download_art_cancel_ = std::make_shared<std::atomic<bool>>(false);
        download_art_account_ = account_generation_;
        download_meta_pending_.clear(); download_meta_retry_.clear();
    }
    const auto entries = downloads_.snapshot();
    const double now = now_seconds();
    const int account = account_generation_;
    for (auto it = download_meta_retry_.begin(); it != download_meta_retry_.end();) {
        if (std::none_of(entries.begin(), entries.end(), [&](const DownloadEntry& entry) {
                return entry.id == it->first || "episodes:" + entry.type + ':' + entry.media_id == it->first;
            }))
            it = download_meta_retry_.erase(it);
        else ++it;
    }
    std::set<std::string> requested_urls;
    if (!downloads_.storage_available()) return;
    for (const auto& entry : entries) {
        // The worker validates and publishes private artwork paths. Polling
        // every row with stat() here can stall navigation behind media writes.
        if (entry.recovery_only || download_poster_full_size(entry.poster_width, entry.poster_height)) continue;
        std::string poster = entry.poster_url;
        if (poster.empty()) {
            // Older manifests had only a temporary cache path. Reuse already
            // loaded catalog metadata before making a provider request.
            const auto take = [&](const Item& item) {
                if (poster.empty() && item.id == entry.media_id && item.type == entry.type) poster = item.poster;
            };
            take(d_item_);
            for (const auto& row : board_) for (const auto& item : row.items) take(item);
            for (const auto& item : disc_items_) take(item);
            for (const auto& item : lib_items_) take(item);
            if (!poster.empty()) downloads_.update_poster_source(entry.id, poster);
        }
        if (poster.empty()) {
            if (offline_ || !signed_in() || download_meta_pending_.size() >= 2 ||
                download_meta_pending_.count(entry.id) || now < download_meta_retry_[entry.id]) continue;
            std::vector<std::string> providers;
            for (const auto& addon : addons_) if (addon->supports("meta", entry.type, entry.media_id)) {
                providers.push_back(addon->resource_url("meta", entry.type, entry.media_id));
                if (providers.size() == 3) break;
            }
            if (providers.empty()) continue;
            const auto cancel = download_art_cancel_;
            const auto id = entry.id;
            download_meta_pending_.insert(id); download_meta_retry_[id] = now + 60;
            g_tasks.run<std::string>([providers, cancel] {
                for (const auto& url : providers) {
                    if (cancel->load()) break;
                    json document; std::string error;
                    if (!fetch_json(url, document, error, 6, cancel.get())) continue;
                    const auto poster = jstr(jobj(document, "meta"), "poster");
                    if (!poster.empty() && poster.size() <= 16384) return poster;
                }
                return std::string{};
            }, [this, id, cancel, account](std::string& url) {
                if (account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
                download_meta_pending_.erase(id);
                if (!url.empty() && downloads_.update_poster_source(id, url)) download_art_refresh_at_ = 0;
            });
            continue;
        }
        if (!requested_urls.insert(poster).second) continue;
        // A shared URL may belong to multiple queued episodes: one callback
        // updates all matching entries and cannot be lost to observer dedupe.
        const auto accept = [this, poster, account, cancel = download_art_cancel_](const std::string& path) {
            if (path.empty() || account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
            for (const auto& item : downloads_.snapshot())
                if (!item.recovery_only && item.poster_url == poster) downloads_.update_poster(item.id, path);
            images_dirty_ = true;
            if (view == "downloads") downloads_refresh();
        };
        const auto ready = [this, poster, accept, account, cancel = download_art_cancel_](const std::string& path) {
            if (account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
            if (!path.empty()) { accept(path); return; }
            // A provider may lack its larger variant. Preserve existing local
            // posters and use the original source only for still-empty covers.
            const auto current = downloads_.snapshot();
            if (std::none_of(current.begin(), current.end(), [&](const DownloadEntry& item) {
                    return !item.recovery_only && item.poster_url == poster && item.poster_path.empty();
                })) return;
            auto fallback = g_art.peek_cached(poster, ArtKind::PosterLarge);
            if (fallback.empty()) fallback = g_art.peek_cached(poster, ArtKind::Poster);
            if (fallback.empty()) fallback = g_art.get_async(poster, ArtKind::PosterLarge, accept, ArtPriority::Visible, &downloads_);
            if (!fallback.empty()) accept(fallback);
        };
        // Completed legacy downloads are upgraded too. Their private image is
        // kept until the worker has validated and atomically copied this one.
        auto cached = g_art.peek_cached(poster, ArtKind::DownloadPoster);
        if (cached.empty()) cached = g_art.get_async(poster, ArtKind::DownloadPoster, ready, ArtPriority::Visible, &downloads_);
        if (!cached.empty()) accept(cached);
    }

    // One series metadata response supplies all its saved episode thumbnails.
    // The images use the same deferred artwork writer as posters, so queued
    // episodes never compete with an active media write or block navigation.
    std::set<std::string> requested_series, requested_thumbnails;
    int thumbnail_requests = 0;
    const auto selected_thumbnail = download_sel >= 0 && download_sel < int(download_rows.size())
        ? download_rows[static_cast<size_t>(download_sel)].id : std::string{};
    for (int priority = 2; priority >= 0; --priority) for (const auto& entry : entries) {
        const bool visible = view == "downloads" && download_series_id == entry.type + ':' + entry.media_id && download_season == entry.season;
        const int relevance = visible ? (entry.id == selected_thumbnail ? 2 : 1) : 0;
        if (relevance != priority) continue;
        if (entry.recovery_only || !entry.thumbnail_path.empty() ||
            (entry.type != "series" && entry.type != "anime")) continue;
        std::string thumbnail = entry.thumbnail_url;
        if (thumbnail.empty() && d_meta_loaded_ && d_item_.type == entry.type && d_item_.id == entry.media_id) {
            for (const auto& video : d_meta_.videos) if (video.id == entry.video_id) {
                thumbnail = video.thumbnail;
                if (!thumbnail.empty()) downloads_.update_thumbnail_source(entry.id, thumbnail);
                break;
            }
        }
        if (thumbnail.empty()) {
            const auto key = "episodes:" + entry.type + ':' + entry.media_id;
            if (!requested_series.insert(key).second || offline_ || !signed_in() ||
                download_meta_pending_.size() >= 2 || download_meta_pending_.count(key) || now < download_meta_retry_[key]) continue;
            std::vector<std::string> providers;
            for (const auto& addon : addons_) if (addon->supports("meta", entry.type, entry.media_id)) {
                providers.push_back(addon->resource_url("meta", entry.type, entry.media_id));
                if (providers.size() == 3) break;
            }
            if (providers.empty()) continue;
            const auto cancel = download_art_cancel_;
            const auto media_id = entry.media_id, type = entry.type;
            std::set<std::string> wanted;
            for (const auto& saved : entries) if (saved.media_id == media_id && saved.type == type && saved.thumbnail_path.empty())
                wanted.insert(saved.video_id);
            download_meta_pending_.insert(key); download_meta_retry_[key] = now + 300;
            g_tasks.run<std::map<std::string, std::string>>([providers, cancel, wanted] {
                std::map<std::string, std::string> result;
                for (const auto& url : providers) {
                    if (cancel->load()) break;
                    json document; std::string error;
                    if (!fetch_json(url, document, error, 6, cancel.get())) continue;
                    const auto& videos = jobj(jobj(document, "meta"), "videos");
                    if (!videos.is_array()) continue;
                    size_t checked = 0;
                    for (const auto& video : videos) {
                        if (++checked > 10000) break;
                        const auto id = jstr(video, "id");
                        if (!wanted.count(id)) continue;
                        const auto thumbnail = jstr(video, "thumbnail");
                        if (!thumbnail.empty() && thumbnail.size() <= 16384) result.emplace(id, thumbnail);
                    }
                    if (result.size() == wanted.size()) break;
                }
                return result;
            }, [this, key, media_id, type, cancel, account](std::map<std::string, std::string>& result) {
                if (account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
                download_meta_pending_.erase(key);
                for (const auto& saved : downloads_.snapshot()) {
                    if (saved.media_id != media_id || saved.type != type) continue;
                    const auto image = result.find(saved.video_id);
                    if (image != result.end()) downloads_.update_thumbnail_source(saved.id, image->second);
                }
                if (!result.empty()) download_art_refresh_at_ = 0;
            });
            continue;
        }
        if (thumbnail_requests >= 16 || !requested_thumbnails.insert(thumbnail).second) continue;
        ++thumbnail_requests;
        const auto accept = [this, thumbnail, account, cancel = download_art_cancel_](const std::string& path) {
            if (path.empty() || account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
            for (const auto& item : downloads_.snapshot())
                if (!item.recovery_only && item.thumbnail_url == thumbnail) downloads_.update_thumbnail(item.id, path);
            images_dirty_ = true;
            if (view == "downloads") downloads_refresh();
        };
        auto cached = g_art.peek_cached(thumbnail, ArtKind::Thumb);
        if (cached.empty()) cached = g_art.get_async(thumbnail, ArtKind::Thumb, accept,
            relevance == 2 ? ArtPriority::Immediate : visible ? ArtPriority::Visible : ArtPriority::Prefetch, &download_rows);
        if (!cached.empty()) accept(cached);
    }
}

void App::downloads_refresh() {
    const bool it = settings_.ui_language == "it";
    const auto selected = download_sel >= 0 && download_sel < int(download_rows.size()) ? download_rows[download_sel].id : "";
    // Capture before the snapshot so a concurrent completion is seen on the next poll.
    download_revision_ = downloads_.revision();
    const auto entries = downloads_.snapshot();
    download_rows.clear();
    size_t pending = 0, recovered = 0;
    for (const auto& entry : entries) {
        UiDownload row;
        row.id = entry.id; row.title = entry.title; row.subtitle = entry.subtitle;
        row.media_id = entry.media_id; row.type = entry.type;
        row.release_year = entry.release_year;
        row.downloaded_ms = entry.state == DownloadState::Complete && entry.completed_ms > 0
            ? entry.completed_ms : entry.created_ms;
        row.season = entry.season; row.episode = entry.episode;
        row.episode_title = entry.episode_title; row.thumbnail = entry.thumbnail_path;
        row.recovery_only = entry.recovery_only;
        if (entry.recovery_only) {
            row.title = (it ? "Download da verificare · " : "Unrecognized download · ") + entry.id.substr(1, 8);
            row.subtitle = it ? "I file sono ancora presenti sulla PS5." : "The files are still on your PS5.";
            row.status = entry.error.empty() || entry.error == "Download metadata is missing or damaged. You can delete the saved files."
                ? (it ? "Informazioni del download mancanti o illeggibili" : "Download information is missing or unreadable")
                : download_error_text(entry.error, it);
            row.size = (it ? "Spazio occupato: " : "Storage used: ") +
                (entry.disk_bytes < 0 ? (it ? "da verificare" : "unknown") : download_bytes(entry.disk_bytes));
            row.failed = true;
            ++recovered;
            download_rows.push_back(std::move(row));
            continue;
        }
        row.image = entry.poster_path.empty() ? entry.background_path : entry.poster_path;
        if (!download_poster_full_size(entry.poster_width, entry.poster_height)) {
            // The media writer can defer private copies for a long queue. Show
            // an already-ready full cover now without touching storage here;
            // the old owned image remains the offline fallback until commit.
            const auto full_poster = g_art.peek_cached(entry.poster_url, ArtKind::DownloadPoster);
            if (!full_poster.empty()) row.image = full_poster;
        }
        row.progress = std::isfinite(entry.progress) ? float(entry.progress) : -1;
        row.complete = entry.state == DownloadState::Complete;
        row.active = entry.state == DownloadState::Downloading;
        row.failed = entry.state == DownloadState::Failed;
        row.paused = entry.state == DownloadState::Paused;
        row.playable_while_downloading = !row.complete && entry.playable_while_downloading;
        switch (entry.state) {
        case DownloadState::Complete: break;
        case DownloadState::Downloading: row.status = it ? "Download in corso" : "Downloading"; ++pending; break;
        case DownloadState::Queued: row.status = it ? "In coda" : "Queued"; ++pending; break;
        case DownloadState::Paused: row.status = it ? "In pausa" : "Paused"; break;
        case DownloadState::Waiting: row.status = it ? "In attesa del motore torrent" : "Waiting for the torrent engine"; ++pending; break;
        case DownloadState::Failed:
            row.status = (it ? "Download interrotto" : "Download interrupted") +
                (entry.error.empty() ? std::string{} : " · " + download_error_text(entry.error, it));
            break;
        }
        if (!row.complete) {
            row.size = download_bytes(entry.done);
            if (entry.total > 0) row.size += " / " + download_bytes(entry.total);
        }
        if (row.active && entry.bytes_per_second > 0 && std::isfinite(entry.bytes_per_second))
            row.speed = download_bytes(int64_t(std::min(entry.bytes_per_second, double(INT64_MAX / 2)))) + "/s";
        if (row.active) {
            row.remaining = download_remaining_text(entry.remaining_seconds, it);
            row.connections = download_connections_text(entry.connected_peers, entry.connected_seeders, it);
        }
        download_rows.push_back(std::move(row));
    }
    download_sel = std::clamp(download_sel, 0, std::max(0, int(download_rows.size()) - 1));
    for (size_t i = 0; i < download_rows.size(); ++i) if (download_rows[i].id == selected) { download_sel = int(i); break; }
    rebuild_download_gallery(*this);
    download_status.clear();
    if (download_relocation_active())
        download_status = it ? "Spostamento dei download in corso…" : "Moving downloads…";
    else if (downloads_.relocation_status().pending)
        download_status = it ? "Spostamento da completare · Riapri la cartella dei download nelle impostazioni."
                             : "Move unfinished · Reopen the download folder setting to continue.";
    else if (!downloads_.storage_readable())
        download_status = it ? "Impossibile leggere la cartella dei download." : "The download folder could not be read.";
    else if (!downloads_.storage_available())
        download_status = entries.empty()
            ? (it ? "Impossibile scrivere nella cartella dei download." : "The download folder is not writable.")
            : (it ? "Download sospesi · I file presenti restano accessibili." : "Downloads suspended · Existing files remain accessible.");
    else if (entries.empty()) download_status = it ? "I video che scarichi appariranno qui." : "Downloaded videos will appear here.";
    else {
        if (pending) download_status = std::to_string(pending) +
            (it ? " in coda o in download" : " queued or downloading");
        if (recovered) download_status += (download_status.empty() ? "" : " · ") +
            std::to_string(recovered) + (it ? " da verificare" : " needing attention");
    }
    dirty_all();
}

void App::downloads_button(Btn button) {
    const bool it = settings_.ui_language == "it";
    if (download_relocation_active()) return;
    if (download_series_id.empty()) {
        if (button == Btn::Circle) { set_view("home"); return; }
        if (button == Btn::L2 || button == Btn::R2) {
            settings_.download_sort = (settings_.download_sort + (button == Btn::L2 ? 2 : 1)) % 3;
            rebuild_download_gallery(*this);
            save_settings(); dirty_all();
            return;
        }
        if (download_groups.empty()) return;
        if (button == Btn::Left || button == Btn::Right || button == Btn::Up || button == Btn::Down) {
            const int step = button == Btn::Left || button == Btn::Up ? -1 : 1;
            download_group_sel = std::clamp(download_group_sel + step, 0, int(download_groups.size()) - 1);
            download_sel = download_groups[static_cast<size_t>(download_group_sel)].representative;
            dirty_all();
            return;
        }
        const auto& group = download_groups[static_cast<size_t>(download_group_sel)];
        if (group.series && (button == Btn::Cross || button == Btn::Options || button == Btn::Square)) {
            // A series opens its inventory first. Deletion always targets an
            // explicitly selected episode, never an unseen season or queue.
            download_series_id = group.id;
            download_season = download_sel >= 0 && download_sel < int(download_rows.size())
                ? download_rows[static_cast<size_t>(download_sel)].season : -1;
            select_download_season(*this, true);
            dirty_all();
            return;
        }
    } else {
        if (button == Btn::Circle) {
            download_series_id.clear(); download_episode_rows.clear();
            if (!download_groups.empty()) download_sel = download_groups[static_cast<size_t>(download_group_sel)].representative;
            dirty_all(); return;
        }
        const auto group = std::find_if(download_groups.begin(), download_groups.end(),
            [&](const UiDownloadGroup& value) { return value.id == download_series_id; });
        if (group == download_groups.end()) { downloads_refresh(); return; }
        if (button == Btn::L1 || button == Btn::R1) {
            const auto season = std::find(group->seasons.begin(), group->seasons.end(), download_season);
            const int index = season == group->seasons.end() ? 0 : int(season - group->seasons.begin());
            const int next = std::clamp(index + (button == Btn::L1 ? -1 : 1), 0, int(group->seasons.size()) - 1);
            if (next != index) { download_season = group->seasons[static_cast<size_t>(next)]; select_download_season(*this, false); }
            dirty_all(); return;
        }
        if (button == Btn::Up || button == Btn::Down) {
            const auto selected = std::find(download_episode_rows.begin(), download_episode_rows.end(), download_sel);
            const int index = selected == download_episode_rows.end() ? 0 : int(selected - download_episode_rows.begin());
            if (!download_episode_rows.empty())
                download_sel = download_episode_rows[static_cast<size_t>(std::clamp(index + (button == Btn::Up ? -1 : 1),
                    0, int(download_episode_rows.size()) - 1))];
            dirty_all(); return;
        }
        if (button != Btn::Cross && button != Btn::Square && button != Btn::Options) return;
    }
    if ((button == Btn::Cross || button == Btn::Square || button == Btn::Options) &&
             download_sel >= 0 && download_sel < int(download_rows.size())) {
        const auto id = download_rows[download_sel].id;
        const auto entry = downloads_.find(id);
        if (!entry) { downloads_refresh(); return; }
        if (downloads_.relocation_status().pending &&
            (button == Btn::Square || entry->state != DownloadState::Complete)) {
            show_toast(pending_relocation_text(it), 7);
            return;
        }
        if (entry->recovery_only && button != Btn::Square) {
            show_toast(it ? "I file sono ancora presenti, ma le informazioni del download non sono leggibili. Puoi eliminarli con Quadrato." :
                           "The files are still present, but their download information could not be read. Press Square to delete them.", 7);
            return;
        }
        if (button == Btn::Options) {
            play_download(id, entry->state != DownloadState::Complete);
            return;
        } else if (button == Btn::Cross) {
            if (entry->state == DownloadState::Complete) { play_download(id); return; }
            if (entry->state == DownloadState::Paused || entry->state == DownloadState::Failed) downloads_.resume(id);
            else downloads_.pause(id);
            downloads_refresh();
        } else if (button == Btn::Square) {
            const auto title = entry->recovery_only ? download_rows[download_sel].title :
                entry->title + (entry->subtitle.empty() ? "" : " · " + entry->subtitle);
            open_dropdown((it ? "Eliminare " : "Delete ") + title + "?",
                {it ? "Annulla" : "Cancel", it ? "Elimina download e file" : "Delete download and files"}, 0,
                [this, id](int selected) {
                    if (selected != 1) return;
                    if (download_relocation_active()) {
                        show_toast(settings_.ui_language == "it" ? "Spostamento dei download in corso." : "Moving downloads. Please wait.");
                        return;
                    }
                    if (downloads_.relocation_status().pending) {
                        show_toast(pending_relocation_text(settings_.ui_language == "it"), 7);
                        return;
                    }
                    std::string error;
                    if (!downloads_.remove(id, error)) {
                        dlog("Download removal failed: %s", error.c_str());
                        show_toast(settings_.ui_language == "it" ? "Impossibile eliminare il download." : "Could not delete the download.");
                    }
                    downloads_refresh();
                });
        }
    }
    dirty_all();
}

void App::play_download(const std::string& id, bool progressive) {
    if (download_relocation_active()) {
        show_toast(settings_.ui_language == "it" ? "Spostamento dei download in corso." : "Moving downloads. Please wait.");
        return;
    }
    const auto entry = downloads_.find(id);
    if (downloads_.relocation_status().pending && (!entry || entry->state != DownloadState::Complete)) {
        show_toast(pending_relocation_text(settings_.ui_language == "it"), 7);
        return;
    }
    if (entry && entry->state == DownloadState::Complete) progressive = false;
    double resume = 0;
    if (entry) {
        const auto* saved = saved_download_progress(*entry, video_progress_, progress_);
        if (saved && resumable_progress(*saved, entry->video_id) &&
            (!progressive || (entry->playable_while_downloading &&
                progressive_resume_proven(*saved, entry->id, entry->source_identity, entry->done, entry->total))))
            resume = saved->time;
    }
    if (resume > 0) {
        const bool it = settings_.ui_language == "it";
        const auto generation = account_generation_;
        open_dropdown(entry->title + (entry->subtitle.empty() ? "" : " · " + entry->subtitle),
            {it ? "Riproduci dall'inizio" : "Play from beginning",
             (it ? "Riproduci da " : "Resume from ") + format_time(resume)}, 1,
            [this, id, resume, generation, progressive](int choice) {
                if (generation != account_generation_ || choice < 0 || choice > 1) return;
                start_download_playback(id, progressive, choice == 0 ? 0 : resume);
            });
        return;
    }
    // A byte percentage cannot prove a timestamp. An incomplete download may
    // resume only an actually viewed position whose saved prefix is present.
    start_download_playback(id, progressive, 0);
}

void App::start_download_playback(const std::string& id, bool progressive, double start) {
    if (download_relocation_active()) {
        show_toast(settings_.ui_language == "it" ? "Spostamento dei download in corso." : "Moving downloads. Please wait.");
        return;
    }
    const auto entry = downloads_.find(id);
    if (downloads_.relocation_status().pending && (!entry || entry->state != DownloadState::Complete)) {
        show_toast(pending_relocation_text(settings_.ui_language == "it"), 7);
        return;
    }
    if (entry && entry->recovery_only) {
        show_toast(settings_.ui_language == "it" ? "Le informazioni di questo download non sono leggibili." :
                   "This download's information could not be read.", 4);
        return;
    }
    if (entry && entry->state == DownloadState::Complete) progressive = false;
    std::shared_ptr<GrowingFilePlayback> growing;
    std::string error;
    if (entry && progressive) growing = downloads_.open_progressive(id, error);
    if (progressive && !growing) {
        dlog("download playback: not ready (%s)", error.c_str());
        show_toast(settings_.ui_language == "it" ?
            "Puoi iniziare a guardare dal 5% di un download in corso." :
            "You can start watching from 5% of an active download.", 4);
        downloads_refresh();
        return;
    }
    if (progressive && start > 0) {
        const auto* saved = saved_download_progress(*entry, video_progress_, progress_);
        const auto prefix = growing->state->snapshot();
        if (!saved || !resumable_progress(*saved, entry->video_id) || start > saved->time ||
            prefix.generation != growing->generation || prefix.total != growing->total ||
            !progressive_resume_proven(*saved, entry->id, entry->source_identity, prefix.available, prefix.total)) {
            dlog("download playback: saved position waits for its verified prefix");
            start = 0;
        }
    }
    if (!entry || (!progressive && (entry->state != DownloadState::Complete || entry->local_path.empty() || !file_exists(entry->local_path)))) {
        show_toast(settings_.ui_language == "it" ? "Il file scaricato non è disponibile." : "The downloaded file is unavailable.");
        downloads_refresh();
        return;
    }
    const auto* saved = saved_download_progress(*entry, video_progress_, progress_);
    const bool hold_bookmark = progressive && start == 0 && saved && resumable_progress(*saved, entry->video_id) &&
        !progressive_resume_proven(*saved, entry->id, entry->source_identity, growing->state->snapshot().available, growing->total);
    if (watching_) watch_stop(false);
    w_offline_ = true; w_download_id_ = id;
    w_growing_download_ = std::move(growing);
    w_download_poster_ = entry->poster_path;
    w_download_background_ = entry->background_path;
    w_download_logo_ = entry->logo_path;
    w_item_ = {};
    w_item_.id = entry->media_id; w_item_.type = entry->type; w_item_.name = entry->title;
    w_item_.poster = entry->poster_url;
    w_video_id_ = entry->video_id;
    const auto path = w_growing_download_ ? w_growing_download_->path : entry->local_path;
    w_stream_ = {}; w_stream_.kind = StreamKind::Direct; w_stream_.url = path;
    w_stream_.filename = entry->title;
    w_transcode_ = false;
    t_visible = false;
    autoplay_pending_ = false;
    launch_title = entry->title; launch_image = entry->background_path; launch_logo = entry->logo_path;
    watch_start(path, {}, start, entry->title, entry->subtitle, false);
    w_hold_download_bookmark_ = hold_bookmark;
}
