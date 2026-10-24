// Stremio Plus - completion of device-code authentication.
// Copyright (C) 2026 LoZazaMastro and contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <utility>

namespace account_link {
enum class Stage { Pending, Failed, Ready };
struct SessionReply {
    bool ok = false;
    std::string key;
};
struct Result {
    Stage stage = Stage::Pending;
    std::string key;
    // Only a distinct newly issued credential can be revoked on cancellation
    // without risking revocation of the browser's approval credential.
    bool separately_issued = false;
};

// Link approval is not the session to persist. Exchange it with the account
// loginWithToken API, and accept only the returned session credential. Network
// errors never fall back to storing the approval token.
template<class Exchange>
Result complete(bool approved, const std::string& approval, Exchange&& exchange) {
    if (!approved) return {};
    Result result;
    result.stage = Stage::Failed;
    if (approval.empty()) return result;
    SessionReply session = std::forward<Exchange>(exchange)(approval);
    if (!session.ok || session.key.empty()) return result;
    result.stage = Stage::Ready;
    result.separately_issued = session.key != approval;
    result.key = std::move(session.key);
    return result;
}
} // namespace account_link
