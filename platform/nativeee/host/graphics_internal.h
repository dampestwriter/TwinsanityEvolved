// platform/nativeee (host side) -- what the graphics seam's modules share: graphics.cpp owns the device, the frame's buckets,
// the materials' reading from guest memory and the GS-valued draw state; graphics_scene.cpp (models, culling, sky, shadows)
// records its draws through these. Not a public header: platform/nativeee/host only.
#pragma once
#include <cstdint>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/scene_interpolator.h"
#include "te_gfx_layout.h"

namespace te::gfx {

// The game's structs as they lie in guest memory (include/game/controllers.h RenderTargetDescription, 0x70 bytes)
struct RenderTargetDescription {
    uint32_t controller;
    int32_t offsetX, offsetY, width, height;
    uint32_t clearColor;
    int32_t displayWidth, displayHeight;
    float clipToScreen[4][4];
    int32_t x, y;
    uint32_t packet, unused6C;
};

// A material read from guest memory: the te_gfx_material, its first shader, that shader's texture and host handle, its bucket
struct MaterialView {
    const te_gfx_material* material = nullptr;
    const te_gfx_shader* shader = nullptr;
    const te_gfx_texture* texture = nullptr;
    uint32_t texture_handle = 0;
    uint32_t bucket = TE_GFX_BUCKET_UI;
};

// One recorded draw: a triangle list under a GS-valued state, drawn at Present in its bucket's order. `copy_frame`
// The frame is copied for the screen-copy texture right before this draw (TR WriteScreenCopy at a screen-copy
// material's packet) -- the draw samples what every draw before it in bucket order left in the frame.
struct Draw {
    ps2e_draw_state state;
    std::vector<ps2e_draw_vertex> verts;
    bool copy_frame = false;
    // What the draw IS, for the presenter's interpolation between two pictures (the engine's SceneInterpolator): a rigid
    // object by instance identity with its matrices as drawn, a vertex run paired by identity and part, or the newest
    // picture's draw as it is (untagged). Stamped from the current tag at OpenDraw (SetDrawTag).
    ps2r::ps2e::InterpTag tag;
    ps2r::ps2e::InterpTransform transform;
    bool has_transform = false;
};

bool EnsureDevice();
MaterialView ReadMaterial(uint32_t address);
// The material's k-th shader as the view's shader (its texture, handle; the same material and bucket); shader null when the
// material has no k-th shader. A material's later shaders are its further passes over the same geometry
MaterialView ShaderView(const MaterialView& m, uint32_t k);
// The first shader's settings as the draw state (the texture when the shader has one)
ps2e_draw_state StateOf(const MaterialView& m);
// (0x10 / 0x11) sample the frame; without it they draw as before (the first pass, untextured by the frame)
// Recording a draw. OpenDraw hands back the bucket's draw to append whole triangles to: the bucket's LAST draw when
// it has this very state and the state is unblended (one glDrawArrays rasterises its triangles in order exactly as
// separate draws of the same state would -- blended states stay one draw each, as the backend may read the destination
// before each), else a fresh draw with the state set. CloseDraw ends the appending (a draw left under one triangle is
// dropped). The reference is valid until the next OpenDraw/Record on any bucket; the vertex storage is the bucket's and is
// kept across frames (a frame's draws cost no allocation after the first frames). Record is the whole-draw form.
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame = false);
void CloseDraw(uint32_t bucket);
void Record(uint32_t bucket, Draw&& d);
// The tag the next OpenDraw stamps (a producer sets it around its draws and clears it after): a rigid object with its
// model->world and model->screen matrices (16 floats each, the title's row-vector convention), a vertex run (no matrices),
// or none. While the presenter interpolates, draws of different tags are not merged into one.
void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16 = nullptr, const float* screen16 = nullptr);
void ClearDrawTag();
// The frame's camera for the interpolator (graphics_scene.cpp: the game's current RenderView's world->camera and
// camera->screen, and its camera object); false without a view.
bool CurrentCamera(ps2r::ps2e::InterpCamera& out, uint32_t* camera_object);
// The per-frame draw ordinals the scene's identities use (graphics_scene.cpp), reset at the frame's start.
void ResetDrawOrdinals();
// The GS origin in frame pixels (2048 - screen/2) every draw subtracts, and the frame's size
float GsOriginX();
float GsOriginY();
int ScreenW();
int ScreenH();
// The game's current render target (g_RenderTarget) or null
const RenderTargetDescription* CurrentTarget();
// Options: TE_GFX_LOG
bool LogEnabled();
// Counters the scene module adds to (the summary line)
struct SceneCounters {
    uint64_t rigid_models = 0, placed_models = 0, sky_models = 0, shadow_meshes = 0, model_triangles = 0, models_decoded = 0,
             models_refused = 0, submodels_missing = 0, culls = 0;
    // the skinned characters (DrawSkin / DrawBlendSkin instances, their triangles, the submodel packets decoded / refused)
    uint64_t skins = 0, blend_skins = 0, skin_triangles = 0, skins_decoded = 0, skins_refused = 0;
    // shadow brackets composited (StartShadows..ApplyShadows with volumes), strips without a +8/-8 winding colour
    uint64_t shadow_passes = 0, shadow_odd_colours = 0;
    // game-thread time in the skin and shadow bodies (ms; the summary's attribution of their cost)
    double skin_ms = 0, shadow_ms = 0;
    // particle blocks drawn (chain members), their triangles, blocks the VU1 program body declined
    uint64_t particle_blocks = 0, particle_triangles = 0, particle_refused = 0;
    // the decals -- AgeDecal calls (the retail VU0 micro), those it culled (outside the view), the blocks DrawDecals
    // ran through the VU1 program body, the quads it kicked, the blocks the body declined
    uint64_t decals_aged = 0, decals_culled = 0, decal_blocks = 0, decal_quads = 0, decal_refused = 0;
    // the screen models (the skid marks) drawn and their triangles after the near clip
    uint64_t screen_models = 0, screen_model_triangles = 0;
    // a material's later shaders drawn as further passes over the submodel (the crystals' lit pass over the screen copy)
    uint64_t material_passes = 0;
};
SceneCounters& Scene();
// Save states: every decoded submodel's kept packet bytes (the game releases the packet's disk memory after ReadModel),
// and the decode replayed from them on restore
struct ModelSource {
    uint32_t model;
    int32_t index, count, vertices;
    std::vector<uint8_t> packet;
};
std::vector<ModelSource> ModelSources();
void ReplayModelSources(const std::vector<ModelSource>& sources);
// the skins' submodel packets the same way (the disk manager may move the game's block; the host keeps the bytes);
// the blend skins' parts are decoded from their heap packets at the first draw and forgotten on a restore
std::vector<ModelSource> SkinSources();
void ReplaySkinSources(const std::vector<ModelSource>& sources);
// One GIF packet a VU1 program body kicked (PACKED: a GIF tag then NLOOP x NREGS quadwords of RGBAQ / ST / UV / XYZ2) lowered
// to triangle-list vertices in frame pixels as the GS assembles a strip (ADC on XYZ2 withholds the kick); u/v as fractions of
// the texture's size when textured. The particles' and the decals' packets (graphics_particles.cpp, graphics_decals.cpp)
// the screen models' records (graphics_screen_models.cpp) for the save state: the live models' handles, materials and
// vertexes as one blob; restored (every model replaced; an empty blob = none)
std::vector<uint8_t> ScreenModelState();
bool RestoreScreenModelState(const uint8_t* data, size_t size);
size_t ScreenModelCount();
bool LowerPackedPacket(const std::vector<uint8_t>& packet, float offX, float offY, float tw, float th, bool textured,
                       std::vector<ps2e_draw_vertex>& out);
// the device's presentation (te_display.h): set now when the device exists, else kept for its creation; and
// whether the run is headless (the frozen judges' captures: the frame's own pixels, no window changes).
void SetPresentation(const ps2e_draw_presentation& presentation);
bool Headless();
// The presenter's cadence (te_display.h Settings::frame_rate: 0 = 30 .. 4 = 240, 5 = uncapped): the render thread's
// PresentPacer takes it at its next picture. Deterministic / synchronous runs present every picture whatever the value.
void SetFrameRate(int rate);

}  // namespace te::gfx
