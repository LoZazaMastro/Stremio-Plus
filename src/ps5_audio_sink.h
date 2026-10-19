// PS5 HDMI audio for the native player: what the TV or receiver can decode,
// and the console ports that carry a compressed bitstream (Dolby Digital,
// Dolby Digital Plus, DTS, DTS-HD, Dolby TrueHD) or 8-channel PCM untouched.
//
// The bitstream route is the one Sony's own media apps use. It was found by
// sainsaji's PS5-Audio-Passthrough-Research and confirmed on hardware: open
// the carrier port, switch HDMI to that format, write IEC 61937 bursts as S16
// frames, then drain, close and restore normal output. The receiver settle
// times follow EVO Player's evo_pt.cpp. Both are GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps5audio {

// CEA-861 short audio descriptor coding types, as the sink's EDID lists them.
enum Coding : int { kLpcm = 1, kAc3 = 2, kAac = 6, kDts = 7, kEac3 = 10, kDtsHd = 11, kMat = 12 };
const char* coding_name(int coding);

struct SinkFormat {
    int coding = 0, channels = 0;
};

struct Sink {
    bool queried = false;  // the console answered; false elsewhere or on error
    int rc = 0;
    std::string name;
    std::vector<SinkFormat> formats;
    bool supports(int coding) const;
    int lpcm_channels() const;  // 0 when unknown
    std::string summary() const;
};
Sink query_sink();

enum class Format { Ac3, Eac3, Dts, DtsHd, TrueHd, Pcm8 };
const char* format_name(Format format);
int coding_for(Format format);

// What the console needs to carry one format.
struct Carrier {
    enum class Kind { Bitstream, BitstreamSys, Pcm };
    Kind kind = Kind::Pcm;
    int mode = -1;           // sceAudioOutExOpen / ExConfigureOutput mode, or the SysOpen mode
    int grain_frames = 256;  // port frames per sceAudioOutOutput call
    int frame_bytes = 4;     // S16 stereo = 4, S16 8-channel = 16
    int frame_rate = 48000;  // frames the port really consumes per second
    size_t grain_bytes() const { return size_t(grain_frames) * size_t(frame_bytes); }
    int bytes_per_second() const { return frame_rate * frame_bytes; }
};
Carrier carrier_for(Format format);

// One open port. write() takes bytes already in the port's frame layout:
// IEC 61937 bursts for a bitstream carrier, interleaved S16 8-channel frames
// (L R C LFE Ls Rs Lb Rb) for PCM. A thread feeds the console one grain at a
// time; while nothing is queued, or while paused or muted, it sends IEC 61937
// null bursts (PCM: zeros) so the receiver keeps its lock.
class Port {
public:
    Port() = default;
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    ~Port();

    bool open(Format format, std::string* error);
    void close();
    bool is_open() const { return open_; }
    Format format() const { return format_; }
    const Carrier& carrier() const { return carrier_; }

    // Queues bytes, waiting while the ring is full. False when not open or closing.
    bool write(const uint8_t* data, size_t bytes);
    // Queues `bytes` of silence shaped for the carrier (null bursts, or zeros).
    void write_silence(size_t bytes);
    void set_paused(bool paused);
    void set_muted(bool muted);
    void flush();
    void drain(int timeout_ms);
    size_t queued_bytes() const;
    int64_t delay_us() const;  // queued bytes plus the grains in flight
    // Port frames of real data sent so far (silence and null bursts excluded).
    uint64_t frames_played() const { return frames_played_.load(std::memory_order_relaxed); }

    // Back to normal PCM output, for exit paths that could not close a port.
    static void restore_hdmi();

private:
    void run();
    void fill_silence(uint8_t* out, size_t bytes) const;

    bool open_ = false;
    Format format_ = Format::Pcm8;
    Carrier carrier_;
    int handle_ = -1;
    [[maybe_unused]] int lead_in_grains_ = 0;  // bitstream carriers only
    std::thread thread_;
    std::atomic<bool> stop_{false}, paused_{false}, muted_{false}, draining_{false};
    std::atomic<uint64_t> frames_played_{0};
    mutable std::mutex mutex_;
    std::condition_variable space_, drained_;
    std::vector<uint8_t> ring_;  // queued bytes, oldest first
    size_t head_ = 0;            // bytes of ring_ already sent
    size_t limit_ = 0;
};

}  // namespace ps5audio
