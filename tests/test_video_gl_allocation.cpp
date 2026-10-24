// Real host GL with injected exhausted texture allocations and a fragmented
// physical-allocation model. The model is not a PS5 hardware execution trace.
// SPDX-License-Identifier: GPL-3.0-or-later
#define main existing_video_gl_test_main
#include "test_video_gl.cpp"
#undef main
#include <map>
#include <utility>

namespace {
struct Range { size_t start, bytes; };
std::vector<Range> ranges;
std::map<GLuint, Range> reservations;
std::map<GLuint, GLuint> attachments;
std::vector<GLuint> generated;
std::vector<std::pair<GLenum,GLenum>> allocation_targets;
int allocation_index = 0, fail_at = 0, draws = 0, deletes = 0, binds = 0;
int deletes_while_attached = 0;
GLenum pending_error = GL_NO_ERROR;
GLenum framebuffer_failure = GL_NO_ERROR;
bool model = false;

size_t aligned(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

bool reserve(GLuint texture, GLenum target, GLenum format, unsigned width, unsigned height) {
    // Exact single-level native footprints from ps5-opengl dce74910:
    // The 2D RGBA8 output is tiled. Rectangle sampled planes remain linear,
    // but Mesa's advisory render-target bind also reserves render staging.
    // Those planes are never attached to a framebuffer or drawn into.
    size_t bytes = 0, alignment = 0;
    if (target == GL_TEXTURE_2D && format == GL_RGBA8) {
        bytes = aligned(size_t((width+127)/128)*((height+127)/128)*65536, 2097152);
        alignment = 2097152;
    } else if (target == GL_TEXTURE_RECTANGLE &&
               (format==GL_R8 || format==GL_RG8 || format==GL_R16 || format==GL_RG16 || format==GL_RGBA8)) {
        const size_t bpp=format==GL_R8?1:format==GL_R16||format==GL_RG8?2:4;
        const size_t tw=bpp<=2?256:128, th=bpp==1?256:128;
        const size_t linear=aligned(size_t(width)*bpp,256)*height;
        const size_t tiled=size_t((width+tw-1)/tw)*((height+th-1)/th)*65536;
        bytes=aligned(aligned(linear,65536)+tiled,65536); alignment=65536;
    } else { check(false,"fragmentation model only admits its audited formats"); return false; }
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto range = ranges[i];
        const auto start = aligned(range.start,alignment);
        if (start < range.start || start-range.start > range.bytes ||
            bytes > range.bytes-(start-range.start)) continue;
        ranges.erase(ranges.begin()+i);
        if (start>range.start) ranges.push_back({range.start,start-range.start});
        if (start+bytes<range.start+range.bytes)
            ranges.push_back({start+bytes,range.start+range.bytes-start-bytes});
        std::sort(ranges.begin(),ranges.end(),[](auto a,auto b){return a.start<b.start;});
        reservations[texture]={start,bytes}; return true;
    }
    return false;
}

bool fail_storage(GLenum target, GLenum format, unsigned width, unsigned height) {
    ++allocation_index;
    allocation_targets.emplace_back(target,format);
    bool fail = fail_at && allocation_index == fail_at;
    if (model) {
        GLint texture = 0;
        glGetIntegerv(target==GL_TEXTURE_RECTANGLE?GL_TEXTURE_BINDING_RECTANGLE:GL_TEXTURE_BINDING_2D,&texture);
        fail = !reserve(GLuint(texture),target,format,width,height);
    }
    if (fail) pending_error = GL_OUT_OF_MEMORY;
    return fail;
}

void drain_errors() { for(int i=0;i<16 && glGetError()!=GL_NO_ERROR;++i) {} }
std::vector<uint8_t> read_texture(VideoGl& gpu,int w,int h) {
    std::vector<uint8_t> out(size_t(w)*h*4);
    if(!gpu.texture()) return out;
    glBindTexture(GL_TEXTURE_2D,gpu.texture());
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,out.data());
    return out;
}
}

extern "C" {
void __real_glGenTextures(GLsizei,GLuint*);
void __real_glDeleteTextures(GLsizei,const GLuint*);
void __real_glBindFramebuffer(GLenum,GLuint);
void __real_glFramebufferTexture2D(GLenum,GLenum,GLenum,GLuint,GLint);
void __real_glDeleteFramebuffers(GLsizei,const GLuint*);
void __real_glTexStorage2D(GLenum,GLsizei,GLenum,GLsizei,GLsizei);
void __real_glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
GLenum __real_glGetError();
GLenum __real_glCheckFramebufferStatus(GLenum);
void __real_glDrawArrays(GLenum,GLint,GLsizei);
void __wrap_glGenTextures(GLsizei n,GLuint* names) {
    __real_glGenTextures(n,names);
    for(int i=0;i<n;++i) if(names[i]) generated.push_back(names[i]);
}
void __wrap_glBindFramebuffer(GLenum target,GLuint name) {
    ++binds; __real_glBindFramebuffer(target,name);
}
void __wrap_glFramebufferTexture2D(GLenum target,GLenum attachment,GLenum texture_target,GLuint texture,GLint level) {
    GLint fbo=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    if(attachment==GL_COLOR_ATTACHMENT0) attachments[GLuint(fbo)]=texture;
    __real_glFramebufferTexture2D(target,attachment,texture_target,texture,level);
}
void __wrap_glDeleteTextures(GLsizei n,const GLuint* names) {
    for(int i=0;i<n;++i) if(names[i]) {
        ++deletes;
        for(auto [fbo,attached]:attachments) if(attached==names[i]) ++deletes_while_attached;
        const auto it=reservations.find(names[i]);
        if(it!=reservations.end()) {ranges.push_back(it->second);reservations.erase(it);}
    }
    __real_glDeleteTextures(n,names);
}
void __wrap_glDeleteFramebuffers(GLsizei n,const GLuint* names) {
    for(int i=0;i<n;++i) attachments.erase(names[i]);
    __real_glDeleteFramebuffers(n,names);
}
void __wrap_glTexStorage2D(GLenum target,GLsizei levels,GLenum format,GLsizei width,GLsizei height) {
    if(!fail_storage(target,format,width,height)) __real_glTexStorage2D(target,levels,format,width,height);
}
void __wrap_glTexImage2D(GLenum target,GLint level,GLint internal,GLsizei width,GLsizei height,
                       GLint border,GLenum format,GLenum type,const void* data) {
    if(!fail_storage(target,GLenum(internal),width,height))
        __real_glTexImage2D(target,level,internal,width,height,border,format,type,data);
}
GLenum __wrap_glGetError() {
    if(pending_error!=GL_NO_ERROR) {auto error=pending_error;pending_error=GL_NO_ERROR;return error;}
    return __real_glGetError();
}
GLenum __wrap_glCheckFramebufferStatus(GLenum target) {
    const GLenum actual=__real_glCheckFramebufferStatus(target);
    return framebuffer_failure!=GL_NO_ERROR?framebuffer_failure:actual;
}
void __wrap_glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    ++draws; __real_glDrawArrays(mode,first,count);
}
}

int main() {
    auto get_display=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display=get_display(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    EGLint major=0,minor=0;
    if(!eglInitialize(display,&major,&minor)||!eglBindAPI(EGL_OPENGL_API)) return 2;
    const EGLint attrs[]={EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    EGLContext context=eglCreateContext(display,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,attrs);
    if(context==EGL_NO_CONTEXT||!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context))return 2;
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    for(int failed_allocation=1;failed_allocation<=4;++failed_allocation) {
        VideoGl gpu;check(gpu.init(),"failure test initializes real GPU shader");
        auto old=solid(Layout::Planar420,true,0,504,512,512,64,48);
        auto next=solid(Layout::Planar420,true,0,504,512,512,128,72);
        check(gpu.upload(old),"initial picture before failure is displayed");
        drain_errors();
        allocation_index=0;fail_at=failed_allocation;generated.clear();allocation_targets.clear();
        const int before_draws=draws,before_attached=deletes_while_attached;
        check(!gpu.upload(next),"every exhausted allocation rejects the incomplete picture");
        check(gpu.last_failure()==VideoGl::Failure::Allocation,"allocation failure is classified separately from bad frame data");
        check(draws==before_draws,"no GPU draw is submitted after any allocation failure");
        check(gpu.texture()==0,"failed reconfiguration exposes no incomplete output texture");
        check(!allocation_targets.empty() && allocation_targets[0]==std::pair<GLenum,GLenum>{GL_TEXTURE_2D,GL_RGBA8},
              "the large 2D render target is allocated before any input plane");
        check(std::all_of(allocation_targets.begin()+1,allocation_targets.end(),[](auto entry){return entry.first==GL_TEXTURE_RECTANGLE;}),
              "injected plane failures exercise rectangle storage, not stale 2D bindings");
        check(deletes_while_attached==before_attached,"reconfiguration detaches old FBO storage before deletion");
        for(auto texture:generated) check(!glIsTexture(texture),"failure releases every partial texture object");
        fail_at=0;drain_errors();
        check(gpu.upload(old),"return to the old dimensions recreates valid storage after failure");
        auto pixels=read_texture(gpu,old.w,old.h);
        check(glGetError()==GL_NO_ERROR,"recovery conversion/readback reports no GL error");
        near(pixels,128,128,128,1,"recovery image is complete at the requested resolution");
        generated.clear();
        const int before_delete=deletes;
        gpu.clear();
        check(deletes-before_delete==4,"stopping playback releases all four per-video textures");
        const int cleared_delete=deletes,cleared_bind=binds;
        for(int i=0;i<5;++i) gpu.clear();
        check(deletes==cleared_delete&&binds==cleared_bind,"subsequent empty clear calls perform no GL deletion or FBO bind");
        check(gpu.upload(old),"reopening after clear creates a full-resolution image");
        near(read_texture(gpu,old.w,old.h),128,128,128,1,"reopened image retains original pixel values");
        drain_errors();
    }
    {
        VideoGl gpu;check(gpu.init(),"framebuffer failure test initializes real GPU shader");
        auto f=solid(Layout::Planar420,false,0,126,128,128);
        generated.clear();framebuffer_failure=GL_FRAMEBUFFER_UNSUPPORTED;
        const int before_draws=draws;
        check(!gpu.upload(f),"unsupported framebuffer rejects the picture before drawing");
        check(gpu.last_failure()==VideoGl::Failure::Framebuffer,"framebuffer status without OOM is not labeled memory exhaustion");
        check(draws==before_draws,"unsupported framebuffer submits no video draw");
        for(auto texture:generated) check(!glIsTexture(texture),"framebuffer failure releases candidate storage");
        framebuffer_failure=GL_NO_ERROR;
        check(gpu.upload(f),"framebuffer failure leaves the next attempt able to allocate");
        auto invalid=f;invalid.pixels.resize(1);
        const auto retained=gpu.texture();
        check(!gpu.upload(invalid),"invalid plane bounds remain rejected");
        check(gpu.last_failure()==VideoGl::Failure::InvalidPlane,"descriptor failure is distinguished from GPU allocation");
        check(gpu.texture()==retained,"invalid descriptor retains the previous clean frame");
        check(glGetError()==GL_NO_ERROR,"framebuffer and descriptor rejection leave no GL error");
    }
    {
        VideoGl gpu;check(gpu.init(),"fragmentation test initializes real GPU shader");
        auto f=solid(Layout::Planar420,true,0,504,512,512,3840,2160);
        constexpr size_t MiB=1048576;
        ranges={{0,68*MiB},{80*MiB,9*MiB},{96*MiB,9*MiB}};
        reservations.clear();allocation_index=0;model=true;
        const bool accepted=gpu.upload(f);
        check(accepted,"largest surfaces fit a fragmented but sufficient native-size memory budget");
        size_t used=0;for(auto [texture,range]:reservations) used+=range.bytes;
        check(used==84738048,"native footprint includes each R16 staging allocation and the tiled target");
        if(accepted) near(read_texture(gpu,f.w,f.h),128,128,128,1,"fragmented-budget playback preserves real 4K pixels");
        check(glGetError()==GL_NO_ERROR,"fragmented-budget allocation has no GL error");
        gpu.clear();check(reservations.empty(),"playback stop releases the modeled native reservation");
        model=false;drain_errors();
    }
    {
        VideoGl gpu;check(gpu.init(),"stale-error test initializes real GPU shader");
        auto f=solid(Layout::Planar420,false,0,126,128,128);
        pending_error=GL_INVALID_ENUM;
        check(gpu.upload(f),"preexisting unrelated UI error does not reject a valid source frame");
        check(glGetError()==GL_NO_ERROR,"resource setup separates and consumes the prior UI error");
    }
    {
        VideoGl gpu;check(gpu.init(),"1080p rectangle budget test initializes real GPU shader");
        constexpr size_t MiB=1048576;
        // A 1080p SDR frame exercises the actual R8 path affected by CPU
        // tiling. Count the driver-reserved staging too, even though no input
        // rectangle is rendered into and upload maps its linear storage.
        auto f=solid(Layout::Planar420,false,0,126,128,128,1920,1080);
        ranges={{0,20*MiB}}; reservations.clear();allocation_targets.clear();
        allocation_index=0;model=true;
        check(gpu.upload(f),"1080p rectangle inputs and 2D output fit the audited native budget");
        size_t used=0;for(auto [texture,range]:reservations) used+=range.bytes;
        check(used==18087936,"1080p native accounting includes linear R8 planes plus their reserved staging");
        check(reservations.size()==4,"rectangle budget tracks four distinct texture allocations");
        if(gpu.texture()) near(read_texture(gpu,f.w,f.h),128,128,128,1,"budgeted rectangle upload preserves the 1080p image");
        gpu.clear();check(reservations.empty(),"rectangle input and 2D output reservations all release after stop");
        model=false;drain_errors();
    }
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context);eglTerminate(display);
    std::printf("GPU allocation tests: %d checks, %d failures (real host GL; injected API exhaustion and modeled native extents)\n",checks,failures);
    return failures?1:0;
}
