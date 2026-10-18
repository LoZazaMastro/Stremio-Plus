// Detail page: full metadata, a seasonal episode carousel, and usable sources.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "app.h"
#include "download_metadata.h"
#include "download_text.h"
#include "http.h"
#include "stream_presentation.h"
#include "torrent/engine.h"
#include "torrent/torrent_stream.h"

static std::string random_id() {
	static const char* hex = "0123456789abcdef";
	std::string s;
	uint64_t x = uint64_t(now_epoch_ms()) * 6364136223846793005ULL + uint64_t(rand());
	for (int i = 0; i < 16; i++) {
		s += hex[x & 15];
		x = x * 6364136223846793005ULL + 1442695040888963407ULL;
	}
	return s;
}

std::string App::episode_label(const Video& v) const {
	std::string t = v.title.empty() ? (settings_.ui_language == "it" ? "Episodio" : "Episode") : v.title;
	if (v.episode > 0) return std::to_string(v.episode) + ". " + t;
	return t;
}

// A missing season is not season zero. Keep unnumbered uploads/episodes in an
// explicit episode group; only an actual season=0 declaration means specials.
static int episode_season(const Video& video) {
	if (video.season != 0 || jobj(video.raw, "season").is_number()) return video.season;
	return -1;
}

static std::string season_label(int season, bool italian) {
	if (season < 0) return italian ? "Episodi" : "Episodes";
	if (season == 0) return italian ? "Speciali" : "Specials";
	return (italian ? "Stagione " : "Season ") + std::to_string(season);
}

static bool episodic_meta(const Meta& meta, const std::string& catalog_type) {
	const bool episodic_type = meta.type == "series" || meta.type == "anime" || catalog_type == "series";
	const bool numbered = std::any_of(meta.videos.begin(), meta.videos.end(), [](const Video& video) {
		return video.season > 0 || video.episode > 0;
	});
	return !meta.is_live && !meta.videos.empty() && (episodic_type ||
	       (meta.type != "movie" && (numbered || (!meta.has_scheduled_videos && meta.videos.size() > 1))));
}

// The account's protected Local Files descriptor points to the desktop Stremio
// service, not to a PS5 file provider. Only remap this exact built-in endpoint;
// other local/custom addon addresses retain their own transport and identity.
static std::string detail_resource_url(const Addon& addon, const std::string& resource,
                                       const std::string& type, const std::string& id,
                                       const std::string& configured_server) {
	const std::string base = lower(addon.base);
	const bool desktop_local = addon.id == "org.stremio.local" &&
	    (base == "http://127.0.0.1:11470/local-addon" || base == "http://localhost:11470/local-addon" ||
	     base == "http://[::1]:11470/local-addon");
	if (!desktop_local) return addon.resource_url(resource, type, id);
	if (configured_server.empty()) return "";
	const std::string original = addon.resource_url(resource, type, id);
	if (original.size() <= addon.base.size()) return "";
	return configured_server + "/local-addon" + original.substr(addon.base.size());
}

static void label_inline_streams(Meta& meta, const std::string& name, const std::string& identity) {
	for (auto& video : meta.videos)
		for (auto& stream : video.streams) {
			stream.addon = name;
			stream.addon_url = identity.empty() ? "inline" : identity;
			for (auto& sub : stream.subtitles) sub.addon = name;
		}
}

// Catalog MetaPreview objects can contain only the latest episodes. The meta
// resource supplies the complete list; fill its missing artwork/descriptions
// and matching video fields from the preview without inventing any video IDs.
// A declared streams array is exclusive, including an explicitly empty array.
static Meta complete_catalog_meta(Meta full, const Meta& preview) {
	auto missing = [](auto& value, const auto& fallback) { if (value.empty()) value = fallback; };
	missing(full.id, preview.id); missing(full.type, preview.type); missing(full.name, preview.name);
	missing(full.poster, preview.poster); missing(full.background, preview.background); missing(full.logo, preview.logo);
	missing(full.description, preview.description); missing(full.release_info, preview.release_info);
	missing(full.runtime, preview.runtime); missing(full.imdb_rating, preview.imdb_rating); missing(full.year, preview.year);
	missing(full.genres, preview.genres); missing(full.cast, preview.cast); missing(full.directors, preview.directors);
	missing(full.released, preview.released); missing(full.language, preview.language); missing(full.country, preview.country);
	missing(full.awards, preview.awards); missing(full.website, preview.website); missing(full.links, preview.links);
	missing(full.trailers, preview.trailers); missing(full.default_video_id, preview.default_video_id);
	const auto& hints = jobj(full.raw, "behaviorHints");
	if (!hints.contains("isLive")) full.is_live = full.is_live || preview.is_live;
	if (!hints.contains("hasScheduledVideos")) full.has_scheduled_videos = preview.has_scheduled_videos;
	if (!full.raw.contains("posterShape")) full.poster_shape = preview.poster_shape;
	std::map<std::string, size_t> by_id;
	for (size_t i = 0; i < full.videos.size(); ++i) by_id.emplace(full.videos[i].id, i);
	for (const auto& old : preview.videos) {
		auto found = by_id.find(old.id);
		if (found == by_id.end()) {
			by_id.emplace(old.id, full.videos.size());
			full.videos.push_back(old);
			continue;
		}
		auto& video = full.videos[found->second];
		missing(video.title, old.title); missing(video.thumbnail, old.thumbnail); missing(video.overview, old.overview);
		missing(video.released, old.released); missing(video.start_time, old.start_time); missing(video.end_time, old.end_time);
		if (!jobj(video.raw, "season").is_number() && jobj(old.raw, "season").is_number()) {
			video.season = old.season; video.raw["season"] = old.season;
		}
		if (!jobj(video.raw, "episode").is_number() && !jobj(video.raw, "number").is_number() && old.episode > 0) {
			video.episode = old.episode; video.raw["episode"] = old.episode;
		}
		if (!video.raw.contains("available")) video.available = old.available;
		if (!video.has_inline_streams && old.has_inline_streams) {
			video.has_inline_streams = true;
			video.streams = old.streams;
			video.raw["streams"] = jobj(old.raw, "streams");
		}
	}
	return full;
}

bool App::watched(const std::string& video_id) const { return watched_.count(video_id) > 0; }

void App::toggle_watched(const std::string& video_id) {
	if (watched_.count(video_id)) {
		watched_.erase(video_id);
		show_toast("Marked as not watched");
	} else {
		watched_.insert(video_id);
		show_toast("Marked as watched");
	}
	save_progress();
}

// ---------------------------------------------------------------------------

void App::detail_open(const Item& it) {
	g_art.clear_queue();
	if (d_stream_cancel_) d_stream_cancel_->store(true);
	if (d_meta_cancel_) d_meta_cancel_->store(true);
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	d_meta_cancel_ = cancel;
	d_item_ = it;
	d_meta_ = Meta();
	d_meta_loaded_ = false;
	d_meta_pending_ = false;
	d_pending_video_id_.clear();
	d_gen_++;
	d_meta_gen_++;
	d_video_id_.clear();
	view = "detail";

	d_name = it.name;
	d_runtime = it.runtime;
	d_year = it.release_info;
	d_imdb = it.imdb_rating;
	d_genres = join(it.genres, ", ");
	d_cast.clear();
	d_directors.clear();
	d_description = it.description;
	d_background = art(it.background, ArtKind::Backdrop);
	d_logo = art(it.logo, ArtKind::LogoBox);
	art(it.logo, ArtKind::LaunchLogo);
	d_series = it.type == "series" || it.type == "anime";
	d_zone = d_series ? "episodes" : "streams";
	d_episodes.clear();
	d_season_videos_.clear();
	d_seasons_.clear();
	d_seasons.clear();
	d_season_sel = d_season_idx_ = d_episode_sel = 0;
	d_season_label.clear();
	d_streams.clear();
	d_stream_list_.clear();
	d_stream_buckets_.clear();
	d_stream_errors_.clear();
	d_stream_source_names_.clear();
	d_stream_source_urls_.clear();
	d_sources.clear();
	d_source_urls_.clear();
	d_source_answered_.clear();
	d_source_preferred_.clear();
	d_source_sel = d_stream_sel = 0;
	d_pending_ = d_answered_ = 0;
	d_streams_title = "Streams";
	d_streams_status = "Loading details...";
	detail_update_resume();

	const bool has_inline = it.inline_meta.is_object() && !it.inline_meta.empty();
	if (has_inline) {
		d_meta_ = parse_meta(it.inline_meta);
		label_inline_streams(d_meta_, it.metadata_addon, it.metadata_addon_url);
		if (d_meta_.id.empty()) d_meta_.id = it.id;
		if (d_meta_.type.empty()) d_meta_.type = it.type;
	}
	const Meta preview = d_meta_;

	// A catalog preview may contain a recent-episodes subset. Always resolve
	// the full meta resource when the account has a compatible endpoint.
	struct MetaSource { std::string name, url, identity; };
	std::vector<MetaSource> urls;
	for (auto& addon : addons_) {
		if (!addon->supports("meta", it.type, it.id)) continue;
		const std::string url = detail_resource_url(*addon, "meta", it.type, it.id, server());
		if (!url.empty()) urls.push_back({addon->name, url, addon->transport_url});
	}
	std::stable_partition(urls.begin(), urls.end(), [&](const MetaSource& source) {
		return source.identity == it.metadata_addon_url;
	});
	if (urls.empty()) {
		if (has_inline) {
			d_meta_loaded_ = true;
			detail_apply_meta();
		} else {
			d_series = false;
			d_zone = "streams";
			detail_load_streams(it.video_id.empty() ? it.id : it.video_id);
		}
		dirty_all();
		return;
	}
	d_meta_pending_ = true;
	if (has_inline && episodic_meta(d_meta_, it.type)) {
		// The carousel can be browsed immediately. Stream-provider requests wait
		// for full metadata, unless this preview declares exclusive streams.
		d_meta_loaded_ = true;
		detail_apply_meta();
	}
	const int gen = d_meta_gen_, account_gen = account_generation_;
	struct Res {
		bool ok = false;
		Meta meta;
		std::string error, name, identity;
	};
	bg<Res>(
	    [urls, cancel]() {
		    Res r;
		    for (const auto& source : urls) {
			    if (cancel->load()) break;
			    json response;
			    std::string error;
			    if (!fetch_json(source.url, response, error, 20, cancel.get())) {
				    r.error = error;
				    if (error != "cancelled") dlog("metadata provider %s: %s", source.name.c_str(), error.c_str());
				    continue;
			    }
			    const json& meta = jobj(response, "meta");
			    if (!meta.is_object() || meta.empty()) continue;
			    r.meta = parse_meta(meta);
			    r.name = source.name;
			    r.identity = source.identity;
			    label_inline_streams(r.meta, r.name, r.identity);
			    r.ok = true;
			    break;
		    }
		    return r;
	    },
	    [this, gen, account_gen, preview, has_inline](Res& r) {
		    if (gen != d_meta_gen_ || account_gen != account_generation_ || view != "detail") return;
		    d_meta_pending_ = false;
		    const bool preserve = d_meta_loaded_;
		    const bool in_streams = preserve && d_zone == "streams";
		    const bool open_pending = in_streams && !d_pending_video_id_.empty() && d_pending_video_id_ == d_video_id_;
		    const std::string pending_video = d_pending_video_id_;
		    const std::string previous_status = d_streams_status, previous_title = d_streams_title;
		    std::string selected_video;
		    const int selected_season = d_season_idx_ >= 0 && d_season_idx_ < int(d_seasons_.size())
		        ? d_seasons_[d_season_idx_] : INT_MIN;
		    if (d_episode_sel >= 0 && d_episode_sel < int(d_season_videos_.size()))
			    selected_video = d_season_videos_[d_episode_sel]->id;
		    d_pending_video_id_.clear();
		    if (!r.ok) {
			    if (has_inline) {
				    // Missing/offline meta endpoints never erase a usable catalog
				    // carousel or its explicit inline stream contract.
				    if (!preserve) {
					    d_meta_ = preview;
					    d_meta_loaded_ = true;
					    detail_apply_meta();
				    } else if (open_pending) detail_load_streams(pending_video);
			    } else {
				    if (!r.error.empty() && r.error != "cancelled") show_toast("Could not load details: " + r.error);
				    d_series = false;
				    d_zone = "streams";
				    detail_load_streams(d_item_.video_id.empty() ? d_item_.id : d_item_.video_id);
			    }
			    dirty_all();
			    return;
		    }
		    d_meta_ = complete_catalog_meta(std::move(r.meta), preview);
		    d_item_.metadata_addon = r.name;
		    d_item_.metadata_addon_url = r.identity;
		    if (d_meta_.id.empty()) d_meta_.id = d_item_.id;
		    if (d_meta_.type.empty()) d_meta_.type = d_item_.type;
		    d_meta_loaded_ = true;
		    // Pointers into the provisional metadata are now invalid. Rebuild
		    // them before restoring the focused episode by its exact identity.
		    d_season_videos_.clear();
		    detail_apply_meta();
		    if (preserve && d_series) {
			    int season = selected_season;
			    for (const auto& video : d_meta_.videos)
				    if (video.id == selected_video) { season = episode_season(video); break; }
			    const auto found = std::find(d_seasons_.begin(), d_seasons_.end(), season);
			    if (found != d_seasons_.end()) detail_set_season(int(found - d_seasons_.begin()));
			    for (size_t i = 0; i < d_season_videos_.size(); ++i)
				    if (d_season_videos_[i]->id == selected_video) d_episode_sel = int(i);
			    if (in_streams) {
				    d_zone = "streams";
				    if (open_pending) detail_load_streams(pending_video);
				    else { d_streams_status = previous_status; d_streams_title = previous_title; }
			    }
			    refresh_images();
			    dirty_all();
		    }
	    });
	dirty_all();
}

void App::detail_apply_meta() {
	const Meta& m = d_meta_;
	if (!m.name.empty()) d_name = m.name;
	d_runtime = m.runtime;
	d_year = m.release_info;
	d_imdb = m.imdb_rating;
	d_genres = join(m.genres, ", ");
	std::vector<std::string> cast = m.cast;
	if (cast.size() > 6) cast.resize(6);
	d_cast = join(cast, ", ");
	d_directors = join(m.directors, ", ");
	d_description = m.description;
	if (d_item_.poster.empty()) d_item_.poster = m.poster;
	if (d_item_.background.empty()) d_item_.background = m.background;
	if (d_item_.logo.empty()) d_item_.logo = m.logo;
	if (d_item_.name.empty()) d_item_.name = m.name;
	d_background = art(m.background.empty() ? d_item_.background : m.background, ArtKind::Backdrop);
	d_logo = art(m.logo.empty() ? d_item_.logo : m.logo, ArtKind::LogoBox);
	art(m.logo.empty() ? d_item_.logo : m.logo, ArtKind::LaunchLogo);

	// Series, anime and custom episodic catalogs share the carousel. EPG/live
	// programme IDs remain separate from the live channel's playback identity.
	if (episodic_meta(m, d_item_.type)) {
		d_series = true;
		d_seasons_.clear();
		d_seasons.clear();
		for (auto& v : m.videos) {
			const int season = episode_season(v);
			if (std::find(d_seasons_.begin(), d_seasons_.end(), season) == d_seasons_.end()) d_seasons_.push_back(season);
		}
		// Numbered seasons, specials, then episodes without a season declaration.
		std::stable_sort(d_seasons_.begin(), d_seasons_.end(), [](int a, int b) {
			const int ar = a > 0 ? 0 : a == 0 ? 1 : 2;
			const int br = b > 0 ? 0 : b == 0 ? 1 : 2;
			return ar != br ? ar < br : a < b;
		});
		for (const int season : d_seasons_) d_seasons.push_back({season_label(season, settings_.ui_language == "it")});
		// Start on the season being watched, else the first.
		int season_idx = 0;
		std::string resume_vid = d_item_.video_id;
		for (auto& cw : continue_watching())
			if (cw.id == d_item_.id) resume_vid = cw.video_id;
		if (resume_vid.empty() && progress_.count(d_item_.id)) resume_vid = progress_[d_item_.id].video_id;
		if (resume_vid.empty()) resume_vid = m.default_video_id;
		for (auto& v : m.videos)
			if (v.id == resume_vid) {
				for (size_t i = 0; i < d_seasons_.size(); i++)
					if (d_seasons_[i] == episode_season(v)) season_idx = int(i);
			}
		d_zone = "episodes";
		detail_set_season(season_idx);
		if (!resume_vid.empty())
			for (size_t i = 0; i < d_season_videos_.size(); i++)
				if (d_season_videos_[i]->id == resume_vid) d_episode_sel = int(i);
		d_streams_status.clear();
		// defaultVideoId only chooses focus. Every season remains reachable with
		// L1/R1 before opening an episode's streams.
	} else {
		d_series = false;
		d_seasons.clear();
		d_zone = "streams";
		std::string vid = m.default_video_id;
		if (m.is_live || m.has_scheduled_videos) vid = m.id;  // programme IDs are not channel stream IDs
		if (vid.empty()) vid = m.videos.size() == 1 ? m.videos[0].id : m.id;
		if (vid.empty()) vid = d_item_.video_id.empty() ? d_item_.id : d_item_.video_id;
		detail_load_streams(vid);
	}
	detail_update_resume();
	refresh_images();
	dirty_all();
}

void App::detail_set_season(int idx) {
	if (d_seasons_.empty()) return;
	idx = std::max(0, std::min(idx, int(d_seasons_.size()) - 1));
	d_pending_video_id_.clear();
	d_season_sel = d_season_idx_ = idx;
	int season = d_seasons_[idx];
	d_season_label = season_label(season, settings_.ui_language == "it");
	d_season_videos_.clear();
	std::set<std::string> seen;
	for (auto& v : d_meta_.videos)
		if (episode_season(v) == season && seen.insert(v.id).second) d_season_videos_.push_back(&v);
	std::stable_sort(d_season_videos_.begin(), d_season_videos_.end(), [](const Video* a, const Video* b) {
		if ((a->episode > 0) != (b->episode > 0)) return a->episode > 0;
		return a->episode > 0 && b->episode > 0 && a->episode < b->episode;
	});
	d_episodes.clear();
	int first_unwatched = -1;
	for (size_t i = 0; i < d_season_videos_.size(); i++) {
		const Video* v = d_season_videos_[i];
		UiEpisode e;
		e.title = v->title.empty() ? (settings_.ui_language == "it" ? "Episodio" : "Episode") : v->title;
		e.number = v->episode > 0 ? std::to_string(v->episode) : "";
		e.description = v->overview;
		std::string released = v->released.size() >= 10 ? v->released.substr(0, 10) : v->released;
		e.sub = released;
		e.watched = watched(v->id);
		e.thumb = art(v->thumbnail.empty() ? d_item_.background : v->thumbnail, ArtKind::Thumb, i < 8);
		if (!e.watched && first_unwatched < 0) first_unwatched = int(i);
		d_episodes.push_back(e);
	}
	d_episode_sel = first_unwatched >= 0 ? first_unwatched : 0;
	refresh_images();
	dirty_all();
}

const Video* App::detail_next_episode(const std::string& current_id) const {
	// Work from complete metadata and the same ordering used by the carousel;
	// offering the next episode must not change the current season/selection.
	const auto current = std::find_if(d_meta_.videos.begin(), d_meta_.videos.end(),
	    [&](const Video& video) { return video.id == current_id; });
	if (current == d_meta_.videos.end() || current_id.empty()) return nullptr;
	const int season = episode_season(*current);
	auto episodes = [&](int selected_season) {
		std::vector<const Video*> result;
		std::set<std::string> seen;
		for (const auto& video : d_meta_.videos)
			if (!video.id.empty() && episode_season(video) == selected_season && seen.insert(video.id).second)
				result.push_back(&video);
		std::stable_sort(result.begin(), result.end(), [](const Video* a, const Video* b) {
			if ((a->episode > 0) != (b->episode > 0)) return a->episode > 0;
			return a->episode > 0 && b->episode > 0 && a->episode < b->episode;
		});
		return result;
	};
	const auto videos = episodes(season);
	for (size_t i = 0; i < videos.size(); ++i)
		if (videos[i]->id == current_id && i + 1 < videos.size()) return videos[i + 1];
	if (season <= 0) return nullptr;
	int next_season = INT_MAX;
	for (const auto& video : d_meta_.videos) {
		const int candidate = episode_season(video);
		if (!video.id.empty() && candidate > season && candidate < next_season) next_season = candidate;
	}
	if (next_season == INT_MAX) return nullptr;
	const auto next = episodes(next_season);
	return next.empty() ? nullptr : next.front();
}

bool App::detail_select_episode(const std::string& id) {
	const auto video = std::find_if(d_meta_.videos.begin(), d_meta_.videos.end(),
	    [&](const Video& candidate) { return candidate.id == id; });
	if (video == d_meta_.videos.end()) return false;
	const auto season = std::find(d_seasons_.begin(), d_seasons_.end(), episode_season(*video));
	if (season == d_seasons_.end()) return false;
	detail_set_season(int(season - d_seasons_.begin()));
	for (size_t i = 0; i < d_season_videos_.size(); ++i) {
		if (d_season_videos_[i]->id != id) continue;
		d_episode_sel = int(i);
		return true;
	}
	return false;
}

void App::detail_update_resume() {
	d_resume.clear();
	for (auto& cw : continue_watching()) {
		if (cw.id != d_item_.id || cw.offset <= 0) continue;
		if (cw.type == "series" || split(cw.video_id, ':').size() >= 3) {
			auto parts = split(cw.video_id, ':');
			if (parts.size() >= 3)
				d_resume = "Resume S" + parts[parts.size() - 2] + "E" + parts.back() + " at " + format_time(cw.offset);
			else d_resume = "Resume at " + format_time(cw.offset);
		} else {
			d_resume = "Resume at " + format_time(cw.offset);
		}
	}
}

// Not-found/not-implemented responses describe an absent resource, not a
// provider outage. Authentication, rate limits, network and server failures
// retain their useful diagnostics. Do not inspect provider messages or URLs.
static bool absent_stream_resource(const std::string& error) {
	for (const char* code : {"404", "501"}) {
		const std::string http = std::string("HTTP ") + code;
		if (error == http || starts_with(error, http + " ")) return true;
		if (error == std::string("addon reported an error (code ") + code + ")") return true;
	}
	return error == "cancelled";
}

void App::detail_load_streams(const std::string& video_id) {
	const int gen = ++d_gen_, account_gen = account_generation_;
	if (d_stream_cancel_) d_stream_cancel_->store(true);
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	d_stream_cancel_ = cancel;
	d_video_id_ = video_id;
	d_pending_video_id_.clear();
	d_stream_list_.clear();
	d_stream_buckets_.clear();
	d_stream_errors_.clear();
	d_stream_source_names_.clear();
	d_stream_source_urls_.clear();
	d_streams.clear();
	d_stream_sel = 0;
	d_pending_ = d_answered_ = 0;
	d_sources.clear();
	d_source_urls_.clear();
	d_source_answered_.clear();
	d_source_sel = 0;
	d_view_.clear();
	d_pick_res = false;
	d_res_ = "All sources";
	d_res_sel_ = 0;

	std::string type = d_meta_loaded_ ? d_meta_.type : d_item_.type;
	if (type.empty()) type = d_item_.type;
	d_streams_base_title_ = "Streams";
	if (d_series) {
		for (auto* video : d_season_videos_)
			if (video->id == video_id)
				d_streams_base_title_ = video->season > 0 && video->episode > 0
				    ? "S" + std::to_string(video->season) + "E" + std::to_string(video->episode) +
				      (video->title.empty() ? "" : " · " + video->title) : episode_label(*video);
	}
	d_streams_title = d_streams_base_title_;
	if (video_id.empty()) {
		autoplay_pending_ = false;
		d_streams_status = "This item does not identify a playable video.";
		dirty_all();
		return;
	}
	for (const auto& video : d_meta_.videos) {
		if (video.id != video_id || !video.has_inline_streams) continue;
		d_stream_list_ = video.streams;
		std::string identity = d_item_.metadata_addon_url.empty() ? "inline" : d_item_.metadata_addon_url;
		std::string name = d_item_.metadata_addon.empty() ? "Metadata" : d_item_.metadata_addon;
		// A full meta response may have inherited an exclusive stream array
		// from a different catalog provider. Keep that original installation.
		if (!d_stream_list_.empty()) {
			if (!d_stream_list_.front().addon_url.empty()) identity = d_stream_list_.front().addon_url;
			if (!d_stream_list_.front().addon.empty()) name = d_stream_list_.front().addon;
		}
		for (auto& stream : d_stream_list_)
			if (stream.addon_url.empty()) stream.addon_url = identity;
		d_stream_buckets_ = {d_stream_list_};
		d_stream_source_names_ = {name};
		d_stream_source_urls_ = {identity};
		d_source_answered_ = {true};
		d_pending_ = d_answered_ = 1;
		detail_refresh_streams();
		if (video.streams.empty()) d_streams_status = "The metadata addon provided no streams for this video.";
		return;
	}
	if (d_meta_pending_) {
		// Keep the exact Cross intent. The full meta response may carry an
		// exclusive (even empty) streams array, so no provider request can race
		// it. Circle/season/episode navigation clears this pending identity.
		d_pending_video_id_ = video_id;
		d_streams_status = "Loading details...";
		dirty_all();
		return;
	}

	struct Source { std::string name, url, identity; };
	std::vector<Source> sources;
	for (auto& addon : addons_) {
		if (!addon->supports("stream", type, video_id)) continue;
		const std::string url = detail_resource_url(*addon, "stream", type, video_id, server());
		if (!url.empty()) sources.push_back({addon->name, url, addon->transport_url});
	}
	for (const auto& source : sources) {
		d_stream_source_names_.push_back(source.name);
		d_stream_source_urls_.push_back(source.identity);
		d_source_answered_.push_back(false);
	}
	d_pending_ = int(sources.size());
	d_stream_buckets_.resize(sources.size());
	d_stream_errors_.resize(sources.size());
	if (sources.empty()) {
		autoplay_pending_ = false;
		d_streams_status = addons_loading_
		                       ? "Addons are still loading"
		                       : "None of your addons provide streams for this. Configure stream addons in your Stremio account.";
		dirty_all();
		return;
	}
	d_streams_status = "Loading streams... (0 of " + std::to_string(sources.size()) + " addons answered)";
	for (size_t source_idx = 0; source_idx < sources.size(); source_idx++) {
		const auto& source = sources[source_idx];
		const std::string name = source.name, url = source.url, identity = source.identity;
		struct Res {
			std::vector<Stream> streams;
			std::string error;
		};
		bg<Res>(
		    [name, url, identity, cancel]() {
			    Res r;
			    json response;
			    if (fetch_json(url, response, r.error, 30, cancel.get())) r.streams = parse_streams(response, name, true);
			    else if (absent_stream_resource(r.error)) r.error.clear();
			    for (auto& stream : r.streams) stream.addon_url = identity;
			    return r;
		    },
		    [this, gen, account_gen, name, source_idx](Res& r) {
			    if (gen != d_gen_ || account_gen != account_generation_ || view != "detail" ||
			        source_idx >= d_stream_buckets_.size() || source_idx >= d_source_answered_.size()) return;
			    // Preserve selected bucket/row, not media URL or display name:
			    // configured installations may need different request headers.
			    size_t selected_bucket = d_stream_buckets_.size(), selected_offset = 0;
			    if (!d_pick_res && d_stream_sel >= 0 && d_stream_sel < int(d_view_.size()) && d_view_[d_stream_sel] >= 0) {
				    size_t index = size_t(d_view_[d_stream_sel]);
				    for (size_t bucket = 0; bucket < d_stream_buckets_.size(); bucket++) {
					    if (index < d_stream_buckets_[bucket].size()) {
						    selected_bucket = bucket;
						    selected_offset = index;
						    break;
					    }
					    index -= d_stream_buckets_[bucket].size();
				    }
			    }
			    d_answered_++;
			    d_source_answered_[source_idx] = true;
			    if (!r.error.empty()) dlog("streams from %s: %s", name.c_str(), r.error.c_str());
			    d_stream_errors_[source_idx] = r.error.empty() ? "" : name + ": " + r.error;
			    d_stream_buckets_[source_idx] = std::move(r.streams);
			    d_stream_list_.clear();
			    for (const auto& bucket : d_stream_buckets_)
				    d_stream_list_.insert(d_stream_list_.end(), bucket.begin(), bucket.end());
			    detail_refresh_streams();
			    if (!d_pick_res && selected_bucket < d_stream_buckets_.size() &&
			        selected_offset < d_stream_buckets_[selected_bucket].size()) {
				    size_t index = selected_offset;
				    for (size_t bucket = 0; bucket < selected_bucket; bucket++) index += d_stream_buckets_[bucket].size();
				    const auto row = std::find(d_view_.begin(), d_view_.end(), int(index));
				    if (row != d_view_.end()) d_stream_sel = int(row - d_view_.begin());
			    }
		    });
	}
	dirty_all();
}

// A stream's resolution, from what addons put in its name and title
// (Torrentio: "Torrentio\n4k DV | HDR", "...2160p...", "1080p", ...).
static const char* const kResOrder[] = {"All sources", "4K", "1440p", "1080p", "720p", "SD", "Other"};

static std::string stream_resolution(const Stream& st) {
	std::string s = lower(st.name + " " + st.description + " " + st.filename);
	auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
	if (has("2160p") || has("4k") || has("uhd")) return "4K";
	if (has("1440p") || has("2k")) return "1440p";
	if (has("1080p") || has("1080i") || has("fhd")) return "1080p";
	if (has("720p")) return "720p";
	if (has("576p") || has("480p") || has("360p") || has("dvdrip") || has("dvdscr")) return "SD";
	return "Other";
}

void App::detail_refresh_streams() {
	d_streams.clear();
	// A manifest declares capability, not an available source for this video.
	// Only completed buckets containing a native-playable stream become tabs.
	// The raw buckets still retain external/unsupported descriptors and reasons.
	std::vector<size_t> usable;
	std::map<std::string, int> names, instances;
	for (size_t bucket = 0; bucket < d_stream_buckets_.size(); ++bucket) {
		if (bucket >= d_stream_source_urls_.size() || bucket >= d_stream_source_names_.size()) continue;
		const bool has_stream = std::any_of(d_stream_buckets_[bucket].begin(), d_stream_buckets_[bucket].end(),
		                                   [](const Stream& stream) { return stream.playable(); });
		if (!has_stream) continue;
		usable.push_back(bucket);
		++names[d_stream_source_names_[bucket]];
	}
	d_sources.clear();
	d_source_urls_.clear();
	if (usable.size() > 1) {
		d_sources.push_back({settings_.ui_language == "it" ? "Tutte le sorgenti" : "All sources"});
		d_source_urls_.push_back("");
	}
	for (const size_t bucket : usable) {
		const std::string& name = d_stream_source_names_[bucket];
		std::string label = name;
		if (names[name] > 1) label += " · " + std::to_string(++instances[name]);
		d_sources.push_back({label});
		d_source_urls_.push_back(d_stream_source_urls_[bucket]);
	}
	d_source_sel = 0;
	const auto preferred = std::find(d_source_urls_.begin(), d_source_urls_.end(), d_source_preferred_);
	if (preferred != d_source_urls_.end()) d_source_sel = int(preferred - d_source_urls_.begin());
	// Preserve each addon's ranking and the account's addon order. Quality is
	// an optional filter, not a forced reordering of AIOStreams/Torrentio.
	std::map<std::string, std::vector<int>> groups;
	const std::string source = d_source_sel >= 0 && d_source_sel < int(d_source_urls_.size())
	    ? d_source_urls_[d_source_sel] : "";
	for (size_t i = 0; i < d_stream_list_.size(); i++) {
		if (!d_stream_list_[i].playable()) continue;
		if (!source.empty() && d_stream_list_[i].addon_url != source) continue;
		groups["All sources"].push_back(int(i));
		groups[stream_resolution(d_stream_list_[i])].push_back(int(i));
	}

	if (d_pick_res) {
		d_res_keys_.clear();
		d_view_.clear();
		for (const char* key : kResOrder) {
			auto g = groups.find(key);
			if (g == groups.end()) continue;
			std::vector<std::string> addons;
			for (int i : g->second) {
				const std::string& a = d_stream_list_[i].addon;
				if (std::find(addons.begin(), addons.end(), a) == addons.end()) addons.push_back(a);
			}
			UiStream u;
			u.name = key;
			u.addon = std::to_string(g->second.size()) + (g->second.size() == 1 ? " stream" : " streams");
			u.desc = "From " + join(addons, ", ");
			d_streams.push_back(u);
			d_res_keys_.push_back(key);
		}
		d_streams_title = d_streams_base_title_ + (d_streams.empty() ? "" : " · Choose a quality");
	} else {
		d_view_ = groups[d_res_];
		for (int i : d_view_) {
			const Stream& st = d_stream_list_[i];
			UiStream u;
			u.name = st.name.empty() ? (st.filename.empty() ? "Stream" : st.filename) : st.name;
			u.addon = st.addon;
			u.desc = st.description.empty() ? st.filename : st.description;
			const auto presentation = stream_presentation(st);
			u.languages = presentation.languages;
			u.seeders = presentation.seeders;
			u.type = presentation.type;
			u.quality = presentation.quality;
			u.size = presentation.size;
			u.cached = presentation.cached;
			if (!st.playable()) {
				u.name = "Unavailable · " + u.name;
				u.desc += (u.desc.empty() ? "" : "\n") + st.unsupported_reason;
			}
			d_streams.push_back(u);
		}
		d_streams_title = d_streams_base_title_ + (d_res_ == "All sources" ? "" : " · " + d_res_);
	}
	if (d_stream_sel >= int(d_streams.size())) d_stream_sel = std::max(0, int(d_streams.size()) - 1);

	if (autoplay_pending_ && d_answered_ >= d_pending_) {
		// Next episode: same binge group, else same addon; first stream once all answered.
		int pick = -1;
		for (size_t i = 0; i < d_stream_list_.size() && pick < 0; i++)
			if (d_stream_list_[i].playable() && !autoplay_binge_.empty() &&
			    d_stream_list_[i].binge_group == autoplay_binge_) pick = int(i);
		bool all = d_answered_ >= d_pending_;
		if (pick < 0 && all)
			for (size_t i = 0; i < d_stream_list_.size() && pick < 0; i++)
				if (d_stream_list_[i].playable() &&
				    (d_stream_list_[i].addon_url.empty() ? d_stream_list_[i].addon : d_stream_list_[i].addon_url) == autoplay_addon_)
					pick = int(i);
		if (pick < 0 && all)
			for (size_t i = 0; i < d_stream_list_.size() && pick < 0; i++)
				if (d_stream_list_[i].playable()) pick = int(i);
		if (pick >= 0) {
			autoplay_pending_ = false;
			const auto row = std::find(d_view_.begin(), d_view_.end(), pick);
			d_stream_sel = row == d_view_.end() ? 0 : int(row - d_view_.begin());
			play_stream(d_stream_list_[pick], false);
		} else if (all) {
			autoplay_pending_ = false;
		}
	}

	const bool selected_loading = d_answered_ < d_pending_ &&
	    (d_source_preferred_.empty() || source != d_source_preferred_);
	if (selected_loading)
		d_streams_status = "Loading streams... (" + std::to_string(d_answered_) + " of " + std::to_string(d_pending_) +
		                   " addons answered)";
	else {
		std::vector<std::string> errors;
		for (size_t i = 0; i < d_stream_errors_.size(); ++i)
			if ((d_source_preferred_.empty() || (i < d_stream_source_urls_.size() && source == d_stream_source_urls_[i])) &&
			    !d_stream_errors_[i].empty())
				errors.push_back(d_stream_errors_[i]);
		if (d_streams.empty()) {
			if (usable.empty() && !d_stream_list_.empty()) {
				d_streams_status = "No compatible streams for this video.";
				std::vector<std::string> reasons;
				for (const auto& stream : d_stream_list_)
					if (!stream.unsupported_reason.empty() &&
					    std::find(reasons.begin(), reasons.end(), stream.unsupported_reason) == reasons.end())
						reasons.push_back(stream.unsupported_reason);
				if (!reasons.empty()) d_streams_status += " " + join(reasons, " · ");
			} else d_streams_status = usable.empty() ? "No streams found for this video."
			    : "No streams match the selected quality.";
		} else d_streams_status.clear();
		if (!errors.empty()) {
			if (!d_streams_status.empty()) d_streams_status += " ";
			d_streams_status += "Some providers could not answer: " + join(errors, " · ");
		}
	}
	dirty_all();
}

void App::detail_select_source(int index) {
	if (d_source_urls_.empty()) return;
	const int count = int(d_source_urls_.size());
	d_source_sel = (index % count + count) % count;
	d_source_preferred_ = d_source_urls_[d_source_sel];
	d_stream_sel = 0;
	d_pick_res = false;
	d_res_ = "All sources";
	d_res_sel_ = 0;
	detail_refresh_streams();
}

void App::detail_close() {
	d_gen_++;
	d_meta_gen_++;
	if (d_stream_cancel_) d_stream_cancel_->store(true);
	if (d_meta_cancel_) d_meta_cancel_->store(true);
	d_meta_pending_ = false;
	d_pending_video_id_.clear();
	view = return_view_;
	zone = return_zone_;
	if (view == "home") refresh_home_cards();
	if (view == "library") library_refresh();
	if (view == "discover") refresh_images();
	if (view == "search") refresh_search_cards();
	dirty_all();
}

void App::detail_button(Btn b) {
	autoplay_pending_ = false;  // any button cancels playing the next episode
	// Old saved/UI fixtures can still name the removed season-focus stage.
	if (d_zone == "seasons") d_zone = "episodes";
	if (d_zone == "episodes") {
		switch (b) {
		case Btn::L1:
		case Btn::R1:
			if (!d_seasons_.empty()) {
				const int count = int(d_seasons_.size());
				detail_set_season((d_season_idx_ + (b == Btn::L1 ? -1 : 1) + count) % count);
			}
			break;
		case Btn::Left:
			d_pending_video_id_.clear();
			if (d_episode_sel > 0) d_episode_sel--;
			refresh_images();
			break;
		case Btn::Right:
			d_pending_video_id_.clear();
			if (d_episode_sel + 1 < int(d_episodes.size())) d_episode_sel++;
			refresh_images();
			break;
		case Btn::Cross:
			if (d_episode_sel >= 0 && d_episode_sel < int(d_season_videos_.size())) {
				d_zone = "streams";
				detail_load_streams(d_season_videos_[d_episode_sel]->id);
			}
			break;
		case Btn::Triangle:
			if (d_episode_sel >= 0 && d_episode_sel < int(d_season_videos_.size())) {
				toggle_watched(d_season_videos_[d_episode_sel]->id);
				d_episodes[d_episode_sel].watched = watched(d_season_videos_[d_episode_sel]->id);
			}
			break;
		case Btn::Circle: detail_close(); return;
		default: break;
		}
		dirty_all();
		return;
	}

	// The source switcher is local to this episode. A provider's own result
	// order is preserved; an optional quality picker lives behind Options.
	switch (b) {
	case Btn::L1: detail_select_source(d_source_sel - 1); break;
	case Btn::R1: detail_select_source(d_source_sel + 1); break;
	case Btn::Up:
		if (d_stream_sel > 0) d_stream_sel--;
		break;
	case Btn::Down:
		if (d_stream_sel + 1 < int(d_streams.size())) d_stream_sel++;
		break;
	case Btn::Cross:
	case Btn::Square:
		if (d_pick_res) {
			if (b == Btn::Cross && d_stream_sel >= 0 && d_stream_sel < int(d_res_keys_.size())) {
				d_res_ = d_res_keys_[d_stream_sel];
				d_res_sel_ = d_stream_sel;
				d_pick_res = false;
				d_stream_sel = 0;
				detail_refresh_streams();
			}
		} else if (d_stream_sel >= 0 && d_stream_sel < int(d_view_.size())) {
			const auto index = d_view_[d_stream_sel];
			if (index >= 0 && index < int(d_stream_list_.size())) {
				if (b == Btn::Square) download_selected_stream(d_stream_list_[index]);
				else play_stream(d_stream_list_[index], false);
			}
		}
		break;
	case Btn::Options:
		d_pick_res = !d_pick_res;
		d_stream_sel = d_pick_res ? d_res_sel_ : 0;
		detail_refresh_streams();
		break;
	case Btn::Left:
	case Btn::Circle:
		if (d_pick_res) {
			d_pick_res = false;
			d_stream_sel = 0;
			detail_refresh_streams();
			break;
		}
		if (d_series) {
			d_gen_++;
			if (d_stream_cancel_) d_stream_cancel_->store(true);
			d_pending_video_id_.clear();
			d_zone = "episodes";
		} else {
			detail_close();
			return;
		}
		break;
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Playing

bool App::play_saved_stream(const Stream& stream) {
	DownloadRequest request;
	request.media_id = d_item_.id;
	request.type = d_item_.type;
	request.video_id = d_video_id_;
	request.stream = stream;
	download_episode_metadata(request, d_meta_loaded_ ? d_meta_ : Meta{});
	if (!stream.url.empty() && !stream.response_headers.empty() && !server().empty()) {
		request.stream.url = stream_proxy_url(stream, server());
		request.stream.request_headers.clear();
		request.stream.response_headers.clear();
		if (request.stream.url.empty()) return false;
	}
	const auto entry = downloads_.find_matching(request);
	if (!entry) return false;
	if (entry->state == DownloadState::Complete || entry->playable_while_downloading) {
		play_download(entry->id, entry->state != DownloadState::Complete);
		return true;
	}
	const bool italian = settings_.ui_language == "it";
	if (offline_ || !signed_in()) {
		show_toast(italian ? "Collegati per riprendere questo download." : "Connect to resume this download.", 5);
		return true;
	}
	if (download_relocation_active() || downloads_.relocation_status().pending) {
		show_toast(italian ? "Completa lo spostamento dei download prima di riprodurre questo video." :
		                     "Finish moving downloads before playing this video.", 6);
		return true;
	}
	if (watching_) watch_stop(false);
	if (launch_cancel_) launch_cancel_->store(true);
	launch_cancel_ = std::make_shared<std::atomic<bool>>(false);
	saved_stream_download_ = entry->id;
	saved_stream_generation_ = ++w_gen_;
	saved_stream_poll_at_ = 0;
	saved_stream_resuming_ = entry->state == DownloadState::Paused || entry->state == DownloadState::Failed;
	// The existing download keeps sole ownership of its transfer and writer.
	// Readiness is observed through published snapshots, never a second engine.
	w_offline_ = true;
	w_download_id_ = entry->id;
	w_download_poster_ = entry->poster_path;
	w_download_background_ = entry->background_path;
	w_download_logo_ = entry->logo_path;
	w_item_ = d_item_;
	w_item_.name = entry->title;
	w_video_id_ = entry->video_id;
	launch_visible = true;
	launch_title = entry->title;
	launch_image = entry->background_path;
	launch_logo = entry->logo_path;
	launch_progress = entry->progress >= 0 ? float(entry->progress) : -1;
	launch_status = italian ? "Preparazione del download..." : "Preparing download...";
	t_visible = false;
	dirty_all();
	if (saved_stream_resuming_) {
		const auto id = entry->id;
		const auto cancel = launch_cancel_;
		const int generation = saved_stream_generation_;
		bg<bool>([manager = &downloads_, id, cancel] {
			return !cancel->load() && manager->resume(id);
		}, [this, id, cancel, generation](bool& resumed) {
			if (cancel->load() || generation != w_gen_ || saved_stream_download_ != id) return;
			saved_stream_resuming_ = false;
			saved_stream_poll_at_ = 0;
			if (!resumed) {
				const auto current = downloads_.find(id);
				if (!current || current->state == DownloadState::Paused || current->state == DownloadState::Failed) {
					launch_visible = false;
					saved_stream_download_.clear();
					const bool it = settings_.ui_language == "it";
					const auto error = current && !current->error.empty() ? download_error_text(current->error, it) :
					    (it ? "Impossibile riprendere il download." : "The download could not be resumed.");
					show_toast(error, 7);
					dirty_all();
				}
			}
		});
	}
	return true;
}

void App::saved_stream_tick() {
	if (saved_stream_download_.empty()) return;
	if (saved_stream_generation_ != w_gen_ || !launch_visible ||
	    !launch_cancel_ || launch_cancel_->load()) {
		saved_stream_download_.clear();
		return;
	}
	const double now = now_seconds();
	if (saved_stream_resuming_ || now < saved_stream_poll_at_) return;
	saved_stream_poll_at_ = now + 0.25;
	const auto entry = downloads_.find(saved_stream_download_);
	const bool italian = settings_.ui_language == "it";
	if (!entry || entry->state == DownloadState::Failed || entry->state == DownloadState::Paused) {
		launch_visible = false;
		saved_stream_download_.clear();
		show_toast(entry && !entry->error.empty() ? download_error_text(entry->error, italian) :
		    (italian ? "Il download non è in corso. Riprendilo da Download." :
		               "This download is not running. Resume it from Downloads."), 7);
		dirty_all();
		return;
	}
	if (entry->state == DownloadState::Complete || entry->playable_while_downloading) {
		launch_visible = false;
		saved_stream_download_.clear();
		play_download(entry->id, entry->state != DownloadState::Complete);
		dirty_all();
		return;
	}
	launch_progress = entry->progress >= 0 ? float(entry->progress) : -1;
	if (entry->state == DownloadState::Queued || entry->state == DownloadState::Waiting)
		launch_status = italian ? "In attesa del download selezionato..." : "Waiting for the selected download...";
	else {
		const int percent = std::max(0, int(launch_progress * 100));
		launch_status = (italian ? "Preparazione del download " : "Preparing download ") + std::to_string(percent) +
		    (italian ? "% · Riproduzione dal 5%" : "% · Playback starts at 5%");
	}
	dirty("launch_progress");
	dirty("launch_status");
}

void App::play_stream(const Stream& st, bool transcode) {
	if (!st.playable()) {
		show_toast(st.unsupported_reason.empty() ? "This source cannot be played on this device." : st.unsupported_reason, 8);
		return;
	}
	if (!transcode && play_saved_stream(st)) return;
	std::string srv = server();
	bool torrent = st.url.empty() && !st.info_hash.empty();
	// Torrents play with the app's own engine, unless Settings says to use
	// the server; transcoding is always the server's.
	bool builtin = torrent && !transcode && (settings_.builtin_torrents || srv.empty());
	if (torrent && !builtin && srv.empty()) {
		show_toast("Torrent streams need your Stremio streaming server: set it in Settings.", 6);
		return;
	}
	if (transcode && srv.empty()) {
		show_toast("Transcoding needs your Stremio streaming server: set it in Settings.", 6);
		return;
	}
	w_offline_ = false;
	w_download_id_.clear();
	w_torrent_slot_ = builtin;
	downloads_.set_torrent_playback_active(builtin);

	// What's being played, for progress and the player's titles.
	w_item_ = d_item_;
	if (d_meta_loaded_) {
		w_item_.name = d_meta_.name.empty() ? d_item_.name : d_meta_.name;
		w_item_.type = d_meta_.type.empty() ? d_item_.type : d_meta_.type;
		if (!d_meta_.poster.empty()) w_item_.poster = d_meta_.poster;
		if (!d_meta_.background.empty()) w_item_.background = d_meta_.background;
		if (!d_meta_.logo.empty()) w_item_.logo = d_meta_.logo;
	}
	w_video_id_ = d_video_id_;
	w_stream_ = st;
	w_transcode_ = transcode;

	std::string title = w_item_.name, subtitle;
	if (d_series) {
		for (auto& v : d_meta_.videos)
			if (v.id == w_video_id_)
				subtitle = "S" + std::to_string(v.season) + "E" + std::to_string(v.episode) +
				           (v.title.empty() ? "" : " · " + v.title);
	} else {
		subtitle = d_year;
	}

	// Resume where it was left.
	double start = 0;
	const auto saved = video_progress_.find(video_progress_key(w_item_.type, w_item_.id, w_video_id_));
	if (saved != video_progress_.end()) {
		if (resumable_progress(saved->second, w_video_id_) &&
		    (saved->second.duration <= 0 || saved->second.time < saved->second.duration * 0.95)) start = saved->second.time;
	} else for (const auto& cw : continue_watching()) {
		if (cw.id != w_item_.id) continue;
		const bool same_video = cw.video_id == w_video_id_ || (!d_series && cw.video_id.empty());
		if (same_video && cw.offset > 5 && (cw.duration <= 0 || cw.offset < cw.duration * 0.95)) start = cw.offset;
	}

	int gen = ++w_gen_;
	launch_visible = true;
	launch_title = title;
	launch_image = art(w_item_.background, ArtKind::Background);
	launch_logo = art(w_item_.logo, ArtKind::LaunchLogo);
	launch_progress = -1;
	launch_status = builtin   ? "Finding peers..."
	                : torrent ? "Preparing torrent..."
	                : transcode ? "Starting transcoding on the server..."
	                            : "Opening stream...";
	if (start > 0) launch_status += "  Resume at " + format_time(start);
	dirty_all();

	struct Res {
		std::string url, error, filename;
		std::vector<std::string> headers;
		int file_idx = -1;
		uint64_t file_size = 0;
	};
	int season = -1, episode = -1;
	if (d_series)
		for (auto& v : d_meta_.videos)
			if (v.id == w_video_id_) season = v.season, episode = v.episode;
	if (builtin) torrent_stats_start(TorrentStream::make_url(st.info_hash, -1), st.info_hash);
	else if (torrent) torrent_stats_start(srv + "/" + st.info_hash + "/stats.json", st.info_hash);
	else t_visible = false;
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	launch_cancel_ = cancel;
	Stream s = st;
	// Keep a cancelled resolver from racing a resumed background torrent.
	// Only the worker owns the lease; its UI completion callback does not.
	auto torrent_resolution = builtin ? downloads_.acquire_torrent_resolution() : std::shared_ptr<void>{};
	bg<Res>(
	    [s, srv, torrent, builtin, transcode, season, episode, cancel, download_manager = &downloads_,
	     torrent_resolution = std::move(torrent_resolution)]() {
		    Res r;
		    std::string media = s.url;
		    if (!s.url.empty()) r.headers = s.request_headers;
		    if (builtin) {
			    if (!download_manager->wait_for_torrent_idle(*cancel)) { r.error = "cancelled"; return r; }
			    // The app's own engine: start it, wait for the file list (the
			    // peers send it when the addon gave only the hash), pick the
			    // file and play it from the engine.
			    bt::Engine& eng = bt::Engine::get();
			    if (cancel->load()) { r.error = "cancelled"; return r; }
			    eng.start(s.info_hash, s.sources);
			    std::vector<bt::FileInfo> files;
			    std::string err;
			    if (!eng.wait_metadata(s.info_hash, files, cancel.get(), 900, &err)) {
				    if (err == "cancelled") r.error = "cancelled";
				    else if (err == "timed out")
					    r.error = "No peers sent this torrent's file list in 15 minutes. Try another stream.";
				    else r.error = "The torrent couldn't start (" + err + ").";
				    return r;
			    }
			    int idx = s.file_idx;
			    if (idx >= 0 && size_t(idx) >= files.size()) {
				    r.error = "The addon selected a file index that is not in this torrent.";
				    return r;
			    }
			    if (idx < 0) idx = bt::Engine::guess_file(files, season, episode);
			    if (idx < 0 || size_t(idx) >= files.size()) {
				    r.error = "This torrent has no video file.";
				    return r;
			    }
			    dlog("torrent: playing selected file %d (%lld bytes)", idx, static_cast<long long>(files[size_t(idx)].size));
			    eng.select_file(s.info_hash, idx);
			    r.url = TorrentStream::make_url(s.info_hash, idx);
			    r.file_idx = idx;
			    r.filename = files[size_t(idx)].path;
			    auto slash = r.filename.find_last_of('/');
			    if (slash != std::string::npos) r.filename.erase(0, slash + 1);
			    if (files[size_t(idx)].size > 0) r.file_size = uint64_t(files[size_t(idx)].size);
			    return r;
		    }
		    if (torrent) {
			    // As Stremio's own player does (stremio-video createTorrent):
			    // with no trackers from the addon and a known file, play the
			    // file URL straight away and let the server find peers its
			    // own way (its default trackers and DHT). Only otherwise ask
			    // it to /create the torrent first.
			    int idx = s.file_idx;
			    std::vector<std::string> sources;
			    for (auto& x : s.sources) {
				    std::string src = starts_with(x, "tracker:") || starts_with(x, "dht:") ? x : "tracker:" + x;
				    if (std::find(sources.begin(), sources.end(), src) == sources.end()) sources.push_back(src);
			    }
			    if (!sources.empty() || idx < 0) {
				    json body;
				    body["torrent"] = {{"infoHash", s.info_hash}};
				    if (!sources.empty()) {
					    std::vector<std::string> all = {"dht:" + s.info_hash};
					    for (auto& x : sources)
						    if (x != all[0]) all.push_back(x);
					    sources = all;
					    body["peerSearch"] = {{"sources", sources}, {"min", 40}, {"max", 200}};
				    }
				    if (idx < 0) {
					    json guess = json::object();
					    if (season >= 0) guess["season"] = season;
					    if (episode >= 0) guess["episode"] = episode;
					    body["guessFileIdx"] = guess;
				    } else {
					    body["guessFileIdx"] = false;
				    }
				    // With few seeders the server may need minutes to get the
				    // torrent's metadata; the PC app waits as long as it takes,
				    // so wait 15 minutes (Circle cancels). The proxy in front
				    // of the server answers 502/503/504 when it tires of
				    // waiting: that means "not yet", so ask again.
				    const double deadline = now_seconds() + 900;
				    HttpResponse h;
				    for (;;) {
					    long left = long(deadline - now_seconds());
					    h = http_post_json(srv + "/" + s.info_hash + "/create", body.dump(), std::max(left, 1L),
					                       cancel.get());
					    bool not_yet = h.error.empty() && h.status >= 502 && h.status <= 504;
					    if (!not_yet || cancel->load() || now_seconds() + 3 >= deadline) break;
					    dlog("torrent: server answered HTTP %ld to /create, asking again", h.status);
					    for (int i = 0; i < 20 && !cancel->load(); i++) SDL_Delay(100);
				    }
				    if (cancel->load()) h.error = "cancelled";
				    if (!h.error.empty() || !h.ok()) {
					    if (h.error == "cancelled") r.error = "cancelled";
					    else if (h.error.find("timed out") != std::string::npos)
						    r.error = "Your streaming server found no peers for this torrent in 15 minutes. Try another "
						              "stream.";
					    else r.error = "Your streaming server: " + h.describe();
					    return r;
				    }
				    if (idx < 0) {
					    json j = json::parse(h.body, nullptr, false);
					    double chosen = jnum(j, "guessedFileIdx", -1);
					    if (chosen < 0 || chosen > INT_MAX || chosen != std::floor(chosen)) {
						    r.error = "The streaming server did not identify a video file in this torrent.";
						    return r;
					    }
					    idx = int(chosen);
				    }
			    }
			    media = srv + "/" + s.info_hash + "/" + std::to_string(idx);
			    std::string query;
			    for (auto& x : sources) query += (query.empty() ? "?tr=" : "&tr=") + url_encode(x);
			    media += query;
			    r.file_idx = idx;
			    r.headers.clear();
		    }
		    if (!torrent && !srv.empty() && (transcode || !s.response_headers.empty()) &&
		        (!s.request_headers.empty() || !s.response_headers.empty())) {
			    media = stream_proxy_url(s, srv);
			    if (media.empty()) { r.error = "The stream's proxy URL could not be prepared."; return r; }
			    r.headers.clear();
		    }
		    if (transcode) {
			    r.url = srv + "/hlsv2/" + random_id() + "/master.m3u8?mediaURL=" + url_encode(media) +
			            "&videoCodecs=h264&audioCodecs=aac&audioCodecs=mp3&maxAudioChannels=2";
			    r.headers.clear();
		    } else {
			    r.url = media;
		    }
		    return r;
	    },
	    [this, gen, start, title, subtitle, srv, s, builtin](Res& r) {
		    if (gen != w_gen_ || !launch_visible) return;
		    if (!r.error.empty()) {
			    if (w_torrent_slot_) { downloads_.set_torrent_playback_active(false); w_torrent_slot_ = false; }
			    launch_visible = false;
			    t_visible = false;
			    if (r.error != "cancelled") show_toast(r.error, 8);
			    dirty_all();
			    return;
		    }
		    if (builtin) t_stats_url_ = r.url;
		    else if (r.file_idx >= 0)
			    t_stats_url_ = srv + "/" + s.info_hash + "/" + std::to_string(r.file_idx) + "/stats.json";
		    if (r.file_idx >= 0) w_stream_.file_idx = r.file_idx;
		    if (w_stream_.filename.empty()) w_stream_.filename = r.filename;
		    if (!w_stream_.has_video_size && r.file_size > 0) {
			    w_stream_.video_size = r.file_size;
			    w_stream_.has_video_size = true;
		    }
		    // Several connections and the long read-ahead for big files: direct
		    // links and the streaming server's torrent streams (a single
		    // connection from the server gave 3-6 MB/s, six 8 MB/s; PS5
		    // 2026-10-06). Not for transcoding, which is a playlist of pieces.
		    bool par = r.url.find("/hlsv2/") == std::string::npos;
		    watch_start(r.url, r.headers, start, title, subtitle, par);
	    });
}
