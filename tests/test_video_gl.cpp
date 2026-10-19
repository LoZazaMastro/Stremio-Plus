// Host GPU checks: real GLSL conversion, real textures and framebuffer readback.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_gl.h"
#include "video_dovi.h"
#include "gfx/gl_program.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Frame = Player::VideoFrame;
using Layout = Frame::Layout;
int failures = 0, checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); }
}

Frame solid(Layout layout, bool wide, int shift, int y, int u, int v, int w = 8, int h = 6) {
    Frame f;
    f.layout = layout; f.wide = wide; f.shift = shift;
    f.w = w; f.h = h; f.display_aspect = double(w)/h;
    const size_t bytes = wide ? 2 : 1, chroma = size_t((w+1)/2)*((h+1)/2);
    f.offsets[1] = size_t(w)*h*bytes;
    f.offsets[2] = f.offsets[1] + chroma*bytes*(layout == Layout::Nv12 ? 2 : 1);
    f.pixels.resize(f.offsets[1] + chroma*bytes*2);
    auto set = [&](size_t at, int value) {
        if (wide) { const std::uint16_t sample = std::uint16_t(value << shift); std::memcpy(f.pixels.data()+at, &sample, 2); }
        else f.pixels[at] = std::uint8_t(value);
    };
    for (size_t i = 0; i < size_t(w)*h; ++i) set(i*bytes, y);
    for (size_t i = 0; i < chroma; ++i) {
        if (layout == Layout::Nv12) { set(f.offsets[1]+i*bytes*2, u); set(f.offsets[1]+(i*2+1)*bytes, v); }
        else { set(f.offsets[1]+i*bytes, u); set(f.offsets[2]+i*bytes, v); }
    }
    return f;
}
std::vector<unsigned char> read(VideoGl& gpu, const Frame& f) {
    check(gpu.upload(f), "GPU accepts packed source frame");
    std::vector<unsigned char> pixels(size_t(f.w)*f.h*4);
    glBindTexture(GL_TEXTURE_2D, gpu.texture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    check(glGetError() == GL_NO_ERROR, "conversion and readback have no GL error");
    return pixels;
}
void near(const std::vector<unsigned char>& pixels, int r, int g, int b, int tolerance, const char* message) {
    bool equal = true;
    for (size_t i = 0; i < pixels.size(); i += 4)
        equal = equal && std::abs(int(pixels[i])-r) <= tolerance &&
            std::abs(int(pixels[i+1])-g) <= tolerance && std::abs(int(pixels[i+2])-b) <= tolerance && pixels[i+3] == 255;
    if (!equal) std::fprintf(stderr, "observed %d,%d,%d; expected %d,%d,%d\n", pixels[0],pixels[1],pixels[2],r,g,b);
    check(equal, message);
}

int main() {
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) return 2;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    EGLContext context = eglCreateContext(display,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) return 2;
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    {
        VideoGl gpu;
        check(gpu.init(), "video shader compiles and links");
        for (auto layout : {Layout::Nv12, Layout::Planar420}) {
            for (int bits : {8,10}) {
                for (int shift : {0,6}) {
                    if (bits == 8 && shift) continue;
                    const int factor = bits == 10 ? 4 : 1;
                    auto black = solid(layout,bits==10,shift,16*factor,128*factor,128*factor);
                    near(read(gpu,black),0,0,0,1,"limited-range black");
                    auto white = solid(layout,bits==10,shift,235*factor,128*factor,128*factor);
                    near(read(gpu,white),255,255,255,1,"limited-range white");
                    auto red = solid(layout,bits==10,shift,81*factor,90*factor,240*factor);
                    red.colors = YuvColors::Bt601;
                    near(read(gpu,red),254,0,0,2,"BT.601 color matrix");
                    auto neutral = solid(layout,bits==10,shift,bits==10?1023:255,128*factor,128*factor);
                    neutral.full_range = true;
                    near(read(gpu,neutral),255,255,255,1,"full-range white");
                    neutral = solid(layout,bits==10,shift,0,128*factor,128*factor);
                    neutral.full_range = true;
                    near(read(gpu,neutral),0,0,0,1,"full-range black");
                }
            }
        }
        // Software P010's high bits and VideoDec2's low bits are equivalent.
        auto low = solid(Layout::Nv12,true,0,510,450,615);
        auto high = solid(Layout::Nv12,true,6,510,450,615);
        auto a = read(gpu,low), b = read(gpu,high);
        check(a == b, "low/high aligned Main10 yield identical pixels");
        // PQ output is checked against the existing scalar reference, not
        // against a copy of the shader. LUT quantization permits 3 code values.
        for (const auto& sample : {std::array<int,3>{64,512,512}, {450,480,550}, {700,580,450}, {940,512,512}}) {
            auto f = solid(Layout::Planar420,true,0,sample[0],sample[1],sample[2]);
            f.colors = YuvColors::Hdr10;
            YuvPicture p;
            p.y = f.pixels.data(); p.u = p.y + f.offsets[1]; p.v = p.y + f.offsets[2];
            p.y_stride = f.w*2; p.c_stride = ((f.w+1)/2)*2;
            p.width = f.w; p.height = f.h; p.wide = true;
            std::vector<std::uint8_t> reference(size_t(f.w)*f.h*4);
            yuv_to_bgra(p,YuvColors::Hdr10,1,reference.data(),f.w,0,f.h);
            near(read(gpu,f),reference[2],reference[1],reference[0],3,"PQ / BT.2020 / Hable agrees with scalar reference");
        }
        auto hlg = solid(Layout::Nv12,true,0,64,512,512);
        hlg.hlg = true;
        near(read(gpu,hlg),0,0,0,1,"HLG black is finite and black");
        hlg = solid(Layout::Nv12,true,0,940,512,512); hlg.hlg = true;
        near(read(gpu,hlg),255,255,255,1,"HLG reference white maps to SDR white");
        auto odd = solid(Layout::Planar420,false,0,126,128,128,9,7);
        near(read(gpu,odd),128,128,128,1,"odd chroma dimensions are retained");
        auto large = solid(Layout::Nv12,true,0,504,512,512,3840,2160);
        auto pixels = read(gpu,large);
        GLint width = 0,height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
        check(width == 3840 && height == 2160,"4K source is not downscaled before presentation");
        // BGRA fallback and row orientation survive the GPU conversion.
        Frame bgra; bgra.w = 2; bgra.h = 2;
        bgra.pixels = {0,0,255,255, 0,255,0,255, 255,0,0,255, 255,255,255,255};
        const auto corners = read(gpu,bgra);
        check(corners == std::vector<unsigned char>({255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255}),
            "BGRA channels and source row order are preserved");
        bgra.pixels.resize(3);
        const auto clean_texture = gpu.texture();
        check(!gpu.upload(bgra),"truncated frame is rejected before texture upload");
        check(gpu.texture() == clean_texture,"a rejected picture retains the last clean image");
        gpu.clear(); check(gpu.texture() == 0,"closing playback clears the displayed frame");

        auto read_presented = [&] {
            glBindTexture(GL_TEXTURE_2D, gpu.texture());
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            std::vector<unsigned char> values(size_t(w) * h * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, values.data());
            check(glGetError() == GL_NO_ERROR, "direct texture readback has no GL error");
            return values;
        };
        // Playback retains one source-sized conversion surface. The UI scales
        // this texture directly, without a reconstructed 4K intermediate.
        auto first = solid(Layout::Planar420, false, 0, 81, 90, 240, 1920, 1080);
        first.colors = YuvColors::Bt601;
        first.presentation_time = 20; first.pts = 0; first.serial = 1;
        check(gpu.upload(first), "1080p direct rendering accepts the first frame");
        const auto direct_texture = gpu.texture();
        near(read_presented(),254,0,0,2,"the first 1080p source reaches the output unchanged");
        GLint min_filter = 0, mag_filter = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
        check(width == 1920 && height == 1080, "1080p output keeps the source resolution");
        check(min_filter == GL_LINEAR && mag_filter == GL_LINEAR,
            "the displayed texture uses ordinary bilinear scaling");
        check(gpu.fps() == 0, "source cadence starts without a previous frame");

        auto second = solid(Layout::Planar420, false, 0, 41, 240, 110, 1920, 1080);
        second.colors = YuvColors::Bt601;
        second.presentation_time = first.presentation_time + 1.0 / 24;
        second.pts = first.pts + 1.0 / 24; second.serial = first.serial;
        check(gpu.upload(second), "direct rendering accepts a consecutive source frame");
        check(gpu.texture() == direct_texture, "same-size frames reuse the conversion texture");
        near(read_presented(),0,0,255,2,"the latest frame replaces the previous color without blending");
        check(std::abs(gpu.fps() - 24) < 0.01, "per-picture presentation dates retain source cadence");
        check(gpu.upload(second), "a repeated display callback remains a valid picture");
        check(std::abs(gpu.fps() - 24) < 0.01, "repeated display callbacks do not reset source cadence");
        near(read_presented(),0,0,255,2,"repeated presentation retains the latest clean picture");

        first.serial++; first.pts = 90; first.presentation_time = second.presentation_time + 1.0 / 24;
        check(gpu.upload(first), "a seek generation accepts its current frame");
        check(gpu.fps() == 0, "a seek resets the diagnostic cadence");
        near(read_presented(),254,0,0,2,"the first frame after seek has no image from the previous generation");
        first.pts += 1.0 / 24; first.presentation_time += 1.0 / 24;
        check(gpu.upload(first), "the next frame after seek is accepted");
        check(std::abs(gpu.fps() - 24) < 0.01, "cadence recovers from the new seek generation");

        auto hdr_frame = solid(Layout::Planar420, true, 0, 64, 512, 512, 64, 48);
        hdr_frame.colors = YuvColors::Hdr10; hdr_frame.presentation_time = 30;
        check(gpu.upload(hdr_frame), "direct rendering accepts an HDR source");
        near(read_presented(),0,0,0,1,"direct HDR conversion remains black and uncorrupted");
        auto large_sdr = solid(Layout::Planar420, false, 0, 16, 128, 128, 2048, 1080);
        large_sdr.presentation_time = 40;
        check(gpu.upload(large_sdr), "large SDR source is accepted");
        near(read_presented(),0,0,0,1,"large SDR sources preserve direct rendering");

        auto metadata = std::make_shared<VideoDoviParams>();
        for (int c = 0; c < 3; ++c) { metadata->ycc[c][c] = 1; metadata->lms[c][c] = 1; }
        auto dovi = solid(Layout::Planar420,true,0,0,0,0);
        dovi.dovi = metadata; dovi.presentation_time = 50;
        check(gpu.upload(dovi), "Dolby Vision metadata reaches the GPU UBO");
        check(gpu.dolby_vision_active(), "renderer reports actual Dolby Vision reshaping");
        near(read_presented(),0,0,0,1,"Profile 5 GPU black stays finite and black");
        dovi = solid(Layout::Planar420,true,0,1023,1023,1023); dovi.dovi = metadata;
        check(gpu.upload(dovi), "Dolby Vision maximum signal is accepted");
        near(read_presented(),255,255,255,1,"Profile 5 GPU PQ maximum clamps to SDR white");
        dovi.dovi.reset(); check(gpu.upload(dovi), "ordinary frame follows Dolby Vision");
        check(!gpu.dolby_vision_active(), "stale Dolby Vision metadata is never reused");
        gpu.clear(); check(gpu.texture() == 0,"close clears the direct output");
        check(gpu.fps() == 0,"close resets diagnostic cadence");
    }
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context); eglTerminate(display);
    std::printf("GPU tests: %d checks, %d failures (host OpenGL; no console performance claim)\n",checks,failures);
    return failures ? 1 : 0;
}
