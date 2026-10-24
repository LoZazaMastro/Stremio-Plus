#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>

#include "artcache.h"
#include "calendar_date.h"
#include "display_output.h"
#include "http.h"
#include "ui_language.h"
#include "tasks.h"
#include "torrent/engine.h"

Item item_from_meta_json(const json& j) {
	Item it;
	it.id = jstr(j, "id", jstr(j, "imdb_id"));
	it.type = jstr(j, "type");
	it.name = jstr(j, "name");
	it.poster = jstr(j, "poster");
	it.background = jstr(j, "background");
	it.logo = jstr(j, "logo");
	it.description = jstr(j, "description");
	it.release_info = jstr(j, "releaseInfo", jstr(j, "year"));
	it.runtime = jstr(j, "runtime");
	it.imdb_rating = jstr(j, "imdbRating");
	const json& g = jobj(j, "genres");
	if (g.is_array())
		for (auto& v : g)
			if (v.is_string()) it.genres.push_back(v.get<std::string>());
	const auto people = [&](const char* key, std::vector<std::string>& result) {
		const auto& values = jobj(j, key);
		if (!values.is_array()) return;
		for (const auto& value : values) {
			if (!value.is_string()) continue;
			auto name = value.get<std::string>();
			if (name.empty() || name.size() > 256) continue;
			if (std::find(result.begin(), result.end(), name) == result.end()) result.push_back(std::move(name));
			if (result.size() == 16) break;
		}
	};
	people("cast", it.cast);
	people("director", it.directors);
	if (it.directors.empty()) people("directors", it.directors);
	return it;
}

// ---------------------------------------------------------------------------
// Setup

bool App::init(const std::string& base_dir, const std::string& data_dir, bool offline) {
	offline_ = offline;
	base_dir_ = base_dir;
	data_dir_ = data_dir;
	// Rolling stream caches grow only as received data is written. Their
	// capacity is a limit, never an installation-time disk reservation.
	bt::Engine::get().configure(data_dir_, 6ll << 30);
	NetStream::set_cache_dir(data_dir_);

	system_ui_language_ = platform_ui_language();
	load_settings();
	load_progress();
	// Keep the old download directory in the inventory when selecting another
	// volume. The registry in appdata records every previously used directory.
#ifdef PLATFORM_PS5_NATIVE
	const std::string downloads_parent = "/data/Stremio";
#else
	const std::string& downloads_parent = data_dir_;
#endif
	std::string download_error;
	if (!downloads_.init(downloads_parent, data_dir_, settings_.download_directory, &download_error))
		dlog("Download storage initialization: readable=%d writable=0 error=%s",
			int(downloads_.storage_readable()), download_error.c_str());
	else
		dlog("Download storage ready: %s", downloads_.download_directory().c_str());
	settings_.download_directory = downloads_.download_directory();
	save_settings();
	downloads_.set_enabled(!offline_ && signed_in());

	refresh_clock();
	user = settings_.user_email.empty() ? "" : std::string(1, char(toupper((unsigned char)settings_.user_email[0])));
	if (!config_note_.empty()) show_toast(config_note_, 8);

	view = "home";
	zone = "content";
	settings_refresh();
	if (!offline_) {
		if (signed_in()) {
			load_addons();
			load_library();
		} else {
			login_start();
		}
	}
	dirty_all();
	return true;
}

void App::shutdown() {
	close_download_directory_picker(true);
	watch_cancel_next_episode();
	for (auto* cancel : {&home_cancel_, &search_cancel_, &disc_cancel_, &d_stream_cancel_,
	                     &d_meta_cancel_, &w_sub_cancel_, &launch_cancel_, &preview_metadata_cancel_, &download_art_cancel_})
		if (*cancel) (*cancel)->store(true);
	++addons_gen_;
	++account_generation_;
	if (watching_) watch_stop(false);
	player_.close();
	downloads_.shutdown();
	// The process owner stops the shared engine after all resolver tasks join.
	// A worker already past its cancellation check must not restart it later.
	flush_settings(true);
	save_progress();
}

void App::dirty(const char*) { all_dirty_ = true; }

void App::show_toast(const std::string& msg, double seconds) {
	toast = msg;
	++toast_revision;
	toast_until_ = now_seconds() + seconds;
	dirty("toast");
}

// ---------------------------------------------------------------------------
// Persistence

void App::load_settings() {
#ifdef PLATFORM_PS5_NATIVE
	settings_.server_url.clear();
	settings_.builtin_torrents = true;
#endif
	settings_.ui_language = resolve_ui_language(settings_.ui_language_mode, system_ui_language_);
	json j;
	if (load_json(data_dir_ + "/settings.json", j)) {
#ifndef PLATFORM_PS5_NATIVE
		settings_.server_url = jstr(j, "server_url");
		settings_.builtin_torrents = jbool(j, "builtin_torrents", settings_.builtin_torrents);
#endif
		const double torrent_profile = jnum(j, "torrent_speed_profile", 2);
		settings_.torrent_speed_profile = std::isfinite(torrent_profile) && torrent_profile >= 0 &&
		    torrent_profile <= 2 && std::floor(torrent_profile) == torrent_profile ? int(torrent_profile) : 2;
		settings_.subtitle_langs = jstr(j, "subtitle_languages", settings_.subtitle_langs);
		settings_.auto_subtitles = jbool(j, "auto_subtitles", settings_.auto_subtitles);
		settings_.download_directory = jstr(j, "download_directory");
		const double download_sort = jnum(j, "download_sort", 2);
		settings_.download_sort = std::isfinite(download_sort) && download_sort >= 0 && download_sort <= 2 &&
			std::floor(download_sort) == download_sort ? int(download_sort) : 2;
		settings_.sub_size = jstr(j, "subtitle_size", settings_.sub_size);
		auto known = [&j](const char* key, const std::string& fallback, std::initializer_list<const char*> allowed) {
			const auto value = jstr(j, key, fallback);
			for (const auto* choice : allowed) if (value == choice) return value;
			return fallback;
		};
		settings_.sub_color = known("subtitle_color", "white", {"white", "yellow", "green", "cyan"});
		settings_.sub_effect = known("subtitle_effect", "shadow", {"none", "outline", "shadow", "raised", "depressed"});
		settings_.sub_font = known("subtitle_font", "sans", {"sans", "serif", "mono"});
		settings_.sub_background_opacity = std::clamp(int(jnum(j, "subtitle_background_opacity", 70)), 0, 100);
		settings_.audio_langs = jstr(j, "audio_languages", settings_.audio_langs);
		settings_.autoplay_next = jbool(j, "autoplay_next", settings_.autoplay_next);
		const double next_delay = jnum(j, "next_episode_delay_seconds", 15);
		settings_.next_episode_delay_seconds = std::isfinite(next_delay) ?
			int(std::clamp(next_delay, 5.0, 120.0)) : 15;
		settings_.auth_key = jstr(j, "auth_key");
		settings_.user_email = jstr(j, "user_email");
        settings_.reduced_motion = jbool(j, "reduced_motion", false);
        settings_.ui_sounds = jbool(j, "ui_sounds", true);
        settings_.high_contrast = jbool(j, "high_contrast", false);
        settings_.show_stats = jbool(j, "show_stats", false);
        settings_.sound_volume = std::clamp(int(jnum(j, "sound_volume", 35)), 0, 100);
        const auto& media_volume = jobj(j, "media_volume");
        settings_.media_volume = playback_volume::saved_percent(media_volume.is_number()
            ? media_volume.get<double>() : playback_volume::default_percent);
        settings_.seek_seconds = std::clamp(int(jnum(j, "seek_seconds", 10)), 5, 60);
        settings_.shoulder_seek_seconds = std::clamp(int(jnum(j, "shoulder_seek_seconds", 60)), 5, 300);
        settings_.subtitle_offset_ms = std::clamp(int(jnum(j, "subtitle_offset_ms", 0)), -10000, 10000);
        // The old numeric field also stored the forced-1080p default. A new
        // explicit key migrates existing installations to following the PS5.
        settings_.display_resolution = display_output_preference(jstr(j, "display_output_mode", "ps5"));
		const auto video_choice = [&](const char* key, int maximum) {
			const double value = jnum(j, key, 0);
			return std::isfinite(value) && value >= 0 && value <= maximum && std::floor(value) == value
			    ? static_cast<int>(value) : 0;
		};
		settings_.display_refresh = video_choice("display_refresh", 2);
		settings_.audio_passthrough = jbool(j, "audio_passthrough", false);
        // A language saved by an older build is retained as an explicit choice.
        const auto legacy_language = jstr(j, "ui_language");
        settings_.ui_language_mode = known("ui_language_mode",
            legacy_language == "it" || legacy_language == "en" ? legacy_language : "auto", {"auto", "en", "it"});
        settings_.ui_language = resolve_ui_language(settings_.ui_language_mode, system_ui_language_);
        settings_.date_format = known("date_format", "short", {"short", "long", "numeric", "iso"});
        const double date_parts = jnum(j, "date_components", kAllDateComponents);
        settings_.date_components = std::isfinite(date_parts) ? int(std::clamp(date_parts, 0.0, double(kAllDateComponents))) : kAllDateComponents;
        settings_.controller_ambient_light = jbool(j, "controller_ambient_light", true);

		const json& ex = jobj(j, "extra_addons");
		if (ex.is_array())
			for (auto& v : ex)
				if (v.is_string()) settings_.extra_addons.push_back(v.get<std::string>());
	}

	// config.json next to the data (or the app) can preset things, e.g. a
	// server address or an account, without typing on the controller.
	json cfg;
	std::string err;
	std::string cfg_path = data_dir_ + "/config.json";
	if (!file_exists(cfg_path)) cfg_path = base_dir_ + "/config.json";
	if (file_exists(cfg_path)) {
		if (!load_json(cfg_path, cfg, &err)) {
			config_note_ = "config.json is not valid JSON (" + err + ")";
		} else {
#ifndef PLATFORM_PS5_NATIVE
			if (settings_.server_url.empty()) settings_.server_url = jstr(cfg, "server_url");
#endif
			if (cfg.contains("subtitle_languages")) settings_.subtitle_langs = jstr(cfg, "subtitle_languages");
			if (cfg.contains("audio_languages")) settings_.audio_langs = jstr(cfg, "audio_languages");
			const json& ex = jobj(cfg, "extra_addons");
			if (ex.is_array())
				for (auto& v : ex)
					if (v.is_string()) {
						std::string u = normalize_addon_url(v.get<std::string>());
						if (std::find(settings_.extra_addons.begin(), settings_.extra_addons.end(), u) ==
						    settings_.extra_addons.end())
							settings_.extra_addons.push_back(u);
					}
            if (cfg.contains("login_password"))
                config_note_ = "Use the Stremio sign-in code in Settings to connect your account.";

		}
	}
	w_sub_size = settings_.sub_size;
	bt::Engine::get().set_speed_profile(static_cast<bt::SpeedProfile>(settings_.torrent_speed_profile));
	save_settings();
}

void App::save_settings() {
#ifdef PLATFORM_PS5_NATIVE
    settings_.server_url.clear();
    settings_.builtin_torrents = true;
#endif
    ui_reduced_motion = settings_.reduced_motion;
    ui_sounds = settings_.ui_sounds;
    ui_high_contrast = settings_.high_contrast;
    ui_show_stats = settings_.show_stats;
    ui_sound_volume = settings_.sound_volume;
    ui_language = settings_.ui_language;
    refresh_clock();
    if (offline_) return;
    ++*settings_io_epoch_;
    settings_pending_ = true;
    settings_save_at_ = now_seconds() + 0.4;
}

void App::flush_settings(bool synchronous) {
    if (offline_ || (!synchronous && (!settings_pending_ || settings_writing_))) return;
    json j = {
        {"schema_version", 7}, {"server_url", settings_.server_url},
        {"download_directory", settings_.download_directory},
        {"download_sort", settings_.download_sort},
        {"builtin_torrents", settings_.builtin_torrents},
        {"torrent_speed_profile", settings_.torrent_speed_profile},
        {"subtitle_languages", settings_.subtitle_langs}, {"auto_subtitles", settings_.auto_subtitles},
        {"subtitle_size", settings_.sub_size}, {"audio_languages", settings_.audio_langs},
        {"subtitle_color", settings_.sub_color}, {"subtitle_effect", settings_.sub_effect},
        {"subtitle_font", settings_.sub_font}, {"subtitle_background_opacity", settings_.sub_background_opacity},
        {"autoplay_next", settings_.autoplay_next}, {"extra_addons", settings_.extra_addons},
        {"next_episode_delay_seconds", settings_.next_episode_delay_seconds},
        {"auth_key", settings_.auth_key}, {"user_email", settings_.user_email},
        {"reduced_motion", settings_.reduced_motion}, {"ui_sounds", settings_.ui_sounds},
        {"high_contrast", settings_.high_contrast}, {"show_stats", settings_.show_stats},
        {"sound_volume", settings_.sound_volume}, {"seek_seconds", settings_.seek_seconds},
        {"media_volume", settings_.media_volume},
        {"shoulder_seek_seconds", settings_.shoulder_seek_seconds},
        {"date_format", settings_.date_format}, {"date_components", settings_.date_components},
        {"controller_ambient_light", settings_.controller_ambient_light},
        {"subtitle_offset_ms", settings_.subtitle_offset_ms},
        {"display_output_mode", display_output_preference_name(settings_.display_resolution)},
		{"display_refresh", settings_.display_refresh},
		{"audio_passthrough", settings_.audio_passthrough},
        {"ui_language", settings_.ui_language}, {"ui_language_mode", settings_.ui_language_mode}
    };
    const auto path = data_dir_ + "/settings.json";
    auto mutex = settings_io_mutex_;
    auto epoch = settings_io_epoch_;
    const auto version = epoch->load();
    auto write = [j = std::move(j), path, mutex, epoch, version]() {
        std::lock_guard<std::mutex> lock(*mutex);
        if (epoch->load() != version) return true;
        return save_json(path, j);
    };
    settings_pending_ = false;
    if (synchronous) { write(); return; }
    settings_writing_ = true;
    g_tasks.run<bool>(std::move(write), [this](bool& ok) {
        settings_writing_ = false;
        if (!ok) show_toast("Could not save Stremio settings.");
    });
}

void App::load_progress() {
	json j;
	if (!load_json(data_dir_ + "/progress.json", j)) return;
	const auto load = [&](const json& items, std::map<std::string, Progress>& destination) {
		if (!items.is_object()) return;
		for (auto it = items.begin(); it != items.end(); ++it) {
			if (!it.value().is_object() || it.key().size() > 8192) continue;
			Progress p;
			p.type = jstr(it.value(), "type");
			p.name = jstr(it.value(), "name");
			p.poster = jstr(it.value(), "poster");
			p.video_id = jstr(it.value(), "video_id");
			p.time = jnum(it.value(), "time");
			p.duration = jnum(it.value(), "duration");
			const auto integer = [&](const char* key, int64_t fallback) {
				const auto found = it.value().find(key);
				if (found == it.value().end() || !found->is_number_integer() ||
				    (found->is_number_unsigned() && found->get<uint64_t>() > uint64_t(INT64_MAX))) return fallback;
				return found->get<int64_t>();
			};
			p.updated = integer("updated", 0);
			p.download_id = jstr(it.value(), "download_id");
			p.download_source = jstr(it.value(), "download_source");
			p.download_prefix = integer("download_prefix", 0);
			p.download_total = integer("download_total", -1);
			if (!std::isfinite(p.time) || p.time < 0 || !std::isfinite(p.duration) || p.duration < 0) continue;
			destination[it.key()] = std::move(p);
		}
	};
	load(jobj(j, "items"), progress_);
	load(jobj(j, "videos"), video_progress_);
	// Existing installs have only one bookmark per series. Seed the new video
	// map without replacing a newer, independently saved episode bookmark.
	for (const auto& [media_id, progress] : progress_)
		video_progress_.try_emplace(video_progress_key(progress.type, media_id, progress.video_id), progress);
	const json& w = jobj(j, "watched");
	if (w.is_array())
		for (auto& v : w)
			if (v.is_string()) watched_.insert(v.get<std::string>());
}

void App::save_progress() {
	if (offline_) return;
	json j;
	const auto save = [](const std::map<std::string, Progress>& entries) {
		json items = json::object();
		for (const auto& kv : entries) {
			items[kv.first] = json{{"type", kv.second.type},         {"name", kv.second.name},
		                       {"poster", kv.second.poster},     {"video_id", kv.second.video_id},
		                       {"time", kv.second.time},         {"duration", kv.second.duration},
		                       {"updated", kv.second.updated}, {"download_id", kv.second.download_id},
		                       {"download_source", kv.second.download_source},
		                       {"download_prefix", kv.second.download_prefix}, {"download_total", kv.second.download_total}};
		}
		return items;
	};
	j["items"] = save(progress_);
	j["videos"] = save(video_progress_);
	j["watched"] = json(std::vector<std::string>(watched_.begin(), watched_.end()));
	if (!save_json(data_dir_ + "/progress.json", j))
		dlog("Playback progress could not be saved to %s (errno %d)", data_dir_.c_str(), errno);
}

std::string App::server() const {
#ifdef PLATFORM_PS5_NATIVE
	return {};
#else
	std::string s = trim(settings_.server_url);
	while (!s.empty() && s.back() == '/') s.pop_back();
	if (!s.empty() && !starts_with(s, "http://") && !starts_with(s, "https://")) s = "http://" + s;
	return s;
#endif
}

static std::vector<std::string> lang_list(const std::string& s) {
	std::vector<std::string> out;
	for (auto& part : split(s, ',')) {
		std::string p = trim(part);
		if (p.empty() || p == "-") continue;
		out.push_back(language_to_iso639_2(p));
	}
	return out;
}

std::vector<std::string> App::pref_sub_langs() const { return lang_list(settings_.subtitle_langs); }
std::vector<std::string> App::pref_audio_langs() const { return lang_list(settings_.audio_langs); }

// ---------------------------------------------------------------------------
// Addons and library

const Addon* App::find_addon(const std::string& url) const {
	for (auto& a : addons_)
		if (a->transport_url == url) return a.get();
	return nullptr;
}

void App::load_addons() {
	if (offline_) return;
	// Only the first model is staged. Later catalog callbacks update the
	// existing shelves without re-opening the loading screen.
	if (signed_in() && home_rows.empty() && !home_startup_.active())
		home_startup_.begin(now_seconds());
	struct Result {
		std::vector<std::shared_ptr<Addon>> addons;
		std::vector<std::string> errors;
		bool auth_failed = false;
	};
	std::string key = settings_.auth_key;
	std::vector<std::string> extra = settings_.extra_addons;
	int gen = ++addons_gen_;
	addons_loading_ = true;
	if (home_rows.empty()) dirty_all();

	bg<Result>(
	    [key, extra]() {
		    Result r;
		    std::set<std::string> seen;
		    if (!key.empty()) {
			    ApiResult ar = api_addon_collection(key);
			    if (ar.ok) {
				    const json& list = jobj(ar.result, "addons");
				    if (list.is_array()) {
					    for (auto& e : list) {
						    auto a = std::make_shared<Addon>();
						    std::string url = jstr(e, "transportUrl");
						    if (parse_addon(jobj(e, "manifest"), url, *a)) {
							    a->from_account = true;
							    if (seen.insert(url).second) r.addons.push_back(a);
						    }
					    }
				    }
			    } else {
				    r.errors.push_back("Could not load your addons (" + ar.error + ")");
				    std::string l = lower(ar.error);
				    if (l.find("session") != std::string::npos || l.find("auth") != std::string::npos)
					    r.auth_failed = true;
			    }
		    }
		    std::vector<std::string> urls;
		    if (r.addons.empty()) urls = {kCinemetaUrl, kOpenSubtitlesUrl};
		    for (auto& u : extra) urls.push_back(u);
		    for (auto& u : urls) {
			    if (seen.count(u)) continue;
			    auto a = std::make_shared<Addon>();
			    std::string err;
			    if (fetch_addon(u, *a, err)) {
				    seen.insert(u);
				    r.addons.push_back(a);
			    } else {
				    r.errors.push_back("Could not reach " + http_log_target(u) + " (" + err + ")");
			    }
		    }
		    return r;
	    },
	    [this, gen](Result& r) {
		    if (gen != addons_gen_) return;
		    addons_loading_ = false;
		    addons_.clear();
		    for (auto& a : r.addons) addons_.push_back(std::make_unique<Addon>(*a));
		    if (!r.errors.empty()) show_toast(r.errors.front(), 6);
		    if (r.auth_failed) show_toast("Your Stremio session expired. Sign in again in Settings.", 8);
		    dlog("addons: %zu loaded", addons_.size());
		    on_addons_loaded();
	    });
}

void App::on_addons_loaded() {
	banner.clear();
	build_home();
	if (view == "discover") enter_discover();
	if (view == "addons") enter_addons();
	if (view == "settings") settings_refresh();
	dirty_all();
}

void App::load_library() {
	if (offline_) return;
	const int account_gen = account_generation_;
	const auto revision = library_revision_;
	// An older snapshot must never replace an optimistic favorite/progress
	// change. The serial writer refreshes the account after its queue drains.
	if (library_write_inflight_ || !library_pending_changes_.empty()) {
		library_refresh_pending_ = true;
		return;
	}
	library_refresh_pending_ = false;
	const auto load_generation = ++library_load_generation_;
	std::string key = settings_.auth_key;
	if (key.empty()) return;
	bg<ApiResult>([key]() { return api_library_get(key); },
	              [this, account_gen, revision, load_generation](ApiResult& r) {
		              if (account_gen != account_generation_ || load_generation != library_load_generation_) return;
		              if (revision != library_revision_ || library_write_inflight_ || !library_pending_changes_.empty()) {
		                  library_refresh_pending_ = true;
		                  if (!library_write_inflight_ && library_pending_changes_.empty()) load_library();
		                  return;
		              }
		              if (!r.ok) {
		                  library_initial_settled_ = true;
			              show_toast("Could not load your library: " + r.error);
			              return;
		              }
		              library_.clear();
		              if (r.result.is_array())
			              for (auto& it : r.result) {
				              std::string id = jstr(it, "_id");
				              if (!id.empty()) library_[id] = it;
			              }
		              library_loaded_ = true;
		              library_initial_settled_ = true;
		              dlog("library: %zu items", library_.size());
		              refresh_library_membership();
		              dirty_all();
	              });
}

// ---------------------------------------------------------------------------
// Views and input

void App::set_view(const std::string& v) {
	if (next_episode_visible && watching_) watch_stop(true);
	else watch_cancel_next_episode();
	g_art.clear_queue();
	if (v != "downloads") download_series_id.clear();
	view = v;
	zone = "content";
	if (v == "home") nav_sel = 0, refresh_home_cards();
	else if (v == "discover") nav_sel = 1, enter_discover();
	else if (v == "library") nav_sel = 2, enter_library();
	else if (v == "downloads") nav_sel = 3, downloads_refresh();
	else if (v == "addons") nav_sel = 4, enter_addons();
	else if (v == "settings") nav_sel = 4, enter_settings();
	dirty_all();
}

void App::nav_button(Btn b) {
	// Old persisted/fixture focus values cannot reactivate the header. It is
	// an indicator only; shoulder buttons own main-page changes.
	zone = (view == "discover" || view == "library") ? "filters" : "content";
	if (b == Btn::L1 || b == Btn::R1) browse_shortcut(b);
}

void App::open_search() {
	view = "search";
	zone = "searchbox";
	dirty_all();
	on_button(Btn::Cross);
}

// Buttons that do the same thing on every page outside the player.
bool App::browse_shortcut(Btn b) {
	static const char* views[] = {"home", "discover", "library", "downloads", "settings"};
	switch (b) {
	case Btn::L1:  // previous / next page of the menu
	case Btn::R1: {
		if (view == "search" || view == "addons") return false;
		int next = nav_sel + (b == Btn::L1 ? -1 : 1);
		if (next < 0 || next > 4) return true;
		set_view(views[next]);
		return true;
	}
	case Btn::L2:  // page up / down
	case Btn::R2:
		if (zone != "content") return true;
		for (int i = 0; i < 5 && zone == "content"; i++) on_button(b == Btn::L2 ? Btn::Up : Btn::Down);
		return true;
	case Btn::Touchpad:
	case Btn::L3:
	case Btn::R3:
		return true;
	case Btn::Triangle:  // Search has one explicit controller shortcut.
		open_search();
		return true;
	default: return false;
	}
}

void App::on_button(Btn b) {
	if (home_startup_visible()) return;
	if (input_visible_) return;  // the system keyboard dialog has the controller
	if (directory_picker_visible) { directory_picker_button(b); return; }
	if (download_directory_notice) {
		if (b == Btn::Cross || b == Btn::Circle) { download_directory_notice = false; dirty_all(); }
		return;
	}
	if (dd_visible) return dropdown_button(b);
	if (login_visible) {
		if (b == Btn::Circle) {
			if (signed_in()) login_close();
			else exit_ = true;
		}
		else if (b == Btn::Cross) request_login_code();
		return;
	}
	if (next_episode_visible) { watch_next_episode_button(b); return; }
	if (launch_visible && !watching_) {  // the server is still preparing the stream
		if (b == Btn::Circle) {
			if (launch_cancel_) *launch_cancel_ = true;
			w_gen_++;
			launch_visible = false;
			if (w_torrent_slot_) { downloads_.set_torrent_playback_active(false); w_torrent_slot_ = false; }
			t_visible = false;
			dirty_all();
		}
		return;
	}
	if (watching_) {
		if (menu_visible) return watch_menu_button(b);
		return watch_button(b);
	}
	if (view == "detail") {
		return detail_button(b);
	}
	if (view == "downloads" && (!download_series_id.empty() || b == Btn::L2 || b == Btn::R2)) {
		downloads_button(b);
		return;
	}
	if (browse_shortcut(b)) return;

	if (zone == "nav") nav_button(b);
	if (zone == "searchbox") {
		switch (b) {
		case Btn::Cross:
			open_input("Search", view == "search" ? search_query : "", "Type, then press Enter",
			           [this](const std::string& q) {
				           if (!trim(q).empty()) start_search(trim(q));
			           });
			break;
		case Btn::Down: zone = "content"; break;
		case Btn::Left:
		case Btn::Circle:
			if (view == "search") {
				static const char* views[] = {"home", "discover", "library", "downloads", "settings"};
				set_view(views[std::clamp(nav_sel, 0, 4)]);
			} else zone = "content";
			break;
		default: break;
		}
		dirty_all();
		return;
	}
	if (view == "home") board_button(b);
	else if (view == "search") search_button(b);
	else if (view == "discover") discover_button(b);
	else if (view == "library") library_button(b);
	else if (view == "downloads") downloads_button(b);
	else if (view == "addons") addons_button(b);
	else if (view == "settings") settings_button(b);
	if (zone == "nav") zone = (view == "discover" || view == "library") ? "filters" : "content";
}

void App::on_text(const std::string& utf8) {
	if (!input_visible_) return;
	input_value += utf8;
}

// ---------------------------------------------------------------------------
// Frame

std::string App::art(const std::string& url, ArtKind kind, bool want) {
	if (url.empty()) return "";
	if (!want) return g_art.peek_cached(url, kind);
	const auto priority = kind == ArtKind::Logo || kind == ArtKind::LogoBox || kind == ArtKind::LaunchLogo ||
	    kind == ArtKind::Backdrop || kind == ArtKind::Qr ? ArtPriority::Immediate : ArtPriority::Visible;
	return g_art.get_async(url, kind, [this](const std::string& path) {
		if (!path.empty()) images_dirty_ = true;
	}, priority, this);
}

void App::refresh_images() {
	if (view == "home") refresh_home_cards();
	if (view == "search") refresh_search_cards();
	if (view == "discover") {
		for (size_t i = 0; i < disc_cards.size() && i < disc_items_.size(); i++) {
			bool near = int(i) / kDiscCols >= disc_sel / kDiscCols - 1 && int(i) / kDiscCols <= disc_sel / kDiscCols + 2;
			disc_cards[i].image = art(disc_items_[i].poster, ArtKind::PosterLarge, near);
		}
		discover_preview();
	}
	if (view == "library") {
		for (size_t i = 0; i < lib_cards.size() && i < lib_items_.size(); i++) {
			bool near = int(i) / 7 >= lib_sel / 7 - 2 && int(i) / 7 <= lib_sel / 7 + 3;
			lib_cards[i].image = art(lib_items_[i].poster, ArtKind::Poster, near);
		}
	}
	if (view == "detail") {
		d_background = art(d_meta_loaded_ && !d_meta_.background.empty() ? d_meta_.background : d_item_.background, ArtKind::Backdrop);
		d_logo = art(d_meta_loaded_ && !d_meta_.logo.empty() ? d_meta_.logo : d_item_.logo, ArtKind::LogoBox);
		for (size_t i = 0; i < d_episodes.size() && i < d_season_videos_.size(); i++) {
			bool near = std::abs(int(i) - d_episode_sel) <= 6;
			const auto& thumbnail = d_season_videos_[i]->thumbnail;
			d_episodes[i].thumb = art(thumbnail.empty() ? d_item_.background : thumbnail, ArtKind::Thumb, near);
		}
	}
	if (view == "addons")
		for (size_t i = 0; i < addon_rows.size() && i < addons_.size(); i++)
			addon_rows[i].logo = art(jstr(addons_[i]->manifest, "logo"), ArtKind::Icon);
	if (launch_visible) {
		launch_image = w_offline_ ? w_download_background_ : art(w_item_.background, ArtKind::Background);
		launch_logo = w_offline_ ? w_download_logo_ : art(w_item_.logo, ArtKind::LaunchLogo);
	}
	dirty_all();
}

bool App::visible_art_missing() const {
	if (login_visible) return false;
	if (launch_visible) return !w_offline_ && ((!w_item_.background.empty() && launch_image.empty()) ||
	                           (!w_item_.logo.empty() && launch_logo.empty()));
	if (watching_) return false;
	auto posters_missing = [](const std::vector<UiCard>& cards, const std::vector<Item>& items, int begin, int end) {
		for (int i = std::max(0, begin); i < end && i < int(cards.size()) && i < int(items.size()); ++i)
			if (cards[i].image.empty() && !items[i].poster.empty()) return true;
		return false;
	};
	if (view == "home" || view == "search") {
		const auto& rows = view == "home" ? home_rows : search_rows;
		const auto& source = view == "home" ? board_ : search_;
		const auto& mapping = view == "home" ? home_map_ : search_map_;
		const int selected = view == "home" ? home_row : search_row;
		const int col = view == "home" ? home_col : search_col;
		for (int row = std::max(0, selected - 1); row <= selected + 1 && row < int(rows.size()) && row < int(mapping.size()); ++row) {
			if (mapping[row] < 0 || mapping[row] >= int(source.size())) continue;
			const int focus = row == selected ? col : 0;
			if (posters_missing(rows[row].cards, source[mapping[row]].items, focus - 2, focus + 9)) return true;
		}
	} else if (view == "discover") {
		if (disc_sel >= 0 && disc_sel < int(disc_items_.size())) {
			const auto& item = disc_items_[disc_sel];
			if (!item.logo.empty() && dp_logo.empty()) return true;
		}
		const int row = disc_sel / kDiscCols;
		return posters_missing(disc_cards, disc_items_, (row - 1) * kDiscCols, (row + 3) * kDiscCols);
	} else if (view == "library") {
		const int row = lib_sel / 7;
		return posters_missing(lib_cards, lib_items_, (row - 1) * 7, (row + 3) * 7);
	} else if (view == "detail") {
		const auto& background = d_meta_loaded_ && !d_meta_.background.empty() ? d_meta_.background : d_item_.background;
		const auto& logo = d_meta_loaded_ && !d_meta_.logo.empty() ? d_meta_.logo : d_item_.logo;
		if ((!background.empty() && d_background.empty()) || (!logo.empty() && d_logo.empty())) return true;
		if (d_series && d_zone != "streams") {
			for (int i = std::max(0, d_episode_sel - 6); i <= d_episode_sel + 6 && i < int(d_episodes.size()) && i < int(d_season_videos_.size()); ++i)
				if (d_episodes[i].thumb.empty() && (!d_season_videos_[i]->thumbnail.empty() || !d_item_.background.empty())) return true;
		}
	} else if (view == "addons") {
		for (int i = std::max(0, (addon_sel / kAddonCols - 1) * kAddonCols);
		     i < (addon_sel / kAddonCols + 3) * kAddonCols && i < int(addon_rows.size()) && i < int(addons_.size()); ++i)
			if (addon_rows[i].logo.empty() && !jstr(addons_[i]->manifest, "logo").empty()) return true;
	}
	return false;
}

void App::refresh_clock() {
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	if (!localtime_r(&now, &local)) return;
	char value[16];
	std::snprintf(value, sizeof(value), "%02d:%02d", local.tm_hour, local.tm_min);
	const auto current_date = calendar_date(local, settings_.ui_language, settings_.date_format, settings_.date_components);
	if (clock != value || date != current_date) {
		clock = value;
		date = current_date;
		dirty_all();
	}
}

std::pair<std::string, std::string> App::ambient_artwork() const {
	if (login_visible || !settings_.controller_ambient_light) return {};
	if ((watching_ || launch_visible) && w_offline_)
		return {"download:" + w_download_id_, w_download_poster_.empty() ? w_download_background_ : w_download_poster_};
	if (view == "downloads" && download_sel >= 0 && download_sel < int(download_rows.size()))
		return {"download:" + download_rows[download_sel].id, download_rows[download_sel].image};
	const Item* item = nullptr;
	if (watching_ || launch_visible) item = &w_item_;
	else if (view == "detail") item = &d_item_;
	else if (view == "discover" && disc_sel >= 0 && disc_sel < int(disc_items_.size())) item = &disc_items_[disc_sel];
	else if (view == "library" && lib_sel >= 0 && lib_sel < int(lib_items_.size())) item = &lib_items_[lib_sel];
	else if (view == "home" || view == "search") {
		const auto& mapping = view == "home" ? home_map_ : search_map_;
		const auto& source = view == "home" ? board_ : search_;
		const int row = view == "home" ? home_row : search_row;
		const int col = view == "home" ? home_col : search_col;
		if (row >= 0 && row < int(mapping.size()) && mapping[row] >= 0 && mapping[row] < int(source.size())) {
			const auto& items = source[mapping[row]].items;
			if (col >= 0 && col < int(items.size())) item = &items[col];
		}
	}
	if (!item || item->id.empty()) return {};
	std::string path;
	for (const auto kind : {ArtKind::Poster, ArtKind::PosterLarge}) {
		path = g_art.peek_cached(item->poster, kind);
		if (!path.empty()) break;
	}
	if (path.empty())
		for (const auto kind : {ArtKind::Background, ArtKind::Backdrop, ArtKind::Still}) {
			path = g_art.peek_cached(item->background, kind);
			if (!path.empty()) break;
		}
	return {item->type + ":" + item->id, path};
}

void App::update() {
	g_tasks.drain();
	input_poll();
	if (directory_picker_committing) directory_picker_tick();
	double now = now_seconds();
	if (!offline_ && signed_in() && now >= download_art_refresh_at_) {
		download_art_refresh_at_ = now + 2;
		refresh_download_artwork();
	}
	if (view == "downloads" && now >= download_refresh_at_) {
		download_refresh_at_ = now + 0.25;
		if (download_revision_ != downloads_.revision()) downloads_refresh();
	}

	if (now - last_clock_ > 1) {
		last_clock_ = now;
		refresh_clock();
	}
	if (!toast.empty() && now > toast_until_) {
		toast.clear();
		dirty("toast");
	}
	bool b = busy_count_ > 0 || addons_loading_;
	if (b != busy) {
		busy = b;
		dirty("busy");
	}
	if (login_visible) login_poll();
	if (t_visible) torrent_stats_poll();
	saved_stream_tick();
	if (watching_ || launch_visible) watch_update();
	watch_next_episode_tick();
	// A row whose requests all failed has no success callback to wake it.
	// Revisit only visible missing artwork; ArtCache enforces each URL's
	// retry backoff, and a completed row stops this work automatically.
	if (now >= images_retry_at_) {
		images_retry_at_ = now + 2;
		if (visible_art_missing()) images_dirty_ = true;
	}
	if (images_dirty_ && now - images_refreshed_ > 0.25) {
		images_dirty_ = false;
		images_refreshed_ = now;
		refresh_images();
	}
	update_preview_metadata();
	refresh_home_preview();
	if (settings_pending_ && now >= settings_save_at_) flush_settings();
	all_dirty_ = false;
}
