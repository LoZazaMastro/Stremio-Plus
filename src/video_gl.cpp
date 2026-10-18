// Stremio - preserve 4:2:0 source resolution and convert color on the GPU.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_gl.h"
#include "gfx/gl_program.hpp"
#include "util.h"
#include <GL/glcorearb.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr const char* vertex = R"GLSL(
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// PQ/Hable constants and BT.2020->709 transform match the existing CPU
// reference in yuv_convert.cpp. HLG follows its inverse OETF and SDR output.
constexpr const char* fragment = R"GLSL(
in vec2 uv;
out vec4 color;
uniform sampler2D ytex, utex, vtex;
uniform int layout_mode, color_mode, wide, sample_shift, full_range, hlg;

vec3 pq(vec3 v) {
    vec3 p = pow(clamp(v, 0.0, 1.0), vec3(1.0 / 78.84375));
    return pow(max(p - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * p, 0.000001),
               vec3(1.0 / 0.1593017578125)) * 100.0;
}
float hable(float x) {
    return ((x * (0.15*x + 0.05) + 0.004) / (x * (0.15*x + 0.5) + 0.06)) - 0.02 / 0.3;
}
float hlg_linear(float x) {
    x = clamp(x, 0.0, 1.0);
    float scene = x <= 0.5 ? x*x / 3.0 : (exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
    return pow(scene, 1.2) * 10.0;
}
vec3 transfer709(vec3 v) {
    v = clamp(v, 0.0, 1.0);
    return mix(1.099 * pow(v, vec3(0.45)) - 0.099, 4.5 * v, lessThan(v, vec3(0.018)));
}
void main() {
    if (layout_mode == 0) { color = vec4(texture(ytex, uv).rgb, 1.0); return; }
    vec3 code;
    code.x = texture(ytex, uv).r;
    if (layout_mode == 1) code.yz = texture(utex, uv).rg;
    else { code.y = texture(utex, uv).r; code.z = texture(vtex, uv).r; }
    float scale = wide != 0 ? 65535.0 / exp2(float(sample_shift)) : 255.0;
    code *= scale;
    float factor = wide != 0 ? 4.0 : 1.0;
    float maximum = wide != 0 ? 1023.0 : 255.0;
    float y = full_range != 0 ? code.x / maximum : (code.x - 16.0*factor) / (219.0*factor);
    vec2 c = (code.yz - 128.0*factor) / (full_range != 0 ? maximum : 224.0*factor);
    vec3 rgb;
    if (color_mode == 2 || hlg != 0) rgb = vec3(y + 1.4746*c.y, y - 0.16455*c.x - 0.57135*c.y, y + 1.8814*c.x);
    else if (color_mode == 0) rgb = vec3(y + 1.402*c.y, y - 0.344136*c.x - 0.714136*c.y, y + 1.772*c.x);
    else rgb = vec3(y + 1.5748*c.y, y - 0.187324*c.x - 0.468124*c.y, y + 1.8556*c.x);
    if (color_mode == 2 || hlg != 0) {
        vec3 light = hlg != 0 ? vec3(hlg_linear(rgb.r), hlg_linear(rgb.g), hlg_linear(rgb.b)) : pq(rgb);
        light *= 2.5;
        light = vec3(dot(light, vec3(1.6605, -0.5876, -0.0728)),
                     dot(light, vec3(-0.1246, 1.1329, -0.0083)),
                     dot(light, vec3(-0.0182, -0.1006, 1.1187)));
        float peak = max(max(light.r, light.g), max(light.b, 0.000001));
        rgb = transfer709(light * hable(peak) / (hable(10.0) * peak));
    }
    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)GLSL";

void texture_params() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}
}

VideoGl::~VideoGl() { release(); }
bool VideoGl::init() {
    program_ = hui::gfx::build_program("Stremio YUV video", vertex, fragment);
    if (!program_) return false;
    glGenVertexArrays(1, &vao_); glGenFramebuffers(1, &framebuffer_);
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "ytex"), 0);
    glUniform1i(glGetUniformLocation(program_, "utex"), 1);
    glUniform1i(glGetUniformLocation(program_, "vtex"), 2);
    glUseProgram(0);
    return true;
}
void VideoGl::clear() { valid_ = false; }
void VideoGl::release() {
    if (output_) glDeleteTextures(1, &output_);
    if (planes_[0] || planes_[1] || planes_[2]) glDeleteTextures(3, planes_);
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    output_ = framebuffer_ = vao_ = program_ = 0;
    planes_[0] = planes_[1] = planes_[2] = 0;
    width_ = height_ = 0; format_ = -1; valid_ = false;
}

bool VideoGl::upload(const Player::VideoFrame& f) {
    using Layout = Player::VideoFrame::Layout;
    if (!program_ || f.w <= 0 || f.h <= 0 || f.w > 8192 || f.h > 8192 || !f.data() || !f.size()) return false;
    const int layout = f.layout == Layout::Bgra ? 0 : f.layout == Layout::Nv12 ? 1 : 2;
    const int format = layout + (f.wide ? 4 : 0);
    const int cw = (f.w + 1) / 2, ch = (f.h + 1) / 2;
    const int bytes = f.wide ? 2 : 1;
    const int count = layout == 0 ? 1 : layout == 1 ? 2 : 3;
    std::array<size_t, 3> row_pitches{};
    for (int i = 0; i < count; ++i) {
        const size_t pixel_bytes = layout == 0 ? 4 : bytes * ((i == 1 && layout == 1) ? 2 : 1);
        const size_t row = size_t(i == 0 ? f.w : cw) * pixel_bytes;
        const size_t height = size_t(i == 0 ? f.h : ch);
        const size_t pitch = f.pitches[i] ? f.pitches[i] : row;
        if (pitch < row || pitch % pixel_bytes || pitch / pixel_bytes > 32768 ||
            f.offsets[i] > f.size() || (height - 1) * pitch + row > f.size() - f.offsets[i]) return false;
        row_pitches[i] = pitch;
    }
    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    if (width_ != f.w || height_ != f.h || format_ != format) {
        glDeleteTextures(3, planes_); glDeleteTextures(1, &output_);
        glGenTextures(3, planes_); glGenTextures(1, &output_);
        for (int i = 0; i < count; ++i) {
            glBindTexture(GL_TEXTURE_2D, planes_[i]); texture_params();
            const GLenum internal = layout == 0 ? GL_RGBA8 : (i == 1 && layout == 1) ? (f.wide ? GL_RG16 : GL_RG8) : (f.wide ? GL_R16 : GL_R8);
            glTexStorage2D(GL_TEXTURE_2D, 1, internal, i == 0 ? f.w : cw, i == 0 ? f.h : ch);
        }
        glBindTexture(GL_TEXTURE_2D, output_); texture_params();
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, f.w, f.h);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous_fbo)); valid_ = false; return false;
        }
        width_ = f.w; height_ = f.h; format_ = format;
        dlog("video: GPU conversion %dx%d, layout=%d, source=%d-bit, output SDR", f.w, f.h, layout, f.wide ? 10 : 8);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (int i = 0; i < count; ++i) {
        glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, planes_[i]);
        const GLenum external = layout == 0 ? GL_BGRA : (i == 1 && layout == 1) ? GL_RG : GL_RED;
        const size_t pixel_bytes = layout == 0 ? 4 : bytes * ((i == 1 && layout == 1) ? 2 : 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(row_pitches[i] / pixel_bytes));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, i == 0 ? f.w : cw, i == 0 ? f.h : ch, external,
                       f.wide && layout != 0 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, f.data() + f.offsets[i]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_); glViewport(0, 0, f.w, f.h);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(program_); glBindVertexArray(vao_);
    auto uniform = [&](const char* name, int value) { glUniform1i(glGetUniformLocation(program_, name), value); };
    uniform("layout_mode", layout); uniform("wide", f.wide); uniform("sample_shift", f.shift);
    uniform("full_range", f.full_range); uniform("hlg", f.hlg);
    uniform("color_mode", f.colors == YuvColors::Bt601 ? 0 : f.colors == YuvColors::Hdr10 ? 2 : 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous_fbo)); glActiveTexture(GL_TEXTURE0);
    valid_ = true;
    aspect_ = std::isfinite(f.display_aspect) && f.display_aspect > 0 ? f.display_aspect : double(f.w)/f.h;
    return true;
}
