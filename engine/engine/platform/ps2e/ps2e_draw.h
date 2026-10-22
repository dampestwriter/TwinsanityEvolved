/* engine/platform/ps2e/ps2e_draw.h -- ps2e_draw: the typed-draw surface a NON-recompiled producer (a decompiled
 * game's native platform arm over this engine) draws a frame through.
 *
 * The engine's owned graphics path consumes DrawIr records (engine/gs/hle/ir/draw_ir.h) produced by the recompiled
 * title's HLE producers over a GS VRAM model. A decomp has no VRAM, no GIF packets and no DMA chains: it knows a frame
 * of W x H pixels, textures as RGBA8 images, and draws as lists of vertices with a small render state. This header is
 * that surface, in C so a foreign toolchain (wasm2c output, a C runner) can call it without the engine's C++ types:
 *
 *   device  = ps2e_draw_create(&config)          one SDL window + the render backend the build's arm supplies + one frame
 *                                                target (PS2R_PS2E_DRAW_BACKEND: gl = a GL 4.1 context + GlDevice; null)
 *   tex     = ps2e_draw_texture_create(...)      RGBA8, top row first, PS2 alpha bytes (0x80 = 1.0)
 *   ps2e_draw_begin_frame(device, &clear)        the frame target cleared (colour and/or depth) or kept
 *   ps2e_draw_submit(device, &state, verts, n)   one draw: GS PRIM/TEST/ALPHA-valued state over frame-pixel vertices
 *   ps2e_draw_present(device)                    the frame target scanned out to the window and swapped
 *
 * Value contract: the state's numbers are the PS2's (the GS User's Manual's PRIM, TEST.ZTST/ATST/AFAIL, ALPHA.A/B/C/D/
 * FIX, TEX0.TFX/TCC, TEX1.MMAG, CLAMP.WMS/WMT) because the game's materials carry exactly those values and the backend
 * (engine/render/backend.h) implements their exact semantics; this header does NOT carry any VRAM address, packet
 * word or register bit layout. Vertex x/y are frame pixels (0,0 the frame's top-left corner; 1/16 fractions are kept),
 * z is the GS depth (0 .. 2^24-1 for a Z24 buffer; bigger values are clamped by the backend's mapping), u/v are
 * fractions of the texture (0..1, wrapping per the state; times the vertex's q when one is given -- the GS's S, T and Q,
 * interpolated linearly and divided per pixel: perspective-correct), colours are RGBA bytes with the PS2 alpha scale.
 * Every draw is a TRIANGLE LIST: the producer expands its strips and sprites (so the vertex order is the triangle
 * order the GS would have rasterised, and the backend's bounding-box and coverage rules apply unchanged).
 *
 * Threading: one thread owns the device from create to destroy (the GL arm's context is current on it). Headless devices
 * (config.headless) draw into a hidden window; presentation then only feeds the capture (ps2e_draw_read_presented),
 * which is how a judge measures a frame without a display.
 */
#ifndef PS2E_DRAW_H
#define PS2E_DRAW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2e_draw ps2e_draw;

typedef struct ps2e_draw_config {
    int frame_width;     /* the logical frame (render target) in pixels, e.g. 512 x 448 (NTSC) / 512 x 512 (PAL) */
    int frame_height;
    int window_width;    /* the window's client size; 0 = the frame's size */
    int window_height;
    int headless;        /* 1 = hidden window (judges, CI); presentation still renders for capture */
    const char* title;   /* window title; NULL = "ps2e_draw" */
    int stencil_diag;    /* 1 = log the stencil's histogram at the shadow bracket's clear and composite ([ps2e.stencil] lines) */
} ps2e_draw_config;

/* Creates the window, the arm's backend (the GL arm: its GL 4.1 core context + device) and the frame target. NULL when
 * the arm cannot be made (the GL arm on a host without GL). */
ps2e_draw* ps2e_draw_create(const ps2e_draw_config* config);
void ps2e_draw_destroy(ps2e_draw* device);

/* Processes the window's events. Returns 1 when the user asked the window to close, else 0. The window's thread
 * (the one that created the device) calls this; it notes the drawable size the present uses. */
int ps2e_draw_pump(ps2e_draw* device);
/* Threads. The GL context is current on the creating thread after ps2e_draw_create. A producer that draws from another
 * thread (a render thread: submit + present off the game's thread) first releases it on the creating thread
 * (bind = 0) and then binds it on the drawing thread (bind = 1); every call but ps2e_draw_pump then belongs to the
 * drawing thread (texture create/upload/destroy, resize, submit, present, the presented-image reads). Returns 1 when
 * the context changed hands, 0 when the device has no context (headless without GL) or the call failed. */
int ps2e_draw_bind_thread(ps2e_draw* device, int bind);
#define PS2E_DRAW_HAS_BIND_THREAD 1   /* for consumers pinned across engine versions */
/* The frame target re-made at a new logical size (the title's display height may change: 448/512). */
int ps2e_draw_resize_frame(ps2e_draw* device, int frame_width, int frame_height);
void ps2e_draw_frame_size(const ps2e_draw* device, int* frame_width, int* frame_height);

/* ---- textures ------------------------------------------------------------------------------------------------ */
/* An RGBA8 texture, `width * height * 4` bytes, top row first, PS2 alpha bytes (0x80 = fully opaque when modulated;
 * the device applies the ONE GPU alpha scaling). NULL pixels = an uninitialised texture to be filled by
 * ps2e_draw_texture_upload. Returns 0 on failure. */
uint32_t ps2e_draw_texture_create(ps2e_draw* device, int width, int height, const void* rgba8);
int ps2e_draw_texture_upload(ps2e_draw* device, uint32_t texture, int x, int y, int width, int height,
                             const void* rgba8, size_t stride_bytes);
/* The same with GPU alpha bytes already (0xFF opaque): what the engine's TextureDecoder produces. */
uint32_t ps2e_draw_texture_create_gpu_alpha(ps2e_draw* device, int width, int height, const void* rgba8);
void ps2e_draw_texture_destroy(ps2e_draw* device, uint32_t texture);
/* A texture's levels below the first, as the PS2 kept them (MIPTBP1/2): `count` images of halving size (at least 1x1),
 * each `max(width >> i, 1) * max(height >> i, 1) * 4` bytes with the device's GPU alpha (as
 * ps2e_draw_texture_create_gpu_alpha takes them). Replaces any levels the texture had. Returns 1 on success. */
int ps2e_draw_texture_set_mips(ps2e_draw* device, uint32_t texture, int count, const void* const* levels);

/* ---- a frame ------------------------------------------------------------------------------------------------- */
typedef struct ps2e_draw_clear {
    uint8_t red, green, blue;   /* the clear colour (alpha written 0x80) */
    int clear_colour;           /* 1 = clear the colour buffer to the colour; 0 = keep the previous frame's pixels */
    int clear_depth;            /* 1 = clear the depth buffer to the GS far plane (0) */
} ps2e_draw_clear;
void ps2e_draw_begin_frame(ps2e_draw* device, const ps2e_draw_clear* clear);

typedef struct ps2e_draw_vertex {
    float x, y;                 /* frame pixels from the frame's top-left corner (fractions kept: the GS's 1/16) */
    float z;                    /* GS depth */
    float u, v;                 /* fractions of the texture, times q when q is given (the GS's S and T) */
    uint8_t r, g, b, a;         /* RGBA bytes, PS2 alpha scale */
    float q;                    /* the GS's Q (1/w): u and v are S = u'.q and T = v'.q; 0 = 1 (u and v as they are). The
                                   backend interpolates S, T and Q linearly over the screen and divides per pixel as the GS
                                   does, so a producer that gives its vertices' q gets perspective-correct texturing; one that
                                   leaves it 0 gets the affine mapping (every triangle's texture flat in screen space) */
} ps2e_draw_vertex;
/* The vertex carries q (perspective-correct texturing). A producer built against an older header leaves it 0 (affine). */
#define PS2E_DRAW_HAS_VERTEX_Q 1

/* GS numbering throughout (the game's shader settings ARE these values). */
enum ps2e_draw_depth_test { PS2E_DRAW_DEPTH_NEVER = 0, PS2E_DRAW_DEPTH_ALWAYS = 1, PS2E_DRAW_DEPTH_GEQUAL = 2,
                            PS2E_DRAW_DEPTH_GREATER = 3 };
enum ps2e_draw_alpha_test { PS2E_DRAW_ATST_NEVER = 0, PS2E_DRAW_ATST_ALWAYS = 1, PS2E_DRAW_ATST_LESS = 2,
                            PS2E_DRAW_ATST_LEQUAL = 3, PS2E_DRAW_ATST_EQUAL = 4, PS2E_DRAW_ATST_GEQUAL = 5,
                            PS2E_DRAW_ATST_GREATER = 6, PS2E_DRAW_ATST_NOTEQUAL = 7 };
enum ps2e_draw_wrap { PS2E_DRAW_WRAP_REPEAT = 0, PS2E_DRAW_WRAP_CLAMP = 1 };
enum ps2e_draw_tfx { PS2E_DRAW_TFX_MODULATE = 0, PS2E_DRAW_TFX_DECAL = 1, PS2E_DRAW_TFX_HIGHLIGHT = 2,
                     PS2E_DRAW_TFX_HIGHLIGHT2 = 3 };

typedef struct ps2e_draw_state {
    uint32_t texture;           /* 0 = untextured (vertex colours only) */
    int gouraud;                /* 1 = per-vertex colour interpolation (PRIM.IIP), 0 = flat (the last vertex's) */
    int depth_test;             /* ps2e_draw_depth_test (TEST.ZTST; ZTE on) */
    int depth_write;            /* 0 = ZBUF.ZMSK */
    int blend;                  /* PRIM.ABE */
    uint8_t blend_a, blend_b, blend_c, blend_d, blend_fix;   /* ALPHA: (A - B) * C + D over {Cs, Cd, 0} / {As, Ad, FIX} */
    int alpha_test;             /* TEST.ATE */
    int alpha_method;           /* ps2e_draw_alpha_test (TEST.ATST) */
    uint8_t alpha_reference;    /* TEST.AREF */
    int alpha_fail;             /* TEST.AFAIL: 0 KEEP, 1 FB_ONLY, 2 ZB_ONLY, 3 RGB_ONLY */
    int linear;                 /* TEX1.MMAG/MMIN linear (1) or nearest (0) */
    int wrap_u, wrap_v;         /* ps2e_draw_wrap (CLAMP.WMS/WMT) */
    int tfx;                    /* ps2e_draw_tfx (TEX0.TFX) */
    int tcc;                    /* TEX0.TCC: 1 = the texture's alpha is used */
    int scissor_x0, scissor_y0, scissor_x1, scissor_y1;   /* inclusive frame pixels; all zero = the whole frame */
    int no_colour_write;        /* 1 = FRAME.FBMSK all set: the draw writes depth only (a depth clear) */
    int stencil;                /* ps2e_draw_stencil: the shadow-volume counter (0 = off) */
    int fba;                    /* FRAME alpha forced: the written alpha's top bit set (an unblended draw; see below) */
    int dest_alpha_test;        /* on a COUNT_UP / COUNT_DOWN draw: TEST.DATE with DATM 0 against the frame's alpha at the clear */
    /* TEX1's mipmapping over the texture's levels (ps2e_draw_texture_set_mips); all zero = level 0 only */
    int mip_max_level;          /* TEX1.MXL: the deepest level the draw may use (0 = no mipmapping) */
    int mip_k;                  /* TEX1.K, signed 7.4 fixed point */
    int mip_l;                  /* TEX1.L */
    int mip_minify;             /* TEX1.MMIN with mipmaps: 2..5 (the GS numbering) */
} ps2e_draw_state;

/* The draw state carries TEX1's mipmapping and the textures their levels (ps2e_draw_texture_set_mips). */
#define PS2E_DRAW_HAS_MIPS 1

/* The casters' exclusion (shadows.cpp StartShadows). The GS shrank the frame's alpha into the half-size buffer
 * and counted the volume strips only where that alpha's top bit was 0 (DATE, DATM 0): the lit world writes alpha 0x7E
 * and receives the shadows, a skin writes 0x80 (or a material writes with FBA) and is never darkened -- a caster's own
 * volume would otherwise darken half his body. Here the counter's CLEAR also marks every frame pixel whose alpha is
 * the PS2 0x80 or more (the backend's GPU-scaled 0xFF) in a stencil bit above the counter, and a counting draw with
 * dest_alpha_test skips those pixels. fba is honoured on unblended draws (a blended draw keeps the fixed-function blend
 * and writes its source alpha). The frame's clear writes the top bit set (the undrawn background is never a receiver). */
#define PS2E_DRAW_HAS_FBA_DATE 1

/* The shadow volumes' counter (TR shadows.cpp StartShadows / the type-0B volume strips / ApplyShadows, as the GS did it in
 * a half-size accumulator: the counter byte starts at 15, every volume strip adds +1 (its +8 winding colour) or -1 (248)
 * where it passes the depth test against the scene, and the composite darkens where the count is 1..16). Here the frame's
 * stencil is the accumulator at the frame's resolution: CLEAR writes the origin (a submit with this value draws nothing),
 * COUNT_UP / COUNT_DOWN wrap the low 5 bits per fragment FAILING the depth test -- the GS tested the volume's negated z
 * against the scene depth's inverted copy, so a face counts where it lies at or behind the scene surface (a volume sets
 * depth_test GEQUAL, no_colour_write and depth_write 0; its z is the ordinary GS depth) -- and INSIDE draws only where the
 * count is 1..16 (the composite quad).
 * PS2E_DRAW_HAS_STENCIL lets a producer built against an older device keep its volumes as stamps. */
#define PS2E_DRAW_HAS_STENCIL 1
enum ps2e_draw_stencil { PS2E_DRAW_STENCIL_OFF = 0, PS2E_DRAW_STENCIL_COUNT_UP = 1, PS2E_DRAW_STENCIL_COUNT_DOWN = 2,
                         PS2E_DRAW_STENCIL_INSIDE = 3, PS2E_DRAW_STENCIL_CLEAR = 4 };

/* One draw of `count` vertices (a multiple of 3: a triangle list). Returns the number of triangles rasterised, or a
 * negative value when the state or the vertices were refused (nothing drawn). */
int ps2e_draw_submit(ps2e_draw* device, const ps2e_draw_state* state, const ps2e_draw_vertex* vertices, int count);

/* NATIVE MESHES: a model's triangles kept on the device and drawn through a matrix, the transform to the GS's 12.4 screen,
 * the divide and the lighting done per vertex by the device (the CPU builds nothing per frame). ps2e_draw_mesh_create
 * takes a triangle list and returns its handle, or 0 when the backend has no native transform (the producer then builds
 * the triangles itself and submits them). A mesh is destroyed only once no submitted frame still draws it. */
#define PS2E_DRAW_HAS_MESH 1
typedef struct ps2e_draw_mesh_vertex {
    float x, y, z;              /* the model's space */
    float s, t, q;              /* the vertex's STQ as the model has it (a draw's UV scroll is added before the divide) */
    uint8_t r, g, b, a;         /* the colour an unlit draw shows */
    uint8_t fog, pad[3];        /* 0 */
    float nx, ny, nz;           /* the normal in the model's space (0 without one: a lit draw then shows the ambient) */
    uint8_t lr, lg, lb, la;     /* the colour a lit draw lights */
} ps2e_draw_mesh_vertex;        /* 48 bytes */
typedef struct ps2e_draw_mesh_params {
    float screen[16];           /* model -> GS screen (x / y in 12.4 fixed pixels, depth), the title's row-vector
                                   convention: rows 0..3, position = row3 + x.row0 + y.row1 + z.row2 */
    float offset[2];            /* the GS origin taken off after the divide (frame pixels) */
    float scroll[2];            /* added to S and T before the divide */
    int lit;                    /* colour = lit colour x (ambient + sum colour_i max(0, normal . dir_i)), clamped */
    float light_dir[3][3];
    float light_colour[3][3];
    float ambient[3];
    float box[4];               /* the draw's screen box (frame pixels x0, y0, x1, y1): the area a blend reading the
                                   destination copies */
} ps2e_draw_mesh_params;
uint32_t ps2e_draw_mesh_create(ps2e_draw* device, const ps2e_draw_mesh_vertex* vertices, int count);
void ps2e_draw_mesh_destroy(ps2e_draw* device, uint32_t mesh);
/* One draw of a mesh under `state` (its texture, blend, tests; never the stencil clear). Returns > 0 drawn, < 0 refused. */
int ps2e_draw_submit_mesh(ps2e_draw* device, const ps2e_draw_state* state, uint32_t mesh, const ps2e_draw_mesh_params* params);

/* The SCREEN COPY (TwinsanityEvolved; TR shaders.cpp WriteScreenCopy / shadersettings.cpp MakeScreenCopySettings): the
 * screen-copy shader types (0x10 / 0x11, and 0x18's distortion) began their material's packet by copying the frame as
 * drawn so far into a buffer they then sampled as their TEXTURE (the whole frame across 0..1, clamped) -- the cavern's
 * crystals refract what lies behind them. ps2e_draw_copy_frame takes that copy of the frame target, ordered with the
 * submits around it (every draw submitted before it is in the copy, nothing after); a state whose `texture` is
 * PS2E_DRAW_TEXTURE_FRAME_COPY samples the latest copy of this frame (u, v = fractions of the frame, CLAMP; the copy's
 * alpha is the frame's as written). A frame-copy draw with no copy taken this frame is refused (nothing drawn). The
 * copy is the frame target's physical size and is re-made with it. */
#define PS2E_DRAW_HAS_FRAME_COPY 1
#define PS2E_DRAW_TEXTURE_FRAME_COPY 0xFFFFFFF0u
int ps2e_draw_copy_frame(ps2e_draw* device);

/* Scans the frame target out to the window (letterboxed to the window's size) and swaps. */
void ps2e_draw_present(ps2e_draw* device);
uint64_t ps2e_draw_frames_presented(const ps2e_draw* device);
/* The frame RESOLVED as a present would resolve it -- the armed capture (ps2e_draw_read_presented, ps2e_draw_write_ppm)
 * holds it afterwards -- but never shown: no blit to the window, no swap, the presented count unchanged. A producer
 * that presents at its own cadence (every other picture at 30; interpolated frames between the pictures at 120 and
 * above) resolves the pictures it does not show this way, so a judge's picture hashes are the same at every rate.
 * Without an armed capture the call only ends the frame. */
void ps2e_draw_capture_frame(ps2e_draw* device);
#define PS2E_DRAW_HAS_CAPTURE_FRAME 1
/* The CT16 present (a title's retail look: the frame drawn in 24-bit colour and copied into the 16-bit buffer the CRTC
 * showed, with the GS's dithering -- DTHE and the DIMX matrix). enable 1 = every present quantises each channel to its
 * top five bits after adding the frame pixel's DIMX cell (-4..3), shown expanded to the full range; 0 = the frame's
 * colours as drawn (the default). `dimx` is the GS DIMX register (16 nibbles, a 3-bit two's-complement cell each, row-
 * major from the lowest); 0 = the GS's default matrix. Takes effect from the next present. */
void ps2e_draw_set_present_ct16(ps2e_draw* device, int enable, uint64_t dimx);
int ps2e_draw_present_ct16(const ps2e_draw* device);
#define PS2E_DRAW_HAS_PRESENT_CT16 1

/* ---- presentation (the title's Display page; TwinsanityEvolved) ------------------------------------------*/
/* How the frame is shown: the display SHAPE (the PS2's frame is anamorphic -- 512 wide for a 4:3 TV whether it is 448
 * or 512 lines tall -- so the frame's own pixel aspect is never what a TV showed), the fit, a move of the picture
 * (the console's "centre screen" option: DISPLAY DX/DY), the scan-out filter, the swap interval, the window mode, the
 * frame target's INTERNAL scale (physical = logical * scale: the same geometry rasterised at 2x..4x) and the windowed
 * size. Set from any thread; the window changes (mode, size) are applied by ps2e_draw_pump on the window's thread, the
 * internal scale and the texture filter at the next ps2e_draw_begin_frame, the rest by the next present on the
 * drawing thread. Every default (all zero) reproduces the plain picture exactly -- the frame's own pixels
 * letterboxed, linear, no swap interval, windowed, scale 1 -- so a consumer that never sets a presentation (and the
 * frozen judges' captures) is unchanged; a title's Display page chooses the TV shape (4:3) for play. */
typedef enum ps2e_draw_aspect {
    PS2E_DRAW_ASPECT_PIXEL = 0,  /* the frame's own pixels (512x512 PAL shows square): the default*/
    PS2E_DRAW_ASPECT_4_3 = 1,    /* the console's TV */
    PS2E_DRAW_ASPECT_16_9 = 2,
    PS2E_DRAW_ASPECT_16_10 = 3,
    PS2E_DRAW_ASPECT_21_9 = 4,
    PS2E_DRAW_ASPECT_WINDOW = 5  /* the window's shape (stretch) */
} ps2e_draw_aspect;
typedef enum ps2e_draw_fit { PS2E_DRAW_FIT_LETTERBOX = 0, PS2E_DRAW_FIT_FILL = 1, PS2E_DRAW_FIT_INTEGER = 2 } ps2e_draw_fit;
typedef enum ps2e_draw_window_mode { PS2E_DRAW_WINDOWED = 0, PS2E_DRAW_FULLSCREEN = 1, PS2E_DRAW_BORDERLESS = 2 } ps2e_draw_window_mode;
typedef struct ps2e_draw_presentation {
    int aspect;          /* ps2e_draw_aspect */
    int fit;             /* ps2e_draw_fit */
    int offset_x;        /* the picture moved, in FRAME pixels (scaled with it): the title's MoveDisplay -- the PS2 moved */
    int offset_y;        /*   up to 256 VCK (64 frame pixels) across and 32 lines down */
    int nearest;         /* 1 = nearest scan-out, 0 = linear (the default) */
    int vsync;           /* the swap interval: 1 = on, 0 = off (the default: the producer paces itself) */
    int window_mode;     /* ps2e_draw_window_mode */
    int scale;           /* the frame target's internal scale 1..4 (0 or 1 = the default; the capture stays the logical size) */
    int window_scale;    /* windowed: the window resized to frame * window_scale on the next pump (0 = leave it) */
    int texture_filter;  /* the draws' texture sampling: 0 = the title's own per-draw choice (TEX1), 1 = point, 2 = bilinear */
} ps2e_draw_presentation;
#define PS2E_DRAW_HAS_PRESENTATION 1   /* for consumers pinned across engine versions */
void ps2e_draw_set_presentation(ps2e_draw* device, const ps2e_draw_presentation* presentation);
void ps2e_draw_get_presentation(const ps2e_draw* device, ps2e_draw_presentation* out);
/* The window rectangle the last present drew the frame into (window pixels; the capture's coordinates when headless). */
void ps2e_draw_last_output_rect(const ps2e_draw* device, int* x, int* y, int* w, int* h);

/* ---- the presented image (judges) ---------------------------------------------------------------------------- */
/* While armed, every present keeps the window image it drew. ps2e_draw_read_presented copies it as RGBA8, top row
 * first, into `rgba8` (capacity `capacity` bytes) and reports its size; returns 1 when an image was copied, 0 when
 * none is held or the buffer is too small (the size is still reported so the caller can retry). */
void ps2e_draw_arm_capture(ps2e_draw* device, int armed);
int ps2e_draw_read_presented(ps2e_draw* device, uint8_t* rgba8, size_t capacity, int* width, int* height);
/* The held presented image written as a binary PPM (P6). Returns 1 on success. */
int ps2e_draw_write_ppm(ps2e_draw* device, const char* path);

/* Counters a producer may report: draws submitted and refused since create, triangles this frame. */
typedef struct ps2e_draw_counters {
    uint64_t draws;
    uint64_t refused;
    uint64_t triangles;
    uint64_t textures_alive;
} ps2e_draw_counters;
void ps2e_draw_counts(const ps2e_draw* device, ps2e_draw_counters* out);

#ifdef __cplusplus
}
#endif
#endif /* PS2E_DRAW_H */
