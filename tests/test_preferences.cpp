// Stremio PS5 - Real settings, checklist and page-routing regressions.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include "calendar_date.h"
#include <curl/curl.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0, unexpected_http = 0;
void expect(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
int row(const App& app, const std::string& id) {
    for (size_t i = 0; i < app.s_rows.size(); ++i) if (app.s_rows[i].id == id) return int(i);
    throw std::runtime_error("Missing setting: " + id);
}
int language(const App& app, const std::string& code) {
    for (size_t i = 0; i < app.dd_options.size(); ++i) if (app.dd_options[i].lang == code) return int(i);
    throw std::runtime_error("Missing language: " + code);
}

void calendar_formats() {
    std::tm date{}; date.tm_year = 126; date.tm_mon = 9; date.tm_mday = 6; date.tm_wday = 2;
    expect(calendar_date(date, "it", "short", 15) == "mar 6 ott 2026", "Italian short date");
    expect(calendar_date(date, "it", "long", 15) == "martedì 6 ottobre 2026", "Italian long date preserves accented weekday");
    expect(calendar_date(date, "en", "short", 15) == "Tue Oct 6 2026", "English short date uses month before day");
    expect(calendar_date(date, "en", "long", 15) == "Tuesday October 6 2026", "English long date");
    expect(calendar_date(date, "it", "numeric", 14) == "06/10/2026", "Italian numeric date is day first");
    expect(calendar_date(date, "en", "numeric", 14) == "10/06/2026", "English numeric date is month first");
    expect(calendar_date(date, "it", "iso", 14) == "2026-10-06", "ISO date orders year month day");
    expect(calendar_date(date, "en", "iso", 15) == "Tue 2026-10-06", "ISO date keeps independently selected weekday");
    const char* component_dates[] = {"", "mar", "6", "mar 6", "ott", "mar ott", "6 ott", "mar 6 ott",
        "2026", "mar 2026", "6 2026", "mar 6 2026", "ott 2026", "mar ott 2026", "6 ott 2026", "mar 6 ott 2026"};
    for (int mask = 0; mask < 16; ++mask)
        expect(calendar_date(date, "it", "short", mask) == component_dates[mask], "all date component combinations have clean separators");
    expect(calendar_date(date, "it", "iso", DateYear | DateDay) == "2026-06", "omitted ISO month leaves no empty separator");
    expect(calendar_date(date, "it", "unknown", 15) == "mar 6 ott 2026", "unknown format falls back to short");
    date.tm_year = 124; date.tm_mon = 1; date.tm_mday = 29; date.tm_wday = 4;
    expect(calendar_date(date, "it", "long", 15) == "giovedì 29 febbraio 2024", "leap day renders correctly");
    date.tm_mon = 12;
    expect(calendar_date(date, "it", "short", 15).empty(), "invalid month cannot index month tables");
    date.tm_mon = 1; date.tm_wday = -1;
    expect(calendar_date(date, "it", "short", 15).empty(), "invalid weekday cannot index weekday tables");
}
}

extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void prepare(App& app, const std::string& directory) {
        app.offline_ = true;
        app.data_dir_ = app.base_dir_ = directory;
        app.settings_.ui_language = app.ui_language = "it";
        app.settings_.ui_language_mode = "it";
        app.view = "settings"; app.zone = "content"; app.nav_sel = 4;
        app.settings_refresh();
    }
    static void download_order(const std::string& directory) {
        App app; prepare(app, directory);
        expect(app.settings_.download_sort == 2, "download ordering defaults to newest download first");
        app.settings_.audio_langs = "eng, ita";
        app.settings_.download_directory = "/data/example-downloads";
        for (int mode = 0; mode < 3; ++mode) {
            app.settings_.download_sort = mode;
            app.offline_ = false; app.save_settings(); app.flush_settings(true);
            App restored; prepare(restored, directory); restored.load_settings();
            expect(restored.settings_.download_sort == mode && restored.settings_.audio_langs == "eng, ita" &&
                   restored.settings_.download_directory == "/data/example-downloads",
                   "each gallery ordering restores without changing other preferences or the selected storage directory");
        }
        for (const json& bad : {json(-1), json(3), json(1.5), json("year"), json(nullptr)}) {
            expect(save_json(directory + "/settings.json", json{{"download_sort", bad}}),
                   "invalid saved download-order fixture is written");
            App restored; prepare(restored, directory); restored.load_settings();
            expect(restored.settings_.download_sort == 2, "invalid ordering safely restores the default date mode");
        }
        expect(std::none_of(app.s_rows.begin(), app.s_rows.end(), [](const UiSetting& item) { return item.id == "download_sort"; }),
               "gallery ordering adds no unrelated row to the Settings page");
    }
    static void checklist(const std::string& directory) {
        App app; prepare(app, directory);
        app.settings_.audio_langs = "it-IT, en, ita, ast";
        app.settings_refresh();
        app.s_sel = row(app, "audio_languages");
        app.on_button(Btn::Cross);
        expect(app.dd_visible && app.dd_multiselect && !app.text_entry_active(), "audio preferences open a checklist, never keyboard");
        expect(app.dd_options.size() >= 43, "language picker offers the supported language set");
        expect(app.dd_options[0].lang == "ita" && app.dd_options[1].lang == "eng", "existing order is retained and aliases are normalized");
        expect(app.dd_options[language(app, "ast")].active, "uncommon existing language survives picker opening");
        expect(app.dd_options[language(app, "ita")].active && app.dd_options[language(app, "eng")].active, "existing preferences have checkmarks");
        app.dd_sel = language(app, "eng"); app.on_button(Btn::Cross);
        expect(app.dd_visible && !app.dd_options[app.dd_sel].active, "Cross unchecks and keeps picker open");
        expect(app.settings_.audio_langs == "ita, ast", "unchecking affects only the selected language");
        app.on_button(Btn::Cross);
        expect(app.dd_visible && app.dd_options[app.dd_sel].active, "Cross checks again without closing");
        expect(app.settings_.audio_langs == "ita, ast, eng", "new selections append in preference order");
        app.on_button(Btn::R1); app.on_button(Btn::L1);
        expect(app.view == "settings" && app.dd_visible, "picker owns shoulder input without changing background page");
        app.on_button(Btn::Circle);
        expect(!app.dd_visible && !app.dd_multiselect && !app.text_entry_active(), "Circle closes checklist cleanly");
        app.s_sel = row(app, "subtitle_languages"); app.on_button(Btn::Cross);
        expect(app.dd_visible && app.dd_multiselect, "subtitle language preferences also use a checklist");
        app.dd_sel = language(app, "ita"); app.on_button(Btn::Cross);
        app.dd_sel = language(app, "eng"); app.on_button(Btn::Cross);
        expect(app.settings_.subtitle_langs.empty(), "all languages can be deselected");
        app.on_button(Btn::Circle);
        app.s_sel = row(app, "subtitle_color"); app.on_button(Btn::Cross);
        expect(app.dd_visible && !app.dd_multiselect, "subsequent single-choice picker does not inherit checklist behavior");
        app.dd_sel = 1; app.on_button(Btn::Cross);
        expect(!app.dd_visible && app.settings_.sub_color == "yellow", "single color selection applies and closes normally");
        for (const auto& setting : app.s_rows) {
            expect(setting.id != "add_addon", "there is no manual Add add-on action");
            expect(setting.value.find("Account collegato") == std::string::npos, "settings do not show Account collegato");
        }
        app.s_sel = row(app, "addons"); app.on_button(Btn::Cross);
        expect(app.view == "addons" && app.nav_sel == 4, "add-ons open as a settings child page");
        app.on_button(Btn::Circle);
        expect(app.view == "settings" && app.nav_sel == 4, "Back from add-ons returns to settings");
    }
    static void navigation(const std::string& directory) {
        App app; prepare(app, directory);
        app.set_view("home");
        app.on_button(Btn::Up); app.on_button(Btn::Right); app.on_button(Btn::Left); app.on_button(Btn::Touchpad);
        expect(app.view == "home" && app.zone != "nav", "directional and touchpad input never focuses or switches top tabs");
        app.on_button(Btn::R1);
        expect(app.view == "discover" && app.nav_sel == 1 && app.zone != "nav", "R1 opens Discover");
        app.on_button(Btn::R1);
        expect(app.view == "library" && app.nav_sel == 2 && app.zone != "nav", "R1 opens Library");
        app.on_button(Btn::R1);
        expect(app.view == "downloads" && app.nav_sel == 3, "R1 opens Downloads immediately after Library");
        app.on_button(Btn::R1);
        expect(app.view == "settings" && app.nav_sel == 4, "R1 opens Settings without an Add-ons tab");
        app.on_button(Btn::R1);
        expect(app.view == "settings", "last tab clamps on R1");
        app.on_button(Btn::L1); app.on_button(Btn::L1); app.on_button(Btn::L1); app.on_button(Btn::L1);
        expect(app.view == "home" && app.nav_sel == 0 && app.zone != "nav", "L1 traverses the five main pages");
        app.on_button(Btn::Triangle);
        expect(app.view == "search" && app.text_entry_active(), "Triangle still opens separate search page");
        app.cancel_input(); app.on_button(Btn::Circle);
        expect(app.view == "home" && app.zone == "content", "Search Back returns to originating main page");
        app.view = "settings"; app.zone = "nav"; app.nav_sel = 4; app.s_sel = 0;
        app.on_button(Btn::Up);
        expect(app.view == "settings" && app.zone == "content", "legacy nav focus cannot reactivate header navigation");
    }
    static void date_and_controller(const std::string& directory) {
        App app; prepare(app, directory); app.refresh_clock();
        expect(app.settings_.date_components == 15 && app.settings_.date_format == "short" &&
            app.settings_.controller_ambient_light, "new defaults show a complete short date and enable title light");
        app.s_sel = row(app, "date_components"); app.on_button(Btn::Cross);
        expect(app.dd_visible && app.dd_multiselect && !app.text_entry_active() && app.dd_options.size() == 4,
            "date components open four independent checkboxes without keyboard");
        for (const auto& option : app.dd_options)
            expect(option.active && option.lang.empty(), "date choices are checked and have no language flags");
        for (int i = 0; i < 4; ++i) { app.dd_sel = i; app.on_button(Btn::Cross); }
        expect(app.dd_visible && app.settings_.date_components == 0 && app.date.empty(), "unchecking every component hides the date immediately");
        expect(app.s_rows[row(app, "date_components")].value == "Nascosta", "hidden date is clear in settings");
        app.dd_sel = 2; app.on_button(Btn::Cross);
        expect(app.settings_.date_components == DateMonth && !app.date.empty(), "rechecking a component immediately restores the date");
        app.on_button(Btn::Circle);
        app.s_sel = row(app, "date_format"); app.on_button(Btn::Cross);
        expect(app.dd_visible && !app.dd_multiselect && app.dd_options.size() == 4, "date format uses a separate single-choice picker");
        app.dd_sel = 3; app.on_button(Btn::Cross);
        expect(!app.dd_visible && app.settings_.date_format == "iso" && app.date.size() == 2,
            "format selection closes picker and updates the chosen month component");
        app.settings_.date_components = 15;
        app.set_setting_value(row(app, "date_format"), 1);
        const auto italian = app.date;
        app.set_setting_value(row(app, "ui_language"), 1);
        expect(!app.date.empty() && app.date != italian && app.date == calendar_date_now("en", "long", 15),
            "changing interface language refreshes the date without restart");
        app.s_sel = row(app, "controller_ambient_light"); app.on_button(Btn::Cross);
        expect(!app.settings_.controller_ambient_light && !app.s_rows[app.s_sel].on, "Cross turns title light off through the actual settings toggle");
        app.on_button(Btn::Cross);
        expect(app.settings_.controller_ambient_light && app.s_rows[app.s_sel].on, "Cross turns title light back on");
    }
    static void ambient_selection(const std::string& directory) {
        App app; prepare(app, directory);
        auto item = [](const char* id) { Item value; value.id = id; value.type = "series"; return value; };
        BoardRow ignored, selected; ignored.items = {item("wrong")}; selected.items = {item("first"), item("second")};
        app.board_ = {ignored, selected}; app.home_map_ = {1}; app.home_row = 0; app.home_col = 1; app.view = "home";
        expect(app.ambient_artwork().first == "series:second" && app.ambient_artwork().second.empty(),
            "ambient Home uses visible row mapping and keeps title identity while artwork is pending");
        app.search_ = {selected}; app.search_map_ = {0}; app.search_row = 0; app.search_col = 0; app.view = "search";
        expect(app.ambient_artwork().first == "series:first", "ambient Search follows selected title");
        app.disc_items_ = {item("discovery")}; app.disc_sel = 0; app.view = "discover";
        expect(app.ambient_artwork().first == "series:discovery", "ambient Discover follows selected title");
        app.lib_items_ = {item("library")}; app.lib_sel = 0; app.view = "library";
        expect(app.ambient_artwork().first == "series:library", "ambient Library follows selected title");
        app.d_item_ = item("detail"); app.view = "detail";
        expect(app.ambient_artwork().first == "series:detail", "ambient detail follows the open title");
        app.w_item_ = item("playing"); app.launch_visible = true;
        expect(app.ambient_artwork().first == "series:playing", "stream opening uses playback title before player starts");
        app.launch_visible = false; app.watching_ = true;
        expect(app.ambient_artwork().first == "series:playing", "playback color stays with playing title");
        app.login_visible = true;
        expect(app.ambient_artwork().first.empty(), "login clears ambient title even if stale playback state exists");
        app.login_visible = false; app.settings_.controller_ambient_light = false;
        expect(app.ambient_artwork().first.empty(), "disabled ambient light does not request title artwork");
        app.watching_ = false; app.settings_.controller_ambient_light = true; app.view = "settings";
        expect(app.ambient_artwork().first.empty(), "settings has no stale selected title color");
        app.view = "home"; app.home_col = 99;
        expect(app.ambient_artwork().first.empty(), "out-of-range cover selection resets ambient title safely");
    }
    static void interface_language(const std::string& directory) {
        const auto path = directory + "/language";
        make_dirs(path);
        App fresh;
        fresh.offline_ = true; fresh.data_dir_ = fresh.base_dir_ = path;
        expect(fresh.settings_.ui_language_mode == "auto" && fresh.ui_language == "en",
            "unresolved fresh settings start in Automatic with an English fallback");
        fresh.system_ui_language_ = "it";
        fresh.load_settings(); fresh.settings_refresh();
        expect(fresh.settings_.ui_language_mode == "auto" && fresh.ui_language == "it",
            "fresh Automatic interface follows an Italian system");
        fresh.settings_.audio_langs = "jpn, eng"; fresh.settings_.subtitle_langs = "deu, spa";
        fresh.set_setting_value(row(fresh, "ui_language"), 1);
        expect(fresh.ui_language == "en" && fresh.settings_.ui_language_mode == "en",
            "explicit English overrides an Italian system immediately");
        expect(fresh.settings_.audio_langs == "jpn, eng" && fresh.settings_.subtitle_langs == "deu, spa",
            "interface selection never alters audio or subtitle language preferences");
        fresh.offline_ = false; fresh.save_settings(); fresh.flush_settings(true);
        json persisted; expect(load_json(path + "/settings.json", persisted), "interface preference is saved");
        expect(jstr(persisted, "ui_language_mode") == "en" && jstr(persisted, "ui_language") == "en",
            "saved explicit mode is separate from the effective interface language");
        App restored; restored.offline_ = true; restored.data_dir_ = restored.base_dir_ = path;
        restored.system_ui_language_ = "it"; restored.load_settings(); restored.settings_refresh();
        expect(restored.ui_language == "en" && restored.settings_.ui_language_mode == "en",
            "explicit choice survives restart on a differently configured system");
        restored.set_setting_value(row(restored, "ui_language"), 0);
        expect(restored.ui_language == "it" && restored.settings_.ui_language_mode == "auto",
            "Automatic can be selected again from the settings picker");
        restored.offline_ = false; restored.save_settings(); restored.flush_settings(true);
        App changed_system; changed_system.offline_ = true; changed_system.data_dir_ = changed_system.base_dir_ = path;
        changed_system.system_ui_language_ = "fr"; changed_system.load_settings(); changed_system.settings_refresh();
        expect(changed_system.ui_language == "en" && changed_system.settings_.ui_language_mode == "auto",
            "saved Automatic is re-evaluated at startup and unsupported system languages use English");
        expect(changed_system.settings_.audio_langs == "jpn, eng" && changed_system.settings_.subtitle_langs == "deu, spa",
            "system language changes preserve independently saved media languages");
        changed_system.set_setting_value(row(changed_system, "ui_language"), 2);
        expect(changed_system.ui_language == "it" && changed_system.settings_.ui_language_mode == "it",
            "explicit Italian also overrides an unsupported system locale");
        expect(save_json(path + "/settings.json", json{{"ui_language", "it"}, {"audio_languages", "por"},
            {"subtitle_languages", "ara"}}), "legacy language fixture saved");
        App legacy; legacy.offline_ = true; legacy.data_dir_ = legacy.base_dir_ = path;
        legacy.system_ui_language_ = "en"; legacy.load_settings();
        expect(legacy.ui_language == "it" && legacy.settings_.ui_language_mode == "it",
            "upgrade retains a language saved by previous application versions");
        expect(legacy.settings_.audio_langs == "por" && legacy.settings_.subtitle_langs == "ara",
            "legacy language migration preserves media preferences");
        expect(save_json(path + "/settings.json", json{{"ui_language", "unknown"}}), "unsupported legacy choice fixture saved");
        App unknown; unknown.offline_ = true; unknown.data_dir_ = unknown.base_dir_ = path;
        unknown.system_ui_language_ = "es"; unknown.load_settings();
        expect(unknown.ui_language == "en" && unknown.settings_.ui_language_mode == "auto",
            "unsupported saved values and system language resolve to English");
    }
    static void appearance_and_persistence(const std::string& directory) {
        App app; prepare(app, directory);
        app.set_setting_value(row(app, "subtitle_background_opacity"), 35);
        expect(app.settings_.sub_background_opacity == 35, "background opacity changes the live model");
        app.set_setting_value(row(app, "subtitle_background_opacity"), -20);
        expect(app.settings_.sub_background_opacity == 0, "background opacity permits fully transparent and clamps below zero");
        app.set_setting_value(row(app, "subtitle_background_opacity"), 120);
        expect(app.settings_.sub_background_opacity == 100, "background opacity clamps above 100");
        app.set_setting_value(row(app, "subtitle_background_opacity"), std::numeric_limits<float>::quiet_NaN());
        expect(app.settings_.sub_background_opacity == 100, "nonfinite slider input is ignored");
        app.set_setting_value(row(app, "subtitle_background_opacity"), 25);
        app.set_setting_value(row(app, "subtitle_color"), 3);
        app.set_setting_value(row(app, "subtitle_effect"), 3);
        app.set_setting_value(row(app, "subtitle_font"), 1);
        app.set_setting_value(row(app, "subtitle_size"), 2);
        expect(app.settings_.sub_color == "cyan" && app.settings_.sub_effect == "raised" && app.settings_.sub_font == "serif", "color, raised effect and serif font are independently selectable");
        expect(app.w_sub_size == "sub-l", "size applies to active player model");
        const char* effects[] = {"none", "outline", "shadow", "raised", "depressed"};
        for (int i = 0; i < 5; ++i) {
            app.set_setting_value(row(app, "subtitle_effect"), float(i));
            expect(app.settings_.sub_effect == effects[i], "all five effects are selectable");
        }
        app.set_setting_value(row(app, "subtitle_font"), 2);
        expect(app.settings_.sub_font == "mono", "monospaced font is separately selectable");
        expect(app.settings_.shoulder_seek_seconds == 60 && app.settings_.seek_seconds == 10,
            "default shoulder and directional seek intervals remain distinct");
        app.set_setting_value(row(app, "shoulder_seek_seconds"), 75);
        expect(app.settings_.shoulder_seek_seconds == 75 && app.settings_.seek_seconds == 10,
            "changing shoulder seek preserves directional seek");
        app.set_setting_value(row(app, "seek_seconds"), 15);
        expect(app.settings_.shoulder_seek_seconds == 75 && app.settings_.seek_seconds == 15,
            "changing directional seek preserves shoulder seek");
        app.set_setting_value(row(app, "shoulder_seek_seconds"), -20);
        expect(app.settings_.shoulder_seek_seconds == 5, "shoulder seek clamps to five seconds");
        app.set_setting_value(row(app, "shoulder_seek_seconds"), 400);
        expect(app.settings_.shoulder_seek_seconds == 300, "shoulder seek clamps to five minutes");
        app.set_setting_value(row(app, "shoulder_seek_seconds"), 75);
        expect(app.settings_.torrent_speed_profile == 2,
            "new settings default to the Ultra fast torrent profile");
        app.set_setting_value(row(app, "torrent_speed_profile"), 0);
        expect(app.settings_.torrent_speed_profile == 0,
            "Balanced profile is an explicit saved choice, not a missing value");
        app.settings_.audio_langs = "ita, eng, jpn";
        app.settings_.date_components = DateDay | DateMonth;
        app.settings_.date_format = "numeric";
        app.settings_.controller_ambient_light = false;
        app.offline_ = false; app.save_settings(); app.flush_settings(true);
        json file; expect(load_json(directory + "/settings.json", file), "settings persist as JSON");
        expect(jstr(file, "subtitle_color") == "cyan" && jstr(file, "subtitle_effect") == "depressed" &&
            jstr(file, "subtitle_font") == "mono" && jnum(file, "subtitle_background_opacity") == 25,
            "persisted appearance fields match live selections");
        App restored; prepare(restored, directory); restored.load_settings();
        expect(jnum(file, "torrent_speed_profile", -1) == 0 &&
            restored.settings_.torrent_speed_profile == 0,
            "torrent profile restores after restart without resetting a saved Balanced choice");
        expect(restored.settings_.sub_color == "cyan" && restored.settings_.sub_effect == "depressed" &&
            restored.settings_.sub_font == "mono" && restored.settings_.sub_background_opacity == 25,
            "appearance restores after restart");
        expect(restored.settings_.audio_langs == "ita, eng, jpn", "audio preference order restores after restart");
        expect(jstr(file, "date_format") == "numeric" && jnum(file, "date_components") == 6 &&
            !jbool(file, "controller_ambient_light", true) && restored.settings_.date_format == "numeric" &&
            restored.settings_.date_components == 6 && !restored.settings_.controller_ambient_light,
            "date components, format and DualSense toggle persist and restore independently");
        expect(jnum(file, "shoulder_seek_seconds") == 75 && jnum(file, "seek_seconds") == 15 &&
            restored.settings_.shoulder_seek_seconds == 75 && restored.settings_.seek_seconds == 15,
            "both independent seek intervals persist and restore after restart");
        expect(save_json(directory + "/settings.json", json{{"subtitle_color", "broken"}, {"subtitle_effect", "broken"},
            {"subtitle_font", "broken"}, {"subtitle_background_opacity", -100}, {"date_format", "broken"},
            {"date_components", -100}, {"torrent_speed_profile", 1.5}}), "malformed-choice fixture saved");
        App invalid; prepare(invalid, directory); invalid.load_settings();
        expect(invalid.settings_.sub_color == "white" && invalid.settings_.sub_effect == "shadow" && invalid.settings_.sub_font == "sans", "unknown appearance values fall back to supported defaults");
        expect(invalid.settings_.sub_background_opacity == 0, "out-of-range persisted opacity is clamped");
        expect(invalid.settings_.torrent_speed_profile == 2,
            "fractional torrent profile falls back to Ultra fast without truncating to Fast");
        expect(invalid.settings_.date_components == 0 && invalid.settings_.date_format == "short",
            "invalid date format falls back and negative component mask clamps to zero");
        expect(save_json(directory + "/settings.json", json{{"date_components", 1000000},
            {"torrent_speed_profile", 1e100}}), "oversized numeric fixture saved");
        App oversized; prepare(oversized, directory); oversized.load_settings();
        expect(oversized.settings_.date_components == 15, "oversized persisted mask clamps before integer conversion");
        expect(oversized.settings_.torrent_speed_profile == 2,
            "oversized torrent profile falls back before integer conversion");
        expect(save_json(directory + "/settings.json", json{{"audio_languages", "it,en"}, {"subtitle_size", "sub-s"}}), "0.4.1 migration fixture saved");
        App migrated; prepare(migrated, directory); migrated.load_settings();
        expect(migrated.settings_.torrent_speed_profile == 2,
            "settings from a previous release acquire the Ultra fast torrent default");
        expect(migrated.settings_.sub_background_opacity == 70 && migrated.settings_.sub_color == "white" &&
            migrated.settings_.sub_effect == "shadow" && migrated.settings_.sub_font == "sans", "old settings acquire compatible subtitle appearance defaults");
        expect(migrated.settings_.audio_langs == "it,en" && migrated.settings_.sub_size == "sub-s", "migration preserves prior preferences");
        expect(migrated.settings_.shoulder_seek_seconds == 60,
            "old settings acquire the previous sixty-second shoulder seek as a compatible default");
        expect(migrated.settings_.date_components == 15 && migrated.settings_.date_format == "short" &&
            migrated.settings_.controller_ambient_light, "old settings acquire compatible date and controller light defaults");
    }
    static void video_controls_and_persistence(const std::string& directory) {
        const auto profile = directory + "/video-controls";
        expect(make_dirs(profile), "video settings fixture directory is created");
        App app; prepare(app, profile);
        expect(app.settings_.display_refresh == 0 && !app.settings_.audio_passthrough,
               "HDMI changes and passthrough remain opt-in");
        for (const auto& language : {"it", "en"}) {
            app.settings_.ui_language = language; app.settings_refresh();
            expect(std::none_of(app.s_rows.begin(), app.s_rows.end(), [](const auto& setting) {
                return setting.id == "video_upscaler" || setting.id == "video_sharpness" ||
                       setting.id == "motion_smoothing";
            }), "removed video effects are absent in both interface languages");
        }
        app.set_setting_value(row(app, "display_refresh"), 2);
        app.s_sel = row(app, "audio_passthrough"); app.on_button(Btn::Cross);
        expect(app.settings_.display_refresh == 2 && app.settings_.audio_passthrough,
               "retained video options route to their own values after settings regrouping");
        app.offline_ = false; app.save_settings(); app.flush_settings(true);
        App restored; prepare(restored, profile); restored.load_settings(); restored.settings_refresh();
        expect(restored.settings_.display_refresh == 2 && restored.settings_.audio_passthrough,
               "HDMI and audio options survive a real settings file round trip");
        restored.set_setting_value(row(restored, "display_refresh"), std::numeric_limits<float>::infinity());
        expect(restored.settings_.display_refresh == 2, "nonfinite input cannot change the HDMI preference");
        expect(save_json(profile + "/settings.json", json{{"video_upscaler", 1.5},
            {"display_refresh", 1e100}, {"motion_smoothing", -1}, {"video_sharpness", 9},
            {"audio_passthrough", "true"}}), "invalid video setting fixture saved");
        App invalid; prepare(invalid, profile); invalid.load_settings();
        expect(invalid.settings_.display_refresh == 0 && !invalid.settings_.audio_passthrough,
               "malformed HDMI and boolean settings fall back safely alongside ignored legacy effects");
        const json legacy = {{"video_upscaler", 2}, {"video_sharpness", .7}, {"motion_smoothing", 2},
            {"audio_languages", "eng, jpn"}, {"display_output_mode", "2160p"},
            {"display_refresh", 1}, {"audio_passthrough", true}, {"download_directory", profile + "/downloads"},
            {"auth_key", "fixture-account-token"}, {"user_email", "viewer@example.invalid"}};
        expect(save_json(profile + "/settings.json", legacy), "previous-version active video effects fixture saved");
        App upgraded; prepare(upgraded, profile); upgraded.load_settings(); upgraded.settings_refresh();
        upgraded.offline_ = false; upgraded.save_settings(); upgraded.flush_settings(true);
        json saved;
        expect(load_json(profile + "/settings.json", saved), "legacy effect preferences migrate through normal persistence");
        expect(!saved.contains("video_upscaler") && !saved.contains("video_sharpness") &&
               !saved.contains("motion_smoothing"), "retired effect values are ignored and omitted from the saved profile");
        for (const auto* key : {"audio_languages", "display_output_mode", "display_refresh", "audio_passthrough",
                               "download_directory", "auth_key", "user_email"})
            expect(saved.at(key) == legacy.at(key), "removing effects preserves existing media, display, download and account preferences");
        App restarted; prepare(restarted, profile); restarted.load_settings();
        expect(restarted.settings_.display_refresh == 1 && restarted.settings_.audio_passthrough &&
               restarted.settings_.display_resolution == 2 && restarted.settings_.audio_langs == "eng, jpn",
               "the upgraded profile restores its retained video and audio choices after restart");
    }
    static void engine_policy_and_migration(const std::string& directory) {
        const auto profile = directory + "/engine-policy";
        expect(make_dirs(profile), "engine migration fixture directory is created");
        App app; prepare(app, profile);
        for (const auto& language : {"it", "en"}) {
            app.settings_.ui_language = language; app.settings_refresh();
            expect(std::none_of(app.s_rows.begin(), app.s_rows.end(), [](const auto& setting) {
                return setting.id == "builtin_torrents" || setting.id == "server_url";
            }), "engine selection and external streaming server input are absent in both interface languages");
        }
        expect(row(app, "torrent_speed_profile") >= 0 && app.settings_.torrent_speed_profile == 2,
               "native torrent speed tuning remains available with Ultra fast as its default");
        expect(row(app, "audio_languages") >= 0 && row(app, "subtitle_languages") >= 0 &&
               row(app, "subtitle_effect") >= 0 && row(app, "display_resolution") >= 0,
               "removing engine selection preserves track preferences, subtitle appearance and output resolution");
        expect(save_json(profile + "/settings.json", json{{"builtin_torrents", false},
            {"server_url", "http://192.0.2.1:11470"}, {"audio_languages", "eng, jpn"},
            {"subtitle_font", "serif"}, {"display_output_mode", "1440p"}, {"torrent_speed_profile", 1},
            {"auth_key", "fixture-account-token"}, {"user_email", "viewer@example.invalid"}}),
            "legacy external-engine settings are stored for migration");
        expect(save_json(profile + "/config.json", json{{"server_url", "http://192.0.2.2:11470"}}),
            "legacy config server override is stored for migration");
        app.settings_.server_url = "http://192.0.2.3:11470";
        app.settings_.builtin_torrents = false;
        app.load_settings();
        expect(app.settings_.audio_langs == "eng, jpn" && app.settings_.sub_font == "serif" &&
               app.settings_.display_resolution == 1 && app.settings_.torrent_speed_profile == 1,
               "engine migration preserves audio, subtitle, output and native speed preferences");
#ifdef PLATFORM_PS5_NATIVE
        expect(app.settings_.builtin_torrents && app.settings_.server_url.empty() && app.server().empty(),
               "native PS5 settings migrate legacy server selection and config overrides to the local engine");
        app.settings_.server_url = "http://192.0.2.4:11470";
        app.settings_.builtin_torrents = false;
        expect(app.server().empty(), "native runtime cannot route playback through a stale external server value");
#else
        expect(!app.settings_.builtin_torrents && app.server() == "http://192.0.2.1:11470",
               "desktop configuration compatibility remains separate from the native PS5 engine policy");
#endif
        app.offline_ = false; app.save_settings(); app.flush_settings(true);
        json saved;
        expect(load_json(profile + "/settings.json", saved), "migrated engine settings persist normally");
#ifdef PLATFORM_PS5_NATIVE
        expect(jbool(saved, "builtin_torrents") && jstr(saved, "server_url").empty(),
               "native persistence replaces legacy engine choices with the built-in path");
#endif
        App restored; prepare(restored, profile); restored.load_settings();
#ifdef PLATFORM_PS5_NATIVE
        expect(restored.settings_.builtin_torrents && restored.server().empty(),
               "legacy config cannot restore an external playback engine after restart");
#endif
        expect(restored.settings_.audio_langs == "eng, jpn" && restored.settings_.sub_font == "serif" &&
               restored.settings_.display_resolution == 1 && restored.settings_.torrent_speed_profile == 1,
               "unrelated preferences survive the migrated profile round trip");
        expect(restored.settings_.auth_key == "fixture-account-token" && restored.settings_.user_email == "viewer@example.invalid",
               "forcing native playback preserves the signed-in account across migration and restart");
    }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        calendar_formats();
        AppProtocolTest::checklist(argv[1]);
        AppProtocolTest::navigation(argv[1]);
        AppProtocolTest::date_and_controller(argv[1]);
        AppProtocolTest::ambient_selection(argv[1]);
        AppProtocolTest::interface_language(argv[1]);
        AppProtocolTest::appearance_and_persistence(argv[1]);
        AppProtocolTest::video_controls_and_persistence(argv[1]);
        AppProtocolTest::engine_policy_and_migration(argv[1]);
        AppProtocolTest::download_order(argv[1]);
        expect(unexpected_http == 0, "no HTTP is required to edit settings or navigate offline");
        std::cout << "PREFERENCES_TESTS_OK checks=" << checks << " real_http_requests=" << unexpected_http << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "PREFERENCES_TESTS_FAILED: " << e.what() << '\n'; return 1;
    }
}
