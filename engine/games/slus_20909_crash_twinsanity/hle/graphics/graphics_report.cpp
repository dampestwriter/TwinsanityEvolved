#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_report.h"

namespace title {

std::string GraphicsReport::Row(const GraphicsService::VariantCounters& variant) {
    return "calls=" + std::to_string(variant.calls) +
           ",replaced=" + std::to_string(variant.replaced) +
           ",model_calls=" + std::to_string(variant.model_calls) +
           ",interior=" + std::to_string(variant.interior_fallback);
}

void GraphicsReport::Strips(Meta& meta, const GraphicsService::Counters& counts) {
    static constexpr const char* kVariantKeys[] = {
        "graphics_strip_plain", "graphics_strip_tinted",
        "graphics_strip_placed", "graphics_strip_placed_tinted"};
    static_assert(sizeof(kVariantKeys) / sizeof(kVariantKeys[0]) ==
                  std::tuple_size<decltype(counts.variants)>::value);
    for (size_t i = 0; i < counts.variants.size(); ++i)
        meta.Set(kVariantKeys[i], Row(counts.variants[i]));
}

void GraphicsReport::Sprites(Meta& meta, const SpriteService::Counters& counts) {
    meta.Set("graphics_sprites", "replaced=" + std::to_string(counts.replaced) +
        ",model_calls=" + std::to_string(counts.model_calls) +
        ",failed=" + std::to_string(counts.failed) +
        ",empty=" + std::to_string(counts.empty) +
        ",refused=" + std::to_string(counts.refused));
}

void GraphicsReport::Text(Meta& meta, const TextService::Counters& counts) {
    meta.Set("graphics_text", "replaced=" + std::to_string(counts.replaced) +
        ",model_calls=" + std::to_string(counts.model_calls) +
        ",failed=" + std::to_string(counts.failed) +
        ",refused=" + std::to_string(counts.refused));
}

std::string GraphicsReport::Row(const SkyPass::Counts& count) {
    return "replaced=" + std::to_string(count.replaced) +
           ",model_calls=" + std::to_string(count.model_calls) +
           ",interior_calls=" + std::to_string(count.interior_calls);
}

void GraphicsReport::Sky(Meta& meta, const SkyPass::Counters& counts) {
    meta.Set("graphics_sky_start", Row(counts.start));
    meta.Set("graphics_sky_model", Row(counts.model));
    meta.Set("graphics_sky_finish", Row(counts.finish));
    // The load-boundary conversion census: a selected pass needs every sky
    // model (SkyModelRead 0x1bcb58) and texture (SkyTextureRead 0x1bc920) of
    // the frame converted at load; a declined load is the `missing_resource`
    // refusal (detail 4 ModelMissing / 6 Material) the pass raises later.
    meta.Set("graphics_sky_assets", "models_loaded=" + std::to_string(counts.models_loaded) +
        ",textures_loaded=" + std::to_string(counts.textures_loaded) +
        ",load_declines=" + std::to_string(counts.load_declines) +
        ",declines_memory=" + std::to_string(counts.declines_memory) +
        ",declines_asset=" + std::to_string(counts.declines_asset) +
        ",declines_limit=" + std::to_string(counts.declines_limit) +
        ",declines_converted=" + std::to_string(counts.declines_converted) +
        ",textures_converted=" + std::to_string(counts.textures_converted) +
        ",refused=" + std::to_string(counts.refused) +
        ",last_refusal=" + std::to_string(uint32_t(counts.last_refusal)) +
        ",faults=" + std::to_string(counts.faults) +
        ",programs_uploaded=" + std::to_string(counts.programs_uploaded));
}

std::string GraphicsReport::Row(const FrameService::Count& count) {
    return "calls=" + std::to_string(count.calls) +
           ",replaced=" + std::to_string(count.replaced) +
           ",model_calls=" + std::to_string(count.model_calls);
}

void GraphicsReport::Frame(Meta& meta, const FrameService::Counters& counts) {
    meta.Set("graphics_frame_setup", Row(counts.setup));
    meta.Set("graphics_frame_send", Row(counts.send));
    meta.Set("graphics_frame_interrupt", Row(counts.interrupt));
    meta.Set("graphics_frame_clear", Row(counts.clear));
    meta.Set("graphics_frame_layout", Row(counts.layout));
}

std::string GraphicsReport::Row(const char* name, const MeshPass::Count& count) {
    const std::string prefix = std::string(",") + name + "_";
    return prefix + "calls=" + std::to_string(count.calls) + prefix + "replaced=" + std::to_string(count.replaced) +
           prefix + "model_calls=" + std::to_string(count.model_calls) + prefix + "interior=" + std::to_string(count.interior);
}

void GraphicsReport::Meshes(Meta& meta, const MeshPass::Counters& counts, bool selected, uint64_t draws) {
    meta.Set("graphics_meshes", "selected=" + std::to_string(selected) +
        Row("rigid", counts.rigid) + Row("placed", counts.placed) + Row("flush", counts.flush) + Row("direct", counts.direct) +
        ",blocks=" + std::to_string(counts.blocks) + ",instances=" + std::to_string(counts.instances) +
        ",draws=" + std::to_string(draws) + ",faults=" + std::to_string(counts.faults));
}

}  // namespace title
