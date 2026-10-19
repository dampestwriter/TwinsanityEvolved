#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_renderer.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "engine/gs/hle/texsrc/texture_source.h"
#include "tests/lib/fake_backend.h"

namespace {
using Graphics = title::Graphics;
using Resources = title::GraphicsResources;
using Targets = title::GraphicsTargets;
using Renderer = title::GraphicsRenderer;
using Name = Targets::Name;
using ps2r::gs::DrawIr;
using ps2r::render::SurfaceFormat;
using ps2r::render::SurfaceId;
int checks = 0, failures = 0;
void Check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

struct Backend : ps2r::test::FakeBackend {
    struct Delivery {
        DrawIr draw;
        ps2r::gs::TextureSource texture;
        ps2r::render::DrawParams params;
    };
    std::vector<Delivery> deliveries;
    bool fail_draw = false;
    ps2r::render::DrawResult Draw(const DrawIr& draw, const ps2r::gs::TextureSource& tex,
                                 const ps2r::render::DrawParams& params) override {
        if (fail_draw) throw std::runtime_error("draw failure");
        deliveries.push_back({draw, tex, params});
        return FakeBackend::Draw(draw, tex, params);
    }
};

DrawIr Triangle(bool textured = false) {
    DrawIr draw;
    draw.prim_type = 4;
    draw.vertex_count = 3;
    draw.verts.resize(3);
    draw.verts[1].x = 1;
    draw.verts[2].y = 1;
    draw.verts[2].a = 0x4c; // OLD FBA requires bit-7 OR, not max(a, 0x80).
    draw.tex_enabled = textured;
    draw.use_fst = draw.tex.fst = true;
    draw.alpha_blend = true;
    draw.raster.alpha = {0, 1, 2, 2, 0x78}; // OLD exact 0122 fixed-alpha route.
    draw.raster.fba = true;
    draw.raster.test = {true, 5, 0x80, 3, true, false, true, 2};
    draw.raster.zbuf.zmsk = true;
    draw.raster.frame.fbmsk = 0x00ffffff;
    draw.raster.colclamp = false;
    draw.raster.pabe = true;
    draw.raster.dthe = 1;
    draw.raster.dimx = 0x0123456789abcdef;
    draw.raster.fogcol_r = 0x78;
    draw.raster.fogcol_g = 0x80;
    draw.raster.fogcol_b = 0xff;
    draw.raster.scissor = {0, 0, 31, 15};
    draw.fog = true;
    draw.scanmsk = 2;
    // Source audit metadata is deliberately unrelated to resource identities.
    draw.raster.frame.fbp = 0x70000;
    draw.raster.zbuf.zbp = 0x150000;
    draw.tex.tbp = 0x300000;
    draw.tex.cbp = 0x390000;
    for (auto& address : draw.tex.mip_tbp) address = 0x110000;
    return draw;
}

struct Fixture {
    Backend backend;
    Resources resources{backend};
    Graphics queue;
    Targets targets{queue};
    Renderer renderer{backend, resources};
    Graphics::Handle color, depth, copy, sky;
    Fixture() {
        CHECK(resources.Register(queue, {64,32,SurfaceFormat::Rgba8,true,2}, &color) == Resources::Result::Ok);
        CHECK(resources.Register(queue, {64,32,SurfaceFormat::Depth32f,false,2}, &depth) == Resources::Result::Ok);
        CHECK(resources.Register(queue, {64,32,SurfaceFormat::Rgba8,true,2}, &copy) == Resources::Result::Ok);
        CHECK(resources.Register(queue, {16,8,SurfaceFormat::Rgba8,true,1}, &sky) == Resources::Result::Ok);
        CHECK(targets.Bind(Name::SceneColor, color) == Targets::Result::Ok);
        CHECK(targets.Bind(Name::SceneDepth, depth) == Targets::Result::Ok);
        CHECK(targets.Bind(Name::SceneCopy, copy) == Targets::Result::Ok);
        CHECK(targets.Bind(Name::SkyColor, sky) == Targets::Result::Ok);
    }
    Graphics::Hooks Hooks() {
        Graphics::Hooks hooks;
        hooks.draw = [&](const DrawIr& draw, const std::vector<Graphics::Lease>& bindings) {
            return renderer.Draw(draw, bindings) == Renderer::Result::Ok;
        };
        hooks.upload = [&](const Graphics::Upload& upload) {
            return resources.Upload(upload) == Resources::Result::Ok;
        };
        hooks.drain = [] { return true; };
        return hooks;
    }
    std::vector<Graphics::Lease> Bindings(bool textured = false) {
        std::vector<Graphics::Lease> result;
        for (auto handle : {color, depth, copy}) {
            Graphics::Lease lease;
            CHECK(queue.RetainResource(handle, &lease) == Graphics::Result::Ok);
            result.push_back(lease);
        }
        if (textured) {
            Graphics::Lease lease;
            CHECK(queue.RetainResource(sky, &lease) == Graphics::Result::Ok);
            result.push_back(lease);
        }
        return result;
    }
};

void TestOrderedNamedTargets() {
    Fixture f;
    CHECK(f.queue.BeginFrame(7) == Graphics::Result::Ok);
    auto original = Triangle(true);
    CHECK(f.targets.Record(f.queue.End(24), original, Name::SkyColor) == Targets::Result::Ok);
    CHECK(f.queue.RecordUpload(f.queue.End(0), f.sky, {0,0,16,8}, 64,
                               std::vector<uint8_t>(16*8*4, 0x78)) == Graphics::Result::Ok);
    Graphics::Handle replacement;
    CHECK(f.resources.Register(f.queue, {64,32,SurfaceFormat::Rgba8,true,2}, &replacement) == Resources::Result::Ok);
    CHECK(f.targets.Bind(Name::SceneColor, replacement) == Targets::Result::Ok);
    CHECK(f.queue.ReleaseResource(f.color) == Graphics::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), Triangle()) == Targets::Result::Ok);
    CHECK(f.backend.creates == 0 && f.backend.draws == 0 && f.backend.uploads == 0);
    auto hooks = f.Hooks();
    CHECK(f.queue.Submit(hooks) == Graphics::Result::Ok);
    CHECK(f.backend.deliveries.size() == 2);
    const auto& first = f.backend.deliveries.at(0);
    const auto& second = f.backend.deliveries.at(1);
    CHECK(first.params.color.id != second.params.color.id); // record-time binding
    CHECK(first.params.depth.id == second.params.depth.id);
    CHECK(first.params.rt_copy.id == second.params.rt_copy.id);
    CHECK(first.texture.owner.kind == ps2r::gs::TextureSource::OwnerKind::Surface);
    CHECK(first.texture.enabled && !second.texture.enabled);
    CHECK(f.backend.At(first.texture.owner.surface).rgba == std::vector<uint8_t>(16*8*4, 0x78));
    CHECK(first.texture.transform.uv_tex_w == 16 && first.texture.transform.uv_tex_h == 8);
    CHECK(first.texture.transform.uv_pre_w == 16 && first.texture.transform.uv_pre_h == 8);
    CHECK(first.texture.transform.draw_scale == 1 && !first.texture.transform.uv_native);
    CHECK(!first.texture.sampler.mag_linear && first.texture.sampler.mmin == 0);
    CHECK(first.texture.sampler.wms == 1 && first.texture.sampler.wmt == 1);
    CHECK(first.params.render_w == 32 && first.params.render_h == 16);
    CHECK(first.params.target_w == 32 && first.params.target_h == 16 && first.params.scale == 2);
    CHECK(first.params.rt_copy_available && first.params.rt_copy.id != 0);
    CHECK(first.params.cd_copy == nullptr); // OLD stale Cd-copy/alpha prevention
    CHECK(first.params.inherited_uniforms == 0); // OLD BUG058/061: complete state
    CHECK(!first.params.do_frame_clear && !first.params.clear_color && !first.params.alt16_route);
    CHECK(!first.params.zsrc_armed && first.params.program == ps2r::render::StencilProgram::None);
    CHECK(f.backend.readbacks == 0 && f.backend.clears == 0 && f.backend.presents == 0);

    original.src_frame = 7;
    original.raster.frame.fbp = original.raster.zbuf.zbp = 0;
    original.tex.tbp = original.tex.cbp = 0;
    for (auto& address : original.tex.mip_tbp) address = 0;
    auto a = original.ToDrawRec(), b = first.draw.ToDrawRec();
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0); // all serialized material/depth/alpha state
    CHECK(first.draw.raster.dthe == original.raster.dthe);
    CHECK(first.draw.raster.dimx == original.raster.dimx);
    CHECK(first.draw.verts[2].a == 0x4c);
    CHECK(first.draw.raster.fba && first.draw.raster.test.date);
    CHECK(first.draw.raster.alpha.a == 0 && first.draw.raster.alpha.b == 1 &&
          first.draw.raster.alpha.c == 2 && first.draw.raster.alpha.d == 2);
    CHECK(first.draw.raster.test.ztst == 2 && first.draw.raster.zbuf.zmsk);
    CHECK(f.renderer.Counts().draws == 2 && f.renderer.Counts().failures == 0);
    CHECK(f.resources.CollectAfterQuiescence() == Resources::Result::Ok);
    CHECK(!f.backend.Has(first.params.color)); // old version released after the fence
    CHECK(f.backend.Has(second.params.color));
}

void TestGraphRejectionsAndRetain() {
    Fixture f;
    CHECK(f.targets.Bind(Name::Count, f.color) == Targets::Result::InvalidName);
    CHECK(f.targets.Bind(Name::SceneDepth, f.color) == Targets::Result::InvalidBinding);
    CHECK(f.targets.Bind(Name::SceneColor, f.depth) == Targets::Result::InvalidBinding);
    CHECK(f.targets.Bind(Name::SceneColor, {}) == Targets::Result::InvalidBinding);
    Graphics other;
    Graphics::Lease lease;
    CHECK(f.queue.RetainResource(f.color, &lease) == Graphics::Result::Ok);
    auto unchanged = lease;
    CHECK(other.RetainResource(f.color, &lease) == Graphics::Result::InvalidResource && lease == unchanged);
    CHECK(f.queue.RetainResource(f.color, nullptr) == Graphics::Result::InvalidResource);
    CHECK(f.queue.BeginFrame(0) == Graphics::Result::Ok);
    auto draw = Triangle();
    CHECK(f.targets.Record(f.queue.End(24), draw, Name::SkyColor) == Targets::Result::UnsupportedDraw);
    draw.raster.frame.psm = 2;
    CHECK(f.targets.Record(f.queue.End(24), draw) == Targets::Result::UnsupportedDraw);
    draw = Triangle(true);
    CHECK(f.targets.Record(f.queue.End(24), draw, Name::SceneColor) == Targets::Result::UnsupportedDraw);
    CHECK(f.targets.Record(f.queue.End(24), draw, static_cast<Name>(-1)) == Targets::Result::UnsupportedDraw);
    CHECK(f.targets.Bind(Name::SkyColor, f.color) == Targets::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), draw, Name::SkyColor) == Targets::Result::Feedback);
    CHECK(f.targets.Bind(Name::SkyColor, f.copy) == Targets::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), draw, Name::SkyColor) == Targets::Result::Feedback);
    CHECK(f.targets.Bind(Name::SkyColor, f.sky) == Targets::Result::Ok);
    CHECK(f.targets.Bind(Name::SceneCopy, f.color) == Targets::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), Triangle()) == Targets::Result::Feedback);
    CHECK(f.targets.Bind(Name::SceneCopy, f.sky) == Targets::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), Triangle()) == Targets::Result::InvalidBinding);
    CHECK(f.targets.Bind(Name::SceneCopy, f.copy) == Targets::Result::Ok);
    CHECK(f.queue.ReleaseResource(f.sky) == Graphics::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), draw, Name::SkyColor) == Targets::Result::MissingTarget);
    f.targets.Unbind(Name::SceneDepth);
    CHECK(f.targets.Record(f.queue.End(24), Triangle()) == Targets::Result::MissingTarget);
    CHECK(f.queue.PendingEntries() == 0 && f.backend.creates == 0);
    f.targets.Reset();
    CHECK(f.queue.Reset() == Graphics::Result::Ok);
    CHECK(f.queue.RetainResource(f.color, &lease) == Graphics::Result::InvalidResource && lease == unchanged);
}

void TestRendererPreflightAndFailure() {
    Fixture f;
    auto bindings = f.Bindings(true);
    auto draw = Triangle(true);
    for (size_t count : {size_t(0), size_t(3), size_t(5)}) {
        auto bad = bindings;
        bad.resize(count);
        CHECK(f.renderer.Draw(draw, bad) == Renderer::Result::InvalidBindings);
    }
    auto bad = bindings;
    bad[0].reset();
    CHECK(f.renderer.Draw(draw, bad) == Renderer::Result::InvalidBindings);
    bad = bindings; bad[2] = bad[0];
    CHECK(f.renderer.Draw(draw, bad) == Renderer::Result::InvalidBindings);
    for (size_t slot : {size_t(0), size_t(2)}) {
        bad = bindings; bad[3] = bad[slot];
        CHECK(f.renderer.Draw(draw, bad) == Renderer::Result::UnsupportedTexture);
    }
    for (int test = 0; test != 9; ++test) {
        auto invalid = draw;
        switch (test) {
        case 0: invalid.verts.clear(); break;
        case 1: invalid.vertex_count = 6; break;
        case 2: invalid.verts[0].x = std::numeric_limits<float>::quiet_NaN(); break;
        case 3: invalid.verts[0].z = std::numeric_limits<float>::infinity(); break;
        case 4: invalid.prim_type = 0; break;
        case 5: invalid.prim_type = 7; break;
        case 6: invalid.raster.frame.psm = 2; break;
        case 7: invalid.raster.zbuf.psm = 2; break;
        case 8: invalid.shuffle_type = 1; break;
        }
        CHECK(f.renderer.Draw(invalid, bindings) == Renderer::Result::InvalidDraw);
    }
    for (int test = 0; test != 6; ++test) {
        auto invalid = draw;
        switch (test) {
        case 0: invalid.use_fst = false; break;
        case 1: invalid.tex.fst = false; break;
        case 2: invalid.tex.psm = 19; break;
        case 3: invalid.tex.mmag = true; break;
        case 4: invalid.tex.mmin = 1; break;
        case 5: invalid.tex.mxl = 1; break;
        }
        CHECK(f.renderer.Draw(invalid, bindings) == Renderer::Result::UnsupportedTexture);
    }
    for (size_t slot : {size_t(0), size_t(3)}) {
        auto forged = std::make_shared<Graphics::Resource>(*bindings[slot]);
        forged->description.width = std::numeric_limits<uint32_t>::max();
        forged->description.scale = 1;
        bad = bindings; bad[slot] = forged;
        CHECK(f.renderer.Draw(draw, bad) == (slot == 0 ? Renderer::Result::InvalidBindings
                                                     : Renderer::Result::UnsupportedTexture));
    }
    CHECK(f.backend.creates == 0 && f.backend.draws == 0);
    CHECK(f.queue.BeginFrame(0) == Graphics::Result::Ok);
    CHECK(f.targets.Record(f.queue.End(24), draw, Name::SkyColor) == Targets::Result::Ok);
    f.backend.fail_draw = true;
    auto hooks = f.Hooks();
    CHECK(f.queue.Submit(hooks) == Graphics::Result::DispatchFailed);
    CHECK(f.queue.CurrentState() == Graphics::State::Faulted);
    CHECK(f.renderer.Counts().failures == 1 && f.renderer.Counts().draws == 0);
    f.backend.fail_draw = false;
    CHECK(f.queue.Recover(hooks) == Graphics::Result::Ok);
    CHECK(f.backend.deliveries.empty()); // recovery never replays a failed draw
}

void TestDecodedSkyRejections() {
    Fixture f;
    Graphics::Handle image;
    CHECK(f.resources.Register(f.queue, {32,32,SurfaceFormat::Rgba8,false,1}, &image) == Resources::Result::Ok);
    auto bindings = f.Bindings(true);
    CHECK(f.queue.RetainResource(image, &bindings[3]) == Graphics::Result::Ok);
    auto draw = Triangle(true);
    draw.use_fst = draw.tex.fst = draw.fog = false;
    draw.tex.psm = 19;
    draw.tex.tw = draw.tex.th = 5;
    draw.raster.test.ztst = 1;
    for (unsigned variant = 0; variant != 7; ++variant) {
        auto invalid = draw;
        switch (variant) {
        case 0: invalid.tex.tw = 6; break;
        case 1: invalid.tex.th = 32; break; // reject before shifting by the extent
        case 2: invalid.tex.mmin = 2; break; // requires separately owned mip levels
        case 3: invalid.tex.wms = 1; break;
        case 4: invalid.tex.mxl = 5; break;
        case 5: invalid.use_fst = true; break;
        case 6: invalid.verts[0].q = std::numeric_limits<float>::infinity(); break;
        }
        CHECK(f.renderer.Draw(invalid, bindings) == (variant == 6 ? Renderer::Result::InvalidDraw :
                                                                   Renderer::Result::UnsupportedTexture));
    }
    CHECK(f.targets.Bind(Name::SkyColor, f.color) == Targets::Result::Ok);
    CHECK(f.targets.Bind(Name::SkyDepth, f.depth) == Targets::Result::Ok);
    CHECK(f.targets.Bind(Name::SkyCopy, f.sky) == Targets::Result::Ok);
    CHECK(f.queue.BeginFrame(0) == Graphics::Result::Ok);
    CHECK(f.targets.RecordSky(f.queue.End(0), draw, image) == Targets::Result::InvalidBinding);
    CHECK(f.targets.Bind(Name::SkyCopy, f.color) == Targets::Result::Ok);
    CHECK(f.targets.RecordSky(f.queue.End(0), draw, image) == Targets::Result::Feedback);
    CHECK(f.targets.Bind(Name::SkyCopy, f.copy) == Targets::Result::Ok);
    CHECK(f.targets.RecordSky(f.queue.End(0), draw, f.sky) == Targets::Result::InvalidBinding);
    CHECK(f.targets.RecordSky(f.queue.End(0), draw, image) == Targets::Result::Ok);
    draw.raster.test.ztst = 2;
    CHECK(f.targets.RecordSky(f.queue.End(0), draw, image) == Targets::Result::UnsupportedDraw);
    CHECK(f.queue.PendingEntries() == 1 && f.backend.creates == 0 && f.backend.draws == 0);
}

void TestOwnedMeshMips() {
    Fixture f;
    Graphics::Handle image;
    CHECK(f.resources.Register(f.queue, {256,128,SurfaceFormat::Rgba8,false,1}, &image) == Resources::Result::Ok);
    auto bindings = f.Bindings(true);
    CHECK(f.queue.RetainResource(image, &bindings[3]) == Graphics::Result::Ok);
    const auto texture = bindings[3];
    auto draw = Triangle(true);
    draw.use_fst = draw.tex.fst = draw.fog = false;
    draw.tex.psm = 0; // authored RGBA, decoded once by the texture owner
    draw.tex.tw = 8; draw.tex.th = 7;
    draw.tex.mmag = true; draw.tex.mmin = 4; draw.tex.mxl = 4;
    draw.tex.k = -31; draw.tex.l = 2;
    draw.raster.frame.fbmsk = 0;
    draw.raster.zbuf.zmsk = false;
    draw.raster.alpha = {2,0,0,1,0}; // OLD portal subtractive route
    for (auto& vertex : draw.verts) { vertex.s = 0.5f; vertex.t = -0.75f; vertex.q = 1.25f; }
    CHECK(f.renderer.Draw(draw, bindings) == Renderer::Result::UnsupportedTexture);
    f.renderer.TextureLevels([&](const Graphics::Lease& lease) { CHECK(lease == texture); return uint8_t(4); });
    CHECK(f.renderer.Draw(draw, bindings) == Renderer::Result::UnsupportedTexture);
    CHECK(f.backend.creates == 0 && f.backend.draws == 0);
    f.renderer.TextureLevels([&](const Graphics::Lease& lease) { CHECK(lease == texture); return uint8_t(5); });
    CHECK(f.queue.BeginFrame(1) == Graphics::Result::Ok);
    CHECK(f.queue.RecordUpload(f.queue.End(2), image, {0,0,256,128}, 1024,
                              std::vector<uint8_t>(256*128*4, 0x43)) == Graphics::Result::Ok);
    CHECK(f.targets.RecordMesh(f.queue.End(2), draw, image) == Targets::Result::Ok);
    // Already recorded work retains the exact allocation after source reload.
    CHECK(f.queue.ReleaseResource(image) == Graphics::Result::Ok);
    auto hooks = f.Hooks();
    CHECK(f.queue.Submit(hooks) == Graphics::Result::Ok);
    CHECK(f.backend.deliveries.size() == 1);
    if (f.backend.deliveries.empty()) return;
    const auto& delivered = f.backend.deliveries[0];
    CHECK(delivered.texture.owner.kind == ps2r::gs::TextureSource::OwnerKind::Decoded);
    CHECK(delivered.texture.owner.levels == 4 && delivered.texture.transform.mip_levels == 4);
    CHECK(delivered.texture.transform.mip_k == -31 && delivered.texture.transform.mip_l == 2);
    CHECK(delivered.texture.sampler.mmin == 4 && delivered.texture.sampler.mag_linear);
    CHECK(delivered.texture.sampler.wms == 0 && delivered.texture.sampler.wmt == 0);
    CHECK(delivered.params.scale == 2 && delivered.params.render_w == 32);
    CHECK(delivered.params.cd_copy == nullptr && delivered.params.inherited_uniforms == 0);
    CHECK(delivered.draw.raster.fba && !delivered.draw.raster.zbuf.zmsk);
    CHECK(delivered.draw.raster.alpha.a == 2 && delivered.draw.raster.alpha.b == 0 &&
          delivered.draw.raster.alpha.c == 0 && delivered.draw.raster.alpha.d == 1);
    CHECK(delivered.draw.raster.test.ate && delivered.draw.raster.test.afail == 3 &&
          delivered.draw.raster.test.ztst == 2);
    CHECK(delivered.draw.tex.tbp == 0 && delivered.draw.tex.cbp == 0 &&
          delivered.draw.raster.frame.fbp == 0 && delivered.draw.raster.zbuf.zbp == 0);
    CHECK(f.backend.At(delivered.texture.owner.surface).rgba[0] == 0x43);
}

void TestSharedSceneAllocations() {
    Backend backend;
    const ps2r::render::SurfaceDesc color_desc{64,32,SurfaceFormat::Rgba8,true,2};
    const ps2r::render::SurfaceDesc depth_desc{64,32,SurfaceFormat::Depth32f,false,2};
    const auto color = backend.CreateSurface(color_desc);
    const auto depth = backend.CreateSurface(depth_desc);
    const auto copy = backend.CreateSurface(color_desc);
    const auto replacement = backend.CreateSurface(color_desc);
    auto owner = std::make_shared<int>(0); // root pins these exact allocations until below
    {
        Resources resources(backend);
        Graphics queue;
        Targets targets(queue);
        Renderer renderer(backend, resources);
        Graphics::Handle color_handle, depth_handle, copy_handle, alias_handle, replacement_handle;
        CHECK(resources.RegisterExisting(queue, color_desc, color, owner, &color_handle) == Resources::Result::Ok);
        CHECK(resources.RegisterExisting(queue, depth_desc, depth, owner, &depth_handle) == Resources::Result::Ok);
        CHECK(resources.RegisterExisting(queue, color_desc, copy, owner, &copy_handle) == Resources::Result::Ok);
        CHECK(resources.RegisterExisting(queue, color_desc, color, owner, &alias_handle) == Resources::Result::Ok);
        CHECK(resources.RegisterExisting(queue, color_desc, replacement, owner, &replacement_handle) == Resources::Result::Ok);
        CHECK(targets.Bind(Name::SceneColor, color_handle) == Targets::Result::Ok);
        CHECK(targets.Bind(Name::SceneDepth, depth_handle) == Targets::Result::Ok);
        CHECK(targets.Bind(Name::SceneCopy, alias_handle) == Targets::Result::Ok);
        CHECK(queue.BeginFrame(1) == Graphics::Result::Ok);
        // Duplicate imports of one actual surface cannot hide framebuffer feedback.
        CHECK(targets.Record(queue.End(24), Triangle()) == Targets::Result::Feedback);
        CHECK(targets.Bind(Name::SceneCopy, copy_handle) == Targets::Result::Ok);
        CHECK(targets.Record(queue.End(24), Triangle()) == Targets::Result::Ok);
        CHECK(targets.Bind(Name::SceneColor, replacement_handle) == Targets::Result::Ok);
        CHECK(queue.ReleaseResource(color_handle) == Graphics::Result::Ok);
        CHECK(queue.ReleaseResource(alias_handle) == Graphics::Result::Ok);
        CHECK(targets.Record(queue.End(24), Triangle()) == Targets::Result::Ok);
        Graphics::Hooks hooks;
        hooks.draw = [&](const DrawIr& draw, const std::vector<Graphics::Lease>& bindings) {
            return renderer.Draw(draw, bindings) == Renderer::Result::Ok;
        };
        hooks.drain = [] { return true; };
        CHECK(queue.Submit(hooks) == Graphics::Result::Ok);
        CHECK(backend.deliveries.size() == 2);
        if (backend.deliveries.size() == 2) {
            // No private clone of the retained scene or depth attachment.
            CHECK(backend.deliveries[0].params.color.id == color.id);
            CHECK(backend.deliveries[1].params.color.id == replacement.id);
            for (const auto& draw : backend.deliveries) {
                CHECK(draw.params.depth.id == depth.id && draw.params.rt_copy.id == copy.id);
                CHECK(draw.params.scale == 2 && draw.params.render_w == 32 && draw.params.render_h == 16);
                CHECK(draw.params.cd_copy == nullptr && draw.params.inherited_uniforms == 0);
            }
        }
        CHECK(backend.creates == 4 && backend.uploads == 0 && backend.readbacks == 0);
        CHECK(resources.Counts().created == 0);
        CHECK(resources.CollectAfterQuiescence() == Resources::Result::Ok);
        CHECK(resources.Counts().live_surfaces == 3 && backend.Has(color));
        targets.Reset();
        CHECK(queue.Reset() == Graphics::Result::Ok);
        CHECK(resources.CollectAfterQuiescence() == Resources::Result::Ok);
        CHECK(resources.Counts().live_surfaces == 0 && resources.Counts().destroyed == 0);
    }
    CHECK(backend.destroys == 0);
    for (auto surface : {color, depth, copy, replacement}) backend.DestroySurface(surface);
    CHECK(backend.destroys == 4 && owner.use_count() == 1);
}

DrawIr MovieStrip() {
    // A source MovieDraw strip, not a second producer implementation. The
    // complete producer's aspect/crop arithmetic is checked by MovieGraphicsOracle.
    DrawIr draw;
    draw.prim_type = 6;
    draw.ctx = 1;
    draw.tex_enabled = draw.use_fst = draw.tex.fst = true;
    draw.tex.tw = draw.tex.th = 10;
    draw.tex.tbw = 512;
    draw.tex.tcc = draw.tex.tfx = draw.tex.cld = 1;
    draw.raster.frame.fbw = 512;
    draw.raster.frame.fbmsk = 0xff000000;
    draw.raster.zbuf.zmsk = true;
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 2;
    draw.raster.scissor.x1 = draw.raster.scissor.y1 = 1024;
    draw.raster.colclamp = true;
    draw.raster.dimx = 0x6071243571603524ull;
    ps2r::gs::VertexRec a{}, b{}, c{}, d{};
    for (auto* v : {&a, &b, &c, &d}) {
        v->z = float(std::numeric_limits<uint32_t>::max());
        v->r = v->g = v->b = v->a = 0x80;
        v->q = 1;
        v->draw_kick = 1;
    }
    a.x = c.x = 32; b.x = d.x = 64;
    a.y = b.y = 80; c.y = d.y = 368; // OLD 80/288/80 border
    a.uv_u = c.uv_u = 32 * 16 + 8; b.uv_u = d.uv_u = 64 * 16 + 8;
    a.uv_v = b.uv_v = 8; c.uv_v = d.uv_v = 288 * 16 + 8;
    for (auto* v : {&a, &b, &c, &d}) { v->s = float(v->uv_u) / 16; v->t = float(v->uv_v) / 16; }
    draw.verts = {a,b,c,b,d,c};
    draw.vertex_count = 6;
    return draw;
}

void TestOwnedMoviePictures() {
    for (auto extent : {std::array<uint32_t, 2>{512,288}, {512,384}, {640,480}}) {
        Fixture f;
        Graphics::Handle image;
        CHECK(f.resources.Register(f.queue, {extent[0],extent[1],SurfaceFormat::Rgba8,false,1},
                                   &image) == Resources::Result::Ok);
        auto bindings = f.Bindings(true);
        CHECK(f.queue.RetainResource(image, &bindings[Targets::Texture]) == Graphics::Result::Ok);
        auto draw = MovieStrip();
        for (unsigned variant = 0; variant != 10; ++variant) {
            auto bad = draw;
            switch (variant) {
            case 0: bad.use_fst = false; break;
            case 1: bad.tex.fst = false; break;
            case 2: bad.tex.psm = 19; break;
            case 3: bad.tex.mmag = true; break;
            case 4: bad.tex.mmin = 1; break;
            case 5: bad.tex.mxl = 1; break;
            case 6: bad.tex.wms = 2; break;
            case 7: bad.tex.wmt = 3; break;
            case 8: bad.tex.tfx = 4; break;
            case 9: bad.tex.tcc = 2; break;
            }
            CHECK(f.renderer.Draw(bad, bindings) == Renderer::Result::UnsupportedTexture);
        }
        CHECK(f.backend.creates == 0 && f.backend.draws == 0);
        CHECK(f.queue.BeginFrame(9) == Graphics::Result::Ok);
        auto invalid = draw; invalid.ctx = 0;
        CHECK(f.targets.RecordMovie(f.queue.End(27), invalid, image) == Targets::Result::UnsupportedDraw);
        invalid = draw; invalid.tex.tw = 9;
        CHECK(f.targets.RecordMovie(f.queue.End(27), invalid, image) == Targets::Result::UnsupportedDraw);
        CHECK(f.targets.RecordMovie(f.queue.End(27), draw, f.sky) == Targets::Result::InvalidBinding);
        Graphics::Handle malformed;
        CHECK(f.resources.Register(f.queue, {513,288,SurfaceFormat::Rgba8,false,1},
                                   &malformed) == Resources::Result::Ok);
        CHECK(f.targets.RecordMovie(f.queue.End(27), draw, malformed) == Targets::Result::InvalidBinding);
        // Real decoded byte order/alpha reaches the consumer before this draw.
        std::vector<uint8_t> rgba(size_t(extent[0]) * extent[1] * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i] = uint8_t(i / 4); rgba[i + 1] = 0x7f; rgba[i + 2] = 0x40; rgba[i + 3] = 0x80;
        }
        CHECK(f.queue.RecordUpload(f.queue.End(27), image, {0,0,int32_t(extent[0]),int32_t(extent[1])},
                                   size_t(extent[0]) * 4, rgba) == Graphics::Result::Ok);
        CHECK(f.targets.RecordMovie(f.queue.End(27), draw, image) == Targets::Result::Ok);
        CHECK(f.queue.ReleaseResource(image) == Graphics::Result::Ok); // Close before consumer drain
        const auto hooks = f.Hooks();
        CHECK(f.queue.Submit(hooks) == Graphics::Result::Ok);
        CHECK(f.backend.deliveries.size() == 1);
        if (f.backend.deliveries.empty()) continue;
        const auto& delivered = f.backend.deliveries[0];
        CHECK(delivered.texture.owner.kind == ps2r::gs::TextureSource::OwnerKind::Decoded);
        CHECK(delivered.texture.transform.uv_tex_w == int32_t(extent[0]) &&
              delivered.texture.transform.uv_tex_h == int32_t(extent[1]));
        CHECK(delivered.texture.transform.uv_pre_w == int32_t(extent[0]) &&
              delivered.texture.transform.uv_pre_h == int32_t(extent[1]));
        CHECK(!delivered.texture.transform.uv_native && delivered.texture.transform.draw_scale == 1);
        CHECK(!delivered.texture.sampler.mag_linear && delivered.texture.sampler.mmin == 0);
        CHECK(delivered.texture.sampler.wms == 0 && delivered.texture.sampler.wmt == 0);
        CHECK(delivered.texture.owner.levels == 0 && delivered.texture.transform.mip_levels == 0);
        CHECK(f.backend.At(delivered.texture.owner.surface).rgba == rgba);
        CHECK(delivered.params.scale == 2 && delivered.params.cd_copy == nullptr &&
              delivered.params.inherited_uniforms == 0);
        draw.src_frame = 9;
        const auto expected = draw.ToDrawRec(), actual = delivered.draw.ToDrawRec();
        CHECK(std::memcmp(&expected, &actual, sizeof(expected)) == 0);
        CHECK(std::memcmp(draw.verts.data(), delivered.draw.verts.data(),
                          draw.verts.size() * sizeof(draw.verts[0])) == 0);
        CHECK(delivered.draw.raster.frame.fbmsk == 0xff000000 && delivered.draw.raster.zbuf.zmsk);
        CHECK(delivered.draw.raster.test.ztst == 2 && delivered.draw.verts[0].y == 80 &&
              delivered.draw.verts[2].y == 368);
        CHECK(f.backend.presents == 0 && f.backend.clears == 0 && f.backend.readbacks == 0);
    }
}
}  // namespace

int main() {
    TestOrderedNamedTargets();
    TestGraphRejectionsAndRetain();
    TestRendererPreflightAndFailure();
    TestDecodedSkyRejections();
    TestOwnedMeshMips();
    TestSharedSceneAllocations();
    TestOwnedMoviePictures();
    std::printf("GraphicsRenderer: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
