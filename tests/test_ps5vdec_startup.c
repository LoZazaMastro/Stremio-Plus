/* Fault injection for the production PS5 decoder startup and memory ownership. */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int64_t mtime_t;
typedef pthread_mutex_t vlc_mutex_t;
#define VLC_STATIC_MUTEX PTHREAD_MUTEX_INITIALIZER
#define vlc_mutex_lock pthread_mutex_lock
#define vlc_mutex_unlock pthread_mutex_unlock
#define VLC_CODEC_H264 1u
#define VLC_CODEC_HEVC 2u
#define VLC_CODEC_NV12 3u
#define VIDEO_ES 1
#define VLC_EGENERIC (-1)
#define VLC_ENOMEM (-12)
#define VLC_SUCCESS 0
typedef struct decoder_sys_t decoder_sys_t;
typedef struct {
    unsigned i_width, i_height, i_chroma;
    unsigned i_visible_width, i_visible_height, i_frame_rate, i_frame_rate_base;
} video_format_t;
typedef struct {
    unsigned i_codec;
    int i_profile, i_level, i_cat, i_extra;
    void *p_extra;
    video_format_t video;
} es_format_t;
typedef struct block_t { int unused; } block_t;
typedef struct decoder_t {
    decoder_sys_t *p_sys;
    es_format_t fmt_in, fmt_out;
    int (*pf_decode)(struct decoder_t *, block_t *);
    void (*pf_flush)(struct decoder_t *);
} decoder_t;
typedef decoder_t vlc_object_t;

static unsigned checks, cases, case_checks;
static const char *current_case;
#define CHECK(condition) do { \
    ++checks; ++case_checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s line %d: %s\n", current_case, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static bool enabled, available;
static bool state_allocation_failure, header_allocation_failure;
static char messages[65536];
static size_t message_bytes;
static void capture_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(messages + message_bytes, sizeof(messages) - message_bytes, format, args);
    va_end(args);
    CHECK(n >= 0 && (size_t)n + 2 < sizeof(messages) - message_bytes);
    message_bytes += (size_t)n;
    messages[message_bytes++] = '\n';
    messages[message_bytes] = 0;
}
#define msg_Warn(object, ...) do { (void)(object); capture_message(__VA_ARGS__); } while (0)
#define msg_Err(object, ...) msg_Warn(object, __VA_ARGS__)
static bool var_InheritBool(decoder_t *dec, const char *name)
{
    (void)dec;
    CHECK(strcmp(name, "ps5vdec") == 0);
    return enabled;
}
static mtime_t mdate(void) { return 0; }
static int Decode(decoder_t *dec, block_t *block) { (void)dec; (void)block; return 0; }
static void Flush(decoder_t *dec) { (void)dec; }
static void es_format_Copy(es_format_t *dst, const es_format_t *src) { *dst = *src; }
static void *startup_calloc(size_t count, size_t size)
{
    return state_allocation_failure ? NULL : calloc(count, size);
}
static void *startup_malloc(size_t size)
{
    return header_allocation_failure ? NULL : malloc(size);
}

#define calloc startup_calloc
#define malloc startup_malloc
#include "ps5vdec_startup_under_test.h"
#undef calloc
#undef malloc

enum { MAX_ALLOCS = 128 };
typedef struct {
    bool direct, owned, mapped;
    size_t bytes;
    int64_t start;
    void *address;
    int protection;
} Allocation;
static Allocation allocations[MAX_ALLOCS];
static unsigned allocation_count;
static int queue_fault, layout_fault;
static bool flexible_failure, flexible_null, create_failure, create_null, query_failure;
static bool depth_failure, level_failure;
static bool avc_level52_query_failure, avc_level51_query_failure;
static unsigned expected_width, expected_height;
static VdecConfig attempted_configs[4];
static int allocation_failure_at, mapping_failure_at;
static unsigned direct_ordinal, map_ordinal;
static unsigned queue_queries, queue_creates, decoder_queries, decoder_creates, decoder_deletes;
static unsigned flexible_maps, direct_allocations, direct_releases, flexible_releases, unmaps;
static unsigned active_notifications, inactive_notifications;
static bool decoder_live;
static bool in_decoder_attempt;
static unsigned queue_owned;

static unsigned live_allocations(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < allocation_count; ++i)
        n += allocations[i].owned;
    return n;
}

static Allocation *new_allocation(bool direct, size_t bytes)
{
    CHECK(allocation_count < MAX_ALLOCS);
    Allocation *a = &allocations[allocation_count];
    *a = (Allocation){ .direct = direct, .owned = true, .bytes = bytes,
        .start = (int64_t)allocation_count * 0x1000000,
        .address = (void *)(uintptr_t)(0x10000000ull + (uint64_t)allocation_count * 0x1000000) };
    ++allocation_count;
    return a;
}

static Allocation *by_start(int64_t start)
{
    for (unsigned i = 0; i < allocation_count; ++i)
        if (allocations[i].direct && allocations[i].start == start) return &allocations[i];
    CHECK(false);
    return NULL;
}

static Allocation *by_address(const void *address)
{
    for (unsigned i = 0; i < allocation_count; ++i)
        if (allocations[i].address == address) return &allocations[i];
    CHECK(false);
    return NULL;
}

int vlc_ps5_vdec_available(void) { return available; }
void vlc_ps5_vdec_active(int on)
{
    if (on) ++active_notifications;
    else ++inactive_notifications;
}
int64_t sceKernelGetDirectMemorySize(void) { return INT64_C(8) << 30; }
int sceKernelAllocateDirectMemory(int64_t first, int64_t last, size_t bytes,
                                  size_t alignment, int type, int64_t *start)
{
    CHECK(first == 0 && last == sceKernelGetDirectMemorySize());
    CHECK(bytes && !(bytes & (PAGE_16K - 1)));
    CHECK(alignment >= PAGE_16K && !(alignment & (alignment - 1)) && type == DIRECT_TYPE);
    ++direct_allocations;
    if (in_decoder_attempt) {
        ++direct_ordinal;
        if ((int)direct_ordinal == allocation_failure_at) return -1001;
    } else if (queue_fault == 2) return -2002;
    Allocation *a = new_allocation(true, bytes);
    *start = a->start;
    return 0;
}
int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags,
                             int64_t start, size_t alignment)
{
    (void)alignment;
    Allocation *a = by_start(start);
    CHECK(a->owned && !a->mapped && a->bytes == bytes && flags == 0);
    CHECK(protection == 0x33);
    if (in_decoder_attempt) {
        ++map_ordinal;
        if ((int)map_ordinal == mapping_failure_at) return -1002;
    } else if (queue_fault == 3) return -2003;
    a->mapped = true;
    a->protection = protection;
    *address = a->address;
    return 0;
}
int sceKernelReleaseDirectMemory(int64_t start, size_t bytes)
{
    Allocation *a = by_start(start);
    CHECK(a->owned && !a->mapped && a->bytes == bytes);
    a->owned = false;
    ++direct_releases;
    return 0;
}
int sceKernelMunmap(void *address, size_t bytes)
{
    Allocation *a = by_address(address);
    CHECK(a->direct && a->owned && a->mapped && a->bytes == bytes);
    a->mapped = false;
    ++unmaps;
    return 0;
}
int sceKernelMapFlexibleMemory(void **address, size_t bytes, int protection, int flags)
{
    CHECK(in_decoder_attempt && bytes && protection == 3 && flags == 0);
    CHECK(!(bytes & (PAGE_16K - 1)));
    ++flexible_maps;
    if (flexible_failure) return -1003;
    if (flexible_null) { *address = NULL; return 0; }
    Allocation *a = new_allocation(false, bytes);
    a->mapped = true;
    a->protection = protection;
    *address = a->address;
    return 0;
}
int sceKernelReleaseFlexibleMemory(void *address, size_t bytes)
{
    Allocation *a = by_address(address);
    CHECK(!a->direct && a->owned && a->mapped && a->bytes == bytes);
    a->mapped = a->owned = false;
    ++flexible_releases;
    return 0;
}
int sceVideodec2QueryComputeMemoryInfo(VdecComputeMemory *info)
{
    ++queue_queries;
    CHECK(!in_decoder_attempt && info->this_size == sizeof(*info));
    if (queue_fault == 1) return -2001;
    info->cpu_gpu_memory_size = queue_fault == 6 ? UINT64_MAX : 17000;
    return 0;
}
int sceVideodec2AllocateComputeQueue(const VdecComputeConfig *config,
                                     const VdecComputeMemory *memory, void **queue)
{
    ++queue_creates;
    CHECK(config->this_size == sizeof(*config));
    CHECK(memory->cpu_gpu_memory_size == 32768 && live_allocations() == 1);
    if (queue_fault == 4) return -2004;
    if (queue_fault == 5) { *queue = NULL; return 0; }
    *queue = (void *)(uintptr_t)0xc0000000;
    return 0;
}
int sceVideodec2QueryDecoderMemoryInfo(const VdecConfig *config, VdecMemory *memory)
{
    CHECK(live_allocations() == queue_owned && !decoder_live);
    CHECK(config->compute_queue == compute_queue && compute_queue);
    CHECK(config->this_size == sizeof(*config) && memory->this_size == sizeof(*memory));
    CHECK(config->max_frame_width == (int32_t)expected_width &&
          config->max_frame_height == (int32_t)expected_height);
    in_decoder_attempt = true;
    direct_ordinal = map_ordinal = 0;
    CHECK(decoder_queries < sizeof(attempted_configs) / sizeof(*attempted_configs));
    attempted_configs[decoder_queries] = *config;
    ++decoder_queries;
    if (query_failure) return -1004;
    if (config->codec_type == VDEC_CODEC_AVC &&
        ((avc_level52_query_failure && config->max_level == 52) ||
         (avc_level51_query_failure && config->max_level == 51)))
        return (int)UINT32_C(0x811d0200);
    memory->cpu_memory_size = 32769;
    memory->gpu_memory_size = 49153;
    memory->cpu_gpu_memory_size = 65537;
    memory->max_frame_buffer_size = 98305;
    memory->frame_buffer_alignment = PAGE_16K;
    if (layout_fault == 1) memory->frame_buffer_alignment = 0xc000;
    if (layout_fault == 2) memory->max_frame_buffer_size = 0;
    if (layout_fault == 3) memory->max_frame_buffer_size = UINT64_MAX;
    if (layout_fault == 4) memory->max_frame_buffer_size = UINT64_C(1) << 63;
    if (layout_fault == 5) memory->cpu_memory_size = UINT64_MAX;
    if (layout_fault == 6) memory->gpu_memory_size = UINT64_MAX;
    if (layout_fault == 7) memory->cpu_gpu_memory_size = UINT64_MAX;
    if (layout_fault == 8) memory->cpu_memory_size = memory->gpu_memory_size = memory->cpu_gpu_memory_size = 0;
    return 0;
}
int sceVideodec2CreateDecoder(const VdecConfig *config, const VdecMemory *memory, void **decoder)
{
    ++decoder_creates;
    CHECK(!decoder_live && !*decoder);
    if (memory->cpu_memory_size) {
        Allocation *a = by_address(memory->cpu_memory);
        CHECK(a->owned && a->mapped && a->bytes >= memory->cpu_memory_size);
        CHECK(a->direct == (flexible_failure || flexible_null));
        CHECK(a->protection == (a->direct ? 0x33 : 3));
    }
    if (memory->gpu_memory_size) CHECK(by_address(memory->gpu_memory)->mapped);
    if (memory->cpu_gpu_memory_size) CHECK(by_address(memory->cpu_gpu_memory)->mapped);
    if (create_failure || (depth_failure && config->decode_pipeline_depth == 4) ||
        (level_failure && config->max_level == 156)) return -1005;
    if (create_null) { *decoder = NULL; return 0; }
    *decoder = (void *)(uintptr_t)0xd0000000;
    decoder_live = true;
    return 0;
}
int sceVideodec2DeleteDecoder(void *decoder)
{
    CHECK(decoder == (void *)(uintptr_t)0xd0000000 && decoder_live);
    decoder_live = false;
    ++decoder_deletes;
    return 0;
}

static decoder_t new_decoder(void)
{
    return (decoder_t){ .fmt_in = { .i_codec = VLC_CODEC_HEVC, .i_profile = 2, .i_level = 153,
        .video = { .i_width = 3840, .i_height = 2176 } } };
}

static decoder_t avc_decoder(unsigned width, unsigned height, int profile, int level)
{
    expected_width = width ? (width + 15) & ~15u : 3840;
    expected_height = height ? (height + 15) & ~15u : 2176;
    return (decoder_t){ .fmt_in = { .i_codec = VLC_CODEC_H264, .i_profile = profile, .i_level = level,
        .video = { .i_width = width, .i_height = height,
            .i_visible_width = width, .i_visible_height = height,
            .i_frame_rate = 25, .i_frame_rate_base = 1 } } };
}

static void begin_case(const char *name, bool prepare)
{
    current_case = name;
    case_checks = 0;
    CHECK(!live_allocations() && !decoder_live);
    enabled = available = true;
    state_allocation_failure = header_allocation_failure = false;
    queue_fault = layout_fault = allocation_failure_at = mapping_failure_at = 0;
    flexible_failure = flexible_null = create_failure = create_null = query_failure = false;
    depth_failure = level_failure = false;
    avc_level52_query_failure = avc_level51_query_failure = false;
    expected_width = 3840;
    expected_height = 2176;
    memset(attempted_configs, 0, sizeof(attempted_configs));
    in_decoder_attempt = false;
    queue_queries = queue_creates = decoder_queries = decoder_creates = decoder_deletes = 0;
    flexible_maps = direct_allocations = direct_releases = flexible_releases = unmaps = 0;
    active_notifications = inactive_notifications = queue_owned = 0;
    allocation_count = 0;
    memset(allocations, 0, sizeof(allocations));
    compute_queue = NULL;
    queue_mem = (Mem){ .start = -1 };
    queue_prepared = false;
    queue_stage = "not-prepared";
    queue_status = 0;
    message_bytes = 0;
    messages[0] = 0;
    if (prepare) {
        vlc_ps5_vdec_prepare();
        queue_owned = live_allocations();
        CHECK(queue_owned == 1 && vlc_ps5_vdec_queue_status(NULL, NULL) == 1);
        vlc_ps5_vdec_prepare();
        CHECK(queue_queries == 1 && queue_creates == 1);
    }
}

static void finish_case(void)
{
    CHECK(!decoder_live && live_allocations() == queue_owned);
    mem_free(&queue_mem);
    mem_free(&queue_mem);
    CHECK(live_allocations() == 0);
    for (unsigned i = 0; i < allocation_count; ++i) CHECK(!allocations[i].mapped);
    CHECK(direct_releases + flexible_releases == allocation_count);
    printf("PASS\t%s\t%u\n", current_case, case_checks);
    ++cases;
}

static void opens_and_closes(decoder_t *dec)
{
    CHECK(OpenDecoder((vlc_object_t *)dec) == VLC_SUCCESS);
    CHECK(dec->p_sys && decoder_live && dec->pf_decode == Decode && dec->pf_flush == Flush);
    CHECK(strstr(messages, "ps5vdec: ready") != NULL);
    CHECK(active_notifications == 1 && inactive_notifications == 0);
    CloseDecoder((vlc_object_t *)dec);
    dec->p_sys = NULL;
    CHECK(active_notifications == 1 && inactive_notifications == 1 && decoder_deletes == 1);
}

static void rejects(decoder_t *dec, const char *stage)
{
    CHECK(OpenDecoder((vlc_object_t *)dec) != VLC_SUCCESS);
    CHECK(!dec->p_sys && !decoder_live && !active_notifications && !inactive_notifications);
    CHECK(strstr(messages, stage) != NULL);
    CHECK(live_allocations() == queue_owned);
}

int main(void)
{
    decoder_t dec;
    begin_case("flexible workspace succeeds", true);
    dec = new_decoder();
    opens_and_closes(&dec);
    CHECK(flexible_maps == 1 && flexible_releases == 1 && decoder_queries == 1);
    CHECK(strstr(messages, "cpu-backing=flexible") != NULL);
    finish_case();

    for (int null_success = 0; null_success < 2; ++null_success) {
        begin_case(null_success ? "null flexible mapping falls back to direct" : "flexible failure falls back to direct 0x33", true);
        flexible_null = null_success;
        flexible_failure = !null_success;
        dec = new_decoder();
        opens_and_closes(&dec);
        CHECK(!flexible_releases && decoder_queries == 1);
        CHECK(strstr(messages, "cpu-backing=direct") && strstr(messages, "CPU workspace fallback"));
        finish_case();
    }

    for (int map = 0; map < 2; ++map) {
        for (int slot = 1; slot <= 12; ++slot) {
            char name[100];
            snprintf(name, sizeof(name), "%s failure at direct allocation %d cleans every attempt", map ? "mapping" : "physical", slot);
            begin_case(name, true);
            flexible_failure = true;
            if (map) mapping_failure_at = slot;
            else allocation_failure_at = slot;
            dec = new_decoder();
            rejects(&dec, map ? "stage=direct-map" : "stage=direct-allocate");
            CHECK(decoder_queries == 3 && decoder_creates == 0 && flexible_maps == 3);
            CHECK(unmaps + (map ? 3u : 0u) == direct_releases);
            finish_case();
        }
    }

    begin_case("GPU failure also releases flexible CPU workspace", true);
    allocation_failure_at = 1;
    dec = new_decoder();
    rejects(&dec, "component=gpu");
    CHECK(flexible_releases == 3 && decoder_queries == 3);
    finish_case();

    for (int mode = 0; mode < 3; ++mode) {
        const char *names[] = { "memory query failure", "decoder create failure", "decoder create returns null" };
        begin_case(names[mode], true);
        query_failure = mode == 0;
        create_failure = mode == 1;
        create_null = mode == 2;
        dec = new_decoder();
        rejects(&dec, mode == 0 ? "stage=memory-query" : "stage=decoder-create");
        CHECK(decoder_queries == 3 && decoder_creates == (mode ? 3u : 0u));
        CHECK(!decoder_deletes);
        finish_case();
    }

    for (int level = 0; level < 2; ++level) {
        begin_case(level ? "lower decoder level succeeds after clean retries" : "lower pipeline depth succeeds after clean retry", true);
        depth_failure = !level;
        level_failure = level;
        dec = new_decoder();
        opens_and_closes(&dec);
        CHECK(decoder_queries == (level ? 3u : 2u));
        finish_case();
    }

    for (unsigned profile = 0; profile < 3; ++profile) {
        const int profiles[] = { 66, 77, 100 };
        char name[100];
        snprintf(name, sizeof(name), "1080p AVC profile %d retries supported level after query refusal", profiles[profile]);
        begin_case(name, true);
        avc_level52_query_failure = true;
        dec = avc_decoder(1920, 1080, profiles[profile], 41);
        opens_and_closes(&dec);
        CHECK(decoder_queries == 3 && decoder_creates == 1);
        CHECK(attempted_configs[0].max_level == 52 && attempted_configs[0].decode_pipeline_depth == 4);
        CHECK(attempted_configs[1].max_level == 52 && attempted_configs[1].decode_pipeline_depth == 1);
        CHECK(attempted_configs[2].max_level == 51 && attempted_configs[2].decode_pipeline_depth == 1);
        CHECK(attempted_configs[2].profile == 100 && attempted_configs[2].max_dpb_frame_count == -1);
        CHECK(strstr(messages, "input codec=H264 size=1920x1080 visible=1920x1080") != NULL);
        CHECK(strstr(messages, "input-level=41 rc=0x811d0200") != NULL);
        CHECK(strstr(messages, "decoder-level=51 pipeline=1") != NULL);
        finish_case();
    }

    begin_case("AVC level 5.1 and padded 1088 height remain eligible", true);
    avc_level52_query_failure = true;
    dec = avc_decoder(1920, 1088, 100, 51);
    opens_and_closes(&dec);
    CHECK(decoder_queries == 3 && attempted_configs[2].max_level == 51);
    finish_case();

    begin_case("AVC successful first configuration needs no retry", true);
    dec = avc_decoder(1920, 1080, 100, 41);
    opens_and_closes(&dec);
    CHECK(decoder_queries == 1 && attempted_configs[0].max_level == 52);
    finish_case();

    begin_case("AVC rejected level 5.1 exhausts bounded retries without allocation", true);
    avc_level52_query_failure = avc_level51_query_failure = true;
    dec = avc_decoder(1920, 1080, 100, 41);
    rejects(&dec, "stage=configurations-exhausted");
    CHECK(decoder_queries == 3 && decoder_creates == 0 && flexible_maps == 0);
    CHECK(direct_allocations == 1); /* The previously prepared compute queue only. */
    finish_case();

    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        const char *names[] = { "level 5.2", "unknown level", "zero level", "unknown profile",
            "width above 1080p bound", "height above 1088 bound", "unknown width", "unknown height" };
        char name[100];
        snprintf(name, sizeof(name), "AVC %s never lowers the decoder level", names[invalid]);
        begin_case(name, true);
        avc_level52_query_failure = true;
        dec = avc_decoder(invalid == 4 ? 2048 : invalid == 6 ? 0 : 1920,
                          invalid == 5 ? 1090 : invalid == 7 ? 0 : 1080,
                          invalid == 3 ? -1 : 100,
                          invalid == 0 ? 52 : invalid == 1 ? -1 : invalid == 2 ? 0 : 41);
        rejects(&dec, "stage=configurations-exhausted");
        CHECK(decoder_queries == 2 && decoder_creates == 0);
        CHECK(attempted_configs[1].max_level == 52);
        CHECK(strstr(messages, "level=52 -> 51") == NULL);
        finish_case();
    }

    for (unsigned failure = 0; failure < 2; ++failure) {
        begin_case(failure ? "AVC decoder create failure does not lower level" :
                   "AVC allocation failure does not lower level", true);
        if (failure) create_failure = true;
        else allocation_failure_at = 1;
        dec = avc_decoder(1920, 1080, 100, 41);
        rejects(&dec, failure ? "stage=decoder-create" : "component=gpu");
        CHECK(decoder_queries == 2 && decoder_creates == (failure ? 2u : 0u));
        CHECK(strstr(messages, "level=52 -> 51") == NULL);
        finish_case();
    }

    for (int layout = 1; layout <= 8; ++layout) {
        char name[80];
        snprintf(name, sizeof(name), "decoder memory layout %d", layout);
        begin_case(name, true);
        layout_fault = layout;
        dec = new_decoder();
        if (layout == 8) {
            opens_and_closes(&dec);
            CHECK(flexible_maps == 0 && strstr(messages, "cpu-backing=none"));
        } else {
            rejects(&dec, layout <= 4 ? "stage=memory-layout" : "stage=direct-size");
            CHECK(decoder_queries == 3 && decoder_creates == 0);
        }
        finish_case();
    }

    for (int excessive = 0; excessive < 2; ++excessive) {
        begin_case(excessive ? "frame count above cap is rejected before allocation" : "zero frame count is rejected before allocation", true);
        dec = new_decoder();
        decoder_sys_t *sys = calloc(1, sizeof(*sys));
        CHECK(sys);
        sys->frame_count = excessive ? MAX_FRAMES + 1 : 0;
        sys->cpu.start = sys->gpu.start = sys->cpu_gpu.start = sys->frames.start = -1;
        for (int i = 0; i < AU_SLOTS; ++i) sys->au[i].start = -1;
        VdecConfig config = { .this_size = sizeof(config), .compute_queue = compute_queue,
            .max_frame_width = 3840, .max_frame_height = 2176 };
        VdecMemory memory;
        bool query_rejected = true;
        CHECK(bring_up(&dec, sys, &config, &memory, &query_rejected) == EINVAL);
        CHECK(!query_rejected);
        CHECK(!decoder_creates && !flexible_maps && direct_allocations == 1);
        CHECK(strstr(messages, "stage=memory-layout"));
        free_sys(sys);
        finish_case();
    }

    const char *gate_names[] = { "disabled", "module-unavailable", "codec", "H264 High10", "H264 422", "H264 444",
        "H264 CAVLC444", "HEVC range extensions", "oversized width", "oversized height", "state allocation", "header allocation" };
    for (unsigned gate = 0; gate < sizeof(gate_names) / sizeof(*gate_names); ++gate) {
        begin_case(gate_names[gate], true);
        dec = new_decoder();
        const char *stage;
        uint8_t headers[] = { 0, 0, 0, 1, 2, 3, 4 };
        if (gate == 0) { enabled = false; stage = "stage=disabled"; }
        else if (gate == 1) { available = false; stage = "stage=module-unavailable"; }
        else if (gate == 2) { dec.fmt_in.i_codec = 999; stage = "stage=codec"; }
        else if (gate < 7) {
            static const int profiles[] = { 110, 122, 244, 44 };
            dec.fmt_in.i_codec = VLC_CODEC_H264;
            dec.fmt_in.i_profile = profiles[gate - 3];
            stage = "stage=profile";
        } else if (gate == 7) { dec.fmt_in.i_profile = 3; stage = "stage=profile"; }
        else if (gate == 8) { dec.fmt_in.video.i_width = 4096; stage = "stage=size"; }
        else if (gate == 9) { dec.fmt_in.video.i_height = 2304; stage = "stage=size"; }
        else if (gate == 10) { state_allocation_failure = true; stage = "stage=decoder-state"; }
        else {
            header_allocation_failure = true;
            dec.fmt_in.p_extra = headers;
            dec.fmt_in.i_extra = sizeof(headers);
            stage = "stage=headers";
        }
        rejects(&dec, stage);
        CHECK(decoder_queries == (gate == 11 ? 1u : 0u));
        CHECK(queue_queries == 1 && queue_creates == 1);
        finish_case();
    }

    begin_case("unprepared queue is never prepared by OpenDecoder", false);
    dec = new_decoder();
    rejects(&dec, "operation=not-prepared");
    CHECK(!queue_queries && !queue_creates && !decoder_queries);
    CHECK(vlc_ps5_vdec_queue_status(NULL, NULL) == 0);
    finish_case();

    for (int fault = 1; fault <= 6; ++fault) {
        char name[80];
        snprintf(name, sizeof(name), "queue failure %d stays cached without late retry", fault);
        begin_case(name, false);
        queue_fault = fault;
        vlc_ps5_vdec_prepare();
        CHECK(vlc_ps5_vdec_queue_status(NULL, NULL) == -1 && live_allocations() == 0);
        const char *stage = NULL;
        int native_code = 0;
        CHECK(vlc_ps5_vdec_queue_status(&stage, &native_code) == -1 && stage && native_code);
        CHECK(vlc_ps5_vdec_queue_status(&stage, NULL) == -1);
        CHECK(vlc_ps5_vdec_queue_status(NULL, &native_code) == -1);
        vlc_ps5_vdec_prepare();
        dec = new_decoder();
        rejects(&dec, "stage=compute-queue");
        CHECK(strstr(messages, stage) != NULL);
        CHECK(queue_queries == 1 && queue_creates == ((fault == 4 || fault == 5) ? 1u : 0u));
        CHECK(!decoder_queries);
        finish_case();
    }
    printf("TOTAL\t%u\t%u\n", cases, checks);
    return 0;
}
