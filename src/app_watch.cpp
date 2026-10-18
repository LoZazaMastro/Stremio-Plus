// Watching: the player's overlays, tracks, subtitles and progress.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "app.h"
#include "download_metadata.h"
#include "http.h"
#include "torrent/engine.h"
#include "torrent/torrent_stream.h"

static const double kInfoSeconds = 5;
static const double kSaveEvery = 30;

namespace {
std::string progressive_error_it(const std::string& error) {
	if (error.find("download changed") != std::string::npos)
		return "Il download è cambiato. Avvia di nuovo la riproduzione.";
	if (error.find("requested video data is not downloaded yet") != std::string::npos)
		return "Quel punto del video non è ancora disponibile. Lascia proseguire il download e riprova.";
	if (error.find("playback metadata is not downloaded yet") != std::string::npos)
		return "I dati necessari per aprire questo video non sono ancora disponibili. Lascia proseguire il download e riprova.";
	if (error.find("download restarted") != std::string::npos || error.find("download source changed") != std::string::npos)
		return "Il download è ripartito. Avvia di nuovo la riproduzione.";
	if (error.find("download was removed") != std::string::npos)
		return "Il download è stato eliminato.";
	if (error.find("downloaded portion has ended") != std::string::npos)
		return "Hai raggiunto la fine della parte scaricata. Riprendi il download per continuare a guardare.";
	if (error.find("download stopped before") != std::string::npos || error.find("download has not provided more data") != std::string::npos)
		return "Il download non sta ricevendo altri dati. Riprendilo dalla sezione Download.";
	if (error.find("downloaded portion cannot seek") != std::string::npos)
		return "Quel punto del video non è ancora disponibile. Lascia proseguire il download e riprova.";
	if (error.find("downloaded file") != std::string::npos)
		return "La parte scaricata del file non è disponibile per la riproduzione.";
	return "Impossibile avviare questo video. Lascia proseguire il download e riprova.";
}
// A subtitle/hash reader must never keep a pool worker blocked indefinitely
// waiting for a rare torrent piece. This deadline also follows playback cancel.
class TorrentAuxReader {
public:
	TorrentAuxReader(const std::string& hash, int index, std::shared_ptr<std::atomic<bool>> cancel, int seconds)
	    : index_(index), parent_cancel_(std::move(cancel)) {
		torrent_ = bt::Engine::get().open_reader(hash, index, &reader_, &size_, &error_, bt::ReaderRole::Auxiliary);
		if (!torrent_) return;
		deadline_thread_ = std::thread([this, seconds]() {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
			std::unique_lock<std::mutex> lock(mutex_);
			while (!finished_.load()) {
				if ((parent_cancel_ && parent_cancel_->load()) || std::chrono::steady_clock::now() >= deadline) {
					abort_.store(true);
					return;
				}
				cv_.wait_for(lock, std::chrono::milliseconds(50));
			}
		});
	}
	~TorrentAuxReader() {
		finished_.store(true);
		cv_.notify_all();
		if (deadline_thread_.joinable()) deadline_thread_.join();
		if (torrent_) bt::Engine::get().close_reader(torrent_, reader_);
	}
	int64_t size() const { return size_; }
	const std::string& error() const { return error_; }
	bool read(int64_t offset, size_t bytes, std::string& data) {
		data.clear();
		if (!torrent_ || offset < 0 || offset > size_ || bytes > uint64_t(size_ - offset) || bytes > 8u * 1024 * 1024) {
			if (error_.empty()) error_ = "The torrent subtitle is invalid or exceeds 8 MiB";
			return false;
		}
		data.resize(bytes);
		size_t copied = 0;
		while (copied < bytes) {
			if (parent_cancel_ && parent_cancel_->load()) abort_.store(true);
			int count = abort_.load() ? -1 : bt::Engine::get().read(torrent_, reader_, index_, offset + copied,
			    reinterpret_cast<uint8_t*>(&data[copied]), int(bytes - copied), &abort_);
			if (count <= 0) {
				error_ = parent_cancel_ && parent_cancel_->load() ? "cancelled" :
				         abort_.load() ? "Timed out waiting for torrent subtitle data" : "Torrent data is unavailable";
				data.clear();
				return false;
			}
			copied += size_t(count);
		}
		return true;
	}
private:
	int index_ = -1, reader_ = -1;
	int64_t size_ = 0;
	std::string error_;
	std::shared_ptr<bt::Torrent> torrent_;
	std::shared_ptr<std::atomic<bool>> parent_cancel_;
	std::atomic<bool> abort_{false}, finished_{false};
	std::mutex mutex_;
	std::condition_variable cv_;
	std::thread deadline_thread_;
};
}  // namespace

void App::watch_start(const std::string& url, const std::vector<std::string>& headers, double start,
                      const std::string& title, const std::string& subtitle, bool direct) {
	watch_cancel_next_episode();
	w_ended_handled_ = false;
	if (w_sub_cancel_) w_sub_cancel_->store(true);
	w_sub_cancel_ = std::make_shared<std::atomic<bool>>(false);
	w_media_url_ = url;
	w_media_headers_ = headers;
	w_sub_requests_.clear();
	w_sub_pending_ = 0;
	w_sub_hash_pending_ = false;
	Player::Options o;
	o.url = url;
	o.parallel = direct;
	o.headers = headers;
	o.start = start;
	o.audio_langs = pref_audio_langs();
	o.growing_download = w_growing_download_;
	o.progressive_resume_limit = w_growing_download_ ? start : 0;
	if (before_playback) before_playback();
	player_.open(o);

	watching_ = true;
	launch_visible = true;
	launch_status = w_offline_ ? (settings_.ui_language == "it" ? "Apertura file..." : "Opening file...") : "Opening stream...";
	launch_progress = -1;
	w_seekable_until = w_growing_download_ ? 0 : -1;
	launch_logo = w_offline_ ? w_download_logo_ : art(w_item_.logo, ArtKind::LaunchLogo);
	w_title = title;
	w_subtitle = subtitle;
	w_sub_size = settings_.sub_size;
	w_start_ = start;
	w_observed_position_ = -1; w_observed_duration_ = 0;
	w_observed_download_prefix_ = 0; w_observed_download_total_ = -1;
	w_hold_download_bookmark_ = false;
	w_started_ = false;
	w_frame_presented_ = false;
	w_frame_failures_ = 0;
	w_heavy_warned_ = false;
	w_menu_built_ = false;
	w_auto_sub_done_ = false;
	w_subs_.clear();
	w_sub_active_ = -1;
	w_sub_delay_ = settings_.subtitle_offset_ms / 1000.0;
	w_sub_rml.clear();
	w_sub_shown_.clear();
	w_last_save_ = now_seconds();
	w_last_saved_pos_ = start;
	menu_visible = false;
	w_paused = false;
	info_visible = true;
	w_info_until_ = now_seconds() + kInfoSeconds;
	w_progress = "0%";
	w_time = format_time(start);
	w_duration.clear();
	w_tracks.clear();
	w_stats.clear();
	watch_add_subtitles(w_stream_.subtitles);
	watch_load_addon_subtitles();
	watch_enrich_subtitle_params();
	dirty_all();
}

// ---------------------------------------------------------------------------
// Torrent statistics (peers, speed, how much of the file is there)

void App::torrent_stats_start(const std::string& url, const std::string& info_hash) {
	t_gen_++;
	t_stats_url_ = url;
	t_hash = info_hash;
	t_peers = "0";
	t_speed = "0.00 MiB/s";
	t_progress = "0.00 %";
	t_visible = true;
	t_next_poll_ = 0;
	t_polling_ = false;
	dirty_all();
}

void App::torrent_stats_poll() {
	// Only while the stream starts or buffers, as the panel is shown then.
	if (!launch_visible && !w_buffering) return;
	if (t_polling_ || now_seconds() < t_next_poll_) return;
	t_polling_ = true;
	int gen = t_gen_;
	std::string url = t_stats_url_;
	struct Stats {
		bool ok = false;
		double peers = 0, speed = 0, progress = -1;
		int known = -1;  // built-in engine only
		bool has_meta = false, incoming = false;
	};
	g_tasks.run<Stats>(
	    [url]() {
		    Stats s;
		    if (TorrentStream::is_url(url)) {  // the built-in engine
			    bt::Stats es = bt::Engine::get().stats(url.substr(10, 40));
			    s.ok = es.found;
			    s.peers = es.peers;
			    s.speed = es.download_rate;
			    s.progress = es.file_progress;
			    s.known = es.known_peers;
			    s.has_meta = es.has_metadata;
			    s.incoming = es.incoming;
			    return s;
		    }
		    HttpResponse h = http_get(url, 5);
		    if (!h.ok()) return s;
		    json j = json::parse(h.body, nullptr, false);
		    if (!j.is_object()) return s;
		    s.ok = true;
		    s.peers = jnum(j, "peers");
		    s.speed = jnum(j, "downloadSpeed");
		    if (j.contains("streamProgress")) s.progress = jnum(j, "streamProgress");
		    return s;
	    },
	    [this, gen](Stats& s) {
		    t_polling_ = false;
		    t_next_poll_ = now_seconds() + 1;
		    if (gen != t_gen_ || !s.ok) return;
		    char buf[32];
		    t_peers = std::to_string(int(s.peers));
		    snprintf(buf, sizeof(buf), "%.2f MiB/s", s.speed / (1024 * 1024));
		    t_speed = buf;
		    if (s.progress >= 0) {
			    snprintf(buf, sizeof(buf), "%.2f %%", s.progress * 100);
			    t_progress = buf;
		    }
		    dirty("t_peers");
		    dirty("t_speed");
		    dirty("t_progress");
		    // Built-in engine, before playback starts: say what it's waiting for.
		    if (s.known >= 0 && launch_visible && !watching_ && !s.has_meta) {
			    if (s.known == 0) launch_status = "Finding peers...";
			    else if (s.peers == 0)
				    launch_status = "Finding peers... " + std::to_string(s.known) + " found, none answering yet";
			    else launch_status = "Getting the file list from " + std::to_string(int(s.peers)) + " peers...";
			    dirty("launch_status");
		    }
	    });
}

void App::watch_show_info() {
	w_info_until_ = now_seconds() + kInfoSeconds;
	if (!info_visible) {
		info_visible = true;
		dirty("info_visible");
	}
}

void App::video_frame_presented() {
	if (watching_ && !w_ended_handled_) {
		w_frame_presented_ = true;
		w_frame_failures_ = 0;
	}
}

void App::video_frame_rejected() {
	if (!watching_ || w_ended_handled_ || ++w_frame_failures_ < 3) return;
	dlog("video: GPU output rejected three consecutive decoded pictures; stopping playback");
	watch_stop(false);
	show_toast("The video frame could not be displayed.", 8);
}

void App::watch_update() {
	// A resolver can still show the launch overlay after the previous player
	// reached Ended. Only an actually opened player owns an end-of-file event.
	if (!watching_ || w_ended_handled_) return;
	double now = now_seconds();
	Player::State st = player_.state();
	player_.log_stats();

	if (st == Player::State::Failed) {
		std::string err = player_.error();
		const bool progressive_italian = bool(w_growing_download_) && settings_.ui_language == "it";
		if (progressive_italian) err = progressive_error_it(err);
		watch_stop(false);
		show_toast((progressive_italian ? "Riproduzione non disponibile: " : "Playback failed: ") + err +
		               (w_stream_.info_hash.empty() ? "" : ". The torrent may have no seeders; try another stream."),
		           8);
		return;
	}
	if (st == Player::State::Ended) {
		watch_finish_episode();
		return;
	}

	int pct = 0;
	bool buffering = player_.buffering(&pct);
	const double seek_limit = player_.progressive_download() ? player_.seekable_until() : -1;
	if (seek_limit != w_seekable_until) {
		w_seekable_until = seek_limit;
		dirty("w_seekable_until");
	}
	if (launch_visible) {
		std::string s;
		launch_progress = buffering ? std::clamp(pct, 0, 100) / 100.0f : -1.0f;
		if (st == Player::State::Opening)
			s = w_offline_ ? (settings_.ui_language == "it" ? "Apertura file..." : "Opening file...") : "Opening stream...";
		else if (buffering) s = "Buffering " + std::to_string(pct) + "%";
		if (!s.empty() && s != launch_status) {
			launch_status = s;
			dirty("launch_status");
		}
		if (w_frame_presented_ ||
		    (!player_.video_expected() && st == Player::State::Playing && !buffering)) {
			launch_progress = 1;
			launch_visible = false;
			w_started_ = true;
			watch_show_info();
			dirty_all();
		}
	}

	if (st == Player::State::Playing && !w_menu_built_) {
		// The file is open: its own subtitle tracks go first.
		w_menu_built_ = true;
		std::vector<WatchSub> embedded;
		for (auto& t : player_.subtitle_tracks()) {
			WatchSub s;
			s.label = t.label + " (in file)";
			s.lang = t.lang;
			s.embedded = t.stream;
			embedded.push_back(s);
		}
		w_subs_.insert(w_subs_.begin(), embedded.begin(), embedded.end());
		if (w_sub_active_ >= 0) w_sub_active_ += int(embedded.size());
		watch_auto_subtitles();
	}

	double pos = player_.position();
	double dur = player_.duration();
	if (st == Player::State::Playing && w_started_ && std::isfinite(pos) && pos >= 0 &&
	    std::isfinite(dur) && (dur > 0 || (w_offline_ && dur == 0))) {
		w_observed_position_ = pos; w_observed_duration_ = dur;
		if (w_growing_download_ && w_growing_download_->state) {
			const auto prefix = w_growing_download_->state->snapshot();
			if (prefix.generation == w_growing_download_->generation && prefix.total == w_growing_download_->total) {
				w_observed_download_prefix_ = prefix.available;
				w_observed_download_total_ = prefix.total;
			}
		}
	}

	// Subtitles, every frame.
	std::string sub;
	player_.select_subtitle(w_sub_active_ >= 0 && w_sub_active_ < int(w_subs_.size())
	    ? w_subs_[w_sub_active_].embedded : -1);
	if (w_sub_active_ >= 0 && w_sub_active_ < int(w_subs_.size())) {
		WatchSub& ws = w_subs_[w_sub_active_];
		double t = pos - w_sub_delay_;
		if (ws.embedded >= 0) sub = player_.embedded_subtitle(ws.embedded, t);
		else if (!ws.cues.empty()) sub = cues_at(ws.cues, t);
	}
	if (sub != w_sub_shown_) {
		w_sub_shown_ = sub;
		w_sub_rml = sub;
		dirty("w_sub_rml");
	}

	if (now - w_last_ui_ >= 0.25) {
		w_last_ui_ = now;
		char pbuf[32];
		snprintf(pbuf, sizeof(pbuf), "%.2f%%", dur > 0 ? std::min(100.0, pos * 100 / dur) : 0.0);
		w_progress = pbuf;
		w_time = format_time(pos);
		w_duration = dur > 0 ? format_time(dur) : "";
		std::string bt = buffering && !launch_visible ? "Buffering " + std::to_string(pct) + "%" : "";
		bool show_buf = !bt.empty() && w_started_;
		if (show_buf != w_buffering || bt != w_buffer_text) {
			w_buffering = show_buf;
			w_buffer_text = bt;
			dirty("w_buffering");
			dirty("w_buffer_text");
		}
		if (player_.paused() != w_paused) {
			w_paused = player_.paused();
			dirty("w_paused");
		}
		if (info_visible) {
			std::string audio, subs = "Off";
			for (auto& t : player_.audio_tracks())
				if (t.stream == player_.audio_stream()) audio = t.label;
			if (w_sub_active_ >= 0 && w_sub_active_ < int(w_subs_.size())) subs = w_subs_[w_sub_active_].label;
			w_tracks = "Audio: " + (audio.empty() ? "-" : audio) + "     Subtitles: " + subs;
			if (w_sub_delay_ != 0) {
				char d[32];
				snprintf(d, sizeof(d), " (%+.2fs)", w_sub_delay_);
				w_tracks += d;
			}
			w_stats = player_.stats();
			if (!w_stream_.name.empty()) w_stats = replace_all(w_stream_.name, "\n", " ") + " · " + w_stats;
		}
		dirty("w_progress");
		dirty("w_time");
		dirty("w_duration");
		dirty("w_tracks");
		dirty("w_stats");
	}

	if (info_visible && !menu_visible && !player_.paused() && now > w_info_until_) {
		info_visible = false;
		dirty("info_visible");
	}

	if (w_started_ && now - w_last_save_ > kSaveEvery) {
		w_last_save_ = now;
		watch_save_progress(false);
	}

	if (player_.too_heavy() && !w_heavy_warned_) {
		w_heavy_warned_ = true;
		show_toast("This video is too heavy to decode smoothly. Pick a lower-resolution stream.",
		           10);
	}
}

void App::watch_stop(bool ended) {
	watch_cancel_next_episode();
	if (watching_ && w_started_ && !ended && !w_ended_handled_) watch_save_progress(true);
	if (ended && !w_video_id_.empty() && watched_.insert(w_video_id_).second) {
		save_progress();
	}
	w_gen_++;
	if (w_sub_cancel_) w_sub_cancel_->store(true);
	player_.close();
	w_growing_download_.reset();
	if (w_torrent_slot_) { downloads_.set_torrent_playback_active(false); w_torrent_slot_ = false; }
	w_offline_ = false;
	w_download_id_.clear(); w_download_poster_.clear();
	w_download_background_.clear(); w_download_logo_.clear();
	watching_ = false;
	w_frame_presented_ = false;
	w_frame_failures_ = 0;
	launch_visible = false;
	launch_progress = -1;
	t_visible = false;
	menu_visible = false;
	w_sub_rml.clear();
	w_sub_shown_.clear();
	w_buffering = false;
	w_seekable_until = -1;
	w_paused = false;
	// Back on the detail page: refresh what changed.
	if (view == "detail") {
		detail_update_resume();
		for (size_t i = 0; i < d_episodes.size() && i < d_season_videos_.size(); i++)
			d_episodes[i].watched = watched(d_season_videos_[i]->id);
	}
	dirty_all();
}

void App::watch_save_progress(bool final) {
	double pos = player_.position(), dur = player_.duration();
	const auto state = player_.state();
	if ((state == Player::State::Failed || state == Player::State::Idle || !std::isfinite(pos) ||
	     !std::isfinite(dur) || dur <= 0) && w_observed_position_ >= 0) {
		pos = w_observed_position_; dur = w_observed_duration_;
	}
	if (w_growing_download_) {
		// This sample was taken after presentation, together with its available
		// prefix. A pending seek target cannot establish progressive provenance.
		if (w_observed_position_ < 0) return;
		pos = w_observed_position_; dur = w_observed_duration_;
	}
	if (!std::isfinite(pos) || pos < 0 || !std::isfinite(dur) || dur < 0 ||
	    (dur == 0 && !w_offline_) || w_item_.id.empty()) return;
	const auto video_key = video_progress_key(w_item_.type, w_item_.id, w_video_id_);
	if (w_hold_download_bookmark_) {
		const auto saved = video_progress_.find(video_key);
		if (saved != video_progress_.end() && pos < saved->second.time) return;
		w_hold_download_bookmark_ = false;
	}
	bool finished = dur > 0 && pos > dur * 0.9;
	if (!final && pos < 1) return;

	Progress& p = progress_[w_item_.id];
	p.type = w_item_.type;
	p.name = w_item_.name;
	p.poster = w_item_.poster;
	p.video_id = w_video_id_;
	p.time = finished ? 0 : pos;
	p.duration = dur;
	p.updated = now_epoch_ms();
	p.download_id.clear(); p.download_source.clear(); p.download_prefix = 0; p.download_total = -1;
	if (w_offline_ && !w_download_id_.empty()) {
		const auto entry = downloads_.find(w_download_id_);
		if (entry && !entry->source_identity.empty()) {
			if (w_growing_download_ && w_observed_position_ >= 0 && pos <= w_observed_position_ &&
			    w_observed_download_total_ > 0 && w_observed_download_prefix_ > 0) {
				p.download_prefix = w_observed_download_prefix_; p.download_total = w_observed_download_total_;
			} else if (entry->state == DownloadState::Complete) {
				p.download_prefix = entry->done; p.download_total = entry->total;
			}
			if (p.download_prefix > 0) { p.download_id = entry->id; p.download_source = entry->source_identity; }
		}
	}
	video_progress_[video_key] = p;
	if (finished) watched_.insert(w_video_id_);
	save_progress();

	double delta = w_last_saved_pos_ >= 0 ? std::max(0.0, std::min(pos - w_last_saved_pos_, kSaveEvery * 2)) : 0;
	w_last_saved_pos_ = pos;

	if (!signed_in() || w_offline_) return;
	std::string now = iso8601_now();
	json item;
	auto it = library_.find(w_item_.id);
	if (it != library_.end()) {
		item = it->second;
	} else {
		item = json{{"_id", w_item_.id},
		            {"name", w_item_.name},
		            {"type", w_item_.type},
		            {"poster", w_item_.poster},
		            {"posterShape", "poster"},
		            {"removed", true},
		            {"temp", true},
		            {"_ctime", now},
		            {"state", json::object()},
		            {"behaviorHints", {{"defaultVideoId", nullptr}, {"featuredVideoId", nullptr}, {"hasScheduledVideos", false}}}};
	}
	json& s = item["state"];
	if (!s.is_object()) s = json::object();
	s["lastWatched"] = now;
	s["timeOffset"] = int64_t(finished ? 0 : pos * 1000);
	s["duration"] = int64_t(dur * 1000);
	s["video_id"] = w_video_id_;
	s["timeWatched"] = int64_t(jnum(s, "timeWatched") + delta * 1000);
	s["overallTimeWatched"] = int64_t(jnum(s, "overallTimeWatched") + delta * 1000);
	if (!s.contains("timesWatched")) s["timesWatched"] = 0;
	if (!s.contains("flaggedWatched")) s["flaggedWatched"] = 0;
	if (!s.contains("noNotif")) s["noNotif"] = false;
	if (!s.contains("watched")) s["watched"] = nullptr;
	if (finished && final) {
		s["timesWatched"] = int(jnum(s, "timesWatched")) + 1;
		if (w_item_.type == "movie") s["flaggedWatched"] = 1;
	}
	item["_mtime"] = now;
	queue_library_change(item);
}

// ---------------------------------------------------------------------------
// Input

void App::watch_seek(double target) {
	if (!std::isfinite(target)) return;
	target = std::max(0.0, target);
	if (player_.progressive_download()) {
		const double limit = player_.seekable_until();
		target = std::min(target, std::isfinite(limit) ? std::max(0.0, limit) : 0.0);
	}
	player_.seek(target);
}

void App::watch_button(Btn b) {
	if (launch_visible) {
		if (b == Btn::Circle) watch_stop(false);
		return;
	}
	// Back dismisses the visible transport before it can stop playback. Handle
	// it before the usual reveal action, otherwise a hidden bar would be shown
	// again and the two-stage Back behavior could never reach the exit path.
	if (b == Btn::Circle) {
		if (info_visible) {
			info_visible = false;
			w_info_until_ = 0;
			dirty("info_visible");
		} else {
			watch_stop(false);
		}
		return;
	}
	watch_show_info();
	switch (b) {
	case Btn::Cross:
	case Btn::Touchpad: player_.set_paused(!player_.paused()); break;
	case Btn::Up:
	case Btn::Down: break;  // reveal controls only; Options opens audio/subtitles
	case Btn::L3: watch_seek(0); break;  // from the start
	case Btn::Left: watch_seek(player_.position() - settings_.seek_seconds); break;
	case Btn::Right: watch_seek(player_.position() + settings_.seek_seconds); break;
	case Btn::L1: watch_seek(player_.position() - settings_.shoulder_seek_seconds); break;
	case Btn::R1: watch_seek(player_.position() + settings_.shoulder_seek_seconds); break;
	case Btn::Square: {
		// Cycle: off -> each subtitle -> off
		int n = int(w_subs_.size());
		if (n == 0) {
			dlog("No subtitles available for the selected video");
			break;
		}
		int next = w_sub_active_ + 1;
		if (next >= n) next = -1;
		watch_select_sub(next);
		break;
	}
	case Btn::Triangle: {
		auto tracks = player_.audio_tracks();
		if (tracks.size() < 2) {
			show_toast(tracks.empty() ? "No audio" : "Only one audio track");
			break;
		}
		size_t cur = 0;
		for (size_t i = 0; i < tracks.size(); i++)
			if (tracks[i].stream == player_.audio_stream()) cur = i;
		auto& t = tracks[(cur + 1) % tracks.size()];
		player_.select_audio(t.stream);
		show_toast("Audio: " + t.label);
		break;
	}
	case Btn::Options:
		watch_build_menu();
		menu_visible = true;
		dirty_all();
		break;
	case Btn::R3:
		w_sub_requests_.clear();
		watch_load_addon_subtitles();
		watch_enrich_subtitle_params();
		dlog("Subtitle refresh requested");
		break;
	case Btn::L2:
		w_sub_delay_ -= 0.25;
		dlog("Subtitle delay: %d ms", int(std::lround(w_sub_delay_ * 1000)));
		break;
	case Btn::R2:
		w_sub_delay_ += 0.25;
		dlog("Subtitle delay: %d ms", int(std::lround(w_sub_delay_ * 1000)));
		break;
	default: break;
	}
	dirty("w_paused");
}

void App::watch_build_menu() {
	m_audio.clear();
	int audio_sel = 0;
	auto tracks = player_.audio_tracks();
	for (size_t i = 0; i < tracks.size(); i++) {
		bool active = tracks[i].stream == player_.audio_stream();
		if (active) audio_sel = int(i);
		m_audio.push_back({tracks[i].label, active, language_to_iso639_2(tracks[i].lang)});
	}
	m_subs.clear();
	m_subs.push_back({"Off", w_sub_active_ < 0});
	for (size_t i = 0; i < w_subs_.size(); i++) {
		std::string label = w_subs_[i].label;
		if (w_subs_[i].loading) label += "  (loading)";
		if (w_subs_[i].failed) label += "  (failed)";
		m_subs.push_back({label, int(i) == w_sub_active_, language_to_iso639_2(w_subs_[i].lang)});
	}
	if (!menu_visible) {
		m_audio_sel = audio_sel;
		m_sub_sel = w_sub_active_ + 1;
		m_col = tracks.size() > 1 ? 0 : 1;
	}
	m_audio_sel = std::min(m_audio_sel, std::max(0, int(m_audio.size()) - 1));
	m_sub_sel = std::min(m_sub_sel, int(m_subs.size()) - 1);
	char d[64];
	snprintf(d, sizeof(d), "Subtitle delay %+.2fs", w_sub_delay_);
	m_delay = d;
}

void App::watch_menu_button(Btn b) {
	switch (b) {
	case Btn::Left:
	case Btn::L1: m_col = 0; break;
	case Btn::Right:
	case Btn::R1: m_col = 1; break;
	case Btn::Up:
		if (m_col == 0 && m_audio_sel > 0) m_audio_sel--;
		if (m_col == 1 && m_sub_sel > 0) m_sub_sel--;
		break;
	case Btn::Down:
		if (m_col == 0 && m_audio_sel + 1 < int(m_audio.size())) m_audio_sel++;
		if (m_col == 1 && m_sub_sel + 1 < int(m_subs.size())) m_sub_sel++;
		break;
	case Btn::Cross:
		if (m_col == 0) {
			auto tracks = player_.audio_tracks();
			if (m_audio_sel < int(tracks.size())) player_.select_audio(tracks[m_audio_sel].stream);
			for (size_t i = 0; i < m_audio.size(); i++) m_audio[i].active = int(i) == m_audio_sel;
		} else {
			watch_select_sub(m_sub_sel - 1);
		}
		break;
	case Btn::L2: w_sub_delay_ -= 0.25; break;
	case Btn::R2: w_sub_delay_ += 0.25; break;
	case Btn::Circle:
	case Btn::Options:
	case Btn::Touchpad:
		menu_visible = false;
		watch_show_info();
		break;
	default: break;
	}
	if (menu_visible) watch_build_menu();
	dirty_all();
}

// ---------------------------------------------------------------------------
// Subtitles

void App::watch_select_sub(int idx) {
	w_auto_sub_done_ = true;  // an explicit Off/selection stays in effect after refresh
	if (idx < 0 || idx >= int(w_subs_.size())) {
		w_sub_active_ = -1;
		dlog("Subtitles disabled");
		if (menu_visible) watch_build_menu();
		dirty_all();
		return;
	}
	w_sub_active_ = idx;
	WatchSub& ws = w_subs_[idx];
	dlog("Subtitle track selected");
	if (ws.embedded < 0 && ws.cues.empty() && !ws.loading) {
		ws.loading = true;
		ws.failed = false;
		std::string url = ws.url, lang = ws.lang;
		std::string active_hash;
		int active_index = -1;
		TorrentStream::parse_url(w_media_url_, &active_hash, &active_index);
		std::string srv = server();
		auto cancel = w_sub_cancel_;
		int gen = w_gen_;
		struct Res {
			std::vector<Cue> cues;
			std::string error;
		};
		bg<Res>(
		    [url, lang, active_hash, srv, cancel]() {
			    Res r;
			    std::string body, resolved_url = url, subtitle_hash;
			    int subtitle_index = -1;
			    if (parse_local_torrent_subtitle_url(url, subtitle_hash, subtitle_index)) {
				    if (!active_hash.empty() && subtitle_hash == active_hash) {
					    // This auxiliary reader never starts/replaces a torrent and
					    // never changes the video selected for playback.
					    TorrentAuxReader reader(subtitle_hash, subtitle_index, cancel, 20);
					    if (reader.size() <= 0 || reader.size() > 8 * 1024 * 1024 ||
					        !reader.read(0, size_t(reader.size()), body)) {
						    r.error = reader.error().empty() ? "Torrent subtitle is empty or exceeds 8 MiB" : reader.error();
						    return r;
					    }
				    } else if (!srv.empty()) {
					    resolved_url = srv + "/" + subtitle_hash + "/" + std::to_string(subtitle_index);
				    } else {
					    r.error = "This subtitle belongs to a torrent that is not playing; a streaming server is needed";
					    return r;
				    }
			    }
			    if (body.empty()) {
				    HttpResponse h = http_get(resolved_url, 30, cancel.get(), {}, 8u * 1024 * 1024);
				    if (!h.ok()) { r.error = h.describe(); return r; }
				    body = std::move(h.body);
			    }
			    r.cues = parse_subtitle_file(to_utf8(body, lang));
			    if (r.cues.empty()) r.error = "That subtitle file is empty or unreadable";
			    return r;
		    },
		    [this, gen, url](Res& r) {
			    if (gen != w_gen_) return;
			    for (auto& s : w_subs_) {
				    if (s.url != url) continue;
				    s.loading = false;
				    s.cues = r.cues;
				    s.failed = !r.error.empty();
			    }
			    if (!r.error.empty() && r.error != "cancelled") {
				    dlog("subtitle %s: %s", http_log_target(url).c_str(), r.error.c_str());
			    }
			    if (menu_visible) watch_build_menu();
			    dirty_all();
		    });
	}
	if (menu_visible) watch_build_menu();
	dirty_all();
}

void App::watch_load_addon_subtitles() {
	if (w_offline_) return;
	std::string type = w_item_.type, vid = w_video_id_;
	if (vid.empty()) return;
	const auto extras = stream_subtitle_extras(w_stream_);
	std::vector<std::pair<std::string, std::string>> sources;
	for (auto& a : addons_)
		if (a->supports("subtitles", type, vid)) {
			auto url = a->resource_url_with_extras("subtitles", type, vid, extras);
			if (w_sub_requests_.insert(url).second) sources.push_back({a->name, std::move(url)});
		}
	int gen = w_gen_;
	auto cancel = w_sub_cancel_;
	w_sub_pending_ += int(sources.size());
	for (auto& src : sources) {
		std::string name = src.first, url = src.second;
		struct Res { std::vector<SubtitleTrack> tracks; std::string error; };
		bg<Res>(
		    [name, url, cancel]() {
			    Res r;
			    json j;
			    if (fetch_json(url, j, r.error, 25, cancel.get())) r.tracks = parse_subtitles(j, name);
			    return r;
		    },
		    [this, gen, name](Res& r) {
			    if (gen != w_gen_) return;
			    w_sub_pending_ = std::max(0, w_sub_pending_ - 1);
			    if (!r.error.empty() && r.error != "cancelled") dlog("subtitle provider %s: %s", name.c_str(), r.error.c_str());
			    watch_add_subtitles(std::move(r.tracks));
		    });
	}
}

void App::watch_add_subtitles(std::vector<SubtitleTrack> tracks) {
	const auto prefs = pref_sub_langs();
	auto rank = [&](const std::string& lang) {
		auto it = std::find(prefs.begin(), prefs.end(), language_to_iso639_2(lang));
		return size_t(it - prefs.begin());
	};
	std::stable_sort(tracks.begin(), tracks.end(), [&](const auto& a, const auto& b) { return rank(a.lang) < rank(b.lang); });
	for (const auto& track : tracks) {
		const auto lang = language_to_iso639_2(track.lang);
		bool duplicate = false;
		for (auto& existing : w_subs_) {
			if (existing.embedded >= 0 || existing.lang != lang) continue;
			if (existing.url == track.url || (!track.id.empty() && existing.id == track.id && existing.addon == track.addon)) {
				if (!existing.loading && existing.cues.empty()) existing.url = track.url;
				duplicate = true;
				break;
			}
		}
		if (duplicate || w_subs_.size() >= 256) continue;
		WatchSub sub;
		sub.id = track.id;
		sub.addon = track.addon;
		sub.label = track.label.empty() ? language_name(track.lang) : track.label;
		if (!track.addon.empty()) sub.label += " · " + track.addon;
		sub.lang = lang;
		sub.url = track.url;
		w_subs_.push_back(std::move(sub));
	}
	watch_auto_subtitles();
	if (menu_visible) watch_build_menu();
	dirty_all();
}

void App::watch_enrich_subtitle_params() {
	if (w_offline_) return;
	if (w_sub_hash_pending_ || (!w_stream_.video_hash.empty() && w_stream_.has_video_size)) return;
	w_sub_hash_pending_ = true;
	// HLS transcoding changes the representation. Hash the original file,
	// which is what subtitle providers match, rather than a playlist/segment.
	std::string media = w_media_url_;
	std::vector<std::string> headers = w_media_headers_;
	if (w_transcode_) {
		if (!w_stream_.url.empty()) { media = w_stream_.url; headers = w_stream_.request_headers; }
		else if (!server().empty() && w_stream_.file_idx >= 0)
			media = server() + "/" + w_stream_.info_hash + "/" + std::to_string(w_stream_.file_idx);
	}
	auto cancel = w_sub_cancel_;
	const int gen = w_gen_;
	struct Res { std::string hash, error; uint64_t size = 0; };
	bg<Res>(
	    [media, headers, cancel]() {
		    Res r;
		    if (cancel && cancel->load()) return r;
		    std::string info_hash;
		    int index = -1;
		    if (TorrentStream::parse_url(media, &info_hash, &index)) {
			    TorrentAuxReader reader(info_hash, index, cancel, 15);
			    if (reader.size() > 0) r.size = uint64_t(reader.size());
			    std::string first, last;
			    if (r.size >= 131072 && reader.read(0, 65536, first) &&
			        reader.read(int64_t(r.size - 65536), 65536, last)) r.hash = opensubtitles_hash(first, last, r.size);
			    else r.error = reader.error();
		    } else if (http_valid_url(media)) {
			    http_opensubtitles_hash(media, headers, r.hash, r.size, r.error, cancel.get(), 8);
		    }
		    return r;
	    },
	    [this, gen](Res& r) {
		    if (gen != w_gen_ || !watching_) return;
		    w_sub_hash_pending_ = false;
		    bool changed = false;
		    if (w_stream_.video_hash.empty() && !r.hash.empty()) { w_stream_.video_hash = r.hash; changed = true; }
		    if (!w_stream_.has_video_size && r.size > 0) {
			    w_stream_.video_size = r.size;
			    w_stream_.has_video_size = true;
			    changed = true;
		    }
		    if (!r.error.empty() && r.error != "cancelled") dlog("subtitle file matching: %s", r.error.c_str());
		    if (changed) watch_load_addon_subtitles();
	    });
}

void App::watch_auto_subtitles() {
	if (!settings_.auto_subtitles || !w_menu_built_ || w_auto_sub_done_ || w_sub_active_ >= 0) return;
	const auto languages = pref_sub_langs();
	for (size_t priority = 0; priority < languages.size(); priority++) {
		const auto& lang = languages[priority];
		if (priority > 0 && w_sub_pending_ > 0) return;
		// Don't auto-pick subtitles in the language being heard.
		bool heard = false;
		for (auto& t : player_.audio_tracks())
			if (t.stream == player_.audio_stream() && language_to_iso639_2(t.lang) == lang) heard = true;
		if (heard) {
			w_auto_sub_done_ = true;
			return;
		}
		for (size_t i = 0; i < w_subs_.size(); i++) {
			if (w_subs_[i].lang != lang || w_subs_[i].failed) continue;
			w_auto_sub_done_ = true;
			watch_select_sub(int(i));
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Next episode

void App::watch_finish_episode() {
	if (!watching_ || w_ended_handled_) return;
	w_ended_handled_ = true;
	watch_save_progress(true);
	if (!w_video_id_.empty() && watched_.insert(w_video_id_).second) save_progress();
	if (settings_.autoplay_next && (w_item_.type == "series" || w_item_.type == "anime" || (!w_offline_ && d_series)))
		watch_offer_next_episode();
	if (!next_episode_visible) watch_stop(true);
}

void App::watch_offer_next_episode() {
	watch_cancel_next_episode();
	if (!settings_.autoplay_next) return;
	Video local_video;
	std::string local_art;
	const Video* next = nullptr;
	if (w_offline_) {
		const auto current = downloads_.find(w_download_id_);
		if (!current || current->state != DownloadState::Complete || current->media_id != w_item_.id) return;
		// Cached complete metadata can establish a season boundary. Without it,
		// only the exact adjacent numbered episode is safe to infer locally.
		const Video* expected = d_item_.id == w_item_.id ? detail_next_episode(w_video_id_) : nullptr;
		const bool metadata_knows_current = d_item_.id == w_item_.id &&
		    std::any_of(d_meta_.videos.begin(), d_meta_.videos.end(),
		        [this](const Video& video) { return video.id == w_video_id_; });
		for (const auto& entry : downloads_.snapshot()) {
			if (entry.state != DownloadState::Complete || entry.recovery_only || entry.media_id != current->media_id ||
			    entry.id == current->id || entry.video_id.empty() || entry.local_path.empty()) continue;
			const bool follows = expected ? entry.video_id == expected->id : !metadata_knows_current &&
			    current->season >= 0 && current->episode > 0 && entry.season == current->season &&
			    int64_t(entry.episode) == int64_t(current->episode) + 1;
			if (!follows) continue;
			local_video = expected ? *expected : Video{};
			local_video.id = entry.video_id;
			local_video.season = entry.season; local_video.episode = entry.episode;
			local_video.raw["season"] = entry.season;
			if (local_video.title.empty()) local_video.title = entry.episode_title.empty()
			    ? download_legacy_episode_title(entry.season, entry.episode, entry.subtitle) : entry.episode_title;
			next_episode_download_id_ = entry.id;
			local_art = entry.background_path.empty() ? entry.poster_path : entry.background_path;
			next = &local_video;
			break;
		}
	} else {
		if (view != "detail" || w_item_.id != d_item_.id) return;
		next = detail_next_episode(w_video_id_);
	}
	if (!next) return;
	next_episode_id_ = next->id;
	next_episode_series_ = w_item_.id;
	next_episode_from_ = w_video_id_;
	next_episode_account_generation_ = account_generation_;
	next_episode_title = next->title.empty() ? (settings_.ui_language == "it" ? "Episodio" : "Episode") : next->title;
	next_episode_label = next->season > 0 ? "S" + std::to_string(next->season) :
	    next->season == 0 && jobj(next->raw, "season").is_number() ? (settings_.ui_language == "it" ? "Speciale" : "Special") : "";
	if (next->episode > 0) next_episode_label += (next_episode_label.empty() ? "" : " · ") +
	    std::string(settings_.ui_language == "it" ? "Episodio " : "Episode ") + std::to_string(next->episode);
	const std::string thumbnail = next->thumbnail.empty() ? d_item_.background : next->thumbnail;
	next_episode_thumb = w_offline_ ? local_art :
	    g_art.get_async(thumbnail, ArtKind::Thumb, {}, ArtPriority::Immediate, this);
	next_episode_seconds = std::clamp(settings_.next_episode_delay_seconds, 5, 120);
	next_episode_deadline_ = now_seconds() + next_episode_seconds;
	next_episode_sel = 0;
	next_episode_visible = true;
	info_visible = menu_visible = launch_visible = t_visible = w_buffering = false;
	w_sub_rml.clear();
	w_sub_shown_.clear();
	dirty_all();
}

void App::watch_cancel_next_episode() {
	next_episode_visible = false;
	next_episode_seconds = 0;
	next_episode_deadline_ = 0;
	next_episode_id_.clear();
	next_episode_series_.clear();
	next_episode_from_.clear();
	next_episode_download_id_.clear();
	next_episode_thumb.clear();
}

void App::watch_next_episode_tick() {
	if (!next_episode_visible) return;
	if (download_relocation_active()) {
		if (watching_ && w_ended_handled_) watch_stop(true);
		else watch_cancel_next_episode();
		return;
	}
	const bool local = !next_episode_download_id_.empty();
	if (!watching_ || !settings_.autoplay_next || (!local && view != "detail") ||
	    next_episode_account_generation_ != account_generation_ ||
	    next_episode_series_ != w_item_.id || (!local && next_episode_series_ != d_item_.id) ||
	    next_episode_from_ != w_video_id_) {
		if (watching_ && w_ended_handled_) watch_stop(true);
		else watch_cancel_next_episode();
		return;
	}
	const int seconds = std::max(0, int(std::ceil(next_episode_deadline_ - now_seconds())));
	if (next_episode_seconds != seconds) { next_episode_seconds = seconds; dirty_all(); }
	// Cached lookups refresh the art without retaining a pointer into metadata
	// or a callback able to paint a different episode after the card is closed.
	if (!local && next_episode_thumb.empty()) {
		const auto found = std::find_if(d_meta_.videos.begin(), d_meta_.videos.end(),
		    [this](const Video& video) { return video.id == next_episode_id_; });
		if (found != d_meta_.videos.end()) {
			next_episode_thumb = g_art.peek_cached(found->thumbnail.empty() ? d_item_.background : found->thumbnail, ArtKind::Thumb);
			if (!next_episode_thumb.empty()) dirty_all();
		}
	}
	if (seconds == 0) watch_next_episode();
}

void App::watch_next_episode_button(Btn button) {
	if (!next_episode_visible) return;
	if (button == Btn::Circle || (button == Btn::Cross && next_episode_sel == 1)) {
		watch_stop(true);
		return;
	}
	if (button == Btn::Left || button == Btn::Right) {
		next_episode_sel = button == Btn::Right ? 1 : 0;
		dirty_all();
	} else if (button == Btn::Cross) watch_next_episode();
}

void App::watch_next_episode() {
	if (!next_episode_visible) return;
	if (download_relocation_active()) {
		if (watching_ && w_ended_handled_) watch_stop(true);
		else watch_cancel_next_episode();
		return;
	}
	const std::string id = next_episode_id_;
	const std::string download_id = next_episode_download_id_;
	const bool local = !download_id.empty();
	const bool valid = next_episode_account_generation_ == account_generation_ &&
	    next_episode_series_ == w_item_.id && next_episode_from_ == w_video_id_ &&
	    (local || (view == "detail" && next_episode_series_ == d_item_.id));
	const std::string binge = w_stream_.binge_group;
	const std::string addon = w_stream_.addon_url.empty() ? w_stream_.addon : w_stream_.addon_url;
	watch_stop(true);
	if (!valid) return;
	if (local) {
		const auto entry = downloads_.find(download_id);
		if (entry && entry->state == DownloadState::Complete && entry->video_id == id && !entry->recovery_only)
			start_download_playback(download_id, false, 0);
		return;
	}
	if (!detail_select_episode(id)) return;
	autoplay_pending_ = true;
	autoplay_binge_ = binge;
	autoplay_addon_ = addon;
	d_zone = "streams";
	detail_load_streams(id);
	dirty_all();
}
