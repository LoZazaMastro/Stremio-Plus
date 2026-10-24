// Stremio - account linking and text entry state.
#include <algorithm>
#include <cmath>
#include "app.h"
#include "account_link_session.h"

// ---------------------------------------------------------------------------
// Sign in with a code

namespace {
// The Link API has no TTL field. This is the application's own pairing
// window; an expired screen cannot accept a late approval from its old poll.
constexpr int kLinkWindowSeconds = 5 * 60;

void discard_link_session(const account_link::Result& result) {
    // A cancelled/expired login must not leave a newly created device session
    // behind. Never revoke the credential originally returned by link/read.
    if (result.stage != account_link::Stage::Ready || !result.separately_issued) return;
    const auto key = result.key;
    g_tasks.run([key]() { api_logout(key); });
}
}

void App::request_login_code() {
	if (login_visible && login_state != LoginState::loading) login_start();
}

void App::login_start() {
	int gen = ++login_gen_;
	login_visible = true;
	login_state = LoginState::loading;
	login_seconds_remaining = 0;
	login_code.clear();
	login_code_raw_.clear();
	login_qr_url_.clear();
	login_qr.clear();
	login_link = "link.stremio.com";
	login_status = "Loading...";
	login_polling_ = false;
	dirty_all();
	bg<LinkCode>([]() { return link_create(); },
	             [this, gen](LinkCode& lc) {
		             if (gen != login_gen_ || !login_visible) return;
		             if (!lc.ok) {
			             login_state = LoginState::error;
			             login_status = "Could not get a code: " + lc.error;
			             dirty_all();
			             return;
		             }
		             login_state = LoginState::ready;
		             login_code_raw_ = lc.code;
		             login_code = lc.code;
		             std::string shown = lc.link;
		             if (starts_with(shown, "https://")) shown = shown.substr(8);
		             login_link = shown;
		             login_qr_url_ = "https://link.stremio.com/qr?data=" + url_encode(lc.link);
		             login_qr = art(login_qr_url_, ArtKind::Qr);
		             login_status = "Waiting for you to approve the sign-in on your phone or computer...";
		             login_next_poll_ = now_seconds() + 3;
		             login_seconds_remaining = kLinkWindowSeconds;
		             login_expires_ = now_seconds() + kLinkWindowSeconds;
		             dirty_all();
	             });
}

void App::login_poll() {
	if (!login_visible || login_state != LoginState::ready) return;
	const double now = now_seconds();
	const int remaining = std::max(0, static_cast<int>(std::ceil(login_expires_ - now)));
	if (remaining != login_seconds_remaining) {
		login_seconds_remaining = remaining;
		dirty("login_seconds_remaining");
	}
	if (now >= login_expires_) {
		++login_gen_; // Discard an approval still in flight for the expired code.
		login_polling_ = false;
		login_state = LoginState::expired;
		login_status = "The sign-in code expired. Request a new link.";
		login_code_raw_.clear();
		login_code.clear();
		login_qr_url_.clear();
		login_qr.clear();
		dirty_all();
		return;
	}
	if (login_qr.empty() && !login_qr_url_.empty()) {
		// The QR image may have arrived since.
		std::string q = art(login_qr_url_, ArtKind::Qr, false);
		if (!q.empty()) {
			login_qr = q;
			dirty("login_qr");
		}
	}
	if (login_code_raw_.empty() || login_polling_ || now < login_next_poll_) return;
	login_polling_ = true;
	int gen = login_gen_;
	std::string code = login_code_raw_;
	g_tasks.run<account_link::Result>(
	    [code]() {
		    const auto approval = link_read(code);
		    return account_link::complete(approval.ok, jstr(approval.result, "authKey"),
		        [](const std::string& token) {
			        // This runs on the worker, not the UI thread. This is the
			        // official account login step after device-code approval.
			        const auto session = api_call("loginWithToken",
			            json{{"type", "LoginWithToken"}, {"token", token}});
			        return account_link::SessionReply{session.ok, jstr(session.result, "authKey")};
		        });
	    },
	    [this, gen](account_link::Result& result) {
		    if (gen != login_gen_ || !login_visible || login_state != LoginState::ready) {
			    discard_link_session(result);
			    return;
		    }
		    if (now_seconds() >= login_expires_) {
			    discard_link_session(result);
			    login_poll();
			    return;
		    }
		    login_polling_ = false;
		    login_next_poll_ = now_seconds() + 3;
		    if (result.stage == account_link::Stage::Ready) login_done(result.key);
		    else if (result.stage == account_link::Stage::Failed) {
			    // A failed exchange is not another pending approval. Clear the
			    // consumed code and offer retry, without exposing server tokens.
			    login_done("");
		    }
	    });
}

void App::login_close() {
	login_gen_++;
	login_visible = false;
	login_state = LoginState::loading;
	login_seconds_remaining = 0;
	login_polling_ = false;
	login_code_raw_.clear();
	login_code.clear();
	login_qr_url_.clear();
	login_qr.clear();
	dirty_all();
}

void App::login_done(const std::string& auth_key) {
	if (auth_key.empty()) {
		login_state = LoginState::error;
		login_seconds_remaining = 0;
		login_code_raw_.clear();
		login_code.clear();
		login_qr_url_.clear();
		login_qr.clear();
		login_status = ui_language == "it" ? "Accesso non completato. Riprova." : "Sign-in was not completed. Please try again.";
		dirty_all();
		return;
	}
	login_close();
	++account_generation_;
	if (download_art_cancel_) download_art_cancel_->store(true);
	if (preview_metadata_cancel_) preview_metadata_cancel_->store(true);
	++preview_metadata_generation_;
	preview_metadata_.clear();
	preview_metadata_key_.clear();
	preview_metadata_loading_ = false;
	++library_revision_;
	library_pending_changes_.clear();
	library_original_changes_.clear();
	library_write_inflight_ = false;
	library_refresh_pending_ = false;
	settings_.auth_key = auth_key;
	downloads_.set_enabled(!offline_);
	save_settings();
	// Persist the device session before exposing the signed-in screen, instead
	// of relying on the ordinary settings debounce after the browser closes.
	flush_settings(true);
	show_toast(ui_language == "it" ? "Accesso effettuato" : "Signed in");
	const int account_gen = account_generation_;
	bg<ApiResult>([auth_key]() { return api_get_user(auth_key); },
	              [this, account_gen](ApiResult& r) {
		              if (account_gen != account_generation_) return;
		              if (!r.ok) return;
		              settings_.user_email = jstr(r.result, "email");
		              user = settings_.user_email.empty() ? "" : std::string(1, char(toupper((unsigned char)settings_.user_email[0])));
		              save_settings();
		              settings_refresh();
		              show_toast("Signed in as " + settings_.user_email);
	              });
	library_.clear();
	library_loaded_ = false;
	library_initial_settled_ = false;
	load_addons();
	load_library();
	settings_refresh();
	set_view("home");
	zone = "content";
}

void App::sign_out() {
	if (next_episode_visible && watching_) watch_stop(true);
	else watch_cancel_next_episode();
	login_close(); // Invalidate every pending code/poll before removing this session.
	++account_generation_;
	if (download_art_cancel_) download_art_cancel_->store(true);
	d_meta_pending_ = false;
	d_pending_video_id_.clear();
	d_stream_source_names_.clear();
	d_stream_source_urls_.clear();
	d_source_preferred_.clear();
	d_seasons.clear();
	d_season_sel = 0;
	if (preview_metadata_cancel_) preview_metadata_cancel_->store(true);
	++preview_metadata_generation_;
	preview_metadata_.clear();
	preview_metadata_key_.clear();
	preview_metadata_loading_ = false;
	++library_revision_;
	library_pending_changes_.clear();
	library_original_changes_.clear();
	library_write_inflight_ = false;
	library_refresh_pending_ = false;
	std::string key = settings_.auth_key;
	if (!key.empty()) g_tasks.run([key]() { api_logout(key); });
	settings_.auth_key.clear();
	downloads_.set_enabled(false);
	// Keep queued sources from resuming automatically under a later account.
	for (const auto& entry : downloads_.snapshot())
		if (entry.state == DownloadState::Queued || entry.state == DownloadState::Waiting)
			downloads_.pause(entry.id);
	settings_.user_email.clear();
	user.clear();
	library_.clear();
	library_loaded_ = false;
	library_initial_settled_ = false;
	save_settings();
	// Session removal must not wait for the ordinary settings debounce. The
	// epoch also prevents an older settings writer from restoring the token.
	flush_settings(true);
	show_toast(ui_language == "it" ? "Account disconnesso" : "Signed out");
	load_addons();
	settings_refresh();
	login_start();
}

// ---------------------------------------------------------------------------
// Text entry

// Keyboard state is rendered by ps5-homebrew-ui on the same OpenGL surface.
void App::open_input(const std::string& title, const std::string& value, const std::string& hint,
                     std::function<void(const std::string&)> done) {
    input_title = title;
    input_hint = hint;
    input_value = value;
    input_done_ = std::move(done);
    input_visible_ = true;
    input_opened_ = now_seconds();
    dirty_all();
}

void App::input_finish(bool ok) {
    input_visible_ = false;
    auto fn = std::move(input_done_);
    input_done_ = nullptr;
    const std::string value = input_value;
    dirty_all();
    if (ok && fn) fn(value);
}

void App::input_submit() { if (input_visible_) input_finish(true); }
void App::input_poll() {}
