// platform/sdl/gl_context.cpp -- GlContext implementation.
#include "engine/platform/sdl/gl_context.h"

#include <SDL.h>
#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstdio>

#include "engine/platform/sdl/window.h"

namespace ps2r {
namespace platform {

GlContext::~GlContext() { Destroy(); }

void GlContext::RequestAttributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#ifdef _WIN32
    // 24, not 32, on Windows. The accelerated ICDs (nvoglv64 / igxelpicd64)
    // advertise 24-bit depth with 8 stencil and no 32-bit depth format at all,
    // while the Microsoft GDI Generic software renderer DOES offer 32 -- so
    // asking for 32 does not fail, it silently selects the software format and
    // the context comes back as "GDI Generic" OpenGL 1.1, where none of the 4.1
    // core entry points exist. (The 24-bit retry in platform/sdl/window.cpp only
    // fires when SDL_CreateWindow fails outright, which this does not.)
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#else
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 32);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

bool GlContext::Create(Window& window) {
    Destroy();
    if (!window.Handle()) return false;
#if PS2R_HOST_VITA
    // The result is only whether the requested size fell back to the display's maximum (vitaGL vgl.c), not success.
    // No MSAA: the frame target and every PS2 target are plain single-sampled framebuffers.
    const GLboolean resolution_fallback = vglInitExtended(0, 960, 544, 32 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    std::fprintf(stderr, "[gl] vitaGL ready (resolution fallback: %s)\n", resolution_fallback ? "yes" : "no");
    std::fflush(stderr);
    // compile each shader in glCompileShader (a vertex shader then its fragment shader, as GlShaderLibrary builds them),
    // so a shader Sony's compiler refuses is reported there instead of failing inside glLinkProgram
    vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
    // the runtime shader compiler's messages (the GLSL translation and Sony's Cg compiler) into the log
    shark_install_log_cb([](const char* msg, shark_log_level level, int line) {
        std::fprintf(stderr, "[gl] shader compiler %s (line %d): %s\n",
                     level == SHARK_LOG_ERROR ? "error" : level == SHARK_LOG_WARNING ? "warning" : "info", line, msg ? msg : "");
        std::fflush(stderr);
    });
    vita_initialized_ = true;
    return true;
#endif
    context_ = SDL_GL_CreateContext(window.Handle());
    if (!context_) {
        std::fprintf(stderr, "[gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    if (!MakeCurrent(window)) return false;
#if defined(_WIN32)
    // The GL entry points are function pointers on Windows (engine/platform/windows/gl_loader.h): fill them from this
    // context's driver before the first GL call below. A driver without one of the 4.1-core names the engine uses
    // cannot run the backend: refuse here with the names rather than crash on the first null call.
    const char* missing[8];
    const int n_missing = ps2r::glwin::Load(&SDL_GL_GetProcAddress, missing, 8);
    if (n_missing) {
        std::fprintf(stderr, "[gl] %d of %d GL entry points are missing from this driver (first:", n_missing, ps2r::glwin::Count());
        for (int i = 0; i < n_missing && i < 8; ++i) std::fprintf(stderr, " %s", missing[i]);
        std::fprintf(stderr, ") -- an OpenGL 4.1 core driver is required\n");
        Destroy();
        return false;
    }
#endif
    SetSwapInterval(0);
#if !PS2R_HOST_VITA
    const GLubyte* v = glGetString(GL_VERSION);
    const GLubyte* r = glGetString(GL_RENDERER);
    version_ = v ? reinterpret_cast<const char*>(v) : "";
    renderer_ = r ? reinterpret_cast<const char*>(r) : "";
#endif
    return true;
}

void GlContext::Destroy() {
#if PS2R_HOST_VITA
    vita_initialized_ = false;
#else
    if (context_) {
        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(context_));
        context_ = nullptr;
    }
#endif
}

bool GlContext::MakeCurrent(Window& window) {
#if PS2R_HOST_VITA
    (void)window;
    return vita_initialized_;
#else
    if (!context_ || !window.Handle()) return false;
    if (SDL_GL_MakeCurrent(window.Handle(), static_cast<SDL_GLContext>(context_)) != 0) {
        std::fprintf(stderr, "[gl] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
#endif
}

void GlContext::SetSwapInterval(int interval) {
#if PS2R_HOST_VITA
    (void)interval;
#else
    SDL_GL_SetSwapInterval(interval);
#endif
}

}  // namespace platform
}  // namespace ps2r
