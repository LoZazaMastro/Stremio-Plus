// Real HEVC playback through the native arena and native VLC picture allocator.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include "diagnostics_native.h"
#include "http.h"
#include "native_memory.h"
#include "player.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <sys/mman.h>

extern "C" {
void* __wrap_malloc(size_t);
void __wrap_free(void*);
int __wrap_posix_memalign(void**, size_t, size_t);
void __real_stremio_diagnostics_allocation_failure(unsigned, size_t, size_t, uintptr_t);
void __real_stremio_diagnostics_heap_snapshot(unsigned, size_t, size_t, size_t, size_t, uintptr_t);
}

namespace {
constexpr size_t MiB = size_t(1) << 20;
constexpr size_t direct_capacity = 2304 * MiB;
constexpr size_t expected_heap_capacity = 1280 * MiB;
constexpr size_t pressure_bytes = 256 * MiB;
struct Mapping { int64_t offset; size_t length; void* address; bool live; };
Mapping mappings[256]{};
std::mutex mapping_mutex;
size_t direct_live = 0, direct_peak = 0, picture_live = 0, picture_peak = 0;
unsigned picture_allocations = 0, picture_releases = 0, mapping_errors = 0;
std::atomic<size_t> heap_capacity{0}, heap_used{0}, heap_free{0}, heap_peak{0};
std::atomic<unsigned> allocation_failures{0};
std::atomic<size_t> first_failed_bytes{0};
std::atomic<unsigned> first_failed_operation{0};
unsigned checks = 0;
size_t largest_gap(int64_t* start) {
    size_t best = 0, cursor = 0;
    while (cursor < direct_capacity) {
        const Mapping* next = nullptr;
        for (const auto& entry : mappings) if (entry.live && size_t(entry.offset) >= cursor &&
            (!next || entry.offset < next->offset)) next = &entry;
        const size_t end = next ? size_t(next->offset) : direct_capacity;
        if (end - cursor > best) { best = end - cursor; *start = int64_t(cursor); }
        if (!next) break;
        cursor = size_t(next->offset) + next->length;
    }
    return best;
}
double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool require(bool condition, const char* message) {
    ++checks;
    if (!condition) std::fprintf(stderr, "PLAYER_MEMORY_FAILURE %s\n", message);
    return condition;
}
}

// PS5 syscall boundaries are backed by real, separately owned host mappings.
// The production allocators and the real VLC/FFmpeg picture pools run unchanged.
extern "C" int64_t sceKernelGetDirectMemorySize() { return direct_capacity; }
extern "C" int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t* offset, size_t* size) {
    std::lock_guard lock(mapping_mutex);
    *offset = 0; *size = largest_gap(offset);
    return 0;
}
extern "C" int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t length, size_t alignment,
    int, int64_t* offset) {
    std::lock_guard lock(mapping_mutex);
    if (!length || alignment < 0x4000 || length > direct_capacity - direct_live) return ENOMEM;
    int64_t start = 0;
    if (largest_gap(&start) < length) return ENOMEM;
    for (unsigned i = 0; i < std::size(mappings); ++i) if (!mappings[i].live) {
        mappings[i] = {start, length, nullptr, true};
        *offset = mappings[i].offset;
        direct_live += length; direct_peak = std::max(direct_peak, direct_live);
        if (length != expected_heap_capacity) {
            picture_live += length; picture_peak = std::max(picture_peak, picture_live); ++picture_allocations;
        }
        return 0;
    }
    return ENOMEM;
}
extern "C" int sceKernelMapDirectMemory(void** address, size_t length, int, int, int64_t offset, size_t) {
    std::lock_guard lock(mapping_mutex);
    for (auto& entry : mappings) if (entry.live && entry.offset == offset && entry.length == length && !entry.address) {
        void* result = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result == MAP_FAILED) return ENOMEM;
        entry.address = result; *address = result; return 0;
    }
    ++mapping_errors; return EINVAL;
}
extern "C" int sceKernelMunmap(void* address, size_t length) {
    std::lock_guard lock(mapping_mutex);
    for (auto& entry : mappings) if (entry.live && entry.address == address && entry.length == length) {
        if (munmap(address, length)) return errno;
        entry.address = nullptr; return 0;
    }
    ++mapping_errors; return EINVAL;
}
extern "C" int sceKernelReleaseDirectMemory(int64_t offset, size_t length) {
    std::lock_guard lock(mapping_mutex);
    for (auto& entry : mappings) if (entry.live && entry.offset == offset && entry.length == length) {
        if (entry.address) { ++mapping_errors; return EBUSY; }
        direct_live -= length;
        if (length != expected_heap_capacity) { picture_live -= length; ++picture_releases; }
        entry = {}; return 0;
    }
    ++mapping_errors; return EINVAL;
}
extern "C" int sceKernelDebugOutText(int, const char*) { return 0; }
extern "C" void __wrap_stremio_diagnostics_allocation_failure(unsigned operation, size_t size, size_t alignment, uintptr_t caller) {
    if (allocation_failures.fetch_add(1) == 0) { first_failed_operation = operation; first_failed_bytes = size; }
    __real_stremio_diagnostics_allocation_failure(operation, size, alignment, caller);
}
extern "C" void __wrap_stremio_diagnostics_heap_snapshot(unsigned checkpoint, size_t capacity,
    size_t used, size_t available, size_t peak, uintptr_t caller) {
    heap_capacity = capacity; heap_used = used; heap_free = available; heap_peak = peak;
    __real_stremio_diagnostics_heap_snapshot(checkpoint, capacity, used, available, peak, caller);
}

// Match the native link's C++ allocation route as well as its C wrappers.
void* operator new(size_t size) { if (void* value = __wrap_malloc(size ? size : 1)) return value; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { __wrap_free(value); }
void operator delete[](void* value) noexcept { __wrap_free(value); }
void operator delete(void* value, size_t) noexcept { __wrap_free(value); }
void operator delete[](void* value, size_t) noexcept { __wrap_free(value); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return __wrap_malloc(size ? size : 1); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return __wrap_malloc(size ? size : 1); }
void operator delete(void* value, const std::nothrow_t&) noexcept { __wrap_free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { __wrap_free(value); }
void* operator new(size_t size, std::align_val_t alignment) {
    void* value = nullptr;
    if (__wrap_posix_memalign(&value, size_t(alignment), size ? size : 1) == 0) return value;
    throw std::bad_alloc();
}
void* operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* value, std::align_val_t) noexcept { __wrap_free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { __wrap_free(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { __wrap_free(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { __wrap_free(value); }

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (!require(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0, "SDL did not initialize")) return 1;
    diagnostics_start(std::string(argv[1]) + "/diagnostics");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    void* pressure = __wrap_malloc(pressure_bytes);
    if (!require(pressure != nullptr, "Application/network pressure did not fit the native arena")) return 1;
    stremio_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED);
    if (!require(heap_capacity == expected_heap_capacity, "The test did not use the console's 1280 MiB arena")) return 1;
    unsigned total_frames = 0, sessions = 0;
    size_t least_headroom = expected_heap_capacity, highest_picture = 0;
    bool passed = true;
    for (unsigned cycle = 0; cycle < 3 && passed; ++cycle) {
        Player player;
        Player::VideoFrame frame, held;
        Player::Options options;
        options.url = argv[3]; options.start = cycle == 1 ? 1.0 : 0.0;
        player.open(options);
        const double deadline = seconds() + 20;
        const unsigned expected_frames = cycle == 2 ? 48 : 24;
        unsigned frames = 0;
        uint64_t held_digest = 0;
        while (seconds() < deadline && frames < expected_frames) {
            if (player.take_frame(frame)) {
                passed &= require(frame.w == 3840 && frame.h == 2160 && frame.wide &&
                    frame.layout == Player::VideoFrame::Layout::Planar420, "Decoded HEVC 4K10 format was lost");
                passed &= require(frame.colors == YuvColors::Hdr10 && frame.size() >= 23 * MiB,
                    "The output lost its full-resolution 10-bit HDR planes");
                if (frames == 0) {
                    held = frame;
                    for (size_t i = 0; i < held.size(); i += 4093) held_digest = held_digest * 131 + held.data()[i];
                }
                stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
                least_headroom = std::min(least_headroom, heap_free.load());
                { std::lock_guard lock(mapping_mutex); highest_picture = std::max(highest_picture, picture_peak); }
                ++frames; ++total_frames;
            }
            if (player.state() == Player::State::Failed || allocation_failures) break;
            SDL_Delay(5);
        }
        const double position = player.position();
        uint64_t actual_digest = 0;
        if (held.storage) for (size_t i = 0; i < held.size(); i += 4093) actual_digest = actual_digest * 131 + held.data()[i];
        passed &= require(frames >= expected_frames, "HEVC playback did not deliver the expected frames");
        if (frames) {
            passed &= require(position > options.start + .5, "HEVC playback position did not advance after resume");
            passed &= require(held.storage && actual_digest == held_digest, "A retained render-frame lease was overwritten");
        }
        player.close();
        // The renderer may own a frame across stop; its storage must remain valid.
        if (held.storage) {
            actual_digest = 0;
            for (size_t i = 0; i < held.size(); i += 4093) actual_digest = actual_digest * 131 + held.data()[i];
            passed &= require(actual_digest == held_digest, "Stopping playback freed a retained render frame");
        }
        held = {}; frame = {};
        stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
#ifndef STREMIO_MEMORY_BASELINE
        stremio_video_memory_stats video_memory{};
        stremio_video_memory_get_stats(&video_memory);
        passed &= require(video_memory.direct_bytes == picture_live &&
            video_memory.direct_allocations == picture_allocations &&
            !video_memory.active_mappings && !video_memory.retired_mappings,
            "Production picture ownership counters disagree with actual host mappings");
#endif
        passed &= require(picture_live == 0 && picture_allocations == picture_releases,
            "Stopping HEVC playback leaked an external picture mapping");
        passed &= require(mapping_errors == 0, "Picture memory was freed through the wrong ownership path");
        ++sessions;
        std::printf("PLAYER_MEMORY_SESSION cycle=%u frames=%u position=%.3f heap_used=%zu heap_free=%zu picture_peak=%zu failures=%u\n",
            cycle, frames, position, heap_used.load(), heap_free.load(), picture_peak, allocation_failures.load());
        std::fflush(stdout);
    }
    __wrap_free(pressure);
    passed &= require(allocation_failures == 0, "A real playback allocation exhausted the native arena");
    passed &= require(highest_picture >= 400 * MiB && highest_picture <= 512 * MiB,
        "Real VLC did not use the bounded external picture budget");
    passed &= require(least_headroom >= 192 * MiB, "Playback left inadequate measured arena headroom");
    std::printf("PLAYER_MEMORY_RESULT {\"result\":\"%s\",\"checks\":%u,\"sessions\":%u,\"frames\":%u,\"heap_capacity\":%zu,"
        "\"pressure_bytes\":%zu,\"heap_peak\":%zu,\"minimum_heap_free\":%zu,\"picture_peak\":%zu,\"picture_allocations\":%u,"
        "\"picture_releases\":%u,\"mapping_errors\":%u,\"allocation_failures\":%u,\"first_failed_bytes\":%zu,"
        "\"first_failed_operation\":%u}\n",
        passed ? "PASS" : "FAIL", checks, sessions, total_frames, heap_capacity.load(), pressure_bytes,
        heap_peak.load(), total_frames ? least_headroom : 0, highest_picture, picture_allocations, picture_releases,
        mapping_errors, allocation_failures.load(), first_failed_bytes.load(), first_failed_operation.load());
    diagnostics_stop(); SDL_Quit();
    return passed ? 0 : 1;
}
