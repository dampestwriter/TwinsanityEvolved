// test_graphics_screen_models.cpp -- graphics.h's screen models (the skid marks) over the host bodies without a
// module: the builder (material, colours, vertexes) hands back a handle above the 32 MB, a model is drawn through the view's
// matrices as a near-clipped triangle strip into its material's bucket (counted when the host has a GL device), the models
// round-trip through the graphics save-state section, and deleting forgets them.
#include <cstdio>
#include <cstring>

#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"
#include "te_test.h"

namespace {
constexpr uint32_t kMaterial = 0x200000, kShader = 0x200100, kToScreen = 0x200200, kToClip = 0x200240, kClip = 0x200280,
                   kPlace = 0x200300;

void WriteMatrix(uint32_t at, const float m[16]) { std::memcpy(te::GuestPointer(at, 64), m, 64); }
}  // namespace

int main() {
    using namespace te_test;
    setenv("TE_GFX_HEADLESS", "1", 1);
    Reset(DefaultSettings());

    // A skid material: one shader, blended additive, the particles' bucket (what MakeSkidMaterial makes on the wasm side)
    te_gfx_material material{};
    material.magic = TE_GFX_MATERIAL_MAGIC;
    material.kind = TE_GFX_MATERIAL_SKID;
    material.bucket = 22;
    material.shaderCount = 1;
    material.shaders[0] = kShader;
    te_gfx_shader shader{};
    shader.magic = TE_GFX_SHADER_MAGIC;
    shader.type = 0x12;
    shader.settings = 1ull | (uint64_t(TE_GFX_PRESET_ADD) << 1);   // blends, the additive preset
    shader.colour[0] = shader.colour[1] = shader.colour[2] = 0x80;
    shader.colour[3] = 0x80;
    WriteValue(kMaterial, material);
    WriteValue(kShader, shader);

    // --- the builder: a quad strip of four coloured vertexes
    ps2e_graphics_begin_screen_model();
    ps2e_graphics_set_screen_model_material(kMaterial);
    const float places[4][4] = {{-1.f, 0.f, 5.f, 1.f}, {1.f, 0.f, 5.f, 1.f}, {-1.f, 0.f, 7.f, 1.f}, {1.f, 0.f, 7.f, 1.f}};
    const int32_t colours[4] = {int32_t(0x80FF0000), int32_t(0x8000FF00), int32_t(0x800000FF), int32_t(0x80FFFFFF)};
    for (int k = 0; k < 4; ++k) {
        ps2e_graphics_set_screen_model_colour(colours[k]);
        std::memcpy(te::GuestPointer(kPlace, 16), places[k], 16);
        ps2e_graphics_add_screen_model_vertex(kPlace);
    }
    const uint32_t model = ps2e_graphics_end_screen_model();
    CHECK(model >= 0xF0000000u);   // never a guest address
    CHECK_EQ(te::gfx::ScreenModelCount(), 1u);
    const uint32_t other = (ps2e_graphics_begin_screen_model(), ps2e_graphics_end_screen_model());
    CHECK(other != model && other != 0);
    CHECK_EQ(te::gfx::ScreenModelCount(), 2u);

    // --- the save state carries them
    const std::vector<uint8_t> state = te::gfx::ScreenModelState();
    CHECK(state.size() > 8);
    ps2e_graphics_delete_screen_model(other);
    CHECK_EQ(te::gfx::ScreenModelCount(), 1u);
    CHECK(te::gfx::RestoreScreenModelState(state.data(), state.size()));
    CHECK_EQ(te::gfx::ScreenModelCount(), 2u);
    CHECK(!te::gfx::RestoreScreenModelState(state.data(), state.size() - 3));   // a truncated blob is refused
    CHECK(te::gfx::RestoreScreenModelState(state.data(), state.size()));

    // --- drawn through a screen matrix (GS 12.4 with the 2048 origin, x16) and a clip matrix (w = z), near-clipped
    const float toScreen[16] = {16.f, 0.f, 0.f, 0.f, 0.f, 16.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 2048.f * 16.f, 2048.f * 16.f, 0.f, 0.f};
    const float toClip[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    const float clip[4] = {0.2f, 0.2f, 0.f, 0.f};
    WriteMatrix(kToScreen, toScreen);
    WriteMatrix(kToClip, toClip);
    std::memcpy(te::GuestPointer(kClip, 16), clip, 16);
    const uint64_t before = te::gfx::Scene().screen_model_triangles;
    ps2e_graphics_draw_screen_model(model, kToScreen, kToClip, kClip);
    if (te::gfx::EnsureDevice()) {
        CHECK_EQ(te::gfx::Scene().screen_models, 1u);
        CHECK_EQ(te::gfx::Scene().screen_model_triangles - before, 2u);   // the quad, wholly in front of the camera
        // a strip crossing the camera: the triangle behind is clipped away, the one straddling becomes a fan
        ps2e_graphics_begin_screen_model();
        ps2e_graphics_set_screen_model_material(kMaterial);
        ps2e_graphics_set_screen_model_colour(int32_t(0x80FFFFFF));
        const float crossing[3][4] = {{0.f, 0.f, -2.f, 1.f}, {1.f, 0.f, 2.f, 1.f}, {-1.f, 0.f, 2.f, 1.f}};
        for (int k = 0; k < 3; ++k) { std::memcpy(te::GuestPointer(kPlace, 16), crossing[k], 16); ps2e_graphics_add_screen_model_vertex(kPlace); }
        const uint32_t straddle = ps2e_graphics_end_screen_model();
        const uint64_t mid = te::gfx::Scene().screen_model_triangles;
        ps2e_graphics_draw_screen_model(straddle, kToScreen, kToClip, kClip);
        CHECK_EQ(te::gfx::Scene().screen_model_triangles - mid, 2u);   // one vertex behind: a quad remains
        ps2e_graphics_delete_screen_model(straddle);
    } else {
        std::fprintf(stderr, "  (no GL device: the draw's triangle count is not checked)\n");
    }
    // a missing model or matrix draws nothing and does not crash
    ps2e_graphics_draw_screen_model(0x12345678u, kToScreen, kToClip, kClip);
    ps2e_graphics_draw_screen_model(model, 0x7FFFFFF0u, kToClip, kClip);

    ps2e_graphics_delete_screen_model(model);
    ps2e_graphics_delete_screen_model(other);
    CHECK_EQ(te::gfx::ScreenModelCount(), 0u);
    return Finish("test_graphics_screen_models");
}
