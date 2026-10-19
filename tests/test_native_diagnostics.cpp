// Exercise the native emergency hooks in real, isolated host processes.
#include "diagnostics.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

static thread_local bool emergency_path;
static void disallow_runtime_services() { if (emergency_path) _exit(120); }

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __real_pthread_mutex_trylock(pthread_mutex_t*);
int __real_vfprintf(FILE*, const char*, va_list);
int __real_fflush(FILE*);
int __real_vsnprintf(char*, size_t, const char*, va_list);
__attribute__((noreturn)) void stremio_native_assert(const char*, const char*, int, const char*) __asm__("__assert");
// mkstemps can be retained by host libc; its counter provides fixture uniqueness.
uint64_t sceKernelGetProcessTime(void) { disallow_runtime_services(); return 100000; }

void* __wrap_malloc(size_t n) { disallow_runtime_services(); return __real_malloc(n); }
void* __wrap_calloc(size_t n, size_t size) { disallow_runtime_services(); return __real_calloc(n, size); }
void* __wrap_realloc(void* p, size_t n) { disallow_runtime_services(); return __real_realloc(p, n); }
void __wrap_free(void* p) { disallow_runtime_services(); __real_free(p); }
int __wrap_pthread_mutex_lock(pthread_mutex_t* p) { disallow_runtime_services(); return __real_pthread_mutex_lock(p); }
int __wrap_pthread_mutex_trylock(pthread_mutex_t* p) { disallow_runtime_services(); return __real_pthread_mutex_trylock(p); }
int __wrap_vfprintf(FILE* p, const char* format, va_list args) {
    disallow_runtime_services(); return __real_vfprintf(p, format, args);
}
int __wrap_fprintf(FILE* p, const char* format, ...) {
    disallow_runtime_services(); va_list args; va_start(args, format);
    const int result = __real_vfprintf(p, format, args); va_end(args); return result;
}
int __wrap_fflush(FILE* p) { disallow_runtime_services(); return __real_fflush(p); }
int __wrap_vsnprintf(char* p, size_t n, const char* format, va_list args) {
    disallow_runtime_services(); return __real_vsnprintf(p, n, format, args);
}
int __wrap_snprintf(char* p, size_t n, const char* format, ...) {
    disallow_runtime_services(); va_list args; va_start(args, format);
    const int result = __real_vsnprintf(p, n, format, args); va_end(args); return result;
}
}

void* operator new(size_t n) {
    disallow_runtime_services();
    if (void* p = __real_malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { disallow_runtime_services(); __real_free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const struct rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    if (!diagnostics_start(argv[1]) || !diagnostics_flush()) return 3;
    void* fault_page = nullptr;
    if (std::strcmp(argv[2], "segv") == 0) {
        fault_page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (fault_page == MAP_FAILED) return 4;
        std::printf("expected_fault=%p\n", fault_page);
        std::fflush(stdout);
    }
    emergency_path = true;
    if (std::strcmp(argv[2], "immediate_exit") == 0) std::_Exit(37);
    if (std::strcmp(argv[2], "abort") == 0) std::abort();
    if (std::strcmp(argv[2], "assert") == 0)
        stremio_native_assert("render_frame", "/private/build/source/video.c", 217, "frame != NULL && width > 0");
    if (std::strcmp(argv[2], "assert_privacy") == 0)
        stremio_native_assert("check_input", "/private/build/source/input.c", 42,
                 "strcmp(url, \"https://private.example/secret\") == 0");
    if (std::strcmp(argv[2], "signal_abort") == 0) raise(SIGABRT);
    if (std::strcmp(argv[2], "segv") == 0) *static_cast<volatile int*>(fault_page) = 1;
    if (std::strcmp(argv[2], "allocation") == 0) {
        errno = EDOM;
        stremio_diagnostics_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED, 1024, 256, 768, 256, 0x1234);
        if (errno != EDOM) _exit(121);
        for (unsigned i = 0; i < 16; ++i) {
            stremio_diagnostics_allocation_failure(STREMIO_ALLOC_MALLOC, 0x100000 + i, 32, 0x5678 + i);
            if (errno != EDOM) _exit(122);
        }
    } else return 5;
    emergency_path = false;
    diagnostics_stop();
    return 0;
}
