// Real HDR output on the PS5. While an HDR10 or HLG video (a Dolby Vision
// film's HDR10 base layer included) plays on a TV whose link runs in HDR, the
// console's scan-out is switched to 10-bit BT.2020 PQ and the video goes out
// as it is, no tone mapping. The interface is composed on top at reference
// white (203 nits). Otherwise nothing here is active and the SDR picture is
// drawn as before.
//
// How the console is told, the way PS5MC and BlackBearReloaded's video
// research found and Sp9nky's Stremio port (gl/hdr_out.cpp, GPL-3.0-or-later)
// ships: the title declares HDR capability in param.json (attribute
// 0x62000000), and the registered scan-out buffers are retyped in place with
// sceVideoOutSubmitChangeBufferAttribute2. They stay 32 bits per pixel; the
// final pass writes each pixel as a packed 2:10:10:10 word.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/draw_list.hpp"
#include "gfx/renderer.hpp"

#include <cstdint>

namespace hdr_out {

// After the GL context exists. Nothing is asked of the console here: the
// video output (the driver keeps its handle private) is checked when the
// first HDR video plays. `handle_hint` is the handle number the app's own
// resolution probe got and closed before the driver opened its own: the
// console hands the number out again, so one status call on it is the only
// probe ever made. (Sweeping handle numbers, as the original port did as a
// fallback, shut the console down.)
void init(int surface_width, int surface_height, int32_t handle_hint = -1);

// Settings → HDR output. Off: the scan-out stays (or returns to) SDR.
void set_enabled(bool enabled);

// Called every frame before drawing: asks the console to switch the scan-out
// to HDR when `want` (an HDR video is playing), and back when it is not.
// Returns whether the scan-out is in HDR now.
bool update(bool want);
bool scanout_hdr();
bool wanted();  // an HDR video is playing, whatever the scan-out does

// What happened, in words: "Off", "On: the TV is in HDR", "Refused by the
// console (code 0x...)" ... The version changes whenever the text does.
const char* status();
unsigned status_version();

// The HDR frame: the renderer's queued layers into a transparent target, then
// the video (a 10-bit texture holding PQ-coded BT.2020, shown in `video_rect`
// of the 1920 x 1080 layout) and the interface into the scan-out. False if it
// could not be done: the caller then draws the SDR frame instead.
bool present(hui::gfx::Renderer& renderer, int width, int height, uint32_t video,
             const hui::gfx::Rect& video_rect, uint32_t target_framebuffer = 0);

}  // namespace hdr_out
