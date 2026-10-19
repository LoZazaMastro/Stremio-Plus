// Stremio PS5 - Focused metadata enrichment against controlled boundaries.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0;
std::atomic<int> calls{0}, http_calls{0};
std::vector<ArtKind> requested_art;
void expect(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < end) {
        g_tasks.drain(); if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("metadata response timeout");
}
struct Reply {
    json data;
    bool ok;
    std::atomic<bool> entered{false}, released{false};
    std::string url;
};
std::mutex replies_mutex;
std::deque<std::shared_ptr<Reply>> replies;
std::vector<std::shared_ptr<Reply>> all_replies;
std::shared_ptr<Reply> reply(json data, bool ok = true, bool released = true) {
    auto r = std::make_shared<Reply>(); r->data = std::move(data); r->ok = ok; r->released = released;
    std::lock_guard<std::mutex> lock(replies_mutex); replies.push_back(r); all_replies.push_back(r); return r;
}
json art(const char* logo = "https://art.test/title.png") {
    return {{"meta", {{"id", "tt001"}, {"type", "movie"}, {"logo", logo},
                     {"background", "https://art.test/hero.jpg"}, {"description", "Provider description"}}}};
}
void fence() {
    auto done = std::make_shared<std::atomic<bool>>(false);
    g_tasks.run([done] { g_tasks.post([done] { done->store(true); }); });
    until([&] { return done->load(); });
}
}

bool wrapped_fetch(const std::string&, json&, std::string&, long, const std::atomic<bool>*) asm(WRAP_FETCH_JSON);
bool wrapped_fetch(const std::string& url, json& document, std::string& error, long timeout, const std::atomic<bool>*) {
    std::shared_ptr<Reply> r;
    {
        std::lock_guard<std::mutex> lock(replies_mutex);
        if (replies.empty()) throw std::runtime_error("unexpected preview metadata request");
        r = replies.front(); replies.pop_front();
    }
    r->url = url; r->entered = true; ++calls;
    if (timeout != 6) throw std::runtime_error("preview request timeout was not bounded");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!r->released && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!r->released) throw std::runtime_error("unreleased fixture gate");
    document = r->data; error = r->ok ? "" : "fixture failure"; return r->ok;
}
std::string wrapped_art(App*, const std::string&, ArtKind, bool) asm(WRAP_APP_ART);
std::string wrapped_art(App*, const std::string& url, ArtKind kind, bool) {
    if (!url.empty()) requested_art.push_back(kind);
    return url;
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++http_calls; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void prepare(App& app) {
        app.offline_ = false; app.view = "home"; app.zone = "content";
        Item first; first.id = "tt001"; first.type = "movie"; first.name = "Catalog title";
        first.metadata_addon_url = "https://b.test/manifest.json";
        Item second = first; second.id = "tt002"; second.name = "Second title";
        BoardRow row; row.items = {first, second}; row.loaded = true;
        app.board_ = {row}; app.home_map_ = {0};
        for (const char* name : {"a", "b", "c", "d"}) {
            auto addon = std::make_unique<Addon>();
            const json manifest = {{"id", name}, {"name", name}, {"types", json::array({"movie"})}, {"resources", json::array({"meta"})}};
            expect(parse_addon(manifest, std::string("https://") + name + ".test/manifest.json", *addon), "metadata provider fixture parses");
            app.addons_.push_back(std::move(addon));
        }
    }
    static void settle(App& app) {
        app.update_preview_metadata();
        app.preview_metadata_selected_at_ -= 1;
        app.update_preview_metadata();
    }
    static void load_and_cache() {
        App app; prepare(app);
        const int before = calls;
        app.update_preview_metadata();
        expect(!app.preview_metadata_loading_ && calls == before, "moving focus starts a debounce, not a request");
        auto response = reply(art(), true, false);
        settle(app); until([&] { return response->entered.load(); });
        expect(response->url.find("https://b.test/") == 0, "catalog metadata provider is preferred by installation identity");
        for (int i = 0; i < 120; ++i) app.update_preview_metadata();
        expect(calls == before + 1, "stable focus never duplicates an in-flight metadata request");
        response->released = true; until([&] { return !app.preview_metadata_loading_; });
        expect(app.board_[0].items[0].logo == "https://art.test/title.png", "missing catalog title logo is enriched from metadata");
        expect(app.home_preview_logo == "https://art.test/title.png" && app.home_preview_background == "https://art.test/hero.jpg", "enrichment reaches the real home hero model");
        expect(app.board_[0].items[0].name == "Catalog title", "enrichment preserves catalog title identity");
        expect(app.board_[0].items[0].description == "Provider description", "missing summary is enriched with artwork");
        app.board_[0].items[0].logo.clear(); app.board_[0].items[0].background.clear();
        app.update_preview_metadata();
        expect(calls == before + 1 && !app.board_[0].items[0].logo.empty(), "in-memory cache enriches a rebuilt catalog without HTTP");
        app.board_[0].items[0].metadata_addon_url = "https://other.test/manifest.json";
        app.board_[0].items[0].logo.clear(); app.board_[0].items[0].background.clear();
        app.update_preview_metadata();
        expect(app.board_[0].items[0].logo.empty(), "same content ID from another installation does not inherit cached provider art");
        fence();
    }
    static void stale_selection_and_account() {
        App app; prepare(app);
        auto response = reply(art(), true, false);
        settle(app); until([&] { return response->entered.load(); });
        auto cancel = app.preview_metadata_cancel_;
        app.home_col = 1; app.update_preview_metadata();
        expect(cancel->load(), "moving cover focus cancels the previous metadata request");
        response->released = true; fence();
        expect(app.board_[0].items[0].logo.empty() && app.board_[0].items[1].logo.empty(), "late previous-cover response cannot overwrite current artwork");
        auto second = reply(art("https://art.test/account.png"), true, false);
        settle(app); until([&] { return second->entered.load(); });
        ++app.account_generation_;
        second->released = true; fence();
        expect(app.board_[0].items[1].logo.empty(), "old-account response is discarded");
        app.view = "detail"; app.update_preview_metadata();
        expect(!app.preview_metadata_loading_ && app.preview_metadata_key_.empty(), "leaving catalog pages clears preview work state");
    }
    static void discover_details_without_hero() {
        App app; prepare(app);
        app.view = "discover";
        app.disc_items_ = {app.board_[0].items[0]};
        app.disc_items_[0].logo = "https://art.test/existing-logo.png";
        app.disc_items_[0].background = "https://art.test/existing-hero.jpg";
        app.disc_cards.resize(1);
        const int before = calls;
        auto metadata = art();
        metadata["meta"]["releaseInfo"] = "2026";
        metadata["meta"]["runtime"] = "121 min";
        metadata["meta"]["imdbRating"] = "8.2";
        metadata["meta"]["genres"] = json::array({"Drama", "Mystery"});
        metadata["meta"]["director"] = json::array({"Example director"});
        metadata["meta"]["cast"] = json::array({"Actor one", "Actor two", "Actor three", "Actor four", "Actor five", "Actor six"});
        reply(std::move(metadata));
        requested_art.clear();
        settle(app); until([&] { return !app.preview_metadata_loading_; });
        expect(calls == before + 1, "Discover fills descriptive metadata even when the title logo already exists");
        expect(app.dp_name == "Catalog title" && app.dp_logo == "https://art.test/existing-logo.png" && app.dp_still.empty(),
               "Discover preserves the selected title and logo while suppressing the backdrop model");
        expect(app.dp_year == "2026" && app.dp_runtime == "121 min" && app.dp_imdb == "8.2" &&
               app.dp_genres == "Drama · Mystery" && app.dp_directors == "Example director" &&
               app.dp_desc == "Provider description", "provider year, rating, duration, genres, director and description reach Discover");
        expect(app.dp_cast.find("Actor five") != std::string::npos && app.dp_cast.find("Actor six") == std::string::npos,
               "Discover bounds the visible principal cast to five names");
        expect(std::none_of(requested_art.begin(), requested_art.end(), [](ArtKind kind) {
            return kind == ArtKind::Background || kind == ArtKind::Backdrop || kind == ArtKind::Still;
        }), "Discover enrichment never requests or decodes a hidden hero image");
        for (int i = 0; i < 100; ++i) app.update_preview_metadata();
        expect(calls == before + 1, "complete Discover metadata is reused without duplicate provider requests");
        fence();
    }
    static void provider_bound_and_retry() {
        App app; prepare(app);
        const int before = calls;
        reply({}, false); reply({{"meta", {{"id", "tt001"}}}}); reply({}, false);
        settle(app); until([&] { return !app.preview_metadata_loading_; });
        expect(calls == before + 3, "preview resolution visits at most three compatible providers");
        expect(app.preview_metadata_.size() == 1, "missing artwork has a retry cache entry");
        for (int i = 0; i < 20; ++i) app.update_preview_metadata();
        expect(calls == before + 3, "missing artwork does not retry on every frame");
        app.preview_metadata_.begin()->second.retry_at = now_seconds() - 1;
        auto recovered = reply(art()); app.update_preview_metadata();
        until([&] { return !app.preview_metadata_loading_; });
        expect(recovered->entered && !app.home_preview_logo.empty(), "artwork can recover after retry expiry");
        fence();
    }
    static void bounded_cache_and_offline() {
        App app; prepare(app);
        for (int i = 0; i < 96; ++i) app.preview_metadata_["older-" + std::to_string(i)] = {{}, 0, double(i)};
        reply(art()); settle(app); until([&] { return !app.preview_metadata_loading_; });
        expect(app.preview_metadata_.size() == 96 && !app.preview_metadata_.count("older-0"), "metadata cache evicts the least-recent entry at96items");
        const int before = calls;
        app.home_col = 1; app.offline_ = true; settle(app);
        expect(calls == before && !app.preview_metadata_loading_, "offline fixtures never trigger metadata network work");
        fence();
    }
    static void visible_art_retry() {
        App app; app.offline_ = true; app.view = "home"; app.zone = "content";
        Item item; item.id = "tt-retry"; item.type = "movie";
        item.poster = "https://art.test/poster.jpg";
        BoardRow board; board.items = {item}; board.loaded = true;
        app.board_ = {board}; app.home_map_ = {0};
        UiRow ui; ui.cards.resize(1); app.home_rows = {ui};
        expect(app.visible_art_missing(), "a visible failed poster remains eligible for periodic retry");
        app.home_rows[0].cards[0].image = "ready.rgba";
        expect(!app.visible_art_missing(), "a completed row stops periodic artwork refresh");
        app.home_rows[0].cards[0].image.clear(); app.login_visible = true;
        expect(!app.visible_art_missing(), "login never wakes artwork from a hidden catalog");
        app.login_visible = false; app.watching_ = true;
        expect(!app.visible_art_missing(), "playback never wakes artwork from a hidden catalog");
        app.watching_ = false;
        app.board_[0].items.resize(20); app.home_rows[0].cards.resize(20);
        app.board_[0].items[0].poster.clear(); app.board_[0].items[19].poster = item.poster;
        expect(!app.visible_art_missing(), "offscreen posters do not trigger visible-row retries");

        app.view = "discover"; app.disc_items_ = {item}; app.disc_cards.resize(1);
        app.disc_cards[0].image = "ready.rgba";
        app.disc_items_[0].logo = "https://art.test/logo.png";
        app.disc_items_[0].background = "https://art.test/hero.jpg";
        app.dp_logo.clear(); app.dp_still = "ready-hero.rgba";
        expect(app.visible_art_missing(), "Discover retries a failed title logo even with all posters loaded");
        app.dp_logo = "ready-logo.rgba"; app.dp_still.clear();
        expect(!app.visible_art_missing(), "a missing Discover hero never wakes artwork work for a hidden image");
        app.dp_logo.clear();
        app.disc_items_[0].poster.clear(); // Only the wrapped logo art boundary is needed below.
        app.images_retry_at_ = 0; app.images_refreshed_ = 0;
        app.update();
        expect(app.dp_logo == "https://art.test/logo.png" && app.dp_still.empty() && app.images_refreshed_ > 0,
            "failed Discover artwork recovers through periodic App update without navigation");
        expect(!app.visible_art_missing(), "recovered Discover artwork stops retry work");
        app.dp_logo.clear(); app.update();
        expect(app.dp_logo.empty(), "periodic retry is throttled instead of running every frame");
        app.images_retry_at_ = 0; app.images_refreshed_ = 0; app.update();
        expect(!app.dp_logo.empty() && app.dp_still.empty(), "the next periodic retry recovers the title logo without adding a hero");
    }
};

int main() {
    g_tasks.start(1);
    try {
        AppProtocolTest::load_and_cache();
        AppProtocolTest::stale_selection_and_account();
        AppProtocolTest::discover_details_without_hero();
        AppProtocolTest::provider_bound_and_retry();
        AppProtocolTest::bounded_cache_and_offline();
        AppProtocolTest::visible_art_retry();
        expect(http_calls == 0, "all remote boundaries are controlled; no real HTTP escapes");
        expect(replies.empty(), "every planned response was exercised");
        g_tasks.stop();
        std::cout << "PREVIEW_METADATA_TESTS_OK checks=" << checks << " real_http_requests=" << http_calls << '\n'; return 0;
    } catch (const std::exception& e) {
        for (auto& r : all_replies) r->released = true;
        g_tasks.stop(); std::cerr << "PREVIEW_METADATA_TESTS_FAILED: " << e.what() << '\n'; return 1;
    }
}
