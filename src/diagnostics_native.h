#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum stremio_heap_checkpoint {
    STREMIO_HEAP_APP_INITIALIZED = 1,
    STREMIO_HEAP_VIDEO_PRESENTED = 2
};

enum stremio_allocation_operation {
    STREMIO_ALLOC_MALLOC = 1,
    STREMIO_ALLOC_CALLOC,
    STREMIO_ALLOC_REALLOC,
    STREMIO_ALLOC_REALLOCF,
    STREMIO_ALLOC_POSIX_MEMALIGN,
    STREMIO_ALLOC_MEMALIGN,
    STREMIO_ALLOC_ALIGNED_ALLOC,
    STREMIO_ALLOC_VALLOC
};

/* Native heap checkpoints are infrequent, explicit calls outside the allocator.
 * The observed peak covers these checkpoints, not every allocation. */
void stremio_heap_snapshot(unsigned checkpoint);

/* These sinks only write bounded records to the descriptor opened at startup.
 * They never enter the regular logger or allocate memory. Callers must supply
 * the return address captured at the outer native wrapper. */
void stremio_diagnostics_abort(uintptr_t caller);
void stremio_diagnostics_assert(const char* function, const char* file, int line,
                                const char* expression, uintptr_t caller);
void stremio_diagnostics_allocation_failure(unsigned operation, size_t requested,
                                           size_t alignment, uintptr_t caller);
void stremio_diagnostics_heap_snapshot(unsigned checkpoint, size_t capacity,
                                      size_t used, size_t available,
                                      size_t observed_peak, uintptr_t caller);

#ifdef __cplusplus
}
#endif
