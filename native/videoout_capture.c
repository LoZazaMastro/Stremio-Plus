// SPDX-License-Identifier: GPL-3.0-or-later
// Observe the renderer-owned output handle without opening a second display.
#include <stdatomic.h>
#include <stdint.h>

static atomic_int output_handle = ATOMIC_VAR_INIT(-1);
extern int __real_sceVideoOutOpen(int32_t, int32_t, int32_t, const void*);
extern int __real_sceVideoOutClose(int32_t);

int stremio_videoout_handle(void) { return atomic_load(&output_handle); }

int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* parameters) {
    const int handle = __real_sceVideoOutOpen(user, bus, index, parameters);
    if (handle >= 0) atomic_store(&output_handle, handle);
    return handle;
}

int __wrap_sceVideoOutClose(int32_t handle) {
    const int result = __real_sceVideoOutClose(handle);
    if (result == 0) {
        int expected = handle;
        atomic_compare_exchange_strong(&output_handle, &expected, -1);
    }
    return result;
}
