// Stremio - preserve 4:2:0 source resolution and convert color on the GPU.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_gl.h"
#include "video_dovi.h"
#include "video_evo_shaders.h"
#include "gfx/gl_program.hpp"
#include "util.h"
#include <GL/glcorearb.h>
#include <cmath>
#include <chrono>
#include <string>

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
constexpr const char* fragment_begin = R"GLSL(
in vec2 uv;
out vec4 color;
uniform sampler2DRect ytex, utex, vtex;
uniform int layout_mode, color_mode, wide, sample_shift, full_range, hlg, dovi_active;
)GLSL";
constexpr const char* fragment_end = R"GLSL(

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
    // Rectangle planes use texel coordinates. Scale each plane independently:
    // odd 4:2:0 dimensions and interleaved chroma retain their own extents.
    vec2 yuv = uv * vec2(textureSize(ytex));
    if (layout_mode == 0) { color = vec4(texture(ytex, yuv).rgb, 1.0); return; }
    vec3 code;
    code.x = texture(ytex, yuv).r;
    if (layout_mode == 1) code.yz = texture(utex, uv * vec2(textureSize(utex))).rg;
    else {
        code.y = texture(utex, uv * vec2(textureSize(utex))).r;
        code.z = texture(vtex, uv * vec2(textureSize(vtex))).r;
    }
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
    if (color_mode == 2 || hlg != 0 || dovi_active != 0) {
        vec3 light = dovi_active != 0 ? dovi_nits(code / maximum) / 100.0 :
            hlg != 0 ? vec3(hlg_linear(rgb.r), hlg_linear(rgb.g), hlg_linear(rgb.b)) : pq(rgb);
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

void texture_params(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Rectangle textures have exactly one image level by definition.
    if (target == GL_TEXTURE_2D) glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
}

using UploadClock = std::chrono::steady_clock;
double elapsed_ms(UploadClock::time_point begin) {
    return std::chrono::duration<double, std::milli>(UploadClock::now() - begin).count();
}

GLenum consume_gl_errors() {
    const GLenum first = glGetError();
    // There is one error flag per GL error kind. Bound the drain even if the
    // context has failed; a sticky error must not block the interface.
    if (first != GL_NO_ERROR)
        for (unsigned i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
    return first;
}
}

VideoGl::VideoGl() = default;
VideoGl::~VideoGl() { release(); }
uint32_t VideoGl::texture() const { return valid_ ? output_ : 0; }
double VideoGl::fps() const { return period_ > 0 ? 1.0 / period_ : 0; }
bool VideoGl::init() {
    const std::string fragment = std::string(fragment_begin) + evo_shader::dovi + fragment_end;
    program_ = hui::gfx::build_program("Stremio YUV video", vertex, fragment.c_str());
    if (!program_) return false;
    glGenVertexArrays(1, &vao_); glGenFramebuffers(1, &framebuffer_);
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "ytex"), 0);
    glUniform1i(glGetUniformLocation(program_, "utex"), 1);
    glUniform1i(glGetUniformLocation(program_, "vtex"), 2);
    glGenBuffers(1, &dovi_buffer_);
    glBindBuffer(GL_UNIFORM_BUFFER, dovi_buffer_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(VideoDoviParams), nullptr, GL_DYNAMIC_DRAW);
    glUniformBlockBinding(program_, glGetUniformBlockIndex(program_, "DoviParams"), 3);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glUseProgram(0);
    return true;
}
void VideoGl::discard_textures() {
    if (output_ || planes_[0] || planes_[1] || planes_[2]) {
        // A texture deleted while attached to an unbound FBO retains its
        // backing store. Detach it before allocating the next video surface.
        if (framebuffer_) {
            GLint previous_draw = 0, previous_read = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(previous_draw));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(previous_read));
        }
        glDeleteTextures(3, planes_);
        glDeleteTextures(1, &output_);
    }
    output_ = planes_[0] = planes_[1] = planes_[2] = 0;
    width_ = height_ = 0;
    format_ = -1;
    valid_ = false;
}
void VideoGl::clear() {
    discard_textures();
    dovi_active_ = false;
    stamp_ = period_ = 0;
    previous_media_pts_ = -1;
    serial_ = -1;
    upload_timing_ = {};
}
void VideoGl::release() {
    clear();
    if (dovi_buffer_) glDeleteBuffers(1, &dovi_buffer_);
    dovi_buffer_ = 0; dovi_active_ = false;
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    output_ = framebuffer_ = vao_ = program_ = 0;
    planes_[0] = planes_[1] = planes_[2] = 0;
    failure_ = Failure::None;
}

bool VideoGl::reject(Failure failure, const char* stage, const Player::VideoFrame& f,
                     int plane, uint32_t gl_error, uint32_t framebuffer_status) {
    failure_ = failure;
    if (rejection_notes_++ < 16)
        dlog("video: frame rejected stage=%s gl_error=0x%04x framebuffer=0x%04x "
             "size=%dx%d layout=%d bits=%d bytes=%zu plane=%d "
             "offsets=%zu,%zu,%zu pitches=%zu,%zu,%zu",
             stage, gl_error, framebuffer_status, f.w, f.h, int(f.layout), f.wide ? 10 : 8,
             f.size(), plane, f.offsets[0], f.offsets[1], f.offsets[2],
             f.pitches[0], f.pitches[1], f.pitches[2]);
    return false;
}

bool VideoGl::upload(const Player::VideoFrame& f) {
    using Layout = Player::VideoFrame::Layout;
    upload_timing_ = {};
    failure_ = Failure::None;
    if (!program_ || !framebuffer_ || !vao_ || !dovi_buffer_)
        return reject(Failure::InvalidFrame, "renderer_objects", f);
    if (f.w <= 0 || f.h <= 0 || f.w > 8192 || f.h > 8192 || !f.data() || !f.size())
        return reject(Failure::InvalidFrame, "frame_descriptor", f);
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
            f.offsets[i] > f.size() || (height - 1) * pitch + row > f.size() - f.offsets[i])
            return reject(Failure::InvalidPlane, "plane_descriptor", f, i);
        row_pitches[i] = pitch;
    }
    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    const bool size_changed = width_ != f.w || height_ != f.h;
    if (size_changed || format_ != format) {
        const auto allocation_started = UploadClock::now();
        upload_timing_.reconfigured = true;
        const GLenum prior_error = consume_gl_errors();
        if (prior_error != GL_NO_ERROR && prior_gl_notes_++ < 8)
            dlog("video: prior GL error before resource setup gl_error=0x%04x", unsigned(prior_error));
        discard_textures();
        glActiveTexture(GL_TEXTURE0);
        auto failed_setup = [&](Failure failure, const char* stage, int plane,
                                GLenum error, GLenum status = 0) {
            discard_textures();
            glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous_fbo));
            glActiveTexture(GL_TEXTURE0);
            upload_timing_.allocation_ms = elapsed_ms(allocation_started);
            return reject(failure, stage, f, plane, error, status);
        };
        GLenum allocation_error = GL_NO_ERROR;
        auto allocate = [&](GLuint& texture, GLenum target, GLenum internal, GLenum external, GLenum type,
                            int width, int height) {
            glGenTextures(1, &texture);
            glBindTexture(target, texture); texture_params(target);
            // This pinned Mesa's mutable allocation path flushes and retries
            // once on resource exhaustion. TexStorage returns OOM immediately.
            // MAX_LEVEL and the non-mipmap filter above keep one image level.
            glTexImage2D(target, 0, GLint(internal), width, height, 0, external, type, nullptr);
            allocation_error = consume_gl_errors();
            return texture != 0 && allocation_error == GL_NO_ERROR;
        };
        // The 4K RGBA target needs a contiguous 32 MiB region aligned to 2 MiB.
        // Allocate it before smaller planes fragment the remaining regions.
        if (!allocate(output_, GL_TEXTURE_2D, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, f.w, f.h))
            return failed_setup(Failure::Allocation, "output_storage", -1, allocation_error);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_, 0);
        const GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum framebuffer_error = consume_gl_errors();
        if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE || framebuffer_error != GL_NO_ERROR)
            return failed_setup(framebuffer_error == GL_OUT_OF_MEMORY ? Failure::Allocation : Failure::Framebuffer,
                                "output_framebuffer", -1, framebuffer_error, framebuffer_status);
        for (int i = 0; i < count; ++i) {
            const GLenum internal = layout == 0 ? GL_RGBA8 : (i == 1 && layout == 1) ? (f.wide ? GL_RG16 : GL_RG8) : (f.wide ? GL_R16 : GL_R8);
            const GLenum external = layout == 0 ? GL_BGRA : (i == 1 && layout == 1) ? GL_RG : GL_RED;
            const GLenum type = f.wide && layout != 0 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
            // The pinned PS5 driver tiles renderable single-level 2D R8/RG8
            // images during every CPU upload. Rectangle sampled planes retain
            // linear storage, avoiding that per-pixel CPU tiling loop. Keep the
            // rendered RGBA output 2D for the UI and never render to a plane.
            if (!allocate(planes_[i], GL_TEXTURE_RECTANGLE, internal, external, type,
                          i == 0 ? f.w : cw, i == 0 ? f.h : ch))
                return failed_setup(Failure::Allocation, "plane_storage", i, allocation_error);
        }
        width_ = f.w; height_ = f.h; format_ = format;
        dlog("video: GPU conversion %dx%d, layout=%d, source=%d-bit, output SDR, upload=rectangle", f.w, f.h, layout, f.wide ? 10 : 8);
        upload_timing_.allocation_ms = elapsed_ms(allocation_started);
    }
    const auto planes_started = UploadClock::now();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (int i = 0; i < count; ++i) {
        glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_RECTANGLE, planes_[i]);
        const GLenum external = layout == 0 ? GL_BGRA : (i == 1 && layout == 1) ? GL_RG : GL_RED;
        const size_t pixel_bytes = layout == 0 ? 4 : bytes * ((i == 1 && layout == 1) ? 2 : 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(row_pitches[i] / pixel_bytes));
        glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, i == 0 ? f.w : cw, i == 0 ? f.h : ch, external,
                       f.wide && layout != 0 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, f.data() + f.offsets[i]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    upload_timing_.plane_ms = elapsed_ms(planes_started);
    const auto draw_started = UploadClock::now();
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_); glViewport(0, 0, f.w, f.h);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(program_); glBindVertexArray(vao_);
    auto uniform = [&](const char* name, int value) { glUniform1i(glGetUniformLocation(program_, name), value); };
    uniform("layout_mode", layout); uniform("wide", f.wide); uniform("sample_shift", f.shift);
    uniform("full_range", f.full_range); uniform("hlg", f.hlg);
    dovi_active_ = bool(f.dovi);
    uniform("dovi_active", dovi_active_);
    if (f.dovi) {
        glBindBuffer(GL_UNIFORM_BUFFER, dovi_buffer_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(VideoDoviParams), f.dovi.get());
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, dovi_buffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    uniform("color_mode", f.colors == YuvColors::Bt601 ? 0 : f.colors == YuvColors::Hdr10 ? 2 : 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    upload_timing_.draw_ms = elapsed_ms(draw_started);
    // Cadence is diagnostic only. Rendering always exposes this source-sized
    // conversion texture, and the UI applies ordinary bilinear scaling.
    const double time = f.presentation_time > 0 ? f.presentation_time :
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (size_changed || serial_ != f.serial || stamp_ <= 0 || time != stamp_) {
        const bool jumped = size_changed || serial_ != f.serial || !std::isfinite(time) ||
            (stamp_ > 0 && (time <= stamp_ || time - stamp_ > 0.2)) ||
            (previous_media_pts_ >= 0 && std::abs(f.pts - previous_media_pts_) > 1.0);
        if (jumped) period_ = 0;
        else if (stamp_ > 0) {
            const double elapsed = time - stamp_;
            period_ = period_ > 0 ? period_ * 0.8 + elapsed * 0.2 : elapsed;
        }
        stamp_ = time;
        previous_media_pts_ = f.pts;
        serial_ = f.serial;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous_fbo)); glActiveTexture(GL_TEXTURE0);
    valid_ = true;
    aspect_ = std::isfinite(f.display_aspect) && f.display_aspect > 0 ? f.display_aspect : double(f.w)/f.h;
    return true;
}
