// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

class DisplayRefresh {
public:
    // Call between frames, with the renderer's GPU-idle callback. Preferences:
    // 0 preserves the system mode, 1 automatic, 2 playback only.
    bool update(int preference, bool playing, void (*wait_idle)());
    void restore(void (*wait_idle)());
    bool available() const { return available_; }
    bool active() const { return active_; }
    bool safe() const { return safe_; }
    std::uint64_t refresh_code() const { return refresh_code_; }
    const char* status() const { return status_; }
private:
    int handle_ = -1;
    int attempted_ = -1;
    bool available_ = false, active_ = false, owned_ = false, rejected_ = false, safe_ = true;
    std::uint64_t baseline_code_ = 0, refresh_code_ = 0;
    const char* status_ = "system";
};
