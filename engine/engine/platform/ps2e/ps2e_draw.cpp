// engine/platform/ps2e/ps2e_draw.cpp -- ps2e_draw over Window + the render::Backend the arm supplies.
//
// The surface names no graphics API: the backend behind it is whatever ps2e_draw_backend.h's selected arm creates
// (ps2e_draw_backend_gl.cpp: a GL 4.1 context on the window + GlDevice; ps2e_draw_backend_null.cpp: nothing
// rasterised), and every operation here -- surfaces, uploads, draws, the shadow-volume stencil counter, the frame
// copy, the scanout, the presented-image capture -- is an entry of engine/render/backend.h.
//
// Each submitted draw becomes ONE DrawIr (the record the backend already lowers) whose raster state is the frame
// target's (FRAME at 0, the frame's width, PSMCT32; ZBUF a Z32 buffer; SCISSOR the frame or the state's) and whose
// vertices are the producer's triangle list in frame pixels (DrawIr vertices are post-XYOFFSET pixels, so off_x/off_y
// are 0). A textured draw samples a Decoded, owned TextureSource (the texture the producer created) in STQ mode with
// the texel-space prescale set to the texture's size, so u/v fractions reach the shader as the GS's S/T would.
#include "engine/platform/ps2e/ps2e_draw.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine/gs/format/pixel_convert.h"
#include "engine/platform/host/platform.h"
#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/platform/ps2e/ps2e_draw_backend.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backend.h"

using namespace ps2r;

struct ps2e_draw {
    platform::Window window;
    platform::GlContext context;             // created by the arm that needs one (ps2e_draw_backend_gl.cpp); else uncreated
    std::unique_ptr<render::Backend> device;
    render::SurfaceId color, depth;
    render::CdCopyState cd_copy;
    int frame_w = 0, frame_h = 0;         // the LOGICAL frame (the title's pixels); physical = logical * scale
    int scale = 1;                        // the frame target's internal scale (ps2e_draw_presentation.scale)
    int drawable_w = 0, drawable_h = 0;   // the window's drawable size as the pump last saw it (the window's thread)
    // How the frame is shown. `wanted` is what the producer last set (any thread, under the mutex); the pump
    // applies the window half (mode, size) on the window's thread, the present the rest on the drawing thread.
    std::mutex presentation_mutex;
    ps2e_draw_presentation wanted{};    // all zero = the plain picture (the frame's pixels letterboxed, linear, windowed, scale 1)
    ps2e_draw_presentation applied{};
    int applied_window_mode = PS2E_DRAW_WINDOWED, applied_window_scale = 0;   // the pump's half
    int last_out_x = 0, last_out_y = 0, last_out_w = 0, last_out_h = 0;        // the last present's output rectangle
    bool headless = false;
    bool stencil_diag = false;   // ps2e_draw_config.stencil_diag
    bool in_frame = false;
    bool capture = false;
    bool present_ct16 = false;   // ps2e_draw_set_present_ct16
    uint64_t dimx = render::kDefaultDimx;
    uint64_t presented = 0;
    uint32_t next_texture = 1;
    struct Texture {
        render::SurfaceId surface;
        int w = 0, h = 0;
        int levels = 0;   // the levels below the first (ps2e_draw_texture_set_mips)
    };
    std::unordered_map<uint32_t, Texture> textures;
    // The screen copy (ps2e_draw_copy_frame): a texture of the frame target's physical size, re-made with it; valid
    // once a copy has been taken this frame (PS2E_DRAW_TEXTURE_FRAME_COPY draws before that are refused)
    Texture frame_copy;
    bool frame_copy_valid = false;
    ps2e_draw_counters counters{};
    std::vector<uint8_t> scratch;
    gs::DrawIr ir;   // the submit's record, kept so its vertex storage is reused across draws (no allocation per draw)
    // the last mesh draw's state while the next draw may be another mesh under it (the device's DrawMeshAgain): valid
    // while no other draw came between (the draw count it left)
    bool mesh_run = false;
    ps2e_draw_state mesh_run_state{};
    uint64_t mesh_run_draws = 0;
};

namespace {

constexpr uint8_t kStencilOrigin = 15;
constexpr uint8_t kCasterBit = 0x20;   // the stencil bit above the 5-bit counter: a caster pixel (SeedCasterMask)
constexpr uint8_t kCounterMask = 0x1F; // the 5-bit counter, wrapping as the GS byte did
constexpr uint8_t kInsideBit = 0x10;   // bit 4 set <=> the count is 1..16

// The stencil test around one counting or composite draw (the GS tested the volumes' NEGATED z against the scene
// depth's byte-NOT copy -- a fragment counts where the volume face is AT OR BEHIND the scene surface, i.e. where the
// ordinary depth test FAILS (the volume's z against the frame's depth, GEQUAL); the composite reads bit 4).
render::StencilState VolumeStencil(const ps2e_draw_state* st) {
    render::StencilState s;
    s.enabled = true;
    if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) {
        s.func = render::StencilState::kEqual;
        s.ref = kInsideBit;
        s.cmp_mask = kInsideBit;
        s.write_mask = 0;
        return s;
    }
    // dest_alpha_test: the pixels the clear marked as casters fail the test and keep their count
    if (st->dest_alpha_test) {
        s.func = render::StencilState::kEqual;
        s.ref = 0;
        s.cmp_mask = kCasterBit;
    } else {
        s.func = render::StencilState::kAlways;
        s.ref = 0;
        s.cmp_mask = 0xFF;
    }
    s.write_mask = kCounterMask;
    s.depth_fail_op = st->stencil == PS2E_DRAW_STENCIL_COUNT_UP ? render::StencilState::kIncrWrap
                                                                 : render::StencilState::kDecrWrap;
    return s;
}

// ps2e_draw_config.stencil_diag: the stencil's content read back at the shadow bracket's points (the clear, the
// composite) -- a histogram of the counter's values and the share with bit 4 set (the composite's test). The consumer
// decides how the toggle is set (the port: PS2E_DRAW_STENCIL_DIAG); the engine reads no environment outside the config
// loader (CoreSingleGetenvSite). A backend that cannot read its stencil reports that and nothing else.
void StencilProbe(ps2e_draw* d, const char* tag) {
    if (!d->stencil_diag) return;
    const int w = d->frame_w * d->scale, h = d->frame_h * d->scale;
    static std::vector<uint8_t> buf;
    buf.assign(size_t(w) * size_t(h), 0xEE);
    if (!d->device->ReadbackStencil(d->depth, render::Rect{0, 0, w, h}, buf.data())) {
        std::fprintf(stderr, "[ps2e.stencil] present %llu %s: no stencil readback\n", (unsigned long long)d->presented, tag);
        return;
    }
    size_t hist[256] = {}, hist0[256] = {};
    for (uint8_t v : buf) hist[v]++;
    std::memcpy(hist0, hist, sizeof hist);
    size_t inside = 0;
    for (int v = 0; v < 256; ++v) if (v & 0x10) inside += hist[v];
    std::fprintf(stderr, "[ps2e.stencil] present %llu %s: inside %.3f origin %.3f top:", (unsigned long long)d->presented, tag,
                 double(inside) / buf.size(), double(hist[kStencilOrigin]) / buf.size());
    for (int n = 0; n < 4; ++n) {
        int best = -1;
        for (int v = 0; v < 256; ++v) if (hist[v] && (best < 0 || hist[v] > hist[best])) best = v;
        if (best < 0) break;
        std::fprintf(stderr, " %d:%.3f", best, double(hist[best]) / buf.size());
        hist[best] = 0;
    }
    size_t masked = 0;
    for (int v = 0; v < 256; ++v) if (v & kCasterBit) masked += hist0[v];
    std::fprintf(stderr, " casters %.3f\n", double(masked) / buf.size());
    // The frame's alpha (a caster is a pixel whose PS2 alpha is >= 0x80 -- the GPU's 0xFF after the x2 scale): the
    // share of such pixels per 64-row band (top row first) and the most common alpha bytes
    static std::vector<uint8_t> px;
    px.assign(size_t(w) * size_t(h) * 4, 0);
    d->device->Readback(d->color, render::Rect{0, 0, w, h}, px.data(), size_t(w) * 4);
    size_t ahist[256] = {};
    for (size_t i = 3; i < px.size(); i += 4) ahist[px[i]]++;
    std::fprintf(stderr, "[ps2e.stencil] present %llu %s: frame alpha 0xff per band", (unsigned long long)d->presented, tag);
    for (int band = 0; band < h; band += 64) {
        size_t bandSet = 0, n = 0;
        for (int y = band; y < std::min(h, band + 64); ++y)
            for (int x = 0; x < w; ++x) {
                bandSet += px[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] == 0xFF ? 1 : 0;   // rows bottom-up: a share per band either way
                n++;
            }
        std::fprintf(stderr, " %.2f", n ? double(bandSet) / n : 0.0);
    }
    std::fprintf(stderr, " (all %.3f) alpha top:", double(ahist[0xFF]) / (size_t(w) * size_t(h)));
    for (int n = 0; n < 6; ++n) {
        int best = -1;
        for (int v = 0; v < 256; ++v) if (ahist[v] && (best < 0 || ahist[v] > ahist[best])) best = v;
        if (best < 0) break;
        std::fprintf(stderr, " %#x:%.3f", best, double(ahist[best]) / (size_t(w) * size_t(h)));
        ahist[best] = 0;
    }
    std::fprintf(stderr, "\n");
}

// StartShadows' SeedAlpha: the frame's alpha, shrunk into the half-size buffer, was the volume strips' DATE
// source -- a strip counted only where the frame's alpha top bit was 0, so the casters (Crash's skin: alpha 0x80, or a
// material with FBA) were never darkened while the receivers (the lit world: alpha 0x7E) were. The frame's alpha here is
// the backend's GPU-scaled byte (PS2 0x80 -> 0xFF, 0x7E -> 0xFB), so the bit is `alpha == 0xFF`; the backend marks it
// as stencil bit kCasterBit over the whole frame right after the counter's clear (Backend::MarkStencilWhereOpaque), and
// a counting draw with dest_alpha_test skips the pixels that carry it (TEST.DATE with DATM 0).
void SeedCasterMask(ps2e_draw* d) { d->device->MarkStencilWhereOpaque(d->color, d->depth, kCasterBit); }

bool MakeFrameTarget(ps2e_draw& d, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    const int scale = std::clamp(d.scale, 1, 4);
    if (uint64_t(w) * scale > 8192 || uint64_t(h) * scale > 8192) return false;
    if (d.color.id) d.device->DestroySurface(d.color);
    if (d.depth.id) d.device->DestroySurface(d.depth);
    render::SurfaceDesc cd;
    cd.width = uint32_t(w * scale);     // physical texels (the backend rasterises through the viewport at the scale)
    cd.height = uint32_t(h * scale);
    cd.scale = uint8_t(scale);
    cd.format = render::SurfaceFormat::Rgba8;
    cd.renderable = true;
    // Depth + stencil in one attachment: the stencil is the shadow volumes' counter (ps2e_draw_stencil); GL binds a
    // depth-only texture and a stencil renderbuffer to one framebuffer only as the same image.
    render::SurfaceDesc dd = cd;
    dd.format = render::SurfaceFormat::DepthStencil24_8;
    dd.renderable = false;
    d.color = d.device->CreateSurface(cd);
    d.depth = d.device->CreateSurface(dd);
    if (d.frame_copy.surface.id) d.device->DestroySurface(d.frame_copy.surface);
    render::SurfaceDesc fc = cd;
    fc.renderable = false;
    d.frame_copy = ps2e_draw::Texture{d.device->CreateSurface(fc), int(cd.width), int(cd.height)};
    if (d.frame_copy.surface.id) {   // the storage specified whole (a pooled texture object may hold another size)
        std::vector<uint8_t> zero(size_t(cd.width) * cd.height * 4, 0);
        d.device->Upload(d.frame_copy.surface, render::Rect{0, 0, int(cd.width), int(cd.height)}, zero.data(), size_t(cd.width) * 4);
    }
    d.frame_copy_valid = false;
    d.cd_copy = render::CdCopyState{};
    d.frame_w = w;
    d.frame_h = h;
    d.scale = scale;
    if (!d.color.id || !d.depth.id) return false;
    d.device->Clear(d.color, d.depth, 0x80000000u, 0.0f, render::kClearColor | render::kClearDepth);
    return true;
}

// PS2 alpha bytes -> the GPU alpha the backend's samplers expect (the decoder's one scaling site).
void ScaleAlpha(const uint8_t* in, size_t stride_in, int w, int h, std::vector<uint8_t>& out) {
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + size_t(y) * stride_in;
        uint8_t* dst = out.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 0];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 2];
            dst[x * 4 + 3] = gs::PixelConvert::ScaleAlphaPs2ToGpu(row[x * 4 + 3]);
        }
    }
}

}  // namespace

extern "C" {

ps2e_draw* ps2e_draw_create(const ps2e_draw_config* config) {
    if (!config || config->frame_width <= 0 || config->frame_height <= 0) return nullptr;
    auto d = std::make_unique<ps2e_draw>();
    d->headless = config->headless != 0;
    d->stencil_diag = config->stencil_diag != 0;
    const int ww = config->window_width > 0 ? config->window_width : config->frame_width;
    const int wh = config->window_height > 0 ? config->window_height : config->frame_height;
    if (!d->window.Create(d->headless, ww, wh, config->title ? config->title : "ps2e_draw")) return nullptr;
    d->device = ps2e::CreateDrawBackend(d->window, d->context);
    if (!d->device) return nullptr;
    // The device's window size is the frame's when headless (the hidden window is 64x64: the capture must be the
    // frame's size so a judge sees the frame, not a thumbnail).
    d->device->SetWindowSize(d->headless ? config->frame_width : d->window.Width(),
                             d->headless ? config->frame_height : d->window.Height());
    if (!MakeFrameTarget(*d, config->frame_width, config->frame_height)) return nullptr;
    if (!d->headless) SDL_GL_GetDrawableSize(d->window.Handle(), &d->drawable_w, &d->drawable_h);   // the window's thread
    return d.release();
}

void ps2e_draw_destroy(ps2e_draw* d) {
    if (!d) return;
    if (d->device) {
        for (auto& t : d->textures) d->device->DestroySurface(t.second.surface);
        if (d->frame_copy.surface.id) d->device->DestroySurface(d->frame_copy.surface);
        if (d->color.id) d->device->DestroySurface(d->color);
        if (d->depth.id) d->device->DestroySurface(d->depth);
        d->device.reset();   // the arm's device releases its objects (the context is still current)
    }
    d->context.Destroy();
    d->window.Destroy();
    delete d;
}

int ps2e_draw_pump(ps2e_draw* d) {
    if (!d) return 0;
    int quit = 0;
    // The window's half of the presentation (the window's thread): the mode and the windowed size
    if (!d->headless) {
        int mode, wscale;
        {
            std::lock_guard<std::mutex> lock(d->presentation_mutex);
            mode = d->wanted.window_mode;
            wscale = d->wanted.window_scale;
        }
        if (mode != d->applied_window_mode) {
            const Uint32 flags = mode == PS2E_DRAW_FULLSCREEN ? SDL_WINDOW_FULLSCREEN_DESKTOP : mode == PS2E_DRAW_BORDERLESS ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
            if (SDL_SetWindowFullscreen(d->window.Handle(), flags) == 0) {
                SDL_SetWindowBordered(d->window.Handle(), mode == PS2E_DRAW_BORDERLESS ? SDL_FALSE : SDL_TRUE);
                d->applied_window_mode = mode;
            } else {
                d->applied_window_mode = mode;   // refused by the window system: not retried every pump
            }
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
            if (w > 0 && h > 0) { d->drawable_w = w; d->drawable_h = h; }
        }
        if (wscale > 0 && wscale != d->applied_window_scale) {
            d->applied_window_scale = wscale;
            if (d->applied_window_mode == PS2E_DRAW_WINDOWED) {
                const int ws = std::clamp(wscale, 1, 8);
                // the window sized for the picture at the frame's height times the scale: the TV's 4:3 for a TV shape,
                // the frame's own size for the pixel shape, 16:9 / 16:10 / 21:9 as chosen
                int aspect_mode;
                { std::lock_guard<std::mutex> lock(d->presentation_mutex); aspect_mode = d->wanted.aspect; }
                double aspect = double(d->frame_w) / d->frame_h;
                if (aspect_mode == PS2E_DRAW_ASPECT_4_3) aspect = 4.0 / 3.0;
                else if (aspect_mode == PS2E_DRAW_ASPECT_16_9) aspect = 16.0 / 9.0;
                else if (aspect_mode == PS2E_DRAW_ASPECT_16_10) aspect = 16.0 / 10.0;
                else if (aspect_mode == PS2E_DRAW_ASPECT_21_9) aspect = 21.0 / 9.0;
                d->window.Resize(int(d->frame_h * ws * aspect + 0.5), d->frame_h * ws);
                int w = 0, h = 0;
                SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
                if (w > 0 && h > 0) { d->drawable_w = w; d->drawable_h = h; }
            }
        }
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = 1;
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
                int w = 0, h = 0;
                SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
                if (w > 0 && h > 0 && !d->headless) {
                    d->drawable_w = w;
                    d->drawable_h = h;
                }
            }
        }
    }
    // the size asked every pump as well: a maximize / restore / mode change whose event came before the window settled
    // (or never came) must not leave the picture laid out for the old size, anchored at the drawable's bottom-left
    if (!d->headless) {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(d->window.Handle(), &w, &h);
        if (w > 0 && h > 0) {
            d->drawable_w = w;
            d->drawable_h = h;
        }
    }
    return quit;
}

int ps2e_draw_bind_thread(ps2e_draw* d, int bind) {
    if (!d || !d->context.IsCreated()) return 0;
#if PS2R_HOST_VITA
    // vitaGL has one context and no per-thread binding (SDL's Vita video has no GL to hand over): the frames may be drawn
    // on another thread as they are, one thread at a time -- the render thread's hand-over keeps it so
    (void)bind;
    return 1;
#endif
    if (bind) return d->context.MakeCurrent(d->window) ? 1 : 0;
    return SDL_GL_MakeCurrent(d->window.Handle(), nullptr) == 0 ? 1 : 0;
}

int ps2e_draw_resize_frame(ps2e_draw* d, int w, int h) {
    if (!d || d->in_frame) return 0;
    if (w == d->frame_w && h == d->frame_h) return 1;
    const bool ok = MakeFrameTarget(*d, w, h);
    if (ok && d->headless) d->device->SetWindowSize(w, h);
    return ok ? 1 : 0;
}

void ps2e_draw_frame_size(const ps2e_draw* d, int* w, int* h) {
    if (w) *w = d ? d->frame_w : 0;
    if (h) *h = d ? d->frame_h : 0;
}

uint32_t ps2e_draw_texture_create(ps2e_draw* d, int w, int h, const void* rgba8) {
    if (!d || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    render::SurfaceDesc td;
    td.width = uint32_t(w);
    td.height = uint32_t(h);
    td.format = render::SurfaceFormat::Rgba8;
    td.renderable = false;
    const render::SurfaceId s = d->device->CreateSurface(td);
    if (!s.id) return 0;
    const uint32_t id = d->next_texture++;
    d->textures[id] = ps2e_draw::Texture{s, w, h};
    d->counters.textures_alive++;
    if (rgba8) ps2e_draw_texture_upload(d, id, 0, 0, w, h, rgba8, size_t(w) * 4);
    return id;
}

uint32_t ps2e_draw_texture_create_gpu_alpha(ps2e_draw* d, int w, int h, const void* rgba8) {
    const uint32_t id = ps2e_draw_texture_create(d, w, h, nullptr);
    if (!id || !rgba8) return id;
    auto it = d->textures.find(id);
    d->device->Upload(it->second.surface, render::Rect{0, 0, w, h}, rgba8, size_t(w) * 4);
    return id;
}

int ps2e_draw_texture_upload(ps2e_draw* d, uint32_t texture, int x, int y, int w, int h, const void* rgba8,
                             size_t stride) {
    if (!d || !rgba8 || w <= 0 || h <= 0) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    if (x < 0 || y < 0 || x + w > it->second.w || y + h > it->second.h) return 0;
    if (stride == 0) stride = size_t(w) * 4;
    ScaleAlpha(static_cast<const uint8_t*>(rgba8), stride, w, h, d->scratch);
    d->device->Upload(it->second.surface, render::Rect{x, y, w, h}, d->scratch.data(), size_t(w) * 4);
    return 1;
}

int ps2e_draw_texture_set_mips(ps2e_draw* d, uint32_t texture, int count, const void* const* levels) {
    if (!d || count < 0 || count > 6 || (count && !levels)) return 0;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return 0;
    ps2e_draw::Texture& t = it->second;
    for (int i = 1; i <= count; ++i) {
        if (!levels[i - 1]) return 0;
        d->device->UploadLevel(t.surface, i, std::max(t.w >> i, 1), std::max(t.h >> i, 1), levels[i - 1]);
    }
    d->device->FinishMipChain(t.surface, count, false);
    t.levels = count;
    return 1;
}

void ps2e_draw_texture_destroy(ps2e_draw* d, uint32_t texture) {
    if (!d) return;
    auto it = d->textures.find(texture);
    if (it == d->textures.end()) return;
    d->device->DestroySurface(it->second.surface);
    d->textures.erase(it);
    d->counters.textures_alive--;
}

void ps2e_draw_begin_frame(ps2e_draw* d, const ps2e_draw_clear* clear) {
    if (!d) return;
    d->mesh_run = false;
    if (d->in_frame) d->device->EndDrawRun();
    // A changed internal scale takes effect at a frame's start (the target re-made before its draws); the texture
    // filter override with it (the present applies the rest)
    {
        std::lock_guard<std::mutex> lock(d->presentation_mutex);
        const int want_scale = std::clamp(d->wanted.scale, 1, 4);   // 0 = 1
        if (want_scale != d->scale) {
            d->scale = want_scale;
            MakeFrameTarget(*d, d->frame_w, d->frame_h);
        }
        d->applied.texture_filter = d->wanted.texture_filter;
    }
    d->device->BeginFrame();
    d->in_frame = true;
    d->frame_copy_valid = false;
    d->counters.triangles = 0;
    if (clear && (clear->clear_colour || clear->clear_depth)) {
        uint32_t flags = 0;
        if (clear->clear_colour) flags |= render::kClearColor;
        if (clear->clear_depth) flags |= render::kClearDepth;
        // Clear's rgba is the attachment's byte order (R in the low byte); the alpha written is the GPU's opaque.
        const uint32_t rgba = uint32_t(clear->red) | uint32_t(clear->green) << 8 | uint32_t(clear->blue) << 16 |
                              0xFFu << 24;
        d->device->Clear(d->color, d->depth, rgba, 0.0f, flags);
    }
    d->device->BeginDrawRun();
}

// The draw's GS record (d->ir, its vertices left as they are) and texture source from the producer's state; 0, or -2
// when the state names a texture that is not there (counted as refused)
static int PrepareRecord(ps2e_draw* d, const ps2e_draw_state* st, gs::TextureSource& ts) {
    // The record starts from its defaults each draw; the vertex vector keeps its capacity (a frame's draws allocate
    // nothing once the largest has been seen -- a fresh vector per draw cost a large allocation and its page faults).
    gs::DrawIr& ir = d->ir;
    {
        std::vector<gs::VertexRec> kept = std::move(ir.verts);
        ir = gs::DrawIr{};
        ir.verts = std::move(kept);
    }
    ir.prim_type = 3;
    ir.gouraud = st->gouraud != 0;
    ir.alpha_blend = st->blend != 0;
    ir.use_fst = false;
    ir.ctx = 0;
    ir.off_x = ir.off_y = 0.f;
    ir.raster.frame.fbp = 0;
    ir.raster.frame.fbw = uint32_t(d->frame_w);
    ir.raster.frame.psm = 0;      // PSMCT32
    ir.raster.frame.fbmsk = st->no_colour_write ? 0xFFFFFFFFu : 0u;
    // A nominal Z32 buffer base apart from the frame: the backend keys depth by the attachment, not the address.
    ir.raster.zbuf.zbp = uint32_t(d->frame_w) * uint32_t(d->frame_h) * 4u;
    ir.raster.zbuf.psm = 0;
    ir.raster.zbuf.zmsk = st->depth_write == 0;
    ir.raster.test.zte = true;
    ir.raster.test.ztst = uint8_t(std::clamp(st->depth_test, 0, 3));
    ir.raster.test.ate = st->alpha_test != 0;
    ir.raster.test.atst = uint8_t(std::clamp(st->alpha_method, 0, 7));
    ir.raster.test.aref = st->alpha_reference;
    ir.raster.test.afail = uint8_t(std::clamp(st->alpha_fail, 0, 3));
    ir.raster.alpha.a = uint8_t(st->blend_a & 3);
    ir.raster.alpha.b = uint8_t(st->blend_b & 3);
    ir.raster.alpha.c = uint8_t(st->blend_c & 3);
    ir.raster.alpha.d = uint8_t(st->blend_d & 3);
    ir.raster.alpha.fix = st->blend_fix;
    ir.raster.colclamp = true;
    // FBA on an unblended draw: the written alpha's top bit (the GPU's 0xFF). A blended draw keeps the fixed-function
    // blend (the backend's FBA arm would read the destination back per draw); its alpha stays the source's.
    ir.raster.fba = st->fba != 0 && st->blend == 0;
    if (st->scissor_x0 == 0 && st->scissor_y0 == 0 && st->scissor_x1 == 0 && st->scissor_y1 == 0) {
        ir.raster.scissor = {0, 0, uint16_t(d->frame_w - 1), uint16_t(d->frame_h - 1)};
    } else {
        ir.raster.scissor = {uint16_t(std::clamp(st->scissor_x0, 0, d->frame_w - 1)),
                             uint16_t(std::clamp(st->scissor_y0, 0, d->frame_h - 1)),
                             uint16_t(std::clamp(st->scissor_x1, 0, d->frame_w - 1)),
                             uint16_t(std::clamp(st->scissor_y1, 0, d->frame_h - 1))};
    }

    const ps2e_draw::Texture* tex = nullptr;
    if (st->texture == PS2E_DRAW_TEXTURE_FRAME_COPY) {
        // The screen copy as the texture -- only once a copy of this frame has been taken
        if (!d->frame_copy_valid || !d->frame_copy.surface.id) {
            d->counters.refused++;
            return -2;
        }
        tex = &d->frame_copy;
    } else if (st->texture) {
        auto it = d->textures.find(st->texture);
        if (it == d->textures.end()) {
            d->counters.refused++;
            return -2;
        }
        tex = &it->second;
    }
    if (tex) {
        ir.tex_enabled = true;
        ir.tex.fst = false;
        ir.tex.tfx = uint32_t(std::clamp(st->tfx, 0, 3));
        ir.tex.tcc = st->tcc ? 1u : 0u;
        ir.tex.psm = 0;
        ir.tex.tw = uint32_t(std::lround(std::log2(double(tex->w))));
        ir.tex.th = uint32_t(std::lround(std::log2(double(tex->h))));
        ir.tex.tbw = uint32_t(tex->w);
        ir.tex.mmag = st->linear != 0;
        ir.tex.mmin = st->linear ? 1 : 0;
        ir.tex.wms = uint8_t(st->wrap_u ? 1 : 0);
        ir.tex.wmt = uint8_t(st->wrap_v ? 1 : 0);
        ts.enabled = true;
        ts.owner.kind = gs::TextureSource::OwnerKind::Decoded;
        ts.owner.surface = tex->surface;
        ts.owner.owned = true;
        ts.transform.uv_tex_w = ts.transform.uv_pre_w = tex->w;
        ts.transform.uv_tex_h = ts.transform.uv_pre_h = tex->h;
        ts.transform.draw_scale = 1;
        // The title's TEX1 choice, or the Display page's override (ps2e_draw_presentation.texture_filter)
        const bool linear = d->applied.texture_filter == 1 ? false : d->applied.texture_filter == 2 ? true : st->linear != 0;
        ts.sampler.mag_linear = linear;
        ts.sampler.mmin = linear ? 1 : 0;
        ts.sampler.wms = uint8_t(st->wrap_u ? 1 : 0);
        ts.sampler.wmt = uint8_t(st->wrap_v ? 1 : 0);
        // TEX1's mipmapping over the texture's levels: the shader's per-pixel LOD (Q-based, LCM 0) between them
        const int mxl = std::min(st->mip_max_level, tex->levels);
        if (mxl > 0 && st->mip_minify >= 2 && st->mip_minify <= 5) {
            ts.owner.levels = uint8_t(tex->levels);
            ts.transform.mip_levels = uint8_t(mxl);
            ts.transform.mip_lcm = false;
            ts.transform.mip_k = int16_t(int32_t(uint32_t(st->mip_k) << 20) >> 20);   // the 12-bit field, sign-extended
            ts.transform.mip_l = uint8_t(st->mip_l & 3);
            // the Display page's point / bilinear choice keeps the mipmaps, nearest or linear within a level
            ts.sampler.mmin = uint8_t(d->applied.texture_filter == 1 ? 2 : d->applied.texture_filter == 2 ? 4 : st->mip_minify);
            ir.tex.mmin = ts.sampler.mmin;
        }
    }

    return 0;
}

// The frame target the draws go to, and the shadow counter's stencil test around this one draw
static render::DrawParams TargetParams(ps2e_draw* d, const ps2e_draw_state* st) {
    render::DrawParams p;
    p.color = d->color;
    p.depth = d->depth;
    p.render_w = p.target_w = d->frame_w;
    p.render_h = p.target_h = d->frame_h;
    p.scale = uint8_t(d->scale);
    p.has_32bit_content = true;
    p.cd_copy = &d->cd_copy;
    // The counter: the frame's depth attachment is the DepthStencil24_8 surface, and the backend applies the stencil
    // test and update around this one draw (VolumeStencil).
    if (st->stencil == PS2E_DRAW_STENCIL_INSIDE) StencilProbe(d, "composite");
    if (st->stencil != PS2E_DRAW_STENCIL_OFF) p.stencil_state = VolumeStencil(st);
    return p;
}

int ps2e_draw_submit(ps2e_draw* d, const ps2e_draw_state* st, const ps2e_draw_vertex* v, int count) {
    if (!d || !st || !v || count < 3) {
        if (d) d->counters.refused++;
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    if (st->stencil == PS2E_DRAW_STENCIL_CLEAR) {
        // The counter's origin (15: counts 1..16 set bit 4, as the GS byte 120 + 8n set bit 7 for n in 1..16)
        StencilProbe(d, "before-clear");
        d->device->ClearStencil(d->depth, kStencilOrigin);
        SeedCasterMask(d);
        StencilProbe(d, "after-clear");
        d->counters.draws++;
        return 0;
    }
    const int tri_verts = (count / 3) * 3;
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    if (PrepareRecord(d, st, ts) < 0) return -2;
    const render::DrawParams p = TargetParams(d, st);
    {
        // the device's raw draw when it takes this one: the vertices as given, their screen box (a vertex whose Q is
        // not finite takes the records, which read it as 1)
        static_assert(sizeof(ps2e_draw_vertex) == sizeof(render::RawVertex), "the producer's vertex is the raw layout");
        if (d->device->DrawsRaw(ir, ts, p)) {
            float box[4] = {v[0].x, v[0].y, v[0].x, v[0].y};
            bool finite = true;
            for (int i = 0; i < tri_verts; ++i) {
                const float x = v[i].x, y = v[i].y;
                box[0] = std::min(box[0], x);
                box[1] = std::min(box[1], y);
                box[2] = std::max(box[2], x);
                box[3] = std::max(box[3], y);
                if (!std::isfinite(v[i].q)) finite = false;
            }
            if (finite) {
                ir.verts.clear();
                ir.vertex_count = uint32_t(tri_verts);
                const render::DrawResult r =
                    d->device->DrawRaw(ir, ts, p, reinterpret_cast<const render::RawVertex*>(v), size_t(tri_verts), box);
                d->counters.draws++;
                if (!r.drew) {
                    d->counters.refused++;
                    return -3;
                }
                d->counters.triangles += uint64_t(tri_verts / 3);
                return tri_verts / 3;
            }
        }
    }
    ir.verts.resize(size_t(tri_verts));
    // Each record assembled in a local and copied out whole (no zeroing pass over the records first)
    struct alignas(4) Rec {
        float x, y, z, s, t, q;
        uint16_t uv_u, uv_v;
        uint8_t r, g, b, a, fog, draw_kick, pad[2];
    };
    static_assert(sizeof(Rec) == sizeof(gs::VertexRec), "Rec mirrors VertexRec's layout");
    uint8_t* out = reinterpret_cast<uint8_t*>(ir.verts.data());
    for (int i = 0; i < tri_verts; ++i) {
        const ps2e_draw_vertex& in = v[i];
        Rec rec;
        rec.x = in.x;
        rec.y = in.y;
        rec.z = in.z;
        // The GS's STQ: a producer giving q has u = S = u'.q, v = T = v'.q (the backend's per-pixel divide makes the
        // mapping perspective-correct); one leaving q 0 gets q = 1 (the affine mapping of its u, v)
        rec.q = (std::isfinite(in.q) && in.q != 0.f) ? in.q : 1.f;
        rec.s = in.u;
        rec.t = in.v;
        // uv_u / uv_v (the UV register's 12.4 texels) stay 0: the record is STQ (tex.fst off), and only the UV mode reads them
        rec.uv_u = rec.uv_v = 0;
        rec.r = in.r;
        rec.g = in.g;
        rec.b = in.b;
        rec.a = in.a;
        rec.fog = 0;
        rec.draw_kick = 1;
        rec.pad[0] = rec.pad[1] = 0;
        std::memcpy(out + size_t(i) * sizeof(Rec), &rec, sizeof(Rec));
    }
    ir.vertex_count = uint32_t(tri_verts);

    const render::DrawResult r = d->device->Draw(ir, ts, p);
    d->counters.draws++;
    if (!r.drew) {
        d->counters.refused++;
        return -3;
    }
    d->counters.triangles += uint64_t(tri_verts / 3);
    return tri_verts / 3;
}

uint32_t ps2e_draw_mesh_create(ps2e_draw* d, const ps2e_draw_mesh_vertex* verts, int count) {
    static_assert(sizeof(ps2e_draw_mesh_vertex) == sizeof(render::MeshVertex), "the mesh vertex is the backend's");
    if (!d || !d->device || !verts || count < 3) return 0;
    return d->device->CreateMesh(reinterpret_cast<const render::MeshVertex*>(verts), size_t(count / 3 * 3));
}

void ps2e_draw_mesh_destroy(ps2e_draw* d, uint32_t mesh) {
    if (d && d->device && mesh) d->device->DestroyMesh(mesh);
}

int ps2e_draw_submit_mesh(ps2e_draw* d, const ps2e_draw_state* st, uint32_t mesh, const ps2e_draw_mesh_params* mp) {
    if (!d || !st || !mesh || !mp || st->stencil == PS2E_DRAW_STENCIL_CLEAR) {
        if (d) d->counters.refused++;
        return -1;
    }
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    render::MeshDrawParams m;
    std::memcpy(m.screen, mp->screen, sizeof m.screen);
    m.offset[0] = mp->offset[0];
    m.offset[1] = mp->offset[1];
    m.scroll[0] = mp->scroll[0];
    m.scroll[1] = mp->scroll[1];
    m.lit = mp->lit != 0;
    std::memcpy(m.light_dir, mp->light_dir, sizeof m.light_dir);
    std::memcpy(m.light_colour, mp->light_colour, sizeof m.light_colour);
    std::memcpy(m.ambient, mp->ambient, sizeof m.ambient);
    std::memcpy(m.box, mp->box, sizeof m.box);
    static const bool xNoAgain = std::getenv("TE_X_NO_AGAIN") != nullptr;   // XBISECT
    if (!xNoAgain && d->mesh_run && d->mesh_run_draws == d->counters.draws && std::memcmp(&d->mesh_run_state, st, sizeof *st) == 0) {
        const render::DrawResult again = d->device->DrawMeshAgain(mesh, m);
        if (again.drew) {
            d->counters.draws++;
            d->mesh_run_draws = d->counters.draws;
            return 1;
        }
    }
    d->mesh_run = false;
    gs::DrawIr& ir = d->ir;
    gs::TextureSource ts;
    if (PrepareRecord(d, st, ts) < 0) return -2;
    ir.verts.clear();
    ir.vertex_count = 0;
    const render::DrawParams p = TargetParams(d, st);
    const render::DrawResult r = d->device->DrawMesh(ir, ts, p, mesh, m);
    d->counters.draws++;
    if (!r.drew) {
        d->counters.refused++;
        return -3;
    }
    d->mesh_run = true;
    d->mesh_run_state = *st;
    d->mesh_run_draws = d->counters.draws;
    return 1;
}

int ps2e_draw_copy_frame(ps2e_draw* d) {
    if (!d || !d->frame_copy.surface.id) return 0;
    d->mesh_run = false;
    if (!d->in_frame) ps2e_draw_begin_frame(d, nullptr);
    // The draws so far are issued (the device draws as it goes; the vertex ring is flushed per draw), so the copy holds
    // every submit before this call and none after it.
    const render::Rect whole{0, 0, d->frame_copy.w, d->frame_copy.h};
    d->device->CopyRect(d->color, whole, d->frame_copy.surface, whole, 0);
    d->frame_copy_valid = true;
    return 1;
}

namespace {
// The present's body: the frame target scanned out -- to the window and swapped (to_window), or resolved for the armed
// capture only (ps2e_draw_capture_frame: a picture the producer measures but does not show).
void Scanout(ps2e_draw* d, bool to_window);
}  // namespace

void ps2e_draw_present(ps2e_draw* d) {
    if (!d) return;
    d->mesh_run = false;
    Scanout(d, true);
    d->presented++;
}

void ps2e_draw_capture_frame(ps2e_draw* d) {
    if (!d) return;
    Scanout(d, false);
}

namespace {
void Scanout(ps2e_draw* d, bool to_window) {
    if (!d->in_frame) {
        d->device->BeginFrame();
        d->device->BeginDrawRun();
        d->in_frame = true;
    }
    d->device->EndDrawRun();
    // The drawing thread's half of the presentation -- the swap interval, the picture's shape / fit / move, the
    // scan-out filter (the internal scale and the texture filter change at a frame's start: ps2e_draw_begin_frame).
    ps2e_draw_presentation want;
    {
        std::lock_guard<std::mutex> lock(d->presentation_mutex);
        want = d->wanted;
    }
    if (want.vsync != d->applied.vsync && d->context.IsCreated()) d->context.SetSwapInterval(want.vsync ? 1 : 0);
    const int filter_in_use = d->applied.texture_filter;   // the frame's draws were sampled with it; begin_frame moves it
    d->applied = want;
    d->applied.texture_filter = filter_in_use;
    render::ScanoutDesc sd;
    sd.source = d->color;
    sd.src = {0, 0, d->frame_w * d->scale, d->frame_h * d->scale};   // physical texels
    // The drawable size the window's thread noted (the pump), else asked once here (the creating thread, before any pump)
    int ww = d->frame_w, wh = d->frame_h;
    if (!d->headless && d->drawable_w > 0 && d->drawable_h > 0) {
        ww = d->drawable_w;
        wh = d->drawable_h;
    }
    if (ww <= 0 || wh <= 0) {
        ww = d->frame_w;
        wh = d->frame_h;
    }
    // The picture's SHAPE: the frame's own pixels by default, the console's TV (4:3: the frame is
    // anamorphic, 512 wide for a 4:3 TV at 448 or 512 lines) or another the player chose, or the window's shape.
    double aspect = double(d->frame_w) / d->frame_h;
    switch (want.aspect) {
    case PS2E_DRAW_ASPECT_4_3: aspect = 4.0 / 3.0; break;
    case PS2E_DRAW_ASPECT_16_9: aspect = 16.0 / 9.0; break;
    case PS2E_DRAW_ASPECT_16_10: aspect = 16.0 / 10.0; break;
    case PS2E_DRAW_ASPECT_21_9: aspect = 21.0 / 9.0; break;
    case PS2E_DRAW_ASPECT_WINDOW: aspect = double(ww) / wh; break;
    default: break;
    }
    int dw = 1, dh = 1;
    if (want.fit == PS2E_DRAW_FIT_FILL) {
        // the window covered, the picture's shape kept (its edges cropped)
        if (double(ww) / wh > aspect) { dw = ww; dh = std::max(1, int(ww / aspect + 0.5)); }
        else { dh = wh; dw = std::max(1, int(wh * aspect + 0.5)); }
    } else {
        // letterbox: the largest picture of that shape inside the window
        if (double(ww) / wh > aspect) { dh = wh; dw = std::max(1, int(wh * aspect + 0.5)); }
        else { dw = ww; dh = std::max(1, int(ww / aspect + 0.5)); }
        if (want.fit == PS2E_DRAW_FIT_INTEGER) {
            // whole multiples of the logical frame's height (the width follows the shape)
            const int k = std::max(1, dh / d->frame_h);
            dh = k * d->frame_h;
            dw = std::max(1, int(dh * aspect + 0.5));
        }
    }
    // MoveDisplay: the picture moved by frame pixels, scaled as the picture is
    const int ox = int(std::lround(double(want.offset_x) * dw / d->frame_w));
    const int oy = int(std::lround(double(want.offset_y) * dh / d->frame_h));
    sd.dst = {(ww - dw) / 2 + ox, (wh - dh) / 2 + oy, dw, dh};
    sd.filter = want.nearest ? render::PresentationFilter::Nearest : render::PresentationFilter::Linear;
    sd.dither16 = d->present_ct16;   // The retail CT16 scanout
    sd.dimx = d->dimx;
    sd.to_window = to_window;
    d->last_out_x = sd.dst.x;
    d->last_out_y = sd.dst.y;
    d->last_out_w = dw;
    d->last_out_h = dh;
    d->device->SetWindowSize(ww, wh);
    d->device->Present(sd);
    d->device->EndFrame();
    d->in_frame = false;
}
}  // namespace

uint64_t ps2e_draw_frames_presented(const ps2e_draw* d) { return d ? d->presented : 0; }

void ps2e_draw_set_presentation(ps2e_draw* d, const ps2e_draw_presentation* p) {
    if (!d || !p) return;
    std::lock_guard<std::mutex> lock(d->presentation_mutex);
    d->wanted = *p;
    d->wanted.scale = std::clamp(p->scale, 0, 4);
}

void ps2e_draw_get_presentation(const ps2e_draw* d, ps2e_draw_presentation* out) {
    if (!out) return;
    if (!d) { *out = ps2e_draw_presentation{}; return; }
    std::lock_guard<std::mutex> lock(const_cast<ps2e_draw*>(d)->presentation_mutex);
    *out = d->wanted;
}

void ps2e_draw_last_output_rect(const ps2e_draw* d, int* x, int* y, int* w, int* h) {
    if (x) *x = d ? d->last_out_x : 0;
    if (y) *y = d ? d->last_out_y : 0;
    if (w) *w = d ? d->last_out_w : 0;
    if (h) *h = d ? d->last_out_h : 0;
}

void ps2e_draw_set_present_ct16(ps2e_draw* d, int enable, uint64_t dimx) {
    if (!d) return;
    d->present_ct16 = enable != 0;
    d->dimx = dimx ? dimx : render::kDefaultDimx;
}
int ps2e_draw_present_ct16(const ps2e_draw* d) { return d && d->present_ct16 ? 1 : 0; }

void ps2e_draw_arm_capture(ps2e_draw* d, int armed) {
    if (!d) return;
    d->capture = armed != 0;
    d->device->ArmPresentCapture(d->capture);
}

int ps2e_draw_read_presented(ps2e_draw* d, uint8_t* out, size_t capacity, int* w, int* h) {
    if (!d) return 0;
    std::vector<uint8_t> img;
    int iw = 0, ih = 0;
    if (!d->device->ReadPresented(&img, &iw, &ih) || iw <= 0 || ih <= 0) {
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (w) *w = iw;
    if (h) *h = ih;
    const size_t need = size_t(iw) * ih * 4;
    if (!out || capacity < need) return 0;
    // ReadPresented is bottom-up (GL rows); the contract is top row first.
    for (int y = 0; y < ih; ++y)
        std::memcpy(out + size_t(y) * iw * 4, img.data() + size_t(ih - 1 - y) * iw * 4, size_t(iw) * 4);
    return 1;
}

int ps2e_draw_write_ppm(ps2e_draw* d, const char* path) {
    if (!d || !path) return 0;
    int w = 0, h = 0;
    ps2e_draw_read_presented(d, nullptr, 0, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    if (!ps2e_draw_read_presented(d, rgba.data(), rgba.size(), &w, &h)) return 0;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
    return 1;
}

void ps2e_draw_counts(const ps2e_draw* d, ps2e_draw_counters* out) {
    if (!out) return;
    *out = d ? d->counters : ps2e_draw_counters{};
}

}  // extern "C"
