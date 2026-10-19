#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_fixture.h"

#include <algorithm>
#include <stdexcept>

EffectsPixelFixture::EffectsPixelFixture(ps2r::render::Backend& backend, uint8_t factor, uint32_t height)
    : surfaces(backend), scale(ps2r::gs::ScaleSettings{factor}, &layout), cache(surfaces, memory, scale),
      refresher(surfaces, memory, cache), resolver(surfaces, memory, cache, refresher, layout),
      scanout(surfaces, cache), presenter(scanout),
      pipeline(ps2r::gs::DrawPipeline::Deps{
          &surfaces, &memory, &cache, &refresher, &resolver, &scanout, &presenter, &layout, nullptr,
          &sequence}),
      resources(surfaces), renderer(surfaces, resources), scene(surfaces, resources, renderer, pipeline),
      effects(scene, resources, pipeline) {
    frame.height = height;
    pipeline.OnSetWindow(512, int(height), 512, int(height));
    pipeline.OnSetFrontBuffer(frame.color_buffer, 512, 0);
}
EffectsPixelFixture::~EffectsPixelFixture() {
    effects.ResetAfterQuiescence();
    scene.CollectAfterQuiescence();
}
void EffectsPixelFixture::Initial(const ps2r::gs::DrawIr& full_rect,
                                  const std::vector<uint8_t>& rgba, float depth) {
    // Establish valid ordinary scene color/depth write history through the
    // current NEW pipeline, then provide the SAME original pixel input to both
    // fixtures. No effect or memory-alias state is pre-seeded for either path.
    auto initial = full_rect;
    initial.raster.frame.fbp = frame.color_buffer;
    initial.raster.frame.fbmsk = 0;
    initial.raster.zbuf.zbp = frame.depth_buffer;
    initial.raster.zbuf.zmsk = false;
    initial.tex_enabled = false;
    initial.src_frame = frame.source_frame;
    if (!pipeline.DrawGeneric(initial).drew) throw std::runtime_error("pixel fixture initial draw");
    auto* target = cache.Find(frame.color_buffer);
    if (!target) throw std::runtime_error("pixel fixture scene missing");
    const auto& identity = target->identity;
    const ps2r::render::Rect rectangle{0, 0, identity.PhysicalW(), identity.PhysicalH()};
    if (rgba.size() != size_t(rectangle.w) * rectangle.h * 4)
        throw std::runtime_error("pixel fixture input dimensions");
    // Clear binds the color target's framebuffer before its depth attachment.
    // A missing color target makes the backend decline the clear entirely.
    surfaces.Clear(target->views.color, target->views.depth, 0, depth, ps2r::render::kClearDepth);
    std::vector<float> actual_depth(size_t(rectangle.w) * rectangle.h);
    surfaces.Readback(target->views.depth, rectangle, actual_depth.data(), size_t(rectangle.w) * sizeof(float));
    if (!std::all_of(actual_depth.begin(), actual_depth.end(), [depth](float actual) { return actual == depth; }))
        throw std::runtime_error("pixel fixture depth clear did not author the input");
    surfaces.Upload(target->views.color, rectangle, rgba.data(), size_t(rectangle.w) * 4);
    // The fixture's direct input upload authors every byte; the next blend must
    // read that new input, not the previous case's destination-color copy.
    // The authored attachment is the main color route. Mark its full extent
    // dirty so the first partial draw refreshes every initialized copy pixel.
    target->coherence.cd_copy = {false, 0, 0, identity.width, identity.height, 0};
}
std::vector<uint8_t> EffectsPixelFixture::Pixels() {
    auto* target = cache.Find(frame.color_buffer);
    if (!target) throw std::runtime_error("pixel fixture output missing");
    const auto& identity = target->identity;
    const ps2r::render::Rect rectangle{0, 0, identity.PhysicalW(), identity.PhysicalH()};
    std::vector<uint8_t> result(size_t(rectangle.w) * rectangle.h * 4);
    surfaces.Readback(target->views.color, rectangle, result.data(), size_t(rectangle.w) * 4);
    return result;
}
