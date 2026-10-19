// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_remote.h"
#include <cassert>
#include <cstring>
#include <vector>

using Raw = std::array<std::uint8_t, 120>;
static Raw snapshot{};
static std::vector<Raw> pending;
static int closed = 0, initialized = 0, open_result = 7, reads = 0, pad_init_result = 0;

void dlog(const char*, ...) {}
extern "C" int sceUserServiceInitialize(const void*) { return 0; }
extern "C" int scePadInit() { ++initialized; return pad_init_result; }
extern "C" int scePadOpen(int user, int port, int index, const void*) {
    assert(user == 0xff && port == 16 && index == 0); return open_result;
}
extern "C" int scePadGetHandle(int user, int port, int) {
    assert(user == 0xff && port == 16); return 7;
}
extern "C" int scePadClose(int handle) { assert(handle == 7); ++closed; return 0; }
extern "C" int scePadReadState(int handle, void* dst) {
    assert(handle == 7); std::memcpy(dst, snapshot.data(), snapshot.size()); return 0;
}
extern "C" int scePadRead(int handle, void* dst, int count) {
    assert(handle == 7 && count == 64); ++reads;
    assert(pending.size() <= std::size_t(count));
    const int n = int(pending.size());
    if (n) std::memcpy(dst, pending.data(), pending.size() * sizeof(Raw));
    pending.clear(); return n;
}

Raw raw(std::uint8_t key, std::uint64_t timestamp, std::uint32_t buttons = 0) {
    Raw r{};
    r[0x4c] = 1;
    r[0x6c] = key;
    std::memcpy(r.data(), &buttons, sizeof(buttons));
    std::memcpy(r.data() + 0x50, &timestamp, sizeof(timestamp));
    return r;
}

int main() {
    using namespace hui;
    assert(!decode_remote_sample(nullptr, 120).pad.connected);
    auto r = raw(13, 1);
    assert(!decode_remote_sample(r.data(), 119).pad.connected);
    assert(decode_remote_sample(r.data(), r.size()).pad.buttons == pad_bits::kCross);
    r = raw(45, 2, pad_bits::kCross);
    auto decoded = decode_remote_sample(r.data(), r.size());
    assert(decoded.command == MediaCommand::PlayPause && decoded.pad.buttons == 0);
    r = raw(45, 3, pad_bits::kIntercepted | pad_bits::kRight);
    decoded = decode_remote_sample(r.data(), r.size());
    assert(decoded.intercepted && decoded.command == MediaCommand::None && decoded.pad.buttons == pad_bits::kIntercepted);
    r = raw(255, 4);
    assert(decode_remote_sample(r.data(), r.size()).command == MediaCommand::None);

    InputFrame controller, remote;
    controller.held = action_bit(Action::confirm);
    controller.stick_x = .75f;
    remote.pressed = action_bit(Action::confirm) | action_bit(Action::back);
    remote.held = action_bit(Action::confirm);
    remote.nav = Direction::left;
    merge_remote_input(controller, remote);
    assert(!(controller.pressed & action_bit(Action::confirm)));
    assert(controller.is_pressed(Action::back) && controller.stick_x == .75f && controller.nav == Direction::left);

    snapshot = raw(0, 100);
    {
        MediaRemote device;
        assert(device.poll(1000000).count == 0);
        assert(initialized == 1);
        pending = {raw(13, 101), raw(0, 102)};
        snapshot = raw(0, 102);
        auto frame = device.poll(1016000);
        assert(frame.input.is_pressed(Action::confirm));
        assert(!frame.input.is_held(Action::confirm));
        assert(device.poll(1032000).input.pressed == 0);

        pending = {raw(45, 103)};
        frame = device.poll(1048000);
        assert(frame.count == 1 && frame.commands[0] == MediaCommand::PlayPause);
        pending = {raw(45, 104)};
        assert(device.poll(1064000).count == 0);
        pending = {raw(0, 105), raw(45, 106)};
        frame = device.poll(1080000);
        assert(frame.count == 1);
        pending = {raw(45, 90)};
        assert(device.poll(1096000).count == 0);
        pending = {raw(45, 107, pad_bits::kIntercepted)};
        assert(device.poll(1112000).count == 0);
    }
    assert(closed == 1);
    open_result = static_cast<int>(0x80920004u);
    snapshot = raw(45, 200);
    {
        MediaRemote borrowed;
        assert(borrowed.poll(2000000).count == 0);
        pending = {raw(45, 201)};
        assert(borrowed.poll(2016000).count == 0);
        pending = {raw(0, 202), raw(45, 203)};
        assert(borrowed.poll(2032000).count == 1);
    }
    assert(closed == 1);
    assert(reads > 5);
    open_result = 7;
    pad_init_result = -1;
    snapshot = raw(0, 300);
    {
        MediaRemote already_initialized;
        assert(already_initialized.poll(3000000).count == 0);
        pending = {raw(45, 301)};
        assert(already_initialized.poll(3016000).count == 1);
    }
    assert(closed == 2);
}
