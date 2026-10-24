// Real App playback requests and dropdown input; only external player, artwork
// and HTTP boundaries are replaced. No actual decoder or provider is opened.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include <curl/curl.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0, player_opens = 0, audio_releases = 0;
std::atomic<int> unexpected_http{0}, unexpected_json{0};
Player::Options opened;
void expect(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void until(Predicate condition, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        g_tasks.drain();
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(std::string("Timed out: ") + message);
}
Stream source(const std::string& name = "chosen") {
    Stream value;
    value.name = name; value.kind = StreamKind::Direct;
    value.url = "https://media.invalid/" + name + ".mkv";
    value.request_headers = {"X-Fixture: " + name};
    // Already supplied by the fixture provider: no media hash probe is needed.
    value.video_hash = "0123456789abcdef"; value.has_video_size = true; value.video_size = 1000000;
    return value;
}
}

void wrapped_player_open(Player*, const Player::Options&) asm(WRAP_PLAYER_OPEN);
void wrapped_player_open(Player*, const Player::Options& options) { ++player_opens; opened = options; }
std::string wrapped_art_get(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_GET);
std::string wrapped_art_get(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) { return {}; }
std::string wrapped_art_peek(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_PEEK);
std::string wrapped_art_peek(ArtCache*, const std::string&, ArtKind) { return {}; }
std::string wrapped_art_get_async(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_GET_ASYNC);
std::string wrapped_art_get_async(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) { return {}; }
std::string wrapped_art_peek_cached(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_PEEK_CACHED);
std::string wrapped_art_peek_cached(ArtCache*, const std::string&, ArtKind) { return {}; }
bool wrapped_fetch(const std::string&, json&, std::string&, long, const std::atomic<bool>*) asm(WRAP_FETCH_JSON);
bool wrapped_fetch(const std::string&, json&, std::string& error, long, const std::atomic<bool>*) {
    ++unexpected_json; error = "Unexpected fixture JSON request"; return false;
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void prepare(App& app, const std::string& directory) {
        app.data_dir_ = app.base_dir_ = directory;
        app.offline_ = true; app.login_visible = false;
        app.settings_.ui_language = "it"; app.settings_.ui_language_mode = "it";
        app.settings_.auto_subtitles = false;
        app.view = "detail"; app.zone = "content"; app.d_zone = "streams";
        app.d_item_.id = "fixture:series"; app.d_item_.type = "series"; app.d_item_.name = "Fixture series";
        app.d_meta_loaded_ = true; app.d_series = true;
        app.d_meta_.id = app.d_item_.id; app.d_meta_.type = app.d_item_.type;
        app.d_meta_.name = "Complete fixture title";
        Video first, second;
        first.id = "fixture:episode:1"; first.title = "First episode"; first.season = 1; first.episode = 1;
        second.id = "fixture:episode:2"; second.title = "Second episode"; second.season = 1; second.episode = 2;
        app.d_meta_.videos = {first, second}; app.d_video_id_ = first.id;
        app.before_playback = [] { ++audio_releases; };
        std::string error;
        expect(app.downloads_.init(directory, &error), "download inventory initializes without a provider");
        app.downloads_.set_enabled(false);
    }
    static std::string key(const App& app) {
        return video_progress_key(app.d_item_.type, app.d_item_.id, app.d_video_id_);
    }
    static void bookmark(App& app, double position = 123.5, double duration = 1800) {
        app.video_progress_[key(app)] = {app.d_item_.type, app.d_item_.name, "", app.d_video_id_, position, duration, 1};
    }
    static void stopped(App& app) {
        if (app.watching()) app.watch_stop(false);
        if (app.dd_visible) app.on_button(Btn::Circle);
        app.launch_visible = false;
    }
    static void prompt(App& app, const Stream& stream, bool resume) {
        const int prior_opens = player_opens, prior_audio = audio_releases;
        const auto prior_tasks = g_tasks.pending();
        const int prior_busy = app.busy_count_, prior_generation = app.w_gen_;
        app.play_stream(stream, false);
        expect(app.dd_visible && app.dd_options.size() == 2, "every playable source first opens a two-action choice");
        expect(!app.watching() && !app.launch_visible && !app.w_torrent_slot_ && !app.t_visible,
               "choice does not open playback, a launch overlay, torrent reservation or statistics");
        expect(player_opens == prior_opens && audio_releases == prior_audio && app.busy_count_ == prior_busy &&
               g_tasks.pending() == prior_tasks && app.w_gen_ == prior_generation,
               "waiting for the choice does not release audio, enqueue a resolver or change playback generation");
        expect(app.dd_sel == (resume ? 1 : 0), "valid resume is focused, otherwise beginning is focused");
        expect(app.dd_options[1].label == (resume ? "Riproduci da 2:03" : "Annulla"),
               "choice offers the actual bookmark or Cancel, never a fabricated resume time");
    }
    static void choose(App& app, int choice, double expected_start, const std::string& expected_url) {
        const int before = player_opens;
        app.dd_sel = choice; app.on_button(Btn::Cross);
        until([&] { return player_opens == before + 1; }, "confirmed source reaches Player::open");
        expect(app.watching() && !app.dd_visible && opened.url == expected_url && opened.start == expected_start,
               "the confirmed source opens once at the explicitly selected timestamp");
    }
    static void invalidated(App& app, const std::function<void()>& invalidate, const char* message) {
        bookmark(app); prompt(app, source(), true);
        const int before = player_opens, prior_busy = app.busy_count_;
        invalidate(); app.on_button(Btn::Cross); g_tasks.drain();
        expect(player_opens == before && app.busy_count_ == prior_busy && !app.watching() && !app.launch_visible, message);
    }
    static void run(App& app, const std::string& directory) {
        prepare(app, directory);
        const Stream stream = source();
        prompt(app, stream, false);
        app.dd_sel = 1; app.on_button(Btn::Cross);
        expect(!app.dd_visible && player_opens == 0, "Cancel action on an unwatched source opens nothing");

        prompt(app, stream, false);
        choose(app, 0, 0, stream.url); stopped(app);

        bookmark(app); prompt(app, stream, true);
        expect(app.dd_options[0].label == "Riproduci dall'inizio", "the beginning option is explicit and localized");
        const auto cancelled = app.dd_chosen_;
        app.on_button(Btn::Circle);
        const int before_cancelled = player_opens;
        cancelled(1); g_tasks.drain();
        expect(!app.dd_visible && player_opens == before_cancelled && !app.watching() && !app.launch_visible,
               "Circle and a subsequently delivered stale choice cannot launch a cancelled request");
        expect(app.video_progress_.at(key(app)).time == 123.5, "cancelling leaves the saved position intact");

        Stream torrent = stream;
        torrent.url.clear(); torrent.info_hash = "0123456789abcdef0123456789abcdef01234567";
        prompt(app, torrent, true);
        app.on_button(Btn::Circle);
        expect(!app.w_torrent_slot_ && !app.t_visible && g_tasks.pending() == 0,
               "cancelling a torrent choice never starts metadata resolution or claims the foreground engine");

        prompt(app, stream, true); choose(app, 0, 0, stream.url); stopped(app);
        bookmark(app); prompt(app, stream, true);
        const auto confirmed = app.dd_chosen_;
        choose(app, 1, 123.5, stream.url);
        const int once = player_opens;
        confirmed(1); g_tasks.drain();
        expect(player_opens == once, "a repeated confirmation cannot open a second player");
        stopped(app);

        bookmark(app); Stream mutable_stream = source("captured"); prompt(app, mutable_stream, true);
        const auto original_url = mutable_stream.url;
        const auto original_headers = mutable_stream.request_headers;
        mutable_stream = source("replacement");
        choose(app, 1, 123.5, original_url);
        expect(opened.headers == original_headers, "the selected source and its request headers are captured by value");
        stopped(app);

        invalidated(app, [&] { ++app.account_generation_; }, "account changes invalidate an outstanding playback choice");
        invalidated(app, [&] { ++app.d_meta_gen_; }, "replacement metadata invalidates the old choice");
        invalidated(app, [&] { ++app.d_gen_; }, "a replacement stream request invalidates the old choice");
        invalidated(app, [&] { ++app.w_gen_; }, "a later playback request invalidates the old choice");
        invalidated(app, [&] { app.d_video_id_ = "fixture:episode:2"; }, "a different episode cannot inherit an older choice");

        app.video_progress_.clear();
        app.progress_[app.d_item_.id] = {app.d_item_.type, app.d_item_.name, "", "fixture:episode:1", 99, 1800, 1};
        prompt(app, stream, false); choose(app, 0, 0, stream.url);
        expect(app.w_video_id_ == "fixture:episode:2", "the next episode starts with its own identity and no previous bookmark");
        stopped(app);

        app.progress_.clear(); bookmark(app, std::numeric_limits<double>::quiet_NaN());
        prompt(app, stream, false); app.on_button(Btn::Circle);
        bookmark(app, 1790, 1800);
        prompt(app, stream, false); app.on_button(Btn::Circle);

        app.video_progress_.clear(); app.settings_.auth_key = "FIXTURE_DEVICE_SESSION"; app.library_loaded_ = true;
        app.library_[app.d_item_.id] = {{"type", "series"}, {"name", "Cloud fixture"},
            {"state", {{"video_id", app.d_video_id_}, {"timeOffset", 123500}, {"duration", 1800000}}}};
        prompt(app, stream, true); choose(app, 1, 123.5, stream.url); stopped(app);
        app.settings_.auth_key.clear(); app.library_.clear();

        bookmark(app); app.d_meta_.is_live = true;
        prompt(app, stream, false);
        expect(app.dd_options[0].label == "Avvia diretta", "a live source is identified without promising a seek to its beginning");
        choose(app, 0, 0, stream.url); stopped(app);
        app.d_meta_.is_live = false;

        app.video_progress_.clear(); app.d_meta_.has_scheduled_videos = true;
        prompt(app, stream, false);
        expect(app.dd_options[0].label == "Riproduci dall'inizio", "scheduled on-demand episodes are not classified as live");
        app.on_button(Btn::Circle);

        bookmark(app); prompt(app, stream, true);
        const auto superseded = app.dd_chosen_;
        app.play_stream(source("new-choice"), false);
        const int before_stale = player_opens;
        superseded(1); g_tasks.drain();
        expect(player_opens == before_stale && !app.launch_visible && app.dd_visible,
               "superseding the dialog invalidates its previous captured callback");
        choose(app, 1, 123.5, source("new-choice").url); stopped(app);

        expect(unexpected_http == 0 && unexpected_json == 0, "source choice and fixture playback require no external HTTP requests");
        app.before_playback = {}; app.shutdown();
    }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    App app;
    try {
        g_tasks.start(2);
        AppProtocolTest::run(app, argv[1]);
        g_tasks.stop();
        std::cout << "PASS: " << checks << " real App playback-choice assertions; decoder and HTTP boundaries controlled\n";
        return 0;
    } catch (const std::exception& error) {
        app.before_playback = {};
        app.shutdown();
        g_tasks.stop();
        std::cerr << "FAIL after " << checks << " assertions: " << error.what() << '\n';
        return 1;
    }
}
