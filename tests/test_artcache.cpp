#include "artcache.h"
#include "http.h"
#include "tasks.h"
#include "util.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

using namespace std::chrono_literals;
namespace {
int checks = 0;
std::mutex server_mutex;
std::condition_variable server_wake;
std::map<std::string, int> calls;
std::vector<std::string> requested;
bool gate_closed = false;
const std::thread::id ui_thread = std::this_thread::get_id();
bool wrong_http_thread = false;
std::atomic<bool> track_controller_storage{false};
std::atomic<unsigned> controller_storage_calls{0};

void expect(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<typename Predicate> void until(Predicate done, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
        g_tasks.drain();
        if (done()) return;
        std::this_thread::sleep_for(3ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(message);
}
int request_count(const std::string& name) {
    std::lock_guard<std::mutex> lock(server_mutex);
    return calls["https://art.example/" + name];
}
std::string fixture_image() {
    // A tiny uncompressed TGA exercises stb_image without external assets.
    unsigned char data[30]{};
    data[2] = 2; data[12] = 2; data[14] = 2; data[16] = 24; data[17] = 32;
    for (size_t i = 18; i < sizeof(data); i += 3) { data[i] = 40; data[i + 1] = 80; data[i + 2] = 200; }
    return std::string(reinterpret_cast<char*>(data), sizeof(data));
}
std::string special_image(const std::string& url) {
    const bool hero = ends_with(url, "sharp-hero");
    const bool empty = ends_with(url, "empty-logo");
    const int w = hero ? 16 : 12, h = 8;
    std::string data(18 + w * h * 4, '\0');
    data[2] = 2; data[12] = char(w); data[14] = char(h); data[16] = 32; data[17] = 40;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        auto* pixel = reinterpret_cast<unsigned char*>(&data[18 + (y * w + x) * 4]);
        if (hero) { pixel[0] = pixel[1] = pixel[2] = ((x + y) % 2) ? 255 : 0; pixel[3] = 255; }
        else if (!empty && x >= 4 && x < 8 && y >= 3 && y < 5) {
            pixel[0] = 30; pixel[1] = 80; pixel[2] = 200; pixel[3] = 255;
        }
    }
    return data;
}
std::string detailed_poster_image() {
    const int w = 780, h = 1170;
    std::string data(18 + size_t(w) * h * 4, '\0');
    data[2] = 2; data[12] = char(w & 255); data[13] = char(w >> 8);
    data[14] = char(h & 255); data[15] = char(h >> 8); data[16] = 32; data[17] = 40;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        auto* pixel = reinterpret_cast<unsigned char*>(&data[18 + (size_t(y) * w + x) * 4]);
        pixel[0] = pixel[1] = pixel[2] = (x / 8) % 2 ? 255 : 0;
        pixel[3] = 255;
    }
    return data;
}
struct Cache {
    ArtCache art;
    Cache(const std::string& dir, int workers = 1) { art.start(dir, workers); }
    ~Cache() { art.stop(); g_tasks.drain(); }
};
void queue_priority_and_observers(const std::string& dir) {
    Cache cache(dir);
    gate_closed = true;
    int completed = 0, hero_observer = 0, other_observer = 0;
    bool wrong_callback_thread = false;
    auto done = [&](const std::string& path) {
        if (std::this_thread::get_id() != ui_thread) wrong_callback_thread = true;
        if (!path.empty()) ++completed;
    };
    cache.art.get("https://art.example/gate-first", ArtKind::Logo, done);
    until([&] { return request_count("gate-first") == 1; }, "first worker did not reach the deterministic gate");
    cache.art.get("https://art.example/poster-first", ArtKind::Poster, done);
    cache.art.get("https://art.example/poster-second", ArtKind::Poster, done);
    cache.art.get("https://art.example/hero", ArtKind::Background, done, ArtPriority::Visible, &hero_observer);
    cache.art.get("https://art.example/title-logo", ArtKind::LogoBox, done);
    cache.art.get("https://art.example/adjacent-prefetch", ArtKind::LogoBox, done, ArtPriority::Prefetch);
    // One thousand selected-item refreshes must remain one subscription.
    for (int i = 0; i < 1000; ++i)
        cache.art.get("https://art.example/hero", ArtKind::Background, [&](const std::string& path) {
            if (!path.empty()) { ++hero_observer; ++completed; }
        }, ArtPriority::Visible, &hero_observer);
    cache.art.get("https://art.example/hero", ArtKind::Background, [&](const std::string& path) {
        if (!path.empty()) { ++other_observer; ++completed; }
    }, ArtPriority::Visible, &other_observer);
    {
        std::lock_guard<std::mutex> lock(server_mutex); gate_closed = false;
    }
    server_wake.notify_all();
    until([&] { return completed == 7; }, "art callbacks did not complete");
    expect(hero_observer == 1 && other_observer == 1, "per-frame requests coalesce only the same observer");
    expect(request_count("hero") == 1, "duplicate requests share one transfer");
    expect(!wrong_callback_thread && !wrong_http_thread, "HTTP stays off UI thread; completion callbacks return to UI thread");
    std::vector<std::string> order;
    { std::lock_guard<std::mutex> lock(server_mutex); order = requested; }
    expect(order.size() == 6, "six distinct artwork URLs fetch once each");
    expect(ends_with(order[1], "title-logo") && ends_with(order[2], "hero"), "visible title and hero precede queued poster/prefetch work");
    expect(ends_with(order[3], "poster-first") && ends_with(order[4], "poster-second"), "posters load in row order rather than reversed LIFO");
    expect(ends_with(order[5], "adjacent-prefetch"), "adjacent prefetch stays behind visible images");
    const auto path = cache.art.peek("https://art.example/hero", ArtKind::Background);
    expect(!path.empty() && file_exists(path), "completed image is immediately visible to cache peek");
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
    expect(load_image_rgba(path, pixels, w, h) && w == 1280 && h == 720, "native RGBA cache can be loaded directly at its expected dimensions");
    expect(pixels.size() == size_t(w) * h * 4 && pixels[0] == 200 && pixels[1] == 80 && pixels[2] == 40 && pixels[3] == 255,
           "direct disk loading preserves actual pixels");
    bool cache_callback = false;
    expect(cache.art.get("https://art.example/hero", ArtKind::Background, [&](auto&) { cache_callback = true; }) == path,
           "cache hits return a local path immediately");
    g_tasks.drain();
    expect(!cache_callback && request_count("hero") == 1, "cache hit does not redownload or enqueue a redundant callback");
    cache.art.stop();
    cache.art.start(dir, 1);
    expect(cache.art.get("https://art.example/hero", ArtKind::Background, {}) == path && request_count("hero") == 1,
           "disk cache survives worker stop/restart");
}
void cross_size_and_queue_cancel(const std::string& dir) {
    Cache cache(dir, 2);
    int done = 0;
    std::string small, large;
    cache.art.get("https://art.example/same-origin", ArtKind::Background, [&](const std::string& path) { small = path; ++done; });
    cache.art.get("https://art.example/same-origin", ArtKind::Backdrop, [&](const std::string& path) { large = path; ++done; });
    until([&] { return done == 2; }, "shared origin variants did not complete");
    expect(!small.empty() && !large.empty() && small != large, "one origin produces independent native-size derivatives");
    expect(request_count("same-origin") == 1, "concurrent Background/Backdrop requests reuse one origin download");
    expect(cache.art.peek("https://art.example/miss", ArtKind::Thumb).empty(), "disk miss is reported without network");
    cache.art.get("https://art.example/miss", ArtKind::Thumb, [&](const std::string& path) { if (!path.empty()) ++done; });
    until([&] { return done == 3; }, "peek miss prevented later demand loading");
    expect(request_count("miss") == 1, "remembered disk misses still fetch immediately when visible");

    cache.art.stop(); cache.art.start(dir, 1);
    { std::lock_guard<std::mutex> lock(server_mutex); gate_closed = true; }
    cache.art.get("https://art.example/gate-clear", ArtKind::Logo, [&](const std::string& path) { if (!path.empty()) ++done; });
    until([&] { return request_count("gate-clear") == 1; }, "clear queue gate unavailable");
    cache.art.get("https://art.example/cancel-queued", ArtKind::Poster, {});
    cache.art.clear_queue();
    { std::lock_guard<std::mutex> lock(server_mutex); gate_closed = false; }
    server_wake.notify_all();
    until([&] { return done == 4; }, "active work should survive clearing only queued downloads");
    expect(request_count("cancel-queued") == 0, "leaving a page discards unstarted artwork downloads");
}
void controller_cache_lookup(const std::string& dir) {
    Cache cache(dir);
    std::string original;
    cache.art.get("https://art.example/controller-cover", ArtKind::Poster,
                  [&](const std::string& path) { original = path; });
    until([&] { return !original.empty(); }, "controller cache fixture was not downloaded");
    cache.art.stop(); cache.art.start(dir, 1);
    std::string recovered;
    controller_storage_calls = 0; track_controller_storage = true;
    expect(cache.art.peek_cached("https://art.example/controller-cover", ArtKind::Poster).empty(),
           "memory-only lookup does not probe an undiscovered disk entry after restart");
    const auto immediate = cache.art.get_async("https://art.example/controller-cover", ArtKind::Poster,
        [&](const std::string& path) { recovered = path; });
    for (int i = 0; i < 1000; ++i) {
        cache.art.peek_cached("https://art.example/controller-cover", ArtKind::Poster);
        cache.art.peek_cached("https://art.example/controller-missing", ArtKind::PosterLarge);
    }
    track_controller_storage = false;
    expect(immediate.empty() && controller_storage_calls == 0,
           "controller capture and deferred disk discovery perform zero UI filesystem probes");
    until([&] { return !recovered.empty(); }, "artwork worker did not recover the cached poster");
    expect(recovered == original && request_count("controller-cover") == 1,
           "asynchronous disk discovery reuses existing cover bytes without another HTTP request");
    expect(cache.art.peek_cached("https://art.example/controller-cover", ArtKind::Poster) == original,
           "discovered artwork becomes immediately available to memory-only capture");
    recovered.clear();
    controller_storage_calls = 0; track_controller_storage = true;
    cache.art.get_async("https://art.example/controller-missing", ArtKind::PosterLarge,
        [&](const std::string& path) { recovered = path; });
    track_controller_storage = false;
    expect(controller_storage_calls == 0, "missing cover submission also leaves disk probing off the controller thread");
    until([&] { return !recovered.empty(); }, "missing cover was not fetched by its artwork worker");
    expect(request_count("controller-missing") == 1 && !wrong_http_thread,
           "uncached posters still recover through one background network request");
}
void transient_failures(const std::string& dir) {
    Cache cache(dir, 2);
    int failed = 0, recovered = 0;
    for (const char* name : {"fails-once", "bad-body-once"})
        cache.art.get(std::string("https://art.example/") + name, ArtKind::Logo, [&](const std::string& path) { if (path.empty()) ++failed; });
    until([&] { return failed == 2; }, "failure callbacks missing");
    for (int i = 0; i < 100; ++i)
        cache.art.get("https://art.example/fails-once", ArtKind::Logo, {});
    expect(request_count("fails-once") == 1, "rapid refreshes respect network failure cooldown");
    std::this_thread::sleep_for(5100ms);
    for (const char* name : {"fails-once", "bad-body-once"})
        cache.art.get(std::string("https://art.example/") + name, ArtKind::Logo, [&](const std::string& path) { if (!path.empty()) ++recovered; });
    until([&] { return recovered == 2; }, "transient network/decode failure did not recover without restart");
    expect(request_count("fails-once") == 2, "network retry refetches after cooldown");
    expect(request_count("bad-body-once") == 2, "HTTP-200 error body is evicted from compressed cache before retry");
    expect(!cache.art.peek("https://art.example/fails-once", ArtKind::Logo).empty(), "successful retry becomes a normal cache hit");

    { std::lock_guard<std::mutex> lock(server_mutex); gate_closed = true; }
    cache.art.get("https://art.example/gate-stop", ArtKind::Logo, {});
    until([&] { return request_count("gate-stop") == 1; }, "stop gate unavailable");
    const auto start = std::chrono::steady_clock::now();
    cache.art.stop();
    expect(std::chrono::steady_clock::now() - start < 1s, "stop cancels active HTTP rather than waiting out its timeout");
}
void bounded_queue(const std::string& dir) {
    Cache cache(dir);
    { std::lock_guard<std::mutex> lock(server_mutex); gate_closed = true; }
    int notified = 0;
    cache.art.get("https://art.example/gate-budget", ArtKind::Logo, [&](auto&) { ++notified; });
    until([&] { return request_count("gate-budget") == 1; }, "queue budget gate unavailable");
    for (int i = 0; i < 1000; ++i)
        cache.art.get("https://art.example/scroll-" + std::to_string(i), ArtKind::Thumb,
                      [&](auto&) { ++notified; }, ArtPriority::Prefetch);
    { std::lock_guard<std::mutex> lock(server_mutex); gate_closed = false; }
    server_wake.notify_all();
    until([&] { return notified == 1001; }, "bounded queue did not settle all accepted/dropped observers");
    int transfers = 0;
    { std::lock_guard<std::mutex> lock(server_mutex);
      for (const auto& request : calls) if (request.first.find("/scroll-") != std::string::npos) transfers += request.second; }
    expect(transfers <= 192, "rapid scrolling cannot accumulate unbounded queued downloads");
}
void malformed_rgba(const std::string& dir) {
    make_dirs(dir);
    const auto path = dir + "/broken.rgba";
    std::string data("RGBA", 4);
    uint32_t dimensions[2] = {2, 2};
    data.append(reinterpret_cast<const char*>(dimensions), 8);
    data.append(4, '\0');
    expect(write_file(path, data), "malformed cache fixture written");
    std::vector<unsigned char> pixels(100, 9);
    int w = 44, h = 55;
    expect(!load_image_rgba(path, pixels, w, h) && pixels.empty() && w == 0 && h == 0,
           "truncated cache entries cannot leave stale pixel data or dimensions");
}
void launch_logo_and_sharp_hero(const std::string& dir) {
    Cache cache(dir);
    std::string logo, hero;
    int completed = 0;
    cache.art.get("https://art.example/padded-logo", ArtKind::LaunchLogo,
        [&](const std::string& path) { logo = path; ++completed; });
    cache.art.get("https://art.example/sharp-hero", ArtKind::Backdrop,
        [&](const std::string& path) { hero = path; ++completed; });
    until([&] { return completed == 2; }, "launch artwork did not complete");
    int w = 0, h = 0; std::vector<unsigned char> pixels;
    expect(load_image_rgba(logo, pixels, w, h) && w == 1536 && h == 768,
        "launch logo uses visible 2:1 artwork bounds rather than transparent source padding");
    expect(ends_with(logo, "ll.rgba"), "large launch logo has an independent cache key");
    bool visible = true;
    for (size_t i = 0; i < pixels.size(); i += 4)
        visible &= pixels[i] == 200 && pixels[i + 1] == 80 && pixels[i + 2] == 30 && pixels[i + 3] == 255;
    expect(visible, "cropping removes only transparent padding and retains visible artwork color and alpha");
    expect(load_image_rgba(hero, pixels, w, h) && w == 1920 && h == 1080 && ends_with(hero, "bd2.rgba"),
        "sharp detail hero replaces the old blurred cache variant at full output size");
    bool sharp = true;
    for (size_t i = 0; i < pixels.size(); i += 4)
        sharp &= (pixels[i] == 0 || pixels[i] == 255) && pixels[i + 1] == pixels[i] && pixels[i + 2] == pixels[i] && pixels[i + 3] == 255;
    expect(sharp, "high-contrast hero edges have no baked blur or softened intermediate pixels");
    cache.art.get("https://art.example/empty-logo", ArtKind::LaunchLogo,
        [&](const std::string& path) { logo = path; ++completed; });
    until([&] { return completed == 3; }, "empty launch logo did not settle");
    expect(logo.empty(), "fully transparent logo safely leaves the title fallback available");
}
void full_sized_download_posters(const std::string& dir) {
    Cache cache(dir);
    const std::string original = "https://images.metahub.space/poster/small/quality-fixture/img";
    const std::string large = "https://images.metahub.space/poster/large/quality-fixture/img";
    std::string old_path, full_path;
    cache.art.get_async(original, ArtKind::Poster, [&](const std::string& path) { old_path = path; });
    until([&] { return !old_path.empty(); }, "legacy small poster fixture did not complete");
    cache.art.get_async(original, ArtKind::DownloadPoster, [&](const std::string& path) { full_path = path; });
    until([&] { return !full_path.empty(); }, "full-sized Download poster did not complete");
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    expect(full_path != old_path && load_image_rgba(old_path, pixels, width, height) && width == 164 && height == 249,
           "the Download derivative does not overwrite or mistake a legacy catalog thumbnail for a full cover");
    expect(load_image_rgba(full_path, pixels, width, height) && width == 720 && height == 1080 &&
           pixels.size() == 720u * 1080u * 4u && pixels.front() == 0 && pixels[3] == 255,
           "the Download cover stays bounded at 720x1080 and contains an uncropped sharp edge for GPU corner rendering");
    unsigned transitions = 0;
    for (int x = 1; x < width; ++x)
        if (pixels[(size_t(540) * width + x) * 4] != pixels[(size_t(540) * width + x - 1) * 4]) ++transitions;
    expect(transitions > 60, "the high-resolution source retains fine poster detail beyond a small stretched thumbnail");
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        expect(calls[original] == 1 && calls[large] == 1,
               "Download requests the provider's larger image while catalog artwork retains its original URL");
    }
    const std::vector<std::pair<std::string, std::string>> variants{
        {"https://images.metahub.space/poster/medium/other/img?lang=it", "https://images.metahub.space/poster/large/other/img?lang=it"},
        {"https://image.tmdb.org/t/p/w185/cover.jpg", "https://image.tmdb.org/t/p/w780/cover.jpg"},
        {"https://image.tmdb.org/t/p/original/bounded.jpg", "https://image.tmdb.org/t/p/w780/bounded.jpg"},
        {"https://art.example/poster/small/custom?token=fixture", "https://art.example/poster/small/custom?token=fixture"},
        {"https://images.metahub.space.art.example/poster/small/lookalike/img", "https://images.metahub.space.art.example/poster/small/lookalike/img"},
    };
    int done = 0;
    for (const auto& [url, expected] : variants)
        cache.art.get_async(url, ArtKind::DownloadPoster, [&](const std::string& path) { if (!path.empty()) ++done; });
    until([&] { return done == int(variants.size()); }, "Download source variants did not complete");
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        for (const auto& [url, expected] : variants)
            expect(calls[expected] == 1, "known image sizing preserves metadata parameters and leaves other providers unchanged");
    }
    cache.art.stop(); cache.art.start(dir, 1); full_path.clear();
    cache.art.get_async(original, ArtKind::DownloadPoster, [&](const std::string& path) { full_path = path; });
    until([&] { return !full_path.empty(); }, "full-sized Download cache did not reopen");
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        expect(calls[large] == 1, "the full-sized derivative reopens from disk without another artwork transfer");
    }
}
} // namespace

extern "C" int __real_stat(const char* path, struct stat* info);
extern "C" int __wrap_stat(const char* path, struct stat* info) {
    if (track_controller_storage.load() && std::this_thread::get_id() == ui_thread) ++controller_storage_calls;
    return __real_stat(path, info);
}

HttpResponse http_get(const std::string& url, long, const std::atomic<bool>* cancel,
                      const std::vector<std::string>&, size_t) {
    std::unique_lock<std::mutex> lock(server_mutex);
    if (std::this_thread::get_id() == ui_thread) wrong_http_thread = true;
    const int attempt = ++calls[url]; requested.push_back(url);
    server_wake.notify_all();
    if (url.find("/gate-") != std::string::npos)
        while (gate_closed && !(cancel && *cancel)) server_wake.wait_for(lock, 10ms);
    HttpResponse response;
    if (cancel && *cancel) { response.error = "cancelled fixture"; return response; }
    response.status = ends_with(url, "fails-once") && attempt == 1 ? 503 : 200;
    response.body = url == "https://images.metahub.space/poster/large/quality-fixture/img" ? detailed_poster_image() :
        ends_with(url, "bad-body-once") && attempt == 1 ? "<html>temporary error</html>" :
        (ends_with(url, "padded-logo") || ends_with(url, "empty-logo") || ends_with(url, "sharp-hero")) ? special_image(url) : fixture_image();
    return response;
}
std::string HttpResponse::describe() const { return error.empty() ? "HTTP " + std::to_string(status) : error; }
std::string http_log_target(const std::string&) { return "https://art.example/[redacted]"; }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::string dir = argv[1];
        queue_priority_and_observers(dir + "/priority");
        cross_size_and_queue_cancel(dir + "/reuse");
        controller_cache_lookup(dir + "/controller");
        transient_failures(dir + "/failures");
        bounded_queue(dir + "/bounded");
        malformed_rgba(dir + "/malformed");
        launch_logo_and_sharp_hero(dir + "/launch-art");
        full_sized_download_posters(dir + "/download-art");
        std::cout << "PASS " << checks << " artwork checks (actual cache/decoder/worker code; fake HTTP only).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
