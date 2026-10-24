> Historical source-only snapshot, retained from the earlier attachment.
> For the subsequent implementation and tests, see [LOCAL_0_5_19_VALIDATION.md](LOCAL_0_5_19_VALIDATION.md).

# Post-0.5.18 work in progress

This is an UNBUILT source-only work snapshot, not a release. No PS5 package
has been produced from these changes, and no tests have been executed for
this snapshot. Do not relabel the 0.5.18 binary as a new build.

## Implemented source change: device login session

The previous `App::login_poll` passed `link/read`'s approval credential
directly to `login_done`. This snapshot exchanges it through
`https://api.strem.io/api/loginWithToken`, using `type: LoginWithToken` and
`token`, and persists only the response's `authKey`.

The exchange runs on the existing worker queue. A pending approval keeps
polling. A failed exchange clears the consumed code and exposes the existing
retry screen; it never falls back to saving the browser credential. Stale
and expired callbacks cannot authenticate another attempt. A distinct newly
issued session is revoked on cancellation; an ambiguous same-key response
is never revoked as a cleanup action. Successful login forces the existing
settings writer to run synchronously instead of waiting for its debounce.

This change follows the official Stremio request and authentication test:
- https://github.com/Stremio/stremio-core/blob/development/src/types/api/request.rs
- https://github.com/Stremio/stremio-core/blob/development/src/unit_tests/ctx/authenticate.rs

Existing stored credentials are not silently replaced. After installing an
actually built and verified update, an installation paired with the older
flow needs a fresh sign-in. The real sequence (pair, log out in the phone
browser, refresh account data, restart PS5 app) still needs live validation.
This snapshot does not claim that server-side revocation behavior has been
verified.

## Tests supplied, NOT executed

The pure credential-selection test does not access the network:

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -Isrc \
  tests/test_account_link_session.cpp -o /tmp/stremio-account-link-test
/tmp/stremio-account-link-test
```

After rebuilding the host app, `tests/run_account_tests.py BUILD_DIR` uses
an additional offline wrapper for `api_call`. The existing App/Tasks harness
must still be run to check login cancellation, expiry and persistence. Its
fixtures retain their original identifiers; the separate pure test covers
distinct browser and device credentials. Neither test establishes behavior
on an actual Stremio account or PS5.

## Still open: startup presentation

`refresh_home_cards()` reconstructs the row mapping on catalog completion,
and `refresh_library_membership()` can trigger a second rebuild when the
account snapshot arrives. A staged initial model plus a bounded readiness
gate is required. Merely adding a fixed delay or waiting for every provider
would hide symptoms or stall on slow/unavailable add-ons. Keep rendering
behind the startup overlay until the leading shelves and initial artwork
settle, then reveal once. Preserve catalog order and selection as later rows
arrive. This behavior is NOT implemented in this snapshot.

## Still open: right-stick playback volume

Confirmed control: right stick UP increases media volume, DOWN decreases it.
Do not reassign L2/R2 (subtitle delay during playback, gallery sorting in
Downloads). Add dead zone/hysteresis, bounded repeat, a temporary percentage
indicator, clamping and persistence. Do not change navigation focus while
adjusting volume. PCM gain must be applied in the player audio path; bitstream
passthrough must not display a fictitious software gain. This behavior is NOT
implemented in this snapshot.

## Still open: 1080p microstutter on firmware 12.7

The report does not establish which decoder was active. No new log from the
reported 12.7 console is available here. Preserve native memory, audio-lifetime
and texture-safety protections. Collect the actual decoder selection/fallback,
source frame rate, frame presentation timing and output cadence before claiming
a hardware-decoder fix. No decoder or native driver change is included here.

## Build blocker

The editing session's container and Python services fail even for a minimal
command (ClientError). This is not evidence of a compiler error in the app.
The snapshot has only been reviewed as source; host compilation, sanitizers,
native compilation and package verification remain outstanding. The published
0.5.18 release, main branch and catalog must remain unchanged by this snapshot.
