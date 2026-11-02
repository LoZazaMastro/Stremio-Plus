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
#include "video_gl.h"

#include <cstdint>

namespace hdr_out {

// After the GL context exists. Uses the renderer-owned VideoOut handle
// captured by native/videoout_capture.c; no guessed handles or second port.
void init(int surface_width, int surface_height);
void release();  // while the GL context is still current

// Settings → HDR output. Off: the scan-out stays (or returns to) SDR.
void set_enabled(bool enabled);

// First validate the display and allocate the final-pass resources, then
// upload the retained video planes in the returned mode. Only call
// update(true) after that upload succeeds. This also works while paused.
bool prepare(bool want);
void failed(const char* reason);  // disable further HDR attempts this session
// Commit the prepared encoding before presenting. A refused HDR request
// leaves SDR active; a refused SDR restoration keeps the HDR compositor.
bool update(bool want);
bool safe();  // false after two unsuccessful SDR restoration attempts
bool scanout_hdr();
bool wanted();  // an HDR video is playing, whatever the scan-out does

// What happened, in words: "Off", "On: the TV is in HDR", "Refused by the
// console (code 0x...)" ... The version changes whenever the text does.
const char* status();
unsigned status_version();

// The HDR frame: the renderer's queued layers into a transparent target, then
// the sampled YUV planes (or a fallback SDR texture) and interface into the
// scan-out. The converter and packer share this pass, without RGB10_A2 storage.
bool present(hui::gfx::Renderer& renderer, int width, int height, uint32_t video,
             const hui::gfx::Rect& video_rect, uint32_t target_framebuffer = 0, bool video_pq = true,
             const VideoGl::HdrPlanes* planes = nullptr);
// Small SDR copy only when glass controls are visible. No full-size SDR copy.
uint32_t glass_texture(uint32_t video, bool video_pq, const VideoGl::HdrPlanes* planes = nullptr);

}  // namespace hdr_out
