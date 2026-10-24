// Real App/Tasks account state and persistence, with only remote boundaries replaced.
#include "app.h"
#include <curl/curl.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0;
std::atomic<int> unexpected_http{0}, addons_reload{0}, library_reload{0}, qr_requests{0};
std::atomic<int> collection_requests{0}, library_requests{0};
void expect(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
template<class Predicate> void until(Predicate condition, const char* description, bool drain = true) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (drain) g_tasks.drain();
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < limit);
    throw std::runtime_error(std::string("Timed out: ") + description);
}
struct GateBase { virtual ~GateBase() = default; virtual void release() = 0; };
std::vector<std::weak_ptr<GateBase>> gates;
template<class T> struct Gate : GateBase {
    explicit Gate(T value) : response(std::move(value)) {}
    T response;
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    std::atomic<bool> entered{false};
    std::string argument;
    void release() override { std::lock_guard<std::mutex> lock(mutex); open = true; cv.notify_all(); }
    T call(const std::string& value = {}) {
        std::unique_lock<std::mutex> lock(mutex);
        argument = value;
        entered.store(true);
        if (!cv.wait_for(lock, std::chrono::seconds(6), [&] { return open; }))
            throw std::runtime_error("Test response gate was never released");
        return response;
    }
};
template<class T> struct Plan {
    std::mutex mutex;
    std::deque<std::shared_ptr<Gate<T>>> replies;
    std::atomic<int> calls{0};
    std::shared_ptr<Gate<T>> add(T value) {
        auto result = std::make_shared<Gate<T>>(std::move(value));
        gates.push_back(result);
        std::lock_guard<std::mutex> lock(mutex);
        replies.push_back(result);
        return result;
    }
    T take(const std::string& argument = {}) {
        std::shared_ptr<Gate<T>> reply;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (replies.empty()) throw std::runtime_error("Unexpected account boundary call");
            reply = replies.front(); replies.pop_front(); ++calls;
        }
        return reply->call(argument);
    }
};
Plan<LinkCode> creates;
Plan<ApiResult> reads, users, logouts, exchanges;
LinkCode code(const std::string& value) { return {true, value, "https://link.stremio.com/" + value, {}}; }
ApiResult approved(const std::string& key) { return {true, json{{"authKey", key}}, {}}; }
ApiResult profile(const std::string& email) { return {true, json{{"email", email}}, {}}; }
void flush_tasks() {
    auto done = std::make_shared<std::atomic<bool>>(false);
    g_tasks.run([done] { g_tasks.post([done] { done->store(true); }); });
    until([&] { return done->load(); }, "worker/UI fence");
}
json saved(const std::string& directory) {
    json result;
    expect(load_json(directory + "/settings.json", result), "settings are readable immediately");
    return result;
}
} // namespace

ApiResult account_test_session_exchange(const std::string& token) { return exchanges.take(token); }

// Names come from nm + c++filt for the existing host executable, in the runner.
LinkCode wrapped_create() asm(WRAP_LINK_CREATE);
LinkCode wrapped_create() { return creates.take(); }
ApiResult wrapped_read(const std::string&) asm(WRAP_LINK_READ);
ApiResult wrapped_read(const std::string& value) { return reads.take(value); }
ApiResult wrapped_user(const std::string&) asm(WRAP_GET_USER);
ApiResult wrapped_user(const std::string& value) { return users.take(value); }
ApiResult wrapped_logout(const std::string&) asm(WRAP_LOGOUT);
ApiResult wrapped_logout(const std::string& value) { return logouts.take(value); }
// Calls inside app.cpp bind locally and cannot be intercepted with --wrap for
// App::load_addons/load_library. Startup exercises those implementations and
// wraps their external API boundaries instead.
ApiResult wrapped_collection(const std::string&) asm(WRAP_ADDON_COLLECTION);
ApiResult wrapped_collection(const std::string&) {
    ++collection_requests;
    const json manifest = {{"id", "test.account.streams"}, {"name", "Offline account fixture"},
                           {"resources", json::array({"stream"})}, {"types", json::array({"movie"})}};
    return {true, json{{"addons", json::array({json{{"transportUrl", "https://account.test/manifest.json"}, {"manifest", manifest}}})}}, {}};
}
ApiResult wrapped_library_get(const std::string&) asm(WRAP_LIBRARY_GET);
ApiResult wrapped_library_get(const std::string&) { ++library_requests; return {true, json::array(), {}}; }
void wrapped_addons(App*) asm(WRAP_ADDONS);
void wrapped_addons(App*) { ++addons_reload; }
void wrapped_library(App*) asm(WRAP_LIBRARY);
void wrapped_library(App*) { ++library_reload; }
std::string wrapped_art(App*, const std::string&, ArtKind, bool) asm(WRAP_ART);
std::string wrapped_art(App*, const std::string&, ArtKind kind, bool) {
    expect(kind == ArtKind::Qr, "account flow requests QR artwork only");
    ++qr_requests;
    return "fixture-qr.rgba";
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void prepare(App& app, const std::string& directory) {
        app.data_dir_ = directory;
        app.offline_ = false; // Exercise real save_settings/flush_settings and their epoch.
        app.settings_.ui_language = app.ui_language = "it";
        app.settings_.ui_language_mode = "it";
        app.settings_refresh();
    }
    // A valid active link response, installed as a fixture independently of the
    // create-code boundary tested below. This permits exact callback ordering.
    static void active_code(App& app, const std::string& value) {
        ++app.login_gen_;
        app.login_visible = true;
        app.login_state = App::LoginState::ready;
        app.login_code = app.login_code_raw_ = value;
        app.login_qr_url_.clear();
        app.login_next_poll_ = 0;
        app.login_expires_ = now_seconds() + 600;
        app.login_polling_ = false;
    }
    static void separate_device_session(const std::string& directory) {
        const auto profile_path = directory + "/device-session";
        expect(make_dirs(profile_path), "device-session profile directory exists");
        App app; prepare(app, profile_path);
        active_code(app, "DISTINCT_SESSION");
        auto approval = reads.add(approved("BROWSER_SUCCESS"));
        auto exchange = exchanges.add(approved("DEVICE_SUCCESS"));
        auto user = users.add(profile("device@example.test"));
        app.login_poll(); approval->release();
        until([&] { return exchange->entered.load(); }, "device-session exchange", false);
        expect(exchange->argument == "BROWSER_SUCCESS", "exchange receives the browser approval credential");
        expect(!app.signed_in(), "approval alone does not authenticate before the session exchange completes");
        exchange->release();
        until([&] { return user->entered.load(); }, "profile using device session");
        expect(app.signed_in() && app.settings_.auth_key == "DEVICE_SUCCESS",
               "App retains the returned device credential instead of the browser approval");
        expect(user->argument == "DEVICE_SUCCESS", "authenticated profile request uses the new device session");
        expect(jstr(saved(profile_path), "auth_key") == "DEVICE_SUCCESS",
               "device credential is on disk before the profile request completes");
        // Model revocation of only the browser credential at the API boundary.
        // The production server's revocation policy still requires a live test.
        auto browser_logout = logouts.add({true, json::object(), {}});
        browser_logout->release();
        expect(api_logout("BROWSER_SUCCESS").ok, "browser-session revocation fixture succeeds");
        expect(browser_logout->argument == "BROWSER_SUCCESS" && app.settings_.auth_key != browser_logout->argument,
               "browser logout never selects the saved device credential");
        expect(jstr(saved(profile_path), "auth_key") == "DEVICE_SUCCESS",
               "browser logout leaves the distinct persisted device credential intact");
        user->release(); flush_tasks();
        app.shutdown(); flush_tasks();
        App restarted;
        expect(restarted.init(profile_path, profile_path, false), "saved device-session profile restarts");
        expect(restarted.signed_in() && restarted.settings_.auth_key == "DEVICE_SUCCESS" && !restarted.login_visible,
               "restart loads the exchanged device credential without another pairing");
        restarted.shutdown(); flush_tasks();
    }
    static void session_exchange_failures(const std::string& directory) {
        App app; prepare(app, directory);
        for (const auto& response : std::vector<ApiResult>{{false, json{{"authKey", "IGNORED"}}, "offline"},
                                                          {true, json::object(), {}}}) {
            active_code(app, "FAILED_EXCHANGE");
            auto approval = reads.add(approved("BROWSER_FAILED"));
            auto exchange = exchanges.add(response);
            approval->release(); exchange->release();
            app.login_poll(); flush_tasks();
            expect(!app.signed_in() && app.login_state == App::LoginState::error &&
                   app.login_code.empty() && app.login_code_raw_.empty(),
                   "failed or empty exchange clears pairing and cannot persist the browser credential");
        }
        app.login_close();
    }
    static void cancelled_device_session(const std::string& directory, bool expire) {
        App app; prepare(app, directory);
        active_code(app, expire ? "EXPIRING_EXCHANGE" : "CANCELLED_EXCHANGE");
        const std::string browser = expire ? "BROWSER_EXPIRED" : "BROWSER_CANCELLED";
        const std::string device = expire ? "DEVICE_EXPIRED" : "DEVICE_CANCELLED";
        auto approval = reads.add(approved(browser));
        auto exchange = exchanges.add(approved(device));
        auto cleanup = logouts.add({true, json::object(), {}});
        app.login_poll(); approval->release();
        until([&] { return exchange->entered.load(); }, "exchange held across cancellation/expiry", false);
        if (expire) {
            app.login_expires_ = now_seconds() - 1;
            app.login_poll();
            expect(app.login_state == App::LoginState::expired, "expiry remains responsive during exchange");
        } else app.login_close();
        exchange->release();
        until([&] { return cleanup->entered.load(); }, "discarding newly issued device session");
        expect(!app.signed_in() && cleanup->argument == device && cleanup->argument != browser,
               "cancelled/expired exchange revokes only its newly issued device credential");
        cleanup->release(); flush_tasks();
        app.login_close();
    }
    static void startup_routes(const std::string& directory) {
        const auto unsigned_dir = directory + "/startup-unsigned";
        const auto signed_dir = directory + "/startup-signed";
        const auto offline_dir = directory + "/startup-offline";
        expect(make_dirs(unsigned_dir) && make_dirs(signed_dir) && make_dirs(offline_dir), "startup fixture directories exist");
        auto fresh = creates.add(code("STARTUP_CODE"));
        const int old_addons = addons_reload + collection_requests, old_library = library_reload + library_requests;
        {
            App app;
            expect(app.init(unsigned_dir, unsigned_dir, false), "online startup with no saved account initializes");
            expect(!app.signed_in() && app.login_visible && app.login_state == App::LoginState::loading,
                   "unsigned online startup opens the login page immediately");
            expect(addons_reload + collection_requests == old_addons && library_reload + library_requests == old_library,
                   "unsigned startup does not load authenticated browsing data");
            fresh->release(); flush_tasks();
            expect(app.login_state == App::LoginState::ready && app.login_code == "STARTUP_CODE", "startup login loads its code");
            app.on_button(Btn::Circle);
            expect(app.wants_exit() && !app.signed_in(), "Circle on required login exits instead of entering Home anonymously");
            app.shutdown(); flush_tasks();
        }
        expect(save_json(signed_dir + "/settings.json", json{{"auth_key", "SAVED_STARTUP_AUTH"}, {"user_email", "saved@example.test"}}),
               "saved-account startup fixture is persisted");
        const int previous_creates = creates.calls;
        {
            App app;
            expect(app.init(signed_dir, signed_dir, false), "online startup with saved account initializes");
            expect(app.signed_in() && !app.login_visible && app.view == "home" && !app.wants_exit(),
                   "saved account enters Home without requesting another login");
            flush_tasks();
            expect(addons_reload + collection_requests == old_addons + 1 && library_reload + library_requests == old_library + 1 && creates.calls == previous_creates,
                   "saved-account startup loads addons and library without a create request");
            app.shutdown(); flush_tasks();
        }
        {
            App app;
            expect(app.init(offline_dir, offline_dir, true), "offline preview initializes");
            expect(!app.login_visible && !app.signed_in() && creates.calls == previous_creates && addons_reload + collection_requests == old_addons + 1,
                   "offline preview remains isolated from automatic login and remote addon loading");
            app.shutdown(); flush_tasks();
        }
    }
    static void create_and_cancel(const std::string& directory) {
        App app; prepare(app, directory);
        auto old = creates.add(code("OLD")), current = creates.add(code("CURRENT"));
        app.login_start();
        until([&] { return old->entered.load(); }, "old code request", false);
        expect(app.login_state == App::LoginState::loading, "code request exposes the loading state");
        app.login_close();
        expect(!app.login_visible && app.login_code.empty() && app.login_qr.empty(), "cancel clears code and QR");
        app.login_start();
        old->release();
        until([&] { return current->entered.load(); }, "new code request", false);
        g_tasks.drain();
        expect(app.login_visible && app.login_code.empty(), "stale create response cannot populate a new login");
        current->release(); flush_tasks();
        expect(app.login_code == "CURRENT" && app.login_code_raw_ == "CURRENT", "current create response supplies code");
        expect(app.login_state == App::LoginState::ready, "received code exposes the ready state");
        expect(app.login_link == "link.stremio.com/CURRENT" && app.login_qr == "fixture-qr.rgba", "link and QR are exposed");
        app.login_close();
        expect(app.login_code.empty() && app.login_code_raw_.empty() && app.login_qr_url_.empty() &&
               app.login_qr.empty() && !app.login_polling_, "closing invalidates every login payload");
        auto failed = creates.add({false, {}, {}, "fixture unavailable"});
        app.login_start(); failed->release(); flush_tasks();
        expect(app.login_status.find("fixture unavailable") != std::string::npos && !app.signed_in(),
               "create failure is visible without authenticating");
        expect(app.login_state == App::LoginState::error && app.login_code.empty() && app.login_code_raw_.empty() &&
               app.login_qr.empty() && app.login_qr_url_.empty(), "create failure clears code and QR instead of showing loading");
        app.login_close();
    }
    static void pending_expired_empty(const std::string& directory) {
        App app; prepare(app, directory);
        active_code(app, "PENDING");
        auto pending = reads.add({false, {}, {}});
        app.login_poll(); pending->release(); flush_tasks();
        expect(!app.signed_in() && app.login_visible && !app.login_polling_, "unapproved link stays pending");
        const int calls = reads.calls.load();
        app.login_poll(); flush_tasks();
        expect(reads.calls == calls, "poll interval prevents immediate duplicate requests");
        app.login_next_poll_ = 0;
        app.login_expires_ = now_seconds() - 1;
        app.login_qr = "expired-fixture-qr.rgba";
        app.login_qr_url_ = "https://link.stremio.com/qr?data=EXPIRED_FIXTURE";
        app.login_poll();
        expect(reads.calls == calls && app.login_state == App::LoginState::expired,
               "expired code is stopped locally");
        expect(app.login_code.empty() && app.login_code_raw_.empty() && app.login_qr.empty() && app.login_qr_url_.empty(),
               "expired login clears display code, raw code and QR payloads");
        active_code(app, "EMPTY_AUTH");
        app.login_qr = "empty-auth-fixture-qr.rgba";
        app.login_qr_url_ = "https://link.stremio.com/qr?data=EMPTY_AUTH_FIXTURE";
        auto empty = reads.add(approved(""));
        app.login_poll(); empty->release(); flush_tasks();
        expect(!app.signed_in() && app.login_visible && app.login_status.find("Riprova") != std::string::npos,
               "approved response without authKey is rejected");
        expect(app.login_state == App::LoginState::error && app.login_code.empty() && app.login_code_raw_.empty() &&
               app.login_qr.empty() && app.login_qr_url_.empty(), "invalid approval clears code and QR and exposes an error");
        app.login_close();
    }
    static void stale_poll(const std::string& directory) {
        App app; prepare(app, directory);
        active_code(app, "OLD_POLL");
        auto old = reads.add(approved("STALE_AUTH"));
        app.login_poll(); until([&] { return old->entered.load(); }, "old poll", false);
        app.login_close(); active_code(app, "NEW_POLL");
        auto current = reads.add({false, {}, {}});
        app.login_poll(); old->release();
        until([&] { return current->entered.load(); }, "new poll in flight", false);
        g_tasks.drain();
        expect(app.login_polling_, "stale poll callback cannot release the current attempt's in-flight guard");
        expect(!app.signed_in() && app.login_code == "NEW_POLL", "stale approval cannot sign in a new attempt");
        const int calls = reads.calls.load();
        app.login_poll();
        expect(reads.calls == calls && g_tasks.pending() == 0, "stale callback cannot trigger an overlapping current poll");
        current->release(); flush_tasks();
        expect(!app.login_polling_, "current poll releases its own guard");
        app.login_close();
    }
    static void expiry_and_public_retry(const std::string& directory) {
        App app; prepare(app, directory);
        active_code(app, "EXPIRING_IN_FLIGHT");
        app.login_qr = "expiry-fixture-qr.rgba";
        app.login_qr_url_ = "https://link.stremio.com/qr?data=EXPIRING_IN_FLIGHT";
        app.login_expires_ = now_seconds() + 123;
        auto old = reads.add(approved("EXPIRED_APPROVAL"));
        app.login_poll(); until([&] { return old->entered.load(); }, "poll held across expiry", false);
        expect(app.login_seconds_remaining >= 122 && app.login_seconds_remaining <= 123,
               "countdown reflects the active code deadline");
        app.login_expires_ = now_seconds() - 1;
        app.login_poll();
        expect(app.login_state == App::LoginState::expired && app.login_seconds_remaining == 0 && !app.login_polling_,
               "expiry updates during an in-flight request and releases its guard");
        expect(app.login_code.empty() && app.login_code_raw_.empty() && app.login_qr.empty() && app.login_qr_url_.empty(),
               "in-flight expiry removes the previous code and QR");
        auto renewed = creates.add(code("RENEWED_CODE"));
        const int before = creates.calls.load();
        app.request_login_code();
        expect(app.login_state == App::LoginState::loading && app.login_visible && app.login_code.empty(),
               "public retry enters loading with no previous code");
        const size_t pending = g_tasks.pending();
        app.request_login_code(); app.on_button(Btn::Cross);
        expect(g_tasks.pending() == pending, "repeated retry and Cross while loading cannot enqueue duplicate requests");
        old->release(); until([&] { return renewed->entered.load(); }, "fresh code after expiry", false);
        g_tasks.drain();
        expect(!app.signed_in() && app.login_state == App::LoginState::loading && app.login_code.empty(),
               "approved response for expired code cannot authenticate or overwrite retry");
        renewed->release(); flush_tasks();
        expect(creates.calls == before + 1 && app.login_state == App::LoginState::ready && app.login_code == "RENEWED_CODE",
               "public retry invokes the real create boundary exactly once and shows its new code");
        expect(app.login_seconds_remaining == 300,
               "renewed code receives the application's five-minute pairing window");
        app.login_close();
        const int after = creates.calls.load();
        app.request_login_code(); flush_tasks();
        expect(creates.calls == after && !app.login_visible, "retry cannot reopen a dismissed login page");
    }
    static void late_profile_after_logout(const std::string& directory) {
        App app; prepare(app, directory);
        app.view = "settings"; app.nav_sel = 4;
        active_code(app, "ACCEPT");
        auto auth = reads.add(approved("ACCEPTED_AUTH"));
        auto user = users.add(profile("late@example.test"));
        auto logout = logouts.add({true, json::object(), {}});
        auto fresh = creates.add(code("AFTER_PROFILE_LOGOUT"));
        const int before_addons = addons_reload, before_library = library_reload;
        app.login_poll(); auth->release();
        until([&] { return user->entered.load(); }, "profile fetch after successful approval");
        expect(app.signed_in() && !app.login_visible && app.login_code.empty(), "valid approval signs in and closes link UI");
        expect(app.view == "home" && app.nav_sel == 0 && app.zone == "content", "successful approval returns from settings to Home content");
        expect(addons_reload == before_addons + 1 && library_reload == before_library + 1,
               "successful sign-in reloads account addons and library");
        expect(user->argument == "ACCEPTED_AUTH", "profile request receives the accepted session");
        app.sign_out();
        expect(app.login_visible && app.login_state == App::LoginState::loading && app.login_code.empty(),
               "logout opens a fresh login page while its new code loads");
        const auto removed = saved(directory);
        expect(jstr(removed, "auth_key").empty() && jstr(removed, "user_email").empty(),
               "logout removes persisted credentials before background work returns");
        user->release();
        until([&] { return logout->entered.load(); }, "logout request", false);
        g_tasks.drain();
        expect(!app.signed_in() && app.settings_.user_email.empty() && app.user.empty(),
               "late profile cannot restore identity after logout");
        expect(app.login_visible && app.login_state == App::LoginState::loading && app.login_code.empty(),
               "late profile cannot alter the new login page");
        expect(logout->argument == "ACCEPTED_AUTH", "remote logout uses the previous session only");
        logout->release(); fresh->release(); flush_tasks();
        expect(jstr(saved(directory), "auth_key").empty(), "late profile cannot restore persisted token");
        expect(app.login_visible && app.login_state == App::LoginState::ready && app.login_code == "AFTER_PROFILE_LOGOUT",
               "logout requests and displays a new login code");
        app.login_close();
    }
    static void late_poll_after_logout(const std::string& directory) {
        App app; prepare(app, directory);
        app.settings_.auth_key = "EXISTING_AUTH";
        active_code(app, "LATE_APPROVAL");
        auto poll = reads.add(approved("UNWANTED_NEW_AUTH"));
        auto logout = logouts.add({true, json::object(), {}});
        auto fresh = creates.add(code("AFTER_POLL_LOGOUT"));
        app.login_poll(); until([&] { return poll->entered.load(); }, "approval pending during logout", false);
        app.sign_out();
        expect(app.login_visible && !app.signed_in() && app.login_state == App::LoginState::loading,
               "logout replaces the previous pending login with a fresh loading page");
        poll->release(); until([&] { return logout->entered.load(); }, "logout after late approval", false);
        g_tasks.drain();
        expect(!app.signed_in() && app.login_code.empty() && app.login_state == App::LoginState::loading,
               "late approved poll cannot undo logout or mutate the new attempt");
        logout->release(); fresh->release(); flush_tasks();
        expect(app.login_visible && app.login_state == App::LoginState::ready && app.login_code == "AFTER_POLL_LOGOUT",
               "fresh login code survives an earlier approved poll");
        app.login_close();
    }
    static void logout_choices_and_stale_save(const std::string& directory) {
        App app; prepare(app, directory);
        std::string download_error;
        expect(app.downloads_.init(directory, &download_error), "logout download fixture initializes");
        DownloadRequest direct;
        direct.media_id = direct.video_id = "logout:direct"; direct.type = "movie"; direct.title = "Queued direct video";
        direct.stream.kind = StreamKind::Direct; direct.stream.url = "https://media.invalid/queued.mp4";
        const auto queued = app.downloads_.enqueue(direct, download_error);
        DownloadRequest torrent = direct;
        torrent.media_id = torrent.video_id = "logout:torrent"; torrent.title = "Waiting torrent video";
        torrent.stream.kind = StreamKind::Torrent; torrent.stream.url.clear(); torrent.stream.info_hash = std::string(40, 'a');
        const auto waiting = app.downloads_.enqueue(torrent, download_error);
        app.downloads_.set_torrent_playback_active(true);
        expect(!queued.empty() && !waiting.empty() && app.downloads_.find(queued)->state == DownloadState::Queued &&
               app.downloads_.find(waiting)->state == DownloadState::Waiting,
               "logout fixture has one queued direct source and one waiting torrent without starting network");
        app.settings_.auth_key = "CHOICE_AUTH"; app.settings_.user_email = "person@example.test";
        app.user = "P"; app.library_["one"] = json{{"_id", "one"}}; app.library_loaded_ = true;
        app.save_settings(); app.flush_settings(true); app.settings_refresh();
        expect(app.s_rows[0].label == "Esci dall'account", "signed-in settings clearly expose logout");
        app.s_sel = 0; app.settings_button(Btn::Cross);
        expect(app.dd_visible && app.dd_sel == 0 && app.dd_options[0].label == "Annulla", "logout confirmation defaults to cancel");
        app.dropdown_button(Btn::Cross);
        expect(app.signed_in() && !app.dd_visible, "confirming default cancel preserves the account");
        expect(app.downloads_.find(queued)->state == DownloadState::Queued && app.downloads_.find(waiting)->state == DownloadState::Waiting,
               "cancelling logout preserves queued and waiting downloads");
        expect(jstr(saved(directory), "auth_key") == "CHOICE_AUTH", "cancel preserves persisted account");
        app.settings_button(Btn::Cross); app.dropdown_button(Btn::Circle);
        expect(app.signed_in() && !app.dd_visible, "Circle cancels logout");

        auto blocked = std::make_shared<Gate<bool>>(true); gates.push_back(blocked);
        g_tasks.run([blocked] { blocked->call(); });
        until([&] { return blocked->entered.load(); }, "worker held before old settings save", false);
        app.save_settings(); app.flush_settings(false); // Old auth snapshot queued behind blocker.
        auto logout = logouts.add({false, {}, "fixture network failure"});
        auto fresh = creates.add(code("AFTER_CONFIRMED_LOGOUT"));
        app.settings_button(Btn::Cross); app.dropdown_button(Btn::Down); app.dropdown_button(Btn::Cross);
        expect(!app.signed_in() && app.settings_.user_email.empty() && app.user.empty() &&
               app.library_.empty() && !app.library_loaded_, "explicit logout clears account and private library");
        expect(app.login_visible && app.login_state == App::LoginState::loading && app.login_code.empty(),
               "confirmed logout goes directly to the fresh login page");
        expect(jstr(saved(directory), "auth_key").empty(), "logout persists immediately while old writer is blocked");
        expect(app.downloads_.find(queued)->state == DownloadState::Paused && app.downloads_.find(waiting)->state == DownloadState::Paused,
               "confirmed logout pauses queued and waiting sources instead of resuming them under a later account");
        blocked->release(); until([&] { return logout->entered.load(); }, "queued logout after stale settings writer", false);
        g_tasks.drain();
        expect(jstr(saved(directory), "auth_key").empty(), "old asynchronous writer cannot resurrect the token");
        logout->release(); fresh->release(); flush_tasks();
        expect(!app.signed_in() && jstr(saved(directory), "auth_key").empty(), "remote logout failure never restores the local session");
        expect(app.s_rows[0].label == "Account Stremio", "signed-out settings offer login again");
        expect(app.login_visible && app.login_state == App::LoginState::ready && app.login_code == "AFTER_CONFIRMED_LOGOUT",
               "failed remote logout does not prevent showing a fresh login code");
        app.login_close();
    }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    g_tasks.start(1);
    try {
        AppProtocolTest::startup_routes(argv[1]);
        AppProtocolTest::separate_device_session(argv[1]);
        AppProtocolTest::session_exchange_failures(argv[1]);
        AppProtocolTest::cancelled_device_session(argv[1], false);
        AppProtocolTest::cancelled_device_session(argv[1], true);
        AppProtocolTest::create_and_cancel(argv[1]);
        AppProtocolTest::pending_expired_empty(argv[1]);
        AppProtocolTest::stale_poll(argv[1]);
        AppProtocolTest::expiry_and_public_retry(argv[1]);
        AppProtocolTest::late_profile_after_logout(argv[1]);
        AppProtocolTest::late_poll_after_logout(argv[1]);
        AppProtocolTest::logout_choices_and_stale_save(argv[1]);
        expect(unexpected_http == 0, "no real HTTP requests escape the controlled account boundaries");
        expect(qr_requests > 0 && creates.calls > 0 && reads.calls > 0 && users.calls > 0 && logouts.calls > 0 && exchanges.calls >= 5,
               "every account boundary is exercised");
        flush_tasks(); g_tasks.stop();
        std::cout << "ACCOUNT_TESTS_OK checks=" << checks << " real_http_requests=" << unexpected_http.load() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ACCOUNT_TESTS_FAILED: " << error.what() << '\n';
        for (auto& weak : gates) if (auto gate = weak.lock()) gate->release();
        g_tasks.stop();
        return 1;
    }
}
