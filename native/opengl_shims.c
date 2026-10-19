// Stremio Plus - Native-title runtime hooks required by ps5-opengl.
// Adapted from ps5-homebrew-ui, Copyright (C) 2026 BlackBearReloaded.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/diagnostics_native.h"

extern int sceKernelUsleep(uint32_t microseconds);
extern uint64_t sceKernelGetProcessTime(void);
extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
extern int __real_sceSystemServiceHideSplashScreen(void);

/* Keep the application's existing logger and direct-memory heap. Linking the kit's
 * complete runtime file would install a second logger and duplicate libc hooks. */
void stremio_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void stremio_glapi_tls_context_init(void) {}

__attribute__((noreturn)) void catchReturnFromMain(int status)
{
    fprintf(stderr, "[Stremio Plus] main returned %d; requesting system exit\n", status);
    fflush(NULL);
    (void)sceSystemServiceLoadExec("exit", NULL);
    /* exit()/return reaches the native crash reporter; let the system close it. */
    for (;;) sceKernelUsleep(100000);
}

extern __attribute__((noreturn)) void __real_abort(void);
extern __attribute__((noreturn)) void __real__Exit(int);

__attribute__((noreturn, noinline)) void __wrap__Exit(int status)
{
    const uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    if (status != 0) stremio_diagnostics_native_exit(status, caller);
    __real__Exit(status);
}

__attribute__((noreturn, noinline)) void __wrap_abort(void)
{
    const uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    stremio_diagnostics_abort(caller);
    __real_abort();
}

__attribute__((noreturn, noinline)) void __assert(const char *function, const char *file, int line,
                                        const char *expression)
{
    const uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    stremio_diagnostics_assert(function, file, line, expression, caller);
    abort();
}

/* The SDK's mkstemp import belongs to libScePosixForWebKit, which native
 * media titles do not load. O_EXCL keeps concurrent shader-cache writes safe. */
int mkstemps(char *template_name, int suffix_length)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned counter;
    const size_t length = template_name ? strlen(template_name) : 0;
    if (suffix_length < 0 || length < (size_t)suffix_length + 6u) {
        errno = EINVAL;
        return -1;
    }
    char *name = template_name + length - (size_t)suffix_length - 6u;
    for (int i = 0; i < 6; ++i) {
        if (name[i] != 'X') { errno = EINVAL; return -1; }
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        uint64_t value = sceKernelGetProcessTime() +
            (uint64_t)__atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED) * UINT64_C(0x9E3779B97F4A7C15);
        for (int i = 0; i < 6; ++i) { name[i] = letters[value % 36u]; value /= 36u; }
        const int descriptor = open(template_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (descriptor >= 0 || errno != EEXIST) return descriptor;
    }
    errno = EEXIST;
    return -1;
}

static int splash_released;
void hui_release_splash(void) { splash_released = 1; }
int __wrap_sceSystemServiceHideSplashScreen(void)
{
    return splash_released ? __real_sceSystemServiceHideSplashScreen() : 0;
}
