// Host-only presentation fixtures. Never linked into the native PS5 title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "preview_fixture.h"
#include "app.h"
#include "bidi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>

namespace {
namespace fs = std::filesystem;

std::string path_string(const fs::path& base, const std::string& value) {
    if (value.empty()) return {};
    fs::path path(value);
    if (path.is_relative()) path = base / path;
    std::error_code error;
    return fs::is_regular_file(path, error) ? path.lexically_normal().string() : std::string();
}

struct Fixture {
    json data;
    fs::path base, art;
    std::string poster_fallback, background_fallback;
    std::map<std::string, UiCard> cards;

    std::string image(const std::string& name, bool backdrop = false) const {
        std::string path = path_string(art, name);
        if (path.empty()) path = path_string(base, name);
        return path.empty() ? (backdrop ? background_fallback : poster_fallback) : path;
    }
    const UiCard& card(const std::string& id) const {
        const auto it = cards.find(id);
        static const UiCard missing;
        return it != cards.end() ? it->second : missing;
    }
    std::vector<UiCard> collection(const json& ids) const {
        std::vector<UiCard> result;
        if (ids.is_array()) for (const auto& id : ids)
            if (id.is_string()) {
                const auto it = cards.find(id.get<std::string>());
                if (it != cards.end()) result.push_back(it->second);
            }
        return result;
    }
};

void set_home_preview(App& app, const UiCard& card) {
    app.home_preview_name = card.title;
    app.home_preview_logo = card.logo;
    app.home_preview_background = card.background;
    app.home_preview_description = card.description;
    std::vector<std::string> metadata;
    if (!card.year.empty()) metadata.push_back(card.year);
    if (!card.runtime.empty()) metadata.push_back(card.runtime);
    if (!card.rating.empty()) metadata.push_back("IMDb " + card.rating);
    app.home_preview_meta = join(metadata, "  ·  ");
}

void set_detail(App& app, const Fixture& fixture, bool sources) {
    const auto& detail = jobj(fixture.data, "detail");
    const auto& card = fixture.card(jstr(detail, "id"));
    app.view = "detail";
    app.d_name = card.title;
    app.d_background = card.background;
    app.d_logo = card.logo;
    app.d_description = card.description;
    app.d_year = card.year;
    app.d_runtime = card.runtime;
    app.d_imdb = card.rating;
    app.d_genres = jstr(detail, "genres");
    app.d_cast = jstr(detail, "cast");
    app.d_directors = jstr(detail, "directors");
    app.d_resume = jstr(detail, "resume");
    app.d_series = jbool(detail, "series");
    app.d_season_label = jstr(detail, "season", "Stagione 1");
    app.d_seasons = {{"Speciali"}, {"Stagione 1"}, {"Stagione 2"}, {"Stagione 3"}, {"Stagione 4"}};
    app.d_season_sel = 1;
    app.d_episodes.clear();
    const auto& episodes = jobj(detail, "episodes");
    if (episodes.is_array()) for (const auto& entry : episodes) {
        UiEpisode episode;
        episode.title = jstr(entry, "title");
        episode.number = jstr(entry, "number", std::to_string(app.d_episodes.size() + 1));
        // Older checked-in fixtures put the number in the title. Production
        // metadata now keeps it separately so the carousel can lay it out.
        const auto leading = episode.title.find_first_not_of("0123456789");
        if (leading > 0 && leading != std::string::npos && std::isspace(static_cast<unsigned char>(episode.title[leading]))) {
            const auto title_start = episode.title.find_first_not_of(" \t", leading);
            if (title_start != std::string::npos) episode.title.erase(0, title_start);
        }
        episode.description = jstr(entry, "description", card.description);
        episode.sub = jstr(entry, "subtitle");
        episode.thumb = fixture.image(jstr(entry, "image"), true);
        episode.watched = jbool(entry, "watched");
        app.d_episodes.push_back(std::move(episode));
    }
    app.d_episode_sel = std::clamp(int(jnum(detail, "episode", 0)), 0,
                                  std::max(0, int(app.d_episodes.size()) - 1));
    app.d_zone = sources || !app.d_series ? "streams" : "episodes";
    app.d_pick_res = false;
    app.d_streams_title.clear();
    app.d_source_sel = 0;
    app.d_sources = {{"Tutte"}, {"Torrentio"}, {"Comet"}, {"AIOStreams"}};
    app.d_streams_status.clear();
    app.d_streams.clear();
    const auto& items = jobj(detail, "streams");
    if (items.is_array()) for (const auto& item : items) {
        UiStream stream;
        stream.name = jstr(item, "name"); stream.addon = jstr(item, "addon"); stream.desc = jstr(item, "description");
        stream.seeders = static_cast<int64_t>(jnum(item, "seeders", -1));
        stream.type = jstr(item, "type"); stream.quality = jstr(item, "quality"); stream.size = jstr(item, "size");
        stream.cached = jbool(item, "cached");
        const auto& languages = jobj(item, "languages");
        if (languages.is_array()) for (const auto& lang : languages)
            if (lang.is_string()) stream.languages.push_back(lang.get<std::string>());
        app.d_streams.push_back(std::move(stream));
    }
    app.d_stream_sel = 0;
}

void set_addons(App& app, const Fixture& fixture) {
    app.addon_rows.clear();
    const auto& addons = jobj(fixture.data, "addons");
    if (addons.is_array()) for (const auto& entry : addons) {
        UiAddon addon;
        addon.name = jstr(entry, "name");
        addon.version = jstr(entry, "version");
        addon.desc = jstr(entry, "description");
        addon.types = jstr(entry, "types");
        addon.initials = initials(addon.name);
        const auto logo = jstr(entry, "logo");
        if (!logo.empty()) addon.logo = fixture.image(logo);
        addon.local = jbool(entry, "local");
        app.addon_rows.push_back(std::move(addon));
    }
    app.addon_sel = 0;
}
}

bool load_preview_fixture(App& app, const std::string& scenario, const std::string& fixture_path) {
    static const std::array<const char*, 44> names{{"home", "detail", "streams", "discover", "library", "addons",
                                                 "settings", "dropdown", "login", "keyboard", "player", "search",
                                                 "nav", "tracks", "subtitles", "launch", "dialog", "home_page2",
                                                 "home_last", "login_error", "login_expired", "account_signed_in", "logout", "subtitle_appearance", "audio_languages", "movie_streams", "episodes",
                                                 "date_options", "launch_direct", "episodes_last", "buffering",
                                                 "downloads", "downloads_empty", "downloads_last", "download_folder", "download_notice", "next_episode", "download_folder_moving",
                                                 "downloads_series", "downloads_episodes", "downloads_specials",
                                                 "downloads_sort_title", "downloads_sort_year", "downloads_sort_date"}};
    if (std::find(names.begin(), names.end(), scenario) == names.end()) return false;
    Fixture fixture;
    if (!load_json(fixture_path, fixture.data) || !fixture.data.is_object()) return false;
    fixture.base = fs::absolute(fs::path(fixture_path)).parent_path();
    fixture.art = fixture.base / jstr(fixture.data, "artRoot");
    fixture.poster_fallback = path_string(fixture.base, jstr(fixture.data, "fallbackPoster"));
    fixture.background_fallback = path_string(fixture.base, jstr(fixture.data, "fallbackBackdrop"));
    const auto& metas = jobj(fixture.data, "metas");
    if (!metas.is_array() || metas.empty()) return false;
    for (const auto& item : metas) {
        const std::string id = jstr(item, "id");
        if (id.empty()) continue;
        UiCard card;
        card.title = jstr(item, "name");
        card.description = jstr(item, "description");
        card.year = jstr(item, "year");
        card.runtime = jstr(item, "runtime");
        card.rating = jstr(item, "rating");
        card.image = fixture.image(jstr(item, "poster"));
        card.logo = path_string(fixture.art, jstr(item, "logo"));
        const auto& library_ids = jobj(fixture.data, "library");
        card.in_library = library_ids.is_array() && std::find(library_ids.begin(), library_ids.end(), id) != library_ids.end();
        card.background = fixture.image(jstr(item, "background"), true);
        card.initials = initials(card.title);
        card.badge = jstr(item, "badge");
        card.progress = jstr(item, "progress", "0%");
        fixture.cards.emplace(id, std::move(card));
    }
    if (fixture.cards.empty()) return false;

    app.zone = "content";
    app.view = "home";
    app.nav_sel = 0;
    app.clock = "21:42";
    app.date = "mar 6 ott 2026";
    // The showcase's copy and metadata are Italian. This presentation-only
    // choice does not replace the stored/system language preference.
    app.ui_language = jstr(fixture.data, "language", "it");
    app.user.clear();
    app.banner.clear();
    app.toast.clear();
    app.busy = false;
    app.home_row = app.home_col = app.search_row = app.search_col = 0;
    app.dd_visible = app.login_visible = app.input_visible_ = app.launch_visible = false;
    app.directory_picker_visible = app.download_directory_notice = app.next_episode_visible = false;
    app.directory_picker_loading = app.directory_picker_committing = app.directory_picker_error = false;
    app.directory_move_done = 0; app.directory_move_total = -1;
    app.directory_move_items_done = app.directory_move_items_total = 0;
    app.directory_move_title.clear();
    app.dd_multiselect = false;
    app.login_state = App::LoginState::loading;
    app.login_seconds_remaining = 0;
    app.watching_ = app.menu_visible = app.w_buffering = app.w_paused = app.t_visible = false;
    app.info_visible = true;
    app.launch_logo.clear();
    app.launch_progress = -1;
    app.download_rows.clear();
    app.download_sel = 0;
    app.download_groups.clear();
    app.download_episode_rows.clear();
    app.download_group_sel = 0;
    app.download_season = -1;
    app.download_series_id.clear();
    app.download_status.clear();
    app.search_status.clear();
    app.home_rows.clear();
    const auto& rows = jobj(fixture.data, "home");
    if (rows.is_array()) for (const auto& source : rows) {
        UiRow row;
        row.title = jstr(source, "title");
        row.dom_id = "fixture-shelf-" + std::to_string(app.home_rows.size());
        row.see_all = false;
        row.continue_watching = jbool(source, "continueWatching");
        row.cards = fixture.collection(jobj(source, "items"));
        app.home_rows.push_back(std::move(row));
    }
    const auto& hero = fixture.card(jstr(fixture.data, "hero"));
    set_home_preview(app, hero);
    set_addons(app, fixture);
    if (scenario == "home") return true;
    if (scenario == "home_page2" || scenario == "home_last") {
        if (!app.home_rows.empty()) {
            // A longer host-only shelf makes both overflow edges and the final
            // stop observable. The ordinary Home fixture keeps its own rows.
            app.home_row = 0;
            auto& cards = app.home_rows.front().cards;
            for (const auto& [id, card] : fixture.cards) {
                const bool present = std::any_of(cards.begin(), cards.end(),
                    [&](const UiCard& item) { return item.title == card.title; });
                if (!present) cards.push_back(card);
            }
            if (!cards.empty()) {
                app.home_col = scenario == "home_page2" ? std::min(4, int(cards.size()) - 1) : int(cards.size()) - 1;
                set_home_preview(app, cards[size_t(app.home_col)]);
            }
        }
    } else if (scenario == "nav") {
        app.zone = "nav";
    } else if (scenario == "detail" || scenario == "streams" || scenario == "movie_streams" || scenario == "episodes" || scenario == "episodes_last") {
        set_detail(app, fixture, scenario == "streams" || scenario == "movie_streams");
        if (scenario == "episodes") app.d_zone = "episodes";
        if (scenario == "episodes_last") app.d_episode_sel = std::max(0, static_cast<int>(app.d_episodes.size()) - 1);
        if (scenario == "movie_streams") {
            app.d_series = false; app.d_episodes.clear(); app.d_zone = "streams";
            app.d_name = hero.title; app.d_background = hero.background; app.d_logo = hero.logo;
            app.d_description = hero.description; app.d_year = hero.year; app.d_runtime = hero.runtime;
            app.d_imdb = hero.rating; app.d_genres = "Fantascienza · Avventura · Dramma";
            app.d_cast.clear(); app.d_directors.clear(); app.d_resume.clear();
        }
    } else if (scenario == "discover") {
        app.view = "discover"; app.nav_sel = 1;
        app.disc_chips = {{"Film"}, {"Cinemeta"}, {"Tutti i generi"}};
        app.disc_chip = 0; app.disc_sel = 0;
        app.disc_cards = fixture.collection(jobj(fixture.data, "discover"));
        app.disc_status.clear();
        app.dp_name = hero.title; app.dp_desc = hero.description;
        app.dp_still.clear(); app.dp_logo = hero.logo;
        app.dp_year = hero.year; app.dp_runtime = hero.runtime;
        app.dp_imdb = hero.rating; app.dp_genres = "Fantascienza · Avventura · Dramma";
        app.dp_directors = "Christopher Nolan";
        app.dp_cast = "Matthew McConaughey · Anne Hathaway · Jessica Chastain";
    } else if (scenario == "library") {
        app.view = "library"; app.nav_sel = 2;
        app.lib_chips = {{"Tutti i tipi"}, {"Visti di recente"}};
        app.lib_cards = fixture.collection(jobj(fixture.data, "library"));
        app.lib_chip = 0; app.lib_sel = 0;
        app.lib_status = std::to_string(app.lib_cards.size()) + " titoli";
    } else if (scenario == "downloads" || scenario == "downloads_empty" || scenario == "downloads_last" ||
               scenario == "downloads_series" || scenario == "downloads_episodes" || scenario == "downloads_specials" ||
               scenario == "downloads_sort_title" || scenario == "downloads_sort_year" || scenario == "downloads_sort_date") {
        app.view = "downloads"; app.nav_sel = 3;
        const bool italian = app.ui_language != "en";
        const bool sort_gallery = scenario == "downloads_sort_title" || scenario == "downloads_sort_year" ||
                                  scenario == "downloads_sort_date";
        if (scenario != "downloads_empty") {
            std::vector<UiCard> covers;
            for (const auto& [id, card] : fixture.cards) covers.push_back(card);
            for (int i = 0; i < 6; ++i) {
                const auto& card = covers[static_cast<std::size_t>(i) % covers.size()];
                UiDownload item;
                item.id = "preview-download-" + std::to_string(i);
                item.media_id = item.id;
                item.type = "movie";
                item.title = card.title;
                item.release_year = std::atoi(card.year.c_str());
                constexpr std::array<int, 6> download_days{{2, 5, 3, 1, 4, 0}};
                item.downloaded_ms = 1792000000000ll + int64_t(download_days[size_t(i)]) * 86400000;
                item.subtitle = italian ? "Film · 1080p" : "Movie · 1080p";
                item.image = card.image;
                if (sort_gallery || i == 0 || i == 5) {
                    item.complete = true;
                    item.progress = 1;
                } else if (i == 1) {
                    item.active = true;
                    item.progress = .376f;
                    item.status = italian ? "Download in corso" : "Downloading";
                    item.size = "1,80 GB / 4,80 GB";
                    item.speed = "20,40 MB/s";
                    item.remaining = italian ? "2 min 30 sec rimanenti" : "2 min 30 sec remaining";
                    item.connections = "48 peers · 29 seeders";
                } else if (i == 2) {
                    item.paused = true;
                    item.progress = .64f;
                    item.status = italian ? "In pausa" : "Paused";
                    item.size = "2,50 GB / 3,90 GB";
                } else if (i == 3) {
                    item.progress = -1;
                    item.status = italian ? "In coda" : "Queued";
                    item.size = italian ? "In attesa della dimensione" : "Waiting for file size";
                } else {
                    item.failed = true;
                    item.progress = .03f;
                    item.status = italian ? "Connessione interrotta · Riprova per continuare" : "Connection interrupted · Retry to continue";
                    item.size = "120 MB / 4,00 GB";
                }
                app.download_rows.push_back(std::move(item));
                UiDownloadGroup group;
                group.id = app.download_rows.back().id;
                group.title = card.title; group.image = card.image;
                group.release_year = app.download_rows.back().release_year;
                group.downloaded_ms = app.download_rows.back().downloaded_ms;
                group.representative = i; group.rows = {i};
                group.complete = app.download_rows.back().complete ? 1 : 0;
                app.download_groups.push_back(std::move(group));
            }
            app.download_sel = scenario == "downloads_last" ? 5 : 1;
            app.download_group_sel = app.download_sel;
            app.download_status = italian ? "2 in coda o in download" : "2 queued or downloading";
            if (sort_gallery) {
                app.download_status.clear();
                const int mode = scenario == "downloads_sort_title" ? 0 : scenario == "downloads_sort_year" ? 1 : 2;
                // Use the real controller path to reorder the public gallery.
                // A full cycle also refreshes the order when the mode matches.
                do { app.on_button(Btn::R2); } while (app.settings().download_sort != mode);
            }
            if (scenario == "downloads_series" || scenario == "downloads_episodes" || scenario == "downloads_specials") {
                const auto& detail = jobj(fixture.data, "detail");
                const auto& card = fixture.card(jstr(detail, "id"));
                auto& active = app.download_rows[1];
                active.media_id = jstr(detail, "id"); active.type = "series";
                active.title = card.title; active.image = card.image;
                active.season = 1; active.episode = 3;
                active.episode_title = "Lifeboats";
                active.thumbnail = card.background;
                active.subtitle = "S01E03 · Lifeboats";
                auto& group = app.download_groups[1];
                group.id = "series:" + active.media_id;
                group.title = card.title; group.image = card.image;
                group.series = true; group.complete = 2;
                group.seasons = {0, 1, 3};
                const UiDownload prototype = active;
                for (int i = 0; i < 3; ++i) {
                    UiDownload episode = prototype;
                    episode.id = "preview-episode-" + std::to_string(i);
                    episode.season = i == 0 ? 1 : i == 1 ? 0 : 3;
                    episode.episode = i == 2 ? 2 : 1;
                    episode.episode_title = i == 0 ? "Celebration" : i == 1 ? "Behind the scenes" : "Mass in Time of War";
                    episode.active = false;
                    episode.complete = i < 2;
                    episode.progress = i < 2 ? 1 : -1;
                    episode.status = i < 2 ? "" : (italian ? "In coda" : "Queued");
                    episode.speed.clear(); episode.remaining.clear(); episode.connections.clear();
                    episode.size = i < 2 ? "" : (italian ? "In attesa della dimensione" : "Waiting for file size");
                    group.rows.push_back(int(app.download_rows.size()));
                    app.download_rows.push_back(std::move(episode));
                }
                app.download_status = italian ? "3 in coda o in download" : "3 queued or downloading";
                if (scenario != "downloads_series") {
                    app.download_series_id = group.id;
                    app.download_season = scenario == "downloads_specials" ? 0 : 1;
                    app.download_episode_rows = scenario == "downloads_specials" ? std::vector<int>{7} : std::vector<int>{6, 1};
                    app.download_sel = app.download_episode_rows.front();
                }
            }
        }
    } else if (scenario == "download_folder" || scenario == "download_folder_moving") {
        app.view = "settings"; app.nav_sel = 4;
        app.directory_picker_visible = true;
        app.directory_picker_path = "/mnt/ext1/Movies";
        app.directory_picker_entries = {"..", "Animation", "Documentaries", "Series", "Stremio Plus Downloads"};
        app.directory_picker_sel = 4;
        app.directory_picker_status.clear();
        if (scenario == "download_folder_moving") {
            app.directory_picker_committing = true;
            app.directory_move_done = 6ll << 30;
            app.directory_move_total = 18ll << 30;
            app.directory_move_items_done = 1; app.directory_move_items_total = 4;
            app.directory_move_title = "Succession · S1 E2";
            app.directory_picker_status = app.ui_language == "it" ? "Spostamento dei download…" : "Moving downloads…";
        }
    } else if (scenario == "download_notice") {
        set_detail(app, fixture, true);
        app.download_directory_notice = true;
    } else if (scenario == "addons") {
        app.view = "addons"; app.nav_sel = 4;
    } else if (scenario == "settings" || scenario == "subtitle_appearance" || scenario == "audio_languages" || scenario == "date_options") {
        app.view = "settings"; app.nav_sel = 4;
        const std::string id = scenario == "subtitle_appearance" ? "subtitle_background_opacity"
                             : scenario == "audio_languages" ? "audio_languages"
                             : scenario == "date_options" ? "date_components" : "display_resolution";
        const auto row = std::find_if(app.s_rows.begin(), app.s_rows.end(),
            [&](const UiSetting& setting) { return setting.id == id; });
        app.s_sel = row == app.s_rows.end() ? 0 : int(row - app.s_rows.begin());
        if (scenario == "audio_languages" || scenario == "date_options") app.on_button(Btn::Cross);
    } else if (scenario == "account_signed_in" || scenario == "logout") {
        // Run these with a separate --data directory containing explicitly
        // fake settings.json. Snapshot mode initialized the real App offline.
        app.view = "settings"; app.nav_sel = 4; app.s_sel = 0;
        const auto& email = app.settings().user_email;
        app.user = email.empty() ? "" : std::string(1, char(std::toupper(static_cast<unsigned char>(email[0]))));
        if (scenario == "logout") app.on_button(Btn::Cross);
    } else if (scenario == "dropdown") {
        app.view = "discover"; app.nav_sel = 1; app.zone = "filters";
        app.disc_chips = {{"Film"}, {"Cinemeta"}, {"Tutti i generi"}};
        app.disc_cards = fixture.collection(jobj(fixture.data, "discover"));
        app.dd_visible = true; app.dd_title = "Genere";
        app.dd_options = {{"Tutti i generi", true, ""}, {"Avventura", false, ""}, {"Azione", false, ""}, {"Commedia", false, ""},
                          {"Dramma", false, ""}, {"Fantascienza", false, ""}, {"Mistero", false, ""}, {"Thriller", false, ""}};
        app.dd_sel = 5;
    } else if (scenario == "login" || scenario == "login_error" || scenario == "login_expired") {
        app.view = "settings"; app.nav_sel = 4;
        app.s_sel = 0; app.login_visible = true;
        app.login_state = App::LoginState::ready;
        app.login_seconds_remaining = 295;
        app.login_link = "https://link.stremio.com";
        app.login_code = "DEMO";
        app.login_qr = path_string(fixture.art, "login-preview.rgba");
        app.login_status = "Anteprima grafica offline. Nessuna sessione collegata.";
        if (scenario != "login") {
            app.login_state = scenario == "login_expired" ? App::LoginState::expired : App::LoginState::error;
            app.login_seconds_remaining = 0;
            app.login_qr.clear();
            app.login_link.clear();
            app.login_code = scenario == "login_expired" ? "SCADUTO" : "";
            app.login_status = scenario == "login_expired" ? "Il codice di accesso è scaduto."
                                                           : "Impossibile contattare Stremio. Riprova.";
        }
    } else if (scenario == "keyboard") {
        app.view = "search"; app.zone = "searchbox";
        app.input_visible_ = true; app.input_title = "Cerca su Stremio";
        app.input_hint = "Film, serie, persone o il titolo che hai in mente.";
        app.input_value = "Interstellar";
    } else if (scenario == "player" || scenario == "tracks" || scenario == "subtitles" || scenario == "buffering" || scenario == "next_episode") {
        app.watching_ = true;
        app.w_title = hero.title;
        app.w_subtitle = "Anteprima dei controlli · 1920 × 1080";
        app.w_time = "32:18"; app.w_duration = "2:49:00"; app.w_progress = "19.11%";
        app.w_sub_rml = "Il viaggio continua.<br>Una riga di sottotitoli con accenti: perché, così.";
        app.w_sub_size = "sub-m";
        app.launch_image = hero.background; // static frame drawn by the host capture path
        app.m_audio = {{"Italiano · AAC · Stereo", true, "ita"}, {"English · AAC · Stereo", false, "eng"}};
        app.m_subs = {{"Disattivati", false, ""}, {"Italiano · SRT", true, "ita"}, {"English · SRT", false, "eng"}};
        app.m_audio_sel = 0; app.m_sub_sel = 1; app.m_col = 0; app.m_delay = "+0.00 s";
        app.w_stats.clear();
        if (scenario == "next_episode") {
            set_detail(app, fixture, false);
            app.next_episode_visible = true;
            app.next_episode_seconds = 12;
            app.next_episode_sel = 0;
            app.next_episode_label = "S1 · E2";
            if (!app.d_episodes.empty()) {
                const auto& episode = app.d_episodes[std::min<std::size_t>(1, app.d_episodes.size() - 1)];
                app.next_episode_title = episode.title;
                app.next_episode_thumb = episode.thumb;
            } else {
                app.next_episode_title = "Next episode";
                app.next_episode_thumb = hero.background;
            }
            app.info_visible = false;
            app.w_sub_rml.clear();
        } else if (scenario == "tracks") {
            app.menu_visible = true; app.m_col = 1;
            app.m_subs = {{"Disattivati", false, ""}, {"Italiano · SRT · OpenSubtitles", true, "ita"},
                          {"Italiano · Community Subtitles", false, "ita"}, {"English · SRT · OpenSubtitles", false, "eng"},
                          {"English · SDH · OpenSubtitles", false, "eng"}, {"Español · SRT", false, "spa"},
                          {"Français · SRT", false, "fre"}, {"Deutsch · SRT", false, "ger"}};
            app.w_sub_rml.clear();
        } else if (scenario == "subtitles") {
            app.info_visible = false;
            app.w_sub_rml = bidi_visual_rml("هذه ترجمة عربية على شاشة التلفاز.<br/>כתוביות בעברית על המסך.");
        } else if (scenario == "buffering") {
            app.info_visible = false;
            app.w_buffering = true;
            app.w_buffer_text = "Buffering · 37%";
            app.w_sub_rml.clear();
        }
    } else if (scenario == "launch" || scenario == "launch_direct") {
        app.launch_visible = true; app.launch_title = hero.title;
        app.launch_image = hero.background;
        app.launch_logo = hero.logo;
        app.launch_status = "Buffering iniziale · 62%\nPreparazione della riproduzione";
        app.launch_progress = .62f;
        app.t_visible = scenario == "launch";
        app.t_peers = "12"; app.t_speed = "7.40 MB/s"; app.t_progress = "3.20 %";
    } else if (scenario == "dialog") {
        app.view = "addons"; app.nav_sel = 4;
        app.dd_visible = true; app.dd_title = "Torrentio";
        app.dd_options = {{"Mantieni", true, ""}, {"Rimuovi add-on", false, ""}}; app.dd_sel = 0;
    } else if (scenario == "search") {
        app.view = "search"; app.search_query = "Interstellar";
        app.search_rows = {{"Cinemeta · Film", "fixture-search", "", false, {hero}}};
    }
    return true;
}
