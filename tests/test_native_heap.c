// Native allocator wrappers with a host-backed direct-memory arena.
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "diagnostics_native.h"
#include "native_memory.h"
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
enum { mapping_slots = 128 };
struct mapping { void* address; size_t size; int64_t offset; int physical, mapped, heap; };
static struct mapping mappings[mapping_slots];
static size_t physical_bytes;
static int fail_allocate, fail_unmap;
static unsigned real_frees, real_reallocs;
static atomic_uint checks;
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
void __real_free(void* pointer) { ++real_frees; free(pointer); }
void* __real_realloc(void* pointer, size_t size) { ++real_reallocs; return realloc(pointer, size); }
int64_t sceKernelGetDirectMemorySize(void) { return (int64_t)2304 << 20; }
int sceKernelAllocateDirectMemory(int64_t begin, int64_t end, size_t size,
                                  size_t alignment, int type, int64_t* offset) {
    (void)begin; (void)end;
    if (!reserved && size != arena_size) return ENOMEM;
    if (reserved && fail_allocate) { --fail_allocate; return ENOMEM; }
    if (size > ((size_t)2304 << 20) - physical_bytes) return ENOMEM;
    require(alignment >= 16384 && !(alignment & (alignment - 1)), "Kernel allocation lost alignment");
    struct mapping* slot = NULL;
    int64_t next = 0;
    for (unsigned i = 0; i < mapping_slots; ++i) {
        const struct mapping* m = &mappings[i];
        if (!m->physical) { if (!slot) slot = &mappings[i]; continue; }
        if (m->offset + (int64_t)m->size > next) next = m->offset + (int64_t)m->size;
    }
    require(slot != NULL, "Kernel fixture mapping ledger exhausted");
    next = (next + (int64_t)alignment - 1) & ~((int64_t)alignment - 1);
    if (next < 0 || size > ((size_t)2304 << 20) - (size_t)next) return ENOMEM;
    *slot = (struct mapping){.size = size, .offset = next, .physical = 1, .heap = !reserved};
    if (!reserved) reserved = size;
    else require(type == 11 || type == 12, "Generic large allocation did not use a cached memory type");
    physical_bytes += size;
    *offset = next;
    return 0;
}
int sceKernelMapDirectMemory(void** address, size_t size, int protection, int flags,
                             int64_t offset, size_t alignment) {
    (void)protection; (void)flags;
    struct mapping* slot = NULL;
    for (unsigned i = 0; i < mapping_slots; ++i)
        if (mappings[i].physical && mappings[i].offset == offset) { slot = &mappings[i]; break; }
    require(slot && !slot->mapped && slot->size == size, "Map lost physical ownership");
    void* raw = mmap(NULL, size + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return ENOMEM;
    const uintptr_t aligned = ((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1);
    const size_t prefix = aligned - (uintptr_t)raw;
    if (prefix) require(munmap(raw, prefix) == 0, "Could not trim native mapping prefix");
    if (alignment > prefix) require(munmap((void*)(aligned + size), alignment - prefix) == 0, "Could not trim native mapping suffix");
    slot->address = *address = (void*)aligned;
    slot->mapped = 1;
    if (slot->heap) arena = slot->address;
    return 0;
}
int sceKernelMunmap(void* address, size_t size) {
    for (unsigned i = 0; i < mapping_slots; ++i) {
        struct mapping* slot = &mappings[i];
        if (!slot->physical || !slot->mapped || slot->address != address) continue;
        require(slot->size == size && !slot->heap, "Native free changed mapping extent or unmapped the ordinary arena");
        if (fail_unmap) { --fail_unmap; return EIO; }
        require(munmap(address, size) == 0, "Native mapping could not be unmapped");
        slot->mapped = 0; slot->address = NULL;
        return 0;
    }
    require(0, "Native free double-unmapped or consumed a system pointer");
    return EINVAL;
}
int sceKernelReleaseDirectMemory(int64_t offset, size_t size) {
    for (unsigned i = 0; i < mapping_slots; ++i) {
        struct mapping* slot = &mappings[i];
        if (!slot->physical || slot->offset != offset) continue;
        require(!slot->mapped && slot->size == size, "Physical memory released while still mapped");
        physical_bytes -= size;
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    require(0, "Native release lost its physical owner");
    return EINVAL;
}
int sceKernelAvailableDirectMemorySize(int64_t begin, int64_t end, size_t alignment,
                                       int64_t* offset, size_t* size) {
    (void)alignment;
    *offset = begin; *size = 0;
    int64_t cursor = begin;
    while (cursor < end) {
        int64_t occupied_end = cursor, next = end;
        for (unsigned i = 0; i < mapping_slots; ++i) {
            const struct mapping* slot = &mappings[i];
            if (!slot->physical || slot->offset >= end || slot->offset + (int64_t)slot->size <= cursor) continue;
            if (slot->offset <= cursor && slot->offset + (int64_t)slot->size > occupied_end)
                occupied_end = slot->offset + (int64_t)slot->size;
            else if (slot->offset > cursor && slot->offset < next) next = slot->offset;
        }
        if (occupied_end > cursor) { cursor = occupied_end; continue; }
        if ((size_t)(next - cursor) > *size) { *offset = cursor; *size = (size_t)(next - cursor); }
        cursor = next;
    }
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
        if (__wrap_malloc((size_t)2048 << 20) != NULL || errno != ENOMEM)
            atomic_fetch_add(&thread_errors, 1);
    }
    return NULL;
}

static int in_arena(const void* p) {
    return (uintptr_t)p >= (uintptr_t)arena && (uintptr_t)p < (uintptr_t)arena + arena_size;
}

static void require_empty_pool(void) {
    struct stremio_video_memory_stats stats;
    stremio_video_memory_get_stats(&stats);
    require(stats.direct_bytes == 0 && stats.active_mappings == 0 && stats.retired_mappings == 0 &&
            physical_bytes == arena_size, "Large allocation leaked a live or retired physical mapping");
}

static void test_generic_large_allocations(void) {
    const size_t mib = (size_t)1 << 20;
    void* normal = __wrap_malloc(2 * mib);
    unsigned char* zeroed = __wrap_calloc(2, mib);
    require(normal && zeroed && !in_arena(normal) && !in_arena(zeroed),
            "malloc or calloc bypassed the shared large-block allocator");
    int all_zero = 1;
    for (size_t i = 0; i < 2 * mib; ++i) if (zeroed[i]) { all_zero = 0; break; }
    require(all_zero, "Large calloc did not zero every requested byte");
    require(__wrap_malloc_usable_size(normal) >= 2 * mib && stremio_direct_usable_size(zeroed) >= 2 * mib,
            "Generic large blocks lost their usable size");
    __wrap_free(normal); __wrap_free(zeroed);
    require_empty_pool();

    /* The failed console av_malloc was a 64-byte aligned 4,308,544-byte
     * request. Exercise that real size through the public POSIX entry point. */
    const size_t frame_size = 4308544;
    const size_t alignments[] = {64, 65536, mib};
    for (unsigned i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
        void* frame = NULL;
        errno = EDOM;
        require(__wrap_posix_memalign(&frame, alignments[i], frame_size) == 0 && errno == EDOM &&
                frame && !in_arena(frame) && !((uintptr_t)frame & (alignments[i] - 1)),
                "FFmpeg-size aligned allocation was not direct, aligned or errno-preserving");
        memset(frame, 0x63, frame_size);
        require(__wrap_malloc_usable_size(frame) >= frame_size, "Aligned direct allocation lost its capacity");
        __wrap_free(frame);
    }
    void* aligned[] = {__wrap_memalign(64, 2 * mib), __wrap_aligned_alloc(64, 2 * mib), __wrap_valloc(mib)};
    for (unsigned i = 0; i < sizeof(aligned) / sizeof(aligned[0]); ++i) {
        require(aligned[i] && !in_arena(aligned[i]) && !((uintptr_t)aligned[i] & 16383),
                "An aligned wrapper failed to route its large allocation through direct memory");
        __wrap_free(aligned[i]);
    }
    require_empty_pool();

    unsigned char* small = __wrap_malloc(65536);
    require(small && in_arena(small), "Small realloc fixture did not use the ordinary arena");
    memset(small, 0x41, 65536);
    unsigned char* grown = __wrap_realloc(small, 4 * mib);
    require(grown && !in_arena(grown) && grown[0] == 0x41 && grown[65535] == 0x41,
            "Growing realloc did not migrate an ordinary block or preserve its contents");
    memset(grown, 0x52, 4 * mib);
    unsigned char* larger = __wrap_realloc(grown, 8 * mib);
    require(larger && !in_arena(larger) && larger[0] == 0x52 && larger[4 * mib - 1] == 0x52,
            "Growing direct realloc lost contents or returned memory to the ordinary heap");
    require(__wrap_realloc(larger, 8 * mib - 1) == larger, "Page-local direct shrink remapped an existing block");
    unsigned char* reduced = __wrap_realloc(larger, 2 * mib);
    struct stremio_video_memory_stats reduced_stats;
    stremio_video_memory_get_stats(&reduced_stats);
    require(reduced && !in_arena(reduced) && reduced[0] == 0x52 && reduced[2 * mib - 1] == 0x52 &&
            reduced_stats.direct_bytes == 2 * mib, "Large realloc shrink retained the oversized physical mapping");
    unsigned char* shrunk = __wrap_realloc(reduced, 128);
    require(shrunk && in_arena(shrunk) && shrunk[0] == 0x52 && shrunk[127] == 0x52,
            "Small realloc did not release excess direct memory and preserve its contents");
    require_empty_pool();
    __wrap_free(shrunk);

    unsigned char* pressure = __wrap_malloc(512 * mib);
    require(pressure && !in_arena(pressure), "Shrink-pressure fixture did not use a direct mapping");
    pressure[0] = 0x34; pressure[384 * mib - 1] = 0x34;
    const unsigned shrink_failures_before = atomic_load(&failures);
    fail_allocate = 2;
    errno = EDOM;
    require(__wrap_realloc(pressure, 384 * mib) == pressure && errno == EDOM &&
            pressure[0] == 0x34 && pressure[384 * mib - 1] == 0x34 &&
            atomic_load(&failures) == shrink_failures_before,
            "A shrink failed or lost data when temporary direct and arena capacity were unavailable");
    __wrap_free(pressure);
    require_empty_pool();

    void* original = __wrap_malloc(mib);
    require(original && !in_arena(original), "Failed-realloc fixture was not a direct allocation");
    memset(original, 0x76, mib);
    for (unsigned release = 0; release < 2; ++release) {
        const unsigned before = atomic_load(&failures);
        fail_allocate = 2;
        errno = EDOM;
        void* failed = release ? __wrap_reallocf(original, 512 * mib) : __wrap_realloc(original, 512 * mib);
        require(!failed && errno == ENOMEM, "Genuine direct and arena exhaustion did not fail realloc safely");
        check_failure(before, release ? STREMIO_ALLOC_REALLOCF : STREMIO_ALLOC_REALLOC, 512 * mib, 0);
        if (!release)
            require(__wrap_malloc_usable_size(original) >= mib && ((unsigned char*)original)[0] == 0x76 &&
                    ((unsigned char*)original)[mib - 1] == 0x76, "Failed realloc freed or changed the old direct allocation");
    }
    require_empty_pool();

    original = __wrap_malloc(mib);
    require(original && __wrap_realloc(original, 0) == NULL, "Zero-size realloc did not release its direct allocation");
    require_empty_pool();

    fail_allocate = 2;
    void* fallback = __wrap_malloc(4 * mib);
    require(fallback && in_arena(fallback), "A failed large mapping did not fall back to the ordinary heap");
    __wrap_free(fallback);
    require_empty_pool();

    unsigned char* retained = __wrap_malloc(mib);
    require(retained != NULL, "Retirement fixture allocation failed");
    retained[0] = 0x19;
    const unsigned free_before = real_frees;
    fail_unmap = 2;
    __wrap_free(retained);
    require(retained[0] == 0x19, "A failed unmap discarded the backing pages");
    __wrap_free(retained);
    require(real_frees == free_before, "A still-owned direct pointer reached the system free function");
    struct stremio_video_memory_stats stats;
    stremio_video_memory_get_stats(&stats);
    require(stats.retired_mappings == 1 && stats.active_mappings == 0, "Pending cleanup escaped the ownership ledger");
    void* recovery = __wrap_malloc(mib);
    require(recovery != NULL, "Allocation did not retry a retained kernel mapping");
    __wrap_free(recovery);
    require_empty_pool();

    unsigned char* system = malloc(128);
    require(system != NULL, "System ownership fixture allocation failed");
    memset(system, 0x3d, 128);
    const unsigned realloc_before = real_reallocs;
    system = __wrap_realloc(system, 256);
    require(system && system[127] == 0x3d && real_reallocs == realloc_before + 1 &&
            __wrap_malloc_usable_size(system) == 0, "System realloc or its usable-size boundary lost ownership");
    const unsigned final_free_before = real_frees;
    __wrap_free(system);
    require(real_frees == final_free_before + 1, "System allocation did not return to its own free function");
}

static void* concurrent_large(void* opaque) {
    const unsigned char value = (unsigned char)(uintptr_t)opaque;
    for (unsigned round = 0; round < 8; ++round) {
        unsigned char* block = __wrap_malloc(STREMIO_DIRECT_MIN_BYTES);
        if (!block) { atomic_fetch_add(&thread_errors, 1); continue; }
        block[0] = value; block[STREMIO_DIRECT_MIN_BYTES - 1] = value;
        unsigned char* grown = __wrap_realloc(block, STREMIO_DIRECT_MIN_BYTES + 16384);
        if (!grown) { atomic_fetch_add(&thread_errors, 1); __wrap_free(block); continue; }
        if (grown[0] != value || grown[STREMIO_DIRECT_MIN_BYTES - 1] != value)
            atomic_fetch_add(&thread_errors, 1);
        __wrap_free(grown);
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
    void* large = __wrap_malloc((size_t)512 << 10);
    require(large != NULL, "The checkpoint fixture could not allocate half a MiB");
    stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
    require(snapshot_peak >= snapshot_used && snapshot_peak > first_peak, "Observed heap peak did not include its checkpoint");
    const size_t peak = snapshot_peak;
    __wrap_free(large);
    stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
    require(snapshot_peak == peak && snapshot_used < snapshot_peak, "The recorded heap peak went backward after free");
    malloc_failure(SIZE_MAX);
    malloc_failure((size_t)2048 << 20);
    calloc_failure(2, SIZE_MAX);
    calloc_failure(1, (size_t)2048 << 20);
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
    require(!__wrap_memalign(3, 32) && errno == EINVAL, "Invalid memalign did not return EINVAL");
    require(!__wrap_aligned_alloc(64, 65) && errno == EINVAL, "Invalid aligned_alloc size was accepted");
    require(atomic_load(&failures) == before, "An invalid aligned allocation was reported as an out-of-memory failure");
    test_generic_large_allocations();
    const unsigned before_threads = atomic_load(&failures);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) require(pthread_create(&threads[i], NULL, concurrent_failure, NULL) == 0,
        "Concurrent allocator test did not start its worker");
    for (unsigned i = 0; i < 4; ++i) require(pthread_join(threads[i], NULL) == 0, "Allocator worker did not exit");
    require(atomic_load(&thread_errors) == 0 && atomic_load(&failures) == before_threads + 16 &&
        atomic_load(&probes) == atomic_load(&failures), "Concurrent failure records lost an event, errno or unlocked probe");
    for (unsigned i = 0; i < 4; ++i) require(pthread_create(&threads[i], NULL, concurrent_large, (void*)(uintptr_t)(i + 1)) == 0,
        "Concurrent large-allocation test did not start its worker");
    for (unsigned i = 0; i < 4; ++i) require(pthread_join(threads[i], NULL) == 0, "Large-allocation worker did not exit");
    require(atomic_load(&thread_errors) == 0 && atomic_load(&failures) == before_threads + 16,
            "Concurrent large malloc/realloc/free lost contents or exhausted the pool");
    require_empty_pool();
    const unsigned count = atomic_load(&failures);
    for (unsigned i = 0; i < count; ++i) {
        const struct failure_record* record = &records[i];
        printf("NATIVE_HEAP_EVENT operation=%u requested=%zu alignment=%zu caller=0x%zx\n",
            record->operation, record->requested, record->alignment, (size_t)record->caller);
    }
    printf("NATIVE_HEAP_TESTS_OK checks=%u failures=%u unlocked_probes=%u\n", checks, count, atomic_load(&probes));
    return 0;
}
