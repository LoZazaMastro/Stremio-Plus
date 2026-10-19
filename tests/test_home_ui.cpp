// Controller integration checks against App and the real homebrew-ui widgets.
// Link with the host application's objects except src/main.cpp. No GL context,
// catalog request, account login or media playback is required.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "home_ui.h"
#include "preview_fixture.h"
#include "tasks.h"
#include "core/input.hpp"
#include "ui/feedback.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
bool require(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_home_ui APP_DIRECTORY FIXTURE_JSON\n");
        return 2;
    }
    char temporary[] = "/tmp/stremio-ui-check-XXXXXX";
    const char* data_dir = mkdtemp(temporary);
    if (!data_dir) return 2;
    std::fprintf(stderr, "UI checks: initialize offline App\n");
    App app;
    if (!app.init(argv[1], data_dir, true)) return 2;
    std::fprintf(stderr, "UI checks: form routing\n");
    HomeUi ui;
    hui::ui::Feedback feedback;
    int failures = 0;
    auto check = [&](bool condition, const char* message) { if (!require(condition, message)) ++failures; };
    auto send = [&](hui::Direction direction, hui::Action action = hui::Action::count) {
        hui::InputFrame input;
        input.connected = true;
        input.nav = direction;
        if (action != hui::Action::count) input.pressed = hui::action_bit(action);
        feedback.clear();
        ui.update(app, .016f);
        ui.handle(app, input, feedback);
        ui.update(app, .016f);
    };
    using D = hui::Direction;
    using A = hui::Action;
    check(app.settings().download_sort == 2, "Downloads defaults to most recently downloaded first");
    auto setting = [&](const std::string& id) {
        const auto it = std::find_if(app.s_rows.begin(), app.s_rows.end(),
                                    [&](const UiSetting& row) { return row.id == id; });
        check(it != app.s_rows.end(), ("setting ID exists: " + id).c_str());
        return it == app.s_rows.end() ? 0 : static_cast<int>(it - app.s_rows.begin());
    };

    check(load_preview_fixture(app, "settings", argv[2]), "load host settings fixture");
    app.s_sel = setting("reduced_motion"); // a typed toggle
    send(D::right);
    check(app.settings().reduced_motion && app.ui_reduced_motion, "toggle reaches persistent model once");
    send(D::left);
    send(D::left);
    check(!app.settings().reduced_motion && app.zone == "content", "left at toggle limit keeps focus in form");
    app.s_sel = setting("sound_volume"); // a typed slider
    const int volume = app.settings().sound_volume;
    send(D::right);
    check(app.settings().sound_volume == volume + 5, "slider advances by one configured step");
    check(app.zone == "content", "slider consumes horizontal movement");

    app.s_sel = setting("display_resolution");
    send(D::down);
    check(app.s_sel == setting("display_refresh"), "Video and Display follows visual section order after effect controls are removed");
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_options.size() == 3, "display refresh retains its three HDMI policy choices");
    app.dd_sel = 2;
    send(D::none, A::confirm);
    check(app.settings().display_refresh == 2, "display refresh choice resolves the retained setting ID");
    send(D::down);
    check(app.s_sel == setting("audio_languages"), "form headers cannot take focus between display and audio controls");
    send(D::down);
    check(app.s_sel == setting("audio_passthrough"), "audio controls preserve their visual order after removed setting IDs");
    send(D::right);
    check(app.settings().audio_passthrough, "Audio passthrough toggle resolves its stable source ID");
    send(D::left);
    check(!app.settings().audio_passthrough, "Audio passthrough returns to its safe default without affecting another row");

    app.s_sel = setting("reduced_motion");
    app.s_rows[setting("reduced_motion")].enabled = false;
    send(D::none, A::confirm);
    check(!app.settings().reduced_motion, "disabled form row refuses confirm without backend activation");
    app.s_rows[setting("reduced_motion")].enabled = true;

    std::fprintf(stderr, "UI checks: choice modal\n");
    app.s_sel = setting("ui_language"); // opens the real dropdown callback
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_options.size() == 3, "language choice exposes Automatic, English and Italian");
    const int setting_focus = app.s_sel;
    send(D::down);
    check(app.dd_sel == 1 && app.s_sel == setting_focus, "modal owns navigation without moving underlying form");
    send(D::none, A::confirm);
    check(!app.dd_visible && app.settings().ui_language == "en", "modal commits actual choice and closes");

    std::fprintf(stderr, "UI checks: native torrent speed profile\n");
    app.s_sel = setting("torrent_speed_profile");
    check(app.settings().torrent_speed_profile == 2 && app.s_rows[app.s_sel].value == "Ultra fast",
          "new installations default to the maximum native torrent profile");
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_options.size() == 3 && app.dd_sel == 2,
          "torrent profile opens three choices with the current Ultra fast selection");
    send(D::up);
    send(D::none, A::confirm);
    check(!app.dd_visible && app.settings().torrent_speed_profile == 1 &&
          app.s_rows[app.s_sel].value == "Fast", "profile choice reaches the live settings model");
    send(D::none, A::confirm);
    send(D::down);
    send(D::none, A::confirm);
    check(app.settings().torrent_speed_profile == 2 && app.s_rows[app.s_sel].value == "Ultra fast",
          "maximum profile can be restored without opening a keyboard");

    std::fprintf(stderr, "UI checks: UTF-8 keyboard\n");
    send(D::none, A::north);
    check(app.view == "search" && app.input_visible_, "Search opens the controller keyboard");
    app.replace_input("città");
    send(D::none, A::west);
    check(app.input_value == "citt", "keyboard backspace removes a full UTF-8 code point");
    send(D::none, A::north);
    check(app.input_value == "citt ", "keyboard space shortcut reaches model");
    send(D::none, A::back);
    check(!app.input_visible_ && app.search_query.empty(), "keyboard cancel preserves the previous query");
    send(D::none, A::back);
    check(app.view == "settings", "Search Back returns to the originating Settings page");

    std::fprintf(stderr, "UI checks: focus transfer\n");
    app.s_sel = setting("reduced_motion");
    ui.update(app, .016f);
    hui::InputFrame lost;
    lost.pressed = hui::action_bit(A::confirm);
    lost.focus_lost = true;
    ui.handle(app, lost, feedback);
    check(!app.settings().reduced_motion, "system-owned input cannot activate background controls");

    check(load_preview_fixture(app, "library", argv[2]), "load populated grid fixture");
    send(D::right);
    check(app.lib_sel == 1, "grid and App agree on one horizontal move");
    send(D::down);
    check(app.lib_sel == 8, "library uses seven columns in model and presentation");
    send(D::left);
    send(D::up);
    check(app.lib_sel == 0, "grid returns to first poster deterministically");
    send(D::left);
    check(app.zone != "nav" && app.nav_sel == 2, "left boundary cannot focus or change the top sections");

    std::fprintf(stderr, "UI checks: shoulder-only top sections and Search page\n");
    check(load_preview_fixture(app, "home", argv[2]), "load home navigation fixture");
    send(D::none, A::menu);
    check(app.view == "home" && !app.input_visible_ && app.zone == "content",
          "Options on Home never opens Search or its keyboard");
    app.on_button(Btn::R3);
    check(app.view == "home" && !app.input_visible_, "R3 is not a second Search shortcut");
    send(D::up);
    check(app.zone == "content" && app.nav_sel == 0, "up from first shelf cannot focus the header");
    check(load_preview_fixture(app, "home", argv[2]), "restore public shelf model");
    send(D::right);
    check(app.nav_sel == 0 && app.view == "home", "horizontal cover navigation cannot switch sections");
    send(D::none, A::page_next);
    check(app.nav_sel == 1 && app.view == "discover" && app.zone != "nav", "R1 opens Discover exactly once");
    send(D::none, A::page_next);
    check(app.nav_sel == 2 && app.view == "library", "R1 opens Library");
    send(D::none, A::page_next);
    check(app.nav_sel == 3 && app.view == "downloads", "R1 opens Downloads directly after Library");
    send(D::none, A::page_next);
    check(app.nav_sel == 4 && app.view == "settings", "R1 opens Settings with no Add-ons home tab");
    send(D::none, A::page_next);
    check(app.nav_sel == 4 && app.view == "settings", "last section stops at Settings");
    send(D::none, A::page_prev);
    send(D::none, A::page_prev);
    send(D::none, A::page_prev);
    send(D::none, A::page_prev);
    check(app.nav_sel == 0 && app.view == "home" && app.zone == "content", "L1 returns to Home content");
    send(D::none, A::north);
    check(app.view == "search" && app.input_visible_, "Triangle opens Search page and query keyboard");
    send(D::none, A::back);
    check(app.view == "search" && !app.input_visible_ && app.zone == "searchbox",
          "canceling query keeps the real Search page accessible");
    send(D::left);
    check(app.view == "home" && app.zone == "content", "leaving Search returns to Home without header focus");

    std::fprintf(stderr, "UI checks: Downloads list and boundaries\n");
    check(load_preview_fixture(app, "downloads", argv[2]), "load mixed offline/active/paused download fixture");
    ui.snap(app);
    check(app.nav_sel == 3 && app.download_rows.size() == 6 && app.download_sel == 1,
          "Downloads fixture keeps separate rows and focuses the active video");
    check(app.download_rows[0].complete && app.download_rows[1].active && app.download_rows[2].paused,
          "download rows distinguish offline availability, progress and paused state");
    send(D::down);
    check(app.download_sel == 2 && app.view == "downloads", "Downloads moves to the next video once");
    send(D::up);
    check(app.download_sel == 1, "Downloads returns to its previous video");
    send(D::left);
    check(app.nav_sel == 3 && app.view == "downloads" && app.zone == "content",
          "Downloads cannot focus or change the header with the d-pad");
    check(load_preview_fixture(app, "downloads_last", argv[2]), "load last download fixture");
    send(D::down);
    check(app.download_sel == 5, "Downloads stops at the actual final video");
    check(load_preview_fixture(app, "downloads_empty", argv[2]), "load empty Downloads fixture");
    send(D::down);
    check(app.download_rows.empty() && app.download_sel == 0 && app.view == "downloads",
          "empty Downloads stays stable when navigating");
    send(D::none, A::page_prev);
    check(app.view == "library" && app.nav_sel == 2, "L1 leaves Downloads for Library");

    std::fprintf(stderr, "UI checks: Downloads trigger sorting and selection identity\n");
    check(load_preview_fixture(app, "downloads", argv[2]), "load Downloads before changing sort order");
    const std::vector<std::string> sort_titles{"zulu", "Èlite", "alpha", "BETA", "Àlba", "Omega"};
    const std::vector<int> sort_years{2010, 2015, 2020, 0, 2000, 2025};
    const std::vector<int64_t> sort_dates{1000, 6000, 3000, 5000, 2000, 4000};
    for (size_t i = 0; i < app.download_rows.size(); ++i) {
        app.download_rows[i].title = sort_titles[i];
        app.download_rows[i].release_year = sort_years[i];
        app.download_rows[i].downloaded_ms = sort_dates[i];
    }
    const auto unsorted_rows = app.download_rows;
    const std::string selected_download_id = app.download_rows[1].id;
    const std::string selected_group_id = app.download_groups[1].id;
    auto gallery_order = [&] {
        std::vector<std::string> ids;
        for (const auto& group : app.download_groups) ids.push_back(group.id);
        return ids;
    };
    auto expected_order = [&](std::initializer_list<int> indexes) {
        std::vector<std::string> ids;
        for (int index : indexes) ids.push_back(unsorted_rows[size_t(index)].id);
        return ids;
    };
    auto same_download_focus = [&] {
        return app.download_group_sel >= 0 && app.download_group_sel < int(app.download_groups.size()) &&
               app.download_sel >= 0 && app.download_sel < int(app.download_rows.size()) &&
               app.download_groups[size_t(app.download_group_sel)].id == selected_group_id &&
               app.download_rows[size_t(app.download_sel)].id == selected_download_id;
    };
    ui.snap(app);
    send(D::none, A::jump_next);
    check(app.settings().download_sort == 0 && gallery_order() == expected_order({4, 2, 3, 1, 5, 0}),
          "R2 wraps download date to title A-Z and folds Italian accents and case");
    check(same_download_focus() && app.download_group_sel == 3,
          "title sorting keeps the same selected download when its poster moves to another index");
    send(D::none, A::jump_next);
    check(app.settings().download_sort == 1 && gallery_order() == expected_order({5, 2, 1, 0, 4, 3}),
          "R2 selects release year descending with unknown years last");
    check(same_download_focus() && app.download_group_sel == 2,
          "year sorting preserves the selected title and its action target");
    send(D::none, A::jump_next);
    check(app.settings().download_sort == 2 && gallery_order() == expected_order({1, 3, 5, 2, 4, 0}),
          "R2 selects download date descending independently of the queue order");
    check(same_download_focus() && app.download_group_sel == 0 && app.nav_sel == 3 && app.view == "downloads",
          "trigger sorting keeps the selected video and stays on Downloads");
    send(D::none, A::jump_next);
    send(D::none, A::jump_prev);
    check(app.settings().download_sort == 2 && gallery_order() == expected_order({1, 3, 5, 2, 4, 0}) && same_download_focus(),
          "L2 wraps title back to download date without paging the cover selection");
    send(D::none, A::jump_prev);
    check(app.settings().download_sort == 1 && gallery_order() == expected_order({5, 2, 1, 0, 4, 3}) && same_download_focus(),
          "L2 moves back from download date to release year");
    send(D::none, A::jump_prev);
    check(app.settings().download_sort == 0 && gallery_order() == expected_order({4, 2, 3, 1, 5, 0}) && same_download_focus(),
          "L2 moves back from release year to title");
    check(std::equal(app.download_rows.begin(), app.download_rows.end(), unsorted_rows.begin(), unsorted_rows.end(),
          [](const UiDownload& after, const UiDownload& before) {
              return after.id == before.id && after.status == before.status && after.speed == before.speed &&
                     after.progress == before.progress && after.active == before.active && after.paused == before.paused &&
                     after.failed == before.failed && after.complete == before.complete;
          }), "sorting changes the gallery while retaining row order, queue states and torrent details");
    send(D::none, A::jump_prev);
    check(load_preview_fixture(app, "downloads_empty", argv[2]), "load empty Downloads for trigger sorting");
    send(D::none, A::jump_next);
    check(app.settings().download_sort == 0 && app.download_groups.empty() && app.download_sel == 0 && app.view == "downloads",
          "an empty Downloads page accepts one sort change without creating a selection");
    send(D::none, A::jump_prev);

    std::fprintf(stderr, "UI checks: grouped download coverflow and season inventory\n");
    check(load_preview_fixture(app, "downloads_series", argv[2]), "load grouped series gallery fixture");
    ui.snap(app);
    check(app.download_groups.size() == 6 && app.download_rows.size() == 9 &&
          app.download_groups[1].series && app.download_groups[1].rows.size() == 4,
          "one series poster represents every queued and offline episode");
    send(D::none, A::west);
    check(!app.download_series_id.empty() && !app.dd_visible && app.download_season == 1 &&
          app.download_episode_rows == std::vector<int>({6, 1}),
          "Square on a series opens its sorted inventory before any episode can be deleted");
    check(ui.wants_glass(), "episode inventory uses the floating glass panel");
    const int panel_sort = app.settings().download_sort;
    const int panel_selection = app.download_sel;
    const auto panel_group = app.download_series_id;
    send(D::none, A::jump_next);
    send(D::none, A::jump_prev);
    check(app.settings().download_sort == panel_sort && app.download_series_id == panel_group &&
          app.download_sel == panel_selection && app.download_season == 1 &&
          app.download_episode_rows == std::vector<int>({6, 1}),
          "L2/R2 inside the series panel preserve its season and episode selection without sorting the gallery");
    send(D::up);
    check(app.download_sel == 6, "episode list selects the earlier episode independently of queue insertion order");
    send(D::down);
    check(app.download_sel == 1, "episode list returns to the active download");
    send(D::none, A::page_next);
    check(app.download_season == 3 && app.download_episode_rows == std::vector<int>({8}) &&
          app.view == "downloads" && app.nav_sel == 3,
          "R1 skips empty seasons and keeps the queued episode accessible without changing main tabs");
    send(D::none, A::page_next);
    check(app.download_season == 3, "season bar stops at the actual last available season");
    send(D::none, A::page_prev);
    send(D::none, A::page_prev);
    check(app.download_season == 0 && app.download_episode_rows == std::vector<int>({7}),
          "L1 reaches saved specials through the same season bar");
    send(D::none, A::back);
    check(app.download_series_id.empty() && app.download_group_sel == 1 && app.download_sel == 1,
          "Circle collapses the series panel to its original poster and active download");
    check(ui.wants_glass(), "the outgoing episode panel remains drawable during its return fade");
    ui.snap(app);
    check(!ui.wants_glass(), "the return animation releases the glass pass after the episode panel disappears");
    send(D::none, A::page_prev);
    check(app.view == "library", "L1 changes main pages again after the episode panel closes");

    std::fprintf(stderr, "UI checks: language checklist and caption appearance\n");
    check(load_preview_fixture(app, "settings", argv[2]), "load settings for typed language picker");
    app.s_sel = setting("audio_languages");
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_multiselect && !app.input_visible_, "audio languages open a checklist, never keyboard");
    check(!app.dd_options.empty() && app.dd_options[0].lang == "ita", "language checklist exposes canonical flag metadata");
    const bool checked = app.dd_options[0].active;
    app.dd_sel = 0;
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_options[0].active != checked, "Cross toggles one language and leaves checklist open");
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_options[0].active == checked, "Cross can deselect and reselect independently");
    send(D::none, A::back);
    check(!app.dd_visible && !app.input_visible_, "Circle closes checklist without opening keyboard");
    app.s_sel = setting("subtitle_background_opacity");
    const int opacity = app.settings().sub_background_opacity;
    send(D::right);
    check(app.settings().sub_background_opacity == std::min(100, opacity + 5), "subtitle opacity slider reaches live shared appearance model");
    app.s_sel = setting("shoulder_seek_seconds");
    const int directional_step = app.settings().seek_seconds;
    const int shoulder_step = app.settings().shoulder_seek_seconds;
    send(D::right);
    check(app.settings().shoulder_seek_seconds == std::min(300, shoulder_step + 5) &&
          app.settings().seek_seconds == directional_step, "shoulder seek slider is independent of directional seek");
    app.s_sel = setting("subtitle_color");
    send(D::none, A::confirm);
    app.dd_sel = 1;
    send(D::none, A::confirm);
    check(app.settings().sub_color == "yellow", "subtitle color choice reaches shared preview/player settings");
    check(std::none_of(app.s_rows.begin(), app.s_rows.end(), [](const UiSetting& row) { return row.id == "add_addon"; }),
          "settings no longer offer manual Add add-on");

    std::fprintf(stderr, "UI checks: date components checklist\n");
    app.s_sel = setting("date_components");
    const int date_mask = app.settings().date_components;
    send(D::none, A::confirm);
    check(app.dd_visible && app.dd_multiselect && !app.input_visible_ && app.dd_options.size() == 4,
          "date components open four independent checkboxes, not the keyboard");
    check(std::all_of(app.dd_options.begin(), app.dd_options.end(), [](const UiMenuItem& item) { return item.lang.empty(); }),
          "date component choices have no language flags");
    app.dd_sel = 0;
    send(D::none, A::confirm);
    check(app.dd_visible && app.settings().date_components == (date_mask ^ 1),
          "date checkbox toggles exactly one component and keeps the sheet open");
    send(D::none, A::confirm);
    check(app.settings().date_components == date_mask, "date checkbox can restore the prior format components");
    send(D::none, A::back);
    check(!app.dd_visible && app.view == "settings", "Circle closes date checklist without leaving Settings");
    app.s_sel = setting("date_format");
    send(D::none, A::confirm);
    check(app.dd_visible && !app.dd_multiselect && app.dd_options.size() == 4, "date format opens an exclusive format choice");
    app.dd_sel = 2;
    send(D::none, A::confirm);
    check(!app.dd_visible && app.settings().date_format == "numeric", "date format commits numeric ordering through the real form");

    std::fprintf(stderr, "UI checks: playback bar and audio/subtitle shoulders\n");
    check(load_preview_fixture(app, "player", argv[2]), "load player control fixture");
    ui.snap(app);
    check(ui.wants_glass(), "visible playback bar requests blurred video background");
    send(D::up);
    check(!app.menu_visible, "Up never opens the audio/subtitle menu");
    app.menu_visible = true;
    app.m_col = 0;
    send(D::none, A::page_next);
    check(app.m_col == 1 && app.menu_visible, "R1 selects subtitles without leaving the player menu");
    send(D::none, A::page_prev);
    check(app.m_col == 0 && app.menu_visible, "L1 selects audio without leaving the player menu");
    send(D::none, A::back);
    check(!app.menu_visible, "Circle closes the player menu");
    app.w_paused = true;
    app.info_visible = true;
    send(D::none, A::back);
    check(app.watching() && !app.info_visible, "Circle on a visible bar hides controls without exiting playback");
    ui.snap(app);
    check(!ui.wants_glass(), "explicit hidden controls stay hidden while paused");
    app.w_buffering = true;
    ui.snap(app);
    check(!ui.wants_glass(), "buffering cannot force the hidden playback bar to reappear");
    send(D::down);
    check(app.watching() && app.info_visible && !app.menu_visible, "Down reveals controls without opening track menu");
    send(D::none, A::back);
    send(D::up);
    check(app.watching() && app.info_visible && !app.menu_visible, "Up also reveals controls without opening track menu");
    send(D::none, A::back);
    send(D::none, A::back);
    check(!app.watching(), "Circle exits only after the playback bar is already hidden");
    app.watching_ = false;
    check(load_preview_fixture(app, "buffering", argv[2]), "load card-free playback buffering fixture");
    ui.snap(app);
    check(app.w_buffering && !app.info_visible && !ui.wants_glass(),
          "standalone buffering indicator never requests a glass card");
    app.watching_ = false;

    std::fprintf(stderr, "UI checks: continuous shelf focus\n");
    check(load_preview_fixture(app, "home", argv[2]), "load first-card shelf fixture");
    send(D::right);
    check(app.home_col == 1, "first shelf card advances to its actual neighbour");
    // The screenshot fixture supplies public rows, not App's private catalog
    // cache; a move rebuilds those rows. Restore the fixture for each input.
    check(load_preview_fixture(app, "home", argv[2]), "restore shelf model after catalog refresh");
    app.home_col = 1;
    send(D::left);
    check(app.home_col == 0 && app.zone == "content", "first card remains fully navigable");
    check(load_preview_fixture(app, "home_page2", argv[2]), "load middle-card shelf fixture");
    const int middle = app.home_col;
    send(D::right);
    check(app.home_col == middle + 1, "middle focus advances once without page-index translation");
    check(load_preview_fixture(app, "home_page2", argv[2]), "restore middle shelf model after catalog refresh");
    app.home_col = middle + 1;
    send(D::left);
    check(app.home_col == middle, "continuous shelf returns to its previous actual item");
    check(load_preview_fixture(app, "home_last", argv[2]), "load last-card shelf fixture");
    const int last = app.home_col;
    send(D::right);
    check(app.home_col == last && app.zone == "content", "last card stops at the true catalog boundary");
    send(D::left);
    check(app.home_col == last - 1, "last card can return to its adjacent predecessor");

    std::fprintf(stderr, "UI checks: repeated library notification events\n");
    const auto first_toast_revision = app.toast_revision;
    app.notify("Aggiunto alla libreria", 2.5);
    ui.update(app, .016f);
    check(app.toast_revision == first_toast_revision + 1, "first library action publishes a distinct notification event");
    app.notify("Aggiunto alla libreria", 2.5);
    ui.update(app, .016f);
    check(app.toast_revision == first_toast_revision + 2, "identical consecutive library message publishes another notification event");

    std::fprintf(stderr, "UI checks: complete provider information\n");
    check(load_preview_fixture(app, "streams", argv[2]), "load provider information fixture");
    ui.snap(app);
    const std::string raw_description = app.d_streams.front().desc;
    send(D::none, A::north);
    check(ui.wants_glass() && app.view == "detail", "provider information opens a presentation modal");
    send(D::down);
    check(app.d_stream_sel == 0, "source text scrolling cannot select another stream");
    send(D::none, A::back);
    ui.snap(app);
    check(!ui.wants_glass() && app.d_streams.front().desc == raw_description,
          "provider information closes and retains raw add-on metadata");

    const std::string old_view = app.view;
    check(!load_preview_fixture(app, "unknown", argv[2]) && app.view == old_view,
          "invalid capture scenario cannot alter model");
    check(!load_preview_fixture(app, "home", "/missing/stremio-fixture.json"), "missing fixture fails visibly");
    std::fprintf(stderr, "UI checks: dedicated login page\n");
    check(load_preview_fixture(app, "login_error", argv[2]), "load failed login fixture");
    ui.snap(app);
    check(!ui.wants_glass(), "login is a full page and never blurs the underlying settings");
    const int previous_setting = app.s_sel;
    send(D::down);
    check(app.s_sel == previous_setting, "login owns input rather than moving background settings");
    // This test does not start g_tasks workers: retry queues the real action
    // but cannot execute an account request or contact any network service.
    const auto requests = g_tasks.pending();
    send(D::none, A::confirm);
    check(app.login_visible && app.login_state == App::LoginState::loading &&
          app.login_code.empty() && app.login_qr.empty() && g_tasks.pending() == requests + 1,
          "Cross requests a fresh link and clears the expired payload exactly once");
    send(D::none, A::confirm);
    check(g_tasks.pending() == requests + 1, "loading retry button cannot queue duplicate requests");
    send(D::none, A::back);
    check(app.wants_exit() && app.login_visible,
          "Circle without an account exits rather than exposing the catalog");
    g_tasks.stop(); // Discard the queued, deliberately unexecuted account request.
    app.watching_ = false;
    std::fprintf(stderr, "UI checks: shutdown\n");
    app.shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(data_dir, ignored);
    if (failures) return 1;
    std::puts("PASS: native UI controller integration (typed form, checklist, subtitle appearance, glass player, card-free buffering, five shoulder sections, Downloads navigation, tracks, Search, continuous shelves, grid, source info, login retry/exit)");
    return 0;
}
