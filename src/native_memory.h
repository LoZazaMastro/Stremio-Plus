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

void *vlc_ps5_big_alloc(size_t size);
int vlc_ps5_big_free(void *buffer);
void stremio_video_memory_get_stats(struct stremio_video_memory_stats *out);

#ifdef __cplusplus
}
#endif
