// The native app's malloc heap, in direct memory.
//
// A native title's malloc comes from the system heap, which grows out of the
// title's *flexible* memory: only a few hundred MB, most of it taken before
// main() by the program image itself. On the console the self-test showed
// 8 MB working and 8 MB + 16 bytes failing, so SDL couldn't even create the
// screen. Titles have far more *direct* memory (about 11 GB), so - like the
// Kodi port for PS5 (shims/native-app/heap_dmem.c, GPL-2.0-or-later) - this
// reserves one large block of it at start-up and runs dlmalloc (Doug Lea,
// MIT-0, native/dlmalloc.c) inside it.
//
// The link wraps malloc, free, calloc, realloc, the aligned variants and
// malloc_usable_size (native/build.sh), so every call in the app and its
// libraries comes here. A pointer outside the block was allocated by the
// system itself (inside a system module) or by the shared large-block pool.
// The latter also serves FFmpeg's aligned buffers, not only VLC pictures.

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/diagnostics_native.h"
#include "../src/native_memory.h"

#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 1
// The PS5 target assumes every allocation is 32-byte aligned
// (__STDCPP_DEFAULT_NEW_ALIGNMENT__ is 32): the compiler stores 32 bytes at
// once (vmovaps ymm) into fresh blocks, which faulted on dlmalloc's default 16.
#define MALLOC_ALIGNMENT 32
#define MALLOC_FAILURE_ACTION errno = ENOMEM;
#define ABORT __builtin_trap()
#include "dlmalloc.c"

void* __real_malloc(size_t size);
void __real_free(void* p);
void* __real_realloc(void* p, size_t size);

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t* start);
int sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags,
                             int64_t start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);
int sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment,
                                       int64_t* start_out, size_t* size_out);
int sceKernelDebugOutText(int channel, const char* text);

static mspace g_heap;
static uintptr_t g_lo, g_hi;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void heap_log(const char* fmt, long long a, long long b) {
	char line[200];
	snprintf(line, sizeof(line), fmt, a, b);
	sceKernelDebugOutText(0, line);
}

// One block of direct memory, CPU read/write (the parameters the Kodi port
// and ProsperoLight use on hardware).
static void* map_direct(size_t size) {
	static const int types[] = {12, 0};
	static const int protections[] = {0x03, 0x33};
	int64_t limit = sceKernelGetDirectMemorySize();
	if (limit <= 0) return NULL;
	for (unsigned t = 0; t < 2; t++) {
		int64_t start = -1;
		if (sceKernelAllocateDirectMemory(0, limit, size, 0x4000, types[t], &start) != 0) continue;
		for (unsigned p = 0; p < 2; p++) {
			void* address = NULL;
			if (sceKernelMapDirectMemory(&address, size, protections[p], 0, start, 0x4000) == 0 && address)
				return address;
		}
		sceKernelReleaseDirectMemory(start, size);
	}
	return NULL;
}

// Keep ordinary UI/control allocations in a moderate arena. Large codec and
// rendering buffers use the shared pool on demand and return physical memory
// when released, so their capacity is not trapped inside a 1280 MiB mspace.
// The pool separately enforces its physical reserve and total byte bound.

static void heap_init(void) {
	static const size_t sizes_mb[] = {768, 512, 256};
	int64_t total = sceKernelGetDirectMemorySize(), avail_start = 0;
	size_t avail = 0;
	if (total <= 0 || sceKernelAvailableDirectMemorySize(0, total, 0x4000, &avail_start, &avail) != 0) avail = 0;
	heap_log("[stremio] heap: direct memory %lld MB, %lld MB free\n", (long long)(total >> 20),
	         (long long)(avail >> 20));
	const size_t cap = total > 0 ? (size_t)total : 0;
	for (unsigned i = 0; i < sizeof(sizes_mb) / sizeof(sizes_mb[0]); i++) {
		size_t size = sizes_mb[i] << 20;
		if (size > cap) continue;
		void* base = map_direct(size);
		if (!base) continue;
		g_heap = create_mspace_with_base(base, size, 1);
		if (!g_heap) continue;
		g_lo = (uintptr_t)base;
		g_hi = g_lo + size;
		heap_log("[stremio] heap: %lld MB of direct memory at 0x%llx\n", (long long)sizes_mb[i],
		         (long long)g_lo);
		return;
	}
	sceKernelDebugOutText(0, "[stremio] heap: no direct memory; using the small system heap\n");
}

static inline int heap_ready(void) {
	pthread_once(&g_once, heap_init);
	return g_heap != NULL;
}

static inline int ours(const void* p) {
	return (uintptr_t)p >= g_lo && (uintptr_t)p < g_hi;
}

static void* allocate(size_t size) {
	const int ready = heap_ready();
	if (size >= STREMIO_DIRECT_MIN_BYTES) {
		void* direct = stremio_direct_alloc(size, MALLOC_ALIGNMENT);
		if (direct) return direct;
	}
	return ready ? mspace_malloc(g_heap, size) : __real_malloc(size);
}

static void* record_allocation(void* result, unsigned operation, size_t size,
                               size_t alignment, uintptr_t caller) {
	if (!result && size) {
		const int saved_errno = errno;
		/* The allocator has returned and released its lock before this call. */
		stremio_diagnostics_allocation_failure(operation, size, alignment, caller);
		errno = saved_errno;
	}
	return result;
}

#define ALLOCATION_CALLER ((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))

__attribute__((noinline)) void* __wrap_malloc(size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	return record_allocation(allocate(size), STREMIO_ALLOC_MALLOC, size, 0, caller);
}

void __wrap_free(void* p) {
	if (!p) return;
	if (ours(p))
		mspace_free(g_heap, p);
	else if (!stremio_direct_free(p))
		__real_free(p);  // allocated by the system
}

__attribute__((noinline)) void* __wrap_calloc(size_t n, size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	if (size && n > SIZE_MAX / size) {
		errno = ENOMEM;
		return record_allocation(NULL, STREMIO_ALLOC_CALLOC, SIZE_MAX, 0, caller);
	}
	const size_t total = n * size;
	void* p = allocate(total);
	if (p) memset(p, 0, total);
	return record_allocation(p, STREMIO_ALLOC_CALLOC, total, 0, caller);
}

static void* reallocate(void* p, size_t size) {
	if (!p) return allocate(size);
	if (!size) { __wrap_free(p); return NULL; }
	if (ours(p)) {
		const size_t old_size = mspace_usable_size(p);
		if (size >= STREMIO_DIRECT_MIN_BYTES && size > old_size) {
			void* direct = stremio_direct_alloc(size, MALLOC_ALIGNMENT);
			if (direct) {
				memcpy(direct, p, old_size);
				mspace_free(g_heap, p);
				return direct;
			}
		}
		return mspace_realloc(g_heap, p, size);
	}
	const size_t old_size = stremio_direct_usable_size(p);
	if (old_size) {
		if (size >= STREMIO_DIRECT_MIN_BYTES && size <= old_size &&
		    ((size + 16383) & ~(size_t)16383) == old_size) return p;
		const int saved_errno = errno;
		void* replacement = allocate(size);
		if (!replacement) {
			// A shrink still fits its live mapping when temporary capacity is
			// unavailable. Keep that valid allocation without reporting OOM.
			if (size <= old_size) { errno = saved_errno; return p; }
			errno = ENOMEM;
			return NULL;
		}
		memcpy(replacement, p, size < old_size ? size : old_size);
		stremio_direct_free(p);
		return replacement;
	}
	return __real_realloc(p, size);
}

__attribute__((noinline)) void* __wrap_realloc(void* p, size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	return record_allocation(reallocate(p, size), STREMIO_ALLOC_REALLOC, size, 0, caller);
}

__attribute__((noinline)) void* __wrap_reallocf(void* p, size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	void* q = reallocate(p, size);
	const int saved_errno = errno;
	if (!q && size) __wrap_free(p);
	errno = saved_errno;
	return record_allocation(q, STREMIO_ALLOC_REALLOCF, size, 0, caller);
}

static void* aligned(size_t align, size_t size) {
	if (align <= 32) return allocate(size);
	const int ready = heap_ready();
	if (size >= STREMIO_DIRECT_MIN_BYTES) {
		void* direct = stremio_direct_alloc(size, align);
		if (direct) return direct;
	}
	if (ready) return mspace_memalign(g_heap, align, size);
	errno = ENOMEM;
	return NULL;
}

__attribute__((noinline)) int __wrap_posix_memalign(void** out, size_t align, size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	if (!out || align < sizeof(void*) || (align & (align - 1)) != 0) return EINVAL;
	const int saved_errno = errno;
	void* p = aligned(align, size);
	if (!p) {
		(void)record_allocation(NULL, STREMIO_ALLOC_POSIX_MEMALIGN, size, align, caller);
		errno = saved_errno;
		return ENOMEM;
	}
	*out = p;
	errno = saved_errno;
	return 0;
}

__attribute__((noinline)) void* __wrap_memalign(size_t align, size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	if (!align || (align & (align - 1)) != 0) { errno = EINVAL; return NULL; }
	return record_allocation(aligned(align, size), STREMIO_ALLOC_MEMALIGN, size, align, caller);
}
__attribute__((noinline)) void* __wrap_aligned_alloc(size_t align, size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	if (!align || (align & (align - 1)) != 0 || (size & (align - 1)) != 0) { errno = EINVAL; return NULL; }
	return record_allocation(aligned(align, size), STREMIO_ALLOC_ALIGNED_ALLOC, size, align, caller);
}
__attribute__((noinline)) void* __wrap_valloc(size_t size) {
	const uintptr_t caller = ALLOCATION_CALLER;
	return record_allocation(aligned(16384, size), STREMIO_ALLOC_VALLOC, size, 16384, caller);
}

size_t __wrap_malloc_usable_size(const void* p) {
	if (!p) return 0;
	return ours(p) ? mspace_usable_size(p) : stremio_direct_usable_size(p);
}

__attribute__((noinline)) void stremio_heap_snapshot(unsigned checkpoint) {
	static size_t observed_peak;
	const uintptr_t caller = ALLOCATION_CALLER;
	const int saved_errno = errno;
	/* Only the main thread requests checkpoints. Never called by an allocator
	 * wrapper or signal handler; mallinfo may take the allocator's lock. */
	size_t used = 0, available = 0;
	if (g_heap) {
		const struct mallinfo usage = mspace_mallinfo(g_heap);
		used = usage.uordblks;
		available = usage.fordblks;
	}
	if (used > observed_peak) observed_peak = used;
	stremio_diagnostics_heap_snapshot(checkpoint, (size_t)(g_hi - g_lo), used,
	                                 available, observed_peak, caller);
	errno = saved_errno;
}

// Start-up check, written to the kernel log: which allocation sizes work.
void heap_selftest(void) {
	static const size_t sizes[] = {64, 1 << 20, 8 << 20, 34 << 20, 128 << 20, 512 << 20};
	char line[160];
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void* p = __wrap_malloc(sizes[i]);
		void* z = __wrap_calloc(1, sizes[i]);
		snprintf(line, sizeof(line), "[stremio] heap: %zu bytes: malloc %s, calloc %s%s\n", sizes[i],
		         p ? "ok" : "FAILED", z ? "ok" : "FAILED",
		         ((uintptr_t)p | (uintptr_t)z) & 31 ? " (NOT 32-byte aligned)" : "");
		sceKernelDebugOutText(0, line);
		__wrap_free(p);
		__wrap_free(z);
	}
}
