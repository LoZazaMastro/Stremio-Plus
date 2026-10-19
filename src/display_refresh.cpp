// SPDX-License-Identifier: GPL-3.0-or-later
// Output-mode ABI and the 119.88 Hz mode follow sainsaji's EVO Player PS5.
// Adapted for a Mesa-owned display; dimensions are verified before presenting.
#include "display_refresh.h"
#include "util.h"
#include <cstddef>

#ifdef PLATFORM_PS5_NATIVE
namespace {
struct ResolutionStatus {
    std::uint32_t full_width, full_height, pane_width, pane_height;
    std::uint64_t refresh;
    float inches;
    std::uint32_t reserved[4];
};
static_assert(sizeof(ResolutionStatus) == 48);
static_assert(offsetof(ResolutionStatus, refresh) == 16);
constexpr std::uint64_t kHighRefreshMode = 0xf, kDefaultMode = 1, kHighRefreshCode = 13;
bool dimensions_match(const ResolutionStatus& a, const ResolutionStatus& b) {
    return a.full_width && a.full_height && a.full_width == b.full_width && a.full_height == b.full_height;
}
}
extern "C" {
int stremio_videoout_handle(void);
int sceVideoOutIsOutputSupported(int, std::uint64_t, const void*, const void*, std::uint64_t);
int sceVideoOutConfigureOutput(int, std::uint64_t, const void*, const void*, std::uint64_t);
int sceVideoOutGetResolutionStatus(int, void*);
}
#endif

bool DisplayRefresh::update(int preference, bool playing, void (*wait_idle)()) {
#ifdef PLATFORM_PS5_NATIVE
    const int current_handle = stremio_videoout_handle();
    if (current_handle < 0) return false;
    bool changed = false;
    if (current_handle != handle_) {
        handle_ = current_handle;
        attempted_ = -1;
        owned_ = rejected_ = false;
        safe_ = true;
        ResolutionStatus initial{};
        const int query = sceVideoOutGetResolutionStatus(handle_, &initial);
        if (query != 0 || !initial.full_width || !initial.full_height) {
            available_ = active_ = false;
            attempted_ = 0;
            status_ = "query_unavailable";
            dlog("display refresh: initial readback unavailable rc=0x%08x", unsigned(query));
            return true;
        }
        baseline_code_ = refresh_code_ = initial.refresh;
        active_ = initial.refresh == kHighRefreshCode;
        const int supported = sceVideoOutIsOutputSupported(handle_, kHighRefreshMode, nullptr, nullptr, 0);
        available_ = supported >= 0 || active_;
        status_ = available_ ? "system" : "unsupported_display";
        dlog("display refresh: probe rc=0x%08x code=%llu dimensions=%ux%u supported=%d",
             unsigned(supported), static_cast<unsigned long long>(refresh_code_), initial.full_width,
             initial.full_height, int(available_));
        changed = true;
    }
    const bool wanted = preference == 1 || (preference == 2 && playing);
    if (attempted_ == int(wanted)) return changed;
    attempted_ = int(wanted);
    if ((wanted && (!available_ || rejected_ || active_)) || (!wanted && !owned_)) return changed;

    ResolutionStatus before{};
    if (sceVideoOutGetResolutionStatus(handle_, &before) != 0 || !before.full_width || !before.full_height) {
        status_ = "query_unavailable";
        return true;
    }
    if (wait_idle) wait_idle();
    const auto restore_mode = baseline_code_ == kHighRefreshCode ? kHighRefreshMode : kDefaultMode;
    const int configure = sceVideoOutConfigureOutput(handle_, wanted ? kHighRefreshMode : restore_mode,
                                                     nullptr, nullptr, 0);
    ResolutionStatus after{};
    const int query = sceVideoOutGetResolutionStatus(handle_, &after);
    const bool rate_matches = query == 0 && (wanted ? after.refresh == kHighRefreshCode
        : (baseline_code_ == kHighRefreshCode ? after.refresh == kHighRefreshCode : after.refresh != kHighRefreshCode));
    if (configure == 0 && rate_matches && dimensions_match(before, after)) {
        owned_ = wanted;
        active_ = after.refresh == kHighRefreshCode;
        refresh_code_ = after.refresh;
        status_ = wanted ? "119.88_hz" : "system";
        dlog("display refresh: applied mode=%s code=%llu dimensions=%ux%u",
             status_, static_cast<unsigned long long>(after.refresh), after.full_width, after.full_height);
        return true;
    }

    // Some HDMI paths accept the request by reducing resolution. Restore the
    // original mode before Mesa presents buffers registered for another size.
    const auto previous_mode = before.refresh == kHighRefreshCode ? kHighRefreshMode : kDefaultMode;
    const int rollback = sceVideoOutConfigureOutput(handle_, previous_mode, nullptr, nullptr, 0);
    ResolutionStatus restored{};
    const int restored_query = sceVideoOutGetResolutionStatus(handle_, &restored);
    safe_ = rollback == 0 && restored_query == 0 && dimensions_match(before, restored);
    active_ = safe_ && restored.refresh == kHighRefreshCode;
    refresh_code_ = safe_ ? restored.refresh : 0;
    if (wanted) rejected_ = true;
    status_ = safe_ ? "mode_refused" : "restore_failed";
    dlog("display refresh: request refused configure=0x%08x query=0x%08x code=%llu dimensions=%ux%u "
         "rollback=0x%08x restored=%d",
         unsigned(configure), unsigned(query), static_cast<unsigned long long>(after.refresh),
         after.full_width, after.full_height, unsigned(rollback), int(safe_));
    return true;
#else
    (void)preference; (void)playing; (void)wait_idle;
    return false;
#endif
}

void DisplayRefresh::restore(void (*wait_idle)()) {
    attempted_ = -1;
    update(0, false, wait_idle);
}
