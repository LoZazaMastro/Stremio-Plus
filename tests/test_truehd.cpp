// Compare MAT against an independent FFmpeg muxer, including fragmented input.
#include "player_iec61937.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static unsigned checks;
static void check(bool okay, const char* message) {
    ++checks;
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static std::vector<uint8_t> read(const char* name) {
    std::ifstream file(name, std::ios::binary);
    check(bool(file), "open fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}
static std::vector<uint8_t> packed(const std::vector<uint8_t>& input, size_t fragment,
                                   bool drain, std::vector<uint8_t>* before_drain = nullptr) {
    pt::Packer packer(pt::Codec::truehd);
    std::vector<uint8_t> pending, output;
    const char* error = nullptr;
    for (size_t at = 0; at < input.size(); at += fragment) {
        pending.insert(pending.end(), input.begin() + at, input.begin() + std::min(input.size(), at + fragment));
        size_t cursor = 0;
        while (packer.next_burst(pending, &cursor, &output, &error, false)) {}
        check(error == nullptr && cursor <= pending.size(), "fragmented MAT input remains bounded");
        pending.erase(pending.begin(), pending.begin() + cursor);
    }
    check(pending.empty(), "consume every complete TrueHD access unit");
    if (before_drain) *before_drain = output;
    if (drain) {
        size_t cursor = 0;
        while (packer.next_burst(pending, &cursor, &output, &error, true)) {}
        check(error == nullptr, "drain final MAT frame");
        const size_t size = output.size();
        check(!packer.next_burst(pending, &cursor, &output, &error, true) && output.size() == size,
              "repeated drain cannot duplicate a frame");
    }
    return output;
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const auto input = read(argv[1]), reference = read(argv[2]);
    check(reference.size() >= 4 * 61440 && reference.size() % 61440 == 0, "independent multi-frame MAT reference");
    bool wrapped = false;
    uint16_t previous = 0;
    for (size_t at = 0; at < input.size();) {
        const auto frame = pt::parse_frame(pt::Codec::truehd, input, at);
        check(frame.size >= 4, "valid timed access-unit fixture");
        const uint16_t timing = uint16_t(unsigned(input[at + 2]) << 8 | input[at + 3]);
        if (at && timing < previous) wrapped = true;
        previous = timing; at += frame.size;
    }
    check(wrapped, "fixture crosses the 16-bit input-timing wrap");
    for (size_t fragment : {size_t(1), size_t(7), size_t(511), input.size()}) {
        std::vector<uint8_t> before;
        const auto output = packed(input, fragment, true, &before);
        check(before == reference, "MAT bytes exactly match FFmpeg including timing padding and boundary splits");
        check(output.size() >= reference.size() && output.size() <= reference.size() + 61440,
              "final drain adds at most one complete MAT frame");
        check(std::equal(reference.begin(), reference.end(), output.begin()), "drain preserves all reference bytes");
        if (output.size() > reference.size()) {
            const auto at = reference.size();
            check(output[at] == 0x72 && output[at + 1] == 0xf8 && output[at + 4] == 0x16,
                  "final drain has IEC61937 TrueHD preamble");
            check(output[at + 8 + 30708] == 0xc1 && output[at + 8 + 61408] == 0xc2,
                  "final drain preserves middle and end markers");
        }
    }
    const auto resumed = read(argv[3]), resumed_reference = read(argv[4]);
    check(resumed.size() > 10 && !(resumed[4] == 0xf8 && resumed[5] == 0x72 && resumed[6] == 0x6f),
          "resume fixture begins on a minor-sync access unit");
    for (size_t fragment : {size_t(1), size_t(511), resumed.size()})
        check(packed(resumed, fragment, false) == resumed_reference,
              "minor-sync resume finds major sync and matches independent FFmpeg bytes");
    {
        pt::Packer resync(pt::Codec::truehd);
        const auto first = pt::parse_frame(pt::Codec::truehd, resumed, 0);
        size_t at = 0; const char* reason = nullptr; std::vector<uint8_t> bytes;
        check(!resync.next_burst(pt::Span<uint8_t>(resumed).subspan(0, first.size), &at, &bytes, &reason) && !reason &&
              bytes.empty() && resync.take_startup_skipped_samples() == 40,
              "discarded minor unit reports its exact 40-sample PTS offset");
        std::vector<uint8_t> unsynced(20 * 1024, 0);
        for (size_t i = 0; i < unsynced.size(); i += 20) unsynced[i + 1] = 10;
        at = 0; resync.set_codec(pt::Codec::truehd);
        check(!resync.next_burst(unsynced, &at, &bytes, &reason) && reason && bytes.empty() && at <= unsynced.size(),
              "missing major sync reaches a bounded fallback without HDMI payload");
    }
    pt::Packer packer(pt::Codec::truehd);
    const char* error = nullptr;
    std::vector<uint8_t> malformed{0, 1}, output;
    size_t cursor = 0;
    check(!packer.next_burst(malformed, &cursor, &output, &error) && error && output.empty(),
          "reject an undersized access unit without emitting HDMI data");
    auto truncated = input; truncated.pop_back();
    cursor = 0; error = nullptr; packer.set_codec(pt::Codec::truehd);
    while (packer.next_burst(truncated, &cursor, &output, &error, true)) {}
    check(error != nullptr, "drain rejects an incomplete access unit");
    std::printf("{\"result\":\"PASS\",\"checks\":%u,\"referenceBytes\":%zu}\n", checks, reference.size());
}
