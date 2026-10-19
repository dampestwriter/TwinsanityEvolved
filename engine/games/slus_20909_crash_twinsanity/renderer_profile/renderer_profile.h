// games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h --
// RendererProfile: the title-bound data the GS HLE layers consult -- the VRAM
// layout (which buffer is the scene accumulator, the display, the FMV stage, the
// fog lanes; the native scanout tuple; the reader band pages; the CLUT-heal
// exemption window), the draw-class stamps and raw-shape rules, the Accept shape
// of every class renderer, and the stencil-composite sequence constants. It also
// owns the class renderers it registers (RegisterClassRenderers).
#pragma once

#include <cstdio>
#include <memory>

#include "engine/gs/hle/classrdr/class_classifier.h"
#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_shape_spec.h"
#include "engine/gs/hle/classrdr/fmv_blit_renderer.h"
#include "engine/gs/hle/classrdr/particles_renderer.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/classrdr/skinned_renderer.h"
#include "engine/gs/hle/classrdr/sprite_hud_renderer.h"
#include "engine/gs/hle/classrdr/stencil_composite_renderer.h"
#include "engine/gs/hle/classrdr/world_tristrip_renderer.h"
#include "engine/gs/hle/targets/vram_layout.h"
#include "engine/render/backend.h"

namespace ps2r {
namespace title {

class RendererProfile {
public:
    struct Shapes {
        gs::DrawShapeSpec world_strip;        // the DATE volume strips (tristrip)
        gs::DrawShapeSpec composite_fan;      // the same family as a triangle fan
        gs::DrawShapeSpec skinned;            // skinned mesh (also world family 2 and the raw skin)
        gs::DrawShapeSpec sprite_common;      // HUD sprites: shared pins
        gs::DrawShapeSpec sprite_textured;    // + the atlas read
        gs::DrawShapeSpec sprite_untextured;  // + flat
        gs::DrawShapeSpec particles;
        gs::DrawShapeSpec fmv_blit;
        gs::DrawShapeSpec composite_clear;    // the accumulator CLEAR (sequence pass A)
        gs::DrawShapeSpec composite_invert;   // the byte-NOT self-read (sequence pass D)
    };

    RendererProfile();
    const gs::VramLayout& Vram() const { return vram_; }
    const gs::ClassClassifier::Rules& ClassRules() const { return rules_; }
    const Shapes& Shape() const { return shapes_; }
    const gs::SequenceRecipe::Constants& Sequence() const { return sequence_; }

    // Register the title's class renderers into `reg` (the slots the peak
    // configuration fills: world-tristrip, skinned, particles, sprites, raw-blit,
    // raw-other = the raw skin, and the stencil composite over `sequence` -- the
    // FSM the pipeline shares -- when one is given). Returns the number of slots
    // filled. The renderers are owned here; a repeated call replaces them without
    // touching any backend (the previous set holds no backend resource the device
    // does not release itself at Shutdown -- call ReleaseBackendResources first
    // when the old backend is still alive and its surfaces must go now).
    int RegisterClassRenderers(gs::ClassRegistry& reg, gs::SequenceRecipe* sequence = nullptr);
    // Free every backend resource the renderers hold (the S9 stencil attachment)
    // through `be` -- the backend they were drawn against, still alive. The
    // renderers stay registered and usable; the next draw re-creates what it needs.
    void ReleaseBackendResources(render::Backend& be);
    void EmitRendererCounters(std::FILE* f) const;

private:
    void FillRules();
    void FillShapes();
    void FillSequence();

    gs::VramLayout vram_;
    gs::ClassClassifier::Rules rules_;
    Shapes shapes_;
    gs::SequenceRecipe::Constants sequence_;
    gs::SkinnedRenderer::Stats skin_stats_;
    std::unique_ptr<gs::SkinnedRenderer> skinned_;
    std::unique_ptr<gs::SkinnedRenderer> raw_skin_;
    std::unique_ptr<gs::WorldTristripRenderer> world_;
    std::unique_ptr<gs::SpriteHudRenderer> sprites_;
    std::unique_ptr<gs::ParticlesRenderer> particles_;
    std::unique_ptr<gs::FmvBlitRenderer> fmv_;
    std::unique_ptr<gs::StencilCompositeRenderer> stencil_;
};

}  // namespace title
}  // namespace ps2r
