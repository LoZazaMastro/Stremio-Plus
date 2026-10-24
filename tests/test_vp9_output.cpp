// Production native VP9 input/output association with a deterministic hardware
// boundary. No PS5 API or actual hardware decoder executes on the host.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <string>
#define private public
#include "../src/hwdec_ps5.h"
#undef private
#define PLATFORM_PS5_NATIVE 1
#include "../src/hwdec_ps5.cpp"

#include <array>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
unsigned checks = 0, cases = 0;
void check(bool okay, const char* description) {
    ++checks;
    if (!okay) throw std::runtime_error(description);
}
struct NativePicture { std::array<uint8_t, 384> bytes; bool error = false; };
std::deque<NativePicture> native_pictures;
std::deque<size_t> native_pending;
unsigned output_delay = 0, decode_calls = 0, delete_calls = 0, create_calls = 0, reset_calls = 0;
int next_decode_result = 0, create_result = 0, reset_result = 0;
bool buffer_accepted = true, next_output_error = false;
int dummy_decoder;

void output_picture(SceVideodec2OutputInfo* out) {
    if (native_pending.empty()) return;
    auto& frame = native_pictures[native_pending.front()];
    native_pending.pop_front();
    out->valid = out->picture_count = 1;
    out->width = out->height = out->pitch = out->pitch_bytes = 16;
    out->buffer = frame.bytes.data(); out->buffer_size = frame.bytes.size();
    out->error = frame.error;
}
using Pictures = std::vector<std::pair<int, int64_t>>;
Pictures queued(const HwDecoder::Impl& decoder) {
    Pictures result;
    for (const auto& frame : decoder.ro)
        if (frame.used) result.emplace_back(frame.pic.y[0], frame.pic.pts_us);
    return result;
}

// The native boundary owns the mock picture bytes until reset. These tests
// verify PTS/show association, not Sony buffer submission or GPU ownership.
struct Fixture {
    Slot slot;
    std::vector<uint8_t> input = std::vector<uint8_t>(kInputSlots * kInputSlotBytes);
    std::vector<uint8_t> frames = std::vector<uint8_t>(kFrameSlots * 384);
    HwDecoder player;
    explicit Fixture(unsigned delay = 0, Kind kind = VP9) {
        native_pending.clear(); native_pictures.clear();
        output_delay = delay;
        decode_calls = delete_calls = create_calls = reset_calls = 0;
        next_decode_result = create_result = reset_result = 0;
        buffer_accepted = true; next_output_error = false;
        slot.input_mem = input.data(); slot.frame_mem = frames.data(); slot.frame_size = 384;
        slot.decoder = &dummy_decoder; slot.ready = slot.owned = true;
        player.d_ = new HwDecoder::Impl;
        state().slot = &slot; state().desc = &kCodecs[kind];
        state().disp_w = state().disp_h = 16;
    }
    HwDecoder::Impl& state() { return *player.d_; }
    int submit(int marker, int64_t pts, bool show = true) {
        const uint8_t packet[] = {uint8_t(show ? 0x82 : 0x80), uint8_t(marker), 0x10};
        return state().decode_one(packet, sizeof(packet), pts, show);
    }
    void drain() { check(player.send(nullptr, 0, INT64_MIN) == 0, "end-of-stream drain succeeded"); }
};
template<class F> void scenario(const char* name, F&& body) {
    const unsigned before = checks;
    body(); ++cases;
    std::cout << "PASS\t" << name << '\t' << checks - before << '\n';
}
}

void dlog(const char*, ...) {}
extern "C" {
int32_t sceVideodec2Decode(void*, SceVideodec2InputData* input,
    SceVideodec2FrameBuffer* frame, SceVideodec2OutputInfo* out) {
    ++decode_calls;
    frame->accepted = buffer_accepted;
    const int error = std::exchange(next_decode_result, 0);
    if (error) return error;
    NativePicture picture;
    picture.bytes.fill(static_cast<const uint8_t*>(input->au)[1]);
    picture.error = std::exchange(next_output_error, false);
    native_pictures.push_back(picture);
    native_pending.push_back(native_pictures.size() - 1);
    if (native_pending.size() > output_delay) output_picture(out);
    return 0;
}
int32_t sceVideodec2Flush(void*, SceVideodec2FrameBuffer*, SceVideodec2OutputInfo* out) {
    output_picture(out); return 0;
}
int32_t sceVideodec2DeleteDecoder(void*) { ++delete_calls; return 0; }
int32_t sceVideodec2CreateDecoder(const SceVideodec2DecoderConfigInfo*,
    const SceVideodec2DecoderMemoryInfo*, void** decoder) {
    ++create_calls;
    *decoder = create_result ? nullptr : &dummy_decoder;
    return create_result;
}
int32_t sceVideodec2Reset(void*) {
    ++reset_calls;
    native_pending.clear(); native_pictures.clear();
    return reset_result;
}
}

int main() {
    try {
        scenario("immediate visible and hidden output", [] {
            Fixture f;
            check(f.submit(11, 100) == 0 && f.submit(22, INT64_MIN, false) == 0 && f.submit(33, 200) == 0,
                  "immediate inputs accepted");
            check(queued(f.state()) == Pictures{{11, 100}, {33, 200}}, "hidden reference picture is never presented");
            check(f.state().vp9_count == 0 && f.state().pts_n == 0, "VP9 tags are independent of the AVC/HEVC PTS pool");
        });
        scenario("delayed output retains the output's show flag", [] {
            Fixture f(1);
            check(f.submit(11, 100) == 0 && f.submit(22, INT64_MIN, false) == 0,
                  "a visible frame followed by hidden input decodes");
            check(queued(f.state()) == Pictures{{11, 100}}, "hidden input must not discard an older visible output");
            check(f.submit(33, 200) == 0, "visible input after hidden reference decodes");
            check(queued(f.state()) == Pictures{{11, 100}}, "visible input must not expose an older hidden output");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 100}, {33, 200}}, "drain presents only the remaining visible output");
            check(f.state().vp9_count == 0, "drain consumes all output tags");
        });
        scenario("multiple hidden frames and unknown timestamps", [] {
            Fixture f(3);
            check(f.submit(11, INT64_MIN, false) == 0 && f.submit(22, 0, false) == 0 &&
                  f.submit(33, INT64_MIN) == 0 && f.submit(44, 300) == 0, "delayed reference sequence accepted");
            f.drain();
            check(queued(f.state()) == Pictures{{33, INT64_MIN}, {44, 300}},
                  "hidden and unknown-PTS entries cannot steal later visible timestamps");
        });
        scenario("FIFO association is independent of PTS sort order", [] {
            Fixture f(2);
            check(f.submit(11, 300) == 0 && f.submit(22, 100) == 0 && f.submit(33, 200) == 0,
                  "non-monotonic PTS inputs accepted");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 300}, {22, 100}, {33, 200}},
                  "each native picture keeps its own PTS rather than the smallest pending one");
        });
        scenario("errored native picture consumes its tag", [] {
            Fixture f(1);
            next_output_error = true;
            check(f.submit(11, 100) == 0 && f.submit(22, 200) == 0, "errored picture scenario accepted");
            check(queued(f.state()).empty(), "a native error picture is not shown");
            f.drain();
            check(queued(f.state()) == Pictures{{22, 200}}, "discarded error output does not shift the next timestamp");
        });
        scenario("EAGAIN does not submit or consume a tag", [] {
            Fixture f;
            for (int i = 0; i < kReorderSlots; ++i) check(f.submit(i + 1, (i + 1) * 100) == 0, "fill presentation queue");
            const unsigned calls = decode_calls;
            const uint8_t packet[] = {0x82, 6, 0x10};
            check(f.player.send(packet, sizeof(packet), 600) == 1, "full presentation queue returns EAGAIN");
            check(decode_calls == calls && f.state().vp9_count == 0, "EAGAIN leaves native decode and tag FIFO untouched");
            HwDecoder::Picture output;
            check(f.player.receive(&output) == 1 && output.pts_us == 100, "caller receives one queued picture");
            check(f.player.send(packet, sizeof(packet), 600) == 0 && decode_calls == calls + 1,
                  "retry submits exactly once after backpressure clears");
        });
        scenario("native rejection never adds an input tag", [] {
            Fixture f(1);
            check(f.submit(11, 100) == 0, "first accepted input waits for output");
            next_decode_result = -1;
            check(f.submit(22, 999) == 0, "existing bounded pre-picture error tolerance remains in force");
            check(f.state().vp9_count == 1 && f.state().fails == 1, "rejected AU adds no output metadata");
            check(f.submit(33, 200) == 0, "later valid input accepted");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 100}, {33, 200}}, "native rejection does not mislabel later output");
        });
        scenario("frame-buffer acceptance is not AU acceptance", [] {
            Fixture f(1);
            buffer_accepted = false;
            check(f.submit(11, 100) == 0 && f.state().vp9_count == 1,
                  "successful Decode queues its AU tag even when the supplied output buffer was not retained");
            buffer_accepted = true;
            check(f.submit(22, 200) == 0, "next AU succeeds");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 100}, {22, 200}}, "buffer ownership flag cannot shift AU timestamps");
        });
        scenario("bounded pending metadata fails before another submission", [] {
            Fixture f(std::numeric_limits<unsigned>::max());
            for (int i = 0; i < kPtsPool; ++i) check(f.submit(i, i * 100, i % 2 == 0) == 0, "fill bounded pending FIFO");
            const unsigned calls = decode_calls;
            check(f.submit(99, 9999) == -1 && f.state().fatal, "pending overflow takes normal software-fallback path");
            check(decode_calls == calls && f.state().vp9_count == kPtsPool,
                  "overflow neither submits extra input nor discards metadata");
        });
        scenario("ring wrap preserves visibility and timestamps", [] {
            Fixture f(1);
            for (int i = 0; i < kPtsPool * 4; ++i) {
                check(f.submit(i + 1, i * 100, false) == 0, "hidden references circulate through bounded ring");
                check(f.state().vp9_count == 1 && f.state().ro_count == 0, "wrapped hidden tag remains ordered");
            }
            check(f.submit(77, 9000) == 0, "visible input after several ring wraps");
            f.drain();
            check(queued(f.state()) == Pictures{{77, 9000}}, "ring wrapping preserves the final visible output");
        });
        scenario("seek flush clears pending visibility before decoder renewal", [] {
            Fixture f(2);
            check(f.submit(11, 100) == 0 && f.submit(22, 200, false) == 0, "pre-seek native inputs pending");
            auto* input = f.slot.input_mem; auto* frames = f.slot.frame_mem;
            f.player.flush();
            check(f.state().vp9_count == 0 && f.state().vp9_head == 0 && f.state().ro_count == 0,
                  "seek resets all VP9 associations and old pictures");
            check(delete_calls == 1 && create_calls == 1 && reset_calls == 1 &&
                  f.slot.input_mem == input && f.slot.frame_mem == frames,
                  "existing decoder-renewal and resident buffer policy is unchanged");
            check(f.submit(33, 300) == 0, "post-seek input accepted");
            f.drain();
            check(queued(f.state()) == Pictures{{33, 300}}, "pre-seek tags never appear after reset");
        });
        scenario("failed decoder renewal cannot reuse old metadata", [] {
            Fixture f(1);
            check(f.submit(11, 100) == 0, "old pending frame accepted");
            create_result = -1;
            f.player.flush();
            check(f.state().fatal && !f.slot.ready && !f.slot.decoder && f.state().vp9_count == 0,
                  "existing renewal failure leaves decoder failed with an empty VP9 FIFO");
            const uint8_t packet[] = {0x82, 22, 0x10};
            const unsigned calls = decode_calls;
            check(f.player.send(packet, sizeof(packet), 200) == -1 && decode_calls == calls,
                  "failed decoder does not submit another frame");
        });
        scenario("orphan output is rejected without a fabricated timestamp", [] {
            Fixture f;
            NativePicture orphan; orphan.bytes.fill(99);
            native_pictures.push_back(orphan); native_pending.push_back(0);
            check(f.player.send(nullptr, 0, INT64_MIN) == -1 && f.state().fatal,
                  "drain reports missing output metadata through the existing failure path");
            check(queued(f.state()).empty(), "orphan native output is never displayed");
        });
        scenario("real FFmpeg VP9 filter and no-packet EAGAIN", [] {
            Fixture f(1);
            f.state().bsf_par = avcodec_parameters_alloc();
            check(f.state().bsf_par != nullptr, "allocate real bitstream-filter parameters");
            f.state().bsf_par->codec_type = AVMEDIA_TYPE_VIDEO;
            f.state().bsf_par->codec_id = AV_CODEC_ID_VP9;
            f.state().bsf_par->width = f.state().bsf_par->height = 16;
            check(f.state().bsf_build(), "production VP9 superframe filter starts");
            f.state().in_pkt = av_packet_alloc(); f.state().out_pkt = av_packet_alloc();
            check(f.state().in_pkt && f.state().out_pkt, "real AVPacket allocation");
            check(f.state().feed_filtered() == 0 && decode_calls == 0 && f.state().vp9_count == 0,
                  "FFmpeg EAGAIN with no packet creates no native tag");
            const uint8_t a[] = {0x82, 11, 0x10}, hidden[] = {0x80, 22, 0x10}, c[] = {0x82, 33, 0x10};
            check(f.player.send(a, sizeof(a), 100) == 0 && f.player.send(hidden, sizeof(hidden), INT64_MIN) == 0 &&
                  f.player.send(c, sizeof(c), 200) == 0, "actual production filter classifies shown and hidden frames");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 100}, {33, 200}}, "real filter preserves delayed-output association");
        });
        scenario("real VP9 superframe splits hidden and shown AUs", [] {
            Fixture f(1);
            f.state().bsf_par = avcodec_parameters_alloc();
            check(f.state().bsf_par != nullptr, "allocate superframe filter parameters");
            f.state().bsf_par->codec_type = AVMEDIA_TYPE_VIDEO;
            f.state().bsf_par->codec_id = AV_CODEC_ID_VP9;
            f.state().bsf_par->width = f.state().bsf_par->height = 16;
            check(f.state().bsf_build(), "production superframe filter starts");
            f.state().in_pkt = av_packet_alloc(); f.state().out_pkt = av_packet_alloc();
            check(f.state().in_pkt && f.state().out_pkt, "superframe AVPacket allocation");
            const uint8_t first[] = {0x82, 11, 0x10};
            // Two three-byte frames, then the VP9 superframe index: two
            // lengths of one byte, bracketed by marker 0xc1.
            const uint8_t superframe[] = {0x80, 22, 0x10, 0x82, 33, 0x10, 0xc1, 3, 3, 0xc1};
            check(f.player.send(first, sizeof(first), 100) == 0, "first shown frame waits for output");
            check(f.player.send(superframe, sizeof(superframe), 200) == 0 && decode_calls == 3,
                  "one actual AVPacket emits two AUs through the real VP9 bitstream filter");
            check(queued(f.state()) == Pictures{{11, 100}} && f.state().vp9_count == 1,
                  "hidden superframe AU cannot discard the preceding visible output or be presented itself");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 100}, {33, 200}},
                  "shown superframe AU retains its packet timestamp after delayed output and drain");
        });
        scenario("AVC retains its existing timestamp reorder policy", [] {
            Fixture f(2, H264);
            check(f.submit(11, 300) == 0 && f.submit(22, 100) == 0 && f.submit(33, 200) == 0,
                  "AVC input sequence accepted");
            f.drain();
            check(queued(f.state()) == Pictures{{11, 100}, {22, 200}, {33, 300}},
                  "VP9 FIFO does not alter the existing AVC minimum-PTS policy");
            check(f.state().vp9_count == 0, "AVC never enters the VP9 FIFO");
        });
        scenario("invalid input length never enters native decode", [] {
            Fixture f;
            const uint8_t packet[] = {0x82, 11, 0x10};
            check(f.state().decode_one(packet, 0, 100, true) == -1 &&
                  f.state().decode_one(packet, int(kInputSlotBytes + 1), 100, true) == -1,
                  "existing access-unit size bounds remain enforced");
            check(decode_calls == 0 && f.state().vp9_count == 0, "invalid size changes no metadata or decoder state");
        });
        std::cout << "{\"result\":\"PASS\",\"cases\":" << cases << ",\"checks\":" << checks
                  << ",\"hardware_boundary\":\"mocked\",\"real_ffmpeg_bitstream_filter\":true,\"ps5_tested\":false"
                  << ",\"ffmpeg_version\":\"" << av_version_info() << "\",\"libavcodec_version\":" << avcodec_version()
                  << ",\"libavutil_version\":" << avutil_version() << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "VP9_OUTPUT_FAIL: " << error.what() << '\n';
        return 1;
    }
}
