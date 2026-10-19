// Discover (one catalog as a grid, with a preview), Library, Addons, and
// the dropdown used by their filters.

#include <algorithm>

#include "app.h"

static const int kLibCols = 7;

static std::string catalog_label(const Addon* a, const Catalog* c) {
	if (!c->name.empty()) return c->name;
	return a->name;
}

static bool browsable_catalog(const Catalog& catalog) {
	const auto* search = catalog.extra("search");
	return !search || !search->required;
}

// Moves a selection in a grid of `cols` columns; returns false when the move
// leaves the grid (up from the first row / left from the first column).
static bool grid_move(int& sel, int n, int cols, Btn b, bool& left_edge, bool& top_edge) {
	left_edge = top_edge = false;
	if (n == 0) {
		left_edge = b == Btn::Left;
		top_edge = b == Btn::Up;
		return false;
	}
	switch (b) {
	case Btn::Up:
		if (sel >= cols) sel -= cols;
		else top_edge = true;
		return !top_edge;
	case Btn::Down:
		if (sel + cols < n) sel += cols;
		else if (sel / cols < (n - 1) / cols) sel = n - 1;
		else return false;
		return true;
	case Btn::Left:
		if (sel % cols == 0) left_edge = true;
		else sel--;
		return !left_edge;
	case Btn::Right:
		if (sel % cols < cols - 1 && sel + 1 < n) {
			sel++;
			return true;
		}
		return false;
	default: return false;
	}
}

// ---------------------------------------------------------------------------
// Discover

void App::enter_discover() {
	// Types offered by catalogs that can be browsed without a search.
	disc_types_.clear();
	for (auto& a : addons_)
		for (auto& c : a->catalogs) {
			if (!browsable_catalog(c)) continue;
			if (std::find(disc_types_.begin(), disc_types_.end(), c.type) == disc_types_.end())
				disc_types_.push_back(c.type);
		}
	auto rank = [](const std::string& t) { return t == "movie" ? 0 : t == "series" ? 1 : 2; };
	std::stable_sort(disc_types_.begin(), disc_types_.end(),
	                 [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
		if (disc_types_.empty()) {
		disc_chips.clear();
		disc_cards.clear();
		disc_items_.clear();
		dp_name.clear();
		disc_status = addons_loading_ ? "Addons are still loading..." : "None of your addons have catalogs to browse.";
		dirty_all();
		return;
	}
	if (std::find(disc_types_.begin(), disc_types_.end(), disc_type_) == disc_types_.end()) {
		disc_type_ = disc_types_[0];
		disc_catalog_ = 0;
		disc_genre_.clear();
	}
	discover_build_chips();
	if (disc_items_.empty()) discover_load(false);
	dirty_all();
}

void App::discover_build_chips() {
	disc_catalogs_.clear();
	for (auto& a : addons_)
		for (auto& c : a->catalogs)
			if (c.type == disc_type_ && browsable_catalog(c)) disc_catalogs_.push_back({a.get(), &c});
	if (disc_catalog_ < 0 || disc_catalog_ >= int(disc_catalogs_.size())) disc_catalog_ = 0;

	disc_chips.clear();
	disc_extra_keys_.clear();
	disc_chips.push_back({capitalize(disc_type_)});
	if (!disc_catalogs_.empty()) {
		auto& dc = disc_catalogs_[disc_catalog_];
		std::string key = dc.addon->transport_url + "\n" + dc.catalog->type + "\n" + dc.catalog->id;
		if (key != disc_filter_catalog_key_) {
			disc_filter_catalog_key_ = key;
			disc_extra_values_.clear();
			for (const auto& value : dc.catalog->default_extras()) disc_extra_values_[value.first] = value.second;
			disc_items_.clear();
			disc_cards.clear();
			disc_sel = 0;
			disc_skip_ = 0;
		}
		disc_chips.push_back({catalog_label(dc.addon, dc.catalog)});
		for (const auto& extra : dc.catalog->extras) {
			if (extra.name == "skip" || extra.name == "search" || extra.options_limit == 0) continue;
			auto& value = disc_extra_values_[extra.name];
			if (value.empty() && extra.required && !extra.options.empty()) value = extra.options.front();
			if (value.empty() && extra.required && extra.name == "date") value = iso8601_now().substr(0, 10);
			disc_extra_keys_.push_back(extra.name);
			disc_chips.push_back({capitalize(extra.name) + ": " +
			    (value.empty() ? (extra.required ? "Set..." : "All") : value)});
		}
		disc_genre_ = disc_extra_values_["genre"];
	}
	if (disc_chip >= int(disc_chips.size())) disc_chip = std::max(0, int(disc_chips.size()) - 1);
}

void App::discover_load(bool more) {
	if (disc_catalogs_.empty() || disc_catalog_ < 0 || disc_catalog_ >= int(disc_catalogs_.size())) return;
	if (more && (disc_loading_ || disc_end_)) return;
	if (!more) {
		disc_gen_++;
		if (disc_cancel_) disc_cancel_->store(true);
		disc_cancel_ = std::make_shared<std::atomic<bool>>(false);
		disc_items_.clear();
		disc_cards.clear();
		disc_sel = 0;
		disc_skip_ = 0;
		disc_loading_ = false;
		disc_end_ = false;
		dp_name.clear();
	}
	auto& dc = disc_catalogs_[disc_catalog_];
	const bool has_skip = dc.catalog->has_extra("skip");
	if (more && !has_skip) { disc_end_ = true; return; }
	AddonExtras extra;
	for (const auto& declaration : dc.catalog->extras) {
		if (declaration.name == "skip") {
			if (more || declaration.required) extra.emplace_back("skip", std::to_string(disc_skip_));
			continue;
		}
		auto value = disc_extra_values_.find(declaration.name);
		if (value != disc_extra_values_.end() && !value->second.empty()) extra.emplace_back(value->first, value->second);
		else if (declaration.required) {
			disc_status = "Set the required " + declaration.name + " filter to load this catalog.";
			zone = "filters";
			dirty_all();
			return;
		}
	}
	if (!dc.catalog->supports_extras(extra)) {
		disc_status = "This catalog does not accept the selected filters.";
		dirty_all();
		return;
	}
	std::string url = dc.addon->resource_url_with_extras("catalog", dc.catalog->type, dc.catalog->id, extra);
	std::string type = dc.catalog->type, addon_name = dc.addon->name, addon_url = dc.addon->transport_url;
	int gen = disc_gen_;
	auto cancel = disc_cancel_;
	disc_loading_ = true;
	if (disc_items_.empty()) disc_status = "Loading...";
	struct Res {
		std::vector<Item> items;
		std::string error;
		size_t received = 0;
	};
	bg<Res>(
	    [url, cancel, type, addon_name, addon_url]() {
		    Res r;
		    json j;
		    if (fetch_json(url, j, r.error, 20, cancel.get())) {
			    const json& metas = jobj(j, "metas").is_array() ? jobj(j, "metas") : jobj(j, "metasDetailed");
			    if (metas.is_array()) {
				    r.received = metas.size();
				    for (auto& m : metas) {
					    Item it = item_from_meta_json(m);
					    if (it.type.empty()) it.type = type;
					    if (jobj(j, "metasDetailed").is_array() || jobj(m, "videos").is_array()) it.inline_meta = m;
					    it.metadata_addon = addon_name;
					    it.metadata_addon_url = addon_url;
					    if (!it.id.empty() && !it.name.empty()) r.items.push_back(it);
				    }
			    }
		    }
		    return r;
	    },
	    [this, gen, has_skip](Res& r) {
		    if (gen != disc_gen_) return;
		    disc_loading_ = false;
		    if (!r.error.empty()) {
			    disc_status = "Could not load this catalog (" + r.error + ").";
			    dlog("discover catalog: %s", r.error.c_str());
			    dirty_all();
			    return;
		    }
		    disc_skip_ += r.received;
		    // Skip duplicates (some addons ignore skip and send page one again).
		    size_t added = 0;
		    std::set<std::pair<std::string, std::string>> seen;
		    for (const auto& existing : disc_items_) seen.insert({existing.type, existing.id});
		    for (auto& it : r.items) {
			    if (!seen.insert({it.type, it.id}).second) continue;
			    disc_items_.push_back(it);
			    disc_cards.push_back(make_card(it, false));
			    added++;
		    }
		    if (!has_skip || added == 0 || r.received == 0) disc_end_ = true;
		    if (disc_items_.empty()) disc_status = "Nothing here.";
		    else disc_status = "";
		    refresh_images();
		    discover_preview();
		    dirty_all();
	    });
	dirty_all();
}

void App::discover_preview() {
	if (disc_sel >= int(disc_items_.size())) {
		dp_name.clear();
		return;
	}
	const Item& it = disc_items_[disc_sel];
	dp_name = it.name;
	dp_logo = art(it.logo, ArtKind::LogoBox);
	dp_still.clear();
	dp_runtime = it.runtime;
	dp_year = it.release_info;
	dp_imdb = it.imdb_rating;
	dp_genres = join(it.genres, " · ");
	dp_desc = it.description;
	dp_directors = join(it.directors, ", ");
	std::vector<std::string> main_cast(it.cast.begin(), it.cast.begin() + std::min<size_t>(it.cast.size(), 5));
	dp_cast = join(main_cast, ", ");
}

void App::discover_button(Btn b) {
	if (zone == "filters") {
		switch (b) {
		case Btn::Left:
			if (disc_chip > 0) disc_chip--;
			break;
		case Btn::Right:
			if (disc_chip + 1 < int(disc_chips.size())) disc_chip++;
			break;
		case Btn::Up: break;
		case Btn::Down:
			if (!disc_cards.empty()) zone = "content";
			break;
		case Btn::Circle: zone = "content"; break;
		case Btn::Cross:
			if (disc_chip == 0) {
				std::vector<std::string> opts;
				int active = 0;
				for (size_t i = 0; i < disc_types_.size(); i++) {
					opts.push_back(capitalize(disc_types_[i]));
					if (disc_types_[i] == disc_type_) active = int(i);
				}
				open_dropdown("Type", opts, active, [this](int i) {
					if (disc_types_[i] == disc_type_) return;
					disc_type_ = disc_types_[i];
					disc_catalog_ = 0;
					disc_genre_.clear();
					discover_build_chips();
					discover_load(false);
				});
			} else if (disc_chip == 1) {
				std::vector<std::string> opts;
				for (auto& dc : disc_catalogs_) {
					std::string label = catalog_label(dc.addon, dc.catalog);
					if (label != dc.addon->name) label += " (" + dc.addon->name + ")";
					opts.push_back(label);
				}
				open_dropdown("Catalog", opts, disc_catalog_, [this](int i) {
					if (i == disc_catalog_) return;
					disc_catalog_ = i;
					disc_genre_.clear();
					discover_build_chips();
					discover_load(false);
				});
			} else if (disc_chip >= 2 && !disc_catalogs_.empty() && disc_chip - 2 < int(disc_extra_keys_.size())) {
				const std::string name = disc_extra_keys_[disc_chip - 2];
				const CatalogExtra* g = disc_catalogs_[disc_catalog_].catalog->extra(name);
				if (!g) break;
				const std::string catalog_key = disc_filter_catalog_key_;
				if (g->options.empty()) {
					open_input(capitalize(name), disc_extra_values_[name],
					           name == "date" ? "YYYY-MM-DD" : (g->required ? "Required by this catalog" : "Leave empty for all"),
					           [this, name, catalog_key](const std::string& value) {
						           if (catalog_key != disc_filter_catalog_key_) return;
						           disc_extra_values_[name] = trim(value);
						           discover_build_chips();
						           discover_load(false);
					           });
					break;
				}
				std::vector<std::string> opts;
				int active = 0;
				if (!g->required) opts.push_back("All");
				for (auto& o : g->options) {
					if (o == disc_extra_values_[name]) active = int(opts.size());
					opts.push_back(o);
				}
				bool required = g->required;
				open_dropdown(capitalize(name), opts, active, [this, name, opts, required, catalog_key](int i) {
					if (catalog_key != disc_filter_catalog_key_) return;
					std::string value = (!required && i == 0) ? "" : opts[i];
					if (value == disc_extra_values_[name]) return;
					disc_extra_values_[name] = value;
					discover_build_chips();
					discover_load(false);
				});
			}
			break;
		default: break;
		}
		dirty_all();
		return;
	}

	bool left_edge, top_edge;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		if (grid_move(disc_sel, int(disc_cards.size()), kDiscCols, b, left_edge, top_edge)) {
			discover_preview();
			refresh_images();
		}
		if ((b == Btn::Down || b == Btn::Right) && !disc_end_ && !disc_loading_ &&
		    disc_sel + 2 * kDiscCols >= int(disc_items_.size())) discover_load(true);
		if (top_edge) zone = "filters";
		break;
	case Btn::Cross:
		if (disc_sel < int(disc_items_.size())) open_item(disc_items_[disc_sel]);
		break;
	case Btn::Square:
		if (disc_sel >= 0 && disc_sel < int(disc_items_.size())) toggle_library(disc_items_[disc_sel]);
		break;
	case Btn::Options:
		if (!disc_loading_) discover_load(false);
		break;
	case Btn::Circle: zone = "filters"; break;
	default: break;
	}
	dirty_all();
}

void App::discover_see_all(const BoardRow& row) {
	// set_view may rebuild the board/addon collections; capture identity first.
	const std::string addon_url = row.addon_url, catalog_id = row.catalog.id;
	disc_type_ = row.catalog.type;
	disc_genre_.clear();
	disc_filter_catalog_key_.clear();
	disc_catalog_ = 0;
	disc_items_.clear();
	set_view("discover");  // builds types/catalogs for disc_type_
	for (size_t i = 0; i < disc_catalogs_.size(); i++) {
		if (disc_catalogs_[i].addon->transport_url == addon_url && disc_catalogs_[i].catalog->id == catalog_id) {
			disc_catalog_ = int(i);
			break;
		}
	}
	discover_build_chips();
	discover_load(false);
	zone = "content";
	dirty_all();
}

// ---------------------------------------------------------------------------
// Library

void App::enter_library() {
	if (lib_chips.empty()) lib_chip = 0;
	if (signed_in() && !library_loaded_) load_library();
	library_refresh();
}

void App::library_refresh() {
	std::vector<Item> all;
	if (signed_in()) {
		for (auto& kv : library_) {
			const json& li = kv.second;
			if (jbool(li, "removed") || jbool(li, "temp")) continue;
			const json& st = jobj(li, "state");
			Item it;
			it.id = kv.first;
			it.type = jstr(li, "type");
			it.name = jstr(li, "name");
			it.poster = jstr(li, "poster");
			it.background = jstr(li, "background");
			it.logo = jstr(li, "logo");
			it.video_id = jstr(st, "video_id");
			it.offset = jnum(st, "timeOffset") / 1000.0;
			it.duration = jnum(st, "duration") / 1000.0;
			it.times_watched = int(jnum(st, "timesWatched"));
			it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
			if (!it.last_watched) it.last_watched = iso8601_to_ms(jstr(li, "_mtime"));
			all.push_back(it);
		}
	} else {
		for (auto& kv : progress_) {
			Item it;
			it.id = kv.first;
			it.type = kv.second.type;
			it.name = kv.second.name;
			it.poster = kv.second.poster;
			it.video_id = kv.second.video_id;
			it.offset = kv.second.time;
			it.duration = kv.second.duration;
			it.last_watched = kv.second.updated;
			all.push_back(it);
		}
	}

	std::vector<std::string> types;
	for (auto& it : all)
		if (!it.type.empty() && std::find(types.begin(), types.end(), it.type) == types.end()) types.push_back(it.type);
	std::sort(types.begin(), types.end());
	if (!lib_type_.empty() && std::find(types.begin(), types.end(), lib_type_) == types.end()) lib_type_.clear();

	static const char* sorts[] = {"Last watched", "A-Z", "Most watched"};
	lib_chips.clear();
	lib_chips.push_back({lib_type_.empty() ? "All types" : capitalize(lib_type_)});
	lib_chips.push_back({sorts[lib_sort_]});

	lib_items_.clear();
	for (auto& it : all)
		if (lib_type_.empty() || it.type == lib_type_) lib_items_.push_back(it);
	if (lib_sort_ == 0)
		std::sort(lib_items_.begin(), lib_items_.end(),
		          [](const Item& a, const Item& b) { return a.last_watched > b.last_watched; });
	else if (lib_sort_ == 1)
		std::sort(lib_items_.begin(), lib_items_.end(),
		          [](const Item& a, const Item& b) { return lower(a.name) < lower(b.name); });
	else
		std::sort(lib_items_.begin(), lib_items_.end(),
		          [](const Item& a, const Item& b) { return a.times_watched > b.times_watched; });

	lib_cards.clear();
	for (auto& it : lib_items_) lib_cards.push_back(make_card(it, true));
	if (lib_sel >= int(lib_cards.size())) lib_sel = std::max(0, int(lib_cards.size()) - 1);

	if (!lib_items_.empty()) lib_status = "";
	else if (signed_in() && !library_loaded_) lib_status = "Loading your library...";
	else if (!signed_in() && all.empty()) lib_status = "Sign in to your Stremio account (Settings) to see your library here.";
	else lib_status = "Nothing in your library matches this filter.";

	if (view == "library") refresh_images();
	dirty_all();
}

void App::library_button(Btn b) {
	if (zone == "filters") {
		switch (b) {
		case Btn::Left:
			if (lib_chip > 0) lib_chip--;
			break;
		case Btn::Right:
			if (lib_chip + 1 < int(lib_chips.size())) lib_chip++;
			break;
		case Btn::Up: break;
		case Btn::Down:
			if (!lib_cards.empty()) zone = "content";
			break;
		case Btn::Circle: zone = "content"; break;
		case Btn::Cross:
			if (lib_chip == 0) {
				std::vector<std::string> types;
				for (auto& kv : library_) {
					std::string t = jstr(kv.second, "type");
					if (!t.empty() && std::find(types.begin(), types.end(), t) == types.end()) types.push_back(t);
				}
				for (auto& kv : progress_)
					if (!kv.second.type.empty() && std::find(types.begin(), types.end(), kv.second.type) == types.end())
						types.push_back(kv.second.type);
				std::sort(types.begin(), types.end());
				std::vector<std::string> opts = {"All types"};
				int active = 0;
				for (auto& t : types) {
					if (t == lib_type_) active = int(opts.size());
					opts.push_back(capitalize(t));
				}
				open_dropdown("Type", opts, active, [this, types](int i) {
					lib_type_ = i == 0 ? "" : types[i - 1];
					lib_sel = 0;
					library_refresh();
				});
			} else {
				open_dropdown("Sort by", {"Last watched", "A-Z", "Most watched"}, lib_sort_, [this](int i) {
					lib_sort_ = i;
					lib_sel = 0;
					library_refresh();
				});
			}
			break;
		default: break;
		}
		dirty_all();
		return;
	}
	bool left_edge, top_edge;
	switch (b) {
	case Btn::Up:
	case Btn::Down:
	case Btn::Left:
	case Btn::Right:
		if (grid_move(lib_sel, int(lib_cards.size()), kLibCols, b, left_edge, top_edge)) refresh_images();
		if (top_edge) zone = "filters";
		break;
	case Btn::Cross:
		if (lib_sel < int(lib_items_.size())) open_item(lib_items_[lib_sel]);
		break;
	case Btn::Square:
		if (lib_sel >= 0 && lib_sel < int(lib_items_.size()) && in_library(lib_items_[lib_sel].id))
			toggle_library(lib_items_[lib_sel]);
		break;
	case Btn::Circle: zone = "filters"; break;
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Addons

void App::enter_addons() {
	addon_rows.clear();
	for (auto& a : addons_) {
		UiAddon r;
		r.name = a->name;
		r.version = a->version.empty() ? "" : "v" + a->version;
		r.desc = a->description;
		std::vector<std::string> types;
		for (auto& t : a->types) types.push_back(capitalize(t));
		r.types = join(types, " · ");
		r.local = !a->from_account;
		// No logo: the first letter of the name (one UTF-8 character).
		size_t len = a->name.empty() ? 0 : 1;
		while (len < a->name.size() && (a->name[len] & 0xC0) == 0x80) len++;
		r.initials = a->name.substr(0, len);
		r.logo = art(jstr(a->manifest, "logo"), ArtKind::Icon);
		addon_rows.push_back(r);
	}
	if (addon_sel >= int(addon_rows.size())) addon_sel = std::max(0, int(addon_rows.size()) - 1);
	dirty_all();
}

void App::addons_button(Btn b) {
	switch (b) {
	// Tiles, kAddonCols to a row.
	case Btn::Up:
		if (addon_sel >= kAddonCols) addon_sel -= kAddonCols;
		break;
	case Btn::Down: {
		int n = int(addon_rows.size());
		if (addon_sel / kAddonCols < (n - 1) / kAddonCols) addon_sel = std::min(addon_sel + kAddonCols, n - 1);
		break;
	}
	case Btn::Left:
		if (addon_sel % kAddonCols > 0) addon_sel--;
		break;
	case Btn::Right:
		if (addon_sel % kAddonCols < kAddonCols - 1 && addon_sel + 1 < int(addon_rows.size())) addon_sel++;
		break;
	case Btn::Circle: set_view("settings"); zone = "content"; break;
	case Btn::Square:
		show_toast("Reloading addons...");
		load_addons();
		if (signed_in()) load_library();
		break;
	case Btn::Cross: {
		if (addon_sel >= int(addons_.size())) break;
		std::string url = addons_[addon_sel]->transport_url;
		auto it = std::find(settings_.extra_addons.begin(), settings_.extra_addons.end(), url);
		if (it == settings_.extra_addons.end()) {
			show_toast("Install and remove addons in Stremio on your phone or computer; they sync here.",
			           7);
			break;
		}
		open_dropdown(addons_[addon_sel]->name, {"Remove addon", "Keep"}, 1, [this, url](int i) {
			if (i != 0) return;
			auto& ex = settings_.extra_addons;
			ex.erase(std::remove(ex.begin(), ex.end(), url), ex.end());
			save_settings();
			load_addons();
		});
		break;
	}
	default: break;
	}
	dirty_all();
}

// ---------------------------------------------------------------------------
// Dropdown

void App::open_dropdown(const std::string& title, const std::vector<std::string>& options, int active,
                        std::function<void(int)> chosen) {
	dd_multiselect = false;
	dd_title = title;
	dd_options.clear();
	for (size_t i = 0; i < options.size(); i++) dd_options.push_back({options[i], int(i) == active});
	dd_sel = std::max(0, std::min(active, int(options.size()) - 1));
	dd_chosen_ = chosen;
	dd_visible = true;
	dirty_all();
}

void App::dropdown_button(Btn b) {
	switch (b) {
	case Btn::Up:
		if (dd_sel > 0) dd_sel--;
		break;
	case Btn::Down:
		if (dd_sel + 1 < int(dd_options.size())) dd_sel++;
		break;
	case Btn::Cross: {
		auto fn = dd_chosen_;
		int sel = dd_sel;
		if (!dd_multiselect) {
			dd_visible = false;
			dd_chosen_ = nullptr;
		}
		if (fn && sel >= 0 && sel < int(dd_options.size())) fn(sel);
		break;
	}
	case Btn::Circle:
		dd_visible = false;
		dd_multiselect = false;
		dd_chosen_ = nullptr;
		break;
	default: break;
	}
	dirty_all();
}
