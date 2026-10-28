// HDR10 scan-out and the final composition pass. Ported from Sp9nky's
// unofficial Stremio PS5 port, gl/hdr_out.cpp (GPL-3.0-or-later), which took
// the scan-out format and the switch sequence from PS5MC and BlackBearReloaded's
// video research; here the video arrives as a texture VideoGl already converted.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hdr_out.h"

#include "gfx/gl_program.hpp"
#include "util.h"

#include <GL/glcorearb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef PLATFORM_PS5_NATIVE
extern "C" {
int32_t sceVideoOutGetOutputStatus(int32_t handle, void* status);
int32_t sceVideoOutGetResolutionStatus(int32_t handle, void* status);
void sceVideoOutSetBufferAttribute2(void* attribute, uint64_t format, uint32_t tiling, uint32_t width, uint32_t height,
                                    uint64_t option, uint32_t reserved0, uint64_t reserved1);
int32_t sceVideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t set_index, const void* attribute, void* option);
}
#endif

namespace hdr_out {
namespace {

#ifdef PLATFORM_PS5_NATIVE
// The scan-out buffer formats: the driver's own SDR one, and HDR 10-bit BT.2020 PQ (2:10:10:10).
constexpr uint64_t kFormatSdr = UINT64_C(0x8000000000000000);
constexpr uint64_t kFormatHdr = UINT64_C(0x8100070422000000);
#endif

struct State {
    int surface_w = 0, surface_h = 0;
    int32_t handle = -1;
    int32_t handle_hint = -1;  // the resolution probe's number (display_output.cpp)
    bool probed = false;       // the one status call on the hint was made
    bool enabled = true;
    bool hdr = false;
    int failures = 0;
    double switched_at = 0;
    bool confirmed = false;
    bool want_prev = false;  // a video wanting HDR was already playing last call
    bool link_sdr = false;   // the link reported SDR when that video started: SDR until it ends
    bool wanted = false;
    std::string status = "Starting";
    std::string no_handle_text = "Not available: no video output found";
    unsigned status_version = 0;
    // GL
    GLuint ui_fbo = 0, ui_tex = 0;
    int ui_w = 0, ui_h = 0;
    GLuint program = 0, vao = 0;
    bool gl_failed = false;
} g;

#ifdef PLATFORM_PS5_NATIVE
// SceVideoOutOutputStatus: resolution, dynamic range (0 unknown, 1 SDR, 2 HDR), refresh, flags (bit 0: HDR active).
struct OutputStatus {
    uint32_t resolution;
    uint32_t dynamic_range;
    uint64_t refresh;
    uint64_t flags;
    uint64_t reserved[3];
};

bool read_status(int32_t handle, OutputStatus* out) {
    uint64_t raw[16] = {};  // larger than the structure, zeroed: safe if the layout grew
    if (sceVideoOutGetOutputStatus(handle, raw) != 0) return false;
    std::memcpy(out, raw, sizeof(*out));
    return true;
}

// PS5MC's IsDisplayHdr: the link runs in HDR (dynamic range 2 or flag bit 0).
bool link_is_hdr(const char* when) {
    OutputStatus st{};
    if (!read_status(g.handle, &st)) {
        dlog("hdr: display (%s): status unreadable", when);
        return false;
    }
    dlog("hdr: display (%s): dynamic range %u (%s), flags %#llx", when, st.dynamic_range,
         st.dynamic_range == 2 ? "HDR" : st.dynamic_range == 1 ? "SDR" : "unknown",
         static_cast<unsigned long long>(st.flags));
    return st.dynamic_range == 2 || (st.flags & 1);
}
#endif

void set_status(const std::string& text) {
    if (text == g.status) return;
    g.status = text;
    ++g.status_version;
    dlog("hdr: %s", text.c_str());
}

bool switch_scanout(bool hdr, int* rc_out) {
#ifdef PLATFORM_PS5_NATIVE
    if (g.handle < 0) return false;
    glFinish();  // nothing may still be drawing in the old format
    alignas(8) unsigned char attribute[128] = {};
    sceVideoOutSetBufferAttribute2(attribute, hdr ? kFormatHdr : kFormatSdr, 0, uint32_t(g.surface_w),
                                   uint32_t(g.surface_h), 0, 0, 0);
    const int32_t rc = sceVideoOutSubmitChangeBufferAttribute2(g.handle, 0, attribute, nullptr);
    dlog("hdr: scan-out to %s: %s (rc %#x)", hdr ? "HDR (BT.2020 PQ, 10-bit)" : "SDR", rc == 0 ? "in effect" : "refused",
         static_cast<unsigned>(rc));
    *rc_out = rc;
    return rc == 0;
#else
    (void)hdr;
    *rc_out = -1;
    return false;
#endif
}

constexpr const char* kVertex = R"GLSL(
void main() {
    vec2 p = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0))[gl_VertexID];
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Everything is floating point (no integer conversions): the packed word is built from its four bytes.
constexpr const char* kFragment = R"GLSL(
uniform sampler2D u_ui;
uniform sampler2D u_video;
uniform vec4 u_rect;        // the video's rectangle in pixels: x, y (from the top), width, height
uniform vec2 u_size;        // the surface in pixels
uniform float u_has_video;  // 0 none, 1 a 10-bit PQ texture
out vec4 frag_color;

vec3 pq_encode(vec3 l) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 p = pow(max(l, vec3(0.0)), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    vec2 fc = gl_FragCoord.xy;
    vec3 pq = vec3(0.0);
    if (u_has_video > 0.5) {
        vec2 uv = vec2((fc.x - u_rect.x) / u_rect.z, ((u_size.y - fc.y) - u_rect.y) / u_rect.w);
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) pq = texture(u_video, uv).rgb;
    }
    // The interface was drawn into a transparent target, so its colour is premultiplied. It is SDR
    // (sRGB-ish, BT.709): to light, to BT.2020, scaled so that white is 203 nits, to PQ.
    vec4 ui = texture(u_ui, fc / u_size);
    if (ui.a > 0.0) {
        vec3 c = clamp(ui.rgb / ui.a, 0.0, 1.0);
        vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
        vec3 wide = vec3(dot(lin, vec3(0.6274, 0.3293, 0.0433)), dot(lin, vec3(0.0691, 0.9195, 0.0114)),
                         dot(lin, vec3(0.0164, 0.0880, 0.8956)));
        pq = mix(pq, pq_encode(wide * (203.0 / 10000.0)), ui.a);
    }
    vec3 q = floor(clamp(pq, 0.0, 1.0) * 1023.0 + 0.5);
    // The 2:10:10:10 word (R in bits 9-0, G 19-10, B 29-20, 3 in 31-30) as the framebuffer's four bytes
    float b0 = mod(q.r, 256.0);
    float b1 = floor(q.r / 256.0) + 4.0 * mod(q.g, 64.0);
    float b2 = floor(q.g / 64.0) + 16.0 * mod(q.b, 16.0);
    float b3 = floor(q.b / 16.0) + 192.0;
    // a quarter of a step in: lands on the right byte whether the hardware rounds or cuts
    frag_color = (vec4(b2, b1, b0, b3) + 0.25) / 255.0;
}
)GLSL";

bool ensure_program() {
    if (g.program != 0) return true;
    g.program = hui::gfx::build_program("Stremio HDR final", kVertex, kFragment);
    if (g.program == 0) return false;
    glGenVertexArrays(1, &g.vao);
    glUseProgram(g.program);
    glUniform1i(glGetUniformLocation(g.program, "u_ui"), 0);
    glUniform1i(glGetUniformLocation(g.program, "u_video"), 1);
    glUseProgram(0);
    return true;
}

bool ensure_target(int w, int h) {
    if (g.ui_fbo != 0 && g.ui_w == w && g.ui_h == h) return true;
    if (g.ui_fbo != 0) {
        glDeleteFramebuffers(1, &g.ui_fbo);
        glDeleteTextures(1, &g.ui_tex);
        g.ui_fbo = g.ui_tex = 0;
    }
    glGenTextures(1, &g.ui_tex);
    glBindTexture(GL_TEXTURE_2D, g.ui_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g.ui_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g.ui_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.ui_tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        dlog("hdr: the interface target %dx%d is incomplete (%#x)", w, h, static_cast<unsigned>(status));
        return false;
    }
    g.ui_w = w;
    g.ui_h = h;
    return true;
}

}  // namespace

#ifdef PLATFORM_PS5_NATIVE
// The driver opened the video output and keeps its handle private. The app's
// own probe (display_output.cpp) opened and closed one just before, and the
// console hands that number out again: one status call checks it, made when
// the first HDR video plays, not at startup. No other handle number is tried;
// a sweep over 0-4095 shut the console down.
void probe_handle() {
    g.probed = true;
    uint64_t raw[16] = {};
    if (g.handle_hint >= 0 && sceVideoOutGetOutputStatus(g.handle_hint, raw) == 0) {
        g.handle = g.handle_hint;
        dlog("hdr: video out handle %d (the resolution probe's number, reused by the driver)", g.handle_hint);
        return;
    }
    char text[128];
    std::snprintf(text, sizeof(text), "Not available: the video output handle is unknown (probe hint %d)", g.handle_hint);
    g.no_handle_text = text;
    dlog("hdr: %s", g.no_handle_text.c_str());
}
#endif

void init(int surface_width, int surface_height, int32_t handle_hint) {
    g.surface_w = surface_width;
    g.surface_h = surface_height;
    g.handle_hint = handle_hint;
#ifdef PLATFORM_PS5_NATIVE
    dlog("hdr: ready; the video output is checked when the first HDR video plays (probe hint %d)", handle_hint);
#endif
}

void set_enabled(bool enabled) { g.enabled = enabled; }
bool scanout_hdr() { return g.hdr; }
bool wanted() { return g.wanted; }

bool update(bool want) {
    g.wanted = want;
#ifdef PLATFORM_PS5_NATIVE
    if (want && !g.probed) probe_handle();
#endif
    if (g.handle < 0) {
        set_status(g.probed ? g.no_handle_text : "Off");
        return false;
    }
    if (!g.enabled) {
        if (want) set_status("Off: disabled in Settings");
        want = false;
    }
    if (g.gl_failed) {
        set_status("Stopped: the HDR pass failed");
        want = false;
    }
    const double now = now_seconds();
    // PS5MC's rule: HDR only for an HDR10/HLG video AND a link that runs in HDR. Asked once, when the video starts.
#ifdef PLATFORM_PS5_NATIVE
    if (want && !g.want_prev) g.link_sdr = !link_is_hdr("video start");
#endif
    if (!want) g.link_sdr = false;
    g.want_prev = want;
    if (want && g.link_sdr) {
        want = false;  // SDR link: the video is tone mapped as before
        set_status("Off: the display link is SDR");
    }
    if (want && !g.hdr && g.failures < 2) {
        int rc = 0;
        if (switch_scanout(true, &rc)) {
            g.hdr = true;
            g.confirmed = false;
            g.switched_at = now;
            set_status("Switching to HDR");
        } else {
            ++g.failures;
            char text[96];
            std::snprintf(text, sizeof(text), "Refused by the console (code %#x)", static_cast<unsigned>(rc));
            set_status(text);
        }
    } else if (!want && g.hdr) {
        int rc = 0;
        if (switch_scanout(false, &rc)) {
            g.hdr = false;
            set_status("Off");
        }
    } else if (!want && !g.hdr && g.failures == 0 && !g.gl_failed && !g.link_sdr && g.enabled) {
        set_status("Off");
    }
    // A moment after the switch: what does the display say?
#ifdef PLATFORM_PS5_NATIVE
    if (g.hdr && !g.confirmed && now - g.switched_at > 1.5) {
        g.confirmed = true;
        OutputStatus st{};
        if (read_status(g.handle, &st) && (st.dynamic_range == 2 || (st.flags & 1))) {
            set_status("On: the TV is in HDR");
        } else {
            char text[112];
            std::snprintf(text, sizeof(text), "Switched, but the TV reports SDR (range %u)", st.dynamic_range);
            set_status(text);
        }
    }
#endif
    return g.hdr;
}

const char* status() { return g.status.c_str(); }
unsigned status_version() { return g.status_version; }

bool present(hui::gfx::Renderer& renderer, int width, int height, uint32_t video, const hui::gfx::Rect& video_rect,
             uint32_t target_framebuffer) {
    if (!ensure_program() || !ensure_target(width, height)) {
        g.gl_failed = true;
        return false;
    }
    // The interface into a transparent target (colour comes out premultiplied, alpha is the coverage)...
    glBindFramebuffer(GL_FRAMEBUFFER, g.ui_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer.present(g.ui_fbo, width, height);

    // ...then the video and the interface into the scan-out as 2:10:10:10 words.
    const hui::gfx::Viewport vp = hui::gfx::fit_viewport(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, target_framebuffer);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);  // the bytes must land exactly as computed
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.ui_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, video != 0 ? video : g.ui_tex);
    glUniform4f(glGetUniformLocation(g.program, "u_rect"), vp.offset_x + video_rect.x * vp.scale,
                vp.offset_y + video_rect.y * vp.scale, video_rect.w * vp.scale, video_rect.h * vp.scale);
    glUniform2f(glGetUniformLocation(g.program, "u_size"), float(width), float(height));
    glUniform1f(glGetUniformLocation(g.program, "u_has_video"), video != 0 ? 1.0f : 0.0f);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_DITHER);
    return true;
}

}  // namespace hdr_out
