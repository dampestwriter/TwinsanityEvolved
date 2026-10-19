#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_fixture.h"

#include <stdexcept>

ShadowFixture::ShadowFixture(ps2r::render::Backend& backend, uint32_t width, uint32_t height, uint8_t scale)
    : resources(backend) {
    for (size_t i = 0; i < images.size(); ++i) {
        ps2r::render::SurfaceDesc d{width * scale, height * scale, ps2r::render::SurfaceFormat::Rgba8, true, scale};
        if (i == 1) { d.format = ps2r::render::SurfaceFormat::Depth32f; d.renderable = false; }
        title::Graphics::Handle handle;
        if (resources.Register(registry, d, &handle) != title::GraphicsResources::Result::Ok ||
            registry.RetainResource(handle, &scene.images[i]) != title::Graphics::Result::Ok ||
            resources.Resolve(scene.images[i], &images[i]) != title::GraphicsResources::Result::Ok)
            throw std::runtime_error("shadow fixture scene allocation");
        registry.ReleaseResource(handle);
    }
    scene.publish = [this](const ps2r::gs::DrawIr&, const ps2r::render::DrawResult& result) {
        if (!result.drew) return false;
        ++publications;
        return publication_ok;
    };
}
ps2r::gs::DrawIr ShadowFixture::Volume(uint32_t width, uint32_t height, uint8_t colour, float depth) {
    ps2r::gs::DrawIr draw;
    ps2r::gs::VertexRec a{}, b{}, c{};
    a.x = a.y = b.y = c.x = 2;
    b.x = float(width - 2);
    c.y = float(height - 2);
    for (auto* v : {&a, &b, &c}) {
        v->r = v->g = v->b = colour;
        v->a = 128;
        v->z = depth;
        v->q = 1;
        v->draw_kick = 1;
    }
    draw.verts = {a, b, c};
    draw.vertex_count = 3;
    return draw;
}
