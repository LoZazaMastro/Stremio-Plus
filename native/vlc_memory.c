/* Large native allocations and VLC pictures share a bounded cached pool.
 * Ownership lasts until the last user releases the allocation; failed kernel
 * cleanup remains tracked separately until a later call can finish it. */
#include "../src/native_memory.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define PICTURE_ALIGNMENT ((size_t)0x4000)
#define PICTURE_POOL_BYTES ((size_t)1536 << 20)
#define DIRECT_RESERVE_BYTES ((size_t)128 << 20)
#define PICTURE_MAPS 2048
#define BUDGET_QUERY_LIMIT 64

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t *, size_t *);
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
int sceKernelMunmap(void *, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);

enum mapping_state { MAP_EMPTY, MAP_LIVE, MAP_RETIRING, MAP_PHYSICAL };
struct picture_mapping {
    void *address;
    size_t length;
    int64_t offset;
    enum mapping_state state;
};

static pthread_mutex_t picture_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct picture_mapping picture_maps[PICTURE_MAPS];
static struct stremio_video_memory_stats picture_stats;

/* Failed kernel releases remain in the ownership table and count against the
 * bound. A later allocation/free retries them; they never reach heap free(). */
static void release_mapping(struct picture_mapping *entry)
{
    if (entry->state == MAP_RETIRING) {
        if (sceKernelMunmap(entry->address, entry->length) != 0) {
            ++picture_stats.release_failures;
            return;
        }
        entry->state = MAP_PHYSICAL;
    }
    if (entry->state == MAP_PHYSICAL) {
        if (sceKernelReleaseDirectMemory(entry->offset, entry->length) != 0) {
            ++picture_stats.release_failures;
            return;
        }
        picture_stats.direct_bytes -= entry->length;
        memset(entry, 0, sizeof(*entry));
    }
}

/* The kernel reports one largest free extent, not total available memory.
 * Usually that extent alone proves our reserve. Under fragmentation, inspect
 * disjoint ranges on both sides and count only validated free extents. The
 * bounded stack and query count keep this allocator independent of malloc;
 * unknown or unvisited space can only make the estimate more conservative. */
static int direct_budget_allows(size_t length, int64_t total)
{
    struct range { int64_t first, last; } ranges[BUDGET_QUERY_LIMIT];
    unsigned count = 1;
    ranges[0] = (struct range){0, total};
    size_t available = 0;
    int contiguous = 0;
    const size_t needed = length + DIRECT_RESERVE_BYTES;
    for (unsigned query = 0; count && query < BUDGET_QUERY_LIMIT; ++query) {
        const struct range range = ranges[--count];
        int64_t start = -1;
        size_t size = 0;
        if (sceKernelAvailableDirectMemorySize(range.first, range.last, PICTURE_ALIGNMENT,
                                               &start, &size) != 0 ||
            start < range.first || start >= range.last || !size ||
            size > (uint64_t)(range.last - start))
            continue;
        if (size >= length) contiguous = 1;
        available += size > needed - available ? needed - available : size;
        if (contiguous && available >= needed) return 1;
        const int64_t end = start + (int64_t)size;
        if (start > range.first && count < BUDGET_QUERY_LIMIT)
            ranges[count++] = (struct range){range.first, start};
        if (end < range.last && count < BUDGET_QUERY_LIMIT)
            ranges[count++] = (struct range){end, range.last};
    }
    return 0;
}

void *stremio_direct_alloc(size_t size, size_t alignment)
{
    const int saved_errno = errno;
    void *result = NULL;
    if (size < STREMIO_DIRECT_MIN_BYTES || size > PICTURE_POOL_BYTES ||
        !alignment || (alignment & (alignment - 1)) != 0)
        return NULL;
    const size_t length = (size + PICTURE_ALIGNMENT - 1) & ~(PICTURE_ALIGNMENT - 1);
    if (alignment < PICTURE_ALIGNMENT) alignment = PICTURE_ALIGNMENT;
    pthread_mutex_lock(&picture_mutex);
    struct picture_mapping *entry = NULL;
    for (unsigned i = 0; i < PICTURE_MAPS; ++i) {
        release_mapping(&picture_maps[i]);
        if (!entry && picture_maps[i].state == MAP_EMPTY)
            entry = &picture_maps[i];
    }
    const int64_t total = sceKernelGetDirectMemorySize();
    if (!entry || length > PICTURE_POOL_BYTES - picture_stats.direct_bytes ||
        total <= 0 || alignment > (uint64_t)total || !direct_budget_allows(length, total))
        goto fallback;

    /* Software decode repeatedly reads its reference pictures. Keep them in
     * cached memory, as in EVO Player's large-block allocator; uncached
     * memory can make a decoder stall even when capacity is available. */
    const int types[] = {11, 12};
    const int protections[] = {0x03, 0x33};
    for (unsigned type = 0; type < sizeof(types) / sizeof(types[0]); ++type) {
        int64_t offset = -1;
        if (sceKernelAllocateDirectMemory(0, total, length, alignment, types[type], &offset) != 0) {
            ++picture_stats.allocation_failures;
            continue;
        }
        *entry = (struct picture_mapping){NULL, length, offset, MAP_PHYSICAL};
        picture_stats.direct_bytes += length;
        if (picture_stats.direct_bytes > picture_stats.direct_peak_bytes)
            picture_stats.direct_peak_bytes = picture_stats.direct_bytes;
        for (unsigned protection = 0; protection < sizeof(protections) / sizeof(protections[0]); ++protection) {
            void *address = NULL;
            if (sceKernelMapDirectMemory(&address, length, protections[protection], 0,
                                         offset, alignment) == 0 && address) {
                entry->address = result = address;
                entry->state = MAP_LIVE;
                ++picture_stats.direct_allocations;
                goto done;
            }
            ++picture_stats.allocation_failures;
        }
        release_mapping(entry);
        if (entry->state != MAP_EMPTY)
            break;
    }
fallback:
    /* The caller can now try its ordinary allocator after this lock is gone. */
    ++picture_stats.heap_fallbacks;
done:
    pthread_mutex_unlock(&picture_mutex);
    errno = saved_errno;
    return result;
}

int stremio_direct_free(void *buffer)
{
    if (!buffer)
        return 0;
    const int saved_errno = errno;
    int owned = 0;
    pthread_mutex_lock(&picture_mutex);
    for (unsigned i = 0; i < PICTURE_MAPS; ++i) {
        struct picture_mapping *entry = &picture_maps[i];
        if (entry->state != MAP_EMPTY && entry->address == buffer) {
            if (entry->state == MAP_LIVE) entry->state = MAP_RETIRING;
            owned = 1;
        }
        release_mapping(entry);
    }
    pthread_mutex_unlock(&picture_mutex);
    errno = saved_errno;
    return owned;
}

size_t stremio_direct_usable_size(const void *buffer)
{
    if (!buffer) return 0;
    const int saved_errno = errno;
    size_t size = 0;
    pthread_mutex_lock(&picture_mutex);
    for (unsigned i = 0; i < PICTURE_MAPS; ++i) {
        const struct picture_mapping *entry = &picture_maps[i];
        if (entry->state == MAP_LIVE && entry->address == buffer) {
            size = entry->length;
            break;
        }
    }
    pthread_mutex_unlock(&picture_mutex);
    errno = saved_errno;
    return size;
}

void *vlc_ps5_big_alloc(size_t size)
{
    return stremio_direct_alloc(size, PICTURE_ALIGNMENT);
}

int vlc_ps5_big_free(void *buffer)
{
    return stremio_direct_free(buffer);
}

void stremio_video_memory_get_stats(struct stremio_video_memory_stats *out)
{
    if (!out)
        return;
    const int saved_errno = errno;
    pthread_mutex_lock(&picture_mutex);
    *out = picture_stats;
    out->active_mappings = out->retired_mappings = 0;
    for (unsigned i = 0; i < PICTURE_MAPS; ++i) {
        if (picture_maps[i].state == MAP_LIVE)
            ++out->active_mappings;
        else if (picture_maps[i].state != MAP_EMPTY)
            ++out->retired_mappings;
    }
    pthread_mutex_unlock(&picture_mutex);
    errno = saved_errno;
}
