// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/input.hpp"
#include "media_commands.h"
#include <array>
#include <cstdint>
#include <cstddef>

struct RemoteSample {
    hui::PadSample pad;
    MediaCommand command = MediaCommand::None;
    std::uint8_t code = 0;
    bool intercepted = false;
};
RemoteSample decode_remote_sample(const std::uint8_t* bytes, std::size_t size);
void merge_remote_input(hui::InputFrame& controller, const hui::InputFrame& remote);

class MediaRemote {
public:
    struct Frame {
        hui::InputFrame input;
        std::array<MediaCommand, 64> commands{};
        std::size_t count = 0;
    };
    ~MediaRemote();
    Frame poll(std::uint64_t now_us);
private:
    int handle_ = -1;
    bool owns_handle_ = false;
    bool service_ready_ = false;
    std::uint64_t next_open_ = 0, last_timestamp_ = 0;
    std::uint32_t ignored_buttons_ = 0;
    MediaCommand ignored_command_ = MediaCommand::None, last_command_ = MediaCommand::None;
    std::uint32_t snapshot_buttons_ = 0;
    MediaCommand snapshot_command_ = MediaCommand::None;
    hui::InputTracker tracker_;
};
