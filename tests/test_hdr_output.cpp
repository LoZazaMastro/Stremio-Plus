// Real EGL rendering, with only the Sony HDMI calls and exhausted GL storage mocked.
// SPDX-License-Identifier: GPL-3.0-or-later
#define main existing_video_gl_test_main
#include "test_video_gl.cpp"
#undef main
#define PLATFORM_PS5_NATIVE 1
#include "../src/hdr_out.cpp"

namespace {
int owned_handle = 77, status_calls = 0, switch_calls = 0;
int hdr_rc = 0, sdr_rc = 0;
unsigned dynamic_range = 2;
uint64_t submitted_format = 0;
bool fail_ui = false, fail_pq = false;
int rgb10_allocations = 0, plane_allocations = 0;
GLenum injected_error = GL_NO_ERROR;
}
extern "C" {
int stremio_videoout_handle() { return owned_handle; }
int32_t sceVideoOutGetOutputStatus(int32_t handle, void* value) {
    check(handle == owned_handle, "only the captured renderer-owned handle is queried");
    ++status_calls;
    auto* words = static_cast<uint32_t*>(value);
    words[1] = dynamic_range;
    return 0;
}
void sceVideoOutSetBufferAttribute2(void* attribute, uint64_t format, uint32_t, uint32_t, uint32_t,
                                    uint64_t, uint32_t, uint64_t) {
    std::memcpy(attribute, &format, sizeof(format));
}
int32_t sceVideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t index, const void* attribute, void*) {
    check(handle == owned_handle && index == 0, "only renderer buffer set zero is changed");
    std::memcpy(&submitted_format, attribute, sizeof(submitted_format));
    ++switch_calls;
    return submitted_format == UINT64_C(0x8100070422000000) ? hdr_rc : sdr_rc;
}
void __real_glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
GLenum __real_glGetError();
void __wrap_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                        GLint border, GLenum format, GLenum type, const void* data) {
    if (internal == GL_RGB10_A2) ++rgb10_allocations;
    if (target == GL_TEXTURE_RECTANGLE) ++plane_allocations;
    if ((fail_ui && internal == GL_RGBA8) || (fail_pq && target == GL_TEXTURE_RECTANGLE)) {
        fail_ui = fail_pq = false;
        injected_error = GL_OUT_OF_MEMORY;
        return;
    }
    __real_glTexImage2D(target, level, internal, w, h, border, format, type, data);
}
GLenum __wrap_glGetError() {
    if (injected_error != GL_NO_ERROR) {
        const auto error = injected_error; injected_error = GL_NO_ERROR; return error;
    }
    return __real_glGetError();
}
}

double decode_pq(double v) {
    const double p = std::pow(v, 1.0 / 78.84375);
    return 10000 * std::pow(std::max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875*p), 1.0 / 0.1593017578125);
}
uint32_t packed_pixel(GLuint fbo, int x, int y) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    uint8_t bytes[4]{};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
    return uint32_t(bytes[2]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[0]) << 16 | uint32_t(bytes[3]) << 24;
}

int main() {
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) return 2;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    EGLContext context = eglCreateContext(display,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) return 2;
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    {
        VideoGl video;
        hui::gfx::Renderer renderer;
        hui::gfx::Canvas target;
        check(video.init() && renderer.init() && target.create(64,36,1), "real EGL video/UI/final targets initialize");
        auto frame = solid(Layout::Nv12,true,0,723,512,512,64,36);
        frame.colors = YuvColors::Hdr10;
        hdr_out::init(64,36);
        check(hdr_out::prepare(true), "HDR resources prepare on an HDR display");
        check(switch_calls == 0 && !hdr_out::scanout_hdr(), "preparation never changes scanout before the video is ready");
        video.set_hdr_output(true);
        check(video.upload(frame) && video.hdr_planes_active(), "retained source uploads as direct HDR planes");
        check(video.has_frame() && video.texture()==0, "direct HDR exposes a valid picture without allocating an RGB intermediate");
        check(rgb10_allocations==0 && plane_allocations>0, "HDR never requests RGB10_A2 native storage");
        const int expected = int(std::lround((723.0-64)/876*1023));
        check(hdr_out::update(true), "scanout commits after successful plane upload");
        renderer.begin();
        check(hdr_out::present(renderer,64,36,0,{0,0,1920,1080},target.framebuffer(),true,&video.hdr_planes()), "real direct-plane HDR final shader draws");
        const auto direct = packed_pixel(target.framebuffer(),32,18);
        for(int c=0;c<3;++c)
            check(std::abs(int((direct>>(10*c))&1023)-expected)<=1,
                  "packed direct-plane output preserves independent ten-bit PQ reference");
        const double yy=(450.0-64)/876, cb=(480.0-512)/896, cr=(550.0-512)/896;
        const std::array<double,3> reference{yy+1.4746*cr, yy-.16455*cb-.57135*cr, yy+1.8814*cb};
        for(const auto layout:{Layout::Nv12,Layout::Planar420}) for(const int shift:{0,6}) {
            // Padded odd source extents exercise both three-plane software
            // output and the hardware decoder's interleaved low/high bits.
            auto colored=solid(layout,true,shift,450,480,550,9,7);
            colored.colors=YuvColors::Hdr10;
            check(video.upload(colored), "colored HDR planes upload in both layouts and alignments");
            renderer.begin();
            check(hdr_out::present(renderer,64,36,0,{0,0,1920,1080},target.framebuffer(),true,&video.hdr_planes()), "colored direct-plane output renders");
            const auto packed=packed_pixel(target.framebuffer(),32,18);
            for(int c=0;c<3;++c)
                check(std::abs(int((packed>>(10*c))&1023)-int(std::lround(reference[c]*1023)))<=1,
                      "direct HDR conversion matches independent distinct RGB channel reference");
            check(glGetError()==GL_NO_ERROR, "odd direct HDR layouts and alignment leave no GL error");
        }
        auto hlg=solid(Layout::Nv12,true,0,940,512,512,64,36);
        hlg.hlg=true;
        check(video.upload(hlg), "HLG source uploads without intermediate conversion");
        renderer.begin();
        check(hdr_out::present(renderer,64,36,0,{0,0,1920,1080},target.framebuffer(),true,&video.hdr_planes()), "HLG direct final pass renders");
        const auto hlg_pixel=packed_pixel(target.framebuffer(),32,18);
        check(std::abs(decode_pq(double(hlg_pixel&1023)/1023)-1000)<8,
              "direct HLG full signal maps to independent 1000-nit PQ reference");
        check(video.upload(frame), "neutral reference restored before overlay checks");
        hui::gfx::DrawList overlay;
        overlay.rounded_rect({0,0,1920,1080},0,hui::gfx::Color::rgb(0,.5f));
        renderer.begin(); renderer.draw(overlay);
        check(hdr_out::present(renderer,64,36,video.texture(),{0,0,1920,1080},target.framebuffer(),true,&video.hdr_planes()), "HDR overlay renders");
        const auto covered = packed_pixel(target.framebuffer(),32,18);
        const double light = decode_pq(double(covered&1023)/1023);
        const double original_light = decode_pq(double(direct&1023)/1023);
        check(std::abs(light - original_light*.5) < 12, "50% black overlay retains half the linear light, not half the PQ code");
        const auto glass = hdr_out::glass_texture(video.texture(),true,&video.hdr_planes());
        glBindTexture(GL_TEXTURE_2D,glass);
        GLint width=0,height=0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
        check(glass && width==480 && height==270, "glass uses a small tone-mapped copy without a full-size SDR surface");
        hui::gfx::DrawList glass_bg, glass_panel;
        glass_bg.image(glass,{0,0,1920,1080},hui::gfx::kFullUv,hui::gfx::Color::rgb(0xffffff));
        renderer.begin(); renderer.glass_background(&glass_bg); renderer.glass(); renderer.draw(glass_panel);
        check(hdr_out::present(renderer,64,36,video.texture(),{0,0,1920,1080},target.framebuffer(),true,&video.hdr_planes()), "glass-only movie background renders");
        glBindTexture(GL_TEXTURE_2D,renderer.glass_texture());
        std::vector<uint8_t> glass_pixels(480*270*4);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,glass_pixels.data());
        check(glass_pixels[4*(135*480+240)]>0, "glass captures the movie instead of an empty black background");
        hdr_out::set_enabled(false);
        check(!hdr_out::prepare(true), "HDR off requests SDR with the same retained paused frame");
        video.set_hdr_output(false);
        check(video.upload(frame) && !video.hdr_planes_active(), "paused retained frame reconverts before SDR commit");
        check(!hdr_out::update(false), "SDR commit follows the retained-frame conversion");
        hdr_out::set_enabled(true);
        check(hdr_out::prepare(true), "paused frame can prepare HDR again without decoder input");
        video.set_hdr_output(true); check(video.upload(frame), "paused frame converts to PQ again");
        hdr_rc=-7;
        check(!hdr_out::update(true), "refused HDR leaves scanout SDR");
        video.set_hdr_output(false); check(video.upload(frame) && !video.hdr_planes_active(), "same frame falls back SDR after native refusal");
        check(!hdr_out::update(true), "second native refusal stays SDR");
        const int refused=switch_calls;
        for(int i=0;i<5;++i) { check(!hdr_out::prepare(true), "refused HDR is bounded"); hdr_out::update(true); }
        check(switch_calls==refused, "no unbounded native HDR retry loop");
        hdr_out::release(); hdr_rc=0;
        hdr_out::init(64,36);
        fail_ui=true;
        const int before_prepare=switch_calls;
        check(!hdr_out::prepare(true), "UI allocation exhaustion fails during preparation");
        check(switch_calls==before_prepare && !hdr_out::scanout_hdr(), "UI allocation exhaustion never changes display encoding");
        check(!hdr_out::prepare(true), "failed HDR resource setup remains disabled for this session");
        hdr_out::release();
        hdr_out::init(64,36);
        check(hdr_out::prepare(true), "fresh session prepares after prior failure");
        video.set_hdr_output(true); fail_pq=true;
        check(!video.upload(frame) && video.last_failure()==VideoGl::Failure::Allocation, "source-plane allocation exhaustion is identified");
        hdr_out::failed("test: source-plane allocation failed");
        video.set_hdr_output(false);
        check(video.upload(frame) && !video.hdr_planes_active(), "same retained frame successfully retries SDR after plane allocation failure");
        check(!hdr_out::update(false), "failed source-plane allocation retains SDR scanout");
        hdr_out::release();
        hdr_out::init(64,36);
        check(hdr_out::prepare(true), "restoration failure fixture prepares");
        check(hdr_out::update(true), "restoration failure fixture enters HDR");
        sdr_rc=-8;
        check(hdr_out::update(false) && hdr_out::safe(), "first failed restoration retains HDR encoding and allows one retry");
        renderer.begin();
        check(hdr_out::present(renderer,64,36,video.texture(),{0,0,1920,1080},target.framebuffer(),false), "SDR texture is safely recoded if native restoration is pending");
        check(hdr_out::update(false) && !hdr_out::safe(), "second failed restoration stops the session");
        const int restored=switch_calls;
        for(int i=0;i<10;++i) hdr_out::update(false);
        check(switch_calls==restored, "SDR restoration retries stop after two failures");
        hdr_out::release(); sdr_rc=0;
        hdr_out::init(64,36); dynamic_range=1;
        check(!hdr_out::prepare(true), "SDR display retains tone-mapped output");
        hdr_out::release(); dynamic_range=2; owned_handle=-1;
        hdr_out::init(64,36);
        check(!hdr_out::prepare(true), "missing captured handle never probes invented handles");
        hdr_out::release();
        check(glGetError()==GL_NO_ERROR, "HDR preparation, rendering, failure and restoration leave no GL errors");
    }
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context); eglTerminate(display);
    std::printf("HDR tests: %d checks, %d failures (real host GL; mocked Sony HDMI calls)\n",checks,failures);
    return failures ? 1 : 0;
}
