// Real App/DownloadManager wiring. Only transfer, artwork and player boundaries
// are replaced; no network, account, decoder or torrent engine starts.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "download_transfer.h"
#include "download_text.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0, player_opens = 0, art_lookups = 0;
std::atomic<int> unexpected_http{0}, unexpected_json{0};
std::atomic<bool> fail_transfer{true};
std::atomic<int> progressive_stage{0};
std::mutex transfer_mutex;
std::vector<DownloadTransferRequest> transfers;
Player::Options opened;
std::string artwork;
std::string delayed_poster_url;
std::vector<std::function<void(const std::string&)>> delayed_art_callbacks;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void until(Predicate condition, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    do {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(std::string("Timed out: ") + message);
}
Stream source(const std::string& name, const std::string& route) {
    Stream result;
    result.name = name; result.addon = "Configured provider";
    result.url = "https://media.invalid/" + route;
    result.kind = StreamKind::Direct;
    result.request_headers = {"X-Installation: " + name};
    return result;
}
Video episode(const std::string& id, const std::string& title, int season, int number) {
    Video result;
    result.id = id; result.title = title; result.season = season; result.episode = number;
    return result;
}
void make_artwork(const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    const uint32_t dimensions[] = {1, 1};
    const unsigned char pixel[] = {20, 50, 120, 255};
    file.write("RGBA", 4);
    file.write(reinterpret_cast<const char*>(dimensions), sizeof(dimensions));
    file.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    if (!file.good()) throw std::runtime_error("Could not write fixture artwork");
}
}

DownloadTransferResult wrapped_transfer(const DownloadTransferRequest&, const DownloadProgressCallback&,
                                       const std::atomic<bool>&) asm(WRAP_DOWNLOAD_TRANSFER);
DownloadTransferResult wrapped_transfer(const DownloadTransferRequest& request,
                                       const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
    { std::lock_guard lock(transfer_mutex); transfers.push_back(request); }
    if (request.stream.url.find("progressive.mkv") != std::string::npos) {
        { std::ofstream output(request.partial_path, std::ios::binary | std::ios::trunc); output << std::string(499, 'p'); }
        progress({499, 10000, 10000});
        while (!cancel && progressive_stage < 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (cancel) return {DownloadTransferStatus::Cancelled, {}, ".mkv", 499, 10000};
        { std::ofstream output(request.partial_path, std::ios::binary | std::ios::app); output << 'p'; }
        while (!cancel && progressive_stage < 2) {
            progress({500, 10000, 10000});
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        if (cancel) return {DownloadTransferStatus::Cancelled, {}, ".mkv", 500, 10000};
        { std::ofstream output(request.partial_path, std::ios::binary | std::ios::app); output << std::string(9500, 'p'); }
        progress({10000, 10000, 10000});
        return {DownloadTransferStatus::Complete, {}, ".mkv", 10000, 10000};
    }
    if (request.stream.url.find("hold.mkv") != std::string::npos) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!cancel && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        return {cancel ? DownloadTransferStatus::Cancelled : DownloadTransferStatus::Error,
                cancel ? "" : "Fixture transfer was not cancelled"};
    }
    if (request.stream.url.find("retry.mkv") != std::string::npos && fail_transfer)
        return {DownloadTransferStatus::Error, "Fixture connection interrupted"};
    const std::string bytes(512, 'V');
    std::ofstream file(request.partial_path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); file.close();
    if (!file) return {DownloadTransferStatus::Error, "Fixture file write failed"};
    progress({512, 512, 0});
    return {DownloadTransferStatus::Complete, "", ".mkv", 512, 512};
}
void wrapped_player_open(Player*, const Player::Options&) asm(WRAP_PLAYER_OPEN);
void wrapped_player_open(Player*, const Player::Options& options) { ++player_opens; opened = options; }
std::string wrapped_art_get(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_GET);
std::string wrapped_art_get(ArtCache*, const std::string& url, ArtKind,
    std::function<void(const std::string&)> callback, ArtPriority, const void*) {
    ++art_lookups;
    if (!delayed_poster_url.empty() && url == delayed_poster_url) {
        if (callback) delayed_art_callbacks.push_back(std::move(callback));
        return {};
    }
    return url.empty() ? "" : artwork;
}
std::string wrapped_art_peek(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_PEEK);
std::string wrapped_art_peek(ArtCache*, const std::string& url, ArtKind) {
    ++art_lookups;
    return url.empty() || (!delayed_poster_url.empty() && url == delayed_poster_url) ? "" : artwork;
}
std::string wrapped_art_get_async(ArtCache*, const std::string&, ArtKind,
    std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_GET_ASYNC);
std::string wrapped_art_get_async(ArtCache* cache, const std::string& url, ArtKind kind,
    std::function<void(const std::string&)> callback, ArtPriority priority, const void* observer) {
    return wrapped_art_get(cache, url, kind, std::move(callback), priority, observer);
}
std::string wrapped_art_peek_cached(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_PEEK_CACHED);
std::string wrapped_art_peek_cached(ArtCache* cache, const std::string& url, ArtKind kind) {
    return wrapped_art_peek(cache, url, kind);
}
bool wrapped_fetch(const std::string&, json&, std::string&, long, const std::atomic<bool>*) asm(WRAP_FETCH_JSON);
bool wrapped_fetch(const std::string&, json&, std::string& error, long, const std::atomic<bool>*) {
    ++unexpected_json; error = "Unexpected offline JSON request"; return false;
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void prepare(App& app, const std::string& directory) {
        app.data_dir_ = app.base_dir_ = directory;
        app.offline_ = true; app.settings_.auth_key = "FIXTURE_ACCOUNT";
        app.settings_.ui_language = "it"; app.settings_.ui_language_mode = "it";
        app.library_loaded_ = true; app.login_visible = false;
        std::string error;
        expect(app.downloads_.init(directory, &error), "persistent download manager initializes");
        app.downloads_.set_enabled(false);
        app.view = "detail"; app.zone = "content"; app.d_zone = "streams";
        app.d_item_.id = "series:opaque"; app.d_item_.type = "series"; app.d_item_.name = "Original title";
        app.d_item_.poster = "fixture-poster"; app.d_item_.background = "fixture-background";
        app.d_item_.logo = "fixture-logo";
        app.d_meta_loaded_ = true; app.d_series = true;
        app.d_meta_.id = app.d_item_.id; app.d_meta_.type = app.d_item_.type; app.d_meta_.name = "Complete title";
        app.d_meta_.videos = {episode("episode:first", "First episode", 1, 1),
                              episode("episode:selected/+?", "The selected episode", 3, 7),
                              episode("episode:last", "Last episode", 3, 8)};
        app.d_video_id_ = "episode:selected/+?";
        app.d_stream_list_ = {source("A", "first.mkv"), source("B", "second.mkv"),
                              source("C", "selected.mkv?installation=A%2BB")};
        app.d_view_ = {2, 0}; // Filtered display order differs from backing provider order.
        app.d_streams.resize(2);
        app.d_streams[0].name = "Selected C"; app.d_streams[1].name = "Other A";
        app.d_stream_sel = 0;
    }
    static void focus(App& app, const std::string& id) {
        app.set_view("downloads");
        const auto found = std::find_if(app.download_rows.begin(), app.download_rows.end(),
                                        [&](const UiDownload& item) { return item.id == id; });
        expect(found != app.download_rows.end(), "requested download remains in the public list");
        app.download_sel = static_cast<int>(found - app.download_rows.begin());
    }
    static std::string enqueue(App& app, const Stream& stream) {
        const auto previous = app.downloads_.snapshot();
        app.download_selected_stream(stream);
        until([&] {
            g_tasks.drain();
            return app.downloads_.snapshot().size() == previous.size() + 1 &&
                   (app.toast == "Video added to Downloads" || app.toast == "Video aggiunto a Download");
        }, "one selected request is durably committed and delivered to the UI");
        const auto entries = app.downloads_.snapshot();
        expect(entries.size() == previous.size() + 1, "one additional source creates one job");
        return entries.back().id;
    }
    static void run(App& app, const std::string& directory) {
        expect(download_error_text("Insufficient storage for this download", true) == "Spazio insufficiente per completare il download.",
               "insufficient-storage download errors are localized to Italian");
        expect(download_error_text("Download source returned HTTP 429", true) == "La sorgente ha risposto con HTTP 429.",
               "HTTP status remains visible in the localized download reason");
        expect(download_error_text("The downloaded file is missing or incomplete. Download it again.", true) ==
               "Il file scaricato è assente o incompleto. Scaricalo di nuovo.", "missing offline file errors are localized");
        expect(download_error_text("Encrypted HLS and DRM sources are not supported for offline downloads", true).find("cifrate") != std::string::npos,
               "encrypted HLS failures receive a specific localized explanation");
        expect(download_error_text("Unknown transport diagnostic", false) == "Unknown transport diagnostic",
               "English preserves the original safe transport diagnostic");
        expect(download_error_text("Unknown transport diagnostic", true) == "Riprova il download o scegli un'altra sorgente.",
               "unknown download reasons do not leak English into the Italian interface");
        expect(download_error_text("", true).empty(), "empty error remains empty without introducing a notification");
        expect(download_error_text("A download relocation is pending. Retry the same destination to finish moving the saved files.", true)
                   .find("stessa destinazione") != std::string::npos,
               "interrupted relocation tells Italian users to retry the same folder");
        prepare(app, directory);
        std::string selection_error;
        (void)app.downloads_.init(directory, directory, "", &selection_error);
        expect(app.downloads_.download_directory().empty(), "new profiles have no implicitly selected download folder");
        app.on_button(Btn::Square);
        expect(app.download_directory_notice && app.downloads_.snapshot().empty(),
               "download without a folder opens only the acknowledgement notice");
        const auto original_view = app.view;
        app.on_button(Btn::Triangle); app.on_button(Btn::L1);
        expect(app.download_directory_notice && app.view == original_view,
               "folder reminder consumes navigation without opening search or settings");
        app.on_button(Btn::Cross);
        expect(!app.download_directory_notice && app.view == original_view && app.downloads_.snapshot().empty(),
               "OK closes the reminder without queuing or changing screens");
        app.on_button(Btn::Square); app.on_button(Btn::Circle);
        expect(!app.download_directory_notice && app.downloads_.snapshot().empty(),
               "Circle dismisses the same reminder without starting a download");
        expect(app.downloads_.set_download_directory(directory + "/downloads", selection_error),
               "an explicit directory selection enables downloads");
        app.directory_picker_committing = true;
        const auto waiting_tasks = g_tasks.pending();
        app.download_selected_stream(app.d_stream_list_.front());
        expect(app.downloads_.snapshot().empty() && g_tasks.pending() == waiting_tasks,
               "a folder move blocks new stream submissions before its worker starts");
        app.directory_picker_committing = false;
        const auto tasks_before = g_tasks.pending();
        app.on_button(Btn::Square);
        app.d_stream_sel = 1;
        until([&] {
            g_tasks.drain();
            return app.downloads_.snapshot().size() == 1 && app.toast == "Video aggiunto a Download";
        }, "highlighted source is committed asynchronously without blocking input");
        app.d_stream_sel = 0;
        auto entries = app.downloads_.snapshot();
        expect(entries.size() == 1, "Square creates exactly one download for the highlighted stream");
        const auto first = entries[0].id;
        expect(entries[0].media_id == "series:opaque" && entries[0].video_id == "episode:selected/+?",
               "download retains the exact title and opaque selected episode identity");
        expect(entries[0].season == 3 && entries[0].episode == 7 && entries[0].title == "Complete title",
               "download stores selected season/episode and authoritative metadata title");
        expect(entries[0].subtitle.find("The selected episode") != std::string::npos,
               "download records the selected episode label");
        expect(app.view == "detail" && !app.watching() && !app.launch_visible && player_opens == 0,
               "Square stays on the stream list without starting foreground playback");
        expect(g_tasks.pending() == tasks_before, "enqueue does not create provider/autoplay tasks");
        app.on_button(Btn::Square);
        until([&] { g_tasks.drain(); return app.toast == "Video aggiunto a Download"; }, "duplicate selected request is acknowledged after deduplication");
        expect(app.downloads_.snapshot().size() == 1, "a repeated Square does not duplicate the same stream");
        app.d_pick_res = true; app.on_button(Btn::Square); app.d_pick_res = false;
        app.d_zone = "episodes"; app.on_button(Btn::Square); app.d_zone = "streams";
        app.d_stream_sel = 9; app.on_button(Btn::Square); app.d_stream_sel = 0;
        expect(app.downloads_.snapshot().size() == 1 && !app.watching(),
               "quality rows, episode rows and absent selections never enqueue a batch or play a video");

        app.downloads_.set_enabled(true);
        until([&] { const auto e = app.downloads_.find(first); return e && e->state == DownloadState::Complete; },
              "selected transfer completion");
        app.downloads_.set_enabled(false);
        {
            std::lock_guard lock(transfer_mutex);
            expect(transfers.size() == 1 && transfers[0].stream.url == app.d_stream_list_[2].url,
                   "filtered row resolves to its backing stream, not the same numeric index");
            expect(transfers[0].stream.request_headers == app.d_stream_list_[2].request_headers,
                   "the selected installation's request headers survive to the transfer");
            expect(transfers[0].season == 3 && transfers[0].episode == 7,
                   "transfer receives only the chosen torrent/video episode hint");
        }
        expect(player_opens == 0, "background completion never auto-opens the player");
        app.downloads_refresh();
        expect(app.download_rows.size() == 1 && app.download_rows[0].complete && app.download_rows[0].progress == 1,
               "completed file becomes a 100-percent offline item in the UI model");
        app.settings_.ui_language = "en"; app.downloads_refresh();
        expect(app.download_rows[0].status == "Available offline" && app.download_status.find("available offline") != std::string::npos,
               "English download status and section summary use the selected interface language");
        app.settings_.ui_language = "it"; app.downloads_refresh();
        expect(app.download_rows[0].status == "Disponibile offline" && app.download_status.find("disponibili offline") != std::string::npos,
               "Italian download status and section summary are restored independently of media language");
        until([&] {
            const auto e = app.downloads_.find(first);
            return e && e->poster_path.find("/downloads/") != std::string::npos &&
                   !e->logo_path.empty() && std::filesystem::is_regular_file(e->logo_path);
        }, "independent artwork worker persists the completed video's assets");
        const auto complete = app.downloads_.find(first);
        expect(complete && !complete->poster_path.empty() && !complete->logo_path.empty(),
               "cached artwork has been copied into persistent download storage");

        const auto held = enqueue(app, source("Held", "hold.mkv"));
        app.downloads_.set_enabled(true);
        until([&] { const auto e = app.downloads_.find(held); return e && e->state == DownloadState::Downloading; },
              "active download");
        focus(app, held); app.on_button(Btn::Cross);
        until([&] { const auto e = app.downloads_.find(held); return e && e->state == DownloadState::Paused; },
              "pause action");
        app.downloads_.set_enabled(false);
        // Wait for the cooperative transfer to release its active slot before
        // asking for Resume; the native queue owns cancellation itself.
        until([&] { return app.downloads_.resume(held); }, "cancelled transfer releases its slot");
        app.downloads_.pause(held); app.downloads_refresh(); focus(app, held);
        app.on_button(Btn::Cross);
        expect(app.downloads_.find(held)->state == DownloadState::Queued,
               "Cross resumes the same paused job instead of adding a new one");
        app.on_button(Btn::Cross);
        expect(app.downloads_.find(held)->state == DownloadState::Paused,
               "Cross can pause a queued job while retaining its stable identity");

        const auto retry = enqueue(app, source("Retry", "retry.mkv"));
        app.downloads_.set_enabled(true);
        until([&] { const auto e = app.downloads_.find(retry); return e && e->state == DownloadState::Failed; },
              "controlled transfer failure");
        app.downloads_.set_enabled(false); focus(app, retry);
        expect(app.download_rows[app.download_sel].failed, "transfer failure is reflected in the download row");
        app.on_button(Btn::Cross);
        expect(app.downloads_.find(retry)->state == DownloadState::Queued && app.downloads_.snapshot().size() == 3,
               "Cross retries the failed ID with no extra queue item");
        fail_transfer = false; app.downloads_.set_enabled(true);
        until([&] { const auto e = app.downloads_.find(retry); return e && e->state == DownloadState::Complete; },
              "retried transfer completion");
        app.downloads_.set_enabled(false);

        focus(app, held); app.on_button(Btn::Square);
        expect(app.dd_visible && app.dd_options.size() == 2 && app.dd_sel == 0,
               "Square opens file deletion with Cancel selected by default");
        app.on_button(Btn::Circle);
        expect(app.downloads_.find(held).has_value(), "cancelling deletion preserves the download");
        focus(app, held); app.on_button(Btn::Square);
        const auto stale_delete = app.dd_chosen_;
        app.on_button(Btn::Circle);
        app.directory_picker_committing = true;
        stale_delete(1);
        expect(app.downloads_.find(held).has_value(),
               "a deletion callback opened before relocation cannot remove a moving job");
        app.directory_picker_committing = false;
        focus(app, held); app.on_button(Btn::Square);
        std::reverse(app.download_rows.begin(), app.download_rows.end()); app.download_sel = 0;
        app.dd_sel = 1; app.on_button(Btn::Cross);
        expect(!app.downloads_.find(held) && app.downloads_.find(first) && app.downloads_.find(retry),
               "confirmed deletion targets the captured ID even if visible row indices changed");

        app.set_view("library"); app.on_button(Btn::R1);
        expect(app.view == "downloads" && app.nav_sel == 3, "Downloads is after Library in main navigation");
        app.on_button(Btn::R1);
        expect(app.view == "settings" && app.nav_sel == 4, "Settings follows Downloads");
        app.on_button(Btn::L1);
        expect(app.view == "downloads" && app.nav_sel == 3, "L1 returns from Settings to Downloads");
        app.download_sel = int(app.download_rows.size()) - 1; app.on_button(Btn::Down);
        expect(app.download_sel == int(app.download_rows.size()) - 1, "Download navigation stops at the actual last row");

        std::filesystem::remove(artwork); // The ordinary artwork cache is gone.
        expect(std::filesystem::is_regular_file(complete->poster_path), "offline artwork survives original cache removal");
        app.progress_[complete->media_id] = {complete->type, complete->title, "", complete->video_id, 38, 1000, 0};
        focus(app, first);
        app.directory_picker_committing = true;
        app.play_download(first);
        app.start_download_playback(first, false, 0);
        expect(player_opens == 0 && !app.dd_visible && !app.watching(),
               "moving a completed file prevents both resume prompts and direct player opens");
        app.directory_picker_committing = false;
        app.play_download(first);
        const auto stale_resume = app.dd_chosen_;
        app.on_button(Btn::Circle);
        app.directory_picker_committing = true;
        stale_resume(1);
        expect(player_opens == 0 && !app.watching(),
               "a previously opened resume choice cannot reopen an old path during relocation");
        app.directory_picker_committing = false;
        const auto lookup_before = art_lookups;
        const auto notification_before = app.toast_revision;
        int audio_releases = 0;
        app.before_playback = [&] { ++audio_releases; };
        app.on_button(Btn::Cross);
        expect(app.dd_visible && app.dd_options.size() == 2 && app.dd_sel == 1 &&
               app.dd_options[0].label == "Riproduci dall'inizio" && app.dd_options[1].label == "Riproduci da 0:38",
               "a watched complete local file offers beginning or the exact same-episode resume time");
        expect(player_opens == 0 && audio_releases == 0 && !app.watching(),
               "the resume choice is presented before opening the player or releasing audio");
        app.on_button(Btn::Circle);
        expect(!app.dd_visible && player_opens == 0 && app.downloads_.find(first).has_value(),
               "Circle cancels local playback without opening or removing the download");
        app.on_button(Btn::Cross);
        std::reverse(app.download_rows.begin(), app.download_rows.end()); app.download_sel = 0;
        app.on_button(Btn::Cross);
        expect(player_opens == 1 && audio_releases == 1 && app.watching() && app.w_offline_ && app.w_download_id_ == first,
               "the resume choice opens its captured download even if the rows are reordered");
        expect(opened.url == complete->local_path && opened.headers.empty() && !opened.parallel && opened.start == 38,
               "offline player receives the local file, no provider credentials, and the same-episode resume point");
        expect(app.launch_image == complete->background_path && app.launch_logo == complete->logo_path,
               "offline launch uses only artwork stored alongside the downloaded video");
        expect(g_tasks.pending() == tasks_before && art_lookups == lookup_before && unexpected_json == 0,
               "offline playback does not fetch add-ons, hashes, subtitles or cache artwork");
        expect(app.toast_revision == notification_before, "offline playback does not emit subtitle notifications");
        expect(!app.autoplay_pending_ && !app.t_visible, "offline playback starts no next episode or torrent statistics");
        app.watch_stop(false);
        expect(!app.watching() && !app.w_offline_ && app.view == "downloads",
               "stopping offline playback returns to the Downloads section");
        app.progress_[complete->media_id] = {complete->type, complete->title, "", complete->video_id, 38, 1000, 0};
        focus(app, first); app.on_button(Btn::Cross);
        app.dd_sel = 0; app.on_button(Btn::Cross);
        expect(player_opens == 2 && opened.start == 0 && app.watching(),
               "Play from beginning explicitly starts the local file at zero");
        app.watch_stop(false);
        app.progress_[complete->media_id] = {complete->type, complete->title, "", "another-episode", 38, 1000, 0};
        focus(app, first); app.on_button(Btn::Cross);
        expect(player_opens == 3 && !app.dd_visible && opened.start == 0,
               "progress belonging to another episode neither prompts nor seeks this download");
        app.watch_stop(false);
        app.progress_[complete->media_id] = {complete->type, complete->title, "", complete->video_id,
                                           std::numeric_limits<double>::quiet_NaN(), 1000, 0};
        focus(app, first); app.on_button(Btn::Cross);
        expect(player_opens == 4 && !app.dd_visible && opened.start == 0,
               "invalid stored progress cannot enter a resume label or decoder seek");
        app.watch_stop(false);
        app.progress_[complete->media_id] = {complete->type, complete->title, "", complete->video_id, 38, 1000, 0};
        focus(app, first); app.on_button(Btn::Cross);
        ++app.account_generation_;
        app.on_button(Btn::Cross);
        expect(player_opens == 4 && !app.watching(),
               "a stale resume choice cannot start playback after an account change");
        app.before_playback = {};
        const auto progressive = enqueue(app, source("Progressive", "progressive.mkv"));
        app.downloads_.set_enabled(true);
        until([&] { const auto e = app.downloads_.find(progressive); return e && e->done == 499; },
              "progressive fixture reaches 4.99 percent");
        focus(app, progressive);
        const int opens_before = player_opens;
        app.on_button(Btn::Options);
        expect(player_opens == opens_before && !app.watching() && app.view == "downloads",
               "Options at 4.99 percent cannot play or open Search");
        progressive_stage = 1;
        until([&] { const auto e = app.downloads_.find(progressive); return e && e->playable_while_downloading; },
              "progressive fixture reaches exact five percent");
        app.downloads_refresh(); focus(app, progressive);
        expect(app.download_rows[app.download_sel].playable_while_downloading &&
               app.downloads_.find(progressive)->done == 500, "exact five percent exposes the selected row's Options action");
        expect(!app.download_rows[app.download_sel].remaining.empty() &&
               app.download_rows[app.download_sel].connections.empty(),
               "active download rows carry the ETA without invented torrent counters for HTTP");
        app.on_button(Btn::Options);
        expect(player_opens == opens_before + 1 && app.watching() && app.w_offline_ && app.w_growing_download_,
               "Options routes through the real App to the growing-file Player input");
        expect(opened.growing_download == app.w_growing_download_ && opened.headers.empty() && !opened.parallel &&
               opened.url.ends_with("/media.part") && app.w_download_id_ == progressive && opened.start == 0,
               "Options opens only the selected local prefix without provider headers or a second stream");
        expect(app.downloads_.find(progressive)->state == DownloadState::Downloading && !app.w_torrent_slot_,
               "progressive watching leaves its download running without a foreground torrent reservation");
        const auto live_source = opened.growing_download;
        progressive_stage = 2;
        until([&] { const auto e = app.downloads_.find(progressive); return e && e->state == DownloadState::Complete; },
              "background transfer completes during playback");
        expect(live_source->state->snapshot().phase == GrowingFilePhase::Complete &&
               app.watching() && player_opens == opens_before + 1,
               "download completion updates the same live source without reopening or stopping Player");
        app.watch_stop(false);
        expect(!app.w_growing_download_ && app.view == "downloads" &&
               app.downloads_.find(progressive)->state == DownloadState::Complete,
               "closing progressive playback releases its lease and preserves the completed offline video");
        app.downloads_.set_enabled(false);
        delayed_poster_url = "https://art.invalid/download-poster.jpg";
        app.d_item_.poster = app.d_meta_.poster = delayed_poster_url;
        app.d_video_id_ = "episode:queued-art-first";
        const auto art_first = enqueue(app, source("Art first", "art-first.mkv"));
        app.d_video_id_ = "episode:queued-art-second";
        const auto art_second = enqueue(app, source("Art second", "art-second.mkv"));
        expect(!delayed_art_callbacks.empty() && app.downloads_.find(art_first)->poster_url == delayed_poster_url &&
               app.downloads_.find(art_second)->poster_url == delayed_poster_url,
               "queued episodes retain their missing poster URL and request artwork asynchronously");
        expect(app.downloads_.find(art_first)->poster_path.empty() && app.downloads_.find(art_second)->poster_path.empty(),
               "a cache miss remains pending rather than pretending that a placeholder was saved");
        const std::string arrived_poster = directory + "/delayed-poster.rgba";
        make_artwork(arrived_poster);
        for (const auto& callback : delayed_art_callbacks) callback(arrived_poster);
        delayed_art_callbacks.clear();
        until([&] {
            const auto one = app.downloads_.find(art_first), two = app.downloads_.find(art_second);
            return one && two && one->poster_path.find("/downloads/" + art_first + "/") != std::string::npos &&
                   two->poster_path.find("/downloads/" + art_second + "/") != std::string::npos &&
                   std::filesystem::is_regular_file(one->poster_path) && std::filesystem::is_regular_file(two->poster_path);
        }, "one poster arrival is privately persisted for every queued episode using its URL");
        app.downloads_refresh(); focus(app, art_second);
        expect(app.download_rows[app.download_sel].image == app.downloads_.find(art_second)->poster_path,
               "queued episode UI uses the newly persisted real cover");
        std::filesystem::remove(arrived_poster);
        app.downloads_.shutdown();
        expect(app.downloads_.init(directory) &&
               std::filesystem::is_regular_file(app.downloads_.find(art_first)->poster_path) &&
               std::filesystem::is_regular_file(app.downloads_.find(art_second)->poster_path),
               "all queued covers survive manager restart after original cache artwork is removed");
        delayed_poster_url.clear();

        app.downloads_.set_enabled(false);
        app.downloads_.shutdown();
        const std::string orphan_id = "d" + std::string(32, 'a');
        const std::string orphan_directory = directory + "/downloads/" + orphan_id;
        std::filesystem::create_directory(orphan_directory);
        { std::ofstream partial(orphan_directory + "/media.part", std::ios::binary); partial << std::string(4096, 'x'); }
        expect(app.downloads_.init(directory), "readable orphan files do not prevent inventory startup");
        app.downloads_.set_enabled(false);
        focus(app, orphan_id);
        const auto& recovered = app.download_rows[app.download_sel];
        expect(recovered.recovery_only && recovered.failed && !recovered.complete &&
               !recovered.playable_while_downloading && recovered.progress < 0,
               "unrecognized disk files remain visible without invented playback or completion state");
        expect(recovered.title.find("Download da verificare") == 0 &&
               recovered.size.find("Spazio occupato:") == 0,
               "recovered files have an identifiable localized title and occupied storage label");
        const int opens_before_recovery = player_opens;
        const auto callbacks_before_recovery = delayed_art_callbacks.size();
        app.on_button(Btn::Options);
        app.on_button(Btn::Cross);
        expect(player_opens == opens_before_recovery && app.downloads_.find(orphan_id)->recovery_only &&
               delayed_art_callbacks.size() == callbacks_before_recovery &&
               app.toast.find("informazioni del download") != std::string::npos,
               "recovery actions explain the retained files without playing or resuming an unknown source");
        expect(std::filesystem::file_size(orphan_directory + "/media.part") == 4096,
               "viewing recovery information preserves every partial-file byte");
        app.on_button(Btn::Square);
        expect(app.dd_visible && app.dd_sel == 0 && app.dd_title.find("Download da verificare") != std::string::npos,
               "recovery deletion identifies the chosen files and defaults to Cancel");
        app.on_button(Btn::Circle);
        expect(std::filesystem::exists(orphan_directory + "/media.part"),
               "cancelling recovery deletion leaves the disk files intact");
        app.on_button(Btn::Square); app.dd_sel = 1; app.on_button(Btn::Cross);
        until([&] { return !std::filesystem::exists(orphan_directory); },
              "confirmed orphan deletion finishes on the background worker");
        expect(!app.downloads_.find(orphan_id) && !std::filesystem::exists(orphan_directory),
               "confirmed recovery deletion removes its retained files as well as the visible entry");
        expect(unexpected_http == 0 && unexpected_json == 0, "all App wiring checks completed without network traffic");
    }

    static void save_observed(App& app, double position, double duration, int64_t prefix = 0, int64_t total = -1) {
        // The decoder boundary stays closed/zero. Supply the last actually
        // presented sample to exercise the production teardown recovery path.
        app.w_started_ = true; app.w_observed_position_ = position; app.w_observed_duration_ = duration;
        app.w_observed_download_prefix_ = prefix; app.w_observed_download_total_ = total;
        app.offline_ = false;
        app.watch_stop(false);
        app.offline_ = true;
    }

    static void bookmarks(const std::string& directory) {
        std::filesystem::create_directories(directory);
        App app;
        prepare(app, directory);
        app.d_meta_.has_scheduled_videos = true;
        const auto first = enqueue(app, source("Episode one", "bookmarked-one.mkv"));
        app.d_video_id_ = "episode:last";
        const auto second = enqueue(app, source("Episode two", "bookmarked-two.mkv"));
        app.downloads_.set_enabled(true);
        until([&] { return app.downloads_.find(first)->state == DownloadState::Complete &&
                           app.downloads_.find(second)->state == DownloadState::Complete; }, "scheduled episodes finish downloading");
        app.downloads_.set_enabled(false);
        focus(app, first);
        expect(app.download_rows[app.download_sel].title == "Complete title" &&
               app.download_rows[app.download_sel].subtitle == "S03E07 · The selected episode",
               "scheduled episode rows display the series, season, episode and episode title");
        const auto first_entry = *app.downloads_.find(first);
        app.d_video_id_ = first_entry.video_id;
        expect(app.play_saved_stream(source("Episode one", "bookmarked-one.mkv")) && app.watching() &&
               !app.dd_visible && opened.url == first_entry.local_path,
               "selecting the same online source reuses its complete local file without a new transfer");
        save_observed(app, 123.5, 1800);
        const auto first_key = video_progress_key(first_entry.type, first_entry.media_id, first_entry.video_id);
        expect(app.video_progress_.at(first_key).time == 123.5 && app.progress_.at(first_entry.media_id).time == 123.5,
               "closing a zeroed decoder saves the last presented time before teardown");
        focus(app, second); app.on_button(Btn::Options);
        expect(app.watching() && !app.dd_visible && opened.start == 0, "the next episode does not inherit the previous episode's timestamp");
        save_observed(app, 87, 1600);
        expect(app.video_progress_.at(first_key).time == 123.5,
               "watching another episode does not overwrite the earlier download bookmark");
        app.progress_.clear(); app.video_progress_.clear(); app.watched_.clear(); app.load_progress();
        expect(app.video_progress_.at(first_key).time == 123.5 &&
               app.progress_.at(first_entry.media_id).video_id == "episode:last",
               "restart restores individual episode bookmarks and the series continue-watching position separately");
        app.downloads_.shutdown();
        expect(app.downloads_.init(directory), "downloaded episode inventory reloads without a metadata provider");
        focus(app, first); app.on_button(Btn::Options);
        expect(app.dd_visible && app.dd_options[1].label == "Riproduci da 2:03" &&
               app.dd_title == "Complete title · S03E07 · The selected episode",
               "Options after restart offers the exact saved episode time with its complete identity");
        app.on_button(Btn::Cross);
        expect(app.watching() && opened.start == 123.5, "confirming Resume reaches the local decoder with the persisted timestamp");
        app.watch_stop(false);
        app.d_meta_.is_live = true;
        const auto jobs_before = app.downloads_.snapshot().size();
        app.download_selected_stream(source("Actual live", "live.m3u8"));
        expect(app.downloads_.snapshot().size() == jobs_before && app.toast.find("dirette") != std::string::npos,
               "actual live TV remains excluded while scheduled series are downloadable");
        app.d_meta_.is_live = false;

        progressive_stage = 0;
        app.d_video_id_ = "episode:first";
        const auto partial_stream = source("Partial bookmark", "progressive.mkv");
        const auto partial = enqueue(app, partial_stream);
        app.downloads_.set_enabled(true);
        until([&] { return app.downloads_.find(partial)->done == 499; }, "online reuse fixture reaches its initial partial prefix");
        app.offline_ = false;
        const auto opens_before_wait = player_opens;
        expect(app.play_saved_stream(partial_stream) && app.launch_visible && !app.watching() &&
               !app.w_torrent_slot_ && app.saved_stream_download_ == partial,
               "same-source playback below five percent waits on the existing background transfer");
        app.on_button(Btn::Circle); app.saved_stream_tick();
        expect(!app.launch_visible && app.saved_stream_download_.empty() && player_opens == opens_before_wait &&
               app.downloads_.find(partial)->state == DownloadState::Downloading,
               "Circle cancels only the pending playback, preserving its running download");
        app.offline_ = true; progressive_stage = 1;
        until([&] { return app.downloads_.find(partial)->playable_while_downloading; }, "partial bookmark reaches a verified prefix");
        expect(app.play_saved_stream(partial_stream) && app.watching() && opened.start == 0 && !app.w_torrent_slot_,
               "a ready same-source download opens progressive playback without another torrent reservation");
        save_observed(app, 45, 1000, 500, 10000);
        const auto partial_entry = *app.downloads_.find(partial);
        const auto partial_key = video_progress_key(partial_entry.type, partial_entry.media_id, partial_entry.video_id);
        expect(app.video_progress_.at(partial_key).download_prefix == 500 &&
               app.video_progress_.at(partial_key).download_source == partial_entry.source_identity,
               "partial viewing persists its actually presented timestamp with the acknowledged source prefix");
        app.video_progress_.clear(); app.progress_.clear(); app.load_progress();
        focus(app, partial); app.on_button(Btn::Options);
        expect(app.dd_visible && app.dd_options[1].label == "Riproduci da 0:45", "a restored prefix proof offers partial-file Resume");
        app.on_button(Btn::Cross);
        expect(app.watching() && opened.start == 45 && opened.progressive_resume_limit == 45 && opened.growing_download,
               "the player receives a verified progressive resume limit rather than a percentage estimate");
        app.watch_stop(false);

        app.video_progress_[partial_key].download_prefix = 501;
        focus(app, partial); app.on_button(Btn::Options);
        expect(app.watching() && !app.dd_visible && opened.start == 0 && opened.progressive_resume_limit == 0,
               "a recovered prefix below the saved bound starts safely without seeking unavailable data");
        save_observed(app, 10, 1000, 500, 10000);
        expect(app.video_progress_.at(partial_key).time == 45 && app.video_progress_.at(partial_key).download_prefix == 501,
               "safe fallback after rollback preserves the older bookmark for later completion");
        app.video_progress_[partial_key].download_prefix = 500;
        focus(app, partial); app.on_button(Btn::Options);
        expect(app.dd_visible, "the same bookmark becomes available when its prefix is verified again");
        const auto delayed_choice = app.dd_chosen_;
        app.on_button(Btn::Circle);
        app.video_progress_[partial_key].download_prefix = 501;
        delayed_choice(1);
        expect(app.watching() && opened.start == 0 && opened.progressive_resume_limit == 0,
               "a rollback between showing Resume and confirming it is rechecked before opening the player");
        app.watch_stop(false);
        progressive_stage = 2;
        until([&] { return app.downloads_.find(partial)->state == DownloadState::Complete; }, "partial bookmark download completes");
        focus(app, partial); app.on_button(Btn::Options);
        expect(app.dd_visible, "a completed file can use its saved episode bookmark even after an earlier checkpoint rollback");
        app.on_button(Btn::Circle);
        app.downloads_.set_enabled(false);
        app.d_video_id_ = "episode:last";
        const auto paused_stream = source("Paused reuse", "hold.mkv");
        const auto paused = enqueue(app, paused_stream);
        expect(app.downloads_.pause(paused), "online reuse fixture pauses the saved selected source");
        app.offline_ = false;
        expect(app.play_saved_stream(paused_stream) && app.launch_visible && app.saved_stream_download_ == paused,
               "reselecting a paused online source submits resume for the existing job");
        until([&] { g_tasks.drain(); return !app.saved_stream_resuming_ &&
                   app.downloads_.find(paused)->state == DownloadState::Queued; }, "saved-source resume commits on its worker");
        app.saved_stream_poll_at_ = 0; app.saved_stream_tick();
        expect(!app.w_torrent_slot_ && app.launch_visible && !app.watching(),
               "queued saved-source playback holds no second foreground engine reservation");
        app.on_button(Btn::Circle); app.saved_stream_tick();
        expect(app.downloads_.find(paused)->state == DownloadState::Queued && app.saved_stream_download_.empty(),
               "cancelling queued playback does not delete or pause the resumed download");
        auto different_stream = paused_stream; ++different_stream.file_idx;
        expect(!app.play_saved_stream(different_stream), "another torrent file index cannot reuse the selected saved episode");
        app.d_video_id_ = "episode:first";
        expect(!app.play_saved_stream(paused_stream), "the same URL attached to another episode cannot reuse this download");
        app.offline_ = true;
        app.shutdown();
        expect(unexpected_http == 0 && unexpected_json == 0, "bookmark persistence and recovery require no online provider requests");
    }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    App app;
    try {
        g_tasks.start(2);
        artwork = std::string(argv[1]) + "/source-art.rgba";
        make_artwork(artwork);
        AppProtocolTest::run(app, argv[1]);
        AppProtocolTest::bookmarks(std::string(argv[1]) + "/bookmarks");
        app.shutdown(); g_tasks.stop();
        std::cout << "PASS: " << checks << " real App download/controller/offline assertions; no HTTP or decoder started\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " assertions: " << error.what() << '\n';
        app.before_playback = {};
        app.shutdown(); g_tasks.stop();
        return 1;
    }
}
