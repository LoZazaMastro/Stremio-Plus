// Board (home) and search results: rows of posters, one per catalog.

#include <algorithm>
#include <cstdlib>

#include "app.h"

static const size_t kMaxRowCards = 50;

static std::string row_title(const Catalog& c) {
	std::string type = capitalize(c.type);
	return c.name.empty() ? type : c.name + " - " + type;
}

// "tt0903747:2:5" -> "S2E5"
static std::string episode_badge(const std::string& video_id) {
	auto parts = split(video_id, ':');
	if (parts.size() >= 3) {
		int s = atoi(parts[parts.size() - 2].c_str()), e = atoi(parts.back().c_str());
		if (s > 0 || e > 0) return "S" + std::to_string(s) + "E" + std::to_string(e);
	}
	return "";
}

UiCard App::make_card(const Item& it, bool show_progress) {
	UiCard c;
	c.title = it.name;
	c.initials = initials(it.name);
	c.description = it.description;
	c.year = it.release_info;
	c.runtime = it.runtime;
	c.rating = it.imdb_rating;
	c.in_library = in_library(it.id);
	c.logo = art(it.logo, ArtKind::LogoBox, false);
	c.background = art(it.background, ArtKind::Background, false);
	if (show_progress) {
		if (it.type == "series") c.badge = episode_badge(it.video_id);
		if (it.duration > 0 && it.offset > 0) {
			int pct = std::max(2, std::min(100, int(it.offset * 100 / it.duration)));
			c.progress = std::to_string(pct) + "%";
		}
	}
	return c;
}

void App::refresh_home_preview() {
	if (view != "home" && view != "search") return;
	const auto& rows = view == "home" ? board_ : search_;
	const auto& mapping = view == "home" ? home_map_ : search_map_;
	const int row = view == "home" ? home_row : search_row;
	const int col = view == "home" ? home_col : search_col;
	auto clear = [this] {
		home_preview_name.clear(); home_preview_background.clear(); home_preview_logo.clear();
		home_preview_description.clear(); home_preview_meta.clear();
	};
	if (row < 0 || row >= int(mapping.size()) || mapping[row] < 0 || mapping[row] >= int(rows.size())) { clear(); return; }
	const auto& items = rows[mapping[row]].items;
	if (col < 0 || col >= int(items.size())) { clear(); return; }
	const auto& item = items[col];
	home_preview_name = item.name;
	home_preview_description = item.description;
	home_preview_meta = item.release_info;
	if (!item.runtime.empty()) home_preview_meta += "  ·  " + item.runtime;
	if (!item.imdb_rating.empty()) home_preview_meta += "  ·  IMDb " + item.imdb_rating;
	home_preview_background = art(item.background, ArtKind::Background);
	home_preview_logo = art(item.logo, ArtKind::LogoBox);
	// Warm only neighbouring heroes at lower priority; this never blocks the
	// currently focused image or asks every catalog row for large backdrops.
	for (int neighbour : {col - 1, col + 1}) {
		if (neighbour < 0 || neighbour >= int(items.size())) continue;
		const auto& next = items[neighbour];
		g_art.get(next.background, ArtKind::Background, {}, ArtPriority::Prefetch);
		g_art.get(next.logo, ArtKind::LogoBox, {}, ArtPriority::Prefetch);
	}
}

static std::vector<Item> parse_catalog(const json& j, const std::string& type, const std::string& addon_name,
                                       const std::string& addon_url) {
	std::vector<Item> items;
	const json& metas = jobj(j, "metas").is_array() ? jobj(j, "metas") : jobj(j, "metasDetailed");
	if (!metas.is_array()) return items;
	for (auto& m : metas) {
		Item it = item_from_meta_json(m);
		if (it.type.empty()) it.type = type;
		if (jobj(j, "metasDetailed").is_array() || jobj(m, "videos").is_array()) it.inline_meta = m;
		it.metadata_addon = addon_name;
		it.metadata_addon_url = addon_url;
		if (!it.id.empty() && !it.name.empty()) items.push_back(it);
	}
	return items;
}

// ---------------------------------------------------------------------------
// Account library. Apply changes immediately to every visible cover; one writer
// serializes API requests so rapid add/remove presses cannot finish backwards.

bool App::in_library(const std::string& id) const {
	auto item = library_.find(id);
	return item != library_.end() && !jbool(item->second, "removed") && !jbool(item->second, "temp");
}

void App::refresh_library_membership() {
	refresh_home_cards();
	refresh_search_cards();
	for (size_t i = 0; i < disc_cards.size() && i < disc_items_.size(); ++i)
		disc_cards[i].in_library = in_library(disc_items_[i].id);
	library_refresh();
	dirty_all();
}

void App::queue_library_change(const json& item) {
	const std::string id = jstr(item, "_id");
	if (!signed_in() || id.empty()) return;
	if (!library_pending_changes_.count(id)) {
		auto old = library_.find(id);
		library_original_changes_[id] = old == library_.end() ? json() : old->second;
	}
	library_pending_changes_[id] = item;
	library_[id] = item;
	library_loaded_ = true;
	++library_revision_;
	flush_library_changes();
}

void App::flush_library_changes() {
	if (library_write_inflight_ || library_pending_changes_.empty() || !signed_in()) return;
	auto changed = std::move(library_pending_changes_);
	auto original = std::move(library_original_changes_);
	library_pending_changes_.clear();
	library_original_changes_.clear();
	json changes = json::array();
	for (const auto& entry : changed) changes.push_back(entry.second);
	const std::string key = settings_.auth_key;
	const int account_gen = account_generation_;
	library_write_inflight_ = true;
	bg<ApiResult>([key, changes] { return api_library_put(key, changes); },
	              [this, key, account_gen, changed, original](ApiResult& result) {
		              if (account_gen != account_generation_ || key != settings_.auth_key) return;
		              library_write_inflight_ = false;
		              bool restored = false;
		              for (const auto& entry : changed) {
			              const std::string& id = entry.first;
			              const json previous = original.count(id) ? original.at(id) : json();
			              if (library_pending_changes_.count(id)) {
				              // A newer local edit wins, but a failed future write must
				              // roll back to the last state actually accepted by Stremio.
				              library_original_changes_[id] = result.ok ? entry.second : previous;
			              } else if (!result.ok) {
				              if (previous.is_null()) library_.erase(id);
				              else library_[id] = previous;
				              restored = true;
			              }
		              }
		              if (!result.ok) {
			              dlog("library update failed: %s", result.error.c_str());
			              show_toast(settings_.ui_language == "it"
			                  ? "Impossibile aggiornare la libreria. Riprova."
			                  : "Could not update your library. Try again.", 5);
		              }
		              if (restored) {
			              ++library_revision_;
			              refresh_library_membership();
		              }
		              if (!library_pending_changes_.empty()) flush_library_changes();
		              else if (library_refresh_pending_) {
			              library_refresh_pending_ = false;
			              load_library();
		              }
	              });
}

void App::toggle_library(const Item& selected) {
	// selected can refer into a row rebuilt below; keep a value snapshot.
	const Item it = selected;
	if (it.id.empty()) return;
	if (!signed_in()) {
		show_toast(settings_.ui_language == "it" ? "Accedi per usare la tua libreria." : "Sign in to use your library.");
		return;
	}
	if (!library_loaded_) {
		// A catalog can finish before the first account snapshot. Membership is
		// unknown until that snapshot arrives, so do not guess add versus remove.
		show_toast(settings_.ui_language == "it" ? "Caricamento della libreria…" : "Loading your library…", 2.5);
		load_library();
		return;
	}
	const bool remove = in_library(it.id);
	auto previous = library_.find(it.id);
	const std::string now = iso8601_now();
	json entry = previous == library_.end() ? json::object() : previous->second;
	entry["_id"] = it.id;
	if (!it.name.empty()) entry["name"] = it.name;
	if (!it.type.empty()) entry["type"] = it.type;
	if (!it.poster.empty()) entry["poster"] = it.poster;
	if (!it.background.empty()) entry["background"] = it.background;
	if (!it.logo.empty()) entry["logo"] = it.logo;
	if (!entry.contains("posterShape")) entry["posterShape"] = "poster";
	if (!entry.contains("_ctime")) entry["_ctime"] = now;
	entry["_mtime"] = now;
	json& state = entry["state"];
	if (!state.is_object()) state = json::object();
	for (const char* field : {"timeWatched", "timeOffset", "overallTimeWatched", "timesWatched", "flaggedWatched", "duration"})
		if (!state.contains(field)) state[field] = 0;
	if (!state.contains("lastWatched")) state["lastWatched"] = nullptr;
	if (!state.contains("video_id")) state["video_id"] = nullptr;
	if (!state.contains("watched")) state["watched"] = nullptr;
	if (!state.contains("noNotif")) state["noNotif"] = false;
	if (!entry.contains("behaviorHints")) entry["behaviorHints"] = {
		{"defaultVideoId", nullptr}, {"featuredVideoId", nullptr}, {"hasScheduledVideos", false}};
	entry["removed"] = remove;
	// Removing a favourite leaves its ongoing episode in Continue Watching.
	// That row has its own Square action and never changes membership.
	entry["temp"] = remove && jnum(state, "timeOffset") > 0;
	queue_library_change(entry);
	refresh_library_membership();
	show_toast(settings_.ui_language == "it"
		? (remove ? "Rimosso dalla libreria" : "Aggiunto alla libreria")
		: (remove ? "Removed from library" : "Added to library"), 2.5);
}

// ---------------------------------------------------------------------------
// Continue watching

std::vector<Item> App::continue_watching() const {
	std::vector<Item> out;
	if (signed_in() && library_loaded_) {
		for (auto& kv : library_) {
			const json& li = kv.second;
			const json& st = jobj(li, "state");
			double offset = jnum(st, "timeOffset") / 1000.0;
			bool removed = jbool(li, "removed"), temp = jbool(li, "temp");
			if (offset <= 0 || (removed && !temp)) continue;
			Item it;
			it.id = kv.first;
			it.type = jstr(li, "type");
			it.name = jstr(li, "name");
			it.poster = jstr(li, "poster");
			it.background = jstr(li, "background");
			it.logo = jstr(li, "logo");
			it.video_id = jstr(st, "video_id");
			it.offset = offset;
			it.duration = jnum(st, "duration") / 1000.0;
			it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
			out.push_back(it);
		}
	} else {
		for (auto& kv : progress_) {
			if (kv.second.time <= 0) continue;
			Item it;
			it.id = kv.first;
			it.type = kv.second.type;
			it.name = kv.second.name;
			it.poster = kv.second.poster;
			it.video_id = kv.second.video_id;
			it.offset = kv.second.time;
			it.duration = kv.second.duration;
			it.last_watched = kv.second.updated;
			out.push_back(it);
		}
	}
	std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.last_watched > b.last_watched; });
	if (out.size() > kMaxRowCards) out.resize(kMaxRowCards);
	return out;
}

void App::remove_from_continue_watching(const Item& it) {
	const std::string id = it.id;
	auto p = progress_.find(id);
	if (p != progress_.end()) {
		p->second.time = 0;
		save_progress();
	}
	auto li = library_.find(id);
	if (signed_in() && li != library_.end()) {
		json item = li->second;
		item["state"]["timeOffset"] = 0;
		item["_mtime"] = iso8601_now();
		queue_library_change(item);
	}
	show_toast(settings_.ui_language == "it" ? "Rimosso da Continua a guardare" : "Removed from Continue Watching", 2.5);
	refresh_home_cards();
}

// ---------------------------------------------------------------------------
// Board

void App::build_home() {
	int gen = ++home_gen_;
	if (home_cancel_) home_cancel_->store(true);
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	home_cancel_ = cancel;
	board_.clear();
	BoardRow cw;
	cw.title = "Continue Watching";
	cw.continue_watching = true;
	cw.loaded = true;
	board_.push_back(cw);

	for (auto& a : addons_) {
		for (auto& c : a->catalogs) {
			const auto* search_extra = c.extra("search");
			if (search_extra && search_extra->required) continue;
			if (!c.supports_extras(c.default_extras())) continue;
			BoardRow row;
			row.title = row_title(c);
			row.addon_url = a->transport_url;
			row.catalog = c;
			board_.push_back(row);
		}
	}

	for (size_t i = 1; i < board_.size(); i++) {
		const Addon* a = find_addon(board_[i].addon_url);
		if (!a) continue;
		const Catalog& catalog = board_[i].catalog;
		std::string url = a->resource_url_with_extras("catalog", catalog.type, catalog.id, catalog.default_extras());
		std::string type = catalog.type, addon_name = a->name, addon_url = a->transport_url;
		struct Res {
			std::vector<Item> items;
			std::string error;
		};
		bg<Res>(
		    [url, cancel, type, addon_name, addon_url]() {
			    Res r;
			    json j;
			    if (fetch_json(url, j, r.error, 20, cancel.get())) r.items = parse_catalog(j, type, addon_name, addon_url);
			    return r;
		    },
		    [this, gen, i](Res& r) {
			    if (gen != home_gen_ || i >= board_.size()) return;
			    board_[i].loaded = true;
			    board_[i].error = r.error;
			    board_[i].items = std::move(r.items);
			    if (board_[i].items.size() > kMaxRowCards) board_[i].items.resize(kMaxRowCards);
			    if (!r.error.empty()) dlog("catalog %s: %s", board_[i].title.c_str(), r.error.c_str());
			    refresh_home_cards();
		    });
	}
	refresh_home_cards();
}

void App::refresh_home_cards() {
	// Keep the selection on the same catalog when rows appear above it.
	int selected_board = (home_row >= 0 && home_row < int(home_map_.size())) ? home_map_[home_row] : -1;
	std::string selected_id, selected_video, selected_type;
	if (selected_board >= 0 && selected_board < int(board_.size()) && home_col >= 0 &&
	    home_col < int(board_[selected_board].items.size())) {
		const auto& item = board_[selected_board].items[home_col];
		selected_id = item.id; selected_video = item.video_id; selected_type = item.type;
	}
	if (!board_.empty()) board_[0].items = continue_watching();

	home_map_.clear();
	std::vector<UiRow> rows;
	bool any_loading = false;
	for (size_t i = 0; i < board_.size(); i++) {
		BoardRow& br = board_[i];
		if (!br.loaded) {
			any_loading = true;
			continue;
		}
		if (br.items.empty() && br.error.empty()) continue;
		UiRow r;
		r.title = br.title;
		r.dom_id = "home-strip-" + std::to_string(home_gen_) + "-" + std::to_string(i);
		r.continue_watching = br.continue_watching;
		r.see_all = false;
		if (!br.error.empty()) r.message = "Could not load this catalog: " + br.error;
		home_map_.push_back(int(i));
		rows.push_back(std::move(r));
	}
	// Rows still loading show up later; say so when there's nothing yet.
	if (rows.empty() && !any_loading && !addons_loading_ && !addons_.empty()) {
		UiRow r;
		r.title = "Catalogs";
		r.dom_id = "home-strip-none";
		r.message = "No catalog entries to show. Browse other catalogs and filters in Discover.";
		home_map_.push_back(-1);
		rows.push_back(r);
	}

	if (selected_board >= 0) {
		for (size_t r = 0; r < home_map_.size(); r++)
			if (home_map_[r] == selected_board) home_row = int(r);
	}
	if (home_startup_.active()) {
		// Input is hidden during staging. Choose the first usable shelf only
		// once the configured-order model has settled, not the fastest reply.
		home_row = home_col = 0;
		for (size_t r = 0; r < home_map_.size(); ++r)
			if (home_map_[r] >= 0 && !board_[home_map_[r]].items.empty()) { home_row = int(r); break; }
	} else if (!selected_id.empty() && selected_board >= 0 && selected_board < int(board_.size())) {
		const auto& items = board_[selected_board].items;
		for (size_t c = 0; c < items.size(); ++c)
			if (items[c].id == selected_id && items[c].type == selected_type && items[c].video_id == selected_video) {
				home_col = int(c);
				break;
			}
	}
	if (home_row >= int(rows.size())) home_row = std::max(0, int(rows.size()) - 1);
	if (home_row >= 0 && home_row < int(home_map_.size()) && home_map_[home_row] >= 0)
		home_col = std::clamp(home_col, 0, std::max(0, int(board_[home_map_[home_row]].items.size()) - 1));
	// Queue the selected hero before workers can claim newly visible posters.
	if (view == "home") refresh_home_preview();

	for (size_t r = 0; r < rows.size(); r++) {
		int bi = home_map_[r];
		if (bi < 0) continue;
		auto& items = board_[bi].items;
		for (size_t c = 0; c < items.size(); c++) {
			UiCard card = make_card(items[c], board_[bi].continue_watching);
			bool near = view == "home" && std::abs(int(r) - home_row) <= 1 && int(c) >= home_col - 2 && int(c) <= home_col + 8;
			card.image = art(items[c].poster, ArtKind::PosterLarge, near);
			rows[r].cards.push_back(std::move(card));
		}
	}
	if (home_row < int(rows.size())) home_col = std::min(home_col, std::max(0, int(rows[home_row].cards.size()) - 1));
	home_rows = std::move(rows);
	refresh_home_preview();
	dirty_all();
}

std::vector<std::string> App::home_startup_artwork() const {
	std::vector<std::string> paths;
	const auto add = [&](const std::string& path) {
		if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
	};
	add("assets/icons/logo.png");
	add(home_preview_background);
	add(home_preview_logo);
	int visible = 0;
	for (const auto& row : home_rows) {
		if (row.cards.empty()) continue;
		for (size_t i = 0; i < std::min<size_t>(8, row.cards.size()); ++i) add(row.cards[i].image);
		if (++visible == 2) break;
	}
	return paths;
}

void App::update_home_startup(bool textures_ready) {
	update_home_startup_at(now_seconds(), textures_ready);
}

void App::update_home_startup_at(double now, bool textures_ready) {
	if (!home_startup_.active()) return;
	if (view != "home" || login_visible || watching_) { home_startup_.cancel(); return; }
	HomeStartupGate::State state;
	state.account_settled = !signed_in() || library_loaded_ || library_initial_settled_;
	state.leading_catalogs_settled = !addons_loading_;
	state.all_catalogs_settled = !addons_loading_;
	for (size_t i = 1; i < board_.size(); ++i) {
		if (!board_[i].loaded) {
			state.all_catalogs_settled = false;
			if (i <= 2) state.leading_catalogs_settled = false;
		}
	}
	state.textures_ready = textures_ready;
	state.artwork_ready = true;
	std::uint64_t signature = 1469598103934665603ull;
	const auto hash = [&](const std::string& value) {
		for (unsigned char ch : value) { signature ^= ch; signature *= 1099511628211ull; }
		signature ^= 0xff; signature *= 1099511628211ull;
	};
	int visible = 0;
	for (size_t r = 0; r < home_rows.size(); ++r) {
		const auto& row = home_rows[r];
		if (row.cards.empty()) continue;
		state.content = true;
		hash(row.dom_id);
		hash(std::to_string(row.cards.size()));
		const int bi = r < home_map_.size() ? home_map_[r] : -1;
		for (size_t c = 0; c < std::min<size_t>(8, row.cards.size()); ++c) {
			hash(row.cards[c].title); hash(row.cards[c].image);
			if (bi >= 0 && bi < int(board_.size()) && c < board_[bi].items.size() &&
			    !board_[bi].items[c].poster.empty() && row.cards[c].image.empty()) state.artwork_ready = false;
		}
		if (++visible == 2) break;
	}
	if (home_row >= 0 && home_row < int(home_map_.size())) {
		const int bi = home_map_[home_row];
		if (bi >= 0 && bi < int(board_.size()) && !board_[bi].items.empty()) {
			const auto& item = board_[bi].items[std::clamp(home_col, 0, int(board_[bi].items.size()) - 1)];
			if ((!item.background.empty() && home_preview_background.empty()) ||
			    (!item.logo.empty() && home_preview_logo.empty())) state.artwork_ready = false;
		}
	}
	hash(home_preview_background); hash(home_preview_logo);
	state.signature = signature;
	const auto result = home_startup_.update(now, state);
	if (result != HomeStartupGate::Result::Waiting) {
		const char* reason = result == HomeStartupGate::Result::Ready ? "ready" :
		    result == HomeStartupGate::Result::Partial ? "partial" :
		    result == HomeStartupGate::Result::Empty ? "empty" : "deadline";
		dlog("Home startup reveal: reason=%s elapsed=%.3fs rows=%zu leading_ready=%d account_ready=%d artwork_ready=%d textures_ready=%d",
		    reason, home_startup_.elapsed(now), home_rows.size(), int(state.leading_catalogs_settled),
		    int(state.account_settled), int(state.artwork_ready), int(state.textures_ready));
		dirty_all();
	}
}

void App::open_item(const Item& it) {
	return_view_ = view;
	return_zone_ = zone;
	detail_open(it);
}

static void move_in_rows(const std::vector<UiRow>& rows, int& row, int& col, Btn b, bool& moved) {
	moved = false;
	if (rows.empty()) {
		return;
	}
	int ncards = int(rows[row].cards.size());
	switch (b) {
	case Btn::Up:
		if (row > 0) {
			row--;
			col = std::min(col, std::max(0, int(rows[row].cards.size()) - 1));
			moved = true;
		}
		break;
	case Btn::Down:
		if (row + 1 < int(rows.size())) {
			row++;
			col = std::min(col, std::max(0, int(rows[row].cards.size()) - 1));
			moved = true;
		}
		break;
	case Btn::Left:
		if (col > 0) {
			col--;
			moved = true;
		}
		break;
	case Btn::Right:
		if (col + 1 < ncards) {
			col++;
			moved = true;
		}
		break;
	default: break;
	}
}

void App::board_button(Btn b) {
	bool moved = false;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		move_in_rows(home_rows, home_row, home_col, b, moved);
		if (moved) refresh_home_cards();
		break;
	case Btn::Cross:
		if (home_row < int(home_map_.size()) && home_map_[home_row] >= 0) {
			auto& items = board_[home_map_[home_row]].items;
			if (home_col < int(items.size())) open_item(items[home_col]);
		}
		break;
	case Btn::Square:
		if (home_row >= 0 && home_row < int(home_map_.size()) && home_map_[home_row] >= 0) {
			auto& row = board_[home_map_[home_row]];
			if (home_col >= 0 && home_col < int(row.items.size())) {
				if (row.continue_watching) remove_from_continue_watching(row.items[home_col]);
				else toggle_library(row.items[home_col]);
			}
		}
		break;
	case Btn::Circle: break;
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Search

void App::start_search(const std::string& q) {
	int gen = ++search_gen_;
	if (search_cancel_) search_cancel_->store(true);
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	search_cancel_ = cancel;
	search_query = q;
	view = "search";
	zone = "content";
	search_row = search_col = 0;
	search_.clear();
	for (auto& a : addons_) {
		for (auto& c : a->catalogs) {
			if (!c.has_extra("search")) continue;
			AddonExtras extra = c.default_extras();
			extra.erase(std::remove_if(extra.begin(), extra.end(), [](const auto& value) { return value.first == "search"; }), extra.end());
			extra.emplace_back("search", q);
			if (!c.supports_extras(extra)) continue;
			BoardRow row;
			row.title = row_title(c);
			row.addon_url = a->transport_url;
			row.catalog = c;
			search_.push_back(row);
		}
	}
	if (search_.empty()) {
		search_status = addons_loading_ ? "Addons are still loading..." : "None of your addons can search";
		refresh_search_cards();
		return;
	}
	search_status = "Searching...";
	search_pending_ = int(search_.size());
	for (size_t i = 0; i < search_.size(); i++) {
		const Addon* a = find_addon(search_[i].addon_url);
		const Catalog& catalog = search_[i].catalog;
		AddonExtras extra = catalog.default_extras();
		extra.erase(std::remove_if(extra.begin(), extra.end(), [](const auto& value) { return value.first == "search"; }), extra.end());
		extra.emplace_back("search", q);
		std::string url = a ? a->resource_url_with_extras("catalog", catalog.type, catalog.id, extra) : "";
		std::string type = catalog.type, addon_name = a ? a->name : "", addon_url = a ? a->transport_url : "";
		struct Res {
			std::vector<Item> items;
			std::string error;
		};
		bg<Res>(
		    [url, cancel, type, addon_name, addon_url]() {
			    Res r;
			    json j;
			    if (!url.empty() && fetch_json(url, j, r.error, 20, cancel.get())) r.items = parse_catalog(j, type, addon_name, addon_url);
			    if (url.empty()) r.error = "The catalog addon is no longer installed";
			    return r;
		    },
		    [this, gen, i, q](Res& r) {
			    if (gen != search_gen_ || i >= search_.size()) return;
			    search_[i].loaded = true;
			    search_[i].error = r.error;
			    search_[i].items = std::move(r.items);
			    if (search_[i].items.size() > kMaxRowCards) search_[i].items.resize(kMaxRowCards);
			    search_pending_--;
			    size_t total = 0, failed = 0;
			    for (auto& s : search_) { total += s.items.size(); failed += !s.error.empty(); }
			    if (search_pending_ > 0) search_status = "Searching...";
			    else if (failed > 0) search_status = std::to_string(failed) + " search catalog(s) could not answer";
			    else if (total == 0) search_status = "No results for \"" + q + "\"";
			    else search_status = "";
			    if (!r.error.empty()) dlog("search catalog %s: %s", search_[i].title.c_str(), r.error.c_str());
			    refresh_search_cards();
		    });
	}
	refresh_search_cards();
}

void App::refresh_search_cards() {
	int selected = (search_row < int(search_map_.size())) ? search_map_[search_row] : -1;
	search_map_.clear();
	std::vector<UiRow> rows;
	for (size_t i = 0; i < search_.size(); i++) {
		if (!search_[i].loaded || (search_[i].items.empty() && search_[i].error.empty())) continue;
		UiRow r;
		r.title = search_[i].title;
		r.dom_id = "search-strip-" + std::to_string(i);
		if (!search_[i].error.empty()) r.message = "Could not search this catalog: " + search_[i].error;
		search_map_.push_back(int(i));
		rows.push_back(std::move(r));
	}
	if (selected >= 0)
		for (size_t r = 0; r < search_map_.size(); r++)
			if (search_map_[r] == selected) search_row = int(r);
	if (search_row >= int(rows.size())) search_row = std::max(0, int(rows.size()) - 1);
	if (search_row >= 0 && search_row < int(search_map_.size()) && search_map_[search_row] >= 0)
		search_col = std::clamp(search_col, 0, std::max(0, int(search_[search_map_[search_row]].items.size()) - 1));
	if (view == "search") refresh_home_preview();
	for (size_t r = 0; r < rows.size(); r++) {
		auto& items = search_[search_map_[r]].items;
		for (size_t c = 0; c < items.size(); c++) {
			UiCard card = make_card(items[c], false);
			bool near = view == "search" && std::abs(int(r) - search_row) <= 1 && int(c) >= search_col - 2 && int(c) <= search_col + 8;
			card.image = art(items[c].poster, ArtKind::PosterLarge, near);
			rows[r].cards.push_back(std::move(card));
		}
	}
	if (search_row < int(rows.size()))
		search_col = std::min(search_col, std::max(0, int(rows[search_row].cards.size()) - 1));
	search_rows = std::move(rows);
	refresh_home_preview();
	dirty_all();
}

void App::search_button(Btn b) {
	bool moved = false;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		move_in_rows(search_rows, search_row, search_col, b, moved);
		if (moved) refresh_search_cards();
		break;
	case Btn::Cross:
		if (search_row < int(search_map_.size())) {
			auto& items = search_[search_map_[search_row]].items;
			if (search_col < int(items.size())) open_item(items[search_col]);
		}
		break;
	case Btn::Square:
		if (search_row >= 0 && search_row < int(search_map_.size()) && search_map_[search_row] >= 0) {
			const auto& items = search_[search_map_[search_row]].items;
			if (search_col >= 0 && search_col < int(items.size())) toggle_library(items[search_col]);
		}
		break;
	case Btn::Circle:
		search_gen_++;
		if (search_cancel_) search_cancel_->store(true);
		set_view("home");
		zone = "content";
		break;
	default: break;
	}
	dirty_all();
}
