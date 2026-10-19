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
#include <thread>

extern "C" {
void* __wrap_malloc(size_t);
void __wrap_free(void*);
int __wrap_posix_memalign(void**, size_t, size_t);
void __real_stremio_diagnostics_allocation_failure(unsigned, size_t, size_t, uintptr_t);
void __real_stremio_diagnostics_heap_snapshot(unsigned, size_t, size_t, size_t, size_t, uintptr_t);
// Added by the test-only translation unit that includes the unmodified heap.
void stremio_test_heap_probe(size_t*, size_t*, size_t*);
}

namespace {
constexpr size_t MiB = size_t(1) << 20;
enum class Kind { Arena, Graphics, Dynamic };
struct Mapping { int64_t offset; size_t length; void* address; Kind kind; bool live; };
Mapping mappings[512]{};
std::mutex mapping_mutex;
size_t direct_live = 0, direct_peak = 0, dynamic_live = 0, dynamic_peak = 0, arena_bytes = 0;
unsigned dynamic_allocations = 0, dynamic_releases = 0, mapping_errors = 0;
bool reserving_graphics = false;
std::atomic<size_t> heap_capacity{0}, heap_used{0}, heap_free{0}, heap_peak{0};
std::atomic<unsigned> allocation_failures{0};
std::atomic<size_t> first_failed_bytes{0};
std::atomic<unsigned> first_failed_operation{0};
std::atomic<size_t> failure_heap_used{0}, failure_heap_free{0}, failure_heap_largest{0};
std::atomic<size_t> failure_direct_free{0}, failure_direct_largest{0};
unsigned checks = 0;
size_t direct_capacity() {
    // getenv/strtoull do not use C++ allocation; this is safe before main.
    const char* value = std::getenv("STREMIO_TEST_DIRECT_CAPACITY_MIB");
    return (value ? std::strtoull(value, nullptr, 10) : 2304) * MiB;
}
size_t largest_gap(int64_t* start, size_t alignment = 0x4000, size_t low = 0, size_t high = 0) {
    if (!high || high > direct_capacity()) high = direct_capacity();
    size_t best = 0, cursor = low;
    while (cursor < high) {
        const Mapping* next = nullptr;
        for (const auto& entry : mappings) if (entry.live && size_t(entry.offset) + entry.length > cursor &&
            size_t(entry.offset) < high &&
            (!next || entry.offset < next->offset)) next = &entry;
        const size_t end = next ? size_t(next->offset) : high;
        const size_t aligned = (cursor + alignment - 1) & ~(alignment - 1);
        if (end > aligned && end - aligned > best) { best = end - aligned; *start = int64_t(aligned); }
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
uint64_t digest(const Player::VideoFrame& frame) {
    uint64_t result = 0;
    if (frame.storage) for (size_t i = 0; i < frame.size(); i += 4093)
        result = result * 131 + frame.data()[i];
    return result;
}
}

// PS5 syscall boundaries are backed by real, separately owned host mappings.
// The production allocators and the real VLC/FFmpeg picture pools run unchanged.
extern "C" int64_t sceKernelGetDirectMemorySize() { return direct_capacity(); }
extern "C" int sceKernelAvailableDirectMemorySize(int64_t low, int64_t high, size_t alignment,
    int64_t* offset, size_t* size) {
    std::lock_guard lock(mapping_mutex);
    if (low < 0 || high <= low || alignment < 0x4000) return EINVAL;
    *offset = low; *size = largest_gap(offset, alignment, size_t(low), size_t(high));
    return 0;
}
extern "C" int sceKernelAllocateDirectMemory(int64_t low, int64_t high, size_t length, size_t alignment,
    int, int64_t* offset) {
    std::lock_guard lock(mapping_mutex);
    if (!length || alignment < 0x4000 || length > direct_capacity() - direct_live) return ENOMEM;
    int64_t start = 0;
    if (low < 0 || high <= low || largest_gap(&start, alignment, size_t(low), size_t(high)) < length) return ENOMEM;
    for (unsigned i = 0; i < std::size(mappings); ++i) if (!mappings[i].live) {
        const Kind kind = reserving_graphics ? Kind::Graphics : arena_bytes ? Kind::Dynamic : Kind::Arena;
        mappings[i] = {start, length, nullptr, kind, true};
        if (kind == Kind::Arena) arena_bytes = length;
        *offset = mappings[i].offset;
        direct_live += length; direct_peak = std::max(direct_peak, direct_live);
        if (kind == Kind::Dynamic) {
            dynamic_live += length; dynamic_peak = std::max(dynamic_peak, dynamic_live); ++dynamic_allocations;
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
        if (entry.kind == Kind::Dynamic) { dynamic_live -= length; ++dynamic_releases; }
        entry = {}; return 0;
    }
    ++mapping_errors; return EINVAL;
}
extern "C" int sceKernelDebugOutText(int, const char*) { return 0; }
extern "C" void __wrap_stremio_diagnostics_allocation_failure(unsigned operation, size_t size, size_t alignment, uintptr_t caller) {
    if (allocation_failures.fetch_add(1) == 0) {
        first_failed_operation = operation; first_failed_bytes = size;
        // The failed allocator call has released its lock. This probe reads
        // actual allocator state, unlike the console's cached crash snapshot.
        size_t used = 0, available = 0, largest = 0;
        stremio_test_heap_probe(&used, &available, &largest);
        failure_heap_used = used; failure_heap_free = available; failure_heap_largest = largest;
        std::lock_guard lock(mapping_mutex);
        int64_t start = 0;
        failure_direct_free = direct_capacity() - direct_live;
        failure_direct_largest = largest_gap(&start);
    }
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
    const size_t pressure_bytes = (argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 512) * MiB;
    const size_t graphics_bytes = (argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 384) * MiB;
    const unsigned cycle_count = argc > 6 ? unsigned(std::strtoul(argv[6], nullptr, 10)) : 3;
    const unsigned expected_frames = argc > 7 ? unsigned(std::strtoul(argv[7], nullptr, 10)) : 72;
    const double cycle_timeout = argc > 8 ? std::strtod(argv[8], nullptr) : 35.0;
    const bool split_graphics = argc <= 9 || std::strcmp(argv[9], "split") == 0;
    // Small resident blocks keep the pressure in the ordinary arena even when
    // the production allocator moves large codec buffers to direct memory.
    constexpr size_t pressure_block = 64 * 1024;
    static void* pressure[16384]{};
    if (pressure_bytes > sizeof(pressure) / sizeof(*pressure) * pressure_block) return 2;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (!require(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0, "SDL did not initialize")) return 1;
    diagnostics_start(std::string(argv[1]) + "/diagnostics");
    http_init(std::string(argv[2]) + "/ca-bundle.crt");
    const size_t pressure_blocks = (pressure_bytes + pressure_block - 1) / pressure_block;
    bool passed = true;
    for (size_t i = 0; i < pressure_blocks; ++i) {
        pressure[i] = __wrap_malloc(pressure_block);
        if (!pressure[i]) { passed = false; break; }
        std::memset(pressure[i], int(i & 255), pressure_block);
    }
    stremio_heap_snapshot(STREMIO_HEAP_APP_INITIALIZED);
    const size_t app_heap_used = heap_used;
    passed &= require(passed, "Application/network pressure did not fit the native arena");
    passed &= require(heap_capacity == arena_bytes && arena_bytes > 0,
        "Reported arena size disagrees with the actual direct-memory mapping");
    int64_t graphics_offset = -1;
    void* graphics = nullptr;
    if (graphics_bytes) {
        reserving_graphics = true;
        // A center mapping leaves two usable direct-memory extents. This models
        // driver fragmentation without increasing the driver's total budget.
        const size_t begin = split_graphics ? arena_bytes +
            ((direct_capacity() - arena_bytes - graphics_bytes) / 2 & ~size_t(0x3fff)) : 0;
        const size_t end = split_graphics ? begin + graphics_bytes : direct_capacity();
        int rc = sceKernelAllocateDirectMemory(begin, end, graphics_bytes, 0x4000, 11, &graphics_offset);
        if (!rc) rc = sceKernelMapDirectMemory(&graphics, graphics_bytes, 3, 0, graphics_offset, 0x4000);
        reserving_graphics = false;
        passed &= require(rc == 0 && graphics, "Driver direct-memory reservation did not fit");
    }
    unsigned total_frames = 0, sessions = 0, progressed_frames = 0, content_changes = 0;
    size_t least_headroom = heap_capacity, first_frame_heap_used = 0, peak_live_after_close = 0;
    size_t stopped_heap_first = 0, stopped_heap_last = 0;
    Player::VideoFrame carry;
    uint64_t carry_digest = 0;
    std::printf("PLAYER_MEMORY_BUDGET capacity=%zu arena=%zu app_pressure=%zu app_heap_used=%zu graphics=%zu layout=%s\n",
        direct_capacity(), heap_capacity.load(), pressure_bytes, app_heap_used, graphics_bytes,
        split_graphics ? "split" : "contiguous");
    for (unsigned cycle = 0; cycle < cycle_count && passed; ++cycle) {
        Player player;
        Player::VideoFrame frame, held;
        Player::Options options;
        options.url = argv[3]; options.start = cycle == 1 ? 1.0 : 0.0;
        player.open(options);
        const double deadline = seconds() + cycle_timeout;
        unsigned frames = 0;
        uint64_t held_digest = 0, previous_digest = 0;
        double previous_position = -1;
        while (seconds() < deadline && frames < expected_frames) {
            if (player.take_frame(frame)) {
                passed &= require(frame.w == 3840 && frame.h == 2160 && frame.wide &&
                    frame.layout == Player::VideoFrame::Layout::Planar420, "Decoded HEVC 4K10 format was lost");
                passed &= require(frame.colors == YuvColors::Hdr10 && frame.size() >= 23 * MiB,
                    "The output lost its full-resolution 10-bit HDR planes");
                if (frames == 0) {
                    held = frame;
                    held_digest = digest(held);
                    if (carry.storage) passed &= require(digest(carry) == carry_digest,
                        "A frame retained across player restart was overwritten or freed");
                    carry = {};
                }
                stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
                if (!first_frame_heap_used) first_frame_heap_used = heap_used;
                least_headroom = std::min(least_headroom, heap_free.load());
                const double position = player.position();
                if (position > previous_position + 0.001) ++progressed_frames;
                const uint64_t frame_digest = digest(frame);
                if (frames && frame_digest != previous_digest) ++content_changes;
                previous_digest = frame_digest; previous_position = position;
                ++frames; ++total_frames;
            }
            if (player.state() == Player::State::Failed || allocation_failures) break;
            SDL_Delay(5);
        }
        const double position = player.position();
        const auto state = player.state();
        const std::string error = player.error();
        passed &= require(frames >= expected_frames, "HEVC playback did not deliver the expected frames");
        if (frames) {
            passed &= require(position > options.start + .5, "HEVC playback position did not advance after resume");
            passed &= require(held.storage && digest(held) == held_digest, "A retained render-frame lease was overwritten");
        }
        player.close();
        // The renderer may own a frame across stop; its storage must remain valid.
        if (held.storage) {
            passed &= require(digest(held) == held_digest, "Stopping playback freed a retained render frame");
        }
        if (passed && cycle + 1 < cycle_count) { carry = held; carry_digest = held_digest; }
        held = {}; frame = {};
        stremio_heap_snapshot(STREMIO_HEAP_VIDEO_PRESENTED);
        if (!cycle) stopped_heap_first = heap_used;
        stopped_heap_last = heap_used;
        peak_live_after_close = std::max(peak_live_after_close, dynamic_live);
        stremio_video_memory_stats video_memory{};
        stremio_video_memory_get_stats(&video_memory);
        passed &= require(video_memory.direct_bytes == dynamic_live &&
            video_memory.direct_allocations == dynamic_allocations && !video_memory.retired_mappings,
            "Production ownership counters disagree with actual direct mappings");
        if (!carry.storage) passed &= require(dynamic_live == 0 && dynamic_allocations == dynamic_releases,
            "Stopping HEVC playback leaked an external mapping after all leases were released");
        passed &= require(mapping_errors == 0, "Picture memory was freed through the wrong ownership path");
        ++sessions;
        std::printf("PLAYER_MEMORY_SESSION cycle=%u frames=%u position=%.3f state=%u heap_used=%zu heap_free=%zu dynamic_peak=%zu dynamic_live=%zu failures=%u\n",
            cycle, frames, position, unsigned(state), heap_used.load(), heap_free.load(), dynamic_peak,
            dynamic_live, allocation_failures.load());
        if (!error.empty()) std::printf("PLAYER_MEMORY_PLAYER_ERROR %s\n", error.c_str());
        std::fflush(stdout);
    }
    carry = {};
    for (void* block : pressure) __wrap_free(block);
    if (graphics) {
        passed &= require(sceKernelMunmap(graphics, graphics_bytes) == 0, "Driver reservation unmap failed");
        passed &= require(sceKernelReleaseDirectMemory(graphics_offset, graphics_bytes) == 0,
            "Driver reservation release failed");
    }
    passed &= require(allocation_failures == 0, "A real playback allocation exhausted the native arena");
    // VLC's position event is deliberately coarse; several changing frames can
    // share one position value. Require multiple real advances per session,
    // not a timestamp event for every picture.
    passed &= require(progressed_frames >= cycle_count * 3,
        "Playback position did not continue advancing across the sessions");
    passed &= require(content_changes > cycle_count * 8, "The fixture did not exercise changing decoded pictures");
    passed &= require(dynamic_live == 0 && dynamic_allocations == dynamic_releases,
        "External allocations remained after every render-frame lease was released");
    passed &= require(direct_peak <= direct_capacity(), "Allocator exceeded the physical direct-memory budget");
    const char* failure_kind = !allocation_failures ? "none" :
        failure_heap_free < first_failed_bytes ? "arena_exhaustion" :
        failure_heap_largest < first_failed_bytes ? "arena_fragmentation" : "other_or_concurrent_change";
    std::printf("PLAYER_MEMORY_RESULT {\"result\":\"%s\",\"checks\":%u,\"sessions\":%u,\"frames\":%u,\"heap_capacity\":%zu,"
        "\"pressure_bytes\":%zu,\"graphics_bytes\":%zu,\"graphics_layout\":\"%s\",\"direct_capacity\":%zu,\"direct_peak\":%zu,"
        "\"app_heap_used\":%zu,\"first_frame_heap_used\":%zu,\"heap_peak\":%zu,\"minimum_heap_free\":%zu,"
        "\"dynamic_peak\":%zu,\"dynamic_allocations\":%u,\"dynamic_releases\":%u,\"mapping_errors\":%u,"
        "\"post_close_dynamic_peak\":%zu,\"stopped_heap_first\":%zu,\"stopped_heap_last\":%zu,"
        "\"progressed_frames\":%u,\"content_changes\":%u,\"allocation_failures\":%u,\"first_failed_bytes\":%zu,"
        "\"first_failed_operation\":%u,\"failure_heap_used\":%zu,\"failure_heap_free\":%zu,\"failure_heap_largest\":%zu,"
        "\"failure_direct_free\":%zu,\"failure_direct_largest\":%zu,\"failure_kind\":\"%s\"}\n",
        passed ? "PASS" : "FAIL", checks, sessions, total_frames, heap_capacity.load(), pressure_bytes,
        graphics_bytes, split_graphics ? "split" : "contiguous", direct_capacity(), direct_peak,
        app_heap_used, first_frame_heap_used,
        heap_peak.load(), total_frames ? least_headroom : 0, dynamic_peak, dynamic_allocations, dynamic_releases,
        mapping_errors, peak_live_after_close, stopped_heap_first, stopped_heap_last, progressed_frames, content_changes,
        allocation_failures.load(), first_failed_bytes.load(), first_failed_operation.load(),
        failure_heap_used.load(), failure_heap_free.load(), failure_heap_largest.load(),
        failure_direct_free.load(), failure_direct_largest.load(), failure_kind);
    diagnostics_stop(); SDL_Quit();
    return passed ? 0 : 1;
}
