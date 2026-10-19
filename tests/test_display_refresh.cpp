// SPDX-License-Identifier: GPL-3.0-or-later
#include "display_refresh.h"
#include <cassert>
#include <cstdint>
#include <cstring>

struct Status {
    std::uint32_t width = 3840, height = 2160, pane_width = 3840, pane_height = 2160;
    std::uint64_t refresh = 3;
    float inches = 55;
    std::uint32_t reserved[4]{};
};
static Status state;
static int support = 0, calls = 0, idle = 0, queries = 0;
static bool downsize = false, refuse = false, rollback_fail = false, query_fail = false;
void dlog(const char*, ...) {}
extern "C" int stremio_videoout_handle() { return 4; }
extern "C" int sceVideoOutIsOutputSupported(int h, std::uint64_t mode, const void*, const void*, std::uint64_t) {
    assert(h == 4 && mode == 0xf); return support;
}
extern "C" int sceVideoOutGetResolutionStatus(int h, void* status) {
    assert(h == 4); ++queries;
    if (query_fail) return -1;
    std::memcpy(status, &state, sizeof(state)); return 0;
}
extern "C" int sceVideoOutConfigureOutput(int h, std::uint64_t mode, const void*, const void*, std::uint64_t) {
    assert(h == 4 && (mode == 1 || mode == 0xf)); ++calls;
    if (mode == 0xf) {
        if (refuse) return -1;
        state.refresh = 13;
        if (downsize) { state.width = 1920; state.height = 1080; }
    } else {
        if (rollback_fail) return -1;
        state = Status{};
    }
    return 0;
}
static void wait_idle() { ++idle; }
static void reset() {
    state = Status{};
    support = calls = idle = queries = 0;
    downsize = refuse = rollback_fail = query_fail = false;
}

int main() {
    {
        reset(); DisplayRefresh display;
        display.update(0, false, wait_idle);
        assert(display.available() && !display.active() && calls == 0);
        display.update(2, false, wait_idle);
        assert(calls == 0);
        display.update(2, true, wait_idle);
        assert(display.active() && display.safe() && calls == 1 && idle == 1);
        for (int i = 0; i < 1000; ++i) display.update(2, true, wait_idle);
        assert(calls == 1);
        display.update(2, false, wait_idle);
        assert(!display.active() && calls == 2);
        display.update(1, false, wait_idle);
        assert(display.active());
        display.restore(wait_idle);
        assert(!display.active() && display.safe());
    }
    {
        reset(); support = -1; DisplayRefresh display;
        display.update(1, true, wait_idle);
        assert(!display.available() && calls == 0);
    }
    {
        reset(); downsize = true; DisplayRefresh display;
        display.update(1, true, wait_idle);
        assert(display.safe() && !display.active() && state.width == 3840 && calls == 2);
        display.update(0, false, wait_idle);
        display.update(1, true, wait_idle);
        assert(calls == 2);
    }
    {
        reset(); refuse = true; DisplayRefresh display;
        display.update(1, true, wait_idle);
        assert(display.safe() && !display.active() && calls == 2);
    }
    {
        reset(); downsize = rollback_fail = true; DisplayRefresh display;
        display.update(1, true, wait_idle);
        assert(!display.safe());
    }
    {
        reset(); state.refresh = 13; DisplayRefresh display;
        display.update(1, true, wait_idle);
        display.restore(wait_idle);
        assert(display.active() && calls == 0);
    }
    {
        reset(); query_fail = true; DisplayRefresh display;
        for (int i = 0; i < 1000; ++i) display.update(1, false, wait_idle);
        assert(queries == 1 && calls == 0 && !display.available());
    }
}
