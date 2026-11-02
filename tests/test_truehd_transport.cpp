// Production transport, with VLC callbacks and Sony HDMI APIs mocked.
#include <fstream>
#include <cstdio>
#include <iterator>
#include <string>
#include "../src/player_vlc_passthrough.cpp"

static unsigned checks;
static void check(bool okay, const char* message) {
    ++checks;
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Sink {
    bool support = true, capture = false, configured = false, sys = false;
    int mode = 0, drain_error = 0, close_error = 0, config_error = 0;
    unsigned sys_opens = 0, ex_opens = 0, closes = 0, resets = 0, retries = 0, drains = 0;
    const uint8_t* in_flight = nullptr;
    std::vector<uint8_t> retained, bytes;
    bool overwritten = false;
} sink;
static void changed(void*, bool, const char* reason) {
    if (std::string(reason) == "output_failed") ++sink.retries;
}
static void pcm(void*, const void*, unsigned, int64_t) {}
extern "C" int sceAudioOutSysGetHdmiMonitorInfo(int port, void* bytes, unsigned size) {
    check(port == 1 && size == 0x180, "bounded HDMI capability probe");
    std::memset(bytes, 0, size);
    uint32_t count = sink.support ? 1 : 0;
    std::memcpy(static_cast<uint8_t*>(bytes) + 0x94, &count, 4);
    static_cast<uint8_t*>(bytes)[0x98] = 12;
    return 0;
}
extern "C" int sceAudioOutSysOpen(int user, int mode) {
    check(user == 255 && mode == 5, "TrueHD uses system MAT mode 5");
    ++sink.sys_opens; sink.sys = true; sink.mode = mode; return 7;
}
extern "C" int sceAudioOutExOpen(int, int) { ++sink.ex_opens; return 7; }
extern "C" int sceAudioOutSysConfigureOutput(int first, unsigned second, int mode, int enabled, uint64_t reserved) {
    check(first == 1 && !second && mode == 5 && enabled == 1 && !reserved, "system configure ABI");
    sink.configured = !sink.config_error;
    return sink.config_error;
}
extern "C" int sceAudioOutExConfigureOutput(int, unsigned, int mode, int, uint64_t) {
    check(mode == 255 && !sink.in_flight, "PCM reset follows confirmed drain and close");
    ++sink.resets; sink.configured = false; return 0;
}
extern "C" int sceAudioOutSysClose(int handle) {
    check(handle == 7, "close exact system port handle");
    ++sink.closes;
    if (sink.close_error) return sink.close_error;
    check(!sink.in_flight, "port closes only after submitted grain is drained");
    return 0;
}
extern "C" int sceAudioOutExClose(int) { check(false, "TrueHD cannot use ExClose"); return -1; }
extern "C" int sceAudioOutOutput(int handle, const void* data) {
    check(handle == 7 && sink.configured, "output only after native configuration");
    if (!data) {
        ++sink.drains;
        if (sink.drain_error) return sink.drain_error;
    }
    if (sink.in_flight && std::memcmp(sink.in_flight, sink.retained.data(), sink.retained.size())) sink.overwritten = true;
    sink.in_flight = nullptr;
    if (data) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        if (sink.capture) sink.bytes.insert(sink.bytes.end(), bytes, bytes + 16384);
        sink.in_flight = bytes; sink.retained.assign(bytes, bytes + 16384);
    }
    return 0;
}
static std::vector<uint8_t> read(const char* path) {
    std::ifstream stream(path, std::ios::binary); check(bool(stream), "fixture exists");
    return {std::istreambuf_iterator<char>(stream), {}};
}
static void send(audio_output_t* output, const uint8_t* bytes, size_t count) {
    auto* block = new block_t{};
    block->p_buffer = static_cast<uint8_t*>(std::malloc(count));
    std::memcpy(block->p_buffer, bytes, count); block->i_buffer = count; block->i_pts = VLC_TICK_INVALID;
    play(output, block);
}
struct Output {
    aout_sys_t state;
    audio_output_t value{};
    Output() { sink = Sink{}; last_hdmi_reset = {}; value.sys = &state; state.mode = changed; state.pcm = pcm; }
    int open(unsigned rate = 48000, vlc_fourcc_t codec = VLC_CODEC_TRUEHD) {
        audio_sample_format_t format{}; format.i_format = codec; format.i_rate = rate;
        return start(&value, &format);
    }
    ~Output() {
        if (state.shutdown_failed) {
            // Test-only process termination model; production quarantines this
            // owner until process exit, which the assertions below verify.
            hdmi_owner = nullptr; pcm_available = true; sink.in_flight = nullptr;
        } else stop(&value);
    }
};
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const auto input = read(argv[1]), reference = read(argv[2]);
    const auto resumed = read(argv[3]), resumed_reference = read(argv[4]);
    size_t split = 0;
    for (unsigned au = 0; au < 30; ++au) {
        const auto frame = pt::parse_frame(pt::Codec::truehd, input, split);
        check(frame.size > 0, "count complete access units for pause point"); split += frame.size;
    }
    {
        Output output;
        check(output.open() == VLC_SUCCESS && sink.sys_opens == 1 && !sink.ex_opens, "negotiate advertised TrueHD MAT");
        sink.capture = true;
        send(&output.value, input.data(), split);
        check(output.state.stream.size() == 12288, "first MAT retains exactly three quarters of one grain");
        const auto tail = output.state.stream;
        output.state.next_pts = 5000000;
        pause(&output.value, true, 1000);
        check(output.state.stream == tail && !sink.in_flight, "pause retains exact unsent MAT tail and drains native pointer");
        pause(&output.value, false, 4000);
        check(output.state.next_pts == 5003000, "resume shifts retained audio PTS by the paused interval");
        output.state.next_pts = VLC_TICK_INVALID;
        send(&output.value, input.data() + split, input.size() - split);
        flush(&output.value, true);
        check(sink.bytes.size() >= reference.size() && std::equal(reference.begin(), reference.end(), sink.bytes.begin()),
              "pause/resume preserves independent FFmpeg MAT byte sequence");
        check(output.state.stream.empty() && output.state.pending.empty() && !sink.in_flight && !sink.overwritten,
              "drain submits retained tail without mutating deferred native grains");
        sink.capture = false;
        stop(&output.value);
        check(sink.closes == 1 && sink.resets == 1 && stremio_ps5_audio_pcm_available(), "TrueHD shutdown restores PCM once");
    }
    {
        Output output; check(output.open() == 0, "start seek scenario"); sink.capture = true;
        send(&output.value, input.data(), split);
        flush(&output.value, false);
        check(sink.bytes.size() == 65536 && std::equal(reference.begin(), reference.begin() + 61440, sink.bytes.begin()),
              "seek completes already-built MAT tail before resetting timeline");
        check(output.state.stream.empty() && !sink.in_flight, "seek leaves no deferred grain");
        sink.bytes.clear();
        send(&output.value, input.data(), input.size()); flush(&output.value, true);
        check(std::equal(reference.begin(), reference.end(), sink.bytes.begin()), "seek restart begins a fresh major-sync MAT timeline");
    }
    {
        Output output; check(output.open(96000) != 0 && !sink.sys_opens, "96kHz stays PCM without opening MAT port");
        sink.support = false; check(output.open() != 0 && !sink.sys_opens, "unadvertised MAT remains PCM");
    }
    {
        Output output; sink.config_error = -44;
        check(output.open() != 0 && sink.closes == 1 && sink.resets == 1, "Sys configuration failure closes owned port and restores PCM");
    }
    {
        Output output; check(output.open() == 0, "start native drain fault");
        send(&output.value, input.data(), split); sink.drain_error = -55;
        stop(&output.value);
        check(output.state.shutdown_failed && hdmi_owner == &output.state && !stremio_ps5_audio_pcm_available(),
              "failed Sys drain quarantines owner and native grains");
        check(!sink.closes && !sink.resets && sink.in_flight && !sink.overwritten,
              "failed drain neither closes nor reconfigures a live port");
    }
    {
        Output output; check(output.open() == 0, "start malformed input scenario");
        const uint8_t invalid[] = {0, 1}; send(&output.value, invalid, 2); send(&output.value, invalid, 2);
        check(output.state.failed && sink.retries == 1, "malformed MAT requests one PCM restart");
    }
    {
        Output output; check(output.open() == 0, "start resumed minor-sync stream"); sink.capture = true;
        const auto first = pt::parse_frame(pt::Codec::truehd, resumed, 0);
        output.state.next_pts = 1;
        send(&output.value, resumed.data(), first.size);
        check(!output.state.failed && !sink.retries && sink.bytes.empty() && output.state.next_pts == 834,
              "minor-sync startup waits safely and advances exact skipped-audio PTS");
        output.state.next_pts = VLC_TICK_INVALID;
        send(&output.value, resumed.data() + first.size, resumed.size() - first.size);
        flush(&output.value, true);
        check(!output.state.failed && sink.bytes.size() >= resumed_reference.size() &&
              std::equal(resumed_reference.begin(), resumed_reference.end(), sink.bytes.begin()),
              "resumed system-port bytes match independent FFmpeg after bounded major-sync recovery");
    }
    std::printf("{\"result\":\"PASS\",\"checks\":%u,\"scenarios\":7}\n", checks);
}
