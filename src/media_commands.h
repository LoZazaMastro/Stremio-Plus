// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

enum class MediaCommand : std::uint8_t {
    None, PlayPause, Play, Pause, Stop, Previous, Next, Rewind, FastForward,
    Subtitles, Audio, Info
};
