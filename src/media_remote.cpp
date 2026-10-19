// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-control port and key codes follow the PS5 SDL remote backend.
// The DualSense keeps its independent input, light-bar and haptic ownership.
#include "media_remote.h"
#include "util.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr int kSystemUser = 0xff, kRemotePort = 16;
constexpr int kAlreadyOpened = static_cast<int>(0x80920004u);
constexpr std::size_t kSampleBytes = 120, kKeyOffset = 0x6c;
struct alignas(8) RawRemoteSample { std::uint8_t bytes[kSampleBytes]{}; };
static_assert(sizeof(RawRemoteSample) == 120);
}

RemoteSample decode_remote_sample(const std::uint8_t* bytes, std::size_t size) {
    RemoteSample result;
    if (!bytes || size < kSampleBytes) return result;
    using namespace hui::pad_bits;
    std::uint32_t raw = 0;
    std::memcpy(&raw, bytes, sizeof(raw));
    std::memcpy(&result.pad.timestamp_us, bytes + 0x50, sizeof(result.pad.timestamp_us));
    result.intercepted = (raw & kIntercepted) != 0;
    result.pad.connected = bytes[0x4c] != 0;
    if (result.intercepted) {
        result.pad.buttons = kIntercepted;
        return result;
    }
    result.pad.buttons = raw & (kUp | kDown | kLeft | kRight | kCross | kCircle | kSquare | kTriangle | kOptions | kL1 | kR1);
    result.code = bytes[kKeyOffset];
    switch (result.code) {
    case 13: result.pad.buttons |= kCross; break;
    case 15: result.pad.buttons |= kCircle; break;
    case 18: case 27: result.pad.buttons |= kOptions; break;
    case 20: result.command = MediaCommand::Previous; break;
    case 21: result.command = MediaCommand::Next; break;
    case 22: result.command = MediaCommand::Play; break;
    case 23: result.command = MediaCommand::Rewind; break;
    case 24: result.command = MediaCommand::FastForward; break;
    case 25: result.command = MediaCommand::Stop; break;
    case 26: result.command = MediaCommand::Pause; break;
    case 30: result.command = MediaCommand::Subtitles; break;
    case 31: result.command = MediaCommand::Audio; break;
    case 33: result.command = MediaCommand::Info; break;
    case 36: result.pad.buttons |= kTriangle; break;
    case 37: result.pad.buttons |= kSquare; break;
    case 41: result.pad.buttons |= kL1; break;
    case 42: result.pad.buttons |= kR1; break;
    case 45: result.command = MediaCommand::PlayPause; break;
    default: break;
    }
    if (result.command != MediaCommand::None) result.pad.buttons &= ~kCross;
    result.pad.connected = result.pad.connected || result.pad.buttons != 0 || result.command != MediaCommand::None;
    return result;
}

void merge_remote_input(hui::InputFrame& controller, const hui::InputFrame& remote) {
    controller.pressed |= remote.pressed & ~controller.held;
    controller.released = (controller.released & ~remote.held) | (remote.released & ~controller.held);
    controller.held |= remote.held;
    if (controller.nav == hui::Direction::none && remote.nav != hui::Direction::none) {
        controller.nav = remote.nav;
        controller.nav_repeat = remote.nav_repeat;
        controller.nav_from_stick = false;
    }
    controller.connected = controller.connected || remote.connected;
    if (!remote.focus_lost && (remote.pressed || remote.held || remote.nav != hui::Direction::none))
        controller.focus_lost = false;
}

#ifdef PLATFORM_PS5
extern "C" {
int scePadOpen(int, int, int, const void*);
int scePadGetHandle(int, int, int);
int scePadRead(int, void*, int);
int scePadReadState(int, void*);
int scePadClose(int);
int scePadInit(void);
int sceUserServiceInitialize(const void*);
}
#endif

MediaRemote::~MediaRemote() {
#ifdef PLATFORM_PS5
    if (handle_ >= 0 && owns_handle_) scePadClose(handle_);
#endif
}

MediaRemote::Frame MediaRemote::poll(std::uint64_t now_us) {
    Frame result;
#ifdef PLATFORM_PS5
    if (handle_ < 0) {
        if (now_us < next_open_) return result;
        next_open_ = now_us + 2000000;
        if (!service_ready_) {
            const int user_service = sceUserServiceInitialize(nullptr);
            if (user_service != 0 && user_service != static_cast<int>(0x80960003u)) return result;
            const int pad_init = scePadInit();
            // The DualSense may already own the initialized service. An
            // initialization result alone must not prevent opening its remote port.
            if (pad_init != 0) dlog("media remote: pad initialization rc=0x%08x", unsigned(pad_init));
            service_ready_ = pad_init == 0;
        }
        int handle = scePadOpen(kSystemUser, kRemotePort, 0, nullptr);
        owns_handle_ = handle >= 0;
        if (handle == kAlreadyOpened) handle = scePadGetHandle(kSystemUser, kRemotePort, 0);
        if (handle < 0) return result;
        handle_ = handle;
        service_ready_ = true;
        RawRemoteSample current;
        if (scePadReadState(handle_, &current) == 0) {
            const auto initial = decode_remote_sample(current.bytes, sizeof(current));
            ignored_buttons_ = initial.pad.buttons & ~hui::pad_bits::kIntercepted;
            ignored_command_ = last_command_ = initial.command;
            last_timestamp_ = initial.pad.timestamp_us;
            snapshot_buttons_ = initial.pad.buttons;
            snapshot_command_ = initial.command;
        }
        dlog("media remote: opened system-user port=%d handle=%d", kRemotePort, handle_);
    }
    std::array<RawRemoteSample, 64> raw;
    std::array<hui::PadSample, 65> samples;
    std::size_t count = 0;
    const auto accept = [&](const RawRemoteSample& value) {
        auto decoded = decode_remote_sample(value.bytes, sizeof(value));
        if (decoded.pad.timestamp_us && decoded.pad.timestamp_us < last_timestamp_) return;
        last_timestamp_ = std::max(last_timestamp_, decoded.pad.timestamp_us);
        ignored_buttons_ &= decoded.pad.buttons;
        decoded.pad.buttons &= ~ignored_buttons_;
        if (decoded.command != ignored_command_) ignored_command_ = MediaCommand::None;
        if (!decoded.intercepted && decoded.command != MediaCommand::None && decoded.command != ignored_command_ &&
            decoded.command != last_command_ && result.count < result.commands.size()) {
            result.commands[result.count++] = decoded.command;
            dlog("media remote: transport code=%u", unsigned(decoded.code));
        }
        last_command_ = decoded.command;
        if (count < samples.size()) samples[count++] = decoded.pad;
    };
    const int read = scePadRead(handle_, raw.data(), int(raw.size()));
    if (read > 0 && read <= int(raw.size())) {
        for (int i = 0; i < read; ++i) accept(raw[std::size_t(i)]);
    }
    RawRemoteSample current;
    const int snapshot = scePadReadState(handle_, &current);
    if (snapshot == 0) {
        const auto decoded = decode_remote_sample(current.bytes, sizeof(current));
        // A newer batch may contain a tap that the older snapshot cannot see.
        const bool changed = decoded.pad.buttons != snapshot_buttons_ || decoded.command != snapshot_command_;
        if (changed || decoded.intercepted || !last_timestamp_ || decoded.pad.timestamp_us > last_timestamp_ ||
            (!count && (decoded.pad.buttons || decoded.command != MediaCommand::None))) accept(current);
        snapshot_buttons_ = decoded.pad.buttons;
        snapshot_command_ = decoded.command;
    } else if (read < 0) {
        if (owns_handle_) scePadClose(handle_);
        handle_ = -1;
        owns_handle_ = false;
        last_timestamp_ = 0;
        ignored_buttons_ = 0;
        ignored_command_ = last_command_ = MediaCommand::None;
        samples[count++] = hui::PadSample{};
    }
    result.input = tracker_.update(std::span<const hui::PadSample>(samples.data(), count), now_us);
#else
    (void)now_us;
#endif
    return result;
}
