// tools/judges/graphics_selftest.cpp -- te_graphics_selftest: the graphics seam's host side driven WITHOUT the game module.
//
// A 32 MB guest memory is stood up here (the runner's te::GuestMemory contract), the nativeee arm's objects are written
// into it in the te_gfx_layout.h layout the wasm side would have written (a flat material, a textured material whose
// texture is a hand-built GS upload chain exactly shaped like the game's texture resources), and the ps2e_graphics_*
// imports are called the way the game calls them for one title-like frame: StartRenderer, SetUpRenderTarget,
// StartFrame (a clear), DrawSprite (flat and textured), DrawStrip, Font glyph quads, Present. The presented frame is
// written as PPM for tools/judges/title_frame_judge.py:
//
//   te_graphics_selftest <out dir> [--black]      --black draws nothing but the clear (the judge must be RED on it)
//
// Exit 0 when the frame was presented and written; the judge decides green/red.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"

namespace {
// setenv on every lane (the UCRT has _putenv_s, no setenv)
void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
}  // namespace

namespace {
std::vector<uint8_t> g_memory(32u * 1024u * 1024u);
}
namespace te {
uint8_t* GuestMemory() { return g_memory.data(); }
std::size_t GuestMemorySize() { return g_memory.size(); }
const char* GuestString(uint32_t address, std::size_t max) {
    const uint8_t* p = GuestPointer(address, 1);
    if (!p) return nullptr;
    const std::size_t room = std::min<std::size_t>(max, GuestMemorySize() - address);
    return std::memchr(p, 0, room) ? reinterpret_cast<const char*>(p) : nullptr;
}
[[noreturn]] void RequestExit(int status) { std::exit(status); }
}  // namespace te

namespace {
uint32_t g_next = 0x00500000;   // a bump allocator over the guest memory (above the retail image range)
uint32_t Alloc(uint32_t size) {
    const uint32_t at = (g_next + 15) & ~15u;
    g_next = at + size;
    return at;
}
template <typename T>
T* At(uint32_t address) { return reinterpret_cast<T*>(g_memory.data() + address); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void Put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i))); }

// A game texture resource: a 32x32 PSMCT32 checker uploaded as the game's chain (CNT tag with DIRECT, GIF A+D
// TRXPOS/TRXREG/TRXDIR, GIF IMAGE data, RET)
uint32_t MakeTexture(uint32_t id) {
    const int w = 32, h = 32;
    std::vector<uint8_t> chain;
    const uint32_t quads = 1 + 3 + 1 + (w * h * 4) / 16;
    Put32(chain, (1u << 28) | quads);   // CNT, qwc
    Put32(chain, 0);
    Put32(chain, 0);                     // VIF NOP
    Put32(chain, (0x50u << 24) | quads); // VIF DIRECT quads
    // GIF tag: NLOOP 3, PACKED, NREG 1, reg A+D
    Put64(chain, 3ull | (0ull << 58) | (1ull << 60));
    Put64(chain, 0xEull);
    Put64(chain, 0); Put64(chain, 0x51);                                   // TRXPOS: dsax 0 dsay 0
    Put64(chain, uint64_t(w) | (uint64_t(h) << 32)); Put64(chain, 0x52);   // TRXREG
    Put64(chain, 0); Put64(chain, 0x53);                                   // TRXDIR host->local
    // GIF tag: IMAGE, NLOOP = pixel quadwords, EOP
    Put64(chain, uint64_t((w * h * 4) / 16) | (1ull << 15) | (2ull << 58));
    Put64(chain, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool a = ((x / 8) + (y / 8)) & 1;
            chain.push_back(a ? 255 : 40);   // R
            chain.push_back(a ? 200 : 80);   // G
            chain.push_back(a ? 0 : 255);    // B
            chain.push_back(0x80);           // A (PS2 opaque)
        }
    Put32(chain, (6u << 28) | 0);   // RET
    Put32(chain, 0); Put32(chain, 0); Put32(chain, 0);
    const uint32_t image = Alloc(uint32_t(chain.size()));
    std::memcpy(At<uint8_t>(image), chain.data(), chain.size());

    const uint32_t texture = Alloc(sizeof(te_gfx_texture));
    te_gfx_texture* t = At<te_gfx_texture>(texture);
    *t = te_gfx_texture{};
    t->id = id;
    t->size = uint32_t(sizeof(te_gfx_texture_header) + 0x20 + chain.size());
    t->texture.widthPower = 5;
    t->texture.heightPower = 5;
    t->texture.levels = 1;
    t->texture.pixelFormat = 0;    // PSMCT32
    t->texture.uploadFormat = 0;
    t->texture.hasAlpha = 1;
    t->texture.function = 0;       // MODULATE
    t->texture.levelOffsets[0] = 0;
    t->texture.levelWidths[0] = 1; // 64 pixels
    t->texture.paletteOffset = 0;
    t->texture.transfer = -1;
    const ps2e_s32 handle = ps2e_gfx2_texture_loaded(texture, texture + 12, image, ps2e_s32(chain.size()));
    t->texture.hostHandle = uint32_t(handle);
    std::printf("texture %#x -> handle %d\n", texture, handle);
    return texture;
}

uint32_t MakeShader(uint64_t settings, uint32_t texture) {
    const uint32_t shader = Alloc(sizeof(te_gfx_shader));
    te_gfx_shader* s = At<te_gfx_shader>(shader);
    *s = te_gfx_shader{};
    s->magic = TE_GFX_SHADER_MAGIC;
    s->type = texture ? 0x00 : 0x0E;
    s->settings = settings;
    s->texture = texture;
    for (uint8_t& c : s->colour) c = 0x80;
    return shader;
}

uint32_t MakeMaterial(uint32_t kind, uint32_t bucket, uint32_t shader) {
    const uint32_t material = Alloc(sizeof(te_gfx_material));
    te_gfx_material* m = At<te_gfx_material>(material);
    *m = te_gfx_material{};
    m->magic = TE_GFX_MATERIAL_MAGIC;
    m->kind = kind;
    m->bucket = bucket;
    m->shaderCount = 1;
    m->shaders[0] = shader;
    return material;
}

// The flat settings (graphics_resources.cpp FlatSettings): blends, mix, depth ALWAYS, Gouraud, no depth writes
uint64_t FlatSettings() { return (1ull << 0) | (0ull << 1) | (1ull << 21) | (1ull << 26) | (1ull << 59); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: te_graphics_selftest <out dir> [--black]\n");
        return 2;
    }
    const std::string out = argv[1];
    const bool black = argc > 2 && std::strcmp(argv[2], "--black") == 0;
    SetEnv("TE_GFX_HEADLESS", "1");
    SetEnv("TE_GFX_UNPACED", "1");
    SetEnv("TE_GFX_DUMP_DIR", out.c_str());
    SetEnv("TE_GFX_DUMP_AT", "2");
    SetEnv("TE_GFX_SUMMARY_EVERY", "2");

    // The game's globals the arm reads: g_RenderTarget -> a RenderTargetDescription, g_ScreenWidth/Height
    const uint32_t target = Alloc(0x70);
    {
        uint32_t* t = At<uint32_t>(target);
        int32_t* ti = reinterpret_cast<int32_t*>(t);
        ti[1] = 0; ti[2] = 0;          // offsetX/Y
        ti[3] = 512; ti[4] = 448;      // width/height
        t[5] = 0x80102030u;            // clearColor
        ti[6] = 512; ti[7] = 448;      // displayWidth/Height
    }
    *At<uint32_t>(0x00309BDC) = target;
    *At<int32_t>(0x00309AE4) = 512;
    *At<int32_t>(0x00309AE8) = 448;

    ps2e_graphics_start_renderer(448, 0);
    ps2e_graphics_finish_renderer_start();
    ps2e_graphics_set_up_render_target(target);
    const int32_t* tx = At<int32_t>(target + 0x60);
    std::printf("target corner (%d, %d)\n", tx[0], tx[1]);

    const uint32_t flat = MakeMaterial(TE_GFX_MATERIAL_FLAT, TE_GFX_BUCKET_UI, MakeShader(FlatSettings(), 0));
    const uint32_t texture = black ? 0 : MakeTexture(1);
    const uint64_t textured_settings = FlatSettings() | (1ull << 27) | (1ull << 28);
    const uint32_t textured = MakeMaterial(TE_GFX_MATERIAL_RESOURCE, TE_GFX_BUCKET_UI, MakeShader(textured_settings, texture));

    // Two frames: the judge reads the second (TE_GFX_DUMP_AT=2), after the first frame's clear settled the target
    for (int frame = 0; frame < 2; ++frame) {
        ps2e_gfx2_frame_reset(0);
        // FrameStart: the frame at the target's corner, 512x448, cleared to (16, 32, 48)
        const uint32_t fs = Alloc(32);
        int32_t* f = At<int32_t>(fs);
        f[0] = tx[0]; f[1] = tx[1]; f[2] = 512; f[3] = 448; f[4] = 512; f[5] = 448;
        uint8_t* fb = reinterpret_cast<uint8_t*>(f + 6);
        fb[0] = 16; fb[1] = 32; fb[2] = 48; fb[3] = 1; fb[4] = 1;
        ps2e_graphics_start_frame(fs);
        if (!black) {
            // A flat red sprite over the top-left quarter, a textured sprite over the bottom-right quarter, a coloured
            // strip across the middle, and three glyph quads of a "font" using the textured material
            const uint32_t place = Alloc(16), texrect = Alloc(16);
            float* p = At<float>(place); float* tr = At<float>(texrect);
            p[0] = 0.05f; p[1] = 0.05f; p[2] = 0.4f; p[3] = 0.4f;
            tr[0] = 0; tr[1] = 0; tr[2] = 1; tr[3] = 1;
            ps2e_graphics_draw_sprite(flat, ps2e_s32(0x80000080u | 0xFFu), place, texrect);   // red, alpha 0x80
            p = At<float>(Alloc(16));
            p[0] = 0.55f; p[1] = 0.55f; p[2] = 0.4f; p[3] = 0.4f;
            ps2e_graphics_draw_sprite(textured, ps2e_s32(0x80808080u), uint32_t(reinterpret_cast<uint8_t*>(p) - g_memory.data()), texrect);
            const uint32_t places = Alloc(4 * 8), colours = Alloc(16);
            float* v = At<float>(places); uint32_t* c = At<uint32_t>(colours);
            v[0] = 20; v[1] = 200; v[2] = 20; v[3] = 240; v[4] = 490; v[5] = 200; v[6] = 490; v[7] = 240;
            c[0] = 0x8000FF00u; c[1] = 0x80FF0000u; c[2] = 0x800000FFu; c[3] = 0x80FFFFFFu;
            ps2e_graphics_draw_strip(flat, 4, places, colours);
            const uint32_t quads = Alloc(sizeof(te_gfx_text_quad) * 3);
            te_gfx_text_quad* q = At<te_gfx_text_quad>(quads);
            for (int i = 0; i < 3; ++i) {
                const float gx = (2048.f - 256.f + 40.f + 40.f * i) * 16.f, gy = (2048.f - 224.f + 300.f) * 16.f;
                q[i] = te_gfx_text_quad{gx, gy, gx + 32 * 16, gy + 32 * 16, 0, 0, 32 * 16, 32 * 16, 0x80FFFFFFu};
            }
            ps2e_gfx2_text_quads(textured, quads, 3);
        }
        ps2e_graphics_finish_scene(0);
        ps2e_graphics_present(0x12345678);   // commands != 0: send
        ps2e_graphics_finish_frame();
    }
    std::printf("te_graphics_selftest: presented 2 frames into %s (%s)\n", out.c_str(), black ? "black case" : "drawn case");
    return 0;
}
