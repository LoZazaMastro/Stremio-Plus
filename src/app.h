#pragma once

#include <SDL.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "artcache.h"
#include "download_manager.h"
#include "player.h"
#include "media_commands.h"
#include "playback_progress.h"
#include "stremio.h"
#include "tasks.h"
#include "subtitles.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Data model structs (names match assets/main.rml)

struct UiCard {
	std::string title, image, initials, badge, progress = "0%";
	std::string background, description, year, runtime, rating;
	std::string logo;
	bool in_library = false;
};
struct UiRow {
	std::string title, dom_id, message;
	bool see_all = false;
	std::vector<UiCard> cards;
	bool continue_watching = false;
};
struct UiChip {
	std::string value;
};
constexpr int kAddonCols = 4;  // addon tiles per row (browse.rcss)
constexpr int kDiscCols = 4;   // discover posters per row (browse.rcss #disc-grid)
struct UiAddon {
	std::string name, version, desc, types, logo, initials;
	bool local = false;
};
struct UiSetting {
	std::string label, hint, value;
	std::string kind = "action", id;
	bool on = false, enabled = true;
	float number = 0, minimum = 0, maximum = 1, step = 1;
	std::vector<std::string> options;
	int selected = 0;
};
struct UiEpisode {
	std::string title, sub, thumb;
	bool watched = false;
	std::string description, number;
};
struct UiStream {
	std::string name, addon, desc;
	std::vector<std::string> languages;
	int64_t seeders = -1;
	std::string type, quality, size;
	bool cached = false;
};
struct UiDownload {
	std::string id, title, subtitle, image, status, size, speed;
	std::string remaining, connections;
	std::string media_id, type, episode_title, thumbnail;
	int season = -1, episode = -1;
	int release_year = 0;
	int64_t downloaded_ms = 0;
	float progress = -1;
	bool complete = false, active = false, failed = false, paused = false;
	bool playable_while_downloading = false;
	bool recovery_only = false;
};
struct UiDownloadGroup {
	std::string id, title, image;
	std::vector<int> rows;
	std::vector<int> seasons;
	int representative = -1, complete = 0;
	int release_year = 0;
	int64_t downloaded_ms = 0;
	bool series = false;
};
struct UiMenuItem {
	std::string label;
	bool active = false;
	std::string lang;
};
struct UiKey {
	std::string label;
	bool wide = false;
};

// Something that can be opened: a catalog entry, a library item.
struct Item {
	std::string id, type, name, poster, background, logo;
	std::string description, release_info, runtime, imdb_rating;
	std::vector<std::string> genres;
	std::vector<std::string> cast, directors;
	json inline_meta;  // full catalog metadata/inline videos, when explicitly supplied
	std::string metadata_addon, metadata_addon_url;
	// Continue watching
	std::string video_id;
	double offset = 0, duration = 0;  // seconds
	int64_t last_watched = 0;          // epoch ms
	int times_watched = 0;
};
Item item_from_meta_json(const json& j);

enum class Btn { Up, Down, Left, Right, Cross, Circle, Square, Triangle, Options, L1, R1, L2, R2, L3, R3, Touchpad };

// Board / search rows: one per catalog.
struct BoardRow {
	std::string title;
	std::string error;
	const Addon* addon = nullptr;
	std::string addon_url;
	Catalog catalog;
	std::vector<Item> items;
	bool continue_watching = false;
	bool loaded = false;
};

struct Settings {
	// Legacy desktop configuration. Native PS5 playback always uses its own
	// torrent engine and never routes through this optional desktop server.
	std::string server_url;
	bool builtin_torrents = true;
	int torrent_speed_profile = 2; // 0 Balanced, 1 Fast, 2 Ultra fast
	std::string download_directory;
	int download_sort = 2; // Title A-Z, newest release year, newest download.
	std::string subtitle_langs = "ita, eng";
	bool auto_subtitles = true;
	std::string sub_size = "sub-m";
	std::string sub_color = "white", sub_effect = "shadow", sub_font = "sans";
	int sub_background_opacity = 70;
	std::string audio_langs = "ita, eng";
	bool autoplay_next = true;
	int next_episode_delay_seconds = 15;
	bool reduced_motion = false, ui_sounds = true, high_contrast = false, show_stats = false;
	int sound_volume = 35, subtitle_offset_ms = 0, seek_seconds = 10, shoulder_seek_seconds = 60;
	int display_resolution = -1;  // Automatic (PS5); 0/1/2 are explicit 1080p/1440p/2160p.
	int display_refresh = 0;  // Off, automatic, playback only.
	bool audio_passthrough = false;
	int date_components = 15;  // Weekday, day, month, year; zero hides the date.
	std::string date_format = "short";
	bool controller_ambient_light = true;
	std::string ui_language = "en", ui_language_mode = "auto";
	std::vector<std::string> extra_addons;
	std::string auth_key, user_email;
};

class App {
	friend struct AppProtocolTest;  // headless integration tests of real asynchronous flows
public:
	bool init(const std::string& base_dir, const std::string& data_dir, bool offline = false);
	void shutdown();

	void on_button(Btn b);
	void on_media_command(MediaCommand command);
	void open_search();  // show search and its keyboard; query begins only on submit
	void request_login_code();  // replace the current link; ignore while a request is pending
	void on_text(const std::string& utf8);  // text from the keyboard dialog
	void input_submit();                     // its Done (a Return key)
	void update();          // every frame, before the RmlUi update
	void set_setting_value(int row, float number);
	const Settings& settings() const { return settings_; }
	bool take_video_frame(Player::VideoFrame& frame) { return watching_ && player_.take_frame(frame); }
	std::string audio_output_status() const { return player_.audio_output_status(); }
	void video_frame_presented();  // Called only after a successful GPU upload.
	void video_frame_rejected();
	void cancel_input() { if (input_visible_) input_finish(false); }
	void replace_input(const std::string& text) { input_value = text; }
	void request_search(const std::string& text) { start_search(text); }
	void refresh_home_preview();
	bool wants_exit() const { return exit_; }
	bool watching() const { return watching_; }
	bool text_entry_active() const { return input_visible_; }
	bool download_relocation_active() const;
	std::pair<std::string, std::string> ambient_artwork() const;
	// The platform yields its UI sound device before the player claims audio.
	std::function<void()> before_playback;

	// -------------------------------------------------------------------
	// Data model (bound by name)
	std::string zone = "nav", view = "home";
	int nav_sel = 0;
	std::string search_query, clock, date, user, banner;
	std::string home_preview_name, home_preview_background, home_preview_description, home_preview_meta;
	std::string home_preview_logo;
	bool ui_reduced_motion = false, ui_sounds = true, ui_high_contrast = false, ui_show_stats = false;
	int ui_sound_volume = 35;
	std::string ui_language = "en";
	bool busy = false;

	std::vector<UiRow> home_rows;
	int home_row = 0, home_col = 0;
	std::vector<UiRow> search_rows;
	int search_row = 0, search_col = 0;
	std::string search_status;

	std::vector<UiChip> disc_chips;
	int disc_chip = 0;
	std::vector<UiCard> disc_cards;
	int disc_sel = 0;
	std::string disc_status;
	std::string dp_name, dp_logo, dp_still, dp_runtime, dp_year, dp_imdb, dp_genres, dp_desc;
	std::string dp_cast, dp_directors;

	std::vector<UiChip> lib_chips;
	int lib_chip = 0;
	std::vector<UiCard> lib_cards;
	int lib_sel = 0;
	std::string lib_status;
	std::vector<UiDownload> download_rows;
	int download_sel = 0;
	std::string download_status;
	std::vector<UiDownloadGroup> download_groups;
	std::vector<int> download_episode_rows;
	int download_group_sel = 0, download_season = -1;
	std::string download_series_id;

	std::vector<UiAddon> addon_rows;
	int addon_sel = 0;

	std::vector<UiSetting> s_rows;
	int s_sel = 0;
	bool directory_picker_visible = false, directory_picker_loading = false;
	bool directory_picker_committing = false, directory_picker_error = false;
	bool download_directory_notice = false;
	std::string directory_picker_path, directory_picker_status;
	std::vector<std::string> directory_picker_entries;
	int directory_picker_sel = 0;
	int64_t directory_move_done = 0, directory_move_total = -1;
	int directory_move_items_done = 0, directory_move_items_total = 0;
	std::string directory_move_title;

	std::string d_background, d_logo, d_name, d_runtime, d_year, d_imdb, d_genres, d_cast, d_directors,
	    d_description, d_resume;
	bool d_series = false;
	std::string d_zone = "streams", d_season_label;
	std::vector<UiEpisode> d_episodes;
	int d_episode_sel = 0;
	std::vector<UiChip> d_seasons;
	int d_season_sel = 0;
	std::string d_streams_title, d_streams_status;
	std::vector<UiStream> d_streams;
	int d_stream_sel = 0;
	std::vector<UiChip> d_sources;
	int d_source_sel = 0;
	// Sources keep the addon's order; quality filtering is optional.
	bool d_pick_res = false;

	bool watching_ = false;
	std::string w_sub_rml, w_sub_size = "sub-m";
	bool info_visible = true, w_buffering = false, w_paused = false, menu_visible = false;
	// Seconds of conservatively verified local media. -1 means unrestricted.
	double w_seekable_until = -1;
	std::string w_buffer_text, w_title, w_subtitle, w_progress = "0%", w_time, w_duration, w_tracks, w_stats;
	int m_col = 0;
	std::vector<UiMenuItem> m_audio, m_subs;
	int m_audio_sel = 0, m_sub_sel = 0;
	std::string m_delay;

	bool launch_visible = false;
	bool next_episode_visible = false;
	int next_episode_seconds = 0, next_episode_sel = 0;
	std::string next_episode_title, next_episode_label, next_episode_thumb;
	std::string launch_image, launch_logo, launch_title, launch_status;
	float launch_progress = -1;  // -1 unknown; otherwise a normalized startup buffer fill.

	// Torrent statistics from the streaming server, while a torrent starts
	// or buffers (like the PC app's Statistics panel).
	bool t_visible = false;
	std::string t_peers = "0", t_speed = "0.00 MiB/s", t_progress = "0.00 %", t_hash;

	bool dd_visible = false;
	bool dd_multiselect = false;
	std::string dd_title;
	std::vector<UiMenuItem> dd_options;
	int dd_sel = 0;

	enum class LoginState { loading, ready, error, expired };
	LoginState login_state = LoginState::loading;
	bool login_visible = false;
	int login_seconds_remaining = 0;
	std::string login_qr, login_link, login_code, login_status;

	// Text entry through the PS5's own keyboard dialog (app_settings.cpp).
	bool input_visible_ = false, input_shown_ = false;
	double input_opened_ = 0;
	std::string input_value;
	std::string input_title, input_hint;

	std::string toast;
	uint64_t toast_revision = 0;
	// Startup/platform code can surface a recoverable failure through the same
	// timed notification used by the application, without owning UI state.
	void notify(const std::string& message, double seconds = 4) { show_toast(message, seconds); }

private:
	// app.cpp
	void dirty(const char* name);
	void dirty_all() { all_dirty_ = true; }
	void show_toast(const std::string& msg, double seconds = 4);
	void set_view(const std::string& v);
	void nav_button(Btn b);
	bool browse_shortcut(Btn b);
	void load_settings();
	std::string system_ui_language_ = "en";
	void save_settings();
	void load_progress();
	void save_progress();
	void load_addons();
	void on_addons_loaded();
	void load_library();
	const Addon* find_addon(const std::string& url) const;
	std::map<std::string, int> strip_first_;  // first fully shown card, by strip id
	// Local artwork path, "" until downloaded (then the page's images are
	// refreshed). With want=false only looks in the cache.
	std::string art(const std::string& url, ArtKind kind, bool want = true);
	void refresh_images();
	bool visible_art_missing() const;
	void update_preview_metadata();

	// Background work with the top bar's busy indicator.
	template <typename T>
	void bg(std::function<T()> work, std::function<void(T&)> done) {
		busy_count_++;
		g_tasks.run<T>(work, [this, done](T& v) {
			busy_count_--;
			done(v);
		});
	}
	int busy_count_ = 0;
	bool images_dirty_ = false;
	double images_refreshed_ = 0;
	double images_retry_at_ = 0;
	struct PreviewMetadata {
		Item item;
		double retry_at = 0, used_at = 0;
	};
	std::map<std::string, PreviewMetadata> preview_metadata_;
	std::string preview_metadata_key_;
	double preview_metadata_selected_at_ = 0;
	int preview_metadata_generation_ = 0;
	bool preview_metadata_loading_ = false;
	std::shared_ptr<std::atomic<bool>> preview_metadata_cancel_;
	std::string server() const;
	bool signed_in() const { return !settings_.auth_key.empty(); }
	std::vector<std::string> pref_sub_langs() const;
	std::vector<std::string> pref_audio_langs() const;

	// app_board.cpp
	void build_home();
	void refresh_home_cards();
	void board_button(Btn b);
	void open_item(const Item& it);
	void start_search(const std::string& q);
	void refresh_search_cards();
	void search_button(Btn b);
	UiCard make_card(const Item& it, bool show_progress);
	std::vector<Item> continue_watching() const;
	void remove_from_continue_watching(const Item& it);
	bool in_library(const std::string& id) const;
	void toggle_library(const Item& it);
	void refresh_library_membership();
	void queue_library_change(const json& item);
	void flush_library_changes();

	// app_browse.cpp
	void enter_discover();
	void discover_build_chips();
	void discover_load(bool more);
	void discover_preview();
	void discover_button(Btn b);
	void discover_see_all(const BoardRow& row);
	void enter_library();
	void library_refresh();
	void library_button(Btn b);
	void enter_addons();
	void addons_button(Btn b);
	void open_dropdown(const std::string& title, const std::vector<std::string>& options, int active,
	                   std::function<void(int)> chosen);
	void dropdown_button(Btn b);

	// Persistent, device-local downloads; Square enqueues one selected stream.
	void download_selected_stream(const Stream& stream);
	void downloads_refresh();
	void refresh_download_artwork();
	void downloads_button(Btn b);
	void play_download(const std::string& id, bool progressive = false);
	void start_download_playback(const std::string& id, bool progressive, double start);
	DownloadManager downloads_;
	uint64_t download_revision_ = 0;
	double download_refresh_at_ = 0;
	double download_art_refresh_at_ = 0;
	int download_art_account_ = -1;
	std::shared_ptr<std::atomic<bool>> download_art_cancel_;
	std::map<std::string, double> download_meta_retry_;
	std::set<std::string> download_meta_pending_;
	bool w_offline_ = false, w_torrent_slot_ = false;
	std::string w_download_id_, w_download_poster_, w_download_background_, w_download_logo_;
	std::shared_ptr<GrowingFilePlayback> w_growing_download_;

	// app_detail.cpp
	void detail_open(const Item& it);
	void detail_apply_meta();
	void detail_set_season(int idx);
	void detail_load_streams(const std::string& video_id);
	void detail_refresh_streams();
	void detail_select_source(int index);
	void detail_button(Btn b);
	void detail_close();
	void detail_update_resume();
	void play_stream(const Stream& st, bool transcode);
	bool play_saved_stream(const Stream& stream);
	void saved_stream_tick();
	std::string saved_stream_download_;
	int saved_stream_generation_ = 0;
	double saved_stream_poll_at_ = 0;
	bool saved_stream_resuming_ = false;
	std::string d_res_;                 // the resolution being shown (when !d_pick_res)
	std::vector<std::string> d_res_keys_;  // resolution list: row -> resolution
	std::vector<int> d_view_;           // stream list: row -> d_stream_list_ index
	int d_res_sel_ = 0;                 // row to return to in the resolution list
	std::string d_streams_base_title_;
	void torrent_stats_start(const std::string& url, const std::string& info_hash);
	void torrent_stats_poll();
	std::shared_ptr<std::atomic<bool>> launch_cancel_;
	std::string t_stats_url_;
	double t_next_poll_ = 0;
	bool t_polling_ = false;
	int t_gen_ = 0;
	bool watched(const std::string& video_id) const;
	void toggle_watched(const std::string& video_id);
	std::string episode_label(const Video& v) const;
	const Video* detail_next_episode(const std::string& video_id) const;
	bool detail_select_episode(const std::string& video_id);

	// app_settings.cpp
	void enter_settings();
	void settings_refresh();
	void settings_button(Btn b);
	void open_download_directory_picker();
	void close_download_directory_picker(bool shutting_down = false);
	void browse_download_directory(const std::string& path, bool nearest = false,
	                               const std::string& focus = "");
	void directory_picker_button(Btn b);
	void directory_picker_tick();
	int directory_picker_generation_ = 0;
	bool directory_picker_shutdown_ = false;
	std::shared_ptr<std::atomic<bool>> directory_picker_cancel_;
	void open_language_checklist(bool audio);
	void open_date_checklist();
	void refresh_clock();
	void login_start();
	void login_poll();
	void login_close();
	void login_done(const std::string& auth_key);
	void sign_out();
	void open_input(const std::string& title, const std::string& value, const std::string& hint,
	                std::function<void(const std::string&)> done);
	void input_poll();
	void input_finish(bool ok);

	// app_watch.cpp
	// direct: the addon's own link (not through the streaming server)
	void watch_start(const std::string& url, const std::vector<std::string>& headers, double start,
	                 const std::string& title, const std::string& subtitle, bool direct = false);
	void watch_update();
	void watch_button(Btn b);
	void watch_seek(double target);
	void watch_stop(bool ended);
	void watch_save_progress(bool final);
	void watch_build_menu();
	void watch_menu_button(Btn b);
	void watch_select_sub(int idx);
	void watch_load_addon_subtitles();
	void watch_add_subtitles(std::vector<SubtitleTrack> tracks);
	void watch_enrich_subtitle_params();
	void watch_auto_subtitles();
	void watch_next_episode();
	void watch_offer_next_episode();
	void watch_next_episode_tick();
	void watch_next_episode_button(Btn b);
	void watch_cancel_next_episode();
	void watch_finish_episode();
	bool w_ended_handled_ = false;
	std::string next_episode_id_, next_episode_series_, next_episode_from_, next_episode_download_id_;
	int next_episode_account_generation_ = 0;
	double next_episode_deadline_ = 0;
	void watch_show_info();

	// -------------------------------------------------------------------
	std::string base_dir_, data_dir_;
	bool offline_ = false;
	int account_generation_ = 0;
	bool settings_pending_ = false, settings_writing_ = false;
	double settings_save_at_ = 0;
	void flush_settings(bool synchronous = false);
	std::shared_ptr<std::mutex> settings_io_mutex_ = std::make_shared<std::mutex>();
	std::shared_ptr<std::atomic<uint64_t>> settings_io_epoch_ = std::make_shared<std::atomic<uint64_t>>(0);
	bool all_dirty_ = true;
	bool exit_ = false;
	double toast_until_ = 0;
	double last_clock_ = 0;
	int generation_ = 0;  // bumped to invalidate in-flight work for a page

	Settings settings_;
	std::map<std::string, Progress> progress_;  // by meta id
	std::map<std::string, Progress> video_progress_;  // by type, meta id and video id
	std::set<std::string> watched_;              // video ids
	std::string config_note_;

	// Addons
	std::vector<std::unique_ptr<Addon>> addons_;
	bool addons_loading_ = false;
	int addons_gen_ = 0;

	// Library (account)
	std::map<std::string, json> library_;  // libraryItem by _id
	bool library_loaded_ = false;
	std::map<std::string, json> library_pending_changes_;
	std::map<std::string, json> library_original_changes_;
	bool library_write_inflight_ = false;
	bool library_refresh_pending_ = false;
	uint64_t library_revision_ = 0;
	uint64_t library_load_generation_ = 0;

	// Board / search. The UI rows skip empty catalogs; the maps give the
	// board_/search_ index of each UI row.
	std::vector<BoardRow> board_;
	std::vector<int> home_map_;
	int home_gen_ = 0;
	std::shared_ptr<std::atomic<bool>> home_cancel_, search_cancel_;
	std::vector<BoardRow> search_;
	std::vector<int> search_map_;
	int search_gen_ = 0, search_pending_ = 0;

	// Discover
	struct DiscCatalog {
		const Addon* addon;
		const Catalog* catalog;
	};
	std::vector<std::string> disc_types_;
	std::string disc_type_;
	std::vector<DiscCatalog> disc_catalogs_;  // for disc_type_
	int disc_catalog_ = 0;
	std::string disc_genre_;  // "" = none
	std::map<std::string, std::string> disc_extra_values_;
	std::vector<std::string> disc_extra_keys_;
	std::string disc_filter_catalog_key_;
	size_t disc_skip_ = 0;  // raw provider records, before duplicate/invalid filtering
	std::shared_ptr<std::atomic<bool>> disc_cancel_;
	std::vector<Item> disc_items_;
	bool disc_loading_ = false, disc_end_ = false;
	int disc_gen_ = 0;

	// Library
	std::vector<Item> lib_items_;
	std::string lib_type_;  // "" = all
	int lib_sort_ = 0;      // 0 recent, 1 A-Z, 2 most watched

	// Previous place, for Circle on the detail page.
	std::string return_view_ = "home", return_zone_ = "content";

	// Detail
	Item d_item_;
	Meta d_meta_;
	bool d_meta_loaded_ = false;
	std::vector<int> d_seasons_;
	int d_season_idx_ = 0;
	std::vector<const Video*> d_season_videos_;
	std::string d_video_id_;  // streams are for this (meta id for movies)
	std::vector<Stream> d_stream_list_;
	std::vector<std::vector<Stream>> d_stream_buckets_;  // installed addon order
	std::vector<std::string> d_stream_errors_;
	std::vector<std::string> d_stream_source_names_, d_stream_source_urls_;
	std::vector<std::string> d_source_urls_;
	std::string d_source_preferred_;
	bool d_meta_pending_ = false;
	std::string d_pending_video_id_;
	std::vector<bool> d_source_answered_;
	std::shared_ptr<std::atomic<bool>> d_stream_cancel_, d_meta_cancel_;
	int d_gen_ = 0, d_pending_ = 0, d_answered_ = 0;  // streams
	int d_meta_gen_ = 0;

	// Dropdown
	std::function<void(int)> dd_chosen_;

	// Login
	std::string login_code_raw_, login_qr_url_;
	double login_next_poll_ = 0, login_expires_ = 0;
	bool login_polling_ = false;
	int login_gen_ = 0;

	// Input
	std::function<void(const std::string&)> input_done_;

	// Watch
	Player player_;
	Item w_item_;
	std::string w_video_id_;
	Stream w_stream_;
	std::string w_media_url_;
	std::vector<std::string> w_media_headers_;
	std::shared_ptr<std::atomic<bool>> w_sub_cancel_;
	std::set<std::string> w_sub_requests_;  // prevents repeated requests with the same extras
	int w_sub_pending_ = 0;
	bool w_sub_hash_pending_ = false;
	bool w_transcode_ = false;
	double w_info_until_ = 0;
	double w_last_save_ = 0;
	double w_start_ = 0;
	double w_observed_position_ = -1, w_observed_duration_ = 0;
	int64_t w_observed_download_prefix_ = 0, w_observed_download_total_ = -1;
	bool w_hold_download_bookmark_ = false;
	bool w_started_ = false;
	bool w_frame_presented_ = false;
	unsigned w_frame_failures_ = 0;
	bool w_heavy_warned_ = false;
	struct WatchSub {
		std::string label, lang;
		std::string id, addon;
		int embedded = -1;       // stream index, or
		std::string url;         // addon subtitle
		std::vector<Cue> cues;   // loaded
		bool loading = false, failed = false;
	};
	std::vector<WatchSub> w_subs_;
	int w_sub_active_ = -1;      // index in w_subs_, -1 off
	double w_sub_delay_ = 0;
	bool w_menu_built_ = false;
	bool w_auto_sub_done_ = false;
	int w_gen_ = 0;
	double w_last_ui_ = 0;
	double w_last_saved_pos_ = -1;
	std::string w_sub_shown_;

	// Next episode: play a stream like the last one once streams arrive.
	bool autoplay_pending_ = false;
	std::string autoplay_binge_, autoplay_addon_;
};
