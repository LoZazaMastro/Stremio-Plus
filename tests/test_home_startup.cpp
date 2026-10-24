// Production first-page model and actual carousel draw-list regressions.
// Network and PS5 APIs are not required. The GPU texture handles are fixtures.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "home_ui.h"
#include "core/input.hpp"
#include "gfx/font.hpp"
#include "ui/components/component.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
int checks = 0;
void expect(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
Item media(const std::string& id) {
    Item result;
    result.id = id; result.name = "Title " + id; result.type = "movie";
    return result;
}
BoardRow catalog(const std::string& name, bool loaded) {
    BoardRow result;
    result.title = name; result.loaded = loaded;
    if (loaded) result.items = {media(name + "-a"), media(name + "-b")};
    return result;
}
UiRow shelf(const std::string& id, const std::string& texture) {
    UiRow result;
    result.dom_id = id; result.title = id;
    for (int i = 0; i < 4; ++i) {
        UiCard card; card.title = id + std::to_string(i);
        card.image = i == 1 ? texture : "other";
        result.cards.push_back(card);
    }
    return result;
}
std::vector<hui::gfx::Instance> images(const hui::gfx::DrawList& list, std::uint32_t texture) {
    std::vector<hui::gfx::Instance> found;
    for (const auto& run : list.runs()) {
        if (run.mesh || run.texture != texture) continue;
        for (std::size_t i = run.first; i < run.first + run.count; ++i)
            if (int(list.instances()[i].params[3]) == int(hui::gfx::Shape::image))
                found.push_back(list.instances()[i]);
    }
    return found;
}
}

struct AppProtocolTest {
    static void prepare(App& app, double now, bool leading_ready = true, bool account_ready = true) {
        app.offline_ = true;
        app.view = "home"; app.zone = "content";
        app.settings_.auth_key = "offline-fixture";
        app.library_.clear(); app.library_loaded_ = false;
        app.library_initial_settled_ = account_ready;
        app.addons_loading_ = false;
        app.board_.clear(); app.home_map_.clear(); app.home_rows.clear();
        BoardRow continued;
        continued.title = "Continue Watching"; continued.continue_watching = true; continued.loaded = true;
        app.board_ = {continued, catalog("First", true), catalog("Second", leading_ready), catalog("Later", false)};
        app.home_startup_.begin(now);
        app.refresh_home_cards();
    }
    static void tick(App& app, double now, bool textures = true) { app.update_home_startup_at(now, textures); }
    static std::string selected(const App& app) {
        return app.board_.at(app.home_map_.at(app.home_row)).items.at(app.home_col).id;
    }
    static void model() {
        App app;
        prepare(app, 100, false, false);
        tick(app, 100);
        expect(app.home_startup_visible(), "one fast reply does not reveal a changing initial page");
        app.on_button(Btn::R1);
        expect(app.view == "home" && app.home_row == 0, "startup owns input while content is staged");
        tick(app, 100.4);
        expect(app.home_startup_visible(), "unknown account and second shelf are given a bounded chance to settle");
        app.board_[2] = catalog("Second", true); app.library_initial_settled_ = true;
        app.refresh_home_cards(); tick(app, 100.5); tick(app, 100.79);
        expect(app.home_startup_visible(), "new leading layout must settle before reveal");
        tick(app, 100.81);
        expect(!app.home_startup_visible() && app.home_startup_.result() == HomeStartupGate::Result::Ready,
               "ready leading shelves reveal without waiting for off-screen catalogs");
        expect(!app.board_[3].loaded, "slow lower catalog was actually still pending at reveal");

        prepare(app, 200);
        tick(app, 200, false); tick(app, 200.4, false);
        expect(app.home_startup_visible(), "downloaded artwork is not equivalent to uploaded GPU textures");
        tick(app, 200.5, true);
        expect(!app.home_startup_visible(), "completed visible texture uploads release the screen promptly");

        prepare(app, 300, false, false);
        tick(app, 300); tick(app, 301.49);
        expect(app.home_startup_visible(), "slow leading provider grace is respected");
        tick(app, 301.51);
        expect(!app.home_startup_visible() && app.home_startup_.result() == HomeStartupGate::Result::Partial,
               "slow leading provider and account cannot prevent a usable page");

        prepare(app, 400);
        app.board_[1].items[0].poster = "https://fixture.invalid/never-completes.jpg";
        app.refresh_home_cards(); tick(app, 400); tick(app, 401.2);
        expect(app.home_startup_visible(), "missing first-row artwork gets a bounded preparation window");
        tick(app, 401.26);
        expect(!app.home_startup_visible(), "failed artwork cannot extend loading indefinitely");

        prepare(app, 500);
        tick(app, 500);
        app.board_[3] = catalog("Later", true);
        app.refresh_home_cards(); tick(app, 500.31);
        expect(!app.home_startup_visible(), "unrelated lower rows do not restart the leading stability clock");

        prepare(app, 600, false, false);
        app.board_[1].items.clear(); app.board_[1].loaded = false; app.addons_loading_ = true;
        app.refresh_home_cards(); tick(app, 600); tick(app, 607.99);
        expect(app.home_startup_visible(), "unavailable providers are bounded by the total deadline");
        tick(app, 608);
        expect(!app.home_startup_visible() && app.home_startup_.result() == HomeStartupGate::Result::Deadline,
               "eight-second deadline works even with no provider or account callback");
        app.board_[1] = catalog("First", true); app.refresh_home_cards(); tick(app, 609);
        expect(!app.home_startup_visible(), "late results never re-open the first-page loading screen");

        prepare(app, 700);
        for (auto& row : app.board_) { row.items.clear(); row.loaded = true; }
        app.refresh_home_cards(); tick(app, 700); tick(app, 700.31);
        expect(!app.home_startup_visible() && app.home_startup_.result() == HomeStartupGate::Result::Empty,
               "known empty catalogs reveal an actionable empty page");

        prepare(app, 800); app.home_startup_.cancel();
        app.board_[1].loaded = false; app.board_[1].items.clear();
        app.refresh_home_cards(); app.home_row = 0; app.home_col = 1;
        const std::string focus = selected(app);
        app.board_[1] = catalog("First", true); app.refresh_home_cards();
        expect(app.home_row == 1 && app.home_col == 1 && selected(app) == focus,
               "late earlier catalog preserves the selected catalog and title");
        app.library_["resume"] = {{"_id", "resume"}, {"name", "Resume"}, {"type", "movie"},
            {"state", {{"timeOffset", 60000}, {"duration", 600000}, {"lastWatched", "2026-10-22T10:00:00Z"}}}};
        app.library_loaded_ = true; app.refresh_home_cards();
        expect(app.home_row == 2 && app.home_col == 1 && selected(app) == focus,
               "late account snapshot inserts Continue Watching without stealing selection");
        app.home_row = 0; app.home_col = 0;
        const std::string resumed = selected(app);
        app.library_["newer"] = {{"_id", "newer"}, {"name", "Newer"}, {"type", "movie"},
            {"state", {{"timeOffset", 80000}, {"duration", 600000}, {"lastWatched", "2026-10-23T10:00:00Z"}}}};
        app.refresh_home_cards();
        expect(app.home_col == 1 && selected(app) == resumed,
               "Continue Watching reorder keeps the same title selected");
    }
    static void start(App& app) { app.home_startup_.begin(0); }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        AppProtocolTest::model();
        std::ifstream source(std::string(argv[1]) + "/hui/fonts/inter-regular.huifont", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        hui::gfx::Font font;
        expect(font.load(bytes), "bundled font loads for production draw-list checks");
        hui::ui::Fonts fonts;
        fonts.regular = fonts.semibold = fonts.display = fonts.mono = fonts.pixel = fonts.hand = {&font, hui::gfx::kFontHandleBase + 1};
        App app;
        app.view = "home"; app.zone = "content";
        app.home_rows = {shelf("A", "A-image"), shelf("B", "focused-image")};
        app.home_row = 1; app.home_col = 1;
        HomeUi ui([](const std::string& path) -> std::uint32_t {
            if (path == "focused-image") return 42;
            if (path == "new-image") return 43;
            return path.empty() ? 0 : 17;
        });
        const auto draw = [&] {
            hui::gfx::DrawList list;
            hui::ui::Canvas canvas{list, fonts, 0, 0};
            ui.draw(app, canvas);
            return list;
        };
        ui.snap(app);
        const auto initial = images(draw(), 42);
        expect(initial.size() == 1, "selected shelf emits its own actual image draw command");
        app.home_rows.insert(app.home_rows.begin(), shelf("Late account", "new-image"));
        app.home_row = 2;
        ui.update(app, 0.016f);
        const auto inserted = images(draw(), 42);
        expect(inserted.size() == 1, "retained carousel artwork callback uses its new row index");
        expect(std::fabs(initial[0].color_top[3] - inserted[0].color_top[3]) < 0.002f,
               "late row does not restart the opacity entrance of visible covers");
        for (int i = 0; i < 4; ++i)
            expect(std::fabs(initial[0].rect[i] - inserted[0].rect[i]) < 0.05f,
                   "late row preserves the focused image viewport geometry");
        app.home_rows.erase(app.home_rows.begin()); app.home_row = 1;
        ui.update(app, 0.016f);
        const auto removed = images(draw(), 42);
        expect(removed.size() == 1 && std::fabs(removed[0].rect[1] - initial[0].rect[1]) < 0.05f,
               "removing an earlier empty row also preserves viewport placement");
        app.home_rows[1].cards.push_back(app.home_rows[1].cards.back());
        ui.update(app, 0.016f);
        const auto grown = images(draw(), 42);
        expect(grown.size() == 1 && grown[0].color_top[3] > 0.99f,
               "poster-count updates do not fade out an already visible catalog");

        AppProtocolTest::start(app);
        ui.update(app, 0.016f);
        expect(images(draw(), 42).empty(), "loading screen actually hides catalog image draws");
        hui::InputFrame input;
        input.connected = true; input.nav = hui::Direction::right;
        input.pressed = hui::action_bit(hui::Action::page_next);
        hui::ui::Feedback feedback;
        ui.handle(app, input, feedback);
        expect(app.home_col == 1 && app.view == "home", "loading screen masks controller actions in actual HomeUi");
        std::cout << "{\"result\":\"PASS\",\"checks\":" << checks << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HOME_STARTUP_FAIL: " << error.what() << '\n';
        return 1;
    }
}
