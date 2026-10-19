// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_dovi.h"
struct picture_t;
std::shared_ptr<const VideoDoviParams> vlc_picture_dovi(const picture_t* picture);
