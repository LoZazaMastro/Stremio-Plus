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
int stremio_videoout_handle(void);
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
    bool enabled = true;
    bool hdr = false;
    int failures = 0;
    int restore_failures = 0;
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
    GLuint glass_program = 0, glass_fbo = 0, glass_tex = 0;
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

// The sampled source planes stay linear rectangle textures on the pinned
// driver. Convert them only when writing scanout (and the tiny glass copy),
// as the credited upstream port's direct-plane path does.
constexpr const char* kPlaneSampling = R"GLSL(
uniform sampler2DRect u_yplane, u_uplane, u_vplane;
uniform int u_plane_layout, u_plane_wide, u_plane_shift, u_plane_full, u_plane_hlg;
uniform int u_direct_planes;
vec3 sample_planes(vec2 uv) {
    vec3 code;
    code.x = texture(u_yplane, uv * vec2(textureSize(u_yplane))).r;
    if (u_plane_layout == 1) code.yz = texture(u_uplane, uv * vec2(textureSize(u_uplane))).rg;
    else {
        code.y = texture(u_uplane, uv * vec2(textureSize(u_uplane))).r;
        code.z = texture(u_vplane, uv * vec2(textureSize(u_vplane))).r;
    }
    code *= u_plane_wide != 0 ? 65535.0 / exp2(float(u_plane_shift)) : 255.0;
    float factor = u_plane_wide != 0 ? 4.0 : 1.0;
    float maximum = u_plane_wide != 0 ? 1023.0 : 255.0;
    float y = u_plane_full != 0 ? code.x / maximum : (code.x - 16.0*factor) / (219.0*factor);
    vec2 c = (code.yz - 128.0*factor) / (u_plane_full != 0 ? maximum : 224.0*factor);
    vec3 signal = clamp(vec3(y + 1.4746*c.y, y - 0.16455*c.x - 0.57135*c.y, y + 1.8814*c.x), 0.0, 1.0);
    if (u_plane_hlg == 0) return signal;
    vec3 low = signal*signal / 3.0;
    vec3 high = (exp((signal - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
    vec3 scene = mix(low, high, step(0.5, signal));
    float ys = dot(scene, vec3(0.2627, 0.6780, 0.0593));
    vec3 display = scene * pow(max(ys, 1e-6), 0.2) * (1000.0 / 10000.0);
    vec3 p = pow(max(display, vec3(0.0)), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625*p) / (1.0 + 18.6875*p), vec3(78.84375));
}
)GLSL";

void bind_planes(GLuint program, const VideoGl::HdrPlanes* planes) {
    glUniform1i(glGetUniformLocation(program, "u_direct_planes"), planes ? 1 : 0);
    // Give every sampler its own unit even for the inactive branch. Leaving
    // sampler2DRect on a sampler2D unit makes a draw invalid on core GL.
    const char* names[] = {"u_yplane", "u_uplane", "u_vplane"};
    for (int i = 0; i < 3; ++i) {
        glUniform1i(glGetUniformLocation(program, names[i]), 2 + i);
        glActiveTexture(GL_TEXTURE0 + 2 + i);
        glBindTexture(GL_TEXTURE_RECTANGLE, planes ? planes->texture[i] : 0);
    }
    if (planes) {
        glUniform1i(glGetUniformLocation(program, "u_plane_layout"), planes->layout);
        glUniform1i(glGetUniformLocation(program, "u_plane_wide"), planes->wide);
        glUniform1i(glGetUniformLocation(program, "u_plane_shift"), planes->shift);
        glUniform1i(glGetUniformLocation(program, "u_plane_full"), planes->full_range);
        glUniform1i(glGetUniformLocation(program, "u_plane_hlg"), planes->hlg);
    }
}

// Everything is floating point (no integer conversions): the packed word is built from its four bytes.
constexpr const char* kFragment = R"GLSL(
uniform sampler2D u_ui;
uniform sampler2D u_video;
uniform vec4 u_rect;        // the video's rectangle in pixels: x, y (from the top), width, height
uniform vec2 u_size;        // the surface in pixels
uniform float u_has_video;  // 0 none, 1 a 10-bit PQ texture
uniform float u_video_pq;
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
vec3 pq_decode(vec3 code) {
    vec3 p = pow(clamp(code, 0.0, 1.0), vec3(1.0 / 78.84375));
    return pow(max(p - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * p, 1e-6),
               vec3(1.0 / 0.1593017578125));
}
vec3 sdr_light(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
    return vec3(dot(lin, vec3(0.6274, 0.3293, 0.0433)), dot(lin, vec3(0.0691, 0.9195, 0.0114)),
                dot(lin, vec3(0.0164, 0.0880, 0.8956))) * (203.0 / 10000.0);
}

void main() {
    vec2 fc = gl_FragCoord.xy;
    vec3 pq = vec3(0.0);
    if (u_has_video > 0.5) {
        vec2 uv = vec2((fc.x - u_rect.x) / u_rect.z, ((u_size.y - fc.y) - u_rect.y) / u_rect.w);
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
            pq = u_direct_planes != 0 ? sample_planes(uv) : texture(u_video, uv).rgb;
            if (u_direct_planes == 0 && u_video_pq < 0.5) pq = pq_encode(sdr_light(pq));
        }
    }
    // The interface was drawn into a transparent target, so its colour is premultiplied. It is SDR
    // (sRGB-ish, BT.709): to light, to BT.2020, scaled so that white is 203 nits, to PQ.
    vec4 ui = texture(u_ui, fc / u_size);
    if (ui.a > 0.0) {
        vec3 c = clamp(ui.rgb / ui.a, 0.0, 1.0);
        // Coverage blends light, not PQ codes. Keep the uncovered movie on
        // the inexpensive direct PQ path without an encode/decode roundtrip.
        pq = pq_encode(mix(pq_decode(pq), sdr_light(c), ui.a));
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
    const std::string fragment = std::string(kPlaneSampling) + kFragment;
    g.program = hui::gfx::build_program("Stremio HDR final", kVertex, fragment.c_str());
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
    for (unsigned i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
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
    const GLenum error = glGetError();
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (error != GL_NO_ERROR || status != GL_FRAMEBUFFER_COMPLETE) {
        dlog("hdr: the interface target %dx%d failed gl=%#x framebuffer=%#x", w, h,
             static_cast<unsigned>(error), static_cast<unsigned>(status));
        glDeleteFramebuffers(1, &g.ui_fbo);
        glDeleteTextures(1, &g.ui_tex);
        g.ui_fbo = g.ui_tex = 0;
        return false;
    }
    g.ui_w = w;
    g.ui_h = h;
    dlog("hdr: interface target %dx%d, %.2f MiB logical storage; one extra composition pass",
         w, h, double(w) * h * 4 / (1 << 20));
    return true;
}

}  // namespace

void init(int surface_width, int surface_height) {
    g.surface_w = surface_width;
    g.surface_h = surface_height;
    dlog("hdr: initialized for %dx%d; renderer-owned VideoOut handle only", surface_width, surface_height);
}

void set_enabled(bool enabled) { g.enabled = enabled; }
bool scanout_hdr() { return g.hdr; }
bool wanted() { return g.wanted; }
bool safe() { return g.restore_failures < 2; }

void discard_targets() {
    // Once scanout is SDR, return the HDR-only surfaces before an SDR video
    // retry needs its own full-size target. Keep resources during rollback.
    if (g.ui_fbo) glDeleteFramebuffers(1, &g.ui_fbo);
    if (g.ui_tex) glDeleteTextures(1, &g.ui_tex);
    if (g.glass_fbo) glDeleteFramebuffers(1, &g.glass_fbo);
    if (g.glass_tex) glDeleteTextures(1, &g.glass_tex);
    g.ui_fbo = g.ui_tex = g.glass_fbo = g.glass_tex = 0;
    g.ui_w = g.ui_h = 0;
}

void failed(const char* reason) {
    g.gl_failed = true;
    if (!g.hdr) discard_targets();
    set_status(std::string("Off: ") + reason);
}

bool prepare(bool want) {
    g.wanted = want;
    const bool effective = want && g.enabled && !g.gl_failed;
#ifdef PLATFORM_PS5_NATIVE
    const int current_handle = stremio_videoout_handle();
    if (current_handle != g.handle) {
        // A renderer restart must not submit changes to the closed port.
        g.handle = current_handle;
        g.hdr = false;
        g.want_prev = false;
        g.failures = g.restore_failures = 0;
        dlog("hdr: renderer-owned video out handle %d", g.handle);
    }
#endif
    if (!effective) {
        g.want_prev = false;
        g.link_sdr = false;
        if (want && !g.enabled) set_status("Off: disabled in Settings");
        else if (!g.gl_failed && !g.hdr) set_status("Off");
        return false;
    }
    if (g.handle < 0) {
        set_status("Off: renderer video output unavailable");
        return false;
    }
#ifdef PLATFORM_PS5_NATIVE
    if (!g.want_prev) g.link_sdr = !link_is_hdr("video start");
#endif
    g.want_prev = true;
    if (g.link_sdr) {
        set_status("Off: the display link is SDR");
        return false;
    }
    if (g.failures >= 2 || !safe()) return false;
    // Allocate before changing the display encoding. On failure the caller
    // uploads an ordinary SDR frame, without restarting the video decoder.
    if (!ensure_program() || !ensure_target(g.surface_w, g.surface_h)) {
        failed("HDR composition resources unavailable; using SDR");
        return false;
    }
    return true;
}

bool update(bool want) {
    // true is a commit: the caller already has a successfully converted PQ
    // frame and prepare() already validated the display and GL resources.
    want = want && g.enabled && !g.gl_failed;
    const double now = now_seconds();
    if (want && !g.hdr && g.failures < 2 && g.handle >= 0) {
        int rc = 0;
        if (switch_scanout(true, &rc)) {
            g.hdr = true;
            g.confirmed = false;
            g.switched_at = now;
            g.restore_failures = 0;
            set_status("Switching to HDR");
        } else {
            ++g.failures;
            char text[96];
            std::snprintf(text, sizeof(text), "Off: HDR request refused (code %#x)", static_cast<unsigned>(rc));
            set_status(text);
        }
    } else if (!want && g.hdr && safe()) {
        int rc = 0;
        if (switch_scanout(false, &rc)) {
            g.hdr = false;
            g.restore_failures = 0;
            if (g.gl_failed) discard_targets();
            if (!g.gl_failed) set_status("Off");
        } else {
            ++g.restore_failures;
            char text[128];
            std::snprintf(text, sizeof(text), "SDR restoration failed (%d/2, code %#x)",
                          g.restore_failures, static_cast<unsigned>(rc));
            set_status(text);
        }
    }
#ifdef PLATFORM_PS5_NATIVE
    if (g.hdr && want && !g.confirmed && now - g.switched_at > 1.5) {
        g.confirmed = true;
        OutputStatus st{};
        if (read_status(g.handle, &st) && (st.dynamic_range == 2 || (st.flags & 1))) {
            set_status("On: the TV is in HDR");
        } else {
            // An accepted request without HDR readback is not a successful
            // HDR test. Restore SDR and keep that failure visible in logs.
            failed("HDR scan-out could not be confirmed; using SDR");
        }
    }
#else
    (void)now;
#endif
    return g.hdr;
}

void release() {
    update(false);
    glDeleteFramebuffers(1, &g.ui_fbo);
    glDeleteTextures(1, &g.ui_tex);
    glDeleteFramebuffers(1, &g.glass_fbo);
    glDeleteTextures(1, &g.glass_tex);
    glDeleteProgram(g.program);
    glDeleteProgram(g.glass_program);
    glDeleteVertexArrays(1, &g.vao);
    g = State{};
}

const char* status() { return g.status.c_str(); }
unsigned status_version() { return g.status_version; }

bool present(hui::gfx::Renderer& renderer, int width, int height, uint32_t video, const hui::gfx::Rect& video_rect,
             uint32_t target_framebuffer, bool video_pq, const VideoGl::HdrPlanes* planes) {
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
    glUniform1f(glGetUniformLocation(g.program, "u_has_video"), video != 0 || planes ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(g.program, "u_video_pq"), video_pq ? 1.0f : 0.0f);
    bind_planes(g.program, planes);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_DITHER);
    return true;
}

uint32_t glass_texture(uint32_t video, bool video_pq, const VideoGl::HdrPlanes* planes) {
    if (!planes && (!video || !video_pq)) return video;
    constexpr int width = 480, height = 270;
    if (!g.glass_program) {
        // The original SDR tone map, only at the glass capture's size.
        constexpr const char* fragment = R"GLSL(
uniform sampler2D source;
out vec4 color;
float hable(float x) {
    return ((x * (0.15*x + 0.05) + 0.004) / (x * (0.15*x + 0.5) + 0.06)) - 0.02 / 0.3;
}
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(480.0, 270.0);
    vec3 signal = u_direct_planes != 0 ? sample_planes(uv) : texture(source, uv).rgb;
    vec3 p = pow(clamp(signal, 0.0, 1.0), vec3(1.0 / 78.84375));
    vec3 light = pow(max(p - 0.8359375, 0.0) / max(18.8515625 - 18.6875*p, 1e-6),
                     vec3(1.0 / 0.1593017578125)) * 250.0;
    light = vec3(dot(light, vec3(1.6605, -0.5876, -0.0728)),
                 dot(light, vec3(-0.1246, 1.1329, -0.0083)), dot(light, vec3(-0.0182, -0.1006, 1.1187)));
    float peak = max(max(light.r, light.g), max(light.b, 1e-6));
    vec3 v = clamp(light * hable(peak) / (hable(10.0) * peak), 0.0, 1.0);
    color = vec4(mix(1.099 * pow(v, vec3(0.45)) - 0.099, 4.5*v, lessThan(v, vec3(0.018))), 1.0);
}
)GLSL";
        const std::string complete = std::string(kPlaneSampling) + fragment;
        g.glass_program = hui::gfx::build_program("Stremio HDR glass", kVertex, complete.c_str());
        if (!g.glass_program) return 0;
    }
    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    if (!g.glass_fbo) {
        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1, &g.glass_tex);
        glBindTexture(GL_TEXTURE_2D, g.glass_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &g.glass_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g.glass_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.glass_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &g.glass_fbo);
            glDeleteTextures(1, &g.glass_tex);
            g.glass_fbo = g.glass_tex = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous_fbo));
            return 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g.glass_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(g.glass_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, video);
    glUniform1i(glGetUniformLocation(g.glass_program, "source"), 0);
    bind_planes(g.glass_program, planes);
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous_fbo));
    glActiveTexture(GL_TEXTURE0);
    return g.glass_tex;
}

}  // namespace hdr_out
