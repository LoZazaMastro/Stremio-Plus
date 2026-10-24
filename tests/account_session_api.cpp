// Additional offline boundary for the existing App/Tasks account harness.
#include "stremio.h"
#include <stdexcept>

// Tests with browser/device credentials control the real worker boundary,
// including a response held across cancellation or local expiry.
ApiResult account_test_session_exchange(const std::string& token);

ApiResult wrapped_account_api(const std::string&, json) asm(WRAP_API_CALL);
ApiResult wrapped_account_api(const std::string& method, json body) {
    if (method != "loginWithToken" || jstr(body, "type") != "LoginWithToken" ||
        !body.contains("token") || !body["token"].is_string() || body["token"].get<std::string>().empty())
        throw std::runtime_error("Unexpected account session exchange");
    const std::string token = body["token"].get<std::string>();
    if (token.starts_with("BROWSER_")) return account_test_session_exchange(token);
    // Legacy fixtures also exercise the ambiguous same-key response. This
    // mock does not establish actual server-side session independence.
    return {true, json{{"authKey", token}}, {}};
}
