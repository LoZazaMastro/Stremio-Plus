// PS5 HDMI audio: sink capabilities and the bitstream / 8-channel PCM ports.
// Call sequence and port modes from sainsaji's PS5-Audio-Passthrough-Research
// (sample-app/src/platform/ps5/bitstream_out.cpp); settle times and the null
// burst bracketing from EVO Player's evo_pt.cpp. Both GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_audio_sink.h"

#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef PLATFORM_PS5_NATIVE
extern "C" {
// libSceAudioOut. The Ex and Sys functions are not in the SDK headers, but the
// payload SDK's stub exports them, so they link as ordinary imports. A
// sandboxed app cannot look them up with sceKernelDlsym.
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void* samples);
int sceAudioOutExOpen(int user, int mode);                                                        // 6X6dp+07h4U
int sceAudioOutExClose(int handle);                                                               // 0TfjSulCV2A
int sceAudioOutExConfigureOutput(int zero, unsigned flags, int mode, int target, uint64_t opt);   // VcE+gXSwFXI
int sceAudioOutSysOpen(int user, int mode);                                                       // MGkAS4ncQ90
int sceAudioOutSysClose(int handle);                                                              // pKY2S4K-6Mg
int sceAudioOutSysConfigureOutput(int type, unsigned flags, int mode, int target, uint64_t opt);  // ktdp5iauPQc
int sceAudioOutSysGetHdmiMonitorInfo(int type, void* out, unsigned size);                         // Tf9-yOJwF-A
}
#endif

namespace ps5audio {
namespace {
constexpr int kGrainsInFlight = 2;
constexpr int kWriteWaitMs = 3000;

#ifdef PLATFORM_PS5_NATIVE
constexpr int kSystemUser = 0xFF;
constexpr int kTargetHdmi = 1;      // what Sony's media core passes
constexpr int kModeDefault = 0xFF;  // every field "don't care": normal PCM output
constexpr int kAlreadyInitialized = int(0x8026000e);
constexpr unsigned kMonitorInfoSize = 0x180;
constexpr unsigned kPcm8Format = 2;  // sceAudioOutOpen: S16, 8 channels

// A receiver follows HDMI format changes slowly. Each bitstream is bracketed
// with IEC 61937 null bursts, and the port close, the HDMI reset and the next
// open are spaced out; switching back to back left a receiver stuck on the
// previous format.
constexpr int kLeadInMs = 400;      // null bursts after the switch, before the first frame
constexpr int kLeadOutMs = 500;     // null bursts after the last frame, before the port closes
constexpr int kAfterCloseMs = 250;  // between closing the port and resetting HDMI
constexpr int kAfterResetMs = 400;  // after the reset, before returning
constexpr int kBeforeOpenMs = 1000; // minimum gap since the last reset before a new switch
constexpr double kRingSeconds = 1.0;  // VLC decodes ahead; its own clock keeps the blocks on time

std::mutex g_hdmi_mutex;
int64_t g_last_reset_ms = -100000;
std::atomic<int> g_bitstream_ports{0};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void reset_hdmi_locked() {
    const int rc = sceAudioOutExConfigureOutput(0, 0, kModeDefault, kModeDefault, 0);
    g_last_reset_ms = now_ms();
    dlog("ps5audio: HDMI back to PCM output (0x%08x)", unsigned(rc));
}
#endif
}  // namespace

const char* coding_name(int coding) {
    switch (coding) {
    case kLpcm: return "PCM";
    case kAc3: return "Dolby Digital";
    case kAac: return "AAC";
    case kDts: return "DTS";
    case kEac3: return "Dolby Digital Plus";
    case kDtsHd: return "DTS-HD";
    case kMat: return "Dolby TrueHD";
    default: return "other";
    }
}

bool Sink::supports(int coding) const {
    for (const auto& format : formats) if (format.coding == coding) return true;
    return false;
}

int Sink::lpcm_channels() const {
    int channels = 0;
    for (const auto& format : formats) if (format.coding == kLpcm) channels = std::max(channels, format.channels);
    return channels;
}

std::string Sink::summary() const {
    if (!queried) {
        char text[64];
        snprintf(text, sizeof(text), "HDMI sink unknown (0x%08x)", unsigned(rc));
        return text;
    }
    std::string text = name.empty() ? "HDMI sink" : name;
    text += ':';
    if (formats.empty()) return text + " no audio descriptors";
    for (const auto& format : formats) {
        char entry[48];
        snprintf(entry, sizeof(entry), " %s/%dch", coding_name(format.coding), format.channels);
        text += entry;
    }
    return text;
}

Sink query_sink() {
    Sink sink;
#ifdef PLATFORM_PS5_NATIVE
    uint8_t buffer[kMonitorInfoSize] = {};
    sink.rc = sceAudioOutSysGetHdmiMonitorInfo(1 /* HDMI */, buffer, sizeof(buffer));
    if (sink.rc < 0) return sink;
    sink.queried = true;
    // Layout read from a FW 12.70 dump (research docs/FINDINGS.md): the monitor
    // name at +0x0B, the number of short audio descriptors as a 32-bit value
    // at +0x94, then 8-byte descriptors (coding type, channels, rates, ...) from +0x98.
    char name[14] = {};
    std::memcpy(name, buffer + 0x0B, 13);
    for (char& c : name) if (c != 0 && (c < 0x20 || c > 0x7E)) c = 0;
    sink.name = name;
    while (!sink.name.empty() && sink.name.back() == ' ') sink.name.pop_back();
    uint32_t count = 0;
    std::memcpy(&count, buffer + 0x94, sizeof(count));
    for (uint32_t i = 0; i < count && 0x98 + (i + 1) * 8 <= kMonitorInfoSize; ++i) {
        const uint8_t* entry = buffer + 0x98 + i * 8;
        if (entry[0] == 0 || entry[0] > 15) break;
        sink.formats.push_back({entry[0], entry[1]});
    }
#else
    sink.rc = -1;
#endif
    return sink;
}

const char* format_name(Format format) {
    switch (format) {
    case Format::Ac3: return "Dolby Digital";
    case Format::Eac3: return "Dolby Digital Plus";
    case Format::Dts: return "DTS";
    case Format::DtsHd: return "DTS-HD";
    case Format::TrueHd: return "Dolby TrueHD";
    case Format::Pcm8: return "multichannel PCM";
    }
    return "audio";
}

int coding_for(Format format) {
    switch (format) {
    case Format::Ac3: return kAc3;
    case Format::Eac3: return kEac3;
    case Format::Dts: return kDts;
    case Format::DtsHd: return kDtsHd;
    case Format::TrueHd: return kMat;
    case Format::Pcm8: return kLpcm;
    }
    return 0;
}

Carrier carrier_for(Format format) {
    Carrier carrier;
    switch (format) {
    case Format::Ac3: carrier = {Carrier::Kind::Bitstream, 0, 256, 4, 48000}; break;
    case Format::Dts: carrier = {Carrier::Kind::Bitstream, 2, 256, 4, 48000}; break;
    case Format::Eac3: carrier = {Carrier::Kind::Bitstream, 3, 1024, 4, 192000}; break;
    case Format::DtsHd: carrier = {Carrier::Kind::Bitstream, 4, 1024, 4, 192000}; break;
    // Sony's Blu-ray player route: a port labelled 768 kHz whose 16-byte
    // frames are consumed at 192 kHz (a 16384-byte grain blocks for 5.3 ms).
    case Format::TrueHd: carrier = {Carrier::Kind::BitstreamSys, 5, 1024, 16, 192000}; break;
    case Format::Pcm8: carrier = {Carrier::Kind::Pcm, -1, 256, 16, 48000}; break;
    }
    return carrier;
}

Port::~Port() { close(); }

bool Port::open(Format format, std::string* error) {
    close();
#ifndef PLATFORM_PS5_NATIVE
    (void)format;
    if (error) *error = "HDMI audio ports exist on the PS5 only";
    return false;
#else
    carrier_ = carrier_for(format);
    format_ = format;
    const bool bitstream = carrier_.kind != Carrier::Kind::Pcm;
    char why[96];
    std::unique_lock<std::mutex> hdmi(g_hdmi_mutex);
    if (bitstream) {
        const int64_t since_reset = now_ms() - g_last_reset_ms;
        if (since_reset < kBeforeOpenMs) sleep_ms(int(kBeforeOpenMs - since_reset));
    }
    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized) {
        snprintf(why, sizeof(why), "sceAudioOutInit 0x%08x", unsigned(init));
        if (error) *error = why;
        return false;
    }
    // 1. The carrier port first ...
    int handle = -1;
    switch (carrier_.kind) {
    case Carrier::Kind::Pcm:
        handle = sceAudioOutOpen(kSystemUser, 0, 0, unsigned(carrier_.grain_frames), unsigned(carrier_.frame_rate), kPcm8Format);
        break;
    case Carrier::Kind::Bitstream: handle = sceAudioOutExOpen(kSystemUser, carrier_.mode); break;
    case Carrier::Kind::BitstreamSys: handle = sceAudioOutSysOpen(kSystemUser, carrier_.mode); break;
    }
    if (handle <= 0) {
        snprintf(why, sizeof(why), "%s port open (mode %d) 0x%08x", format_name(format), carrier_.mode, unsigned(handle));
        if (error) *error = why;
        return false;
    }
    // 2. ... then HDMI switches to that format. Normal PCM ports are muted
    // from here until the reset.
    if (bitstream) {
        const int rc = carrier_.kind == Carrier::Kind::BitstreamSys
            ? sceAudioOutSysConfigureOutput(1 /* HDMI */, 0, carrier_.mode, kTargetHdmi, 0)
            : sceAudioOutExConfigureOutput(0, 0, carrier_.mode, kTargetHdmi, 0);
        if (rc < 0) {
            if (carrier_.kind == Carrier::Kind::BitstreamSys) sceAudioOutSysClose(handle);
            else sceAudioOutExClose(handle);
            reset_hdmi_locked();
            snprintf(why, sizeof(why), "HDMI refused %s output (mode %d) 0x%08x", format_name(format), carrier_.mode, unsigned(rc));
            if (error) *error = why;
            return false;
        }
        ++g_bitstream_ports;
    }
    hdmi.unlock();
    handle_ = handle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.clear();
        head_ = 0;
        limit_ = size_t(carrier_.bytes_per_second() * kRingSeconds);
    }
    stop_ = paused_ = muted_ = draining_ = false;
    frames_played_ = 0;
    lead_in_grains_ = bitstream ? kLeadInMs * carrier_.frame_rate / 1000 / carrier_.grain_frames : 0;
    thread_ = std::thread([this] { run(); });
    open_ = true;
    dlog("ps5audio: %s port open (mode %d, %zu-byte grains, %d frames/s)", format_name(format), carrier_.mode,
         carrier_.grain_bytes(), carrier_.frame_rate);
    return true;
#endif
}

void Port::close() {
    if (!open_) return;
    stop_ = true;
    space_.notify_all();
    if (thread_.joinable()) thread_.join();
#ifdef PLATFORM_PS5_NATIVE
    const bool bitstream = carrier_.kind != Carrier::Kind::Pcm;
    std::lock_guard<std::mutex> hdmi(g_hdmi_mutex);
    int rc = 0;
    switch (carrier_.kind) {
    case Carrier::Kind::Pcm: rc = sceAudioOutClose(handle_); break;
    case Carrier::Kind::Bitstream: rc = sceAudioOutExClose(handle_); break;
    case Carrier::Kind::BitstreamSys: rc = sceAudioOutSysClose(handle_); break;
    }
    dlog("ps5audio: %s port closed (0x%08x)", format_name(format_), unsigned(rc));
    if (bitstream) {
        sleep_ms(kAfterCloseMs);
        reset_hdmi_locked();
        --g_bitstream_ports;
        sleep_ms(kAfterResetMs);
    }
#endif
    handle_ = -1;
    open_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
    head_ = 0;
}

void Port::fill_silence(uint8_t* out, size_t bytes) const {
    std::memset(out, 0, bytes);
    if (carrier_.kind == Carrier::Kind::Pcm || bytes < 8) return;
    // One IEC 61937 null-data burst: preamble, data type 0, nothing else.
    out[0] = 0x72; out[1] = 0xF8;  // Pa 0xF872, little-endian
    out[2] = 0x1F; out[3] = 0x4E;  // Pb 0x4E1F
}

bool Port::write(const uint8_t* data, size_t bytes) {
    if (!open_ || !data || !bytes) return open_;
    std::unique_lock<std::mutex> lock(mutex_);
    // Slices of whole frames, so a block longer than the ring (VLC's start-up
    // silence can be seconds long) streams through as the thread drains it.
    const size_t frame = size_t(carrier_.frame_bytes);
    while (bytes >= frame) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWriteWaitMs);
        while (!stop_ && ring_.size() - head_ >= limit_) {
            if (space_.wait_until(lock, deadline) == std::cv_status::timeout) return false;
        }
        if (stop_) return false;
        size_t slice = std::min(bytes, limit_ - (ring_.size() - head_));
        slice -= slice % frame;
        ring_.insert(ring_.end(), data, data + slice);
        data += slice;
        bytes -= slice;
    }
    return true;
}

void Port::write_silence(size_t bytes) {
    if (!open_) return;
    bytes -= bytes % size_t(carrier_.frame_bytes);
    if (!bytes) return;
    std::vector<uint8_t> buffer(bytes);
    const size_t grain = carrier_.grain_bytes();
    for (size_t at = 0; at < bytes; at += grain) fill_silence(buffer.data() + at, std::min(grain, bytes - at));
    write(buffer.data(), buffer.size());
}

void Port::set_paused(bool paused) { paused_ = paused; }
void Port::set_muted(bool muted) { muted_ = muted; }

void Port::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.clear();
    head_ = 0;
    space_.notify_all();
    drained_.notify_all();
}

void Port::drain(int timeout_ms) {
    if (!open_) return;
    draining_ = true;
    std::unique_lock<std::mutex> lock(mutex_);
    drained_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return stop_ || ring_.size() == head_; });
    draining_ = false;
}

size_t Port::queued_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.size() - head_;
}

int64_t Port::delay_us() const {
    const size_t pending = queued_bytes() + carrier_.grain_bytes() * kGrainsInFlight;
    return int64_t(pending) * 1000000 / carrier_.bytes_per_second();
}

void Port::run() {
#ifdef PLATFORM_PS5_NATIVE
    const size_t grain_bytes = carrier_.grain_bytes();
    std::vector<uint8_t> grain(grain_bytes);
    int lead_in = lead_in_grains_;
    while (!stop_) {
        bool filled = false;
        if (lead_in > 0) {
            --lead_in;
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t queued = ring_.size() - head_;
            if (!paused_ && !muted_ && queued >= grain_bytes) {
                std::memcpy(grain.data(), ring_.data() + head_, grain_bytes);
                head_ += grain_bytes;
                filled = true;
            } else if (!paused_ && !muted_ && draining_ && queued > 0) {
                // The stream's last bytes do not fill a grain: pad them.
                std::memcpy(grain.data(), ring_.data() + head_, queued);
                fill_silence(grain.data() + queued, grain_bytes - queued);
                head_ += queued;
                filled = true;
            }
            // A burst that is not a whole number of grains (a 61440-byte MAT
            // burst is 3.75 grains of the TrueHD port) waits for its tail: padding
            // it would stretch every burst and break the stream.
            if (head_ == ring_.size()) {
                ring_.clear();
                head_ = 0;
                drained_.notify_all();
            } else if (head_ > (1u << 20) && head_ * 2 > ring_.size()) {
                ring_.erase(ring_.begin(), ring_.begin() + long(head_));
                head_ = 0;
            }
            if (filled) space_.notify_all();
        }
        if (!filled) fill_silence(grain.data(), grain_bytes);
        // Blocks for about one grain, which paces this thread in real time.
        if (sceAudioOutOutput(handle_, grain.data()) < 0) sleep_ms(5);
        if (filled) frames_played_.fetch_add(uint64_t(carrier_.grain_frames), std::memory_order_relaxed);
    }
    if (carrier_.kind != Carrier::Kind::Pcm) {
        // Tell the receiver the stream is ending before the port goes away.
        fill_silence(grain.data(), grain_bytes);
        const int grains = kLeadOutMs * carrier_.frame_rate / 1000 / carrier_.grain_frames;
        for (int i = 0; i < grains; ++i) sceAudioOutOutput(handle_, grain.data());
    }
    sceAudioOutOutput(handle_, nullptr);  // the queued grains play out
#endif
}

void Port::restore_hdmi() {
#ifdef PLATFORM_PS5_NATIVE
    if (g_bitstream_ports.load() <= 0) return;
    std::lock_guard<std::mutex> hdmi(g_hdmi_mutex);
    reset_hdmi_locked();
    g_bitstream_ports = 0;
#endif
}

}  // namespace ps5audio
