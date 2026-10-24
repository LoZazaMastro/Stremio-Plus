// Exercise real App end-of-episode ordering, one-shot countdown and input.
// Network, artwork and decoder boundaries are controlled; no console required.
#include "app.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0, closes = 0, saves = 0, stream_requests = 0, local_plays = 0;
std::vector<DownloadEntry> inventory;
double monotonic_time = 100;
std::string requested, local_played;
ArtPriority requested_priority = ArtPriority::Prefetch;
bool art_ready = true;
void expect(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
Video video(const std::string& id, int season, int episode) {
    Video v; v.id = id; v.title = "Title " + id; v.season = season; v.episode = episode;
    v.thumbnail = "https://fixture.invalid/" + id + ".jpg";
    v.raw = {{"season", season}};
    return v;
}
}

double wrapped_now() asm(WRAP_NOW);
double wrapped_now() { return monotonic_time; }
void wrapped_close(Player*) asm(WRAP_PLAYER_CLOSE);
void wrapped_close(Player*) { ++closes; }
void wrapped_save(App*) asm(WRAP_SAVE_PROGRESS);
void wrapped_save(App*) { ++saves; }
void wrapped_streams(App*, const std::string&) asm(WRAP_DETAIL_STREAMS);
void wrapped_streams(App*, const std::string& id) { ++stream_requests; requested = id; }
std::vector<DownloadEntry> wrapped_snapshot(const DownloadManager*) asm(WRAP_DOWNLOAD_SNAPSHOT);
std::vector<DownloadEntry> wrapped_snapshot(const DownloadManager*) { return inventory; }
std::optional<DownloadEntry> wrapped_find(const DownloadManager*, const std::string&) asm(WRAP_DOWNLOAD_FIND);
std::optional<DownloadEntry> wrapped_find(const DownloadManager*, const std::string& id) {
    for (const auto& entry : inventory) if (entry.id == id) return entry;
    return {};
}
void wrapped_open(Player*, const Player::Options&) asm(WRAP_PLAYER_OPEN);
void wrapped_open(Player*, const Player::Options& options) {
    expect(!options.growing_download && options.start == 0, "confirmed next local video starts complete and from its beginning");
    ++local_plays; local_played = options.url;
}
std::string wrapped_get(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_GET);
std::string wrapped_get(ArtCache*, const std::string& url, ArtKind,
    std::function<void(const std::string&)>, ArtPriority priority, const void*) {
    requested_priority = priority;
    return art_ready && !url.empty() ? "fixture-art.rgba" : "";
}
std::string wrapped_async(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_ASYNC);
std::string wrapped_async(ArtCache* a, const std::string& url, ArtKind kind,
    std::function<void(const std::string&)> done, ArtPriority priority, const void* observer) {
    return wrapped_get(a, url, kind, std::move(done), priority, observer);
}
std::string wrapped_peek(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_PEEK);
std::string wrapped_peek(ArtCache*, const std::string& url, ArtKind) { return art_ready && !url.empty() ? "fixture-art.rgba" : ""; }
std::string wrapped_cached(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_CACHED);
std::string wrapped_cached(ArtCache* a, const std::string& url, ArtKind kind) { return wrapped_peek(a, url, kind); }

struct AppProtocolTest {
    static void prepare(App& app, const std::string& current = "s1e1") {
        closes = saves = stream_requests = local_plays = 0; inventory.clear();
        requested.clear(); local_played.clear(); monotonic_time = 100; art_ready = true;
        app.offline_ = true; app.settings_.autoplay_next = true; app.settings_.next_episode_delay_seconds = 15;
        app.view = "detail"; app.zone = "content"; app.d_series = true; app.d_meta_loaded_ = true;
        app.d_item_.id = "series"; app.d_item_.type = "series"; app.d_item_.name = "Fixture series";
        app.w_item_ = app.d_item_; app.d_meta_.id = "series"; app.d_meta_.type = "series";
        // Unsorted metadata, duplicate identity and specials verify carousel ordering.
        app.d_meta_.videos = {video("s2e2", 2, 2), video("s1e2", 1, 2), video("special", 0, 1),
                             video("s2e1", 2, 1), video("s1e1", 1, 1), video("s1e2", 1, 2)};
        app.d_seasons_ = {1, 2, 0}; app.detail_set_season(current.starts_with("s2") ? 1 : 0);
        app.w_video_id_ = current; app.watching_ = true; app.w_started_ = true;
        app.w_ended_handled_ = false; app.w_offline_ = false;
        app.w_stream_.binge_group = "same-binge"; app.w_stream_.addon_url = "https://provider.invalid/installation";
        app.next_episode_visible = false; app.watched_.clear(); app.autoplay_pending_ = false;
        app.dd_visible = false; app.dd_chosen_ = nullptr; app.launch_visible = false;
        app.progress_.clear(); app.video_progress_.clear();
    }
    static void run(const std::string& directory) {
        App app; prepare(app);
        app.base_dir_ = app.data_dir_ = directory;
        expect(app.settings_.next_episode_delay_seconds == 15, "default countdown is fifteen seconds");
        const int old_season = app.d_season_idx_, old_selection = app.d_episode_sel;
        art_ready = false; app.watch_finish_episode();
        expect(app.next_episode_visible && app.watching_ && closes == 0, "end card retains player and last frame");
        expect(app.next_episode_id_ == "s1e2" && app.next_episode_title == "Title s1e2" &&
               app.next_episode_label == "S1 · Episode 2", "next card identifies exact next episode");
        expect(app.next_episode_seconds == 15 && app.next_episode_sel == 0, "watch now is left default selection");
        expect(!app.info_visible && !app.menu_visible && !app.w_buffering, "only next card overlays ended video");
        expect(app.watched_.contains("s1e1") && saves == 1, "ended episode is marked watched exactly once");
        expect(requested_priority == ArtPriority::Immediate, "next thumbnail receives immediate artwork priority");
        expect(app.d_season_idx_ == old_season && app.d_episode_sel == old_selection && stream_requests == 0,
               "offering does not mutate season selection or fetch next streams");
        app.watch_finish_episode(); app.watch_update();
        expect(saves == 1 && closes == 0 && stream_requests == 0, "repeated Ended updates cannot reopen or save card twice");
        monotonic_time = 100.01; app.watch_next_episode_tick();
        expect(app.next_episode_seconds == 15, "countdown rounds remaining partial second upwards");
        monotonic_time = 101.01; art_ready = true; app.watch_next_episode_tick();
        expect(app.next_episode_seconds == 14 && app.next_episode_thumb == "fixture-art.rgba", "timer and delayed art refresh without callback race");
        monotonic_time = 114.99; app.watch_next_episode_tick();
        expect(stream_requests == 0 && app.next_episode_seconds == 1, "countdown never starts early");
        monotonic_time = 115; app.watch_next_episode_tick(); app.watch_next_episode_tick();
        expect(stream_requests == 1 && requested == "s1e2" && closes == 1 && saves == 1,
               "zero commits exactly one next episode and closes ended player once");
        expect(!app.next_episode_visible && app.autoplay_pending_ && app.autoplay_binge_ == "same-binge" &&
               app.autoplay_addon_ == "https://provider.invalid/installation", "commit preserves binge and installed-provider identity");

        prepare(app); app.watch_finish_episode(); app.on_button(Btn::Circle);
        monotonic_time = 999; app.watch_next_episode_tick(); app.watch_finish_episode();
        expect(!app.next_episode_visible && !app.watching_ && app.view == "detail" &&
               stream_requests == 0 && closes == 1 && saves == 1, "Circle ignores and cancels deadline permanently");
        prepare(app); app.watch_finish_episode(); app.on_button(Btn::Right);
        expect(app.next_episode_sel == 1, "Right focuses Ignore");
        app.on_button(Btn::Cross);
        expect(stream_requests == 0 && !app.next_episode_visible, "Cross on Ignore never starts next");
        prepare(app); app.watch_finish_episode(); app.on_button(Btn::Right); app.on_button(Btn::Left); app.on_button(Btn::Cross);
        expect(stream_requests == 1 && requested == "s1e2", "Left and Cross start Watch now immediately");

        prepare(app, "s1e2"); app.watch_finish_episode();
        expect(app.next_episode_id_ == "s2e1" && app.d_season_idx_ == 0, "offer advances sorted seasons without changing background selection");
        app.on_button(Btn::Cross);
        expect(requested == "s2e1" && app.d_season_idx_ == 1 && app.d_episode_sel == 0,
               "commit selects first episode of next regular season");
        prepare(app, "s2e2"); app.watch_finish_episode();
        expect(!app.next_episode_visible && closes == 1 && stream_requests == 0, "last regular episode does not roll into specials");
        prepare(app, "special"); app.watch_finish_episode();
        expect(!app.next_episode_visible, "last special never rolls into regular seasons");
        prepare(app); app.settings_.autoplay_next = false; app.watch_finish_episode();
        expect(!app.next_episode_visible && !app.watching_, "disabled autoplay returns to details without a card");
        prepare(app); app.w_offline_ = true; app.watch_finish_episode();
        expect(!app.next_episode_visible, "offline playback never starts a provider stream automatically");
        prepare(app); app.d_series = false; app.w_item_.type = "movie"; app.watch_finish_episode();
        expect(!app.next_episode_visible, "films never offer a next episode");
        prepare(app); app.settings_.next_episode_delay_seconds = 2000; app.watch_finish_episode();
        expect(app.next_episode_seconds == 120, "persisted invalid high delay is bounded");
        prepare(app); app.settings_.next_episode_delay_seconds = -20; app.watch_finish_episode();
        expect(app.next_episode_seconds == 5, "persisted invalid low delay is bounded");
        prepare(app); app.watch_finish_episode(); ++app.account_generation_; monotonic_time = 120; app.watch_next_episode_tick();
        expect(!app.next_episode_visible && stream_requests == 0, "account changes invalidate captured episode");
        prepare(app); app.watch_finish_episode(); app.directory_picker_committing = true;
        monotonic_time = 120; app.watch_next_episode_tick();
        expect(!app.next_episode_visible && !app.watching_ && stream_requests == 0,
               "relocation cancels an expired next-episode countdown before starting a new player");
        app.directory_picker_committing = false;
        prepare(app); app.watch_finish_episode(); app.directory_picker_committing = true;
        app.watch_next_episode();
        expect(!app.next_episode_visible && stream_requests == 0,
               "Watch now cannot race an accepted folder relocation");
        app.directory_picker_committing = false;
        prepare(app); app.watch_finish_episode(); app.d_meta_.videos.clear(); app.on_button(Btn::Cross);
        expect(stream_requests == 0 && !app.next_episode_visible, "missing replacement metadata cannot play stale episode identity");

        prepare(app); app.watch_finish_episode(); app.on_button(Btn::Cross);
        Stream unrelated, provider, binge;
        unrelated.url = "https://media.invalid/first"; unrelated.addon_url = "different";
        provider.url = "https://media.invalid/same-provider"; provider.addon_url = app.autoplay_addon_;
        binge.url = "https://media.invalid/same-binge"; binge.binge_group = "same-binge";
        app.d_stream_list_ = {unrelated, provider, binge};
        app.d_stream_buckets_ = {app.d_stream_list_}; app.d_pending_ = 1; app.d_answered_ = 1;
        app.detail_refresh_streams();
        expect(app.dd_visible && !app.launch_visible && !app.watching(),
               "the automatic next source asks before launching playback");
        const int launch_generation = app.w_gen_;
        app.detail_refresh_streams();
        expect(app.dd_visible && app.w_gen_ == launch_generation && !app.autoplay_pending_,
               "repeated source refresh does not replace or bypass the next-episode choice");
        app.on_button(Btn::Cross);
        expect(app.w_stream_.url == binge.url && app.launch_visible && app.w_gen_ == launch_generation + 1,
               "confirming the choice launches the matching binge source exactly once");
        g_tasks.stop();
        prepare(app); app.w_offline_ = true; app.view = "downloads";
        app.d_item_.id = "unrelated"; app.w_download_id_ = "local-one";
        DownloadEntry one, two, three;
        one.id = "local-one"; one.media_id = "series"; one.type = "series"; one.video_id = "s1e1";
        one.season = 1; one.episode = 1; one.state = DownloadState::Complete;
        one.local_path = directory + "/first.mkv";
        std::ofstream(one.local_path) << "local fixture";
        two = one; two.id = "local-two"; two.video_id = "s1e2"; two.episode = 2;
        two.local_path = directory + "/second.mkv";
        std::ofstream(two.local_path) << "local fixture";
        two.subtitle = "S1E2 · Local next title"; two.background_path = "local-next.rgba";
        three = two; three.id = "local-three"; three.video_id = "s1e3"; three.episode = 3;
        inventory = {one, three, two};
        app.watch_finish_episode();
        expect(app.next_episode_visible && app.next_episode_title == "Local next title" &&
               app.next_episode_thumb == "local-next.rgba", "completed offline episode offers exact adjacent downloaded video with local artwork");
        app.on_button(Btn::Cross);
        expect(app.dd_visible && local_plays == 0 && !app.watching(),
               "offline next asks before opening its local player");
        app.on_button(Btn::Cross);
        expect(local_plays == 1 && app.w_download_id_ == "local-two" && local_played == two.local_path && stream_requests == 0,
               "offline next starts existing local player path without any provider request");
        prepare(app, "s1e2"); app.w_offline_ = true; app.view = "downloads"; app.w_download_id_ = "local-two";
        DownloadEntry next_season = two; next_season.id = "local-next-season";
        next_season.video_id = "s2e1"; next_season.season = 2; next_season.episode = 1;
        inventory = {two, next_season}; app.watch_finish_episode();
        expect(app.next_episode_visible && app.next_episode_id_ == "s2e1",
               "complete cached metadata can establish the exact offline season boundary");
        app.on_button(Btn::Cross);
        expect(app.dd_visible && local_plays == 0, "a new offline season also requires the playback choice");
        app.on_button(Btn::Cross);
        expect(app.w_download_id_ == "local-next-season" && local_played == next_season.local_path && stream_requests == 0,
               "known offline season boundary stays entirely local");
        prepare(app, "s1e2"); app.w_offline_ = true; app.view = "downloads";
        app.d_item_.id = "unrelated"; app.w_download_id_ = "local-two";
        inventory = {two, next_season}; app.watch_finish_episode();
        expect(!app.next_episode_visible, "missing metadata never guesses a season boundary from downloaded inventory");
        prepare(app); app.w_offline_ = true; app.view = "downloads";
        app.d_item_.id = "unrelated"; app.w_download_id_ = "local-one"; inventory = {one, three};
        app.watch_finish_episode();
        expect(!app.next_episode_visible && local_plays == 0, "offline next never skips a missing adjacent episode");
        prepare(app); app.w_offline_ = true; app.view = "downloads";
        app.d_item_.id = "unrelated"; app.w_download_id_ = "local-one";
        two.state = DownloadState::Paused; inventory = {one, two}; app.watch_finish_episode();
        expect(!app.next_episode_visible, "offline next refuses incomplete or paused files");
        prepare(app); app.w_offline_ = true; app.view = "downloads";
        app.d_item_.id = "unrelated"; app.w_download_id_ = "local-one";
        two.state = DownloadState::Complete; inventory = {one, two}; app.watch_finish_episode();
        inventory.pop_back(); monotonic_time = 115; app.watch_next_episode_tick();
        expect(local_plays == 0 && stream_requests == 0, "removed next download never falls back to online provider");
        app.watching_ = false;
    }
};
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { AppProtocolTest::run(argv[1]); std::cout << "Next episode: " << checks << " checks passed\n"; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
