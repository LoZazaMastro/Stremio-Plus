/* Large VLC pictures use a bounded pool outside the general-purpose heap.
 * The existing VLC hooks retain ownership until the last picture is released. */
#include "../src/native_memory.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define PICTURE_ALIGNMENT ((size_t)0x4000)
#define PICTURE_MIN_BYTES ((size_t)4 << 20)
#define PICTURE_POOL_BYTES ((size_t)512 << 20)
#define DIRECT_RESERVE_BYTES ((size_t)128 << 20)
#define PICTURE_MAPS 128

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

void *vlc_ps5_big_alloc(size_t size)
{
    const int saved_errno = errno;
    void *result = NULL;
    if (size < PICTURE_MIN_BYTES || size > SIZE_MAX - (PICTURE_ALIGNMENT - 1))
        return NULL;
    const size_t length = (size + PICTURE_ALIGNMENT - 1) & ~(PICTURE_ALIGNMENT - 1);
    pthread_mutex_lock(&picture_mutex);
    struct picture_mapping *entry = NULL;
    for (unsigned i = 0; i < PICTURE_MAPS; ++i) {
        release_mapping(&picture_maps[i]);
        if (!entry && picture_maps[i].state == MAP_EMPTY)
            entry = &picture_maps[i];
    }
    const int64_t total = sceKernelGetDirectMemorySize();
    int64_t free_start = 0;
    size_t available = 0;
    if (!entry || length > PICTURE_POOL_BYTES - picture_stats.direct_bytes ||
        total <= 0 ||
        sceKernelAvailableDirectMemorySize(0, total, PICTURE_ALIGNMENT, &free_start, &available) != 0 ||
        available < DIRECT_RESERVE_BYTES || length > available - DIRECT_RESERVE_BYTES)
        goto fallback;

    const int types[] = {12, 0};
    const int protections[] = {0x03, 0x33};
    for (unsigned type = 0; type < sizeof(types) / sizeof(types[0]); ++type) {
        int64_t offset = -1;
        if (sceKernelAllocateDirectMemory(0, total, length, PICTURE_ALIGNMENT, types[type], &offset) != 0) {
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
                                         offset, PICTURE_ALIGNMENT) == 0 && address) {
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
    /* Returning NULL asks VLC to use its ordinary aligned allocator. */
    ++picture_stats.heap_fallbacks;
done:
    pthread_mutex_unlock(&picture_mutex);
    errno = saved_errno;
    return result;
}

int vlc_ps5_big_free(void *buffer)
{
    if (!buffer)
        return 0;
    const int saved_errno = errno;
    int owned = 0;
    pthread_mutex_lock(&picture_mutex);
    for (unsigned i = 0; i < PICTURE_MAPS; ++i) {
        struct picture_mapping *entry = &picture_maps[i];
        if (entry->state == MAP_LIVE && entry->address == buffer) {
            entry->state = MAP_RETIRING;
            owned = 1;
        }
        release_mapping(entry);
    }
    pthread_mutex_unlock(&picture_mutex);
    errno = saved_errno;
    return owned;
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
