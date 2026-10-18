/* Native large-picture mappings with real host pages and injected kernel failures. */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "native_memory.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIB ((size_t)1 << 20)
#define PAGE_BYTES ((size_t)0x4000)
#define MIN_PICTURE (4 * MIB)
#define POOL_LIMIT (512 * MIB)
#define RESERVE (128 * MIB)
#define EXTENTS 128

struct extent {
    int owned;
    int mapped;
    int type;
    int protection;
    int64_t offset;
    size_t length;
    void *address;
};
static struct extent extents[EXTENTS];
static size_t physical_bytes;
static size_t available_budget = 1024 * MIB;
static int64_t direct_total = (int64_t)2304 << 20;
static int fail_query, fail_allocate, fail_map, null_map, fail_unmap, fail_release;
static unsigned queries, allocations, maps, unmaps, releases;
static int last_type, last_protection;
static atomic_uint checks;
static unsigned scenarios;
static _Thread_local unsigned mapper_depth;
static const char *current_case;
static unsigned case_first_check;

static void require(int condition, const char *message)
{
    atomic_fetch_add(&checks, 1);
    if (condition) return;
    mapper_depth = 0;
    fprintf(stderr, "NATIVE_VIDEO_MEMORY_FAILURE %s: %s\n", current_case, message);
    exit(1);
}

static void forbid_heap(void)
{
    if (!mapper_depth) return;
    const char message[] = "NATIVE_VIDEO_MEMORY_FAILURE heap allocation/free inside mapping hook\n";
    const ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(77);
}

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
int __real_posix_memalign(void **, size_t, size_t);
void *__real_aligned_alloc(size_t, size_t);
void *__wrap_malloc(size_t n) { forbid_heap(); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { forbid_heap(); return __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t size) { forbid_heap(); return __real_realloc(p, size); }
void __wrap_free(void *p) { forbid_heap(); __real_free(p); }
int __wrap_posix_memalign(void **p, size_t alignment, size_t size)
{ forbid_heap(); return __real_posix_memalign(p, alignment, size); }
void *__wrap_aligned_alloc(size_t alignment, size_t size)
{ forbid_heap(); return __real_aligned_alloc(alignment, size); }

static void *picture_alloc(size_t size)
{
    errno = EDOM;
    ++mapper_depth;
    void *p = vlc_ps5_big_alloc(size);
    --mapper_depth;
    require(errno == EDOM, "allocation changed caller errno");
    return p;
}

static int picture_free(void *p)
{
    errno = EDOM;
    ++mapper_depth;
    const int owned = vlc_ps5_big_free(p);
    --mapper_depth;
    require(errno == EDOM, "release changed caller errno");
    return owned;
}

static struct stremio_video_memory_stats snapshot(void)
{
    struct stremio_video_memory_stats s;
    errno = EDOM;
    ++mapper_depth;
    stremio_video_memory_get_stats(&s);
    --mapper_depth;
    require(errno == EDOM, "statistics changed caller errno");
    require(s.direct_bytes <= POOL_LIMIT && s.direct_peak_bytes <= POOL_LIMIT,
            "mapped-memory accounting exceeded the pool limit");
    require(s.direct_bytes == physical_bytes, "statistics disagree with physical ownership");
    return s;
}

static void check_empty(void)
{
    const struct stremio_video_memory_stats s = snapshot();
    require(s.direct_bytes == 0 && s.active_mappings == 0 && s.retired_mappings == 0,
            "a case leaked mapped or physical memory");
    for (unsigned i = 0; i < EXTENTS; ++i)
        require(!extents[i].owned && !extents[i].mapped, "the kernel ledger retained an extent");
}

static void begin_case(const char *name)
{
    current_case = name;
    case_first_check = atomic_load(&checks);
    require(!fail_query && !fail_allocate && !fail_map && !null_map && !fail_unmap && !fail_release,
            "an earlier case left failure injection enabled");
    require(available_budget == 1024 * MIB && direct_total == ((int64_t)2304 << 20),
            "an earlier case changed the kernel budget");
    check_empty();
}

static void end_case(void)
{
    check_empty();
    printf("PASS\t%s\t%u\n", current_case, atomic_load(&checks) - case_first_check);
    ++scenarios;
}

static int injected(int *remaining)
{
    if (*remaining < 0) return 1;
    if (!*remaining) return 0;
    --*remaining;
    return 1;
}

static struct extent *by_offset(int64_t offset)
{
    for (unsigned i = 0; i < EXTENTS; ++i)
        if (extents[i].owned && extents[i].offset == offset) return &extents[i];
    require(0, "kernel operation named an unowned physical extent");
    return NULL;
}

static struct extent *by_address(void *address)
{
    for (unsigned i = 0; i < EXTENTS; ++i)
        if (extents[i].owned && extents[i].mapped && extents[i].address == address) return &extents[i];
    require(0, "unmap named an unowned address or double-unmapped a buffer");
    return NULL;
}

int64_t sceKernelGetDirectMemorySize(void) { errno = EIO; return direct_total; }

int sceKernelAvailableDirectMemorySize(int64_t first, int64_t last, size_t alignment,
                                       int64_t *start, size_t *size)
{
    require(first == 0 && last == direct_total && alignment == PAGE_BYTES,
            "available-memory query changed its search extent or alignment");
    ++queries;
    errno = EIO;
    if (injected(&fail_query)) return EIO;
    *start = 0;
    *size = available_budget > physical_bytes ? available_budget - physical_bytes : 0;
    return 0;
}

int sceKernelAllocateDirectMemory(int64_t first, int64_t last, size_t length,
                                  size_t alignment, int type, int64_t *offset)
{
    require(first == 0 && last == direct_total && alignment == PAGE_BYTES,
            "physical allocation changed its search extent or alignment");
    require(length && !(length & (PAGE_BYTES - 1)) && (type == 12 || type == 0),
            "physical allocation used invalid size or memory type");
    ++allocations;
    last_type = type;
    errno = EIO;
    if (injected(&fail_allocate)) return ENOMEM;
    require(physical_bytes <= available_budget && length <= available_budget - physical_bytes,
            "physical allocation exceeded the available memory");
    struct extent *slot = NULL;
    for (unsigned i = 0; i < EXTENTS; ++i)
        if (!extents[i].owned) { slot = &extents[i]; break; }
    require(slot != NULL, "the mapper exceeded its physical extent table");
    int64_t candidate = 0;
    for (unsigned pass = 0; pass <= EXTENTS; ++pass) {
        int collision = 0;
        for (unsigned i = 0; i < EXTENTS; ++i) {
            const struct extent *e = &extents[i];
            if (!e->owned || candidate >= e->offset + (int64_t)e->length ||
                candidate + (int64_t)length <= e->offset) continue;
            candidate = e->offset + (int64_t)e->length;
            collision = 1;
            break;
        }
        if (!collision) break;
    }
    require(candidate >= 0 && candidate + (int64_t)length <= direct_total,
            "the kernel fixture ran out of physical address space");
    *slot = (struct extent){.owned = 1, .type = type, .offset = candidate, .length = length};
    physical_bytes += length;
    *offset = candidate;
    return 0;
}

int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t offset, size_t alignment)
{
    struct extent *e = by_offset(offset);
    require(!e->mapped && e->length == length && flags == 0 && alignment == PAGE_BYTES,
            "mapping lost physical ownership or changed extent");
    require(protection == 0x03 || protection == 0x33, "mapping used an unsupported protection");
    ++maps;
    last_protection = protection;
    errno = EIO;
    if (injected(&fail_map)) return ENOMEM;
    if (injected(&null_map)) { *address = NULL; return 0; }
    void *raw = mmap(NULL, length + PAGE_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(raw != MAP_FAILED, "host mmap could not create the fixture pages");
    const uintptr_t aligned = ((uintptr_t)raw + PAGE_BYTES - 1) & ~(uintptr_t)(PAGE_BYTES - 1);
    const size_t prefix = aligned - (uintptr_t)raw;
    const size_t suffix = PAGE_BYTES - prefix;
    if (prefix) require(munmap(raw, prefix) == 0, "could not trim host mapping prefix");
    if (suffix) require(munmap((void *)(aligned + length), suffix) == 0, "could not trim host mapping suffix");
    e->address = *address = (void *)aligned;
    e->mapped = 1;
    e->protection = protection;
    errno = EIO;
    return 0;
}

int sceKernelMunmap(void *address, size_t length)
{
    struct extent *e = by_address(address);
    require(e->length == length, "unmap changed the mapped extent");
    ++unmaps;
    errno = EIO;
    if (injected(&fail_unmap)) return EIO;
    require(munmap(address, length) == 0, "host munmap failed");
    e->mapped = 0;
    e->address = NULL;
    errno = EIO;
    return 0;
}

int sceKernelReleaseDirectMemory(int64_t offset, size_t length)
{
    struct extent *e = by_offset(offset);
    require(!e->mapped && e->length == length, "physical release happened before unmapping or changed size");
    ++releases;
    errno = EIO;
    if (injected(&fail_release)) return EIO;
    require(physical_bytes >= length, "physical ownership underflowed");
    physical_bytes -= length;
    memset(e, 0, sizeof(*e));
    return 0;
}

static void mark_picture(void *p, size_t bytes, unsigned char value)
{
    require(p != NULL && !((uintptr_t)p & (PAGE_BYTES - 1)), "picture mapping is missing or misaligned");
    unsigned char *data = p;
    data[0] = value;
    data[bytes / 2] = (unsigned char)(value + 1);
    data[bytes - 1] = (unsigned char)(value + 2);
}

static void check_picture(void *p, size_t bytes, unsigned char value)
{
    const unsigned char *data = p;
    require(data[0] == value && data[bytes / 2] == (unsigned char)(value + 1) &&
            data[bytes - 1] == (unsigned char)(value + 2), "a live picture was overwritten or released early");
}

static void test_regular_and_unknown(void)
{
    begin_case("real_mapping_alignment_and_heap_ownership");
    const size_t bytes = MIN_PICTURE + 1;
    void *p = picture_alloc(bytes);
    mark_picture(p, bytes, 0x27);
    struct stremio_video_memory_stats s = snapshot();
    require(s.direct_bytes == MIN_PICTURE + PAGE_BYTES && s.active_mappings == 1 && !s.retired_mappings,
            "a rounded mapping has incorrect statistics");
    require(picture_free((unsigned char *)p + 1) == 0, "an interior pointer was incorrectly accepted as an allocation");
    void *heap = aligned_alloc(64, 1024);
    require(heap != NULL, "ordinary heap fixture allocation failed");
    memset(heap, 0x42, 1024);
    require(picture_free(heap) == 0, "a heap pointer was consumed by the direct-memory mapper");
    require(((unsigned char *)heap)[1023] == 0x42, "unknown heap pointer was freed or modified");
    free(heap);
    int stack = 17;
    require(picture_free(&stack) == 0 && stack == 17, "unknown stack pointer was touched");
    require(picture_free(NULL) == 0, "NULL was reported as an owned mapping");
    errno = EDOM;
    stremio_video_memory_get_stats(NULL);
    require(errno == EDOM, "NULL statistics request changed errno");
    check_picture(p, bytes, 0x27);
    require(picture_free(p) == 1, "mapped picture did not report its ownership");
    end_case();
}

static void test_limits(void)
{
    begin_case("size_overflow_and_pool_cap");
    const unsigned initial_calls = allocations + queries;
    require(!picture_alloc(0) && !picture_alloc(MIN_PICTURE - 1) && !picture_alloc(SIZE_MAX),
            "invalid or small picture bypassed the ordinary allocator");
    require(initial_calls == allocations + queries, "invalid size reached kernel allocation");
    require(!picture_alloc(POOL_LIMIT + 1), "oversize picture exceeded the pool cap");
    void *p = picture_alloc(POOL_LIMIT);
    mark_picture(p, POOL_LIMIT, 0x1b);
    const unsigned full_calls = allocations;
    require(!picture_alloc(MIN_PICTURE), "a full pool accepted another picture");
    require(allocations == full_calls, "a full pool made a kernel allocation attempt");
    const struct stremio_video_memory_stats s = snapshot();
    require(s.direct_bytes == POOL_LIMIT && s.direct_peak_bytes == POOL_LIMIT && s.active_mappings == 1,
            "full-pool statistics lost their peak or extent");
    check_picture(p, POOL_LIMIT, 0x1b);
    require(picture_free(p) == 1, "full-pool release lost ownership");
    end_case();

    begin_case("all_128_mapping_slots_and_reuse");
    void *pictures[EXTENTS];
    for (unsigned i = 0; i < EXTENTS; ++i) {
        pictures[i] = picture_alloc(MIN_PICTURE);
        mark_picture(pictures[i], MIN_PICTURE, (unsigned char)i);
    }
    const struct stremio_video_memory_stats full = snapshot();
    require(full.active_mappings == EXTENTS && full.direct_bytes == POOL_LIMIT,
            "mapping table did not reach its documented capacity");
    require(!picture_alloc(MIN_PICTURE), "mapping table overflow did not use the heap fallback");
    for (unsigned i = 0; i < EXTENTS; ++i) {
        check_picture(pictures[i], MIN_PICTURE, (unsigned char)i);
        require(picture_free(pictures[i]) == 1, "mapping table release lost an entry");
    }
    void *reused = picture_alloc(MIN_PICTURE);
    require(reused != NULL && picture_free(reused) == 1, "released mapping slots were not reusable");
    end_case();
}

static void test_available_memory(void)
{
    begin_case("available_memory_reserve_and_query_failures");
    available_budget = RESERVE + MIN_PICTURE - 1;
    unsigned before = allocations;
    require(!picture_alloc(MIN_PICTURE) && allocations == before, "allocation consumed the required direct-memory reserve");
    available_budget = RESERVE + MIN_PICTURE;
    void *p = picture_alloc(MIN_PICTURE);
    mark_picture(p, MIN_PICTURE, 0x55);
    require(!picture_alloc(MIN_PICTURE), "allocation ignored the reserve after a live mapping");
    check_picture(p, MIN_PICTURE, 0x55);
    require(picture_free(p) == 1, "reserve-bound mapping was not released");
    available_budget = 1024 * MIB;
    before = allocations;
    fail_query = 1;
    require(!picture_alloc(MIN_PICTURE) && allocations == before, "failed availability query proceeded to allocation");
    direct_total = 0;
    require(!picture_alloc(MIN_PICTURE) && allocations == before, "zero direct-memory size proceeded to allocation");
    direct_total = -1;
    require(!picture_alloc(MIN_PICTURE) && allocations == before, "negative direct-memory size proceeded to allocation");
    direct_total = (int64_t)2304 << 20;
    end_case();
}

static void test_native_fallbacks(void)
{
    begin_case("memory_type_and_protection_fallbacks");
    fail_allocate = 1;
    void *p = picture_alloc(MIN_PICTURE);
    require(p && last_type == 0, "allocation did not fall back from memory type 12 to 0");
    require(picture_free(p) == 1, "fallback memory type lost ownership");
    fail_map = 1;
    p = picture_alloc(MIN_PICTURE);
    require(p && last_protection == 0x33, "mapping did not fall back to protection 0x33");
    require(picture_free(p) == 1, "fallback protection lost ownership");
    const unsigned release_before = releases;
    fail_map = 2;
    p = picture_alloc(MIN_PICTURE);
    require(p && last_type == 0 && releases == release_before + 1,
            "mapping fallback did not release the first physical allocation");
    require(picture_free(p) == 1, "second-type mapped pointer lost ownership");
    end_case();

    begin_case("all_kernel_attempts_fail_without_heap_free");
    const struct stremio_video_memory_stats before = snapshot();
    fail_allocate = 2;
    require(!picture_alloc(MIN_PICTURE), "exhausted physical allocations did not return NULL");
    fail_map = 4;
    require(!picture_alloc(MIN_PICTURE), "exhausted mappings did not return NULL");
    null_map = 4;
    require(!picture_alloc(MIN_PICTURE), "successful status with NULL mapping was accepted");
    const struct stremio_video_memory_stats after = snapshot();
    require(after.heap_fallbacks == before.heap_fallbacks + 3 &&
            after.allocation_failures == before.allocation_failures + 10 &&
            after.direct_bytes == 0, "failure statistics lost a fallback or retained native memory");
    end_case();
}

static void test_retirement(void)
{
    begin_case("failed_map_physical_release_is_retained_and_retried");
    fail_map = 2;
    fail_release = 1;
    require(!picture_alloc(MIN_PICTURE), "failed mapping with pending physical release returned a pointer");
    struct stremio_video_memory_stats s = snapshot();
    require(s.direct_bytes == MIN_PICTURE && s.active_mappings == 0 && s.retired_mappings == 1,
            "failed physical release escaped the ownership bound");
    void *p = picture_alloc(MIN_PICTURE);
    require(p != NULL, "allocation did not retry the pending physical release");
    s = snapshot();
    require(s.direct_bytes == MIN_PICTURE && s.active_mappings == 1 && s.retired_mappings == 0,
            "retry retained a stale physical owner");
    require(picture_free(p) == 1, "post-retry allocation was not owned");
    end_case();

    begin_case("failed_unmap_preserves_pages_and_existing_live_picture");
    p = picture_alloc(MIN_PICTURE);
    mark_picture(p, MIN_PICTURE, 0x38);
    fail_unmap = 2;
    require(picture_free(p) == 1, "failed unmap allowed an owned pointer to reach heap free");
    check_picture(p, MIN_PICTURE, 0x38);
    void *held = picture_alloc(MIN_PICTURE);
    mark_picture(held, MIN_PICTURE, 0x79);
    check_picture(p, MIN_PICTURE, 0x38);
    struct stremio_video_memory_stats s2 = snapshot();
    require(s2.direct_bytes == 2 * MIN_PICTURE && s2.active_mappings == 1 && s2.retired_mappings == 1,
            "pending unmap was reused or stopped counting against the bound");
    require(picture_free(held) == 1, "free did not reclaim both pending and live ownership");
    end_case();

    begin_case("failed_release_retries_without_double_unmap");
    p = picture_alloc(MIN_PICTURE);
    const unsigned unmap_before = unmaps;
    fail_release = 1;
    require(picture_free(p) == 1, "release failure lost mapped ownership");
    s = snapshot();
    require(s.retired_mappings == 1 && s.active_mappings == 0 && s.direct_bytes == MIN_PICTURE,
            "unmapped physical owner disappeared from accounting");
    held = picture_alloc(MIN_PICTURE);
    require(held != NULL && unmaps == unmap_before + 1, "physical-release retry double-unmapped a buffer");
    require(picture_free(held) == 1, "new live mapping was not released");
    end_case();

    begin_case("retired_full_pool_remains_bounded_until_kernel_recovers");
    p = picture_alloc(POOL_LIMIT);
    mark_picture(p, POOL_LIMIT, 0x6f);
    fail_unmap = -1;
    require(picture_free(p) == 1, "persistent failed unmap lost ownership");
    const unsigned allocation_before = allocations;
    for (unsigned i = 0; i < 4; ++i)
        require(!picture_alloc(MIN_PICTURE), "retired full pool permitted over-allocation");
    require(allocations == allocation_before, "retired full pool attempted physical allocation");
    check_picture(p, POOL_LIMIT, 0x6f);
    s = snapshot();
    require(s.direct_bytes == POOL_LIMIT && s.retired_mappings == 1 && s.active_mappings == 0,
            "persistent failed unmap escaped accounting");
    fail_unmap = 0;
    int unknown = 0;
    require(picture_free(&unknown) == 0, "cleanup retry claimed an unknown pointer");
    end_case();
}

struct worker { unsigned index; pthread_barrier_t *barrier; };
static void *concurrent_worker(void *opaque)
{
    struct worker *worker = opaque;
    void *held[8];
    const size_t bytes = MIN_PICTURE + worker->index * PAGE_BYTES;
    for (unsigned round = 0; round < 8; ++round) {
        for (unsigned i = 0; i < 8; ++i) {
            held[i] = picture_alloc(bytes);
            mark_picture(held[i], bytes, (unsigned char)(worker->index * 32 + i));
        }
        int rc = pthread_barrier_wait(worker->barrier);
        require(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD, "concurrent fixture barrier failed");
        sched_yield();
        for (unsigned i = 0; i < 8; ++i) {
            check_picture(held[i], bytes, (unsigned char)(worker->index * 32 + i));
            require(picture_free(held[i]) == 1, "concurrent release lost mapping ownership");
        }
        rc = pthread_barrier_wait(worker->barrier);
        require(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD, "concurrent release barrier failed");
    }
    return NULL;
}

static void test_concurrent(void)
{
    begin_case("four_threads_keep_live_pictures_and_reuse_mappings");
    pthread_barrier_t barrier;
    require(pthread_barrier_init(&barrier, NULL, 4) == 0, "concurrent fixture could not create its barrier");
    pthread_t threads[4];
    struct worker workers[4];
    const struct stremio_video_memory_stats before = snapshot();
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){i, &barrier};
        require(pthread_create(&threads[i], NULL, concurrent_worker, &workers[i]) == 0,
                "concurrent fixture could not start a worker");
    }
    for (unsigned i = 0; i < 4; ++i)
        require(pthread_join(threads[i], NULL) == 0, "concurrent fixture could not join a worker");
    require(pthread_barrier_destroy(&barrier) == 0, "concurrent fixture could not destroy its barrier");
    const struct stremio_video_memory_stats after = snapshot();
    require(after.direct_allocations == before.direct_allocations + 256 &&
            after.heap_fallbacks == before.heap_fallbacks && after.direct_bytes == 0,
            "concurrent mapper lost an allocation or unexpectedly exhausted its pool");
    end_case();
}

int main(void)
{
    test_regular_and_unknown();
    test_limits();
    test_available_memory();
    test_native_fallbacks();
    test_retirement();
    test_concurrent();
    printf("TOTAL\t%u\t%u\n", scenarios, atomic_load(&checks));
    printf("KERNEL\t%u\t%u\t%u\t%u\t%u\n", queries, allocations, maps, unmaps, releases);
    return 0;
}
