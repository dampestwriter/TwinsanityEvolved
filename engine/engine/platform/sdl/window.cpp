// platform/sdl/window.cpp -- Window implementation.
#include "engine/platform/sdl/window.h"

#include <SDL.h>

#include <cstdio>

#include "engine/platform/host/platform.h"
#include "engine/platform/sdl/gl_context.h"
#if PS2R_HOST_VITA
#include PS2R_GL_HEADER
#endif

namespace ps2r {
namespace platform {

Window::~Window() { Destroy(); }

bool Window::Create(bool headless, int w, int h, const char* title) {
    Destroy();
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[sdl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
            return false;
        }
        video_inited_ = true;
    }
    // SDL_VideoInit resets the GL attributes: request the 4.1 core profile AFTER the
    // video subsystem exists and BEFORE the window is created.
    GlContext::RequestAttributes();
    headless_ = headless;
#if PS2R_HOST_VITA
    // SDL's Vita video driver has no OpenGL: a plain window (the screen) for the Vita's own draw arm. It is the whole
    // 960x544 screen vitaGL draws to -- the present takes its size as the drawable size.
    (void)w; (void)h;
    const int cw = 960;
    const int ch = 544;
    Uint32 flags = 0;
#else
    const int cw = headless ? 64 : (w > 0 ? w : 640);
    const int ch = headless ? 64 : (h > 0 ? h : 448);
    Uint32 flags = SDL_WINDOW_OPENGL;
#endif
    flags |= headless ? SDL_WINDOW_HIDDEN : (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    window_ = SDL_CreateWindow(title ? title : "NativeEE", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, cw, ch, flags);
    if (!window_) {
        // The requested 32-bit window depth has no GLX visual on Mesa / Xvfb
        // (24+8 is the deepest); the window's own depth buffer is unused -- every
        // GS target is an FBO with its own depth attachment -- so retry at 24.
        // macOS grants 32 on the first attempt and never reaches this branch.
        std::fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s -- retrying with a 24-bit depth visual\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        window_ = SDL_CreateWindow(title ? title : "NativeEE", SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, cw, ch, flags);
    }
    if (!window_) {
        std::fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GetWindowSize(window_, &width_, &height_);
    return true;
}

void Window::Destroy() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (video_inited_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        video_inited_ = false;
    }
    width_ = height_ = 0;
}

void Window::Resize(int w, int h) {
    if (!window_ || w <= 0 || h <= 0) return;
    SDL_SetWindowSize(window_, w, h);
    SDL_GetWindowSize(window_, &width_, &height_);
}

void Window::Swap() {
#if PS2R_HOST_VITA
    vglSwapBuffers(GL_FALSE);
#else
    if (window_) SDL_GL_SwapWindow(window_);
#endif
}

}  // namespace platform
}  // namespace ps2r
