#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_report.h"

#include <cstdio>

namespace title {
std::string EffectsReport::Row(const ShadowPass::Count& count) {
    return "calls=" + std::to_string(count.calls) +
        ",replaced=" + std::to_string(count.replaced) +
        ",model_calls=" + std::to_string(count.model_calls) +
        ",interior=" + std::to_string(count.interior);
}
void EffectsReport::Shadow(Meta& meta, bool selected, const ShadowLibrary::Counters& counts) {
    meta.Set("graphics_shadow_selection", selected ? "owned" : "model");
    meta.Set("graphics_shadow_begin", Row(counts.passes.begin));
    meta.Set("graphics_shadow_mesh", Row(counts.passes.mesh));
    meta.Set("graphics_shadow_end", Row(counts.passes.end));
    const auto& refusal = counts.passes.last_mesh_refusal;
    char site[200];
    if (refusal.detail.check == ShadowSource::MeshCheck::None) std::snprintf(site, sizeof(site), "none");
    else std::snprintf(site, sizeof(site),
                       "check:%u/result:%u/object:0x%x/geometry:0x%x/model_id:0x%x/asset_id:0x%x/screen:0x%x/camera:0x%x",
                       unsigned(refusal.detail.check), unsigned(refusal.result), refusal.detail.object,
                       refusal.detail.geometry, refusal.detail.model_id, refusal.detail.asset_id,
                       refusal.detail.screen, refusal.detail.camera);
    meta.Set("graphics_shadow", "instances=" + std::to_string(counts.passes.instances) +
        ",blocks=" + std::to_string(counts.passes.blocks) +
        ",groups=" + std::to_string(counts.passes.groups) +
        ",completed=" + std::to_string(counts.passes.completed) +
        ",empty=" + std::to_string(counts.passes.empty) +
        ",faults=" + std::to_string(counts.passes.faults) +
        ",refused=" + std::to_string(counts.passes.refused) +
        ",limit_refusals=" + std::to_string(counts.passes.limit_refusals) +
        ",refused_meshes=" + std::to_string(counts.passes.refused_meshes) +
        ",last_mesh_refusal=" + site +
        ",restores=" + std::to_string(counts.restores));
    // The empty brackets, by class and by frame (the s4 report).
    std::string frames;
    for (const auto& range : counts.passes.empty_frames) {
        if (!frames.empty()) frames += '/';
        frames += std::to_string(range.first);
        if (range.last != range.first) frames += '-' + std::to_string(range.last);
    }
    if (counts.passes.empty_frames_unlisted) frames += "/+" + std::to_string(counts.passes.empty_frames_unlisted);
    meta.Set("graphics_shadow_empty", "frames=" + std::to_string(counts.passes.frames_closed) +
        ",one_bracket=" + std::to_string(counts.passes.frames_one_bracket) +
        ",many_brackets=" + std::to_string(counts.passes.frames_many_brackets) +
        ",no_mesh=" + std::to_string(counts.passes.empty_no_mesh) +
        ",redirected=" + std::to_string(counts.passes.empty_redirected) +
        ",empty_frames=" + std::to_string(counts.passes.empty_frames_total) +
        ",last_published_frame=" + std::to_string(counts.passes.last_published_frame) +
        ",ranges=" + (frames.empty() ? std::string("none") : frames));
    meta.Set("graphics_shadow_assets", "reads=" + std::to_string(counts.loads.reads) +
        ",converted=" + std::to_string(counts.loads.converted) +
        ",unselected=" + std::to_string(counts.loads.unselected) +
        ",destroys=" + std::to_string(counts.loads.destroys) +
        ",failed=" + std::to_string(counts.loads.failed) +
        ",refused=" + std::to_string(counts.loads.refused) +
        ",interior=" + std::to_string(counts.loads.interior) +
        ",loaded=" + std::to_string(counts.assets.loaded) +
        ",released=" + std::to_string(counts.assets.released) +
        ",rejected=" + std::to_string(counts.assets.rejected));
}
void EffectsReport::Distortion(Meta& meta, bool selected, const DistortionLibrary::Counters& counts) {
    meta.Set("graphics_distortion_selection", selected ? "owned" : "model");
    meta.Set("graphics_distortion_draw", "calls=" + std::to_string(counts.pass.calls) +
        ",replaced=" + std::to_string(counts.pass.replaced) +
        ",model_calls=" + std::to_string(counts.pass.model_calls) +
        ",interior=" + std::to_string(counts.pass.interior));
    meta.Set("graphics_distortion", "batches=" + std::to_string(counts.pass.batches) +
        ",blocks=" + std::to_string(counts.pass.blocks) +
        ",draws=" + std::to_string(counts.pass.draws) +
        ",forgets=" + std::to_string(counts.pass.forgets) +
        ",completed=" + std::to_string(counts.pass.completed) +
        ",faults=" + std::to_string(counts.pass.faults) +
        ",refused=" + std::to_string(counts.pass.refused) +
        ",restores=" + std::to_string(counts.restores));
    meta.Set("graphics_distortion_blocks", "initializes=" + std::to_string(counts.blocks.initializes) +
        ",resizes=" + std::to_string(counts.blocks.resizes) +
        ",event_turns=" + std::to_string(counts.blocks.event_turns) +
        ",captured=" + std::to_string(counts.blocks.captured_blocks) +
        ",failures=" + std::to_string(counts.blocks.failures));
    meta.Set("graphics_distortion_settings", "calls=" + std::to_string(counts.settings.calls) +
        ",captured=" + std::to_string(counts.settings.captured) +
        ",rejected=" + std::to_string(counts.settings.rejected) +
        ",interior=" + std::to_string(counts.settings.interior));
}
void EffectsReport::Screen(Meta& meta, bool selected, const ScreenPass::Counters& counts,
                           const EffectsScene::Counters& scene, const ScreenRenderer::Counters& renderer,
                           const WaveRenderer::Counters& wave) {
    meta.Set("graphics_screen_selection", selected ? "owned" : "model");
    meta.Set("graphics_screen_effects", "calls=" + std::to_string(counts.calls) +
        ",empty=" + std::to_string(counts.empty) +
        ",effects=" + std::to_string(counts.effects) +
        ",blur=" + std::to_string(counts.blur) +
        ",filter=" + std::to_string(counts.filter) +
        ",wave=" + std::to_string(counts.wave) +
        ",model_calls=" + std::to_string(counts.model_calls) +
        ",replaced=" + std::to_string(counts.replaced) +
        ",filter_off=" + std::to_string(counts.filter_off) +
        ",refused=" + std::to_string(counts.refused) +
        ",last_refusal=" + ScreenPass::Name(counts.last_refusal) +
        ",faults=" + std::to_string(counts.faults) +
        ",last_palette=" + std::to_string(counts.last_palette) +
        ",last_blends=" + std::to_string(counts.last_blends ? 1 : 0) +
        ",last_height=" + std::to_string(counts.last_height) +
        ",interior=" + std::to_string(counts.interior) +
        ",unreadable=" + std::to_string(counts.unreadable));
    // The owned deliveries (the scene's screen passes) and their draws.
    meta.Set("graphics_screen_scene", "passes=" + std::to_string(scene.screen_passes) +
        ",filter_draws=" + std::to_string(renderer.filter_draws) +
        ",blur_passes=" + std::to_string(renderer.blur_passes) +
        ",palette_uploads=" + std::to_string(renderer.palette_uploads) +
        ",wave_passes=" + std::to_string(wave.passes) +
        ",allocations=" + std::to_string(renderer.allocations) +
        ",failures=" + std::to_string(renderer.failures));
}
}  // namespace title
