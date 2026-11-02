// Production seek dispatch/reopen bodies; VLC lifecycle is a controlled boundary.
#include "player_seek.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

static unsigned checks, stops, releases, direct_seeks;
static bool media_seekable = true;
static int64_t last_direct;
static std::atomic<int64_t> clock_now{0};
static std::function<void()> stopping;
static void check(bool value, const char* message) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using libvlc_time_t = int64_t;
static int64_t libvlc_clock() { return clock_now; }
static void libvlc_media_player_stop(void*) { ++stops; if (stopping) stopping(); }
static void libvlc_media_release(void*) { ++releases; }
static bool libvlc_media_player_is_seekable(void*) { return media_seekable; }
static void libvlc_media_player_set_time(void*, libvlc_time_t time) { ++direct_seeks; last_direct = time; }
template<class... Args> static void dlog(const char*, Args...) {}
struct Player { enum class State { Playing, Opening, Ended }; };
struct TorrentStream { static bool is_url(const std::string& url) { return url.rfind("torrent://", 0) == 0; } };
struct Fixture {
    using State = Player::State;
    struct Parent {
        Player::State state_ = Player::State::Playing;
        bool paused = true;
        int audio_track = 4, subtitle_track = 7;
    } parent;
    struct Input {
        struct Options {
            bool growing_download = false;
            std::string url = "file:///held/movie.mkv", download_id = "same-download";
        } options;
        std::atomic<const char*> cancel_reason{"none"};
        std::atomic<bool> abort{false};
    } input;
    playback_seek::Requests seek_requests;
    std::atomic<uint64_t> active_seek_generation{0};
    std::atomic<bool> stop_requested{false}, end_reached{false}, seeking{false}, seek_time_seen{false};
    std::atomic<bool> native_position_known{true};
    std::atomic<double> presented{100}, seek_target{0};
    double seek_progress = 0, seek_previous_position = -1;
    void* player = this;
    void* media = this;
    unsigned clears = 0;
    std::vector<double> reopened;
    Fixture() { stops = releases = direct_seeks = 0; stopping = {}; media_seekable = true; }
    void clear_output() { ++clears; }
    bool start_media(double target) { reopened.push_back(target); return true; }
#include "seek_reopen_body.inc"
    void dispatch_once() {
        do {
#include "seek_dispatch_body.inc"
        } while (false);
    }
};
int main() {
    {
        playback_seek::Requests queue;
        queue.request(90, 0);
        check(queue.position(100) == 90 && !queue.take_ready(299999), "single request previews immediately and waits only300ms");
        queue.request(80, 250000); queue.request(70, 500000);
        check(!queue.take_ready(799999), "controller repeats coalesce through the final quiet period");
        const auto latest = queue.take_ready(800000);
        check(latest && latest->seconds == 70 && !queue.pending(), "one settled request retains only the latest target");
        queue.request(60, 810000);
        queue.displayed(latest->generation);
        check(queue.position(71) == 60, "old decoded picture cannot erase a newer seek preview");
    }
    {
        Fixture fixture;
        fixture.seek_requests.request(42, 1000000); clock_now = 1299999;
        fixture.dispatch_once();
        check(!stops && !direct_seeks, "debounce never sends intermediate VLC controls");
        clock_now = 1300000; fixture.dispatch_once();
        check(direct_seeks == 1 && last_direct == 42000 && !stops && fixture.reopened.empty(),
              "ordinary local file keeps the direct seek path");
        fixture.seek_requests.displayed(fixture.active_seek_generation);
        check(fixture.seek_requests.position(42.25) == 42.25, "fresh direct-seek output releases target preview");
    }
    {
        Fixture fixture; media_seekable = false;
        fixture.seek_requests.request(42, 0); clock_now = 300000;
        fixture.dispatch_once();
        check(!stops && !direct_seeks && fixture.seek_requests.position(100) == 100,
              "a non-seekable direct source releases the rejected preview");
    }
    for (bool growing : {false, true}) {
        Fixture fixture;
        fixture.input.options.url = growing ? "/held/download.part" : "torrent://same-infohash/4";
        fixture.input.options.growing_download = growing;
        const auto source = fixture.input.options.url;
        fixture.seek_requests.request(90, 0); clock_now = 300000;
        stopping = [&] {
            check(fixture.input.abort && fixture.parent.state_ == Player::State::Opening,
                  "custom source is cancelled before native stop can wait on old demand");
            check(fixture.reopened.empty(), "old input is stopped before another input opens");
            // These requests arrive inside native stop/audio drain, not before
            // dispatch. The production helper must supersede its original90s.
            fixture.seek_requests.request(80, 400000);
            fixture.seek_requests.request(70, 500000);
            check(fixture.seek_requests.position(100) == 70, "bar remains responsive while old input is draining");
            clock_now = 800000;
        };
        fixture.dispatch_once();
        check(stops == 1 && releases == 1 && direct_seeks == 0 && fixture.reopened == std::vector<double>{70},
              "torrent/growing input reopens once at the newest target received during stop");
        check(!fixture.input.abort && fixture.clears == 1 && fixture.active_seek_generation == 3,
              "new decoder generation starts only after obsolete output is cleared");
        check(fixture.input.options.url == source && fixture.input.options.download_id == "same-download" &&
              fixture.parent.paused && fixture.parent.audio_track == 4 && fixture.parent.subtitle_track == 7,
              "reopen handoff preserves source/download identity, pause and track selections");
        fixture.seek_requests.displayed(1);
        check(fixture.seek_requests.position(90) == 70, "stale seek completion cannot replace latest preview");
        fixture.seek_requests.displayed(fixture.active_seek_generation);
        check(fixture.seek_requests.position(70.1) == 70.1, "only the newest displayed generation completes the seek");
    }
    {
        Fixture fixture; fixture.input.options.url = "torrent://same-infohash/4";
        fixture.seek_requests.request(50, 0); clock_now = 300000;
        stopping = [&] { fixture.stop_requested = true; };
        fixture.dispatch_once();
        check(stops == 1 && fixture.reopened.empty() && fixture.input.abort,
              "closing playback during seek drain cannot reopen the source");
    }
    std::printf("{\"result\":\"PASS\",\"checks\":%u}\n", checks);
}
