// Native allocator wrappers with a host-backed direct-memory arena.
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "diagnostics_native.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void* __wrap_malloc(size_t);
void __wrap_free(void*);
void* __wrap_calloc(size_t, size_t);
void* __wrap_realloc(void*, size_t);
void* __wrap_reallocf(void*, size_t);
int __wrap_posix_memalign(void**, size_t, size_t);
void* __wrap_memalign(size_t, size_t);
void* __wrap_aligned_alloc(size_t, size_t);
void* __wrap_valloc(size_t);
size_t __wrap_malloc_usable_size(const void*);

static const size_t arena_size = (size_t)256 << 20;
static void* arena;
static size_t reserved;
static unsigned checks;
static atomic_uint failures, probes, thread_errors;
static size_t snapshot_capacity, snapshot_used, snapshot_free, snapshot_peak;
struct failure_record { unsigned operation; size_t requested, alignment; uintptr_t caller; };
static struct failure_record records[128];

static void require(int condition, const char* message) {
    ++checks;
    if (condition) return;
    fprintf(stderr, "NATIVE_HEAP_FAILURE %s\n", message);
    exit(1);
}

void* __real_malloc(size_t size) { return malloc(size); }
void __real_free(void* pointer) { free(pointer); }
void* __real_realloc(void* pointer, size_t size) { return realloc(pointer, size); }
int64_t sceKernelGetDirectMemorySize(void) { return (int64_t)2304 << 20; }
int sceKernelAllocateDirectMemory(int64_t begin, int64_t end, size_t size,
                                  size_t alignment, int type, int64_t* offset) {
    (void)begin; (void)end; (void)alignment; (void)type;
    if (size != arena_size || reserved) return ENOMEM;
    reserved = size;
    *offset = 0;
    return 0;
}
int sceKernelMapDirectMemory(void** address, size_t size, int protection, int flags,
                             int64_t offset, size_t alignment) {
    (void)protection; (void)flags; (void)offset; (void)alignment;
    if (size != reserved) return EINVAL;
    arena = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) { arena = NULL; return ENOMEM; }
    *address = arena;
    return 0;
}
int sceKernelReleaseDirectMemory(int64_t offset, size_t size) {
    (void)offset;
    if (arena) (void)munmap(arena, size);
    arena = NULL; reserved = 0;
    return 0;
}
int sceKernelAvailableDirectMemorySize(int64_t begin, int64_t end, size_t alignment,
                                       int64_t* offset, size_t* size) {
    (void)begin; (void)end; (void)alignment;
    *offset = 0; *size = arena_size;
    return 0;
}
int sceKernelDebugOutText(int channel, const char* text) { (void)channel; (void)text; return 0; }

void stremio_diagnostics_allocation_failure(unsigned operation, size_t requested,
                                           size_t alignment, uintptr_t caller) {
    const unsigned index = atomic_fetch_add(&failures, 1);
    if (index < sizeof(records) / sizeof(records[0]))
        records[index] = (struct failure_record){operation, requested, alignment, caller};
    // A real allocation here would deadlock if the wrapper still held the
    // arena lock when it called the sink. It must not produce another failure.
    void* probe = __wrap_malloc(64);
    if (probe) {
        memset(probe, 0x5a, 64);
        __wrap_free(probe);
        atomic_fetch_add(&probes, 1);
    }
    errno = EIO; // Simulate an unsuccessful diagnostic write.
}
void stremio_diagnostics_heap_snapshot(unsigned checkpoint, size_t capacity,
                                      size_t used, size_t available, size_t peak, uintptr_t caller) {
    (void)checkpoint; (void)caller;
    snapshot_capacity = capacity; snapshot_used = used; snapshot_free = available; snapshot_peak = peak;
    errno = EIO;
}

static void check_failure(unsigned before, unsigned operation, size_t size, size_t alignment) {
    require(atomic_load(&failures) == before + 1, "An allocator failure did not emit exactly one event");
    const struct failure_record* record = &records[before];
    require(record->operation == operation && record->requested == size && record->alignment == alignment,
        "The event lost the outer operation, requested size or alignment");
    require(record->caller != 0, "An allocator failure lost its outer return address");
    require(atomic_load(&probes) == atomic_load(&failures), "The diagnostic sink could not reenter the unlocked allocator");
}

__attribute__((noinline)) static void malloc_failure(size_t size) {
    const unsigned before = atomic_load(&failures);
    errno = EDOM;
    // dlmalloc's size-wraparound early return preserves errno. Its real
    // arena-exhaustion path sets ENOMEM; the recorder must preserve both.
    const int expected_errno = size == SIZE_MAX ? EDOM : ENOMEM;
    require(__wrap_malloc(size) == NULL && errno == expected_errno, "malloc failure changed its error contract");
    check_failure(before, STREMIO_ALLOC_MALLOC, size, 0);
}
__attribute__((noinline)) static void calloc_failure(size_t count, size_t size) {
    const unsigned before = atomic_load(&failures);
    const size_t reported = size && count > SIZE_MAX / size ? SIZE_MAX : count * size;
    errno = EDOM;
    require(__wrap_calloc(count, size) == NULL && errno == ENOMEM, "calloc failure changed its error contract");
    check_failure(before, STREMIO_ALLOC_CALLOC, reported, 0);
}
__attribute__((noinline)) static void realloc_failure(int with_pointer, int release_on_failure) {
    stremio_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED);
    const size_t used_before = snapshot_used;
    void* pointer = with_pointer ? __wrap_malloc(128) : NULL;
    if (pointer) memset(pointer, 0x6b, 128);
    const unsigned before = atomic_load(&failures);
    errno = EDOM;
    void* result = release_on_failure ? __wrap_reallocf(pointer, SIZE_MAX) : __wrap_realloc(pointer, SIZE_MAX);
    require(result == NULL && errno == (with_pointer ? ENOMEM : EDOM), "realloc failure or its logging changed errno");
    check_failure(before, release_on_failure ? STREMIO_ALLOC_REALLOCF : STREMIO_ALLOC_REALLOC, SIZE_MAX, 0);
    if (pointer && !release_on_failure) {
        const unsigned char* bytes = pointer;
        for (unsigned i = 0; i < 128; ++i) require(bytes[i] == 0x6b, "realloc failure changed the retained allocation");
        __wrap_free(pointer);
    }
    stremio_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED);
    require(snapshot_used == used_before, "reallocf failure leaked the old allocation");
}
__attribute__((noinline)) static void aligned_failure(unsigned operation) {
    const unsigned before = atomic_load(&failures);
    const size_t size = operation == STREMIO_ALLOC_VALLOC ? SIZE_MAX : SIZE_MAX - 63;
    const size_t alignment = operation == STREMIO_ALLOC_VALLOC ? 16384 : 64;
    errno = EDOM;
    if (operation == STREMIO_ALLOC_POSIX_MEMALIGN) {
        void* pointer = (void*)(uintptr_t)0x1234;
        require(__wrap_posix_memalign(&pointer, alignment, size) == ENOMEM && pointer == (void*)(uintptr_t)0x1234,
            "posix_memalign failure changed the output pointer or return value");
    } else {
        void* result = operation == STREMIO_ALLOC_MEMALIGN ? __wrap_memalign(alignment, size) :
            operation == STREMIO_ALLOC_ALIGNED_ALLOC ? __wrap_aligned_alloc(alignment, size) : __wrap_valloc(size);
        require(result == NULL && errno == ENOMEM, "Aligned allocation failure changed errno");
    }
    check_failure(before, operation, size, alignment);
}
__attribute__((noinline)) static void* concurrent_failure(void* opaque) {
    (void)opaque;
    for (unsigned i = 0; i < 4; ++i) {
        errno = EDOM;
        if (__wrap_malloc((size_t)512 << 20) != NULL || errno != ENOMEM)
            atomic_fetch_add(&thread_errors, 1);
    }
    return NULL;
}

int main(void) {
    void* initial = __wrap_malloc(64);
    require(initial != NULL && ((uintptr_t)initial & 31) == 0, "Native arena did not supply a 32-byte aligned allocation");
    require((uintptr_t)initial >= (uintptr_t)arena && (uintptr_t)initial < (uintptr_t)arena + arena_size,
        "The test bypassed the real native arena");
    require(__wrap_malloc_usable_size(initial) >= 64, "Native usable size lost its allocation");
    __wrap_free(initial);
    errno = EDOM;
    stremio_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED);
    require(errno == EDOM && snapshot_capacity == arena_size && snapshot_free > arena_size / 2,
        "Heap checkpoint changed errno or reported the wrong mapped arena");
    const size_t first_peak = snapshot_peak;
    void* large = __wrap_malloc((size_t)1 << 20);
    require(large != NULL, "The checkpoint fixture could not allocate one MiB");
    stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
    require(snapshot_peak >= snapshot_used && snapshot_peak > first_peak, "Observed heap peak did not include its checkpoint");
    const size_t peak = snapshot_peak;
    __wrap_free(large);
    stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
    require(snapshot_peak == peak && snapshot_used < snapshot_peak, "The recorded heap peak went backward after free");
    malloc_failure(SIZE_MAX);
    malloc_failure((size_t)512 << 20);
    calloc_failure(2, SIZE_MAX);
    calloc_failure(1, (size_t)512 << 20);
    realloc_failure(0, 0); realloc_failure(1, 0);
    realloc_failure(0, 1); realloc_failure(1, 1);
    aligned_failure(STREMIO_ALLOC_POSIX_MEMALIGN);
    aligned_failure(STREMIO_ALLOC_MEMALIGN);
    aligned_failure(STREMIO_ALLOC_ALIGNED_ALLOC);
    aligned_failure(STREMIO_ALLOC_VALLOC);
    const unsigned before = atomic_load(&failures);
    void* invalid = (void*)(uintptr_t)0x5678;
    errno = EDOM;
    require(__wrap_posix_memalign(&invalid, 3, 32) == EINVAL && invalid == (void*)(uintptr_t)0x5678 && errno == EDOM,
        "Invalid POSIX alignment changed errno or the output pointer");
    void* zero = __wrap_malloc(0);
    __wrap_free(zero);
    require(atomic_load(&failures) == before, "A zero-size or invalid-alignment request was falsely reported as OOM");
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) require(pthread_create(&threads[i], NULL, concurrent_failure, NULL) == 0,
        "Concurrent allocator test did not start its worker");
    for (unsigned i = 0; i < 4; ++i) require(pthread_join(threads[i], NULL) == 0, "Allocator worker did not exit");
    require(atomic_load(&thread_errors) == 0 && atomic_load(&failures) == before + 16 &&
        atomic_load(&probes) == atomic_load(&failures), "Concurrent failure records lost an event, errno or unlocked probe");
    const unsigned count = atomic_load(&failures);
    for (unsigned i = 0; i < count; ++i) {
        const struct failure_record* record = &records[i];
        printf("NATIVE_HEAP_EVENT operation=%u requested=%zu alignment=%zu caller=0x%zx\n",
            record->operation, record->requested, record->alignment, (size_t)record->caller);
    }
    printf("NATIVE_HEAP_TESTS_OK checks=%u failures=%u unlocked_probes=%u\n", checks, count, atomic_load(&probes));
    return 0;
}
