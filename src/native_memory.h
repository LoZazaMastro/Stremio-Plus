#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stremio_video_memory_stats {
    size_t direct_bytes;
    size_t direct_peak_bytes;
    uint64_t direct_allocations;
    uint64_t heap_fallbacks;
    uint64_t allocation_failures;
    uint64_t release_failures;
    unsigned active_mappings;
    unsigned retired_mappings;
};

#define STREMIO_DIRECT_MIN_BYTES ((size_t)1 << 20)

/* Shared bounded, cached direct-memory storage for large native allocations.
 * Alignment must be a nonzero power of two. NULL requests an ordinary-heap
 * fallback; these hooks preserve errno and never call a heap allocator.
 * Only exact live allocation pointers have a usable size. A failed release
 * retains ownership, so free still consumes its pointer until cleanup works. */
void *stremio_direct_alloc(size_t size, size_t alignment);
int stremio_direct_free(void *buffer);
size_t stremio_direct_usable_size(const void *buffer);

/* Existing VLC picture hooks share the same ownership table and budget. */
void *vlc_ps5_big_alloc(size_t size);
int vlc_ps5_big_free(void *buffer);
void stremio_video_memory_get_stats(struct stremio_video_memory_stats *out);

#ifdef __cplusplus
}
#endif
