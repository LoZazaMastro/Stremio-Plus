// Pure policy tests. No Stremio account, network, SDL or PS5 SDK is required.
#include "account_link_session.h"
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
int main() {
    using namespace account_link;
    try {
        int exchanges = 0;
        auto issue = [&](const std::string& approval) {
            ++exchanges;
            check(approval == "browser-credential", "approval passed to exchange");
            return SessionReply{true, "device-credential"};
        };
        auto pending = complete(false, "", issue);
        check(pending.stage == Stage::Pending && pending.key.empty() && exchanges == 0,
              "pending approval does not authenticate");
        auto missing = complete(true, "", issue);
        check(missing.stage == Stage::Failed && exchanges == 0,
              "empty approval fails without remote exchange");
        auto connected = complete(true, "browser-credential", issue);
        check(exchanges == 1 && connected.stage == Stage::Ready && connected.separately_issued &&
              connected.key == "device-credential", "only exchanged device credential is returned");
        // Simulated server revocation: this tests our choice of credential,
        // not the production Stremio server's revocation semantics.
        const std::string revoked = "browser-credential";
        check(connected.key != revoked, "browser credential is not the saved session");
        auto failed = complete(true, "browser-credential", [](const std::string&) {
            return SessionReply{false, "untrusted-key"};
        });
        check(failed.stage == Stage::Failed && failed.key.empty() && !failed.separately_issued,
              "failed exchange never falls back to the browser credential");
        auto empty = complete(true, "browser-credential", [](const std::string&) {
            return SessionReply{true, ""};
        });
        check(empty.stage == Stage::Failed && empty.key.empty(), "missing returned key is rejected");
        auto ambiguous = complete(true, "same-key", [](const std::string& key) {
            return SessionReply{true, key};
        });
        check(ambiguous.stage == Stage::Ready && !ambiguous.separately_issued,
              "ambiguous same-key reply cannot authorize browser-session revocation on cancellation");
        std::cout << "account-link-session: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "account-link-session: FAIL: " << error.what() << '\n';
        return 1;
    }
}
